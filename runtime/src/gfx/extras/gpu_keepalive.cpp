// SPDX-License-Identifier: GPL-3.0-or-later
// GPU keep-alive (gpu_keepalive.h): a thread that keeps one small ALU-only dispatch running on a compute-only queue for a set share of
// the time while frames are presented, so the driver's load measure stays above the level where it lowers the card's power state.
#include "extras/gpu_keepalive.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "nxrt/ab_switch.h"
#include "nxrt/config.h"
#include "nxrt/frame_limiter.h"
#include "nxrt/log.h"
#include "extras/vk_make.h"

namespace nxrt::extras {
namespace {
constexpr const char* kTag = "keepalive";

const uint32_t kShader[] = {
#include "extras/gpu_keepalive_shader.inc"
};

constexpr uint64_t kIdleAfterNs = 250ull * 1000 * 1000;      // stop this long after the last present
constexpr uint64_t kReportNs = 10ull * 1000 * 1000 * 1000;   // one summary line every 10 s of running
// The requested pause between dispatches (the measured one is used); at the default 50 % duty the kernel runs as long. Short slices:
// a GPU that takes turns between its queues (NVIDIA) can hold a frame's work behind a running keep-alive dispatch, and with 2 ms
// slices a frame of 13-17 ms (60-75 Hz) can miss its refresh (an RTX 3060 under native Linux at 75 Hz logged present stalls of
// 25-260 ms only with the keep-alive on). The driver's load measure sees the same share of time either way.
constexpr double kOffMs = 0.5;
constexpr double kOnMaxMs = 2.0;                              // the longest kernel time per dispatch (duty 100 %, or a long measured pause)
constexpr uint32_t kMinIters = 1000, kMaxIters = 8u << 20;    // 8M iterations: ~6 ms at 2.8 GHz, well under a second at 210 MHz

// A pause of about ms without spinning: a high-resolution waitable timer on Windows (Sleep would round up to the timer tick).
void pause_ms(double ms) {
#ifdef _WIN32
    thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -std::max<LONGLONG>(1, static_cast<LONGLONG>(ms * 10000.0));   // 100 ns units, relative
        if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) {
            WaitForSingleObject(timer, static_cast<DWORD>(ms) + 50);
            return;
        }
    }
#endif
    std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(ms));
}
}  // namespace

struct GpuKeepAlive::Impl {
    gfx::Device* dev = nullptr;
    VkDevice vk = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    double duty = 0.5;
    bool timestamps = false;
    double ts_period_ns = 1.0;
    gfx::Buffer sink;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkQueryPool qp = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    std::atomic<uint64_t> last_present_ns{0};
    std::atomic<bool> stop{false};
    std::mutex mu;
    std::condition_variable cv;
    std::thread th;

    bool init();
    void destroy();
    void run();
    bool dispatch(uint32_t iters, double* gpu_ms);
};

bool GpuKeepAlive::Impl::init() {
    vk = dev->device();
    queue = dev->aux_compute_queue();
    const uint32_t fam = dev->aux_compute_family();
    {
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev->physical(), &nq, nullptr);
        std::vector<VkQueueFamilyProperties> qf(nq);
        vkGetPhysicalDeviceQueueFamilyProperties(dev->physical(), &nq, qf.data());
        timestamps = fam < nq && qf[fam].timestampValidBits > 0 && dev->properties().limits.timestampPeriod > 0.0f;
        ts_period_ns = dev->properties().limits.timestampPeriod;
    }
    // a 256-byte storage buffer the shader writes only if its loop ends on one particular value (it keeps the loop from being removed)
    if (dev->create_buffer(256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, gfx::MemoryUse::GpuOnly, &sink, "keepalive sink") != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 1;
    dl.pBindings = &b;
    if (vkCreateDescriptorSetLayout(vk, &dl, nullptr, &dsl) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo dp = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = 1;
    dp.poolSizeCount = 1;
    dp.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(vk, &dp, nullptr, &dpool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo da = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    da.descriptorPool = dpool;
    da.descriptorSetCount = 1;
    da.pSetLayouts = &dsl;
    if (vkAllocateDescriptorSets(vk, &da, &set) != VK_SUCCESS) return false;
    VkDescriptorBufferInfo bi = {sink.buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.pBufferInfo = &bi;
    vkUpdateDescriptorSets(vk, 1, &w, 0, nullptr);
    VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
    VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &dsl;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(vk, &pl, nullptr, &layout) != VK_SUCCESS) return false;
    VkShaderModule sm = make_module(vk, kShader, sizeof(kShader));
    if (!sm) return false;
    VkComputePipelineCreateInfo cp = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cp.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", nullptr};
    cp.layout = layout;
    const VkResult pr = vkCreateComputePipelines(vk, VK_NULL_HANDLE, 1, &cp, nullptr, &pipe);
    vkDestroyShaderModule(vk, sm, nullptr);
    vk_gauge_shader_modules().sub(1);
    if (pr != VK_SUCCESS) return false;
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = fam;
    if (vkCreateCommandPool(vk, &pci, nullptr, &pool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo ca = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ca.commandPool = pool;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(vk, &ca, &cb) != VK_SUCCESS) return false;
    if (timestamps) {
        VkQueryPoolCreateInfo q = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        q.queryType = VK_QUERY_TYPE_TIMESTAMP;
        q.queryCount = 2;
        if (vkCreateQueryPool(vk, &q, nullptr, &qp) != VK_SUCCESS) timestamps = false;
    }
    VkFenceCreateInfo f = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    return vkCreateFence(vk, &f, nullptr, &fence) == VK_SUCCESS;
}

void GpuKeepAlive::Impl::destroy() {
    if (fence) vkDestroyFence(vk, fence, nullptr);
    if (qp) vkDestroyQueryPool(vk, qp, nullptr);
    if (pool) vkDestroyCommandPool(vk, pool, nullptr);
    if (pipe) vkDestroyPipeline(vk, pipe, nullptr);
    if (layout) vkDestroyPipelineLayout(vk, layout, nullptr);
    if (dpool) vkDestroyDescriptorPool(vk, dpool, nullptr);
    if (dsl) vkDestroyDescriptorSetLayout(vk, dsl, nullptr);
    if (sink) dev->destroy_buffer(sink);
}

// One dispatch of iters loop steps, waited for. gpu_ms: its GPU time (timestamps), else the wall time of submit + wait.
bool GpuKeepAlive::Impl::dispatch(uint32_t iters, double* gpu_ms) {
    if (vkResetCommandBuffer(cb, 0) != VK_SUCCESS) return false;
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) return false;
    if (timestamps) {
        vkCmdResetQueryPool(cb, qp, 0, 2);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 0);
    }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &iters);
    vkCmdDispatch(cb, 1, 1, 1);
    if (timestamps) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 1);
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) return false;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    const uint64_t t0 = FrameLimiter::now_ns();
    VkResult r = vkQueueSubmit(queue, 1, &si, fence);
    if (r == VK_SUCCESS) r = vkWaitForFences(vk, 1, &fence, VK_TRUE, 1000ull * 1000 * 1000);
    dev->note_result(r, "keep-alive dispatch");
    if (r != VK_SUCCESS) return false;
    vkResetFences(vk, 1, &fence);
    *gpu_ms = static_cast<double>(FrameLimiter::now_ns() - t0) * 1e-6;
    uint64_t ts[2] = {};
    if (timestamps && vkGetQueryPoolResults(vk, qp, 0, 2, sizeof(ts), ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ts[1] > ts[0])
        *gpu_ms = static_cast<double>(ts[1] - ts[0]) * ts_period_ns * 1e-6;
    return true;
}

void GpuKeepAlive::Impl::run() {
    log_set_thread_name("gpu-keepalive");
    uint32_t iters = 100000;
    double on_ms = duty >= 1.0 ? kOffMs : std::min(kOnMaxMs, kOffMs * duty / (1.0 - duty));   // kernel time per dispatch, steered so on / (on + off) = duty
    double off_avg_ms = kOffMs;
    bool active = false;
    uint64_t rep_t0 = 0, rep_n = 0;
    double rep_on_ms = 0, rep_off_ms = 0;
    while (!stop.load(std::memory_order_relaxed)) {
        const uint64_t now = FrameLimiter::now_ns();
        const bool want = now - last_present_ns.load(std::memory_order_relaxed) < kIdleAfterNs && diag::ab("gpu_keepalive", true) &&
                          !dev->device_lost();
        if (!want) {
            if (active) NXLOG_I(kTag, "paused (no present for 250 ms, the A/B switch, or the device is lost)");
            active = false;
            std::unique_lock<std::mutex> lk(mu);
            cv.wait_for(lk, std::chrono::milliseconds(50), [this] { return stop.load(); });
            continue;
        }
        if (!active) {
            NXLOG_I(kTag, "running");
            active = true;
            rep_t0 = now;
            rep_n = 0;
            rep_on_ms = rep_off_ms = 0;
        }
        double gpu_ms = 0;
        if (!dispatch(iters, &gpu_ms)) {
            NXLOG_W(kTag, "a dispatch failed; the keep-alive stops for this session");
            return;
        }
        if (gpu_ms > 0.0) {   // steer the loop length toward on_ms (the clocks change under it)
            const double f = std::min(2.0, std::max(0.5, on_ms / gpu_ms));
            iters = static_cast<uint32_t>(std::min<double>(kMaxIters, std::max<double>(kMinIters, iters * (0.7 + 0.3 * f))));
        }
        double off_ms = 0;
        if (duty < 1.0) {
            const uint64_t p0 = FrameLimiter::now_ns();
            pause_ms(kOffMs);
            off_ms = static_cast<double>(FrameLimiter::now_ns() - p0) * 1e-6;
            off_avg_ms = off_avg_ms * 0.9 + off_ms * 0.1;
            on_ms = std::min(kOnMaxMs, std::max(0.1, off_avg_ms * duty / (1.0 - duty)));
        }
        rep_n++;
        rep_on_ms += gpu_ms;
        rep_off_ms += off_ms;
        const uint64_t t = FrameLimiter::now_ns();
        if (t - rep_t0 >= kReportNs) {
            const double span_ms = static_cast<double>(t - rep_t0) * 1e-6;
            NXLOG_I(kTag, "%.1f s: %llu dispatches, kernel %.2f ms / pause %.2f ms each, kernel %.0f%% of the time, %u loop steps", span_ms * 1e-3,
                    static_cast<unsigned long long>(rep_n), rep_on_ms / rep_n, rep_off_ms / rep_n, 100.0 * rep_on_ms / span_ms, iters);
            rep_t0 = t;
            rep_n = 0;
            rep_on_ms = rep_off_ms = 0;
        }
    }
}

std::unique_ptr<GpuKeepAlive> GpuKeepAlive::create(gfx::Device& dev) {
    if (!config_get_bool("gfx", "gpu_keepalive", false) || !dev.aux_compute_queue()) return nullptr;
    auto* m = new Impl;
    m->dev = &dev;
    m->duty = static_cast<double>(std::min<int64_t>(100, std::max<int64_t>(10, config_get_int("gfx", "gpu_keepalive_duty", 50)))) / 100.0;
    if (!m->init()) {
        NXLOG_W(kTag, "could not create its compute pipeline; the keep-alive stays off");
        m->destroy();
        delete m;
        return nullptr;
    }
    NXLOG_I(kTag, "on ([gfx] gpu_keepalive): one ALU-only workgroup on compute queue family %u for %.0f%% of the time while frames are presented%s",
            dev.aux_compute_family(), m->duty * 100.0, m->timestamps ? "" : " (no timestamps on that queue: timed by the CPU)");
    m->th = std::thread([m] { m->run(); });
    return std::unique_ptr<GpuKeepAlive>(new GpuKeepAlive(m));
}

GpuKeepAlive::~GpuKeepAlive() {
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        impl_->stop.store(true);
    }
    impl_->cv.notify_all();
    if (impl_->th.joinable()) impl_->th.join();   // the thread waits for its own dispatch, so nothing of it is in flight after this
    impl_->destroy();
    delete impl_;
}

void GpuKeepAlive::note_present() { impl_->last_present_ns.store(FrameLimiter::now_ns(), std::memory_order_relaxed); }

}  // namespace nxrt::extras

// SPDX-License-Identifier: GPL-3.0-or-later
// GPU timestamps of the NVN back end's submissions: see nvn_gpu_timing.h.
//
// GPU busy time for the performance report: the exec pairs of consecutive submissions overlap when the GPU starts one before the last
// ended, so each submission adds only the part after the end of everything collected before it (slots are reused, and so collected, in
// serial order). The present's own command buffer (output pass, upscaler) is not timed.
//
// Pass sample (report): taken at a frame boundary. When perf::pass_sample_take() answers, the frame being recorded is let go (it started
// before), the submissions of the next frame (perf::frame_index() moved once) are all stamped, and the first submission of the frame after
// closes the sample; it is reported when the last stamped slot has been collected.
#include "nvn_gpu_timing.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/ftrace.h"
#include "nxrt/gfx_tuning.h"
#include "nxrt/log.h"
#include "nxrt/perf_report.h"
#include "nxrt/platform_facts.h"
#include "vk_gauges.h"

namespace nxrt::nvn::be::gpu::timing {
namespace {

constexpr const char* kTag = "nvn.gpu";
constexpr uint32_t kGpMax = 1024;      // pass stamps per submission

struct SlotTiming {
    bool timed = false;                // the exec pair was written (query pair index = slot * 2)
    uint64_t cpu_t0 = 0;               // frame trace: the recording start (gpu.exec is drawn from it)
    uint64_t frame = 0;                // perf::frame_index() when it was recorded
    bool profiled = false;             // pass stamps were written
    bool sampled = false;              // ... for the report's pass sample
    uint32_t gp_n = 0;
    std::vector<std::pair<std::string, uint32_t>> gp_labels;   // label and argument of each interval (stamp i closes interval i)
};

enum class Sample { Idle, Waiting, Recording, Closed };

// A phone: GPU time per frame for the game's dynamic resolution (gfx::Tuning::gpu_frame_us): the union of each submission's timestamp
// pair, as the report's busy time, summed per frame; a frame is published when the first submission of a later one is collected.
struct PhoneBusy {
    bool on = false;
    uint64_t end = 0, frame = 0, ns = 0;   // busy end (ticks), the frame being summed, its sum so far
};
PhoneBusy g_pb;

struct Timing {
    gfx::Device* dev = nullptr;
    double period = 1.0;               // ns per timestamp tick
    uint32_t slots = 0;
    VkQueryPool qpool = VK_NULL_HANDLE;    // exec pairs: two per slot
    VkQueryPool gpool = VK_NULL_HANDLE;    // pass stamps: kGpMax per slot
    bool trace_exec = false;           // the frame trace was on at start: gpu.exec events
    bool report_exec = false;          // discrete GPU, report on: GPU busy time
    bool trace_passes = false;         // frame trace + [prof] gpu_passes: every submission stamped, gp:<label> events
    bool sampling = false;             // report pass samples (report_exec without trace_passes)
    std::vector<SlotTiming> slot;
    std::atomic<bool> recording_profiled{false};
    uint64_t busy_end = 0;             // GPU ticks: the end of the last collected exec pair
    Sample state = Sample::Idle;
    uint64_t sample_frame = 0;         // Waiting: the frame the request came in; Recording: the frame stamped
    uint32_t pending = 0, submissions = 0;
    std::map<std::string, perf::PassTime> acc;
};
Timing* g_t = nullptr;

bool make_pool(Timing& t, uint32_t count, VkQueryPool* out) {
    VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount = count;
    if (vkCreateQueryPool(t.dev->device(), &qi, nullptr, out) != VK_SUCCESS) {
        *out = VK_NULL_HANDLE;
        return false;
    }
    vk_gauge_query_pools().add(1);
    return true;
}

void finish_sample(Timing& t) {
    if (t.state != Sample::Closed || t.pending) return;
    std::vector<perf::PassTime> rows;
    rows.reserve(t.acc.size());
    for (auto& [label, p] : t.acc) rows.push_back(std::move(p));
    perf::pass_sample_done(std::move(rows), t.submissions);
    t.acc.clear();
    t.state = Sample::Idle;
}

// Report pass sample: whether the submission starting now is stamped (see the file comment).
bool sample_this(Timing& t) {
    const uint64_t f = perf::frame_index();
    switch (t.state) {
        case Sample::Idle:
            if (!perf::pass_sample_take()) return false;
            if (!t.gpool && !make_pool(t, t.slots * kGpMax, &t.gpool)) return false;
            t.state = Sample::Waiting;
            t.sample_frame = f;
            t.pending = t.submissions = 0;
            return false;
        case Sample::Waiting:
            if (f == t.sample_frame) return false;
            t.state = Sample::Recording;
            t.sample_frame = f;
            break;
        case Sample::Recording:
            if (f != t.sample_frame) {
                t.state = Sample::Closed;
                finish_sample(t);
                return false;
            }
            break;
        case Sample::Closed:
            return false;
    }
    t.submissions++;
    t.pending++;
    return true;
}

}  // namespace

void init(gfx::Device& dev, uint32_t slots) {
    auto* t = new Timing();
    t->dev = &dev;
    t->slots = slots;
    t->period = dev.properties().limits.timestampPeriod;
    t->slot.resize(slots);
    t->trace_exec = ft::on();
    t->report_exec = perf::enabled() && dev.properties().deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU && dev.timestamps_supported();
    t->trace_passes = ft::on() && config_get_bool("prof", "gpu_passes", false);
    if constexpr (facts::kPhone) g_pb.on = dev.timestamps_supported();
    if ((t->trace_exec || t->report_exec || (facts::kPhone && g_pb.on)) && !make_pool(*t, slots * 2, &t->qpool)) {
        t->trace_exec = t->report_exec = false;
        if constexpr (facts::kPhone) g_pb.on = false;
    }
    if (t->trace_passes && !make_pool(*t, slots * kGpMax, &t->gpool)) t->trace_passes = false;
    t->sampling = t->report_exec && !t->trace_passes;
    perf::set_gpu_timing(t->report_exec);
    if (perf::enabled())
        NXLOG_I(kTag, "performance report: %s", t->report_exec ? "GPU busy time from two timestamps per submission; a one-frame pass sample when the frame rate is GPU-bound (discrete GPU)"
                                                                : "no GPU timestamps (not a discrete GPU): the verdict comes from the CPU side");
    g_t = t;
}

void shutdown(gfx::Device& dev) {
    Timing* t = g_t;
    if (!t) return;
    g_t = nullptr;
    if (t->qpool) { vkDestroyQueryPool(dev.device(), t->qpool, nullptr); vk_gauge_query_pools().sub(1); }
    if (t->gpool) { vkDestroyQueryPool(dev.device(), t->gpool, nullptr); vk_gauge_query_pools().sub(1); }
    delete t;
}

bool queued_wanted() { return g_t && g_t->qpool && g_t->trace_exec; }

void begin(VkCommandBuffer cb, uint32_t i) {
    Timing* t = g_t;
    if (!t) return;
    SlotTiming& s = t->slot[i];
    s.profiled = t->trace_passes;
    s.sampled = !s.profiled && t->sampling && sample_this(*t);
    s.profiled = s.profiled || s.sampled;
    t->recording_profiled.store(s.profiled, std::memory_order_relaxed);
    if (s.profiled) {
        vkCmdResetQueryPool(cb, t->gpool, i * kGpMax, kGpMax);
        s.gp_n = 0;
        s.gp_labels.clear();
        pass_stamp(cb, i, "(start)", 0);
    }
    bool stamp = t->qpool != VK_NULL_HANDLE;
    // a phone stamps for the game's dynamic resolution only while it is on (or for the trace and the report)
    if constexpr (facts::kPhone) stamp = stamp && (t->trace_exec || t->report_exec || gfx::tuning().res_dynamic.load(std::memory_order_relaxed));
    if (stamp) {
        vkCmdResetQueryPool(cb, t->qpool, i * 2, 2);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, t->qpool, i * 2);
        s.timed = true;
        s.cpu_t0 = t->trace_exec ? ft::now_ns() : 0;
        s.frame = perf::frame_index();
    }
}

void end(VkCommandBuffer cb, uint32_t i) {
    Timing* t = g_t;
    if (!t) return;
    SlotTiming& s = t->slot[i];
    if (s.profiled) pass_stamp(cb, i, "tail", 0);
    t->recording_profiled.store(false, std::memory_order_relaxed);
    if (s.timed) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, t->qpool, i * 2 + 1);
}

void collect(uint32_t i, uint64_t serial, uint64_t queued) {
    Timing* t = g_t;
    if (!t) return;
    SlotTiming& s = t->slot[i];
    VkDevice d = t->dev->device();
    if (s.timed) {
        s.timed = false;
        uint64_t ts[2] = {0, 0};
        if (vkGetQueryPoolResults(d, t->qpool, i * 2, 2, sizeof ts, ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ts[1] >= ts[0]) {
            if (t->trace_exec) {
                // gpu.exec: drawn from the recording start. gpu.exec.queued: the same GPU time drawn from when the submission's first command buffer
                // was handed to vkQueueSubmit2 (the GPU cannot start before; it starts later when it is still busy with earlier work). Missing for a
                // flush that rode in the present's submit after being parked ([nvn] one_submit).
                static const uint16_t z = ft::zone_id("gpu.exec"), zq = ft::zone_id("gpu.exec.queued");
                const double ns = static_cast<double>(ts[1] - ts[0]) * t->period;
                ft::event(z, s.cpu_t0, s.cpu_t0 + static_cast<uint64_t>(ns), static_cast<uint32_t>(serial));
                if (queued) ft::event(zq, queued, queued + static_cast<uint64_t>(ns), static_cast<uint32_t>(serial));
            }
            if constexpr (facts::kPhone) {
                if (g_pb.on) {
                    if (ts[1] < g_pb.end) g_pb.end = ts[0];
                    const uint64_t from = std::max(ts[0], g_pb.end);
                    const uint64_t busy = ts[1] > from ? static_cast<uint64_t>(static_cast<double>(ts[1] - from) * t->period) : 0;
                    g_pb.end = std::max(g_pb.end, ts[1]);
                    if (s.frame != g_pb.frame) {
                        if (g_pb.ns) {
                            gfx::tuning().gpu_frame_us.store(static_cast<uint32_t>(std::min<uint64_t>(g_pb.ns / 1000, UINT32_MAX)), std::memory_order_relaxed);
                            gfx::tuning().gpu_frames.fetch_add(1, std::memory_order_relaxed);
                        }
                        g_pb.frame = s.frame;
                        g_pb.ns = 0;
                    }
                    g_pb.ns += busy;
                }
            }
            if (t->report_exec) {
                if (ts[1] < t->busy_end) t->busy_end = ts[0];      // a counter of fewer than 64 bits wrapped (timestampValidBits)
                const uint64_t from = std::max(ts[0], t->busy_end);
                const uint64_t busy = ts[1] > from ? static_cast<uint64_t>(static_cast<double>(ts[1] - from) * t->period) : 0;
                t->busy_end = std::max(t->busy_end, ts[1]);
                perf::note_gpu_busy(s.frame, busy);
            }
        }
    }
    if (s.profiled && s.gp_n >= 2) {      // one interval between two stamps each, named after the stamp that closes it, arg = draws
        uint64_t ts[kGpMax];
        const uint32_t n = std::min(s.gp_n, kGpMax);
        if (vkGetQueryPoolResults(d, t->gpool, i * kGpMax, n, sizeof(uint64_t) * n, ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            static std::map<std::string, uint16_t> zones;
            uint64_t at = s.cpu_t0;
            for (uint32_t k = 1; k < n && k < s.gp_labels.size(); k++) {
                const uint64_t dur = ts[k] >= ts[k - 1] ? static_cast<uint64_t>(static_cast<double>(ts[k] - ts[k - 1]) * t->period) : 0;
                const auto& [label, arg] = s.gp_labels[k];
                if (t->trace_passes) {
                    auto it = zones.find(label);
                    if (it == zones.end()) it = zones.emplace(label, ft::zone_id(("gp:" + label).c_str())).first;
                    ft::event(it->second, at, at + dur, arg);
                    at += dur;
                }
                if (s.sampled) {
                    perf::PassTime& p = t->acc[label];
                    if (p.label.empty()) p.label = label;
                    p.ns += dur;
                    p.draws += arg;
                }
            }
        }
    }
    if (s.sampled && t->pending) {
        t->pending--;
        finish_sample(*t);
    }
    s.gp_n = 0;
    s.gp_labels.clear();
    s.profiled = s.sampled = false;
}

bool pass_on() { return g_t && g_t->recording_profiled.load(std::memory_order_relaxed); }

void pass_stamp(VkCommandBuffer cb, uint32_t i, const char* label, uint32_t arg) {
    Timing* t = g_t;
    if (!t || !t->gpool) return;
    SlotTiming& s = t->slot[i];
    if (!s.profiled || s.gp_n >= kGpMax) return;
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, t->gpool, i * kGpMax + s.gp_n);
    s.gp_labels.emplace_back(label, arg);
    s.gp_n++;
}

}  // namespace nxrt::nvn::be::gpu::timing

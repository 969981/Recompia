// SPDX-License-Identifier: GPL-3.0-or-later
// AMD FSR 3.1 frame generation through the FidelityFX API (framegen.h). The loader and the effect DLLs are the ones the upscaler uses
// (ffx_dll.h): the v2.4.0 loader loads amd_fidelityfx_framegeneration_vk.dll from its own folder.
#include "extras/framegen.h"

#include <windows.h>

#include <atomic>

#include "extras/ffx_dll.h"
#include "nxrt/extras.h"
#include "nxrt/gfx.h"
#include "nxrt/log.h"

#include "../../../third_party/amd_ffx_api/api/include/ffx_api.h"
#include "../../../third_party/amd_ffx_api/api/include/ffx_api_types.h"
#include "../../../third_party/amd_ffx_api/api/include/vk/ffx_api_vk.h"
#include "../../../third_party/amd_ffx_api/framegeneration/include/ffx_framegeneration.h"
#include "../../../third_party/amd_ffx_api/framegeneration/include/vk/ffx_api_framegeneration_vk.h"

namespace nxrt::extras {
namespace {

constexpr const char* kTag = "framegen";

// The one frame-generation swapchain. The renderer reaches it only through the table below (swapchain_fns.h). `native` is set when AMD's code
// could not make the first swapchain: every call then forwards to Vulkan, so the game runs on without frame generation.
struct State {
    gfx::Device* dev = nullptr;
    HMODULE mod = nullptr;
    PfnFfxCreateContext create_ctx = nullptr;
    PfnFfxDestroyContext destroy_ctx = nullptr;
    PfnFfxQuery query = nullptr;
    ffxContext ctx = nullptr;
    ffxQueryDescSwapchainReplacementFunctionsVK fns = {};
    bool native = false;
    gfx::SwapchainFns table;
};
State g;

bool load_api() {
    const std::wstring path = ffx_find_dll();
    if (path.empty()) return false;
    g.mod = LoadLibraryW(path.c_str());
    if (!g.mod) return false;
    g.create_ctx = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(g.mod, "ffxCreateContext"));
    g.destroy_ctx = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(g.mod, "ffxDestroyContext"));
    g.query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(g.mod, "ffxQuery"));
    return g.create_ctx && g.destroy_ctx && g.query;
}

// AMD's code makes the swapchain, the first time with the device's four queues; later calls make it again in place (a resize).
bool make_first(const VkSwapchainCreateInfoKHR* ci, VkSwapchainKHR* out) {
    const gfx::Device::FrameGenQueues& q = g.dev->framegen_queues();
    ffxCreateContextDescFrameGenerationSwapChainVersionVK ver = {};
    ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_VK;
    ver.version = FFX_FRAMEGENERATION_SWAPCHAIN_VK_VERSION;
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    ffxCreateContextDescFrameGenerationSwapChainVK d = {};
    d.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK;
    d.header.pNext = &ver.header;
    d.physicalDevice = g.dev->physical();
    d.device = g.dev->device();
    d.swapchain = &sc;
    d.createInfo = *ci;
    d.createInfo.oldSwapchain = VK_NULL_HANDLE;
    d.gameQueue = {g.dev->queue(), g.dev->queue_family(), nullptr};
    d.asyncComputeQueue = {q.async_compute.queue, q.async_compute.family, nullptr};
    d.presentQueue = {q.present.queue, q.present.family, nullptr};
    d.imageAcquireQueue = {q.acquire.queue, q.acquire.family, nullptr};
    const ffxReturnCode_t r = g.create_ctx(&g.ctx, &d.header, nullptr);
    if (r != FFX_API_RETURN_OK) {
        NXLOG_E(kTag, "ffxCreateContext (frame generation swapchain) failed (%u): no frame generation", r);
        g.ctx = nullptr;
        return false;
    }
    // The replacement functions: asked of the new context, and of the global state when that gives nothing
    const auto have = [] {
        return g.fns.pOutCreateSwapchainFFXAPI && g.fns.pOutDestroySwapchainFFXAPI && g.fns.pOutGetSwapchainImagesKHR && g.fns.pOutAcquireNextImageKHR &&
               g.fns.pOutQueuePresentKHR;
    };
    ffxContext* asks[2] = {&g.ctx, nullptr};
    for (ffxContext* c : asks) {
        g.fns = {};
        g.fns.header.type = FFX_API_QUERY_DESC_TYPE_FGSWAPCHAIN_FUNCTIONS_VK;
        const ffxReturnCode_t qr = g.query(c, &g.fns.header);
        NXLOG_I(kTag, "swapchain functions asked of the %s: code %u, create %d destroy %d images %d acquire %d present %d", c ? "context" : "global state", qr,
                g.fns.pOutCreateSwapchainFFXAPI != nullptr, g.fns.pOutDestroySwapchainFFXAPI != nullptr, g.fns.pOutGetSwapchainImagesKHR != nullptr,
                g.fns.pOutAcquireNextImageKHR != nullptr, g.fns.pOutQueuePresentKHR != nullptr);
        if (qr == FFX_API_RETURN_OK && have()) break;
    }
    if (!have()) {
        NXLOG_E(kTag, "the frame generation DLL gave no swapchain functions: no frame generation");
        g.destroy_ctx(&g.ctx, nullptr);     // the context does not take its swapchain with it: the window stays in use until that is destroyed
        g.ctx = nullptr;
        if (sc) vkDestroySwapchainKHR(g.dev->device(), sc, nullptr);
        return false;
    }
    *out = sc;
    return true;
}

// The swapchain AMD's code made and has not been told to destroy; stale handles the renderer passes later are not ours to destroy.
VkSwapchainKHR g_live = VK_NULL_HANDLE;

// Destroys the context, then its swapchain (the context does not take the swapchain with it).
void destroy_live(VkDevice device, const VkAllocationCallbacks* alloc) {
    const VkSwapchainKHR sc = g_live;
    g_live = VK_NULL_HANDLE;
    ffxContext gone = g.ctx;
    g.ctx = nullptr;
    g.destroy_ctx(&gone, nullptr);
    g.fns.pOutDestroySwapchainFFXAPI(device, sc, alloc, reinterpret_cast<void*>(sc));   // pContext: the swapchain being destroyed
}

// A swapchain made again (a resize, a present mode) is made from scratch: the in-place call of AMD's code hands the old handle, which is not
// a Vulkan swapchain, to vkCreateSwapchainKHR, and a layer on the way (RivaTuner's) crashes on it.
VKAPI_ATTR VkResult VKAPI_CALL fg_create(VkDevice device, const VkSwapchainCreateInfoKHR* ci_in, const VkAllocationCallbacks* alloc, VkSwapchainKHR* out) {
    VkSwapchainCreateInfoKHR ci = *ci_in;
    if (!g.native && g.ctx) {
        g.dev->wait_idle();
        destroy_live(device, alloc);
    }
    if (!g.native) {
        ci.oldSwapchain = VK_NULL_HANDLE;
        if (make_first(&ci, out)) {
            g_live = *out;
            NXLOG_I(kTag, "frame generation swapchain made (%ux%u)", ci.imageExtent.width, ci.imageExtent.height);
            return VK_SUCCESS;
        }
        g.native = true;
    }
    ci.oldSwapchain = VK_NULL_HANDLE;
    return vkCreateSwapchainKHR(device, &ci, alloc, out);
}

VKAPI_ATTR void VKAPI_CALL fg_destroy(VkDevice device, VkSwapchainKHR sc, const VkAllocationCallbacks* alloc) {
    if (g.native) vkDestroySwapchainKHR(device, sc, alloc);
    else if (g.ctx && sc == g_live) destroy_live(device, alloc);
}

// The first calls of each kind are logged before they run: a crash inside the DLL shows which call it was.
void trace_first(const char* what) {
    static std::atomic<int> n{0};
    if (n.fetch_add(1) < 12) NXLOG_I(kTag, "call: %s", what);
}

VKAPI_ATTR VkResult VKAPI_CALL fg_images(VkDevice d, VkSwapchainKHR sc, uint32_t* n, VkImage* imgs) {
    trace_first(imgs ? "get images" : "count images");
    return g.native ? vkGetSwapchainImagesKHR(d, sc, n, imgs) : g.fns.pOutGetSwapchainImagesKHR(d, sc, n, imgs);
}
VKAPI_ATTR VkResult VKAPI_CALL fg_acquire(VkDevice d, VkSwapchainKHR sc, uint64_t timeout, VkSemaphore s, VkFence f, uint32_t* idx) {
    trace_first("acquire");
    return g.native ? vkAcquireNextImageKHR(d, sc, timeout, s, f, idx) : g.fns.pOutAcquireNextImageKHR(d, sc, timeout, s, f, idx);
}
VKAPI_ATTR VkResult VKAPI_CALL fg_present(VkQueue q, const VkPresentInfoKHR* pi) {
    trace_first("present");
    return g.native ? vkQueuePresentKHR(q, pi) : g.fns.pOutQueuePresentKHR(q, pi);
}
VKAPI_ATTR void VKAPI_CALL fg_hdr_metadata(VkDevice d, uint32_t n, const VkSwapchainKHR* scs, const VkHdrMetadataEXT* md) {
    if (g.native || !g.fns.pOutSetHdrMetadataEXT) vkSetHdrMetadataEXT(d, n, scs, md);
    else g.fns.pOutSetHdrMetadataEXT(d, n, scs, md);
}

}  // namespace


bool framegen_dll_found() {
    const std::wstring loader = ffx_find_dll();
    return !loader.empty() && ffx_effect_dll_exists(loader, L"amd_fidelityfx_framegeneration_vk.dll");
}

const gfx::SwapchainFns* framegen_swapchain(gfx::Device& dev) {
    if (!settings().framegen.enabled) return nullptr;
    if (!dev.framegen_queues().ok || !framegen_dll_found() || !load_api()) {
        NXLOG_W(kTag, "frame generation is on but cannot start: %s", framegen_missing_text());
        return nullptr;
    }
    g.dev = &dev;
    g.table.create = fg_create;
    g.table.destroy = fg_destroy;
    g.table.images = fg_images;
    g.table.acquire = fg_acquire;
    g.table.present = fg_present;
    g.table.hdr_metadata = fg_hdr_metadata;
    return &g.table;
}

const char* framegen_missing_text() {
    return "Frame generation needs amd_fidelityfx_framegeneration_vk.dll (AMD FSR SDK v2.4.0, signedbin) in the game's upscalers\\fsr3 folder, "
           "next to amd_fidelityfx_loader_vk.dll. Put it there and restart the game.";
}

}  // namespace nxrt::extras

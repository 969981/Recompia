// SPDX-License-Identifier: GPL-3.0-or-later
// AMD FSR 3.1 through the FidelityFX API (temporal.h). Where the API comes from is temporal_ffx_api.h's job: the signed Vulkan DLLs on
// Windows (temporal_ffx_dll.cpp), the SDK's own sources linked into the runtime on Linux (temporal_ffx_linked.cpp). The upscaler
// version is the newest the provider offers for this GPU (FSR 3.1.x; FSR 4 where AMD's Windows DLLs support it).
#include <string>

#include "extras/temporal.h"
#include "extras/temporal_ffx_api.h"
#include "nxrt/gfx.h"
#include "nxrt/log.h"

namespace nxrt::extras {
namespace {

constexpr const char* kTag = "fsr3";

// FidelityFX messages are wide strings (UTF-16 on Windows, UTF-32 on Linux) and plain ASCII in practice.
void on_message(uint32_t type, const wchar_t* msg) {
    std::string s;
    for (const wchar_t* p = msg ? msg : L""; *p && s.size() < 511; ++p) s += (*p > 0 && *p < 0x80) ? static_cast<char>(*p) : '?';
    if (type == FFX_API_MESSAGE_TYPE_ERROR) NXLOG_E(kTag, "%s", s.c_str());
    else NXLOG_W(kTag, "%s", s.c_str());
}

// The FidelityFX Vulkan backend asks for some functions by their extension name (vkGetBufferMemoryRequirements2KHR); the runtime's
// Vulkan 1.3 device has them as core functions without enabling those extensions, where the extension name returns null.
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL device_proc(VkDevice d, const char* name) {
    if (PFN_vkVoidFunction f = vkGetDeviceProcAddr(d, name)) return f;
    const std::string n = name;
    return n.size() > 3 && n.compare(n.size() - 3, 3, "KHR") == 0 ? vkGetDeviceProcAddr(d, n.substr(0, n.size() - 3).c_str()) : nullptr;
}

FfxApiResource resource(const TemporalImage& t, uint32_t state, uint32_t usage) {
    FfxApiResource r = {};
    r.resource = reinterpret_cast<void*>(t.image);
    r.state = state;
    r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    r.description.format = t.format == VK_FORMAT_R16G16B16A16_SFLOAT ? FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT
                         : t.format == VK_FORMAT_R16G16_SFLOAT     ? FFX_API_SURFACE_FORMAT_R16G16_FLOAT
                         : t.format == VK_FORMAT_R32_SFLOAT        ? FFX_API_SURFACE_FORMAT_R32_FLOAT
                                                                   : FFX_API_SURFACE_FORMAT_UNKNOWN;
    r.description.width = t.width;
    r.description.height = t.height;
    r.description.depth = 1;
    r.description.mipCount = 1;
    r.description.flags = FFX_API_RESOURCE_FLAGS_NONE;
    r.description.usage = usage;
    return r;
}

class Ffx final : public TemporalBackend {
public:
    const char* name() const override { return version_.empty() ? "FSR 3" : version_.c_str(); }

    bool load(gfx::Device& dev) {
        dev_ = &dev;
        return ffx_api_load(&api_);
    }

    bool evaluate(VkCommandBuffer cb, const TemporalFrame& f) override {
        if (failed_) return false;
        if (!ctx_ || f.render.width > max_render_.width || f.render.height > max_render_.height || f.out.width != max_out_.width || f.out.height != max_out_.height ||
            f.depth_inverted != depth_inverted_ || f.hdr != hdr_) {
            if (!make(f.render, f.out, f.depth_inverted, f.hdr)) {
                failed_ = true;
                return false;
            }
        }
        ffxDispatchDescUpscale d = {};
        d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        d.commandList = reinterpret_cast<void*>(cb);
        d.color = resource(f.color, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, FFX_API_RESOURCE_USAGE_UAV);
        d.depth = resource(f.depth, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, FFX_API_RESOURCE_USAGE_UAV);
        d.motionVectors = resource(f.motion, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, FFX_API_RESOURCE_USAGE_UAV);
        d.output = resource(f.output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, FFX_API_RESOURCE_USAGE_UAV);
        d.jitterOffset = {f.jitter_px[0], f.jitter_px[1]};
        d.motionVectorScale = {f.mv_scale[0], f.mv_scale[1]};
        d.renderSize = {f.render.width, f.render.height};
        d.upscaleSize = {f.out.width, f.out.height};
        d.enableSharpening = f.sharpness > 0.0f;
        d.sharpness = f.sharpness;
        d.frameTimeDelta = f.frame_ms;
        d.preExposure = 1.0f;
        d.reset = f.reset;
        d.cameraNear = f.near_z;   // FSR orders near / far itself from the inverted-depth flag
        d.cameraFar = f.far_z;
        d.cameraFovAngleVertical = f.fov_y;
        d.viewSpaceToMetersFactor = 1.0f;
        const ffxReturnCode_t r = api_.dispatch(&ctx_, &d.header);
        if (r != FFX_API_RETURN_OK) {
            NXLOG_E(kTag, "ffxDispatch failed (%u): FSR 3 off", r);
            failed_ = true;
            return false;
        }
        return true;
    }

private:
    bool make(VkExtent2D render, VkExtent2D out, bool depth_inverted, bool hdr) {
        if (ctx_) {
            dev_->wait_idle();
            api_.destroy(&ctx_, nullptr);
            ctx_ = nullptr;
        }
        ffxCreateBackendVKDesc be = {};
        be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
        be.deviceContext.vkDevice = dev_->device();
        be.deviceContext.vkPhysicalDevice = dev_->physical();
        be.deviceContext.vkDeviceProcAddr = device_proc;
        ffxCreateContextDescUpscale cd = {};
        cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        cd.header.pNext = &be.header;
        cd.flags = (hdr ? FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE : 0) | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE |
                   (depth_inverted ? FFX_UPSCALE_ENABLE_DEPTH_INVERTED : 0);
        cd.maxRenderSize = {render.width, render.height};
        cd.maxUpscaleSize = {out.width, out.height};
        cd.fpMessage = &on_message;
        ffxReturnCode_t r = api_.create(&ctx_, &cd.header, nullptr);
        if (r != FFX_API_RETURN_OK) {   // the v1.1.4 DLL knows only the older backend descriptor
            ffxCreateBackendVKDesc_DEPRECATED old = {};
            old.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK_DEPRECATED;
            old.vkDevice = dev_->device();
            old.vkPhysicalDevice = dev_->physical();
            old.vkDeviceProcAddr = device_proc;
            cd.header.pNext = &old.header;
            ctx_ = nullptr;
            r = api_.create(&ctx_, &cd.header, nullptr);
        }
        if (r != FFX_API_RETURN_OK) {
            NXLOG_E(kTag, "ffxCreateContext (upscale %ux%u -> %ux%u) failed (%u): FSR 3 off", render.width, render.height, out.width, out.height, r);
            ctx_ = nullptr;
            return false;
        }
        max_render_ = render;
        max_out_ = out;
        depth_inverted_ = depth_inverted;
        hdr_ = hdr;
        ffxQueryGetProviderVersion pv = {};
        pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        if (api_.query(&ctx_, &pv.header) == FFX_API_RETURN_OK && pv.versionName) version_ = std::string("FSR ") + pv.versionName;
        NXLOG_I(kTag, "context made: %s, %ux%u -> %ux%u, %s, %s depth, auto exposure", name(), render.width, render.height, out.width, out.height,
                hdr ? "HDR" : "display-referred", depth_inverted ? "inverted" : "forward");
        return true;
    }

    gfx::Device* dev_ = nullptr;
    FfxApi api_ = {};
    ffxContext ctx_ = nullptr;
    VkExtent2D max_render_ = {}, max_out_ = {};
    bool depth_inverted_ = false;
    bool hdr_ = true;
    std::string version_;
    bool failed_ = false;
};

}  // namespace

bool temporal_ffx_available() { return ffx_api_present(); }

TemporalBackend* temporal_ffx(void* gfx_device) {
    static Ffx* inst = nullptr;
    static bool tried = false;
    if (!tried && gfx_device) {
        tried = true;
        auto* f = new Ffx();
        if (f->load(*static_cast<gfx::Device*>(gfx_device))) inst = f;
        else delete f;
    }
    return inst;
}

}  // namespace nxrt::extras

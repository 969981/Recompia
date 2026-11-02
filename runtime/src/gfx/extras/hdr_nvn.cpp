// SPDX-License-Identifier: GPL-3.0-or-later
#include "extras/hdr_nvn.h"

#include <mutex>
#include <map>
#include <set>

#include "nxrt/extras.h"
#include "nxrt/log.h"
#include "extras/fsr1_spatial.h"
#include "extras/hdr_tonemap_patch.h"

namespace nxrt::extras {
namespace {
constexpr int kFlagDisplay = 0x1, kFlagCompressible = 0x8;   // NVN_TEXTURE_FLAGS_DISPLAY, NVN_TEXTURE_FLAGS_COMPRESSIBLE
}

VkFormat hdr_promote_target(VkFormat chosen, int flags, bool is_2d, uint32_t levels, uint32_t layers, uint32_t samples, bool nvn_srgb) {
    if (!nvn_srgb || chosen != VK_FORMAT_R8G8B8A8_SRGB || !is_2d || levels != 1 || layers != 1 || samples > 1) return VK_FORMAT_UNDEFINED;
    if (!(flags & (kFlagDisplay | kFlagCompressible))) return VK_FORMAT_UNDEFINED;
    if (!hdr_scene_active()) return VK_FORMAT_UNDEFINED;
    return VK_FORMAT_R16G16B16A16_SFLOAT;
}

bool hdr_tonemap_variant(const std::vector<uint32_t>& words, const std::string& hash, std::vector<uint32_t>& out) {
    if (!hdr_scene_active()) return false;
    static std::mutex mu;
    static std::set<std::string> tried;
    const bool ok = spirv_hdr_tonemap(words, out);
    std::lock_guard<std::mutex> lk(mu);
    if (tried.insert(hash).second && ok) NXLOG_I("hdr", "tone-map shader %s: HDR variant made", hash.c_str());
    return ok;
}

bool hdr_is_tonemap(const std::vector<uint32_t>& words, const std::string& hash) {
    static std::mutex mu;
    static std::map<std::string, bool> cache;
    {
        std::lock_guard<std::mutex> lk(mu);
        auto it = cache.find(hash);
        if (it != cache.end()) return it->second;
    }
    std::vector<uint32_t> scratch;
    const bool ok = words.size() <= 16384 && spirv_hdr_tonemap(words, scratch);
    std::lock_guard<std::mutex> lk(mu);
    cache[hash] = ok;
    return ok;
}

bool scene_upscale_wanted() { return upscale_mode() == UpscaleMode::Fsr1; }

Fsr1Spatial* scene_upscaler(void* gfx_device) {
    static Fsr1Spatial* inst = nullptr;
    static bool tried = false;
    if (!tried && gfx_device) {
        tried = true;
        auto* f = new Fsr1Spatial();
        if (f->init(*static_cast<gfx::Device*>(gfx_device))) inst = f;
        else delete f;
    }
    return inst;
}

float scene_upscale_prescale() { return 0.1f; }

}  // namespace nxrt::extras

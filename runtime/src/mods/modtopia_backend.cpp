// SPDX-License-Identifier: GPL-3.0-or-later
// The mods host's MtBackend (Modtopia docs/HOST.md section 2): what the Modtopia core asks of the running game. Guest memory, the registers
// of a hook stop, guest calls, module bases, which hook points the game build has, the game's own RomFS files, the save folder and messages
// over the picture (notify), the players' pads (pad_read, pad_hold, pad_block: modtopia_input.cpp) and sound under the game's (audio_*).
// Every other callback (notify_image, screenshot, key_down, colour matrix, HUD, network, file notes) stays NULL: the core then tells a mod that asks for it that this host has none (MT_ERR_VERSION), or sends the text alone.
#include <windows.h>

#include <cstring>
#include <string>

#include "mods_internal.h"
#include "nxrt/audio_sink.h"
#include "nxrt/dispatch.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/module.h"
#include "nxrt/notices.h"
#include "nxrt/services_api.h"

namespace nxrt::mods::detail {

namespace {

constexpr const char* kTag = "mods";

// Register numbers of MtBackend::reg_get / reg_set (modtopia_host.h).
constexpr int kRegSp = 31, kRegPc = 32, kRegNzcv = 33, kRegFpcr = 34, kRegFpsr = 35;

thread_local uint64_t t_skip_site = 0;   // call_original: the entry stop at this site is skipped once

// Every byte of [addr, addr + n) is the guest's own memory (its memory map, svcQueryMemory's view) and readable, or also writable.
// Guest addresses are host addresses: this is what keeps a mod's guest address from reaching the host's own memory.
bool guest_range(uint64_t addr, size_t n, bool write) {
    if (n == 0) return true;
    const uint64_t end = addr + n;
    if (end < addr) return false;
    for (uint64_t a = addr; a < end;) {
        const mem::VmInfo v = mem::vm_query(a);
        if (v.state == mem::kMemFree || v.state == mem::kMemInaccessible || !(v.perm & (write ? 2u : 1u)) || v.addr + v.size <= a) return false;
        a = v.addr + v.size;
    }
    return true;
}

// The copy itself, guarded: a page the map calls mapped may still fault (a race with an unmap). The write tracker's vectored handler serves
// its guarded pages before this handler is asked (nxrt/write_watch.h). No C++ object may live in this frame (SEH).
bool guarded_copy(void* dst, const void* src, size_t n) {
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void be_log(void*, int level, const char* line) {
    switch (level) {
        case MT_LOG_DEBUG: NXLOG_D(kTag, "%s", line); break;
        case MT_LOG_INFO: NXLOG_I(kTag, "%s", line); break;
        case MT_LOG_WARN: NXLOG_W(kTag, "%s", line); break;
        default: NXLOG_E(kTag, "%s", line); break;
    }
}

int be_mem_read(void*, uint64_t addr, void* out, size_t n) {
    if (!guest_range(addr, n, false)) return MT_ERR_FAULT;
    return guarded_copy(out, reinterpret_cast<const void*>(addr), n) ? MT_OK : MT_ERR_FAULT;
}
int be_mem_write(void*, uint64_t addr, const void* in, size_t n) {
    if (!guest_range(addr, n, true)) return MT_ERR_FAULT;
    return guarded_copy(reinterpret_cast<void*>(addr), in, n) ? MT_OK : MT_ERR_FAULT;
}

uint64_t be_module_base(void*, const char* name) {
    const Module* m = name ? module_by_name(name) : nullptr;
    return m ? m->base() : 0;
}

int be_reg_get(void*, void* host_ctx, int reg, uint64_t* out) {
    const Stop* s = static_cast<const Stop*>(host_ctx);
    if (!s || !out) return MT_ERR_UNKNOWN;
    const ::Ctx* c = s->c;
    if (reg >= 0 && reg <= kRegSp) *out = c->x[reg];
    else if (reg == kRegPc) *out = s->site;
    else if (reg == kRegNzcv) *out = (uint64_t{c->N & 1} << 31) | (uint64_t{c->Z & 1} << 30) | (uint64_t{c->C & 1} << 29) | (uint64_t{c->V & 1} << 28);
    else if (reg == kRegFpcr) *out = c->fpcr;
    else if (reg == kRegFpsr) *out = c->fpsr;
    else return MT_ERR_UNKNOWN;
    return MT_OK;
}
// NZCV is not writable: the lifted code keeps the flags in a local across a site callout and does not read them back from the Ctx there
// (recomp/nxrecomp/locals.py, 'lean': nx_site_* are FLAT_RUNTIME), so a write could not reach the game's code. pc is read only (HOST.md).
int be_reg_set(void*, void* host_ctx, int reg, uint64_t value) {
    Stop* s = static_cast<Stop*>(host_ctx);
    if (!s) return MT_ERR_UNKNOWN;
    ::Ctx* c = s->c;
    if (reg >= 0 && reg <= kRegSp) c->x[reg] = value;
    else if (reg == kRegFpcr) c->fpcr = value;
    else if (reg == kRegFpsr) c->fpsr = value;
    else return MT_ERR_UNKNOWN;
    return MT_OK;
}
int be_vreg_get(void*, void* host_ctx, int vreg, void* out16) {
    const Stop* s = static_cast<const Stop*>(host_ctx);
    if (!s || !out16 || vreg < 0 || vreg > 31) return MT_ERR_UNKNOWN;
    std::memcpy(out16, s->c->v[vreg].b, 16);
    return MT_OK;
}
int be_vreg_set(void*, void* host_ctx, int vreg, const void* in16) {
    Stop* s = static_cast<Stop*>(host_ctx);
    if (!s || !in16 || vreg < 0 || vreg > 31) return MT_ERR_UNKNOWN;
    std::memcpy(s->c->v[vreg].b, in16, 16);
    return MT_OK;
}

// A game function on the calling game thread, on that thread's own registers (the stop's, or the thread's when an event calls): x0..x7 and
// v0..v7 from regs, everything else as the caller has it, and the whole register file put back afterwards (as the game layer's guest calls
// do), so the stop's registers come back unchanged. An address that is neither a lifted function nor a host function is refused instead
// of trapping.
int be_call_guest(void*, void* host_ctx, uint64_t addr, MtCall* regs) {
    ::Ctx* own = kernel::current_ctx();
    if (!own || !regs) return MT_ERR_STATE;
    ::Ctx* c = host_ctx ? static_cast<Stop*>(host_ctx)->c : own;
    if (!fn_lookup(addr) && !hle_is_thunk(addr)) return MT_ERR_UNKNOWN;
    const ::Ctx saved = *c;
    for (int i = 0; i < 8; ++i) {
        c->x[i] = regs->x[i];
        std::memcpy(c->v[i].b, regs->v[i], 16);
    }
    dispatch_call(c, addr);
    for (int i = 0; i < 8; ++i) {
        regs->x[i] = c->x[i];
        std::memcpy(regs->v[i], c->v[i].b, 16);
    }
    *c = saved;
    return MT_OK;
}

// replace points: the game's function at site on the stop's registers, its entry stop skipped this once (take_skip in the entry hook);
// its results stay in the stop's registers.
int be_call_original(void*, void* host_ctx, uint64_t site) {
    Stop* s = static_cast<Stop*>(host_ctx);
    if (!s || !kernel::current_ctx() || !fn_lookup(site)) return MT_ERR_STATE;
    t_skip_site = site;
    dispatch_call(s->c, site);
    t_skip_site = 0;
    return MT_OK;
}

int be_hook_built_in(void*, uint64_t addr, int kind) {
    const auto it = host().built.find(addr);
    return it != host().built.end() && it->second.plan_kind >= kind ? 1 : 0;
}

int be_game_has_file(void*, const char* path) { return path && services::fsapi::romfs_game_file_size(path) >= 0 ? 1 : 0; }
int64_t be_game_file_size(void*, const char* path) { return path ? services::fsapi::romfs_game_file_size(path) : -1; }
int64_t be_game_read(void*, const char* path, uint64_t offset, void* buf, uint64_t n) {
    return path && buf ? services::fsapi::romfs_game_read(path, offset, buf, n) : -1;
}

const char* be_save_dir(void*) { return host().save_dir.c_str(); }

int be_pad_read(void*, int p, MtPad* out) { return pad_read(p, out); }
int be_pad_hold(void*, int p, const MtPad* add) { return pad_hold(p, add); }
int be_pad_block(void*, int p, const MtPad* mask) { return pad_block(p, mask); }

// A mod's sound: the core gives each mod that plays a voice (0..MT_AUDIO_VOICES - 1), mixed under the game's output (nxrt/audio_sink.h).
static_assert(MT_AUDIO_VOICES <= audio::kExtraVoices, "a voice for every voice the core gives out");
int be_audio_write(void*, int voice, const int16_t* stereo, int frames) {
    return frames > 0 ? static_cast<int>(audio::extra_voice_write(voice, stereo, static_cast<uint32_t>(frames))) : 0;
}
int be_audio_queued(void*, int voice) { return static_cast<int>(audio::extra_voice_queued(voice)); }
int be_audio_clear(void*, int voice) {
    audio::extra_voice_clear(voice);
    return MT_OK;
}

// A mod's message over the picture (nxrt/notices.h); its newer one replaces its earlier one.
int be_notify(void*, const char* mod_id, const char* text, double seconds) {
    if (!mod_id || !text) return MT_ERR_UNKNOWN;
    notices::post(std::string("mod:") + mod_id, text, seconds);
    return MT_OK;
}

}  // namespace

bool take_skip(uint64_t site) {
    if (t_skip_site != site) return false;
    t_skip_site = 0;
    return true;
}

MtBackend make_backend() {
    Host& h = host();
    MtBackend be{};
    be.size = sizeof be;
    be.host_name = h.host_name.c_str();
    be.game_version = h.game_version.c_str();
    be.game_build_id = h.build_id.empty() ? nullptr : h.build_id.c_str();
    be.platform = nullptr;   // mt_platform(): this build's own tag
    be.log = &be_log;
    be.mem_read = &be_mem_read;
    be.mem_write = &be_mem_write;
    be.module_base = &be_module_base;
    be.reg_get = &be_reg_get;
    be.reg_set = &be_reg_set;
    be.vreg_get = &be_vreg_get;
    be.vreg_set = &be_vreg_set;
    be.call_guest = &be_call_guest;
    be.call_original = &be_call_original;
    be.hook_built_in = &be_hook_built_in;
    be.game_has_file = &be_game_has_file;
    be.game_file_size = &be_game_file_size;
    be.game_read = &be_game_read;
    be.save_dir = &be_save_dir;
    be.notify = &be_notify;
    be.pad_read = &be_pad_read;
    be.pad_hold = &be_pad_hold;
    be.pad_block = &be_pad_block;
    be.audio_write = &be_audio_write;
    be.audio_queued = &be_audio_queued;
    be.audio_clear = &be_audio_clear;
    return be;
}

}  // namespace nxrt::mods::detail

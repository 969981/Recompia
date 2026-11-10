// SPDX-License-Identifier: GPL-3.0-or-later
// stpl:u: Switch 2 stream play (GameShare), answered as "nothing to share" (see misc.cpp).
#include "misc_internal.h"

namespace nxrt::services::miscsvc {

namespace {

// stpl:u (Switch 2 stream play / GameShare): nn::streamplay::detail::GetUserService opens it and calls cmd 0 for the user service object.
// GameShare is not available here, but the two IUserService commands the SDK sends at start-up abort the process on any failure
// (sdk 0x75006b9744 and 0x75006ad634: AbortImpl), so they answer "nothing to share" instead of failing:
//   300 GetHidSharedMemory (sdk proxy 0x75006b78f4, caller GetUserService 0x75006b95f0): out {u64 size, u64 offset} + a copy handle.
//       HidApplicationResourceHolder (0x75006b08b8) attaches `size` bytes, maps them and reads the guest-input format at `offset`.
//   1   GetRequiredMemorySize(u8 type, u8 mode, u32 guests) -> u32 (sdk proxy 0x75006b76f0): the work memory sizes below (it was 0, but
//       nn::streamplay::Initialize divides the buffer size by the per-guest size, CalculateGuestCountFromMemorySize
//       (sdk 0x75006b9850), and aborts when the quotient is not at least 1: 0 made hosting a GameShare session abort).
// The format, from HidSharedMemoryAccessor's readers (sdk 0x75006b0980..0x75006b24a0), for up to three guests (IsGuestIdValid: id < 3):
// pads at 0, 0x24000 per guest pad id (0, 1, 32 = handheld) and 0xC000 per guest inside it (u32 NpadStyleSet at +0, pad ring at +0x28
// of 33 x 0x38 entries, six-axis rings from +0x2490 every 0xE80, controller colours at +0x7B90); touch screens at 0x6C000, 0x6000 per
// guest; Jcms rings at 0x7E000, 0x1800 per (pad 0/1, Joy-Con type 0/1) and 0x800 per guest. The readers reach 0x84000. "No GameShare
// session" is all zero: every style set 0 (GetGuestPadStyleSet: no pad) and every ring count 0 (the state readers return 0 entries).
//
// Hosting a GameShare session (Pokopia: main 0x7101132f60 state 1 nn::streamplay::Initialize, state 2 OpenSessionWithMatchmakeSupport).
// A console with no guests around shows the matchmaking applet and the player backs out of it; am.cpp ends that applet (0x1000024 /
// 0x1030024) as cancelled, the SDK returns 0x3D1F and the game (main 0x7101133704) goes back quietly, without the error applet, and
// tears the session down (nn::streamplay::Finalize). Before that applet every IUserService command on the way must succeed: a failure
// in cmd 502 / 503 / 505 / 506 aborts the SDK, and a failure in Initialize makes the game show it through the error applet. Command
// ids are the IUserService proxy's (vtable 0x7500dd8d20, thunks 0x75006b7708..0x75006b79a8; slot +32 is cmd 1, +40 cmd 2 ...), the
// layouts come from the CMIF writers they branch to:
//   2   Initialize(u8 StreamPlayType, u8 StreamingMode, u32 guest count, pid) (0x75006b7ae0): kept for 502 / 503. No output.
//   3   Finalize, 5 CloseSession: accepted.
//   7   GetConnectionStateChangedEvent -> copy handle (Initialize attaches it): an event that never fires (no guest ever connects).
//   8   GetConnectionState -> 8 bytes (u8 flags, bit 0 connected; u32 result at 4): all zero, not connected.
//   25  SetSupportedGuestNpadStyleSet(u32), 27 SetGuestNpadJoyHoldType(u32): stored nowhere (no guest pads), accepted.
//   502 -> u8, 503 -> u8 (0x75001cb5e8): OpenSessionWithMatchmakeSupport passes them on as the StreamPlayType and StreamingMode
//       (CreateWorkBufferInProcessGlobal(type, mode), the same pair GetRequiredMemorySize takes; 502 also picks the applet id), so
//       they answer what Initialize was given. Type 1 skips the in-process work buffers (505 / 506).
//   504 (32-byte MatchmakeConfig) -> 0x80 bytes (0x75006b8990): the applet's argument, opaque to the SDK: zeros.
//   505 (copy handle: the work buffer's transfer memory, u64 size), 506 (u32 guest, 3 copy handles, 3 u64 sizes): accepted; the
//       handles stay the client's (it closes them).
//   507, 509 (no data), 510 (16 bytes: the audio output volumes Initialize reads): accepted.
//   9   GetWorkBufferSizes(u8 type, u8 mode) -> 4 x u64 {global, three per-guest parts} (0x75006b7d0c): type 0 (in-process) carves
//       its transfer memories out of the game's buffer with these sizes (CreateWorkBufferInProcessGlobal / PerGuest). Real sizes are
//       not known; one page each keeps everything page aligned and the game's buffer small (nothing is ever streamed). Command 1
//       answers to match: type 0 = global + guests x 3 pages, type 1 = guests x 1 page (CalculateRequiredWorkBufferSizePerInterface
//       asks cmd 1 for one guest of type 1).
// Every other command (StartStreaming's 4 / 500 / 501, the guest queries) is not implemented: it fails and is logged, so a GameShare
// path that gets past the matchmaking applet shows up in the log instead of running on zeros.
constexpr uint64_t kStreamplayHidSharedMemorySize = 0x84000;   // the extent of the format above (page aligned)

constexpr uint64_t kStreamplayPage = 0x1000;   // every work buffer part (see cmd 9 above)

uint32_t streamplay_required_memory(uint8_t type, uint32_t guests) {
    if (guests > 3) guests = 3;   // the SDK never asks for more (CalculateGuestCountFromMemorySize caps at 3)
    return static_cast<uint32_t>(type == 1 ? guests * kStreamplayPage : kStreamplayPage + guests * 3 * kStreamplayPage);
}

// What Initialize was given (one GameShare session per process).
struct StreamplayState {
    std::mutex mu;
    uint8_t type = 0;
    uint8_t mode = 0;
    ServiceEvent connection_changed{false};
};
StreamplayState& streamplay_state() {
    static StreamplayState s;
    return s;
}

Handler streamplay_ok(const char* name) {
    return [name](Request&, Response&) -> uint32_t {
        NXLOG_I("streamplay", "%s: accepted (no GameShare session here)", name);
        return 0;
    };
}

std::shared_ptr<ipc::Service> make_streamplay_user_service() {
    return make_iface("nn::streamplay::detail::IUserService", [](Service& s) {
        s.add_cmd(1, "GetRequiredMemorySize", [](Request& req, Response& rsp) -> uint32_t {
            rsp.push<uint32_t>(streamplay_required_memory(req.arg<uint8_t>(0), req.arg<uint32_t>(4)));
            return 0;
        });
        s.add_cmd(9, "GetWorkBufferSizes", [](Request&, Response& rsp) -> uint32_t {
            for (int i = 0; i < 4; ++i) rsp.push<uint64_t>(kStreamplayPage);
            return 0;
        });
        s.add_cmd(300, "GetHidSharedMemory", [](Request&, Response& rsp) -> uint32_t {
            services::ServiceSharedMemory sm(kStreamplayHidSharedMemorySize, "streamplay.hid");
            if (!sm.ok()) return kNotSupported;
            const uint32_t h = sm.new_handle();
            if (!h) return kNotSupported;
            rsp.push<uint64_t>(kStreamplayHidSharedMemorySize);
            rsp.push<uint64_t>(0);   // the format starts at the beginning of the block
            rsp.out_copy.push_back(h);
            return 0;
        });
        s.add_cmd(2, "Initialize", [](Request& req, Response&) -> uint32_t {
            StreamplayState& st = streamplay_state();
            std::lock_guard<std::mutex> lk(st.mu);
            st.type = req.arg<uint8_t>(0);
            st.mode = req.arg<uint8_t>(1);
            NXLOG_I("streamplay", "Initialize(type %u, mode %u, %u guests): accepted, no guest will connect", st.type, st.mode,
                    req.arg<uint32_t>(4));
            return 0;
        });
        s.add_cmd(3, "Finalize", streamplay_ok("Finalize"));
        s.add_cmd(5, "CloseSession", streamplay_ok("CloseSession"));
        s.add_cmd(7, "GetConnectionStateChangedEvent", [](Request&, Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(streamplay_state().connection_changed.new_handle());
            return 0;
        });
        s.add_cmd(8, "GetConnectionState", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<uint64_t>(0);   // not connected, result 0
            return 0;
        });
        s.add_cmd(25, "SetSupportedGuestNpadStyleSet", streamplay_ok("SetSupportedGuestNpadStyleSet"));
        s.add_cmd(27, "SetGuestNpadJoyHoldType", streamplay_ok("SetGuestNpadJoyHoldType"));
        s.add_cmd(502, "GetStreamPlayType", [](Request&, Response& rsp) -> uint32_t {
            StreamplayState& st = streamplay_state();
            std::lock_guard<std::mutex> lk(st.mu);
            rsp.push<uint8_t>(st.type);
            return 0;
        });
        s.add_cmd(503, "GetStreamingMode", [](Request&, Response& rsp) -> uint32_t {
            StreamplayState& st = streamplay_state();
            std::lock_guard<std::mutex> lk(st.mu);
            rsp.push<uint8_t>(st.mode);
            return 0;
        });
        s.add_cmd(504, "PrepareMatchmake", [](Request&, Response& rsp) -> uint32_t {
            NXLOG_I("streamplay", "matchmaking requested: the matchmaking applet comes next (it ends as cancelled)");
            rsp.out.insert(rsp.out.end(), 0x80, uint8_t{0});
            return 0;
        });
        s.add_cmd(505, "SetWorkBuffer", streamplay_ok("SetWorkBuffer (cmd 505)"));
        s.add_cmd(506, "SetGuestWorkBuffer", streamplay_ok("SetGuestWorkBuffer (cmd 506)"));
        s.add_cmd(507, "Cmd507", streamplay_ok("cmd 507"));
        s.add_cmd(509, "Cmd509", streamplay_ok("cmd 509"));
        s.add_cmd(510, "SetAudioOutputVolumes", streamplay_ok("SetAudioOutputVolumes (cmd 510)"));
    });
}

}  // namespace

std::shared_ptr<ipc::Service> make_streamplay_user() {
    auto s = std::make_shared<ipc::Service>("stpl:u");
    s->add_cmd(0, "CreateUserService", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push_object(make_streamplay_user_service());
        return 0;
    });
    return s;
}

}  // namespace nxrt::services::miscsvc

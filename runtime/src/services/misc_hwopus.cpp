// SPDX-License-Identifier: GPL-3.0-or-later
// hwopus: Opus decoders that "decode" to silence (see misc.cpp).
#include "misc_internal.h"

namespace nxrt::services::miscsvc {

namespace {

// ---- hwopus -----------------------------------------------------------------------------------------------------------------------
// No Opus decoder on this host yet: the decoders open and "decode" to silence of the right length, so titles that need one (Mario Kart
// World opens one at boot and aborts on a refusal) run with their Opus voices muted. The length comes from the packet's TOC byte
// (RFC 6716 section 3.1). Input: Nintendo's 8-byte header (u32 big-endian payload size, u32 final range) then the Opus packet.

// Samples per channel at `rate` of one Opus packet (0 when the header or TOC is unusable).
uint32_t opus_packet_samples(const uint8_t* p, size_t n, uint32_t rate) {
    if (n < 1) return 0;
    const uint32_t config = p[0] >> 3;
    // frame duration in units of 1/400 s (2.5 ms)
    static const uint32_t kSilk[4] = {4, 8, 16, 24}, kHybrid[2] = {4, 8}, kCelt[4] = {1, 2, 4, 8};
    const uint32_t q = config < 12 ? kSilk[config & 3] : config < 16 ? kHybrid[config & 1] : kCelt[config & 3];
    uint32_t frames = 1;
    switch (p[0] & 3) {
        case 0: frames = 1; break;
        case 1: case 2: frames = 2; break;
        default: if (n < 2) return 0; frames = p[1] & 0x3f; break;
    }
    return frames * q * rate / 400;
}

std::shared_ptr<Service> make_hwopus_decoder(uint32_t rate, uint32_t channels) {
    if (rate == 0) rate = 48000;
    if (channels == 0 || channels > 8) channels = 2;
    return make_iface("IHardwareOpusDecoder", [rate, channels](Service& s) {
        // Decode*: consumed bytes, samples per channel [, performance time]; the PCM (s16 interleaved) is zero.
        auto decode = [rate, channels](bool perf) -> Handler {
            return [rate, channels, perf](Request& req, Response& rsp) -> uint32_t {
                const ipc::Buffer in = req.in_buf(0), out = req.out_buf(0);
                const uint8_t* src = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(in.addr));
                uint32_t consumed = 0, samples = 0;
                if (src && in.size >= 8) {
                    const uint32_t size = (uint32_t(src[0]) << 24) | (uint32_t(src[1]) << 16) | (uint32_t(src[2]) << 8) | src[3];
                    if (size <= in.size - 8) {
                        consumed = 8 + size;
                        samples = opus_packet_samples(src + 8, size, rate);
                    }
                }
                if (out.addr && out.size) {
                    const uint64_t bytes = std::min<uint64_t>(out.size, uint64_t(samples) * channels * 2);
                    samples = static_cast<uint32_t>(bytes / (channels * 2));
                    std::memset(reinterpret_cast<void*>(static_cast<uintptr_t>(out.addr)), 0, static_cast<size_t>(out.size));
                }
                rsp.push<uint32_t>(consumed);
                rsp.push<uint32_t>(samples);
                if (perf) rsp.push<uint64_t>(0);
                return 0;
            };
        };
        s.add_cmd(0, "DecodeInterleavedOld", decode(false));
        s.add_cmd(1, "SetContext", [](Request&, Response&) -> uint32_t { return 0; });
        s.add_cmd(2, "DecodeInterleavedForMultiStreamOld", decode(false));
        s.add_cmd(3, "SetContextForMultiStream", [](Request&, Response&) -> uint32_t { return 0; });
        s.add_cmd(4, "DecodeInterleavedWithPerfOld", decode(true));
        s.add_cmd(5, "DecodeInterleavedForMultiStreamWithPerfOld", decode(true));
        s.add_cmd(6, "DecodeInterleavedWithPerfAndResetOld", decode(true));
        s.add_cmd(7, "DecodeInterleavedForMultiStreamWithPerfAndResetOld", decode(true));
        s.add_cmd(8, "DecodeInterleaved", decode(true));
        s.add_cmd(9, "DecodeInterleavedForMultiStream", decode(true));
    });
}

}  // namespace

std::shared_ptr<Service> make_hwopus() {
    return make_iface("IHardwareOpusDecoderManager", [](Service& s) {
        // Open*(params..., work buffer size, transfer memory): a silent decoder. Raw arguments of the old forms are
        // {s32 sample rate, s32 channels, u32 work size}; the Ex forms pass OpusParametersEx {sample rate, channels, ...} in a buffer.
        auto open = [](bool ex) -> Handler {
            return [ex](Request& req, Response& rsp) -> uint32_t {
                uint32_t rate = 0, ch = 0;
                if (ex) {
                    const ipc::Buffer b = req.in_buf(0);
                    if (b.addr && b.size >= 8) {
                        std::memcpy(&rate, reinterpret_cast<const void*>(static_cast<uintptr_t>(b.addr)), 4);
                        std::memcpy(&ch, reinterpret_cast<const void*>(static_cast<uintptr_t>(b.addr + 4)), 4);
                    }
                } else {
                    rate = req.arg<uint32_t>(0);
                    ch = req.arg<uint32_t>(4);
                }
                static std::atomic<int> logs{0};
                if (logs.fetch_add(1) < 4)
                    NXLOG_I("hwopus", "Opus decoder opened (cmd %u, %u Hz, %u channels): no Opus decoder here, it decodes silence", req.cmd, rate, ch);
                rsp.push_object(make_hwopus_decoder(rate, ch));
                return 0;
            };
        };
        s.add_cmd(0, "OpenHardwareOpusDecoder", open(false));
        s.add_cmd(2, "OpenHardwareOpusDecoderForMultiStream", refuse(kNotSupported, "hwopus", "OpenHardwareOpusDecoderForMultiStream"));
        s.add_cmd(4, "OpenHardwareOpusDecoderEx", open(true));
        s.add_cmd(6, "OpenHardwareOpusDecoderForMultiStreamEx", refuse(kNotSupported, "hwopus", "OpenHardwareOpusDecoderForMultiStreamEx"));
        const Handler size = [](Request& req, Response& rsp) -> uint32_t {
            NXLOG_I("hwopus", "game asked for the Opus work buffer size (cmd %u); reporting %u", req.cmd, kOpusWorkBufferSize);
            rsp.push<uint32_t>(kOpusWorkBufferSize);
            return 0;
        };
        s.add_cmd(1, "GetWorkBufferSize", size);
        s.add_cmd(3, "GetWorkBufferSizeForMultiStream", size);
        s.add_cmd(5, "GetWorkBufferSizeEx", size);
        s.add_cmd(7, "GetWorkBufferSizeForMultiStreamEx", size);
        s.add_cmd(8, "GetWorkBufferSizeExEx", size);
        s.add_cmd(9, "GetWorkBufferSizeForMultiStreamExEx", size);
    });
}

}  // namespace nxrt::services::miscsvc

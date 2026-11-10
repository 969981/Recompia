// SPDX-License-Identifier: GPL-3.0-or-later
// Shared helpers of the small services: the accept/refuse command handlers and the events misc_reset() clears (see misc.cpp).
#include "misc_internal.h"

#include <cstring>

namespace nxrt::services::miscsvc {

void zero_buffer(const ipc::Buffer& b) {
    if (b.addr && b.size) std::memset(buf_mut(b), 0, static_cast<size_t>(b.size));
}

// A command that is accepted and ignored (logged at Debug).
Handler accept(const char* tag, const char* what) {
    return [tag, what](Request& req, Response&) -> uint32_t {
        NXLOG_D(tag, "%s (cmd %u) accepted and ignored", what, req.cmd);
        return 0;
    };
}

// A command that answers with an error result (logged at Warn).
Handler refuse(uint32_t rc, const char* tag, const char* what) {
    return [rc, tag, what](Request& req, Response&) -> uint32_t {
        NXLOG_W(tag, "%s (cmd %u) answered with 0x%X", what, req.cmd, rc);
        return rc;
    };
}

MiscEvents& events() {
    static MiscEvents e;
    return e;
}

}  // namespace nxrt::services::miscsvc

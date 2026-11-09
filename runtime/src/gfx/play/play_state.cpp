// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode: the shared state, see play_state.h.
#include "play_state.h"

namespace nxrt::play {
using namespace detail;
namespace detail {

G& g() {
    static G inst;
    return inst;
}

}  // namespace detail
}  // namespace nxrt::play

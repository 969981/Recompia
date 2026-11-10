// SPDX-License-Identifier: GPL-3.0-or-later
// Where the Switch 2 platform pieces (core/switch2.cpp) hand what the guest declares to the layer that can use it. A sink left unset is a
// no-op, so the core builds and runs without the graphics layer; the graphics layer registers at start-up and may leave it set.
#pragma once

namespace nxrt {

// nn::vi::SetLayerNext{Extended,Standard}ContentLuminanceRangeMetadata: the picture's luminance range in nits (extended = HDR range).
using HdrLuminanceSink = void (*)(bool extended, unsigned min_nits, unsigned max_nits);
void set_hdr_luminance_sink(HdrLuminanceSink fn);

}  // namespace nxrt

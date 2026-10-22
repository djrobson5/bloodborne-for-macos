// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the main thread's water interaction setup (CUSA03173 1.09) records clears into the
// render thread's command buffer while that thread may still be building a frame; this makes it
// wait for the frame first (see bbport_gx_handoff.cpp).
#pragma once

#include <cstddef>
#include <cstdint>

namespace BbGxHandoff {
/// Hooks the water interaction setup if the engine's bytes match the 1.09 image (the loader calls
/// it once, before any game code runs). BB_GX_HANDOFF=0: the hook only reports, never waits.
void Install(std::uint8_t* image, std::size_t size);
} // namespace BbGxHandoff

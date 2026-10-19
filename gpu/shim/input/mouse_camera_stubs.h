// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from bbhost db5457cbd447f4ef129b58df0a78e0a8819c7033.
#pragma once

// The asm stubs of the mouse camera's hooks on the game's follow camera
// (engine/mouse_camera_stubs.cpp).

#include <cstdint>

// The assembly names these symbols as they are spelled here; Mach-O would otherwise prefix the C
// names with an underscore (BB_ASM_NAME pins the plain name on every platform).
#define BB_ASM_NAME(name) __asm__(#name)

extern "C" {
// What the store stub calls - thunk_wrap(free_camera_hook) in the game, a
// test's own function in the kit - as (cam, &the update's input byte,
// stick pitch, stick yaw), and where both stubs go on: the guest's 0x183ce67.
extern void* g_mouse_camera_host BB_ASM_NAME(g_mouse_camera_host);
extern std::uint64_t g_mouse_camera_resume BB_ASM_NAME(g_mouse_camera_resume);
// At the free camera's store, 0x183ce54 (19 bytes, a jmp [rip+0] there): the
// two stores it displaced, then the host, then 0x183ce67.
void bb_mouse_camera_stub() BB_ASM_NAME(bb_mouse_camera_stub);
// At each of the fast turn's exits, 0x183f9bc and 0x183fabf (14 bytes): the
// pitch store it displaced, the free camera noted as run, then 0x183ce67.
void bb_mouse_camera_ramp_stub() BB_ASM_NAME(bb_mouse_camera_ramp_stub);
}

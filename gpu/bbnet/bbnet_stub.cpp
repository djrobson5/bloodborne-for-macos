// SPDX-License-Identifier: GPL-2.0-or-later
// Online play left out (BB_ONLINE=OFF, the macOS default): the entry points bbgpu calls.
#include <cstdint>
#include <cstdio>

extern "C" void bbnet_initialize(void) {
    std::puts("Online: not built into this version (BB_ONLINE=OFF); playing offline");
}
extern "C" void bbnet_register(void) {}
extern "C" void bbnet_set_image(unsigned char*, std::uint64_t) {}

// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: an intermittent crash while an area loads ("Unimplemented PM4 type 0", a SIGSEGV in
// ParseFetchShader, an HLE copy assert; ~1 boot in 11) was a race between two guest threads on
// one command buffer.
//
// The render thread (GXRenderThread) and the main thread hand frames over with three events on
// the render thread object (sub_21b6620: +0x28 kick, +0x48, +0x68 frame done; each a mutex, a
// condition variable and flags at +0x18: 1 initialized, 2 signaled). The render thread builds a
// frame into its immediate command buffer, sets "frame done" and waits for "kick"; the main
// thread waits for "frame done", resets it and sets "kick".
//
// While an area loads, the main thread constructs the water interaction objects (sub_ebb960,
// strings WaterInteraction / GXWaterInteractionHeightMap) after it has kicked the render thread:
// it creates three render targets and clears each through sub_2168630 - a compute dispatch
// (setCsShader with shader 0x74d1b005..., the packets of every crash dump) recorded into the
// render thread's immediate command buffer ([[[0x55406c8] + 0x10] + 8] + 0x10). Nothing orders
// that against the frame the render thread is building: both threads advance the same write
// pointer and their packets interleave, which the GPU then decodes as garbage. Here the render
// thread spends ~8 ms after a kick before it writes its packets (in a burst under 1 ms; what it
// waits on is not identified), so the main thread's clears, ~5-17 ms after the kick, landed in
// that burst in about 1 boot in 9. Presumably the console's timing keeps them apart (unverified).
//
// The hook makes the water interaction setup wait until "frame done" is set: the render thread
// is then waiting for the next kick, which only the main thread gives (and it is busy here). It
// polls the flag rather than taking the event's mutex; only the main thread clears it.
#include "bbport_gx_handoff.h"
#include "engine/engine_hooks.h"
#include "bbport_platform.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace BbGxHandoff {
namespace {
using u8 = std::uint8_t;
using u64 = std::uint64_t;

constexpr std::size_t kWaterInit = 0xebb960;
// push rbp; mov rbp, rsp; push r15..r12; push rbx; sub rsp, 0x58
constexpr u8 kWaterInitPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                     0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x58};
// In sub_2194580: mov [r12 + 0x348], rcx - the render thread object on the graphics system,
// whose pointer it stores at 0x55406c8 (lea rax, [rip + ...]; mov [rax], r12 at 0x21945d7).
constexpr std::size_t kStoreThread = 0x21949ec;
constexpr u8 kStoreThreadBytes[] = {0x49, 0x89, 0x8c, 0x24, 0x48, 0x03, 0x00, 0x00};
constexpr std::size_t kGxSystem = 0x55406c8;
constexpr std::size_t kRenderThreadOffset = 0x348;
constexpr std::size_t kRenderThreadVtable = 0x535fbc0;
constexpr std::size_t kEventVtable = 0x52b9790 + 0x10;
constexpr std::size_t kFrameDone = 0x68, kEventFlags = 0x18;
constexpr std::uint32_t kInitialized = 1, kSignaled = 2;
constexpr auto kLimit = std::chrono::milliseconds(250);

u8* g_image = nullptr;
bool g_wait = true;
std::atomic<u64> g_calls{0}, g_waited{0}, g_timeouts{0}, g_unknown{0};

template <typename T> bool Read(u64 address, T& out) {
    return address && BbPlatform::ReadProcessMemory(reinterpret_cast<const void*>(address), &out,
                                                   sizeof(out));
}

/// The render thread's "frame done" flags, or null when the objects do not look as expected.
const std::uint32_t* FrameDoneFlags() {
    const u64 image = reinterpret_cast<u64>(g_image);
    u64 system = 0, thread = 0, vtable = 0, event_vtable = 0;
    std::uint32_t flags = 0;
    if (!Read(image + kGxSystem, system) || !Read(system + kRenderThreadOffset, thread) ||
        !Read(thread, vtable) || vtable != image + kRenderThreadVtable ||
        !Read(thread + kFrameDone, event_vtable) || event_vtable != image + kEventVtable ||
        !Read(thread + kFrameDone + kEventFlags, flags) || !(flags & kInitialized)) {
        return nullptr;
    }
    return reinterpret_cast<const std::uint32_t*>(thread + kFrameDone + kEventFlags);
}

__attribute__((sysv_abi)) std::int64_t WaterInit(std::uint64_t, const std::uint64_t*) {
    const u64 call = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    const std::uint32_t* flags = FrameDoneFlags();
    if (!flags) {
        if (g_unknown.fetch_add(1, std::memory_order_relaxed) == 0) {
            std::printf("GX hand-off: render thread object not found, water setup not ordered\n");
        }
        return 0;
    }
    if (__atomic_load_n(flags, __ATOMIC_ACQUIRE) & kSignaled) {
        std::printf("GX hand-off: water setup %llu, render thread idle\n", (unsigned long long)call);
        return 0; // run the setup
    }
    const auto start = std::chrono::steady_clock::now();
    bool done = false;
    while (g_wait && std::chrono::steady_clock::now() - start < kLimit) {
        if (__atomic_load_n(flags, __ATOMIC_ACQUIRE) & kSignaled) {
            done = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (!g_wait) {
        std::printf("GX hand-off: water setup %llu during a render thread frame (not waiting: "
                    "BB_GX_HANDOFF=0)\n",
                    (unsigned long long)call);
    } else if (done) {
        g_waited.fetch_add(1, std::memory_order_relaxed);
        std::printf("GX hand-off: water setup %llu waited %.3f ms for the render thread's frame\n",
                    (unsigned long long)call, ms);
    } else {
        g_timeouts.fetch_add(1, std::memory_order_relaxed);
        std::printf("GX hand-off: water setup %llu gave up after %.0f ms (render thread frame not "
                    "done)\n",
                    (unsigned long long)call, ms);
    }
    std::fflush(stdout);
    return 0;
}
} // namespace

void Install(std::uint8_t* image, std::size_t size) {
    const char* env = std::getenv("BB_GX_HANDOFF");
    g_wait = !(env && env[0] == '0');
    if (size < kGxSystem + 8 || size < kStoreThread + sizeof(kStoreThreadBytes) ||
        std::memcmp(image + kStoreThread, kStoreThreadBytes, sizeof(kStoreThreadBytes)) != 0) {
        std::printf("GX hand-off: engine bytes differ, water setup not ordered\n");
        return;
    }
    BbEngine::Hook hook;
    if (!BbEngine::Prepare(hook, image, size, kWaterInit, kWaterInitPrologue, WaterInit)) {
        std::printf("GX hand-off: engine bytes differ, water setup not ordered\n");
        return;
    }
    g_image = image;
    BbEngine::Commit({&hook, 1});
}
} // namespace BbGxHandoff

// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: helper thread sizing. Counts follow the hardware threads this process may run on
// (the affinity mask, so `taskset` can emulate a Steam Deck), and speculative helpers run as
// SCHED_IDLE: they use cores the game leaves idle and never take time from its threads.

#pragma once

#include <algorithm>
#include <thread>
#ifdef __APPLE__
#include <pthread.h>
#include <pthread/qos.h>
#elif !defined(_WIN32)
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace BbThreads {

#ifdef _WIN32
// Windows (bbport_platform.cpp): the process affinity mask; THREAD_PRIORITY_IDLE.
unsigned Available();
void MakeBackground();
/// The serial render chain (GPU command thread, draw recording, Vulkan recording) runs above
/// the game's own threads (THREAD_PRIORITY_ABOVE_NORMAL; BB_GPU_THREAD_PRIORITY=0: normal).
/// `bulk`: threads doing large copies (texture uploads while an area streams in) are raised
/// only with 16 or more hardware threads; with fewer they took the cores the game's own
/// threads needed exactly then.
void MakeCritical(bool bulk = false);
/// An idle spin of a raised thread before it sleeps, in microseconds: `wanted`, at most 50
/// with fewer than 16 hardware threads (a raised thread spinning keeps a game thread waiting).
unsigned CapSpinUs(unsigned wanted);
#elif defined(__APPLE__)
/// macOS: quality-of-service classes. The render chain asks for performance cores
/// (BB_GPU_THREAD_PRIORITY=0: left at the default); speculative helpers run as utility work.
inline unsigned Available() { return std::max(1u, std::thread::hardware_concurrency()); }
inline void MakeBackground() { pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0); }
void MakeCritical(bool bulk = false);
inline unsigned CapSpinUs(unsigned wanted) { return wanted; }
#else
/// Linux: unchanged (normal priority; raising it needs privileges).
inline void MakeCritical(bool = false) {}
inline unsigned CapSpinUs(unsigned wanted) { return wanted; }
/// Hardware threads available to the process.
inline unsigned Available() {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        return std::max(1, CPU_COUNT(&set));
    }
    return std::max(1u, std::thread::hardware_concurrency());
}

/// The calling thread only runs on otherwise idle cores (falls back to the lowest nice level).
inline void MakeBackground() {
    sched_param param{};
    if (sched_setscheduler(0, SCHED_IDLE, &param) != 0) {
        setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), 19);
    }
}
#endif

} // namespace BbThreads

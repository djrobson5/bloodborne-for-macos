// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: Windows and macOS implementations of bbport_platform.h and bbport_threads.h.

#ifdef _WIN32
#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <windows.h>
#include <psapi.h>
#include "bbport_platform.h"
#include "bbport_threads.h"

namespace BbPlatform {

static std::uint64_t FileTimeUs(const FILETIME& t) {
    return ((std::uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime) / 10;
}

bool GetUsage(bool thread, Usage& out) {
    FILETIME creation, exit, kernel, user;
    const BOOL ok = thread ? GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user)
                           : GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user);
    if (!ok) {
        return false;
    }
    out = {};
    out.user_us = FileTimeUs(user);
    out.sys_us = FileTimeUs(kernel);
    if (!thread) {
        PROCESS_MEMORY_COUNTERS counters{};
        if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
            out.minor_faults = counters.PageFaultCount;
        }
    }
    return true;
}

bool GetMemory(Memory& out) {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
                              reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                              sizeof(counters)) ||
        !GlobalMemoryStatusEx(&status)) {
        return false;
    }
    out.private_bytes = counters.PrivateUsage;
    out.working_set = counters.WorkingSetSize;
    out.peak_working_set = counters.PeakWorkingSetSize;
    out.free_phys = status.ullAvailPhys;
    out.total_phys = status.ullTotalPhys;
    out.commit_left = status.ullAvailPageFile;
    return true;
}

std::uint32_t CurrentThreadId() {
    return static_cast<std::uint32_t>(GetCurrentThreadId());
}

int ThreadCpuClock() {
    HANDLE handle = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle,
                         THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0)) {
        return -1;
    }
    return static_cast<int>(reinterpret_cast<std::intptr_t>(handle));
}

std::uint64_t ReadThreadCpuClockNs(int clock) {
    FILETIME creation, exit, kernel, user;
    const HANDLE handle = reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(clock));
    if (!GetThreadTimes(handle, &creation, &exit, &kernel, &user)) {
        return 0;
    }
    return (FileTimeUs(kernel) + FileTimeUs(user)) * 1000;
}

void DescribeAddress(const void* address, char* out, std::size_t size) {
    HMODULE module = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(address), &module) &&
        GetModuleFileNameA(module, name, sizeof(name))) {
        std::snprintf(out, size, "%s+0x%llx", name,
                      static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(address) -
                                                      reinterpret_cast<std::uintptr_t>(module)));
        return;
    }
    std::snprintf(out, size, "%p", address);
}

std::size_t CaptureStack(void** frames, std::size_t count) {
    return CaptureStackBackTrace(0, static_cast<DWORD>(count), frames, nullptr);
}

bool ReadProcessMemory(const void* source, void* destination, std::size_t size) {
    SIZE_T read = 0;
    return ::ReadProcessMemory(GetCurrentProcess(), source, destination, size, &read) && read == size;
}

} // namespace BbPlatform

namespace BbThreads {

unsigned Available() {
    DWORD_PTR process = 0, system = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system) && process) {
        return std::max(1, std::popcount(static_cast<unsigned long long>(process)));
    }
    return std::max(1u, std::thread::hardware_concurrency());
}

void MakeBackground() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);
}

// The game runs dozens of threads at normal priority (cloth, character, Havok, job pools); on
// 12 or fewer hardware threads Windows gives a ready thread of equal priority a core only when
// another one's quantum ends (tens of ms), so a render-chain thread queued behind them delays
// the frame. Its idle spins are bounded (BB_GPU_SPIN_US, BB_PIPE_SPIN_US, recording: 200 us)
// before it sleeps, so the higher priority takes no lasting time from the game's threads.
namespace {
bool Raising() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_GPU_THREAD_PRIORITY");
        return !(env && env[0] == '0');
    }();
    return enabled;
}
} // namespace

void MakeCritical(bool bulk) {
    if (Raising() && (!bulk || Available() >= 16)) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    }
}

unsigned CapSpinUs(unsigned wanted) {
    return Raising() && Available() < 16 ? std::min(wanted, 50u) : wanted;
}

} // namespace BbThreads
#endif

#ifdef __APPLE__
// macOS (x86-64 under Rosetta 2): Mach thread and task information.
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <dlfcn.h>
#include <execinfo.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include "bbport_platform.h"
#include "bbport_threads.h"

namespace BbPlatform {

static std::uint64_t MachTimeUs(const time_value_t& t) {
    return std::uint64_t(t.seconds) * 1000000 + std::uint64_t(t.microseconds);
}

bool GetUsage(bool thread, Usage& out) {
    if (thread) {
        thread_basic_info_data_t info{};
        mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
        const mach_port_t self = pthread_mach_thread_np(pthread_self());
        if (thread_info(self, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &count) != KERN_SUCCESS) {
            return false;
        }
        out.user_us = MachTimeUs(info.user_time);
        out.sys_us = MachTimeUs(info.system_time);
        return true; // no per-thread switch or fault counters
    }
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return false;
    }
    out.user_us = std::uint64_t(usage.ru_utime.tv_sec) * 1000000 + usage.ru_utime.tv_usec;
    out.sys_us = std::uint64_t(usage.ru_stime.tv_sec) * 1000000 + usage.ru_stime.tv_usec;
    out.invol_switches = usage.ru_nivcsw;
    out.vol_switches = usage.ru_nvcsw;
    out.minor_faults = usage.ru_minflt;
    return true;
}

bool GetMemory(Memory& out) {
    task_vm_info_data_t task{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&task), &count) != KERN_SUCCESS) {
        return false;
    }
    out.private_bytes = task.phys_footprint;
    out.working_set = task.resident_size;
    out.peak_working_set = task.resident_size_peak;
    std::uint64_t total = 0;
    std::size_t size = sizeof(total);
    sysctlbyname("hw.memsize", &total, &size, nullptr, 0);
    out.total_phys = total;
    vm_statistics64_data_t vm{};
    count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) == KERN_SUCCESS) {
        out.free_phys = (std::uint64_t(vm.free_count) + vm.inactive_count) * vm_page_size;
    }
    out.commit_left = out.free_phys;
    return true;
}

std::uint32_t CurrentThreadId() {
    std::uint64_t id = 0;
    pthread_threadid_np(nullptr, &id);
    return static_cast<std::uint32_t>(id);
}

int ThreadCpuClock() {
    return static_cast<int>(pthread_mach_thread_np(pthread_self()));
}

std::uint64_t ReadThreadCpuClockNs(int clock) {
    thread_basic_info_data_t info{};
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    if (clock < 0 || thread_info(static_cast<mach_port_t>(clock), THREAD_BASIC_INFO,
                                 reinterpret_cast<thread_info_t>(&info), &count) != KERN_SUCCESS) {
        return 0;
    }
    return (MachTimeUs(info.user_time) + MachTimeUs(info.system_time)) * 1000;
}

void DescribeAddress(const void* address, char* out, std::size_t size) {
    Dl_info info{};
    if (dladdr(address, &info) && info.dli_fname) {
        const char* name = std::strrchr(info.dli_fname, '/');
        std::snprintf(out, size, "%s+0x%llx", name ? name + 1 : info.dli_fname,
                      static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(address) -
                                                      reinterpret_cast<std::uintptr_t>(info.dli_fbase)));
    } else {
        std::snprintf(out, size, "%p", address);
    }
}

std::size_t CaptureStack(void** frames, std::size_t count) {
    return static_cast<std::size_t>(backtrace(frames, static_cast<int>(count)));
}

bool ReadProcessMemory(const void* source, void* destination, std::size_t size) {
    mach_vm_size_t got = 0;
    return mach_vm_read_overwrite(mach_task_self(), reinterpret_cast<mach_vm_address_t>(source), size,
                                  reinterpret_cast<mach_vm_address_t>(destination), &got) == KERN_SUCCESS &&
           got == size;
}

} // namespace BbPlatform

namespace BbThreads {

void MakeCritical(bool bulk) {
    static const bool enabled = [] {
        const char* e = std::getenv("BB_GPU_THREAD_PRIORITY");
        return !(e && e[0] == '0');
    }();
    if (!enabled) {
        return;
    }
    // Bulk copy threads stay below the render chain: they took the cores the game needed.
    pthread_set_qos_class_self_np(bulk ? QOS_CLASS_USER_INITIATED : QOS_CLASS_USER_INTERACTIVE, 0);
}

} // namespace BbThreads
#endif

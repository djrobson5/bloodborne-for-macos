// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_barrier_tracker.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/types.h"

namespace VideoCore {
class BlitHelper;
} // namespace VideoCore

namespace Vulkan {

class GpuProfiler;
class SceneTargets;

class Instance;
class Scheduler;

class Runtime {
public:
    explicit Runtime(const Instance& instance, Scheduler& scheduler);
    ~Runtime() = default;

    Scheduler& GetScheduler() {
        return scheduler;
    }

    const Instance& GetInstance() const {
        return instance;
    }

    StagingBufferPool& GetStagingPool() {
        return staging_pool;
    }

    void TickFrame();
    SceneTargets* scene_targets = nullptr;

    void CopyBuffer(const VideoCore::Buffer* src, const VideoCore::Buffer* dst,
                    std::span<const vk::BufferCopy> copies);

    void FillBuffer(const VideoCore::Buffer* dst, u64 offset, u64 size, u32 value);

    void InlineData(VideoCore::Buffer* dst, u64 offset, u32 value);
    /// bbport BB_GUEST_IN_PLACE: copies from the game's memory itself (a guest memory chunk's
    /// buffer, host memory the CPU writes) into `dst`: uploads without a CPU copy.
    void CopyFromGuestChunk(vk::Buffer src, const VideoCore::Buffer* dst,
                            std::span<const vk::BufferCopy> copies);
    /// bbport: `data` written into `dst` by the GPU in stream order (vkCmdUpdateBuffer pieces).
    void UpdateBuffer(const VideoCore::Buffer* dst, u64 offset, std::span<const u8> data);

    /// bbport: a clear pending on the image (ClearImage, clear folding) is recorded first,
    /// unless `keep_pending_clear`: the render pass begin that will load the image with it.
    bool Transit(VideoCore::Image* image, vk::ImageLayout dst_layout,
                 vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access,
                 std::optional<VideoCore::SubresourceRange> subres_range = {},
                 bool keep_pending_clear = false);

    /// bbport (clear folding; BB_FOLD_CLEARS=0 or experiment bit 1<<29 turns it off): whole-image
    /// colour clears wait on the image
    /// (BackingImage::clear_pending) for the next render pass that draws to it, which clears
    /// with its load operation instead: no clear encoder writing the image and the pass
    /// loading it back on a tile-based GPU. Every other access records the clear first.
    static bool FoldClears();
    /// Records the pending clear of `image` now (before the caller's own barriers).
    void FlushPendingClear(VideoCore::Image* image);

    /// BB_GPU_PROFILE: a profiler segment for a transfer of `image` while it lives; the
    /// segment it interrupted continues afterwards.
    class TransferMark {
    public:
        TransferMark(Runtime& runtime, const char* what, const VideoCore::Image& image);
        ~TransferMark();
        TransferMark(const TransferMark&) = delete;
        TransferMark& operator=(const TransferMark&) = delete;

    private:
        GpuProfiler* profiler = nullptr;
        u64 resume = 0;
    };
    void UploadImage(VideoCore::Image* dst, const VideoCore::Buffer* src,
                     std::span<const vk::BufferImageCopy> upload_copies);
    void DownloadImage(VideoCore::Image* src, const VideoCore::Buffer* dst,
                       std::span<const vk::BufferImageCopy> download_copies);

    void CopyImage(VideoCore::Image* src, VideoCore::Image* dst);
    void CopyImageWithBuffer(VideoCore::Image* src, VideoCore::Image* dst,
                             const VideoCore::Buffer* buffer, u64 offset);
    void CopyMip(VideoCore::Image* src, VideoCore::Image* dst, u32 mip, u32 slice);

    void CopyColorAndDepth(VideoCore::Image* src, VideoCore::Image* dst);

    void CopyDepthStencil(VideoCore::Image* src, VideoCore::Image* dst,
                          const VideoCore::SubresourceRange& sub_range);

    void ResolveImage(VideoCore::Image* src, VideoCore::Image* dst,
                      const VideoCore::SubresourceRange& src_range,
                      const VideoCore::SubresourceRange& dst_range);
    void ClearImage(VideoCore::Image* dst, const VideoCore::SubresourceRange& range,
                    const vk::ClearValue& clear_value);

    void SetBackingSamples(VideoCore::Image* image, u32 num_samples, bool copy_backing = true);
    /// bbport t23: gives `image` a backing with its full usage in place of a lazy one
    /// (BackingImage::view_formats), with the contents copied. No-op for a full backing.
    void UpgradeBacking(VideoCore::Image* image);

    void AccessBuffer(const VideoCore::Buffer* handle, u64 offset, u64 size,
                      vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access);

    bool IsBufferAccessed(const VideoCore::Buffer* handle, u64 offset, u64 size,
                          bool check_read_access = false);

    /// bbport: IsBufferAccessed(dst, dst_min, dst_max - dst_min, true) for the destinations of
    /// many copies: reads the bounding range overlaps are checked against the regions themselves
    /// (a read between them is no write after a read).
    bool IsCopyDestinationAccessed(const VideoCore::Buffer* dst,
                                   std::span<const vk::BufferCopy> copies, u64 dst_min,
                                   u64 dst_max);

    /// bbport: Conflicts: the caller flushes for the conflicts IsBufferAccessed found. The
    /// barrier waits for the stages that wrote and those of the reads a write conflicted with;
    /// other reads stay pending for later writes (BB_PRECISE_BARRIERS). A barrier from all
    /// graphics stages makes MoltenVK's next vertex stage wait for the last fragment stage.
    enum class FlushScope { All, Conflicts };
    void FlushBarriers(FlushScope scope = FlushScope::All);

    /// bbport: runs before this thread changes image state (layouts, pending image
    /// barriers): the rasterizer joins its texture binding helper there (BindHelper).
    void SetImageAccessHook(void (*hook)(void*), void* context) {
        image_access_hook = hook;
        image_access_context = context;
    }
    void BeforeImageAccess() {
        if (image_access_hook) {
            image_access_hook(image_access_context);
        }
    }

private:
    void (*image_access_hook)(void*) = nullptr;
    void* image_access_context = nullptr;
    const Instance& instance;
    Scheduler& scheduler;
    std::unique_ptr<VideoCore::BlitHelper> blit_helper;
    StagingBufferPool staging_pool;
    BarrierTracker barrier_tracker;
    VideoCore::Image::Barriers image_barriers;
    vk::MemoryBarrier2 memory_barrier{};
    /// bbport: barrier_tracker holds the writes; the reads are tracked by the GPU work a barrier
    /// waits for (MoltenVK's fences: vertex, fragment, compute, transfer), so that a write after
    /// a read waits only for the stages of the reads it overwrites.
    static constexpr u32 NumReadClasses = 4;
    std::array<BarrierTracker, NumReadClasses> read_trackers;
    std::array<vk::PipelineStageFlags2, NumReadClasses> read_stages{};
    vk::PipelineStageFlags2 write_stages{};
    std::atomic<u32> read_conflicts{0}; ///< read classes IsBufferAccessed found writes after
    // bbport: ranges inserted into barrier_tracker since its last Clear(); re-inserting a
    // contained range is a no-op, and draws re-bind the same ranges constantly.
    struct AccessMemo {
        u64 resource;
        u64 start;
        u64 end;
        u32 epoch;
        u32 access;
    };
    std::array<AccessMemo, 512> access_memo{};
    u32 access_epoch = 1;
};

} // namespace Vulkan

// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <string>

/// bbport macOS: untracked Metal heaps for VkDeviceMemory (vk_instance.cpp, BB_UNTRACKED_HEAPS).
/// MoltenVK makes every heap it creates hazard-tracked, so Metal orders each render pass after
/// the previous one through the few large buffers that hold the game's memory: a pass's vertex
/// work never overlapped the previous pass's fragment work. Memory imported from an untracked
/// heap leaves that ordering to MoltenVK's barrier fences.
namespace Vulkan::MetalHeap {

/// A new untracked placement heap of at least `size` bytes on `mtl_device` (an id<MTLDevice>),
/// private or shared storage as MoltenVK picks for the memory type; retained, or null.
void* CreateUntracked(void* mtl_device, std::uint64_t size, bool host_visible);

/// Releases an object returned by CreateUntracked.
void Release(void* object);

/// Whether a buffer created in `heap` the way MoltenVK creates them (no tracking mode in its
/// options) comes out untracked.
bool PlacesUntracked(void* heap);

/// bbport t23 (BB_IMAGE_DIAG): storage mode, usage, GPU-optimized contents, compression and heap
/// of `texture` (an id<MTLTexture>), one line.
std::string DescribeTexture(void* texture);

} // namespace Vulkan::MetalHeap

// SPDX-License-Identifier: GPL-2.0-or-later

// Compiled without ARC (CMakeLists.txt): retains and releases are explicit.

#import <Metal/Metal.h>

#include "video_core/renderer_vulkan/vk_metal_heap.h"

namespace Vulkan::MetalHeap {

void* CreateUntracked(void* mtl_device, std::uint64_t size, bool host_visible) {
    MTLHeapDescriptor* desc = [MTLHeapDescriptor new];
    // As MoltenVK's own heaps (MVKDeviceMemory::ensureMTLHeap), but untracked.
    desc.type = MTLHeapTypePlacement;
    desc.storageMode = host_visible ? MTLStorageModeShared : MTLStorageModePrivate;
    desc.cpuCacheMode = MTLCPUCacheModeDefaultCache;
    desc.hazardTrackingMode = MTLHazardTrackingModeUntracked;
    desc.size = size;
    id<MTLHeap> heap = [(id<MTLDevice>)mtl_device newHeapWithDescriptor:desc];
    [desc release];
    return heap;
}

void Release(void* object) {
    [(id)object release];
}

bool PlacesUntracked(void* heap) {
    const id<MTLHeap> mtl_heap = (id<MTLHeap>)heap;
    const MTLResourceOptions options = (MTLResourceOptions)mtl_heap.storageMode
                                           << MTLResourceStorageModeShift |
                                       (MTLResourceOptions)mtl_heap.cpuCacheMode
                                           << MTLResourceCPUCacheModeShift;
    id<MTLBuffer> buffer = [mtl_heap newBufferWithLength:4096 options:options offset:0];
    const bool untracked = buffer && buffer.hazardTrackingMode == MTLHazardTrackingModeUntracked;
    [buffer release];
    return untracked;
}

std::string DescribeTexture(void* texture) {
    const id<MTLTexture> tex = (id<MTLTexture>)texture;
    if (!tex) {
        return "no texture";
    }
    static const char* const modes[] = {"Shared", "Managed", "Private", "Memoryless"};
    const auto mode = [](MTLStorageMode m) { return m < 4 ? modes[m] : "?"; };
    const MTLTextureUsage usage = tex.usage;
    std::string text = std::string("storage ") + mode(tex.storageMode) + ", usage";
    if (usage & MTLTextureUsageShaderRead) text += " Read";
    if (usage & MTLTextureUsageShaderWrite) text += " Write";
    if (usage & MTLTextureUsageRenderTarget) text += " RT";
    if (usage & MTLTextureUsagePixelFormatView) text += " PFV";
    if (usage & MTLTextureUsageShaderAtomic) text += " Atomic";
    text += tex.allowGPUOptimizedContents ? ", GPU-optimized" : ", not GPU-optimized";
    text += tex.compressionType == MTLTextureCompressionTypeLossy ? ", lossy" : "";
    text += tex.isAliasable ? ", aliasable" : "";
    if (const id<MTLHeap> heap = tex.heap) {
        text += std::string(", heap ") + mode(heap.storageMode) +
                (heap.hazardTrackingMode == MTLHazardTrackingModeUntracked ? " untracked"
                                                                          : " tracked") +
                " offset " + std::to_string(tex.heapOffset);
    } else {
        text += ", no heap";
    }
    text += tex.parentTexture ? ", a view" : "";
    return text;
}

} // namespace Vulkan::MetalHeap

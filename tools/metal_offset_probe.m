// SPDX-License-Identifier: GPL-2.0-or-later
// bbport macOS: which Metal commands honour buffer offsets at or past 4 GiB?
//
// A merged dense arena (BufferCache::GetArena) is one MTLBuffer of 8 GiB placed in an untracked
// heap, so draws bind it at offsets past 4 GiB. Two other macOS ports report that Metal draws
// wrongly there (indexed and indirect draws only, says one; any data past 4 GiB, says the other).
// This probe puts the right data at 4 GiB + x and a decoy at x in one buffer placed in a heap, as
// the arenas are, and reports for each command whether the GPU read the right one. "alias" rows
// read the same bytes through a second buffer placed at 4 GiB in the same heap.
//
// Build: clang -arch x86_64 -fobjc-arc -framework Metal -framework Foundation \
//            tools/metal_offset_probe.m -o out/metal_offset_probe
// Run (commits ~4.1 GiB of memory): out/metal_offset_probe [shared|private]
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <string.h>

static const uint64_t kHigh = 1ull << 32;   // where the "right" data lives: kHigh + offset
static const uint64_t kTail = 64ull << 20;  // bytes past 4 GiB
static const uint64_t kIdx = 0x10000;       // index data
static const uint64_t kArgs = 0x20000;      // indirect arguments
static const uint64_t kValue = 0x30000;     // a uint read by a shader / copied by a blit
static const uint64_t kCopyDst = 0x40000;   // blit destination
static const uint64_t kFill = 0x50000;      // fill destination

static NSString* const kSource = @"#include <metal_stdlib>\n"
"using namespace metal;\n"
"vertex void vs_record(uint vid [[vertex_id]], uint iid [[instance_id]],\n"
"                      device atomic_uint* out [[buffer(0)]]) {\n"
"    atomic_fetch_add_explicit(&out[0], 1u, memory_order_relaxed);\n"
"    atomic_store_explicit(&out[1], vid, memory_order_relaxed);\n"
"    atomic_store_explicit(&out[2], iid, memory_order_relaxed);\n"
"}\n"
"vertex void vs_read(uint vid [[vertex_id]], device uint* out [[buffer(0)]],\n"
"                    device const uint* in [[buffer(1)]]) { out[0] = in[0]; }\n"
"struct FetchIn { uint v [[attribute(0)]]; };\n"
"vertex void vs_fetch(FetchIn in [[stage_in]], device uint* out [[buffer(0)]]) { out[0] = in.v; }\n"
"kernel void cs_count(device atomic_uint* out [[buffer(0)]]) {\n"
"    atomic_fetch_add_explicit(&out[0], 1u, memory_order_relaxed);\n"
"}\n"
"kernel void cs_read(device uint* out [[buffer(0)]], device const uint* in [[buffer(1)]]) {\n"
"    out[0] = in[0];\n"
"}\n";

typedef struct {
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLBuffer> big;      // the whole heap, offset 0 (like a merged arena)
    id<MTLBuffer> alias;    // placed at 4 GiB in the same heap
    id<MTLBuffer> staging;  // shared, for private heaps
    id<MTLBuffer> out;
    id<MTLRenderPipelineState> record, read, fetch, fetch_dynamic;
    id<MTLComputePipelineState> count, cs_read;
    bool shared;
    int failures;
} Probe;

// Writes bytes at offset in the big buffer (through the staging buffer and a blit at a low offset
// of the alias buffer for the high half when the heap is private: alias blits are checked below).
static void Put(Probe* p, uint64_t offset, const void* data, size_t size) {
    if (p->shared) {
        memcpy((uint8_t*)p->big.contents + offset, data, size);
        return;
    }
    memcpy(p->staging.contents, data, size);
    id<MTLCommandBuffer> cb = [p->queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    if (offset >= kHigh) {
        [blit copyFromBuffer:p->staging sourceOffset:0 toBuffer:p->alias
           destinationOffset:offset - kHigh size:size];
    } else {
        [blit copyFromBuffer:p->staging sourceOffset:0 toBuffer:p->big
           destinationOffset:offset size:size];
    }
    [blit endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
}

static uint32_t Get(Probe* p, uint64_t offset) {
    if (p->shared) {
        return *(const uint32_t*)((const uint8_t*)p->big.contents + offset);
    }
    id<MTLCommandBuffer> cb = [p->queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    if (offset >= kHigh) {
        [blit copyFromBuffer:p->alias sourceOffset:offset - kHigh toBuffer:p->staging
           destinationOffset:0 size:4];
    } else {
        [blit copyFromBuffer:p->big sourceOffset:offset toBuffer:p->staging
           destinationOffset:0 size:4];
    }
    [blit endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    return *(const uint32_t*)p->staging.contents;
}

static void Report(Probe* p, const char* what, uint32_t got, uint32_t right, uint32_t decoy) {
    const char* verdict = got == right ? "ok" : got == decoy ? "WRONG (read the low decoy)" : "WRONG";
    if (got != right) {
        ++p->failures;
    }
    printf("  %-44s %-26s (got %u, right %u, decoy %u)\n", what, verdict, got, right, decoy);
}

static void ClearOut(Probe* p) {
    memset(p->out.contents, 0, p->out.length);
}

static id<MTLRenderCommandEncoder> RenderPass(id<MTLCommandBuffer> cb) {
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.renderTargetWidth = 16;
    rp.renderTargetHeight = 16;
    rp.defaultRasterSampleCount = 1;
    return [cb renderCommandEncoderWithDescriptor:rp];
}

// The right values live at kHigh + base and the decoys at base, for buffer `buf` bound at `at`.
static void RunAll(Probe* p, id<MTLBuffer> buf, uint64_t at, const char* label) {
    printf("%s\n", label);
    // Index data: right = 7, decoy = 3.
    const uint32_t right_idx = 7, decoy_idx = 3;
    // Indexed indirect: {indexCount, instanceCount, indexStart, baseVertex, baseInstance}.
    const uint32_t right_ind[5] = {1, 1, 0, 100, 0}, decoy_ind[5] = {1, 1, 0, 200, 0};
    // Draw indirect: {vertexCount, instanceCount, vertexStart, baseInstance}.
    const uint32_t right_draw[4] = {1, 1, 11, 0}, decoy_draw[4] = {1, 1, 22, 0};
    // Dispatch indirect: threadgroups.
    const uint32_t right_disp[3] = {3, 1, 1}, decoy_disp[3] = {5, 1, 1};
    const uint32_t right_value = 0xC0FFEE, decoy_value = 0xDECAF;

    Put(p, kHigh + kIdx, &right_idx, 4);
    Put(p, kIdx, &decoy_idx, 4);
    Put(p, kHigh + kArgs, right_ind, sizeof(right_ind));
    Put(p, kArgs, decoy_ind, sizeof(decoy_ind));
    Put(p, kHigh + kArgs + 0x100, right_draw, sizeof(right_draw));
    Put(p, kArgs + 0x100, decoy_draw, sizeof(decoy_draw));
    Put(p, kHigh + kArgs + 0x200, right_disp, sizeof(right_disp));
    Put(p, kArgs + 0x200, decoy_disp, sizeof(decoy_disp));
    Put(p, kHigh + kValue, &right_value, 4);
    Put(p, kValue, &decoy_value, 4);

    const uint64_t idx = at + kIdx, args = at + kArgs, value = at + kValue;
    // 1. drawIndexedPrimitives: index buffer offset.
    {
        ClearOut(p);
        id<MTLCommandBuffer> cb = [p->queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = RenderPass(cb);
        [enc setRenderPipelineState:p->record];
        [enc setVertexBuffer:p->out offset:0 atIndex:0];
        [enc drawIndexedPrimitives:MTLPrimitiveTypePoint indexCount:1 indexType:MTLIndexTypeUInt32
                       indexBuffer:buf indexBufferOffset:idx];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        Report(p, "drawIndexed: index offset", ((uint32_t*)p->out.contents)[1], right_idx, decoy_idx);
    }
    // 2. Indexed indirect: arguments in buf, indices in buf.
    {
        ClearOut(p);
        id<MTLCommandBuffer> cb = [p->queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = RenderPass(cb);
        [enc setRenderPipelineState:p->record];
        [enc setVertexBuffer:p->out offset:0 atIndex:0];
        [enc drawIndexedPrimitives:MTLPrimitiveTypePoint indexType:MTLIndexTypeUInt32 indexBuffer:buf
                 indexBufferOffset:idx indirectBuffer:buf indirectBufferOffset:args];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        Report(p, "drawIndexed indirect: index + args offsets", ((uint32_t*)p->out.contents)[1],
               right_idx + right_ind[3], decoy_idx + decoy_ind[3]);
    }
    // 3. drawPrimitives indirect.
    {
        ClearOut(p);
        id<MTLCommandBuffer> cb = [p->queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = RenderPass(cb);
        [enc setRenderPipelineState:p->record];
        [enc setVertexBuffer:p->out offset:0 atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypePoint indirectBuffer:buf indirectBufferOffset:args + 0x100];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        Report(p, "drawPrimitives indirect: args offset", ((uint32_t*)p->out.contents)[1],
               right_draw[2], decoy_draw[2]);
    }
    // 4. dispatchThreadgroups indirect.
    {
        ClearOut(p);
        id<MTLCommandBuffer> cb = [p->queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:p->count];
        [enc setBuffer:p->out offset:0 atIndex:0];
        [enc dispatchThreadgroupsWithIndirectBuffer:buf indirectBufferOffset:args + 0x200
                              threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        Report(p, "dispatch indirect: args offset", ((uint32_t*)p->out.contents)[0], right_disp[0],
               decoy_disp[0]);
    }
    // 5. Vertex shader reads through setVertexBuffer:offset:.
    {
        ClearOut(p);
        id<MTLCommandBuffer> cb = [p->queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = RenderPass(cb);
        [enc setRenderPipelineState:p->read];
        [enc setVertexBuffer:p->out offset:0 atIndex:0];
        [enc setVertexBuffer:buf offset:value atIndex:1];
        [enc drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:1];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        Report(p, "vertex shader: setVertexBuffer offset", ((uint32_t*)p->out.contents)[0],
               right_value, decoy_value);
    }
    // 5b. Fixed-function vertex fetch ([[stage_in]], a vertex descriptor), as our pipelines read
    // vertices: a static stride, and a dynamic one (MoltenVK's dynamic vertex input).
    for (int dynamic = 0; dynamic < 2; ++dynamic) {
        ClearOut(p);
        id<MTLCommandBuffer> cb = [p->queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = RenderPass(cb);
        [enc setRenderPipelineState:dynamic ? p->fetch_dynamic : p->fetch];
        [enc setVertexBuffer:p->out offset:0 atIndex:0];
        if (dynamic) {
            [enc setVertexBuffer:buf offset:value attributeStride:4 atIndex:1];
        } else {
            [enc setVertexBuffer:buf offset:value atIndex:1];
        }
        [enc drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:1];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        Report(p, dynamic ? "vertex fetch (stage_in), dynamic stride" : "vertex fetch (stage_in)",
               ((uint32_t*)p->out.contents)[0], right_value, decoy_value);
    }
    // 6. Compute shader reads through setBuffer:offset:.
    {
        ClearOut(p);
        id<MTLCommandBuffer> cb = [p->queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:p->cs_read];
        [enc setBuffer:p->out offset:0 atIndex:0];
        [enc setBuffer:buf offset:value atIndex:1];
        [enc dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        Report(p, "compute shader: setBuffer offset", ((uint32_t*)p->out.contents)[0], right_value,
               decoy_value);
    }
    // 7. Blit: source offset.
    {
        id<MTLBuffer> dst = [p->device newBufferWithLength:16 options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = [p->queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit copyFromBuffer:buf sourceOffset:value toBuffer:dst destinationOffset:0 size:4];
        [blit endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        Report(p, "blit copy: source offset", *(const uint32_t*)dst.contents, right_value, decoy_value);
    }
    // 8. Blit: destination offset (the high copy must change only the high bytes).
    {
        const uint32_t before = 0x1111, written = 0x2222;
        Put(p, kHigh + kCopyDst, &before, 4);
        Put(p, kCopyDst, &before, 4);
        id<MTLBuffer> src = [p->device newBufferWithLength:16 options:MTLResourceStorageModeShared];
        memcpy(src.contents, &written, 4);
        id<MTLCommandBuffer> cb = [p->queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit copyFromBuffer:src sourceOffset:0 toBuffer:buf destinationOffset:at + kCopyDst size:4];
        [blit endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        const uint32_t high = Get(p, kHigh + kCopyDst), low = Get(p, kCopyDst);
        Report(p, "blit copy: destination offset (high bytes)", high, written, before);
        Report(p, "blit copy: destination offset (low bytes kept)", low, before, written);
    }
    // 9. Blit fill.
    {
        const uint32_t before = 0x1111;
        Put(p, kHigh + kFill, &before, 4);
        Put(p, kFill, &before, 4);
        id<MTLCommandBuffer> cb = [p->queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit fillBuffer:buf range:NSMakeRange(at + kFill, 4) value:0x77];
        [blit endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        const uint32_t high = Get(p, kHigh + kFill), low = Get(p, kFill);
        Report(p, "blit fill (high bytes)", high, 0x77777777u, before);
        Report(p, "blit fill (low bytes kept)", low, before, 0x77777777u);
    }
}

int main(int argc, char** argv) {
    @autoreleasepool {
        Probe p = {0};
        p.shared = !(argc > 1 && strcmp(argv[1], "private") == 0);
        p.device = MTLCreateSystemDefaultDevice();
        p.queue = [p.device newCommandQueue];
        printf("Device: %s, %s heap, maxBufferLength %.1f GiB\n", p.device.name.UTF8String,
               p.shared ? "shared" : "private", p.device.maxBufferLength / 1073741824.0);

        MTLHeapDescriptor* desc = [MTLHeapDescriptor new];
        desc.type = MTLHeapTypePlacement;
        desc.storageMode = p.shared ? MTLStorageModeShared : MTLStorageModePrivate;
        desc.hazardTrackingMode = MTLHazardTrackingModeUntracked;
        desc.size = kHigh + kTail;
        id<MTLHeap> heap = [p.device newHeapWithDescriptor:desc];
        if (!heap) {
            printf("cannot create a %.2f GiB heap\n", (kHigh + kTail) / 1073741824.0);
            return 2;
        }
        const MTLResourceOptions options =
            (p.shared ? MTLResourceStorageModeShared : MTLResourceStorageModePrivate) |
            MTLResourceHazardTrackingModeUntracked;
        p.big = [heap newBufferWithLength:kHigh + kTail options:options offset:0];
        p.alias = [heap newBufferWithLength:kTail options:options offset:kHigh];
        p.staging = [p.device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
        p.out = [p.device newBufferWithLength:256 options:MTLResourceStorageModeShared];
        if (!p.big || !p.alias) {
            printf("cannot place the buffers\n");
            return 2;
        }

        NSError* error = nil;
        id<MTLLibrary> lib = [p.device newLibraryWithSource:kSource options:nil error:&error];
        if (!lib) {
            printf("shader: %s\n", error.localizedDescription.UTF8String);
            return 2;
        }
        MTLRenderPipelineDescriptor* rd = [MTLRenderPipelineDescriptor new];
        rd.rasterizationEnabled = NO;
        rd.vertexFunction = [lib newFunctionWithName:@"vs_record"];
        p.record = [p.device newRenderPipelineStateWithDescriptor:rd error:&error];
        rd.vertexFunction = [lib newFunctionWithName:@"vs_read"];
        p.read = [p.device newRenderPipelineStateWithDescriptor:rd error:&error];
        rd.vertexFunction = [lib newFunctionWithName:@"vs_fetch"];
        MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
        vd.attributes[0].format = MTLVertexFormatUInt;
        vd.attributes[0].offset = 0;
        vd.attributes[0].bufferIndex = 1;
        vd.layouts[1].stride = 4;
        rd.vertexDescriptor = vd;
        p.fetch = [p.device newRenderPipelineStateWithDescriptor:rd error:&error];
        vd.layouts[1].stride = MTLBufferLayoutStrideDynamic;
        p.fetch_dynamic = [p.device newRenderPipelineStateWithDescriptor:rd error:&error];
        p.count = [p.device newComputePipelineStateWithFunction:[lib newFunctionWithName:@"cs_count"]
                                                          error:&error];
        p.cs_read = [p.device newComputePipelineStateWithFunction:[lib newFunctionWithName:@"cs_read"]
                                                            error:&error];
        if (!p.record || !p.read || !p.fetch || !p.fetch_dynamic || !p.count || !p.cs_read) {
            printf("pipeline: %s\n", error.localizedDescription.UTF8String);
            return 2;
        }

        RunAll(&p, p.big, kHigh, "One buffer over the heap, offsets 4 GiB + x (a merged arena):");
        RunAll(&p, p.alias, 0, "A second buffer placed at 4 GiB in the heap, offsets x (a window):");
        printf("%d wrong\n", p.failures);
        return 0;
    }
}

// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
}
namespace VideoCore {
struct Image;
struct ImageInfo;
}

/// bbport t23 (BB_IMAGE_DIAG=1): what Metal's lossless compression would need of the images.
/// Metal compresses a private texture only without ShaderWrite and PixelFormatView; MoltenVK sets
/// those for every colour image (Storage usage; MutableFormat without a format list). This logs
/// the Metal textures of the first images of each format and size, and counts per class which
/// images ever get a storage view or a view in another format than their own or its sRGB twin.
namespace BbImageDiag {

/// BB_IMAGE_KEYS=<file>: with BB_IMAGE_DIAG=1, the images that got a storage view or a view in
/// another format are appended to it (guest address, format, size); with BB_IMAGE_COMPRESS, it
/// lists images created with their full usage from the start.
///
/// BB_IMAGE_COMPRESS (t23 lazy usages; on unless =0; GPU 14.3 -> 12.5 ms/frame): colour images from BB_IMAGE_COMPRESS_MIN (1280) wide
/// are created without Storage and with a format list of their own format and its sRGB/linear
/// twin, so Metal may compress them. The first view that needs more (a storage view, another
/// format, a host pass writing it: EnsureStorage) replaces the backing with a full one
/// (Runtime::UpgradeBacking); later images of the same guest address, format and size are
/// created full. Returns the list to chain, or null for the full usage.
const vk::ImageFormatListCreateInfo* CompressList(const VideoCore::ImageInfo& info);

bool Enabled();
void OnCreate(const Vulkan::Instance& instance, const VideoCore::Image& image);
/// A view about to be created. One a lazy backing cannot have counts as a miss (a bug: the
/// backing should have been upgraded first).
void OnView(const VideoCore::Image& image, vk::Format view_format, bool storage);
/// Runtime::UpgradeBacking replaced the lazy backing of `image` (`copied`: with its contents).
void OnUpgrade(const VideoCore::Image& image, bool copied, const char* why);
/// For a host pass (`pass`) that writes `image` through a storage view of its own: upgrades a
/// lazy backing, then returns whether the image has Storage usage.
bool EnsureStorage(VideoCore::Image& image, const char* pass);

} // namespace BbImageDiag

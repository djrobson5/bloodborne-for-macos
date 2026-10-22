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
/// another format are appended to it (guest address, format, size); with BB_IMAGE_COMPRESS=1, it
/// lists the images that keep their full usage.
///
/// BB_IMAGE_COMPRESS=1 (t23 bound test): colour images from 512 wide, not in the keys file, are
/// created without Storage and with a format list of their own format and its sRGB/linear twin,
/// so Metal may compress them. Returns the list to chain, or null for the usual image.
const vk::ImageFormatListCreateInfo* CompressList(const VideoCore::ImageInfo& info);

bool Enabled();
void OnCreate(const Vulkan::Instance& instance, const VideoCore::Image& image);
void OnView(const VideoCore::Image& image, vk::Format view_format, bool storage);

} // namespace BbImageDiag

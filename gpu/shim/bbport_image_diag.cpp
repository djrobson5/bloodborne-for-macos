// SPDX-License-Identifier: GPL-2.0-or-later

#include "bbport_image_diag.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>

#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/texture_cache/image.h"
#ifdef __APPLE__
#include <vulkan/vulkan_metal.h>
#include "video_core/renderer_vulkan/vk_metal_heap.h"
#endif

namespace BbImageDiag {
namespace {

using ClassKey = std::tuple<int, u32, u32, u32, u32>; // format, width, height, layers, levels

struct ClassStats {
    u64 created = 0;
    u64 bytes = 0;
    u64 storage = 0;     ///< images that got a storage view
    u64 reinterpret = 0; ///< images viewed in another format than their own or its sRGB twin
};

struct ImageEntry {
    ClassKey key;
    bool storage = false;
    bool reinterpret = false;
    bool compressed = false; ///< created without Storage (BB_IMAGE_COMPRESS)
};

using GuestKey = std::tuple<u64, int, u32, u32>; // guest address, format, width, height

struct FormatList {
    vk::Format formats[2];
    vk::ImageFormatListCreateInfo info;
};

std::mutex mutex;
std::map<ClassKey, ClassStats> classes;
std::unordered_map<u64, ImageEntry> images; // by image_uid
std::set<std::tuple<int, u32, u32, bool>> dumped;
std::chrono::steady_clock::time_point last_report;
u64 compressed_images = 0, misses = 0;

const char* KeysPath() {
    static const char* path = std::getenv("BB_IMAGE_KEYS");
    return path;
}

bool CompressOn() {
    static const bool on = [] {
        const char* env = std::getenv("BB_IMAGE_COMPRESS");
        return env && env[0] == '1';
    }();
    return on;
}

GuestKey KeyOf(const VideoCore::ImageInfo& info) {
    return {info.guest_address, int(info.pixel_format), info.size.width, info.size.height};
}

/// The keys file, read once (compress mode).
const std::set<GuestKey>& KeptKeys() {
    static const std::set<GuestKey> keys = [] {
        std::set<GuestKey> result;
        if (const char* path = KeysPath()) {
            if (FILE* file = std::fopen(path, "r")) {
                unsigned long long address;
                int format;
                unsigned width, height;
                while (std::fscanf(file, "%llx %d %u %u", &address, &format, &width, &height) == 4) {
                    result.emplace(address, format, width, height);
                }
                std::fclose(file);
            }
        }
        std::printf("Image compress (BB_IMAGE_COMPRESS): %zu images keep full usage (%s)\n",
                    result.size(), KeysPath() ? KeysPath() : "no BB_IMAGE_KEYS");
        return result;
    }();
    return keys;
}

void RecordKeyLocked(const VideoCore::ImageInfo& info) {
    static std::set<GuestKey> written;
    const char* path = KeysPath();
    if (!path || CompressOn() || !written.insert(KeyOf(info)).second) {
        return;
    }
    if (FILE* file = std::fopen(path, "a")) {
        std::fprintf(file, "%llx %d %u %u\n", (unsigned long long)info.guest_address,
                     int(info.pixel_format), info.size.width, info.size.height);
        std::fclose(file);
    }
}

vk::Format Twin(vk::Format format) {
    switch (format) {
    case vk::Format::eR8G8B8A8Unorm:
        return vk::Format::eR8G8B8A8Srgb;
    case vk::Format::eR8G8B8A8Srgb:
        return vk::Format::eR8G8B8A8Unorm;
    case vk::Format::eB8G8R8A8Unorm:
        return vk::Format::eB8G8R8A8Srgb;
    case vk::Format::eB8G8R8A8Srgb:
        return vk::Format::eB8G8R8A8Unorm;
    case vk::Format::eA8B8G8R8UnormPack32:
        return vk::Format::eA8B8G8R8SrgbPack32;
    case vk::Format::eA8B8G8R8SrgbPack32:
        return vk::Format::eA8B8G8R8UnormPack32;
    default:
        return vk::Format::eUndefined;
    }
}

bool Tracked(const VideoCore::Image& image) {
    return !image.info.props.is_block && !image.info.props.is_depth;
}

std::string Linear(vk::Format format) {
    std::string name = vk::to_string(format);
    if (const auto pos = name.find("Srgb"); pos != std::string::npos) {
        name.replace(pos, 4, "Unorm");
    }
    return name;
}

void ReportLocked() {
    std::printf("Image diag (colour, non-block; cumulative): format WxH Llayers Mlevels: created, "
                "MiB of the last, with storage views, with other-format views; %llu created "
                "compressible, %llu misses\n",
                (unsigned long long)compressed_images, (unsigned long long)misses);
    for (const auto& [key, stats] : classes) {
        const auto [format, width, height, layers, levels] = key;
        if (width < 256 && stats.storage == 0 && stats.reinterpret == 0) {
            continue;
        }
        std::printf("  %s %ux%u L%u M%u: %llu, %.1f MiB, storage %llu, other format %llu\n",
                    vk::to_string(vk::Format(format)).c_str(), width, height, layers, levels,
                    (unsigned long long)stats.created, double(stats.bytes) / stats.created / 1048576.0,
                    (unsigned long long)stats.storage, (unsigned long long)stats.reinterpret);
    }
}

void MaybeReportLocked() {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_report > std::chrono::seconds(20)) {
        last_report = now;
        ReportLocked();
    }
}

} // namespace

const vk::ImageFormatListCreateInfo* CompressList(const VideoCore::ImageInfo& info) {
    if (!CompressOn() || info.props.is_block || info.props.is_depth || info.size.width < 1280 ||
        KeptKeys().contains(KeyOf(info))) {
        return nullptr;
    }
    static std::map<vk::Format, FormatList> lists; // node-stable: image_ci keeps the pointer
    std::scoped_lock lock{mutex};
    ++compressed_images;
    auto [it, inserted] = lists.try_emplace(info.pixel_format);
    if (inserted) {
        auto& list = it->second;
        list.formats[0] = info.pixel_format;
        list.formats[1] = Twin(info.pixel_format);
        list.info = vk::ImageFormatListCreateInfo{
            .viewFormatCount = list.formats[1] == vk::Format::eUndefined ? 1u : 2u,
            .pViewFormats = list.formats,
        };
    }
    return &it->second.info;
}

bool Enabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_IMAGE_DIAG");
        return (env && env[0] == '1') || CompressOn();
    }();
    return enabled;
}

void OnCreate(const Vulkan::Instance& instance, const VideoCore::Image& image) {
    if (!Enabled()) {
        return;
    }
    const auto& info = image.info;
    std::scoped_lock lock{mutex};
    if (last_report == std::chrono::steady_clock::time_point{}) {
        last_report = std::chrono::steady_clock::now();
    }
#ifdef __APPLE__
    // The Metal texture of the first image of each format and size (from 512 wide).
    if (info.size.width >= 512 &&
        dumped.emplace(int(info.pixel_format), info.size.width, info.size.height,
                       info.props.is_block)
            .second &&
        dumped.size() <= 60) {
        static const auto export_objects = reinterpret_cast<PFN_vkExportMetalObjectsEXT>(
            VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr(instance.GetDevice(),
                                                              "vkExportMetalObjectsEXT"));
        VkExportMetalTextureInfoEXT texture_info = {
            .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT,
            .image = image.GetImage(),
            .plane = VK_IMAGE_ASPECT_PLANE_0_BIT,
        };
        VkExportMetalObjectsInfoEXT objects = {
            .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
            .pNext = &texture_info,
        };
        if (export_objects) {
            export_objects(instance.GetDevice(), &objects);
        }
        std::printf("Image diag: %s %ux%u L%u M%u usage %s flags %s: %s\n",
                    vk::to_string(info.pixel_format).c_str(), info.size.width, info.size.height,
                    info.resources.layers, info.resources.levels,
                    vk::to_string(image.usage_flags).c_str(),
                    info.props.is_block ? "block" : info.props.is_depth ? "depth" : "colour",
                    Vulkan::MetalHeap::DescribeTexture(texture_info.mtlTexture).c_str());
    }
#endif
    if (!Tracked(image)) {
        return;
    }
    const ClassKey key{int(info.pixel_format), info.size.width, info.size.height,
                       info.resources.layers, info.resources.levels};
    auto& stats = classes[key];
    ++stats.created;
    stats.bytes += info.guest_size;
    images[image.image_uid] = ImageEntry{
        .key = key, .compressed = !(image.usage_flags & vk::ImageUsageFlagBits::eStorage)};
    MaybeReportLocked();
}

void OnView(const VideoCore::Image& image, vk::Format view_format, bool storage) {
    if (!Enabled() || !Tracked(image)) {
        return;
    }
    const bool reinterpret =
        view_format != image.info.pixel_format && Linear(view_format) != Linear(image.info.pixel_format);
    if (!storage && !reinterpret) {
        return;
    }
    std::scoped_lock lock{mutex};
    const auto it = images.find(image.image_uid);
    if (it == images.end()) {
        return;
    }
    auto& entry = it->second;
    auto& stats = classes[entry.key];
    RecordKeyLocked(image.info);
    if (entry.compressed && ((storage && !entry.storage) || (reinterpret && !entry.reinterpret))) {
        ++misses;
        if (misses <= 10) {
            std::printf("Image compress miss: %s %ux%u at %#llx needs a %s view (%s)\n",
                        vk::to_string(image.info.pixel_format).c_str(), image.info.size.width,
                        image.info.size.height, (unsigned long long)image.info.guest_address,
                        storage ? "storage" : "reinterpreting", vk::to_string(view_format).c_str());
        }
    }
    if (storage && !entry.storage) {
        entry.storage = true;
        ++stats.storage;
    }
    if (reinterpret && !entry.reinterpret) {
        entry.reinterpret = true;
        ++stats.reinterpret;
        if (stats.reinterpret <= 2) {
            std::printf("Image diag: %s %ux%u viewed as %s%s\n",
                        vk::to_string(image.info.pixel_format).c_str(), image.info.size.width,
                        image.info.size.height, vk::to_string(view_format).c_str(),
                        storage ? " (storage)" : "");
        }
    }
    MaybeReportLocked();
}

bool HasStorage(const VideoCore::Image& image, const char* pass) {
    const bool has = bool(image.usage_flags & vk::ImageUsageFlagBits::eStorage);
    if (!Enabled()) {
        return has;
    }
    std::scoped_lock lock{mutex};
    if (has) {
        RecordKeyLocked(image.info);
    } else if (const auto it = images.find(image.image_uid);
               it != images.end() && it->second.compressed && !it->second.storage) {
        it->second.storage = true; // reported once per image
        ++misses;
        std::printf("Image compress miss: %s %ux%u at %#llx: %s skipped (no Storage)\n",
                    vk::to_string(image.info.pixel_format).c_str(), image.info.size.width,
                    image.info.size.height, (unsigned long long)image.info.guest_address, pass);
    }
    return has;
}

} // namespace BbImageDiag

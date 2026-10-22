// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <bit>
#include <bitset>
#include <xxhash.h>

#include "common/types.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/profile.h"

namespace Shader {

struct VsAttribSpecialization {
    u32 divisor{};
    AmdGpu::NumberClass num_class{};
    AmdGpu::CompMapping dst_select{};

    bool operator==(const VsAttribSpecialization&) const = default;
};

struct BufferSpecialization {
    u32 stride : 14;
    u32 is_formatted : 1;
    u32 swizzle_enable : 1;
    u32 data_format : 6;
    u32 num_format : 4;
    u32 index_stride : 2;
    u32 element_size : 2;
    AmdGpu::CompMapping dst_select{};
    AmdGpu::NumberConversion num_conversion{};

    bool operator==(const BufferSpecialization& other) const {
        return stride == other.stride && is_formatted == other.is_formatted &&
               swizzle_enable == other.swizzle_enable &&
               (!is_formatted ||
                (data_format == other.data_format && num_format == other.num_format &&
                 dst_select == other.dst_select && num_conversion == other.num_conversion)) &&
               (!swizzle_enable ||
                (index_stride == other.index_stride && element_size == other.element_size));
    }
};

struct ImageSpecialization {
    AmdGpu::ImageType type = AmdGpu::ImageType::Color2D;
    bool is_integer = false;
    bool is_storage = false;
    bool is_cube = false;
    bool is_srgb = false;
    AmdGpu::CompMapping dst_select{};
    AmdGpu::NumberConversion num_conversion{};
    // FIXME any pipeline cache changes needed?
    u32 num_bindings = 0;

    bool operator==(const ImageSpecialization&) const = default;
};

struct FMaskSpecialization {
    u32 width;
    u32 height;

    bool operator==(const FMaskSpecialization&) const = default;
};

struct SamplerSpecialization {
    u8 force_unnormalized : 1;
    u8 force_degamma : 1;

    bool operator==(const SamplerSpecialization&) const = default;
};

/**
 * Alongside runtime information, this structure also checks bound resources
 * for compatibility. Can be used as a key for storing shader permutations.
 * Is separate from runtime information, because resource layout can only be deduced
 * after the first compilation of a module.
 */
struct StageSpecialization {
    static constexpr size_t MaxStageResources = 128;

    const Info* info{};
    RuntimeInfo runtime_info{};
    std::bitset<MaxStageResources> bitset{};
    std::optional<Gcn::FetchShaderData> fetch_shader_data{};
    boost::container::small_vector<VsAttribSpecialization, 32> vs_attribs;
    boost::container::small_vector<BufferSpecialization, 16> buffers;
    boost::container::small_vector<ImageSpecialization, 16> images;
    boost::container::small_vector<FMaskSpecialization, 8> fmasks;
    boost::container::small_vector<SamplerSpecialization, 16> samplers;
    Backend::Bindings start{};

    StageSpecialization() = default;
    StageSpecialization(const Info& info_, RuntimeInfo runtime_info_, const Profile& profile_,
                        Backend::Bindings start_)
        : info{&info_}, runtime_info{runtime_info_}, start{start_} {
        fetch_shader_data = Gcn::ParseFetchShader(info_);
        if (info_.sw_stage == SwStage::Vertex && fetch_shader_data) {
            // Specialize shader on VS input number types to follow spec.
            ForEachSharp(vs_attribs, fetch_shader_data->attributes,
                         [this](auto& spec, const auto& desc, AmdGpu::Buffer sharp) {
                             using InstanceIdType = Shader::Gcn::VertexAttribute::InstanceIdType;
                             if (const auto step_rate = desc.GetStepRate();
                                 step_rate != InstanceIdType::None) {
                                 spec.divisor = step_rate == InstanceIdType::OverStepRate0
                                                    ? runtime_info.sw.vs.step_rate_0
                                                    : (step_rate == InstanceIdType::OverStepRate1
                                                           ? runtime_info.sw.vs.step_rate_1
                                                           : 1);
                             }
                             spec.num_class = AmdGpu::GetNumberClass(sharp.GetNumberFmt());
                             spec.dst_select = sharp.DstSelect();
                         });
        }
        u32 binding{};
        ForEachSharp(binding, buffers, info->buffers,
                     [](auto& spec, const auto& desc, AmdGpu::Buffer sharp) {
                         spec.stride = sharp.GetStride();
                         spec.is_formatted = desc.is_formatted;
                         spec.swizzle_enable = sharp.swizzle_enable;
                         if (spec.is_formatted) {
                             spec.data_format = static_cast<u32>(sharp.GetDataFmt());
                             spec.num_format = static_cast<u32>(sharp.GetNumberFmt());
                             spec.dst_select = sharp.DstSelect();
                             spec.num_conversion = sharp.GetNumberConversion();
                         }
                         if (spec.swizzle_enable) {
                             spec.index_stride = sharp.index_stride;
                             spec.element_size = sharp.element_size;
                         }
                     });
        ForEachSharp(binding, images, info->images,
                     [&](auto& spec, const auto& desc, AmdGpu::Image sharp) {
                         spec.type = sharp.GetViewType(desc.is_array);
                         spec.is_integer = AmdGpu::IsInteger(sharp.GetNumberFmt());
                         spec.is_storage = desc.is_written;
                         spec.is_cube = sharp.IsCube();
                         if (spec.is_storage) {
                             spec.dst_select = sharp.DstSelect();
                         } else {
                             spec.is_srgb = sharp.GetNumberFmt() == AmdGpu::NumberFormat::Srgb;
                         }
                         spec.num_conversion = sharp.GetNumberConversion();
                         spec.num_bindings = desc.NumBindings(*info);
                     });
        ForEachSharp(binding, fmasks, info->fmasks,
                     [](auto& spec, const auto& desc, AmdGpu::Image sharp) {
                         spec.width = sharp.width;
                         spec.height = sharp.height;
                     });
        ForEachSharp(samplers, info->samplers,
                     [](auto& spec, const auto& desc, AmdGpu::Sampler sharp) {
                         spec.force_unnormalized = sharp.force_unnormalized;
                         spec.force_degamma = sharp.force_degamma;
                     });

        // Initialize runtime_info fields that rely on analysis in tessellation passes
        if (info->sw_stage == SwStage::TessellationControl ||
            info->sw_stage == SwStage::TessellationEval) {
            TessellationDataConstantBuffer tess_constants{};
            info->ReadTessConstantBuffer(tess_constants);
            runtime_info.InitFromTessConstants(tess_constants);
        }
    }

    void ForEachSharp(auto& spec_list, auto& desc_list, auto&& func) {
        for (const auto& desc : desc_list) {
            auto& spec = spec_list.emplace_back();
            const auto sharp = desc.GetSharp(*info);
            if (!sharp) {
                continue;
            }
            func(spec, desc, sharp);
        }
    }

    void ForEachSharp(u32& binding, auto& spec_list, auto& desc_list, auto&& func) {
        for (const auto& desc : desc_list) {
            auto& spec = spec_list.emplace_back();
            const auto sharp = desc.GetSharp(*info);
            if (!sharp) {
                binding++;
                continue;
            }
            bitset.set(binding++);
            func(spec, desc, sharp);
        }
    }

    [[nodiscard]] bool Valid() const {
        return info != nullptr;
    }

    bool operator==(const StageSpecialization& other) const {
        if (!Valid() || !other.Valid() || buffers.size() != other.buffers.size() ||
            images.size() != other.images.size() || samplers.size() != other.samplers.size()) {
            return false;
        }

        if (vs_attribs != other.vs_attribs) {
            return false;
        }

        if (runtime_info != other.runtime_info) {
            return false;
        }

        if (fetch_shader_data != other.fetch_shader_data) {
            return false;
        }

        if (fmasks != other.fmasks) {
            return false;
        }

        // For VS which only generates geometry and doesn't have any inputs, its start
        // bindings still may change as they depend on previously processed FS. The check below
        // handles this case and prevents generation of redundant permutations. This is also safe
        // for other types of shaders with no bindings.
        if (bitset.none() && other.bitset.none()) {
            return true;
        }

        if (start != other.start) {
            return false;
        }

        u32 binding{};
        for (u32 i = 0; i < buffers.size(); i++) {
            if (other.bitset[binding++] && buffers[i] != other.buffers[i]) {
                return false;
            }
        }
        for (u32 i = 0; i < images.size(); i++) {
            if (other.bitset[binding++] && images[i] != other.images[i]) {
                return false;
            }
        }

        for (u32 i = 0; i < samplers.size(); i++) {
            if (samplers[i] != other.samplers[i]) {
                return false;
            }
        }
        return true;
    }

    /// bbport: identifies this permutation in the on-disk pipeline cache (with the program
    /// hash, Vulkan::Program::Key). The creation index used to be the key, so a run that made
    /// a program's permutations in another order rewrote a cached pipeline's stages under it.
    /// Keep in step with operator==: it hashes what that compares and nothing else (no
    /// inactive union members, padding, pointers or ignored fields). The bitset is hashed
    /// too, so permutations that differ never share a key; the rare pairs operator== still
    /// matches across different bitsets only cost a separate cache entry.
    [[nodiscard]] u64 Hash() const {
        boost::container::small_vector<u64, 256> words;
        const auto put = [&](auto... values) { (words.push_back(u64(values)), ...); };
        const auto put_swizzle = [&](const AmdGpu::CompMapping& mapping) {
            for (const auto swizzle : mapping.array) {
                put(swizzle);
            }
        };
        const auto put_outputs = [&](const auto& output_maps) {
            for (const auto& map : output_maps) {
                for (const auto output : map) {
                    put(output);
                }
            }
        };

        const auto& rt = runtime_info;
        const auto& props = rt.props;
        put(rt.sw_stage, rt.hw_stage, props.fp_denorm_mode32, props.fp_denorm_mode16_64,
            props.fp_round_mode32, props.fp_round_mode16_64, props.num_user_data,
            props.num_input_vgprs, props.num_allocated_vgprs);
        switch (rt.sw_stage) { // RuntimeInfo::HasSameSwInfo
        case SwStage::Vertex: {
            const auto& vs = rt.sw.vs;
            put(vs.step_rate_0, vs.step_rate_1, vs.vertex_sgpr_offset, vs.instance_sgpr_offset,
                vs.tess_emulated_primitive);
            break;
        }
        case SwStage::TessellationControl: {
            const auto& tcs = rt.sw.tcs;
            put(tcs.hs_output_cp_stride, tcs.num_input_control_points, tcs.num_threads,
                tcs.tess_type, tcs.offchip_lds_enable, tcs.ls_stride, tcs.hs_output_base);
            break;
        }
        case SwStage::TessellationEval: {
            const auto& tes = rt.sw.tes;
            put(tes.hs_output_cp_stride, tes.tess_type, tes.tess_topology, tes.tess_partitioning,
                tes.as_vertex);
            for (const u8 format : tes.input_formats) {
                put(format);
            }
            for (const u8 output : tes.ls_out_map) {
                put(output);
            }
            break;
        }
        default:
            break;
        }
        switch (rt.hw_stage) { // RuntimeInfo::HasSameHwInfo
        case HwStage::Local:
            put(rt.hw.ls.ls_stride);
            break;
        case HwStage::Export:
            put(rt.hw.es.vertex_data_size);
            break;
        case HwStage::Geometry: {
            // Its operator== skips the data sizes and mode, and only tests num_invocations != 0.
            const auto& gs = rt.hw.gs;
            put(gs.num_outputs);
            put_outputs(gs.outputs);
            put(gs.num_invocations != 0, gs.output_vertices, gs.in_primitive, gs.vs_copy_hash);
            for (const auto primitive : gs.out_primitive) {
                put(primitive);
            }
            break;
        }
        case HwStage::Vertex: {
            const auto& vs = rt.hw.vs;
            put(vs.num_outputs);
            put_outputs(vs.outputs);
            put(vs.emulate_depth_negative_one_to_one, vs.clip_disable, vs.user_clip_plane_mask,
                vs.motion_vectors, vs.codegen);
            break;
        }
        case HwStage::Fragment: {
            const auto& fs = rt.hw.fs;
            for (const auto& cb : fs.color_buffers) {
                put(cb.data_format, cb.num_format, cb.num_conversion, cb.export_format,
                    cb.blend_self_scale);
                put_swizzle(cb.swizzle);
            }
            // PsInput is 16 one-bit fields; the upper half of its u32 is not part of it.
            static_assert(sizeof(AmdGpu::PsInput) == sizeof(u32));
            put(std::bit_cast<u32>(fs.en_flags) & 0xffffu,
                std::bit_cast<u32>(fs.addr_flags) & 0xffffu, fs.num_inputs, fs.z_export_format,
                fs.num_samples, fs.mrtz_mask, fs.front_face_all_bits, fs.dual_source_blending,
                fs.clip_distance_emulation, fs.motion_vectors, fs.motion_format);
            const u32 num_inputs = std::min<u32>(fs.num_inputs, u32(fs.inputs.size()));
            for (u32 i = 0; i < num_inputs; ++i) {
                const auto& input = fs.inputs[i];
                put(input.param_index, input.is_default, input.is_flat, input.default_value);
            }
            break;
        }
        case HwStage::Compute: {
            // Its operator== skips shared_memory_size.
            const auto& cs = rt.hw.cs;
            for (const u32 size : cs.workgroup_size) {
                put(size);
            }
            for (const bool enable : cs.tgid_enable) {
                put(enable);
            }
            break;
        }
        default:
            break;
        }

        put(buffers.size(), images.size(), samplers.size(), vs_attribs.size(), fmasks.size());
        for (const auto& attrib : vs_attribs) {
            put(attrib.divisor, attrib.num_class);
            put_swizzle(attrib.dst_select);
        }
        put(fetch_shader_data.has_value());
        if (fetch_shader_data) {
            // FetchShaderData::operator== skips size, and VertexAttribute's skips inst_offset
            // and the format overrides.
            put(fetch_shader_data->attributes.size(), fetch_shader_data->vertex_offset_sgpr,
                fetch_shader_data->instance_offset_sgpr);
            for (const auto& attrib : fetch_shader_data->attributes) {
                put(attrib.semantic, attrib.dest_vgpr, attrib.num_elements, attrib.sgpr_base,
                    attrib.dword_offset, attrib.instance_data);
            }
        }
        for (const auto& fmask : fmasks) {
            put(fmask.width, fmask.height);
        }

        // As in operator==: without bound resources the start bindings and resources don't
        // matter.
        if (bitset.any()) {
            static_assert(MaxStageResources == 128);
            const std::bitset<MaxStageResources> low_mask{~0ull};
            put((bitset >> 64).to_ullong(), (bitset & low_mask).to_ullong());
            put(start.unified, start.buffer, start.user_data);
            u32 binding{};
            for (const auto& buffer : buffers) {
                if (!bitset[binding++]) {
                    continue;
                }
                put(buffer.stride, buffer.is_formatted, buffer.swizzle_enable);
                if (buffer.is_formatted) {
                    put(buffer.data_format, buffer.num_format, buffer.num_conversion);
                    put_swizzle(buffer.dst_select);
                }
                if (buffer.swizzle_enable) {
                    put(buffer.index_stride, buffer.element_size);
                }
            }
            for (const auto& image : images) {
                if (!bitset[binding++]) {
                    continue;
                }
                put(image.type, image.is_integer, image.is_storage, image.is_cube, image.is_srgb,
                    image.num_conversion, image.num_bindings);
                put_swizzle(image.dst_select);
            }
            for (const auto& sampler : samplers) {
                put(sampler.force_unnormalized, sampler.force_degamma);
            }
        }
        return XXH3_64bits(words.data(), words.size() * sizeof(u64));
    }

    void Serialize(Serialization::Archive& ar) const;
    bool Deserialize(Serialization::Archive& ar);
};

} // namespace Shader

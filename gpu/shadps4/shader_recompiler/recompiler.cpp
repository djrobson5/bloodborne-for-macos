// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging/classes.h"
#include "shader_recompiler/frontend/control_flow_graph.h"
#include "shader_recompiler/frontend/decode.h"
#include "shader_recompiler/frontend/structured_control_flow.h"
#include "shader_recompiler/frontend/translate/translate.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/post_order.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include <cmath>
#include <optional>

namespace Shader {

IR::BlockList GenerateBlocks(const IR::AbstractSyntaxList& syntax_list) {
    size_t num_syntax_blocks{};
    for (const auto& [_, type] : syntax_list) {
        if (type == IR::AbstractSyntaxNode::Type::Block) {
            ++num_syntax_blocks;
        }
    }
    IR::BlockList blocks{};
    blocks.reserve(num_syntax_blocks);
    for (const auto& [data, type] : syntax_list) {
        if (type == IR::AbstractSyntaxNode::Type::Block) {
            blocks.push_back(data.block);
        }
    }
    return blocks;
}

void EmitControlFlowGraph(IR::Program& program, Pools& pools, Gcn::CFG& cfg,
                          RuntimeInfo& runtime_info, const Profile& profile) {
    Gcn::Translator translator{program.info, runtime_info, profile};
    for (auto& block : cfg) {
        const u32 start = block.begin_index;
        const u32 size = block.end_index - start + 1;
        auto* ir_block = pools.block_pool.Create(pools.inst_pool);
        ir_block->cfg_block = &block;
        block.ir_block = ir_block;
        translator.Translate(ir_block, block.begin, block.cond,
                             std::span{program.ins_list}.subspan(start, size));
        program.blocks.push_back(ir_block);
    }
    translator.EmitPrologue(program.blocks.front());
    ASSERT_MSG(!program.info.translation_failed, "Shader translation has failed");
    for (auto& block : cfg) {
        auto* ir_block = block.ir_block;
        if (block.branch_true) {
            auto* true_block = block.branch_true->ir_block;
            ir_block->AddBranch(true_block);
        }
        if (block.branch_false) {
            auto* false_block = block.branch_false->ir_block;
            ir_block->AddBranch(false_block);
        }
    }
    program.post_order_blocks = Shader::IR::PostOrder(program.blocks.front());
}

/// bbport: tessellation drawn without the tessellation stages (TessAsVertex in
/// vk_pipeline_cache.cpp) runs the domain shader as the vertex shader, which then needs what
/// the local and hull shaders would have handed it. Only shaders that move data and write
/// constant factors qualify: everything they do is checked here, anything else (memory
/// accesses, arithmetic, control flow) leaves them out (Info::ls_pass_through/hs_pass_through).
static void AnalyzeTessPassThrough(const IR::Program& program, Info& info) {
    using IR::Opcode;
    const bool local = info.hw_stage == HwStage::Local;
    const bool hull = info.hw_stage == HwStage::Hull;
    if (!local && !hull) {
        return;
    }
    // A local shader's F32 value as a vertex input component or a constant.
    const auto source = [](const IR::Value& value) -> std::optional<u8> {
        IR::Value v = value;
        std::optional<u32> element;
        for (int depth = 0; depth < 8; ++depth) {
            if (v.IsImmediate()) {
                if (element || v.Type() != IR::Type::F32) {
                    return std::nullopt;
                }
                const f32 f = v.F32();
                if (f == 0.0f && !std::signbit(f)) {
                    return TessPassZero;
                }
                return f == 1.0f ? std::optional<u8>{TessPassOne} : std::nullopt;
            }
            const IR::Inst* inst = v.Inst();
            switch (inst->GetOpcode()) {
            case Opcode::GetAttribute: {
                const IR::Attribute attr = inst->Arg(0).Attribute();
                if (element || !IR::IsParam(attr) || !inst->Arg(1).IsImmediate() ||
                    !inst->Arg(2).IsImmediate() || inst->Arg(2).U32() != 0) {
                    return std::nullopt;
                }
                const u32 location = u32(attr) - u32(IR::Attribute::Param0);
                const u32 comp = inst->Arg(1).U32();
                if (location >= 32 || comp >= 4) {
                    return std::nullopt;
                }
                return u8(location * 4 + comp);
            }
            case Opcode::CompositeExtractF32x2:
            case Opcode::CompositeExtractF32x3:
            case Opcode::CompositeExtractF32x4:
                if (element || !inst->Arg(1).IsImmediate()) {
                    return std::nullopt;
                }
                element = inst->Arg(1).U32();
                v = inst->Arg(0);
                continue;
            case Opcode::CompositeConstructF32x2:
            case Opcode::CompositeConstructF32x3:
            case Opcode::CompositeConstructF32x4:
                if (!element || *element >= inst->NumArgs()) {
                    return std::nullopt;
                }
                v = inst->Arg(*element);
                element.reset();
                continue;
            case Opcode::CompositeShuffleF32x2:
            case Opcode::CompositeShuffleF32x3:
            case Opcode::CompositeShuffleF32x4: {
                const u32 n = inst->GetOpcode() == Opcode::CompositeShuffleF32x2   ? 2
                              : inst->GetOpcode() == Opcode::CompositeShuffleF32x3 ? 3
                                                                                   : 4;
                if (!element || *element >= n || !inst->Arg(2 + *element).IsImmediate()) {
                    return std::nullopt;
                }
                const u32 pick = inst->Arg(2 + *element).U32();
                if (pick >= 2 * n) {
                    return std::nullopt;
                }
                v = inst->Arg(pick < n ? 0 : 1);
                element = pick % n;
                continue;
            }
            default:
                return std::nullopt;
            }
        }
        return std::nullopt;
    };
    std::array<u8, 32> ls_map;
    ls_map.fill(TessPassUndefined);
    std::array<float, 6> factors{};
    u32 factors_set = 0;
    for (const IR::Block* block : program.blocks) {
        for (const IR::Inst& inst : block->Instructions()) {
            switch (inst.GetOpcode()) {
            case Opcode::Prologue:
            case Opcode::Epilogue:
            case Opcode::Void:
            case Opcode::Phi:
            case Opcode::GetAttribute:
            case Opcode::GetAttributeU32:
            case Opcode::GetTessGenericAttribute:
            case Opcode::CompositeExtractF32x2:
            case Opcode::CompositeExtractF32x3:
            case Opcode::CompositeExtractF32x4:
            case Opcode::CompositeConstructF32x2:
            case Opcode::CompositeConstructF32x3:
            case Opcode::CompositeConstructF32x4:
            case Opcode::CompositeShuffleF32x2:
            case Opcode::CompositeShuffleF32x3:
            case Opcode::CompositeShuffleF32x4:
                break; // values only: what reaches an output is checked there
            case Opcode::SetAttribute: {
                if (!local) {
                    return;
                }
                const IR::Attribute attr = inst.Arg(0).Attribute();
                const auto src = source(inst.Arg(1));
                if (!IR::IsParam(attr) || !inst.Arg(2).IsImmediate() || !src) {
                    return;
                }
                const u32 index = (u32(attr) - u32(IR::Attribute::Param0)) * 4 + inst.Arg(2).U32();
                if (index >= ls_map.size()) {
                    return;
                }
                ls_map[index] = *src;
                break;
            }
            case Opcode::SetTcsGenericAttribute: {
                // out[invocation][attr][comp] = in[invocation or 0][attr][comp]
                if (!hull || !inst.Arg(1).IsImmediate() || !inst.Arg(2).IsImmediate() ||
                    inst.Arg(0).IsImmediate()) {
                    return;
                }
                const IR::Inst* load = inst.Arg(0).Inst();
                if (load->GetOpcode() != Opcode::GetTessGenericAttribute ||
                    !load->Arg(1).IsImmediate() || !load->Arg(2).IsImmediate() ||
                    load->Arg(1).U32() != inst.Arg(1).U32() ||
                    load->Arg(2).U32() != inst.Arg(2).U32()) {
                    return;
                }
                const IR::Value vertex = load->Arg(0);
                const bool own = !vertex.IsImmediate() &&
                                 vertex.Inst()->GetOpcode() == Opcode::GetAttributeU32 &&
                                 vertex.Inst()->Arg(0).Attribute() == IR::Attribute::InvocationId;
                if (!own && !(vertex.IsImmediate() && vertex.U32() == 0)) {
                    return;
                }
                break;
            }
            case Opcode::SetPatch: {
                if (!hull) {
                    return;
                }
                const IR::Patch patch = inst.Arg(0).Patch();
                if (IR::IsGeneric(patch)) {
                    break; // per-patch outputs: the pipeline cache requires a TES not reading them
                }
                const u32 index = u32(patch) - u32(IR::Patch::TessellationLodLeft);
                if (index >= factors.size() || !inst.Arg(1).IsImmediate()) {
                    return;
                }
                factors[index] = inst.Arg(1).F32();
                factors_set |= 1u << index;
                break;
            }
            default:
                return;
            }
        }
    }
    if (local) {
        info.ls_out_map = ls_map;
        info.ls_pass_through = true;
    } else if (factors_set == 0x3f) {
        info.hs_tess_factors = factors;
        info.hs_pass_through = true;
    }
}

IR::Program TranslateProgram(const std::span<const u32>& code, Pools& pools, Info& info,
                             RuntimeInfo& runtime_info, const Profile& profile) {
    // Ensure first instruction is expected.
    constexpr u32 token_mov_vcchi = 0xBEEB03FF;
    if (code[0] != token_mov_vcchi) {
        LOG_WARNING(Render_Recompiler, "First instruction is not s_mov_b32 vcc_hi, #imm");
    }

    Gcn::GcnCodeSlice slice(code.data(), code.data() + code.size());
    Gcn::GcnDecodeContext decoder;

    // Decode and save instructions
    IR::Program program{info};
    program.ins_list.reserve(code.size());
    while (!slice.atEnd()) {
        program.ins_list.emplace_back(decoder.decodeInstruction(slice));
    }

    // Clear any previous pooled data.
    pools.ReleaseContents();

    // Create control flow graph
    Common::ObjectPool<Gcn::Block> gcn_block_pool{64};
    Gcn::CFG cfg{gcn_block_pool, program.ins_list};
    EmitControlFlowGraph(program, pools, cfg, runtime_info, profile);

    // On NVIDIA GPUs HW interpolation of clip distance values seems broken, and we need to emulate
    // it with expensive discard in PS.
    Shader::InjectClipDistanceAttributes(program, runtime_info);

    // Run optimization passes on unstructured graph
    if (!profile.support_float64) {
        Shader::Optimization::LowerFp64ToFp32(program);
    }
    Shader::Optimization::SsaRewritePass(program);
    Shader::Optimization::ConstantPropagationPass(program.post_order_blocks);
    if (info.sw_stage == SwStage::TessellationControl) {
        Shader::Optimization::TessellationPreprocess(program, runtime_info);
        Shader::Optimization::HullShaderTransform(program, runtime_info);
    } else if (info.sw_stage == SwStage::TessellationEval) {
        Shader::Optimization::TessellationPreprocess(program, runtime_info);
        Shader::Optimization::DomainShaderTransform(program, runtime_info);
    }
    Shader::Optimization::RingAccessElimination(program, runtime_info);
    Shader::Optimization::ReadLaneEliminationPass(program);
    Shader::IR::DumpProgram(program, info, "pre-res-discover.");
    auto resources = Shader::Optimization::ResourceDiscoverPass(program, profile);
    Shader::Optimization::FlattenExtendedUserdataPass(program);
    Shader::IR::DumpProgram(program, info, "pre-res-patch.");
    Shader::Optimization::ResourcePatchingPass(program.info, resources, profile);
    Shader::Optimization::LowerBufferFormatToRaw(program);
    Shader::Optimization::SharedMemorySimplifyPass(program, profile);
    Shader::Optimization::SharedMemoryToStoragePass(program, runtime_info, profile);
    Shader::Optimization::LowerUserClipPlanes(program, runtime_info);
    Shader::Optimization::PhiSimplificationPass(program);
    Shader::Optimization::InverseBallotEliminationPass(program);
    Shader::Optimization::LowerHardwareIntrinsics(program);
    Shader::IR::DumpProgram(program, info, "pre-lower-phi.");

    // Prepare for structurization by clearing flow graph and lowering phis
    for (auto* ir_block : program.blocks) {
        ir_block->imm_predecessors.clear();
        ir_block->imm_successors.clear();
        ir_block->ssa_state.Reset();
    }
    Shader::Optimization::LowerPhisToRegsPass(program);

    // Structurize control flow graph and create program.
    program.syntax_list = Shader::Gcn::BuildASL(pools, cfg, info);
    program.blocks = GenerateBlocks(program.syntax_list);
    program.post_order_blocks = Shader::IR::PostOrder(program.syntax_list.front().data.block);

    // Run optimization passes on structured graph
    Shader::Optimization::SsaRepairPass(program);
    Shader::Optimization::SsaRewritePass(program);
    Shader::Optimization::SharedMemoryBarrierPass(program, runtime_info, profile);
    Shader::Optimization::DeadCodeEliminationPass(program);
    Shader::Optimization::LowerWave64BallotPass(program, runtime_info, profile);
    Shader::Optimization::ConstantPropagationPass(program.post_order_blocks);
    Shader::Optimization::DeadCodeEliminationPass(program);
    if (info.sw_stage == SwStage::Fragment && profile.needs_integer_interpolation_fix) {
        Shader::Optimization::InterpolatedIntegerPass(program);
    }
    Shader::Optimization::CollectShaderInfoPass(program, profile);
    AnalyzeTessPassThrough(program, info);
    Shader::IR::DumpProgram(program, info);

    return program;
}

} // namespace Shader

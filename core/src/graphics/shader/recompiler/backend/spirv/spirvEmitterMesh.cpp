// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright KytyPS5 contributors; based on Kyty, Copyright (c) 2021 InoriRus (MIT)
// SPDX-License-Identifier: GPL-2.0-only

#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInstructions.h"

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {
namespace {

uint32_t MeshArray(EmitterState& state, spv::StorageClass storage, uint32_t type, uint32_t count) {
	const auto array = state.builder.Type(spv::OpTypeArray, type, ConstantU32(state, count));
	return state.builder.DefineGlobalVariable(TypePointer(state, storage, array), storage);
}

uint32_t MeshElement(EmitterState& state, uint32_t variable, spv::StorageClass storage,
                     uint32_t type, uint32_t index) {
	const auto pointer = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAccessChain, TypePointer(state, storage, type), pointer,
	                          variable, index);
	return pointer;
}

uint32_t MeshLoad(EmitterState& state, uint32_t variable, spv::StorageClass storage, uint32_t type,
                  uint32_t index) {
	const auto pointer = MeshElement(state, variable, storage, type, index);
	const auto value   = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLoad, type, value, pointer);
	return value;
}

uint32_t MeshOutputType(EmitterState& state, IR::StageOutputKind kind) {
	switch (kind) {
		case IR::StageOutputKind::Layer: return TypeU32(state);
		case IR::StageOutputKind::ClipDistance:
		case IR::StageOutputKind::CullDistance: return TypeF32(state);
		case IR::StageOutputKind::Position:
		case IR::StageOutputKind::Parameter: return TypeF32Vector(state, 4);
		default: EXIT("unsupported mesh output kind=%u\n", static_cast<uint32_t>(kind));
	}
}

uint32_t MeshPushDword(EmitterState& state, uint32_t index) {
	const auto pointer = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAccessChain, TypePushConstantElementPointer(state), pointer,
	                          state.push_constant_variable, ConstantU32(state, 0),
	                          ConstantU32(state, index));
	const auto value = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLoad, TypeU32(state), value, pointer);
	return value;
}

uint32_t MeshAddress(EmitterState& state, uint32_t low, uint32_t high) {
	const auto pair = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpCompositeConstruct, TypeU32Vector(state, 2), pair, low, high);
	const auto address = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpBitcast, TypeU64(state), address, pair);
	return address;
}

uint32_t MeshAddBytes(EmitterState& state, uint32_t address, uint32_t bytes) {
	const auto wide = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpUConvert, TypeU64(state), wide, bytes);
	const auto sum = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpIAdd, TypeU64(state), sum, address, wide);
	return sum;
}

// Stores dword `index` of the u32 array at the 64-bit address `base`.
void MeshStoreDword(EmitterState& state, uint32_t base, uint32_t index, uint32_t value) {
	const auto address = MeshAddBytes(
	    state, base, EmitBinaryU32(state, spv::OpShiftLeftLogical, index, ConstantU32(state, 2)));
	const auto pointer = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpConvertUToPtr, TypePhysicalU32Pointer(state), pointer, address);
	state.builder.AddFunction(spv::OpStore, pointer, value, spv::MemoryAccessAlignedMask, 4u);
}

uint32_t MeshBitsU32(EmitterState& state, uint32_t value) {
	const auto bits = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpBitcast, TypeU32(state), bits, value);
	return bits;
}

// Compute form of the mesh entry point: every workgroup owns max_vertices vertex records and
// max_primitives triangles of the output buffer. The renderer zeroes the index region first, so
// triangles a workgroup does not emit stay degenerate.
void EmitEmulatedMeshEntryPoint(EmitterState& state) {
	const auto& mesh          = state.input_info.vertex->mesh;
	const auto  record_dwords = IR::MeshRecordDwords(state.program.info.outputs);
	const auto  vertices      = MeshLoad(state, state.mesh_allocation, spv::StorageClassWorkgroup,
	                                     TypeU32(state), ConstantU32(state, 0));
	const auto  primitives    = MeshLoad(state, state.mesh_allocation, spv::StorageClassWorkgroup,
	                                     TypeU32(state), ConstantU32(state, 1));
	const auto  group         = EmitAddU32(
        state, EmitInputComponentU32(state, IR::StageInputKind::WorkgroupId, 0),
        EmitBinaryU32(state, spv::OpIMul,
                      EmitInputComponentU32(state, IR::StageInputKind::WorkgroupId, 1),
                      MeshPushDword(state, 6)));
	const auto records = MeshAddress(state, MeshPushDword(state, 8), MeshPushDword(state, 9));
	const auto indices = MeshAddBytes(state, records, MeshPushDword(state, 7));
	const auto vertex_base =
	    EmitBinaryU32(state, spv::OpIMul, group, ConstantU32(state, mesh.max_vertices));
	const auto primitive_base =
	    EmitBinaryU32(state, spv::OpIMul, group, ConstantU32(state, mesh.max_primitives));
	for (uint32_t half = 0; half < state.lane_count; half++) {
		state.lane_half      = half;
		const auto index     = EmitLocalInvocationIndex(state);
		const auto is_vertex = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpULessThan, TypeBool(state), is_vertex, index, vertices);
		EmitIfCondition(state, is_vertex, [&] {
			const auto record = EmitBinaryU32(state, spv::OpIMul, EmitAddU32(state, vertex_base, index),
			                                  ConstantU32(state, record_dwords));
			uint32_t offset = 0;
			for (const auto& output: state.outputs) {
				const auto type   = MeshOutputType(state, output.kind);
				const bool shared = output.kind == IR::StageOutputKind::Layer;
				const auto value =
				    MeshLoad(state, output.mesh_data_variable,
				             shared ? spv::StorageClassWorkgroup : spv::StorageClassPrivate, type,
				             shared ? index : ConstantU32(state, half));
				const auto count = IR::MeshRecordOutputDwords(output.kind);
				for (uint32_t component = 0; component < count; component++) {
					uint32_t bits = value;
					if (count == 4) {
						const auto element = state.builder.AllocateId();
						state.builder.AddFunction(spv::OpCompositeExtract, TypeF32(state), element,
						                          value, component);
						bits = MeshBitsU32(state, element);
					} else if (!shared) {
						bits = MeshBitsU32(state, value);
					}
					MeshStoreDword(state, records,
					               EmitAddU32(state, record, ConstantU32(state, offset + component)),
					               bits);
				}
				offset += count;
			}
		});
		const auto is_primitive = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpULessThan, TypeBool(state), is_primitive, index,
		                          primitives);
		EmitIfCondition(state, is_primitive, [&] {
			const auto packed = MeshLoad(state, state.mesh_primitive_data, spv::StorageClassPrivate,
			                             TypeU32(state), ConstantU32(state, half));
			const auto null_bit =
			    EmitBinaryU32(state, spv::OpBitwiseAnd, packed, ConstantU32(state, 0x80000000u));
			const auto culled = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpINotEqual, TypeBool(state), culled, null_bit,
			                          ConstantU32(state, 0));
			uint32_t vertex[3] {};
			for (uint32_t component = 0; component < 3; component++) {
				vertex[component] = state.builder.AllocateId();
				state.builder.AddFunction(spv::OpBitFieldUExtract, TypeU32(state), vertex[component],
				                          packed, ConstantU32(state, component * 10u),
				                          ConstantU32(state, 10));
			}
			const auto triangle =
			    EmitBinaryU32(state, spv::OpIMul, EmitAddU32(state, primitive_base, index),
			                  ConstantU32(state, 3));
			for (uint32_t component = 0; component < 3; component++) {
				// A culled triangle collapses onto its first vertex.
				auto local = vertex[component];
				if (component != 0) {
					local = state.builder.AllocateId();
					state.builder.AddFunction(spv::OpSelect, TypeU32(state), local, culled,
					                          vertex[0], vertex[component]);
				}
				MeshStoreDword(state, indices,
				               EmitAddU32(state, triangle, ConstantU32(state, component)),
				               EmitAddU32(state, vertex_base, local));
			}
		});
	}
	state.lane_half = 0;
}

} // namespace

uint32_t ResolutionScale(EmitterState& state) {
	if (state.resolution_scale == 0) {
		state.resolution_scale = state.builder.Constant(spv::OpSpecConstant, TypeF32(state),
		                                                std::bit_cast<uint32_t>(1.0f));
		state.builder.AddAnnotation(spv::OpDecorate, state.resolution_scale, spv::DecorationSpecId,
		                            IR::ResolutionScaleSpecId);
	}
	return state.resolution_scale;
}

uint32_t ScaledImageMask(EmitterState& state) {
	if (state.scaled_image_mask == 0) {
		state.scaled_image_mask = state.builder.Constant(spv::OpSpecConstant, TypeU32(state), 0u);
		state.builder.AddAnnotation(spv::OpDecorate, state.scaled_image_mask, spv::DecorationSpecId,
		                            IR::ScaledImageMaskSpecId);
	}
	return state.scaled_image_mask;
}

bool MeshEmulated(const EmitterState& state) {
	return state.program.stage == ShaderType::Mesh && state.input_info.vertex != nullptr &&
	       state.input_info.vertex->mesh.emulate;
}

void DefineMeshOutputs(EmitterState& state, uint32_t clip_distance_count,
                       uint32_t cull_distance_count) {
	const auto& mesh = state.input_info.vertex->mesh;
	for (auto& output: state.outputs) {
		const auto type = MeshOutputType(state, output.kind);
		const bool clip = output.kind == IR::StageOutputKind::ClipDistance;
		const bool cull = output.kind == IR::StageOutputKind::CullDistance;
		auto& variable = clip ? state.clip_distance_variable
		                 : cull ? state.cull_distance_variable : output.variable_id;
		if (variable == 0 && !MeshEmulated(state)) {
			const auto element_type = clip || cull
			                              ? state.builder.Type(spv::OpTypeArray, type,
			                                                   ConstantU32(state, clip ? clip_distance_count
			                                                                           : cull_distance_count))
			                              : type;
			variable = MeshArray(state, spv::StorageClassOutput, element_type,
			                     output.kind == IR::StageOutputKind::Layer ? mesh.max_primitives
			                                                             : mesh.max_vertices);
			state.interface_variables.push_back(variable);
			state.builder.AddName(variable, output.debug_name.c_str());
			if (output.kind == IR::StageOutputKind::Parameter) {
				state.builder.AddAnnotation(spv::OpDecorate, variable,
				                            spv::DecorationLocation, output.location);
			} else {
				const auto builtin = clip ? spv::BuiltInClipDistance
				                     : cull ? spv::BuiltInCullDistance
				                     : output.kind == IR::StageOutputKind::Layer ? spv::BuiltInLayer
				                                                                 : spv::BuiltInPosition;
				state.builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationBuiltIn, builtin);
			}
			if (output.kind == IR::StageOutputKind::Layer) {
				state.builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationPerPrimitiveEXT);
			}
		}
		output.variable_id = variable;
		// Only Layer is read by another invocation, through the primitive's provoking vertex.
		const bool shared = output.kind == IR::StageOutputKind::Layer;
		output.mesh_data_variable =
		    MeshArray(state, shared ? spv::StorageClassWorkgroup : spv::StorageClassPrivate, type,
		              shared ? mesh.max_vertices : state.lane_count);
	}
	state.mesh_allocation = MeshArray(state, spv::StorageClassWorkgroup, TypeU32(state), 2);
	state.mesh_primitive_data =
	    MeshArray(state, spv::StorageClassPrivate, TypeU32(state), state.lane_count);
	if (MeshEmulated(state)) {
		return;
	}
	state.mesh_primitives =
	    MeshArray(state, spv::StorageClassOutput, TypeU32Vector(state, 3), mesh.max_primitives);
	state.mesh_cull =
	    MeshArray(state, spv::StorageClassOutput, TypeBool(state), mesh.max_primitives);
	state.interface_variables.push_back(state.mesh_primitives);
	state.interface_variables.push_back(state.mesh_cull);
	state.builder.AddAnnotation(
	    spv::OpDecorate, state.mesh_primitives, spv::DecorationBuiltIn,
	    spv::BuiltInPrimitiveTriangleIndicesEXT); // PrimitiveTriangleIndicesEXT
	state.builder.AddAnnotation(spv::OpDecorate, state.mesh_cull, spv::DecorationBuiltIn,
	                            spv::BuiltInCullPrimitiveEXT); // CullPrimitiveEXT
	state.builder.AddAnnotation(spv::OpDecorate, state.mesh_cull, spv::DecorationPerPrimitiveEXT);
}

uint32_t MeshOutputPointer(EmitterState& state, IR::StageOutputKind kind, uint32_t index) {
	const auto output = std::ranges::find_if(state.outputs, [=](const OutputBinding& binding) {
		return binding.kind == kind && binding.index == index;
	});
	if (output == state.outputs.end()) {
		EXIT("mesh export has no output binding: kind=%u index=%u\n", static_cast<uint32_t>(kind),
		     index);
	}
	const bool shared = kind == IR::StageOutputKind::Layer;
	return MeshElement(
	    state, output->mesh_data_variable,
	    shared ? spv::StorageClassWorkgroup : spv::StorageClassPrivate, MeshOutputType(state, kind),
	    shared ? EmitLocalInvocationIndex(state) : ConstantU32(state, state.lane_half));
}

uint32_t MeshPrimitivePointer(EmitterState& state) {
	return MeshElement(state, state.mesh_primitive_data, spv::StorageClassPrivate, TypeU32(state),
	                   ConstantU32(state, state.lane_half));
}

void EmitMeshAllocate(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto&      state = ctx.state;
	const auto first = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpIEqual, TypeBool(state), first,
	                          EmitLocalInvocationIndex(state), ConstantU32(state, 0));
	EmitIfCondition(state, first, [&] {
		const auto allocation = ctx.Arg(inst, 0);
		for (uint32_t field = 0; field < 2; field++) {
			const auto value = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpBitFieldUExtract, TypeU32(state), value, allocation,
			                          ConstantU32(state, field * 12u),
			                          ConstantU32(state, field == 0 ? 10u : 11u));
			const auto pointer =
			    MeshElement(state, state.mesh_allocation, spv::StorageClassWorkgroup,
			                TypeU32(state), ConstantU32(state, field));
			state.builder.AddFunction(spv::OpStore, pointer, value);
		}
	});
}

void EmitMeshEntryPoint(EmitterState& state) {
	state.builder.AddFunction(spv::OpFunction, TypeVoid(state), state.main_func,
	                          spv::FunctionControlMaskNone, TypeFunction(state));
	EmitLabel(state, state.builder.AllocateId());
	state.builder.AddFunction(spv::OpFunctionCall, TypeVoid(state), state.builder.AllocateId(),
	                          state.mesh_guest_func);
	// All guest waves finish before the uniform Vulkan allocation and output stores.
	EmitBarrier(state);
	if (MeshEmulated(state)) {
		EmitEmulatedMeshEntryPoint(state);
		state.builder.AddFunction(spv::OpReturn);
		state.builder.AddFunction(spv::OpFunctionEnd);
		return;
	}
	const auto vertices   = MeshLoad(state, state.mesh_allocation, spv::StorageClassWorkgroup,
	                                 TypeU32(state), ConstantU32(state, 0));
	const auto primitives = MeshLoad(state, state.mesh_allocation, spv::StorageClassWorkgroup,
	                                 TypeU32(state), ConstantU32(state, 1));
	state.builder.AddFunction(spv::OpSetMeshOutputsEXT, vertices,
	                          primitives); // OpSetMeshOutputsEXT
	for (uint32_t half = 0; half < state.lane_count; half++) {
		state.lane_half      = half;
		const auto index     = EmitLocalInvocationIndex(state);
		const auto is_vertex = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpULessThan, TypeBool(state), is_vertex, index, vertices);
		EmitIfCondition(state, is_vertex, [&] {
			for (const auto& output: state.outputs) {
				if (output.kind == IR::StageOutputKind::Layer) {
					continue;
				}
				const auto type  = MeshOutputType(state, output.kind);
				const auto value =
				    MeshLoad(state, output.mesh_data_variable, spv::StorageClassPrivate, type,
				             ConstantU32(state, half));
				uint32_t pointer;
				if (output.kind == IR::StageOutputKind::ClipDistance ||
				    output.kind == IR::StageOutputKind::CullDistance) {
					pointer = state.builder.AllocateId();
					state.builder.AddFunction(
					    spv::OpAccessChain, TypePointer(state, spv::StorageClassOutput, type), pointer,
					    output.variable_id, index, ConstantU32(state, output.index));
				} else {
					pointer = MeshElement(state, output.variable_id, spv::StorageClassOutput, type, index);
				}
				state.builder.AddFunction(spv::OpStore, pointer, value);
			}
		});
		const auto is_primitive = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpULessThan, TypeBool(state), is_primitive, index,
		                          primitives);
		EmitIfCondition(state, is_primitive, [&] {
			const auto packed = MeshLoad(state, state.mesh_primitive_data, spv::StorageClassPrivate,
			                             TypeU32(state), ConstantU32(state, half));
			uint32_t   vertex[3] {};
			for (uint32_t component = 0; component < 3; component++) {
				vertex[component] = state.builder.AllocateId();
				state.builder.AddFunction(
				    spv::OpBitFieldUExtract, TypeU32(state), vertex[component], packed,
				    ConstantU32(state, component * 10u), ConstantU32(state, 10));
			}
			const auto triangle = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpCompositeConstruct, TypeU32Vector(state, 3), triangle,
			                          vertex[0], vertex[1], vertex[2]);
			const auto triangle_pointer =
			    MeshElement(state, state.mesh_primitives, spv::StorageClassOutput,
			                TypeU32Vector(state, 3), index);
			state.builder.AddFunction(spv::OpStore, triangle_pointer, triangle);
			const auto null_bit =
			    EmitBinaryU32(state, spv::OpBitwiseAnd, packed, ConstantU32(state, 0x80000000u));
			const auto culled = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpINotEqual, TypeBool(state), culled, null_bit,
			                          ConstantU32(state, 0));
			state.builder.AddFunction(spv::OpStore,
			                          MeshElement(state, state.mesh_cull, spv::StorageClassOutput,
			                                      TypeBool(state), index),
			                          culled);
			for (const auto& output: state.outputs) {
				if (output.kind != IR::StageOutputKind::Layer) {
					continue;
				}
				const auto layer = MeshLoad(state, output.mesh_data_variable,
				                            spv::StorageClassWorkgroup, TypeU32(state),
				                            vertex[state.input_info.vertex->mesh.provoking_vertex]);
				const auto pointer = MeshElement(state, output.variable_id, spv::StorageClassOutput,
				                                 TypeU32(state), index);
				state.builder.AddFunction(spv::OpStore, pointer, layer);
			}
		});
	}
	state.lane_half = 0;
	state.builder.AddFunction(spv::OpReturn);
	state.builder.AddFunction(spv::OpFunctionEnd);
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter

namespace Libs::Graphics::ShaderRecompiler::Spirv {

std::vector<uint32_t> BuildMeshPassthroughVertexShader(const std::vector<IR::StageOutput>& outputs) {
	Builder builder(0x00010500u);
	builder.RequireCapability(spv::CapabilityShader);
	builder.AddMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
	const auto void_type  = builder.Type(spv::OpTypeVoid);
	const auto u32_type   = builder.Type(spv::OpTypeInt, 32, 0);
	const auto f32_type   = builder.Type(spv::OpTypeFloat, 32);
	const auto vec4_type  = builder.Type(spv::OpTypeVector, f32_type, 4);
	const auto func_type  = builder.Type(spv::OpTypeFunction, void_type);
	const auto u32_const  = [&](uint32_t value) { return builder.Constant(spv::OpConstant, u32_type, value); };
	const auto pointer    = [&](spv::StorageClass storage, uint32_t type) {
		return builder.Type(spv::OpTypePointer, storage, type);
	};
	uint32_t clip_count = 0;
	uint32_t cull_count = 0;
	for (const auto& output: outputs) {
		if (output.kind == IR::StageOutputKind::ClipDistance) {
			clip_count = std::max(clip_count, output.index + 1u);
		} else if (output.kind == IR::StageOutputKind::CullDistance) {
			cull_count = std::max(cull_count, output.index + 1u);
		}
	}
	const auto builtin_variable = [&](uint32_t type, spv::BuiltIn builtin) {
		const auto variable = builder.DefineGlobalVariable(pointer(spv::StorageClassOutput, type),
		                                                   spv::StorageClassOutput);
		builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationBuiltIn, builtin);
		return variable;
	};
	uint32_t per_vertex = 0;
	uint32_t clip       = 0;
	uint32_t cull       = 0;
	uint32_t layer      = 0;
	std::vector<uint32_t> interfaces;
	const auto main = builder.AllocateId();
	builder.AddFunction(spv::OpFunction, void_type, main, spv::FunctionControlMaskNone, func_type);
	builder.AddFunction(spv::OpLabel, builder.AllocateId());
	for (uint32_t location = 0; location < outputs.size(); location++) {
		const auto& output = outputs[location];
		const auto  type   = IR::MeshRecordOutputDwords(output.kind) == 4 ? vec4_type
		                     : output.kind == IR::StageOutputKind::Layer   ? u32_type
		                                                                   : f32_type;
		const auto input = builder.DefineGlobalVariable(pointer(spv::StorageClassInput, type),
		                                                spv::StorageClassInput);
		builder.AddAnnotation(spv::OpDecorate, input, spv::DecorationLocation, location);
		const auto value = builder.AllocateId();
		builder.AddFunction(spv::OpLoad, type, value, input);
		uint32_t target = 0;
		switch (output.kind) {
			case IR::StageOutputKind::Position: {
				if (per_vertex == 0) {
					const auto block = builder.DecoratedType(
					    spv::OpTypeStruct,
					    {{spv::OpMemberDecorate, {0, spv::DecorationBuiltIn, spv::BuiltInPosition}},
					     {spv::OpDecorate, {spv::DecorationBlock}}},
					    vec4_type);
					per_vertex = builder.DefineGlobalVariable(pointer(spv::StorageClassOutput, block),
					                                          spv::StorageClassOutput);
				}
				target = builder.AllocateId();
				builder.AddFunction(spv::OpAccessChain, pointer(spv::StorageClassOutput, vec4_type),
				                    target, per_vertex, u32_const(0));
				break;
			}
			case IR::StageOutputKind::Parameter:
				target = builder.DefineGlobalVariable(pointer(spv::StorageClassOutput, vec4_type),
				                                      spv::StorageClassOutput);
				builder.AddAnnotation(spv::OpDecorate, target, spv::DecorationLocation,
				                      output.location);
				break;
			case IR::StageOutputKind::Layer:
				if (layer == 0) {
					builder.RequireCapability(spv::CapabilityShaderLayer);
					layer = builtin_variable(u32_type, spv::BuiltInLayer);
				}
				target = layer;
				break;
			case IR::StageOutputKind::ClipDistance:
			case IR::StageOutputKind::CullDistance: {
				const bool is_clip = output.kind == IR::StageOutputKind::ClipDistance;
				auto&      array   = is_clip ? clip : cull;
				if (array == 0) {
					builder.RequireCapability(is_clip ? spv::CapabilityClipDistance
					                                  : spv::CapabilityCullDistance);
					array = builtin_variable(
					    builder.Type(spv::OpTypeArray, f32_type, u32_const(is_clip ? clip_count : cull_count)),
					    is_clip ? spv::BuiltInClipDistance : spv::BuiltInCullDistance);
				}
				target = builder.AllocateId();
				builder.AddFunction(spv::OpAccessChain, pointer(spv::StorageClassOutput, f32_type),
				                    target, array, u32_const(output.index));
				break;
			}
			default: EXIT("unsupported emulated mesh output kind=%u\n", static_cast<uint32_t>(output.kind));
		}
		builder.AddFunction(spv::OpStore, target, value);
	}
	builder.AddFunction(spv::OpReturn);
	builder.AddFunction(spv::OpFunctionEnd);
	builder.AddEntryPoint(spv::ExecutionModelVertex, main, "main", interfaces);
	return builder.Build();
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv

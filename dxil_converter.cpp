/*
 * Copyright 2019 Hans-Kristian Arntzen for Valve Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "dxil_converter.hpp"
#include <utility>
#include "SpvBuilder.h"
#include "cfg_structurizer.hpp"
#include "node_pool.hpp"
#include "node.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Instructions.h>

namespace DXIL2SPIRV
{
constexpr uint32_t GENERATOR = 1967215134;

struct Converter::Impl
{
	Impl(DXILContainerParser container_parser_, LLVMBCParser bitcode_parser_, SPIRVModule &module_)
		: container_parser(std::move(container_parser_)),
		  bitcode_parser(std::move(bitcode_parser_)),
		  spirv_module(module_)
	{
	}

	DXILContainerParser container_parser;
	LLVMBCParser bitcode_parser;
	SPIRVModule &spirv_module;

	struct BlockMeta
	{
		explicit BlockMeta(llvm::BasicBlock *bb_)
			: bb(bb_)
		{
		}

		llvm::BasicBlock *bb;
		CFGNode *node = nullptr;
	};
	std::vector<std::unique_ptr<BlockMeta>> metas;
	std::unordered_map<llvm::BasicBlock *, BlockMeta *> bb_map;
	std::unordered_map<const llvm::Value *, spv::Id> value_map;

	ConvertedFunction convert_entry_point();
	spv::Id get_id_for_value(const llvm::Value &value, unsigned forced_integer_width = 0);
	spv::Id get_id_for_constant(const llvm::Constant &constant, unsigned forced_width);
	spv::Id get_id_for_undef(const llvm::UndefValue &undef);

	void emit_stage_input_variables();
	void emit_stage_output_variables();

	spv::Id get_type_id(unsigned element_type, unsigned rows, unsigned cols);
	spv::Id get_type_id(const llvm::Type &type);

	std::unordered_map<uint32_t, spv::Id> input_elements_ids;
	std::unordered_map<uint32_t, spv::Id> output_elements_ids;
	void emit_builtin_decoration(spv::Id id, DXIL::Semantic semantic);

	void emit_instruction(CFGNode *block, const llvm::Instruction &instruction);
	void emit_builtin_instruction(CFGNode *block, const llvm::CallInst &instruction);
	void emit_phi_instruction(CFGNode *block, const llvm::PHINode &instruction);
	void emit_binary_instruction(CFGNode *block, const llvm::BinaryOperator &instruction);

	static uint32_t get_constant_operand(const llvm::CallInst &value, unsigned index);
};

Converter::Converter(DXILContainerParser container_parser_, LLVMBCParser bitcode_parser_, SPIRVModule &module_)
{
	impl = std::make_unique<Impl>(std::move(container_parser_), std::move(bitcode_parser_), module_);
}

Converter::~Converter()
{
}

ConvertedFunction Converter::convert_entry_point()
{
	return impl->convert_entry_point();
}

spv::Id Converter::Impl::get_id_for_constant(const llvm::Constant &constant, unsigned forced_width)
{
	auto &builder = spirv_module.get_builder();

	switch (constant.getType()->getTypeID())
	{
	case llvm::Type::TypeID::FloatTyID:
	{
		auto &fp = llvm::cast<llvm::ConstantFP>(constant);
		return builder.makeFloatConstant(fp.getValueAPF().convertToFloat());
	}

	case llvm::Type::TypeID::DoubleTyID:
	{
		auto &fp = llvm::cast<llvm::ConstantFP>(constant);
		return builder.makeDoubleConstant(fp.getValueAPF().convertToDouble());
	}

	case llvm::Type::TypeID::IntegerTyID:
	{
		unsigned integer_width = forced_width ? forced_width : constant.getType()->getIntegerBitWidth();
		switch (integer_width)
		{
		case 32:
			return builder.makeUintConstant(constant.getUniqueInteger().getZExtValue());

		default:
			return 0;
		}
	}

	default:
		return 0;
	}
}

spv::Id Converter::Impl::get_id_for_undef(const llvm::UndefValue &undef)
{
	auto &builder = spirv_module.get_builder();
	switch (undef.getType()->getTypeID())
	{
	case llvm::Type::TypeID::FloatTyID:
	{
		return 0;
	}

	default:
		return 0;
	}
}

spv::Id Converter::Impl::get_id_for_value(const llvm::Value &value, unsigned forced_width)
{
	auto itr = value_map.find(&value);
	if (itr != value_map.end())
		return itr->second;

	spv::Id ret;
	if (auto *constant = llvm::dyn_cast<llvm::Constant>(&value))
		ret = get_id_for_constant(*constant, forced_width);
	else if (auto *undef = llvm::dyn_cast<llvm::UndefValue>(&value))
		ret = get_id_for_undef(*undef);
	else
		ret = spirv_module.allocate_id();

	value_map[&value] = ret;
	return ret;
}

static std::string get_entry_point_name(const llvm::Module &module)
{
	auto *ep_meta = module.getNamedMetadata("dx.entryPoints");
	auto *node = ep_meta->getOperand(0);
	return llvm::cast<llvm::MDString>(node->getOperand(1))->getString();
}

template <typename T = uint32_t>
static T get_constant_metadata(const llvm::MDNode *node, unsigned index)
{
	return T(llvm::cast<llvm::ConstantAsMetadata>(node->getOperand(index))->getValue()->getUniqueInteger().getSExtValue());
}

static void print_shader_model(const llvm::Module &module)
{
	auto *shader_model = module.getNamedMetadata("dx.shaderModel");
	auto *shader_model_node = shader_model->getOperand(0);
	fprintf(stderr, "Profile: %s_%u_%u\n", llvm::cast<llvm::MDString>(shader_model_node->getOperand(0))->getString().data(),
	        get_constant_metadata(shader_model_node, 1),
	        get_constant_metadata(shader_model_node, 2));
}

static std::string get_string_metadata(const llvm::MDNode *node, unsigned index)
{
	return llvm::cast<llvm::MDString>(node->getOperand(index))->getString();
}

spv::Id Converter::Impl::get_type_id(const llvm::Type &type)
{
	auto &builder = spirv_module.get_builder();
	switch (type.getTypeID())
	{
	case llvm::Type::TypeID::FloatTyID:
		return builder.makeFloatType(32);

	default:
		return 0;
	}
}

spv::Id Converter::Impl::get_type_id(unsigned element_type, unsigned rows, unsigned cols)
{
	auto &builder = spirv_module.get_builder();

	spv::Id component_type;
	switch (static_cast<DXIL::ComponentType>(element_type))
	{
	case DXIL::ComponentType::I1:
		component_type = builder.makeBoolType();
		break;

	case DXIL::ComponentType::I16:
		component_type = builder.makeIntegerType(16, true);
		break;

	case DXIL::ComponentType::U16:
		component_type = builder.makeIntegerType(16, false);
		break;

	case DXIL::ComponentType::I32:
		component_type = builder.makeIntegerType(32, true);
		break;

	case DXIL::ComponentType::U32:
		component_type = builder.makeIntegerType(32, false);
		break;

	case DXIL::ComponentType::I64:
		component_type = builder.makeIntegerType(64, true);
		break;

	case DXIL::ComponentType::U64:
		component_type = builder.makeIntegerType(64, false);
		break;

	case DXIL::ComponentType::F16:
		component_type = builder.makeFloatType(16);
		break;

	case DXIL::ComponentType::F32:
		component_type = builder.makeFloatType(32);
		break;

	case DXIL::ComponentType::F64:
		component_type = builder.makeFloatType(64);
		break;

	default:
		return 0;
	}

	if (rows == 1 && cols == 1)
		return component_type;
	else if (rows == 1)
	{
		auto vector_type = builder.makeVectorType(component_type, cols);
		return vector_type;
	}
	else
	{
		auto matrix_type = builder.makeMatrixType(component_type, rows, cols);
		return matrix_type;
	}
}

void Converter::Impl::emit_stage_output_variables()
{
	auto &module = bitcode_parser.get_module();

	auto *ep_meta = module.getNamedMetadata("dx.entryPoints");
	auto *node = ep_meta->getOperand(0);
	auto &signature = node->getOperand(2);
	auto *outputs_node = llvm::cast<llvm::MDNode>(llvm::cast<llvm::MDNode>(signature)->getOperand(1));

	auto &builder = spirv_module.get_builder();

	unsigned location = 0;

	for (unsigned i = 0; i < outputs_node->getNumOperands(); i++)
	{
		auto *output = llvm::cast<llvm::MDNode>(outputs_node->getOperand(i));
		auto element_id = get_constant_metadata(output, 0);
		auto semantic_name = get_string_metadata(output, 1);
		auto element_type = get_constant_metadata(output, 2);
		auto system_value = static_cast<DXIL::Semantic>(get_constant_metadata(output, 3));

		// Semantic index?
		//auto interpolation = get_constant_metadata(input, 5);
		auto rows = get_constant_metadata(output, 6);
		auto cols = get_constant_metadata(output, 7);

#if 0
		auto start_row = get_constant_metadata(input, 8);
		auto col = get_constant_metadata(input, 9);
#endif

#if 0
		fprintf(stderr, "Semantic output %u: %s\n", element_id, semantic_name.c_str());
		fprintf(stderr, "  Type: %u\n", element_type);
		fprintf(stderr, "  System value: %u\n", system_value);
		fprintf(stderr, "  Interpolation: %u\n", interpolation);
		fprintf(stderr, "  Rows: %u\n", rows);
		fprintf(stderr, "  Cols: %u\n", cols);
		fprintf(stderr, "  Start row: %u\n", start_row);
		fprintf(stderr, "  Col: %u\n", col);
#endif

		spv::Id type_id = get_type_id(element_type, rows, cols);
		spv::Id variable_id = builder.createVariable(spv::StorageClassOutput, type_id, semantic_name.c_str());
		output_elements_ids[element_id] = variable_id;

		if (system_value != DXIL::Semantic::User)
			emit_builtin_decoration(variable_id, system_value);
		else
		{
			builder.addDecoration(variable_id, spv::DecorationLocation, location);
			location += rows;
		}

		spirv_module.get_entry_point()->addIdOperand(variable_id);
	}
}

void Converter::Impl::emit_builtin_decoration(spv::Id id, DXIL::Semantic semantic)
{
	auto &builder = spirv_module.get_builder();
	switch (semantic)
	{
	case DXIL::Semantic::Position:
		builder.addDecoration(id, spv::DecorationBuiltIn, spv::BuiltInPosition);
		break;

	default:
		break;
	}
}

void Converter::Impl::emit_stage_input_variables()
{
	auto &module = bitcode_parser.get_module();

	auto *ep_meta = module.getNamedMetadata("dx.entryPoints");
	auto *node = ep_meta->getOperand(0);
	auto &signature = node->getOperand(2);
	auto *inputs_node = llvm::cast<llvm::MDNode>(llvm::cast<llvm::MDNode>(signature)->getOperand(0));

	auto &builder = spirv_module.get_builder();

	unsigned location = 0;

	for (unsigned i = 0; i < inputs_node->getNumOperands(); i++)
	{
		auto *input = llvm::cast<llvm::MDNode>(inputs_node->getOperand(i));
		auto element_id = get_constant_metadata(input, 0);
		auto semantic_name = get_string_metadata(input, 1);
		auto element_type = get_constant_metadata(input, 2);
		auto system_value = static_cast<DXIL::Semantic>(get_constant_metadata(input, 3));

		// Semantic index?
		//auto interpolation = get_constant_metadata(input, 5);
		auto rows = get_constant_metadata(input, 6);
		auto cols = get_constant_metadata(input, 7);

#if 0
		auto start_row = get_constant_metadata(input, 8);
		auto col = get_constant_metadata(input, 9);
#endif

#if 0
		fprintf(stderr, "Semantic output %u: %s\n", element_id, semantic_name.c_str());
		fprintf(stderr, "  Type: %u\n", element_type);
		fprintf(stderr, "  System value: %u\n", system_value);
		fprintf(stderr, "  Interpolation: %u\n", interpolation);
		fprintf(stderr, "  Rows: %u\n", rows);
		fprintf(stderr, "  Cols: %u\n", cols);
		fprintf(stderr, "  Start row: %u\n", start_row);
		fprintf(stderr, "  Col: %u\n", col);
#endif

		spv::Id type_id = get_type_id(element_type, rows, cols);
		spv::Id variable_id = builder.createVariable(spv::StorageClassInput, type_id, semantic_name.c_str());
		input_elements_ids[element_id] = variable_id;

		if (system_value != DXIL::Semantic::User)
			emit_builtin_decoration(variable_id, system_value);
		else
		{
			builder.addDecoration(variable_id, spv::DecorationLocation, location);
			location += rows;
		}

		spirv_module.get_entry_point()->addIdOperand(variable_id);
	}
}

uint32_t Converter::Impl::get_constant_operand(const llvm::CallInst &value, unsigned index)
{
	auto *constant = llvm::cast<llvm::Constant>(value.getOperand(index));
	return uint32_t(constant->getUniqueInteger().getZExtValue());
}

void Converter::Impl::emit_builtin_instruction(CFGNode *block, const llvm::CallInst &instruction)
{
	auto &builder = spirv_module.get_builder();
	// DXIL built-in call.

	// The opcode is encoded as a constant integer.
	auto opcode = static_cast<DXIL::Op>(get_constant_operand(instruction, 0));

	switch (opcode)
	{
	case DXIL::Op::LoadInput:
	{
		uint32_t var_id = input_elements_ids[get_constant_operand(instruction, 1)];
		uint32_t ptr_id;
		Operation op;

		uint32_t num_rows = builder.getNumTypeComponents(builder.getDerefTypeId(var_id));

		if (num_rows > 1)
		{
			ptr_id = spirv_module.allocate_id();

			op.op = Op::InBoundsAccessChain;
			op.id = ptr_id;
			op.type_id = get_type_id(*instruction.getType());
			op.type_id = builder.makePointer(spv::StorageClassInput, op.type_id);
			op.arguments = {
				var_id,
				get_id_for_value(*instruction.getOperand(3), 32)
			};
			assert(op.arguments[0]);
			assert(op.arguments[1]);

			block->ir.operations.push_back(std::move(op));
		}
		else
			ptr_id = var_id;

		op = {};
		op.op = Op::Load;
		op.id = get_id_for_value(instruction);
		op.type_id = get_type_id(*instruction.getType());
		op.arguments = { ptr_id };
		assert(op.arguments[0]);

		block->ir.operations.push_back(std::move(op));
		break;
	}

	case DXIL::Op::StoreOutput:
	{
		uint32_t var_id = output_elements_ids[get_constant_operand(instruction, 1)];
		uint32_t ptr_id;
		Operation op;

		uint32_t num_rows = builder.getNumTypeComponents(builder.getDerefTypeId(var_id));

		if (num_rows > 1)
		{
			ptr_id = spirv_module.allocate_id();

			op.op = Op::InBoundsAccessChain;
			op.id = ptr_id;
			op.type_id = builder.getScalarTypeId(builder.getDerefTypeId(var_id));
			op.type_id = builder.makePointer(spv::StorageClassOutput, op.type_id);
			op.arguments = {
				var_id,
				get_id_for_value(*instruction.getOperand(3), 32)
			};
			assert(op.arguments[0]);
			assert(op.arguments[1]);

			block->ir.operations.push_back(std::move(op));
		}
		else
			ptr_id = var_id;

		op = {};
		op.op = Op::Store;
		op.arguments = {
			ptr_id,
			get_id_for_value(*instruction.getOperand(4))
		};
		assert(op.arguments[0]);
		assert(op.arguments[1]);

		block->ir.operations.push_back(std::move(op));
		break;
	}

	default:
		break;
	}
}

void Converter::Impl::emit_phi_instruction(CFGNode *block, const llvm::PHINode &instruction)
{
	PHI phi;
	phi.id = get_id_for_value(instruction);
	phi.type_id = spirv_module.get_builder().makeIntegerType(32, false);

	unsigned count = instruction.getNumIncomingValues();
	for (unsigned i = 0; i < count; i++)
	{
		IncomingValue incoming = {};
		incoming.block = bb_map[instruction.getIncomingBlock(i)]->node;
		auto *value = instruction.getIncomingValue(i);
		if (llvm::isa<llvm::PHINode>(value))
			incoming.id = get_id_for_value(*value);
		else
		{
			// Just invent something ...
			incoming.id = spirv_module.get_builder().makeUintConstant(phi.id);
		}
		phi.incoming.push_back(incoming);
	}

	block->ir.phi.push_back(std::move(phi));
}

void Converter::Impl::emit_binary_instruction(CFGNode *block, const llvm::BinaryOperator &instruction)
{
	Operation op;
	op.id = get_id_for_value(instruction);
	op.type_id = get_type_id(*instruction.getType());

	uint32_t id0 = get_id_for_value(*instruction.getOperand(0));
	uint32_t id1 = get_id_for_value(*instruction.getOperand(1));
	op.arguments = { id0, id1 };

	switch (instruction.getOpcode())
	{
	case llvm::BinaryOperator::BinaryOps::FAdd:
		op.op = Op::FAdd;
		break;

	default:
		op.op = Op::Nop;
		break;
	}

	block->ir.operations.push_back(std::move(op));
}

void Converter::Impl::emit_instruction(CFGNode *block, const llvm::Instruction &instruction)
{
	if (auto *call_inst = llvm::dyn_cast<llvm::CallInst>(&instruction))
	{
		auto *called_function = call_inst->getCalledFunction();
		if (strncmp(called_function->getName().data(), "dx.op", 5) == 0)
		{
			emit_builtin_instruction(block, *call_inst);
		}
		else
		{
			fprintf(stderr, "Normal function call ...\n");
		}
	}
	else if (auto *binary_inst = llvm::dyn_cast<llvm::BinaryOperator>(&instruction))
	{
		emit_binary_instruction(block, *binary_inst);
	}
	else if (auto *phi_inst = llvm::dyn_cast<llvm::PHINode>(&instruction))
	{
		emit_phi_instruction(block, *phi_inst);
	}
}

ConvertedFunction Converter::Impl::convert_entry_point()
{
	ConvertedFunction result;
	result.node_pool = std::make_unique<CFGNodePool>();
	auto &pool = *result.node_pool;

	auto *module = &bitcode_parser.get_module();

	print_shader_model(*module);
	fprintf(stderr, "Entry point: %s\n", get_entry_point_name(*module).c_str());

	emit_stage_input_variables();
	emit_stage_output_variables();

	llvm::Function *func = module->getFunction(get_entry_point_name(*module));
	assert(func);

	auto *entry = &func->getEntryBlock();
	auto entry_meta = std::make_unique<BlockMeta>(entry);
	bb_map[entry] = entry_meta.get();
	result.entry = pool.create_node();
	bb_map[entry]->node = result.entry;
	result.entry->name = entry->getName().data();
	result.entry->name += ".entry";
	metas.push_back(std::move(entry_meta));

	std::vector<llvm::BasicBlock *> to_process;
	std::vector<llvm::BasicBlock *> processing;
	to_process.push_back(entry);

	// Traverse the CFG and register all blocks in the pool.
	while (!to_process.empty())
	{
		std::swap(to_process, processing);
		for (auto *block : processing)
		{
			for (auto itr = succ_begin(block); itr != succ_end(block); ++itr)
			{
				auto *succ = *itr;
				if (!bb_map.count(succ))
				{
					to_process.push_back(succ);
					auto succ_meta = std::make_unique<BlockMeta>(succ);
					bb_map[succ] = succ_meta.get();
					auto *succ_node = pool.create_node();
					bb_map[succ]->node = succ_node;
					succ_node->name = succ->getName().data();
					metas.push_back(std::move(succ_meta));
				}

				bb_map[block]->node->add_branch(bb_map[succ]->node);
			}
		}
		processing.clear();
	}

	for (auto &key_value : bb_map)
	{
		llvm::BasicBlock *bb = key_value.first;
		CFGNode *node = key_value.second->node;

		// Scan opcodes.
		for (auto &instruction : *bb)
			emit_instruction(node, instruction);

		auto *instruction = bb->getTerminator();
		if (auto *inst = llvm::dyn_cast<llvm::BranchInst>(instruction))
		{
			if (inst->isConditional())
			{
				node->ir.terminator.type = Terminator::Type::Condition;
				node->ir.terminator.conditional_id = spirv_module.get_builder().makeBoolConstant(true);
				assert(inst->getNumSuccessors() == 2);
				node->ir.terminator.true_block = bb_map[inst->getSuccessor(0)]->node;
				node->ir.terminator.false_block = bb_map[inst->getSuccessor(1)]->node;
			}
			else
			{
				node->ir.terminator.type = Terminator::Type::Branch;
				assert(inst->getNumSuccessors() == 1);
				node->ir.terminator.direct_block = bb_map[inst->getSuccessor(0)]->node;
			}
		}
		else if (auto *inst = llvm::dyn_cast<llvm::ReturnInst>(instruction))
		{
			node->ir.terminator.type = Terminator::Type::Return;
			if (inst->getReturnValue())
				node->ir.terminator.return_value = spirv_module.get_builder().makeBoolConstant(true);
		}
		else if (llvm::isa<llvm::UnreachableInst>(instruction))
		{
			node->ir.terminator.type = Terminator::Type::Unreachable;
		}
		else
			fprintf(stderr, "Unsupported terminator ...\n");
	}

#if 0
		BasicBlock &block = func->getEntryBlock();

		for (auto itr = succ_begin(&block); itr != succ_end(&block); ++itr)
		{
			fprintf(stderr, "Successor!\n");
		}

		//block.print(errs());

		for (auto itr = block.begin(); itr != block.end(); ++itr)
		{
			Instruction &inst = *itr;
			if (isa<llvm::CallInst>(inst))
			{
				fprintf(stderr, "Call!\n");
				auto &call = cast<llvm::CallInst>(inst);
				fprintf(stderr, "Calling %s\n", call.getCalledFunction()->getName().data());
				auto *constant = cast<llvm::ConstantInt>(call.getOperand(0));
				uint32_t value = constant->getZExtValue();
				fprintf(stderr, "Calling opcode: %u\n", value);

				call.setMetadata(0, MDNode::get(call.getContext(), MDString::get(call.getContext(), "OHAI")));
				fflush(stderr);
				cast<llvm::MDString>(call.getMetadata(0)->getOperand(0))->print(errs());
				fflush(stderr);
				//fprintf(stderr, "Value name: %s\n", call.getName().data());
			}
			else if (isa<llvm::ReturnInst>(inst))
				fprintf(stderr, "Return!\n");
			else if (isa<llvm::SelectInst>(inst))
				fprintf(stderr, "Select instruction!\n");
			else if (isa<llvm::CmpInst>(inst))
				fprintf(stderr, "Compare instruction!\n");
			else if (isa<llvm::BinaryOperator>(inst))
			{
				fprintf(stderr, "Binary operator!\n");
				auto &binop = cast<llvm::BinaryOperator>(inst);
				switch (binop.getOpcode())
				{
				case BinaryOperator::FAdd:
					fprintf(stderr, "FADD!\n");
					break;

				case BinaryOperator::FMul:
					fprintf(stderr, "FMul!\n");
					break;

				default:
					break;
				}
			}
			else
				fprintf(stderr, "? ...\n");
		}
	}
#endif

	return result;
}
} // namespace DXIL2SPIRV
/*
 * Copyright 2019-2020 Hans-Kristian Arntzen for Valve Corporation
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

#include "dxil_spirv_c.h"
#include "dxil_parser.hpp"
#include "dxil_converter.hpp"
#include "llvm_bitcode_parser.hpp"
#include "logging.hpp"
#include <new>

#include "spirv-tools/libspirv.hpp"

using namespace DXIL2SPIRV;

void dxil_spv_get_version(unsigned *major, unsigned *minor, unsigned *patch)
{
	*major = DXIL_SPV_API_VERSION_MAJOR;
	*minor = DXIL_SPV_API_VERSION_MINOR;
	*patch = DXIL_SPV_API_VERSION_PATCH;
}

struct dxil_spv_parsed_blob_s
{
	LLVMBCParser bc;
};

struct dxil_spv_converter_s : ResourceRemappingInterface
{
	explicit dxil_spv_converter_s(LLVMBCParser &bc_parser)
		: converter(bc_parser, module)
	{
	}
	SPIRVModule module;
	Converter converter;
	std::vector<uint32_t> spirv;

	bool remap_srv(const D3DBinding &binding, VulkanBinding &vk_binding) override
	{
		if (srv_remapper)
		{
			const dxil_spv_d3d_binding c_binding = { binding.resource_index, binding.register_space, binding.register_index, binding.range_size };
			dxil_spv_vulkan_binding c_vk_binding = {};
			return bool(srv_remapper(srv_userdata, &c_binding, &c_vk_binding));
		}
		else
		{
			vk_binding.bindless.use_heap = false;
			vk_binding.descriptor_set = binding.register_space;
			vk_binding.binding = binding.register_index;
			return true;
		}
	}

	bool remap_sampler(const D3DBinding &binding, VulkanBinding &vk_binding) override
	{
		if (sampler_remapper)
		{
			const dxil_spv_d3d_binding c_binding = { binding.resource_index, binding.register_space, binding.register_index, binding.range_size };
			dxil_spv_vulkan_binding c_vk_binding = {};
			return bool(sampler_remapper(sampler_userdata, &c_binding, &c_vk_binding));
		}
		else
		{
			vk_binding.bindless.use_heap = false;
			vk_binding.descriptor_set = binding.register_space;
			vk_binding.binding = binding.register_index;
			return true;
		}
	}

	bool remap_uav(const D3DUAVBinding &binding, VulkanUAVBinding &vk_binding) override
	{
		if (uav_remapper)
		{
			dxil_spv_uav_d3d_binding c_binding = {
				{ binding.binding.resource_index, binding.binding.register_space, binding.binding.resource_index, binding.binding.range_size },
				binding.counter ? DXIL_SPV_TRUE : DXIL_SPV_FALSE
			};

			dxil_spv_uav_vulkan_binding c_vk_binding = {};
			return bool(uav_remapper(uav_userdata, &c_binding, &c_vk_binding));
		}
		else
		{
			vk_binding.buffer_binding.bindless.use_heap = false;
			vk_binding.counter_binding.bindless.use_heap = false;
			vk_binding.buffer_binding.descriptor_set = binding.binding.register_space;
			vk_binding.buffer_binding.binding = binding.binding.register_index;
			vk_binding.counter_binding.descriptor_set = binding.binding.register_space + 1;
			vk_binding.counter_binding.binding = binding.binding.register_index;
			return true;
		}
	}

	bool remap_cbv(const D3DBinding &binding, VulkanCBVBinding &vk_binding) override
	{
		if (cbv_remapper)
		{
			const dxil_spv_d3d_binding c_binding = { binding.resource_index, binding.register_space, binding.register_index, binding.range_size };
			dxil_spv_cbv_vulkan_binding c_vk_binding = {};
			return bool(cbv_remapper(cbv_userdata, &c_binding, &c_vk_binding));
		}
		else
		{
			vk_binding.buffer.bindless.use_heap = false;
			vk_binding.buffer.descriptor_set = binding.register_space;
			vk_binding.buffer.binding = binding.register_index;
			return true;
		}
	}

	unsigned get_root_constant_word_count() override
	{
		return root_constant_word_count;
	}

	dxil_spv_srv_sampler_remapper_cb srv_remapper = nullptr;
	void *srv_userdata = nullptr;

	dxil_spv_srv_sampler_remapper_cb sampler_remapper = nullptr;
	void *sampler_userdata = nullptr;

	dxil_spv_uav_remapper_cb uav_remapper = nullptr;
	void *uav_userdata = nullptr;

	dxil_spv_cbv_remapper_cb cbv_remapper = nullptr;
	void *cbv_userdata = nullptr;

	unsigned root_constant_word_count = 0;
};

dxil_spv_result dxil_spv_parse_dxil_blob(const void *data, size_t size, dxil_spv_parsed_blob *blob)
{
	auto *parsed = new (std::nothrow) dxil_spv_parsed_blob_s;
	if (!parsed)
		return DXIL_SPV_ERROR_OUT_OF_MEMORY;

	DXILContainerParser parser;
	if (!parser.parse_container(data, size))
	{
		delete parsed;
		return DXIL_SPV_ERROR_PARSER;
	}

	if (!parsed->bc.parse(parser.get_bitcode_data(), parser.get_bitcode_size()))
	{
		delete parsed;
		return DXIL_SPV_ERROR_PARSER;
	}

	*blob = parsed;
	return DXIL_SPV_SUCCESS;
}

dxil_spv_result dxil_spv_parse_dxil(const void *data, size_t size, dxil_spv_parsed_blob *blob)
{
	auto *parsed = new (std::nothrow) dxil_spv_parsed_blob_s;
	if (!parsed)
		return DXIL_SPV_ERROR_OUT_OF_MEMORY;

	if (!parsed->bc.parse(data, size))
	{
		delete parsed;
		return DXIL_SPV_ERROR_PARSER;
	}

	*blob = parsed;
	return DXIL_SPV_SUCCESS;
}

void dxil_spv_parsed_blob_dump_llvm_ir(dxil_spv_parsed_blob blob)
{
	auto &module = blob->bc.get_module();
	module.print(llvm::errs(), nullptr);
}

void dxil_spv_parsed_blob_free(dxil_spv_parsed_blob blob)
{
	delete blob;
}

dxil_spv_result dxil_spv_create_converter(dxil_spv_parsed_blob blob, dxil_spv_converter *converter)
{
	auto *conv = new (std::nothrow) dxil_spv_converter_s(blob->bc);
	if (!conv)
		return DXIL_SPV_ERROR_OUT_OF_MEMORY;

	*converter = conv;
	return DXIL_SPV_SUCCESS;
}

void dxil_spv_converter_free(dxil_spv_converter converter)
{
	delete converter;
}

dxil_spv_result dxil_spv_converter_run(dxil_spv_converter converter)
{
	auto entry_point = converter->converter.convert_entry_point();
	if (entry_point.entry == nullptr)
	{
		LOGE("Failed to convert function.\n");
		return DXIL_SPV_ERROR_GENERIC;
	}

	{
		DXIL2SPIRV::CFGStructurizer structurizer(entry_point.entry, *entry_point.node_pool, converter->module);
		structurizer.run();
		converter->module.emit_entry_point_function_body(structurizer);
	}

	for (auto &leaf : entry_point.leaf_functions)
	{
		if (!leaf.entry)
		{
			LOGE("Leaf function is nullptr!\n");
			return DXIL_SPV_ERROR_GENERIC;
		}
		DXIL2SPIRV::CFGStructurizer structurizer(leaf.entry, *entry_point.node_pool, converter->module);
		structurizer.run();
		converter->module.emit_leaf_function_body(leaf.func, structurizer);
	}

	if (!converter->module.finalize_spirv(converter->spirv))
	{
		LOGE("Failed to finalize SPIR-V.\n");
		return DXIL_SPV_ERROR_GENERIC;
	}

	return DXIL_SPV_SUCCESS;
}

dxil_spv_result dxil_spv_converter_validate_spirv(dxil_spv_converter converter)
{
	if (converter->spirv.empty())
		return DXIL_SPV_ERROR_UNSUPPORTED_FEATURE;

	spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
	tools.SetMessageConsumer([](spv_message_level_t, const char *, const spv_position_t &, const char *message) {
		LOGE("SPIRV-Tools message: %s\n", message);
	});
	return tools.Validate(converter->spirv) ? DXIL_SPV_SUCCESS : DXIL_SPV_ERROR_FAILED_VALIDATION;
}

dxil_spv_result dxil_spv_converter_get_compiled_spirv(dxil_spv_converter converter,
                                                      dxil_spv_compiled_spirv *compiled)
{
	if (converter->spirv.empty())
		return DXIL_SPV_ERROR_GENERIC;

	compiled->data = converter->spirv.data();
	compiled->size = converter->spirv.size() * sizeof(uint32_t);
	return DXIL_SPV_SUCCESS;
}

void dxil_spv_converter_set_srv_remapper(
		dxil_spv_converter converter,
		dxil_spv_srv_sampler_remapper_cb remapper,
		void *userdata)
{
	converter->srv_remapper = remapper;
	converter->srv_userdata = userdata;
}

void dxil_spv_converter_set_sampler_remapper(
		dxil_spv_converter converter,
		dxil_spv_srv_sampler_remapper_cb remapper,
		void *userdata)
{
	converter->sampler_remapper = remapper;
	converter->sampler_userdata = userdata;
}

void dxil_spv_converter_set_root_constant_word_count(dxil_spv_converter converter,
                                                     unsigned num_words)
{
	converter->root_constant_word_count = num_words;
}

void dxil_spv_converter_set_uav_remapper(
		dxil_spv_converter converter,
		dxil_spv_uav_remapper_cb remapper,
		void *userdata)
{
	converter->uav_remapper = remapper;
	converter->uav_userdata = userdata;
}

void dxil_spv_converter_set_cbv_remapper(
		dxil_spv_converter converter,
		dxil_spv_cbv_remapper_cb remapper,
		void *userdata)
{
	converter->cbv_remapper = remapper;
	converter->cbv_userdata = userdata;
}


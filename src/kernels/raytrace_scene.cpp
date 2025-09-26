#include "raytrace_scene.h"
#include "gpu_mesh.h"

#include "lib.h"

#include <cassert>
#include <cstring>

void Raytrace_Scene::create(const GPU_Mesh& gpu_mesh, VkImageView texture_view, VkSampler sampler) {
    descriptor_buffer_properties = VkPhysicalDeviceDescriptorBufferPropertiesEXT{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT };

    VkPhysicalDeviceProperties2 physical_device_properties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    physical_device_properties.pNext = &descriptor_buffer_properties;
    vkGetPhysicalDeviceProperties2(vk.physical_device, &physical_device_properties);

    uniform_buffer = vk_create_mapped_buffer(static_cast<VkDeviceSize>(sizeof(Matrix3x4)),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &(void*&)mapped_uniform_buffer, "rt_uniform_buffer");

    accelerator = create_intersection_accelerator({gpu_mesh});
    create_pipeline(gpu_mesh, texture_view, sampler);

    // shader binding table
    {
        uint32_t miss_offset = round_up(properties.shaderGroupHandleSize /* raygen slot*/, properties.shaderGroupBaseAlignment);
        uint32_t hit_offset = round_up(miss_offset + properties.shaderGroupHandleSize /* miss slot */, properties.shaderGroupBaseAlignment);
        uint32_t sbt_buffer_size = hit_offset + properties.shaderGroupHandleSize;

        std::vector<uint8_t> data(sbt_buffer_size);
        VK_CHECK(vkGetRayTracingShaderGroupHandlesKHR(vk.device, pipeline, 0, 1, properties.shaderGroupHandleSize, data.data() + 0)); // raygen slot
        VK_CHECK(vkGetRayTracingShaderGroupHandlesKHR(vk.device, pipeline, 1, 1, properties.shaderGroupHandleSize, data.data() + miss_offset)); // miss slot
        VK_CHECK(vkGetRayTracingShaderGroupHandlesKHR(vk.device, pipeline, 2, 1, properties.shaderGroupHandleSize, data.data() + hit_offset)); // hit slot
        const VkBufferUsageFlags usage = VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        shader_binding_table = vk_create_buffer_with_alignment(sbt_buffer_size, usage, properties.shaderGroupBaseAlignment, data.data(), "shader_binding_table");
    }
}

void Raytrace_Scene::destroy() {
    descriptor_buffer.destroy();
    uniform_buffer.destroy();
    shader_binding_table.destroy();
    accelerator.destroy();

    vkDestroyDescriptorSetLayout(vk.device, descriptor_set_layout, nullptr);
    vkDestroyPipelineLayout(vk.device, pipeline_layout, nullptr);
    vkDestroyPipeline(vk.device, pipeline, nullptr);
}

void Raytrace_Scene::update_output_image_descriptor(VkImageView output_image_view) {
    // Write descriptor 0 (output image)
    {
        VkDescriptorImageInfo image_info;
        image_info.imageView = output_image_view;
        image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorGetInfoEXT descriptor_info{ VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT };
        descriptor_info.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        descriptor_info.data.pStorageImage = &image_info;

        VkDeviceSize offset;
        vkGetDescriptorSetLayoutBindingOffsetEXT(vk.device, descriptor_set_layout, 0, &offset);
        vkGetDescriptorEXT(vk.device, &descriptor_info, descriptor_buffer_properties.storageImageDescriptorSize,
            (uint8_t*)mapped_descriptor_buffer_ptr + offset);
    }
}

void Raytrace_Scene::update(const Matrix3x4& model_transform, const Matrix3x4& camera_to_world_transform) {
    assert(accelerator.bottom_level_accels.size() == 1);

    VkAccelerationStructureInstanceKHR& instance = *accelerator.mapped_instance_buffer;
    memcpy(&instance.transform.matrix[0][0], &model_transform.a[0][0], 12 * sizeof(float));
    instance.instanceCustomIndex = 0;
    instance.mask = 0xff;
    instance.instanceShaderBindingTableRecordOffset = 0;
    instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    instance.accelerationStructureReference = accelerator.bottom_level_accels[0].device_address;

    memcpy(mapped_uniform_buffer, &camera_to_world_transform, sizeof(camera_to_world_transform));
}

void Raytrace_Scene::create_pipeline(const GPU_Mesh& gpu_mesh, VkImageView texture_view, VkSampler sampler) {
    descriptor_set_layout = Vk_Descriptor_Set_Layout()
        .storage_image (0, VK_SHADER_STAGE_RAYGEN_BIT_KHR)
        .accelerator (1, VK_SHADER_STAGE_RAYGEN_BIT_KHR)
        .uniform_buffer (2, VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR)
        .storage_buffer (3, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR)
        .storage_buffer (4, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR)
        .sampled_image (5, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR)
        .sampler (6, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR)
        .create ("rt_set_layout");

    pipeline_layout = vk_create_pipeline_layout(
        { descriptor_set_layout },
        { VkPushConstantRange{VK_SHADER_STAGE_RAYGEN_BIT_KHR, 0, 4}, 
          VkPushConstantRange{VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, 4, 4} },
        "rt_pipeline_layout"
    );

    // pipeline
    {
        Vk_Shader_Module rgen_shader(get_resource_path("spirv/rt_mesh.rgen.spv"));
        Vk_Shader_Module miss_shader(get_resource_path("spirv/rt_mesh.rmiss.spv"));
        Vk_Shader_Module chit_shader(get_resource_path("spirv/rt_mesh.rchit.spv"));

        VkPipelineShaderStageCreateInfo stage_infos[3] {};
        stage_infos[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage_infos[0].stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
        stage_infos[0].module = rgen_shader.handle;
        stage_infos[0].pName = "main";

        stage_infos[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage_infos[1].stage = VK_SHADER_STAGE_MISS_BIT_KHR;
        stage_infos[1].module = miss_shader.handle;
        stage_infos[1].pName = "main";

        stage_infos[2].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage_infos[2].stage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
        stage_infos[2].module = chit_shader.handle;
        stage_infos[2].pName = "main";

        VkRayTracingShaderGroupCreateInfoKHR shader_groups[3];
        {
            auto& group = shader_groups[0];
            group = VkRayTracingShaderGroupCreateInfoKHR { VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR };
            group.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
            group.generalShader = 0;
            group.closestHitShader = VK_SHADER_UNUSED_KHR;
            group.anyHitShader = VK_SHADER_UNUSED_KHR;
            group.intersectionShader = VK_SHADER_UNUSED_KHR;
        }
        {
            auto& group = shader_groups[1];
            group = VkRayTracingShaderGroupCreateInfoKHR { VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR };
            group.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
            group.generalShader = 1;
            group.closestHitShader = VK_SHADER_UNUSED_KHR;
            group.anyHitShader = VK_SHADER_UNUSED_KHR;
            group.intersectionShader = VK_SHADER_UNUSED_KHR;
        }
        {
            auto& group = shader_groups[2];
            group = VkRayTracingShaderGroupCreateInfoKHR { VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR };
            group.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
            group.generalShader = VK_SHADER_UNUSED_KHR;
            group.closestHitShader = 2;
            group.anyHitShader = VK_SHADER_UNUSED_KHR;
            group.intersectionShader = VK_SHADER_UNUSED_KHR;
        }

        VkRayTracingPipelineCreateInfoKHR create_info { VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR };
        create_info.flags =
            VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT |
            VK_PIPELINE_CREATE_RAY_TRACING_NO_NULL_CLOSEST_HIT_SHADERS_BIT_KHR |
            VK_PIPELINE_CREATE_RAY_TRACING_NO_NULL_MISS_SHADERS_BIT_KHR;
        create_info.stageCount = (uint32_t)std::size(stage_infos);
        create_info.pStages = stage_infos;
        create_info.groupCount = (uint32_t)std::size(shader_groups);
        create_info.pGroups = shader_groups;
        create_info.maxPipelineRayRecursionDepth = 1;
        create_info.layout = pipeline_layout;
        VK_CHECK(vkCreateRayTracingPipelinesKHR(vk.device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &create_info, nullptr, &pipeline));
    }

    // Descriptor buffer.
    {
        VkDeviceSize layout_size_in_bytes = 0;
        vkGetDescriptorSetLayoutSizeEXT(vk.device, descriptor_set_layout, &layout_size_in_bytes);

        descriptor_buffer = vk_create_mapped_buffer(
            layout_size_in_bytes,
            VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT | VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT,
            &mapped_descriptor_buffer_ptr, "ray_tracing_descriptor_buffer"
        );
        assert(descriptor_buffer.device_address % descriptor_buffer_properties.descriptorBufferOffsetAlignment == 0);

        // Write descriptor 1 (acceleration structure)
        {
            VkDescriptorGetInfoEXT descriptor_info{ VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT };
            descriptor_info.type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            descriptor_info.data.accelerationStructure = accelerator.top_level_accel.buffer.device_address;

            VkDeviceSize offset;
            vkGetDescriptorSetLayoutBindingOffsetEXT(vk.device, descriptor_set_layout, 1, &offset);
            vkGetDescriptorEXT(vk.device, &descriptor_info, descriptor_buffer_properties.accelerationStructureDescriptorSize,
                (uint8_t*)mapped_descriptor_buffer_ptr + offset);
        }
        // Write descriptor 2 (uniform buffer)
        {
            VkDescriptorAddressInfoEXT address_info{ VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT };
            address_info.address = uniform_buffer.device_address;
            address_info.range = sizeof(Matrix3x4);

            VkDescriptorGetInfoEXT descriptor_info{ VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT };
            descriptor_info.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            descriptor_info.data.pUniformBuffer = &address_info;

            VkDeviceSize offset;
            vkGetDescriptorSetLayoutBindingOffsetEXT(vk.device, descriptor_set_layout, 2, &offset);
            vkGetDescriptorEXT(vk.device, &descriptor_info, descriptor_buffer_properties.uniformBufferDescriptorSize,
                (uint8_t*)mapped_descriptor_buffer_ptr + offset);
        }
        // Write descriptor 3 (index buffer)
        {
            VkDescriptorAddressInfoEXT address_info{ VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT };
            address_info.address = gpu_mesh.index_buffer.device_address;
            address_info.range = gpu_mesh.index_count * 4 /*VK_INDEX_TYPE_UINT32*/;

            VkDescriptorGetInfoEXT descriptor_info{ VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT };
            descriptor_info.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            descriptor_info.data.pStorageBuffer = &address_info;

            VkDeviceSize offset;
            vkGetDescriptorSetLayoutBindingOffsetEXT(vk.device, descriptor_set_layout, 3, &offset);
            vkGetDescriptorEXT(vk.device, &descriptor_info, descriptor_buffer_properties.storageBufferDescriptorSize,
                (uint8_t*)mapped_descriptor_buffer_ptr + offset);
        }
        // Write descriptor 4 (vertex buffer)
        {
            VkDescriptorAddressInfoEXT address_info{ VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT };
            address_info.address = gpu_mesh.vertex_buffer.device_address;
            address_info.range = gpu_mesh.vertex_count * sizeof(Vertex);

            VkDescriptorGetInfoEXT descriptor_info{ VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT };
            descriptor_info.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            descriptor_info.data.pStorageBuffer = &address_info;

            VkDeviceSize offset;
            vkGetDescriptorSetLayoutBindingOffsetEXT(vk.device, descriptor_set_layout, 4, &offset);
            vkGetDescriptorEXT(vk.device, &descriptor_info, descriptor_buffer_properties.storageBufferDescriptorSize,
                (uint8_t*)mapped_descriptor_buffer_ptr + offset);
        }
        // Write descriptor 5 (sampled image)
        {
            VkDescriptorImageInfo image_info;
            image_info.imageView = texture_view;
            image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkDescriptorGetInfoEXT descriptor_info{ VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT };
            descriptor_info.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            descriptor_info.data.pSampledImage = &image_info;

            VkDeviceSize offset;
            vkGetDescriptorSetLayoutBindingOffsetEXT(vk.device, descriptor_set_layout, 5, &offset);
            vkGetDescriptorEXT(vk.device, &descriptor_info, descriptor_buffer_properties.sampledImageDescriptorSize,
                (uint8_t*)mapped_descriptor_buffer_ptr + offset);
        }
        // Write descriptor 6 (sampler)
        {
            VkDescriptorGetInfoEXT descriptor_info{ VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT };
            descriptor_info.type = VK_DESCRIPTOR_TYPE_SAMPLER;
            descriptor_info.data.pSampler = &sampler;

            VkDeviceSize offset;
            vkGetDescriptorSetLayoutBindingOffsetEXT(vk.device, descriptor_set_layout, 6, &offset);
            vkGetDescriptorEXT(vk.device, &descriptor_info, descriptor_buffer_properties.samplerDescriptorSize,
                (uint8_t*)mapped_descriptor_buffer_ptr + offset);
        }
    }
}

void Raytrace_Scene::dispatch(bool spp4, bool show_texture_lod) {
    accelerator.rebuild_top_level_accel(vk.command_buffer);

    VkDescriptorBufferBindingInfoEXT descriptor_buffer_binding_info{ VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT };
    descriptor_buffer_binding_info.address = descriptor_buffer.device_address;
    descriptor_buffer_binding_info.usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT | VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT;
    vkCmdBindDescriptorBuffersEXT(vk.command_buffer, 1, &descriptor_buffer_binding_info);

    const uint32_t buffer_index = 0;
    const VkDeviceSize set_offset = 0;
    vkCmdSetDescriptorBufferOffsetsEXT(vk.command_buffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipeline_layout, 0, 1, &buffer_index, &set_offset);

    vkCmdBindPipeline(vk.command_buffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipeline);

    uint32_t push_constants[2] = { spp4, show_texture_lod };
    vkCmdPushConstants(vk.command_buffer, pipeline_layout, VK_SHADER_STAGE_RAYGEN_BIT_KHR, 0, 4, &push_constants[0]);
    vkCmdPushConstants(vk.command_buffer, pipeline_layout, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, 4, 4, &push_constants[1]);

    const VkBuffer sbt = shader_binding_table.handle;
    const uint32_t sbt_slot_size = properties.shaderGroupHandleSize;
    const uint32_t miss_offset = round_up(sbt_slot_size /* raygen slot*/, properties.shaderGroupBaseAlignment);
    const uint32_t hit_offset = round_up(miss_offset + sbt_slot_size /* miss slot */, properties.shaderGroupBaseAlignment);

    VkStridedDeviceAddressRegionKHR raygen_sbt{};
    raygen_sbt.deviceAddress = shader_binding_table.device_address + 0;
    raygen_sbt.stride = sbt_slot_size;
    raygen_sbt.size = sbt_slot_size;

    VkStridedDeviceAddressRegionKHR miss_sbt{};
    miss_sbt.deviceAddress = shader_binding_table.device_address + miss_offset;
    miss_sbt.stride = sbt_slot_size;
    miss_sbt.size = sbt_slot_size;

    VkStridedDeviceAddressRegionKHR chit_sbt{};
    chit_sbt.deviceAddress = shader_binding_table.device_address + hit_offset;
    chit_sbt.stride = sbt_slot_size;
    chit_sbt.size = sbt_slot_size;

    VkStridedDeviceAddressRegionKHR callable_sbt{};

    vkCmdTraceRaysKHR(vk.command_buffer, &raygen_sbt, &miss_sbt, &chit_sbt, &callable_sbt,
        vk.surface_size.width, vk.surface_size.height, 1);
}

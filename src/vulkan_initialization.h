#pragma once

#include "vulkan_definitions.h"
#include <vector>

struct SDL_SysWMinfo;

void initialize_vulkan(const SDL_SysWMinfo& window_sys_info);
void deinitialize_vulkan();

VkPhysicalDevice get_physical_device();
VkDevice get_device();
uint32_t get_queue_family_index();
VkQueue get_queue();
VkSwapchainKHR get_swapchain();
VkFormat get_swapchain_image_format();
const std::vector<VkImageView>& get_swapchain_image_views();

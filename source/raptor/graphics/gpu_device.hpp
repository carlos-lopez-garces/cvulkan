#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define VK_USE_PLATFORM_WIN32_KHR

#include <vulkan/vulkan.h>
#include "graphics/gpu_resources.hpp"
#include "foundation/data_structures.hpp"
#include "foundation/string.hpp"
#include "foundation/service.hpp"
#include "foundation/array.hpp"
#include "external/vk_mem_alloc.h"

// Forward declarations.
struct Allocator;

namespace cvulkan {

struct DeviceCreation {
    Allocator *allocator = nullputr;

    StackAllocator *temporary_allocator = nullptr;

    void *window = nullptr;

    // Swapchain buffers' resolution.
    u16 width = 1;
    u16 height = 1;

    // TODO: add remaining fields.

    DeviceCreation &set_window(u32 width, u32 height, void *handle);
    DeviceCreation &set_allocator(Allocator *allocator);
    DeviceCreation &set_linear_allocator(StackAllocator *allocator);
};

}
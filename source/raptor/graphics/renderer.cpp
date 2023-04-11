#pragma

#include "graphics/gpu_device.hpp"
#include "graphics/gpu_resources.hpp"
#include "foundation/resource_manager.hpp"

namespace cvulkan {

struct RendererCreation {
    cvulkan::GpuDevice *gpu;
    Allocator *allocator;
};

}
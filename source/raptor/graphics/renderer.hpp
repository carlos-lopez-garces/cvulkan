#pragma

#include "graphics/gpu_device.hpp"
#include "graphics/gpu_resources.hpp"
#include "foundation/resource_manager.hpp"

namespace cvulkan {

struct RendererCreation {
    cvulkan::GpuDevice *gpu;
    Allocator *allocator;
};

// Service is in foundation.
class Renderer : public Service {
private:
    cvulkan::GpuDevice *gpu;

    // Swapchain buffers' resolution.
    u16 width;
    u16 height;

    // ResourcePoolTyped is in foundation.
    ResourcePoolTyped<TextureResource> textures;
    ResourcePoolTyped<BufferResource> buffers;
    ResourcePoolTyped<SamplerResource> samplers;

    // A resource cache is comprised of a hash map for textures, one for buffers,
    // and one for samplers.
    ResourceCache resource_cache;

public:
    RAPTOR_DECLARE_SERVICE(Renderer);

    void init(const RendererCreation &creation); 
};

}
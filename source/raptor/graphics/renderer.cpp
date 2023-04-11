#include "graphics/renderer.hpp"

#include "foundation/memory.hpp"
#include "foundation/file.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include "external/stb_image.h"

namespace cvulkan {

// Single instance of Renderer service. Declared and allocated here, with
// file-level linkage and scope.
static Renderer s_renderer;

static TextureLoader s_texture_loader;
static BufferLoader s_buffer_loader;
static SamplerLoader s_sampler_loader;

// Declaration is made by RAPTOR_DECLARE_SERVICE in Renderer declaration.
Renderer *Renderer::instance() {
    return &s_renderer;
}

void Renderer::init(const RendererCreation &creation) {
    rprint("Renderer init\n");

    gpu = creation.gpu;

    // Swapchain buffers' resolution.
    width = gpu->swapchain_width;
    height = gpu->swapchain_height;

    // Second argument is pool size, so 512 textures, 512 buffers, and 128 samplers.
    textures.init(creation.allocator, 512);
    buffers.init(creation.allocator, 512);
    samplers.init(creation.allocator, 128);

    // Cache for textures, buffers, and samplers.
    resource_cache.init(creation.allocator);
    TextureResource::k_type_hash = hash_calculate( TextureResource::k_type );
    BufferResource::k_type_hash = hash_calculate( BufferResource::k_type );
    SamplerResource::k_type_hash = hash_calculate( SamplerResource::k_type );

    s_texture_loader.renderer = this;
    s_buffer_loader.renderer = this;
    s_sampler_loader.renderer = this;
}

}
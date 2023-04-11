#include "graphics/renderer.hpp"

#include "foundation/memory.hpp"
#include "foundation/file.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include "external/stb_image.h"

namespace cvulkan {

// Single instance of Renderer service. Declared and allocated here, with
// file-level linkage and scope.
static Renderer s_renderer;

// Declaration is made by RAPTOR_DECLARE_SERVICE in Renderer declaration.
Renderer *Renderer::instance() {
    return &s_renderer;
}

}
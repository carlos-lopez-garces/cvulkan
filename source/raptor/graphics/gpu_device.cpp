#include "graphics/gpu_device.hpp"
#include "graphics/command_buffer.hpp"
#include "foundation/memory.hpp"
#include "foundation/hash_map.hpp"
#include "foundation/process.hpp"
#include "foundation/file.hpp"

namespace cvulkan {

void GpuDevice::init(const DeviceCreation &creation) {
    rprint("Gpu Device init\n");

    allocator = creation.allocator;
    temporary_allocator = creation.temporary_allocator;
    string_buffer.init(1024 * 1024, creation.allocator);

    VkResult result;
    vulkan_allocation_callbacks = nullptr;

    VkApplicationInfo application_info = { 
        VK_STRUCTURE_TYPE_APPLICATION_INFO,
        nullptr,
        // Application name.
        "CVULKAN Graphics Device",
        1,
        // Engine name.
        "cvulkan",
        // Engine version.
        1,
        // API version.
        VK_MAKE_VERSION( 1, 2, 0 )
    };

    VkInstanceCreateInfo create_info = { 
        VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        nullptr, 
        0, 
        &application_info,
#if defined(VULKAN_DEBUG_REPORT)
        ArraySize(s_requested_layers),
        s_requested_layers,
#else
        0,
        nullptr,
#endif
        ArraySize(s_requested_extensions), 
        s_requested_extensions
    };

// Enable debug features (specified in create_info).
#if defined(VULKAN_DEBUG_REPORT)
    // VK_EXT_debug_report.
    // Due to the nature of the Vulkan interface, there is very little error information
    // available to the developer and application. By enabling optional validation layers
    // and using the VK_EXT_debug_report extension, developers can obtain much more
    // detailed feedback on the application's use of Vulkan. This extension defines a way
    // for layers and the implementation to call back to the application for events of
    // interest to the application.
    const VkDebugUtilsMessengerCreateInfoEXT debug_create_info = create_debug_utils_messenger_info();

if defined(VULKAN_SYNCHRONIZATION_VALIDATION)
    const VkValidationFeatureEnableEXT features_requested[] = { 
        // Enables GPU-assisted validation, instrumenting shader programs to generate
        // additional diagnostic data.
        VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT,
        // This feature reports resource access conflicts due to missing or incorrect
        // synchronization operations between actions (Draw, Copy, Dispatch, Blit) reading
        // or writing the same regions of memory.
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
        /*, VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT*/ 
    };

    VkValidationFeaturesEXT features = {};
    features.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
    features.pNext = &debug_create_info;
    features.enableValidationFeatureCount = _countof(features_requested);
    features.pEnableValidationFeatures = features_requested;

    create_info.pNext = &features;
#else
    create_info.pNext = &debug_create_info;
#endif
#endif

    // Create Vulkan instance.
    result = vkCreateInstance(
        &create_info,
        vulkan_allocation_callbacks,
        &vulkan_instance
    );
    check(result);
}

}
#include "gfx/vulkan/vk_internal.h"

#include "core/assert.h"
#include "core/log.h"

#include <SDL3/SDL_vulkan.h>

#include <cstring>
#include <vector>

namespace engine::gfx {

    namespace {

        constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

        /// Reports a validation message through the engine log at a matching level.
        VKAPI_ATTR VkBool32 VKAPI_CALL
        debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                       VkDebugUtilsMessageTypeFlagsEXT /*types*/,
                       const VkDebugUtilsMessengerCallbackDataEXT* data, void* /*user_data*/) {
            if (data == nullptr || data->pMessage == nullptr) {
                return VK_FALSE;
            }

            if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0U) {
                ENGINE_LOG_ERROR("vulkan: {}", data->pMessage);
            } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0U) {
                ENGINE_LOG_WARN("vulkan: {}", data->pMessage);
            } else {
                ENGINE_LOG_DEBUG("vulkan: {}", data->pMessage);
            }

            // The specification requires false here. True is reserved for the
            // layer's own testing and aborts the offending call.
            return VK_FALSE;
        }

        VkDebugUtilsMessengerCreateInfoEXT messenger_info() {
            VkDebugUtilsMessengerCreateInfoEXT info{};
            info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                   VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            info.pfnUserCallback = debug_callback;
            return info;
        }

        bool validation_layer_present() {
            std::uint32_t count = 0;
            if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS) {
                return false;
            }

            std::vector<VkLayerProperties> layers(count);
            if (vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS) {
                return false;
            }

            for (const VkLayerProperties& layer : layers) {
                if (std::strcmp(layer.layerName, kValidationLayer) == 0) {
                    return true;
                }
            }
            return false;
        }

        /**
         * Whether the validation layer offers an instance extension.
         *
         * The validation features extension is implemented by the layer rather
         * than by the driver, so it has to be enumerated against the layer name.
         * Asking the driver for it reports nothing.
         */
        bool layer_extension_present(const char* extension) {
            std::uint32_t count = 0;
            if (vkEnumerateInstanceExtensionProperties(kValidationLayer, &count, nullptr) !=
                VK_SUCCESS) {
                return false;
            }

            std::vector<VkExtensionProperties> extensions(count);
            if (vkEnumerateInstanceExtensionProperties(kValidationLayer, &count,
                                                       extensions.data()) != VK_SUCCESS) {
                return false;
            }

            for (const VkExtensionProperties& entry : extensions) {
                if (std::strcmp(entry.extensionName, extension) == 0) {
                    return true;
                }
            }
            return false;
        }

        /// A Vulkan 1.0 loader returns VK_ERROR_INCOMPATIBLE_DRIVER when the
        /// application asks for a higher version, and that error says nothing
        /// useful. Ask first, so the failure names the version we found.
        Result check_loader_version() {
            std::uint32_t version = VK_API_VERSION_1_0;

            // vkEnumerateInstanceVersion arrived in Vulkan 1.1. volk leaves the
            // pointer null on an older loader.
            if (vkEnumerateInstanceVersion != nullptr) {
                ENGINE_VK_TRY(vkEnumerateInstanceVersion(&version));
            }

            if (version < VK_API_VERSION_1_3) {
                ENGINE_LOG_CRITICAL("The Vulkan loader reports {}.{}.{}. This engine needs 1.3.",
                                    VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version),
                                    VK_API_VERSION_PATCH(version));
                return Result::ErrorInit;
            }

            ENGINE_LOG_DEBUG("Vulkan loader {}.{}.{}.", VK_API_VERSION_MAJOR(version),
                             VK_API_VERSION_MINOR(version), VK_API_VERSION_PATCH(version));
            return Result::Success;
        }

        Result create_instance(Device& device, const DeviceDesc& desc) {
            const Result loader = check_loader_version();
            if (!succeeded(loader)) {
                return loader;
            }

            // An offscreen device presents to nothing, so it needs no surface
            // extension. Asking SDL for one also needs its video subsystem, which
            // a machine with no desktop does not have.
            std::vector<const char*> extensions;
            if (desc.window != nullptr) {
                std::uint32_t sdl_count = 0;
                char const* const* sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&sdl_count);
                if (sdl_extensions == nullptr) {
                    ENGINE_LOG_CRITICAL("SDL could not report the Vulkan instance extensions.");
                    return Result::ErrorInit;
                }
                extensions.assign(sdl_extensions, sdl_extensions + sdl_count);
            }

            const bool want_validation = desc.enable_validation && validation_layer_present();
            if (desc.enable_validation && !want_validation) {
                ENGINE_LOG_WARN("Validation was requested but {} is not installed.",
                                kValidationLayer);
            }
            if (want_validation) {
                extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            }

            VkApplicationInfo app{};
            app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
            app.pApplicationName = desc.app_name;
            app.pEngineName = "Camina Engine";
            app.apiVersion = VK_API_VERSION_1_3;

            VkInstanceCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
            info.pApplicationInfo = &app;
            info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
            info.ppEnabledExtensionNames = extensions.data();

            // The messenger must outlive the create call to catch failures inside
            // it, so it lives here and hangs off pNext.
            const VkDebugUtilsMessengerCreateInfoEXT debug = messenger_info();

            // Synchronization validation reads the barriers rather than the
            // calls. It is what reports a read that races a write, which is the
            // failure a wrong barrier gives and the one that shows on a single
            // vendor. It costs real time on every frame, so it is off unless
            // somebody asks. Both structures must outlive vkCreateInstance.
            constexpr VkValidationFeatureEnableEXT kSyncFeature =
                VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
            VkValidationFeaturesEXT features{};
            features.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
            features.enabledValidationFeatureCount = 1;
            features.pEnabledValidationFeatures = &kSyncFeature;

            if (want_validation) {
                info.enabledLayerCount = 1;
                info.ppEnabledLayerNames = &kValidationLayer;
                info.pNext = &debug;
            }

            // The structure above may only sit in the pNext chain when the
            // extension that defines it is enabled. The specification says so,
            // and a chain without it is invalid however well it happens to run.
            // The extension comes from the layer, so this is asked of the layer.
            bool want_sync = want_validation && desc.enable_sync_validation;
            if (want_sync && !layer_extension_present(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME)) {
                ENGINE_LOG_WARN("Synchronization validation was asked for, and {} is not there "
                                "to turn it on with.",
                                VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
                want_sync = false;
            }
            if (want_sync) {
                extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
                info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
                info.ppEnabledExtensionNames = extensions.data();

                // The messenger chains behind the features, so both are read.
                features.pNext = &debug;
                info.pNext = &features;
                ENGINE_LOG_INFO("Synchronization validation is on. Frames will be slower.");
            }

            ENGINE_VK_TRY(vkCreateInstance(&info, nullptr, &device.instance));
            volkLoadInstance(device.instance);

            if (want_validation) {
                ENGINE_VK_TRY(vkCreateDebugUtilsMessengerEXT(device.instance, &debug, nullptr,
                                                             &device.messenger));
            }
            return Result::Success;
        }

        /// Finds a queue family that draws, and that presents to our surface
        /// when there is one. A null surface means offscreen, and then drawing
        /// is the whole requirement.
        bool find_graphics_family(VkPhysicalDevice physical, VkSurfaceKHR surface,
                                  std::uint32_t& out_family) {
            std::uint32_t count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
            std::vector<VkQueueFamilyProperties> families(count);
            vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());

            for (std::uint32_t i = 0; i < count; ++i) {
                if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0U) {
                    continue;
                }
                if (surface == VK_NULL_HANDLE) {
                    out_family = i;
                    return true;
                }
                VkBool32 present = VK_FALSE;
                if (vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, surface, &present) !=
                    VK_SUCCESS) {
                    continue;
                }
                if (present == VK_TRUE) {
                    out_family = i;
                    return true;
                }
            }
            return false;
        }

        bool supports_swapchain(VkPhysicalDevice physical) {
            std::uint32_t count = 0;
            if (vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr) !=
                VK_SUCCESS) {
                return false;
            }
            std::vector<VkExtensionProperties> extensions(count);
            if (vkEnumerateDeviceExtensionProperties(physical, nullptr, &count,
                                                     extensions.data()) != VK_SUCCESS) {
                return false;
            }
            for (const VkExtensionProperties& extension : extensions) {
                if (std::strcmp(extension.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
                    return true;
                }
            }
            return false;
        }

        /// Both features are required. DESIGN.md section 2 picks dynamic rendering
        /// and synchronization2, so a device without them is not a candidate.
        bool supports_required_features(VkPhysicalDevice physical) {
            VkPhysicalDeviceVulkan13Features features13{};
            features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;

            VkPhysicalDeviceFeatures2 features{};
            features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            features.pNext = &features13;

            vkGetPhysicalDeviceFeatures2(physical, &features);
            return features13.dynamicRendering == VK_TRUE && features13.synchronization2 == VK_TRUE;
        }

        Result select_physical_device(Device& device) {
            std::uint32_t count = 0;
            ENGINE_VK_TRY(vkEnumeratePhysicalDevices(device.instance, &count, nullptr));
            if (count == 0) {
                ENGINE_LOG_CRITICAL("No Vulkan device is present.");
                return Result::ErrorNoDevice;
            }

            std::vector<VkPhysicalDevice> candidates(count);
            ENGINE_VK_TRY(vkEnumeratePhysicalDevices(device.instance, &count, candidates.data()));

            VkPhysicalDevice best = VK_NULL_HANDLE;
            std::uint32_t best_family = 0;
            bool best_is_discrete = false;

            for (VkPhysicalDevice candidate : candidates) {
                VkPhysicalDeviceProperties properties{};
                vkGetPhysicalDeviceProperties(candidate, &properties);

                if (properties.apiVersion < VK_API_VERSION_1_3) {
                    ENGINE_LOG_DEBUG("Skipping {}: it reports Vulkan below 1.3.",
                                     properties.deviceName);
                    continue;
                }
                // A GPU that cannot present is still a candidate offscreen. That
                // is the case on a machine with no desktop, which is the whole
                // reason the offscreen path exists.
                if ((!device.headless && !supports_swapchain(candidate)) ||
                    !supports_required_features(candidate)) {
                    ENGINE_LOG_DEBUG("Skipping {}: a required extension or feature is missing.",
                                     properties.deviceName);
                    continue;
                }

                std::uint32_t family = 0;
                if (!find_graphics_family(candidate, device.surface, family)) {
                    ENGINE_LOG_DEBUG("Skipping {}: no queue draws{}.", properties.deviceName,
                                     device.headless ? "" : " and presents");
                    continue;
                }

                const bool discrete =
                    properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;

                // Take the first device that qualifies, then upgrade only to a
                // discrete one. Anything finer needs a real workload to judge.
                if (best == VK_NULL_HANDLE || (discrete && !best_is_discrete)) {
                    best = candidate;
                    best_family = family;
                    best_is_discrete = discrete;
                    device.properties = properties;
                }
            }

            if (best == VK_NULL_HANDLE) {
                ENGINE_LOG_CRITICAL("No Vulkan 1.3 device supports dynamic rendering, "
                                    "synchronization2, and presenting to this window.");
                return Result::ErrorNoDevice;
            }

            device.physical = best;
            device.graphics_family = best_family;
            return Result::Success;
        }

        Result create_logical_device(Device& device) {
            const float priority = 1.0F;

            VkDeviceQueueCreateInfo queue{};
            queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queue.queueFamilyIndex = device.graphics_family;
            queue.queueCount = 1;
            queue.pQueuePriorities = &priority;

            VkPhysicalDeviceVulkan13Features features13{};
            features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
            features13.dynamicRendering = VK_TRUE;
            features13.synchronization2 = VK_TRUE;

            VkPhysicalDeviceFeatures2 features{};
            features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            features.pNext = &features13;

            const char* const swapchain_extension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;

            VkDeviceCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
            info.pNext = &features;
            info.queueCreateInfoCount = 1;
            info.pQueueCreateInfos = &queue;
            // An offscreen device never builds a swapchain, and asking for the
            // extension would refuse a GPU that can draw but cannot present.
            info.enabledExtensionCount = device.headless ? 0U : 1U;
            info.ppEnabledExtensionNames = device.headless ? nullptr : &swapchain_extension;

            ENGINE_VK_TRY(vkCreateDevice(device.physical, &info, nullptr, &device.device));
            volkLoadDevice(device.device);
            vkGetDeviceQueue(device.device, device.graphics_family, 0, &device.graphics_queue);
            return Result::Success;
        }

        Result create_allocator(Device& device) {
            // volk resolves every entry point, so VMA takes the two loader hooks
            // and finds the rest itself. See VMA_DYNAMIC_VULKAN_FUNCTIONS in
            // vk_common.h.
            VmaVulkanFunctions functions{};
            functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
            functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

            VmaAllocatorCreateInfo info{};
            info.physicalDevice = device.physical;
            info.device = device.device;
            info.instance = device.instance;
            info.vulkanApiVersion = VK_API_VERSION_1_3;
            info.pVulkanFunctions = &functions;

            ENGINE_VK_TRY(vmaCreateAllocator(&info, &device.allocator));
            return Result::Success;
        }

        Result create_frames(Device& device) {
            for (Frame& frame : device.frames) {
                // The command list carries the device so that cmd_bind_pipeline()
                // can resolve a handle without a second argument.
                frame.commands.owner = &device;

                VkCommandPoolCreateInfo pool{};
                pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
                pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
                pool.queueFamilyIndex = device.graphics_family;
                ENGINE_VK_TRY(vkCreateCommandPool(device.device, &pool, nullptr, &frame.pool));

                VkCommandBufferAllocateInfo buffer{};
                buffer.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                buffer.commandPool = frame.pool;
                buffer.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                buffer.commandBufferCount = 1;
                ENGINE_VK_TRY(
                    vkAllocateCommandBuffers(device.device, &buffer, &frame.commands.buffer));

                VkSemaphoreCreateInfo semaphore{};
                semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
                ENGINE_VK_TRY(vkCreateSemaphore(device.device, &semaphore, nullptr,
                                                &frame.image_available));

                // Created signaled so that the first wait on each slot returns at once.
                VkFenceCreateInfo fence{};
                fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
                fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
                ENGINE_VK_TRY(vkCreateFence(device.device, &fence, nullptr, &frame.in_flight));
            }
            return Result::Success;
        }

        void destroy_frames(Device& device) {
            for (Frame& frame : device.frames) {
                if (frame.in_flight != VK_NULL_HANDLE) {
                    vkDestroyFence(device.device, frame.in_flight, nullptr);
                    frame.in_flight = VK_NULL_HANDLE;
                }
                if (frame.image_available != VK_NULL_HANDLE) {
                    vkDestroySemaphore(device.device, frame.image_available, nullptr);
                    frame.image_available = VK_NULL_HANDLE;
                }
                if (frame.pool != VK_NULL_HANDLE) {
                    vkDestroyCommandPool(device.device, frame.pool, nullptr);
                    frame.pool = VK_NULL_HANDLE;
                    frame.commands.buffer = VK_NULL_HANDLE;
                }
            }
        }

    } // namespace

    namespace vk {

        const char* vk_result_name(VkResult result) {
            switch (result) {
            case VK_SUCCESS:
                return "VK_SUCCESS";
            case VK_NOT_READY:
                return "VK_NOT_READY";
            case VK_TIMEOUT:
                return "VK_TIMEOUT";
            case VK_SUBOPTIMAL_KHR:
                return "VK_SUBOPTIMAL_KHR";
            case VK_ERROR_OUT_OF_HOST_MEMORY:
                return "VK_ERROR_OUT_OF_HOST_MEMORY";
            case VK_ERROR_OUT_OF_DEVICE_MEMORY:
                return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
            case VK_ERROR_INITIALIZATION_FAILED:
                return "VK_ERROR_INITIALIZATION_FAILED";
            case VK_ERROR_DEVICE_LOST:
                return "VK_ERROR_DEVICE_LOST";
            case VK_ERROR_EXTENSION_NOT_PRESENT:
                return "VK_ERROR_EXTENSION_NOT_PRESENT";
            case VK_ERROR_FEATURE_NOT_PRESENT:
                return "VK_ERROR_FEATURE_NOT_PRESENT";
            case VK_ERROR_OUT_OF_DATE_KHR:
                return "VK_ERROR_OUT_OF_DATE_KHR";
            case VK_ERROR_SURFACE_LOST_KHR:
                return "VK_ERROR_SURFACE_LOST_KHR";
            case VK_ERROR_INCOMPATIBLE_DRIVER:
                return "VK_ERROR_INCOMPATIBLE_DRIVER";
            case VK_ERROR_LAYER_NOT_PRESENT:
                return "VK_ERROR_LAYER_NOT_PRESENT";
            default:
                break;
            }
            // ENGINE_VK_TRY also logs the number, so an unmapped value stays
            // traceable to the specification.
            return "an unmapped VkResult";
        }

        Result to_result(VkResult result) {
            switch (result) {
            case VK_SUCCESS:
            case VK_SUBOPTIMAL_KHR:
                return Result::Success;
            case VK_ERROR_OUT_OF_DATE_KHR:
                return Result::OutOfDate;
            case VK_ERROR_SURFACE_LOST_KHR:
                return Result::ErrorSurface;
            case VK_ERROR_DEVICE_LOST:
                return Result::ErrorDeviceLost;
            case VK_ERROR_OUT_OF_HOST_MEMORY:
            case VK_ERROR_OUT_OF_DEVICE_MEMORY:
                return Result::ErrorOutOfMemory;
            case VK_ERROR_INITIALIZATION_FAILED:
            case VK_ERROR_EXTENSION_NOT_PRESENT:
            case VK_ERROR_FEATURE_NOT_PRESENT:
            case VK_ERROR_INCOMPATIBLE_DRIVER:
            case VK_ERROR_LAYER_NOT_PRESENT:
                return Result::ErrorInit;
            default:
                break;
            }
            return Result::ErrorUnknown;
        }

    } // namespace vk

    Result create_device(const DeviceDesc& desc, Device** out_device) {
        ENGINE_CHECK(out_device != nullptr, "create_device needs somewhere to put the device.");
        *out_device = nullptr;

        // A null window is not an error. It renders offscreen, which is what
        // makes a run reproducible and lets a machine with no desktop draw. See
        // gfx::DeviceDesc::window.
        if (volkInitialize() != VK_SUCCESS) {
            ENGINE_LOG_CRITICAL("No Vulkan loader is present on this system.");
            return Result::ErrorInit;
        }

        auto* device = new Device();
        device->vsync = desc.vsync;
        device->headless = desc.window == nullptr;

        Result result = create_instance(*device, desc);
        if (!succeeded(result)) {
            destroy_device(device);
            return result;
        }

        auto* window = static_cast<SDL_Window*>(desc.window);
        if (!device->headless &&
            !SDL_Vulkan_CreateSurface(window, device->instance, nullptr, &device->surface)) {
            ENGINE_LOG_CRITICAL("SDL could not create a Vulkan surface for the window.");
            destroy_device(device);
            return Result::ErrorSurface;
        }

        result = select_physical_device(*device);
        if (!succeeded(result)) {
            destroy_device(device);
            return result;
        }

        result = create_logical_device(*device);
        if (!succeeded(result)) {
            destroy_device(device);
            return result;
        }

        result = create_allocator(*device);
        if (!succeeded(result)) {
            destroy_device(device);
            return result;
        }

        result = create_frames(*device);
        if (!succeeded(result)) {
            destroy_device(device);
            return result;
        }

        result = vk::create_shared_resources(*device);
        if (!succeeded(result)) {
            destroy_device(device);
            return result;
        }

        if (device->headless) {
            result = vk::create_offscreen_targets(*device, desc.offscreen_extent);
        } else {
            int width = 0;
            int height = 0;
            SDL_GetWindowSizeInPixels(window, &width, &height);
            result = vk::create_swapchain(*device, { static_cast<std::uint32_t>(width),
                                                     static_cast<std::uint32_t>(height) });
        }
        if (!succeeded(result)) {
            destroy_device(device);
            return result;
        }

        ENGINE_LOG_INFO("Vulkan ready on {}. {} {} images at {}x{}, {} frames in flight.",
                        device->properties.deviceName, device->images.size(),
                        device->headless ? "offscreen" : "swapchain",
                        device->swapchain_extent.width, device->swapchain_extent.height,
                        kFramesInFlight);

        *out_device = device;
        return Result::Success;
    }

    void destroy_device(Device* device) {
        if (device == nullptr) {
            return;
        }

        if (device->device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device->device);
            vk::destroy_pipelines(*device);
            vk::destroy_textures(*device);
            vk::destroy_buffers(*device);
            vk::destroy_swapchain(*device);
            vk::destroy_shared_resources(*device);
            destroy_frames(*device);
        }

        if (device->allocator != VK_NULL_HANDLE) {
            vmaDestroyAllocator(device->allocator);
        }
        if (device->device != VK_NULL_HANDLE) {
            vkDestroyDevice(device->device, nullptr);
        }
        if (device->surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(device->instance, device->surface, nullptr);
        }
        if (device->messenger != VK_NULL_HANDLE) {
            vkDestroyDebugUtilsMessengerEXT(device->instance, device->messenger, nullptr);
        }
        if (device->instance != VK_NULL_HANDLE) {
            vkDestroyInstance(device->instance, nullptr);
        }

        delete device;
    }

    const char* device_name(const Device* device) {
        if (device == nullptr) {
            return "none";
        }
        return static_cast<const char*>(device->properties.deviceName);
    }

    void device_wait_idle(Device* device) {
        if (device != nullptr && device->device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device->device);
        }
    }

} // namespace engine::gfx

// The ImGui Vulkan backend includes vulkan.h, so rule 4.1 puts this file here.
// It is the only place that knows ImGui and Vulkan at the same time.

#include "core/log.h"
#include "gfx/imgui.h"
#include "gfx/vulkan/vk_internal.h"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include <SDL3/SDL_events.h>

namespace engine::gfx {

    namespace {

        /// How many textures the overlay can bind at once. The font is one.
        constexpr std::uint32_t kImGuiTextureSlots = 16;

        /// True between a successful imgui_init() and imgui_shutdown().
        bool g_started = false;

        void report_vulkan_error(VkResult result) {
            if (result != VK_SUCCESS) {
                ENGINE_LOG_ERROR("The ImGui backend reported {}",
                                 vk::vk_result_name(result));
            }
        }

    } // namespace

    Result imgui_init(Device* device, void* sdl_window) {
        if (device == nullptr || sdl_window == nullptr) {
            return Result::ErrorInit;
        }
        if (g_started) {
            ENGINE_LOG_WARN("imgui_init was called twice. The second call does nothing.");
            return Result::Success;
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();

        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        // The overlay has no home directory to write to, and a stray imgui.ini
        // next to the executable surprises people. M8 gives the editor a real
        // settings path.
        io.IniFilename = nullptr;

        if (!ImGui_ImplSDL3_InitForVulkan(static_cast<SDL_Window*>(sdl_window))) {
            ENGINE_LOG_ERROR("The ImGui SDL3 backend did not start.");
            ImGui::DestroyContext();
            return Result::ErrorInit;
        }

        // Every attachment the frame opens must appear here. A pipeline that
        // leaves one out fails validation the moment it draws. This is the same
        // trap the cube pass hit in M1.3.
        VkPipelineRenderingCreateInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &device->swapchain_format;
        rendering.depthAttachmentFormat = device->depth_format;

        ImGui_ImplVulkan_InitInfo info{};
        info.ApiVersion = VK_API_VERSION_1_3;
        info.Instance = device->instance;
        info.PhysicalDevice = device->physical;
        info.Device = device->device;
        info.QueueFamily = device->graphics_family;
        info.Queue = device->graphics_queue;
        // A non-zero size makes the backend own its descriptor pool, so the
        // overlay never competes with the texture pool for slots.
        info.DescriptorPoolSize = kImGuiTextureSlots;
        info.MinImageCount = static_cast<std::uint32_t>(device->images.size());
        info.ImageCount = static_cast<std::uint32_t>(device->images.size());
        info.UseDynamicRendering = true;
        info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        info.PipelineInfoMain.PipelineRenderingCreateInfo = rendering;
        info.CheckVkResultFn = report_vulkan_error;

        if (!ImGui_ImplVulkan_Init(&info)) {
            ENGINE_LOG_ERROR("The ImGui Vulkan backend did not start.");
            ImGui_ImplSDL3_Shutdown();
            ImGui::DestroyContext();
            return Result::ErrorInit;
        }

        g_started = true;
        ENGINE_LOG_INFO("The ImGui overlay started with {} swapchain images.",
                        device->images.size());
        return Result::Success;
    }

    void imgui_shutdown(Device* device) {
        if (!g_started) {
            return;
        }
        if (device != nullptr) {
            // The backend frees its own images and buffers, and the GPU may still
            // be reading them.
            vkDeviceWaitIdle(device->device);
        }
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        g_started = false;
    }

    void imgui_process_event(const void* sdl_event) {
        if (!g_started || sdl_event == nullptr) {
            return;
        }
        ImGui_ImplSDL3_ProcessEvent(static_cast<const SDL_Event*>(sdl_event));
    }

    void imgui_wants_input(bool* mouse, bool* keyboard) {
        const bool started = g_started;
        if (mouse != nullptr) {
            *mouse = started && ImGui::GetIO().WantCaptureMouse;
        }
        if (keyboard != nullptr) {
            *keyboard = started && ImGui::GetIO().WantCaptureKeyboard;
        }
    }

    void imgui_new_frame() {
        if (!g_started) {
            return;
        }
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
    }

    void imgui_render(CommandList* commands) {
        if (!g_started) {
            return;
        }
        ImGui::Render();
        if (commands != nullptr) {
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commands->buffer);
        }
    }

} // namespace engine::gfx

// The ImGui Vulkan backend includes vulkan.h, so rule 4.1 puts this file here.
// It is the only place that knows ImGui and Vulkan at the same time.

#include "core/log.h"
#include "gfx/imgui.h"
#include "gfx/vulkan/vk_internal.h"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include <SDL3/SDL_events.h>

#include <cmath>

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

        /// Converts one sRGB channel to linear, for the hardware sRGB encode.
        [[nodiscard]] float srgb_to_linear(float c) {
            if (c <= 0.04045F) {
                return c / 12.92F;
            }
            return std::pow((c + 0.055F) / 1.055F, 2.4F);
        }

        /**
         * Walks the draw data and converts every vertex colour from sRGB to
         * linear. ImGui works in sRGB, but the swapchain is _SRGB and the
         * hardware encodes every write. Giving the hardware a linear colour
         * gets the sRGB one back out. See DESIGN.md section 3.
         */
        void convert_draw_colors_to_linear(ImDrawData* data) {
            for (int n = 0; n < data->CmdListsCount; ++n) {
                const ImDrawList* list = data->CmdLists[n];
                ImDrawVert* vertices = list->VtxBuffer.Data;
                for (int v = 0; v < list->VtxBuffer.Size; ++v) {
                    ImU32 col = vertices[v].col;
                    float r = static_cast<float>((col >> IM_COL32_R_SHIFT) & 0xFF) / 255.0F;
                    float g = static_cast<float>((col >> IM_COL32_G_SHIFT) & 0xFF) / 255.0F;
                    float b = static_cast<float>((col >> IM_COL32_B_SHIFT) & 0xFF) / 255.0F;
                    float a = static_cast<float>((col >> IM_COL32_A_SHIFT) & 0xFF) / 255.0F;

                    r = srgb_to_linear(r);
                    g = srgb_to_linear(g);
                    b = srgb_to_linear(b);

                    vertices[v].col =
                        IM_COL32(static_cast<int>(r * 255.0F), static_cast<int>(g * 255.0F),
                                 static_cast<int>(b * 255.0F), static_cast<int>(a * 255.0F));
                }
            }
        }

    } // namespace

    Result imgui_init(Device* device, const ImGuiDesc& desc) {
        if (device == nullptr || desc.sdl_window == nullptr) {
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
        // Keyboard navigation otherwise raises WantCaptureKeyboard for every
        // frame a window holds focus, and imgui.h says so at the flag itself.
        // An application reading that flag to decide whether a key belongs to
        // it therefore never sees a key at all. With this false the flag means
        // what a caller expects: a widget is really taking the keyboard, such
        // as an open text field. Navigation still works.
        io.ConfigNavCaptureKeyboard = false;
        // The application decides both. The runtime overlay passes no path, so
        // it writes no file and its windows open where its constants put them.
        // The editor passes one under the user preferences directory, and its
        // panels come back where the last session left them.
        io.IniFilename = desc.ini_path;
        if (desc.docking) {
            io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        }

        if (!ImGui_ImplSDL3_InitForVulkan(static_cast<SDL_Window*>(desc.sdl_window))) {
            ENGINE_LOG_ERROR("The ImGui SDL3 backend did not start.");
            ImGui::DestroyContext();
            return Result::ErrorInit;
        }

        // ImGui draws in the tonemap scope, which attaches no depth. The
        // pipeline must agree.
        VkPipelineRenderingCreateInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &device->swapchain_format;
        rendering.depthAttachmentFormat = VK_FORMAT_UNDEFINED;

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
        ENGINE_LOG_INFO("The ImGui overlay started with {} swapchain images. Docking is {}, "
                        "and the layout file is {}.",
                        device->images.size(), desc.docking ? "on" : "off",
                        desc.ini_path != nullptr ? desc.ini_path : "none");
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
            ImDrawData* data = ImGui::GetDrawData();
            convert_draw_colors_to_linear(data);
            ImGui_ImplVulkan_RenderDrawData(data, commands->buffer);
        }
    }

} // namespace engine::gfx

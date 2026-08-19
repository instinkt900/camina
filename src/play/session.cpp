#include "play/session.h"

#include "assets/content.h"
#include "assets/manifest.h"
#include "assets/script.h"
#include "core/log.h"
#include "core/profile.h"
#include "scene/prefab.h"
#include "scene/world.h"

#include <cstddef>
#include <string_view>

namespace engine::play {

    void Session::build(scene::World& world) { simulation_.build(world); }

#if defined(ENGINE_WITH_LUA)

    script::Services Session::step_services(const View& view) {
        camera_ = script::CameraView{ .position = view.position, .forward = view.forward };
        return script::Services{
            .physics = &simulation_,
            .input = &step_input_,
            .prefabs = &scene::prefabs(),
            .camera = &camera_,
            .motion = &motion_,
        };
    }

    void Session::load_scripts(assets::AssetSource& content) {
        std::size_t loaded = 0;
        std::size_t failed = 0;

        std::vector<assets::AssetRecord> records;
        if (!content.assets_of_kind(assets::kScriptExtension, records)) {
            ENGINE_LOG_ERROR("The scripts could not be listed, so none were loaded.");
            return;
        }

        for (const assets::AssetRecord& record : records) {
            std::vector<std::byte> bytes;
            if (!content.read(record.guid, bytes)) {
                ENGINE_LOG_ERROR("{} is in the project and will not read.", record.name);
                ++failed;
                continue;
            }
            if (scripts_.load(record.guid, record.name, bytes)) {
                ++loaded;
            } else {
                ++failed;
            }
        }

        ENGINE_LOG_INFO("Loaded {} script(s), {} failed.", loaded, failed);
    }

    void Session::reload_scripts(assets::AssetSource& content,
                                 const std::vector<assets::AssetChange>& changed) {
        for (const assets::AssetChange& change : changed) {
            if (change.gone ||
                !std::string_view{ change.cooked }.ends_with(assets::kScriptExtension)) {
                continue;
            }

            std::vector<std::byte> bytes;
            if (!content.read(change.guid, bytes)) {
                ENGINE_LOG_ERROR("{} cooked and will not read, so it was not loaded again.",
                                 change.cooked);
                continue;
            }
            if (scripts_.reload(change.guid, change.cooked, bytes)) {
                ENGINE_LOG_INFO("{} was read again.", change.cooked);
            }
        }
    }

    void Session::stop_scripts(scene::World& world, const View& view) {
        scripts_.stop(world, step_services(view));
    }

#else

    void Session::load_scripts(assets::AssetSource& content) { (void)content; }

    void Session::reload_scripts(assets::AssetSource& content,
                                 const std::vector<assets::AssetChange>& changed) {
        (void)content;
        (void)changed;
    }

    void Session::stop_scripts(scene::World& world, const View& view) {
        (void)world;
        (void)view;
    }

#endif

    void Session::feed_input(const platform::InputFrame& frame) {
        latest_ = frame;
        for (std::size_t i = 0; i < frame.keys.size(); ++i) {
            pending_.keys.at(i) = pending_.keys.at(i) || frame.keys.at(i);
        }
        for (std::size_t i = 0; i < frame.mouse_buttons.size(); ++i) {
            pending_.mouse_buttons.at(i) =
                pending_.mouse_buttons.at(i) || frame.mouse_buttons.at(i);
        }
        pending_.focused = pending_.focused || frame.focused;
    }

    void Session::advance(scene::World& world, const View& view, float delta_seconds) {
        for (std::uint32_t left = clock_.advance(delta_seconds); left > 0; --left) {
            ENGINE_PROFILE_ZONE_N("fixed step");

            // The actions the game reads move to this step, and the fold starts
            // again from whatever the device last read. So a key held across
            // several steps stays held, and a key tapped and let go between two
            // of them still raises exactly one edge.
            step_input_.update(pending_);
            pending_ = latest_;

            // The game reads the pose the last step left rather than the blend
            // the last frame drew. Without this the motion of a frame that fell
            // between two steps feeds back in and compounds.
            motion_.begin_step(world);

            seconds_ += static_cast<double>(clock_.step_seconds());

            // The game moves things, then the solver runs. A kinematic body the
            // game drives has to carry its new transform into the step, so this
            // order is the one that works.
#if defined(ENGINE_WITH_LUA)
            // Passed on each step rather than held, so a reload that builds a
            // new simulation cannot leave a script driving the old one. See
            // issue #273.
            scripts_.update(world, seconds_, step_services(view));
#else
            (void)view;
#endif
            simulation_.step(world, clock_.step_seconds());

            // Inside the loop, because the simulation keeps one step of events.
            // Reading them after the loop would report the last step alone.
#if defined(ENGINE_WITH_LUA)
            scripts_.deliver_physics_events(world, simulation_, step_services(view));
#endif
        }

        // One alpha for both, because they blend the same pair of steps. Two
        // would let the game and the physics draw different instants.
        const float alpha = clock_.alpha();
        simulation_.interpolate(world, alpha);
        motion_.interpolate(world, alpha);
    }

} // namespace engine::play

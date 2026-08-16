#include "editor/play_mode.h"

#include "core/log.h"
#include "scene/scene_file.h"
#include "scene/world.h"

namespace engine::editor {

    bool PlayMode::play(scene::World& world, const PlayDesc& desc) {
        if (state_ != PlayState::Edit) {
            return false;
        }

        // Before anything runs, because this document is the only copy of what
        // a person authored once the first step has moved something.
        snapshot_ = scene::save_scene(world);

        session_ = std::make_unique<play::Session>();
        if (desc.bind_actions != nullptr) {
            desc.bind_actions(session_->input());
        }
        if (desc.content != nullptr) {
            session_->load_scripts(*desc.content);
        }

        // After the scripts, so an entity that names one finds it loaded rather
        // than reporting it missing on the first step. A body starts where its
        // entity sits, so this reads the world as the person left it.
        session_->build(world);

        state_ = PlayState::Playing;
        ENGINE_LOG_INFO("Play started on {} entities.", world.size());
        return true;
    }

    void PlayMode::pause() {
        if (state_ == PlayState::Playing) {
            state_ = PlayState::Paused;
        }
    }

    void PlayMode::resume() {
        if (state_ == PlayState::Paused) {
            state_ = PlayState::Playing;
        }
    }

    void PlayMode::stop(scene::World& world) {
        if (state_ == PlayState::Edit) {
            return;
        }

        // While the simulation is still up, so a teardown that pushes a body or
        // reads a velocity reaches a live one rather than nothing.
        session_->stop_scripts(world);
        session_.reset();

        // Every entity goes, and the session that held poses and bodies for
        // them has already gone. EnTT hands the same numbers out again, so
        // nothing may carry an entity across this line.
        world.clear();

        if (!scene::load_scene(snapshot_, world)) {
            ENGINE_LOG_ERROR("The snapshot did not read back, so the world is not the one you "
                             "authored. Open the scene again.");
        }

        // The load writes local transforms, and nothing has composed the world
        // matrices from them yet.
        world.update();

        snapshot_ = nlohmann::json{};
        state_ = PlayState::Edit;
        ENGINE_LOG_INFO("Play stopped. The world holds {} entities again.", world.size());
    }

    void PlayMode::advance(scene::World& world, const play::View& view, float delta_seconds) {
        if (state_ != PlayState::Playing) {
            return;
        }
        session_->advance(world, view, delta_seconds);
    }

    void PlayMode::feed_input(const platform::InputFrame& frame) {
        if (session_) {
            session_->feed_input(frame);
        }
    }

} // namespace engine::editor

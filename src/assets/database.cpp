#include "assets/database.h"

namespace engine::assets {

    const char* state_name(AssetState state) {
        switch (state) {
        case AssetState::Unloaded:
            return "not loaded";
        case AssetState::Loaded:
            return "loaded";
        case AssetState::Failed:
            return "broken";
        }
        return "in an unknown state";
    }

} // namespace engine::assets

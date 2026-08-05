#include "gfx/types.h"

namespace engine::gfx {

    const char* result_name(Result result) {
        switch (result) {
        case Result::Success:
            return "Success";
        case Result::OutOfDate:
            return "OutOfDate";
        case Result::ErrorInit:
            return "ErrorInit";
        case Result::ErrorNoDevice:
            return "ErrorNoDevice";
        case Result::ErrorSurface:
            return "ErrorSurface";
        case Result::ErrorDeviceLost:
            return "ErrorDeviceLost";
        case Result::ErrorOutOfMemory:
            return "ErrorOutOfMemory";
        case Result::ErrorUnknown:
            break;
        }
        return "ErrorUnknown";
    }

    const char* resource_state_name(ResourceState state) {
        switch (state) {
        case ResourceState::Undefined:
            return "Undefined";
        case ResourceState::ColorTarget:
            return "ColorTarget";
        case ResourceState::DepthTarget:
            return "DepthTarget";
        case ResourceState::DepthRead:
            return "DepthRead";
        case ResourceState::ShaderRead:
            return "ShaderRead";
        case ResourceState::ComputeWrite:
            return "ComputeWrite";
        case ResourceState::CopySource:
            return "CopySource";
        case ResourceState::CopyDestination:
            return "CopyDestination";
        case ResourceState::Present:
            break;
        }
        return "Present";
    }

} // namespace engine::gfx

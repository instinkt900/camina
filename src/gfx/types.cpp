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

} // namespace engine::gfx

#include "reflect/registry.h"

#include <algorithm>

namespace engine::reflect {

    const TypeInfo* Registry::find(std::string_view name) const {
        const auto match = std::find_if(types_.begin(), types_.end(), [name](const TypeInfo& info) {
            return name == info.name;
        });
        return match == types_.end() ? nullptr : &(*match);
    }

    Registry& registry() {
        // Built on the first call, so it cannot depend on static init order.
        static Registry instance;
        return instance;
    }

} // namespace engine::reflect

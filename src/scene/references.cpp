#include "scene/references.h"

#include "assets/reference.h"
#include "core/guid.h"
#include "scene/document.h"

#include <string>
#include <utility>

namespace engine::scene {

    namespace {

        /// Visits the tagged fields of one component set, keyed by type name.
        bool visit_components(nlohmann::json& parts, const ComponentRegistry& types,
                              const ReferenceFieldVisitor& visit) {
            if (!parts.is_object()) {
                return true;
            }
            for (auto part : parts.items()) {
                const ComponentOps* ops = types.find(part.key());
                if (ops == nullptr || !part.value().is_object()) {
                    continue;
                }
                for (const char* name : ops->reference_field_names) {
                    const auto field = part.value().find(name);
                    // A patch names only the fields it changed, so a missing
                    // field is the normal case rather than a fault.
                    if (field == part.value().end() || !field->is_string()) {
                        continue;
                    }
                    if (!visit(*field)) {
                        return false;
                    }
                }
            }
            return true;
        }

        /// Visits one entity record, whether it is a plain entity or an instance.
        bool visit_entity(nlohmann::json& record, const ComponentRegistry& types,
                          const ReferenceFieldVisitor& visit) {
            if (!record.is_object()) {
                return true;
            }

            if (const auto parts = record.find(kComponentsKey); parts != record.end()) {
                if (!visit_components(*parts, types, visit)) {
                    return false;
                }
            }

            // A prefab instance. The patch is keyed by member index, and each
            // value holds the same shape a component set does.
            if (const auto patches = record.find(kOverridesKey);
                patches != record.end() && patches->is_object()) {
                for (auto& patch : *patches) {
                    if (!visit_components(patch, types, visit)) {
                        return false;
                    }
                }
            }

            // An entity the instance added carries a whole component set of its
            // own, in the shape an entity record has.
            if (const auto added = record.find(kAddedKey);
                added != record.end() && added->is_array()) {
                for (auto& entry : *added) {
                    if (!visit_entity(entry, types, visit)) {
                        return false;
                    }
                }
            }
            return true;
        }

    } // namespace

    bool for_each_reference_field(nlohmann::json& document, const ComponentRegistry& types,
                                  const ReferenceFieldVisitor& visit) {
        const auto entities = document.find(kEntitiesKey);
        if (entities == document.end() || !entities->is_array()) {
            return true;
        }
        for (auto& record : *entities) {
            if (!visit_entity(record, types, visit)) {
                return false;
            }
        }
        return true;
    }

    std::size_t restore_references(nlohmann::json& document, const assets::Manifest& manifest,
                                   const ComponentRegistry& types) {
        std::size_t count = 0;
        for_each_reference_field(document, types, [&](nlohmann::json& value) {
            Guid guid;
            if (!Guid::parse(value.get<std::string>(), guid)) {
                // A field that already holds a reference, or one a person left
                // empty. Both are text the manifest cannot answer for.
                return true;
            }
            std::string named = assets::reference_for(manifest, guid);
            if (named.empty()) {
                // An identity nothing in this manifest produced. Leaving it
                // alone keeps a document readable rather than dropping what it
                // held.
                return true;
            }
            value = std::move(named);
            ++count;
            return true;
        });
        return count;
    }

} // namespace engine::scene

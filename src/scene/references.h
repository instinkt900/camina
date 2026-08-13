#pragma once

/**
 * @file
 * @brief Finds the fields of a scene document that name an asset.
 *
 * A component field means something because of its type, not because of its
 * text. `reflect::AssetRef` marks a field that holds an asset identity, and
 * `scene::ComponentOps::reference_field_names` carries those names for each
 * registered component. This walk reads that list and touches nothing else.
 *
 * It lives in `scene/` because it needs the component registry, and `assets/`
 * cannot depend on `scene/`. The manifest arrives as a parameter instead.
 */

#include "assets/manifest.h"
#include "scene/component_registry.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <functional>

namespace engine::scene {

    /**
     * @brief What a reference walk does with one field value.
     *
     * The value is always a JSON string, and the visitor may replace it.
     *
     * @param value The field to read or write.
     * @return True to carry on, false to stop the walk.
     */
    using ReferenceFieldVisitor = std::function<bool(nlohmann::json& value)>;

    /**
     * @brief Calls @p visit for each field a document holds that names an asset.
     *
     * The walk knows the document shape. It reads the components of every
     * entity, the field patches of every prefab instance, and the entities an
     * instance added. It never looks at a string outside a described field, so
     * a name that reads like an identity is left alone.
     *
     * A component the registry does not know is skipped, because nothing says
     * which of its fields name an asset.
     *
     * @param document The scene or prefab document, changed in place.
     * @param types The component types to read the tagged field names from.
     * @param visit Called once for each tagged field that holds a string.
     * @return True when the walk finished, false when @p visit stopped it.
     *
     * @code
     * engine::scene::for_each_reference_field(document, engine::scene::components(),
     *                                         [](nlohmann::json& value) {
     *     ENGINE_LOG_INFO("{}", value.get<std::string>());
     *     return true;
     * });
     * @endcode
     */
    bool for_each_reference_field(nlohmann::json& document, const ComponentRegistry& types,
                                  const ReferenceFieldVisitor& visit);

    /**
     * @brief Puts every reference back into a document about to be written.
     *
     * A live document holds identities, because that is what the engine reads.
     * A document a person edits again holds references, because an identity is
     * derived and nobody chose it. This replaces the identity in each field
     * that names an asset with the reference naming it. It is the mirror of
     * what the cooker does on the way in.
     *
     * An identity nothing in the manifest produced is left alone, so the
     * document keeps what it held.
     *
     * @param document The document to change in place.
     * @param manifest The manifest that says what produced what.
     * @param types The component types to read the tagged field names from.
     * @return How many fields were replaced.
     */
    std::size_t restore_references(nlohmann::json& document, const assets::Manifest& manifest,
                                   const ComponentRegistry& types = components());

} // namespace engine::scene

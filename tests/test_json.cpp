// M2.3 tests. The round trip is the milestone's own test, per DESIGN.md
// section 7: register a struct, edit it, and write it back to disk unchanged.

#include "check.h"
#include "math/conventions.h"
#include "reflect/attributes.h"
#include "reflect/json.h"
#include "reflect/reflect.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace {

    using test::check;

    /// A nested type, so the serializer has to recurse.
    struct Transform {
        engine::Vec3 position{ 0.0F, 0.0F, 0.0F };
        engine::Quat rotation{ 1.0F, 0.0F, 0.0F, 0.0F };
        float scale = 1.0F;
    };

    /// The type under test. It holds one of everything the reader handles.
    struct Entity {
        std::string name = "unnamed";
        bool enabled = true;
        std::int32_t hit_points = 100;
        Transform transform;
        std::vector<std::int32_t> tags;
        std::uint64_t cache = 0; ///< Transient. Never written.
        float shield = 50.0F;    ///< Added in schema version 2.
    };

    /// A described enum, with a gap and an out-of-order value so a document that
    /// stored the position rather than the value would be caught. Fog is left out
    /// of the description, the way an enumerator somebody forgot would be.
    enum class Weather : std::uint8_t {
        Clear = 0,
        Rain = 7,
        Snow = 3,
        Fog = 42,
    };

    /// An enum that describes itself no way at all, so this holds the line that
    /// such a type keeps writing its number and nothing that works today stops.
    enum class Quality : std::uint8_t { Low = 0,
                                        High = 1 };

    /// Carries both kinds, so one round trip covers both paths.
    struct Forecast {
        Weather weather = Weather::Clear;
        Quality quality = Quality::Low;
    };

} // namespace

/// @brief An enum describes itself with enumerators() where a struct uses fields().
template <>
struct engine::reflect::Describe<Weather> {
    static constexpr const char* name = "Weather";
    static constexpr auto enumerators() {
        return std::make_tuple(ENGINE_ENUMERATOR(Weather, Clear),
                               ENGINE_ENUMERATOR(Weather, Rain),
                               ENGINE_ENUMERATOR(Weather, Snow));
    }
};

template <>
struct engine::reflect::Describe<Forecast> {
    static constexpr const char* name = "Forecast";
    static constexpr auto fields() {
        return std::make_tuple(ENGINE_FIELD(Forecast, weather),
                               ENGINE_FIELD(Forecast, quality));
    }
};

template <>
struct engine::reflect::Describe<Transform> {
    static constexpr const char* name = "Transform";
    static constexpr auto fields() {
        return std::make_tuple(ENGINE_FIELD(Transform, position),
                               ENGINE_FIELD(Transform, rotation),
                               ENGINE_FIELD(Transform, scale, Range{ 0.01, 100.0, 0.01 }));
    }
};

template <>
struct engine::reflect::Describe<Entity> {
    static constexpr const char* name = "Entity";
    static constexpr auto fields() {
        return std::make_tuple(
            ENGINE_FIELD(Entity, name, Category{ "Identity" }),
            ENGINE_FIELD(Entity, enabled), ENGINE_FIELD(Entity, hit_points),
            ENGINE_FIELD(Entity, transform, Category{ "Placement" }),
            ENGINE_FIELD(Entity, tags), ENGINE_FIELD(Entity, cache, Transient{}),
            ENGINE_FIELD(Entity, shield, Version{ 2 }));
    }
};

namespace {

    namespace rf = engine::reflect;

    /// An object that differs from the default in every field.
    Entity changed_entity() {
        Entity entity;
        entity.name = "player one";
        entity.enabled = false;
        entity.hit_points = 42;
        entity.transform.position = engine::Vec3{ 1.5F, -2.0F, 3.25F };
        entity.transform.rotation = engine::Quat{ 0.5F, 0.5F, -0.5F, 0.5F };
        entity.transform.scale = 2.5F;
        entity.tags = { 7, 8, 9 };
        entity.cache = 0xDEADBEEF;
        entity.shield = 12.5F;
        return entity;
    }

    bool same_transform(const Transform& left, const Transform& right) {
        return left.position == right.position && left.rotation == right.rotation &&
               left.scale == right.scale;
    }

    void test_schema_version() {
        check(rf::schema_version<Transform>() == 1, "a type with no Version is version 1");
        check(rf::schema_version<Entity>() == 2, "the largest field Version sets the type version");
    }

    void test_round_trip() {
        const Entity original = changed_entity();
        const nlohmann::json document = rf::to_json(original);

        Entity loaded;
        check(rf::from_json(document, loaded), "a document this writer made reads back");

        check(loaded.name == original.name, "a string survives the round trip");
        check(loaded.enabled == original.enabled, "a boolean survives the round trip");
        check(loaded.hit_points == original.hit_points, "an integer survives the round trip");
        check(same_transform(loaded.transform, original.transform),
              "a nested described type survives the round trip");
        check(loaded.tags == original.tags, "a list survives the round trip");
        check(loaded.shield == original.shield, "a versioned field survives the round trip");
    }

    void test_transient() {
        const Entity original = changed_entity();
        const nlohmann::json document = rf::to_json(original);

        check(!document.contains("cache"), "the writer skips a Transient field");

        // A file that names the field anyway must not reach the object.
        nlohmann::json tampered = document;
        tampered["cache"] = 12345;

        Entity loaded;
        check(rf::from_json(tampered, loaded), "an unknown key does not fail the read");
        check(loaded.cache == 0, "the reader skips a Transient field");
    }

    void test_version_migration() {
        // A version 1 document predates the shield field. The reader must keep
        // the default and stay quiet.
        nlohmann::json old_document = rf::to_json(changed_entity());
        old_document.erase("shield");
        old_document[rf::kVersionKey] = 1;

        Entity loaded;
        const float default_shield = loaded.shield;
        check(rf::from_json(old_document, loaded), "a version 1 document still loads");
        check(loaded.shield == default_shield, "a field the document predates keeps its default");
        check(loaded.hit_points == 42, "the fields the old document does hold still load");

        // The same document at version 2 is missing a field it should carry.
        // That is a warning and not a failure, so the rest still loads.
        nlohmann::json truncated = old_document;
        truncated[rf::kVersionKey] = 2;

        Entity second;
        check(rf::from_json(truncated, second), "a missing field is a warning, not an error");
        check(second.shield == default_shield, "a missing field keeps its default");
    }

    void test_no_version_key() {
        nlohmann::json document = rf::to_json(changed_entity());
        document.erase(rf::kVersionKey);
        document.erase("shield");

        Entity loaded;
        const float default_shield = loaded.shield;
        check(rf::from_json(document, loaded), "a document with no version key loads");
        check(loaded.shield == default_shield, "no version key counts as the oldest schema");
    }

    void test_enum_round_trip() {
        Forecast forecast;
        forecast.weather = Weather::Snow;
        forecast.quality = Quality::High;

        const nlohmann::json out = rf::to_json(forecast);

        // The name, not the number. A document that stored 3 for Snow would say
        // Rain the moment somebody reordered the enumerators.
        check(out["weather"] == "Snow", "a described enum writes its name");
        check(out["quality"] == 1, "an undescribed enum still writes its number");

        Forecast loaded;
        check(rf::from_json(out, loaded), "the document reads back");
        check(loaded.weather == Weather::Snow, "the described enum survives the trip");
        check(loaded.quality == Quality::High, "so does the undescribed one");
    }

    void test_enum_value_with_no_name() {
        Forecast forecast;
        forecast.weather = Weather::Fog;

        // Writing the number keeps the value. Writing nothing, or writing a
        // default, would turn a description somebody forgot to update into a
        // silent change of the data.
        const nlohmann::json out = rf::to_json(forecast);
        check(out["weather"] == 42, "a value the description leaves out writes its number");
    }

    void test_enum_survives_a_reorder() {
        // The reason for names. Snow is 3 and Rain is 7, so a reader that took
        // the position would answer Rain for this document.
        nlohmann::json document;
        document["weather"] = "Snow";
        document["quality"] = 0;

        Forecast loaded;
        check(rf::from_json(document, loaded), "a hand-written document reads");
        check(loaded.weather == Weather::Snow, "the name picks the value, not the position");
    }

    void test_bad_enum_names() {
        nlohmann::json unknown;
        unknown["weather"] = "Hail";
        unknown["quality"] = 0;

        Forecast loaded;
        check(!rf::from_json(unknown, loaded), "an unknown enumerator name fails the read");

        nlohmann::json a_number;
        a_number["weather"] = 3;
        a_number["quality"] = 0;
        check(!rf::from_json(a_number, loaded),
              "a number where a described enum wants a name fails the read");
    }

    void test_bad_documents() {
        Entity loaded;
        check(!rf::from_json(nlohmann::json::array(), loaded), "an array is not an object");

        nlohmann::json wrong_type = rf::to_json(changed_entity());
        wrong_type["hit_points"] = "not a number";
        check(!rf::from_json(wrong_type, loaded), "a field of the wrong type fails the read");

        nlohmann::json short_vector = rf::to_json(changed_entity());
        short_vector["transform"]["position"] = nlohmann::json::array({ 1.0, 2.0 });
        check(!rf::from_json(short_vector, loaded), "a vector of the wrong length fails the read");

        nlohmann::json bad_version = rf::to_json(changed_entity());
        bad_version[rf::kVersionKey] = "two";
        check(!rf::from_json(bad_version, loaded), "a version key that is not a number fails");

        nlohmann::json negative_version = rf::to_json(changed_entity());
        negative_version[rf::kVersionKey] = -1;
        check(!rf::from_json(negative_version, loaded), "a negative version fails");

        // A program that builds a document in memory writes a plain int, and a
        // parser reading the same text writes an unsigned. Both must load.
        nlohmann::json signed_version = rf::to_json(changed_entity());
        signed_version[rf::kVersionKey] = 2;
        check(rf::from_json(signed_version, loaded), "a signed version number still loads");
    }

    void test_quaternion_order() {
        Transform transform;
        transform.rotation = engine::Quat{ 0.1F, 0.2F, 0.3F, 0.4F };
        const nlohmann::json document = rf::to_json(transform);

        // wxyz, per DESIGN.md section 3. A reader in another language needs this
        // order to be fixed and written down.
        check(document["rotation"][0].get<float>() == 0.1F, "a quaternion writes w first");
        check(document["rotation"][1].get<float>() == 0.2F, "then x");
        check(document["rotation"][3].get<float>() == 0.4F, "and z last");
    }

    void test_file_round_trip() {
        const std::filesystem::path path =
            std::filesystem::temp_directory_path() / "camina_test_entity.json";
        std::filesystem::remove(path);

        const Entity original = changed_entity();
        check(rf::save_json(path, original), "save_json writes the file");
        check(std::filesystem::exists(path), "the file is on disk");

        Entity loaded;
        check(rf::load_json(path, loaded), "load_json reads the file back");
        check(loaded.name == original.name, "the file holds what the object held");
        check(loaded.shield == original.shield, "every field made the trip through disk");

        // Writing what we read must give the same bytes. This is the check that
        // catches a field the writer drops and the reader defaults.
        const std::filesystem::path second_path =
            std::filesystem::temp_directory_path() / "camina_test_entity_2.json";
        check(rf::save_json(second_path, loaded), "the second write succeeds");
        check(rf::to_json(loaded) == rf::to_json(original), "the two documents are equal");

        std::filesystem::remove(path);
        std::filesystem::remove(second_path);

        Entity missing;
        check(!rf::load_json(path, missing), "load_json reports a file that is not there");
    }

} // namespace

int main() {
    std::printf("schema version\n");
    test_schema_version();
    std::printf("round trip\n");
    test_round_trip();
    std::printf("attributes\n");
    test_transient();
    test_version_migration();
    test_no_version_key();
    std::printf("enums\n");
    test_enum_round_trip();
    test_enum_value_with_no_name();
    test_enum_survives_a_reorder();
    test_bad_enum_names();
    std::printf("bad input\n");
    test_bad_documents();
    std::printf("conventions\n");
    test_quaternion_order();
    std::printf("files\n");
    test_file_round_trip();
    return test::report();
}

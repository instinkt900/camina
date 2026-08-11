// M2.1 tests. They cover the descriptor shape, every attribute, and the
// registry. See DESIGN.md section 7.

#include "check.h"
#include "reflect/attributes.h"
#include "reflect/reflect.h"
#include "reflect/registry.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

    using test::check;

    /// A type that exercises every attribute at least once.
    struct Player {
        float health = 100.0F;
        int lives = 3;
        std::string display_name = "player";
        std::uint64_t cache = 0;
        int debug_counter = 0;
        float legacy_scale = 1.0F;
    };

    /// A second type, so the registry has more than one entry to tell apart.
    struct Camera {
        float fov = 60.0F;
        float near_plane = 0.1F;
    };

    /// An enum with a gap and an out-of-order value, because a description that
    /// only ever sees 0, 1, 2 would pass while assuming the position is the value.
    ///
    /// Fog is deliberately left out of the description below. Somebody adding an
    /// enumerator and forgetting to describe it is the realistic way a value
    /// arrives that no name covers.
    enum class Weather : std::uint8_t {
        Clear = 0,
        Rain = 7,
        Snow = 3,
        Fog = 42,
    };

    /// An enum nobody describes, to hold the line that such a type still works.
    enum class Undescribed : std::uint8_t { One,
                                            Two };

} // namespace

template <>
struct engine::reflect::Describe<Player> {
    static constexpr const char* name = "Player";
    static constexpr auto fields() {
        return std::make_tuple(
            ENGINE_FIELD(Player, health, Range{ 0.0, 100.0, 1.0 }, Tooltip{ "Hit points" },
                         Category{ "Combat" }),
            ENGINE_FIELD(Player, lives, Range{ 0.0, 9.0, 1.0 }, Category{ "Combat" }),
            ENGINE_FIELD(Player, display_name, Category{ "Identity" }),
            ENGINE_FIELD(Player, cache, Transient{}, Hidden{}),
            ENGINE_FIELD(Player, debug_counter, EditorOnly{}, ReadOnly{}),
            ENGINE_FIELD(Player, legacy_scale, Version{ 2 }));
    }
};

template <>
struct engine::reflect::Describe<Camera> {
    static constexpr const char* name = "Camera";
    static constexpr auto fields() {
        return std::make_tuple(ENGINE_FIELD(Camera, fov, Range{ 1.0, 179.0, 1.0 }),
                               ENGINE_FIELD(Camera, near_plane));
    }
};

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

namespace {

    namespace rf = engine::reflect;

    void test_describe() {
        check(rf::Described<Player>, "a described type satisfies the concept");
        check(!rf::Described<int>, "an undescribed type does not satisfy the concept");
        check(std::string_view{ rf::type_name<Player>() } == "Player", "type_name reports the name");
        check(rf::field_count<Player>() == 6, "field_count counts every field");
        check(rf::field_count<Camera>() == 2, "field_count is per type");
    }

    void test_visit_reads() {
        const Player player;

        int visited = 0;
        std::vector<std::string> names;
        rf::for_each_field(player, [&](const auto& field, const auto& /*value*/) {
            ++visited;
            names.emplace_back(field.name());
        });

        check(visited == 6, "for_each_field visits every field");
        check(names.at(0) == "health", "ENGINE_FIELD derives the name from the member");
        check(names.at(2) == "display_name", "a name with an underscore survives");
    }

    void test_visit_writes() {
        Player player;

        // A non-const object hands the visitor a mutable reference, which is what
        // an editor needs. The same entry point serves a serializer.
        rf::for_each_field(player, [](const auto& field, auto& value) {
            using Value = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, float>) {
                if (std::string_view{ field.name() } == "health") {
                    value = 42.0F;
                }
            }
        });

        check(player.health == 42.0F, "a visitor can change a field through the reference");
        check(player.lives == 3, "a visitor leaves the other fields alone");
    }

    void test_attributes() {
        const auto fields = rf::Describe<Player>::fields();

        const auto& health = std::get<0>(fields);
        check(rf::has_attribute_v<rf::Range, decltype(health)>, "has reports an attribute that is present");
        check(!rf::has_attribute_v<rf::Transient, decltype(health)>, "has reports an attribute that is absent");
        check(health.attribute<rf::Range>().max == 100.0, "an attribute keeps its value");
        check(std::string_view{ health.attribute<rf::Tooltip>().text } == "Hit points",
              "a field carries more than one attribute");
        check(std::string_view{ health.attribute<rf::Category>().name } == "Combat",
              "the third attribute is reachable too");

        const auto& cache = std::get<3>(fields);
        check(rf::has_attribute_v<rf::Transient, decltype(cache)>, "Transient marks a field the serializer skips");
        check(rf::has_attribute_v<rf::Hidden, decltype(cache)>, "Hidden marks a field the editor does not show");

        const auto& debug_counter = std::get<4>(fields);
        check(rf::has_attribute_v<rf::EditorOnly, decltype(debug_counter)>, "EditorOnly marks editor metadata");
        check(rf::has_attribute_v<rf::ReadOnly, decltype(debug_counter)>, "ReadOnly marks a field the editor locks");

        const auto& legacy = std::get<5>(fields);
        check(legacy.attribute<rf::Version>().added_in == 2, "Version records the schema version");

        const auto& display_name = std::get<2>(fields);
        check(!rf::has_attribute_v<rf::Range, decltype(display_name)>, "a field without an attribute reports false");
    }

    /// The point of the attribute list is that a consumer can filter on it. This
    /// stands in for the serializer that M2.3 builds.
    void test_consumer_filter() {
        const Player player;

        int serialized = 0;
        rf::for_each_field(player, [&](const auto& field, const auto& /*value*/) {
            if constexpr (!rf::has_attribute_v<rf::Transient, decltype(field)>) {
                ++serialized;
            }
        });

        check(serialized == 5, "a consumer can skip Transient at compile time");
    }

    void test_registry() {
        rf::Registry local;
        check(local.size() == 0, "a new registry is empty");

        local.add<Player>();
        local.add<Camera>();
        check(local.size() == 2, "add records a type");

        local.add<Player>();
        check(local.size() == 2, "adding the same type twice does nothing");

        const rf::TypeInfo* found = local.find("Player");
        check(found != nullptr, "find locates a registered type");
        check(found != nullptr && found->field_count == 6, "the entry carries the field count");
        check(found != nullptr && found->size == sizeof(Player), "the entry carries the size");
        check(local.find("Missing") == nullptr, "find reports nothing for an unknown name");

        local.clear();
        check(local.size() == 0, "clear empties the registry");
    }

    void test_constexpr() {
        // The descriptors must be compile-time constants, so a consumer can
        // branch on an attribute with if constexpr and pay nothing at runtime.
        constexpr auto fields = rf::Describe<Camera>::fields();
        static_assert(std::tuple_size_v<decltype(fields)> == 2);
        static_assert(rf::has_attribute_v<rf::Range, decltype(std::get<0>(fields))>);
        static_assert(!rf::has_attribute_v<rf::Range, decltype(std::get<1>(fields))>);
        check(true, "descriptors are usable in a constant expression");
    }

    void test_enum_description() {
        check(rf::DescribedEnum<Weather>, "a described enum satisfies DescribedEnum");
        check(!rf::DescribedEnum<Undescribed>, "an undescribed enum does not");

        // A struct concept and an enum concept must not overlap. A consumer that
        // walks fields would otherwise pick up an enum and fail to compile.
        check(!rf::Described<Weather>, "a described enum is not Described");
        check(rf::Described<Camera>, "a described struct still is");

        check(rf::enumerator_count<Weather>() == 3, "the count comes from the description");
        check(std::string_view(rf::type_name<Weather>()) == "Weather", "an enum carries a name");
    }

    void test_enum_lookup() {
        check(std::string_view(rf::enumerator_name(Weather::Rain)) == "Rain",
              "a value gives its name");

        // Rain is 7 and Snow is 3, so a lookup that returned the position rather
        // than the value would answer Snow here.
        check(std::string_view(rf::enumerator_name(Weather::Snow)) == "Snow",
              "an out-of-order value gives its own name");
        check(rf::enumerator_name(Weather::Fog) == nullptr,
              "a value the description leaves out has no name");

        Weather value = Weather::Clear;
        check(rf::enumerator_value<Weather>("Snow", value) && value == Weather::Snow,
              "a name gives its value");

        Weather untouched = Weather::Rain;
        check(!rf::enumerator_value<Weather>("Hail", untouched) && untouched == Weather::Rain,
              "an unknown name changes nothing and says so");

        // The names reach a file and a script, so a near miss is a mistake worth
        // reporting rather than guessing at.
        check(!rf::enumerator_value<Weather>("snow", untouched), "the match is exact");
    }

    void test_enum_constexpr() {
        // The whole point of describing an enum here rather than at run time.
        static_assert(rf::enumerator_count<Weather>() == 3);
        static_assert(std::string_view(rf::enumerator_name(Weather::Clear)) == "Clear");
        check(true, "an enum description is usable in a constant expression");
    }

} // namespace

int main() {
    std::printf("describe\n");
    test_describe();
    std::printf("visit\n");
    test_visit_reads();
    test_visit_writes();
    std::printf("attributes\n");
    test_attributes();
    test_consumer_filter();
    std::printf("registry\n");
    test_registry();
    std::printf("constexpr\n");
    test_constexpr();
    std::printf("enums\n");
    test_enum_description();
    test_enum_lookup();
    test_enum_constexpr();
    return test::report();
}

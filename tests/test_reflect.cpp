// M2.1 tests. They cover the descriptor shape, every attribute, and the
// registry. See DESIGN.md section 7.

#include "check.h"
#include "core/guid.h"
#include "math/conventions.h"
#include "reflect/attributes.h"
#include "reflect/reflect.h"
#include "reflect/registry.h"
#include "reflect/value.h"

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

    /// One field of every kind a reflect::Value carries, and two it does not.
    ///
    /// M8.2 reads and writes a field by name, so this type is what says which
    /// kinds cross that boundary. The last two are the ceiling issue #271 holds.
    struct Prop {
        bool visible = true;
        float scale = 1.0F;
        int count = 7;
        std::uint8_t small = 200;
        std::string label = "prop";
        engine::Guid asset;
        engine::Vec2 uv{ 0.25F, 0.5F };
        engine::Vec3 position{ 1.0F, 2.0F, 3.0F };
        engine::Vec4 tint{ 0.1F, 0.2F, 0.3F, 0.4F };
        engine::Quat rotation{ 1.0F, 0.0F, 0.0F, 0.0F };
        Weather weather = Weather::Rain;
        Camera nested;                     ///< A described struct. Unsupported, per #271.
        std::vector<int> readings{ 1, 2 }; ///< A list. Unsupported, per #271.
    };

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

/// @brief Describes one field of every kind a Value carries.
template <>
struct engine::reflect::Describe<Prop> {
    static constexpr const char* name = "Prop";
    static constexpr auto fields() {
        return std::make_tuple(
            ENGINE_FIELD(Prop, visible), ENGINE_FIELD(Prop, scale),
            ENGINE_FIELD(Prop, count), ENGINE_FIELD(Prop, small),
            ENGINE_FIELD(Prop, label), ENGINE_FIELD(Prop, asset), ENGINE_FIELD(Prop, uv),
            ENGINE_FIELD(Prop, position), ENGINE_FIELD(Prop, tint),
            ENGINE_FIELD(Prop, rotation), ENGINE_FIELD(Prop, weather),
            ENGINE_FIELD(Prop, nested), ENGINE_FIELD(Prop, readings));
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

    /// Reads one field of @p prop through the registry, by name.
    [[nodiscard]] rf::Value read(const rf::TypeInfo& info, const Prop& prop,
                                 std::string_view field, bool& found) {
        rf::Value value;
        found = info.get_field(&prop, field, value);
        return value;
    }

    void test_dynamic_read() {
        rf::Registry registry;
        registry.add<Prop>();
        const rf::TypeInfo* info = registry.find("Prop");
        check(info != nullptr, "the registry holds Prop");
        if (info == nullptr) {
            return;
        }
        check(info->get_field != nullptr, "and it can read a field by name");

        Prop prop;
        prop.asset = engine::Guid{ 0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL };
        bool found = false;

        const rf::Value visible = read(*info, prop, "visible", found);
        check(found && visible.kind == rf::ValueKind::Bool && visible.boolean,
              "a bool reads as a Bool");

        const rf::Value scale = read(*info, prop, "scale", found);
        check(found && scale.kind == rf::ValueKind::Number && scale.number == 1.0,
              "a float reads as a Number");

        const rf::Value count = read(*info, prop, "count", found);
        check(found && count.kind == rf::ValueKind::Number && count.number == 7.0,
              "an int reads as a Number too");

        // Every width collapses to double, which holds each of them exactly.
        const rf::Value small = read(*info, prop, "small", found);
        check(found && small.number == 200.0, "and so does a narrow unsigned one");

        const rf::Value label = read(*info, prop, "label", found);
        check(found && label.kind == rf::ValueKind::Text && label.text == "prop",
              "a string reads as Text");

        // A Guid declares to_text, so it takes the TextValue branch rather than
        // becoming two large numbers. That is the M4.1 decision holding here.
        const rf::Value asset = read(*info, prop, "asset", found);
        check(found && asset.kind == rf::ValueKind::Text &&
                  asset.text == prop.asset.to_text(),
              "a Guid reads as its text");

        const rf::Value uv = read(*info, prop, "uv", found);
        check(found && uv.kind == rf::ValueKind::Vec2 && uv.vector.x == 0.25F,
              "a Vec2 reads as a Vec2");

        const rf::Value position = read(*info, prop, "position", found);
        check(found && position.kind == rf::ValueKind::Vec3 && position.vector.y == 2.0F,
              "a Vec3 reads as a Vec3");

        const rf::Value tint = read(*info, prop, "tint", found);
        check(found && tint.kind == rf::ValueKind::Vec4 && tint.vector.w == 0.4F,
              "a Vec4 reads as a Vec4");

        const rf::Value rotation = read(*info, prop, "rotation", found);
        check(found && rotation.kind == rf::ValueKind::Quat && rotation.quat.w == 1.0F,
              "a quaternion reads as a Quat");

        // The name and not the number. Weather::Rain is 7, so a Value carrying
        // 7 would pass a weaker check while saying nothing a person can read.
        const rf::Value weather = read(*info, prop, "weather", found);
        check(found && weather.kind == rf::ValueKind::Enum && weather.text == "Rain",
              "a described enum reads as its enumerator name");
    }

    void test_dynamic_read_reports_what_it_cannot_carry() {
        rf::Registry registry;
        registry.add<Prop>();
        const rf::TypeInfo* info = registry.find("Prop");
        if (info == nullptr) {
            check(false, "the registry holds Prop");
            return;
        }

        Prop prop;
        bool found = false;

        // Unsupported is a different answer from None on purpose. The field is
        // there and it cannot cross, which is not the same as a typo in a
        // script. See issue #271.
        const rf::Value nested = read(*info, prop, "nested", found);
        check(found && nested.kind == rf::ValueKind::Unsupported,
              "a nested described struct is found and reported as Unsupported");

        const rf::Value list = read(*info, prop, "readings", found);
        check(found && list.kind == rf::ValueKind::Unsupported,
              "and so is a list");

        rf::Value missing;
        check(!info->get_field(&prop, "nothing_called_this", missing),
              "a name no field carries is not found");
        check(missing.kind == rf::ValueKind::None, "and it reports None rather than Unsupported");
    }

    void test_a_reused_value_carries_nothing_over() {
        rf::Registry registry;
        registry.add<Prop>();
        const rf::TypeInfo* info = registry.find("Prop");
        if (info == nullptr) {
            check(false, "the registry holds Prop");
            return;
        }

        const Prop prop;

        // One Value across several reads, which is what a caller in a loop
        // does. Every earlier test declared a fresh one for each read, and that
        // is exactly what hid this: a branch that fills only the member its
        // kind names leaves the rest holding the last read.
        rf::Value value;
        check(info->get_field(&prop, "label", value), "a string reads");
        check(value.text == "prop", "and it carries the text");

        check(info->get_field(&prop, "scale", value), "a number reads into the same Value");
        check(value.kind == rf::ValueKind::Number, "it reports the new kind");
        check(value.text.empty(), "and the text of the read before is gone");

        check(info->get_field(&prop, "nested", value), "an Unsupported field reads");
        check(value.kind == rf::ValueKind::Unsupported, "it says so");
        check(value.number == 0.0, "and it carries no number from the read before");

        check(!info->get_field(&prop, "nothing_called_this", value), "a miss reports false");
        check(value.kind == rf::ValueKind::None, "and it leaves None rather than the last kind");
    }

    void test_dynamic_write() {
        rf::Registry registry;
        registry.add<Prop>();
        const rf::TypeInfo* info = registry.find("Prop");
        if (info == nullptr) {
            check(false, "the registry holds Prop");
            return;
        }
        check(info->set_field != nullptr, "the registry can write a field by name");

        Prop prop;

        rf::Value number;
        number.kind = rf::ValueKind::Number;
        number.number = 2.5;
        check(info->set_field(&prop, "scale", number), "a Number writes to a float");
        check(prop.scale == 2.5F, "and the field changed");

        // The narrowing happens here, at the field, where the type is known.
        number.number = 42.0;
        check(info->set_field(&prop, "count", number), "and to an int");
        check(prop.count == 42, "and it narrows on the way in");

        rf::Value boolean;
        boolean.kind = rf::ValueKind::Bool;
        boolean.boolean = false;
        check(info->set_field(&prop, "visible", boolean), "a Bool writes to a bool");
        check(!prop.visible, "and the field changed");

        rf::Value text;
        text.kind = rf::ValueKind::Text;
        text.text = "renamed";
        check(info->set_field(&prop, "label", text), "Text writes to a string");
        check(prop.label == "renamed", "and the field changed");

        rf::Value vector;
        vector.kind = rf::ValueKind::Vec3;
        vector.vector = engine::Vec4{ 9.0F, 8.0F, 7.0F, 0.0F };
        check(info->set_field(&prop, "position", vector), "a Vec3 writes to a Vec3");
        check(prop.position.x == 9.0F && prop.position.z == 7.0F, "and every component lands");

        rf::Value enumerated;
        enumerated.kind = rf::ValueKind::Enum;
        enumerated.text = "Snow";
        check(info->set_field(&prop, "weather", enumerated), "an enumerator name writes an enum");
        check(prop.weather == Weather::Snow, "and it is the value, not the position");
    }

    void test_dynamic_write_refuses_a_wrong_kind() {
        rf::Registry registry;
        registry.add<Prop>();
        const rf::TypeInfo* info = registry.find("Prop");
        if (info == nullptr) {
            check(false, "the registry holds Prop");
            return;
        }

        Prop prop;
        const float before = prop.scale;

        // Half-writing a component is worse than refusing, so a wrong kind has
        // to leave the field alone as well as report.
        rf::Value text;
        text.kind = rf::ValueKind::Text;
        text.text = "not a number";
        check(!info->set_field(&prop, "scale", text), "Text does not write to a float");
        check(prop.scale == before, "and the field is untouched");

        // The whole vector, recorded before the call. Comparing one component
        // proved nothing here: the value being written carries x == 1.0F and so
        // does the default position, so the check passed whether the write was
        // refused or half applied.
        const engine::Vec3 position_before = prop.position;
        rf::Value wrong_length;
        wrong_length.kind = rf::ValueKind::Vec2;
        wrong_length.vector = engine::Vec4{ 9.0F, 9.0F, 0.0F, 0.0F };
        check(!info->set_field(&prop, "position", wrong_length),
              "a Vec2 does not write to a Vec3");
        check(prop.position == position_before, "and every component is untouched");

        rf::Value bad_name;
        bad_name.kind = rf::ValueKind::Enum;
        bad_name.text = "Hurricane";
        check(!info->set_field(&prop, "weather", bad_name),
              "an enumerator nobody described is refused");
        check(prop.weather == Weather::Rain, "and the enum keeps its value");

        rf::Value number;
        number.kind = rf::ValueKind::Number;
        number.number = 1.0;
        check(!info->set_field(&prop, "nothing_called_this", number),
              "a name no field carries is refused");
    }

    void test_dynamic_round_trip() {
        rf::Registry registry;
        registry.add<Prop>();
        const rf::TypeInfo* info = registry.find("Prop");
        if (info == nullptr) {
            check(false, "the registry holds Prop");
            return;
        }

        // Reading a field and writing it straight back must change nothing.
        // This is what a script does when it edits one field of a component and
        // puts the whole thing back.
        Prop prop;
        prop.asset = engine::Guid{ 5, 9 };
        Prop copy;

        for (const char* field : info->field_names) {
            rf::Value value;
            if (!info->get_field(&prop, field, value) ||
                value.kind == rf::ValueKind::Unsupported) {
                continue;
            }
            check(info->set_field(&copy, field, value), field);
        }

        check(copy.scale == prop.scale && copy.count == prop.count, "the numbers came back");
        check(copy.label == prop.label, "the string came back");
        check(copy.asset == prop.asset, "the Guid came back");
        check(copy.position == prop.position && copy.tint == prop.tint,
              "the vectors came back");
        check(copy.weather == prop.weather, "and the enum came back");
    }

    void test_field_names_are_listed() {
        rf::Registry registry;
        registry.add<Prop>();
        registry.add<Camera>();
        const rf::TypeInfo* info = registry.find("Prop");
        const rf::TypeInfo* camera = registry.find("Camera");
        if (info == nullptr || camera == nullptr) {
            check(false, "the registry holds both types");
            return;
        }

        check(info->field_names.size() == info->field_count,
              "every described field has a name listed");
        check(std::string_view{ info->field_names.front() } == "visible",
              "and they are in the order Describe gave them");

        // A caller that holds only a type name needs this to answer "what can I
        // read", without building an instance to walk.
        check(camera->field_names.size() == 2, "a second type lists its own fields");
        check(std::string_view{ camera->field_names[1] } == "near_plane",
              "and it is not the first type's list");
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
    std::printf("dynamic field access\n");
    test_dynamic_read();
    test_dynamic_read_reports_what_it_cannot_carry();
    test_a_reused_value_carries_nothing_over();
    test_dynamic_write();
    test_dynamic_write_refuses_a_wrong_kind();
    test_dynamic_round_trip();
    test_field_names_are_listed();
    return test::report();
}

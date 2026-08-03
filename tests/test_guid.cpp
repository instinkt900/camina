// M4.1 tests for the asset identity.
//
// The checks that matter are the round trip through text, and the refusal of
// text that is not a GUID. A reader that accepts bad text writes a wrong
// identity into a cooked file. Nothing reports that until a reference fails to
// resolve, much later.

#include "check.h"
#include "core/guid.h"
#include "reflect/json.h"
#include "reflect/reflect.h"

#include <set>
#include <string>
#include <unordered_set>

namespace {

    using test::check;
    using engine::Guid;

    /// A struct that holds a GUID, so the field reaches the serializer.
    struct Reference {
        Guid target;
        int count = 0;
    };

} // namespace

template <>
struct engine::reflect::Describe<Reference> {
    static constexpr const char* name = "Reference";
    static constexpr auto fields() {
        return std::make_tuple(ENGINE_FIELD(Reference, target), ENGINE_FIELD(Reference, count));
    }
};

namespace {

    void test_default_is_null() {
        const Guid empty;
        check(!empty.valid(), "a default GUID names nothing");
        check(empty.to_text() == "00000000-0000-0000-0000-000000000000",
              "the null GUID writes as all zeros");

        const Guid made = Guid::generate();
        check(made.valid(), "a generated GUID names something");
        check(made != empty, "a generated GUID is not the null GUID");
    }

    void test_text_round_trip() {
        for (int i = 0; i < 100; ++i) {
            const Guid original = Guid::generate();
            Guid parsed;
            check(Guid::parse(original.to_text(), parsed), "the text form parses");
            if (parsed != original) {
                check(false, "a GUID survives the trip through text");
                return;
            }
        }
        check(true, "a GUID survives the trip through text");
    }

    void test_text_shape() {
        const std::string text = Guid::generate().to_text();
        check(text.size() == Guid::kTextLength, "the text form is 36 characters");
        check(text[8] == '-' && text[13] == '-' && text[18] == '-' && text[23] == '-',
              "the dashes sit in the 8-4-4-4-12 places");

        // RFC 4122 version 4 and the matching variant. Other tools check these,
        // so a text form that fails here is not a UUID they will accept.
        check(text[14] == '4', "the version digit says 4");
        const char variant = text[19];
        check(variant == '8' || variant == '9' || variant == 'a' || variant == 'b',
              "the variant digit is one of 8, 9, a, or b");
    }

    void test_case_and_bad_text() {
        Guid original = Guid::generate();
        std::string upper = original.to_text();
        for (char& letter : upper) {
            if (letter >= 'a' && letter <= 'f') {
                letter = static_cast<char>(letter - 'a' + 'A');
            }
        }
        Guid from_upper;
        check(Guid::parse(upper, from_upper) && from_upper == original,
              "uppercase digits read the same as lowercase");

        // A rejected text must leave the caller's GUID alone. Otherwise a bad
        // line in a hand-edited file silently clears a reference.
        Guid keeper = original;

        // These fail on the length, so the reader stops before it reads a
        // digit. They say nothing about what a half-read text leaves behind.
        check(!Guid::parse("", keeper), "empty text is not a GUID");
        check(!Guid::parse("not a guid at all", keeper), "words are not a GUID");
        check(!Guid::parse(original.to_text() + "0", keeper), "an extra digit is refused");
        check(!Guid::parse(original.to_text().substr(1), keeper), "a short text is refused");

        // These are the right length and fail part way through, after the
        // reader has already worked out some of the value. A reader that built
        // the result in the caller's GUID rather than in a local would leave
        // part of a new value behind here.
        std::string moved_dash = original.to_text();
        std::swap(moved_dash[8], moved_dash[9]);
        check(!Guid::parse(moved_dash, keeper), "a dash in the wrong place is refused");

        // The last character, so 31 of the 32 digits are already read.
        std::string bad_digit = original.to_text();
        bad_digit[Guid::kTextLength - 1] = 'g';
        check(!Guid::parse(bad_digit, keeper), "a digit outside hexadecimal is refused");

        std::string no_dashes = original.to_text();
        std::erase(no_dashes, '-');
        check(!Guid::parse(no_dashes, keeper), "the packed form without dashes is refused");

        // One check, after every refusal above. Asking earlier would only prove
        // it for the ones that stop on the length.
        check(keeper == original, "no refused text changed the GUID");
    }

    void test_generate_does_not_repeat() {
        constexpr int kCount = 10000;
        std::unordered_set<Guid> seen;
        for (int i = 0; i < kCount; ++i) {
            seen.insert(Guid::generate());
        }
        check(seen.size() == kCount, "10000 generated GUIDs are all different");
    }

    void test_ordering() {
        // std::set needs operator<, and a cooker writes a manifest in a stable
        // order so that two runs give the same file.
        std::set<Guid> ordered;
        const Guid low{ .high = 1, .low = 0 };
        const Guid high{ .high = 1, .low = 5 };
        ordered.insert(high);
        ordered.insert(low);
        check(*ordered.begin() == low, "the smaller GUID sorts first");
        check(low < high && !(high < low), "the order is strict");
    }

    void test_field_round_trip() {
        // The point of the text form. A GUID reaches a file as one string that a
        // person can read in a diff, not as a nested object of two numbers.
        const Reference original{ .target = Guid::generate(), .count = 7 };
        const nlohmann::json document = engine::reflect::to_json(original);

        check(document.at("target").is_string(), "a GUID field writes as a string");
        check(document.at("target").get<std::string>() == original.target.to_text(),
              "and the string is the text form");

        Reference loaded;
        check(engine::reflect::from_json(document, loaded), "the document loads");
        check(loaded.target == original.target, "the GUID came back");
        check(loaded.count == original.count, "the ordinary field still works");
    }

    void test_field_refuses_bad_documents() {
        const Reference original{ .target = Guid::generate(), .count = 7 };

        nlohmann::json wrong_type = engine::reflect::to_json(original);
        wrong_type["target"] = 12;
        Reference loaded;
        check(!engine::reflect::from_json(wrong_type, loaded),
              "a number where a GUID belongs is refused");

        nlohmann::json bad_text = engine::reflect::to_json(original);
        bad_text["target"] = "nonsense";
        Reference kept;
        kept.target = original.target;
        check(!engine::reflect::from_json(bad_text, kept), "text that is not a GUID is refused");
        check(kept.target == original.target, "and the field keeps the GUID it held");
    }

    void test_derive_is_stable() {
        const Guid parent = Guid::generate();

        // The whole point. Nothing is stored, so the answer has to come out the
        // same on every run and on every machine. A cook that derived a new GUID
        // each time would break every prefab that named a mesh.
        check(Guid::derive(parent, "mesh", 0) == Guid::derive(parent, "mesh", 0),
              "the same parent, kind, and index give the same GUID");

        const Guid derived = Guid::derive(parent, "mesh", 0);
        check(derived.valid(), "a derived GUID is not null");
        check(derived != parent, "and it is not the parent");

        // Version 8 is what RFC 9562 reserves for a custom scheme. Keeping it
        // apart from the version 4 that generate() makes means a derived GUID
        // can never collide with one somebody generated.
        check(derived.to_text().at(14) == '8', "a derived GUID carries UUID version 8");
        check(Guid::generate().to_text().at(14) == '4', "and a generated one carries version 4");

        // A parent with no identity has nothing to derive from. Answering
        // anything else would give every part of every broken asset one GUID.
        check(!Guid::derive(Guid{}, "mesh", 0).valid(), "a null parent derives nothing");
    }

    void test_derive_separates_its_inputs() {
        const Guid parent = Guid::generate();
        const Guid other = Guid::generate();

        check(Guid::derive(parent, "mesh", 0) != Guid::derive(parent, "mesh", 1),
              "two indices give two GUIDs");
        check(Guid::derive(parent, "mesh", 0) != Guid::derive(parent, "material", 0),
              "two kinds give two GUIDs");
        check(Guid::derive(parent, "mesh", 0) != Guid::derive(other, "mesh", 0),
              "two parents give two GUIDs");

        // The two halves must fold their material differently. Folding it the
        // same way would make every derived GUID a pair of equal halves, which
        // halves the space and shows up as a repeat far sooner than it should.
        const Guid derived = Guid::derive(parent, "mesh", 0);
        check(derived.high != derived.low, "the two halves of a derived GUID differ");

        // A run of derivations from one parent, all distinct. This is the shape
        // a glTF file with many meshes produces.
        constexpr std::uint32_t kParts = 64;
        std::set<Guid> seen;
        for (std::uint32_t at = 0; at < kParts; ++at) {
            seen.insert(Guid::derive(parent, "mesh", at));
        }
        check(seen.size() == kParts, "64 parts of one asset give 64 distinct GUIDs");
    }

} // namespace

int main() {
    test::section("the value");
    test_default_is_null();
    test_generate_does_not_repeat();
    test_ordering();
    test::section("text");
    test_text_round_trip();
    test_text_shape();
    test_case_and_bad_text();
    test::section("reflection");
    test_field_round_trip();
    test_field_refuses_bad_documents();
    test::section("derived identities");
    test_derive_is_stable();
    test_derive_separates_its_inputs();
    return test::report();
}

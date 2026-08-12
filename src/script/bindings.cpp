#include "script/bindings.h"

#include "core/log.h"
#include "math/conventions.h"

#include <sol/sol.hpp>

#include <cmath>
#include <string>

namespace engine::script {

    namespace {

        /// Below this a vector is too short to normalize without filling it with NaN.
        constexpr float kShortestVector = 1.0e-6F;

        [[nodiscard]] std::string vec3_text(const Vec3& value) {
            return "vec3(" + std::to_string(value.x) + ", " + std::to_string(value.y) + ", " +
                   std::to_string(value.z) + ")";
        }

        [[nodiscard]] std::string quat_text(const Quat& value) {
            return "quat(" + std::to_string(value.w) + ", " + std::to_string(value.x) + ", " +
                   std::to_string(value.y) + ", " + std::to_string(value.z) + ")";
        }

        /**
         * Normalizes, or returns the zero vector.
         *
         * glm::normalize divides by the length, so a vector of nearly zero
         * fills every component with NaN and that spreads through whatever the
         * script does next. Returning zero is wrong in a way a person can see.
         */
        [[nodiscard]] Vec3 safe_normalize(const Vec3& value) {
            const float length = glm::length(value);
            if (length < kShortestVector) {
                return Vec3{ 0.0F, 0.0F, 0.0F };
            }
            return value / length;
        }

    } // namespace

    void bind_math(sol::state& lua) {
        lua.new_usertype<Vec3>(
            "vec3",
            // call_constructor is what makes `vec3(1, 2, 3)` work. Without it
            // sol2 binds the constructors to `vec3.new`, which is not the
            // spelling an author reaches for.
            sol::call_constructor, sol::constructors<Vec3(), Vec3(float, float, float)>(),

            "x", &Vec3::x, "y", &Vec3::y, "z", &Vec3::z,

            sol::meta_function::addition,
            [](const Vec3& a, const Vec3& b) { return a + b; },
            sol::meta_function::subtraction,
            [](const Vec3& a, const Vec3& b) { return a - b; },
            sol::meta_function::unary_minus, [](const Vec3& a) { return -a; },

            // Both orders, because an author writes each without thinking.
            sol::meta_function::multiplication,
            sol::overload([](const Vec3& a, float s) { return a * s; },
                          [](float s, const Vec3& a) { return a * s; },
                          [](const Vec3& a, const Vec3& b) { return a * b; }),
            sol::meta_function::division, [](const Vec3& a, float s) { return a / s; },

            sol::meta_function::equal_to,
            [](const Vec3& a, const Vec3& b) { return a == b; },
            sol::meta_function::to_string, &vec3_text,

            "length", [](const Vec3& a) { return glm::length(a); },
            // Named for what it gives back rather than what it does, because it
            // returns a new vector and leaves this one alone.
            "normalized", &safe_normalize,
            "dot", [](const Vec3& a, const Vec3& b) { return glm::dot(a, b); },
            "cross", [](const Vec3& a, const Vec3& b) { return glm::cross(a, b); });

        lua.new_usertype<Quat>(
            "quat",
            // wxyz, per DESIGN.md section 3. The order is the one the engine
            // stores and the one a scene file writes, so a script that reads a
            // rotation and builds one sees the same shape.
            sol::call_constructor,
            sol::constructors<Quat(), Quat(float, float, float, float)>(),

            "w", &Quat::w, "x", &Quat::x, "y", &Quat::y, "z", &Quat::z,

            sol::meta_function::multiplication,
            sol::overload([](const Quat& a, const Quat& b) { return a * b; },
                          // Turning a vector by a rotation, which is the whole
                          // reason a game script holds a quaternion.
                          [](const Quat& q, const Vec3& v) { return q * v; }),
            sol::meta_function::equal_to,
            [](const Quat& a, const Quat& b) { return a == b; },
            sol::meta_function::to_string, &quat_text,

            "normalized", [](const Quat& q) { return glm::normalize(q); },
            "inverse", [](const Quat& q) { return glm::inverse(q); });

        // A rotation of so many radians about an axis, which is what an author
        // reaches for rather than four numbers they have to work out.
        lua.set_function("quat_from_axis_angle", [](const Vec3& axis, float radians) {
            const Vec3 unit = safe_normalize(axis);
            if (unit == Vec3{ 0.0F, 0.0F, 0.0F }) {
                // glm::angleAxis wants a unit axis and does not check. Given a
                // zero axis it works out cos(angle / 2) for w and leaves the
                // rest zero, so half a turn gives quat(0, 0, 0, 0).
                //
                // That is not a rotation. It survives a multiply by a vector
                // and a matrix cast, because both happen to read as identity
                // with every term zero, so nothing reports it. It does not
                // survive being written to a rotation field and read back, and
                // it has no unit length for anything that assumes one.
                //
                // An axis of no direction names no rotation, so identity is the
                // honest answer.
                return Quat{ 1.0F, 0.0F, 0.0F, 0.0F };
            }
            return glm::angleAxis(radians, unit);
        });
    }

    void bind_random(sol::state& lua) {
        sol::table math = lua["math"];

        // Held before the replacement below hides it, so the seeded form still
        // has something to call.
        const sol::protected_function original = math["randomseed"];
        if (!original.valid()) {
            ENGINE_LOG_WARN("The Lua math library has no randomseed. A run using "
                            "math.random will not be reproducible.");
            return;
        }

        // Lua 5.4 seeds from the clock at startup, so this has to run whatever
        // the script does. A fixed seed is what makes two runs of one command
        // produce the same numbers. See DESIGN.md section 9.
        const sol::protected_function_result seeded = original(kRandomSeed);
        if (!seeded.valid()) {
            ENGINE_LOG_WARN("The Lua random source refused a seed. A run using "
                            "math.random will not be reproducible.");
        }

        // math.randomseed() with no argument seeds from the clock again, so a
        // script could undo the line above without meaning to. This replaces it
        // with a form that has no such spelling.
        math.set_function("randomseed", [original](const sol::object& value) {
            if (!value.is<double>()) {
                ENGINE_LOG_ERROR("math.randomseed needs a number. Seeding from the clock "
                                 "would make a run different every time, so that form is "
                                 "not available. The seed is unchanged.");
                return;
            }
            (void)original(value.as<double>());
        });
    }

} // namespace engine::script

#pragma once

/**
 * @file
 * @brief The hand-written half of the script surface.
 *
 * `DESIGN.md` §10 M8 asks for a curated surface and says why: a fully mechanical
 * binding produces an API that nobody enjoys. `script/host.cpp` holds the other
 * half, which reads any component through the reflection descriptors and needs
 * no code written for each type.
 *
 * **This header names sol2, so nothing outside `src/script/` may include it.**
 * `script/host.h` is the public face and names no sol2 type, which is what keeps
 * Lua a PRIVATE link of `engine_core`.
 */

/**
 * @page script_determinism What a script may do and stay reproducible
 *
 * `DESIGN.md` §9 rests a reproducible run on the fixed step, and M7 proved
 * three offscreen runs byte-identical. Lua can lose that in two ways, and
 * neither one announces itself. The first symptom is a determinism test that
 * fails once in ten runs.
 *
 * **`math.random` is settled.** bind_random() seeds it from a fixed value and
 * takes the clock-seeded spelling away, so a script cannot undo that by
 * accident. Nothing is asked of the author.
 *
 * **`pairs` is not, and the author has to know.** Lua 5.4 seeds its string hash
 * from the clock and from an address, so the walk order of a **string-keyed**
 * table changes between two runs of one binary. A script that iterates one and
 * acts on the order is not reproducible.
 *
 * This is not a rule about tables in general. The array part is ordered, so
 * `ipairs` and an integer-keyed table are already safe. **Use `ipairs` and a
 * list when the order decides anything.**
 *
 * Making `pairs` itself deterministic means patching `luai_makeseed`, and hard
 * rule 4.4 then turns Lua from a Conan package into a vendored dependency. That
 * is a large price for one function, so the engine does not pay it and says so
 * here instead. See issue #262.
 */

#include <sol/forward.hpp>

namespace engine::script {

    /**
     * @brief The seed the Lua random source starts from.
     *
     * Any fixed value works. What matters is that it is fixed: Lua 5.4 seeds
     * `math.random` from the clock when nobody says otherwise, and a run that
     * did that could not be reproduced. See `DESIGN.md` §9.
     */
    inline constexpr int kRandomSeed = 20260812;

    /**
     * @brief Binds `vec3` and `quat`, with the operators an author expects.
     *
     * A script writes `a + b` and `q * v` rather than calling a function for
     * each. Both are the engine types, so a value crosses to a component field
     * with no conversion.
     *
     * Vec2 and Vec4 stay plain tables. No component carries either one, and
     * rule 4.6 says to build what the sandbox needs.
     *
     * @param lua The state to bind into.
     */
    void bind_math(sol::state& lua);

    /**
     * @brief Makes the Lua random source reproducible, and closes the way back.
     *
     * Seeds `math.random` from ::kRandomSeed, then replaces `math.randomseed`
     * with a form that requires a number. Lua 5.4 seeds from the clock when
     * `math.randomseed()` is called with no arguments, and a script that did
     * that would break a reproducible run with nothing to show for it.
     *
     * So the guarantee is structural rather than a note somebody has to read.
     *
     * @param lua The state to bind into.
     */
    void bind_random(sol::state& lua);

} // namespace engine::script

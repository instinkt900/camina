#pragma once

/**
 * @file
 * @brief What a cooked script is, shared by the cooker and the runtime.
 *
 * A cooked script has no header struct, and that is the whole format. The bytes
 * are the Lua source text, exactly as a person wrote them. So this file carries
 * a name and a decision rather than a layout.
 *
 * **The cooker still holds a rule for it, and the rule copies the bytes.** A
 * `.lua` would reach the copy fallback on its own and come out with the same
 * GUID, the same sidecar, and the same manifest entry. A rule is worth having
 * anyway, for three reasons.
 *
 * - The identity of a script stops depending on the absence of a rule.
 * - Issue #178 wants the fallback to stop writing a sidecar for a file that has
 *   no consumer. A script has a consumer, and a rule keeps it out of that test.
 * - Issue #258 has somewhere to add a precompile step later.
 *
 * **Why the bytes are source text and not bytecode.** Lua writes the
 * instruction size, the integer size, the number size, and the byte order into
 * the header of a binary chunk, and the loader refuses a chunk that disagrees.
 * Both engine targets are x86-64 and little-endian and build Lua the same way,
 * so a chunk would in fact load across them today. The format promises nothing
 * about that, and bytecode buys nothing measurable for a handful of small
 * scripts.
 *
 * `luaL_loadbuffer` reads source text or bytecode through one call, because it
 * reads the signature and picks. So adding bytecode later changes the cooker
 * and never the loader. See issue #258.
 */

namespace engine::assets {

    /// @brief The name a script file carries, before and after the cook.
    inline constexpr const char* kScriptExtension = ".lua";

} // namespace engine::assets

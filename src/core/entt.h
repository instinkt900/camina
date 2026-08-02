#pragma once

/**
 * @file
 * @brief Points the EnTT assertions at the engine assertion macros.
 *
 * EnTT guards its own preconditions with `ENTT_ASSERT`. When nothing defines
 * that macro, EnTT falls back to `assert()`, and `NDEBUG` removes `assert()`
 * from a RelWithDebInfo build. That is the build CI and ctest run, so every
 * EnTT precondition was unchecked there. A `get<T>()` for a component that an
 * entity does not carry read past the end of a pool and killed the process
 * with no message.
 *
 * This file defines `ENTT_ASSERT` before EnTT can, so the check runs and
 * reports the file, the line, and the condition through the engine log.
 * `ENGINE_ASSERT` is the right level, because a missing component is a
 * programmer error. It stays active in every build except Release, which
 * matches the engine policy in core/assert.h.
 *
 * @warning Include this file before any EnTT header. Every engine header that
 * includes one does that already. The guard below turns a wrong order into a
 * build error rather than a quiet loss of the check.
 */

/// @cond
// EnTT sets this when its own config header runs. Reaching it first means EnTT
// already chose the fallback, and a definition here would arrive too late.
#if defined(ENTT_CONFIG_CONFIG_H)
#error "core/entt.h must come before any EnTT header, or ENTT_ASSERT keeps the assert() fallback that NDEBUG removes."
#endif
/// @endcond

#include "core/assert.h"

/**
 * @brief The macro EnTT calls for each of its own preconditions.
 *
 * @param condition The expression EnTT needs to be true.
 * @param message The text EnTT supplies with the check.
 */
#define ENTT_ASSERT(condition, message) ENGINE_ASSERT(condition, message)

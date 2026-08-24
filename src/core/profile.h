#pragma once

/**
 * @file
 * @brief Profiler markers, wrapping Tracy.
 *
 * When the tracy package has enable=False, every Tracy macro compiles to nothing,
 * so these markers cost nothing.
 *
 * @warning **Leave Tracy on under a sanitizer.** A marker is the only reader of a
 * name or a text a caller passes, so turning Tracy off hides every lifetime bug
 * on that path. Issue #453 is one: it lives on ENGINE_PROFILE_ZONE_TEXT, and a
 * sanitizer run with Tracy off cannot see it.
 */

#include <tracy/Tracy.hpp>

/**
 * @brief Marks the end of a frame.
 *
 * Call this once per frame, at the same point each time. The profiler measures
 * frame time between consecutive calls.
 */
#define ENGINE_PROFILE_FRAME() FrameMark

/// @brief Times the enclosing scope. The zone takes the name of the function.
#define ENGINE_PROFILE_ZONE() ZoneScoped

/**
 * @brief Times the enclosing scope under an explicit name.
 * @param name A string literal naming the zone.
 */
#define ENGINE_PROFILE_ZONE_N(name) ZoneScopedN(name)

/**
 * @brief Attaches text to the zone the enclosing scope already opened.
 *
 * Use this when the name is known only at run time, which ENGINE_PROFILE_ZONE_N
 * cannot express. Open a zone with a literal first, then hang the detail on it.
 *
 * @param text A string. Tracy copies it, so it does not have to outlive the call.
 * @param size How many characters to copy, not counting a terminator.
 */
#define ENGINE_PROFILE_ZONE_TEXT(text, size) ZoneText(text, size)

/**
 * @brief Names the calling thread in the profiler view.
 * @param name A null-terminated string. Tracy copies it.
 */
#define ENGINE_PROFILE_THREAD(name) tracy::SetThreadName(name)

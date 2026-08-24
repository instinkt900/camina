#pragma once

/**
 * @file
 * @brief What miniaudio is built with, for every file that includes it.
 *
 * The declarations and the implementation must agree about these macros, so
 * both `audio/device.cpp` and `audio/miniaudio_impl.cpp` include this first. A
 * macro set in one and not in the other changes what a struct looks like, and
 * that failure links cleanly and goes wrong at run time.
 *
 * The decoders stay on. M11.2 streams a long track from its encoded bytes, and
 * that path reads them through miniaudio.
 *
 * @warning This header is inside the containment line. Nothing outside
 *          `src/audio/` may include it. See
 *          `scripts/check-miniaudio-containment.sh`.
 */

/// @brief The engine writes no audio file. This takes the encoders out.
#define MA_NO_ENCODING

/// @brief Nothing synthesizes a waveform. `scripts/` generates the sandbox sounds.
#define MA_NO_GENERATION

/**
 * @brief No device layer in a build with no audio.
 *
 * The decoders are needed either way, because `import/sound.cpp` decodes a
 * short effect at cook time and a cook must produce the same bytes whatever
 * `with_audio` says. The device layer is not: nothing opens one in a build
 * with the option off.
 *
 * So there is one miniaudio implementation for each binary and never two.
 * `apps/editor` links `engine::core` and `engine::import` together, and two
 * implementations in one binary are duplicate symbols at best and two
 * disagreeing struct layouts at worst.
 *
 * @warning This macro changes what a struct looks like, so every translation
 *          unit that includes miniaudio has to agree about it. That is what
 *          this file is for. See issue #424.
 */
#if !defined(ENGINE_WITH_AUDIO)
#define MA_NO_DEVICE_IO
#endif

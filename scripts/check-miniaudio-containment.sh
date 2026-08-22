#!/usr/bin/env bash
#
# Keeps miniaudio inside src/audio/.
#
# DESIGN.md section 5 rejects an audio plugin ABI, so `IAudioDevice` is not
# there to carry a second backend. It is there to keep one library out of the
# rest of the engine. miniaudio is one very large header that declares a mixer,
# a resource manager, a decoder and a device layer, and every file that includes
# it pays for all of it and can reach into all of it.
#
# This is the same shape as check-box3d-containment.sh and
# check-vulkan-containment.sh. All three call containment.sh, which is the
# search itself.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/containment.sh
. "${script_dir}/containment.sh"

# The directive is anchored to the start of a line. Without the anchor, a line
# that only writes about an include, in a comment or in a string, reads as a
# violation.
#
# audio/miniaudio_config.h is in the pattern as well as miniaudio.h itself. It
# carries the macros the header is built with, so a file that includes it is
# inside the same seam whether it includes miniaudio or not.
pattern='^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"](miniaudio\.h|audio/miniaudio_config\.h)'

check_containment \
    "check-miniaudio-containment" \
    "src/audio/" \
    "${pattern}" \
    "A miniaudio header" \
    "the audio::IAudioDevice interface" \
    "DESIGN.md section 10 M11" \
    "${1:-.}"

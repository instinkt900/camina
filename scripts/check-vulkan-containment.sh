#!/usr/bin/env bash
#
# Enforces rule 4.1 in DESIGN.md.
#
# Only files under src/gfx/vulkan/ can include a Vulkan header. Every layer
# above talks in gfx:: types. This keeps the later plugin ABI extraction
# mechanical instead of a rewrite.
#
# containment.sh holds the search itself, which the Box3D check shares.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/containment.sh
. "${script_dir}/containment.sh"

# The directive is anchored to the start of a line. Without the anchor, a line
# that only writes about an include, in a comment or in a string, reads as a
# violation.
pattern='^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"](vulkan/|volk\.h|vk_mem_alloc\.h|vulkan\.h)'

check_containment \
    "check-vulkan-containment" \
    "src/gfx/vulkan/" \
    "${pattern}" \
    "Rule 4.1 violation. A Vulkan header" \
    "gfx:: types" \
    "DESIGN.md section 4" \
    "${1:-.}"

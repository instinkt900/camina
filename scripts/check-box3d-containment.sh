#!/usr/bin/env bash
#
# Keeps Box3D inside src/physics/.
#
# Box3D is an alpha at a pinned commit. DESIGN.md section 5 says we will read,
# patch, and update it more than any other dependency. Every such update is a
# diff against one directory rather than against the whole engine, and that
# only holds while one directory includes it.
#
# This is the same shape as check-vulkan-containment.sh, which enforces rule
# 4.1. The Box3D rule is narrower. It protects an update path rather than a
# later ABI extraction. Both call containment.sh, which is the search itself.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/containment.sh
. "${script_dir}/containment.sh"

# The directive is anchored to the start of a line. Without the anchor, a line
# that only writes about an include, in a comment or in a string, reads as a
# violation.
pattern='^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]box3d/'

check_containment \
    "check-box3d-containment" \
    "src/physics/" \
    "${pattern}" \
    "A Box3D header" \
    "engine types" \
    "DESIGN.md section 5" \
    "${1:-.}"

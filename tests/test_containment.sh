#!/usr/bin/env bash
#
# Drives both containment checks over a fixture tree.
#
# Neither script had a test. The Vulkan one passed a real violation whenever the
# offending line happened to name the allowed directory, and nothing reported
# that. A check that fails open is worse than no check, because the passing
# output looks the same either way. See issue #231.
#
# Every case below is written twice, once for each rule, because the two used to
# be separate copies and the whole point is that they now behave alike.

set -uo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"

vulkan_check="${repo_root}/scripts/check-vulkan-containment.sh"
box3d_check="${repo_root}/scripts/check-box3d-containment.sh"

failures=0

pass() {
    echo "  pass  $1"
}

fail() {
    echo "  FAIL  $1"
    failures=$((failures + 1))
}

# Runs a check over a fixture tree and compares the exit code.
#
# $1 The script to run.
# $2 The tree to run it over.
# $3 The exit code expected, 0 for a clean tree and 1 for a violation.
# $4 What the case is called.
expect_exit() {
    local check="$1"
    local tree="$2"
    local want="$3"
    local what="$4"

    local output
    local got=0
    output="$(bash "${check}" "${tree}" 2>&1)" || got=$?

    if [ "${got}" -eq "${want}" ]; then
        pass "${what}"
        return 0
    fi

    fail "${what}: expected exit ${want} and got ${got}"
    echo "${output}" | sed 's/^/        /'
    return 1
}

# Writes a file and every directory above it.
write_file() {
    mkdir -p "$(dirname "$1")"
    printf '%s\n' "$2" > "$1"
}

fixture="$(mktemp -d)"
trap 'rm -rf "${fixture}"' EXIT

# One tree for each case, so a case cannot be hidden by another one's violation.
#
# case_dir <name> gives a tree that already holds the allowed directory, so the
# "no source directories found" early return is never what is being measured.
case_dir() {
    local dir="${fixture}/$1"
    mkdir -p "${dir}/src/gfx/vulkan" "${dir}/src/physics"
    printf '%s' "${dir}"
}

echo "containment: a clean tree"
clean="$(case_dir clean)"
write_file "${clean}/src/render/mesh_pass.cpp" '#include "gfx/device.h"'
expect_exit "${vulkan_check}" "${clean}" 0 "a tree with no Vulkan include passes"
expect_exit "${box3d_check}" "${clean}" 0 "and the same tree passes the Box3D check"

echo "containment: a real violation"
plain="$(case_dir plain)"
write_file "${plain}/src/render/mesh_pass.cpp" '#include <volk.h>'
write_file "${plain}/src/scene/world.cpp" '#include <box3d/box3d.h>'
expect_exit "${vulkan_check}" "${plain}" 1 "a Vulkan include outside the allowed directory fails"
expect_exit "${box3d_check}" "${plain}" 1 "and a Box3D include outside src/physics/ fails"

# The bug this issue was filed for. The old Vulkan check filtered the whole grep
# record, which is path:line:text, so the allowed prefix matched against the
# text as well as the path.
echo "containment: a violation whose line names the allowed directory"
named="$(case_dir named)"
write_file "${named}/src/render/mesh_pass.cpp" \
    '#include <volk.h> // moved here from src/gfx/vulkan/ in a hurry'
write_file "${named}/src/scene/world.cpp" \
    '#include <box3d/box3d.h> // lifted out of src/physics/ for now'
expect_exit "${vulkan_check}" "${named}" 1 \
    "a Vulkan violation naming src/gfx/vulkan/ on the same line still fails"
expect_exit "${box3d_check}" "${named}" 1 \
    "and a Box3D violation naming src/physics/ still fails"

# The other direction. This one is loud rather than silent, so it costs a person
# a confusing CI failure rather than a missed violation.
#
# The comment must not name the allowed directory. The old Vulkan check had both
# faults at once, and on a line that named it they cancelled: the unanchored
# pattern matched the comment, and then the record filter dropped the match. So
# a fixture that mentions the directory measures nothing about the anchor.
echo "containment: a mention that is not an include"
mention="$(case_dir mention)"
write_file "${mention}/src/render/notes.h" \
    '// Do not #include <volk.h> here. Rule 4.1 says where it belongs.'
write_file "${mention}/src/scene/notes.h" \
    '// Never #include <box3d/box3d.h> outside the one directory that may.'
expect_exit "${vulkan_check}" "${mention}" 0 "a comment writing about the include passes"
expect_exit "${box3d_check}" "${mention}" 0 "and so does the Box3D one"

echo "containment: an include inside the allowed directory"
inside="$(case_dir inside)"
write_file "${inside}/src/gfx/vulkan/vk_device.cpp" '#include <volk.h>'
write_file "${inside}/src/physics/world.cpp" '#include <box3d/box3d.h>'
expect_exit "${vulkan_check}" "${inside}" 0 "the allowed directory may include the header"
expect_exit "${box3d_check}" "${inside}" 0 "and so may src/physics/"

# A prefix compare with no trailing slash would call this allowed.
echo "containment: a path that only looks like the allowed one"
lookalike="$(case_dir lookalike)"
write_file "${lookalike}/src/gfx/vulkan_helpers/helper.cpp" '#include <volk.h>'
write_file "${lookalike}/src/physics_debug/draw.cpp" '#include <box3d/box3d.h>'
expect_exit "${vulkan_check}" "${lookalike}" 1 "src/gfx/vulkan_helpers/ is not src/gfx/vulkan/"
expect_exit "${box3d_check}" "${lookalike}" 1 "and src/physics_debug/ is not src/physics/"

# An indented directive is still a directive.
echo "containment: an indented include"
indented="$(case_dir indented)"
write_file "${indented}/src/render/mesh_pass.cpp" '  #  include <volk.h>'
write_file "${indented}/src/scene/world.cpp" '  #  include <box3d/box3d.h>'
expect_exit "${vulkan_check}" "${indented}" 1 "an indented Vulkan include still fails"
expect_exit "${box3d_check}" "${indented}" 1 "and an indented Box3D include still fails"

echo
if [ "${failures}" -ne 0 ]; then
    echo "${failures} containment test(s) failed."
    exit 1
fi

echo "All tests passed."
exit 0

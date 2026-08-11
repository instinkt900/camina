#!/usr/bin/env bash
#
# Keeps Box3D inside src/physics/.
#
# Box3D is an alpha at a pinned commit, and DESIGN.md section 5 says we will
# read, patch, and update it more than any other dependency. Every such update
# is a diff against one directory, not against the whole engine, and that only
# holds while one directory includes it.
#
# This is the same shape as check-vulkan-containment.sh, which enforces rule
# 4.1. The Box3D rule is narrower: it protects an update path rather than a
# later ABI extraction.

set -euo pipefail

root="${1:-.}"
allowed="src/physics/"

search_dirs=()
for dir in src apps tools tests sandbox; do
    if [ -d "${root}/${dir}" ]; then
        search_dirs+=("${root}/${dir}")
    fi
done

if [ ${#search_dirs[@]} -eq 0 ]; then
    echo "check-box3d-containment: no source directories found under ${root}"
    exit 0
fi

pattern='#[[:space:]]*include[[:space:]]*[<"]box3d/'

violations="$(grep -rnE "${pattern}" "${search_dirs[@]}" 2>/dev/null | grep -v "${allowed}" || true)"

if [ -n "${violations}" ]; then
    echo "A Box3D header is included outside ${allowed}"
    echo
    echo "${violations}"
    echo
    echo "Move the code under ${allowed}, or express it with engine types."
    echo "See DESIGN.md section 5."
    exit 1
fi

echo "check-box3d-containment: pass"
exit 0

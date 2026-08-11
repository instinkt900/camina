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

# The directive is anchored to the start of a line. Without the anchor, a line
# that only writes about an include, in a comment or in a string, reads as a
# violation.
pattern='^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]box3d/'

# Decide by file name, not by the whole grep record. A record carries the path,
# the line number, and the line. Filtering the record lets a violation outside
# src/physics/ pass whenever the line itself mentions that directory. That
# failure is silent, which is the worse direction for a check to fail in.
violations=""
while IFS= read -r record; do
    [ -n "${record}" ] || continue
    file="${record%%:*}"
    relative="${file#"${root}/"}"
    case "${relative}" in
        "${allowed}"*) continue ;;
    esac
    violations="${violations}${record}"$'\n'
done < <(grep -rnE "${pattern}" "${search_dirs[@]}" 2>/dev/null || true)

if [ -n "${violations}" ]; then
    echo "A Box3D header is included outside ${allowed}"
    echo
    printf '%s' "${violations}"
    echo
    echo "Move the code under ${allowed}, or express it with engine types."
    echo "See DESIGN.md section 5."
    exit 1
fi

echo "check-box3d-containment: pass"
exit 0

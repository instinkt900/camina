#!/usr/bin/env bash
#
# Enforces rule 4.1 in DESIGN.md.
#
# Only files under src/gfx/vulkan/ can include a Vulkan header. Every layer
# above talks in gfx:: types. This keeps the later plugin ABI extraction
# mechanical instead of a rewrite.

set -euo pipefail

root="${1:-.}"
allowed="src/gfx/vulkan/"

search_dirs=()
for dir in src apps tools tests sandbox; do
    if [ -d "${root}/${dir}" ]; then
        search_dirs+=("${root}/${dir}")
    fi
done

if [ ${#search_dirs[@]} -eq 0 ]; then
    echo "check-vulkan-containment: no source directories found under ${root}"
    exit 0
fi

pattern='#[[:space:]]*include[[:space:]]*[<"](vulkan/|volk\.h|vk_mem_alloc\.h|vulkan\.h)'

violations="$(grep -rnE "${pattern}" "${search_dirs[@]}" 2>/dev/null | grep -v "${allowed}" || true)"

if [ -n "${violations}" ]; then
    echo "Rule 4.1 violation. A Vulkan header is included outside ${allowed}"
    echo
    echo "${violations}"
    echo
    echo "Move the code under ${allowed}, or express it with gfx:: types."
    echo "See DESIGN.md section 4."
    exit 1
fi

echo "check-vulkan-containment: pass"
exit 0

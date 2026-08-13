#!/usr/bin/env bash
#
# The shared half of the containment checks.
#
# Two rules have this shape. Rule 4.1 in DESIGN.md keeps Vulkan inside
# src/gfx/vulkan/, and the Box3D rule in section 5 keeps Box3D inside
# src/physics/. Each one names a directory that may include a family of
# headers, and forbids the include everywhere else.
#
# This file holds the part that does not change between them. The two scripts
# beside it pass their own directory, pattern, and messages.
#
# It exists because the two used to be separate copies, and they drifted. The
# Box3D copy was fixed for two faults that the Vulkan copy kept, and nothing
# reported the difference. One implementation cannot drift from itself. See
# issue #231.

# Reports every include of a forbidden header outside the one allowed directory.
#
# $1 The check name, used in its own output.
# $2 The directory allowed to include, with a trailing slash.
# $3 An extended regular expression matching the include directive.
# $4 What the header family is called in the failure message.
# $5 What to express the code with instead, named for the rule.
# $6 Where to read about the rule.
# $7 The tree to search.
check_containment() {
    local name="$1"
    local allowed="$2"
    local pattern="$3"
    local what="$4"
    local instead="$5"
    local doc="$6"
    local root="$7"

    local search_dirs=()
    local dir
    for dir in src apps tools tests sandbox; do
        if [ -d "${root}/${dir}" ]; then
            search_dirs+=("${root}/${dir}")
        fi
    done

    if [ ${#search_dirs[@]} -eq 0 ]; then
        echo "${name}: no source directories found under ${root}"
        return 0
    fi

    # Decide by file name, not by the whole grep record. A record is
    # path:line:text, so filtering the record lets a violation outside the
    # allowed directory pass whenever the line itself mentions that directory.
    # That failure is silent, which is the worse direction for a check to fail
    # in.
    #
    # The prefix carries a trailing slash and the match is anchored to the
    # start, so a sibling directory whose name merely starts the same way, such
    # as src/gfx/vulkan_helpers/, is not the allowed one.
    local violations=""
    local record file relative
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
        echo "${what} is included outside ${allowed}"
        echo
        printf '%s' "${violations}"
        echo
        echo "Move the code under ${allowed}, or express it with ${instead}."
        echo "See ${doc}."
        return 1
    fi

    echo "${name}: pass"
    return 0
}

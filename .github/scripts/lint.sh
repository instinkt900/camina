#!/usr/bin/env bash
#
# Runs clang-tidy over the project, without compiling it.
#
# M13 era. clang-tidy used to run inside the compile, through
# CMAKE_CXX_CLANG_TIDY, which made a Linux build take twice as long as the same
# build on MSVC. It runs here instead, in a job of its own, so a build reports
# whether the code compiles and the tests pass without waiting for the lint.
#
# **A green build no longer means the lint gate passed.** They are two checks
# and both have to be green.
#
# ## Which files it lints
#
# The compilation database holds every file the build compiles, third-party
# sources included, and clang-tidy has plenty to say about those. In the build
# they are excluded by SKIP_LINTING, which is a CMake source file property that
# a compilation database knows nothing about.
#
# **Every file that carries SKIP_LINTING also carries `-w`**, because a file we
# do not lint is a file we do not warn on either. So the skip list is read back
# off the compile command rather than written out a second time here. Two lists
# would drift, and this one cannot.
#
# A file that gains SKIP_LINTING without `-w` is linted anyway, and reports.
# That is the loud failure rather than the quiet one.
#
# Usage: lint.sh <build directory>

set -euo pipefail

if [ "$#" -ne 1 ]; then
    echo "lint.sh needs the build directory that holds compile_commands.json."
    exit 2
fi

readonly build="$1"
readonly database="${build}/compile_commands.json"

if [ ! -f "${database}" ]; then
    echo "::error::${database} is not there. Configure the build first."
    exit 1
fi

readonly files="$(mktemp)"
trap 'rm -f "${files}"' EXIT

python3 - "${database}" > "${files}" <<'PYTHON'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    database = json.load(handle)

seen = set()
for entry in database:
    command = entry.get("command") or " ".join(entry.get("arguments", []))
    # The SKIP_LINTING marker, read back off the compile command.
    if " -w " in command or command.rstrip().endswith(" -w"):
        continue
    path = entry["file"]
    if path in seen:
        continue
    seen.add(path)
    print(path)
PYTHON

readonly count="$(wc -l < "${files}")"
if [ "${count}" -eq 0 ]; then
    echo "::error::the compilation database named no file to lint."
    exit 1
fi

echo "Linting ${count} file(s) with $(nproc) job(s)."
# shellcheck disable=SC2046
run-clang-tidy-19 -p "${build}" -quiet -j "$(nproc)" $(cat "${files}")

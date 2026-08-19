#!/usr/bin/env bash
#
# Installs apt packages, doing as little as it can get away with.
#
# ## Do no work when there is none to do
#
# The runner image already carries many of these packages. Asking dpkg first
# costs milliseconds, and when nothing is missing this step does no network work
# at all.
#
# ## An update only when the install needs one
#
# The runner image ships with populated package lists, so `apt-get install`
# usually succeeds without `apt-get update` in front of it. The update is the
# slow half: it hits every repository, and on a bad day it has taken 200 seconds
# on its own while the install took 20. So the update runs only after an install
# has actually failed.
#
# ## And it survives a runner that stalls
#
# GitHub's hosted runners hang inside apt rather than failing. That happened four
# times in one day in August 2026, on three different steps. The work is wrapped
# in run-until-stalled.sh, which judges a stall by silence rather than by how
# long the command has taken. A working install took 912 seconds on the same
# day, so no total timeout can tell the two apart.
#
# Usage: apt-install.sh <package> [package ...]

set -euo pipefail

readonly kAttempts=3
readonly kSilentSeconds=120
readonly here="$(dirname "$0")"

if [ "$#" -eq 0 ]; then
    echo "apt-install.sh needs at least one package."
    exit 2
fi

missing=()
for package in "$@"; do
    if ! dpkg -s "${package}" > /dev/null 2>&1; then
        missing+=("${package}")
    fi
done

if [ "${#missing[@]}" -eq 0 ]; then
    echo "Every package is already installed. Nothing to do."
    exit 0
fi

echo "Missing: ${missing[*]}"
readonly packages="${missing[*]}"

for attempt in $(seq 1 "${kAttempts}"); do
    # The install on its own first. The runner's package lists are usually
    # current enough, and skipping the update saves the slow half.
    if "${here}/run-until-stalled.sh" "${kSilentSeconds}" \
        "sudo apt-get install -y ${packages}"; then
        exit 0
    fi

    echo "::warning::the install needs current package lists. Updating."
    if "${here}/run-until-stalled.sh" "${kSilentSeconds}" "sudo apt-get update" &&
        "${here}/run-until-stalled.sh" "${kSilentSeconds}" \
            "sudo apt-get install -y ${packages}"; then
        exit 0
    fi

    echo "::warning::apt attempt ${attempt} of ${kAttempts} did not finish. Retrying."
    "${here}/apt-unlock.sh"
    sleep 10
done

echo "::error::apt did not finish after ${kAttempts} attempts."
exit 1

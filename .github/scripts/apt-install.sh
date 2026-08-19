#!/usr/bin/env bash
#
# Installs apt packages, and survives a runner that stalls part way through.
#
# GitHub's hosted runners hang inside apt rather than failing. The job then
# never finishes and reports nothing, which looks exactly like a slow build. It
# happened four times in one day in August 2026, on three different steps:
# Install system packages, Install clang-format 19, and Install Doxygen. Every
# re-run on a fresh runner was clean, so the branch was never the cause.
#
# **This waits on silence rather than on a total duration.** On the same day a
# working install took 912 seconds, so any total timeout short enough to catch
# a hang also kills an install that would have finished. See
# run-until-stalled.sh, which is where that reasoning lives.
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

readonly packages="$*"

for attempt in $(seq 1 "${kAttempts}"); do
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

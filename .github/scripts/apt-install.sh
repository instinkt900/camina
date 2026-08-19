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
# A timeout turns that hang into a failed attempt, and the retry usually
# succeeds. So a stall costs a minute rather than a cancel, a re-run, and
# whatever time passed before somebody noticed.
#
# Usage: apt-install.sh <package> [package ...]

set -euo pipefail

readonly kAttempts=3
readonly kUpdateSeconds=180
readonly kInstallSeconds=420

if [ "$#" -eq 0 ]; then
    echo "apt-install.sh needs at least one package."
    exit 2
fi

for attempt in $(seq 1 "${kAttempts}"); do
    if timeout "${kUpdateSeconds}" sudo apt-get update &&
        timeout "${kInstallSeconds}" sudo apt-get install -y "$@"; then
        exit 0
    fi

    echo "::warning::apt attempt ${attempt} of ${kAttempts} did not finish. Retrying."

    # A killed apt can leave a package half configured, and the next attempt
    # then refuses to start. This is a no-op when nothing was interrupted.
    sudo dpkg --configure -a || true
    sleep 10
done

echo "::error::apt did not finish after ${kAttempts} attempts."
exit 1

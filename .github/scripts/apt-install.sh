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
# **The worst case here has to fit inside the job's timeout-minutes.** Three
# attempts of an update and an install is 3 x (90 + 180) = 810 seconds. A job
# cut off part way through its retries pays the delay and gets no benefit,
# which is exactly what happened on the first run of this script.
#
# Usage: apt-install.sh <package> [package ...]

set -euo pipefail

readonly kAttempts=3
readonly kUpdateSeconds=90
readonly kInstallSeconds=180

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

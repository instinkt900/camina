#!/usr/bin/env bash
#
# Runs a command with a timeout, and tries again when it does not finish.
#
# GitHub's hosted runners stall inside network commands rather than failing, so
# a step can sit until the job timeout with nothing in the log. A timeout turns
# that into a failed attempt, and the retry usually succeeds.
#
# **Keep the worst case under the job's timeout-minutes.** Attempts times the
# per-attempt timeout is what a stalling runner costs, and a job cut off part
# way through its retries gets the delay without the benefit. See
# .github/workflows/ci.yml.
#
# Usage: retry.sh <seconds> <shell command>

set -euo pipefail

readonly kAttempts=2

if [ "$#" -lt 2 ]; then
    echo "retry.sh needs a timeout in seconds and a command."
    exit 2
fi

readonly seconds="$1"
shift
readonly command="$*"

for attempt in $(seq 1 "${kAttempts}"); do
    if timeout "${seconds}" bash -c "${command}"; then
        exit 0
    fi
    echo "::warning::attempt ${attempt} of ${kAttempts} did not finish within ${seconds}s: ${command}"
    sleep 5
done

echo "::error::did not finish after ${kAttempts} attempts: ${command}"
exit 1

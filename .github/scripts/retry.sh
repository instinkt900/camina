#!/usr/bin/env bash
#
# Runs a command, and tries again when it stalls.
#
# The stall is judged by silence rather than by a total duration, because a
# runner that is merely slow must not be killed. See run-until-stalled.sh.
#
# Usage: retry.sh <silent-seconds> <shell command>

set -uo pipefail

readonly kAttempts=2
readonly here="$(dirname "$0")"

if [ "$#" -lt 2 ]; then
    echo "retry.sh needs a silence timeout in seconds and a command."
    exit 2
fi

readonly silence="$1"
shift
readonly command="$*"

for attempt in $(seq 1 "${kAttempts}"); do
    if "${here}/run-until-stalled.sh" "${silence}" "${command}"; then
        exit 0
    fi
    echo "::warning::attempt ${attempt} of ${kAttempts} did not finish: ${command}"

    # A killed command can leave apt holding its lock. Every retry in this
    # project is around a package operation.
    "${here}/apt-unlock.sh"
    sleep 5
done

echo "::error::did not finish after ${kAttempts} attempts: ${command}"
exit 1

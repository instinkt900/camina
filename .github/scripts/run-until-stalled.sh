#!/usr/bin/env bash
#
# Runs a command, and kills it when it stops producing output.
#
# **A total timeout cannot tell a hung command from a slow one.** GitHub's
# runners have been both: a hung apt sat for 25 to 43 minutes and never
# finished, while a working one took 912 seconds on the same day. Any total
# timeout that catches the first also kills the second.
#
# So this watches for silence instead. A working apt prints as it goes, whatever
# the pace. A hung one prints nothing at all, and that is the difference the
# duration cannot see.
#
# The whole process group is killed, because `timeout` and a bare `kill` reach
# only the command itself. `add-apt-repository` starts an `apt-get` of its own,
# and leaving that alive holds the apt lock and defeats every later attempt.
#
# Usage: run-until-stalled.sh <silent-seconds> <shell command>

set -uo pipefail

readonly kPollSeconds=5

if [ "$#" -lt 2 ]; then
    echo "run-until-stalled.sh needs a silence timeout in seconds and a command."
    exit 2
fi

readonly silence="$1"
shift
readonly command="$*"

readonly log="$(mktemp)"
trap 'rm -f "${log}"' EXIT

# setsid gives the command a process group of its own, so everything it starts
# can be killed together.
setsid bash -c "${command}" > "${log}" 2>&1 &
readonly pid=$!

silent=0
size=0
while kill -0 "${pid}" 2>/dev/null; do
    sleep "${kPollSeconds}"
    now="$(stat -c %s "${log}" 2>/dev/null || echo 0)"
    if [ "${now}" -eq "${size}" ]; then
        silent=$((silent + kPollSeconds))
    else
        silent=0
        size="${now}"
    fi

    if [ "${silent}" -ge "${silence}" ]; then
        echo "::warning::no output for ${silence}s, so the command is stalled. Killing it."
        # The group, not the process. Negative pid means the group.
        sudo kill -9 -- "-${pid}" 2>/dev/null || kill -9 -- "-${pid}" 2>/dev/null || true
        wait "${pid}" 2>/dev/null || true
        cat "${log}"
        exit 124
    fi
done

wait "${pid}"
readonly status=$?
cat "${log}"
exit "${status}"

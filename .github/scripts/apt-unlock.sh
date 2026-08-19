#!/usr/bin/env bash
#
# Clears an apt lock left behind by a command that was killed.
#
# `timeout` kills the command it was given and not the children that command
# started. `add-apt-repository` runs its own `apt-get update`, so killing it left
# that update running and holding /var/lib/apt/lists/lock. Every later attempt
# then failed at once with "Could not get lock", and the retry could not help
# because the lock holder was still alive.
#
# So a failed attempt cleans up before the next one. Nothing here is needed when
# an attempt failed for an ordinary reason, and all of it is harmless then.

set -uo pipefail

sudo pkill -9 -x apt-get 2>/dev/null || true
sudo pkill -9 -x apt 2>/dev/null || true
sudo pkill -9 -x dpkg 2>/dev/null || true

# The lock files themselves, in case a killed process never released them.
sudo rm -f /var/lib/apt/lists/lock /var/cache/apt/archives/lock \
    /var/lib/dpkg/lock /var/lib/dpkg/lock-frontend 2>/dev/null || true

# A killed apt can leave a package half configured, and the next attempt then
# refuses to start.
sudo dpkg --configure -a 2>/dev/null || true

exit 0

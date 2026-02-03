#!/bin/sh

# Die on any failures.
set -e

# Regenerate the build system.
# You probably shouldn't need to modify this.

cd "$(dirname "$0")"
mkdir -p m4
aclocal --install
autoreconf --install

#!/bin/sh

# Die on failures.
set -e

# Regenerate the build system.
# You probably shouldn't need to modify this.

# Make sure we're in the project directory.
cd "$(dirname "$0")"

# Make sure we've got an m4 macro directory.
mkdir -p m4 build-aux

# If we use macros available on the system, copy them into our m4 directory if
# we don't have them, or if the system has a newer version.
aclocal --install

# Regenerate our build system.
autoreconf --install --force

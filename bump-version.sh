#!/bin/bash
set -euo pipefail

if [ $# -ne 1 ]; then
    echo "Usage: $0 <new-version>"
    echo "Example: $0 0.3.0"
    exit 1
fi

NEW="$1"

if ! [[ "$NEW" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "Error: version must be in X.Y.Z format"
    exit 1
fi

OLD=$(sed -n 's/^project(smx-sdk-mp VERSION \([0-9.]*\) .*/\1/p' CMakeLists.txt)

if [ -z "$OLD" ]; then
    echo "Error: could not read current version from CMakeLists.txt"
    exit 1
fi

if [ "$OLD" = "$NEW" ]; then
    echo "Already at version $NEW"
    exit 0
fi

# Use the `-i.bak` suffix form, which both GNU sed (Linux) and BSD sed (macOS)
# accept, then remove the backups. (Bare `-i` is GNU-only and errors on macOS.)
sed -i.bak "s/VERSION $OLD/VERSION $NEW/" CMakeLists.txt && rm -f CMakeLists.txt.bak
sed -i.bak "s/$OLD/$NEW/g" include/SMX.h && rm -f include/SMX.h.bak

echo "$OLD -> $NEW"

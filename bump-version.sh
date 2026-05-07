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

sed -i "s/VERSION $OLD/VERSION $NEW/" CMakeLists.txt
sed -i "s/$OLD/$NEW/g" README.md include/SMX.h src/SMX.cpp

echo "$OLD -> $NEW"

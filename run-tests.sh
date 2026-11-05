#!/bin/sh
# Build the Linux test image and run the official checker (make check) in it.
set -e
cd "$(dirname "$0")"
docker build -q -t mem-alloc-env . >/dev/null
docker run --rm -t -v "$PWD":/mem-alloc -w /mem-alloc/tests mem-alloc-env make -s check "$@"

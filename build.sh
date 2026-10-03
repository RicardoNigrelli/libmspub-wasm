#!/usr/bin/env bash
# Builds the libmspub WebAssembly module inside Docker and copies the
# artifacts to ./dist (dist/O3 and dist/Oz). Nothing is installed on the host.
set -euo pipefail
cd "$(dirname "$0")"
docker build --target export --output type=local,dest=dist .
ls -l dist/*/

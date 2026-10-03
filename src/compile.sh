#!/usr/bin/env bash
# Compiles the embind binding + JSON generator and links it against the
# static libmspub/librevenge built in the `deps` stage. Runs inside the
# emscripten/emsdk container (see ../Dockerfile).
#
#   compile.sh <outdir>
#
# Produces <outdir>/O3/libmspub.{js,wasm} and <outdir>/Oz/libmspub.{js,wasm}.
set -euo pipefail

OUT="${1:-/out}"
PREFIX="${PREFIX:-/opt/pub}"
EH_FLAGS="${EH_FLAGS:--fwasm-exceptions}"
PORTS="-sUSE_ZLIB=1 -sUSE_ICU=1 -sUSE_BOOST_HEADERS=1"
INCLUDES="-I${PREFIX}/include/librevenge-0.0 -I${PREFIX}/include/libmspub-0.1"
LIBS="-L${PREFIX}/lib -lmspub-0.1 -lrevenge-generators-0.0 -lrevenge-stream-0.0 -lrevenge-0.0"

COMMON_LINK=(
  ${EH_FLAGS} ${PORTS}
  -lembind
  -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createLibmspub
  -sENVIRONMENT=web,worker,node
  -sALLOW_MEMORY_GROWTH=1
  # libmspub recurses through shape groups; the 64 KiB default is too tight.
  -sSTACK_SIZE=1048576
  -sINITIAL_MEMORY=33554432
  -sFILESYSTEM=0
  -sINCOMING_MODULE_JS_API=print,printErr,locateFile,wasmBinary,instantiateWasm
)

for LEVEL in O3 Oz; do
  mkdir -p "${OUT}/${LEVEL}"
  em++ -std=c++17 -${LEVEL} ${EH_FLAGS} ${PORTS} ${INCLUDES} \
    -c JSONDrawingGenerator.cpp -o "/tmp/JSONDrawingGenerator-${LEVEL}.o"
  em++ -std=c++17 -${LEVEL} ${EH_FLAGS} ${PORTS} ${INCLUDES} \
    -c binding.cpp -o "/tmp/binding-${LEVEL}.o"
  emcc -${LEVEL} -c icudata_subset.c -o "/tmp/icudata-${LEVEL}.o"
  em++ -${LEVEL} \
    "/tmp/binding-${LEVEL}.o" "/tmp/JSONDrawingGenerator-${LEVEL}.o" "/tmp/icudata-${LEVEL}.o" \
    ${LIBS} "${COMMON_LINK[@]}" \
    -o "${OUT}/${LEVEL}/libmspub.js"
done

# Comparison build: same as Oz but WITHOUT the filtered ICU data (stubdata
# only), to measure what the codepage fix costs.
mkdir -p "${OUT}/Oz-stubdata"
em++ -std=c++17 -Oz -DOI_NO_ICUDATA ${EH_FLAGS} ${PORTS} ${INCLUDES}   -c binding.cpp -o /tmp/binding-nodata.o
em++ -Oz /tmp/binding-nodata.o /tmp/JSONDrawingGenerator-Oz.o   ${LIBS} "${COMMON_LINK[@]}" -o "${OUT}/Oz-stubdata/libmspub.js"

# Compressed sizes are measured with Node (test/measure.mjs), which has brotli.
ls -l "${OUT}"/*/

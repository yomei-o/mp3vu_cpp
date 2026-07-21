#!/bin/bash
set -e; cd "$(dirname "$0")"
EMSDK="${EMSDK:-/c/prog/emsdk/emsdk}"
export EM_CONFIG="$EMSDK/.emscripten"
export PATH="$EMSDK/upstream/emscripten:$EMSDK/upstream/bin:$EMSDK/node/22.16.0_64bit/bin:$EMSDK/python/3.13.3_64bit:$PATH"
OUT="${OUT:-wasmdist}"; mkdir -p "$OUT"
emcc -O3 -std=c++17 wasm_mp3.cpp \
  -sMODULARIZE=1 -sEXPORT_NAME=createMP3 -sENVIRONMENT=web -sALLOW_MEMORY_GROWTH=1 \
  -sEXPORTED_FUNCTIONS=_mp3_load,_pcm_ptr,_pcm_frames,_pcm_channels,_sample_rate,_meter_set_mode,_meter_mode,_meter_set_time,_meter_release,_meter_render,_meter_w,_meter_h,_malloc,_free \
  -sEXPORTED_RUNTIME_METHODS=cwrap,HEAPU8,HEAPF32 \
  -o "$OUT/mp3.js"
echo "built $OUT/mp3.js (+.wasm)"

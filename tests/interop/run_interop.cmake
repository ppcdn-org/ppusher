# Runs the Go interop check (mediacommon's mpegts.Reader = ppmmx's SRT ingest parser) over synthetic streams.
# -DGEN=<gen_ts> -DGO=<go> -DSRC=<tests/interop> -DOUT=<scratch dir>
file(MAKE_DIRECTORY "${OUT}")
execute_process(COMMAND "${GEN}" "${OUT}" RESULT_VARIABLE r)
if(NOT r EQUAL 0)
  message(FATAL_ERROR "gen_ts failed (${r})")
endif()

function(run_case codec file layers sei)
  set(flags -codec ${codec} -layers ${layers})
  if(sei)
    list(APPEND flags -sei)
  endif()
  execute_process(COMMAND "${CMAKE_COMMAND}" -E env GOFLAGS=-mod=mod "${GO}" run . ${flags} "${OUT}/${file}"
                  WORKING_DIRECTORY "${SRC}" RESULT_VARIABLE r OUTPUT_VARIABLE out ERROR_VARIABLE err)
  message("${out}${err}")
  if(NOT r EQUAL 0)
    message(FATAL_ERROR "interop check failed for ${file}")
  endif()
endfunction()

#        codec  file                  layers sei
run_case(h264   h264_opus.ts          1      OFF)
run_case(hevc   hevc_aac_sei.ts       1      ON)
run_case(h264   h264_x4_opus.ts       4      OFF)   # ppmmx maximum for H.264
run_case(hevc   hevc_x3_aac_sei.ts    3      ON)    # ppmmx maximum for HEVC

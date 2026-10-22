# Runs the Go interop check (mediacommon's mpegts.Reader = ppmmx's SRT ingest parser) over synthetic streams.
# -DGEN=<gen_ts> -DGO=<go> -DSRC=<tests/interop> -DOUT=<scratch dir>
file(MAKE_DIRECTORY "${OUT}")
execute_process(COMMAND "${GEN}" "${OUT}" RESULT_VARIABLE r)
if(NOT r EQUAL 0)
  message(FATAL_ERROR "gen_ts failed (${r})")
endif()
foreach(case "h264|h264_opus.ts|" "hevc|hevc_aac_sei.ts|-sei")
  string(REPLACE "|" ";" parts "${case}")
  list(GET parts 0 codec)
  list(GET parts 1 file)
  list(GET parts 2 extra)
  execute_process(COMMAND "${CMAKE_COMMAND}" -E env GOFLAGS=-mod=mod "${GO}" run . -codec ${codec} ${extra} "${OUT}/${file}"
                  WORKING_DIRECTORY "${SRC}" RESULT_VARIABLE r OUTPUT_VARIABLE out ERROR_VARIABLE err)
  message("${out}${err}")
  if(NOT r EQUAL 0)
    message(FATAL_ERROR "interop check failed for ${file}")
  endif()
endforeach()

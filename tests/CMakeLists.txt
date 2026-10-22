function(pp_add_test name)
  add_executable(${name} ${name}.c)
  target_link_libraries(${name} PRIVATE ppusher_core)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME ${name} COMMAND ${name})
endfunction()

pp_add_test(test_streamid)
pp_add_test(test_degrade)
pp_add_test(test_loss_sm)
pp_add_test(test_ts)

# network layer against in-process loopback mock servers
add_executable(test_net test_net.c)
target_link_libraries(test_net PRIVATE ppusher_net Threads::Threads)
target_include_directories(test_net PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME test_net COMMAND test_net)

# session layer: real libsrt listener + mock ppcenter + mock degrade WebSocket
if(PPUSHER_WITH_SRT)
  add_executable(test_session test_session.c)
  target_link_libraries(test_session PRIVATE ppusher)
  target_include_directories(test_session PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src)
  add_test(NAME test_session COMMAND test_session)
  set_tests_properties(test_session PROPERTIES TIMEOUT 120)
endif()

# parse our muxer's output with the library ppmmx's SRT ingest uses (needs Go + the module cache / network)
find_program(GO_EXECUTABLE go)
if(GO_EXECUTABLE)
  add_executable(gen_ts gen_ts.c)
  target_link_libraries(gen_ts PRIVATE ppusher_core)
  add_test(NAME interop_mediacommon
    COMMAND ${CMAKE_COMMAND} -DGEN=$<TARGET_FILE:gen_ts> -DGO=${GO_EXECUTABLE}
            -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/interop -DOUT=${CMAKE_CURRENT_BINARY_DIR}/interop-out
            -P ${CMAKE_CURRENT_SOURCE_DIR}/interop/run_interop.cmake)
  set_tests_properties(interop_mediacommon PROPERTIES TIMEOUT 300)
else()
  message(STATUS "Go not found: skipping the mediacommon interop test")
endif()

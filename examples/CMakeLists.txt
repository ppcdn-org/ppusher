add_executable(ppusher-cli ppusher-cli.c ts_demux.c)
target_link_libraries(ppusher-cli PRIVATE ppusher)
# the CLI also pokes the muxer directly for --dry-run
target_include_directories(ppusher-cli PRIVATE ${PROJECT_SOURCE_DIR}/src)
install(TARGETS ppusher-cli RUNTIME DESTINATION bin)

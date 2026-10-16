# ares-embed (SPIKE): the N64 core headless behind the Odin64 editor's viewport. Shares ares-test's EmulatorRunner.
add_executable(ares-embed main.cpp ../test-runner/runner.cpp ../test-runner/runner.hpp)
target_include_directories(ares-embed PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(ares-embed PRIVATE ares::ares ares::mia ares::nall libco)
if(ARES_ENABLE_CHD)
  target_link_libraries(ares-embed PRIVATE chdr-static)
endif()
add_custom_command(TARGET ares-embed POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:ares-embed>/Firmware/Nintendo 64"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/mia/Firmware/Nintendo 64/pif.ntsc.rom"
    "${CMAKE_SOURCE_DIR}/mia/Firmware/Nintendo 64/pif.pal.rom"
    "${CMAKE_SOURCE_DIR}/mia/Firmware/Nintendo 64/pif.sm5.rom"
    "$<TARGET_FILE_DIR:ares-embed>/Firmware/Nintendo 64"
)

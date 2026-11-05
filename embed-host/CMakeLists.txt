# ares-embed: the N64 core headless behind the Odin64 editor's N64 tab. Shares ares-test's EmulatorRunner.
add_executable(ares-embed main.cpp ../test-runner/runner.cpp ../test-runner/runner.hpp)
target_include_directories(ares-embed PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(ares-embed PRIVATE ares::ares ares::mia ares::nall libco)
if(ARES_ENABLE_CHD)
  target_link_libraries(ares-embed PRIVATE chdr-static)
endif()

# ares-test: headless N64 test runner (no SDL/ImGui/ruby, no window, no GPU).
# Built when ARES_BUILD_HEADLESS is ON, on Linux, BSD, macOS and Windows. Pairs
# naturally with -DARES_ENABLE_VULKAN=OFF (the linux-headless and macos-headless
# presets) so parallel-rdp is not compiled at all, but also builds in a normal
# tree for local use.

add_executable(ares-test main.cpp runner.cpp js-host.cpp runner.hpp js-host.hpp)

target_include_directories(ares-test PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(ares-test PRIVATE ares::ares ares::mia ares::nall libco qjs)

if(ARES_ENABLE_CHD)
  target_link_libraries(ares-test PRIVATE chdr-static)
endif()

# rspq-libdragon.json next to the binary: RSPCapture::autoDetect() checks
# Path::program() when the JSON isn't found next to the ROM's ELF (used by the
# ares.rspProfile* / waitRspCommand scripting APIs).
add_custom_command(TARGET ares-test POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/desktop-ui/ui/rsp-overlays/rspq-libdragon.json"
    "$<TARGET_FILE_DIR:ares-test>"
)

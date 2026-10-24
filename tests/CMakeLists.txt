# Unit tests: pure logic and state machines (fake transport, virtual time). No OBS runtime needed.
add_executable(mf-unit-tests)
target_sources(mf-unit-tests PRIVATE unit/main.cpp unit/test-core.cpp unit/test-sessions.cpp)
if(OS_WINDOWS)
  # Real Windows Credential Manager round trips (runs on the Windows CI runner).
  target_sources(mf-unit-tests PRIVATE unit/test-wincred.cpp)
elseif(OS_MACOS)
  # Real macOS Keychain round trips (runs on the macOS CI runner).
  target_sources(mf-unit-tests PRIVATE unit/test-keychain.cpp)
endif()
target_include_directories(mf-unit-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(mf-unit-tests PRIVATE matchframe-core)
add_test(NAME mf-unit-tests COMMAND mf-unit-tests)

# Integration tests: real libobs 32.2.2 modules (obs-outputs, obs-x264, obs-ffmpeg, rtmp-services)
# streaming to local RTMP receivers. Linux only: needs an X display (Xvfb) for libobs-opengl.
if(OS_LINUX)
  add_executable(mf-integration-tests)
  target_sources(mf-integration-tests PRIVATE integration/main.cpp integration/obs-harness.cpp integration/scenarios.cpp)
  target_include_directories(mf-integration-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(mf-integration-tests PRIVATE matchframe-core)
  find_package(X11 REQUIRED)
  target_link_libraries(mf-integration-tests PRIVATE X11::X11)
endif()

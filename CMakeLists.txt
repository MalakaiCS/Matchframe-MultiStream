cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)
option(MF_BUILD_TESTS "Build unit and libobs integration tests" OFF)
option(MF_BUILD_UI_TEST_DRIVER "Build the test-only OBS UI driver plugin (never ship it)" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

find_package(libobs REQUIRED)

# Qt-free core: configuration, credentials, encoder group, sessions, output manager.
add_library(matchframe-core STATIC)
target_sources(
  matchframe-core
  PRIVATE
    src/core/config-repository.cpp
    src/core/credential-store.cpp
    src/core/destination-config.cpp
    src/core/destination-session.cpp
    src/core/encoder-catalog.cpp
    src/core/encoder-profile.cpp
    src/core/error-classifier.cpp
    src/core/lifecycle-worker.cpp
    src/core/media-info.cpp
    src/core/obs-rtmp-transport.cpp
    src/core/output-manager.cpp
    src/core/platform.cpp
    src/core/profile-config.cpp
    src/core/redaction.cpp
    src/core/rtmp-url.cpp
    src/core/service-constraints.cpp
    src/core/shared-encoder-group.cpp
)
if(OS_WINDOWS)
  target_sources(matchframe-core PRIVATE src/core/credential-store-win.cpp)
  target_link_libraries(matchframe-core PRIVATE Advapi32)
elseif(OS_MACOS)
  target_sources(matchframe-core PRIVATE src/core/credential-store-mac.cpp)
  target_link_libraries(
    matchframe-core
    PRIVATE "$<LINK_LIBRARY:FRAMEWORK,Security.framework>" "$<LINK_LIBRARY:FRAMEWORK,CoreFoundation.framework>"
  )
endif()
target_include_directories(matchframe-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(matchframe-core PUBLIC OBS::libobs)
set_target_properties(matchframe-core PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(${CMAKE_PROJECT_NAME} MODULE)

target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE matchframe-core OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 REQUIRED COMPONENTS Widgets Core Gui)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets Qt6::Gui)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin/plugin-main.cpp
    src/plugin/plugin-context.cpp
    src/plugin/plugin-context.hpp
    src/ui/destination-card.cpp
    src/ui/destination-card.hpp
    src/ui/destination-editor.cpp
    src/ui/destination-editor.hpp
    src/ui/encoder-settings-dialog.cpp
    src/ui/encoder-settings-dialog.hpp
    src/ui/multistream-dock.cpp
    src/ui/multistream-dock.hpp
    src/ui/qt-dispatcher.cpp
    src/ui/qt-dispatcher.hpp
    src/ui/ui-helpers.cpp
    src/ui/ui-helpers.hpp
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

if(OS_WINDOWS)
  # Release zip root: install/uninstall/key-removal scripts and the operator documentation.
  install(
    FILES
      packaging/windows/Install-MatchFrameMultistream.ps1
      packaging/windows/Uninstall-MatchFrameMultistream.ps1
      packaging/windows/Remove-MatchFrameStoredKeys.ps1
      docs/INSTALL.md
      docs/OPERATOR_GUIDE.md
      LICENSE
    DESTINATION .
  )
endif()

if(MF_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(MF_BUILD_UI_TEST_DRIVER)
  add_subdirectory(tests/ui-driver)
endif()

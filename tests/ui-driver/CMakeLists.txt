# Test-only OBS module that drives the MatchFrame Multistream dock through its widgets.
# Built only with -DMF_BUILD_UI_TEST_DRIVER=ON; never installed by the release packaging.
add_library(mf-ui-driver MODULE)
target_sources(mf-ui-driver PRIVATE ui-driver.cpp)
target_link_libraries(mf-ui-driver PRIVATE OBS::libobs OBS::obs-frontend-api Qt6::Core Qt6::Widgets)
set_target_properties(mf-ui-driver PROPERTIES PREFIX "" AUTOMOC ON)
target_compile_options(
  mf-ui-driver
  PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
)
if(OS_MACOS)
  # OBS on macOS loads modules as <name>.plugin bundles from ~/Library/Application Support/obs-studio/plugins.
  set_target_properties(
    mf-ui-driver
    PROPERTIES
      AUTOMOC_COMPILER_PREDEFINES OFF # see the plugin target in the top-level CMakeLists.txt
      BUNDLE TRUE
      BUNDLE_EXTENSION plugin
      XCODE_ATTRIBUTE_PRODUCT_NAME mf-ui-driver
      XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER com.matchframe.mf-ui-driver
      XCODE_ATTRIBUTE_GENERATE_INFOPLIST_FILE YES
      XCODE_ATTRIBUTE_INFOPLIST_FILE ""
  )
endif()

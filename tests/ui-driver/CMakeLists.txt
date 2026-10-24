# Test-only OBS module that drives the MatchFrame Multistream dock through its widgets.
# Built only with -DMF_BUILD_UI_TEST_DRIVER=ON; never installed by the release packaging.
add_library(mf-ui-driver MODULE)
target_sources(mf-ui-driver PRIVATE ui-driver.cpp)
target_link_libraries(mf-ui-driver PRIVATE OBS::libobs OBS::obs-frontend-api Qt6::Core Qt6::Widgets)
set_target_properties(mf-ui-driver PROPERTIES PREFIX "" AUTOMOC ON)

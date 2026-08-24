# Unit tests for the optics core. No external test framework: TestMain.cpp
# carries a small assertion harness, so the tests build wherever the app does.
add_executable(optics_tests TestMain.cpp NewTests.inc)

target_link_libraries(optics_tests PRIVATE optics_core)

# Put the test binary beside the app, where the deployed OCCT and Qt DLLs live,
# so `ctest` works without a hand-built PATH.
set_target_properties(optics_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

if(MSVC)
    target_compile_options(optics_tests PRIVATE /W3 /MP)
endif()

# One CTest entry per suite, so a failure names the area that broke.
foreach(suite intersect bvh trace edge tir sampling detector worker patterns fresnel absorption scatter roughness spectral intensity metrics focus convergence source rays params config media material spectrum units detectors photometry validation estimator instances cad sweep optimise derived report overrides bsdf coating polarised imagequality tolerancing geometry)
    add_test(NAME ${suite} COMMAND optics_tests ${suite})
    set_tests_properties(${suite} PROPERTIES TIMEOUT 300)
endforeach()

# Qt needs its platform plugin resolved even for a console binary on Windows.
set_tests_properties(intersect bvh trace edge tir sampling detector worker patterns fresnel absorption scatter roughness spectral intensity metrics focus convergence source rays params config media material spectrum units detectors photometry validation estimator instances cad sweep optimise derived report overrides bsdf coating polarised imagequality tolerancing geometry
                     PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

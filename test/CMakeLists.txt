# Unit tests for the optics core. No external test framework: TestMain.cpp
# carries a small assertion harness, so the tests build wherever the app does.
add_executable(optics_tests TestMain.cpp NewTests.inc AuditTests.inc SceneDocTests.inc)

# The reference corpus lives beside the tests, and the test binary runs from the
# build tree, so the path has to be baked in rather than guessed at run time.
target_compile_definitions(optics_tests PRIVATE
    LUXTRACE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(optics_tests PRIVATE optics_core)

# Put the test binary beside the app, where the deployed OCCT and Qt DLLs live,
# so `ctest` works without a hand-built PATH.
set_target_properties(optics_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

if(MSVC)
    target_compile_options(optics_tests PRIVATE /W3 /MP)
endif()

# One CTest entry per suite, so a failure names the area that broke. Named once
# and reused: the list was duplicated for the properties below, which is one
# list to forget when a suite is added.
set(LUXTRACE_TEST_SUITES
    intersect bvh trace edge tir sampling detector worker patterns fresnel
    absorption scatter roughness spectral intensity metrics focus convergence
    source rays params config media material spectrum units detectors photometry
    validation estimator instances cad sweep optimise derived report overrides
    bsdf coating polarised imagequality tolerancing geometry channels diagnostics
    overridekeys polarisedmetal catalogue rayfile multisource regression
    configkeys scattermodel importaudit simd preview rayinspector pythonenv
    fidelity sceneparams thetabin meshadaptive scenedoc)

foreach(suite IN LISTS LUXTRACE_TEST_SUITES)
    add_test(NAME ${suite} COMMAND optics_tests ${suite})
    set_tests_properties(${suite} PROPERTIES TIMEOUT 300)
endforeach()

# Qt needs its platform plugin resolved even for a console binary on Windows,
# and the Qt DLLs have to be findable before that can happen. Prepending the
# kit's own bin directory to PATH does both: Qt6Core carries the prefix it was
# built with, so the offscreen plugin resolves from there without any of it
# being copied into the build tree. OCCT is already beside the binary -- the
# vcpkg toolchain copies it there on every build (VCPKG_APPLOCAL_DEPS) -- so a
# fresh clone runs ctest with nothing deployed by hand.
set_tests_properties(${LUXTRACE_TEST_SUITES} PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

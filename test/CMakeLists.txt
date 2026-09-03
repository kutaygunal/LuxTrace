# Unit tests for the optics core. No external test framework: TestMain.cpp
# carries a small assertion harness, so the tests build wherever the app does.
add_executable(optics_tests TestMain.cpp NewTests.inc AuditTests.inc SceneDocTests.inc
               AppearanceTests.inc ThetaBinTests.inc MeasuredDataTests.inc
               SceneParamsTests.inc AtomicGridTests.inc AdaptiveMeshTests.inc
               MultiEditTests.inc BackendCheckTests.inc GpuBackendTests.inc
               BackwardTests.inc EnvironmentTests.inc MetasurfaceTests.inc)

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

# ---------------------------------------------------------------------------
# One CTest entry per suite, so a failure names the area that broke -- and the
# list of suites comes from the binary rather than from this file.
#
# It used to be typed here. That list fell ten suites behind what the binary
# carried (backendcheck, camera, determinism, environment, gpu, immersed,
# metafile, metasurface, straypaths, tircollimator all ran under `optics_tests`
# and none of them ran under `ctest`), because adding a suite and remembering to
# name it here are two separate acts and only the first one is enforced by the
# compiler. Asking the binary makes them one act: a suite is registered with
# CTest by existing.
#
# The mechanics are the ones GoogleTest's discovery uses, for the same reason:
# CTest needs the list at configure time and the binary only knows it once it is
# linked, so a POST_BUILD step writes an include file that CTest reads. The
# include file is generated at configure time as well, so a configured-but-not-
# yet-built tree registers a failing placeholder rather than silently no tests.
# ---------------------------------------------------------------------------
set(suite_timeout 300)
set(discovery_base "${CMAKE_CURRENT_BINARY_DIR}/optics_tests_suites")

# The generated list names the binary by its full path, so a multi-config tree
# keeps one list per configuration rather than letting the last build win and
# sending `ctest -C Release` at a Debug binary.
set(discovery_tests "${discovery_base}_tests-$<CONFIG>.cmake")

add_custom_command(TARGET optics_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND}
            -D "TEST_EXECUTABLE=$<TARGET_FILE:optics_tests>"
            -D "TESTS_FILE=${discovery_tests}"
            -D "QT_BIN_DIR=$<TARGET_FILE_DIR:Qt6::Core>"
            -D "TEST_TIMEOUT=${suite_timeout}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/DiscoverSuites.cmake"
    BYPRODUCTS "${discovery_tests}"
    VERBATIM
    COMMENT "Enumerating optics_tests suites for CTest")

# A tree that was configured but never built has no suite list. Registering a
# test that cannot run says so; registering nothing would let `ctest` come back
# green having run no tests at all.
file(GENERATE OUTPUT "${discovery_base}_include-$<CONFIG>.cmake" CONTENT
"if(EXISTS \"${discovery_tests}\")
    include(\"${discovery_tests}\")
else()
    add_test(optics_tests_NOT_BUILT optics_tests_NOT_BUILT)
endif()
")

# CTest reads the file below with generator expressions already spent, so the
# configuration has to be chosen with a variable it sets rather than with
# $<CONFIG>. One indirection buys that.
if(CMAKE_CONFIGURATION_TYPES)
    file(WRITE "${discovery_base}_include.cmake"
         "include(\"${discovery_base}_include-\${CTEST_CONFIGURATION_TYPE}.cmake\")
")
else()
    file(GENERATE OUTPUT "${discovery_base}_include.cmake" CONTENT
         "include(\"${discovery_base}_include-$<CONFIG>.cmake\")
")
endif()

set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
             "${discovery_base}_include.cmake")

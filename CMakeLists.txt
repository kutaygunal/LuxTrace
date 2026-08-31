cmake_minimum_required(VERSION 3.22)

# ---------------------------------------------------------------------------
# Where the two prerequisites live. Read from the environment rather than
# written into this file, so nobody has to edit the build to build: VCPKG_ROOT
# is the vcpkg checkout, QT_ROOT_DIR the Qt MSVC kit. Both run before project()
# because a toolchain file is only consulted there.
#
# A preset, a -D on the command line, or an already-configured cache all still
# win: each is set only when nothing has set it already.
# ---------------------------------------------------------------------------
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE FILEPATH "vcpkg toolchain, taken from VCPKG_ROOT")
endif()

if(NOT CMAKE_PREFIX_PATH AND DEFINED ENV{QT_ROOT_DIR})
    set(CMAKE_PREFIX_PATH "$ENV{QT_ROOT_DIR}"
        CACHE PATH "Qt kit, taken from QT_ROOT_DIR")
endif()

project(LuxTrace VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# OpenCASCADE Technology and Qt 6. Both are searched quietly so a missing one
# reports what to do about it, rather than the wall of text a REQUIRED failure
# produces -- "a stranger cannot build it" starts with an error nobody can act
# on.
find_package(OpenCASCADE CONFIG QUIET)
if(NOT OpenCASCADE_FOUND)
    message(FATAL_ERROR
        "OpenCASCADE was not found.\n"
        "  Install it:   vcpkg install opencascade --triplet x64-windows\n"
        "  Point at it:  set VCPKG_ROOT to your vcpkg checkout, or configure with\n"
        "                -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake")
endif()

find_package(Qt6 QUIET COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets)
if(NOT Qt6_FOUND)
    message(FATAL_ERROR
        "Qt 6 was not found.\n"
        "  Point at it:  set QT_ROOT_DIR to your Qt MSVC kit "
        "(for example C:/Qt/6.8.2/msvc2022_64),\n"
        "                or configure with -DCMAKE_PREFIX_PATH=<kit>")
endif()

find_package(Threads REQUIRED)

enable_testing()

# ---------------------------------------------------------------------------
# Core: geometry, meshing, BVH and the ray tracer. Kept as its own library so
# the tests link the same objects the app runs, with no Qt Widgets dependency.
# ---------------------------------------------------------------------------
add_library(optics_core STATIC
    src/core/Vec3.h
    src/core/Optics.h
    src/core/SurfaceOptics.h
    src/core/Sampling.h src/core/Sampling.cpp
    src/core/ThreadPool.h src/core/ThreadPool.cpp
    src/core/Material.h src/core/Material.cpp
    src/core/RayFile.h src/core/RayFile.cpp
    src/core/MaterialFile.h src/core/MaterialFile.cpp
    src/core/PythonEnv.h src/core/PythonEnv.cpp
    src/core/Bsdf.h src/core/Bsdf.cpp
    src/core/Coating.h src/core/Coating.cpp
    src/core/Polarisation.h src/core/Polarisation.cpp
    src/core/Spectrum.h src/core/Spectrum.cpp
    src/core/Mesh.h src/core/Mesh.cpp
    src/core/GeometryProvider.h src/core/GeometryProvider.cpp
    src/core/SceneDocument.h src/core/SceneDocument.cpp
    # The field-flag copies behind multi-object editing. Core rather than UI:
    # nothing here needs Qt Widgets, and "an edit to one number stays an edit
    # to one number" is a correctness property a headless test can assert.
    src/core/MultiEditFields.h src/core/MultiEditFields.cpp
    src/core/CadImport.h src/core/CadImport.cpp
    src/core/MeshBuilder.h src/core/MeshBuilder.cpp
    src/core/TraceScene.h src/core/TraceScene.cpp
    src/core/TraceSceneSimd.h src/core/TraceSceneSimd.cpp
    src/core/RayTracer.h src/core/RayTracer.cpp
    src/core/SimulationResult.h src/core/SimulationResult.cpp
    src/core/Simulation.h src/core/Simulation.cpp
    src/core/SimulationWorker.h src/core/SimulationWorker.cpp
    src/core/GeometryWorker.h src/core/GeometryWorker.cpp
    src/core/Analysis.h src/core/Analysis.cpp
    src/core/ConfigIO.h src/core/ConfigIO.cpp
    src/core/JobRunner.h src/core/JobRunner.cpp
    src/core/Studies.h src/core/Studies.cpp
    src/core/Report.h src/core/Report.cpp
    src/core/StudyWorker.h src/core/StudyWorker.cpp
    # The appearance preview's scene side: SurfaceOptics -> Graphic3d_BSDF, and
    # the presentations built from it. Here rather than in the application
    # because neither file needs Qt Widgets or a GL context, so the whole
    # material mapping is assertable in a headless test -- the view that
    # displays the result is the only part that needs a window.
    src/render/AppearanceMaterials.h src/render/AppearanceMaterials.cpp
    src/render/AppearanceScene.h src/render/AppearanceScene.cpp
    src/render/AppearanceEmitters.h src/render/AppearanceEmitters.cpp
    src/render/RadianceMap.h src/render/RadianceMap.cpp
    src/render/AppearanceExport.h src/render/AppearanceExport.cpp
)

target_include_directories(optics_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(optics_core PUBLIC
    Qt6::Core
    # QImage, for the figures a report embeds. Gui only -- no widgets, so the
    # tests still link the core without a window system.
    Qt6::Gui
    Threads::Threads
    # OpenCASCADE toolkits
    TKernel TKMath TKService
    # Visualization: the embedded 3D viewport (V3d_View + AIS + OpenGl driver)
    TKV3d TKOpenGl
    TKG2d TKG3d TKBRep TKGeomBase TKGeomAlgo
    TKPrim TKBO TKBool TKMesh TKTopAlgo
    TKFillet TKHLR TKShHealing
    # Data exchange: reading a customer's CAD. These ship with OCCT and were
    # simply never linked, which is the whole reason the app could only open its
    # own twenty-five demonstration scenes.
    TKDESTEP TKDEIGES TKDESTL TKXSBase
)

# ---------------------------------------------------------------------------
# Application
#
# WIN32_EXECUTABLE is deliberately NOT set: the diagnostics subcommands
# (--smoke, --study, --bench, --meshcheck) write to stdout, and a GUI-subsystem
# binary on Windows has no console to write to.
# ---------------------------------------------------------------------------
qt_add_executable(LuxTrace
    src/main.cpp
    resources/luxtrace.qrc
    src/ui/Palette.h src/ui/Palette.cpp
    src/ui/PlotWidget.h src/ui/PlotWidget.cpp
    src/ui/PolarPlotWidget.h src/ui/PolarPlotWidget.cpp
    src/ui/HeatmapWidget.h src/ui/HeatmapWidget.cpp
    src/ui/RayDiagramWidget.h src/ui/RayDiagramWidget.cpp
    src/ui/ControlsPanel.h src/ui/ControlsPanel.cpp
    src/ui/ImportDialog.h src/ui/ImportDialog.cpp
    src/ui/MultiEdit.h src/ui/MultiEdit.cpp
    src/ui/SurfaceInspector.h src/ui/SurfaceInspector.cpp
    src/ui/ObjectLibraryPanel.h src/ui/ObjectLibraryPanel.cpp
    src/ui/SceneTreePanel.h src/ui/SceneTreePanel.cpp
    src/ui/ObjectInspector.h src/ui/ObjectInspector.cpp
    src/ui/PythonPanel.h src/ui/PythonPanel.cpp
    src/ui/EnergyBarWidget.h src/ui/EnergyBarWidget.cpp
    src/ui/SourceDialog.h src/ui/SourceDialog.cpp
    src/ui/OcctViewWidget.h src/ui/OcctViewWidget.cpp
    src/render/AppearanceView.h src/render/AppearanceView.cpp
    # MainWindow is one class across several translation units. The header and
    # every call site are unchanged; the split is a file boundary, drawn where
    # the window already has seams -- studies, appearance, the scene document,
    # and file I/O are four areas that do not call into each other.
    src/ui/MainWindow.h src/ui/MainWindowInternal.h src/ui/MainWindow.cpp
    src/ui/MainWindowStudies.cpp
    src/ui/MainWindowAppearance.cpp
    src/ui/MainWindowScene.cpp
    src/ui/MainWindowIO.cpp
)

# The Windows resource script carries the executable's own icon and its version
# block. Qt's resource covers the running window; this one covers the file as the
# shell sees it, before the process starts.
if(WIN32)
    target_sources(LuxTrace PRIVATE resources/LuxTrace.rc)
endif()

target_link_libraries(LuxTrace PRIVATE
    optics_core
    Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets
)

if(WIN32)
    target_link_libraries(LuxTrace PRIVATE opengl32 user32 gdi32 advapi32 ws2_32)
endif()

# The ray tracer is only meaningful in an optimised build. /fp:fast is
# deliberately NOT used: the slab test tolerates the NaN that an axis-aligned
# ray produces, and fast math is allowed to assume NaNs never occur.
# The four-wide leaf intersection is the one translation unit compiled for
# AVX2, and it is guarded at run time by a CPUID probe that lives elsewhere.
# /fp:precise is stated rather than assumed: it is what stops the compiler
# fusing a multiply and an add into an FMA, which would re-round an
# intermediate and make the wide path disagree with the scalar one in the last
# bits -- and being bit-identical is the whole claim.
if(MSVC)
    set_source_files_properties(src/core/TraceSceneSimd.cpp PROPERTIES
        COMPILE_OPTIONS "/arch:AVX2;/fp:precise")
else()
    set_source_files_properties(src/core/TraceSceneSimd.cpp PROPERTIES
        COMPILE_OPTIONS "-mavx2;-ffp-contract=off")
endif()

# ---------------------------------------------------------------------------
# Sanitizers. Off by default; -DLUXTRACE_SANITIZE=thread is what CI's TSan job
# turns on.
#
# The tracer's claim is about threads: a shared atomic receiver grid written by
# a relaxed compare-exchange loop, an acquire/release publication counter, and a
# condition-variable pool. None of that is checked by comparing two runs'
# floats -- a race can produce the same number a thousand times and a different
# one on the machine that matters. ThreadSanitizer is the check that actually
# looks at the memory model, so it gets a build option rather than a hand-rolled
# set of flags in a workflow file.
#
# MSVC has no TSan, so this is a Clang/GCC-only build and the CI job that uses
# it runs on Linux. Everything is compiled with it, not just the core: a race
# between a worker and the thread that reads its result is exactly the kind
# these three suites are pointed at.
set(LUXTRACE_SANITIZE "" CACHE STRING
    "Sanitizer to build with: thread, address, undefined, or empty for none")
set_property(CACHE LUXTRACE_SANITIZE PROPERTY STRINGS "" thread address undefined)

if(LUXTRACE_SANITIZE)
    if(MSVC)
        message(FATAL_ERROR
            "LUXTRACE_SANITIZE=${LUXTRACE_SANITIZE} needs Clang or GCC; MSVC ships "
            "no ThreadSanitizer. Configure with clang++ instead.")
    endif()
    # -O1 and frame pointers: a sanitizer report is only useful with a readable
    # stack, and -O0 makes the trace suites too slow to finish a CI job.
    add_compile_options(-fsanitize=${LUXTRACE_SANITIZE}
                        -fno-omit-frame-pointer -g -O1)
    add_link_options(-fsanitize=${LUXTRACE_SANITIZE})
    message(STATUS "Building with -fsanitize=${LUXTRACE_SANITIZE}")
endif()

if(MSVC)
    target_compile_options(optics_core PRIVATE /W3 /MP)
    target_compile_options(LuxTrace PRIVATE /W3 /MP)
    target_compile_options(optics_core PRIVATE $<$<CONFIG:Release,RelWithDebInfo>:/O2>)
else()
    target_compile_options(optics_core PRIVATE -Wall -Wextra)
endif()

add_subdirectory(test)

# ---------------------------------------------------------------------------
# Install and package
#
# `cmake --install build --config Release --prefix <dir>` assembles a tree that
# runs on a machine with neither Qt nor OCCT on it. Two owners, split so they
# cannot fight over the same files:
#
#   The runtime dependency set walks the imports of the built binary and brings
#   OCCT along with the freetype / libpng / brotli / zlib chain underneath it.
#   Walking the binary rather than naming toolkits means it cannot drift from
#   the link line above.
#
#   Qt is excluded from that set and deployed by windeployqt instead, because
#   the DLLs are the smaller half of what Qt needs: the platform, imageformats,
#   iconengines, styles, networkinformation and tls plugins do not appear in any
#   import table, and only windeployqt knows to go and get them.
# ---------------------------------------------------------------------------
set(LUXTRACE_RUNTIME_SEARCH_DIRS "")
if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    # Debug and release DLLs live in separate trees under a vcpkg triplet.
    list(APPEND LUXTRACE_RUNTIME_SEARCH_DIRS
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/$<$<CONFIG:Debug>:debug/>bin")
endif()

# Everything lands flat, next to the executable. The app is a console binary as
# much as a window -- the diagnostics subcommands print to stdout -- so
# `LuxTrace.exe --validate` has to work in the directory the archive unpacks
# into rather than one level below it.
#
# RUNTIME_DEPENDENCY_SET precedes the per-kind blocks in both signatures below,
# and the dependency set needs a RUNTIME block of its own: on Windows a DLL is a
# runtime artifact, and without the keyword the destination is not applied.
install(TARGETS LuxTrace
        RUNTIME_DEPENDENCY_SET luxtrace_runtime
        RUNTIME DESTINATION .)

install(RUNTIME_DEPENDENCY_SET luxtrace_runtime
        PRE_EXCLUDE_REGEXES
            # The Windows API sets and the DLLs that resolve only inside a
            # running process. Named before resolution is attempted, because
            # these have no file on disk to find and a failure to find one is
            # a hard error rather than a skip.
            "api-ms-win-.*" "ext-ms-.*" "^hvsifiletrust\\.dll$" "^pdmutilities\\.dll$"
            # Qt is deployed by its own tool below. It has to be dropped here
            # rather than after resolution: the Qt kit is not on the search
            # path, so leaving it in means failing to find a file that was
            # never wanted, and an unresolved dependency is a hard error.
            "[Qq]t6[A-Za-z0-9]*\\.dll"
        POST_EXCLUDE_REGEXES
            # Whatever resolved into the system directory belongs to Windows.
            ".*[/\\\\][Ss]ystem32[/\\\\].*\\.dll"
        DIRECTORIES ${LUXTRACE_RUNTIME_SEARCH_DIRS}
        RUNTIME DESTINATION .)

if(COMMAND qt_generate_deploy_app_script)
    qt_generate_deploy_app_script(
        TARGET LuxTrace
        OUTPUT_SCRIPT luxtrace_deploy_script
        NO_UNSUPPORTED_PLATFORM_ERROR)

    # Qt's deploy support defaults the runtime directory to "bin" behind an
    # `if(NOT QT_DEPLOY_BIN_DIR)` guard, which would leave the Qt DLLs one
    # directory away from the executable that needs them. Setting it first wins
    # the guard; both run in the same install script, so the value carries.
    install(CODE "set(QT_DEPLOY_BIN_DIR \".\")")
    install(SCRIPT ${luxtrace_deploy_script})
endif()

# windeployqt ships one platform plugin: the one the window needs. But every
# headless diagnostic this app documents -- --validate, --smoke, --bench,
# --serve -- runs under QT_QPA_PLATFORM=offscreen, and a platform plugin Qt
# cannot find is a qFatal, so an installed tree could build the GUI and abort
# on every command-line entry point. These two are a few hundred kilobytes.
foreach(plugin Qt6::QOffscreenIntegrationPlugin Qt6::QMinimalIntegrationPlugin)
    if(TARGET ${plugin})
        install(FILES "$<TARGET_FILE:${plugin}>" DESTINATION plugins/platforms)
    endif()
endforeach()

# A ZIP is what a release actually is here: the app has no registry state and
# reads nothing from outside its own directory, so unpacking it is installing
# it. `cpack -C Release` from the build directory writes one.
# A package that cannot be read without the repository beside it is not a
# package. Guarded because neither file is required to build.
foreach(doc LICENSE README.md)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${doc}")
        install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/${doc}" DESTINATION .)
    endif()
endforeach()

set(CPACK_PACKAGE_NAME              "LuxTrace")
set(CPACK_PACKAGE_VENDOR            "LuxTrace")
set(CPACK_PACKAGE_VERSION           "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Optical Design Studio")
set(CPACK_PACKAGE_FILE_NAME         "LuxTrace-${PROJECT_VERSION}-win64")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "LuxTrace")
set(CPACK_GENERATOR                 "ZIP")
include(CPack)

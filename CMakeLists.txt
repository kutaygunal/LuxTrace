cmake_minimum_required(VERSION 3.21)
project(LuxTrace VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# OpenCASCADE Technology (installed via vcpkg, triplet x64-windows)
find_package(OpenCASCADE CONFIG REQUIRED)

# Qt 6 (MSVC 2022 kit at C:/Qt/6.8.2/msvc2022_64)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets)

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
    src/core/Bsdf.h src/core/Bsdf.cpp
    src/core/Coating.h src/core/Coating.cpp
    src/core/Polarisation.h src/core/Polarisation.cpp
    src/core/Spectrum.h src/core/Spectrum.cpp
    src/core/Mesh.h src/core/Mesh.cpp
    src/core/GeometryProvider.h src/core/GeometryProvider.cpp
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
    src/ui/SurfaceInspector.h src/ui/SurfaceInspector.cpp
    src/ui/PythonPanel.h src/ui/PythonPanel.cpp
    src/ui/EnergyBarWidget.h src/ui/EnergyBarWidget.cpp
    src/ui/SourceDialog.h src/ui/SourceDialog.cpp
    src/ui/OcctViewWidget.h src/ui/OcctViewWidget.cpp
    src/ui/MainWindow.h src/ui/MainWindow.cpp
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

if(MSVC)
    target_compile_options(optics_core PRIVATE /W3 /MP)
    target_compile_options(LuxTrace PRIVATE /W3 /MP)
    target_compile_options(optics_core PRIVATE $<$<CONFIG:Release,RelWithDebInfo>:/O2>)
else()
    target_compile_options(optics_core PRIVATE -Wall -Wextra)
endif()

add_subdirectory(test)

cmake_minimum_required(VERSION 3.18)
project(gmix_win32 LANGUAGES CXX)

# ─────────────────────────────────────────────────────────────────────────────
# GMix (Windows) — temporal frame-blending tool (D3D11), delivered as a
# zero-copy OBS Studio plugin. Windows analogue of ../linux-x86_64/CMakeLists.txt.
# Build targets:
#   obs-gmix-source — the OBS plugin (capture receive + blend + shared-handle render)
#   opengl32 (proxy DLL) — capture proxy dropped next to osu!stable.exe (phase 1 producer)
#   gmix            — minimal CLI: --install-proxy / --list-gpus
#
# Requires MSVC (uses #pragma comment(lib, ...) for D3D11/DXGI import libs)
# and the Windows SDK (d3d11_1.h, dxgi1_2.h, fxc.exe).
# ─────────────────────────────────────────────────────────────────────────────

if(NOT MSVC)
    message(WARNING "GMix (Windows) is developed/tested against MSVC; other toolchains are unsupported.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ── HLSL compute shader → .cso via fxc ───────────────────────────────────────
find_program(FXC_COMPILER NAMES fxc.exe fxc REQUIRED)

set(GMIX_SHADER_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
add_custom_command(
    OUTPUT "${GMIX_SHADER_OUT_DIR}/blend.cso"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GMIX_SHADER_OUT_DIR}"
    COMMAND ${FXC_COMPILER} /nologo /T cs_5_0 /E main
            /Fo "${GMIX_SHADER_OUT_DIR}/blend.cso" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/blend.hlsl"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/blend.hlsl"
    VERBATIM
    COMMENT "Compiling HLSL: blend.hlsl -> blend.cso")
add_custom_target(gmix_shader_blend DEPENDS "${GMIX_SHADER_OUT_DIR}/blend.cso")

# ── gmix_core: D3D11 context, blend engine, IPC-receive pieces ─────────────
# Shared by the `gmix` CLI executable and the OBS plugin target.
add_library(gmix_core OBJECT
    "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d11/context.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/blend/blend_engine.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/blend/weight_generator.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ipc/frame_receiver.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ipc/imported_frame.cpp")
target_include_directories(gmix_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(gmix_core PUBLIC d3d11 dxgi)
# blend_engine.cpp embeds the compiled .cso path at runtime.
target_compile_definitions(gmix_core PUBLIC
    GMIX_SHADER_DIR="${GMIX_SHADER_OUT_DIR}")
add_dependencies(gmix_core gmix_shader_blend)

# ── gmix CLI: minimal utility (proxy install / GPU list) ───────────────────
add_executable(gmix
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/cli.cpp")
target_include_directories(gmix PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(gmix PRIVATE gmix_core)

# ── Proxy opengl32.dll capture layer (phase 1: osu!stable/OpenGL producer) ──
# Generates a full-passthrough .def at configure+build time (see
# proxy_dll/generate_def.ps1's header comment for why this can't be a static
# .def file) and forwards everything except wglSwapBuffers to a renamed copy
# of the real driver DLL that `gmix --install-proxy` places alongside it.
find_program(POWERSHELL_EXE NAMES pwsh.exe powershell.exe REQUIRED)
set(GMIX_PROXY_DEF "${CMAKE_CURRENT_BINARY_DIR}/opengl32_proxy.generated.def")
add_custom_command(
    OUTPUT "${GMIX_PROXY_DEF}"
    COMMAND ${POWERSHELL_EXE} -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/proxy_dll/generate_def.ps1"
            -OutFile "${GMIX_PROXY_DEF}"
    VERBATIM
    COMMENT "Generating opengl32 proxy .def from the real system opengl32.dll's export table")
add_custom_target(gmix_proxy_def DEPENDS "${GMIX_PROXY_DEF}")

add_library(gmix_proxy_opengl32 SHARED
    "${CMAKE_CURRENT_SOURCE_DIR}/proxy_dll/opengl32_proxy.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/proxy_dll/gl_dx_interop_capture.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ipc/frame_sender.cpp")
# NOT added to the sources list above: CMake's own "this is a .def, pass
# /DEF: automatically" handling for a SOURCE-listed .def conflicts with the
# explicit target_link_options /DEF: below (both fire, and the resulting
# double /DEF: made link.exe silently fail to recognize the `name = other.
# name` forwarder syntax, so every forwarded GL export came back as an
# "unresolved external symbol" instead of a forwarder RVA). Depend on the
# generating target directly instead.
add_dependencies(gmix_proxy_opengl32 gmix_proxy_def)
set_target_properties(gmix_proxy_opengl32 PROPERTIES
    OUTPUT_NAME "opengl32"
    # Distinct from OUTPUT_NAME: our own import lib would otherwise also be
    # named opengl32.lib, colliding with the real Windows SDK opengl32.lib
    # this target links against (see the comment above target_link_libraries)
    # -- "output filename matches input filename" from link.exe otherwise.
    ARCHIVE_OUTPUT_NAME "gmix_proxy_opengl32"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/proxy"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/proxy")
target_include_directories(gmix_proxy_opengl32 PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
# The Windows SDK's own opengl32.lib import library is linked here NOT to
# call into it (we never do -- proxy_common.cpp calls the real DLL only via
# GetProcAddress) but because MSVC's `name = module.name` .def forwarder
# syntax only accepts a `name` the linker can already resolve as a known
# import symbol; it uses that purely to validate the entry exists somewhere,
# then writes the LITERAL "opengl32_orig.<name>" text from our own .def into
# the export table regardless of which import lib satisfied the check. Without
# this, every forwarded entry fails as an "unresolved external symbol" even
# though forwarders normally need no local definition at all.
target_link_libraries(gmix_proxy_opengl32 PRIVATE d3d11 dxgi opengl32)
target_link_options(gmix_proxy_opengl32 PRIVATE "/DEF:${GMIX_PROXY_DEF}")
add_dependencies(gmix gmix_proxy_opengl32)   # so `gmix --install-proxy` always has a fresh build to copy

# ── obs-gmix-source: the OBS plugin (D3D11 shared-handle delivery) ─────────
option(GMIX_BUILD_OBS_PLUGIN "Build the obs-gmix-source OBS Studio plugin" OFF)
if(GMIX_BUILD_OBS_PLUGIN)
    find_path(OBS_INCLUDE_DIR obs.h
        PATH_SUFFIXES obs
        DOC "Directory containing OBS Studio's obs.h (see OBS_INCLUDE_DIR)")
    if(NOT OBS_INCLUDE_DIR)
        message(FATAL_ERROR
            "GMIX_BUILD_OBS_PLUGIN=ON but obs.h was not found. Pass "
            "-DOBS_INCLUDE_DIR=C:/path/to/obs-studio/libobs.")
    endif()
    add_library(obs-gmix-source MODULE "${CMAKE_CURRENT_SOURCE_DIR}/src/obs_plugin/gmix_source.cpp")
    set_target_properties(obs-gmix-source PROPERTIES
        OUTPUT_NAME "obs-gmix-source"
        # MODULE targets' primary .dll artifact is placed via
        # LIBRARY_OUTPUT_DIRECTORY on Windows too (unlike SHARED targets,
        # which split into RUNTIME_OUTPUT_DIRECTORY for the .dll and
        # ARCHIVE_OUTPUT_DIRECTORY for the .lib) -- confirmed empirically:
        # RUNTIME_OUTPUT_DIRECTORY alone left the .dll sitting in the build
        # root instead of here. Set both so this doesn't silently regress if
        # the target type ever changes.
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/obs_plugin/bin/64bit"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/obs_plugin/bin/64bit")
    get_filename_component(OBS_INCLUDE_PARENT "${OBS_INCLUDE_DIR}" DIRECTORY)
    # obs-config.h #includes "obsconfig.h", a file CMake generates (via
    # configure_file) into libobs's OWN build tree, not its source tree --
    # when building against a from-source OBS checkout (rather than an
    # installed SDK package that already bundles it), this must be located
    # and added separately. Harmless/unused if already reachable another way.
    find_path(OBS_CONFIG_INCLUDE_DIR obsconfig.h
        DOC "Directory containing OBS Studio's generated obsconfig.h (typically <obs build dir>/config)")
    target_include_directories(obs-gmix-source PRIVATE
        "${OBS_INCLUDE_DIR}"
        "${OBS_INCLUDE_PARENT}"
        "${OBS_CONFIG_INCLUDE_DIR}")
    target_link_libraries(obs-gmix-source PRIVATE gmix_core)
    # obs_*/gs_*/blog symbols resolve against OBS's own obs.lib import library
    # at link time on Windows (unlike the Linux .so, which defers resolution
    # to dlopen time) -- point at wherever the OBS SDK/dev package puts it.
    find_library(OBS_IMPORT_LIB NAMES obs libobs
        DOC "Path to obs.lib / libobs.lib from the OBS Studio dev package")
    if(NOT OBS_IMPORT_LIB)
        message(FATAL_ERROR
            "GMIX_BUILD_OBS_PLUGIN=ON but obs.lib/libobs.lib was not found. Pass "
            "-DOBS_IMPORT_LIB=C:/path/to/obs.lib.")
    endif()
    target_link_libraries(obs-gmix-source PRIVATE "${OBS_IMPORT_LIB}")
    add_custom_command(TARGET obs-gmix-source POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo
            "obs-gmix-source staged at: ${CMAKE_CURRENT_BINARY_DIR}/obs_plugin/"
        VERBATIM)
endif()

# ── Tests ───────────────────────────────────────────────────────────────────
enable_testing()

# Pure-math weight generator (no GPU) -- portable, identical to the Linux test.
add_executable(test_weight_generator
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_weight_generator.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/blend/weight_generator.cpp")
target_include_directories(test_weight_generator PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests")
add_test(NAME weight_generator COMMAND test_weight_generator)

# GPU blend engine dispatch sanity (needs a real D3D11 device + the compiled
# blend.cso -- see the GMIX_SHADER_DIR compile definition on gmix_core, which
# this target also links).
add_executable(test_blend_engine "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_blend_engine.cpp")
target_include_directories(test_blend_engine PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests")
target_link_libraries(test_blend_engine PRIVATE gmix_core)
add_test(NAME blend_engine COMMAND test_blend_engine)

# IPC round-trip (named pipe; no GPU). frame_sender.cpp is only otherwise
# compiled into the proxy DLL, so it's added explicitly here too, same as
# the Linux test_ipc target adds frame_sender.cpp alongside gmix_core.
add_executable(test_ipc
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ipc.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ipc/frame_sender.cpp")
target_include_directories(test_ipc PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests")
target_link_libraries(test_ipc PRIVATE gmix_core)
add_test(NAME ipc COMMAND test_ipc)

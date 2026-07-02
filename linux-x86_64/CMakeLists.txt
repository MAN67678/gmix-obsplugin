cmake_minimum_required(VERSION 3.18)
project(gmix LANGUAGES CXX C)

# ─────────────────────────────────────────────────────────────────────────────
# GMix — temporal frame-blending tool (Vulkan), delivered as a zero-copy OBS
# Studio plugin. Build targets:
#   obs-gmix-source — the OBS plugin (capture receive + blend + dma-buf render)
#   VkLayer_GMIX    — implicit Vulkan capture layer (.so + manifest JSON)
#   gmix            — minimal CLI: --install-layer / --list-gpus / --attach
# ─────────────────────────────────────────────────────────────────────────────

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_compile_options(
    -Wall -Wextra -Wpedantic -Wno-unused-parameter
    # Vulkan headers are chatty
    $<$<CXX_COMPILER_ID:GNU>:-Wno-nullability-extension>
)

# ── Dependencies ─────────────────────────────────────────────────────────────
find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)

# Shader → SPIR-V at build time via glslangValidator.
find_program(GLSLANG_VALIDATOR NAMES glslangValidator REQUIRED)

# ── Shader compilation helper ────────────────────────────────────────────────
# Produces shaders/<name>.spv and a custom target so other targets can depend
# on the SPIR-V being built before they run.
function(gmix_compile_shader src_file)
    get_filename_component(_name "${src_file}" NAME_WE)
    set(_out "${CMAKE_CURRENT_BINARY_DIR}/shaders/${_name}.spv")
    add_custom_command(
        OUTPUT "${_out}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
        COMMAND ${GLSLANG_VALIDATOR} -V --target-env vulkan1.2
                "${src_file}" -o "${_out}"
        DEPENDS "${src_file}"
        VERBATIM
        COMMENT "Compiling SPIR-V: ${_name}.comp → ${_name}.spv"
    )
    add_custom_target(gmix_shader_${_name} DEPENDS "${_out}")
endfunction()
gmix_compile_shader("${CMAKE_CURRENT_SOURCE_DIR}/shaders/blend.comp")
gmix_compile_shader("${CMAKE_CURRENT_SOURCE_DIR}/shaders/resample_blur.comp")

# ── gmix_core: Vulkan context, blend engine, IPC-receive pieces ─────────────
# Shared by the `gmix` CLI executable and the OBS plugin target. Deliberately
# an explicit source list, not a GLOB -- must never contain main.cpp/cli.cpp.
add_library(gmix_core OBJECT
    "${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan/context.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/blend/blend_engine.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/blend/weight_generator.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ipc/frame_receiver.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ipc/imported_frame.cpp")
target_include_directories(gmix_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(gmix_core PUBLIC
    Vulkan::Vulkan
    Threads::Threads)
# blend_engine.cpp embeds the compiled SPIR-V path at runtime.
target_compile_definitions(gmix_core PUBLIC
    GMIX_SHADER_DIR="${CMAKE_CURRENT_BINARY_DIR}/shaders")
add_dependencies(gmix_core gmix_shader_blend gmix_shader_resample_blur)

# ── gmix CLI: minimal utility (layer install / GPU list / IPC debug client) ─
add_executable(gmix
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/cli.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ipc/ipc_client.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gmix.hpp")
target_include_directories(gmix PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(gmix PRIVATE gmix_core rt)

# ── VkLayer_GMIX capture layer ──────────────────────────────────────────────
file(GLOB_RECURSE LAYER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/layer/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/capture/*.cpp")
# VulkanLayerCapture needs FrameSender (its glob above only covers src/capture/).
list(APPEND LAYER_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/ipc/frame_sender.cpp")
add_library(VkLayer_GMIX SHARED ${LAYER_SOURCES})
set_target_properties(VkLayer_GMIX PROPERTIES
    PREFIX ""                          # libVkLayer_GMIX.so → VkLayer_GMIX.so
    OUTPUT_NAME "VkLayer_GMIX"
    SOVERSION "")
target_include_directories(VkLayer_GMIX PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/layer"
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(VkLayer_GMIX PRIVATE
    Vulkan::Vulkan
    Threads::Threads
    rt)

# Place the manifest + .so in build/layer/ so the layer dir is self-contained
# for development testing via VK_LAYER_PATH or by copying into the user dir.
set_target_properties(VkLayer_GMIX PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/layer")
add_custom_command(TARGET VkLayer_GMIX POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_CURRENT_SOURCE_DIR}/layer/VkLayer_GMIX.json"
        "${CMAKE_CURRENT_BINARY_DIR}/layer/VkLayer_GMIX.json"
    COMMAND ${CMAKE_COMMAND} -E echo
        "Layer staged at: ${CMAKE_CURRENT_BINARY_DIR}/layer/"
    VERBATIM)

# ── obs-gmix-source: the OBS plugin (zero-copy dma-buf delivery) ───────────
option(GMIX_BUILD_OBS_PLUGIN "Build the obs-gmix-source OBS Studio plugin" OFF)
if(GMIX_BUILD_OBS_PLUGIN)
    find_path(OBS_INCLUDE_DIR obs.h
        PATH_SUFFIXES obs
        DOC "Directory containing OBS Studio's obs.h (see OBS_INCLUDE_DIR)")
    if(NOT OBS_INCLUDE_DIR)
        message(FATAL_ERROR
            "GMIX_BUILD_OBS_PLUGIN=ON but obs.h was not found. Pass "
            "-DOBS_INCLUDE_DIR=/path/to/obs/include (e.g. the Flatpak OBS's "
            ".../files/include/obs).")
    endif()
    add_library(obs-gmix-source MODULE src/obs_plugin/gmix_source.cpp)
    set_target_properties(obs-gmix-source PROPERTIES
        PREFIX ""
        OUTPUT_NAME "obs-gmix-source"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/obs_plugin/bin/64bit")
    # OBS's public headers pull in simde (SIMD Everywhere) for graphics/vec*.h;
    # it ships as a sibling of obs/ in the same include tree, not inside it.
    get_filename_component(OBS_INCLUDE_PARENT "${OBS_INCLUDE_DIR}" DIRECTORY)
    target_include_directories(obs-gmix-source PRIVATE
        "${OBS_INCLUDE_DIR}"
        "${OBS_INCLUDE_PARENT}")
    target_link_libraries(obs-gmix-source PRIVATE gmix_core)
    # obs_*/gs_*/blog symbols are resolved at dlopen time against the host
    # OBS process's already-loaded libobs -- do not require them at link time.
    target_link_options(obs-gmix-source PRIVATE -Wl,--allow-shlib-undefined)
    add_custom_command(TARGET obs-gmix-source POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${CMAKE_CURRENT_BINARY_DIR}/obs_plugin/data/locale"
        COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_SOURCE_DIR}/data/locale/en-US.ini"
            "${CMAKE_CURRENT_BINARY_DIR}/obs_plugin/data/locale/en-US.ini"
        COMMAND ${CMAKE_COMMAND} -E echo
            "obs-gmix-source staged at: ${CMAKE_CURRENT_BINARY_DIR}/obs_plugin/"
        VERBATIM)
endif()

# ── Tests ───────────────────────────────────────────────────────────────────
enable_testing()

# Pure-math weight generator (no GPU).
add_executable(test_weight_generator tests/test_weight_generator.cpp
                                 src/blend/weight_generator.cpp)
target_include_directories(test_weight_generator PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests")
target_link_libraries(test_weight_generator PRIVATE Threads::Threads)
add_test(NAME weight_generator COMMAND test_weight_generator)

# GPU blend engine dispatch sanity (needs Vulkan + the compiled SPIR-V).
add_executable(test_blend_engine tests/test_blend_engine.cpp)
target_include_directories(test_blend_engine PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests")
target_link_libraries(test_blend_engine PRIVATE gmix_core)
add_test(NAME blend_engine COMMAND test_blend_engine)

# IPC round-trip (unix socket + SCM_RIGHTS fd passing; no GPU).
add_executable(test_ipc tests/test_ipc.cpp
               src/ipc/frame_sender.cpp)
target_include_directories(test_ipc PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests")
target_link_libraries(test_ipc PRIVATE gmix_core)
add_test(NAME ipc COMMAND test_ipc)

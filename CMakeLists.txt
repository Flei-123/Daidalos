# Daidalos -- CMake build. Mirrors build.sh layer by layer; build.sh stays the
# reference (it runs the leak tests), this file exists so an IDE or a package
# manager can build the engine without reading a shell script.
#
#   cmake -B build-cmake -DTALOS_DIR=../talos -DAULOS_DIR=../aulos -DMNEMOSYNE_DIR=../mnemosyne
#   cmake --build build-cmake -j
#   ctest --test-dir build-cmake
#
# Every sibling library is optional; missing ones are skipped the same way
# build.sh skips them.
cmake_minimum_required(VERSION 3.16)
project(Daidalos LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(TALOS_DIR     "${CMAKE_SOURCE_DIR}/../talos"     CACHE PATH "Talos checkout (built: build/libtalos.a)")
set(AULOS_DIR     "${CMAKE_SOURCE_DIR}/../aulos"     CACHE PATH "Aulos checkout (built: build/libaulos.a)")
set(MNEMOSYNE_DIR "${CMAKE_SOURCE_DIR}/../mnemosyne" CACHE PATH "Mnemosyne checkout (sources compiled in)")
option(DAI_WITH_VULKAN "Build the Vulkan renderer, editor and examples" ON)

set(DAI_FLAGS -fno-rtti -fno-exceptions -ffp-contract=fast -Wall -Wno-unused-parameter
              -mavx2 -mbmi -mpopcnt -mlzcnt -mf16c -mfma -mfpmath=sse)
find_package(Threads REQUIRED)

# ---- core: engine + physics backends + scene/editor/document layers -----------
set(HAVE_TALOS OFF)
if(EXISTS "${TALOS_DIR}/TalC/talos.h" AND EXISTS "${TALOS_DIR}/build/libtalos.a")
  set(HAVE_TALOS ON)
endif()
set(HAVE_AULOS OFF)
if(EXISTS "${AULOS_DIR}/include" AND EXISTS "${AULOS_DIR}/build/libaulos.a")
  set(HAVE_AULOS ON)
endif()
message(STATUS "Talos: ${HAVE_TALOS}  Aulos: ${HAVE_AULOS}")

add_library(daidalos STATIC
  src/dai_engine.cpp src/physics_null.cpp src/dai_audio.cpp
  src/dai_scene.cpp src/dai_input.cpp src/dai_editor.cpp src/dai_dock.cpp src/dai_project.cpp
  src/dai_doc.cpp src/dai_doc_text.cpp src/dai_doc_sync.cpp)
target_include_directories(daidalos PUBLIC include PRIVATE src)
target_compile_options(daidalos PRIVATE ${DAI_FLAGS})
target_link_libraries(daidalos PUBLIC Threads::Threads m)

# dai_engine.cpp is compiled WITHOUT the Talos include path on purpose (leak
# test, see build.sh). Only the backend file sees talos.h.
if(HAVE_TALOS)
  add_library(daidalos_talos OBJECT src/physics_talos.cpp)
  target_include_directories(daidalos_talos PRIVATE include src "${TALOS_DIR}/TalC")
  target_compile_options(daidalos_talos PRIVATE ${DAI_FLAGS})
  target_sources(daidalos PRIVATE $<TARGET_OBJECTS:daidalos_talos>)
  target_link_libraries(daidalos PUBLIC "${TALOS_DIR}/build/libtalos.a")
else()
  target_compile_definitions(daidalos PRIVATE DAI_NO_TALOS)
endif()

if(HAVE_AULOS)
  target_include_directories(daidalos PRIVATE "${AULOS_DIR}/include")
  target_link_libraries(daidalos PUBLIC "${AULOS_DIR}/build/libaulos.a")
else()
  target_compile_definitions(daidalos PRIVATE DAI_NO_AUDIO)
endif()

# ---- tests that need no renderer ----------------------------------------------
enable_testing()
function(dai_test name)
  add_executable(${name} ${ARGN})
  target_include_directories(${name} PRIVATE include src)
  target_compile_options(${name} PRIVATE ${DAI_FLAGS})
  target_link_libraries(${name} PRIVATE daidalos)
  add_test(NAME ${name} COMMAND ${name})
endfunction()
dai_test(test_daidalos tests/test_daidalos.cpp)
dai_test(test_merge    tests/test_merge.cpp)
dai_test(test_save     tests/test_save.cpp)
dai_test(test_input    tests/test_input.cpp)
dai_test(test_editor   tests/test_editor.cpp)
dai_test(test_editor_live tests/test_editor_live.cpp)
dai_test(test_doc      tests/test_doc.cpp)
dai_test(test_play     tests/test_play.cpp)
dai_test(test_cam      tests/test_cam.cpp)
dai_test(test_image    tests/test_image.cpp src/dai_inflate.cpp)
dai_test(test_font     tests/test_font.cpp src/dai_font.cpp)
dai_test(test_svg      tests/test_svg.cpp src/dai_svg.cpp src/dai_icons.cpp)
dai_test(test_project  tests/test_project.cpp)
if(HAVE_TALOS)
  dai_test(test_talos tests/test_talos.cpp)
endif()

add_executable(hello_daidalos examples/hello_daidalos.cpp)
target_link_libraries(hello_daidalos PRIVATE daidalos)
target_compile_options(hello_daidalos PRIVATE ${DAI_FLAGS})

# ---- renderer, assets, editor, examples (need Vulkan headers) -------------------
find_package(Vulkan QUIET)
find_package(X11 QUIET)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(DAI_WITH_VULKAN AND Vulkan_FOUND AND Python3_FOUND)
  message(STATUS "Renderer: Vulkan (X11: ${X11_FOUND})")
  add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/dai_shaders_embed.cpp
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/embed_shaders.py
            ${CMAKE_SOURCE_DIR}/shaders ${CMAKE_BINARY_DIR}/dai_shaders_embed.cpp
    DEPENDS tools/embed_shaders.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "embedding shaders")
  set(DAI_VK_SOURCES
    src/rhi_vulkan.cpp src/rhi_vulkan_frame.cpp src/rhi_vulkan_texture.cpp
    ${CMAKE_BINARY_DIR}/dai_shaders_embed.cpp
    src/dai_meshgen.cpp src/dai_image.cpp src/dai_inflate.cpp src/dai_json.cpp
    src/dai_gltf.cpp src/dai_gltf_geom.cpp src/dai_fracture.cpp src/dai_gltf_write.cpp
    src/dai_particles.cpp src/dai_font.cpp src/dai_svg.cpp src/dai_icons.cpp
    src/dai_ui.cpp src/dai_update.cpp src/dai_editor_ui.cpp)
  if(X11_FOUND)
    list(APPEND DAI_VK_SOURCES src/rhi_vulkan_window.cpp)
  endif()
  add_library(daidalos_vk STATIC ${DAI_VK_SOURCES})
  target_include_directories(daidalos_vk PUBLIC include PRIVATE src)
  target_compile_options(daidalos_vk PRIVATE ${DAI_FLAGS})
  target_link_libraries(daidalos_vk PUBLIC daidalos Vulkan::Vulkan)
  if(X11_FOUND)
    target_link_libraries(daidalos_vk PUBLIC X11::X11)
  endif()

  if(EXISTS "${MNEMOSYNE_DIR}/include/mnemosyne.h")
    message(STATUS "Assets: Mnemosyne at ${MNEMOSYNE_DIR}")
    add_library(daidalos_assets STATIC src/dai_assets.cpp
      "${MNEMOSYNE_DIR}/src/mne_path.cpp" "${MNEMOSYNE_DIR}/src/mne_vfs.cpp"
      "${MNEMOSYNE_DIR}/src/mne_pack.cpp" "${MNEMOSYNE_DIR}/src/mne_registry.cpp")
    target_include_directories(daidalos_assets PUBLIC include "${MNEMOSYNE_DIR}/include" PRIVATE src "${MNEMOSYNE_DIR}/src")
    set_source_files_properties(src/dai_assets.cpp PROPERTIES COMPILE_OPTIONS "${DAI_FLAGS}")
    target_link_libraries(daidalos_assets PUBLIC daidalos_vk)
  endif()

  foreach(ex sandbox_demo vehicle_demo model_viewer particles_demo)
    if(EXISTS "${CMAKE_SOURCE_DIR}/examples/${ex}.cpp")
      add_executable(${ex} examples/${ex}.cpp)
      target_compile_options(${ex} PRIVATE ${DAI_FLAGS})
      target_link_libraries(${ex} PRIVATE daidalos_vk)
    endif()
  endforeach()
  if(X11_FOUND)
    add_executable(window_demo examples/window_demo.cpp)
    target_link_libraries(window_demo PRIVATE daidalos_vk)
    target_compile_options(window_demo PRIVATE ${DAI_FLAGS})
  endif()
  add_executable(daifracture tools/daifracture.cpp src/dai_fracture.cpp src/dai_gltf_geom.cpp src/dai_gltf_write.cpp src/dai_json.cpp)
  target_include_directories(daifracture PRIVATE include src)
  target_compile_options(daifracture PRIVATE ${DAI_FLAGS})
else()
  message(STATUS "Renderer: skipped (Vulkan headers or python3 missing, or DAI_WITH_VULKAN=OFF)")
endif()

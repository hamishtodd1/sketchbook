cmake_minimum_required(VERSION 3.20)
project(sketchbook LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------- dependencies
include(FetchContent)

set(BUILD_EXAMPLES   OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# A 4.3 core context, not raylib's default 3.3. Compute shaders and shader
# storage buffers are both 4.3, and both are load-bearing now: a viz builds its
# mesh in a compute shader and hands it over in an SSBO, and the per-line values
# a box reports come back the same way. Our own shaders still say `#version 330`
# where 330 is enough -- it is forward compatible into a 4.3 context.
set(OPENGL_VERSION "4.3" CACHE STRING "" FORCE)

# The release tarball rather than a git clone: the same source, without a .git
# directory -- which, shallow clone or not, was 261 MB of the 336 MB fetched.
# Pinned by hash, so a changed archive fails the configure instead of building.
FetchContent_Declare(raylib
  URL      https://github.com/raysan5/raylib/archive/refs/tags/5.5.tar.gz
  URL_HASH SHA256=aea98ecf5bc5c5e0b789a76de0083a21a70457050ea4cc2aec7566935f5e258e
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(raylib)

# --------------------------------------------------------------------- sources
add_executable(sketchbook
  src/main.cpp
  src/app.cpp
  src/blocks.cpp
  src/compiler.cpp
  src/mathprogram.cpp
  src/mathline.cpp
  src/surfacedw.cpp
  src/volumedw.cpp
  src/types/Clifford.cpp
  src/types/Complex.cpp
  src/types/Color.cpp
  src/types/Complex.cpp
  src/types/RealFunction.cpp
  src/types/RealViz.cpp
  src/types/RealFunctionViz.cpp
  src/shaderwindow.cpp
  src/colordw.cpp
  src/media.cpp
  src/video.cpp
  src/slides.cpp
  src/spacedw.cpp
  src/filedialog_win32.cpp
  src/highlight.cpp
  src/fontstore.cpp
  src/glext_win32.cpp
  src/glslops.cpp
  src/grid.cpp
  src/text.cpp
  src/touch.cpp
  src/touch_win32.cpp
  src/process_win32.cpp
  src/busy_win32.cpp
  src/recent.cpp
  src/settings.cpp
  src/ui.cpp
  src/viewport.cpp
  src/viz.cpp
  src/vizglsl.cpp
  src/tex/tex_parse.cpp
  src/tex/tex_layout.cpp
  src/tex/tex_symbols.cpp
)

target_include_directories(sketchbook PRIVATE src)
find_package(Threads REQUIRED)
target_link_libraries(sketchbook PRIVATE raylib Threads::Threads)
if(WIN32)
  # GetOpenFileName/GetSaveFileName for the canvas load/save dialogs.
  target_link_libraries(sketchbook PRIVATE comdlg32)
endif()

# Assets are found next to the executable first, then at the source tree path
# (so the binary runs both from the build dir and from an installed copy).
target_compile_definitions(sketchbook PRIVATE
  SKETCHBOOK_SOURCE_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets"
  # The built-in vizes are sketches, and they live in the repo.
  SKETCHBOOK_VIZES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/vizes")

if(MSVC)
  target_compile_options(sketchbook PRIVATE /W4 /permissive-)
else()
  target_compile_options(sketchbook PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# A build has to succeed while the app is open, since it usually is. Windows
# will not overwrite a running executable but will rename one, so the old one is
# moved aside just before the link (cmake/MoveAsideRunning.cmake).
add_custom_command(TARGET sketchbook PRE_LINK
  COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:sketchbook>
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/MoveAsideRunning.cmake")

add_custom_command(TARGET sketchbook POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CMAKE_CURRENT_SOURCE_DIR}/assets"
          "$<TARGET_FILE_DIR:sketchbook>/assets"
  COMMENT "Staging assets next to the executable")

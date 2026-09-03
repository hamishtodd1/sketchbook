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

FetchContent_Declare(raylib
  GIT_REPOSITORY https://github.com/raysan5/raylib.git
  GIT_TAG        5.5
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   TRUE)
FetchContent_MakeAvailable(raylib)

# --------------------------------------------------------------------- sources
add_executable(sketchbook
  src/main.cpp
  src/app.cpp
  src/blocks.cpp
  src/compiler.cpp
  src/mathprogram.cpp
  src/mathline.cpp
  src/types/Clifford.cpp
  src/types/RealFunction.cpp
  src/types/RealViz.cpp
  src/types/RealFunctionViz.cpp
  src/shaderwindow.cpp
  src/media.cpp
  src/video.cpp
  src/slides.cpp
  src/filedialog_win32.cpp
  src/highlight.cpp
  src/fontstore.cpp
  src/grid.cpp
  src/text.cpp
  src/touch.cpp
  src/touch_win32.cpp
  src/process_win32.cpp
  src/ui.cpp
  src/viewport.cpp
  src/viz.cpp
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
  SKETCHBOOK_SOURCE_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets")

if(MSVC)
  target_compile_options(sketchbook PRIVATE /W4 /permissive-)
else()
  target_compile_options(sketchbook PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

add_custom_command(TARGET sketchbook POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CMAKE_CURRENT_SOURCE_DIR}/assets"
          "$<TARGET_FILE_DIR:sketchbook>/assets"
  COMMENT "Staging assets next to the executable")

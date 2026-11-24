cmake_minimum_required(VERSION 3.20)
project(Tributary VERSION 0.2.0 LANGUAGES CXX)
add_compile_definitions(TRIBUTARY_VERSION="${PROJECT_VERSION}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(TRIBUTARY_BUILD_APP "Build the desktop app (needs the Vulkan loader)" ON)
option(TRIBUTARY_BUILD_TOOLS "Build developer CLI and tests" ON)


# ---------------------------------------------------------------- git engine
add_library(tribgit STATIC
  src/git/core.cpp src/git/zlib.cpp src/git/objects.cpp src/git/refs.cpp
  src/git/index.cpp src/git/diff.cpp src/git/repo.cpp src/git/transport.cpp src/git/process.cpp src/git/lfs.cpp src/git/archive.cpp
  src/net/http.cpp src/net/json.cpp src/net/github.cpp src/net/ai.cpp
  src/net/tls_openssl.cpp src/net/tls_securetransport.cpp src/net/tls_schannel.cpp)
target_include_directories(tribgit PUBLIC src)
if(WIN32)
  target_link_libraries(tribgit PUBLIC ws2_32 secur32 crypt32)
  target_compile_definitions(tribgit PUBLIC _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
elseif(APPLE)
  target_link_libraries(tribgit PUBLIC "-framework Security" "-framework CoreFoundation")
else()
  find_package(OpenSSL REQUIRED)
  target_link_libraries(tribgit PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif()
if(MSVC)
  target_compile_options(tribgit PRIVATE /W3 /utf-8 /bigobj)
else()
  target_compile_options(tribgit PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

if(TRIBUTARY_BUILD_TOOLS)
  add_executable(tribcli tools/tribcli.cpp)
  target_link_libraries(tribcli PRIVATE tribgit)
endif()

# ---------------------------------------------------------------- app
if(TRIBUTARY_BUILD_APP)
  # Only Vulkan *headers* are needed: the loader (vulkan-1.dll / libvulkan.so.1) is
  # opened at runtime, so there's no link-time dependency. macOS uses the Metal backend.
  find_path(VULKAN_HEADERS_DIR vulkan/vulkan.h HINTS $ENV{VULKAN_SDK}/include)
  if(NOT VULKAN_HEADERS_DIR)
    include(FetchContent)
    FetchContent_Declare(vulkan_headers
      GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
      GIT_TAG v1.3.296
      GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(vulkan_headers)
    set(VULKAN_HEADERS_DIR ${vulkan_headers_SOURCE_DIR}/include)
  endif()
  # Tributary's own typefaces, generated from the designs in tools/fontgen at build time
  add_executable(fontgen tools/fontgen/fontgen.cpp)
  set(FONT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/fonts)
  add_custom_command(
    OUTPUT ${FONT_DIR}/TributarySans-Regular.ttf ${FONT_DIR}/TributarySans-Bold.ttf ${FONT_DIR}/TributaryMono-Regular.ttf
    COMMAND ${CMAKE_COMMAND} -E make_directory ${FONT_DIR}
    COMMAND fontgen ${FONT_DIR}
    DEPENDS fontgen tools/fontgen/fontgen.cpp tools/fontgen/geom.hpp tools/fontgen/ttf.hpp
            tools/fontgen/decomp.inc tools/fontgen/boxdraw.inc
    VERBATIM)
  set(APP_SOURCES
    src/gfx/font.cpp src/gfx/renderer.cpp src/gfx/embedded.cpp src/gfx/image.cpp
    src/ui/ui.cpp src/ui/widgets.cpp src/ui/code_editor.cpp src/ui/terminal.cpp
    src/app/main.cpp src/app/app.cpp src/app/views.cpp src/app/dialogs.cpp src/app/ide.cpp src/app/agent.cpp src/app/mcp.cpp src/app/update.cpp src/platform/common.cpp)
  if(WIN32)
    list(APPEND APP_SOURCES src/platform/platform_win32.cpp src/platform/pty_win32.cpp src/app/tributary.rc src/app/tributary.manifest)
    add_executable(Tributary WIN32 ${APP_SOURCES})
    target_link_libraries(Tributary PRIVATE user32 gdi32 shell32 ole32 dwmapi imm32 comdlg32 shcore)
  elseif(APPLE)
    list(APPEND APP_SOURCES src/platform/platform_cocoa.cpp src/platform/pty_posix.cpp src/gfx/renderer_metal.cpp)
    add_executable(Tributary MACOSX_BUNDLE ${APP_SOURCES} assets/icon/Tributary.icns)
    set_source_files_properties(assets/icon/Tributary.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(Tributary PROPERTIES
      MACOSX_BUNDLE_BUNDLE_NAME "Tributary"
      MACOSX_BUNDLE_GUI_IDENTIFIER "dev.p1e2r.tributary"
      MACOSX_BUNDLE_ICON_FILE "Tributary.icns"
      MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
      MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION})
    target_link_libraries(Tributary PRIVATE "-framework Cocoa" "-framework QuartzCore" "-framework Metal" "-framework CoreGraphics")
  else()
    find_package(X11 REQUIRED)
    list(APPEND APP_SOURCES src/platform/platform_x11.cpp src/platform/pty_posix.cpp)
    add_executable(Tributary ${APP_SOURCES})
    target_link_libraries(Tributary PRIVATE X11::X11)
  endif()
  target_link_libraries(Tributary PRIVATE tribgit ${CMAKE_DL_LIBS})
  target_include_directories(Tributary PRIVATE src ${VULKAN_HEADERS_DIR} ${CMAKE_CURRENT_BINARY_DIR}/generated)
  install(TARGETS Tributary BUNDLE DESTINATION . RUNTIME DESTINATION bin)
  if(UNIX AND NOT APPLE)
    install(FILES packaging/linux/tributary.desktop DESTINATION share/applications)
    install(FILES assets/icon/icon-256.png DESTINATION share/icons/hicolor/256x256/apps RENAME tributary.png)
  endif()
  if(MSVC)
    target_compile_options(Tributary PRIVATE /W3 /utf-8 /bigobj)
  endif()

  # Embed fonts (and precompiled SPIR-V) into the binary with a tiny CMake step.
  set(EMBED_FILES
    ${FONT_DIR}/TributarySans-Regular.ttf
    ${FONT_DIR}/TributarySans-Bold.ttf
    ${FONT_DIR}/TributaryMono-Regular.ttf
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/shaders/ui.vert.spv
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/shaders/ui.frag.spv)
  # GitHub Desktop's empty-state illustrations (MIT), pre-rendered at 2x
  file(GLOB ILLUSTRATIONS ${CMAKE_CURRENT_SOURCE_DIR}/assets/illustrations/*.png)
  list(APPEND EMBED_FILES ${ILLUSTRATIONS})
  string(REPLACE ";" "|" EMBED_LIST "${EMBED_FILES}")
  add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_data.inc
    COMMAND ${CMAKE_COMMAND} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_data.inc
            "-DFILES=${EMBED_LIST}" -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
    DEPENDS ${EMBED_FILES} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
    VERBATIM)
  add_custom_target(embed_assets DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_data.inc)
  add_dependencies(Tributary embed_assets)
  set_source_files_properties(src/gfx/embedded.cpp PROPERTIES
    OBJECT_DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_data.inc)
endif()

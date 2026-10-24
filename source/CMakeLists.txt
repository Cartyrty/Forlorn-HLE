cmake_minimum_required(VERSION 3.16)
project(ForlornHLE LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# No build-machine paths in the binaries (they carry the user name): source file names in assert messages and
# __FILE__ are made relative to the source folder (dynarmic and its externals included, so this comes first), and
# the Windows exe names its PDB without a folder.
if(MSVC)
  file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/" SOURCE_PREFIX)
  file(TO_NATIVE_PATH "${CMAKE_CURRENT_BINARY_DIR}/" BINARY_PREFIX)
  add_compile_options("/d1trimfile:${SOURCE_PREFIX}" "/d1trimfile:${BINARY_PREFIX}")
else()
  add_compile_options("-ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}/=" "-ffile-prefix-map=${CMAKE_CURRENT_BINARY_DIR}/=")
  # No RPATH either: it would record the library folders of the build machine (play_linux.sh sets the library path).
  set(CMAKE_SKIP_RPATH ON)
endif()

# dynarmic: 32-bit ARM guest (armv7, ARM + Thumb-2), x86-64 host.
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(Boost_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/boost" CACHE PATH "" FORCE)
add_subdirectory(external/dynarmic EXCLUDE_FROM_ALL)

# The HLE core (CPU, loader, Objective-C runtime, libc, Foundation, UIKit, GL, audio) is the same on every host; the
# operating-system layer has one implementation per host: src/platform/win32 (Windows API) and src/platform/sdl
# (Linux: SDL2, FFmpeg, POSIX). Self-registering HLE functions (static registrars) are kept by building the sources
# straight into the executable.
file(GLOB_RECURSE HLE_SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.h)
list(FILTER HLE_SOURCES EXCLUDE REGEX "/src/platform/(win32|sdl)/")
if(WIN32)
  file(GLOB PLATFORM_SOURCES CONFIGURE_DEPENDS src/platform/win32/*.cpp)
else()
  file(GLOB PLATFORM_SOURCES CONFIGURE_DEPENDS src/platform/sdl/*.cpp)
endif()
add_executable(ForlornHLE ${HLE_SOURCES} ${PLATFORM_SOURCES})
target_include_directories(ForlornHLE PRIVATE src external/stb)
target_link_libraries(ForlornHLE PRIVATE dynarmic)

if(WIN32)
  target_link_libraries(ForlornHLE PRIVATE opengl32 winmm comctl32 ole32 xaudio2 mfplat mfreadwrite mfuuid)
  target_compile_definitions(ForlornHLE PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
  pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswresample)
  find_package(OpenGL REQUIRED)
  find_package(Threads REQUIRED)
  target_link_libraries(ForlornHLE PRIVATE PkgConfig::SDL2 PkgConfig::FFMPEG OpenGL::GL Threads::Threads ${CMAKE_DL_LIBS})
endif()
target_compile_definitions(ForlornHLE PRIVATE _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING)

if(MSVC)
  target_compile_options(ForlornHLE PRIVATE /utf-8 /W3 /permissive- /Zc:preprocessor /MP)
  target_link_options(ForlornHLE PRIVATE /DEBUG /PDBALTPATH:%_PDB%)
else()
  target_compile_options(ForlornHLE PRIVATE -Wno-deprecated-declarations)
endif()

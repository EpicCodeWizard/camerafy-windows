cmake_minimum_required(VERSION 3.25)
project(camerafy LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(CF_X64 ON)
else()
  set(CF_X64 OFF)
endif()

# Frame-path flags apply everywhere: there is no translation unit where a stack cookie
# or a missed inlining opportunity is worth having. /Zi keeps PDBs for disassembly checks.
foreach(cfg RELEASE RELWITHDEBINFO)
  string(REPLACE "/Ob2" "/Ob3" CMAKE_CXX_FLAGS_${cfg} "${CMAKE_CXX_FLAGS_${cfg}}")
  string(REPLACE "/Ob1" "/Ob3" CMAKE_CXX_FLAGS_${cfg} "${CMAKE_CXX_FLAGS_${cfg}}")
endforeach()
add_compile_options(
  /permissive- /utf-8 /W3 /Zc:__cplusplus /Zc:preprocessor /MP
  /arch:AVX2 /Gw /GS- /Zi
  $<$<NOT:$<CONFIG:Debug>>:/O2>
)
add_compile_definitions(
  WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
  _WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A00000C
  _CRT_SECURE_NO_WARNINGS
)
add_link_options(/DEBUG /OPT:REF /OPT:ICF)

set(CF_COMMON ${CMAKE_SOURCE_DIR}/src/common)

# ---------------------------------------------------------------- DirectShow base classes
file(GLOB STRMBASE_SRC ${CMAKE_SOURCE_DIR}/third_party/baseclasses/*.cpp)
add_library(strmbase STATIC ${STRMBASE_SRC})
target_include_directories(strmbase PUBLIC ${CMAKE_SOURCE_DIR}/third_party/baseclasses)
# The vendored sources predate /W3-clean code; keep their noise out of our build log.
target_compile_options(strmbase PRIVATE /W0 /Zc:strictStrings- /UNOMINMAX)
target_link_libraries(strmbase PUBLIC strmiids winmm)

# ---------------------------------------------------------------- DirectShow filter DLL
add_library(camerafy_ds SHARED
  src/ds_filter/ds_filter.cpp
  src/ds_filter/ds_filter.h
  src/ds_filter/dllmain.cpp
  src/ds_filter/camerafy_ds.def)
target_include_directories(camerafy_ds PRIVATE ${CF_COMMON})
target_link_libraries(camerafy_ds PRIVATE strmbase ole32 oleaut32 uuid advapi32 winmm)
set_target_properties(camerafy_ds PROPERTIES OUTPUT_NAME camerafy_ds)

# ---------------------------------------------------------------- MF media source DLL
add_library(camerafy_mf SHARED
  src/mf_source/dllmain.cpp
  src/mf_source/activator.cpp
  src/mf_source/media_source.cpp
  src/mf_source/media_stream.cpp
  src/mf_source/shared_frames.cpp
  src/mf_source/mf_source.h
  src/mf_source/camerafy_mf.def)
target_include_directories(camerafy_mf PRIVATE ${CF_COMMON})
target_link_libraries(camerafy_mf PRIVATE mfplat mfuuid mf avrt ole32 oleaut32 uuid advapi32 ksuser)

# ---------------------------------------------------------------- shared-memory test reader
add_executable(cf_test_reader src/test_reader/test_reader.cpp)
target_include_directories(cf_test_reader PRIVATE ${CF_COMMON})
target_link_libraries(cf_test_reader PRIVATE synchronization)

# ---------------------------------------------------------------- DirectShow probe (both bitnesses)
# Enumerates video input devices the way DirectShow apps do and pulls frames from ours
# through a real graph, so the x86 registration is tested by an x86 process.
add_executable(ds_probe src/ds_probe/ds_probe.cpp)
target_include_directories(ds_probe PRIVATE ${CF_COMMON})
target_link_libraries(ds_probe PRIVATE strmbase ole32 oleaut32 uuid strmiids winmm)

# ---------------------------------------------------------------- CLI (x64 only: NVDEC)
option(CF_BUILD_CLI "Build camerafy.exe" ON)
if(CF_X64 AND CF_BUILD_CLI)
  add_executable(camerafy
    src/cli/main.cpp
    src/cli/options.cpp
    src/cli/console.cpp
    src/cli/env_check.cpp
    src/cli/net_guard.cpp
    src/cli/plist.cpp
    src/cli/usbmux.cpp
    src/cli/lockdown.cpp
    src/cli/arena.cpp
    src/cli/assembler.cpp
    src/cli/nvdec_api.cpp
    src/cli/decoder.cpp
    src/cli/publisher.cpp
    src/cli/pipeline.cpp
    src/cli/sideband.cpp
    src/cli/clock_sync.cpp
    src/cli/telemetry.cpp
    src/cli/tuning.cpp
    src/cli/vcam_mf.cpp
    src/cli/vcam_ds.cpp
    src/cli/preview.cpp
    src/cli/selftest.cpp
    src/cli/bench.cpp)
  target_include_directories(camerafy PRIVATE ${CF_COMMON} src/cli ${CMAKE_SOURCE_DIR}/third_party/nvcodec
                             ${CMAKE_CURRENT_BINARY_DIR}/shaders)

  # Preview shaders are compiled at build time; nothing is compiled at runtime.
  set(CF_KITS "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin")
  find_program(FXC fxc REQUIRED HINTS
    "${CF_KITS}/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
    "${CF_KITS}/10.0.26100.0/x64" "${CF_KITS}/10.0.22621.0/x64")
  set(CF_SHADER_SRC ${CMAKE_SOURCE_DIR}/src/cli/preview.hlsl)
  set(CF_SHADER_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
  add_custom_command(
    OUTPUT ${CF_SHADER_DIR}/preview_vs.h ${CF_SHADER_DIR}/preview_ps.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CF_SHADER_DIR}
    COMMAND ${FXC} /nologo /T vs_5_0 /E vsMain /O3 /Vn g_vsMain /Fh ${CF_SHADER_DIR}/preview_vs.h ${CF_SHADER_SRC}
    COMMAND ${FXC} /nologo /T ps_5_0 /E psMain /O3 /Vn g_psMain /Fh ${CF_SHADER_DIR}/preview_ps.h ${CF_SHADER_SRC}
    DEPENDS ${CF_SHADER_SRC}
    COMMENT "Compiling preview shaders")
  target_sources(camerafy PRIVATE ${CF_SHADER_DIR}/preview_vs.h ${CF_SHADER_DIR}/preview_ps.h)
  target_link_libraries(camerafy PRIVATE
    ws2_32 iphlpapi avrt mfplat mf mfreadwrite mfuuid mfsensorgroup ole32 oleaut32 uuid advapi32 strmiids
    d3d11 dxgi d3dcompiler onecore synchronization powrprof setupapi)
endif()

# ---------------------------------------------------------------- contention test (x64: CUDA loads)
# Loads CPU / DRAM / GPU / VRAM / PCIe in phases while consuming the camera like an app, to show
# the camera keeps its latency under load (README: "Contention test").
if(CF_X64)
  add_executable(cf_contention src/contention/contention.cpp src/cli/nvdec_api.cpp)
  target_include_directories(cf_contention PRIVATE ${CF_COMMON} src/cli ${CMAKE_SOURCE_DIR}/third_party/nvcodec)
  target_link_libraries(cf_contention PRIVATE synchronization)
endif()

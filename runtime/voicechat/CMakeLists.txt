# MKW VoiceChat source integration stage 2.
#
# The installer provisions pinned Opus, SpeexDSP and miniaudio source trees
# under MKWVC_DEP_ROOT. WiiCompiled's shipped LLVM-MinGW toolchain then builds
# the existing MKW-VoiceChat audio/core sources locally. libdatachannel/ICE is
# intentionally still the next stage.

if(NOT MKW_PLATFORM_WINDOWS)
    return()
endif()

set(MKWVC_DEP_ROOT "$ENV{MKWVC_DEP_ROOT}")
if(NOT MKWVC_DEP_ROOT)
    message(FATAL_ERROR "MKWVC_DEP_ROOT is not set; run the WiiCompiled Voicechat installer")
endif()

set(MKWVC_OPUS_DIR "${MKWVC_DEP_ROOT}/opus")
set(MKWVC_SPEEXDSP_DIR "${MKWVC_DEP_ROOT}/speexdsp")
set(MKWVC_MINIAUDIO_DIR "${MKWVC_DEP_ROOT}/miniaudio")

foreach(required
    "${MKWVC_OPUS_DIR}/CMakeLists.txt"
    "${MKWVC_SPEEXDSP_DIR}/libspeexdsp/preprocess.c"
    "${MKWVC_MINIAUDIO_DIR}/miniaudio.h")
    if(NOT EXISTS "${required}")
        message(FATAL_ERROR "MKW VoiceChat dependency source is missing: ${required}")
    endif()
endforeach()

# Opus 1.6.1: use its own CMake target, but keep the dependency local/static and
# remove optional programs/tests/install metadata from the game build.
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(
    "${MKWVC_OPUS_DIR}"
    "${CMAKE_BINARY_DIR}/mkwvc-opus"
    EXCLUDE_FROM_ALL)

# SpeexDSP 1.2.1: vcpkg uses a tiny CMake wrapper for this release. Keep the
# same floating-point source set directly here so users do not need vcpkg.
add_library(mkwvc_speexdsp STATIC
    "${MKWVC_SPEEXDSP_DIR}/libspeexdsp/buffer.c"
    "${MKWVC_SPEEXDSP_DIR}/libspeexdsp/fftwrap.c"
    "${MKWVC_SPEEXDSP_DIR}/libspeexdsp/filterbank.c"
    "${MKWVC_SPEEXDSP_DIR}/libspeexdsp/jitter.c"
    "${MKWVC_SPEEXDSP_DIR}/libspeexdsp/mdf.c"
    "${MKWVC_SPEEXDSP_DIR}/libspeexdsp/preprocess.c"
    "${MKWVC_SPEEXDSP_DIR}/libspeexdsp/resample.c"
    "${MKWVC_SPEEXDSP_DIR}/libspeexdsp/scal.c"
    "${MKWVC_SPEEXDSP_DIR}/libspeexdsp/smallft.c")
target_include_directories(mkwvc_speexdsp
    PUBLIC "${MKWVC_SPEEXDSP_DIR}/include"
    PRIVATE "${MKWVC_SPEEXDSP_DIR}/libspeexdsp")
target_compile_definitions(mkwvc_speexdsp PRIVATE
    FLOATING_POINT
    USE_SMALLFT
    VAR_ARRAYS
    EXPORT=)
target_compile_options(mkwvc_speexdsp PRIVATE -O2 -w -pipe)
set_target_properties(mkwvc_speexdsp PROPERTIES
    UNITY_BUILD OFF
    C_EXTENSIONS ON)

# Existing standalone implementation sources. Nothing is reimplemented here.
add_library(mkw_voicechat_core_stage2 STATIC
    src/AdaptiveJitterBuffer.cpp
    src/AudioEngine.cpp
    src/AudioProcessor.cpp
    src/EmbeddedCore.cpp
    src/NetworkSimulator.cpp
    src/OpusCodec.cpp
    src/UdpVoiceTransport.cpp
    src/VoiceClient.cpp)

target_include_directories(mkw_voicechat_core_stage2
    PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include"
    PRIVATE "${MKWVC_MINIAUDIO_DIR}")
target_compile_features(mkw_voicechat_core_stage2 PRIVATE cxx_std_23)
target_compile_definitions(mkw_voicechat_core_stage2 PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(mkw_voicechat_core_stage2 PRIVATE -O2 -w -pipe)
target_link_libraries(mkw_voicechat_core_stage2 PUBLIC
    Opus::opus
    mkwvc_speexdsp
    ole32
    winmm)
set_target_properties(mkw_voicechat_core_stage2 PROPERTIES
    UNITY_BUILD OFF
    CXX_EXTENSIONS OFF)

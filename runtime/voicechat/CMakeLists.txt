# MKW VoiceChat source integration stage 1.
#
# VoiceClient.cpp is compiled with WiiCompiled's shipped LLVM-MinGW toolchain
# as a hard source-compatibility gate. The archive is linked into RetroRewind
# through EmbeddedCore.cpp, while VoiceClient itself is not instantiated until
# Opus, audio processing and libdatachannel are linked in the next stage.

if(NOT MKW_PLATFORM_WINDOWS)
    return()
endif()

add_library(mkw_voicechat_core_stage1 STATIC
    src/EmbeddedCore.cpp
    src/VoiceClient.cpp)

target_include_directories(mkw_voicechat_core_stage1
    PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
target_compile_features(mkw_voicechat_core_stage1 PRIVATE cxx_std_23)
target_compile_definitions(mkw_voicechat_core_stage1 PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(mkw_voicechat_core_stage1 PRIVATE -O2 -w -pipe)
set_target_properties(mkw_voicechat_core_stage1 PROPERTIES
    UNITY_BUILD OFF
    CXX_EXTENSIONS OFF)

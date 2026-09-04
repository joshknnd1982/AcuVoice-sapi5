cmake_minimum_required(VERSION 3.15)
project(AcuVoiceSAPI VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(BUILD_X64 TRUE)
    message(STATUS "Building the x64 side (SAPI engine + diagnostics; the engine itself runs in the 32-bit worker)")
else()
    set(BUILD_X64 FALSE)
    message(STATUS "Building the x86 side (SAPI engine, worker, configuration utility, diagnostics)")
endif()

set(COMMON_DEFS _CRT_SECURE_NO_WARNINGS NOMINMAX UNICODE _UNICODE)

# ---------------------------------------------------------------------------------
# The engine layer. avcore.dll is 32-bit, so only the x86 side ever links it in; the
# x64 side gets the same audio through the worker.
# ---------------------------------------------------------------------------------
add_library(av_dsp STATIC src/av_dsp.cpp)
target_include_directories(av_dsp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(av_dsp PRIVATE ${COMMON_DEFS})

add_library(av_text STATIC src/text_prep.cpp)
target_include_directories(av_text PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(av_text PRIVATE ${COMMON_DEFS})

# Built for both architectures. The 64-bit build cannot load avcore.dll -- there is no
# 64-bit build of it -- but the ini reader beside it is architecture independent and the
# 64-bit diagnostics needs it.
add_library(av_engine STATIC src/av_engine.cpp)
target_include_directories(av_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(av_engine PRIVATE ${COMMON_DEFS})

add_library(pipe_client STATIC src/pipe_client.cpp)
target_include_directories(pipe_client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pipe_client PUBLIC shlwapi)
target_compile_definitions(pipe_client PRIVATE ${COMMON_DEFS})

# ---------------------------------------------------------------------------------
# The SAPI5 engine
# ---------------------------------------------------------------------------------
set(SAPI_SOURCES
    src/sapi_main.cpp
    src/com.cpp
    src/registry.cpp
    src/ISpDataKeyImpl.cpp
    src/IEnumSpObjectTokensImpl.cpp
    src/ISpTTSEngineImpl.cpp
    src/voice_token.cpp
    src/acuvoice_sapi.def
)

add_library(AcuVoiceSAPI SHARED ${SAPI_SOURCES})
target_include_directories(AcuVoiceSAPI PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(AcuVoiceSAPI PRIVATE ${COMMON_DEFS})
target_link_libraries(AcuVoiceSAPI PRIVATE av_dsp av_text pipe_client ole32 oleaut32 advapi32 shell32)

if(BUILD_X64)
    target_compile_definitions(AcuVoiceSAPI PRIVATE BUILD_X64)
else()
    target_link_libraries(AcuVoiceSAPI PRIVATE av_engine)
endif()

# ---------------------------------------------------------------------------------
# The 32-bit worker, the configuration utility and the diagnostics tool
# ---------------------------------------------------------------------------------
if(NOT BUILD_X64)
    add_executable(AcuVoiceServer src/av_server.cpp)
    target_include_directories(AcuVoiceServer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(AcuVoiceServer PRIVATE ${COMMON_DEFS})
    target_link_libraries(AcuVoiceServer PRIVATE av_engine av_dsp shlwapi)
    set_target_properties(AcuVoiceServer PROPERTIES WIN32_EXECUTABLE FALSE)

    add_executable(AcuVoiceConfig WIN32
        tools/acuvoice_config.cpp
        tools/acuvoice_config.rc
    )
    target_include_directories(AcuVoiceConfig PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tools
    )
    target_compile_definitions(AcuVoiceConfig PRIVATE ${COMMON_DEFS})
    target_link_libraries(AcuVoiceConfig PRIVATE
        av_engine av_dsp av_text comctl32 winmm shlwapi advapi32 ole32 uuid)
endif()

add_executable(reg_test tools/reg_test.cpp src/registry.cpp)
target_include_directories(reg_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(reg_test PRIVATE ${COMMON_DEFS})
target_link_libraries(reg_test PRIVATE ole32 oleaut32 advapi32 sapi)

add_executable(AcuVoiceDiagnostics tools/acuvoice_diag.cpp)
target_include_directories(AcuVoiceDiagnostics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(AcuVoiceDiagnostics PRIVATE ${COMMON_DEFS})
target_link_libraries(AcuVoiceDiagnostics PRIVATE av_engine av_dsp av_text pipe_client ole32 oleaut32 advapi32 sapi winmm)
if(BUILD_X64)
    target_compile_definitions(AcuVoiceDiagnostics PRIVATE BUILD_X64)
endif()

if(MSVC)
    foreach(t av_engine av_dsp av_text pipe_client AcuVoiceSAPI AcuVoiceDiagnostics reg_test)
        target_compile_options(${t} PRIVATE /W3 /MP)
        set_property(TARGET ${t} PROPERTY MSVC_RUNTIME_LIBRARY
                     "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endforeach()
    if(NOT BUILD_X64)
        foreach(t AcuVoiceServer AcuVoiceConfig)
            target_compile_options(${t} PRIVATE /W3 /MP)
            set_property(TARGET ${t} PROPERTY MSVC_RUNTIME_LIBRARY
                         "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endforeach()
    endif()
endif()

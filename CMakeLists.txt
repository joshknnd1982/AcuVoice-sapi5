cmake_minimum_required(VERSION 3.15)
project(AcuVoiceSAPI VERSION 1.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# The product is 64-bit. avcore.dll is not, and never will be -- it is a 1999 binary with
# no source -- so the 32-bit side is reduced to what genuinely has to be 32-bit: the
# worker that holds the engine, and a SAPI dll for the 32-bit hosts (NVDA, JAWS, 32-bit
# Balabolka) that load an engine in process. Everything the user runs directly is 64-bit.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(BUILD_X64 TRUE)
    message(STATUS "x64: the SAPI engine, the configuration utility and the diagnostics")
else()
    set(BUILD_X64 FALSE)
    message(STATUS "x86: the engine worker, plus a SAPI engine and diagnostics for 32-bit hosts")
endif()

set(COMMON_DEFS _CRT_SECURE_NO_WARNINGS NOMINMAX UNICODE _UNICODE)

# ---------------------------------------------------------------------------------
# Shared pieces. av_engine is built for both: the 64-bit build cannot load avcore, but
# the ini reader beside it is architecture independent and both sides need it.
# ---------------------------------------------------------------------------------
add_library(av_dsp STATIC src/av_dsp.cpp)
target_include_directories(av_dsp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(av_dsp PRIVATE ${COMMON_DEFS})

add_library(av_text STATIC src/text_prep.cpp)
target_include_directories(av_text PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(av_text PRIVATE ${COMMON_DEFS})

add_library(av_engine STATIC src/av_engine.cpp)
target_include_directories(av_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(av_engine PRIVATE ${COMMON_DEFS})

add_library(pipe_client STATIC src/pipe_client.cpp)
target_include_directories(pipe_client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pipe_client PUBLIC shlwapi)
target_compile_definitions(pipe_client PRIVATE ${COMMON_DEFS})

set(SHARED_TARGETS av_dsp av_text av_engine pipe_client)

# ---------------------------------------------------------------------------------
# The SAPI5 engine -- built for both, because a host loads the dll of its own bitness
# ---------------------------------------------------------------------------------
add_library(AcuVoiceSAPI SHARED
    src/sapi_main.cpp
    src/com.cpp
    src/registry.cpp
    src/ISpDataKeyImpl.cpp
    src/IEnumSpObjectTokensImpl.cpp
    src/ISpTTSEngineImpl.cpp
    src/voice_token.cpp
    src/acuvoice_sapi.def
)
target_include_directories(AcuVoiceSAPI PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(AcuVoiceSAPI PRIVATE ${COMMON_DEFS})
target_link_libraries(AcuVoiceSAPI PRIVATE av_dsp av_text pipe_client ole32 oleaut32 advapi32 shell32)

if(BUILD_X64)
    target_compile_definitions(AcuVoiceSAPI PRIVATE BUILD_X64)
else()
    target_link_libraries(AcuVoiceSAPI PRIVATE av_engine)
endif()

# ---------------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------------
add_executable(AcuVoiceDiagnostics tools/acuvoice_diag.cpp)
target_include_directories(AcuVoiceDiagnostics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(AcuVoiceDiagnostics PRIVATE ${COMMON_DEFS})
target_link_libraries(AcuVoiceDiagnostics PRIVATE
    av_engine av_dsp av_text pipe_client ole32 oleaut32 advapi32 sapi winmm)

add_executable(reg_test tools/reg_test.cpp src/registry.cpp)
target_include_directories(reg_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(reg_test PRIVATE ${COMMON_DEFS})
target_link_libraries(reg_test PRIVATE ole32 oleaut32 advapi32 sapi)

set(ARCH_TARGETS AcuVoiceSAPI AcuVoiceDiagnostics reg_test)

if(BUILD_X64)
    target_compile_definitions(AcuVoiceDiagnostics PRIVATE BUILD_X64)

    # The configuration utility is 64-bit only. It reaches the engine through the worker
    # like any other 64-bit program, so it needs nothing from avcore directly.
    add_executable(AcuVoiceConfig WIN32
        tools/acuvoice_config.cpp
        tools/acuvoice_config.rc
    )
    target_include_directories(AcuVoiceConfig PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tools
    )
    target_compile_definitions(AcuVoiceConfig PRIVATE ${COMMON_DEFS} BUILD_X64)
    target_link_libraries(AcuVoiceConfig PRIVATE
        av_engine av_dsp av_text pipe_client comctl32 winmm shlwapi advapi32 ole32 uuid)

    # Reports what a screen reader sees in a Win32 dialog. Not installed; used to check
    # that every control in the configuration utility is named and reachable.
    add_executable(a11y_dump tools/a11y_dump.cpp)
    target_include_directories(a11y_dump PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(a11y_dump PRIVATE ${COMMON_DEFS})
    target_link_libraries(a11y_dump PRIVATE oleacc ole32 oleaut32 user32)

    list(APPEND ARCH_TARGETS AcuVoiceConfig a11y_dump)
else()
    # The only process in the product that can hold the engine.
    add_executable(AcuVoiceServer src/av_server.cpp)
    target_include_directories(AcuVoiceServer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(AcuVoiceServer PRIVATE ${COMMON_DEFS})
    target_link_libraries(AcuVoiceServer PRIVATE av_engine av_dsp shlwapi)
    set_target_properties(AcuVoiceServer PROPERTIES WIN32_EXECUTABLE FALSE)

    list(APPEND ARCH_TARGETS AcuVoiceServer)
endif()

if(MSVC)
    foreach(t ${SHARED_TARGETS} ${ARCH_TARGETS})
        target_compile_options(${t} PRIVATE /W3 /MP)
        set_property(TARGET ${t} PROPERTY MSVC_RUNTIME_LIBRARY
                     "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endforeach()
endif()

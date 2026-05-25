cmake_minimum_required(VERSION 3.20)

if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

project(DATPlayer
    VERSION 0.1.0
    DESCRIPTION "Portable Windows DAT player foundation"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_library(dat_indexer
    src/DatFrameIndexer.cpp
    src/RecordingMetadata.cpp)

target_include_directories(dat_indexer
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include)

function(dat_player_set_warnings target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

function(dat_player_optimize_release_size target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE
            $<$<CONFIG:Release>:/O1 /Ob1>)
        target_link_options(${target_name} PRIVATE
            $<$<CONFIG:Release>:/OPT:REF /OPT:ICF /INCREMENTAL:NO>)
    endif()
endfunction()

dat_player_set_warnings(dat_indexer)
dat_player_optimize_release_size(dat_indexer)

set(DAT_PLAYER_DOTNET_EXECUTABLE "" CACHE FILEPATH "Path to dotnet executable used to publish the SEF2 signature verifier.")
if(NOT DAT_PLAYER_DOTNET_EXECUTABLE)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.dotnet/dotnet.exe")
        set(DAT_PLAYER_DOTNET_EXECUTABLE "${CMAKE_CURRENT_SOURCE_DIR}/.dotnet/dotnet.exe")
    else()
        find_program(DAT_PLAYER_DOTNET_EXECUTABLE dotnet)
    endif()
endif()

set(DAT_PLAYER_SEF2_VERIFIER_TARGET "")
if(WIN32 AND DAT_PLAYER_DOTNET_EXECUTABLE)
    execute_process(
        COMMAND "${DAT_PLAYER_DOTNET_EXECUTABLE}" --list-sdks
        OUTPUT_VARIABLE DAT_PLAYER_DOTNET_SDKS
        ERROR_QUIET
        RESULT_VARIABLE DAT_PLAYER_DOTNET_SDK_RESULT)
    if(DAT_PLAYER_DOTNET_SDK_RESULT EQUAL 0 AND NOT DAT_PLAYER_DOTNET_SDKS STREQUAL "")
        set(DAT_PLAYER_SEF2_VERIFIER_TARGET sef2_signature_verifier)
        set(DAT_PLAYER_SEF2_VERIFIER_PROJECT "${CMAKE_CURRENT_SOURCE_DIR}/src/Sef2SignatureVerifier/Sef2SignatureVerifier.csproj")
        set(DAT_PLAYER_SEF2_VERIFIER_EXE "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/DatPlayer.Sef2SignatureVerifier.exe")
        add_custom_target(sef2_signature_verifier ALL
            COMMAND "${CMAKE_COMMAND}" -E env "LIB=" "${DAT_PLAYER_DOTNET_EXECUTABLE}" publish "${DAT_PLAYER_SEF2_VERIFIER_PROJECT}"
                -c Release
                -r win-x64
                --self-contained true
                -o "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>"
                /p:PublishSingleFile=true
                /p:PublishTrimmed=false
                /p:DebugType=none
                /p:DebugSymbols=false
            BYPRODUCTS "${DAT_PLAYER_SEF2_VERIFIER_EXE}"
            COMMENT "Publishing SEF2 XML signature verifier")
    else()
        message(WARNING "A dotnet executable was found, but no SDK is available. SEF2 signature verification helper will not be built.")
    endif()
endif()

add_executable(dat_indexer_tests
    tests/DatFrameIndexerTests.cpp)

target_link_libraries(dat_indexer_tests PRIVATE dat_indexer)
target_compile_definitions(dat_indexer_tests
    PRIVATE
        DAT_PLAYER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
dat_player_set_warnings(dat_indexer_tests)
dat_player_optimize_release_size(dat_indexer_tests)
if(DAT_PLAYER_SEF2_VERIFIER_TARGET)
    add_dependencies(dat_indexer_tests ${DAT_PLAYER_SEF2_VERIFIER_TARGET})
endif()

if(WIN32)
    add_library(dat_playback
        src/playback/H264Decoder.cpp)

    target_include_directories(dat_playback
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/src)

    target_link_libraries(dat_playback
        PUBLIC
            dat_indexer
        PRIVATE
            mf
            mfplat
            mfuuid
            ole32)

    target_compile_definitions(dat_playback
        PUBLIC
            UNICODE
            _UNICODE
            WIN32_LEAN_AND_MEAN
            NOMINMAX)

    dat_player_set_warnings(dat_playback)
    dat_player_optimize_release_size(dat_playback)

    add_executable(dat_player WIN32
        src/player/main.cpp
        src/player/DatPlayer.rc)

    set_target_properties(dat_player
        PROPERTIES
            OUTPUT_NAME "DatPlayer")

    target_include_directories(dat_player
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src)

    target_link_libraries(dat_player
        PRIVATE
            dat_indexer
            dat_playback
            comctl32
            comdlg32
            gdiplus
            shell32)

    target_compile_definitions(dat_player
        PRIVATE
            UNICODE
            _UNICODE
            WIN32_LEAN_AND_MEAN
            NOMINMAX)

    dat_player_set_warnings(dat_player)
    dat_player_optimize_release_size(dat_player)
    if(DAT_PLAYER_SEF2_VERIFIER_TARGET)
        add_dependencies(dat_player ${DAT_PLAYER_SEF2_VERIFIER_TARGET})
    endif()

    add_executable(dat_decode_test
        src/playback/DecodeTest.cpp)

    target_link_libraries(dat_decode_test
        PRIVATE
            dat_indexer
            dat_playback)

    dat_player_set_warnings(dat_decode_test)
    dat_player_optimize_release_size(dat_decode_test)
endif()

enable_testing()
add_test(NAME dat_indexer_tests COMMAND dat_indexer_tests)
if(DAT_PLAYER_SEF2_VERIFIER_TARGET)
    set_tests_properties(dat_indexer_tests PROPERTIES
        ENVIRONMENT "DAT_PLAYER_SEF2_VERIFIER=${DAT_PLAYER_SEF2_VERIFIER_EXE}")
endif()

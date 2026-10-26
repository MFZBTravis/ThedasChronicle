cmake_minimum_required(VERSION 3.20)
project(ThedasChronicleHook VERSION 1.2.0 LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Dragon Age: Inquisition is 64-bit; configure with -A x64.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT so the .asi has no runtime DLL dependencies.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        v1.3.4
)
FetchContent_MakeAvailable(minhook)

FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(json)

add_library(ThedasChronicleHook SHARED
    src/dllmain.cpp
    src/log.cpp
    src/game_hook.cpp
    src/hooks_worldstate.cpp
    src/hooks_vault.cpp
    src/hooks_nug.cpp
    src/hooks_offline.cpp
    src/keep_answers.cpp
    src/settings.cpp
    src/worldstate_file.cpp
)

target_link_libraries(ThedasChronicleHook PRIVATE minhook nlohmann_json::nlohmann_json ws2_32)
target_compile_definitions(ThedasChronicleHook PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    THEDAS_VERSION="${PROJECT_VERSION}")
target_compile_options(ThedasChronicleHook PRIVATE /W4 /permissive-)

# Ultimate ASI Loader loads *.asi files, which are ordinary DLLs.
set_target_properties(ThedasChronicleHook PROPERTIES SUFFIX ".asi")

# Optional: cmake -DDAI_DIR="C:/Program Files/EA Games/Dragon Age Inquisition" ...
# copies the built plugin into <game>/scripts after each build.
set(DAI_DIR "" CACHE PATH "Dragon Age: Inquisition install folder")
if(DAI_DIR)
    add_custom_command(TARGET ThedasChronicleHook POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DAI_DIR}/scripts"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ThedasChronicleHook> "${DAI_DIR}/scripts/"
        COMMENT "Deploying to ${DAI_DIR}/scripts")
endif()

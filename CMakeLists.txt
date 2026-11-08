cmake_minimum_required(VERSION 3.21)
project(shell LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The exe builds straight into the plugin folder, because plugin.json names
# it as runtime.program. Keep the ra9fael.binarylab convention.
add_executable(shell main.cpp)
target_compile_definitions(shell PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
# Clipboard reads/writes live in user32; ShellExecuteExW (admin runs) in
# shell32; processes and files in kernel32.
target_link_libraries(shell PRIVATE user32 shell32)

if(MSVC)
    target_compile_options(shell PRIVATE /utf-8 /W4 /permissive-)
    # Static CRT: no runtime DLL to load from the plugin folder.
    set_target_properties(shell PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_compile_options(shell PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8 -Wall -Wextra -O2)
    target_link_options(shell PRIVATE -static -static-libgcc -static-libstdc++)
endif()

set_target_properties(shell PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}")
foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(shell PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${config} "${CMAKE_SOURCE_DIR}")
endforeach()

# Pure-logic tests: logic.h has no process or file dependency, so they build
# and run on any C++17 compiler without starting a plugin process.
add_executable(shell_tests tests/test_logic.cpp)
target_include_directories(shell_tests PRIVATE "${CMAKE_SOURCE_DIR}")
if(MSVC)
    target_compile_options(shell_tests PRIVATE /utf-8 /W4 /permissive-)
else()
    target_compile_options(shell_tests PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8 -Wall -Wextra -O2)
endif()
set_target_properties(shell_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(shell_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${config} "${CMAKE_BINARY_DIR}/tests")
endforeach()

enable_testing()
add_test(NAME shell_logic COMMAND shell_tests)

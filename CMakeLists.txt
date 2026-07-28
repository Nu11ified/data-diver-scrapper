cmake_minimum_required(VERSION 3.20)
project(data_diver LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

add_library(dd_engine STATIC
  src/core.cpp
  src/json.cpp
  src/html.cpp
  src/csv.cpp
)

target_include_directories(dd_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(dd_engine PRIVATE -Wall -Wextra -Wpedantic -Wshadow)

find_package(Threads REQUIRED)
target_link_libraries(dd_engine PUBLIC Threads::Threads)

enable_testing()
add_executable(dd_tests tests/tests.cpp)
target_link_libraries(dd_tests PRIVATE dd_engine)
target_compile_options(dd_tests PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME engine COMMAND dd_tests WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

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
  src/pdf.cpp
  src/document.cpp
  src/fetch.cpp
  src/metrics.cpp
  src/features.cpp
  src/model.cpp
  src/classify.cpp
  src/schema.cpp
  src/entity.cpp
  src/events.cpp
)

target_include_directories(dd_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(dd_engine PRIVATE -Wall -Wextra -Wpedantic -Wshadow)

find_package(Threads REQUIRED)
target_link_libraries(dd_engine PUBLIC Threads::Threads)

# libcurl gives us http(s). Without it the engine still runs against file://
# and local fixtures, and reports the missing transport instead of pretending.
find_package(CURL)
if(CURL_FOUND)
  target_link_libraries(dd_engine PUBLIC CURL::libcurl)
  target_compile_definitions(dd_engine PUBLIC DD_HAVE_CURL=1)
else()
  message(WARNING "libcurl not found: http(s) sources will be rejected at fetch time")
endif()

# zlib lets the PDF reader inflate FlateDecode content streams.
find_package(ZLIB)
if(ZLIB_FOUND)
  target_link_libraries(dd_engine PUBLIC ZLIB::ZLIB)
  target_compile_definitions(dd_engine PUBLIC DD_HAVE_ZLIB=1)
else()
  message(WARNING "zlib not found: compressed PDF streams will be skipped")
endif()

add_executable(dd_train src/train_main.cpp)
target_link_libraries(dd_train PRIVATE dd_engine)
target_compile_options(dd_train PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()
add_executable(dd_tests tests/tests.cpp)
target_link_libraries(dd_tests PRIVATE dd_engine)
target_compile_options(dd_tests PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME engine COMMAND dd_tests WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

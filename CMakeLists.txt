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
  src/core/core.cpp
  src/core/json.cpp
  src/core/metrics.cpp
  src/parse/html.cpp
  src/parse/query.cpp
  src/parse/csv.cpp
  src/parse/pdf.cpp
  src/parse/document.cpp
  src/net/fetch.cpp
  src/ml/features.cpp
  src/ml/model.cpp
  src/ml/classify.cpp
  src/ml/columns.cpp
  src/engine/schema.cpp
  src/engine/bench.cpp
  src/engine/compile.cpp
  src/engine/entity.cpp
  src/engine/events.cpp
  src/engine/exporter.cpp
  src/engine/store.cpp
  src/engine/harvest.cpp
  src/engine/heal.cpp
  src/engine/pipeline.cpp
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

add_executable(datadiver src/app/main.cpp src/app/commands.cpp src/app/render.cpp)
target_link_libraries(datadiver PRIVATE dd_engine)
target_compile_options(datadiver PRIVATE -Wall -Wextra -Wpedantic)

# The WASM boundary for the Cloudflare Worker: the compute half of the engine
# (parse, ml, schema, events, compile) with the host owning network and
# storage. Build with: emcmake cmake -S . -B build-wasm && cmake --build build-wasm
if(EMSCRIPTEN)
  add_executable(datadiver_wasm
    wasm/api.cpp
    src/core/core.cpp
    src/core/json.cpp
    src/parse/html.cpp
    src/parse/query.cpp
    src/parse/csv.cpp
    src/parse/pdf.cpp
    src/parse/document.cpp
    src/ml/features.cpp
    src/ml/model.cpp
    src/ml/classify.cpp
    src/ml/columns.cpp
    src/engine/schema.cpp
    src/engine/entity.cpp
    src/engine/events.cpp
    src/engine/compile.cpp
  )
  target_include_directories(datadiver_wasm PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_options(datadiver_wasm PRIVATE -Wall -Wextra -Wpedantic -O2)
  target_link_options(datadiver_wasm PRIVATE
    -sMODULARIZE=1
    -sEXPORT_ES6=1
    -sENVIRONMENT=web,worker,node
    -sALLOW_MEMORY_GROWTH=1
    -sFILESYSTEM=0
    -sEXPORTED_FUNCTIONS=_dd_version,_dd_process_document,_dd_compile_county,_dd_free,_malloc,_free
    -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,stringToNewUTF8,UTF8ToString
  )
  set_target_properties(datadiver_wasm PROPERTIES OUTPUT_NAME "datadiver" SUFFIX ".mjs")
endif()

enable_testing()
add_executable(dd_tests tests/tests.cpp)
target_link_libraries(dd_tests PRIVATE dd_engine)
target_compile_options(dd_tests PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME engine COMMAND dd_tests WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

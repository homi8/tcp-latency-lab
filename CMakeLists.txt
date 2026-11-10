cmake_minimum_required(VERSION 3.16)
project(tcp-latency-lab CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra)
# Profilers need frame pointers + debug info to give clean stacks; this costs
# ~nothing on this code and makes docs/PERF.md work out of the box.
add_compile_options(-g -fno-omit-frame-pointer)

if(APPLE)
  list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew" "/usr/local")
endif()

# ---- Raw TCP pair: zero dependencies ----
add_executable(mock_exchange src/mock_exchange.cpp)
target_include_directories(mock_exchange PRIVATE src)
add_executable(hft_client src/hft_client.cpp)
target_include_directories(hft_client PRIVATE src)

# ---- gRPC pair: optional, skipped cleanly when deps are absent ----
find_package(Protobuf CONFIG QUIET)
find_package(gRPC CONFIG QUIET)
if(Protobuf_FOUND AND gRPC_FOUND)
  add_library(exchpb proto/exchange.proto)
  target_link_libraries(exchpb PUBLIC protobuf::libprotobuf gRPC::grpc gRPC::grpc++)
  # Generated files land in build/proto/ with new protoc (source-relative
  # layout) or build/ with old protoc (flat); both dirs keep the grpc
  # #includes working across versions.
  target_include_directories(exchpb PUBLIC ${CMAKE_CURRENT_BINARY_DIR}
                                           ${CMAKE_CURRENT_BINARY_DIR}/proto)
  protobuf_generate(TARGET exchpb LANGUAGE cpp)
  protobuf_generate(TARGET exchpb LANGUAGE grpc
    GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
    PLUGIN "protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>")
  add_executable(grpc_exchange_server src/grpc_exchange_server.cpp)
  target_link_libraries(grpc_exchange_server PRIVATE exchpb)
  target_include_directories(grpc_exchange_server PRIVATE src)
  add_executable(grpc_hft_client src/grpc_hft_client.cpp)
  target_link_libraries(grpc_hft_client PRIVATE exchpb)
  target_include_directories(grpc_hft_client PRIVATE src)
  set(HAVE_GRPC ON)
else()
  message(STATUS "gRPC/Protobuf not found - building TCP-only. "
          "Install grpc + protobuf for the comparison (see README).")
endif()

enable_testing()
add_test(NAME smoke COMMAND bash ${CMAKE_SOURCE_DIR}/scripts/smoke.sh ${CMAKE_BINARY_DIR})

# Generates C++ sources from the .proto files with protoc + grpc_cpp_plugin.
# Uses pkg-config so it works with the Ubuntu packages (libgrpc++-dev, libprotobuf-dev).
find_package(PkgConfig REQUIRED)
pkg_check_modules(GRPCPP REQUIRED IMPORTED_TARGET grpc++)
pkg_check_modules(PROTOBUF REQUIRED IMPORTED_TARGET protobuf)
find_program(LSMDB_PROTOC protoc REQUIRED)
find_program(LSMDB_GRPC_PLUGIN grpc_cpp_plugin REQUIRED)

set(LSMDB_PROTO_NAMES common kv node command)
set(LSMDB_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${LSMDB_GEN_DIR})

set(LSMDB_PROTO_SRCS)
foreach(name ${LSMDB_PROTO_NAMES})
  set(proto_file ${CMAKE_CURRENT_SOURCE_DIR}/lsmdb/v1/${name}.proto)
  set(out_base ${LSMDB_GEN_DIR}/lsmdb/v1/${name})
  add_custom_command(
    OUTPUT ${out_base}.pb.cc ${out_base}.pb.h ${out_base}.grpc.pb.cc ${out_base}.grpc.pb.h
    COMMAND ${LSMDB_PROTOC}
            -I ${CMAKE_CURRENT_SOURCE_DIR}
            --cpp_out=${LSMDB_GEN_DIR}
            --grpc_out=${LSMDB_GEN_DIR}
            --plugin=protoc-gen-grpc=${LSMDB_GRPC_PLUGIN}
            ${proto_file}
    DEPENDS ${proto_file} ${CMAKE_CURRENT_SOURCE_DIR}/lsmdb/v1/common.proto
    COMMENT "protoc ${name}.proto")
  list(APPEND LSMDB_PROTO_SRCS ${out_base}.pb.cc ${out_base}.grpc.pb.cc)
endforeach()

add_library(lsmdb_proto STATIC ${LSMDB_PROTO_SRCS})
target_include_directories(lsmdb_proto PUBLIC ${LSMDB_GEN_DIR})
target_link_libraries(lsmdb_proto PUBLIC PkgConfig::GRPCPP PkgConfig::PROTOBUF)
# Generated code triggers warnings we do not own.
target_compile_options(lsmdb_proto PRIVATE -w)

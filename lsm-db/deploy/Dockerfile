FROM ubuntu:24.04 AS build
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake pkg-config libgrpc++-dev libprotobuf-dev protobuf-compiler \
    protobuf-compiler-grpc libgtest-dev && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . /src
RUN cmake -S . -B /build -DLSMDB_BUILD_SUHANI_RPC=ON -DLSMDB_BUILD_SUHANI_TESTS=ON \
    -DLSMDB_USE_ATISHAY_SKIPLIST=ON -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /build -j2 \
    && ctest --test-dir /build --output-on-failure

FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends libgrpc++-dev libprotobuf-dev \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 --create-home lsmdb
COPY --from=build /build/lsm-db/rpc/lsmdb_node /app/lsmdb_node
COPY --from=build /build/lsm-db/rpc/kv_cli /app/kv_cli
USER 10001:10001
EXPOSE 50051
ENTRYPOINT ["/app/lsmdb_node"]

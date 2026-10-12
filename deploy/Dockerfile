# Multi-stage Dockerfile for the real lsmdb_node binary
FROM ubuntu:24.04 AS build
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    pkg-config \
    libgrpc++-dev \
    libprotobuf-dev \
    protobuf-compiler-grpc \
    libgtest-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S lsm-db-full-weeks1-6/lsm-db -B build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build --target lsmdb_node -j4

FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    libgrpc++1.51t64 \
    libprotobuf32t64 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Real node runs as non-root user (uid 10001) matching fsGroup in Helm
RUN groupadd -g 10001 lsmdb && useradd -u 10001 -g lsmdb -m lsmdb

WORKDIR /app
COPY --from=build /src/build/rpc/lsmdb_node /app/lsmdb_node
RUN ln -sf /app/lsmdb_node /app/node

USER lsmdb
EXPOSE 50051

ENTRYPOINT ["/app/lsmdb_node"]

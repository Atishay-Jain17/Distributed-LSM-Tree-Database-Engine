# --- Build stage ---
FROM ubuntu:22.04 AS build

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt .
COPY src ./src

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build --config Release

# --- Runtime stage ---
FROM ubuntu:22.04

WORKDIR /app
COPY --from=build /src/build/node /app/node

EXPOSE 50051
ENTRYPOINT ["/app/node"]

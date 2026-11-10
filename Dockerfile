# Linux build + bench environment. This is also the box to run `perf` on
# (see docs/PERF.md) since macOS has no perf.
#
#   docker build -t latencylab .
#   docker run --rm -v "$PWD/results:/work/results" latencylab
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake python3 \
      protobuf-compiler protobuf-compiler-grpc libprotobuf-dev \
      libgrpc++-dev libabsl-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /work
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"$(nproc)"
CMD ["bash", "scripts/bench.sh", "build", "results"]

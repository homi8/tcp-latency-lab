# TCP-only build: zero dependencies beyond a C++17 compiler.
# The gRPC pair needs CMake + protobuf + grpc (see README).
CXX ?= c++
CXXFLAGS ?= -O3 -std=c++17 -Wall -Wextra -g -fno-omit-frame-pointer
BUILD := build/make

ALL := $(BUILD)/mock_exchange $(BUILD)/hft_client

all: $(ALL)

$(BUILD)/mock_exchange: src/mock_exchange.cpp src/wire.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -Isrc src/mock_exchange.cpp -o $@

$(BUILD)/hft_client: src/hft_client.cpp src/wire.h src/latency.h src/sys.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -Isrc src/hft_client.cpp -o $@

smoke: all
	bash scripts/smoke.sh $(BUILD)

bench: all
	bash scripts/bench.sh $(BUILD) results

clean:
	rm -rf build

.PHONY: all smoke bench clean

# Convenience build entrypoint for the full regfw SDK (Linux/macOS).
# All actual compilation and test registration lives in CMake.
BUILD ?= build-cmake
JOBS ?= 2
CMAKE ?= cmake
CTEST ?= ctest
CMAKE_BUILD_TYPE ?= Release
CMAKE_FLAGS ?=

.PHONY: all configure lib build test run-tests bench clean
all: build
configure:
	$(CMAKE) -S . -B $(BUILD) -DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE) -DREGFW_BUILD_TESTS=ON $(CMAKE_FLAGS)
lib: configure
	$(CMAKE) --build $(BUILD) --target regfw --parallel $(JOBS)
build: configure
	$(CMAKE) --build $(BUILD) --parallel $(JOBS)
run-tests: build
	$(CTEST) --test-dir $(BUILD) --output-on-failure
test: run-tests
bench:
	$(CMAKE) -S . -B $(BUILD) -DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE) -DREGFW_BUILD_BENCHMARKS=ON $(CMAKE_FLAGS)
	$(CMAKE) --build $(BUILD) --target bench_init_formula bench_mmio_dynamic bench_mmio_formula bench_scale_formula --parallel $(JOBS)
	REGFW_BENCH_BUILD=$(BUILD) python3 benchmarks/measure_formula.py
clean:
	$(CMAKE) -E rm -rf $(BUILD)

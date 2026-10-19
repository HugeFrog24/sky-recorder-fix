CMAKE ?= cmake
.DEFAULT_GOAL := build

.PHONY: build lint
build: BUILD_DIR ?= build/release
build:
	$(CMAKE) -S . -B "$(BUILD_DIR)" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSKY_RECORDER_TRACE=OFF -DSKY_RECORDER_LINT=OFF
	$(CMAKE) --build "$(BUILD_DIR)" --target sky_recorder_fix

lint: BUILD_DIR ?= build/lint
lint:
	$(CMAKE) -S . -B "$(BUILD_DIR)" -G Ninja -DSKY_RECORDER_LINT=ON
	$(CMAKE) --build "$(BUILD_DIR)" --target lint

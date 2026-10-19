CMAKE ?= cmake
BUILD_DIR ?= build/lint

.PHONY: lint
lint:
	$(CMAKE) -S . -B "$(BUILD_DIR)" -G Ninja -DSKY_RECORDER_LINT=ON
	$(CMAKE) --build "$(BUILD_DIR)" --target lint

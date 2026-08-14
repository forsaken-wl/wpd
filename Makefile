BUILD_DIR ?= build
MESON ?= meson

.PHONY: all configure install clean test

all: configure
	$(MESON) compile -C $(BUILD_DIR)

configure:
	$(MESON) setup $(BUILD_DIR) --reconfigure

install: all
	$(MESON) install -C $(BUILD_DIR)

clean:
	$(MESON) compile -C $(BUILD_DIR) --clean

test: all
	$(BUILD_DIR)/wpd --version
	$(BUILD_DIR)/wpd --help >/dev/null

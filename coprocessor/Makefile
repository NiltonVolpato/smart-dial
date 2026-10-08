.PHONY: all build clean fmt fmt-check tidy test _check_managed_components
.DEFAULT_GOAL := all

_check_managed_components:
	@# PlatformIO only fetches ESP-IDF managed components during a fresh CMake configure.
	@# If the source directory is missing, force a reconfigure so the component manager runs.
	@if [ ! -d managed_components ] && [ -d .pio/build/esp32dev ]; then \
		echo "managed_components/ missing; forcing PlatformIO reconfigure"; \
		rm -rf .pio/build/esp32dev; \
	fi

LLVM_BIN := /opt/homebrew/opt/llvm@22/bin
SYSROOT := /Users/nilton/.platformio/packages/toolchain-xtensa-esp-elf/xtensa-esp-elf

CACHE_DIR := .cache/coprocessor
STAMP_DIR := $(CACHE_DIR)/stamps
QEMU_BUILD_DIR := $(CACHE_DIR)/qemu

ELF := .pio/build/esp32dev/firmware.elf
COMPILE_DB := compile_commands.json
FILELIST_SHA_FILE := $(STAMP_DIR)/.filelist.sha256
TIDY_STAMP := $(STAMP_DIR)/.tidy.stamp
FMT_STAMP := $(STAMP_DIR)/.fmt.stamp
FMT_CHECK_STAMP := $(STAMP_DIR)/.fmt_check.stamp

# Check if any files were added, removed, or renamed recursively in src/, include/, or managed_components/
CURRENT_FILELIST_SHA := $(shell find src include managed_components -type f 2>/dev/null | sort | sha256sum)
SAVED_FILELIST_SHA := $(shell cat $(FILELIST_SHA_FILE) 2>/dev/null)

ifneq ($(CURRENT_FILELIST_SHA),$(SAVED_FILELIST_SHA))
.PHONY: $(FILELIST_SHA_FILE)
endif

$(FILELIST_SHA_FILE):
	@mkdir -p $(dir $@)
	@echo "$(CURRENT_FILELIST_SHA)" > $@

COMPILE_DB_DEPS := platformio.ini CMakeLists.txt sdkconfig.defaults \
                   src/CMakeLists.txt src/idf_component.yml $(FILELIST_SHA_FILE)

$(COMPILE_DB): $(COMPILE_DB_DEPS)
	pio run -t compiledb
	@touch $(COMPILE_DB)

FMT_SRCS := $(wildcard src/*.cpp) $(wildcard src/*.h)
SRCS := $(FMT_SRCS) $(wildcard src/CMakeLists.txt) $(wildcard src/idf_component.yml) $(wildcard src/web/*) \
        $(wildcard include/*.h) platformio.ini CMakeLists.txt sdkconfig.defaults

all: build

build: $(ELF)

$(ELF): _check_managed_components $(SRCS) $(FILELIST_SHA_FILE)
	pio run
	@mkdir -p $(STAMP_DIR)
	@echo "$(CURRENT_FILELIST_SHA)" > $(FILELIST_SHA_FILE)
	@touch $(ELF)

tidy: $(TIDY_STAMP)

$(TIDY_STAMP): $(COMPILE_DB) $(SRCS)
	@PATH="$(LLVM_BIN):$$PATH" $(LLVM_BIN)/run-clang-tidy \
		-clang-tidy-binary=$(LLVM_BIN)/clang-tidy \
		-clang-apply-replacements-binary=$(LLVM_BIN)/clang-apply-replacements \
		-p . \
		-quiet \
		-hide-progress \
		-source-filter '.*/coprocessor/src/.*' \
		-header-filter '^(coprocessor/)?src/.*\.h$$' \
		-warnings-as-errors='*' \
		-removed-arg='-mlongcalls' \
		-removed-arg='-fno-shrink-wrap' \
		-removed-arg='-fno-tree-switch-conversion' \
		-removed-arg='-fstrict-volatile-bitfields' \
		-extra-arg='--target=xtensa-esp-elf' \
		-extra-arg='-Qunused-arguments' \
		-extra-arg='-Wno-error' \
		-extra-arg='--sysroot=$(SYSROOT)' \
		-extra-arg='-isystem/Users/nilton/.platformio/packages/toolchain-xtensa-esp-elf/picolibc/include' \
		-extra-arg='-isystem/Users/nilton/.platformio/packages/toolchain-xtensa-esp-elf/xtensa-esp-elf/include/c++/15.2.0' \
		-extra-arg='-isystem/Users/nilton/.platformio/packages/toolchain-xtensa-esp-elf/xtensa-esp-elf/include/c++/15.2.0/xtensa-esp-elf/esp32' \
		-extra-arg='-isystem/Users/nilton/.platformio/packages/toolchain-xtensa-esp-elf/lib/gcc/xtensa-esp-elf/15.2.0/include' \
		-extra-arg='-isystem/Users/nilton/.platformio/packages/toolchain-xtensa-esp-elf/xtensa-esp-elf/include'
	@mkdir -p $(STAMP_DIR) && touch $(TIDY_STAMP)

fmt: $(FMT_STAMP)

$(FMT_STAMP): $(FMT_SRCS)
	$(LLVM_BIN)/clang-format -i $(FMT_SRCS)
	@mkdir -p $(STAMP_DIR) && touch $(FMT_STAMP) $(FMT_CHECK_STAMP)

fmt-check: $(FMT_CHECK_STAMP)

$(FMT_CHECK_STAMP): $(FMT_SRCS)
	$(LLVM_BIN)/clang-format --dry-run --Werror $(FMT_SRCS)
	@mkdir -p $(STAMP_DIR) && touch $(FMT_CHECK_STAMP)

test:
	pio test -e native

clean:
	pio run -t clean
	rm -rf $(STAMP_DIR) $(QEMU_BUILD_DIR) qemu_app/build

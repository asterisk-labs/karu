SHELL := /bin/sh

CMAKE        ?= cmake
CTEST        ?= ctest
CLANG_FORMAT ?= clang-format

BUILD      ?= build
BUILD_TYPE ?= Release
JOBS       ?=
WERROR     ?= ON
PREFIX     ?= $(abspath $(BUILD)/stage)

ASAN_BUILD    ?= build-asan
TSAN_BUILD    ?= build-tsan
PACKAGE_BUILD ?= $(BUILD)/package-test

SYSTEM_NAME := $(shell uname -s 2>/dev/null)
ifeq ($(SYSTEM_NAME),Darwin)
ASAN_RUNTIME_OPTIONS ?= halt_on_error=1
else
ASAN_RUNTIME_OPTIONS ?= detect_leaks=1:halt_on_error=1
endif

CMAKE_FLAGS ?=
PARALLEL := --parallel $(JOBS)

CHANGED_SOURCES := $(shell \
	(git diff --name-only --diff-filter=ACMR HEAD -- include src tests; \
	 git ls-files --others --exclude-standard -- include src tests) | \
	sort -u | awk '/\.(c|h|cpp|hpp)$$/')

.DEFAULT_GOAL := all

.PHONY: help all configure build test debug release install package-test \
	test-asan test-tsan check format format-check clean

help: ## Show the available targets and configuration variables
	@awk 'BEGIN {FS = ":.*## "; print "Karu development targets:\n"} \
		/^[a-zA-Z0-9_-]+:.*## / {printf "  %-16s %s\n", $$1, $$2} \
		END {print "\nVariables: BUILD, BUILD_TYPE, JOBS, WERROR, PREFIX, CMAKE_FLAGS"}' \
		$(MAKEFILE_LIST)

all: build ## Configure and build Karu

configure: ## Configure BUILD with tests enabled
	$(CMAKE) -S . -B "$(BUILD)" \
		-DCMAKE_BUILD_TYPE="$(BUILD_TYPE)" \
		-DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
		-DKARU_BUILD_TESTS=ON \
		-DKARU_WERROR="$(WERROR)" $(CMAKE_FLAGS)

build: configure ## Build Karu
	$(CMAKE) --build "$(BUILD)" --config "$(BUILD_TYPE)" $(PARALLEL)

test: build ## Build and run the component tests
	$(CTEST) --test-dir "$(BUILD)" --build-config "$(BUILD_TYPE)" --output-on-failure

debug: ## Build and test a Debug configuration in build-debug
	$(MAKE) test BUILD=build-debug BUILD_TYPE=Debug

release: ## Build and test a Release configuration in build-release
	$(MAKE) test BUILD=build-release BUILD_TYPE=Release

install: build ## Install Karu into PREFIX (defaults to BUILD/stage)
	$(CMAKE) --install "$(BUILD)" --config "$(BUILD_TYPE)" --prefix "$(PREFIX)"

package-test: install ## Build and test external C and C++ package consumers
	$(CMAKE) -S tests/package -B "$(PACKAGE_BUILD)" \
		-DCMAKE_BUILD_TYPE="$(BUILD_TYPE)" \
		-DCMAKE_PREFIX_PATH="$(PREFIX)" $(CMAKE_FLAGS)
	$(CMAKE) --build "$(PACKAGE_BUILD)" --config "$(BUILD_TYPE)" $(PARALLEL)
	$(CTEST) --test-dir "$(PACKAGE_BUILD)" --build-config "$(BUILD_TYPE)" --output-on-failure

test-asan: ## Run tests with AddressSanitizer and UndefinedBehaviorSanitizer
	$(CMAKE) -S . -B "$(ASAN_BUILD)" \
		-DCMAKE_BUILD_TYPE=Debug \
		-DKARU_BUILD_TESTS=ON \
		-DKARU_WERROR="$(WERROR)" \
		-DKARU_SANITIZE=address,undefined $(CMAKE_FLAGS)
	$(CMAKE) --build "$(ASAN_BUILD)" --config Debug $(PARALLEL)
	ASAN_OPTIONS="$(ASAN_RUNTIME_OPTIONS)" \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	$(CTEST) --test-dir "$(ASAN_BUILD)" --build-config Debug --output-on-failure

test-tsan: ## Run tests with ThreadSanitizer
	$(CMAKE) -S . -B "$(TSAN_BUILD)" \
		-DCMAKE_BUILD_TYPE=Debug \
		-DKARU_BUILD_TESTS=ON \
		-DKARU_WERROR="$(WERROR)" \
		-DKARU_SANITIZE=thread $(CMAKE_FLAGS)
	$(CMAKE) --build "$(TSAN_BUILD)" --config Debug $(PARALLEL)
	TSAN_OPTIONS=halt_on_error=1 \
	$(CTEST) --test-dir "$(TSAN_BUILD)" --build-config Debug --output-on-failure

check: ## Run the complete local validation suite
	$(MAKE) test
	$(MAKE) package-test
	$(MAKE) test-asan
	$(MAKE) test-tsan

format: ## Format changed C and C++ sources with clang-format
	@if [ -n "$(CHANGED_SOURCES)" ]; then \
		$(CLANG_FORMAT) -i $(CHANGED_SOURCES); \
	fi

format-check: ## Verify formatting of changed C and C++ sources
	@if [ -n "$(CHANGED_SOURCES)" ]; then \
		$(CLANG_FORMAT) --dry-run --Werror $(CHANGED_SOURCES); \
	fi

clean: ## Remove only Makefile-managed build directories
	$(CMAKE) -E rm -rf build build-debug build-release build-asan build-tsan

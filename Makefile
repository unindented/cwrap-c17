MAKEFLAGS += --no-builtin-variables --no-builtin-rules

.DEFAULT_GOAL := debug

# Tools.
CC ?= cc
CC := $(CC)
CLANG_FORMAT ?= clang-format
CLANG_FORMAT := $(CLANG_FORMAT)
CLANG_TIDY ?= clang-tidy
CLANG_TIDY := $(CLANG_TIDY)
CPPCHECK ?= cppcheck
CPPCHECK := $(CPPCHECK)

# Project dirs and files.
BUILD_DIR ?= build
BUILD_DIR := $(BUILD_DIR)
DIST_DIR := dist
TARGET := cwrap
TSAN_TARGET := cwrap-tsan
OUT ?= $(TARGET)
VERSION ?= 0.0.0-dev
GOLDEN_DEBUG_DIR := $(BUILD_DIR)/golden-debug
IDEMPOTENT_DEBUG_DIR := $(BUILD_DIR)/idempotent-debug

# Every C file under `tests/fixtures` is a fixture, and the file with the same name under
# `tests/expected` holds the output it must reproduce. A fixture added there needs no change here.
GOLDEN_FIXTURES := $(sort $(notdir $(wildcard tests/fixtures/*.c)))

# Widths bracket the golden width so a shape that only appears once prose collapses onto the opener
# line, or once it no longer fits at all, is still exercised.
IDEMPOTENT_WIDTHS := 20 40 60 100
IDEMPOTENT_INPUTS := $(sort $(wildcard tests/fixtures/*.c) $(wildcard tests/expected/*.c) \
                       $(wildcard src/*/*.c) $(wildcard src/*/*.h))

SRC := $(wildcard src/*/*.c)
LIB_SRC := $(filter-out src/app/main.c,$(SRC))
TEST_SRC := $(wildcard tests/test_*.c)
TEST_NAMES := $(patsubst tests/test_%.c,%,$(TEST_SRC))

# Flags.
CSTD := -std=c17

# Keep a warning flag only when the active compiler accepts it, so a compiler-specific flag cannot
# break the `-Werror` build under the other compiler. The probe preprocesses an empty file, which
# accepts or rejects the flag without compiling anything.
cc-option = $(shell $(CC) -Werror $(1) -E -x c /dev/null >/dev/null 2>&1 && echo $(1))

# Warnings. `WARN_SHARED` is everything the source and tests both get: the broad baseline promoted
# to errors, plus the specific checks that stay quiet on test code. `WARN` layers the stricter
# source-only groups on top. GCC-only and Clang-only flags pass through `cc-option`, so each
# compiler enables the ones it knows and silently drops the rest.
WARN_SHARED := -Wall -Wextra -Wpedantic -Werror
# Format strings, with argument signedness where the compiler offers it.
WARN_SHARED += -Wformat=2 $(call cc-option,-Wformat-signedness)
# No variable-length arrays, so every stack buffer stays bounded by a named constant.
WARN_SHARED += -Wvla
# No goto that jumps over a variable's initialization; the cleanup idiom pre-declares instead.
WARN_SHARED += -Wjump-misses-init
# Copy-paste conditions, operator logic slips, and out-of-width shifts (GCC).
WARN_SHARED += $(call cc-option,-Wlogical-op) $(call cc-option,-Wduplicated-cond) $(call cc-option,-Wshift-overflow=2)
# Reads uninitialized on some path, out-of-range enum assignment, and comma-operator misuse (Clang).
WARN_SHARED += $(call cc-option,-Wconditional-uninitialized) $(call cc-option,-Wassign-enum) $(call cc-option,-Wcomma)

# Source only. On the tests these would fire on fixture setup and deliberate casts without surfacing
# real defects, so `TEST_WARN` leaves them off.
WARN := $(WARN_SHARED)
# Implicit conversions that lose range or flip signedness.
WARN += -Wconversion -Wsign-conversion
# Declaration hygiene: full prototypes, no shadowing, no undefined macro in an `#if`.
WARN += -Wstrict-prototypes -Wmissing-prototypes -Wshadow -Wundef
# Const-correctness and read-only string literals.
WARN += -Wcast-qual -Wwrite-strings

# Test only.
TEST_WARN := $(WARN_SHARED)

CPPFLAGS := -Isrc -isystem vendor/copt -isystem vendor/acutest -DCWRAP_VERSION=\"$(VERSION)\"
SAN_FLAGS := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
TSAN_FLAGS := -fsanitize=thread -fno-sanitize-recover=all -fno-omit-frame-pointer
DEBUG_BASE_CFLAGS := $(CSTD) -g -Og
DEBUG_CFLAGS := $(DEBUG_BASE_CFLAGS) $(WARN) $(SAN_FLAGS)
RELEASE_CFLAGS := $(CSTD) -O2 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3 $(WARN)
RELEASE_LDFLAGS := -s
TSAN_CFLAGS := $(DEBUG_BASE_CFLAGS) $(WARN) $(TSAN_FLAGS)
TEST_CFLAGS := $(DEBUG_BASE_CFLAGS) $(TEST_WARN) $(SAN_FLAGS)

# Build artifacts.
DEBUG_DIR := $(BUILD_DIR)/debug
RELEASE_DIR := $(BUILD_DIR)/release
TSAN_DIR := $(BUILD_DIR)/tsan
TEST_DIR := $(BUILD_DIR)/test

DEBUG_BIN := $(DEBUG_DIR)/$(TARGET)
RELEASE_BIN := $(RELEASE_DIR)/$(TARGET)
TSAN_BIN := $(TSAN_DIR)/$(TSAN_TARGET)

DEBUG_SRC_OBJS := $(patsubst src/%.c,$(DEBUG_DIR)/src/%.o,$(SRC))
RELEASE_SRC_OBJS := $(patsubst src/%.c,$(RELEASE_DIR)/src/%.o,$(SRC))
TSAN_SRC_OBJS := $(patsubst src/%.c,$(TSAN_DIR)/src/%.o,$(SRC))
DEBUG_OBJS := $(DEBUG_SRC_OBJS)
RELEASE_OBJS := $(RELEASE_SRC_OBJS)
TSAN_OBJS := $(TSAN_SRC_OBJS)
DEBUG_LIB_OBJS := $(patsubst src/%.c,$(DEBUG_DIR)/src/%.o,$(LIB_SRC))
TEST_OBJS := $(patsubst tests/%.c,$(TEST_DIR)/tests/%.o,$(TEST_SRC))
TEST_BINS := $(addprefix $(TEST_DIR)/test_,$(TEST_NAMES))

DEPS := $(DEBUG_OBJS:.o=.d) $(RELEASE_OBJS:.o=.d) $(TSAN_OBJS:.o=.d) $(TEST_OBJS:.o=.d)

ALL_OBJS := $(DEBUG_OBJS) $(RELEASE_OBJS) $(TSAN_OBJS) $(TEST_OBJS)
ALL_BINS := $(DEBUG_BIN) $(RELEASE_BIN) $(TSAN_BIN) $(TEST_BINS)
BUILD_DIRS := $(sort $(dir $(ALL_OBJS) $(ALL_BINS)))

.PHONY: all debug release tsan dirs format lint test-debug golden-debug idempotent-debug ci clean

# Public targets.
all: debug

debug: $(DEBUG_BIN)
	@cp "$<" "$(TARGET)"

release: $(RELEASE_BIN)
	@mkdir -p "$(dir $(OUT))"
	@cp "$<" "$(OUT)"

tsan: $(TSAN_BIN)
	@cp "$<" "$(TSAN_TARGET)"

dirs:
	@mkdir -p $(BUILD_DIRS)

# Canned recipes shared by every build variant.
define COMPILE
@$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c "$<" -o "$@"
endef

define LINK
@$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o "$@"
endef

# Generate the compile and link rules for one build variant.
# $(1) is the variable prefix: DEBUG, RELEASE, or TSAN.
define build_variant
$$($(1)_OBJS): CFLAGS := $$($(1)_CFLAGS)
$$($(1)_SRC_OBJS): $$($(1)_DIR)/src/%.o: src/%.c Makefile | dirs
	$$(COMPILE)

$$($(1)_BIN): CFLAGS := $$($(1)_CFLAGS)
$$($(1)_BIN): LDFLAGS := $$($(1)_LDFLAGS)
$$($(1)_BIN): $$($(1)_OBJS) | dirs
	$$(LINK)
endef

$(eval $(call build_variant,DEBUG))
$(eval $(call build_variant,RELEASE))
$(eval $(call build_variant,TSAN))

# Tests reuse the debug library objects.
$(TEST_OBJS): CFLAGS := $(TEST_CFLAGS)
$(TEST_OBJS): $(TEST_DIR)/tests/%.o: tests/%.c Makefile | dirs
	$(COMPILE)

$(TEST_DIR)/test_%: $(TEST_DIR)/tests/test_%.o $(DEBUG_LIB_OBJS) Makefile | dirs
	@$(CC) $(TEST_CFLAGS) "$<" $(DEBUG_LIB_OBJS) $(LDLIBS) -o "$@"

# Quality.
format:
	@$(CLANG_FORMAT) -i src/*/*.[ch] tests/*.[ch]

lint:
	@if command -v $(CLANG_FORMAT) >/dev/null 2>&1; then \
		$(CLANG_FORMAT) --dry-run --Werror src/*/*.[ch] tests/*.[ch]; \
	else \
		echo "skipping clang-format: $(CLANG_FORMAT) not found"; \
	fi
	@if command -v $(CLANG_TIDY) >/dev/null 2>&1; then \
		$(CLANG_TIDY) --quiet src/*/*.c -- $(CPPFLAGS) -std=c17; \
	else \
		echo "skipping clang-tidy: $(CLANG_TIDY) not found"; \
	fi
	@if command -v $(CPPCHECK) >/dev/null 2>&1; then \
		$(CPPCHECK) --enable=warning,performance,portability --std=c17 --quiet --error-exitcode=1 src tests; \
	else \
		echo "skipping cppcheck: $(CPPCHECK) not found"; \
	fi

test-debug: debug $(TEST_BINS)
	@set -e; for test in $(TEST_BINS); do $$test; done

# Rewraps every fixture and diffs each result against its expected output. The scratch directory is
# wiped first, so an output left behind by an earlier run cannot pass.
golden-debug: debug
	@rm -rf "$(GOLDEN_DEBUG_DIR)"
	@mkdir -p "$(GOLDEN_DEBUG_DIR)"
	@set -e; for fixture in $(GOLDEN_FIXTURES); do \
		"$(DEBUG_BIN)" -w 40 "tests/fixtures/$$fixture" > "$(GOLDEN_DEBUG_DIR)/$$fixture"; \
		diff -u "tests/expected/$$fixture" "$(GOLDEN_DEBUG_DIR)/$$fixture"; \
	done

# A second pass over cwrap's own output must change nothing, and `--check` must agree that it would
# not. A shape the emitter cannot reproduce from its own output makes `--check` report a rewrap for a
# file `--in-place` just wrote, which fails CI on already-formatted sources. `src/` is included
# because real comment shapes cover far more than the fixtures do.
idempotent-debug: debug
	@rm -rf "$(IDEMPOTENT_DEBUG_DIR)"
	@mkdir -p "$(IDEMPOTENT_DEBUG_DIR)"
	@set -e; once="$(IDEMPOTENT_DEBUG_DIR)/once.c"; twice="$(IDEMPOTENT_DEBUG_DIR)/twice.c"; \
	for width in $(IDEMPOTENT_WIDTHS); do \
		for input in $(IDEMPOTENT_INPUTS); do \
			"$(DEBUG_BIN)" -w "$$width" "$$input" > "$$once"; \
			"$(DEBUG_BIN)" -w "$$width" "$$once" > "$$twice"; \
			if ! diff -u "$$once" "$$twice"; then \
				echo "not idempotent: $$input at width $$width" >&2; exit 1; \
			fi; \
			if ! "$(DEBUG_BIN)" --check -w "$$width" "$$once"; then \
				echo "--check disagrees with own output: $$input at width $$width" >&2; exit 1; \
			fi; \
		done; \
	done

ci:
	@$(MAKE) lint
	@$(MAKE) release
	@$(MAKE) test-debug
	@$(MAKE) golden-debug
	@$(MAKE) idempotent-debug

clean:
	@rm -rf "$(BUILD_DIR)" "$(DIST_DIR)" "$(TARGET)" "$(TSAN_TARGET)"

# Generated dependencies.
-include $(DEPS)

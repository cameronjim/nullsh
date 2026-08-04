# nullsh build.
# Targets: all (= release), debug, release, test, clean.
# Every object and binary lands under build/, so clean is one rm -rf.

CC := gcc

WARN           := -std=c17 -Wall -Wextra -Werror -pedantic
CFLAGS_DEBUG   := $(WARN) -g -O0 -fsanitize=address,undefined
CFLAGS_RELEASE := $(WARN) -O2

BUILD := build

SRCS     := $(shell find src -name '*.c' ! -name '*_test.c')
REL_OBJS := $(patsubst %.c,$(BUILD)/release/%.o,$(SRCS))
DBG_OBJS := $(patsubst %.c,$(BUILD)/debug/%.o,$(SRCS))

REL_BIN := $(BUILD)/nullsh
DBG_BIN := $(BUILD)/nullsh-debug

# A unit test links the object sitting next to it (src/x/foo_test.c pulls in
# build/debug/src/x/foo.o) plus the core objects every module leans on.
# main.o is never linked into a test binary because it owns main().
CORE_TEST_OBJS := $(BUILD)/debug/src/alloc/alloc.o $(BUILD)/debug/src/alloc/firstfit.o \
                  $(BUILD)/debug/src/alloc/buddy.o $(BUILD)/debug/src/util/error.o \
                  $(BUILD)/debug/src/util/str.o $(BUILD)/debug/src/util/vec.o \
                  $(BUILD)/debug/src/shell/lexer.o $(BUILD)/debug/src/shell/expand.o \
                  $(BUILD)/debug/src/shell/history.o $(BUILD)/debug/src/shell/parser.o \
                  $(BUILD)/debug/src/shell/builtin.o \
                  $(BUILD)/debug/src/alloc/heap_builtin.o

UNIT_TEST_SRCS := $(shell find src -name '*_test.c')
SELF_TEST_SRCS := $(sort $(wildcard tests/*.c))
INTEGRATION_SH := $(sort $(wildcard tests/integration/*.sh))

TEST_BINS := $(foreach t,$(UNIT_TEST_SRCS),$(BUILD)/tests/$(subst /,_,$(basename $(t)))) \
             $(foreach t,$(SELF_TEST_SRCS),$(BUILD)/tests/$(basename $(notdir $(t))))

.PHONY: all release debug test clean

all: release

release: $(REL_BIN)

debug: $(DBG_BIN)

$(REL_BIN): $(REL_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_RELEASE) -o $@ $^

$(DBG_BIN): $(DBG_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_DEBUG) -o $@ $^

$(BUILD)/release/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_RELEASE) -MMD -MP -c $< -o $@

$(BUILD)/debug/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_DEBUG) -MMD -MP -c $< -o $@

define UNIT_TEST_RULE
$(BUILD)/tests/$(subst /,_,$(basename $(1))): $(1) $(sort $(BUILD)/debug/$(patsubst %_test.c,%.o,$(1)) $(CORE_TEST_OBJS))
	@mkdir -p $$(dir $$@)
	$$(CC) $$(CFLAGS_DEBUG) -o $$@ $$^
endef

define SELF_TEST_RULE
$(BUILD)/tests/$(basename $(notdir $(1))): $(1) $(CORE_TEST_OBJS)
	@mkdir -p $$(dir $$@)
	$$(CC) $$(CFLAGS_DEBUG) -o $$@ $$^
endef

$(foreach t,$(UNIT_TEST_SRCS),$(eval $(call UNIT_TEST_RULE,$(t))))
$(foreach t,$(SELF_TEST_SRCS),$(eval $(call SELF_TEST_RULE,$(t))))

# The whole suite runs once per allocator strategy; NSH_ALLOC_STRATEGY picks the boot arena.
test: $(DBG_BIN) $(TEST_BINS)
	@fail=0; \
	for strat in firstfit buddy; do \
		echo "===== PASS: NSH_ALLOC_STRATEGY=$$strat ====="; \
		for t in $(TEST_BINS); do \
			echo "== unit [$$strat] $$t"; \
			NSH_ALLOC_STRATEGY=$$strat ./$$t || { echo "FAILED [$$strat]: $$t"; fail=1; }; \
		done; \
		for s in $(INTEGRATION_SH); do \
			echo "== integration [$$strat] $$s"; \
			NSH_ALLOC_STRATEGY=$$strat sh $$s ./$(DBG_BIN) || { echo "FAILED [$$strat]: $$s"; fail=1; }; \
		done; \
		echo "===== END PASS: $$strat ====="; \
	done; \
	if [ $$fail -ne 0 ]; then echo "SUITE FAILED"; exit 1; fi; \
	echo "SUITE PASSED (firstfit + buddy)"

clean:
	rm -rf $(BUILD)

-include $(REL_OBJS:.o=.d) $(DBG_OBJS:.o=.d)

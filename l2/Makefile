CC ?= cc
CPPFLAGS += -Iinclude
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Werror
BUILD_DIR ?= build

.PHONY: all apps check check-demos
all: apps $(BUILD_DIR)/test_probe $(BUILD_DIR)/test_evset $(BUILD_DIR)/build_sets
apps: $(BUILD_DIR)/l2-attacker $(BUILD_DIR)/l2-victim

APP_HEADERS := $(wildcard include/*.h)
APP_OBJECTS := $(BUILD_DIR)/l2_evset.o $(BUILD_DIR)/l2_pagemap.o $(BUILD_DIR)/util.o
DEMO_WRAPS := shm_open shm_unlink mmap sem_wait sem_post pin_to_cpu prctl l2_prepare_measurement l2_evsets_create l2_evsets_destroy
DEMO_WRAP_FLAGS := $(foreach symbol,$(DEMO_WRAPS),-Wl,--wrap=$(symbol))

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/integration:
	mkdir -p $@

$(BUILD_DIR)/test_probe: tests/test_probe.c include/l2_probe.h include/l2_victim.h include/l2_timing.h include/l2_types.h include/l2_config.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -UNDEBUG $(LDFLAGS) $< -o $@ $(LDLIBS)

$(BUILD_DIR)/l2_pagemap.o: src/l2_pagemap.c include/l2_pagemap.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/l2_evset.o: src/l2_evset.c include/l2_evset.h include/l2_pagemap.h include/l2_types.h include/l2_config.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/util.o: src/util.c include/l2_util.h include/l2_timing.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/l2-attacker: ../demo/l2-attacker.c $(APP_OBJECTS) $(APP_HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< $(APP_OBJECTS) -pthread -o $@ $(LDLIBS)

$(BUILD_DIR)/l2-victim: ../demo/l2-victim.c $(APP_OBJECTS) $(APP_HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< $(APP_OBJECTS) -pthread -o $@ $(LDLIBS)

$(BUILD_DIR)/test_evset: tests/test_evset.c $(BUILD_DIR)/l2_evset.o include/l2_evset.h include/l2_pagemap.h include/l2_types.h include/l2_config.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -UNDEBUG $(LDFLAGS) $< $(BUILD_DIR)/l2_evset.o -o $@ $(LDLIBS)

$(BUILD_DIR)/build_sets: examples/build_sets.c $(BUILD_DIR)/l2_evset.o $(BUILD_DIR)/l2_pagemap.o include/l2_evset.h include/l2_types.h include/l2_config.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< $(BUILD_DIR)/l2_evset.o $(BUILD_DIR)/l2_pagemap.o -o $@ $(LDLIBS)

$(BUILD_DIR)/integration/l2-attacker: ../demo/l2-attacker.c tests/demo_stubs.c $(APP_OBJECTS) $(APP_HEADERS) | $(BUILD_DIR)/integration
	$(CC) $(CPPFLAGS) $(CFLAGS) -UNDEBUG $(LDFLAGS) $< tests/demo_stubs.c $(APP_OBJECTS) $(DEMO_WRAP_FLAGS) -pthread -o $@ $(LDLIBS)

$(BUILD_DIR)/integration/l2-victim: ../demo/l2-victim.c tests/demo_stubs.c $(APP_OBJECTS) $(APP_HEADERS) | $(BUILD_DIR)/integration
	$(CC) $(CPPFLAGS) $(CFLAGS) -UNDEBUG $(LDFLAGS) $< tests/demo_stubs.c $(APP_OBJECTS) $(DEMO_WRAP_FLAGS) -pthread -o $@ $(LDLIBS)

check-demos: $(BUILD_DIR)/integration/l2-attacker $(BUILD_DIR)/integration/l2-victim
	python3 tests/test_demos.py $^

check: all check-demos
	$(BUILD_DIR)/test_probe
	$(BUILD_DIR)/test_evset

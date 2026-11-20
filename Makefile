# LintasGuard top-level Makefile. Run from Git Bash. Target builds need qnx-env.cmd loaded.
CC_HOST ?= gcc
CFLAGS  := -Wall -Wextra -Werror -std=c11
QCC     ?= qcc -Vgcc_ntox86_64
BUILD   := build

CORE_SRC := $(wildcard core/*.c)
CORE_INC := -Icore/include -Icommon/include
UNIT_SRC := $(wildcard tests/unit/test_*.c)
UNIT_BIN := $(patsubst tests/unit/%.c,$(BUILD)/host/%,$(UNIT_SRC))
HDRS     := $(wildcard common/include/*.h core/include/*.h target/include/*.h tests/unit/*.h)

TARGET_INC  := $(CORE_INC) -Itarget/include
# Strict -std=c11 hides the POSIX/QNX declarations in the SDP headers; _QNX_SOURCE brings them back.
QNX_DEFS    := -D_QNX_SOURCE
TARGET_BINS := $(BUILD)/target/lg-stub $(BUILD)/target/lg-gate $(BUILD)/target/lg-gate-test $(BUILD)/target/lg-prio-demo

.PHONY: host test lint target image deploy evidence clean

host: $(UNIT_BIN)

$(BUILD)/host/%: tests/unit/%.c $(CORE_SRC) $(HDRS)
	@mkdir -p $(BUILD)/host
	$(CC_HOST) $(CFLAGS) $(CORE_INC) $(filter %.c,$^) -o $@

test: host
	@rc=0; for t in $(UNIT_BIN); do ./$$t || rc=1; done; exit $$rc

lint:
	@command -v clang-format >/dev/null 2>&1 && \
	  clang-format --dry-run -Werror $(shell find core common target tests -name '*.c' -o -name '*.h' 2>/dev/null) || \
	  echo "clang-format not installed: format check skipped"
	@$(MAKE) --no-print-directory host

target: $(TARGET_BINS)

# $(call qcc_build,<extra link flags>): build the .c prerequisites into $@ with qcc.
define qcc_build
	@command -v qcc >/dev/null 2>&1 || { echo "qcc not found: load E:\\code\\Heketon\\QNX\\qnx-env.cmd first"; exit 1; }
	@mkdir -p $(BUILD)/target
	$(QCC) $(CFLAGS) $(QNX_DEFS) $(TARGET_INC) $(filter %.c,$^) -o $@ $(1)
endef

$(BUILD)/target/lg-stub: target/tools/qnx_stub/stub.c $(CORE_SRC) $(HDRS)
	$(call qcc_build,)

$(BUILD)/target/lg-gate: $(wildcard target/lg-gate/*.c) core/gate_model.c $(HDRS)
	$(call qcc_build,-lslog2)

$(BUILD)/target/lg-gate-test: $(wildcard target/tools/lg-gate-test/*.c) core/gate_model.c $(HDRS)
	$(call qcc_build,)

$(BUILD)/target/lg-prio-demo: $(wildcard target/tools/lg-prio-demo/*.c) $(HDRS)
	$(call qcc_build,)

image:
	@echo "Run in CMD: call E:\\code\\Heketon\\QNX\\qnx-env.cmd && image\\build_vm.cmd"

deploy: target
	@test -n "$(IP)" || { echo "usage: make deploy IP=<vm-ip>"; exit 1; }
	bash scripts/deploy.sh $(IP)

evidence: target
	@test -n "$(IP)" || { echo "usage: make evidence IP=<vm-ip>"; exit 1; }
	bash scripts/deploy.sh $(IP)
	bash scripts/collect_evidence.sh $(IP)

clean:
	rm -rf $(BUILD)

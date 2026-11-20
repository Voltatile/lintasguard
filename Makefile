# LintasGuard top-level Makefile. Run from Git Bash. Target builds need qnx-env.cmd loaded.
CC_HOST ?= gcc
CFLAGS  := -Wall -Wextra -Werror -std=c11
QCC     ?= qcc -Vgcc_ntox86_64
BUILD   := build

CORE_SRC := $(wildcard core/*.c)
CORE_INC := -Icore/include -Icommon/include
UNIT_SRC := $(wildcard tests/unit/test_*.c)
UNIT_BIN := $(patsubst tests/unit/%.c,$(BUILD)/host/%,$(UNIT_SRC))
TARGET_BIN := $(BUILD)/target/lg-stub

.PHONY: host test lint target image deploy evidence clean

host: $(UNIT_BIN)

$(BUILD)/host/%: tests/unit/%.c $(CORE_SRC)
	@mkdir -p $(BUILD)/host
	$(CC_HOST) $(CFLAGS) $(CORE_INC) $^ -o $@

test: host
	@rc=0; for t in $(UNIT_BIN); do ./$$t || rc=1; done; exit $$rc

lint:
	@command -v clang-format >/dev/null 2>&1 && \
	  clang-format --dry-run -Werror $(shell find core common target tests -name '*.c' -o -name '*.h' 2>/dev/null) || \
	  echo "clang-format not installed: format check skipped"
	@$(MAKE) --no-print-directory host

target: $(TARGET_BIN)

$(TARGET_BIN): target/tools/qnx_stub/stub.c $(CORE_SRC)
	@command -v qcc >/dev/null 2>&1 || { echo "qcc not found: load E:\code\Heketon\QNX\qnx-env.cmd first"; exit 1; }
	@mkdir -p $(BUILD)/target
	$(QCC) $(CFLAGS) $(CORE_INC) $^ -o $@

image:
	@echo "Run in CMD: call E:\code\Heketon\QNX\qnx-env.cmd && image\build_vm.cmd"

deploy: target
	@test -n "$(IP)" || { echo "usage: make deploy IP=<vm-ip>"; exit 1; }
	bash scripts/deploy.sh $(IP)

evidence:
	@test -n "$(IP)" || { echo "usage: make evidence IP=<vm-ip>"; exit 1; }
	bash scripts/collect_evidence.sh $(IP)

clean:
	rm -rf $(BUILD)

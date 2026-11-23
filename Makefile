# LintasGuard top-level Makefile. Run from Git Bash. Target builds need qnx-env.cmd loaded.
CC_HOST ?= gcc
CFLAGS  := -Wall -Wextra -Werror -std=c11
QCC     ?= qcc -Vgcc_ntox86_64
BUILD   := build

CORE_SRC := $(wildcard core/*.c)
CORE_INC := -Icore/include -Icommon/include
UNIT_SRC := $(wildcard tests/unit/test_*.c)
SCEN_SRC := $(wildcard tests/scenario/replay_*.c)
UNIT_BIN := $(patsubst tests/unit/%.c,$(BUILD)/host/%,$(UNIT_SRC)) \
            $(patsubst tests/scenario/%.c,$(BUILD)/host/%,$(SCEN_SRC))
HDRS     := $(wildcard common/include/*.h core/include/*.h target/include/*.h tests/unit/*.h)

TARGET_INC  := $(CORE_INC) -Itarget/include
# Strict -std=c11 hides the POSIX/QNX declarations in the SDP headers; _QNX_SOURCE brings them back.
QNX_DEFS    := -D_QNX_SOURCE
TARGET_BINS := $(addprefix $(BUILD)/target/,lg-stub lg-gate lg-gate-test lg-prio-demo lg-plant lg-sensor lgctl lg-sample-sink lg-jitter lg-chan-demo lg-controller lg-meter lg-supervisor lg-bridge)

.PHONY: host test lint target image deploy evidence clean

host: $(UNIT_BIN)

$(BUILD)/host/%: tests/unit/%.c $(CORE_SRC) $(HDRS)
	@mkdir -p $(BUILD)/host
	$(CC_HOST) $(CFLAGS) $(CORE_INC) $(filter %.c,$^) -o $@

$(BUILD)/host/%: tests/scenario/%.c $(CORE_SRC) $(HDRS)
	@mkdir -p $(BUILD)/host
	$(CC_HOST) $(CFLAGS) $(CORE_INC) $(filter %.c,$^) -o $@

# C unit tests and replays, the static no-gate check of lg-bridge (and its self-test), and the host Python
# tests. Python is python or python3; without either the Python tests are reported as SKIP, not silently.
test: host
	@rc=0; for t in $(UNIT_BIN); do ./$$t || rc=1; done; \
	sh tests/static/check_bridge_no_gate.sh || rc=1; \
	sh tests/static/test_check_bridge_no_gate.sh || rc=1; \
	py=""; for c in python python3; do command -v $$c >/dev/null 2>&1 && { py=$$c; break; }; done; \
	if [ -z "$$py" ]; then echo "SKIP: host python tests (no python or python3)"; \
	elif [ -d tests/host ]; then \
	  if $$py -m unittest discover -s tests/host -p 'test_*.py' -q >build/host/pytest.log 2>&1; \
	  then echo "PASS: host python tests"; else cat build/host/pytest.log; echo "FAIL: host python tests"; rc=1; fi; \
	fi; exit $$rc

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

$(BUILD)/target/lg-plant: $(wildcard target/lg-plant/*.c) core/plant_model.c core/scenario.c core/json_mini.c $(HDRS)
	$(call qcc_build,-lslog2)

$(BUILD)/target/lg-sensor: $(wildcard target/lg-sensor/*.c) core/sensor_model.c core/metrics.c $(HDRS)
	$(call qcc_build,-lslog2)

$(BUILD)/target/lgctl: $(wildcard target/lgctl/*.c) core/sensor_model.c $(HDRS)
	$(call qcc_build,)

$(BUILD)/target/lg-sample-sink: $(wildcard target/tools/lg-sample-sink/*.c) $(HDRS)
	$(call qcc_build,)

$(BUILD)/target/lg-jitter: $(wildcard target/tools/lg-jitter/*.c) $(HDRS)
	$(call qcc_build,)

$(BUILD)/target/lg-chan-demo: $(wildcard target/tools/lg-chan-demo/*.c) $(HDRS)
	$(call qcc_build,)

$(BUILD)/target/lg-controller: $(wildcard target/lg-controller/*.c) core/controller_logic.c core/voting.c core/mode_manager.c core/metrics.c core/gate_model.c core/guards.c core/json_mini.c core/scenario.c $(HDRS)
	$(call qcc_build,-lslog2)

$(BUILD)/target/lg-supervisor: $(wildcard target/lg-supervisor/*.c) core/supervisor_logic.c $(HDRS)
	$(call qcc_build,-lslog2)

$(BUILD)/target/lg-bridge: $(wildcard target/lg-bridge/*.c) core/bridge_proto.c core/json_mini.c core/metrics.c $(HDRS)
	$(call qcc_build,-lslog2 -lsocket)

$(BUILD)/target/lg-meter: $(wildcard target/tools/lg-meter/*.c) core/metrics.c core/gate_model.c $(HDRS)
	$(call qcc_build,)

image:
	@echo "Run in CMD: call E:\\code\\Heketon\\QNX\\qnx-env.cmd && image\\build_vm.cmd"

deploy: target
	@test -n "$(IP)" || { echo "usage: make deploy IP=<vm-ip>"; exit 1; }
	bash scripts/deploy.sh $(IP)

evidence: target
	@test -n "$(IP)" || { echo "usage: make evidence IP=<vm-ip> [TESTS=\"step06_sensors ...\"]"; exit 1; }
	bash scripts/deploy.sh $(IP)
	bash scripts/collect_evidence.sh $(IP) $(TESTS)

clean:
	rm -rf $(BUILD)

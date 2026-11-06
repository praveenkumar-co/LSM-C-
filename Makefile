# Build and run every tests/*.cpp in one of four modes. All modes use -Werror.
#   make test        optimized build
#   make asan        AddressSanitizer + UndefinedBehaviorSanitizer
#   make tsan        ThreadSanitizer (defines TSAN_BUILD)
#   make test-quick  optimized build with -DTEST_QUICK: smaller workloads, under 32 MiB written
#   make bench       delta-update and GC benchmarks (not part of any test mode)
#   make bench-stress  100,000-key, 16-thread stress benchmark with GC and 50 kill -9 crashes
#   make bench-compare IndexedStore vs SQLite vs legacy MongoLite (links -lsqlite3)
#   make bench-scan  range scans of 100 keys, 1 and 4 threads (normal build + -DSCAN_PROFILE build)
#   make bench-reality  builds bench_reality and bench_crash (macOS only; run via scripts/reality/run_safe.sh)
#   make clean
#
# Each test binary prints one "[PASS] ..." line per test (TEST_VERBOSE=1 adds measurements) and
# "[Budget] bytes written: N of M". After a run, the Makefile adds up N over all binaries and
# fails if the sum is over the mode's limit (512 MiB for test/asan/tsan, 32 MiB for test-quick).

ifeq ($(origin CXX),default)
CXX := clang++
endif
CXXSTD   := -std=c++17
WARN     := -Wall -Wextra -Werror
INCLUDES := -Iinclude
LDFLAGS  := -pthread

TEST_SRCS := $(wildcard tests/*.cpp)
TEST_NAMES := $(notdir $(basename $(TEST_SRCS)))
HEADERS := $(wildcard include/vlog/*.hpp include/index/*.hpp include/doc/*.hpp tests/*.hpp)

FLAGS_test  := -O2
FLAGS_asan  := -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all
FLAGS_tsan  := -O1 -g -fsanitize=thread -DTSAN_BUILD
FLAGS_quick := -O2 -DTEST_QUICK

LIMIT_test  := 536870912
LIMIT_asan  := 536870912
LIMIT_tsan  := 536870912
LIMIT_quick := 33554432

.PHONY: test asan tsan quick test-quick bench bench-stress bench-compare bench-scan bench-reality clean check-deps

# vlog/ and doc/ are bottom layers: vlog/ must not depend on index/, doc/ or legacy/, and doc/
# (JSON and update ops) must not depend on vlog/, index/ or legacy/.
check-deps:
	@if grep -nE '#include *[<"](\.\./)*(index|doc|legacy)/' include/vlog/*.hpp; then \
	  echo "check-deps: FAIL (include/vlog must not include index/, doc/ or legacy/)"; exit 1; \
	elif grep -nE '#include *[<"](\.\./)*(index|vlog|legacy)/' include/doc/*.hpp; then \
	  echo "check-deps: FAIL (include/doc must not include index/, vlog/ or legacy/)"; exit 1; \
	else echo "check-deps: OK (vlog/ and doc/ have no upward includes)"; fi

define MODE_RULES
build/$(1)/%: tests/%.cpp $(HEADERS) Makefile
	@mkdir -p build/$(1)
	$$(CXX) $$(CXXSTD) $$(WARN) $$(FLAGS_$(1)) $$(INCLUDES) $$(LDFLAGS) $$< -o $$@

$(1): check-deps $(addprefix build/$(1)/,$(TEST_NAMES))
	@total=0; for t in $(TEST_NAMES); do \
	  echo "=== [$(1)] $$$$t ==="; \
	  { ./build/$(1)/$$$$t 2>&1; echo $$$$? > build/$(1)/$$$$t.rc; } | tee build/$(1)/$$$$t.log; \
	  rc=$$$$(cat build/$(1)/$$$$t.rc); \
	  if [ $$$$rc -ne 0 ]; then echo "=== [$(1)] $$$$t: FAILED (exit $$$$rc) ==="; exit $$$$rc; fi; \
	  echo "=== [$(1)] $$$$t: exit 0 ==="; \
	  b=$$$$(sed -n 's/^\[Budget\] bytes written: \([0-9]*\).*/\1/p' build/$(1)/$$$$t.log | tail -1); \
	  if [ -z "$$$$b" ]; then echo "=== [$(1)] $$$$t: no [Budget] line ==="; exit 1; fi; \
	  total=$$$$((total + b)); \
	done; \
	echo "=== [$(1)] bytes written by all test binaries: $$$$total (limit $(LIMIT_$(1))) ==="; \
	if [ $$$$total -gt $(LIMIT_$(1)) ]; then echo "=== [$(1)] FAILED: write budget exceeded ==="; exit 1; fi
endef

$(eval $(call MODE_RULES,test))
$(eval $(call MODE_RULES,asan))
$(eval $(call MODE_RULES,tsan))
$(eval $(call MODE_RULES,quick))

test-quick: quick

build/bench/bench_delta: bench/bench_delta.cpp $(HEADERS) Makefile
	@mkdir -p build/bench
	$(CXX) $(CXXSTD) $(WARN) -O2 $(INCLUDES) $(LDFLAGS) $< -o $@

build/bench/bench_gc: bench/bench_gc.cpp $(HEADERS) Makefile
	@mkdir -p build/bench
	$(CXX) $(CXXSTD) $(WARN) -O2 $(INCLUDES) $(LDFLAGS) $< -o $@

bench: check-deps build/bench/bench_delta build/bench/bench_gc
	./build/bench/bench_delta
	./build/bench/bench_gc

# The stress benchmark writes about 1 GB, so it sets its own write cap (2 GiB).
build/bench/bench_stress: bench/bench_stress.cpp $(HEADERS) Makefile
	@mkdir -p build/bench
	$(CXX) $(CXXSTD) $(WARN) -O2 -DBENCH_BUDGET_BYTES=2147483648ULL $(INCLUDES) $(LDFLAGS) $< -o $@

bench-stress: check-deps build/bench/bench_stress
	./build/bench/bench_stress

# Engine comparison: legacy/ is included as system headers (unmodified, its own warnings are
# not ours); write cap 20 GB; the Mac is kept awake while it runs.
build/bench/bench_compare: bench/bench_compare.cpp $(HEADERS) Makefile
	@mkdir -p build/bench
	$(CXX) $(CXXSTD) $(WARN) -O2 -DBENCH_BUDGET_BYTES=20000000000ULL $(INCLUDES) -isystem legacy $(LDFLAGS) $< -o $@ -lsqlite3

KEEP_AWAKE := $(shell command -v caffeinate >/dev/null 2>&1 && echo "caffeinate -dims")
bench-compare: check-deps build/bench/bench_compare
	$(KEEP_AWAKE) ./build/bench/bench_compare

# Range scan benchmark: write cap 256 MiB per binary; each binary checks free disk >= 10 GB at
# start. bench_scan_profile is the same program with -DSCAN_PROFILE (timers in the read path).
build/bench/bench_scan: bench/bench_scan.cpp $(HEADERS) Makefile
	@mkdir -p build/bench
	$(CXX) $(CXXSTD) $(WARN) -O2 -DBENCH_BUDGET_BYTES=268435456ULL $(INCLUDES) $(LDFLAGS) $< -o $@

build/bench/bench_scan_profile: bench/bench_scan.cpp $(HEADERS) Makefile
	@mkdir -p build/bench
	$(CXX) $(CXXSTD) $(WARN) -O2 -DSCAN_PROFILE -DBENCH_BUDGET_BYTES=268435456ULL $(INCLUDES) $(LDFLAGS) $< -o $@

bench-scan: check-deps build/bench/bench_scan build/bench/bench_scan_profile
	$(KEEP_AWAKE) ./build/bench/bench_scan
	$(KEEP_AWAKE) ./build/bench/bench_scan_profile

# Reality-check tools (Phase 7): build only, macOS only. They use macOS APIs (libproc's
# proc_pid_rusage, mach task_info) and the scripts use pmset/hdiutil/caffeinate, so on any other
# OS bench-reality prints a message and builds nothing. No test or CI target depends on it.
# Runs go through scripts/reality/run_safe.sh (AC power, free disk, swap, thermal, run time and
# a total write budget) with an explicit write cap.
UNAME_S ?= $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
build/bench/bench_reality: bench/bench_reality.cpp bench/reality_common.hpp $(HEADERS) Makefile
	@mkdir -p build/bench
	$(CXX) $(CXXSTD) $(WARN) -O2 $(INCLUDES) $(LDFLAGS) $< -o $@

build/bench/bench_crash: bench/bench_crash.cpp bench/reality_common.hpp $(HEADERS) Makefile
	@mkdir -p build/bench
	$(CXX) $(CXXSTD) $(WARN) -O2 $(INCLUDES) $(LDFLAGS) $< -o $@

bench-reality: check-deps build/bench/bench_reality build/bench/bench_crash
else
bench-reality:
	@echo "bench-reality: skipped on $(UNAME_S) (macOS only: uses libproc/mach APIs); nothing built"
endif

clean:
	rm -rf build

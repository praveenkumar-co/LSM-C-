# Build and run every tests/*.cpp in one of four modes. All modes use -Werror.
#   make test        optimized build
#   make asan        AddressSanitizer + UndefinedBehaviorSanitizer
#   make tsan        ThreadSanitizer (defines TSAN_BUILD)
#   make test-quick  optimized build with -DTEST_QUICK: smaller workloads, under 20 MiB written
#   make clean
#
# Each test binary prints "[Budget] Total bytes written by all stages: N ...". After a run, the
# Makefile adds up N over all binaries and fails if the sum is over the mode's limit
# (512 MiB for test/asan/tsan, 20 MiB for test-quick).

ifeq ($(origin CXX),default)
CXX := clang++
endif
CXXSTD   := -std=c++17
WARN     := -Wall -Wextra -Werror
INCLUDES := -Iinclude
LDFLAGS  := -pthread

TEST_SRCS := $(wildcard tests/*.cpp)
TEST_NAMES := $(notdir $(basename $(TEST_SRCS)))
HEADERS := $(wildcard include/vlog/*.hpp include/index/*.hpp tests/*.hpp)

FLAGS_test  := -O2
FLAGS_asan  := -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all
FLAGS_tsan  := -O1 -g -fsanitize=thread -DTSAN_BUILD
FLAGS_quick := -O2 -DTEST_QUICK

LIMIT_test  := 536870912
LIMIT_asan  := 536870912
LIMIT_tsan  := 536870912
LIMIT_quick := 20971520

.PHONY: test asan tsan quick test-quick clean check-deps

# vlog/ is the bottom layer: it must not depend on index/ or legacy/.
check-deps:
	@if grep -nE '#include *[<"](\.\./)*(index|legacy)/' include/vlog/*.hpp; then \
	  echo "check-deps: FAIL (include/vlog must not include index/ or legacy/)"; exit 1; \
	else echo "check-deps: OK (include/vlog has no index/ or legacy/ includes)"; fi

define MODE_RULES
build/$(1)/%: tests/%.cpp $(HEADERS) Makefile
	@mkdir -p build/$(1)
	$$(CXX) $$(CXXSTD) $$(WARN) $$(FLAGS_$(1)) $$(INCLUDES) $$(LDFLAGS) $$< -o $$@

$(1): check-deps $(addprefix build/$(1)/,$(TEST_NAMES))
	@total=0; for t in $(TEST_NAMES); do \
	  echo "=== [$(1)] $$$$t ==="; \
	  ./build/$(1)/$$$$t > build/$(1)/$$$$t.log 2>&1; rc=$$$$?; cat build/$(1)/$$$$t.log; \
	  if [ $$$$rc -ne 0 ]; then echo "=== [$(1)] $$$$t: FAILED (exit $$$$rc) ==="; exit $$$$rc; fi; \
	  echo "=== [$(1)] $$$$t: exit 0 ==="; \
	  b=$$$$(sed -n 's/^\[Budget\] Total bytes written by all stages: \([0-9]*\).*/\1/p' build/$(1)/$$$$t.log | tail -1); \
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

clean:
	rm -rf build

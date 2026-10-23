# Build and run every tests/*.cpp in one of three modes. All modes use -Werror.
#   make test   optimized build
#   make asan   AddressSanitizer + UndefinedBehaviorSanitizer
#   make tsan   ThreadSanitizer (defines TSAN_BUILD)
#   make clean

ifeq ($(origin CXX),default)
CXX := clang++
endif
CXXSTD   := -std=c++17
WARN     := -Wall -Wextra -Werror
INCLUDES := -Iinclude
LDFLAGS  := -pthread

TEST_SRCS := $(wildcard tests/*.cpp)
TEST_NAMES := $(notdir $(basename $(TEST_SRCS)))
HEADERS := $(wildcard include/vlog/*.hpp include/index/*.hpp)

FLAGS_test := -O2
FLAGS_asan := -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all
FLAGS_tsan := -O1 -g -fsanitize=thread -DTSAN_BUILD

.PHONY: test asan tsan clean check-deps

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
	@set -e; for t in $(TEST_NAMES); do \
	  echo "=== [$(1)] $$$$t ==="; ./build/$(1)/$$$$t; \
	  echo "=== [$(1)] $$$$t: exit 0 ==="; done
endef

$(eval $(call MODE_RULES,test))
$(eval $(call MODE_RULES,asan))
$(eval $(call MODE_RULES,tsan))

clean:
	rm -rf build

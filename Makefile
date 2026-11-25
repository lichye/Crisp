JOBS ?= 4
CMAKE ?= cmake
SMART := src/components/smartVerilog
SMART_DEPS_ROOT ?= $(SMART)/.deps
CVC5_ROOT ?= src/components/cvc5/packages/1.4.2
VEVAL := src/components/veval
HWCBMC := src/components/hw-cbmc
CBMC := $(HWCBMC)/lib/cbmc
SLANG := src/components/slang
VERSION := 0.1.0
FRONT_OBJECTS := build/front/parse.o build/front/prepare.o
APP_OBJECTS := build/main.o build/check/run.o $(FRONT_OBJECTS)

.PHONY: crisp ebmc test test-front test-goto-vv test-smart deps smart smart-deps veval slang
crisp: build/crisp

$(HWCBMC)/src/ebmc/Makefile:
	git submodule update --init -- $(HWCBMC)

$(CBMC)/src/Makefile: | $(HWCBMC)/src/ebmc/Makefile
	git -C $(HWCBMC) submodule update --init -- lib/cbmc

$(CBMC)/minisat-2.2.1/minisat/core/Solver.h: | $(CBMC)/src/Makefile
	$(MAKE) -C $(CBMC)/src minisat2-download

ebmc: $(HWCBMC)/src/ebmc/Makefile $(CBMC)/src/Makefile $(CBMC)/minisat-2.2.1/minisat/core/Solver.h
	+$(MAKE) -s -C $(HWCBMC)/src -j$(JOBS) ebmc.dir

build/crisp: $(APP_OBJECTS) ebmc
	+$(MAKE) -s -C $(HWCBMC)/src/ebmc -f Makefile -f "$(abspath src/check/ebmc.mk)" \
		CRISP_ROOT="$(CURDIR)" CRISP_OBJECTS="$(abspath $(APP_OBJECTS))" "$(abspath $@)"

build/main.o: CPPFLAGS += -DCRISP_VERSION='"$(VERSION)"'
build/check/run.o: CPPFLAGS += -I$(VEVAL)/src
build/%.o: src/%.cpp Makefile
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++17 -Wall -Wextra -Wpedantic -MMD -MP -Isrc -c $< -o $@

build/tests/front.o: tests/front/interface.cpp Makefile
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++17 -Wall -Wextra -Wpedantic -MMD -MP -Isrc -c $< -o $@

build/tests/test-front: build/tests/front.o $(FRONT_OBJECTS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test-front: build/crisp build/tests/test-front
	bash tests/front/run.sh

test: test-front
	+$(MAKE) -s -C $(HWCBMC)/src/ebmc -f Makefile -f "$(abspath src/check/ebmc.mk)" \
		CRISP_ROOT="$(CURDIR)" "$(abspath build/tests/test-witness)"
	build/tests/test-witness
	bash tests/cases/run.sh

test-goto-vv: build/crisp
	bash tests/goto-vv/run.sh

$(SLANG)/CMakeLists.txt:
	git submodule update --init -- $(SLANG)

# rtl-mode strict runs slang's command-line program, built from the v11.0 tag into $(SLANG)/build (spec 23).
slang: $(SLANG)/CMakeLists.txt
	$(CMAKE) -S "$(SLANG)" -B "$(SLANG)/build" -DCMAKE_BUILD_TYPE=Release -DSLANG_INCLUDE_TOOLS=ON \
		-DSLANG_INCLUDE_TESTS=OFF -DSLANG_INCLUDE_INSTALL=OFF -DSLANG_INCLUDE_PYLIB=OFF -DSLANG_INCLUDE_DOCS=OFF
	$(CMAKE) --build "$(SLANG)/build" --target slang_driver --parallel "$(JOBS)"

-include $(APP_OBJECTS:.o=.d) build/tests/front.d

deps: ebmc smart-deps
	SMART_DEPS_ROOT="$(abspath $(SMART_DEPS_ROOT))" CVC5_ROOT="$(abspath $(CVC5_ROOT))" "$(SMART)/install.sh" --deps-only

smart-deps:
	@if test -L "$(SMART_DEPS_ROOT)"; then unlink "$(SMART_DEPS_ROOT)"; fi
	SMART_DEPS_ROOT="$(abspath $(SMART_DEPS_ROOT))" CVC5_ROOT="$(abspath $(CVC5_ROOT))" "$(SMART)/install.sh" --build-deps

.PHONY: $(SMART)/build $(VEVAL)/build
$(SMART)/build $(VEVAL)/build:
	@if test -L "$@"; then unlink "$@"; fi; mkdir -p "$@"

smart: ebmc smart-deps | $(SMART)/build
	$(CMAKE) -S "$(SMART)" -B "$(SMART)/build" -DCMAKE_BUILD_TYPE=Release \
		-DHWCBMC_DIR="$(abspath $(HWCBMC))" -DSMART_DEPS_ROOT="$(abspath $(SMART_DEPS_ROOT))" \
		-DCVC5_ROOT="$(abspath $(CVC5_ROOT))"
	$(CMAKE) --build "$(SMART)/build" --target smart --parallel "$(JOBS)"

test-smart: smart crisp
	$(CMAKE) --build "$(SMART)/build" --target test_smart_ebmc --parallel "$(JOBS)"
	bash tests/smart/run.sh

veval: | $(VEVAL)/build $(CBMC)/src/Makefile
	$(CMAKE) -S "$(VEVAL)" -B "$(VEVAL)/build" -DCMAKE_BUILD_TYPE=Release -DCRISP_DRIVER_SOURCE_DIR:PATH=
	$(CMAKE) --build "$(VEVAL)/build" --parallel "$(JOBS)"

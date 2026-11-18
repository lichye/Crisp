JOBS ?= 4
CMAKE ?= cmake
SMART := components/smartVerilog
VEVAL := components/veval
HWCBMC := src/components/hw-cbmc
CBMC := $(HWCBMC)/lib/cbmc
VERSION := 0.1.0
FRONT_OBJECTS := build/front/parse.o build/front/prepare.o
APP_OBJECTS := build/main.o build/check/run.o $(FRONT_OBJECTS)

.PHONY: crisp ebmc test test-front deps smart veval
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
	bash tests/cases/run.sh

-include $(APP_OBJECTS:.o=.d) build/tests/front.d

deps: | $(SMART)/.deps $(SMART)/otherTools
	JOBS="$(JOBS)" ./$(SMART)/install.sh --deps-only

$(SMART)/build $(VEVAL)/build $(SMART)/.deps $(SMART)/otherTools:
	@cache=$$(mktemp -d "/var/tmp/crisp-$(notdir $(@D))-$(notdir $@).XXXXXXXX"); \
	if test -L "$@" && ! test -e "$@"; then unlink "$@"; fi; \
	if ! ln -sT "$$cache" "$@"; then rmdir "$$cache"; test -d "$@"; fi

smart: | $(SMART)/build
	$(CMAKE) -S "$(SMART)" -B "$(SMART)/build" -DCMAKE_BUILD_TYPE=Release $(if $(SMART_DEPS_ROOT),-DSMART_DEPS_ROOT="$(abspath $(SMART_DEPS_ROOT))") $(if $(CVC5_ROOT),-DCVC5_ROOT="$(abspath $(CVC5_ROOT))")
	$(CMAKE) --build "$(SMART)/build" --target smart --parallel "$(JOBS)"

veval: | $(VEVAL)/build
	$(CMAKE) -S "$(VEVAL)" -B "$(VEVAL)/build" -DCMAKE_BUILD_TYPE=Release -DCRISP_DRIVER_SOURCE_DIR:PATH=
	$(CMAKE) --build "$(VEVAL)/build" --parallel "$(JOBS)"

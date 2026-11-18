JOBS ?= 4
CMAKE ?= cmake
SMART := components/smartVerilog
VEVAL := components/veval
VERSION := 0.1.0
FRONT_OBJECTS := build/front/parse.o build/front/prepare.o

.PHONY: crisp test-front deps smart veval
crisp: build/crisp

build/crisp: build/main.o $(FRONT_OBJECTS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

build/main.o: CPPFLAGS += -DCRISP_VERSION='"$(VERSION)"'
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

-include build/main.d $(FRONT_OBJECTS:.o=.d) build/tests/front.d

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

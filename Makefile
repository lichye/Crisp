JOBS ?= 4
CMAKE ?= cmake
SMART := components/smartVerilog
VEVAL := components/veval

.PHONY: build deps smart veval binary release example test
build: smart veval binary

binary:
	JOBS="$(JOBS)" ./src/components/ebmc/build.sh

release: build
	./src/package.sh

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
	$(CMAKE) -S "$(VEVAL)" -B "$(VEVAL)/build" -DCMAKE_BUILD_TYPE=Release -DCRISP_DRIVER_SOURCE_DIR="$(abspath src)"
	$(CMAKE) --build "$(VEVAL)/build" --parallel "$(JOBS)"

example:
	./examples/token/reproduce.sh

test: veval binary
	$(CMAKE) --build "$(VEVAL)/build" --target crisp_verify_tests --parallel "$(JOBS)"
	./tests/verify.sh
	./tests/direct.sh

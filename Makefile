JOBS ?= 4
SMART := components/smartVerilog
VEVAL := components/veval

.PHONY: build deps smart veval example
build: smart veval

deps:
	JOBS="$(JOBS)" ./$(SMART)/install.sh --deps-only

smart:
	cmake -S "$(SMART)" -B "$(SMART)/build" -DCMAKE_BUILD_TYPE=Release $(if $(SMART_DEPS_ROOT),-DSMART_DEPS_ROOT="$(abspath $(SMART_DEPS_ROOT))") $(if $(CVC5_ROOT),-DCVC5_ROOT="$(abspath $(CVC5_ROOT))")
	cmake --build "$(SMART)/build" --target smart --parallel "$(JOBS)"

veval:
	cmake -S "$(VEVAL)" -B "$(VEVAL)/build" -DCMAKE_BUILD_TYPE=Release
	cmake --build "$(VEVAL)/build" --parallel "$(JOBS)"

example:
	python3 examples/token/reproduce.py

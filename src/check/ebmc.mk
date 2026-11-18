# Loaded after the fork's src/ebmc/Makefile, so the adapter has precisely
# the same CP_* flags, irep vocabulary and solver libraries as the engine.
CRISP_ADAPTER := $(CRISP_ROOT)/build/check/engine.o
$(CRISP_ADAPTER): $(CRISP_ROOT)/src/check/engine.cpp $(CRISP_ROOT)/src/check/ebmc.mk
	@mkdir -p "$(@D)"
	$(CXX) $(CP_CXXFLAGS) -I"$(CRISP_ROOT)/src" -c $< -o $@

-include $(CRISP_ADAPTER:.o=.d)

$(CRISP_ROOT)/build/crisp: $(CRISP_OBJECTS) $(CRISP_ADAPTER) $(filter-out main.o,$(OBJ))
	@set -e; staging=$$(mktemp -d "$(@D)/.link.XXXXXX"); \
	trap 'rm -rf -- "$$staging"' EXIT; \
	$(subst $@,"$$staging/crisp",$(LINKBIN)); mv -- "$$staging/crisp" "$@"

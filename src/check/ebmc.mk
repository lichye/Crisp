# Loaded after the fork's src/ebmc/Makefile, so the adapter has precisely
# the same CP_* flags, irep vocabulary and solver libraries as the engine.
CRISP_ADAPTER := $(addprefix $(CRISP_ROOT)/build/check/,engine.o vacuity.o bounded.o witness.o)
$(CRISP_ADAPTER): $(CRISP_ROOT)/build/check/%.o: $(CRISP_ROOT)/src/check/%.cpp $(CRISP_ROOT)/src/check/ebmc.mk
	@mkdir -p "$(@D)"
	$(CXX) $(CP_CXXFLAGS) -I"$(CRISP_ROOT)/src" -c $< -o $@

-include $(CRISP_ADAPTER:.o=.d)

# veval's lowering steps (spec 25), with the engine's flags and veval's headers.
CRISP_LOWER := $(CRISP_ROOT)/build/check/lower.o
$(CRISP_LOWER): $(CRISP_ROOT)/src/check/lower.cpp $(CRISP_ROOT)/src/check/ebmc.mk
	@mkdir -p "$(@D)"
	$(CXX) $(filter-out -Werror,$(CP_CXXFLAGS)) -I"$(CRISP_ROOT)/src" -I"$(CRISP_ROOT)/src/components/veval/src" -c $< -o $@

-include $(CRISP_LOWER:.o=.d)

# veval's lowering (spec 25), compiled with the engine's flags and linked
# against the engine's hw-cbmc libraries. veval's own CMake target
# hw_cbmc_frontend recompiles those hw-cbmc sources, so it is not used; of
# its three veval files, two are taken. The third, RunFrontendCommand.cpp,
# replaces CBMC's run(), which the engine uses to start solvers such as yices
# and Spot; Crisp keeps CBMC's run() for both the engine and veval. veval's code is not -Werror clean under the
# engine's warning flags, so warnings stay warnings here.
CRISP_VEVAL := $(CRISP_ROOT)/src/components/veval/src
CRISP_VEVAL_SOURCES := \
	frontend/rtl/ElaboratedDesign.cpp util/verilog/ExpressionPrinter.cpp \
	frontend/input/ReplaceDontCareConstants.cpp \
	frontend/sva/PropertyMonitor.cpp frontend/sva/LocalVariableMonitor.cpp \
	frontend/sva/ClockSampling.cpp frontend/sva/SafetyMonitor.cpp \
	lowering/Symbolic/BitRewriting.cpp lowering/Symbolic/PropertySubstitution.cpp \
	lowering/Symbolic/CombinationalDefinitions.cpp lowering/Symbolic/SignalAliases.cpp \
	lowering/connectivity/ConnectionMap.cpp lowering/environment/AssumptionDependencies.cpp \
	lowering/environment/CopyPortAssumptions.cpp lowering/output/WriteLocalSpecs.cpp \
	lowering/propagation/LandingEligibility.cpp lowering/propagation/PropertyLowering.cpp \
	lowering/connectivity/SignalReachability.cpp lowering/reports/ReachabilityReport.cpp \
	lowering/reports/ExportHierarchyGraph.cpp \
	util/database/Json.cpp \
	lowering/environment/LocalEnvironment.cpp lowering/LowerDesign.cpp util/database/SavedVerdicts.cpp \
	lowering/reports/VerdictReport.cpp \
	check/prepare/ReadLocalSpec.cpp \
	frontend/rtl/HwCbmcVerilogSynthesis.cpp util/verilog/ProductExpression.cpp
CRISP_VEVAL_OBJECTS := $(patsubst %.cpp,$(CRISP_ROOT)/build/veval/%.o,$(CRISP_VEVAL_SOURCES))
$(CRISP_ROOT)/build/veval/%.o: $(CRISP_VEVAL)/%.cpp $(CRISP_ROOT)/src/check/ebmc.mk
	@mkdir -p "$(@D)"
	$(CXX) $(filter-out -Werror,$(CP_CXXFLAGS)) -I"$(CRISP_VEVAL)" -c $< -o $@

-include $(CRISP_VEVAL_OBJECTS:.o=.d)

$(CRISP_ROOT)/build/crisp: $(CRISP_OBJECTS) $(CRISP_ADAPTER) $(CRISP_LOWER) $(CRISP_VEVAL_OBJECTS) $(filter-out main.o,$(OBJ))
	@set -e; staging=$$(mktemp -d "$(@D)/.link.XXXXXX"); \
	trap 'rm -rf -- "$$staging"' EXIT; \
	$(subst $@,"$$staging/crisp",$(LINKBIN)); mv -- "$$staging/crisp" "$@"

CRISP_WITNESS_TEST := $(CRISP_ROOT)/build/tests/witness.o
$(CRISP_WITNESS_TEST): $(CRISP_ROOT)/tests/cases/witness.cpp $(CRISP_ROOT)/src/check/ebmc.mk
	@mkdir -p "$(@D)"
	$(CXX) $(CP_CXXFLAGS) -I"$(CRISP_ROOT)/src" -c $< -o $@

-include $(CRISP_WITNESS_TEST:.o=.d)

$(CRISP_ROOT)/build/tests/test-witness: $(CRISP_WITNESS_TEST) $(filter-out main.o,$(OBJ))
	$(LINKBIN)

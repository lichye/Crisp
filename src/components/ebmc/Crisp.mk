# Read after the pinned fork's src/ebmc/Makefile. Reuse its exact engine objects
# and solver libraries, with Crisp's entry point in place of the upstream CLI.
crisp_direct.o crisp_command.o: CXXFLAGS += -I"$(CRISP_ROOT)/components/veval/src" -I"$(CRISP_ROOT)/components/veval/src/util/process"
crisp_direct.o: crisp_notices.inc
crisp: $(filter-out main.o,$(OBJ)) crisp_direct.o crisp_command.o
	$(LINKBIN)

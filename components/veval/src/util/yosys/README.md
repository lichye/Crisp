# Yosys integration

The Yosys passes, version compatibility and identifier conversion live here.
SBY harnesses and check execution live in [util/sby](../../util/sby/).

| Files | Responsibility |
|---|---|
| [YosysIdentifier.cpp](YosysIdentifier.cpp) | Encode/decode Yosys identifier spellings. |
| [YosysApiCompatibility.h](YosysApiCompatibility.h) | Handle Yosys API/version differences. |
| [AuditMissingEnvironmentPass.cpp](AuditMissingEnvironmentPass.cpp) | Report interface dependencies absent from a local check without modifying the design. |
| [LivenessToSafetyPass.cpp](LivenessToSafetyPass.cpp) | Encode liveness obligations using the existing liveness-to-safety construction. |
| [ExposePropertySignalsPass.cpp](ExposePropertySignalsPass.cpp) | Expose selected read-only property observations in Yosys. |
| [EncodeSignalRelationsPass.cpp](EncodeSignalRelationsPass.cpp) | Preserve mux semantics and encode checked same-frame signal relations in Yosys. |

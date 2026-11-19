# Local environments

Construct the reset, constant-wiring and propagated premises used at each
landing, and track which asserted copies justify assumptions. Saved annotations
live in [util/database](../../util/database/), and formatted verdict reports in
[reports/](../reports/).

| Files | Responsibility |
|---|---|
| [AssumptionDependencies.cpp](AssumptionDependencies.cpp) / [AssumptionDependencies.h](AssumptionDependencies.h) | Associate assumed copies with their asserting producers and saved results. |
| [CopyPortAssumptions.cpp](CopyPortAssumptions.cpp) / [CopyPortAssumptions.h](CopyPortAssumptions.h) | Emit independently checked copies of a source assumption at repeated whole input bindings. |
| [AssumptionSelection.h](AssumptionSelection.h) | Define which propagated premises a generated check receives. |
| [LocalEnvironment.cpp](LocalEnvironment.cpp) / [LocalEnvironment.h](LocalEnvironment.h) | Collect reset contracts, constant connections and recognized clamps for a local check. |

`LocalEnvironment::environment` gathers premises and records their stable
fingerprint inputs through one insertion path. Alias resolution and unsigned
clamp recognition do not expand sequential logic.

`buildAssumptionDependencies` pairs source sentences with saved results, creates
one record per assumed landing, and checks for circular support. Premise tiers
are computed once per instance with a decided result. This is annotation of
saved checks, not source-level lifting or a new solver invocation.

If `a` and `b` both connect to `x`, an authored `x < 8` assumption produces
both `a < 8` and `b < 8`. They keep the same source entry and environment
selection; the product gives them separate labels and monitor states. This
does not impose `a == b` on the isolated module. Only identical typed whole
input bindings qualify. Each additional copy changes one port; no Cartesian
product of multi-signal alternatives is generated. Reset contracts and
assertions are not duplicated by this rule.

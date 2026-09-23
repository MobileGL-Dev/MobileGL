# GL CTS multi-suite conformance report

| Backend | Suite | Expected | Result | Pass | Accepted | Crash | Hang | Unrun | Duplicate | Coverage | Strict Pass-only | Conformance-accepted | Validation |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|:---:|
| DirectVulkan | shader-image | 125 | 125 | 63 | 119 | 0 | 0 | 0 | 0 | 100.00% (125/125) | 50.40% (63/125) | 95.20% (119/125) | OK |
| DirectVulkan | ssbo | 124 | 124 | 124 | 124 | 0 | 0 | 0 | 0 | 100.00% (124/124) | 100.00% (124/124) | 100.00% (124/124) | OK |
| DirectVulkan | dsa | 371 | 371 | 368 | 369 | 0 | 0 | 0 | 0 | 100.00% (371/371) | 99.19% (368/371) | 99.46% (369/371) | OK |
| DirectVulkan | texture | 1055 | 1054 | 815 | 1036 | 0 | 0 | 1 | 0 | 99.91% (1054/1055) | 77.25% (815/1055) | 98.20% (1036/1055) | INCOMPLETE |
| DirectVulkan | packed-pixels | 4732 | 4732 | 4732 | 4732 | 0 | 0 | 0 | 0 | 100.00% (4732/4732) | 100.00% (4732/4732) | 100.00% (4732/4732) | OK |
| **DirectVulkan** | **DirectVulkan weighted subtotal** | **6407** | **6406** | **6102** | **6380** | **0** | **0** | **1** | **0** | **99.98% (6406/6407)** | **95.24% (6102/6407)** | **99.58% (6380/6407)** | **INCOMPLETE** |
| **All backends** | **Overall weighted** | **6407** | **6406** | **6102** | **6380** | **0** | **0** | **1** | **0** | **99.98% (6406/6407)** | **95.24% (6102/6407)** | **99.58% (6380/6407)** | **INCOMPLETE** |

Rates use unique **Expected** caselist cases as the denominator. Unrun cases remain in the denominator and are not accepted.
Accepted statuses: `Pass`, `NotSupported`, `QualityWarning`, `CompatibilityWarning`, `Waiver`.
Duplicate is the number of extra QPA observations; the final observation wins.

## Validation details

- **DirectVulkan/shader-image OK**: result directory has no run_state.json; backend provenance is unverified
- **DirectVulkan/ssbo OK**: result directory has no run_state.json; backend provenance is unverified
- **DirectVulkan/dsa OK**: result directory has no run_state.json; backend provenance is unverified
- **DirectVulkan/texture INCOMPLETE**: result directory has no run_state.json; backend provenance is unverified
- **DirectVulkan/packed-pixels OK**: result directory has no run_state.json; backend provenance is unverified

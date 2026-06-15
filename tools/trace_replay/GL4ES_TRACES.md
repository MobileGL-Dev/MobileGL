# GL4ES Trace Inventory

The GL4ES repository currently provides these apitrace fixtures under
`NG-GL4ES/NG-GL4ES/traces` with reference images under `NG-GL4ES/NG-GL4ES/refs`.
MobileGL's trace replay tests only use traces that exercise a core-profile style
OpenGL path. Compatibility/fixed-function traces are intentionally excluded.

| Trace | Status | Reason |
| --- | --- | --- |
| `openra` | Included | No fixed-function calls were detected in the trace scan, and it passes Linux replay on both `DirectGLES` and `DirectVulkan`. |
| `glsl_lighting` | Excluded | Uses compatibility matrix-stack calls (`glLoadIdentity`, `glPopMatrix`) and replays to a black image on MobileGL (`DirectGLES` mismatchPixels=30659 against the GL4ES reference). |
| `descent3` | Excluded | Uses fixed-function calls such as `glBegin`, `glEnd`, `glAlphaFunc`, `glOrtho`, and client arrays. |
| `foobillardplus` | Excluded | Uses display lists, matrix stack, material/fog state, and fixed-function client arrays. |
| `glxgears` | Excluded | Uses display lists, `glBegin`/`glEnd`, matrix stack, and material state. |
| `neverball` | Excluded | Uses display lists, `glBegin`/`glEnd`, fixed-function lighting/material state, clip planes, and matrix stack. |
| `pointsprite` | Excluded | Uses `glBegin`/`glEnd`, matrix stack, `glOrtho`, and fixed-function point state. |
| `stuntcarracer` | Excluded | Uses `glBegin`/`glEnd`, matrix stack, `glAlphaFunc`, and fixed-function client arrays. |

The scan is deliberately conservative: a trace is only added as a MobileGL
regression fixture after it replays and validates against its reference image on
Linux and Android. Traces that mainly test GL4ES compatibility behavior should
remain outside MobileGL core-profile CI.

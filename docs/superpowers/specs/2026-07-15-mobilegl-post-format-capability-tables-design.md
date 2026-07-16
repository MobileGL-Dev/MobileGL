# MobileGL POST Format Capability Tables

## Goal

Expose the format-capability results used during MobileGL backend startup in the Android plugin's driver POST screen. The screen must show the exact `Full`, `Caveat`, or `None` result for every backend, target, internal format, and capability without duplicating the backend's detection rules.

## Existing Architecture

- `DriverPost.cpp` probes the device GLES and Vulkan drivers before `MobileGL::Initialize()` and returns a `BackendPostReport` for each backend.
- `DriverPostJni.cpp` serializes those reports to JSON for `PostActivity`.
- `PostActivity` uses platform Android views and already supports collapsible check details and a collapsible raw report.
- Backend startup fills a `FormatCapabilityCache` in the DirectGLES and DirectVulkan `InitCapabilities()` paths. `FullCaps` takes precedence over `CaveatCaps`; an absent bit means `None`.
- The capability matrix contains 12 targets, 75 internal formats, and 14 capability columns per backend.

## Selected Approach

Extract callable format-probe entry points from the existing DirectGLES and DirectVulkan implementations. Backend startup and the POST will call these same functions, so their results cannot drift.

The POST will run each probe while its temporary driver resources are still valid:

- DirectGLES: after the GLES function table and capabilities have been populated, while the 1x1 pbuffer context is current.
- DirectVulkan: after selecting the physical device, while the Vulkan instance and physical device handles remain valid.

The resulting optional `FormatCapabilityCache` will be stored in each `BackendPostReport`. Failure to obtain a format table will not discard the existing POST checks or change their verdict; the UI will instead report that the format table is unavailable.

## JSON Contract

The JNI report will add an optional `formatCapabilities` object to each backend. To avoid repeating tens of thousands of status strings, the object will contain:

- one ordered capability-name array;
- one entry per target;
- one compact row per internal format containing the format name, a Full bitmask, and a Caveat bitmask.

Java resolves each cell in this order:

1. Full bit present: `Full`.
2. Otherwise Caveat bit present: `Caveat`.
3. Otherwise: `None`.

This preserves the backend's current precedence and keeps the raw JSON reasonably small.

## Android UI

Each backend section keeps its existing verdict, renderer string, and check table. A new `Format capabilities` subsection follows it.

- Each of the 11 texture targets and `Renderbuffer` is a separate, initially collapsed table.
- Target headers can be expanded independently.
- Table content is created on expansion and removed when collapsed, preventing the activity from retaining roughly 27,000 status views.
- Each expanded table is placed in a horizontal scroll container.
- The first column contains internal-format names. The remaining columns use the ordered capability names from the JSON report.
- Every status cell displays its status text and uses the conventional color mapping:
  - `Full`: green background with white text.
  - `Caveat`: yellow background with black text.
  - `None`: red background with white text.
- Header and format-name cells use neutral dark backgrounds consistent with the existing POST theme.
- The existing raw-report toggle remains available at the end of the screen.

## Performance and Lifecycle

- The existing single-flight native POST and cached JSON behavior remains unchanged.
- Format tables are lazily materialized and discarded on collapse.
- The JSON carries bitmasks rather than repeated `Full`, `Caveat`, and `None` strings.
- Existing configuration-change handling remains unchanged.

## Validation

1. Run focused source checks and `git diff --check`.
2. Build the Android plugin APK with the repository's current Gradle workflow.
3. If an Android target is connected, install the APK and open `PostActivity`.
4. Verify both backend sections, all target toggles, horizontal scrolling, visible cell text, and the green/yellow/red mapping.
5. Confirm collapsing a table removes its generated content and expanding it recreates the same values.

## Non-Goals

- Changing the meanings of `Full`, `Caveat`, or `None`.
- Changing POST verdict rules.
- Displaying sample-count vectors in this iteration.
- Replacing the existing platform-view UI with Compose, AppCompat, or WebView.

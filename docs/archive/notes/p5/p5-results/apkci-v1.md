# apkci v1 result

## Diagnosis

Run 35114525520 failed before compilation because `android-actions/setup-android@v4` drifted to cmdline-tools `14742923` (20.0) while its default `packages` input still requested the removed legacy `tools` package alongside `platform-tools`. Every `Setup Android SDK` step therefore failed at `sdkmanager tools`.

## `github/dev` check

`github/dev` did not contain a fix for this failure. Its recent workflow history was `5d51372c`, `bf376b23`, and `00a326ef`; the only `apk.yml` diff from the source branch removed the temporary `feat/disaggregated` push trigger. Nothing was ported from `dev`.

## Change

Commit `94ce2ee7` adds these inputs to all three `android-actions/setup-android@v4` steps in `.github/workflows/apk.yml`:

```yaml
packages: platform-tools
cmdline-tools-version: 13114758
```

`13114758` is cmdline-tools 19, the newest known-good release documented by the action from the supplied choices. The explicit package list drops only the missing legacy `tools` package. License handling, the temporary branch trigger, build targets, AVD setup, and retrace cases are unchanged. `.github/workflows/test.yml` does not use `android-actions/setup-android@v4`.

Diff stat:

```text
 .github/workflows/apk.yml | 6 ++++++
 1 file changed, 6 insertions(+)
```

## Validation

- Parsed `.github/workflows/apk.yml` successfully with `yaml.safe_load` using PyYAML in a temporary environment.
- `git diff --check` passed before commit.
- `actionlint` was not installed in WSL, so it was not run.
- Checked Google's current SDK repository metadata for `platform-tools`, `emulator`, `ndk;27.3.13750724`, `platforms;android-34`, `platforms;android-35`, `build-tools;34.0.0`, and `system-images;android-35;google_apis;x86_64`; all are present.
- Confirmed the committer identity was `Swung0x48 <swung0x48@outlook.com>` before committing.

## CI-only confirmation

The next GitHub Actions run must confirm that all three setup steps download cmdline-tools 19, install only `platform-tools`, proceed through license acceptance and package installation, and reach compilation. Emulator boot and retrace outcomes remain CI-dependent; isolated retrace failures are outside this repair.

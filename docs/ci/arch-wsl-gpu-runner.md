# MiniPC-8845 Arch WSL GPU Runner

Configuration recorded on 2026-10-05.

## Host

| Component | Configuration |
| --- | --- |
| Hostname | `MiniPC-8845` |
| Windows build | `10.0.26200.9445` |
| GPU | AMD Radeon 780M Graphics |
| Windows GPU driver | `32.0.12033.1030` |
| WSL version | `2.7.3.0` |
| Distribution | `archlinux`, Arch Linux on WSL 2 |
| Linux kernel | `6.6.114.1-microsoft-standard-WSL2` |
| Init system | systemd |
| WSL networking | Mirrored |
| GPU access | `/dev/dxg` and `/usr/lib/wsl/lib` |

## GitHub Actions Runner

| Setting | Value |
| --- | --- |
| Repository | `MobileGL-Dev/MobileGL` |
| Runner name | `minipc-8845-arch-wsl-gpu` |
| Runner version | `2.337.0`, automatic updates enabled |
| Labels | `self-hosted`, `Linux`, `X64`, `arch`, `wsl`, `gpu`, `gles`, `vulkan`, `d3d12` |
| Concurrent jobs | 1 |
| Linux account | `github-runner`, UID 1001, no sudo configuration |
| Installation directory | `/opt/actions-runner/mobilegl` |
| Work directory | `/opt/actions-runner/mobilegl/_work` |
| Environment file | `/etc/mobilegl-runner/gpu.env` |

Workflow runner selector:

```yaml
runs-on: [self-hosted, Linux, X64, arch, wsl, gpu, gles, vulkan]
```

## Graphics

| Component | Configuration |
| --- | --- |
| Mesa | `26.2.1-arch1.1` |
| OpenGL / GLES driver | Mesa D3D12 |
| Renderer | `D3D12 (AMD Radeon 780M Graphics)` |
| EGL | 1.5 |
| OpenGL | 4.6 |
| OpenGL ES | 3.1 |
| Vulkan driver | Mesa Dozen, translating Vulkan to D3D12 |
| Vulkan device API | 1.2.354 |
| Vulkan ICD | `/usr/share/vulkan/icd.d/dzn_icd.json` |
| Default EGL platform | Surfaceless |

Dozen reports a non-conformant Vulkan implementation. The exposed capabilities
are GLES 3.1 and Vulkan 1.2; GLES 3.2 and full Vulkan conformance are not available.

The runner service selects the Radeon GPU with these environment variables:

```sh
DISPLAY=:0
WAYLAND_DISPLAY=wayland-0
XDG_RUNTIME_DIR=/mnt/wslg/runtime-dir
LD_LIBRARY_PATH=/usr/lib/wsl/lib
MESA_LOADER_DRIVER_OVERRIDE=d3d12
GALLIUM_DRIVER=d3d12
MESA_D3D12_DEFAULT_ADAPTER_NAME="AMD Radeon 780M Graphics"
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/dzn_icd.json
EGL_PLATFORM=surfaceless
PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
```

The installed GPU probe is `/usr/local/libexec/mobilegl-runner/gpu-smoke`.
Its GLES source is `/usr/local/libexec/mobilegl-runner/gles-smoke.c`, and the
recorded verification log is `/var/log/mobilegl-runner/gpu-smoke.log`.

## Build Tools

Installed tools include GCC, Clang 22, CMake, Ninja, ccache, LLD, libc++, Git LFS,
GitHub CLI, and EGL/GLES/Vulkan development headers.

## Service and Startup

| Setting | Value |
| --- | --- |
| systemd unit | `actions.runner.MobileGL-Dev-MobileGL.minipc-8845-arch-wsl-gpu.service` |
| Service drop-in | `/etc/systemd/system/actions.runner.MobileGL-Dev-MobileGL.minipc-8845-arch-wsl-gpu.service.d/gpu.conf` |
| Service startup | Enabled at WSL systemd boot |
| Restart policy | `Restart=always`, `RestartSec=15` |
| Startup checks | GPU device access and WSLg X11/Wayland sockets |
| Windows scheduled task | `MobileGL Arch WSL Runner` |
| Task trigger | Windows login by `Swung0x48` |
| Task behavior | Starts WSL in a hidden window and keeps it running |
| Task execution limit | Unlimited |
| Task retry policy | Every minute, up to 999 retries |

Automatic startup requires the Windows user to log in. The runner cannot execute
jobs while the host is asleep.

## Network

The runner uses the Windows Clash proxy through WSL mirrored networking:

```sh
http_proxy=http://127.0.0.1:7897
https_proxy=http://127.0.0.1:7897
no_proxy=localhost,127.0.0.1,::1
```

Clash must be running for the runner's configured GitHub connection to work.

## Main Test workflow

CPU jobs do not query GPU availability. They default to GitHub `ubuntu-latest`.
Repository variable `CI_CPU_RUNNER` may specify a JSON runner label array, for example
`["self-hosted", "Linux", "X64", "cpu"]`. A self-hosted CPU runner needs Docker and
permission to run job containers. CPU builds/tests run in `ubuntu:24.04` on either
host type, with Clang 20 and the same userspace ABI. They do not require `/dev/dxg`.
GitHub Actions has no native "any hosted OR self-hosted" selector; the variable
chooses the CPU pool independently, without reserving this GPU machine.

After each runtime build completes, a small GitHub-hosted selection job queries
this repository's runner inventory for the corresponding GPU stage. Integration,
verify, split and retrace select independently. Benchmark uses the integration
selection. `integration-verify-split` has a separate CPU build and GPU test job.
Retrace compilation and fixture preparation no longer wait for integration tests.

An online 8845 runner is preferred even when busy. Offline, unregistered, or
mismatched labels select `ubuntu-latest`. Test failures never trigger fallback.
Configure Actions secret `RUNNER_STATUS_TOKEN` with repository Administration:
Read-only access. The secret is passed only to the selection jobs. A missing token,
expired credential or API failure is an error, not evidence that the runner is offline.

`prepare_test_runtime.py` runs when producing and consuming runtime artifacts. It
records/relocates workspace paths, resolves CMake module paths on the execution
host, and repairs escaped ENVIRONMENT/LABELS separators before GoogleTest discovery.
This prevents a backend value like `DirectVulkan;MOBILEGL_TRANSPORT=inproc;...`.
At test time it replaces the built-in ICD path and removes software-rendering
and Mesa version overrides on the hardware runner. Thus an Ubuntu build can be
tested on Arch WSL with Dozen/D3D12 without recompiling or using the build host's
software driver configuration. Capability failures remain visible test failures.

Each GPU stage samples availability after its inputs are ready. A disconnection
later still leaves already scheduled jobs queued: GitHub cannot migrate those
jobs automatically. Rerun the workflow to select again. Arch-built native artifacts
are not mixed with Ubuntu consumers; CPU artifacts use the common Ubuntu container.

Native GPU jobs use Arch's installed tools and do not install packages, provision
swap, download an SDK or change privileged core settings. The old standalone
`wsl-gpu-smoke.yml` has been removed; the host's installed probe remains available
for manual diagnostics.

// MobileGL - MobileGL/MG_Util/SelfTest/DriverPost.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

namespace MobileGL::MG_Util::SelfTest {
    // One row of a backend power-on self-test (POST) report.
    struct PostCheck {
        String name;
        String status; // "PASS" | "WARN" | "FAIL" | "INFO"
        String detail;
    };

    // Verdict for one backend's device driver.
    //   - UNSUPPORTED: a fatal check failed; the backend cannot run on this driver.
    //   - DEGRADED: every fatal check passed but at least one soft expectation is unmet.
    //   - OK: all expectations met.
    // available is false when no probeable driver exists at all (library missing, display
    // uninitializable, zero Vulkan physical devices, ...).
    struct BackendPostReport {
        Bool available = false;
        String verdict = "UNSUPPORTED"; // "OK" | "DEGRADED" | "UNSUPPORTED"
        String rendererInfo;
        Vector<PostCheck> checks;
    };

    // Probes the DEVICE GLES driver with self-contained EGL boilerplate (1x1 pbuffer
    // context on the default display). Runs in the plugin APK's own process before
    // MobileGL::Initialize(); it only touches MG_External tables and BackendLoader
    // functions, never MG_State.
    BackendPostReport RunGlesDriverPost();

    // Probes the DEVICE Vulkan driver through a dlopen'd loader and a throwaway
    // Vulkan 1.1 instance. Same standalone constraints as RunGlesDriverPost().
    BackendPostReport RunVulkanDriverPost();
} // namespace MobileGL::MG_Util::SelfTest

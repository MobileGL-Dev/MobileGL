// MobileGL - MobileGL/MG_Util/SelfTest/DriverPost.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include "DriverBugProbes.h"

#include <Includes.h>
#include <MG_Backend/BackendObject.h>

namespace MobileGL::MG_Util::SelfTest {
    // One row of a backend power-on self-test (POST) report.
    //
    // EVERY CAPABILITY ROW IS PASS, WARN OR FAIL; INFO IS FOR IDENTITY ONLY.
    //   PASS - the backend supports the capability directly.
    //   WARN - not directly, but a MobileGL quirk substitutes and the application still sees
    //          correct behaviour; the detail names the substitute and what it costs.
    //   FAIL - unsupported with no substitute; an application that uses it gets wrong output, a
    //          failed draw, or nothing.
    //   INFO - identity only: renderer name, API version, driver strings, and the strings
    //          MobileGL itself reports to applications. Never a capability answer.
    // A FAIL row does not by itself mean the backend cannot run - see BackendPostReport::verdict.
    struct PostCheck {
        String name;
        String status; // "PASS" | "WARN" | "FAIL" | "INFO"
        String detail;
        // Display ordering rank within a backend section (lower renders first): FAIL,
        // WARN, PASS, then the device-driver identity strings, then the strings
        // MobileGL itself reports to applications. Rows are stable-sorted by this rank
        // before the report is returned; it is not serialized to JSON.
        Int displayRank = 0;
    };

    // Verdict for one backend's device driver, derived from the rows.
    //   - UNSUPPORTED: a REQUIRED capability failed; the backend cannot run on this driver.
    //   - DEGRADED: every required capability is present, but at least one row is WARN or is a
    //     FAIL on an optional capability - the backend runs, and something an application might
    //     ask for is substituted or missing.
    //   - OK: every row passed.
    // So a section can carry FAIL rows and still be DEGRADED rather than UNSUPPORTED: a device
    // with no dual-source blend still runs. Which capabilities are required is decided at the
    // row (ReportBuilder::Fail vs ReportBuilder::FailOptional in DriverPost.cpp).
    // available is false when no probeable driver exists at all (library missing, display
    // uninitializable, zero Vulkan physical devices, ...).
    struct BackendPostReport {
        Bool available = false;
        String verdict = "UNSUPPORTED"; // "OK" | "DEGRADED" | "UNSUPPORTED"
        String rendererInfo;
        Vector<PostCheck> checks;
        // The "Known Driver Bugs" section, kept apart from `checks` on purpose. `checks` asks
        // whether a feature is there and roughly works; these are core features the driver
        // claims, accepts, and then does not perform - a separate question, from a separate
        // inventory (campaign findings, not the extension string). See DriverBugProbes.h.
        //
        // Only bugs this device ACTUALLY HAS appear here: a probe that comes back clean
        // contributes no entry, so an unaffected driver renders the section empty rather than
        // as a list of reassurances. That is also why the verdict vocabulary is FIXED /
        // UNFIXABLE rather than PASS / FAIL - every row is a bug that is present, and the
        // verdict says whether MobileGL can do anything about it.
        Vector<DriverBugFinding> knownDriverBugs;
        Optional<MG_Backend::FormatCapabilityCache> formatCapabilities;
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

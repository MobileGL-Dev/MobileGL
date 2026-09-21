// MobileGL - MobileGL/MG_Remote/Server/ServerEntry.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The server image's main(). Package `sm`.
//
// Deliberately the smallest thing that can be one: everything it could do
// belongs to mobilegl_server_main, which lives IN THE LIBRARY so that one
// shared library serves both roles (ARCHITECTURE.md:496). Keeping this file
// empty of logic is what stops the server growing a second, divergent copy of
// bring-up that the client half never executes.

extern "C" int mobilegl_server_main(int argc, char** argv);

int main(int argc, char** argv) { return mobilegl_server_main(argc, argv); }

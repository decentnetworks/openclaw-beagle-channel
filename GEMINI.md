# Project: OpenClaw Beagle Channel

This repository contains the Beagle Chat integration for OpenClaw, consisting of a C++ Sidecar (bridge to Elastos Carrier P2P) and a TypeScript Channel (OpenClaw extension).

## Architectural Guidelines
- **Sidecar Pattern:** The `beagle-sidecar` (C++) is the source of truth for P2P connectivity. It exposes an HTTP API (default port 39091) for the `beagle-channel`.
- **Hybrid Codebase:** 
  - C++ (packages/beagle-sidecar): Native SDK bridge.
  - TypeScript (packages/beagle-channel): OpenClaw plugin logic.
- **SDK Modes:** 
  - **Stub Mode:** Use `-DBEAGLE_SDK_STUB=ON` during CMake to build without the full Elastos Carrier SDK. This is preferred for UI/routing testing.
  - **Real Mode:** Requires `BEAGLE_SDK_ROOT` to point to the built Elastos Carrier Native SDK.

## Development Standards
- **C++:** Follow idiomatic C++11/14. Use `log_line` for debugging. Ensure any new SDK methods are also implemented in the stub block in `beagle_sdk.cpp`.
- **TypeScript:** Use functional patterns and strict typing. 
- **Dependencies:** Always check `INSTALL.md` before making changes to the build system or adding new system-level dependencies.

## Deployment & Installation
- Local testing should use `install.sh` to sync the plugin to `~/.openclaw/extensions/beagle/`.
- Do not commit the `build/` directories or compiled binaries.
- Ensure `BEAGLE_SDK_ROOT` is exported when building in real mode.

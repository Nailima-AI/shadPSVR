# PSVR through OpenXR (experimental fork)

Drives PSVR (PS4) titles with any OpenXR headset, e.g. SteamVR. Off by default.

## Layout

| Piece | Language | Role |
| --- | --- | --- |
| `src/xr/psvr_xr_math` | Rust (staticlib, C ABI) | Pose/FOV/eye-offset math, recentering. Unit tested with `cargo test`. |
| `src/core/xr/openxr_runtime.*` | C++ | OpenXR instance + session on shadPS4's own Vulkan device, frame loop thread, tracking snapshots. |
| `src/core/xr/openxr_runtime_stub.cpp` | C++ | Used when `ENABLE_OPENXR=OFF`; everything reports "no headset". |
| `libSceHmd`, `libSceVrTracker` | C++ | Report a connected headset and return real poses, FOV and eye offsets. |

## Build and run

```
cmake -B build -DENABLE_OPENXR=ON
cmake --build build
SHADPS4_OPENXR=1 ./build/shadps4 <game>
```

Needs `cargo` on PATH. Start SteamVR (or another OpenXR runtime) first.

## Status

- [x] Step 1: OpenXR session bound to shadPS4's Vulkan device, head pose streaming.
- [x] Step 2 (first pass): `sceHmdGetDeviceInformation`, `sceHmdGetFieldOfView`,
      `sceHmdGet2DEyeOffset`, `sceVrTrackerGetResult` (HMD) backed by OpenXR.
- [ ] Step 3: eye-buffer submit (`sceHmdReprojection*` → OpenXR swapchain), and finding out
      whether titles pre-distort.
- [ ] Step 4: PS Move ↔ SteamVR controllers.

## Known gaps

- Nothing is shown in the headset yet: the frame loop ends every frame with zero layers.
- The Orbis tracking origin is the head position at session start (or last recenter). Real
  PSVR positions are relative to the PS Camera; some titles may expect an offset.
- The PSVR camera (`sceCamera*`) is still unimplemented; titles that gate on it will stall.

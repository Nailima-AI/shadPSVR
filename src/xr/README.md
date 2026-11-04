# PSVR through OpenXR (experimental fork)

Drives PSVR (PS4) titles with any OpenXR headset, e.g. SteamVR. Off by default.

## Layout

| Piece | Language | Role |
| --- | --- | --- |
| `src/xr/psvr_xr_math` | Rust (staticlib, C ABI) | Pose/FOV/eye-offset math, recentering, eye-texture FOV blocks, PS Move frame and IMU. `cargo test`. |
| `src/core/xr/openxr_runtime.*` | C++ | OpenXR instance + session on shadPS4's Vulkan device, frame loop, controller actions, haptics, eye swapchains, projection layer. |
| `src/core/xr/openxr_runtime_stub.cpp` | C++ | Used when `ENABLE_OPENXR=OFF`; everything reports "no headset". |
| `src/video_core/renderer_vulkan/vk_presenter_xr.cpp` | C++ | Draws the title's eye textures into the OpenXR swapchains. |
| `libSceHmd`, `libSceVrTracker`, `libSceMove`, `libSceCamera` | C++ | HLE backed by the above. |

## Build and run

```
cmake -B build -DENABLE_OPENXR=ON
cmake --build build
SHADPS4_OPENXR=1 ./build/shadps4 <game>
```

Needs `cargo` and Clang 19+. Start SteamVR (or another OpenXR runtime) first.

## Status

- [x] Head tracking: `sceHmdGetDeviceInformation`, `sceHmdGetFieldOfView`, `sceHmdGet2DEyeOffset`,
      `sceVrTrackerGetResult` (HMD).
- [x] Display: `sceHmdReprojectionStart` / `StartWithOverlay` eye textures → OpenXR projection
      layer, rendered with the pose the title used. The runtime does reprojection and lens
      distortion. User events registered with `SetUserEventStart/End` fire once per headset frame.
- [x] PS Move: two SteamVR controllers as Move 0 (right hand) and Move 1 (left hand), buttons,
      trigger, IMU, rumble, and sphere poses through `sceVrTrackerGetResult`.
- [x] DualShock 4 pose (experimental): midpoint of the two controllers.
- [x] Camera: a virtual PlayStation Camera when no host camera is selected; webcam or real PS
      Camera passthrough otherwise. See [CAMERA.md](CAMERA.md).
- [ ] `sceHmdReprojectionStart2dVr` (cinematic 2D screen) and multilayer submits: logged, not shown.
- [ ] Social screen on a separate VideoOut bus (PS VR Worlds asserts in `sceVideoOutOpen`).

## Controller mapping

| Move | Index | Touch | Vive |
| --- | --- | --- | --- |
| Cross / Circle | A / B | A,X / B,Y | – |
| Square | trackpad press | thumbstick click | – |
| Triangle | grip squeeze | grip | grip |
| Move button | thumbstick click | – | trackpad click |
| T | trigger | trigger | trigger |
| Start / Select | – | left menu / – | left menu / right menu |

Rebind in SteamVR's controller settings for the "psvr" action set.

## Known gaps and guesses

- Untested on a commercial title. Verified only against Monado's simulated headset.
- The Orbis origin is the head position at session start (or last recenter). Real PSVR positions
  are relative to the PS Camera; some titles may expect an offset.
- The frame thread holds the Vulkan submit lock across `xrBeginFrame` / `xrEndFrame`, as
  XR_KHR_vulkan_enable requires for queue access. If SteamVR blocks inside `xrEndFrame`, emulator
  GPU submits wait with it: profile there first if the frame rate drops.
- `sceVrTrackerGetResult` ignores `prediction_time` and returns the pose OpenXR predicted for the
  next headset frame. Display stays correct because each frame is handed to SteamVR with the pose
  it was rendered with, so its reprojection removes the difference.
- `label` in the reprojection frame parameters is never written. If a title waits on it, it will
  stall.
- Move sphere offset (9 cm from the grip) and the Move axis convention (+Y along the shaft) come
  from psmoveapi and PS3 libgem; PS4 may differ.
- Struct layouts for the reprojection calls come from ALVR-PS4's hardware notes
  (github.com/nicolas-riera/ALVR-PS4, `docs/psvr-technical-reference.md`).

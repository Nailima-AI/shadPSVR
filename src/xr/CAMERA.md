# PlayStation Camera and PSVR

## How PSVR uses the camera

- The PS4 Camera (CUH-ZEY1/ZEY2) is two 1280x800 sensors behind an OV580 bridge. It offers
  1280x800 @ 60, 640x400 @ 120 and 320x192 @ 240 fps, raw or YUV422, over a USB 3.0 based AUX plug.
- The camera tracks the headset's 9 LEDs and the Move/DS4 light bars, and the headset IMU supplies
  rotation. VR titles refuse to start without a camera ("connect the PlayStation Camera"), most
  likely through `sceCameraIsAttached` or a `NOT_CONNECTED` error.
- Per frame a title roughly does `sceCameraGetFrameData` → `sceVrTrackerGpuSubmit` (which embeds
  the whole `OrbisCameraFrameData`) → `GpuWait` / `CpuProcess` → `sceVrTrackerGetResult`. Titles
  with social or AR features also read the pixels themselves.

## Spoof (what this fork does)

With `SHADPS4_OPENXR=1` and no host camera selected (`camera_id = -1`):

- `sceCameraIsAttached` = 1, `sceCameraGetDeviceInfo` OK, `Open` / `SetConfig` / `Start` OK.
- `sceCameraGetFrameData` returns neutral black YUV422 (Y=16, U=V=128) for both sensors at every
  requested format level (1280x800, 640x400, 320x200, 160x100) with an increasing frame counter
  and process-time timestamps.
- VrTracker ignores the frames: poses come from OpenXR, and `GpuSubmit` / `GpuWait` succeed.

Not yet handled: `libScePadTracker` / `libSceMoveTracker` are LLE system modules in shadPS4.
They would only see black frames, so titles that track through them instead of VrTracker get
no pose.

## Webcam passthrough

Select a camera with `camera_id` (index into SDL3's camera list). The frame path now:

- converts whatever SDL delivers to YUY2, honours the row pitch, and scales to 1280x800;
- copies a mono webcam into both sensors (zero disparity);
- fills format levels 1–3 (YUV422/Y16/Y8) by downscaling;
- writes `meta.frame`, `meta.timestamp` and `deviceTimestamp`.

## Real PS4 Camera on a PC

1. Wire the AUX plug to USB 3.0 (DIY mod or Sony's CFI-ZAA1 PS5 adaptor).
2. Load its firmware after every power-up: OrbisEyeCam (Windows) or `ps4eye_init.py` (Linux).
   It then enumerates as "USB Camera-OV580" (05a9:058a), a UVC YUY2 device at 3448x808.
3. Set `camera_id` to it. The fork detects the 3448x808 mode, skips the 96-byte row header, and
   splits each row into the left and right 1280-pixel images. It ignores the 8 extra rows.

The PS5 HD Camera also needs a firmware loader and is not a PSVR tracking camera.

## Emulator support elsewhere

- Upstream shadPS4: SDL3 webcam passthrough (PR 3952) for level 0 only. VR was closed as not
  planned (issue 1663).
- fpPS4: stubs only (`IsAttached` = 0).

## Sources

- github.com/psxdev/PS4EYECam, github.com/psxdev/OrbisEyeCam, github.com/longjie/ps4eye
- psdevwiki.com/ps4/Aux, psdevwiki.com/ps4/Talk:PlayStation_4_Camera
- github.com/shadps4-emu/shadPS4/pull/3952, /pull/4937, /issues/1663
- github.com/red-prig/fpPS4 (`ps4_libscecamera.pas`)
- wiki.libsdl.org/SDL3/SDL_OpenCamera

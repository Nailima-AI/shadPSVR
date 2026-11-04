// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/elf_info.h"
#include "common/logging/log.h"
#include "core/emulator_settings.h"
#include "core/libraries/camera/camera.h"
#include "core/libraries/camera/camera_helpers.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/kernel/memory.h"
#include "core/libraries/kernel/process.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/libs.h"
#include "core/memory.h"

#include <array>
#include <optional>
#include <utility>
#include <vector>

#include <thread>
#include "SDL3/SDL_camera.h"

namespace Libraries::Camera {

bool g_library_opened = false;
s32 g_firmware_version = 0;

static s32 g_handles = 0;
static constexpr s32 c_width = 1280, c_height = 800;

static u16 *raw16_buffer1{}, *raw16_buffer2{};
static u8 *raw8_buffer1{}, *raw8_buffer2{};

static constexpr u32 c_scaled_levels = ORBIS_CAMERA_MAX_FORMAT_LEVEL_NUM - 1;
static constexpr std::array<std::pair<u32, u32>, c_scaled_levels> c_scaled_sizes{{
    {c_width / 2, c_height / 2},
    {c_width / 4, c_height / 4},
    {c_width / 8, c_height / 8},
}};
static u8* scaled_buffers[ORBIS_CAMERA_MAX_DEVICE_NUM][c_scaled_levels]{};
static constexpr size_t c_yuy2_frame_size = static_cast<size_t>(c_width) * c_height * 2;
static std::array<std::vector<u8>, ORBIS_CAMERA_MAX_DEVICE_NUM> yuy2_frames{
    std::vector<u8>(c_yuy2_frame_size), std::vector<u8>(c_yuy2_frame_size)};
static bool g_host_is_ps_camera = false;
static bool g_frame_content_changed = false;

// A PlayStation Camera with its firmware loaded shows up as one packed YUY2 frame per pair:
// a 96 byte row header, then the left and right 1280 pixel images, then padding.
static constexpr u32 c_ps_camera_width = 3448;
static constexpr u32 c_ps_camera_height = 808;
static constexpr u32 c_ps_camera_row_header = 96;
static std::vector<u8> yuy2_scaled(c_yuy2_frame_size / 4);
static u32 g_format_levels[ORBIS_CAMERA_MAX_DEVICE_NUM]{1, 1};
static u64 g_frame_counter = 0;
static u64 g_frame_timestamp = 0;
static constexpr u64 c_virtual_frame_interval_us = 1'000'000 / 60;

struct OutputLayout {
    OrbisCameraFormat formats[ORBIS_CAMERA_MAX_DEVICE_NUM];
    u32 levels[ORBIS_CAMERA_MAX_DEVICE_NUM];

    bool operator==(const OutputLayout& other) const {
        return std::memcmp(this, &other, sizeof(OutputLayout)) == 0;
    }
};
static std::optional<OutputLayout> g_converted_layout;

SDL_Camera* sdl_camera = nullptr;
OrbisCameraConfigExtention output_config0, output_config1;

s32 PS4_SYSV_ABI sceCameraAccGetData() {
    LOG_ERROR(Lib_Camera, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceCameraChangeAppModuleState() {
    LOG_ERROR(Lib_Camera, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceCameraClose(s32 handle) {
    LOG_INFO(Lib_Camera, "called, handle: {}", handle);
    if (handle < 1) {
        return ORBIS_CAMERA_ERROR_PARAM;
    }
    if (!g_library_opened) {
        return ORBIS_CAMERA_ERROR_NOT_OPEN;
    }

    // Decrement handles on close.
    // If no handles remain, then the library itself is considered closed.
    if (--g_handles == 0) {
        g_library_opened = false;
    }

    if (sdl_camera) {
        SDL_CloseCamera(sdl_camera);
        sdl_camera = nullptr;
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceCameraCloseByHandle(s32 handle) {
    LOG_DEBUG(Lib_Camera, "called");
    if (handle < 1) {
        return ORBIS_CAMERA_ERROR_PARAM;
    }
    if (!g_library_opened) {
        return ORBIS_CAMERA_ERROR_NOT_OPEN;
    }

    // Decrement handles on close.
    // If no handles remain, then the library itself is considered closed.
    if (--g_handles == 0) {
        g_library_opened = false;
    }
    return ORBIS_OK;
}

static bool AcquireHostFrame() {
    Uint64 timestamp_ns = 0;
    SDL_Surface* frame = SDL_AcquireCameraFrame(sdl_camera, &timestamp_ns);
    if (!frame) {
        return false;
    }
    SDL_Surface* converted = frame->format == SDL_PIXELFORMAT_YUY2
                                 ? frame
                                 : SDL_ConvertSurface(frame, SDL_PIXELFORMAT_YUY2);
    if (converted) {
        const auto* pixels = static_cast<const u8*>(converted->pixels);
        if (g_host_is_ps_camera && converted->w == static_cast<int>(c_ps_camera_width)) {
            for (s32 device = 0; device < ORBIS_CAMERA_MAX_DEVICE_NUM; ++device) {
                ScaleYUY2(pixels + c_ps_camera_row_header + device * c_width * 2, converted->pitch,
                          c_width, c_height, yuy2_frames[device].data(), c_width, c_height);
            }
        } else {
            ScaleYUY2(pixels, converted->pitch, converted->w, converted->h, yuy2_frames[0].data(),
                      c_width, c_height);
            yuy2_frames[1] = yuy2_frames[0];
        }
        if (converted != frame) {
            SDL_DestroySurface(converted);
        }
    }
    SDL_ReleaseCameraFrame(sdl_camera, frame);
    g_frame_content_changed |= converted != nullptr;
    return converted != nullptr;
}

static void* BaseLevelBuffer(OrbisCameraBaseFormat format, u16* raw16, u8* raw8) {
    switch (format) {
    case ORBIS_CAMERA_FORMAT_YUV422:
    case ORBIS_CAMERA_FORMAT_RAW16:
        return raw16;
    case ORBIS_CAMERA_FORMAT_RAW8:
        return raw8;
    default:
        return nullptr;
    }
}

static void WriteBaseLevel(const std::vector<u8>& yuy2_frame, OrbisCameraBaseFormat format,
                           void* out) {
    switch (format) {
    case ORBIS_CAMERA_FORMAT_YUV422:
        std::memcpy(out, yuy2_frame.data(), c_yuy2_frame_size);
        break;
    case ORBIS_CAMERA_FORMAT_RAW16:
        ConvertYUY2ToRAW16(yuy2_frame.data(), static_cast<u16*>(out), c_width, c_height);
        break;
    case ORBIS_CAMERA_FORMAT_RAW8:
        ConvertYUY2ToRAW8(yuy2_frame.data(), static_cast<u8*>(out), c_width, c_height);
        break;
    default:
        break;
    }
}

static void WriteScaledLevel(const std::vector<u8>& yuy2_frame, OrbisCameraScaleFormat format,
                             u32 width, u32 height, u8* dst) {
    ScaleYUY2(yuy2_frame.data(), c_width * 2, c_width, c_height, yuy2_scaled.data(), width, height);
    switch (format) {
    case ORBIS_CAMERA_SCALE_FORMAT_YUV422:
        std::memcpy(dst, yuy2_scaled.data(), width * height * 2);
        break;
    case ORBIS_CAMERA_SCALE_FORMAT_Y16:
        ConvertYUY2ToY16(yuy2_scaled.data(), reinterpret_cast<u16*>(dst), width, height);
        break;
    case ORBIS_CAMERA_SCALE_FORMAT_Y8:
        ConvertYUY2ToY8(yuy2_scaled.data(), dst, width, height);
        break;
    default:
        break;
    }
}

s32 PS4_SYSV_ABI sceCameraGetFrameData(s32 handle, OrbisCameraFrameData* frame_data) {
    if (frame_data == nullptr) {
        return ORBIS_CAMERA_ERROR_PARAM;
    }
    LOG_DEBUG(Lib_Camera, "called, read mode: {:#x}", frame_data->readMode);
    frame_data->status[0] = -1;
    frame_data->status[1] = -1;
    if (handle < 1 || frame_data->sizeThis > 584) {
        return ORBIS_CAMERA_ERROR_PARAM;
    }
    const CameraBackend backend = GetCameraBackend();
    if (!g_library_opened || (backend == CameraBackend::Host && !sdl_camera)) {
        return ORBIS_CAMERA_ERROR_NOT_OPEN;
    }
    if (backend == CameraBackend::Disconnected) {
        return ORBIS_CAMERA_ERROR_NOT_CONNECTED;
    }

    const u64 now = Libraries::Kernel::sceKernelGetProcessTime();
    const bool virtual_frame_due =
        backend == CameraBackend::Virtual &&
        (g_frame_counter == 0 || now - g_frame_timestamp >= c_virtual_frame_interval_us);
    const bool new_frame =
        virtual_frame_due || (backend == CameraBackend::Host && AcquireHostFrame());
    if (new_frame) {
        ++g_frame_counter;
        g_frame_timestamp = now;
    }

    const OrbisCameraConfigExtention* configs[ORBIS_CAMERA_MAX_DEVICE_NUM] = {&output_config0,
                                                                              &output_config1};
    const OutputLayout layout{
        .formats = {output_config0.format, output_config1.format},
        .levels = {g_format_levels[0], g_format_levels[1]},
    };
    const bool convert =
        g_frame_content_changed || !g_converted_layout || *g_converted_layout != layout;
    g_converted_layout = layout;
    g_frame_content_changed = false;
    const bool mono = !g_host_is_ps_camera || backend == CameraBackend::Virtual;
    u16* raw16_buffers[ORBIS_CAMERA_MAX_DEVICE_NUM] = {raw16_buffer1, raw16_buffer2};
    u8* raw8_buffers[ORBIS_CAMERA_MAX_DEVICE_NUM] = {raw8_buffer1, raw8_buffer2};
    for (s32 device = 0; device < ORBIS_CAMERA_MAX_DEVICE_NUM; ++device) {
        const auto& format = configs[device]->format;
        void* base =
            BaseLevelBuffer(format.formatLevel0, raw16_buffers[device], raw8_buffers[device]);
        const bool copy_device0 = mono && device == 1 &&
                                  std::memcmp(&format, &configs[0]->format, sizeof(format)) == 0 &&
                                  g_format_levels[1] == g_format_levels[0];
        const u32 base_size = c_width * c_height * SizeOfBaseFormat(format.formatLevel0);
        if (convert && base) {
            if (copy_device0) {
                std::memcpy(base, frame_data->pFramePointerList[0][0], base_size);
            } else {
                WriteBaseLevel(yuy2_frames[device], format.formatLevel0, base);
            }
        }
        frame_data->pFramePointerList[device][0] = base;
        frame_data->framePosition[device][0] = {0, 0, c_width, c_height};
        frame_data->frameSize[device][0] = base_size;
        frame_data->meta.format[device][0] = format.formatLevel0;

        const OrbisCameraScaleFormat scaled_formats[c_scaled_levels] = {
            format.formatLevel1, format.formatLevel2, format.formatLevel3};
        for (u32 level = 0; level < c_scaled_levels; ++level) {
            const auto [width, height] = c_scaled_sizes[level];
            const OrbisCameraScaleFormat scaled = scaled_formats[level];
            const bool requested = (g_format_levels[device] & (2u << level)) != 0;
            if (!requested || SizeOfScaleFormat(scaled) == 0) {
                frame_data->pFramePointerList[device][level + 1] = nullptr;
                frame_data->frameSize[device][level + 1] = 0;
                continue;
            }
            if (convert && copy_device0) {
                std::memcpy(scaled_buffers[1][level], scaled_buffers[0][level],
                            width * height * SizeOfScaleFormat(scaled));
            } else if (convert) {
                WriteScaledLevel(yuy2_frames[device], scaled, width, height,
                                 scaled_buffers[device][level]);
            }
            frame_data->pFramePointerList[device][level + 1] = scaled_buffers[device][level];
            frame_data->framePosition[device][level + 1] = {0, 0, width, height};
            frame_data->frameSize[device][level + 1] = width * height * SizeOfScaleFormat(scaled);
            frame_data->meta.format[device][level + 1] = scaled;
        }
        frame_data->meta.frame[device] = g_frame_counter;
        frame_data->meta.timestamp[device] = g_frame_timestamp;
        frame_data->meta.deviceTimestamp[device] = static_cast<u32>(g_frame_timestamp);
        frame_data->status[device] = new_frame ? 1 : -1;
    }

    // on older firmwares, this wasn't present, and the original library also checks struct size
    // instead of the SDK version, and without this check, we'd smash the stack in those games
    if (frame_data->sizeThis == 584) {
        for (s32 device = 0; device < ORBIS_CAMERA_MAX_DEVICE_NUM; ++device) {
            for (s32 level = 0; level < ORBIS_CAMERA_MAX_FORMAT_LEVEL_NUM; ++level) {
                frame_data->pFramePointerListGarlic[device][level] =
                    frame_data->pFramePointerList[device][level];
            }
        }
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceCameraIsAttached(s32 index) {
    static bool first_log = true;
    if (first_log) {
        LOG_INFO(Lib_Camera, "called");
        first_log = false;
    } else {
        LOG_DEBUG(Lib_Camera, "called");
    }
    if (index != 0) {
        return ORBIS_CAMERA_ERROR_PARAM;
    }
    // 0 = disconnected, 1 = connected
    return IsCameraConnected() ? 1 : 0;
}

s32 PS4_SYSV_ABI sceCameraIsConfigChangeDone() {
    LOG_ERROR(Lib_Camera, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceCameraIsValidFrameData(s32 handle, OrbisCameraFrameData* frame_data) {
    LOG_DEBUG(Lib_Camera, "called");
    if (handle < 1 || frame_data == nullptr || frame_data->sizeThis > 584) {
        return ORBIS_CAMERA_ERROR_PARAM;
    }
    if (!g_library_opened) {
        return ORBIS_CAMERA_ERROR_NOT_OPEN;
    }

    return frame_data->status[0] == 1 && frame_data->status[1] == 1;
}

s32 PS4_SYSV_ABI sceCameraOpen(Libraries::UserService::OrbisUserServiceUserId user_id, s32 type,
                               s32 index, OrbisCameraOpenParameter* param) {
    LOG_INFO(Lib_Camera, "called");
    if (user_id != Libraries::UserService::ORBIS_USER_SERVICE_USER_ID_SYSTEM || type != 0 ||
        index != 0) {
        LOG_ERROR(Lib_Camera, "ORBIS_CAMERA_ERROR_PARAM");
        return ORBIS_CAMERA_ERROR_PARAM;
    }

    static bool buffers_initialized = false;

    if (!buffers_initialized) {
        constexpr s32 camera_system_mem_size = 0xfd0000;
        void* camera_garlic_pool = (void*)0xfd0000000;

        s32 ret = Libraries::Kernel::sceKernelMapNamedSystemFlexibleMemory(
            &camera_garlic_pool, camera_system_mem_size,
            (s32)(Core::MemoryProt::CpuReadWrite | Core::MemoryProt::GpuRead), 0,
            "SceCameraGpuGarlicPool");
        ASSERT(ret == ORBIS_OK);

        constexpr s32 raw8_buffer_size = c_width * c_height * sizeof(u8);
        constexpr s32 raw16_buffer_size = c_width * c_height * sizeof(u16);

        u8* remaining_camera_buf = (u8*)camera_garlic_pool;

        raw8_buffer1 = remaining_camera_buf;
        remaining_camera_buf += raw8_buffer_size;
        raw8_buffer2 = remaining_camera_buf;
        remaining_camera_buf += raw8_buffer_size;
        raw16_buffer1 = (u16*)remaining_camera_buf;
        remaining_camera_buf += raw16_buffer_size;
        raw16_buffer2 = (u16*)remaining_camera_buf;
        remaining_camera_buf += raw16_buffer_size;
        for (auto& device_buffers : scaled_buffers) {
            for (u32 level = 0; level < c_scaled_levels; ++level) {
                device_buffers[level] = remaining_camera_buf;
                remaining_camera_buf +=
                    c_scaled_sizes[level].first * c_scaled_sizes[level].second * sizeof(u16);
            }
        }

        ASSERT(remaining_camera_buf <= (u8*)camera_garlic_pool + camera_system_mem_size);

        ASSERT(Core::Memory::Instance()->IsValidGpuMapping((VAddr)camera_garlic_pool,
                                                           camera_system_mem_size));
        for (auto& frame : yuy2_frames) {
            FillBlackYUY2(frame.data(), c_width, c_height);
        }
        buffers_initialized = true;
    }
    g_frame_counter = 0;
    g_converted_layout.reset();
    g_frame_content_changed = false;

    g_library_opened = true;
    return ++g_handles;
}

s32 PS4_SYSV_ABI sceCameraOpenByModuleId() {
    LOG_ERROR(Lib_Camera, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceCameraSetAppModuleFocus() {
    LOG_ERROR(Lib_Camera, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceCameraRemoveAppModuleFocus() {
    LOG_ERROR(Lib_Camera, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceCameraStart(s32 handle, OrbisCameraStartParameter* param) {
    LOG_INFO(Lib_Camera, "called");
    if (handle < 1 || param == nullptr || param->sizeThis != sizeof(OrbisCameraStartParameter)) {
        LOG_ERROR(Lib_Camera, "ORBIS_CAMERA_ERROR_PARAM");
        return ORBIS_CAMERA_ERROR_PARAM;
    }
    if (!g_library_opened) {
        LOG_ERROR(Lib_Camera, "ORBIS_CAMERA_ERROR_NOT_OPEN");
        return ORBIS_CAMERA_ERROR_NOT_OPEN;
    }
    if (g_firmware_version >= Common::ElfInfo::FW_250 &&
        (param->formatLevel[0] > 0xf || param->formatLevel[1] > 0xf ||
         (param->formatLevel[0] | param->formatLevel[1]) == 0)) {
        LOG_ERROR(Lib_Camera, "ORBIS_CAMERA_ERROR_FORMAT_UNKNOWN");
        return ORBIS_CAMERA_ERROR_FORMAT_UNKNOWN;
    }

    g_format_levels[0] = param->formatLevel[0];
    g_format_levels[1] = param->formatLevel[1];

    switch (GetCameraBackend()) {
    case CameraBackend::Disconnected:
        return ORBIS_CAMERA_ERROR_NOT_CONNECTED;
    case CameraBackend::Virtual:
        LOG_INFO(Lib_Camera, "No host camera selected, using a virtual PlayStation Camera");
        return ORBIS_OK;
    case CameraBackend::Host:
        break;
    }

    SDL_CameraID* devices = NULL;
    int devcount = 0;
    devices = SDL_GetCameras(&devcount);
    if (devices == NULL) {
        LOG_ERROR(Lib_Camera, "Couldn't enumerate camera devices: {}", SDL_GetError());
        return ORBIS_CAMERA_ERROR_FATAL;
    } else if (devcount <= EmulatorSettings.GetCameraId()) {
        LOG_INFO(Lib_Camera, "Camera {} is not connected ({} found)",
                 EmulatorSettings.GetCameraId(), devcount);
        SDL_free(devices);
        return ORBIS_CAMERA_ERROR_NOT_CONNECTED;
    }
    SDL_CameraSpec cam_spec{};
    cam_spec.format = SDL_PIXELFORMAT_YUY2; // to be swizzled
    cam_spec.height = c_height;
    cam_spec.width = c_width;
    cam_spec.framerate_numerator = 60;
    cam_spec.framerate_denominator = 1;
    g_host_is_ps_camera = false;
    s32 format_count = 0;
    if (SDL_CameraSpec** formats =
            SDL_GetCameraSupportedFormats(devices[EmulatorSettings.GetCameraId()], &format_count)) {
        for (s32 i = 0; i < format_count; ++i) {
            if (formats[i]->width == static_cast<int>(c_ps_camera_width) &&
                formats[i]->height == static_cast<int>(c_ps_camera_height)) {
                g_host_is_ps_camera = true;
                cam_spec.width = c_ps_camera_width;
                cam_spec.height = c_ps_camera_height;
                LOG_INFO(Lib_Camera, "Host camera is a PlayStation Camera, using both sensors");
                break;
            }
        }
        SDL_free(formats);
    }
    sdl_camera = SDL_OpenCamera(devices[EmulatorSettings.GetCameraId()], &cam_spec);

    if (!sdl_camera) {
        LOG_ERROR(Lib_Camera, "Failed to open camera: {}", SDL_GetError());
        return ORBIS_CAMERA_ERROR_FATAL;
    }

    LOG_INFO(Lib_Camera, "SDL backend in use: {}", SDL_GetCurrentCameraDriver());
    char const* camera_name = SDL_GetCameraName(devices[EmulatorSettings.GetCameraId()]);
    if (camera_name)
        LOG_INFO(Lib_Camera, "SDL camera name: {}", camera_name);
    SDL_CameraSpec spec;
    SDL_GetCameraFormat(sdl_camera, &spec);
    LOG_INFO(Lib_Camera, "SDL camera format: {:#x}", std::to_underlying(spec.format));
    LOG_INFO(Lib_Camera, "SDL camera framerate: {}",
             (float)spec.framerate_numerator / (float)spec.framerate_denominator);
    LOG_INFO(Lib_Camera, "SDL camera dimensions: {}x{}", spec.width, spec.height);

    SDL_free(devices);

    // "warm up" the device, as recommended by SDL
    u64 timestamp;
    SDL_Surface* frame = nullptr;
    frame = SDL_AcquireCameraFrame(sdl_camera, &timestamp);
    if (!frame) {
        for (int i = 0; i < 1000; i++) {
            frame = SDL_AcquireCameraFrame(sdl_camera, &timestamp);
            if (frame) {
                SDL_ReleaseCameraFrame(sdl_camera, frame);
                break;
            }
            std::this_thread::sleep_for(std::chrono::nanoseconds(10));
        }
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceCameraStartByHandle(s32 handle, OrbisCameraStartParameter* param) {
    LOG_DEBUG(Lib_Camera, "called");
    if (handle < 1 || param == nullptr || param->sizeThis != sizeof(OrbisCameraStartParameter)) {
        return ORBIS_CAMERA_ERROR_PARAM;
    }
    if (!g_library_opened) {
        return ORBIS_CAMERA_ERROR_NOT_OPEN;
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceCameraStop(s32 handle) {
    LOG_INFO(Lib_Camera, "called, handle: {}", handle);
    if (handle < 1) {
        return ORBIS_CAMERA_ERROR_PARAM;
    }
    if (!g_library_opened) {
        return ORBIS_CAMERA_ERROR_NOT_OPEN;
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceCameraStopByHandle(s32 handle) {
    LOG_DEBUG(Lib_Camera, "called");
    return sceCameraStop(handle);
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    Libraries::Kernel::sceKernelGetCompiledSdkVersion(&g_firmware_version);

    LIB_FUNCTION("QhjrPkRPUZQ", "libSceCamera", 1, "libSceCamera", sceCameraAccGetData);
    LIB_FUNCTION("UFonL7xopFM", "libSceCamera", 1, "libSceCamera", sceCameraAudioClose);
    LIB_FUNCTION("fkZE7Hup2ro", "libSceCamera", 1, "libSceCamera", sceCameraAudioGetData);
    LIB_FUNCTION("hftC5A1C8OQ", "libSceCamera", 1, "libSceCamera", sceCameraAudioGetData2);
    LIB_FUNCTION("DhqqFiBU+6g", "libSceCamera", 1, "libSceCamera", sceCameraAudioOpen);
    LIB_FUNCTION("wyU98EXAYxU", "libSceCamera", 1, "libSceCamera", sceCameraAudioReset);
    LIB_FUNCTION("Y0pCDajzkVQ", "libSceCamera", 1, "libSceCamera", sceCameraChangeAppModuleState);
    LIB_FUNCTION("OMS9LlcrvBo", "libSceCamera", 1, "libSceCamera", sceCameraClose);
    LIB_FUNCTION("ztqH5qNTpTk", "libSceCamera", 1, "libSceCamera", sceCameraCloseByHandle);
    LIB_FUNCTION("nBH6i2s4Glc", "libSceCamera", 1, "libSceCamera", sceCameraDeviceOpen);
    LIB_FUNCTION("0btIPD5hg5A", "libSceCamera", 1, "libSceCamera", sceCameraGetAttribute);
    LIB_FUNCTION("oEi6vM-3E2c", "libSceCamera", 1, "libSceCamera", sceCameraGetAutoExposureGain);
    LIB_FUNCTION("qTPRMh4eY60", "libSceCamera", 1, "libSceCamera", sceCameraGetAutoWhiteBalance);
    LIB_FUNCTION("hHA1frlMxYE", "libSceCamera", 1, "libSceCamera", sceCameraGetCalibData);
    LIB_FUNCTION("5Oie5RArfWs", "libSceCamera", 1, "libSceCamera", sceCameraGetCalibDataFromDevice);
    LIB_FUNCTION("RHYJ7GKOSMg", "libSceCamera", 1, "libSceCamera", sceCameraGetCalibrationData);
    LIB_FUNCTION("ZaqmGEtYuL0", "libSceCamera", 1, "libSceCamera", sceCameraGetConfig);
    LIB_FUNCTION("a5xFueMZIMs", "libSceCamera", 1, "libSceCamera", sceCameraGetContrast);
    LIB_FUNCTION("tslCukqFE+E", "libSceCamera", 1, "libSceCamera",
                 sceCameraGetDefectivePixelCancellation);
    LIB_FUNCTION("DSOLCrc3Kh8", "libSceCamera", 1, "libSceCamera", sceCameraGetDeviceConfig);
    LIB_FUNCTION("n+rFeP1XXyM", "libSceCamera", 1, "libSceCamera",
                 sceCameraGetDeviceConfigWithoutHandle);
    LIB_FUNCTION("jTJCdyv9GLU", "libSceCamera", 1, "libSceCamera", sceCameraGetDeviceID);
    LIB_FUNCTION("-H3UwGQvNZI", "libSceCamera", 1, "libSceCamera", sceCameraGetDeviceIDWithoutOpen);
    LIB_FUNCTION("WZpxnSAM-ds", "libSceCamera", 1, "libSceCamera", sceCameraGetDeviceInfo);
    LIB_FUNCTION("ObIste7hqdk", "libSceCamera", 1, "libSceCamera", sceCameraGetExposureGain);
    LIB_FUNCTION("mxgMmR+1Kr0", "libSceCamera", 1, "libSceCamera", sceCameraGetFrameData);
    LIB_FUNCTION("WVox2rwGuSc", "libSceCamera", 1, "libSceCamera", sceCameraGetGamma);
    LIB_FUNCTION("zrIUDKZx0iE", "libSceCamera", 1, "libSceCamera", sceCameraGetHue);
    LIB_FUNCTION("XqYRHc4aw3w", "libSceCamera", 1, "libSceCamera", sceCameraGetLensCorrection);
    LIB_FUNCTION("B260o9pSzM8", "libSceCamera", 1, "libSceCamera", sceCameraGetMmapConnectedCount);
    LIB_FUNCTION("ULxbwqiYYuU", "libSceCamera", 1, "libSceCamera", sceCameraGetProductInfo);
    LIB_FUNCTION("olojYZKYiYs", "libSceCamera", 1, "libSceCamera", sceCameraGetRegister);
    LIB_FUNCTION("hawKak+Auw4", "libSceCamera", 1, "libSceCamera", sceCameraGetRegistryInfo);
    LIB_FUNCTION("RTDOsWWqdME", "libSceCamera", 1, "libSceCamera", sceCameraGetSaturation);
    LIB_FUNCTION("c6Fp9M1EXXc", "libSceCamera", 1, "libSceCamera", sceCameraGetSharpness);
    LIB_FUNCTION("IAz2HgZQWzE", "libSceCamera", 1, "libSceCamera", sceCameraGetVrCaptureInfo);
    LIB_FUNCTION("HX5524E5tMY", "libSceCamera", 1, "libSceCamera", sceCameraGetWhiteBalance);
    LIB_FUNCTION("0wnf2a60FqI", "libSceCamera", 1, "libSceCamera",
                 sceCameraInitializeRegistryCalibData);
    LIB_FUNCTION("p6n3Npi3YY4", "libSceCamera", 1, "libSceCamera", sceCameraIsAttached);
    LIB_FUNCTION("wQfd7kfRZvo", "libSceCamera", 1, "libSceCamera", sceCameraIsConfigChangeDone);
    LIB_FUNCTION("U3BVwQl2R5Q", "libSceCamera", 1, "libSceCamera", sceCameraIsValidFrameData);
    LIB_FUNCTION("BHn83xrF92E", "libSceCamera", 1, "libSceCamera", sceCameraOpen);
    LIB_FUNCTION("eTywOSWsEiI", "libSceCamera", 1, "libSceCamera", sceCameraOpenByModuleId);
    LIB_FUNCTION("py8p6kZcHmA", "libSceCamera", 1, "libSceCamera", sceCameraRemoveAppModuleFocus);
    LIB_FUNCTION("j5isFVIlZLk", "libSceCamera", 1, "libSceCamera", sceCameraSetAppModuleFocus);
    LIB_FUNCTION("doPlf33ab-U", "libSceCamera", 1, "libSceCamera", sceCameraSetAttribute);
    LIB_FUNCTION("96F7zp1Xo+k", "libSceCamera", 1, "libSceCamera", sceCameraSetAttributeInternal);
    LIB_FUNCTION("yfSdswDaElo", "libSceCamera", 1, "libSceCamera", sceCameraSetAutoExposureGain);
    LIB_FUNCTION("zIKL4kZleuc", "libSceCamera", 1, "libSceCamera", sceCameraSetAutoWhiteBalance);
    LIB_FUNCTION("LEMk5cTHKEA", "libSceCamera", 1, "libSceCamera", sceCameraSetCalibData);
    LIB_FUNCTION("VQ+5kAqsE2Q", "libSceCamera", 1, "libSceCamera", sceCameraSetConfig);
    LIB_FUNCTION("9+SNhbctk64", "libSceCamera", 1, "libSceCamera", sceCameraSetConfigInternal);
    LIB_FUNCTION("3i5MEzrC1pg", "libSceCamera", 1, "libSceCamera", sceCameraSetContrast);
    LIB_FUNCTION("vejouEusC7g", "libSceCamera", 1, "libSceCamera", sceCameraSetDebugStop);
    LIB_FUNCTION("jMv40y2A23g", "libSceCamera", 1, "libSceCamera",
                 sceCameraSetDefectivePixelCancellation);
    LIB_FUNCTION("vER3cIMBHqI", "libSceCamera", 1, "libSceCamera",
                 sceCameraSetDefectivePixelCancellationInternal);
    LIB_FUNCTION("wgBMXJJA6K4", "libSceCamera", 1, "libSceCamera", sceCameraSetExposureGain);
    LIB_FUNCTION("jeTpU0MqKU0", "libSceCamera", 1, "libSceCamera", sceCameraSetForceActivate);
    LIB_FUNCTION("lhEIsHzB8r4", "libSceCamera", 1, "libSceCamera", sceCameraSetGamma);
    LIB_FUNCTION("QI8GVJUy2ZY", "libSceCamera", 1, "libSceCamera", sceCameraSetHue);
    LIB_FUNCTION("K7W7H4ZRwbc", "libSceCamera", 1, "libSceCamera", sceCameraSetLensCorrection);
    LIB_FUNCTION("eHa3vhGu2rQ", "libSceCamera", 1, "libSceCamera",
                 sceCameraSetLensCorrectionInternal);
    LIB_FUNCTION("lS0tM6n+Q5E", "libSceCamera", 1, "libSceCamera", sceCameraSetProcessFocus);
    LIB_FUNCTION("NVITuK83Z7o", "libSceCamera", 1, "libSceCamera",
                 sceCameraSetProcessFocusByHandle);
    LIB_FUNCTION("8MjO05qk5hA", "libSceCamera", 1, "libSceCamera", sceCameraSetRegister);
    LIB_FUNCTION("bSKEi2PzzXI", "libSceCamera", 1, "libSceCamera", sceCameraSetSaturation);
    LIB_FUNCTION("P-7MVfzvpsM", "libSceCamera", 1, "libSceCamera", sceCameraSetSharpness);
    LIB_FUNCTION("3VJOpzKoIeM", "libSceCamera", 1, "libSceCamera", sceCameraSetTrackerMode);
    LIB_FUNCTION("nnR7KAIDPv8", "libSceCamera", 1, "libSceCamera", sceCameraSetUacModeInternal);
    LIB_FUNCTION("wpeyFwJ+UEI", "libSceCamera", 1, "libSceCamera", sceCameraSetVideoSync);
    LIB_FUNCTION("8WtmqmE4edw", "libSceCamera", 1, "libSceCamera", sceCameraSetVideoSyncInternal);
    LIB_FUNCTION("k3zPIcgFNv0", "libSceCamera", 1, "libSceCamera", sceCameraSetWhiteBalance);
    LIB_FUNCTION("9EpRYMy7rHU", "libSceCamera", 1, "libSceCamera", sceCameraStart);
    LIB_FUNCTION("cLxF1QtHch0", "libSceCamera", 1, "libSceCamera", sceCameraStartByHandle);
    LIB_FUNCTION("2G2C0nmd++M", "libSceCamera", 1, "libSceCamera", sceCameraStop);
    LIB_FUNCTION("+X1Kgnn3bzg", "libSceCamera", 1, "libSceCamera", sceCameraStopByHandle);
};

} // namespace Libraries::Camera
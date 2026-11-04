// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include <vector>

#include "camera_helpers.h"
#include "core/emulator_settings.h"
#include "core/xr/openxr_runtime.h"

namespace Libraries::Camera {

CameraBackend GetCameraBackend() {
    if (EmulatorSettings.GetCameraId() != -1) {
        return CameraBackend::Host;
    }
    if (Core::Xr::Runtime::Instance().IsRequested()) {
        return CameraBackend::Virtual;
    }
    return CameraBackend::Disconnected;
}

bool IsCameraConnected() {
    return GetCameraBackend() != CameraBackend::Disconnected;
}

void ScaleYUY2(const u8* src, u32 src_pitch, u32 src_width, u32 src_height, u8* dst, u32 dst_width,
               u32 dst_height) {
    const u32 dst_pairs = dst_width / 2;
    if (src_width < 2 || src_height == 0) {
        FillBlackYUY2(dst, dst_width, dst_height);
        return;
    }
    const bool same_width = (src_width & ~1u) == (dst_width & ~1u);

    // Per output pair: the source macropixel holding the first pixel's chroma, and the byte
    // offsets of both output pixels' luma.
    struct Column {
        u32 chroma;
        u32 luma0;
        u32 luma1;
    };
    thread_local std::vector<Column> columns;
    if (!same_width) {
        columns.resize(dst_pairs);
        for (u32 pair = 0; pair < dst_pairs; ++pair) {
            const u32 x0 = static_cast<u32>(u64{pair * 2} * src_width / dst_width);
            const u32 x1 = static_cast<u32>(u64{pair * 2 + 1} * src_width / dst_width);
            columns[pair] = {(x0 & ~1u) * 2, x0 * 2, x1 * 2};
        }
    }
    for (u32 y = 0; y < dst_height; ++y) {
        const u8* src_row = src + u64{y} * src_height / dst_height * src_pitch;
        u8* dst_row = dst + u64{y} * dst_width * 2;
        if (same_width) {
            std::memcpy(dst_row, src_row, dst_pairs * 4);
            continue;
        }
        for (u32 pair = 0; pair < dst_pairs; ++pair) {
            const Column& column = columns[pair];
            u32 macropixel;
            std::memcpy(&macropixel, src_row + column.chroma, sizeof(macropixel));
            macropixel = (macropixel & 0xFF00FF00u) | src_row[column.luma0] |
                         (u32{src_row[column.luma1]} << 16);
            std::memcpy(dst_row + pair * 4, &macropixel, sizeof(macropixel));
        }
    }
}

void FillBlackYUY2(u8* dst, u32 width, u32 height) {
    const u64 pairs = static_cast<u64>(width / 2) * height;
    for (u64 i = 0; i < pairs; ++i) {
        dst[i * 4 + 0] = 16;
        dst[i * 4 + 1] = 128;
        dst[i * 4 + 2] = 16;
        dst[i * 4 + 3] = 128;
    }
}

static inline u8 ClampU8(int v) {
    return static_cast<u8>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static inline u16 ClampU16(int v) {
    return static_cast<u16>(v < 0 ? 0 : (v > 65535 ? 65535 : v));
}

static inline u8 RgbaToLuma(const u8* px) {
    const u8 b = px[0];
    const u8 g = px[1];
    const u8 r = px[2];
    return ClampU8((77 * r + 150 * g + 29 * b) >> 8);
}

void ConvertRGBA8888ToRAW16(const u8* src, u16* dst, int width, int height) {
    for (int y = 0; y < height; ++y) {
        const u8* row = src + static_cast<u64>(y) * width * 4;
        u16* outRow = dst + static_cast<u64>(y) * width;
        const bool evenRow = (y & 1) == 0;
        for (int x = 0; x < width; ++x) {
            const u8* px = row + x * 4;
            const u16 b = u16(px[1]) << 4;
            const u16 g = u16(px[2]) << 4;
            const u16 r = u16(px[3]) << 4;
            const bool evenCol = (x & 1) == 0;
            if (evenRow && evenCol)
                outRow[x] = b;
            else if (evenRow && !evenCol)
                outRow[x] = g;
            else if (!evenRow && evenCol)
                outRow[x] = g;
            else
                outRow[x] = r;
        }
    }
}

void ConvertRGBA8888ToRAW8(const u8* src, u8* dst, int width, int height) {
    for (int y = 0; y < height; ++y) {
        const u8* row = src + static_cast<u64>(y) * width * 4;
        u8* outRow = dst + static_cast<u64>(y) * width;
        const bool evenRow = (y & 1) == 0;
        for (int x = 0; x < width; ++x) {
            const u8* px = row + x * 4;
            const u8 b = px[1];
            const u8 g = px[2];
            const u8 r = px[3];
            const bool evenCol = (x & 1) == 0;
            if (evenRow && evenCol)
                outRow[x] = b;
            else if (evenRow && !evenCol)
                outRow[x] = g;
            else if (!evenRow && evenCol)
                outRow[x] = g;
            else
                outRow[x] = r;
        }
    }
}

void ConvertRGBA8888ToY8(const u8* src, u8* dst, int width, int height) {
    for (int y = 0; y < height; ++y) {
        const u8* row = src + static_cast<u64>(y) * width * 4;
        u8* outRow = dst + static_cast<u64>(y) * width;
        for (int x = 0; x < width; ++x) {
            outRow[x] = RgbaToLuma(row + x * 4);
        }
    }
}

void ConvertRGBA8888ToY16(const u8* src, u16* dst, int width, int height) {
    for (int y = 0; y < height; ++y) {
        const u8* row = src + static_cast<u64>(y) * width * 4;
        u16* outRow = dst + static_cast<u64>(y) * width;
        for (int x = 0; x < width; ++x) {
            outRow[x] = static_cast<u16>(RgbaToLuma(row + x * 4)) << 2;
        }
    }
}

void ConvertYUY2ToY8(const u8* src, u8* dst, int width, int height) {
    for (int y = 0; y < height; ++y) {
        const u8* row = src + static_cast<u64>(y) * width * 2;
        u8* outRow = dst + static_cast<u64>(y) * width;
        for (int x = 0; x < width; x += 2) {
            outRow[x] = row[x * 2 + 0];     // Y0
            outRow[x + 1] = row[x * 2 + 2]; // Y1
        }
    }
}

void ConvertYUY2ToY16(const u8* src, u16* dst, int width, int height) {
    for (int y = 0; y < height; ++y) {
        const u8* row = src + static_cast<u64>(y) * width * 2;
        u16* outRow = dst + static_cast<u64>(y) * width;
        for (int x = 0; x < width; x += 2) {
            outRow[x] = static_cast<u16>(row[x * 2 + 0]) << 2;
            outRow[x + 1] = static_cast<u16>(row[x * 2 + 2]) << 2;
        }
    }
}

static inline void YuvToRgb(u8 y, u8 u, u8 v, u8& r, u8& g, u8& b) {
    const int c = static_cast<int>(y) - 16;
    const int d = static_cast<int>(u) - 128;
    const int e = static_cast<int>(v) - 128;

    r = ClampU8((298 * c + 409 * e + 128) >> 8);
    g = ClampU8((298 * c - 100 * d - 208 * e + 128) >> 8);
    b = ClampU8((298 * c + 516 * d + 128) >> 8);
}

void ConvertYUY2ToRAW8(const u8* src, u8* dst, int width, int height) {
    for (int y = 0; y < height; ++y) {
        const u8* row = src + static_cast<u64>(y) * width * 2;
        u8* outRow = dst + static_cast<u64>(y) * width;
        const bool evenRow = (y & 1) == 0;

        for (int x = 0; x < width; x += 2) {
            const u8 y0 = row[x * 2 + 0];
            const u8 u = row[x * 2 + 1];
            const u8 y1 = row[x * 2 + 2];
            const u8 v = row[x * 2 + 3];

            u8 r0, g0, b0;
            u8 r1, g1, b1;
            YuvToRgb(y0, u, v, r0, g0, b0);
            YuvToRgb(y1, u, v, r1, g1, b1);

            if (evenRow) {
                outRow[x] = b0;
                outRow[x + 1] = g1;
            } else {
                outRow[x] = g0;
                outRow[x + 1] = r1;
            }
        }
    }
}

void ConvertYUY2ToRAW16(const u8* src, u16* dst, int width, int height) {
    for (int y = 0; y < height; ++y) {
        const u8* row = src + static_cast<u64>(y) * width * 2;
        u16* outRow = dst + static_cast<u64>(y) * width;
        const bool evenRow = (y & 1) == 0;

        for (int x = 0; x < width; x += 2) {
            const u8 y0 = row[x * 2 + 0];
            const u8 u = row[x * 2 + 1];
            const u8 y1 = row[x * 2 + 2];
            const u8 v = row[x * 2 + 3];

            u8 r0, g0, b0;
            u8 r1, g1, b1;
            YuvToRgb(y0, u, v, r0, g0, b0);
            YuvToRgb(y1, u, v, r1, g1, b1);

            if (evenRow) {
                outRow[x] = static_cast<u16>(b0) << 4;
                outRow[x + 1] = static_cast<u16>(g1) << 4;
            } else {
                outRow[x] = static_cast<u16>(g0) << 4;
                outRow[x + 1] = static_cast<u16>(r1) << 4;
            }
        }
    }
}

s32 SizeOfBaseFormat(OrbisCameraBaseFormat f) {
    switch (f) {
    case ORBIS_CAMERA_FORMAT_YUV422:
        return 2;
    case ORBIS_CAMERA_FORMAT_RAW16:
        return 2;
    case ORBIS_CAMERA_FORMAT_RAW8:
        return 1;
    case ORBIS_CAMERA_FORMAT_NO_USE:
    default:
        return 0;
    }
}

s32 SizeOfScaleFormat(OrbisCameraScaleFormat f) {
    switch (f) {
    case ORBIS_CAMERA_SCALE_FORMAT_YUV422:
        return 2;
    case ORBIS_CAMERA_SCALE_FORMAT_Y16:
        return 2;
    case ORBIS_CAMERA_SCALE_FORMAT_Y8:
        return 1;
    case ORBIS_CAMERA_SCALE_FORMAT_NO_USE:
    default:
        return 0;
    }
}
} // namespace Libraries::Camera
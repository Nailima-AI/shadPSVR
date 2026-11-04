// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging/log.h"
#include "shader_recompiler/resource.h"
#include "video_core/renderer_vulkan/vk_presenter.h"

namespace Vulkan {

static bool IsSrgbFormat(vk::Format format) {
    return format == vk::Format::eR8G8B8A8Srgb || format == vk::Format::eB8G8R8A8Srgb;
}

void Presenter::SubmitXrEyes(const std::array<AmdGpu::Image, 2>& eyes,
                             const Core::Xr::EyeSubmission& submission) {
    auto& xr = Core::Xr::Runtime::Instance();
    const auto format = static_cast<vk::Format>(xr.GetSwapchainFormat());
    if (format == vk::Format::eUndefined) {
        return;
    }
    if (!xr_pp_created) {
        xr_pp_pass.Create(instance.GetDevice(), format);
        xr_pp_created = true;
    }

    if (!xr.EyeSwapchainsMatch(submission.width, submission.height)) {
        draw_scheduler.Finish();
        xr_views = {};
    }
    if (xr.EnsureEyeSwapchains(submission.width, submission.height)) {
        for (u32 eye = 0; eye < 2; ++eye) {
            for (VkImage image : xr.GetEyeSwapchainImages(eye)) {
                auto [result, view] = instance.GetDevice().createImageViewUnique({
                    .image = image,
                    .viewType = vk::ImageViewType::e2D,
                    .format = format,
                    .subresourceRange{
                        .aspectMask = vk::ImageAspectFlagBits::eColor,
                        .levelCount = 1,
                        .layerCount = 1,
                    },
                });
                if (result != vk::Result::eSuccess) {
                    LOG_ERROR(Render_Vulkan, "Failed to create XR image view: {}",
                              vk::to_string(result));
                    xr_views = {};
                    return;
                }
                xr_views[eye].push_back(std::move(view));
            }
        }
    }

    std::array<u32, 2> indices{};
    for (u32 eye = 0; eye < 2; ++eye) {
        const auto index = xr.AcquireEyeImage(eye);
        if (!index || *index >= xr_views[eye].size()) {
            return;
        }
        indices[eye] = *index;
    }

    // The post-processing shader encodes to sRGB itself, which an sRGB attachment would repeat.
    const HostPasses::PostProcessingPass::Settings settings{.hdr = IsSrgbFormat(format) ? 1u : 0u};
    const vk::ImageSubresourceRange color_range{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };

    draw_scheduler.EndRendering();
    const auto cmdbuf = draw_scheduler.CommandBuffer();
    for (u32 eye = 0; eye < 2; ++eye) {
        VideoCore::TextureCache::ImageDesc desc{eyes[eye], Shader::ImageResource{}};
        desc.view_info.mapping.a = AmdGpu::CompSwizzle::One;
        const auto image_id = texture_cache.FindImage(desc);
        texture_cache.UpdateImage(image_id);
        auto& image = texture_cache.GetImage(image_id);
        const auto image_view = *image.FindView(desc.view_info).image_view;
        runtime.Transit(&image, vk::ImageLayout::eShaderReadOnlyOptimal,
                        vk::PipelineStageFlagBits2::eFragmentShader,
                        vk::AccessFlagBits2::eShaderRead);
        runtime.FlushBarriers();

        Frame target{
            .width = submission.width[eye],
            .height = submission.height[eye],
            .image = xr.GetEyeSwapchainImages(eye)[indices[eye]],
            .image_view = *xr_views[eye][indices[eye]],
        };
        const vk::ImageMemoryBarrier2 to_attachment{
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .image = target.image,
            .subresourceRange = color_range,
        };
        cmdbuf.pipelineBarrier2(
            {.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_attachment});

        xr_pp_pass.Render(cmdbuf, image_view, {image.info.size.width, image.info.size.height},
                          target, settings);

        const vk::ImageMemoryBarrier2 to_runtime{
            .srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .image = target.image,
            .subresourceRange = color_range,
        };
        cmdbuf.pipelineBarrier2(
            {.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_runtime});
    }

    draw_scheduler.Flush();
    xr.PresentEyes(submission);
}

} // namespace Vulkan

/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "vulkan_video.h"
#include "vulkan_decode.h"
#include "config_components.h"

#if CONFIG_H264_VULKAN_HWACCEL
extern const VkExtensionProperties ff_vk_dec_h264_ext;
#endif
#if CONFIG_HEVC_VULKAN_HWACCEL
extern const VkExtensionProperties ff_vk_dec_hevc_ext;
#endif

static const VkExtensionProperties *dec_ext[] = {
#if CONFIG_H264_VULKAN_HWACCEL
    [AV_CODEC_ID_H264] = &ff_vk_dec_h264_ext,
#endif
#if CONFIG_HEVC_VULKAN_HWACCEL
    [AV_CODEC_ID_HEVC] = &ff_vk_dec_hevc_ext,
#endif
};

static int vk_decode_create_view(FFVulkanDecodeContext *ctx, VkImageView *dst_view,
                                 VkImageAspectFlags *aspect, AVVkFrame *src,
                                 int base_layer)
{
    VkResult ret;
    FFVulkanFunctions *vk = &ctx->s.vkfn;
    VkImageAspectFlags aspect_mask = ff_vk_aspect_bits_from_vkfmt(ctx->pic_format);

    VkSamplerYcbcrConversionInfo yuv_sampler_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
        .conversion = ctx->yuv_sampler,
    };
    VkImageViewCreateInfo img_view_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = &yuv_sampler_info,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = ctx->pic_format,
        .image = src->img[0],
        .components = (VkComponentMapping) {
            .r = VK_COMPONENT_SWIZZLE_IDENTITY,
            .g = VK_COMPONENT_SWIZZLE_IDENTITY,
            .b = VK_COMPONENT_SWIZZLE_IDENTITY,
            .a = VK_COMPONENT_SWIZZLE_IDENTITY,
        },
        .subresourceRange = (VkImageSubresourceRange) {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseArrayLayer = base_layer,
            .layerCount     = 1,
            .levelCount     = 1,
        },
    };

    ret = vk->CreateImageView(ctx->s.hwctx->act_dev, &img_view_create_info,
                              ctx->s.hwctx->alloc, dst_view);
    if (ret != VK_SUCCESS)
        return AVERROR_EXTERNAL;

    *aspect = aspect_mask;

    return 0;
}

static AVBufferRef *vk_get_dpb_pool(FFVulkanDecodeContext *ctx)
{
    AVHWFramesContext *dpb_frames = (AVHWFramesContext *)ctx->dpb_hwfc_ref->data;
    return av_buffer_pool_get(dpb_frames->pool);
}

int ff_vk_decode_prepare_frame(FFVulkanDecodeContext *ctx, AVFrame *pic,
                               FFVulkanDecodePicture *vkpic, int is_current,
                               int dpb_layer)
{
    int err;

    vkpic->slices_size = 0;

    /* If the decoder made a blank frame to make up for a missing ref, or the
     * frame is the current frame so it's missing one, create a re-representation */
    if (vkpic->img_view_ref)
        return 0;

    /* Pre-allocate slice buffer with a reasonable default */
    if (is_current) {
        vkpic->slices = av_fast_realloc(NULL, &vkpic->slices_size_max, 4096);
        if (!vkpic->slices)
            return AVERROR(ENOMEM);
    }

    if (ctx->dedicated_dpb) {
        if (!ctx->layered_dpb) {
            vkpic->dpb_ref = vk_get_dpb_pool(ctx);
            if (!vkpic->dpb_ref)
                return AVERROR(ENOMEM);

            vkpic->dpb_frame = (AVVkFrame *)vkpic->dpb_ref->data;
        } else {
            vkpic->dpb_frame = (AVVkFrame *)ctx->layered_frame->data;
            vkpic->dpb_ref = NULL;
        }

        err = vk_decode_create_view(ctx, &vkpic->img_view_ref,
                                    &vkpic->img_aspect_ref,
                                    vkpic->dpb_frame,
                                    ctx->layered_dpb ? dpb_layer : 0);
        if (err < 0)
            return err;
    }

    if (!ctx->dedicated_dpb || is_current) {
        err = vk_decode_create_view(ctx, &vkpic->img_view_out,
                                    &vkpic->img_aspect,
                                    (AVVkFrame *)pic->buf[0]->data, 0);
        if (err < 0)
            return err;

        if (!ctx->dedicated_dpb) {
            vkpic->img_view_ref = vkpic->img_view_out;
            vkpic->img_aspect_ref = vkpic->img_aspect;
        }
    }

    return 0;
}

int ff_vk_decode_add_slice(FFVulkanDecodePicture *vp,
                           const uint8_t *data, size_t size, int add_startcode,
                           int *nb_slices, const uint32_t **offsets)
{
    int nb = *nb_slices;
    static const uint8_t startcode_prefix[3] = { 0x0, 0x0, 0x1 };
    size_t startcode_len = add_startcode ? sizeof(startcode_prefix) : 0;

    uint8_t *slices;
    uint32_t *slice_off;

    slices = av_fast_realloc(vp->slices, &vp->slices_size_max,
                             vp->slices_size + size + startcode_len);
    if (!slices)
        return AVERROR(ENOMEM);

    slice_off = av_fast_realloc(vp->slice_off, &vp->slice_off_max,
                                (nb + 1)*sizeof(slice_off));
    if (!slice_off)
        return AVERROR(ENOMEM);

    /* Copy new slice data */
    if (add_startcode) {
        memcpy(slices + vp->slices_size + 0, startcode_prefix, startcode_len);
        memcpy(slices + vp->slices_size + startcode_len, data, size);
    } else {
        memcpy(slices, data, size);
    }

    /* Set offset for the slice */
    slice_off[nb] = vp->slices_size;

    *offsets = vp->slice_off = slice_off;
    *nb_slices = nb + 1;

    vp->slices_size += startcode_len + size;

    vp->slices = slices;

    return 0;
}

void ff_vk_decode_flush(AVCodecContext *avctx)
{
    FFVulkanDecodeContext *ctx = avctx->internal->hwaccel_priv_data;
    FFVulkanFunctions *vk = &ctx->s.vkfn;
    VkVideoBeginCodingInfoKHR decode_start = {
        .sType = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR,
        .videoSession = ctx->common.session,
        .videoSessionParameters = ctx->empty_session_params,
    };
    VkVideoCodingControlInfoKHR decode_ctrl = {
        .sType = VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR,
        .flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR,
    };
    VkVideoEndCodingInfoKHR decode_end = {
        .sType = VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR,
    };

    VkCommandBuffer cmd_buf;
    FFVkExecContext *exec = ff_vk_exec_get(&ctx->exec_pool);
    ff_vk_exec_start(&ctx->s, exec);
    cmd_buf = exec->buf;

    vk->CmdBeginVideoCodingKHR(cmd_buf, &decode_start);
    vk->CmdControlVideoCodingKHR(cmd_buf, &decode_ctrl);
    vk->CmdEndVideoCodingKHR(cmd_buf, &decode_end);
    ff_vk_exec_submit(&ctx->s, exec);
}

int ff_vk_decode_frame(AVCodecContext *avctx,
                       AVFrame *pic,    FFVulkanDecodePicture *vp,
                       AVFrame *rpic[], FFVulkanDecodePicture *rvkp[])
{
    int err;
    VkResult ret;
    VkCommandBuffer cmd_buf;
    FFVkVideoBuffer *sd_buf;

    FFVulkanDecodeContext *ctx = avctx->internal->hwaccel_priv_data;
    FFVulkanFunctions *vk = &ctx->s.vkfn;

    /* Output */
    AVVkFrame *vkf = (AVVkFrame *)pic->buf[0]->data;

    /* Quirks */
    const int dedicated_dpb = ctx->dedicated_dpb;
    const int layered_dpb   = ctx->layered_dpb;

    VkVideoBeginCodingInfoKHR decode_start = {
        .sType = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR,
        .videoSession = ctx->common.session,
        .videoSessionParameters = vp->session_params,
        .referenceSlotCount = vp->decode_info.referenceSlotCount,
        .pReferenceSlots = vp->decode_info.pReferenceSlots,
    };
    VkVideoEndCodingInfoKHR decode_end = {
        .sType = VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR,
    };

    VkImageMemoryBarrier2 img_bar[37];
    VkDependencyInfo dep_info;
    int nb_img_bar = 0;
    AVBufferRef *sd_ref;
    size_t data_size = FFALIGN(vp->slices_size, ctx->common.caps.minBitstreamBufferSizeAlignment);

    FFVkExecContext *exec = ff_vk_exec_get(&ctx->exec_pool);

    if (ctx->exec_pool.nb_queries) {
        int64_t prev_sub_res = 0;
        ff_vk_exec_wait(&ctx->s, exec);
        ret = ff_vk_exec_get_query(&ctx->s, exec, NULL, &prev_sub_res);
        if (ret != VK_NOT_READY && ret != VK_SUCCESS) {
            av_log(avctx, AV_LOG_ERROR, "Unable to perform query: %s!\n",
                   ff_vk_ret2str(ret));
            return AVERROR_EXTERNAL;
        }

        if (ret == VK_SUCCESS)
            av_log(avctx, prev_sub_res < 0 ? AV_LOG_ERROR : AV_LOG_DEBUG,
                   "Result of previous frame decoding: %li\n", prev_sub_res);
    }

    err = ff_vk_video_get_buffer(&ctx->s, &ctx->common, &sd_ref,
                                 VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR,
                                 &ctx->profile_list, vp->slices_size);
    if (err < 0)
        return err;

    sd_buf = (FFVkVideoBuffer *)sd_ref->data;

    /* Copy the slices data to the buffer */
    memcpy(sd_buf->mem, vp->slices, vp->slices_size);

    /* Flush if needed */
    if (!(sd_buf->buf.flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        VkMappedMemoryRange flush_buf = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = sd_buf->buf.mem,
            .offset = 0,
            .size = FFALIGN(data_size,
                            ctx->s.props.properties.limits.nonCoherentAtomSize),
        };

        ret = vk->FlushMappedMemoryRanges(ctx->s.hwctx->act_dev, 1, &flush_buf);
        if (ret != VK_SUCCESS) {
            av_log(avctx, AV_LOG_ERROR, "Failed to flush memory: %s\n",
                   ff_vk_ret2str(ret));
            av_buffer_unref(&sd_ref);
            return AVERROR_EXTERNAL;
        }
    }

    vp->decode_info.srcBuffer       = sd_buf->buf.buf;
    vp->decode_info.srcBufferOffset = 0;
    vp->decode_info.srcBufferRange  = data_size;

    /* Start command buffer recording */
    ff_vk_exec_start(&ctx->s, exec);
    cmd_buf = exec->buf;

    err = ff_vk_exec_add_dep_buf(&ctx->s, exec, &sd_ref, 1, 0);
    if (err < 0)
        return err;

    err = ff_vk_exec_add_dep_frame(&ctx->s, exec, pic->buf[0],
                                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
    if (err < 0)
        return err;

    /* Output image - change layout, as it comes from a pool */
    img_bar[nb_img_bar] = (VkImageMemoryBarrier2) {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = NULL,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = vkf->access[0],
        .dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR,
        .dstAccessMask = VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR,
        .oldLayout = vkf->layout[0],
        .newLayout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR,
        .srcQueueFamilyIndex = vkf->queue_family[0],
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = vkf->img[0],
        .subresourceRange = (VkImageSubresourceRange) {
            .aspectMask = vp->img_aspect,
            .layerCount = 1,
            .levelCount = 1,
        },
    };
    ff_vk_exec_update_frame(&ctx->s, exec, pic->buf[0], &img_bar[nb_img_bar++]);

    /* Reference for the current image, if needed */
    if (dedicated_dpb && !layered_dpb) {
        err = ff_vk_exec_add_dep_frame(&ctx->s, exec, vp->dpb_ref,
                                       VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
        if (err < 0)
            return err;
    }

    if (!layered_dpb) {
        /* All references (apart from the current) for non-layered refs */

        for (int i = 0; i < vp->decode_info.referenceSlotCount; i++) {
            AVFrame *ref_frame = rpic[i];
            FFVulkanDecodePicture *rvp = rvkp[i];
            AVBufferRef *ref = dedicated_dpb ? rvp->dpb_ref : ref_frame->buf[0];

            err = ff_vk_exec_add_dep_frame(&ctx->s, exec, ref,
                                           VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
            if (err < 0)
                return err;

            if (!dedicated_dpb) {
                AVVkFrame *rvkf = (AVVkFrame *)ref->data;

                img_bar[nb_img_bar] = (VkImageMemoryBarrier2) {
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                    .pNext = NULL,
                    .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    .srcAccessMask = rvkf->access[0],
                    .dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR,
                    .dstAccessMask = VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR |
                                     VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR,
                    .oldLayout = rvkf->layout[0],
                    .newLayout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR,
                    .srcQueueFamilyIndex = rvkf->queue_family[0],
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = rvkf->img[0],
                    .subresourceRange = (VkImageSubresourceRange) {
                        .aspectMask = rvp->img_aspect_ref,
                        .layerCount = 1,
                        .levelCount = 1,
                    },
                };
                ff_vk_exec_update_frame(&ctx->s, exec, ref,
                                        &img_bar[nb_img_bar++]);
            }
        }
    } else {
        /* Single barrier for a single layered ref */
        err = ff_vk_exec_add_dep_frame(&ctx->s, exec, ctx->layered_frame,
                                       VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
        if (err < 0)
            return err;
    }

    dep_info = (VkDependencyInfo) {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
        .pImageMemoryBarriers = img_bar,
        .imageMemoryBarrierCount = nb_img_bar,
    };

    /* Change image layout */
    vk->CmdPipelineBarrier2KHR(cmd_buf, &dep_info);

    /* Start, use parameters, decode and end decoding */
    vk->CmdBeginVideoCodingKHR(cmd_buf, &decode_start);

    /* Start status query TODO: remove check when radv gets support */
    if (ctx->exec_pool.nb_queries)
        vk->CmdBeginQuery(cmd_buf, ctx->exec_pool.query_pool, exec->query_idx + 0, 0);

    vk->CmdDecodeVideoKHR(cmd_buf, &vp->decode_info);

    /* End status query */
    if (ctx->exec_pool.nb_queries)
        vk->CmdEndQuery(cmd_buf, ctx->exec_pool.query_pool, exec->query_idx + 0);

    vk->CmdEndVideoCodingKHR(cmd_buf, &decode_end);

    /* Destroy semaphore details. We do not have access to the AVVkFrame when
     * destroying it. */
    vp->sem       = vkf->sem[0];
    vp->sem_value = vkf->sem_value[0] + 1;

    /* End recording and submit for execution */
    ff_vk_exec_submit(&ctx->s, exec);

    return 0;
}

void ff_vk_decode_free_frame(FFVulkanDecodeContext *ctx, FFVulkanDecodePicture *vp)
{
    FFVulkanFunctions *vk;
    VkSemaphoreWaitInfo sem_wait;

    // TODO: investigate why this happens
    if (!ctx) {
        av_freep(&vp->slices);
        av_freep(&vp->slice_off);
        av_buffer_unref(&vp->dpb_ref);
        return;
    }

    vk = &ctx->s.vkfn;

    /* We do not have to lock the frame here because we're not interested
     * in the actual current semaphore value, but only that it's later than
     * the time we submitted the image for decoding. */
    sem_wait = (VkSemaphoreWaitInfo) {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .pSemaphores = &vp->sem,
        .pValues = &vp->sem_value,
        .semaphoreCount = 1,
    };

    if (vp->sem)
        vk->WaitSemaphores(ctx->s.hwctx->act_dev, &sem_wait, UINT64_MAX);

    /* Free slices data
     * TODO: use a pool in the decode context instead to avoid per-frame allocs. */
    av_freep(&vp->slices);
    av_freep(&vp->slice_off);

    /* Destroy parameters */
    if (vp->session_params)
        vk->DestroyVideoSessionParametersKHR(ctx->s.hwctx->act_dev, vp->session_params,
                                             ctx->s.hwctx->alloc);

    /* Destroy image view (out) */
    if (vp->img_view_out != vp->img_view_ref && vp->img_view_out)
        vk->DestroyImageView(ctx->s.hwctx->act_dev, vp->img_view_out, ctx->s.hwctx->alloc);

    /* Destroy image view (ref) */
    if (vp->img_view_ref)
        vk->DestroyImageView(ctx->s.hwctx->act_dev, vp->img_view_ref, ctx->s.hwctx->alloc);

    av_buffer_unref(&vp->dpb_ref);
}

/* Since to even get decoder capabilities, we have to initialize quite a lot,
 * this function does initialization and saves it to hwaccel_priv_data if
 * available. */
static int vulkan_decode_check_init(AVCodecContext *avctx, AVBufferRef *frames_ref,
                                    int *width_align, int *height_align,
                                    enum AVPixelFormat *pix_fmt, int *dpb_dedicate)
{
    VkResult ret;
    int err, max_level, score = INT32_MAX;
    const struct FFVkCodecMap *vk_codec = &ff_vk_codec_map[avctx->codec_id];
    AVHWFramesContext *frames = (AVHWFramesContext *)frames_ref->data;
    AVHWDeviceContext *device = (AVHWDeviceContext *)frames->device_ref->data;
    AVVulkanDeviceContext *hwctx = device->hwctx;
    enum AVPixelFormat context_format = frames->sw_format;
    int context_format_was_found = 0;
    int base_profile, cur_profile = avctx->profile;

    int dedicated_dpb;
    int layered_dpb;

    FFVulkanDecodeContext *ctx = avctx->internal->hwaccel_priv_data;
    FFVulkanExtensions local_extensions = 0x0;
    FFVulkanExtensions *extensions = ctx ? &ctx->s.extensions : &local_extensions;
    FFVulkanFunctions local_vk = { 0 };
    FFVulkanFunctions *vk = ctx ? &ctx->s.vkfn : &local_vk;
    VkVideoCapabilitiesKHR local_caps = { 0 };
    VkVideoCapabilitiesKHR *caps = ctx ? &ctx->common.caps : &local_caps;
    VkVideoDecodeCapabilitiesKHR local_dec_caps = { 0 };
    VkVideoDecodeCapabilitiesKHR *dec_caps = ctx ? &ctx->dec_caps : &local_dec_caps;
    VkVideoDecodeUsageInfoKHR local_usage = { 0 };
    VkVideoDecodeUsageInfoKHR *usage = ctx ? &ctx->usage : &local_usage;
    VkVideoProfileInfoKHR local_profile = { 0 };
    VkVideoProfileInfoKHR *profile = ctx ? &ctx->profile : &local_profile;
    VkVideoProfileListInfoKHR local_profile_list = { 0 };
    VkVideoProfileListInfoKHR *profile_list = ctx ? &ctx->profile_list : &local_profile_list;

    VkVideoDecodeH264ProfileInfoKHR local_h264_profile = { 0 };
    VkVideoDecodeH264ProfileInfoKHR *h264_profile = ctx ? &ctx->h264_profile : &local_h264_profile;

    VkVideoDecodeH264ProfileInfoKHR local_h265_profile = { 0 };
    VkVideoDecodeH264ProfileInfoKHR *h265_profile = ctx ? &ctx->h265_profile : &local_h265_profile;

    VkPhysicalDeviceVideoFormatInfoKHR fmt_info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR,
        .pNext = profile_list,
    };
    VkVideoDecodeH264CapabilitiesKHR h264_caps = {
        .sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR,
    };
    VkVideoDecodeH265CapabilitiesKHR h265_caps = {
        .sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_CAPABILITIES_KHR,
    };
    VkVideoFormatPropertiesKHR *ret_info;
    uint32_t nb_out_fmts = 0;

    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(avctx->sw_pix_fmt);
    if (!desc)
        return AVERROR(EINVAL);

    if (ctx && ctx->init)
        return 0;

    if (!vk_codec->decode_op)
        return AVERROR(EINVAL);

    *extensions = ff_vk_extensions_to_mask(hwctx->enabled_dev_extensions,
                                           hwctx->nb_enabled_dev_extensions);

    if (!(*extensions & FF_VK_EXT_VIDEO_DECODE_QUEUE)) {
        av_log(avctx, AV_LOG_ERROR, "Device does not support the %s extension!\n",
               VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME);
        return AVERROR(ENOSYS);
    } else if (!vk_codec->decode_extension) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported codec for Vulkan decoding: %s!\n",
               avcodec_get_name(avctx->codec_id));
        return AVERROR(ENOSYS);
    } else if (!(vk_codec->decode_extension & *extensions)) {
        av_log(avctx, AV_LOG_ERROR, "Device does not support decoding %s!\n",
               avcodec_get_name(avctx->codec_id));
        return AVERROR(ENOSYS);
    }

    err = ff_vk_load_functions(device, vk, *extensions, 1, 1);
    if (err < 0)
        return err;

repeat:
    if (avctx->codec_id == AV_CODEC_ID_H264) {
        base_profile = FF_PROFILE_H264_CONSTRAINED_BASELINE;
        dec_caps->pNext = &h264_caps;
        usage->pNext = h264_profile;
        h264_profile->sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR;
        h264_profile->stdProfileIdc = cur_profile;
        h264_profile->pictureLayout = avctx->field_order == AV_FIELD_PROGRESSIVE ?
                                      VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR :
                                      VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_INTERLACED_INTERLEAVED_LINES_BIT_KHR;
    } else if (avctx->codec_id == AV_CODEC_ID_H265) {
        base_profile = FF_PROFILE_HEVC_MAIN;
        dec_caps->pNext = &h265_caps;
        usage->pNext = h265_profile;
        h265_profile->sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PROFILE_INFO_KHR;
        h265_profile->stdProfileIdc = cur_profile;
    }

    usage->sType           = VK_STRUCTURE_TYPE_VIDEO_DECODE_USAGE_INFO_KHR;
    usage->videoUsageHints = VK_VIDEO_DECODE_USAGE_DEFAULT_KHR;

    profile->sType               = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
    /* NOTE: NVIDIA's implementation fails if the USAGE hint is inserted.
     * Remove this once it's fixed. */
    profile->pNext               = usage->pNext;
    profile->videoCodecOperation = vk_codec->decode_op;
    profile->chromaSubsampling   = ff_vk_subsampling_from_av_desc(desc);
    profile->lumaBitDepth        = ff_vk_depth_from_av_depth(desc->comp[0].depth);
    profile->chromaBitDepth      = profile->lumaBitDepth;

    profile_list->sType        = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR;
    profile_list->profileCount = 1;
    profile_list->pProfiles    = profile;

    /* Get the capabilities of the decoder for the given profile */
    caps->sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;
    caps->pNext = dec_caps;
    dec_caps->sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR;
    /* dec_caps->pNext already filled in */

    ret = vk->GetPhysicalDeviceVideoCapabilitiesKHR(hwctx->phys_dev, profile,
                                                    caps);
    if (ret == VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR &&
        avctx->flags & AV_HWACCEL_FLAG_ALLOW_PROFILE_MISMATCH &&
        cur_profile != base_profile) {
        cur_profile = base_profile;
        av_log(avctx, AV_LOG_VERBOSE, "%s profile %s not supported, attempting "
               "again with profile %s\n",
               avcodec_get_name(avctx->codec_id),
               avcodec_profile_name(avctx->codec_id, avctx->profile),
               avcodec_profile_name(avctx->codec_id, base_profile));
        goto repeat;
    } else if (ret == VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR) {
        av_log(avctx, AV_LOG_VERBOSE, "Unable to initialize video session: "
               "%s profile \"%s\" not supported!\n",
               avcodec_get_name(avctx->codec_id),
               avcodec_profile_name(avctx->codec_id, cur_profile));
        return AVERROR(EINVAL);
    } else if (ret == VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR) {
        av_log(avctx, AV_LOG_VERBOSE, "Unable to initialize video session: "
               "format (%s) not supported!\n",
               av_get_pix_fmt_name(avctx->sw_pix_fmt));
        return AVERROR(EINVAL);
    } else if (ret == VK_ERROR_FEATURE_NOT_PRESENT ||
               ret == VK_ERROR_FORMAT_NOT_SUPPORTED) {
        return AVERROR(EINVAL);
    } else if (ret != VK_SUCCESS) {
        return AVERROR_EXTERNAL;
    }

    max_level = avctx->codec_id == AV_CODEC_ID_H264 ? h264_caps.maxLevelIdc :
                avctx->codec_id == AV_CODEC_ID_H265 ? h265_caps.maxLevelIdc :
                0;

    if (ctx) {
        av_log(avctx, AV_LOG_VERBOSE, "Decoder capabilities for %s profile \"%s\":\n",
               avcodec_get_name(avctx->codec_id),
               avcodec_profile_name(avctx->codec_id, avctx->profile));
        av_log(avctx, AV_LOG_VERBOSE, "    Maximum level: %i\n",
               max_level);
        av_log(avctx, AV_LOG_VERBOSE, "    Width: from %i to %i\n",
               caps->minCodedExtent.width, caps->maxCodedExtent.width);
        av_log(avctx, AV_LOG_VERBOSE, "    Height: from %i to %i\n",
               caps->minCodedExtent.height, caps->maxCodedExtent.height);
        av_log(avctx, AV_LOG_VERBOSE, "    Width alignment: %i\n",
               caps->pictureAccessGranularity.width);
        av_log(avctx, AV_LOG_VERBOSE, "    Height alignment: %i\n",
               caps->pictureAccessGranularity.height);
        av_log(avctx, AV_LOG_VERBOSE, "    Bitstream offset alignment: %"PRIu64"\n",
               caps->minBitstreamBufferOffsetAlignment);
        av_log(avctx, AV_LOG_VERBOSE, "    Bitstream size alignment: %"PRIu64"\n",
               caps->minBitstreamBufferSizeAlignment);
        av_log(avctx, AV_LOG_VERBOSE, "    Maximum references: %u\n",
               caps->maxDpbSlots);
        av_log(avctx, AV_LOG_VERBOSE, "    Maximum active references: %u\n",
               caps->maxActiveReferencePictures);
        av_log(avctx, AV_LOG_VERBOSE, "    Codec header version: %i.%i.%i (driver), %i.%i.%i (compiled)\n",
               CODEC_VER(caps->stdHeaderVersion.specVersion),
               CODEC_VER(dec_ext[avctx->codec_id]->specVersion));
        av_log(avctx, AV_LOG_VERBOSE, "    Decode modes:%s%s%s\n",
               dec_caps->flags ? "" :
                   " invalid",
               dec_caps->flags & VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR ?
                   " reuse_dst_dpb" : "",
               dec_caps->flags & VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_DISTINCT_BIT_KHR ?
                   " dedicated_dpb" : "");
        av_log(avctx, AV_LOG_VERBOSE, "    Capability flags:%s%s%s\n",
               caps->flags ? "" :
                   " none",
               caps->flags & VK_VIDEO_CAPABILITY_PROTECTED_CONTENT_BIT_KHR ?
                   " protected" : "",
               caps->flags & VK_VIDEO_CAPABILITY_SEPARATE_REFERENCE_IMAGES_BIT_KHR ?
                   " separate_references" : "");
    }

    /* Check if decoding is possible with the given parameters */
    if (avctx->coded_width  < caps->minCodedExtent.width   ||
        avctx->coded_height < caps->minCodedExtent.height  ||
        avctx->coded_width  > caps->maxCodedExtent.width   ||
        avctx->coded_height > caps->maxCodedExtent.height)
        return AVERROR(EINVAL);

    if (!(avctx->hwaccel_flags & AV_HWACCEL_FLAG_IGNORE_LEVEL) &&
        avctx->level > max_level)
        return AVERROR(EINVAL);

    /* Some basic sanity checking */
    if (!(dec_caps->flags & (VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR |
                             VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_DISTINCT_BIT_KHR))) {
        av_log(avctx, AV_LOG_ERROR, "Buggy driver signals invalid decoding mode: neither "
               "VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR nor "
               "VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_DISTINCT_BIT_KHR are set!\n");
        return AVERROR_EXTERNAL;
    } else if ((dec_caps->flags & (VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR |
                                   VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_DISTINCT_BIT_KHR) ==
                                   VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR) &&
               !(caps->flags & VK_VIDEO_CAPABILITY_SEPARATE_REFERENCE_IMAGES_BIT_KHR)) {
        av_log(avctx, AV_LOG_ERROR, "Cannot initialize Vulkan decoding session, buggy driver: "
               "VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR set "
               "but VK_VIDEO_CAPABILITY_SEPARATE_REFERENCE_IMAGES_BIT_KHR is unset!\n");
        return AVERROR_EXTERNAL;
    }

    /* TODO: make dedicated_dpb tunable */
    dedicated_dpb = !(dec_caps->flags & VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR);
    layered_dpb   = !(caps->flags & VK_VIDEO_CAPABILITY_SEPARATE_REFERENCE_IMAGES_BIT_KHR);

    if (dedicated_dpb) {
        fmt_info.imageUsage = VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR;
    } else {
        fmt_info.imageUsage = VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR |
                              VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR |
                              VK_IMAGE_USAGE_SAMPLED_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }

    /* Get the format of the images necessary */
    ret = vk->GetPhysicalDeviceVideoFormatPropertiesKHR(hwctx->phys_dev,
                                                        &fmt_info,
                                                        &nb_out_fmts, NULL);
    if (ret == VK_ERROR_FORMAT_NOT_SUPPORTED ||
        (!nb_out_fmts && ret == VK_SUCCESS)) {
        return AVERROR(EINVAL);
    } else if (ret != VK_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR, "Unable to get Vulkan format properties: %s!\n",
               ff_vk_ret2str(ret));
        return AVERROR_EXTERNAL;
    }

    ret_info = av_mallocz(sizeof(*ret_info)*nb_out_fmts);
    if (!ret_info)
        return AVERROR(ENOMEM);

    for (int i = 0; i < nb_out_fmts; i++)
        ret_info[i].sType = VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR;

    ret = vk->GetPhysicalDeviceVideoFormatPropertiesKHR(hwctx->phys_dev,
                                                        &fmt_info,
                                                        &nb_out_fmts, ret_info);
    if (ret == VK_ERROR_FORMAT_NOT_SUPPORTED ||
        (!nb_out_fmts && ret == VK_SUCCESS)) {
        av_free(ret_info);
        return AVERROR(EINVAL);
    } else if (ret != VK_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR, "Unable to get Vulkan format properties: %s!\n",
               ff_vk_ret2str(ret));
        av_free(ret_info);
        return AVERROR_EXTERNAL;
    }

    if (ctx) {
        ctx->dedicated_dpb = dedicated_dpb;
        ctx->layered_dpb = layered_dpb;
        ctx->init = 1;
    }

    *pix_fmt = AV_PIX_FMT_NONE;

    av_log(avctx, AV_LOG_DEBUG, "Pixel format list for decoding:\n");
    for (int i = 0; i < nb_out_fmts; i++) {
        int tmp_score;
        enum AVPixelFormat tmp = ff_vk_pix_fmt_from_vkfmt(ret_info[i].format,
                                                          &tmp_score);
        const AVPixFmtDescriptor *tmp_desc = av_pix_fmt_desc_get(tmp);
        if (tmp == AV_PIX_FMT_NONE || !tmp_desc)
            continue;

        av_log(avctx, AV_LOG_DEBUG, "    %i - %s (%i), score %i\n", i,
               av_get_pix_fmt_name(tmp), ret_info[i].format, tmp_score);

        if (context_format == tmp || tmp_score < score) {
            if (ctx)
                ctx->pic_format = ret_info[i].format;
            *pix_fmt = tmp;
            context_format_was_found |= context_format == tmp;
            if (context_format_was_found)
                break;
        }
    }

    if (*pix_fmt == AV_PIX_FMT_NONE) {
        av_log(avctx, AV_LOG_ERROR, "No valid pixel format for decoding!\n");
        return AVERROR(EINVAL);
    }

    if (width_align)
        *width_align = caps->pictureAccessGranularity.width;
    if (height_align)
        *height_align = caps->pictureAccessGranularity.height;
    if (dpb_dedicate)
        *dpb_dedicate = dedicated_dpb;

    av_free(ret_info);

    av_log(avctx, AV_LOG_VERBOSE, "Chosen frames format: %s\n",
           av_get_pix_fmt_name(*pix_fmt));

    if (context_format != AV_PIX_FMT_NONE && !context_format_was_found) {
        av_log(avctx, AV_LOG_ERROR, "Frames context had a pixel format set which "
               "was not available for decoding into!\n");
        return AVERROR(EINVAL);
    }

    return *pix_fmt == AV_PIX_FMT_NONE ? AVERROR(EINVAL) : 0;
}

int ff_vk_frame_params(AVCodecContext *avctx, AVBufferRef *hw_frames_ctx)
{
    int err, width_align, height_align, dedicated_dpb;
    AVHWFramesContext *frames_ctx = (AVHWFramesContext*)hw_frames_ctx->data;
    AVVulkanFramesContext *hwfc = frames_ctx->hwctx;

    err = vulkan_decode_check_init(avctx, hw_frames_ctx, &width_align, &height_align,
                                   &frames_ctx->sw_format, &dedicated_dpb);
    if (err < 0)
        return err;

    frames_ctx->width  = FFALIGN(avctx->coded_width, width_align);
    frames_ctx->height = FFALIGN(avctx->coded_height, height_align);
    frames_ctx->format = AV_PIX_FMT_VULKAN;

    hwfc->tiling       = VK_IMAGE_TILING_OPTIMAL;
    hwfc->usage        = VK_IMAGE_USAGE_TRANSFER_SRC_BIT         |
                         VK_IMAGE_USAGE_SAMPLED_BIT              |
                         VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR;

    if (!dedicated_dpb)
        hwfc->usage |= VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR;

    return err;
}

int ff_vk_decode_uninit(AVCodecContext *avctx)
{
    FFVulkanDecodeContext *ctx = avctx->internal->hwaccel_priv_data;
    FFVulkanContext *s = &ctx->s;
    FFVulkanFunctions *vk = &ctx->s.vkfn;

    /* Wait on and free execution pool */
    ff_vk_exec_pool_free(s, &ctx->exec_pool);

    /* This also frees all references from this pool */
    av_buffer_unref(&ctx->layered_frame);
    av_buffer_unref(&ctx->dpb_hwfc_ref);

    /* Destroy parameters */
    if (ctx->empty_session_params)
        vk->DestroyVideoSessionParametersKHR(s->hwctx->act_dev,
                                             ctx->empty_session_params,
                                             s->hwctx->alloc);

    ff_vk_video_common_uninit(s, &ctx->common);

    vk->DestroySamplerYcbcrConversion(s->hwctx->act_dev, ctx->yuv_sampler,
                                      s->hwctx->alloc);

    av_buffer_pool_uninit(&ctx->tmp_pool);

    ff_vk_uninit(s);

    return 0;
}

int ff_vk_decode_init(AVCodecContext *avctx)
{
    int err, qf, cxpos = 0, cypos = 0, nb_q = 0;
    VkResult ret;
    FFVulkanDecodeContext *ctx = avctx->internal->hwaccel_priv_data;
    FFVulkanContext *s = &ctx->s;
    FFVulkanFunctions *vk = &ctx->s.vkfn;

    VkVideoDecodeH264SessionParametersCreateInfoKHR h264_params = {
        .sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR,
    };
    VkVideoDecodeH265SessionParametersCreateInfoKHR h265_params = {
        .sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_SESSION_PARAMETERS_CREATE_INFO_KHR,
    };
    VkVideoSessionParametersCreateInfoKHR session_params_create = {
        .sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR,
        .pNext = avctx->codec_id == AV_CODEC_ID_H264 ? (void *)&h264_params :
                 avctx->codec_id == AV_CODEC_ID_HEVC ? (void *)&h265_params :
                 NULL,
    };
    VkVideoSessionCreateInfoKHR session_create = {
        .sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR,
    };
    VkSamplerYcbcrConversionCreateInfo yuv_sampler_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
        .components = ff_comp_identity_map,
        .ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY,
        .ycbcrRange = avctx->color_range == AVCOL_RANGE_MPEG, /* Ignored */
    };

    err = ff_decode_get_hw_frames_ctx(avctx, AV_HWDEVICE_TYPE_VULKAN);
    if (err < 0)
        return err;

    s->frames_ref = av_buffer_ref(avctx->hw_frames_ctx);
    s->frames = (AVHWFramesContext *)s->frames_ref->data;
    s->hwfc = s->frames->hwctx;

    s->device_ref = av_buffer_ref(s->frames->device_ref);
    s->device = (AVHWDeviceContext *)s->device_ref->data;
    s->hwctx = s->device->hwctx;

    /* Get parameters, capabilities and final pixel/vulkan format */
    err = vulkan_decode_check_init(avctx, s->frames_ref, NULL, NULL,
                                   &ctx->sw_format, NULL);
    if (err < 0)
        goto fail;

    /* Load all properties */
    err = ff_vk_load_props(s);
    if (err < 0)
        goto fail;

    /* Create queue context */
    qf = ff_vk_qf_init(s, &ctx->qf_dec, VK_QUEUE_VIDEO_DECODE_BIT_KHR);

    /* Check for support */
    if (!(s->video_props[qf].videoCodecOperations &
          ff_vk_codec_map[avctx->codec_id].decode_op)) {
        av_log(avctx, AV_LOG_ERROR, "Decoding %s not supported on the given "
               "queue family %i!\n", avcodec_get_name(avctx->codec_id), qf);
        return AVERROR(EINVAL);
    }

    /* TODO: enable when stable and tested. */
    if (s->query_props[qf].queryResultStatusSupport)
        nb_q = 1;

    /* Create decode exec context.
     * 4 async contexts per thread seems like a good number. */
    err = ff_vk_exec_pool_init(s, &ctx->qf_dec, &ctx->exec_pool, 4*avctx->thread_count,
                               nb_q, VK_QUERY_TYPE_RESULT_STATUS_ONLY_KHR, 0,
                               &ctx->profile);
    if (err < 0)
        goto fail;

    session_create.pVideoProfile = &ctx->profile;
    session_create.flags = 0x0;
    session_create.queueFamilyIndex = s->hwctx->queue_family_decode_index;
    session_create.maxCodedExtent = ctx->common.caps.maxCodedExtent;
    session_create.maxDpbSlots = ctx->common.caps.maxDpbSlots;
    session_create.maxActiveReferencePictures = ctx->common.caps.maxActiveReferencePictures;
    session_create.pictureFormat = ctx->pic_format;
    session_create.referencePictureFormat = session_create.pictureFormat;
    session_create.pStdHeaderVersion = dec_ext[avctx->codec_id];

    err = ff_vk_video_common_init(avctx, s, &ctx->common, &session_create);
    if (err < 0)
        goto fail;

    /* Get sampler */
    av_chroma_location_enum_to_pos(&cxpos, &cypos, avctx->chroma_sample_location);
    yuv_sampler_info.xChromaOffset = cxpos >> 7;
    yuv_sampler_info.yChromaOffset = cypos >> 7;
    yuv_sampler_info.format = ctx->pic_format;
    ret = vk->CreateSamplerYcbcrConversion(s->hwctx->act_dev, &yuv_sampler_info,
                                           s->hwctx->alloc, &ctx->yuv_sampler);
    if (ret != VK_SUCCESS) {
        err = AVERROR_EXTERNAL;
        goto fail;
    }

    /* If doing an out-of-place decoding, create a DPB pool */
    if (ctx->dedicated_dpb) {
        AVHWFramesContext *dpb_frames;
        AVVulkanFramesContext *dpb_hwfc;

        ctx->dpb_hwfc_ref = av_hwframe_ctx_alloc(s->device_ref);
        if (!ctx->dpb_hwfc_ref) {
            err = AVERROR(ENOMEM);
            goto fail;
        }

        dpb_frames = (AVHWFramesContext *)ctx->dpb_hwfc_ref->data;
        dpb_frames->format    = s->frames->format;
        dpb_frames->sw_format = s->frames->sw_format;
        dpb_frames->width     = s->frames->width;
        dpb_frames->height    = s->frames->height;

        dpb_hwfc = dpb_frames->hwctx;
        dpb_hwfc->create_pnext = &ctx->profile_list;
        dpb_hwfc->tiling = VK_IMAGE_TILING_OPTIMAL;
        dpb_hwfc->usage  = VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR |
                           VK_IMAGE_USAGE_SAMPLED_BIT; /* Shuts validator up. */

        if (ctx->layered_dpb)
            dpb_hwfc->nb_layers = ctx->common.caps.maxDpbSlots;

        err = av_hwframe_ctx_init(ctx->dpb_hwfc_ref);
        if (err < 0)
            goto fail;

        if (ctx->layered_dpb) {
            ctx->layered_frame = vk_get_dpb_pool(ctx);
            if (!ctx->layered_frame) {
                err = AVERROR(ENOMEM);
                goto fail;
            }
        }
    }

    session_params_create.videoSession = ctx->common.session;
    ret = vk->CreateVideoSessionParametersKHR(s->hwctx->act_dev, &session_params_create,
                                              s->hwctx->alloc, &ctx->empty_session_params);
    if (ret != VK_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR, "Unable to create empty Vulkan video session parameters: %s!\n",
               ff_vk_ret2str(ret));
        return AVERROR_EXTERNAL;
    }

    ff_vk_decode_flush(avctx);

    av_log(avctx, AV_LOG_VERBOSE, "Vulkan decoder initialization sucessful\n");

    return 0;

fail:
    ff_vk_decode_uninit(avctx);

    return err;
}

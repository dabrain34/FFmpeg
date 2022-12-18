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

#include "codec_id.h"

#include "vulkan_video.h"

const FFVkCodecMap ff_vk_codec_map[AV_CODEC_ID_FIRST_AUDIO] = {
    [AV_CODEC_ID_H264] = {
#if CONFIG_VULKAN_ENCODE
                           FF_VK_EXT_VIDEO_ENCODE_H264 | FF_VK_EXT_SYNC2,
                           VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_EXT,
#else
                           0,
                           0,
#endif
                           FF_VK_EXT_VIDEO_DECODE_H264 | FF_VK_EXT_SYNC2,
                           VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR,
    },
    [AV_CODEC_ID_HEVC] = {
#if CONFIG_VULKAN_ENCODE
                           FF_VK_EXT_VIDEO_ENCODE_H265 | FF_VK_EXT_SYNC2,
                           VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_EXT,
#else
                           0,
                           0,
#endif
                           FF_VK_EXT_VIDEO_DECODE_H265 | FF_VK_EXT_SYNC2,
                           VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR
    },
};

enum AVPixelFormat ff_vk_pix_fmt_from_vkfmt(VkFormat vkf, int *score)
{
    switch (vkf) {
    /* Mono */
    case VK_FORMAT_R8_UNORM:
        *score = 1;
        return AV_PIX_FMT_GRAY8;
    case VK_FORMAT_R10X6_UNORM_PACK16:
    case VK_FORMAT_R12X4_UNORM_PACK16:
        *score = 2;
        return AV_PIX_FMT_GRAY16;
    case VK_FORMAT_R16_UNORM:
        *score = 1;
        return AV_PIX_FMT_GRAY16;

    /* RGB */
    case VK_FORMAT_B8G8R8A8_UNORM:
        *score = 1;
        return AV_PIX_FMT_BGRA;
    case VK_FORMAT_R8G8B8A8_UNORM:
        *score = 1;
        return AV_PIX_FMT_RGBA;
    case VK_FORMAT_R8G8B8_UNORM:
        *score = 1;
        return AV_PIX_FMT_RGB24;
    case VK_FORMAT_B8G8R8_UNORM:
        *score = 1;
        return AV_PIX_FMT_BGR24;

    /* 420 */
    case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM:
        *score = 1;
        return AV_PIX_FMT_NV12;
    case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM:
        *score = 1;
        return AV_PIX_FMT_YUV420P;
    case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16:
        *score = 2;
        return AV_PIX_FMT_P010;
    case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16:
        *score = 2;
        return AV_PIX_FMT_YUV420P16;
    /* No support for VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16 */
    case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16:
        *score = 2;
        return AV_PIX_FMT_YUV420P12;
    case VK_FORMAT_G16_B16_R16_3PLANE_420_UNORM:
        *score = 1;
        return AV_PIX_FMT_YUV420P16;

    /* 422 */
    case VK_FORMAT_G8_B8R8_2PLANE_422_UNORM:
        *score = 1;
        return AV_PIX_FMT_NV16;
    case VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM:
        *score = 1;
        return AV_PIX_FMT_YUV422P;
    case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16:
        *score = 2;
        return AV_PIX_FMT_NV20;
    case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_422_UNORM_3PACK16:
        *score = 2;
        return AV_PIX_FMT_YUV422P10;
    /* No support for VK_FORMAT_G12X4_B12X4R12X4_2PLANE_422_UNORM_3PACK16 */
    case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_422_UNORM_3PACK16:
        *score = 2;
        return AV_PIX_FMT_YUV422P12;
    case VK_FORMAT_G16_B16_R16_3PLANE_422_UNORM:
        *score = 1;
        return AV_PIX_FMT_YUV422P16;

    /* 444 */
    case VK_FORMAT_G8_B8R8_2PLANE_444_UNORM_EXT:
        *score = 1;
        return AV_PIX_FMT_NV24;
    case VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM:
        *score = 1;
        return AV_PIX_FMT_YUV444P;
    /* No support for VK_FORMAT_G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16_EXT */
    case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16:
        *score = 2;
        return AV_PIX_FMT_YUV444P10;
    /* No support for VK_FORMAT_G12X4_B12X4R12X4_2PLANE_444_UNORM_3PACK16_EXT */
    case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_444_UNORM_3PACK16:
        *score = 2;
        return AV_PIX_FMT_YUV444P12;
    case VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM:
        *score = 1;
        return AV_PIX_FMT_YUV444P16;
    default:
        break;
    }

    return AV_PIX_FMT_NONE;
}

VkImageAspectFlags ff_vk_aspect_bits_from_vkfmt(VkFormat vkf)
{
    switch (vkf) {
    case VK_FORMAT_R8_UNORM:
    case VK_FORMAT_R10X6_UNORM_PACK16:
    case VK_FORMAT_R12X4_UNORM_PACK16:
    case VK_FORMAT_R16_UNORM:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8_UNORM:
    case VK_FORMAT_B8G8R8_UNORM:
        return VK_IMAGE_ASPECT_COLOR_BIT;

    /* 420 */
    case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM:
    case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16:
    case VK_FORMAT_G8_B8R8_2PLANE_422_UNORM:
    case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16:
    case VK_FORMAT_G8_B8R8_2PLANE_444_UNORM_EXT:
        return VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT;

    case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM:
    case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16:
    case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16:
    case VK_FORMAT_G16_B16_R16_3PLANE_420_UNORM:
    case VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM:
    case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_422_UNORM_3PACK16:
    case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_422_UNORM_3PACK16:
    case VK_FORMAT_G16_B16_R16_3PLANE_422_UNORM:
    case VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM:
    case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16:
    case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_444_UNORM_3PACK16:
    case VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM:
        return VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT | VK_IMAGE_ASPECT_PLANE_2_BIT;

    default:
        break;
    }

    return VK_IMAGE_ASPECT_NONE;
}

VkVideoChromaSubsamplingFlagBitsKHR ff_vk_subsampling_from_av_desc(const AVPixFmtDescriptor *desc)
{
    if (desc->nb_components == 1)
        return VK_VIDEO_CHROMA_SUBSAMPLING_MONOCHROME_BIT_KHR;
    else if (!desc->log2_chroma_w && !desc->log2_chroma_h)
        return VK_VIDEO_CHROMA_SUBSAMPLING_444_BIT_KHR;
    else if (!desc->log2_chroma_w && desc->log2_chroma_h == 1)
        return VK_VIDEO_CHROMA_SUBSAMPLING_422_BIT_KHR;
    else if (desc->log2_chroma_w == 1 && desc->log2_chroma_h == 1)
        return VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
    return VK_VIDEO_CHROMA_SUBSAMPLING_INVALID_KHR;
}

VkVideoComponentBitDepthFlagBitsKHR ff_vk_depth_from_av_depth(int depth)
{
    switch (depth) {
    case  8: return VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    case 10: return VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR;
    case 12: return VK_VIDEO_COMPONENT_BIT_DEPTH_12_BIT_KHR;
    default: break;
    }
    return VK_VIDEO_COMPONENT_BIT_DEPTH_INVALID_KHR;
}

static void free_data_buf(void *opaque, uint8_t *data)
{
    FFVulkanContext *ctx = opaque;
    FFVkVideoBuffer *buf = (FFVkVideoBuffer *)data;
    ff_vk_unmap_buffers(ctx, &buf->buf, 1, 0);
    ff_vk_free_buf(ctx, &buf->buf);
    av_free(data);
}

static AVBufferRef *alloc_data_buf(void *opaque, size_t size)
{
    uint8_t *buf = av_mallocz(size);
    if (!buf)
        return NULL;

    return av_buffer_create(buf, size, free_data_buf, opaque, 0);
}

int ff_vk_video_get_buffer(FFVulkanContext *ctx, FFVkVideoCommon *s,
                           AVBufferRef **buf, VkBufferUsageFlags usage,
                           void *create_pNext, size_t size)
{
    int err;
    AVBufferRef *ref;
    FFVkVideoBuffer *data;

    if (!s->buf_pool) {
        s->buf_pool = av_buffer_pool_init2(sizeof(FFVkVideoBuffer), ctx,
                                           alloc_data_buf, NULL);
        if (!s->buf_pool)
            return AVERROR(ENOMEM);
    }

    *buf = ref = av_buffer_pool_get(s->buf_pool);
    if (!ref)
        return AVERROR(ENOMEM);

    data = (FFVkVideoBuffer *)ref->data;

    if (data->buf.size >= size)
        return 0;

    /* No point in requesting anything smaller. */
    size = FFMAX(size, 1024*1024);
    size = FFALIGN(size, s->caps.minBitstreamBufferSizeAlignment);

    /* Align buffer to nearest power of two. Makes fragmentation management
     * easier, and gives us ample headroom. */
    size--;
    size |= size >>  1;
    size |= size >>  2;
    size |= size >>  4;
    size |= size >>  8;
    size |= size >> 16;
    size++;

    ff_vk_free_buf(ctx, &data->buf);
    memset(data, 0, sizeof(FFVkVideoBuffer));

    err = ff_vk_create_buf(ctx, &data->buf, size,
                           create_pNext, NULL, usage,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (err < 0) {
        av_buffer_unref(&ref);
        return err;
    }

    /* Map the buffer */
    err = ff_vk_map_buffers(ctx, &data->buf, &data->mem, 1, 0);
    if (err < 0) {
        av_buffer_unref(&ref);
        return err;
    }

    return 0;
}

av_cold void ff_vk_video_common_uninit(FFVulkanContext *s,
                                        FFVkVideoCommon *common)
{
    FFVulkanFunctions *vk = &s->vkfn;

    if (common->session)
        vk->DestroyVideoSessionKHR(s->hwctx->act_dev, common->session,
                                   s->hwctx->alloc);

    if (common->nb_mem && common->mem)
        for (int i = 0; i < common->nb_mem; i++)
            vk->FreeMemory(s->hwctx->act_dev, common->mem[i], s->hwctx->alloc);

    av_freep(&common->mem);

    av_buffer_pool_uninit(&common->buf_pool);
}

av_cold int ff_vk_video_common_init(void *log, FFVulkanContext *s,
                                    FFVkVideoCommon *common,
                                    VkVideoSessionCreateInfoKHR *session_create)
{
    int err;
    VkResult ret;
    FFVulkanFunctions *vk = &s->vkfn;
    VkMemoryRequirements2 *mem_req = NULL;
    VkVideoSessionMemoryRequirementsKHR *mem = NULL;
    VkBindVideoSessionMemoryInfoKHR *bind_mem = NULL;

    /* Create session */
    ret = vk->CreateVideoSessionKHR(s->hwctx->act_dev, session_create,
                                    s->hwctx->alloc, &common->session);
    if (ret != VK_SUCCESS)
        return AVERROR_EXTERNAL;

    /* Get memory requirements */
    ret = vk->GetVideoSessionMemoryRequirementsKHR(s->hwctx->act_dev,
                                                   common->session,
                                                   &common->nb_mem,
                                                   NULL);
    if (ret != VK_SUCCESS) {
        err = AVERROR_EXTERNAL;
        goto fail;
    }

    /* Allocate all memory needed to actually allocate memory */
    common->mem = av_mallocz(sizeof(*common->mem)*common->nb_mem);
    if (!common->mem) {
        err = AVERROR(ENOMEM);
        goto fail;
    }
    mem = av_mallocz(sizeof(*mem)*common->nb_mem);
    if (!mem) {
        err = AVERROR(ENOMEM);
        goto fail;
    }
    mem_req = av_mallocz(sizeof(*mem_req)*common->nb_mem);
    if (!mem_req) {
        err = AVERROR(ENOMEM);
        goto fail;
    }
    bind_mem = av_mallocz(sizeof(*bind_mem)*common->nb_mem);
    if (!bind_mem) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    /* Set the needed fields to get the memory requirements */
    for (int i = 0; i < common->nb_mem; i++) {
        mem_req[i] = (VkMemoryRequirements2) {
            .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
        };
        mem[i] = (VkVideoSessionMemoryRequirementsKHR) {
            .sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR,
            .memoryRequirements = mem_req[i].memoryRequirements,
        };
    }

    /* Finally get the memory requirements */
    ret = vk->GetVideoSessionMemoryRequirementsKHR(s->hwctx->act_dev,
                                                   common->session, &common->nb_mem,
                                                   mem);
    if (ret != VK_SUCCESS) {
        err = AVERROR_EXTERNAL;
        goto fail;
    }

    /* Now allocate each requested memory.
     * For ricing, could pool together memory that ends up in the same index. */
    for (int i = 0; i < common->nb_mem; i++) {
        err = ff_vk_alloc_mem(s, &mem[i].memoryRequirements,
                              UINT32_MAX, NULL, NULL, &common->mem[i]);
        if (err < 0)
            goto fail;

        bind_mem[i] = (VkBindVideoSessionMemoryInfoKHR) {
            .sType = VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR,
            .memory = common->mem[i],
            .memoryBindIndex = mem[i].memoryBindIndex,
            .memoryOffset = 0,
            .memorySize = mem[i].memoryRequirements.size,
        };

        av_log(log, AV_LOG_VERBOSE, "Allocating %lu bytes in bind index %i for video session\n",
               bind_mem[i].memorySize, bind_mem[i].memoryBindIndex);
    }

    /* Bind the allocated memory */
    ret = vk->BindVideoSessionMemoryKHR(s->hwctx->act_dev, common->session,
                                        common->nb_mem, bind_mem);
    if (ret != VK_SUCCESS) {
        err = AVERROR_EXTERNAL;
        goto fail;
    }

    av_freep(&mem);
    av_freep(&mem_req);
    av_freep(&bind_mem);

    return 0;

fail:
    av_freep(&mem);
    av_freep(&mem_req);
    av_freep(&bind_mem);

    ff_vk_video_common_uninit(s, common);
    return err;
}

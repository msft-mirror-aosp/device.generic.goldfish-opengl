/*
 * Copyright 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "GoldfishHevcHelper.h"

#define LOG_TAG "GoldfishHevcHelper"
#include <log/log.h>

#include "ihevc_typedefs.h"
#include "ihevcd_cxa.h"

#define DEBUG 0
#if DEBUG
#define DDD(...) ALOGD(__VA_ARGS__)
#else
#define DDD(...) ((void)0)
#endif


#include <Codec2Mapper.h>

#define ivdec_api_function ihevcd_cxa_api_function
#define ivdext_create_ip_t ihevcd_cxa_create_ip_t
#define ivdext_create_op_t ihevcd_cxa_create_op_t
#define ivdext_delete_ip_t ihevcd_cxa_delete_ip_t
#define ivdext_delete_op_t ihevcd_cxa_delete_op_t
#define ivdext_ctl_set_num_cores_ip_t ihevcd_cxa_ctl_set_num_cores_ip_t
#define ivdext_ctl_set_num_cores_op_t ihevcd_cxa_ctl_set_num_cores_op_t
#define ivdext_ctl_get_vui_params_ip_t ihevcd_cxa_ctl_get_vui_params_ip_t
#define ivdext_ctl_get_vui_params_op_t ihevcd_cxa_ctl_get_vui_params_op_t
#define ALIGN128(x) ((((x) + 127) >> 7) << 7)
#define MAX_NUM_CORES 4
#define IVDEXT_CMD_CTL_SET_NUM_CORES                                           \
    (IVD_CONTROL_API_COMMAND_TYPE_T) IHEVCD_CXA_CMD_CTL_SET_NUM_CORES
#define MIN(a, b) (((a) < (b)) ? (a) : (b))

namespace android {

static void *ivd_aligned_malloc(void *ctxt, WORD32 alignment, WORD32 size) {
    (void) ctxt;
    return memalign(alignment, size);
}

static void ivd_aligned_free(void *ctxt, void *mem) {
    (void) ctxt;
    free(mem);
}


GoldfishHevcHelper::GoldfishHevcHelper(int w, int h):mWidth(w),mHeight(h) { createDecoder(); }

GoldfishHevcHelper::~GoldfishHevcHelper() {
    destroyDecoder();
    if (mOutBufferFlush) {
        ivd_aligned_free(nullptr, mOutBufferFlush);
        mOutBufferFlush = nullptr;
    }
}

void GoldfishHevcHelper::createDecoder() {
    ivdext_create_ip_t s_create_ip = {};
    ivdext_create_op_t s_create_op = {};

    s_create_ip.s_ivd_create_ip_t.u4_size = sizeof(ivdext_create_ip_t);
    s_create_ip.s_ivd_create_ip_t.e_cmd = IVD_CMD_CREATE;
    s_create_ip.s_ivd_create_ip_t.u4_share_disp_buf = 0;
    s_create_ip.s_ivd_create_ip_t.e_output_format = mIvColorformat;
    s_create_ip.s_ivd_create_ip_t.pf_aligned_alloc = ivd_aligned_malloc;
    s_create_ip.s_ivd_create_ip_t.pf_aligned_free = ivd_aligned_free;
    s_create_ip.s_ivd_create_ip_t.pv_mem_ctxt = nullptr;
    s_create_op.s_ivd_create_op_t.u4_size = sizeof(ivdext_create_op_t);
    IV_API_CALL_STATUS_T status =
        ivdec_api_function(mDecHandle, &s_create_ip, &s_create_op);
    if (status != IV_SUCCESS) {
        ALOGE("error in %s: 0x%x", __func__,
              s_create_op.s_ivd_create_op_t.u4_error_code);
        return;
    }
    mDecHandle = (iv_obj_t *)s_create_op.s_ivd_create_op_t.pv_handle;
    mDecHandle->pv_fxns = (void *)ivdec_api_function;
    mDecHandle->u4_size = sizeof(iv_obj_t);

    mStride = ALIGN128(mWidth);

    setNumCores();
}

void GoldfishHevcHelper::destroyDecoder() {
    if (mDecHandle) {
        ivdext_delete_ip_t s_delete_ip = {};
        ivdext_delete_op_t s_delete_op = {};

        s_delete_ip.s_ivd_delete_ip_t.u4_size = sizeof(ivdext_delete_ip_t);
        s_delete_ip.s_ivd_delete_ip_t.e_cmd = IVD_CMD_DELETE;
        s_delete_op.s_ivd_delete_op_t.u4_size = sizeof(ivdext_delete_op_t);
        IV_API_CALL_STATUS_T status =
            ivdec_api_function(mDecHandle, &s_delete_ip, &s_delete_op);
        if (status != IV_SUCCESS) {
            ALOGE("error in %s: 0x%x", __func__,
                  s_delete_op.s_ivd_delete_op_t.u4_error_code);
        }
        mDecHandle = nullptr;
    }
}

void GoldfishHevcHelper::setNumCores() {
    ivdext_ctl_set_num_cores_ip_t s_set_num_cores_ip = {};
    ivdext_ctl_set_num_cores_op_t s_set_num_cores_op = {};

    s_set_num_cores_ip.u4_size = sizeof(ivdext_ctl_set_num_cores_ip_t);
    s_set_num_cores_ip.e_cmd = IVD_CMD_VIDEO_CTL;
    s_set_num_cores_ip.e_sub_cmd = IVDEXT_CMD_CTL_SET_NUM_CORES;
    s_set_num_cores_ip.u4_num_cores = mNumCores;
    s_set_num_cores_op.u4_size = sizeof(ivdext_ctl_set_num_cores_op_t);
    IV_API_CALL_STATUS_T status = ivdec_api_function(
        mDecHandle, &s_set_num_cores_ip, &s_set_num_cores_op);
    if (IV_SUCCESS != status) {
        DDD("error in %s: 0x%x", __func__, s_set_num_cores_op.u4_error_code);
    }
}

void GoldfishHevcHelper::resetDecoder() {
    ivd_ctl_reset_ip_t s_reset_ip = {};
    ivd_ctl_reset_op_t s_reset_op = {};

    s_reset_ip.u4_size = sizeof(ivd_ctl_reset_ip_t);
    s_reset_ip.e_cmd = IVD_CMD_VIDEO_CTL;
    s_reset_ip.e_sub_cmd = IVD_CMD_CTL_RESET;
    s_reset_op.u4_size = sizeof(ivd_ctl_reset_op_t);
    IV_API_CALL_STATUS_T status =
        ivdec_api_function(mDecHandle, &s_reset_ip, &s_reset_op);
    if (IV_SUCCESS != status) {
        ALOGE("error in %s: 0x%x", __func__, s_reset_op.u4_error_code);
    }
    setNumCores();
}

void GoldfishHevcHelper::setParams(size_t stride,
                                   IVD_VIDEO_DECODE_MODE_T dec_mode) {
    ihevcd_cxa_ctl_set_config_ip_t s_hevcd_set_dyn_params_ip = {};
    ihevcd_cxa_ctl_set_config_op_t s_hevcd_set_dyn_params_op = {};
    ivd_ctl_set_config_ip_t *ps_set_dyn_params_ip =
        &s_hevcd_set_dyn_params_ip.s_ivd_ctl_set_config_ip_t;
    ivd_ctl_set_config_op_t *ps_set_dyn_params_op =
        &s_hevcd_set_dyn_params_op.s_ivd_ctl_set_config_op_t;

    ps_set_dyn_params_ip->u4_size = sizeof(ihevcd_cxa_ctl_set_config_ip_t);
    ps_set_dyn_params_ip->e_cmd = IVD_CMD_VIDEO_CTL;
    ps_set_dyn_params_ip->e_sub_cmd = IVD_CMD_CTL_SETPARAMS;
    ps_set_dyn_params_ip->u4_disp_wd = (UWORD32)stride;
    ps_set_dyn_params_ip->e_frm_skip_mode = IVD_SKIP_NONE;
    ps_set_dyn_params_ip->e_frm_out_mode = IVD_DISPLAY_FRAME_OUT;
    ps_set_dyn_params_ip->e_vid_dec_mode = dec_mode;
    ps_set_dyn_params_op->u4_size = sizeof(ihevcd_cxa_ctl_set_config_op_t);
    IV_API_CALL_STATUS_T status = ivdec_api_function(
        mDecHandle, ps_set_dyn_params_ip, ps_set_dyn_params_op);
    if (status != IV_SUCCESS) {
        ALOGE("error in %s: 0x%x", __func__,
              ps_set_dyn_params_op->u4_error_code);
    }
}

bool GoldfishHevcHelper::isVpsFrame(const uint8_t* frame, int inSize) {
    if (inSize < 5) return false;
    if (frame[0] == 0 && frame[1] == 0 && frame[2] == 0 && frame[3] == 1) {
        const bool forbiddenBitIsInvalid = 0x80 & frame[4];
        if (forbiddenBitIsInvalid) {
            return false;
        }
        // nalu type is the lower 6 bits after shiftting to right 1 bit
        uint8_t naluType = 0x3f & (frame[4] >> 1);
        if (naluType == 32
            || naluType == 33
            || naluType == 34
                ) return true;
        else return false;
    } else {
        return false;
    }
}

bool GoldfishHevcHelper::decodeHeader(const uint8_t *frame, int inSize) {
    // should we check the header for vps/sps/pps frame ? otherwise
    // there is no point calling decoder
    if (!isVpsFrame(frame, inSize)) {
        DDD("could not find valid vps frame");
        return false;
    } else {
        DDD("found valid vps frame");
    }

    ihevcd_cxa_video_decode_ip_t s_hevcd_decode_ip = {};
    ihevcd_cxa_video_decode_op_t s_hevcd_decode_op = {};
    ivd_video_decode_ip_t *ps_decode_ip =
        &s_hevcd_decode_ip.s_ivd_video_decode_ip_t;
    ivd_video_decode_op_t *ps_decode_op =
        &s_hevcd_decode_op.s_ivd_video_decode_op_t;

    // setup input/output arguments to decoder
    setDecodeArgs(ps_decode_ip, ps_decode_op, frame, mStride,
            0, // offset
            inSize, // size
            0 // time-stamp, does not matter
            );

    setParams(mStride, IVD_DECODE_HEADER);

    // now kick off the decoding
    IV_API_CALL_STATUS_T status = ivdec_api_function(mDecHandle, ps_decode_ip, ps_decode_op);
    if (status != IV_SUCCESS) {
        ALOGE("failed to call decoder function for header\n");
        ALOGE("error in %s: 0x%x", __func__,
              ps_decode_op->u4_error_code);
    }

    if (IVD_RES_CHANGED == (ps_decode_op->u4_error_code & IVD_ERROR_MASK)) {
        DDD("resolution changed, reset decoder");
        resetDecoder();
        setParams(mStride, IVD_DECODE_HEADER);
        ivdec_api_function(mDecHandle, ps_decode_ip, ps_decode_op);
    }

    // get the w/h and update
    if (0 < ps_decode_op->u4_pic_wd && 0 < ps_decode_op->u4_pic_ht) {
        DDD("success decode w/h %d %d", ps_decode_op->u4_pic_wd , ps_decode_op->u4_pic_ht);
        DDD("existing w/h %d %d", mWidth, mHeight);
        if (ps_decode_op->u4_pic_wd != mWidth ||  ps_decode_op->u4_pic_ht != mHeight) {
            mWidth = ps_decode_op->u4_pic_wd;
            mHeight = ps_decode_op->u4_pic_ht;
            return true;
        } else {
            DDD("success decode w/h, but they are the same %d %d", ps_decode_op->u4_pic_wd , ps_decode_op->u4_pic_ht);
        }
    } else {
        ALOGE("could not decode w/h");
    }

    // get output delay
    if (ps_decode_op->i4_reorder_depth >= 0) {
        if (mOutputDelay != ps_decode_op->i4_reorder_depth) {
            mOutputDelay = ps_decode_op->i4_reorder_depth;
            DDD("New Output delay %d ", mOutputDelay);
        } else {
            DDD("same Output delay %d ", mOutputDelay);
        }
    }

    return false;
}

bool GoldfishHevcHelper::setDecodeArgs(ivd_video_decode_ip_t *ps_decode_ip,
                                       ivd_video_decode_op_t *ps_decode_op,
                                       const uint8_t *inBuffer,
                                       uint32_t displayStride, size_t inOffset,
                                       size_t inSize, uint32_t tsMarker) {
    uint32_t displayHeight = mHeight;
    size_t lumaSize = displayStride * displayHeight;
    size_t chromaSize = lumaSize >> 2;

    if (mStride != displayStride) {
        mStride = displayStride;
    }

    // force decoder to always decode header and get dimensions,
    // hope this will be quick and cheap
    setParams(mStride, IVD_DECODE_HEADER);

    ps_decode_ip->u4_size = sizeof(ihevcd_cxa_video_decode_ip_t);
    ps_decode_ip->e_cmd = IVD_CMD_VIDEO_DECODE;
    if (inBuffer) {
        ps_decode_ip->u4_ts = tsMarker;
        ps_decode_ip->pv_stream_buffer = const_cast<uint8_t *>(inBuffer) + inOffset;
        ps_decode_ip->u4_num_Bytes = inSize;
    } else {
        ps_decode_ip->u4_ts = 0;
        ps_decode_ip->pv_stream_buffer = nullptr;
        ps_decode_ip->u4_num_Bytes = 0;
    }
    DDD("setting pv_stream_buffer 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x",
            ((uint8_t*)(ps_decode_ip->pv_stream_buffer))[0],
            ((uint8_t*)(ps_decode_ip->pv_stream_buffer))[1],
            ((uint8_t*)(ps_decode_ip->pv_stream_buffer))[2],
            ((uint8_t*)(ps_decode_ip->pv_stream_buffer))[3],
            ((uint8_t*)(ps_decode_ip->pv_stream_buffer))[4],
            ((uint8_t*)(ps_decode_ip->pv_stream_buffer))[5],
            ((uint8_t*)(ps_decode_ip->pv_stream_buffer))[6],
            ((uint8_t*)(ps_decode_ip->pv_stream_buffer))[7]
            );
    DDD("input bytes %d", ps_decode_ip->u4_num_Bytes);

    ps_decode_ip->s_out_buffer.u4_min_out_buf_size[0] = lumaSize;
    ps_decode_ip->s_out_buffer.u4_min_out_buf_size[1] = chromaSize;
    ps_decode_ip->s_out_buffer.u4_min_out_buf_size[2] = chromaSize;
    if (!mOutBufferFlush) {
        uint32_t displayStride = mStride;
        uint32_t displayHeight = mHeight;
        uint32_t bufferSize = displayStride * displayHeight * 3 / 2;
        mOutBufferFlush = (uint8_t *)ivd_aligned_malloc(nullptr, 128, bufferSize);
    }
    {
        ps_decode_ip->s_out_buffer.pu1_bufs[0] = mOutBufferFlush;
        ps_decode_ip->s_out_buffer.pu1_bufs[1] = mOutBufferFlush + lumaSize;
        ps_decode_ip->s_out_buffer.pu1_bufs[2] =
            mOutBufferFlush + lumaSize + chromaSize;
    }
    ps_decode_ip->s_out_buffer.u4_num_bufs = 3;
    ps_decode_op->u4_size = sizeof(ihevcd_cxa_video_decode_op_t);
    ps_decode_op->u4_output_present = 0;

    return true;
}

} // namespace android

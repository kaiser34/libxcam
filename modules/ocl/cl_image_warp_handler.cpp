/*
 * cl_image_warp_handler.cpp - CL image warping handler
 *
 *  Copyright (c) 2016 Intel Corporation
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
 *
 * Author: Zong Wei <wei.zong@intel.com>
 */

#include "xcam_utils.h"
#include "cl_image_warp_handler.h"

namespace XCam {

#define CL_IMAGE_WARP_WG_WIDTH   8
#define CL_IMAGE_WARP_WG_HEIGHT  4

#define CL_BUFFER_POOL_SIZE      20
#define CL_IMAGE_WARP_WRITE_UINT 1

enum {
#if CL_IMAGE_WARP_WRITE_UINT
    KernelImageWarp   = 0,
#else
    KernelImageWarp   = 1,
#endif
};

const XCamKernelInfo kernel_image_warp_info [] = {
    {
        "kernel_image_warp_8_pixel",
#include "kernel_image_warp.clx"
        , 0,
    },
    {
        "kernel_image_warp_1_pixel",
#include "kernel_image_warp.clx"
        , 0,
    }
};

CLImageWarpKernel::CLImageWarpKernel (SmartPtr<CLContext> &context,
                                      const char *name,
                                      uint32_t channel,
                                      SmartPtr<CLImageWarpHandler> &handler)
    : CLImageKernel (context, name)
    , _channel (channel)
    , _handler (handler)
{
    _input_frame_id = -1;
    _warp_frame_id = -1;
    _warp_config = _handler->get_warp_config ();
}

XCamReturn
CLImageWarpKernel::prepare_arguments (
    SmartPtr<DrmBoBuffer> &input, SmartPtr<DrmBoBuffer> &output,
    CLArgument args[], uint32_t &arg_count,
    CLWorkSize &work_size)
{
    SmartPtr<CLContext> context = get_context ();

    const VideoBufferInfo & video_info_in = input->get_video_info ();
    const VideoBufferInfo & video_info_out = output->get_video_info ();

    uint32_t info_index = 0;
    if (_channel == CL_IMAGE_CHANNEL_Y) {
        info_index = 0;
    } else if (_channel == CL_IMAGE_CHANNEL_UV) {
        info_index = 1;
    }

    CLImageDesc cl_desc_in, cl_desc_out;
    cl_desc_in.format.image_channel_order = info_index == 0 ? CL_R : CL_RG;
    cl_desc_in.format.image_channel_data_type = CL_UNORM_INT8;
    cl_desc_in.width = video_info_in.width >> info_index;
    cl_desc_in.height = video_info_in.height >> info_index;
    cl_desc_in.row_pitch = video_info_in.strides[info_index];

#if CL_IMAGE_WARP_WRITE_UINT
    cl_desc_out.format.image_channel_data_type = info_index == 0 ? CL_UNSIGNED_INT16 : CL_UNSIGNED_INT32;
    cl_desc_out.format.image_channel_order = CL_RGBA;
    cl_desc_out.width = XCAM_ALIGN_DOWN (video_info_out.width >> info_index, 4) / 8;
    cl_desc_out.height = video_info_out.height >> info_index;
#else
    cl_desc_out.format.image_channel_order = info_index == 0 ? CL_R : CL_RG;
    cl_desc_out.format.image_channel_data_type = CL_UNORM_INT8;
    cl_desc_out.width = video_info_out.width >> info_index;
    cl_desc_out.height = video_info_out.height >> info_index;
#endif

    cl_desc_out.row_pitch = video_info_out.strides[info_index];
    _image_in = new CLVaImage (context, input, cl_desc_in, video_info_in.offsets[info_index]);
    _input_frame_id ++;

    _warp_config = _handler->get_warp_config ();

    if ((_warp_config.trim_ratio > 0.5f) || (_warp_config.trim_ratio < 0.0f)) {
        _warp_config.trim_ratio = 0.0f;
    }

    /*
       For NV12 image (YUV420), UV plane has half horizontal & vertical coordinate size of Y plane,
       need to adjust the projection matrix as:
       H(uv) = [0.5, 0, 0; 0, 0.5, 0; 0, 0, 1] * H(y) * [2, 0, 0; 0, 2, 0; 0, 0, 1]
    */
    if (_channel == CL_IMAGE_CHANNEL_UV) {
        _warp_config.proj_mat[2] = 0.5 * _warp_config.proj_mat[2];
        _warp_config.proj_mat[5] = 0.5 * _warp_config.proj_mat[5];
        _warp_config.proj_mat[6] = 2.0 * _warp_config.proj_mat[6];
        _warp_config.proj_mat[7] = 2.0 * _warp_config.proj_mat[7];
    }

    if (_image_in_list.size () >= CL_BUFFER_POOL_SIZE) {
        XCAM_LOG_DEBUG ("image list pop front");
        _image_in_list.pop_front ();
        _image_in_list.push_back (_image_in);
    } else {
        _image_in_list.push_back (_image_in);
    }

    XCAM_LOG_DEBUG ("image channel(%lu), image list size(%u)", _channel, _image_in_list.size());
    XCAM_LOG_DEBUG ("warp config image size(%dx%d)", _warp_config.width, _warp_config.height);
    XCAM_LOG_DEBUG ("proj_mat[%d]=(%f, %f, %f, %f, %f, %f, %f, %f, %f)", _warp_config.frame_id,
                    _warp_config.proj_mat[0], _warp_config.proj_mat[1], _warp_config.proj_mat[2],
                    _warp_config.proj_mat[3], _warp_config.proj_mat[4], _warp_config.proj_mat[5],
                    _warp_config.proj_mat[6], _warp_config.proj_mat[7], _warp_config.proj_mat[8]);

    _image_out = new CLVaImage (context, output, cl_desc_out, video_info_out.offsets[info_index]);
    XCAM_ASSERT (_image_in->is_valid () && _image_out->is_valid ());
    XCAM_FAIL_RETURN (
        WARNING,
        _image_in->is_valid () && _image_out->is_valid (),
        XCAM_RETURN_ERROR_MEM,
        "cl image kernel(%s) in/out memory not available", get_kernel_name ());

    //set args;
    work_size.dim = XCAM_DEFAULT_IMAGE_DIM;
    work_size.local[0] = CL_IMAGE_WARP_WG_WIDTH;
    work_size.local[1] = CL_IMAGE_WARP_WG_HEIGHT;
    work_size.global[0] = XCAM_ALIGN_UP (cl_desc_out.width, work_size.local[0]);
    work_size.global[1] = XCAM_ALIGN_UP(cl_desc_out.height, work_size.local[1]);

    std::list<SmartPtr<CLImage>>::iterator it = _image_in_list.begin ();
    args[0].arg_adress = &(*it)->get_mem_id ();
    args[0].arg_size = sizeof (cl_mem);

    args[1].arg_adress = &_image_out->get_mem_id ();
    args[1].arg_size = sizeof (cl_mem);

    args[2].arg_adress = &_warp_config;
    args[2].arg_size = sizeof (_warp_config);

    arg_count = 3;

    return XCAM_RETURN_NO_ERROR;
}

XCamReturn
CLImageWarpKernel::post_execute (SmartPtr<DrmBoBuffer> &output)
{
    if (_warp_config.valid > 0) {
        _warp_frame_id ++;
        XCAM_LOG_DEBUG ("POP Image channel(%d), input frame id(%d)", _channel, _input_frame_id);
        XCAM_LOG_DEBUG ("Warp config id(%d), Warp image id(%d)", _warp_config.frame_id, _warp_frame_id);
        XCAM_LOG_DEBUG ("image list size(%lu)", _image_in_list.size());
        _image_in_list.pop_front ();
        //XCAM_ASSERT (abs(_warp_config.frame_id - _warp_frame_id) <= 2);
    }

    return CLImageKernel::post_execute (output);
}

CLImageWarpHandler::CLImageWarpHandler ()
    : CLImageHandler ("CLImageWarpHandler")
{
    _warp_config.frame_id = -1;
    _warp_config.valid = -1;
    _warp_config.trim_ratio = 0.05f;
    reset_projection_matrix ();
}

void
CLImageWarpHandler::reset_projection_matrix ()
{
    _warp_config.proj_mat[0] = 1.0f;
    _warp_config.proj_mat[1] = 0.0f;
    _warp_config.proj_mat[2] = 0.0f;
    _warp_config.proj_mat[3] = 0.0f;
    _warp_config.proj_mat[4] = 1.0f;
    _warp_config.proj_mat[5] = 0.0f;
    _warp_config.proj_mat[6] = 0.0f;
    _warp_config.proj_mat[7] = 0.0f;
    _warp_config.proj_mat[8] = 1.0f;
}

bool
CLImageWarpHandler::set_warp_config (const XCamDVSResult& config)
{
    _warp_config.frame_id = config.frame_id;
    _warp_config.valid = config.valid;
    _warp_config.width = config.frame_width;
    _warp_config.height = config.frame_height;
    for( int i = 0; i < 9; i++ ) {
        _warp_config.proj_mat[i] = config.proj_mat[i];
    }
    XCAM_LOG_DEBUG ("set_warp_config[%d]=(%f, %f, %f, %f, %f, %f, %f, %f, %f)", _warp_config.frame_id,
                    _warp_config.proj_mat[0], _warp_config.proj_mat[1], _warp_config.proj_mat[2],
                    _warp_config.proj_mat[3], _warp_config.proj_mat[4], _warp_config.proj_mat[5],
                    _warp_config.proj_mat[6], _warp_config.proj_mat[7], _warp_config.proj_mat[8]);
    return true;
}

SmartPtr<CLImageWarpKernel>
create_kernel_image_warp (SmartPtr<CLContext> &context,
                          uint32_t channel,
                          SmartPtr<CLImageWarpHandler> handler)
{
    SmartPtr<CLImageWarpKernel> warp_kernel;

    const char *name = (channel == CL_IMAGE_CHANNEL_Y ? "kernel_image_warp_y" : "kernel_image_warp_uv");
    char build_options[1024];
    xcam_mem_clear (build_options);

    snprintf (build_options, sizeof (build_options),
              " -DWARP_Y=%d ",
              (channel == CL_IMAGE_CHANNEL_Y ? 1 : 0));

    warp_kernel = new CLImageWarpKernel (context, name, channel, handler);
    XCAM_ASSERT (warp_kernel.ptr ());
    XCAM_FAIL_RETURN (
        ERROR, warp_kernel->build_kernel (kernel_image_warp_info[KernelImageWarp], build_options) == XCAM_RETURN_NO_ERROR,
        NULL, "build image warp kernel failed");
    XCAM_ASSERT (warp_kernel->is_valid ());

    return warp_kernel;
}

SmartPtr<CLImageHandler>
create_cl_image_warp_handler (SmartPtr<CLContext> &context)
{
    SmartPtr<CLImageWarpHandler> warp_handler;
    SmartPtr<CLImageKernel> warp_kernel;
    SmartPtr<CLImageKernel> trim_kernel;

    warp_handler = new CLImageWarpHandler ();
    XCAM_ASSERT (warp_handler.ptr ());

    warp_kernel = create_kernel_image_warp (context, CL_IMAGE_CHANNEL_Y, warp_handler);
    XCAM_ASSERT (warp_kernel.ptr ());
    warp_handler->add_kernel (warp_kernel);

    warp_kernel = create_kernel_image_warp (context, CL_IMAGE_CHANNEL_UV, warp_handler);
    XCAM_ASSERT (warp_kernel.ptr ());
    warp_handler->add_kernel (warp_kernel);

    return warp_handler;
}

};
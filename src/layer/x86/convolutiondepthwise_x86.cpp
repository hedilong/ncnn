// Tencent is pleased to support the open source community by making ncnn available.
//
// Copyright (C) 2017 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the BSD 3-Clause License (the "License"); you may not use this file except
// in compliance with the License. You may obtain a copy of the License at
//
// https://opensource.org/licenses/BSD-3-Clause
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

#include "convolutiondepthwise_x86.h"

#ifdef _OPENMP
#include <omp.h>
#endif

#include "layer_type.h"

namespace ncnn {

#include "convolutiondepthwise_3x3.h"

#include "convolutiondepthwise_3x3_int8.h"

DEFINE_LAYER_CREATOR(ConvolutionDepthWise_x86)

ConvolutionDepthWise_x86::ConvolutionDepthWise_x86()
{
    activation = 0;
}

int ConvolutionDepthWise_x86::create_pipeline(const Option& opt)
{
    Option opt_cpu = opt;
    opt_cpu.use_vulkan_compute = false;

    if (activation_type == 1)
    {
        activation = ncnn::create_layer(ncnn::LayerType::ReLU);

        ncnn::ParamDict pd;
        activation->load_param(pd);
    }
    else if (activation_type == 2)
    {
        activation = ncnn::create_layer(ncnn::LayerType::ReLU);

        ncnn::ParamDict pd;
        pd.set(0, activation_params[0]);// slope
        activation->load_param(pd);
    }
    else if (activation_type == 3)
    {
        activation = ncnn::create_layer(ncnn::LayerType::Clip);

        ncnn::ParamDict pd;
        pd.set(0, activation_params[0]);// min
        pd.set(1, activation_params[1]);// max
        activation->load_param(pd);
    }
    else if (activation_type == 4)
    {
        activation = ncnn::create_layer(ncnn::LayerType::Sigmoid);

        ncnn::ParamDict pd;
        activation->load_param(pd);
    }

    if (activation)
    {
        activation->create_pipeline(opt_cpu);
    }

    // create Convolution op for each group
    const int maxk = kernel_w * kernel_h;
    int channels = (weight_data_size / group) / maxk / (num_output / group) * group;

    for (int i=0; i<(int)group_ops.size(); i++)
        delete group_ops[i];

    group_ops.clear();      

    if (channels == group && group == num_output)
    {
        // depth-wise specific
        if (kernel_w == 3 && kernel_h == 3 && dilation_w == 1 && dilation_h == 1)
        {
            if ((stride_w == 1 && stride_h == 1) || (stride_w == 2 && stride_h == 2))
            {
                return 0;
            }
        }
    }    

    const int channels_g = channels / group;
    const int num_output_g = num_output / group;

    group_ops.resize(group);     

    for (int g=0; g<group; g++)
    {  
        Mat weight_data_g = weight_data.range(maxk * channels_g * num_output_g * g, maxk * channels_g * num_output_g);
        Mat bias_data_g;
        if (bias_term)
            bias_data_g = bias_data.range(num_output_g * g, num_output_g);

        ncnn::Layer* op = ncnn::create_layer(ncnn::LayerType::Convolution);

        // set param
        ncnn::ParamDict pd;
        pd.set(0, num_output_g);// num_output
        pd.set(1, kernel_w);
        pd.set(11, kernel_h);
        pd.set(2, dilation_w);
        pd.set(12, dilation_h);
        pd.set(3, stride_w);
        pd.set(13, stride_h);
        pd.set(4, 0);// pad_w
        pd.set(14, 0);// pad_h
        pd.set(5, bias_term);
        pd.set(6, maxk * channels_g * num_output_g);// weight_data_size
        pd.set(8, int8_scale_term);

        op->load_param(pd);

        // set weights
        if (bias_term)
        {
            ncnn::Mat weights[4];
            weights[0] = weight_data_g;
            weights[1] = bias_data_g;

            if (int8_scale_term)
            {
                weights[2] = weight_data_int8_scales.range(g, 1);
                weights[3] = bottom_blob_int8_scales.range(g, 1);     
            }

            op->load_model(ModelBinFromMatArray(weights));
        }
        else
        {
            ncnn::Mat weights[3];
            weights[0] = weight_data_g;

            if (int8_scale_term)
            {
                weights[1] = weight_data_int8_scales.range(g, 1);
                weights[2] = bottom_blob_int8_scales.range(g, 1);
            }

            op->load_model(ModelBinFromMatArray(weights));
        }

        op->create_pipeline(opt_cpu);

        group_ops[g] = op;
    }      

    return 0;
}

int ConvolutionDepthWise_x86::destroy_pipeline(const Option& opt)
{
    Option opt_cpu = opt;
    opt_cpu.use_vulkan_compute = false;

    if (activation)
    {
        activation->destroy_pipeline(opt_cpu);
        delete activation;
        activation = 0;
    }

    for (int i=0; i<(int)group_ops.size(); i++)
    {
        group_ops[i]->destroy_pipeline(opt_cpu);
        delete group_ops[i];
    }
    group_ops.clear();

    return 0;
}

int ConvolutionDepthWise_x86::forward(const Mat& bottom_blob, Mat& top_blob, const Option& opt) const
{
    // convolv with NxN kernel
    // value = value + bias

    int w = bottom_blob.w;
    int h = bottom_blob.h;
    int channels = bottom_blob.c;
    size_t elemsize = bottom_blob.elemsize;

    if (channels % group != 0 || num_output % group != 0)
    {
        // reject invalid group
        return -100;
    }

    const int kernel_extent_w = dilation_w * (kernel_w - 1) + 1;
    const int kernel_extent_h = dilation_h * (kernel_h - 1) + 1;

    Mat bottom_blob_unbordered = bottom_blob;
    if (use_int8_inference && elemsize != 1)
    {
        Mat bottom_blob_int8;
        bottom_blob_int8.create(w, h, channels, (size_t)1u, opt.workspace_allocator);
        if (bottom_blob_int8.empty())
            return -100;

        const int channels_g = channels / group;

        // quantize, scale and round to nearest
        #pragma omp parallel for num_threads(opt.num_threads)
        for (int g=0; g<group; g++)
        {
            ncnn::Option opt_g = opt;
            opt_g.num_threads = 1;
            opt_g.blob_allocator = bottom_blob_int8.allocator;

            const Mat bottom_blob_g = bottom_blob.channel_range(channels_g * g, channels_g);
            Mat bottom_blob_int8_g = bottom_blob_int8.channel_range(channels_g * g, channels_g);
            quantize_ops[g]->forward(bottom_blob_g, bottom_blob_int8_g, opt_g);
        }

        bottom_blob_unbordered = bottom_blob_int8;       
    }     

    Mat bottom_blob_bordered = bottom_blob_unbordered;
    if (pad_w > 0 || pad_h > 0)
    {
        copy_make_border(bottom_blob_unbordered, bottom_blob_bordered, pad_h, pad_h, pad_w, pad_w, BORDER_CONSTANT, 0.f, opt.workspace_allocator, opt.num_threads);
        if (bottom_blob_bordered.empty())
            return -100;

        w = bottom_blob_bordered.w;
        h = bottom_blob_bordered.h;
    }
    else if (pad_w == -233 && pad_h == -233)
    {
        int wpad = kernel_extent_w + (w - 1) / stride_w * stride_w - w;
        int hpad = kernel_extent_h + (h - 1) / stride_h * stride_h - h;
        if (wpad > 0 || hpad > 0)
        {
            copy_make_border(bottom_blob_unbordered, bottom_blob_bordered, hpad / 2, hpad - hpad / 2, wpad / 2, wpad - wpad / 2, BORDER_CONSTANT, 0.f, opt.workspace_allocator, opt.num_threads);
            if (bottom_blob_bordered.empty())
                return -100;
        }

        w = bottom_blob_bordered.w;
        h = bottom_blob_bordered.h;
    }

    int outw = (w - kernel_extent_w) / stride_w + 1;
    int outh = (h - kernel_extent_h) / stride_h + 1;

    // int8
    if (use_int8_inference)
    {
        if (use_int8_requantize)
        {
            Mat top_blob_tm;
            top_blob_tm.create(outw, outh, num_output, (size_t)4u, opt.workspace_allocator);
            if (top_blob_tm.empty())
                return -100;
            
            top_blob.create(outw, outh, num_output, (size_t)1u, opt.blob_allocator);
            if (top_blob.empty())
                return -100;

            // depth-wise
            if (channels == group && group == num_output)
            {                
                if (kernel_w == 3 && kernel_h == 3 && dilation_w == 1 && dilation_h == 1)
                {
                    if ((stride_w == 1 && stride_h == 1) || (stride_w == 2 && stride_h == 2))
                    {
                        if (stride_w == 1 && stride_h == 1)
                        {
                            convdw3x3s1_int8_requant_sse(bottom_blob_bordered, top_blob, weight_data, bias_data, requantize_scales, opt);                          
                        }
                        else if (stride_w == 2 && stride_h == 2)
                        {
                            convdw3x3s2_int8_requant_sse(bottom_blob_bordered, top_blob, weight_data, bias_data, requantize_scales, opt);                           
                        }

                        return 0;
                    }
                }

                #pragma omp parallel for num_threads(opt.num_threads)
                for (int g=0; g<group; g++)
                {
                    const Mat bottom_blob_bordered_g = bottom_blob_bordered.channel_range(g, 1);
                    Mat top_blob_tm_g = top_blob_tm.channel_range(g, 1);

                    const ncnn::Layer* op = group_ops[g];

                    ncnn::Option opt_g = opt;
                    opt_g.num_threads = 1;
                    opt_g.blob_allocator = top_blob.allocator;

                    // forward
                    op->forward(bottom_blob_bordered_g, top_blob_tm_g, opt_g);
                }

                return 0;
            }

            const int channels_g = channels / group;
            const int num_output_g = num_output / group;

            #pragma omp parallel for num_threads(opt.num_threads)
            for (int g=0; g<group; g++)
            {
                const Mat bottom_blob_bordered_g = bottom_blob_bordered.channel_range(channels_g * g, channels_g);
                Mat top_blob_tm_g = top_blob_tm.channel_range(num_output_g * g, num_output_g);

                const ncnn::Layer* op = group_ops[g];

                ncnn::Option opt_g = opt;
                opt_g.blob_allocator = top_blob.allocator;

                // forward
                op->forward(bottom_blob_bordered_g, top_blob_tm_g, opt_g);
            }       
        }
        else
        {
            top_blob.create(outw, outh, num_output, (size_t)4u, opt.blob_allocator);
            if (top_blob.empty())
                return -100;

            // depth-wise
            if (channels == group && group == num_output)
            {                
                if (kernel_w == 3 && kernel_h == 3 && dilation_w == 1 && dilation_h == 1)
                {
                    if ((stride_w == 1 && stride_h == 1) || (stride_w == 2 && stride_h == 2))
                    {
                        if (stride_w == 1 && stride_h == 1)
                        {
                            convdw3x3s1_int8_dequant_sse(bottom_blob_bordered, top_blob, weight_data, bias_data, dequantize_scales, opt);
                        }
                        else if (stride_w == 2 && stride_h == 2)
                        {
                            convdw3x3s2_int8_dequant_sse(bottom_blob_bordered, top_blob, weight_data, bias_data, dequantize_scales, opt);                          
                        }

                        return 0;
                    }
                }

                #pragma omp parallel for num_threads(opt.num_threads)
                for (int g=0; g<group; g++)
                {
                    const Mat bottom_blob_bordered_g = bottom_blob_bordered.channel_range(g, 1);
                    Mat top_blob_g = top_blob.channel_range(g, 1);

                    const ncnn::Layer* op = group_ops[g];

                    ncnn::Option opt_g = opt;
                    opt_g.num_threads = 1;
                    opt_g.blob_allocator = top_blob.allocator;

                    // forward
                    op->forward(bottom_blob_bordered_g, top_blob_g, opt_g);
                }
              
                return 0;
            }

            const int channels_g = channels / group;
            const int num_output_g = num_output / group;

            #pragma omp parallel for num_threads(opt.num_threads)
            for (int g=0; g<group; g++)
            {
                const Mat bottom_blob_bordered_g = bottom_blob_bordered.channel_range(channels_g * g, channels_g);
                Mat top_blob_g = top_blob.channel_range(num_output_g * g, num_output_g);

                const ncnn::Layer* op = group_ops[g];

                ncnn::Option opt_g = opt;
                opt_g.blob_allocator = top_blob.allocator;

                // forward
                op->forward(bottom_blob_bordered_g, top_blob_g, opt_g);
            }                      
        }

        return 0;
    }

    // float32
    top_blob.create(outw, outh, num_output, elemsize, opt.blob_allocator);
    if (top_blob.empty())
        return -100;
    
    // depth-wise
    if (channels == group && group == num_output)
    {
        if (kernel_w == 3 && kernel_h == 3 && dilation_w == 1 && dilation_h == 1)
        {
            if (stride_w == 1 && stride_h == 1)
            {
                convdw3x3s1_sse(bottom_blob_bordered, top_blob, weight_data, bias_data, opt);
            }
            else if (stride_w == 2 && stride_h == 2)
            {
                convdw3x3s2_sse(bottom_blob_bordered, top_blob, weight_data, bias_data, opt);
            }

            if (activation)
            {
                activation->forward_inplace(top_blob, opt);
            }

            return 0;
        }

        #pragma omp parallel for num_threads(opt.num_threads)
        for (int g=0; g<group; g++)
        {
            const Mat bottom_blob_bordered_g = bottom_blob_bordered.channel_range(g, 1);
            Mat top_blob_g = top_blob.channel_range(g, 1);

            const ncnn::Layer* op = group_ops[g];

            ncnn::Option opt_g = opt;
            opt_g.num_threads = 1;
            opt_g.blob_allocator = top_blob.allocator;

         // forward
            op->forward(bottom_blob_bordered_g, top_blob_g, opt_g);
        }

        if (activation)
        {
            activation->forward_inplace(top_blob, opt);
        }

        return 0;
    }

    const int channels_g = channels / group;
    const int num_output_g = num_output / group;

    for (int g=0; g<group; g++)
    {
        const Mat bottom_blob_bordered_g = bottom_blob_bordered.channel_range(channels_g * g, channels_g);
        Mat top_blob_g = top_blob.channel_range(num_output_g * g, num_output_g);

        const ncnn::Layer* op = group_ops[g];

        ncnn::Option opt_g = opt;
        opt_g.blob_allocator = top_blob.allocator;

        // forward
        op->forward(bottom_blob_bordered_g, top_blob_g, opt_g);
    }

    if (activation)
    {
        activation->forward_inplace(top_blob, opt);
    }

    return 0;
}

} // namespace ncnn

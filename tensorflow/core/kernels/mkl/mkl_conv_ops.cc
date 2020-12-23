/* Copyright 2015 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

// See docs in ../ops/nn_ops.cc.
#ifdef INTEL_MKL

#include "tensorflow/core/kernels/mkl/mkl_conv_ops.h"

#include <algorithm>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "absl/strings/str_join.h"
#include "mkldnn.hpp"
#include "tensorflow/core/framework/bounds_check.h"
#include "tensorflow/core/framework/numeric_op.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/register_types.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/tensor_shape.h"
#include "tensorflow/core/framework/tensor_slice.h"
#include "tensorflow/core/kernels/mkl/mkl_quantized_conv_ops.h"
#include "tensorflow/core/kernels/no_op.h"
#include "tensorflow/core/kernels/ops_util.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/gtl/array_slice.h"
#include "tensorflow/core/lib/strings/numbers.h"
#include "tensorflow/core/lib/strings/str_util.h"
#include "tensorflow/core/lib/strings/strcat.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/macros.h"
#include "tensorflow/core/util/mkl_util.h"
#include "tensorflow/core/util/padding.h"
#include "tensorflow/core/util/tensor_format.h"

using mkldnn::convolution_forward;
using mkldnn::prop_kind;
using mkldnn::stream;
using ConvFwdPd = mkldnn::convolution_forward::primitive_desc;
using ReorderPd = mkldnn::reorder::primitive_desc;

namespace tensorflow {
// This structure aggregates multiple inputs to Conv2DFwd* methods.
struct MklConvFwdParams {
  memory::dims src_dims;
  memory::dims filter_dims;
  memory::dims bias_dims;
  memory::dims dst_dims;
  memory::dims strides;
  memory::dims dilations;
  memory::dims padding_left;
  memory::dims padding_right;
  MklTensorFormat tf_fmt;
  string dtypes = string("");
  struct PostOpParam {
    string name;
    mkldnn::algorithm alg;
    std::vector<float> param;
    std::string partial_key;
  };
  std::vector<PostOpParam> post_op_params;

  MklConvFwdParams(memory::dims src_dims, memory::dims filter_dims,
                   memory::dims bias_dims, memory::dims dst_dims,
                   memory::dims strides, memory::dims dilations,
                   memory::dims padding_left, memory::dims padding_right,
                   MklTensorFormat tf_fmt)
      : src_dims(src_dims),
        filter_dims(filter_dims),
        bias_dims(bias_dims),
        dst_dims(dst_dims),
        strides(strides),
        dilations(dilations),
        padding_left(padding_left),
        padding_right(padding_right),
        tf_fmt(tf_fmt) {}
};

// With quantization, input, filter, and output can have different types
// so we use different template parameter for each type
template <typename Tinput, typename Tfilter, typename Tbias, typename Toutput>
class MklConvFwdPrimitive : public MklPrimitive {
 public:
  explicit MklConvFwdPrimitive(const MklConvFwdParams& convFwdDims)
      : MklPrimitive(engine(engine::kind::cpu, 0)) {
    // Create convolution primitive
    if (context_.conv_fwd == nullptr) {
      Setup(convFwdDims);
    }
  }
  ~MklConvFwdPrimitive() {}

  // Convolution forward execute with bias
  //   src_data:    input data buffer of src
  //   filter_data: input data buffer of filter (weights)
  //   bias_data:   input data buffer of bias
  //   dst_data:    output data buffer of dst
  void Execute(const Tinput* src_data, const Tfilter* filter_data,
               const Tbias* bias_data, const Toutput* dst_data,
               std::shared_ptr<stream> fwd_stream) {
#ifdef ENABLE_MKLDNN_THREADPOOL
    // TODO: Create a common function and avoid the duplicate code
    context_.src_mem->set_data_handle(
        static_cast<void*>(const_cast<Tinput*>(src_data)), *fwd_stream);
    context_.filter_mem->set_data_handle(
        static_cast<void*>(const_cast<Tfilter*>(filter_data)), *fwd_stream);
    if (bias_data != nullptr) {
      context_.bias_mem->set_data_handle(
          static_cast<void*>(const_cast<Tbias*>(bias_data)), *fwd_stream);
    }
    context_.dst_mem->set_data_handle(
        static_cast<void*>(const_cast<Toutput*>(dst_data)), *fwd_stream);
#else
    context_.src_mem->set_data_handle(
        static_cast<void*>(const_cast<Tinput*>(src_data)));
    context_.filter_mem->set_data_handle(
        static_cast<void*>(const_cast<Tfilter*>(filter_data)));
    if (bias_data != nullptr) {
      context_.bias_mem->set_data_handle(
          static_cast<void*>(const_cast<Tbias*>(bias_data)));
    }
    context_.dst_mem->set_data_handle(
        static_cast<void*>(const_cast<Toutput*>(dst_data)));
#endif  // ENABLE_MKLDNN_THREADPOOL

    DCHECK_EQ(context_.fwd_primitives.size(),
              context_.fwd_primitives_args.size());
    for (size_t i = 0; i < context_.fwd_primitives.size(); ++i) {
      context_.fwd_primitives.at(i).execute(*fwd_stream,
                                            context_.fwd_primitives_args.at(i));
    }

    // After execution, set data handle back
    context_.src_mem->set_data_handle(DummyData);
    context_.filter_mem->set_data_handle(DummyData);
    if (bias_data != nullptr) {
      context_.bias_mem->set_data_handle(DummyData);
    }
    context_.dst_mem->set_data_handle(DummyData);
  }

  // Convolution forward execute without bias
  //   src_data:    input data buffer of src
  //   filter_data: input data buffer of filter (weights)
  //   dst_data:    output data buffer of dst
  void Execute(const Tinput* src_data, const Tfilter* filter_data,
               const Toutput* dst_data, std::shared_ptr<stream> fwd_stream) {
    Execute(src_data, filter_data, nullptr, dst_data, fwd_stream);
  }

  std::shared_ptr<ConvFwdPd> GetPrimitiveDesc() const {
    return context_.fwd_pd;
  }

 private:
  // Primitive reuse context for Conv2D Fwd op
  struct ConvFwdContext {
    // MKL-DNN memory
    std::shared_ptr<mkldnn::memory> src_mem;
    std::shared_ptr<mkldnn::memory> filter_mem;
    std::shared_ptr<mkldnn::memory> bias_mem;
    std::shared_ptr<mkldnn::memory> dst_mem;

    // Desc & primitive desc
    std::shared_ptr<mkldnn::convolution_forward::desc> fwd_desc;

    // Memory desc
    std::shared_ptr<mkldnn::memory::desc> src_md;
    std::shared_ptr<mkldnn::memory::desc> filter_md;
    std::shared_ptr<mkldnn::memory::desc> bias_md;
    std::shared_ptr<mkldnn::memory::desc> dst_md;

    // Convolution primitive
    std::shared_ptr<ConvFwdPd> fwd_pd;
    std::shared_ptr<mkldnn::primitive> conv_fwd;

    std::vector<mkldnn::primitive> fwd_primitives;
    std::vector<std::unordered_map<int, memory>> fwd_primitives_args;

    ConvFwdContext()
        : src_mem(nullptr),
          filter_mem(nullptr),
          bias_mem(nullptr),
          dst_mem(nullptr),
          fwd_desc(nullptr),
          src_md(nullptr),
          filter_md(nullptr),
          bias_md(nullptr),
          fwd_pd(nullptr),
          conv_fwd(nullptr) {}
  };

  void Setup(const MklConvFwdParams& convFwdDims) {
    memory::format_tag user_data_fmt =
        MklTensorFormatToMklDnnDataFormat(convFwdDims.tf_fmt);
    context_.src_md.reset(new memory::desc(
        {convFwdDims.src_dims}, MklDnnType<Tinput>(), user_data_fmt));

    context_.filter_md.reset(new memory::desc({convFwdDims.filter_dims},
                                              MklDnnType<Tfilter>(),
                                              memory::format_tag::any));

    context_.dst_md.reset(new memory::desc(
        {convFwdDims.dst_dims}, MklDnnType<Toutput>(), user_data_fmt));

    if (!convFwdDims.bias_dims.empty())
      context_.bias_md.reset(new memory::desc({convFwdDims.bias_dims},
                                              MklDnnType<Tbias>(),
                                              memory::format_tag::any));

    // Create a convolution descriptor
    if (!convFwdDims.bias_dims.empty()) {
      context_.fwd_desc.reset(new convolution_forward::desc(
          prop_kind::forward, mkldnn::algorithm::convolution_direct,
          *context_.src_md, *context_.filter_md, *context_.bias_md,
          *context_.dst_md, convFwdDims.strides, convFwdDims.dilations,
          convFwdDims.padding_left, convFwdDims.padding_right));
    } else {
      context_.fwd_desc.reset(new convolution_forward::desc(
          prop_kind::forward, mkldnn::algorithm::convolution_direct,
          *context_.src_md, *context_.filter_md, *context_.dst_md,
          convFwdDims.strides, convFwdDims.dilations, convFwdDims.padding_left,
          convFwdDims.padding_right));
    }

    context_.fwd_pd.reset(new ConvFwdPd(*context_.fwd_desc, cpu_engine_));

    // Check if there is any fusions as post-ops
    auto const& post_op_params = convFwdDims.post_op_params;
    mkldnn::primitive_attr post_ops_attr;
    mkldnn::post_ops post_ops;
    if (!post_op_params.empty()) {
      for (auto const& post_op_param : post_op_params) {
        if (post_op_param.name == "activation") {
          DCHECK_EQ(post_op_param.param.size(), 3);
          float op_scale = post_op_param.param[0];
          float op_alpha = post_op_param.param[1];
          float op_beta = post_op_param.param[2];
          post_ops.append_eltwise(op_scale, post_op_param.alg, op_alpha,
                                  op_beta);
        } else if (post_op_param.name == "sum") {
          DCHECK_EQ(post_op_param.param.size(), 1);
          float op_scale = post_op_param.param[0];
          post_ops.append_sum(op_scale);
        } else if (post_op_param.name == "output_scale") {
          if (post_op_param.param.size() == 1) {
            post_ops_attr.set_output_scales(0, post_op_param.param);
          } else {
            post_ops_attr.set_output_scales(2, post_op_param.param);
          }
        } else {
          DCHECK((post_op_param.name == "activation") ||
                 (post_op_param.name == "sum") ||
                 (post_op_param.name == "output_scale"));
        }
      }
      post_ops_attr.set_post_ops(post_ops);
      context_.fwd_pd.reset(
          new ConvFwdPd(*context_.fwd_desc, post_ops_attr, cpu_engine_));
    } else {
      context_.fwd_pd.reset(new ConvFwdPd(*context_.fwd_desc, cpu_engine_));
    }

    // Create memory primitive based on dummy data
    context_.src_mem.reset(
        new memory(context_.fwd_pd.get()->src_desc(), cpu_engine_, DummyData));
    context_.filter_mem.reset(new memory(context_.fwd_pd.get()->weights_desc(),
                                         cpu_engine_, DummyData));
    context_.dst_mem.reset(
        new memory(context_.fwd_pd.get()->dst_desc(), cpu_engine_, DummyData));

    // Create convolution primitive and add it to net
    if (!convFwdDims.bias_dims.empty()) {
      context_.bias_mem.reset(new memory(
          {{convFwdDims.bias_dims}, MklDnnType<Tbias>(), memory::format_tag::x},
          cpu_engine_, DummyData));
      context_.conv_fwd.reset(new convolution_forward(*context_.fwd_pd));
      context_.fwd_primitives_args.push_back(
          {{MKLDNN_ARG_SRC, *context_.src_mem},
           {MKLDNN_ARG_WEIGHTS, *context_.filter_mem},
           {MKLDNN_ARG_BIAS, *context_.bias_mem},
           {MKLDNN_ARG_DST, *context_.dst_mem}});
    } else {
      context_.conv_fwd.reset(new convolution_forward(*context_.fwd_pd));
      context_.fwd_primitives_args.push_back(
          {{MKLDNN_ARG_SRC, *context_.src_mem},
           {MKLDNN_ARG_WEIGHTS, *context_.filter_mem},
           {MKLDNN_ARG_DST, *context_.dst_mem}});
    }
    context_.fwd_primitives.push_back(*context_.conv_fwd);
  }

  struct ConvFwdContext context_;
};

// Base class for convolution forward operations
template <typename Device, typename Tinput, typename Tfilter, typename Tbias,
          typename Toutput, typename Ttemp_output, typename Tpadding,
          bool bias_enabled, bool pad_enabled, bool is_depthwise>
class MklConvOp : public OpKernel {
 public:
  ~MklConvOp() {}

  explicit MklConvOp(OpKernelConstruction* context) : OpKernel(context) {
    OP_REQUIRES_OK(context, context->GetAttr("dilations", &dilations_));

    // Conv and QuantizedConv ops have different padding attributes
    // (`padding_list` versus `explicit_paddings`). But one and only one
    // attribute is expected.
    OP_REQUIRES(
        context,
        !(context->HasAttr("padding_list") &&
          context->HasAttr("explicit_paddings")),
        errors::InvalidArgument("Can only have 1 `padding` list at most"));
    if (context->HasAttr("padding_list")) {
      OP_REQUIRES_OK(context, context->GetAttr("padding_list", &padding_list_));
    }
    if (context->HasAttr("explicit_paddings")) {
      OP_REQUIRES_OK(context,
                     context->GetAttr("explicit_paddings", &padding_list_));
    }

    OP_REQUIRES_OK(context, context->GetAttr("strides", &strides_));
    string data_format;
    OP_REQUIRES_OK(context, context->GetAttr("data_format", &data_format));
    OP_REQUIRES(context, FormatFromString(data_format, &data_format_),
                errors::InvalidArgument("Invalid data format"));
    OP_REQUIRES(context, (strides_.size() == 4 || strides_.size() == 5),
                errors::InvalidArgument("Sliding window strides field must "
                                        "specify 4 or 5 dimensions"));

    const int64 stride_n = GetTensorDim(strides_, data_format_, 'N');
    const int64 stride_c = GetTensorDim(strides_, data_format_, 'C');
    OP_REQUIRES(
        context, stride_n == 1 && stride_c == 1,
        errors::Unimplemented("Current implementation does not yet support "
                              "strides in the batch and depth dimensions."));

    OP_REQUIRES_OK(context, context->GetAttr("padding", &padding_));
    is_filter_const_ = false;
    if (context->HasAttr("is_filter_const")) {
      OP_REQUIRES_OK(context,
                     context->GetAttr("is_filter_const", &is_filter_const_));
    }

    if (strides_.size() == 4) {
      OP_REQUIRES(context, dilations_.size() == 4,
                  errors::InvalidArgument("Sliding window dilations field must "
                                          "specify 4 dimensions"));
      const int64 dilation_n = GetTensorDim(dilations_, data_format_, 'N');
      const int64 dilation_c = GetTensorDim(dilations_, data_format_, 'C');
      const int64 dilation_h = GetTensorDim(dilations_, data_format_, 'H');
      const int64 dilation_w = GetTensorDim(dilations_, data_format_, 'W');
      OP_REQUIRES(context, dilation_n == 1 && dilation_c == 1,
                  errors::InvalidArgument(
                      "Current implementation does not yet support "
                      "dilations in the batch and depth dimensions."));
      OP_REQUIRES(
          context, dilation_h > 0 && dilation_w > 0,
          errors::InvalidArgument("Dilated rates should be larger than 0."));
    } else if (strides_.size() == 5) {
      OP_REQUIRES(context, dilations_.size() == 5,
                  errors::InvalidArgument("Dilation rates field must "
                                          "specify 5 dimensions"));
      OP_REQUIRES(context,
                  (GetTensorDim(dilations_, data_format_, 'N') == 1 &&
                   GetTensorDim(dilations_, data_format_, 'C') == 1),
                  errors::InvalidArgument(
                      "Current implementation does not yet support "
                      "dilations rates in the batch and depth dimensions."));
      OP_REQUIRES(
          context,
          (GetTensorDim(dilations_, data_format_, '0') > 0 &&
           GetTensorDim(dilations_, data_format_, '1') > 0 &&
           GetTensorDim(dilations_, data_format_, '2') > 0),
          errors::InvalidArgument("Dilated rates should be larger than 0."));
    }
  }

  void Compute(OpKernelContext* context) override {
    try {
      // Input tensors
      const Tensor& src_tensor = context->input(kInputIndex_Src);
      const Tensor& filter_tensor = context->input(kInputIndex_Filter);

      MklDnnData<Tfilter> filter(&cpu_engine_);

      memory::dims src_dims, filter_dims, padding_left, padding_right,
          dilations, strides;
      memory::dims dst_dims_tf_order, dst_dims_mkl_order;

      // For any Conv with `EXPLICIT` padding, get padding from `padding_list`
      // attribute. Otherwise, get it from one of the inputs.
      bool pad_attr_enabled = false;
      for (auto const& padding_val : padding_list_) {
        if (padding_val) {
          pad_attr_enabled = true;

          break;
        }
      }

      if (fuse_pad_ || pad_attr_enabled) {
        PadWithConvFusion(context, padding_left, padding_right,
                          pad_attr_enabled);
      }

      // Get shapes of input tensors in MKL-DNN order
      MklDnnConvUtil conv_utl(context, strides_, padding_, data_format_,
                              dilations_);
      auto src_tf_shape = src_tensor.shape();
      auto filter_tf_shape = filter_tensor.shape();

      conv_utl.GetConvFwdSizesInMklOrder(
          src_tf_shape, filter_tf_shape, &src_dims, &filter_dims, &strides,
          &dilations, &dst_dims_tf_order, &dst_dims_mkl_order, &padding_left,
          &padding_right, (fuse_pad_ || pad_attr_enabled), is_depthwise);

      if (!context->status().ok()) return;

      // Check for corner case - if there is nothing to compute, return.
      TensorShape dst_tf_shape = MklDnnDimsToTFShape(dst_dims_tf_order);

      // Corner cases: output with 0 elements and 0 batch size.
      Tensor* dst_tensor = nullptr;
      if (dst_tf_shape.num_elements() == 0 || dst_dims_tf_order[0] == 0) {
        OP_REQUIRES_OK(context,
                       context->allocate_output(kOutputIndex_Dst, src_tf_shape,
                                                &dst_tensor));
        return;
      }

      bool is_conv2d = (strides_.size() == 4);

      if (!is_conv2d) {
        OP_REQUIRES(
            context, !pad_enabled,
            errors::InvalidArgument("Pad + Conv fusion only works for 2D"));
        OP_REQUIRES(
            context, !fuse_pad_,
            errors::InvalidArgument("Pad+Conv fusion only works for 2D"));
      }

      // TODO(gzmkl) 3-D support for Depthwise is not there
      if (is_depthwise) {
        OP_REQUIRES(context, is_conv2d,
                    errors::InvalidArgument(
                        "Only 2D convolution is supported for depthwise."));
      }

      // Create memory for user data.
      // Describe how the inputs and outputs of Convolution look like. Also
      // specify buffers containing actual input and output data.
      auto tf_fmt = is_conv2d ? TFDataFormatToMklDnnDataFormat(data_format_)
                              : TFDataFormatToMklDnn3DDataFormat(data_format_);

      auto mkl_fmt_tag = MklTensorFormatToMklDnnDataFormat(tf_fmt);
      // NOTE: `mkl_fmt_tag` will be `format_tag::undef` for ReLU
      OP_REQUIRES(context, mkl_fmt_tag != memory::format_tag::undef,
                  errors::InvalidArgument("Invalid data format"));

      // For constructing TF layout for input, although input shape (src_dims)
      // is required to be in MKL-DNN order, the input layout is actually in
      // TF layout depending on the data format:
      //     Conv2D: NHWC or NCHW
      //     Conv3D: NDHWC or NCDHW
      auto src_md = memory::desc(src_dims, MklDnnType<Tinput>(), mkl_fmt_tag);

      // Although filter shape (filter_dims) required is in MKL-DNN order,
      // the layout is Tensorflow's layout (HWIO) and (HWIGO) for
      // depthwise/group convolutions.
      auto filter_format = is_conv2d ? (is_depthwise ? memory::format_tag::hwigo
                                                     : memory::format_tag::hwio)
                                     : memory::format_tag::dhwio;

      auto filter_md =
          memory::desc(filter_dims, MklDnnType<Tfilter>(), filter_format);
      filter.SetUsrMem(filter_md, &filter_tensor);

      // MKL-DNN dilations start from 0.
      for (int i = 0; i < dilations.size(); ++i) --dilations[i];

      // Get a conv2d fwd from primitive pool
      MklConvFwdPrimitive<Tinput, Tfilter, Tbias, Ttemp_output>* conv_fwd =
          nullptr;
      memory::dims bias_dims = {};
      if (fuse_biasadd_) {
        conv_utl.GetBiasSizeInMklOrder(kInputIndex_Bias, &bias_dims);
      }
      MklConvFwdParams convFwdDims(src_dims, filter_dims,
                                   fuse_biasadd_ ? bias_dims : NONE_DIMS,
                                   dst_dims_mkl_order, strides, dilations,
                                   padding_left, padding_right, tf_fmt);

      // TODO(mdfaijul): Extend the basic parameters for data types and fusions
      this->ExtendConvFwdParams(context, convFwdDims);
      conv_fwd = new MklConvFwdPrimitive<Tinput, Tfilter, Tbias, Ttemp_output>(
          convFwdDims);
      // Allocate output tensors `dst_tensor` and `filter_out_tensor`
      std::shared_ptr<ConvFwdPd> conv_fwd_pd = conv_fwd->GetPrimitiveDesc();
      AllocateOutputTensor(context, *conv_fwd_pd, dst_dims_mkl_order,
                           data_format_, is_conv2d, &dst_tensor);

      Tensor* filter_out_tensor = nullptr;

      Ttemp_output* dst_data =
          reinterpret_cast<Ttemp_output*>(dst_tensor->flat<Toutput>().data());

      // Check whether filter needs to be reordered.
      Tinput* src_data = nullptr;
      src_data = static_cast<Tinput*>(
          const_cast<Tinput*>(src_tensor.flat<Tinput>().data()));

      Tfilter* filter_data = nullptr;
      if (filter_md != conv_fwd_pd->weights_desc()) {
        bool is_filter_cached = false;
        // If filter is a constant, we can avoid the conversion of filter from
        // Tensorflow format to MKL format by caching the filter when it is
        // converted for the first time. This cached filter can then be reused
        // in subsequent iterations.
        if (is_filter_const_) {
          if (IsFilterCacheEmpty(context)) {
            // Cache filter if it is not already cached.
            CacheFilter(context, conv_fwd_pd, filter_data, filter_tensor,
                        filter, filter_md);
          }
          filter_data = GetCachedFilter(context, conv_fwd_pd->weights_desc());
          is_filter_cached = (filter_data != nullptr);
        }
        if (!is_filter_cached) {
          filter.SetUsrMem(filter_md, &filter_tensor);
          if (filter_out_tensor == nullptr) {
            filter.CheckReorderToOpMem(conv_fwd_pd->weights_desc(), cpu_engine_,
                                       context);
          } else {
            filter.CheckReorderToOpMem(
                conv_fwd_pd->weights_desc(),
                filter.GetTensorBuffer(filter_out_tensor), cpu_engine_,
                context);
          }
          filter_data =
              static_cast<Tfilter*>(filter.GetOpMem().get_data_handle());
        }
      } else {
        filter_data = static_cast<Tfilter*>(
            const_cast<Tfilter*>(filter_tensor.flat<Tfilter>().data()));
      }

      // Execute convolution
      std::shared_ptr<stream> fwd_cpu_stream;
      fwd_cpu_stream.reset(CreateStream(context, conv_fwd->GetEngine()));
      if (fuse_biasadd_) {
        const Tensor& bias_tensor = context->input(kInputIndex_Bias);
        Tbias* bias_data =
            this->GetBiasHandle(context, conv_fwd_pd, bias_tensor);
        conv_fwd->Execute(src_data, filter_data, bias_data, dst_data,
                          fwd_cpu_stream);
      } else {
        conv_fwd->Execute(src_data, filter_data, dst_data, fwd_cpu_stream);
      }

      delete conv_fwd;

    } catch (mkldnn::error& e) {
      string error_msg = tensorflow::strings::StrCat(
          "Status: ", e.status, ", message: ", string(e.message), ", in file ",
          __FILE__, ":", __LINE__);
      OP_REQUIRES_OK(
          context,
          errors::Aborted("Operation received an exception:", error_msg));
    }
  }

  void PadWithConvFusion(OpKernelContext* context, memory::dims& padding_left,
                         memory::dims& padding_right, bool pad_attr_enabled) {
    Tpadding* paddings = nullptr;
    if (pad_attr_enabled) {
      paddings = padding_list_.data();
    } else {
      const Tensor& paddings_tf = context->input(input_index_pad_);
      OP_REQUIRES(context, paddings_tf.dims() == 2,
                  errors::InvalidArgument("paddings must be 2-dimensional: ",
                                          paddings_tf.shape().DebugString()));
      // Flatten tensor to get individual paddings.
      paddings = static_cast<Tpadding*>(
          const_cast<Tpadding*>(paddings_tf.flat<Tpadding>().data()));
    }
    // If the data format is NHWC, indices 0, 1, 6 and 7 of paddings(_tf)
    // will be zero.
    // Example:
    // paddings_tf = [ [0, 0] [1, 2] [3, 4] [0, 0] ],
    // flat method = row-major, then:
    // paddings = {0, 0, 1, 2, 3, 4, 0, 0}.
    // Hence, the values are: top = 1, bottom = 2, left = 3, right = 4.
    //
    // Similarly, if the data format is NCHW, indices 0, 1, 2 and 3 of
    // paddings(_tf) will be zero.
    // i.e. for the above example, paddings = {0, 0, 0, 0, 1, 2, 3, 4}.
    int64 pad_top = 0, pad_left = 0;
    int64 pad_bottom = 0, pad_right = 0;
    string data_format = ToString(data_format_);
    if (data_format == "NHWC") {
      pad_top = paddings[2];
      pad_bottom = paddings[3];
      pad_left = paddings[4];
      pad_right = paddings[5];
    } else if (data_format == "NCHW") {
      pad_top = paddings[4];
      pad_bottom = paddings[5];
      pad_left = paddings[6];
      pad_right = paddings[7];
    }
    // Create padding arrays for MKL-DNN convolutions.
    // MKL-DNN uses asymmetric padding.
    padding_left = {static_cast<int>(pad_top), static_cast<int>(pad_left)};
    padding_right = {static_cast<int>(pad_bottom), static_cast<int>(pad_right)};
  }

 protected:
  void set_fuse_biasadd(bool fuse_biasadd) { fuse_biasadd_ = fuse_biasadd; }
  void set_fuse_activation(bool fuse_activation,
                           mkldnn::algorithm activation_alg,
                           float alpha_or_upbound = 0.0) {
    fuse_activation_ = fuse_activation;
    activation_alg_ = activation_alg;
    // This variable is used for alpha in leakyrelu or upper bound in relu6
    // depending on the context
    alpha_or_upbound_ = alpha_or_upbound;
  }
  void set_fuse_pad(bool fuse_pad) {
    fuse_pad_ = fuse_pad;
    // In PadwithFusedConv OP, pad is the fourth index.
    input_index_pad_ = 3;
  }
  void set_fuse_add(bool fuse_add) { fuse_add_ = fuse_add; }

  // This method is for the base class MklConvOp, which handles the
  // floating point implementation of Conv. The quantized conv implementations
  // will use overridden versions of this method.
  virtual void ExtendConvFwdParams(OpKernelContext* context,
                                   MklConvFwdParams& params) {
    // Create a string from data types of input, filter, bias, and output.
    params.dtypes.append(typeid(Tinput).name());
    params.dtypes.append(typeid(Tfilter).name());
    params.dtypes.append(typeid(Tbias).name());
    params.dtypes.append(typeid(Toutput).name());

    // Add fusions as post ops
    // NOTE: Fusion of BiasAdd is handled directly inside MklConvOp by
    // checking `fuse_biasadd_` flag.
    if (fuse_add_) {
      params.post_op_params.push_back(
          {"sum", mkldnn::algorithm::undef, {1.0}, ""});
    }
    if (fuse_activation_) {
      params.post_op_params.push_back(
          {"activation", activation_alg_, {1.0, alpha_or_upbound_, 0.0}, ""});
    }
  }

  virtual Tbias* GetBiasHandle(OpKernelContext* context,
                               std::shared_ptr<ConvFwdPd>& conv2d_fwd_pd,
                               const Tensor& bias_tensor) {
    if (fuse_biasadd_) {
      return static_cast<Tbias*>(
          const_cast<Tbias*>(bias_tensor.flat<Tbias>().data()));
    }
    return nullptr;
  }

  virtual void AllocateOutputTensor(OpKernelContext* context,
                                    const ConvFwdPd& conv_prim_desc,
                                    const memory::dims& output_dims_mkl_order,
                                    TensorFormat output_tf_format,
                                    bool is_conv2d, Tensor** output_tensor) {
    DCHECK(output_tensor);
    auto dst_md = conv_prim_desc.dst_desc();

    if (!std::is_same<Ttemp_output, Toutput>::value) {
      dst_md.data.data_type =
          static_cast<mkldnn_data_type_t>(MklDnnType<Toutput>());
    }

    // Allocate shape of TF tensor
    TensorShape output_tf_shape;

    if (is_conv2d) {
      const int64 out_batch = output_dims_mkl_order[0];
      const int64 out_rows = output_dims_mkl_order[2];
      const int64 out_cols = output_dims_mkl_order[3];
      const int64 out_depth = output_dims_mkl_order[1];
      output_tf_shape = ShapeFromFormat(output_tf_format, out_batch, out_rows,
                                        out_cols, out_depth);
    } else {
      const int64 out_batch = output_dims_mkl_order[0];
      const int64 out_planes = output_dims_mkl_order[2];
      const int64 out_rows = output_dims_mkl_order[3];
      const int64 out_cols = output_dims_mkl_order[4];
      const int64 out_depth = output_dims_mkl_order[1];
      output_tf_shape =
          ShapeFromFormat(output_tf_format, out_batch,
                          {{out_planes, out_rows, out_cols}}, out_depth);
    }

    if (fuse_add_) {
      const Tensor& add_tensor = context->input(kInputIndex_Add);
      // Forward the summand tensor to the output only if it has no other
      // references, otherwise make a copy of it.
      if (context->forward_input_to_output_with_shape(
              kInputIndex_Add, kOutputIndex_Dst, output_tf_shape,
              output_tensor)) {
        return;
      } else {
        OP_REQUIRES_OK(
            context, context->allocate_output(kOutputIndex_Dst, output_tf_shape,
                                              output_tensor));
        void* add_buf = static_cast<void*>(
            const_cast<Toutput*>(add_tensor.flat<Toutput>().data()));
        void* dst_buf =
            static_cast<void*>((*output_tensor)->flat<Ttemp_output>().data());
        // We are simply deep copying the add_tensor to output_tensor without
        // changing memory layout, hence using same memory descriptor.
        auto add_md =
            memory::desc({add_tensor.NumElements()}, MklDnnType<Toutput>(),
                         mkldnn::memory::format_tag::x);
        dst_md = add_md;
        fuse_add_src_.reset(new memory(add_md, this->cpu_engine_, add_buf));
        fuse_add_dst_.reset(new memory(dst_md, this->cpu_engine_, dst_buf));
        auto reorder_desc =
            ReorderPd(this->cpu_engine_, add_md, this->cpu_engine_, dst_md);

        CreateAndExecuteReorder(reorder_desc, *fuse_add_src_, *fuse_add_dst_,
                                this->cpu_engine_, context);
      }
    } else {
      OP_REQUIRES_OK(context,
                     context->allocate_output(kOutputIndex_Dst, output_tf_shape,
                                              output_tensor));
    }
  }

  engine cpu_engine_ = engine(engine::kind::cpu, 0);

 private:
  std::shared_ptr<mkldnn::memory> fuse_add_src_;
  std::shared_ptr<mkldnn::memory> fuse_add_dst_;
  std::vector<int32> strides_;
  std::vector<int32> dilations_;
  std::vector<Tpadding> padding_list_;
  bool is_filter_const_;
  mutex mu_;
  Padding padding_;
  TensorFormat data_format_;
  PersistentTensor cached_filter_data_ptensor_ TF_GUARDED_BY(mu_);
  PersistentTensor cached_filter_md_ptensor_ TF_GUARDED_BY(mu_);

  // Initialize to values the template is instantiated with
  bool fuse_biasadd_ = bias_enabled;
  bool fuse_activation_ = false;
  bool fuse_pad_ = pad_enabled;
  bool fuse_add_ = false;

  // This variable is used for alpha in leakyrelu or upper bound in relu6
  // depending on the context
  float alpha_or_upbound_ = 0.0;
  mkldnn::algorithm activation_alg_ = mkldnn::algorithm::undef;

  int input_index_pad_ = 2;

  const int kInputIndex_Src = 0, kInputIndex_Filter = 1, kInputIndex_Bias = 2;
  const int kInputIndex_Add = 3;
  const int kOutputIndex_Dst = 0, kOutputIndex_Filter = 1;
  const int kDilationH = 0, kDilationW = 1;

  // Allocate persistent tensors for cached filter data and
  // cached filter memory descriptor (data format)
  void AllocatePersistentTensor(OpKernelContext* context,
                                const ConvFwdPd& conv_prim_desc,
                                Tensor** filter_tensor) {
    DCHECK(filter_tensor);
    TensorShape filter_tf_shape;
    filter_tf_shape.AddDim(
        (conv_prim_desc.weights_desc().get_size() / sizeof(Tfilter)));
    OP_REQUIRES_OK(context, context->allocate_persistent(
                                DataTypeToEnum<Tfilter>::value, filter_tf_shape,
                                &cached_filter_data_ptensor_, filter_tensor));

    Tensor* second_tensor = nullptr;

    // There is no tensor format in DNNL 1.x. So we cache the complete filter
    // descriptor as flat byte array.
    TensorShape cached_filter_md_shape;
    memory::desc weights_desc = conv_prim_desc.weights_desc();
    // We don't use .get_size() method of memory::desc since it returns size
    // required to store primitive's input memory. It is much more than size of
    // memory::desc itself.
    cached_filter_md_shape.AddDim(sizeof(weights_desc) / sizeof(uint8));
    OP_REQUIRES_OK(context, context->allocate_persistent(
                                DT_UINT8, cached_filter_md_shape,
                                &cached_filter_md_ptensor_, &second_tensor));
    *reinterpret_cast<memory::desc*>(second_tensor->flat<uint8>().data()) =
        weights_desc;
  }

  // TF_LOCKS_EXCLUDED annotation ensures that the lock (mu_) cannot
  // be acquired before entering the function, since it is acquired
  // inside the function.
  inline bool IsFilterCacheEmpty(OpKernelContext* context)
      TF_LOCKS_EXCLUDED(mu_) {
    tf_shared_lock lock(mu_);
    const Tensor& cached_filter_data_tensor =
        *cached_filter_data_ptensor_.AccessTensor(context);
    return (cached_filter_data_tensor.NumElements() == 0);
  }

  // Cache the converted filter in a persistent tensor.
  // Only one thread can execute this method at any given time.
  void CacheFilter(OpKernelContext* context,
                   const std::shared_ptr<ConvFwdPd>& conv_fwd_pd,
                   Tfilter* filter_data, const Tensor& filter_tensor,
                   MklDnnData<Tfilter>& filter, const memory::desc& filter_md)
      TF_LOCKS_EXCLUDED(mu_) {
    mutex_lock lock(mu_);
    const Tensor& cached_filter_data_tensor =
        *cached_filter_data_ptensor_.AccessTensor(context);

    // If filter is already cached, there's nothing to do.
    if (cached_filter_data_tensor.NumElements() > 0) {
      return;
    }

    // Otherwise, cache filter
    filter.SetUsrMem(filter_md, &filter_tensor);
    filter.CheckReorderToOpMem(conv_fwd_pd.get()->weights_desc(),
                               this->cpu_engine_, context);
    filter_data = static_cast<Tfilter*>(filter.GetOpMem().get_data_handle());

    Tensor* filter_tensor_ptr = nullptr;
    AllocatePersistentTensor(context, *conv_fwd_pd, &filter_tensor_ptr);
    void* cached_filter_data = filter.GetTensorBuffer(filter_tensor_ptr);
    size_t cached_filter_data_size = filter.GetOpMem().get_desc().get_size();
    memcpy(cached_filter_data, filter_data, cached_filter_data_size);
  }

  Tfilter* GetCachedFilter(OpKernelContext* context,
                           const memory::desc& filter_md)
      TF_LOCKS_EXCLUDED(mu_) {
    tf_shared_lock lock(mu_);
    const Tensor& cached_filter_data =
        *cached_filter_data_ptensor_.AccessTensor(context);
    const Tensor& cached_filter_md =
        *cached_filter_md_ptensor_.AccessTensor(context);

    // Check if the memory descriptor of the cached weights is the same as
    // filter_md. If so, we can use the cached weights; otherwise
    // return nullptr.
    if (filter_md == *static_cast<memory::desc*>(cached_filter_md.data())) {
      return static_cast<Tfilter*>(
          const_cast<Tfilter*>(cached_filter_data.flat<Tfilter>().data()));
    }
    return nullptr;
  }
};

// Base class for fused convolution forward operations
template <typename Device, typename Tinput, typename Tfilter, typename Tbias,
          typename Toutput, typename Ttemp_output, typename Tpadding,
          bool pad_enabled>
class MklFusedConvOp
    : public MklConvOp<Device, Tinput, Tfilter, Tbias, Toutput, Ttemp_output,
                       Tpadding, false, false, false> {
 public:
  explicit MklFusedConvOp(OpKernelConstruction* context)
      : MklConvOp<Device, Tinput, Tfilter, Tbias, Toutput, Ttemp_output,
                  Tpadding, false, false, false>(context) {
    // Since we came here through the registration of _MklFusedConv2D, get
    // all information from 'fused_ops' and 'num_args'
    std::vector<string> fused_ops;
    OP_REQUIRES_OK(context, context->GetAttr("fused_ops", &fused_ops));

    int num_args;
    OP_REQUIRES_OK(context, context->GetAttr("num_args", &num_args));
    OP_REQUIRES(context, !fused_ops.empty(),
                errors::InvalidArgument(
                    "Fused Conv2D must have at least one fused op."));

    if (fused_ops == std::vector<string>{"BiasAdd"}) {
      this->set_fuse_biasadd(true);
      OP_REQUIRES(context, num_args == 1,
                  errors::InvalidArgument(
                      "Fused Conv2D must have one extra argument: bias."));
    } else if (fused_ops == std::vector<string>{"Relu"}) {
      this->set_fuse_activation(true, mkldnn::algorithm::eltwise_relu);
    } else if (fused_ops == std::vector<string>{"Relu6"}) {
      this->set_fuse_activation(true, mkldnn::algorithm::eltwise_bounded_relu,
                                6.0);
    } else if (fused_ops == std::vector<string>{"Elu"}) {
      this->set_fuse_activation(true, mkldnn::algorithm::eltwise_elu, 1.0);
    } else if (fused_ops == std::vector<string>{"LeakyRelu"}) {
      float leakyrelu_alpha;
      OP_REQUIRES_OK(context,
                     context->GetAttr("leakyrelu_alpha", &leakyrelu_alpha));
      this->set_fuse_activation(true, mkldnn::algorithm::eltwise_relu,
                                leakyrelu_alpha);
    } else if (fused_ops == std::vector<string>{"BiasAdd", "Relu"}) {
      this->set_fuse_biasadd(true);
      this->set_fuse_activation(true, mkldnn::algorithm::eltwise_relu);
      OP_REQUIRES(context, num_args == 1,
                  errors::InvalidArgument(
                      "Fused Conv2D must have one extra argument: bias."));
    } else if (fused_ops == std::vector<string>{"BiasAdd", "Relu6"}) {
      this->set_fuse_biasadd(true);
      this->set_fuse_activation(true, mkldnn::algorithm::eltwise_bounded_relu,
                                6.0);
      OP_REQUIRES(context, num_args == 1,
                  errors::InvalidArgument(
                      "Fused Conv2D must have one extra argument: bias."));
    } else if (fused_ops == std::vector<string>{"BiasAdd", "Elu"}) {
      this->set_fuse_biasadd(true);
      this->set_fuse_activation(true, mkldnn::algorithm::eltwise_elu, 1.0);
      OP_REQUIRES(context, num_args == 1,
                  errors::InvalidArgument(
                      "Fused Conv2D must have one extra argument: bias."));
    } else if (fused_ops == std::vector<string>{"BiasAdd", "LeakyRelu"}) {
      this->set_fuse_biasadd(true);
      float leakyrelu_alpha;
      OP_REQUIRES_OK(context,
                     context->GetAttr("leakyrelu_alpha", &leakyrelu_alpha));
      this->set_fuse_activation(true, mkldnn::algorithm::eltwise_relu,
                                leakyrelu_alpha);
      OP_REQUIRES(context, num_args == 1,
                  errors::InvalidArgument(
                      "Fused Conv2D must have one extra argument: bias."));
    } else if (fused_ops == std::vector<string>{"BiasAdd", "Add"}) {
      this->set_fuse_biasadd(true);
      this->set_fuse_add(true);
      OP_REQUIRES(
          context, num_args == 2,
          errors::InvalidArgument(
              "Fused Conv2D must have two extra arguments: bias and add."));
    } else if (fused_ops == std::vector<string>{"BiasAdd", "Add", "Relu"}) {
      this->set_fuse_biasadd(true);
      this->set_fuse_add(true);
      this->set_fuse_activation(true, mkldnn::algorithm::eltwise_relu);
      OP_REQUIRES(
          context, num_args == 2,
          errors::InvalidArgument(
              "Fused Conv2D must have two extra arguments: bias and add."));
    } else if (fused_ops == std::vector<string>{"BiasAdd", "Add", "Relu6"}) {
      this->set_fuse_biasadd(true);
      this->set_fuse_add(true);
      this->set_fuse_activation(true, mkldnn::algorithm::eltwise_bounded_relu,
                                6.0);
      OP_REQUIRES(
          context, num_args == 2,
          errors::InvalidArgument(
              "Fused Conv2D must have two extra arguments: bias and add."));
    } else if (fused_ops == std::vector<string>{"BiasAdd", "Add", "Elu"}) {
      this->set_fuse_biasadd(true);
      this->set_fuse_add(true);
      this->set_fuse_activation(true, mkldnn::algorithm::eltwise_elu, 1.0);
      OP_REQUIRES(
          context, num_args == 2,
          errors::InvalidArgument(
              "Fused Conv2D must have two extra arguments: bias and add."));
    } else if (fused_ops ==
               std::vector<string>{"BiasAdd", "Add", "LeakyRelu"}) {
      this->set_fuse_biasadd(true);
      this->set_fuse_add(true);
      float leakyrelu_alpha;
      OP_REQUIRES_OK(context,
                     context->GetAttr("leakyrelu_alpha", &leakyrelu_alpha));
      this->set_fuse_activation(true, mkldnn::algorithm::eltwise_relu,
                                leakyrelu_alpha);
      OP_REQUIRES(
          context, num_args == 2,
          errors::InvalidArgument(
              "Fused Conv2D must have two extra arguments: bias and add."));
    } else {
      OP_REQUIRES(context, false,
                  errors::Unimplemented("Fusion is not implemented: [",
                                        absl::StrJoin(fused_ops, ","), "]"));
    }

    if (pad_enabled) {
      this->set_fuse_pad(true);
    }
  }

  virtual ~MklFusedConvOp() {}
};

template <typename Device, typename Tinput, typename Tfilter, typename Tbias,
          typename Toutput, typename Ttemp_output, typename Tpadding,
          bool pad_enabled, bool bias_enabled, bool is_depthwise>
class MklFusedDepthwiseConvOp
    : public MklConvOp<Device, Tinput, Tfilter, Tbias, Toutput, Ttemp_output,
                       Tpadding, bias_enabled, false, is_depthwise> {
 public:
  explicit MklFusedDepthwiseConvOp(OpKernelConstruction* context)
      : MklConvOp<Device, Tinput, Tfilter, Tbias, Toutput, Ttemp_output,
                  Tpadding, bias_enabled, false, is_depthwise>(context) {
    // Since we came here through the registration of
    // _MklFusedDepthwiseConv2dNative, get all
    // information from 'fused_ops' and 'num_args'
    std::vector<string> fused_ops;
    OP_REQUIRES_OK(context, context->GetAttr("fused_ops", &fused_ops));

    int num_args;
    OP_REQUIRES_OK(context, context->GetAttr("num_args", &num_args));
    OP_REQUIRES(context, !fused_ops.empty(),
                errors::InvalidArgument(
                    "Fused DepthwiseConv2D must have at least one fused op."));

    if (fused_ops == std::vector<string>{"BiasAdd"}) {
      this->set_fuse_biasadd(true);
    } else if (fused_ops == std::vector<string>{"BiasAdd", "Relu"}) {
      this->set_fuse_biasadd(true);
      this->set_fuse_activation(true, mkldnn::algorithm::eltwise_relu);
    } else if (fused_ops == std::vector<string>{"BiasAdd", "Relu6"}) {
      this->set_fuse_biasadd(true);
      this->set_fuse_activation(true, mkldnn::algorithm::eltwise_bounded_relu,
                                6.0);
    } else if (fused_ops == std::vector<string>{"BiasAdd", "Elu"}) {
      this->set_fuse_biasadd(true);
      this->set_fuse_activation(true, mkldnn::algorithm::eltwise_elu, 1.0);
    } else {
      OP_REQUIRES(context, false,
                  errors::Unimplemented("Fusion is not implemented: [",
                                        absl::StrJoin(fused_ops, ","), "]"));
    }

    OP_REQUIRES(
        context, num_args == 1,
        errors::InvalidArgument(
            "Fused DepthwiseConv2D must have one extra argument: bias."));

    if (pad_enabled) {
      this->set_fuse_pad(true);
    }
  }

  virtual ~MklFusedDepthwiseConvOp() {}
};

// Register 2D operations
#define REGISTER_MKL_CPU_2D(T)                                  \
  REGISTER_KERNEL_BUILDER(                                      \
      Name("Conv2D").Device(DEVICE_CPU).TypeConstraint<T>("T"), \
      MklConvOp<CPUDevice, T, T, T, T, T, int32, false, false, false>);

TF_CALL_float(REGISTER_MKL_CPU_2D);
TF_CALL_bfloat16(REGISTER_MKL_CPU_2D);

#define REGISTER_MKL_CPU_2D_DEPTHWISE(T)                                       \
  REGISTER_KERNEL_BUILDER(Name("_FusedDepthwiseConv2dNative")                  \
                              .Device(DEVICE_CPU)                              \
                              .TypeConstraint<T>("T"),                         \
                          MklFusedDepthwiseConvOp<CPUDevice, T, T, T, T, T,    \
                                                  int32, false, true, true>);  \
  REGISTER_KERNEL_BUILDER(                                                     \
      Name("DepthwiseConv2dNative").Device(DEVICE_CPU).TypeConstraint<T>("T"), \
      MklConvOp<CPUDevice, T, T, T, T, T, int32, false, false, true>);

TF_CALL_float(REGISTER_MKL_CPU_2D_DEPTHWISE);
TF_CALL_bfloat16(REGISTER_MKL_CPU_2D_DEPTHWISE);

// Note we are registering _MklFusedConv2D.
// We check the fused_ops attributes to decide if bias is enabled or not.
#define REGISTER_MKL_CPU_2D_FUSED(T)                                  \
  REGISTER_KERNEL_BUILDER(                                            \
      Name("_FusedConv2D").Device(DEVICE_CPU).TypeConstraint<T>("T"), \
      MklFusedConvOp<CPUDevice, T, T, T, T, T, int32, false>);

TF_CALL_float(REGISTER_MKL_CPU_2D_FUSED);
TF_CALL_bfloat16(REGISTER_MKL_CPU_2D_FUSED);

// Register 3D operations
#define REGISTER_MKL_CPU_3D(T)                                  \
  REGISTER_KERNEL_BUILDER(                                      \
      Name("Conv3D").Device(DEVICE_CPU).TypeConstraint<T>("T"), \
      MklConvOp<CPUDevice, T, T, T, T, T, int32, false, false, false>);

TF_CALL_float(REGISTER_MKL_CPU_3D);
TF_CALL_bfloat16(REGISTER_MKL_CPU_3D);

}  // namespace tensorflow
#endif  // INTEL_MKL

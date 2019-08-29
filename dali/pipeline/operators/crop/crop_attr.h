// Copyright (c) 2019, NVIDIA CORPORATION. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef DALI_PIPELINE_OPERATORS_CROP_CROP_ATTR_H_
#define DALI_PIPELINE_OPERATORS_CROP_CROP_ATTR_H_

#include <cmath>
#include <utility>
#include <vector>
#include "dali/core/common.h"
#include "dali/core/error_handling.h"
#include "dali/pipeline/operators/common.h"
#include "dali/pipeline/operators/operator.h"
#include "dali/util/crop_window.h"

namespace dali {

/**
 * @brief Crop parameter and input size handling.
 *
 * Responsible for accessing image type, starting points and size of crop area.
 */
class CropAttr {
 protected:
  explicit inline CropAttr(const OpSpec &spec)
    : spec__(spec)
    , batch_size__(spec__.GetArgument<int>("batch_size")) {
    int crop_h = 0, crop_w = 0, crop_d = 0;
    bool has_crop_arg = spec__.HasArgument("crop");
    bool has_crop_w_arg = spec__.ArgumentDefined("crop_w");
    bool has_crop_h_arg = spec__.ArgumentDefined("crop_h");
    bool has_crop_d_arg = spec__.ArgumentDefined("crop_d");
    is_whole_image_ = !has_crop_arg && !has_crop_w_arg && !has_crop_h_arg && !has_crop_d_arg;

    DALI_ENFORCE(has_crop_w_arg == has_crop_h_arg,
      "`crop_w` and `crop_h` arguments must be provided together");

    if (has_crop_d_arg) {
      DALI_ENFORCE(has_crop_w_arg,
        "`crop_d` argument must be provided together with `crop_w` and `crop_h`");
    }

    if (has_crop_arg) {
      DALI_ENFORCE(!has_crop_h_arg && !has_crop_w_arg && !has_crop_d_arg,
        "`crop` argument is not compatible with `crop_h`, `crop_w`, `crop_d`");

      auto cropArg = spec.GetRepeatedArgument<float>("crop");
      DALI_ENFORCE(cropArg.size() > 0 && cropArg.size() <= 2);
      crop_h = static_cast<int>(cropArg[0]);
      crop_w = static_cast<int>(cropArg.size() >= 2 ? cropArg[1] : cropArg[0]);

      DALI_ENFORCE(crop_h >= 0,
        "Crop height must be greater than zero. Received: " +
        std::to_string(crop_h));

      DALI_ENFORCE(crop_w >= 0,
        "Crop width must be greater than zero. Received: " +
         std::to_string(crop_w));
    }

    crop_height_.resize(batch_size__, crop_h);
    crop_width_.resize(batch_size__, crop_w);
    if (has_crop_d_arg)
      crop_depth_.resize(batch_size__, crop_d);
    crop_x_norm_.resize(batch_size__, 0.0f);
    crop_y_norm_.resize(batch_size__, 0.0f);
    if (has_crop_d_arg)
      crop_z_norm_.resize(batch_size__, 0.0f);
    crop_window_generators_.resize(batch_size__, {});
  }

  void ProcessArguments(const ArgumentWorkspace *ws, std::size_t data_idx) {
    crop_x_norm_[data_idx] = spec__.GetArgument<float>("crop_pos_x", ws, data_idx);
    crop_y_norm_[data_idx] = spec__.GetArgument<float>("crop_pos_y", ws, data_idx);
    if (Is3dCrop())
      crop_z_norm_[data_idx] = spec__.GetArgument<float>("crop_pos_z", ws, data_idx);
    if (!is_whole_image_) {
      if (crop_width_[data_idx] == 0) {
        crop_width_[data_idx] = static_cast<int>(
          spec__.GetArgument<float>("crop_w", ws, data_idx));
      }
      if (crop_height_[data_idx] == 0) {
        crop_height_[data_idx] = static_cast<int>(
          spec__.GetArgument<float>("crop_h", ws, data_idx));
      }
      if (Is3dCrop() && crop_depth_[data_idx] == 0) {
        crop_depth_[data_idx] = static_cast<int>(
          spec__.GetArgument<float>("crop_d", ws, data_idx));
      }
    }

    crop_window_generators_[data_idx] =
      [this, data_idx](kernels::TensorShape<> input_shape) {
        CropWindow crop_window;
        if (input_shape.size() == 3) {
          kernels::TensorShape<> crop_shape =
            {crop_depth_[data_idx], crop_height_[data_idx], crop_width_[data_idx]};
          crop_window.SetShape(crop_shape);

          float anchor_norm[3] =
            {crop_z_norm_[data_idx], crop_y_norm_[data_idx], crop_x_norm_[data_idx]};
          auto anchor = CalculateAnchor(make_span(anchor_norm), crop_shape, input_shape);
          crop_window.SetAnchor(std::move(anchor));
        } else if (input_shape.size() == 2) {
          kernels::TensorShape<> crop_shape = {crop_height_[data_idx], crop_width_[data_idx]};
          crop_window.SetShape(crop_shape);

          float anchor_norm[2] = {crop_y_norm_[data_idx], crop_x_norm_[data_idx]};
          auto anchor = CalculateAnchor(make_span(anchor_norm), crop_shape, input_shape);
          crop_window.SetAnchor(std::move(anchor));
        } else {
          DALI_FAIL("not supported number of dimensions (" +
                    std::to_string(input_shape.size()) + ")");
        }
        DALI_ENFORCE(crop_window.IsInRange(input_shape));
        return crop_window;
    };
  }

  kernels::TensorShape<> CalculateAnchor(const span<float>& anchor_norm,
                                         const kernels::TensorShape<>& crop_shape,
                                         const kernels::TensorShape<>& input_shape) {
    DALI_ENFORCE(anchor_norm.size() == crop_shape.size()
              && anchor_norm.size() == input_shape.size());

    kernels::TensorShape<> anchor(anchor_norm.size(), 0);
    for (int dim = 0; dim < anchor_norm.size(); dim++) {
      DALI_ENFORCE(anchor_norm[dim] >= 0.0f && anchor_norm[dim] <= 1.0f,
        "Anchor for dimension " + std::to_string(dim) + " (" + std::to_string(anchor_norm[dim]) +
        ") is out of range [0.0, 1.0]");
      DALI_ENFORCE(crop_shape[dim] > 0 && crop_shape[dim] <= input_shape[dim],
        "Crop shape for dimension " + std::to_string(dim) + " (" + std::to_string(crop_shape[dim]) +
        ") is out of range [0, " + std::to_string(input_shape[dim]) + "]");
      anchor[dim] = std::roundf(anchor_norm[dim] * (input_shape[dim] - crop_shape[dim]));
    }

    return anchor;
  }

  void ProcessArguments(const ArgumentWorkspace &ws) {
    for (std::size_t data_idx = 0; data_idx < batch_size__; data_idx++) {
      ProcessArguments(&ws, data_idx);
    }
  }

  void ProcessArguments(const SampleWorkspace &ws) {
    ProcessArguments(&ws, ws.data_idx());
  }

  const CropWindowGenerator& GetCropWindowGenerator(std::size_t data_idx) const {
    DALI_ENFORCE(data_idx < crop_window_generators_.size());
    return crop_window_generators_[data_idx];
  }

  inline bool IsWholeImage() const {
    return is_whole_image_;
  }

  inline bool Is3dCrop() const {
    return !crop_depth_.empty();
  }

  std::vector<int> crop_height_;
  std::vector<int> crop_width_;
  std::vector<int> crop_depth_;
  std::vector<float> crop_x_norm_;
  std::vector<float> crop_y_norm_;
  std::vector<float> crop_z_norm_;
  std::vector<CropWindowGenerator> crop_window_generators_;
  bool is_whole_image_ = false;

 private:
  OpSpec spec__;
  std::size_t batch_size__;
};

}  // namespace dali

#endif  // DALI_PIPELINE_OPERATORS_CROP_CROP_ATTR_H_

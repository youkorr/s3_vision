// SPDX-FileCopyrightText: 2026 youkorr
// SPDX-License-Identifier: GPL-3.0-or-later
// Inner classification wrapper for the vision ESPHome component.
//
// Compiled-in for VISION_FAMILY == 5 (imagenet_cls) or 6 (hand_gesture_recognition).
// For other families this file is a no-op (the whole body is #if-guarded out)
// so the linker doesn't pull in the classification postprocessor on detection
// builds.
//
// The model bytes share the same runtime override mechanism as the detection
// inner: vision_set_runtime_model_data(...) installed by vision_component.cpp
// before this constructor runs.

#include "vision_classify.hpp"

#ifndef VISION_FAMILY
#define VISION_FAMILY 0
#endif

#define VISION_FAMILY_IMAGENET_CLS              5
#define VISION_FAMILY_HAND_GESTURE_RECOGNITION  6

#if VISION_FAMILY == VISION_FAMILY_IMAGENET_CLS || \
    VISION_FAMILY == VISION_FAMILY_HAND_GESTURE_RECOGNITION

#include "esp_log.h"
#include "dl_image_preprocessor.hpp"

#if VISION_FAMILY == VISION_FAMILY_IMAGENET_CLS
#include "imagenet_cls_postprocessor.hpp"
#elif VISION_FAMILY == VISION_FAMILY_HAND_GESTURE_RECOGNITION
#include "hand_gesture_cls_postprocessor.hpp"
#endif

// Same flash rodata symbol as the detection inner. Only one of the two
// inner files is "active" per build, so there is no duplicate definition.
extern const uint8_t vision_detect_espdl[] asm("_binary_vision_detect_espdl_start");

// Runtime override - shared variable defined in vision_detect_inner.cpp.
// We re-declare it here as extern; vision_detect_inner.cpp guards its
// definition the same way so exactly one TU provides the storage.
extern const uint8_t *vision_runtime_model_data;

namespace vision_classify {

VisionClassifyImpl::VisionClassifyImpl(const char *model_name) {
  const uint8_t *bytes =
      vision_runtime_model_data ? vision_runtime_model_data : vision_detect_espdl;
  if (bytes == nullptr) {
    ESP_LOGE("vision_classify", "no model data available");
  }
  m_model = new dl::Model(
      reinterpret_cast<const char *>(bytes), model_name,
      static_cast<fbs::model_location_type_t>(CONFIG_YOLO11_DETECT_MODEL_LOCATION));
  m_model->minimize();

  // ImageNet normalization: mean=[123.675,116.28,103.53] std=[58.395,57.12,57.375]
  m_image_preprocessor = new dl::image::ImagePreprocessor(
      m_model, {123.675f, 116.28f, 103.53f}, {58.395f, 57.12f, 57.375f});

#if VISION_FAMILY == VISION_FAMILY_IMAGENET_CLS
  // topk=5, score_thr=lowest (upstream default) -> ranked but unfiltered;
  // we filter at the component level using the YAML score_threshold.
  m_postprocessor = new dl::cls::ImageNetClsPostprocessor(
      m_model, 5, std::numeric_limits<float>::lowest(), true);
#elif VISION_FAMILY == VISION_FAMILY_HAND_GESTURE_RECOGNITION
  m_postprocessor = new dl::cls::HandGestureClsPostprocessor(
      m_model, 1, std::numeric_limits<float>::lowest(), true);
#endif
}

}  // namespace vision_classify

VisionClassify::VisionClassify() {
  m_impl = new vision_classify::VisionClassifyImpl("vision_model.espdl");
}

VisionClassify::~VisionClassify() {
  delete m_impl;
}

std::vector<dl::cls::result_t> &VisionClassify::run(const dl::image::img_t &img) {
  return m_impl->run(img);
}

void VisionClassify::set_topk(int topk) {
  if (m_impl) m_impl->set_topk(topk);
}

void VisionClassify::set_score_thr(float score_thr) {
  if (m_impl) m_impl->set_score_thr(score_thr);
}

#else  // detection family

// Stub when classification isn't selected. The header is still included by
// vision_component.cpp but the class methods are never called.
VisionClassify::VisionClassify() {}
VisionClassify::~VisionClassify() {}
std::vector<dl::cls::result_t> &VisionClassify::run(const dl::image::img_t &) {
  static std::vector<dl::cls::result_t> empty;
  return empty;
}
void VisionClassify::set_topk(int) {}
void VisionClassify::set_score_thr(float) {}

#endif

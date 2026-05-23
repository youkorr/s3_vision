// Self-contained classification wrapper for the vision ESPHome component.
//
// Used when VISION_FAMILY selects a classification model:
//   5 = imagenet_cls              (ImageNet 1000 classes, MobileNetV2)
//   6 = hand_gesture_recognition  (HandGesture classes, MobileNetV2 0.5)
#pragma once

#include "dl_cls_base.hpp"

namespace vision_classify {
class VisionClassifyImpl : public dl::cls::ClsImpl {
 public:
  VisionClassifyImpl(const char *model_name);
};
}  // namespace vision_classify

class VisionClassify {
 public:
  VisionClassify();
  ~VisionClassify();

  std::vector<dl::cls::result_t> &run(const dl::image::img_t &img);
  void set_topk(int topk);
  void set_score_thr(float score_thr);

 private:
  vision_classify::VisionClassifyImpl *m_impl{nullptr};
};

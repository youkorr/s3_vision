// Self-contained detection wrapper for the vision ESPHome component.
#pragma once

#include "dl_detect_base.hpp"
#include "dl_detect_yolo11_postprocessor.hpp"

namespace vision_detect {
class VisionDetectImpl : public dl::detect::DetectImpl {
public:
    VisionDetectImpl(const char *model_name);
};
} // namespace vision_detect

class VisionDetect : public dl::detect::DetectWrapper {
public:
    typedef enum { YOLO11_S8_V1 } model_type_t;
    VisionDetect(const char *sdcard_model_dir = nullptr,
                 model_type_t model_type = static_cast<model_type_t>(CONFIG_YOLO11_DETECT_MODEL_TYPE));
protected:
    void load_model() override {} // Empty implementation - model is loaded in constructor
};

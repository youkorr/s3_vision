#include "esphome/core/defines.h"

#ifdef USE_TEXT_SENSOR

#include "vision_text_sensor.h"
#include "esphome/core/log.h"

namespace esphome {
namespace vision {

static const char *const TAG = "vision.text_sensor";

void VisionTextSensor::dump_config() {
  ESP_LOGCONFIG(TAG, "Vision detection text sensor:");
  ESP_LOGCONFIG(TAG, "  Last value: %s",
                this->last_published_.empty() ? "<no detections yet>"
                                              : this->last_published_.c_str());
}

}  // namespace vision
}  // namespace esphome

#endif  // USE_TEXT_SENSOR

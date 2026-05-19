#pragma once

#include "esphome/core/defines.h"

#ifdef USE_TEXT_SENSOR

#include "esphome/core/component.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "vision_component.h"

namespace esphome {
namespace vision {

// Sub-platform that publishes the latest detection summary string after
// each successful inference pass. The publish happens via the listener
// callback registered on VisionComponent, so the user never has to
// call an action - it just shows up in HA.
class VisionTextSensor : public text_sensor::TextSensor,
                         public Component,
                         public VisionListener {
 public:
  void setup() override {}
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void on_detections(const std::vector<DetectionBox> &detections,
                     const std::string &summary) override {
    // Avoid spamming HA: only publish when the string actually changes.
    if (summary != this->last_published_) {
      this->publish_state(summary);
      this->last_published_ = summary;
    }
  }

 protected:
  std::string last_published_;
};

}  // namespace vision
}  // namespace esphome

#endif  // USE_TEXT_SENSOR

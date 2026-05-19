#pragma once

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/core/helpers.h"
#include "esphome/components/esp32_camera/esp32_camera.h"
#include "esphome/components/camera/camera.h"

// Include the actual ESP-DL type definitions so that img_t/result_t are
// complete types in every translation unit that includes this header.
// Forward-declaring them as "struct" conflicts with the typedef'd
// struct definitions in the ESP-DL headers.
#include "dl_image_define.hpp"
#include "dl_detect_define.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <vector>

// Forward decl - only types whose full definition is NOT needed here
namespace dl {
class Model;
namespace image {
class ImagePreprocessor;
}
namespace detect {
class yolo11PostProcessor;
}
}

namespace esphome {
namespace vision {

// COCO detection result returned by ESP-DL.
struct DetectionBox {
  int x1, y1, x2, y2;
  float score;
  int category;       // 0..79 COCO index
  const char *label;  // pointer into the static COCO_CLASSES table
};

// Listener interface used by the text_sensor sub-platform.
class VisionListener {
 public:
  virtual ~VisionListener() = default;
  virtual void on_detections(const std::vector<DetectionBox> &detections,
                              const std::string &summary) = 0;
};


class VisionComponent : public Component, public camera::CameraListener {
 public:
  // ---------- ESPHome lifecycle ----------
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  // ---------- CameraListener interface ----------
  void on_camera_image(const std::shared_ptr<camera::CameraImage> &image) override;

  // ---------- YAML setters ----------
  void set_camera(esp32_camera::ESP32Camera *cam) { this->camera_ = cam; }
  void set_score_threshold(float v) { this->score_threshold_ = v; }
  void set_nms_threshold(float v) { this->nms_threshold_ = v; }
  void set_detection_interval_ms(int v) { this->detection_interval_ms_ = v; }
  void set_max_detections(int v) { this->max_detections_ = v; }
  void set_inference_task_stack_size(int v) { this->task_stack_size_ = v; }
  void set_inference_task_priority(int v) { this->task_priority_ = v; }
  void set_draw_enabled(bool v) { this->draw_enabled_ = v; }

  // Resolution setters - called from the YAML codegen since there are
  // no public getters on ESP32Camera for these values.
  void set_frame_width(uint16_t w) { this->frame_width_ = w; }
  void set_frame_height(uint16_t h) { this->frame_height_ = h; }

  // Model-from-file path (alternative to flash-rodata embed).
  void set_model_buffer(const uint8_t *data, size_t size) {
    this->external_model_data_ = data;
    this->external_model_size_ = size;
  }

  // ---------- triggers / listeners ----------
  void add_on_object_detected_callback(std::function<void(int, std::string)> cb) {
    this->on_object_detected_callbacks_.push_back(std::move(cb));
  }
  void add_on_detection_image_callback(std::function<void(uint8_t *, size_t)> cb) {
    this->on_detection_image_callbacks_.push_back(std::move(cb));
  }
  void add_listener(VisionListener *l) { this->listeners_.push_back(l); }

  // JPEG quality for the snapshot fired through on_detection_image (1..100)
  void set_jpeg_quality(uint8_t q) { this->jpeg_quality_ = q; }

  // ---------- public API ----------
  // Force a one-shot inference on the most recently captured frame.
  void trigger_inference();

  // Enable/disable the inference pipeline at runtime. When disabled the
  // background task still runs but frames coming from the camera are
  // dropped before being queued, so no inference happens. Useful for
  // power saving or for gating detection on a switch/schedule.
  void set_inference_enabled(bool enabled) { this->inference_enabled_ = enabled; }
  bool is_inference_enabled() const { return this->inference_enabled_; }

  // Get current cached detections. Mutex-protected.
  std::vector<DetectionBox> get_detections();
  int get_detected_count();
  std::string get_last_summary() const { return this->last_summary_; }

  // Draw current cached detections (boxes + class name in white) into
  // the supplied RGB565 buffer. Same convention as vision_detection on
  // the P4 side: 2-pixel hollow rectangle + 5x7 bitmap font label
  // above the box. Call from your YAML's `on_image:` automation if you
  // want the boxes to appear on the camera display.
  //
  // No-op when draw_enabled is false (default true).
  void draw_on_frame(uint8_t *img_data, uint16_t width, uint16_t height);

 protected:
  // Background inference task.
  static void inference_task_trampoline(void *arg);
  void inference_task_loop_();
  bool initialise_detector_();
  void run_one_inference_();

  // Helper: build a "label:score%,..." summary string.
  static std::string build_summary_(const std::vector<DetectionBox> &dets, int max_items);

  // 5x7 bitmap text helper, used by draw_on_frame for the class label.
  static void draw_text_(uint16_t *buffer, uint16_t width, uint16_t height,
                          int x, int y, const char *text,
                          uint16_t color, int scale);

  // ---------- members ----------
  esp32_camera::ESP32Camera *camera_{nullptr};

  // External model buffer (set when model_id: is provided).
  const uint8_t *external_model_data_{nullptr};
  size_t external_model_size_{0};

  float score_threshold_{0.30f};
  float nms_threshold_{0.50f};
  int detection_interval_ms_{200};
  int max_detections_{10};
  int task_stack_size_{8192};
  int task_priority_{5};
  bool draw_enabled_{true};

  // Frame resolution (set from YAML config or inferred from frame data).
  uint16_t frame_width_{320};
  uint16_t frame_height_{240};

  // Runtime gating of the inference pipeline. Default ON.
  bool inference_enabled_{true};

  // ESP-DL state - opaque in the public header.
  dl::Model *model_{nullptr};
  bool model_ready_{false};

  // Single-slot frame queue.
  uint8_t *pending_frame_data_{nullptr};
  size_t pending_frame_size_{0};
  uint32_t last_inference_ms_{0};

  TaskHandle_t inference_task_handle_{nullptr};
  SemaphoreHandle_t frame_signal_{nullptr};
  SemaphoreHandle_t state_mutex_{nullptr};

  std::vector<DetectionBox> cached_detections_;
  std::string last_summary_;

  std::vector<std::function<void(int, std::string)>> on_object_detected_callbacks_;
  std::vector<std::function<void(uint8_t *, size_t)>> on_detection_image_callbacks_;
  std::vector<VisionListener *> listeners_;

  // PSRAM frame copy buffer for JPEG encode (allocated in setup)
  uint8_t *frame_copy_buf_{nullptr};
  size_t frame_copy_capacity_{0};
  uint8_t jpeg_quality_{50};
};


// =====================================================================
// Trigger fired after each detection pass (count + summary string).
// =====================================================================
class ObjectDetectedTrigger : public Trigger<int, std::string> {
 public:
  explicit ObjectDetectedTrigger(VisionComponent *parent) {
    parent->add_on_object_detected_callback(
        [this](int count, std::string summary) {
          this->trigger(count, std::move(summary));
        });
  }
};


// =====================================================================
// DetectionImage - mirrors esp32_camera::CameraImageData so the user can
// reference `image.data` and `image.length` in the trigger lambda exactly
// like in `esp32_camera.on_image:`.
// =====================================================================
struct DetectionImage {
  uint8_t *data;
  size_t length;
};


// =====================================================================
// Trigger fired ONLY when count > 0 with the JPEG-encoded snapshot.
//   - Yaml usage: on_detection_image: -> image.data / image.length
// =====================================================================
class DetectionImageTrigger : public Trigger<DetectionImage> {
 public:
  explicit DetectionImageTrigger(VisionComponent *parent) {
    parent->add_on_detection_image_callback(
        [this](uint8_t *data, size_t length) {
          DetectionImage img{data, length};
          this->trigger(img);
        });
  }
};


// =====================================================================
// Action: vision.inference - force a one-shot inference now.
// =====================================================================
template<typename... Ts>
class RunInferenceAction : public Action<Ts...>, public Parented<VisionComponent> {
 public:
  void play(const Ts &...x) override { this->parent_->trigger_inference(); }
};

// =====================================================================
// Action: vision.start - resume the inference pipeline.
// =====================================================================
template<typename... Ts>
class StartInferenceAction : public Action<Ts...>, public Parented<VisionComponent> {
 public:
  void play(const Ts &...x) override { this->parent_->set_inference_enabled(true); }
};

// =====================================================================
// Action: vision.stop - suspend the inference pipeline. Frames are
// dropped at the listener level; the inference task stays alive.
// =====================================================================
template<typename... Ts>
class StopInferenceAction : public Action<Ts...>, public Parented<VisionComponent> {
 public:
  void play(const Ts &...x) override { this->parent_->set_inference_enabled(false); }
};


}  // namespace vision
}  // namespace esphome

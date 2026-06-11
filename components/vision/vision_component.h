// SPDX-FileCopyrightText: 2026 youkorr
// SPDX-License-Identifier: GPL-3.0-or-later
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

class VisionClassify;  // defined in vision_classify.hpp

namespace esphome {
namespace vision {

// Pose keypoint (x, y) in image pixel coordinates. For COCO pose there are
// 17 keypoints per person in the standard order: nose, left_eye, right_eye,
// left_ear, right_ear, left_shoulder, right_shoulder, left_elbow, right_elbow,
// left_wrist, right_wrist, left_hip, right_hip, left_knee, right_knee,
// left_ankle, right_ankle.
struct KeyPoint {
  int x, y;
};

// COCO detection result returned by ESP-DL.
struct DetectionBox {
  int x1, y1, x2, y2;
  float score;
  int category;       // 0..79 COCO index
  const char *label;  // pointer into the static COCO_CLASSES table
  // Populated only for pose families (coco_pose). Empty otherwise.
  std::vector<KeyPoint> keypoints;
};

// Classification result (imagenet_cls / hand_gesture_recognition families).
struct ClassificationResult {
  std::string label;
  float score;
};

// Result of face recognition: who is in the frame + how confident.
// Produced only by the human_face_recognition family. The numeric id is
// the slot in the embeddings database; name is the human-readable label
// looked up from NVS preferences. similarity is the cosine similarity
// (0..1) between the live face embedding and the stored one.
struct RecognitionResult {
  std::string name;
  uint16_t id;
  float similarity;
  int x1, y1, x2, y2;  // bbox of the recognised face on the frame
  bool known;          // false when the face matched no enrolled person
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
  void set_topk(int v) { this->topk_ = v; }

  // Resolution setters - called from the YAML codegen since there are
  // no public getters on ESP32Camera for these values.
  void set_frame_width(uint16_t w) { this->frame_width_ = w; }
  void set_frame_height(uint16_t h) { this->frame_height_ = h; }

  // Model-from-file path (alternative to flash-rodata embed).
  void set_model_buffer(const uint8_t *data, size_t size) {
    this->external_model_data_ = data;
    this->external_model_size_ = size;
  }

  // Second model buffer - only used by face_recognition family (the
  // embedding feature extractor; the detector still comes from model_id).
  void set_feat_model_buffer(const uint8_t *data, size_t size) {
    this->external_feat_model_data_ = data;
    this->external_feat_model_size_ = size;
  }

  void set_recognition_threshold(float v) { this->recognition_threshold_ = v; }
  void set_recognition_db_path(const std::string &p) { this->recognition_db_path_ = p; }

  // ---------- triggers / listeners ----------
  void add_on_object_detected_callback(std::function<void(int, std::string)> cb) {
    this->on_object_detected_callbacks_.push_back(std::move(cb));
  }
  void add_on_detection_image_callback(std::function<void(uint8_t *, size_t)> cb) {
    this->on_detection_image_callbacks_.push_back(std::move(cb));
  }
  void add_on_classification_callback(std::function<void(std::string, float)> cb) {
    this->on_classification_callbacks_.push_back(std::move(cb));
  }
  void add_on_recognition_callback(std::function<void(std::string, float)> cb) {
    this->on_recognition_callbacks_.push_back(std::move(cb));
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

  // Get current cached classifications (imagenet_cls / hand_gesture families).
  // Empty for detection families.
  std::vector<ClassificationResult> get_classifications();

  // Get current cached recognitions (human_face_recognition family).
  // Empty for other families. Each entry pairs a detected face bbox with
  // the matched person name + cosine similarity score.
  std::vector<RecognitionResult> get_recognitions();

  // --- Face recognition database management ---
  // Enroll the largest face currently visible in the frame under the given
  // name. Captures the next frame, runs detect+feat, stores the embedding.
  // Returns true on success.
  bool enroll_face(const std::string &name);
  // Remove all enrolled embeddings for the given name.
  bool forget_face(const std::string &name);
  // Wipe the entire face recognition database (embeddings + name map).
  bool clear_faces();
  // Number of currently enrolled embeddings.
  int get_enrolled_face_count();

  // Unified JSON output. Returns a JSON string adapted to the current family:
  //   detection:      {"type":"detection","count":N,"objects":[{...}]}
  //   pose:           {"type":"pose","count":N,"objects":[{...,"keypoints":{...}}]}
  //   classification: {"type":"classification","count":N,"objects":[{class,score}]}
  //   recognition:    {"type":"recognition","count":N,"objects":[{class,score,box}]}
  // Designed to be called from on_augmented_image: / on_event: lambdas so a
  // single YAML works across all model families.
  std::string get_inference_json();

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

  // Encode the current frame as JPEG and fire the on_detection_image /
  // on_augmented_image callbacks. Returns true if a JPEG was actually emitted.
  // `draw_overlay` controls whether boxes are drawn on the snapshot before
  // encoding (false for classification, true for detection/pose).
  bool encode_and_fire_jpeg_(uint16_t w, uint16_t h, bool draw_overlay);

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

  // Second external model buffer for face_recognition family.
  const uint8_t *external_feat_model_data_{nullptr};
  size_t external_feat_model_size_{0};

  float score_threshold_{0.30f};
  float nms_threshold_{0.50f};
  int detection_interval_ms_{200};
  int max_detections_{10};
  int task_stack_size_{8192};
  int task_priority_{5};
  bool draw_enabled_{true};
  int topk_{1};
  float recognition_threshold_{0.5f};
  std::string recognition_db_path_{"/sdcard/face_db"};
  // Pending enrollment: when non-empty, the next inference cycle will
  // enroll the largest detected face under this name instead of recognising.
  std::string pending_enroll_name_;

  // Frame resolution (set from YAML config or inferred from frame data).
  uint16_t frame_width_{320};
  uint16_t frame_height_{240};

  // Runtime gating of the inference pipeline. Default ON.
  bool inference_enabled_{true};

  // ESP-DL state - opaque in the public header.
  // For detection families: model_ is cast to VisionDetect *.
  // For classification families: classifier_ holds the VisionClassify *.
  // For face_recognition family: model_ is the detector AND recognizer_ is a
  // separately-typed opaque pointer to the HumanFaceRecognizer.
  dl::Model *model_{nullptr};
  VisionClassify *classifier_{nullptr};
  void *recognizer_{nullptr};  // HumanFaceRecognizer * (opaque)
  bool model_ready_{false};

  // Single-slot frame queue.
  uint8_t *pending_frame_data_{nullptr};
  size_t pending_frame_size_{0};
  uint32_t last_inference_ms_{0};

  TaskHandle_t inference_task_handle_{nullptr};
  SemaphoreHandle_t frame_signal_{nullptr};
  SemaphoreHandle_t state_mutex_{nullptr};

  std::vector<DetectionBox> cached_detections_;
  std::vector<ClassificationResult> cached_classifications_;
  std::vector<RecognitionResult> cached_recognitions_;
  std::string last_summary_;

  std::vector<std::function<void(int, std::string)>> on_object_detected_callbacks_;
  std::vector<std::function<void(uint8_t *, size_t)>> on_detection_image_callbacks_;
  std::vector<std::function<void(std::string, float)>> on_classification_callbacks_;
  std::vector<std::function<void(std::string, float)>> on_recognition_callbacks_;
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
// Trigger fired once per inference for classification families
// (imagenet_cls, hand_gesture_recognition) with the top-1 result.
//   - Yaml usage: on_classification: -> label (std::string), score (float)
// =====================================================================
class ClassificationTrigger : public Trigger<std::string, float> {
 public:
  explicit ClassificationTrigger(VisionComponent *parent) {
    parent->add_on_classification_callback(
        [this](std::string label, float score) {
          this->trigger(std::move(label), score);
        });
  }
};


// =====================================================================
// Trigger fired when a face is recognised (similarity >= threshold).
//   - Yaml usage: on_recognition: -> name (std::string), similarity (float)
// =====================================================================
class RecognitionTrigger : public Trigger<std::string, float> {
 public:
  explicit RecognitionTrigger(VisionComponent *parent) {
    parent->add_on_recognition_callback(
        [this](std::string name, float similarity) {
          this->trigger(std::move(name), similarity);
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

// =====================================================================
// Action: vision.enroll - enroll the next detected face under `name`.
// Takes a templatable string so it can be parameterised from a template
// switch or from an MQTT topic.
// =====================================================================
template<typename... Ts>
class EnrollFaceAction : public Action<Ts...>, public Parented<VisionComponent> {
 public:
  TEMPLATABLE_VALUE(std::string, name)
  void play(Ts... x) override {
    this->parent_->enroll_face(this->name_.value(x...));
  }
};

// =====================================================================
// Action: vision.forget - delete every embedding stored under `name`.
// =====================================================================
template<typename... Ts>
class ForgetFaceAction : public Action<Ts...>, public Parented<VisionComponent> {
 public:
  TEMPLATABLE_VALUE(std::string, name)
  void play(Ts... x) override {
    this->parent_->forget_face(this->name_.value(x...));
  }
};

// =====================================================================
// Action: vision.clear_faces - wipe the entire face recognition DB.
// =====================================================================
template<typename... Ts>
class ClearFacesAction : public Action<Ts...>, public Parented<VisionComponent> {
 public:
  void play(const Ts &...x) override { this->parent_->clear_faces(); }
};


}  // namespace vision
}  // namespace esphome

#include "vision_component.h"
#include "vision_detect.hpp"
#include "vision_classify.hpp"
#include "esphome/core/log.h"
#include "esphome/core/application.h"

#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "esp_camera.h"  // for fmt2jpg() / pixformat_t

#ifdef ESP_DL_MODEL_YOLO11
#include "dl_image.hpp"
#endif

#include "dl_cls_define.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

// Compile-time switch: classification families take a different pipeline.
#if defined(VISION_FAMILY_NAME_IMAGENET_CLS) || \
    defined(VISION_FAMILY_NAME_HAND_GESTURE_RECOGNITION)
#define VISION_IS_CLASSIFICATION 1
#else
#define VISION_IS_CLASSIFICATION 0
#endif

// Defined in vision_detect_inner.cpp. Sets the runtime override for the
// model bytes; pass nullptr to fall back to the build-embedded blob.
extern "C" void vision_set_runtime_model_data(const uint8_t *data);

// Global overloads to resolve ABI mismatch in pre-compiled libfbs_model.a
extern "C" {
void *malloc_aligned(size_t size, uint32_t caps) {
  return dl::tool::malloc_aligned(size, caps);
}
void *calloc_aligned(size_t n, size_t size, uint32_t caps) {
  return dl::tool::calloc_aligned(n, size, caps);
}
}

namespace esphome {
namespace vision {

static const char *const TAG = "vision";

// Class names table. Selected per model family at build time so we
// don't waste flash on unused class lists.
#if defined(VISION_FAMILY_NAME_PEDESTRIAN_DETECT)
static const char *const COCO_CLASSES[] = { "person" };
#elif defined(VISION_FAMILY_NAME_HAND_DETECT)
static const char *const COCO_CLASSES[] = { "hand" };
#elif defined(VISION_FAMILY_NAME_HUMAN_FACE_DETECT)
static const char *const COCO_CLASSES[] = { "face" };
#elif defined(VISION_FAMILY_NAME_COCO_POSE)
static const char *const COCO_CLASSES[] = { "person" };
#else
// Default: coco_detect (YOLO11 / YOLO26) - 80 COCO classes.
static const char *const COCO_CLASSES[] = {
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat", "traffic_light",
    "fire_hydrant", "stop_sign", "parking_meter", "bench", "bird", "cat", "dog", "horse", "sheep", "cow",
    "elephant", "bear", "zebra", "giraffe", "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee",
    "skis", "snowboard", "sports_ball", "kite", "baseball_bat", "baseball_glove", "skateboard", "surfboard",
    "tennis_racket", "bottle", "wine_glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple",
    "sandwich", "orange", "broccoli", "carrot", "hot_dog", "pizza", "donut", "cake", "chair", "couch",
    "potted_plant", "bed", "dining_table", "toilet", "tv", "laptop", "mouse", "remote", "keyboard", "cell_phone",
    "microwave", "oven", "toaster", "sink", "refrigerator", "book", "clock", "vase", "scissors", "teddy_bear",
    "hair_drier", "toothbrush",
};
#endif
static constexpr int COCO_CLASS_COUNT = sizeof(COCO_CLASSES) / sizeof(COCO_CLASSES[0]);


// ---------------------------------------------------------------------------
// 5x7 bitmap font - same glyph set as in the P4 vision detection so the
// labels look identical on both platforms.
// ---------------------------------------------------------------------------
static const uint8_t FONT_5X7[][7] = {
  {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, // A
  {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}, // B
  {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}, // C
  {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}, // D
  {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}, // E
  {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}, // F
  {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}, // G
  {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, // H
  {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}, // I
  {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}, // J
  {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, // K
  {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}, // L
  {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}, // M
  {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}, // N
  {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, // O
  {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}, // P
  {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}, // Q
  {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}, // R
  {0x0E, 0x11, 0x10, 0x0E, 0x01, 0x11, 0x0E}, // S
  {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}, // T
  {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, // U
  {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}, // V
  {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}, // W
  {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}, // X
  {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}, // Y
  {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}, // Z
  {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, // 0
  {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}, // 1
  {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, // 2
  {0x0E, 0x11, 0x01, 0x06, 0x01, 0x11, 0x0E}, // 3
  {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, // 4
  {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}, // 5
  {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, // 6
  {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, // 7
  {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, // 8
  {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}, // 9
  {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // Space
  {0x00, 0x00, 0x04, 0x00, 0x04, 0x00, 0x00}, // : (colon)
  {0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x08}, // , (comma)
  // % (percent) - clearer 5x7 glyph: two square dots + diagonal slash
  // Row layout (5 bits, MSB=leftmost):
  //   ##..#
  //   ##..#
  //   ...#.
  //   ..#..
  //   .#...
  //   #..##
  //   #..##
  {0x19, 0x19, 0x02, 0x04, 0x08, 0x13, 0x13},
  {0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00}, // . (dot)
  {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}, // - (hyphen)
  {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // _ (underscore as space)
};

static int font_index_for(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a';
  if (c >= '0' && c <= '9') return c - '0' + 26;
  if (c == ' ') return 36;
  if (c == ':') return 37;
  if (c == ',') return 38;
  if (c == '%') return 39;
  if (c == '.') return 40;
  if (c == '-') return 41;
  if (c == '_') return 42;
  return -1;
}







// CameraListener callback - called each time the camera produces a new frame.
void VisionComponent::on_camera_image(const std::shared_ptr<camera::CameraImage> &image) {
  if (image == nullptr) return;
  // When inference is disabled via vision.stop / set_inference_enabled(false)
  // we drop frames here so the camera task isn't blocked but no work happens.
  if (!this->inference_enabled_) return;
  uint8_t *data = image->get_data_buffer();
  size_t len = image->get_data_length();
  if (data == nullptr || len == 0) return;
  if (this->state_mutex_ == nullptr) return;
  if (xSemaphoreTake(this->state_mutex_, pdMS_TO_TICKS(2)) == pdTRUE) {
    this->pending_frame_data_ = data;
    this->pending_frame_size_ = len;
    xSemaphoreGive(this->state_mutex_);
    if (this->frame_signal_) xSemaphoreGive(this->frame_signal_);
  }
}


void VisionComponent::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Vision detector (ESP32-S3)...");

  if (this->camera_ == nullptr) {
    ESP_LOGE(TAG, "esp32_camera not configured");
    this->mark_failed();
    return;
  }

  this->state_mutex_ = xSemaphoreCreateMutex();
  this->frame_signal_ = xSemaphoreCreateBinary();
  if (!this->state_mutex_ || !this->frame_signal_) {
    ESP_LOGE(TAG, "Failed to create mutex/semaphore");
    this->mark_failed();
    return;
  }

  // Register ourselves as a CameraListener to receive frames.
  this->camera_->add_listener(this);

  // Allocate PSRAM buffer to snapshot the inference frame before JPEG encode.
  // Sized for RGB565 at the configured resolution; 16-byte aligned for SIMD.
  this->frame_copy_capacity_ = (size_t) this->frame_width_ * this->frame_height_ * 2;
  this->frame_copy_buf_ = static_cast<uint8_t *>(
      heap_caps_aligned_alloc(16, this->frame_copy_capacity_,
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (this->frame_copy_buf_ == nullptr) {
    ESP_LOGW(TAG, "PSRAM allocation for JPEG snapshot buffer failed (%zu bytes); "
                  "on_detection_image: triggers will be disabled",
             this->frame_copy_capacity_);
    this->frame_copy_capacity_ = 0;
  } else {
    ESP_LOGI(TAG, "Allocated %zu byte PSRAM frame snapshot buffer @ %p",
             this->frame_copy_capacity_, this->frame_copy_buf_);
  }

#ifdef ESP_DL_MODEL_YOLO11
  BaseType_t ok = xTaskCreatePinnedToCore(
      &VisionComponent::inference_task_trampoline, "vision_inf",
      this->task_stack_size_, this,
      this->task_priority_, &this->inference_task_handle_, 1);
  if (ok != pdPASS) {
    ESP_LOGE(TAG, "Failed to create inference task");
    this->mark_failed();
    return;
  }
  ESP_LOGI(TAG, "Vision inference task started (core=1, prio=%d, stack=%d)",
           this->task_priority_, this->task_stack_size_);
#else
  ESP_LOGE(TAG, "ESP_DL_MODEL_YOLO11 not defined - this component needs the");
  ESP_LOGE(TAG, "ESP-DL build flags from vision/__init__.py");
  this->mark_failed();
  return;
#endif
}

void VisionComponent::loop() {}

void VisionComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Vision detector:");
  ESP_LOGCONFIG(TAG, "  Score threshold:    %.2f", this->score_threshold_);
  ESP_LOGCONFIG(TAG, "  NMS threshold:      %.2f", this->nms_threshold_);
  ESP_LOGCONFIG(TAG, "  Detection interval: %d ms", this->detection_interval_ms_);
  ESP_LOGCONFIG(TAG, "  Max detections:     %d", this->max_detections_);
  ESP_LOGCONFIG(TAG, "  Draw enabled:       %s", this->draw_enabled_ ? "yes" : "no");
  if (this->external_model_data_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Model source:       file: buffer (%zu bytes @ %p) [runtime]",
                  this->external_model_size_, this->external_model_data_);
  } else {
    ESP_LOGCONFIG(TAG, "  Model source:       flash rodata (build-embedded)");
  }
  ESP_LOGCONFIG(TAG, "  Model ready:        %s", this->model_ready_ ? "yes" : "no");
}


void VisionComponent::inference_task_trampoline(void *arg) {
  static_cast<VisionComponent *>(arg)->inference_task_loop_();
}

void VisionComponent::inference_task_loop_() {
#ifdef ESP_DL_MODEL_YOLO11
  esp_task_wdt_reset();
  if (!this->initialise_detector_()) {
    ESP_LOGE(TAG, "Detector initialisation failed; task exiting");
    vTaskDelete(nullptr);
    return;
  }
  this->model_ready_ = true;
  esp_task_wdt_reset();

  while (true) {
    if (xSemaphoreTake(this->frame_signal_, portMAX_DELAY) != pdTRUE) {
      continue;
    }
    uint32_t now = millis();
    if ((now - this->last_inference_ms_) < (uint32_t) this->detection_interval_ms_) {
      continue;
    }
    this->last_inference_ms_ = now;
    this->run_one_inference_();
  }
#else
  vTaskDelete(nullptr);
#endif
}


bool VisionComponent::initialise_detector_() {
#ifdef ESP_DL_MODEL_YOLO11
  // If the user provided a runtime model (model_id: pointing at a
  // jesserockz file: array), install it as the override BEFORE
  // the model constructor reads the model bytes.
  if (this->external_model_data_ != nullptr) {
    ESP_LOGI(TAG, "Loading model from runtime buffer (%zu bytes @ %p)",
             this->external_model_size_, this->external_model_data_);
    vision_set_runtime_model_data(this->external_model_data_);
  } else {
    ESP_LOGI(TAG, "Loading model from flash rodata (build-embedded)");
    vision_set_runtime_model_data(nullptr);
  }

#if VISION_IS_CLASSIFICATION
  VisionClassify *classifier = new VisionClassify();
  if (classifier == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate VisionClassify");
    return false;
  }
  classifier->set_topk(this->topk_);
  classifier->set_score_thr(this->score_threshold_);
  this->classifier_ = classifier;
  ESP_LOGI(TAG, "Vision classifier initialised (topk=%d score=%.2f)",
           this->topk_, this->score_threshold_);
  return true;
#else
  VisionDetect *detector = new VisionDetect();
  if (detector == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate VisionDetect");
    return false;
  }
  detector->set_score_thr(this->score_threshold_);
  detector->set_nms_thr(this->nms_threshold_);
  this->model_ = reinterpret_cast<dl::Model *>(detector);
  ESP_LOGI(TAG, "Vision detector initialised (score=%.2f nms=%.2f)",
           this->score_threshold_, this->nms_threshold_);
  return true;
#endif
#else
  return false;
#endif
}


void VisionComponent::run_one_inference_() {
#ifdef ESP_DL_MODEL_YOLO11
  if (this->camera_ == nullptr) return;
#if VISION_IS_CLASSIFICATION
  if (this->classifier_ == nullptr) return;
#else
  if (this->model_ == nullptr) return;
#endif

  uint8_t *frame = nullptr;
  size_t frame_size = 0;
  if (xSemaphoreTake(this->state_mutex_, pdMS_TO_TICKS(20)) == pdTRUE) {
    frame = this->pending_frame_data_;
    frame_size = this->pending_frame_size_;
    xSemaphoreGive(this->state_mutex_);
  }
  if (!frame || !frame_size) return;

  uint16_t w = this->frame_width_;
  uint16_t h = this->frame_height_;
  if (w == 0 || h == 0) return;

  if (frame_size < (size_t) w * h * 2) {
    static bool warned = false;
    if (!warned) {
      ESP_LOGE(TAG, "Frame size %zu < expected %u for RGB565 %ux%u - "
                    "set `pixel_format: rgb565` on your esp32_camera!",
               frame_size, (unsigned) (w * h * 2), w, h);
      warned = true;
    }
    return;
  }

  dl::image::img_t img = {
      .data = frame,
      .width = w,
      .height = h,
      // ESP32 DVP cameras typically deliver RGB565 in big-endian byte order
      .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565BE,
  };

#if VISION_IS_CLASSIFICATION
  // ---------------------------------------------------------------------
  // Classification pipeline (imagenet_cls / hand_gesture_recognition)
  // ---------------------------------------------------------------------
  std::vector<dl::cls::result_t> &cls_results = this->classifier_->run(img);

  std::vector<ClassificationResult> cls;
  cls.reserve(cls_results.size());
  for (const auto &r : cls_results) {
    ClassificationResult c;
    c.label = (r.cat_name != nullptr) ? std::string(r.cat_name) : std::string("unknown");
    c.score = r.score;
    cls.push_back(c);
  }

  if (xSemaphoreTake(this->state_mutex_, pdMS_TO_TICKS(20)) == pdTRUE) {
    this->cached_classifications_ = cls;
    if (!cls.empty()) {
      char buf[128];
      snprintf(buf, sizeof(buf), "%s:%d",
               cls[0].label.c_str(), (int)(cls[0].score * 100));
      this->last_summary_ = buf;
    } else {
      this->last_summary_ = "none";
    }
    xSemaphoreGive(this->state_mutex_);
  }

  // Fire the family-specific on_classification trigger (top-1, above threshold).
  bool above_thr = !cls.empty() && cls[0].score >= this->score_threshold_;
  if (above_thr) {
    for (auto &cb : this->on_classification_callbacks_) {
      cb(cls[0].label, cls[0].score);
    }
  }

  // Fire the generic on_event trigger (mapped to the same callback list as
  // on_object_detected) so a single YAML works across detection and
  // classification families. object_count = 1 above threshold, 0 otherwise.
  int event_count = above_thr ? 1 : 0;
  std::string event_summary = above_thr ? this->last_summary_ : std::string("none");
  for (auto &cb : this->on_object_detected_callbacks_) {
    cb(event_count, event_summary);
  }

  // Fire on_augmented_image (= on_detection_image callback list) with the
  // raw frame as JPEG when there is a result above threshold. No box overlay
  // because classification doesn't produce bounding boxes.
  if (above_thr) {
    this->encode_and_fire_jpeg_(w, h, /*draw_overlay=*/false);
  }
  return;  // skip detection logic below
#else
  VisionDetect *detector = reinterpret_cast<VisionDetect *>(this->model_);
  std::list<dl::detect::result_t> &results = detector->run(img);

  std::vector<DetectionBox> dets;
  dets.reserve(results.size());
  for (const auto &r : results) {
    if ((int) dets.size() >= this->max_detections_) break;
    DetectionBox box;
    box.x1 = r.box[0];
    box.y1 = r.box[1];
    box.x2 = r.box[2];
    box.y2 = r.box[3];
    box.score = r.score;
    box.category = r.category;
    box.label = (r.category >= 0 && r.category < COCO_CLASS_COUNT) ?
                COCO_CLASSES[r.category] : "unknown";
    // Pose family: r.keypoint holds (x1,y1,x2,y2,...) - copy into the box.
    if (!r.keypoint.empty()) {
      size_t n = r.keypoint.size() / 2;
      box.keypoints.reserve(n);
      for (size_t k = 0; k < n; k++) {
        KeyPoint kp;
        kp.x = r.keypoint[2 * k];
        kp.y = r.keypoint[2 * k + 1];
        box.keypoints.push_back(kp);
      }
    }
    dets.push_back(box);
  }

  std::string summary = build_summary_(dets, this->max_detections_);

  if (xSemaphoreTake(this->state_mutex_, pdMS_TO_TICKS(20)) == pdTRUE) {
    this->cached_detections_ = dets;
    this->last_summary_ = summary;
    xSemaphoreGive(this->state_mutex_);
  }

  for (auto *l : this->listeners_) {
    l->on_detections(dets, summary);
  }
  for (auto &cb : this->on_object_detected_callbacks_) {
    cb(static_cast<int>(dets.size()), summary);
  }

  // ---------------------------------------------------------------------
  // JPEG snapshot for on_detection_image: callbacks
  // Only encode when there is actually something to look at, the buffer
  // is allocated, and someone is subscribed.
  // ---------------------------------------------------------------------
  if (!dets.empty()) {
    this->encode_and_fire_jpeg_(w, h, /*draw_overlay=*/true);
  }
#endif  // !VISION_IS_CLASSIFICATION
#endif  // ESP_DL_MODEL_YOLO11
}


bool VisionComponent::encode_and_fire_jpeg_(uint16_t w, uint16_t h, bool draw_overlay) {
  if (this->on_detection_image_callbacks_.empty()) return false;
  if (this->frame_copy_buf_ == nullptr) return false;
  size_t expected = (size_t) w * h * 2;
  if (expected > this->frame_copy_capacity_) {
    ESP_LOGW(TAG, "Frame size %zu exceeds snapshot capacity %zu; skipping JPEG",
             expected, this->frame_copy_capacity_);
    return false;
  }
  // Snapshot the frame under the mutex so the camera task can't overwrite
  // it while we encode.
  bool snapshot_ok = false;
  if (xSemaphoreTake(this->state_mutex_, pdMS_TO_TICKS(20)) == pdTRUE) {
    if (this->pending_frame_data_ != nullptr &&
        this->pending_frame_size_ >= expected) {
      memcpy(this->frame_copy_buf_, this->pending_frame_data_, expected);
      snapshot_ok = true;
    }
    xSemaphoreGive(this->state_mutex_);
  }
  if (!snapshot_ok) return false;

  if (draw_overlay && this->draw_enabled_) {
    this->draw_on_frame(this->frame_copy_buf_, w, h);
  }

  uint8_t *jpeg_out = nullptr;
  size_t jpeg_len = 0;
  bool ok = fmt2jpg(this->frame_copy_buf_, expected, w, h,
                    PIXFORMAT_RGB565, this->jpeg_quality_,
                    &jpeg_out, &jpeg_len);
  if (ok && jpeg_out != nullptr && jpeg_len > 0) {
    ESP_LOGD(TAG, "JPEG snapshot %u bytes (q=%u, %ux%u)",
             (unsigned) jpeg_len, this->jpeg_quality_, w, h);
    for (auto &cb : this->on_detection_image_callbacks_) {
      cb(jpeg_out, jpeg_len);
    }
    free(jpeg_out);
    return true;
  }
  ESP_LOGW(TAG, "fmt2jpg failed (ok=%d out=%p len=%zu)", ok, jpeg_out, jpeg_len);
  if (jpeg_out) free(jpeg_out);
  return false;
}


std::vector<ClassificationResult> VisionComponent::get_classifications() {
  std::vector<ClassificationResult> copy;
  if (xSemaphoreTake(this->state_mutex_, pdMS_TO_TICKS(5)) == pdTRUE) {
    copy = this->cached_classifications_;
    xSemaphoreGive(this->state_mutex_);
  }
  return copy;
}


std::string VisionComponent::get_inference_json() {
#if VISION_IS_CLASSIFICATION
  std::vector<ClassificationResult> cls = this->get_classifications();
  std::string out;
  out.reserve(96);
  out += "{\"type\":\"classification\"";
  if (!cls.empty()) {
    char buf[160];
    snprintf(buf, sizeof(buf),
             ",\"label\":\"%s\",\"score\":%.3f",
             cls[0].label.c_str(), cls[0].score);
    out += buf;
    // Include the full top-k list for downstream consumers that want it.
    out += ",\"topk\":[";
    for (size_t i = 0; i < cls.size(); i++) {
      if (i) out += ',';
      char b[160];
      snprintf(b, sizeof(b),
               "{\"label\":\"%s\",\"score\":%.3f}",
               cls[i].label.c_str(), cls[i].score);
      out += b;
    }
    out += "]";
  } else {
    out += ",\"label\":null,\"score\":0";
  }
  out += "}";
  return out;
#else
  std::vector<DetectionBox> dets = this->get_detections();
  // Detect whether keypoints are present to switch type label.
  bool has_keypoints = false;
  for (const auto &d : dets) {
    if (!d.keypoints.empty()) { has_keypoints = true; break; }
  }
  std::string out;
  out.reserve(128 + dets.size() * 96);
  out += has_keypoints ? "{\"type\":\"pose\"" : "{\"type\":\"detection\"";
  char hdr[40];
  snprintf(hdr, sizeof(hdr), ",\"count\":%d,\"objects\":[", (int) dets.size());
  out += hdr;
  for (size_t i = 0; i < dets.size(); i++) {
    if (i) out += ',';
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"class\":\"%s\",\"score\":%.3f,\"box\":[%d,%d,%d,%d]",
             dets[i].label ? dets[i].label : "unknown",
             dets[i].score,
             dets[i].x1, dets[i].y1, dets[i].x2, dets[i].y2);
    out += buf;
    if (!dets[i].keypoints.empty()) {
      out += ",\"keypoints\":[";
      for (size_t k = 0; k < dets[i].keypoints.size(); k++) {
        if (k) out += ',';
        char kp[24];
        snprintf(kp, sizeof(kp), "[%d,%d]",
                 dets[i].keypoints[k].x, dets[i].keypoints[k].y);
        out += kp;
      }
      out += "]";
    }
    out += "}";
  }
  out += "]}";
  return out;
#endif
}


std::string VisionComponent::build_summary_(
    const std::vector<DetectionBox> &dets, int max_items) {
  if (dets.empty()) return std::string("none");
  std::string out;
  int n = std::min<int>(dets.size(), max_items);
  out.reserve(n * 16);
  for (int i = 0; i < n; i++) {
    if (i > 0) out += ',';
    out += dets[i].label;
    out += ':';
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", static_cast<int>(dets[i].score * 100));
    out += buf;
  }
  return out;
}


void VisionComponent::trigger_inference() {
  this->last_inference_ms_ = 0;
  if (this->frame_signal_) xSemaphoreGive(this->frame_signal_);
}

std::vector<DetectionBox> VisionComponent::get_detections() {
  std::vector<DetectionBox> copy;
  if (xSemaphoreTake(this->state_mutex_, pdMS_TO_TICKS(5)) == pdTRUE) {
    copy = this->cached_detections_;
    xSemaphoreGive(this->state_mutex_);
  }
  return copy;
}

int VisionComponent::get_detected_count() {
  int n = 0;
  if (xSemaphoreTake(this->state_mutex_, pdMS_TO_TICKS(5)) == pdTRUE) {
    n = static_cast<int>(this->cached_detections_.size());
    xSemaphoreGive(this->state_mutex_);
  }
  return n;
}


void VisionComponent::draw_text_(uint16_t *buffer, uint16_t width, uint16_t height,
                                   int x, int y, const char *text,
                                   uint16_t color, int scale) {
  if (buffer == nullptr || text == nullptr || scale < 1) return;
  while (*text) {
    int idx = font_index_for(*text);
    if (idx >= 0) {
      const uint8_t *glyph = FONT_5X7[idx];
      for (int row = 0; row < 7; row++) {
        uint8_t row_data = glyph[row];
        for (int col = 0; col < 5; col++) {
          if (row_data & (1 << (4 - col))) {
            for (int sy = 0; sy < scale; sy++) {
              for (int sx = 0; sx < scale; sx++) {
                int px = x + (col * scale) + sx;
                int py = y + (row * scale) + sy;
                if (px >= 0 && px < width && py >= 0 && py < height) {
                  buffer[py * width + px] = color;
                }
              }
            }
          }
        }
      }
    }
    x += 6 * scale;
    text++;
  }
}


void VisionComponent::draw_on_frame(uint8_t *img_data, uint16_t width, uint16_t height) {
  if (!this->draw_enabled_) return;
  if (img_data == nullptr || width == 0 || height == 0) return;
  if (this->state_mutex_ == nullptr) return;

  std::vector<DetectionBox> dets;
  if (xSemaphoreTake(this->state_mutex_, pdMS_TO_TICKS(5)) == pdTRUE) {
    dets = this->cached_detections_;
    xSemaphoreGive(this->state_mutex_);
  }
  if (dets.empty()) return;

  uint16_t *buffer = reinterpret_cast<uint16_t *>(img_data);

  const uint16_t COLOR_RED    = 0xF800;
  const uint16_t COLOR_GREEN  = 0x07E0;
  const uint16_t COLOR_BLUE   = 0x001F;
  const uint16_t COLOR_YELLOW = 0xFFE0;
  const uint16_t COLOR_WHITE  = 0xFFFF;

  const int line_width = 2;

  for (const auto &box : dets) {
    int x1 = std::max(2, std::min((int)box.x1, (int)width - 3));
    int y1 = std::max(2, std::min((int)box.y1, (int)height - 3));
    int x2 = std::max(x1 + 10, std::min((int)box.x2, (int)width - 3));
    int y2 = std::max(y1 + 10, std::min((int)box.y2, (int)height - 3));

    uint16_t color;
    switch (box.category) {
      case 0:  color = COLOR_RED;    break;
      case 2:  color = COLOR_GREEN;  break;
      case 16: color = COLOR_BLUE;   break;
      default: color = COLOR_YELLOW; break;
    }

    for (int x = x1; x <= x2; x++) {
      for (int t = 0; t < line_width; t++) {
        int top = (y1 + t) * width + x;
        if (top >= 0 && top < (int)(width * height)) buffer[top] = color;
        int bot = (y2 - t) * width + x;
        if (bot >= 0 && bot < (int)(width * height)) buffer[bot] = color;
      }
    }
    for (int y = y1; y <= y2; y++) {
      for (int t = 0; t < line_width; t++) {
        int left  = y * width + (x1 + t);
        if (left  >= 0 && left  < (int)(width * height)) buffer[left]  = color;
        int right = y * width + (x2 - t);
        if (right >= 0 && right < (int)(width * height)) buffer[right] = color;
      }
    }

    char label[64];
    const char *cls = (box.category >= 0 && box.category < COCO_CLASS_COUNT)
                          ? COCO_CLASSES[box.category]
                          : "unknown";
    snprintf(label, sizeof(label), "%s %d%%", cls,
             static_cast<int>(box.score * 100));

    int text_x = std::max(0, x1);
    int text_y = std::max(0, y1 - 9);
    draw_text_(buffer, width, height, text_x, text_y, label, COLOR_WHITE, 1);

    // Pose family: draw the 17-point COCO skeleton as line segments
    // connecting joints. Each box carries its own keypoints, so this
    // works correctly with multiple people in frame.
    if (!box.keypoints.empty()) {
      // COCO 17-keypoint connections (skeleton).
      static const int SKELETON[][2] = {
        {5, 7}, {7, 9},        // left arm
        {6, 8}, {8, 10},       // right arm
        {5, 6},                // shoulders
        {5, 11}, {6, 12},      // shoulders -> hips
        {11, 12},              // hips
        {11, 13}, {13, 15},    // left leg
        {12, 14}, {14, 16},    // right leg
        {0, 1}, {0, 2},        // nose -> eyes
        {1, 3}, {2, 4},        // eyes -> ears
      };
      const int n_segments = sizeof(SKELETON) / sizeof(SKELETON[0]);
      const int n_kp = (int) box.keypoints.size();
      for (int s = 0; s < n_segments; s++) {
        int a = SKELETON[s][0];
        int b = SKELETON[s][1];
        if (a >= n_kp || b >= n_kp) continue;
        int ax = box.keypoints[a].x;
        int ay = box.keypoints[a].y;
        int bx = box.keypoints[b].x;
        int by = box.keypoints[b].y;
        if (ax <= 0 && ay <= 0) continue;  // unreliable keypoint
        if (bx <= 0 && by <= 0) continue;
        // Bresenham's line algorithm, 1px thick green segments.
        int dx = std::abs(bx - ax), sx = ax < bx ? 1 : -1;
        int dy = -std::abs(by - ay), sy = ay < by ? 1 : -1;
        int err = dx + dy;
        int x = ax, y = ay;
        for (int guard = 0; guard < (int) width + (int) height; guard++) {
          if (x >= 0 && x < (int) width && y >= 0 && y < (int) height) {
            buffer[y * width + x] = COLOR_GREEN;
          }
          if (x == bx && y == by) break;
          int e2 = 2 * err;
          if (e2 >= dy) { err += dy; x += sx; }
          if (e2 <= dx) { err += dx; y += sy; }
        }
      }
      // Also draw a small 3x3 dot at each keypoint.
      for (const auto &kp : box.keypoints) {
        if (kp.x <= 0 && kp.y <= 0) continue;
        for (int dy = -1; dy <= 1; dy++) {
          for (int dx = -1; dx <= 1; dx++) {
            int px = kp.x + dx, py = kp.y + dy;
            if (px >= 0 && px < (int) width && py >= 0 && py < (int) height) {
              buffer[py * width + px] = COLOR_YELLOW;
            }
          }
        }
      }
    }
  }
}

}  // namespace vision
}  // namespace esphome


// ===========================================================================
// Linker stubs - kept here so they are guaranteed to be picked up by
// ESPHome's main src/ compile pass regardless of the build_script logic.
// ===========================================================================

// ---------------------------------------------------------------------------
// dl::base::dotprod overloads.
//
// The upstream dl_base_dotprod.cpp is excluded from compilation because it
// pulls in esp-dsp (dsps_dotprod_f32) and the ESP32-P4 SIMD intrinsics. We
// only need the symbols to resolve for the templated mat_vec_dotprod<>
// instantiations in dl_model_base.o. Quantized inference still goes through
// the tie728 .S kernels for the heavy work; these dotprod() helpers are only
// used in fallback paths so going scalar here is fine for ESP32-S3.
// ---------------------------------------------------------------------------
#include <cstdint>
#include <algorithm>

namespace dl {
namespace base {

static inline int16_t saturate_to_int16(int64_t v) {
  if (v > INT16_MAX) return INT16_MAX;
  if (v < INT16_MIN) return INT16_MIN;
  return static_cast<int16_t>(v);
}

__attribute__((weak)) void dotprod(int8_t *input0_ptr, int8_t *input1_ptr,
                                   int16_t *output_ptr, int length, int shift) {
  int32_t result = 0;
  for (int i = 0; i < length; i++) {
    result += static_cast<int32_t>(input0_ptr[i]) *
              static_cast<int32_t>(input1_ptr[i]);
  }
  if (shift > 0) {
    int32_t bias = 1 << (shift - 1);
    result = (result + bias) >> shift;
  } else if (shift < 0) {
    result <<= -shift;
  }
  *output_ptr = saturate_to_int16(static_cast<int64_t>(result));
}

__attribute__((weak)) void dotprod(int8_t *input0_ptr, int16_t *input1_ptr,
                                   int16_t *output_ptr, int length, int shift) {
  int64_t result = 0;
  for (int i = 0; i < length; i++) {
    result += static_cast<int64_t>(input0_ptr[i]) *
              static_cast<int64_t>(input1_ptr[i]);
  }
  if (shift > 0) {
    int64_t bias = 1LL << (shift - 1);
    result = (result + bias) >> shift;
  } else if (shift < 0) {
    result <<= -shift;
  }
  *output_ptr = saturate_to_int16(result);
}

__attribute__((weak)) void dotprod(int16_t *input0_ptr, int16_t *input1_ptr,
                                   int16_t *output_ptr, int length, int shift) {
  int64_t result = 0;
  for (int i = 0; i < length; i++) {
    result += static_cast<int64_t>(input0_ptr[i]) *
              static_cast<int64_t>(input1_ptr[i]);
  }
  if (shift > 0) {
    int64_t bias = 1LL << (shift - 1);
    result = (result + bias) >> shift;
  } else if (shift < 0) {
    result <<= -shift;
  }
  *output_ptr = saturate_to_int16(result);
}

__attribute__((weak)) void dotprod(float *input0_ptr, float *input1_ptr,
                                   float *output_ptr, int length, int /*shift*/) {
  float result = 0.0f;
  for (int i = 0; i < length; i++) {
    result += input0_ptr[i] * input1_ptr[i];
  }
  *output_ptr = result;
}

}  // namespace base
}  // namespace dl


// ---------------------------------------------------------------------------
// mbedtls AES weak stubs.
//
// Referenced by ESP-DL's fbs_loader for encrypted models. We only ship plain
// .espdl files so the AES path is never reached at runtime - we just need the
// symbols to resolve. If the real mbedtls is also linked (it ships with
// ESP-IDF), its strong symbols will override these weak ones.
// ---------------------------------------------------------------------------
extern "C" {

__attribute__((weak)) void mbedtls_aes_init(void *ctx) { (void) ctx; }
__attribute__((weak)) void mbedtls_aes_free(void *ctx) { (void) ctx; }

__attribute__((weak)) int mbedtls_aes_setkey_enc(void *ctx, const unsigned char *key,
                                                  unsigned int keybits) {
  (void) ctx; (void) key; (void) keybits;
  return 0;
}

__attribute__((weak)) int mbedtls_aes_setkey_dec(void *ctx, const unsigned char *key,
                                                  unsigned int keybits) {
  (void) ctx; (void) key; (void) keybits;
  return 0;
}

__attribute__((weak)) int mbedtls_aes_crypt_ctr(void *ctx, size_t length,
                                                 size_t *nc_off,
                                                 unsigned char nonce_counter[16],
                                                 unsigned char stream_block[16],
                                                 const unsigned char *input,
                                                 unsigned char *output) {
  (void) ctx; (void) nc_off; (void) nonce_counter; (void) stream_block;
  if (input != output && input != nullptr && output != nullptr) {
    for (size_t i = 0; i < length; i++) output[i] = input[i];
  }
  return 0;
}

}  // extern "C"



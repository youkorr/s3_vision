// Face recognition implementation - only compiled when the family is set
// to human_face_recognition. Mirrors vision_detect_inner.cpp's structure
// but loads a separate "feat" model (the embedding extractor) via its own
// runtime-buffer mechanism.

#ifdef VISION_FAMILY_NAME_HUMAN_FACE_RECOGNITION

#include "vision_recognize.hpp"

#include "esp_log.h"
#include "dl_model_base.hpp"
#include "dl_feat_image_preprocessor.hpp"
#include "dl_feat_postprocessor.hpp"
#include "fbs_loader.hpp"
#include <algorithm>

static const char *const TAG = "vision_recognize";

// Runtime feat model data, set from the YAML codegen via file: platform.
const uint8_t *vision_runtime_feat_model_data = nullptr;

extern "C" void vision_set_runtime_feat_model_data(const uint8_t *data) {
    vision_runtime_feat_model_data = data;
}

namespace vision_recognize {

FeatImpl::FeatImpl(const char *model_name) {
    if (vision_runtime_feat_model_data == nullptr) {
        ESP_LOGE(TAG, "no feat model data (set feat_model_id: in YAML)");
        return;
    }
    // Flash-rodata model location, same convention as the detector.
    m_model = new dl::Model(
        reinterpret_cast<const char *>(vision_runtime_feat_model_data),
        model_name, fbs::MODEL_LOCATION_IN_FLASH_RODATA);
    m_model->minimize();
    // MobileFaceNet-style normalisation: mean/std = 127.5, swap RGB to BGR.
    m_image_preprocessor =
        new dl::image::FeatImagePreprocessor(m_model,
                                              {127.5, 127.5, 127.5},
                                              {127.5, 127.5, 127.5}, true);
    m_postprocessor = new dl::feat::FeatPostprocessor(m_model);
}

FaceRecognizer::FaceRecognizer(const std::string &db_path)
    : m_db_path(db_path) {
    m_feat = new FeatImpl("human_face_feat.espdl");
    if (m_feat->get_raw_model() == nullptr) {
        ESP_LOGE(TAG, "feat model failed to load - recognition disabled");
        return;
    }
    int feat_len = m_feat->get_feat_len();
    ESP_LOGI(TAG, "feat model loaded, embedding length = %d", feat_len);
    // The database file is created on first use if it doesn't exist.
    // db_path must be on a writable filesystem (SD card or LittleFS).
    m_db = new dl::recognition::DataBase(m_db_path, feat_len);
}

FaceRecognizer::~FaceRecognizer() {
    delete m_db;
    delete m_feat;
}

dl::recognition::result_t FaceRecognizer::recognize(
        const dl::image::img_t &img,
        const std::list<dl::detect::result_t> &detect_res,
        float threshold) {
    if (m_db == nullptr || m_feat == nullptr || detect_res.empty()) {
        return {0, 0.0f};
    }
    // Pick the largest face (most likely the subject of interest).
    auto best = std::max_element(detect_res.begin(), detect_res.end(),
        [](const dl::detect::result_t &a, const dl::detect::result_t &b) {
            return a.box_area() < b.box_area();
        });
    auto feat = m_feat->run(img, best->keypoint);
    if (feat == nullptr) {
        return {0, 0.0f};
    }
    auto results = m_db->query_feat(feat, threshold, 1);
    if (results.empty()) {
        return {0, 0.0f};
    }
    return results.front();
}

int FaceRecognizer::enroll(const dl::image::img_t &img,
                            const std::list<dl::detect::result_t> &detect_res) {
    if (m_db == nullptr || m_feat == nullptr) {
        ESP_LOGW(TAG, "enroll: recognizer not ready");
        return -1;
    }
    if (detect_res.empty()) {
        ESP_LOGW(TAG, "enroll: no face detected in frame");
        return -1;
    }
    auto best = std::max_element(detect_res.begin(), detect_res.end(),
        [](const dl::detect::result_t &a, const dl::detect::result_t &b) {
            return a.box_area() < b.box_area();
        });
    auto feat = m_feat->run(img, best->keypoint);
    if (feat == nullptr) {
        return -1;
    }
    int next_id = m_db->get_num_feats() + 1;  // DB ids are 1-indexed
    if (m_db->enroll_feat(feat) != ESP_OK) {
        ESP_LOGE(TAG, "enroll_feat failed");
        return -1;
    }
    return next_id;
}

bool FaceRecognizer::delete_id(uint16_t id) {
    if (m_db == nullptr) return false;
    return m_db->delete_feat(id) == ESP_OK;
}

bool FaceRecognizer::clear_all() {
    if (m_db == nullptr) return false;
    return m_db->clear_all_feats() == ESP_OK;
}

int FaceRecognizer::get_count() {
    if (m_db == nullptr) return 0;
    return m_db->get_num_feats();
}

}  // namespace vision_recognize

#endif  // VISION_FAMILY_NAME_HUMAN_FACE_RECOGNITION

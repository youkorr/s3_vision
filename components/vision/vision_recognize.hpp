// SPDX-FileCopyrightText: 2026 youkorr
// SPDX-License-Identifier: GPL-3.0-or-later
// Face recognition wrapper for the vision ESPHome component.
// Wraps ESP-DL's HumanFaceRecognizer with a runtime-loaded model buffer
// so it integrates with the jesserockz `file:` platform.
#pragma once

#ifdef VISION_FAMILY_NAME_HUMAN_FACE_RECOGNITION

#include "dl_feat_base.hpp"
#include "dl_recognition_database.hpp"
#include "dl_detect_define.hpp"
#include "dl_image.hpp"
#include <string>
#include <vector>

extern "C" void vision_set_runtime_feat_model_data(const uint8_t *data);

namespace vision_recognize {

class FeatImpl : public dl::feat::FeatImpl {
 public:
  explicit FeatImpl(const char *model_name);
};

class FaceRecognizer {
 public:
  FaceRecognizer(const std::string &db_path);
  ~FaceRecognizer();

  // Run recognition on the largest detected face. Returns {id=0, similarity=0}
  // if no face was matched above threshold.
  dl::recognition::result_t recognize(const dl::image::img_t &img,
                                       const std::list<dl::detect::result_t> &detect_res,
                                       float threshold);

  // Enroll the largest detected face. Returns the new feature id on success,
  // or -1 on failure.
  int enroll(const dl::image::img_t &img,
             const std::list<dl::detect::result_t> &detect_res);

  bool delete_id(uint16_t id);
  bool clear_all();
  int get_count();

 private:
  FeatImpl *m_feat{nullptr};
  dl::recognition::DataBase *m_db{nullptr};
  std::string m_db_path;
};

}  // namespace vision_recognize

#endif  // VISION_FAMILY_NAME_HUMAN_FACE_RECOGNITION

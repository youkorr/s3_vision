#include "sd_mmc_card.h"
#include "esp_task_wdt.h"

#include <algorithm>
#include <vector>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>
#include <sys/stat.h>

#include "math.h"
#include "esphome/core/log.h"

#ifdef USE_ESP_IDF
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/sdmmc_types.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "esp_heap_caps.h"
#include <dirent.h>
#include <errno.h>

int constexpr SD_OCR_SDHC_CAP = (1 << 30);  // value defined in esp-idf/components/sdmmc/include/sd_protocol_defs.h
#endif

namespace esphome {
namespace sd_mmc_card {

static const char *TAG = "sd_mmc_card";

#ifdef USE_ESP_IDF
static const size_t FILE_PATH_MAX = ESP_VFS_PATH_MAX + 256;
static const std::string MOUNT_POINT("/sdcard");

std::string build_path(const char *path_cstr) {
  // Normalise the path so the mount point isn't doubled and a missing
  // leading slash is handled gracefully. Examples:
  //   nullptr            -> "/sdcard"
  //   ""                 -> "/sdcard"
  //   "/sdcard/foo.bin"  -> "/sdcard/foo.bin"   (left as-is)
  //   "/foo.bin"         -> "/sdcard/foo.bin"
  //   "foo.bin"          -> "/sdcard/foo.bin"
  // Without this, the naive MOUNT_POINT + path produced paths like
  // "/sdcard/sdcard/foo.bin" or "/sdcardfoo.bin" depending on how the
  // caller wrote the path - which made every read fail with ENOENT.
  std::string p = path_cstr ? std::string(path_cstr) : std::string();
  if (p.empty()) return MOUNT_POINT;
  if (p.rfind(MOUNT_POINT, 0) == 0) return p;
  if (p.front() != '/') p.insert(p.begin(), '/');
  return MOUNT_POINT + p;
}
#endif

#ifdef USE_SENSOR
FileSizeSensor::FileSizeSensor(sensor::Sensor *sensor, std::string const &path) : sensor(sensor), path(path) {}
#endif

void SdMmc::loop() {}

void SdMmc::dump_config() {
  ESP_LOGCONFIG(TAG, "SD MMC Component");
  ESP_LOGCONFIG(TAG, "  Mode 1 bit: %s", TRUEFALSE(this->mode_1bit_));
  ESP_LOGCONFIG(TAG, "  Slot: %d", this->slot_);
  ESP_LOGCONFIG(TAG, "  Init delay: %u ms", this->init_delay_ms_);
  ESP_LOGCONFIG(TAG, "  CLK Pin: %d", this->clk_pin_);
  ESP_LOGCONFIG(TAG, "  CMD Pin: %d", this->cmd_pin_);
  ESP_LOGCONFIG(TAG, "  DATA0 Pin: %d", this->data0_pin_);
  if (!this->mode_1bit_) {
    ESP_LOGCONFIG(TAG, "  DATA1 Pin: %d", this->data1_pin_);
    ESP_LOGCONFIG(TAG, "  DATA2 Pin: %d", this->data2_pin_);
    ESP_LOGCONFIG(TAG, "  DATA3 Pin: %d", this->data3_pin_);
  }
  if (this->power_ctrl_pin_ != nullptr) {
    LOG_PIN("  Power Ctrl Pin: ", this->power_ctrl_pin_);
  }

  if (!this->is_failed()) {
    const char *freq_unit = card_->real_freq_khz < 1000 ? "kHz" : "MHz";
    const float freq = card_->real_freq_khz < 1000 ? card_->real_freq_khz : card_->real_freq_khz / 1000.0;
    const char *max_freq_unit = card_->max_freq_khz < 1000 ? "kHz" : "MHz";
    const float max_freq = card_->max_freq_khz < 1000 ? card_->max_freq_khz : card_->max_freq_khz / 1000.0;
    ESP_LOGCONFIG(TAG, "  Card Speed:  %.2f %s (limit: %.2f %s)%s", freq, freq_unit, max_freq, max_freq_unit, card_->is_ddr ? ", DDR" : "");
  }

#ifdef USE_SENSOR
  LOG_SENSOR("  ", "Used space", this->used_space_sensor_);
  LOG_SENSOR("  ", "Total space", this->total_space_sensor_);
  LOG_SENSOR("  ", "Free space", this->free_space_sensor_);
  for (auto &sensor : this->file_size_sensors_) {
    if (sensor.sensor != nullptr)
      LOG_SENSOR("  ", "File size", sensor.sensor);
  }
#endif
#ifdef USE_TEXT_SENSOR
  LOG_TEXT_SENSOR("  ", "SD Card Type", this->sd_card_type_text_sensor_);
#endif
  if (this->is_failed()) {
    ESP_LOGE(TAG, "Setup failed : %s", SdMmc::error_code_to_string(this->init_error_).c_str());
    return;
  }
}
#ifdef USE_ESP_IDF

void SdMmc::setup() {
  // Power control pin: only configure it as output - DO NOT drive it.
  // On ESP32-S3-BOX-3 (and similar boards where the SD is permanently
  // powered), the user might still pass a power_ctrl_pin out of habit;
  // forcing it high reconfigures the GPIO and on S3-BOX-3 GPIO43 also
  // conflicts with U0_TXD. The pin's default state is whatever the
  // hardware pull-up/strap defines, which is usually correct.
  if (this->power_ctrl_pin_ != nullptr) {
    this->power_ctrl_pin_->setup();
    ESP_LOGD(TAG, "Power control pin configured (not driven)");
  }

  // Délai de stabilisation : laisser au lecteur/carte SD le temps d'être
  // alimenté et prêt avant de tenter le montage. Certaines cartes (ou leur
  // circuit d'alimentation) ne répondent pas immédiatement après la mise
  // sous tension : sans cette attente, la carte peut se monter mais échouer
  // ensuite à la lecture. Réglable via l'option `init_delay` dans le YAML.
  if (this->init_delay_ms_ > 0) {
    ESP_LOGD(TAG, "Waiting %u ms for SD card to power up...", this->init_delay_ms_);
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(this->init_delay_ms_));
    esp_task_wdt_reset();
  }

  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
    .format_if_mount_failed = false,
    .max_files = 16,
    .allocation_unit_size = 256 * 1024,
  };

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.slot = SDMMC_HOST_SLOT_0 + this->slot_;
  host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;  // 40 MHz - safe across S3 + P4

  // DDR mode is what made the old (working) driver tick in 4-bit on
  // ESP32-S3-BOX-3. Without this flag the host's clock phase is off and
  // some SD cards never reply to ACMD41 (send_op_cond timeout 0x107).
  if (!this->mode_1bit_) {
    host.flags |= SDMMC_HOST_FLAG_DDR;
  } else {
    host.flags &= ~SDMMC_HOST_FLAG_DDR;
  }

  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  slot_config.width = this->mode_1bit_ ? 1 : 4;

  #ifdef SOC_SDMMC_USE_GPIO_MATRIX
  slot_config.clk = static_cast<gpio_num_t>(this->clk_pin_);
  slot_config.cmd = static_cast<gpio_num_t>(this->cmd_pin_);
  slot_config.d0 = static_cast<gpio_num_t>(this->data0_pin_);
  if (!this->mode_1bit_) {
    slot_config.d1 = static_cast<gpio_num_t>(this->data1_pin_);
    slot_config.d2 = static_cast<gpio_num_t>(this->data2_pin_);
    slot_config.d3 = static_cast<gpio_num_t>(this->data3_pin_);
  }
  #endif

  slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  // esp_vfs_fat_sdmmc_mount() does host_init -> slot_init -> card_init -> mount
  // in one shot. Do NOT call sdmmc_host_init_slot() manually first - it
  // requires sdmmc_host_init() to have run, which only happens implicitly
  // when another component (eg. esp32_hosted on P4) initialised the host.
  esp_err_t ret = ESP_FAIL;
  for (int attempt = 1; attempt <= 3; attempt++) {
    ESP_LOGI(TAG, "Mounting SD on slot %d (attempt %d/3)...",
             this->slot_, attempt);
    // Feed the task watchdog around the ~4 s blocking call.
    esp_task_wdt_reset();
    ret = esp_vfs_fat_sdmmc_mount(MOUNT_POINT.c_str(), &host, &slot_config,
                                   &mount_config, &this->card_);
    esp_task_wdt_reset();
    if (ret == ESP_OK) {
      ESP_LOGI(TAG, "SD mounted on slot %d!", this->slot_);
      break;
    }
    ESP_LOGD(TAG, "Mount attempt %d failed: %s",
             attempt, esp_err_to_name(ret));
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  if (ret != ESP_OK) {
    if (ret == ESP_FAIL) {
      this->init_error_ = ErrorCode::ERR_MOUNT;
      ESP_LOGE(TAG, "Failed to mount filesystem on SD card (slot %d)", this->slot_);
    } else {
      this->init_error_ = ErrorCode::ERR_NO_CARD;
      ESP_LOGE(TAG, "No SD card detected on slot %d", this->slot_);
    }
    mark_failed();
    return;
  }

  // Diagnostic de la carte
  ESP_LOGI(TAG, "SD Card Info (slot %d):", this->slot_);
  ESP_LOGI(TAG, "  Name: %s", this->card_->cid.name);
  ESP_LOGI(TAG, "  Type: %s", sd_card_type().c_str());
  ESP_LOGI(TAG, "  Speed: %d kHz", this->card_->real_freq_khz);
  ESP_LOGI(TAG, "  Bus width: %d-bit", this->mode_1bit_ ? 1 : 4);
  ESP_LOGI(TAG, "  DDR mode: %s", this->card_->is_ddr ? "YES" : "NO");
  ESP_LOGI(TAG, "  Size: %llu MB", ((uint64_t)this->card_->csd.capacity * this->card_->csd.sector_size) / (1024 * 1024));

  // Performance diagnostic
  float theoretical_speed_mbps = (this->card_->real_freq_khz / 1000.0) * (this->mode_1bit_ ? 1 : 4) / 8.0;
  if (this->card_->is_ddr) {
    theoretical_speed_mbps *= 2;  // DDR doubles the data rate
  }
  ESP_LOGI(TAG, "  Theoretical max speed: %.1f MB/s", theoretical_speed_mbps);

  update_sensors();

  // Diagnostic (pourquoi le listage renvoie 0 alors que fopen marche ?)
  this->diagnose_filesystem_();
}

// ============================================================================
//  Diagnostic d'énumération du système de fichiers.
//  But : comprendre pourquoi readdir()/f_readdir() renvoient 0 entrée alors
//  que fopen("/sdcard/...") fonctionne. On lit directement le secteur de boot
//  de la carte pour connaître son FORMAT RÉEL (FAT32 / exFAT / taille de
//  secteur), puis on tente une énumération POSIX de /sdcard.
// ============================================================================
void SdMmc::diagnose_filesystem_() {
  ESP_LOGI(TAG, "===== [diag] Filesystem enumeration =====");

  // --- 1) Format réel : lecture du secteur de boot (LBA 0) ---
  const size_t ss = this->card_->csd.sector_size ? this->card_->csd.sector_size : 512;
  uint8_t *sec = (uint8_t *) heap_caps_malloc(ss, MALLOC_CAP_DMA);
  if (sec != nullptr) {
    esp_err_t r = sdmmc_read_sectors(this->card_, sec, 0, 1);
    if (r == ESP_OK) {
      char oem[9] = {0};
      memcpy(oem, sec + 3, 8);
      bool is_exfat = (memcmp(sec + 3, "EXFAT   ", 8) == 0);
      uint16_t bps = (uint16_t) (sec[11] | (sec[12] << 8));  // bytes/sector (FAT)
      char fat16[9] = {0}, fat32[9] = {0};
      memcpy(fat16, sec + 0x36, 8);  // "FAT12 "/"FAT16 " (FAT12/16)
      memcpy(fat32, sec + 0x52, 8);  // "FAT32   "        (FAT32)
      ESP_LOGI(TAG, "[diag] boot OEM='%s'  exFAT=%s  bytes/sec(BPB)=%u  card_sector=%u",
               oem, is_exfat ? "YES" : "no", bps, (unsigned) ss);
      ESP_LOGI(TAG, "[diag] FStype @0x36='%s'  @0x52='%s'  bootsig=%02X%02X",
               fat16, fat32, sec[510], sec[511]);
    } else {
      ESP_LOGW(TAG, "[diag] read sector 0 failed: %s", esp_err_to_name(r));
    }
    heap_caps_free(sec);
  } else {
    ESP_LOGW(TAG, "[diag] DMA alloc of %u bytes failed", (unsigned) ss);
  }

  // --- 2) FS montée : espace total/libre ---
  uint64_t total = 0, freeb = 0;
  if (esp_vfs_fat_info(MOUNT_POINT.c_str(), &total, &freeb) == ESP_OK) {
    ESP_LOGI(TAG, "[diag] esp_vfs_fat_info: total=%llu  free=%llu  used=%llu",
             total, freeb, total - freeb);
  } else {
    ESP_LOGW(TAG, "[diag] esp_vfs_fat_info failed");
  }

  // --- 3) stat() du point de montage ---
  struct stat st;
  if (stat(MOUNT_POINT.c_str(), &st) == 0) {
    ESP_LOGI(TAG, "[diag] stat(%s): is_dir=%d", MOUNT_POINT.c_str(), S_ISDIR(st.st_mode));
  } else {
    ESP_LOGW(TAG, "[diag] stat(%s) failed (errno=%d)", MOUNT_POINT.c_str(), errno);
  }

  // --- 4) Énumération POSIX de /sdcard (le cœur du problème) ---
  errno = 0;
  DIR *dir = opendir(MOUNT_POINT.c_str());
  if (dir == nullptr) {
    ESP_LOGE(TAG, "[diag] opendir(%s) -> NULL (errno=%d)", MOUNT_POINT.c_str(), errno);
    ESP_LOGI(TAG, "===== [diag] end =====");
    return;
  }
  ESP_LOGI(TAG, "[diag] opendir(%s) OK -> readdir:", MOUNT_POINT.c_str());
  int n = 0;
  errno = 0;
  struct dirent *e;
  while ((e = readdir(dir)) != nullptr) {
    ESP_LOGI(TAG, "[diag]   #%d  name='%s'  d_type=%d", n, e->d_name, (int) e->d_type);
    if (++n >= 30) {
      ESP_LOGI(TAG, "[diag]   ... (truncated at 30)");
      break;
    }
  }
  ESP_LOGI(TAG, "[diag] readdir done: %d entry(ies), errno=%d", n, errno);
  closedir(dir);
  ESP_LOGI(TAG, "===== [diag] end =====");
}

void SdMmc::write_file_chunked(const char *path, const uint8_t *buffer, size_t len, size_t chunk_size) {
  std::string absolut_path = build_path(path);
  FILE *file = NULL;
  file = fopen(absolut_path.c_str(), "a");
  if (file == NULL) {
    ESP_LOGE(TAG, "Failed to open file for chunked writing");
    return;
  }

  size_t written = 0;
  while (written < len) {
    size_t to_write = std::min(chunk_size, len - written);
    bool ok = fwrite(buffer + written, 1, to_write, file);
    if (!ok) {
      ESP_LOGE(TAG, "Failed to write chunk to file");
      break;
    }
    written += to_write;

    // CRITIQUE: Forcer l'écriture immédiate sur la carte SD après chaque chunk
    // Sans cela, les données restent dans le buffer RAM et peuvent être perdues
    // lors de l'enregistrement vidéo à haute fréquence
    fflush(file);
  }
  fclose(file);
  this->update_sensors();
}

// Fonction optimisée pour l'écriture de frames vidéo
// Paramètres:
//   - path: chemin du fichier
//   - buffer: buffer contenant la frame vidéo
//   - len: taille de la frame
//   - force_sync: si true, force l'écriture sur disque avec fsync() (par défaut true)
//
// Cette fonction est optimisée pour le streaming vidéo:
// - Utilise fflush() pour vider le buffer stdio
// - Utilise fsync() pour garantir l'écriture sur le disque physique
// - Évite les pertes de frames lors de l'enregistrement vidéo
void SdMmc::write_file_video(const char *path, const uint8_t *buffer, size_t len, bool force_sync) {
  std::string absolut_path = build_path(path);
  FILE *file = fopen(absolut_path.c_str(), "ab");  // Mode append binaire
  if (file == NULL) {
    ESP_LOGE(TAG, "Failed to open video file for writing: %s (errno=%d)", path, errno);
    return;
  }

  // Écriture de la frame complète
  size_t written = fwrite(buffer, 1, len, file);
  if (written != len) {
    ESP_LOGE(TAG, "Video write incomplete: wrote %zu/%zu bytes", written, len);
    fclose(file);
    return;
  }

  // Forcer l'écriture du buffer stdio vers le kernel
  if (fflush(file) != 0) {
    ESP_LOGE(TAG, "Video fflush failed: errno=%d", errno);
  }

  // Si force_sync est activé, forcer l'écriture sur le disque physique
  // ATTENTION: fsync() peut ralentir les écritures mais garantit la persistance des données
  // Pour vidéo haute résolution/framerate, vous pouvez désactiver force_sync
  if (force_sync) {
    int fd = fileno(file);
    if (fd >= 0) {
      if (fsync(fd) != 0) {
        ESP_LOGW(TAG, "Video fsync failed: errno=%d (data may be cached)", errno);
      }
    }
  }

  fclose(file);
  // Ne pas appeler update_sensors() à chaque frame pour éviter la surcharge
}

// Ecriture d'un fichier avec mode explicite ("wb" = ecraser, "ab" = ajouter, ...).
// Ces surcharges etaient declarees dans le .h et enregistrees comme actions
// (sd_mmc_card.write_file / append_file) mais n'avaient AUCUNE implementation :
// toute utilisation echouait a l'edition de liens.
void SdMmc::write_file(const char *path, const uint8_t *buffer, size_t len, const char *mode) {
  std::string absolut_path = build_path(path);
  FILE *file = fopen(absolut_path.c_str(), mode);
  if (file == nullptr) {
    ESP_LOGE(TAG, "Failed to open file for writing: %s (errno=%d)", absolut_path.c_str(), errno);
    return;
  }
  size_t written = fwrite(buffer, 1, len, file);
  fclose(file);
  if (written != len) {
    ESP_LOGE(TAG, "Write incomplete: wrote %zu/%zu bytes to %s", written, len, absolut_path.c_str());
    return;
  }
  this->update_sensors();
}

void SdMmc::write_file(const char *path, const uint8_t *buffer, size_t len) {
  this->write_file(path, buffer, len, "wb");
}

void SdMmc::append_file(const char *path, const uint8_t *buffer, size_t len) {
  this->write_file(path, buffer, len, "ab");
}
#else
void SdMmc::write_file_chunked(const char *path, const uint8_t *buffer, size_t len, size_t chunk_size) {
  ESP_LOGV(TAG, "Writing chunked to file: %s", path);
  size_t written = 0;
  while (written < len) {
    size_t to_write = std::min(chunk_size, len - written);
    this->write_file(path, buffer + written, to_write, "a");
    written += to_write;
  }
}
#endif

std::vector<std::string> SdMmc::list_directory(const char *path, uint8_t depth) {
  std::vector<std::string> list;
  std::vector<FileInfo> infos = list_directory_file_info(path, depth);
  list.reserve(infos.size());
  // IMPORTANT : utiliser std::back_inserter, pas list.begin().
  // Le vecteur de destination est vide ici : ecrire au travers de begin()
  // est un comportement indefini et laissait la liste vide. C'est ce qui
  // donnait l'impression que la carte "se monte mais ne lit/liste rien".
  std::transform(infos.cbegin(), infos.cend(), std::back_inserter(list),
                 [](FileInfo const &info) { return info.path; });
  return list;
}

std::vector<std::string> SdMmc::list_directory(std::string path, uint8_t depth) {
  return this->list_directory(path.c_str(), depth);
}

std::vector<FileInfo> SdMmc::list_directory_file_info(const char *path, uint8_t depth) {
  std::vector<FileInfo> list;
  list_directory_file_info_rec(path, depth, list);
  return list;
}

std::vector<FileInfo> SdMmc::list_directory_file_info(std::string path, uint8_t depth) {
  return this->list_directory_file_info(path.c_str(), depth);
}

#ifdef USE_ESP_IDF
std::vector<FileInfo> &SdMmc::list_directory_file_info_rec(const char *path, uint8_t depth,
                                                           std::vector<FileInfo> &list) {
  ESP_LOGV(TAG, "Listing directory file info: %s\n", path);
  std::string vfs_path = build_path(path);

  DIR *dir = opendir(vfs_path.c_str());
  if (dir == nullptr) {
    ESP_LOGE(TAG, "Failed to open directory: %s (errno %d)", vfs_path.c_str(), errno);
    return list;
  }

  char entry_path[FILE_PATH_MAX];
  size_t path_len = strlen(path);
  strlcpy(entry_path, path, sizeof(entry_path));
  strlcpy(entry_path + path_len, "/", sizeof(entry_path) - path_len);
  path_len = strlen(entry_path);

  struct dirent *entry;
  while ((entry = readdir(dir)) != nullptr) {
    strlcpy(entry_path + path_len, entry->d_name, sizeof(entry_path) - path_len);

    std::string entry_vfs_path = build_path(entry_path);
    struct stat st;
    if (stat(entry_vfs_path.c_str(), &st) != 0) {
      continue;
    }

    bool is_dir = S_ISDIR(st.st_mode);
    size_t file_size = is_dir ? 0 : st.st_size;

    list.emplace_back(entry_path, file_size, is_dir);
    if (is_dir && depth)
      list_directory_file_info_rec(entry_path, depth - 1, list);
  }
  closedir(dir);
  return list;
}

bool SdMmc::is_directory(const char *path) {
  std::string absolut_path = build_path(path);
  struct stat st;
  if (stat(absolut_path.c_str(), &st) == 0) {
    return S_ISDIR(st.st_mode);
  }
  return false;
}

size_t SdMmc::file_size(const char *path) {
  std::string absolut_path = build_path(path);
  struct stat info;
  size_t file_size = 0;
  if (stat(absolut_path.c_str(), &info) < 0) {
    ESP_LOGE(TAG, "Failed to stat file: %s", strerror(errno));
    return -1;
  }
  return info.st_size;
}

std::string SdMmc::sd_card_type() const {
  if (this->card_->is_sdio) {
    return "SDIO";
  } else if (this->card_->is_mmc) {
    return "MMC";
  } else {
    return (this->card_->ocr & SD_OCR_SDHC_CAP) ? "SDHC/SDXC" : "SDSC";
  }
  return "UNKNOWN";
}

void SdMmc::update_sensors() {
#ifdef USE_SENSOR
  if (this->card_ == nullptr)
    return;

  uint64_t total_bytes = 0, free_bytes = 0, used_bytes = 0;
  esp_err_t ret = esp_vfs_fat_info(MOUNT_POINT.c_str(), &total_bytes, &free_bytes);
  if (ret == ESP_OK) {
    used_bytes = total_bytes - free_bytes;
  } else {
    ESP_LOGE(TAG, "Failed to get filesystem info: %s", esp_err_to_name(ret));
  }

  if (this->used_space_sensor_ != nullptr)
    this->used_space_sensor_->publish_state(used_bytes);
  if (this->total_space_sensor_ != nullptr)
    this->total_space_sensor_->publish_state(total_bytes);
  if (this->free_space_sensor_ != nullptr)
    this->free_space_sensor_->publish_state(free_bytes);

  for (auto &sensor : this->file_size_sensors_) {
    if (sensor.sensor != nullptr)
      sensor.sensor->publish_state(this->file_size(sensor.path));
  }
#endif
}

bool SdMmc::create_directory(const char *path) {
  ESP_LOGV(TAG, "Create directory: %s", path);
  std::string absolut_path = build_path(path);
  if (mkdir(absolut_path.c_str(), 0777) < 0) {
    ESP_LOGE(TAG, "Failed to create a new directory: %s", strerror(errno));
    return false;
  }
  this->update_sensors();
  return true;
}

bool SdMmc::remove_directory(const char *path) {
  ESP_LOGV(TAG, "Remove directory: %s", path);
  if (!this->is_directory(path)) {
    ESP_LOGE(TAG, "Not a directory");
    return false;
  }
  std::string absolut_path = build_path(path);
  if (remove(absolut_path.c_str()) != 0) {
    ESP_LOGE(TAG, "Failed to remove directory: %s", strerror(errno));
  }
  this->update_sensors();
  return true;
}

bool SdMmc::delete_file(const char *path) {
  ESP_LOGV(TAG, "Delete File: %s", path);
  if (this->is_directory(path)) {
    ESP_LOGE(TAG, "Not a file");
    return false;
  }
  std::string absolut_path = build_path(path);
  if (remove(absolut_path.c_str()) != 0) {
    ESP_LOGE(TAG, "Failed to remove file: %s", strerror(errno));
  }
  this->update_sensors();
  return true;
}

// Lecture complète d'un fichier
std::vector<uint8_t> SdMmc::read_file(const char *path) {
  ESP_LOGV(TAG, "Read File: %s", path);

  // Vérifier d'abord la taille du fichier
  size_t file_size = this->file_size(path);

  // file_size() renvoie (size_t)-1 si stat() echoue (fichier introuvable).
  // Sans ce test, la valeur enorme passait le seuil "trop gros" et masquait
  // la vraie cause ("File too large" au lieu de "fichier introuvable").
  if (file_size == static_cast<size_t>(-1)) {
    ESP_LOGE(TAG, "Failed to read, file not found: %s", path);
    return {};
  }

  // Limite de sécurité, par exemple 5MB
  constexpr size_t MAX_SAFE_SIZE = 5 * 1024 * 1024;

  if (file_size > MAX_SAFE_SIZE) {
    ESP_LOGE(TAG, "File too large for direct reading: %zu bytes (max: %zu). Use read_file_stream instead.", 
             file_size, MAX_SAFE_SIZE);
    return {};
  }

  std::string absolut_path = build_path(path);
  FILE *file = fopen(absolut_path.c_str(), "rb");
  if (file == nullptr) {
    ESP_LOGE(TAG, "Failed to open file for reading");
    return {};
  }

  std::vector<uint8_t> res(file_size);
  size_t read_len = fread(res.data(), 1, file_size, file);
  fclose(file);

  if (read_len != file_size) {
    ESP_LOGE(TAG, "Read incomplete: expected %zu bytes, got %zu", file_size, read_len);
    return {};
  }

  return res;
}



// Lecture en streaming par chunks avec reset du WDT
void SdMmc::read_file_stream(const char *path, size_t offset, size_t chunk_size,
                             std::function<void(const uint8_t*, size_t)> callback) {
  std::string absolut_path = build_path(path);
  FILE *file = fopen(absolut_path.c_str(), "rb");
  if (!file) {
    ESP_LOGE(TAG, "Failed to open file: %s", absolut_path.c_str());
    return;
  }

  std::unique_ptr<FILE, decltype(&fclose)> file_guard(file, fclose);

  if (fseek(file, offset, SEEK_SET) != 0) {
    ESP_LOGE(TAG, "Failed to seek to position %zu in file: %s (errno: %d)", offset, absolut_path.c_str(), errno);
    return;
  }

  std::vector<uint8_t> buffer(chunk_size);
  size_t read = 0;
  size_t bytes_since_reset = 0;

  while ((read = fread(buffer.data(), 1, chunk_size, file)) > 0) {
    callback(buffer.data(), read);
    bytes_since_reset += read;

    if (bytes_since_reset >= 64 * 1024) {
      esp_task_wdt_reset();
      bytes_since_reset = 0;
    }
  }

  if (ferror(file)) {
    ESP_LOGE(TAG, "Error reading file: %s", absolut_path.c_str());
  }
}

// Fonction optimisée pour la lecture de fichiers vidéo
// Cette fonction est un wrapper simplifié de read_file_stream() pour les cas d'usage courants
//
// Paramètres:
//   - path: chemin du fichier vidéo
//   - max_size: taille maximale à lire (0 = lire le fichier complet)
//
// Avantages par rapport à read_file():
// - Pas de limite de 5MB
// - Reset du watchdog automatique pour éviter les timeouts
// - Optimisé pour les gros fichiers vidéo (300+ Mo)
//
// Note: Pour les très gros fichiers (>500 Mo), préférez utiliser read_file_stream()
// directement avec un callback pour éviter d'allouer trop de mémoire d'un coup
std::vector<uint8_t> SdMmc::read_file_video(const char *path, size_t max_size) {
  // Vérifier la taille du fichier ((size_t)-1 = stat() a echoue = introuvable)
  size_t file_size = this->file_size(path);
  if (file_size == 0 || file_size == static_cast<size_t>(-1)) {
    ESP_LOGE(TAG, "File not found or empty: %s", path);
    return {};
  }

  // Si max_size est spécifié, limiter la lecture
  size_t bytes_to_read = (max_size > 0 && max_size < file_size) ? max_size : file_size;

  // Vérification de mémoire disponible
  size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  if (bytes_to_read > free_heap / 2) {
    ESP_LOGE(TAG, "Not enough memory to read video file: need %zu bytes, only %zu available",
             bytes_to_read, free_heap);
    ESP_LOGE(TAG, "Use read_file_stream() with callback for large files");
    return {};
  }

  ESP_LOGI(TAG, "Reading video file: %s (%zu bytes)", path, bytes_to_read);

  // Préparer le buffer de sortie
  std::vector<uint8_t> result;
  result.reserve(bytes_to_read);

  // Utiliser read_file_stream avec un callback qui accumule les données
  size_t bytes_read = 0;
  this->read_file_stream(path, 0, 32 * 1024, [&](const uint8_t* data, size_t len) {
    // Limiter au max_size si spécifié
    size_t to_append = len;
    if (max_size > 0 && bytes_read + len > max_size) {
      to_append = max_size - bytes_read;
    }

    if (to_append > 0) {
      result.insert(result.end(), data, data + to_append);
      bytes_read += to_append;
    }
  });

  ESP_LOGI(TAG, "Video file read complete: %zu bytes", result.size());
  return result;
}

// Lecture d'un seul morceau (chunk) : chunk_size octets a partir de offset.
// Cette surcharge const char* etait declaree et appelee par la version
// std::string (et par l'action ReadFileChunked) mais n'etait pas definie.
std::vector<uint8_t> SdMmc::read_file_chunked(const char *path, size_t offset, size_t chunk_size) {
  std::string absolut_path = build_path(path);
  FILE *file = fopen(absolut_path.c_str(), "rb");
  if (file == nullptr) {
    ESP_LOGE(TAG, "Failed to open file for reading: %s (errno=%d)", absolut_path.c_str(), errno);
    return {};
  }
  std::unique_ptr<FILE, decltype(&fclose)> file_guard(file, fclose);

  if (offset != 0 && fseek(file, offset, SEEK_SET) != 0) {
    ESP_LOGE(TAG, "Failed to seek to offset %zu in %s (errno=%d)", offset, absolut_path.c_str(), errno);
    return {};
  }

  std::vector<uint8_t> buffer(chunk_size);
  size_t read_len = fread(buffer.data(), 1, chunk_size, file);
  buffer.resize(read_len);
  return buffer;
}

#endif
size_t SdMmc::file_size(std::string const &path) { return this->file_size(path.c_str()); }

bool SdMmc::is_directory(std::string const &path) { return this->is_directory(path.c_str()); }

bool SdMmc::delete_file(std::string const &path) { return this->delete_file(path.c_str()); }

std::vector<uint8_t> SdMmc::read_file(std::string const &path) { return this->read_file(path.c_str()); }

std::vector<uint8_t> SdMmc::read_file_chunked(std::string const &path, size_t offset, size_t chunk_size) {
  return this->read_file_chunked(path.c_str(), offset, chunk_size);
}

#ifdef USE_SENSOR
void SdMmc::add_file_size_sensor(sensor::Sensor *sensor, std::string const &path) {
  this->file_size_sensors_.emplace_back(sensor, path);
}
#endif

void SdMmc::set_clk_pin(uint8_t pin) { this->clk_pin_ = pin; }

void SdMmc::set_cmd_pin(uint8_t pin) { this->cmd_pin_ = pin; }

void SdMmc::set_data0_pin(uint8_t pin) { this->data0_pin_ = pin; }

void SdMmc::set_data1_pin(uint8_t pin) { this->data1_pin_ = pin; }

void SdMmc::set_data2_pin(uint8_t pin) { this->data2_pin_ = pin; }

void SdMmc::set_data3_pin(uint8_t pin) { this->data3_pin_ = pin; }

void SdMmc::set_mode_1bit(bool b) { this->mode_1bit_ = b; }

void SdMmc::set_power_ctrl_pin(GPIOPin *pin) { this->power_ctrl_pin_ = pin; }

std::string SdMmc::error_code_to_string(SdMmc::ErrorCode code) {
  switch (code) {
    case ErrorCode::ERR_PIN_SETUP:
      return "Failed to set pins";
    case ErrorCode::ERR_MOUNT:
      return "Failed to mount card";
    case ErrorCode::ERR_NO_CARD:
      return "No card found";
    default:
      return "Unknown error";
  }
}

long double convertBytes(uint64_t value, MemoryUnits unit) {
  return value * 1.0 / pow(1024, static_cast<uint64_t>(unit));
}

FileInfo::FileInfo(std::string const &path, size_t size, bool is_directory)
    : path(path), size(size), is_directory(is_directory) {}

}  // namespace sd_mmc_card
}  // namespace esphome




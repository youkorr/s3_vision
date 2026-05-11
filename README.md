# s3_yolo

Composant ESPHome pour faire tourner **YOLO11** (détection d'objets COCO 80 classes) sur **ESP32-S3** via le framework **ESP-DL** d'Espressif, en utilisant la caméra DVP du composant `esp32_camera` standard.

Le composant capture en RGB565, fait l'inférence sur un core dédié, dessine les boîtes de détection sur l'image, encode en JPEG, et expose le tout via des triggers ESPHome (`on_object_detected`, `on_detection_image`) pour intégration MQTT / Home Assistant.

---

## Sommaire
- [Prérequis matériels](#prérequis-matériels)
- [Installation](#installation)
- [Configuration YAML complète](#configuration-yaml-complète)
- [Options du composant `yolov11`](#options-du-composant-yolov11)
- [Triggers](#triggers)
- [Actions](#actions)
- [Modèles disponibles](#modèles-disponibles)
- [Intégration MQTT / Home Assistant](#intégration-mqtt--home-assistant)
- [Table des partitions](#table-des-partitions)
- [Dépannage](#dépannage)
- [Architecture interne](#architecture-interne)

---

## Prérequis matériels

| Élément | Minimum | Recommandé |
|---|---|---|
| MCU | ESP32-**S3** (S2 / C3 / C6 non supportés) | ESP32-S3 R8 (Octal PSRAM) |
| Flash | 8 MB | 16 MB |
| PSRAM | 2 MB Quad | 8 MB Octal |
| Caméra | DVP / parallèle (OV2640, OV3660, OV5640…) | OV2640 320×240 |

**Important** : la caméra doit être en `pixel_format: RGB565`. Le S3 n'a pas de décodeur JPEG hardware ; tenter de décoder du JPEG en software pendant l'inférence fait tomber le framerate sous 1 fps et risque de crasher.

Boards testées :
- Waveshare ESP32-S3-SIM7670G-4G
- ESP32-S3-DevKitC + module caméra OV2640
- Freenove ESP32-S3 WROOM

---

## Installation

### 1. Référencer le composant dans le YAML

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/youkorr/s3_yolo
      ref: main
    components: [yolov11]
    refresh: 0s
```

Pendant la phase de debug, mettre `refresh: 0s` pour forcer ESPHome à re-pull à chaque build. Une fois stable, repasser à `refresh: 1d` (défaut).

### 2. Placer un modèle `.espdl` à côté du YAML

Au choix :

**A — Modèle embarqué dans le firmware (recommandé)** :
```yaml
yolov11:
  model_path: ./coco_detect_yolo11n_320_s8_v3.espdl
```
Le fichier `.espdl` est lu au moment du build et embarqué en flash rodata. Aucune RAM utilisée.

**B — Modèle via le composant `file:` de jesserockz** :
```yaml
external_components:
  - source: github://jesserockz/esphome-components@1b449c22e749933d11ca57c77d8303f851a817e1
    components: [file]
    refresh: 10s

file:
  - id: model_coco_detect
    path: ./coco_detect_yolo11n_320_s8_v3.espdl

yolov11:
  model_id: model_coco_detect
```

Si ni `model_path:` ni `model_id:` ne sont fournis, le build script utilise par défaut `components/models/coco_detect/models/s3/coco_detect_yolo11n_s8_v1.espdl` du repo.

### 3. Table de partitions custom

Le binaire fait ~7 MB (modèle 2.9 MB + ESP-DL + ESPHome + framework). La partition app par défaut (1.8 MB) ne suffit pas. Créer `partitions_custom_16mb.csv` à côté du YAML :

```csv
# Name,    Type, SubType, Offset,   Size,    Flags
nvs,       data, nvs,     0x9000,   0x6000,
phy_init,  data, phy,     0xf000,   0x1000,
factory,   app,  factory, 0x10000,  0xC00000,
nvs_keys,  data, nvs_keys,0xc10000, 0x1000,
```

12 MB app, 4 MB libres pour NVS / SPIFFS / OTA si besoin.

Référencer dans le YAML :
```yaml
esp32:
  partitions: partitions_custom_16mb.csv
  framework:
    type: esp-idf
    sdkconfig_options:
      CONFIG_PARTITION_TABLE_CUSTOM: "y"
      CONFIG_PARTITION_TABLE_CUSTOM_FILENAME: "partitions_custom_16mb.csv"
      CONFIG_ESPTOOLPY_FLASHSIZE_16MB: "y"
```

---

## Configuration YAML complète

```yaml
substitutions:
  name: my-yolo-cam

esphome:
  name: ${name}
  min_version: 2025.1.0

esp32:
  board: esp32-s3-devkitc-1
  cpu_frequency: 240Mhz
  flash_size: 16MB
  partitions: partitions_custom_16mb.csv
  framework:
    type: esp-idf
    sdkconfig_options:
      CONFIG_COMPILER_OPTIMIZATION_PERF: "y"
      CONFIG_ESP32_DEFAULT_CPU_FREQ_240: "y"
      CONFIG_PARTITION_TABLE_CUSTOM: "y"
      CONFIG_PARTITION_TABLE_CUSTOM_FILENAME: "partitions_custom_16mb.csv"
      CONFIG_ESPTOOLPY_FLASHSIZE_16MB: "y"

psram:
  mode: octal           # ou "quad" selon ta board (R2 = quad, R8 = octal)
  speed: 80MHz

logger:
  level: INFO

api:
  encryption:
    key: !secret api_key

ota:
  - platform: esphome

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

external_components:
  - source:
      type: git
      url: https://github.com/youkorr/s3_yolo
      ref: main
    components: [yolov11]
    refresh: 0s

i2c:
  - id: camera_i2c
    sda: GPIO15
    scl: GPIO16

esp32_camera:
  id: my_camera
  name: ${name}_camera
  external_clock:
    pin: GPIO39
    frequency: 20MHz
  i2c_id: camera_i2c
  data_pins: [GPIO07, GPIO08, GPIO09, GPIO10, GPIO11, GPIO12, GPIO13, GPIO14]
  vsync_pin: GPIO42
  href_pin: GPIO41
  pixel_clock_pin: GPIO46
  resolution: 320x240
  pixel_format: RGB565     # OBLIGATOIRE
  # NE PAS définir jpeg_quality - sinon ESPHome convertit RGB565 -> JPEG
  # avant de livrer le frame à YOLO, ce qui casse l'inférence.
  frame_buffer_count: 1
  frame_buffer_location: PSRAM
  idle_framerate: 0.05fps

yolov11:
  id: my_yolo
  esp32_camera_id: my_camera
  model_path: ./coco_detect_yolo11n_320_s8_v3.espdl

  # Seuils de détection
  score_threshold: 0.30        # 0..1, classes en dessous sont filtrées
  nms_threshold: 0.50          # 0..1, suppression non-maxima entre boîtes

  # Cadence et limites
  detection_interval_ms: 200   # délai minimum entre 2 inférences (ms)
  max_detections: 10           # max d'objets retournés par frame

  # Tâche FreeRTOS d'inférence (core 1)
  inference_task_stack_size: 8192
  inference_task_priority: 5

  # Doit matcher la résolution de la caméra
  frame_width: 320
  frame_height: 240

  # Encodage des snapshots
  jpeg_quality: 50             # 1..100 (50 ~ 5-15 KB pour 320x240)
  draw_boxes: true             # dessine les boîtes + labels sur le snapshot
```

---

## Options du composant `yolov11`

| Option | Type | Défaut | Description |
|---|---|---|---|
| `id` | id | — | Identifiant ESPHome |
| `esp32_camera_id` | id | — | **Requis**. Réf. au composant `esp32_camera:` |
| `model_path` | string | — | Chemin relatif vers un fichier `.espdl` (embarqué au build) |
| `model_id` | id | — | Alternative : référence à un composant `file:` |
| `score_threshold` | float | 0.30 | Confiance minimum pour qu'une détection soit retenue |
| `nms_threshold` | float | 0.50 | Seuil IoU pour la non-maximum suppression |
| `detection_interval_ms` | int | 200 | Délai minimum entre 2 inférences |
| `max_detections` | int | 10 | Plafond de détections par inférence |
| `inference_task_stack_size` | int | 8192 | Taille de pile de la tâche FreeRTOS |
| `inference_task_priority` | int | 5 | Priorité FreeRTOS (1-10) |
| `frame_width` | int | 320 | Doit matcher la caméra |
| `frame_height` | int | 240 | Doit matcher la caméra |
| `jpeg_quality` | int | 50 | Qualité JPEG des snapshots (1-100) |
| `draw_boxes` | bool | true | Overlay des rectangles + labels sur le JPEG |
| `on_object_detected` | trigger | — | Voir [Triggers](#triggers) |
| `on_detection_image` | trigger | — | Voir [Triggers](#triggers) |

---

## Triggers

### `on_object_detected`

Tiré à **chaque inférence** (donc toutes les `detection_interval_ms`, même si rien n'est détecté).

Variables disponibles :
- `int object_count` — nombre d'objets détectés
- `std::string summary` — chaîne `"label:score,label:score,..."` (`"none"` si rien)

```yaml
on_object_detected:
  - then:
      - logger.log:
          format: "Detected %d objects: %s"
          args: ["object_count", "summary.c_str()"]
```

### `on_detection_image`

Tiré **uniquement quand `object_count > 0`**, après encodage JPEG du frame.

Variable disponible :
- `image` — struct `{uint8_t *data; size_t length;}` pointant sur le buffer JPEG

```yaml
on_detection_image:
  - then:
      - mqtt.publish:
          topic: device/${name}/camera/snapshot
          payload: !lambda 'return esphome::base64_encode(image.data, image.length);'
```

⚠️ Le buffer `image.data` est libéré après que tous les callbacks ont retourné. Ne pas le stocker.

---

## Actions

### `yolov11.inference`

Force une inférence one-shot maintenant (ignore `detection_interval_ms`).

```yaml
button:
  - platform: template
    name: "Force YOLO inference"
    on_press:
      - yolov11.inference: my_yolo
```

### `yolov11.start` / `yolov11.stop`

Active / désactive le pipeline d'inférence au runtime. Les frames continuent d'être captureées par la caméra, mais sont **droppées** par YOLO. La tâche FreeRTOS reste vivante (coût CPU nul).

```yaml
switch:
  - platform: template
    name: "${name}_yolo_enabled"
    id: yolo_enabled
    optimistic: true
    restore_mode: ALWAYS_ON
    turn_on_action:
      - yolov11.start: my_yolo
    turn_off_action:
      - yolov11.stop: my_yolo
```

Avec MQTT discovery activé, le switch apparaît automatiquement dans Home Assistant.

---

## Modèles disponibles

Le repo embarque plusieurs `.espdl` pré-quantifiés pour S3 dans `components/models/coco_detect/models/s3/` :

| Fichier | Taille | Notes |
|---|---|---|
| `coco_detect_yolo11n_s8_v1.espdl` | 2.86 MB | YOLO11n v1, entrée 256×256 |
| `coco_detect_yolo11n_s8_v2.espdl` | 2.92 MB | v2 |
| `coco_detect_yolo11n_s8_v3.espdl` | 2.86 MB | **v3 (le plus précis)** |
| `coco_detect_yolo11n_320_s8_v3.espdl` | 2.86 MB | v3 entraîné spécifiquement en 320×320 |

Tous détectent les **80 classes COCO** (`person`, `bicycle`, `car`, `motorcycle`, … `toothbrush`).

Pour utiliser un modèle custom (entraîné sur ton propre dataset), exporte-le avec [esp-ppq](https://github.com/espressif/esp-ppq) au format `.espdl` quantifié S8 et passe le chemin via `model_path:`.

---

## Intégration MQTT / Home Assistant

### Discovery automatique HA

```yaml
mqtt:
  broker: !secret mqtt_broker
  username: !secret mqtt_user
  password: !secret mqtt_password
  discovery: true              # auto-expose les entités
```

Tout `sensor:`, `switch:`, `text_sensor:` apparaît dans HA sans config manuelle.

### Publication snapshot + métadonnées JSON

```yaml
yolov11:
  on_detection_image:
    - then:
        - if:
            condition:
              mqtt.connected:
            then:
              # Snapshot JPEG (base64)
              - mqtt.publish:
                  topic: device/${name}/camera/snapshot
                  payload: !lambda 'return esphome::base64_encode(image.data, image.length);'
              # Métadonnées avec bbox + classes
              - mqtt.publish:
                  topic: device/${name}/yolo_detection/state
                  payload: !lambda |-
                    auto dets = id(my_yolo).get_detections();
                    std::string out = "{\"count\":";
                    out += std::to_string(dets.size());
                    out += ",\"objects\":[";
                    for (size_t i = 0; i < dets.size(); i++) {
                      if (i) out += ",";
                      char buf[128];
                      snprintf(buf, sizeof(buf),
                        "{\"class\":\"%s\",\"score\":%d,\"x1\":%d,\"y1\":%d,\"x2\":%d,\"y2\":%d}",
                        dets[i].label, (int)(dets[i].score * 100),
                        dets[i].x1, dets[i].y1, dets[i].x2, dets[i].y2);
                      out += buf;
                    }
                    out += "]}";
                    return out;
```

Payload exemple :
```json
{
  "count": 2,
  "objects": [
    {"class":"person","score":87,"x1":12,"y1":30,"x2":98,"y2":215},
    {"class":"dog","score":62,"x1":150,"y1":160,"x2":230,"y2":230}
  ]
}
```

### Text sensor du dernier résumé

```yaml
text_sensor:
  - platform: yolov11
    yolov11_id: my_yolo
    detection:
      id: my_detection
      name: ${name}_current_detection
      on_value:
        then:
          - mqtt.publish:
              topic: device/${name}/yolo_detection/text
              payload: !lambda "return x;"
```

Format publié : `person:87,car:62,dog:55` ou `none`.

### Côté Home Assistant

```yaml
# configuration.yaml
mqtt:
  sensor:
    - name: "YOLO count"
      state_topic: "device/my-yolo-cam/yolo_detection/state"
      value_template: "{{ value_json.count }}"
    - name: "YOLO classes"
      state_topic: "device/my-yolo-cam/yolo_detection/state"
      value_template: >
        {{ value_json.objects | map(attribute='class') | list | unique | join(', ') }}
  image:
    - name: "YOLO snapshot"
      image_topic: "device/my-yolo-cam/camera/snapshot"
      content_type: "image/jpeg"
      image_encoding: "b64"
```

---

## Table des partitions

Voir [Installation §3](#3-table-de-partitions-custom). Si tu veux garder l'OTA dual-slot :

```csv
# Name,   Type, SubType, Offset,   Size,    Flags
nvs,      data, nvs,     0x9000,   0x6000,
otadata,  data, ota,     0xf000,   0x2000,
app0,     app,  ota_0,   0x10000,  0x600000,
app1,     app,  ota_1,   0x610000, 0x600000,
nvs_keys, data, nvs_keys,0xc10000, 0x1000,
spiffs,   data, spiffs,  0xc11000, 0x3ef000,
```

6 MB × 2 OTA + ~4 MB SPIFFS.

---

## Dépannage

### `Frame size XXXX < expected 153600 for RGB565 320x240`

ESPHome convertit le frame RGB565 en JPEG avant de le donner à YOLO. **Retirer la ligne `jpeg_quality:`** de la config `esp32_camera:`. Cf code esphome upstream `esp32_camera.cpp` :
```cpp
if (pixel_format != PIXFORMAT_JPEG && jpeg_quality > 0) {
    // converts to JPEG
}
```

### `ESP_ERROR_CHECK failed ... dl_image_preprocessor.cpp line 130`

L'`ImagePreprocessor::transform()` n'a pas trouvé de dispatcher pour la conversion src→dst. Vérifier qu'on est bien à jour avec la branche fixée (commit `c0ffa4a` ou plus récent) — les flags `CONFIG_PIX_CVT_*_SUPPORT` doivent être définis dans `__init__.py`.

Forcer le re-fetch :
```bash
rm -rf .esphome/external_components/
rm -rf .pioenvs/
```

### `Error: program size (XXXXX) > maximum allowed (1835008)`

Partition app trop petite. Utiliser `partitions_custom_16mb.csv` (cf [Installation §3](#3-table-de-partitions-custom)).

### `undefined reference to 'dl::base::dotprod(...)'`

Le composant fournit déjà des stubs scalaires inline dans `yolov11_component.cpp`. Si l'erreur persiste après pull récent, c'est que le cache `external_components/<hash>/` n'a pas été rafraîchi.

### `'CameraImage' is not a member of 'esp32_camera'`

Version d'ESPHome trop ancienne. Mettre à jour à ESPHome ≥ 2025.1.0 (le composant `camera::` abstrait n'existait pas avant).

### Détections fantaisistes (chien à la place de personne, etc.)

- Augmenter `score_threshold:` à 0.45-0.50
- Essayer le modèle `_320_s8_v3.espdl` (le plus précis)
- Vérifier la luminosité ambiante
- Filtrer les classes côté HA / lambda

### Le snapshot JPEG est vert/bleu/inversé

Byte order RGB565. Modifier `yolov11_component.cpp:293` :
```cpp
.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE,   // au lieu de BE
```

La plupart des caméras DVP livrent en BE mais certaines configurations diffèrent.

### Out-of-memory au démarrage

PSRAM insuffisante. Le composant alloue 153 KB (320×240×2) pour le buffer snapshot. Avec seulement 2 MB PSRAM et un modèle YOLO ~1 MB en activations, c'est juste. Solutions :
- Passer en résolution 240×176 (`frame_width: 240, frame_height: 176`)
- Désactiver `draw_boxes: false` pour économiser une copie temporaire
- Désactiver le streaming caméra HA en parallèle

---

## Architecture interne

```
┌─────────────────┐    on_camera_image()       ┌──────────────────┐
│  esp32_camera   ├───────────────────────────►│  YOLOv11         │
│  (driver DVP)   │  shared_ptr<CameraImage>   │  (CameraListener)│
└─────────────────┘                            └────────┬─────────┘
                                                        │ pending_frame_data_
                                                        ▼
┌────────────────────────────────────────────────────────────────┐
│ inference_task (core 1, FreeRTOS prio 5)                       │
│                                                                │
│ 1. wait on frame_signal_ semaphore                             │
│ 2. dl::image::ImagePreprocessor::preprocess(rgb565_frame)      │
│    └─ pixel_cvt_dispatch_rgb5652rgb888_qint8 (scalar fallback) │
│ 3. dl::Model::run() — YOLO11n quantifié S8                     │
│    └─ TIE728 SIMD kernels (dl/base/isa/tie728/*.S)             │
│ 4. yolo11PostProcessor::postprocess() — NMS + decode bboxes    │
│ 5. fire on_object_detected callbacks (count, summary)          │
│ 6. if count > 0:                                               │
│    a. memcpy frame -> PSRAM snapshot buffer                    │
│    b. draw_on_frame (boîtes + labels)        [if draw_boxes]   │
│    c. fmt2jpg(snapshot)                       [esp32-camera]   │
│    d. fire on_detection_image callbacks(image{data,length})    │
└────────────────────────────────────────────────────────────────┘
```

### Fichiers du composant

- `__init__.py` — schema YAML, codegen, build flags
- `yolov11_component.{h,cpp}` — composant principal + stubs `dotprod` / `mbedtls_aes`
- `yolo11_detect.hpp` / `yolo11_detect_inner.cpp` — wrapper ESP-DL pour YOLO11
- `yolov11_text_sensor.{h,cpp}` + `text_sensor.py` — sous-plateforme text_sensor
- `yolov11_build.py` — extra_script PlatformIO (sources ESP-DL, embed modèle)
- `dl_image_color_isa_stubs.cpp` — fallbacks scalaires des SIMD helpers ESP-DL (manquants pour S3)
- `dl_base_dotprod_no_dsp.cpp` — fallback scalaire dotprod (évite la dépendance esp-dsp)
- `mbedtls_aes_stub.c` — stubs weak pour fbs_loader (modèles non chiffrés)

### Tâche d'inférence

La tâche est épinglée sur **core 1** (priorité 5) pour ne pas concurrencer la stack Wi-Fi / API ESPHome qui tourne sur core 0. Stack par défaut 8 KB ; à augmenter si tu observes des stack overflow.

### Allocations PSRAM

Le composant alloue au setup :
- `frame_copy_buf_` — `width × height × 2` octets (153 KB en 320×240) pour le snapshot avant JPEG

Le buffer JPEG de sortie est alloué dynamiquement par `fmt2jpg()` à chaque détection et libéré après le trigger.

Activations du modèle (1-2 MB selon le modèle) allouées par ESP-DL en interne.

---

## Crédits

- [Espressif ESP-DL](https://github.com/espressif/esp-dl) — framework deep learning ESP32
- Modèles `.espdl` de [esp-dl/models/coco_detect](https://github.com/espressif/esp-dl/tree/master/models/coco_detect)
- Composant ESPHome [`esp32_camera`](https://github.com/esphome/esphome/tree/dev/esphome/components/esp32_camera)
- Composant [`file:` de jesserockz](https://github.com/jesserockz/esphome-components/tree/main/components/file)

## Licence

Voir `components/esp-dl/LICENSE` (Apache 2.0 pour les sources ESP-DL).
Code du composant `yolov11` sous la même licence Apache 2.0.

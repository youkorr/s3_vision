# s3_yolo

ESPHome external component to run **YOLO11** (COCO 80-class object detection) on **ESP32-S3** using Espressif's **ESP-DL** framework, fed by the standard `esp32_camera` DVP camera driver.

The component captures RGB565 frames, runs inference on a dedicated core, draws detection boxes on the image, encodes it to JPEG, and exposes everything through ESPHome triggers (`on_object_detected`, `on_detection_image`) for MQTT / Home Assistant integration.

---

## Table of contents
- [Hardware requirements](#hardware-requirements)
- [Installation](#installation)
- [Full YAML config](#full-yaml-config)
- [`yolov11` component options](#yolov11-component-options)
- [Triggers](#triggers)
- [Actions](#actions)
- [Available models](#available-models)
- [MQTT / Home Assistant integration](#mqtt--home-assistant-integration)
- [Runtime enable/disable switch](#runtime-enabledisable-switch)
- [Partition table](#partition-table)
- [Troubleshooting](#troubleshooting)
- [Internal architecture](#internal-architecture)

---

## Hardware requirements

| Item | Minimum | Recommended |
|---|---|---|
| MCU | ESP32-**S3** (S2 / C3 / C6 not supported) | ESP32-S3 R8 (Octal PSRAM) |
| Flash | 8 MB | 16 MB |
| PSRAM | 2 MB Quad | 8 MB Octal |
| Camera | DVP / parallel (OV2640, OV3660, OV5640…) | OV2640 320×240 |

**Important**: the camera must be configured with `pixel_format: RGB565`. The S3 has no hardware JPEG decoder; software-decoding JPEG frames during inference drops the framerate below 1 fps and may crash.

Tested boards:
- Waveshare ESP32-S3-SIM7670G-4G
- ESP32-S3-DevKitC + OV2640 camera module
- Freenove ESP32-S3 WROOM

---

## Installation

### 1. Reference the component in your YAML

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/youkorr/s3_yolo
      ref: main
    components: [yolov11]
    refresh: 0s
```

During debugging, set `refresh: 0s` to force ESPHome to re-pull on every build. Once stable, switch back to `refresh: 1d` (default).

### 2. Provide an `.espdl` model

Two options:

**A — Model embedded into the firmware (recommended)**:
```yaml
yolov11:
  model_path: ./coco_detect_yolo11n_320_s8_v3.espdl
```
The `.espdl` file is read at build time and embedded into flash rodata. No RAM cost at runtime.

**B — Model via jesserockz's `file:` component**:
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

If neither `model_path:` nor `model_id:` is provided, the build script falls back to `components/models/coco_detect/models/s3/coco_detect_yolo11n_s8_v1.espdl` shipped in the repo.

### 3. Custom partition table

The firmware is ~7 MB (model 2.9 MB + ESP-DL + ESPHome + framework). The default app partition (1.8 MB) is too small. Create `partitions_custom_16mb.csv` next to your YAML:

```csv
# Name,    Type, SubType, Offset,   Size,    Flags
nvs,       data, nvs,     0x9000,   0x6000,
phy_init,  data, phy,     0xf000,   0x1000,
factory,   app,  factory, 0x10000,  0xC00000,
nvs_keys,  data, nvs_keys,0xc10000, 0x1000,
```

12 MB app, 4 MB free for NVS / SPIFFS / OTA if needed.

Reference it in the YAML:
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

## Full YAML config

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
  mode: octal           # or "quad" depending on your board (R2 = quad, R8 = octal)
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
  pixel_format: RGB565     # MANDATORY
  # DO NOT set jpeg_quality - ESPHome would convert RGB565 -> JPEG before
  # passing the frame to YOLO, which breaks inference.
  frame_buffer_count: 1
  frame_buffer_location: PSRAM
  idle_framerate: 0.05fps

yolov11:
  id: my_yolo
  esp32_camera_id: my_camera
  model_path: ./coco_detect_yolo11n_320_s8_v3.espdl

  # Detection thresholds
  score_threshold: 0.30        # 0..1, lower-scoring classes are filtered out
  nms_threshold: 0.50          # 0..1, non-maximum suppression IoU threshold

  # Rate limiting
  detection_interval_ms: 200   # minimum delay between two inferences (ms)
  max_detections: 10           # max objects returned per frame

  # Inference FreeRTOS task (pinned on core 1)
  inference_task_stack_size: 8192
  inference_task_priority: 5

  # Must match the camera resolution
  frame_width: 320
  frame_height: 240

  # Snapshot encoding
  jpeg_quality: 50             # 1..100 (50 ~ 5-15 KB for 320x240)
  draw_boxes: true             # overlay boxes + labels on the snapshot
```

---

## `yolov11` component options

| Option | Type | Default | Description |
|---|---|---|---|
| `id` | id | — | ESPHome identifier |
| `esp32_camera_id` | id | — | **Required**. Reference to the `esp32_camera:` component |
| `model_path` | string | — | Relative path to an `.espdl` file (embedded at build) |
| `model_id` | id | — | Alternative: reference to a `file:` component |
| `score_threshold` | float | 0.30 | Minimum confidence for a detection to be kept |
| `nms_threshold` | float | 0.50 | IoU threshold for non-maximum suppression |
| `detection_interval_ms` | int | 200 | Minimum delay between two inferences |
| `max_detections` | int | 10 | Max detections per inference |
| `inference_task_stack_size` | int | 8192 | FreeRTOS task stack size |
| `inference_task_priority` | int | 5 | FreeRTOS task priority (1-10) |
| `frame_width` | int | 320 | Must match the camera |
| `frame_height` | int | 240 | Must match the camera |
| `jpeg_quality` | int | 50 | Snapshot JPEG quality (1-100) |
| `draw_boxes` | bool | true | Overlay bounding boxes + labels on the JPEG |
| `on_object_detected` | trigger | — | See [Triggers](#triggers) |
| `on_detection_image` | trigger | — | See [Triggers](#triggers) |

---

## Triggers

### `on_object_detected`

Fires on **every inference** (i.e. every `detection_interval_ms`, even when nothing is detected).

Available variables:
- `int object_count` — number of detected objects
- `std::string summary` — `"label:score,label:score,..."` string (`"none"` if no detection)

```yaml
on_object_detected:
  - then:
      - logger.log:
          format: "Detected %d objects: %s"
          args: ["object_count", "summary.c_str()"]
```

### `on_detection_image`

Fires **only when `object_count > 0`**, after JPEG encoding of the frame.

Available variable:
- `image` — struct `{uint8_t *data; size_t length;}` pointing at the JPEG buffer

```yaml
on_detection_image:
  - then:
      - mqtt.publish:
          topic: device/${name}/camera/snapshot
          payload: !lambda 'return esphome::base64_encode(image.data, image.length);'
```

⚠️ The `image.data` buffer is freed after all callbacks return. Do not store the pointer.

---

## Actions

### `yolov11.inference`

Force a one-shot inference now (bypasses `detection_interval_ms`).

```yaml
button:
  - platform: template
    name: "Force YOLO inference"
    on_press:
      - yolov11.inference: my_yolo
```

### `yolov11.start` / `yolov11.stop`

Enable / disable the inference pipeline at runtime. Camera frames keep flowing but are **dropped** by YOLO. The FreeRTOS task stays alive (zero CPU cost when stopped).

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

With MQTT discovery enabled the switch shows up automatically in Home Assistant.

---

## Available models

The repo ships several pre-quantized `.espdl` files for the S3 in `components/models/coco_detect/models/s3/`:

| File | Size | Notes |
|---|---|---|
| `coco_detect_yolo11n_s8_v1.espdl` | 2.86 MB | YOLO11n v1, 256×256 input |
| `coco_detect_yolo11n_s8_v2.espdl` | 2.92 MB | v2 |
| `coco_detect_yolo11n_s8_v3.espdl` | 2.86 MB | **v3 (most accurate)** |
| `coco_detect_yolo11n_320_s8_v3.espdl` | 2.86 MB | v3 trained specifically for 320×320 |

All detect the **80 COCO classes** (`person`, `bicycle`, `car`, `motorcycle`, … `toothbrush`).

For a custom model (trained on your own dataset), export it through [esp-ppq](https://github.com/espressif/esp-ppq) as an S8-quantized `.espdl` and point `model_path:` at it.

---

## MQTT / Home Assistant integration

### HA auto-discovery

```yaml
mqtt:
  broker: !secret mqtt_broker
  username: !secret mqtt_user
  password: !secret mqtt_password
  discovery: true              # auto-expose entities
```

Any `sensor:`, `switch:`, `text_sensor:` shows up in HA without manual config.

### Publishing snapshot + JSON metadata

```yaml
yolov11:
  on_detection_image:
    - then:
        - if:
            condition:
              mqtt.connected:
            then:
              # JPEG snapshot (base64)
              - mqtt.publish:
                  topic: device/${name}/camera/snapshot
                  payload: !lambda 'return esphome::base64_encode(image.data, image.length);'
              # Metadata with bboxes + classes
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

Sample payload:
```json
{
  "count": 2,
  "objects": [
    {"class":"person","score":87,"x1":12,"y1":30,"x2":98,"y2":215},
    {"class":"dog","score":62,"x1":150,"y1":160,"x2":230,"y2":230}
  ]
}
```

### Text sensor (latest summary) — optional

The `text_sensor: platform: yolov11` block is **fully optional**. It just exposes the latest detection summary as a Home Assistant text entity. Skip the whole block if you don't need it — the main `yolov11:` component works standalone.

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

Published format: `person:87,car:62,dog:55` or `none`.

### Home Assistant side

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

## Runtime enable/disable switch

The component exposes `yolov11.start` and `yolov11.stop` actions to gate the inference pipeline at runtime. While stopped, camera frames are dropped before being queued for YOLO — the FreeRTOS inference task stays alive (zero CPU cost) so resuming is instantaneous.

### Option A — Template switch (auto-exposed via HA discovery)

The cleanest approach. With `mqtt.discovery: true`, the switch shows up in Home Assistant as a toggle.

```yaml
switch:
  - platform: template
    name: "${name}_yolo_enabled"
    id: yolo_enabled
    icon: "mdi:eye"
    optimistic: true
    restore_mode: ALWAYS_ON       # or RESTORE_DEFAULT_ON / ALWAYS_OFF
    turn_on_action:
      - yolov11.start: my_yolo
      - logger.log: "YOLO inference started"
    turn_off_action:
      - yolov11.stop: my_yolo
      - logger.log: "YOLO inference stopped"
```

### Option B — Raw MQTT topic (no HA needed)

Subscribe to a custom topic and toggle from any MQTT client.

```yaml
mqtt:
  broker: ...
  on_message:
    - topic: device/${name}/yolo/set
      qos: 0
      then:
        - if:
            condition:
              lambda: 'return x == "on" || x == "ON" || x == "1" || x == "true";'
            then:
              - yolov11.start: my_yolo
              - mqtt.publish:
                  topic: device/${name}/yolo/state
                  retain: true
                  payload: "on"
            else:
              - yolov11.stop: my_yolo
              - mqtt.publish:
                  topic: device/${name}/yolo/state
                  retain: true
                  payload: "off"
```

Test from the command line:
```bash
mosquitto_pub -h <broker> -t "device/my-yolo-cam/yolo/set" -m "off"
mosquitto_pub -h <broker> -t "device/my-yolo-cam/yolo/set" -m "on"
```

### Option C — Combined (HA switch + raw MQTT topic, single source of truth)

Both HA and a raw MQTT topic stay in sync.

```yaml
mqtt:
  broker: ...
  on_message:
    - topic: device/${name}/yolo/set
      then:
        - if:
            condition: { lambda: 'return x == "on";' }
            then: [ switch.turn_on: yolo_enabled ]
            else: [ switch.turn_off: yolo_enabled ]

switch:
  - platform: template
    name: "${name}_yolo_enabled"
    id: yolo_enabled
    optimistic: true
    restore_mode: ALWAYS_ON
    turn_on_action:
      - yolov11.start: my_yolo
      - mqtt.publish:
          topic: device/${name}/yolo/state
          retain: true
          payload: "on"
    turn_off_action:
      - yolov11.stop: my_yolo
      - mqtt.publish:
          topic: device/${name}/yolo/state
          retain: true
          payload: "off"
```

### Other use cases for `yolov11.start` / `yolov11.stop`

**Schedule (e.g. only during daytime)**:
```yaml
time:
  - platform: sntp
    on_time:
      - hours: 22
        then: [ yolov11.stop: my_yolo ]
      - hours: 6
        then: [ yolov11.start: my_yolo ]
```

**One-shot inference via a button**:
```yaml
button:
  - platform: template
    name: "Snapshot now"
    on_press:
      - yolov11.start: my_yolo
      - delay: 5s
      - yolov11.stop: my_yolo
```

**Lambda gating from other components** (e.g. PIR motion sensor):
```yaml
binary_sensor:
  - platform: gpio
    pin: GPIO5
    name: "PIR"
    on_press:
      - lambda: 'id(my_yolo).set_inference_enabled(true);'
    on_release:
      - lambda: 'id(my_yolo).set_inference_enabled(false);'
```

The component also exposes `bool is_inference_enabled()` for queries.

---

## Partition table

See [Installation §3](#3-custom-partition-table). If you want dual-slot OTA:

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

## Troubleshooting

### `Frame size XXXX < expected 153600 for RGB565 320x240`

ESPHome silently converts the RGB565 frame to JPEG before passing it to YOLO. **Remove the `jpeg_quality:` line** from your `esp32_camera:` block. From the upstream esphome `esp32_camera.cpp`:
```cpp
if (pixel_format != PIXFORMAT_JPEG && jpeg_quality > 0) {
    // converts to JPEG
}
```

### `ESP_ERROR_CHECK failed ... dl_image_preprocessor.cpp line 130`

`ImagePreprocessor::transform()` couldn't find a dispatcher for the src→dst pixel conversion. Make sure you're on a recent commit — the `CONFIG_PIX_CVT_*_SUPPORT` flags must be defined in `__init__.py`.

Force a re-fetch:
```bash
rm -rf .esphome/external_components/
rm -rf .pioenvs/
```

### `Error: program size (XXXXX) > maximum allowed (1835008)`

App partition too small. Use `partitions_custom_16mb.csv` (cf [Installation §3](#3-custom-partition-table)).

### `undefined reference to 'dl::base::dotprod(...)'`

The component already ships inline scalar stubs in `yolov11_component.cpp`. If the error persists after a recent pull, the `external_components/<hash>/` cache hasn't been refreshed.

### `'CameraImage' is not a member of 'esp32_camera'`

ESPHome too old. Upgrade to ESPHome ≥ 2025.1.0 — the abstract `camera::` component didn't exist before.

### Wrong classes (dog instead of person, etc.)

- Raise `score_threshold:` to 0.45-0.50
- Try `_320_s8_v3.espdl` (the most accurate model)
- Check the ambient lighting
- Filter classes in HA / via a lambda

### Snapshot looks green/blue/inverted

RGB565 byte-order mismatch. Edit `yolov11_component.cpp:293`:
```cpp
.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE,   // instead of BE
```

Most DVP cameras output big-endian but a few configurations differ.

### Out-of-memory at boot

PSRAM too small. The component allocates 153 KB (320×240×2) for the snapshot buffer. With only 2 MB PSRAM and a YOLO model using ~1 MB for activations, it's tight. Options:
- Drop to 240×176 (`frame_width: 240, frame_height: 176`)
- Set `draw_boxes: false` to skip a temporary copy
- Disable the HA camera streaming feature in parallel

---

## Internal architecture

```
┌─────────────────┐    on_camera_image()       ┌──────────────────┐
│  esp32_camera   ├───────────────────────────►│  YOLOv11         │
│  (DVP driver)   │  shared_ptr<CameraImage>   │  (CameraListener)│
└─────────────────┘                            └────────┬─────────┘
                                                        │ pending_frame_data_
                                                        ▼
┌────────────────────────────────────────────────────────────────┐
│ inference_task (core 1, FreeRTOS prio 5)                       │
│                                                                │
│ 1. wait on frame_signal_ semaphore                             │
│ 2. dl::image::ImagePreprocessor::preprocess(rgb565_frame)      │
│    └─ pixel_cvt_dispatch_rgb5652rgb888_qint8 (scalar fallback) │
│ 3. dl::Model::run() — S8-quantized YOLO11n                     │
│    └─ TIE728 SIMD kernels (dl/base/isa/tie728/*.S)             │
│ 4. yolo11PostProcessor::postprocess() — NMS + decode bboxes    │
│ 5. fire on_object_detected callbacks (count, summary)          │
│ 6. if count > 0:                                               │
│    a. memcpy frame -> PSRAM snapshot buffer                    │
│    b. draw_on_frame (boxes + labels)         [if draw_boxes]   │
│    c. fmt2jpg(snapshot)                       [esp32-camera]   │
│    d. fire on_detection_image callbacks(image{data,length})    │
└────────────────────────────────────────────────────────────────┘
```

### Component files

- `__init__.py` — YAML schema, codegen, build flags
- `yolov11_component.{h,cpp}` — main component + inline `dotprod` / `mbedtls_aes` stubs
- `yolo11_detect.hpp` / `yolo11_detect_inner.cpp` — ESP-DL YOLO11 wrapper
- `yolov11_text_sensor.{h,cpp}` + `text_sensor.py` — text_sensor sub-platform
- `yolov11_build.py` — PlatformIO extra_script (ESP-DL sources, model embed)
- `dl_image_color_isa_stubs.cpp` — scalar fallbacks for the ESP-DL SIMD helpers (missing for S3)
- `dl_base_dotprod_no_dsp.cpp` — scalar dotprod fallback (avoids esp-dsp dependency)
- `mbedtls_aes_stub.c` — weak stubs for fbs_loader (we ship unencrypted models)

### Inference task

The task is pinned to **core 1** (priority 5) to avoid contending with the ESPHome Wi-Fi / API stack on core 0. Default stack 8 KB; bump it if you observe stack overflows.

### PSRAM allocations

The component allocates at setup:
- `frame_copy_buf_` — `width × height × 2` bytes (153 KB at 320×240) for the snapshot buffer before JPEG encoding

The JPEG output buffer is allocated dynamically by `fmt2jpg()` on every detection and freed after the trigger.

Model activations (1-2 MB depending on the variant) are allocated internally by ESP-DL.

---

## Credits

- [Espressif ESP-DL](https://github.com/espressif/esp-dl) — deep learning framework for ESP32
- `.espdl` models from [esp-dl/models/coco_detect](https://github.com/espressif/esp-dl/tree/master/models/coco_detect)
- ESPHome [`esp32_camera`](https://github.com/esphome/esphome/tree/dev/esphome/components/esp32_camera) component
- [jesserockz `file:` component](https://github.com/jesserockz/esphome-components/tree/main/components/file)

## License

See `components/esp-dl/LICENSE` (Apache 2.0 for the ESP-DL sources).
`yolov11` component code released under the same Apache 2.0 license.

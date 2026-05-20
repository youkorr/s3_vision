# s3_vision examples — ESP32-S3-A7670E + 4G + MQTT

Each YAML here is a complete, flashable ESPHome configuration for the
Waveshare ESP32-S3-A7670E board, configured for one specific model family.

| YAML | Family | Model file | Type | Output |
|---|---|---|---|---|
| `s3a7670e_coco_detect.yaml` | `coco_detect` | `coco_detect_yolo11n_320_s8_v3.espdl` | Detection | 80 COCO classes |
| `s3a7670e_pedestrian_detect.yaml` | `pedestrian_detect` | `pedestrian_detect_pico_s8_v1.espdl` | Detection | `person` |
| `s3a7670e_hand_detect.yaml` | `hand_detect` | `espdet_pico_224_224_hand.espdl` | Detection | `hand` |
| `s3a7670e_human_face_detect.yaml` | `human_face_detect` | `human_face_detect_msr_s8_v1.espdl` | Detection | `face` |
| `s3a7670e_coco_pose.yaml` | `coco_pose` | `coco_pose_yolo11n_pose_s8_v1.espdl` | Pose | `person` + 17 keypoints |
| `s3a7670e_imagenet_cls.yaml` | `imagenet_cls` | `imagenet_cls_mobilenetv2_s8_v1.espdl` | Classification | 1000 ImageNet classes |
| `s3a7670e_hand_gesture.yaml` | `hand_gesture_recognition` | `mobilenetv2_0_5_128_128_gesture.espdl` | Classification | Hand gesture labels |

## Triggers

There are two equivalent ways to subscribe to inference output — the
generic names (recommended, family-agnostic) and the family-specific
legacy names.

| Generic name (recommended) | Legacy alias | Args | Fires for |
|---|---|---|---|
| `on_event:` | `on_object_detected:`, `on_detection:` | `object_count`, `summary` | All families |
| `on_augmented_image:` | `on_detection_image:` | `image.data`, `image.length` | All families |
| `on_classification:` | — | `label`, `score` | Classification only |

`on_event` fires once per inference with `object_count` and `summary`:
- Detection / pose: `object_count` = number of boxes, `summary` = `"class:score,class:score,..."`
- Classification: `object_count` = `1` when above `score_threshold` else `0`, `summary` = `"label:score"`

`on_augmented_image` fires with a JPEG snapshot whenever there is
something to report (detections above threshold, or a classification
result above threshold). For detection/pose families the snapshot has
bounding boxes overlaid; for classification it's the raw frame.

`on_classification` is the family-specific trigger and fires only for
`imagenet_cls` / `hand_gesture_recognition` with the top-1 result.

## Unified JSON output

`id(my_vision).get_inference_json()` returns a JSON string adapted to the
current family, so the same YAML works across detection, pose and
classification builds:

```yaml
on_augmented_image:
  - then:
      - if:
          condition:
            modem.connected:
          then:
            - mqtt.publish:
                topic: device/${name}/camera/snapshot
                payload: !lambda return esphome::base64_encode(image.data, image.length);
            - mqtt.publish:
                topic: device/${name}/inference/state
                payload: !lambda return id(my_vision).get_inference_json();
```

Output shapes:

```jsonc
// coco_detect, pedestrian_detect, hand_detect, human_face_detect
{"type":"detection","count":2,"objects":[
  {"class":"person","score":0.87,"box":[12,40,180,220]},
  {"class":"dog","score":0.74,"box":[200,90,310,230]}
]}

// coco_pose - keypoints array is [x,y] for the 17 COCO joints
{"type":"pose","count":1,"objects":[
  {"class":"person","score":0.92,"box":[40,20,200,230],
   "keypoints":[[120,30],[125,28],[115,28], ...]}
]}

// imagenet_cls, hand_gesture_recognition
{"type":"classification","label":"golden_retriever","score":0.81,
 "topk":[{"label":"golden_retriever","score":0.81}, ...]}
```

Or roll your own lambda using `get_detections()` / `get_classifications()`
if you need a custom format.

## Usage

1. Drop the matching `.espdl` next to the YAML you want to flash (the
   `file:` component references `./<model_name>`).
2. Provide a `secrets.yaml` with `cloud_mqtt_address`, `cloud_mqtt_user`,
   `cloud_mqtt_password`, `cloud_certificate`, `sim_pin`.
3. Provide `partitions_custom_16mb.csv` (16 MB flash partition table).
4. `esphome run examples/s3a7670e_<family>.yaml`.

## Switching models

Each YAML uses two substitutions you can change without touching the rest:

```yaml
substitutions:
  model_name: <espdl filename>
  model_family: <coco_detect | pedestrian_detect | hand_detect | human_face_detect>
```

The pair MUST match — picking the wrong `model_family` for a given
`model_name` runs the wrong postprocessor on the model output and produces
zero detections (silent failure).

## What is NOT supported

The vision component only wires the 4 detection families above. Other
ESP-DL model categories shipped under `components/models/` are NOT usable
through `vision:` today:

| Folder | Type | Reason |
|---|---|---|
| `yolo26` | Detection | No YOLO26 postprocessor exists in `esp-dl/vision/detect/` |
| `human_face_recognition` | Recognition | Needs face embeddings + persistent DB (not implemented yet) |
| `motion_detect` | Motion | Different pipeline (frame diff, not NN) |
| `speaker_verification` | Audio | Not a vision model |

Also, within `human_face_detect`:
- `human_face_detect_msr_s8_v1.espdl` → **supported** (used here)
- `human_face_detect_mnp_s8_v1.espdl` → not supported (MNP postprocessor excluded)
- `espdet_pico_224_224_face.espdl` / `espdet_pico_416_416_face.espdl` →
  not directly supported; would work with `model_family: hand_detect`
  (same ESPDet postprocessor) but the reported label will be `hand`.

## Score threshold cheat-sheet

| Family | Upstream default | YAML default | Recommended in these examples |
|---|---|---|---|
| coco_detect | 0.25 | 0.30 | 0.30 |
| pedestrian_detect | 0.50 | 0.30 | 0.30 |
| hand_detect | 0.25 | 0.30 | **0.20** (lower than YAML default) |
| human_face_detect | 0.50 | 0.30 | 0.50 |

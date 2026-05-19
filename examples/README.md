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

## Trigger map per family

| Family | Trigger to use | Available data |
|---|---|---|
| Detection (coco_detect, pedestrian, hand, face, pose) | `on_object_detected:` / `on_detection_image:` | `object_count`, `summary`, `image.data`, `image.length` |
| Classification (imagenet_cls, hand_gesture_recognition) | `on_classification:` | `label` (string), `score` (float) |

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

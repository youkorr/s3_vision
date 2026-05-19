# s3_vision examples — ESP32-S3-A7670E + 4G + MQTT

Each YAML here is a complete, flashable ESPHome configuration for the
Waveshare ESP32-S3-A7670E board, configured for one specific model family.

| YAML | Family | Model file | Postprocessor | Classes |
|---|---|---|---|---|
| `s3a7670e_coco_detect.yaml` | `coco_detect` | `coco_detect_yolo11n_320_s8_v3.espdl` | YOLO11 | 80 (COCO) |
| `s3a7670e_pedestrian_detect.yaml` | `pedestrian_detect` | `pedestrian_detect_pico_s8_v1.espdl` | Pico | 1 (`person`) |
| `s3a7670e_hand_detect.yaml` | `hand_detect` | `espdet_pico_224_224_hand.espdl` | ESPDet | 1 (`hand`) |
| `s3a7670e_human_face_detect.yaml` | `human_face_detect` | `human_face_detect_msr_s8_v1.espdl` | MSR | 1 (`face`) |
| `s3a7670e_coco_pose.yaml` | `coco_pose` | `coco_pose_yolo11n_pose_s8_v1.espdl` | yolo11pose | 1 (`person`) + 17 keypoints |

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
| `imagenet_cls` | Classification | Outputs class probabilities, not boxes |
| `hand_gesture_recognition` | Recognition | Needs gesture recognition postprocessor |
| `human_face_recognition` | Recognition | Needs face embeddings + DB lookup |
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

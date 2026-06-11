# SPDX-FileCopyrightText: 2026 youkorr
# SPDX-License-Identifier: MIT
"""
ESPHome component: vision
--------------------------
Object detection for ESP32-S3 boards using the standard
`esp32_camera` component (DVP/parallel) instead of the MIPI-CSI
`esp_cam_sensor` we use on the ESP32-P4.

Camera input must be RGB565 - JPEG is NOT supported (the ESP32-S3 has
no hardware JPEG decoder and software decode would crash inference
under 5 fps). Set `pixel_format: rgb565` on your `esp32_camera:` block.

Model selection:
  - `model_path: ./my_model.espdl`  -> picks ANY .espdl file the user
    drops next to the YAML and embeds it at build time.
  - omitted: falls back to the bundled default .espdl model.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.core import CORE
from esphome import automation
import os

CODEOWNERS = ["@youkorr"]
DEPENDENCIES = ["esp32_camera"]

# ----- yaml keys -----
CONF_ESP32_CAMERA_ID = "esp32_camera_id"
CONF_MODEL_ID = "model_id"
CONF_MODEL_PATH = "model_path"
CONF_FEAT_MODEL_ID = "feat_model_id"
CONF_RECOGNITION_THRESHOLD = "recognition_threshold"
CONF_RECOGNITION_DB_PATH = "recognition_db_path"
CONF_ON_RECOGNITION = "on_recognition"
CONF_SCORE_THRESHOLD = "score_threshold"
CONF_NMS_THRESHOLD = "nms_threshold"
CONF_DETECTION_INTERVAL_MS = "detection_interval_ms"
CONF_ON_OBJECT_DETECTED = "on_object_detected"
CONF_ON_DETECTION = "on_detection"
CONF_ON_DETECTION_IMAGE = "on_detection_image"
CONF_ON_CLASSIFICATION = "on_classification"
# Generic aliases that work for ALL model families (detection, pose, classification).
# `on_event` fires once per inference with (object_count, summary).
# `on_augmented_image` fires with the JPEG snapshot (boxes drawn for detection
# and pose, raw frame for classification).
CONF_ON_EVENT = "on_event"
CONF_ON_AUGMENTED_IMAGE = "on_augmented_image"
CONF_INFERENCE_TASK_STACK_SIZE = "inference_task_stack_size"
CONF_INFERENCE_TASK_PRIORITY = "inference_task_priority"
CONF_MAX_DETECTIONS = "max_detections"
CONF_FRAME_WIDTH = "frame_width"
CONF_FRAME_HEIGHT = "frame_height"
CONF_JPEG_QUALITY = "jpeg_quality"
CONF_DRAW_BOXES = "draw_boxes"
CONF_DRAW_OUTPUTS = "draw_outputs"
CONF_MODEL_FAMILY = "model_family"
CONF_TOPK = "topk"

MODEL_FAMILIES = {
    "coco_detect": 0,
    "pedestrian_detect": 1,
    "hand_detect": 2,
    "human_face_detect": 3,
    "coco_pose": 4,
    "imagenet_cls": 5,
    "hand_gesture_recognition": 6,
    "human_face_recognition": 7,
    # Short aliases
    "detect": 0,
    "pedestrian": 1,
    "hand": 2,
    "face": 3,
    "pose": 4,
    "classify": 5,
    "gesture": 6,
    "recognize": 7,
    "face_recognition": 7,
}

# Classification families don't return bounding boxes. Used by the YAML
# validator and the build script to switch pipeline.
CLASSIFICATION_FAMILIES = {"imagenet_cls", "hand_gesture_recognition", "classify", "gesture"}

# Recognition families need a second model (the feature extractor) and a
# persistent embeddings database with id -> name mapping.
RECOGNITION_FAMILIES = {"human_face_recognition", "recognize", "face_recognition"}

# ----- C++ namespaces -----
vision_ns = cg.esphome_ns.namespace("vision")
VisionComponent = vision_ns.class_("VisionComponent", cg.Component)

ObjectDetectedTrigger = vision_ns.class_(
    "ObjectDetectedTrigger", automation.Trigger.template(cg.int_, cg.std_string)
)
DetectionImage = vision_ns.struct("DetectionImage")
DetectionImageTrigger = vision_ns.class_(
    "DetectionImageTrigger", automation.Trigger.template(DetectionImage)
)
ClassificationTrigger = vision_ns.class_(
    "ClassificationTrigger", automation.Trigger.template(cg.std_string, cg.float_)
)
RunInferenceAction = vision_ns.class_("RunInferenceAction", automation.Action)
StartInferenceAction = vision_ns.class_("StartInferenceAction", automation.Action)
StopInferenceAction = vision_ns.class_("StopInferenceAction", automation.Action)
EnrollFaceAction = vision_ns.class_("EnrollFaceAction", automation.Action)
ForgetFaceAction = vision_ns.class_("ForgetFaceAction", automation.Action)
ClearFacesAction = vision_ns.class_("ClearFacesAction", automation.Action)
RecognitionTrigger = vision_ns.class_(
    "RecognitionTrigger", automation.Trigger.template(cg.std_string, cg.float_)
)

# ----- esp32_camera reference -----
esp32_camera_ns = cg.esphome_ns.namespace("esp32_camera")
ESP32Camera = esp32_camera_ns.class_("ESP32Camera", cg.Component)


_TRIGGER_SCHEMA = automation.validate_automation(
    {
        cv.GenerateID(): cv.declare_id(ObjectDetectedTrigger),
    }
)

_DETECTION_IMAGE_TRIGGER_SCHEMA = automation.validate_automation(
    {
        cv.GenerateID(): cv.declare_id(DetectionImageTrigger),
    }
)

_CLASSIFICATION_TRIGGER_SCHEMA = automation.validate_automation(
    {
        cv.GenerateID(): cv.declare_id(ClassificationTrigger),
    }
)

_RECOGNITION_TRIGGER_SCHEMA = automation.validate_automation(
    {
        cv.GenerateID(): cv.declare_id(RecognitionTrigger),
    }
)


def _validate_model_path(value):
    """Validate that the .espdl file exists on disk, relative to YAML."""
    value = cv.string(value)
    if not value:
        raise cv.Invalid("model_path must not be empty")
    abs_path = value
    if not os.path.isabs(abs_path):
        abs_path = os.path.join(CORE.config_dir, value)
    if not os.path.isfile(abs_path):
        raise cv.Invalid(
            f"model_path: file not found at {abs_path}\n"
            f"Place your .espdl model next to your YAML and use a relative path."
        )
    return value


def _posix(p):
    """Normalise a path to forward slashes so gcc -I flags work on Windows.

    cg.add_build_flag(f"-I{path}") goes through platformio.ini's build_flags
    which on Windows can mishandle backslash-laden paths (gcc treats `\\` as
    line-continuation in some contexts and `\\d` as an escape in others).
    Using forward slashes is safe on every host OS - mingw and msys gcc
    accept them on Windows just like on Linux.
    """
    return p.replace("\\", "/")


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(VisionComponent),
        cv.Required(CONF_ESP32_CAMERA_ID): cv.use_id(ESP32Camera),
        cv.Optional(CONF_MODEL_PATH): _validate_model_path,
        cv.Optional(CONF_MODEL_ID): cv.use_id(cg.uint8),
        cv.Optional(CONF_FEAT_MODEL_ID): cv.use_id(cg.uint8),
        cv.Optional(CONF_RECOGNITION_THRESHOLD, default=0.5): cv.float_range(min=0.0, max=1.0),
        cv.Optional(CONF_RECOGNITION_DB_PATH, default="/sdcard/face_db"): cv.string,
        cv.Optional(CONF_SCORE_THRESHOLD, default=0.30): cv.float_range(min=0.0, max=1.0),
        cv.Optional(CONF_NMS_THRESHOLD, default=0.50): cv.float_range(min=0.0, max=1.0),
        cv.Optional(CONF_DETECTION_INTERVAL_MS, default=200): cv.int_range(min=50, max=10000),
        cv.Optional(CONF_MAX_DETECTIONS, default=10): cv.int_range(min=1, max=50),
        cv.Optional(CONF_INFERENCE_TASK_STACK_SIZE, default=8192): cv.int_range(min=4096, max=32768),
        cv.Optional(CONF_INFERENCE_TASK_PRIORITY, default=5): cv.int_range(min=1, max=10),
        cv.Optional(CONF_FRAME_WIDTH, default=320): cv.int_range(min=96, max=2560),
        cv.Optional(CONF_FRAME_HEIGHT, default=240): cv.int_range(min=96, max=1920),
        cv.Optional(CONF_JPEG_QUALITY, default=50): cv.int_range(min=1, max=100),
        cv.Optional(CONF_DRAW_OUTPUTS, default=True): cv.boolean,
        cv.Optional(CONF_DRAW_BOXES): cv.boolean,  # deprecated alias
        cv.Optional(CONF_MODEL_FAMILY, default="coco_detect"): cv.enum(MODEL_FAMILIES, lower=True),
        cv.Optional(CONF_TOPK, default=1): cv.int_range(min=1, max=10),
        cv.Optional(CONF_ON_OBJECT_DETECTED): _TRIGGER_SCHEMA,
        cv.Optional(CONF_ON_DETECTION): _TRIGGER_SCHEMA,
        cv.Optional(CONF_ON_EVENT): _TRIGGER_SCHEMA,
        cv.Optional(CONF_ON_DETECTION_IMAGE): _DETECTION_IMAGE_TRIGGER_SCHEMA,
        cv.Optional(CONF_ON_AUGMENTED_IMAGE): _DETECTION_IMAGE_TRIGGER_SCHEMA,
        cv.Optional(CONF_ON_CLASSIFICATION): _CLASSIFICATION_TRIGGER_SCHEMA,
        cv.Optional(CONF_ON_RECOGNITION): _RECOGNITION_TRIGGER_SCHEMA,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cam = await cg.get_variable(config[CONF_ESP32_CAMERA_ID])
    cg.add(var.set_camera(cam))

    cg.add(var.set_score_threshold(config[CONF_SCORE_THRESHOLD]))
    cg.add(var.set_nms_threshold(config[CONF_NMS_THRESHOLD]))
    cg.add(var.set_detection_interval_ms(config[CONF_DETECTION_INTERVAL_MS]))
    cg.add(var.set_max_detections(config[CONF_MAX_DETECTIONS]))
    cg.add(var.set_inference_task_stack_size(config[CONF_INFERENCE_TASK_STACK_SIZE]))
    cg.add(var.set_inference_task_priority(config[CONF_INFERENCE_TASK_PRIORITY]))
    cg.add(var.set_frame_width(config[CONF_FRAME_WIDTH]))
    cg.add(var.set_frame_height(config[CONF_FRAME_HEIGHT]))
    cg.add(var.set_jpeg_quality(config[CONF_JPEG_QUALITY]))
    # draw_outputs (preferred) takes priority over deprecated draw_boxes
    draw = config.get(CONF_DRAW_OUTPUTS, config.get(CONF_DRAW_BOXES, True))
    cg.add(var.set_draw_enabled(draw))
    cg.add(var.set_topk(config[CONF_TOPK]))

    if CONF_MODEL_PATH in config:
        model_path = config[CONF_MODEL_PATH]
        if not os.path.isabs(model_path):
            model_path = os.path.join(CORE.config_dir, model_path)
        # Forward to the build script via a CPP define. Forward-slash the
        # path so gcc on Windows doesn't choke on backslashes in the
        # macro value.
        cg.add_build_flag(f'-DVISION_USER_MODEL_PATH="{_posix(model_path)}"')

    if CONF_MODEL_ID in config:
        model_arr = await cg.get_variable(config[CONF_MODEL_ID])
        cg.add(var.set_model_buffer(model_arr, cg.RawExpression(f"sizeof({model_arr})")))
        cg.add_define("VISION_MODEL_FROM_FILE")

    # Face recognition needs a second model (the embedding extractor).
    if CONF_FEAT_MODEL_ID in config:
        feat_arr = await cg.get_variable(config[CONF_FEAT_MODEL_ID])
        cg.add(var.set_feat_model_buffer(feat_arr, cg.RawExpression(f"sizeof({feat_arr})")))
        cg.add_define("VISION_FEAT_MODEL_FROM_FILE")

    cg.add(var.set_recognition_threshold(config[CONF_RECOGNITION_THRESHOLD]))
    cg.add(var.set_recognition_db_path(config[CONF_RECOGNITION_DB_PATH]))

    # ------------------------------------------------------------------
    # Build flags - ESP32-S3 specific
    # ------------------------------------------------------------------
    cg.add_build_flag("-DESP_DL_MODEL_YOLO11=1")
    cg.add_build_flag("-DCONFIG_IDF_TARGET_ESP32S3=1")

    # Model family selection. Default = coco_detect (0). The C++ side
    # (vision_detect_inner.cpp) and the build script (vision_build.py)
    # both read VISION_FAMILY to pick the right postprocessor, default
    # .espdl file and per-family class names.
    # Resolve short aliases (e.g. "pose" -> "coco_pose") to the canonical
    # name so VISION_FAMILY_NAME_* build flags match the C++ #ifdefs.
    FAMILY_CANONICAL = {
        "detect": "coco_detect",
        "pedestrian": "pedestrian_detect",
        "hand": "hand_detect",
        "face": "human_face_detect",
        "pose": "coco_pose",
        "classify": "imagenet_cls",
        "gesture": "hand_gesture_recognition",
        "recognize": "human_face_recognition",
        "face_recognition": "human_face_recognition",
    }
    family_name = config[CONF_MODEL_FAMILY]
    family_id = MODEL_FAMILIES[family_name]
    canonical = FAMILY_CANONICAL.get(family_name, family_name)
    cg.add_build_flag(f"-DVISION_FAMILY={family_id}")
    cg.add_build_flag(f"-DVISION_FAMILY_NAME_{canonical.upper()}=1")

    cg.add_build_flag("-DCONFIG_COCO_DETECT_YOLO11N_S8_V1=1")
    cg.add_build_flag("-DCONFIG_DEFAULT_COCO_DETECT_MODEL=0")
    cg.add_build_flag("-DCONFIG_YOLO11_DETECT_S8_V1=1")
    cg.add_build_flag("-DCONFIG_YOLO11_DETECT_MODEL_TYPE=0")

    cg.add_build_flag("-DCONFIG_YOLO11_DETECT_MODEL_IN_FLASH_RODATA=1")
    cg.add_build_flag("-DCONFIG_YOLO11_DETECT_MODEL_LOCATION=0")
    cg.add_build_flag("-DCONFIG_COCO_DETECT_MODEL_IN_FLASH_RODATA=1")
    cg.add_build_flag("-DCONFIG_COCO_DETECT_MODEL_LOCATION=0")

    # ESP-DL pixel conversion support flags.
    cg.add_build_flag("-DCONFIG_PIX_CVT_RGB565_TO_RGB888_SUPPORT=1")
    cg.add_build_flag("-DCONFIG_PIX_CVT_RGB565_TO_RGB565_SUPPORT=1")
    cg.add_build_flag("-DCONFIG_PIX_CVT_RGB565_TO_GRAY_SUPPORT=1")
    cg.add_build_flag("-DCONFIG_PIX_CVT_RGB888_TO_RGB888_SUPPORT=1")
    cg.add_build_flag("-DCONFIG_PIX_CVT_RGB888_TO_RGB565_SUPPORT=1")
    cg.add_build_flag("-DCONFIG_PIX_CVT_RGB888_TO_GRAY_SUPPORT=1")
    cg.add_build_flag("-DCONFIG_PIX_CVT_GRAY_TO_GRAY_SUPPORT=1")

    # ------------------------------------------------------------------
    # ESP-DL include paths (S3 variants)
    # ------------------------------------------------------------------
    component_dir = os.path.dirname(__file__)
    parent_components_dir = os.path.dirname(component_dir)

    cg.add_build_flag(f"-I{_posix(component_dir)}")

    esp_dl_dir = os.path.join(parent_components_dir, "esp-dl")
    if os.path.exists(esp_dl_dir):
        for inc in [
            "dl",
            "dl/tool/include",
            "dl/tool/isa/tie728",
            "dl/tool/isa/xtensa",
            "dl/tool/src",
            "dl/tensor/include",
            "dl/tensor/src",
            "dl/base",
            "dl/base/isa",
            "dl/base/isa/tie728",
            "dl/base/isa/xtensa",
            "dl/math/include",
            "dl/math/src",
            "dl/model/include",
            "dl/model/src",
            "dl/module/include",
            "dl/module/src",
            "fbs_loader/include",
            "fbs_loader/src",
            "vision/detect",
            "vision/classification",
            "vision/image",
            "vision/image/isa",
        ]:
            inc_path = os.path.join(esp_dl_dir, inc)
            if os.path.exists(inc_path):
                cg.add_build_flag(f"-I{_posix(inc_path)}")

    # ------------------------------------------------------------------
    # Triggers (both yaml keys go through the same class)
    # ------------------------------------------------------------------
    triggers = []
    triggers.extend(config.get(CONF_ON_OBJECT_DETECTED, []))
    triggers.extend(config.get(CONF_ON_DETECTION, []))
    triggers.extend(config.get(CONF_ON_EVENT, []))
    for conf in triggers:
        trigger = cg.new_Pvariable(conf[CONF_ID], var)
        await automation.build_automation(
            trigger,
            [(cg.int_, "object_count"), (cg.std_string, "summary")],
            conf,
        )

    image_triggers = []
    image_triggers.extend(config.get(CONF_ON_DETECTION_IMAGE, []))
    image_triggers.extend(config.get(CONF_ON_AUGMENTED_IMAGE, []))
    for conf in image_triggers:
        trigger = cg.new_Pvariable(conf[CONF_ID], var)
        await automation.build_automation(
            trigger,
            [(DetectionImage, "image")],
            conf,
        )

    for conf in config.get(CONF_ON_CLASSIFICATION, []):
        trigger = cg.new_Pvariable(conf[CONF_ID], var)
        await automation.build_automation(
            trigger,
            [(cg.std_string, "label"), (cg.float_, "score")],
            conf,
        )

    for conf in config.get(CONF_ON_RECOGNITION, []):
        trigger = cg.new_Pvariable(conf[CONF_ID], var)
        await automation.build_automation(
            trigger,
            [(cg.std_string, "name"), (cg.float_, "similarity")],
            conf,
        )

    # ------------------------------------------------------------------
    # Build script (post: extra_scripts)
    # ------------------------------------------------------------------
    build_script = os.path.join(component_dir, "vision_build.py")
    if os.path.exists(build_script):
        cg.add_platformio_option("extra_scripts", [f"post:{_posix(build_script)}"])


# ============================================================================
# Action: vision.inference
# ============================================================================
INFERENCE_ACTION_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.use_id(VisionComponent),
    }
)


@automation.register_action(
    "vision.inference", RunInferenceAction, INFERENCE_ACTION_SCHEMA, synchronous=True
)
async def run_inference_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


# ============================================================================
# Actions: vision.start  /  vision.stop
# Resume/suspend the inference pipeline. Frames are dropped while stopped;
# the inference task itself stays alive so resume is instantaneous.
# ============================================================================
_GATING_ACTION_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.use_id(VisionComponent),
    }
)


@automation.register_action(
    "vision.start", StartInferenceAction, _GATING_ACTION_SCHEMA, synchronous=True
)
async def start_inference_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_action(
    "vision.stop", StopInferenceAction, _GATING_ACTION_SCHEMA, synchronous=True
)
async def stop_inference_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


# ============================================================================
# Actions: vision.enroll  /  vision.forget  /  vision.clear_faces
# Face recognition database management. Embeddings are stored on the path
# given by recognition_db_path; the human-readable name is mapped to the
# numeric id via NVS preferences.
# ============================================================================
CONF_NAME = "name"

ENROLL_ACTION_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.use_id(VisionComponent),
        cv.Required(CONF_NAME): cv.templatable(cv.string),
    }
)

FORGET_ACTION_SCHEMA = ENROLL_ACTION_SCHEMA


@automation.register_action(
    "vision.enroll", EnrollFaceAction, ENROLL_ACTION_SCHEMA
)
async def enroll_face_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    name_ = await cg.templatable(config[CONF_NAME], args, cg.std_string)
    cg.add(var.set_name(name_))
    return var


@automation.register_action(
    "vision.forget", ForgetFaceAction, FORGET_ACTION_SCHEMA
)
async def forget_face_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    name_ = await cg.templatable(config[CONF_NAME], args, cg.std_string)
    cg.add(var.set_name(name_))
    return var


@automation.register_action(
    "vision.clear_faces", ClearFacesAction, _GATING_ACTION_SCHEMA, synchronous=True
)
async def clear_faces_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var

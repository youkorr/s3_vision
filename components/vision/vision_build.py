"""
Build script for the ESP32-S3 vision ESPHome component.

Pulls in the ESP-DL sources for ESP32-S3 and embeds the YOLO11 model
as flash rodata. The vision_detect inner wrapper (.cpp + .hpp) lives
INSIDE the vision/ component now, so it is auto-compiled by ESPHome's
main src/ pass and we don't have to add it from a sibling directory.

Model selection priority (first match wins):
  1. YAML `model_path:` -> the user's .espdl file (passed as
     -DVISION_USER_MODEL_PATH="..." in __init__.py)
  2. components/vision/models/*.espdl (any .espdl shipped with the
     component)
  3. Sibling vision_detect/models/p4/yolo11_detect_s8_v1.espdl
     (the upstream P4 model, which is binary-compatible with S3 for
     the s8_v1 quantized variant)

ESP32-S3 ISA mapping (from the official CMakeLists.txt):
  - dl/base/isa/tie728  -> TIE728 SIMD kernels (.S files)
  - dl/base/isa/xtensa  -> Xtensa fallback kernels (.S files)
  - dl/tool/isa/tie728  -> TIE728 mem utilities (.S files)
  - dl/tool/isa/xtensa  -> Xtensa mem utilities (.S files)
  Note: there is NO dl/base/isa/esp32s3/ directory. The ESP32-S3 uses
  the tie728 and xtensa ISA directories.
"""

import os
import glob
import re
import sys

Import("env")  # noqa: F821 (provided by SCons / PlatformIO)


try:
    component_dir = os.path.dirname(os.path.abspath(__file__))
except NameError:
    component_dir = Dir('.').srcnode().abspath  # noqa: F821

parent_components_dir = os.path.dirname(component_dir)

print("[Vision S3 Build] Build script running...")
print(f"[Vision S3 Build] component_dir         = {component_dir}")
print(f"[Vision S3 Build] parent_components_dir = {parent_components_dir}")

env.Append(CPPDEFINES=[
    ("CONFIG_IDF_TARGET_ESP32S3", "1"),
])
print("[Vision S3 Build] CONFIG_IDF_TARGET_ESP32S3=1")

sources_to_add = []

# ---------------------------------------------------------------------------
# ESP-DL Sources - YOLO11 only, ESP32-S3 ISA
# ---------------------------------------------------------------------------
esp_dl_dir = os.path.join(parent_components_dir, "esp-dl")
if os.path.exists(esp_dl_dir):
    # Include dirs matching the official CMakeLists.txt for ESP32-S3:
    #   dl/base/isa/tie728, dl/base/isa/xtensa (NOT esp32s3/)
    #   dl/tool/isa/tie728, dl/tool/isa/xtensa
    esp_dl_include_dirs = [
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
        "vision/image",
        "vision/image/isa",
    ]
    for inc in esp_dl_include_dirs:
        inc_path = os.path.join(esp_dl_dir, inc)
        if os.path.exists(inc_path):
            env.Append(CPPPATH=[inc_path])
    print("[Vision S3 Build] ESP-DL include paths added")

    # C++ source directories (core library code)
    esp_dl_source_dirs = [
        "dl/tensor/src",
        "dl/model/src",
        "dl/module/src",
        "dl/tool/src",
        "dl/math/src",
        "fbs_loader/src",
        "vision/image",
        "vision/detect",
    ]
    # Determine which postprocessor(s) we need based on the selected family.
    # VISION_FAMILY: 0=coco_detect 1=pedestrian 2=hand 3=human_face_detect
    family_id = 0
    for d in env.get("CPPDEFINES", []):
        if isinstance(d, (tuple, list)) and len(d) >= 2 and d[0] == "VISION_FAMILY":
            try:
                family_id = int(str(d[1]).strip().strip('"\''))
            except (TypeError, ValueError):
                pass
        elif isinstance(d, str):
            s = d.strip().lstrip("-D")
            if s.startswith("VISION_FAMILY="):
                try:
                    family_id = int(s.split("=", 1)[1].strip().strip('"\''))
                except ValueError:
                    pass

    family_name = {
        0: "coco_detect",
        1: "pedestrian_detect",
        2: "hand_detect",
        3: "human_face_detect",
    }.get(family_id, "coco_detect")
    print(f"[Vision S3 Build] model family: id={family_id} name={family_name}")

    # Start with all the postprocessors and pose excluded, then re-include
    # the one needed by the selected family.
    esp_dl_exclude = [
        "dl_base_dotprod.cpp",
        "dl_image_jpeg.cpp",
        "dl_image_bmp.cpp",
        "dl_detect_msr_postprocessor.cpp",
        "dl_detect_mnp_postprocessor.cpp",
        "dl_pose_yolo11_postprocessor.cpp",
        "dl_detect_espdet_postprocessor.cpp",
        "dl_detect_pico_postprocessor.cpp",
    ]
    family_postprocessor = {
        0: None,                                       # yolo11 - already kept
        1: "dl_detect_pico_postprocessor.cpp",         # pedestrian
        2: "dl_detect_espdet_postprocessor.cpp",       # hand
        3: "dl_detect_msr_postprocessor.cpp",          # human_face_detect
    }.get(family_id)
    if family_postprocessor and family_postprocessor in esp_dl_exclude:
        esp_dl_exclude.remove(family_postprocessor)
        print(f"[Vision S3 Build] postprocessor enabled: {family_postprocessor}")
    # NOTE: We do NOT exclude the dl_image_pixel_cvt_dispatch_*.cpp files.
    # ImagePreprocessor::transform() needs them at runtime to convert RGB565
    # camera frames to the quantized model input. The SIMD helpers they
    # transitively reference (cvt_color_simd_helper_*) are provided as scalar
    # stubs in dl_image_color_isa_stubs.cpp that ships in this component.
    esp_dl_exclude_prefixes = ()

    counts = {"base": 0, "isa_S": 0, "isa_cpp": 0, "core": 0, "vision": 0}
    for src_dir in esp_dl_source_dirs:
        src_dir_path = os.path.join(esp_dl_dir, src_dir)
        if not os.path.exists(src_dir_path):
            continue
        if src_dir.startswith("vision/"):
            for src_file in glob.glob(
                os.path.join(src_dir_path, "**", "*.cpp"), recursive=True
            ):
                bn = os.path.basename(src_file)
                if bn in esp_dl_exclude:
                    continue
                if bn.startswith(esp_dl_exclude_prefixes):
                    continue
                sources_to_add.append(src_file)
                counts["vision"] += 1
        else:
            for src_file in glob.glob(os.path.join(src_dir_path, "*.cpp")):
                bn = os.path.basename(src_file)
                if bn in esp_dl_exclude:
                    continue
                if bn.startswith(esp_dl_exclude_prefixes):
                    continue
                sources_to_add.append(src_file)
                counts["core"] += 1

    # dl/base/*.cpp (Conv, Pool, etc.)
    dl_base_dir = os.path.join(esp_dl_dir, "dl", "base")
    for src_file in glob.glob(os.path.join(dl_base_dir, "*.cpp")):
        if os.path.basename(src_file) in esp_dl_exclude:
            continue
        sources_to_add.append(src_file)
        counts["base"] += 1

    # -----------------------------------------------------------------------
    # ESP32-S3 ISA assembly files (.S) and any .cpp in ISA dirs
    # These are the actual directories that exist in the repository,
    # matching the official CMakeLists.txt for CONFIG_IDF_TARGET_ESP32S3:
    #   dl/tool/isa/xtensa  -> dl_xtensa_bzero.S
    #   dl/tool/isa/tie728  -> dl_tie728_bzero.S, dl_tie728_memcpy.S, dl_tie728_memset.S
    #   dl/base/isa/xtensa  -> dl_xtensa_s16_block.S
    #   dl/base/isa/tie728  -> 48 assembly files (convolution, pooling, etc.)
    # -----------------------------------------------------------------------
    s3_isa_dirs = [
        # Tool ISA (memory operations)
        ("dl/tool/isa/xtensa", "*.S"),
        ("dl/tool/isa/xtensa", "*.cpp"),
        ("dl/tool/isa/tie728", "*.S"),
        ("dl/tool/isa/tie728", "*.cpp"),
        # Base ISA (neural network kernels)
        ("dl/base/isa/xtensa", "*.S"),
        ("dl/base/isa/xtensa", "*.cpp"),
        ("dl/base/isa/tie728", "*.S"),
        ("dl/base/isa/tie728", "*.cpp"),
    ]
    for sub, pattern in s3_isa_dirs:
        path = os.path.join(esp_dl_dir, sub)
        if not os.path.exists(path):
            print(f"[Vision S3 Build] WARN: ISA dir not found: {sub}")
            continue
        matched = glob.glob(os.path.join(path, pattern))
        for f in matched:
            sources_to_add.append(f)
            if pattern == "*.S":
                counts["isa_S"] += 1
            else:
                counts["isa_cpp"] += 1

    print(f"[Vision S3 Build] ESP-DL: base:{counts['base']} "
          f"isa_asm:{counts['isa_S']} isa_cpp:{counts['isa_cpp']} "
          f"core:{counts['core']} vision:{counts['vision']}")

    # -----------------------------------------------------------------------
    # Link pre-compiled libfbs_model.a for ESP32-S3
    # -----------------------------------------------------------------------
    fbs_lib_dir = os.path.join(esp_dl_dir, "fbs_loader", "lib", "esp32s3")
    fbs_lib = os.path.join(fbs_lib_dir, "libfbs_model.a")
    if os.path.exists(fbs_lib):
        env.Append(LIBPATH=[fbs_lib_dir])
        env.Prepend(LIBS=["fbs_model"])
        print("[Vision S3 Build] Linked libfbs_model.a (S3)")
    else:
        print(f"[Vision S3 Build] WARNING: libfbs_model.a not found at {fbs_lib}")
        print("[Vision S3 Build] FbsModel symbols will be unresolved!")


# ---------------------------------------------------------------------------
# Helpers for picking up CPP defines (CPPDEFINES + BUILD_FLAGS)
# ---------------------------------------------------------------------------
def has_define(name):
    for d in env.get("CPPDEFINES", []):
        if isinstance(d, (tuple, list)) and d and d[0] == name:
            return True
        if isinstance(d, str) and d.split("=", 1)[0] == name:
            return True
    return False


def get_define_string(name):
    """Return the value of a `-DNAME="value"` macro as a python str, or None."""
    for d in env.get("CPPDEFINES", []):
        if isinstance(d, (tuple, list)) and len(d) >= 2 and d[0] == name:
            v = str(d[1])
            return v.strip('"\\\'')
    for flag in env.get("BUILD_FLAGS", []):
        s = str(flag).strip()
        if s.startswith("-D"):
            s = s[2:]
        if "=" in s:
            k, v = s.split("=", 1)
            if k == name:
                return v.strip('"\\\'')
    return None


# ---------------------------------------------------------------------------
# Embed model in flash rodata
# ---------------------------------------------------------------------------
model_from_file = has_define("VISION_MODEL_FROM_FILE")
if not model_from_file:
    user_model = get_define_string("VISION_USER_MODEL_PATH")
    candidates = []

    # 1. Explicit YAML model_path: takes precedence
    if user_model:
        if os.path.isfile(user_model):
            candidates.append(user_model)
            print(f"[Vision S3 Build] Using user-provided model: {user_model}")
        else:
            print(f"[Vision S3 Build] WARNING: model_path file not found: {user_model}")

    # 2. Anything dropped in components/vision/models/
    if not candidates:
        own_dir = os.path.join(component_dir, "models")
        if os.path.isdir(own_dir):
            candidates += sorted(glob.glob(os.path.join(own_dir, "*.espdl")))

    # 3. Family-specific default model. Each model family has its own
    #    bundled .espdl in components/models/<family>/models/s3/.
    if not candidates:
        family_defaults = {
            "coco_detect": "coco_detect_yolo11n_s8_v1.espdl",
            "pedestrian_detect": "pedestrian_detect_pico_s8_v1.espdl",
            "hand_detect": "espdet_pico_224_224_hand.espdl",
            "human_face_detect": "human_face_detect_msr_s8_v1.espdl",
        }
        family_default_name = family_defaults.get(family_name)
        if family_default_name:
            fallback = os.path.join(parent_components_dir, "models", family_name,
                                    "models", "s3", family_default_name)
            if os.path.exists(fallback):
                candidates.append(fallback)
                print(f"[Vision S3 Build] using family default: {fallback}")
            else:
                print(f"[Vision S3 Build] WARN: family default not found: {fallback}")

    if not candidates:
        sys.exit("[Vision S3 Build] FATAL: no .espdl model found for embedding.\n"
                 "  Either set `model_path:` in your YAML or drop a .espdl in\n"
                 "  components/vision/models/.")

    model_path = candidates[0]
    embed_c = os.path.join(component_dir, "vision_model_embed.c")

    print(f"[Vision S3 Build] Embedding model: {model_path}")
    with open(model_path, "rb") as f:
        data = f.read()
    lines = [
        "// Auto-generated Vision model blob",
        f"// Source: {os.path.basename(model_path)}",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "",
        "__attribute__((aligned(16)))",
        "const uint8_t _binary_vision_detect_espdl_start[] = {",
    ]
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    lines += [
        "};",
        "",
        f"const uint8_t *const _binary_vision_detect_espdl_end = "
        f"_binary_vision_detect_espdl_start + {len(data)};",
        f"const size_t _binary_vision_detect_espdl_size = {len(data)};",
        "",
    ]
    with open(embed_c, "w") as f:
        f.write("\n".join(lines))
    # Add the generated .c to the sources list so it gets compiled into
    # libvision_s3.a.  ESPHome's main src/ pass does NOT auto-compile
    # .c files that are generated by post: extra_scripts.
    sources_to_add.append(embed_c)
    print(f"[Vision S3 Build] Embedded {len(data)} bytes -> "
          f"{os.path.basename(embed_c)} (added to libvision_s3.a)")
else:
    print("[Vision S3 Build] VISION_MODEL_FROM_FILE set - skipping flash embed")

# ---------------------------------------------------------------------------
# Local stub helpers
# ---------------------------------------------------------------------------
def _add_or_fallback(local_name, fallback_dir):
    local = os.path.join(component_dir, local_name)
    if os.path.exists(local):
        sources_to_add.append(local)
        print(f"[Vision S3 Build] + {local_name} (local)")
        return
    fb = os.path.join(parent_components_dir, fallback_dir, local_name)
    if os.path.exists(fb):
        sources_to_add.append(fb)
        print(f"[Vision S3 Build] + {local_name} (from {fallback_dir}/)")

_add_or_fallback("dl_base_dotprod_no_dsp.cpp", "vision_detection")
_add_or_fallback("mbedtls_aes_stub.c", "face_detection")

env.Append(CPPPATH=[component_dir])

# ---------------------------------------------------------------------------
# Compile + link as static library
# ---------------------------------------------------------------------------
if sources_to_add:
    objects = []
    for src in sources_to_add:
        try:
            objects.extend(env.Object(src))
        except Exception as e:
            print(f"[Vision S3 Build] Failed to compile {os.path.basename(src)}: {e}")

    if objects:
        lib = env.StaticLibrary(
            os.path.join("$BUILD_DIR", "libvision_s3"),
            objects,
        )
        env.Prepend(LIBS=[lib])
        env['_LIBFLAGS'] = '-Wl,--start-group ' + env['_LIBFLAGS'] + ' -Wl,--end-group'
        print(f"[Vision S3 Build] {len(sources_to_add)} sources -> libvision_s3.a")

print("[Vision S3 Build] Build script completed")

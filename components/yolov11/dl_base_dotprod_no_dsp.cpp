// Pure-C scalar replacement for dl/base/dl_base_dotprod.cpp.
//
// The upstream file uses dsps_dotprod_f32 from esp-dsp and the TIE728/ESP32P4
// SIMD intrinsics. Linking esp-dsp into an ESPHome PlatformIO build is messy,
// so we provide scalar fallbacks for the 4 dotprod() overloads referenced by
// the templated dl::base::mat_vec_dotprod<...> in dl_model_base.o.
//
// Quantized inference still uses the tie728 .S kernels for the *outer* convs
// (dl_tie728_*); these dotprod() helpers are only called from a small number
// of fallback paths inside dl_model_base, so going scalar here doesn't hurt
// real-world inference throughput on the ESP32-S3.

#include <cstdint>
#include "dl_tool.hpp"

namespace dl {
namespace base {

void dotprod(int8_t *input0_ptr, int8_t *input1_ptr, int16_t *output_ptr, int length, int shift) {
    int32_t result = 0;
    float scale = DL_RESCALE(shift);
    for (int i = 0; i < length; i++) {
        result += static_cast<int32_t>(input0_ptr[i]) * static_cast<int32_t>(input1_ptr[i]);
    }
    dl::tool::truncate(*output_ptr, dl::tool::round(result * scale));
}

void dotprod(int8_t *input0_ptr, int16_t *input1_ptr, int16_t *output_ptr, int length, int shift) {
    int32_t result = 0;
    float scale = DL_RESCALE(shift);
    for (int i = 0; i < length; i++) {
        result += static_cast<int32_t>(input0_ptr[i]) * static_cast<int32_t>(input1_ptr[i]);
    }
    dl::tool::truncate(*output_ptr, dl::tool::round(result * scale));
}

void dotprod(int16_t *input0_ptr, int16_t *input1_ptr, int16_t *output_ptr, int length, int shift) {
    int64_t result = 0;
    float scale = DL_RESCALE(shift);
    for (int i = 0; i < length; i++) {
        result += static_cast<int64_t>(input0_ptr[i]) * static_cast<int64_t>(input1_ptr[i]);
    }
    dl::tool::truncate(*output_ptr, dl::tool::round(result * scale));
}

void dotprod(float *input0_ptr, float *input1_ptr, float *output_ptr, int length, int /*shift*/) {
    float result = 0.0f;
    for (int i = 0; i < length; i++) {
        result += input0_ptr[i] * input1_ptr[i];
    }
    *output_ptr = result;
}

}  // namespace base
}  // namespace dl

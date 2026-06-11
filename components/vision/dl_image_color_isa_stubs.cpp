// SPDX-FileCopyrightText: 2026 youkorr
// SPDX-License-Identifier: GPL-3.0-or-later
// dl_image_color_isa_stubs.cpp
//
// Scalar fallback implementations of the cvt_color_simd_helper_* and
// resize_nn_simd_helper_* functions declared in
// vision/image/isa/dl_image_color_isa.hpp.
//
// The upstream esp-dl library only ships SIMD versions of these for
// ESP32-P4 (vision/image/isa/esp32p4/*.S). On ESP32-S3 we provide these
// scalar fallbacks so ImagePreprocessor::transform() can still convert
// camera frames (RGB565) into the quantized model input (RGB888 qint8/16).
//
// All functions are __attribute__((weak)) - if a real implementation is
// linked in (e.g. on a P4 target via the .S files), it overrides ours.

#include <cstdint>
#include <cstring>
#include <cstdlib>

namespace {

inline void unpack_rgb565_to_rgb888(uint16_t pix, uint8_t *r, uint8_t *g, uint8_t *b) {
  // Match esp-dl extract_channel*_from_rgb565{le,be}: top-aligned 8-bit
  // values (low bits zero). The reverse-LSB replication is not done; this
  // matches what esp-dl's scalar operator() produces.
  *r = static_cast<uint8_t>((pix & 0xF800) >> 8);
  *g = static_cast<uint8_t>((pix & 0x07E0) >> 3);
  *b = static_cast<uint8_t>((pix & 0x001F) << 3);
}

inline uint16_t read565_be(const uint8_t *p) {
  return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

inline uint16_t read565_le(const uint8_t *p) {
  return static_cast<uint16_t>((static_cast<uint16_t>(p[1]) << 8) | p[0]);
}

inline void write565_be(uint8_t *p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v >> 8);
  p[1] = static_cast<uint8_t>(v & 0xFF);
}

inline void write565_le(uint8_t *p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>(v >> 8);
}

inline uint16_t pack_rgb888_to_rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

inline uint8_t rgb_to_gray_y(uint8_t r, uint8_t g, uint8_t b) {
  // BT.601 luminance, fixed-point: 0.299*R + 0.587*G + 0.114*B
  return static_cast<uint8_t>(((static_cast<uint32_t>(r) * 77 +
                                static_cast<uint32_t>(g) * 150 +
                                static_cast<uint32_t>(b) * 29)) >> 8);
}

template<bool BE, bool DstBE>
inline void rgb565_to_bgr565(uint8_t *src, uint8_t *dst, int n) {
  for (int i = 0; i < n; i++) {
    uint16_t pix = BE ? read565_be(src) : read565_le(src);
    uint16_t r5 = (pix >> 11) & 0x1F;
    uint16_t g6 = (pix >> 5) & 0x3F;
    uint16_t b5 = pix & 0x1F;
    uint16_t out = static_cast<uint16_t>((b5 << 11) | (g6 << 5) | r5);
    if constexpr (DstBE) write565_be(dst, out);
    else write565_le(dst, out);
    src += 2;
    dst += 2;
  }
}

template<bool BE, bool Swap>
inline void rgb565_to_888(uint8_t *src, uint8_t *dst, int n) {
  for (int i = 0; i < n; i++) {
    uint16_t pix = BE ? read565_be(src) : read565_le(src);
    uint8_t r, g, b;
    unpack_rgb565_to_rgb888(pix, &r, &g, &b);
    if constexpr (Swap) { dst[0] = b; dst[1] = g; dst[2] = r; }
    else                { dst[0] = r; dst[1] = g; dst[2] = b; }
    src += 2;
    dst += 3;
  }
}

template<bool BE, bool BgrSrc>
inline void rgb565_to_gray_one(uint8_t *src, uint8_t *dst, int n) {
  for (int i = 0; i < n; i++) {
    uint16_t pix = BE ? read565_be(src) : read565_le(src);
    uint8_t r, g, b;
    unpack_rgb565_to_rgb888(pix, &r, &g, &b);
    if constexpr (BgrSrc) { uint8_t t = r; r = b; b = t; }
    dst[0] = rgb_to_gray_y(r, g, b);
    src += 2;
    dst += 1;
  }
}

template<bool BE, bool Swap, typename Q>
inline void rgb565_to_888_q3(uint8_t *src, uint8_t *dst, int n, int *lut) {
  Q *out = reinterpret_cast<Q *>(dst);
  for (int i = 0; i < n; i++) {
    uint16_t pix = BE ? read565_be(src) : read565_le(src);
    uint8_t r, g, b;
    unpack_rgb565_to_rgb888(pix, &r, &g, &b);
    if constexpr (Swap) {
      out[0] = static_cast<Q>(lut[0 * 256 + b]);
      out[1] = static_cast<Q>(lut[1 * 256 + g]);
      out[2] = static_cast<Q>(lut[2 * 256 + r]);
    } else {
      out[0] = static_cast<Q>(lut[0 * 256 + r]);
      out[1] = static_cast<Q>(lut[1 * 256 + g]);
      out[2] = static_cast<Q>(lut[2 * 256 + b]);
    }
    src += 2;
    out += 3;
  }
}

template<bool BE, bool BgrSrc, typename Q>
inline void rgb565_to_gray_q1(uint8_t *src, uint8_t *dst, int n, int *lut) {
  Q *out = reinterpret_cast<Q *>(dst);
  for (int i = 0; i < n; i++) {
    uint16_t pix = BE ? read565_be(src) : read565_le(src);
    uint8_t r, g, b;
    unpack_rgb565_to_rgb888(pix, &r, &g, &b);
    if constexpr (BgrSrc) { uint8_t t = r; r = b; b = t; }
    out[0] = static_cast<Q>(lut[rgb_to_gray_y(r, g, b)]);
    src += 2;
    out += 1;
  }
}

template<bool Swap, typename Q>
inline void rgb888_to_888_q3(uint8_t *src, uint8_t *dst, int n, int *lut) {
  Q *out = reinterpret_cast<Q *>(dst);
  for (int i = 0; i < n; i++) {
    uint8_t r = src[0], g = src[1], b = src[2];
    if constexpr (Swap) {
      out[0] = static_cast<Q>(lut[0 * 256 + b]);
      out[1] = static_cast<Q>(lut[1 * 256 + g]);
      out[2] = static_cast<Q>(lut[2 * 256 + r]);
    } else {
      out[0] = static_cast<Q>(lut[0 * 256 + r]);
      out[1] = static_cast<Q>(lut[1 * 256 + g]);
      out[2] = static_cast<Q>(lut[2 * 256 + b]);
    }
    src += 3;
    out += 3;
  }
}

template<bool BgrSrc, typename Q>
inline void rgb888_to_gray_q1(uint8_t *src, uint8_t *dst, int n, int *lut) {
  Q *out = reinterpret_cast<Q *>(dst);
  for (int i = 0; i < n; i++) {
    uint8_t r = src[0], g = src[1], b = src[2];
    if constexpr (BgrSrc) { uint8_t t = r; r = b; b = t; }
    out[0] = static_cast<Q>(lut[rgb_to_gray_y(r, g, b)]);
    src += 3;
    out += 1;
  }
}

template<bool Swap>
inline void rgb888_to_565(uint8_t *src, uint8_t *dst, int n, bool dst_be) {
  for (int i = 0; i < n; i++) {
    uint8_t r = Swap ? src[2] : src[0];
    uint8_t g = src[1];
    uint8_t b = Swap ? src[0] : src[2];
    uint16_t pix = pack_rgb888_to_rgb565(r, g, b);
    if (dst_be) write565_be(dst, pix);
    else write565_le(dst, pix);
    src += 3;
    dst += 2;
  }
}

}  // namespace

extern "C" {

// ===== RGB565 -> RGB565 (passthrough or swap) =====
__attribute__((weak)) void cvt_color_simd_helper_rgb5652rgb565(uint8_t *src, uint8_t *dst, int n) {
  std::memcpy(dst, src, static_cast<size_t>(n) * 2);
}
__attribute__((weak)) void cvt_color_simd_helper_rgb565le2rgb565be(uint8_t *src, uint8_t *dst, int n) {
  for (int i = 0; i < n; i++) { dst[0] = src[1]; dst[1] = src[0]; src += 2; dst += 2; }
}
__attribute__((weak)) void cvt_color_simd_helper_rgb565le2bgr565le(uint8_t *src, uint8_t *dst, int n) {
  rgb565_to_bgr565<false, false>(src, dst, n);
}
__attribute__((weak)) void cvt_color_simd_helper_rgb565be2bgr565be(uint8_t *src, uint8_t *dst, int n) {
  rgb565_to_bgr565<true, true>(src, dst, n);
}
__attribute__((weak)) void cvt_color_simd_helper_rgb565le2bgr565be(uint8_t *src, uint8_t *dst, int n) {
  rgb565_to_bgr565<false, true>(src, dst, n);
}
__attribute__((weak)) void cvt_color_simd_helper_rgb565be2bgr565le(uint8_t *src, uint8_t *dst, int n) {
  rgb565_to_bgr565<true, false>(src, dst, n);
}

// ===== RGB565 -> RGB888 / BGR888 =====
__attribute__((weak)) void cvt_color_simd_helper_rgb565le2rgb888(uint8_t *src, uint8_t *dst, int n) {
  rgb565_to_888<false, false>(src, dst, n);
}
__attribute__((weak)) void cvt_color_simd_helper_rgb565be2rgb888(uint8_t *src, uint8_t *dst, int n) {
  rgb565_to_888<true, false>(src, dst, n);
}
__attribute__((weak)) void cvt_color_simd_helper_rgb565le2bgr888(uint8_t *src, uint8_t *dst, int n) {
  rgb565_to_888<false, true>(src, dst, n);
}
__attribute__((weak)) void cvt_color_simd_helper_rgb565be2bgr888(uint8_t *src, uint8_t *dst, int n) {
  rgb565_to_888<true, true>(src, dst, n);
}

// ===== RGB565 -> grayscale =====
__attribute__((weak)) void cvt_color_simd_helper_rgb565le2gray(uint8_t *src, uint8_t *dst, int n) {
  rgb565_to_gray_one<false, false>(src, dst, n);
}
__attribute__((weak)) void cvt_color_simd_helper_rgb565be2gray(uint8_t *src, uint8_t *dst, int n) {
  rgb565_to_gray_one<true, false>(src, dst, n);
}
__attribute__((weak)) void cvt_color_simd_helper_bgr565le2gray(uint8_t *src, uint8_t *dst, int n) {
  rgb565_to_gray_one<false, true>(src, dst, n);
}
__attribute__((weak)) void cvt_color_simd_helper_bgr565be2gray(uint8_t *src, uint8_t *dst, int n) {
  rgb565_to_gray_one<true, true>(src, dst, n);
}

// ===== RGB888 -> RGB888 / BGR888 / RGB565 / Gray =====
__attribute__((weak)) void cvt_color_simd_helper_rgb8882rgb888(uint8_t *src, uint8_t *dst, int n) {
  std::memcpy(dst, src, static_cast<size_t>(n) * 3);
}
__attribute__((weak)) void cvt_color_simd_helper_rgb8882bgr888(uint8_t *src, uint8_t *dst, int n) {
  for (int i = 0; i < n; i++) { dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; src += 3; dst += 3; }
}
__attribute__((weak)) void cvt_color_simd_helper_rgb8882rgb565le(uint8_t *src, uint8_t *dst, int n) {
  rgb888_to_565<false>(src, dst, n, false);
}
__attribute__((weak)) void cvt_color_simd_helper_bgr8882rgb565le(uint8_t *src, uint8_t *dst, int n) {
  rgb888_to_565<true>(src, dst, n, false);
}
__attribute__((weak)) void cvt_color_simd_helper_rgb8882rgb565be(uint8_t *src, uint8_t *dst, int n) {
  rgb888_to_565<false>(src, dst, n, true);
}
__attribute__((weak)) void cvt_color_simd_helper_bgr8882rgb565be(uint8_t *src, uint8_t *dst, int n) {
  rgb888_to_565<true>(src, dst, n, true);
}
__attribute__((weak)) void cvt_color_simd_helper_rgb8882gray(uint8_t *src, uint8_t *dst, int n) {
  for (int i = 0; i < n; i++) { dst[0] = rgb_to_gray_y(src[0], src[1], src[2]); src += 3; dst += 1; }
}
__attribute__((weak)) void cvt_color_simd_helper_bgr8882gray(uint8_t *src, uint8_t *dst, int n) {
  for (int i = 0; i < n; i++) { dst[0] = rgb_to_gray_y(src[2], src[1], src[0]); src += 3; dst += 1; }
}
__attribute__((weak)) void cvt_color_simd_helper_gray2gray(uint8_t *src, uint8_t *dst, int n) {
  std::memcpy(dst, src, static_cast<size_t>(n));
}

// ===== Quantized variants (qint8/qint16) - the path actually used by YOLO =====
__attribute__((weak)) void cvt_color_simd_helper_rgb565le2rgb888_qint8(uint8_t *s, uint8_t *d, int n, int *lut) { rgb565_to_888_q3<false, false, int8_t>(s, d, n, lut); }
__attribute__((weak)) void cvt_color_simd_helper_rgb565be2rgb888_qint8(uint8_t *s, uint8_t *d, int n, int *lut) { rgb565_to_888_q3<true,  false, int8_t>(s, d, n, lut); }
__attribute__((weak)) void cvt_color_simd_helper_rgb565le2bgr888_qint8(uint8_t *s, uint8_t *d, int n, int *lut) { rgb565_to_888_q3<false, true,  int8_t>(s, d, n, lut); }
__attribute__((weak)) void cvt_color_simd_helper_rgb565be2bgr888_qint8(uint8_t *s, uint8_t *d, int n, int *lut) { rgb565_to_888_q3<true,  true,  int8_t>(s, d, n, lut); }
__attribute__((weak)) void cvt_color_simd_helper_rgb565le2rgb888_qint16(uint8_t *s, uint8_t *d, int n, int *lut) { rgb565_to_888_q3<false, false, int16_t>(s, d, n, lut); }
__attribute__((weak)) void cvt_color_simd_helper_rgb565be2rgb888_qint16(uint8_t *s, uint8_t *d, int n, int *lut) { rgb565_to_888_q3<true,  false, int16_t>(s, d, n, lut); }
__attribute__((weak)) void cvt_color_simd_helper_rgb565le2bgr888_qint16(uint8_t *s, uint8_t *d, int n, int *lut) { rgb565_to_888_q3<false, true,  int16_t>(s, d, n, lut); }
__attribute__((weak)) void cvt_color_simd_helper_rgb565be2bgr888_qint16(uint8_t *s, uint8_t *d, int n, int *lut) { rgb565_to_888_q3<true,  true,  int16_t>(s, d, n, lut); }

__attribute__((weak)) void cvt_color_simd_helper_rgb565le2gray_qint8(uint8_t *s, uint8_t *d, int n, int *lut) { rgb565_to_gray_q1<false, false, int8_t>(s, d, n, lut); }
__attribute__((weak)) void cvt_color_simd_helper_rgb565be2gray_qint8(uint8_t *s, uint8_t *d, int n, int *lut) { rgb565_to_gray_q1<true,  false, int8_t>(s, d, n, lut); }
__attribute__((weak)) void cvt_color_simd_helper_bgr565le2gray_qint8(uint8_t *s, uint8_t *d, int n, int *lut) { rgb565_to_gray_q1<false, true,  int8_t>(s, d, n, lut); }
__attribute__((weak)) void cvt_color_simd_helper_bgr565be2gray_qint8(uint8_t *s, uint8_t *d, int n, int *lut) { rgb565_to_gray_q1<true,  true,  int8_t>(s, d, n, lut); }
__attribute__((weak)) void cvt_color_simd_helper_rgb565le2gray_qint16(uint8_t *s, uint8_t *d, int n, int *lut) { rgb565_to_gray_q1<false, false, int16_t>(s, d, n, lut); }
__attribute__((weak)) void cvt_color_simd_helper_rgb565be2gray_qint16(uint8_t *s, uint8_t *d, int n, int *lut) { rgb565_to_gray_q1<true,  false, int16_t>(s, d, n, lut); }
__attribute__((weak)) void cvt_color_simd_helper_bgr565le2gray_qint16(uint8_t *s, uint8_t *d, int n, int *lut) { rgb565_to_gray_q1<false, true,  int16_t>(s, d, n, lut); }
__attribute__((weak)) void cvt_color_simd_helper_bgr565be2gray_qint16(uint8_t *s, uint8_t *d, int n, int *lut) { rgb565_to_gray_q1<true,  true,  int16_t>(s, d, n, lut); }

__attribute__((weak)) void cvt_color_simd_helper_rgb8882rgb888_qint8(uint8_t *s, uint8_t *d, int n, int *lut)  { rgb888_to_888_q3<false, int8_t>(s, d, n, lut); }
__attribute__((weak)) void cvt_color_simd_helper_rgb8882bgr888_qint8(uint8_t *s, uint8_t *d, int n, int *lut)  { rgb888_to_888_q3<true,  int8_t>(s, d, n, lut); }
__attribute__((weak)) void cvt_color_simd_helper_rgb8882rgb888_qint16(uint8_t *s, uint8_t *d, int n, int *lut) { rgb888_to_888_q3<false, int16_t>(s, d, n, lut); }
__attribute__((weak)) void cvt_color_simd_helper_rgb8882bgr888_qint16(uint8_t *s, uint8_t *d, int n, int *lut) { rgb888_to_888_q3<true,  int16_t>(s, d, n, lut); }
__attribute__((weak)) void cvt_color_simd_helper_rgb8882gray_qint8(uint8_t *s, uint8_t *d, int n, int *lut)   { rgb888_to_gray_q1<false, int8_t>(s, d, n, lut); }
__attribute__((weak)) void cvt_color_simd_helper_bgr8882gray_qint8(uint8_t *s, uint8_t *d, int n, int *lut)   { rgb888_to_gray_q1<true,  int8_t>(s, d, n, lut); }
__attribute__((weak)) void cvt_color_simd_helper_rgb8882gray_qint16(uint8_t *s, uint8_t *d, int n, int *lut)  { rgb888_to_gray_q1<false, int16_t>(s, d, n, lut); }
__attribute__((weak)) void cvt_color_simd_helper_bgr8882gray_qint16(uint8_t *s, uint8_t *d, int n, int *lut)  { rgb888_to_gray_q1<true,  int16_t>(s, d, n, lut); }

// ===== HSV / YUV helpers - YOLO11 path doesn't use these. Keep weak stubs
//      so the link succeeds; they zero the output if ever called. =====
__attribute__((weak)) void cvt_color_simd_helper_rgb8882hsv(uint8_t *, uint8_t *dst, int n, int *, int *) { std::memset(dst, 0, static_cast<size_t>(n) * 3); }
__attribute__((weak)) void cvt_color_simd_helper_bgr8882hsv(uint8_t *, uint8_t *dst, int n, int *, int *) { std::memset(dst, 0, static_cast<size_t>(n) * 3); }
__attribute__((weak)) void cvt_color_simd_helper_rgb565le2hsv(uint8_t *, uint8_t *dst, int n, int *, int *) { std::memset(dst, 0, static_cast<size_t>(n) * 3); }
__attribute__((weak)) void cvt_color_simd_helper_rgb565be2hsv(uint8_t *, uint8_t *dst, int n, int *, int *) { std::memset(dst, 0, static_cast<size_t>(n) * 3); }
__attribute__((weak)) void cvt_color_simd_helper_bgr565le2hsv(uint8_t *, uint8_t *dst, int n, int *, int *) { std::memset(dst, 0, static_cast<size_t>(n) * 3); }
__attribute__((weak)) void cvt_color_simd_helper_bgr565be2hsv(uint8_t *, uint8_t *dst, int n, int *, int *) { std::memset(dst, 0, static_cast<size_t>(n) * 3); }

// ===== Resize-NN (nearest neighbor) helpers =====
// Signature: (uint8_t **src, int *offsets, uint8_t *dst, int n).
// `*src` points at a row in the source image; `offsets[i]` is the byte
// offset from `*src` to the i-th sampled source pixel. We write `n` dest
// pixels in the appropriate format. *src is left unmodified.
}  // extern "C"  (templates can't live inside C linkage)

namespace {
template<bool BE, bool Swap>
inline void resize_nn_565_to_888(uint8_t **src, int *offsets, uint8_t *dst, int n) {
  uint8_t *s = *src;
  for (int i = 0; i < n; i++) {
    uint8_t *p = s + offsets[i];
    uint16_t pix = BE ? read565_be(p) : read565_le(p);
    uint8_t r, g, b;
    unpack_rgb565_to_rgb888(pix, &r, &g, &b);
    if constexpr (Swap) { dst[0] = b; dst[1] = g; dst[2] = r; }
    else                { dst[0] = r; dst[1] = g; dst[2] = b; }
    dst += 3;
  }
}

template<bool BE, bool BgrSrc>
inline void resize_nn_565_to_gray(uint8_t **src, int *offsets, uint8_t *dst, int n) {
  uint8_t *s = *src;
  for (int i = 0; i < n; i++) {
    uint8_t *p = s + offsets[i];
    uint16_t pix = BE ? read565_be(p) : read565_le(p);
    uint8_t r, g, b;
    unpack_rgb565_to_rgb888(pix, &r, &g, &b);
    if constexpr (BgrSrc) { uint8_t t = r; r = b; b = t; }
    dst[0] = rgb_to_gray_y(r, g, b);
    dst += 1;
  }
}
}  // namespace

extern "C" {

__attribute__((weak)) void resize_nn_simd_helper_rgb5652rgb565(uint8_t **src, int *offsets, uint8_t *dst, int n) {
  uint8_t *s = *src;
  for (int i = 0; i < n; i++) { uint8_t *p = s + offsets[i]; dst[0] = p[0]; dst[1] = p[1]; dst += 2; }
}
__attribute__((weak)) void resize_nn_simd_helper_rgb565le2rgb565be(uint8_t **src, int *offsets, uint8_t *dst, int n) {
  uint8_t *s = *src;
  for (int i = 0; i < n; i++) { uint8_t *p = s + offsets[i]; dst[0] = p[1]; dst[1] = p[0]; dst += 2; }
}
__attribute__((weak)) void resize_nn_simd_helper_rgb565le2bgr565le(uint8_t **src, int *offsets, uint8_t *dst, int n) {
  uint8_t *s = *src;
  for (int i = 0; i < n; i++) {
    uint8_t *p = s + offsets[i];
    uint16_t pix = read565_le(p);
    uint16_t r5 = (pix >> 11) & 0x1F, g6 = (pix >> 5) & 0x3F, b5 = pix & 0x1F;
    write565_le(dst, static_cast<uint16_t>((b5 << 11) | (g6 << 5) | r5));
    dst += 2;
  }
}
__attribute__((weak)) void resize_nn_simd_helper_rgb565be2bgr565be(uint8_t **src, int *offsets, uint8_t *dst, int n) {
  uint8_t *s = *src;
  for (int i = 0; i < n; i++) {
    uint8_t *p = s + offsets[i];
    uint16_t pix = read565_be(p);
    uint16_t r5 = (pix >> 11) & 0x1F, g6 = (pix >> 5) & 0x3F, b5 = pix & 0x1F;
    write565_be(dst, static_cast<uint16_t>((b5 << 11) | (g6 << 5) | r5));
    dst += 2;
  }
}
__attribute__((weak)) void resize_nn_simd_helper_rgb565le2bgr565be(uint8_t **src, int *offsets, uint8_t *dst, int n) {
  uint8_t *s = *src;
  for (int i = 0; i < n; i++) {
    uint8_t *p = s + offsets[i];
    uint16_t pix = read565_le(p);
    uint16_t r5 = (pix >> 11) & 0x1F, g6 = (pix >> 5) & 0x3F, b5 = pix & 0x1F;
    write565_be(dst, static_cast<uint16_t>((b5 << 11) | (g6 << 5) | r5));
    dst += 2;
  }
}
__attribute__((weak)) void resize_nn_simd_helper_rgb565be2bgr565le(uint8_t **src, int *offsets, uint8_t *dst, int n) {
  uint8_t *s = *src;
  for (int i = 0; i < n; i++) {
    uint8_t *p = s + offsets[i];
    uint16_t pix = read565_be(p);
    uint16_t r5 = (pix >> 11) & 0x1F, g6 = (pix >> 5) & 0x3F, b5 = pix & 0x1F;
    write565_le(dst, static_cast<uint16_t>((b5 << 11) | (g6 << 5) | r5));
    dst += 2;
  }
}

__attribute__((weak)) void resize_nn_simd_helper_rgb565le2rgb888(uint8_t **src, int *offsets, uint8_t *dst, int n) { resize_nn_565_to_888<false, false>(src, offsets, dst, n); }
__attribute__((weak)) void resize_nn_simd_helper_rgb565be2rgb888(uint8_t **src, int *offsets, uint8_t *dst, int n) { resize_nn_565_to_888<true,  false>(src, offsets, dst, n); }
__attribute__((weak)) void resize_nn_simd_helper_rgb565le2bgr888(uint8_t **src, int *offsets, uint8_t *dst, int n) { resize_nn_565_to_888<false, true>(src, offsets, dst, n); }
__attribute__((weak)) void resize_nn_simd_helper_rgb565be2bgr888(uint8_t **src, int *offsets, uint8_t *dst, int n) { resize_nn_565_to_888<true,  true>(src, offsets, dst, n); }

__attribute__((weak)) void resize_nn_simd_helper_rgb565le2gray(uint8_t **src, int *offsets, uint8_t *dst, int n) { resize_nn_565_to_gray<false, false>(src, offsets, dst, n); }
__attribute__((weak)) void resize_nn_simd_helper_rgb565be2gray(uint8_t **src, int *offsets, uint8_t *dst, int n) { resize_nn_565_to_gray<true,  false>(src, offsets, dst, n); }
__attribute__((weak)) void resize_nn_simd_helper_bgr565le2gray(uint8_t **src, int *offsets, uint8_t *dst, int n) { resize_nn_565_to_gray<false, true>(src, offsets, dst, n); }
__attribute__((weak)) void resize_nn_simd_helper_bgr565be2gray(uint8_t **src, int *offsets, uint8_t *dst, int n) { resize_nn_565_to_gray<true,  true>(src, offsets, dst, n); }

__attribute__((weak)) void resize_nn_simd_helper_rgb8882rgb888(uint8_t **src, int *offsets, uint8_t *dst, int n) {
  uint8_t *s = *src;
  for (int i = 0; i < n; i++) { uint8_t *p = s + offsets[i]; dst[0] = p[0]; dst[1] = p[1]; dst[2] = p[2]; dst += 3; }
}
__attribute__((weak)) void resize_nn_simd_helper_rgb8882bgr888(uint8_t **src, int *offsets, uint8_t *dst, int n) {
  uint8_t *s = *src;
  for (int i = 0; i < n; i++) { uint8_t *p = s + offsets[i]; dst[0] = p[2]; dst[1] = p[1]; dst[2] = p[0]; dst += 3; }
}
__attribute__((weak)) void resize_nn_simd_helper_rgb8882rgb565le(uint8_t **src, int *offsets, uint8_t *dst, int n) {
  uint8_t *s = *src;
  for (int i = 0; i < n; i++) { uint8_t *p = s + offsets[i]; write565_le(dst, pack_rgb888_to_rgb565(p[0], p[1], p[2])); dst += 2; }
}
__attribute__((weak)) void resize_nn_simd_helper_bgr8882rgb565le(uint8_t **src, int *offsets, uint8_t *dst, int n) {
  uint8_t *s = *src;
  for (int i = 0; i < n; i++) { uint8_t *p = s + offsets[i]; write565_le(dst, pack_rgb888_to_rgb565(p[2], p[1], p[0])); dst += 2; }
}
__attribute__((weak)) void resize_nn_simd_helper_rgb8882rgb565be(uint8_t **src, int *offsets, uint8_t *dst, int n) {
  uint8_t *s = *src;
  for (int i = 0; i < n; i++) { uint8_t *p = s + offsets[i]; write565_be(dst, pack_rgb888_to_rgb565(p[0], p[1], p[2])); dst += 2; }
}
__attribute__((weak)) void resize_nn_simd_helper_bgr8882rgb565be(uint8_t **src, int *offsets, uint8_t *dst, int n) {
  uint8_t *s = *src;
  for (int i = 0; i < n; i++) { uint8_t *p = s + offsets[i]; write565_be(dst, pack_rgb888_to_rgb565(p[2], p[1], p[0])); dst += 2; }
}
__attribute__((weak)) void resize_nn_simd_helper_rgb8882gray(uint8_t **src, int *offsets, uint8_t *dst, int n) {
  uint8_t *s = *src;
  for (int i = 0; i < n; i++) { uint8_t *p = s + offsets[i]; dst[0] = rgb_to_gray_y(p[0], p[1], p[2]); dst += 1; }
}
__attribute__((weak)) void resize_nn_simd_helper_bgr8882gray(uint8_t **src, int *offsets, uint8_t *dst, int n) {
  uint8_t *s = *src;
  for (int i = 0; i < n; i++) { uint8_t *p = s + offsets[i]; dst[0] = rgb_to_gray_y(p[2], p[1], p[0]); dst += 1; }
}
__attribute__((weak)) void resize_nn_simd_helper_gray2gray(uint8_t **src, int *offsets, uint8_t *dst, int n) {
  uint8_t *s = *src;
  for (int i = 0; i < n; i++) { dst[i] = s[offsets[i]]; }
}

// HSV resize_nn helpers - never used by YOLO11, weak zero-fill stubs.
__attribute__((weak)) void resize_nn_simd_helper_rgb8882hsv(uint8_t **, int *, uint8_t *dst, int n, int *, int *) { std::memset(dst, 0, static_cast<size_t>(n) * 3); }
__attribute__((weak)) void resize_nn_simd_helper_bgr8882hsv(uint8_t **, int *, uint8_t *dst, int n, int *, int *) { std::memset(dst, 0, static_cast<size_t>(n) * 3); }
__attribute__((weak)) void resize_nn_simd_helper_rgb565le2hsv(uint8_t **, int *, uint8_t *dst, int n, int *, int *) { std::memset(dst, 0, static_cast<size_t>(n) * 3); }
__attribute__((weak)) void resize_nn_simd_helper_rgb565be2hsv(uint8_t **, int *, uint8_t *dst, int n, int *, int *) { std::memset(dst, 0, static_cast<size_t>(n) * 3); }
__attribute__((weak)) void resize_nn_simd_helper_bgr565le2hsv(uint8_t **, int *, uint8_t *dst, int n, int *, int *) { std::memset(dst, 0, static_cast<size_t>(n) * 3); }
__attribute__((weak)) void resize_nn_simd_helper_bgr565be2hsv(uint8_t **, int *, uint8_t *dst, int n, int *, int *) { std::memset(dst, 0, static_cast<size_t>(n) * 3); }

}  // extern "C"

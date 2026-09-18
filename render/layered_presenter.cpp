// layered_presenter.cpp - 透明叠加窗口 + DirectComposition 合成器实现
// danmaku_overlay - 高性能 mpv 弹幕叠加渲染器

#include "layered_presenter.h"
#include "common/log.h"

#include <cstdlib>   // std::abs (WindowManager 轮询阈值)
#include <cmath>
#include <cstring>

namespace danmaku_overlay {

namespace {

// scRGB 参考白: 1.0 对应的绝对亮度 (nit), hdr_peak_ 语义基准
constexpr float kScrgbReferenceWhiteNits = 80.0f;

// HDR 合成着色器字节码 (内联自 hdr_composite_shader.h, 不再单独成文件)
constexpr size_t kHdrCompositeVsSize = 716;
const unsigned char kHdrCompositeVs[] = {
    0x44,0x58,0x42,0x43,0x69,0x10,0xC3,0x12,0xE8,0xF6,0xC9,0x6A,
    0xA3,0xCD,0x61,0x12,0x4D,0x78,0xEF,0x2E,0x01,0x00,0x00,0x00,
    0xCC,0x02,0x00,0x00,0x05,0x00,0x00,0x00,0x34,0x00,0x00,0x00,
    0x80,0x00,0x00,0x00,0xB4,0x00,0x00,0x00,0x0C,0x01,0x00,0x00,
    0x50,0x02,0x00,0x00,0x52,0x44,0x45,0x46,0x44,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x1C,0x00,0x00,0x00,0x00,0x04,0xFE,0xFF,0x00,0x01,0x00,0x00,
    0x1C,0x00,0x00,0x00,0x4D,0x69,0x63,0x72,0x6F,0x73,0x6F,0x66,
    0x74,0x20,0x28,0x52,0x29,0x20,0x48,0x4C,0x53,0x4C,0x20,0x53,
    0x68,0x61,0x64,0x65,0x72,0x20,0x43,0x6F,0x6D,0x70,0x69,0x6C,
    0x65,0x72,0x20,0x31,0x30,0x2E,0x31,0x00,0x49,0x53,0x47,0x4E,
    0x2C,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x08,0x00,0x00,0x00,
    0x20,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x06,0x00,0x00,0x00,
    0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x01,0x00,0x00,
    0x53,0x56,0x5F,0x56,0x65,0x72,0x74,0x65,0x78,0x49,0x44,0x00,
    0x4F,0x53,0x47,0x4E,0x50,0x00,0x00,0x00,0x02,0x00,0x00,0x00,
    0x08,0x00,0x00,0x00,0x38,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x01,0x00,0x00,0x00,0x03,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x0F,0x00,0x00,0x00,0x44,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x03,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
    0x03,0x0C,0x00,0x00,0x53,0x56,0x5F,0x50,0x4F,0x53,0x49,0x54,
    0x49,0x4F,0x4E,0x00,0x54,0x45,0x58,0x43,0x4F,0x4F,0x52,0x44,
    0x00,0xAB,0xAB,0xAB,0x53,0x48,0x44,0x52,0x3C,0x01,0x00,0x00,
    0x40,0x00,0x01,0x00,0x4F,0x00,0x00,0x00,0x60,0x00,0x00,0x04,
    0x12,0x10,0x10,0x00,0x00,0x00,0x00,0x00,0x06,0x00,0x00,0x00,
    0x67,0x00,0x00,0x04,0xF2,0x20,0x10,0x00,0x00,0x00,0x00,0x00,
    0x01,0x00,0x00,0x00,0x65,0x00,0x00,0x03,0x32,0x20,0x10,0x00,
    0x01,0x00,0x00,0x00,0x68,0x00,0x00,0x02,0x01,0x00,0x00,0x00,
    0x55,0x00,0x00,0x07,0x12,0x00,0x10,0x00,0x00,0x00,0x00,0x00,
    0x0A,0x10,0x10,0x00,0x00,0x00,0x00,0x00,0x01,0x40,0x00,0x00,
    0x01,0x00,0x00,0x00,0x01,0x00,0x00,0x07,0x12,0x00,0x10,0x00,
    0x00,0x00,0x00,0x00,0x0A,0x00,0x10,0x00,0x00,0x00,0x00,0x00,
    0x01,0x40,0x00,0x00,0x01,0x00,0x00,0x00,0x56,0x00,0x00,0x05,
    0x22,0x00,0x10,0x00,0x00,0x00,0x00,0x00,0x0A,0x00,0x10,0x00,
    0x00,0x00,0x00,0x00,0x32,0x00,0x00,0x0A,0x22,0x20,0x10,0x00,
    0x00,0x00,0x00,0x00,0x1A,0x00,0x10,0x80,0x41,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x01,0x40,0x00,0x00,0x00,0x00,0x00,0x40,
    0x01,0x40,0x00,0x00,0x00,0x00,0x80,0x3F,0x01,0x00,0x00,0x07,
    0x42,0x00,0x10,0x00,0x00,0x00,0x00,0x00,0x0A,0x10,0x10,0x00,
    0x00,0x00,0x00,0x00,0x01,0x40,0x00,0x00,0x01,0x00,0x00,0x00,
    0x56,0x00,0x00,0x05,0x12,0x00,0x10,0x00,0x00,0x00,0x00,0x00,
    0x2A,0x00,0x10,0x00,0x00,0x00,0x00,0x00,0x32,0x00,0x00,0x09,
    0x12,0x20,0x10,0x00,0x00,0x00,0x00,0x00,0x0A,0x00,0x10,0x00,
    0x00,0x00,0x00,0x00,0x01,0x40,0x00,0x00,0x00,0x00,0x00,0x40,
    0x01,0x40,0x00,0x00,0x00,0x00,0x80,0xBF,0x36,0x00,0x00,0x05,
    0x32,0x20,0x10,0x00,0x01,0x00,0x00,0x00,0x46,0x00,0x10,0x00,
    0x00,0x00,0x00,0x00,0x36,0x00,0x00,0x08,0xC2,0x20,0x10,0x00,
    0x00,0x00,0x00,0x00,0x02,0x40,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x80,0x3F,
    0x3E,0x00,0x00,0x01,0x53,0x54,0x41,0x54,0x74,0x00,0x00,0x00,
    0x0A,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x03,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x03,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};

constexpr size_t kHdrCompositePsSize = 1212;
const unsigned char kHdrCompositePs[] = {
    0x44,0x58,0x42,0x43,0xEB,0x0F,0x15,0xC9,0x9C,0xE4,0x93,0x0E,
    0x06,0x71,0x8A,0xDB,0x35,0x81,0xD8,0xA4,0x01,0x00,0x00,0x00,
    0xBC,0x04,0x00,0x00,0x05,0x00,0x00,0x00,0x34,0x00,0x00,0x00,
    0x40,0x01,0x00,0x00,0x98,0x01,0x00,0x00,0xCC,0x01,0x00,0x00,
    0x40,0x04,0x00,0x00,0x52,0x44,0x45,0x46,0x04,0x01,0x00,0x00,
    0x01,0x00,0x00,0x00,0x90,0x00,0x00,0x00,0x03,0x00,0x00,0x00,
    0x1C,0x00,0x00,0x00,0x00,0x04,0xFF,0xFF,0x00,0x01,0x00,0x00,
    0xDC,0x00,0x00,0x00,0x7C,0x00,0x00,0x00,0x03,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
    0x83,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x05,0x00,0x00,0x00,
    0x04,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,
    0x01,0x00,0x00,0x00,0x0D,0x00,0x00,0x00,0x89,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
    0x01,0x00,0x00,0x00,0x67,0x5F,0x73,0x61,0x6D,0x70,0x00,0x67,
    0x5F,0x73,0x72,0x63,0x00,0x50,0x61,0x72,0x61,0x6D,0x73,0x00,
    0x89,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0xA8,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x10,0x00,0x00,0x00,
    0x02,0x00,0x00,0x00,0xCC,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x67,0x5F,0x70,0x61,0x72,0x61,0x6D,0x73,0x00,0xAB,0xAB,0xAB,
    0x01,0x00,0x03,0x00,0x01,0x00,0x04,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x4D,0x69,0x63,0x72,0x6F,0x73,0x6F,0x66,
    0x74,0x20,0x28,0x52,0x29,0x20,0x48,0x4C,0x53,0x4C,0x20,0x53,
    0x68,0x61,0x64,0x65,0x72,0x20,0x43,0x6F,0x6D,0x70,0x69,0x6C,
    0x65,0x72,0x20,0x31,0x30,0x2E,0x31,0x00,0x49,0x53,0x47,0x4E,
    0x50,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x08,0x00,0x00,0x00,
    0x38,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
    0x03,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x00,0x00,0x00,
    0x44,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x03,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x03,0x03,0x00,0x00,
    0x53,0x56,0x5F,0x50,0x4F,0x53,0x49,0x54,0x49,0x4F,0x4E,0x00,
    0x54,0x45,0x58,0x43,0x4F,0x4F,0x52,0x44,0x00,0xAB,0xAB,0xAB,
    0x4F,0x53,0x47,0x4E,0x2C,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
    0x08,0x00,0x00,0x00,0x20,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x03,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x0F,0x00,0x00,0x00,0x53,0x56,0x5F,0x54,0x61,0x72,0x67,0x65,
    0x74,0x00,0xAB,0xAB,0x53,0x48,0x44,0x52,0x6C,0x02,0x00,0x00,
    0x40,0x00,0x00,0x00,0x9B,0x00,0x00,0x00,0x59,0x00,0x00,0x04,
    0x46,0x8E,0x20,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
    0x5A,0x00,0x00,0x03,0x00,0x60,0x10,0x00,0x00,0x00,0x00,0x00,
    0x58,0x18,0x00,0x04,0x00,0x70,0x10,0x00,0x00,0x00,0x00,0x00,
    0x55,0x55,0x00,0x00,0x62,0x10,0x00,0x03,0x32,0x10,0x10,0x00,
    0x01,0x00,0x00,0x00,0x65,0x00,0x00,0x03,0xF2,0x20,0x10,0x00,
    0x00,0x00,0x00,0x00,0x68,0x00,0x00,0x02,0x03,0x00,0x00,0x00,
    0x31,0x00,0x00,0x08,0x12,0x00,0x10,0x00,0x00,0x00,0x00,0x00,
    0x01,0x40,0x00,0x00,0x00,0x00,0xC0,0x3F,0x1A,0x80,0x20,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x1F,0x00,0x04,0x03,
    0x0A,0x00,0x10,0x00,0x00,0x00,0x00,0x00,0x32,0x00,0x00,0x0D,
    0xF2,0x00,0x10,0x00,0x00,0x00,0x00,0x00,0xE6,0x8E,0x20,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x02,0x40,0x00,0x00,
    0x00,0x00,0x00,0xBF,0x00,0x00,0x00,0xBF,0x00,0x00,0x00,0x3F,
    0x00,0x00,0x00,0xBF,0x46,0x14,0x10,0x00,0x01,0x00,0x00,0x00,
    0x45,0x00,0x00,0x09,0xF2,0x00,0x10,0x00,0x01,0x00,0x00,0x00,
    0x46,0x00,0x10,0x00,0x00,0x00,0x00,0x00,0x46,0x7E,0x10,0x00,
    0x00,0x00,0x00,0x00,0x00,0x60,0x10,0x00,0x00,0x00,0x00,0x00,
    0x45,0x00,0x00,0x09,0xF2,0x00,0x10,0x00,0x00,0x00,0x00,0x00,
    0xE6,0x0A,0x10,0x00,0x00,0x00,0x00,0x00,0x46,0x7E,0x10,0x00,
    0x00,0x00,0x00,0x00,0x00,0x60,0x10,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x07,0xF2,0x00,0x10,0x00,0x00,0x00,0x00,0x00,
    0x46,0x0E,0x10,0x00,0x00,0x00,0x00,0x00,0x46,0x0E,0x10,0x00,
    0x01,0x00,0x00,0x00,0x32,0x00,0x00,0x0D,0xF2,0x00,0x10,0x00,
    0x01,0x00,0x00,0x00,0xE6,0x8E,0x20,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x02,0x40,0x00,0x00,0x00,0x00,0x00,0xBF,
    0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x3F,
    0x46,0x14,0x10,0x00,0x01,0x00,0x00,0x00,0x45,0x00,0x00,0x09,
    0xF2,0x00,0x10,0x00,0x02,0x00,0x00,0x00,0x46,0x00,0x10,0x00,
    0x01,0x00,0x00,0x00,0x46,0x7E,0x10,0x00,0x00,0x00,0x00,0x00,
    0x00,0x60,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x07,
    0xF2,0x00,0x10,0x00,0x00,0x00,0x00,0x00,0x46,0x0E,0x10,0x00,
    0x00,0x00,0x00,0x00,0x46,0x0E,0x10,0x00,0x02,0x00,0x00,0x00,
    0x45,0x00,0x00,0x09,0xF2,0x00,0x10,0x00,0x01,0x00,0x00,0x00,
    0xE6,0x0A,0x10,0x00,0x01,0x00,0x00,0x00,0x46,0x7E,0x10,0x00,
    0x00,0x00,0x00,0x00,0x00,0x60,0x10,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x07,0xF2,0x00,0x10,0x00,0x00,0x00,0x00,0x00,
    0x46,0x0E,0x10,0x00,0x00,0x00,0x00,0x00,0x46,0x0E,0x10,0x00,
    0x01,0x00,0x00,0x00,0x38,0x00,0x00,0x0A,0xF2,0x00,0x10,0x00,
    0x00,0x00,0x00,0x00,0x46,0x0E,0x10,0x00,0x00,0x00,0x00,0x00,
    0x02,0x40,0x00,0x00,0x00,0x00,0x80,0x3E,0x00,0x00,0x80,0x3E,
    0x00,0x00,0x80,0x3E,0x00,0x00,0x80,0x3E,0x36,0x00,0x00,0x05,
    0x82,0x20,0x10,0x00,0x00,0x00,0x00,0x00,0x3A,0x00,0x10,0x00,
    0x00,0x00,0x00,0x00,0x12,0x00,0x00,0x01,0x45,0x00,0x00,0x09,
    0xF2,0x00,0x10,0x00,0x00,0x00,0x00,0x00,0x46,0x10,0x10,0x00,
    0x01,0x00,0x00,0x00,0x46,0x7E,0x10,0x00,0x00,0x00,0x00,0x00,
    0x00,0x60,0x10,0x00,0x00,0x00,0x00,0x00,0x36,0x00,0x00,0x05,
    0x82,0x20,0x10,0x00,0x00,0x00,0x00,0x00,0x3A,0x00,0x10,0x00,
    0x00,0x00,0x00,0x00,0x15,0x00,0x00,0x01,0x38,0x00,0x00,0x08,
    0x72,0x20,0x10,0x00,0x00,0x00,0x00,0x00,0x46,0x02,0x10,0x00,
    0x00,0x00,0x00,0x00,0x06,0x80,0x20,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x3E,0x00,0x00,0x01,0x53,0x54,0x41,0x54,
    0x74,0x00,0x00,0x00,0x13,0x00,0x00,0x00,0x03,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x08,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x02,0x00,0x00,0x00,
    0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x05,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x02,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};

} // anonymous namespace

// 静态成员
WindowManager* WindowManager::s_instance = nullptr;
const wchar_t* WindowManager::kClassName = L"MpvDanmakuOverlayClass";

// 计算 mpv 客户区 (视频区域) 在**屏幕坐标**下的矩形。
// 不能用 GetWindowRect — 那包含标题栏 / 边框 / DWM 阴影, 弹幕会画到标题栏上。
// GetClientRect 得到的是客户区坐标 (相对客户区 0,0), 用 ClientToScreen 换算。
static bool get_mpv_client_rect_screen(HWND mpv_hwnd, RECT& out) {
    if (!mpv_hwnd || !IsWindow(mpv_hwnd)) return false;
    RECT cr{};
    if (!GetClientRect(mpv_hwnd, &cr)) return false;
    POINT tl{ cr.left, cr.top };
    POINT br{ cr.right, cr.bottom };
    ClientToScreen(mpv_hwnd, &tl);
    ClientToScreen(mpv_hwnd, &br);
    out.left = tl.x; out.top = tl.y;
    out.right = br.x; out.bottom = br.y;
    return true;
}

// ============================================================
// 构造 / 析构
// ============================================================

WindowManager::WindowManager() {
    s_instance = this;
}

WindowManager::~WindowManager() {
    destroy();
}

// ============================================================
// 注册窗口类
// ============================================================

bool WindowManager::register_window_class() {
    if (class_registered_) return true;

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(WNDCLASSEXW);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = static_wnd_proc;
    wc.hInstance      = hinstance_;
    wc.hCursor        = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground  = nullptr;  // 无背景画刷 (透明)
    wc.lpszClassName  = kClassName;

    ATOM atom = RegisterClassExW(&wc);
    if (atom == 0) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            DANMAKU_LOG_ERROR(LOG_TAG, "RegisterClassEx 失败 (GLE=%lu)", err);
            return false;
        }
    }

    class_registered_ = true;
    return true;
}

// ============================================================
// 创建透明叠加窗口
// ============================================================

ErrorCode WindowManager::create(HINSTANCE hInstance, uint32_t width,
                                 uint32_t height, bool /*borderless*/) {
    hinstance_ = hInstance;

    if (!register_window_class()) {
        return ErrorCode::WindowFailed;
    }

    // 获取 DPI
    if (mpv_hwnd_) {
        dpi_ = platform::GetWindowDpi(mpv_hwnd_);
    } else {
        dpi_.dpi_x = dpi_.dpi_y = 96;
        dpi_.update();
    }

    // 物理尺寸
    phys_width_  = platform::LogicalToPhysical(width, dpi_.dpi_x);
    phys_height_ = platform::LogicalToPhysical(height, dpi_.dpi_y);

    // ============================================================
    // 幽灵窗口方案 (DirectComposition 透明显示层)
    // ============================================================
    // 顶层层窗口, 作为 mpv 的 **owned window** (拥有者关系, 非父子关系):
    //   CreateWindowEx 的 hwndParent 对 WS_POPUP 窗口 = 设置 owner。
    //   owned window 自动保持在 owner (mpv) 之上 — "仅置顶于 mpv" 而不全局置顶;
    //   且 owner 最小化/关闭时 owned window 跟随。不裁剪内容 (弹幕铺满客户区)。
    //
    // 窗口扩展样式:
    //   WS_EX_NOREDIRECTIONBITMAP - 不做 GDI 重定向, 窗口内容完全由
    //       DirectComposition (CreateTargetForHwnd) 提供, DWM 在 GPU 直接合成
    //   WS_EX_LAYERED      - ⚠ 关键: DComp 窗口 (NOREDIRECTIONBITMAP) 默认
    //       "整个窗口统一命中"鼠标 (hit-test 子系统不知道客户区透明),
    //       必须加 LAYERED 让 DWM 走 layered 的 per-pixel 命中测试路径。
    //       与 NOREDIRECTIONBITMAP 共存 (Win8+) 不会引入 CPU 读回 —
    //       内容仍由 DComp 提供, LAYERED 只负责输入命中语义。
    //   WS_EX_TRANSPARENT    - 忽略窗口形状, 整个窗口鼠标事件穿透到下层 mpv
    //       (LAYERED + TRANSPARENT = 输入全透明, Chrome/Firefox 的 DComp
    //       合成器窗口同款组合)
    //   WS_EX_TOOLWINDOW     - 不出现在任务栏 / Alt-Tab
    //   WS_EX_NOACTIVATE     - 点击不激活、不抢焦点
    //   ⚠ 不加 WS_EX_TOPMOST — owned window 已保证在 mpv 之上,
    //     全局置顶反而会盖住其它程序窗口。
    DWORD ex_style = WS_EX_LAYERED
                   | WS_EX_TRANSPARENT
                   | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE
                   | WS_EX_NOREDIRECTIONBITMAP;
    DWORD style = WS_POPUP;

    // 初始位置: 对齐 mpv 客户区 (屏幕坐标, 不含标题栏)
    int x = 0, y = 0;
    int w = static_cast<int>(phys_width_);
    int h = static_cast<int>(phys_height_);

    if (mpv_hwnd_ && IsWindow(mpv_hwnd_)) {
        RECT rc{};
        if (get_mpv_client_rect_screen(mpv_hwnd_, rc)) {
            x = rc.left;
            y = rc.top;
            w = rc.right - rc.left;
            h = rc.bottom - rc.top;
            phys_width_  = static_cast<uint32_t>(w);
            phys_height_ = static_cast<uint32_t>(h);
            mpv_rect_x_ = x;
            mpv_rect_y_ = y;
            mpv_rect_w_ = w;
            mpv_rect_h_ = h;
        }
    }

    DANMAKU_LOG_INFO(LOG_TAG, "创建幽灵窗口: owner=0x%p x=%d y=%d w=%d h=%d dpi=%u",
                     mpv_hwnd_, x, y, w, h, dpi_.dpi_x);

    hwnd_ = CreateWindowExW(
        ex_style,
        kClassName,
        L"mpv danmaku overlay",
        style,
        x, y, w, h,
        mpv_hwnd_,         // owner = mpv (owned window, 保持在 mpv 之上)
        nullptr,
        hinstance_,
        nullptr
    );

    if (!hwnd_) {
        DANMAKU_LOG_ERROR(LOG_TAG, "CreateWindowEx (幽灵窗口) 失败 (GLE=%lu)",
                          GetLastError());
        return ErrorCode::WindowFailed;
    }

    // owned window 创建后可能被排到 mpv 后面, 主动把它放到 mpv 上方
    bring_above_mpv();

    // ⚠ 不在这里 ShowWindow:
    //   DirectComposition 的 target (CreateTargetForHwnd) 在引擎初始化时
    //   才建立 (presenter.initialize)。若此刻就显示窗口, 会闪现"尚无 DComp
    //   内容"的窗口 (可能黑屏) 盖住 mpv。改为由 main 线程在 engine_ready
    //   之后调用 window.show() (届时首帧已清屏为透明)。
    DANMAKU_LOG_INFO(LOG_TAG,
                     "幽灵窗口已创建: hwnd=0x%p ex_style=TRANSPARENT|TOOLWINDOW|NOREDIRECTIONBITMAP",
                     hwnd_);
    return ErrorCode::Ok;
}

// ============================================================
// DPI 获取
// ============================================================

DpiInfo WindowManager::get_dpi() const {
    if (hwnd_) return platform::GetWindowDpi(hwnd_);
    return dpi_;
}

// ============================================================
// 更新来自 IPC 的 mpv 窗口位置
// ============================================================

void WindowManager::update_mpv_window_pos(int x, int y, int width,
                                            int height, int dpi) {
    // ⚠ 这个 IPC 消息已被废弃 (bridge 不再发送), 仅保留协议兼容。
    //
    // 窗口几何的单一可信来源是 sync_to_mpv_window() 的轮询:
    //   - overlay 现在是 mpv 的**子窗口**, 位置 (0,0) 由父子关系自动跟随;
    //   - 尺寸由 GetClientRect(mpv) 轮询同步。
    // 这里**绝不能**再写 mpv_rect_w_ / mpv_rect_h_ — 否则 bridge 若发来
    // OSD 尺寸 (1920x1080) 会覆盖掉真实的客户区缓存 (1936x1096),
    // 导致 sync_to_mpv_window 每帧都判定"尺寸变了"从而反复 resize +
    // bring_above_mpv(), 产生无谓的窗口消息与 Z-order 抖动。
    //
    // 形参仅为协议兼容保留, 一律忽略 (显式 void 掉避免 unused-parameter 告警)。
    (void)x; (void)y; (void)width; (void)height; (void)dpi;
}

// ============================================================
// 主动轮询 mpv 窗口位置 (在渲染 tick 中调用)
// ============================================================

void WindowManager::sync_to_mpv_window() {
    if (!mpv_hwnd_ || !IsWindow(mpv_hwnd_)) return;

    // 幽灵窗口是**顶层 owned 窗口**, 位置/尺寸都需自己跟随 mpv 客户区
    // (屏幕坐标, 不含标题栏)。用 GetClientRect + ClientToScreen 轮询。
    RECT rc{};
    if (!get_mpv_client_rect_screen(mpv_hwnd_, rc)) return;

    int x = rc.left;
    int y = rc.top;
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    // 加 2px 阈值: mpv 客户区坐标可能因 DWM 边框/缩放产生 1px 抖动,
    // 每帧 SetWindowPos 会触发 DWM 重排, 造成不必要的窗口消息与合成开销。
    const int kPosThreshold = 2;
    const int kSizeThreshold = 2;
    if (std::abs(x - mpv_rect_x_) >= kPosThreshold ||
        std::abs(y - mpv_rect_y_) >= kPosThreshold ||
        std::abs(w - mpv_rect_w_) >= kSizeThreshold ||
        std::abs(h - mpv_rect_h_) >= kSizeThreshold) {
        mpv_rect_x_ = x;
        mpv_rect_y_ = y;
        mpv_rect_w_ = w;
        mpv_rect_h_ = h;
        phys_width_  = static_cast<uint32_t>(w);
        phys_height_ = static_cast<uint32_t>(h);
        // 同步位置 + 尺寸 (owned window, 屏幕坐标)
        SetWindowPos(hwnd_, nullptr, x, y, w, h,
                     SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);

        // 检查 DPI 是否变化 (跨显示器移动)
        DpiInfo new_dpi = platform::GetWindowDpi(mpv_hwnd_);
        if (new_dpi.dpi_x != dpi_.dpi_x) {
            dpi_ = new_dpi;
        }
    }
}

// ============================================================
// 将 overlay 置于 mpv 窗口正上方 (Z-order)
// ============================================================

void WindowManager::bring_above_mpv() {
    if (!hwnd_ || !mpv_hwnd_ || !IsWindow(mpv_hwnd_)) return;

    // 幽灵窗口是 mpv 的 **owned window**。Windows 保证 owned window 恒在
    // owner (mpv) 之上、owner 之后 (其它程序激活时 overlay 跟着 mpv 沉底),
    // 这正好满足"仅置顶于 mpv、不盖其它程序"。
    // ⚠ 不能用 SetWindowPos(hwnd_, HWND_TOP) — 那会把 overlay 抬到整个
    //   Z-order 顶端, 即使用户切到别的程序也会盖住它, 违反"仅置顶于 mpv"。
    //   因此这里无事可做 (也不需要周期重申), 保留函数仅为接口兼容。
}

// ============================================================
// 显示 / 隐藏
// ============================================================

void WindowManager::show() {
    if (hwnd_) ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
}

void WindowManager::hide() {
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
}

void WindowManager::set_visible(bool visible) {
    if (visible) {
        // 显示前先把窗口摆回 mpv 客户区 (隐藏期间 mpv 可能移动过)
        RECT rc{};
        if (get_mpv_client_rect_screen(mpv_hwnd_, rc)) {
            SetWindowPos(hwnd_, nullptr, rc.left, rc.top,
                         rc.right - rc.left, rc.bottom - rc.top,
                         SWP_NOACTIVATE | SWP_NOZORDER);
        }
        show();
    } else {
        hide();
    }
}

// ============================================================
// 销毁
// ============================================================

void WindowManager::destroy() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (s_instance == this) {
        s_instance = nullptr;
    }
}

// ============================================================
// 窗口过程
// ============================================================

LRESULT CALLBACK WindowManager::static_wnd_proc(HWND hwnd, UINT msg,
                                                   WPARAM wp, LPARAM lp) {
    WindowManager* self = s_instance;
    if (self) {
        return self->wnd_proc(hwnd, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT WindowManager::wnd_proc(HWND hwnd, UINT msg,
                                  WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    // 阻止 overlay 在点击时夺取焦点 / 切到前台
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    // ============================================================
    // 幽灵窗口鼠标处理 (failsafe)
    // ============================================================
    // 正确路径: WS_EX_TRANSPARENT 让 DWM 把整个窗口的鼠标事件**直接穿透**
    //   到 mpv, overlay 根本收不到 WM_NCHITTEST / 鼠标消息 — 这是幽灵窗口
    //   的原生行为, 用户要求"不接收任何鼠标键盘事件"。
    //
    // failsafe: 万一某台机器上 WS_EX_TRANSPARENT 未生效, overlay 仍会收到
    //   鼠标消息。此时走下面的转发分支 (HTCLIENT + PostMessage) 兜底,
    //   保证 mpv 至少还能收到事件。两条路径任一生效即可。
    case WM_NCHITTEST: {
        // ⚠ 不能返回 HTTRANSPARENT (跨进程会丢消息, 见 memory)。
        //   返回 HTCLIENT 让消息到达转发分支。
        return HTCLIENT;
    }

    // --- 鼠标消息转发给 mpv (failsafe) ---
    // ⚠ 必须用 PostMessage 而不是 SendMessage:
    //   - Windows 原生投递鼠标输入是投递到线程消息队列;
    //   - WM_MOUSEMOVE 高频, 同步 SendMessage 每条都要等 mpv 返回, 会积压 /
    //     超时丢弃 (曾表现为 uosc 不随移动弹出);
    //   - PostMessage 走 mpv 正常消息循环, GetMessagePos() 等队列状态才对。
    // 队列满 (极罕见) 时退回同步发送。
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK: {
        if (mpv_hwnd_ && IsWindow(mpv_hwnd_)) {
            // 只在最初几条打日志 (failsafe 触发说明 WS_EX_TRANSPARENT 没生效)
            static int fwd_logged = 0;
            if (fwd_logged < 3) {
                ++fwd_logged;
                DANMAKU_LOG_INFO(LOG_TAG,
                    "[failsafe] 转发鼠标消息给 mpv: msg=0x%04X (第 %d 条, "
                    "说明 WS_EX_TRANSPARENT 未生效)", msg, fwd_logged);
            }
            if (!PostMessageW(mpv_hwnd_, msg, wp, lp)) {
                DWORD_PTR res = 0;
                SendMessageTimeoutW(mpv_hwnd_, msg, wp, lp,
                                    SMTO_ABORTIFHUNG | SMTO_NORMAL, 20, &res);
            }
        }
        return 0;
    }

    // 光标 (failsafe): 转发给 mpv, wParam 换成 mpv 的 hwnd
    case WM_SETCURSOR: {
        if (mpv_hwnd_ && IsWindow(mpv_hwnd_)) {
            DWORD_PTR res = 0;
            SendMessageTimeoutW(mpv_hwnd_, WM_SETCURSOR,
                                reinterpret_cast<WPARAM>(mpv_hwnd_), lp,
                                SMTO_ABORTIFHUNG | SMTO_NORMAL, 20, &res);
            return static_cast<LRESULT>(res);
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    case WM_SIZE: {
        // 窗口大小变化时更新物理尺寸
        uint32_t new_w = LOWORD(lp);
        uint32_t new_h = HIWORD(lp);
        if (new_w > 0 && new_h > 0) {
            phys_width_  = new_w;
            phys_height_ = new_h;
        }
        return 0;
    }

    case WM_GETMINMAXINFO: {
        // 允许窗口最小尺寸为 1x1
        MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize.x = 1;
        mmi->ptMinTrackSize.y = 1;
        return 0;
    }

    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

// ============================================================
// 构造 / 析构
// ============================================================

LayeredPresenter::LayeredPresenter() {}
LayeredPresenter::~LayeredPresenter() { shutdown(); }

// ============================================================
// initialize
// ============================================================

ErrorCode LayeredPresenter::initialize(HWND hwnd, uint32_t width,
                                       uint32_t height, DpiInfo dpi) {
    hwnd_ = hwnd;
    phys_width_  = width;
    phys_height_ = height;
    dpi_ = dpi;

    ErrorCode ec = init_d3d11();
    if (ec != ErrorCode::Ok) return ec;

    ec = init_d2d();
    if (ec != ErrorCode::Ok) return ec;

    ec = create_composition_resources();
    if (ec != ErrorCode::Ok) return ec;

    // 圆角描边样式 (D2D1_LINE_JOIN_ROUND: 汉字撇捺的尖角不会炸出毛刺)
    if (d2d_factory_) {
        D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_ROUND,   // startCap
            D2D1_CAP_STYLE_ROUND,   // endCap
            D2D1_CAP_STYLE_ROUND,   // dashCap
            D2D1_LINE_JOIN_ROUND,   // lineJoin
            10.0f,                  // miterLimit
            D2D1_DASH_STYLE_SOLID,
            0.0f);
        if (FAILED(d2d_factory_->CreateStrokeStyle(sp, nullptr, 0,
                                                   &stroke_style_))) {
            DANMAKU_LOG_WARN(LOG_TAG, "创建描边样式失败, 使用默认样式");
            stroke_style_.Reset();
        }
    }

    // --- SDR 路径画刷 (直接画进 swap chain, sRGB 恒等) ---
    HRESULT hr = d2d_context_->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &color_brush_);
    if (FAILED(hr)) return ErrorCode::D2DFailed;

    // --- HDR 离屏 AA 路径 (独立上下文 + 独立画刷, 画到 scratch 上) ---
    // 画刷数值范围与 SDR 一致 (≤1.0), 峰值亮度由合成 pass 统一施加。
    if (hdr_scratch_ctx_) {
        hr = hdr_scratch_ctx_->CreateSolidColorBrush(
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &hdr_brush_);
        if (FAILED(hr)) hdr_brush_.Reset();
    }

    // 首帧清屏: 确保窗口显示前 back buffer 是透明的, 避免闪现垃圾/黑帧。
    begin_frame();
    end_frame();
    present(1.0f);

    return ErrorCode::Ok;
}

// ============================================================
// init_d3d11
// ============================================================

ErrorCode LayeredPresenter::init_d3d11() {
    D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3,
    };
    D3D_FEATURE_LEVEL selected_level;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        levels, 4, D3D11_SDK_VERSION,
        &d3d_device_, &selected_level, &d3d_context_);
    if (FAILED(hr)) {
        DANMAKU_LOG_ERROR(LOG_TAG, "D3D11CreateDevice 失败 (HR=0x%08X)",
                          static_cast<unsigned int>(hr));
        return ErrorCode::D3D11Failed;
    }

    DANMAKU_LOG_INFO(LOG_TAG, "D3D11 设备创建成功 (feature level 0x%04X)",
                     static_cast<unsigned int>(selected_level));
    return ErrorCode::Ok;
}

// ============================================================
// init_d2d
// ============================================================

ErrorCode LayeredPresenter::init_d2d() {
    HRESULT hr = D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED,
        __uuidof(ID2D1Factory1), nullptr, &d2d_factory_);
    if (FAILED(hr)) return ErrorCode::D2DFailed;

    ComPtr<IDXGIDevice> dxgi_device;
    d3d_device_.As(&dxgi_device);
    hr = d2d_factory_->CreateDevice(dxgi_device.Get(), &d2d_device_);
    if (FAILED(hr)) return ErrorCode::D2DFailed;

    hr = d2d_device_->CreateDeviceContext(
        D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2d_context_);
    if (FAILED(hr)) return ErrorCode::D2DFailed;

    d2d_context_->SetDpi(96.0f, 96.0f);

    hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory), &dwrite_factory_);
    if (FAILED(hr)) return ErrorCode::D2DFailed;

    // --- HDR 离屏 AA 用的独立 D2D 设备上下文 ---
    // 与 d2d_context_ 分开, 避免每帧在 "HDR swap chain 目标" 和 "scratch"
    // 之间来回 SetTarget; 两个上下文来自同一个 ID2D1Device, 共享 GPU 资源。
    hr = d2d_device_->CreateDeviceContext(
        D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &hdr_scratch_ctx_);
    if (FAILED(hr)) {
        DANMAKU_LOG_WARN(LOG_TAG, "HDR 离屏上下文创建失败, HDR 将退化为直画路径");
        hdr_scratch_ctx_.Reset();
    } else {
        hdr_scratch_ctx_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }

    // --- 颜色查找表 ---
    // sdr_lin_lut_: sRGB → 线性 (0..1), 与峰值无关, 只建一次。
    for (int i = 0; i < 256; ++i) {
        float c = static_cast<float>(i) / 255.0f;
        sdr_lin_lut_[i] = (c <= 0.04045f)
                        ? c / 12.92f
                        : std::pow((c + 0.055f) / 1.055f, 2.4f);
    }
    // --- 合成 pass 的 D3D11 资源 ---
    if (init_hdr_composite() != ErrorCode::Ok) {
        DANMAKU_LOG_WARN(LOG_TAG,
            "HDR 合成着色器初始化失败, HDR 模式将不可用 (SDR 不受影响)");
    }

    return ErrorCode::Ok;
}

// ============================================================
// init_hdr_composite - HDR 合成 pass 的 D3D11 资源
// ============================================================

ErrorCode LayeredPresenter::init_hdr_composite() {
    if (!d3d_device_) return ErrorCode::D3D11Failed;

    HRESULT hr = d3d_device_->CreateVertexShader(
        kHdrCompositeVs, kHdrCompositeVsSize, nullptr, &comp_vs_);
    if (FAILED(hr)) {
        DANMAKU_LOG_ERROR(LOG_TAG, "创建合成 VS 失败 (HR=0x%08X)",
                          static_cast<unsigned int>(hr));
        return ErrorCode::D3D11Failed;
    }

    hr = d3d_device_->CreatePixelShader(
        kHdrCompositePs, kHdrCompositePsSize, nullptr, &comp_ps_);
    if (FAILED(hr)) {
        DANMAKU_LOG_ERROR(LOG_TAG, "创建合成 PS 失败 (HR=0x%08X)",
                          static_cast<unsigned int>(hr));
        return ErrorCode::D3D11Failed;
    }

    // 常量缓冲: float4 (亮度缩放, 合成源缩放, 1/宽, 1/高)
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth           = 16;
    cbd.Usage               = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags           = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags      = D3D11_CPU_ACCESS_WRITE;
    cbd.MiscFlags           = 0;
    cbd.StructureByteStride = 0;
    hr = d3d_device_->CreateBuffer(&cbd, nullptr, &comp_cb_);
    if (FAILED(hr)) return ErrorCode::D3D11Failed;

    // 预乘 source-over: out = src + dst * (1 - src.a)
    D3D11_BLEND_DESC bd = {};
    bd.AlphaToCoverageEnable  = FALSE;
    bd.IndependentBlendEnable = FALSE;
    D3D11_RENDER_TARGET_BLEND_DESC& rt = bd.RenderTarget[0];
    rt.BlendEnable           = TRUE;
    rt.SrcBlend              = D3D11_BLEND_ONE;
    rt.DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp               = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha         = D3D11_BLEND_ONE;
    rt.DestBlendAlpha        = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha          = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = d3d_device_->CreateBlendState(&bd, &comp_blend_);
    if (FAILED(hr)) return ErrorCode::D3D11Failed;

    // 点采样: scratch → 目标是 1:1 (或整数倍降采样), 不需要插值,
    // 避免线性插值在边缘引入额外的亮度泄漏。
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MipLODBias     = 0.0f;
    sd.MaxAnisotropy  = 1;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MinLOD         = 0.0f;
    sd.MaxLOD         = D3D11_FLOAT32_MAX;
    hr = d3d_device_->CreateSamplerState(&sd, &comp_sampler_);
    if (FAILED(hr)) return ErrorCode::D3D11Failed;

    // D2D 会往立即上下文里塞自己的光栅化状态 (含 scissor), 显式覆盖掉
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode        = D3D11_FILL_SOLID;
    rd.CullMode        = D3D11_CULL_NONE;
    rd.FrontCounterClockwise = FALSE;
    rd.DepthBias       = 0;
    rd.ScissorEnable   = FALSE;
    rd.AntialiasedLineEnable = FALSE;
    rd.MultisampleEnable     = FALSE;
    hr = d3d_device_->CreateRasterizerState(&rd, &comp_rs_);
    if (FAILED(hr)) return ErrorCode::D3D11Failed;

    hdr_composite_ready_ = true;
    return ErrorCode::Ok;
}

// ============================================================
// ensure_hdr_scratch - 按需 (重新)创建离屏缓冲
// ============================================================

ErrorCode LayeredPresenter::ensure_hdr_scratch() {
    if (!d3d_device_ || !hdr_scratch_ctx_) return ErrorCode::D3D11Failed;
    if (phys_width_ == 0 || phys_height_ == 0) return ErrorCode::InvalidArgument;

    // 尺寸未变且位图仍有效 → 直接复用 (热路径, 零分配)
    if (hdr_scratch_bmp_ && hdr_scratch_tex_ && hdr_scratch_srv_
        && hdr_scratch_w_ == phys_width_
        && hdr_scratch_h_ == phys_height_) {
        return ErrorCode::Ok;
    }

    return create_hdr_scratch();
}

ErrorCode LayeredPresenter::create_hdr_scratch() {
    const uint32_t w = phys_width_;
    const uint32_t h = phys_height_;

    release_hdr_scratch_target();
    hdr_scratch_bmp_.Reset();
    hdr_scratch_srv_.Reset();
    hdr_scratch_tex_.Reset();

    D3D11_TEXTURE2D_DESC td = {};
    td.Width          = w;
    td.Height         = h;
    td.MipLevels      = 1;
    td.ArraySize      = 1;
    td.Format         = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count   = 1;
    td.SampleDesc.Quality = 0;
    td.Usage          = D3D11_USAGE_DEFAULT;
    td.BindFlags      = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = 0;
    td.MiscFlags      = 0;

    HRESULT hr = d3d_device_->CreateTexture2D(&td, nullptr, &hdr_scratch_tex_);
    if (FAILED(hr)) {
        DANMAKU_LOG_ERROR(LOG_TAG, "HDR scratch 纹理创建失败 (HR=0x%08X, %ux%u)",
                          static_cast<unsigned int>(hr), w, h);
        return ErrorCode::D3D11Failed;
    }

    hr = d3d_device_->CreateShaderResourceView(
        hdr_scratch_tex_.Get(), nullptr, &hdr_scratch_srv_);
    if (FAILED(hr)) {
        hdr_scratch_tex_.Reset();
        return ErrorCode::D3D11Failed;
    }

    ComPtr<IDXGISurface> surface;
    hr = hdr_scratch_tex_.As(&surface);
    if (FAILED(hr)) {
        hdr_scratch_srv_.Reset(); hdr_scratch_tex_.Reset();
        return ErrorCode::D3D11Failed;
    }

    // dpi 固定 96: 缓冲与屏幕 1:1, DIP 坐标 == 物理像素
    D2D1_BITMAP_PROPERTIES1 props = {};
    props.pixelFormat.format    = DXGI_FORMAT_R16G16B16A16_FLOAT;
    props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    props.dpiX = 96.0f;
    props.dpiY = 96.0f;
    props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;

    hr = hdr_scratch_ctx_->CreateBitmapFromDxgiSurface(
        surface.Get(), props, &hdr_scratch_bmp_);
    if (FAILED(hr)) {
        DANMAKU_LOG_ERROR(LOG_TAG, "HDR scratch D2D 位图创建失败 (HR=0x%08X)",
                          static_cast<unsigned int>(hr));
        hdr_scratch_srv_.Reset(); hdr_scratch_tex_.Reset();
        return ErrorCode::D2DFailed;
    }

    hdr_scratch_ctx_->SetDpi(96.0f, 96.0f);

    hdr_scratch_w_ = w;
    hdr_scratch_h_ = h;

    DANMAKU_LOG_INFO(LOG_TAG, "HDR 离屏缓冲: %ux%u (%.1f MB)",
                     w, h, static_cast<double>(w) * h * 8.0 / 1048576.0);
    return ErrorCode::Ok;
}

void LayeredPresenter::release_hdr_scratch_target() {
    if (hdr_scratch_ctx_) hdr_scratch_ctx_->SetTarget(nullptr);
}

void LayeredPresenter::destroy_hdr_scratch() {
    release_hdr_scratch_target();
    hdr_scratch_bmp_.Reset();
    hdr_scratch_srv_.Reset();
    hdr_scratch_tex_.Reset();
    hdr_scratch_w_ = 0;
    hdr_scratch_h_ = 0;
}

// ============================================================
// back buffer RTV 缓存
// ============================================================

ID3D11RenderTargetView* LayeredPresenter::acquire_backbuffer_rtv(
    ID3D11Texture2D* tex) {
    if (!tex || !d3d_device_) return nullptr;

    for (int i = 0; i < kBackBufferRtvSlots; ++i) {
        if (bb_rtv_tex_[i] == tex && bb_rtv_[i]) return bb_rtv_[i].Get();
    }

    // slot 轮转替换 (swap chain 只有 2~3 个缓冲, 命中率 100%)
    const int slot = bb_rtv_next_ % kBackBufferRtvSlots;
    bb_rtv_next_ = (bb_rtv_next_ + 1) % kBackBufferRtvSlots;
    bb_rtv_[slot].Reset();
    if (FAILED(d3d_device_->CreateRenderTargetView(tex, nullptr,
                                                   bb_rtv_[slot].GetAddressOf()))) {
        bb_rtv_tex_[slot] = nullptr;
        return nullptr;
    }
    bb_rtv_tex_[slot] = tex;
    return bb_rtv_[slot].Get();
}

void LayeredPresenter::release_backbuffer_rtvs() {
    for (int i = 0; i < kBackBufferRtvSlots; ++i) {
        bb_rtv_[i].Reset();
        bb_rtv_tex_[i] = nullptr;
    }
    bb_rtv_next_ = 0;
}

// ============================================================
// composite_hdr_scratch - scratch → HDR swap chain
// ============================================================
// out.rgb = scratch.rgb * (hdr_peak / 80)   (预乘, 绝对亮度)
// out.a   = scratch.a                        (coverage)
// 混合: ONE / INV_SRC_ALPHA  →  标准预乘 source-over
// ============================================================

void LayeredPresenter::composite_hdr_scratch() {
    if (!hdr_composite_ready_ || !d3d_context_ || !back_buffer_
        || !hdr_scratch_srv_) {
        return;
    }

    ID3D11RenderTargetView* rtv = acquire_backbuffer_rtv(back_buffer_.Get());
    if (!rtv) return;

    // --- 常量: 亮度缩放 + 合成源缩放 ---
    struct CbData {
        float scale;
        float src_scale;
        float inv_w;
        float inv_h;
    } cb;
    // 亮度缩放: 白色 scRGB = hdr_peak / 80。
    cb.scale = hdr_peak_ / kScrgbReferenceWhiteNits;
    cb.src_scale = 1.0f;   // 离屏缓冲与屏幕 1:1, 无需缩放 (字段保留以匹配 shader cbuffer 布局)
    cb.inv_w = 1.0f / static_cast<float>(hdr_scratch_w_);
    cb.inv_h = 1.0f / static_cast<float>(hdr_scratch_h_);

    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(d3d_context_->Map(comp_cb_.Get(), 0,
                                    D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        if (ms.pData) std::memcpy(ms.pData, &cb, sizeof(cb));
        d3d_context_->Unmap(comp_cb_.Get(), 0);
    }

    // --- 状态 (D2D 改过立即上下文的状态, 这里显式覆盖需要的部分) ---
    D3D11_VIEWPORT vp = {};
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width    = static_cast<float>(phys_width_);
    vp.Height   = static_cast<float>(phys_height_);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    d3d_context_->RSSetViewports(1, &vp);
    d3d_context_->RSSetState(comp_rs_.Get());
    d3d_context_->RSSetScissorRects(0, nullptr);

    ID3D11RenderTargetView* rtvs[1] = { rtv };
    d3d_context_->OMSetRenderTargets(1, rtvs, nullptr);
    const float kBlendFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    d3d_context_->OMSetBlendState(comp_blend_.Get(), kBlendFactor, 0xFFFFFFFF);

    d3d_context_->IASetInputLayout(nullptr);
    d3d_context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    d3d_context_->VSSetShader(comp_vs_.Get(), nullptr, 0);
    d3d_context_->PSSetShader(comp_ps_.Get(), nullptr, 0);

    ID3D11Buffer* cbs[1] = { comp_cb_.Get() };
    d3d_context_->PSSetConstantBuffers(0, 1, cbs);
    ID3D11SamplerState* samps[1] = { comp_sampler_.Get() };
    d3d_context_->PSSetSamplers(0, 1, samps);
    ID3D11ShaderResourceView* srvs[1] = { hdr_scratch_srv_.Get() };
    d3d_context_->PSSetShaderResources(0, 1, srvs);

    d3d_context_->Draw(4, 0);   // VS 用 SV_VertexID 生成全屏三角形带, 无顶点缓冲

    // 解绑: 下一帧 scratch 要重新作为 D2D 渲染目标
    ID3D11ShaderResourceView* null_srv[1] = { nullptr };
    d3d_context_->PSSetShaderResources(0, 1, null_srv);
    ID3D11RenderTargetView* null_rtv[1] = { nullptr };
    d3d_context_->OMSetRenderTargets(1, null_rtv, nullptr);
}

// ============================================================
// create_composition_resources - swap chain + DComp 视觉树
// ============================================================

ErrorCode LayeredPresenter::create_composition_resources() {
    // 1. 创建 composition swap chain (初始 SDR 格式, HDR 由 rebuild_for_hdr 切换)
    ErrorCode ec = create_swap_chain(DXGI_FORMAT_B8G8R8A8_UNORM);
    if (ec != ErrorCode::Ok) return ec;

    HRESULT hr;

    // 3. 创建 DirectComposition 设备
    hr = DCompositionCreateDevice(
        nullptr, __uuidof(IDCompositionDevice),
        reinterpret_cast<void**>(dcomp_device_.GetAddressOf()));
    if (FAILED(hr)) {
        DANMAKU_LOG_ERROR(LOG_TAG, "DCompositionCreateDevice 失败 (HR=0x%08X)",
                          static_cast<unsigned int>(hr));
        return ErrorCode::D3D11Failed;
    }

    // 4. 创建 target + visual, 绑定 swap chain 到视觉树
    hr = dcomp_device_->CreateTargetForHwnd(hwnd_, TRUE,
                                            dcomp_target_.GetAddressOf());
    if (FAILED(hr)) {
        DANMAKU_LOG_ERROR(LOG_TAG, "CreateTargetForHwnd 失败 (HR=0x%08X)",
                          static_cast<unsigned int>(hr));
        return ErrorCode::D3D11Failed;
    }
    hr = dcomp_device_->CreateVisual(dcomp_visual_.GetAddressOf());
    if (FAILED(hr)) {
        DANMAKU_LOG_ERROR(LOG_TAG, "CreateVisual 失败 (HR=0x%08X)",
                          static_cast<unsigned int>(hr));
        return ErrorCode::D3D11Failed;
    }
    hr = dcomp_visual_->SetContent(swap_chain_.Get());
    if (FAILED(hr)) return ErrorCode::D3D11Failed;

    // ⚠ 不再挂 IDCompositionEffectGroup: 该 legacy 离屏合成路径在 HDR/FP16 层上
    //   疑似会让 DWM 丢弃 per-pixel alpha (实测 HDR 模式整层变成不透明黑幕)。
    //   整层透明度改为在 map_color 里乘进每像素 premultiplied alpha。

    hr = dcomp_target_->SetRoot(dcomp_visual_.Get());
    if (FAILED(hr)) return ErrorCode::D3D11Failed;
    hr = dcomp_device_->Commit();
    if (FAILED(hr)) return ErrorCode::D3D11Failed;

    DANMAKU_LOG_INFO(LOG_TAG,
                     "DirectComposition 合成初始化完成 (%ux%u, 无 CPU 读回)",
                     phys_width_, phys_height_);
    return ErrorCode::Ok;
}

// ============================================================
// valid - 设备级资源是否可用
// ============================================================

bool LayeredPresenter::valid() const {
    return d2d_context_ && swap_chain_ && dcomp_device_ && dcomp_visual_;
}

// ============================================================
// recreate_target - 释放 per-frame 引用 (下一帧 begin_frame 重建)
// ============================================================

ErrorCode LayeredPresenter::recreate_target() {
    if (d2d_context_) d2d_context_->SetTarget(nullptr);
    d2d_target_.Reset();
    back_buffer_.Reset();
    // HDR 离屏位图同样失效, 下一帧 ensure_hdr_scratch 会按新尺寸重建
    release_hdr_scratch_target();
    hdr_scratch_bmp_.Reset();
    release_backbuffer_rtvs();
    return ErrorCode::Ok;
}

// ============================================================
// resize - 尺寸变更 (ResizeBuffers 重建 swap chain)
// ============================================================

ErrorCode LayeredPresenter::resize(uint32_t width, uint32_t height) {
    if (!swap_chain_ || width == 0 || height == 0)
        return ErrorCode::InvalidArgument;
    if (width == phys_width_ && height == phys_height_)
        return ErrorCode::Ok;

    DANMAKU_LOG_INFO(LOG_TAG, "窗口尺寸变更: %ux%u → %ux%u",
                     phys_width_, phys_height_, width, height);

    // 释放所有对 back buffer 的引用 (ResizeBuffers 前必须, RTV 也算)
    if (d2d_context_) d2d_context_->SetTarget(nullptr);
    d2d_target_.Reset();
    back_buffer_.Reset();
    release_backbuffer_rtvs();

    HRESULT hr = swap_chain_->ResizeBuffers(0, width, height,
                                            DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        DANMAKU_LOG_ERROR(LOG_TAG, "ResizeBuffers 失败 (HR=0x%08X)",
                          static_cast<unsigned int>(hr));
        return ErrorCode::D3D11Failed;
    }

    phys_width_  = width;
    phys_height_ = height;

    // scratch 尺寸随之变化 → 释放, 下一帧 ensure_hdr_scratch 按需重建
    release_hdr_scratch_target();
    hdr_scratch_bmp_.Reset();
    hdr_scratch_srv_.Reset();
    hdr_scratch_tex_.Reset();
    hdr_scratch_w_ = 0;
    hdr_scratch_h_ = 0;
    return ErrorCode::Ok;
}

// ============================================================
// begin_frame / end_frame
// ============================================================

HRESULT LayeredPresenter::begin_frame() {
    if (!swap_chain_ || !d2d_context_) return E_FAIL;

    // 获取当前 back buffer (flip model 每帧轮换)
    back_buffer_.Reset();
    HRESULT hr = swap_chain_->GetBuffer(
        0, __uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(back_buffer_.GetAddressOf()));
    if (FAILED(hr)) return hr;

    // ---------------- HDR: 离屏合成路径 ----------------
    // back buffer 用 D3D11 直接清 (这一帧没有任何 D2D 绘制落在它上面,
    // 全部内容由 end_frame 的合成 pass 写入), 避免 D2D/D3D 交叉提交顺序问题。
    if (hdr_active_) {
        if (!hdr_composite_ready_ || !hdr_scratch_ctx_) return E_FAIL;
        if (ensure_hdr_scratch() != ErrorCode::Ok) return E_FAIL;

        ID3D11RenderTargetView* rtv = acquire_backbuffer_rtv(back_buffer_.Get());
        if (!rtv) return E_FAIL;
        const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        d3d_context_->ClearRenderTargetView(rtv, zero);

        hdr_scratch_ctx_->SetTarget(hdr_scratch_bmp_.Get());
        hdr_scratch_ctx_->BeginDraw();
        hdr_scratch_ctx_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        return S_OK;
    }

    // ---------------- SDR: 直画 swap chain (原路径, 完全不变) ----------------
    ComPtr<IDXGISurface> surface;
    hr = back_buffer_.As(&surface);
    if (FAILED(hr)) return hr;

    D2D1_BITMAP_PROPERTIES1 props = {};
    props.pixelFormat.format    = DXGI_FORMAT_B8G8R8A8_UNORM;
    props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    props.dpiX = 96.0f;
    props.dpiY = 96.0f;
    props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET
                        | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;

    d2d_target_.Reset();
    hr = d2d_context_->CreateBitmapFromDxgiSurface(
        surface.Get(), props, &d2d_target_);
    if (FAILED(hr)) return hr;

    d2d_context_->SetTarget(d2d_target_.Get());
    d2d_context_->BeginDraw();
    d2d_context_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    return S_OK;
}

HRESULT LayeredPresenter::end_frame() {
    if (hdr_active_) {
        if (!hdr_scratch_ctx_) return E_FAIL;
        HRESULT hr = hdr_scratch_ctx_->EndDraw();
        if (SUCCEEDED(hr)) {
            // 把 SDR 幅度的 coverage 缓冲按 hdr_peak 缩放后合成进 HDR 目标
            composite_hdr_scratch();
        }
        return hr;
    }
    if (!d2d_context_) return E_FAIL;
    return d2d_context_->EndDraw();
}

// ============================================================
// present - 提交帧: 释放引用 → 施加透明度 → Present → Commit
// ============================================================

void LayeredPresenter::present(float global_alpha) {
    if (!swap_chain_ || !dcomp_visual_) return;

    // 整层透明度记录到 global_alpha_, 供 map_color / map_color_hdr_layer
    // 在每像素 premultiplied alpha 上直接施加。
    float op = global_alpha;
    if (op < 0.0f) op = 0.0f;
    if (op > 1.0f) op = 1.0f;
    global_alpha_ = op;

    // 释放 per-frame 引用 (否则 Present 报 DXGI_ERROR_INVALID_CALL)
    if (hdr_active_) {
        if (d3d_context_) d3d_context_->OMSetRenderTargets(0, nullptr, nullptr);
        back_buffer_.Reset();
    } else {
        if (d2d_context_) d2d_context_->SetTarget(nullptr);
        d2d_target_.Reset();
        back_buffer_.Reset();
    }

    // Present(0,0): 不等待 vsync，由 DanmakuEngine 控制帧率,
    // DWM 在下一次合成周期 (显示器 vsync) 读取新帧。
    swap_chain_->Present(0, 0);
}

// ============================================================
// shutdown
// ============================================================

void LayeredPresenter::shutdown() {
    // 先释放 per-frame 引用
    if (d2d_context_) d2d_context_->SetTarget(nullptr);
    d2d_target_.Reset();
    back_buffer_.Reset();
    release_backbuffer_rtvs();

    // DComp 视觉树 (先解绑再释放)
    if (dcomp_target_ && dcomp_visual_) {
        dcomp_target_->SetRoot(nullptr);
    }
    dcomp_visual_.Reset();
    dcomp_target_.Reset();
    dcomp_device_.Reset();

    swap_chain_.Reset();
    dxgi_factory2_.Reset();

    // HDR 离屏 AA 路径
    destroy_hdr_scratch();
    hdr_brush_.Reset();
    hdr_scratch_ctx_.Reset();
    comp_vs_.Reset();
    comp_ps_.Reset();
    comp_cb_.Reset();
    comp_blend_.Reset();
    comp_sampler_.Reset();
    comp_rs_.Reset();
    hdr_composite_ready_ = false;

    color_brush_.Reset();
    stroke_style_.Reset();
    d2d_factory_.Reset();
    d2d_device_.Reset();
    d2d_context_.Reset();
    dwrite_factory_.Reset();

    d3d_context_.Reset();
    d3d_device_.Reset();

    global_alpha_ = 1.0f;
}

// ============================================================
// HDR 渲染模式
// ============================================================

ErrorCode LayeredPresenter::create_swap_chain(DXGI_FORMAT format) {
    // 获取 IDXGIFactory2 (CreateSwapChainForComposition 需要, 只取一次)
    if (!dxgi_factory2_) {
        ComPtr<IDXGIDevice> dxgi_device;
        HRESULT hr = d3d_device_.As(&dxgi_device);
        if (FAILED(hr)) return ErrorCode::D3D11Failed;

        ComPtr<IDXGIAdapter> adapter;
        hr = dxgi_device->GetAdapter(&adapter);
        if (FAILED(hr)) return ErrorCode::D3D11Failed;

        hr = adapter->GetParent(
            __uuidof(IDXGIFactory2),
            reinterpret_cast<void**>(dxgi_factory2_.GetAddressOf()));
        if (FAILED(hr)) {
            DANMAKU_LOG_ERROR(LOG_TAG, "获取 IDXGIFactory2 失败 (HR=0x%08X)",
                              static_cast<unsigned int>(hr));
            return ErrorCode::D3D11Failed;
        }
    }

    // 创建 composition swap chain (DWM 直接合成, 无 CPU 读回)
    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.Width       = phys_width_;
    scd.Height      = phys_height_;
    scd.Format      = format;
    scd.Stereo      = FALSE;
    scd.SampleDesc.Count = 1;
    scd.SampleDesc.Quality = 0;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;                              // flip model 至少 2
    scd.Scaling     = DXGI_SCALING_STRETCH;
    scd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.AlphaMode   = DXGI_ALPHA_MODE_PREMULTIPLIED;  // 透明合成
    scd.Flags       = 0;

    HRESULT hr = dxgi_factory2_->CreateSwapChainForComposition(
        d3d_device_.Get(), &scd, nullptr, swap_chain_.GetAddressOf());
    if (FAILED(hr)) {
        DANMAKU_LOG_ERROR(LOG_TAG,
                          "CreateSwapChainForComposition 失败 (HR=0x%08X)",
                          static_cast<unsigned int>(hr));
        return ErrorCode::D3D11Failed;
    }

    // 色彩空间: HDR float 用 scRGB 线性 (G10/P709, 1.0 = 系统 SDR 白点,
    //   可 >1.0), SDR 用 sRGB (G22/P709)。
    ComPtr<IDXGISwapChain3> sc3;
    if (SUCCEEDED(swap_chain_.As(&sc3))) {
        sc3->SetColorSpace1(
            format == DXGI_FORMAT_R16G16B16A16_FLOAT
                ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
                : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    }

    return ErrorCode::Ok;
}

ErrorCode LayeredPresenter::rebuild_for_hdr(bool hdr) {
    // 释放 per-frame back buffer 引用 (否则重建 swap chain 时残留引用)
    if (d2d_context_) d2d_context_->SetTarget(nullptr);
    d2d_target_.Reset();
    back_buffer_.Reset();
    release_backbuffer_rtvs();

    // HDR 需要合成 pass 与离屏上下文都就绪, 否则拒绝切换 (保持 SDR 可用)
    if (hdr && (!hdr_composite_ready_ || !hdr_scratch_ctx_)) {
        DANMAKU_LOG_ERROR(LOG_TAG,
            "HDR 合成资源未就绪 (shader=%d ctx=%d), 拒绝切换到 HDR",
            hdr_composite_ready_ ? 1 : 0, hdr_scratch_ctx_ ? 1 : 0);
        hdr = false;
    }

    // 释放旧 swap chain, 按新格式重建
    swap_chain_.Reset();
    ErrorCode ec = create_swap_chain(hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                         : DXGI_FORMAT_B8G8R8A8_UNORM);
    if (ec != ErrorCode::Ok) {
        // HDR 创建失败 → 回退 SDR, 保证弹幕仍可显示
        DANMAKU_LOG_ERROR(LOG_TAG, "HDR swap chain 创建失败, 回退 SDR");
        swap_chain_.Reset();
        ec = create_swap_chain(DXGI_FORMAT_B8G8R8A8_UNORM);
        if (ec != ErrorCode::Ok) return ec;
        hdr = false;
    }

    // 重新绑定到 DComp visual
    if (dcomp_visual_) {
        HRESULT hr = dcomp_visual_->SetContent(swap_chain_.Get());
        if (FAILED(hr)) return ErrorCode::D3D11Failed;
        dcomp_device_->Commit();
    }

    // 切到 HDR: 预建离屏 buffer; 切回 SDR: 释放它省显存
    if (hdr) {
        if (ensure_hdr_scratch() != ErrorCode::Ok) {
            DANMAKU_LOG_ERROR(LOG_TAG, "HDR 离屏缓冲创建失败, 回退 SDR");
            swap_chain_.Reset();
            if (create_swap_chain(DXGI_FORMAT_B8G8R8A8_UNORM) != ErrorCode::Ok) {
                return ErrorCode::D3D11Failed;
            }
            if (dcomp_visual_) dcomp_visual_->SetContent(swap_chain_.Get());
            hdr = false;
        }
    } else {
        destroy_hdr_scratch();
    }

    hdr_active_ = hdr;
    DANMAKU_LOG_INFO(LOG_TAG, "HDR 渲染模式: %s (峰值亮度=%.1f nits)",
                     hdr ? "开启" : "关闭", hdr_peak_);
    return ErrorCode::Ok;
}

void LayeredPresenter::request_hdr_enabled(bool enabled) {
    hdr_enabled_requested_.store(enabled, std::memory_order_relaxed);
    hdr_enabled_pending_.store(true, std::memory_order_release);
}

void LayeredPresenter::request_hdr_peak(float peak) {
    if (peak <= 0.0f) peak = 203.0f;
    hdr_peak_requested_.store(peak, std::memory_order_relaxed);
    hdr_peak_pending_.store(true, std::memory_order_release);
}

void LayeredPresenter::process_pending_hdr() {
    // 峰值亮度更新 (重建 LUT, 热改峰值不用动 swap chain)
    if (hdr_peak_pending_.exchange(false, std::memory_order_acq_rel)) {
        float peak = hdr_peak_requested_.load(std::memory_order_relaxed);
        if (peak <= 0.0f) peak = 203.0f;
        if (std::fabs(peak - hdr_peak_) > 1e-3f) {
            hdr_peak_ = peak;
        }
    }

    // HDR 开关切换 (重建 swap chain 格式)
    if (!hdr_enabled_pending_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    bool want = hdr_enabled_requested_.load(std::memory_order_relaxed);
    if (want != hdr_active_) {
        rebuild_for_hdr(want);
    }
}

D2D1_COLOR_F LayeredPresenter::map_color(uint8_t r, uint8_t g, uint8_t b,
                                         float alpha) const {
    // 整层透明度 global_alpha_ 直接乘进 alpha (premultiplied 目标下 D2D 会据此
    // 自动做 RGB*alpha 预乘, 最终写进交换链的 premultiplied 像素即 颜色*alpha*
    // global, 由 DWM 走原生 per-pixel 混合)。
    const float a = alpha * global_alpha_;
    // SDR: 恒等映射 (c/255, sRGB)
    return D2D1::ColorF(r / 255.0f, g / 255.0f, b / 255.0f, a);
}

D2D1_COLOR_F LayeredPresenter::map_color_hdr_layer(uint8_t r, uint8_t g,
                                                    uint8_t b,
                                                    float alpha) const {
    // HDR 离屏路径: **只给 SDR 幅度的线性色 (0..1)**, 不带峰值缩放。
    // 峰值 (hdr_peak_ / 80) 由 composite_hdr_scratch() 的合成 pass 统一施加。
    //
    // 这样 DirectWrite / grayscale AA 全程只处理 ≤1.0 的颜色, coverage 的
    // 生成过程与 SDR 路径完全一致; 文字的绝对亮度完全由之后的线性标量
    // 乘法决定 —— AA coverage 与 HDR 绝对亮度彻底分离。
    const float a = alpha * global_alpha_;
    return D2D1::ColorF(sdr_lin_lut_[r], sdr_lin_lut_[g], sdr_lin_lut_[b], a);
}
} // namespace danmaku_overlay

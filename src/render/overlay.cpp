#include "overlay.h"

#include <cmath>

namespace sm2m {

static void Unpack(uint32_t c, float& r, float& g, float& b, float& a) {
    r = float((c >> 24) & 0xFF) / 255.0f;
    g = float((c >> 16) & 0xFF) / 255.0f;
    b = float((c >> 8) & 0xFF) / 255.0f;
    a = float(c & 0xFF) / 255.0f;
}

void Overlay::Begin(float w, float h, const FontInfo& font) {
    verts_.clear();
    w_ = w;
    h_ = h;
    font_ = font;
    uiScale_ = h > 0 ? h / 1080.0f : 1.0f;
}

void Overlay::Tri(float x0, float y0, float x1, float y1, float x2, float y2, float u, float v, uint32_t c) {
    float r, g, b, a;
    Unpack(c, r, g, b, a);
    verts_.push_back({x0, y0, u, v, r, g, b, a});
    verts_.push_back({x1, y1, u, v, r, g, b, a});
    verts_.push_back({x2, y2, u, v, r, g, b, a});
}

void Overlay::Quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t c) {
    float r, g, b, a;
    Unpack(c, r, g, b, a);
    verts_.push_back({x0, y0, u0, v0, r, g, b, a});
    verts_.push_back({x1, y0, u1, v0, r, g, b, a});
    verts_.push_back({x1, y1, u1, v1, r, g, b, a});
    verts_.push_back({x0, y0, u0, v0, r, g, b, a});
    verts_.push_back({x1, y1, u1, v1, r, g, b, a});
    verts_.push_back({x0, y1, u0, v1, r, g, b, a});
}

void Overlay::Rect(float x, float y, float w, float h, uint32_t c) {
    Quad(x, y, x + w, y + h, font_.whiteU, font_.whiteV, font_.whiteU, font_.whiteV, c);
}

float Overlay::LineHeight(float scale) const { return float(font_.cellH) * 0.8f * scale * uiScale_; }

float Overlay::TextWidth(const std::string& s, float scale) const {
    return float(s.size()) * float(font_.cellW) * 0.8f * scale * uiScale_;
}

void Overlay::Text(float x, float y, const std::string& s, uint32_t c, float scale) {
    const float k = 0.8f * scale * uiScale_;
    const float gw = float(font_.cellW) * k, gh = float(font_.cellH) * k;
    for (char ch : s) {
        int code = static_cast<unsigned char>(ch);
        if (code >= font_.firstChar && code <= font_.lastChar && ch != ' ') {
            int idx = code - font_.firstChar;
            int col = idx % font_.columns, row = idx / font_.columns;
            float u0 = float(col * font_.cellW) / float(font_.atlasW);
            float v0 = float(row * font_.cellH) / float(font_.atlasH);
            float u1 = float((col + 1) * font_.cellW) / float(font_.atlasW);
            float v1 = float((row + 1) * font_.cellH) / float(font_.atlasH);
            Quad(x, y, x + gw, y + gh, u0, v0, u1, v1, c);
        }
        x += gw;
    }
}

void Overlay::ShadowText(float x, float y, const std::string& s, uint32_t c, float scale) {
    const float o = std::fmax(1.0f, 2.0f * uiScale_);
    Text(x + o, y + o, s, 0x000000C0u, scale);
    Text(x, y, s, c, scale);
}

void Overlay::Meter(float cx, float cy, float radius, int wedges, int filled, uint32_t fill, uint32_t empty,
                    uint32_t rim) {
    const int segPer = 6;
    const float twoPi = 6.2831853f;
    const float gap = 0.06f; // radians between wedges
    // Rim
    const int rimSegs = 48;
    for (int i = 0; i < rimSegs; ++i) {
        float a0 = twoPi * float(i) / rimSegs, a1 = twoPi * float(i + 1) / rimSegs;
        float r0 = radius * 1.12f;
        Tri(cx, cy, cx + std::sin(a0) * r0, cy - std::cos(a0) * r0, cx + std::sin(a1) * r0, cy - std::cos(a1) * r0,
            font_.whiteU, font_.whiteV, rim);
    }
    for (int w = 0; w < wedges; ++w) {
        const uint32_t col = w < filled ? fill : empty;
        const float start = twoPi * float(w) / float(wedges) + gap * 0.5f;
        const float end = twoPi * float(w + 1) / float(wedges) - gap * 0.5f;
        for (int s = 0; s < segPer; ++s) {
            float a0 = start + (end - start) * float(s) / segPer;
            float a1 = start + (end - start) * float(s + 1) / segPer;
            Tri(cx, cy, cx + std::sin(a0) * radius, cy - std::cos(a0) * radius, cx + std::sin(a1) * radius,
                cy - std::cos(a1) * radius, font_.whiteU, font_.whiteV, col);
        }
    }
}

} // namespace sm2m

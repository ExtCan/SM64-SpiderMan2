// 2D overlay geometry (HUD, toasts, debug text). Platform independent; the
// renderer supplies the font metrics and draws the vertices.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sm2m {

struct OverlayVertex {
    float x, y;       // pixels
    float u, v;
    float r, g, b, a;
};

struct FontInfo {
    int atlasW = 256, atlasH = 256;
    int cellW = 16, cellH = 30; // glyph cell in the atlas (pixels)
    int columns = 16;
    int firstChar = 32, lastChar = 126;
    float whiteU = 0, whiteV = 0; // a fully covered texel for solid fills
    float basePixelHeight = 30;   // cell height at scale 1
};

class Overlay {
public:
    void Begin(float screenW, float screenH, const FontInfo& font);
    // Scale factor used for text/HUD so the layout is resolution independent (1080p = 1).
    float UiScale() const { return uiScale_; }
    float Width() const { return w_; }
    float Height() const { return h_; }

    void Rect(float x, float y, float w, float h, uint32_t rgba);
    void Text(float x, float y, const std::string& s, uint32_t rgba, float scale = 1.0f);
    // Text with a dark drop shadow for readability over the game.
    void ShadowText(float x, float y, const std::string& s, uint32_t rgba, float scale = 1.0f);
    float TextWidth(const std::string& s, float scale = 1.0f) const;
    float LineHeight(float scale = 1.0f) const;
    // A round meter split into `wedges` slices, `filled` of them lit.
    void Meter(float cx, float cy, float radius, int wedges, int filled, uint32_t fill, uint32_t empty, uint32_t rim);

    const std::vector<OverlayVertex>& Vertices() const { return verts_; }
    void Clear() { verts_.clear(); }

private:
    void Tri(float x0, float y0, float x1, float y1, float x2, float y2, float u, float v, uint32_t rgba);
    void Quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t rgba);

    std::vector<OverlayVertex> verts_;
    FontInfo font_;
    float w_ = 1920, h_ = 1080, uiScale_ = 1;
};

} // namespace sm2m

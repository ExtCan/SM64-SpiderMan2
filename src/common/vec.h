// Small vector/matrix helpers. Game-space positions use double (the city is
// several km across); everything near the camera or Mario uses float.
#pragma once

#include <cmath>

namespace sm2m {

struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3() = default;
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    float operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
    float& operator[](int i) { return i == 0 ? x : (i == 1 ? y : z); }
};

inline float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float Length(const Vec3& v) { return std::sqrt(Dot(v, v)); }
inline Vec3 Normalize(const Vec3& v) {
    float l = Length(v);
    return l > 1e-12f ? v * (1.0f / l) : Vec3(0, 0, 0);
}
inline bool IsFinite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

struct DVec3 {
    double x = 0, y = 0, z = 0;
    DVec3() = default;
    DVec3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}
    explicit DVec3(const Vec3& v) : x(v.x), y(v.y), z(v.z) {}
    DVec3 operator+(const DVec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    DVec3 operator-(const DVec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    DVec3 operator*(double s) const { return {x * s, y * s, z * s}; }
    Vec3 ToFloat() const { return {float(x), float(y), float(z)}; }
    double operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
    double& operator[](int i) { return i == 0 ? x : (i == 1 ? y : z); }
};

inline double Dot(const DVec3& a, const DVec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline double Length(const DVec3& v) { return std::sqrt(Dot(v, v)); }
inline bool IsFinite(const DVec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Row-major 4x4 used with row vectors on the CPU (v' = v * M). Uploaded
// transposed so HLSL can use mul(M, v) with column vectors.
struct Mat4 {
    float m[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};

    static Mat4 Identity() { return Mat4(); }

    Mat4 operator*(const Mat4& b) const {
        Mat4 r;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) {
                float s = 0;
                for (int k = 0; k < 4; ++k) s += m[i][k] * b.m[k][j];
                r.m[i][j] = s;
            }
        return r;
    }

    Mat4 Transposed() const {
        Mat4 r;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) r.m[i][j] = m[j][i];
        return r;
    }

    // Transform a point (row vector).
    void TransformPoint(const float in[3], float out[4]) const {
        for (int j = 0; j < 4; ++j)
            out[j] = in[0] * m[0][j] + in[1] * m[1][j] + in[2] * m[2][j] + m[3][j];
    }
};

} // namespace sm2m

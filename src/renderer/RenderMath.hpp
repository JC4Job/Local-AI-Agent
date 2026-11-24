#pragma once

#include <cmath>

/*
 * RenderMath - the little linear algebra the renderer and the animation need: 3- and 4-vectors and 4x4
 * matrices.
 *
 * NOTE: matrices are column-major (m[column * 4 + row]) and multiply column vectors (M * v). That is the layout
 * HLSL's default cbuffer packing and GLSL / SPIR-V both read, so a backend copies a Mat4 to the GPU as it is.
 *
 * NOTE: the world is right-handed, +y up; a model faces +z, so its own left is +x.
 */

constexpr float kPi = 3.14159265358979f;

inline float DegToRad(float degrees) {
    return degrees * (kPi / 180.0f);
}

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    constexpr Vec3() = default;
    constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    Vec3  operator+(const Vec3& b) const { return {x + b.x, y + b.y, z + b.z}; }
    Vec3  operator-(const Vec3& b) const { return {x - b.x, y - b.y, z - b.z}; }
    Vec3  operator-()              const { return {-x, -y, -z}; }
    Vec3  operator*(float s)       const { return {x * s, y * s, z * s}; }
    Vec3& operator+=(const Vec3& b) {
        x += b.x;
        y += b.y;
        z += b.z;
        return *this;
    }
};

inline float Dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline float Length(const Vec3& v) {
    return std::sqrt(Dot(v, v));
}

// The zero vector stays zero, so a degenerate triangle's normal adds nothing.
inline Vec3 Normalize(const Vec3& v) {
    const float len = Length(v);
    return len > 1e-20f ? v * (1.0f / len) : Vec3();
}

inline Vec3 Min(const Vec3& a, const Vec3& b) {
    return {std::fmin(a.x, b.x), std::fmin(a.y, b.y), std::fmin(a.z, b.z)};
}

inline Vec3 Max(const Vec3& a, const Vec3& b) {
    return {std::fmax(a.x, b.x), std::fmax(a.y, b.y), std::fmax(a.z, b.z)};
}

struct Vec4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;

    constexpr Vec4() = default;
    constexpr Vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    constexpr Vec4(const Vec3& v, float w_) : x(v.x), y(v.y), z(v.z), w(w_) {}
};

struct Mat4 {
    float m[16] = {1.0f, 0.0f, 0.0f, 0.0f,
                   0.0f, 1.0f, 0.0f, 0.0f,
                   0.0f, 0.0f, 1.0f, 0.0f,
                   0.0f, 0.0f, 0.0f, 1.0f};  // identity

    static Mat4 Identity() { return Mat4(); }

    static Mat4 Translation(const Vec3& t) {
        Mat4 r;
        r.m[12] = t.x;
        r.m[13] = t.y;
        r.m[14] = t.z;
        return r;
    }

    static Mat4 Scale(const Vec3& s) {
        Mat4 r;
        r.m[0]  = s.x;
        r.m[5]  = s.y;
        r.m[10] = s.z;
        return r;
    }

    // Right-handed: a positive angle turns +y toward +z.
    static Mat4 RotationX(float radians) {
        const float c = std::cos(radians);
        const float s = std::sin(radians);
        Mat4 r;
        r.m[5]  = c;
        r.m[6]  = s;
        r.m[9]  = -s;
        r.m[10] = c;
        return r;
    }

    // A positive angle turns +z toward +x.
    static Mat4 RotationY(float radians) {
        const float c = std::cos(radians);
        const float s = std::sin(radians);
        Mat4 r;
        r.m[0]  = c;
        r.m[2]  = -s;
        r.m[8]  = s;
        r.m[10] = c;
        return r;
    }

    // A positive angle turns +x toward +y.
    static Mat4 RotationZ(float radians) {
        const float c = std::cos(radians);
        const float s = std::sin(radians);
        Mat4 r;
        r.m[0] = c;
        r.m[1] = s;
        r.m[4] = -s;
        r.m[5] = c;
        return r;
    }

    // The view from `eye` toward `target`: right-handed, the camera looks down its -z.
    static Mat4 LookAt(const Vec3& eye, const Vec3& target, const Vec3& up) {
        const Vec3 f = Normalize(target - eye);
        const Vec3 s = Normalize(Cross(f, up));
        const Vec3 u = Cross(s, f);
        Mat4 r;
        r.m[0]  = s.x;
        r.m[4]  = s.y;
        r.m[8]  = s.z;
        r.m[12] = -Dot(s, eye);
        r.m[1]  = u.x;
        r.m[5]  = u.y;
        r.m[9]  = u.z;
        r.m[13] = -Dot(u, eye);
        r.m[2]  = -f.x;
        r.m[6]  = -f.y;
        r.m[10] = -f.z;
        r.m[14] = Dot(f, eye);
        return r;
    }

    /*
     * Right-handed perspective with clip depth 0 (near) to 1 (far), the range of both Direct3D and Vulkan.
     *
     * NOTE: clip y is up. Vulkan's framebuffer y goes down, so a Vulkan backend flips it with a negative
     * viewport height (core since Vulkan 1.1) rather than with a different matrix here.
     */
    static Mat4 Perspective(float fov_y_radians, float aspect, float z_near, float z_far) {
        const float f = 1.0f / std::tan(fov_y_radians * 0.5f);
        Mat4 r;
        r.m[0]  = f / aspect;
        r.m[5]  = f;
        r.m[10] = z_far / (z_near - z_far);
        r.m[11] = -1.0f;
        r.m[14] = z_near * z_far / (z_near - z_far);
        r.m[15] = 0.0f;
        return r;
    }

    Mat4 operator*(const Mat4& b) const {
        Mat4 r;
        for (int c = 0; c < 4; ++c) {
            for (int row = 0; row < 4; ++row) {
                r.m[c * 4 + row] = m[0 * 4 + row] * b.m[c * 4 + 0] + m[1 * 4 + row] * b.m[c * 4 + 1] +
                                   m[2 * 4 + row] * b.m[c * 4 + 2] + m[3 * 4 + row] * b.m[c * 4 + 3];
            }
        }
        return r;
    }

    Vec3 TransformPoint(const Vec3& p) const {
        return {m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12],
                m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
                m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
    }
};

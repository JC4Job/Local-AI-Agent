#pragma once

#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "framework/FileSystem.hpp"
#include "renderer/RenderMath.hpp"

/*
 * The vertex every backend's surface pipeline reads: position and normal, 24 bytes.
 *
 * WARNING: the backends' input layouts (POSITION at 0, NORMAL at 12) are written against this struct.
 */
struct DrawVert {
    Vec3 xyz;
    Vec3 normal;
};
static_assert(sizeof(DrawVert) == 24, "the backends' vertex layouts expect 24-byte vertices");

struct RenderMaterial {
    std::string name;
    Vec3        diffuse   {0.80f, 0.80f, 0.80f};  // Kd, an sRGB color
    Vec3        specular  {0.06f, 0.06f, 0.06f};  // Ks
    float       shininess = 32.0f;                // Ns
};

/*
 * A part of a model: one "o" object of the .obj (or one material of it), a range of the model's triangle
 * indices. Parts are rigid: a bone moves a part whole.
 */
struct ModelPart {
    std::string name;               // the object's name
    char        side        = 'C';  // 'L' on the character's left (+x), 'R' on its right, 'C' across the middle
    int         occurrence  = 1;    // 1 for the first object of this name in the file, 2 for the second...
    int         material    = 0;    // in RenderModel::Materials()
    uint32_t    first_index = 0;
    uint32_t    index_count = 0;
    Vec3        mins;
    Vec3        maxs;

    Vec3 Center() const { return (mins + maxs) * 0.5f; }
};

/*
 * Which way a model file's axes point, in the file's own axes: the one that is up and the one the front faces.
 * The renderer's are +y up and +z front (toward the camera), so +x is the character's left; LoadOBJ turns the
 * file's into those as it reads it, before anything (bounds, sides, bones, the camera) looks at the vertices.
 *
 * NOTE: scans and CAD exports are often z up and face -y: drawn as they are, they lie face down.
 */
struct ModelAxes {
    Vec3 up    {0.0f, 1.0f, 0.0f};
    Vec3 front {0.0f, 0.0f, 1.0f};

    bool IsDefault() const { return *this == ModelAxes(); }
    // Two of the file's axes, neither the same nor opposite.
    bool IsValid()   const { return Dot(up, front) == 0.0f; }

    bool operator==(const ModelAxes& o) const {
        return up.x == o.up.x && up.y == o.up.y && up.z == o.up.z && front.x == o.front.x &&
               front.y == o.front.y && front.z == o.front.z;
    }
    bool operator!=(const ModelAxes& o) const { return !(*this == o); }

    // "x", "+y", "-z"...: one of the file's axes. False for anything else.
    static bool ParseAxis(const std::string& word, Vec3& axis) {
        std::string w = word;
        float       sign = 1.0f;
        if (!w.empty() && (w[0] == '+' || w[0] == '-')) {
            sign = w[0] == '-' ? -1.0f : 1.0f;
            w.erase(0, 1);
        }
        if (w == "x" || w == "X") {
            axis = Vec3(sign, 0.0f, 0.0f);
        } else if (w == "y" || w == "Y") {
            axis = Vec3(0.0f, sign, 0.0f);
        } else if (w == "z" || w == "Z") {
            axis = Vec3(0.0f, 0.0f, sign);
        } else {
            return false;
        }
        return true;
    }
};

/*
 * RenderModel - a static mesh in memory: one vertex array, one array of triangle indices, and the parts and
 * materials the file names. Like Doom 3's idRenderModel it knows nothing of the GPU; RenderSystem uploads it.
 *
 * LoadOBJ reads a Wavefront .obj and the .mtl it names: positions, faces (triangulated as fans), objects and
 * diffuse / specular colors. Normals are worked out here, smooth over each part, whatever the file says.
 *
 * NOTE: vertices at the same place in a part share one normal ("welded"), so a sphere's seam and poles,
 * where the file repeats a vertex, shade smooth.
 *
 * WARNING: the file's own normals (vn), texture coordinates (vt) and smoothing groups (s) are ignored: a
 * model with hard edges comes out rounded, and one with textures comes out in its diffuse colors.
 */
class RenderModel {
public:
    // False, with the reason in `error`, when the file cannot be read or holds no faces. `axes`: the file's.
    bool LoadOBJ(const std::filesystem::path& path, std::string& error, const ModelAxes& axes = ModelAxes()) {
        *this = RenderModel();
        path_ = path;
        axes_ = axes;
        std::string text;
        if (!fileSystem.ReadFile(path, text)) {
            error = "cannot read " + path.u8string();
            return false;
        }

        std::unordered_map<std::string, int> occurrences;
        std::string object     = "default";
        int         occurrence = 1;
        int         material   = -1;
        bool        new_part   = true;
        std::vector<uint32_t> face;

        const char* p   = SkipBOM(text);
        const char* end = text.data() + text.size();
        while (p < end) {
            const char* eol = p;
            while (eol < end && *eol != '\n') {
                ++eol;
            }
            const char* line_end = eol;
            if (line_end > p && line_end[-1] == '\r') {
                --line_end;
            }
            const char* s = SkipSpaces(p, line_end);
            p = eol + 1;
            if (s >= line_end || *s == '#') {
                continue;
            }
            const char* key_end = s;
            while (key_end < line_end && *key_end != ' ' && *key_end != '\t') {
                ++key_end;
            }
            const std::string key(s, key_end);
            const char*       rest = SkipSpaces(key_end, line_end);

            if (key == "v") {
                Vec3 v;
                rest = ParseFloat(rest, line_end, v.x);
                rest = ParseFloat(rest, line_end, v.y);
                ParseFloat(rest, line_end, v.z);
                verts_.push_back({v, Vec3(0.0f, 1.0f, 0.0f)});
            } else if (key == "f") {
                face.clear();
                while (rest < line_end) {
                    long index = 0;
                    if (!ParseIndex(rest, line_end, index)) {
                        break;
                    }
                    // 1-based; negative counts back from the last vertex read.
                    const long resolved = index > 0 ? index - 1 : static_cast<long>(verts_.size()) + index;
                    if (resolved < 0) {
                        error = path.filename().u8string() + ": a face uses vertex " + std::to_string(index) +
                                ", before the first one";
                        return false;
                    }
                    face.push_back(static_cast<uint32_t>(resolved));
                    while (rest < line_end && *rest != ' ' && *rest != '\t') {
                        ++rest;  // the /vt/vn after the vertex
                    }
                    rest = SkipSpaces(rest, line_end);
                }
                if (face.size() < 3) {
                    continue;
                }
                if (new_part) {
                    if (material < 0) {
                        material = FindMaterial("default");
                    }
                    ModelPart part;
                    part.name        = object;
                    part.occurrence  = occurrence;
                    part.material    = material;
                    part.first_index = static_cast<uint32_t>(indexes_.size());
                    parts_.push_back(part);
                    new_part = false;
                }
                for (size_t i = 1; i + 1 < face.size(); ++i) {
                    indexes_.push_back(face[0]);
                    indexes_.push_back(face[i]);
                    indexes_.push_back(face[i + 1]);
                }
                parts_.back().index_count = static_cast<uint32_t>(indexes_.size()) - parts_.back().first_index;
                ++face_count_;
            } else if (key == "o" || key == "g") {
                object = std::string(rest, line_end);
                while (!object.empty() && (object.back() == ' ' || object.back() == '\t')) {
                    object.pop_back();
                }
                if (object.empty()) {
                    object = "default";
                }
                occurrence = ++occurrences[object];
                new_part   = true;
            } else if (key == "usemtl") {
                material = FindMaterial(std::string(rest, line_end));
                new_part = true;
            } else if (key == "mtllib") {
                LoadMTL(path.parent_path() / std::filesystem::u8path(std::string(rest, line_end)));
            }
        }

        if (indexes_.empty()) {
            error = path.filename().u8string() + " has no faces";
            return false;
        }
        for (uint32_t index : indexes_) {
            if (index >= verts_.size()) {
                error = path.filename().u8string() + ": a face uses vertex " + std::to_string(index + 1) +
                        ", but the file has " + std::to_string(verts_.size());
                return false;
            }
        }
        if (!axes.IsDefault()) {
            // Each vertex as far along the renderer's left, up and front as it is along the file's.
            const Vec3 left = Cross(axes.up, axes.front);
            for (DrawVert& v : verts_) {
                const Vec3 p = v.xyz;
                v.xyz = Vec3(Dot(p, left), Dot(p, axes.up), Dot(p, axes.front));
            }
        }
        FinishParts();
        return true;
    }

    const std::filesystem::path&       Path()      const { return path_; }
    // How the file's axes were turned into the renderer's as it was read.
    const ModelAxes&                   Axes()      const { return axes_; }
    const std::vector<DrawVert>&       Verts()     const { return verts_; }
    const std::vector<uint32_t>&       Indexes()   const { return indexes_; }
    const std::vector<ModelPart>&      Parts()     const { return parts_; }
    const std::vector<RenderMaterial>& Materials() const { return materials_; }
    const Vec3&                        Mins()      const { return mins_; }
    const Vec3&                        Maxs()      const { return maxs_; }
    // The polygons of the file, before they were cut into triangles.
    size_t                             FaceCount() const { return face_count_; }
    // Missing .mtl files and materials it did not find there, for the user.
    const std::vector<std::string>&    Warnings()  const { return warnings_; }

private:
    std::filesystem::path       path_;
    ModelAxes                   axes_;
    std::vector<DrawVert>       verts_;
    std::vector<uint32_t>       indexes_;
    std::vector<ModelPart>      parts_;
    std::vector<RenderMaterial> materials_;
    std::vector<std::string>    warnings_;
    Vec3                        mins_;
    Vec3                        maxs_;
    size_t                      face_count_ = 0;
    bool                        mtl_read_   = false;  // a .mtl was found, so a material not in it is worth a warning

    // WARNING: some editors start UTF-8 files with a byte order mark; left in, it hides line 1's keyword.
    static const char* SkipBOM(const std::string& text) {
        return text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? text.data() + 3 : text.data();
    }

    static const char* SkipSpaces(const char* p, const char* end) {
        while (p < end && (*p == ' ' || *p == '\t')) {
            ++p;
        }
        return p;
    }

    /*
     * The number at `p`, then the spaces after it.
     *
     * NOTE: std::from_chars, not strtof: it ignores the C locale, so a decimal comma setting cannot change what
     * a file means, and it is several times faster on a model of tens of thousands of vertices.
     */
    static const char* ParseFloat(const char* p, const char* end, float& value) {
        if (p < end && *p == '+') {
            ++p;  // from_chars takes no plus sign
        }
        const std::from_chars_result r = std::from_chars(p, end, value);
        return SkipSpaces(r.ptr, end);
    }

    static bool ParseIndex(const char* p, const char* end, long& value) {
        if (p < end && *p == '+') {
            ++p;
        }
        const std::from_chars_result r = std::from_chars(p, end, value);
        return r.ec == std::errc() && value != 0;
    }

    // The material of that name, added with the default colors when the .mtl did not have it.
    int FindMaterial(const std::string& name) {
        for (size_t i = 0; i < materials_.size(); ++i) {
            if (materials_[i].name == name) {
                return static_cast<int>(i);
            }
        }
        if (mtl_read_ && name != "default") {
            warnings_.push_back("material '" + name + "' is not in the .mtl: drawn in gray");
        }
        RenderMaterial m;
        m.name = name;
        materials_.push_back(m);
        return static_cast<int>(materials_.size()) - 1;
    }

    // newmtl, Kd, Ks and Ns; everything else (textures, transparency) is ignored.
    void LoadMTL(const std::filesystem::path& path) {
        std::string text;
        if (!fileSystem.ReadFile(path, text)) {
            warnings_.push_back("cannot read " + path.filename().u8string() + ": every part is gray");
            return;
        }
        mtl_read_ = true;
        RenderMaterial* m   = nullptr;
        const char*     p   = SkipBOM(text);
        const char*     end = text.data() + text.size();
        while (p < end) {
            const char* eol = p;
            while (eol < end && *eol != '\n') {
                ++eol;
            }
            const char* line_end = eol;
            if (line_end > p && line_end[-1] == '\r') {
                --line_end;
            }
            const char* s = SkipSpaces(p, line_end);
            p = eol + 1;
            const char* key_end = s;
            while (key_end < line_end && *key_end != ' ' && *key_end != '\t') {
                ++key_end;
            }
            const std::string key(s, key_end);
            const char*       rest = SkipSpaces(key_end, line_end);
            if (key == "newmtl") {
                RenderMaterial added;
                added.name = std::string(rest, line_end);
                materials_.push_back(added);
                m = &materials_.back();
            } else if (m && (key == "Kd" || key == "Ks")) {
                Vec3 c;
                rest = ParseFloat(rest, line_end, c.x);
                rest = ParseFloat(rest, line_end, c.y);
                ParseFloat(rest, line_end, c.z);
                (key == "Kd" ? m->diffuse : m->specular) = c;
            } else if (m && key == "Ns") {
                ParseFloat(rest, line_end, m->shininess);
            }
        }
    }

    // Bounds, welded smooth normals and sides, once every part is read.
    void FinishParts() {
        struct Key {
            long long x, y, z;
            bool operator==(const Key& o) const { return x == o.x && y == o.y && z == o.z; }
        };
        struct KeyHash {
            size_t operator()(const Key& k) const {
                return static_cast<size_t>(k.x * 73856093LL ^ k.y * 19349663LL ^ k.z * 83492791LL);
            }
        };
        // NOTE: 1e-5 of a unit: the file's own repeats (seams, poles) are exact, and nothing this close is apart.
        const auto key_of = [](const Vec3& v) {
            return Key{std::llround(v.x * 1e5), std::llround(v.y * 1e5), std::llround(v.z * 1e5)};
        };

        std::unordered_map<Key, int, KeyHash> slots;
        std::vector<Vec3>                     sums;
        std::vector<int>                      slot_of(verts_.size(), -1);
        std::vector<int>                      stamp(verts_.size(), -1);  // the part slot_of was set for

        mins_ = Vec3(1e30f, 1e30f, 1e30f);
        maxs_ = Vec3(-1e30f, -1e30f, -1e30f);
        for (size_t pi = 0; pi < parts_.size(); ++pi) {
            ModelPart&     part  = parts_[pi];
            const uint32_t first = part.first_index;
            const uint32_t last  = first + part.index_count;
            part.mins = Vec3(1e30f, 1e30f, 1e30f);
            part.maxs = Vec3(-1e30f, -1e30f, -1e30f);
            slots.clear();
            sums.clear();
            for (uint32_t i = first; i < last; ++i) {
                const uint32_t v = indexes_[i];
                if (stamp[v] == static_cast<int>(pi)) {
                    continue;
                }
                stamp[v] = static_cast<int>(pi);
                const Vec3& p = verts_[v].xyz;
                part.mins = Min(part.mins, p);
                part.maxs = Max(part.maxs, p);
                const auto found = slots.emplace(key_of(p), static_cast<int>(sums.size()));
                if (found.second) {
                    sums.push_back(Vec3());
                }
                slot_of[v] = found.first->second;
            }
            // Each triangle adds its normal, as long as it is twice its area, so big faces weigh more.
            for (uint32_t i = first; i + 2 < last; i += 3) {
                const uint32_t a = indexes_[i];
                const uint32_t b = indexes_[i + 1];
                const uint32_t c = indexes_[i + 2];
                const Vec3     n = Cross(verts_[b].xyz - verts_[a].xyz, verts_[c].xyz - verts_[a].xyz);
                sums[slot_of[a]] += n;
                sums[slot_of[b]] += n;
                sums[slot_of[c]] += n;
            }
            for (uint32_t i = first; i < last; ++i) {
                const uint32_t v = indexes_[i];
                const Vec3     n = Normalize(sums[slot_of[v]]);
                verts_[v].normal = (n.x == 0.0f && n.y == 0.0f && n.z == 0.0f) ? Vec3(0.0f, 1.0f, 0.0f) : n;
            }
            mins_ = Min(mins_, part.mins);
            maxs_ = Max(maxs_, part.maxs);
        }

        const float middle = (mins_.x + maxs_.x) * 0.5f;
        const float margin = (maxs_.x - mins_.x) * 0.02f;
        for (ModelPart& part : parts_) {
            const float x = part.Center().x - middle;
            part.side = x > margin ? 'L' : x < -margin ? 'R' : 'C';
        }
    }
};

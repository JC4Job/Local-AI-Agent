#pragma once

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "framework/FileSystem.hpp"
#include "renderer/Model.hpp"
#include "renderer/RenderSystem.hpp"

/*
 * The animation of a model's parts: a rig of bones (Rig), clips read from text files (AnimClip), and the
 * Animator that plays them and blends one into the next. Like Doom 3's game/anim (idAnimator, idAnimBlend), it
 * sits above the renderer: it reads the model's parts and gives the renderer one matrix per part.
 *
 * Nothing here knows what a clip is for. The window picks the clip of an agent's life state by its key
 * (gui/AvatarPanel.hpp), so a new avatar, a new state or new motion needs only files; the formats are
 * described in data\avatar\README.txt and in the files of each agent's folder, data\avatar\<agent name>\.
 */

// The text files' small grammar: "key = value" lines, # comments, numbers with a decimal point.
namespace animtext {

inline std::string Trim(const std::string& s) {
    const size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return std::string();
    }
    const size_t last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

inline std::string Lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}

inline std::vector<std::string> Words(const std::string& s) {
    std::vector<std::string> words;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
            ++i;
        }
        const size_t start = i;
        while (i < s.size() && s[i] != ' ' && s[i] != '\t') {
            ++i;
        }
        if (i > start) {
            words.push_back(s.substr(start, i - start));
        }
    }
    return words;
}

// NOTE: std::from_chars ignores the locale: "0.5" means a half whatever the user's decimal separator.
inline bool Number(const std::string& s, float& value) {
    const char* begin = s.c_str();
    const char* end   = begin + s.size();
    if (begin < end && *begin == '+') {
        ++begin;
    }
    const std::from_chars_result r = std::from_chars(begin, end, value);
    return r.ec == std::errc() && r.ptr == end && std::isfinite(value);
}

// Three numbers from words[at].
inline bool Vector(const std::vector<std::string>& words, size_t at, Vec3& v) {
    return words.size() >= at + 3 && Number(words[at], v.x) && Number(words[at + 1], v.y) &&
           Number(words[at + 2], v.z);
}

/*
 * Calls fn(key, value, line) for each "key = value" line of `text`, the key in lower case. Blank lines and
 * lines starting with # are skipped; a line without = is passed with an empty key, for the caller to report.
 */
template <class Fn>
void ForEachSetting(const std::string& text, Fn fn) {
    int    line_no = 0;
    size_t pos     = 0;
    // WARNING: Notepad and PowerShell 5.1 start UTF-8 files with a byte order mark; left in, it hid line 1's key.
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) {
        pos = 3;
    }
    while (pos <= text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) {
            eol = text.size();
        }
        ++line_no;
        std::string line = Trim(text.substr(pos, eol - pos));
        pos = eol + 1;
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            fn(std::string(), line, line_no);
            continue;
        }
        fn(Lower(Trim(line.substr(0, eq))), Trim(line.substr(eq + 1)), line_no);
    }
}

/*
 * Sets a light or background key of lights.txt or a clip: true when `key` is one of them. A bad value leaves
 * the lights as they were and says why in `error`.
 */
inline bool LightSetting(RenderLights& lights, const std::string& key, const std::string& value, std::string& error) {
    static const struct {
        const char* key;
        Vec3 RenderLights::*field;
    } kKeys[] = {
        {"key_light",         &RenderLights::key_color},
        {"key_light_dir",     &RenderLights::key_dir},
        {"fill_light",        &RenderLights::fill_color},
        {"fill_light_dir",    &RenderLights::fill_dir},
        {"rim_light",         &RenderLights::rim_color},
        {"rim_light_dir",     &RenderLights::rim_dir},
        {"ambient",           &RenderLights::ambient},
        {"background_top",    &RenderLights::bg_top},
        {"background_bottom", &RenderLights::bg_bottom},
    };
    for (const auto& k : kKeys) {
        if (key != k.key) {
            continue;
        }
        Vec3 v;
        if (!Vector(Words(value), 0, v)) {
            error = key + " needs three numbers, as r g b or x y z";
        } else {
            lights.*k.field = v;
        }
        return true;
    }
    return false;
}

}  // namespace animtext

/*
 * What a track moves. Turns are in degrees: rx > 0 nods the bone down (its front goes down), ry > 0 turns it to
 * the character's left, rz > 0 raises its left side. Moves are in model units, along x (the character's
 * left), y (up) and z (forward). Scales multiply its size along x, y, z, or all three (s).
 */
enum class Channel { RX, RY, RZ, TX, TY, TZ, SX, SY, SZ, S, Count };

constexpr size_t      kChannelCount                = static_cast<size_t>(Channel::Count);
constexpr const char* kChannelNames[kChannelCount] = {"rx", "ry", "rz", "tx", "ty", "tz", "sx", "sy", "sz", "s"};

inline bool IsScale(Channel c) {
    return c >= Channel::SX;
}

struct Bone {
    std::string name;
    int         parent = -1;  // an earlier bone, so a bone's parent is always worked out before it
    Vec3        pivot;        // what it turns and scales about, in model space
};

/*
 * Rig - a tree of bones, and the bone that holds each part of a model.
 *
 * Built from rig.txt's "bone =" lines:  <name> <parent or -> <pivot x y z, or auto> : <parts>
 * A part is named as the .obj names its object; Name.L / Name.R / Name.C picks the one on the character's left,
 * right or middle (ModelPart::side), Name#2 the second object of that name, and * every part no other bone
 * takes. A part goes to the first bone that names it. An auto pivot is the middle of the bone's parts.
 */
class Rig {
public:
    // One bone line's value, and where it is, for the warnings.
    struct BoneLine {
        std::string value;
        int         line = 0;
    };

    // Builds the rig for `model`. What it cannot use is skipped with a warning in `warnings`.
    void Build(const std::vector<BoneLine>& lines, const RenderModel& model, const std::string& file,
               std::vector<std::string>& warnings) {
        bones_.clear();
        part_bone_.assign(model.Parts().size(), -1);
        struct Spec {
            std::vector<std::string> selectors;
            bool                     auto_pivot = false;
            int                      line       = 0;
        };
        std::vector<Spec> specs;
        for (const BoneLine& bl : lines) {
            const std::string where = file + " line " + std::to_string(bl.line) + ": ";
            const size_t      colon = bl.value.find(':');
            const std::vector<std::string> head = animtext::Words(bl.value.substr(0, colon));
            Bone bone;
            Spec spec;
            spec.line = bl.line;
            if (colon != std::string::npos) {
                spec.selectors = animtext::Words(bl.value.substr(colon + 1));
            }
            if (head.size() == 3 && animtext::Lower(head[2]) == "auto") {
                spec.auto_pivot = true;
            } else if (head.size() != 5 || !animtext::Vector(head, 2, bone.pivot)) {
                warnings.push_back(where + "a bone is: <name> <parent or -> <x y z or auto> : <parts>");
                continue;
            }
            bone.name = head[0];
            if (Find(bone.name) >= 0) {
                warnings.push_back(where + "there is already a bone named " + bone.name);
                continue;
            }
            if (head[1] != "-") {
                bone.parent = Find(head[1]);
                if (bone.parent < 0) {
                    warnings.push_back(where + "no bone named " + head[1] + " above it; " + bone.name +
                                       " has no parent");
                }
            }
            bones_.push_back(bone);
            specs.push_back(spec);
        }

        // Named parts first, in bone order, then * takes what is left.
        for (int pass = 0; pass < 2; ++pass) {
            for (size_t b = 0; b < specs.size(); ++b) {
                for (const std::string& selector : specs[b].selectors) {
                    if ((selector == "*") != (pass == 1)) {
                        continue;
                    }
                    int matched = 0;
                    for (size_t p = 0; p < part_bone_.size(); ++p) {
                        const bool all = selector == "*";
                        if (part_bone_[p] < 0 && (all || Matches(model.Parts()[p], selector))) {
                            part_bone_[p] = static_cast<int>(b);
                            ++matched;
                        } else if (!all && Matches(model.Parts()[p], selector)) {
                            ++matched;  // taken by an earlier bone: still named, so no warning
                        }
                    }
                    if (matched == 0 && selector != "*") {
                        warnings.push_back(file + " line " + std::to_string(specs[b].line) + ": no part " +
                                           selector + " in " + model.Path().filename().u8string());
                    }
                }
            }
        }

        for (size_t b = 0; b < bones_.size(); ++b) {
            if (!specs[b].auto_pivot) {
                continue;
            }
            Vec3 mins(1e30f, 1e30f, 1e30f);
            Vec3 maxs(-1e30f, -1e30f, -1e30f);
            for (size_t p = 0; p < part_bone_.size(); ++p) {
                if (part_bone_[p] == static_cast<int>(b)) {
                    mins = Min(mins, model.Parts()[p].mins);
                    maxs = Max(maxs, model.Parts()[p].maxs);
                }
            }
            if (mins.x > maxs.x) {
                warnings.push_back(file + " line " + std::to_string(specs[b].line) + ": " + bones_[b].name +
                                   " holds no part, so its auto pivot is 0 0 0");
            } else {
                bones_[b].pivot = (mins + maxs) * 0.5f;
            }
        }
    }

    const std::vector<Bone>& Bones() const { return bones_; }

    /*
     * How rig.txt names part `index` of `model` on its own: its name when no other part has it, else
     * Name.L / Name.R when its side tells it apart, else Name#n. What to write after a bone's colon.
     */
    static std::string Selector(const RenderModel& model, size_t index) {
        const std::vector<ModelPart>& parts = model.Parts();
        const ModelPart&              part  = parts[index];
        int same_name = 0;
        int same_side = 0;
        for (const ModelPart& p : parts) {
            if (p.name == part.name) {
                ++same_name;
                same_side += p.side == part.side ? 1 : 0;
            }
        }
        if (same_name == 1) {
            return part.name;
        }
        if (same_side == 1) {
            return part.name + "." + part.side;
        }
        return part.name + "#" + std::to_string(part.occurrence);
    }

    // The bone that holds the part, -1 for none: it stays as modeled.
    int PartBone(size_t part) const {
        return part < part_bone_.size() ? part_bone_[part] : -1;
    }

    int Find(const std::string& name) const {
        for (size_t i = 0; i < bones_.size(); ++i) {
            if (bones_[i].name == name) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    // The bones a track names: one by its name, or with a final *, every bone whose name starts so.
    std::vector<int> Match(const std::string& pattern) const {
        std::vector<int> found;
        if (!pattern.empty() && pattern.back() == '*') {
            const std::string prefix = pattern.substr(0, pattern.size() - 1);
            for (size_t i = 0; i < bones_.size(); ++i) {
                if (bones_[i].name.compare(0, prefix.size(), prefix) == 0) {
                    found.push_back(static_cast<int>(i));
                }
            }
        } else if (Find(pattern) >= 0) {
            found.push_back(Find(pattern));
        }
        return found;
    }

private:
    std::vector<Bone> bones_;
    std::vector<int>  part_bone_;

    // "Eye" is every object named Eye, "Eye.L" the one on the left, "Lip#2" the second Lip in the file.
    static bool Matches(const ModelPart& part, const std::string& selector) {
        if (part.name == selector) {
            return true;  // an object named "Eye.L" in the file is that one, before any rule below
        }
        const size_t dot = selector.size() >= 2 ? selector.size() - 2 : std::string::npos;
        if (dot != std::string::npos && selector[dot] == '.') {
            const char side = selector[dot + 1];
            if (side == 'L' || side == 'R' || side == 'C') {
                return part.side == side && part.name == selector.substr(0, dot);
            }
        }
        const size_t hash = selector.rfind('#');
        if (hash != std::string::npos && hash + 1 < selector.size()) {
            int n = 0;
            const char* begin = selector.c_str() + hash + 1;
            const char* end   = selector.c_str() + selector.size();
            const std::from_chars_result r = std::from_chars(begin, end, n);
            if (r.ec == std::errc() && r.ptr == end) {
                return part.occurrence == n && part.name == selector.substr(0, hash);
            }
        }
        return false;
    }
};

/*
 * One curve of a clip, on one channel of one or more bones:
 *   hold <value>                              the value, all the time
 *   wave <amplitude> <seconds> [phase] [center]
 *                                             a sine around center (0 for turns and moves, 1 for scales), one
 *                                             cycle in that many seconds, phase a share of a cycle (0 to 1)
 *   keys <seconds> [vary <share>] <t> <v> ... values at times within a loop of that many seconds, eased in and
 *                                             out between them; vary makes each loop up to that share longer
 *                                             or shorter, at random
 *
 * NOTE: the random length of a loop comes from its number and its seconds alone, so tracks with the same
 * seconds and vary stay in step: a blink's eyes and lids close together.
 */
struct AnimTrack {
    enum Kind { Hold, Wave, Keys };
    std::vector<int>   bones;
    Channel            channel   = Channel::RX;
    Kind               kind      = Hold;
    float              value     = 0.0f;  // Hold: the value; Wave: the center
    float              amplitude = 0.0f;
    float              seconds   = 1.0f;  // Wave: one cycle; Keys: one loop
    float              phase     = 0.0f;
    float              vary      = 0.0f;
    std::vector<float> times;
    std::vector<float> values;

    /*
     * The value at `time` seconds into the clip (Hold, Wave), or at `loop_phase` (0 to 1) of the loop under
     * way (Keys).
     */
    float Evaluate(float time, float loop_phase) const {
        switch (kind) {
        case Hold:
            return value;
        case Wave:
            return value + amplitude * std::sin(2.0f * kPi * (time / seconds + phase));
        case Keys:
            break;
        }
        const float  t = loop_phase * seconds;
        const size_t n = times.size();
        if (n == 1) {
            return values[0];
        }
        // The key before t and the one after, going round the loop past its end.
        size_t next = 0;
        while (next < n && times[next] <= t) {
            ++next;
        }
        const size_t prev = (next + n - 1) % n;
        next %= n;
        float t0 = times[prev];
        float t1 = times[next];
        float at = t;
        if (t1 <= t0) {
            t1 += seconds;  // from the last key round to the first
            if (at < t0) {
                at += seconds;
            }
        }
        const float u    = t1 > t0 ? std::clamp((at - t0) / (t1 - t0), 0.0f, 1.0f) : 0.0f;
        const float ease = u * u * (3.0f - 2.0f * u);
        return values[prev] + (values[next] - values[prev]) * ease;
    }
};

/*
 * AnimClip - motion and lights read from a .anim file: the lights and background to show it with (the keys of
 * lights.txt, which give what a clip leaves out), how many seconds to blend into it, and its tracks:
 *   track = <bones> <channel> <curve...>
 * where bones is a bone of the rig, or a name ending in * for every bone starting so (eye* is eye.L and eye.R),
 * and channel is one of kChannelNames. Tracks on the same channel of a bone add up; scales add their
 * difference from 1.
 */
struct AnimClip {
    std::string           key;      // the file's name without .anim: what the window asks for
    std::filesystem::path file;
    float                 blend = 0.6f;
    RenderLights          lights;
    std::vector<AnimTrack> tracks;

    /*
     * Reads `file` against `rig`; `lights` gives what the file does not set. Lines it cannot use are skipped
     * with a warning; null, with the reason in `warnings`, when the file cannot be read.
     */
    static std::shared_ptr<AnimClip> Load(const std::filesystem::path& file, const std::string& key, const Rig& rig,
                                          const RenderLights& lights, std::vector<std::string>& warnings) {
        std::string text;
        if (!fileSystem.ReadFile(file, text)) {
            warnings.push_back("cannot read " + file.u8string());
            return nullptr;
        }
        auto clip    = std::make_shared<AnimClip>();
        clip->key    = key;
        clip->file   = file;
        clip->lights = lights;
        const std::string        name = file.filename().u8string();
        std::vector<std::string> missing;  // bones the tracks name that the rig does not have
        int                      skipped = 0;
        animtext::ForEachSetting(text, [&](const std::string& k, const std::string& value, int line) {
            const std::string where = name + " line " + std::to_string(line) + ": ";
            std::string       error;
            if (k == "track") {
                AnimTrack   track;
                std::string bone;
                if (ParseTrack(value, rig, track, bone, error)) {
                    clip->tracks.push_back(std::move(track));
                } else if (!bone.empty()) {
                    ++skipped;
                    if (std::find(missing.begin(), missing.end(), bone) == missing.end()) {
                        missing.push_back(bone);
                    }
                }
            } else if (k == "blend") {
                if (!animtext::Number(value, clip->blend) || clip->blend < 0.0f) {
                    error = "blend is the seconds to blend into this clip, 0 or more";
                    clip->blend = 0.6f;
                }
            } else if (k.empty()) {
                error = "not a \"key = value\" line";
            } else if (!animtext::LightSetting(clip->lights, k, value, error)) {
                error = "unknown key " + k;
            }
            if (!error.empty()) {
                warnings.push_back(where + error);
            }
        });
        /*
         * NOTE: one line for every track a missing bone skipped, not one per track: a new model's rig without
         * the bones the clips were written for would bury everything else. The names are what to add.
         */
        if (skipped > 0) {
            std::string names;
            for (const std::string& bone : missing) {
                names += (names.empty() ? "" : ", ") + bone;
            }
            warnings.push_back(name + ": " + std::to_string(skipped) + " track(s) skipped, for bones rig.txt "
                               "does not have: " + names);
        }
        return clip;
    }

private:
    // False with `missing_bone` set when the bones are not in the rig, false with `error` set for anything else.
    static bool ParseTrack(const std::string& value, const Rig& rig, AnimTrack& track, std::string& missing_bone,
                           std::string& error) {
        const std::vector<std::string> w = animtext::Words(value);
        if (w.size() < 4) {
            error = "a track is: <bones> <channel> <hold | wave | keys> <numbers>";
            return false;
        }
        track.bones = rig.Match(w[0]);
        if (track.bones.empty()) {
            missing_bone = w[0];
            return false;
        }
        const std::string channel = animtext::Lower(w[1]);
        size_t c = 0;
        while (c < kChannelCount && channel != kChannelNames[c]) {
            ++c;
        }
        if (c == kChannelCount) {
            error = "no channel " + w[1] + "; there are rx ry rz, tx ty tz, sx sy sz and s";
            return false;
        }
        track.channel = static_cast<Channel>(c);
        const float rest = IsScale(track.channel) ? 1.0f : 0.0f;

        std::vector<float> n;
        const std::string  kind = animtext::Lower(w[2]);
        size_t             i    = 3;
        if (kind == "keys" && w.size() > 5 && animtext::Lower(w[4]) == "vary") {
            float seconds = 0.0f;
            if (!animtext::Number(w[3], seconds) || !animtext::Number(w[5], track.vary) || track.vary < 0.0f ||
                track.vary > 0.9f) {
                error = "keys <seconds> vary <0 to 0.9> <t> <v> ...";
                return false;
            }
            n.push_back(seconds);
            i = 6;
        }
        for (; i < w.size(); ++i) {
            float x = 0.0f;
            if (!animtext::Number(w[i], x)) {
                error = "'" + w[i] + "' is not a number";
                return false;
            }
            n.push_back(x);
        }

        if (kind == "hold" && n.size() == 1) {
            track.kind  = AnimTrack::Hold;
            track.value = n[0];
            return true;
        }
        if (kind == "wave" && n.size() >= 2 && n.size() <= 4 && n[1] > 0.0f) {
            track.kind      = AnimTrack::Wave;
            track.amplitude = n[0];
            track.seconds   = n[1];
            track.phase     = n.size() > 2 ? n[2] : 0.0f;
            track.value     = n.size() > 3 ? n[3] : rest;
            return true;
        }
        if (kind == "keys" && n.size() >= 3 && n.size() % 2 == 1 && n[0] > 0.0f) {
            track.kind    = AnimTrack::Keys;
            track.seconds = n[0];
            for (size_t k = 1; k < n.size(); k += 2) {
                if (n[k] < 0.0f || n[k] > track.seconds || (!track.times.empty() && n[k] < track.times.back())) {
                    error = "key times go up, from 0 to the loop's seconds";
                    return false;
                }
                track.times.push_back(n[k]);
                track.values.push_back(n[k + 1]);
            }
            return true;
        }
        error = kind == "hold" ? "hold <value>"
                : kind == "wave" ? "wave <amplitude> <seconds above 0> [phase] [center]"
                : kind == "keys" ? "keys <seconds above 0> [vary <share>] <t> <v> <t> <v> ..."
                                 : "the curve is hold, wave or keys, not " + w[2];
        return false;
    }
};

/*
 * Animator - plays clips on a rig. Play() blends the clip in over its blend seconds while the ones before
 * it fade out; Advance() moves time on; Evaluate() gives each part's matrix and the lights, as of now.
 *
 * NOTE: the clips are held by shared_ptr, so a clip still fading out survives its file being read again.
 */
class Animator {
public:
    // Forgets every clip; the next Play starts at once, with no blend. For a new rig.
    void Reset() {
        layers_.clear();
    }

    // Blends into `clip`; null blends into the rest pose. The clip already on top goes on as it is.
    void Play(std::shared_ptr<const AnimClip> clip) {
        if (!layers_.empty() && layers_.back().clip == clip) {
            return;
        }
        Layer layer;
        layer.clip = std::move(clip);
        layer.tracks.resize(layer.clip ? layer.clip->tracks.size() : 0);
        const float blend = layer.clip ? layer.clip->blend : 0.5f;
        if (layers_.empty() || blend <= 0.0f) {
            layers_.clear();
            layer.weight = 1.0f;
            layers_.push_back(std::move(layer));
            return;
        }
        // Each layer under the new one keeps its share of what the new one leaves.
        for (Layer& l : layers_) {
            l.base = l.weight;
        }
        layer.weight = 0.0f;
        layers_.push_back(std::move(layer));
        if (layers_.size() > 4) {
            const float dropped = layers_.front().base;  // WARNING: a rush of changes; its share goes to the next
            layers_.erase(layers_.begin());
            layers_.front().base += dropped;
        }
    }

    // The clip on top: playing, or blending in. Null for the rest pose, or before any Play.
    const AnimClip* Current() const {
        return layers_.empty() ? nullptr : layers_.back().clip.get();
    }

    void Advance(float seconds) {
        if (layers_.empty() || seconds <= 0.0f) {
            return;
        }
        Layer&      top   = layers_.back();
        const float blend = top.clip ? top.clip->blend : 0.5f;
        top.weight = blend > 0.0f ? std::min(1.0f, top.weight + seconds / blend) : 1.0f;
        for (size_t i = 0; i + 1 < layers_.size(); ++i) {
            layers_[i].weight = layers_[i].base * (1.0f - top.weight);
        }
        if (top.weight >= 1.0f) {
            layers_.erase(layers_.begin(), layers_.end() - 1);
        }
        for (Layer& l : layers_) {
            l.time += seconds;
            if (!l.clip) {
                continue;
            }
            for (size_t t = 0; t < l.tracks.size(); ++t) {
                const AnimTrack& track = l.clip->tracks[t];
                if (track.kind != AnimTrack::Keys) {
                    continue;
                }
                TrackState& s = l.tracks[t];
                s.phase += seconds / LoopSeconds(track, s.loop);
                while (s.phase >= 1.0f) {
                    s.phase -= 1.0f;
                    ++s.loop;
                }
            }
        }
    }

    /*
     * One matrix per part of a model with `part_count` parts, posed by the clips as of now, and the lights,
     * blended as the clips are; `rest` gives the lights of the rest pose.
     */
    void Evaluate(const Rig& rig, size_t part_count, const RenderLights& rest, std::vector<Mat4>& parts,
                  RenderLights& lights) const {
        const std::vector<Bone>& bones = rig.Bones();
        offsets_.assign(bones.size() * kChannelCount, 0.0f);

        lights = Scaled(rest, 0.0f);
        float total = 0.0f;
        for (const Layer& l : layers_) {
            if (l.weight <= 0.0f) {
                continue;
            }
            total += l.weight;
            Add(lights, l.clip ? l.clip->lights : rest, l.weight);
            if (!l.clip) {
                continue;
            }
            for (size_t t = 0; t < l.clip->tracks.size(); ++t) {
                const AnimTrack& track = l.clip->tracks[t];
                const float      v     = track.Evaluate(l.time, l.tracks[t].phase);
                const float      d     = (IsScale(track.channel) ? v - 1.0f : v) * l.weight;
                for (int b : track.bones) {
                    if (b >= 0 && static_cast<size_t>(b) < bones.size()) {
                        offsets_[b * kChannelCount + static_cast<size_t>(track.channel)] += d;
                    }
                }
            }
        }
        if (total <= 0.0f) {
            lights = rest;
        }

        /*
         * Parent before child, as Rig::Build orders them: yaw, then pitch, then roll, about the pivot.
         *
         * WARNING: scales are kept above kMinScale. Tracks add up, so a held squash plus a blink can pass 0,
         * and a negative scale would turn the part inside out.
         */
        world_.resize(bones.size());
        for (size_t b = 0; b < bones.size(); ++b) {
            const float* o = &offsets_[b * kChannelCount];
            const float  s = std::max(kMinScale, 1.0f + o[static_cast<size_t>(Channel::S)]);
            const Vec3   scale(std::max(kMinScale, 1.0f + o[static_cast<size_t>(Channel::SX)]) * s,
                               std::max(kMinScale, 1.0f + o[static_cast<size_t>(Channel::SY)]) * s,
                               std::max(kMinScale, 1.0f + o[static_cast<size_t>(Channel::SZ)]) * s);
            const Vec3   move(o[static_cast<size_t>(Channel::TX)], o[static_cast<size_t>(Channel::TY)],
                              o[static_cast<size_t>(Channel::TZ)]);
            const Mat4   local = Mat4::Translation(bones[b].pivot + move) *
                                 Mat4::RotationY(DegToRad(o[static_cast<size_t>(Channel::RY)])) *
                                 Mat4::RotationX(DegToRad(o[static_cast<size_t>(Channel::RX)])) *
                                 Mat4::RotationZ(DegToRad(o[static_cast<size_t>(Channel::RZ)])) *
                                 Mat4::Scale(scale) * Mat4::Translation(-bones[b].pivot);
            world_[b] = bones[b].parent >= 0 ? world_[bones[b].parent] * local : local;
        }
        parts.resize(part_count);
        for (size_t p = 0; p < part_count; ++p) {
            const int b = rig.PartBone(p);
            parts[p] = b >= 0 && static_cast<size_t>(b) < world_.size() ? world_[b] : Mat4::Identity();
        }
    }

private:
    struct TrackState {
        float    phase = 0.0f;  // Keys: how far into the loop under way, 0 to 1
        uint32_t loop  = 0;     // Keys: the loops done
    };

    struct Layer {
        std::shared_ptr<const AnimClip> clip;
        float                           weight = 1.0f;
        float                           base   = 1.0f;  // its weight when the layer above began to blend in
        float                           time   = 0.0f;  // seconds since it began
        std::vector<TrackState>         tracks;
    };

    static constexpr float kMinScale = 0.02f;

    std::vector<Layer>         layers_;  // oldest first; the last is the clip playing, or blending in
    mutable std::vector<float> offsets_;  // Evaluate's scratch, kept to spare an allocation each frame
    mutable std::vector<Mat4>  world_;

    // The length of loop number `loop` of a keys track: its seconds, made longer or shorter by up to vary.
    static float LoopSeconds(const AnimTrack& track, uint32_t loop) {
        if (track.vary <= 0.0f) {
            return track.seconds;
        }
        uint32_t bits = 0;
        std::memcpy(&bits, &track.seconds, sizeof(bits));
        uint32_t h = loop * 2654435761u ^ bits * 2246822519u;
        h ^= h >> 15;
        h *= 2654435761u;
        h ^= h >> 13;
        const float r = static_cast<float>(h & 0xFFFFu) / 32767.5f - 1.0f;  // -1 to 1
        return std::max(0.05f, track.seconds * (1.0f + track.vary * r));
    }

    static RenderLights Scaled(const RenderLights& l, float w) {
        RenderLights r = l;
        for (Vec3 RenderLights::*f : kLightFields) {
            r.*f = l.*f * w;
        }
        return r;
    }

    static void Add(RenderLights& sum, const RenderLights& l, float w) {
        for (Vec3 RenderLights::*f : kLightFields) {
            sum.*f += l.*f * w;
        }
    }

    static constexpr Vec3 RenderLights::*kLightFields[] = {
        &RenderLights::key_dir,  &RenderLights::key_color, &RenderLights::fill_dir,
        &RenderLights::fill_color, &RenderLights::rim_dir, &RenderLights::rim_color,
        &RenderLights::ambient,  &RenderLights::bg_top,    &RenderLights::bg_bottom,
    };
};

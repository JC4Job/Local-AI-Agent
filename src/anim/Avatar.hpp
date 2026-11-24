#pragma once

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "anim/Animator.hpp"
#include "framework/FileSystem.hpp"
#include "renderer/Model.hpp"
#include "renderer/RenderSystem.hpp"

/*
 * AvatarCamera - how the camera sees an avatar, read from the avatar's camera.txt:
 *
 *   target   = <x y z> | auto   the point it looks at; auto: the middle of the model's box
 *   distance = <units> | auto   from the target; auto: the model's whole height in view
 *   height   = <units>          of the eye above the target
 *   fov      = <degrees>        the vertical field of view, 5 to 120
 *
 * Every key is optional: one left out is auto, or the default below. The camera looks along -z, at the
 * model's front, with +y up: the avatar's own axes (model.txt turns a model given in other axes into them).
 *
 * NOTE: the auto values need the model, so Fit works them out once it is read; Setting only takes the file's.
 */
struct AvatarCamera {
    Vec3  target;
    float distance      = 6.0f;   // in model units
    float height        = 0.35f;
    float fov           = 28.0f;
    bool  auto_target   = true;
    bool  auto_distance = true;

    /*
     * Sets `key` of camera.txt from `value`: true when `key` is one of them. A bad value leaves the camera as it
     * was and says why in `error`.
     */
    bool Setting(const std::string& key, const std::string& value, std::string& error) {
        if (key == "target") {
            if (animtext::Lower(value) == "auto") {
                auto_target = true;
            } else if (animtext::Vector(animtext::Words(value), 0, target)) {
                auto_target = false;
            } else {
                error = "target needs x y z, or auto";
            }
            return true;
        }
        if (key == "distance") {
            float v = 0.0f;
            if (animtext::Lower(value) == "auto") {
                auto_distance = true;
            } else if (animtext::Number(value, v) && v > 0.0f) {
                distance      = v;
                auto_distance = false;
            } else {
                error = "distance needs a number above 0, or auto";
            }
            return true;
        }
        if (key == "height" || key == "fov") {
            float v = 0.0f;
            if (!animtext::Number(value, v)) {
                error = key + " needs a number";
            } else if (key == "height") {
                height = v;
            } else {
                fov = std::clamp(v, 5.0f, 120.0f);
            }
            return true;
        }
        return false;
    }

    // Works out what camera.txt left to auto, for `model`.
    void Fit(const RenderModel& model) {
        if (auto_target) {
            target = (model.Mins() + model.Maxs()) * 0.5f;
        }
        if (auto_distance) {
            // The whole height in view, with a little room.
            // NOTE: from the target to the model's front, not from 0: a model need not sit about the origin.
            const float half  = (model.Maxs().y - model.Mins().y) * 0.55f;
            const float front = std::max(0.0f, model.Maxs().z - target.z);
            distance          = half / std::tan(DegToRad(fov) * 0.5f) + front;
        }
    }

    /*
     * The camera's view of `model`, lit by `lights`. Its near and far planes hold every point the model can
     * reach turned about the target (the window's drag turns it there), with room for a clip's moves.
     *
     * NOTE: worked out from the model's size, not fixed: a scan in millimeters, hundreds of units across, shows
     * as well as a model a few units tall, and a small one keeps the depth buffer's precision.
     */
    RenderView View(const RenderModel& model, const RenderLights& lights) const {
        RenderView view;
        view.target        = target;
        view.eye           = target + Vec3(0.0f, height, distance);
        view.fov_y_degrees = fov;
        view.lights        = lights;
        const Vec3& lo     = model.Mins();
        const Vec3& hi     = model.Maxs();
        float       reach  = 0.0f;  // the farthest corner of the model's box from the target
        for (int corner = 0; corner < 8; ++corner) {
            const Vec3 c((corner & 1) ? hi.x : lo.x, (corner & 2) ? hi.y : lo.y, (corner & 4) ? hi.z : lo.z);
            reach = std::max(reach, Length(c - target));
        }
        reach *= 1.5f;  // a clip's moves and scales
        const float d = Length(view.eye - view.target);
        view.z_far    = d + reach;
        // WARNING: the depth buffer is 24-bit: with near much below far / 1000, close surfaces fight.
        view.z_near   = std::max(d - reach, view.z_far * 0.001f);
        return view;
    }
};

/*
 * Avatar - an animated character, as the files of its folder describe it, each file one kind of information:
 *
 *   model.txt          the model (a Wavefront .obj), its axes and its materials' shine    (needed)
 *   camera.txt         how the camera sees it (AvatarCamera)
 *   lights.txt         its lights and background, what a clip does not set
 *   rig.txt            the bones that move its parts
 *   anims\<key>.anim   its clips, one per key; the window plays the one of the agent's life state
 *
 * The window shows each agent the avatar of data\avatar\<agent name>\ (ChatAgentConfig::AvatarFolder);
 * data\avatar\README.txt tells the formats.
 *
 * Load reads them all; what it cannot use is skipped and said in `warnings`, and `error` is set only when
 * there is nothing to draw (no model.txt, no model).
 *
 * NOTE: it touches no GPU and no UI, so it loads on a worker thread (gui/AvatarPanel.hpp) and in tools.
 */
struct Avatar {
    static constexpr const char* kModelFile  = "model.txt";
    static constexpr const char* kCameraFile = "camera.txt";
    static constexpr const char* kLightsFile = "lights.txt";
    static constexpr const char* kRigFile    = "rig.txt";
    static constexpr const char* kAnimsDir   = "anims";

    std::filesystem::path                                  folder;
    std::filesystem::path                                  model_path;
    std::shared_ptr<const RenderModel>                     model;
    std::string                                            model_stamp;  // the model's files as they were read
    std::vector<RenderMaterial>                            materials;    // the model's, with model.txt's specular
    Rig                                                    rig;
    RenderLights                                           lights;       // what a clip does not set
    AvatarCamera                                           camera;
    std::map<std::string, std::shared_ptr<const AnimClip>> clips;        // by key, in lower case
    std::vector<std::string>                               warnings;
    std::string                                            error;
    std::string                                            signature;    // Signature() when it was read

    // The clip of `key`, else the "default" clip, else null (the rest pose).
    std::shared_ptr<const AnimClip> Clip(const std::string& key) const {
        auto it = clips.find(animtext::Lower(key));
        if (it == clips.end()) {
            it = clips.find("default");
        }
        return it == clips.end() ? nullptr : it->second;
    }

    // The camera's view of the model, lit by `view_lights`; see AvatarCamera::View. Only with a model.
    RenderView View(const RenderLights& view_lights) const { return camera.View(*model, view_lights); }

    static std::filesystem::path AnimsFolder(const std::filesystem::path& folder) { return folder / kAnimsDir; }

    /*
     * A string that changes when any of the avatar's files does: its .txt files, the model and the .mtl files
     * next to it, and every .anim. Cheap enough to work out every second: no file is read.
     *
     * `model_path`: the model of the avatar now read; model.txt changing covers a new one.
     */
    static std::string Signature(const std::filesystem::path& folder, const std::filesystem::path& model_path) {
        std::string s = "avatar";
        for (const char* file : {kModelFile, kCameraFile, kLightsFile, kRigFile}) {
            s += "|" + Stamp(folder / file);
        }
        if (!model_path.empty()) {
            s += "|" + ModelStamp(model_path);
        }
        std::error_code ec;
        for (std::filesystem::directory_iterator it(AnimsFolder(folder), ec), end; !ec && it != end;
             it.increment(ec)) {
            if (animtext::Lower(it->path().extension().u8string()) == ".anim") {
                s += "|" + it->path().filename().u8string() + ":" + Stamp(it->path());
            }
        }
        return s;
    }

    /*
     * Reads the avatar in `folder`. `previous` is the avatar read before: its model is taken as it is when
     * model.txt still names it and its files have not changed, so saving a clip does not read a big model again.
     */
    static std::shared_ptr<Avatar> Load(const std::filesystem::path& folder,
                                        std::shared_ptr<const Avatar> previous = nullptr) {
        auto a       = std::make_shared<Avatar>();
        a->folder    = folder;
        a->signature = Signature(folder, {});

        // --- model.txt: the one file an avatar needs ---
        std::string                              model_name;
        ModelAxes                                axes;
        std::vector<std::pair<std::string, int>> speculars;  // value and line, once the model's materials are known
        const bool has_model_file = ReadSettings(*a, kModelFile, [&](const std::string& key, const std::string& value,
                                                                    int line, std::string& error) {
            if (key == "model") {
                model_name = value;
            } else if (key == "model_up" || key == "model_front") {
                if (!ModelAxes::ParseAxis(value, key == "model_up" ? axes.up : axes.front)) {
                    error = key + " needs one of x, y, z, -x, -y, -z";
                }
            } else if (key == "specular") {
                speculars.push_back({value, line});
            } else {
                return false;
            }
            return true;
        });
        if (!has_model_file) {
            std::error_code ec;
            a->error = std::filesystem::is_directory(folder, ec)
                           ? "No " + std::string(kModelFile) + " in " + folder.u8string() +
                                 ": it names the model to show."
                           : "No avatar folder " + folder.u8string() + ": make it, with a " +
                                 std::string(kModelFile) + " naming the model (README.txt beside it tells how).";
            return a;
        }

        // --- camera.txt, lights.txt, rig.txt: each one optional ---
        ReadSettings(*a, kCameraFile, [&](const std::string& key, const std::string& value, int,
                                          std::string& error) { return a->camera.Setting(key, value, error); });
        ReadSettings(*a, kLightsFile, [&](const std::string& key, const std::string& value, int,
                                          std::string& error) {
            return animtext::LightSetting(a->lights, key, value, error);
        });
        std::vector<Rig::BoneLine> bones;
        ReadSettings(*a, kRigFile, [&](const std::string& key, const std::string& value, int line, std::string&) {
            if (key != "bone") {
                return false;
            }
            bones.push_back({value, line});
            return true;
        });

        if (model_name.empty()) {
            a->error = std::string(kModelFile) + " names no model (model = <path of an .obj>).";
            return a;
        }
        if (!axes.IsValid()) {
            a->warnings.push_back(std::string(kModelFile) + ": model_up and model_front are the same axis, or "
                                  "opposite ones: both ignored, the model is read as y up, front z");
            axes = ModelAxes();
        }
        a->model_path  = fileSystem.RelativePathToOSPath(model_name);
        a->signature   = Signature(folder, a->model_path);  // WARNING: before reading, so a change while reading is seen
        a->model_stamp = ModelStamp(a->model_path);
        if (previous && previous->model && previous->model_path == a->model_path &&
            previous->model_stamp == a->model_stamp && previous->model->Axes() == axes) {
            a->model = previous->model;
        } else {
            auto        model = std::make_shared<RenderModel>();
            std::string error;
            if (!model->LoadOBJ(a->model_path, error, axes)) {
                a->error = "Could not load the model: " + error + ".";
                return a;
            }
            a->model = model;
        }
        for (const std::string& w : a->model->Warnings()) {
            a->warnings.push_back(a->model->Path().filename().u8string() + ": " + w);
        }

        a->materials = a->model->Materials();
        for (const auto& specular : speculars) {
            ApplySpecular(*a, specular.first, std::string(kModelFile) + " line " + std::to_string(specular.second) +
                                                  ": ");
        }
        /*
         * NOTE: a model given no bones yet (a new one, before its rig is written) still shows, as one bone,
         * root, so a clip can move it whole; right-click > Model parts lists what there is to name.
         */
        if (bones.empty()) {
            bones.push_back({"root - auto : *", 0});
            a->warnings.push_back(std::string(kRigFile) + " has no bones: the whole model is one bone, root");
        }
        a->rig.Build(bones, *a->model, kRigFile, a->warnings);
        a->camera.Fit(*a->model);

        std::error_code ec;
        for (std::filesystem::directory_iterator it(AnimsFolder(folder), ec), end; !ec && it != end;
             it.increment(ec)) {
            const std::filesystem::path& file = it->path();
            if (animtext::Lower(file.extension().u8string()) != ".anim") {
                continue;
            }
            const std::string key  = animtext::Lower(file.stem().u8string());
            auto              clip = AnimClip::Load(file, key, a->rig, a->lights, a->warnings);
            if (clip) {
                a->clips[key] = clip;
            }
        }
        return a;
    }

private:
    /*
     * Reads `name` in the avatar's folder, calling fn(key, value, line, error) for each "key = value" line. fn
     * returns false for a key that is not its file's, and sets `error` for a bad value; both, and a line with no
     * =, go to the warnings. False when the file is not there.
     */
    template <class Fn>
    static bool ReadSettings(Avatar& a, const char* name, Fn fn) {
        std::string text;
        if (!fileSystem.ReadFile(a.folder / name, text)) {
            return false;
        }
        animtext::ForEachSetting(text, [&](const std::string& key, const std::string& value, int line) {
            std::string error;
            if (key.empty()) {
                error = "not a \"key = value\" line";
            } else if (!fn(key, value, line, error)) {
                error = "unknown key " + key + KeyHome(key, name);
            }
            if (!error.empty()) {
                a.warnings.push_back(std::string(name) + " line " + std::to_string(line) + ": " + error);
            }
        });
        return true;
    }

    /*
     * For a key read in the wrong file: " (it goes in camera.txt)", or empty. Takes the keys of the one file
     * avatar.txt the avatars had before, camera_fov and the like, too.
     */
    static std::string KeyHome(const std::string& key, const char* read_in) {
        std::string  error;
        AvatarCamera camera;
        RenderLights lights;
        const char*  home = nullptr;
        std::string  as   = key;
        if (key.compare(0, 7, "camera_") == 0 && camera.Setting(key.substr(7), "0", error)) {
            home = kCameraFile;
            as   = key.substr(7);
        } else if (camera.Setting(key, "0", error)) {
            home = kCameraFile;
        } else if (animtext::LightSetting(lights, key, "0 0 0", error)) {
            home = kLightsFile;
        } else if (key == "bone") {
            home = kRigFile;
        } else if (key == "model" || key == "model_up" || key == "model_front" || key == "specular") {
            home = kModelFile;
        }
        if (!home || std::string(home) == read_in) {
            return std::string();
        }
        return " (it goes in " + std::string(home) + (as == key ? "" : ", as " + as) + ")";
    }

    // The file's last write time and size, or "-" when it is not there.
    static std::string Stamp(const std::filesystem::path& file) {
        std::error_code ec;
        const auto      time = std::filesystem::last_write_time(file, ec);
        if (ec) {
            return "-";
        }
        const auto size = std::filesystem::file_size(file, ec);
        return std::to_string(time.time_since_epoch().count()) + "/" + std::to_string(ec ? 0 : size);
    }

    // The .obj and every .mtl beside it: the model's look changes with any of them.
    static std::string ModelStamp(const std::filesystem::path& model_path) {
        std::string     s = Stamp(model_path);
        std::error_code ec;
        for (std::filesystem::directory_iterator it(model_path.parent_path(), ec), end; !ec && it != end;
             it.increment(ec)) {
            if (animtext::Lower(it->path().extension().u8string()) == ".mtl") {
                s += "|" + it->path().filename().u8string() + ":" + Stamp(it->path());
            }
        }
        return s;
    }

    /*
     * "specular = <material> <strength> <shininess>": a gray specular color and its sharpness for one material.
     * `where` starts its warnings.
     */
    static void ApplySpecular(Avatar& a, const std::string& value, const std::string& where) {
        const std::vector<std::string> w = animtext::Words(value);
        float strength  = 0.0f;
        float shininess = 0.0f;
        if (w.size() != 3 || !animtext::Number(w[1], strength) || !animtext::Number(w[2], shininess)) {
            a.warnings.push_back(where + "specular = <material> <strength> <shininess>");
            return;
        }
        for (RenderMaterial& m : a.materials) {
            if (m.name == w[0]) {
                m.specular  = Vec3(strength, strength, strength);
                m.shininess = std::max(1.0f, shininess);
                return;
            }
        }
        a.warnings.push_back(where + "no material " + w[0] + " in the model");
    }
};

/*
 * avatar_render - draws an agent's avatar (data\avatar\<agent name>\) as the window does, without the window,
 * into a .bmp: to see an animation file or a camera.txt while writing it, and to check the renderer. Not
 * packaged.
 *
 *   avatar_render [--agent <name>] [--state <key>] [--time <seconds>] [--size <pixels>] [--yaw <degrees>]
 *                 [--frames <n> --step <seconds>] <out.bmp>
 *   avatar_render [--agent <name>] --parts
 *
 * --agent picks whose avatar: by default the default agent's, agent_name of config\settings.txt. It plays the
 * clip <key> (default homeostasis) for <seconds> and draws the frame, or <n> frames <step> seconds apart side by
 * side. It prints the avatar's warnings and how long each stage took.
 *
 * --parts lists what the model of model.txt is made of, to write rig.txt: each part with the name to give it in
 * a bone line, its material, size and middle, and the bone that holds it now. It needs no GPU.
 */

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "agent/AgentConfig.hpp"
#include "anim/Avatar.hpp"
#include "renderer/RenderSystem.hpp"
#include "renderer/dx11/RenderBackendDX11.hpp"

namespace {

using Clock = std::chrono::steady_clock;

double Ms(Clock::time_point since) {
    return std::chrono::duration<double, std::milli>(Clock::now() - since).count();
}

// A 24-bit bottom-up .bmp of `rgba`, width x height, rows from the top.
bool WriteBMP(const std::string& path, const std::vector<uint8_t>& rgba, int width, int height) {
    const int      row  = (width * 3 + 3) & ~3;
    const uint32_t data = static_cast<uint32_t>(row * height);
    uint8_t header[54] = {'B', 'M'};
    const auto put32 = [&header](int at, uint32_t v) {
        header[at]     = static_cast<uint8_t>(v);
        header[at + 1] = static_cast<uint8_t>(v >> 8);
        header[at + 2] = static_cast<uint8_t>(v >> 16);
        header[at + 3] = static_cast<uint8_t>(v >> 24);
    };
    put32(2, 54 + data);
    put32(10, 54);
    put32(14, 40);
    put32(18, static_cast<uint32_t>(width));
    put32(22, static_cast<uint32_t>(height));
    header[26] = 1;
    header[28] = 24;
    put32(34, data);
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(header), sizeof(header));
    std::vector<uint8_t> line(static_cast<size_t>(row), 0);
    for (int y = height - 1; y >= 0; --y) {
        for (int x = 0; x < width; ++x) {
            const uint8_t* p = &rgba[(static_cast<size_t>(y) * width + x) * 4];
            line[x * 3]     = p[2];
            line[x * 3 + 1] = p[1];
            line[x * 3 + 2] = p[0];
        }
        out.write(reinterpret_cast<const char*>(line.data()), row);
    }
    return static_cast<bool>(out);
}

}  // namespace

int main(int argc, char** argv) {
    std::string agent;
    std::string state  = "homeostasis";
    std::string output;
    float       time   = 2.0f;
    float       yaw    = 0.0f;
    float       step   = 0.5f;
    int         size   = 512;
    int         frames = 1;
    bool        parts_only = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a    = argv[i];
        const bool        more = i + 1 < argc;
        if (a == "--parts") {
            parts_only = true;
        } else if (a == "--agent" && more) {
            agent = argv[++i];
        } else if (a == "--state" && more) {
            state = argv[++i];
        } else if (a == "--time" && more) {
            time = std::strtof(argv[++i], nullptr);
        } else if (a == "--size" && more) {
            size = std::atoi(argv[++i]);
        } else if (a == "--yaw" && more) {
            yaw = std::strtof(argv[++i], nullptr);
        } else if (a == "--frames" && more) {
            frames = std::atoi(argv[++i]);
        } else if (a == "--step" && more) {
            step = std::strtof(argv[++i], nullptr);
        } else if (!a.empty() && a[0] != '-') {
            output = a;
        } else {
            output.clear();
            break;
        }
    }
    if (agent.empty()) {
        agent = ChatAgentConfig::Load().agent_name;
    }
    const std::filesystem::path folder = ChatAgentConfig::AvatarFolderOf(agent);
    std::printf("avatar of %s: %s\n", agent.c_str(), folder.u8string().c_str());

    if (parts_only) {
        const std::shared_ptr<Avatar> avatar = Avatar::Load(folder);
        if (!avatar->error.empty()) {
            std::fprintf(stderr, "%s\n", avatar->error.c_str());
            return 1;
        }
        const RenderModel& model = *avatar->model;
        std::printf("%s: %zu parts, %zu faces. Character's left is +x, up +y, front +z.\n\n",
                    model.Path().u8string().c_str(), model.Parts().size(), model.FaceCount());
        std::printf("  %-26s %-10s %9s  %-24s %s\n", "part (name in a bone)", "material", "triangles",
                    "middle (x y z)", "bone now");
        for (size_t i = 0; i < model.Parts().size(); ++i) {
            const ModelPart& p    = model.Parts()[i];
            const int        bone = avatar->rig.PartBone(i);
            const Vec3       c    = p.Center();
            char             middle[64];
            std::snprintf(middle, sizeof(middle), "%.3f %.3f %.3f", c.x, c.y, c.z);
            std::printf("  %-26s %-10s %9u  %-24s %s\n", Rig::Selector(model, i).c_str(),
                        model.Materials()[p.material].name.c_str(), p.index_count / 3, middle,
                        bone >= 0 ? avatar->rig.Bones()[bone].name.c_str() : "-");
        }
        for (const std::string& w : avatar->warnings) {
            std::printf("warning: %s\n", w.c_str());
        }
        return 0;
    }
    if (output.empty() || size < 16 || size > 4096 || frames < 1 || frames > 16) {
        std::fprintf(stderr, "usage: avatar_render [--agent <name>] [--state <key>] [--time <seconds>] "
                             "[--size <pixels>] [--yaw <degrees>] [--frames <1-16> --step <seconds>] <out.bmp>\n"
                             "       avatar_render [--agent <name>] --parts\n");
        return 2;
    }

    // A device of its own, with no window: the hardware one, else the WARP software renderer.
    ID3D11Device*        device  = nullptr;
    ID3D11DeviceContext* context = nullptr;
    const D3D_FEATURE_LEVEL levels[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                   &device, nullptr, &context);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &device,
                               nullptr, &context);
    }
    if (FAILED(hr)) {
        std::fprintf(stderr, "no Direct3D 11 device\n");
        return 1;
    }

    Clock::time_point t0 = Clock::now();
    std::string error;
    const bool ready = renderSystem.Init(std::make_unique<RenderBackendDX11>(device, context), error);
    context->Release();
    device->Release();  // the backend holds its own references
    if (!ready) {
        std::fprintf(stderr, "renderer: %s\n", error.c_str());
        return 1;
    }
    std::printf("renderer: %s, %dx MSAA, ready in %.0f ms\n", renderSystem.BackendName(), renderSystem.Samples(),
                Ms(t0));

    t0 = Clock::now();
    const std::shared_ptr<Avatar> avatar = Avatar::Load(folder);
    std::printf("avatar: loaded in %.0f ms\n", Ms(t0));
    for (const std::string& w : avatar->warnings) {
        std::printf("  warning: %s\n", w.c_str());
    }
    if (!avatar->error.empty()) {
        std::fprintf(stderr, "%s\n", avatar->error.c_str());
        return 1;
    }
    const RenderModel& model = *avatar->model;
    std::printf("model: %s, %zu parts, %zu faces, %zu triangles, %zu vertices\n",
                model.Path().filename().u8string().c_str(), model.Parts().size(), model.FaceCount(),
                model.Indexes().size() / 3, model.Verts().size());
    const AvatarCamera& cam = avatar->camera;
    std::printf("camera: target %.3f %.3f %.3f%s, distance %.3f%s, height %.3f, fov %.1f\n", cam.target.x,
                cam.target.y, cam.target.z, cam.auto_target ? " (auto)" : "", cam.distance,
                cam.auto_distance ? " (auto)" : "", cam.height, cam.fov);
    std::printf("bones: %zu; clips:", avatar->rig.Bones().size());
    for (const auto& kv : avatar->clips) {
        std::printf(" %s (%zu tracks)", kv.first.c_str(), kv.second->tracks.size());
    }
    std::printf("\n");

    t0 = Clock::now();
    const ModelHandle gpu = renderSystem.UploadModel(model);
    TargetHandle      target = renderSystem.EnsureTarget(0, size, size);
    if (!gpu || !target) {
        std::fprintf(stderr, "could not upload the model or make the target\n");
        return 1;
    }
    std::printf("upload: %.0f ms\n", Ms(t0));

    const std::shared_ptr<const AnimClip> clip = avatar->Clip(state);
    std::printf("playing: %s\n", clip ? clip->file.filename().u8string().c_str() : "(rest pose: no clip)");
    Animator animator;
    animator.Play(clip);
    animator.Advance(time);

    const Vec3           pivot = cam.target;
    std::vector<uint8_t> sheet(static_cast<size_t>(size) * frames * size * 4);
    std::vector<uint8_t> pixels;
    std::vector<Mat4>    parts;
    double               render_ms = 0.0;
    for (int f = 0; f < frames; ++f) {
        if (f > 0) {
            animator.Advance(step);
        }
        RenderLights lights;
        animator.Evaluate(avatar->rig, model.Parts().size(), avatar->lights, parts, lights);
        const RenderView view = avatar->View(lights);
        RenderEntity entity;
        entity.model           = gpu;
        entity.transform       = Mat4::Translation(pivot) * Mat4::RotationY(DegToRad(yaw)) * Mat4::Translation(-pivot);
        entity.part_transforms = &parts;
        entity.materials       = &avatar->materials;

        t0 = Clock::now();
        renderSystem.RenderScene(target, size, size, view, &entity, 1);
        if (!renderSystem.ReadPixels(target, size, size, pixels)) {
            std::fprintf(stderr, "could not read the frame back\n");
            return 1;
        }
        render_ms += Ms(t0);
        for (int y = 0; y < size; ++y) {
            std::memcpy(&sheet[(static_cast<size_t>(y) * size * frames + static_cast<size_t>(f) * size) * 4],
                        &pixels[static_cast<size_t>(y) * size * 4], static_cast<size_t>(size) * 4);
        }
    }
    std::printf("render: %d frame(s), %.1f ms each with the read-back\n", frames, render_ms / frames);

    renderSystem.Shutdown();
    if (!WriteBMP(output, sheet, size * frames, size)) {
        std::fprintf(stderr, "could not write %s\n", output.c_str());
        return 1;
    }
    std::printf("wrote %s\n", output.c_str());
    return 0;
}

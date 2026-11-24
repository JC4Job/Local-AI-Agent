#pragma once

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "renderer/Model.hpp"
#include "renderer/RenderBackend.hpp"

using ModelHandle = uint32_t;  // a model RenderSystem uploaded; 0 is none

/*
 * The lights of a view and the background behind it. Light colors are linear multiples of white, so above 1 is
 * brighter than white; directions point from the model toward the light, in world space, and need not be
 * normalized. Background colors are sRGB, top to bottom.
 *
 * NOTE: the defaults are a soft studio: a warm key light from the upper left in front, a cool fill from the
 * right, a rim light from behind and above, on the window's own purple.
 */
struct RenderLights {
    Vec3 key_dir    {-0.45f, 0.55f, 0.70f};
    Vec3 key_color  {1.10f, 1.02f, 0.95f};
    Vec3 fill_dir   {0.75f, 0.10f, 0.50f};
    Vec3 fill_color {0.28f, 0.30f, 0.38f};
    Vec3 rim_dir    {0.00f, 0.45f, -1.00f};
    Vec3 rim_color  {0.40f, 0.34f, 0.55f};
    Vec3 ambient    {0.26f, 0.24f, 0.30f};
    Vec3 bg_top     {0.17f, 0.13f, 0.24f};
    Vec3 bg_bottom  {0.07f, 0.06f, 0.11f};
};

// A camera and its lights.
struct RenderView {
    Vec3         eye;
    Vec3         target;
    Vec3         up            {0.0f, 1.0f, 0.0f};
    float        fov_y_degrees = 28.0f;
    float        z_near        = 0.1f;
    float        z_far         = 100.0f;
    RenderLights lights;
};

/*
 * A model placed in a view, like Doom 3's renderEntity_t. Each part is drawn with
 * transform * part_transforms[part]: the animation's pose, then the whole model's place.
 */
struct RenderEntity {
    ModelHandle                        model           = 0;
    Mat4                               transform;
    const std::vector<Mat4>*           part_transforms = nullptr;  // one per part; null: as modeled
    const std::vector<RenderMaterial>* materials       = nullptr;  // in place of the model's (a skin); null: its own
};

/*
 * RenderSystem - the renderer's front end, the part the rest of the program uses, like Doom 3's renderSystem.
 * It owns the backend (RenderBackend.hpp), keeps the models it uploaded and the render targets it made, and
 * turns a view of entities into backend commands. The commands of a frame pile up as the UI is laid out and
 * EndFrame submits them once, before the UI is drawn, so a view rendered in a frame shows in that frame.
 *
 * WARNING: UI thread only, like the backend.
 *
 * NOTE: everything given out (models, targets) is freed by Shutdown, and freeing a handle after it does nothing,
 * so objects that outlive the renderer, such as the tabs or a static, need no special care.
 */
class RenderSystem {
public:
    // False, with the reason (also kept for InitError), when the backend cannot draw.
    bool Init(std::unique_ptr<RenderBackend> backend, std::string& error) {
        Shutdown();
        error.clear();
        if (!backend) {
            error = "no backend";
        } else if (backend->Init(error)) {
            backend_ = std::move(backend);
        }
        init_error_ = error;
        return backend_ != nullptr;
    }

    void Shutdown() {
        if (!backend_) {
            return;
        }
        frame_.Clear();
        backend_->Shutdown();
        backend_.reset();
        models_.clear();
        targets_.clear();
    }

    bool               IsReady()     const { return backend_ != nullptr; }
    const char*        BackendName() const { return backend_ ? backend_->Name() : "none"; }
    int                Samples()     const { return backend_ ? backend_->Samples() : 0; }
    // Why Init failed; empty when it did not, or was never called.
    const std::string& InitError()   const { return init_error_; }

    // Copies the model's vertices and indexes to the GPU. 0 when it cannot.
    ModelHandle UploadModel(const RenderModel& model) {
        if (!backend_) {
            return 0;
        }
        GpuModel gpu;
        gpu.vertex_buffer = backend_->CreateBuffer(BufferUsage::Vertex, model.Verts().data(),
                                                   model.Verts().size() * sizeof(DrawVert));
        gpu.index_buffer  = backend_->CreateBuffer(BufferUsage::Index, model.Indexes().data(),
                                                   model.Indexes().size() * sizeof(uint32_t));
        if (!gpu.vertex_buffer || !gpu.index_buffer) {
            backend_->DestroyBuffer(gpu.vertex_buffer);
            backend_->DestroyBuffer(gpu.index_buffer);
            return 0;
        }
        gpu.parts     = model.Parts();
        gpu.materials = model.Materials();
        const ModelHandle handle = next_model_++;
        models_[handle] = std::move(gpu);
        return handle;
    }

    void FreeModel(ModelHandle model) {
        const auto it = models_.find(model);
        if (it == models_.end() || !backend_) {
            return;
        }
        backend_->DestroyBuffer(it->second.vertex_buffer);
        backend_->DestroyBuffer(it->second.index_buffer);
        models_.erase(it);
    }

    /*
     * A target to render width x height pixels to: `target` when it is big enough and not much bigger,
     * otherwise a new one in its place (the old is freed). 0 when none can be made.
     *
     * NOTE: made in steps of 64 pixels and drawn in its top-left corner, so dragging the window's edge does not
     * make a new target every frame; TargetUV gives the share to show.
     */
    TargetHandle EnsureTarget(TargetHandle target, int width, int height) {
        if (!backend_ || width <= 0 || height <= 0) {
            return target;
        }
        const auto it = targets_.find(target);
        if (it != targets_.end() && it->second.first >= width && it->second.second >= height &&
            it->second.first <= width * 2 + 64 && it->second.second <= height * 2 + 64) {
            return target;
        }
        FreeTarget(target);
        const int w = (width + 63) / 64 * 64;
        const int h = (height + 63) / 64 * 64;
        const TargetHandle made = backend_->CreateTarget(w, h);
        if (made) {
            targets_[made] = {w, h};
        }
        return made;
    }

    void FreeTarget(TargetHandle target) {
        const auto it = targets_.find(target);
        if (it == targets_.end() || !backend_) {
            return;
        }
        backend_->DestroyTarget(target);
        targets_.erase(it);
    }

    // What Dear ImGui draws the target with (ImTextureID); 0 for none.
    uint64_t UITexture(TargetHandle target) const {
        return backend_ ? backend_->UITexture(target) : 0;
    }

    // The bottom-right texture coordinate of a width x height view in the target.
    Vec3 TargetUV(TargetHandle target, int width, int height) const {
        const auto it = targets_.find(target);
        if (it == targets_.end()) {
            return Vec3(1.0f, 1.0f, 0.0f);
        }
        return Vec3(static_cast<float>(width) / it->second.first, static_cast<float>(height) / it->second.second,
                    0.0f);
    }

    /*
     * Records a pass that draws `view` of `entities` into the top-left width x height pixels of `target`: the
     * background, then every part of every entity. It reaches the GPU at EndFrame.
     */
    void RenderScene(TargetHandle target, int width, int height, const RenderView& view,
                     const RenderEntity* entities, size_t count) {
        if (!backend_ || targets_.find(target) == targets_.end() || width <= 0 || height <= 0) {
            return;
        }
        const RenderLights& l = view.lights;
        ViewConstants vc;
        vc.view_proj  = Mat4::Perspective(DegToRad(view.fov_y_degrees), static_cast<float>(width) / height,
                                          view.z_near, view.z_far) *
                        Mat4::LookAt(view.eye, view.target, view.up);
        vc.camera_pos = Vec4(view.eye, 1.0f);
        vc.key_dir    = Vec4(Normalize(l.key_dir), 0.0f);
        vc.key_color  = Vec4(l.key_color, 0.0f);
        vc.fill_dir   = Vec4(Normalize(l.fill_dir), 0.0f);
        vc.fill_color = Vec4(l.fill_color, 0.0f);
        vc.rim_dir    = Vec4(Normalize(l.rim_dir), 0.0f);
        vc.rim_color  = Vec4(l.rim_color, 0.0f);
        vc.ambient    = Vec4(l.ambient, 0.0f);
        vc.bg_top     = Vec4(l.bg_top, 1.0f);
        vc.bg_bottom  = Vec4(l.bg_bottom, 1.0f);

        frame_.BeginPass(target, width, height, Vec4(l.bg_bottom, 1.0f));
        frame_.SetView(vc);
        frame_.BindPipeline(PipelineKind::Background);
        frame_.Draw(3);
        frame_.BindPipeline(PipelineKind::LitSurface);
        for (size_t e = 0; e < count; ++e) {
            const RenderEntity& entity = entities[e];
            const auto          it     = models_.find(entity.model);
            if (it == models_.end()) {
                continue;
            }
            const GpuModel&                    gpu       = it->second;
            const std::vector<RenderMaterial>& materials = entity.materials ? *entity.materials : gpu.materials;
            frame_.BindGeometry(gpu.vertex_buffer, gpu.index_buffer);
            for (size_t p = 0; p < gpu.parts.size(); ++p) {
                const ModelPart& part = gpu.parts[p];
                DrawConstants    dc;
                dc.model = entity.transform;
                if (entity.part_transforms && p < entity.part_transforms->size()) {
                    dc.model = entity.transform * (*entity.part_transforms)[p];
                }
                RenderMaterial m;
                if (part.material >= 0 && static_cast<size_t>(part.material) < materials.size()) {
                    m = materials[part.material];
                }
                dc.diffuse  = Vec4(m.diffuse, 1.0f);
                dc.specular = Vec4(m.specular, m.shininess);
                frame_.DrawIndexed(dc, part.first_index, part.index_count);
            }
        }
        frame_.EndPass();
    }

    // Submits the frame's commands. Call once per frame, after the UI is laid out and before it is drawn.
    void EndFrame() {
        if (backend_ && !frame_.Empty()) {
            backend_->Submit(frame_);
        }
        frame_.Clear();
    }

    // The target's last finished frame, after submitting what is recorded; see RenderBackend::ReadPixels.
    bool ReadPixels(TargetHandle target, int width, int height, std::vector<uint8_t>& rgba) {
        EndFrame();
        return backend_ && backend_->ReadPixels(target, width, height, rgba);
    }

private:
    struct GpuModel {
        BufferHandle                vertex_buffer = 0;
        BufferHandle                index_buffer  = 0;
        std::vector<ModelPart>      parts;
        std::vector<RenderMaterial> materials;
    };

    std::unique_ptr<RenderBackend>                             backend_;
    CommandList                                                frame_;
    std::unordered_map<ModelHandle, GpuModel>                  models_;
    std::unordered_map<TargetHandle, std::pair<int, int>>      targets_;  // its size in pixels
    ModelHandle                                                next_model_ = 1;
    std::string                                                init_error_;
};

// The renderer, like Doom 3's renderSystem. Init is up to the program that has a device (gui/gui_main.cpp).
inline RenderSystem renderSystem;

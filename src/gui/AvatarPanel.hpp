#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "imgui.h"
#include "agent/AgentConfig.hpp"
#include "anim/Avatar.hpp"
#include "bio/EndocrineSystem.hpp"
#include "gui/LifePanel.hpp"
#include "gui/UiCommon.hpp"
#include "renderer/RenderSystem.hpp"
#include "sys/Sys.hpp"

namespace ui {

/*
 * AvatarSlot - the avatar of one folder (anim/Avatar.hpp), shared by the panels that show it. It is read on a
 * worker thread, its model uploaded to the GPU on the UI thread, and its files looked at every second, so a
 * file saved in the folder shows while the window is open, without a restart.
 *
 * WARNING: UI thread only, like the renderer. Only the read runs on the worker, on its own Avatar.
 */
class AvatarSlot {
public:
    explicit AvatarSlot(std::filesystem::path folder) : folder_(std::move(folder)) {}
    ~AvatarSlot() { renderSystem.FreeModel(gpu_); }

    AvatarSlot(const AvatarSlot&)            = delete;
    AvatarSlot& operator=(const AvatarSlot&) = delete;

    /*
     * Takes a finished read, or starts one when the files changed. Once per frame, before anything is drawn
     * with the avatar; every panel showing it calls it, and only the first call of a frame does anything.
     *
     * NOTE: a model left behind is freed here, before this frame records anything with it, so no recorded
     * command uses it.
     */
    void Poll() {
        const int frame = ImGui::GetFrameCount();
        if (frame == polled_frame_) {
            return;
        }
        polled_frame_ = frame;
        if (loading_.valid()) {
            if (loading_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                Adopt(loading_.get());
            }
            return;
        }
        const double now = ImGui::GetTime();
        if (!reload_ && now < next_check_) {
            return;
        }
        next_check_ = now + 1.0;
        const std::filesystem::path model = avatar_ ? avatar_->model_path : std::filesystem::path();
        if (reload_ || Avatar::Signature(folder_, model) != signature_) {
            reload_  = false;
            loading_ = std::async(std::launch::async, [folder = folder_, previous = avatar_] {
                try {
                    return Avatar::Load(folder, previous);
                } catch (const std::exception& e) {
                    auto failed   = std::make_shared<Avatar>();
                    failed->error = std::string("Could not read the avatar: ") + e.what();
                    return failed;
                }
            });
        }
    }

    // Reads the files again at the next Poll, changed or not.
    void Reload() { reload_ = true; }

    // Null until the first read ends.
    const std::shared_ptr<const Avatar>& Current()    const { return avatar_; }
    ModelHandle                          GpuModel()   const { return gpu_; }
    // Goes up with each avatar read, so a panel knows to start its animation over.
    int                                  Generation() const { return generation_; }
    bool                                 Loading()    const { return loading_.valid(); }
    const std::filesystem::path&         Folder()     const { return folder_; }

private:
    std::filesystem::path                folder_;
    std::shared_ptr<const Avatar>        avatar_;
    std::shared_ptr<const RenderModel>   gpu_model_;  // what gpu_ holds
    ModelHandle                          gpu_          = 0;
    std::future<std::shared_ptr<Avatar>> loading_;
    std::string                          signature_;
    double                               next_check_   = 0.0;
    int                                  generation_   = 0;
    int                                  polled_frame_ = -1;
    bool                                 reload_       = false;

    void Adopt(std::shared_ptr<Avatar> a) {
        signature_ = a->signature;
        if (a->error.empty() && a->model != gpu_model_) {
            const ModelHandle uploaded = renderSystem.UploadModel(*a->model);
            if (uploaded) {
                renderSystem.FreeModel(gpu_);
                gpu_       = uploaded;
                gpu_model_ = a->model;
            } else {
                a->error = renderSystem.IsReady() ? "The GPU did not take the model."
                                                  : "No renderer: " + renderSystem.InitError() + ".";
            }
        }
        avatar_ = std::move(a);
        ++generation_;
    }
};

/*
 * AvatarLibrary - the avatars the window shows, one AvatarSlot per folder: each agent's own
 * (ChatAgentConfig::AvatarFolder), read and uploaded once however many panels show it.
 *
 * WARNING: UI thread only, like the renderer.
 */
class AvatarLibrary {
public:
    static AvatarLibrary& Get() {
        static AvatarLibrary library;
        return library;
    }

    // The avatar of `folder`, read at its first Poll.
    std::shared_ptr<AvatarSlot> Slot(const std::filesystem::path& folder) {
        std::shared_ptr<AvatarSlot>& slot = slots_[folder.lexically_normal()];
        if (!slot) {
            slot = std::make_shared<AvatarSlot>(folder.lexically_normal());
        }
        return slot;
    }

    /*
     * Lets go of the avatars no panel shows any more (a deleted agent's), and of their models. Once per frame,
     * before any panel draws.
     *
     * NOTE: here, not when the last panel goes: a panel goes after it drew, and the frame it drew is not
     * submitted yet. One still being read waits: letting go of it would wait for the read.
     */
    void Sweep() {
        const int frame = ImGui::GetFrameCount();
        if (frame == swept_frame_) {
            return;
        }
        swept_frame_ = frame;
        for (auto it = slots_.begin(); it != slots_.end();) {
            if (it->second.use_count() == 1 && !it->second->Loading()) {
                it = slots_.erase(it);
            } else {
                ++it;
            }
        }
    }

private:
    std::map<std::filesystem::path, std::shared_ptr<AvatarSlot>> slots_;
    int                                                          swept_frame_ = -1;

    AvatarLibrary() = default;
};

/*
 * AvatarPanel - one agent's avatar, drawn in a square beside its conversation (AgentTab), with a line under it.
 *
 * It shows the avatar of the agent's folder, data\avatar\<agent name>\, and plays the clip whose key is the
 * agent's life state (bio/EndocrineSystem.hpp: homeostasis, fight_or_flight, ...), blending into the next one
 * when the state changes, so new animations, or a new model, are only files: anims\<state key>.anim and
 * model.txt, camera.txt, lights.txt, rig.txt in that folder. Dragging turns the model, double-click faces it
 * again, and right-click opens the menu: preview any clip, import a file as a state's animation, list the
 * model's parts (what rig.txt's bones can name), open the folder, read the files again.
 *
 * NOTE: each panel has its own animation and render target, so tabs never share a half-blended pose; only the
 * shown tab draws, so only one renders a frame.
 */
class AvatarPanel {
public:
    // Shows the avatar of `folder`: ChatAgentConfig::AvatarFolder.
    explicit AvatarPanel(const std::filesystem::path& folder) : slot_(AvatarLibrary::Get().Slot(folder)) {}
    ~AvatarPanel() { renderSystem.FreeTarget(target_); }

    AvatarPanel(const AvatarPanel&)            = delete;
    AvatarPanel& operator=(const AvatarPanel&) = delete;

    /*
     * The side of the square beside a conversation `avail_w` wide and `height` tall, its line under it
     * included: at most 40% of the width. 0 when it would be too small to see: the conversation takes it all.
     */
    static float Side(float avail_w, float height) {
        const float side = std::min(avail_w * 0.4f, height - ImGui::GetTextLineHeightWithSpacing());
        return side >= ImGui::GetFontSize() * 7.0f ? std::floor(side) : 0.0f;
    }

    // Draws the square, `side` pixels, and its line, at the cursor, for an agent in `state`.
    void Draw(float side, LifeState state) {
        AvatarLibrary::Get().Sweep();
        AvatarSlot& slot = *slot_;
        slot.Poll();
        if (!pending_import_.empty()) {
            const std::string key = pending_import_;
            pending_import_.clear();
            Import(key);  // WARNING: here, out of the menu: the Open dialog stops the frames until it closes
        }
        const std::shared_ptr<const Avatar> avatar = slot.Current();
        if (generation_ != slot.Generation()) {
            generation_ = slot.Generation();
            animator_.Reset();
        }
        const std::string                     key  = preview_.empty() ? Info(state).key : preview_;
        const std::shared_ptr<const AnimClip> clip = avatar ? avatar->Clip(key) : nullptr;

        ImGui::BeginGroup();
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const ImVec2 p1(p0.x + side, p0.y + side);
        ImGui::InvisibleButton("##avatar", ImVec2(side, side));
        // WARNING: read now: the menu's popup, drawn next, has items of its own.
        const bool hovered = ImGui::IsItemHovered();
        const bool active  = ImGui::IsItemActive();
        if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
            const ImVec2 d = ImGui::GetIO().MouseDelta;
            yaw_   = std::fmod(yaw_ + d.x * 0.4f, 360.0f);
            pitch_ = std::clamp(pitch_ + d.y * 0.25f, -30.0f, 30.0f);
        }
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            yaw_   = 0.0f;
            pitch_ = 0.0f;
        }
        DrawMenu(avatar.get());
        if (hovered && !active && !ImGui::IsPopupOpen("##avatar_menu")) {
            DrawTooltip(avatar.get(), clip.get(), key);
        }

        ImDrawList* dl       = ImGui::GetWindowDrawList();
        const float rounding = ImGui::GetStyle().ChildRounding;
        std::string why_empty;
        if (!renderSystem.IsReady()) {
            why_empty = "No renderer: " + renderSystem.InitError();
        } else if (!avatar) {
            why_empty = "Reading the avatar...";
        } else if (!avatar->error.empty()) {
            why_empty = avatar->error;
        } else if (!Render(*avatar, clip, slot.GpuModel(), side, p0, p1, rounding)) {
            why_empty = "Could not make a render target.";
        }
        if (!why_empty.empty()) {
            const float pad = ImGui::GetStyle().FramePadding.x;
            dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImGuiCol_FrameBg), rounding);
            dl->AddText(nullptr, 0.0f, ImVec2(p0.x + pad, p0.y + pad), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                        why_empty.c_str(), nullptr, side - pad * 2.0f);
        }
        DrawMessage(p0, p1, side);
        dl->AddRect(p0, p1, ImGui::GetColorU32(ImGuiCol_Border), rounding);

        DrawCaption(avatar.get(), state, side);
        ImGui::EndGroup();
    }

private:
    std::shared_ptr<AvatarSlot> slot_;              // never null
    Animator                    animator_;
    std::vector<Mat4>           parts_;
    TargetHandle                target_     = 0;
    int                         generation_ = -1;
    float                       yaw_        = 0.0f;  // degrees, from dragging
    float                       pitch_      = 0.0f;
    std::string                 preview_;            // the clip key picked in the menu; empty: the agent's state
    std::string                 pending_import_;     // the key Import animation for was picked for, until next frame
    std::string                 message_;            // what the last import did, shown over the square for a while
    bool                        message_error_ = false;
    double                      message_until_ = 0.0;

    // Poses the model, renders it, and puts the picture in the square. False when there is no target.
    bool Render(const Avatar& avatar, const std::shared_ptr<const AnimClip>& clip, ModelHandle gpu, float side,
                const ImVec2& p0, const ImVec2& p1, float rounding) {
        animator_.Play(clip);  // the clip already playing goes on
        animator_.Advance(ImGui::GetIO().DeltaTime);
        RenderLights lights;
        animator_.Evaluate(avatar.rig, avatar.model->Parts().size(), avatar.lights, parts_, lights);

        const int px = std::max(16, static_cast<int>(side));
        target_ = renderSystem.EnsureTarget(target_, px, px);
        if (!target_) {
            return false;
        }
        const AvatarCamera& cam  = avatar.camera;
        const RenderView    view = avatar.View(lights);
        // Turned about the point the camera looks at, so the lights stay where they are, as in a studio.
        RenderEntity entity;
        entity.model           = gpu;
        entity.transform       = Mat4::Translation(cam.target) * Mat4::RotationY(DegToRad(yaw_)) *
                                 Mat4::RotationX(DegToRad(pitch_)) * Mat4::Translation(-cam.target);
        entity.part_transforms = &parts_;
        entity.materials       = &avatar.materials;
        renderSystem.RenderScene(target_, px, px, view, &entity, 1);

        const Vec3 uv = renderSystem.TargetUV(target_, px, px);
        ImGui::GetWindowDrawList()->AddImageRounded(static_cast<ImTextureID>(renderSystem.UITexture(target_)), p0,
                                                    p1, ImVec2(0.0f, 0.0f), ImVec2(uv.x, uv.y), IM_COL32_WHITE,
                                                    rounding);
        return true;
    }

    void DrawTooltip(const Avatar* avatar, const AnimClip* clip, const std::string& key) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
        if (avatar && avatar->model) {
            const RenderModel& m = *avatar->model;
            ImGui::Text("%s: %zu parts, %zu faces, %zu bones", m.Path().filename().u8string().c_str(),
                        m.Parts().size(), m.FaceCount(), avatar->rig.Bones().size());
            ImGui::TextDisabled("Drawn with %s, %dx multisampling.", renderSystem.BackendName(),
                                renderSystem.Samples());
        }
        if (clip) {
            ImGui::Text("Playing %s%s.", clip->file.filename().u8string().c_str(),
                        clip->key == key ? "" : " (no file of its own)");
        } else {
            ImGui::Text("No %s.anim and no default.anim: it stands still.", key.c_str());
        }
        ImGui::Spacing();
        ImGui::TextDisabled("Drag to turn it; double-click to face it again. Right-click to preview a state, "
                            "import an animation or list the model's parts.");
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    /*
     * The state shown, in its color (or the clip previewed), and how many warnings the files gave, listed on
     * hover. Clipped to the square's width; the file playing is in the square's tooltip.
     */
    void DrawCaption(const Avatar* avatar, LifeState state, float side) {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::PushClipRect(at, ImVec2(at.x + side, at.y + ImGui::GetTextLineHeightWithSpacing()), true);
        if (preview_.empty()) {
            ImGui::TextColored(LifeStateColor(state), "%s", Info(state).name);
        } else {
            ImGui::TextColored(kBusyColor, "Preview: %s", KeyLabel(preview_).c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Right-click > Preview > The agent's feeling, to follow %s again.",
                                  Info(state).name);
            }
        }
        const bool warn = avatar && !avatar->warnings.empty();
        if (warn) {
            ImGui::SameLine();
            ImGui::TextColored(kBusyColor, "%zu warning(s)", avatar->warnings.size());
        }
        const bool hovered = warn && ImGui::IsItemHovered();
        ImGui::PopClipRect();
        if (!hovered) {
            return;
        }
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
        ImGui::Text("What the avatar's files have that could not be used (%s):",
                    ShortPath(slot_->Folder().u8string()).c_str());
        const size_t shown = std::min<size_t>(avatar->warnings.size(), 25);
        for (size_t i = 0; i < shown; ++i) {
            ImGui::BulletText("%s", avatar->warnings[i].c_str());
        }
        if (shown < avatar->warnings.size()) {
            ImGui::TextDisabled("... and %zu more.", avatar->warnings.size() - shown);
        }
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    // The last import's result, over the bottom of the square, for a few seconds.
    void DrawMessage(const ImVec2& p0, const ImVec2& p1, float side) {
        if (message_.empty() || ImGui::GetTime() > message_until_) {
            return;
        }
        const float  pad  = ImGui::GetStyle().FramePadding.x;
        const float  wrap = side - pad * 2.0f;
        const ImVec2 size = ImGui::CalcTextSize(message_.c_str(), nullptr, false, wrap);
        const ImVec2 top(p0.x, std::max(p0.y, p1.y - size.y - pad * 2.0f));
        ImDrawList*  dl   = ImGui::GetWindowDrawList();
        dl->AddRectFilled(top, p1, IM_COL32(0, 0, 0, 170), ImGui::GetStyle().ChildRounding,
                          ImDrawFlags_RoundCornersBottom);
        dl->AddText(nullptr, 0.0f, ImVec2(top.x + pad, top.y + pad),
                    ImGui::GetColorU32(message_error_ ? kErrorColor : kBusyColor), message_.c_str(), nullptr, wrap);
    }

    void Say(std::string text, bool error) {
        message_       = std::move(text);
        message_error_ = error;
        message_until_ = ImGui::GetTime() + 10.0;
    }

    void DrawMenu(const Avatar* avatar) {
        if (!ImGui::BeginPopupContextItem("##avatar_menu")) {
            return;
        }
        ImGui::TextDisabled("Avatar, from %s", ShortPath(slot_->Folder().u8string()).c_str());
        ImGui::Separator();

        if (ImGui::BeginMenu("Preview")) {
            if (ImGui::MenuItem("The agent's feeling", nullptr, preview_.empty())) {
                preview_.clear();
            }
            ImGui::Separator();
            for (const LifeStateInfo& info : kLifeStateInfo) {
                const bool        has   = avatar && avatar->clips.count(info.key) > 0;
                const std::string label = std::string(info.name) + (has ? "" : "  (no file)");
                if (ImGui::MenuItem(label.c_str(), nullptr, preview_ == info.key)) {
                    preview_ = info.key;
                }
            }
            // Clips no state has yet: ready for one to come.
            bool separated = false;
            if (avatar) {
                for (const auto& kv : avatar->clips) {
                    if (IsStateKey(kv.first)) {
                        continue;
                    }
                    if (!separated) {
                        ImGui::Separator();
                        separated = true;
                    }
                    if (ImGui::MenuItem(kv.first.c_str(), nullptr, preview_ == kv.first)) {
                        preview_ = kv.first;
                    }
                }
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Import animation for", avatar && avatar->model)) {
            for (const LifeStateInfo& info : kLifeStateInfo) {
                if (ImGui::MenuItem(info.name)) {
                    pending_import_ = info.key;
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Any state without its own (default)")) {
                pending_import_ = "default";
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Model parts", avatar && avatar->model)) {
            DrawParts(*avatar);
            ImGui::EndMenu();
        }

        ImGui::Separator();
        if (ImGui::MenuItem("Open the avatar folder")) {
            // NOTE: made when it is not there yet: an agent without an avatar gets its folder to fill.
            std::error_code ec;
            std::filesystem::create_directories(slot_->Folder(), ec);
            if (!Sys_OpenFolder(slot_->Folder())) {
                Say("Windows could not open " + slot_->Folder().u8string(), true);
            }
        }
        if (ImGui::MenuItem("Read the files again")) {
            slot_->Reload();
        }
        if (ImGui::MenuItem("Face it again", nullptr, false, yaw_ != 0.0f || pitch_ != 0.0f)) {
            yaw_   = 0.0f;
            pitch_ = 0.0f;
        }
        ImGui::EndPopup();
    }

    /*
     * What the model is made of: each part by the name a bone line gives it (Rig::Selector), its material, its
     * size, its middle and the bone that holds it now. Copy puts the list on the clipboard, to write the bones
     * of a new model from.
     */
    void DrawParts(const Avatar& avatar) {
        const RenderModel& model = *avatar.model;
        if (ImGui::MenuItem("Copy the list")) {
            std::string text = "# " + model.Path().filename().u8string() + ": part, material, triangles, middle "
                               "(x y z), bone now\n";
            for (size_t i = 0; i < model.Parts().size(); ++i) {
                text += PartLine(avatar, i) + "\n";
            }
            ImGui::SetClipboardText(text.c_str());
        }
        ImGui::TextDisabled("Character's left is +x, up +y, front +z. Name a part this way after a bone's colon.");
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit |
                                      ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV;
        const float height = std::min(ImGui::GetTextLineHeightWithSpacing() * (model.Parts().size() + 1.5f),
                                      ImGui::GetMainViewport()->WorkSize.y * 0.6f);
        if (!ImGui::BeginTable("##parts", 5, flags, ImVec2(0.0f, height))) {
            return;
        }
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Part");
        ImGui::TableSetupColumn("Material");
        ImGui::TableSetupColumn("Triangles");
        ImGui::TableSetupColumn("Middle (x y z)");
        ImGui::TableSetupColumn("Bone now");
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < model.Parts().size(); ++i) {
            const ModelPart& p    = model.Parts()[i];
            const int        bone = avatar.rig.PartBone(i);
            const Vec3       c    = p.Center();
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(Rig::Selector(model, i).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(model.Materials()[p.material].name.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%u", p.index_count / 3);
            ImGui::TableNextColumn();
            ImGui::Text("%.2f %.2f %.2f", c.x, c.y, c.z);
            ImGui::TableNextColumn();
            if (bone >= 0) {
                ImGui::TextUnformatted(avatar.rig.Bones()[bone].name.c_str());
            } else {
                ImGui::TextDisabled("-");
            }
        }
        ImGui::EndTable();
    }

    static std::string PartLine(const Avatar& avatar, size_t i) {
        const RenderModel& model = *avatar.model;
        const ModelPart&   p     = model.Parts()[i];
        const int          bone  = avatar.rig.PartBone(i);
        const Vec3         c     = p.Center();
        char               line[256];
        std::snprintf(line, sizeof(line), "%-24s %-10s %7u  %6.3f %6.3f %6.3f  %s", Rig::Selector(model, i).c_str(),
                      model.Materials()[p.material].name.c_str(), p.index_count / 3, c.x, c.y, c.z,
                      bone >= 0 ? avatar.rig.Bones()[bone].name.c_str() : "-");
        return line;
    }

    static bool IsStateKey(const std::string& key) {
        for (const LifeStateInfo& info : kLifeStateInfo) {
            if (key == info.key) {
                return true;
            }
        }
        return false;
    }

    static std::string KeyLabel(const std::string& key) {
        for (const LifeStateInfo& info : kLifeStateInfo) {
            if (key == info.key) {
                return info.name;
            }
        }
        return key == "default" ? "any state without its own" : key;
    }

    /*
     * Asks for a file and makes it the animation of `key`: anims\<key>.anim in the avatar's folder. It is read
     * against the avatar's bones first, so a file the avatar cannot play replaces nothing; the file it replaces
     * goes to config\backup\<date and time>\avatar\<agent's folder name>\.
     */
    void Import(const std::string& key) {
        namespace fs = std::filesystem;
        AvatarSlot&                         slot   = *slot_;
        const std::shared_ptr<const Avatar> avatar = slot.Current();
        if (!avatar || !avatar->model) {
            Say("Not imported: there is no model to check the file against.", true);
            return;
        }
        const std::string  label  = KeyLabel(key);
        const std::wstring title  = fs::u8path("Animation for " + label).wstring();
        const HWND         owner  = static_cast<HWND>(ImGui::GetMainViewport()->PlatformHandleRaw);
        const fs::path     chosen = Sys_OpenFileDialog(owner, title.c_str(),
                                                       L"Animations (*.anim)\0*.anim;*.txt\0All files\0*.*\0");
        if (chosen.empty()) {
            return;
        }
        std::vector<std::string>  warnings;
        std::shared_ptr<AnimClip> clip = AnimClip::Load(chosen, key, avatar->rig, avatar->lights, warnings);
        if (!clip) {
            Say("Not imported: " + (warnings.empty() ? std::string("cannot read it") : warnings.front()), true);
            return;
        }
        if (clip->tracks.empty() && !warnings.empty()) {
            Say("Not imported: nothing in " + chosen.filename().u8string() + " the avatar can play. " +
                warnings.front(), true);
            return;
        }
        const fs::path  dest = Avatar::AnimsFolder(slot.Folder()) / fs::u8path(key + ".anim");
        std::error_code ec;
        if (fs::equivalent(chosen, dest, ec)) {
            Say("That file is already the animation for " + label + ".", false);
            return;
        }
        const std::string backup_folder = (fs::u8path("avatar") / slot.Folder().filename()).u8string();
        const std::string backup        = ChatAgentConfig::MoveToBackup(dest, backup_folder.c_str());
        if (!backup.empty()) {
            Say("Not imported: " + backup + ".", true);
            return;
        }
        fs::create_directories(dest.parent_path(), ec);
        if (!fs::copy_file(chosen, dest, fs::copy_options::overwrite_existing, ec)) {
            Say("Not imported: could not copy it to " + dest.u8string() + (ec ? ": " + ec.message() : ""), true);
            return;
        }
        Say("Imported " + chosen.filename().u8string() + " for " + label +
            (warnings.empty() ? "." : ", with " + std::to_string(warnings.size()) + " warning(s): hover the line "
                                      "under the avatar."), !warnings.empty());
        preview_ = key == "default" ? std::string() : key;  // show what was imported
        slot.Reload();
    }
};

}  // namespace ui

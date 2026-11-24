#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "renderer/RenderMath.hpp"

/*
 * RenderBackend - the one part of the renderer that talks to a graphics API, behind an interface shaped like
 * Vulkan's. The front end (renderer/RenderSystem.hpp) creates buffers and render targets through it, records each
 * frame into a CommandList, a plain array of commands that names no API, and hands the list over with Submit(),
 * once per frame. Doom 3 splits its renderer the same way: the front end fills a list of render commands and
 * RB_ExecuteBackEndCommands plays them.
 *
 * The backend in use is Direct3D 11 (renderer/dx11/RenderBackendDX11.hpp): the window and Dear ImGui already
 * draw with it, so what this renders is a texture ImGui can show as it is. A Vulkan backend implements this
 * same interface; each part below notes the Vulkan object it stands for.
 *
 * WARNING: UI thread only. The D3D11 immediate context is not thread-safe, and neither is a Vulkan queue.
 */

// GPU objects the backend owns, by number; 0 is none. A VkBuffer / VkImage plus its memory in Vulkan.
using BufferHandle = uint32_t;
using TargetHandle = uint32_t;

// VK_BUFFER_USAGE_VERTEX_BUFFER_BIT / VK_BUFFER_USAGE_INDEX_BUFFER_BIT. Indexes are 32-bit.
enum class BufferUsage { Vertex, Index };

/*
 * The pipelines the front end draws with, a VkPipeline each: shaders, vertex layout, depth and raster state.
 *   Background  a full-screen triangle with no buffers: the vertical gradient behind the model.
 *   LitSurface  DrawVert triangles (renderer/Model.hpp), lit by the view's lights, depth-tested.
 */
enum class PipelineKind { Background, LitSurface };

/*
 * What every draw reads for one view: a uniform buffer in Vulkan (set 0, binding 0), cbuffer b0 in D3D11.
 * Light colors are linear multiples of white; directions point toward the light, normalized. The background
 * colors are sRGB.
 *
 * WARNING: every member is 16 bytes or a multiple, so std140 and HLSL's cbuffer packing lay it out alike;
 * the backends' shaders are written against this order.
 */
struct ViewConstants {
    Mat4 view_proj;
    Vec4 camera_pos;     // xyz
    Vec4 key_dir;
    Vec4 key_color;
    Vec4 fill_dir;
    Vec4 fill_color;
    Vec4 rim_dir;
    Vec4 rim_color;
    Vec4 ambient;        // rgb, the light from everywhere, brighter from above
    Vec4 bg_top;         // rgb
    Vec4 bg_bottom;      // rgb
};
static_assert(sizeof(ViewConstants) % 16 == 0, "uniform buffers are made of 16-byte rows");

/*
 * What one draw reads: push constants in Vulkan, cbuffer b1 in D3D11.
 *
 * WARNING: at most 128 bytes, the push constant space every Vulkan implementation has.
 */
struct DrawConstants {
    Mat4 model;       // model to world; the normals follow it by its cofactor matrix, so any scale is fine
    Vec4 diffuse;     // rgb sRGB
    Vec4 specular;    // rgb, w: shininess
};
static_assert(sizeof(DrawConstants) <= 128, "push constants are only sure to have 128 bytes");

struct RenderCommand {
    enum Kind {
        BeginPass,     // vkCmdBeginRenderPass: bind `target`, viewport width x height at its top left, clear
        SetView,       // update the view's uniform buffer: views[index]
        BindPipeline,  // vkCmdBindPipeline
        BindGeometry,  // vkCmdBindVertexBuffers + vkCmdBindIndexBuffer
        DrawIndexed,   // vkCmdPushConstants(draws[index]) + vkCmdDrawIndexed(count, 1, first, 0, 0)
        Draw,          // vkCmdDraw(count, 1, 0, 0): no buffers
        EndPass        // vkCmdEndRenderPass, then resolve the samples into the texture the UI shows
    };
    Kind          kind          = BeginPass;
    TargetHandle  target        = 0;
    int           width         = 0;
    int           height        = 0;
    Vec4          clear;
    PipelineKind  pipeline      = PipelineKind::LitSurface;
    BufferHandle  vertex_buffer = 0;
    BufferHandle  index_buffer  = 0;
    uint32_t      first         = 0;
    uint32_t      count         = 0;
    uint32_t      index         = 0;  // into CommandList's views or draws
};

/*
 * CommandList - one frame's commands, as a VkCommandBuffer records them, with the constants they use kept
 * beside them. Recording calls no API, so the front end can record anywhere in the frame.
 */
class CommandList {
public:
    void BeginPass(TargetHandle target, int width, int height, const Vec4& clear) {
        RenderCommand c;
        c.kind   = RenderCommand::BeginPass;
        c.target = target;
        c.width  = width;
        c.height = height;
        c.clear  = clear;
        commands_.push_back(c);
    }

    void SetView(const ViewConstants& view) {
        RenderCommand c;
        c.kind  = RenderCommand::SetView;
        c.index = static_cast<uint32_t>(views_.size());
        views_.push_back(view);
        commands_.push_back(c);
    }

    void BindPipeline(PipelineKind pipeline) {
        RenderCommand c;
        c.kind     = RenderCommand::BindPipeline;
        c.pipeline = pipeline;
        commands_.push_back(c);
    }

    void BindGeometry(BufferHandle vertex_buffer, BufferHandle index_buffer) {
        RenderCommand c;
        c.kind          = RenderCommand::BindGeometry;
        c.vertex_buffer = vertex_buffer;
        c.index_buffer  = index_buffer;
        commands_.push_back(c);
    }

    void DrawIndexed(const DrawConstants& constants, uint32_t first_index, uint32_t index_count) {
        RenderCommand c;
        c.kind  = RenderCommand::DrawIndexed;
        c.first = first_index;
        c.count = index_count;
        c.index = static_cast<uint32_t>(draws_.size());
        draws_.push_back(constants);
        commands_.push_back(c);
    }

    void Draw(uint32_t vertex_count) {
        RenderCommand c;
        c.kind  = RenderCommand::Draw;
        c.count = vertex_count;
        commands_.push_back(c);
    }

    void EndPass() {
        RenderCommand c;
        c.kind = RenderCommand::EndPass;
        commands_.push_back(c);
    }

    void Clear() {
        commands_.clear();
        views_.clear();
        draws_.clear();
    }

    bool                              Empty()                  const { return commands_.empty(); }
    const std::vector<RenderCommand>& Commands()               const { return commands_; }
    const ViewConstants&              View(uint32_t index)     const { return views_[index]; }
    const DrawConstants&              DrawData(uint32_t index) const { return draws_[index]; }

private:
    std::vector<RenderCommand> commands_;
    std::vector<ViewConstants> views_;
    std::vector<DrawConstants> draws_;
};

class RenderBackend {
public:
    virtual ~RenderBackend() = default;

    // "Direct3D 11", for the user.
    virtual const char* Name() const = 0;

    // Compiles the pipelines. False, with the reason in `error`, when the backend cannot draw.
    virtual bool Init(std::string& error) = 0;

    // Frees every buffer, target and pipeline. The handles given out are no good after it.
    virtual void Shutdown() = 0;

    // A buffer filled with `bytes` of `data`, which the GPU only reads. 0 when it cannot be made.
    virtual BufferHandle CreateBuffer(BufferUsage usage, const void* data, size_t bytes) = 0;
    virtual void         DestroyBuffer(BufferHandle buffer) = 0;

    /*
     * Somewhere to render to and then show in the UI: a color and a depth image of width x height pixels,
     * multisampled when the GPU can, with the single-sample copy the UI samples (VkFramebuffer with a resolve
     * attachment). 0 when it cannot be made.
     */
    virtual TargetHandle CreateTarget(int width, int height) = 0;
    virtual void         DestroyTarget(TargetHandle target) = 0;

    /*
     * What Dear ImGui draws the target's last finished frame with (ImTextureID): an ID3D11ShaderResourceView*
     * here; a VkDescriptorSet from ImGui_ImplVulkan_AddTexture in Vulkan.
     */
    virtual uint64_t UITexture(TargetHandle target) const = 0;

    // Samples per pixel the targets have (MSAA); 1 without.
    virtual int Samples() const = 0;

    /*
     * Runs the frame's commands on the GPU, in order.
     *
     * WARNING: a target's pass must end before the UI draws its texture in the same frame; the front end ends
     * every pass it begins.
     */
    virtual void Submit(const CommandList& commands) = 0;

    /*
     * The top-left width x height pixels of the target's last finished frame, as RGBA rows from the top, for
     * snapshots. Waits for the GPU. False when it cannot.
     */
    virtual bool ReadPixels(TargetHandle target, int width, int height, std::vector<uint8_t>& rgba) = 0;
};

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "renderer/Model.hpp"
#include "renderer/RenderBackend.hpp"

/*
 * RenderBackendDX11 - RenderBackend on Direct3D 11, on the device and immediate context the window already
 * draws with (gui/GuiHost.hpp), so a target's texture goes straight to Dear ImGui.
 *
 * Targets are 4x multisampled when the GPU can (2x, then none, when it cannot) and resolved at the end of each
 * pass into the single-sample texture the UI shows. The shaders are HLSL, compiled when Init runs by the
 * D3DCompiler the program already links for ImGui; a Vulkan backend would carry the same two pipelines as
 * SPIR-V.
 *
 * NOTE: shader model 4.0, so the feature level 10.0 devices GuiHost accepts can draw it too.
 *
 * NOTE: a buffer or target destroyed right after a Submit that used it is safe here: D3D11 keeps it alive
 * until the GPU is done. A Vulkan backend must hold such objects until the frame's fence signals.
 */
class RenderBackendDX11 final : public RenderBackend {
public:
    // Holds a reference to the device and the context until Shutdown.
    RenderBackendDX11(ID3D11Device* device, ID3D11DeviceContext* context) : device_(device), context_(context) {
        if (device_) {
            device_->AddRef();
        }
        if (context_) {
            context_->AddRef();
        }
    }

    ~RenderBackendDX11() override { Shutdown(); }

    RenderBackendDX11(const RenderBackendDX11&)            = delete;
    RenderBackendDX11& operator=(const RenderBackendDX11&) = delete;

    const char* Name() const override { return "Direct3D 11"; }

    bool Init(std::string& error) override {
        if (!device_ || !context_) {
            error = "no Direct3D 11 device";
            return false;
        }
        ID3DBlob* surface_vs = Compile("SurfaceVS", "vs_4_0", error);
        ID3DBlob* surface_ps = surface_vs ? Compile("SurfacePS", "ps_4_0", error) : nullptr;
        ID3DBlob* back_vs    = surface_ps ? Compile("BackgroundVS", "vs_4_0", error) : nullptr;
        ID3DBlob* back_ps    = back_vs ? Compile("BackgroundPS", "ps_4_0", error) : nullptr;
        bool ok = back_ps != nullptr;
        if (ok) {
            const D3D11_INPUT_ELEMENT_DESC layout[2] = {
                {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(DrawVert, xyz),    D3D11_INPUT_PER_VERTEX_DATA, 0},
                {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(DrawVert, normal), D3D11_INPUT_PER_VERTEX_DATA, 0},
            };
            ok = SUCCEEDED(device_->CreateVertexShader(surface_vs->GetBufferPointer(), surface_vs->GetBufferSize(),
                                                       nullptr, &surface_vs_)) &&
                 SUCCEEDED(device_->CreatePixelShader(surface_ps->GetBufferPointer(), surface_ps->GetBufferSize(),
                                                      nullptr, &surface_ps_)) &&
                 SUCCEEDED(device_->CreateVertexShader(back_vs->GetBufferPointer(), back_vs->GetBufferSize(),
                                                       nullptr, &back_vs_)) &&
                 SUCCEEDED(device_->CreatePixelShader(back_ps->GetBufferPointer(), back_ps->GetBufferSize(),
                                                      nullptr, &back_ps_)) &&
                 SUCCEEDED(device_->CreateInputLayout(layout, 2, surface_vs->GetBufferPointer(),
                                                      surface_vs->GetBufferSize(), &layout_));
            if (!ok) {
                error = "Direct3D 11 refused the shaders";
            }
        }
        Release(surface_vs);
        Release(surface_ps);
        Release(back_vs);
        Release(back_ps);
        if (!ok || !CreateStates(error)) {
            Shutdown();
            return false;
        }
        samples_ = PickSamples();
        return true;
    }

    void Shutdown() override {
        for (auto& kv : targets_) {
            ReleaseTarget(kv.second);
        }
        targets_.clear();
        for (auto& kv : buffers_) {
            Release(kv.second);
        }
        buffers_.clear();
        Release(view_cb_);
        Release(draw_cb_);
        Release(depth_on_);
        Release(depth_off_);
        Release(raster_);
        Release(layout_);
        Release(surface_vs_);
        Release(surface_ps_);
        Release(back_vs_);
        Release(back_ps_);
        Release(context_);
        Release(device_);
    }

    BufferHandle CreateBuffer(BufferUsage usage, const void* data, size_t bytes) override {
        if (!device_ || !data || bytes == 0 || bytes > 0xFFFFFFFFu) {
            return 0;
        }
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = static_cast<UINT>(bytes);
        desc.Usage     = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = usage == BufferUsage::Vertex ? D3D11_BIND_VERTEX_BUFFER : D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem = data;
        ID3D11Buffer* buffer = nullptr;
        if (FAILED(device_->CreateBuffer(&desc, &init, &buffer))) {
            return 0;
        }
        const BufferHandle handle = next_handle_++;
        buffers_[handle] = buffer;
        return handle;
    }

    void DestroyBuffer(BufferHandle buffer) override {
        const auto it = buffers_.find(buffer);
        if (it != buffers_.end()) {
            Release(it->second);
            buffers_.erase(it);
        }
    }

    TargetHandle CreateTarget(int width, int height) override {
        if (!device_ || width <= 0 || height <= 0) {
            return 0;
        }
        Target t;
        t.width  = width;
        t.height = height;

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width            = static_cast<UINT>(width);
        desc.Height           = static_cast<UINT>(height);
        desc.MipLevels        = 1;
        desc.ArraySize        = 1;
        desc.Format           = kColorFormat;
        desc.SampleDesc.Count = 1;
        desc.Usage            = D3D11_USAGE_DEFAULT;
        desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE | (samples_ == 1 ? D3D11_BIND_RENDER_TARGET : 0);
        bool ok = SUCCEEDED(device_->CreateTexture2D(&desc, nullptr, &t.resolved)) &&
                  SUCCEEDED(device_->CreateShaderResourceView(t.resolved, nullptr, &t.srv));
        if (ok && samples_ > 1) {
            desc.SampleDesc.Count = static_cast<UINT>(samples_);
            desc.BindFlags        = D3D11_BIND_RENDER_TARGET;
            ok = SUCCEEDED(device_->CreateTexture2D(&desc, nullptr, &t.color_ms)) &&
                 SUCCEEDED(device_->CreateRenderTargetView(t.color_ms, nullptr, &t.rtv));
        } else if (ok) {
            ok = SUCCEEDED(device_->CreateRenderTargetView(t.resolved, nullptr, &t.rtv));
        }
        if (ok) {
            desc.Format           = kDepthFormat;
            desc.SampleDesc.Count = static_cast<UINT>(samples_);
            desc.BindFlags        = D3D11_BIND_DEPTH_STENCIL;
            ok = SUCCEEDED(device_->CreateTexture2D(&desc, nullptr, &t.depth)) &&
                 SUCCEEDED(device_->CreateDepthStencilView(t.depth, nullptr, &t.dsv));
        }
        if (!ok) {
            ReleaseTarget(t);
            return 0;
        }
        const TargetHandle handle = next_handle_++;
        targets_[handle] = t;
        return handle;
    }

    void DestroyTarget(TargetHandle target) override {
        const auto it = targets_.find(target);
        if (it != targets_.end()) {
            ReleaseTarget(it->second);
            targets_.erase(it);
        }
    }

    uint64_t UITexture(TargetHandle target) const override {
        const auto it = targets_.find(target);
        return it == targets_.end() ? 0 : reinterpret_cast<uint64_t>(it->second.srv);
    }

    int Samples() const override { return samples_; }

    void Submit(const CommandList& list) override {
        if (!context_) {
            return;
        }
        const Target* target = nullptr;  // the pass under way; commands outside a pass are skipped
        ID3D11Buffer* constants[2] = {view_cb_, draw_cb_};
        for (const RenderCommand& c : list.Commands()) {
            switch (c.kind) {
            case RenderCommand::BeginPass: {
                const auto it = targets_.find(c.target);
                target = it == targets_.end() ? nullptr : &it->second;
                if (!target) {
                    break;
                }
                context_->OMSetRenderTargets(1, &target->rtv, target->dsv);
                D3D11_VIEWPORT vp{};
                vp.Width    = static_cast<float>(std::min(c.width, target->width));
                vp.Height   = static_cast<float>(std::min(c.height, target->height));
                vp.MaxDepth = 1.0f;
                context_->RSSetViewports(1, &vp);
                const float clear[4] = {c.clear.x, c.clear.y, c.clear.z, c.clear.w};
                context_->ClearRenderTargetView(target->rtv, clear);
                context_->ClearDepthStencilView(target->dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
                // WARNING: ImGui's own draw leaves these unset; a stage left over from anything else would run.
                context_->GSSetShader(nullptr, nullptr, 0);
                context_->HSSetShader(nullptr, nullptr, 0);
                context_->DSSetShader(nullptr, nullptr, 0);
                break;
            }
            case RenderCommand::SetView:
                if (target) {
                    Upload(view_cb_, &list.View(c.index), sizeof(ViewConstants));
                }
                break;
            case RenderCommand::BindPipeline:
                if (!target) {
                    break;
                }
                if (c.pipeline == PipelineKind::LitSurface) {
                    context_->IASetInputLayout(layout_);
                    context_->VSSetShader(surface_vs_, nullptr, 0);
                    context_->PSSetShader(surface_ps_, nullptr, 0);
                    context_->OMSetDepthStencilState(depth_on_, 0);
                } else {
                    context_->IASetInputLayout(nullptr);
                    context_->VSSetShader(back_vs_, nullptr, 0);
                    context_->PSSetShader(back_ps_, nullptr, 0);
                    context_->OMSetDepthStencilState(depth_off_, 0);
                }
                context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                context_->RSSetState(raster_);
                context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
                context_->VSSetConstantBuffers(0, 2, constants);
                context_->PSSetConstantBuffers(0, 2, constants);
                break;
            case RenderCommand::BindGeometry: {
                const auto vb = buffers_.find(c.vertex_buffer);
                const auto ib = buffers_.find(c.index_buffer);
                if (!target || vb == buffers_.end() || ib == buffers_.end()) {
                    break;
                }
                const UINT stride = sizeof(DrawVert);
                const UINT offset = 0;
                context_->IASetVertexBuffers(0, 1, &vb->second, &stride, &offset);
                context_->IASetIndexBuffer(ib->second, DXGI_FORMAT_R32_UINT, 0);
                break;
            }
            case RenderCommand::DrawIndexed:
                if (target) {
                    Upload(draw_cb_, &list.DrawData(c.index), sizeof(DrawConstants));
                    context_->DrawIndexed(c.count, c.first, 0);
                }
                break;
            case RenderCommand::Draw:
                if (target) {
                    context_->Draw(c.count, 0);
                }
                break;
            case RenderCommand::EndPass:
                if (target && target->color_ms) {
                    context_->ResolveSubresource(target->resolved, 0, target->color_ms, 0, kColorFormat);
                }
                // WARNING: unbound, so the UI can sample the texture without D3D11 unbinding it behind our back.
                context_->OMSetRenderTargets(0, nullptr, nullptr);
                target = nullptr;
                break;
            }
        }
    }

    bool ReadPixels(TargetHandle handle, int width, int height, std::vector<uint8_t>& rgba) override {
        const auto it = targets_.find(handle);
        if (it == targets_.end() || width <= 0 || height <= 0) {
            return false;
        }
        const Target& t = it->second;
        width  = std::min(width, t.width);
        height = std::min(height, t.height);
        D3D11_TEXTURE2D_DESC desc{};
        t.resolved->GetDesc(&desc);
        desc.Usage          = D3D11_USAGE_STAGING;
        desc.BindFlags      = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* staging = nullptr;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, &staging))) {
            return false;
        }
        context_->CopyResource(staging, t.resolved);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const bool ok = SUCCEEDED(context_->Map(staging, 0, D3D11_MAP_READ, 0, &mapped));
        if (ok) {
            rgba.resize(static_cast<size_t>(width) * height * 4);
            for (int y = 0; y < height; ++y) {
                std::memcpy(&rgba[static_cast<size_t>(y) * width * 4],
                            static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
                            static_cast<size_t>(width) * 4);
            }
            context_->Unmap(staging, 0);
        }
        Release(staging);
        return ok;
    }

private:
    struct Target {
        int                       width    = 0;
        int                       height   = 0;
        ID3D11Texture2D*          color_ms = nullptr;  // multisampled color; null when samples_ is 1
        ID3D11RenderTargetView*   rtv      = nullptr;  // on color_ms, or on resolved without MSAA
        ID3D11Texture2D*          depth    = nullptr;
        ID3D11DepthStencilView*   dsv      = nullptr;
        ID3D11Texture2D*          resolved = nullptr;  // what the UI samples
        ID3D11ShaderResourceView* srv      = nullptr;
    };

    static constexpr DXGI_FORMAT kColorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    static constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

    ID3D11Device*            device_     = nullptr;
    ID3D11DeviceContext*     context_    = nullptr;
    ID3D11VertexShader*      surface_vs_ = nullptr;
    ID3D11PixelShader*       surface_ps_ = nullptr;
    ID3D11VertexShader*      back_vs_    = nullptr;
    ID3D11PixelShader*       back_ps_    = nullptr;
    ID3D11InputLayout*       layout_     = nullptr;
    ID3D11Buffer*            view_cb_    = nullptr;
    ID3D11Buffer*            draw_cb_    = nullptr;
    ID3D11DepthStencilState* depth_on_   = nullptr;
    ID3D11DepthStencilState* depth_off_  = nullptr;
    ID3D11RasterizerState*   raster_     = nullptr;
    int                      samples_    = 1;
    uint32_t                 next_handle_ = 1;
    std::unordered_map<uint32_t, ID3D11Buffer*> buffers_;
    std::unordered_map<uint32_t, Target>        targets_;

    template <class T>
    static void Release(T*& p) {
        if (p) {
            p->Release();
            p = nullptr;
        }
    }

    static void ReleaseTarget(Target& t) {
        Release(t.srv);
        Release(t.resolved);
        Release(t.dsv);
        Release(t.depth);
        Release(t.rtv);
        Release(t.color_ms);
    }

    void Upload(ID3D11Buffer* buffer, const void* data, size_t bytes) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context_->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            std::memcpy(mapped.pData, data, bytes);
            context_->Unmap(buffer, 0);
        }
    }

    // 4 samples per pixel, else 2, else 1: what the GPU can do for both the color and the depth format.
    int PickSamples() const {
        for (UINT count : {4u, 2u}) {
            UINT color = 0;
            UINT depth = 0;
            if (SUCCEEDED(device_->CheckMultisampleQualityLevels(kColorFormat, count, &color)) && color > 0 &&
                SUCCEEDED(device_->CheckMultisampleQualityLevels(kDepthFormat, count, &depth)) && depth > 0) {
                return static_cast<int>(count);
            }
        }
        return 1;
    }

    bool CreateStates(std::string& error) {
        D3D11_BUFFER_DESC cb{};
        cb.Usage          = D3D11_USAGE_DYNAMIC;
        cb.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
        cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        cb.ByteWidth      = (sizeof(ViewConstants) + 15) & ~15u;
        bool ok = SUCCEEDED(device_->CreateBuffer(&cb, nullptr, &view_cb_));
        cb.ByteWidth = (sizeof(DrawConstants) + 15) & ~15u;
        ok = ok && SUCCEEDED(device_->CreateBuffer(&cb, nullptr, &draw_cb_));

        D3D11_DEPTH_STENCIL_DESC ds{};
        ds.DepthEnable    = TRUE;
        ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        ds.DepthFunc      = D3D11_COMPARISON_LESS;
        ok = ok && SUCCEEDED(device_->CreateDepthStencilState(&ds, &depth_on_));
        ds.DepthEnable    = FALSE;
        ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        ok = ok && SUCCEEDED(device_->CreateDepthStencilState(&ds, &depth_off_));

        /*
         * NOTE: no culling. The model's winding is whatever its file has, and thin parts (lids, brows, strands)
         * are seen from both sides; the pixel shader turns each normal toward the side being drawn.
         *
         * WARNING: counter-clockwise is the front. The math is right-handed (RenderMath.hpp), so a face whose
         * normal (worked out from its winding, Model.hpp) points at the camera is counter-clockwise on screen.
         * With D3D11's default, clockwise, every visible face counted as a back face and the shader turned
         * every normal away from the camera. A Vulkan backend sets VK_FRONT_FACE_COUNTER_CLOCKWISE likewise.
         */
        D3D11_RASTERIZER_DESC rs{};
        rs.FillMode              = D3D11_FILL_SOLID;
        rs.CullMode              = D3D11_CULL_NONE;
        rs.FrontCounterClockwise = TRUE;
        rs.DepthClipEnable   = TRUE;
        rs.MultisampleEnable = TRUE;
        ok = ok && SUCCEEDED(device_->CreateRasterizerState(&rs, &raster_));
        if (!ok) {
            error = "Direct3D 11 could not make the renderer's buffers and states";
        }
        return ok;
    }

    ID3DBlob* Compile(const char* entry, const char* profile, std::string& error) const {
        ID3DBlob* code   = nullptr;
        ID3DBlob* errors = nullptr;
        const HRESULT hr = D3DCompile(kShaderSource, std::strlen(kShaderSource), "renderer_dx11.hlsl", nullptr,
                                      nullptr, entry, profile,
                                      D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
        if (FAILED(hr)) {
            error = std::string("could not compile ") + entry;
            if (errors) {
                error += ": " + std::string(static_cast<const char*>(errors->GetBufferPointer()),
                                            errors->GetBufferSize());
            }
            Release(code);
        }
        Release(errors);
        return SUCCEEDED(hr) ? code : nullptr;
    }

    /*
     * The two pipelines' shaders. The cbuffers mirror ViewConstants and DrawConstants (RenderBackend.hpp).
     *
     * Lighting is done in linear color: the material and background colors are sRGB and are converted on the
     * way in, the result on the way out. The diffuse wraps a little past the terminator, as light does on skin,
     * a hemisphere ambient is brighter from above, and a rim light from behind outlines the silhouette.
     *
     * NOTE: front-facing is decided by the winding on screen, the same winding the model's normals were
     * worked out from (Model.hpp), so flipping the normal of a back face turns it toward the viewer whatever
     * way the file winds its faces.
     */
    static constexpr const char* kShaderSource = R"hlsl(
cbuffer View : register(b0) {
    float4x4 view_proj;
    float4   camera_pos;
    float4   key_dir;
    float4   key_color;
    float4   fill_dir;
    float4   fill_color;
    float4   rim_dir;
    float4   rim_color;
    float4   ambient;
    float4   bg_top;
    float4   bg_bottom;
};

cbuffer Draw : register(b1) {
    float4x4 model;
    float4   diffuse;
    float4   specular;
};

struct SurfaceIn {
    float3 pos    : POSITION;
    float3 normal : NORMAL;
};

struct SurfaceOut {
    float4 pos    : SV_Position;
    float3 world  : TEXCOORD0;
    float3 normal : TEXCOORD1;
};

SurfaceOut SurfaceVS(SurfaceIn v) {
    SurfaceOut o;
    float4 world = mul(model, float4(v.pos, 1.0));
    o.pos   = mul(view_proj, world);
    o.world = world.xyz;
    // By the cofactor matrix of the model's 3x3, which keeps a normal perpendicular under any scale.
    float3 c0 = model._m00_m10_m20;
    float3 c1 = model._m01_m11_m21;
    float3 c2 = model._m02_m12_m22;
    o.normal = v.normal.x * cross(c1, c2) + v.normal.y * cross(c2, c0) + v.normal.z * cross(c0, c1);
    return o;
}

float3 ToLinear(float3 c) {
    return pow(max(c, 0.0), 2.2);
}

float3 ToDisplay(float3 c) {
    return pow(saturate(c), 1.0 / 2.2);
}

float Wrapped(float3 n, float3 l) {
    return saturate((dot(n, l) + 0.3) / 1.3);
}

float4 SurfacePS(SurfaceOut i, bool front : SV_IsFrontFace) : SV_Target {
    float3 n = normalize(i.normal);
    n = front ? n : -n;
    float3 v      = normalize(camera_pos.xyz - i.world);
    float3 albedo = ToLinear(diffuse.rgb);
    float  sky    = n.y * 0.5 + 0.5;
    float3 light  = ambient.rgb * lerp(0.55, 1.0, sky) + key_color.rgb * Wrapped(n, key_dir.xyz) +
                    fill_color.rgb * Wrapped(n, fill_dir.xyz);
    float3 h      = normalize(key_dir.xyz + v);
    float  spec   = pow(saturate(dot(n, h)), max(specular.w, 1.0)) * saturate(dot(n, key_dir.xyz) * 4.0);
    // WARNING: only edges that face the rim light: thin parts (hair strands) are all edge, and a wider rim
    // washed the dark hair out to the rim light's color.
    float  rim    = pow(1.0 - saturate(dot(n, v)), 4.0) * saturate(dot(n, rim_dir.xyz));
    float3 color  = albedo * light + specular.rgb * key_color.rgb * spec + rim_color.rgb * rim;
    return float4(ToDisplay(color), 1.0);
}

struct BackgroundOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

// One triangle that covers the viewport, made from the vertex number alone: no buffers.
BackgroundOut BackgroundVS(uint id : SV_VertexID) {
    BackgroundOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv  = uv;
    return o;
}

float4 BackgroundPS(BackgroundOut i) : SV_Target {
    float3 c = lerp(bg_top.rgb, bg_bottom.rgb, saturate(i.uv.y));
    float2 d = i.uv - float2(0.5, 0.42);
    c *= 1.0 - 0.35 * saturate(dot(d, d) * 1.6);  // darker toward the corners
    return float4(c, 1.0);
}
)hlsl";
};

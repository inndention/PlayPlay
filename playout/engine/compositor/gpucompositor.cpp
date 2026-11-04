#include "gpucompositor.h"
#include <QDebug>
#include <QElapsedTimer>
#include <QJsonObject>
#include <cstring>
#include <cmath>
#include <map>

#ifdef _WIN32
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

namespace {

const char kShader[] = R"HLSL(
cbuffer Draw : register(b0) {
    float4 dstRect;     // x0,y0,x1,y1 in NDC
    float4 srcRect;     // u0,v0,u1,v1
    float4 params;      // opacity, premultiplied(0/1), 0, 0
};
Texture2D tex : register(t0);
SamplerState samp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VS(uint id : SV_VertexID) {
    // Two triangles covering dstRect (triangle strip order via id 0..3).
    float2 corner = float2(id & 1, (id >> 1) & 1);          // (0,0) (1,0) (0,1) (1,1)
    VSOut o;
    o.pos = float4(lerp(dstRect.x, dstRect.z, corner.x), lerp(dstRect.y, dstRect.w, corner.y), 0.0, 1.0);
    o.uv = float2(lerp(srcRect.x, srcRect.z, corner.x), lerp(srcRect.y, srcRect.w, corner.y));
    return o;
}
float4 PS(VSOut i) : SV_Target {
    float4 c = tex.Sample(samp, i.uv);
    // Straight alpha is premultiplied after filtering; a premultiplied texture (PE-08) is used as is, which also filters
    // correctly at scaled edges (no colour bleeding in from transparent texels).
    if (params.y < 0.5) c.rgb *= c.a;                      // straight alpha -> premultiplied
    return c * params.x;
}
)HLSL";

struct DrawCb { float dst[4]; float src[4]; float params[4]; };

// Readback through a linear buffer: the render target is packed into a raw buffer by a compute shader (B,G,R,A bytes, the
// mixer's memory order), and that buffer is copied to a staging buffer the CPU maps.
const char kPackShader[] = R"HLSL(
Texture2D<float4> src : register(t0);
RWByteAddressBuffer dst : register(u0);
cbuffer P : register(b0) { uint width; uint height; uint pad0; uint pad1; };
[numthreads(16, 16, 1)]
void CS(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    uint4 u = uint4(round(saturate(src.Load(int3(id.xy, 0))) * 255.0));
    dst.Store((id.y * width + id.x) * 4, u.b | (u.g << 8) | (u.r << 16) | (u.a << 24));
}
)HLSL";

class D3D11Compositor : public GpuCompositor {
public:
    bool init(int w, int h, QString* err) {
        m_w = w; m_h = h;
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        // PLAYPLAY_GPU_NO_DRIVER_THREADS=1: no driver worker thread (measured slower on the Arc driver: uploads become synchronous).
        if (qEnvironmentVariableIsSet("PLAYPLAY_GPU_NO_DRIVER_THREADS")) flags |= D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS;
        D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
        D3D_FEATURE_LEVEL got;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 4, D3D11_SDK_VERSION, &m_dev, &got, &m_ctx);
        if (FAILED(hr)) hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, 4, D3D11_SDK_VERSION, &m_dev, &got, &m_ctx);
        if (FAILED(hr)) { if (err) *err = QString("D3D11CreateDevice failed 0x%1").arg(uint(hr), 8, 16, QChar('0')); return false; }
        {
            ComPtr<IDXGIDevice> dxgi; ComPtr<IDXGIAdapter> adapter; DXGI_ADAPTER_DESC desc{};
            if (SUCCEEDED(m_dev.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc)))
                m_adapter = QString::fromWCharArray(desc.Description);
            // The clock thread drives this device alone: keep at most two frames queued so a slow GPU shows up as a wait, not as latency.
            ComPtr<IDXGIDevice1> d1; if (SUCCEEDED(m_dev.As(&d1))) d1->SetMaximumFrameLatency(2);
        }
        D3D11_TEXTURE2D_DESC td{}; td.Width = UINT(w); td.Height = UINT(h); td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(m_dev->CreateTexture2D(&td, nullptr, &m_rt))) { if (err) *err = "render target"; return false; }
        if (FAILED(m_dev->CreateRenderTargetView(m_rt.Get(), nullptr, &m_rtv))) { if (err) *err = "rtv"; return false; }
        if (m_bufferReadback && got >= D3D_FEATURE_LEVEL_11_0) {
            ComPtr<ID3DBlob> cs, cerr;
            bool ok = SUCCEEDED(D3DCompile(kPackShader, sizeof(kPackShader) - 1, "pack", nullptr, nullptr, "CS", "cs_5_0", 0, 0, &cs, &cerr))
                   && SUCCEEDED(m_dev->CreateComputeShader(cs->GetBufferPointer(), cs->GetBufferSize(), nullptr, &m_packCs))
                   && SUCCEEDED(m_dev->CreateShaderResourceView(m_rt.Get(), nullptr, &m_rtSrv));
            D3D11_BUFFER_DESC bd{}; bd.ByteWidth = UINT(w) * UINT(h) * 4; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS; bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
            ok = ok && SUCCEEDED(m_dev->CreateBuffer(&bd, nullptr, &m_packBuf));
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud{}; ud.Format = DXGI_FORMAT_R32_TYPELESS; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; ud.Buffer.NumElements = UINT(w) * UINT(h); ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
            ok = ok && SUCCEEDED(m_dev->CreateUnorderedAccessView(m_packBuf.Get(), &ud, &m_packUav));
            D3D11_BUFFER_DESC cbd{}; cbd.ByteWidth = 16; cbd.Usage = D3D11_USAGE_IMMUTABLE; cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            const UINT dims[4] = { UINT(w), UINT(h), 0, 0 }; D3D11_SUBRESOURCE_DATA init{ dims, 0, 0 };
            ok = ok && SUCCEEDED(m_dev->CreateBuffer(&cbd, &init, &m_packCb));
            if (!ok) { qWarning() << "[GpuCompositor] buffer readback unavailable; using staging textures"; m_bufferReadback = false; }
        } else m_bufferReadback = false;
        ComPtr<ID3DBlob> vs, ps, errs;
        if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "comp", nullptr, nullptr, "VS", "vs_4_0", 0, 0, &vs, &errs))) { if (err) *err = "VS: " + QString::fromLatin1(errs ? (const char*)errs->GetBufferPointer() : ""); return false; }
        if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "comp", nullptr, nullptr, "PS", "ps_4_0", 0, 0, &ps, &errs))) { if (err) *err = "PS: " + QString::fromLatin1(errs ? (const char*)errs->GetBufferPointer() : ""); return false; }
        if (FAILED(m_dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &m_vs))) return false;
        if (FAILED(m_dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &m_ps))) return false;
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = sizeof(DrawCb); bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(m_dev->CreateBuffer(&bd, nullptr, &m_cb))) return false;
        D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(m_dev->CreateSamplerState(&sd, &m_sampler))) return false;
        D3D11_BLEND_DESC bl{}; bl.RenderTarget[0].BlendEnable = TRUE;
        bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA; bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(m_dev->CreateBlendState(&bl, &m_blend))) return false;
        // The first draw of a tick is usually the opaque program picture: no blending needed (saves a full-raster read).
        bl.RenderTarget[0].BlendEnable = FALSE;
        if (FAILED(m_dev->CreateBlendState(&bl, &m_noBlend))) return false;
        D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.ScissorEnable = TRUE; rd.DepthClipEnable = TRUE;
        if (FAILED(m_dev->CreateRasterizerState(&rd, &m_raster))) return false;
        return true;
    }

    int submit(const std::vector<DrawOp>& ops) override {
        QElapsedTimer timer; timer.start();
        ++m_submitSerial;
        // A staging texture to receive this picture.
        int ticket = -1;
        for (size_t i = 0; i < m_staging.size(); ++i) if (!m_staging[i].busy) { ticket = int(i); break; }
        if (ticket < 0) {
            if (m_staging.size() >= 8) { qWarning() << "[GpuCompositor] no free staging texture (fetches are not keeping up)"; return -1; }
            Staging s;
            if (m_bufferReadback) {
                D3D11_BUFFER_DESC bd{}; bd.ByteWidth = UINT(m_w) * UINT(m_h) * 4; bd.Usage = D3D11_USAGE_STAGING; bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                if (FAILED(m_dev->CreateBuffer(&bd, nullptr, &s.buf))) return -1;
            } else {
                D3D11_TEXTURE2D_DESC td{}; td.Width = UINT(m_w); td.Height = UINT(m_h); td.MipLevels = 1; td.ArraySize = 1;
                td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                if (FAILED(m_dev->CreateTexture2D(&td, nullptr, &s.tex))) return -1;
            }
            D3D11_QUERY_DESC qd{}; qd.Query = D3D11_QUERY_EVENT;
            m_dev->CreateQuery(&qd, &s.done);
            qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT; m_dev->CreateQuery(&qd, &s.disjoint);
            qd.Query = D3D11_QUERY_TIMESTAMP; m_dev->CreateQuery(&qd, &s.t0); m_dev->CreateQuery(&qd, &s.tCopy); m_dev->CreateQuery(&qd, &s.t1);
            m_staging.push_back(std::move(s));
            ticket = int(m_staging.size() - 1);
        }

        Staging& st = m_staging[size_t(ticket)];
        if (st.disjoint) { m_ctx->Begin(st.disjoint.Get()); m_ctx->End(st.t0.Get()); }
        const float clear[4] = { 0, 0, 0, 1 };
        m_ctx->ClearRenderTargetView(m_rtv.Get(), clear);
        ID3D11RenderTargetView* rtv = m_rtv.Get();
        m_ctx->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp{ 0, 0, float(m_w), float(m_h), 0, 1 };
        m_ctx->RSSetViewports(1, &vp);
        m_ctx->RSSetState(m_raster.Get());
        m_ctx->IASetInputLayout(nullptr);
        m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        m_ctx->VSSetShader(m_vs.Get(), nullptr, 0);
        m_ctx->PSSetShader(m_ps.Get(), nullptr, 0);
        ID3D11Buffer* cb = m_cb.Get();
        m_ctx->VSSetConstantBuffers(0, 1, &cb);
        m_ctx->PSSetConstantBuffers(0, 1, &cb);
        ID3D11SamplerState* ss = m_sampler.Get();
        m_ctx->PSSetSamplers(0, 1, &ss);

        bool first = true;
        for (const DrawOp& op : ops) {
            if (!op.frame || op.opacity <= 0.0) continue;
            ID3D11ShaderResourceView* srv = textureFor(op.frame, op.upload);
            if (!srv) return -1;
            D3D11_RECT sc{ 0, 0, m_w, m_h };
            if (!op.clip.isNull()) { sc.left = LONG(std::floor(op.clip.left())); sc.top = LONG(std::floor(op.clip.top())); sc.right = LONG(std::ceil(op.clip.right())); sc.bottom = LONG(std::ceil(op.clip.bottom())); }
            if (sc.right <= sc.left || sc.bottom <= sc.top) continue;
            m_ctx->RSSetScissorRects(1, &sc);
            // A premultiplied picture (PE-08: template / HTML) may carry alpha: it is blended onto the cleared black, so
            // the output stays opaque like the CPU path's, never written over the target as is.
            const bool fullOpaque = first && !op.premultiplied && op.opacity >= 1.0 && op.clip.isNull() && op.dst == QRectF(0, 0, m_w, m_h);
            m_ctx->OMSetBlendState(fullOpaque ? m_noBlend.Get() : m_blend.Get(), nullptr, 0xFFFFFFFF);
            first = false;
            DrawCb c{};
            c.dst[0] = float(op.dst.left() / m_w * 2.0 - 1.0); c.dst[1] = float(1.0 - op.dst.top() / m_h * 2.0);
            c.dst[2] = float(op.dst.right() / m_w * 2.0 - 1.0); c.dst[3] = float(1.0 - op.dst.bottom() / m_h * 2.0);
            const QRectF sr = op.src.isNull() ? QRectF(0, 0, op.frame->width, op.frame->height) : op.src;
            c.src[0] = float(sr.left() / op.frame->width); c.src[1] = float(sr.top() / op.frame->height);
            c.src[2] = float(sr.right() / op.frame->width); c.src[3] = float(sr.bottom() / op.frame->height);
            c.params[0] = float(op.opacity); c.params[1] = op.premultiplied ? 1.f : 0.f;
            D3D11_MAPPED_SUBRESOURCE m{};
            if (FAILED(m_ctx->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return -1;
            memcpy(m.pData, &c, sizeof c);
            m_ctx->Unmap(m_cb.Get(), 0);
            m_ctx->PSSetShaderResources(0, 1, &srv);
            m_ctx->Draw(4, 0);
        }
        if (st.disjoint) m_ctx->End(st.tCopy.Get());
        if (m_bufferReadback) {
            ID3D11RenderTargetView* none = nullptr; m_ctx->OMSetRenderTargets(1, &none, nullptr);   // the RT is read as a texture now
            m_ctx->CSSetShader(m_packCs.Get(), nullptr, 0);
            ID3D11ShaderResourceView* srv = m_rtSrv.Get(); m_ctx->CSSetShaderResources(0, 1, &srv);
            ID3D11UnorderedAccessView* uav = m_packUav.Get(); m_ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
            ID3D11Buffer* pcb = m_packCb.Get(); m_ctx->CSSetConstantBuffers(0, 1, &pcb);
            m_ctx->Dispatch(UINT((m_w + 15) / 16), UINT((m_h + 15) / 16), 1);
            ID3D11ShaderResourceView* nsrv = nullptr; m_ctx->CSSetShaderResources(0, 1, &nsrv);
            ID3D11UnorderedAccessView* nuav = nullptr; m_ctx->CSSetUnorderedAccessViews(0, 1, &nuav, nullptr);
            m_ctx->CopyResource(st.buf.Get(), m_packBuf.Get());
        } else {
            m_ctx->CopyResource(st.tex.Get(), m_rt.Get());
        }
        if (st.disjoint) { m_ctx->End(st.t1.Get()); m_ctx->End(st.disjoint.Get()); }
        if (st.done) m_ctx->End(st.done.Get());
        m_ctx->Flush();                     // start the GPU now; the readback is fetched next tick
        // A non-blocking poll that is allowed to flush: on the Arc driver Flush() alone left the batch in the driver's
        // queue until the next Map, which then waited for the whole frame of GPU work.
        if (st.done && !m_noKick) { BOOL fin = FALSE; m_ctx->GetData(st.done.Get(), &fin, sizeof fin, 0); }
        m_staging[size_t(ticket)].busy = true;
        m_submits++;
        m_lastMs = timer.nsecsElapsed() / 1e6;
        m_submitMsSum += m_lastMs;
        return ticket;
    }

    bool fetch(int ticket, const FramePtr& out) override {
        if (ticket < 0 || size_t(ticket) >= m_staging.size() || !m_staging[size_t(ticket)].busy) return false;
        QElapsedTimer timer; timer.start();
        Staging& s = m_staging[size_t(ticket)];
        bool ok = false;
        if (out && out->width == m_w && out->height == m_h) {
            D3D11_MAPPED_SUBRESOURCE m{};
            if (s.done) { BOOL fin = FALSE; if (m_ctx->GetData(s.done.Get(), &fin, sizeof fin, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && fin) m_gpuDoneAtFetch++; else m_gpuBusyAtFetch++; }
            HRESULT hr = m_ctx->Map(s.res(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
                QElapsedTimer w; w.start();
                hr = m_ctx->Map(s.res(), 0, D3D11_MAP_READ, 0, &m);
                m_waits++; m_waitMs += w.nsecsElapsed() / 1e6;
            }
            if (SUCCEEDED(hr)) {
                QElapsedTimer ct; ct.start();
                const size_t row = size_t(m_w) * 4;
                if (s.buf) m.RowPitch = UINT(row);           // a buffer is tightly packed
                if (size_t(out->linesize[0]) == row && m.RowPitch == row) memcpy(out->data[0], m.pData, row * size_t(m_h));
                else for (int y = 0; y < m_h; ++y) memcpy(out->data[0] + size_t(y) * out->linesize[0], static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch, row);
                m_ctx->Unmap(s.res(), 0);
                m_copyMs += ct.nsecsElapsed() / 1e6;
                if (s.disjoint) {
                    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{}; UINT64 a = 0, b = 0, c = 0;
                    if (m_ctx->GetData(s.disjoint.Get(), &dj, sizeof dj, 0) == S_OK && !dj.Disjoint && dj.Frequency &&
                        m_ctx->GetData(s.t0.Get(), &a, sizeof a, 0) == S_OK && m_ctx->GetData(s.tCopy.Get(), &b, sizeof b, 0) == S_OK && m_ctx->GetData(s.t1.Get(), &c, sizeof c, 0) == S_OK) {
                        m_gpuDrawMs += double(b - a) * 1000.0 / dj.Frequency; m_gpuCopyMs += double(c - b) * 1000.0 / dj.Frequency; m_gpuTimed++;
                    }
                }
                ok = true;
            }
        }
        s.busy = false;
        m_fetchMs = timer.nsecsElapsed() / 1e6;
        m_fetchMsSum += m_fetchMs; m_fetches++;
        return ok;
    }

    void release(int ticket) override { if (ticket >= 0 && size_t(ticket) < m_staging.size()) m_staging[size_t(ticket)].busy = false; }

    QString adapterName() const override { return m_adapter; }
    double lastRenderMs() const override { return m_lastMs; }
    double lastFetchMs() const override { return m_fetchMs; }
    QJsonObject stats() const override { return countersJson(counters()); }
    Counters counters() const override {
        Counters c;
        c.submits = double(m_submits); c.uploads = double(m_uploads); c.cacheHits = double(m_hits);
        c.uploadedMB = m_uploadedBytes / 1048576.0; c.fetchWaits = double(m_waits); c.fetchWaitMs = m_waitMs;
        c.cachedTextures = double(m_cache.size()); c.stagingTextures = double(m_staging.size());
        c.gpuDoneAtFetch = double(m_gpuDoneAtFetch); c.gpuBusyAtFetch = double(m_gpuBusyAtFetch); c.copyMs = m_copyMs;
        if (m_submits) c.submitMsAvg = m_submitMsSum / m_submits;
        if (m_fetches) c.fetchMsAvg = m_fetchMsSum / m_fetches;
        if (m_gpuTimed) { c.gpuDrawMsAvg = m_gpuDrawMs / m_gpuTimed; c.gpuCopyMsAvg = m_gpuCopyMs / m_gpuTimed; }
        c.lastRenderMs = m_lastMs; c.lastFetchMs = m_fetchMs;
        return c;
    }

private:
    struct Staging { ComPtr<ID3D11Texture2D> tex; ComPtr<ID3D11Buffer> buf; ComPtr<ID3D11Query> done, disjoint, t0, tCopy, t1; bool busy = false;
                     ID3D11Resource* res() const { return buf ? static_cast<ID3D11Resource*>(buf.Get()) : static_cast<ID3D11Resource*>(tex.Get()); } };
    struct Tex { ComPtr<ID3D11Texture2D> tex; ComPtr<ID3D11ShaderResourceView> srv; int w = 0, h = 0; };
    struct Cached {
        FramePtr held;                       // keeps the buffer referenced: while held, no pool can recycle it, so the pixels cannot change
        Tex tex;
        quint64 lastUsed = 0;
    };

    /// The texture holding `f`'s pixels, uploading only when this buffer has not been uploaded before.
    ID3D11ShaderResourceView* textureFor(const FramePtr& f, QRect region) {
        if (region.isEmpty()) region = QRect(0, 0, f->width, f->height);
        const uint8_t* key = f->data[0];
        auto it = m_cache.find(key);
        if (it != m_cache.end() && it->second.tex.w == f->width && it->second.tex.h == f->height && !m_noCache) {
            it->second.lastUsed = m_submitSerial;
            it->second.held = f;
            m_hits++;
            return it->second.tex.srv.Get();
        }
        Tex t;
        if (it != m_cache.end()) { t = std::move(it->second.tex); m_cache.erase(it); }
        if (t.w != f->width || t.h != f->height) t = takeTexture(f->width, f->height);
        if (!t.tex) return nullptr;
        if (m_dynamicUpload) {
            // Write straight into GPU-visible upload memory (WRITE_DISCARD renames, so a texture still in use never stalls).
            D3D11_MAPPED_SUBRESOURCE m{};
            if (FAILED(m_ctx->Map(t.tex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return nullptr;
            const size_t row = size_t(region.width()) * 4, x0 = size_t(region.x()) * 4;
            if (region == QRect(0, 0, f->width, f->height) && m.RowPitch == row && size_t(f->linesize[0]) == row) memcpy(m.pData, f->data[0], row * size_t(f->height));
            else for (int y = region.top(); y <= region.bottom(); ++y) memcpy(static_cast<uint8_t*>(m.pData) + size_t(y) * m.RowPitch + x0, f->data[0] + size_t(y) * f->linesize[0] + x0, row);
            m_ctx->Unmap(t.tex.Get(), 0);
        } else {
            D3D11_BOX box{ UINT(region.x()), UINT(region.y()), 0, UINT(region.right() + 1), UINT(region.bottom() + 1), 1 };
            m_ctx->UpdateSubresource(t.tex.Get(), 0, &box, f->data[0] + size_t(region.y()) * f->linesize[0] + size_t(region.x()) * 4, UINT(f->linesize[0]), 0);
        }
        m_uploads++; m_uploadedBytes += double(region.width()) * 4 * region.height();
        Cached c; c.held = f; c.tex = std::move(t); c.lastUsed = m_submitSerial;
        ID3D11ShaderResourceView* srv = c.tex.srv.Get();
        m_cache.emplace(key, std::move(c));
        evict();
        return srv;
    }
    /// Drop cache entries not used in the last few submits (their buffers go back to their pools); keep their textures for reuse.
    void evict() {
        if (m_cache.size() <= 8) return;
        for (auto it = m_cache.begin(); it != m_cache.end();) {
            if (m_submitSerial - it->second.lastUsed > 4) { m_free.push_back(std::move(it->second.tex)); it = m_cache.erase(it); }
            else ++it;
        }
        while (m_free.size() > 8) m_free.erase(m_free.begin());
    }
    Tex takeTexture(int w, int h) {
        for (auto it = m_free.begin(); it != m_free.end(); ++it) if (it->w == w && it->h == h) { Tex t = std::move(*it); m_free.erase(it); return t; }
        // Evict the least recently used entry of this size before allocating more video memory.
        Tex t; t.w = w; t.h = h;
        D3D11_TEXTURE2D_DESC td{}; td.Width = UINT(w); td.Height = UINT(h); td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.Usage = m_dynamicUpload ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT; td.CPUAccessFlags = m_dynamicUpload ? D3D11_CPU_ACCESS_WRITE : 0;
        if (FAILED(m_dev->CreateTexture2D(&td, nullptr, &t.tex))) return Tex();
        if (FAILED(m_dev->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv))) return Tex();
        return t;
    }

    int m_w = 0, m_h = 0;
    const bool m_noCache = qEnvironmentVariableIsSet("PLAYPLAY_GPU_NOCACHE");   // benchmark: the old always-upload behaviour
    const bool m_noKick = qEnvironmentVariableIsSet("PLAYPLAY_GPU_NOKICK");
    const bool m_dynamicUpload = !qEnvironmentVariableIsSet("PLAYPLAY_GPU_UPDATESUBRESOURCE");
    bool m_bufferReadback = qEnvironmentVariable("PLAYPLAY_GPU_READBACK") == "buffer";
    ComPtr<ID3D11Device> m_dev; ComPtr<ID3D11DeviceContext> m_ctx;
    ComPtr<ID3D11Texture2D> m_rt; ComPtr<ID3D11RenderTargetView> m_rtv;
    ComPtr<ID3D11ShaderResourceView> m_rtSrv; ComPtr<ID3D11ComputeShader> m_packCs; ComPtr<ID3D11Buffer> m_packBuf, m_packCb; ComPtr<ID3D11UnorderedAccessView> m_packUav;
    ComPtr<ID3D11VertexShader> m_vs; ComPtr<ID3D11PixelShader> m_ps;
    ComPtr<ID3D11Buffer> m_cb; ComPtr<ID3D11SamplerState> m_sampler;
    ComPtr<ID3D11BlendState> m_blend, m_noBlend; ComPtr<ID3D11RasterizerState> m_raster;
    std::vector<Staging> m_staging;
    std::map<const uint8_t*, Cached> m_cache;
    std::vector<Tex> m_free;
    quint64 m_submitSerial = 0;
    qint64 m_submits = 0, m_uploads = 0, m_hits = 0, m_waits = 0, m_gpuDoneAtFetch = 0, m_gpuBusyAtFetch = 0;
    double m_copyMs = 0, m_gpuDrawMs = 0, m_gpuCopyMs = 0, m_submitMsSum = 0, m_fetchMsSum = 0; qint64 m_gpuTimed = 0, m_fetches = 0;
    double m_uploadedBytes = 0, m_waitMs = 0;
    QString m_adapter; double m_lastMs = 0.0, m_fetchMs = 0.0;
};

} // namespace

std::unique_ptr<GpuCompositor> GpuCompositor::create(int width, int height, QString* error) {
    auto c = std::make_unique<D3D11Compositor>();
    if (!c->init(width, height, error)) return nullptr;
    return c;
}
#else
std::unique_ptr<GpuCompositor> GpuCompositor::create(int, int, QString* error) {
    if (error) *error = "GPU compositor is Direct3D 11 only";
    return nullptr;
}
#endif

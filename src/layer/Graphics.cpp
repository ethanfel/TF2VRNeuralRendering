#include "Graphics.h"
#include "Support.h"
#include "Bindings.h"
#include "DepthCapture.h"
#include <cmath>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <limits>
#include <locale>
#include <sstream>
#include <vector>
#include <wrl/client.h>
#include <d3d11_4.h>
namespace tf2vrnr {
using Microsoft::WRL::ComPtr;
bool CopyPresentationImage(ID3D11Device* device,ID3D11Texture2D* destination,ID3D11Texture2D* source) {
    if(!device||!destination||!source)return false;
    D3D11_TEXTURE2D_DESC d{},s{};destination->GetDesc(&d);source->GetDesc(&s);
    auto rgba=[](DXGI_FORMAT f){return f==DXGI_FORMAT_R8G8B8A8_UNORM||f==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB||f==DXGI_FORMAT_R8G8B8A8_TYPELESS;};
    if(d.Width!=s.Width||d.Height!=s.Height||d.ArraySize!=s.ArraySize||d.MipLevels!=s.MipLevels||d.SampleDesc.Count!=s.SampleDesc.Count||!rgba(d.Format)||!rgba(s.Format))return false;
    ComPtr<ID3D11DeviceContext> context;device->GetImmediateContext(&context);
    ComPtr<ID3D11Multithread> protection;context.As(&protection);
    if(protection)protection->Enter();
    context->CopyResource(destination,source);context->Flush();
    if(protection)protection->Leave();
    return SUCCEEDED(device->GetDeviceRemovedReason());
}
namespace {
unsigned BytesPerPixel(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R16_FLOAT: return 2;
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R16G16_FLOAT: return 4;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM: return 8;
    default: return 0;
    }
}
struct DdsHeader {
    uint32_t magic = 0x20534444, size = 124, flags = 0x100f, height = 0, width = 0, pitch = 0, depth = 0, mips = 0;
    uint32_t reserved[11]{};
    uint32_t pfSize = 32, pfFlags = 4, fourcc = 0x30315844, bits = 0, r = 0, g = 0, b = 0, a = 0;
    uint32_t caps = 0x1000, caps2 = 0, caps3 = 0, caps4 = 0, reserved2 = 0;
    uint32_t format = 0, dimension = 3, misc = 0, arraySize = 1, alpha = 0;
};
static_assert(sizeof(DdsHeader) == 148);
struct Candidate {
    std::string name;
    int slot = -1;
    DXGI_FORMAT sourceFormat{};
    DXGI_FORMAT fileFormat{};
    uintptr_t resourceId = 0;
    std::shared_ptr<CapturedImage> snapshot;
    bool beforeClear = false;
    std::string bindings;
    std::string stage;
    uint64_t frame = 0;
    unsigned ordinal = 0, flags = 0, stencil = 0;
    float clearValue = 0;
    uint32_t width = 0, height = 0;
};
class Snapshot final : public CapturedImage {
public:
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> texture;
    D3D11_TEXTURE2D_DESC desc{};
    DXGI_FORMAT format{};
    std::string bindings;
    bool candidatesRequested = false;
    std::vector<Candidate> candidates;
    unsigned earlyObserved = 0, earlySkipped = 0;
    std::vector<std::string> passEvents;
    unsigned passEventsDropped = 0;
    unsigned enginePostObserved = 0, enginePostSkipped = 0;
    std::vector<uint8_t> pixels;
    bool cpuReady = false;
    const std::string& BindingsJson() const override { return bindings; }
    bool PrepareForAsyncWrite() override {
        if (cpuReady) return true;
        const auto bpp = BytesPerPixel(format);
        if (!context || !texture || !bpp) return false;
        const size_t pitch = size_t(desc.Width) * bpp;
        const size_t sliceBytes = pitch * desc.Height;
        std::vector<uint8_t> owned(sliceBytes * desc.ArraySize);
        for (UINT slice = 0; slice < desc.ArraySize; ++slice) {
            const auto sub = D3D11CalcSubresource(0, slice, desc.MipLevels);
            D3D11_MAPPED_SUBRESOURCE map{};
            const auto hr = context->Map(texture.Get(), sub, D3D11_MAP_READ, 0, &map);
            if (FAILED(hr)) { Log("capture_readback_failed hr=0x%08lx", static_cast<unsigned long>(hr)); return false; }
            const bool valid = map.pData && map.RowPitch >= pitch;
            if (valid) for (UINT y = 0; y < desc.Height; ++y)
                std::memcpy(owned.data() + slice * sliceBytes + y * pitch,
                    static_cast<const uint8_t*>(map.pData) + size_t(y) * map.RowPitch, pitch);
            context->Unmap(texture.Get(), sub);
            if (!valid) return false;
        }
        pixels = std::move(owned);
        // Never send GPU resources or the immediate context to the IO worker.
        texture.Reset(); context.Reset();
        for (auto& candidate : candidates) if (candidate.snapshot) {
            bool ready = false;
            try { ready = candidate.snapshot->PrepareForAsyncWrite(); }
            catch (...) { Log("capture_candidate_readback_failed exception"); }
            if (!ready) candidate.snapshot.reset(); // manifest records saved=false
        }
        cpuReady = true;
        return true;
    }
    void WriteCandidates(const ViewLayout& view, const std::wstring& base) override {
        if (!candidatesRequested) return;
        std::ostringstream out; out.imbue(std::locale::classic());
        out << "{\n  \"schema\":4, \"stage\":\"image_release_with_earlier_copies\",\n"
            << "  \"validated_neural_guides\":false, \"bound_resources_may_be_stale\":true,\n"
            << "  \"selection\":\"bound depth and first eye-sized RGBA16F pixel-shader resource; single slice/mip/sample only\",\n"
            << "  \"render_pass_selection\":\"latest complete HDR/depth/RG16F triplet within copy budget\",\n"
            << "  \"engine_post_selection\":\"304-byte PS b1, matching HDR t1, R16F 1x1 t30, full-eye output and viewport; binding signature, not shader bytecode identity\",\n"
            << "  \"pre_clear_observed\":" << earlyObserved << ",\"pre_clear_skipped_or_evicted\":" << earlySkipped << ",\n"
            << "  \"engine_post_observed\":" << enginePostObserved << ",\"engine_post_skipped\":" << enginePostSkipped << ",\n"
            << "  \"render_pass_events_dropped\":" << passEventsDropped << ",\n  \"render_pass_events\":[";
        for (size_t n = 0; n < passEvents.size(); ++n) { if (n) out << ','; out << passEvents[n]; }
        out << "],\n  \"candidates\":[";
        bool comma = false;
        for (auto& candidate : candidates) {
            const std::string suffix = "-" + candidate.name + ".dds";
            auto imageView = view;
            if (candidate.width && candidate.height)
                imageView = {0,0,0,0,static_cast<int32_t>(candidate.width),static_cast<int32_t>(candidate.height)};
            const bool saved = candidate.snapshot && candidate.snapshot->WriteView(imageView,
                base + std::wstring(suffix.begin(), suffix.end()));
            if (comma) out << ',';
            comma = true;
            out << "\n    {\"name\":\"" << candidate.name << "\",\"pixel_shader_slot\":" << candidate.slot
                << ",\"source_format\":" << candidate.sourceFormat << ",\"dds_format\":" << candidate.fileFormat
                << ",\"resource_id\":\"0x" << std::hex << candidate.resourceId << std::dec
                << "\",\"width\":" << imageView.width << ",\"height\":" << imageView.height
                << ",\"file_suffix\":\"" << suffix << "\",\"saved\":" << (saved ? "true" : "false")
                << ",\"stage\":\"" << (!candidate.stage.empty() ? candidate.stage.c_str() :
                    candidate.beforeClear ? "before_ClearDepthStencilView" : "before_xrReleaseSwapchainImage") << '"';
            if (candidate.beforeClear) {
                out << ",\"source_frame\":" << candidate.frame << ",\"clear_ordinal\":" << candidate.ordinal
                    << ",\"clear_flags\":" << candidate.flags << ",\"clear_stencil\":" << candidate.stencil
                    << ",\"clear_depth\":";
                if (std::isfinite(candidate.clearValue)) out << candidate.clearValue; else out << "null";
            }
            if (!candidate.stage.empty()) out << ",\"source_frame\":" << candidate.frame << ",\"pass_ordinal\":" << candidate.ordinal;
            if (candidate.beforeClear || !candidate.stage.empty()) {
                const auto bindingSuffix = "-" + candidate.name + "-bindings.json";
                FILE* file = _wfopen((base + std::wstring(bindingSuffix.begin(), bindingSuffix.end())).c_str(), L"wb");
                bool bindingSaved = false;
                if (file) {
                    bindingSaved = !candidate.bindings.empty() && std::fwrite(candidate.bindings.data(), 1, candidate.bindings.size(), file) == candidate.bindings.size();
                    if (std::fclose(file)) bindingSaved = false;
                }
                out << ",\"bindings_suffix\":\"" << bindingSuffix << "\",\"bindings_saved\":" << (bindingSaved ? "true" : "false");
            }
            out << '}';
            Log("capture_candidate name=%s slot=%d saved=%d validated_neural_guides=false",
                candidate.name.c_str(), candidate.slot, saved);
        }
        out << "\n  ]\n}\n";
        const auto json = out.str();
        FILE* file = _wfopen((base + L"-candidates.json").c_str(), L"wb");
        bool saved = false;
        if (file) {
            saved = std::fwrite(json.data(), 1, json.size(), file) == json.size();
            if (std::fclose(file)) saved = false;
        }
        Log("capture_candidate_manifest saved=%d count=%zu", saved, candidates.size());
    }
    bool WriteView(const ViewLayout& view, const std::wstring& filename) override {
        const auto bpp = BytesPerPixel(format);
        if (!bpp || !Fits(desc.Width, desc.Height, desc.ArraySize, view)) {
            Log("capture_view_rejected format=%u size=%ux%u array=%u slice=%u rect=%d,%d,%d,%d",
                static_cast<unsigned>(format), desc.Width, desc.Height, desc.ArraySize,
                view.slice, view.x, view.y, view.width, view.height);
            return false;
        }
        const auto sub = D3D11CalcSubresource(0, view.slice, desc.MipLevels);
        D3D11_MAPPED_SUBRESOURCE map{};
        if (cpuReady) {
            map.RowPitch = desc.Width * bpp;
            map.pData = pixels.data() + size_t(view.slice) * desc.Height * map.RowPitch;
        } else {
            const auto hr = context->Map(texture.Get(), sub, D3D11_MAP_READ, 0, &map);
            if (FAILED(hr)) { Log("capture_map_failed hr=0x%08lx", static_cast<unsigned long>(hr)); return false; }
        }
        FILE* f = _wfopen(filename.c_str(), L"wb");
        bool ok = false;
        if (f) {
            DdsHeader h;
            h.width = static_cast<uint32_t>(view.width); h.height = static_cast<uint32_t>(view.height);
            h.pitch = h.width * bpp; h.format = static_cast<uint32_t>(format);
            ok = map.RowPitch >= (static_cast<uint64_t>(view.x) + h.width) * bpp &&
                std::fwrite(&h, sizeof(h), 1, f) == 1;
            for (uint32_t y = 0; ok && y < h.height; ++y) {
                const auto row = static_cast<const uint8_t*>(map.pData) +
                    (static_cast<uint64_t>(view.y) + y) * map.RowPitch + static_cast<uint64_t>(view.x) * bpp;
                ok = std::fwrite(row, h.pitch, 1, f) == 1;
            }
            if (std::fclose(f)) ok = false;
        } else Log("capture_file_open_failed errno=%d", errno);
        if (!cpuReady) context->Unmap(texture.Get(), sub);
        if (!ok) { Log("capture_write_failed errno=%d row_pitch=%u", errno, map.RowPitch); DeleteFileW(filename.c_str()); }
        return ok;
    }
};
// Only inspect the currently bound views, and take owned copies now. Never
// retain a game texture for later readback: it may be reused for the other eye.
void CaptureCandidates(ID3D11Device* device, Snapshot& result) {
    const auto& color = result.desc;
    if (color.ArraySize != 1 || color.SampleDesc.Count != 1) return;
    auto add = [&](ID3D11View* view, DXGI_FORMAT format, const char* name, int slot) {
        ComPtr<ID3D11Resource> resource; view->GetResource(resource.GetAddressOf());
        ComPtr<ID3D11Texture2D> texture;
        if (!resource || FAILED(resource.As(&texture))) return false;
        D3D11_TEXTURE2D_DESC d{}; texture->GetDesc(&d);
        if (d.Width != color.Width || d.Height != color.Height || d.ArraySize != 1 ||
            d.MipLevels != 1 || d.SampleDesc.Count != 1) return false;
        // Each extra copy is capped at 64 MiB; at most depth + one HDR image.
        if (uint64_t(d.Width) * d.Height * BytesPerPixel(format) > 64ull * 1024 * 1024) return false;
        if (format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
            d.Format != DXGI_FORMAT_R16G16B16A16_TYPELESS && d.Format != format) return false;
        if (format == DXGI_FORMAT_D24_UNORM_S8_UINT &&
            d.Format != DXGI_FORMAT_R24G8_TYPELESS && d.Format != format) return false;
        if (format == DXGI_FORMAT_D32_FLOAT && d.Format != DXGI_FORMAT_R32_TYPELESS && d.Format != format) return false;
        Candidate candidate;
        candidate.name = name; candidate.slot = slot; candidate.sourceFormat = d.Format;
        candidate.fileFormat = format; candidate.resourceId = reinterpret_cast<uintptr_t>(resource.Get());
        candidate.snapshot = CaptureImage(device, texture.Get(), format, false, false);
        result.candidates.push_back(std::move(candidate));
        return true;
    };
    ComPtr<ID3D11DepthStencilView> depth;
    result.context->OMGetRenderTargets(0, nullptr, depth.GetAddressOf());
    if (depth) {
        D3D11_DEPTH_STENCIL_VIEW_DESC d{}; depth->GetDesc(&d);
        if (d.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2D && d.Texture2D.MipSlice == 0 &&
            (d.Format == DXGI_FORMAT_D24_UNORM_S8_UINT || d.Format == DXGI_FORMAT_D32_FLOAT))
            add(depth.Get(), d.Format, "depth-candidate", -1);
    }
    for (UINT slot = 0; slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; ++slot) {
        ComPtr<ID3D11ShaderResourceView> srv;
        result.context->PSGetShaderResources(slot, 1, srv.GetAddressOf());
        if (!srv) continue;
        D3D11_SHADER_RESOURCE_VIEW_DESC d{}; srv->GetDesc(&d);
        if (d.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D && d.Texture2D.MostDetailedMip == 0 &&
            d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
            add(srv.Get(), d.Format, "hdr-candidate", static_cast<int>(slot))) break;
    }
}
}
std::shared_ptr<CapturedImage> CaptureImage(ID3D11Device* device, ID3D11Texture2D* source, int64_t xrFormat,
    bool inspectBindings, bool captureCandidates) {
    if (!device || !source || xrFormat < 0 || xrFormat > std::numeric_limits<uint32_t>::max()) {
        Log("capture_invalid_source device=%p source=%p format=%lld", static_cast<void*>(device),
            static_cast<void*>(source), static_cast<long long>(xrFormat));
        return {};
    }
    const auto format = static_cast<DXGI_FORMAT>(xrFormat);
    const auto bpp = BytesPerPixel(format);
    if (!bpp) { Log("capture_unsupported_format=%lld", static_cast<long long>(xrFormat)); return {}; }
    D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc);
    if (!desc.Width || !desc.Height || desc.MipLevels != 1 || !desc.ArraySize || desc.ArraySize > 4 ||
        static_cast<uint64_t>(desc.Width) * desc.Height * desc.ArraySize * bpp > 256ull * 1024 * 1024) {
        Log("capture_rejected_size=%ux%u arrays=%u mips=%u", desc.Width, desc.Height, desc.ArraySize, desc.MipLevels);
        return {};
    }
    auto result = std::make_shared<Snapshot>();
    device->GetImmediateContext(result->context.GetAddressOf());
    if (!result->context) { Log("capture_missing_immediate_context"); return {}; }
    if (inspectBindings) {
        try { result->bindings = InspectBindings(result->context.Get(), source); }
        catch (...) { Log("capture_bindings_failed exception; continuing color capture"); }
    }
    result->format = format;
    result->desc = desc;
    result->desc.SampleDesc = {1, 0};
    result->desc.Usage = D3D11_USAGE_STAGING;
    result->desc.BindFlags = result->desc.MiscFlags = 0;
    result->desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    // Depth-compatible staging resources have no depth binding. Use the
    // typeless member of the same format family; CopyResource preserves bits.
    if (format == DXGI_FORMAT_D24_UNORM_S8_UINT) result->desc.Format = DXGI_FORMAT_R24G8_TYPELESS;
    if (format == DXGI_FORMAT_D32_FLOAT) result->desc.Format = DXGI_FORMAT_R32_TYPELESS;
    auto hr = device->CreateTexture2D(&result->desc, nullptr, result->texture.GetAddressOf());
    if (FAILED(hr)) { Log("capture_staging_failed hr=0x%08lx", static_cast<unsigned long>(hr)); return {}; }
    ComPtr<ID3D11Texture2D> resolved;
    if (desc.SampleDesc.Count > 1) {
        UINT support = 0;
        if (FAILED(device->CheckFormatSupport(format, &support)) || !(support & D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE)) {
            Log("capture_resolve_unsupported format=%u", static_cast<unsigned>(format)); return {};
        }
        auto r = result->desc; r.Usage = D3D11_USAGE_DEFAULT; r.CPUAccessFlags = 0;
        hr = device->CreateTexture2D(&r, nullptr, resolved.GetAddressOf());
        if (FAILED(hr)) { Log("capture_resolve_texture_failed hr=0x%08lx", static_cast<unsigned long>(hr)); return {}; }
        for (UINT slice = 0; slice < desc.ArraySize; ++slice)
            result->context->ResolveSubresource(resolved.Get(), slice, source, slice, format);
        result->context->CopyResource(result->texture.Get(), resolved.Get());
    } else result->context->CopyResource(result->texture.Get(), source);
    if (captureCandidates) {
        result->candidatesRequested = true;
        // A multisampled color source cannot establish the candidate layout.
        if (desc.SampleDesc.Count == 1) {
            try { CaptureCandidates(device, *result); }
            catch (...) { Log("capture_candidates_failed exception; continuing color capture"); }
        }
        auto early = TakeDepthCapture(device);
        result->earlyObserved = early.observed; result->earlySkipped = early.skipped;
        result->passEvents = std::move(early.passEvents); result->passEventsDropped = early.passEventsDropped;
        result->enginePostObserved = early.enginePostObserved; result->enginePostSkipped = early.enginePostSkipped;
        for (auto& copy : early.copies) {
            Candidate c;
            c.name = copy.name.empty() ? "depth-before-clear-" + std::to_string(copy.ordinal) : copy.name;
            c.beforeClear = copy.stage.empty(); c.stage = copy.stage; c.resourceId = copy.resourceId;
            c.sourceFormat = static_cast<DXGI_FORMAT>(copy.sourceFormat);
            c.fileFormat = static_cast<DXGI_FORMAT>(copy.fileFormat);
            c.frame = copy.frame; c.ordinal = copy.ordinal; c.flags = copy.flags;
            c.stencil = copy.stencil; c.clearValue = copy.clearValue;
            c.slot = copy.slot; c.width = copy.width; c.height = copy.height;
            c.snapshot = std::move(copy.snapshot); result->candidates.push_back(std::move(c));
            result->candidates.back().bindings = std::move(copy.bindings);
        }
    }
    // Copy commands precede the downstream release in this immediate context.
    // Mapping the owned copy later waits for those commands, not the headset.
    return result;
}
}

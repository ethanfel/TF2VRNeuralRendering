#include "Bindings.h"
#include <cmath>
#include <cstdint>
#include <locale>
#include <sstream>
#include <wrl/client.h>
namespace tf2vrnr {
namespace {
using Microsoft::WRL::ComPtr;
// D3D11 Get* methods AddRef returned interfaces. Release every reference even
// if description/JSON allocation throws; no resource survives this snapshot.
template<class T, UINT N> struct Returned {
    T* values[N]{};
    ~Returned() { for (auto* p : values) if (p) p->Release(); }
    Returned() = default;
    Returned(const Returned&) = delete;
    Returned& operator=(const Returned&) = delete;
};
void Pointer(std::ostream& out, const void* p) {
    out << '"' << "0x" << std::hex << reinterpret_cast<uintptr_t>(p) << std::dec << '"';
}
void Number(std::ostream& out, float f) {
    if (std::isfinite(f)) out << f; else out << "null";
}
void Resource(std::ostream& out, ID3D11Resource* resource) {
    if (!resource) { out << "null"; return; }
    D3D11_RESOURCE_DIMENSION dimension{}; resource->GetType(&dimension);
    out << "{\"id\":"; Pointer(out, resource);
    out << ",\"dimension\":" << dimension;
    ComPtr<ID3D11Texture2D> texture;
    if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(texture.GetAddressOf())))) {
        D3D11_TEXTURE2D_DESC d{}; texture->GetDesc(&d);
        out << ",\"width\":" << d.Width << ",\"height\":" << d.Height
            << ",\"format\":" << d.Format << ",\"mip_levels\":" << d.MipLevels
            << ",\"array_size\":" << d.ArraySize << ",\"samples\":" << d.SampleDesc.Count
            << ",\"bind_flags\":" << d.BindFlags << ",\"usage\":" << d.Usage
            << ",\"depth_stencil_bind\":" << ((d.BindFlags & D3D11_BIND_DEPTH_STENCIL) ? "true" : "false");
    }
    out << '}';
}
void ViewResource(std::ostream& out, ID3D11View* view) {
    ComPtr<ID3D11Resource> resource;
    view->GetResource(resource.GetAddressOf());
    out << ",\"resource\":"; Resource(out, resource.Get());
}
void ShaderViews(std::ostream& out, ID3D11DeviceContext* ctx, bool compute) {
    Returned<ID3D11ShaderResourceView, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> views;
    if (compute) ctx->CSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, views.values);
    else ctx->PSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, views.values);
    out << '['; bool comma = false;
    for (UINT slot = 0; slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; ++slot) {
        auto* view = views.values[slot]; if (!view) continue;
        if (comma) out << ',';
        comma = true;
        D3D11_SHADER_RESOURCE_VIEW_DESC d{}; view->GetDesc(&d);
        out << "\n    {\"slot\":" << slot << ",\"view_format\":" << d.Format << ",\"view_dimension\":" << d.ViewDimension;
        if (d.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D)
            out << ",\"first_mip\":" << d.Texture2D.MostDetailedMip << ",\"mip_levels\":" << d.Texture2D.MipLevels;
        else if (d.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY)
            out << ",\"first_mip\":" << d.Texture2DArray.MostDetailedMip << ",\"mip_levels\":" << d.Texture2DArray.MipLevels
                << ",\"first_slice\":" << d.Texture2DArray.FirstArraySlice << ",\"slice_count\":" << d.Texture2DArray.ArraySize;
        else if (d.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY)
            out << ",\"first_slice\":" << d.Texture2DMSArray.FirstArraySlice << ",\"slice_count\":" << d.Texture2DMSArray.ArraySize;
        ViewResource(out, view); out << '}';
    }
    out << ']';
}
void Buffers(std::ostream& out, ID3D11DeviceContext* ctx, bool vertex) {
    Returned<ID3D11Buffer, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT> buffers;
    if (vertex) ctx->VSGetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, buffers.values);
    else ctx->PSGetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, buffers.values);
    out << '['; bool comma = false;
    for (UINT slot = 0; slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; ++slot) {
        auto* buffer = buffers.values[slot]; if (!buffer) continue;
        if (comma) out << ',';
        comma = true;
        D3D11_BUFFER_DESC d{}; buffer->GetDesc(&d);
        out << "{\"slot\":" << slot << ",\"id\":"; Pointer(out, buffer);
        out << ",\"byte_width\":" << d.ByteWidth << '}';
    }
    out << ']';
}
}
std::string InspectBindings(ID3D11DeviceContext* context, ID3D11Texture2D* color, const char* stage) {
    if (!context) return {};
    std::ostringstream out; out.imbue(std::locale::classic());
    // Stage is an internal literal supplied by the capture points.
    out << "{\n  \"schema\": 1, \"stage\": \"" << stage << "\",\n"
        << "  \"validated_neural_guides\": false, \"bound_resources_may_be_stale\": true,\n"
        << "  \"context_type\":" << context->GetType() << ",\n  \"submitted_color\":";
    Resource(out, color);
    ComPtr<ID3D11PixelShader> shader;
    context->PSGetShader(shader.GetAddressOf(), nullptr, nullptr);
    out << ",\n  \"pixel_shader\":"; Pointer(out, shader.Get());
    Returned<ID3D11RenderTargetView, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> rtvs;
    ComPtr<ID3D11DepthStencilView> dsv;
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs.values, dsv.GetAddressOf());
    out << ",\n  \"render_targets\":["; bool comma = false;
    for (UINT slot = 0; slot < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++slot) {
        auto* view = rtvs.values[slot]; if (!view) continue;
        if (comma) out << ',';
        comma = true;
        D3D11_RENDER_TARGET_VIEW_DESC d{}; view->GetDesc(&d);
        out << "{\"slot\":" << slot << ",\"view_format\":" << d.Format << ",\"view_dimension\":" << d.ViewDimension;
        if (d.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D) out << ",\"mip\":" << d.Texture2D.MipSlice;
        else if (d.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY)
            out << ",\"mip\":" << d.Texture2DArray.MipSlice << ",\"first_slice\":" << d.Texture2DArray.FirstArraySlice
                << ",\"slice_count\":" << d.Texture2DArray.ArraySize;
        ViewResource(out, view); out << '}';
    }
    out << "],\n  \"depth_stencil\":";
    if (dsv) {
        D3D11_DEPTH_STENCIL_VIEW_DESC d{}; dsv->GetDesc(&d);
        out << "{\"view_format\":" << d.Format << ",\"view_dimension\":" << d.ViewDimension;
        if (d.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2D) out << ",\"mip\":" << d.Texture2D.MipSlice;
        else if (d.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DARRAY)
            out << ",\"mip\":" << d.Texture2DArray.MipSlice << ",\"first_slice\":" << d.Texture2DArray.FirstArraySlice
                << ",\"slice_count\":" << d.Texture2DArray.ArraySize;
        ViewResource(out, dsv.Get()); out << '}';
    } else out << "null";
    out << ",\n  \"pixel_shader_resources\":"; ShaderViews(out, context, false);
    out << ",\n  \"compute_shader_resources\":"; ShaderViews(out, context, true);
    out << ",\n  \"pixel_constant_buffers\":"; Buffers(out, context, false);
    out << ",\n  \"vertex_constant_buffers\":"; Buffers(out, context, true);
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    context->RSGetViewports(&count, viewports);
    out << ",\n  \"viewports\":[";
    for (UINT n = 0; n < count && n < D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE; ++n) {
        if (n) out << ',';
        const auto& v = viewports[n];
        out << "{\"x\":"; Number(out, v.TopLeftX);
        out << ",\"y\":"; Number(out, v.TopLeftY);
        out << ",\"width\":"; Number(out, v.Width);
        out << ",\"height\":"; Number(out, v.Height);
        out << ",\"min_depth\":"; Number(out, v.MinDepth);
        out << ",\"max_depth\":"; Number(out, v.MaxDepth); out << '}';
    }
    out << "]\n}\n";
    return out.str();
}
}

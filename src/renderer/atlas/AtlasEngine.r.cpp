// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "AtlasEngine.h"
#include "AtlasEngine.h"

#include "dwrite.h"

// #### NOTE ####
// If you see any code in here that contains "_api." you might be seeing a race condition.
// The AtlasEngine::Present() method is called on a background thread without any locks,
// while any of the API methods (like AtlasEngine::Invalidate) might be called concurrently.
// The usage of the _r field is safe as its members are in practice
// only ever written to by the caller of Present() (the "Renderer" class).
// The _api fields on the other hand are concurrently written to by others.

#pragma warning(disable : 4100) // '...': unreferenced formal parameter
// Disable a bunch of warnings which get in the way of writing performant code.
#pragma warning(disable : 26429) // Symbol 'data' is never tested for nullness, it can be marked as not_null (f.23).
#pragma warning(disable : 26446) // Prefer to use gsl::at() instead of unchecked subscript operator (bounds.4).
#pragma warning(disable : 26459) // You called an STL function '...' with a raw pointer parameter at position '...' that may be unsafe [...].
#pragma warning(disable : 26481) // Don't use pointer arithmetic. Use span instead (bounds.1).
#pragma warning(disable : 26482) // Only index into arrays using constant expressions (bounds.2).

// https://en.wikipedia.org/wiki/Inversion_list
template<size_t N>
constexpr bool isInInversionList(const std::array<wchar_t, N>& ranges, wchar_t needle)
{
    const auto beg = ranges.begin();
    const auto end = ranges.end();
    decltype(ranges.begin()) it;

    // Linear search is faster than binary search for short inputs.
    if constexpr (N < 16)
    {
        it = std::find_if(beg, end, [=](wchar_t v) { return needle < v; });
    }
    else
    {
        it = std::upper_bound(beg, end, needle);
    }

    const auto idx = it - beg;
    return (idx & 1) != 0;
}

template<typename T = D2D1_COLOR_F>
constexpr T colorFromU32(uint32_t rgba)
{
    const auto r = static_cast<float>((rgba >> 0) & 0xff) / 255.0f;
    const auto g = static_cast<float>((rgba >> 8) & 0xff) / 255.0f;
    const auto b = static_cast<float>((rgba >> 16) & 0xff) / 255.0f;
    const auto a = static_cast<float>((rgba >> 24) & 0xff) / 255.0f;
    return { r, g, b, a };
}

using namespace Microsoft::Console::Render;

#pragma region IRenderEngine

// Present() is called without the console buffer lock being held.
// --> Put as much in here as possible.
[[nodiscard]] HRESULT AtlasEngine::Present() noexcept
try
{
    const til::rect fullRect{ 0, 0, _r.cellCount.x, _r.cellCount.y };

    // A change in the selection or background color (etc.) forces a full redraw.
    if (WI_IsFlagSet(_r.invalidations, RenderInvalidations::ConstBuffer) || _r.customPixelShader)
    {
        _r.dirtyRect = fullRect;
    }

    if (!_r.dirtyRect)
    {
        return S_OK;
    }

    // See documentation for IDXGISwapChain2::GetFrameLatencyWaitableObject method:
    // > For every frame it renders, the app should wait on this handle before starting any rendering operations.
    // > Note that this requirement includes the first frame the app renders with the swap chain.
    assert(debugGeneralPerformance || _r.frameLatencyWaitableObjectUsed);

    if (!_r.atlasBuffer)
    {
        {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = 2048;
            desc.Height = 2048;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc = { 1, 0 };
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            THROW_IF_FAILED(_r.device->CreateTexture2D(&desc, nullptr, _r.atlasBuffer.addressof()));
            THROW_IF_FAILED(_r.device->CreateShaderResourceView(_r.atlasBuffer.get(), nullptr, _r.atlasView.addressof()));
        }

        {
            _r.rectPackerData.resize(2048);
            stbrp_init_target(&_r.rectPacker, 2048, 2048, _r.rectPackerData.data(), gsl::narrow_cast<int>(_r.rectPackerData.size()));
        }

        {
            wil::com_ptr<IDWriteGdiInterop> interop;
            THROW_IF_FAILED(_sr.dwriteFactory->GetGdiInterop(interop.addressof()));

            wil::com_ptr<IDWriteBitmapRenderTarget> dwriteRenderTarget0;
            THROW_IF_FAILED(interop->CreateBitmapRenderTarget(nullptr, _r.dwriteRenderTargetSize.x, _r.dwriteRenderTargetSize.y, dwriteRenderTarget0.addressof()));
            _r.dwriteRenderTarget = dwriteRenderTarget0.query<IDWriteBitmapRenderTarget1>();

            THROW_IF_FAILED(_r.dwriteRenderTarget->SetPixelsPerDip(_r.pixelPerDIP));
            THROW_IF_FAILED(_r.dwriteRenderTarget->SetTextAntialiasMode(DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE));
        }

        {
            DWrite_GetRenderParams(_sr.dwriteFactory.get(), &_r.gamma, &_r.cleartypeEnhancedContrast, &_r.grayscaleEnhancedContrast, _r.renderingParams.addressof());

            //const auto surface = _r.atlasBuffer.query<IDXGISurface>();
            //
            //D2D1_RENDER_TARGET_PROPERTIES props{};
            //props.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
            //props.pixelFormat = { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED };
            //props.dpiX = static_cast<float>(_r.dpi);
            //props.dpiY = static_cast<float>(_r.dpi);
            //wil::com_ptr<ID2D1RenderTarget> renderTarget;
            //THROW_IF_FAILED(_sr.d2dFactory->CreateDxgiSurfaceRenderTarget(surface.get(), &props, renderTarget.addressof()));
            //_r.d2dRenderTarget = renderTarget.query<ID2D1DeviceContext>();
            //
            //// We don't really use D2D for anything except DWrite, but it
            //// can't hurt to ensure that everything it does is pixel aligned.
            //_r.d2dRenderTarget->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
            //// In case _api.realizedAntialiasingMode is D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE we'll
            //// continuously adjust it in AtlasEngine::_drawGlyph. See _drawGlyph.
            //_r.d2dRenderTarget->SetTextAntialiasMode(static_cast<D2D1_TEXT_ANTIALIAS_MODE>(_api.realizedAntialiasingMode));
            //// Ensure that D2D uses the exact same gamma as our shader uses.
            //_r.d2dRenderTarget->SetTextRenderingParams(_r.renderingParams.get());
        }
    }

    {
        for (const auto& row : _r.rows)
        {
            for (auto& placement : row)
            {
                const auto [it, ok] = _r.glyphCache.try_emplace(placement.key);
                if (ok)
                {
                    //DWRITE_FONT_METRICS metrics;
                    //placement.key.fontFace->GetMetrics(&metrics);
                    //
                    //DWRITE_GLYPH_METRICS glyphMetrics[4];
                    //placement.key.fontFace->GetDesignGlyphMetrics(placement.key.glyphs.data(), placement.key.glyphs.size(), &glyphMetrics[0], false);
                    //
                    //int maxBaseline = 0;
                    //int maxHeight = 0;
                    //int maxWidth = 0;
                    //
                    //for (size_t i = 0; i < placement.key.glyphs.size(); ++i)
                    //{
                    //    const auto& m = glyphMetrics[i];
                    //    const auto baseline = metrics.ascent - m.topSideBearing;
                    //    const auto height = baseline + metrics.descent - m.bottomSideBearing;
                    //    const auto width = static_cast<INT32>(m.advanceWidth) - m.leftSideBearing - m.rightSideBearing;
                    //    maxBaseline = std::max(maxBaseline, baseline);
                    //    maxHeight = std::max(maxHeight, height);
                    //    maxWidth = std::max(maxWidth, width);
                    //}

                    while (true)
                    {
                        const auto dc = _r.dwriteRenderTarget->GetMemoryDC();
                        const auto bitmap = static_cast<HBITMAP>(GetCurrentObject(dc, OBJ_BITMAP));

                        DIBSECTION dib;
                        GetObjectW(bitmap, sizeof(dib), &dib);

                        memset(dib.dsBm.bmBits, 0, dib.dsBm.bmWidthBytes * dib.dsBm.bmHeight);

                        DWRITE_GLYPH_RUN glyphRun;
                        glyphRun.fontFace = placement.key.fontFace.get();
                        glyphRun.fontEmSize = _r.fontMetrics.fontSizeInDIP;
                        glyphRun.glyphCount = gsl::narrow_cast<UINT32>(placement.key.glyphs.size());
                        glyphRun.glyphIndices = placement.key.glyphs.data();
                        glyphRun.glyphAdvances = nullptr;
                        glyphRun.glyphOffsets = nullptr;
                        glyphRun.isSideways = FALSE;
                        glyphRun.bidiLevel = 0;
                        RECT blackBox;
                        THROW_IF_FAILED(_r.dwriteRenderTarget->DrawGlyphRun(_r.dwriteRenderTargetOriginDIP.x, _r.dwriteRenderTargetOriginDIP.y, DWRITE_MEASURING_MODE_NATURAL, &glyphRun, _r.renderingParams.get(), 0xffffffff, &blackBox));

                        stbrp_rect rect{};
                        rect.w = blackBox.right - blackBox.left;
                        rect.h = blackBox.bottom - blackBox.top;

                        if (!rect.w || !rect.h)
                        {
                            break;
                        }

                        const auto outOfBounds = blackBox.left < 0 || blackBox.top < 0;
                        const auto tooSmall = blackBox.right > _r.dwriteRenderTargetSize.x || blackBox.bottom > _r.dwriteRenderTargetSize.y;
                        if (outOfBounds)
                        {
                            const auto x = roundf(_r.dwriteRenderTargetOriginDIP.x * _r.pixelPerDIP - blackBox.left);
                            const auto y = roundf(_r.dwriteRenderTargetOriginDIP.y * _r.pixelPerDIP - blackBox.top);
                            _r.dwriteRenderTargetOrigin.x = gsl::narrow_cast<i32>(x);
                            _r.dwriteRenderTargetOrigin.y = gsl::narrow_cast<i32>(y);
                            _r.dwriteRenderTargetOriginDIP.x = x * _r.dipPerPixel;
                            _r.dwriteRenderTargetOriginDIP.y = y * _r.dipPerPixel;
                        }
                        if (tooSmall)
                        {
                            _r.dwriteRenderTargetSize.x = _r.dwriteRenderTargetOrigin.x * 2 + rect.w;
                            _r.dwriteRenderTargetSize.y = _r.dwriteRenderTargetOrigin.y * 2 + rect.h;
                            _r.dwriteRenderTarget->Resize(_r.dwriteRenderTargetSize.x, _r.dwriteRenderTargetSize.y);
                        }
                        if (outOfBounds || tooSmall)
                        {
                            continue;
                        }

                        if (!stbrp_pack_rects(&_r.rectPacker, &rect, 1))
                        {
                            __debugbreak();
                            break;
                        }

                        D3D11_BOX box;
                        box.left = rect.x;
                        box.top = rect.y;
                        box.front = 0;
                        box.right = rect.x + rect.w;
                        box.bottom = rect.y + rect.h;
                        box.back = 1;
                        _r.deviceContext->UpdateSubresource1(_r.atlasBuffer.get(), 0, &box, dib.dsBm.bmBits, dib.dsBm.bmWidthBytes, 0, D3D11_COPY_NO_OVERWRITE);

                        it->second.uv.x = static_cast<f32>(rect.x);
                        it->second.uv.y = static_cast<f32>(rect.y);
                        it->second.wh.x = static_cast<f32>(rect.w);
                        it->second.wh.y = static_cast<f32>(rect.h);
                        it->second.baselineOrigin.x = 0;
                        it->second.baselineOrigin.y = _r.fontMetrics.baselineInDIP * _r.pixelPerDIP;

                        break;
                    }
                }

                            
                { XMFLOAT3(1.f, 1.f, 0.f), XMFLOAT2(1.f, 0.f) },
                { XMFLOAT3(1.f, -1.f, 0.f), XMFLOAT2(1.f, 1.f) },
                { XMFLOAT3(-1.f, 1.f, 0.f), XMFLOAT2(0.f, 0.f) },
                { XMFLOAT3(-1.f, -1.f, 0.f), XMFLOAT2(0.f, 1.f) },

                auto& ref = _r.vertexData.emplace_back();
                ref.position.x = placement.baseline.x;
                ref.position.y = placement.baseline.y;
                ref.texcoord = it->second.uv;
                ref.color = 0xffffffff;
            }
        }
    }

    if (_r.vertexData.empty())
    {
        return S_OK;
    }

    {
#pragma warning(suppress : 26494) // Variable 'mapped' is uninitialized. Always initialize an object (type.5).
        D3D11_MAPPED_SUBRESOURCE mapped;
        THROW_IF_FAILED(_r.deviceContext->Map(_r.vertexBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
        memcpy(mapped.pData, _r.vertexData.data(), _r.vertexData.size() * sizeof(VertexData));
        _r.deviceContext->Unmap(_r.vertexBuffer.get(), 0);
    }

    {
        D3D11_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(_api.sizeInPixel.x);
        viewport.Height = static_cast<float>(_api.sizeInPixel.y);

        // IA: Input Assembler
        _r.deviceContext->IASetInputLayout(_r.textInputLayout.get());
        static constexpr UINT stride = sizeof(VertexData);
        static constexpr UINT offset = 0;
        _r.deviceContext->IASetVertexBuffers(0, 1, _r.vertexBuffer.addressof(), &stride, &offset);
        _r.deviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

        // VS: Vertex Shader
        _r.deviceContext->VSSetShader(_r.vertexShader.get(), nullptr, 0);
        _r.deviceContext->VSSetConstantBuffers(0, 0, nullptr);

        // RS: Rasterizer Stage
        _r.deviceContext->RSSetViewports(1, &viewport);

        // PS: Pixel Shader
        _r.deviceContext->PSSetShader(_r.pixelShader.get(), nullptr, 0);

        // OM: Output Merger
        _r.deviceContext->OMSetBlendState(_r.textBlendState.get(), nullptr, 0xffffffff);
        _r.deviceContext->OMSetRenderTargets(1, _r.renderTargetView.addressof(), nullptr);

        _r.deviceContext->Draw(gsl::narrow_cast<UINT>(_r.vertexData.size()), 0);
    }

    if (_r.dirtyRect != fullRect)
    {
        auto dirtyRectInPx = _r.dirtyRect;
        dirtyRectInPx.left *= _r.fontMetrics.cellSize.x;
        dirtyRectInPx.top *= _r.fontMetrics.cellSize.y;
        dirtyRectInPx.right *= _r.fontMetrics.cellSize.x;
        dirtyRectInPx.bottom *= _r.fontMetrics.cellSize.y;

        RECT scrollRect{};
        POINT scrollOffset{};
        DXGI_PRESENT_PARAMETERS params{
            .DirtyRectsCount = 1,
            .pDirtyRects = dirtyRectInPx.as_win32_rect(),
        };

        if (_r.scrollOffset)
        {
            scrollRect = {
                0,
                std::max<til::CoordType>(0, _r.scrollOffset),
                _r.cellCount.x,
                _r.cellCount.y + std::min<til::CoordType>(0, _r.scrollOffset),
            };
            scrollOffset = {
                0,
                _r.scrollOffset,
            };

            scrollRect.top *= _r.fontMetrics.cellSize.y;
            scrollRect.right *= _r.fontMetrics.cellSize.x;
            scrollRect.bottom *= _r.fontMetrics.cellSize.y;

            scrollOffset.y *= _r.fontMetrics.cellSize.y;

            params.pScrollRect = &scrollRect;
            params.pScrollOffset = &scrollOffset;
        }

        THROW_IF_FAILED(_r.swapChain->Present1(1, 0, &params));
    }
    else
    {
        THROW_IF_FAILED(_r.swapChain->Present(1, 0));
    }

    _r.waitForPresentation = true;

    if (!_r.dxgiFactory->IsCurrent())
    {
        WI_SetFlag(_api.invalidations, ApiInvalidations::Device);
    }

    return S_OK;
}
catch (const wil::ResultException& exception)
{
    // TODO: this writes to _api.
    return _handleException(exception);
}
CATCH_RETURN()

[[nodiscard]] bool AtlasEngine::RequiresContinuousRedraw() noexcept
{
    return debugGeneralPerformance || _r.requiresContinuousRedraw;
}

void AtlasEngine::WaitUntilCanRender() noexcept
{
    // IDXGISwapChain2::GetFrameLatencyWaitableObject returns an auto-reset event.
    // Once we've waited on the event, waiting on it again will block until the timeout elapses.
    // _r.waitForPresentation guards against this.
    if (std::exchange(_r.waitForPresentation, false))
    {
        WaitForSingleObjectEx(_r.frameLatencyWaitableObject.get(), 100, true);
#ifndef NDEBUG
        _r.frameLatencyWaitableObjectUsed = true;
#endif
    }
}

#pragma endregion

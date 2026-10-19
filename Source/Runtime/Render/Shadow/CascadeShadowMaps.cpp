#include "EnginePCH.h"
#include "CascadeShadowMaps.h"

#include <utility>

#include "Render/RenderCommand.h"

bool FCascadeShadowMaps::Init(uint32 InResolution, uint32 InCascadeCount)
{

    if (InResolution == 0 || InCascadeCount == 0 || InCascadeCount > MaxCascades)
    {
        return false;
    }

    ID3D11Device *Device = RenderCommand::GetDevice();

    if (!Device)
        return false;

    ComPtr<ID3D11Texture2D> NewTexture;
    ComPtr<ID3D11ShaderResourceView> NewArraySRV;

    TArray<ComPtr<ID3D11DepthStencilView>> NewSliceDSVs;
    TArray<ComPtr<ID3D11ShaderResourceView>> NewSliceSRVs;

    NewSliceDSVs.SetNum(InCascadeCount);
    NewSliceSRVs.SetNum(InCascadeCount);

    D3D11_TEXTURE2D_DESC TextureDesc{};
    TextureDesc.Width = InResolution; // Slice 한장의 해상도
    TextureDesc.Height = InResolution; // Slice 한장의 해상도
    TextureDesc.MipLevels = 1;
    TextureDesc.ArraySize = InCascadeCount; // 같은 크기·포맷의 Slice를 캐스케이드 개수만큼 생성
    TextureDesc.Format = DXGI_FORMAT_R24G8_TYPELESS;
    TextureDesc.SampleDesc.Count = 1;
    TextureDesc.Usage = D3D11_USAGE_DEFAULT;
    TextureDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(Device->CreateTexture2D(&TextureDesc, nullptr, NewTexture.GetAddressOf())))
    {
        return false;
    }

    // 추후 CSM 그림자 샘플링에서 사용할 전체 배열 View.
    D3D11_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
    SRVDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    SRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    SRVDesc.Texture2DArray.MostDetailedMip = 0;
    SRVDesc.Texture2DArray.MipLevels = 1;
    SRVDesc.Texture2DArray.FirstArraySlice = 0;
    SRVDesc.Texture2DArray.ArraySize = InCascadeCount;

    if (FAILED(Device->CreateShaderResourceView(NewTexture.Get(), &SRVDesc, NewArraySRV.GetAddressOf())))
    {
        return false;
    }

    ///
    for (uint32 Index = 0; Index < InCascadeCount; ++Index)
    {
        // 해당 Slice에만 깊이를 기록한다.
        D3D11_DEPTH_STENCIL_VIEW_DESC DSVDesc{};
        DSVDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        DSVDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY; // 이 리소스를 2D 텍스처 배열로 읽겠다
        DSVDesc.Texture2DArray.MipSlice = 0;
        DSVDesc.Texture2DArray.FirstArraySlice = Index;
        DSVDesc.Texture2DArray.ArraySize = 1;

        if (FAILED(Device->CreateDepthStencilView(NewTexture.Get(), &DSVDesc, NewSliceDSVs[Index].GetAddressOf())))
        {
            return false;
        }

        // ImGui 미리보기에서는 이 View 하나를 이미지 하나로 사용할 예정
        SRVDesc.Texture2DArray.FirstArraySlice = Index;
        SRVDesc.Texture2DArray.ArraySize = 1;

        if (FAILED(Device->CreateShaderResourceView(NewTexture.Get(), &SRVDesc, NewSliceSRVs[Index].GetAddressOf())))
        {
            return false;
        }
    }

    Texture = std::move(NewTexture);
    ArraySRV = std::move(NewArraySRV);
    SliceDSVs = std::move(NewSliceDSVs);
    SliceSRVs = std::move(NewSliceSRVs);

    Resolution = InResolution;
    CascadeCount = InCascadeCount;

    return true;
}

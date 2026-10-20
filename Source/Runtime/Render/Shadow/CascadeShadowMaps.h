#pragma once

#include "EnginePCH.h"

class FCascadeShadowMaps
{
public:

    static constexpr uint32 MaxCascades = 4;

    bool Init(uint32 InResolution = 2048, uint32 InCascadeCount = 4);

    uint32 GetResolution() const {return Resolution;}
    uint32 GetCascadeCount() const {return CascadeCount;}

    ID3D11DepthStencilView *GetSliceDSV(uint32 Index) const
    {
        return SliceDSVs.IsValidIndex(Index) ? SliceDSVs[Index].Get() : nullptr;
    }

    ID3D11ShaderResourceView* GetSliceSRV(uint32 Index) const
    {
        return SliceSRVs.IsValidIndex(Index) ? SliceSRVs[Index].Get() : nullptr;
    }

    ID3D11ShaderResourceView *GetArraySRV() const
    {
        return ArraySRV.Get();
    }

    bool CopyFrom(const FCascadeShadowMaps &Source);

private:
    uint32 Resolution = 0;
    uint32 CascadeCount = 0;

    ComPtr<ID3D11Texture2D> Texture;
    ComPtr<ID3D11ShaderResourceView> ArraySRV;

    TArray<ComPtr<ID3D11DepthStencilView>> SliceDSVs;
    TArray<ComPtr<ID3D11ShaderResourceView>> SliceSRVs;

};

#pragma once

#include <array>

#include "Math/EngineMath.h"
#include "Math/Box.h"

#include "Render/Texture2D.h"

struct FShadowPassDesc
{
    ID3D11DepthStencilView *DepthView = nullptr;
    uint32 Width = 0;
    uint32 Height = 0;
};

class FShadowRenderer
{
public:
    bool Init(uint32 ShadowMapSize = 2048);

    bool BeginShadowPass(const FShadowPassDesc &Desc);
    void EndShadowPass();

    FMatrix BuildDirectionalLightView(const std::array<FVector, 8>& FrustumCornersWorld, const FVector& LightDirection) const;
    FBox CalculateLightSpaceBounds(const std::array<FVector, 8>& FrustumCornersWorld, const FMatrix& LightView) const;
    FMatrix BuildDirectionalLightProjection(const FBox& LightSpaceBounds) const;

    FMatrix BuildDirectionalLightVP(const FMatrix& CameraVP, float CameraNear, float CameraFar, float ShadowDistance, const FVector& LightDirection) const;

    std::array<FVector, 8> LimitFrustumToShadowDistance(const std::array<FVector, 8>& Corners, float CameraNear, float CameraFar, float ShadowDistance) const;

    FTexture2D *GetDirectionalShadowMap() const { return DirectionalShadowMap.get(); }
    FTexture2D *GetSpotShadowMap() const { return SpotShadowMap.get(); }    

private:
    TUniquePtr<FTexture2D> DirectionalShadowMap;
    TUniquePtr<FTexture2D> SpotShadowMap;
};

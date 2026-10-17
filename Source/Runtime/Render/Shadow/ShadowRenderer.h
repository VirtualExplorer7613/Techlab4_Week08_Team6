#pragma once

#include <array>

#include "Math/EngineMath.h"
#include "Math/Box.h"

#include "Render/Buffer.h"
#include "Render/Texture2D.h"
#include "Render/RenderingInfo.h"

class FShadowRenderer
{
public:
    FMatrix BuildDirectionalLightView(const std::array<FVector, 8>& FrustumCornersWorld, const FVector& LightDirection) const;
    FBox CalculateLightSpaceBounds(const std::array<FVector, 8>& FrustumCornersWorld, const FMatrix& LightView) const;
    FMatrix BuildDirectionalLightProjection(const FBox& LightSpaceBounds) const;

    FMatrix BuildDirectionalLightVP(const FMatrix& CameraVP, float CameraNear, float CameraFar, float ShadowDistance, const FVector& LightDirection) const;

    std::array<FVector, 8> LimitFrustumToShadowDistance(const std::array<FVector, 8>& Corners, float CameraNear, float CameraFar, float ShadowDistance) const;

    bool Init(uint32 ShadowMapSize = 2048);

    void BeginShadowPass();
    void EndShadowPass();

    void BindLightVP(const FMatrix& LightVP, bool bEnabled) const;

    FTexture2D* GetShadowMap() const { return ShadowMap.get(); }

private:
    TUniquePtr<FTexture2D> ShadowMap;
    FRenderingInfo ShadowRenderingInfo;
    TUniquePtr<FConstantBuffer> ShadowCB;
};
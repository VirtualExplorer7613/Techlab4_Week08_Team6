#pragma once

#include <array>
#include "Math/EngineMath.h"
#include "Math/Box.h"

class FShadowRenderer
{
public:
    FMatrix BuildDirectionalLightView(const std::array<FVector, 8>& FrustumCornersWorld, const FVector& LightDirection) const;
    FBox CalculateLightSpaceBounds(const std::array<FVector, 8>& FrustumCornersWorld, const FMatrix& LightView) const;
    FMatrix BuildDirectionalLightProjection(const FBox& LightSpaceBounds) const;

    FMatrix BuildDirectionalLightVP(const FMatrix& CameraVP, float CameraNear, float CameraFar, float ShadowDistance, const FVector& LightDirection) const;

    std::array<FVector, 8> LimitFrustumToShadowDistance(const std::array<FVector, 8>& Corners, float CameraNear, float CameraFar, float ShadowDistance) const;
};
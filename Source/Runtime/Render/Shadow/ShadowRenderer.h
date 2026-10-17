#pragma once

#include <array>

#include "Math/EngineMath.h"
#include "Math/Box.h"
#include "Math/Vector.h"

#include "Render/Texture2D.h"
#include "Render/TextureCube.h"

struct FShadowPassDesc
{
    ID3D11DepthStencilView *DepthView = nullptr;
    uint32 Width = 0;
    uint32 Height = 0;
};

const FCubeFaceDirection CubeFaceDirections[6] = {
    {FVector(1.0f, 0.0f, 0.0f), FVector(0.0f, 1.0f, 0.0f)},  // +X
    {FVector(-1.0f, 0.0f, 0.0f), FVector(0.0f, 1.0f, 0.0f)}, // -X
    {FVector(0.0f, 1.0f, 0.0f), FVector(0.0f, 0.0f, -1.0f)}, // +Y
    {FVector(0.0f, -1.0f, 0.0f), FVector(0.0f, 0.0f, 1.0f)}, // -Y
    {FVector(0.0f, 0.0f, 1.0f), FVector(0.0f, 1.0f, 0.0f)},  // +Z
    {FVector(0.0f, 0.0f, -1.0f), FVector(0.0f, 1.0f, 0.0f)}  // -Z
};

class FShadowRenderer
{
public:
    bool Init(uint32 ShadowMapSize = 2048);

    bool BeginShadowPass(const FShadowPassDesc &Desc);
    void EndShadowPass();

    FBox CalculateLightSpaceBounds(const std::array<FVector, 8> &FrustumCornersWorld, const FMatrix &LightView) const;

    FMatrix BuildDirectionalLightView(const std::array<FVector, 8>& FrustumCornersWorld, const FVector& LightDirection) const;    
    FMatrix BuildDirectionalLightProjection(const FBox& LightSpaceBounds) const;
    FMatrix BuildDirectionalLightVP(const FMatrix& CameraVP, float CameraNear, float CameraFar, float ShadowDistance, const FVector& LightDirection) const;    

    std::array<FVector, 8> LimitFrustumToShadowDistance(const std::array<FVector, 8>& Corners, float CameraNear, float CameraFar, float ShadowDistance) const;

    FMatrix BuildPointLightView(const FVector &LightPosition, uint32 FaceIndex) const;
    FMatrix BuildPointLightProjection(float NearPlane, float FarPlane) const;
    FMatrix BuildPointLightVP(const FVector &LightPosition, float NearPlane, float FarPlane, uint32 FaceIndex) const;

    FTexture2D *GetDirectionalShadowMap() const { return DirectionalShadowMap.get(); }
    FTexture2D *GetSpotShadowMap() const { return SpotShadowMap.get(); }
    FTextureCube* GetPointLightShadowMap() const { return PointLightShadowMap.get(); }

private:
    TUniquePtr<FTexture2D> DirectionalShadowMap;
    TUniquePtr<FTextureCube> PointLightShadowMap;    
    TUniquePtr<FTexture2D> SpotShadowMap;
    TUniquePtr<FTextureCube> PointLightShadowMap;
};

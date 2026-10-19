#pragma once

#include <array>
#include <limits>

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

    // Frustum 중심과 광원 방향을 기준으로 Light View 행렬을 구성한다.
    FMatrix BuildDirectionalLightView(const std::array<FVector, 8>& FrustumCornersWorld, const FVector& LightDirection) const;

    // Frustum의 8개 꼭짓점을 Light Space로 변환해 경계 상자를 구한다.
    FBox CalculateLightSpaceBounds(const std::array<FVector, 8>& FrustumCornersWorld, const FMatrix& LightView) const;

    // Light Space의 X축은 깊이, Y/Z축은 화면 평면으로 사용해 직교투영 행렬을 구성한다.
    FMatrix BuildDirectionalLightProjection(const FBox& LightSpaceBounds) const;

    // 카메라 Frustum을 지정된 그림자 거리까지 제한해 Directional Light VP를 계산한다.
    FMatrix BuildDirectionalLightVP(const FMatrix& CameraVP, float CameraNear, float CameraFar, float ShadowDistance, const FVector& LightDirection) const;

    // Frustum 코너 8개를 받아 Directional Light VP를 계산한다.
    FMatrix BuildDirectionalLightVPFromCorners(const std::array<FVector, 8>& FrustumCornersWorld, const FVector& LightDirection, float MinCasterDepth = std::numeric_limits<float>::infinity()) const;

    // Frustum의 원거리 꼭짓점을 ShadowDistance에 맞춰 보간한다.
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
};

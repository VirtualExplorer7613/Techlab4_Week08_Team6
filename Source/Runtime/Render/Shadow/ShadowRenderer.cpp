#include "EnginePCH.h"
#include "ShadowRenderer.h"

#include "Math/Frustum.h"
#include "Render/RenderCommand.h"

#include <limits>

FMatrix FShadowRenderer::BuildDirectionalLightView(const std::array<FVector, 8>& FrustumCornersWorld, const FVector& LightDirection) const
{
    FVector Center(0.0f, 0.0f, 0.0f);

    for (const FVector& Corner : FrustumCornersWorld)
        Center += Corner;

    Center /= 8.0f;

    const FVector Forward = LightDirection.Normalized();
    const FVector WorldUp = std::abs(Forward.Dot(UpVector)) > 0.99f ? RightVector : UpVector;
    const FVector Right = WorldUp.Cross(Forward).Normalized();
    const FVector Up = Forward.Cross(Right);

    float MaxDistance = 0.0f;
    for (const FVector& Corner : FrustumCornersWorld)
        MaxDistance = std::max(MaxDistance, (Corner - Center).Size());

    const FVector LightPosition = Center - Forward * MaxDistance;

    const FMatrix LightWorld(
        FVector4(Forward.X, Forward.Y, Forward.Z, 0.0f),
        FVector4(Right.X, Right.Y, Right.Z, 0.0f),
        FVector4(Up.X, Up.Y, Up.Z, 0.0f),
        FVector4(LightPosition.X, LightPosition.Y, LightPosition.Z, 1.0f)
    );

    return LightWorld.Inverse();
}

FBox FShadowRenderer::CalculateLightSpaceBounds(const std::array<FVector, 8>& FrustumCornersWorld, const FMatrix& LightView) const
{
    const FVector FirstPoint = LightView.TransformPosition(FrustumCornersWorld[0]);
    FBox Box{ FirstPoint, FirstPoint };

    for (size_t i = 1; i < FrustumCornersWorld.size(); ++i)
    {
        const FVector Point = LightView.TransformPosition(FrustumCornersWorld[i]);
        Box.Expand(FBox{ Point, Point });
    }

    return Box;
}

FMatrix FShadowRenderer::BuildDirectionalLightProjection(const FBox& LightSpaceBounds) const
{
    const float Left = LightSpaceBounds.Min.Y;
    const float Right = LightSpaceBounds.Max.Y;
    const float Bottom = LightSpaceBounds.Min.Z;
    const float Top = LightSpaceBounds.Max.Z;
    const float Near = LightSpaceBounds.Min.X;
    const float Far = LightSpaceBounds.Max.X;

    const float HorizontalScale = 2.0f / (Right - Left);
    const float HorizontalOffset = -(Right + Left) / (Right - Left);

    const float VerticalScale = 2.0f / (Top - Bottom);
    const float VerticalOffset = -(Top + Bottom) / (Top - Bottom);

    const float DepthScale = 1.0f / (Far - Near);
    const float DepthOffset = -Near / (Far - Near);

    return FMatrix(
        0.0f, 0.0f, DepthScale, 0.0f,
        HorizontalScale, 0.0f, 0.0f, 0.0f,
        0.0f, VerticalScale, 0.0f, 0.0f,
        HorizontalOffset, VerticalOffset, DepthOffset, 1.0f
    );
}


FMatrix FShadowRenderer::BuildDirectionalLightVP(const FMatrix& CameraVP, float CameraNear, float CameraFar, float ShadowDistance, const FVector& LightDirection, uint32 ShadowResolution) const const
{
    const std::array<FVector, 8> Corners = GetFrustumCornersWorld(CameraVP);
    const std::array<FVector, 8> ShadowCorners = LimitFrustumToShadowDistance(Corners, CameraNear, CameraFar, ShadowDistance);

    return BuildDirectionalLightVPFromCorners(ShadowCorners, LightDirection, ShadowResolution);
}


FMatrix FShadowRenderer::BuildDirectionalLightVPFromCorners(const std::array<FVector, 8>& FrustumCornersWorld, const FVector& LightDirection, uint32 ShadowResolution, float MinCasterDepth) const
{
    FVector Center(0.0f, 0.0f, 0.0f);
    for (const FVector &Corner : FrustumCornersWorld)
        Center += Corner;
    Center /= 8.0f;

    // Frustum을 감싸는 구의 반경을 이용해 카메라 회전에도 투영 크기를 일정하게 유지한다.
    float Radius = 0.0f;
    for (const FVector &Corner : FrustumCornersWorld)
        Radius = std::max(Radius, (Corner - Center).Size());

    Radius = std::ceil(Radius * 16.0f) / 16.0f;

    const FMatrix LightView = BuildDirectionalLightView(FrustumCornersWorld, LightDirection);
    FBox LightSpaceBounds = CalculateLightSpaceBounds(FrustumCornersWorld, LightView);

    // Frustum보다 광원 쪽에 있는 Caster까지 깊이 범위를 확장한다.
    LightSpaceBounds.Min.X = std::min(LightSpaceBounds.Min.X, MinCasterDepth);

    const float Extent = Radius * 1.01f;
    LightSpaceBounds.Min.Y = -Extent;
    LightSpaceBounds.Max.Y = Extent;
    LightSpaceBounds.Min.Z = -Extent;
    LightSpaceBounds.Max.Z = Extent;

    FMatrix LightVP = LightView * BuildDirectionalLightProjection(LightSpaceBounds);

    // 실제 Shadow Map 해상도를 기준으로 투영 위치를 텍셀 격자에 정렬한다.
    if (ShadowResolution > 0)
    {
        const float HalfResolution = static_cast<float>(ShadowResolution) * 0.5f;
        const FVector Origin = LightVP.TransformPosition(FVector(0.0f, 0.0f, 0.0f));

        LightVP.M[3][0] += (std::round(Origin.X * HalfResolution) - Origin.X * HalfResolution) / HalfResolution;
        LightVP.M[3][1] += (std::round(Origin.Y * HalfResolution) - Origin.Y * HalfResolution) / HalfResolution;
    }

    return LightVP;
}


std::array<FVector, 8> FShadowRenderer::LimitFrustumToShadowDistance(const std::array<FVector, 8>& Corners, float CameraNear, float CameraFar, float ShadowDistance) const
{
    std::array<FVector, 8> Result = Corners;

    const float ClampedDistance = std::clamp(ShadowDistance, CameraNear, CameraFar);
    const float Ratio = (ClampedDistance - CameraNear) / (CameraFar - CameraNear);

    for (size_t i = 0; i < 4; ++i)
    {
        const FVector NearPoint = Corners[i];
        const FVector FarPoint = Corners[i + 4];

        const FVector NewFarPoint = NearPoint + (FarPoint - NearPoint) * Ratio;
        Result[i + 4] = NewFarPoint;
    }

    return Result;
}

FMatrix FShadowRenderer::BuildPointLightView(const FVector &LightPosition, uint32 FaceIndex) const
{
    const FVector Eye = LightPosition;
    const FVector Target = LightPosition + CubeFaceDirections[FaceIndex].Forward;
    const FVector Up = CubeFaceDirections[FaceIndex].Up;

    return FMatrix::MakeLookAt(Eye, Target, Up);
}

FMatrix FShadowRenderer::BuildPointLightProjection(float NearPlane, float FarPlane, uint32 RequestedSize) const
{
    float Size = (float)RequestedSize;
    float ExpandesFOV = 2.0f * atan(Size / (Size - 2.0f * (float)SHADOW_ATLAS_BORDER));

    return FMatrix::MakePerspective(ExpandesFOV, 1.0f, NearPlane, FarPlane);
}

FMatrix FShadowRenderer::BuildPointLightVP(const FVector &LightPosition, float NearPlane, float FarPlane, uint32 RequestedSize, uint32 FaceIndex) const
{
    return BuildPointLightView(LightPosition, FaceIndex) * BuildPointLightProjection(NearPlane, FarPlane, RequestedSize);
}

bool FShadowRenderer::Init(uint32 ShadowMapSize)
{
    D3D11_TEXTURE2D_DESC DirectionalDesc{};
    DirectionalDesc.Width = ShadowMapSize;
    DirectionalDesc.Height = ShadowMapSize;
    DirectionalDesc.MipLevels = 1;
    DirectionalDesc.ArraySize = 1;
    DirectionalDesc.Format = DXGI_FORMAT_R24G8_TYPELESS;
    DirectionalDesc.SampleDesc.Count = 1;
    DirectionalDesc.Usage = D3D11_USAGE_DEFAULT;
    DirectionalDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;

    DirectionalShadowMap = RenderCommand::CreateTexture2D(DirectionalDesc);

    if (!DirectionalShadowMap || !DirectionalShadowMap->GetRawPtr() || !DirectionalShadowMap->GetDSV() || !DirectionalShadowMap->GetSRV())
    {
        DirectionalShadowMap.reset();
        return false;
    }

    SpotShadowMap = RenderCommand::CreateTexture2D(DirectionalDesc);

    if (!SpotShadowMap || !SpotShadowMap->GetRawPtr() || !SpotShadowMap->GetDSV() || !SpotShadowMap->GetSRV())
    {
        SpotShadowMap.reset();
        return false;
    }

    D3D11_TEXTURE2D_DESC PointLightDesc{};
    PointLightDesc.Width = ShadowMapSize;
    PointLightDesc.Height = ShadowMapSize;
    PointLightDesc.MipLevels = 1;
    PointLightDesc.ArraySize = 6;
    PointLightDesc.Format = DXGI_FORMAT_R24G8_TYPELESS;
    PointLightDesc.SampleDesc.Count = 1;
    PointLightDesc.Usage = D3D11_USAGE_DEFAULT;
    PointLightDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    PointLightDesc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;

    PointLightShadowMap = RenderCommand::CreateTextureCube(PointLightDesc, {});

    if (!PointLightShadowMap || !PointLightShadowMap->GetRawPtr() || !PointLightShadowMap->GetFaceDSV(0) || !PointLightShadowMap->GetSRV())
    {
        PointLightShadowMap.reset();
        //return false;
    }

    D3D11_TEXTURE2D_DESC ShadowMapAtlasDesc{};
    ShadowMapAtlasDesc.Width = SHADOW_ATLAS_MAP_SIZE;
    ShadowMapAtlasDesc.Height = SHADOW_ATLAS_MAP_SIZE;
    ShadowMapAtlasDesc.MipLevels = 1;
    ShadowMapAtlasDesc.ArraySize = 1;
    ShadowMapAtlasDesc.Format = DXGI_FORMAT_R24G8_TYPELESS;
    ShadowMapAtlasDesc.SampleDesc.Count = 1;
    ShadowMapAtlasDesc.Usage = D3D11_USAGE_DEFAULT;
    ShadowMapAtlasDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;

    ShadowMapAtlas = RenderCommand::CreateTexture2D(ShadowMapAtlasDesc);
    if (!ShadowMapAtlas || !ShadowMapAtlas->GetRawPtr() || !ShadowMapAtlas->GetDSV() || !ShadowMapAtlas->GetSRV())
    {
        ShadowMapAtlas.reset();
        return false;
    }

    ShadowAtlasAllocator.Init(SHADOW_ATLAS_MAP_SIZE);

    return true;
}

bool FShadowRenderer::BeginShadowPass(const FShadowPassDesc &Desc)
{
    if (!Desc.DepthView || Desc.Width == 0 || Desc.Height == 0)
        return false;

    RenderCommand::GetContext()->ClearDepthStencilView(Desc.DepthView, D3D11_CLEAR_DEPTH, 1.0f, 0);

    ID3D11DeviceContext *Context = RenderCommand::GetContext();

    // 컬러 타깃 없이 DepthView만 렌더링 대상으로 설정
    Context->OMSetRenderTargets(0, nullptr, Desc.DepthView);

    // Shadow Map 크기에 맞춰 렌더링 영역 설정
    RenderCommand::SetViewport(0, 0, Desc.Width, Desc.Height);

    // 그림자 깊이 렌더링에 필요한 GPU 상태 설정
    RenderCommand::SetRasterizerState(ERasterizerState::ShadowDepth);
    RenderCommand::SetDepthStencilState(EDepthStencilState::Default);
    RenderCommand::SetBlendState(EBlendState::NoColorWrite);

    return true;
}

void FShadowRenderer::SetShadowViewPort(const uint32 X, const uint32 Y, const uint32 Width, const uint32 Height)
{
    RenderCommand::SetViewport(X, Y, Width, Height);
}

void FShadowRenderer::EndShadowPass()
{
    // Shadow DSV 바인딩 해제
    RenderCommand::GetContext()->OMSetRenderTargets(0, nullptr, nullptr);
}

#include "EnginePCH.h"
#include "ShadowRenderer.h"

#include "Math/Frustum.h"
#include "Render/RenderCommand.h"

struct FShadowConstants
{
    FMatrix LightVP;
    float bEnabled = 0.0f;
    float Padding[3] = {};
};

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

FMatrix FShadowRenderer::BuildDirectionalLightVP(const FMatrix& CameraVP, float CameraNear, float CameraFar, float ShadowDistance, const FVector& LightDirection) const
{
    const std::array<FVector, 8> Corners = GetFrustumCornersWorld(CameraVP);
    const std::array<FVector, 8> ShadowCorners = LimitFrustumToShadowDistance(Corners, CameraNear, CameraFar, ShadowDistance);

    // 절두체를 감싸는 구의 반경으로 투영 크기를 안정화한다.
    FVector Center(0.0f, 0.0f, 0.0f);
    for (const FVector& Corner : ShadowCorners)
        Center += Corner;
    Center /= 8.0f;

    float Radius = 0.0f;
    for (const FVector& Corner : ShadowCorners)
        Radius = std::max(Radius, (Corner - Center).Size());

    Radius = std::ceil(Radius * 16.0f) / 16.0f;

    const FMatrix LightView = BuildDirectionalLightView(ShadowCorners, LightDirection);
    FBox LightSpaceBounds = CalculateLightSpaceBounds(ShadowCorners, LightView);

    // 회전할 때마다 바뀌던 XY 투영 범위를 고정한다.
    const float Extent = Radius * 1.01f;
    LightSpaceBounds.Min.Y = -Extent;
    LightSpaceBounds.Max.Y = Extent;
    LightSpaceBounds.Min.Z = -Extent;
    LightSpaceBounds.Max.Z = Extent;

    FMatrix LightVP = LightView * BuildDirectionalLightProjection(LightSpaceBounds);

    // 투영 영역의 이동을 Shadow Map 텍셀 단위로 정렬한다.
    if (ShadowMap && ShadowMap->GetWidth() > 0)
    {
        const float HalfResolution = static_cast<float>(ShadowMap->GetWidth()) * 0.5f;
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

bool FShadowRenderer::Init(uint32 ShadowMapSize)
{
    D3D11_TEXTURE2D_DESC Desc{};
    Desc.Width = ShadowMapSize;
    Desc.Height = ShadowMapSize;
    Desc.MipLevels = 1;
    Desc.ArraySize = 1;
    Desc.Format = DXGI_FORMAT_R24G8_TYPELESS;
    Desc.SampleDesc.Count = 1;
    Desc.Usage = D3D11_USAGE_DEFAULT;
    Desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;

    ShadowMap = RenderCommand::CreateTexture2D(Desc);

    if (!ShadowMap || !ShadowMap->GetRawPtr() || !ShadowMap->GetDSV() || !ShadowMap->GetSRV())
    {
        ShadowMap.reset();
        return false;
    }

    ShadowRenderingInfo.DepthStencil.Texture = ShadowMap.get();
    ShadowRenderingInfo.DepthStencil.LoadOp = ERenderTargetLoadOp::Clear;
    ShadowRenderingInfo.DepthStencil.ClearValue.DepthClearValue = 1.0f;
    ShadowRenderingInfo.ViewportSetting.Width = ShadowMapSize;
    ShadowRenderingInfo.ViewportSetting.Height = ShadowMapSize;

    ShadowCB = RenderCommand::CreateConstantBuffer(sizeof(FShadowConstants));
    if (!ShadowCB || !ShadowCB->GetBuffer())
        return false;

    return true;
}

void FShadowRenderer::BeginShadowPass()
{
    RenderCommand::BeginRenderPass(ShadowRenderingInfo);
}

void FShadowRenderer::EndShadowPass()
{
    RenderCommand::EndRenderPass(ShadowRenderingInfo);
}

void FShadowRenderer::BindLightVP(const FMatrix& LightVP, bool bEnabled) const
{
    FShadowConstants Constants{};
    Constants.LightVP = LightVP;
    Constants.bEnabled = bEnabled ? 1.0f : 0.0f;

    RenderCommand::UpdateBufferData(ShadowCB.get(), &Constants);
    RenderCommand::BindConstantBuffer(5, ShadowCB.get(), EShaderBindFlagBits::Pixel);
}
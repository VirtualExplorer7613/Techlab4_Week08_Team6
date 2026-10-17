#pragma once

#include "SceneComponent.h"
#include "Math/Vector4.h"

class FTexture2D;

class ULightComponentBase : public USceneComponent
{

	DECLARE_CLASS(ULightComponentBase, USceneComponent)
	REFLECT_START(ULightComponentBase)
		PROPERTY(Intensity)
		PROPERTY_TYPE(LightColor, Color)
		PROPERTY(bVisible)
		PROPERTY(bCastShadows)
		PROPERTY(ShadowDepthBias)
		PROPERTY(ShadowSlopeBias)		
		PROPERTY(FarPlane)
		PROPERTY(NearPlane)
	REFLECT_END()

public:
	ULightComponentBase() = default;
	~ULightComponentBase() override;
	virtual void InitializeComponent() override;

	float GetIntensity() const { return Intensity; }
	void SetIntensity(float InIntensity) { Intensity = InIntensity; }

	const FVector4& GetLightColor() const { return LightColor; }
	void SetLightColor(const FVector4& InLightColor) { LightColor = InLightColor; }

	bool GetCastShadows() const { return bCastShadows; }
	void SetCastShadows(bool bInCastShadows) { bCastShadows = bInCastShadows; }

	float GetShadowDepthBias() const { return ShadowDepthBias; }
	void SetShadowDepthBias(float InShadowDepthBias) { ShadowDepthBias = InShadowDepthBias; }

	float GetShadowSlopeBias() const { return ShadowSlopeBias; }
	void SetShadowSlopeBias(float InShadowSlopeBias) { ShadowSlopeBias = InShadowSlopeBias; }

	float GetShadowMapWidth() const { return ShadowMapWidth; }
	void SetShadowMapWidth(float InShadowMapWidth) { ShadowMapWidth = InShadowMapWidth; }

	float GetFarPlane() const { return FarPlane; }
	void SetFarPlane(float InFarPlane) { FarPlane = InFarPlane; }

	float GetNearPlane() const { return NearPlane; }
	void SetNearPlane(float InNearPlane) { NearPlane = InNearPlane; }

    FTexture2D *GetShadowMapTexture() const
    {
        return ShadowMapTexture;
    }
    void SetShadowMapTexture(FTexture2D *InShadowMapTexture)
    {
        ShadowMapTexture = InShadowMapTexture;
    }

protected:
	float Intensity = 1.8f;
	FVector4 LightColor = FVector4(1.f, 1.f, 1.f, 1.0f);
	bool bVisible = true;

private:
	bool bCastShadows = true;
	float ShadowDepthBias = 0.0f;
	float ShadowSlopeBias = 0.0f;
	float ShadowMapWidth = 2048.0f;
	float FarPlane = 250.0f;
	float NearPlane = 10.0f;

    FTexture2D *ShadowMapTexture = nullptr;
};


class ULightComponent : public ULightComponentBase
{
	DECLARE_CLASS(ULightComponent, ULightComponentBase)
	REFLECT_START(ULightComponent)
	REFLECT_END()
public:
private:
};

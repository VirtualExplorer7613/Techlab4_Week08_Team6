#pragma once
#include "LightComponent.h"

class FLineBatcher;

class UPointLightComponent : public ULightComponent
{
	DECLARE_CLASS(UPointLightComponent, ULightComponent)
	REFLECT_START(ClassName)
		PROPERTY(AttenuationRadius)
		PROPERTY(LightFalloffExponent)
		PROPERTY(LightSourceRadius)
	REFLECT_END()

public:
	UPointLightComponent();
	virtual ~UPointLightComponent() override;
	virtual void InitializeComponent() override;

	float GetAttenuationRadius() const { return AttenuationRadius; }
	void SetAttenuationRadius(float InRadius) { AttenuationRadius = InRadius; }

	float GeLightFalloffExponent() const { return LightFalloffExponent; }
	void SetLightFalloffExponent(float InFalloff) { LightFalloffExponent = InFalloff; }

	float GetLightSourceRadius() const { return LightSourceRadius; }
	void SetLightSourceRadius(float InLightSourceRadius) { LightSourceRadius = InLightSourceRadius; }

	void DrawDebug(FLineBatcher* LineBatcher) const;

	FVector4 GetFaceAtlasScaleOffset(uint32 Face) { return AtlasScaleOffset[Face]; }
	void SetFaceAtlasScaleOffset(uint32 Face, FVector4 InFaceAtlasScaleOffset) { AtlasScaleOffset[Face] = InFaceAtlasScaleOffset; }

	const uint32 GetLightIndex() { return LightIndex; }
	void SetLightIndex(uint32 InLightIndex) { LightIndex = InLightIndex; }

protected:
	float AttenuationRadius = 6.5f;
	float LightFalloffExponent = 3.0f;

	uint32 LightIndex = -1;

	FVector4 AtlasScaleOffset[6] = {};
	float LightSourceRadius = 0.05f;
};

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

protected:
	float AttenuationRadius = 6.5f;
	float LightFalloffExponent = 3.0f;
	float LightSourceRadius = 0.05f;
};

#pragma once

#include "SceneComponent.h"
#include "Math/Vector4.h"

class ULightComponentBase : public USceneComponent
{

	DECLARE_CLASS(ULightComponentBase, USceneComponent)
	REFLECT_START(ULightComponentBase)
		PROPERTY(Intensity)
		PROPERTY_TYPE(LightColor, Color)
		PROPERTY(bVisible)
	REFLECT_END()

public:
	ULightComponentBase() = default;
	~ULightComponentBase() override;
	virtual void InitializeComponent() override;

	float GetIntensity() const { return Intensity; }
	void SetIntensity(float InIntensity) { Intensity = InIntensity; }

	const FVector4& GetLightColor() const { return LightColor; }
	void SetLightColor(const FVector4& InLightColor) { LightColor = InLightColor; }

protected:
	float Intensity = 1.8f;
	FVector4 LightColor = FVector4(1.f, 1.f, 1.f, 1.0f);
	bool bVisible = true;
};


class ULightComponent : public ULightComponentBase
{
	DECLARE_CLASS(ULightComponent, ULightComponentBase)
	REFLECT_START(ULightComponent)
	REFLECT_END()
public:
private:
};
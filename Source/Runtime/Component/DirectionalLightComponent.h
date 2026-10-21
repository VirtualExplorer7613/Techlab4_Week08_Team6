#pragma once

#include "LightComponent.h"
class FLineBatcher;

class UDirectionalLightComponent : public ULightComponent
{
	DECLARE_CLASS(UDirectionalLightComponent, ULightComponent)
	REFLECT_START(ClassName)
	PROPERTY(LightSourceAngle)
	REFLECT_END()

public:
	UDirectionalLightComponent() = default;
	virtual void InitializeComponent() override;

	float GetLightSourceAngle() const { return LightSourceAngle; }
	void SetLightSourceAngle(float InLightSourceAngle) { LightSourceAngle = InLightSourceAngle; }

	void DrawDebug(FLineBatcher* LineBatcher) const;

protected:
	float LightSourceAngle = 0.5357f;
};

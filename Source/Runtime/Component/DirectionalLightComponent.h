#pragma once

#include "LightComponent.h"
class FLineBatcher;

class UDirectionalLightComponent : public ULightComponent
{
	DECLARE_CLASS(UDirectionalLightComponent, ULightComponent)
	REFLECT_START(ClassName)
	REFLECT_END()

public:
	UDirectionalLightComponent() = default;
	virtual void InitializeComponent() override;

	void DrawDebug(FLineBatcher* LineBatcher) const;
};

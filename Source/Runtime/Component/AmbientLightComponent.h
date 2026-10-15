#pragma once

#include "LightComponent.h"

class UAmbientLightComponent : public ULightComponent
{
	DECLARE_CLASS(UAmbientLightComponent, ULightComponent)
	REFLECT_START(ClassName)
		REFLECT_END()

public:
	UAmbientLightComponent();
	virtual void InitializeComponent() override;
};

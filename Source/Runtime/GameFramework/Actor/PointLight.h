#pragma once

#include "GameFramework/Actor.h"
#include "Component/BillboardComponent.h"
#include "Component/PointLightComponent.h"

class APointLight : public AActor
{
	DECLARE_CLASS(APointLight, AActor)
	REFLECT_START(ClassName)
		REFLECT_END()

public:
	APointLight();
	UPointLightComponent* GetPointLightComponent() const { return PointLightComponent; }
	UBillboardComponent* GetBillboardComponent() const { return BillboardComponent; }

private:
	UPointLightComponent* PointLightComponent;
	UBillboardComponent* BillboardComponent = nullptr;
};
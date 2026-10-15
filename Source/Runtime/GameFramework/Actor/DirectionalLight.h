#pragma once

#include "GameFramework/Actor.h"
#include "Component/BillboardComponent.h"
#include "Component/DirectionalLightComponent.h"
#include "Component/StaticMeshComponent.h"

class ADirectionalLight : public AActor
{
	DECLARE_CLASS(ADirectionalLight, AActor)

	REFLECT_START(ClassName)
		REFLECT_END()

public:
	ADirectionalLight();
	virtual ~ADirectionalLight() override = default;

	UDirectionalLightComponent* GetDirctionalLightComponent() const { return DirectionalLightComponent; }
	UStaticMeshComponent* GetArrowComponent() const { return StaticMeshComponent; }
	UBillboardComponent* GetBillboardComponent() const { return BillboardComponent; }

private:
	UDirectionalLightComponent* DirectionalLightComponent = nullptr;
	UStaticMeshComponent* StaticMeshComponent = nullptr;
	UBillboardComponent* BillboardComponent = nullptr;
};

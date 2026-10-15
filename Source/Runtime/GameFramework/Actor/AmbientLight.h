#pragma once

#include "GameFramework/Actor.h"
#include "Component/BillboardComponent.h"
#include "Component/AmbientLightComponent.h"

// 에디터에서 아이콘(빌보드)으로 보이고, 선택하면 스포트라이트 원뿔이 라인으로 그려진다.
class AAmbientLight : public AActor
{
	DECLARE_CLASS(AAmbientLight, AActor)

	REFLECT_START(ClassName)
		REFLECT_END()

public:
	AAmbientLight();
	virtual ~AAmbientLight() override = default;

	UAmbientLightComponent* GetAmbientLightComponent() const { return AmbientLightComponent; }
	UBillboardComponent* GetBillboardComponent() const { return BillboardComponent; }

private:
	UAmbientLightComponent* AmbientLightComponent = nullptr;
	UBillboardComponent* BillboardComponent = nullptr;
};

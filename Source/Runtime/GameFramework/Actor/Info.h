#pragma once

#include "GameFramework/Actor.h"
#include "Component/BillboardComponent.h"

class AInfo : public AActor
{
	DECLARE_CLASS(AInfo, AActor)

	REFLECT_START(ClassName)
	REFLECT_END()

public:
	AInfo();
	virtual ~AInfo() override = default;

	UBillboardComponent* GetBillboardComponent() const { return BillboardComponent; }
private:
	// 클릭해서 고를 수 있어야 하므로 프리미티브인 빌보드를 루트로 둔다
	UBillboardComponent* BillboardComponent = nullptr;
};
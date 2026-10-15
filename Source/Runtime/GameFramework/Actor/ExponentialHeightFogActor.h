#pragma once

#include "Info.h"
#include "Component/BillboardComponent.h"
#include "Component/ExponentialHeightFogComponent.h"


class AExponentialHeightFogActor final : public AInfo
{
	DECLARE_CLASS(AExponentialHeightFogActor, AInfo)




public:
	AExponentialHeightFogActor();
	virtual ~AExponentialHeightFogActor() override = default;

	AExponentialHeightFogActor(const AExponentialHeightFogActor&) = delete;
	AExponentialHeightFogActor operator=(const AExponentialHeightFogActor&) = delete;

	UExponentialHeightFogComponent* GetExponentialHeightFogComponent() const { return ExponentialHeightFogComponent; }

private:
	UExponentialHeightFogComponent* ExponentialHeightFogComponent = nullptr;
};
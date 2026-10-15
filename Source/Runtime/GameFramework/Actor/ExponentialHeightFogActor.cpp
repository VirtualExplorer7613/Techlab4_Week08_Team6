#include "EnginePCH.h"
#include "ExponentialHeightFogActor.h"

AExponentialHeightFogActor::AExponentialHeightFogActor()
{
	ExponentialHeightFogComponent = CreateDefaultSubobject<UExponentialHeightFogComponent>("UExponentialHeightFogComponent");
	SetRootComponent(ExponentialHeightFogComponent);

	// AInfo의 아이콘은 에디터 전용이라 게임 빌드에서 빠질 수 있으므로, 안개 높이 기준이 되는 컴포넌트를 루트로 올린다
	GetBillboardComponent()->SetupAttachment(ExponentialHeightFogComponent);
}

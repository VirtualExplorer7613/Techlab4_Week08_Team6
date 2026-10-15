#include "EnginePCH.h"
#include "SpotLight.h"
#include "Asset/AssetManager.h"
#include "Render/Material.h"

ASpotLight::ASpotLight()
{
	SpotLightComponent = CreateDefaultSubobject<USpotLightComponent>("USpotLightComponent");
	SetRootComponent(SpotLightComponent);

	// 아이콘은 에디터 전용이라 게임 빌드에서 빠질 수 있으므로 루트가 아닌 라이트 아래에 붙인다
	BillboardComponent = CreateDefaultSubobject<UBillboardComponent>("UBillboardComponent");
	BillboardComponent->SetupAttachment(SpotLightComponent);
	BillboardComponent->SetHiddenInDetails(true);

	UMaterial* Material = UAssetManager::GetAssetByPath<UMaterial>("SpotLightIconMaterial");
	assert(Material);
	BillboardComponent->SetMaterial(0, Material);
}

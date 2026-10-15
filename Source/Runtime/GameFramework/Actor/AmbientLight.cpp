
#include "EnginePCH.h"
#include "AmbientLight.h"
#include "Asset/AssetManager.h"
#include "Render/Material.h"

AAmbientLight::AAmbientLight()
{
	AmbientLightComponent = CreateDefaultSubobject<UAmbientLightComponent>("UAmbientLightComponent");
	SetRootComponent(AmbientLightComponent);

	// 아이콘은 에디터 전용이라 게임 빌드에서 빠질 수 있으므로 루트가 아닌 라이트 아래에 붙인다
	BillboardComponent = CreateDefaultSubobject<UBillboardComponent>("UBillboardComponent");
	BillboardComponent->SetupAttachment(AmbientLightComponent);
	BillboardComponent->SetHiddenInDetails(true);

	UMaterial* Material = UAssetManager::GetAssetByPath<UMaterial>("AmbientLightIconMaterial");
	assert(Material);
	BillboardComponent->SetMaterial(0, Material);
}


#include "EnginePCH.h"
#include "DirectionalLight.h"
#include "Asset/AssetManager.h"
#include "Render/Material.h"

ADirectionalLight::ADirectionalLight()
{
	DirectionalLightComponent = CreateDefaultSubobject<UDirectionalLightComponent>("UDirectionalLightComponent");
	SetRootComponent(DirectionalLightComponent);

	// 아이콘은 에디터 전용이라 게임 빌드에서 빠질 수 있으므로 루트가 아닌 라이트 아래에 붙인다
	BillboardComponent = CreateDefaultSubobject<UBillboardComponent>("UBillboardComponent");
	BillboardComponent->SetupAttachment(DirectionalLightComponent);
	BillboardComponent->SetHiddenInDetails(true);

	//TODO : 에디터에서만 보이게 해야 한다.
	StaticMeshComponent = CreateDefaultSubobject<UStaticMeshComponent>("ArrowComponent");
	StaticMeshComponent->SetupAttachment(DirectionalLightComponent);

	StaticMeshComponent->SetStaticMesh(UAssetManager::GetAssetByPath<UStaticMesh>("Arrow"));
	StaticMeshComponent->SetRelativeRotation(FRotator(90.0f, 0.0f, 0.0f));
	StaticMeshComponent->SetWorldScale3D(FVector(0.5f));


	UMaterial* Material = UAssetManager::GetAssetByPath<UMaterial>("DirectionalLightIconMaterial");
	assert(Material);
	BillboardComponent->SetMaterial(0, Material);
}

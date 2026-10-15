#include "EnginePCH.h"
#include "PointLight.h"
#include "Asset/AssetManager.h"
#include "Render/Texture2D.h"
#include "Render/Material.h"

APointLight::APointLight()
{
	PointLightComponent = CreateDefaultSubobject<UPointLightComponent>("UPointLightComponent");
	SetRootComponent(PointLightComponent);

	// 아이콘은 에디터 전용이라 게임 빌드에서 빠질 수 있으므로 루트가 아닌 라이트 아래에 붙인다
	BillboardComponent = CreateDefaultSubobject<UBillboardComponent>("UBillboardComponent");
	BillboardComponent->SetupAttachment(PointLightComponent);
	BillboardComponent->SetHiddenInDetails(true);

	UMaterial* Material = UAssetManager::GetAssetByPath<UMaterial>("PointLightIconMaterial");
	BillboardComponent->SetMaterial(0, Material);

	//RotatingMovementComponent = CreateDefaultSubobject<URotatingMovementComponent>("URotatingMovementComponent");	
	//RotatingMovementComponent->RotationRate = FRotator(0.0f, 100.0f, 100.0f);
	//RotatingMovementComponent->PivotTranslation = FVector(0.0f, 0.0f, 0.0f);

	//ProjectileMovementComponent = CreateDefaultSubobject<UProjectileMovementComponent>("UProjectileMovementComponent");
	//ProjectileMovementComponent->InintialSpeed = 1.0f;
	//ProjectileMovementComponent->MaxSpeed = 5.0f;
	//ProjectileMovementComponent->Velocity = FVector(5.0f, 0.0f, 0.0f);
	//ProjectileMovementComponent->bShouldBounce = false;
	//ProjectileMovementComponent->bInitialVelocityInLocalSpace = true;
	//ProjectileMovementComponent->ProjectileGravityScale = 0.0f;
}

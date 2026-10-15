#include "EnginePCH.h"
#include "Info.h"

AInfo::AInfo()
{
	BillboardComponent = CreateDefaultSubobject<UBillboardComponent>("UBillboardComponent");
	SetRootComponent(BillboardComponent);
	BillboardComponent->SetHiddenInDetails(true);
}
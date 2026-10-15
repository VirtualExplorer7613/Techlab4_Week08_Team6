#include "EnginePCH.h"
#include "LightComponent.h"
#include "Engine/World.h"


void ULightComponentBase::InitializeComponent()
{
    Super::InitializeComponent();

    if (AActor* Owner = GetOwner())
    {
        if (UWorld* World = Owner->GetWorld())
        {
            World->GetScene().RegisterLightComponent(this);
        }
    }
}


ULightComponentBase::~ULightComponentBase()
{
    if (AActor* Owner = GetOwner())
    {
        if (UWorld* World = Owner->GetWorld())
        {
            World->GetScene().UnRegisterLightComponent(this);
        }
    }
}
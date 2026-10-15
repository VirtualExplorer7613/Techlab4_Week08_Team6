#include "EnginePCH.h"
#include "DirectionalLightComponent.h"
#include "Render/LineBatcher.h"

void UDirectionalLightComponent::InitializeComponent()
{
	Super::InitializeComponent();
}

void UDirectionalLightComponent::DrawDebug(FLineBatcher* LineBatcher) const
{
	if (LineBatcher == nullptr)
	{
		return;
	}

	FTransform WorldTransform;
	WorldTransform.Rotation = GetWorldRotation();

	FVector Start = GetWorldLocation();
	FVector End = Start + WorldTransform.GetForward().Normalized() * 1000.f;
	LineBatcher->AddLine(Start, End, LightColor);
}
	
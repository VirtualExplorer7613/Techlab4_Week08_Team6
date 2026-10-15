#include "EnginePCH.h"
#include "PointLightComponent.h"
#include "Engine/World.h"
#include "Render/LineBatcher.h"

namespace
{
	//원의 분할 수
	constexpr int32 POINT_SEGMENT_COUNT = 24;

	void AddWireCircle(FLineBatcher* LineBatcher,
		const FVector& Apex,
		const FVector& Right,
		const FVector& Up,
		const float Radius,
		const FVector4& Color
	)
	{
		FVector FirstPoint;
		FVector PreviousPoint;

		for (int32 i = 0; i < POINT_SEGMENT_COUNT; ++i)
		{
			const float Angle = (2.0f * PI * i) / POINT_SEGMENT_COUNT;

			const FVector Point =
				Apex +
				Right * (std::cosf(Angle) * Radius) +
				Up * (std::sinf(Angle) * Radius);

			if (i == 0)
			{
				FirstPoint = Point;
			}
			else
			{
				LineBatcher->AddLine(PreviousPoint, Point, Color);
			}

			PreviousPoint = Point;
		}

		// 마지막 점과 첫 점을 이어 원을 닫는다
		LineBatcher->AddLine(PreviousPoint, FirstPoint, Color);
	}
}

void UPointLightComponent::InitializeComponent()
{
    Super::InitializeComponent();
}


UPointLightComponent::~UPointLightComponent()
{
}

void UPointLightComponent::DrawDebug(FLineBatcher* LineBatcher) const
{
	if (LineBatcher == nullptr)
	{
		return;
	}

	FTransform WorldTransform;
	WorldTransform.Location = GetWorldLocation();
	WorldTransform.Rotation = GetWorldRotation();

	FVector Apex = WorldTransform.Location;
	FVector Forward = WorldTransform.GetForward();
	FVector Right = WorldTransform.GetRight();
	FVector Up = WorldTransform.GetUp();

	AddWireCircle(LineBatcher, Apex, Right, Up, AttenuationRadius, LightColor);
	AddWireCircle(LineBatcher, Apex, Forward, Up, AttenuationRadius, LightColor);
	AddWireCircle(LineBatcher, Apex, Right, Forward, AttenuationRadius, LightColor);
}
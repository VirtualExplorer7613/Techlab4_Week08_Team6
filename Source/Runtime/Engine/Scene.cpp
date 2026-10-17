#include "EnginePCH.h"
#include "Scene.h"
#include "Component/PointLightComponent.h"
#include "Component/SpotLightComponent.h"
#include "Component/DirectionalLightComponent.h"
#include "Component/AmbientLightComponent.h"
#include "Component/BillboardComponent.h"
#include "Shared/LightDefinitions.h"
#include "Render/ShadowInfo.h"

void FScene::AddPrimitive(UPrimitiveComponent* Component)
{
	if (!Component || Component->SceneProxy) return;

	FPrimitiveSceneProxy* Proxy = new FPrimitiveSceneProxy(Component);
	Proxy->Scene = this;
	Proxy->PackedIndex = Proxies.Num();
	Component->SceneProxy = Proxy;
	MarkDirty(Proxy);
	MarkRenderStateDirty(Proxy);

	Proxies.Add(Proxy);
	PrimitiveBounds.Add(FAABB{});
	PrimitiveFlags.Add(0);

	bElementListChanged = true;
}

void FScene::RemovePrimitive(UPrimitiveComponent* Component)
{
	if (!Component || !Component->SceneProxy) return;

	FPrimitiveSceneProxy* Proxy = Component->SceneProxy;
	if (!Proxy) return;

	const uint32 Index = static_cast<uint32>(Proxy->PackedIndex);

	Proxies.RemoveAtSwap(Index);
	PrimitiveBounds.RemoveAtSwap(Index);
	PrimitiveFlags.RemoveAtSwap(Index);

	if (Index < static_cast<uint32>(Proxies.Num()))
		Proxies[Index]->PackedIndex = static_cast<int32>(Index);

	if (Proxy->bQueuedForUpdate)
	{
		const int32 Found = DirtyProxies.Find(Proxy);   // 프로젝트 TArray의 Find 이름에 맞게
		if (Found != INDEX_NONE) DirtyProxies.RemoveAtSwap(Found);
	}

	if (Proxy->bRenderStateQueued)
	{
		const int32 Found = RenderStateDirtyProxies.Find(Proxy);
		if (Found != INDEX_NONE) RenderStateDirtyProxies.RemoveAtSwap(Found);
	}

	Component->SceneProxy = nullptr;
	delete Proxy;

	bElementListChanged = true;
}

void FScene::RemoveAllPrimitives()
{
	// 하나씩 RemovePrimitive하면 대기열 검색(Find)이 물체 수만큼 반복되므로 통째로 비운다.
	for (FPrimitiveSceneProxy* Proxy : Proxies)
	{
		if (UPrimitiveComponent* Component = Proxy->GetComponent())
			Component->SceneProxy = nullptr;
		delete Proxy;
	}
	Proxies.Reset();
	PrimitiveBounds.Reset();
	PrimitiveFlags.Reset();
	DirtyProxies.Reset();
	RenderStateDirtyProxies.Reset();
	BVH.Clear();
	bElementListChanged = true;
}

void FScene::AddFogInfo(uint32 Id, const FFogInfo& FogInfo)
{
	FExponentialHeightFogSceneInfo sceneFogInfo;
	sceneFogInfo.Id = Id;
	sceneFogInfo.FogInfo = FogInfo;


	FogInfos.Add(sceneFogInfo);
}

void FScene::RemoveFogInfo(uint32 Id)
{
	// 렌더러는 FogInfos[0]만 쓰므로, 순서를 유지하려고 RemoveAtSwap 대신 RemoveAt을 쓴다.
	for (int32 i = 0; i < FogInfos.Num(); ++i)
	{
		if (FogInfos[i].Id == Id)
		{
			FogInfos.RemoveAt(i);
			return;
		}
	}
}

void FScene::RemoveAllFogInfo()
{
	FogInfos.Reset();

}

void FScene::UpdateFogInfo(uint32 Id, const FFogInfo& FogInfo)
{
	for (FExponentialHeightFogSceneInfo& SceneFogInfo : FogInfos)
	{
		if (SceneFogInfo.Id == Id)
		{
			SceneFogInfo.FogInfo = FogInfo;
			return;
		}


	}
}

//void FScene::RemoveFogInfo(uint32 Id)
//{
//	for (int32 i = 0; i < FogInfos.Num(); ++i)
//	{
//		if (FogInfos[i].Id == Id)
//		{
//			FogInfos.RemoveAtSwap(i);
//			return;
//		}
//	}
//}


void FScene::UpdateAllTransforms()
{
	for (FPrimitiveSceneProxy* Proxy : RenderStateDirtyProxies)
	{
		Proxy->UpdateRenderState();
		Proxy->bRenderStateQueued = false;
	}
	RenderStateDirtyProxies.Reset();

	for (FPrimitiveSceneProxy* Proxy : DirtyProxies)
	{
		Proxy->UpdateTransform();
		PrimitiveBounds[Proxy->PackedIndex] = Proxy->GetBounds();
		PrimitiveFlags[Proxy->PackedIndex] = Proxy->GetComponent()->IsVisible() ? 1 : 0;
		Proxy->bQueuedForUpdate = false;
	}
	
	//const int32 Count = Proxies.Num();
	//for (int32 i = 0; i < Count; ++i)
	//{
	//	FPrimitiveSceneProxy* Proxy = Proxies[i];
	//	Proxy->UpdateTransform();

	//	const FAABB& Bounds = Proxy->GetBounds();
	//	if (PrimitiveBounds[i].Center != Bounds.Center || PrimitiveBounds[i].Extent != Bounds.Extent)
	//	{
	//		bBoundsChanged = true;
	//	}

	//	PrimitiveBounds[i] = Proxy->GetBounds();
	//	PrimitiveFlags[i] = Proxy->GetComponent()->IsVisible() ? 1 : 0;
	//}

	const bool bAnyMoved = DirtyProxies.Num() > 0;
	DirtyProxies.Reset();
	

	if (bElementListChanged)
	{
		BuildBVH();
		bElementListChanged = false;
	}
	else if (bAnyMoved)
	{
		BVH.Refit();
	}
}

void FScene::BuildBVH()
{
	BVH.Clear();
	// BillboardComponents.Reset();

	TArray<FPrimitiveSceneProxy*> Elements;
	Elements.Reserve(Proxies.Num());
	for (FPrimitiveSceneProxy* Proxy : Proxies)
	{
		if (Proxy && Proxy->GetComponent())
			Elements.Add(Proxy);
	}

	BVH.Build(std::span(Elements.GetData(), Elements.Num()));
}

void FScene::MarkDirty(FPrimitiveSceneProxy* Proxy)
{
	if (Proxy->bQueuedForUpdate) return;            // 중복 추가 방지
	Proxy->bQueuedForUpdate = true;
	DirtyProxies.Add(Proxy);
}

void FScene::MarkRenderStateDirty(FPrimitiveSceneProxy* Proxy)
{
	if (!Proxy || Proxy->bRenderStateQueued) return;
	Proxy->bRenderStateQueued = true;
	RenderStateDirtyProxies.Add(Proxy);
}


void FScene::UpdateLightComponents(FRenderer* Renderer, const FMatrix& CameraViewProjection)
{
	if (!Renderer)
		return;

	FLightConstants Constants{};
	FDirectionalShadowInfo ShadowInfo{};
	for (ULightComponentBase* LightComponent : LightComponents)
	{
		if (!LightComponent)
			continue;

		// 라이트 액터 아이콘을 LightColor로 칠한다. Details 패널은 필드를 직접 바꾸므로 매 프레임 복사한다.
		// 아이콘은 라이트 컴포넌트 자식으로 붙어 있고, 게임 빌드에서는 없을 수 있다.
		for (USceneComponent* Child : LightComponent->GetAttachChildren())
		{
			if (UBillboardComponent* Billboard = Cast<UBillboardComponent>(Child))
			{
				Billboard->SetTint(LightComponent->GetLightColor());
			}
		}

		UClass* LightClass = LightComponent->GetClass();

		
		//SpotLight가 PointLight를 상속받기 때문에 먼저 검사해야 한다.
		if (USpotLightComponent* SpotLightComponent = Cast<USpotLightComponent>(LightComponent))
		{
			if (Constants.SpotLightCount >= NUM_SPOT_LIGHT)
			{
				continue;
			}

			UpdateSpotLight(Constants, SpotLightComponent);
		}
		else if (UPointLightComponent* PointLightComponent = Cast<UPointLightComponent>(LightComponent))
		{
			if (Constants.PointLightCount >= NUM_POINT_LIGHT)
			{
				continue;
			}

			UpdatePointLight(Constants, PointLightComponent);
		}
		else if (UAmbientLightComponent* AmbientLightComponent = Cast<UAmbientLightComponent>(LightComponent))
		{
			UpdateAmbientLight(Constants, AmbientLightComponent);
		}

		else if (UDirectionalLightComponent* DirectionalLightComponent = Cast<UDirectionalLightComponent>(LightComponent))
		{
			UpdateDirectionalLight(Constants, DirectionalLightComponent, CameraViewProjection, ShadowInfo);
		}
	}

	Renderer->SetLight(Constants);
	Renderer->GetShadowRenderer().SetDirectionalShadowInfo(ShadowInfo);
}

void FScene::RegisterLightComponent(ULightComponentBase* LightComponent)
{
	if (!LightComponent)
		return;

	for (ULightComponentBase* Existing : LightComponents)
	{
		if (Existing == LightComponent)
			return;
	}
	LightComponents.Add(LightComponent);
}
void FScene::UnRegisterLightComponent(ULightComponentBase* LightComponent)
{
	for (uint32 i = 0; i < LightComponents.Num(); ++i)
	{
		if (LightComponents[i] == LightComponent)
		{
			LightComponents.RemoveAt(i, 1);
			return;
		}
	}
}

void FScene::UpdatePointLight(FLightConstants& LightConstants, UPointLightComponent *LightComponent)
{
	static FPointLight PointData{};
	const FVector Position = LightComponent->GetWorldLocation();
	const FVector4 Color = LightComponent->GetLightColor();

	PointData.PositionRadius = FVector4(
		Position.X,
		Position.Y,
		Position.Z,
		std::max<float>(LightComponent->GetAttenuationRadius(), 0.001f)
	);

	PointData.ColorIntensity = FVector4(
		Color.X,
		Color.Y,
		Color.Z,
		std::max<float>(LightComponent->GetIntensity(), 0.0f)
	);

	PointData.FalloffEnabled = FVector4(
		std::max<float>(LightComponent->GeLightFalloffExponent(), 0.001f),
		1.0f,
		0.0f,
		0.0f
	);

	LightConstants.PointLights[LightConstants.PointLightCount] = PointData;
	LightConstants.PointLightCount++;
}

void FScene::UpdateSpotLight(FLightConstants & LightConstants, USpotLightComponent* LightComponent)
{
	static FSpotLight SpotData{};

	FTransform WorldTransform;
	WorldTransform.Location = LightComponent->GetWorldLocation();
	WorldTransform.Rotation = LightComponent->GetWorldRotation();
	const FVector4 Color = LightComponent->GetLightColor();

	SpotData.PositionRadius = FVector4(
		WorldTransform.Location.X,
		WorldTransform.Location.Y,
		WorldTransform.Location.Z,
		std::max<float>(LightComponent->GetAttenuationRadius(), 0.001f)
	);

	SpotData.ColorIntensity = FVector4(
		Color.X,
		Color.Y,
		Color.Z,
		std::max<float>(LightComponent->GetIntensity(), 0.0f)
	);

	SpotData.FalloffEnabled = FVector4(
		std::max<float>(LightComponent->GeLightFalloffExponent(), 0.001f),
		1.0f,
		std::max<float>(LightComponent->GetConeFalloffExponent(), 0.001f),
		0.0f
	);

	const float InnerAngle = LightComponent->GetInnerConeAngle();
	const float OuterAngle = LightComponent->GetOuterConeAngle();
	const float SafeOuterAngle = FMath::Clamp(OuterAngle, 0.0f, 89.0f);
	const float SafeInnerAngle = FMath::Clamp(InnerAngle, 0.0f, SafeOuterAngle);
	const float cosOuter = cosf(FMath::DegreesToRadians(SafeOuterAngle));
	const float cosInner = cosf(FMath::DegreesToRadians(SafeInnerAngle));
	const float CosDiff = cosInner - cosOuter < 0.001f ? 0.001f : cosInner - cosOuter;
	const float InvCosDiff = 1.f / CosDiff;

	SpotData.OutAngleInvCosDiff = FVector4(
		std::max<float>(cosOuter, 0.001f),
		InvCosDiff,
		0.f,
		0.f
	);

	SpotData.ForwardVector = FVector4(
		WorldTransform.GetForward(),
		0.f
	);

	LightConstants.SpotLights[LightConstants.SpotLightCount] = SpotData;
	LightConstants.SpotLightCount++;
}

void FScene::UpdateAmbientLight(FLightConstants & LightConstants, UAmbientLightComponent* LightComponent)
{
	//언리얼은 환경광(SkyLight)를 여러개 배치하면 마지막에 수정한 SkyLight만 반영된다
	//TODO : 이 엔진도 마찬가지로 마지막 수정이 된 Ambient만 반영한다.(현재는 마지막에 등록된 Ambient가 반영)
	static FAmbientLight AmbientData{};
	const FVector4 Color = LightComponent->GetLightColor();

	AmbientData.ColorIntensity = FVector4(
		Color.X,
		Color.Y,
		Color.Z,
		std::max<float>(LightComponent->GetIntensity(), 0.0f)
	);

	LightConstants.AmbientLight = AmbientData;
}

void FScene::UpdateDirectionalLight(FLightConstants& LightConstants, UDirectionalLightComponent* LightComponent, const FMatrix& CameraViewProjection, FDirectionalShadowInfo& OutShadowInfo)
{
	//TODO : 디렉셔널 라이트가 여러 개 있으면, 우선순위(priority) 순으로 고르고,
	// 우선순위가 같으면 에디터용 경고 메시지를 띄우며 하나만 골라 Intensity에 넣는다.
	// 언리얼은 Defferred  방식이기 때문에 불투명 객체에 모든 Directional Light가 합산되어 밝게 보이지만
	// 이 엔진은 Forward 방식으로 계산하므로 Directinal이 여러 개 있어도 밝아질 일이 없다.
	static FDirectionalLight DirectionalData{};

	const FVector4 Color = LightComponent->GetLightColor();
	// 부모(빌보드 루트)를 기즈모로 돌려도 반영되도록 월드 회전 기준으로 방향을 구한다.
	FTransform WorldTransform;
	WorldTransform.Rotation = LightComponent->GetWorldRotation();
	const FVector Direction = WorldTransform.GetForward().Normalized();

	DirectionalData.ColorIntensity = FVector4(
		Color.X,
		Color.Y,
		Color.Z,
		std::max<float>(LightComponent->GetIntensity(), 0.0f)
	);

	DirectionalData.Direction = Direction;

	//TODO : 우선순위 도입하기
	LightConstants.DirectionalLight = DirectionalData;

	{
		//// 테스트용 고정 영역. 장면의 위치와 크기에 맞게 조절한다.
		//const FVector Center(0.0f, 0.0f, 0.0f);
		//const float Width = 10.0f;
		//const float Height = 10.0f;
		//const float NearPlane = 0.1f;
		//const float FarPlane = 200.0f;

		// 카메라 프러스텀의 월드 꼭짓점 복원
		FVector WorldCorners[8];
		GetFrustumWorldCorners(CameraViewProjection, WorldCorners);

		// 방향광의 회전만 적용한 좌표계 준비
		// 아직 라이트 위치는 정하지 않고, 방향 기준으로만 변환한다.
		const FMatrix LightRotation =
			WorldTransform.Rotation.Quaternion().ToFMatrix();

		const FMatrix InvLightRotation = LightRotation.GetTransposed();

		// 라이트 방향 기준의 min/max 계산
		const FVector4 FirstPoint =	FVector4(WorldCorners[0], 1.0f) * InvLightRotation;

		FVector MinPoint(FirstPoint.X, FirstPoint.Y, FirstPoint.Z);
		FVector MaxPoint = MinPoint;

		for (int32 i = 1; i < 8; ++i)
		{
			const FVector4 Point =
				FVector4(WorldCorners[i], 1.0f) * InvLightRotation;

			MinPoint.X = std::min<float>(MinPoint.X, Point.X);
			MinPoint.Y = std::min<float>(MinPoint.Y, Point.Y);
			MinPoint.Z = std::min<float>(MinPoint.Z, Point.Z);

			MaxPoint.X = std::max<float>(MaxPoint.X, Point.X);
			MaxPoint.Y = std::max<float>(MaxPoint.Y, Point.Y);
			MaxPoint.Z = std::max<float>(MaxPoint.Z, Point.Z);
		}

		// 장면 크기에 맞게 조절할 여유 거리
		const float SidePadding = 0.5f;
		const float DepthPadding = 50.0f;

		const float FitWidth = std::max<float>(	MaxPoint.Y - MinPoint.Y + 2.0f * SidePadding, 0.001f);

		const float FitHeight = std::max<float>(MaxPoint.Z - MinPoint.Z + 2.0f * SidePadding, 0.001f);

		const float Width = FitWidth;
		const float Height = FitHeight;

		const float NearPlane = 0.1f;
		const float DepthRange = std::max<float>(MaxPoint.X - MinPoint.X + 2.0f * DepthPadding,	0.001f);

		const FVector LightPositionInRotatedSpace(MinPoint.X - DepthPadding - NearPlane, (MinPoint.Y + MaxPoint.Y) * 0.5f, (MinPoint.Z + MaxPoint.Z) * 0.5f);

		// 회전 좌표계에서 정한 라이트 위치를 월드 위치로 변환
		const FVector4 LightPositionH =	FVector4(LightPositionInRotatedSpace, 1.0f) * LightRotation;

		const FVector LightPosition(LightPositionH.X, LightPositionH.Y,	LightPositionH.Z);

		OutShadowInfo.LightView = FMatrix::MakeTranslation(-LightPosition) * InvLightRotation;

		OutShadowInfo.LightProjection = FMatrix(
			0.0f, 0.0f, 1.0f / DepthRange, 0.0f,
			2.0f / Width, 0.0f, 0.0f, 0.0f,
			0.0f, 2.0f / Height, 0.0f, 0.0f,
			0.0f, 0.0f, -NearPlane / DepthRange, 1.0f
		);

		OutShadowInfo.LightViewProjection =	OutShadowInfo.LightView * OutShadowInfo.LightProjection;

 		OutShadowInfo.bEnabled = true;
	}
}

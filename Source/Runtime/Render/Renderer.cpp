#include "EnginePCH.h"
#include "Renderer.h"
#include "Shader.h"
#include "Mesh.h"

#include "Asset/AssetManager.h"
#include "Core/EngineTimer.h"
#include "Core/Stats/LightweightStats.h"
#include "Core/Stats/ShadowStats.h"
#include "Core/Stats/StatOverlay.h"
#include "Component/DirectionalLightComponent.h"
#include "Component/SpotLightComponent.h"

#include "Engine/PrimitiveSceneProxy.h"
#include "Engine/Scene.h"

#include "RenderCommand.h"
#include "RenderResourceManager.h"

#include "Camera/CameraComponent.h"

#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <bit>

DECLARE_CYCLE_STAT("Draw Render Packets", STAT_DrawRenderPackets);
DECLARE_CYCLE_STAT("Render Queue Sorting", STAT_RenderQueueSorting);
DECLARE_CYCLE_STAT("Upload Per-Object CB", STAT_UploadPerObjectCB);

namespace
{
    bool IsShadowCaster(const FPrimitiveSceneProxy* Proxy)
    {
        return Proxy && Proxy->IsVisible() && Proxy->GetCastShadows() && Proxy->GetMesh() &&
            Proxy->GetRenderState() && Proxy->GetLODCount() > 0;
    }

    void RecordShadowCasterQuery(const FScene& Scene, uint64 Retained)
    {
        if (!FStatOverlay::IsEnabled(EStatFlags::Shadow))
            return;

        // 적격 후보는 Scene마다 프레임당 한 번만 센다. BVH나 Draw 목록을 재생성하지 않는다.
        auto [Entry, bInserted] = ShadowStats::CasterCandidates.try_emplace(&Scene, 0);
        if (bInserted)
            for (const FPrimitiveSceneProxy* Proxy : Scene.Proxies)
                Entry->second += IsShadowCaster(Proxy) ? 1u : 0u;
        ShadowStats::RecordCasterQuery(Entry->second, Retained);
    }

	// VSSetConstantBuffers1의 오프셋은 16개 상수(256바이트) 단위여야 하므로 오브젝트마다 256바이트 칸을 쓴다.
	constexpr uint32 PerObjectSlotConstants = 16;
	static_assert(sizeof(FPerObjectConstants) <= ObjectSlotBytes);

	constexpr uint32 MinPerObjectSlots = 1024;

	const FMatrix& GetPacketWorld(const FRenderPacket& Packet)
	{
		if (Packet.Proxy) return Packet.Proxy->GetLocalToWorld();
		return Packet.Model ? *Packet.Model : FMatrix::Identity;
	}

	uint64 MakeSortKey(const FRenderPacket& Packet)
	{
		if (Packet.Material->BlendState != EBlendState::Opaque)
		{
			const uint32 DistanceBits = std::bit_cast<uint32>(Packet.CameraToParticleDistance);
			return (1ull << 63) | static_cast<uint64>(~DistanceBits);
		}

		return (static_cast<uint64>(Packet.Material->SortID) << 47)
			| (static_cast<uint64>(Packet.Mesh->SortID) << 31)
			| (static_cast<uint64>(Packet.LODIndex & 0x3) << 29);
	}

	// 불투명 패킷과 같은 규칙의 키 (묶음은 전부 불투명)
	uint64 MakeGroupKey(const FStaticDrawGroup& Group)
	{
		return (static_cast<uint64>(Group.Material->SortID) << 47)
			| (static_cast<uint64>(Group.Mesh->SortID) << 31)
			| (static_cast<uint64>(Group.LODIndex & 0x3) << 29);
	}

	bool CreateDynamicStructuredBuffer(uint32 Stride, uint32 Count, ComPtr<ID3D11Buffer>& OutBuffer, ComPtr<ID3D11ShaderResourceView>& OutSRV)
	{
		ID3D11Device* Device = RenderCommand::GetDevice();

		D3D11_BUFFER_DESC Desc{};
		Desc.ByteWidth = Stride * Count;
		Desc.Usage = D3D11_USAGE_DYNAMIC;
		Desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		Desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		Desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		Desc.StructureByteStride = Stride;

		D3D11_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
		SRVDesc.Format = DXGI_FORMAT_UNKNOWN;
		SRVDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		SRVDesc.Buffer.NumElements = Count;

		if (FAILED(Device->CreateBuffer(&Desc, nullptr, OutBuffer.GetAddressOf())) || FAILED(Device->CreateShaderResourceView(OutBuffer.Get(), &SRVDesc, OutSRV.GetAddressOf())))
			return false;

		return true;
	}

	void UploadDynamicBuffer(ID3D11Buffer* Buffer, const void* Data, uint32 Size)
	{
		if (!Buffer) return;
		D3D11_MAPPED_SUBRESOURCE MappedResource{};
		ID3D11DeviceContext* Context = RenderCommand::GetContext();
		if (SUCCEEDED(Context->Map(Buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &MappedResource)))
		{
			std::memcpy(MappedResource.pData, Data, Size);
			Context->Unmap(Buffer, 0);
		}
	}

    bool GetIntersectedAABBWithLight(const FVector LightPos, const float LightRadius, const FAABB ShadowBound)
    {
        FVector Distance;      
        Distance.X = FMath::Max(abs(LightPos.X - ShadowBound.Center.X) - ShadowBound.Extent.X, 0.0f);
        Distance.Y = FMath::Max(abs(LightPos.Y - ShadowBound.Center.Y) - ShadowBound.Extent.Y, 0.0f);
        Distance.Z = FMath::Max(abs(LightPos.Z - ShadowBound.Center.Z) - ShadowBound.Extent.Z, 0.0f);

        float DistanceSqr = Distance.SizeSquared();
        if(DistanceSqr <= LightRadius * LightRadius)
        {
            return true;
        }
        else
        {
            return false;
        }
    }    
}

bool FRenderer::Init()
{
	bUsePerObjectSlots = RenderCommand::SupportsConstantBufferOffsets();
	PerObjectCB = RenderCommand::CreateConstantBuffer(sizeof(FPerObjectConstants));
	ViewCB = RenderCommand::CreateConstantBuffer(sizeof(FViewConstants));
	LightCB = RenderCommand::CreateConstantBuffer(sizeof(FLightGlobalConstants));

    ShadowRenderer = MakeUnique<FShadowRenderer>();
    CascadeDebugCB = RenderCommand::CreateConstantBuffer(sizeof(FCascadeDebugConstants));

    if (!ShadowRenderer->Init(2048))
        return false;

    if (!RecreateCSMResources(CSMSettings))
    {
        HTR_LOG(Error, "[CSM] Failed to create cascade resources.");
        return false;
    }

    if (!InitCascadePreviewResources())
    {
        HTR_LOG(Error, "[CSM] Failed to create preview resources.");
        return false;
    }

	if (!CreateDynamicStructuredBuffer(sizeof(FPointLight), NUM_POINT_LIGHT, PointLightBuffer, PointLightSRV) || !CreateDynamicStructuredBuffer(sizeof(FSpotLight), NUM_SPOT_LIGHT, SpotLightBuffer, SpotLightSRV))
	{
		HTR_LOG(Error, "[Renderer] Failed to create dynamic structured buffers for lights.");
	}
	GPUOcclusion.Init();   // 실패해도 오클루전만 못 쓸 뿐 렌더링은 된다
	TileBasedCulling.Init();

	return true;
}

void FRenderer::DepthPrepass(const FMatrix& ViewProjection, const EViewMode ViewMode)
{
	bDepthOnly = true;
	DrawStaticGroups(ViewMode);
	DrawPackets(0, FirstTranslucentIndex, ViewProjection, ViewMode);
	bDepthOnly = false;
}

// 필요한 칸 수가 용량을 넘을 때만 두 배씩 키워 재할당을 드물게 한다.
void FRenderer::EnsurePerObjectSlotCapacity(uint32 SlotCount)
{
	if (SlotCount <= PerObjectSlotCapacity)
		return;

	uint32 NewCapacity = std::max(PerObjectSlotCapacity * 2, MinPerObjectSlots);
	while (NewCapacity < SlotCount)
		NewCapacity *= 2;

	PerObjectSlotCB = RenderCommand::CreateConstantBuffer(NewCapacity * ObjectSlotBytes);
	if (!PerObjectSlotCB || !PerObjectSlotCB->GetBuffer())
	{
		HTR_LOG(Warning, "Per-object constant buffer ({} slots) creation failed. Falling back to per-draw updates.", NewCapacity);
		PerObjectSlotCB.reset();
		PerObjectSlotCapacity = 0;
		return;
	}
	PerObjectSlotCapacity = NewCapacity;
}

// 원래 패킷 순서로 World 행렬을 올린다. 정렬 목록은 원래 패킷 번호의 칸을 바인딩한다.
void FRenderer::UploadPerObjectConstants()
{
	SCOPE_CYCLE_COUNTER(STAT_UploadPerObjectCB);

	const uint32 Count = static_cast<uint32>(RenderPackets.size());
	if (!bUsePerObjectSlots || Count == 0)
		return;

	EnsurePerObjectSlotCapacity(Count);
	if (!PerObjectSlotCB)
	{
		bUsePerObjectSlots = false;
		return;
	}

	uint8* Dest = static_cast<uint8*>(RenderCommand::MapWriteDiscard(PerObjectSlotCB.get()));
	if (!Dest)
	{
		bUsePerObjectSlots = false;
		return;
	}

	// 매핑된 메모리는 write-combined라 순차 쓰기만 하고 읽지 않는다.
	for (uint32 Index = 0; Index < Count; ++Index)
	{
		FRenderPacket& P = RenderPackets[Index];
		const FMatrix& Model = GetPacketWorld(P);
		WriteObjectSlot(Dest, Index, Model, P.Proxy->GetReceiveShadows());
		P.Slot = Index;
	}

	RenderCommand::Unmap(PerObjectSlotCB.get());
}

// 카메라의 ViewProjection을 공통 렌더 경로로 전달한다.
void FRenderer::RenderAll(FRenderQueue& InQueue, UCameraComponent* CameraComponent, const EViewMode ViewMode)
{
	RenderAll(InQueue, CameraComponent->GetViewProjectionMatrix(), CameraComponent->GetWorldLocation(), ViewMode);
}

// 불투명 우선·반투명 거리순으로 정렬해 View 행렬과 Section 범위로 그린다.
void FRenderer::RenderAll(FRenderQueue& InQueue, const FMatrix& ViewProjection, const FVector CameraLocation, const EViewMode ViewMode)
{
	RenderQueueSorting(InQueue, ViewProjection, CameraLocation);
	RenderOpaque(ViewProjection, ViewMode);
	RenderTranslucent(ViewProjection, ViewMode);
}

void FRenderer::RenderCascadeShadowPreview(const FScene &Scene, const FMatrix &CameraViewProjection, const FVector &CameraLocation, EViewMode ViewMode)
{
    if (CascadeConstants.CSMEnabled == 0)
        return;

    bool bUpdatedMap = false;

    // 미리보기 Pixel Shader는 t0에서 배열을 읽는다.
    RenderCommand::UnbindShaderResource(0, EShaderBindFlagBits::Pixel | EShaderBindFlagBits::Vertex);

   // 장면 셰이더가 읽던 캐스케이드 배열 해제.
    RenderCommand::UnbindShaderResource(13, EShaderBindFlagBits::Pixel | EShaderBindFlagBits::Vertex);


    // const bool bCastShadows = LightData.DirectionalLight.ShadowParameters.W > 0.0f;

    for (uint32 Index = 0; Index < CascadeConstants.CascadeCount; ++Index)
    {
        FShadowPassDesc Desc{};

        Desc.DepthView = CascadeShadowMaps.GetSliceDSV(Index);
        Desc.Width = CascadeShadowMaps.GetResolution();
        Desc.Height = CascadeShadowMaps.GetResolution();

        // Caster가 없는 Cascade는 깊이만 초기화하고 Shadow Draw를 생략한다.
        if (CascadeShadowCasters[Index].IsEmpty() && Desc.DepthView && Desc.Width > 0 && Desc.Height > 0)
        {
            FScopedShadowGPUTime GPUTime(ShadowGPUProfiler);
            RenderCommand::GetContext()->ClearDepthStencilView(Desc.DepthView, D3D11_CLEAR_DEPTH, 1.0f, 0);
            ++ShadowStats::Pending.DepthClears;
            bUpdatedMap = true;
            continue;
        }

        if (!RenderShadowDepth(Scene, Desc, CascadeConstants.LightVP[Index], ViewMode, &CascadeShadowCasters[Index]))
        {
            // 일부 맵만 생성된 경우 일반 그림자
            CascadeConstants.CSMEnabled = 0;
            RenderCommand::UpdateBufferData(CascadeDebugCB.get(), &CascadeConstants, sizeof(CascadeConstants));

            break;
        }
        ++ShadowStats::Pending.CascadePasses;
        bUpdatedMap = true;
    }

    if (bUpdatedMap)
        ++ShadowStats::Pending.UpdatedLights[0];

    // Shadow Draw가 변경한 ViewCB를 복원한다.
    FViewConstants CameraConstants{};
    CameraConstants.VP = CameraViewProjection;
    CameraConstants.CameraPosition = CameraLocation;

    RenderCommand::UpdateBufferData(ViewCB.get(), &CameraConstants, sizeof(FViewConstants));
}

/*void FRenderer::PrepareCascadeShadows(const FScene &Scene, const FMatrix &CameraViewProjection, const FVector &CameraLocation, const FVector &CameraForward, float CameraNear, float CameraFar, bool bOrthographic, EViewMode ViewMode)
{
    // CSM 비활성·직교 여부와 관계없이 설정 요청을 먼저 처리한다.
    // 실패하면 기존 리소스를 유지하고 아래 계산을 계속한다.
    ApplyPendingCSMSettings();

    UpdateCascadeDebugConstants(Scene, CameraViewProjection, CameraForward, CameraNear, CameraFar, bOrthographic);

    RenderCascadeShadowPreview(Scene, CameraViewProjection, CameraLocation, ViewMode);
}*/

void FRenderer::PrepareCascadeShadows(const FScene& Scene, const FMatrix& CameraViewProjection, const FVector& CameraLocation, const FVector& CameraForward, float CameraNear, float CameraFar, bool bOrthographic, EViewMode ViewMode, const FMatrix& CameraWorld, float TanHalfFovX, float TanHalfFovY)
{
    ApplyPendingCSMSettings();
    UpdateCascadeDebugConstants(Scene, CameraWorld, CameraForward, CameraNear, CameraFar, bOrthographic, TanHalfFovX, TanHalfFovY);

    RenderCascadeShadowPreview(Scene, CameraViewProjection, CameraLocation, ViewMode);

}

bool FRenderer::InitCascadePreviewResources()
{
    ID3D11DeviceContext *Context = RenderCommand::GetContext();
    if (!Context)
        return false;

    D3D11_TEXTURE2D_DESC Desc{};
    Desc.Width = CascadePreviewResolution;
    Desc.Height = CascadePreviewResolution;
    Desc.MipLevels = 1;
    Desc.ArraySize = 1;
    Desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    Desc.SampleDesc.Count = 1;
    Desc.Usage = D3D11_USAGE_DEFAULT;
    Desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    const float ClearColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};

    for (uint32 Index = 0; Index < FCascadeShadowMaps::MaxCascades; ++Index)
    {
        auto Texture = RenderCommand::CreateTexture2D(Desc);

        if (!Texture || !Texture->GetRawPtr() || !Texture->GetRTV() || !Texture->GetSRV())
        {
            return false;
        }

        Context->ClearRenderTargetView(Texture->GetRTV(), ClearColor);

        CascadePreviewTextures[Index] = std::move(Texture);
    }

    CascadePreviewCount = 0;
    return true;
}

/*void FRenderer::UpdateCascadeDebugConstants(const FScene &Scene, const FMatrix &CameraViewProjection,  const FVector &CameraForward, float CameraNear, float CameraFar, bool bOrthographic)
{
    CascadeConstants = {};

    for (auto &Casters : CascadeShadowCasters)
    {
        Casters.Reset();
    }

    CascadeConstants.BlendEnabled = bCascadeBlendEnabled ? 1u : 0u;

    const FVector Forward = CameraForward.Normalized();
    CascadeConstants.CameraForward = FVector4(Forward.X, Forward.Y, Forward.Z, 0.0f);

    CascadeConstants.DebugColorsEnabled = bCascadeDebugColors ? 1u : 0u;

    const uint32 Count = std::min(CascadeShadowMaps.GetCascadeCount(), 4u);

    const uint32 Resolution = CascadeShadowMaps.GetResolution();
    const float EndDepth = std::min(CameraFar, CSMShadowDistance);

    const bool bCanUseCSM = {
        bCSMEnabled &&
        !bOrthographic && // Perspective 뷰에서만 CSM을 적용
        ShadowRenderer &&
        CascadeShadowMaps.GetArraySRV() &&
        LightData.DirectionalLight.ShadowParameters.W > 0.0f &&
        Count > 0 && Resolution > 0 &&
        CameraNear > 0.0f &&
        CameraFar > CameraNear && EndDepth > CameraNear
    };

    if (bCanUseCSM)
    {
        CascadeConstants.CascadeCount = Count;
        CascadeConstants.ShadowDistance = EndDepth;
        CascadeConstants.InvResolution = 1.0f / static_cast<float>(Resolution);

        CascadeConstants.SplitDepths = FVector4(EndDepth);
        CascadeConstants.BlendEndDepths = FVector4(EndDepth);

        const FVector4 &Direction = LightData.DirectionalLight.Direction;
        const FVector LightDirection(Direction.X, Direction.Y, Direction.Z);
        const auto CameraCorners = GetFrustumCornersWorld(CameraViewProjection);

        // 기본 분할 정책 E = 3;
        //constexpr float DistributionExponent = 3.0f;
        // 

        const float DistributionExponent = CSMDistributionExponent;

        float TotalWeight = 0.0f;
        float Weight = 1.0f;

        // Count개의 캐스케이드에 필요한 가중치 합을 계산
        for (uint32 Index = 0; Index < Count; ++Index)
        {
            TotalWeight += Weight;
            Weight *= DistributionExponent;
        }

        const float BaseLength = (EndDepth - CameraNear) / TotalWeight;

        float SplitNear = CameraNear;
        Weight = 1.0f;

        // 캐스케이드마다 하나씩 LightVP를 만든다.
        // Count가 4라면 Index는 0, 1, 2, 3.

        for (uint32 Index = 0; Index < Count; ++Index)
        {
            // ──────────────────────
            // 이번 Cascade가 담당할 카메라 깊이 구간 결정
            // ──────────────────────
            // SplitNear: 이번 구간의 시작 깊이
            // SplitFar : 이번 구간의 끝 깊이.
            //
            //
            // 첫 Cascade의 SplitNear는 CameraNear에서 시작한다.
            // 이후에는 이전 Cascade SplitFar를 이어받는다.
            //
            // BaseLength * Weight가 이번 구간의 길이다.
            // Weight가 1 -> 3 -> 9 -> 27로 증가하므로,
            // 멀리 있는 Cascade면 더 넓은 깊이 구간을 담당한다.
            //
            // 마지막 Cascade는 끝을 EndDepth로 명시한다.

            const float SplitFar = Index == Count - 1 ? EndDepth : SplitNear + BaseLength * Weight;

            // 이번 Cascade의 기본 경계와 다음 Cascade로 전환할 구간을 계산한다.
            //
            // SplitDepths[Index]    : 확장 전 끝 깊이. 다음 Cascade와의 보간 시작점.
            // BlendEndDepths[Index] : 확장된 끝 깊이. 다음 Cascade와의 보간 종료점.
            //
            // 예:
            // 기본 범위가 0~100이고 전환 비율이 10% 라면
            // 현재 맵은 0~110까지 렌더링하고, 100~110에서 다음 맵의 결과와 보간한다.
            // 
            // 현재 Cascade 깊이 구간의 10%를 다음 캐스케이드와의 전환 길이로 사용한다.
            constexpr float BlendFraction = 0.1f;

            // 다음 Cascade 있으면 전환 길이를 계산한다.
            // 마지막 Cascade는 다음 맵과 보간하지 않으므로 0으로 설정한다.
            const float FadeLength = Index + 1 < Count ? (SplitFar - SplitNear) * BlendFraction : 0.0f;

            // 렌더링 시작 깊이는 원래 구간의 시작을 유지한다.
            const float RenderNear = SplitNear;

            // 끝 깊이를 전환 길이만큼 확장하여 다음 캐스케이드와 겹치는 구간을 만든다.
            // 전체 그림자 거리인 EndDepth를 넘지 않도록 제한한다.
            const float RenderFar = std::min(SplitFar + FadeLength, EndDepth);

            // 원래 끝 깊이를 보간 시작점으로 저장한다.
            CascadeConstants.SplitDepths[Index] = SplitFar;

            // 확장된 끝 깊이를 보간 종료점으로 저장한다.
            // 셰이더는 SplitDepths[Index] ~ BlendEndDepths[Index] 구간에서
            // 현재 맵과 다음 맵의 결과를 섞는다.
            CascadeConstants.BlendEndDepths[Index] = RenderFar;


            // ───────────────────────
            // 이번 깊이 구간을 전체 카메라 구간의 비율로 변환
            // ───────────────────────
            // 전체 카메라 깊이 범위:
            // CameraNear ─CameraFar
            //     0           1
            //
            // 이번 구간의 시작과 끝이 이 범위에서 어디에 있는지 구한다.
            //
            // 예:
            // CameraNear = 1, CameraFar = 101
            // SplitNear  = 11, SplitFar = 31
            // 
            // 전환 길이가 구간 길이의 10%라면:
            // FadeLength = (31 - 11) * 0.1 = 2
            // RenderNear = 11
            // RenderFar = 33(EndDepth에 의해 제한되지 않는 경우)
            // 
            // NearRatio = (11 - 1) / (101 - 1) = 0.1
            // FarRatio  = (33 - 1) / (101 - 1) = 0.32
            // 이 비율로 카메라 프러스텀의 Near~Far 꼭짓점 사이를 보간하여,
            // 이번 Cascade를 렌더링할 프러스텀 꼭짓점을 구한다.

            const float NearRatio = (RenderNear - CameraNear) / (CameraFar - CameraNear);
            const float FarRatio = (RenderFar - CameraNear) / (CameraFar - CameraNear);

            // 이번 Cascade 프러스텀 꼭짓점 8개를 저장한다.
            //
            // [0~3]: 이번 구간의 Near 평면 꼭짓점
            // [4~7]: 이번 구간의 Far 평면 꼭짓점
            //
            // 모든 좌표는 World Space 좌표기준

            std::array<FVector, 8> SplitCorners{};

            // ──────────────────────
            // 전체 카메라 프러스텀에서 이번 구간만 잘라내기
            // ──────────────────────

            for (uint32 Corner = 0; Corner < 4; ++Corner)
            {
                // 전체 카메라 Near 평면의 꼭짓점 하나.
                const FVector &NearPoint = CameraCorners[Corner];

                // 그 Near 꼭짓점에서 대응하는 Far 꼭짓점까지의 길이.
                //
                // 방향뿐 아니라 길이까지 포함한 벡터다.
                // 따라서 Normalize()하면 안된다!!

                const FVector Ray = CameraCorners[Corner + 4] - NearPoint;

                // 전체 Near =>  Far 선분에서 NearRatio, FarRatio만큼 이동한다.
                // 이번 Cascade Near 평면 꼭짓점
                //
                SplitCorners[Corner] = NearPoint + Ray * NearRatio;

                // 이번 Cascade far 평면 꼭짓점
                SplitCorners[Corner + 4] = NearPoint + Ray * FarRatio;
            }

            
            // ──────────────────────
            // Cascade 그림자 생성 후보 수집
            // ──────────────────────

            // SplitCorners가 나타내는 공간을 기준으로 light camera를 배치하고,
            // LightDirection 방향을 바라보도록 한다.
            //
            // 이번 Cascade 범위에 맞는 컬링용 Light ViewProjection을 만든다.
            // 투영된 경계의 X/Y 범위로 그림자 맵 영역과 겹치는 물체를 찾는다.

            const FMatrix CullingLightVP = ShadowRenderer->BuildDirectionalLightVPFromCorners(SplitCorners, LightDirection, Resolution);

            TArray<FPrimitiveSceneProxy *> &ShadowCasters = CascadeShadowCasters[Index];

            GatherShadowCasters(Scene, CullingLightVP, FrustumSidePlanesMask, ShadowCasters);

            const FMatrix LightView = ShadowRenderer->BuildDirectionalLightView(SplitCorners, LightDirection);

            const FBox ReceiverBounds = ShadowRenderer->CalculateLightSpaceBounds(SplitCorners, LightView);

            float MinCasterDepth = ReceiverBounds.Min.X;

            // Receiver의 최소 깊이를 기준으로,
            // 수집된 caster가 더 광원 쪽에 있으면 Near 경계를 확장한다.
            for (const FPrimitiveSceneProxy* Proxy : ShadowCasters)
            {
                const FAABB &Bounds = Proxy->GetBounds();

                const FBox WorldBounds{Bounds.Center - Bounds.Extent, Bounds.Center + Bounds.Extent};

                 const FBox LightBounds = WorldBounds.GetWorldAABB(LightView);

                 MinCasterDepth = std::min(MinCasterDepth, LightBounds.Min.X);
            }

            CascadeConstants.LightVP[Index] = ShadowRenderer->BuildDirectionalLightVPFromCorners(SplitCorners, LightDirection, Resolution, MinCasterDepth);

            SplitNear = SplitFar;
            Weight *= DistributionExponent;

            CascadeConstants.CSMEnabled = 1u;
        }
    }
    RenderCommand::UpdateBufferData(CascadeDebugCB.get(), &CascadeConstants, sizeof(CascadeConstants));
}*/

void FRenderer::UpdateCascadeDebugConstants(const FScene& Scene, const FMatrix& CameraWorld, const FVector& CameraForward, float CameraNear, float CameraFar, bool bOrthographic, float TanHalfFovX, float TanHalfFovY)
{
    CascadeConstants = {};

    for (auto& Casters : CascadeShadowCasters)
    {
        Casters.Reset();
    }

    CascadeConstants.BlendEnabled = bCascadeBlendEnabled ? 1u : 0u;

    const FVector Forward = CameraForward.Normalized();
    CascadeConstants.CameraForward = FVector4(Forward.X, Forward.Y, Forward.Z, 0.0f);

    CascadeConstants.DebugColorsEnabled = bCascadeDebugColors ? 1u : 0u;

    const uint32 Count = std::min(CascadeShadowMaps.GetCascadeCount(), 4u);

    const uint32 Resolution = CascadeShadowMaps.GetResolution();
    const float EndDepth = std::min(CameraFar, CSMShadowDistance);

    const bool bCanUseCSM = {
        bCSMEnabled &&
        !bOrthographic && // Perspective 뷰에서만 CSM을 적용
        ShadowRenderer &&
        CascadeShadowMaps.GetArraySRV() &&
        LightData.DirectionalLight.ShadowParameters.W > 0.0f &&
        LightData.DirectionalLight.ColorIntensity.W > 0.0f &&
        Count > 0 && Resolution > 0 &&
        CameraNear > 0.0f &&
        CameraFar > CameraNear && EndDepth > CameraNear
    };

    if (bCanUseCSM)
    {
        CascadeConstants.CascadeCount = Count;
        CascadeConstants.ShadowDistance = EndDepth;
        CascadeConstants.InvResolution = 1.0f / static_cast<float>(Resolution);

        CascadeConstants.SplitDepths = FVector4(EndDepth);
        CascadeConstants.BlendEndDepths = FVector4(EndDepth);

        const FVector4& Direction = LightData.DirectionalLight.Direction;
        const FVector LightDirection(Direction.X, Direction.Y, Direction.Z);
        //const auto CameraCorners = GetFrustumCornersWorld(CameraViewProjection);

        // 기본 분할 정책 E = 3;
        //constexpr float DistributionExponent = 3.0f;
        // 

        const float DistributionExponent = CSMDistributionExponent;

        float TotalWeight = 0.0f;
        float Weight = 1.0f;

        // Count개의 캐스케이드에 필요한 가중치 합을 계산
        for (uint32 Index = 0; Index < Count; ++Index)
        {
            TotalWeight += Weight;
            Weight *= DistributionExponent;
        }

        const float BaseLength = (EndDepth - CameraNear) / TotalWeight;

        float SplitNear = CameraNear;
        Weight = 1.0f;

        // 캐스케이드마다 하나씩 LightVP를 만든다.
        // Count가 4라면 Index는 0, 1, 2, 3.

        // UI Bias를 고정된 월드 거리로 환산한다. 다른 Cascade의 깊이 범위를 참조하지 않는다.
        constexpr float UIBiasToWorld = 1.0f;
        // 기본 거리 보정부터 사용하며, 필요하면 텍셀 비례 추가량을 조정한다.
        constexpr float TexelBiasAmount = 0.0f;

        const auto GetNormalizedTexelSize = [](const FMatrix& LightVP, float MapResolution) -> float
        {
            // 행 벡터 방식이므로 각 "열"의 XYZ 길이를 사용한다.
            const FVector WidthColumn(
                LightVP.M[0][0],
                LightVP.M[1][0],
                LightVP.M[2][0]);

            const FVector DepthColumn(
                LightVP.M[0][2],
                LightVP.M[1][2],
                LightVP.M[2][2]);

            const float WidthScale = WidthColumn.Size(); // 투영 폭
            const float DepthScale = DepthColumn.Size(); // 깊이 범위

            if (WidthScale <= 0.0f || DepthScale <= 0.0f || MapResolution <= 0.0f)
            {
                return 0.0f;
            }

            const float ProjectionWidth = 2.0f / WidthScale;
            const float DepthRange = 1.0f / DepthScale;
            const float WorldTexelSize = ProjectionWidth / MapResolution;

            return WorldTexelSize / DepthRange;
        };

        for (uint32 Index = 0; Index < Count; ++Index)
        {
            // ──────────────────────
            // 이번 Cascade가 담당할 카메라 깊이 구간 결정
            // ──────────────────────
            // SplitNear: 이번 구간의 시작 깊이
            // SplitFar : 이번 구간의 끝 깊이.
            //
            //
            // 첫 Cascade의 SplitNear는 CameraNear에서 시작한다.
            // 이후에는 이전 Cascade SplitFar를 이어받는다.
            //
            // BaseLength * Weight가 이번 구간의 길이다.
            // Weight가 1 -> 3 -> 9 -> 27로 증가하므로,
            // 멀리 있는 Cascade면 더 넓은 깊이 구간을 담당한다.
            //
            // 마지막 Cascade는 끝을 EndDepth로 명시한다.

            const float SplitFar = Index == Count - 1 ? EndDepth : SplitNear + BaseLength * Weight;

            // 이번 Cascade의 기본 경계와 다음 Cascade로 전환할 구간을 계산한다.
            //
            // SplitDepths[Index]    : 확장 전 끝 깊이. 다음 Cascade와의 보간 시작점.
            // BlendEndDepths[Index] : 확장된 끝 깊이. 다음 Cascade와의 보간 종료점.
            //
            // 예:
            // 기본 범위가 0~100이고 전환 비율이 10% 라면
            // 현재 맵은 0~110까지 렌더링하고, 100~110에서 다음 맵의 결과와 보간한다.
            // 
            // 현재 Cascade 깊이 구간의 10%를 다음 캐스케이드와의 전환 길이로 사용한다.
            constexpr float BlendFraction = 0.1f;

            // 다음 Cascade 있으면 전환 길이를 계산한다.
            // 마지막 Cascade는 다음 맵과 보간하지 않으므로 0으로 설정한다.
            const float FadeLength = Index + 1 < Count ? (SplitFar - SplitNear) * BlendFraction : 0.0f;

            // 렌더링 시작 깊이는 원래 구간의 시작을 유지한다.
            const float RenderNear = SplitNear;

            // 끝 깊이를 전환 길이만큼 확장하여 다음 캐스케이드와 겹치는 구간을 만든다.
            // 전체 그림자 거리인 EndDepth를 넘지 않도록 제한한다.
            const float RenderFar = std::min(SplitFar + FadeLength, EndDepth);

            // 원래 끝 깊이를 보간 시작점으로 저장한다.
            CascadeConstants.SplitDepths[Index] = SplitFar;

            // 확장된 끝 깊이를 보간 종료점으로 저장한다.
            // 셰이더는 SplitDepths[Index] ~ BlendEndDepths[Index] 구간에서
            // 현재 맵과 다음 맵의 결과를 섞는다.
            CascadeConstants.BlendEndDepths[Index] = RenderFar;


            // ───────────────────────
            // 이번 깊이 구간을 전체 카메라 구간의 비율로 변환
            // ───────────────────────
            // 전체 카메라 깊이 범위:
            // CameraNear ─CameraFar
            //     0           1
            //
            // 이번 구간의 시작과 끝이 이 범위에서 어디에 있는지 구한다.
            //
            // 예:
            // CameraNear = 1, CameraFar = 101
            // SplitNear  = 11, SplitFar = 31
            // 
            // 전환 길이가 구간 길이의 10%라면:
            // FadeLength = (31 - 11) * 0.1 = 2
            // RenderNear = 11
            // RenderFar = 33(EndDepth에 의해 제한되지 않는 경우)
            // 
            // NearRatio = (11 - 1) / (101 - 1) = 0.1
            // FarRatio  = (33 - 1) / (101 - 1) = 0.32
            // 이 비율로 카메라 프러스텀의 Near~Far 꼭짓점 사이를 보간하여,
            // 이번 Cascade를 렌더링할 프러스텀 꼭짓점을 구한다.

            const float NearRatio = (RenderNear - CameraNear) / (CameraFar - CameraNear);
            const float FarRatio = (RenderFar - CameraNear) / (CameraFar - CameraNear);

            // 이번 Cascade 프러스텀 꼭짓점 8개를 저장한다.
            //
            // [0~3]: 이번 구간의 Near 평면 꼭짓점
            // [4~7]: 이번 구간의 Far 평면 꼭짓점
            //
            // 모든 좌표는 World Space 좌표기준

            std::array<FVector, 8> SplitCorners{};

            // ──────────────────────
            // 전체 카메라 프러스텀에서 이번 구간만 잘라내기
            // ──────────────────────

            for (uint32 Plane = 0; Plane < 2; ++Plane)
            {
                const float Depth = Plane == 0 ? RenderNear : RenderFar;
                const float HalfWidth = Depth * TanHalfFovX;
                const float HalfHeight = Depth * TanHalfFovY;

                for (uint32 Corner = 0; Corner < 4; ++Corner)
                {
                    const float YSign = (Corner & 1u) ? 1.0f : -1.0f;
                    const float ZSign = (Corner & 2u) ? 1.0f : -1.0f;

                    const FVector LocalCorner(Depth, YSign * HalfWidth, ZSign * HalfHeight);

                    SplitCorners[Plane * 4 + Corner] = CameraWorld.TransformPosition(LocalCorner);
                }
            }

            // 카메라 위치·회전의 영향을 받지 않는 반경.
            const float HalfDepth = (RenderFar - RenderNear) * 0.5f;
            const float FarHalfWidth = RenderFar * TanHalfFovX;
            const float FarHalfHeight = RenderFar * TanHalfFovY;

            const float StableRadius = std::sqrt(HalfDepth * HalfDepth + FarHalfWidth * FarHalfWidth + FarHalfHeight * FarHalfHeight);


            // ──────────────────────
            // Cascade 그림자 생성 후보 수집
            // ──────────────────────

            // SplitCorners가 나타내는 공간을 기준으로 light camera를 배치하고,
            // LightDirection 방향을 바라보도록 한다.
            //
            // 이번 Cascade 범위에 맞는 컬링용 Light ViewProjection을 만든다.
            // 투영된 경계의 X/Y 범위로 그림자 맵 영역과 겹치는 물체를 찾는다.

            const FMatrix CullingLightVP =  ShadowRenderer->BuildDirectionalLightVPFromCorners(SplitCorners, LightDirection, Resolution,std::numeric_limits<float>::infinity(), StableRadius);

            TArray<FPrimitiveSceneProxy*>& ShadowCasters = CascadeShadowCasters[Index];

            Scene.BVH.Query(
                [&CullingLightVP](const FBox& WorldBounds) {
                    const FBox ProjectedBounds = WorldBounds.GetWorldAABB(CullingLightVP);

                    // 깊이는 이후 caster를 기준으로 확장하므로 여기서는 검사하지 않는다.
                    return ProjectedBounds.Min.X <= 1.0f && ProjectedBounds.Max.X >= -1.0f &&
                        ProjectedBounds.Min.Y <= 1.0f && ProjectedBounds.Max.Y >= -1.0f;
                },
                [&ShadowCasters](FPrimitiveSceneProxy* Proxy) {
                    if (!Proxy || !Proxy->IsVisible() || !Proxy->GetCastShadows() || !Proxy->GetMesh() || !Proxy->GetRenderState() || Proxy->GetLODCount() == 0)
                    {
                        return;
                    }

                    ShadowCasters.Add(Proxy);
                });

            RecordShadowCasterQuery(Scene, ShadowCasters.Num());

            const FMatrix LightView = ShadowRenderer->BuildDirectionalLightView(SplitCorners, LightDirection);

            const FBox ReceiverBounds = ShadowRenderer->CalculateLightSpaceBounds(SplitCorners, LightView);

            float MinCasterDepth = ReceiverBounds.Min.X;

            // Receiver의 최소 깊이를 기준으로,
            // 수집된 caster가 더 광원 쪽에 있으면 Near 경계를 확장한다.
            for (const FPrimitiveSceneProxy* Proxy : ShadowCasters)
            {
                const FAABB& Bounds = Proxy->GetBounds();

                const FBox WorldBounds{ Bounds.Center - Bounds.Extent, Bounds.Center + Bounds.Extent };

                const FBox LightBounds = WorldBounds.GetWorldAABB(LightView);

                MinCasterDepth = std::min(MinCasterDepth, LightBounds.Min.X);
            }

            // CascadeConstants.LightVP[Index] = ShadowRenderer->BuildDirectionalLightVPFromCorners(SplitCorners, LightDirection, Resolution, MinCasterDepth);
            CascadeConstants.LightVP[Index] = ShadowRenderer->BuildDirectionalLightVPFromCorners(SplitCorners, LightDirection, Resolution, MinCasterDepth, StableRadius);

            const FMatrix& FinalLightVP = CascadeConstants.LightVP[Index];

            const FVector DepthColumn(
                FinalLightVP.M[0][2],
                FinalLightVP.M[1][2],
                FinalLightVP.M[2][2]);

            // 직교 투영의 깊이 열 길이 = 1 / (Far - Near).
            const float InvDepthRange = DepthColumn.Size();
            CascadeConstants.BiasScale[Index] = UIBiasToWorld * InvDepthRange;
            CascadeConstants.BiasOffset[Index] = TexelBiasAmount *
                GetNormalizedTexelSize(FinalLightVP, static_cast<float>(Resolution));

            SplitNear = SplitFar;
            Weight *= DistributionExponent;

            CascadeConstants.CSMEnabled = 1u;
        }
    }
    RenderCommand::UpdateBufferData(CascadeDebugCB.get(), &CascadeConstants, sizeof(CascadeConstants));
}

/*void FRenderer::CaptureCascadePreview()
{
    // 직교 뷰, CSM 비활성, 깊이 패스 실패 시에는 복사하지 않는다.
    if (!bCSMEnabled || CascadeConstants.CSMEnabled == 0 || CascadeConstants.CascadeCount == 0)
    {
        return;
    }

    const uint32 Resolution = CascadeShadowMaps.GetResolution();
    const uint32 Count = CascadeShadowMaps.GetCascadeCount();

    // 모든 Slice가 생성된 경우에만 배열 전체를 보존한다.
    if (CascadeConstants.CascadeCount != Count)
        return;

     // 처음 필요한 시점에 생성한다. 해상도·개수가 바뀌면 재생성한다.
    if (!CascadePreviewMaps.GetArraySRV() || CascadePreviewMaps.GetResolution() != Resolution || CascadePreviewMaps.GetCascadeCount() != Count)
    {
        if (!CascadePreviewMaps.Init(Resolution, Count))
            return;
    }

    // 이전 UI 패스에서 미리보기 배열을 읽었던 슬롯을 해제한다.
    RenderCommand::UnbindShaderResource(0, EShaderBindFlagBits::Pixel | EShaderBindFlagBits::Vertex);
}*/

void FRenderer::CaptureCascadePreview()
{
    const uint32 Count = CascadeShadowMaps.GetCascadeCount();

    // 새 CSM이 없으면 이전 미리보기를 유지한다.
    if (!bCSMEnabled || CascadeConstants.CSMEnabled == 0 || Count == 0 || Count > FCascadeShadowMaps::MaxCascades || CascadeConstants.CascadeCount != Count)
    {
        return;
    }

    ID3D11DeviceContext *Context = RenderCommand::GetContext();

    if (!Context)
        return;

    const FString ShaderPath = "Resources/Shader/CascadeDepthPreview.hlsl";

    FShaderProgram *Program = FRenderResourceManager::GetShaderProgram(ShaderPath);

    // 로드 실패 시 반환될 수 있는 기본 셰이더도 제외한다.
    if (!Program || Program->SourcePath != ShaderPath || !Program->VertexShader || !Program->VertexShader->IsValid() || !Program->PixelShader || !Program->PixelShader->IsValid())
    {
        return;
    }

    for (uint32 Index = 0; Index < Count; ++Index)
    {
        const FTexture2D *Preview = CascadePreviewTextures[Index].get();

        if (!Preview || !Preview->GetRTV() || !CascadeShadowMaps.GetSliceSRV(Index))
        {
            return;
        }
    }

    // 이전 ImGui 패스가 읽던 컬러 텍스처를 해제한다.
    RenderCommand::UnbindShaderResource(0, EShaderBindFlagBits::Pixel | EShaderBindFlagBits::Vertex);

    Context->OMSetRenderTargets(0, nullptr, nullptr);

    RenderCommand::BindShaderProgram(Program);
    RenderCommand::SetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    RenderCommand::SetRasterizerState(ERasterizerState::SolidNone);
    RenderCommand::SetDepthStencilState(EDepthStencilState::Disabled);
    RenderCommand::SetBlendState(EBlendState::Opaque);

    RenderCommand::SetViewport(0, 0, CascadePreviewResolution, CascadePreviewResolution);

    for (uint32 Index = 0; Index < Count; ++Index)
    {
        ID3D11RenderTargetView *DestinationRTV = CascadePreviewTextures[Index]->GetRTV();

        ID3D11ShaderResourceView *SourceSRV = CascadeShadowMaps.GetSliceSRV(Index);

        // 깊이 타깃 없이 컬러 텍스처에만 기록한다.
        Context->OMSetRenderTargets(1, &DestinationRTV, nullptr);

        Context->PSSetShaderResources(0, 1, &SourceSRV);

        RenderCommand::Draw(3);
    }

    RenderCommand::UnbindShaderResource(0, EShaderBindFlagBits::Pixel);

    // ImGui가 컬러 텍스처를 SRV로 읽을 수 있도록 해제한다.
    Context->OMSetRenderTargets(0, nullptr, nullptr);

    RenderCommand::SetRasterizerState(ERasterizerState::SolidBack);
    RenderCommand::SetDepthStencilState(EDepthStencilState::Default);

    CascadePreviewCount = Count;
}

bool FRenderer::RequestCSMSettings(uint32 Resolution, uint32 CascadeCount)
{
     const FCSMSettings Requested{Resolution, CascadeCount};

    if (!Requested.IsValid())
        return false;

    // 여러 요청이 들어오면 마지막 요청을 사용한다.
    PendingCSMSettings = Requested;

     bHasPendingCSMSettings = Requested.Resolution != CSMSettings.Resolution || Requested.CascadeCount != CSMSettings.CascadeCount;

    return true;
}

bool FRenderer::ApplyPendingCSMSettings()
{
    if (!bHasPendingCSMSettings)
        return true;

    const FCSMSettings Requested = PendingCSMSettings;
    bHasPendingCSMSettings = false;

    if (!RecreateCSMResources(Requested))
    {
        HTR_LOG(Warning, "[CSM] Failed to apply settings. Keeping existing resources.");

        return false;
    }

    return true;
}

//void FRenderer::UpdateCascadeDebugConstants(const FVector &CameraForward)
//{
//    FCascadeDebugConstants Constants{};
//
//    const float DistributionExponent = 3.0f; // E. 1이면 균등 분할
//    const float StartDepth = 0.1f; // 카메라 Near
//    const float EndDepth = 400.0f; // CSM 최대 거리(언리얼 참고함)
//
//    const float E = DistributionExponent;
//    const float E2 = E * E;
//    const float E3 = E2 * E;
//
//    // 구간 길이의 비율: 1 : E : E^2 : E^3
//    const float BaseLength = (EndDepth - StartDepth) / (1.0f + E + E2 + E3);
//
//    Constants.SplitDepths = FVector4(StartDepth + BaseLength, StartDepth + BaseLength * (1.0f + E), StartDepth + BaseLength * (1.0f + E + E2), 0.0f);
//
//    const FVector Forward = CameraForward.Normalized();
//    Constants.CameraForward = FVector4(Forward.X, Forward.Y, Forward.Z, 0.0f);
//
//    Constants.Enabled = bCascadeDebugColors ? 1u : 0u;
//    Constants.CascadeCount = CascadeShadowMaps.GetCascadeCount();
//
//    const uint32 Resolution = CascadeShadowMaps.GetResolution();
//    Constants.InvResolution = Resolution > 0 ? 1.0f / static_cast<float>(Resolution) : 0.0f;
//
//    RenderCommand::UpdateBufferData(CascadeDebugCB.get(), &Constants, sizeof(Constants));
//}



void FRenderer::BindLightResources()
{
	RenderCommand::BindConstantBuffer(3, LightCB.get(), EShaderBindFlagBits::Pixel | EShaderBindFlagBits::Vertex);
    

	// Gouraud는 VS에서, 나머지 Lit은 PS에서 라이트를 읽으므로 두 스테이지에 모두 꽂는다.
	constexpr uint32 LightSRVSlot = 8;   // UberLit의 register(t8), register(t9)
	ID3D11ShaderResourceView* LightSRVs[2] = { PointLightSRV.Get(), SpotLightSRV.Get() };
	ID3D11DeviceContext* Context = RenderCommand::GetContext();
	Context->VSSetShaderResources(LightSRVSlot, 2, LightSRVs);
	Context->PSSetShaderResources(LightSRVSlot, 2, LightSRVs);

    RenderCommand::BindShaderResourceArray(10, GetDirectionalShadowMap(), EShaderBindFlagBits::Pixel | EShaderBindFlagBits::Vertex);
    RenderCommand::BindShaderResourceArray(14, GetShadowMap(), EShaderBindFlagBits::Pixel | EShaderBindFlagBits::Vertex);
    
    ID3D11ShaderResourceView* CascadeSRV = CascadeShadowMaps.GetArraySRV();
    Context->VSSetShaderResources(13, 1, &CascadeSRV);
    Context->PSSetShaderResources(13, 1, &CascadeSRV);

    RenderCommand::BindSamplerState(1, ESamplerState::PointBorder, EShaderBindFlagBits::Pixel | EShaderBindFlagBits::Vertex);
    RenderCommand::BindConstantBuffer(5, CascadeDebugCB.get(), EShaderBindFlagBits::Pixel | EShaderBindFlagBits::Vertex);   
}

void FRenderer::RenderOpaque(const FMatrix& ViewProjection, const EViewMode ViewMode, bool bDepthPrepassed)
{
	BindLightResources();

	this->bDepthPrepassed = bDepthPrepassed;
	DrawStaticGroups(ViewMode);
	DrawPackets(0, FirstTranslucentIndex, ViewProjection, ViewMode);
	this->bDepthPrepassed = false;
}

// RenderOpaque가 남긴 반투명 패킷을 먼 것부터 그린다.
void FRenderer::RenderTranslucent(const FMatrix& ViewProjection, const EViewMode ViewMode)
{
	BindLightResources();

	DrawPackets(FirstTranslucentIndex, SortEntries.Num(), ViewProjection, ViewMode);
	RenderPackets.Reset();
	FirstTranslucentIndex = 0;
	StaticGroups.clear();   // 묶음 메모리는 World 것이므로 이번 프레임이 끝나면 놓는다
}

// 스태틱 메시 묶음을 정렬 키 순서로 그린다. 바인딩은 묶음마다 한 번, 항목마다는 칸 바인딩과 드로우만 한다.
void FRenderer::DrawStaticGroups(const EViewMode ViewMode)
{
	if (StaticGroups.empty())
		return;

	SCOPE_CYCLE_COUNTER(STAT_DrawRenderPackets);
	RenderCommand::BindConstantBuffer(0, ViewCB.get(), EShaderBindFlagBits::Vertex | EShaderBindFlagBits::Pixel);

	UMaterial* BoundMaterial = nullptr;
	for (const FStaticDrawGroup* Group : StaticGroups)
	{
		RenderCommand::BindMesh(Group->Mesh, Group->LODIndex);
		if (Group->Material != BoundMaterial)
		{
			BoundMaterial = Group->Material;
			BindMaterial(BoundMaterial, ViewMode);
			FRenderPacket MaterialOnly;           // 머티리얼 파라미터 갱신은 패킷을 받으므로 머티리얼만 채워 넘긴다
			MaterialOnly.Material = BoundMaterial;
			UpdateMaterialParams(MaterialOnly);
		}

		for (const FStaticDrawItem &Item : Group->Items)
        {
            if (bShadowMapPass && Item.Proxy && !Item.Proxy->GetCastShadows())
            {
                continue;
            }                

			if (bUsePerObjectSlots && Item.Slot != InvalidObjectSlot)
			{
				RenderCommand::BindConstantBufferRange(2, PerObjectSlotCB.get(), Item.Slot * PerObjectSlotConstants, PerObjectSlotConstants, EShaderBindFlagBits::Vertex | EShaderBindFlagBits::Pixel);
			}
			else
			{
				RenderCommand::BindConstantBuffer(2, PerObjectCB.get(), EShaderBindFlagBits::Vertex | EShaderBindFlagBits::Pixel);                
				UpdatePerObjectConstants(Item.Proxy->GetLocalToWorld(), Item.Proxy ? Item.Proxy->GetReceiveShadows() : false);
			}
			RenderCommand::DrawIndexed(Item.IndexCount, Item.StartIndex);
		}
	}

	// 뒤따르는 DrawPackets가 처음부터 다시 바인딩하도록 기록을 비운다.
	LastMesh = nullptr;
	LastMaterial = nullptr;
}

void FRenderer::RenderQueueSorting(FRenderQueue& InQueue, const FMatrix& ViewProjection, const FVector& CameraPosition)
{
	FViewConstants ViewConstants;
	ViewConstants.VP = ViewProjection;
	ViewConstants.CameraPosition = CameraPosition;
	RenderCommand::UpdateBufferData(ViewCB.get(), &ViewConstants, sizeof(FViewConstants));
	{
		SCOPE_CYCLE_COUNTER(STAT_RenderQueueSorting);

		std::swap(RenderPackets, InQueue);
		InQueue.Reset();

		// ① 패킷을 한 번 훑으며 키를 만든다. 그릴 수 없는 패킷은 목록에 넣지 않는다.
		SortEntries.Reset();
		SortEntries.Reserve(RenderPackets.Num());
		for (uint32 i = 0; i < RenderPackets.Num(); ++i)
		{
			const FRenderPacket& P = RenderPackets[i];
			if (!P.Mesh || !P.Material) continue;
			SortEntries.Add({ MakeSortKey(P), i });
		}

		// ② 16B 항목만 정렬
		std::sort(SortEntries.begin(), SortEntries.end(),
			[](const FSortEntry& A, const FSortEntry& B) { return A.Key < B.Key; });

		// ③ 반투명 시작 위치 = 최상위 비트가 처음 1인 곳
		FirstTranslucentIndex = 0;
		while (FirstTranslucentIndex < SortEntries.Num() && !(SortEntries[FirstTranslucentIndex].Key >> 63))
			++FirstTranslucentIndex;

		// ④ 스태틱 메시 묶음: 빈 묶음을 빼고 키 순으로 정렬한다. 조각마다 같은 키 묶음이 있으므로 정렬하면 서로 붙는다.
		std::erase_if(StaticGroups, [](const FStaticDrawGroup* Group) { return Group->Items.empty(); });
		std::sort(StaticGroups.begin(), StaticGroups.end(),
			[](const FStaticDrawGroup* A, const FStaticDrawGroup* B) { return MakeGroupKey(*A) < MakeGroupKey(*B); });
	}
	// Gather uploads group and packet slots together. Other queues need an upload here.
	if (!bObjectConstantsPrepared)
		UploadPerObjectConstants();
	bObjectConstantsPrepared = false;
}

// 정렬된 패킷 중 [Begin, End) 범위를 View 행렬과 Section 범위로 그린다.
void FRenderer::DrawPackets(uint32 Begin, uint32 End, const FMatrix& ViewProjection, const EViewMode ViewMode)
{
	SCOPE_CYCLE_COUNTER(STAT_DrawRenderPackets);

	LastMesh = nullptr;
	LastMaterial = nullptr;
	uint8 LastLODIndex = 0;

	RenderCommand::BindConstantBuffer(0, ViewCB.get(), EShaderBindFlagBits::Vertex | EShaderBindFlagBits::Pixel);


	for (uint32 k = Begin; k < End; ++k)          // k = 정렬된 위치
	{
		const uint32 PacketIndex = SortEntries[k].PacketIndex;
		const FRenderPacket& RenderPacket = RenderPackets[PacketIndex];
		//if (RenderPacket.Mesh == nullptr || RenderPacket.Material == nullptr) continue;

        if (bShadowMapPass && RenderPacket.Proxy && !RenderPacket.Proxy->GetCastShadows())
        {
            continue; 
        }

		if (RenderPacket.Mesh != LastMesh || RenderPacket.LODIndex != LastLODIndex) {
			RenderCommand::BindMesh(RenderPacket.Mesh, RenderPacket.LODIndex);
		}

		if (RenderPacket.Material != LastMaterial)
			BindMaterial(RenderPacket.Material, ViewMode);
			
		if (RenderPacket.Material != LastMaterial || RenderPacket.MaterialParamData)
			UpdateMaterialParams(RenderPacket);

		if (bUsePerObjectSlots && RenderPacket.Slot != InvalidObjectSlot)
		{
			RenderCommand::BindConstantBufferRange(2, PerObjectSlotCB.get(), RenderPacket.Slot * PerObjectSlotConstants, PerObjectSlotConstants, EShaderBindFlagBits::Vertex | EShaderBindFlagBits::Pixel);
		}
		else
		{
			RenderCommand::BindConstantBuffer(2, PerObjectCB.get(), EShaderBindFlagBits::Vertex | EShaderBindFlagBits::Pixel);
			UpdatePerObjectConstants(RenderPacket, ViewProjection, RenderPacket.Proxy ? RenderPacket.Proxy->GetReceiveShadows() : false);
		}

		RenderCommand::DrawIndexed(
			RenderPacket.IndexCount ? RenderPacket.IndexCount : RenderPacket.Mesh->GetIndexBuffer(RenderPacket.LODIndex)->GetIndexCount(),
			RenderPacket.StartIndex
		);
		LastMaterial = RenderPacket.Material;
		LastMesh = RenderPacket.Mesh;
		LastLODIndex = RenderPacket.LODIndex;
	}
}

FOcclusionMeasureResult FRenderer::MeasureOpaqueOcclusion(const FMatrix& ViewProjection, const EViewMode ViewMode)
{
	FOcclusionMeasureResult Result;

	// 불투명 드로우를 두 경로(스태틱 묶음, 일반 패킷)에서 한 목록으로 모은다. 그린 순서와 같게 묶음 먼저.
	struct FMeasureDraw
	{
		UStaticMesh* Mesh;
		UMaterial* Material;
		uint8 LODIndex;
		uint32 Slot;
		uint32 StartIndex;
		uint32 IndexCount;
		const FMatrix* World;       // 칸이 없을 때만 쓴다
		const void* ObjectKey;      // 같은 물체의 섹션을 한 물체로 센다
		bool bOccludedByGpu;
		const FRenderPacket* Packet;
	};
	std::vector<FMeasureDraw> Draws;
	for (const FStaticDrawGroup* Group : StaticGroups)
		for (const FStaticDrawItem& Item : Group->Items)
			Draws.push_back({ Group->Mesh, Group->Material, Group->LODIndex, Item.Slot, Item.StartIndex, Item.IndexCount,
				&Item.Proxy->GetLocalToWorld(), Item.Proxy, Item.bOccludedByGpu != 0, nullptr });
	for (uint32 k = 0; k < FirstTranslucentIndex; ++k)
	{
		const FRenderPacket& Packet = RenderPackets[SortEntries[k].PacketIndex];
		const uint32 IndexCount = Packet.IndexCount ? Packet.IndexCount : Packet.Mesh->GetIndexBuffer(Packet.LODIndex)->GetIndexCount();
		// 프록시가 없는 패킷(빌보드 등)은 패킷 자체를 한 물체로 센다.
		const void* Key = Packet.Proxy ? static_cast<const void*>(Packet.Proxy) : static_cast<const void*>(&Packet);
		Draws.push_back({ Packet.Mesh, Packet.Material, Packet.LODIndex, Packet.Slot, Packet.StartIndex, IndexCount,
			&GetPacketWorld(Packet), Key, Packet.bOccludedByGpu, &Packet });
	}

	const uint32 Count = static_cast<uint32>(Draws.size());
	if (Count == 0)
		return Result;

	const auto StartTime = std::chrono::high_resolution_clock::now();

	ID3D11Device* Device = RenderCommand::GetDevice();
	ID3D11DeviceContext* Context = RenderCommand::GetContext();

	// 같은 셰이더·같은 행렬로 다시 그리면 깊이가 비트 단위로 같으므로,
	// LESS_EQUAL이면 최종 깊이 버퍼에서 이 물체가 이긴 픽셀만 통과한다.
	if (!DepthLessEqualReadOnly)
	{
		D3D11_DEPTH_STENCIL_DESC Desc{};
		Desc.DepthEnable = TRUE;
		Desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		Desc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
		if (FAILED(Device->CreateDepthStencilState(&Desc, DepthLessEqualReadOnly.GetAddressOf())))
			return Result;
	}

	while (OcclusionQueries.Num() < Count)	
	{
		const D3D11_QUERY_DESC QueryDesc{ D3D11_QUERY_OCCLUSION, 0 };
		ComPtr<ID3D11Query> Query;
		if (FAILED(Device->CreateQuery(&QueryDesc, Query.GetAddressOf())))
			return Result;
		OcclusionQueries.Add(std::move(Query));
	}

	RenderCommand::BindConstantBuffer(0, ViewCB.get(), EShaderBindFlagBits::Vertex | EShaderBindFlagBits::Pixel);
	if (!bUsePerObjectSlots)
		RenderCommand::BindConstantBuffer(2, PerObjectCB.get(), EShaderBindFlagBits::Vertex);

	UStaticMesh* BoundMesh = nullptr;
	UMaterial* BoundMaterial = nullptr;
	uint8 BoundLOD = 0;

	// 1) 드로우마다 쿼리를 걸고 다시 그린다. 본 패스와 같은 바인딩(칸 또는 PerObjectCB)을 쓴다.
	for (uint32 Index = 0; Index < Count; ++Index)
	{
		const FMeasureDraw& Draw = Draws[Index];

		if (Draw.Mesh != BoundMesh || Draw.LODIndex != BoundLOD)
			RenderCommand::BindMesh(Draw.Mesh, Draw.LODIndex);


		if (Draw.Material != BoundMaterial)
		{
			BindMaterial(Draw.Material, ViewMode);
			// BindMaterial이 바꾼 상태를 측정용으로 덮어쓴다.
			RenderCommand::SetBlendState(EBlendState::NoColorWrite);
			Context->OMSetDepthStencilState(DepthLessEqualReadOnly.Get(), 0);
		}
		if (Draw.Packet && (Draw.Material != BoundMaterial || Draw.Packet->MaterialParamData))
			UpdateMaterialParams(*Draw.Packet);

		if (bUsePerObjectSlots && Draw.Slot != InvalidObjectSlot)
			RenderCommand::BindConstantBufferRange(2, PerObjectSlotCB.get(),
				Draw.Slot * PerObjectSlotConstants, PerObjectSlotConstants, EShaderBindFlagBits::Vertex | EShaderBindFlagBits::Pixel);
		else
		{
			RenderCommand::BindConstantBuffer(2, PerObjectCB.get(), EShaderBindFlagBits::Vertex);
			UpdatePerObjectConstants(*Draw.World);
		}

		ID3D11Query* Query = OcclusionQueries[Index].Get();
		Context->Begin(Query);
		RenderCommand::DrawIndexed(Draw.IndexCount, Draw.StartIndex);
		Context->End(Query);

		BoundMesh = Draw.Mesh;
		BoundMaterial = Draw.Material;
		BoundLOD = Draw.LODIndex;
	}

	// 2) 결과를 기다려 모은다. 한 물체가 Section 여러 개로 나뉘면 하나라도 보이면 보이는 것으로 친다.
	std::unordered_map<const void*, bool> ObjectVisible;
	ObjectVisible.reserve(Count);
	for (uint32 Index = 0; Index < Count; ++Index)
	{
		const FMeasureDraw& Draw = Draws[Index];

		UINT64 Samples = 0;
		while (Context->GetData(OcclusionQueries[Index].Get(), &Samples, sizeof(Samples), 0) == S_FALSE) {}

		const uint64 Triangles = Draw.IndexCount / 3;
		const bool bVisible = Samples > 0;

		// Cull을 끄고 판정만 한 드로우: 가렸다고 했는데 최종 화면에 픽셀이 남았으면 잘못 가린 것이다.
		if (Draw.bOccludedByGpu)
		{
			++Result.GPUOccludedDraws;
			if (bVisible)
				++Result.FalseCulls;
		}

		++Result.TotalDraws;
		Result.TotalTriangles += Triangles;
		if (bVisible)
		{
			++Result.VisibleDraws;
			Result.VisibleTriangles += Triangles;
		}

		bool& bObjectVisible = ObjectVisible[Draw.ObjectKey];
		bObjectVisible = bObjectVisible || bVisible;
	}

	Result.TotalObjects = static_cast<uint32>(ObjectVisible.size());
	for (const auto& [Key, bVisible] : ObjectVisible)
		Result.VisibleObjects += bVisible ? 1 : 0;

	// 뒤따르는 Grid·반투명 패스를 위해 상태를 되돌린다.
	RenderCommand::SetBlendState(EBlendState::Opaque);
	RenderCommand::SetDepthStencilState(EDepthStencilState::Default);

	Result.ElapsedMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - StartTime).count();
	Result.bValid = true;
	return Result;
}

uint8* FRenderer::BeginObjectConstants(uint32 MaxSlots)
{
	bObjectConstantsPrepared = false;
	if (!bUsePerObjectSlots || MaxSlots == 0) return nullptr;
	EnsurePerObjectSlotCapacity(MaxSlots);
	if (!PerObjectSlotCB) { bUsePerObjectSlots = false; return nullptr; }
	return static_cast<uint8*>(RenderCommand::MapWriteDiscard(PerObjectSlotCB.get()));
}

void FRenderer::EndObjectConstants()
{
	if (PerObjectSlotCB)
	{
		RenderCommand::Unmap(PerObjectSlotCB.get());
		bObjectConstantsPrepared = true;
	}
}

// Material마다 Shader/Texture/Sampler/State 꽂기
void FRenderer::BindMaterial(UMaterial* material, const EViewMode ViewMode)
{
	FShaderProgram* Shader = material->Shader;
	const char* Permutation = ViewModeToPermutation[(int)ViewMode];

	//이전 View와 Permutation이 다르다면, 셰이더를 교체해준다.
	if (Shader && !Shader->Permutation.empty() && Shader->Permutation.compare(Permutation))
	{
		if (FShaderProgram* Variant = FRenderResourceManager::GetShaderProgram(Shader->SourcePath, ViewModeToPermutation[(int)ViewMode]))
			Shader = Variant;
	}

	RenderCommand::BindShaderProgram(Shader);

	//Depth Prepass인 경우, Pixel Shader을 nullptr로, DepthStencilState를 Default로 한다.
	//그 뒤의 머테리얼 설정은 깊이 프리패스에 필요 없으므로 건너뛴다.
	if (bDepthOnly)
	{
		RenderCommand::GetContext()->PSSetShader(nullptr, nullptr, 0); // PS 없이 깊이만
		RenderCommand::SetDepthStencilState(EDepthStencilState::Default); // LESS + Write
		return;
	}

	RenderCommand::SetBlendState(material->BlendState);

	// Opaque 패스에서 Depth Prepass가 실행되었다면, 기존 깊이값이 버려지지 않기 위해 LessEqualReadOnly로 변경한다.
	const bool bTranslucent = material->BlendState != EBlendState::Opaque;
	if (!bTranslucent && bDepthPrepassed)
		RenderCommand::SetDepthStencilState(EDepthStencilState::LessEqualReadOnly);

	// 반투명은 뒤에 그려지는 Grid·다른 반투명을 가리지 않도록 깊이를 쓰지 않는다.
	else
		RenderCommand::SetDepthStencilState(bTranslucent && material->DepthStencilState == EDepthStencilState::Default
        ? EDepthStencilState::ReadOnly : material->DepthStencilState);

	//for (int i = 0; i < material->Textures.size(); i++)
	//{
	//	RenderCommand::BindShaderResource(i, material->Textures[i], EShaderBindFlagBits::Pixel);
	//}

	int32 TextureSlotCount = static_cast<int32>(material->Textures.Num());

	// StaticMesh는 텍스처 배열이 한 칸이어도 t1까지 처리한다.
	if (material->ParamLayout == EMaterialParamLayout::StaticMesh && TextureSlotCount < 2)
	{
		TextureSlotCount = 2;
	}

	for (int32 Slot = 0; Slot < TextureSlotCount; ++Slot)
	{
		UTexture2D* Texture = Slot < static_cast<int32>(material->Textures.Num()) ? material->Textures[Slot] : nullptr;
		if (material->ParamLayout == EMaterialParamLayout::StaticMesh && Slot == 0 && !Texture)
		{
			Texture = UAssetManager::GetAssetByPath<UTexture2D>("WhiteTexture");
		}

		// Normal Texture를 t1가 아닌 t3에 바인딩
		const uint32 GPUTextureSlot = (material->ParamLayout == EMaterialParamLayout::StaticMesh && Slot == 1) ? 3u : static_cast<uint32>(Slot);

		RenderCommand::BindShaderResource(GPUTextureSlot, Texture, EShaderBindFlagBits::Pixel);
	}

	RenderCommand::BindSamplerState(0, material->SamplerState, EShaderBindFlagBits::Pixel);
}

// b1 내용 채우고 꽂기
void FRenderer::UpdateMaterialParams(const FRenderPacket& RenderPacket)
{
	switch (RenderPacket.Material->ParamLayout)
	{
	case EMaterialParamLayout::StaticMesh:
	{
		//break; // 라이팅 적용 시 제거

		const UMaterial* Material = RenderPacket.Material;
		const float TotalTime = EngineTimer::GetTotalTime();

		UTexture2D* NormalTexture =	Material->Textures.Num() > 1 ? Material->Textures[1] : nullptr;
		FTexture2D* NormalResource = NormalTexture ? NormalTexture->GetResource() : nullptr;

		const bool bHasNormalMap = NormalResource && NormalResource->GetSRV();

		FStaticMeshMaterialParams Params{};
		Params.BaseColor = RenderPacket.Material->BaseColor;
		Params.UVOffset = RenderPacket.Material->UVScrollSpeed * TotalTime;
		Params.bOpaque = RenderPacket.Material->BlendState == EBlendState::Opaque ? 1.0f : 0.0f;
		Params.Shininess = RenderPacket.Material->Shininess;
		Params.NormalStrength = Material->NormalStrength;
		Params.HasNormalMap = bHasNormalMap ? 1.0f : 0.0f;

		RenderCommand::UpdateBufferData(RenderPacket.Material->ParamBuffer.get(), &Params, sizeof(FStaticMeshMaterialParams));
		RenderCommand::BindConstantBuffer(1, RenderPacket.Material->ParamBuffer.get(), EShaderBindFlagBits::Pixel | EShaderBindFlagBits::Vertex);
		break;
	}
	case EMaterialParamLayout::ParticleSubUV:
	{
		if (RenderPacket.Material->ParamBuffer && RenderPacket.MaterialParamData != nullptr)
		{
			RenderCommand::UpdateBufferData(RenderPacket.Material->ParamBuffer.get(), RenderPacket.MaterialParamData, RenderPacket.MaterialParamDataSize);
			RenderCommand::BindConstantBuffer(1, RenderPacket.Material->ParamBuffer.get(), EShaderBindFlagBits::Pixel | EShaderBindFlagBits::Vertex);
		}
		break;
	}
	case EMaterialParamLayout::Billboard:
	{
		if (RenderPacket.Material->ParamBuffer && RenderPacket.MaterialParamData != nullptr)
		{
			RenderCommand::UpdateBufferData(RenderPacket.Material->ParamBuffer.get(), RenderPacket.MaterialParamData, RenderPacket.MaterialParamDataSize);
			RenderCommand::BindConstantBuffer(1, RenderPacket.Material->ParamBuffer.get(), EShaderBindFlagBits::Pixel | EShaderBindFlagBits::Vertex);
		}
		break;
	}
	case EMaterialParamLayout::None:
	{
		break;
	}
	}
}

// b0 MVP 채우고 꽂기
void FRenderer::UpdatePerObjectConstants(const FRenderPacket& RenderPacket, const FMatrix& ViewProjection, const bool bReceiveShadow)
{
	// rp.Transform 과 Camera VP 행렬 곱
	// 행렬곱의 결과 (MVP Matrix) Constant Buffer 업데이트 필요
    FPerObjectConstants Constants;

    Constants.World = GetPacketWorld(RenderPacket);
    Constants.WorldInvTranspose = Constants.World.Inverse().GetTransposed();
    Constants.bReceiveShadow = bReceiveShadow ? 1 : 0;
    RenderCommand::UpdateBufferData(PerObjectCB.get(), &Constants);	
}

void FRenderer::UpdatePerObjectConstants(const FMatrix& World)
{
	FPerObjectConstants Constants;

	Constants.World = World;
	Constants.WorldInvTranspose = World.Inverse().GetTransposed();	

	RenderCommand::UpdateBufferData(PerObjectCB.get(), &Constants);
}

void FRenderer::UpdatePerObjectConstants(const FMatrix &World, const bool bReceiveShadow)
{
    FPerObjectConstants Constants;

    Constants.World = World;
    Constants.WorldInvTranspose = World.Inverse().GetTransposed();
    Constants.bReceiveShadow = bReceiveShadow ? 1 : 0;

    RenderCommand::UpdateBufferData(PerObjectCB.get(), &Constants);
}

void FRenderer::UploadLightGlobalConstants(const FMatrix &DirectionalLightVP)
{
    FLightGlobalConstants Globals{};
    Globals.AmbientLight = LightData.AmbientLight;
    Globals.DirectionalLight = LightData.DirectionalLight;
    Globals.DirectionalLight.ViewProjection = DirectionalLightVP;
    Globals.PointLightCount = LightData.PointLightCount;
    Globals.SpotLightCount = LightData.SpotLightCount;
    Globals.bShowShadows = bShadowsEnabled ? 1u : 0u;
    Globals.ShadowQuality = ShadowQuality;
    Globals.PCSSMaxKernel = PCSSMaxKernel;

    RenderCommand::UpdateBufferData(LightCB.get(), &Globals, sizeof(FLightGlobalConstants));
}

void FRenderer::SetLight(const FLightConstants &InLightData)
{
    LightData = InLightData;

    UploadLightGlobalConstants(LightData.DirectionalLight.ViewProjection);
}

void FRenderer::SetShadowsEnabled(bool bEnabled)
{
    bShadowsEnabled = bEnabled;
    UploadLightGlobalConstants(LightData.DirectionalLight.ViewProjection);
}

void FRenderer::GatherShadowCasters(const FScene& Scene, const FMatrix& LightVP, uint32 PlaneMask, TArray<FPrimitiveSceneProxy*>& OutCasters) const
{
    OutCasters.Reset();

    const FFrustumPlanes Frustum = ExtractFrustumPlanes(LightVP);

    Scene.BVH.QueryCull(
        PlaneMask,
        [&Frustum](const FBox& Bounds, uint32& Mask)
        {
            const EFrustumTest Result = ClassifyBoxInFrustum(Bounds, Frustum, Mask);

            if (Result == EFrustumTest::Outside)
                return EBVHCullResult::Outside;

            if (Result == EFrustumTest::Inside)
                return EBVHCullResult::Inside;

            return EBVHCullResult::Intersect;
        },
        [&OutCasters](FPrimitiveSceneProxy* Proxy)
        {
            if (!Proxy || !Proxy->IsVisible() || !Proxy->GetCastShadows() || !Proxy->GetMesh() || !Proxy->GetRenderState() || Proxy->GetLODCount() == 0)
                return;

            OutCasters.Add(Proxy);
        });
    RecordShadowCasterQuery(Scene, OutCasters.Num());
}

void FRenderer::DrawShadowCasters(const FScene& Scene, const FMatrix& LightVP, EViewMode ViewMode, const TArray<FPrimitiveSceneProxy*>* ShadowCasters)
{
    FViewConstants ViewConstants{};
    ViewConstants.VP = LightVP;
    RenderCommand::UpdateBufferData(ViewCB.get(), &ViewConstants, sizeof(FViewConstants));

    RenderCommand::BindConstantBuffer(0, ViewCB.get(), EShaderBindFlagBits::Vertex | EShaderBindFlagBits::Pixel);
    RenderCommand::BindConstantBuffer(2, PerObjectCB.get(), EShaderBindFlagBits::Vertex | EShaderBindFlagBits::Pixel);

    bDepthOnly = true;
    RenderCommand::SetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    const TArray<FPrimitiveSceneProxy *> &Casters = ShadowCasters ? *ShadowCasters : Scene.Proxies;

    for (const FPrimitiveSceneProxy *Proxy : Casters)
    {
        if (!Proxy || !Proxy->IsVisible() || !Proxy->GetCastShadows())
            continue;

        UStaticMesh *Mesh = Proxy->GetMesh();
        if (!Mesh || !Proxy->GetRenderState() || Proxy->GetLODCount() == 0)
            continue;

        // 현재는 첫 번째 LOD를 사용한다.
        RenderCommand::BindMesh(Mesh, 0);
        UpdatePerObjectConstants(Proxy->GetLocalToWorld(), Proxy->GetReceiveShadows());

        const FCachedMeshLOD &LOD = Proxy->GetLOD(0);

        bool bSubmittedCaster = false;

        for (uint32 i = 0; i < LOD.NumSections; ++i)
        {
            const FCachedMeshSection &Section = Proxy->GetSection(LOD.FirstSection + i);
            if (!Section.Material || !Section.Material->Shader)
                continue;

            // Depth Only 모드이므로 Pixel Shader는 비활성화된다.
            BindMaterial(Section.Material, EViewMode::Unlit);
            RenderCommand::DrawIndexed(Section.IndexCount, Section.StartIndex);
            ++ShadowStats::Pending.DrawCalls;
            ShadowStats::Pending.Triangles += Section.IndexCount / 3;
            bSubmittedCaster = true;
        }

        if (bSubmittedCaster)
            ++ShadowStats::Pending.CasterSubmissions;
    }

    bDepthOnly = false;
}

bool FRenderer::RecreateCSMResources(const FCSMSettings &NewSettings)
{
    ID3D11DeviceContext *Context = RenderCommand::GetContext();

    if (!Context || !NewSettings.IsValid())
        return false;

    // 기존 리소스는 아직 변경하지 않는다.
    FCascadeShadowMaps NewShadowMaps;

    if (!NewShadowMaps.Init(NewSettings.Resolution, NewSettings.CascadeCount))
    {
        return false;
    }

    for (uint32 Index = 0; Index < NewSettings.CascadeCount; ++Index)
    {
        Context->ClearDepthStencilView(NewShadowMaps.GetSliceDSV(Index), D3D11_CLEAR_DEPTH, 1.0f, 0);
    }

    // Init 또는 View의 CSM 준비 단계에서만 호출한다.
    Context->OMSetRenderTargets(0, nullptr, nullptr);

    RenderCommand::UnbindShaderResource(0, EShaderBindFlagBits::Pixel | EShaderBindFlagBits::Vertex);

    RenderCommand::UnbindShaderResource(13, EShaderBindFlagBits::Pixel | EShaderBindFlagBits::Vertex);

    CascadeShadowMaps = std::move(NewShadowMaps);
    CSMSettings = NewSettings;

    // 새 설정에 맞는 상수는 이후 각 View에서 다시 계산한다.
    CascadeConstants = {};

    return true;
}


bool FRenderer::RenderShadowDepth(const FScene& Scene, const FShadowPassDesc& Desc, const FMatrix& LightVP, EViewMode ViewMode, const TArray<FPrimitiveSceneProxy*>* ShadowCasters)
{
    FScopedShadowGPUTime GPUTime(ShadowGPUProfiler);
    if (!ShadowRenderer || !ShadowRenderer->BeginShadowPass(Desc))
        return false;

    ++ShadowStats::Pending.Passes;

    DrawShadowCasters(Scene, LightVP, ViewMode, ShadowCasters);

    ShadowRenderer->EndShadowPass();
    return true;
}

void FRenderer::RenderDirectionalShadowMap(const FScene& Scene, const FMatrix& CameraViewProjection, const FVector& CameraLocation, float CameraNear, float CameraFar, bool bOrthographic, const EViewMode ViewMode, const FMatrix& CameraWorld, float TanHalfFovX, float TanHalfFovY)
{
    if (LightData.DirectionalLight.ShadowParameters.W <= 0.0f || LightData.DirectionalLight.ColorIntensity.W <= 0.0f)
        return;

    FTexture2D* ShadowMap = ShadowRenderer ? ShadowRenderer->GetDirectionalShadowMap() : nullptr;
    if (!ShadowMap || !ShadowMap->GetDSV())
        return;

    // 같은 텍스처를 SRV와 DSV로 동시에 바인딩하지 않도록 해제
    RenderCommand::UnbindShaderResource(10, EShaderBindFlagBits::Pixel | EShaderBindFlagBits::Vertex);

    FMatrix LightVP = LightData.DirectionalLight.ViewProjection;
    TArray<FPrimitiveSceneProxy*> ShadowCasters;

    if (!bOrthographic)
    {
        const FVector4& Direction = LightData.DirectionalLight.Direction;
        const FVector LightDirection(Direction.X, Direction.Y, Direction.Z);
        const float ShadowDistance = std::min(CameraFar, 40.0f);

        //LightVP = ShadowRenderer->BuildDirectionalLightVP(CameraViewProjection, CameraNear, CameraFar, ShadowDistance, LightDirection, ShadowMap->GetWidth());

        // CameraNear ~ ShadowDistance 구간의 코너를 직접 생성한다.
        std::array<FVector, 8> ShadowCorners{};

        for (uint32 Plane = 0; Plane < 2; ++Plane)
        {
            const float Depth = Plane == 0 ? CameraNear : ShadowDistance;

            for (uint32 Corner = 0; Corner < 4; ++Corner)
            {
                const float YSign = (Corner & 1u) ? 1.0f : -1.0f;
                const float ZSign = (Corner & 2u) ? 1.0f : -1.0f;

                const FVector LocalCorner(
                    Depth,
                    YSign * Depth * TanHalfFovX,
                    ZSign * Depth * TanHalfFovY);

                ShadowCorners[Plane * 4 + Corner] =
                    CameraWorld.TransformPosition(LocalCorner);
            }
        }
        
        // 카메라 위치·회전과 관계없이 일정한 반경을 사용한다.
        const float HalfDepth = (ShadowDistance - CameraNear) * 0.5f;
        const float FarHalfWidth = ShadowDistance * TanHalfFovX;
        const float FarHalfHeight = ShadowDistance * TanHalfFovY;

        const float StableRadius = std::sqrt(HalfDepth * HalfDepth + FarHalfWidth * FarHalfWidth + FarHalfHeight * FarHalfHeight);

        // BVH 컬링에 사용할 행렬.
        LightVP = ShadowRenderer->BuildDirectionalLightVPFromCorners(ShadowCorners, LightDirection, ShadowMap->GetWidth(), std::numeric_limits<float>::infinity(), StableRadius);

        GatherShadowCasters(Scene, LightVP,FrustumSidePlanesMask, ShadowCasters);
        // 그림자를 받을 영역의 Light Space 깊이 범위를 구한다.
        const FMatrix LightView =  ShadowRenderer->BuildDirectionalLightView(ShadowCorners, LightDirection);
        const FBox ReceiverBounds = ShadowRenderer->CalculateLightSpaceBounds(ShadowCorners, LightView);

        float MinCasterDepth = ReceiverBounds.Min.X;

        // 검색된 Caster 중 광원에 가장 가까운 깊이를 찾는다.
        for (const FPrimitiveSceneProxy *Proxy : ShadowCasters)
        {
            const FAABB &Bounds = Proxy->GetBounds();
            const FBox WorldBounds{ Bounds.Center - Bounds.Extent, Bounds.Center + Bounds.Extent };
            const FBox LightBounds = WorldBounds.GetWorldAABB(LightView);

            MinCasterDepth = std::min(MinCasterDepth, LightBounds.Min.X);
        }

        //LightVP = ShadowRenderer->BuildDirectionalLightVPFromCorners(ShadowCorners, LightDirection, ShadowMap->GetWidth(), MinCasterDepth);
        LightVP = ShadowRenderer->BuildDirectionalLightVPFromCorners(ShadowCorners, LightDirection, ShadowMap->GetWidth(), MinCasterDepth, StableRadius);
    }

    FShadowPassDesc Desc{};
    Desc.DepthView = ShadowMap->GetDSV();
    Desc.Width = ShadowMap->GetWidth();
    Desc.Height = ShadowMap->GetHeight();

    // Perspective 뷰에서 Caster가 없으면 Depth만 초기화한다.
    if (!bOrthographic && ShadowCasters.IsEmpty())
    {
        FScopedShadowGPUTime GPUTime(ShadowGPUProfiler);
        RenderCommand::GetContext()->ClearDepthStencilView(Desc.DepthView, D3D11_CLEAR_DEPTH, 1.0f, 0);
        ++ShadowStats::Pending.DepthClears;
    }
    else if (!RenderShadowDepth(Scene, Desc, LightVP, ViewMode, bOrthographic ? nullptr : &ShadowCasters))
    {
        return;
    }
    else
    {
        ++ShadowStats::Pending.DirectionalPasses;
    }
    ++ShadowStats::Pending.UpdatedLights[0];

    // Shadow Map 생성과 샘플링에 동일한 Light VP를 사용한다.
    UploadLightGlobalConstants(LightVP);

    // Shadow Pass에서 변경한 ViewCB를 메인 카메라 기준으로 복원
    FViewConstants CameraViewConstants{};
    CameraViewConstants.VP = CameraViewProjection;
    CameraViewConstants.CameraPosition = CameraLocation;
    RenderCommand::UpdateBufferData(ViewCB.get(), &CameraViewConstants, sizeof(FViewConstants));
}

void FRenderer::RenderSpotShadowMap(const FScene& Scene, const FMatrix &CameraViewProjection, const FVector &CameraLocation, const EViewMode ViewMode)
{
    //// TODO : Atlas 도입시에 SpotLightCount > 1일 경우도 처리해야 한다.
    //// 그림자를 만드는 Spot은 0번 하나뿐이다
    //if (LightData.SpotLightCount == 0 || LightData.SpotLights[0].ShadowParameters.W <= 0.0f)
    //    return;

    //FTexture2D *ShadowMap = ShadowRenderer ? ShadowRenderer->GetSpotShadowMap() : nullptr;
    //if (!ShadowMap || !ShadowMap->GetDSV())
    //    return;

    //// 같은 텍스처를 SRV와 DSV로 동시에 바인딩하지 않도록 해제 (Gouraud는 VS에서도 읽는다)
    //RenderCommand::UnbindShaderResource(11, EShaderBindFlagBits::Pixel | EShaderBindFlagBits::Vertex);

    //const FMatrix &LightVP = LightData.SpotLights[0].ViewProjection;

    //FShadowPassDesc Desc{};
    //Desc.DepthView = ShadowMap->GetDSV();
    //Desc.Width = ShadowMap->GetWidth();
    //Desc.Height = ShadowMap->GetHeight();

    //if (!RenderShadowDepth(Scene, Desc, LightVP, ViewMode))
    //    return;

    //// Shadow Pass에서 변경한 ViewCB를 메인 카메라 기준으로 복원
    //FViewConstants CameraViewConstants{};
    //CameraViewConstants.VP = CameraViewProjection;
    //CameraViewConstants.CameraPosition = CameraLocation;
    //RenderCommand::UpdateBufferData(ViewCB.get(), &CameraViewConstants, sizeof(FViewConstants));
}

void FRenderer::RenderPointShadowMap(const FScene &Scene, const FMatrix &CameraViewProjection, const FVector &CameraLocation, const EViewMode ViewMode)
{
    // TODO : PointLightCount > 1일 경우도 처리해야 한다.
    //if (LightData.PointLightCount == 0 || LightData.PointLights[0].ShadowParameters.W <= 0.0f)
    //{
    //    return;
    //}

    //FTextureCube *ShadowMap = ShadowRenderer ? ShadowRenderer->GetPointLightShadowMap() : nullptr;
    //if (!ShadowMap || !ShadowMap->GetFaceDSV(0))
    //{
    //    return;
    //}

    //RenderCommand::UnbindShaderResource(12, EShaderBindFlagBits::Pixel | EShaderBindFlagBits::Vertex);

    //const FVector LightPosition = FVector(LightData.PointLights[0].PositionRadius.X, LightData.PointLights[0].PositionRadius.Y, LightData.PointLights[0].PositionRadius.Z);
    //const float Far = LightData.PointLights[0].PositionRadius.W;
    //const float Near = 0.1f;

    //for (uint32 face = 0; face < 6; face++)
    //{
    //    FShadowPassDesc Desc{};
    //    Desc.DepthView = ShadowMap->GetFaceDSV(face);
    //    Desc.Width = ShadowMap->GetWidth();
    //    Desc.Height = ShadowMap->GetHeight();

    //    const FMatrix FaceLightVP = ShadowRenderer->BuildPointLightVP(LightPosition, Near, Far, face);

    //    RenderShadowDepth(Scene, Desc, FaceLightVP, ViewMode);
    //}

    //// Shadow Pass에서 변경한 ViewCB를 메인 카메라 기준으로 복원
    //FViewConstants CameraViewConstants{};
    //CameraViewConstants.VP = CameraViewProjection;
    //CameraViewConstants.CameraPosition = CameraLocation;
    //RenderCommand::UpdateBufferData(ViewCB.get(), &CameraViewConstants, sizeof(FViewConstants));
}

void FRenderer::BeginShadowStatsFrame()
{
    ShadowStats::BeginFrame();
    if (ShadowStatsResetGeneration != FStatOverlay::GetResetGeneration())
    {
        ShadowGPUProfiler.ResetStats();
        ShadowStatsResetGeneration = FStatOverlay::GetResetGeneration();
    }
    ShadowGPUProfiler.BeginFrame(FStatOverlay::IsEnabled(EStatFlags::Shadow));
}

void FRenderer::RecordShadowSceneStats(const FScene& Scene)
{
    if (!FStatOverlay::IsEnabled(EStatFlags::Shadow))
        return;
    for (ULightComponentBase* Light : Scene.LightComponents)
    {
        if (!Light || !Light->GetCastShadows() || Light->GetIntensity() <= 0.0f)
            continue;
        if (Cast<USpotLightComponent>(Light))
            ShadowStats::RecordLight(Light, 2);
        else if (Cast<UPointLightComponent>(Light))
            ShadowStats::RecordLight(Light, 1);
        else if (Cast<UDirectionalLightComponent>(Light))
            ShadowStats::RecordLight(Light, 0);
    }
}

void FRenderer::RecordShadowViewStats(bool bShadowView)
{
    auto& Stats = ShadowStats::Pending;
    ++Stats.Views;
    Stats.ShadowViews += bShadowView ? 1u : 0u;

    // 이미 수집된 Light Buffer만 읽는다. Scene 순회나 Caster 재수집은 하지 않는다.
    Stats.EligibleLights[0] += LightData.DirectionalLight.ShadowParameters.W > 0.0f &&
        LightData.DirectionalLight.ColorIntensity.W > 0.0f ? 1u : 0u;
    for (uint32 Index = 0; Index < LightData.PointLightCount; ++Index)
    {
        Stats.EligibleLights[1] += LightData.PointLights[Index].ShadowParameters.W > 0.0f &&
            LightData.PointLights[Index].ColorIntensity.W > 0.0f ? 1u : 0u;
    }
    for (uint32 Index = 0; Index < LightData.SpotLightCount; ++Index)
    {
        Stats.EligibleLights[2] += LightData.SpotLights[Index].ShadowParameters.W > 0.0f &&
            LightData.SpotLights[Index].ColorIntensity.W > 0.0f ? 1u : 0u;
    }

    if (!bShadowView)
    {
        Stats.SkippedLights[0] += LightData.DirectionalLight.ShadowParameters.W > 0.0f &&
            LightData.DirectionalLight.ColorIntensity.W > 0.0f ? 1u : 0u;
        for (uint32 Index = 0; Index < LightData.PointLightCount; ++Index)
            Stats.SkippedLights[1] += LightData.PointLights[Index].ShadowParameters.W > 0.0f &&
                LightData.PointLights[Index].ColorIntensity.W > 0.0f ? 1u : 0u;
        for (uint32 Index = 0; Index < LightData.SpotLightCount; ++Index)
            Stats.SkippedLights[2] += LightData.SpotLights[Index].ShadowParameters.W > 0.0f &&
                LightData.SpotLights[Index].ColorIntensity.W > 0.0f ? 1u : 0u;
    }

    if (bShadowView && IsCSMActive())
    {
        Stats.ActiveCascades = std::max(Stats.ActiveCascades, CascadeConstants.CascadeCount);
        ++Stats.CSMViews;
    }
}

void FRenderer::CompleteShadowStatsFrame()
{
    // Reset 버튼은 UI 프레임 도중에도 눌릴 수 있다. 현재 GPU 표본도 게시 전에 폐기한다.
    if (ShadowStatsResetGeneration != FStatOverlay::GetResetGeneration())
    {
        ShadowGPUProfiler.ResetStats();
        ShadowStatsResetGeneration = FStatOverlay::GetResetGeneration();
    }
    ShadowGPUProfiler.EndFrame();
    auto& Stats = ShadowStats::Pending;
    const auto& GPUTime = ShadowGPUProfiler.GetStats();
    Stats.bGPUTimeAvailable = GPUTime.bAvailable;
    Stats.GPULastMs = GPUTime.LastMs;
    Stats.GPUAverageMs = GPUTime.AverageMs;
    Stats.GPUMaxMs = GPUTime.MaxMs;
    Stats.ConfiguredCascades = CSMSettings.CascadeCount;
    Stats.bCSMEnabled = bCSMEnabled;
    Stats.CascadeResolution = CascadeShadowMaps.GetResolution();
    Stats.ShadowDistance = CSMShadowDistance;

    // 텍스처 소유자마다 한 번만 합산한다. SRV/DSV/배열 Slice View는 더하지 않는다.
    // 현재 Shadow 리소스는 비압축 포맷이며 실제 Desc의 mip/array/sample 크기를 반영한다.
    const auto EstimateTextureBytes = [](ID3D11Resource* Resource) -> uint64
    {
        if (!Resource)
            return 0;
        ComPtr<ID3D11Texture2D> Texture;
        if (FAILED(Resource->QueryInterface(IID_PPV_ARGS(Texture.GetAddressOf()))))
            return 0;
        D3D11_TEXTURE2D_DESC Desc{};
        Texture->GetDesc(&Desc);
        uint64 Texels = 0;
        uint32 Width = Desc.Width;
        uint32 Height = Desc.Height;
        for (uint32 Mip = 0; Mip < Desc.MipLevels; ++Mip)
        {
            Texels += static_cast<uint64>(Width) * Height;
            Width = std::max(Width / 2, 1u);
            Height = std::max(Height / 2, 1u);
        }
        return Texels * Desc.ArraySize * Desc.SampleDesc.Count * FormatToBytes(Desc.Format);
    };

    if (ShadowRenderer)
    {
        const FTexture2D* Directional = ShadowRenderer->GetDirectionalShadowMap();
        const FTexture2D* Atlas = ShadowRenderer->GetShadowMapAtlas();
        const FTexture2D* Spot = ShadowRenderer->GetSpotShadowMap();
        const FTextureCube* Point = ShadowRenderer->GetPointLightShadowMap();
        Stats.DirectionalResolution = Directional ? Directional->GetWidth() : 0;
        Stats.AtlasResolution = Atlas ? Atlas->GetWidth() : 0;
        const auto& Allocator = ShadowRenderer->GetShadowAtlasAllocator();
        Stats.AtlasTiles = Allocator.GetAllocatedTileCount();
        Stats.AtlasAllocatedTexels = Allocator.GetAllocatedTexels();
        Stats.EstimatedBytes += EstimateTextureBytes(Directional ? Directional->GetRawPtr() : nullptr);
        Stats.EstimatedBytes += EstimateTextureBytes(Atlas ? Atlas->GetRawPtr() : nullptr);
        // Atlas 도입 후 사용하지 않더라도 아직 할당되어 있는 전용 맵도 포함한다.
        Stats.EstimatedBytes += EstimateTextureBytes(Spot ? Spot->GetRawPtr() : nullptr);
        Stats.EstimatedBytes += EstimateTextureBytes(Point ? Point->GetRawPtr() : nullptr);
        Stats.bResourcesAvailable = true;
    }
    Stats.EstimatedBytes += EstimateTextureBytes(CascadeShadowMaps.GetTexture());
    for (const auto& Preview : CascadePreviewTextures)
    {
        Stats.PreviewBytes += EstimateTextureBytes(Preview ? Preview->GetRawPtr() : nullptr);
    }
    Stats.EstimatedBytes += Stats.PreviewBytes;
    ShadowStats::EndFrame();
}

void FRenderer::RenderShadowMap(const FScene& Scene, const FMatrix& CameraViewProjection, const FVector& CameraLocation, const float TanHalfFovY, const float ViewportHeight, const EViewMode ViewMode, bool bOrthographic)
{
    // Shadow Pass가 필요하지 않아도 일반 조명 데이터는 갱신한다.
    if (!bShadowsEnabled)
    {
        ::UploadDynamicBuffer(SpotLightBuffer.Get(), LightData.SpotLights, sizeof(FSpotLight) * LightData.SpotLightCount);
        ::UploadDynamicBuffer(PointLightBuffer.Get(), LightData.PointLights, sizeof(FPointLight) * LightData.PointLightCount);
        return;
    }

    const FFrustumPlanes CameraFrustum = ExtractFrustumPlanes(CameraViewProjection);

    const float CotanHalfFOV = 1.0f / TanHalfFovY;
    constexpr float MinShadowDiameterPixels = 100.0f;

    // 광원 영향 구체의 화면상 크기가 너무 작으면 그림자 생성을 생략한다.
    const auto IsShadowTooSmall = [&](const FVector4& PositionRadius)
        {
            if (bOrthographic)
                return false;

            const FVector LightPosition{ PositionRadius.X, PositionRadius.Y, PositionRadius.Z };
            const float Distance = (LightPosition - CameraLocation).Length();

            // 카메라가 광원 범위 안에 있으면 생략하지 않는다.
            if (Distance <= PositionRadius.W)
                return false;

            const float NearestDistance = Distance - PositionRadius.W;
            const float ProjectedDiameter = PositionRadius.W * ViewportHeight * CotanHalfFOV / NearestDistance;

            return ProjectedDiameter <= MinShadowDiameterPixels;
        };

    FTexture2D *ShadowMap = ShadowRenderer ? ShadowRenderer->GetShadowMapAtlas() : nullptr;
    if (!ShadowMap || !ShadowMap->GetDSV())
    {
        return;
    }

    RenderCommand::UnbindShaderResource(14, EShaderBindFlagBits::Pixel | EShaderBindFlagBits::Vertex); // ShadowMapAtlasBuffer

    // Invalid Cache
    for(uint32 i = LightData.SpotLightCount; i < NUM_SPOT_LIGHT; i++)
    {
        InvalidSpotCache(i);
    }
    for(uint32 i = LightData.PointLightCount; i < NUM_POINT_LIGHT; i++)
    {
        InvalidPointCache(i);
    }

    // 카메라 시야에 영향을 주는 그림자 광원이 없으면 Shadow Atlas Pass 전체를 생략한다.
    bool bHasVisibleShadowCastingLight = false;

    for (uint32 i = 0; i < LightData.PointLightCount; ++i)
    {
        FPointLight& PointLightData = LightData.PointLights[i];

        if (PointLightData.ShadowParameters.W <= 0.0f || PointLightData.ColorIntensity.W <= 0.0f)
        {
            for (uint32 Face = 0; Face < 6; ++Face)
                PointLightData.AtlasScaleOffset[Face] = { 0.0f, 0.0f, 0.0f, 0.0f };

            continue;
        }

        const FVector4& PositionRadius = PointLightData.PositionRadius;
        if (IsSphereInFrustum(FVector(PositionRadius.X, PositionRadius.Y, PositionRadius.Z), PositionRadius.W, CameraFrustum) && !IsShadowTooSmall(PositionRadius))
        {
            bHasVisibleShadowCastingLight = true;
        }
        else 
        {
            for(uint32 Face = 0; Face < 6; ++Face)
            {
                PointLightData.AtlasScaleOffset[Face] = { 0.0f, 0.0f, 0.0f, 0.0f };
            }
            InvalidPointCache(i);
        }
    }

    for (uint32 i = 0; i < LightData.SpotLightCount; ++i)
    {
        FSpotLight& SpotLightData = LightData.SpotLights[i];

        if (SpotLightData.ShadowParameters.W <= 0.0f || SpotLightData.ColorIntensity.W <= 0.0f)
        {
            for (uint32 Face = 0; Face < 6; ++Face)
                SpotLightData.AtlasScaleOffset = { 0.0f, 0.0f, 0.0f, 0.0f };

            continue;
        }

        const FVector4& PositionRadius = SpotLightData.PositionRadius;
        if (IsSphereInFrustum(FVector(PositionRadius.X, PositionRadius.Y, PositionRadius.Z), PositionRadius.W, CameraFrustum) && !IsShadowTooSmall(PositionRadius))
        {
            bHasVisibleShadowCastingLight = true;
        }
        else
        {
            SpotLightData.AtlasScaleOffset = { 0.0f, 0.0f, 0.0f, 0.0f };
            InvalidSpotCache(i);
        }
    }

    if (!bHasVisibleShadowCastingLight)
    {
        for (uint32 Index = 0; Index < LightData.PointLightCount; ++Index)
            ShadowStats::Pending.SkippedLights[1] += LightData.PointLights[Index].ShadowParameters.W > 0.0f &&
                LightData.PointLights[Index].ColorIntensity.W > 0.0f ? 1u : 0u;
        for (uint32 Index = 0; Index < LightData.SpotLightCount; ++Index)
            ShadowStats::Pending.SkippedLights[2] += LightData.SpotLights[Index].ShadowParameters.W > 0.0f &&
                LightData.SpotLights[Index].ColorIntensity.W > 0.0f ? 1u : 0u;
        // Shadow Pass는 생략하더라도 일반 조명 계산에 필요한 GPU 데이터는 갱신한다.
        ::UploadDynamicBuffer(SpotLightBuffer.Get(), LightData.SpotLights, sizeof(FSpotLight) * LightData.SpotLightCount);
        ::UploadDynamicBuffer(PointLightBuffer.Get(), LightData.PointLights, sizeof(FPointLight) * LightData.PointLightCount);
        Scene.ShadowDirtyBounds.Reset();
        return;
    }

    FShadowPassDesc Desc{};
    Desc.DepthView = ShadowMap->GetDSV();
    Desc.Width = ShadowMap->GetWidth();
    Desc.Height = ShadowMap->GetHeight();
    Desc.bClearDepth = !bShadowAtlasInitialCleared;
    bShadowAtlasInitialCleared = true;

    if (Desc.bClearDepth)
    {
        FScopedShadowGPUTime GPUTime(ShadowGPUProfiler);
        ShadowRenderer->BeginShadowPass(Desc);
    }
    else
    {
        ShadowRenderer->BeginShadowPass(Desc);
    }

    float AtlasSize = ShadowMap->GetWidth();

    // Point Light Loop
    uint32 PointLightCount = LightData.PointLightCount;

    for(uint32 i = 0; i < PointLightCount; i++)
    {
        // 광원의 영향 구체가 카메라 시야 밖이면 Shadow Atlas 할당을 생략한다.
        const FVector4& PositionRadius = LightData.PointLights[i].PositionRadius;
        const FVector LightPosition = { PositionRadius.X, PositionRadius.Y, PositionRadius.Z };

        uint32 RequestSize = ShadowRenderer->GetShadowAtlasAllocator().GetMinTileSize();
        CalculateShadowMapLOD(LightData.PointLights[i].PositionRadius, CameraLocation, ViewportHeight, CotanHalfFOV, RequestSize);
        RequestSize = RequestSize >> 1; // PointLight Down scaling

        FPointShadowCache& Cache = PointShadowCaches[i];

        if(LightData.PointLights[i].ShadowParameters.W == 0.0f || !IsSphereInFrustum(LightPosition, PositionRadius.W, CameraFrustum) || RequestSize == 0)
        {
            ShadowStats::Pending.SkippedLights[1] += LightData.PointLights[i].ShadowParameters.W > 0.0f &&
                LightData.PointLights[i].ColorIntensity.W > 0.0f ? 1u : 0u;
            for(uint32 face = 0; face < 6; face++)
            {
                LightData.PointLights[i].AtlasScaleOffset[face] = { 0.0f, 0.0f, 0.0f, 0.0f };
            }
            InvalidPointCache(i);
            continue;
        }

        const float Far = LightData.PointLights[i].PositionRadius.W;
        const float Near = LightData.PointLights[i].ShadowFilterParameters.W;

        if(!Cache.bIsDirty)
        {
            if(!Cache.bIsValid || PositionRadius != Cache.PrevPositionRadius)
            {
                Cache.bIsDirty = true;
            }
            else if(RequestSize > Cache.CachedSize || RequestSize <= (Cache.CachedSize >> 2))
            {
                Cache.bIsDirty = true;
            }
            else
            {
                for(const FAABB& DirtyBox : Scene.ShadowDirtyBounds)
                {
                    if(::GetIntersectedAABBWithLight(LightPosition, PositionRadius.W, DirtyBox))
                    {
                        Cache.bIsDirty = true;
                        break;
                    }
                }
            }
        }

        FShadowAtlasTile ShadowAtlasTile;
        bool bHasChangedRequestSize = false;
        uint32 AllocatedFaceSize = 0;
        bool bUpdatedPointMap = false;

        for(uint32 face = 0; face < 6; face++)
        {
            // Point Light는 타일 전체에 렌더하고 투영 함수 내부에서 border를 반영한다.
            FMatrix FaceVP = ShadowRenderer->BuildPointLightVP(LightPosition, Near, Far, RequestSize, face);

            // Cache Hit
            if(Cache.bIsValid && !Cache.bIsDirty && RequestSize <= Cache.CachedSize)
            {
                LightData.PointLights[i].AtlasScaleOffset[face] = Cache.CachedScaleOffsets[face];
                LightData.PointLights[i].ViewProjection[face] = Cache.PrevViewProjections[face];
                LightData.PointLights[i].ShadowParameters.Z = 1.0f / AtlasSize;
                continue;
            }

            TArray<FPrimitiveSceneProxy*> ShadowCasters;
            GatherShadowCasters(Scene, FaceVP, FrustumAllPlanesMask, ShadowCasters);

            // 해당 방향에 Caster가 없다면 Atlas 타일을 할당하지 않는다.
            if(ShadowCasters.IsEmpty())
            {
                if(Cache.CachedTiles[face].NodeIndex != -1)
                {
                    ShadowRenderer->GetShadowAtlasAllocator().Free(Cache.CachedTiles[face].NodeIndex);
                    Cache.CachedTiles[face].NodeIndex = -1;
                }
                Cache.CachedScaleOffsets[face] = { 0.0f, 0.0f, 0.0f, 0.0f };
                Cache.PrevViewProjections[face] = FaceVP;
                LightData.PointLights[i].AtlasScaleOffset[face] = { 0.0f, 0.0f, 0.0f, 0.0f };
                continue;
            }

            bool bTileReady = false;

            // Reuse Tile
            if(Cache.bIsValid && Cache.CachedTiles[face].NodeIndex != -1 && RequestSize <= Cache.CachedTiles[face].Size && RequestSize > (Cache.CachedTiles[face].Size >> 2))
            {
                ShadowAtlasTile = Cache.CachedTiles[face];
                bTileReady = true;
            }
            else
            {
                if(Cache.CachedTiles[face].NodeIndex != -1)
                {
                    ShadowRenderer->GetShadowAtlasAllocator().Free(Cache.CachedTiles[face].NodeIndex);
                    Cache.CachedTiles[face].NodeIndex = -1;
                }

                while(true)
                {                
                    bool bHasAllocated = ShadowRenderer->GetShadowAtlasAllocator().Allocate(RequestSize, ShadowAtlasTile);

                    if(bHasAllocated)
                    {
                        bTileReady = true;
                        break;
                    }
                    else
                    {
                        if(RequestSize / 2 >= ShadowRenderer->GetShadowAtlasAllocator().GetMinTileSize())
                        {
                            RequestSize /= 2;
                            continue;
                        }

                        bHasChangedRequestSize = true;
                        break;
                    }
                }
            }

            if(bTileReady)
            {
                uint32 InnerX = ShadowAtlasTile.X;
                uint32 InnerY = ShadowAtlasTile.Y;
                uint32 InnerSize = ShadowAtlasTile.Size;
                AllocatedFaceSize = InnerSize;

                {
                    FScopedShadowGPUTime GPUTime(ShadowGPUProfiler);
                    ShadowRenderer->ClearShadowTile(ShadowAtlasTile);
                }

                ShadowRenderer->SetShadowViewPort(InnerX, InnerY, InnerSize, InnerSize);

                if (InnerSize != RequestSize)
                {
                    FaceVP = ShadowRenderer->BuildPointLightVP(LightPosition, Near, Far, InnerSize, face);
                    ShadowCasters.Reset();
                    GatherShadowCasters(Scene, FaceVP, FrustumAllPlanesMask, ShadowCasters);
                }

                LightData.PointLights[i].ViewProjection[face] = FaceVP;
                LightData.PointLights[i].AtlasScaleOffset[face] = { (float)InnerSize / AtlasSize, (float)InnerSize / AtlasSize, (float)InnerX / AtlasSize, (float)InnerY / AtlasSize }; //UV
                LightData.PointLights[i].ShadowParameters.Z = 1.0f / AtlasSize;

                {
                    FScopedShadowGPUTime GPUTime(ShadowGPUProfiler);
                    DrawShadowCasters(Scene, FaceVP, ViewMode, &ShadowCasters);
                }
                ++ShadowStats::Pending.Passes;
                ++ShadowStats::Pending.PointFaces;
                bUpdatedPointMap = true;

                Cache.CachedScaleOffsets[face] = LightData.PointLights[i].AtlasScaleOffset[face];
                Cache.CachedTiles[face] = ShadowAtlasTile;
                Cache.PrevViewProjections[face] = FaceVP;
            }
            else
            {
                LightData.PointLights[i].AtlasScaleOffset[face] = { 0.0f, 0.0f, 0.0f, 0.0f };
                Cache.CachedTiles[face].NodeIndex = -1;
            }
        }

        if (LightData.PointLights[i].ColorIntensity.W > 0.0f)
        {
            if (bUpdatedPointMap)
                ++ShadowStats::Pending.UpdatedLights[1];
            else
                ++ShadowStats::Pending.SkippedLights[1];
        }

        Cache.bIsValid = true;
        Cache.bIsDirty = false;
        Cache.CachedSize = (AllocatedFaceSize > 0) ? AllocatedFaceSize : RequestSize;
        Cache.PrevPositionRadius = PositionRadius;

        if(bHasChangedRequestSize)
        {
            for(uint32 face = 0; face < 6; face++)
            {
                LightData.PointLights[i].AtlasScaleOffset[face] = { 0.0f, 0.0f, 0.0f, 0.0f };
                if(Cache.CachedTiles[face].NodeIndex != -1)
                {
                    ShadowRenderer->GetShadowAtlasAllocator().Free(Cache.CachedTiles[face].NodeIndex);
                    Cache.CachedTiles[face].NodeIndex = -1;
                }
            }
            Cache.bIsValid = false;
        }
    }
    // SpotLight Loop
    uint32 SpotLightCount = LightData.SpotLightCount;
    for(uint32 i = 0; i < SpotLightCount; i++)
    {
        FSpotShadowCache& Cache = SpotShadowCaches[i];

        // Spot Light의 영향 범위를 구체로 보수적으로 검사한다.
        const FVector4& PositionRadius = LightData.SpotLights[i].PositionRadius;
        const FVector LightPosition = { PositionRadius.X, PositionRadius.Y, PositionRadius.Z };

        uint32 RequestSize;
        CalculateShadowMapLOD(LightData.SpotLights[i].PositionRadius, CameraLocation, ViewportHeight, CotanHalfFOV, RequestSize);

        if(LightData.SpotLights[i].ShadowParameters.W == 0.0f || !IsSphereInFrustum(LightPosition, PositionRadius.W, CameraFrustum) || RequestSize == 0)
        {
            ShadowStats::Pending.SkippedLights[2] += LightData.SpotLights[i].ShadowParameters.W > 0.0f &&
                LightData.SpotLights[i].ColorIntensity.W > 0.0f ? 1u : 0u;
            LightData.SpotLights[i].AtlasScaleOffset = { 0.0f, 0.0f, 0.0f, 0.0f };
            InvalidSpotCache(i);
            continue;
        }
        const float Far = LightData.SpotLights[i].PositionRadius.W;
        const float Near = 0.1f;

        const FMatrix& LightVP = LightData.SpotLights[i].ViewProjection;

        FShadowAtlasTile ShadowAtlasTile;

        if(!Cache.bIsDirty)
        {
            if(!Cache.bIsValid || PositionRadius != Cache.PrevPositionRadius || LightVP != Cache.PrevViewProjection)
            {
                Cache.bIsDirty = true;
            }
            else if(RequestSize > Cache.CachedSize || RequestSize <= (Cache.CachedSize >> 2))
            {
                Cache.bIsDirty = true;
            }
            else
            {
                for(const FAABB& DirtyBox : Scene.ShadowDirtyBounds)
                {
                    if(::GetIntersectedAABBWithLight(LightPosition, PositionRadius.W, DirtyBox))
                    {
                        Cache.bIsDirty = true;
                        break;
                    }
                }
            }
        }

        // Cache Hit
        if(Cache.bIsValid && !Cache.bIsDirty && RequestSize <= Cache.CachedSize)
        {
            ShadowStats::Pending.SkippedLights[2] += LightData.SpotLights[i].ColorIntensity.W > 0.0f ? 1u : 0u;
            LightData.SpotLights[i].AtlasScaleOffset = Cache.CachedScaleOffset;
            LightData.SpotLights[i].ShadowParameters.Z = 1.0f / AtlasSize;
            continue;
        }

        // Cache Miss
        TArray<FPrimitiveSceneProxy*> ShadowCasters;
        GatherShadowCasters(Scene, LightVP, FrustumAllPlanesMask, ShadowCasters);

        // Caster가 없으면 Atlas 타일을 할당하지 않고 다음 Spot Light로 넘어간다.
        if (ShadowCasters.IsEmpty())
        {
            ShadowStats::Pending.SkippedLights[2] += LightData.SpotLights[i].ColorIntensity.W > 0.0f ? 1u : 0u;
            ShadowRenderer->GetShadowAtlasAllocator().Free(Cache.CachedTile.NodeIndex);
            Cache.CachedTile.NodeIndex = -1;
            Cache.bIsValid = true;
            Cache.bIsDirty = false;
            Cache.CachedSize = RequestSize;
            Cache.CachedScaleOffset = { 0.0f, 0.0f, 0.0f, 0.0f };
            Cache.PrevPositionRadius = PositionRadius;
            Cache.PrevViewProjection = LightVP;            
            LightData.SpotLights[i].AtlasScaleOffset = { 0.0f, 0.0f, 0.0f, 0.0f };
            continue;
        }
        else
        {
            bool bTileReady = false;

            // Reuse Tile
            if(Cache.bIsValid && Cache.CachedTile.NodeIndex != -1 && RequestSize <= Cache.CachedSize && RequestSize > (Cache.CachedSize >> 2))
            {
                ShadowAtlasTile = Cache.CachedTile;
                bTileReady = true;
            }
            else
            {
                if(Cache.CachedTile.NodeIndex != -1)
                {
                    ShadowRenderer->GetShadowAtlasAllocator().Free(Cache.CachedTile.NodeIndex);
                    Cache.CachedTile.NodeIndex = -1;
                }

                while(true)
                {
                    bool bHasAllocated = ShadowRenderer->GetShadowAtlasAllocator().Allocate(RequestSize, ShadowAtlasTile);

                    if(bHasAllocated)
                    {
                        bTileReady = true;

                        break;
                    }
                    else
                    {
                        if(RequestSize / 2 >= ShadowRenderer->GetShadowAtlasAllocator().GetMinTileSize())
                        {
                            RequestSize /= 2;
                            continue;
                        }
                        else
                        {
                            break;
                        }
                    }
                }
            }

            if(bTileReady)
            {
                uint32 InnerX = ShadowAtlasTile.X + SHADOW_ATLAS_BORDER;
                uint32 InnerY = ShadowAtlasTile.Y + SHADOW_ATLAS_BORDER;
                uint32 InnerSize = ShadowAtlasTile.Size - 2 * SHADOW_ATLAS_BORDER;

                {
                    FScopedShadowGPUTime GPUTime(ShadowGPUProfiler);
                    ShadowRenderer->ClearShadowTile(ShadowAtlasTile);
                }

                ShadowRenderer->SetShadowViewPort(InnerX, InnerY, InnerSize, InnerSize);

                LightData.SpotLights[i].AtlasScaleOffset = { (float)InnerSize / AtlasSize , (float)InnerSize / AtlasSize , (float)InnerX / AtlasSize, (float)InnerY / AtlasSize }; // UV
                LightData.SpotLights[i].ShadowParameters.Z = 1.0f / AtlasSize;

                {
                    FScopedShadowGPUTime GPUTime(ShadowGPUProfiler);
                    DrawShadowCasters(Scene, LightVP, ViewMode, &ShadowCasters);
                }
                ++ShadowStats::Pending.Passes;
                ++ShadowStats::Pending.SpotTiles;
                ShadowStats::Pending.UpdatedLights[2] += LightData.SpotLights[i].ColorIntensity.W > 0.0f ? 1u : 0u;

                Cache.bIsValid = true;
                Cache.bIsDirty = false;
                Cache.CachedSize = ShadowAtlasTile.Size;
                Cache.CachedTile = ShadowAtlasTile;
                Cache.CachedScaleOffset = LightData.SpotLights[i].AtlasScaleOffset;
                Cache.PrevPositionRadius = PositionRadius;
                Cache.PrevViewProjection = LightVP;
            }
            else
            {
                LightData.SpotLights[i].AtlasScaleOffset = { 0.0f,0.0f,0.0f,0.0f };
                Cache.CachedTile.NodeIndex = -1;
                Cache.bIsValid = false;
                ShadowStats::Pending.SkippedLights[2] += LightData.SpotLights[i].ColorIntensity.W > 0.0f ? 1u : 0u;
            }
        }

        //if(ShadowRenderer->GetShadowAtlasAllocator().Allocate(RequestSize, ShadowAtlasTile))
        //{
        //    uint32 InnerX = ShadowAtlasTile.X + SHADOW_ATLAS_BORDER;
        //    uint32 InnerY = ShadowAtlasTile.Y + SHADOW_ATLAS_BORDER;
        //    uint32 InnerSize = ShadowAtlasTile.Size - 2 * SHADOW_ATLAS_BORDER;

        //    ShadowRenderer->SetShadowViewPort(InnerX, InnerY, InnerSize, InnerSize);

        //    LightData.SpotLights[i].AtlasScaleOffset = { (float)InnerSize / AtlasSize , (float)InnerSize / AtlasSize , (float)InnerX / AtlasSize, (float)InnerY / AtlasSize }; // UV
        //    LightData.SpotLights[i].ShadowParameters.Z = 1.0f / AtlasSize;

        //    const FMatrix& LightVP = LightData.SpotLights[i].ViewProjection;

        //    TArray<FPrimitiveSceneProxy*> ShadowCasters;
        //    GatherShadowCasters(Scene, LightVP, FrustumAllPlanesMask, ShadowCasters);
        //    DrawShadowCasters(Scene, LightVP, ViewMode, &ShadowCasters);
        //}
        //else
        //{
        //    LightData.SpotLights[i].AtlasScaleOffset = { 0.0f,0.0f,0.0f,0.0f };
        //    LightData.SpotLights[i].ShadowParameters.W = 0.0f;
        //}
    }

    ShadowRenderer->EndShadowPass();    

    // Upload Light data to buffer
    ::UploadDynamicBuffer(SpotLightBuffer.Get(), LightData.SpotLights, sizeof(FSpotLight)* LightData.SpotLightCount);
    ::UploadDynamicBuffer(PointLightBuffer.Get(), LightData.PointLights, sizeof(FPointLight)* LightData.PointLightCount);

    // Shadow Pass에서 변경한 ViewCB를 메인 카메라 기준으로 복원
    FViewConstants CameraViewConstants{};
    CameraViewConstants.VP = CameraViewProjection;
    CameraViewConstants.CameraPosition = CameraLocation;
    RenderCommand::UpdateBufferData(ViewCB.get(), &CameraViewConstants, sizeof(FViewConstants));

    Scene.ShadowDirtyBounds.Reset();
}

void FRenderer::CalculateShadowMapLOD(const FVector4 InPositionRadius, const FVector& CameraLocation, const float InViewportHeight, const float CotanHalfFOV, uint32& OutRequestSize)
{
    uint32 ShadowQualityShift = static_cast<uint32>(ShadowResolutionQuality);
    OutRequestSize = SHADOW_LOD_LEVEL_4 >> ShadowQualityShift;

    FVector LightPosition = { InPositionRadius.X, InPositionRadius.Y, InPositionRadius.Z };
    float LightDistance = (LightPosition - CameraLocation).Length();
    float Radius = InPositionRadius.W;

    float Ratio = (Radius * InViewportHeight) / LightDistance * CotanHalfFOV;

    if(Ratio > SHADOW_LOD_PIXEL_1)
    {
        OutRequestSize = SHADOW_LOD_LEVEL_1 >> ShadowQualityShift;
    }
    else if(Ratio > SHADOW_LOD_PIXEL_2)
    {
        OutRequestSize = SHADOW_LOD_LEVEL_2 >> ShadowQualityShift;
    }
    else if(Ratio > SHADOW_LOD_PIXEL_3)
    {
        OutRequestSize = SHADOW_LOD_LEVEL_3 >> ShadowQualityShift;
    }
    else if(Ratio > SHADOW_LOD_PIXEL_4)
    {
        OutRequestSize = SHADOW_LOD_LEVEL_4 >> ShadowQualityShift;
    }
    else
    {
        OutRequestSize = 0;
    }
}

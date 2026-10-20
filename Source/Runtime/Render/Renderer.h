#pragma once

#include "RenderPacket.h"
#include "Texture2D.h"
#include "Text/Font.h"

#include "RenderingInfo.h"
#include "Render/ViewMode.h"
#include "Occlusion/GPUOcclusion.h"
#include "TileBasedCulling/TileBasedCulling.h"
#include "Shared/LightDefinitions.h"
#include "Shadow/ShadowRenderer.h"
#include "Shadow/CascadeShadowMaps.h"

constexpr uint32 ObjectSlotBytes = 256;

struct FViewConstants
{
	FMatrix VP;
	FVector CameraPosition;
	float   Padding = 0.0f;
};

struct FPerObjectConstants
{
	FMatrix World;
	FMatrix WorldInvTranspose;
	uint32 bReceiveShadow = 1;    
	uint32 Paddings[3] = {};
};

struct FSortEntry
{
	uint64 Key;
	uint32 PacketIndex;
};

class UCameraComponent;
class FScene;

// 오클루전 컬링의 효과 상한을 재기 위한 측정 결과 (디버그 전용)
struct FOcclusionMeasureResult
{
	bool bValid = false;
	uint32 TotalObjects = 0;      // 불투명 패스에 제출된 물체 수
	uint32 VisibleObjects = 0;    // 최종 깊이 버퍼에 픽셀을 1개 이상 남긴 물체 수
	uint32 TotalDraws = 0;
	uint32 VisibleDraws = 0;
	uint64 TotalTriangles = 0;
	uint64 VisibleTriangles = 0;
	double ElapsedMs = 0.0;       // 측정 자체에 걸린 시간 (GPU 대기 포함)
	// GPU 오클루전 검증. 판정만 하고 거르지 않은(Cull 끔) 상태에서 측정해야 의미가 있다.
	uint32 GPUOccludedDraws = 0;  // GPU가 가렸다고 판정한 드로우 수
	uint32 FalseCulls = 0;        // 그중 실제로는 픽셀이 보인 드로우 수. 0이어야 한다
};

struct alignas(16) FPointLight
{
	FVector4 PositionRadius = FVector4(0, 0, 0, 0); // xyz: 위치, w: 반경
	FVector4 ColorIntensity = FVector4(0, 0, 0, 0); // rgb: 색상, w: 강도
	FVector4 FalloffEnabled = FVector4(1, 0, 0, 0); // x: 반경 감쇠, y: 사용 여부, zw: 패딩
    FVector4 ShadowParameters; // x : Shadow Depth Bias, y : Shdaow Slope Scale, z : Inveerse Shadow Map Size, w : bCastShowShadow(1.0f : 0.0f)
	FVector4 ShadowFilterParameters; // x : ShadowSharpen ,y : TransitionScale, z : LightSourceRadius, w : nearPlane
    FMatrix ViewProjection[6];
    FVector4 AtlasScaleOffset[6]; // xy : scale, zw : offset
};
static_assert(sizeof(FPointLight) == 560, "FPointLight must be 544 bytes");

struct alignas(16) FSpotLight
{
	FVector4 PositionRadius = FVector4(0, 0, 0, 0); // xyz: 위치, w: 반경
	FVector4 ColorIntensity = FVector4(0, 0, 0, 0); // rgb: 색상, w: 강도
	FVector4 FalloffEnabled = FVector4(1, 0, 1, 0); // x: 반경 감쇠, y: 사용 여부, z : 콘 사이 감쇠, w: 패딩
	FVector4 OutAngleInvCosDiff = FVector4(0, 0, 0, 0);// x : cosOuter, y : 1 / (cosInner - cosOuter)
	FVector4 ForwardVector = FVector4(1, 0, 0, 0);		// xyz : 전방벡터
    FMatrix ViewProjection;                            // ViewProjection 행렬. 그림자 맵을 만들 때 사용
    FVector4 ShadowParameters; // x : Shadow Depth Bias, y : Shdaow Slope Scale, z : Inveerse Shadow Map Size, w : bCastShowShadow(1.0f : 0.0f)
	FVector4 ShadowFilterParameters; // x : ShadowSharpen ,y : TransitionScale, z : LightSourceRadius , w : nearPlane
    FVector4 AtlasScaleOffset; // xy : scale, zw : offset
};
static_assert(sizeof(FSpotLight) == 192, "FSpotLight must be 192 bytes");

struct alignas(16) FAmbientLight
{
	FVector4 ColorIntensity = FVector4(0, 0, 0, 0); // rgb: 색상, w: 강도
};

struct alignas(16) FDirectionalLight
{
	FVector4 ColorIntensity; // rgb : 색상, w: 강도
	FVector4 Direction;		 // xyz : 광선 방향(World)
	FMatrix ViewProjection;	 // ViewProjection 행렬. 그림자 맵을 만들 때 사용
	FVector4 ShadowParameters; // x : Shadow Depth Bias, y : Shdaow Slope Scale, z : Inveerse Shadow Map Size, w : bCastShowShadow(1.0f : 0.0f)
    FVector4 ShadowFilterParameters; // x : ShadowSharpen ,y : TransitionScale, z : tan(LightSourceAngle / 2), w : 패딩
};

static_assert(sizeof(FDirectionalLight) == 128, "FDirectionalLight must be 128 bytes");

// Scene이 채우는 CPU 쪽 라이트 묶음. 로컬 라이트 배열은 StructuredBuffer로, 나머지는 FLightGlobalConstants로 올라간다.
struct alignas(16) FLightConstants
{
	FPointLight			PointLights[NUM_POINT_LIGHT];
	FSpotLight			SpotLights[NUM_SPOT_LIGHT];
	FAmbientLight		AmbientLight;
	FDirectionalLight	DirectionalLight;
	uint32 PointLightCount = 0;
	uint32 SpotLightCount = 0;
	uint32 Padding = 0;
};

// UberLit의 LightConstants(b3)와 같은 레이아웃. 로컬 라이트 배열은 빠져 있다.
struct alignas(16) FLightGlobalConstants
{
	FAmbientLight		AmbientLight;
	FDirectionalLight	DirectionalLight;
	uint32 PointLightCount = 0;
	uint32 SpotLightCount = 0;
    uint32 bShowShadows = 1;
    uint32 ShadowQuality = 0;
    uint32 PCSSMaxKernel = 16;
};
static_assert(sizeof(FLightGlobalConstants) == 176, "FLightGlobalConstants must match UberLit LightConstants");

struct alignas(16) FCascadeDebugConstants
{
    FMatrix LightVP[4];

    FVector4 CameraForward = FVector4(1, 0, 0, 0);

    // 임시 경계값, 월드 단위. W는 사용하지 않는다.
    FVector4 SplitDepths = FVector4(5, 15, 30, 0);
    FVector4 BlendEndDepths;

    // 16바이트
    float ShadowDistance = 0.0f;
    float InvResolution = 0.0f;
    uint32 CascadeCount = 0;
    uint32 CSMEnabled = 0;
    // 16바이트
    uint32 DebugColorsEnabled = 0;
    uint32 BlendEnabled = 0;
    uint32 Padding[2] = {};
};

static_assert(sizeof(FCascadeDebugConstants) == 336, "FCascadeDebugConstants Size Error");

struct FCSMSettings
{
    uint32 Resolution = 2048;
    uint32 CascadeCount = 4;

    bool IsValid() const
    {
        const bool bValidResolution = Resolution == 512 || Resolution == 1024 || Resolution == 2048 || Resolution == 4096;

        return bValidResolution && CascadeCount >= 1 && CascadeCount <= FCascadeShadowMaps::MaxCascades;
    }
};

class FRenderer
{
public:
	bool Init();

	void DepthPrepass(const FMatrix& ViewProjection, const EViewMode ViewMode);

	// 기존 단일 카메라의 ViewProjection으로 렌더 큐 전체를 그린다.
	void RenderAll(FRenderQueue& InQueue, UCameraComponent* CameraComponent, const EViewMode ViewMode);

	// Adapter가 계산한 ViewProjection을 직접 받아 View별 렌더 큐를 그린다.
	void RenderAll(FRenderQueue& InQueue, const FMatrix& ViewProjection, const FVector CameraLocation, const EViewMode ViewMode);

	// 큐를 정렬해 불투명 패킷만 그린다. 반투명은 RenderTranslucent 호출 전까지 보관한다.
	void RenderOpaque(const FMatrix& ViewProjection, const EViewMode ViewMode, bool bDepthPrepassed = false);

	// Gather 전에 World가 불러 가려진 물체를 거른다.
	FGPUOcclusion& GetGPUOcclusion() { return GPUOcclusion; }
	FTileBasedCulling& GetTileBasedCulling() { return TileBasedCulling; }

	// 캐시된 스태틱 메시 경로: World::Gather가 조각별로 채운 묶음을 이번 프레임 불투명 패스에 넘긴다.
	// 묶음 메모리는 World 소유이며 다음 Gather 전까지 유효하다. RenderQueueSorting이 묶음을 정렬하고 RenderOpaque가 그린다.
	void ResetStaticDrawGroups() { StaticGroups.clear(); }
	void AddStaticDrawGroup(const FStaticDrawGroup* Group) { StaticGroups.push_back(Group); }

	// RenderOpaque가 보관한 반투명 패킷을 먼 것부터 그린다.
	void RenderTranslucent(const FMatrix& ViewProjection, const EViewMode ViewMode);

	// 큐를 Material, Opaque, Mesh에 따라 정렬한다
	void RenderQueueSorting(FRenderQueue& InQueue, const FMatrix& ViewProjection, const FVector& CameraPosition);

	// [측정 전용] RenderOpaque 직후에 호출한다. 불투명 패킷을 깊이 LESS_EQUAL·색 쓰기 없이 다시 그리며
	// 패킷마다 오클루전 쿼리를 걸어, 최종 화면에 실제로 픽셀을 남긴 물체 수를 센다.
	// GPU 결과를 기다리므로 매우 느리다. 버튼 등으로 한 프레임만 실행할 것.
	FOcclusionMeasureResult MeasureOpaqueOcclusion(const FMatrix& ViewProjection, const EViewMode ViewMode);

	uint8* BeginObjectConstants(uint32 MaxSlots);
	void EndObjectConstants();

    void UploadLightGlobalConstants(const FMatrix &DirectionalLightVP);

	void SetLight(const FLightConstants& LightData);
    void SetShadowQuality(const uint32 &InShadowQuality){ ShadowQuality = InShadowQuality; }
    void SetPCSSMaxKernel(const uint32& InPCSSMaxKernel) { PCSSMaxKernel = InPCSSMaxKernel; }
    float GetTransitionFactor(){ return TransitionFactor; }
    void SetTransitionFactor(const float&InTransitionFactor){ TransitionFactor = InTransitionFactor; }

    // 광원별 DSV와 Light VP를 받아 공통 Shadow Depth Pass를 실행한다.
    bool RenderShadowDepth(const FScene& Scene, const FShadowPassDesc& Desc, const FMatrix& LightVP, EViewMode ViewMode, const TArray<FPrimitiveSceneProxy*>* ShadowCasters = nullptr);

    // Directional Light의 그림자 맵을 렌더링하고 메인 카메라의 ViewCB를 복원한다.
    void RenderDirectionalShadowMap(const FScene& Scene, const FMatrix &CameraViewProjection, const FVector &CameraLocation, float CameraNear, float CameraFar, bool bOrthographic, const EViewMode ViewMode);

    void RenderSpotShadowMap(const FScene& Scene, const FMatrix &CameraViewProjection, const FVector &CameraLocation, const EViewMode ViewMode);
    void RenderPointShadowMap(const FScene& Scene, const FMatrix &CameraViewProjection, const FVector &CameraLocation, const EViewMode ViewMode);

    void RenderShadowMap(const FScene& Scene, const FMatrix &CameraViewProjection, const FVector &CameraLocation, const EViewMode ViewMode);

	// 오브젝트 칸에 World와 노멀 변환용 역전치를 함께 쓴다. 모든 칸 쓰기는 이 함수를 거친다.
	inline void WriteObjectSlot(uint8* SlotBase, uint32 SlotIndex, const FMatrix& World)
	{
		FPerObjectConstants Constants;
		Constants.World = World;
		Constants.WorldInvTranspose = World.Inverse().GetTransposed();
		std::memcpy(SlotBase + size_t(SlotIndex) * ObjectSlotBytes, &Constants, sizeof(Constants));
	}

    inline void WriteObjectSlot(uint8 *SlotBase, uint32 SlotIndex, const FMatrix &World, const bool bReceiveShadow)
    {
        FPerObjectConstants Constants;
        Constants.World = World;
        Constants.WorldInvTranspose = World.Inverse().GetTransposed();
        Constants.bReceiveShadow = bReceiveShadow ? 1 : 0;
        std::memcpy(SlotBase + size_t(SlotIndex) * ObjectSlotBytes, &Constants, sizeof(Constants));
    }

	inline void WriteObjectSlot(uint8* SlotBase, uint32 SlotIndex, const FMatrix& World, const FMatrix& WorldInverse)
	{
		FPerObjectConstants Constants;
		Constants.World = World;
		Constants.WorldInvTranspose = WorldInverse.GetTransposed();
		std::memcpy(SlotBase + size_t(SlotIndex) * ObjectSlotBytes, &Constants, sizeof(Constants));
	}

    inline void WriteObjectSlot(uint8* SlotBase, uint32 SlotIndex, const FMatrix& World, const FMatrix& WorldInverse, bool bReceiveShadow)
    {
        FPerObjectConstants Constants;
        Constants.World = World;
        Constants.WorldInvTranspose = WorldInverse.GetTransposed();
        Constants.bReceiveShadow = bReceiveShadow ? 1 : 0;        
        std::memcpy(SlotBase + size_t(SlotIndex) * ObjectSlotBytes, &Constants, sizeof(Constants));
    }

	ID3D11ShaderResourceView* GetPointLightSRV() const { return PointLightSRV.Get(); }
	ID3D11ShaderResourceView* GetSpotLightSRV() const { return SpotLightSRV.Get(); }

	uint32 GetPointLightCount() const { return LightData.PointLightCount; }
	uint32 GetSpotLightCount() const { return LightData.SpotLightCount; }

    void SetShadowsEnabled(bool bEnabled);

    FTexture2D *GetDirectionalShadowMap() const
    {
        return ShadowRenderer ? ShadowRenderer->GetDirectionalShadowMap() : nullptr;
    }

    FTexture2D *GetSpotShadowMap() const
    {
        return ShadowRenderer ? ShadowRenderer->GetSpotShadowMap() : nullptr;
    }
    FTextureCube* GetPointShadowMap() const
    {
        return ShadowRenderer ? ShadowRenderer->GetPointLightShadowMap() : nullptr;
    }
    FTexture2D* GetShadowMap() const
    {
        return ShadowRenderer ? ShadowRenderer->GetShadowMapAtlas() : nullptr;
    }

    ID3D11ShaderResourceView *GetSpotShadowMapSRV() const
    {
        FTexture2D *SpotShadowMap = GetSpotShadowMap();
        return SpotShadowMap ? SpotShadowMap->GetSRV() : nullptr;
    }

	ID3D11ShaderResourceView* GetDirectionalShadowMapSRV() const
    {
        FTexture2D *DirectionalShadowMap = GetDirectionalShadowMap();
        return DirectionalShadowMap ? DirectionalShadowMap->GetSRV() : nullptr;
    }

    ID3D11ShaderResourceView* GetShadowMapSRV() const
    {
        FTexture2D* ShadowMap = GetShadowMap();
        return ShadowMap ? ShadowMap->GetSRV() : nullptr;
    }

    const FCascadeShadowMaps &GetCascadeShadowMaps() const
    {
        return CascadeShadowMaps;
    }

    bool GetCascadeDebugColors() const
    {
        return bCascadeDebugColors;
    }

    void SetCascadeDebugColors(bool bEnabled)
    {
        bCascadeDebugColors = bEnabled;
    }

    void UpdateCascadeDebugConstants(const FVector &CameraForward);

    float GetCSMDistributionExponent() const
    {
        return CSMDistributionExponent;
    }

    void SetCSMDistributionExponent(float Value)
    {
        // UI 외부에서 변경해도 범위 보장
        CSMDistributionExponent = Value < 1.0f ? 1.0f : (Value > 10.0f ? 10.0f : Value);
    }

    void SetCSMEnabled(bool bEnabled)
    {
        bCSMEnabled = bEnabled;
    }

    bool GetCSMEnabled() const
    {
        return bCSMEnabled;
    }

    bool GetCascadeBlendEnabled() const
    {
        return bCascadeBlendEnabled;
    }

    void SetCascadeBlendEnabled(bool bEnabled)
    {
        bCascadeBlendEnabled = bEnabled;
    }

    void CaptureCascadePreview();

    static constexpr uint32 CascadePreviewResolution = 200;

    uint32 GetCascadePreviewCount() const
    {
        return CascadePreviewCount;
    }

    const FTexture2D *GetCascadePreviewTexture(uint32 Index) const
    {
        return Index < CascadePreviewCount ? CascadePreviewTextures[Index].get() : nullptr;
    }

    // 변경 요청만 저장한다.
    bool RequestCSMSettings(uint32 Resolution, uint32 CascadeCount);

    // 실제 적용된 설정을 반환한다.
    const FCSMSettings &GetCSMSettings() const
    {
        return CSMSettings;
    }

    void PrepareCascadeShadows(const FScene &Scene, const FMatrix &CameraViewProjection, const FVector &CameraLocation, const FVector &CameraForward, float CameraNear, float CameraFar,bool bOrthographic, EViewMode ViewMode);

private:
	// FIFO 소비용 배열의 용량만 재사용하며 매 View의 패킷 값은 새로 채운다.
	FRenderQueue RenderPackets;
	// 정렬된 RenderPackets에서 반투명 패킷이 시작되는 위치
	uint32 FirstTranslucentIndex = 0;
	uint32 FirstMaterialIndex = 0;
	TUniquePtr<FConstantBuffer> PerObjectCB;
	TUniquePtr<FConstantBuffer> ViewCB;

	// 모든 패킷의 World 행렬을 256바이트 칸에 한 번에 올린 버퍼. D3D11.1 오프셋 바인딩을 못 쓰면 PerObjectCB로 돌아간다.
	TUniquePtr<FConstantBuffer> PerObjectSlotCB;
	uint32 PerObjectSlotCapacity = 0;
	bool bUsePerObjectSlots = false;
	bool bObjectConstantsPrepared = false;
	UMaterial* LastMaterial;
	UStaticMesh* LastMesh;

	// MeasureOpaqueOcclusion 전용. 처음 측정할 때 만들고 이후 재사용한다.
	TArray<ComPtr<ID3D11Query>> OcclusionQueries;
	ComPtr<ID3D11DepthStencilState> DepthLessEqualReadOnly;

	FGPUOcclusion GPUOcclusion;
	FTileBasedCulling TileBasedCulling;

	// 라이트 cbuffer(b3)와 로컬 라이트 StructuredBuffer(t8, t9)를 VS·PS에 꽂는다.
	void BindLightResources();
	void DrawPackets(uint32 Begin, uint32 End, const FMatrix& ViewProjection, const EViewMode ViewMode);
	void DrawStaticGroups(const EViewMode ViewMode);
	void UpdatePerObjectConstants(const FMatrix& World);
    void UpdatePerObjectConstants(const FMatrix &World, const bool bReceiveShadow);

	// 이번 프레임 스태틱 메시 묶음 (정렬 키 순). 비어 있지 않은 것만 담는다.
	std::vector<const FStaticDrawGroup*> StaticGroups;
	void BindMaterial(UMaterial* material, const EViewMode ViewMode);
	void UpdateMaterialParams(const FRenderPacket& RenderPacket);
	void UpdatePerObjectConstants(const FRenderPacket& RenderPacket, const FMatrix& ViewProjection, const bool bReceiveShadow);
	void EnsurePerObjectSlotCapacity(uint32 SlotCount);
	void UploadPerObjectConstants();

	TArray<FSortEntry> SortEntries;

	TUniquePtr<FConstantBuffer> LightCB;
	FLightConstants LightData{};

    uint32  ShadowQuality = 0;
    float TransitionFactor = 2;
    uint32  PCSSMaxKernel = 16;

	//CS, PS가 읽는 동적 StructuredBuffer
	ComPtr<ID3D11Buffer> PointLightBuffer;
	ComPtr<ID3D11ShaderResourceView> PointLightSRV;
	ComPtr<ID3D11Buffer> SpotLightBuffer;
	ComPtr<ID3D11ShaderResourceView> SpotLightSRV;

    TUniquePtr<FShadowRenderer> ShadowRenderer;

    // 카메라 RenderQueue와 독립적으로 Scene의 Shadow Caster를 렌더링한다.
    // Caster 목록이 전달되면 해당 목록을, 없으면 Scene.Proxies를 순회한다.
    void DrawShadowCasters(const FScene& Scene, const FMatrix& LightVP, EViewMode ViewMode, const TArray<FPrimitiveSceneProxy*>* ShadowCasters = nullptr);

	//DepthPrepass 시 잠깐 켜짐, 머테리얼 바인딩을 건너뜀
	bool bDepthOnly = false;

	//DepthPrepass가 이전에 실행되었다면 true, Opaque pass에서만 사용됨
	bool bDepthPrepassed = false;

    bool bShadowsEnabled = true;

    // Shadow map pass에서만 사용
    bool bShadowMapPass = false;

    FCascadeDebugConstants CascadeConstants{};
    // 각 Cascade의 Shadow Pass에 사용할 CPU 측 목록.
    TArray<FPrimitiveSceneProxy *> CascadeShadowCasters[4];
    bool bCSMEnabled = true;
    float CSMShadowDistance = 40.0f;
    float CSMDistributionExponent = 3.0f;

    FCascadeShadowMaps CascadeShadowMaps;
    bool bCascadeDebugColors = false;
    bool bCascadeBlendEnabled = false;
    TUniquePtr<FTexture2D> CascadePreviewTextures[FCascadeShadowMaps::MaxCascades];
    // 마지막으로 캡처한 이미지 개수
    uint32 CascadePreviewCount = 0;
    bool InitCascadePreviewResources();
    TUniquePtr<FConstantBuffer> CascadeDebugCB;
    void UpdateCascadeDebugConstants(const FScene &Scene, const FMatrix &CameraViewProjection, const FVector &CameraForward, float CameraNear, float CameraFar, bool bOrthographic);
    void RenderCascadeShadowPreview(const FScene &Scene, const FMatrix &CameraViewProjection, const FVector &CameraLocation, EViewMode ViewMode);

    // 렌더 패스를 시작하기 전에 요청을 적용한다.
    bool ApplyPendingCSMSettings();
    bool RecreateCSMResources(const FCSMSettings &NewSettings);
    FCSMSettings CSMSettings;
    FCSMSettings PendingCSMSettings;
    bool bHasPendingCSMSettings = false;
};

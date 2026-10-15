#include "EnginePCH.h"
#include "TileBasedCulling.h"

#include "Render/Renderer.h"
#include "Render/RenderCommand.h"
#include "Core/Async/TaskPool.h"
#include <d3dcompiler.h>

namespace
{
	// 셰이더 폴더의 .hlsl은 전부 VS/PS로 자동 컴파일되므로 컴퓨트 셰이더는 소스를 여기에 둔다.
	constexpr char TileBasedCullingShaderSource[] = R"(
#pragma pack_matrix(row_major)
#define TILE_SIZE 16
#define MAX_LIGHTS_PER_TILE 64
#define SPOT_FLAG 0x80000000


struct FPointLightInfo
{
    float4 PositionRadius;
    float4 ColorIntensity;
    float4 FalloffEnabled;
};

struct FSpotLightInfo
{
    float4 PositionRadius;
    float4 ColorIntensity;
    float4 FalloffEnabled;
    float4 OutAngleInvCosDiff;
    float4 FowardVector;
};

//Tile Culling에 필요한 상수 버퍼
cbuffer TileCullConstants : register(b0)
{
    matrix View;
    matrix InvProjection;
    
    uint2 TileCount;
    uint2 ScreenSize;
    
    uint TilePointLightCount;
    uint TileSpotLightCount;
    uint2 Padding;
};

//StructuredBuffer (읽기용)
StructuredBuffer<FPointLightInfo> PointLights : register(t0);
StructuredBuffer<FSpotLightInfo> SpotLights : register(t1);
Texture2D<float> SceneDepth : register(t2);

//RWSTructuredBuffer (쓰기용)
RWStructuredBuffer<uint2> TileHeader : register(u0); // x = offset, y = count
RWStructuredBuffer<uint> TileLightIndices : register(u1);

//groupshared : 그룹 내 공유하는 메모리
groupshared uint sMinDepth; //최소 깊이
groupshared uint sMaxDepth; //최대 깊이
groupshared uint sList[MAX_LIGHTS_PER_TILE]; //현재 타일에 포함된 라이트
groupshared uint sCount; //현재 타일에 포함된 라이트 수

float3 Unproject(float2 ndcXY, float ndcZ)
{
    float4 p = mul(float4(ndcXY, ndcZ, 1.0f), InvProjection);
    return p.xyz / p.w;
}

bool SphereIntersectsAABB(float3 Center, float Radius, float3 BoxCenter, float3 BoxExtent)
{
    float3 d = max(0, abs(Center - BoxCenter) - BoxExtent); // 박스 표면까지의 축별 거리 (안쪽이면 0)
    return dot(d, d) <= Radius * Radius; // 가장 가까운 점까지의 거리² ≤ 반지름²
}

void AddLight(uint LightId)
{
    uint Slot;
    InterlockedAdd(sCount, 1, Slot); // Slot = 더하기 전 값 → 나만의 칸
    if (Slot < MAX_LIGHTS_PER_TILE)
        sList[Slot] = LightId; // 다른 스레드와 겹치지 않음
}

[numthreads(TILE_SIZE, TILE_SIZE, 1)] // 그룹 하나 = 16×16×1 = 256 스레드
void TileCullCS(
	uint3 Gid : SV_GroupID, //그룹 번호
	uint3 GTid : SV_GroupThreadID, //그룹 안의 위치(0~15, 0~15, 0)
	uint3 DTid : SV_DispatchThreadID, //전체 위치 = Gid x numthreads + GTid (이중배열 계산 방식)
	uint GI : SV_GroupIndex)		    //그룹 안의 1차원 번호 (0~255)
{
	//그룹의 첫 번째 스레드 실행 시, 그룹변수를 초기화한다.
    if (GI == 0)
    {
        sMinDepth = 0x7F7FFFFF;
        sMaxDepth = 0;
        sCount = 0;
    }

    GroupMemoryBarrierWithGroupSync();

    // 1. 스레드마다 픽셀 깊이 기록
    // 마지막 줄·열 타일의 화면 밖 스레드는 읽지 않는다 (범위 밖 읽기는 0을 돌려줘 MinDepth를 망친다)
    if (all(DTid.xy < ScreenSize))
    {
        float d = SceneDepth[DTid.xy];
        if (d < 1.0f) // 하늘 픽셀 제외
        {
            InterlockedMin(sMinDepth, asuint(d)); //asuint : float -> uint로 reinterpret
            InterlockedMax(sMaxDepth, asuint(d));
        }
    }

    //2. 같은 그룹 안의 스레드가 1번을 다할 때까지 대기
    GroupMemoryBarrierWithGroupSync();

    //3. 그룹마다 2번 결과물로 Frustum을 만들기
    //256개의 스레드가 같은 AABB 도출 연산을 하지만, 같은 워프의 스레드는 어처피 같은 명령을
    //동시에 실행하므로 시간차이는 거의 없다.(워프 = 32개면, 실행시간은 256/32 = 8번 계산하는 정도)
    
    const uint TileIndex = Gid.y * TileCount.x + Gid.x;
    // 타일 전체가 하늘이면 깊이 범위가 없어 AABB를 만들 수 없다 (그룹 전체가 같은 값이라 분기해도 안전)
    const bool bEmpty = (sMaxDepth == 0);

    if (!bEmpty)
    {
        //3_1. 타일 시작 NDC좌표와 끝 좌표 구하기
        float2 px0 = float2(Gid.xy * TILE_SIZE);
        float2 px1 = px0 + TILE_SIZE;

        float2 InvScreen = 1.0f / float2(ScreenSize);
        float2 ndc0 = float2(px0.x * InvScreen.x * 2 - 1, 1 - px0.y * InvScreen.y * 2);
        float2 ndc1 = float2(px1.x * InvScreen.x * 2 - 1, 1 - px1.y * InvScreen.y * 2);

        float MinD = asfloat(sMinDepth);
        float MaxD = asfloat(sMaxDepth);

        //3_4. 좌표들을 감싸는 AABB 구하기
        float3 BoxMin = 1e30f;
        float3 BoxMax = -1e30f;

        [unroll]
        for (uint k = 0; k < 8; ++k)
        {
            float2 xy = float2((k & 1) ? ndc1.x : ndc0.x,  // 비트 0: 왼쪽/오른쪽
                               (k & 2) ? ndc1.y : ndc0.y); // 비트 1: 위/아래
            float z = (k & 4) ? MaxD : MinD; // 비트 2: 가까운 면/먼 면
            float3 v = Unproject(xy, z);
            BoxMin = min(BoxMin, v);
            BoxMax = max(BoxMax, v);
        }
        float3 BoxCenter = (BoxMin + BoxMax) * 0.5f;
        float3 BoxExtent = BoxMax - BoxCenter;

        //4. AABB와 각 라이트들이 겹치는지 검사, 스레드마다 라이트를 나눠 맡는다.
        // 포인트 라이트
        for (uint i = GI; i < TilePointLightCount; i += TILE_SIZE * TILE_SIZE)
        {
            FPointLightInfo L = PointLights[i];
            if (L.FalloffEnabled.y == 0.0f)
                continue; // 꺼진 라이트 제외

            float3 C = mul(float4(L.PositionRadius.xyz, 1.0f), View).xyz;
            if (SphereIntersectsAABB(C, L.PositionRadius.w, BoxCenter, BoxExtent))
                AddLight(i);
        }

        // 스폿 라이트: 일단 구로 판정 (원뿔 판정은 나중에 추가)
        for (uint j = GI; j < TileSpotLightCount; j += TILE_SIZE * TILE_SIZE)
        {
            FSpotLightInfo L = SpotLights[j];
            if (L.FalloffEnabled.y == 0.0f)
                continue;

            float3 C = mul(float4(L.PositionRadius.xyz, 1.0f), View).xyz;
            if (SphereIntersectsAABB(C, L.PositionRadius.w, BoxCenter, BoxExtent))
                AddLight(j | SPOT_FLAG); // 최상위 비트로 스폿 표시
        }
    }
    

    //5. 같은 그룹 안의 스레드가 끝날때까지 대기 후, 구조화된 버퍼에 기록
    GroupMemoryBarrierWithGroupSync();

    // 칸이 넘쳐도 sCount는 계속 늘어나므로 MAX로 자른다
    const uint Count = bEmpty ? 0 : min(sCount, MAX_LIGHTS_PER_TILE);
    // 타일마다 MAX_LIGHTS_PER_TILE칸씩 할당되어 있다
    const uint Offset = TileIndex * MAX_LIGHTS_PER_TILE;

    // 목록 복사는 스레드끼리 나눠서 한다
    for (uint l = GI; l < Count; l += TILE_SIZE * TILE_SIZE)
        TileLightIndices[Offset + l] = sList[l];

    // 헤더는 타일당 하나이므로 한 스레드만 쓴다
    if (GI == 0)
        TileHeader[TileIndex] = uint2(Offset, Count);
})";

	ComPtr<ID3D11ComputeShader> CompileComputeShader(ID3D11Device* Device, const char* EntryPoint)
	{
		const UINT Flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
		ComPtr<ID3DBlob> Code, Errors;
		const HRESULT Hr = D3DCompile(TileBasedCullingShaderSource, sizeof(TileBasedCullingShaderSource) - 1, "TileCullCS",
			nullptr, nullptr, EntryPoint, "cs_5_0", Flags, 0, Code.GetAddressOf(), Errors.GetAddressOf());
		if (FAILED(Hr))
		{
			if (Errors)
				OutputDebugStringA(static_cast<const char*>(Errors->GetBufferPointer()));
			HTR_LOG(Error, "[TileBasedCulling] compute shader {} compile failed", EntryPoint);
			return nullptr;
		}

		ComPtr<ID3D11ComputeShader> Shader;
		if (FAILED(Device->CreateComputeShader(Code->GetBufferPointer(), Code->GetBufferSize(), nullptr, Shader.GetAddressOf())))
			return nullptr;
		return Shader;
	}

	ComPtr<ID3D11Buffer> CreateDefaultConstantBuffer(ID3D11Device* Device, uint32 Size)
	{
		D3D11_BUFFER_DESC Desc{};
		Desc.ByteWidth = (Size + 15) & ~15u;
		Desc.Usage = D3D11_USAGE_DEFAULT;
		Desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		ComPtr<ID3D11Buffer> Buffer;
		Device->CreateBuffer(&Desc, nullptr, Buffer.GetAddressOf());
		return Buffer;
	}

	constexpr uint32 TileSize = 16;
	constexpr uint32 MaxLightsPerTile = 64;

	bool CreateRWStructuredBuffer(uint32 Stride, uint32 Count, ComPtr<ID3D11Buffer>& OutBuffer, ComPtr<ID3D11UnorderedAccessView>& OutUAV, ComPtr<ID3D11ShaderResourceView>& OutSRV)
	{
		ID3D11Device* Device = RenderCommand::GetDevice();

		D3D11_BUFFER_DESC Desc{};
		Desc.ByteWidth = Stride * Count;
		Desc.Usage = D3D11_USAGE_DEFAULT;
		Desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
		Desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		Desc.StructureByteStride = Stride;

		D3D11_UNORDERED_ACCESS_VIEW_DESC UAVDesc{};
		UAVDesc.Format = DXGI_FORMAT_UNKNOWN;
		UAVDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		UAVDesc.Buffer.NumElements = Count;

		D3D11_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
		SRVDesc.Format = DXGI_FORMAT_UNKNOWN;
		SRVDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		SRVDesc.Buffer.NumElements = Count;

		if (FAILED(Device->CreateBuffer(&Desc, nullptr, OutBuffer.GetAddressOf())) ||
			FAILED(Device->CreateUnorderedAccessView(OutBuffer.Get(), &UAVDesc, OutUAV.GetAddressOf())) ||
			FAILED(Device->CreateShaderResourceView(OutBuffer.Get(), &SRVDesc, OutSRV.GetAddressOf())))
			return false;
		return true;
	}

	struct alignas(16) FTileCullConstants
	{
		FMatrix View;
		FMatrix InvProjection;

		uint32 TileCount[2] = {}; // x,y
		uint32 ScreenSize[2] = {}; // 너비,높이

		uint32 TilePointLightCount = 0;
		uint32 TileSpotLightCount = 0;
		uint32 Padding[2] = {};
	};
	static_assert(sizeof(FTileCullConstants)  == 160, "TileCullConstants must be 16-byte aligned");
}

bool FTileBasedCulling::Init()
{
	ID3D11Device* Device = RenderCommand::GetDevice();

	TileBasedCullingCS = CompileComputeShader(Device, "TileCullCS");
	TileBasedConstantsCB = CreateDefaultConstantBuffer(Device, sizeof(FTileCullConstants));

	const bool bReady = TileBasedCullingCS && TileBasedConstantsCB;
	if (!bReady)
		HTR_LOG(Error, "[TileBasedCulling] init failed. GPU occlusion culling is unavailable.");
	return bReady;
}

bool FTileBasedCulling::Run(const FTileCullInputs& Inputs)
{
	if (!TileBasedCullingCS || !Inputs.DepthSRV || Inputs.Width == 0 || Inputs.Height == 0) return false;

	// 타일 수 -> 결과 버퍼 확보
	const uint32 TileCountX = (Inputs.Width + TileSize - 1) / TileSize;
	const uint32 TileCountY = (Inputs.Height + TileSize - 1) / TileSize;
	if (!EnsureTileCapacity(TileCountX * TileCountY))
		return false;

	// 상수 채우기 -> 버퍼 갱신
	FTileCullConstants Constants{};
	Constants.View = Inputs.View;
	Constants.InvProjection = Inputs.Projection.Inverse();
	Constants.TileCount[0] = TileCountX;
	Constants.TileCount[1] = TileCountY;
	Constants.ScreenSize[0] = Inputs.Width;
	Constants.ScreenSize[1] = Inputs.Height;
	Constants.TilePointLightCount = Inputs.PointLightCount;
	Constants.TileSpotLightCount = Inputs.SpotLightCount;

	ID3D11DeviceContext* Context = RenderCommand::GetContext();
	Context->UpdateSubresource(TileBasedConstantsCB.Get(), 0, nullptr, &Constants, 0, 0);

	// 바인딩
	ID3D11Buffer* ConstBuffers[1] = { TileBasedConstantsCB.Get() };
	ID3D11ShaderResourceView* SRVs[3] = { Inputs.PointLightSRV, Inputs.SpotLightSRV, Inputs.DepthSRV };
	ID3D11UnorderedAccessView* UAVs[2] = { TileHeaderUAV.Get(), TileLightIndicesUAV.Get() };
	Context->CSSetConstantBuffers(0, 1, ConstBuffers);
	Context->CSSetShaderResources(0, 3, SRVs);
	Context->CSSetUnorderedAccessViews(0, 2, UAVs, nullptr);
	Context->CSSetShader(TileBasedCullingCS.Get(), nullptr, 0);

	// 실행
	Context->Dispatch(TileCountX, TileCountY, 1);

	// 해제
	ID3D11ShaderResourceView* NullSRVs[3] = {};
	ID3D11UnorderedAccessView* NullUAVs[2] = {};
	Context->CSSetShaderResources(0, 3, NullSRVs);
	Context->CSSetUnorderedAccessViews(0, 2, NullUAVs, nullptr);
	Context->CSSetShader(nullptr, nullptr, 0);

	return true;
}

void FTileBasedCulling::BindResultToPs() const
{
	ID3D11DeviceContext* Context = RenderCommand::GetContext();

	ID3D11ShaderResourceView* SRVs[2] = { TileHeaderSRV.Get(), TileLightIndicesSRV.Get() };
	Context->PSSetShaderResources(1, 2, SRVs);

	ID3D11Buffer* CBs[1] = { TileBasedConstantsCB.Get() };
	Context->PSSetConstantBuffers(4, 1, CBs);
}

void FTileBasedCulling::UnbindResultFromPs() const
{
	ID3D11DeviceContext* Context = RenderCommand::GetContext();
	ID3D11ShaderResourceView* NullSRVs[2] = {};
	ID3D11Buffer* NullCBs[1] = {};
	Context->PSSetShaderResources(1, 2, NullSRVs);
	Context->PSSetConstantBuffers(4, 1, NullCBs);
}

bool FTileBasedCulling::EnsureTileCapacity(uint32 TileCount)
{
	if (TileCount <= TileHeaderCapacity && TileHeaderBuffer && TileLightIndicesBuffer)
		return true;

	TileHeaderBuffer.Reset(); TileHeaderSRV.Reset(); TileHeaderUAV.Reset();
	TileLightIndicesBuffer.Reset(); TileLightIndicesSRV.Reset(); TileLightIndicesUAV.Reset();
	TileHeaderCapacity = 0;

	if (!CreateRWStructuredBuffer(sizeof(uint64), TileCount, TileHeaderBuffer, TileHeaderUAV, TileHeaderSRV) ||
		!CreateRWStructuredBuffer(sizeof(uint32), MaxLightsPerTile * TileCount, TileLightIndicesBuffer, TileLightIndicesUAV, TileLightIndicesSRV))
	{
		HTR_LOG(Error, "[TileBasedCulling] Failed to create RWStructuredBuffers for tile culling.");
		return false;
	}

	TileHeaderCapacity = TileCount;
	return true;
}



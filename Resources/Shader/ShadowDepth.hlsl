#pragma pack_matrix(row_major)

// 물체마다 갱신하는 데이터
cbuffer ShadowObjectConstants : register(b2)
{
    matrix World;
};

// 빛이 장면을 바라보는 정보
cbuffer ShadowConstants : register(b5)
{
    matrix LightViewProjection;

    float2 InvShadowMapSize;
    float DepthBias;
    uint bEnabled;
};

// 색상, UV, 노멀, 탄젠트는 읽지 않는다.
struct VS_INPUT
{
    float3 position : POSITION;
};

// vertex를 빛 기준으로 변환
float4 mainVS(VS_INPUT input) : SV_POSITION
{
    float4 worldPosition = mul(float4(input.position, 1.0f), World);

    // 월드 위치를 빛 기준 Clip 좌표로 바꾼다.
    return mul(worldPosition, LightViewProjection);
}

// 현재 셰이더 로더가 VS와 PS를 모두 요구해서 작성
// 실행하지 않을 예정
float4 mainPS() : SV_TARGET
{
    return float4(1.0f, 0.0f, 1.0f, 1.0f);
}
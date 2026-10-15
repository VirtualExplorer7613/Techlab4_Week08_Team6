#pragma pack_matrix(row_major)

#include "Shared/LightDefinitions.h"

#if !defined(LIGHTING_MODEL_GOURAUD) && !defined(LIGHTING_MODEL_LAMBERT) && !defined(LIGHTING_MODEL_PHONG) && !defined(LIGHTING_MODEL_UNLIT) && !defined(VIEWMODE_WORLDNORMAL) && !defined(VIEWMODE_LIGHTCULLING)
    #define LIGHTING_MODEL_PHONG 1
#endif

static const float PI = 3.14159265359f;
static const float3 Ks = float3(0.04f, 0.04f, 0.04f);

cbuffer Viewconstants : register(b0)
{
    matrix VP;
    float3 CameraLocation;
    float ViewPadding;
};

cbuffer Worldconstants : register(b2)
{
    matrix World;
    matrix WorldInvTranspose;
};

cbuffer MaterialParams : register(b1)
{
    float4 BaseColor;
    float2 UVOffset;
    float bOpaque;
    float Shininess;
    
    float NormalStrength;
    float HasNormalMap;
    float2 MaterialPadding;
};

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

struct FAmbientLightInfo
{
    float4 ColorIntensity;
};

struct FDirectionalLightInfo
{
    float4 ColorIntensity;
    float4 Direction;
};

// 개수가 하나뿐인 라이트와 로컬 라이트 개수만 cbuffer에 둔다 (C++ FLightGlobalConstants와 같은 레이아웃)
cbuffer LightConstants : register(b3)
{
    FAmbientLightInfo AmbientLight;
    FDirectionalLightInfo DirectionalLight;
    uint PointLightCount;
    uint SpotLightCount;
    uint2 LightPadding;
};

// 로컬 라이트 배열은 개수 제한이 없고 인덱스가 달라도 느려지지 않는 StructuredBuffer로 읽는다.
// 머티리얼 텍스처(t0~)와 겹치지 않도록 뒤쪽 슬롯을 쓴다 (C++ LightSRVSlot과 같은 값)
StructuredBuffer<FPointLightInfo> PointLights : register(t8);
StructuredBuffer<FSpotLightInfo> SpotLights : register(t9);

cbuffer TileBasedCullingConstants : register(b4)
{
    matrix View;
    matrix InvProjection;
    
    uint2 TileCount;
    uint2 ScreenSize;
    
    uint TilePointLightCount;
    uint TileSpotLightCount;
    uint2 Padding;
};

#define TILE_SIZE 16
#define SPOT_FLAG 0x80000000

StructuredBuffer<uint2> TileHeader : register(t1);
StructuredBuffer<uint> TileLightIndices : register(t2);

// ACES Filmic Tone Mapping
float3 ACESFilmicToneMapping(float3 color)
{
    float a = 2.51f;
    float b = 0.03f;
    float c = 2.43f;
    float d = 0.59f;
    float e = 0.14f;
    return saturate((color * (a * color + b)) / (color * (c * color + d) + e));
}

// 라이트 하나가 표면에 주는 빛. Diffuse는 Albedo와 곱하고 Specular는 그 뒤에 더한다.
struct FLightResult
{
    float3 Diffuse;
    float3 Specular;
};

// 라이트 하나의 Blinn-Phong 계산. Lambert 모드도 같은 함수를 쓰고 Specular만 버린다.
// N: 표면 노멀, L: 표면→라이트, V: 표면→카메라 (모두 정규화), Radiance: 색 × 세기 × 감쇠
// Schlick 근사 프레넬. 정면 반사율 F0에서 시작해 스치는 각도(cosTheta → 0)로 갈수록 1에 가까워진다.
float3 FresnelSchlick(float3 F0, float cosTheta)
{
    return F0 + (1.0f - F0) * pow(1.0f - cosTheta, 5.0f);
}

FLightResult BlinnPhong(float3 N, float3 L, float3 V, float3 Radiance)
{
    FLightResult Result;

    float NdotL = saturate(dot(N, L));
    float n = max(Shininess, 1.0f);
    float norm = (n + 8.0f) / (8.0f * PI); // Blinn-Phong 정규화 상수
    float3 H = normalize(L + V);
    float NdotH = saturate(dot(N, H));

    // 하이라이트를 만드는 미세면의 노멀이 H이므로 입사각은 V·H로 잰다. Ks는 정면 반사율(F0)이다.
    float3 F = FresnelSchlick(Ks, saturate(dot(V, H)));

    // 반사된 만큼(F)을 뺀 나머지가 표면 안으로 들어가 diffuse가 된다.
    Result.Diffuse = Radiance * NdotL * 1/PI * (1.0f - F);
    // 빛을 받지 못하는 면(N·L <= 0)에는 하이라이트가 생기지 않게 한다.
    Result.Specular = Radiance * pow(NdotH, n) * norm * NdotL * F;

    return Result;
}

FLightResult CalculatePointLight(FPointLightInfo info, float3 worldPos, float3 normal, float3 viewDir)
{
    float4 PositionRadius = info.PositionRadius;
    float4 ColorIntensity = info.ColorIntensity;
    float4 FalloffEnabled = info.FalloffEnabled;

    float3 toLight = PositionRadius.xyz - worldPos;
    float distance = length(toLight);
    float3 lightDir = toLight / max(distance, 0.0001f); // length를 한 번만 계산하고 0으로 나누기를 막는다

    float radius = max(PositionRadius.w, 0.001f);
    float Basefalloff = saturate(1.0f - distance / radius);
    float falloff = pow(Basefalloff, max(FalloffEnabled.x, 0.001f));

    float enabled = FalloffEnabled.y;
    float3 radiance = ColorIntensity.rgb * ColorIntensity.w * falloff * enabled;

    return BlinnPhong(normal, lightDir, viewDir, radiance);
}

FLightResult CalculateSpotLight(FSpotLightInfo info, float3 worldPos, float3 normal, float3 viewDir)
{
    float4 PositionRadius = info.PositionRadius;
    float4 ColorIntensity = info.ColorIntensity;
    float4 FalloffEnabled = info.FalloffEnabled;
    float4 OutAngleInvCosDiff = info.OutAngleInvCosDiff;
    float3 SpotFowardVector = normalize(info.FowardVector.xyz);

    float3 toLight = PositionRadius.xyz - worldPos;
    float distance = length(toLight);
    float3 lightDir = toLight / max(distance, 0.0001f); // length를 한 번만 계산하고 0으로 나누기를 막는다

    float radius = max(PositionRadius.w, 0.001f);

    //SpotLight는 거리에 따른 감쇠와 Cone에 따른 감쇠 두가지가 있다.
    float Distancefalloff = saturate(1.0f - distance / radius);

    //콘의 어느 범위에 속하는가 = (cos(=spot 전방벡터와 -L 내적) - cosOuter) * (1 / (cosInner - cosOuter))
    //콘 감쇠 지수(FalloffEnabled.z)는 거리 감쇠 지수(FalloffEnabled.x)와 독립적이다 (D3D9 Falloff)
    float spotforwarddotL = saturate(dot(-lightDir, SpotFowardVector));
    float Conefalloff = pow(saturate((spotforwarddotL - OutAngleInvCosDiff.x) * OutAngleInvCosDiff.y), max(FalloffEnabled.z, 0.001f));
    float falloff = pow(Distancefalloff, max(FalloffEnabled.x, 0.001f)) * Conefalloff;

    float enabled = FalloffEnabled.y;
    float3 radiance = ColorIntensity.rgb * ColorIntensity.w * falloff * enabled;

    return BlinnPhong(normal, lightDir, viewDir, radiance);
}

float3 CalculateAmbientLight(FAmbientLightInfo info)
{
    return info.ColorIntensity.xyz * info.ColorIntensity.w;
}

FLightResult CalculateDirectionalLight(FDirectionalLightInfo info, float3 normal, float3 viewDir)
{
    // Direction은 빛이 나아가는 방향이므로 뒤집어야 표면→라이트 방향이 된다.
    float3 lightDir = normalize(-info.Direction.xyz);
    float3 radiance = info.ColorIntensity.xyz * info.ColorIntensity.w;

    return BlinnPhong(normal, lightDir, viewDir, radiance);
}

FLightResult CalculateLightingTiled(float2 PixelPos, float3 worldPos, float3 normal, float3 viewDir)
{
    FLightResult Total;
    Total.Diffuse = float3(0, 0, 0);
    Total.Specular = float3(0, 0, 0);

    uint2 Tile = uint2(PixelPos / TILE_SIZE);
    uint TileIndex = Tile.y * TileCount.x + Tile.x;
    
    uint2 Header = TileHeader[TileIndex];
    
    for (uint c = 0; c < Header.y; ++c)
    {
        uint Id = TileLightIndices[Header.x + c];
        FLightResult Light;
        if (Id & SPOT_FLAG)
        {
            Light = CalculateSpotLight(SpotLights[Id & ~SPOT_FLAG], worldPos, normal, viewDir);
        }
        else
        {
            Light = CalculatePointLight(PointLights[Id], worldPos, normal, viewDir);
        }
        Total.Diffuse += Light.Diffuse;
        Total.Specular += Light.Specular;
    }
    FLightResult Directional = CalculateDirectionalLight(DirectionalLight, normal, viewDir);
    Total.Diffuse += Directional.Diffuse;
    Total.Specular += Directional.Specular;
    return Total;
}

// 직접광(Point·Spot·Directional)의 Diffuse/Specular 합. Ambient는 따로 더한다.
FLightResult CalculateLighting(float3 worldPos, float3 normal, float3 viewDir)
{
    FLightResult Total;
    Total.Diffuse = float3(0, 0, 0);
    Total.Specular = float3(0, 0, 0);

    //PointLight
    for (uint i = 0; i < PointLightCount; ++i)
    {
        FLightResult Light = CalculatePointLight(PointLights[i], worldPos, normal, viewDir);
        Total.Diffuse += Light.Diffuse;
        Total.Specular += Light.Specular;
    }

    //SpotLight
    for (uint j = 0; j < SpotLightCount; ++j)
    {
        FLightResult Light = CalculateSpotLight(SpotLights[j], worldPos, normal, viewDir);
        Total.Diffuse += Light.Diffuse;
        Total.Specular += Light.Specular;
    }

    //Directional Light
    FLightResult Directional = CalculateDirectionalLight(DirectionalLight, normal, viewDir);
    Total.Diffuse += Directional.Diffuse;
    Total.Specular += Directional.Specular;

    return Total;
}


struct VS_INPUT
{
    float3 p : POSITION; // Input position from vertex buffer
    float3 n : NORMAL;
    float4 c : COLOR; // Input color from vertex buffer
    float2 t : TEXCOORD;
    float4 tangent : TANGENT;
};

struct PS_INPUT
{
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float4 color : COLOR;
    float2 uv : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
#if LIGHTING_MODEL_GOURAUD
    float3 Diffuse : TEXCOORD2;
    float3 Specular : TEXCOORD3;
#endif
    float3 tangentWorld : TEXCOORD4;
    nointerpolation float tangentSign : TEXCOORD5;
};

Texture2D g_txColor : register(t0);
SamplerState g_Sample : register(s0);
Texture2D g_txNormal : register(t3); // CPU에서 Textures[1]에 저장한 normal texture

PS_INPUT mainVS(VS_INPUT input)
{
    PS_INPUT output;

    output.worldPos = mul(float4(input.p, 1.0f), World).xyz;
    output.position = mul(mul(float4(input.p, 1.0f), World), VP);

    // 비균등 스케일에서도 올바른 노멀값을 구하기 위해 역전치행렬 사용
    output.normal = normalize(mul(float4(input.n, 0.0f), WorldInvTranspose).xyz);
    // 탄젠트를 월드 공간으로 변환
    output.tangentWorld = mul(float4(input.tangent.xyz, 0.0f), World).xyz;
    float worldSign = determinant((float3x3) World) < 0.0f ? -1.0f : 1.0f;
    output.tangentSign = input.tangent.w * worldSign;
    
#if LIGHTING_MODEL_GOURAUD
    float3 viewDir = normalize(CameraLocation - output.worldPos);
    FLightResult Light = CalculateLighting(output.worldPos, output.normal, viewDir);
    output.Diffuse = Light.Diffuse;
    output.Specular = Light.Specular;
#endif
    output.color = input.c;
    output.uv = input.t;
    return output;
}

float4 mainPS(PS_INPUT input) : SV_TARGET
{
    float4 texColor = g_txColor.Sample(g_Sample, input.uv + UVOffset);
    float4 albedo = texColor * BaseColor;
    

    float3 normal = normalize(input.normal);
    
#if !LIGHTING_MODEL_UNLIT && !LIGHTING_MODEL_GOURAUD
    if (HasNormalMap > 0.5f)
    {
        float3 N = normal;

        // 보간된 탄젠트를 N과 다시 직교화한다.
        float3 T = input.tangentWorld - N * dot(N, input.tangentWorld);

        // 유효하지 않은 탄젠트면 기존 normal을 유지한다.
        if (dot(T, T) > 1e-8f)
        {
            T = normalize(T);

            float3 B = input.tangentSign * cross(N, T);

            // 색 텍스처와 같은 UV 이동을 적용한다.
            float3 tangentNormal = g_txNormal.Sample(g_Sample, input.uv + UVOffset).rgb;

            // RGB [0, 1] -> 노멀 성분 [-1, 1]
            tangentNormal = tangentNormal * 2.0f - 1.0f;

            // 강도 0이면 탄젠트 공간의 평평한 노멀 방향이 된다.
            tangentNormal.xy *= NormalStrength;

            // 탄젠트 공간 -> 월드 공간
            normal = normalize(tangentNormal.x * T + tangentNormal.y * B + tangentNormal.z * N);
        }
    }
#endif

    // Opaque는 알파를 1로 고정한다. 뷰포트 RT를 ImGui가 알파 블렌딩으로 그리므로 알파가 남으면 비쳐 보인다
    float alpha = bOpaque > 0.5f ? 1.0f : albedo.a;

#if LIGHTING_MODEL_UNLIT
    return float4(albedo.xyz, 1.0f);

#elif VIEWMODE_WORLDNORMAL
    return float4(normal * 0.5f + 0.5f, 1.0f);   // -1~1 노멀을 0~1 색으로

#elif VIEWMODE_LIGHTCULLING
    // 컬링 결과가 없는 물체(반투명 등)는 회색으로 표시한다
    if (TileCount.x == 0)
        return float4(0.2f, 0.2f, 0.2f, 1.0f);

    // 타일에 남은 라이트 개수: 파랑(0개) → 초록(8개) → 빨강(16개 이상)
    uint2 HeatTile = uint2(input.position.xy) / TILE_SIZE;
    uint HeatCount = TileHeader[HeatTile.y * TileCount.x + HeatTile.x].y;
    float HeatT = saturate(HeatCount / 16.0f);
    float3 Heat = HeatT < 0.5f
        ? lerp(float3(0.0f, 0.0f, 1.0f), float3(0.0f, 1.0f, 0.0f), HeatT * 2.0f)
        : lerp(float3(0.0f, 1.0f, 0.0f), float3(1.0f, 0.0f, 0.0f), HeatT * 2.0f - 1.0f);
    if (any(uint2(input.position.xy) % TILE_SIZE == 0))   // 16x16 타일 경계선
        Heat *= 0.5f;
    return float4(Heat, 1.0f);
#endif
   
    albedo.rgb = pow(albedo.rgb, 2.2f);
    float3 viewDir = normalize(CameraLocation - input.worldPos);   // 표면 → 카메라
    float3 ambient = CalculateAmbientLight(AmbientLight);
    
    float3 finalRGB = albedo.rgb;
    
#if LIGHTING_MODEL_GOURAUD
    finalRGB = finalRGB * (ambient + input.Diffuse) + input.Specular;
    finalRGB = ACESFilmicToneMapping(finalRGB);
    finalRGB = pow(finalRGB, 1.0f / 2.2f);
    return float4(finalRGB, alpha);
#endif

    FLightResult Light;
    if (TileCount.x > 0)
        Light = CalculateLightingTiled(input.position.xy, input.worldPos, normal, viewDir);
    else
        Light = CalculateLighting(input.worldPos, normal, viewDir);
    
#if LIGHTING_MODEL_PHONG
    finalRGB =  finalRGB * (ambient + Light.Diffuse) + Light.Specular;
 #else
    finalRGB = finalRGB * (ambient + Light.Diffuse);
#endif
    finalRGB = ACESFilmicToneMapping(finalRGB);
    finalRGB = pow(finalRGB, 1.0f / 2.2f);
    return float4(finalRGB, alpha);
}

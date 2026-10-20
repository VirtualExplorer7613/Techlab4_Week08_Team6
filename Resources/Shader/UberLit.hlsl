#pragma pack_matrix(row_major)

#include "Shared/LightDefinitions.h"

#if !defined(LIGHTING_MODEL_GOURAUD) && !defined(LIGHTING_MODEL_LAMBERT) && !defined(LIGHTING_MODEL_PHONG) && !defined(LIGHTING_MODEL_UNLIT) && !defined(VIEWMODE_WORLDNORMAL) && !defined(VIEWMODE_LIGHTCULLING)
    #define LIGHTING_MODEL_PHONG 1
#endif

static const float PI = 3.14159265359f;
static const float3 Ks = float3(0.04f, 0.04f, 0.04f);

static const float2 DiscSamples5[] =
{ // 5 random points in disc with radius 2.500000
    float2(0.000000, 2.500000),
	float2(2.377641, 0.772542),
	float2(1.469463, -2.022543),
	float2(-1.469463, -2.022542),
	float2(-2.377641, 0.772543),
};

static const float2 DiscSamples12[] =
{ // 12 random points in disc with radius 2.500000
    float2(0.000000, 2.500000),
	float2(1.767767, 1.767767),
	float2(2.500000, -0.000000),
	float2(1.767767, -1.767767),
	float2(-0.000000, -2.500000),
	float2(-1.767767, -1.767767),
	float2(-2.500000, 0.000000),
	float2(-1.767766, 1.767768),
	float2(-1.006119, -0.396207),
	float2(1.000015, 0.427335),
	float2(0.416807, -1.006577),
	float2(-0.408872, 1.024430),
};

static const float2 DiscSamples29[] =
{ // 29 random points in disc with radius 2.500000
    float2(0.000000, 2.500000),
	float2(1.016842, 2.283864),
	float2(1.857862, 1.672826),
	float2(2.377641, 0.772542),
	float2(2.486305, -0.261321),
	float2(2.165063, -1.250000),
	float2(1.469463, -2.022543),
	float2(0.519779, -2.445369),
	float2(-0.519779, -2.445369),
	float2(-1.469463, -2.022542),
	float2(-2.165064, -1.250000),
	float2(-2.486305, -0.261321),
	float2(-2.377641, 0.772543),
	float2(-1.857862, 1.672827),
	float2(-1.016841, 2.283864),
	float2(0.091021, -0.642186),
	float2(0.698035, 0.100940),
	float2(0.959731, -1.169393),
	float2(-1.053880, 1.180380),
	float2(-1.479156, -0.606937),
	float2(-0.839488, -1.320002),
	float2(1.438566, 0.705359),
	float2(0.067064, -1.605197),
	float2(0.728706, 1.344722),
	float2(1.521424, -0.380184),
	float2(-0.199515, 1.590091),
	float2(-1.524323, 0.364010),
	float2(-0.692694, -0.086749),
	float2(-0.082476, 0.654088),
};

#define SHADOW_QUALITY_LOW          0
#define SHADOW_QUALITY_MEDIUM       1
#define SHADOW_QUALITY_HIGH         2
#define SHADOW_QUALITY_EPIC         3
#define SHADOW_QUALITY_BOX3X3       4

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
    uint bReceiveShadow;    
    uint Paddings[3];
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
    float4 ShadowParameters; // x : Shadow Depth Bias, y : Shdaow Slope Scale, z : Inveerse Shadow Map Size, w : bCastShowShadow(1.0f : 0.0f)
    float4 ShadowFilterParameters; // x : ShadowSharpen ,y : TransitionScale, zw : 패딩
    matrix ViewProjection[6];
    float4 AtlasScaleOffset[6]; // xy : scale, zw : offset
};

struct FSpotLightInfo
{
    float4 PositionRadius;
    float4 ColorIntensity;
    float4 FalloffEnabled;
    float4 OutAngleInvCosDiff;
    float4 FowardVector;
    matrix ViewProjection;    
    float4 ShadowParameters; // x : Shadow Depth Bias, y : Shdaow Slope Scale, z : Inveerse Shadow Map Size, w : bCastShowShadow(1.0f : 0.0f)
    float4 ShadowFilterParameters; // x : ShadowSharpen ,y : TransitionScale, zw : 패딩
    float4 AtlasScaleOffset; // xy : scale, zw : offset
};

struct FAmbientLightInfo
{
    float4 ColorIntensity;
};

struct FDirectionalLightInfo
{
    float4 ColorIntensity;
    float4 Direction;
    matrix LightVP;
    float4 ShadowParameters; // x : Shadow Depth Bias, y : Shadow Slope Scale, z : Inveerse Shadow Map Size, w : bCastShowShadow(1.0f : 0.0f)
    float4 ShadowFilterParameters; // x : ShadowSharpen ,y : TransitionScale, zw : 패딩
};

// 개수가 하나뿐인 라이트와 로컬 라이트 개수만 cbuffer에 둔다 (C++ FLightGlobalConstants와 같은 레이아웃)
cbuffer LightConstants : register(b3)
{
    FAmbientLightInfo AmbientLight;
    FDirectionalLightInfo DirectionalLight;
    uint PointLightCount;
    uint SpotLightCount;
    uint bShowShadows;
    uint ShadowQuality;
};

// 로컬 라이트 배열은 개수 제한이 없고 인덱스가 달라도 느려지지 않는 StructuredBuffer로 읽는다.
// 머티리얼 텍스처(t0~)와 겹치지 않도록 뒤쪽 슬롯을 쓴다 (C++ LightSRVSlot과 같은 값)
StructuredBuffer<FPointLightInfo> PointLights : register(t8);
StructuredBuffer<FSpotLightInfo> SpotLights : register(t9);

Texture2D<float> DirectionalShadowMap : register(t10);
Texture2DArray<float> CascadeShadowMap : register(t13);
Texture2D<float> ShadowMapAtlas : register(t14);

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

cbuffer CascadeDebugConstants : register(b5)
{
    row_major float4x4 CascadeLightVP[4];

    float4 CascadeCameraForward;
    float4 CascadeSplitDepths;
    float4 CascadeBlendEndDepths;

    float CascadeShadowDistance;
    float CascadeInvResolution;
    uint CascadeCount;
    uint CSMEnabled;

    uint DebugColorsEnabled;
    uint CascadeBlendEnabled;
    uint2 CascadePadding;
};

#define TILE_SIZE 16
#define SPOT_FLAG 0x80000000

StructuredBuffer<uint2> TileHeader : register(t1);
StructuredBuffer<uint> TileLightIndices : register(t2);

Texture2D g_txColor : register(t0);
SamplerState g_Sample : register(s0);
SamplerState g_SamplePointBorder : register(s1);
Texture2D g_txNormal : register(t3); // CPU에서 Textures[1]에 저장한 normal texture\

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

float Gather1X1(Texture2D<float> ShadowMap, float4 shadowPos, float bias, float invMapSize, float TransitionScale)
{   
    float2 texelPos = (shadowPos.xy / invMapSize) - 0.5f;
    float2 base = floor(texelPos);
    float2 fraction = frac(texelPos);

    float2 gatherUV = (base + 1) * invMapSize;

    float4 depth = ShadowMap.Gather(g_SamplePointBorder, gatherUV); // x = 왼쪽 아래, y = 오른쪽 아래, z = 오른쪽 위, w = 왼쪽 위

    float4 lit = saturate((depth - (shadowPos.z - bias)) * TransitionScale + 1);

    float top = lerp(lit.w, lit.z, fraction.x);
    float bottom = lerp(lit.x, lit.y, fraction.x);

    return lerp(top, bottom, fraction.y);
}

float Gather3X3(Texture2D<float> ShadowMap, float4 shadowPos, float bias, float invMapSize, float TransitionScale)
{
    float visibility = 0;
    float recieveDepth = shadowPos.z - bias;

    
    float2 texelPos = (shadowPos.xy / invMapSize) - 0.5f;
    float2 base = floor(texelPos);
    float2 fraction = frac(texelPos);

    float4 colWeight = { 1 - fraction.x, 1, 1, fraction.x };
    float4 rowWeight = { 1 - fraction.y, 1, 1, fraction.y };
    

    for (int bx = 0; bx < 2; bx++)
    {
        for (int by = 0; by < 2; by++)
        {
            float2 gatherUV = (base + (float2(bx, by) * 2)) * invMapSize;

            float4 depth = ShadowMap.Gather(g_SamplePointBorder, gatherUV); // x = 왼쪽 아래, y = 오른쪽 아래, z = 오른쪽 위, w = 왼쪽 위
            

            float4 lit = saturate((depth - (recieveDepth)) * TransitionScale + 1);

            float2 col = bx == 0 ? colWeight.xy : colWeight.zw;
            float2 row = by == 0 ? rowWeight.xy : rowWeight.zw;
            float4 weight = { (row.y * col.x), (row.y * col.y), (row.x * col.y), (row.x * col.x), };
            visibility += dot(lit, weight);
        }
    }
    return visibility /= 9.0f;
}

float Gather5X5(Texture2D<float> ShadowMap, float2 texelSize, float4 shadowPos, float bias, float invMapSize, float TransitionScale)
{
    float visibility = 0;
    float recieveDepth = shadowPos.z - bias;

    
    float2 texelPos = (shadowPos.xy / invMapSize) - 0.5f;
    float2 base = floor(texelPos);
    float2 fraction = frac(texelPos);

    float2 colWeight[3] = { 1 - fraction.x, 1, 1, 1, 1, fraction.x };
    float2 rowWeight[3] = { 1 - fraction.y, 1, 1, 1, 1, fraction.y };
    

    for (int bx = 0; bx < 3; bx++)
    {
        for (int by = 0; by < 3; by++)
        {
            float2 gatherUV = (base - 1 + (float2(bx, by) * 2)) * invMapSize;

            float4 depth = ShadowMap.Gather(g_SamplePointBorder, gatherUV); // x = 왼쪽 아래, y = 오른쪽 아래, z = 오른쪽 위, w = 왼쪽 위
            

            float4 lit = saturate((depth - (recieveDepth)) * TransitionScale + 1);

            float2 col = bx == 0 ? colWeight[0] : bx == 1 ? colWeight[1] : colWeight[2];
            float2 row = by == 0 ? rowWeight[0] : by == 1 ? rowWeight[1] : rowWeight[2];
            float4 weight = { (row.y * col.x), (row.y * col.y), (row.x * col.y), (row.x * col.x), };
            visibility += dot(lit, weight);
        }
    }
    return visibility /= 25.0f;
}


float ShadowFiltering2D(Texture2D<float> ShadowMap, float2 texelSize, float4 shadowPos, float bias, float invMapSize, float TransitionScale)
{   
    float visibility = 0.0f;

    [branch]
    switch (ShadowQuality)
    {
        case SHADOW_QUALITY_LOW:
        {
            float depth = ShadowMap.SampleLevel(g_SamplePointBorder, shadowPos.xy, 0);
            
            visibility += saturate((depth - (shadowPos.z - bias)) * TransitionScale + 1);
            break;
        }

        case SHADOW_QUALITY_MEDIUM:
        {
            visibility = Gather1X1(ShadowMap, shadowPos, bias, invMapSize, TransitionScale);
            break;   
        }
        
        case SHADOW_QUALITY_HIGH:
        {
            visibility = Gather3X3(ShadowMap, shadowPos, bias, invMapSize, TransitionScale);
            break;
        }
            
        case SHADOW_QUALITY_EPIC:
        {
            visibility = Gather5X5(ShadowMap, texelSize, shadowPos, bias, invMapSize, TransitionScale);
            break;
        }
            
        case SHADOW_QUALITY_BOX3X3:
        {
            for (int x = -1; x <= 1; ++x)
            {
                for (int y = -1; y <= 1; ++y)
                {
                    float2 offset = float2(x, y) * texelSize;
                    float depth = ShadowMap.SampleLevel(g_SamplePointBorder, shadowPos.xy + offset, 0);
                    visibility += saturate((depth - (shadowPos.z - bias)) * TransitionScale + 1);
                }
            }
            visibility /= 9.0f;
            break;
        }
        default:
        {
            float depth = ShadowMap.SampleLevel(g_SamplePointBorder, shadowPos.xy, 0);
            visibility += saturate((depth - (shadowPos.z - bias)) * TransitionScale + 1);
            break;
        }
    }
    
    return visibility;
}

float CalculatePointShadow(float3 worldPos, float3 lightPos, float3 normal, uint index, float invMapSize, float TransitionScale)
{
    if (bShowShadows == 0)
        return 1.0f;
    
    if (PointLights[index].ShadowParameters.w == 0 || bReceiveShadow == 0)
    {
        return 1.0f;
    }

    float3 lightToPixel = worldPos - lightPos;    
    if (length(lightToPixel) > PointLights[index].PositionRadius.w)
    {
        return 1.0f;
    }

    float near = 0.1f;
    float far = PointLights[index].PositionRadius.w;
    float viewDepth = max(abs(lightToPixel.x), max(abs(lightToPixel.y), abs(lightToPixel.z)));
    float projDepth = far / (far - near) - (near * far) / ((far - near) * viewDepth);

    uint faceIndex = 0;
    
    // Find pixel face
    if (viewDepth == abs(lightToPixel.x))
    {
        if (lightToPixel.x > 0)
            faceIndex = 0;
        else
            faceIndex = 1;

    }
    else if (viewDepth == abs(lightToPixel.y))
    {
        if (lightToPixel.y > 0)
            faceIndex = 2;
        else
            faceIndex = 3;
    }
    else
    {
        if (lightToPixel.z > 0)
            faceIndex = 4;
        else
            faceIndex = 5;
    }

    if (PointLights[index].AtlasScaleOffset[faceIndex].x <= 0.0f)
        return 1.0f;

    float4 shadowPos = mul(float4(worldPos.xyz, 1.0f), PointLights[index].ViewProjection[faceIndex]);
    if (shadowPos.w <= 0)
    {
        return 1;
    }
    shadowPos.xyz /= shadowPos.w; // W divde
    shadowPos.x = shadowPos.x * 0.5f + 0.5f;
    shadowPos.y = -shadowPos.y * 0.5f + 0.5f;

    // 뷰포트 밖이면 그림자 없음
    if (shadowPos.x < 0.0f || shadowPos.x > 1.0f || shadowPos.y < 0.0f || shadowPos.y > 1.0f)
    {
        return 1.0f;
    }

    shadowPos.xy = shadowPos.xy * PointLights[index].AtlasScaleOffset[faceIndex].xy + PointLights[index].AtlasScaleOffset[faceIndex].zw;

    if (PointLights[index].ShadowParameters.w == 0.0f || bReceiveShadow == 0)
    {
        return 1.0f;
    }
    
    float NdotL = saturate(dot(normal, -normalize(lightToPixel).xyz));
    float2 texelSize = float2(PointLights[index].ShadowParameters.z, PointLights[index].ShadowParameters.z);

    float bias = PointLights[index].ShadowParameters.x + PointLights[index].ShadowParameters.y * (1.0f - NdotL);

    return ShadowFiltering2D(ShadowMapAtlas, texelSize, shadowPos, bias, invMapSize, TransitionScale);
    //return ShadowFilteringCube(ShadowMapAtlas, lightToPixel, projDepth, bias, invMapSize);

}

FLightResult CalculatePointLight(FPointLightInfo info, float3 worldPos, float3 normal, float3 viewDir, uint index)
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

    FLightResult Result = BlinnPhong(normal, lightDir, viewDir, radiance);

    float Shadow = CalculatePointShadow(worldPos, info.PositionRadius.xyz, normal, index, info.ShadowParameters.z, info.ShadowFilterParameters.y);
    
    Shadow = saturate((Shadow - 0.5) * info.ShadowFilterParameters.x + 0.5);

    Result.Diffuse *= Shadow;
    Result.Specular *= Shadow;
    
    return Result;
}

float CalculateSpotShadow(float3 worldPos, float3 normal, uint index, FSpotLightInfo info)
{
    if (bShowShadows == 0)
        return 1.0f;
    
    float4 shadowPos = mul(float4(worldPos, 1.0f), SpotLights[index].ViewProjection);
    if (shadowPos.w <= 0)
    {
        return 1;
    }
    shadowPos.xyz /= shadowPos.w; // W divde
    shadowPos.x = shadowPos.x * 0.5f + 0.5f;
    shadowPos.y = -shadowPos.y * 0.5f + 0.5f;
    
    // 뷰포트 밖이면 그림자 없음
    if (shadowPos.x < 0.0f || shadowPos.x > 1.0f || shadowPos.y < 0.0f || shadowPos.y > 1.0f)
    {
        return 1.0f;
    }

    shadowPos.xy = shadowPos.xy * SpotLights[index].AtlasScaleOffset.xy + SpotLights[index].AtlasScaleOffset.zw;

    if (SpotLights[index].ShadowParameters.w == 0.0f || bReceiveShadow == 0)
    {
        return 1.0f;
    }
    
    float2 texelSize = float2(SpotLights[index].ShadowParameters.z, SpotLights[index].ShadowParameters.z);

    float3 toLight = SpotLights[index].PositionRadius.xyz - worldPos;
    float distance = length(toLight);
    float3 lightDir = toLight / max(distance, 0.0001f); // length를 한 번만 계산하고 0으로 나누기를 막는다
    
    float NdotL = saturate(dot(normal, lightDir));
    
    float bias = SpotLights[index].ShadowParameters.x + SpotLights[index].ShadowParameters.y * (1.0f - NdotL);
    return ShadowFiltering2D(ShadowMapAtlas, texelSize, shadowPos, bias, info.ShadowParameters.z, info.ShadowFilterParameters.y);
}

FLightResult CalculateSpotLight(FSpotLightInfo info, float3 worldPos, float3 normal, float3 viewDir, uint index)
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

    float Shadow = CalculateSpotShadow(worldPos, normal, index, info);

    Shadow = saturate((Shadow - 0.5) * info.ShadowFilterParameters.x + 0.5);

    
    FLightResult Result = BlinnPhong(normal, lightDir, viewDir, radiance);
    Result.Diffuse = Result.Diffuse * Shadow;
    Result.Specular = Result.Specular * Shadow;
    return Result;
}

float3 CalculateAmbientLight(FAmbientLightInfo info)
{
    return info.ColorIntensity.xyz * info.ColorIntensity.w;
}

bool UseCSM()
{
    return CSMEnabled != 0 && CascadeCount > 0;
}


bool UseCascadeDebug()
{
    return UseCSM() && DebugColorsEnabled != 0;
}

float GetCascadeViewDepth(float3 worldPos)
{
    return dot(worldPos - CameraLocation, CascadeCameraForward.xyz);
}

uint SelectDebugCascade(float3 worldPos)
{
    // float viewDepth = max(dot(worldPos - CameraLocation, CascadeCameraForward.xyz), 0.0f);
    float viewDepth = max(GetCascadeViewDepth(worldPos), 0.0f);

    float4 selectionDepths = CascadeBlendEnabled != 0 ? CascadeBlendEndDepths : CascadeSplitDepths;
    
    uint index = 0;
    index += viewDepth > selectionDepths.x ? 1u : 0u;
    index += viewDepth > selectionDepths.y ? 1u : 0u;
    index += viewDepth > selectionDepths.z ? 1u : 0u;

    return min(index, max(CascadeCount, 1u) - 1u);
}

float3 GetDebugCascadeColor(uint index)
{
    if (index == 0)
        return float3(1, 0, 0);
    
    if (index == 1)
        return float3(0, 1, 0);
    
    if (index == 2)
        return float3(0, 0, 1);
    
    return float3(1, 0, 1);
}

// Cascade 보간
float ApplyCascadeShadowBlend(float3 worldPos, uint cascadeIndex, float currentVisibility, float bias)
{
    if (CascadeBlendEnabled == 0 ||
        cascadeIndex + 1 >= CascadeCount)
    {
        return currentVisibility;
    }

    float viewDepth = GetCascadeViewDepth(worldPos);
    float blendStart = CascadeSplitDepths[cascadeIndex];
    float blendEnd = CascadeBlendEndDepths[cascadeIndex];

    // 전환 구간에 들어오지 않았다면 추가 샘플링 생략
    if (blendEnd <= blendStart || viewDepth <= blendStart)
    {
        return currentVisibility;
    }

    uint nextIndex = cascadeIndex + 1;
    
    float4 nextPos = mul(float4(worldPos, 1.0f), CascadeLightVP[nextIndex]);

    nextPos.xyz /= nextPos.w;
    nextPos.x = nextPos.x * 0.5f + 0.5f;
    nextPos.y = -nextPos.y * 0.5f + 0.5f;

    // 다음 맵에서 유효하지 않은 위치면 현재 결과 유지
    if (any(nextPos.xyz < 0.0f) || any(nextPos.xyz > 1.0f))
    {
        return currentVisibility;
    }

    float2 texelSize = float2(CascadeInvResolution, CascadeInvResolution);

    float nextVisibility = 0.0f;

    // 이중 반복문으로 3*3 위치 방문
    for (int x = -1; x <= 1; ++x)
    {
        for (int y = -1; y <= 1; ++y)
        {
            // 텍셀 단위 이동을 UV 단위로 변환
            float2 offset = float2(x, y) * texelSize;

            float depth = CascadeShadowMap.SampleLevel(g_SamplePointBorder, float3(nextPos.xy + offset, (float) nextIndex), 0);

            nextVisibility += (nextPos.z - bias) <= depth ? 1.0f : 0.0f;
        }
    }
    nextVisibility /= 9.0f;
    // 현재 픽셀이 전환 구간을 얼마나 지나왔는지 0~1로 계산
    float t = saturate((viewDepth - blendStart) / (blendEnd - blendStart));
    
    return lerp(currentVisibility, nextVisibility, t);
    
}

float CalculateDirectionalShadow(float3 worldPos, float3 normal, FDirectionalLightInfo info)
{
    if (bShowShadows == 0)
        return 1.0f;
    
    bool useCascade = UseCSM();
    uint cascadeIndex = useCascade ? SelectDebugCascade(worldPos) : 0u;

    float4 shadowPos;

    if (useCascade)
    {
        shadowPos = mul(float4(worldPos, 1.0f), CascadeLightVP[cascadeIndex]);
    }
    else
    {
        shadowPos = mul(float4(worldPos, 1.0f), DirectionalLight.LightVP);
    }
    
    shadowPos.xyz /= shadowPos.w; // W divde
    shadowPos.x = shadowPos.x * 0.5f + 0.5f;
    shadowPos.y = -shadowPos.y * 0.5f + 0.5f;
    
    // 뷰포트 밖이면 그림자 없음
    if (shadowPos.x < 0.0f || shadowPos.x > 1.0f || shadowPos.y < 0.0f || shadowPos.y > 1.0f || shadowPos.z < 0.0f || shadowPos.z > 1.0f)
    {
        return 1.0f;
    }

    if(DirectionalLight.ShadowParameters.w == 0.0f || bReceiveShadow == 0)
    {
        return 1.0f;
    }

    float invResolution = useCascade ? CascadeInvResolution : DirectionalLight.ShadowParameters.z;

    float2 texelSize = float2(invResolution, invResolution);
    
    float NdotL = saturate(dot(normal, -DirectionalLight.Direction.xyz));
    
    float bias = DirectionalLight.ShadowParameters.x + DirectionalLight.ShadowParameters.y * (1.0f - NdotL);
    
    float visibility = 0.0f;
    

    if (useCascade)
    {// todo 나중에 변경 shadow atlas
        //visibility = ShadowFiltering2D(CascadeShadowMap, texelSize, shadowPos, bias, info.ShadowParameters.z, info.ShadowFilterParameters.y);
        

        for (int x = -1; x <= 1; ++x)
        {
            for (int y = -1; y <= 1; ++y)
            {
                float2 offset = float2(x, y) * texelSize;
                float depth;
                if (useCascade)
                {
                    depth = CascadeShadowMap.SampleLevel(g_SamplePointBorder, float3(shadowPos.xy + offset, (float) cascadeIndex), 0);
                }
                else
                {
                    depth = DirectionalShadowMap.SampleLevel(g_SamplePointBorder, shadowPos.xy + offset, 0);
                }
                visibility += (shadowPos.z - bias) <= depth ? 1.0f : 0.0f;
            }
        }
        visibility /= 9.0f;
        visibility = ApplyCascadeShadowBlend(worldPos, cascadeIndex, visibility, bias);
    }
    else
    {
        visibility = ShadowFiltering2D(DirectionalShadowMap, texelSize, shadowPos, bias, invResolution, info.ShadowFilterParameters.y);
    }
    
    return visibility;
}

FLightResult CalculateDirectionalLight(FDirectionalLightInfo info, float3 normal, float3 viewDir, float3 worldPos)
{
    // Direction은 빛이 나아가는 방향이므로 뒤집어야 표면→라이트 방향이 된다.
    float3 lightDir = normalize(-info.Direction.xyz);
    float3 radiance = info.ColorIntensity.xyz * info.ColorIntensity.w;

    FLightResult Result = BlinnPhong(normal, lightDir, viewDir, radiance);

    float Shadow = CalculateDirectionalShadow(worldPos, normal, info);
    Shadow = saturate((Shadow - 0.5) * info.ShadowFilterParameters.x + 0.5);
    
    Result.Diffuse *= Shadow;
    Result.Specular *= Shadow;
    
    return Result;
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
            uint SpotLightIndex = Id & ~SPOT_FLAG;
            Light = CalculateSpotLight(SpotLights[SpotLightIndex], worldPos, normal, viewDir, SpotLightIndex);
        }
        else
        {
            Light = CalculatePointLight(PointLights[Id], worldPos, normal, viewDir, Id);
        }
        Total.Diffuse += Light.Diffuse;
        Total.Specular += Light.Specular;
    }
    FLightResult Directional = CalculateDirectionalLight(DirectionalLight, normal, viewDir, worldPos);
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
        FLightResult Light = CalculatePointLight(PointLights[i], worldPos, normal, viewDir, i);
        Total.Diffuse += Light.Diffuse;
        Total.Specular += Light.Specular;
    }

    //SpotLight
    for (uint j = 0; j < SpotLightCount; ++j)
    {
        FLightResult Light = CalculateSpotLight(SpotLights[j], worldPos, normal, viewDir, j);
        Total.Diffuse += Light.Diffuse;
        Total.Specular += Light.Specular;
    }

    //Directional Light
    FLightResult Directional = CalculateDirectionalLight(DirectionalLight, normal, viewDir, worldPos);
    Total.Diffuse += Directional.Diffuse;
    Total.Specular += Directional.Specular;

    return Total;
}

float3 ApplyCascadeDebugColor(float3 shadedColor, float3 worldPos, float3 normal)
{
    if (bShowShadows == 0 || !UseCascadeDebug())
        return shadedColor;

    float viewDepth = GetCascadeViewDepth(worldPos);

    if (viewDepth < 0.0f || viewDepth > CascadeShadowDistance)
    {
        return shadedColor;
    }

    // Direction은 빛이 진행하는 방향.
    // 반대로 뒤집어 표면에서 광원으로 향하는 방향을 구한다.
    float3 lightDir = normalize(-DirectionalLight.Direction.xyz);

    float NdotL = dot(normal, lightDir);
    if (NdotL <= 0.0f)
        return shadedColor;
    
    float visibility = CalculateDirectionalShadow(worldPos, normal, DirectionalLight);
    float shadowAmount = saturate(1.0f - visibility);

    uint cascadeIndex = SelectDebugCascade(worldPos);
    float3 debugColor = GetDebugCascadeColor(cascadeIndex);

    return lerp(shadedColor, debugColor, shadowAmount);
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

    finalRGB = ApplyCascadeDebugColor(finalRGB, input.worldPos, normal);    
    return float4(finalRGB, alpha);
}

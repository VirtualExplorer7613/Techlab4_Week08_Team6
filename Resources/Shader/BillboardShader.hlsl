#pragma pack_matrix(row_major)
cbuffer BillboardParams : register(b1)
{
    float4 Tint;
}

cbuffer Viewconstants : register(b0)
{
    matrix VP;
};

cbuffer Worldconstants : register(b2)
{
    matrix World;
};


struct VSInput
{
    float3 Position : POSITION;
    float2 UV : TEXCOORD0;
};

struct PSInput
{
    float4 Position : SV_POSITION;
    float2 UV : TEXCOORD0;
};


Texture2D IconTexture : register(t0);
SamplerState IconSampler : register(s0);

PSInput mainVS(VSInput Input)
{
    PSInput Output;
    float4 WorldPos = mul(float4(Input.Position, 1.0f), World);
    Output.Position = mul(WorldPos, VP);
    Output.UV = Input.UV;
    return Output;
}

float4 mainPS(PSInput Input) : SV_TARGET
{
    float4 Sample = IconTexture.Sample(IconSampler, Input.UV);
    clip(Sample.a - 0.05f);
    return Sample * Tint;
}
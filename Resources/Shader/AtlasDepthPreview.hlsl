Texture2D<float> AtlasDepth : register(t0);
SamplerState PointSampler : register(s0);

struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float4 col : COLOR0;
    float2 uv : TEXCOORD0;
};

// 컴파일 통과용 더미 VS
PS_INPUT mainVS(uint VertexID : SV_VertexID)
{
    PS_INPUT Output;
    float2 UV = float2((VertexID << 1) & 2, VertexID & 2);
    Output.pos = float4(UV * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 1.0f, 1.0f);
    Output.uv = UV;
    Output.col = float4(1, 1, 1, 1);
    return Output;
}

float4 mainPS(PS_INPUT Input) : SV_TARGET
{
    float Depth = AtlasDepth.Sample(PointSampler, Input.uv);

    // 원근 깊이 대비 강조 (0.95~1.0 구간의 물체 윤곽 시각화)
    float Vis = pow(saturate(Depth), 30.0f);

    return float4(Vis, Vis, Vis, 1.0f) * Input.col;
}
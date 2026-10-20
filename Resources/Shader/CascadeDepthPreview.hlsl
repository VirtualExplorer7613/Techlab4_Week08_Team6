Texture2DArray<float> CascadeDepth : register(t0);

struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float4 col : COLOR0;
    float2 uv : TEXCOORD0;
};

// 프로젝트 셰이더 로더의 VS 컴파일용.
// ImGui 미리보기에서는 이 VS를 바인딩하지 않는다.
PS_INPUT mainVS(uint VertexID : SV_VertexID)
{
    PS_INPUT Output;

    float2 UV = float2( (VertexID << 1) & 2, VertexID & 2);

    Output.pos = float4(UV * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f),  0.0f, 1.0f);

    Output.uv = UV;
    Output.col = float4(1, 1, 1, 1);

    return Output;
}

float4 mainPS(PS_INPUT Input) : SV_TARGET
{
    uint Width;
    uint Height;
    uint Layers;
    CascadeDepth.GetDimensions(Width, Height, Layers);

    int2 Pixel = int2(saturate(Input.uv) * float2(Width, Height));

    Pixel = min(Pixel, int2(Width, Height) - 1);

    // Slice별 SRV에는 Layer가 하나뿐이다.
    // 원본 배열의 몇 번째 Slice든 이 View 안에서는 0번이다.
    float Depth = CascadeDepth.Load(int4(Pixel, 0, 0));

    return float4(Depth, Depth, Depth, 1.0f) * Input.col;
}

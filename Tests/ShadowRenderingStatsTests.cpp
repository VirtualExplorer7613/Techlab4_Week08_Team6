#include "EnginePCH.h"
#include "Asset/AssetManager.h"
#include "Component/StaticMeshComponent.h"
#include "Core/Stats/ShadowStats.h"
#include "Engine/Scene.h"
#include "Render/RenderCommand.h"
#include "Render/RenderResourceManager.h"

#include <functional>
#include <stdexcept>

namespace
{
    void Check(bool bCondition, const char* Message)
    {
        if (!bCondition)
            throw std::runtime_error(Message);
    }

    // 테스트에서만 GPU 완료를 기다려 실제 IA 삼각형 수와 계측값을 대조한다.
    // 제품의 Stat 경로에는 Query나 대기를 추가하지 않는다.
    void CheckSubmittedTriangles(const std::function<void()>& Render)
    {
        auto* Context = RenderCommand::GetContext();
        ComPtr<ID3D11Query> Query;
        D3D11_QUERY_DESC Desc{D3D11_QUERY_PIPELINE_STATISTICS, 0};
        Check(SUCCEEDED(RenderCommand::GetDevice()->CreateQuery(&Desc, Query.GetAddressOf())), "pipeline query creation");
        Context->Begin(Query.Get());
        Render();
        Context->End(Query.Get());
        D3D11_QUERY_DATA_PIPELINE_STATISTICS Actual{};
        const uint64 Start = GetTickCount64();
        HRESULT Result;
        while ((Result = Context->GetData(Query.Get(), &Actual, sizeof(Actual), 0)) == S_FALSE)
        {
            Check(GetTickCount64() - Start < 5000, "pipeline query timeout");
            SwitchToThread();
        }
        Check(Result == S_OK, "pipeline query result");
        Check(Actual.IAPrimitives == ShadowStats::Pending.Triangles, "GPU IA triangle count matches stats");
    }
}

int main()
{
    try
    {
        FPlatformTime::InitTiming();
        FRenderDevice Device;
        Check(Device.GetDevice() && Device.GetContext(), "D3D11 device");
        RenderCommand::Init(&Device);
        FRenderResourceManager::Init();
        FGeometryGenerator::CreateDefaultMeshDatas();
        UAssetManager::Get().CreateDefaultTextures();
        UAssetManager::Get().CreateDefaultMaterial();
        UAssetManager::Get().CreateDefaultMeshes();

        // 큰 Light/Cache 배열을 가지므로 테스트 객체는 힙에 둔다.
        auto Renderer = MakeUnique<FRenderer>();
        Check(Renderer->Init(), "renderer init");
        Check(Renderer->RequestCSMSettings(512, 4), "CSM request");
        auto Lights = MakeUnique<FLightConstants>();
        FScene Scene;
        auto* Cube = NewObject<UStaticMeshComponent>();
        Cube->SetStaticMesh(UAssetManager::GetAssetByPath<UStaticMesh>("Cube"));
        Cube->SetRelativeLocation(FVector(5.0f, 0.0f, 0.0f));
        Scene.AddPrimitive(Cube);
        Scene.UpdateAllTransforms();
        const auto* Proxy = Cube->GetSceneProxy();
        Check(Proxy && Proxy->GetRenderState(), "cube proxy");
        const auto& LOD = Proxy->GetLOD(0);
        uint64 MeshTriangles = 0;
        for (uint32 Index = 0; Index < LOD.NumSections; ++Index)
            MeshTriangles += Proxy->GetSection(LOD.FirstSection + Index).IndexCount / 3;

        const FVector Camera(0.0f, 0.0f, 0.0f);
        const FVector Forward(1.0f, 0.0f, 0.0f);
        const FMatrix VP = FMatrix::MakePerspective(1.5707963f, 1.0f, 0.1f, 100.0f);
        const auto RenderAtlas = [&]()
        {
            Renderer->RenderShadowMap(Scene, VP, Camera, 1.0f, 1024.0f, EViewMode::Lit_Phong, false);
        };

        Lights->PointLightCount = 1;
        Lights->PointLights[0].PositionRadius = FVector4(0.0f, 0.0f, 0.0f, 20.0f);
        Lights->PointLights[0].ColorIntensity = FVector4(1.0f);
        Lights->PointLights[0].ShadowParameters.W = 1.0f;
        Lights->PointLights[0].ShadowFilterParameters.W = 0.1f;
        Renderer->SetLight(*Lights);
        ShadowStats::BeginFrame();
        CheckSubmittedTriangles(RenderAtlas);
        Check(ShadowStats::Pending.PointFaces == 1 && ShadowStats::Pending.Passes == 1,
            "only point +X face has caster");
        Check(ShadowStats::Pending.DrawCalls == LOD.NumSections + 1 && ShadowStats::Pending.TileClearDraws == 1,
            "point mesh draws plus tile clear");
        Check(ShadowStats::Pending.Triangles == MeshTriangles + 1 && ShadowStats::Pending.CasterSubmissions == 1,
            "point triangles and caster submission");
        Renderer->RecordShadowViewStats(true);
        Renderer->CompleteShadowStatsFrame();
        Check(ShadowStats::Completed.AtlasTiles == 1 && ShadowStats::Completed.UpdatedLights[1] == 1,
            "point atlas tile and updated light");
        Check(ShadowStats::Completed.EstimatedBytes > 256ull * 1024 * 1024, "resident memory includes atlas and other maps");

        ShadowStats::BeginFrame();
        CheckSubmittedTriangles(RenderAtlas);
        Check(ShadowStats::Pending.DrawCalls == 0 && ShadowStats::Pending.Passes == 0,
            "cache hit has no shadow draw pass");
        Renderer->RecordShadowViewStats(true);
        Renderer->CompleteShadowStatsFrame();
        Check(ShadowStats::Completed.AtlasTiles == 1 && ShadowStats::Completed.UpdatedLights[1] == 0,
            "cached allocation remains resident without updates");

        Renderer->InvalidateAllAtlasCaches();
        Lights->PointLightCount = 0;
        Lights->SpotLightCount = 1;
        Lights->SpotLights[0].PositionRadius = FVector4(0.0f, 0.0f, 0.0f, 20.0f);
        Lights->SpotLights[0].ColorIntensity = FVector4(1.0f);
        Lights->SpotLights[0].ShadowParameters.W = 1.0f;
        Lights->SpotLights[0].ViewProjection = VP;
        Renderer->SetLight(*Lights);
        ShadowStats::BeginFrame();
        CheckSubmittedTriangles(RenderAtlas);
        Check(ShadowStats::Pending.SpotTiles == 1 && ShadowStats::Pending.PointFaces == 0,
            "spot tile is distinct from point face");
        Renderer->RecordShadowViewStats(true);
        Renderer->CompleteShadowStatsFrame();
        Check(ShadowStats::Completed.AtlasTiles == 1, "spot partial atlas");
        const auto Spot = Renderer->GetSpotLightData(0);
        const uint64 InnerSize = static_cast<uint64>(std::round(Spot.AtlasScaleOffset.X * ShadowStats::Completed.AtlasResolution));
        Check(ShadowStats::Completed.AtlasAllocatedTexels == (InnerSize + 2 * SHADOW_ATLAS_BORDER) *
            (InnerSize + 2 * SHADOW_ATLAS_BORDER), "spot resource area includes border");

        // Receiver 없음에 대응하는 기존 진입 조건: 깊이 렌더링은 생략하고 Light Buffer는 갱신한다.
        Renderer->SetShadowsEnabled(false);
        ShadowStats::BeginFrame();
        CheckSubmittedTriangles(RenderAtlas);
        Renderer->RecordShadowViewStats(false);
        Check(ShadowStats::Pending.Passes == 0 && ShadowStats::Pending.ShadowViews == 0, "no receiver / shadows disabled");
        Renderer->SetShadowsEnabled(true);

        for (uint32 Mode = 0; Mode < 2; ++Mode)
        {
            Lights->SpotLights[0].ShadowParameters.W = Mode == 0 ? 0.0f : 1.0f;
            Lights->SpotLights[0].ColorIntensity.W = Mode == 0 ? 1.0f : 0.0f;
            Renderer->SetLight(*Lights);
            ShadowStats::BeginFrame();
            CheckSubmittedTriangles(RenderAtlas);
            Renderer->RecordShadowViewStats(true);
            Check(ShadowStats::Pending.Passes == 0 && ShadowStats::Pending.EligibleLights[2] == 0,
                "CastShadow off or zero intensity");
        }

        Lights->SpotLights[0].ColorIntensity.W = 1.0f;
        Lights->SpotLights[0].PositionRadius = FVector4(50.0f, 0.0f, 0.0f, 0.1f);
        Renderer->SetLight(*Lights);
        ShadowStats::BeginFrame();
        CheckSubmittedTriangles(RenderAtlas);
        Check(ShadowStats::Pending.Passes == 0, "tiny screen light skips shadow pass");

        Lights->SpotLightCount = 0;
        Lights->DirectionalLight.ColorIntensity = FVector4(1.0f);
        Lights->DirectionalLight.Direction = FVector4(0.0f, 0.0f, -1.0f, 0.0f);
        Lights->DirectionalLight.ShadowParameters.W = 1.0f;
        Lights->DirectionalLight.ViewProjection = VP;
        Renderer->SetLight(*Lights);
        const auto RenderDirectional = [&](bool bOrthographic)
        {
            Renderer->PrepareCascadeShadows(Scene, VP, Camera, Forward, 0.1f, 100.0f,
                bOrthographic, EViewMode::Lit_Phong, FMatrix::Identity, 1.0f, 1.0f);
            if (!Renderer->IsCSMActive())
                Renderer->RenderDirectionalShadowMap(Scene, VP, Camera, 0.1f, 100.0f,
                    bOrthographic, EViewMode::Lit_Phong, FMatrix::Identity, 1.0f, 1.0f);
            Renderer->RecordShadowViewStats(true);
        };
        ShadowStats::BeginFrame();
        CheckSubmittedTriangles([&]() { RenderDirectional(false); });
        Check(ShadowStats::Pending.ActiveCascades == 4 && ShadowStats::Pending.CascadePasses > 0,
            "CSM active");
        Check(ShadowStats::Pending.DirectionalPasses == 0, "CSM skips ordinary directional");
        Renderer->CompleteShadowStatsFrame();
        const uint64 CSMMemory = ShadowStats::Completed.EstimatedBytes;

        ShadowStats::BeginFrame();
        CheckSubmittedTriangles([&]() { RenderDirectional(true); });
        Check(ShadowStats::Pending.ActiveCascades == 0 && ShadowStats::Pending.DirectionalPasses == 1,
            "orthographic falls back while CSM setting is enabled");

        Renderer->SetCSMEnabled(false);
        ShadowStats::BeginFrame();
        CheckSubmittedTriangles([&]() { RenderDirectional(false); RenderDirectional(true); });
        Check(ShadowStats::Pending.DirectionalPasses == 2 && ShadowStats::Pending.Views == 2 &&
            ShadowStats::Pending.ActiveCascades == 0, "multi-view directional and orthographic");
        Check(ShadowStats::Pending.CasterSubmissions == 2, "caster submissions repeated across views");
        Renderer->CompleteShadowStatsFrame();
        Check(ShadowStats::Completed.EstimatedBytes == CSMMemory, "disabled CSM resource remains allocated");

        Scene.RemoveAllPrimitives();
        delete Cube;
        std::cout << "PASS: actual D3D11 triangles, point partial faces/cache, spot border, skipping, CSM, ortho, multi-view, memory\n";
        return 0;
    }
    catch (const std::exception& Error)
    {
        std::cerr << "FAIL: " << Error.what() << '\n';
        return 1;
    }
}


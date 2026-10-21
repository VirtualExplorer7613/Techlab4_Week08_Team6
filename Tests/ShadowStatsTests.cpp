#include "EnginePCH.h"
#include "Core/Stats/ShadowStats.h"
#include "Core/Stats/StatOverlay.h"
#include "Render/Shadow/ShadowRenderer.h"

#include <stdexcept>

namespace
{
    DECLARE_CYCLE_STAT("Test Shadow", STAT_TestShadow);
    DECLARE_CYCLE_STAT("Test Picking", STAT_TestPicking);

    void Check(bool bCondition, const char* Message)
    {
        if (!bCondition)
            throw std::runtime_error(Message);
    }

    void TestFrameAggregation()
    {
        FStatRegistry::Reset();
        // 같은 프레임의 여러 View 호출은 하나의 프레임 샘플로 합산한다.
        FStatRegistry::AddCycles(STAT_TestShadow, 40);
        FStatRegistry::AddCycles(STAT_TestShadow, 80);
        FStatRegistry::AddCycles(STAT_TestPicking, 10);
        FStatRegistry::EndFrame();
        const FCycleStatData* Data = FStatRegistry::Find(STAT_TestShadow);
        Check(Data && Data->LastCycles == 120 && Data->SampleCount == 1, "multi-view frame sum");
        Check(Data->CallCount == 2 && Data->MaxFrameCycles == 120, "actual call count and max");

        // 실행 직후 완전히 생략된 프레임: Last=0, 최근 평균=60, 누적 호출 수 유지.
        FStatRegistry::EndFrame();
        Check(Data->LastCycles == 0 && Data->SampleCount == 2, "skipped Last and frame sample");
        Check(Data->WindowSumCycles == 120 && Data->CallCount == 2, "skipped frame does not count as call");
        Check(std::abs(Data->GetRecentAverageMs() - FPlatformTime::ToMilliseconds64(60)) < 1e-12,
            "recent average includes skipped frame");
        const auto* Picking = FStatRegistry::Find(STAT_TestPicking);
        Check(Picking->CallCount == 1 && Picking->TotalCycles == 10 && Picking->LastCycles == 0,
            "picking attempts preserved");

        for (uint32 Index = 0; Index < 118; ++Index)
            FStatRegistry::EndFrame();
        Check(Data->SampleCount == 120 && Data->WindowSumCycles == 120, "120 frame window");
        FStatRegistry::EndFrame();
        Check(Data->WindowSumCycles == 0 && Data->GetRecentAverageMs() == 0.0, "old sample expires at frame 121");
        Check(Data->MaxFrameCycles == 120 && Data->TotalCycles == 120, "lifetime max and total preserved");

        FStatRegistry::SetEnabled(false);
        {
            FScopeCycleCounter Disabled(STAT_TestShadow);
        }
        {
            FScopeCycleCounter Always(STAT_TestPicking, true);
            Always.Finish();
            Always.Finish();
        }
        FStatRegistry::EndFrame();
        Check(Data->CallCount == 2 && Picking->CallCount == 2, "disabled and always scopes, Finish idempotence");
        FStatRegistry::SetEnabled(true);
        FStatRegistry::AddCycles(STAT_TestShadow, 999);
        FStatRegistry::Reset();
        FStatRegistry::EndFrame();
        Check(Data->LastCycles == 0 && Data->TotalCycles == 0 && Data->CallCount == 0,
            "reset also discards pending measurements");
    }

    void TestAtlasAllocation()
    {
        FShadowAtlasAllocator Allocator;
        Allocator.Init(1024, 64);
        FShadowAtlasTile PointFace;
        FShadowAtlasTile SpotTile;
        Check(Allocator.Allocate(512, PointFace), "allocate point face");
        Check(Allocator.Allocate(256, SpotTile), "allocate spot tile");
        const uint64 ExpectedArea = 512ull * 512 + 256ull * 256;
        Check(Allocator.GetAllocatedTileCount() == 2 && Allocator.GetAllocatedTexels() == ExpectedArea,
            "full allocated area includes border");
        Check(100.0 * Allocator.GetAllocatedTexels() / (1024ull * 1024) == 31.25, "partial atlas utilization");
        FShadowAtlasTile Failed;
        Check(!Allocator.Allocate(1024, Failed), "allocation fails when occupied");
        Check(Allocator.GetAllocatedTexels() == ExpectedArea, "failed allocation leaves stats unchanged");
        Allocator.Free(PointFace.NodeIndex);
        Check(Allocator.GetAllocatedTileCount() == 1 && Allocator.GetAllocatedTexels() == 256ull * 256,
            "free point face");
        Allocator.Free(PointFace.NodeIndex);
        Allocator.Free(-1);
        Check(Allocator.GetAllocatedTileCount() == 1, "repeated and invalid free do not decrement twice");
        Allocator.Free(SpotTile.NodeIndex);
        Check(Allocator.GetAllocatedTileCount() == 0 && Allocator.GetAllocatedTexels() == 0,
            "recursive free does not count split nodes");
        Check(Allocator.Allocate(1024, PointFace), "merged root can be allocated");
        Check(Allocator.GetAllocatedTexels() == 1024ull * 1024, "full atlas");
        Allocator.Reset();
        Check(Allocator.GetAllocatedTileCount() == 0 && Allocator.GetAllocatedTexels() == 0, "reset allocator stats");
    }

    void TestFramePublication()
    {
        ShadowStats::BeginFrame();
        ShadowStats::Pending.DrawCalls = 5;
        ShadowStats::Pending.PointFaces = 2;
        ShadowStats::EndFrame();
        ShadowStats::BeginFrame();
        Check(ShadowStats::Completed.DrawCalls == 5 && ShadowStats::Pending.DrawCalls == 0,
            "overlay reads completed frame while next frame collects");
        ShadowStats::EndFrame();
        Check(ShadowStats::Completed.DrawCalls == 0 && ShadowStats::Completed.PointFaces == 0,
            "fully skipped workload is zero next frame");
        Check(ShadowStats::IsShadowStat(ShadowStats::STAT_ShadowTotal), "profile hides total when shadow overlay is enabled");
    }

    void TestDemoCounters()
    {
        ShadowStats::BeginFrame();
        int PointLight = 0;
        int SpotLight = 0;
        ShadowStats::RecordLight(&PointLight, 1);
        ShadowStats::RecordLight(&PointLight, 1); // 동일 광원의 두 View
        ShadowStats::RecordLight(&SpotLight, 2);
        ShadowStats::RecordCasterQuery(10, 3);
        ShadowStats::RecordCasterQuery(10, 0); // 다른 Face에서 전부 제외
        ShadowStats::EndFrame();
        Check(ShadowStats::Completed.Lights[1] == 1 && ShadowStats::Completed.Lights[2] == 1,
            "shadow light count must not multiply with views");
        Check(ShadowStats::Completed.CastersTested == 20 && ShadowStats::Completed.CastersCulled == 17,
            "caster cull counts excluded candidates per actual query");
        ShadowStats::BeginFrame();
        ShadowStats::RecordLight(&PointLight, 1);
        ShadowStats::EndFrame();
        Check(ShadowStats::Completed.CastersTested == 0 && ShadowStats::Completed.CastersCulled == 0,
            "cached pass without query must not claim caster culling");
        Check(ShadowStats::Completed.Lights[1] == 1 && ShadowStats::Completed.Lights[2] == 0,
            "unique light set resets per frame");

        FStatOverlay::ResetSamples();
        FStatOverlay::RecordFrame(100);
        Check(FStatOverlay::GetFrameTiming().LastCycles == 100, "frame stats store unclamped raw cycles");
        FStatOverlay::RecordFrame(0);
        Check(FStatOverlay::GetFrameTiming().SampleCount == 2 &&
            FStatOverlay::GetFrameTiming().WindowSumCycles == 100 &&
            FStatOverlay::GetFrameTiming().MaxFrameCycles == 100, "frame Last Avg Max include zero sample");
        const uint64 Generation = FStatOverlay::GetResetGeneration();
        FStatOverlay::ResetSamples();
        Check(FStatOverlay::GetFrameTiming().SampleCount == 0 &&
            FStatOverlay::GetResetGeneration() != Generation, "reset clears frame samples and notifies GPU profiler");
    }
}

int main()
{
    try
    {
        FPlatformTime::InitTiming();
        TestFrameAggregation();
        TestAtlasAllocation();
        TestFramePublication();
        TestDemoCounters();
        std::cout << "PASS: frame aggregation, skipped frames, 120-frame window, Picking, atlas, unique lights, caster queries, raw frame time/reset\n";
        return 0;
    }
    catch (const std::exception& Error)
    {
        std::cerr << "FAIL: " << Error.what() << '\n';
        return 1;
    }
}

#pragma once

#include "LightweightStats.h"

namespace ShadowStats
{
DECLARE_CYCLE_STAT("Shadow CPU Total", STAT_ShadowTotal);
DECLARE_CYCLE_STAT("Shadow Directional", STAT_ShadowDirectional);
DECLARE_CYCLE_STAT("Shadow Atlas", STAT_ShadowAtlas);
DECLARE_CYCLE_STAT("Shadow CSM", STAT_ShadowCSM);

// 작업량은 모든 View의 제출 횟수를 합산한다. 고유 객체/광원 수가 아니다.
// 리소스는 공유되므로 프레임 끝의 실제 할당 상태를 한 번만 기록한다.
struct FFrameData
{
    uint64 DrawCalls = 0;
    uint64 Triangles = 0;
    uint64 CasterSubmissions = 0;
    uint32 Passes = 0;
    uint32 DirectionalPasses = 0;
    uint32 CascadePasses = 0;
    uint32 PointFaces = 0;
    uint32 SpotTiles = 0;
    uint32 TileClearDraws = 0;
    uint32 DepthClears = 0;
    uint32 Views = 0;
    uint32 ShadowViews = 0;
    uint32 EligibleLights[3] = {}; // Directional / Point / Spot, CastShadow 및 Intensity > 0
    uint32 UpdatedLights[3] = {};  // 이번 프레임에 맵을 갱신한 광원-View 수 (초기화 포함)
    uint32 ActiveCascades = 0;     // View별 활성 개수의 최댓값
    uint32 CSMViews = 0;
    uint32 CascadeResolution = 0;
    uint32 ConfiguredCascades = 0;
    bool bCSMEnabled = false;
    float ShadowDistance = 0.0f;
    uint32 DirectionalResolution = 0;
    uint32 AtlasResolution = 0;
    uint32 AtlasTiles = 0;
    uint64 AtlasAllocatedTexels = 0;
    uint64 EstimatedBytes = 0;
    uint64 PreviewBytes = 0;
    bool bResourcesAvailable = false;
};

inline FFrameData Pending;
inline FFrameData Completed;

inline void BeginFrame()
{
    Pending = {};
}

inline void EndFrame()
{
    Completed = Pending;
}

inline bool IsShadowStat(TStatId StatId)
{
    return StatId == STAT_ShadowTotal || StatId == STAT_ShadowDirectional ||
        StatId == STAT_ShadowCSM || StatId == STAT_ShadowAtlas;
}
} // namespace ShadowStats

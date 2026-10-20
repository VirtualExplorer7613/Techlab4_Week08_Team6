#pragma once

#include "LightweightStats.h"

namespace ShadowStats
{
DECLARE_CYCLE_STAT("Shadow Directional", STAT_ShadowDirectional);
DECLARE_CYCLE_STAT("Shadow Atlas", STAT_ShadowAtlas);
DECLARE_CYCLE_STAT("Shadow CSM", STAT_ShadowCSM);
} // namespace ShadowStats

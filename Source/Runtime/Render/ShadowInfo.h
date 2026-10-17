#pragma once

#include "./Core/Types.h"
#include "Math/Matrix.h"
#include "Math/Vector2.h"

struct FDirectionalShadowInfo
{
    bool bEnabled = false;

    FMatrix LightView = FMatrix::Identity;
    FMatrix LightProjection = FMatrix::Identity;
    FMatrix LightViewProjection = FMatrix::Identity;

    uint32 Resolution = 1024;

    float DepthBias = 0.0f;

};

struct alignas(16) FShadowConstants
{
    FMatrix LightViewProjection = FMatrix::Identity;

    FVector2 InvShadowMapSize = FVector2(0.0f, 0.0f);
    float DepthBias = 0.0f;
    uint32 bEnabled = 0;
};

struct alignas(16) FShadowObjectConstants
{
    FMatrix World = FMatrix::Identity;
};

static_assert(sizeof(FShadowObjectConstants) == 64);

static_assert(sizeof(FShadowConstants) == 80, "FShadowConstants must match the HLSL constant buffer layout.");
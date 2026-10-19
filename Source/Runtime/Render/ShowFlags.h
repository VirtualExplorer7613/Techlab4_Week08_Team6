#pragma once

#include "Core/Types.h"

enum class EShowFlags : uint32
{
    None = 0,
    Primitives = 1 << 0,
    BoundingBox = 1 << 1,
    UUID = 1 << 2,
    BatchLine = 1 << 3,
    PSGrid = 1 << 4,
    ExponentialHeightFog = 1 << 5,
    Shadows = 1 << 6,
};

DEFINE_ENUM_OPERATORS(EShowFlags)

#pragma once

#include <array>
#include <limits>

#include "Math/EngineMath.h"
#include "Math/Box.h"
#include "Math/Vector.h"

#include "Containers/Array.h"

#include "Render/Texture2D.h"
#include "Render/TextureCube.h"

#define SHADOW_ATLAS_MAP_SIZE 1 << 14
#define SHADOW_LOD_LEVEL_1 1024
#define SHADOW_LOD_LEVEL_2 512
#define SHADOW_LOD_LEVEL_3 256
#define SHADOW_ATLAS_BORDER 5

struct FShadowPassDesc
{
    ID3D11DepthStencilView *DepthView = nullptr;
    uint32 Width = 0;
    uint32 Height = 0;
    uint32 X = 0;
    uint32 Y = 0;
};

const FCubeFaceDirection CubeFaceDirections[6] = {
    {FVector(1.0f, 0.0f, 0.0f), FVector(0.0f, 1.0f, 0.0f)},  // +X
    {FVector(-1.0f, 0.0f, 0.0f), FVector(0.0f, 1.0f, 0.0f)}, // -X
    {FVector(0.0f, 1.0f, 0.0f), FVector(0.0f, 0.0f, -1.0f)}, // +Y
    {FVector(0.0f, -1.0f, 0.0f), FVector(0.0f, 0.0f, 1.0f)}, // -Y
    {FVector(0.0f, 0.0f, 1.0f), FVector(0.0f, 1.0f, 0.0f)},  // +Z
    {FVector(0.0f, 0.0f, -1.0f), FVector(0.0f, 1.0f, 0.0f)}  // -Z
};

// For returning result
struct FShadowAtlasTile
{
    uint32 X = 0;
    uint32 Y = 0;
    uint32 Size = 0;
};

enum class EShadowNodeState
{
    Free,
    Split,
    Occupied
};

struct FShadowQuadNode
{
    uint32 X = 0;
    uint32 Y = 0;
    uint32 Size = 0;
    EShadowNodeState ShdowNodeState = EShadowNodeState::Free;
    int32 Children[4] = {-1, -1, -1, -1};
};

class FShadowAtlasAllocator
{
public:
    void Init(uint32 InAtlasSize, uint32 InMinTileSize = 128)
    {
        AtlasSize = InAtlasSize;
        MinTileSize = InMinTileSize;

        uint32 MaxDepth = log2(AtlasSize / MinTileSize);
        uint32 MaxCount = (pow(4, MaxDepth + 1) - 1) / 3;

        Nodes.Reserve(MaxCount);

        Reset();
    }

    void Reset()
    {
        Nodes.Reset();

        // Root
        Nodes.Add({0, 0, AtlasSize, EShadowNodeState::Free});
    }

    bool Allocate(uint32 RequestedSize, FShadowAtlasTile& OutTile)
    {
        uint32 NewRequestedSize = FMath::Clamp(RequestedSize, MinTileSize, AtlasSize);

        if ((NewRequestedSize & (NewRequestedSize - 1)) != 0)
        {
            NewRequestedSize = MinTileSize;
            while (NewRequestedSize <= RequestedSize)
            {
                NewRequestedSize *= 2;

                if (NewRequestedSize > RequestedSize)
                {
                    NewRequestedSize /=2;
                    break;
                }
            }
        }

        return AllocateNode(0, NewRequestedSize, OutTile);
    }

    uint32 GetAtlasSize()
    {
        return AtlasSize;
    }

    uint32 GetMinTileSize()
    {
        return MinTileSize;
    }

private:
    bool AllocateNode(int32 NodeIndex, uint32 RequestedSize, FShadowAtlasTile& OutTile)
    {
        if (Nodes[NodeIndex].ShdowNodeState == EShadowNodeState::Occupied || Nodes[NodeIndex].Size < RequestedSize)
        {
            return false;
        }

        if (Nodes[NodeIndex].ShdowNodeState == EShadowNodeState::Free)
        {
            if (Nodes[NodeIndex].Size == RequestedSize)
            {
                Nodes[NodeIndex].ShdowNodeState = EShadowNodeState::Occupied;
                OutTile.X = Nodes[NodeIndex].X;
                OutTile.Y = Nodes[NodeIndex].Y;
                OutTile.Size = Nodes[NodeIndex].Size;

                return true;
            }
            else if (Nodes[NodeIndex].Size > RequestedSize)
            {
                Nodes[NodeIndex].ShdowNodeState = EShadowNodeState::Split;

                uint32 ParentX = Nodes[NodeIndex].X;
                uint32 ParentY = Nodes[NodeIndex].Y;
                uint32 ParentHalfSize = Nodes[NodeIndex].Size / 2;
                    
                for (uint32 i = 0; i < 4; i++)
                {
                    uint32 ChildX = ParentX + (i % 2) * ParentHalfSize;
                    uint32 ChildY = ParentY + (i / 2) * ParentHalfSize;
                    uint32 ChildSize = ParentHalfSize;

                    int32 ChildIndex = Nodes.Add({ChildX, ChildY, ChildSize, EShadowNodeState::Free});
                    Nodes[NodeIndex].Children[i] = ChildIndex;
                }
            }
        }

        if (Nodes[NodeIndex].ShdowNodeState == EShadowNodeState::Split)
        {
            for (uint32 i = 0; i < 4; i++)
            {
                bool bHasAllocated = AllocateNode(Nodes[NodeIndex].Children[i], RequestedSize, OutTile);

                if (bHasAllocated)
                {
                    return true;
                }
            }
        }

        return false;
    }

    TArray<FShadowQuadNode> Nodes;
    uint32 AtlasSize = 0;
    uint32 MinTileSize = 0;
};

class FShadowRenderer
{
public:
    bool Init(uint32 ShadowMapSize = 2048);

    bool BeginShadowPass(const FShadowPassDesc &Desc);
    void SetShadowViewPort(const uint32 X, const uint32 Y, const uint32 Width, const uint32 Height);
    void EndShadowPass();

    // Frustum 중심과 광원 방향을 기준으로 Light View 행렬을 구성한다.
    FMatrix BuildDirectionalLightView(const std::array<FVector, 8>& FrustumCornersWorld, const FVector& LightDirection) const;

    // Frustum의 8개 꼭짓점을 Light Space로 변환해 경계 상자를 구한다.
    FBox CalculateLightSpaceBounds(const std::array<FVector, 8>& FrustumCornersWorld, const FMatrix& LightView) const;

    // Light Space의 X축은 깊이, Y/Z축은 화면 평면으로 사용해 직교투영 행렬을 구성한다.
    FMatrix BuildDirectionalLightProjection(const FBox& LightSpaceBounds) const;

    // 카메라 Frustum을 지정된 그림자 거리까지 제한해 Directional Light VP를 계산한다.
    FMatrix BuildDirectionalLightVP(const FMatrix& CameraVP, float CameraNear, float CameraFar, float ShadowDistance, const FVector& LightDirection) const;

    // Frustum 코너 8개를 받아 Directional Light VP를 계산한다.
    FMatrix BuildDirectionalLightVPFromCorners(const std::array<FVector, 8>& FrustumCornersWorld, const FVector& LightDirection, float MinCasterDepth = std::numeric_limits<float>::infinity()) const;

    // Frustum의 원거리 꼭짓점을 ShadowDistance에 맞춰 보간한다.
    std::array<FVector, 8> LimitFrustumToShadowDistance(const std::array<FVector, 8>& Corners, float CameraNear, float CameraFar, float ShadowDistance) const;

    FMatrix BuildPointLightView(const FVector &LightPosition, uint32 FaceIndex) const;
    FMatrix BuildPointLightProjection(float NearPlane, float FarPlane) const;
    FMatrix BuildPointLightVP(const FVector &LightPosition, float NearPlane, float FarPlane, uint32 FaceIndex) const;

    FTexture2D *GetDirectionalShadowMap() const { return DirectionalShadowMap.get(); }
    FTexture2D *GetSpotShadowMap() const { return SpotShadowMap.get(); }
    FTextureCube* GetPointLightShadowMap() const { return PointLightShadowMap.get(); }
    FTexture2D* GetShadowMapAtlas() const { return ShadowMapAtlas.get(); }

    FShadowAtlasAllocator& GetShadowAtlasAllocator() { return ShadowAtlasAllocator; }

private:
    FShadowAtlasAllocator ShadowAtlasAllocator;

    TUniquePtr<FTexture2D> DirectionalShadowMap;
    TUniquePtr<FTextureCube> PointLightShadowMap;    
    TUniquePtr<FTexture2D> SpotShadowMap;

    TUniquePtr<FTexture2D> ShadowMapAtlas;
};

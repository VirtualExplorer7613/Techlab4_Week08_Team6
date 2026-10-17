#pragma once

#include "Core/Types.h"
#include "Buffer.h"
#include "Texture2D.h"
#include "RenderPacket.h"
#include "ShadowInfo.h"

class FShadowRenderer
{
public:
	FShadowRenderer() = default;
	~FShadowRenderer() = default;

	FShadowRenderer(const FShadowRenderer&) = delete;

	FShadowRenderer& operator=(const FShadowRenderer&) = delete;

	bool Init(uint32 Resolution = 1024);

	FTexture2D* GetShadowMap() const
	{
		return ShadowMap.get();
	}

	uint32 GetResolution() const
	{
		return ShadowMap ? ShadowMap->GetWidth() : 0;
	}

	void SetDirectionalShadowInfo(const FDirectionalShadowInfo& Info)
	{
		DirectionalShadowInfo = Info;
		// 새 광원 / 뷰의 정보가 들어왔으므로 다시 렌더해야 한다.
		bShadowMapReady = false;
	}

	const FDirectionalShadowInfo& GetDirectionalShadowInfo() const
	{
		return DirectionalShadowInfo;
	}

	void Render(const FRenderQueue& ShadowPackets);
	void BindForLighting();

private:
	TUniquePtr<FTexture2D> ShadowMap;
	TUniquePtr<FConstantBuffer> ShadowCB;
	TUniquePtr<FConstantBuffer> ShadowObjectCB;
	FDirectionalShadowInfo DirectionalShadowInfo{};

	bool bShadowMapReady = false;

};
#pragma once

#include <d3d11.h>
#include "Asset/RenderAsset.h"
#include "Render/RenderDevice.h"
#include "Texture.h"
#include "ImageLoader.h"

class FTexture2D : public FTexture
{
public:
	// RowPitch가 0이면 InDesc.Format에서 계산한다.
	FTexture2D(ID3D11Device* Device, const D3D11_TEXTURE2D_DESC& InDesc, const void* InitialData, uint32 RowPitch = 0);
	// 로더가 포맷과 픽치까지 알고 있으므로 그대로 받는다.
	FTexture2D(ID3D11Device* Device, const D3D11_TEXTURE2D_DESC& InDesc, const FImageData& Image);
	// For Swapchain
	FTexture2D(ID3D11Device* Device, ComPtr<ID3D11Resource> SwapchainTexture, const D3D11_TEXTURE2D_DESC& InDesc);
	~FTexture2D() = default;

	// 깊이 텍스처를 slice 1장짜리 Texture2DArray로 읽는 SRV. 그림자 필터가 CSM 배열과 같은 함수를 쓰기 위해 둔다.
	ID3D11ShaderResourceView* GetArraySRV() const { return ArraySRV.Get(); }

private:
	void CreateViews(ID3D11Device* Device, const D3D11_TEXTURE2D_DESC& InDesc);

	ComPtr<ID3D11ShaderResourceView> ArraySRV;
};

class UTexture2D : public URenderAsset
{
	DECLARE_CLASS(UTexture2D, URenderAsset)

	friend class UAssetManager;
public:
	UTexture2D();
	~UTexture2D();

	uint32 GetWidth()  const { return Texture ? Texture->GetWidth() : 0; }
	uint32 GetHeight() const { return Texture ? Texture->GetHeight() : 0; }

	FTexture2D* GetResource() const { return Texture.get(); }
private:
	void SetResource(TUniquePtr<FTexture2D> InResource) { Texture = std::move(InResource); }

	TUniquePtr<FTexture2D> Texture;

};


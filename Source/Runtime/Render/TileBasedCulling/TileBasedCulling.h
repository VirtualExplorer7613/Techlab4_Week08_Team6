#pragma once

#include "Render/Buffer.h"
#include <d3d11.h>

struct FTileCullInputs
{
	ID3D11ShaderResourceView* DepthSRV = nullptr;
	uint32 Width = 0;
	uint32 Height = 0;

	FMatrix View;
	FMatrix Projection;

	ID3D11ShaderResourceView* PointLightSRV = nullptr;
	ID3D11ShaderResourceView* SpotLightSRV = nullptr;
	uint32 PointLightCount = 0;
	uint32 SpotLightCount = 0;
};

class FTileBasedCulling
{
public:
	bool Init();

	bool Run(const FTileCullInputs& Inputs);

	ID3D11ShaderResourceView* GetTileHeaderSRV() const { return TileHeaderSRV.Get(); }
	ID3D11ShaderResourceView* GetTileLightIndicesSRV() const { return TileLightIndicesSRV.Get(); }

	void BindResultToPs() const;
	void UnbindResultFromPs() const;

private:
	ComPtr<ID3D11ComputeShader> TileBasedCullingCS;

	ComPtr<ID3D11Buffer> TileBasedConstantsCB;

	ComPtr<ID3D11Buffer> TileHeaderBuffer;
	ComPtr<ID3D11ShaderResourceView> TileHeaderSRV;
	ComPtr<ID3D11UnorderedAccessView> TileHeaderUAV;

	ComPtr<ID3D11Buffer> TileLightIndicesBuffer;
	ComPtr<ID3D11ShaderResourceView> TileLightIndicesSRV;
	ComPtr<ID3D11UnorderedAccessView> TileLightIndicesUAV;

	uint32 TileHeaderCapacity = 0;

	bool EnsureTileCapacity(uint32 TileCount);
};
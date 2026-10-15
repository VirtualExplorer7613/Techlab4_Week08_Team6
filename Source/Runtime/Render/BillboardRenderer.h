#pragma once
#include "Render/Buffer.h"
#include "Render/Renderer.h"

class UBillboardComponent;

// 빌보드를 렌더 큐를 거치지 않고 오버레이 패스에서 직접 그린다.
// 렌더 패스는 호출하는 쪽이 연다.
class FBillboardRenderer
{
public:
	FBillboardRenderer() = default;
	~FBillboardRenderer() = default;

	FBillboardRenderer(const FBillboardRenderer&) = delete;
	FBillboardRenderer& operator=(const FBillboardRenderer&) = delete;

	bool Init();

	// 빌보드마다 View·World 상수를 채우기 전에 한 번 호출한다.
	void BeginRender(const FMatrix& ViewProjection, const FVector& CameraPosition);
	// BillboardWorld는 View별로 카메라를 향하게 계산한 행렬이다.
	void OnRender(const UBillboardComponent& Billboard, const FMatrix& BillboardWorld);

private:
	TUniquePtr<FConstantBuffer> ViewCB;
	TUniquePtr<FConstantBuffer> PerObjectCB;
};

#include "EnginePCH.h"

#include "BillboardRenderer.h"
#include "RenderCommand.h"
#include "Mesh.h"
#include "Shader.h"
#include "Component/BillboardComponent.h"

bool FBillboardRenderer::Init()
{
	ViewCB = RenderCommand::CreateConstantBuffer(sizeof(FViewConstants));
	PerObjectCB = RenderCommand::CreateConstantBuffer(sizeof(FPerObjectConstants));
	return ViewCB && PerObjectCB;
}

void FBillboardRenderer::BeginRender(const FMatrix& ViewProjection, const FVector& CameraPosition)
{
	FViewConstants ViewConstants;
	ViewConstants.VP = ViewProjection;
	ViewConstants.CameraPosition = CameraPosition;
	RenderCommand::UpdateBufferData(ViewCB.get(), &ViewConstants);

	// 앞선 Grid·Text 패스가 바꾼 상태를 빌보드 기준으로 맞춘다.
	RenderCommand::SetRasterizerState(ERasterizerState::SolidBack);
	RenderCommand::SetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void FBillboardRenderer::OnRender(const UBillboardComponent& Billboard, const FMatrix& BillboardWorld)
{
	UMaterial* Material = Billboard.GetMaterial(0);
	UStaticMesh* QuadMesh = Billboard.GetQuadMesh();
	if (!Material || !Material->Shader || !QuadMesh)
		return;

	RenderCommand::BindShaderProgram(Material->Shader);
	RenderCommand::SetBlendState(Material->BlendState);
	RenderCommand::SetDepthStencilState(Material->DepthStencilState);
	for (int32 i = 0; i < Material->Textures.Num(); ++i)
	{
		RenderCommand::BindShaderResource(i, Material->Textures[i], EShaderBindFlagBits::Pixel);
	}
	RenderCommand::BindSamplerState(0, Material->SamplerState, EShaderBindFlagBits::Pixel);

	// b0 View, b1 Tint, b2 World (BillboardShader.hlsl 레지스터 배치)
	RenderCommand::BindConstantBuffer(0, ViewCB.get(), EShaderBindFlagBits::Vertex | EShaderBindFlagBits::Pixel);

	if (Material->ParamBuffer)
	{
		RenderCommand::UpdateBufferData(Material->ParamBuffer.get(), &Billboard.GetBillboardParams(), sizeof(FBillboardMaterialParams));
		RenderCommand::BindConstantBuffer(1, Material->ParamBuffer.get(), EShaderBindFlagBits::Vertex | EShaderBindFlagBits::Pixel);
	}

	// 빌보드 셰이더는 노멀을 쓰지 않으므로 역전치 계산은 생략한다.
	FPerObjectConstants ObjectConstants;
	ObjectConstants.World = BillboardWorld;
	ObjectConstants.WorldInvTranspose = FMatrix::Identity;
	RenderCommand::UpdateBufferData(PerObjectCB.get(), &ObjectConstants);
	RenderCommand::BindConstantBuffer(2, PerObjectCB.get(), EShaderBindFlagBits::Vertex);

	RenderCommand::BindMesh(QuadMesh, 0);
	RenderCommand::DrawIndexed(QuadMesh->GetIndexBuffer(0)->GetIndexCount());
}

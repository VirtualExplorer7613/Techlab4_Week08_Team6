#include "EnginePCH.h"
#include "ShadowRenderer.h"

#include "RenderCommand.h"
#include "ShadowInfo.h"
#include "RenderResourceManager.h"
#include "RenderingInfo.h"
#include "Engine/PrimitiveSceneProxy.h"

bool FShadowRenderer::Init(uint32 Resolution)
{
	if (Resolution == 0)
		return false;

	D3D11_TEXTURE2D_DESC Desc{};

	Desc.Width = Resolution;
	Desc.Height = Resolution;
	Desc.MipLevels = 1;
	Desc.ArraySize = 1;
	Desc.Format = DXGI_FORMAT_R24G8_TYPELESS;
	Desc.SampleDesc.Count = 1;
	Desc.Usage = D3D11_USAGE_DEFAULT;
	Desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;

	auto NewShadowMap = RenderCommand::CreateTexture2D(Desc, nullptr);

	if (!NewShadowMap || !NewShadowMap->GetDSV() ||	!NewShadowMap->GetSRV())
	{
		return false;
	}

	auto NewShadowCB = RenderCommand::CreateConstantBuffer(sizeof(FShadowConstants));

	if (!NewShadowCB || !NewShadowCB->GetBuffer())
	{
		return false;
	}

	// 각 물체의 World 행렬을 전달할 상수 버퍼.
	auto NewShadowObjectCB = RenderCommand::CreateConstantBuffer(sizeof(FShadowObjectConstants));

	if (!NewShadowObjectCB || !NewShadowObjectCB->GetBuffer())
	{
		return false;
	}
		
	ShadowMap = std::move(NewShadowMap);
	ShadowCB = std::move(NewShadowCB);
	ShadowObjectCB = std::move(NewShadowObjectCB);

	return true;
}

void FShadowRenderer::Render(const FRenderQueue& ShadowPackets)
{
	if (!ShadowMap || !ShadowMap->GetDSV() || !ShadowCB || !ShadowCB->GetBuffer() || !ShadowObjectCB || !ShadowObjectCB->GetBuffer())
	{
		return;
	}

	const FString ShaderPath = "Resources/Shader/ShadowDepth.hlsl";
	FShaderProgram* Shader = FRenderResourceManager::GetShaderProgram(ShaderPath);

	if (!Shader || Shader->SourcePath != ShaderPath)
	{
		return;
	}

	ID3D11DeviceContext* Context = RenderCommand::GetContext();

	// 이전 SRV를 해제한다.
	RenderCommand::UnbindShaderResource(0, EShaderBindFlagBits::Pixel);

	// Shadow Map의 깊이만 기록한다.
	FRenderingInfo PassInfo{};
	PassInfo.ViewportSetting.Width = ShadowMap->GetWidth();
	PassInfo.ViewportSetting.Height = ShadowMap->GetHeight();

	PassInfo.DepthStencil.Texture = ShadowMap.get();
	PassInfo.DepthStencil.LoadOp = ERenderTargetLoadOp::Clear;
	PassInfo.DepthStencil.ClearValue.DepthClearValue = 1.0f;

	RenderCommand::BeginRenderPass(PassInfo);

	// 광원이 없으면 Clear된 맵만 남긴다.
	if (!DirectionalShadowInfo.bEnabled)
	{
		RenderCommand::EndRenderPass(PassInfo);
		return;
	}

	FShadowConstants Constants{};
	Constants.LightViewProjection = DirectionalShadowInfo.LightViewProjection;
	Constants.InvShadowMapSize = FVector2(
		1.0f / static_cast<float>(ShadowMap->GetWidth()),
		1.0f / static_cast<float>(ShadowMap->GetHeight()));
	Constants.DepthBias = DirectionalShadowInfo.DepthBias;
	Constants.bEnabled = 1;

	RenderCommand::UpdateBufferData(ShadowCB.get(), &Constants);

	// BindShaderProgram은 VS·PS·InputLayout을 모두 설정한다.
	RenderCommand::BindShaderProgram(Shader);
	// 깊이 기록에는 PS가 필요 없으므로 바인딩 후 해제한다.
	Context->PSSetShader(nullptr, nullptr, 0);

	RenderCommand::BindConstantBuffer(5, ShadowCB.get(), EShaderBindFlagBits::Vertex);
	RenderCommand::BindConstantBuffer(2, ShadowObjectCB.get(), EShaderBindFlagBits::Vertex);

	RenderCommand::SetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	RenderCommand::SetRasterizerState(ERasterizerState::SolidBack);

	// Default는 LESS 비교 + 깊이 쓰기 활성화.
	RenderCommand::SetDepthStencilState(EDepthStencilState::Default);

	RenderCommand::SetBlendState(EBlendState::Opaque);

	for (const FRenderPacket& Packet : ShadowPackets)
	{
		//
		if (!Packet.Proxy || !Packet.Mesh || Packet.IndexCount == 0)
		{
			continue;
		}

		//
		if (Packet.LODIndex >= Packet.Mesh->GetLODCount())
		{
			continue;
		}

		if (!Packet.Mesh->GetVertexBuffer(Packet.LODIndex) || !Packet.Mesh->GetIndexBuffer(Packet.LODIndex))
		{
			continue;
		}

		// 이 Section이 속한 메시와 LOD를 연결한다.
		RenderCommand::BindMesh(Packet.Mesh, Packet.LODIndex);

		// 다음 DrawIndexed에 사용할 물체의 World 행렬.
		FShadowObjectConstants ObjectConstants{};
		ObjectConstants.World = Packet.Proxy->GetLocalToWorld();

		RenderCommand::UpdateBufferData(ShadowObjectCB.get(), &ObjectConstants);

		RenderCommand::DrawIndexed(Packet.IndexCount, Packet.StartIndex);
	}

	// UI나 다음 패스에서 SRV로 읽을 수 있도록 DSV 연결 해제.
	RenderCommand::EndRenderPass(PassInfo);
}

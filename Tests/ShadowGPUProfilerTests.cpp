#include "EnginePCH.h"
#include "Render/Shadow/ShadowGPUProfiler.h"

#include <cmath>
#include <functional>
#include <stdexcept>

namespace
{
	void Check(bool bCondition, const char* Message)
	{
		if (!bCondition)
			throw std::runtime_error(Message);
	}

	// 제품은 DONOTFLUSH로 한 번씩만 조회한다. 제출/대기는 이 테스트에서만 수행한다.
	void Drain(ID3D11Device* Device, ID3D11DeviceContext* Context)
	{
		ComPtr<ID3D11Query> Fence;
		D3D11_QUERY_DESC Desc{D3D11_QUERY_EVENT, 0};
		Check(SUCCEEDED(Device->CreateQuery(&Desc, Fence.GetAddressOf())), "event query creation");
		Context->End(Fence.Get());
		Context->Flush();
		const uint64 Start = GetTickCount64();
		HRESULT Result;
		while ((Result = Context->GetData(Fence.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH)) == S_FALSE)
		{
			Check(GetTickCount64() - Start < 5000, "test GPU completion timeout");
			SwitchToThread();
		}
		Check(Result == S_OK, "test GPU completion");
	}

	// BeginFrame에서 지난 프레임을 해결한 뒤 여는 새 프레임은 아직 게시하지 않는다.
	void Resolve(FShadowGPUProfiler& Profiler, ID3D11Device* Device, ID3D11DeviceContext* Context)
	{
		Drain(Device, Context);
		Profiler.BeginFrame(true);
	}
}

int main()
{
	try
	{
		ComPtr<ID3D11Device> Device;
		ComPtr<ID3D11DeviceContext> Context;
		D3D_FEATURE_LEVEL Level;
		Check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
			D3D11_SDK_VERSION, Device.GetAddressOf(), &Level, Context.GetAddressOf())), "real WARP D3D11 device");
		FShadowGPUProfiler Profiler(Device.Get(), Context.Get());
		Profiler.BeginFrame(false);
		Profiler.EndFrame();
		Check(!Profiler.GetStats().bAvailable && Profiler.GetStats().SampleCount == 0, "disabled has no false zero");

		Profiler.BeginFrame(true);
		Profiler.EndFrame();
		Resolve(Profiler, Device.Get(), Context.Get());
		Check(Profiler.GetStats().bAvailable && Profiler.GetStats().LastMs == 0.0 &&
			Profiler.GetStats().SampleCount == 1, "completed zero-pass frame is valid zero");
		Profiler.EndFrame();
		Profiler.ResetStats();
		Check(!Profiler.GetStats().bAvailable && Profiler.GetStats().SampleCount == 0, "reset clears published stats");
		Resolve(Profiler, Device.Get(), Context.Get());
		Check(!Profiler.GetStats().bAvailable && Profiler.GetStats().SampleCount == 0, "reset discards pending result");

		ComPtr<ID3D11Texture2D> Texture;
		D3D11_TEXTURE2D_DESC TextureDesc{};
		TextureDesc.Width = TextureDesc.Height = 512;
		TextureDesc.MipLevels = TextureDesc.ArraySize = 1;
		TextureDesc.Format = DXGI_FORMAT_D32_FLOAT;
		TextureDesc.SampleDesc.Count = 1;
		TextureDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
		Check(SUCCEEDED(Device->CreateTexture2D(&TextureDesc, nullptr, Texture.GetAddressOf())), "depth texture");
		ComPtr<ID3D11DepthStencilView> Depth;
		Check(SUCCEEDED(Device->CreateDepthStencilView(Texture.Get(), nullptr, Depth.GetAddressOf())), "depth view");

		// 직접 쿼리의 두 구간 합계와 대조한다. 중간의 96회 clear는 섀도 구간 밖이다.
		ComPtr<ID3D11Query> Disjoint;
		ComPtr<ID3D11Query> Stamps[8];
		D3D11_QUERY_DESC DisjointDesc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
		D3D11_QUERY_DESC StampDesc{D3D11_QUERY_TIMESTAMP, 0};
		Check(SUCCEEDED(Device->CreateQuery(&DisjointDesc, Disjoint.GetAddressOf())), "reference disjoint");
		for (auto& Stamp : Stamps)
			Check(SUCCEEDED(Device->CreateQuery(&StampDesc, Stamp.GetAddressOf())), "reference timestamp");
		Context->Begin(Disjoint.Get());
		for (uint32 Pass = 0; Pass < 2; ++Pass)
		{
			Context->End(Stamps[Pass * 4].Get());
			{
				FScopedShadowGPUTime Scope(Profiler);
				Context->End(Stamps[Pass * 4 + 1].Get());
				for (uint32 Clear = 0; Clear < 8; ++Clear)
					Context->ClearDepthStencilView(Depth.Get(), D3D11_CLEAR_DEPTH, 0.5f, 0);
				Context->End(Stamps[Pass * 4 + 2].Get());
			}
			Context->End(Stamps[Pass * 4 + 3].Get());
			if (Pass == 0)
				for (uint32 Clear = 0; Clear < 96; ++Clear)
					Context->ClearDepthStencilView(Depth.Get(), D3D11_CLEAR_DEPTH, 0.25f, 0);
		}
		Context->End(Disjoint.Get());
		Profiler.EndFrame();
		Resolve(Profiler, Device.Get(), Context.Get());
		const auto First = Profiler.GetStats();
		Check(First.bAvailable && First.LastMs > 0.0 && First.SampleCount == 1, "multiple real passes publish positive GPU time");
		D3D11_QUERY_DATA_TIMESTAMP_DISJOINT Reference{};
		Check(Context->GetData(Disjoint.Get(), &Reference, sizeof(Reference), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
			!Reference.Disjoint && Reference.Frequency > 0, "reference timestamp frequency");
		UINT64 Ticks[8]{};
		for (uint32 Index = 0; Index < 8; ++Index)
			Check(Context->GetData(Stamps[Index].Get(), &Ticks[Index], sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK,
				"reference timestamp ready");
		const double OuterMs = 1000.0 * static_cast<double>((Ticks[3] - Ticks[0]) + (Ticks[7] - Ticks[4])) / Reference.Frequency;
		const double InnerMs = 1000.0 * static_cast<double>((Ticks[2] - Ticks[1]) + (Ticks[6] - Ticks[5])) / Reference.Frequency;
		Check(First.LastMs + 0.001 >= InnerMs, "GPU time sums both measured passes");
		Check(First.LastMs <= OuterMs + 0.001, "GPU sum excludes unmeasured clear work between passes");

		// 0도 평균에 포함하고, 120개가 지나면 오래된 양수 샘플은 평균에서 제외한다.
		Profiler.EndFrame();
		Resolve(Profiler, Device.Get(), Context.Get());
		Check(Profiler.GetStats().LastMs == 0.0 && Profiler.GetStats().SampleCount == 2 &&
			std::abs(Profiler.GetStats().AverageMs - First.LastMs / 2.0) < 1e-9, "average includes real zero frame");
		for (uint32 Index = 0; Index < 119; ++Index)
		{
			Profiler.EndFrame();
			Resolve(Profiler, Device.Get(), Context.Get());
		}
		Check(Profiler.GetStats().SampleCount == 120 && std::abs(Profiler.GetStats().AverageMs) < 1e-9 &&
			Profiler.GetStats().MaxMs == First.LastMs, "120 resolved sample window and lifetime max");

		Profiler.EndFrame();
		Profiler.BeginFrame(false);
		Check(!Profiler.GetStats().bAvailable, "disable clears availability");
		Resolve(Profiler, Device.Get(), Context.Get());
		Check(!Profiler.GetStats().bAvailable, "re-enable does not resurrect disabled pending frame");
		Profiler.ResetStats();
		Profiler.EndFrame();
		Resolve(Profiler, Device.Get(), Context.Get());
		Check(!Profiler.GetStats().bAvailable && Profiler.GetStats().SampleCount == 0,
			"reset during recording discards that frame");
		for (uint32 Pass = 0; Pass <= FShadowGPUProfiler::PassCapacity; ++Pass)
		{
			FScopedShadowGPUTime Scope(Profiler);
			Context->ClearDepthStencilView(Depth.Get(), D3D11_CLEAR_DEPTH, 0.5f, 0);
		}
		Profiler.EndFrame();
		Resolve(Profiler, Device.Get(), Context.Get());
		Check(!Profiler.GetStats().bAvailable && Profiler.GetStats().SampleCount == 0,
			"pass capacity invalidates full frame instead of partial time");
		Profiler.EndFrame();
		Resolve(Profiler, Device.Get(), Context.Get());
		Check(Profiler.GetStats().bAvailable && Profiler.GetStats().SampleCount == 1, "capture recovers after overflow");
		Profiler.EndFrame();
		Profiler.BeginFrame(false);

		// DONOTFLUSH인 실제 WARP 명령 배치를 제출하지 않아 ring을 채운다.
		FShadowGPUProfiler Saturated(Device.Get(), Context.Get());
		Saturated.BeginFrame(true);
		Saturated.EndFrame();
		Resolve(Saturated, Device.Get(), Context.Get());
		Check(Saturated.GetStats().bAvailable, "ring test starts with a valid sample");
		Saturated.EndFrame();
		for (uint32 Frame = 0; Frame < FShadowGPUProfiler::FrameCapacity; ++Frame)
		{
			Saturated.BeginFrame(true);
			Saturated.EndFrame();
		}
		Check(!Saturated.GetStats().bAvailable && Saturated.GetStats().SampleCount == 1,
			"unsubmitted full ring becomes unavailable without publishing partial data");
		Resolve(Saturated, Device.Get(), Context.Get());
		Check(!Saturated.GetStats().bAvailable && Saturated.GetStats().SampleCount == 1,
			"full ring discards stale results after eventual GPU completion");
		Saturated.EndFrame();
		Resolve(Saturated, Device.Get(), Context.Get());
		Check(Saturated.GetStats().bAvailable && Saturated.GetStats().SampleCount == 2,
			"ring resumes sampling when slots resolve");
		Saturated.EndFrame();
		Saturated.BeginFrame(false);
		std::cout << "ShadowGPUProfilerTests: PASS (real D3D11 WARP; zero, multi-pass, history, disable, reset, pass/ring overflow)\n";
		return 0;
	}
	catch (const std::exception& Error)
	{
		std::cerr << "ShadowGPUProfilerTests: FAIL: " << Error.what() << '\n';
		return 1;
	}
}

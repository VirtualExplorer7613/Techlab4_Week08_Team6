#pragma once

#include "Render/RenderCommand.h"

#include <array>

struct FShadowGPUStats
{
	bool bAvailable = false;
	double LastMs = 0.0;
	double AverageMs = 0.0;
	double MaxMs = 0.0;
	uint32 SampleCount = 0;
};

class FShadowGPUProfiler
{
public:
	static constexpr uint32 FrameCapacity = 8;
	static constexpr uint32 PassCapacity = 128;
	static constexpr uint32 HistoryCapacity = 120;

	FShadowGPUProfiler() = default;
	explicit FShadowGPUProfiler(ID3D11Device* InDevice, ID3D11DeviceContext* InContext)
		: Device(InDevice), Context(InContext), bUseRenderCommand(false)
	{
	}
	~FShadowGPUProfiler() { EndFrame(); }
	FShadowGPUProfiler(const FShadowGPUProfiler&) = delete;
	FShadowGPUProfiler& operator=(const FShadowGPUProfiler&) = delete;

	// Render thread only. There is no Flush, wait, or query overwrite while pending.
	void BeginFrame(bool bEnabled)
	{
		if (CurrentSlot >= 0)
		{
			Invalidate();
			EndFrame();
		}
		if (!bEnabled)
		{
			if (bWasEnabled)
				Invalidate();
			bWasEnabled = false;
			Stats.bAvailable = false;
			ResolvePending();
			return;
		}
		bWasEnabled = true;
		if (!Device || !Context)
		{
			if (bUseRenderCommand)
			{
				Device = RenderCommand::GetDevice();
				Context = RenderCommand::GetContext();
			}
			if (!Device || !Context)
			{
				Invalidate();
				return;
			}
		}
		ResolvePending();
		for (uint32 Index = 0; Index < FrameCapacity; ++Index)
		{
			auto& Frame = Frames[Index];
			if (Frame.bPending)
				continue;
			if (!CreateQuery(Frame.Disjoint, D3D11_QUERY_TIMESTAMP_DISJOINT))
			{
				Invalidate();
				return;
			}
			Frame.PassCount = 0;
			Frame.bPassOpen = false;
			Frame.bValid = true;
			Frame.Generation = Generation;
			Frame.Sequence = ++Sequence;
			Context->Begin(Frame.Disjoint.Get());
			CurrentSlot = static_cast<int32>(Index);
			return;
		}
		// All slots still belong to the GPU. Skip this whole sample, never stall.
		Invalidate();
	}

	void BeginPass()
	{
		if (CurrentSlot < 0)
			return;
		auto& Frame = Frames[CurrentSlot];
		if (!Frame.bValid)
			return;
		if (Frame.bPassOpen || Frame.PassCount == PassCapacity)
		{
			Invalidate();
			return;
		}
		auto& Pass = Frame.Passes[Frame.PassCount];
		if (!CreateQuery(Pass.Start, D3D11_QUERY_TIMESTAMP) || !CreateQuery(Pass.End, D3D11_QUERY_TIMESTAMP))
		{
			Invalidate();
			return;
		}
		Context->End(Pass.Start.Get());
		Frame.bPassOpen = true;
	}

	void EndPass()
	{
		if (CurrentSlot < 0)
			return;
		auto& Frame = Frames[CurrentSlot];
		if (!Frame.bPassOpen)
			return;
		Context->End(Frame.Passes[Frame.PassCount].End.Get());
		Frame.bPassOpen = false;
		++Frame.PassCount;
	}

	void EndFrame()
	{
		if (CurrentSlot < 0)
			return;
		auto& Frame = Frames[CurrentSlot];
		if (Frame.bPassOpen)
		{
			Invalidate();
			EndPass();
		}
		Context->End(Frame.Disjoint.Get());
		Frame.bPending = true;
		CurrentSlot = -1;
	}

	void ResetStats()
	{
		Invalidate();
		Stats = {};
		History = {};
		HistoryIndex = 0;
		HistorySum = 0.0;
	}

	const FShadowGPUStats& GetStats() const { return Stats; }

private:
	struct FPass
	{
		ComPtr<ID3D11Query> Start;
		ComPtr<ID3D11Query> End;
	};
	struct FFrame
	{
		ComPtr<ID3D11Query> Disjoint;
		std::array<FPass, PassCapacity> Passes;
		uint32 PassCount = 0;
		uint64 Generation = 0;
		uint64 Sequence = 0;
		bool bPending = false;
		bool bPassOpen = false;
		bool bValid = false;
	};

	bool CreateQuery(ComPtr<ID3D11Query>& Query, D3D11_QUERY Type)
	{
		if (Query)
			return true;
		const D3D11_QUERY_DESC Desc{Type, 0};
		return SUCCEEDED(Device->CreateQuery(&Desc, Query.GetAddressOf()));
	}

	void Invalidate()
	{
		++Generation;
		Stats.bAvailable = false;
		if (CurrentSlot >= 0)
			Frames[CurrentSlot].bValid = false;
	}

	void ResolvePending()
	{
		if (!Context)
			return;
		// Resolve in submission order so a late old frame never replaces a new Last.
		for (uint32 Attempt = 0; Attempt < FrameCapacity; ++Attempt)
		{
			FFrame* Oldest = nullptr;
			for (auto& Frame : Frames)
				if (Frame.bPending && (!Oldest || Frame.Sequence < Oldest->Sequence))
					Oldest = &Frame;
			if (!Oldest)
				return;
			auto& Frame = *Oldest;
			D3D11_QUERY_DATA_TIMESTAMP_DISJOINT Disjoint{};
			HRESULT Result = Context->GetData(Frame.Disjoint.Get(), &Disjoint, sizeof(Disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH);
			if (Result == S_FALSE)
				return;
			bool bValid = Result == S_OK && !Disjoint.Disjoint && Disjoint.Frequency > 0;
			double Ticks = 0.0;
			if (bValid && Frame.bValid && Frame.Generation == Generation && bWasEnabled)
			{
				for (uint32 Index = 0; Index < Frame.PassCount; ++Index)
				{
					UINT64 Start = 0;
					UINT64 End = 0;
					const HRESULT StartResult = Context->GetData(Frame.Passes[Index].Start.Get(), &Start, sizeof(Start), D3D11_ASYNC_GETDATA_DONOTFLUSH);
					const HRESULT EndResult = Context->GetData(Frame.Passes[Index].End.Get(), &End, sizeof(End), D3D11_ASYNC_GETDATA_DONOTFLUSH);
					if (FAILED(StartResult) || FAILED(EndResult))
					{
						bValid = false;
						break;
					}
					if (StartResult == S_FALSE || EndResult == S_FALSE)
						return;
					if (StartResult != S_OK || EndResult != S_OK || End < Start)
					{
						bValid = false;
						break;
					}
					Ticks += static_cast<double>(End - Start);
				}
			}
			Frame.bPending = false;
			if (!bValid)
			{
				Invalidate();
				// Recreate failed/disjoint query sets lazily instead of retaining bad queries.
				Frame = {};
			}
			else if (Frame.bValid && Frame.Generation == Generation && bWasEnabled)
				Publish(1000.0 * Ticks / static_cast<double>(Disjoint.Frequency));
		}
	}

	void Publish(double Milliseconds)
	{
		HistorySum -= History[HistoryIndex];
		History[HistoryIndex] = Milliseconds;
		HistorySum += Milliseconds;
		HistoryIndex = (HistoryIndex + 1) % HistoryCapacity;
		if (Stats.SampleCount < HistoryCapacity)
			++Stats.SampleCount;
		Stats.LastMs = Milliseconds;
		Stats.AverageMs = HistorySum / Stats.SampleCount;
		if (Milliseconds > Stats.MaxMs)
			Stats.MaxMs = Milliseconds;
		Stats.bAvailable = true;
	}

	ComPtr<ID3D11Device> Device;
	ComPtr<ID3D11DeviceContext> Context;
	std::array<FFrame, FrameCapacity> Frames;
	std::array<double, HistoryCapacity> History{};
	FShadowGPUStats Stats;
	double HistorySum = 0.0;
	uint64 Generation = 0;
	uint64 Sequence = 0;
	uint32 HistoryIndex = 0;
	int32 CurrentSlot = -1;
	bool bWasEnabled = false;
	bool bUseRenderCommand = true;
};

class FScopedShadowGPUTime
{
public:
	explicit FScopedShadowGPUTime(FShadowGPUProfiler& InProfiler) : Profiler(InProfiler) { Profiler.BeginPass(); }
	~FScopedShadowGPUTime() { Profiler.EndPass(); }
	FScopedShadowGPUTime(const FScopedShadowGPUTime&) = delete;
	FScopedShadowGPUTime& operator=(const FScopedShadowGPUTime&) = delete;
private:
	FShadowGPUProfiler& Profiler;
};

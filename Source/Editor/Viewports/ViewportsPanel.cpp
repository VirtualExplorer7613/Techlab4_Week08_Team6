#include "EnginePCH.h"
#include "Editor/Viewports/ViewportsPanel.h"
#include "Editor/LevelEditor/MultipleViewports/Adapter/MultipleViewportsAdapter.h"

#include "Core/Stats/StatOverlay.h"
#include "Core/Stats/LightweightStats.h"
#include "Render/RenderCommand.h"

#include <algorithm>
#include <cassert>
#include <format>

#include "Core/Stats/EditorStats.h"
#include "Core/Stats/ShadowStats.h"

namespace
{
constexpr ImU32 SplitterColor = IM_COL32(55, 55, 55, 230);
constexpr ImU32 SplitterHoverColor = IM_COL32(255, 192, 0, 255);
constexpr const char* CameraPresetLabels[] = {
	"Perspective", "Ortho (Current)", "Top", "Bottom", "Front", "Back", "Left", "Right"};

// Stat Overlay
constexpr float StatOverlayMargin = 8.0f;
constexpr float StatOverlayPadding = 6.0f;
constexpr ImU32 StatOverlayBackgroundColor = IM_COL32(0, 0, 0, 140);
constexpr ImU32 TitleColor = IM_COL32(255, 210, 60, 255);
constexpr ImU32 ValueColor = IM_COL32(235, 235, 235, 255);
}

// 네 View의 렌더 타깃을 최소 크기로 초기화한다.
bool FViewportsPanel::Init()
{
	for (FViewSlot& Slot : Slots)
		ResizeSlot(Slot, 1, 1);
	return true;
}

// 패널의 프레임 갱신 인터페이스이며 별도 계산은 하지 않는다.
void FViewportsPanel::Tick(float DeltaTime)
{
	(void)DeltaTime;
}

// Core Rect에 맞춰 View 활성 상태와 타깃 크기를 갱신한다.
void FViewportsPanel::SetView(const int32 ViewIndex, const FRect& Rect, const bool bActive)
{
	assert(ViewIndex >= 0 && ViewIndex < 4);
	FViewSlot& Slot = Slots[ViewIndex];
	Slot.Rect = Rect;
	Slot.bActive = bActive && Rect.Width > 0.0f && Rect.Height > 0.0f;
	if (!Slot.bActive)
		return;

	const uint32 Width = static_cast<uint32>(std::max(1.0f, Rect.Width));
	const uint32 Height = static_cast<uint32>(std::max(1.0f, Rect.Height));
	if (Width != Slot.Width || Height != Slot.Height)
		ResizeSlot(Slot, Width, Height);
}

// 인덱스를 검사해 해당 View의 렌더 정보를 반환한다.
const FRenderingInfo& FViewportsPanel::GetRenderingInfo(const int32 ViewIndex) const
{
	assert(ViewIndex >= 0 && ViewIndex < 4);
	return Slots[ViewIndex].RenderingInfo;
}

FTexture2D* FViewportsPanel::GetFxaaTarget(const int32 ViewIndex) const
{
	assert(ViewIndex >= 0 && ViewIndex < 4);
	return Slots[ViewIndex].FxaaTarget.get();
}

// 마우스 위치에서 패널 원점을 빼 로컬 좌표로 바꾼다.
FVector2 FViewportsPanel::GetLocalMousePosition() const
{
	const ImVec2 Mouse = ImGui::GetMousePos();
	return {Mouse.x - ContentOrigin.x, Mouse.y - ContentOrigin.y};
}

// 엔진 입력 처리는 ImGui NewFrame보다 먼저 실행되므로 현재 OS 커서로 버튼 영역을 검사한다.
bool FViewportsPanel::IsHovered() const
{
	if (bStatResetButtonVisible && FStatOverlay::IsAnyEnabled())
	{
		POINT Cursor{};
		if (::GetCursorPos(&Cursor) &&
			Cursor.x >= StatResetButtonMin.x && Cursor.x < StatResetButtonMax.x &&
			Cursor.y >= StatResetButtonMin.y && Cursor.y < StatResetButtonMax.y)
			return false;
	}
	return bHovered;
}

// 누적 가로 Splitter 이동량을 반환하고 초기화한다.
float FViewportsPanel::ConsumeHorizontalDrag()
{
	const float Result = PendingHorizontalDrag;
	PendingHorizontalDrag = 0.0f;
	return Result;
}

// 누적 세로 Splitter 이동량을 반환하고 초기화한다.
float FViewportsPanel::ConsumeVerticalDrag()
{
	const float Result = PendingVerticalDrag;
	PendingVerticalDrag = 0.0f;
	return Result;
}

// Layout·Single 대상·Preset을 UI 표시와 동기화한다.
void FViewportsPanel::SetControlState(const ELayoutMode LayoutMode, const int32 SingleViewIndex, const EMultipleViewportsCameraPreset CameraPresets[4])
{
	CurrentLayoutMode = LayoutMode;
	CurrentSingleViewIndex = SingleViewIndex;
	for (int32 Index = 0; Index < 4; ++Index)
		CurrentCameraPresets[Index] = CameraPresets[Index];
}

// 대기 Layout 요청을 한 번 반환하고 플래그를 지운다.
bool FViewportsPanel::ConsumeLayoutRequest(ELayoutMode& OutMode, int32& OutSingleViewIndex)
{
	if (!bHasLayoutRequest)
		return false;
	OutMode = RequestedLayoutMode;
	OutSingleViewIndex = RequestedSingleViewIndex;
	bHasLayoutRequest = false;
	return true;
}

// 대기 Preset 요청을 한 번 반환하고 지운다.
bool FViewportsPanel::ConsumeCameraPresetRequest(int32& OutViewIndex, EMultipleViewportsCameraPreset& OutPreset)
{
	if (PendingCameraPresetViewIndex == InvalidViewIndex)
		return false;
	OutViewIndex = PendingCameraPresetViewIndex;
	OutPreset = PendingCameraPreset;
	PendingCameraPresetViewIndex = InvalidViewIndex;
	return true;
}

// View Texture와 Splitter·Layout·Preset UI를 그리고 요청을 기록한다.
void FViewportsPanel::OnRender()
{
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0.0f, 0.0f});
	ImGui::Begin("Viewports", nullptr,
		ImGuiWindowFlags_NoScrollbar |
		ImGuiWindowFlags_NoScrollWithMouse |
		ImGuiWindowFlags_NoTitleBar);

	ContentOrigin = ImGui::GetCursorScreenPos();
	ContentSize = ImGui::GetContentRegionAvail();
	ContentSize.x = std::max(1.0f, ContentSize.x);
	ContentSize.y = std::max(1.0f, ContentSize.y);
	bHovered = ImGui::IsWindowHovered();
	bStatResetButtonVisible = false;

	// 전체 캔버스를 한 번 확보한 뒤 각 렌더 타깃을 창 DrawList에 직접 그린다.
	// Image 항목 네 개를 따로 배치하면 ImGui 레이아웃과 클리핑 상태가 삽입 순서에
	// 영향을 받아, Core가 올바른 사각형을 줘도 아래쪽 행이 잘릴 수 있다.
	ImGui::Dummy(ContentSize);
	ImDrawList* DrawList = ImGui::GetWindowDrawList();
	DrawList->PushClipRect(ContentOrigin,
		{ContentOrigin.x + ContentSize.x, ContentOrigin.y + ContentSize.y}, true);
	for (int32 ViewIndex = 0; ViewIndex < 4; ++ViewIndex)
	{
		const FViewSlot& Slot = Slots[ViewIndex];
		if (!Slot.bActive || !Slot.ColorTarget)
			continue;

		const ImVec2 ViewMin{ContentOrigin.x + Slot.Rect.X, ContentOrigin.y + Slot.Rect.Y};
		const ImVec2 ViewMax{ViewMin.x + Slot.Rect.Width, ViewMin.y + Slot.Rect.Height};
		DrawList->AddImage(Slot.ColorTarget->GetSRV(), ViewMin, ViewMax);
	}
	DrawList->PopClipRect();

	if (Slots[1].bActive || Slots[2].bActive || Slots[3].bActive)
	{
		// View Rect 사이에 비워 둔 gutter의 중앙에 Splitter 버튼을 배치한다.
		const float SplitX = (Slots[0].Rect.X + Slots[0].Rect.Width + Slots[1].Rect.X) * 0.5f;
		const float SplitY = (Slots[0].Rect.Y + Slots[0].Rect.Height + Slots[2].Rect.Y) * 0.5f;

		const ImVec2 VerticalMin{ContentOrigin.x + SplitX - SplitterThickness * 0.5f, ContentOrigin.y};
		const ImVec2 VerticalMax{VerticalMin.x + SplitterThickness, ContentOrigin.y + ContentSize.y};
		const ImVec2 HorizontalMin{ContentOrigin.x, ContentOrigin.y + SplitY - SplitterThickness * 0.5f};
		const ImVec2 HorizontalMax{ContentOrigin.x + ContentSize.x, HorizontalMin.y + SplitterThickness};

		ImGui::SetCursorScreenPos(VerticalMin);
		ImGui::InvisibleButton("##MultipleViewportsHorizontalSplitter", {SplitterThickness, ContentSize.y});
		ImGui::SetCursorScreenPos(HorizontalMin);
		ImGui::InvisibleButton("##MultipleViewportsVerticalSplitter", {ContentSize.x, SplitterThickness});

		const bool bVerticalHovered = bHovered && ImGui::IsMouseHoveringRect(VerticalMin, VerticalMax);
		const bool bHorizontalHovered = bHovered && ImGui::IsMouseHoveringRect(HorizontalMin, HorizontalMax);
		if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		{
			bDraggingVerticalSplitter = bVerticalHovered;
			bDraggingHorizontalSplitter = bHorizontalHovered;
		}
		if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
		{
			bDraggingVerticalSplitter = false;
			bDraggingHorizontalSplitter = false;
		}

		const bool bVerticalHighlighted = bDraggingVerticalSplitter || bVerticalHovered;
		const bool bHorizontalHighlighted = bDraggingHorizontalSplitter || bHorizontalHovered;
		DrawList->AddRectFilled(VerticalMin, VerticalMax,
			bVerticalHighlighted ? SplitterHoverColor : SplitterColor);
		DrawList->AddRectFilled(HorizontalMin, HorizontalMax,
			bHorizontalHighlighted ? SplitterHoverColor : SplitterColor);

		if (bDraggingVerticalSplitter)
			PendingHorizontalDrag += ImGui::GetIO().MouseDelta.x;
		if (bDraggingHorizontalSplitter)
			PendingVerticalDrag += ImGui::GetIO().MouseDelta.y;

		if (bVerticalHighlighted && bHorizontalHighlighted)
			ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
		else if (bVerticalHighlighted)
			ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
		else if (bHorizontalHighlighted)
			ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
	}
	else
	{
		bDraggingVerticalSplitter = false;
		bDraggingHorizontalSplitter = false;
	}

	float ToolbarExtraHeight[4]{};
	for (int32 ViewIndex = 0; ViewIndex < 4; ++ViewIndex)
	{
		if (!Slots[ViewIndex].bActive)
			continue;

		const FRect& Rect = Slots[ViewIndex].Rect;
		const ImVec2 ViewMin{ContentOrigin.x + Rect.X, ContentOrigin.y + Rect.Y};
		const ImVec2 ViewMax{ViewMin.x + Rect.Width, ViewMin.y + Rect.Height};
		const ImVec2 ToolbarOrigin{ViewMin.x + 8.0f, ViewMin.y + 8.0f};
		const float ToolbarWidth = std::max(1.0f, Rect.Width - 16.0f);
		ImVec2 ToolbarCursor = ToolbarOrigin;

		// View 안에서 항목을 줄바꿈하고, 최소 폭보다 작은 View에서도 옆 View로 넘치지 않게 한다.
		ImGui::PushClipRect(ViewMin, ViewMax, true);
		auto PlaceToolbarItem = [&](float PreferredWidth)
		{
			const float Width = std::min(PreferredWidth, ToolbarWidth);
			if (ToolbarCursor.x > ToolbarOrigin.x && ToolbarCursor.x + Width > ToolbarOrigin.x + ToolbarWidth)
			{
				ToolbarCursor.x = ToolbarOrigin.x;
				ToolbarCursor.y += ImGui::GetFrameHeightWithSpacing();
			}
			ImGui::SetCursorScreenPos(ToolbarCursor);
			ImGui::SetNextItemWidth(Width);
			ToolbarCursor.x += Width + ImGui::GetStyle().ItemSpacing.x;
		};
		auto ComboWidth = [](const char* const* Labels, int32 Count)
		{
			float TextWidth = 0.0f;
			for (int32 Index = 0; Index < Count; ++Index)
				TextWidth = std::max(TextWidth, ImGui::CalcTextSize(Labels[Index]).x);
			return TextWidth + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.0f;
		};
		auto ButtonWidth = [](const char* Label)
		{
			return ImGui::CalcTextSize(Label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
		};

		ImGui::PushID(100 + ViewIndex);
		int SelectedPreset = static_cast<int>(CurrentCameraPresets[ViewIndex]);
		PlaceToolbarItem(ComboWidth(CameraPresetLabels, IM_ARRAYSIZE(CameraPresetLabels)));
		if (ImGui::Combo("##CameraPreset", &SelectedPreset, CameraPresetLabels, IM_ARRAYSIZE(CameraPresetLabels)))
		{
			PendingCameraPresetViewIndex = ViewIndex;
			PendingCameraPreset = static_cast<EMultipleViewportsCameraPreset>(SelectedPreset);
		}
		// 레이아웃과 독립적으로 각 View의 View Mode(셰이딩·Wireframe)를 편집한다.
		if (ViewportAdapter)
		{
			int Mode = static_cast<int>(ViewportAdapter->GetViewMode(ViewIndex));
			PlaceToolbarItem(ComboWidth(ViewModeLabels, IM_ARRAYSIZE(ViewModeLabels)));
			if (ImGui::Combo("##ViewMode", &Mode, ViewModeLabels, IM_ARRAYSIZE(ViewModeLabels)))
			{
				ViewportAdapter->SetViewMode(ViewIndex, static_cast<EViewMode>(Mode));
			}

			PlaceToolbarItem(ButtonWidth("Show") + ImGui::GetFrameHeight());
			if (ImGui::BeginCombo("##ShowFlags", "Show", ImGuiComboFlags_HeightLargest))
			{
				const struct
				{
					const char* Label;
					EShowFlags Flag;
				} ShowItems[] =
				{
					{"Primitives", EShowFlags::Primitives},
					{"Bounding Box", EShowFlags::BoundingBox},
					{"Object UUID", EShowFlags::UUID},
					{"Batch Line / Grid", EShowFlags::BatchLine},
					{"PS Grid", EShowFlags::PSGrid},
					{"Exponential Height Fog", EShowFlags::ExponentialHeightFog},
					{"Shadows", EShowFlags::Shadows}
				};
				for (const auto& Item : ShowItems)
				{
					bool bEnabled = ViewportAdapter->IsShowFlagEnabled(ViewIndex, Item.Flag);
					if (ImGui::Checkbox(Item.Label, &bEnabled))
					{
						ViewportAdapter->SetShowFlag(ViewIndex, Item.Flag, bEnabled);
					}
				}
				ImGui::EndCombo();
			}
		}
		if (CurrentLayoutMode == ELayoutMode::QuadSplit)
		{
			PlaceToolbarItem(ButtonWidth("Single"));
			if (ImGui::Button("Single"))
			{
				RequestedLayoutMode = ELayoutMode::Single;
				RequestedSingleViewIndex = ViewIndex;
				bHasLayoutRequest = true;
			}
		}
		else if (ViewIndex == CurrentSingleViewIndex)
		{
			PlaceToolbarItem(ButtonWidth("Quad"));
			if (ImGui::Button("Quad"))
			{
				RequestedLayoutMode = ELayoutMode::QuadSplit;
				RequestedSingleViewIndex = ViewIndex;
				bHasLayoutRequest = true;
			}
		}
		if (ViewportAdapter && ViewIndex == ViewportAdapter->GetEditorViewIndex() &&
			FStatOverlay::IsAnyEnabled() &&
			(FStatOverlay::IsEnabled(EStatFlags::Profile) ||
				FStatOverlay::IsEnabled(EStatFlags::Shadow)))
		{
			PlaceToolbarItem(ButtonWidth("Reset Stats"));
			if (ImGui::Button("Reset Stats"))
				FStatOverlay::ResetSamples();
			StatResetButtonMin = ImGui::GetItemRectMin();
			StatResetButtonMax = ImGui::GetItemRectMax();
			bStatResetButtonVisible = true;
		}
		ToolbarExtraHeight[ViewIndex] = ToolbarCursor.y - ToolbarOrigin.y;
		ImGui::PopID();
		ImGui::PopClipRect();
	}

	// 마지막으로 선택된 뷰포트만 오버레이
	for (int32 ViewIndex = 0; ViewIndex < 4; ++ViewIndex)
	{
		if (!ViewportAdapter || ViewIndex != ViewportAdapter->GetEditorViewIndex()) continue;
		if (!Slots[ViewIndex].bActive)
			continue;
		const FRect& Rect = Slots[ViewIndex].Rect;
		const ImVec2 ViewMin{ContentOrigin.x + Rect.X, ContentOrigin.y + Rect.Y};
		DrawList->PushClipRect(ViewMin, {ViewMin.x + Rect.Width, ViewMin.y + Rect.Height}, true);
		DrawStatOverlay(DrawList, {
			ViewMin.x, ViewMin.y + ToolbarExtraHeight[ViewIndex]});
		DrawList->PopClipRect();
	}

	ImGui::End();
	ImGui::PopStyleVar();
}

// 항목별 줄을 모아 한 번에 배경과 텍스트를 그린다.
void FViewportsPanel::DrawStatOverlay(ImDrawList* DrawList, const ImVec2& ViewMin) const
{
	if (!DrawList || !FStatOverlay::IsAnyEnabled())
		return;

	constexpr float BytesPerMegabyte = 1024.0f * 1024.0f;

	// 제목은 UE처럼 노란색, 값은 흰색으로 구분한다.
	struct FStatLine
	{
		FString Text;
		ImU32 Color;
	};
	TArray<FStatLine> Lines;

	if (FStatOverlay::IsEnabled(EStatFlags::FPS) && !FStatOverlay::IsEnabled(EStatFlags::Shadow))
	{
		Lines.Add({"FPS", TitleColor});
		Lines.Add({std::format("  {:.1f} fps", FStatOverlay::GetFPS()), ValueColor});
		Lines.Add({std::format("  {:.2f} ms", FStatOverlay::GetFrameTimeMs()), ValueColor});
	}

	if (FStatOverlay::IsEnabled(EStatFlags::Memory))
	{
		Lines.Add({"Memory", TitleColor});
		Lines.Add({std::format("  Object  {:.2f} MB ({} allocs)",
			static_cast<double>(FStatOverlay::GetObjectAllocationBytes()) / BytesPerMegabyte,
			FStatOverlay::GetObjectAllocationCount()), ValueColor});
		Lines.Add({std::format("  Process {:.2f} MB",
			static_cast<double>(FStatOverlay::GetProcessWorkingSetBytes()) / BytesPerMegabyte), ValueColor});
	}

	struct FProfileStatLine
	{
		FString Text;
		ImU32 Color;
		FString Values[3];
		bool bFullWidth = false;
		bool bCurrentValue = false;
	};
	TArray<FProfileStatLine> ProfileLines;
	if (FStatOverlay::IsEnabled(EStatFlags::Profile))
	{
		ProfileLines.Add({"CPU Profile (ms)", TitleColor, {"Last", "Avg", "Max"}});

		for (const auto& [Name, Data] : FStatRegistry::GetAll())
		{
			const TStatId StatId{ Name };

			if (StatId == EditorStats::STAT_PickingTime)
				continue;

			if (FStatOverlay::IsEnabled(EStatFlags::Shadow) && ShadowStats::IsShadowStat(StatId))
				continue;

			FProfileStatLine Line{"  " + FString(Name), ValueColor};
			Line.Values[0] = std::format("{:.2f}", Data.GetLastMs());
			Line.Values[1] = std::format("{:.2f}", Data.GetRecentAverageMs());
			Line.Values[2] = std::format("{:.2f}", Data.GetMaxMs());
			ProfileLines.Add(Line);
		}
	}

    if (FStatOverlay::IsEnabled(EStatFlags::Shadow))
    {
        const auto AddInfo = [&ProfileLines](FString Text, ImU32 Color = ValueColor)
        {
            FProfileStatLine Line{std::move(Text), Color};
            Line.bFullWidth = true;
            ProfileLines.Add(Line);
        };
        const auto& Stats = ShadowStats::Completed;
        AddInfo(std::format("Shadow | {:.1f} FPS | {} View(s)", FStatOverlay::GetFPS(), Stats.Views), TitleColor);
        AddInfo("Avg: 120 frames | Max: since Reset", IM_COL32(175, 180, 190, 255));
        ProfileLines.Add({"Time (ms)", TitleColor, {"Last", "Avg", "Max"}});
        const auto& Frame = FStatOverlay::GetFrameTiming();
        ProfileLines.Add({"  Frame", ValueColor, {
            std::format("{:.2f}", Frame.GetLastMs()),
            std::format("{:.2f}", Frame.GetRecentAverageMs()),
            std::format("{:.2f}", Frame.GetMaxMs())}});
        if (Stats.bGPUTimeAvailable)
        {
            ProfileLines.Add({"  Shadow Map GPU", ValueColor, {
                std::format("{:.2f}", Stats.GPULastMs),
                std::format("{:.2f}", Stats.GPUAverageMs),
                std::format("{:.2f}", Stats.GPUMaxMs)}});
        }
        else
        {
            ProfileLines.Add({"  Shadow Map GPU", ValueColor, {"N/A", "N/A", "N/A"}});
        }
        AddInfo("  GPU: delayed, depth maps only", IM_COL32(175, 180, 190, 255));
        AddInfo("");
        AddInfo("Shadow workload / resources", TitleColor);
        const auto AddCurrent = [&ProfileLines](const char* Label, FString Value)
        {
            FProfileStatLine Line{"  " + FString(Label), ValueColor, {"", "", std::move(Value)}};
            Line.bCurrentValue = true;
            ProfileLines.Add(Line);
        };
        AddCurrent("Memory (estimated)", Stats.bResourcesAvailable ?
            std::format("{:.1f} MB", static_cast<double>(Stats.EstimatedBytes) / BytesPerMegabyte) : "N/A");
        AddCurrent("Lights D / P / S", std::format("{} / {} / {}", Stats.Lights[0], Stats.Lights[1], Stats.Lights[2]));
        const uint64 Updates = uint64(Stats.UpdatedLights[0]) + Stats.UpdatedLights[1] + Stats.UpdatedLights[2];
        const uint64 Skips = uint64(Stats.SkippedLights[0]) + Stats.SkippedLights[1] + Stats.SkippedLights[2];
        AddCurrent("Updates / Skipped", std::format("{} / {}", Updates, Skips));
        AddCurrent("Casters Culled / Tested", std::format("{} / {}", Stats.CastersCulled, Stats.CastersTested));
        AddInfo("  Updates/light-view | Casters/query", IM_COL32(175, 180, 190, 255));
    }

	if (FStatOverlay::IsEnabled(EStatFlags::Profile))
	{
		if (const FCycleStatData* PickingData = FStatRegistry::Find(EditorStats::STAT_PickingTime))
		{
			ProfileLines.Add({ "Picking", TitleColor });
			FProfileStatLine Line{"  Time (ms)", ValueColor};
			Line.Values[0] = std::format("Last: {:.2f}", PickingData->GetLastMs());
			Line.Values[1] = std::format("Attempts: {}", PickingData->CallCount);
			Line.Values[2] = std::format("Acc.: {:.2f}", PickingData->GetTotalMs());
			ProfileLines.Add(Line);
		}
	}

	if (Lines.Num() == 0 && ProfileLines.Num() == 0)
		return;

	// 제어 위젯 한 줄 아래에서 시작해 Combo와 겹치지 않게 한다.
	const float LineHeight = ImGui::GetTextLineHeight();
	const ImVec2 Origin{
		ViewMin.x + StatOverlayMargin,
		ViewMin.y + StatOverlayMargin + ImGui::GetFrameHeight() + StatOverlayMargin};

	// 최대 너비 계산
	float MaxWidth = 0.0f;
	for (int32 Index = 0; Index < Lines.Num(); ++Index)
		MaxWidth = std::max(MaxWidth, ImGui::CalcTextSize(Lines[Index].Text.c_str()).x);

	const float MinValueWidth = ImGui::CalcTextSize("000.00").x;
	float NameWidth = 0.0f;
	float ValueWidths[3] = { MinValueWidth, MinValueWidth, MinValueWidth };
	bool bHasColumns = false;

	for (const FProfileStatLine& Line : ProfileLines)
	{
		const float TextWidth = ImGui::CalcTextSize(Line.Text.c_str()).x;
		if (Line.bFullWidth)
		{
			MaxWidth = std::max(MaxWidth, TextWidth);
			continue;
		}
		if (Line.bCurrentValue)
		{
			MaxWidth = std::max(MaxWidth, TextWidth + ImGui::GetFontSize() + ImGui::CalcTextSize(Line.Values[2].c_str()).x);
			continue;
		}
		bHasColumns = true;
		NameWidth = std::max(NameWidth, TextWidth);
		for (int32 Column = 0; Column < 3; ++Column)
			ValueWidths[Column] = std::max(ValueWidths[Column], ImGui::CalcTextSize(Line.Values[Column].c_str()).x);
	}

	float ColumnRight[3]{};
	if (bHasColumns)
	{
		float Width = NameWidth;
		for (int32 Column = 0; Column < 3; ++Column)
		{
			Width += ImGui::GetFontSize() + ValueWidths[Column];
			ColumnRight[Column] = Origin.x + Width;
		}
		MaxWidth = std::max(MaxWidth, Width);
	}

	const ImVec2 BackgroundMin{Origin.x - StatOverlayPadding, Origin.y - StatOverlayPadding};
	const ImVec2 BackgroundMax{
		Origin.x + MaxWidth + StatOverlayPadding,
		Origin.y + LineHeight * static_cast<float>(Lines.Num() + ProfileLines.Num()) + StatOverlayPadding};
	DrawList->AddRectFilled(BackgroundMin, BackgroundMax, StatOverlayBackgroundColor, 4.0f);

	// Lines 그리기
	for (int32 Index = 0; Index < Lines.Num(); ++Index)
	{
		DrawList->AddText(
			{ Origin.x, Origin.y + LineHeight * static_cast<float>(Index) },
			Lines[Index].Color,
			Lines[Index].Text.c_str());
	}

	// ProfileLines 그리기
	for (int32 Index = 0; Index < ProfileLines.Num(); ++Index)
	{
		const FProfileStatLine& Line = ProfileLines[Index];
		const float Y = Origin.y + LineHeight * static_cast<float>(Lines.Num() + Index);
		DrawList->AddText({ Origin.x, Y }, Line.Color, Line.Text.c_str());
		if (Line.bFullWidth)
			continue;
		if (Line.bCurrentValue)
		{
			const FString& Text = Line.Values[2];
			DrawList->AddText({Origin.x + MaxWidth - ImGui::CalcTextSize(Text.c_str()).x, Y}, Line.Color, Text.c_str());
			continue;
		}

		for (int32 Column = 0; Column < 3; ++Column)
		{
			const FString& Text = Line.Values[Column];
			const float TextWidth = ImGui::CalcTextSize(Text.c_str()).x;
			DrawList->AddText({ ColumnRight[Column] - TextWidth, Y }, Line.Color, Text.c_str());
		}
	}
}

// View 크기에 맞춰 Color·Depth Texture와 렌더 정보를 재생성한다.
void FViewportsPanel::ResizeSlot(FViewSlot& Slot, const uint32 Width, const uint32 Height)
{
	D3D11_TEXTURE2D_DESC Desc{};
	Desc.Width = Width;
	Desc.Height = Height;
	Desc.MipLevels = 1;
	Desc.ArraySize = 1;
	Desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	Desc.SampleDesc.Count = 1;
	Desc.Usage = D3D11_USAGE_DEFAULT;
	Desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	Slot.ColorTarget = RenderCommand::CreateTexture2D(Desc);
	// CopyResource로 되돌려 쓰므로 ColorTarget과 같은 Desc로 만든다
	Slot.FxaaTarget = RenderCommand::CreateTexture2D(Desc);

	// Fog 패스가 깊이를 셰이더에서 읽으므로 DSV와 SRV를 둘 다 만들 수 있는 TYPELESS로 만든다
	Desc.Format = DXGI_FORMAT_R24G8_TYPELESS;
	Desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
	Slot.DepthTarget = RenderCommand::CreateTexture2D(Desc);

	Slot.Width = Width;
	Slot.Height = Height;
	Slot.RenderingInfo.ColorRenderTargets.Reset();
	Slot.RenderingInfo.ViewportSetting.Width = Width;
	Slot.RenderingInfo.ViewportSetting.Height = Height;
	FRenderingDesc ColorDesc{};
	ColorDesc.Texture = Slot.ColorTarget.get();
	Slot.RenderingInfo.ColorRenderTargets.Add(ColorDesc);
	Slot.RenderingInfo.DepthStencil.Texture = Slot.DepthTarget.get();
}

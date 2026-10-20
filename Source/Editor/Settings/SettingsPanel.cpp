#include "EnginePCH.h"
#include "Editor/Settings/SettingsPanel.h"
#include "Editor/LevelEditor/MultipleViewports/Adapter/MultipleViewportsAdapter.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"

#include "Engine/World.h"

#include <limits>

// 종료 시 렌더·에디터·뷰포트 설정을 함께 저장한다.
FSettingsPanel::~FSettingsPanel()
{
	// 종료 시 다른 객체를 참조하지 않고 보존한 설정만 저장한다.
	ViewportAdapter = nullptr;
	SaveSettings();
}

// 설정 파일을 읽어 패널 상태를 초기화한다.
bool FSettingsPanel::Init()
{
	LoadSettings();
	return true;
}


// 설정 패널의 프레임 갱신 진입점이다.
void FSettingsPanel::Tick(float DeltaTime)
{
}

// ImGui 조작으로 렌더·카메라 설정과 저장·복원 요청을 처리한다.
void FSettingsPanel::OnRender()
{
	ImGui::SetNextWindowSize(ImVec2(400, 300), ImGuiCond_FirstUseEver);
	ImGui::Begin("Settings");

	//////////////////////////////////////////////////////////

	// 렌더링 옵션 (Toggles)
	ImGui::SeparatorText("Rendering");

	ImGui::TextDisabled("Fill mode is configured per viewport.");
	ImGui::Checkbox("FXAA", &Settings.bEnableFXAA);
	ImGui::Checkbox("Depth View", &Settings.bDepthView);
	ImGui::Checkbox("Tile-Based Light Culling", &Settings.bTileBasedLightCulling);
	ImGui::Checkbox("Exponential Height Fog", &Settings.bExponentialHeightFog);

    static const char *QualityLabels[] = {"Low", "Medium", "High", "Epic", "Box 3x3"};
    // UberLut.hlsl SHADOW_QUALITY랑 동일하게

    int Quality = static_cast<int>(Settings.ShadowQuality);
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::Combo("Shadow Quality", &Quality, QualityLabels, IM_ARRAYSIZE(QualityLabels)))
    {
        Settings.ShadowQuality = Quality;
    }
    ImGui::SliderFloat("Soft Transition", &Settings.TransitionFactor, 0.0f, 10.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	//////////////////////////////////////////////////////////

	ImGui::Dummy(ImVec2(0.0f, SectionGap));
	ImGui::SeparatorText("Grid");

	ImGui::SetNextItemWidth(200.0f);
	ImGui::SliderInt("Grid Spacing", &Settings.GridSpacing, 1, 100);
	//////////////////////////////////////////////////////////

	// 에디터 수치 설정 (Values)
	ImGui::Dummy(ImVec2(0.0f, SectionGap));
	ImGui::SeparatorText("Editor Settings");

	UCameraComponent* CamCom = World->GetMainCamera()->GetCameraComponent();

	ImGui::SetNextItemWidth(200.0f);
	ImGui::SliderFloat("Camera Rotate Sensitivity", &Settings.MouseSensitivity, 0.01f, 1.0f, "%.2f");
	CamCom->SetMouseSensitivity(Settings.MouseSensitivity);

	ImGui::SetNextItemWidth(200.0f);
	ImGui::SliderFloat("Camera Speed", &Settings.CameraSpeed, 0.1f, 10.0f, "%.2f");
	CamCom->SetMoveSpeed(Settings.CameraSpeed);

	//////////////////////////////////////////////////////////

	ImGui::Dummy(ImVec2(0.0f, SectionGap));
	ImGui::SeparatorText("Load Settings");

	if (ImGui::Button("Load Settings"))
	{
		LoadSettings();
	}

	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("Load settings from editor.ini");
	}

	ImGui::SameLine();

	if (ImGui::Button("Save Settings"))
	{
		SaveSettings();
	}

	//////////////////////////////////////////////////////////

	ImGui::End();
}

// 알려진 설정 섹션을 editor.ini에 함께 기록해 뷰포트 값 유실을 막는다.
bool FSettingsPanel::SaveSettings() const
{
	FEditorSettings Snapshot = Settings;
	ReadViewportSettings(Snapshot);
	std::ofstream File("editor.ini");

	if (!File.is_open())
	{
		HTR_LOG(Error, "Failed to open editor.ini for saving.");
		return false;
	}

	File << "[Rendering]\n";
	File << "EnableFXAA=" << Settings.bEnableFXAA << "\n";
    File << "ShadowQuality=" << Settings.ShadowQuality << "\n";
    File << "TransitionFactor=" << Settings.TransitionFactor << "\n";
	File << "\n";

	File << "[Editor]\n";
	File << "CameraMoveSpeed=" << Settings.CameraSpeed << "\n";
	File << "CameraRotateSensitivity=" << Settings.MouseSensitivity << "\n";
	File << "GridSpacing=" << Settings.GridSpacing << "\n";
	File << "\n";

	File << "[MultipleViewports]\n";
	File << "Horizontal=" << Snapshot.MultipleViewportsHorizontal << "\n";
	File << "Vertical=" << Snapshot.MultipleViewportsVertical << "\n";
	File << "Layout=" << (Snapshot.bMultipleViewportsSingle ? "Single" : "Quad") << "\n";
	File << "SingleViewIndex=" << Snapshot.MultipleViewportsSingleViewIndex << "\n";

	for (int32 Index = 0; Index < 4; ++Index)
    {
        File << "View" << Index << "Fov=" << Snapshot.ViewFov[Index] << "\n";
        File << "View" << Index << "OrthoWidth=" << Snapshot.ViewOrthoWidth[Index] << "\n";
        File << "View" << Index << "Preset=" << Snapshot.ViewPreset[Index] << "\n";
        File << "View" << Index << "ViewMode=" << Snapshot.ViewMode[Index] << "\n";
        const EShowFlags Flags = Snapshot.ViewShowFlags[Index] >= 0
            ? static_cast<EShowFlags>(Snapshot.ViewShowFlags[Index]) : FMultipleViewportsAdapter::DefaultShowFlags;
        File << "View" << Index << "ShowFlags=" << static_cast<uint32>(Flags) << "\n";
        File << "View" << Index << "LocationX=" << Snapshot.ViewLocation[Index].X << "\n";
        File << "View" << Index << "LocationY=" << Snapshot.ViewLocation[Index].Y << "\n";
        File << "View" << Index << "LocationZ=" << Snapshot.ViewLocation[Index].Z << "\n";
        File << "View" << Index << "RotationX=" << Snapshot.ViewRotation[Index].X << "\n";
        File << "View" << Index << "RotationY=" << Snapshot.ViewRotation[Index].Y << "\n";
        File << "View" << Index << "RotationZ=" << Snapshot.ViewRotation[Index].Z << "\n";
        File << "View" << Index << "RotationW=" << Snapshot.ViewRotation[Index].W << "\n";
    }
    File.close();
	return true;
}

// 파일의 키를 파싱해 렌더·카메라·레이아웃 설정을 복원한다.
bool FSettingsPanel::LoadSettings()
{
	std::ifstream File("editor.ini");
	if (!File.is_open())
	{
		return false;
	}

	uint8 LocationComponentMasks[4]{};
	uint8 RotationComponentMasks[4]{};
	EShowFlags LegacyShowFlags = FMultipleViewportsAdapter::DefaultShowFlags;
	for (int32 Index = 0; Index < 4; ++Index)
	{
		Settings.bViewLocationSaved[Index] = false;
		Settings.bViewRotationSaved[Index] = false;
		Settings.ViewShowFlags[Index] = -1;
	}

	std::string Line;
	while (std::getline(File, Line))
	{
		if (Line.empty() || Line[0] == ';' || Line[0] == '[') continue;

		std::istringstream Iss(Line);
		std::string Key;
		if (std::getline(Iss, Key, '='))
		{
			std::string ValueStr;
			if (std::getline(Iss, ValueStr))
			{
				// 새 View 키는 NaN·잘못된 숫자·범위 밖 값을 무시한다.
                for (int32 Index = 0; Index < 4; ++Index)
                {
                    FString Prefix = "View";
                    Prefix += static_cast<char>('0' + Index);
                    if (Key == Prefix + "ShowFlags")
                    {
                        std::istringstream FlagNumber(ValueStr);
                        int64 Value = -1;
                        if ((FlagNumber >> Value) && Value >= 0 && Value <= std::numeric_limits<uint32>::max() &&
                            (FlagNumber >> std::ws).eof())
                        {
                            Settings.ViewShowFlags[Index] = Value;
                        }
                        continue;
                    }
                    std::istringstream Number(ValueStr);
                    float Value = 0;
                    if (!(Number >> Value) || !std::isfinite(Value)) continue;
                    if (Key == Prefix + "Fov" && Value >= 1 && Value <= 179)
                        Settings.ViewFov[Index] = Value;
                    else if (Key == Prefix + "OrthoWidth" && Value >= 0.01f)
                        // 파일 값은 절대 상한으로 먼저 제한하고, 실제 View 비율 상한은 Adapter가 적용한다.
                        Settings.ViewOrthoWidth[Index] = FMath::Clamp(Value, 0.01f, 1000000.0f);
                    else if (Key == Prefix + "Preset" && Value >= 0 && Value <= 7 && Value == static_cast<int32>(Value))
                        Settings.ViewPreset[Index] = static_cast<int32>(Value);
                    else if (Key == Prefix + "ViewMode" && Value >= 0 && Value < static_cast<int32>(EViewMode::MAX) && Value == static_cast<int32>(Value))
                        Settings.ViewMode[Index] = static_cast<int32>(Value);
                    else if (Key == Prefix + "LocationX")
                    {
                        Settings.ViewLocation[Index].X = Value;
                        LocationComponentMasks[Index] |= 1 << 0;
                    }
                    else if (Key == Prefix + "LocationY")
                    {
                        Settings.ViewLocation[Index].Y = Value;
                        LocationComponentMasks[Index] |= 1 << 1;
                    }
                    else if (Key == Prefix + "LocationZ")
                    {
                        Settings.ViewLocation[Index].Z = Value;
                        LocationComponentMasks[Index] |= 1 << 2;
                    }
                    else if (Key == Prefix + "RotationX")
                    {
                        Settings.ViewRotation[Index].X = Value;
                        RotationComponentMasks[Index] |= 1 << 0;
                    }
                    else if (Key == Prefix + "RotationY")
                    {
                        Settings.ViewRotation[Index].Y = Value;
                        RotationComponentMasks[Index] |= 1 << 1;
                    }
                    else if (Key == Prefix + "RotationZ")
                    {
                        Settings.ViewRotation[Index].Z = Value;
                        RotationComponentMasks[Index] |= 1 << 2;
                    }
                    else if (Key == Prefix + "RotationW")
                    {
                        Settings.ViewRotation[Index].W = Value;
                        RotationComponentMasks[Index] |= 1 << 3;
                    }
                }
				EShowFlags LegacyFlag = EShowFlags::None;
                if (Key == "DrawPrimitives") LegacyFlag = EShowFlags::Primitives;
				else if (Key == "DrawBoundingBox") LegacyFlag = EShowFlags::BoundingBox;
				else if (Key == "ShowUUID") LegacyFlag = EShowFlags::UUID;
				else if (Key == "DrawBatchLine") LegacyFlag = EShowFlags::BatchLine;
				else if (Key == "DrawPSGrid") LegacyFlag = EShowFlags::PSGrid;
				else if (Key == "ExponentialHeightFog") LegacyFlag = EShowFlags::ExponentialHeightFog;
				else if (Key == "ShowShadows") LegacyFlag = EShowFlags::Shadows;
				if (LegacyFlag != EShowFlags::None)
				{
					LegacyShowFlags = std::stoi(ValueStr) != 0 ? (LegacyShowFlags | LegacyFlag) : (LegacyShowFlags & ~LegacyFlag);
				}
				else if (Key == "EnableFXAA") Settings.bEnableFXAA = std::stoi(ValueStr);
                else if (Key == "ShadowQuality") Settings.ShadowQuality = std::stoi(ValueStr);
                else if (Key == "TransitionFactor") Settings.TransitionFactor = std::stof(ValueStr);

				else if (Key == "CameraMoveSpeed") Settings.CameraSpeed = std::stof(ValueStr);
				else if (Key == "CameraRotateSensitivity") Settings.MouseSensitivity = std::stof(ValueStr);
				else if (Key == "GridSpacing") Settings.GridSpacing = std::stof(ValueStr);
				else if (Key == "Horizontal") Settings.MultipleViewportsHorizontal = std::stof(ValueStr);
				else if (Key == "Vertical") Settings.MultipleViewportsVertical = std::stof(ValueStr);
				else if (Key == "Layout") Settings.bMultipleViewportsSingle = ValueStr == "Single";
				else if (Key == "SingleViewIndex")
				{
					const int32 ViewIndex = std::stoi(ValueStr);
					Settings.MultipleViewportsSingleViewIndex = ViewIndex >= 0 && ViewIndex < 4 ? ViewIndex : 0;
				}
			}
		}
	}

	File.close();
	for (int32 Index = 0; Index < 4; ++Index)
	{
		// 새 View 값이 있으면 파일 내 공통 키의 순서와 무관하게 우선한다.
		if (Settings.ViewShowFlags[Index] < 0)
			Settings.ViewShowFlags[Index] = static_cast<uint32>(LegacyShowFlags);
		Settings.bViewLocationSaved[Index] = LocationComponentMasks[Index] == 0x7;
		if (RotationComponentMasks[Index] == 0xF)
		{
			const FQuat& Rotation = Settings.ViewRotation[Index];
			const float LengthSquared = Rotation.X * Rotation.X + Rotation.Y * Rotation.Y +
				Rotation.Z * Rotation.Z + Rotation.W * Rotation.W;
			if (LengthSquared > 1.0e-12f)
			{
				Settings.ViewRotation[Index] = Rotation.Normalized();
				Settings.bViewRotationSaved[Index] = true;
			}
		}
	}
	ApplyViewportSettings();
	return true;
}

// 초기화된 카메라에 파일 설정을 적용하고 미저장 값은 초기 설정으로 채운다.
void FSettingsPanel::SetViewportAdapter(FMultipleViewportsAdapter* Value)
{
    ViewportAdapter = Value;
    ApplyViewportSettings();
    CaptureViewportSettings();
}

// Transform·투영·표시·레이아웃을 저장용 설정에 복사한다.
void FSettingsPanel::ReadViewportSettings(FEditorSettings& Out) const
{
    if (!ViewportAdapter) return;
    Out.MultipleViewportsHorizontal = ViewportAdapter->GetSplitRatio().Horizontal;
    Out.MultipleViewportsVertical = ViewportAdapter->GetSplitRatio().Vertical;
    Out.bMultipleViewportsSingle = ViewportAdapter->GetLayoutMode() == ELayoutMode::Single;
    Out.MultipleViewportsSingleViewIndex = ViewportAdapter->GetSingleViewIndex();
    for (int32 Index = 0; Index < 4; ++Index)
    {
        const auto& Camera = ViewportAdapter->GetViewCamera(Index);
        Out.ViewFov[Index] = Camera.Projection.FovDegrees;
        Out.ViewOrthoWidth[Index] = Camera.Projection.OrthoWidth;
        Out.ViewPreset[Index] = static_cast<int32>(ViewportAdapter->GetCameraPreset(Index));
        Out.ViewMode[Index] = static_cast<int32>(ViewportAdapter->GetViewMode(Index));
        Out.ViewShowFlags[Index] = static_cast<uint32>(ViewportAdapter->GetShowFlags(Index));
        Out.ViewLocation[Index] = Camera.Transform.Location;
        Out.ViewRotation[Index] = Camera.Transform.Rotation;
        Out.bViewLocationSaved[Index] = true;
        Out.bViewRotationSaved[Index] = true;
    }
}

// 종료 시 Adapter를 읽지 않도록 살아 있는 동안 저장용 설정을 갱신한다.
void FSettingsPanel::CaptureViewportSettings() { ReadViewportSettings(Settings); }

// 저장 프리셋·투영·표시를 슬롯별로 복원하며 미저장 ViewMode는 기본값을 사용한다.
void FSettingsPanel::ApplyViewportSettings()
{
    if (!ViewportAdapter) return;
    ViewportAdapter->SetSplitRatio({Settings.MultipleViewportsHorizontal, Settings.MultipleViewportsVertical});
    ViewportAdapter->SetSingleViewIndex(Settings.MultipleViewportsSingleViewIndex);
    ViewportAdapter->SetLayoutMode(Settings.bMultipleViewportsSingle ? ELayoutMode::Single : ELayoutMode::QuadSplit);
    for (int32 Index = 0; Index < 4; ++Index)
    {
        if (Settings.ViewPreset[Index] >= 0 && Settings.ViewPreset[Index] <= 7)
            ViewportAdapter->ApplyCameraPreset(Index, static_cast<EMultipleViewportsCameraPreset>(Settings.ViewPreset[Index]));
        FViewCamera Camera = ViewportAdapter->GetViewCamera(Index);
        if (Settings.ViewFov[Index] > 0) Camera.Projection.FovDegrees = Settings.ViewFov[Index];
        if (Settings.ViewOrthoWidth[Index] > 0) Camera.Projection.OrthoWidth = Settings.ViewOrthoWidth[Index];
        if (Settings.bViewLocationSaved[Index]) Camera.Transform.Location = Settings.ViewLocation[Index];
        if (Settings.bViewRotationSaved[Index]) Camera.Transform.Rotation = Settings.ViewRotation[Index];
        ViewportAdapter->ApplyCameraProperties(Index, Camera, false);
        EViewMode Mode = EViewMode::Lit_Phong;
        if (Settings.ViewMode[Index] >= 0)
            Mode = static_cast<EViewMode>(Settings.ViewMode[Index]);
        ViewportAdapter->SetViewMode(Index, Mode);
        const EShowFlags Flags = Settings.ViewShowFlags[Index] >= 0
            ? static_cast<EShowFlags>(Settings.ViewShowFlags[Index]) : FMultipleViewportsAdapter::DefaultShowFlags;
        ViewportAdapter->SetShowFlag(Index, ViewportAdapter->GetShowFlags(Index), false);
        ViewportAdapter->SetShowFlag(Index, Flags, true);
    }
}

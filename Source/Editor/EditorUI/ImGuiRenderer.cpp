#include "EnginePCH.h"
#include "Editor/EditorUI/ImGuiRenderer.h"

#include "Core/Window.h"
#include "Input/InputSystem.h"

#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace
{
	// FWindow가 ImGui를 직접 알지 않도록 메시지 처리기를 훅으로 등록한다.
	bool ImGuiWndProcHook(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		return ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam) != 0;
	}

	void ApplyDefaultStyle()
	{
		ImGuiStyle* style = &ImGui::GetStyle();
		ImVec4* colors = style->Colors;
		const ImVec4 Accent(0.65f, 0.30f, 0.45f, 1.00f);
		const ImVec4 AccentHover(0.75f, 0.40f, 0.55f, 1.00f);
		const ImVec4 AccentActive(0.55f, 0.20f, 0.35f, 1.00f);

		colors[ImGuiCol_Text] = ImVec4(1.000f, 1.000f, 1.000f, 1.000f);
		colors[ImGuiCol_TextDisabled] = ImVec4(0.500f, 0.500f, 0.500f, 1.000f);
		colors[ImGuiCol_TextLink] = AccentHover;
		colors[ImGuiCol_WindowBg] = ImVec4(0.180f, 0.180f, 0.180f, 1.000f);
		colors[ImGuiCol_ChildBg] = ImVec4(0.280f, 0.280f, 0.280f, 0.000f);
		colors[ImGuiCol_PopupBg] = ImVec4(0.313f, 0.313f, 0.313f, 1.000f);
		colors[ImGuiCol_Border] = ImVec4(0.266f, 0.266f, 0.266f, 1.000f);
		colors[ImGuiCol_BorderShadow] = ImVec4(0.000f, 0.000f, 0.000f, 0.000f);
		colors[ImGuiCol_FrameBg] = ImVec4(0.160f, 0.160f, 0.160f, 1.000f);
		colors[ImGuiCol_FrameBgHovered] = ImVec4(0.200f, 0.200f, 0.200f, 1.000f);
		colors[ImGuiCol_FrameBgActive] = ImVec4(0.280f, 0.280f, 0.280f, 1.000f);
		colors[ImGuiCol_InputTextCursor] = colors[ImGuiCol_Text];
		colors[ImGuiCol_TitleBg] = ImVec4(0.148f, 0.148f, 0.148f, 1.000f);
		colors[ImGuiCol_TitleBgActive] = ImVec4(0.148f, 0.148f, 0.148f, 1.000f);
		colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.148f, 0.148f, 0.148f, 1.000f);
		colors[ImGuiCol_MenuBarBg] = ImVec4(0.195f, 0.195f, 0.195f, 1.000f);
		colors[ImGuiCol_ScrollbarBg] = ImVec4(0.160f, 0.160f, 0.160f, 1.000f);
		colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.277f, 0.277f, 0.277f, 1.000f);
		colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.360f, 0.360f, 0.360f, 1.000f);
		colors[ImGuiCol_ScrollbarGrabActive] = AccentActive;
		colors[ImGuiCol_CheckboxSelectedBg] = ImVec4(Accent.x, Accent.y, Accent.z, 0.80f);
		colors[ImGuiCol_CheckMark] = ImVec4(1.000f, 1.000f, 1.000f, 1.000f);
		colors[ImGuiCol_SliderGrab] = ImVec4(0.391f, 0.391f, 0.391f, 1.000f);
		colors[ImGuiCol_SliderGrabActive] = AccentHover;
		colors[ImGuiCol_Button] = ImVec4(Accent.x, Accent.y, Accent.z, 0.80f);
		colors[ImGuiCol_ButtonHovered] = AccentHover;
		colors[ImGuiCol_ButtonActive] = AccentActive;
		colors[ImGuiCol_Header] = ImVec4(0.313f, 0.313f, 0.313f, 1.000f);
		colors[ImGuiCol_HeaderHovered] = ImVec4(0.400f, 0.400f, 0.400f, 1.000f);
		colors[ImGuiCol_HeaderActive] = ImVec4(0.469f, 0.469f, 0.469f, 1.000f);
		colors[ImGuiCol_Separator] = colors[ImGuiCol_Border];
		colors[ImGuiCol_SeparatorHovered] = ImVec4(0.391f, 0.391f, 0.391f, 1.000f);
		colors[ImGuiCol_SeparatorActive] = Accent;
		colors[ImGuiCol_ResizeGrip] = ImVec4(1.000f, 1.000f, 1.000f, 0.250f);
		colors[ImGuiCol_ResizeGripHovered] = ImVec4(1.000f, 1.000f, 1.000f, 0.670f);
		colors[ImGuiCol_ResizeGripActive] = Accent;
		colors[ImGuiCol_Tab] = ImVec4(0.098f, 0.098f, 0.098f, 1.000f);
		colors[ImGuiCol_TabHovered] = ImVec4(0.352f, 0.352f, 0.352f, 1.000f);
		colors[ImGuiCol_TabSelected] = ImVec4(0.250f, 0.250f, 0.250f, 1.000f);
		colors[ImGuiCol_TabDimmed] = ImVec4(0.098f, 0.098f, 0.098f, 1.000f);
		colors[ImGuiCol_TabDimmedSelected] = ImVec4(0.195f, 0.195f, 0.195f, 1.000f);
		colors[ImGuiCol_TabSelectedOverline] = Accent;
		colors[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.40f, 0.30f, 0.35f, 1.00f);
		colors[ImGuiCol_DockingPreview] = ImVec4(Accent.x, Accent.y, Accent.z, 0.65f);
		colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.180f, 0.180f, 0.180f, 1.000f);
		colors[ImGuiCol_PlotLines] = ImVec4(0.469f, 0.469f, 0.469f, 1.000f);
		colors[ImGuiCol_PlotLinesHovered] = AccentHover;
		colors[ImGuiCol_PlotHistogram] = ImVec4(0.586f, 0.586f, 0.586f, 1.000f);
		colors[ImGuiCol_PlotHistogramHovered] = AccentHover;
		colors[ImGuiCol_TableHeaderBg] = ImVec4(0.240f, 0.240f, 0.240f, 1.000f);
		colors[ImGuiCol_TableBorderStrong] = colors[ImGuiCol_Border];
		colors[ImGuiCol_TableBorderLight] = ImVec4(0.220f, 0.220f, 0.220f, 1.000f);
		colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
		colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.03f);
		colors[ImGuiCol_TextSelectedBg] = ImVec4(1.000f, 1.000f, 1.000f, 0.156f);
		colors[ImGuiCol_TreeLines] = colors[ImGuiCol_Border];
		colors[ImGuiCol_DragDropTarget] = AccentHover;
		colors[ImGuiCol_DragDropTargetBg] = ImVec4(Accent.x, Accent.y, Accent.z, 0.18f);
		colors[ImGuiCol_UnsavedMarker] = Accent;
		colors[ImGuiCol_NavCursor] = Accent;
		colors[ImGuiCol_NavWindowingHighlight] = Accent;
		colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.000f, 0.000f, 0.000f, 0.586f);
		colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.000f, 0.000f, 0.000f, 0.586f);

		// 패딩과 위젯 크기는 기준 배율로 정의하고 Init에서 모니터 DPI를 적용한다.
		style->WindowPadding = ImVec2(10.0f, 10.0f);
		style->FramePadding = ImVec2(8.0f, 4.0f);
		style->ItemSpacing = ImVec2(8.0f, 6.0f);
		style->ItemInnerSpacing = ImVec2(6.0f, 4.0f);
		style->CellPadding = ImVec2(6.0f, 4.0f);
		style->IndentSpacing = 18.0f;
		style->WindowMinSize = ImVec2(160.0f, 100.0f);
		style->ChildRounding = 4.0f;
		style->FrameBorderSize = 1.0f;
		style->FrameRounding = 3.0f;
		style->GrabMinSize = 10.0f;
		style->PopupRounding = 4.0f;
		style->ScrollbarRounding = 7.0f;
		style->ScrollbarSize = 14.0f;
		style->TabBorderSize = 1.0f;
		style->TabRounding = 3.0f;
		style->WindowRounding = 4.0f;
	}
}

FImGuiRenderer::~FImGuiRenderer()
{

}

bool FImGuiRenderer::Init(HWND WindowHandle, ID3D11Device* Device, ID3D11DeviceContext* DeviceContext)
{
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();

	ImGuiIO& io = ImGui::GetIO(); (void)io;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

	ImGui_ImplWin32_Init(WindowHandle);
	ImGui_ImplDX11_Init(Device, DeviceContext);
	FWindow::SetWndProcHook(&ImGuiWndProcHook);

	ApplyDefaultStyle();

	const float DpiScale = ImGui_ImplWin32_GetDpiScaleForHwnd(WindowHandle);
	ImGuiStyle& Style = ImGui::GetStyle();
	Style.ScaleAllSizes(DpiScale);
	Style.FontScaleDpi = DpiScale;
	io.ConfigDpiScaleFonts = true;
	io.ConfigDpiScaleViewports = true;

	return true;
}

void FImGuiRenderer::Begin()
{
	ImGui_ImplDX11_NewFrame();
	ImGui_ImplWin32_NewFrame();

	ImGui::NewFrame();

	ImGuiIO& io = ImGui::GetIO(); (void)io;
}

void FImGuiRenderer::End()
{
	ImGuiIO& io = ImGui::GetIO();

	ImGui::Render();
	ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

	ImGui::UpdatePlatformWindows();
	ImGui::RenderPlatformWindowsDefault();
}

void FImGuiRenderer::Shutdown()
{
	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
}

#pragma once

#include "Shader.h"
#include <d3dcommon.h>
#include "Containers/Array.h"
#include "Core/EngineString.h"



class FRenderResourceManager
{
public:

	struct FShaderPermutation
	{
		FString Name;                          // "PHONG"
		TArray<D3D_SHADER_MACRO> Defines;      // { {"LIGHTING_MODEL_PHONG","1"}, {nullptr,nullptr} }
	};


	static FRenderResourceManager& Get()
	{
		static FRenderResourceManager* Instance = new FRenderResourceManager();
		return *Instance;
	}

	static void Init()
	{
		//Uber Shader Compile & Generate Shader Program

		Get().PermutationTable["Resources/Shader/UberLit.hlsl"] = {
			{ "GOURAUD",		{ {"LIGHTING_MODEL_GOURAUD","1"},	{nullptr,nullptr} } },
			{ "LAMBERT",		{ {"LIGHTING_MODEL_LAMBERT","1"},	{nullptr,nullptr} } },
			{ "PHONG",			{ {"LIGHTING_MODEL_PHONG","1"},		{nullptr,nullptr} } },
			{ "UNLIT",			{ {"LIGHTING_MODEL_UNLIT","1"},		{nullptr,nullptr} } },
			{ "WORLDNORMAL",	{ {"VIEWMODE_WORLDNORMAL","1"},		{nullptr,nullptr} } },
			{ "LIGHTCULLING",	{ {"VIEWMODE_LIGHTCULLING","1"},	{nullptr,nullptr} } },
		};

		Get().ScanShaders("Resources/Shader");
	}

	void ScanShaders(const fs::path& ShaderRoot);
	void LoadShaderWithDefines(const FString& ShaderPath, const TArray<FShaderPermutation> &Perms);

	static void Shutdown();

	static FShaderProgram* GetShaderProgram(const FString& InPath, const FString &Macro = "");
private:
	void LoadOrCompileShader(const FString& Path, const D3D_SHADER_MACRO* defines = nullptr, const FString& AdditionalName = "");

	TMap<FString, TUniquePtr<FVertexShader>> VertexShaderMap;
	TMap<FString, TUniquePtr<FPixelShader>>  PixelShaderMap;
	TMap<FString, TUniquePtr<FShaderProgram>> ShaderProgramMap;
	TMap<FString, TArray<FShaderPermutation>> PermutationTable;
}; 
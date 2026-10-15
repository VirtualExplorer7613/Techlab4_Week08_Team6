#include "EnginePCH.h"
#include "StaticMeshData.h"

#include <algorithm>
#include <cmath>
#include <cfloat>
#include <format>
#include <vector>

namespace
{
	uint32 CountVertexCacheMisses(const uint32* Indices, uint32 IndexCount, uint32 CacheSize)
	{
		std::vector<uint32> Cache;
		Cache.reserve(CacheSize);
		uint32 Misses = 0;
		for (uint32 I = 0; I < IndexCount; ++I)
		{
			const uint32 Vertex = Indices[I];
			const auto It = std::find(Cache.begin(), Cache.end(), Vertex);
			if (It == Cache.end())
				++Misses;
			else
				Cache.erase(It);
			Cache.insert(Cache.begin(), Vertex);
			if (Cache.size() > CacheSize)
				Cache.pop_back();
		}
		return Misses;
	}
}

bool FStaticMeshData::Validate(FString& OutError) const
{
	const uint32 VertexCount = static_cast<uint32>(Vertices.Num());
	const uint32 IndexCount = static_cast<uint32>(Indices.Num());

	if (VertexCount == 0 || IndexCount == 0 || IndexCount % 3 != 0)
	{
		OutError = std::format("invalid mesh (vertices {}, indices {})", VertexCount, IndexCount);
		return false;
	}

	for (uint32 Index : Indices)
	{
		if (Index >= VertexCount)
		{
			OutError = std::format("index {} out of range (vertices {})", Index, VertexCount);
			return false;
		}
	}

	uint32 SectionIndexSum = 0;
	for (const FStaticMeshSection& Section : Sections)
	{
		if (Section.StartIndex + Section.IndexCount > IndexCount)
		{
			OutError = std::format("section range {}+{} exceeds indices {}", Section.StartIndex, Section.IndexCount, IndexCount);
			return false;
		}
		if (Section.MaterialSlotIndex >= static_cast<uint32>(MaterialSlots.Num()))
		{
			OutError = std::format("material slot {} out of range (slots {})", Section.MaterialSlotIndex, MaterialSlots.Num());
			return false;
		}
		SectionIndexSum += Section.IndexCount;
	}
	if (SectionIndexSum != IndexCount)
	{
		OutError = std::format("section index sum {} != indices {}", SectionIndexSum, IndexCount);
		return false;
	}

	if (!LODSourceVertices.IsEmpty() &&
		LODSourceVertices.Num() != Vertices.Num())
	{
		OutError = "LOD source vertex count does not match vertex count";
		return false;
	}

	return true;
}

void FStaticMeshData::OptimizeTriangleOrderForVertexCache()
{
	constexpr uint32 CacheSize = 32;
	const uint32 VertexCount = static_cast<uint32>(Vertices.Num());
	for (const FStaticMeshSection& Section : Sections)
	{
		if (Section.IndexCount < 6 || Section.StartIndex % 3 != 0 ||
			Section.IndexCount % 3 != 0 ||
			Section.StartIndex > static_cast<uint32>(Indices.Num()) ||
			Section.IndexCount > static_cast<uint32>(Indices.Num()) - Section.StartIndex)
			continue;

		const uint32 TriangleCount = Section.IndexCount / 3;
		const uint32* Source = Indices.GetData() + Section.StartIndex;
		std::vector<std::vector<uint32>> Adjacent(VertexCount);
		std::vector<uint32> Remaining(VertexCount, 0);
		for (uint32 T = 0; T < TriangleCount; ++T)
		{
			for (uint32 Corner = 0; Corner < 3; ++Corner)
			{
				const uint32 V = Source[T * 3 + Corner];
				if (V >= VertexCount) return;
				Adjacent[V].push_back(T);
				++Remaining[V];
			}
		}

		std::vector<uint8> Emitted(TriangleCount, 0);
		std::vector<uint32> Seen(TriangleCount, 0);
		std::vector<int32> CachePosition(VertexCount, -1);
		std::vector<uint32> Cache;
		Cache.reserve(CacheSize);
		std::vector<uint32> Reordered;
		Reordered.reserve(Section.IndexCount);
		uint32 NextTriangle = 0;
		uint32 Pass = 0;

		while (Reordered.size() < Section.IndexCount)
		{
			++Pass;
			uint32 Best = TriangleCount;
			float BestScore = -FLT_MAX;
			for (uint32 CachedVertex : Cache)
			{
				for (uint32 T : Adjacent[CachedVertex])
				{
					if (Emitted[T] || Seen[T] == Pass) continue;
					Seen[T] = Pass;
					float Score = 0.0f;
					for (uint32 Corner = 0; Corner < 3; ++Corner)
					{
						const uint32 V = Source[T * 3 + Corner];
						const int32 Position = CachePosition[V];
						if (Position >= 0)
						{
							Score += Position < 3 ? 0.75f :
								std::pow(static_cast<float>(CacheSize - Position) /
									static_cast<float>(CacheSize - 3), 1.5f);
						}
						Score += 2.0f / std::sqrt(static_cast<float>(Remaining[V]));
					}
					if (Score > BestScore)
					{
						BestScore = Score;
						Best = T;
					}
				}
			}

			if (Best == TriangleCount)
			{
				while (NextTriangle < TriangleCount && Emitted[NextTriangle]) ++NextTriangle;
				Best = NextTriangle;
			}
			for (uint32 CachedVertex : Cache) CachePosition[CachedVertex] = -1;
			Emitted[Best] = 1;
			for (uint32 Corner = 0; Corner < 3; ++Corner)
			{
				const uint32 V = Source[Best * 3 + Corner];
				Reordered.push_back(V);
				--Remaining[V];
				const auto It = std::find(Cache.begin(), Cache.end(), V);
				if (It != Cache.end()) Cache.erase(It);
				Cache.insert(Cache.begin(), V);
				if (Cache.size() > CacheSize) Cache.pop_back();
			}
			for (uint32 I = 0; I < Cache.size(); ++I)
				CachePosition[Cache[I]] = static_cast<int32>(I);
		}

		const uint32 Old16 = CountVertexCacheMisses(Source, Section.IndexCount, 16);
		const uint32 Old32 = CountVertexCacheMisses(Source, Section.IndexCount, 32);
		const uint32 New16 = CountVertexCacheMisses(Reordered.data(), Section.IndexCount, 16);
		const uint32 New32 = CountVertexCacheMisses(Reordered.data(), Section.IndexCount, 32);
		if (New16 <= Old16 && New32 <= Old32 && (New16 < Old16 || New32 < Old32))
		{
			for (uint32 I = 0; I < Section.IndexCount; ++I)
				Indices[Section.StartIndex + I] = Reordered[I];
		}
	}
}

void FStaticMeshData::CalculateTangents()
{
	const int32 VertexCount = Vertices.Num();

	if (VertexCount == 0)
		return;

	TArray<FVector> TangentSums;
	TangentSums.Init(FVector(0.0f), VertexCount);

	TArray<FVector> BitangentSums;
	BitangentSums.Init(FVector(0.0f), VertexCount);


	for (int32 i = 0; i + 2 < Indices.Num(); i += 3)
	{
		const uint32 I0 = Indices[i];
		const uint32 I1 = Indices[i + 1];
		const uint32 I2 = Indices[i + 2];

		//  Position / Normal / Color / Texcoord(UV)
		const FVertexPNCT& V0 = Vertices[I0];
		const FVertexPNCT& V1 = Vertices[I1];
		const FVertexPNCT& V2 = Vertices[I2];

		const FVector E1 = V1.Position - V0.Position;
		const FVector E2 = V2.Position - V0.Position;

		// UV 변화량
		const FVector2 DUv1 = V1.UV - V0.UV;
		const FVector2 DUv2 = V2.UV - V0.UV;

		const float Det = DUv1.X * DUv2.Y - DUv1.Y * DUv2.X;

		// UV가 일직선이거나 겹치면 T/B를 구할 수 없다.
		if (std::abs(Det) < 1e-8f)
			continue;

		// 면적이 거의 없는 삼각형도 제외
		if (E1.Cross(E2).SizeSquared() < 1e-12f)
			continue;

		const float InvDet = 1.0f / Det;

		const FVector T = (E1 * DUv2.Y - E2 * DUv1.Y) * InvDet;
		const FVector B = (E2 * DUv1.X - E1 * DUv2.X) * InvDet;


		// 정점을 많이 공유하는 메쉬를 고려하여 정점의 평균 방향을 만든다.
		// https://learnopengl.com/Advanced-Lighting/Normal-Mapping 의 One last thing 참고
		TangentSums[I0] += T;
		TangentSums[I1] += T;
		TangentSums[I2] += T;

		BitangentSums[I0] += B;
		BitangentSums[I1] += B;
		BitangentSums[I2] += B;
	}

	for (int32 i = 0; i < VertexCount; ++i)
	{
		const FVector N = Vertices[i].Normal.GetSafeNormal(1e-12f, FVector(0.0f, 0.0f, 1.0f));

		const FVector& SumT = TangentSums[i];

		// Tangent의 Normal 방향 성분 제거
		FVector T = SumT - N * N.Dot(SumT);

		// 유효한 UV가 없거나 누적값이 상쇄된 경우의 대체 축
		if (T.SizeSquared() < 1e-12f)
		{
			// 임시 축 지정
			const FVector Axis = std::abs(N.Z) < 0.9f ? FVector(0.0f, 0.0f, 1.0f) : FVector(0.0f, 1.0f, 0.0f);

			T = Axis.Cross(N);
		}

		T = T.GetSafeNormal(1e-12f);

		// 실제 UV의 B 방향과 cross(N, T)의 방향을 비교한다.
		const float Sign =N.Cross(T).Dot(BitangentSums[i]) < 0.0f ? -1.0f : 1.0f;

		Vertices[i].Tangent = FVector4(T, Sign);
	}

}

void FStaticMeshData::BuildTriangleBVH()
{
	TArray<FMeshTriangleElement> Elements;
	for (int32 i = 0; i + 2 < Indices.Num(); i += 3)
	{
		const FVector& V0 = Vertices[Indices[i]].Position;
		const FVector& V1 = Vertices[Indices[i + 1]].Position;
		const FVector& V2 = Vertices[Indices[i + 2]].Position;

		FMeshTriangleElement Element = {
			.V0 = V0,
			.Edge1 = V1 - V0,
			.Edge2 = V2 - V0,
		};

		Element.Bounds.Min.X = std::min({ V0.X, V1.X, V2.X });
		Element.Bounds.Min.Y = std::min({ V0.Y, V1.Y, V2.Y });
		Element.Bounds.Min.Z = std::min({ V0.Z, V1.Z, V2.Z });
		Element.Bounds.Max.X = std::max({ V0.X, V1.X, V2.X });
		Element.Bounds.Max.Y = std::max({ V0.Y, V1.Y, V2.Y });
		Element.Bounds.Max.Z = std::max({ V0.Z, V1.Z, V2.Z });
		Elements.Add(Element);
	}

	TriangleBVH.emplace([](const FMeshTriangleElement& Element) {
		return Element.Bounds;
	});

	TriangleBVH->Build(Elements);
}

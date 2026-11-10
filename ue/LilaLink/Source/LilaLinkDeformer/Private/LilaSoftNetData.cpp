#include "LilaSoftNetData.h"

#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if WITH_EDITOR
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshModel.h"
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(LilaSoftNetData)

namespace LilaSoftNetBind
{
FString SoftNetReport(const TSharedRef<FJsonObject>& Obj)
{
	FString Out;
	FJsonSerializer::Serialize(Obj, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out));
	return Out;
}

FString FailSoftNet(const FString& Error)
{
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetArrayField(TEXT("errors"), {MakeShared<FJsonValueString>(Error)});
	return SoftNetReport(Obj);
}

/** A point from Lila: where it is at bind and its influence entries. */
struct FNetPoint
{
	FVector Position;
	TArray<FVector2f> Entries;
};

/** Points hashed by position: the point at a vertex, or the nearest one at any distance. */
struct FNetGrid
{
	double Cell = 0.05;
	int32 MaxRing = 0;
	TMap<FIntVector, TArray<int32>> Cells;

	FIntVector KeyOf(const FVector& P) const { return FIntVector(FMath::FloorToInt(P.X / Cell), FMath::FloorToInt(P.Y / Cell), FMath::FloorToInt(P.Z / Cell)); }

	int32 Nearest(const TArray<FNetPoint>& Points, const FVector& P, const double Within, const int32 Rings) const
	{
		const FIntVector K = KeyOf(P);
		int32 Best = INDEX_NONE;
		double BestD = Within;
		for (int32 R = 0; R <= Rings; ++R)
		{
			if (Best != INDEX_NONE && double(R - 1) * Cell > BestD)
			{
				break;
			}
			for (int32 X = -R; X <= R; ++X)
			{
				for (int32 Y = -R; Y <= R; ++Y)
				{
					for (int32 Z = -R; Z <= R; ++Z)
					{
						if (FMath::Max3(FMath::Abs(X), FMath::Abs(Y), FMath::Abs(Z)) != R)
						{
							continue;
						}
						if (const TArray<int32>* Found = Cells.Find(K + FIntVector(X, Y, Z)))
						{
							for (const int32 I : *Found)
							{
								if (const double D = (Points[I].Position - P).Size(); D <= BestD)
								{
									BestD = D;
									Best = I;
								}
							}
						}
					}
				}
			}
		}
		return Best;
	}
};
} // namespace LilaSoftNetBind

FString ULilaSoftNetLibrary::BindSoftNet(USkeletalMesh* Mesh, const FString& Json)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaSoftNetLibrary::BindSoftNet);

#if WITH_EDITOR
	using namespace LilaSoftNetBind;
	if (!Mesh)
	{
		return FailSoftNet(TEXT("no skeletal mesh"));
	}
	TSharedPtr<FJsonObject> P;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), P) || !P.IsValid())
	{
		return FailSoftNet(TEXT("the payload isn't JSON"));
	}
	FSkeletalMeshModel* Model = Mesh->GetImportedModel();
	if (!Model || Model->LODModels.Num() == 0)
	{
		return FailSoftNet(TEXT("the mesh has no imported geometry"));
	}
	const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
	ULilaSoftNetData* Data = NewObject<ULilaSoftNetData>(Mesh);
	auto Need = [&Ref, Mesh](const FString& Bone, FString& OutError) {
		if (Ref.FindRawBoneIndex(FName(*Bone)) == INDEX_NONE)
		{
			OutError = FString::Printf(TEXT("%s has no bone named %s: export the skeleton with deformOnly off so the Soft Net joints go too"), *Mesh->GetName(), *Bone);
			return false;
		}
		return true;
	};
	FString Error;
	for (const TSharedPtr<FJsonValue>& V : P->GetArrayField(TEXT("knots")))
	{
		const TSharedPtr<FJsonObject> K = V->AsObject();
		FLilaSoftNetKnot& Knot = Data->Knots.AddDefaulted_GetRef();
		if (!Need(K->GetStringField(TEXT("frame")), Error) || !Need(K->GetStringField(TEXT("control")), Error))
		{
			return FailSoftNet(Error);
		}
		Knot.Frame = FName(*K->GetStringField(TEXT("frame")));
		Knot.Control = FName(*K->GetStringField(TEXT("control")));
	}
	for (const TSharedPtr<FJsonValue>& V : P->GetArrayField(TEXT("tangents")))
	{
		const TSharedPtr<FJsonObject> T = V->AsObject();
		FLilaSoftNetTangent& Tangent = Data->Tangents.AddDefaulted_GetRef();
		if (!Need(T->GetStringField(TEXT("bone")), Error) || !Need(T->GetStringField(TEXT("rest")), Error))
		{
			return FailSoftNet(Error);
		}
		Tangent.Bone = FName(*T->GetStringField(TEXT("bone")));
		Tangent.Rest = FName(*T->GetStringField(TEXT("rest")));
	}
	const uint32 Dofs = uint32(Data->Knots.Num() + Data->Tangents.Num());

	TArray<FNetPoint> Points;
	FNetGrid Exact;
	FBox Bounds(ForceInit);
	for (const TSharedPtr<FJsonValue>& V : P->GetArrayField(TEXT("points")))
	{
		const TSharedPtr<FJsonObject> Obj = V->AsObject();
		const TArray<TSharedPtr<FJsonValue>>& Pos = Obj->GetArrayField(TEXT("p"));
		FNetPoint& Pt = Points.AddDefaulted_GetRef();
		Pt.Position = Pos.Num() == 3 ? FVector(Pos[0]->AsNumber(), Pos[1]->AsNumber(), Pos[2]->AsNumber()) : FVector::ZeroVector;
		for (const TSharedPtr<FJsonValue>& E : Obj->GetArrayField(TEXT("i")))
		{
			const TArray<TSharedPtr<FJsonValue>>& Pair = E->AsArray();
			if (Pair.Num() == 2 && uint32(Pair[0]->AsNumber()) < Dofs)
			{
				Pt.Entries.Add(FVector2f(FMath::AsFloat(uint32(Pair[0]->AsNumber())), float(Pair[1]->AsNumber())));
			}
		}
		Exact.Cells.FindOrAdd(Exact.KeyOf(Pt.Position)).Add(Points.Num() - 1);
		Bounds += Pt.Position;
	}
	// LODs 1 and up have vertices Lila may not: they take the nearest point's entries, found on a coarser grid.
	FNetGrid Coarse;
	Coarse.Cell = FMath::Max(Bounds.IsValid ? Bounds.GetSize().GetMax() / 64.0 : 1.0, 0.1);
	for (int32 I = 0; I < Points.Num(); ++I)
	{
		Coarse.Cells.FindOrAdd(Coarse.KeyOf(Points[I].Position)).Add(I);
	}

	TArray<TSharedPtr<FJsonValue>> Warnings, LodReports;
	int32 Unmatched0 = 0, Vertices0 = 0;
	for (int32 L = 0; L < Model->LODModels.Num(); ++L)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(LilaSoftNetLibrary::BindLod);

		TArray<FSoftSkinVertex> Vertices;
		Model->LODModels[L].GetVertices(Vertices);
		FLilaSoftNetLod& Out = Data->Lods.AddDefaulted_GetRef();
		Out.VertexCount = Vertices.Num();
		Out.Offsets.Reserve(Vertices.Num() + 1);
		Out.Offsets.Add(0);
		int32 Unmatched = 0;
		for (const FSoftSkinVertex& V : Vertices)
		{
			const FVector Pos(V.Position);
			int32 Match = Exact.Nearest(Points, Pos, 0.05, 1);
			if (Match == INDEX_NONE)
			{
				++Unmatched;
				Match = Coarse.Nearest(Points, Pos, TNumericLimits<double>::Max(), 64);
			}
			if (Match != INDEX_NONE)
			{
				Out.Entries.Append(Points[Match].Entries);
			}
			Out.Offsets.Add(uint32(Out.Entries.Num()));
		}
		TSharedRef<FJsonObject> Lod = MakeShared<FJsonObject>();
		Lod->SetNumberField(TEXT("vertices"), Vertices.Num());
		Lod->SetNumberField(TEXT("matched"), Vertices.Num() - Unmatched);
		Lod->SetNumberField(TEXT("entries"), Out.Entries.Num());
		LodReports.Add(MakeShared<FJsonValueObject>(Lod));
		if (L == 0)
		{
			Unmatched0 = Unmatched;
			Vertices0 = Vertices.Num();
		}
	}
	if (Unmatched0 > 0)
	{
		Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
			TEXT("%d of %d vertices have no Lila point at their position, so they take the nearest point's influence"), Unmatched0, Vertices0)));
	}
	if (ULilaSoftNetData* Old = Mesh->GetAssetUserData<ULilaSoftNetData>())
	{
		Data->Revision = Old->Revision + 1;
		Mesh->RemoveUserDataOfClass(ULilaSoftNetData::StaticClass());
	}
	Mesh->AddAssetUserData(Data);
	Mesh->MarkPackageDirty();

	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetNumberField(TEXT("vertices"), Vertices0);
	Obj->SetNumberField(TEXT("matched"), Vertices0 - Unmatched0);
	Obj->SetNumberField(TEXT("knots"), Data->Knots.Num());
	Obj->SetNumberField(TEXT("tangents"), Data->Tangents.Num());
	Obj->SetArrayField(TEXT("lods"), LodReports);
	Obj->SetArrayField(TEXT("warnings"), Warnings);
	Obj->SetArrayField(TEXT("errors"), {});
	return SoftNetReport(Obj);
#else
	return LilaSoftNetBind::FailSoftNet(TEXT("Soft Nets bind in the editor"));
#endif
}

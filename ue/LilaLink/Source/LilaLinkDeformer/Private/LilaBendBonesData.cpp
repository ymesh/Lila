#include "LilaBendBonesData.h"

#include "LilaBendBonesMath.h"

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

#include UE_INLINE_GENERATED_CPP_BY_NAME(LilaBendBonesData)

namespace
{
FString Report(const TSharedRef<FJsonObject>& Obj)
{
	FString Out;
	FJsonSerializer::Serialize(Obj, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out));
	return Out;
}

FString Fail(const FString& Error)
{
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetArrayField(TEXT("errors"), {MakeShared<FJsonValueString>(Error)});
	return Report(Obj);
}

FVector ReadVector(const TArray<TSharedPtr<FJsonValue>>& V) { return V.Num() == 3 ? FVector(V[0]->AsNumber(), V[1]->AsNumber(), V[2]->AsNumber()) : FVector::ZeroVector; }

/** A point from Lila: where it is and its chains' weights. */
struct FPoint
{
	FVector Position;
	TArray<TPair<int32, double>, TInlineAllocator<2>> Weights;
};

/** Points hashed by position, for finding the Lila point at a vertex. */
struct FPointGrid
{
	double Cell = 0.05;
	TMap<FIntVector, TArray<int32>> Cells;

	FIntVector KeyOf(const FVector& P) const { return FIntVector(FMath::FloorToInt(P.X / Cell), FMath::FloorToInt(P.Y / Cell), FMath::FloorToInt(P.Z / Cell)); }

	int32 Nearest(const TArray<FPoint>& Points, const FVector& P, const double Tolerance) const
	{
		const FIntVector K = KeyOf(P);
		int32 Best = INDEX_NONE;
		double BestD = Tolerance;
		for (int32 X = -1; X <= 1; ++X)
		{
			for (int32 Y = -1; Y <= 1; ++Y)
			{
				for (int32 Z = -1; Z <= 1; ++Z)
				{
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
		return Best;
	}

	/** The nearest point at any distance, searching rings of cells outward; INDEX_NONE when there are none. */
	int32 NearestAny(const TArray<FPoint>& Points, const FVector& P) const
	{
		const FIntVector K = KeyOf(P);
		int32 Best = INDEX_NONE;
		double BestD = TNumericLimits<double>::Max();
		for (int32 R = 0; R <= MaxRing; ++R)
		{
			// Every point in ring R is at least (R - 1) cells away.
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
								if (const double D = (Points[I].Position - P).Size(); D < BestD)
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

	int32 MaxRing = 0;
};
} // namespace

FString ULilaBendBonesLibrary::BindBendBones(USkeletalMesh* Mesh, const FString& Json)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaBendBonesLibrary::BindBendBones);

#if WITH_EDITOR
	using namespace LilaBendBones;
	if (!Mesh)
	{
		return Fail(TEXT("no skeletal mesh"));
	}
	TSharedPtr<FJsonObject> P;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), P) || !P.IsValid())
	{
		return Fail(TEXT("the payload isn't JSON"));
	}
	FSkeletalMeshModel* Model = Mesh->GetImportedModel();
	if (!Model || Model->LODModels.Num() == 0)
	{
		return Fail(TEXT("the mesh has no imported geometry"));
	}
	TArray<TSharedPtr<FJsonValue>> Warnings;

	// The reference pose in component space.
	const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
	TArray<FTransform> RefGlobal;
	RefGlobal.SetNum(Ref.GetRawBoneNum());
	for (int32 B = 0; B < Ref.GetRawBoneNum(); ++B)
	{
		const int32 Parent = Ref.GetRawParentIndex(B);
		RefGlobal[B] = Parent == INDEX_NONE ? Ref.GetRawRefBonePose()[B] : Ref.GetRawRefBonePose()[B] * RefGlobal[Parent];
	}

	const int32 Samples = FMath::Clamp(int32(P->GetNumberField(TEXT("samplesPerSpan"))), 2, 64);
	ULilaBendBonesData* Data = NewObject<ULilaBendBonesData>(Mesh);
	Data->SamplesPerSpan = Samples;
	Data->Volume = float(P->GetNumberField(TEXT("volume")));
	Data->EvenBend = float(P->GetNumberField(TEXT("evenBend")));

	TArray<FCurve> Rest;
	double RestError = 0.0;
	const TArray<TSharedPtr<FJsonValue>>& Chains = P->GetArrayField(TEXT("chains"));
	const TArray<TSharedPtr<FJsonValue>>* CvLists = nullptr;
	P->TryGetArrayField(TEXT("cvs"), CvLists);
	const TArray<TSharedPtr<FJsonValue>>* RestJoints = nullptr;
	P->TryGetArrayField(TEXT("restJoints"), RestJoints);
	for (int32 C = 0; C < Chains.Num(); ++C)
	{
		FLilaBendBonesChain& Out = Data->Chains.AddDefaulted_GetRef();
		FChain Chain;
		const TArray<TSharedPtr<FJsonValue>>& Bones = Chains[C]->AsArray();
		for (int32 J = 0; J < Bones.Num(); ++J)
		{
			const FName Name(*Bones[J]->AsString());
			const int32 Index = Ref.FindRawBoneIndex(Name);
			if (Index == INDEX_NONE)
			{
				return Fail(FString::Printf(TEXT("%s has no bone named %s: export the skeleton with deformOnly off so the bend bones chains go too"), *Mesh->GetName(), *Name.ToString()));
			}
			Out.Bones.Add(Name);
			Chain.Position.Add(RefGlobal[Index].GetLocation());
			Chain.Rotation.Add(RefGlobal[Index].GetRotation());
			if (RestJoints && RestJoints->IsValidIndex(C) && (*RestJoints)[C]->AsArray().IsValidIndex(J))
			{
				RestError = FMath::Max(RestError, (ReadVector((*RestJoints)[C]->AsArray()[J]->AsArray()) - Chain.Position.Last()).Size());
			}
		}
		if (Chain.Position.Num() < 2)
		{
			return Fail(FString::Printf(TEXT("chain %d has fewer than two bones"), C));
		}
		// Bone CVs: kept in their joint's reference pose space, posed from the bone Lila's CV exported as.
		TArray<TPair<int32, FVector>> RestCvs;
		if (CvLists && CvLists->IsValidIndex(C))
		{
			for (const TSharedPtr<FJsonValue>& V : (*CvLists)[C]->AsArray())
			{
				const TSharedPtr<FJsonObject> Cv = V->AsObject();
				const int32 Bone = int32(Cv->GetNumberField(TEXT("bone")));
				if (Bone < 0 || Bone + 1 >= Out.Bones.Num())
				{
					continue;
				}
				const FVector RestPos = ReadVector(Cv->GetArrayField(TEXT("rest")));
				FLilaBendBonesCv& Made = Out.Cvs.AddDefaulted_GetRef();
				Made.Bone = Bone;
				Made.RestLocal = RefGlobal[Ref.FindRawBoneIndex(Out.Bones[Bone])].InverseTransformPosition(RestPos);
				const FName Driver(*Cv->GetStringField(TEXT("name")));
				if (Ref.FindRawBoneIndex(Driver) != INDEX_NONE)
				{
					Made.Driver = Driver;
				}
				else
				{
					Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
						TEXT("bone CV %s isn't in the skeleton, so it rides %s as at rest: export with deformOnly off so it goes too"), *Driver.ToString(), *Out.Bones[Bone].ToString())));
				}
				RestCvs.Emplace(Bone, RestPos);
			}
		}
		PlaceCvs(Chain, RestCvs);
		// Lila's own rest normals when it sent them (the twist depends on which one each joint has), else chosen here.
		const TArray<TSharedPtr<FJsonValue>>* Normals = nullptr;
		if (P->TryGetArrayField(TEXT("restNormals"), Normals) && Normals->IsValidIndex(C) && (*Normals)[C]->AsArray().Num() == Chain.Position.Num())
		{
			for (int32 J = 0; J < Chain.Position.Num(); ++J)
			{
				Out.RestNormals.Add(Chain.Rotation[J].UnrotateVector(ReadVector((*Normals)[C]->AsArray()[J]->AsArray()).GetSafeNormal()));
			}
		}
		else
		{
			Out.RestNormals = RestNormals(Chain, Build(Chain, {}, Samples));
		}
		FCurve Curve = Build(Chain, Out.RestNormals, Samples);
		for (const int32 K : Curve.Knot)
		{
			Out.RestKnotLength.Add(Curve.Length[K]);
		}
		Rest.Add(MoveTemp(Curve));
	}
	if (RestError > 0.1)
	{
		Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
			TEXT("the reference pose sits %.2f cm from Lila's rest at the chain bones: export the mesh in its bind pose, or bends will be off by as much"), RestError)));
	}

	TArray<FPoint> Points;
	FPointGrid Grid;
	for (const TSharedPtr<FJsonValue>& V : P->GetArrayField(TEXT("points")))
	{
		const TSharedPtr<FJsonObject> Obj = V->AsObject();
		FPoint& Pt = Points.AddDefaulted_GetRef();
		Pt.Position = ReadVector(Obj->GetArrayField(TEXT("p")));
		for (const TSharedPtr<FJsonValue>& W : Obj->GetArrayField(TEXT("w")))
		{
			const TArray<TSharedPtr<FJsonValue>>& Pair = W->AsArray();
			if (Pair.Num() == 2 && Rest.IsValidIndex(int32(Pair[0]->AsNumber())) && Pt.Weights.Num() < 2)
			{
				Pt.Weights.Emplace(int32(Pair[0]->AsNumber()), Pair[1]->AsNumber());
			}
		}
		Grid.Cells.FindOrAdd(Grid.KeyOf(Pt.Position)).Add(Points.Num() - 1);
	}
	const double Blend = P->HasField(TEXT("blend")) ? P->GetNumberField(TEXT("blend")) : 0.25;

	// LODs 1 and up have vertices Lila may not: they take the nearest point's weights, found on a coarser grid.
	FBox Bounds(ForceInit);
	for (const FPoint& Pt : Points)
	{
		Bounds += Pt.Position;
	}
	FPointGrid Coarse;
	Coarse.Cell = FMath::Max(Bounds.IsValid ? Bounds.GetSize().GetMax() / 64.0 : 1.0, 0.1);
	Coarse.MaxRing = 64;
	for (int32 I = 0; I < Points.Num(); ++I)
	{
		Coarse.Cells.FindOrAdd(Coarse.KeyOf(Points[I].Position)).Add(I);
	}

	// Binds one LOD's vertices; returns how many had no Lila point at their position.
	auto BindLod = [&](const TArray<FSoftSkinVertex>& Vertices, const bool bNearestPoint, FLilaBendBonesLod& Out) {
		TRACE_CPUPROFILER_EVENT_SCOPE(LilaBendBonesLibrary::BindLod);

		int32 Unmatched = 0;
		Out.VertexCount = Vertices.Num();
		Out.Entries.Reset(Vertices.Num() * 6);
		for (const FSoftSkinVertex& V : Vertices)
		{
			const FVector Pos(V.Position);
			TArray<TPair<int32, double>, TInlineAllocator<2>> Weights;
			TArray<FPlace, TInlineAllocator<2>> Places;
			int32 Match = Grid.Nearest(Points, Pos, 0.05);
			if (Match == INDEX_NONE)
			{
				++Unmatched;
				if (bNearestPoint)
				{
					Match = Coarse.NearestAny(Points, Pos);
				}
			}
			if (Match != INDEX_NONE && Points[Match].Weights.Num() > 0)
			{
				Weights = Points[Match].Weights;
				for (const TPair<int32, double>& W : Weights)
				{
					Places.Add(Locate(Rest[W.Key], Pos));
				}
			}
			else
			{
				// No Lila point here: the nearest chains, blended as Lila binds them.
				TArray<FPlace> All;
				double Nearest = TNumericLimits<double>::Max();
				for (const FCurve& C : Rest)
				{
					All.Add(Locate(C, Pos));
					Nearest = FMath::Min(Nearest, All.Last().Distance);
				}
				const double Band = FMath::Max(Blend * Nearest, 1e-6);
				TArray<TPair<double, int32>> Ranked;
				for (int32 C = 0; C < All.Num(); ++C)
				{
					if (const double Over = All[C].Distance - Nearest; Over < Band)
					{
						const double X = 1.0 - Over / Band;
						Ranked.Emplace(X * X, C);
					}
				}
				Ranked.Sort([](const auto& A, const auto& B) { return A.Key > B.Key; });
				for (int32 K = 0; K < FMath::Min(2, Ranked.Num()); ++K)
				{
					Weights.Emplace(Ranked[K].Value, Ranked[K].Key);
					Places.Add(All[Ranked[K].Value]);
				}
			}
			double Total = 0.0;
			for (const TPair<int32, double>& W : Weights)
			{
				Total += W.Value;
			}
			for (int32 E = 0; E < 2; ++E)
			{
				if (E < Weights.Num() && Total > 1e-12)
				{
					const FPlace& Pl = Places[E];
					Out.Entries.Add(FVector4f(FMath::AsFloat(uint32(Weights[E].Key)), FMath::AsFloat(uint32(Pl.Segment)), float(Weights[E].Value / Total), float(Pl.Fraction)));
					Out.Entries.Add(FVector4f(FVector3f(Pl.Offset), float(Pl.Strand)));
					Out.Entries.Add(FVector4f(float(Pl.Frame.X), float(Pl.Frame.Y), float(Pl.Frame.Z), float(Pl.Frame.W)));
				}
				else
				{
					Out.Entries.Add(FVector4f(FMath::AsFloat(~0u), 0.0f, 0.0f, 0.0f));
					Out.Entries.Add(FVector4f(0.0f, 0.0f, 0.0f, 0.0f));
					Out.Entries.Add(FVector4f(0.0f, 0.0f, 0.0f, 1.0f));
				}
			}
		}
		return Unmatched;
	};

	TArray<TSharedPtr<FJsonValue>> LodReports;
	int32 Unmatched = 0;
	int32 Vertices0 = 0;
	for (int32 L = 0; L < Model->LODModels.Num(); ++L)
	{
		TArray<FSoftSkinVertex> Vertices;
		Model->LODModels[L].GetVertices(Vertices);
		const int32 Missed = BindLod(Vertices, L > 0, Data->Lods.AddDefaulted_GetRef());
		TSharedRef<FJsonObject> Lod = MakeShared<FJsonObject>();
		Lod->SetNumberField(TEXT("vertices"), Vertices.Num());
		Lod->SetNumberField(TEXT("matched"), Vertices.Num() - Missed);
		LodReports.Add(MakeShared<FJsonValueObject>(Lod));
		if (L == 0)
		{
			Unmatched = Missed;
			Vertices0 = Vertices.Num();
		}
	}
	if (Unmatched > 0)
	{
		Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
			TEXT("%d of %d vertices have no Lila point at their position, so they take the nearest chains' weights rather than Lila's"), Unmatched, Vertices0)));
	}

	if (ULilaBendBonesData* Old = Mesh->GetAssetUserData<ULilaBendBonesData>())
	{
		Data->Revision = Old->Revision + 1;
		Mesh->RemoveUserDataOfClass(ULilaBendBonesData::StaticClass());
	}
	Mesh->AddAssetUserData(Data);
	Mesh->MarkPackageDirty();

	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetNumberField(TEXT("vertices"), Vertices0);
	Obj->SetNumberField(TEXT("matched"), Vertices0 - Unmatched);
	Obj->SetNumberField(TEXT("unmatched"), Unmatched);
	Obj->SetNumberField(TEXT("chains"), Data->Chains.Num());
	Obj->SetArrayField(TEXT("lods"), LodReports);
	Obj->SetNumberField(TEXT("restError"), RestError);
	Obj->SetArrayField(TEXT("warnings"), Warnings);
	Obj->SetArrayField(TEXT("errors"), {});
	return Report(Obj);
#else
	return Fail(TEXT("bend bones bind in the editor"));
#endif
}

#pragma once

#include "CoreMinimal.h"
#include "Engine/AssetUserData.h"
#include "Kismet/BlueprintFunctionLibrary.h"

#include "LilaSoftNetData.generated.h"

class USkeletalMesh;

/** A Soft Net knot: the bone riding its frame on the surface and the control bone under it. */
USTRUCT()
struct FLilaSoftNetKnot
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, Category = "Soft Net")
	FName Frame;

	UPROPERTY(VisibleAnywhere, Category = "Soft Net")
	FName Control;
};

/** A Soft Net tangent handle: its bone under the knot's control, and the bone where it sits at rest under the frame. */
USTRUCT()
struct FLilaSoftNetTangent
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, Category = "Soft Net")
	FName Bone;

	UPROPERTY(VisibleAnywhere, Category = "Soft Net")
	FName Rest;
};

/** One LOD's binding: per vertex, the net's degrees of freedom that move it and by how much. */
USTRUCT()
struct FLilaSoftNetLod
{
	GENERATED_BODY()

	/** Vertices bound; the deformer runs on the LOD only while it has this many. */
	UPROPERTY(VisibleAnywhere, Category = "Soft Net")
	int32 VertexCount = 0;

	/** Per vertex, where its entries start; one more than VertexCount. */
	UPROPERTY()
	TArray<uint32> Offsets;

	/** (degree of freedom as uint bits, weight). */
	UPROPERTY()
	TArray<FVector2f> Entries;
};

/**
 * Lila Soft Net on a skeletal mesh: its knot and handle bones and each LOD's vertices' influence, made by LilaLink's
 * unrealSoftNet and read by the Lila Soft Net data interface in the mesh's Deformer Graph.
 */
UCLASS(MinimalAPI)
class ULilaSoftNetData : public UAssetUserData
{
	GENERATED_BODY()

public:
	UPROPERTY(VisibleAnywhere, Category = "Soft Net")
	TArray<FLilaSoftNetKnot> Knots;

	UPROPERTY(VisibleAnywhere, Category = "Soft Net")
	TArray<FLilaSoftNetTangent> Tangents;

	/** A binding per LOD, LOD 0 first. */
	UPROPERTY(VisibleAnywhere, Category = "Soft Net")
	TArray<FLilaSoftNetLod> Lods;

	/** Bumped whenever the binding changes, so deformers upload it again. */
	UPROPERTY()
	int32 Revision = 0;
};

UCLASS()
class LILALINKDEFORMER_API ULilaSoftNetLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Binds every LOD of the mesh to a Soft Net from Lila: Json is unrealSoftNet's payload (knots {frame, control},
	 * tangents {bone, rest}, dofs, points {p, i}). Each vertex takes the entries of the Lila point at its place (LODs 1 and
	 * up the nearest point's). Returns a JSON report: vertices, matched, lods, warnings, errors. Editor only.
	 */
	UFUNCTION(BlueprintCallable, Category = "Lila|Soft Net")
	static FString BindSoftNet(USkeletalMesh* Mesh, const FString& Json);

	/**
	 * The Deformer Graph that runs Lila Soft Net, made at AssetPath (/Game/...) when missing: linear skinning, then each
	 * vertex moved by the net's offset. Returns the graph, or null with OutError saying why. Editor only.
	 */
	UFUNCTION(BlueprintCallable, Category = "Lila|Soft Net")
	static UObject* MakeSoftNetDeformer(const FString& AssetPath, FString& OutError);
};

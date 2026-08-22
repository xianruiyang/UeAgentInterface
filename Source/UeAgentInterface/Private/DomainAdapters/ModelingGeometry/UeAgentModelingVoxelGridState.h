#pragma once

#include "Components/ActorComponent.h"
#include "CoreMinimal.h"

#include "UeAgentModelingVoxelGridState.generated.h"

class ADynamicMeshActor;

UCLASS(NotBlueprintable, ClassGroup = (UAI))
class UEAGENTINTERFACE_API UUeAgentModelingVoxelGridStateComponent final
	: public UActorComponent
{
	GENERATED_BODY()

public:
	UUeAgentModelingVoxelGridStateComponent();

	virtual bool IsEditorOnly() const override { return true; }

	const FString& GetSchema() const { return Schema; }
	const FString& GetGridId() const { return GridId; }
	double GetCellSize() const { return CellSize; }
	const FTransform& GetGridToWorld() const { return GridToWorld; }
	const FVector& GetAnchorLocal() const { return AnchorLocal; }
	int64 GetRevision() const { return Revision; }
	const TArray<FIntVector>& GetCells() const { return Cells; }
	const FString& GetCollisionGeneration() const
	{
		return CollisionGeneration;
	}
	const FString& GetCollisionMode() const { return CollisionMode; }
	const FString& GetMaterialRef() const { return MaterialRef; }

	void SetAuthoringState(
		FString InGridId,
		double InCellSize,
		FTransform InGridToWorld,
		FVector InAnchorLocal,
		int64 InRevision,
		TArray<FIntVector> InCells,
		FString InCollisionGeneration,
		FString InCollisionMode,
		FString InMaterialRef);

private:
	UPROPERTY()
	FString Schema;

	UPROPERTY()
	FString GridId;

	UPROPERTY()
	double CellSize = 100.0;

	UPROPERTY()
	FTransform GridToWorld = FTransform::Identity;

	UPROPERTY()
	FVector AnchorLocal = FVector::ZeroVector;

	UPROPERTY()
	int64 Revision = 0;

	UPROPERTY()
	TArray<FIntVector> Cells;

	UPROPERTY()
	FString CollisionGeneration;

	UPROPERTY()
	FString CollisionMode = TEXT("none");

	UPROPERTY()
	FString MaterialRef;
};

namespace UE::UAI::DomainAdapters::ModelingGeometry::VoxelGrid
{
	struct FVoxelGridAuthoringState final
	{
		FString GridId;
		double CellSize = 100.0;
		FTransform GridToWorld = FTransform::Identity;
		FVector AnchorLocal = FVector::ZeroVector;
		TSet<FIntVector> Cells;
		int64 Revision = 0;
		FString CollisionGeneration;
		FString CollisionMode = TEXT("none");
		FString MaterialRef;
	};

	UEAGENTINTERFACE_API const FString& GetAuthoringStateSchema();
	UEAGENTINTERFACE_API FName GetAuthoringStateComponentName();

	UEAGENTINTERFACE_API bool TryFindAuthoringStateComponent(
		const ADynamicMeshActor& Actor,
		UUeAgentModelingVoxelGridStateComponent*& OutComponent,
		FString& OutError);

	UEAGENTINTERFACE_API bool TryCreateAuthoringStateComponent(
		ADynamicMeshActor& Actor,
		UUeAgentModelingVoxelGridStateComponent*& OutComponent,
		FString& OutError);

	UEAGENTINTERFACE_API bool TryReadAuthoringState(
		const UUeAgentModelingVoxelGridStateComponent& Component,
		int64 MaxCells,
		FVoxelGridAuthoringState& OutState,
		FString& OutError);

	UEAGENTINTERFACE_API bool TryWriteAuthoringState(
		UUeAgentModelingVoxelGridStateComponent& Component,
		const FVoxelGridAuthoringState& State,
		int64 MaxCells,
		FString& OutError);
}

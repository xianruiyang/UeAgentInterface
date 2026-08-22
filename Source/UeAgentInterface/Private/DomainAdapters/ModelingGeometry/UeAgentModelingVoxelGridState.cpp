#include "DomainAdapters/ModelingGeometry/UeAgentModelingVoxelGridState.h"

#include "DynamicMeshActor.h"
#include "GameFramework/Actor.h"

namespace
{
	const FString SchemaId(TEXT("uai.modeling.voxel_grid.authoring_state.v1"));
	const FName ComponentName(TEXT("UAI_VoxelGridAuthoringState"));

	bool IsFiniteVector(const FVector& Value)
	{
		return FMath::IsFinite(Value.X)
			&& FMath::IsFinite(Value.Y)
			&& FMath::IsFinite(Value.Z);
	}

	bool ValidateState(
		const UE::UAI::DomainAdapters::ModelingGeometry::VoxelGrid::
			FVoxelGridAuthoringState& State,
		const int64 MaxCells,
		FString& OutError)
	{
		OutError.Reset();
		if (State.GridId.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("voxel_grid_authoring_state_grid_id_missing");
			return false;
		}
		if (!FMath::IsFinite(State.CellSize)
			|| State.CellSize <= KINDA_SMALL_NUMBER
			|| State.CellSize > 1000000.0)
		{
			OutError = TEXT("voxel_grid_authoring_state_cell_size_invalid");
			return false;
		}
		if (!State.GridToWorld.IsValid()
			|| !IsFiniteVector(State.GridToWorld.GetLocation())
			|| !IsFiniteVector(State.AnchorLocal)
			|| State.Revision < 0
			|| MaxCells < 0
			|| State.Cells.Num() > MaxCells)
		{
			OutError = TEXT("voxel_grid_authoring_state_contract_invalid");
			return false;
		}
		return true;
	}

	TArray<FIntVector> SortedCells(const TSet<FIntVector>& Cells)
	{
		TArray<FIntVector> Result = Cells.Array();
		Result.Sort([](const FIntVector& Left, const FIntVector& Right)
		{
			if (Left.Z != Right.Z) return Left.Z < Right.Z;
			if (Left.Y != Right.Y) return Left.Y < Right.Y;
			return Left.X < Right.X;
		});
		return Result;
	}
}

UUeAgentModelingVoxelGridStateComponent::
	UUeAgentModelingVoxelGridStateComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	bAutoActivate = false;
	Schema = SchemaId;
}

void UUeAgentModelingVoxelGridStateComponent::SetAuthoringState(
	FString InGridId,
	const double InCellSize,
	FTransform InGridToWorld,
	FVector InAnchorLocal,
	const int64 InRevision,
	TArray<FIntVector> InCells,
	FString InCollisionGeneration,
	FString InCollisionMode,
	FString InMaterialRef)
{
	Modify();
	Schema = SchemaId;
	GridId = MoveTemp(InGridId);
	CellSize = InCellSize;
	GridToWorld = MoveTemp(InGridToWorld);
	AnchorLocal = MoveTemp(InAnchorLocal);
	Revision = InRevision;
	Cells = MoveTemp(InCells);
	CollisionGeneration = MoveTemp(InCollisionGeneration);
	CollisionMode = MoveTemp(InCollisionMode);
	MaterialRef = MoveTemp(InMaterialRef);
}

namespace UE::UAI::DomainAdapters::ModelingGeometry::VoxelGrid
{
	const FString& GetAuthoringStateSchema()
	{
		return SchemaId;
	}

	FName GetAuthoringStateComponentName()
	{
		return ComponentName;
	}

	bool TryFindAuthoringStateComponent(
		const ADynamicMeshActor& Actor,
		UUeAgentModelingVoxelGridStateComponent*& OutComponent,
		FString& OutError)
	{
		OutComponent = nullptr;
		OutError.Reset();
		TArray<UUeAgentModelingVoxelGridStateComponent*> Matches;
		Actor.GetComponents<UUeAgentModelingVoxelGridStateComponent>(Matches);
		for (UUeAgentModelingVoxelGridStateComponent* Component : Matches)
		{
			if (Component && Component->GetFName() == ComponentName)
			{
				if (OutComponent)
				{
					OutComponent = nullptr;
					OutError = TEXT("voxel_grid_authoring_state_duplicate");
					return false;
				}
				OutComponent = Component;
			}
		}
		if (!OutComponent)
		{
			OutError = TEXT("voxel_grid_authoring_state_missing");
			return false;
		}
		return true;
	}

	bool TryCreateAuthoringStateComponent(
		ADynamicMeshActor& Actor,
		UUeAgentModelingVoxelGridStateComponent*& OutComponent,
		FString& OutError)
	{
		OutComponent = nullptr;
		OutError.Reset();
		UUeAgentModelingVoxelGridStateComponent* Existing = nullptr;
		FString ExistingError;
		if (TryFindAuthoringStateComponent(Actor, Existing, ExistingError))
		{
			OutComponent = Existing;
			return true;
		}
		if (ExistingError != TEXT("voxel_grid_authoring_state_missing"))
		{
			OutError = MoveTemp(ExistingError);
			return false;
		}
		Actor.Modify();
		OutComponent = NewObject<UUeAgentModelingVoxelGridStateComponent>(
			&Actor,
			UUeAgentModelingVoxelGridStateComponent::StaticClass(),
			ComponentName,
			RF_Transactional);
		if (!OutComponent)
		{
			OutError = TEXT("voxel_grid_authoring_state_create_failed");
			return false;
		}
		OutComponent->CreationMethod = EComponentCreationMethod::Instance;
		Actor.AddInstanceComponent(OutComponent);
		OutComponent->OnComponentCreated();
		OutComponent->RegisterComponent();
		return true;
	}

	bool TryReadAuthoringState(
		const UUeAgentModelingVoxelGridStateComponent& Component,
		const int64 MaxCells,
		FVoxelGridAuthoringState& OutState,
		FString& OutError)
	{
		OutState = {};
		OutError.Reset();
		if (Component.GetSchema() != SchemaId)
		{
			OutError = TEXT("voxel_grid_authoring_state_schema_unsupported");
			return false;
		}
		OutState.GridId = Component.GetGridId();
		OutState.CellSize = Component.GetCellSize();
		OutState.GridToWorld = Component.GetGridToWorld();
		OutState.AnchorLocal = Component.GetAnchorLocal();
		OutState.Revision = Component.GetRevision();
		OutState.CollisionGeneration = Component.GetCollisionGeneration();
		OutState.CollisionMode = Component.GetCollisionMode();
		OutState.MaterialRef = Component.GetMaterialRef();
		if (Component.GetCells().Num() > MaxCells)
		{
			OutError = TEXT("voxel_grid_authoring_state_cell_limit_exceeded");
			return false;
		}
		for (const FIntVector& Cell : Component.GetCells())
		{
			if (OutState.Cells.Contains(Cell))
			{
				OutError = TEXT("voxel_grid_authoring_state_cell_duplicate");
				return false;
			}
			OutState.Cells.Add(Cell);
		}
		return ValidateState(OutState, MaxCells, OutError);
	}

	bool TryWriteAuthoringState(
		UUeAgentModelingVoxelGridStateComponent& Component,
		const FVoxelGridAuthoringState& State,
		const int64 MaxCells,
		FString& OutError)
	{
		if (!ValidateState(State, MaxCells, OutError))
		{
			return false;
		}
		Component.SetAuthoringState(
			State.GridId,
			State.CellSize,
			State.GridToWorld,
			State.AnchorLocal,
			State.Revision,
			SortedCells(State.Cells),
			State.CollisionGeneration,
			State.CollisionMode,
			State.MaterialRef);
		return true;
	}
}

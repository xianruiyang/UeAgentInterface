#include "DomainAdapters/ModelingGeometry/UeAgentModelingVoxelGridState.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "DomainAdapters/ModelingGeometry/UeAgentModelingMechanics.h"
#include "Dom/JsonValue.h"

#include "DynamicMeshActor.h"
#include "GameFramework/Actor.h"

namespace
{
	using namespace UeAgentModelingOps;
	const FString SchemaId(TEXT("uai.modeling.voxel_grid.authoring_state.v2"));
	const FString LegacySchemaId(TEXT("uai.modeling.voxel_grid.authoring_state.v1"));
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
		UE::UAI::Contract::FDiagnostic& OutError, const FString& Target)
	{
		OutError.Reset();
		if (State.GridId.TrimStartAndEnd().IsEmpty())
		{
			OutError = ModelingCondition(TEXT("voxel_grid_authoring_state_grid_id_missing"), TEXT("The native voxel authoring state has no managed grid identity."), TEXT("/grid_identity_present"), false, true, Target);
			OutError.ActualEvidence = MakeShared<FJsonValueString>(State.GridId);
			return false;
		}
		if (!FMath::IsFinite(State.CellSize)
			|| State.CellSize <= KINDA_SMALL_NUMBER)
		{
			const TSharedPtr<FJsonValue> Actual = FMath::IsFinite(State.CellSize)
				? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(State.CellSize))
				: TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(FString::Printf(TEXT("%.17g"), State.CellSize)));
			OutError = ModelingFact(TEXT("voxel_grid_authoring_state_cell_size_invalid"), TEXT("The native voxel cell size must be finite and greater than the engine's small-number tolerance."), TEXT("/cell_size"), Actual, MakeShared<FJsonValueString>(FString::Printf(TEXT("finite number > %.17g"), static_cast<double>(KINDA_SMALL_NUMBER))), Target);
			return false;
		}
		if (!State.GridToWorld.IsValid()
			|| !IsFiniteVector(State.GridToWorld.GetLocation())
			|| !IsFiniteVector(State.AnchorLocal)
			|| State.Revision < 0)
		{
			if (!State.GridToWorld.IsValid())
				OutError = ModelingFact(TEXT("voxel_grid_authoring_state_contract_invalid"), TEXT("The native voxel grid transform fails transform validation."), TEXT("/grid_to_world"), MakeShared<FJsonValueString>(State.GridToWorld.ToString()), MakeShared<FJsonValueString>(TEXT("valid transform")), Target);
			else if (!IsFiniteVector(State.GridToWorld.GetLocation()))
				OutError = ModelingFact(TEXT("voxel_grid_authoring_state_contract_invalid"), TEXT("The native voxel grid location contains a non-finite coordinate."), TEXT("/grid_to_world/location"), MakeShared<FJsonValueString>(State.GridToWorld.GetLocation().ToString()), MakeShared<FJsonValueString>(TEXT("finite vector")), Target);
			else if (!IsFiniteVector(State.AnchorLocal))
				OutError = ModelingFact(TEXT("voxel_grid_authoring_state_contract_invalid"), TEXT("The native voxel anchor contains a non-finite coordinate."), TEXT("/anchor_local"), MakeShared<FJsonValueString>(State.AnchorLocal.ToString()), MakeShared<FJsonValueString>(TEXT("finite vector")), Target);
			else
			{
				OutError = ModelingCondition(TEXT("voxel_grid_authoring_state_contract_invalid"), TEXT("The managed voxel state revision is negative."), TEXT("/managed_revision_valid"), false, true, Target);
				OutError.ActualEvidence = MakeShared<FJsonValueString>(FString::Printf(TEXT("%lld"), State.Revision));
				OutError.ExpectedEvidence = MakeShared<FJsonValueString>(TEXT("non-negative revision"));
			}
			return false;
		}
		if (State.bSolidGeometry)
		{
			if (!State.Cells.IsEmpty())
			{
				OutError = ModelingCondition(TEXT("voxel_grid_authoring_state_mixed_sources"),
					TEXT("A precise solid cannot also retain a second occupied-cell source."), TEXT("/cells_empty"), false, true, Target);
				return false;
			}
			return UE::UAI::DomainAdapters::ModelingGeometry::GridSolid::Validate(State.SolidPieces, OutError);
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
	FString InMaterialRef,
	const bool bInSolidGeometry,
	TArray<FUeAgentGridSolidPiece> InSolidPieces)
{
	Modify();
	Schema = SchemaId;
	GridId = MoveTemp(InGridId);
	CellSize = InCellSize;
	GridToWorld = MoveTemp(InGridToWorld);
	AnchorLocal = MoveTemp(InAnchorLocal);
	Revision = InRevision;
	Cells = MoveTemp(InCells);
	bSolidGeometry = bInSolidGeometry;
	SolidPieces = MoveTemp(InSolidPieces);
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
		UE::UAI::Contract::FDiagnostic& OutError)
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
					OutError = ModelingFact(TEXT("voxel_grid_authoring_state_duplicate"), TEXT("The actor has multiple voxel authoring-state components with the required name."), TEXT("/authoring_state_component_count"), MakeShared<FJsonValueString>(TEXT("at least 2")), MakeShared<FJsonValueNumber>(1), Actor.GetPathName());
					OutError.CandidateName = ComponentName.ToString();
					return false;
				}
				OutComponent = Component;
			}
		}
		if (!OutComponent)
		{
			OutError = ModelingFact(TEXT("voxel_grid_authoring_state_missing"), TEXT("The actor has no voxel authoring-state component with the required name."), TEXT("/authoring_state_component_count"), MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueNumber>(1), Actor.GetPathName());
			OutError.CandidateName = ComponentName.ToString();
			return false;
		}
		return true;
	}

	bool TryCreateAuthoringStateComponent(
		ADynamicMeshActor& Actor,
		UUeAgentModelingVoxelGridStateComponent*& OutComponent,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutComponent = nullptr;
		OutError.Reset();
		UUeAgentModelingVoxelGridStateComponent* Existing = nullptr;
		UE::UAI::Contract::FDiagnostic ExistingError;
		if (TryFindAuthoringStateComponent(Actor, Existing, ExistingError))
		{
			OutComponent = Existing;
			return true;
		}
		if (ExistingError.Code != TEXT("voxel_grid_authoring_state_missing"))
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
			OutError = ModelingCondition(TEXT("voxel_grid_authoring_state_create_failed"), TEXT("The native voxel authoring-state component could not be created on the actor."), TEXT("/authoring_state_component_created"), false, true, Actor.GetPathName());
			OutError.CandidateName = ComponentName.ToString();
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
		FVoxelGridAuthoringState& OutState,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutState = {};
		OutError.Reset();
		if (Component.GetSchema() != SchemaId && Component.GetSchema() != LegacySchemaId)
		{
			OutError = ModelingCondition(TEXT("voxel_grid_authoring_state_schema_unsupported"), TEXT("The native voxel authoring-state component uses an unsupported storage schema."), TEXT("/storage_schema_supported"), false, true, Component.GetOwner() ? Component.GetOwner()->GetPathName() : FString());
			OutError.ActualEvidence = MakeShared<FJsonValueString>(Component.GetSchema());
			OutError.ExpectedEvidence = MakeShared<FJsonValueString>(SchemaId);
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
		OutState.bSolidGeometry = Component.HasSolidGeometry();
		OutState.SolidPieces = Component.GetSolidPieces();
		for (int32 Index = 0; Index < Component.GetCells().Num(); ++Index)
		{
			const FIntVector& Cell = Component.GetCells()[Index];
			if (OutState.Cells.Contains(Cell))
			{
				OutError = ModelingFact(TEXT("voxel_grid_authoring_state_cell_duplicate"), TEXT("The native voxel authoring-state cell list repeats this coordinate."), FString::Printf(TEXT("/cells/%d"), Index), MakeShared<FJsonValueString>(Cell.ToString()), MakeShared<FJsonValueString>(TEXT("unique cell coordinate")), Component.GetOwner() ? Component.GetOwner()->GetPathName() : FString());
				return false;
			}
			OutState.Cells.Add(Cell);
		}
		return ValidateState(OutState, OutError, Component.GetOwner() ? Component.GetOwner()->GetPathName() : FString());
	}

	bool TryWriteAuthoringState(
		UUeAgentModelingVoxelGridStateComponent& Component,
		const FVoxelGridAuthoringState& State,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!ValidateState(State, OutError, Component.GetOwner() ? Component.GetOwner()->GetPathName() : FString()))
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
			State.MaterialRef,
			State.bSolidGeometry,
			State.SolidPieces);
		return true;
	}
}

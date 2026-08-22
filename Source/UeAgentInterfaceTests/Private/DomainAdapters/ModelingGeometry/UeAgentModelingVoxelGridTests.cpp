#include "Misc/AutomationTest.h"

#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "DomainAdapters/ModelingGeometry/UeAgentModelingVoxelGrid.h"
#include "DomainAdapters/ModelingGeometry/UeAgentModelingVoxelGridState.h"
#include "DynamicMeshActor.h"
#include "Engine/World.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "OperationOwners/ModelingGeometry/UeAgentModelingGeometryOperationOwner.h"

namespace
{
	using namespace UE::UAI::DomainCapability;
	using namespace UE::UAI::DomainAdapters::ModelingGeometry::VoxelGrid;
	using namespace UE::UAI::Module::CommandRegistration::Active;

	TSet<FIntVector> ExpandBoxes(
		const TArray<FVoxelGridCollisionBox>& Boxes,
		bool& bOutOverlapped)
	{
		bOutOverlapped = false;
		TSet<FIntVector> Result;
		for (const FVoxelGridCollisionBox& Box : Boxes)
		{
			for (int32 Z = Box.Min.Z; Z < Box.MaxExclusive.Z; ++Z)
			{
				for (int32 Y = Box.Min.Y; Y < Box.MaxExclusive.Y; ++Y)
				{
					for (int32 X = Box.Min.X; X < Box.MaxExclusive.X; ++X)
					{
						const FIntVector Cell(X, Y, Z);
						bOutOverlapped = bOutOverlapped || Result.Contains(Cell);
						Result.Add(Cell);
					}
				}
			}
		}
		return Result;
	}

	bool SameCells(const TSet<FIntVector>& Left, const TSet<FIntVector>& Right)
	{
		if (Left.Num() != Right.Num()) return false;
		for (const FIntVector& Cell : Left)
		{
			if (!Right.Contains(Cell)) return false;
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentModelingVoxelGridContractAndAlgorithmsTest,
	"UeAgentInterface.ModelingGeometry.VoxelGrid.ContractAndAlgorithms",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentModelingVoxelGridContractAndAlgorithmsTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;

	TSet<FIntVector> LShape = {
		FIntVector(0, 0, 0), FIntVector(1, 0, 0), FIntVector(2, 0, 0),
		FIntVector(0, 1, 0), FIntVector(0, 2, 0),
	};
	const TArray<FVoxelGridCollisionBox> Boxes = BuildExactCoverBoxes(LShape);
	TestEqual(TEXT("L shape uses two deterministic cover boxes"), Boxes.Num(), 2);
	bool bOverlapped = false;
	const TSet<FIntVector> Covered = ExpandBoxes(Boxes, bOverlapped);
	TestFalse(TEXT("exact cover boxes never overlap"), bOverlapped);
	TestTrue(TEXT("exact cover boxes equal occupied cells"), SameCells(Covered, LShape));

	const FTransform GridToWorld(
		FRotator(0.0, 45.0, 0.0),
		FVector(1200.0, -300.0, 80.0),
		FVector::OneVector);
	const FBox GridLocalBounds(FVector::ZeroVector, FVector(300.0, 100.0, 100.0));
	FBox WorldAabb(ForceInit);
	for (int32 X = 0; X < 2; ++X)
	{
		for (int32 Y = 0; Y < 2; ++Y)
		{
			for (int32 Z = 0; Z < 2; ++Z)
			{
				WorldAabb += GridToWorld.TransformPosition(FVector(
					X ? GridLocalBounds.Max.X : GridLocalBounds.Min.X,
					Y ? GridLocalBounds.Max.Y : GridLocalBounds.Min.Y,
					Z ? GridLocalBounds.Max.Z : GridLocalBounds.Min.Z));
			}
		}
	}
	FVoxelGridCellRange RotatedCover;
	FString Error;
	TestTrue(
		TEXT("rotated world AABB converts to a grid-local cover"),
		TryConvertBoundsToCellRange(
			WorldAabb,
			TEXT("world"),
			GridToWorld,
			FVector::ZeroVector,
			100.0,
			RotatedCover,
			Error));
	TestTrue(TEXT("rotated cover contains source X cells"),
		RotatedCover.Min.X <= 0 && RotatedCover.MaxExclusive.X >= 3);
	TestTrue(TEXT("rotated cover contains source Y cells"),
		RotatedCover.Min.Y <= 0 && RotatedCover.MaxExclusive.Y >= 1);
	TestTrue(TEXT("rotated cover contains source Z cells"),
		RotatedCover.Min.Z <= 0 && RotatedCover.MaxExclusive.Z >= 1);

	FVoxelGridCellRange AnchoredCover;
	TestTrue(
		TEXT("component-local bounds include the grid anchor"),
		TryConvertBoundsToCellRange(
			FBox(FVector(-50.0), FVector(50.0)),
			TEXT("component_local"),
			FTransform::Identity,
			FVector(50.0),
			100.0,
			AnchoredCover,
			Error));
	TestEqual(TEXT("anchored cover min"), AnchoredCover.Min, FIntVector::ZeroValue);
	TestEqual(TEXT("anchored cover max"),
		AnchoredCover.MaxExclusive, FIntVector(1, 1, 1));

	TSharedPtr<const UE::UAI::CommandRegistry::FUeAgentImmutableCommandRegistry>
		Registry;
	TSharedPtr<const UE::UAI::CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	TestTrue(
		TEXT("process command authority composes"),
		TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error));
	if (!Registry.IsValid())
	{
		AddError(Error);
		return false;
	}
	TestNotNull(TEXT("current voxel-grid session contract exists"),
		Registry->FindExact(GetSessionCommandId()));
	TestNotNull(TEXT("current voxel-grid apply contract exists"),
		Registry->FindExact(GetApplyCommandId()));

	TSharedPtr<const FFrozenDomainCapabilitySet> Frozen;
	TestTrue(TEXT("domain capabilities freeze"),
		TryFreezeProcessDomainCapabilities(Frozen, Error));
	const FDomainCapabilityDeclaration* Modeling = nullptr;
	if (Frozen.IsValid())
	{
		for (const TSharedRef<const FDomainCapabilityContribution>& Contribution
			: Frozen->GetContributions())
		{
			if (Contribution->GetDomainId() == TEXT("modeling-geometry"))
			{
				Modeling = &Contribution->GetDeclaration();
				break;
			}
		}
	}
	TestNotNull(TEXT("modeling geometry has one typed domain contribution"), Modeling);
	if (Modeling)
	{
		const FDomainOperationEntry* SessionOperation =
			Modeling->Operation.Operations.FindByPredicate(
				[](const FDomainOperationEntry& Entry)
				{
					return Entry.OperationId == GetSessionCommandId();
				});
		const FDomainOperationEntry* ApplyOperation =
			Modeling->Operation.Operations.FindByPredicate(
				[](const FDomainOperationEntry& Entry)
				{
					return Entry.OperationId == GetApplyCommandId();
				});
		TestNotNull(TEXT("session operation is declared"), SessionOperation);
		TestNotNull(TEXT("apply operation is declared"), ApplyOperation);
		if (SessionOperation)
		{
			TestEqual(TEXT("session is recoverable transient state"),
				SessionOperation->TransientStatePolicy,
				EDomainTransientStatePolicy::RecoverableSession);
			TestFalse(TEXT("session does not claim persistence"),
				SessionOperation->bPersists);
		}
		if (ApplyOperation)
		{
			TestEqual(TEXT("apply uses a persistent lifecycle"),
				ApplyOperation->TransientStatePolicy,
				EDomainTransientStatePolicy::None);
			TestTrue(TEXT("apply declares persistence"), ApplyOperation->bPersists);
		}
	}
	if (Frozen.IsValid())
	{
		const TSharedPtr<const IDomainOperationInvocationFactory> FrozenBox =
			Frozen->FindOperationFactory(TEXT("mesh.primitive.box.create"));
		const TSharedPtr<const IDomainOperationInvocationFactory> PublicBox =
			UE::UAI::OperationOwners::ModelingGeometry::
				FindOperationInvocationFactory(TEXT("mesh.primitive.box.create"));
		TestTrue(
			TEXT("existing Modeling primitives share the typed operation factory"),
			FrozenBox.IsValid()
				&& PublicBox.IsValid()
				&& FrozenBox.Get() == PublicBox.Get());
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentModelingVoxelGridAuthoringStateTest,
	"UeAgentInterface.ModelingGeometry.VoxelGrid.AuthoringState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentModelingVoxelGridAuthoringStateTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	UWorld* const World = UWorld::CreateWorld(
		EWorldType::EditorPreview, false, NAME_None, nullptr, false);
	if (!TestNotNull(TEXT("state test owns a preview world"), World))
	{
		return false;
	}
	ADynamicMeshActor* const Actor = World->SpawnActor<ADynamicMeshActor>();
	if (!TestNotNull(TEXT("state test owns a dynamic mesh actor"), Actor))
	{
		World->DestroyWorld(false);
		World->MarkAsGarbage();
		return false;
	}

	UUeAgentModelingVoxelGridStateComponent* Component = nullptr;
	FString Error;
	TestTrue(
		TEXT("one editor-only authoring component is created"),
		TryCreateAuthoringStateComponent(*Actor, Component, Error));
	TestNotNull(TEXT("authoring component exists"), Component);
	UUeAgentModelingVoxelGridStateComponent* Reused = nullptr;
	TestTrue(
		TEXT("component creation reuses the unique owner"),
		TryCreateAuthoringStateComponent(*Actor, Reused, Error));
	TestTrue(TEXT("component owner is unique"), Reused == Component);

	FVoxelGridAuthoringState Expected;
	Expected.GridId = TEXT("VG_STATE_TEST");
	Expected.CellSize = 80.0;
	Expected.GridToWorld = FTransform(
		FRotator(0.0, 25.0, 0.0),
		FVector(100.0, 200.0, 300.0),
		FVector::OneVector);
	Expected.AnchorLocal = FVector(40.0);
	Expected.Cells = {FIntVector(0, 0, 0), FIntVector(1, 2, 3)};
	Expected.Revision = 7;
	Expected.CollisionGeneration = TEXT("GEN_STATE_TEST");
	Expected.CollisionMode = TEXT("exact_box_components");
	Expected.MaterialRef = TEXT("/Engine/BasicShapes/BasicShapeMaterial");
	TestTrue(
		TEXT("valid authoring state writes"),
		Component && TryWriteAuthoringState(*Component, Expected, 16, Error));
	FVoxelGridAuthoringState Observed;
	TestTrue(
		TEXT("serialized state reads through the canonical helper"),
		Component && TryReadAuthoringState(*Component, 16, Observed, Error));
	TestTrue(
		TEXT("authoring state round-trips exactly"),
		Observed.GridId == Expected.GridId
			&& Observed.CellSize == Expected.CellSize
			&& Observed.GridToWorld.Equals(Expected.GridToWorld, 0.001)
			&& Observed.AnchorLocal.Equals(Expected.AnchorLocal, 0.001)
			&& SameCells(Observed.Cells, Expected.Cells)
			&& Observed.Revision == Expected.Revision
			&& Observed.CollisionGeneration == Expected.CollisionGeneration
			&& Observed.CollisionMode == Expected.CollisionMode
			&& Observed.MaterialRef == Expected.MaterialRef);

	FVoxelGridAuthoringState Invalid = Expected;
	Invalid.Revision = -1;
	TestFalse(
		TEXT("invalid replacement fails closed"),
		TryWriteAuthoringState(*Component, Invalid, 16, Error));
	FVoxelGridAuthoringState AfterRejectedWrite;
	TestTrue(
		TEXT("failed replacement preserves the previous authoritative state"),
		TryReadAuthoringState(
			*Component, 16, AfterRejectedWrite, Error)
			&& AfterRejectedWrite.Revision == Expected.Revision
			&& SameCells(AfterRejectedWrite.Cells, Expected.Cells));

	World->DestroyWorld(false);
	World->MarkAsGarbage();
	return !HasAnyErrors();
}

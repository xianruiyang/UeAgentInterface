#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CommandEdge/UeAgentCommandDispatch.h"
#include "CoreMinimal.h"

class UWorld;

namespace UE::UAI::DomainAdapters::ModelingGeometry::VoxelGrid
{
	struct FVoxelGridCellRange final
	{
		FIntVector Min = FIntVector::ZeroValue;
		FIntVector MaxExclusive = FIntVector::ZeroValue;

		bool IsValid() const;
		int64 NumCells() const;
	};

	struct FVoxelGridCollisionBox final
	{
		FIntVector Min = FIntVector::ZeroValue;
		FIntVector MaxExclusive = FIntVector::ZeroValue;

		bool IsValid() const;
		int64 NumCells() const;
	};

	UEAGENTINTERFACE_API const FString& GetSessionCommandId();
	UEAGENTINTERFACE_API const FString& GetBeginCommandId();
	UEAGENTINTERFACE_API const FString& GetApplyCommandId();
	UEAGENTINTERFACE_API const TArray<FString>& GetCommandIds();
	UEAGENTINTERFACE_API bool IsCommand(const FString& CommandId);

	UEAGENTINTERFACE_API bool TryConvertBoundsToCellRange(
		const FBox& Bounds,
		const FString& InputSpace,
		const FTransform& GridToWorld,
		const FVector& AnchorLocal,
		double CellSize,
		FVoxelGridCellRange& OutRange,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API TArray<FVoxelGridCollisionBox>
		BuildExactCoverBoxes(const TSet<FIntVector>& Cells);

	UEAGENTINTERFACE_API bool TryCreateMechanicsHandler(
		const FString& CommandId,
		TSharedPtr<CommandEdge::IUeAgentCommandHandler>& OutHandler,
		UE::UAI::Contract::FDiagnostic& OutError);

#if WITH_DEV_AUTOMATION_TESTS
	UEAGENTINTERFACE_API void ResetProcessSessionsForAutomation();
	UEAGENTINTERFACE_API void SetAutomationWorldOverride(UWorld* World);
#endif
}

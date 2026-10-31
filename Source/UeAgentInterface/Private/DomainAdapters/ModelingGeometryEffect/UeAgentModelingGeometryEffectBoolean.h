#pragma once

#include "CoreMinimal.h"
#include "Core/ExecutionMutation/UeAgentExecutionMutation.h"

namespace UE::Geometry { class FDynamicMesh3; }

namespace UE::UAI::DomainAdapters::ModelingGeometryEffect
{
	// The same native kernel supplies the planned geometry and the applied edit.
	bool TryComputeModelingBoolean(const UE::Geometry::FDynamicMesh3& Target,
		const UE::Geometry::FDynamicMesh3& Operand, const FTransform& OperandToTarget,
		const FString& Operation, bool bSimplifyOutput, UE::Geometry::FDynamicMesh3& OutMesh,
		ExecutionMutation::FExecutionMutationError& OutError);
}

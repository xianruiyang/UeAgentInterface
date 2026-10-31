#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryEffectBoolean.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "GeometryScript/MeshBooleanFunctions.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace UE::UAI::DomainAdapters::ModelingGeometryEffect
{
	bool TryComputeModelingBoolean(const UE::Geometry::FDynamicMesh3& Target,
		const UE::Geometry::FDynamicMesh3& Operand, const FTransform& OperandToTarget,
		const FString& Operation, const bool bSimplifyOutput, UE::Geometry::FDynamicMesh3& OutMesh,
		ExecutionMutation::FExecutionMutationError& OutError)
	{
		TStrongObjectPtr<UDynamicMesh> TargetMesh(NewObject<UDynamicMesh>(GetTransientPackage()));
		TStrongObjectPtr<UDynamicMesh> OperandMesh(NewObject<UDynamicMesh>(GetTransientPackage()));
		TargetMesh->EditMesh([&](UE::Geometry::FDynamicMesh3& Mesh) { Mesh = Target; });
		OperandMesh->EditMesh([&](UE::Geometry::FDynamicMesh3& Mesh) { Mesh = Operand; });
		FGeometryScriptMeshBooleanOptions Options;
		Options.OutputTransformSpace = EGeometryScriptBooleanOutputSpace::TargetTransformSpace;
		Options.bSimplifyOutput = bSimplifyOutput;
		const EGeometryScriptBooleanOperation NativeOperation = Operation == TEXT("intersection")
			? EGeometryScriptBooleanOperation::Intersection : Operation == TEXT("difference")
			? EGeometryScriptBooleanOperation::Subtract : EGeometryScriptBooleanOperation::Union;
		UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshBoolean(TargetMesh.Get(),
			FTransform::Identity, OperandMesh.Get(), OperandToTarget, NativeOperation, Options);
		TargetMesh->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& Mesh) { OutMesh = Mesh; });
		if (OutMesh.VertexCount() == 0 || OutMesh.TriangleCount() == 0)
		{
			OutError = ExecutionMutation::FExecutionMutationError::Failure(
				TEXT("modeling_geometry_boolean_empty_result"),
				TEXT("The native boolean produced no mesh geometry."), TEXT("execution"));
			return false;
		}
		return true;
	}
}

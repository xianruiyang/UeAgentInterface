#pragma once

#include "CoreMinimal.h"

namespace UE::Geometry
{
	class FDynamicMesh3;
}

namespace UE::UAI::DomainAdapters::ModelingGeometryEffect
{
	struct FBoundarySemanticObservation final
	{
		FString Fingerprint;
		int32 EdgeCount = 0;
		int32 VertexCount = 0;
		// Retain the sorted hash inputs so a mismatch can report concrete coordinates.
		TArray<FVector3d> OrderedEndpoints;

		bool IsValid() const;
		bool operator==(const FBoundarySemanticObservation& Other) const;
		bool operator!=(const FBoundarySemanticObservation& Other) const;
	};

	FBoundarySemanticObservation ObserveBoundarySemantics(
		const UE::Geometry::FDynamicMesh3& Mesh);
}

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

		bool IsValid() const;
		bool operator==(const FBoundarySemanticObservation& Other) const;
		bool operator!=(const FBoundarySemanticObservation& Other) const;
	};

	FBoundarySemanticObservation ObserveBoundarySemantics(
		const UE::Geometry::FDynamicMesh3& Mesh);
}

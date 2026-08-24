#pragma once

#include "CoreMinimal.h"

namespace UE::Geometry
{
	class FDynamicMesh3;
}

namespace UE::UAI::DomainAdapters::ModelingGeometryEffect
{
	// A face normal is treated as directionally equal after conversion when
	// the normalized vectors differ by no more than this dot-product bound.
	inline constexpr double FaceNormalMinimumAlignmentDot = 0.9999;
	inline constexpr double WeightedNormalMinimumAlignmentDot = 0.9999;

	struct UEAGENTINTERFACE_API FNormalInteriorEdgeObservation
	{
		int32 EdgeId = INDEX_NONE;
		double FaceNormalDot = -1.0;
		bool bFirstVertexSplit = false;
		bool bSecondVertexSplit = false;
		bool bValid = false;
	};

	// These are carrier-independent observations, not an operation pass/fail
	// result. Missing and set corners partition TriangleCornerCount; aligned,
	// misaligned, and invalid corners partition SetTriangleCornerCount.
	struct UEAGENTINTERFACE_API FNormalSemanticObservation
	{
		int32 TriangleCount = 0;
		int32 MeshVertexCount = 0;
		int32 TriangleCornerCount = 0;
		int32 SetTriangleCornerCount = 0;
		int32 MissingTriangleCornerCount = 0;
		int32 ValidTriangleCornerCount = 0;
		int32 FaceAlignedTriangleCornerCount = 0;
		int32 FaceMisalignedTriangleCornerCount = 0;
		int32 InvalidTriangleCornerCount = 0;
		int32 ParentVertexMismatchTriangleCornerCount = 0;
		int32 ReferencedVertexCount = 0;
		int32 SingleNormalElementVertexCount = 0;
		int32 SplitNormalElementVertexCount = 0;
		int32 MissingNormalElementVertexCount = 0;
		int32 ReferencedNormalElementCount = 0;
		int32 WeightedAlignedNormalElementCount = 0;
		int32 WeightedMisalignedNormalElementCount = 0;
		int32 InteriorEdgeCount = 0;
		int32 FullySmoothInteriorEdgeCount = 0;
		int32 FullySplitInteriorEdgeCount = 0;
		int32 PartiallySplitInteriorEdgeCount = 0;
		int32 InvalidInteriorEdgeCount = 0;
		TArray<FString> SplitInteriorEdgeEndpointKeys;
		TArray<FNormalInteriorEdgeObservation> InteriorEdges;
	};

	UEAGENTINTERFACE_API FNormalSemanticObservation
	ObserveNormalSemantics(const UE::Geometry::FDynamicMesh3& Mesh);

	UEAGENTINTERFACE_API bool IsCompleteFaceNormalObservation(
		const FNormalSemanticObservation& Observation);

	UEAGENTINTERFACE_API bool IsCompleteWeightedNormalObservation(
		const FNormalSemanticObservation& Observation);

	UEAGENTINTERFACE_API bool HasPerVertexNormalTopology(
		const FNormalSemanticObservation& Observation);

	UEAGENTINTERFACE_API bool
	IsCompleteAreaAnglePerVertexNormalObservation(
		const FNormalSemanticObservation& Observation);

	UEAGENTINTERFACE_API bool MatchesNormalOpeningAngle(
		const FNormalSemanticObservation& Observation,
		double OpeningAngleDegrees);

	UEAGENTINTERFACE_API bool
	IsCompleteAngleThresholdNormalObservation(
		const FNormalSemanticObservation& Observation,
		double OpeningAngleDegrees);
}

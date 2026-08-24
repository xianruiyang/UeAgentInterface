#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryEffectNormalObservation.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/DynamicMeshOverlay.h"
#include "DynamicMesh/MeshNormals.h"

namespace UE::UAI::DomainAdapters::ModelingGeometryEffect
{
	using UE::Geometry::FDynamicMesh3;
	using UE::Geometry::FDynamicMeshNormalOverlay;
	using UE::Geometry::FIndex2i;
	using UE::Geometry::FIndex3i;
	using UE::Geometry::FMeshNormals;

	namespace
	{
		int32 FindTriangleCorner(
			const FIndex3i& Triangle,
			const int32 VertexId)
		{
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				if (Triangle[Corner] == VertexId)
				{
					return Corner;
				}
			}
			return INDEX_NONE;
		}

		bool PositionLess(
			const FVector3f& Left,
			const FVector3f& Right)
		{
			return Left.X < Right.X
				|| (Left.X == Right.X
					&& (Left.Y < Right.Y
						|| (Left.Y == Right.Y
							&& Left.Z < Right.Z)));
		}

		FString PositionKey(const FVector3f& Position)
		{
			return FString::Printf(
				TEXT("%.9g,%.9g,%.9g"),
				static_cast<double>(Position.X),
				static_cast<double>(Position.Y),
				static_cast<double>(Position.Z));
		}

		FString SplitEndpointKey(
			const FDynamicMesh3& Mesh,
			const FIndex2i& EdgeVertices,
			const int32 EndpointIndex)
		{
			const FVector3f FirstPosition(
				Mesh.GetVertex(EdgeVertices.A));
			const FVector3f SecondPosition(
				Mesh.GetVertex(EdgeVertices.B));
			const bool bSwap = PositionLess(
				SecondPosition,
				FirstPosition);
			const FVector3f& CanonicalFirst = bSwap
				? SecondPosition
				: FirstPosition;
			const FVector3f& CanonicalSecond = bSwap
				? FirstPosition
				: SecondPosition;
			const int32 CanonicalEndpointIndex = bSwap
				? 1 - EndpointIndex
				: EndpointIndex;
			return PositionKey(CanonicalFirst)
				+ TEXT("|")
				+ PositionKey(CanonicalSecond)
				+ TEXT("|")
				+ FString::FromInt(CanonicalEndpointIndex);
		}
	}

	FNormalSemanticObservation ObserveNormalSemantics(
		const FDynamicMesh3& Mesh)
	{
		FNormalSemanticObservation Observation;
		Observation.TriangleCount = Mesh.TriangleCount();
		Observation.MeshVertexCount = Mesh.VertexCount();
		Observation.TriangleCornerCount =
			Observation.TriangleCount * 3;

		const FDynamicMeshNormalOverlay* Normals =
			Mesh.HasAttributes()
				? Mesh.Attributes()->PrimaryNormals()
				: nullptr;
		if (!Normals)
		{
			Observation.MissingTriangleCornerCount =
				Observation.TriangleCornerCount;
			return Observation;
		}

		TMap<int32, TSet<int32>> NormalElementsByVertex;
		TSet<int32> ReferencedNormalElements;
		for (const int32 TriangleId : Mesh.TriangleIndicesItr())
		{
			if (!Normals->IsSetTriangle(TriangleId))
			{
				Observation.MissingTriangleCornerCount += 3;
				continue;
			}

			Observation.SetTriangleCornerCount += 3;
			const FVector3d FaceNormal =
				Mesh.GetTriNormal(TriangleId).GetSafeNormal();
			const bool bValidFaceNormal =
				FaceNormal.SizeSquared() > UE_DOUBLE_SMALL_NUMBER;
			const FIndex3i Elements =
				Normals->GetTriangle(TriangleId);
			const FIndex3i Triangle = Mesh.GetTriangle(TriangleId);
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const int32 ElementId = Elements[Corner];
				if (!bValidFaceNormal
					|| !Normals->IsElement(ElementId))
				{
					++Observation.InvalidTriangleCornerCount;
					continue;
				}
				const int32 VertexId = Triangle[Corner];
				if (Normals->GetParentVertex(ElementId) != VertexId)
				{
					++Observation.InvalidTriangleCornerCount;
					++Observation.
						ParentVertexMismatchTriangleCornerCount;
					continue;
				}

				const FVector3f StoredNormal =
					Normals->GetElement(ElementId);
				const FVector3d Normal(
					StoredNormal.X,
					StoredNormal.Y,
					StoredNormal.Z);
				const FVector3d UnitNormal = Normal.GetSafeNormal();
				if (UnitNormal.SizeSquared()
					<= UE_DOUBLE_SMALL_NUMBER)
				{
					++Observation.InvalidTriangleCornerCount;
					continue;
				}

				++Observation.ValidTriangleCornerCount;
				NormalElementsByVertex.FindOrAdd(VertexId).Add(
					ElementId);
				ReferencedNormalElements.Add(ElementId);
				const double Alignment =
					FVector3d::DotProduct(FaceNormal, UnitNormal);
				if (Alignment >= FaceNormalMinimumAlignmentDot)
				{
					++Observation.FaceAlignedTriangleCornerCount;
				}
				else
				{
					++Observation.FaceMisalignedTriangleCornerCount;
				}
			}
		}

		Observation.ReferencedVertexCount =
			NormalElementsByVertex.Num();
		Observation.MissingNormalElementVertexCount =
			Observation.MeshVertexCount
			- Observation.ReferencedVertexCount;
		for (const TPair<int32, TSet<int32>>& Entry
			: NormalElementsByVertex)
		{
			if (Entry.Value.Num() == 1)
			{
				++Observation.SingleNormalElementVertexCount;
			}
			else if (Entry.Value.Num() > 1)
			{
				++Observation.SplitNormalElementVertexCount;
			}
		}

		Observation.ReferencedNormalElementCount =
			ReferencedNormalElements.Num();
		for (const int32 ElementId : ReferencedNormalElements)
		{
			const FVector3f StoredNormal =
				Normals->GetElement(ElementId);
			const FVector3d ActualNormal(
				StoredNormal.X,
				StoredNormal.Y,
				StoredNormal.Z);
			const FVector3d ExpectedNormal =
				FMeshNormals::ComputeOverlayNormal(
					Mesh,
					Normals,
					ElementId,
					true,
					true).GetSafeNormal();
			const double Alignment = FVector3d::DotProduct(
				ActualNormal.GetSafeNormal(),
				ExpectedNormal);
			if (ActualNormal.SizeSquared() > UE_DOUBLE_SMALL_NUMBER
				&& ExpectedNormal.SizeSquared()
					> UE_DOUBLE_SMALL_NUMBER
				&& Alignment >= WeightedNormalMinimumAlignmentDot)
			{
				++Observation.WeightedAlignedNormalElementCount;
			}
			else
			{
				++Observation.WeightedMisalignedNormalElementCount;
			}
		}

		for (const int32 EdgeId : Mesh.EdgeIndicesItr())
		{
			const FIndex2i EdgeTriangles = Mesh.GetEdgeT(EdgeId);
			if (EdgeTriangles.B < 0)
			{
				continue;
			}
			++Observation.InteriorEdgeCount;
			FNormalInteriorEdgeObservation EdgeObservation;
			EdgeObservation.EdgeId = EdgeId;
			const FVector3d FirstFaceNormal =
				Mesh.GetTriNormal(EdgeTriangles.A).GetSafeNormal();
			const FVector3d SecondFaceNormal =
				Mesh.GetTriNormal(EdgeTriangles.B).GetSafeNormal();
			EdgeObservation.FaceNormalDot = FMath::Clamp(
				FVector3d::DotProduct(
					FirstFaceNormal,
					SecondFaceNormal),
				-1.0,
				1.0);
			if (!Normals->IsSetTriangle(EdgeTriangles.A)
				|| !Normals->IsSetTriangle(EdgeTriangles.B)
				|| FirstFaceNormal.SizeSquared()
					<= UE_DOUBLE_SMALL_NUMBER
				|| SecondFaceNormal.SizeSquared()
					<= UE_DOUBLE_SMALL_NUMBER)
			{
				++Observation.InvalidInteriorEdgeCount;
				Observation.InteriorEdges.Add(EdgeObservation);
				continue;
			}

			const FIndex2i EdgeVertices = Mesh.GetEdgeV(EdgeId);
			const FIndex3i FirstTriangle =
				Mesh.GetTriangle(EdgeTriangles.A);
			const FIndex3i SecondTriangle =
				Mesh.GetTriangle(EdgeTriangles.B);
			const FIndex3i FirstElements =
				Normals->GetTriangle(EdgeTriangles.A);
			const FIndex3i SecondElements =
				Normals->GetTriangle(EdgeTriangles.B);
			const int32 FirstCornerA = FindTriangleCorner(
				FirstTriangle,
				EdgeVertices.A);
			const int32 FirstCornerB = FindTriangleCorner(
				SecondTriangle,
				EdgeVertices.A);
			const int32 SecondCornerA = FindTriangleCorner(
				FirstTriangle,
				EdgeVertices.B);
			const int32 SecondCornerB = FindTriangleCorner(
				SecondTriangle,
				EdgeVertices.B);
			if (FirstCornerA == INDEX_NONE
				|| FirstCornerB == INDEX_NONE
				|| SecondCornerA == INDEX_NONE
				|| SecondCornerB == INDEX_NONE)
			{
				++Observation.InvalidInteriorEdgeCount;
				Observation.InteriorEdges.Add(EdgeObservation);
				continue;
			}
			EdgeObservation.bFirstVertexSplit =
				FirstElements[FirstCornerA]
					!= SecondElements[FirstCornerB];
			EdgeObservation.bSecondVertexSplit =
				FirstElements[SecondCornerA]
					!= SecondElements[SecondCornerB];
			EdgeObservation.bValid = true;
			if (EdgeObservation.bFirstVertexSplit)
			{
				Observation.SplitInteriorEdgeEndpointKeys.Add(
					SplitEndpointKey(
						Mesh,
						EdgeVertices,
						0));
			}
			if (EdgeObservation.bSecondVertexSplit)
			{
				Observation.SplitInteriorEdgeEndpointKeys.Add(
					SplitEndpointKey(
						Mesh,
						EdgeVertices,
						1));
			}
			if (EdgeObservation.bFirstVertexSplit
				&& EdgeObservation.bSecondVertexSplit)
			{
				++Observation.FullySplitInteriorEdgeCount;
			}
			else if (!EdgeObservation.bFirstVertexSplit
				&& !EdgeObservation.bSecondVertexSplit)
			{
				++Observation.FullySmoothInteriorEdgeCount;
			}
			else
			{
				++Observation.PartiallySplitInteriorEdgeCount;
			}
			Observation.InteriorEdges.Add(EdgeObservation);
		}
		Observation.SplitInteriorEdgeEndpointKeys.Sort();

		return Observation;
	}

	bool IsCompleteFaceNormalObservation(
		const FNormalSemanticObservation& Observation)
	{
		return Observation.TriangleCornerCount > 0
			&& Observation.SetTriangleCornerCount
				== Observation.TriangleCornerCount
			&& Observation.ValidTriangleCornerCount
				== Observation.TriangleCornerCount
			&& Observation.FaceAlignedTriangleCornerCount
				== Observation.TriangleCornerCount
			&& Observation.MissingTriangleCornerCount == 0
			&& Observation.FaceMisalignedTriangleCornerCount == 0
			&& Observation.InvalidTriangleCornerCount == 0;
	}

	bool IsCompleteWeightedNormalObservation(
		const FNormalSemanticObservation& Observation)
	{
		return Observation.TriangleCornerCount > 0
			&& Observation.SetTriangleCornerCount
				== Observation.TriangleCornerCount
			&& Observation.ValidTriangleCornerCount
				== Observation.TriangleCornerCount
			&& Observation.MissingTriangleCornerCount == 0
			&& Observation.InvalidTriangleCornerCount == 0
			&& Observation.ParentVertexMismatchTriangleCornerCount == 0
			&& Observation.ReferencedNormalElementCount > 0
			&& Observation.WeightedAlignedNormalElementCount
				== Observation.ReferencedNormalElementCount
			&& Observation.WeightedMisalignedNormalElementCount == 0;
	}

	bool HasPerVertexNormalTopology(
		const FNormalSemanticObservation& Observation)
	{
		return Observation.MeshVertexCount > 0
			&& Observation.ReferencedVertexCount
				== Observation.MeshVertexCount
			&& Observation.SingleNormalElementVertexCount
				== Observation.ReferencedVertexCount
			&& Observation.SplitNormalElementVertexCount == 0
			&& Observation.MissingNormalElementVertexCount == 0
			&& Observation.InvalidInteriorEdgeCount == 0
			&& Observation.PartiallySplitInteriorEdgeCount == 0
			&& Observation.FullySplitInteriorEdgeCount == 0
			&& Observation.FullySmoothInteriorEdgeCount
				== Observation.InteriorEdgeCount
			&& Observation.SplitInteriorEdgeEndpointKeys.IsEmpty();
	}

	bool IsCompleteAreaAnglePerVertexNormalObservation(
		const FNormalSemanticObservation& Observation)
	{
		return IsCompleteWeightedNormalObservation(Observation)
			&& HasPerVertexNormalTopology(Observation);
	}

	bool MatchesNormalOpeningAngle(
		const FNormalSemanticObservation& Observation,
		const double OpeningAngleDegrees)
	{
		if (Observation.InteriorEdgeCount <= 0
			|| Observation.InvalidInteriorEdgeCount != 0
			|| Observation.InteriorEdges.Num()
				!= Observation.InteriorEdgeCount)
		{
			return false;
		}
		const double DotThreshold = FMath::Cos(
			FMath::DegreesToRadians(OpeningAngleDegrees));
		for (const FNormalInteriorEdgeObservation& Edge
			: Observation.InteriorEdges)
		{
			if (!Edge.bValid)
			{
				return false;
			}
			const bool bExpectedSplit =
				Edge.FaceNormalDot < DotThreshold;
			if (Edge.bFirstVertexSplit != bExpectedSplit
				|| Edge.bSecondVertexSplit != bExpectedSplit)
			{
				return false;
			}
		}
		return true;
	}

	bool IsCompleteAngleThresholdNormalObservation(
		const FNormalSemanticObservation& Observation,
		const double OpeningAngleDegrees)
	{
		return IsCompleteWeightedNormalObservation(Observation)
			&& MatchesNormalOpeningAngle(
				Observation,
				OpeningAngleDegrees);
	}
}

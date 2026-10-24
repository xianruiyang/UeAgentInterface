#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryPublicStaticMeshTestSupport.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Engine/StaticMesh.h"
#include "MeshDescriptionToDynamicMesh.h"

namespace UE::UAI::Tests::ModelingGeometryPublicStaticMeshSelectionTopology
{
	using namespace UE::Geometry;
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::CommandRegistry;
	using namespace UE::UAI::Tests::ModelingGeometryPublicStaticMesh;

	struct FSelectionTopologyObservation final
	{
		int32 VertexCount = 0;
		int32 EdgeCount = 0;
		int32 TriangleCount = 0;
		int32 BoundaryEdgeCount = 0;
		FVector3d Min = FVector3d::Zero();
		FVector3d Max = FVector3d::Zero();
		double PositionSquaredSum = 0.0;

		bool operator==(const FSelectionTopologyObservation& Other) const
		{
			return VertexCount == Other.VertexCount
				&& EdgeCount == Other.EdgeCount
				&& TriangleCount == Other.TriangleCount
				&& BoundaryEdgeCount == Other.BoundaryEdgeCount
				&& Min.Equals(Other.Min, 0.001)
				&& Max.Equals(Other.Max, 0.001)
				&& FMath::IsNearlyEqual(
					PositionSquaredSum,
					Other.PositionSquaredSum,
					0.001);
		}
	};

	bool ReadSelectionTopology(
		UStaticMesh& StaticMesh,
		FSelectionTopologyObservation& Out)
	{
		Out = {};
		const FMeshDescription* const Description =
			StaticMesh.GetMeshDescription(0);
		if (!Description)
		{
			return false;
		}
		FDynamicMesh3 Mesh;
		FMeshDescriptionToDynamicMesh Converter;
		Converter.Convert(Description, Mesh);
		const FAxisAlignedBox3d Bounds = Mesh.GetBounds();
		Out.VertexCount = Mesh.VertexCount();
		Out.EdgeCount = Mesh.EdgeCount();
		Out.TriangleCount = Mesh.TriangleCount();
		Out.Min = Bounds.Min;
		Out.Max = Bounds.Max;
		for (const int32 EdgeId : Mesh.EdgeIndicesItr())
		{
			Out.BoundaryEdgeCount += Mesh.IsBoundaryEdge(EdgeId) ? 1 : 0;
		}
		for (const int32 VertexId : Mesh.VertexIndicesItr())
		{
			Out.PositionSquaredSum += Mesh.GetVertex(VertexId).SquaredLength();
		}
		return Out.VertexCount > 0
			&& Out.EdgeCount > 0
			&& Out.TriangleCount > 0
			&& !Bounds.IsEmpty();
	}

	TArray<TSharedPtr<FJsonValue>> Numbers(
		std::initializer_list<double> Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		Result.Reserve(static_cast<int32>(Values.size()));
		for (const double Value : Values)
		{
			Result.Add(MakeShared<FJsonValueNumber>(Value));
		}
		return Result;
	}

	TSharedRef<FJsonObject> TriangleSelection(
		std::initializer_list<int32> TriangleIds)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		Values.Reserve(static_cast<int32>(TriangleIds.size()));
		for (const int32 TriangleId : TriangleIds)
		{
			Values.Add(MakeShared<FJsonValueNumber>(TriangleId));
		}
		const TSharedRef<FJsonObject> Selection = MakeShared<FJsonObject>();
		Selection->SetStringField(TEXT("element_kind"), TEXT("triangle"));
		Selection->SetArrayField(TEXT("element_ids"), MoveTemp(Values));
		return Selection;
	}

	TSharedRef<FJsonObject> EdgeSelection(
		std::initializer_list<int32> EdgeIds)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		Values.Reserve(static_cast<int32>(EdgeIds.size()));
		for (const int32 EdgeId : EdgeIds)
		{
			Values.Add(MakeShared<FJsonValueNumber>(EdgeId));
		}
		const TSharedRef<FJsonObject> Selection = MakeShared<FJsonObject>();
		Selection->SetStringField(TEXT("element_kind"), TEXT("edge"));
		Selection->SetArrayField(TEXT("element_ids"), MoveTemp(Values));
		return Selection;
	}

	TSharedRef<FJsonObject> BoundaryEdgeSelection(const int32 EdgeId)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		Values.Add(MakeShared<FJsonValueNumber>(EdgeId));
		const TSharedRef<FJsonObject> Selection = MakeShared<FJsonObject>();
		Selection->SetStringField(
			TEXT("element_kind"),
			TEXT("boundary_edge"));
		Selection->SetArrayField(TEXT("element_ids"), MoveTemp(Values));
		return Selection;
	}

	bool PrepareOpenTopCube(UStaticMesh& StaticMesh)
	{
		const FMeshDescription* const Description =
			StaticMesh.GetMeshDescription(0);
		if (!Description)
		{
			return false;
		}
		FDynamicMesh3 Mesh;
		FMeshDescriptionToDynamicMesh Converter;
		Converter.Convert(Description, Mesh);
		return Mesh.IsTriangle(2)
			&& Mesh.IsTriangle(3)
			&& Mesh.RemoveTriangle(3, false, false)
				== EMeshResult::Ok
			&& Mesh.RemoveTriangle(2, false, false)
				== EMeshResult::Ok
			&& ReplaceStaticMeshGeometry(StaticMesh, Mesh);
	}

	bool FindOpenTopBoundaryEdge(
		UStaticMesh& StaticMesh,
		int32& OutEdgeId)
	{
		OutEdgeId = INDEX_NONE;
		const FMeshDescription* const Description =
			StaticMesh.GetMeshDescription(0);
		if (!Description)
		{
			return false;
		}
		FDynamicMesh3 Mesh;
		FMeshDescriptionToDynamicMesh Converter;
		Converter.Convert(Description, Mesh);
		for (const int32 EdgeId : Mesh.EdgeIndicesItr())
		{
			if (!Mesh.IsBoundaryEdge(EdgeId))
			{
				continue;
			}
			const FIndex2i Edge = Mesh.GetEdgeV(EdgeId);
			if (FMath::IsNearlyEqual(
					Mesh.GetVertex(Edge.A).Z,
					50.0,
					0.001)
				&& FMath::IsNearlyEqual(
					Mesh.GetVertex(Edge.B).Z,
					50.0,
					0.001))
			{
				OutEdgeId = EdgeId;
				return true;
			}
		}
		return false;
	}

	bool PrepareWeldPair(UStaticMesh& StaticMesh)
	{
		FDynamicMesh3 Mesh;
		Mesh.EnableTriangleGroups();
		const int32 A0 = Mesh.AppendVertex(FVector3d(0.0, 0.0, 0.0));
		const int32 A1 = Mesh.AppendVertex(FVector3d(100.0, 0.0, 0.0));
		const int32 A2 = Mesh.AppendVertex(FVector3d(0.0, 100.0, 0.0));
		const int32 B0 = Mesh.AppendVertex(FVector3d(100.0, 0.0, 0.5));
		const int32 B1 = Mesh.AppendVertex(FVector3d(0.0, 0.0, 0.5));
		const int32 B2 = Mesh.AppendVertex(FVector3d(100.0, -100.0, 0.5));
		if (Mesh.AppendTriangle(FIndex3i(A0, A1, A2), 0)
			== FDynamicMesh3::InvalidID
			|| Mesh.AppendTriangle(FIndex3i(B0, B1, B2), 1)
				== FDynamicMesh3::InvalidID)
		{
			return false;
		}
		Mesh.EnableAttributes();
		return ReplaceStaticMeshGeometry(StaticMesh, Mesh);
	}

	bool FindWeldPair(
		UStaticMesh& StaticMesh,
		int32& OutEdgeA,
		int32& OutEdgeB)
	{
		OutEdgeA = INDEX_NONE;
		OutEdgeB = INDEX_NONE;
		const FMeshDescription* const Description =
			StaticMesh.GetMeshDescription(0);
		if (!Description)
		{
			return false;
		}
		FDynamicMesh3 Mesh;
		FMeshDescriptionToDynamicMesh Converter;
		Converter.Convert(Description, Mesh);
		for (const int32 EdgeId : Mesh.EdgeIndicesItr())
		{
			if (!Mesh.IsBoundaryEdge(EdgeId))
			{
				continue;
			}
			const FIndex2i Edge = Mesh.GetEdgeV(EdgeId);
			const FVector3d A = Mesh.GetVertex(Edge.A);
			const FVector3d B = Mesh.GetVertex(Edge.B);
			if (!FMath::IsNearlyZero(A.Y, 0.001)
				|| !FMath::IsNearlyZero(B.Y, 0.001)
				|| !FMath::IsNearlyEqual(
					FVector3d::Distance(A, B),
					100.0,
					0.001))
			{
				continue;
			}
			if (FMath::IsNearlyZero(A.Z, 0.001)
				&& FMath::IsNearlyZero(B.Z, 0.001))
			{
				OutEdgeA = EdgeId;
			}
			else if (FMath::IsNearlyEqual(A.Z, 0.5, 0.001)
				&& FMath::IsNearlyEqual(B.Z, 0.5, 0.001))
			{
				OutEdgeB = EdgeId;
			}
		}
		return OutEdgeA != INDEX_NONE && OutEdgeB != INDEX_NONE;
	}

	bool HasExpectedTopFrontEdge(UStaticMesh& StaticMesh, const int32 EdgeId)
	{
		const FMeshDescription* const Description =
			StaticMesh.GetMeshDescription(0);
		if (!Description)
		{
			return false;
		}
		FDynamicMesh3 Mesh;
		FMeshDescriptionToDynamicMesh Converter;
		Converter.Convert(Description, Mesh);
		if (!Mesh.IsEdge(EdgeId))
		{
			return false;
		}
		const FIndex2i Edge = Mesh.GetEdgeV(EdgeId);
		const FVector3d A = Mesh.GetVertex(Edge.A);
		const FVector3d B = Mesh.GetVertex(Edge.B);
		return FMath::IsNearlyEqual(A.Y, -50.0, 0.001)
			&& FMath::IsNearlyEqual(B.Y, -50.0, 0.001)
			&& FMath::IsNearlyEqual(A.Z, 50.0, 0.001)
			&& FMath::IsNearlyEqual(B.Z, 50.0, 0.001)
			&& FMath::IsNearlyEqual(FMath::Abs(A.X), 50.0, 0.001)
			&& FMath::IsNearlyEqual(FMath::Abs(B.X), 50.0, 0.001)
			&& FMath::IsNearlyEqual(A.X, -B.X, 0.001);
	}

	TSharedRef<FJsonObject> DuplicateFacesParams(
		const FString& TargetRef,
		const TSharedRef<FJsonObject>& Revision)
	{
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target_ref"), TargetRef);
		Params->SetObjectField(
			TEXT("space_contract"),
			BuildTargetLocalSpaceContract());
		Params->SetObjectField(TEXT("selection"), TriangleSelection({2}));
		Params->SetArrayField(
			TEXT("translation_cm"),
			Numbers({0.0, 0.0, 125.0}));
		return Params;
	}

	TSharedRef<FJsonObject> BevelEdgesParams(
		const FString& TargetRef,
		const TSharedRef<FJsonObject>& Revision,
		const int32 Segments,
		const double Roundness)
	{
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target_ref"), TargetRef);
		Params->SetObjectField(
			TEXT("space_contract"),
			BuildTargetLocalSpaceContract());
		Params->SetObjectField(TEXT("selection"), EdgeSelection({5}));
		Params->SetNumberField(TEXT("distance_cm"), 10.0);
		Params->SetNumberField(TEXT("segments"), Segments);
		Params->SetNumberField(TEXT("roundness"), Roundness);
		return Params;
	}

	TSharedRef<FJsonObject> FillHolesParams(
		const FString& TargetRef,
		const TSharedRef<FJsonObject>& Revision,
		const int32 BoundaryEdgeId,
		const FString& FillMode)
	{
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target_ref"), TargetRef);
		Params->SetObjectField(
			TEXT("selection"),
			BoundaryEdgeSelection(BoundaryEdgeId));
		Params->SetStringField(TEXT("fill_mode"), FillMode);
		return Params;
	}

	TSharedRef<FJsonObject> WeldEdgesParams(
		const FString& TargetRef,
		const TSharedRef<FJsonObject>& Revision,
		const int32 EdgeA,
		const int32 EdgeB,
		const double Tolerance)
	{
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target_ref"), TargetRef);
		TArray<TSharedPtr<FJsonValue>> EdgePairs;
		EdgePairs.Add(MakeShared<FJsonValueArray>(Numbers({
			static_cast<double>(EdgeA),
			static_cast<double>(EdgeB)})));
		Params->SetArrayField(TEXT("edge_pairs"), MoveTemp(EdgePairs));
		Params->SetNumberField(TEXT("tolerance_cm"), Tolerance);
		return Params;
	}

	TSharedRef<FJsonObject> TargetLocalDirection(const FVector3d& Vector)
	{
		const TSharedRef<FJsonObject> Direction = MakeShared<FJsonObject>();
		Direction->SetStringField(
			TEXT("frame_id"),
			TEXT("ue.mesh.target_local"));
		Direction->SetArrayField(
			TEXT("vector"),
			Numbers({Vector.X, Vector.Y, Vector.Z}));
		Direction->SetStringField(
			TEXT("basis_evidence"),
			TEXT("exact target-local direction from owned carrier geometry"));
		return Direction;
	}

	TSharedRef<FJsonObject> ExtrudeFacesParams(
		const FString& TargetRef,
		const TSharedRef<FJsonObject>& Revision,
		const FString& DirectionMode)
	{
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target_ref"), TargetRef);
		Params->SetObjectField(
			TEXT("space_contract"),
			BuildTargetLocalSpaceContract());
		Params->SetObjectField(
			TEXT("selection"),
			DirectionMode == TEXT("fixed")
				? TriangleSelection({2, 3})
				: TriangleSelection({2, 3, 6, 7, 8, 9}));
		Params->SetObjectField(
			TEXT("direction"),
			TargetLocalDirection(
				DirectionMode == TEXT("fixed")
					? FVector3d::UnitX()
					: FVector3d::UnitZ()));
		Params->SetNumberField(TEXT("distance_cm"), 25.0);
		Params->SetStringField(TEXT("direction_mode"), DirectionMode);
		return Params;
	}

	TSharedRef<FJsonObject> InsetFacesParams(
		const FString& TargetRef,
		const TSharedRef<FJsonObject>& Revision,
		const FString& BoundaryMode)
	{
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target_ref"), TargetRef);
		Params->SetObjectField(
			TEXT("space_contract"),
			BuildTargetLocalSpaceContract());
		Params->SetObjectField(TEXT("selection"), TriangleSelection({2, 3}));
		Params->SetNumberField(TEXT("distance_cm"), 15.0);
		Params->SetStringField(TEXT("boundary_mode"), BoundaryMode);
		return Params;
	}

	TSharedRef<FJsonObject> OffsetFacesParams(
		const FString& TargetRef,
		const TSharedRef<FJsonObject>& Revision,
		const FString& OffsetMode)
	{
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target_ref"), TargetRef);
		Params->SetObjectField(
			TEXT("space_contract"),
			BuildTargetLocalSpaceContract());
		Params->SetObjectField(TEXT("selection"), TriangleSelection({2, 3}));
		Params->SetNumberField(TEXT("distance_cm"), 20.0);
		Params->SetStringField(TEXT("offset_mode"), OffsetMode);
		return Params;
	}

	TSharedRef<FJsonObject> PushPullFacesParams(
		const FString& TargetRef,
		const TSharedRef<FJsonObject>& Revision,
		const FString& DirectionMode)
	{
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target_ref"), TargetRef);
		Params->SetObjectField(
			TEXT("space_contract"),
			BuildTargetLocalSpaceContract());
		Params->SetObjectField(TEXT("selection"), TriangleSelection({2, 3}));
		Params->SetObjectField(
			TEXT("direction"),
			TargetLocalDirection(
				DirectionMode == TEXT("fixed")
					? FVector3d(0.0, -1.0, 0.0)
					: FVector3d::UnitZ()));
		Params->SetNumberField(TEXT("distance_cm"), 30.0);
		Params->SetStringField(TEXT("direction_mode"), DirectionMode);
		return Params;
	}

	bool RunCurrentDuplicateFacesProductCase(
		FAutomationTestBase& Test,
		const TSharedPtr<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const FUeAgentExactCommandDispatch>& Dispatch,
		UWorld& World)
	{
		FSelectionTopologyObservation Before;
		FSelectionTopologyObservation After;
		const FCarrierStep PrepareCarrier =
			[](UStaticMesh&)
			{
				return true;
			};
		const FBuildParams BuildParams =
			[](const FString& TargetRef,
				const TSharedRef<FJsonObject>& Revision)
			{
				return DuplicateFacesParams(TargetRef, Revision);
			};
		const FCarrierStep VerifyBefore =
			[&Before](UStaticMesh& StaticMesh)
			{
				return ReadSelectionTopology(StaticMesh, Before)
					&& Before.TriangleCount == 12
					&& Before.BoundaryEdgeCount == 0
					&& FMath::IsNearlyEqual(Before.Max.Z, 50.0, 0.001);
			};
		const FCarrierStep VerifyAfter =
			[&Before, &After](UStaticMesh& StaticMesh)
			{
				return ReadSelectionTopology(StaticMesh, After)
					&& After.TriangleCount == Before.TriangleCount + 1
					&& After.VertexCount == Before.VertexCount + 3
					&& After.BoundaryEdgeCount == 3
					&& FMath::IsNearlyEqual(After.Max.Z, 175.0, 0.001)
					&& After.Min.Equals(Before.Min, 0.001);
			};
		return RunCurrentStaticMeshProductCase(
			Test,
			Registry,
			Dispatch,
			World,
			TEXT("mesh.topology.faces.duplicate"),
			TEXT("T50PublicTopologyDuplicateFaces"),
			PrepareCarrier,
			BuildParams,
			VerifyBefore,
			VerifyAfter);
	}

	bool RunCurrentBevelEdgesProductCase(
		FAutomationTestBase& Test,
		const TSharedPtr<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const FUeAgentExactCommandDispatch>& Dispatch,
		UWorld& World,
		const int32 Segments,
		const double Roundness,
		FSelectionTopologyObservation& OutAfter)
	{
		FSelectionTopologyObservation Before;
		const FCarrierStep PrepareCarrier =
			[](UStaticMesh&)
			{
				return true;
			};
		const FBuildParams BuildParams =
			[Segments, Roundness](
				const FString& TargetRef,
				const TSharedRef<FJsonObject>& Revision)
			{
				return BevelEdgesParams(
					TargetRef,
					Revision,
					Segments,
					Roundness);
			};
		const FCarrierStep VerifyBefore =
			[&Before](UStaticMesh& StaticMesh)
			{
				return ReadSelectionTopology(StaticMesh, Before)
					&& Before.TriangleCount == 12
					&& Before.BoundaryEdgeCount == 0
					&& HasExpectedTopFrontEdge(StaticMesh, 5);
			};
		const FCarrierStep VerifyAfter =
			[&Before, &OutAfter, Segments](UStaticMesh& StaticMesh)
			{
				return ReadSelectionTopology(StaticMesh, OutAfter)
					&& OutAfter.TriangleCount
						== Before.TriangleCount + 4 + 4 * Segments
					&& OutAfter.VertexCount
						== Before.VertexCount + 2 + 2 * Segments
					&& OutAfter.BoundaryEdgeCount == 0
					&& OutAfter.Min.Equals(Before.Min, 0.001)
					&& OutAfter.Max.Equals(Before.Max, 0.001)
					&& !FMath::IsNearlyEqual(
						OutAfter.PositionSquaredSum,
						Before.PositionSquaredSum,
						0.001);
			};
		return RunCurrentStaticMeshProductCase(
			Test,
			Registry,
			Dispatch,
			World,
			TEXT("mesh.topology.edges.bevel"),
			FString::Printf(
				TEXT("T50PublicTopologyBevelS%dR%d"),
				Segments,
				FMath::RoundToInt(Roundness * 100.0)),
			PrepareCarrier,
			BuildParams,
			VerifyBefore,
			VerifyAfter);
	}

	bool RunCurrentFillHolesProductCase(
		FAutomationTestBase& Test,
		const TSharedPtr<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const FUeAgentExactCommandDispatch>& Dispatch,
		UWorld& World,
		const FString& FillMode)
	{
		int32 BoundaryEdgeId = INDEX_NONE;
		FSelectionTopologyObservation Before;
		FSelectionTopologyObservation After;
		const FCarrierStep PrepareCarrier =
			[](UStaticMesh& StaticMesh)
			{
				return PrepareOpenTopCube(StaticMesh);
			};
		const FBuildParams BuildParams =
			[&BoundaryEdgeId, FillMode](
				const FString& TargetRef,
				const TSharedRef<FJsonObject>& Revision)
			{
				return FillHolesParams(
					TargetRef,
					Revision,
					BoundaryEdgeId,
					FillMode);
			};
		const FCarrierStep VerifyBefore =
			[&Before, &BoundaryEdgeId](UStaticMesh& StaticMesh)
			{
				return ReadSelectionTopology(StaticMesh, Before)
					&& Before.VertexCount == 8
					&& Before.TriangleCount == 10
					&& Before.BoundaryEdgeCount == 4
					&& FindOpenTopBoundaryEdge(
						StaticMesh,
						BoundaryEdgeId);
			};
		const FCarrierStep VerifyAfter =
			[&Before, &After, FillMode](UStaticMesh& StaticMesh)
			{
				if (!ReadSelectionTopology(StaticMesh, After)
					|| After.BoundaryEdgeCount != 0
					|| !After.Min.Equals(Before.Min, 0.001)
					|| !After.Max.Equals(Before.Max, 0.001))
				{
					return false;
				}
				return FillMode == TEXT("triangle_fan")
					? After.VertexCount == 9
						&& After.EdgeCount == 21
						&& After.TriangleCount == 14
					: After.VertexCount == 8
						&& After.EdgeCount == 18
						&& After.TriangleCount == 12;
			};
		return RunCurrentStaticMeshProductCase(
			Test,
			Registry,
			Dispatch,
			World,
			TEXT("mesh.topology.holes.fill"),
			TEXT("T50PublicTopologyFillHoles") + FillMode,
			PrepareCarrier,
			BuildParams,
			VerifyBefore,
			VerifyAfter);
	}

	bool RunCurrentWeldEdgesProductCase(
		FAutomationTestBase& Test,
		const TSharedPtr<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const FUeAgentExactCommandDispatch>& Dispatch,
		UWorld& World)
	{
		int32 EdgeA = INDEX_NONE;
		int32 EdgeB = INDEX_NONE;
		FSelectionTopologyObservation Before;
		FSelectionTopologyObservation After;
		const FCarrierStep PrepareCarrier =
			[](UStaticMesh& StaticMesh)
			{
				return PrepareWeldPair(StaticMesh);
			};
		const FBuildParams BuildParams =
			[&EdgeA, &EdgeB](
				const FString& TargetRef,
				const TSharedRef<FJsonObject>& Revision)
			{
				return WeldEdgesParams(
					TargetRef,
					Revision,
					EdgeA,
					EdgeB,
					1.0);
			};
		const FCarrierStep VerifyBefore =
			[&Before, &EdgeA, &EdgeB](UStaticMesh& StaticMesh)
			{
				return ReadSelectionTopology(StaticMesh, Before)
					&& Before.VertexCount == 6
					&& Before.EdgeCount == 6
					&& Before.TriangleCount == 2
					&& Before.BoundaryEdgeCount == 6
					&& FindWeldPair(StaticMesh, EdgeA, EdgeB);
			};
		const FCarrierStep VerifyAfter =
			[&After](UStaticMesh& StaticMesh)
			{
				return ReadSelectionTopology(StaticMesh, After)
					&& After.VertexCount == 4
					&& After.EdgeCount == 5
					&& After.TriangleCount == 2
					&& After.BoundaryEdgeCount == 4
					&& FMath::IsNearlyEqual(After.Min.X, 0.0, 0.001)
					&& FMath::IsNearlyEqual(After.Max.X, 100.0, 0.001)
					&& FMath::IsNearlyEqual(After.Min.Y, -100.0, 0.001)
					&& FMath::IsNearlyEqual(After.Max.Y, 100.0, 0.001);
			};
		return RunCurrentStaticMeshProductCase(
			Test,
			Registry,
			Dispatch,
			World,
			TEXT("mesh.topology.edges.weld"),
			TEXT("T50PublicTopologyWeldEdges"),
			PrepareCarrier,
			BuildParams,
			VerifyBefore,
			VerifyAfter);
	}

	bool RunCurrentWeldEdgesRejectedCase(
		FAutomationTestBase& Test,
		const TSharedPtr<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const FUeAgentExactCommandDispatch>& Dispatch,
		UWorld& World)
	{
		int32 EdgeA = INDEX_NONE;
		int32 EdgeB = INDEX_NONE;
		FSelectionTopologyObservation Before;
		FSelectionTopologyObservation After;
		const FCarrierStep PrepareCarrier =
			[](UStaticMesh& StaticMesh)
			{
				return PrepareWeldPair(StaticMesh);
			};
		const FBuildParams BuildParams =
			[&EdgeA, &EdgeB](
				const FString& TargetRef,
				const TSharedRef<FJsonObject>& Revision)
			{
				return WeldEdgesParams(
					TargetRef,
					Revision,
					EdgeA,
					EdgeB,
					0.1);
			};
		const FCarrierStep VerifyBefore =
			[&Before, &EdgeA, &EdgeB](UStaticMesh& StaticMesh)
			{
				return ReadSelectionTopology(StaticMesh, Before)
					&& Before.VertexCount == 6
					&& Before.EdgeCount == 6
					&& Before.TriangleCount == 2
					&& Before.BoundaryEdgeCount == 6
					&& FindWeldPair(StaticMesh, EdgeA, EdgeB);
			};
		const FCarrierStep VerifyUnchanged =
			[&Before, &After](UStaticMesh& StaticMesh)
			{
				return ReadSelectionTopology(StaticMesh, After)
					&& After == Before;
			};
		return RunCurrentStaticMeshRejectedCase(
			Test,
			Registry,
			Dispatch,
			World,
			TEXT("mesh.topology.edges.weld"),
			TEXT("T50PublicTopologyWeldToleranceRejected"),
			TEXT("modeling_geometry_weld_tolerance_exceeded"),
			PrepareCarrier,
			BuildParams,
			VerifyBefore,
			VerifyUnchanged);
	}

	bool RunCurrentExtrudeFacesProductCase(
		FAutomationTestBase& Test,
		const TSharedPtr<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const FUeAgentExactCommandDispatch>& Dispatch,
		UWorld& World,
		const FString& DirectionMode)
	{
		FSelectionTopologyObservation Before;
		FSelectionTopologyObservation After;
		const FCarrierStep PrepareCarrier =
			[](UStaticMesh&)
			{
				return true;
			};
		const FBuildParams BuildParams =
			[DirectionMode](
				const FString& TargetRef,
				const TSharedRef<FJsonObject>& Revision)
			{
				return ExtrudeFacesParams(
					TargetRef,
					Revision,
					DirectionMode);
			};
		const FCarrierStep VerifyBefore =
			[&Before](UStaticMesh& StaticMesh)
			{
				return ReadSelectionTopology(StaticMesh, Before)
					&& Before.TriangleCount == 12
					&& Before.BoundaryEdgeCount == 0;
			};
		const FCarrierStep VerifyAfter =
			[&Before, &After, DirectionMode](UStaticMesh& StaticMesh)
			{
				if (!ReadSelectionTopology(StaticMesh, After)
					|| After.TriangleCount < Before.TriangleCount + 8
					|| After.VertexCount <= Before.VertexCount
					|| After.BoundaryEdgeCount != 0)
				{
					return false;
				}
				return DirectionMode == TEXT("fixed")
					? After.Max.X >= Before.Max.X + 24.999
						&& FMath::IsNearlyEqual(
							After.Max.Z,
							Before.Max.Z,
							0.001)
					: After.Max.X > Before.Max.X + 0.001
						&& After.Max.Y > Before.Max.Y + 0.001
						&& After.Max.Z > Before.Max.Z + 0.001;
			};
		return RunCurrentStaticMeshProductCase(
			Test,
			Registry,
			Dispatch,
			World,
			TEXT("mesh.topology.faces.extrude"),
			DirectionMode == TEXT("fixed")
				? TEXT("T50PublicTopologyExtrudeFixed")
				: TEXT("T50PublicTopologyExtrudeVertexNormals"),
			PrepareCarrier,
			BuildParams,
			VerifyBefore,
			VerifyAfter);
	}

	bool RunCurrentInsetFacesProductCase(
		FAutomationTestBase& Test,
		const TSharedPtr<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const FUeAgentExactCommandDispatch>& Dispatch,
		UWorld& World,
		const FString& BoundaryMode)
	{
		FSelectionTopologyObservation Before;
		FSelectionTopologyObservation After;
		const FCarrierStep PrepareCarrier =
			[](UStaticMesh&)
			{
				return true;
			};
		const FBuildParams BuildParams =
			[BoundaryMode](
				const FString& TargetRef,
				const TSharedRef<FJsonObject>& Revision)
			{
				return InsetFacesParams(TargetRef, Revision, BoundaryMode);
			};
		const FCarrierStep VerifyBefore =
			[&Before](UStaticMesh& StaticMesh)
			{
				return ReadSelectionTopology(StaticMesh, Before)
					&& Before.TriangleCount == 12;
			};
		const FCarrierStep VerifyAfter =
			[&Before, &After](UStaticMesh& StaticMesh)
			{
				return ReadSelectionTopology(StaticMesh, After)
					&& After.TriangleCount > Before.TriangleCount
					&& After.VertexCount > Before.VertexCount
					&& After.BoundaryEdgeCount == 0
					&& After.Min.Equals(Before.Min, 0.001)
					&& After.Max.Equals(Before.Max, 0.001);
			};
		return RunCurrentStaticMeshProductCase(
			Test,
			Registry,
			Dispatch,
			World,
			TEXT("mesh.topology.faces.inset"),
			BoundaryMode == TEXT("reprojected")
				? TEXT("T50PublicTopologyInsetReprojected")
				: TEXT("T50PublicTopologyInsetUniform"),
			PrepareCarrier,
			BuildParams,
			VerifyBefore,
			VerifyAfter);
	}

	bool RunCurrentOffsetFacesProductCase(
		FAutomationTestBase& Test,
		const TSharedPtr<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const FUeAgentExactCommandDispatch>& Dispatch,
		UWorld& World,
		const FString& OffsetMode)
	{
		FSelectionTopologyObservation Before;
		FSelectionTopologyObservation After;
		const FCarrierStep PrepareCarrier =
			[](UStaticMesh&)
			{
				return true;
			};
		const FBuildParams BuildParams =
			[OffsetMode](
				const FString& TargetRef,
				const TSharedRef<FJsonObject>& Revision)
			{
				return OffsetFacesParams(TargetRef, Revision, OffsetMode);
			};
		const FCarrierStep VerifyBefore =
			[&Before](UStaticMesh& StaticMesh)
			{
				return ReadSelectionTopology(StaticMesh, Before)
					&& Before.TriangleCount == 12;
			};
		const FCarrierStep VerifyAfter =
			[&Before, &After, OffsetMode](UStaticMesh& StaticMesh)
			{
				if (!ReadSelectionTopology(StaticMesh, After)
					|| After.TriangleCount < Before.TriangleCount + 8
					|| After.VertexCount <= Before.VertexCount
					|| After.BoundaryEdgeCount != 0)
				{
					return false;
				}
				if (OffsetMode == TEXT("face_normals"))
				{
					return FMath::IsNearlyEqual(
							After.Max.Z,
							Before.Max.Z + 20.0,
							0.001)
						&& FMath::IsNearlyEqual(After.Max.X, Before.Max.X, 0.001)
						&& FMath::IsNearlyEqual(After.Max.Y, Before.Max.Y, 0.001)
						&& After.Min.Equals(Before.Min, 0.001);
				}
				const double AxisAdvance = 20.0 / FMath::Sqrt(3.0);
				return FMath::IsNearlyEqual(
						After.Max.X,
						Before.Max.X + AxisAdvance,
						0.002)
					&& FMath::IsNearlyEqual(
						After.Max.Y,
						Before.Max.Y + AxisAdvance,
						0.002)
					&& FMath::IsNearlyEqual(
						After.Max.Z,
						Before.Max.Z + AxisAdvance,
						0.002)
					&& FMath::IsNearlyEqual(
						After.Min.X,
						Before.Min.X - AxisAdvance,
						0.002)
					&& FMath::IsNearlyEqual(
						After.Min.Y,
						Before.Min.Y - AxisAdvance,
						0.002)
					&& FMath::IsNearlyEqual(After.Min.Z, Before.Min.Z, 0.001);
			};
		return RunCurrentStaticMeshProductCase(
			Test,
			Registry,
			Dispatch,
			World,
			TEXT("mesh.topology.faces.offset"),
			OffsetMode == TEXT("face_normals")
				? TEXT("T50PublicTopologyOffsetFaceNormals")
				: TEXT("T50PublicTopologyOffsetVertexNormals"),
			PrepareCarrier,
			BuildParams,
			VerifyBefore,
			VerifyAfter);
	}

	bool RunCurrentPushPullFacesProductCase(
		FAutomationTestBase& Test,
		const TSharedPtr<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const FUeAgentExactCommandDispatch>& Dispatch,
		UWorld& World,
		const FString& DirectionMode)
	{
		FSelectionTopologyObservation Before;
		FSelectionTopologyObservation After;
		const FCarrierStep PrepareCarrier =
			[](UStaticMesh&)
			{
				return true;
			};
		const FBuildParams BuildParams =
			[DirectionMode](
				const FString& TargetRef,
				const TSharedRef<FJsonObject>& Revision)
			{
				return PushPullFacesParams(
					TargetRef,
					Revision,
					DirectionMode);
			};
		const FCarrierStep VerifyBefore =
			[&Before](UStaticMesh& StaticMesh)
			{
				return ReadSelectionTopology(StaticMesh, Before)
					&& Before.TriangleCount == 12;
			};
		const FCarrierStep VerifyAfter =
			[&Before, &After, DirectionMode](UStaticMesh& StaticMesh)
			{
				if (!ReadSelectionTopology(StaticMesh, After)
					|| After.TriangleCount < Before.TriangleCount + 8
					|| After.VertexCount <= Before.VertexCount
					|| After.BoundaryEdgeCount != 0)
				{
					return false;
				}
				return DirectionMode == TEXT("fixed")
					? After.Min.Y <= Before.Min.Y - 29.999
						&& FMath::IsNearlyEqual(After.Max.Z, Before.Max.Z, 0.001)
					: After.Max.Z >= Before.Max.Z + 29.999;
			};
		return RunCurrentStaticMeshProductCase(
			Test,
			Registry,
			Dispatch,
			World,
			TEXT("mesh.topology.faces.push_pull"),
			DirectionMode == TEXT("fixed")
				? TEXT("T50PublicTopologyPushPullFixed")
				: TEXT("T50PublicTopologyPushPullFaceNormals"),
			PrepareCarrier,
			BuildParams,
			VerifyBefore,
			VerifyAfter);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentModelingGeometryPublicStaticMeshWeldEdgesTest,
	"UeAgentInterface.DomainAdapters.ModelingGeometryEffect.E4.PublicProduct."
		"StaticMeshTopologyWeldEdges",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentModelingGeometryPublicStaticMeshWeldEdgesTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Tests::ModelingGeometryPublicStaticMesh;
	using namespace
		UE::UAI::Tests::ModelingGeometryPublicStaticMeshSelectionTopology;
	(void)Parameters;
	return RunWithPublicStaticMeshEnvironment(
		*this,
		[this](const auto& Registry, const auto& Dispatch, UWorld& World)
		{
			return RunCurrentWeldEdgesProductCase(
				*this,
				Registry,
				Dispatch,
				World);
		});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentModelingGeometryPublicStaticMeshWeldToleranceRejectedTest,
	"UeAgentInterface.DomainAdapters.ModelingGeometryEffect.E4.PublicProduct."
		"StaticMeshTopologyWeldToleranceRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentModelingGeometryPublicStaticMeshWeldToleranceRejectedTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Tests::ModelingGeometryPublicStaticMesh;
	using namespace
		UE::UAI::Tests::ModelingGeometryPublicStaticMeshSelectionTopology;
	(void)Parameters;
	return RunWithPublicStaticMeshEnvironment(
		*this,
		[this](const auto& Registry, const auto& Dispatch, UWorld& World)
		{
			return RunCurrentWeldEdgesRejectedCase(
				*this,
				Registry,
				Dispatch,
				World);
		});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentModelingGeometryPublicStaticMeshFillHolesTest,
	"UeAgentInterface.DomainAdapters.ModelingGeometryEffect.E4.PublicProduct."
		"StaticMeshTopologyFillHoles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentModelingGeometryPublicStaticMeshFillHolesTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Tests::ModelingGeometryPublicStaticMesh;
	using namespace
		UE::UAI::Tests::ModelingGeometryPublicStaticMeshSelectionTopology;
	(void)Parameters;
	return RunWithPublicStaticMeshEnvironment(
		*this,
		[this](const auto& Registry, const auto& Dispatch, UWorld& World)
		{
			const bool bMinimal = RunCurrentFillHolesProductCase(
				*this,
				Registry,
				Dispatch,
				World,
				TEXT("minimal"));
			const bool bTriangleFan = RunCurrentFillHolesProductCase(
				*this,
				Registry,
				Dispatch,
				World,
				TEXT("triangle_fan"));
			const bool bPlanar = RunCurrentFillHolesProductCase(
				*this,
				Registry,
				Dispatch,
				World,
				TEXT("planar"));
			return bMinimal && bTriangleFan && bPlanar;
		});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentModelingGeometryPublicStaticMeshBevelEdgesTest,
	"UeAgentInterface.DomainAdapters.ModelingGeometryEffect.E4.PublicProduct."
		"StaticMeshTopologyBevelEdges",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentModelingGeometryPublicStaticMeshBevelEdgesTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Tests::ModelingGeometryPublicStaticMesh;
	using namespace
		UE::UAI::Tests::ModelingGeometryPublicStaticMeshSelectionTopology;
	(void)Parameters;
	return RunWithPublicStaticMeshEnvironment(
		*this,
		[this](const auto& Registry, const auto& Dispatch, UWorld& World)
		{
			FSelectionTopologyObservation Flat;
			FSelectionTopologyObservation Segmented;
			FSelectionTopologyObservation Rounded;
			const bool bFlat = RunCurrentBevelEdgesProductCase(
				*this,
				Registry,
				Dispatch,
				World,
				0,
				0.0,
				Flat);
			const bool bSegmented = RunCurrentBevelEdgesProductCase(
				*this,
				Registry,
				Dispatch,
				World,
				2,
				0.0,
				Segmented);
			const bool bRounded = RunCurrentBevelEdgesProductCase(
				*this,
				Registry,
				Dispatch,
				World,
				2,
				1.0,
				Rounded);
			if (!(bFlat && bSegmented && bRounded))
			{
				return false;
			}
			return TestTrue(
				TEXT("segments increase persisted bevel topology"),
				Segmented.VertexCount > Flat.VertexCount
					&& Segmented.TriangleCount > Flat.TriangleCount)
				&& TestEqual(
					TEXT("roundness preserves bevel vertex count"),
					Rounded.VertexCount,
					Segmented.VertexCount)
				&& TestEqual(
					TEXT("roundness preserves bevel triangle count"),
					Rounded.TriangleCount,
					Segmented.TriangleCount)
				&& TestTrue(
					TEXT("roundness changes persisted bevel geometry"),
					!FMath::IsNearlyEqual(
						Rounded.PositionSquaredSum,
						Segmented.PositionSquaredSum,
						0.001));
		});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentModelingGeometryPublicStaticMeshDuplicateFacesTest,
	"UeAgentInterface.DomainAdapters.ModelingGeometryEffect.E4.PublicProduct."
		"StaticMeshTopologyDuplicateFaces",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentModelingGeometryPublicStaticMeshDuplicateFacesTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Tests::ModelingGeometryPublicStaticMesh;
	using namespace
		UE::UAI::Tests::ModelingGeometryPublicStaticMeshSelectionTopology;
	(void)Parameters;
	return RunWithPublicStaticMeshEnvironment(
		*this,
		[this](const auto& Registry, const auto& Dispatch, UWorld& World)
		{
			return RunCurrentDuplicateFacesProductCase(
				*this,
				Registry,
				Dispatch,
				World);
		});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentModelingGeometryPublicStaticMeshExtrudeFacesTest,
	"UeAgentInterface.DomainAdapters.ModelingGeometryEffect.E4.PublicProduct."
		"StaticMeshTopologyExtrudeFaces",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentModelingGeometryPublicStaticMeshExtrudeFacesTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Tests::ModelingGeometryPublicStaticMesh;
	using namespace
		UE::UAI::Tests::ModelingGeometryPublicStaticMeshSelectionTopology;
	(void)Parameters;
	return RunWithPublicStaticMeshEnvironment(
		*this,
		[this](const auto& Registry, const auto& Dispatch, UWorld& World)
		{
			const bool bFixed = RunCurrentExtrudeFacesProductCase(
				*this,
				Registry,
				Dispatch,
				World,
				TEXT("fixed"));
			const bool bVertexNormals = RunCurrentExtrudeFacesProductCase(
				*this,
				Registry,
				Dispatch,
				World,
				TEXT("vertex_normals"));
			return bFixed && bVertexNormals;
		});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentModelingGeometryPublicStaticMeshInsetFacesTest,
	"UeAgentInterface.DomainAdapters.ModelingGeometryEffect.E4.PublicProduct."
		"StaticMeshTopologyInsetFaces",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentModelingGeometryPublicStaticMeshInsetFacesTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Tests::ModelingGeometryPublicStaticMesh;
	using namespace
		UE::UAI::Tests::ModelingGeometryPublicStaticMeshSelectionTopology;
	(void)Parameters;
	return RunWithPublicStaticMeshEnvironment(
		*this,
		[this](const auto& Registry, const auto& Dispatch, UWorld& World)
		{
			const bool bUniform = RunCurrentInsetFacesProductCase(
				*this,
				Registry,
				Dispatch,
				World,
				TEXT("uniform"));
			const bool bReprojected = RunCurrentInsetFacesProductCase(
				*this,
				Registry,
				Dispatch,
				World,
				TEXT("reprojected"));
			return bUniform && bReprojected;
		});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentModelingGeometryPublicStaticMeshOffsetFacesTest,
	"UeAgentInterface.DomainAdapters.ModelingGeometryEffect.E4.PublicProduct."
		"StaticMeshTopologyOffsetFaces",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentModelingGeometryPublicStaticMeshOffsetFacesTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Tests::ModelingGeometryPublicStaticMesh;
	using namespace
		UE::UAI::Tests::ModelingGeometryPublicStaticMeshSelectionTopology;
	(void)Parameters;
	return RunWithPublicStaticMeshEnvironment(
		*this,
		[this](const auto& Registry, const auto& Dispatch, UWorld& World)
		{
			const bool bFaceNormals = RunCurrentOffsetFacesProductCase(
				*this,
				Registry,
				Dispatch,
				World,
				TEXT("face_normals"));
			const bool bVertexNormals = RunCurrentOffsetFacesProductCase(
				*this,
				Registry,
				Dispatch,
				World,
				TEXT("vertex_normals"));
			return bFaceNormals && bVertexNormals;
		});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentModelingGeometryPublicStaticMeshPushPullFacesTest,
	"UeAgentInterface.DomainAdapters.ModelingGeometryEffect.E4.PublicProduct."
		"StaticMeshTopologyPushPullFaces",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentModelingGeometryPublicStaticMeshPushPullFacesTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Tests::ModelingGeometryPublicStaticMesh;
	using namespace
		UE::UAI::Tests::ModelingGeometryPublicStaticMeshSelectionTopology;
	(void)Parameters;
	return RunWithPublicStaticMeshEnvironment(
		*this,
		[this](const auto& Registry, const auto& Dispatch, UWorld& World)
		{
			const bool bFaceNormals = RunCurrentPushPullFacesProductCase(
				*this,
				Registry,
				Dispatch,
				World,
				TEXT("face_normals"));
			const bool bFixed = RunCurrentPushPullFacesProductCase(
				*this,
				Registry,
				Dispatch,
				World,
				TEXT("fixed"));
			return bFaceNormals && bFixed;
		});
}

#endif

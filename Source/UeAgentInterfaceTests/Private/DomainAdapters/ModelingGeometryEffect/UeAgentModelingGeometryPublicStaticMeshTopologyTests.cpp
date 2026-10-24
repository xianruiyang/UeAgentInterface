#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryPublicStaticMeshTestSupport.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/MeshNormals.h"
#include "DynamicMeshToMeshDescription.h"
#include "Engine/StaticMesh.h"
#include "MeshDescriptionToDynamicMesh.h"
#include "StaticMeshAttributes.h"

namespace UE::UAI::Tests::ModelingGeometryPublicStaticMeshTopology
{
	using namespace UE::Geometry;
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::CommandRegistry;
	using namespace UE::UAI::Tests::ModelingGeometryPublicStaticMesh;

	struct FTopologyObservation final
	{
		int32 VertexCount = 0;
		int32 EdgeCount = 0;
		int32 BoundaryEdgeCount = 0;
		int32 TriangleCount = 0;
		int32 NormalSeamEdgeCount = 0;
		double MeanEdgeLength = 0.0;
		FVector3d Min = FVector3d::Zero();
		FVector3d Max = FVector3d::Zero();
		TArray<FVector3d> VertexPositions;
		TArray<FVector3d> BoundaryPositions;
		bool bBoundsValid = false;
	};

	bool PositionLess(const FVector3d& A, const FVector3d& B)
	{
		return A.X != B.X
			? A.X < B.X
			: A.Y != B.Y
				? A.Y < B.Y
				: A.Z < B.Z;
	}

	bool SamePositions(
		const TArray<FVector3d>& A,
		const TArray<FVector3d>& B,
		const double Tolerance)
	{
		if (A.Num() != B.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < A.Num(); ++Index)
		{
			if (!A[Index].Equals(B[Index], Tolerance))
			{
				return false;
			}
		}
		return true;
	}

	bool ContainsPositions(
		const TArray<FVector3d>& Haystack,
		const TArray<FVector3d>& Needles,
		const double Tolerance)
	{
		for (const FVector3d& Needle : Needles)
		{
			if (!Haystack.ContainsByPredicate(
					[&Needle, Tolerance](const FVector3d& Candidate)
					{
						return Candidate.Equals(Needle, Tolerance);
					}))
			{
				return false;
			}
		}
		return true;
	}

	bool ReadTopology(UStaticMesh& StaticMesh, FTopologyObservation& Out)
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
		const FDynamicMeshNormalOverlay* const NormalOverlay =
			Mesh.HasAttributes()
				? Mesh.Attributes()->PrimaryNormals()
				: nullptr;
		double TotalEdgeLength = 0.0;
		for (const int32 EdgeId : Mesh.EdgeIndicesItr())
		{
			const FIndex2i EdgeVertices = Mesh.GetEdgeV(EdgeId);
			TotalEdgeLength += FVector3d::Distance(
				Mesh.GetVertex(EdgeVertices.A),
				Mesh.GetVertex(EdgeVertices.B));
			if (NormalOverlay && NormalOverlay->IsSeamEdge(EdgeId))
			{
				++Out.NormalSeamEdgeCount;
			}
			if (Mesh.IsBoundaryEdge(EdgeId))
			{
				++Out.BoundaryEdgeCount;
			}
		}
		Out.MeanEdgeLength = Out.EdgeCount > 0
			? TotalEdgeLength / Out.EdgeCount
			: 0.0;
		for (const int32 VertexId : Mesh.VertexIndicesItr())
		{
			Out.VertexPositions.Add(Mesh.GetVertex(VertexId));
			if (Mesh.IsBoundaryVertex(VertexId))
			{
				Out.BoundaryPositions.Add(Mesh.GetVertex(VertexId));
			}
		}
		Out.VertexPositions.Sort(PositionLess);
		Out.BoundaryPositions.Sort(PositionLess);
		Out.bBoundsValid = Bounds.IsEmpty() == false;
		return Out.VertexCount > 0
			&& Out.EdgeCount > 0
			&& Out.TriangleCount > 0
			&& Out.bBoundsValid;
	}

	TSharedRef<FJsonObject> SubdivideParams(
		const FString& TargetRef,
		const TSharedRef<FJsonObject>& Revision,
		const FString& Scheme)
	{
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target_ref"), TargetRef);
		Params->SetStringField(TEXT("scheme"), Scheme);
		Params->SetNumberField(TEXT("levels"), 1);
		return Params;
	}

	bool CommitPreparedMesh(UStaticMesh& StaticMesh, FDynamicMesh3& Mesh)
	{
		FMeshDescription Description;
		FStaticMeshAttributes Attributes(Description);
		Attributes.Register();
		FDynamicMeshToMeshDescription Converter;
		Converter.Convert(&Mesh, Description);
		StaticMesh.Modify();
		StaticMesh.CreateMeshDescription(0, Description);
		StaticMesh.CommitMeshDescription(0);
		return true;
	}

	bool PrepareOpenGrid(UStaticMesh& StaticMesh)
	{
		constexpr int32 SideVertexCount = 5;
		constexpr double Spacing = 25.0;
		FDynamicMesh3 Mesh;
		Mesh.EnableTriangleGroups();
		int32 Vertices[SideVertexCount][SideVertexCount];
		for (int32 Y = 0; Y < SideVertexCount; ++Y)
		{
			for (int32 X = 0; X < SideVertexCount; ++X)
			{
				Vertices[Y][X] = Mesh.AppendVertex(FVector3d(
					(X - 2) * Spacing,
					(Y - 2) * Spacing,
					0.0));
			}
		}
		for (int32 Y = 0; Y < SideVertexCount - 1; ++Y)
		{
			for (int32 X = 0; X < SideVertexCount - 1; ++X)
			{
				const int32 V00 = Vertices[Y][X];
				const int32 V10 = Vertices[Y][X + 1];
				const int32 V01 = Vertices[Y + 1][X];
				const int32 V11 = Vertices[Y + 1][X + 1];
				Mesh.AppendTriangle(V00, V10, V11, 0);
				Mesh.AppendTriangle(V00, V11, V01, 0);
			}
		}
		return CommitPreparedMesh(StaticMesh, Mesh);
	}

	bool PrepareHardCube(UStaticMesh& StaticMesh)
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
		if (!Mesh.HasAttributes())
		{
			Mesh.EnableAttributes();
		}
		FMeshNormals::InitializeOverlayToPerTriangleNormals(
			Mesh.Attributes()->PrimaryNormals());
		return CommitPreparedMesh(StaticMesh, Mesh);
	}

	bool PrepareOffsetCube(UStaticMesh& StaticMesh)
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
		for (const int32 VertexId : Mesh.VertexIndicesItr())
		{
			Mesh.SetVertex(
				VertexId,
				Mesh.GetVertex(VertexId) + FVector3d(100.0, 0.0, 0.0));
		}
		return CommitPreparedMesh(StaticMesh, Mesh);
	}

	bool PrepareOpenHalfBox(UStaticMesh& StaticMesh)
	{
		FDynamicMesh3 Mesh;
		Mesh.EnableTriangleGroups();
		const int32 V0 = Mesh.AppendVertex(FVector3d(0.0, -50.0, -50.0));
		const int32 V1 = Mesh.AppendVertex(FVector3d(0.0, 50.0, -50.0));
		const int32 V2 = Mesh.AppendVertex(FVector3d(0.0, 50.0, 50.0));
		const int32 V3 = Mesh.AppendVertex(FVector3d(0.0, -50.0, 50.0));
		const int32 V4 = Mesh.AppendVertex(FVector3d(100.0, -50.0, -50.0));
		const int32 V5 = Mesh.AppendVertex(FVector3d(100.0, 50.0, -50.0));
		const int32 V6 = Mesh.AppendVertex(FVector3d(100.0, 50.0, 50.0));
		const int32 V7 = Mesh.AppendVertex(FVector3d(100.0, -50.0, 50.0));
		const FIndex3i Triangles[] = {
			{V4, V5, V6}, {V4, V6, V7},
			{V0, V4, V7}, {V0, V7, V3},
			{V1, V2, V6}, {V1, V6, V5},
			{V0, V1, V5}, {V0, V5, V4},
			{V3, V7, V6}, {V3, V6, V2},
		};
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(Triangles); ++Index)
		{
			Mesh.AppendTriangle(Triangles[Index], Index / 2);
		}
		Mesh.EnableAttributes();
		return CommitPreparedMesh(StaticMesh, Mesh);
	}

	TSharedRef<FJsonObject> SimplifyParams(
		const FString& TargetRef,
		const TSharedRef<FJsonObject>& Revision,
		const bool bPreserveBoundaries,
		const FString& TargetMode = TEXT("triangle_count"),
		const double TargetValue = 4.0)
	{
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target_ref"), TargetRef);
		Params->SetStringField(TEXT("target_mode"), TargetMode);
		Params->SetNumberField(TEXT("target_value"), TargetValue);
		Params->SetBoolField(TEXT("preserve_boundaries"), bPreserveBoundaries);
		return Params;
	}

	TSharedRef<FJsonObject> RemeshParams(
		const FString& TargetRef,
		const TSharedRef<FJsonObject>& Revision,
		const double TargetEdgeLength,
		const int32 Iterations,
		const bool bPreserveBoundaries,
		const bool bPreserveSharpEdges)
	{
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target_ref"), TargetRef);
		Params->SetObjectField(
			TEXT("space_contract"),
			BuildTargetLocalSpaceContract());
		Params->SetNumberField(
			TEXT("target_edge_length_cm"),
			TargetEdgeLength);
		Params->SetNumberField(TEXT("iterations"), Iterations);
		Params->SetBoolField(
			TEXT("preserve_boundaries"),
			bPreserveBoundaries);
		Params->SetBoolField(
			TEXT("preserve_sharp_edges"),
			bPreserveSharpEdges);
		return Params;
	}

	TSharedRef<FJsonObject> MirrorParams(
		const FString& TargetRef,
		const TSharedRef<FJsonObject>& Revision,
		const bool bKeepOriginal,
		const bool bWeldSeam)
	{
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target_ref"), TargetRef);
		Params->SetObjectField(
			TEXT("space_contract"),
			BuildTargetLocalSpaceContract());
		Params->SetObjectField(
			TEXT("plane"),
			BuildTargetLocalPlane(
				FVector3d::Zero(),
				FVector3d::UnitX()));
		Params->SetBoolField(TEXT("keep_original"), bKeepOriginal);
		Params->SetBoolField(TEXT("weld_seam"), bWeldSeam);
		return Params;
	}

	TSharedRef<FJsonObject> PlaneCutParams(
		const FString& TargetRef,
		const TSharedRef<FJsonObject>& Revision,
		const FString& KeepSide,
		const bool bFillHole)
	{
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target_ref"), TargetRef);
		Params->SetObjectField(
			TEXT("space_contract"),
			BuildTargetLocalSpaceContract());
		Params->SetObjectField(
			TEXT("plane"),
			BuildTargetLocalPlane(
				FVector3d::Zero(),
				FVector3d::UnitX()));
		Params->SetStringField(TEXT("keep_side"), KeepSide);
		Params->SetBoolField(TEXT("fill_hole"), bFillHole);
		return Params;
	}

	bool RunCurrentSubdivideProductCase(
		FAutomationTestBase& Test,
		const TSharedPtr<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const FUeAgentExactCommandDispatch>& Dispatch,
		UWorld& World,
		const FString& Scheme)
	{
		FTopologyObservation Before;
		FTopologyObservation After;
		const FCarrierStep PrepareCarrier =
			[Scheme](UStaticMesh& StaticMesh)
			{
				return Scheme == TEXT("pn_triangles")
					? PrepareHardCube(StaticMesh)
					: true;
			};
		const FBuildParams BuildParams =
			[Scheme](const FString& TargetRef,
				const TSharedRef<FJsonObject>& Revision)
			{
				return SubdivideParams(TargetRef, Revision, Scheme);
			};
		const FCarrierStep VerifyBefore =
			[&Before](UStaticMesh& StaticMesh)
			{
				return ReadTopology(StaticMesh, Before)
					&& Before.TriangleCount == 12;
			};
		const FCarrierStep VerifyAfter =
			[&Before, &After](UStaticMesh& StaticMesh)
			{
				return ReadTopology(StaticMesh, After)
					&& After.TriangleCount == Before.TriangleCount * 4
					&& After.VertexCount > Before.VertexCount
					&& After.EdgeCount > Before.EdgeCount
					&& After.Min.Equals(Before.Min, 0.001)
					&& After.Max.Equals(Before.Max, 0.001);
			};
		return RunCurrentStaticMeshProductCase(
			Test,
			Registry,
			Dispatch,
			World,
			TEXT("mesh.topology.subdivide"),
			Scheme == TEXT("pn_triangles")
				? TEXT("T50PublicTopologySubdividePnTriangles")
				: TEXT("T50PublicTopologySubdivideUniform"),
			PrepareCarrier,
			BuildParams,
			VerifyBefore,
			VerifyAfter);
	}

	bool RunCurrentSimplifyProductCase(
		FAutomationTestBase& Test,
		const TSharedPtr<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const FUeAgentExactCommandDispatch>& Dispatch,
		UWorld& World,
		const bool bPreserveBoundaries)
	{
		FTopologyObservation Before;
		FTopologyObservation After;
		const FCarrierStep PrepareCarrier =
			[](UStaticMesh& StaticMesh)
			{
				return PrepareOpenGrid(StaticMesh);
			};
		const FBuildParams BuildParams =
			[bPreserveBoundaries](
				const FString& TargetRef,
				const TSharedRef<FJsonObject>& Revision)
			{
				return SimplifyParams(
					TargetRef,
					Revision,
					bPreserveBoundaries);
			};
		const FCarrierStep VerifyBefore =
			[&Before](UStaticMesh& StaticMesh)
			{
				return ReadTopology(StaticMesh, Before)
					&& Before.TriangleCount == 32
					&& Before.BoundaryPositions.Num() == 16;
			};
		const FCarrierStep VerifyAfter =
			[&Before, &After, bPreserveBoundaries](UStaticMesh& StaticMesh)
			{
				if (!ReadTopology(StaticMesh, After)
					|| After.TriangleCount >= Before.TriangleCount)
				{
					return false;
				}
				const bool bSameBoundary = SamePositions(
					Before.BoundaryPositions,
					After.BoundaryPositions,
					0.001);
				return bPreserveBoundaries
					? bSameBoundary
					: !bSameBoundary && After.TriangleCount <= 5;
			};
		return RunCurrentStaticMeshProductCase(
			Test,
			Registry,
			Dispatch,
			World,
			TEXT("mesh.topology.simplify"),
			bPreserveBoundaries
				? TEXT("T50PublicTopologySimplifyPreserve")
				: TEXT("T50PublicTopologySimplifyFree"),
			PrepareCarrier,
			BuildParams,
			VerifyBefore,
			VerifyAfter);
	}

	bool RunCurrentSimplifyTargetModeCase(
		FAutomationTestBase& Test,
		const TSharedPtr<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const FUeAgentExactCommandDispatch>& Dispatch,
		UWorld& World,
		const FString& TargetMode,
		const double TargetValue)
	{
		FTopologyObservation Before;
		FTopologyObservation After;
		const bool bTolerance = TargetMode == TEXT("tolerance_cm");
		const FCarrierStep PrepareCarrier =
			[bTolerance](UStaticMesh& StaticMesh)
			{
				return bTolerance ? PrepareOpenGrid(StaticMesh) : true;
			};
		const FBuildParams BuildParams =
			[TargetMode, TargetValue](
				const FString& TargetRef,
				const TSharedRef<FJsonObject>& Revision)
			{
				return SimplifyParams(
					TargetRef,
					Revision,
					false,
					TargetMode,
					TargetValue);
			};
		const FCarrierStep VerifyBefore =
			[&Before, bTolerance](UStaticMesh& StaticMesh)
			{
				return ReadTopology(StaticMesh, Before)
					&& Before.TriangleCount == (bTolerance ? 32 : 12);
			};
		const FCarrierStep VerifyAfter =
			[&Before, &After, bTolerance, TargetValue](
				UStaticMesh& StaticMesh)
			{
				if (!ReadTopology(StaticMesh, After)
					|| After.TriangleCount >= Before.TriangleCount)
				{
					return false;
				}
				return bTolerance
					? FMath::Abs(After.Min.Z) <= TargetValue + 0.001
						&& FMath::Abs(After.Max.Z) <= TargetValue + 0.001
					: After.TriangleCount
						<= FMath::CeilToInt(
							Before.TriangleCount * TargetValue)
								+ 1;
			};
		return RunCurrentStaticMeshProductCase(
			Test,
			Registry,
			Dispatch,
			World,
			TEXT("mesh.topology.simplify"),
			bTolerance
				? TEXT("T50PublicTopologySimplifyTolerance")
				: TEXT("T50PublicTopologySimplifyRatio"),
			PrepareCarrier,
			BuildParams,
			VerifyBefore,
			VerifyAfter);
	}

	bool RunCurrentRemeshDensityProductCase(
		FAutomationTestBase& Test,
		const TSharedPtr<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const FUeAgentExactCommandDispatch>& Dispatch,
		UWorld& World)
	{
		FTopologyObservation Before;
		FTopologyObservation After;
		const FCarrierStep PrepareCarrier =
			[](UStaticMesh&)
			{
				return true;
			};
		const FBuildParams BuildParams =
			[](const FString& TargetRef,
				const TSharedRef<FJsonObject>& Revision)
			{
				return RemeshParams(
					TargetRef,
					Revision,
					25.0,
					10,
					true,
					true);
			};
		const FCarrierStep VerifyBefore =
			[&Before](UStaticMesh& StaticMesh)
			{
				return ReadTopology(StaticMesh, Before)
					&& Before.TriangleCount == 12;
			};
		const FCarrierStep VerifyAfter =
			[&Before, &After](UStaticMesh& StaticMesh)
			{
				return ReadTopology(StaticMesh, After)
					&& After.TriangleCount > Before.TriangleCount
					&& After.MeanEdgeLength < Before.MeanEdgeLength
					&& After.Min.Equals(Before.Min, 0.001)
					&& After.Max.Equals(Before.Max, 0.001);
			};
		return RunCurrentStaticMeshProductCase(
			Test,
			Registry,
			Dispatch,
			World,
			TEXT("mesh.topology.remesh"),
			TEXT("T50PublicTopologyRemeshDensity"),
			PrepareCarrier,
			BuildParams,
			VerifyBefore,
			VerifyAfter);
	}

	bool RunCurrentMirrorReplaceProductCase(
		FAutomationTestBase& Test,
		const TSharedPtr<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const FUeAgentExactCommandDispatch>& Dispatch,
		UWorld& World)
	{
		FTopologyObservation Before;
		FTopologyObservation After;
		const FCarrierStep PrepareCarrier =
			[](UStaticMesh& StaticMesh)
			{
				return PrepareOffsetCube(StaticMesh);
			};
		const FBuildParams BuildParams =
			[](const FString& TargetRef,
				const TSharedRef<FJsonObject>& Revision)
			{
				return MirrorParams(TargetRef, Revision, false, false);
			};
		const FCarrierStep VerifyBefore =
			[&Before](UStaticMesh& StaticMesh)
			{
				return ReadTopology(StaticMesh, Before)
					&& Before.TriangleCount == 12
					&& Before.BoundaryEdgeCount == 0
					&& Before.Min.Equals(FVector3d(50.0, -50.0, -50.0), 0.001)
					&& Before.Max.Equals(FVector3d(150.0, 50.0, 50.0), 0.001);
			};
		const FCarrierStep VerifyAfter =
			[&Before, &After](UStaticMesh& StaticMesh)
			{
				return ReadTopology(StaticMesh, After)
					&& After.VertexCount == Before.VertexCount
					&& After.EdgeCount == Before.EdgeCount
					&& After.TriangleCount == Before.TriangleCount
					&& After.BoundaryEdgeCount == Before.BoundaryEdgeCount
					&& After.Min.Equals(FVector3d(-150.0, -50.0, -50.0), 0.001)
					&& After.Max.Equals(FVector3d(-50.0, 50.0, 50.0), 0.001);
			};
		return RunCurrentStaticMeshProductCase(
			Test,
			Registry,
			Dispatch,
			World,
			TEXT("mesh.topology.mirror"),
			TEXT("T50PublicTopologyMirrorReplace"),
			PrepareCarrier,
			BuildParams,
			VerifyBefore,
			VerifyAfter);
	}

	bool RunCurrentMirrorAppendProductCase(
		FAutomationTestBase& Test,
		const TSharedPtr<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const FUeAgentExactCommandDispatch>& Dispatch,
		UWorld& World,
		const bool bWeldSeam)
	{
		FTopologyObservation Before;
		FTopologyObservation After;
		const FCarrierStep PrepareCarrier =
			[](UStaticMesh& StaticMesh)
			{
				return PrepareOpenHalfBox(StaticMesh);
			};
		const FBuildParams BuildParams =
			[bWeldSeam](const FString& TargetRef,
				const TSharedRef<FJsonObject>& Revision)
			{
				return MirrorParams(TargetRef, Revision, true, bWeldSeam);
			};
		const FCarrierStep VerifyBefore =
			[&Before](UStaticMesh& StaticMesh)
			{
				return ReadTopology(StaticMesh, Before)
					&& Before.VertexCount == 8
					&& Before.TriangleCount == 10
					&& Before.BoundaryEdgeCount == 4;
			};
		const FCarrierStep VerifyAfter =
			[&Before, &After, bWeldSeam](UStaticMesh& StaticMesh)
			{
				if (!ReadTopology(StaticMesh, After)
					|| After.TriangleCount != Before.TriangleCount * 2
					|| !After.Min.Equals(FVector3d(-100.0, -50.0, -50.0), 0.001)
					|| !After.Max.Equals(FVector3d(100.0, 50.0, 50.0), 0.001))
				{
					return false;
				}
				return bWeldSeam
					? After.VertexCount == 12
						&& After.BoundaryEdgeCount == 0
					: After.VertexCount == 16
						&& After.BoundaryEdgeCount == 8;
			};
		return RunCurrentStaticMeshProductCase(
			Test,
			Registry,
			Dispatch,
			World,
			TEXT("mesh.topology.mirror"),
			bWeldSeam
				? TEXT("T50PublicTopologyMirrorAppendWeld")
				: TEXT("T50PublicTopologyMirrorAppendOpen"),
			PrepareCarrier,
			BuildParams,
			VerifyBefore,
			VerifyAfter);
	}

	bool RunCurrentPlaneCutProductCase(
		FAutomationTestBase& Test,
		const TSharedPtr<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const FUeAgentExactCommandDispatch>& Dispatch,
		UWorld& World,
		const FString& KeepSide,
		const bool bFillHole)
	{
		FTopologyObservation Before;
		FTopologyObservation After;
		const FCarrierStep PrepareCarrier =
			[](UStaticMesh&)
			{
				return true;
			};
		const FBuildParams BuildParams =
			[KeepSide, bFillHole](
				const FString& TargetRef,
				const TSharedRef<FJsonObject>& Revision)
			{
				return PlaneCutParams(
					TargetRef,
					Revision,
					KeepSide,
					bFillHole);
			};
		const FCarrierStep VerifyBefore =
			[&Before](UStaticMesh& StaticMesh)
			{
				return ReadTopology(StaticMesh, Before)
					&& Before.TriangleCount == 12
					&& Before.BoundaryEdgeCount == 0;
			};
		const FCarrierStep VerifyAfter =
			[&Before, &After, KeepSide, bFillHole](UStaticMesh& StaticMesh)
			{
				if (!ReadTopology(StaticMesh, After)
					|| (bFillHole
						? After.BoundaryEdgeCount != 0
						: After.BoundaryEdgeCount == 0))
				{
					return false;
				}
				if (KeepSide == TEXT("positive"))
				{
					return After.Min.X >= -0.001
						&& FMath::IsNearlyEqual(After.Max.X, Before.Max.X, 0.001)
						&& After.TriangleCount > 0;
				}
				if (KeepSide == TEXT("negative"))
				{
					return After.Max.X <= 0.001
						&& FMath::IsNearlyEqual(After.Min.X, Before.Min.X, 0.001)
						&& After.TriangleCount > 0;
				}
				return After.Min.Equals(Before.Min, 0.001)
					&& After.Max.Equals(Before.Max, 0.001)
					&& After.TriangleCount > Before.TriangleCount;
			};
		const FString Scope = FString::Printf(
			TEXT("T50PublicTopologyPlaneCut%s%s"),
			KeepSide == TEXT("positive")
				? TEXT("Positive")
				: KeepSide == TEXT("negative")
					? TEXT("Negative")
					: TEXT("Both"),
			bFillHole ? TEXT("Filled") : TEXT("Open"));
		return RunCurrentStaticMeshProductCase(
			Test,
			Registry,
			Dispatch,
			World,
			TEXT("mesh.topology.plane_cut"),
			Scope,
			PrepareCarrier,
			BuildParams,
			VerifyBefore,
			VerifyAfter);
	}

	bool RunCurrentRemeshBoundaryProductCase(
		FAutomationTestBase& Test,
		const TSharedPtr<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const FUeAgentExactCommandDispatch>& Dispatch,
		UWorld& World,
		const bool bPreserveBoundaries)
	{
		FTopologyObservation Before;
		FTopologyObservation After;
		const FCarrierStep PrepareCarrier =
			[](UStaticMesh& StaticMesh)
			{
				return PrepareOpenGrid(StaticMesh);
			};
		const FBuildParams BuildParams =
			[bPreserveBoundaries](
				const FString& TargetRef,
				const TSharedRef<FJsonObject>& Revision)
			{
				return RemeshParams(
					TargetRef,
					Revision,
					80.0,
					10,
					bPreserveBoundaries,
					false);
			};
		const FCarrierStep VerifyBefore =
			[&Before](UStaticMesh& StaticMesh)
			{
				return ReadTopology(StaticMesh, Before)
					&& Before.TriangleCount == 32
					&& Before.BoundaryPositions.Num() == 16;
			};
		const FCarrierStep VerifyAfter =
			[&Before, &After, bPreserveBoundaries](UStaticMesh& StaticMesh)
			{
				if (!ReadTopology(StaticMesh, After)
					|| After.TriangleCount >= Before.TriangleCount)
				{
					return false;
				}
				const bool bSameBoundary = SamePositions(
					Before.BoundaryPositions,
					After.BoundaryPositions,
					0.001);
				return bPreserveBoundaries
					? bSameBoundary
					: !bSameBoundary;
			};
		return RunCurrentStaticMeshProductCase(
			Test,
			Registry,
			Dispatch,
			World,
			TEXT("mesh.topology.remesh"),
			bPreserveBoundaries
				? TEXT("T50PublicTopologyRemeshBoundaryPreserve")
				: TEXT("T50PublicTopologyRemeshBoundaryFree"),
			PrepareCarrier,
			BuildParams,
			VerifyBefore,
			VerifyAfter);
	}

	bool RunCurrentRemeshSharpEdgeProductCase(
		FAutomationTestBase& Test,
		const TSharedPtr<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const FUeAgentExactCommandDispatch>& Dispatch,
		UWorld& World,
		const bool bPreserveSharpEdges)
	{
		FTopologyObservation Before;
		FTopologyObservation After;
		const FCarrierStep PrepareCarrier =
			[](UStaticMesh& StaticMesh)
			{
				return PrepareHardCube(StaticMesh);
			};
		const FBuildParams BuildParams =
			[bPreserveSharpEdges](
				const FString& TargetRef,
				const TSharedRef<FJsonObject>& Revision)
			{
				return RemeshParams(
					TargetRef,
					Revision,
					50.0,
					10,
					false,
					bPreserveSharpEdges);
			};
		const FCarrierStep VerifyBefore =
			[&Before](UStaticMesh& StaticMesh)
			{
				return ReadTopology(StaticMesh, Before)
					&& Before.TriangleCount == 12
					&& Before.NormalSeamEdgeCount > 0;
			};
		const FCarrierStep VerifyAfter =
			[&Before, &After, bPreserveSharpEdges](UStaticMesh& StaticMesh)
			{
				if (!ReadTopology(StaticMesh, After))
				{
					return false;
				}
				const bool bContainsOriginalVertices = ContainsPositions(
					After.VertexPositions,
					Before.VertexPositions,
					0.001);
				return bPreserveSharpEdges
					? bContainsOriginalVertices
						&& After.NormalSeamEdgeCount
							>= Before.NormalSeamEdgeCount
					: !bContainsOriginalVertices;
			};
		return RunCurrentStaticMeshProductCase(
			Test,
			Registry,
			Dispatch,
			World,
			TEXT("mesh.topology.remesh"),
			bPreserveSharpEdges
				? TEXT("T50PublicTopologyRemeshSharpPreserve")
				: TEXT("T50PublicTopologyRemeshSharpFree"),
			PrepareCarrier,
			BuildParams,
			VerifyBefore,
			VerifyAfter);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentModelingGeometryPublicStaticMeshSubdivideTest,
	"UeAgentInterface.DomainAdapters.ModelingGeometryEffect.E4.PublicProduct."
		"StaticMeshTopologySubdivide",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentModelingGeometryPublicStaticMeshSubdivideTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Tests::ModelingGeometryPublicStaticMesh;
	using namespace UE::UAI::Tests::ModelingGeometryPublicStaticMeshTopology;
	(void)Parameters;
	return RunWithPublicStaticMeshEnvironment(
		*this,
		[this](const auto& Registry, const auto& Dispatch, UWorld& World)
		{
			const bool bUniform = RunCurrentSubdivideProductCase(
				*this,
				Registry,
				Dispatch,
				World,
				TEXT("uniform"));
			const bool bPnTriangles = RunCurrentSubdivideProductCase(
				*this,
				Registry,
				Dispatch,
				World,
				TEXT("pn_triangles"));
			return bUniform && bPnTriangles;
		});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentModelingGeometryPublicStaticMeshSimplifyBoundaryPolicyTest,
	"UeAgentInterface.DomainAdapters.ModelingGeometryEffect.E4.PublicProduct."
		"StaticMeshTopologySimplifyBoundaryPolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentModelingGeometryPublicStaticMeshSimplifyBoundaryPolicyTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Tests::ModelingGeometryPublicStaticMesh;
	using namespace UE::UAI::Tests::ModelingGeometryPublicStaticMeshTopology;
	(void)Parameters;
	return RunWithPublicStaticMeshEnvironment(
		*this,
		[this](const auto& Registry, const auto& Dispatch, UWorld& World)
		{
			const bool bPreserved = RunCurrentSimplifyProductCase(
				*this,
				Registry,
				Dispatch,
				World,
				true);
			const bool bFree = RunCurrentSimplifyProductCase(
				*this,
				Registry,
				Dispatch,
				World,
				false);
			return bPreserved && bFree;
		});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentModelingGeometryPublicStaticMeshSimplifyTargetModesTest,
	"UeAgentInterface.DomainAdapters.ModelingGeometryEffect.E4.PublicProduct."
		"StaticMeshTopologySimplifyTargetModes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentModelingGeometryPublicStaticMeshSimplifyTargetModesTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Tests::ModelingGeometryPublicStaticMesh;
	using namespace UE::UAI::Tests::ModelingGeometryPublicStaticMeshTopology;
	(void)Parameters;
	return RunWithPublicStaticMeshEnvironment(
		*this,
		[this](const auto& Registry, const auto& Dispatch, UWorld& World)
		{
			const bool bRatio = RunCurrentSimplifyTargetModeCase(
				*this,
				Registry,
				Dispatch,
				World,
				TEXT("triangle_ratio"),
				0.5);
			const bool bTolerance = RunCurrentSimplifyTargetModeCase(
				*this,
				Registry,
				Dispatch,
				World,
				TEXT("tolerance_cm"),
				0.001);
			return bRatio && bTolerance;
		});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentModelingGeometryPublicStaticMeshMirrorPolicyTest,
	"UeAgentInterface.DomainAdapters.ModelingGeometryEffect.E4.PublicProduct."
		"StaticMeshTopologyMirrorPolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentModelingGeometryPublicStaticMeshMirrorPolicyTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Tests::ModelingGeometryPublicStaticMesh;
	using namespace UE::UAI::Tests::ModelingGeometryPublicStaticMeshTopology;
	(void)Parameters;
	return RunWithPublicStaticMeshEnvironment(
		*this,
		[this](const auto& Registry, const auto& Dispatch, UWorld& World)
		{
			const bool bReplaced = RunCurrentMirrorReplaceProductCase(
				*this,
				Registry,
				Dispatch,
				World);
			const bool bAppendedOpen = RunCurrentMirrorAppendProductCase(
				*this,
				Registry,
				Dispatch,
				World,
				false);
			const bool bAppendedWelded = RunCurrentMirrorAppendProductCase(
				*this,
				Registry,
				Dispatch,
				World,
				true);
			return bReplaced && bAppendedOpen && bAppendedWelded;
		});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentModelingGeometryPublicStaticMeshPlaneCutPolicyTest,
	"UeAgentInterface.DomainAdapters.ModelingGeometryEffect.E4.PublicProduct."
		"StaticMeshTopologyPlaneCutPolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentModelingGeometryPublicStaticMeshPlaneCutPolicyTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Tests::ModelingGeometryPublicStaticMesh;
	using namespace UE::UAI::Tests::ModelingGeometryPublicStaticMeshTopology;
	(void)Parameters;
	return RunWithPublicStaticMeshEnvironment(
		*this,
		[this](const auto& Registry, const auto& Dispatch, UWorld& World)
		{
			const bool bPositiveFilled = RunCurrentPlaneCutProductCase(
				*this,
				Registry,
				Dispatch,
				World,
				TEXT("positive"),
				true);
			const bool bNegativeOpen = RunCurrentPlaneCutProductCase(
				*this,
				Registry,
				Dispatch,
				World,
				TEXT("negative"),
				false);
			const bool bBothFilled = RunCurrentPlaneCutProductCase(
				*this,
				Registry,
				Dispatch,
				World,
				TEXT("both"),
				true);
			const bool bBothOpen = RunCurrentPlaneCutProductCase(
				*this,
				Registry,
				Dispatch,
				World,
				TEXT("both"),
				false);
			return bPositiveFilled
				&& bNegativeOpen
				&& bBothFilled
				&& bBothOpen;
		});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentModelingGeometryPublicStaticMeshRemeshPolicyTest,
	"UeAgentInterface.DomainAdapters.ModelingGeometryEffect.E4.PublicProduct."
		"StaticMeshTopologyRemeshPolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentModelingGeometryPublicStaticMeshRemeshPolicyTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Tests::ModelingGeometryPublicStaticMesh;
	using namespace UE::UAI::Tests::ModelingGeometryPublicStaticMeshTopology;
	(void)Parameters;
	return RunWithPublicStaticMeshEnvironment(
		*this,
		[this](const auto& Registry, const auto& Dispatch, UWorld& World)
		{
			const bool bDensity = RunCurrentRemeshDensityProductCase(
				*this,
				Registry,
				Dispatch,
				World);
			const bool bBoundaryPreserved =
				RunCurrentRemeshBoundaryProductCase(
					*this,
					Registry,
					Dispatch,
					World,
					true);
			const bool bBoundaryFree =
				RunCurrentRemeshBoundaryProductCase(
					*this,
					Registry,
					Dispatch,
					World,
					false);
			const bool bSharpPreserved =
				RunCurrentRemeshSharpEdgeProductCase(
					*this,
					Registry,
					Dispatch,
					World,
					true);
			const bool bSharpFree = RunCurrentRemeshSharpEdgeProductCase(
				*this,
				Registry,
				Dispatch,
				World,
				false);
			return bDensity
				&& bBoundaryPreserved
				&& bBoundaryFree
				&& bSharpPreserved
				&& bSharpFree;
		});
}

#endif

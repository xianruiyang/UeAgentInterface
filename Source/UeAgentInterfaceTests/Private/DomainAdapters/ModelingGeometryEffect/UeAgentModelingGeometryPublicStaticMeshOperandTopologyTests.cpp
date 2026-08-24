#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryPublicStaticMeshTestSupport.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Engine/StaticMesh.h"
#include "MeshDescriptionToDynamicMesh.h"

namespace UE::UAI::Tests::ModelingGeometryPublicStaticMeshOperandTopology
{
	using namespace UE::Geometry;
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::CommandRegistry;
	using namespace UE::UAI::Tests::ModelingGeometryPublicStaticMesh;

	struct FOperandTopologyObservation final
	{
		int32 VertexCount = 0;
		int32 EdgeCount = 0;
		int32 TriangleCount = 0;
		int32 BoundaryEdgeCount = 0;
		FVector3d Min = FVector3d::Zero();
		FVector3d Max = FVector3d::Zero();

		bool operator==(const FOperandTopologyObservation& Other) const
		{
			return VertexCount == Other.VertexCount
				&& EdgeCount == Other.EdgeCount
				&& TriangleCount == Other.TriangleCount
				&& BoundaryEdgeCount == Other.BoundaryEdgeCount
				&& Min.Equals(Other.Min, 0.001)
				&& Max.Equals(Other.Max, 0.001);
		}
	};

	bool ReadOperandTopology(
		UStaticMesh& StaticMesh,
		FOperandTopologyObservation& Out)
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

	TSharedRef<FJsonObject> BuildOperandToTargetTransform()
	{
		const TSharedRef<FJsonObject> Transform = MakeShared<FJsonObject>();
		Transform->SetStringField(
			TEXT("from_frame_id"),
			TEXT("ue.mesh.operand_local"));
		Transform->SetStringField(
			TEXT("to_frame_id"),
			TEXT("ue.mesh.target_local"));
		Transform->SetStringField(TEXT("unit"), TEXT("centimeter"));
		Transform->SetArrayField(
			TEXT("translation"),
			Numbers({75.0, 25.0, 0.0}));
		Transform->SetArrayField(
			TEXT("rotation_quaternion_xyzw"),
			Numbers({0.0, 0.0, 0.0, 1.0}));
		Transform->SetArrayField(
			TEXT("scale"),
			Numbers({1.0, 1.0, 1.0}));
		Transform->SetStringField(
			TEXT("basis_evidence"),
			TEXT("exact owned target and operand carrier geometry"));
		Transform->SetStringField(
			TEXT("conversion_chain"),
			TEXT("operand local -> target local"));
		return Transform;
	}

	TSharedRef<FJsonObject> BuildOperandParamsBase(
		const FString& TargetRef,
		const TSharedRef<FJsonObject>& TargetRevision,
		const FString& OperandRef,
		const TSharedRef<FJsonObject>& OperandRevision)
	{
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target_ref"), TargetRef);
		Params->SetObjectField(TEXT("target_revision"), TargetRevision);
		Params->SetObjectField(
			TEXT("space_contract"),
			BuildTargetLocalSpaceContract());
		Params->SetStringField(TEXT("operand_ref"), OperandRef);
		Params->SetObjectField(TEXT("operand_revision"), OperandRevision);
		Params->SetObjectField(
			TEXT("operand_to_target"),
			BuildOperandToTargetTransform());
		return Params;
	}

	TSharedRef<FJsonObject> BooleanParams(
		const FString& TargetRef,
		const TSharedRef<FJsonObject>& TargetRevision,
		const FString& OperandRef,
		const TSharedRef<FJsonObject>& OperandRevision,
		const FString& Operation,
		const bool bSimplifyOutput)
	{
		const TSharedRef<FJsonObject> Params = BuildOperandParamsBase(
			TargetRef,
			TargetRevision,
			OperandRef,
			OperandRevision);
		Params->SetStringField(TEXT("operation"), Operation);
		Params->SetBoolField(TEXT("simplify_output"), bSimplifyOutput);
		return Params;
	}

	TSharedRef<FJsonObject> MeshCutParams(
		const FString& TargetRef,
		const TSharedRef<FJsonObject>& TargetRevision,
		const FString& OperandRef,
		const TSharedRef<FJsonObject>& OperandRevision,
		const FString& KeepSides,
		const bool bFillCut)
	{
		const TSharedRef<FJsonObject> Params = BuildOperandParamsBase(
			TargetRef,
			TargetRevision,
			OperandRef,
			OperandRevision);
		Params->SetStringField(TEXT("keep_sides"), KeepSides);
		Params->SetBoolField(TEXT("fill_cut"), bFillCut);
		return Params;
	}

	TSharedRef<FJsonObject> TrimParams(
		const FString& TargetRef,
		const TSharedRef<FJsonObject>& TargetRevision,
		const FString& OperandRef,
		const TSharedRef<FJsonObject>& OperandRevision,
		const FString& KeepRegion,
		const bool bFillHole)
	{
		const TSharedRef<FJsonObject> Params = BuildOperandParamsBase(
			TargetRef,
			TargetRevision,
			OperandRef,
			OperandRevision);
		Params->SetStringField(TEXT("keep_region"), KeepRegion);
		Params->SetBoolField(TEXT("fill_hole"), bFillHole);
		return Params;
	}

	TSharedRef<FJsonObject> VoxelBooleanParams(
		const FString& TargetRef,
		const TSharedRef<FJsonObject>& TargetRevision,
		const FString& OperandRef,
		const TSharedRef<FJsonObject>& OperandRevision,
		const FString& Operation,
		const double VoxelSize,
		const double Adaptivity)
	{
		const TSharedRef<FJsonObject> Params = BuildOperandParamsBase(
			TargetRef,
			TargetRevision,
			OperandRef,
			OperandRevision);
		Params->SetStringField(TEXT("operation"), Operation);
		Params->SetNumberField(TEXT("voxel_size_cm"), VoxelSize);
		Params->SetNumberField(TEXT("adaptivity"), Adaptivity);
		return Params;
	}

	bool RunCurrentBooleanProductCase(
		FAutomationTestBase& Test,
		const TSharedPtr<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const FUeAgentExactCommandDispatch>& Dispatch,
		UWorld& World,
		const FString& Operation,
		const bool bSimplifyOutput,
		FOperandTopologyObservation& OutTargetAfter)
	{
		FOperandTopologyObservation TargetBefore;
		FOperandTopologyObservation OperandBefore;
		FOperandTopologyObservation OperandAfter;
		const FCarrierStep Prepare =
			[](UStaticMesh&)
			{
				return true;
			};
		const FBuildOperandParams BuildParams =
			[Operation, bSimplifyOutput](
				const FString& TargetRef,
				const TSharedRef<FJsonObject>& TargetRevision,
				const FString& OperandRef,
				const TSharedRef<FJsonObject>& OperandRevision)
			{
				return BooleanParams(
					TargetRef,
					TargetRevision,
					OperandRef,
					OperandRevision,
					Operation,
					bSimplifyOutput);
			};
		const FCarrierStep VerifyTargetBefore =
			[&TargetBefore](UStaticMesh& StaticMesh)
			{
				return ReadOperandTopology(StaticMesh, TargetBefore)
					&& TargetBefore.VertexCount == 8
					&& TargetBefore.TriangleCount == 12
					&& TargetBefore.BoundaryEdgeCount == 0;
			};
		const FCarrierStep VerifyOperandBefore =
			[&OperandBefore](UStaticMesh& StaticMesh)
			{
				return ReadOperandTopology(StaticMesh, OperandBefore)
					&& OperandBefore.VertexCount == 8
					&& OperandBefore.TriangleCount == 12
					&& OperandBefore.BoundaryEdgeCount == 0;
			};
		const FCarrierStep VerifyTargetAfter =
			[&OutTargetAfter, Operation](UStaticMesh& StaticMesh)
			{
				if (!ReadOperandTopology(StaticMesh, OutTargetAfter)
					|| OutTargetAfter.BoundaryEdgeCount != 0)
				{
					return false;
				}
				const FVector3d ExpectedMin =
					Operation == TEXT("intersection")
						? FVector3d(25.0, -25.0, -50.0)
						: FVector3d(-50.0, -50.0, -50.0);
				const FVector3d ExpectedMax =
					Operation == TEXT("union")
						? FVector3d(125.0, 75.0, 50.0)
						: FVector3d(50.0, 50.0, 50.0);
				return OutTargetAfter.Min.Equals(ExpectedMin, 0.001)
					&& OutTargetAfter.Max.Equals(ExpectedMax, 0.001)
					&& (Operation != TEXT("union")
						|| OutTargetAfter.TriangleCount > 12);
			};
		const FCarrierStep VerifyOperandAfter =
			[&OperandBefore, &OperandAfter](UStaticMesh& StaticMesh)
			{
				return ReadOperandTopology(StaticMesh, OperandAfter)
					&& OperandAfter == OperandBefore;
			};
		return RunCurrentStaticMeshOperandProductCase(
			Test,
			Registry,
			Dispatch,
			World,
			TEXT("mesh.topology.boolean"),
			FString::Printf(
				TEXT("T50PublicTopologyBoolean%s%s"),
				*Operation,
				bSimplifyOutput ? TEXT("Simplified") : TEXT("Exact")),
			Prepare,
			Prepare,
			BuildParams,
			VerifyTargetBefore,
			VerifyOperandBefore,
			VerifyTargetAfter,
			VerifyOperandAfter);
	}

	bool RunCurrentOperandTopologyProductCase(
		FAutomationTestBase& Test,
		const TSharedPtr<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const FUeAgentExactCommandDispatch>& Dispatch,
		UWorld& World,
		const FString& CommandId,
		const FString& Scope,
		const FBuildOperandParams& BuildParams,
		const FVector3d& ExpectedMin,
		const FVector3d& ExpectedMax,
		const double BoundsTolerance,
		const bool bExpectClosed,
		const bool bExpectMoreTriangles,
		FOperandTopologyObservation& OutTargetAfter)
	{
		FOperandTopologyObservation TargetBefore;
		FOperandTopologyObservation OperandBefore;
		FOperandTopologyObservation OperandAfter;
		const FCarrierStep Prepare =
			[](UStaticMesh&)
			{
				return true;
			};
		const FCarrierStep VerifyTargetBefore =
			[&TargetBefore](UStaticMesh& StaticMesh)
			{
				return ReadOperandTopology(StaticMesh, TargetBefore)
					&& TargetBefore.VertexCount == 8
					&& TargetBefore.TriangleCount == 12
					&& TargetBefore.BoundaryEdgeCount == 0;
			};
		const FCarrierStep VerifyOperandBefore =
			[&OperandBefore](UStaticMesh& StaticMesh)
			{
				return ReadOperandTopology(StaticMesh, OperandBefore)
					&& OperandBefore.VertexCount == 8
					&& OperandBefore.TriangleCount == 12
					&& OperandBefore.BoundaryEdgeCount == 0;
			};
		const FCarrierStep VerifyTargetAfter =
			[&OutTargetAfter,
				ExpectedMin,
				ExpectedMax,
				BoundsTolerance,
				bExpectClosed,
				bExpectMoreTriangles](UStaticMesh& StaticMesh)
			{
				return ReadOperandTopology(StaticMesh, OutTargetAfter)
					&& (bExpectClosed
						? OutTargetAfter.BoundaryEdgeCount == 0
						: OutTargetAfter.BoundaryEdgeCount > 0)
					&& OutTargetAfter.Min.Equals(
						ExpectedMin,
						BoundsTolerance)
					&& OutTargetAfter.Max.Equals(
						ExpectedMax,
						BoundsTolerance)
					&& (bExpectMoreTriangles
						? OutTargetAfter.TriangleCount > 12
						: OutTargetAfter.TriangleCount > 0);
			};
		const FCarrierStep VerifyOperandAfter =
			[&OperandBefore, &OperandAfter](UStaticMesh& StaticMesh)
			{
				return ReadOperandTopology(StaticMesh, OperandAfter)
					&& OperandAfter == OperandBefore;
			};
		return RunCurrentStaticMeshOperandProductCase(
			Test,
			Registry,
			Dispatch,
			World,
			CommandId,
			Scope,
			Prepare,
			Prepare,
			BuildParams,
			VerifyTargetBefore,
			VerifyOperandBefore,
			VerifyTargetAfter,
			VerifyOperandAfter);
	}

	bool RunCurrentMeshCutProductCase(
		FAutomationTestBase& Test,
		const TSharedPtr<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const FUeAgentExactCommandDispatch>& Dispatch,
		UWorld& World,
		const FString& KeepSides,
		const bool bFillCut,
		FOperandTopologyObservation& OutTargetAfter)
	{
		const FBuildOperandParams BuildParams =
			[KeepSides, bFillCut](
				const FString& TargetRef,
				const TSharedRef<FJsonObject>& TargetRevision,
				const FString& OperandRef,
				const TSharedRef<FJsonObject>& OperandRevision)
			{
				return MeshCutParams(
					TargetRef,
					TargetRevision,
					OperandRef,
					OperandRevision,
					KeepSides,
					bFillCut);
			};
		return RunCurrentOperandTopologyProductCase(
			Test,
			Registry,
			Dispatch,
			World,
			TEXT("mesh.topology.mesh_cut"),
			FString::Printf(
				TEXT("T50PublicTopologyMeshCut%s%s"),
				*KeepSides,
				bFillCut ? TEXT("Filled") : TEXT("Open")),
			BuildParams,
			KeepSides == TEXT("target_inside")
				? FVector3d(25.0, -25.0, -50.0)
				: FVector3d(-50.0, -50.0, -50.0),
			FVector3d(50.0, 50.0, 50.0),
			0.001,
			bFillCut || KeepSides == TEXT("both"),
			true,
			OutTargetAfter);
	}

	bool RunCurrentTrimProductCase(
		FAutomationTestBase& Test,
		const TSharedPtr<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const FUeAgentExactCommandDispatch>& Dispatch,
		UWorld& World,
		const FString& KeepRegion,
		const bool bFillHole,
		FOperandTopologyObservation& OutTargetAfter)
	{
		const FBuildOperandParams BuildParams =
			[KeepRegion, bFillHole](
				const FString& TargetRef,
				const TSharedRef<FJsonObject>& TargetRevision,
				const FString& OperandRef,
				const TSharedRef<FJsonObject>& OperandRevision)
			{
				return TrimParams(
					TargetRef,
					TargetRevision,
					OperandRef,
					OperandRevision,
					KeepRegion,
					bFillHole);
			};
		return RunCurrentOperandTopologyProductCase(
			Test,
			Registry,
			Dispatch,
			World,
			TEXT("mesh.topology.trim"),
			FString::Printf(
				TEXT("T50PublicTopologyTrim%s%s"),
				*KeepRegion,
				bFillHole ? TEXT("Filled") : TEXT("Open")),
			BuildParams,
			KeepRegion == TEXT("inside")
				? FVector3d(25.0, -25.0, -50.0)
				: FVector3d(-50.0, -50.0, -50.0),
			FVector3d(50.0, 50.0, 50.0),
			0.001,
			bFillHole,
			false,
			OutTargetAfter);
	}

	bool RunCurrentVoxelBooleanProductCase(
		FAutomationTestBase& Test,
		const TSharedPtr<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const FUeAgentExactCommandDispatch>& Dispatch,
		UWorld& World,
		const FString& Operation,
		const double VoxelSize,
		const double Adaptivity,
		FOperandTopologyObservation& OutTargetAfter)
	{
		const FBuildOperandParams BuildParams =
			[Operation, VoxelSize, Adaptivity](
				const FString& TargetRef,
				const TSharedRef<FJsonObject>& TargetRevision,
				const FString& OperandRef,
				const TSharedRef<FJsonObject>& OperandRevision)
			{
				return VoxelBooleanParams(
					TargetRef,
					TargetRevision,
					OperandRef,
					OperandRevision,
					Operation,
					VoxelSize,
					Adaptivity);
			};
		const FVector3d ExpectedMin =
			Operation == TEXT("intersection")
				? FVector3d(25.0, -25.0, -50.0)
				: FVector3d(-50.0, -50.0, -50.0);
		const FVector3d ExpectedMax =
			Operation == TEXT("union")
				? FVector3d(125.0, 75.0, 50.0)
				: FVector3d(50.0, 50.0, 50.0);
		return RunCurrentOperandTopologyProductCase(
			Test,
			Registry,
			Dispatch,
			World,
			TEXT("mesh.topology.voxel_boolean"),
			FString::Printf(
				TEXT("T50PublicTopologyVoxelBoolean%sA%d"),
				*Operation,
				FMath::RoundToInt(Adaptivity * 100.0)),
			BuildParams,
			ExpectedMin,
			ExpectedMax,
			VoxelSize + 1.0,
			true,
			true,
			OutTargetAfter);
	}

	bool RunCurrentVoxelBooleanRejectedCase(
		FAutomationTestBase& Test,
		const TSharedPtr<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const FUeAgentExactCommandDispatch>& Dispatch,
		UWorld& World)
	{
		FOperandTopologyObservation TargetBefore;
		FOperandTopologyObservation OperandBefore;
		FOperandTopologyObservation TargetAfter;
		FOperandTopologyObservation OperandAfter;
		const FCarrierStep Prepare =
			[](UStaticMesh&)
			{
				return true;
			};
		const FBuildOperandParams BuildParams =
			[](
				const FString& TargetRef,
				const TSharedRef<FJsonObject>& TargetRevision,
				const FString& OperandRef,
				const TSharedRef<FJsonObject>& OperandRevision)
			{
				return VoxelBooleanParams(
					TargetRef,
					TargetRevision,
					OperandRef,
					OperandRevision,
					TEXT("union"),
					0.1,
					0.0);
			};
		const FCarrierStep VerifyTargetBefore =
			[&TargetBefore](UStaticMesh& StaticMesh)
			{
				return ReadOperandTopology(StaticMesh, TargetBefore)
					&& TargetBefore.VertexCount == 8
					&& TargetBefore.TriangleCount == 12
					&& TargetBefore.BoundaryEdgeCount == 0;
			};
		const FCarrierStep VerifyOperandBefore =
			[&OperandBefore](UStaticMesh& StaticMesh)
			{
				return ReadOperandTopology(StaticMesh, OperandBefore)
					&& OperandBefore.VertexCount == 8
					&& OperandBefore.TriangleCount == 12
					&& OperandBefore.BoundaryEdgeCount == 0;
			};
		const FCarrierStep VerifyTargetUnchanged =
			[&TargetBefore, &TargetAfter](UStaticMesh& StaticMesh)
			{
				return ReadOperandTopology(StaticMesh, TargetAfter)
					&& TargetAfter == TargetBefore;
			};
		const FCarrierStep VerifyOperandUnchanged =
			[&OperandBefore, &OperandAfter](UStaticMesh& StaticMesh)
			{
				return ReadOperandTopology(StaticMesh, OperandAfter)
					&& OperandAfter == OperandBefore;
			};
		return RunCurrentStaticMeshOperandRejectedCase(
			Test,
			Registry,
			Dispatch,
			World,
			TEXT("mesh.topology.voxel_boolean"),
			TEXT("T50PublicTopologyVoxelResolutionRejected"),
			TEXT("modeling_geometry_voxel_resolution_limit_exceeded"),
			Prepare,
			Prepare,
			BuildParams,
			VerifyTargetBefore,
			VerifyOperandBefore,
			VerifyTargetUnchanged,
			VerifyOperandUnchanged);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentModelingGeometryPublicStaticMeshMeshCutFilledModesTest,
	"UeAgentInterface.DomainAdapters.ModelingGeometryEffect.E4.PublicProduct."
		"StaticMeshTopologyMeshCutFilledModes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentModelingGeometryPublicStaticMeshMeshCutFilledModesTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Tests::ModelingGeometryPublicStaticMesh;
	using namespace
		UE::UAI::Tests::ModelingGeometryPublicStaticMeshOperandTopology;
	(void)Parameters;
	return RunWithPublicStaticMeshEnvironment(
		*this,
		[this](const auto& Registry, const auto& Dispatch, UWorld& World)
		{
			FOperandTopologyObservation Both;
			FOperandTopologyObservation Outside;
			FOperandTopologyObservation Inside;
			return RunCurrentMeshCutProductCase(
					*this,
					Registry,
					Dispatch,
					World,
					TEXT("both"),
					true,
					Both)
				&& RunCurrentMeshCutProductCase(
					*this,
					Registry,
					Dispatch,
					World,
					TEXT("target_outside"),
					true,
					Outside)
				&& RunCurrentMeshCutProductCase(
					*this,
					Registry,
					Dispatch,
					World,
					TEXT("target_inside"),
					true,
					Inside);
		});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentModelingGeometryPublicStaticMeshVoxelBooleanModesTest,
	"UeAgentInterface.DomainAdapters.ModelingGeometryEffect.E4.PublicProduct."
		"StaticMeshTopologyVoxelBooleanModes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentModelingGeometryPublicStaticMeshVoxelBooleanModesTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Tests::ModelingGeometryPublicStaticMesh;
	using namespace
		UE::UAI::Tests::ModelingGeometryPublicStaticMeshOperandTopology;
	(void)Parameters;
	return RunWithPublicStaticMeshEnvironment(
		*this,
		[this](const auto& Registry, const auto& Dispatch, UWorld& World)
		{
			FOperandTopologyObservation UnionExact;
			FOperandTopologyObservation UnionAdaptive;
			FOperandTopologyObservation Intersection;
			FOperandTopologyObservation Difference;
			const bool bUnionExact = RunCurrentVoxelBooleanProductCase(
				*this,
				Registry,
				Dispatch,
				World,
				TEXT("union"),
				5.0,
				0.0,
				UnionExact);
			const bool bUnionAdaptive = RunCurrentVoxelBooleanProductCase(
				*this,
				Registry,
				Dispatch,
				World,
				TEXT("union"),
				5.0,
				0.5,
				UnionAdaptive);
			const bool bIntersection = RunCurrentVoxelBooleanProductCase(
				*this,
				Registry,
				Dispatch,
				World,
				TEXT("intersection"),
				5.0,
				0.0,
				Intersection);
			const bool bDifference = RunCurrentVoxelBooleanProductCase(
				*this,
				Registry,
				Dispatch,
				World,
				TEXT("difference"),
				5.0,
				0.0,
				Difference);
			return bUnionExact
				&& bUnionAdaptive
				&& bIntersection
				&& bDifference
				&& TestTrue(
					TEXT("adaptivity reduces persisted voxel topology"),
					UnionAdaptive.TriangleCount
						< UnionExact.TriangleCount);
		});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentModelingGeometryPublicStaticMeshVoxelResolutionRejectedTest,
	"UeAgentInterface.DomainAdapters.ModelingGeometryEffect.E4.PublicProduct."
		"StaticMeshTopologyVoxelResolutionRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentModelingGeometryPublicStaticMeshVoxelResolutionRejectedTest::
	RunTest(const FString& Parameters)
{
	using namespace UE::UAI::Tests::ModelingGeometryPublicStaticMesh;
	using namespace
		UE::UAI::Tests::ModelingGeometryPublicStaticMeshOperandTopology;
	(void)Parameters;
	return RunWithPublicStaticMeshEnvironment(
		*this,
		[this](const auto& Registry, const auto& Dispatch, UWorld& World)
		{
			return RunCurrentVoxelBooleanRejectedCase(
				*this,
				Registry,
				Dispatch,
				World);
		});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentModelingGeometryPublicStaticMeshMeshCutOpenModesTest,
	"UeAgentInterface.DomainAdapters.ModelingGeometryEffect.E4.PublicProduct."
		"StaticMeshTopologyMeshCutOpenModes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentModelingGeometryPublicStaticMeshMeshCutOpenModesTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Tests::ModelingGeometryPublicStaticMesh;
	using namespace
		UE::UAI::Tests::ModelingGeometryPublicStaticMeshOperandTopology;
	(void)Parameters;
	return RunWithPublicStaticMeshEnvironment(
		*this,
		[this](const auto& Registry, const auto& Dispatch, UWorld& World)
		{
			FOperandTopologyObservation Outside;
			FOperandTopologyObservation Inside;
			return RunCurrentMeshCutProductCase(
					*this,
					Registry,
					Dispatch,
					World,
					TEXT("target_outside"),
					false,
					Outside)
				&& RunCurrentMeshCutProductCase(
					*this,
					Registry,
					Dispatch,
					World,
					TEXT("target_inside"),
					false,
					Inside);
		});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentModelingGeometryPublicStaticMeshTrimFilledModesTest,
	"UeAgentInterface.DomainAdapters.ModelingGeometryEffect.E4.PublicProduct."
		"StaticMeshTopologyTrimFilledModes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentModelingGeometryPublicStaticMeshTrimFilledModesTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Tests::ModelingGeometryPublicStaticMesh;
	using namespace
		UE::UAI::Tests::ModelingGeometryPublicStaticMeshOperandTopology;
	(void)Parameters;
	return RunWithPublicStaticMeshEnvironment(
		*this,
		[this](const auto& Registry, const auto& Dispatch, UWorld& World)
		{
			FOperandTopologyObservation Outside;
			FOperandTopologyObservation Inside;
			return RunCurrentTrimProductCase(
					*this,
					Registry,
					Dispatch,
					World,
					TEXT("outside"),
					true,
					Outside)
				&& RunCurrentTrimProductCase(
					*this,
					Registry,
					Dispatch,
					World,
					TEXT("inside"),
					true,
					Inside);
		});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentModelingGeometryPublicStaticMeshTrimOpenModesTest,
	"UeAgentInterface.DomainAdapters.ModelingGeometryEffect.E4.PublicProduct."
		"StaticMeshTopologyTrimOpenModes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentModelingGeometryPublicStaticMeshTrimOpenModesTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Tests::ModelingGeometryPublicStaticMesh;
	using namespace
		UE::UAI::Tests::ModelingGeometryPublicStaticMeshOperandTopology;
	(void)Parameters;
	return RunWithPublicStaticMeshEnvironment(
		*this,
		[this](const auto& Registry, const auto& Dispatch, UWorld& World)
		{
			FOperandTopologyObservation Outside;
			FOperandTopologyObservation Inside;
			return RunCurrentTrimProductCase(
					*this,
					Registry,
					Dispatch,
					World,
					TEXT("outside"),
					false,
					Outside)
				&& RunCurrentTrimProductCase(
					*this,
					Registry,
					Dispatch,
					World,
					TEXT("inside"),
					false,
					Inside);
		});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentModelingGeometryPublicStaticMeshBooleanUnionTest,
	"UeAgentInterface.DomainAdapters.ModelingGeometryEffect.E4.PublicProduct."
		"StaticMeshTopologyBooleanUnion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentModelingGeometryPublicStaticMeshBooleanUnionTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Tests::ModelingGeometryPublicStaticMesh;
	using namespace
		UE::UAI::Tests::ModelingGeometryPublicStaticMeshOperandTopology;
	(void)Parameters;
	return RunWithPublicStaticMeshEnvironment(
		*this,
		[this](const auto& Registry, const auto& Dispatch, UWorld& World)
		{
			FOperandTopologyObservation Union;
			return RunCurrentBooleanProductCase(
				*this,
				Registry,
				Dispatch,
				World,
				TEXT("union"),
				false,
				Union);
		});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentModelingGeometryPublicStaticMeshBooleanModesTest,
	"UeAgentInterface.DomainAdapters.ModelingGeometryEffect.E4.PublicProduct."
		"StaticMeshTopologyBooleanModes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentModelingGeometryPublicStaticMeshBooleanModesTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Tests::ModelingGeometryPublicStaticMesh;
	using namespace
		UE::UAI::Tests::ModelingGeometryPublicStaticMeshOperandTopology;
	(void)Parameters;
	return RunWithPublicStaticMeshEnvironment(
		*this,
		[this](const auto& Registry, const auto& Dispatch, UWorld& World)
		{
			FOperandTopologyObservation UnionExact;
			FOperandTopologyObservation UnionSimplified;
			FOperandTopologyObservation Intersection;
			FOperandTopologyObservation Difference;
			const bool bUnionExact = RunCurrentBooleanProductCase(
				*this,
				Registry,
				Dispatch,
				World,
				TEXT("union"),
				false,
				UnionExact);
			const bool bUnionSimplified = RunCurrentBooleanProductCase(
				*this,
				Registry,
				Dispatch,
				World,
				TEXT("union"),
				true,
				UnionSimplified);
			const bool bIntersection = RunCurrentBooleanProductCase(
				*this,
				Registry,
				Dispatch,
				World,
				TEXT("intersection"),
				false,
				Intersection);
			const bool bDifference = RunCurrentBooleanProductCase(
				*this,
				Registry,
				Dispatch,
				World,
				TEXT("difference"),
				false,
				Difference);
			return bUnionExact
				&& bUnionSimplified
				&& bIntersection
				&& bDifference
				&& TestTrue(
					TEXT("simplify_output reduces persisted union topology"),
					UnionSimplified.TriangleCount
						< UnionExact.TriangleCount);
		});
}

#endif

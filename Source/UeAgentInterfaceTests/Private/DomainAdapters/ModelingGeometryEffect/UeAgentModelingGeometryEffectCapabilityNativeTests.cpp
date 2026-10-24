#include "Misc/AutomationTest.h"

#include "Components/DynamicMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMeshActor.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"

#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryEffectCapabilities.h"
#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryEffectNormalObservation.h"

#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/Contract/UeAgentContractSchema.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/ExecutionMutation/UeAgentExecutionMutation.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "DomainAdapters/GenericEffect/UeAgentGenericEffectAdapter.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/DynamicMeshOverlay.h"
#include "DynamicMesh/MeshNormals.h"
#include "DynamicMeshToMeshDescription.h"
#include "Engine/StaticMesh.h"
#include "HAL/PlatformTime.h"
#include "Materials/Material.h"
#include "MeshDescriptionToDynamicMesh.h"
#include "PhysicsEngine/AggregateGeom.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshAttributes.h"

namespace
{
using UE::Geometry::FDynamicMesh3;
using UE::Geometry::FDynamicMeshAttributeSet;
using UE::Geometry::FDynamicMeshMaterialAttribute;
using UE::Geometry::FDynamicMeshNormalOverlay;
using UE::Geometry::FDynamicMeshUVOverlay;
using UE::Geometry::FIndex3i;
using UE::UAI::DomainAdapters::ModelingGeometryEffect::
	FNormalSemanticObservation;
using UE::UAI::DomainAdapters::ModelingGeometryEffect::
	IsCompleteAreaAnglePerVertexNormalObservation;
using UE::UAI::DomainAdapters::ModelingGeometryEffect::
	IsCompleteAngleThresholdNormalObservation;
using UE::UAI::DomainAdapters::ModelingGeometryEffect::
	IsCompleteFaceNormalObservation;
using UE::UAI::DomainAdapters::ModelingGeometryEffect::
	ObserveNormalSemantics;

	FString FixturePath()
	{
		return FPaths::ConvertRelativePathToFull(
			FPaths::Combine(
				FPaths::ProjectPluginsDir(),
				TEXT(
					"UeAgentInterface/Source/"
					"UeAgentInterfaceTests/Private/"
					"DomainAdapters/ModelingGeometryEffect/"
					"Fixtures/"
					"modeling_geometry_effect_contract_cases.json")));
	}

	TSharedPtr<FJsonObject> LoadFixture(
		FAutomationTestBase* Test)
	{
		FString Text;
		if (!Test->TestTrue(
				TEXT("load ModelingGeometryEffect fixture"),
				FFileHelper::LoadFileToString(
					Text,
					*FixturePath())))
		{
			return nullptr;
		}
		TSharedPtr<FJsonObject> Fixture;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Text);
		if (!Test->TestTrue(
				TEXT("parse ModelingGeometryEffect fixture"),
				FJsonSerializer::Deserialize(
					Reader,
					Fixture)
					&& Fixture.IsValid()))
		{
			return nullptr;
		}
		return Fixture;
	}

	struct FMeshReadback
	{
		int32 VertexCount = 0;
		int32 TriangleCount = 0;
		FBox Bounds = FBox(EForceInit::ForceInit);
		int64 VertexChecksum = 0;

		bool operator==(const FMeshReadback& Other) const
		{
			return VertexCount == Other.VertexCount
				&& TriangleCount == Other.TriangleCount
				&& Bounds.Min.Equals(Other.Bounds.Min)
				&& Bounds.Max.Equals(Other.Bounds.Max)
				&& VertexChecksum == Other.VertexChecksum;
		}
	};

	void BuildCube(FDynamicMesh3& Mesh)
	{
		Mesh.Clear();
		Mesh.EnableTriangleGroups();
		const int32 V0 = Mesh.AppendVertex(
			FVector3d(-50.0, -50.0, -50.0));
		const int32 V1 = Mesh.AppendVertex(
			FVector3d(50.0, -50.0, -50.0));
		const int32 V2 = Mesh.AppendVertex(
			FVector3d(50.0, 50.0, -50.0));
		const int32 V3 = Mesh.AppendVertex(
			FVector3d(-50.0, 50.0, -50.0));
		const int32 V4 = Mesh.AppendVertex(
			FVector3d(-50.0, -50.0, 50.0));
		const int32 V5 = Mesh.AppendVertex(
			FVector3d(50.0, -50.0, 50.0));
		const int32 V6 = Mesh.AppendVertex(
			FVector3d(50.0, 50.0, 50.0));
		const int32 V7 = Mesh.AppendVertex(
			FVector3d(-50.0, 50.0, 50.0));
		for (const FIndex3i Triangle : {
			FIndex3i(V0, V2, V1),
			FIndex3i(V0, V3, V2),
			FIndex3i(V4, V5, V6),
			FIndex3i(V4, V6, V7),
			FIndex3i(V0, V1, V5),
			FIndex3i(V0, V5, V4),
			FIndex3i(V1, V2, V6),
			FIndex3i(V1, V6, V5),
			FIndex3i(V2, V3, V7),
			FIndex3i(V2, V7, V6),
			FIndex3i(V3, V0, V4),
			FIndex3i(V3, V4, V7),
		})
		{
			Mesh.AppendTriangle(Triangle);
		}
		Mesh.ReverseOrientation(false);
		Mesh.EnableAttributes();
	}

	bool ReadMesh(
		UDynamicMesh& DynamicMesh,
		FMeshReadback& Out)
	{
		Out = FMeshReadback();
		bool bRead = false;
		DynamicMesh.ProcessMesh(
			[&Out, &bRead](const FDynamicMesh3& Mesh)
			{
				Out.VertexCount = Mesh.VertexCount();
				Out.TriangleCount = Mesh.TriangleCount();
				for (const int32 VertexId
					: Mesh.VertexIndicesItr())
				{
					const FVector3d Position =
						Mesh.GetVertex(VertexId);
					Out.Bounds += FVector(Position);
					Out.VertexChecksum +=
						FMath::RoundToInt64(Position.X * 100.0)
						+ FMath::RoundToInt64(
							Position.Y * 1000.0)
						+ FMath::RoundToInt64(
							Position.Z * 10000.0);
				}
				bRead = true;
			});
		return bRead;
	}

	class FOwnedDynamicMeshWorld final
	{
	public:
		FOwnedDynamicMeshWorld()
		{
			const FString Suffix =
				FGuid::NewGuid().ToString(
					EGuidFormats::Digits);
			Package = CreatePackage(
				*(TEXT("/Temp/UAI/ModelingGeometry/")
					+ Suffix));
			World = UWorld::CreateWorld(
				EWorldType::EditorPreview,
				false,
				FName(TEXT("ModelingGeometryOwnedWorld")),
				Package);
			if (!World || !World->PersistentLevel)
			{
				return;
			}
			World->SetCurrentLevel(World->PersistentLevel);
			Actor = SpawnCubeActor(
				TEXT("ModelingGeometryOwnedTarget"),
				FVector(1000.0, 2000.0, 300.0),
				FRotator(0.0, 37.0, 0.0),
				FVector(1.0, 2.0, 0.5));
			if (!Actor)
			{
				return;
			}
			UDynamicMeshComponent* Component =
				Actor->GetDynamicMeshComponent();
			DynamicMesh =
				Component ? Component->GetDynamicMesh() : nullptr;
			Package->SetDirtyFlag(false);
		}

		~FOwnedDynamicMeshWorld()
		{
			if (Package)
			{
				Package->SetDirtyFlag(false);
			}
			if (World)
			{
				World->DestroyWorld(false);
				World->MarkAsGarbage();
			}
			if (Package)
			{
				Package->MarkAsGarbage();
			}
		}

		bool IsValid() const
		{
			return Package
				&& World
				&& World->PersistentLevel
				&& Actor
				&& DynamicMesh;
		}

		ADynamicMeshActor* SpawnCubeActor(
			const FString& Label,
			const FVector& Location,
			const FRotator& Rotation = FRotator::ZeroRotator,
			const FVector& Scale = FVector::OneVector)
		{
			if (!World)
			{
				return nullptr;
			}
			FActorSpawnParameters SpawnParameters;
			SpawnParameters.SpawnCollisionHandlingOverride =
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			SpawnParameters.ObjectFlags |= RF_Transactional;
			ADynamicMeshActor* Created =
				World->SpawnActor<ADynamicMeshActor>(
					Location,
					Rotation,
					SpawnParameters);
			if (!Created)
			{
				return nullptr;
			}
			Created->SetActorScale3D(Scale);
			Created->SetActorLabel(Label);
			UDynamicMeshComponent* Component =
				Created->GetDynamicMeshComponent();
			UDynamicMesh* Mesh =
				Component ? Component->GetDynamicMesh() : nullptr;
			if (!Mesh)
			{
				Created->Destroy();
				return nullptr;
			}
			Mesh->EditMesh(
				[](FDynamicMesh3& EditMesh)
				{
					BuildCube(EditMesh);
				});
			return Created;
		}

		int32 CountDynamicMeshActors() const
		{
			int32 Count = 0;
			if (!World)
			{
				return Count;
			}
			for (TActorIterator<ADynamicMeshActor> It(World); It; ++It)
			{
				if (::IsValid(*It))
				{
					++Count;
				}
			}
			return Count;
		}

		UPackage* Package = nullptr;
		UWorld* World = nullptr;
		ADynamicMeshActor* Actor = nullptr;
		UDynamicMesh* DynamicMesh = nullptr;
	};

	bool ValidateOwnedFixture(
		FAutomationTestBase* Test,
		FOwnedDynamicMeshWorld& Owner,
		FMeshReadback& OutBaseline)
	{
		if (!Test->TestTrue(
				TEXT("owned dynamic-mesh world is valid"),
				Owner.IsValid()))
		{
			return false;
		}
		if (!Test->TestTrue(
				TEXT("independent UDynamicMesh readback succeeds"),
				ReadMesh(*Owner.DynamicMesh, OutBaseline)))
		{
			return false;
		}
		Test->TestEqual(
			TEXT("owned cube has eight vertices"),
			OutBaseline.VertexCount,
			8);
		Test->TestEqual(
			TEXT("owned cube has twelve triangles"),
			OutBaseline.TriangleCount,
			12);
		Test->TestTrue(
			TEXT("owned cube local bounds are exact"),
			OutBaseline.Bounds.Min.Equals(
				FVector(-50.0, -50.0, -50.0))
				&& OutBaseline.Bounds.Max.Equals(
					FVector(50.0, 50.0, 50.0)));
		Test->TestTrue(
			TEXT("fixture has rotated non-uniform world placement"),
			Owner.Actor->GetActorRotation().Equals(
				FRotator(0.0, 37.0, 0.0),
				0.01)
				&& Owner.Actor->GetActorScale3D().Equals(
					FVector(1.0, 2.0, 0.5)));
		Test->TestFalse(
			TEXT("fixture package starts clean"),
			Owner.Package->IsDirty());
		return !Test->HasAnyErrors();
	}

	class FOwnedReferenceResolver final
		: public UE::UAI::DomainAdapters::ModelingGeometryEffect::
			IUeAgentModelingGeometryEffectReferenceResolver
	{
	public:
		bool TryResolveReference(
			UWorld& World,
			const FString& CanonicalRef,
			UObject*& OutObject,
			UE::UAI::ExecutionMutation::
				FExecutionMutationError& OutError) override
		{
			OutObject = Objects.FindRef(CanonicalRef);
			if (!IsValid(OutObject)
				|| (OutObject->GetWorld()
					&& OutObject->GetWorld() != &World))
			{
				OutError =
					UE::UAI::ExecutionMutation::
						FExecutionMutationError(
							TEXT(
								"modeling_geometry_reference_unresolved"),
							CanonicalRef);
				return false;
			}
			return true;
		}

		TMap<FString, UObject*> Objects;
	};

#include "UeAgentModelingGeometryEffectCapabilityProductionTests.inl"
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentModelingNormalObservationContractTest,
	"UeAgentInterface.DomainAdapters.ModelingGeometryEffect."
		"NormalObservationContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentModelingNormalObservationContractTest::RunTest(
	const FString& Parameters)
{
	using UE::UAI::DomainAdapters::ModelingGeometryEffect::
		FNormalSemanticObservation;
	using UE::UAI::DomainAdapters::ModelingGeometryEffect::
		ObserveNormalSemantics;

	FDynamicMesh3 Mesh;
	const int32 A = Mesh.AppendVertex(FVector3d(0.0, 0.0, 0.0));
	const int32 B = Mesh.AppendVertex(FVector3d(100.0, 0.0, 0.0));
	const int32 C = Mesh.AppendVertex(FVector3d(0.0, 100.0, 0.0));
	const int32 TriangleId = Mesh.AppendTriangle(A, B, C);
	Mesh.EnableAttributes();

	const FNormalSemanticObservation Missing =
		ObserveNormalSemantics(Mesh);
	TestEqual(
		TEXT("unset overlay reports every triangle corner missing"),
		Missing.MissingTriangleCornerCount,
		Missing.TriangleCornerCount);

	auto SetCornerNormals =
		[&Mesh, TriangleId](const FVector3f& Value)
		{
			FDynamicMeshNormalOverlay* Normals =
				Mesh.Attributes()->PrimaryNormals();
			Normals->ClearElements();
			const FIndex3i Triangle = Mesh.GetTriangle(TriangleId);
			FIndex3i Elements;
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const int32 ElementId =
					Normals->AppendElement(Value);
				Normals->SetParentVertex(
					ElementId,
					Triangle[Corner]);
				Elements[Corner] = ElementId;
			}
			Normals->SetTriangle(TriangleId, Elements);
		};

	SetCornerNormals(FVector3f(1.0f, 0.0f, 0.0f));
	const FNormalSemanticObservation Misaligned =
		ObserveNormalSemantics(Mesh);
	TestEqual(
		TEXT("all triangle corners are observed"),
		Misaligned.ValidTriangleCornerCount,
		3);
	TestEqual(
		TEXT("orthogonal normals are not face aligned"),
		Misaligned.FaceAlignedTriangleCornerCount,
		0);
	TestEqual(
		TEXT("orthogonal normals are classified as mismatched"),
		Misaligned.FaceMisalignedTriangleCornerCount,
		3);

	const FVector3d FaceNormal = Mesh.GetTriNormal(TriangleId);
	SetCornerNormals(
		FVector3f(
			static_cast<float>(FaceNormal.X),
			static_cast<float>(FaceNormal.Y),
			static_cast<float>(FaceNormal.Z)));
	const FNormalSemanticObservation Aligned =
		ObserveNormalSemantics(Mesh);
	TestEqual(
		TEXT("face normals cover every triangle corner"),
		Aligned.FaceAlignedTriangleCornerCount,
		Aligned.TriangleCornerCount);
	TestEqual(
		TEXT("face normals contain no mismatch"),
		Aligned.FaceMisalignedTriangleCornerCount,
		0);
	TestEqual(
		TEXT("face normals contain no invalid corner"),
		Aligned.InvalidTriangleCornerCount,
		0);
	TestTrue(
		TEXT("face semantic contract accepts the aligned observation"),
		IsCompleteFaceNormalObservation(Aligned));

	FDynamicMesh3 FoldedMesh;
	const int32 V0 = FoldedMesh.AppendVertex(
		FVector3d(0.0, 0.0, 0.0));
	const int32 V1 = FoldedMesh.AppendVertex(
		FVector3d(100.0, 0.0, 0.0));
	const int32 V2 = FoldedMesh.AppendVertex(
		FVector3d(0.0, 100.0, 0.0));
	const int32 V3 = FoldedMesh.AppendVertex(
		FVector3d(0.0, 0.0, 100.0));
	FoldedMesh.AppendTriangle(V0, V1, V2);
	FoldedMesh.AppendTriangle(V0, V3, V1);
	FoldedMesh.EnableAttributes();
	FDynamicMeshNormalOverlay* FoldedNormals =
		FoldedMesh.Attributes()->PrimaryNormals();
	UE::Geometry::FMeshNormals::InitializeOverlayToPerVertexNormals(
		FoldedNormals,
		false);
	const FNormalSemanticObservation PerVertex =
		ObserveNormalSemantics(FoldedMesh);
	TestEqual(
		TEXT("per-vertex normals reference every mesh vertex"),
		PerVertex.ReferencedVertexCount,
		PerVertex.MeshVertexCount);
	TestEqual(
		TEXT("per-vertex normals use one element per vertex"),
		PerVertex.SingleNormalElementVertexCount,
		PerVertex.MeshVertexCount);
	TestEqual(
		TEXT("per-vertex normals keep the folded edge smooth"),
		PerVertex.FullySmoothInteriorEdgeCount,
		1);
	TestEqual(
		TEXT("per-vertex normals match area-angle reference directions"),
		PerVertex.WeightedAlignedNormalElementCount,
		PerVertex.ReferencedNormalElementCount);
	TestTrue(
		TEXT("area-angle semantic contract accepts per-vertex normals"),
		IsCompleteAreaAnglePerVertexNormalObservation(PerVertex));

	FoldedNormals->CreateFromPredicate(
		[](int32, int32, int32)
		{
			return false;
		},
		0.0f);
	UE::Geometry::FMeshNormals FoldedReference(&FoldedMesh);
	FoldedReference.RecomputeOverlayNormals(
		FoldedNormals,
		true,
		true);
	FoldedReference.CopyToOverlay(FoldedNormals);
	const FNormalSemanticObservation Split =
		ObserveNormalSemantics(FoldedMesh);
	TestEqual(
		TEXT("split normals classify the folded edge"),
		Split.FullySplitInteriorEdgeCount,
		1);
	TestEqual(
		TEXT("split normals record both edge endpoints"),
		Split.SplitInteriorEdgeEndpointKeys.Num(),
		2);
	TestEqual(
		TEXT("split normals retain weighted island directions"),
		Split.WeightedAlignedNormalElementCount,
		Split.ReferencedNormalElementCount);
	TestTrue(
		TEXT("angle-threshold semantic contract accepts the folded split"),
		IsCompleteAngleThresholdNormalObservation(Split, 30.0));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentModelingGeometryEffectTypedPlanningTest,
	"UeAgentInterface.DomainAdapters.ModelingGeometryEffect.E1."
		"TypedPlanningAndPreflight",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentModelingGeometryEffectTypedPlanningTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>& Rows =
		Fixture->GetArrayField(TEXT("descriptors"));
	const TArray<TSharedPtr<FJsonValue>>& NegativeCases =
		Fixture->GetArrayField(TEXT("e1_negative_cases"));
	TestEqual(
		TEXT("13 preflight-negative contracts"),
		NegativeCases.Num(),
		13);

	TSet<FString> OperationKeys;
	TSet<FString> DescriptorIds;
	for (const TSharedPtr<FJsonValue>& Value : Rows)
	{
		const TSharedPtr<FJsonObject> Row =
			Value->AsObject();
		const FString DescriptorId =
			Row->GetStringField(TEXT("descriptor_id"));
		const FString OperationKey =
			Row->GetStringField(TEXT("operation_key"));
		TestTrue(
			*(DescriptorId + TEXT(" operation key is unique")),
			!OperationKeys.Contains(OperationKey));
		OperationKeys.Add(OperationKey);
		DescriptorIds.Add(DescriptorId);
		TestTrue(
			*(DescriptorId
				+ TEXT(" has one target identity contract")),
			Row->HasField(TEXT("primitive_output"))
				!= Row->HasField(TEXT("existing_target")));
	}
	TSet<FString> NegativeIds;
	for (const TSharedPtr<FJsonValue>& Value : NegativeCases)
	{
		const TSharedPtr<FJsonObject> Case =
			Value->AsObject();
		const FString CaseId =
			Case->GetStringField(TEXT("case_id"));
		TestTrue(
			*(CaseId + TEXT(" is unique")),
			!NegativeIds.Contains(CaseId));
		NegativeIds.Add(CaseId);
		TestTrue(
			*(CaseId + TEXT(" targets a formal descriptor")),
			DescriptorIds.Contains(
				Case->GetStringField(TEXT("descriptor_id"))));
		TestEqual(
			*(CaseId + TEXT(" is pre-mutation")),
			static_cast<int32>(
				Case->GetNumberField(
					TEXT("mutation_count"))),
			0);
		TestTrue(
			*(CaseId + TEXT(" has a domain error code")),
			Case->GetStringField(TEXT("expected_error"))
				.StartsWith(TEXT("modeling_geometry_")));
	}

	RunModelingProductionPlanningTests(this, Fixture);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentModelingGeometryEffectOwnedCarrierTest,
	"UeAgentInterface.DomainAdapters.ModelingGeometryEffect.E2."
		"OwnedCarrierReadbackRollback",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentModelingGeometryEffectOwnedCarrierTest::RunTest(
	const FString& Parameters)
{
	FOwnedDynamicMeshWorld Owner;
	FMeshReadback Baseline;
	if (!ValidateOwnedFixture(this, Owner, Baseline))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>& Scenarios =
		Fixture->GetArrayField(TEXT("e2_scenarios"));
	TestEqual(
		TEXT("31 independent-readback scenarios"),
		Scenarios.Num(),
		31);

	TSet<FString> DescriptorIds;
	TMap<FString, int32> DirectApiCounts;
	for (const TSharedPtr<FJsonValue>& Value : Scenarios)
	{
		const TSharedPtr<FJsonObject> Scenario =
			Value->AsObject();
		const FString DescriptorId =
			Scenario->GetStringField(TEXT("descriptor_id"));
		const FString DirectApi =
			Scenario->GetStringField(TEXT("direct_api_family"));
		TArray<FString> ApiSegments;
		DirectApi.ParseIntoArray(
			ApiSegments,
			TEXT("."),
			true);
		const bool bNamesInteractiveOwner =
			ApiSegments.Contains(TEXT("Tool"))
			|| ApiSegments.Contains(TEXT("Mode"))
			|| ApiSegments.Contains(TEXT("Session"));
		TestTrue(
			*(DescriptorId + TEXT(" scenario is unique")),
			!DescriptorIds.Contains(DescriptorId));
		DescriptorIds.Add(DescriptorId);
		TestFalse(
			*(DescriptorId
				+ TEXT(" does not name a Modeling tool")),
			bNamesInteractiveOwner);
		TestFalse(
			*(DescriptorId
				+ TEXT(" has an independent oracle")),
			Scenario->GetStringField(
				TEXT("oracle_kind")).IsEmpty());
		DirectApiCounts.FindOrAdd(DirectApi) += 1;
	}
	TestEqual(
		TEXT("31 direct API scenarios are unique"),
		DescriptorIds.Num(),
		31);
	TestTrue(
		TEXT("direct APIs cover multiple geometry subsystems"),
		DirectApiCounts.Num() >= 10);

	FMeshReadback SecondReadback;
	TestTrue(
		TEXT("second independent readback succeeds"),
		ReadMesh(*Owner.DynamicMesh, SecondReadback));
	TestTrue(
		TEXT("fixture readback is stable before mutation"),
		Baseline == SecondReadback);
	TestFalse(
		TEXT("fixture remains clean before production apply"),
		Owner.Package->IsDirty());

	RunModelingProductionOwnedCarrierTests(this, Fixture);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentModelingGeometryEffectPrimitiveSemanticsTest,
	"UeAgentInterface.DomainAdapters.ModelingGeometryEffect.E2."
		"PrimitiveSemantics",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentModelingGeometryEffectPrimitiveSemanticsTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	return Fixture.IsValid()
		&& RunModelingProductionOwnedCarrierTests(
			this,
			Fixture,
			TEXT("geometry.primitive.create"),
			true);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentModelingGeometryEffectTopologySemanticsTest,
	"UeAgentInterface.DomainAdapters.ModelingGeometryEffect.E2."
		"TopologySemantics",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentModelingGeometryEffectTopologySemanticsTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	return Fixture.IsValid()
		&& RunModelingProductionOwnedCarrierTests(
			this,
			Fixture,
			TEXT("geometry.topology.edit"),
			true);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentModelingGeometryEffectAttributeSemanticsTest,
	"UeAgentInterface.DomainAdapters.ModelingGeometryEffect.E2."
		"AttributeSemantics",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentModelingGeometryEffectAttributeSemanticsTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	return Fixture.IsValid()
		&& RunModelingProductionOwnedCarrierTests(
			this,
			Fixture,
			TEXT("geometry.attribute.edit"),
			true);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentModelingGeometryEffectCollisionSemanticsTest,
	"UeAgentInterface.DomainAdapters.ModelingGeometryEffect.E2."
		"CollisionSemantics",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentModelingGeometryEffectCollisionSemanticsTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	return Fixture.IsValid()
		&& RunModelingProductionOwnedCarrierTests(
			this,
			Fixture,
			TEXT("geometry.collision.edit"),
			true);
}

#include "Misc/AutomationTest.h"

#include "Composition/SimpleAuthoring/Families/Modeling/UeAgentModelingSimpleAuthoringProduction.h"
#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringComposition.h"
#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringDurableService.h"
#include "Components/DynamicMeshComponent.h"
#include "Core/BindingGateway/Ticket/UeAgentBindingTicket.h"
#include "Core/Projection/UeAgentProjection.h"
#include "Core/StateSemantic/UeAgentStateRevisionJson.h"
#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryEffectCapabilities.h"
#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryEffectCarrierRevision.h"
#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryEffectNormalObservation.h"
#include "DomainAdapters/WorldSimpleEffect/UeAgentWorldSimpleEffectProduction.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMeshToMeshDescription.h"
#include "DynamicMeshActor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "EditorValidatorSubsystem.h"
#include "Materials/Material.h"
#include "MeshDescriptionToDynamicMesh.h"
#include "Misc/Guid.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PhysicsEngine/BodySetup.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "StaticMeshAttributes.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"
#include "UeAgentSimpleAuthoringPackageE4TestSupport.h"

namespace
{
	using namespace UE::UAI::AssetDurable;
	using namespace UE::UAI::BindingGateway::Ticket;
	using namespace UE::UAI::Composition::SimpleAuthoring;
	using namespace UE::UAI::Composition::SimpleAuthoring::Modeling;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::Discovery;
	using namespace
		UE::UAI::DomainAdapters::ModelingGeometryEffect;
	using namespace UE::UAI::DomainAdapters::WorldSimpleEffect;
	using namespace UE::UAI::ExecutionMutation;
	using namespace UE::UAI::Projection;
	using namespace UE::UAI::StateSemantic;
	using namespace UE::UAI::Verification;
	using UE::Geometry::FDynamicMeshAttributeSet;
	using UE::Geometry::FDynamicMesh3;
	using UE::Geometry::FDynamicMeshNormalOverlay;
	using UE::Geometry::FDynamicMeshUVOverlay;

	static_assert(
		UAI_MODELING_SIMPLE_AUTHORING_PRODUCTION_V1 == 1,
		"Modeling Simple Authoring production contract changed");

	const TCHAR* PrimitiveCapabilities[] = {
		TEXT("mesh.primitive.box.create"),
		TEXT("mesh.primitive.cylinder.create"),
		TEXT("mesh.primitive.plane.create"),
		TEXT("mesh.primitive.ramp.create"),
		TEXT("mesh.primitive.ramp_corner.create"),
		TEXT("mesh.primitive.sphere.create"),
		TEXT("mesh.primitive.stairs.create"),
	};

	TArray<TSharedPtr<FJsonValue>> Numbers(
		const std::initializer_list<double> Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		for (const double Value : Values)
		{
			Result.Add(MakeShared<FJsonValueNumber>(Value));
		}
		return Result;
	}

	TSharedPtr<FJsonObject> Placement()
	{
		const TSharedPtr<FJsonObject> Value =
			MakeShared<FJsonObject>();
		Value->SetStringField(
			TEXT("semantic_frame_id"),
			TEXT("ue.world"));
		Value->SetStringField(
			TEXT("write_frame_id"),
			TEXT("ue.world"));
		Value->SetStringField(
			TEXT("read_frame_id"),
			TEXT("ue.world"));
		Value->SetStringField(
			TEXT("verify_frame_id"),
			TEXT("ue.world"));
		Value->SetStringField(TEXT("unit"), TEXT("centimeter"));
		Value->SetArrayField(
			TEXT("translation"),
			Numbers({100.0, 200.0, 300.0}));
		Value->SetArrayField(
			TEXT("rotation_quaternion_xyzw"),
			Numbers({0.0, 0.0, 0.0, 1.0}));
		Value->SetArrayField(
			TEXT("scale"),
			Numbers({1.0, 2.0, 3.0}));
		Value->SetStringField(
			TEXT("basis_evidence"),
			TEXT("explicit normalized UE world placement"));
		Value->SetStringField(
			TEXT("conversion_chain"),
			TEXT(
				"semantic world -> C4 normalized UE world -> "
				"actor transform -> independent UE world readback"));
		return Value;
	}

	TSharedPtr<FJsonObject> PrimitivePayload(
		const FString& Capability,
		const FString& TargetRef)
	{
		const TSharedPtr<FJsonObject> Payload =
			MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("target_ref"), TargetRef);
		Payload->SetObjectField(TEXT("placement"), Placement());
		if (Capability == TEXT("mesh.primitive.box.create"))
		{
			Payload->SetArrayField(
				TEXT("size_cm"),
				Numbers({120.0, 80.0, 40.0}));
			Payload->SetArrayField(
				TEXT("subdivisions"),
				Numbers({2.0, 3.0, 4.0}));
			Payload->SetStringField(
				TEXT("anchor"),
				TEXT("base_center"));
		}
		else if (
			Capability == TEXT("mesh.primitive.cylinder.create"))
		{
			Payload->SetNumberField(TEXT("radius_cm"), 35.0);
			Payload->SetNumberField(TEXT("height_cm"), 90.0);
			Payload->SetNumberField(TEXT("radial_segments"), 12.0);
			Payload->SetNumberField(TEXT("height_segments"), 3.0);
			Payload->SetBoolField(TEXT("cap_ends"), true);
			Payload->SetStringField(
				TEXT("anchor"),
				TEXT("base_center"));
		}
		else if (
			Capability == TEXT("mesh.primitive.plane.create"))
		{
			Payload->SetArrayField(
				TEXT("size_cm"),
				Numbers({120.0, 80.0}));
			Payload->SetArrayField(
				TEXT("subdivisions"),
				Numbers({3.0, 2.0}));
			Payload->SetBoolField(TEXT("double_sided"), true);
		}
		else if (
			Capability == TEXT("mesh.primitive.ramp.create"))
		{
			Payload->SetArrayField(
				TEXT("size_cm"),
				Numbers({120.0, 80.0, 40.0}));
			Payload->SetStringField(TEXT("slope_axis"), TEXT("x"));
			Payload->SetStringField(
				TEXT("rise_direction"),
				TEXT("negative"));
		}
		else if (
			Capability == TEXT("mesh.primitive.ramp_corner.create"))
		{
			Payload->SetArrayField(
				TEXT("size_cm"),
				Numbers({120.0, 80.0, 40.0}));
			Payload->SetStringField(
				TEXT("high_corner"),
				TEXT("negative_x_negative_y"));
		}
		else if (
			Capability == TEXT("mesh.primitive.sphere.create"))
		{
			Payload->SetNumberField(TEXT("radius_cm"), 50.0);
			Payload->SetNumberField(
				TEXT("longitude_segments"),
				12.0);
			Payload->SetNumberField(
				TEXT("latitude_segments"),
				8.0);
		}
		else
		{
			Payload->SetNumberField(TEXT("step_count"), 4.0);
			Payload->SetNumberField(
				TEXT("step_width_cm"),
				100.0);
			Payload->SetNumberField(
				TEXT("step_depth_cm"),
				30.0);
			Payload->SetNumberField(
				TEXT("step_height_cm"),
				20.0);
			Payload->SetStringField(TEXT("rise_axis"), TEXT("x"));
			Payload->SetStringField(
				TEXT("rise_direction"),
				TEXT("negative"));
		}
		return Payload;
	}

	bool Serialize(
		const TSharedPtr<FJsonObject>& Object,
		FString& OutJson)
	{
		OutJson.Reset();
		const TSharedRef<TJsonWriter<>> Writer =
			TJsonWriterFactory<>::Create(&OutJson);
		return FJsonSerializer::Serialize(
			Object.ToSharedRef(),
			Writer);
	}

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
		for (const UE::Geometry::FIndex3i Triangle : {
			UE::Geometry::FIndex3i(V0, V2, V1),
			UE::Geometry::FIndex3i(V0, V3, V2),
			UE::Geometry::FIndex3i(V4, V5, V6),
			UE::Geometry::FIndex3i(V4, V6, V7),
			UE::Geometry::FIndex3i(V0, V1, V5),
			UE::Geometry::FIndex3i(V0, V5, V4),
			UE::Geometry::FIndex3i(V1, V2, V6),
			UE::Geometry::FIndex3i(V1, V6, V5),
			UE::Geometry::FIndex3i(V2, V3, V7),
			UE::Geometry::FIndex3i(V2, V7, V6),
			UE::Geometry::FIndex3i(V3, V0, V4),
			UE::Geometry::FIndex3i(V3, V4, V7),
		})
		{
			Mesh.AppendTriangle(Triangle);
		}
		Mesh.ReverseOrientation(false);
		Mesh.EnableAttributes();
	}

	class FOwnedModelingWorld final
	{
	public:
		FOwnedModelingWorld()
		{
			Package = CreatePackage(
				*(TEXT("/Temp/UAI/SA008/ModelingProduction/")
					+ FGuid::NewGuid().ToString(
						EGuidFormats::Digits)));
			World = UWorld::CreateWorld(
				EWorldType::EditorPreview,
				false,
				FName(TEXT("SA008ModelingProduction")),
				Package);
			if (!World || !World->PersistentLevel)
			{
				return;
			}
			World->SetCurrentLevel(World->PersistentLevel);
			ContextRevision = FString::Printf(
				TEXT("sa008-modeling-production:%u"),
				World->GetUniqueID());
			Package->SetDirtyFlag(false);
		}

		~FOwnedModelingWorld()
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

		ADynamicMeshActor* FindByLabel(
			const FString& Label) const
		{
			ADynamicMeshActor* Match = nullptr;
			if (!World)
			{
				return nullptr;
			}
			for (TActorIterator<ADynamicMeshActor> It(World);
				It;
				++It)
			{
				if (It->GetActorLabel() != Label)
				{
					continue;
				}
				if (Match)
				{
					return nullptr;
				}
				Match = *It;
			}
			return Match;
		}

		int32 CountActors() const
		{
			int32 Count = 0;
			if (World)
			{
				for (TActorIterator<ADynamicMeshActor> It(World);
					It;
					++It)
				{
					++Count;
				}
			}
			return Count;
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
			ADynamicMeshActor* Actor =
				World->SpawnActor<ADynamicMeshActor>(
					Location,
					Rotation,
					SpawnParameters);
			if (!Actor)
			{
				return nullptr;
			}
			Actor->SetActorScale3D(Scale);
			Actor->SetActorLabel(Label);
			UDynamicMeshComponent* Component =
				Actor->GetDynamicMeshComponent();
			UDynamicMesh* DynamicMesh =
				Component ? Component->GetDynamicMesh() : nullptr;
			if (!DynamicMesh)
			{
				Actor->Destroy();
				return nullptr;
			}
			DynamicMesh->EditMesh(
				[](FDynamicMesh3& Mesh)
				{
					BuildCube(Mesh);
				});
			return Actor;
		}

		bool IsValid() const
		{
			return Package
				&& World
				&& World->PersistentLevel
				&& !ContextRevision.IsEmpty();
		}

		UPackage* Package = nullptr;
		UWorld* World = nullptr;
		FString ContextRevision;
	};

	class FOwnedWorldCatalog final
		: public IUeAgentWorldSimpleEffectWorldCatalog
	{
	public:
		explicit FOwnedWorldCatalog(FOwnedModelingWorld& InOwner)
			: Owner(InOwner)
		{
		}

		bool TryEnumerate(
			TArray<FUeAgentWorldSimpleEffectWorldCandidate>&
				OutCandidates,
			FExecutionMutationError& OutError) override
		{
			OutCandidates.Reset();
			OutCandidates.Emplace(
				Owner.World,
				Owner.World->GetPathName(),
				Owner.World->PersistentLevel->GetPathName(),
				EUeAgentExecutionWorldType::Preview,
				TOptional<int32>(),
				Owner.ContextRevision);
			return true;
		}

	private:
		FOwnedModelingWorld& Owner;
	};

	class FExactReferenceResolver final
		: public IUeAgentModelingGeometryEffectReferenceResolver
	{
	public:
		bool TryResolveReference(
			UWorld& World,
			const FString& CanonicalRef,
			UObject*& OutObject,
			FExecutionMutationError& OutError) override
		{
			OutObject = Objects.FindRef(CanonicalRef);
			if (!OutObject)
			{
				OutError = FExecutionMutationError(
					TEXT("sa008_modeling_reference_unbound"),
					CanonicalRef);
				return false;
			}
			OutError = FExecutionMutationError();
			return true;
		}

		TMap<FString, TObjectPtr<UObject>> Objects;
	};

	class FRecordingDurableService final
		: public IUeAgentSimpleAuthoringAssetDurableService
	{
	public:
		bool TryPrepare(
			const FAssetDurableRequest& Request,
			FUeAgentResourceTracker& Resources,
			FAssetDurableSessionPtr& OutSession,
			FAssetDurableError& OutError) override
		{
			++PrepareCount;
			if (!Request.IsValid()
				|| Request.GetTargets().Num() != 1
				|| Request.GetTargets()[0].GetArtifactKind()
					!= EDurableArtifactKind::Package)
			{
				OutError = FAssetDurableError(
					TEXT(
						"sa008_modeling_durable_target_invalid"));
				return false;
			}
			SeenPackageName =
				Request.GetTargets()[0].GetPackageName();
			TArray<FDurableArtifact> Artifacts;
			Artifacts.Emplace(
				TEXT("artifact:sa008-modeling-production:")
					+ FString::FromInt(PrepareCount),
				Request.GetTargets()[0].GetIdentity(),
				SeenPackageName,
				TEXT(
					"fixture://sa008-modeling-production.umap"),
				EAssetDurableIntent::Save,
				TEXT("world_package"));
			FString InventoryHash;
			if (!TryComputeDurableInventoryHash(
					Artifacts,
					Resources,
					InventoryHash,
					OutError))
			{
				return false;
			}
			OutSession = MakeShared<const FAssetDurableSession>(
				Request,
				FDurableInventory(
					MoveTemp(Artifacts),
					MoveTemp(InventoryHash)),
				TEXT("journal:sa008-modeling-production:")
					+ FString::FromInt(PrepareCount),
				TEXT(
					"runtimeLogs/SA008ModelingProduction/"
					"journal.hjson"),
				TEXT(
					"runtimeLogs/SA008ModelingProduction"),
				TEXT(
					"runtimeLogs/SA008ModelingProduction/"
					"transaction"),
				TEXT(
					"runtimeLogs/SA008ModelingProduction/staging"),
				TArray<FString>{TEXT("prepared")});
			return OutSession.IsValid();
		}

		bool TryEnlist(
			const FAssetDurableSessionRef& Session,
			const TArray<FAssetDurableTarget>& MutationTargets,
			const int64 CurrentFencingToken,
			FUeAgentResourceTracker& Resources,
			FAssetDurableSessionPtr& OutSession,
			FAssetDurableError& OutError) override
		{
			OutError = FAssetDurableError();
			OutSession = Session;
			return MutationTargets.IsEmpty()
				&& CurrentFencingToken
					== Session->GetRequest().GetFencingToken();
		}

		bool TryPersist(
			const FAssetDurableSessionRef& Session,
			const int64 CurrentFencingToken,
			FUeAgentResourceTracker& Resources,
			FDurableReceipt& OutReceipt,
			FAssetDurableError& OutError) override
		{
			++PersistCount;
			OutReceipt = FDurableReceipt(
				Session->GetJournalId(),
				EDurableStatus::PersistedUnverified,
				Session->GetRequest().GetPlan()->GetFingerprint(),
				Session->GetInventory().GetInventoryHash(),
				{TEXT("prepared"), TEXT("persisted_unverified")},
				ERecoveryAction::None,
				true,
				false);
			return CurrentFencingToken
					== Session->GetRequest().GetFencingToken()
				&& OutReceipt.RequiresIndependentVerification();
		}

		bool TryRollback(
			const FAssetDurableSessionRef& Session,
			const int64 CurrentFencingToken,
			const bool bMemoryRollbackConfirmed,
			FUeAgentResourceTracker& Resources,
			FDurableReceipt& OutReceipt,
			FAssetDurableError& OutError) override
		{
			OutError = FAssetDurableError(
				TEXT(
					"sa008_modeling_unexpected_durable_rollback"));
			return false;
		}

		bool TryAcknowledge(
			const FAssetDurableSessionRef& Session,
			const FDurableVerificationAcknowledgement&
				Acknowledgement,
			FUeAgentResourceTracker& Resources,
			FAssetDurableError& OutError) override
		{
			++AcknowledgeCount;
			return Acknowledgement.IsValid()
				&& Acknowledgement.GetJournalId()
					== Session->GetJournalId();
		}

		int32 PrepareCount = 0;
		int32 PersistCount = 0;
		int32 AcknowledgeCount = 0;
		FString SeenPackageName;
	};

	class FUnusedAbsenceOracle final : public IAbsenceOracle
	{
	public:
		bool TryObserve(
			const FUeAgentCanonicalIdentity& Identity,
			const TArray<FDurableArtifact>& Artifacts,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			FAbsenceObservation& OutObservation,
			FVerificationError& OutError) override
		{
			OutObservation = FAbsenceObservation(
				Identity,
				false,
				true,
				TEXT("unused"));
			return true;
		}
	};

	class FUnusedReferenceOracle final : public IReferenceOracle
	{
	public:
		bool TryObserve(
			const FUeAgentCanonicalIdentity& Identity,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			FReferenceObservation& OutObservation,
			FVerificationError& OutError) override
		{
			OutObservation = FReferenceObservation(
				Identity,
				{},
				true,
				TEXT("unused"));
			return true;
		}
	};

	class FTokenEstimator final : public IUeAgentTokenEstimator
	{
	public:
		bool TryEstimate(
			const FUeAgentProjectedPayload& Payload,
			const TArray<FUeAgentArtifactDescriptor>& Artifacts,
			const TArray<FUeAgentProjectionDiagnostic>& Diagnostics,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			FUeAgentTokenEstimate& OutEstimate,
			FUeAgentProjectionError& OutError) override
		{
			OutEstimate = FUeAgentTokenEstimate(
				EUeAgentTokenEstimateKind::Exact,
				32,
				128);
			return true;
		}
	};

	class FNeverCancelled final
		: public IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override
		{
			return false;
		}
	};

	const FUeAgentSimpleAuthoringCommandRoute* FindRoute(
		const FString& CapabilityId)
	{
		return GetSimpleAuthoringCommandRoutes().FindByPredicate(
			[&CapabilityId](
				const FUeAgentSimpleAuthoringCommandRoute& Route)
			{
				return Route.CapabilityId == CapabilityId;
			});
	}

	bool IsComplete(const FUeAgentOperationResult& Result)
	{
		const FString Json = Result.GetValue().GetJsonValue();
		return Json.Contains(
				TEXT("\"effect_outcome\":\"complete\""))
			&& Json.Contains(TEXT("\"status\":\"verified\""));
	}

	bool ReadBounds(
		ADynamicMeshActor& Actor,
		FBox& OutBounds,
		int32& OutTriangles)
	{
		OutBounds = FBox(EForceInit::ForceInit);
		OutTriangles = 0;
		UDynamicMeshComponent* Component =
			Actor.GetDynamicMeshComponent();
		UDynamicMesh* Mesh =
			Component ? Component->GetDynamicMesh() : nullptr;
		if (!Mesh)
		{
			return false;
		}
		Mesh->ProcessMesh(
			[&OutBounds, &OutTriangles](
				const FDynamicMesh3& DynamicMesh)
			{
				for (const int32 VertexId
					: DynamicMesh.VertexIndicesItr())
				{
					OutBounds +=
						FVector(DynamicMesh.GetVertex(VertexId));
				}
				OutTriangles = DynamicMesh.TriangleCount();
			});
		return OutBounds.IsValid && OutTriangles > 0;
	}

	FVector ExpectedSize(const FString& Capability)
	{
		if (Capability == TEXT("mesh.primitive.cylinder.create"))
		{
			return FVector(70.0, 70.0, 90.0);
		}
		if (Capability == TEXT("mesh.primitive.plane.create"))
		{
			return FVector(120.0, 80.0, 0.0);
		}
		if (Capability == TEXT("mesh.primitive.sphere.create"))
		{
			return FVector(100.0);
		}
		if (Capability == TEXT("mesh.primitive.stairs.create"))
		{
			return FVector(120.0, 100.0, 80.0);
		}
		return FVector(120.0, 80.0, 40.0);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringModelingPrimitiveProductionContractTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3."
		"ModelingPrimitiveProductionContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool
FUeAgentSimpleAuthoringModelingPrimitiveProductionContractTest::
	RunTest(const FString& Parameters)
{
	FOwnedModelingWorld Owner;
	if (!TestTrue(
			TEXT("owned exact Modeling world is valid"),
			Owner.IsValid()))
	{
		return false;
	}

	TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
	FString RegistryError;
	TestTrue(
		TEXT("Simple Authoring descriptors compose"),
		TryCreateSimpleAuthoringDescriptorRegistry(
			Registry,
			RegistryError));
	if (!Registry.IsValid())
	{
		AddError(
			TEXT("sa008_modeling_registry_invalid:")
			+ RegistryError);
		return false;
	}

	TArray<FUeAgentSimpleAuthoringCommandRoute> Routes;
	for (const TCHAR* Capability : PrimitiveCapabilities)
	{
		const FUeAgentSimpleAuthoringCommandRoute* Route =
			FindRoute(Capability);
		if (!TestNotNull(
				*FString::Printf(
					TEXT("%s has one exact route"),
					Capability),
				Route))
		{
			return false;
		}
		Routes.Add(*Route);
	}

	const TSharedRef<FOwnedWorldCatalog, ESPMode::ThreadSafe>
		Catalog = MakeShared<
			FOwnedWorldCatalog,
			ESPMode::ThreadSafe>(Owner);
	const TSharedRef<
		FExactReferenceResolver,
		ESPMode::ThreadSafe> ReferenceResolver =
			MakeShared<
				FExactReferenceResolver,
				ESPMode::ThreadSafe>();
	const TSharedRef<
		FRecordingDurableService,
		ESPMode::ThreadSafe> Durable =
			MakeShared<
				FRecordingDurableService,
				ESPMode::ThreadSafe>();

	FUeAgentModelingSimpleAuthoringProviderConfig ProviderConfig;
	ProviderConfig.DescriptorRegistry = Registry;
	ProviderConfig.WorldCatalog = Catalog;
	ProviderConfig.ReferenceResolver = ReferenceResolver;
	ProviderConfig.DurableService = Durable;
	ProviderConfig.AbsenceOracle =
		MakeShared<FUnusedAbsenceOracle>();
	ProviderConfig.ReferenceOracle =
		MakeShared<FUnusedReferenceOracle>();
	ProviderConfig.OwnerLeaseId =
		TEXT("owner:sa008-modeling-production");
	ProviderConfig.Projection.Emplace(
		FUeAgentProjectionProfile(
			TEXT("compact"),
			false,
			false,
			false),
		TArray<FUeAgentArtifactDescriptor>(),
		TArray<FUeAgentProjectionDiagnostic>());

	TSharedPtr<IUeAgentSimpleAuthoringCapabilityProvider> Provider;
	FUeAgentModelingSimpleAuthoringError ProviderError;
	TestTrue(
		TEXT("Modeling production family composes"),
		FUeAgentModelingSimpleAuthoringProviderFactory::TryCreate(
			ProviderConfig,
			Provider,
			ProviderError));
	if (!Provider.IsValid())
	{
		AddError(
			TEXT("sa008_modeling_provider_invalid:")
			+ ProviderError.Code);
		return false;
	}

	TSharedPtr<const FUeAgentOperationTicketService> TicketService;
	FUeAgentOperationTicketError TicketError;
	TestTrue(
		TEXT("ticket service creates"),
		FUeAgentOperationTicketService::TryCreate(
			TEXT("sa008-modeling-production-integrity"),
			60.0,
			[]()
			{
				return 100.0;
			},
			[]()
			{
				return FString(
					TEXT("ticket:sa008-modeling-production"));
			},
			TicketService,
			TicketError));
	TSharedPtr<const FUeAgentResponseProjector> Projector;
	FUeAgentProjectionError ProjectionError;
	TestTrue(
		TEXT("response projector creates"),
		FUeAgentResponseProjector::TryCreate(
			MakeShared<FTokenEstimator>(),
			Projector,
			ProjectionError));
	if (!TicketService.IsValid() || !Projector.IsValid())
	{
		return false;
	}

	FUeAgentSimpleAuthoringApplicationConfig ApplicationConfig;
	ApplicationConfig.DescriptorRegistry = Registry;
	ApplicationConfig.ExecutionContext.ProjectInstanceId =
		TEXT("project:sa008-modeling-production");
	ApplicationConfig.ExecutionContext.EditorInstanceId =
		TEXT("editor:sa008-modeling-production");
	ApplicationConfig.ExecutionContext.StatePlane =
		EUeAgentStatePlane::AuthoringMemory;
	ApplicationConfig.ExecutionContext.WorldType =
		EUeAgentExecutionWorldType::Preview;
	ApplicationConfig.ExecutionContext.WorldIdentity =
		Owner.World->GetPathName();
	ApplicationConfig.ExecutionContext.LevelIdentity =
		Owner.World->PersistentLevel->GetPathName();
	ApplicationConfig.ExecutionContext.ContextRevision =
		Owner.ContextRevision;
	ApplicationConfig.ExecutionContext.bResolvedFromCurrent = false;
	ApplicationConfig.ServerSessionId =
		TEXT("session:sa008-modeling-production");
	ApplicationConfig.RegistryEpoch = 1;


	TSharedPtr<FUeAgentSimpleAuthoringApplication> Application;
	FUeAgentSimpleAuthoringApplicationError ApplicationError;
	TestTrue(
		TEXT("application accepts exact Modeling primitive routes"),
		FUeAgentSimpleAuthoringApplication::TryCreate(
			ApplicationConfig,
			MoveTemp(Routes),
			{Provider.ToSharedRef()},
			TicketService.ToSharedRef(),
			Projector.ToSharedRef(),
			Application,
			ApplicationError));
	if (!Application.IsValid())
	{
		AddError(
			TEXT("sa008_modeling_application_invalid:")
			+ ApplicationError.Code);
		return false;
	}

	const FUeAgentCallerContext Caller(
		TEXT("caller:sa008-modeling-production"),
		{TEXT("uai:modeling-geometry:write")},
		TEXT("correlation:sa008-modeling-production"));
	const TSharedRef<FNeverCancelled> Cancellation =
		MakeShared<FNeverCancelled>();
	TArray<FString> CreatedLabels;
	int32 SubmittedPrimitiveCount = 0;
	for (int32 Index = 0;
		Index < UE_ARRAY_COUNT(PrimitiveCapabilities);
		++Index)
	{
		const FString Capability = PrimitiveCapabilities[Index];
		const FString TargetRef = FString::Printf(
			TEXT("SA008_Modeling_Primitive_%02d"),
			Index);
		const FUeAgentSimpleAuthoringCommandRoute* Route =
			FindRoute(Capability);
		FString Payload;
		TestTrue(
			*(Capability + TEXT(" payload serializes")),
			Serialize(
				PrimitivePayload(Capability, TargetRef),
				Payload));
		++SubmittedPrimitiveCount;
		const FUeAgentOperationResult Result =
			Application->Submit(
				FUeAgentOperationRequest(
					Route->DescriptorIdentity,
					FString::Printf(
						TEXT("request:sa008-modeling:%02d"),
						Index),
					FString::Printf(
						TEXT("idempotency:sa008-modeling:%02d"),
						Index),
					FUeAgentContractValue(Payload)),
				Caller,
				Cancellation);
		if (!IsComplete(Result))
		{
			AddError(
				Capability
				+ TEXT(
					" did not complete verified lifecycle; result=")
				+ Result.GetValue().GetJsonValue());
		}

		ADynamicMeshActor* Actor =
			Owner.FindByLabel(TargetRef);
		if (!TestNotNull(
				*(Capability + TEXT(" created actor is independently found")),
				Actor))
		{
			continue;
		}
		TestTrue(
			*(Capability + TEXT(" world placement is exact")),
			Actor->GetActorLocation().Equals(
					FVector(100.0, 200.0, 300.0),
					0.01)
				&& Actor->GetActorQuat().Equals(
					FQuat::Identity,
					0.0001)
				&& Actor->GetActorScale3D().Equals(
					FVector(1.0, 2.0, 3.0),
					0.0001));
		FBox Bounds;
		int32 TriangleCount = 0;
		TestTrue(
			*(Capability + TEXT(" mesh readback succeeds")),
			ReadBounds(*Actor, Bounds, TriangleCount));
		TestTrue(
			*(Capability + TEXT(" requested local bounds are exact")),
			Bounds.GetSize().Equals(
				ExpectedSize(Capability),
				0.5));
		CreatedLabels.Add(TargetRef);
	}

	TestEqual(
		TEXT("every submitted primitive operation prepares exactly once"),
		Durable->PrepareCount,
		SubmittedPrimitiveCount);
	TestEqual(
		TEXT("every submitted primitive operation persists exactly once"),
		Durable->PersistCount,
		SubmittedPrimitiveCount);
	TestEqual(
		TEXT("every submitted primitive operation acknowledges after C9"),
		Durable->AcknowledgeCount,
		SubmittedPrimitiveCount);
	TestEqual(
		TEXT("seven exact primitive actors exist"),
		Owner.CountActors(),
		7);
	if (CreatedLabels.IsEmpty())
	{
		AddError(TEXT(
			"modeling primitive production created no actor; "
			"duplicate-target assertions cannot continue safely"));
		return false;
	}

	const FString DuplicatePayloadJson = [&]()
	{
		FString Json;
		Serialize(
			PrimitivePayload(
				TEXT("mesh.primitive.box.create"),
				CreatedLabels[0]),
			Json);
		return Json;
	}();
	const FUeAgentSimpleAuthoringCommandRoute* BoxRoute =
		FindRoute(TEXT("mesh.primitive.box.create"));
	if (!TestNotNull(TEXT("box primitive route remains available"), BoxRoute))
	{
		return false;
	}
	const int32 PrepareCountBeforeDuplicate = Durable->PrepareCount;
	const FUeAgentOperationResult Duplicate =
		Application->Submit(
			FUeAgentOperationRequest(
				BoxRoute->DescriptorIdentity,
				TEXT("request:sa008-modeling:duplicate"),
				TEXT("idempotency:sa008-modeling:duplicate"),
				FUeAgentContractValue(DuplicatePayloadJson)),
			Caller,
			Cancellation);
	TestFalse(
		TEXT("duplicate primitive target fails before mutation"),
		IsComplete(Duplicate));
	TestEqual(
		TEXT("duplicate primitive does not prepare durable work"),
		Durable->PrepareCount,
		PrepareCountBeforeDuplicate);
	TestEqual(
		TEXT("duplicate primitive does not spawn an actor"),
		Owner.CountActors(),
		7);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentModelingCarrierRevisionTracksMutableStateTest,
	"UeAgentInterface.Composition.SimpleAuthoring.Modeling."
	"CarrierRevisionTracksMutableState",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentModelingCarrierRevisionTracksMutableStateTest::RunTest(
	const FString& Parameters)
{
	FOwnedModelingWorld Owner;
	if (!TestTrue(TEXT("create owned Modeling world"), Owner.IsValid()))
	{
		return false;
	}
	ADynamicMeshActor* Actor = Owner.SpawnCubeActor(
		TEXT("SA008_CarrierRevision"),
		FVector::ZeroVector);
	if (!TestNotNull(TEXT("spawn revision carrier"), Actor))
	{
		return false;
	}
	const FString SourceId = TEXT("uai://mesh/revision-carrier");
	auto ReadRevision =
		[&]() -> TOptional<FUeAgentStateRevision>
		{
			TOptional<FUeAgentStateRevision> Revision;
			FExecutionMutationError Error;
			TestTrue(
				TEXT("read Modeling carrier revision"),
				TryReadModelingGeometryCarrierRevision(
					SourceId,
					*Actor,
					Revision,
					Error));
			return Revision;
		};

	const TOptional<FUeAgentStateRevision> Initial = ReadRevision();
	const TOptional<FUeAgentStateRevision> StableRead = ReadRevision();
	if (!Initial.IsSet() || !StableRead.IsSet())
	{
		return false;
	}
	TestTrue(
		TEXT("unchanged carrier has a stable revision"),
		StableRead.GetValue() == Initial.GetValue());
	TestEqual(
		TEXT("revision preserves the public source id"),
		Initial->GetSourceId(),
		SourceId);
	TestTrue(
		TEXT("revision sequence is exactly representable in JSON"),
		Initial->GetSequence()
			<= FUeAgentStateRevisionJson::MaxExactJsonSequence);

	Actor->SetActorLocation(FVector(25.0, 0.0, 0.0));
	const TOptional<FUeAgentStateRevision> AfterTransform =
		ReadRevision();
	if (!AfterTransform.IsSet())
	{
		return false;
	}
	TestEqual(
		TEXT("transform change preserves carrier generation"),
		AfterTransform->GetGeneration(),
		Initial->GetGeneration());
	TestNotEqual(
		TEXT("transform change advances semantic revision"),
		AfterTransform->GetSequence(),
		Initial->GetSequence());

	UDynamicMeshComponent* Component =
		Actor->GetDynamicMeshComponent();
	UDynamicMesh* DynamicMesh =
		Component ? Component->GetDynamicMesh() : nullptr;
	if (!TestNotNull(TEXT("carrier dynamic mesh exists"), DynamicMesh))
	{
		return false;
	}
	DynamicMesh->EditMesh(
		[](FDynamicMesh3& Mesh)
		{
			Mesh.SetVertex(
				0,
				Mesh.GetVertex(0) + FVector3d(1.0, 0.0, 0.0));
		});
	const TOptional<FUeAgentStateRevision> AfterGeometry =
		ReadRevision();
	if (!AfterGeometry.IsSet())
	{
		return false;
	}
	TestNotEqual(
		TEXT("geometry change advances semantic revision"),
		AfterGeometry->GetSequence(),
		AfterTransform->GetSequence());

	DynamicMesh->EditMesh(
		[](FDynamicMesh3& Mesh)
		{
			Mesh.RemoveTriangle(0, false, false);
		});
	const TOptional<FUeAgentStateRevision> AfterTopology =
		ReadRevision();
	if (!AfterTopology.IsSet())
	{
		return false;
	}
	TestNotEqual(
		TEXT("topology change advances semantic revision"),
		AfterTopology->GetSequence(),
		AfterGeometry->GetSequence());

	FDynamicMesh3 PersistedEquivalent;
	bool bPersistedRoundTripSucceeded = false;
	DynamicMesh->ProcessMesh(
		[&](const FDynamicMesh3& Mesh)
		{
			FDynamicMesh3 Copy(Mesh);
			TArray<uint8> Serialized;
			FMemoryWriter Writer(Serialized, true);
			Copy.Serialize(Writer);
			const FCustomVersionContainer CustomVersions =
				Writer.GetCustomVersions();
			Writer.Close();
			if (Writer.IsError())
			{
				return;
			}
			FMemoryReader Reader(Serialized, true);
			Reader.SetCustomVersions(CustomVersions);
			PersistedEquivalent.Serialize(Reader);
			Reader.Close();
			bPersistedRoundTripSucceeded = !Reader.IsError();
		});
	if (!TestTrue(
			TEXT("sparse topology survives a persistent compact roundtrip"),
			bPersistedRoundTripSucceeded))
	{
		return false;
	}
	DynamicMesh->EditMesh(
		[&](FDynamicMesh3& Mesh)
		{
			Mesh = MoveTemp(PersistedEquivalent);
		});
	const TOptional<FUeAgentStateRevision> AfterEquivalentCompaction =
		ReadRevision();
	if (!AfterEquivalentCompaction.IsSet())
	{
		return false;
	}
	TestTrue(
		TEXT("persistent compaction preserves the semantic carrier revision"),
		AfterEquivalentCompaction.GetValue()
			== AfterTopology.GetValue());

	DynamicMesh->EditMesh(
		[](FDynamicMesh3& Mesh)
		{
			FDynamicMeshAttributeSet* Attributes = Mesh.Attributes();
			Attributes->SetNumUVLayers(1);
			FDynamicMeshUVOverlay* Uv = Attributes->PrimaryUV();
			for (const int32 TriangleId : Mesh.TriangleIndicesItr())
			{
				const UE::Geometry::FIndex3i Triangle =
					Mesh.GetTriangle(TriangleId);
				UE::Geometry::FIndex3i Elements;
				for (int32 Corner = 0; Corner < 3; ++Corner)
				{
					const int32 ElementId = Uv->AppendElement(
						FVector2f(
							static_cast<float>(Corner) * 0.25f,
							0.5f));
					Uv->SetParentVertex(
						ElementId,
						Triangle[Corner]);
					Elements[Corner] = ElementId;
				}
				Uv->SetTriangle(TriangleId, Elements);
				break;
			}
		});
	const TOptional<FUeAgentStateRevision> AfterUv = ReadRevision();
	if (!AfterUv.IsSet())
	{
		return false;
	}
	TestNotEqual(
		TEXT("UV change advances semantic revision"),
		AfterUv->GetSequence(),
		AfterEquivalentCompaction->GetSequence());

	DynamicMesh->EditMesh(
		[](FDynamicMesh3& Mesh)
		{
			FDynamicMeshNormalOverlay* Normals =
				Mesh.Attributes()->PrimaryNormals();
			for (const int32 TriangleId : Mesh.TriangleIndicesItr())
			{
				const UE::Geometry::FIndex3i Triangle =
					Mesh.GetTriangle(TriangleId);
				UE::Geometry::FIndex3i Elements;
				for (int32 Corner = 0; Corner < 3; ++Corner)
				{
					const int32 ElementId = Normals->AppendElement(
						FVector3f(0.0f, 0.0f, 1.0f));
					Normals->SetParentVertex(
						ElementId,
						Triangle[Corner]);
					Elements[Corner] = ElementId;
				}
				Normals->SetTriangle(TriangleId, Elements);
				break;
			}
		});
	const TOptional<FUeAgentStateRevision> AfterNormals =
		ReadRevision();
	if (!AfterNormals.IsSet())
	{
		return false;
	}
	TestNotEqual(
		TEXT("normal change advances semantic revision"),
		AfterNormals->GetSequence(),
		AfterUv->GetSequence());

	DynamicMesh->EditMesh(
		[](FDynamicMesh3& Mesh)
		{
			FDynamicMeshAttributeSet* Attributes = Mesh.Attributes();
			Attributes->EnableMaterialID();
			for (const int32 TriangleId : Mesh.TriangleIndicesItr())
			{
				Attributes->GetMaterialID()->SetValue(TriangleId, 1);
				break;
			}
		});
	const TOptional<FUeAgentStateRevision> AfterMaterialAssignment =
		ReadRevision();
	if (!AfterMaterialAssignment.IsSet())
	{
		return false;
	}
	TestNotEqual(
		TEXT("material assignment advances semantic revision"),
		AfterMaterialAssignment->GetSequence(),
		AfterNormals->GetSequence());

	UMaterial* Material = NewObject<UMaterial>(
		Owner.Package,
		TEXT("M_SA008_CarrierRevision"),
		RF_Transactional);
	Component->SetMaterial(0, Material);
	const TOptional<FUeAgentStateRevision> AfterMaterial =
		ReadRevision();
	if (!AfterMaterial.IsSet())
	{
		return false;
	}
	TestNotEqual(
		TEXT("material change advances semantic revision"),
		AfterMaterial->GetSequence(),
		AfterMaterialAssignment->GetSequence());

	UBodySetup* BodySetup = Component->GetBodySetup();
	if (!TestNotNull(TEXT("carrier body setup exists"), BodySetup))
	{
		return false;
	}
	BodySetup->AggGeom.BoxElems.Add(FKBoxElem(10.0f));
	const TOptional<FUeAgentStateRevision> AfterCollision =
		ReadRevision();
	if (!AfterCollision.IsSet())
	{
		return false;
	}
	TestNotEqual(
		TEXT("collision change advances semantic revision"),
		AfterCollision->GetSequence(),
		AfterMaterial->GetSequence());

	TOptional<FUeAgentStateRevision> UnsupportedRevision;
	FExecutionMutationError UnsupportedError;
	TestFalse(
		TEXT("non-carrier revision is explicitly unsupported"),
		TryReadModelingGeometryCarrierRevision(
			SourceId,
			*Material,
			UnsupportedRevision,
			UnsupportedError));
	TestEqual(
		TEXT("non-carrier revision returns a stable error"),
		UnsupportedError.GetCode(),
		FString(TEXT("modeling_geometry_carrier_kind_unsupported")));
	return !HasAnyErrors();
}

#include "UeAgentSimpleAuthoringModelingTopologyProductionTests.inl"
#include "UeAgentSimpleAuthoringModelingAttributeCollisionProductionTests.inl"

namespace
{
	using UE::UAI::Tests::SimpleAuthoring::E4::
		CountActiveTransactions;
	using UE::UAI::Tests::SimpleAuthoring::E4::
		CountCompletedJournals;
	using UE::UAI::Tests::SimpleAuthoring::E4::
		FOwnedReloadableAssetPackage;
	using UE::UAI::Tests::SimpleAuthoring::E4::
		FOwnedReloadableWorldCatalog;
	using UE::UAI::Tests::SimpleAuthoring::E4::
		FOwnedReloadableWorldPackage;

	TSharedPtr<FJsonObject> FindModelingOperationRow(
		const TSharedPtr<FJsonObject>& Fixture,
		const FString& OperationKey)
	{
		if (!Fixture.IsValid())
		{
			return nullptr;
		}
		for (const TSharedPtr<FJsonValue>& Value
			: Fixture->GetArrayField(TEXT("descriptors")))
		{
			const TSharedPtr<FJsonObject> Row = Value->AsObject();
			if (Row.IsValid()
				&& Row->GetStringField(TEXT("operation_key"))
					== OperationKey)
			{
				return Row;
			}
		}
		return nullptr;
	}

	class FReloadableModelingReferenceResolver final
		: public IUeAgentModelingGeometryEffectReferenceResolver
	{
	public:
		FReloadableModelingReferenceResolver(
			FOwnedReloadableAssetPackage& InOwner,
			const bool bInReloadBeforeRead)
			: Owner(InOwner)
			, bReloadBeforeRead(bInReloadBeforeRead)
		{
		}

		bool TryResolveReference(
			UWorld& World,
			const FString& CanonicalRef,
			UObject*& OutObject,
			FExecutionMutationError& OutError) override
		{
			++ResolveCount;
			FString ReloadError;
			if (bReloadBeforeRead
				&& !Owner.TryReload(CanonicalRef, ReloadError))
			{
				OutObject = nullptr;
				OutError = FExecutionMutationError(
					ReloadError,
					CanonicalRef);
				return false;
			}

			OutObject = Owner.FindTarget(CanonicalRef);
			if (!::IsValid(OutObject))
			{
				OutObject = nullptr;
				OutError = FExecutionMutationError(
					TEXT("sa008_modeling_asset_reference_unresolved"),
					CanonicalRef);
				return false;
			}
			OutError = FExecutionMutationError();
			return true;
		}

		int32 ResolveCount = 0;

	private:
		FOwnedReloadableAssetPackage& Owner;
		bool bReloadBeforeRead = false;
	};

	ADynamicMeshActor* FindE4ModelingActor(
		UWorld& World,
		const FString& Label);

	class FWorldModelingReferenceResolver final
		: public IUeAgentModelingGeometryEffectReferenceResolver
	{
	public:
		FWorldModelingReferenceResolver(
			FString InTargetRef,
			FString InActorLabel)
			: TargetRef(MoveTemp(InTargetRef))
			, ActorLabel(MoveTemp(InActorLabel))
		{
		}

		bool TryResolveReference(
			UWorld& World,
			const FString& CanonicalRef,
			UObject*& OutObject,
			FExecutionMutationError& OutError) override
		{
			++ResolveCount;
			OutObject = nullptr;
			if (CanonicalRef != TargetRef)
			{
				OutError = FExecutionMutationError(
					TEXT("sa008_modeling_world_reference_unbound"),
					CanonicalRef);
				return false;
			}
			OutObject = FindE4ModelingActor(World, ActorLabel);
			if (!OutObject)
			{
				OutError = FExecutionMutationError(
					TEXT("sa008_modeling_world_actor_unresolved"),
					CanonicalRef);
				return false;
			}
			OutError = FExecutionMutationError();
			return true;
		}

		int32 ResolveCount = 0;

	private:
		FString TargetRef;
		FString ActorLabel;
	};

	ADynamicMeshActor* SpawnE4ModelingCubeActor(
		UWorld& World,
		const FString& Label)
	{
		FActorSpawnParameters SpawnParameters;
		SpawnParameters.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		SpawnParameters.ObjectFlags |= RF_Transactional;
		ADynamicMeshActor* Actor =
			World.SpawnActor<ADynamicMeshActor>(
				FVector(1000.0, 2000.0, 300.0),
				FRotator(0.0, 37.0, 0.0),
				SpawnParameters);
		UDynamicMeshComponent* Component = Actor
			? Actor->GetDynamicMeshComponent()
			: nullptr;
		UDynamicMesh* DynamicMesh = Component
			? Component->GetDynamicMesh()
			: nullptr;
		if (!Actor || !DynamicMesh)
		{
			if (Actor)
			{
				Actor->Destroy();
			}
			return nullptr;
		}
		Actor->SetActorLabel(Label);
		Actor->SetActorScale3D(FVector(1.0, 2.0, 0.5));
		DynamicMesh->EditMesh(
			[](FDynamicMesh3& Mesh)
			{
				BuildCube(Mesh);
			});
		return Actor;
	}

	ADynamicMeshActor* FindE4ModelingActor(
		UWorld& World,
		const FString& Label)
	{
		ADynamicMeshActor* Match = nullptr;
		for (TActorIterator<ADynamicMeshActor> It(&World); It; ++It)
		{
			if (It->GetActorLabel() != Label)
			{
				continue;
			}
			if (Match)
			{
				return nullptr;
			}
			Match = *It;
		}
		return Match;
	}

	bool RunStaticNormalsOwnedPackageCase(
		FAutomationTestBase* Test,
		const FString& Mode,
		const bool bPreserveExistingHardEdges,
		const TOptional<double>& AngleDegrees,
		const FString& Scope)
	{
		const TSharedPtr<FJsonObject> Fixture =
			LoadTopologyFixture(Test);
		const TSharedPtr<FJsonObject> NormalRow =
			FindModelingOperationRow(
				Fixture,
				TEXT("recompute_normals"));
		if (!Test->TestNotNull(
				*(Scope + TEXT(" descriptor row exists")),
				NormalRow.Get()))
		{
			return false;
		}

		TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
		FString RegistryError;
		const FUeAgentSimpleAuthoringCommandRoute* Route = FindRoute(
			NormalRow->GetStringField(TEXT("capability_id")));
		if (!Test->TestTrue(
				*(Scope + TEXT(" descriptors compose")),
				TryCreateSimpleAuthoringDescriptorRegistry(
					Registry,
					RegistryError))
			|| !Test->TestNotNull(
				*(Scope + TEXT(" has one exact route")),
				Route)
			|| !Registry.IsValid())
		{
			return false;
		}

		const FString TargetRef = TEXT("uai://mesh/fixture-target");
		FString OwnedRoot;
		{
			FOwnedModelingWorld WorldOwner;
			FOwnedReloadableAssetPackage AssetOwner(Scope);
			OwnedRoot = AssetOwner.Root;
			if (!Test->TestTrue(
					*(Scope + TEXT(" owned asset package is valid")),
					WorldOwner.IsValid() && AssetOwner.IsValid()))
			{
				return false;
			}

			ADynamicMeshActor* SourceActor =
				WorldOwner.SpawnCubeActor(
					TEXT("SA008_") + Scope + TEXT("_Source"),
					FVector::ZeroVector,
					FRotator::ZeroRotator,
					FVector::OneVector);
			UDynamicMeshComponent* SourceComponent = SourceActor
				? SourceActor->GetDynamicMeshComponent()
				: nullptr;
			UDynamicMesh* SourceMesh = SourceComponent
				? SourceComponent->GetDynamicMesh()
				: nullptr;
			if (SourceMesh && bPreserveExistingHardEdges)
			{
				SeedMixedHardEdgeNormals(*SourceMesh);
			}
			UStaticMesh* StaticTarget = SourceMesh
				? CreateTopologyStaticCarrier(
					*AssetOwner.Package,
					*SourceMesh)
				: nullptr;
			if (!Test->TestNotNull(
					*(Scope + TEXT(" static mesh exists")),
					StaticTarget))
			{
				return false;
			}
			AssetOwner.BindTarget(
				TargetRef,
				StaticTarget->GetFName());

			const TSharedPtr<FJsonObject> Payload =
				BuildAttributeCollisionPayload(
					NormalRow,
					*StaticTarget);
			if (Payload.IsValid())
			{
				Payload->SetStringField(TEXT("mode"), Mode);
				Payload->SetBoolField(
					TEXT("preserve_existing_hard_edges"),
					bPreserveExistingHardEdges);
				if (AngleDegrees.IsSet())
				{
					Payload->SetNumberField(
						TEXT("angle_degrees"),
						AngleDegrees.GetValue());
				}
			}
			FString PayloadJson;
			if (!Test->TestTrue(
					*(Scope + TEXT(" payload serializes")),
					Payload.IsValid()
						&& Serialize(Payload, PayloadJson)))
			{
				return false;
			}

			FAttributeCollisionReadback Before;
			if (!Test->TestTrue(
					*(Scope + TEXT(" reads source static mesh")),
					ReadAttributeCollisionStatic(
						*StaticTarget,
						Before)))
			{
				return false;
			}
			if (bPreserveExistingHardEdges
				&& !Test->TestTrue(
					*(Scope + TEXT(" mixed hard-edge seed survives static projection")),
					HasCompleteMixedHardEdgeSeed(
						Before.NormalObservation)))
			{
				return false;
			}
			AssetOwner.Package->SetDirtyFlag(false);

			const TSharedRef<
				FReloadableModelingReferenceResolver,
				ESPMode::ThreadSafe> SourceResolver = MakeShared<
					FReloadableModelingReferenceResolver,
					ESPMode::ThreadSafe>(AssetOwner, false);
			const TSharedRef<
				FReloadableModelingReferenceResolver,
				ESPMode::ThreadSafe> PersistedResolver = MakeShared<
					FReloadableModelingReferenceResolver,
					ESPMode::ThreadSafe>(AssetOwner, true);

			FUeAgentSimpleAuthoringDurableServiceConfig DurableConfig;
			DurableConfig.JournalRoot = AssetOwner.JournalRoot;
			DurableConfig.StableJournalOwnerId = TEXT("test:modeling:durable");
			DurableConfig.PackageBackend = MakeShared<
				FPackageAssetDurableBackend,
				ESPMode::ThreadSafe>(AssetOwner.PackageRoot);
			TSharedPtr<IUeAgentSimpleAuthoringAssetDurableService>
				Durable;
			FUeAgentSimpleAuthoringProductionError DurableError;
			if (!Test->TestTrue(
					*(Scope + TEXT(" real C8D service composes")),
					FUeAgentSimpleAuthoringDurableServiceFactory::
						TryCreate(
							DurableConfig,
							Durable,
							DurableError))
				|| !Durable.IsValid())
			{
				Test->AddError(
					Scope + TEXT(":durable_invalid:")
						+ DurableError.Code);
				return false;
			}

			TSharedPtr<FUeAgentSimpleAuthoringApplication> Application;
			FString ApplicationError;
			if (!Test->TestTrue(
					*(Scope + TEXT(" application composes")),
					TryCreateTopologyApplication(
						WorldOwner,
						Registry,
						SourceResolver,
						PersistedResolver,
						Durable.ToSharedRef(),
						*Route,
						Application,
						ApplicationError)))
			{
				Test->AddError(
					Scope + TEXT(":composition_failed:")
						+ ApplicationError);
				return false;
			}

			const FUeAgentOperationResult Result = Application->Submit(
				FUeAgentOperationRequest(
					Route->DescriptorIdentity,
					TEXT("request:") + Scope,
					TEXT("idempotency:") + Scope,
					FUeAgentContractValue(PayloadJson)),
				FUeAgentCallerContext(
					TEXT("caller:") + Scope,
					{TEXT("uai:modeling-geometry:write")},
					TEXT("correlation:") + Scope),
				MakeShared<FNeverCancelled>());
			if (!Test->TestTrue(
					*(Scope + TEXT(" completes after reload readback")),
					IsComplete(Result)))
			{
				Test->AddError(Result.GetValue().GetJsonValue());
				return false;
			}

			UStaticMesh* Reloaded = Cast<UStaticMesh>(
				AssetOwner.FindTarget(TargetRef));
			FAttributeCollisionReadback After;
			Test->TestTrue(
				*(Scope + TEXT(" reloads the asset owner")),
				AssetOwner.WasReloaded());
			Test->TestTrue(
				*(Scope + TEXT(" collects the original asset")),
				AssetOwner.WasOriginalObjectCollected());
			Test->TestTrue(
				*(Scope + TEXT(" publishes the owned package")),
				IFileManager::Get().FileExists(
					*AssetOwner.HeaderPath()));
			if (Test->TestNotNull(
					*(Scope + TEXT(" reloaded static mesh is found")),
					Reloaded))
			{
				Test->TestTrue(
					*(Scope + TEXT(" reloaded static mesh is readable")),
					ReadAttributeCollisionStatic(*Reloaded, After));
				if (Mode == TEXT("area_angle_weighted")
					&& bPreserveExistingHardEdges)
				{
					Test->TestTrue(
						*(Scope
							+ TEXT(" preserves mixed hard-edge topology and recomputes directions")),
						HasCompletePreservedHardEdgeObservation(
							Before.NormalObservation,
							After.NormalObservation));
				}
				else if (Mode == TEXT("area_angle_weighted"))
				{
					Test->TestTrue(
						*(Scope
							+ TEXT(" preserves weighted per-vertex normals")),
						IsCompleteAreaAnglePerVertexNormalObservation(
							After.NormalObservation));
				}
				else if (Mode == TEXT("angle_threshold")
					&& AngleDegrees.IsSet())
				{
					Test->TestTrue(
						*(Scope + TEXT(" preserves threshold split topology")),
						IsCompleteAngleThresholdNormalObservation(
							After.NormalObservation,
							AngleDegrees.GetValue()));
				}
			}
			Test->TestTrue(
				*(Scope + TEXT(" uses persisted resolver")),
				PersistedResolver->ResolveCount >= 1);
			Test->TestEqual(
				*(Scope + TEXT(" archives one verified journal")),
				CountCompletedJournals(AssetOwner.JournalRoot),
				1);
			Test->TestEqual(
				*(Scope + TEXT(" leaves no active transaction")),
				CountActiveTransactions(AssetOwner.JournalRoot),
				0);
		}
		Test->TestFalse(
			*(Scope + TEXT(" removes all owned disk artifacts")),
			IFileManager::Get().DirectoryExists(*OwnedRoot));
		return !Test->HasAnyErrors();
	}

	bool RunDynamicNormalsOwnedWorldCase(
		FAutomationTestBase* Test,
		const FString& Mode,
		const bool bPreserveExistingHardEdges,
		const TOptional<double>& AngleDegrees,
		const FString& Scope)
	{
		const TSharedPtr<FJsonObject> Fixture =
			LoadTopologyFixture(Test);
		const TSharedPtr<FJsonObject> NormalRow =
			FindModelingOperationRow(
				Fixture,
				TEXT("recompute_normals"));
		if (!Test->TestNotNull(
				*(Scope + TEXT(" descriptor row exists")),
				NormalRow.Get()))
		{
			return false;
		}

		TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
		FString RegistryError;
		const FUeAgentSimpleAuthoringCommandRoute* Route = FindRoute(
			NormalRow->GetStringField(TEXT("capability_id")));
		if (!Test->TestTrue(
				*(Scope + TEXT(" descriptors compose")),
				TryCreateSimpleAuthoringDescriptorRegistry(
					Registry,
					RegistryError))
			|| !Test->TestNotNull(
				*(Scope + TEXT(" has one exact route")),
				Route)
			|| !Registry.IsValid())
		{
			return false;
		}

		const FString TargetRef = TEXT("uai://mesh/fixture-target");
		const FString ActorLabel = TEXT("SA008_") + Scope + TEXT("_Target");
		FString OwnedRoot;
		{
			FOwnedReloadableWorldPackage Owner(Scope, Scope);
			OwnedRoot = Owner.Root;
			if (!Test->TestTrue(
					*(Scope + TEXT(" owned World is valid")),
					Owner.IsValid()))
			{
				return false;
			}
			ADynamicMeshActor* InitialActor =
				SpawnE4ModelingCubeActor(*Owner.World, ActorLabel);
			if (!Test->TestNotNull(
					*(Scope + TEXT(" dynamic actor exists")),
					InitialActor))
			{
				return false;
			}
			if (bPreserveExistingHardEdges)
			{
				UDynamicMeshComponent* InitialComponent =
					InitialActor->GetDynamicMeshComponent();
				UDynamicMesh* InitialMesh = InitialComponent
					? InitialComponent->GetDynamicMesh()
					: nullptr;
				if (!Test->TestNotNull(
						*(Scope + TEXT(" dynamic mesh exists")),
						InitialMesh))
				{
					return false;
				}
				SeedMixedHardEdgeNormals(*InitialMesh);
			}

			const TSharedPtr<FJsonObject> Payload =
				BuildAttributeCollisionPayload(
					NormalRow,
					*InitialActor);
			if (Payload.IsValid())
			{
				Payload->SetStringField(TEXT("mode"), Mode);
				Payload->SetBoolField(
					TEXT("preserve_existing_hard_edges"),
					bPreserveExistingHardEdges);
				if (AngleDegrees.IsSet())
				{
					Payload->SetNumberField(
						TEXT("angle_degrees"),
						AngleDegrees.GetValue());
				}
			}
			FString PayloadJson;
			if (!Test->TestTrue(
					*(Scope + TEXT(" payload serializes")),
					Payload.IsValid()
						&& Serialize(Payload, PayloadJson)))
			{
				return false;
			}

			FAttributeCollisionReadback Before;
			if (!Test->TestTrue(
					*(Scope + TEXT(" reads source actor")),
					ReadAttributeCollisionDynamic(
						*InitialActor,
						Before)))
			{
				return false;
			}
			if (bPreserveExistingHardEdges
				&& !Test->TestTrue(
					*(Scope + TEXT(" mixed hard-edge seed reads from source actor")),
					HasCompleteMixedHardEdgeSeed(
						Before.NormalObservation)))
			{
				return false;
			}
			Owner.Package->SetDirtyFlag(false);

			const TSharedRef<
				FOwnedReloadableWorldCatalog,
				ESPMode::ThreadSafe> SourceCatalog = MakeShared<
					FOwnedReloadableWorldCatalog,
					ESPMode::ThreadSafe>(Owner, false);
			const TSharedRef<
				FOwnedReloadableWorldCatalog,
				ESPMode::ThreadSafe> PersistedCatalog = MakeShared<
					FOwnedReloadableWorldCatalog,
					ESPMode::ThreadSafe>(Owner, true);
			const TSharedRef<
				FWorldModelingReferenceResolver,
				ESPMode::ThreadSafe> SourceResolver = MakeShared<
					FWorldModelingReferenceResolver,
					ESPMode::ThreadSafe>(TargetRef, ActorLabel);
			const TSharedRef<
				FWorldModelingReferenceResolver,
				ESPMode::ThreadSafe> PersistedResolver = MakeShared<
					FWorldModelingReferenceResolver,
					ESPMode::ThreadSafe>(TargetRef, ActorLabel);

			FUeAgentSimpleAuthoringDurableServiceConfig DurableConfig;
			DurableConfig.JournalRoot = Owner.JournalRoot;
			DurableConfig.StableJournalOwnerId = TEXT("test:modeling:durable");
			DurableConfig.PackageBackend = MakeShared<
				FPackageAssetDurableBackend,
				ESPMode::ThreadSafe>(Owner.PackageRoot);
			TSharedPtr<IUeAgentSimpleAuthoringAssetDurableService>
				Durable;
			FUeAgentSimpleAuthoringProductionError DurableError;
			if (!Test->TestTrue(
					*(Scope + TEXT(" real World C8D service composes")),
					FUeAgentSimpleAuthoringDurableServiceFactory::
						TryCreate(
							DurableConfig,
							Durable,
							DurableError))
				|| !Durable.IsValid())
			{
				Test->AddError(
					Scope + TEXT(":durable_invalid:")
						+ DurableError.Code);
				return false;
			}

			const TSharedRef<
				IUeAgentWorldSimpleEffectWorldCatalog,
				ESPMode::ThreadSafe> SourceWorldCatalog = SourceCatalog;
			const TSharedRef<
				IUeAgentWorldSimpleEffectWorldCatalog,
				ESPMode::ThreadSafe> PersistedWorldCatalog =
					PersistedCatalog;
			const TSharedRef<
				IUeAgentModelingGeometryEffectReferenceResolver,
				ESPMode::ThreadSafe> SourceReferenceResolver =
					SourceResolver;
			const TSharedRef<
				IUeAgentModelingGeometryEffectReferenceResolver,
				ESPMode::ThreadSafe> PersistedReferenceResolver =
					PersistedResolver;
			TSharedPtr<FUeAgentSimpleAuthoringApplication> Application;
			FString ApplicationError;
			if (!Test->TestTrue(
					*(Scope + TEXT(" application composes")),
					TryCreateModelingApplication(
						Registry,
						SourceWorldCatalog,
						PersistedWorldCatalog,
						SourceReferenceResolver,
						PersistedReferenceResolver,
						Durable.ToSharedRef(),
						*Route,
						Owner.WorldIdentity,
						Owner.LevelIdentity,
						Owner.ContextRevision,
						Scope,
						Application,
						ApplicationError)))
			{
				Test->AddError(
					Scope + TEXT(":composition_failed:")
						+ ApplicationError);
				return false;
			}

			const FUeAgentOperationResult Result = Application->Submit(
				FUeAgentOperationRequest(
					Route->DescriptorIdentity,
					TEXT("request:") + Scope,
					TEXT("idempotency:") + Scope,
					FUeAgentContractValue(PayloadJson)),
				FUeAgentCallerContext(
					TEXT("caller:") + Scope,
					{TEXT("uai:modeling-geometry:write")},
					TEXT("correlation:") + Scope),
				MakeShared<FNeverCancelled>());
			if (!Test->TestTrue(
					*(Scope + TEXT(" completes after World reload")),
					IsComplete(Result)))
			{
				Test->AddError(Result.GetValue().GetJsonValue());
				return false;
			}

			ADynamicMeshActor* ReloadedActor = Owner.World.IsValid()
				? FindE4ModelingActor(*Owner.World, ActorLabel)
				: nullptr;
			FAttributeCollisionReadback After;
			Test->TestTrue(
				*(Scope + TEXT(" reloads the World owner")),
				Owner.WasReloaded());
			Test->TestTrue(
				*(Scope + TEXT(" collects the original World")),
				Owner.WasOriginalWorldCollected());
			Test->TestTrue(
				*(Scope + TEXT(" publishes the owned map")),
				IFileManager::Get().FileExists(*Owner.HeaderPath()));
			if (Test->TestNotNull(
					*(Scope + TEXT(" reloaded actor is found")),
					ReloadedActor))
			{
				Test->TestTrue(
					*(Scope + TEXT(" reloaded actor is readable")),
					ReadAttributeCollisionDynamic(
						*ReloadedActor,
						After));
				if (Mode == TEXT("area_angle_weighted")
					&& bPreserveExistingHardEdges)
				{
					Test->TestTrue(
						*(Scope
							+ TEXT(" preserves mixed hard-edge topology and recomputes directions")),
						HasCompletePreservedHardEdgeObservation(
							Before.NormalObservation,
							After.NormalObservation));
				}
				else if (Mode == TEXT("area_angle_weighted"))
				{
					Test->TestTrue(
						*(Scope
							+ TEXT(" preserves weighted per-vertex normals")),
						IsCompleteAreaAnglePerVertexNormalObservation(
							After.NormalObservation));
				}
				else if (Mode == TEXT("angle_threshold")
					&& AngleDegrees.IsSet())
				{
					Test->TestTrue(
						*(Scope + TEXT(" preserves threshold split topology")),
						IsCompleteAngleThresholdNormalObservation(
							After.NormalObservation,
							AngleDegrees.GetValue()));
				}
			}
			Test->TestTrue(
				*(Scope + TEXT(" uses persisted resolver")),
				PersistedResolver->ResolveCount >= 1);
			Test->TestEqual(
				*(Scope + TEXT(" reloads exactly once")),
				PersistedCatalog->EnumerateCount,
				1);
			Test->TestEqual(
				*(Scope + TEXT(" archives one verified journal")),
				CountCompletedJournals(Owner.JournalRoot),
				1);
			Test->TestEqual(
				*(Scope + TEXT(" leaves no active transaction")),
				CountActiveTransactions(Owner.JournalRoot),
				0);
		}
		Test->TestFalse(
			*(Scope + TEXT(" removes all owned disk artifacts")),
			IFileManager::Get().DirectoryExists(*OwnedRoot));
		return !Test->HasAnyErrors();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringModelingOwnedPackageTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E4."
		"ModelingOwnedPackage",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentSimpleAuthoringModelingOwnedPackageTest::RunTest(
	const FString& Parameters)
{
	const FString Capability = TEXT("mesh.primitive.box.create");
	const FString TargetRef = TEXT("SA008_Modeling_E4_Box");
	FString OwnedRoot;
	{
		FOwnedReloadableWorldPackage Owner(
			TEXT("ModelingE4"),
			TEXT("ModelingE4"));
		OwnedRoot = Owner.Root;
		if (!TestTrue(
				TEXT("owned durable Modeling world is valid"),
				Owner.IsValid()))
		{
			return false;
		}

		FUeAgentSimpleAuthoringDurableServiceConfig DurableConfig;
		DurableConfig.JournalRoot = Owner.JournalRoot;
		DurableConfig.StableJournalOwnerId = TEXT("test:modeling:durable");
		DurableConfig.PackageBackend = MakeShared<
			FPackageAssetDurableBackend,
			ESPMode::ThreadSafe>(Owner.PackageRoot);
		TSharedPtr<IUeAgentSimpleAuthoringAssetDurableService>
			Durable;
		FUeAgentSimpleAuthoringProductionError DurableError;
		if (!TestTrue(
				TEXT("real Modeling package C8D service composes"),
				FUeAgentSimpleAuthoringDurableServiceFactory::TryCreate(
					DurableConfig,
					Durable,
					DurableError))
			|| !Durable.IsValid())
		{
			AddError(
				TEXT("sa008_modeling_e4_durable_invalid:")
					+ DurableError.Code);
			return false;
		}

		TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
		FString RegistryError;
		const FUeAgentSimpleAuthoringCommandRoute* Route =
			FindRoute(Capability);
		if (!TestTrue(
				TEXT("Simple Authoring descriptors compose for Modeling E4"),
				TryCreateSimpleAuthoringDescriptorRegistry(
					Registry,
					RegistryError))
			|| !TestNotNull(
				TEXT("Modeling box create has one E4 route"),
				Route)
			|| !Registry.IsValid())
		{
			return false;
		}

		const TSharedRef<
			FOwnedReloadableWorldCatalog,
			ESPMode::ThreadSafe> SourceCatalog = MakeShared<
				FOwnedReloadableWorldCatalog,
				ESPMode::ThreadSafe>(Owner, false);
		const TSharedRef<
			FOwnedReloadableWorldCatalog,
			ESPMode::ThreadSafe> ReloadCatalog = MakeShared<
				FOwnedReloadableWorldCatalog,
				ESPMode::ThreadSafe>(Owner, true);
		const TSharedRef<FExactReferenceResolver, ESPMode::ThreadSafe>
			SourceResolver = MakeShared<
				FExactReferenceResolver,
				ESPMode::ThreadSafe>();
		const TSharedRef<FExactReferenceResolver, ESPMode::ThreadSafe>
			ReloadResolver = MakeShared<
				FExactReferenceResolver,
				ESPMode::ThreadSafe>();

		FUeAgentModelingSimpleAuthoringProviderConfig ProviderConfig;
		ProviderConfig.DescriptorRegistry = Registry;
		ProviderConfig.WorldCatalog = SourceCatalog;
		ProviderConfig.PersistedWorldCatalog = ReloadCatalog;
		ProviderConfig.ReferenceResolver = SourceResolver;
		ProviderConfig.PersistedReferenceResolver = ReloadResolver;
		ProviderConfig.DurableService = Durable;
		ProviderConfig.AbsenceOracle =
			MakeShared<FUnusedAbsenceOracle>();
		ProviderConfig.ReferenceOracle =
			MakeShared<FUnusedReferenceOracle>();
		ProviderConfig.OwnerLeaseId =
			TEXT("owner:sa008-modeling-e4");
		ProviderConfig.Projection.Emplace(
			FUeAgentProjectionProfile(
				TEXT("compact"),
				false,
				false,
				false),
			TArray<FUeAgentArtifactDescriptor>(),
			TArray<FUeAgentProjectionDiagnostic>());
		TSharedPtr<IUeAgentSimpleAuthoringCapabilityProvider> Provider;
		FUeAgentModelingSimpleAuthoringError ProviderError;
		if (!TestTrue(
				TEXT("Modeling E4 family composes"),
				FUeAgentModelingSimpleAuthoringProviderFactory::TryCreate(
					ProviderConfig,
					Provider,
					ProviderError))
			|| !Provider.IsValid())
		{
			return false;
		}

		TSharedPtr<const FUeAgentOperationTicketService> TicketService;
		FUeAgentOperationTicketError TicketError;
		FUeAgentOperationTicketService::TryCreate(
			TEXT("sa008-modeling-e4-integrity"),
			60.0,
			[]() { return 100.0; },
			[]() { return FString(TEXT("ticket:sa008-modeling-e4")); },
			TicketService,
			TicketError);
		TSharedPtr<const FUeAgentResponseProjector> Projector;
		FUeAgentProjectionError ProjectionError;
		FUeAgentResponseProjector::TryCreate(
			MakeShared<FTokenEstimator>(),
			Projector,
			ProjectionError);
		if (!TicketService.IsValid() || !Projector.IsValid())
		{
			return false;
		}

		FUeAgentSimpleAuthoringApplicationConfig ApplicationConfig;
		ApplicationConfig.DescriptorRegistry = Registry;
		ApplicationConfig.ExecutionContext.ProjectInstanceId =
			TEXT("project:sa008-modeling-e4");
		ApplicationConfig.ExecutionContext.EditorInstanceId =
			TEXT("editor:sa008-modeling-e4");
		ApplicationConfig.ExecutionContext.StatePlane =
			EUeAgentStatePlane::AuthoringMemory;
		ApplicationConfig.ExecutionContext.WorldType =
			EUeAgentExecutionWorldType::Preview;
		ApplicationConfig.ExecutionContext.WorldIdentity =
			Owner.WorldIdentity;
		ApplicationConfig.ExecutionContext.LevelIdentity =
			Owner.LevelIdentity;
		ApplicationConfig.ExecutionContext.ContextRevision =
			Owner.ContextRevision;
		ApplicationConfig.ExecutionContext.bResolvedFromCurrent = false;
		ApplicationConfig.ServerSessionId =
			TEXT("session:sa008-modeling-e4");
		ApplicationConfig.RegistryEpoch = 1;

		TSharedPtr<FUeAgentSimpleAuthoringApplication> Application;
		FUeAgentSimpleAuthoringApplicationError ApplicationError;
		if (!TestTrue(
				TEXT("application accepts Modeling E4 route"),
				FUeAgentSimpleAuthoringApplication::TryCreate(
					ApplicationConfig,
					{*Route},
					{Provider.ToSharedRef()},
					TicketService.ToSharedRef(),
					Projector.ToSharedRef(),
					Application,
					ApplicationError))
			|| !Application.IsValid())
		{
			return false;
		}

		FString PayloadJson;
		if (!TestTrue(
				TEXT("Modeling E4 payload serializes"),
				Serialize(
					PrimitivePayload(Capability, TargetRef),
					PayloadJson)))
		{
			return false;
		}
		const FUeAgentOperationResult Result = Application->Submit(
			FUeAgentOperationRequest(
				Route->DescriptorIdentity,
				TEXT("request:sa008-modeling-e4"),
				TEXT("idempotency:sa008-modeling-e4"),
				FUeAgentContractValue(PayloadJson)),
			FUeAgentCallerContext(
				TEXT("caller:sa008-modeling-e4"),
				{TEXT("uai:modeling-geometry:write")},
				TEXT("correlation:sa008-modeling-e4")),
			MakeShared<FNeverCancelled>());
		if (!TestTrue(
				TEXT("Modeling E4 completes after reload readback"),
				IsComplete(Result)))
		{
			AddError(Result.GetValue().GetJsonValue());
			return false;
		}

		ADynamicMeshActor* ReloadedActor = Owner.World.IsValid()
			? FindE4ModelingActor(*Owner.World, TargetRef)
			: nullptr;
		FBox Bounds;
		int32 TriangleCount = 0;
		TestTrue(TEXT("Modeling E4 uses a reloaded world owner"), Owner.WasReloaded());
		TestTrue(TEXT("Modeling E4 collects the original world"), Owner.WasOriginalWorldCollected());
		TestTrue(TEXT("Modeling E4 publishes the owned map"), IFileManager::Get().FileExists(*Owner.HeaderPath()));
		if (TestNotNull(TEXT("reloaded primitive actor is independently found"), ReloadedActor))
		{
			TestTrue(TEXT("reloaded primitive geometry is readable"), ReadBounds(*ReloadedActor, Bounds, TriangleCount));
			TestTrue(TEXT("reloaded primitive preserves the requested bounds"), Bounds.GetSize().Equals(FVector(120.0, 80.0, 40.0), 0.5));
			TestTrue(TEXT("reloaded primitive preserves world placement"), ReloadedActor->GetActorLocation().Equals(FVector(100.0, 200.0, 300.0), 0.01));
		}
		TestEqual(TEXT("Modeling E4 C9 reloads exactly once"), ReloadCatalog->EnumerateCount, 1);
		TestEqual(TEXT("Modeling E4 archives one verified journal"), CountCompletedJournals(Owner.JournalRoot), 1);
		TestEqual(TEXT("Modeling E4 leaves no active transaction"), CountActiveTransactions(Owner.JournalRoot), 0);
	}
	TestFalse(
		TEXT("Modeling E4 removes all owned disk artifacts"),
		IFileManager::Get().DirectoryExists(*OwnedRoot));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringModelingStaticMeshFaceNormalsOwnedPackageTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E4."
		"ModelingStaticMeshFaceNormalsOwnedPackage",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool
FUeAgentSimpleAuthoringModelingStaticMeshFaceNormalsOwnedPackageTest::
	RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture =
		LoadTopologyFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> NormalRow =
		FindModelingOperationRow(Fixture, TEXT("recompute_normals"));
	if (!TestNotNull(
			TEXT("face normals E4 descriptor row exists"),
			NormalRow.Get()))
	{
		return false;
	}

	TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
	FString RegistryError;
	const FString Capability =
		NormalRow->GetStringField(TEXT("capability_id"));
	const FUeAgentSimpleAuthoringCommandRoute* Route =
		FindRoute(Capability);
	if (!TestTrue(
			TEXT("Simple Authoring descriptors compose for face normals E4"),
			TryCreateSimpleAuthoringDescriptorRegistry(
				Registry,
				RegistryError))
		|| !TestNotNull(
			TEXT("face normals E4 has one exact route"),
			Route)
		|| !Registry.IsValid())
	{
		return false;
	}

	const FString TargetRef = TEXT("uai://mesh/fixture-target");
	FString OwnedRoot;
	{
		FOwnedModelingWorld WorldOwner;
		FOwnedReloadableAssetPackage AssetOwner(
			TEXT("ModelingNormalsE4"));
		OwnedRoot = AssetOwner.Root;
		if (!TestTrue(
				TEXT("owned durable face normals asset package is valid"),
				WorldOwner.IsValid() && AssetOwner.IsValid()))
		{
			return false;
		}

		ADynamicMeshActor* SourceActor = WorldOwner.SpawnCubeActor(
			TEXT("SA008_Modeling_Normals_E4_Source"),
			FVector::ZeroVector,
			FRotator::ZeroRotator,
			FVector::OneVector);
		UDynamicMeshComponent* SourceComponent = SourceActor
			? SourceActor->GetDynamicMeshComponent()
			: nullptr;
		UDynamicMesh* SourceMesh = SourceComponent
			? SourceComponent->GetDynamicMesh()
			: nullptr;
		UStaticMesh* StaticTarget = SourceMesh
			? CreateTopologyStaticCarrier(
				*AssetOwner.Package,
				*SourceMesh)
			: nullptr;
		if (!TestNotNull(
				TEXT("owned durable face normals static mesh exists"),
				StaticTarget))
		{
			return false;
		}
		AssetOwner.BindTarget(TargetRef, StaticTarget->GetFName());

		const TSharedPtr<FJsonObject> Payload =
			BuildAttributeCollisionPayload(
				NormalRow,
				*StaticTarget);
		FString PayloadJson;
		if (!TestTrue(
				TEXT("face normals E4 payload serializes"),
				Payload.IsValid()
					&& Serialize(Payload, PayloadJson)))
		{
			return false;
		}

		FAttributeCollisionReadback Before;
		if (!TestTrue(
				TEXT("face normals E4 reads source static mesh"),
				ReadAttributeCollisionStatic(*StaticTarget, Before)))
		{
			return false;
		}
		TestFalse(
			TEXT("source static mesh does not already satisfy face normals"),
			IsCompleteFaceNormalObservation(
				Before.NormalObservation));
		AssetOwner.Package->SetDirtyFlag(false);

		const TSharedRef<
			FReloadableModelingReferenceResolver,
			ESPMode::ThreadSafe> SourceResolver = MakeShared<
				FReloadableModelingReferenceResolver,
				ESPMode::ThreadSafe>(AssetOwner, false);
		const TSharedRef<
			FReloadableModelingReferenceResolver,
			ESPMode::ThreadSafe> PersistedResolver = MakeShared<
				FReloadableModelingReferenceResolver,
				ESPMode::ThreadSafe>(AssetOwner, true);

		FUeAgentSimpleAuthoringDurableServiceConfig DurableConfig;
		DurableConfig.JournalRoot = AssetOwner.JournalRoot;
		DurableConfig.StableJournalOwnerId = TEXT("test:modeling:durable");
		DurableConfig.PackageBackend = MakeShared<
			FPackageAssetDurableBackend,
			ESPMode::ThreadSafe>(AssetOwner.PackageRoot);
		TSharedPtr<IUeAgentSimpleAuthoringAssetDurableService> Durable;
		FUeAgentSimpleAuthoringProductionError DurableError;
		if (!TestTrue(
				TEXT("real face normals package C8D service composes"),
				FUeAgentSimpleAuthoringDurableServiceFactory::TryCreate(
					DurableConfig,
					Durable,
					DurableError))
			|| !Durable.IsValid())
		{
			AddError(
				TEXT("sa008_modeling_normals_e4_durable_invalid:")
					+ DurableError.Code);
			return false;
		}

		TSharedPtr<FUeAgentSimpleAuthoringApplication> Application;
		FString ApplicationError;
		if (!TestTrue(
				TEXT("face normals E4 production application composes"),
				TryCreateTopologyApplication(
					WorldOwner,
					Registry,
					SourceResolver,
					PersistedResolver,
					Durable.ToSharedRef(),
					*Route,
					Application,
					ApplicationError)))
		{
			AddError(
				TEXT("sa008_modeling_normals_e4_composition_failed:")
					+ ApplicationError);
			return false;
		}

		const FUeAgentOperationResult Result = Application->Submit(
			FUeAgentOperationRequest(
				Route->DescriptorIdentity,
				TEXT("request:sa008-modeling-normals-e4"),
				TEXT("idempotency:sa008-modeling-normals-e4"),
				FUeAgentContractValue(PayloadJson)),
			FUeAgentCallerContext(
				TEXT("caller:sa008-modeling-normals-e4"),
				{TEXT("uai:modeling-geometry:write")},
				TEXT("correlation:sa008-modeling-normals-e4")),
			MakeShared<FNeverCancelled>());
		if (!TestTrue(
				TEXT("face normals E4 completes after reload readback"),
				IsComplete(Result)))
		{
			AddError(Result.GetValue().GetJsonValue());
			return false;
		}

		UStaticMesh* Reloaded = Cast<UStaticMesh>(
			AssetOwner.FindTarget(TargetRef));
		FAttributeCollisionReadback After;
		TestTrue(
			TEXT("face normals E4 uses a reloaded asset owner"),
			AssetOwner.WasReloaded());
		TestTrue(
			TEXT("face normals E4 collects the original static mesh"),
			AssetOwner.WasOriginalObjectCollected());
		TestTrue(
			TEXT("face normals E4 publishes the owned asset package"),
			IFileManager::Get().FileExists(*AssetOwner.HeaderPath()));
		if (TestNotNull(
				TEXT("reloaded face normals static mesh is found"),
				Reloaded))
		{
			TestTrue(
				TEXT("reloaded face normals static mesh is readable"),
				ReadAttributeCollisionStatic(*Reloaded, After));
			VerifyAttributeCollisionResult(
				this,
				NormalRow->GetStringField(TEXT("descriptor_id")),
				TEXT("recompute_normals"),
				Payload,
				Before,
				After);
		}
		TestTrue(
			TEXT("face normals E4 C9 uses persisted resolver"),
			PersistedResolver->ResolveCount >= 1);
		TestEqual(
			TEXT("face normals E4 archives one verified journal"),
			CountCompletedJournals(AssetOwner.JournalRoot),
			1);
		TestEqual(
			TEXT("face normals E4 leaves no active transaction"),
			CountActiveTransactions(AssetOwner.JournalRoot),
			0);
	}
	TestFalse(
		TEXT("face normals E4 removes all owned disk artifacts"),
		IFileManager::Get().DirectoryExists(*OwnedRoot));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringModelingDynamicFaceNormalsOwnedWorldTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E4."
		"ModelingDynamicFaceNormalsOwnedWorld",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool
FUeAgentSimpleAuthoringModelingDynamicFaceNormalsOwnedWorldTest::
	RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture =
		LoadTopologyFixture(this);
	const TSharedPtr<FJsonObject> NormalRow =
		FindModelingOperationRow(Fixture, TEXT("recompute_normals"));
	if (!TestNotNull(
			TEXT("dynamic face normals E4 descriptor row exists"),
			NormalRow.Get()))
	{
		return false;
	}

	TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
	FString RegistryError;
	const FUeAgentSimpleAuthoringCommandRoute* Route =
		FindRoute(NormalRow->GetStringField(TEXT("capability_id")));
	if (!TestTrue(
			TEXT("dynamic face normals E4 descriptors compose"),
			TryCreateSimpleAuthoringDescriptorRegistry(
				Registry,
				RegistryError))
		|| !TestNotNull(
			TEXT("dynamic face normals E4 has one exact route"),
			Route)
		|| !Registry.IsValid())
	{
		return false;
	}

	const FString TargetRef = TEXT("uai://mesh/fixture-target");
	const FString ActorLabel =
		TEXT("SA008_Modeling_Dynamic_Normals_E4_Target");
	FString OwnedRoot;
	{
		FOwnedReloadableWorldPackage Owner(
			TEXT("ModelingDynamicNormalsE4"),
			TEXT("ModelingDynamicNormalsE4"));
		OwnedRoot = Owner.Root;
		if (!TestTrue(
				TEXT("owned dynamic face normals world is valid"),
				Owner.IsValid()))
		{
			return false;
		}
		ADynamicMeshActor* InitialActor =
			SpawnE4ModelingCubeActor(*Owner.World, ActorLabel);
		if (!TestNotNull(
				TEXT("owned dynamic face normals actor exists"),
				InitialActor))
		{
			return false;
		}

		const TSharedPtr<FJsonObject> Payload =
			BuildAttributeCollisionPayload(NormalRow, *InitialActor);
		FString PayloadJson;
		if (!TestTrue(
				TEXT("dynamic face normals E4 payload serializes"),
				Payload.IsValid()
					&& Serialize(Payload, PayloadJson)))
		{
			return false;
		}
		FAttributeCollisionReadback Before;
		if (!TestTrue(
				TEXT("dynamic face normals E4 reads source actor"),
				ReadAttributeCollisionDynamic(*InitialActor, Before)))
		{
			return false;
		}
		TestFalse(
			TEXT("source dynamic actor does not already satisfy face normals"),
			IsCompleteFaceNormalObservation(
				Before.NormalObservation));
		Owner.Package->SetDirtyFlag(false);

		const TSharedRef<
			FOwnedReloadableWorldCatalog,
			ESPMode::ThreadSafe> SourceCatalog = MakeShared<
				FOwnedReloadableWorldCatalog,
				ESPMode::ThreadSafe>(Owner, false);
		const TSharedRef<
			FOwnedReloadableWorldCatalog,
			ESPMode::ThreadSafe> PersistedCatalog = MakeShared<
				FOwnedReloadableWorldCatalog,
				ESPMode::ThreadSafe>(Owner, true);
		const TSharedRef<
			FWorldModelingReferenceResolver,
			ESPMode::ThreadSafe> SourceResolver = MakeShared<
				FWorldModelingReferenceResolver,
				ESPMode::ThreadSafe>(TargetRef, ActorLabel);
		const TSharedRef<
			FWorldModelingReferenceResolver,
			ESPMode::ThreadSafe> PersistedResolver = MakeShared<
				FWorldModelingReferenceResolver,
				ESPMode::ThreadSafe>(TargetRef, ActorLabel);

		FUeAgentSimpleAuthoringDurableServiceConfig DurableConfig;
		DurableConfig.JournalRoot = Owner.JournalRoot;
		DurableConfig.StableJournalOwnerId = TEXT("test:modeling:durable");
		DurableConfig.PackageBackend = MakeShared<
			FPackageAssetDurableBackend,
			ESPMode::ThreadSafe>(Owner.PackageRoot);
		TSharedPtr<IUeAgentSimpleAuthoringAssetDurableService> Durable;
		FUeAgentSimpleAuthoringProductionError DurableError;
		if (!TestTrue(
				TEXT("real dynamic normals World C8D service composes"),
				FUeAgentSimpleAuthoringDurableServiceFactory::TryCreate(
					DurableConfig,
					Durable,
					DurableError))
			|| !Durable.IsValid())
		{
			AddError(
				TEXT("sa008_modeling_dynamic_normals_e4_durable_invalid:")
					+ DurableError.Code);
			return false;
		}

		const TSharedRef<
			IUeAgentWorldSimpleEffectWorldCatalog,
			ESPMode::ThreadSafe> SourceWorldCatalog = SourceCatalog;
		const TSharedRef<
			IUeAgentWorldSimpleEffectWorldCatalog,
			ESPMode::ThreadSafe> PersistedWorldCatalog =
				PersistedCatalog;
		const TSharedRef<
			IUeAgentModelingGeometryEffectReferenceResolver,
			ESPMode::ThreadSafe> SourceReferenceResolver =
				SourceResolver;
		const TSharedRef<
			IUeAgentModelingGeometryEffectReferenceResolver,
			ESPMode::ThreadSafe> PersistedReferenceResolver =
				PersistedResolver;
		TSharedPtr<FUeAgentSimpleAuthoringApplication> Application;
		FString ApplicationError;
		if (!TestTrue(
				TEXT("dynamic face normals E4 application composes"),
				TryCreateModelingApplication(
					Registry,
					SourceWorldCatalog,
					PersistedWorldCatalog,
					SourceReferenceResolver,
					PersistedReferenceResolver,
					Durable.ToSharedRef(),
					*Route,
					Owner.WorldIdentity,
					Owner.LevelIdentity,
					Owner.ContextRevision,
					TEXT("sa008-modeling-dynamic-normals-e4"),
					Application,
					ApplicationError)))
		{
			AddError(
				TEXT("sa008_modeling_dynamic_normals_e4_composition_failed:")
					+ ApplicationError);
			return false;
		}

		const FUeAgentOperationResult Result = Application->Submit(
			FUeAgentOperationRequest(
				Route->DescriptorIdentity,
				TEXT("request:sa008-modeling-dynamic-normals-e4"),
				TEXT("idempotency:sa008-modeling-dynamic-normals-e4"),
				FUeAgentContractValue(PayloadJson)),
			FUeAgentCallerContext(
				TEXT("caller:sa008-modeling-dynamic-normals-e4"),
				{TEXT("uai:modeling-geometry:write")},
				TEXT("correlation:sa008-modeling-dynamic-normals-e4")),
			MakeShared<FNeverCancelled>());
		if (!TestTrue(
				TEXT("dynamic face normals E4 completes after World reload"),
				IsComplete(Result)))
		{
			AddError(Result.GetValue().GetJsonValue());
			return false;
		}

		ADynamicMeshActor* ReloadedActor = Owner.World.IsValid()
			? FindE4ModelingActor(*Owner.World, ActorLabel)
			: nullptr;
		FAttributeCollisionReadback After;
		TestTrue(
			TEXT("dynamic face normals E4 reloads the World owner"),
			Owner.WasReloaded());
		TestTrue(
			TEXT("dynamic face normals E4 collects the original World"),
			Owner.WasOriginalWorldCollected());
		TestTrue(
			TEXT("dynamic face normals E4 publishes the owned map"),
			IFileManager::Get().FileExists(*Owner.HeaderPath()));
		if (TestNotNull(
				TEXT("reloaded dynamic face normals actor is found"),
				ReloadedActor))
		{
			TestTrue(
				TEXT("reloaded dynamic face normals actor is readable"),
				ReadAttributeCollisionDynamic(*ReloadedActor, After));
			TestTrue(
				TEXT("reloaded dynamic actor preserves every face normal"),
				IsCompleteFaceNormalObservation(
					After.NormalObservation));
		}
		TestTrue(
			TEXT("dynamic face normals E4 uses persisted resolver"),
			PersistedResolver->ResolveCount >= 1);
		TestEqual(
			TEXT("dynamic face normals E4 reloads exactly once"),
			PersistedCatalog->EnumerateCount,
			1);
		TestEqual(
			TEXT("dynamic face normals E4 archives one verified journal"),
			CountCompletedJournals(Owner.JournalRoot),
			1);
		TestEqual(
			TEXT("dynamic face normals E4 leaves no active transaction"),
			CountActiveTransactions(Owner.JournalRoot),
			0);
	}
	TestFalse(
		TEXT("dynamic face normals E4 removes all owned disk artifacts"),
		IFileManager::Get().DirectoryExists(*OwnedRoot));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringModelingStaticAreaAngleNormalsOwnedPackageTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E4."
		"ModelingStaticAreaAngleNormalsOwnedPackage",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool
FUeAgentSimpleAuthoringModelingStaticAreaAngleNormalsOwnedPackageTest::
	RunTest(const FString& Parameters)
{
	return RunStaticNormalsOwnedPackageCase(
		this,
		TEXT("area_angle_weighted"),
		false,
		TOptional<double>(),
		TEXT("ModelingStaticAreaAngleE4"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringModelingDynamicAreaAngleNormalsOwnedWorldTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E4."
		"ModelingDynamicAreaAngleNormalsOwnedWorld",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool
FUeAgentSimpleAuthoringModelingDynamicAreaAngleNormalsOwnedWorldTest::
	RunTest(const FString& Parameters)
{
	return RunDynamicNormalsOwnedWorldCase(
		this,
		TEXT("area_angle_weighted"),
		false,
		TOptional<double>(),
		TEXT("ModelingDynamicAreaAngleE4"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringModelingStaticPreserveHardEdgesNormalsOwnedPackageTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E4."
		"ModelingStaticPreserveHardEdgesNormalsOwnedPackage",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool
FUeAgentSimpleAuthoringModelingStaticPreserveHardEdgesNormalsOwnedPackageTest::
	RunTest(const FString& Parameters)
{
	return RunStaticNormalsOwnedPackageCase(
		this,
		TEXT("area_angle_weighted"),
		true,
		TOptional<double>(),
		TEXT("ModelingStaticPreserveHardEdgesE4"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringModelingDynamicPreserveHardEdgesNormalsOwnedWorldTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E4."
		"ModelingDynamicPreserveHardEdgesNormalsOwnedWorld",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool
FUeAgentSimpleAuthoringModelingDynamicPreserveHardEdgesNormalsOwnedWorldTest::
	RunTest(const FString& Parameters)
{
	return RunDynamicNormalsOwnedWorldCase(
		this,
		TEXT("area_angle_weighted"),
		true,
		TOptional<double>(),
		TEXT("ModelingDynamicPreserveHardEdgesE4"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringModelingStaticAngleThresholdNormalsOwnedPackageTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E4."
		"ModelingStaticAngleThresholdNormalsOwnedPackage",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool
FUeAgentSimpleAuthoringModelingStaticAngleThresholdNormalsOwnedPackageTest::
	RunTest(const FString& Parameters)
{
	return RunStaticNormalsOwnedPackageCase(
		this,
		TEXT("angle_threshold"),
		false,
		TOptional<double>(30.0),
		TEXT("ModelingStaticAngleThresholdE4"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringModelingDynamicAngleThresholdNormalsOwnedWorldTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E4."
		"ModelingDynamicAngleThresholdNormalsOwnedWorld",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool
FUeAgentSimpleAuthoringModelingDynamicAngleThresholdNormalsOwnedWorldTest::
	RunTest(const FString& Parameters)
{
	return RunDynamicNormalsOwnedWorldCase(
		this,
		TEXT("angle_threshold"),
		false,
		TOptional<double>(30.0),
		TEXT("ModelingDynamicAngleThresholdE4"));
}

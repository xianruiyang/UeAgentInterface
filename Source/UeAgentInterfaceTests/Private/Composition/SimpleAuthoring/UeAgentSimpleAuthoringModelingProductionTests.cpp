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
		FUeAgentPageRequest(),
		TOptional<int64>(),
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
	ApplicationConfig.TimeoutSeconds = 60.0;
	ApplicationConfig.MaxFrameBytes = 4 * 1024 * 1024;

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
		TEXT("seven primitive operations prepare exactly once"),
		Durable->PrepareCount,
		7);
	TestEqual(
		TEXT("seven primitive operations persist exactly once"),
		Durable->PersistCount,
		7);
	TestEqual(
		TEXT("seven primitive operations acknowledge after C9"),
		Durable->AcknowledgeCount,
		7);
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
		7);
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
		AfterTopology->GetSequence());

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
		FOwnedReloadableWorldCatalog;
	using UE::UAI::Tests::SimpleAuthoring::E4::
		FOwnedReloadableWorldPackage;

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
			FUeAgentPageRequest(),
			TOptional<int64>(),
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
		ApplicationConfig.TimeoutSeconds = 60.0;
		ApplicationConfig.MaxFrameBytes = 4 * 1024 * 1024;
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

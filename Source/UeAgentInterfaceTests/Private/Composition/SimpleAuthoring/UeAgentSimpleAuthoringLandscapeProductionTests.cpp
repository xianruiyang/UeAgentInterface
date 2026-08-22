#include "Misc/AutomationTest.h"

#include "Composition/SimpleAuthoring/Families/Landscape/UeAgentLandscapeSimpleAuthoringProduction.h"
#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringComposition.h"
#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringDurableService.h"
#include "Core/BindingGateway/Ticket/UeAgentBindingTicket.h"
#include "Core/Projection/UeAgentProjection.h"
#include "DomainAdapters/LandscapeGeometryEffect/UeAgentLandscapeGeometryEffectCapabilities.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringState.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "EditorValidatorSubsystem.h"
#include "Landscape.h"
#include "LandscapeDataAccess.h"
#include "LandscapeEdit.h"
#include "LandscapeInfo.h"
#include "Misc/Guid.h"
#include "UObject/Package.h"
#include "UeAgentSimpleAuthoringPackageE4TestSupport.h"

// This E3 fixture compiles both the missing-owner red seam and the real provider.

namespace
{
	using namespace UE::UAI::AssetDurable;
	using namespace UE::UAI::BindingGateway::Ticket;
	using namespace UE::UAI::Composition::SimpleAuthoring;
	using namespace UE::UAI::Composition::SimpleAuthoring::Landscape;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::Discovery;
	using namespace
		UE::UAI::DomainAdapters::LandscapeGeometryEffect;
	using namespace UE::UAI::DomainAdapters::WorldSimpleEffect;
	using namespace UE::UAI::ExecutionMutation;
	using namespace UE::UAI::Projection;
	using namespace UE::UAI::StateSemantic;
	using namespace UE::UAI::Verification;

	static_assert(
		UAI_LANDSCAPE_SIMPLE_AUTHORING_PRODUCTION_V1 == 1,
		"Landscape Simple Authoring production contract changed");
	static_assert(UAI_SIMPLE_AUTHORING_CHANGE_TARGETS_V1 == 1);
	static_assert(UAI_SIMPLE_AUTHORING_VERIFICATION_TARGETS_V1 == 1);

	constexpr int32 LandscapeMin = 0;
	constexpr int32 LandscapeMax = 7;
	constexpr int32 LandscapeSize = 8;

	const TCHAR* CreateInput = TEXT(
		"{\"initial_height\":{\"encoding\":"
		"\"landscape_local_height\",\"value\":3},"
		"\"placement\":{\"basis_evidence\":"
		"\"explicit UE world frame and sample-origin placement\","
		"\"frame_id\":\"ue.world\",\"rotation_quaternion_xyzw\":"
		"[0,0,0.7071067811865476,0.7071067811865476],"
		"\"scale\":[100,200,50],\"subject_point\":"
		"\"sample_origin\",\"translation\":[1000,2000,300],"
		"\"unit\":\"centimeter\"},"
		"\"sample_region\":{\"basis_evidence\":"
		"\"one component, one section, seven quads produces an "
		"eight by eight sample grid\",\"frame_id\":"
		"\"ue.landscape.local_sample_grid\","
		"\"max_inclusive\":[7,7],\"min_inclusive\":[0,0]},"
		"\"target_ref\":\"uai://landscape/sa008-created\","
		"\"topology\":{\"component_count\":[1,1],"
		"\"quads_per_section\":7,\"sections_per_component\":1}}");

	const TCHAR* HeightDeltaInput = TEXT(
		"{\"delta\":{\"encoding\":"
		"\"landscape_local_height_delta\",\"row_stride\":3,"
		"\"values\":[1,2,3,4,5,6]},"
		"\"region\":{\"frame_id\":"
		"\"ue.landscape.local_sample_grid\","
		"\"max_inclusive\":[4,2],\"min_inclusive\":[2,1]},"
		"\"space_contract\":{\"basis_evidence\":"
		"\"independent Landscape transform, scale, extent, and "
		"revision\",\"conversion_chain\":\"UE world intent -> "
		"observed Landscape transform -> local sample grid -> "
		"Landscape edit API -> independent readback -> UE world\","
		"\"expected_scale\":[100,200,50],"
		"\"read_frame_id\":\"ue.landscape.local_sample_grid\","
		"\"semantic_frame_id\":\"ue.world\","
		"\"unit\":\"centimeter\","
		"\"verify_frame_id\":\"ue.world\","
		"\"write_frame_id\":"
		"\"ue.landscape.local_sample_grid\"},"
		"\"target_ref\":\"uai://landscape/sa008-existing\"}");

	class FOwnedLandscapeWorld final
	{
	public:
		FOwnedLandscapeWorld()
		{
			Package = CreatePackage(
				*(TEXT("/Temp/UAI/SA008/LandscapeProduction/")
					+ FGuid::NewGuid().ToString(
						EGuidFormats::Digits)));
			World = UWorld::CreateWorld(
				EWorldType::EditorPreview,
				false,
				FName(TEXT("SA008LandscapeProduction")),
				Package);
			if (!World || !World->PersistentLevel)
			{
				return;
			}
			World->SetCurrentLevel(World->PersistentLevel);
			Existing = CreateLandscape(
				TEXT("sa008-existing"),
				FVector(1000.0, 2000.0, 300.0),
				FRotator(0.0, 90.0, 0.0),
				FVector(100.0, 200.0, 50.0),
				0.0f);
			ContextRevision = FString::Printf(
				TEXT("sa008-landscape-production:%u"),
				World->GetUniqueID());
			Package->SetDirtyFlag(false);
		}

		~FOwnedLandscapeWorld()
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

		ALandscape* CreateLandscape(
			const FString& Label,
			const FVector& Translation,
			const FRotator& Rotation,
			const FVector& Scale,
			const float InitialHeight)
		{
			if (!World)
			{
				return nullptr;
			}
			FActorSpawnParameters SpawnParameters;
			SpawnParameters.SpawnCollisionHandlingOverride =
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			SpawnParameters.ObjectFlags |= RF_Transactional;
			ALandscape* Created =
				World->SpawnActor<ALandscape>(
					Translation,
					Rotation,
					SpawnParameters);
			if (!Created)
			{
				return nullptr;
			}
			Created->SetActorScale3D(Scale);
			Created->SetActorLabel(Label);

			TArray<uint16> Heights;
			Heights.Init(
				LandscapeDataAccess::GetTexHeight(InitialHeight),
				LandscapeSize * LandscapeSize);
			TMap<FGuid, TArray<uint16>> HeightsByLayer;
			HeightsByLayer.Add(FGuid(), MoveTemp(Heights));
			TMap<
				FGuid,
				TArray<FLandscapeImportLayerInfo>>
				MaterialLayersByLayer;
			MaterialLayersByLayer.Add(
				FGuid(),
				TArray<FLandscapeImportLayerInfo>());
			const TArrayView<const FLandscapeLayer> EmptyLayers;
			Created->Import(
				FGuid::NewGuid(),
				LandscapeMin,
				LandscapeMin,
				LandscapeMax,
				LandscapeMax,
				1,
				7,
				HeightsByLayer,
				TEXT(""),
				MaterialLayersByLayer,
				ELandscapeImportAlphamapType::Additive,
				EmptyLayers);
			Created->CreateLandscapeInfo();
			Created->PostEditChange();
			return Created;
		}

		bool ReadLocalHeights(
			ALandscape& Target,
			const int32 MinX,
			const int32 MinY,
			const int32 MaxX,
			const int32 MaxY,
			TArray<float>& OutHeights) const
		{
			ULandscapeInfo* Info = Target.GetLandscapeInfo();
			const int32 Width = MaxX - MinX + 1;
			const int32 Height = MaxY - MinY + 1;
			if (!Info || Width <= 0 || Height <= 0)
			{
				return false;
			}
			TArray<uint16> Encoded;
			Encoded.SetNumUninitialized(Width * Height);
			FLandscapeEditDataInterface Edit(Info);
			Edit.GetHeightDataFast(
				MinX,
				MinY,
				MaxX,
				MaxY,
				Encoded.GetData(),
				Width,
				nullptr);
			OutHeights.Reset(Encoded.Num());
			for (const uint16 Value : Encoded)
			{
				OutHeights.Add(
					LandscapeDataAccess::GetLocalHeight(Value));
			}
			return true;
		}

		ALandscape* FindByLabel(const FString& Label) const
		{
			ALandscape* Match = nullptr;
			if (!World)
			{
				return nullptr;
			}
			for (TActorIterator<ALandscape> It(World); It; ++It)
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

		int32 CountLandscapes() const
		{
			int32 Count = 0;
			if (World)
			{
				for (TActorIterator<ALandscape> It(World); It; ++It)
				{
					++Count;
				}
			}
			return Count;
		}

		bool IsValid() const
		{
			return Package
				&& World
				&& World->PersistentLevel
				&& Existing
				&& Existing->GetLandscapeInfo()
				&& !ContextRevision.IsEmpty();
		}

		UPackage* Package = nullptr;
		UWorld* World = nullptr;
		ALandscape* Existing = nullptr;
		FString ContextRevision;
	};

	class FOwnedWorldCatalog final
		: public IUeAgentWorldSimpleEffectWorldCatalog
	{
	public:
		explicit FOwnedWorldCatalog(FOwnedLandscapeWorld& InOwner)
			: Owner(InOwner)
		{
		}

		bool TryEnumerate(
			TArray<FUeAgentWorldSimpleEffectWorldCandidate>&
				OutCandidates,
			FExecutionMutationError& OutError) override
		{
			++EnumerateCount;
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

		int32 EnumerateCount = 0;

	private:
		FOwnedLandscapeWorld& Owner;
	};

	class FBoundReferenceResolver final
		: public IUeAgentLandscapeGeometryEffectReferenceResolver
	{
	public:
		void Bind(
			FString CanonicalRef,
			UObject* Object)
		{
			Bindings.Add(
				MoveTemp(CanonicalRef),
				Object);
		}

		bool TryResolveReference(
			UWorld& World,
			const FString& CanonicalRef,
			UObject*& OutObject,
			FExecutionMutationError& OutError) override
		{
			++ResolveCount;
			OutObject = Bindings.FindRef(CanonicalRef).Get();
			if (!OutObject || OutObject->GetWorld() != &World)
			{
				OutObject = nullptr;
				OutError = FExecutionMutationError(
					TEXT(
						"sa008_landscape_reference_unresolved"),
					CanonicalRef);
				return false;
			}
			return true;
		}

		int32 ResolveCount = 0;

	private:
		TMap<FString, TWeakObjectPtr<UObject>> Bindings;
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
						"sa008_landscape_durable_target_invalid"));
				return false;
			}
			SeenPackageName =
				Request.GetTargets()[0].GetPackageName();
			TArray<FDurableArtifact> Artifacts;
			Artifacts.Emplace(
				TEXT("artifact:sa008-landscape-production"),
				Request.GetTargets()[0].GetIdentity(),
				SeenPackageName,
				TEXT(
					"fixture://sa008-landscape-production.umap"),
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
				TEXT("journal:sa008-landscape-production"),
				TEXT(
					"runtimeLogs/SA008LandscapeProduction/"
					"journal.hjson"),
				TEXT(
					"runtimeLogs/SA008LandscapeProduction"),
				TEXT(
					"runtimeLogs/SA008LandscapeProduction/"
					"transaction"),
				TEXT(
					"runtimeLogs/SA008LandscapeProduction/"
					"staging"),
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
				{
					TEXT("prepared"),
					TEXT("persisted_unverified"),
				},
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
					"sa008_landscape_unexpected_durable_rollback"));
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
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringLandscapeProductionContractTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3."
		"LandscapeProductionContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentSimpleAuthoringLandscapeProductionContractTest::RunTest(
	const FString& Parameters)
{
	FOwnedLandscapeWorld Owner;
	if (!TestTrue(
			TEXT("owned exact Landscape world is valid"),
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
	const FUeAgentSimpleAuthoringCommandRoute* CreateRoute =
		FindRoute(TEXT("landscape.create"));
	const FUeAgentSimpleAuthoringCommandRoute* DeltaRoute =
		FindRoute(TEXT("landscape.height_delta"));
	if (!TestNotNull(
			TEXT("Landscape create has one exact route"),
			CreateRoute)
		|| !TestNotNull(
			TEXT("Landscape height delta has one exact route"),
			DeltaRoute)
		|| !Registry.IsValid())
	{
		AddError(
			TEXT("sa008_landscape_route_missing:")
			+ RegistryError);
		return false;
	}

	const TSharedRef<FOwnedWorldCatalog, ESPMode::ThreadSafe>
		Catalog = MakeShared<
			FOwnedWorldCatalog,
			ESPMode::ThreadSafe>(Owner);
	const TSharedRef<
		FBoundReferenceResolver,
		ESPMode::ThreadSafe> ReferenceResolver =
			MakeShared<
				FBoundReferenceResolver,
				ESPMode::ThreadSafe>();
	ReferenceResolver->Bind(
		TEXT("uai://landscape/sa008-existing"),
		Owner.Existing);
	const TSharedRef<
		FRecordingDurableService,
		ESPMode::ThreadSafe> Durable =
			MakeShared<
				FRecordingDurableService,
				ESPMode::ThreadSafe>();

	FUeAgentLandscapeSimpleAuthoringProviderConfig ProviderConfig;
	ProviderConfig.DescriptorRegistry = Registry;
	ProviderConfig.WorldCatalog = Catalog;
	ProviderConfig.ReferenceResolver = ReferenceResolver;
	ProviderConfig.DurableService = Durable;
	ProviderConfig.AbsenceOracle =
		MakeShared<FUnusedAbsenceOracle>();
	ProviderConfig.ReferenceOracle =
		MakeShared<FUnusedReferenceOracle>();
	ProviderConfig.OwnerLeaseId =
		TEXT("owner:sa008-landscape-production");
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
	FUeAgentLandscapeSimpleAuthoringError ProviderError;
	TestTrue(
		TEXT("Landscape production family composes"),
		FUeAgentLandscapeSimpleAuthoringProviderFactory::TryCreate(
			ProviderConfig,
			Provider,
			ProviderError));
	if (!Provider.IsValid())
	{
		AddError(
			TEXT("sa008_landscape_provider_invalid:")
			+ ProviderError.Code);
		return false;
	}

	TSharedPtr<const FUeAgentOperationTicketService> TicketService;
	FUeAgentOperationTicketError TicketError;
	TestTrue(
		TEXT("ticket service creates"),
		FUeAgentOperationTicketService::TryCreate(
			TEXT("sa008-landscape-production-integrity"),
			30.0,
			[]()
			{
				return 100.0;
			},
			[]()
			{
				return FString(
					TEXT(
						"ticket:sa008-landscape-production"));
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
		TEXT("project:sa008-landscape-production");
	ApplicationConfig.ExecutionContext.EditorInstanceId =
		TEXT("editor:sa008-landscape-production");
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
		TEXT("session:sa008-landscape-production");
	ApplicationConfig.RegistryEpoch = 1;
	ApplicationConfig.TimeoutSeconds = 30.0;
	ApplicationConfig.MaxFrameBytes = 1024 * 1024;

	TSharedPtr<FUeAgentSimpleAuthoringApplication> Application;
	FUeAgentSimpleAuthoringApplicationError ApplicationError;
	TestTrue(
		TEXT("application accepts the exact Landscape routes"),
		FUeAgentSimpleAuthoringApplication::TryCreate(
			ApplicationConfig,
			{*CreateRoute, *DeltaRoute},
			{Provider.ToSharedRef()},
			TicketService.ToSharedRef(),
			Projector.ToSharedRef(),
			Application,
			ApplicationError));
	if (!Application.IsValid())
	{
		AddError(
			TEXT("sa008_landscape_application_invalid:")
			+ ApplicationError.Code);
		return false;
	}

	const int32 InitialCount = Owner.CountLandscapes();
	const FUeAgentCallerContext Caller(
		TEXT("caller:sa008-landscape-production"),
		{TEXT("uai:landscape:write")},
		TEXT("correlation:sa008-landscape-production"));
	const FUeAgentOperationResult CreateResult =
		Application->Submit(
			FUeAgentOperationRequest(
				CreateRoute->DescriptorIdentity,
				TEXT("request:sa008-landscape-create"),
				TEXT("idempotency:sa008-landscape-create"),
				FUeAgentContractValue(CreateInput)),
			Caller,
			MakeShared<FNeverCancelled>());
	TestTrue(
		TEXT("Landscape create completes through production"),
		IsComplete(CreateResult));
	TestEqual(
		TEXT("Landscape create adds exactly one actor"),
		Owner.CountLandscapes(),
		InitialCount + 1);
	ALandscape* Created =
		Owner.FindByLabel(TEXT("sa008-created"));
	if (TestNotNull(
			TEXT("created Landscape is independently located"),
			Created))
	{
		TestTrue(
			TEXT("created translation is real UE state"),
			Created->GetActorLocation().Equals(
				FVector(1000.0, 2000.0, 300.0)));
		TestTrue(
			TEXT("created rotation is real UE state"),
			Created->GetActorRotation().Equals(
				FRotator(0.0, 90.0, 0.0),
				0.01));
		TestTrue(
			TEXT("created non-uniform scale is real UE state"),
			Created->GetActorScale3D().Equals(
				FVector(100.0, 200.0, 50.0)));
		TestEqual(
			TEXT("created Landscape binds the exact managed id"),
			UeAgentLevelAuthoring::GetManagedActorId(*Created),
			FString(TEXT("uai://landscape/sa008-created")));
		TestEqual(
			TEXT("created Landscape uses the shared managed folder"),
			UeAgentLevelAuthoring::GetActorFolderPathText(*Created),
			UeAgentLevelAuthoring::MakeManagedActorFolderPath(
				TEXT("landscape")));
		TestTrue(
			TEXT("created Landscape carries the managed marker"),
			Created->ActorHasTag(
				UeAgentLevelAuthoring::GetManagedActorMarkerTag()));
	}
	TestEqual(
		TEXT("create prepares durability once"),
		Durable->PrepareCount,
		1);
	TestEqual(
		TEXT("create persists once"),
		Durable->PersistCount,
		1);
	TestEqual(
		TEXT("create acknowledges only after C9"),
		Durable->AcknowledgeCount,
		1);
	TestEqual(
		TEXT("C8D receives the exact owned world package"),
		Durable->SeenPackageName,
		Owner.Package->GetName());

	const FUeAgentOperationResult DuplicateResult =
		Application->Submit(
			FUeAgentOperationRequest(
				CreateRoute->DescriptorIdentity,
				TEXT("request:sa008-landscape-create-duplicate"),
				TEXT(
					"idempotency:sa008-landscape-create-duplicate"),
				FUeAgentContractValue(CreateInput)),
			Caller,
			MakeShared<FNeverCancelled>());
	TestFalse(
		TEXT("duplicate Landscape create is rejected"),
		IsComplete(DuplicateResult));
	TestEqual(
		TEXT("duplicate create performs zero mutation"),
		Owner.CountLandscapes(),
		InitialCount + 1);
	TestEqual(
		TEXT("duplicate create is rejected before C8D"),
		Durable->PrepareCount,
		1);

	TArray<float> HeightsBefore;
	TestTrue(
		TEXT("height pre-state reads through Landscape API"),
		Owner.ReadLocalHeights(
			*Owner.Existing,
			2,
			1,
			4,
			2,
			HeightsBefore));
	const FUeAgentOperationResult DeltaResult =
		Application->Submit(
			FUeAgentOperationRequest(
				DeltaRoute->DescriptorIdentity,
				TEXT("request:sa008-landscape-height-delta"),
				TEXT(
					"idempotency:sa008-landscape-height-delta"),
				FUeAgentContractValue(HeightDeltaInput)),
			Caller,
			MakeShared<FNeverCancelled>());
	TestTrue(
		TEXT("Landscape height delta completes through production"),
		IsComplete(DeltaResult));
	TArray<float> HeightsAfter;
	TestTrue(
		TEXT("height post-state reads through Landscape API"),
		Owner.ReadLocalHeights(
			*Owner.Existing,
			2,
			1,
			4,
			2,
			HeightsAfter));
	const TArray<float> ExpectedDelta = {
		1.0f,
		2.0f,
		3.0f,
		4.0f,
		5.0f,
		6.0f,
	};
	TestEqual(
		TEXT("height readback sample count is stable"),
		HeightsAfter.Num(),
		ExpectedDelta.Num());
	for (int32 Index = 0;
		Index < HeightsBefore.Num()
			&& Index < HeightsAfter.Num()
			&& Index < ExpectedDelta.Num();
		++Index)
	{
		TestTrue(
			*FString::Printf(
				TEXT(
					"height sample %d matches independent C9"),
				Index),
			FMath::IsNearlyEqual(
				HeightsAfter[Index],
				HeightsBefore[Index]
					+ ExpectedDelta[Index],
				0.01f));
	}

	// Historical landscape_raise_circle is represented by the general
	// landscape.height_delta primitive. Freeze the exact conversion here:
	// world distance uses both observed XY scales, height centimeters use the
	// observed Z scale, and falloff blends linear attenuation with smoothstep.
	constexpr int32 CircleMinX = 0;
	constexpr int32 CircleMinY = 2;
	constexpr int32 CircleMaxX = 7;
	constexpr int32 CircleMaxY = 6;
	constexpr int32 CircleWidth = CircleMaxX - CircleMinX + 1;
	constexpr double CenterX = 4.0;
	constexpr double CenterY = 4.0;
	constexpr double RadiusCm = 400.0;
	constexpr double StrengthCm = 300.0;
	constexpr double Falloff = 0.75;
	const FVector ObservedScale = Owner.Existing->GetActorScale3D();
	TArray<FString> CircleValueStrings;
	TArray<float> ExpectedCircleDelta;
	for (int32 Y = CircleMinY; Y <= CircleMaxY; ++Y)
	{
		for (int32 X = CircleMinX; X <= CircleMaxX; ++X)
		{
			const double WorldDistance = FMath::Sqrt(
				FMath::Square((X - CenterX) * ObservedScale.X)
				+ FMath::Square((Y - CenterY) * ObservedScale.Y));
			double Weight = 0.0;
			if (WorldDistance <= RadiusCm)
			{
				const double Linear = 1.0 - WorldDistance / RadiusCm;
				const double Smooth = Linear * Linear * (3.0 - 2.0 * Linear);
				Weight = FMath::Lerp(Linear, Smooth, Falloff);
			}
			const float DeltaLocal = static_cast<float>(
				StrengthCm / ObservedScale.Z * Weight);
			ExpectedCircleDelta.Add(DeltaLocal);
			CircleValueStrings.Add(FString::SanitizeFloat(DeltaLocal));
		}
	}
	TArray<float> CircleBefore;
	TestTrue(
		TEXT("raise-circle pre-state reads the exact bounded sample region"),
		Owner.ReadLocalHeights(
			*Owner.Existing,
			CircleMinX,
			CircleMinY,
			CircleMaxX,
			CircleMaxY,
			CircleBefore));
	const FString CircleInput = FString::Printf(
		TEXT("{\"delta\":{\"encoding\":\"landscape_local_height_delta\","
			"\"row_stride\":%d,\"values\":[%s]},"
			"\"region\":{\"frame_id\":\"ue.landscape.local_sample_grid\","
			"\"max_inclusive\":[%d,%d],\"min_inclusive\":[%d,%d]},"
			"\"space_contract\":{\"basis_evidence\":\"historical raise-circle "
			"world-space conversion from independently observed transform and extent\","
			"\"conversion_chain\":\"center world -> inverse Landscape transform -> "
			"bounded sample grid; radius centimeters -> per-axis world distance; "
			"strength centimeters -> local height through observed Z scale; "
			"linear/smoothstep falloff -> delta values\","
			"\"expected_scale\":[100,200,50],"
			"\"read_frame_id\":\"ue.landscape.local_sample_grid\","
			"\"semantic_frame_id\":\"ue.world\",\"unit\":\"centimeter\","
			"\"verify_frame_id\":\"ue.world\","
			"\"write_frame_id\":\"ue.landscape.local_sample_grid\"},"
			"\"target_ref\":\"uai://landscape/sa008-existing\"}"),
		CircleWidth,
		*FString::Join(CircleValueStrings, TEXT(",")),
		CircleMaxX,
		CircleMaxY,
		CircleMinX,
		CircleMinY);
	const FUeAgentOperationResult CircleResult = Application->Submit(
		FUeAgentOperationRequest(
			DeltaRoute->DescriptorIdentity,
			TEXT("request:sa008-landscape-raise-circle-successor"),
			TEXT("idempotency:sa008-landscape-raise-circle-successor"),
			FUeAgentContractValue(CircleInput)),
		Caller,
		MakeShared<FNeverCancelled>());
	TestTrue(
		TEXT("historical raise-circle semantics complete through height_delta"),
		IsComplete(CircleResult));
	TArray<float> CircleAfter;
	TestTrue(
		TEXT("raise-circle successor independently reads the native height field"),
		Owner.ReadLocalHeights(
			*Owner.Existing,
			CircleMinX,
			CircleMinY,
			CircleMaxX,
			CircleMaxY,
			CircleAfter));
	for (int32 Index = 0;
		Index < CircleBefore.Num()
			&& Index < CircleAfter.Num()
			&& Index < ExpectedCircleDelta.Num();
		++Index)
	{
		TestTrue(
			*FString::Printf(
				TEXT("raise-circle sample %d preserves the exact falloff"), Index),
			FMath::IsNearlyEqual(
				CircleAfter[Index],
				CircleBefore[Index] + ExpectedCircleDelta[Index],
				0.01f));
	}
	const int32 CenterIndex = (4 - CircleMinY) * CircleWidth
		+ (4 - CircleMinX);
	const int32 XTwoHundredIndex = (4 - CircleMinY) * CircleWidth
		+ (6 - CircleMinX);
	const int32 YTwoHundredIndex = (5 - CircleMinY) * CircleWidth
		+ (4 - CircleMinX);
	TestTrue(
		TEXT("raise-circle center gains the requested world-space strength"),
		CircleAfter.IsValidIndex(CenterIndex)
			&& CircleBefore.IsValidIndex(CenterIndex)
			&& FMath::IsNearlyEqual(
				(CircleAfter[CenterIndex] - CircleBefore[CenterIndex])
					* ObservedScale.Z,
				StrengthCm,
				0.5));
	TestTrue(
		TEXT("raise-circle uses both XY scales for equal world-distance falloff"),
		ExpectedCircleDelta.IsValidIndex(XTwoHundredIndex)
			&& ExpectedCircleDelta.IsValidIndex(YTwoHundredIndex)
			&& FMath::IsNearlyEqual(
				ExpectedCircleDelta[XTwoHundredIndex],
				ExpectedCircleDelta[YTwoHundredIndex],
				0.001f));
	TestEqual(
		TEXT("three successful operations prepare exactly three times"),
		Durable->PrepareCount,
		3);
	TestEqual(
		TEXT("three successful operations persist exactly three times"),
		Durable->PersistCount,
		3);
	TestEqual(
		TEXT("three successful operations acknowledge exactly three times"),
		Durable->AcknowledgeCount,
		3);
	TestTrue(
		TEXT(
			"resolver, pre-state, mutation and C9 use exact world"),
		Catalog->EnumerateCount >= 8);
	TestTrue(
		TEXT("height target is independently re-resolved"),
		ReferenceResolver->ResolveCount >= 5);
	return !HasAnyErrors();
}

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

	ALandscape* CreateE4Landscape(
		FOwnedReloadableWorldPackage& Owner,
		const FString& Label,
		const FString& ManagedId)
	{
		if (!Owner.World.IsValid())
		{
			return nullptr;
		}
		FActorSpawnParameters SpawnParameters;
		SpawnParameters.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		SpawnParameters.ObjectFlags |= RF_Transactional;
		ALandscape* Created =
			Owner.World->SpawnActor<ALandscape>(
				FVector(1000.0, 2000.0, 300.0),
				FRotator(0.0, 90.0, 0.0),
				SpawnParameters);
		if (!Created)
		{
			return nullptr;
		}
		Created->SetActorScale3D(FVector(100.0, 200.0, 50.0));
		Created->SetActorLabel(Label);
		FString OwnershipError;
		if (!UeAgentLevelAuthoring::TryBindManagedActorOwnership(
				*Created,
				ManagedId,
				TEXT("landscape"),
				OwnershipError))
		{
			Owner.World->DestroyActor(Created);
			return nullptr;
		}
		TArray<uint16> Heights;
		Heights.Init(
			LandscapeDataAccess::GetTexHeight(0.0f),
			LandscapeSize * LandscapeSize);
		TMap<FGuid, TArray<uint16>> HeightsByLayer;
		HeightsByLayer.Add(FGuid(), MoveTemp(Heights));
		TMap<FGuid, TArray<FLandscapeImportLayerInfo>>
			MaterialLayersByLayer;
		MaterialLayersByLayer.Add(
			FGuid(),
			TArray<FLandscapeImportLayerInfo>());
		const TArrayView<const FLandscapeLayer> EmptyLayers;
		Created->Import(
			FGuid::NewGuid(),
			LandscapeMin,
			LandscapeMin,
			LandscapeMax,
			LandscapeMax,
			1,
			7,
			HeightsByLayer,
			TEXT(""),
			MaterialLayersByLayer,
			ELandscapeImportAlphamapType::Additive,
			EmptyLayers);
		Created->CreateLandscapeInfo();
		Created->PostEditChange();
		return Created;
	}

	ALandscape* FindE4Landscape(
		UWorld& World,
		const FString& Label)
	{
		ALandscape* Match = nullptr;
		for (TActorIterator<ALandscape> It(&World); It; ++It)
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

	bool ReadE4LandscapeHeights(
		ALandscape& Landscape,
		TArray<float>& OutHeights)
	{
		ULandscapeInfo* Info = Landscape.GetLandscapeInfo();
		if (!Info)
		{
			return false;
		}
		constexpr int32 MinX = 2;
		constexpr int32 MinY = 1;
		constexpr int32 MaxX = 4;
		constexpr int32 MaxY = 2;
		constexpr int32 Width = MaxX - MinX + 1;
		TArray<uint16> Encoded;
		Encoded.SetNumUninitialized(
			Width * (MaxY - MinY + 1));
		FLandscapeEditDataInterface Edit(Info);
		Edit.GetHeightDataFast(
			MinX,
			MinY,
			MaxX,
			MaxY,
			Encoded.GetData(),
			Width,
			nullptr);
		OutHeights.Reset(Encoded.Num());
		for (const uint16 Value : Encoded)
		{
			OutHeights.Add(
				LandscapeDataAccess::GetLocalHeight(Value));
		}
		return true;
	}

	class FReloadedLandscapeReferenceResolver final
		: public IUeAgentLandscapeGeometryEffectReferenceResolver
	{
	public:
		explicit FReloadedLandscapeReferenceResolver(
			FString InExpectedRef)
			: ExpectedRef(MoveTemp(InExpectedRef))
			, ExpectedLabel(FPaths::GetBaseFilename(ExpectedRef))
		{
		}

		bool TryResolveReference(
			UWorld& World,
			const FString& CanonicalRef,
			UObject*& OutObject,
			FExecutionMutationError& OutError) override
		{
			++ResolveCount;
			OutObject = CanonicalRef == ExpectedRef
				? FindE4Landscape(World, ExpectedLabel)
				: nullptr;
			if (!OutObject)
			{
				OutError = FExecutionMutationError(
					TEXT("sa008_landscape_e4_reference_unresolved"),
					CanonicalRef);
				return false;
			}
			return true;
		}

		int32 ResolveCount = 0;

	private:
		FString ExpectedRef;
		FString ExpectedLabel;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringLandscapeCreateOwnedPackageTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E4."
		"LandscapeCreateOwnedPackage",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentSimpleAuthoringLandscapeCreateOwnedPackageTest::RunTest(
	const FString& Parameters)
{
	const FString TargetRef =
		TEXT("uai://landscape/sa008-e4-created");
	const FString TargetLabel = FPaths::GetBaseFilename(TargetRef);
	FString OwnedRoot;
	{
		FOwnedReloadableWorldPackage Owner(
			TEXT("LandscapeCreateE4"),
			TEXT("LandscapeCreateE4"));
		OwnedRoot = Owner.Root;
		if (!TestTrue(
				TEXT("owned create Landscape world is valid"),
				Owner.IsValid()))
		{
			return false;
		}
		Owner.Package->SetDirtyFlag(false);

		FUeAgentSimpleAuthoringDurableServiceConfig DurableConfig;
		DurableConfig.JournalRoot = Owner.JournalRoot;
		DurableConfig.PackageBackend = MakeShared<
			FPackageAssetDurableBackend,
			ESPMode::ThreadSafe>(Owner.PackageRoot);
		TSharedPtr<IUeAgentSimpleAuthoringAssetDurableService>
			Durable;
		FUeAgentSimpleAuthoringProductionError DurableError;
		if (!TestTrue(
				TEXT("real Landscape create package C8D service composes"),
				FUeAgentSimpleAuthoringDurableServiceFactory::TryCreate(
					DurableConfig,
					Durable,
					DurableError))
			|| !Durable.IsValid())
		{
			AddError(
				TEXT("sa008_landscape_create_e4_durable_invalid:")
					+ DurableError.Code);
			return false;
		}

		TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
		FString RegistryError;
		const FUeAgentSimpleAuthoringCommandRoute* Route =
			FindRoute(TEXT("landscape.create"));
		if (!TestTrue(
				TEXT("Simple Authoring descriptors compose for Landscape create E4"),
				TryCreateSimpleAuthoringDescriptorRegistry(
					Registry,
					RegistryError))
			|| !TestNotNull(
				TEXT("Landscape create has one E4 route"),
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
		const TSharedRef<
			FReloadedLandscapeReferenceResolver,
			ESPMode::ThreadSafe> ReferenceResolver = MakeShared<
				FReloadedLandscapeReferenceResolver,
				ESPMode::ThreadSafe>(TargetRef);

		FUeAgentLandscapeSimpleAuthoringProviderConfig ProviderConfig;
		ProviderConfig.DescriptorRegistry = Registry;
		ProviderConfig.WorldCatalog = SourceCatalog;
		ProviderConfig.PersistedWorldCatalog = ReloadCatalog;
		ProviderConfig.ReferenceResolver = ReferenceResolver;
		ProviderConfig.PersistedReferenceResolver =
			ReferenceResolver;
		ProviderConfig.DurableService = Durable;
		ProviderConfig.AbsenceOracle =
			MakeShared<FUnusedAbsenceOracle>();
		ProviderConfig.ReferenceOracle =
			MakeShared<FUnusedReferenceOracle>();
		ProviderConfig.OwnerLeaseId =
			TEXT("owner:sa008-landscape-create-e4");
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
		FUeAgentLandscapeSimpleAuthoringError ProviderError;
		if (!TestTrue(
				TEXT("Landscape create E4 family composes"),
				FUeAgentLandscapeSimpleAuthoringProviderFactory::TryCreate(
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
			TEXT("sa008-landscape-create-e4-integrity"),
			60.0,
			[]() { return 100.0; },
			[]()
			{
				return FString(
					TEXT("ticket:sa008-landscape-create-e4"));
			},
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
			TEXT("project:sa008-landscape-create-e4");
		ApplicationConfig.ExecutionContext.EditorInstanceId =
			TEXT("editor:sa008-landscape-create-e4");
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
			TEXT("session:sa008-landscape-create-e4");
		ApplicationConfig.RegistryEpoch = 1;
		ApplicationConfig.TimeoutSeconds = 60.0;
		ApplicationConfig.MaxFrameBytes = 4 * 1024 * 1024;
		TSharedPtr<FUeAgentSimpleAuthoringApplication> Application;
		FUeAgentSimpleAuthoringApplicationError ApplicationError;
		if (!TestTrue(
				TEXT("application accepts Landscape create E4 route"),
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

		FString Payload(CreateInput);
		Payload.ReplaceInline(
			TEXT("sa008-created"),
			TEXT("sa008-e4-created"));
		const FUeAgentOperationResult Result = Application->Submit(
			FUeAgentOperationRequest(
				Route->DescriptorIdentity,
				TEXT("request:sa008-landscape-create-e4"),
				TEXT("idempotency:sa008-landscape-create-e4"),
				FUeAgentContractValue(Payload)),
			FUeAgentCallerContext(
				TEXT("caller:sa008-landscape-create-e4"),
				{TEXT("uai:landscape:write")},
				TEXT("correlation:sa008-landscape-create-e4")),
			MakeShared<FNeverCancelled>());
		if (!TestTrue(
				TEXT("Landscape create E4 completes after reload readback"),
				IsComplete(Result)))
		{
			AddError(Result.GetValue().GetJsonValue());
			return false;
		}

		ALandscape* ReloadedLandscape = Owner.World.IsValid()
			? FindE4Landscape(*Owner.World, TargetLabel)
			: nullptr;
		TestTrue(
			TEXT("Landscape create E4 uses a reloaded world owner"),
			Owner.WasReloaded());
		TestTrue(
			TEXT("Landscape create E4 collects the original world"),
			Owner.WasOriginalWorldCollected());
		TestTrue(
			TEXT("Landscape create E4 publishes one map"),
			IFileManager::Get().FileExists(*Owner.HeaderPath()));
		if (TestNotNull(
				TEXT("reloaded created Landscape is independently found"),
				ReloadedLandscape))
		{
			TArray<float> ReloadedHeights;
			TestTrue(
				TEXT("reloaded created Landscape heights are readable"),
				ReadE4LandscapeHeights(
					*ReloadedLandscape,
					ReloadedHeights));
			for (int32 Index = 0; Index < ReloadedHeights.Num(); ++Index)
			{
				TestTrue(
					*FString::Printf(
						TEXT("created Landscape height %d persists"),
						Index),
					FMath::IsNearlyEqual(
						ReloadedHeights[Index],
						3.0f,
						0.01f));
			}
			TestTrue(
				TEXT("created Landscape placement persists"),
				ReloadedLandscape->GetActorLocation().Equals(
						FVector(1000.0, 2000.0, 300.0),
						0.01)
					&& ReloadedLandscape->GetActorRotation().Equals(
						FRotator(0.0, 90.0, 0.0),
						0.01)
					&& ReloadedLandscape->GetActorScale3D().Equals(
						FVector(100.0, 200.0, 50.0),
						0.01));
			TestEqual(
				TEXT("created Landscape managed id persists"),
				UeAgentLevelAuthoring::GetManagedActorId(
					*ReloadedLandscape),
				TargetRef);
			TestEqual(
				TEXT("created Landscape managed folder persists"),
				UeAgentLevelAuthoring::GetActorFolderPathText(
					*ReloadedLandscape),
				UeAgentLevelAuthoring::MakeManagedActorFolderPath(
					TEXT("landscape")));
			TestTrue(
				TEXT("created Landscape managed marker persists"),
				ReloadedLandscape->ActorHasTag(
					UeAgentLevelAuthoring::
						GetManagedActorMarkerTag()));
		}
		TestEqual(
			TEXT("Landscape create E4 C9 reloads exactly once"),
			ReloadCatalog->EnumerateCount,
			1);
		TestEqual(
			TEXT("Landscape create E4 archives one verified journal"),
			CountCompletedJournals(Owner.JournalRoot),
			1);
		TestEqual(
			TEXT("Landscape create E4 leaves no active transaction"),
			CountActiveTransactions(Owner.JournalRoot),
			0);
	}
	TestFalse(
		TEXT("Landscape create E4 removes all owned disk artifacts"),
		IFileManager::Get().DirectoryExists(*OwnedRoot));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringLandscapeOwnedPackageTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E4."
		"LandscapeOwnedPackage",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentSimpleAuthoringLandscapeOwnedPackageTest::RunTest(
	const FString& Parameters)
{
	const FString TargetRef =
		TEXT("uai://landscape/sa008-e4-existing");
	const FString TargetLabel = FPaths::GetBaseFilename(TargetRef);
	FString OwnedRoot;
	{
		FOwnedReloadableWorldPackage Owner(
			TEXT("LandscapeE4"),
			TEXT("LandscapeE4"));
		OwnedRoot = Owner.Root;
		ALandscape* InitialLandscape =
			CreateE4Landscape(
				Owner,
				TargetLabel,
				TargetRef);
		if (!TestTrue(
				TEXT("owned durable Landscape world is valid"),
				Owner.IsValid()
					&& InitialLandscape
					&& InitialLandscape->GetLandscapeInfo()))
		{
			return false;
		}
		Owner.Package->SetDirtyFlag(false);
		TArray<float> HeightsBefore;
		if (!TestTrue(
				TEXT("Landscape E4 reads the initial local region"),
				ReadE4LandscapeHeights(
					*InitialLandscape,
					HeightsBefore)))
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
				TEXT("real Landscape package C8D service composes"),
				FUeAgentSimpleAuthoringDurableServiceFactory::TryCreate(
					DurableConfig,
					Durable,
					DurableError))
			|| !Durable.IsValid())
		{
			AddError(
				TEXT("sa008_landscape_e4_durable_invalid:")
					+ DurableError.Code);
			return false;
		}

		TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
		FString RegistryError;
		const FUeAgentSimpleAuthoringCommandRoute* Route =
			FindRoute(TEXT("landscape.height_delta"));
		if (!TestTrue(
				TEXT("Simple Authoring descriptors compose for Landscape E4"),
				TryCreateSimpleAuthoringDescriptorRegistry(
					Registry,
					RegistryError))
			|| !TestNotNull(
				TEXT("Landscape height delta has one E4 route"),
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
		const TSharedRef<FBoundReferenceResolver, ESPMode::ThreadSafe>
			SourceResolver = MakeShared<
				FBoundReferenceResolver,
				ESPMode::ThreadSafe>();
		SourceResolver->Bind(TargetRef, InitialLandscape);
		const TSharedRef<
			FReloadedLandscapeReferenceResolver,
			ESPMode::ThreadSafe> ReloadResolver = MakeShared<
				FReloadedLandscapeReferenceResolver,
				ESPMode::ThreadSafe>(TargetRef);

		FUeAgentLandscapeSimpleAuthoringProviderConfig ProviderConfig;
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
			TEXT("owner:sa008-landscape-e4");
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
		FUeAgentLandscapeSimpleAuthoringError ProviderError;
		if (!TestTrue(
				TEXT("Landscape E4 family composes"),
				FUeAgentLandscapeSimpleAuthoringProviderFactory::TryCreate(
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
			TEXT("sa008-landscape-e4-integrity"),
			60.0,
			[]() { return 100.0; },
			[]() { return FString(TEXT("ticket:sa008-landscape-e4")); },
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
			TEXT("project:sa008-landscape-e4");
		ApplicationConfig.ExecutionContext.EditorInstanceId =
			TEXT("editor:sa008-landscape-e4");
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
			TEXT("session:sa008-landscape-e4");
		ApplicationConfig.RegistryEpoch = 1;
		ApplicationConfig.TimeoutSeconds = 60.0;
		ApplicationConfig.MaxFrameBytes = 4 * 1024 * 1024;
		TSharedPtr<FUeAgentSimpleAuthoringApplication> Application;
		FUeAgentSimpleAuthoringApplicationError ApplicationError;
		if (!TestTrue(
				TEXT("application accepts Landscape E4 route"),
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

		FString Payload(HeightDeltaInput);
		Payload.ReplaceInline(
			TEXT("sa008-existing"),
			TEXT("sa008-e4-existing"));
		const FUeAgentOperationResult Result = Application->Submit(
			FUeAgentOperationRequest(
				Route->DescriptorIdentity,
				TEXT("request:sa008-landscape-e4"),
				TEXT("idempotency:sa008-landscape-e4"),
				FUeAgentContractValue(Payload)),
			FUeAgentCallerContext(
				TEXT("caller:sa008-landscape-e4"),
				{TEXT("uai:landscape:write")},
				TEXT("correlation:sa008-landscape-e4")),
			MakeShared<FNeverCancelled>());
		if (!TestTrue(
				TEXT("Landscape E4 completes after reload readback"),
				IsComplete(Result)))
		{
			AddError(Result.GetValue().GetJsonValue());
			return false;
		}

		ALandscape* ReloadedLandscape = Owner.World.IsValid()
			? FindE4Landscape(*Owner.World, TargetLabel)
			: nullptr;
		TArray<float> HeightsAfter;
		TestTrue(TEXT("Landscape E4 uses a reloaded world owner"), Owner.WasReloaded());
		TestTrue(TEXT("Landscape E4 collects the original world"), Owner.WasOriginalWorldCollected());
		TestTrue(TEXT("Landscape E4 publishes the owned map"), IFileManager::Get().FileExists(*Owner.HeaderPath()));
		if (TestNotNull(TEXT("reloaded Landscape is independently found"), ReloadedLandscape))
		{
			TestTrue(TEXT("reloaded Landscape region is readable"), ReadE4LandscapeHeights(*ReloadedLandscape, HeightsAfter));
		}
		const TArray<float> ExpectedDelta = {
			1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f,
		};
		TestEqual(TEXT("Landscape E4 sample count is stable"), HeightsAfter.Num(), ExpectedDelta.Num());
		for (int32 Index = 0;
			Index < HeightsBefore.Num()
				&& Index < HeightsAfter.Num()
				&& Index < ExpectedDelta.Num();
			++Index)
		{
			TestTrue(
				*FString::Printf(TEXT("Landscape E4 sample %d persists"), Index),
				FMath::IsNearlyEqual(
					HeightsAfter[Index],
					HeightsBefore[Index] + ExpectedDelta[Index],
					0.01f));
		}
		TestEqual(TEXT("Landscape E4 C9 reloads exactly once"), ReloadCatalog->EnumerateCount, 1);
		if (ReloadedLandscape)
		{
			TestEqual(
				TEXT("Landscape E4 C9 preserves the exact managed identity"),
				UeAgentLevelAuthoring::GetManagedActorId(
					*ReloadedLandscape),
				TargetRef);
		}
		TestEqual(
			TEXT("Landscape E4 C9 does not require the legacy reference fallback"),
			ReloadResolver->ResolveCount,
			0);
		TestEqual(TEXT("Landscape E4 archives one verified journal"), CountCompletedJournals(Owner.JournalRoot), 1);
		TestEqual(TEXT("Landscape E4 leaves no active transaction"), CountActiveTransactions(Owner.JournalRoot), 0);
	}
	TestFalse(
		TEXT("Landscape E4 removes all owned disk artifacts"),
		IFileManager::Get().DirectoryExists(*OwnedRoot));
	return !HasAnyErrors();
}

#include "DomainConformance/UeAgentDomainConformanceHarness.h"

#include "Core/HjsonWorkspace/SourceLoader/UeAgentHjsonSourceLoader.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "OpenSSL/sha.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::UAI::DomainConformance
{
	using namespace DomainCapability;

	namespace
	{
		bool IsExactIdentity(const FString& Value)
		{
			FString Normalized = Value;
			Normalized.TrimStartAndEndInline();
			return !Normalized.IsEmpty() && Normalized == Value;
		}

		bool Fail(FString& OutError, const FString& Code)
		{
			OutError = Code;
			return false;
		}

		ContextResource::FUeAgentResourceTracker MakeResources()
		{
			return ContextResource::FUeAgentResourceTracker(
				ContextResource::FUeAgentResourceBudget({
					{ContextResource::EUeAgentResourceDimension::InputBytes,
						4 * 1024 * 1024},
					{ContextResource::EUeAgentResourceDimension::OutputBytes,
						4 * 1024 * 1024},
					{ContextResource::EUeAgentResourceDimension::StringBytes,
						4 * 1024 * 1024},
					{ContextResource::EUeAgentResourceDimension::Nodes, 16384},
					{ContextResource::EUeAgentResourceDimension::Items, 16384},
					{ContextResource::EUeAgentResourceDimension::ReferenceEdges,
						16384},
					{ContextResource::EUeAgentResourceDimension::WorkUnits,
						131072},
					{ContextResource::EUeAgentResourceDimension::
						EstimatedMemoryBytes, 32 * 1024 * 1024},
				}));
		}

		FString Sha256Bytes(const uint8* Data, const int32 Num)
		{
			uint8 Digest[SHA256_DIGEST_LENGTH] = {};
			const uint8 Empty = 0;
			SHA256(
				Num > 0 ? Data : &Empty,
				static_cast<size_t>(FMath::Max(Num, 0)),
				Digest);
			return TEXT("sha256:")
				+ BytesToHex(Digest, SHA256_DIGEST_LENGTH).ToLower();
		}

		FString Sha256Text(const FString& Text)
		{
			FTCHARToUTF8 Utf8(*Text);
			return Sha256Bytes(
				reinterpret_cast<const uint8*>(Utf8.Get()),
				Utf8.Length());
		}

		bool TryCanonicalSemanticHash(
			const FString& Document,
			const FString& SourceUnit,
			FString& OutHash,
			FString& OutError)
		{
			OutHash.Reset();
			ContextResource::FUeAgentResourceTracker Resources =
				MakeResources();
			HjsonWorkspace::FHjsonDocumentPtr Parsed;
			HjsonWorkspace::FWorkspaceError WorkspaceError;
			if (!HjsonWorkspace::TryParseHjsonDocument(
				Document,
				SourceUnit,
				Resources,
				Parsed,
				WorkspaceError)
				|| !Parsed.IsValid())
			{
				return Fail(
					OutError,
					TEXT("domain_conformance_fixture_hjson_invalid:")
						+ WorkspaceError.GetCode());
			}
			TArray<uint8> Canonical;
			if (!HjsonWorkspace::TryWriteHjsonDocument(
				Parsed.ToSharedRef(),
				Resources,
				Canonical,
				WorkspaceError))
			{
				return Fail(
					OutError,
					TEXT("domain_conformance_fixture_canonicalize_failed:")
						+ WorkspaceError.GetCode());
			}
			OutHash = Sha256Bytes(Canonical.GetData(), Canonical.Num());
			return true;
		}

		bool WriteUtf8(
			const FString& Path,
			const FString& Text,
			FString& OutError)
		{
			const FString Parent = FPaths::GetPath(Path);
			if (!IFileManager::Get().MakeDirectory(*Parent, true)
				|| !FFileHelper::SaveStringToFile(
					Text,
					*Path,
					FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				return Fail(
					OutError,
					TEXT("domain_conformance_fixture_write_failed:") + Path);
			}
			return true;
		}

		bool TryMakeManifest(
			const FDomainConformanceFixture& Fixture,
			const FString& ContentHash,
			FString& OutManifest,
			FString& OutError)
		{
			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(
				TEXT("workspace_schema"),
				TEXT("uai.content_graph.workspace/1"));
			Root->SetNumberField(TEXT("format_version"), 1);
			Root->SetStringField(
				TEXT("target_identity"),
				Fixture.TargetIdentity);
			Root->SetStringField(TEXT("base_revision"), Fixture.BaseRevision);
			Root->SetStringField(TEXT("entry_file"), Fixture.EntryFile);
			const TSharedRef<FJsonObject> File = MakeShared<FJsonObject>();
			File->SetStringField(TEXT("path"), Fixture.EntryFile);
			File->SetStringField(TEXT("content_hash"), ContentHash);
			Root->SetArrayField(
				TEXT("files"),
				{MakeShared<FJsonValueObject>(File)});
			Root->SetArrayField(TEXT("references"), {});
			OutManifest.Reset();
			const TSharedRef<TJsonWriter<>> Writer =
				TJsonWriterFactory<>::Create(&OutManifest);
			if (!FJsonSerializer::Serialize(Root, Writer))
			{
				return Fail(
					OutError,
					TEXT("domain_conformance_manifest_serialize_failed:" )
						+ Fixture.FixtureId);
			}
			return true;
		}

		void AddScenario(
			FDomainConformancePlan& Plan,
			const EDomainConformanceScenario Scenario,
			const EDomainConformanceLayer Layer,
			const FString& CapabilityId,
			const FString& MechanicsFamily)
		{
			if (Plan.Contains(Scenario))
			{
				return;
			}
			Plan.AddScenario({
				Scenario,
				Layer,
				CapabilityId,
				MechanicsFamily,
			});
		}

		FString LayerId(const EDomainConformanceLayer Layer)
		{
			switch (Layer)
			{
			case EDomainConformanceLayer::StaticContract:
				return TEXT("static_contract");
			case EDomainConformanceLayer::DomainAdapter:
				return TEXT("domain_adapter");
			case EDomainConformanceLayer::PublicProduct:
				return TEXT("public_product");
			default:
				return TEXT("unknown");
			}
		}
	}

	bool FDomainConformanceFixture::IsValid() const
	{
		return IsExactIdentity(FixtureId)
			&& IsExactIdentity(DomainId)
			&& IsExactIdentity(TargetIdentity)
			&& IsExactIdentity(BaseRevision)
			&& IsExactIdentity(MechanicsFamily)
			&& IsExactIdentity(PublicRouteId);
	}

	bool FDomainConformancePlan::IsValid() const
	{
		return bValid
			&& IsExactIdentity(DomainId)
			&& IsExactIdentity(FixtureId)
			&& !Scenarios.IsEmpty();
	}

	bool FDomainConformancePlan::Contains(
		const EDomainConformanceScenario Scenario) const
	{
		return Scenarios.ContainsByPredicate(
			[Scenario](const FDomainConformanceScenarioEntry& Entry)
			{
				return Entry.Scenario == Scenario;
			});
	}

	const FString& FDomainConformancePlan::GetDomainId() const
	{
		return DomainId;
	}

	const FString& FDomainConformancePlan::GetFixtureId() const
	{
		return FixtureId;
	}

	const TArray<FDomainConformanceScenarioEntry>&
	FDomainConformancePlan::GetScenarios() const
	{
		return Scenarios;
	}

	void FDomainConformancePlan::AddScenario(
		FDomainConformanceScenarioEntry Entry)
	{
		Scenarios.Add(MoveTemp(Entry));
	}

	FNullHjsonDomainProvider::FNullHjsonDomainProvider(FString InProviderId)
		: ProviderId(MoveTemp(InProviderId))
	{
	}

	const FString& FNullHjsonDomainProvider::GetProviderId() const
	{
		return ProviderId;
	}

	bool FNullHjsonDomainProvider::Matches(
		const HjsonWorkspace::FImportInitializationDomainRequest&) const
	{
		return false;
	}

	bool FNullHjsonDomainProvider::TryResolveInitializationContract(
		const HjsonWorkspace::FImportInitializationDomainRequest&,
		FString& OutInitializationContractId,
		HjsonWorkspace::FWorkspaceError& OutError) const
	{
		OutInitializationContractId.Reset();
		OutError.Set(TEXT("domain_conformance_null_provider"));
		return false;
	}

	bool FNullHjsonDomainProvider::TryExecute(
		const HjsonWorkspace::FImportInitializationDomainRequest&,
		const FString&,
		HjsonWorkspace::FImportInitializationDomainResponse& OutResponse,
		HjsonWorkspace::FWorkspaceError& OutError) const
	{
		OutResponse = {};
		OutError.Set(TEXT("domain_conformance_null_provider"));
		return false;
	}

	TSharedPtr<FJsonObject>
	FDomainWorkspaceCarrier::MakePublicSourceObject() const
	{
		const TSharedRef<FJsonObject> Source = MakeShared<FJsonObject>();
		switch (Kind)
		{
		case HjsonWorkspace::EWorkspaceSourceKind::Inline:
			Source->SetStringField(TEXT("kind"), TEXT("inline"));
			Source->SetStringField(TEXT("document"), InlineDocument);
			break;
		case HjsonWorkspace::EWorkspaceSourceKind::SingleFile:
			Source->SetStringField(TEXT("kind"), TEXT("single_file"));
			Source->SetStringField(TEXT("path"), Path);
			break;
		case HjsonWorkspace::EWorkspaceSourceKind::Directory:
			Source->SetStringField(TEXT("kind"), TEXT("directory"));
			Source->SetStringField(TEXT("path"), Path);
			break;
		default:
			return nullptr;
		}
		return Source;
	}

	bool FDomainWorkspaceCarrier::IsValid() const
	{
		if (!IsExactIdentity(CarrierId))
		{
			return false;
		}
		return Kind == HjsonWorkspace::EWorkspaceSourceKind::Inline
			? !InlineDocument.IsEmpty() && Path.IsEmpty()
			: FPaths::IsRelative(Path) == false && InlineDocument.IsEmpty();
	}

	bool FDomainWorkspaceFixture::IsValid() const
	{
		return bValid
			&& IsExactIdentity(FixtureId)
			&& IsExactIdentity(TargetIdentity)
			&& IsExactIdentity(CanonicalSemanticHash)
			&& Carriers.Num() == 3
			&& Carriers.ContainsByPredicate(
				[](const FDomainWorkspaceCarrier& Carrier)
				{
					return Carrier.Kind
						== HjsonWorkspace::EWorkspaceSourceKind::Inline;
				})
			&& Carriers.ContainsByPredicate(
				[](const FDomainWorkspaceCarrier& Carrier)
				{
					return Carrier.Kind
						== HjsonWorkspace::EWorkspaceSourceKind::SingleFile;
				})
			&& Carriers.ContainsByPredicate(
				[](const FDomainWorkspaceCarrier& Carrier)
				{
					return Carrier.Kind
						== HjsonWorkspace::EWorkspaceSourceKind::Directory;
				});
	}

	const FString& FDomainWorkspaceFixture::GetFixtureId() const
	{
		return FixtureId;
	}

	const FString& FDomainWorkspaceFixture::GetTargetIdentity() const
	{
		return TargetIdentity;
	}

	const FString& FDomainWorkspaceFixture::GetCanonicalSemanticHash() const
	{
		return CanonicalSemanticHash;
	}

	const TArray<FDomainWorkspaceCarrier>&
	FDomainWorkspaceFixture::GetCarriers() const
	{
		return Carriers;
	}

	bool FDomainWorkspaceFixture::TryOverwriteSingleFileForTest(
		const FString& Document,
		FString& OutError)
	{
		for (FDomainWorkspaceCarrier& Carrier : Carriers)
		{
			if (Carrier.Kind
				== HjsonWorkspace::EWorkspaceSourceKind::SingleFile)
			{
				return WriteUtf8(Carrier.Path, Document, OutError);
			}
		}
		return Fail(
			OutError,
			TEXT("domain_conformance_single_file_carrier_missing"));
	}

	void FDomainConformanceLayerResults::Record(
		const EDomainConformanceLayer Layer,
		const EDomainConformanceStatus Status,
		FString EvidenceRef)
	{
		Entries.Add(Layer, {Status, MoveTemp(EvidenceRef)});
	}

	EDomainConformanceStatus FDomainConformanceLayerResults::GetStatus(
		const EDomainConformanceLayer Layer) const
	{
		const FEntry* Entry = Entries.Find(Layer);
		return Entry ? Entry->Status : EDomainConformanceStatus::NotRun;
	}

	const FString& FDomainConformanceLayerResults::GetEvidenceRef(
		const EDomainConformanceLayer Layer) const
	{
		const FEntry* Entry = Entries.Find(Layer);
		static const FString Empty;
		return Entry ? Entry->EvidenceRef : Empty;
	}

	bool FDomainOwnedWorkerBatch::IsValid() const
	{
		return IsExactIdentity(BatchId)
			&& IsExactIdentity(MechanicsFamily)
			&& !TestIds.IsEmpty()
			&& bRequiresHeadless
			&& bPreserveMainEditor
			&& CleanupPolicy
				== TEXT("owned_processes_and_temp_artifacts");
	}

	bool TryBuildDomainConformancePlan(
		TSharedRef<const FDomainCapabilityContribution> Contribution,
		const FDomainConformanceFixture& Fixture,
		FDomainConformancePlan& OutPlan,
		FString& OutError)
	{
		OutPlan = {};
		OutError.Reset();
		if (!Fixture.IsValid())
		{
			return Fail(
				OutError,
				TEXT("domain_conformance_fixture_invalid:")
					+ Fixture.FixtureId);
		}
		if (Fixture.DomainId != Contribution->GetDomainId())
		{
			return Fail(
				OutError,
				TEXT("domain_conformance_fixture_domain_mismatch:")
					+ Fixture.DomainId);
		}
		const FDomainCapabilityDeclaration& Declaration =
			Contribution->GetDeclaration();
		if (Declaration.Hjson.Support == EDomainCapabilitySupport::Supported
			&& (!IsExactIdentity(Fixture.EntryFile)
				|| Fixture.CanonicalDocument.IsEmpty()
				|| FPaths::IsRelative(Fixture.WorkspaceRoot)))
		{
			return Fail(
				OutError,
				TEXT("domain_conformance_hjson_fixture_invalid:")
					+ Fixture.FixtureId);
		}
		const bool bWrite = Declaration.Hjson.Support
				== EDomainCapabilitySupport::Supported
			|| Declaration.Operation.Support
				== EDomainCapabilitySupport::Supported
			|| Declaration.Layout.Support
				== EDomainCapabilitySupport::Supported;
		const bool bPersistedWrite = Declaration.Hjson.Support
				== EDomainCapabilitySupport::Supported
			|| Declaration.Layout.Support
				== EDomainCapabilitySupport::Supported
			|| Declaration.Operation.Operations.ContainsByPredicate(
				[](const FDomainOperationEntry& Operation)
				{
					return Operation.bPersists;
				});
		if (!Fixture.bRealMechanics || !Fixture.bPublicRoute)
		{
			return Fail(
				OutError,
				TEXT("domain_conformance_real_public_fixture_missing:")
					+ Fixture.FixtureId);
		}
		if (bWrite && !Fixture.bIndependentReadback)
		{
			return Fail(
				OutError,
				TEXT("domain_conformance_independent_readback_missing:")
					+ Fixture.FixtureId);
		}
		if (bPersistedWrite && !Fixture.bPersistReload)
		{
			return Fail(
				OutError,
				TEXT("domain_conformance_persist_reload_missing:")
					+ Fixture.FixtureId);
		}
		if (bWrite && !Fixture.bFailureCleanup)
		{
			return Fail(
				OutError,
				TEXT("domain_conformance_failure_cleanup_missing:")
					+ Fixture.FixtureId);
		}
		if (Declaration.Migration.Support
				== EDomainCapabilitySupport::Supported
			&& !Fixture.bLegacyExit)
		{
			return Fail(
				OutError,
				TEXT("domain_conformance_legacy_exit_missing:")
					+ Fixture.FixtureId);
		}

		OutPlan.DomainId = Fixture.DomainId;
		OutPlan.FixtureId = Fixture.FixtureId;
		AddScenario(
			OutPlan,
			EDomainConformanceScenario::CapabilityFreeze,
			EDomainConformanceLayer::StaticContract,
			Fixture.DomainId,
			Fixture.MechanicsFamily);
		if (Declaration.Command.Support
				== EDomainCapabilitySupport::Supported
			|| Declaration.Hjson.Support
				== EDomainCapabilitySupport::Supported
			|| Declaration.Composition.Support
				== EDomainCapabilitySupport::Supported)
		{
			AddScenario(
				OutPlan,
				EDomainConformanceScenario::ProcessAuthorityAdoption,
				EDomainConformanceLayer::StaticContract,
				TEXT("process:authority_adoption"),
				Fixture.MechanicsFamily);
		}
		if (Declaration.Composition.Support
			== EDomainCapabilitySupport::Supported)
		{
			for (const HjsonWorkspace::FTargetCompositionContract& Contract :
				Declaration.Composition.Contracts)
			{
				if (Contract.SemanticRoutes.IsEmpty())
				{
					return Fail(
						OutError,
						TEXT("domain_conformance_composition_routes_missing:")
							+ Contract.TargetKind);
				}
			}
			AddScenario(
				OutPlan,
				EDomainConformanceScenario::CompositionSemanticRoutes,
				EDomainConformanceLayer::StaticContract,
				TEXT("composition:semantic_routes"),
				Fixture.MechanicsFamily);
		}
		if (Declaration.Query.Support
			== EDomainCapabilitySupport::Supported)
		{
			AddScenario(
				OutPlan,
				EDomainConformanceScenario::QueryReadOnlyCompleteness,
				EDomainConformanceLayer::DomainAdapter,
				TEXT("query"),
				Fixture.MechanicsFamily);
		}
		if (Declaration.Discovery.Support
			== EDomainCapabilitySupport::Supported)
		{
			AddScenario(
				OutPlan,
				EDomainConformanceScenario::CandidateDirectRead,
				EDomainConformanceLayer::DomainAdapter,
				TEXT("discovery"),
				Fixture.MechanicsFamily);
		}
		if (Declaration.Hjson.Support
			== EDomainCapabilitySupport::Supported)
		{
			AddScenario(OutPlan,
				EDomainConformanceScenario::InitializationContract,
				EDomainConformanceLayer::StaticContract,
				TEXT("initialization"), Fixture.MechanicsFamily);
			AddScenario(OutPlan,
				EDomainConformanceScenario::HjsonInline,
				EDomainConformanceLayer::DomainAdapter,
				TEXT("hjson:inline"), Fixture.MechanicsFamily);
			AddScenario(OutPlan,
				EDomainConformanceScenario::HjsonSingleFile,
				EDomainConformanceLayer::DomainAdapter,
				TEXT("hjson:single_file"), Fixture.MechanicsFamily);
			AddScenario(OutPlan,
				EDomainConformanceScenario::HjsonDirectory,
				EDomainConformanceLayer::DomainAdapter,
				TEXT("hjson:directory"), Fixture.MechanicsFamily);
			AddScenario(OutPlan,
				EDomainConformanceScenario::ExplicitIntentRevision,
				EDomainConformanceLayer::DomainAdapter,
				TEXT("hjson:intent_revision"), Fixture.MechanicsFamily);
		}
		if (bWrite)
		{
			AddScenario(OutPlan,
				EDomainConformanceScenario::StaleRevisionReject,
				EDomainConformanceLayer::PublicProduct,
				TEXT("write:stale"), Fixture.MechanicsFamily);
			AddScenario(OutPlan,
				EDomainConformanceScenario::OperationStages,
				EDomainConformanceLayer::DomainAdapter,
				TEXT("write:lifecycle"), Fixture.MechanicsFamily);
			AddScenario(OutPlan,
				EDomainConformanceScenario::NoPrematureSuccess,
				EDomainConformanceLayer::PublicProduct,
				TEXT("write:terminal"), Fixture.MechanicsFamily);
			AddScenario(OutPlan,
				EDomainConformanceScenario::PersistReloadReadback,
				EDomainConformanceLayer::PublicProduct,
				TEXT("write:readback"), Fixture.MechanicsFamily);
			AddScenario(OutPlan,
				EDomainConformanceScenario::FailureCleanup,
				EDomainConformanceLayer::PublicProduct,
				TEXT("write:cleanup"), Fixture.MechanicsFamily);
		}
		if (Declaration.Operation.Support
			== EDomainCapabilitySupport::Supported)
		{
			AddScenario(
				OutPlan,
				EDomainConformanceScenario::OperationRequestFactory,
				EDomainConformanceLayer::StaticContract,
				TEXT("operation:request_factory"),
				Fixture.MechanicsFamily);
		}
		for (const FDomainOperationEntry& Operation :
			Declaration.Operation.Operations)
		{
			if (Operation.ExecutionPolicy
				== EDomainOperationExecutionPolicy::LongRunning)
			{
				AddScenario(OutPlan,
					EDomainConformanceScenario::LongRunningTerminal,
					EDomainConformanceLayer::PublicProduct,
					Operation.OperationId, Fixture.MechanicsFamily);
			}
			if (Operation.TransientStatePolicy
				== EDomainTransientStatePolicy::RecoverableSession)
			{
				AddScenario(OutPlan,
					EDomainConformanceScenario::TransientStateRecovery,
					EDomainConformanceLayer::PublicProduct,
					Operation.OperationId, Fixture.MechanicsFamily);
			}
		}
		if (Declaration.Layout.Support
			== EDomainCapabilitySupport::Supported)
		{
			AddScenario(OutPlan,
				EDomainConformanceScenario::GraphLayout,
				EDomainConformanceLayer::PublicProduct,
				TEXT("layout"), Fixture.MechanicsFamily);
		}
		if (Declaration.Migration.Support
			== EDomainCapabilitySupport::Supported)
		{
			AddScenario(OutPlan,
				EDomainConformanceScenario::LegacyExit,
				EDomainConformanceLayer::StaticContract,
				Declaration.Migration.OwnerId, Fixture.MechanicsFamily);
		}
		AddScenario(OutPlan,
			EDomainConformanceScenario::PublicRoute,
			EDomainConformanceLayer::PublicProduct,
			Fixture.PublicRouteId, Fixture.MechanicsFamily);
		OutPlan.bValid = true;
		return true;
	}

	bool ValidateRequestScopedOperationFactory(
		TSharedRef<const FDomainCapabilityContribution> Contribution,
		const FString& OperationId,
		const CommandEdge::FUeAgentCommandRequest& FirstRequest,
		const CommandEdge::FUeAgentCommandRequest& SecondRequest,
		const Contract::FUeAgentCallerContext& CallerContext,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation,
		FString& OutError)
	{
		OutError.Reset();
		const FDomainOperationEntry* Operation =
			Contribution->GetDeclaration().Operation.Operations.FindByPredicate(
				[&OperationId](const FDomainOperationEntry& Entry)
				{
					return Entry.OperationId == OperationId;
				});
		if (!Operation)
		{
			return Fail(
				OutError,
				TEXT("domain_conformance_operation_missing:") + OperationId);
		}
		if (!Operation->InvocationFactory.IsValid())
		{
			return Fail(
				OutError,
				TEXT("domain_conformance_operation_factory_missing:")
					+ OperationId);
		}
		if (Operation->InvocationFactory->GetOperationId() != OperationId)
		{
			return Fail(
				OutError,
				TEXT("domain_conformance_operation_factory_identity_mismatch:")
					+ OperationId + TEXT(":")
					+ Operation->InvocationFactory->GetOperationId());
		}

		FDomainOperationInvocation First;
		FDomainOperationInvocation Second;
		FDomainOperationInvocationError InvocationError;
		if (!Operation->InvocationFactory->TryCreate(
				FirstRequest,
				CallerContext,
				Cancellation,
				First,
				InvocationError))
		{
			return Fail(
				OutError,
				TEXT("domain_conformance_operation_factory_first_failed:")
					+ InvocationError.GetCode());
		}
		if (!Operation->InvocationFactory->TryCreate(
				SecondRequest,
				CallerContext,
				Cancellation,
				Second,
				InvocationError))
		{
			return Fail(
				OutError,
				TEXT("domain_conformance_operation_factory_second_failed:")
					+ InvocationError.GetCode());
		}
		if (!First.IsValid() || !Second.IsValid())
		{
			return Fail(
				OutError,
				TEXT("domain_conformance_operation_invocation_invalid:")
					+ OperationId);
		}
		if (&First.GetPlan().Get() == &Second.GetPlan().Get()
			|| &First.GetAuthorization().Get()
				== &Second.GetAuthorization().Get()
			|| &First.GetService().Get() == &Second.GetService().Get()
			|| &First.GetContext().Get() == &Second.GetContext().Get())
		{
			return Fail(
				OutError,
				TEXT("domain_conformance_operation_invocation_shared_state:")
					+ OperationId);
		}
		if (First.GetPlan()->GetPlanId() == Second.GetPlan()->GetPlanId())
		{
			return Fail(
				OutError,
				TEXT("domain_conformance_operation_plan_identity_reused:")
					+ OperationId);
		}
		return true;
	}

	bool TryBuildEquivalentWorkspaceFixture(
		const FDomainConformanceFixture& Fixture,
		FDomainWorkspaceFixture& OutWorkspace,
		FString& OutError)
	{
		OutWorkspace = {};
		OutError.Reset();
		if (!Fixture.IsValid())
		{
			return Fail(
				OutError,
				TEXT("domain_conformance_fixture_invalid:")
					+ Fixture.FixtureId);
		}
		FString CanonicalHash;
		if (!TryCanonicalSemanticHash(
			Fixture.CanonicalDocument,
			TEXT("inline:domain-conformance"),
			CanonicalHash,
			OutError))
		{
			return false;
		}
		const FString SingleFile = FPaths::Combine(
			Fixture.WorkspaceRoot,
			TEXT("single_file"),
			Fixture.EntryFile);
		const FString DirectoryRoot = FPaths::Combine(
			Fixture.WorkspaceRoot,
			TEXT("directory"));
		const FString DirectoryEntry = FPaths::Combine(
			DirectoryRoot,
			Fixture.EntryFile);
		const FString ManifestPath = FPaths::Combine(
			DirectoryRoot,
			TEXT(".uai/workspace.hjson"));
		if (!WriteUtf8(SingleFile, Fixture.CanonicalDocument, OutError)
			|| !WriteUtf8(
				DirectoryEntry,
				Fixture.CanonicalDocument,
				OutError))
		{
			return false;
		}
		FString Manifest;
		if (!TryMakeManifest(
			Fixture,
			Sha256Text(Fixture.CanonicalDocument),
			Manifest,
			OutError)
			|| !WriteUtf8(ManifestPath, Manifest, OutError))
		{
			return false;
		}

		OutWorkspace.FixtureId = Fixture.FixtureId;
		OutWorkspace.TargetIdentity = Fixture.TargetIdentity;
		OutWorkspace.CanonicalSemanticHash = CanonicalHash;
		OutWorkspace.Carriers = {
			{
				HjsonWorkspace::EWorkspaceSourceKind::Inline,
				TEXT("inline"),
				FString(),
				Fixture.CanonicalDocument,
			},
			{
				HjsonWorkspace::EWorkspaceSourceKind::SingleFile,
				TEXT("single_file"),
				SingleFile,
				FString(),
			},
			{
				HjsonWorkspace::EWorkspaceSourceKind::Directory,
				TEXT("directory"),
				DirectoryRoot,
				FString(),
			},
		};
		OutWorkspace.bValid = true;
		if (!OutWorkspace.IsValid())
		{
			OutWorkspace = {};
			return Fail(
				OutError,
				TEXT("domain_conformance_workspace_fixture_invalid:")
					+ Fixture.FixtureId);
		}
		return ValidateEquivalentWorkspaceFixture(OutWorkspace, OutError);
	}

	bool ValidateEquivalentWorkspaceFixture(
		const FDomainWorkspaceFixture& Workspace,
		FString& OutError)
	{
		OutError.Reset();
		if (!Workspace.IsValid())
		{
			return Fail(
				OutError,
				TEXT("domain_conformance_workspace_fixture_invalid:")
					+ Workspace.FixtureId);
		}
		for (const FDomainWorkspaceCarrier& Carrier : Workspace.Carriers)
		{
			ContextResource::FUeAgentResourceTracker Resources =
				MakeResources();
			HjsonWorkspace::FResolvedContentGraphImportSourcePtr Source;
			HjsonWorkspace::FWorkspaceError WorkspaceError;
			if (!HjsonWorkspace::TryLoadContentGraphImportSource(
				Carrier.MakePublicSourceObject(),
				Workspace.TargetIdentity,
				Resources,
				Source,
				WorkspaceError)
				|| !Source.IsValid()
				|| !Source->GetEntryDocument())
			{
				return Fail(
					OutError,
					TEXT("domain_conformance_source_load_failed:")
						+ Carrier.CarrierId + TEXT(":")
						+ WorkspaceError.GetCode());
			}
			FString Hash;
			if (!TryCanonicalSemanticHash(
				Source->GetEntryDocument()->GetDocument(),
				Carrier.CarrierId,
				Hash,
				OutError))
			{
				return false;
			}
			if (Hash != Workspace.CanonicalSemanticHash)
			{
				return Fail(
					OutError,
					TEXT("domain_conformance_source_semantic_drift:")
						+ Carrier.CarrierId);
			}
		}
		return true;
	}

	bool TryBuildOwnedWorkerBatch(
		const FDomainConformancePlan& Plan,
		const FDomainConformanceFixture& Fixture,
		const FDomainConformanceLayerResults& Results,
		FDomainOwnedWorkerBatch& OutBatch,
		FString& OutError)
	{
		OutBatch = {};
		OutError.Reset();
		if (!Plan.IsValid() || !Fixture.IsValid()
			|| Plan.GetDomainId() != Fixture.DomainId
			|| Plan.GetFixtureId() != Fixture.FixtureId)
		{
			return Fail(OutError, TEXT("domain_conformance_plan_invalid"));
		}
		for (const EDomainConformanceLayer Layer : {
			EDomainConformanceLayer::StaticContract,
			EDomainConformanceLayer::DomainAdapter})
		{
			if (Results.GetStatus(Layer)
				!= EDomainConformanceStatus::Passed
				|| !IsExactIdentity(Results.GetEvidenceRef(Layer)))
			{
				return Fail(
					OutError,
					TEXT("domain_conformance_low_cost_layer_incomplete:")
						+ LayerId(Layer));
			}
		}
		OutBatch.BatchId = TEXT("domain-conformance-") + Fixture.DomainId;
		OutBatch.MechanicsFamily = Fixture.MechanicsFamily;
		OutBatch.TestIds.Add(
			Fixture.PublicAutomationTestId.IsEmpty()
				? TEXT("UeAgentInterface.DomainConformance.Public.")
					+ Fixture.DomainId
				: Fixture.PublicAutomationTestId);
		OutBatch.bRequiresHeadless = true;
		OutBatch.bPreserveMainEditor = true;
		OutBatch.CleanupPolicy =
			TEXT("owned_processes_and_temp_artifacts");
		return OutBatch.IsValid()
			|| Fail(OutError, TEXT("domain_conformance_owned_batch_invalid"));
	}

	bool ValidateProductEvidence(
		const FDomainConformanceFixture& Fixture,
		const FDomainProductEvidence& Evidence,
		FString& OutError)
	{
		OutError.Reset();
		if (!Fixture.IsValid()
			|| Evidence.RouteId != Fixture.PublicRouteId
			|| !IsExactIdentity(Evidence.ReceiptRef))
		{
			return Fail(
				OutError,
				TEXT("domain_conformance_product_evidence_identity_invalid:")
					+ Evidence.ReceiptRef);
		}
		if (Evidence.bSynthetic)
		{
			return Fail(
				OutError,
				TEXT("domain_conformance_synthetic_product_evidence:")
					+ Evidence.ReceiptRef);
		}
		if (!Evidence.bReceiptExists
			|| !Evidence.bPublicRouteObserved
			|| !Evidence.bIndependentReadbackObserved
			|| !Evidence.bCleanupObserved)
		{
			return Fail(
				OutError,
				TEXT("domain_conformance_public_product_evidence_incomplete:")
					+ Evidence.ReceiptRef);
		}
		return true;
	}
}

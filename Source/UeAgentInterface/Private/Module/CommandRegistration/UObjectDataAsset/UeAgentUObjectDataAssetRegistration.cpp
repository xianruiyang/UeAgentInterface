#include "Module/CommandRegistration/UObjectDataAsset/UeAgentUObjectDataAssetRegistration.h"

#include "Utilities/UeAgentJsonDiagnostics.h"
#include "Utilities/UeAgentThreadCpuTiming.h"

#include "Dom/JsonValue.h"
#include "HAL/PlatformTime.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Utilities/AuthoringExport/UeAgentExportWorkspaceWriter.h"

#include "CommandHandlers/UObjectDataAsset/UeAgentUObjectDataAssetCommandHandlers.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "Core/HjsonWorkspace/SourceLoader/UeAgentHjsonSourceLoader.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceResolver.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringDurableTransaction.h"
#include "Core/HjsonWorkspace/UeAgentWorkspaceRefresh.h"
#include "Core/HjsonWorkspace/UeAgentCommittedWorkspaceExport.h"
#include "Core/ArtifactPublisher/UeAgentArtifactPublisher.h"

namespace UE::UAI::Module::CommandRegistration::UObjectDataAsset
{
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandHandlers::UObjectDataAsset;
	using namespace CommandRegistry;
	using namespace Module::CommandRegistration::Active;
	using namespace OperationOwners::UObjectDataAsset;

	namespace
	{
		const FString Owner =
			TEXT("content-graph-workspace-owner");

		FUeAgentCommandFieldDeclaration Field(
			const TCHAR* Name,
			const TCHAR* WireType,
			const bool bRequired,
			const TCHAR* Description,
			FString SchemaJson = FString())
		{
			return {
				Name,
				WireType,
				bRequired,
				false,
				Description,
				MoveTemp(SchemaJson),
			};
		}

		FUeAgentCommandErrorDeclaration Error(
			const TCHAR* Code,
			const TCHAR* Description)
		{
			return {Code, Description};
		}

		FUeAgentCommandDeclaration Declaration(
			const FUeAgentUObjectDataAssetCommandRoute& Route)
		{
			FUeAgentCommandDeclaration Result;
			Result.CommandId = Route.CommandId;
			Result.PythonNamespace = TEXT("content_graph");
			Result.PythonFunction = Route.PythonFunction;
			Result.ContractVersion = 1;
			Result.PublicOwner = Owner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = Route.Kind ==
					EUeAgentUObjectDataAssetRouteKind::Import
				? EUeAgentCommandSideEffect::AssetState
				: Route.Kind == EUeAgentUObjectDataAssetRouteKind::Export
					? EUeAgentCommandSideEffect::ExternalState
					: EUeAgentCommandSideEffect::None;
			Result.StateLayers = Route.Kind ==
					EUeAgentUObjectDataAssetRouteKind::Import
				? TArray<FString>{TEXT("asset_persisted_state")}
				: TArray<FString>{TEXT("asset_persisted_state")};
			Result.InputFields = {
				Field(TEXT("target"), TEXT("string"), true,
					TEXT("Exact /Game package.object identity; query results may be reused unchanged.")),
			};
			if (Route.Kind == EUeAgentUObjectDataAssetRouteKind::Import)
			{
				Result.InputFields.Append({
					Field(TEXT("source"), TEXT("object"), true,
						TEXT("Use directory for export-edit-import. A complete inline or single_file document is create-only and carries an internal missing-target expectation. Revision and impact authorization remain machine-owned."),
						HjsonWorkspace::GetContentGraphImportSourceSchemaJson()),
					Field(TEXT("analysis_ref"), TEXT("string"), false,
						TEXT("Optional scene analysis reference for a Level scene_editing directory. Apply only the analyzed HJSON and related live scene version; ordinary imports remain unchanged.")),
				});
			}
			else if (Route.Kind == EUeAgentUObjectDataAssetRouteKind::Export)
			{
				Result.InputFields.Append({
					Field(TEXT("workspace_path"), TEXT("string"), true,
						TEXT("Absolute output workspace path.")),
					Field(TEXT("profile_id"), TEXT("string"), false,
						TEXT("Optional export profile identity.")),
					Field(TEXT("export_mode"), TEXT("string"), false,
						TEXT("Defaults to committed: publish accumulated successful HJSON commits and their observed effects. Incremental synchronizes all current UE changes; full rebuilds. Empty workspaces initialize fully. Unsupported default consumers retain incremental with an explicit receipt reason; explicit committed diagnoses them."),
						TEXT("{\"enum\":[\"committed\",\"incremental\",\"full\"],\"default\":\"committed\"}")),
					Field(TEXT("workspace_format"), TEXT("string"), false,
						TEXT("v2, v3 or v4. Omitted publishes v4 with direct publication identity. v2/v3 retain the legacy whole-model fingerprint. The existing publisher migrates the same workspace atomically; old writers must not continue a v4 store."),
						TEXT("{\"enum\":[\"v2\",\"v3\",\"v4\"]}")),
					Field(TEXT("source"), TEXT("object"), false,
						TEXT("Optional complete external-input intent to prepare a workspace without importing. Source integrity remains server-owned."),
						HjsonWorkspace::GetContentGraphImportSourceSchemaJson()),
				});
			}
			else if (Route.Kind == EUeAgentUObjectDataAssetRouteKind::Query)
			{
				Result.InputFields.Append({
					Field(TEXT("input"), TEXT("object"), false,
						TEXT("Optional exact read projection input.")),
					Field(TEXT("context"), TEXT("object"), false,
						TEXT("Optional bounded read context.")),
					Field(TEXT("projection"), TEXT("object"), false,
						TEXT("Optional response projection.")),
				});
			}
			if (Route.Kind == EUeAgentUObjectDataAssetRouteKind::Import)
			{
				Result.ResultFields = {
					Field(TEXT("status"), TEXT("string"), true,
						TEXT("Succeeded only after apply, settle, persistence and independent verification.")),
				};
			}
			else if (Route.Kind == EUeAgentUObjectDataAssetRouteKind::Export)
			{
				Result.ResultFields = {
					Field(TEXT("status"), TEXT("string"), true,
						TEXT("Succeeded only after the complete workspace is published.")),
					Field(TEXT("export_mode"), TEXT("string"), false,
						TEXT("Actual published mode: incremental or full.")),
					Field(TEXT("export_reason"), TEXT("string"), false,
						TEXT("Why a requested incremental export initialized a full baseline.")),
					Field(TEXT("workspace_format"), TEXT("string"), false,
						TEXT("Actual semantic workspace format: v2, v3 or v4.")),
				};
			}
			else if (Route.Kind == EUeAgentUObjectDataAssetRouteKind::Query)
			{
				Result.ResultFields = {
					Field(TEXT("query_result"), TEXT("object"), true,
						TEXT("Readback from the real persisted UObject/DataAsset graph.")),
				};
			}
			Result.Errors = {
				Error(TEXT("content_graph_authority_required"),
					TEXT("The route-specific public authority is missing.")),
				Error(TEXT("content_graph_game_thread_required"),
					TEXT("The public ingress did not enter the UE game thread.")),
				Error(TEXT("hjson_domain_command_capability_unavailable"),
					TEXT("The target domain does not explicitly support this optional content-graph command.")),
				Error(TEXT("resource_accounting_failed"),
					TEXT("The recursive plan received invalid internal resource accounting.")),
			};
			if (Route.Kind == EUeAgentUObjectDataAssetRouteKind::Import)
			{
				Result.Errors.Append({
					Error(TEXT("content_graph_workspace_manifest_missing"),
						TEXT("A directory source is missing .uai/workspace.hjson.")),
					Error(TEXT("content_graph_workspace_digest_mismatch"),
						TEXT("A declared directory document does not match its SHA-256 digest.")),
					Error(TEXT("workspace_path_invalid"),
						TEXT("A source path is relative, escapes the workspace root, or is otherwise invalid.")),
					Error(TEXT("hjson_domain_identity_required"),
						TEXT("An inline or single-file source is not a complete domain document with schema and root type identity; use the exported directory workspace for ordinary editing.")),
					Error(TEXT("content_graph_workspace_machine_context_unavailable"),
						TEXT("The source does not carry a verified hidden revision and impact baseline; export and import a directory workspace.")),
					Error(TEXT("content_graph_public_machine_token_forbidden"),
						TEXT("Revision and authorization tokens are machine-owned and cannot be supplied as public parameters.")),
				});
			}
			else
			{
				Result.Errors.Add(
					Error(TEXT("content_graph_read_target_unresolved"),
						TEXT("The exact persisted target could not be resolved.")));
				if (Route.Kind == EUeAgentUObjectDataAssetRouteKind::Export)
				{
					Result.Errors.Add(
						Error(TEXT("workspace_path_invalid"),
							TEXT("The requested export workspace path is invalid.")));
				}
			}
			Result.Purpose.Summary = Route.Kind ==
					EUeAgentUObjectDataAssetRouteKind::Import
				? TEXT("Create or update one supported content-graph target through its registered HJSON domain provider.")
				: Route.Kind == EUeAgentUObjectDataAssetRouteKind::Export
					? TEXT("Publish a semantically verified HJSON workspace for one supported content-graph target.")
					: TEXT("Read one exact facet of a supported persisted content graph.");
			Result.Purpose.Target =
				TEXT("One exact supported content graph package.object identity.");
			Result.Purpose.Preconditions = {
				TEXT("Use the public exact command identity and contract hash unchanged."),
				TEXT("The Editor service dispatches the request on the UE game thread."),
			};
			Result.Purpose.ParameterGuidance = Route.Kind ==
					EUeAgentUObjectDataAssetRouteKind::Import
				? TEXT("For edits, export the target, change only model-visible HJSON, and import that directory workspace. Complete inline or single_file documents are create-only. The server recovers the hidden edit baseline or missing-target expectation and impact authorization; callers never copy or generate machine values.")
				: TEXT("Preserve exact type, target, field and candidate identities; do not alias them in the SDK or caller.");
			Result.Purpose.ResultGuidance =
				Route.Kind == EUeAgentUObjectDataAssetRouteKind::Export
					? TEXT("Require semantic verification and inspect scope before editing or re-importing; a domain may export a complete field set without claiming complete asset structure.")
					: Route.Kind == EUeAgentUObjectDataAssetRouteKind::Import
						? TEXT("A succeeded result already includes internal settle, persistence and independent verification; use Query or re-export to inspect the new state.")
						: TEXT("Inspect only the requested business projection.");
			Result.Purpose.FailureGuidance =
				TEXT("Validation and lifecycle failures never fall back to a legacy provider or partial success.");
			Result.Purpose.SideEffectGuidance = Route.Kind ==
					EUeAgentUObjectDataAssetRouteKind::Import
				? TEXT("Creates or updates and saves one authorized asset; confirmed rollback restores or removes partial work.")
				: Route.Kind == EUeAgentUObjectDataAssetRouteKind::Export
					? TEXT("Writes only the requested verified external workspace.")
					: TEXT("Read-only with respect to UE and external state.");
			Result.Purpose.MinimumExample = Route.Kind ==
					EUeAgentUObjectDataAssetRouteKind::Import
				? TEXT("workspace.import_changes()")
				: Route.Kind == EUeAgentUObjectDataAssetRouteKind::Export
					? TEXT("client.content_graph.export_workspace(target)")
					: Route.PythonFunction + TEXT("(client, target)");
			Result.Purpose.RelatedFlows = {
				TEXT("query.command.search"),
				TEXT("query.command.usage"),
				TEXT("content_graph.import_workspace"),
				TEXT("content_graph.export_workspace"),
			};
			Result.Purpose.Keywords = {
				TEXT("content graph"), TEXT("asset"), TEXT("hjson"),
				TEXT("workspace"),
			};
			if (Route.Kind == EUeAgentUObjectDataAssetRouteKind::Import)
			{
				Result.Purpose.Keywords.Append({
					TEXT("import"), TEXT("initialize"),
					TEXT("create"), TEXT("update"),
				});
				Result.Purpose.Synonyms = {
					TEXT("import HJSON workspace"),
					TEXT("create or update an asset from HJSON"),
					TEXT("apply an edited HJSON workspace"),
				};
			}
			else if (Route.Kind == EUeAgentUObjectDataAssetRouteKind::Export)
			{
				Result.Purpose.Keywords.Append({
					TEXT("export"), TEXT("publish"), TEXT("serialize"),
				});
				Result.Purpose.Synonyms = {
					TEXT("export HJSON workspace"),
					TEXT("publish asset content graph"),
				};
			}
			else
			{
				Result.Purpose.Keywords.Append({
					TEXT("read"), TEXT("query"), TEXT("inspect"),
					TEXT("properties"), TEXT("relations"),
				});
				Result.Purpose.Synonyms = {
					TEXT("read asset content graph"),
					TEXT("inspect persisted target structure"),
				};
			}
			return Result;
		}

		class FLazyProductionService final
			: public IUeAgentUObjectDataAssetCommandService
		{
		public:
			bool Execute(
				const FUeAgentUObjectDataAssetCommandRequest& Request,
				FUeAgentUObjectDataAssetCommandResponse& OutResponse,
				FUeAgentUObjectDataAssetCommandError& OutError) override
			{
				TSharedPtr<IUeAgentUObjectDataAssetCommandService> Service;
				UE::UAI::Contract::FDiagnostic CreationError;
				if (!TryCreateProductionUObjectDataAssetCommandService(
						Service,
						CreationError)
					|| !Service.IsValid())
				{
					OutResponse = {};
					OutError = CreationError.IsEmpty() ? UE::UAI::Contract::FDiagnostic::Failure(TEXT("uobject_dataasset_service_unavailable"), TEXT("The UObject data asset service could not be created."), TEXT("service_creation")) : CreationError;
					return false;
				}
				return Service->Execute(Request, OutResponse, OutError);
			}
		};

		class FMachineContextCustodyService final
			: public IUeAgentUObjectDataAssetCommandService
		{
		public:
			explicit FMachineContextCustodyService(
				TSharedRef<IUeAgentUObjectDataAssetCommandService> InInner)
				: Inner(MoveTemp(InInner))
			{
			}

			bool Execute(
				const FUeAgentUObjectDataAssetCommandRequest& Request,
				FUeAgentUObjectDataAssetCommandResponse& OutResponse,
				FUeAgentUObjectDataAssetCommandError& OutError) override
			{
				if (Request.CommandId != TEXT("content_graph.import_workspace"))
				{
					if (Request.CommandId == TEXT("content_graph.export_workspace"))
						return ExecuteExport(Request, OutResponse, OutError);
					return Inner->Execute(Request, OutResponse, OutError);
				}
				const double ImportStarted = FPlatformTime::Seconds();
				UeAgentAuthoringWorkspaceTransaction::FScopedDurableConsumerHistory HistoryScope;
				UeAgentAuthoringWorkspaceTransaction::FScopedDurableRecoveryNotice RecoveryNotice(Request.RequestId);
				UeAgentAuthoringDiagnostics::FScopedWarnings OperationWarnings;
				const auto CustodyJson = [&]() {
					TArray<TSharedPtr<FJsonValue>> Journals;
					for (const auto& Id : RecoveryNotice.GetJournalIds()) {
						const auto Journal = MakeShared<FJsonObject>(); Journal->SetStringField(TEXT("journal_id"), Id);
						Journals.Add(MakeShared<FJsonValueObject>(Journal));
					}
					return Journals;
				};
				ON_SCOPE_EXIT {
					if (!OutError.IsEmpty() && !RecoveryNotice.GetJournalIds().IsEmpty()) {
						auto Notice = Contract::FDiagnostic::Failure(TEXT("durable_recovery_custody_available"),
							TEXT("Recovery custody for the original import is retained; do not replay its native action."), TEXT("operation_recovery"));
						Notice.Severity = Contract::ESeverity::Warning;
						const auto Evidence = MakeShared<FJsonObject>(); Evidence->SetArrayField(TEXT("durable_journals"), CustodyJson());
						Notice.ActualEvidence = MakeShared<FJsonValueObject>(Evidence); OutError.Diagnostics.Add(MoveTemp(Notice));
					}
				};
				const auto CompleteNoop = [&]() {
					auto Notice = OutError; Notice.Severity = Contract::ESeverity::Info;
					const auto Payload = MakeShared<FJsonObject>(); Payload->SetStringField(TEXT("status"), TEXT("unchanged"));
					Payload->SetArrayField(TEXT("warnings"), {MakeShared<FJsonValueObject>(Notice.ToPublicJson())});
					OutError.Reset();
					return Json::TryCanonicalize(Payload, OutResponse.PayloadJson, OutError);
				};
				const auto ImportCpu = UE::UAI::Timing::SampleThreadCpu();
				double TimedWorkFinished = 0, ResultFinished = 0;
				ON_SCOPE_EXIT {
					const double Finished = FPlatformTime::Seconds();
					UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=command_handler_lifetime total=%.6f result_and_cleanup=%.6f result_s=%.6f cleanup_s=%.6f completed_timed_work=%s"),
						Finished - ImportStarted, TimedWorkFinished > 0 ? Finished - TimedWorkFinished : 0.0,
						ResultFinished > 0 ? ResultFinished - TimedWorkFinished : 0.0, ResultFinished > 0 ? Finished - ResultFinished : 0.0,
						TimedWorkFinished > 0 ? TEXT("true") : TEXT("false"));
				};

				TSharedPtr<FJsonObject> Params;
				UE::UAI::Contract::FDiagnostic JsonError;
				if (!CommandContract::Json::TryParseObject(
						Request.CanonicalParamsJson, Params, JsonError)
					|| !Params.IsValid())
				{
					OutResponse = {};
					OutError = MoveTemp(JsonError);
					return false;
				}
				if (Params->HasField(TEXT("expected_revision"))
					|| Params->HasField(TEXT("authorization_id")))
				{
					OutResponse = {};
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("content_graph_public_machine_token_forbidden"), TEXT("Machine custody fields cannot be supplied through the public import command."), TEXT("input_decode"), FString(), Request.CommandId);
					TArray<TSharedPtr<FJsonValue>> SuppliedFields;
					for (const TCHAR* Field : {TEXT("expected_revision"), TEXT("authorization_id")})
					{
						if (Params->HasField(Field)) SuppliedFields.Add(MakeShared<FJsonValueString>(Field));
					}
					OutError.PropertyPath = TEXT("params");
					OutError.CandidateName = Request.CommandId;
					OutError.ActualValue = MakeShared<FJsonValueArray>(MoveTemp(SuppliedFields));
					return false;
				}

				FString Target;
				const TSharedPtr<FJsonObject>* Source = nullptr;
				if (!Params->TryGetStringField(TEXT("target"), Target)
					|| Target.IsEmpty()
					|| !Params->TryGetObjectField(TEXT("source"), Source)
					|| !Source || !Source->IsValid())
				{
					OutResponse = {};
					const bool bTargetInvalid = !Params->TryGetStringField(TEXT("target"), Target) || Target.IsEmpty();
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("uobject_dataasset_import_params_invalid"), bTargetInvalid ? TEXT("The import target must be a non-empty string.") : TEXT("The import source must be an object."), TEXT("input_decode"), FString(), Request.CommandId);
					OutError.PropertyPath = bTargetInvalid ? TEXT("target") : TEXT("source");
					OutError.ExpectedValue = MakeShared<FJsonValueString>(bTargetInvalid ? TEXT("non-empty string") : TEXT("object"));
					OutError.CandidateName = Request.CommandId;
					const auto Value = Params->TryGetField(OutError.PropertyPath);
					OutError.ActualValuePresent = Value.IsValid();
					const auto Actual = MakeShared<FJsonObject>();
					Actual->SetStringField(TEXT("type"), UeAgentJsonDiagnostics::JsonValueTypeToString(Value));
					if (Value.IsValid() && Value->Type == EJson::String) Actual->SetBoolField(TEXT("empty"), Value->AsString().IsEmpty());
					OutError.ActualValue = MakeShared<FJsonValueObject>(Actual);
					return false;
				}

				ContextResource::FUeAgentResourceTracker Resources =
					HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
				HjsonWorkspace::FSemanticWorkspaceReadScope ReadScope;
				HjsonWorkspace::FResolvedContentGraphImportSourcePtr ResolvedSource;
				FString ExpectedRevision;
				FString AuthorizationId;
				TArray<FString> ManagedSourcePaths;
				HjsonWorkspace::FWorkspaceError WorkspaceError;
				if (!HjsonWorkspace::TryRecoverContentGraphImportMachineContext(
					*Source,
					Target,
					Resources,
					ResolvedSource,
					ExpectedRevision,
						AuthorizationId,
						WorkspaceError,
						&ManagedSourcePaths))
				{
					OutResponse = {};
					OutError = WorkspaceError.GetDiagnostic();
					if (OutError.Code == TEXT("hjson_import_no_explicit_changes")) return CompleteNoop();
					return false;
				}

				Params->SetStringField(
					TEXT("expected_revision"), ExpectedRevision);
				Params->SetStringField(
					TEXT("authorization_id"), AuthorizationId);
				Params->SetObjectField(
					TEXT("source"),
					ResolvedSource->MakeNormalizedSourceObject());
				FUeAgentUObjectDataAssetCommandRequest InternalRequest = Request;
				InternalRequest.WorkspaceProfileId = ResolvedSource->GetWorkspaceProfileId();
				if (!CommandContract::Json::TryCanonicalize(
						Params, InternalRequest.CanonicalParamsJson, JsonError))
				{
					OutResponse = {};
					OutError = MoveTemp(JsonError);
					return false;
				}
				// Recovery already resolved and verified this directory. Reuse its
				// managed paths rather than parsing every authored unit a second time.
				HjsonWorkspace::FAuthoringModelPtr RefreshBaseline;
				HjsonWorkspace::FResolvedSemanticWorkspacePtr RefreshSource;
				FString PublishedSemanticFingerprint;
				bool bCommittedConsumer = false;
				FString CommittedTargetKind;
				TUniquePtr<HjsonWorkspace::FCommittedWorkspaceTransaction> CommonCommittedTransaction;
				bool bNativeAlreadyApplied = false;
				if (ResolvedSource->IsSemanticWorkspaceDirectory()) {
					HjsonWorkspace::FResolvedSemanticWorkspacePtr Workspace = HjsonWorkspace::GetAcceptedSemanticWorkspace(ResolvedSource->GetSourceUnit());
					if (!Workspace && !HjsonWorkspace::TryLoadHjsonImportSource(HjsonWorkspace::FWorkspaceSource::Directory(ResolvedSource->GetSourceUnit()),
						HjsonWorkspace::ESemanticWorkspaceMode::State, Resources, Workspace, WorkspaceError)) {
						OutError = WorkspaceError.GetDiagnostic(); return false;
					}
					const auto& SourceModel = Workspace->GetModel();
					RefreshSource = Workspace;
					PublishedSemanticFingerprint = Workspace->GetPublishedSemanticFingerprint();
					if (SourceModel->GetTargetUnitIds().Num() == 1) {
						const auto* RootUnit = SourceModel->FindUnit(SourceModel->GetTargetUnitIds()[0]);
						bCommittedConsumer = RootUnit && HjsonWorkspace::HasCommittedExportConsumer(RootUnit->GetSemanticTypeRef());
						if (RootUnit) CommittedTargetKind = RootUnit->GetSemanticTypeRef();
					}
					if (bCommittedConsumer && !HjsonWorkspace::TryFindAppliedCommittedSource(ResolvedSource->GetSourceUnit(),
						*Workspace, bNativeAlreadyApplied, WorkspaceError)) { OutError = WorkspaceError.GetDiagnostic(); return false; }
					if (bCommittedConsumer && !bNativeAlreadyApplied && CommittedTargetKind != TEXT("ue.level.asset"))
						CommonCommittedTransaction = MakeUnique<HjsonWorkspace::FCommittedWorkspaceTransaction>(
							CommittedTargetKind, Target, ResolvedSource->GetSourceUnit(), Workspace->GetWorkspaceFingerprint(), SourceModel, Workspace);
					if (CommonCommittedTransaction && !CommonCommittedTransaction->IsReady(WorkspaceError))
					{ OutError = WorkspaceError.GetDiagnostic(); return false; }
				}
				const double DomainStarted = FPlatformTime::Seconds();
				const auto DomainCpu = UE::UAI::Timing::SampleThreadCpu();
				if (bNativeAlreadyApplied) {
					OutResponse.PayloadJson = TEXT("{\"status\":\"already_applied\",\"retry_import\":false}");
					UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=native_apply_skipped reason=exact_committed_source_pending_publication"));
				}
				else if (!Inner->Execute(InternalRequest, OutResponse, OutError)) {
					if (OutError.Code == TEXT("hjson_import_no_explicit_changes")) return CompleteNoop();
					return false;
				}
				const auto DomainFinishedCpu = UE::UAI::Timing::SampleThreadCpu();
				const double DomainFinished = FPlatformTime::Seconds();
				if (OutResponse.PayloadJson.IsEmpty()) OutResponse.PayloadJson = TEXT("{\"status\":\"succeeded\",\"asset_applied\":true}");
				if (!ResolvedSource->IsSemanticWorkspaceDirectory()) {
					if (!RecoveryNotice.GetJournalIds().IsEmpty()) {
						TSharedPtr<FJsonObject> Payload;
						if (!Json::TryParseObject(OutResponse.PayloadJson, Payload, OutError)) return false;
						Payload->SetArrayField(TEXT("durable_journals"), CustodyJson());
						if (!Json::TryCanonicalize(Payload, OutResponse.PayloadJson, OutError)) return false;
					}
					OutResponse.PayloadJson = OperationWarnings.AppendToPayload(OutResponse.PayloadJson);
					return true;
				}

				TArray<UE::UAI::Contract::FDiagnostic> RefreshWarnings;
				double RefreshExportSeconds = 0.0;
				double RefreshExportCpuSeconds = 0.0;
				bool bRefreshExportCpuValid = true;
				FString RefreshActualMode(TEXT("committed"));
				TArray<FString> RefreshObservationScopes;
				int32 RefreshedCommitCount = 0;
				const double RefreshStarted = FPlatformTime::Seconds();
				const auto RefreshCpu = UE::UAI::Timing::SampleThreadCpu();
				const bool bRefreshed = HjsonWorkspace::TryRefreshImportedWorkspace(
					ResolvedSource->GetSourceUnit(), ManagedSourcePaths,
					[&](const FString& Destination, HjsonWorkspace::FPreparedWorkspaceExport& PreparedExport,
						HjsonWorkspace::FWorkspaceError& RefreshError)
					{
						HjsonWorkspace::FCommittedExportSelection Selection;
						if (bCommittedConsumer && !RefreshBaseline
							&& !HjsonWorkspace::TryBuildCommittedExportBaseline(*RefreshSource, Resources, RefreshBaseline, RefreshError)) return false;
						const bool bCommitted = bCommittedConsumer && HjsonWorkspace::TrySelectCommittedWorkspaceExport(
							ResolvedSource->GetSourceUnit(), Target, RefreshBaseline.ToSharedRef(), PublishedSemanticFingerprint, Resources, Selection, RefreshError);
						if (bCommitted) {
							const double Started = FPlatformTime::Seconds();
							const auto Cpu = UE::UAI::Timing::SampleThreadCpu();
							const bool bPrepared = HjsonWorkspace::TryPrepareCommittedWorkspacePlan(Selection, Destination,
								PreparedExport, Resources, RefreshError);
							const double Seconds = UE::UAI::Timing::ThreadCpuSeconds(Cpu, UE::UAI::Timing::SampleThreadCpu());
							bRefreshExportCpuValid &= Seconds >= 0;
							if (Seconds >= 0) RefreshExportCpuSeconds += Seconds;
							RefreshExportSeconds += FPlatformTime::Seconds() - Started;
							RefreshActualMode = TEXT("committed");
							RefreshObservationScopes = Selection.ObservationScopes;
							RefreshedCommitCount = Selection.AppliedRecords;
							return bPrepared;
						}
						if (bCommittedConsumer) {
							if (RefreshError.GetCode() == TEXT("workspace_committed_baseline_missing")) {
								auto Notice = RefreshError.GetDiagnostic();
								Notice.Code = TEXT("workspace_committed_observation_pending");
								Notice.Severity = Contract::ESeverity::Warning;
								Notice.RetryImport = false;
								RefreshError.SetFailure(MoveTemp(Notice));
							}
							return false;
						}
						// Native work already completed. Missing observation support is
						// not permission to mix unrelated current state into this commit.
						auto Notice = Contract::FDiagnostic::Failure(TEXT("workspace_committed_observation_pending"),
							TEXT("The native import completed, but this target has no committed observation consumer. Keep the original custody and synchronize explicitly with incremental/full export; do not replay the import."), TEXT("workspace_refresh"));
						Notice.Severity = Contract::ESeverity::Warning;
						Notice.AssetApplied = true; Notice.RetryImport = false;
						Notice.RecoveryAction = TEXT("synchronize_incremental_or_full_without_replaying_import");
						RefreshError.SetFailure(MoveTemp(Notice));
						return false;
					}, Resources, WorkspaceError, RefreshWarnings);
				const double RefreshFinished = FPlatformTime::Seconds();
				const auto RefreshFinishedCpu = UE::UAI::Timing::SampleThreadCpu();
				const double RefreshCpuSeconds = UE::UAI::Timing::ThreadCpuSeconds(RefreshCpu, RefreshFinishedCpu);
				TimedWorkFinished = RefreshFinished;
				UE_LOG(LogTemp, Display, TEXT("UAI content_graph import timing target=%s prepare=%.3fs domain_import=%.3fs refresh_export=%.3fs refresh_other=%.3fs refreshed=%s"),
					*Target, DomainStarted - ImportStarted, DomainFinished - DomainStarted, RefreshExportSeconds,
					RefreshFinished - RefreshStarted - RefreshExportSeconds, bRefreshed ? TEXT("true") : TEXT("false"));
				UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=command_thread_cpu prepare_s=%.6f domain_s=%.6f refresh_export_s=%.6f refresh_other_s=%.6f total_s=%.6f unavailable_value=-1"),
					UE::UAI::Timing::ThreadCpuSeconds(ImportCpu, DomainCpu), UE::UAI::Timing::ThreadCpuSeconds(DomainCpu, DomainFinishedCpu),
					bRefreshExportCpuValid ? RefreshExportCpuSeconds : -1.0,
					bRefreshExportCpuValid && RefreshCpuSeconds >= 0 ? RefreshCpuSeconds - RefreshExportCpuSeconds : -1.0,
					UE::UAI::Timing::ThreadCpuSeconds(ImportCpu, RefreshFinishedCpu));
				if (!bRefreshed)
				{
					if (WorkspaceError.GetCode() == TEXT("workspace_committed_observation_pending")) {
						TSharedPtr<FJsonObject> Payload;
						if (!Json::TryParseObject(OutResponse.PayloadJson, Payload, OutError)) return false;
						const auto Refresh = MakeShared<FJsonObject>();
						Refresh->SetStringField(TEXT("status"), TEXT("pending"));
						Refresh->SetBoolField(TEXT("retry_import"), false);
						Payload->SetObjectField(TEXT("workspace_refresh"), Refresh);
						if (!RecoveryNotice.GetJournalIds().IsEmpty()) Payload->SetArrayField(TEXT("durable_journals"), CustodyJson());
						OperationWarnings.Add(WorkspaceError.GetDiagnostic());
						if (!Json::TryCanonicalize(Payload, OutResponse.PayloadJson, OutError)) return false;
						OutResponse.PayloadJson = OperationWarnings.AppendToPayload(OutResponse.PayloadJson);
						return true;
					}
					OutError = HjsonWorkspace::MakeAppliedWorkspaceFailure(
							TEXT("content_graph_import_applied_workspace_refresh_failed"), WorkspaceError);
					for (const auto& Warning : RefreshWarnings) OutError.Append(Warning);
					return false;
				}
				TSharedPtr<FJsonObject> Payload;
				if (!CommandContract::Json::TryParseObject(OutResponse.PayloadJson, Payload, JsonError))
				{
					WorkspaceError.SetFailure(JsonError);
					OutError = HjsonWorkspace::MakeAppliedWorkspaceFailure(
							TEXT("content_graph_import_applied_result_invalid"), WorkspaceError);
					for (const auto& Warning : RefreshWarnings) OutError.Append(Warning);
					return false;
				}
				const TSharedRef<FJsonObject> Refresh = MakeShared<FJsonObject>();
				Refresh->SetStringField(TEXT("status"), TEXT("refreshed"));
				Refresh->SetStringField(TEXT("export_mode"), RefreshActualMode);
				if (RefreshActualMode == TEXT("committed")) {
					Refresh->SetNumberField(TEXT("consumed_commit_count"), RefreshedCommitCount);
					TArray<TSharedPtr<FJsonValue>> Scopes;
					for (const auto& Scope : RefreshObservationScopes) Scopes.Add(MakeShared<FJsonValueString>(Scope));
					Refresh->SetArrayField(TEXT("native_observation_scopes"), MoveTemp(Scopes));
				}
				if (!RefreshWarnings.IsEmpty()) {
					TArray<TSharedPtr<FJsonValue>> Warnings;
					for (const auto& Warning : RefreshWarnings)
						Warnings.Add(MakeShared<FJsonValueObject>(Warning.ToPublicJson()));
					Refresh->SetArrayField(TEXT("warnings"), MoveTemp(Warnings));
				}
				Payload->SetObjectField(TEXT("workspace_refresh"), Refresh);
				const auto Timing = MakeShared<FJsonObject>();
				Timing->SetStringField(TEXT("scope"), TEXT("server_directory_import"));
				Timing->SetNumberField(TEXT("source_prepare_seconds"), DomainStarted - ImportStarted);
				Timing->SetNumberField(TEXT("domain_import_seconds"), DomainFinished - DomainStarted);
				Timing->SetNumberField(TEXT("workspace_export_seconds"), RefreshExportSeconds);
				Timing->SetNumberField(TEXT("workspace_refresh_other_seconds"), RefreshFinished - RefreshStarted - RefreshExportSeconds);
				Timing->SetNumberField(TEXT("total_seconds"), RefreshFinished - ImportStarted);
				Payload->SetObjectField(TEXT("execution_timing"), Timing);
				if (!RecoveryNotice.GetJournalIds().IsEmpty()) Payload->SetArrayField(TEXT("durable_journals"), CustodyJson());
				if (!CommandContract::Json::TryCanonicalize(Payload, OutResponse.PayloadJson, JsonError))
				{
					WorkspaceError.SetFailure(JsonError);
					OutError = HjsonWorkspace::MakeAppliedWorkspaceFailure(
							TEXT("content_graph_import_applied_result_invalid"), WorkspaceError);
					for (const auto& Warning : RefreshWarnings) OutError.Append(Warning);
					return false;
				}
				ResultFinished = FPlatformTime::Seconds();
				OutResponse.PayloadJson = OperationWarnings.AppendToPayload(OutResponse.PayloadJson);
				return true;
			}

		private:
			bool ExecuteExport(const FUeAgentUObjectDataAssetCommandRequest& Request,
				FUeAgentUObjectDataAssetCommandResponse& OutResponse, FUeAgentUObjectDataAssetCommandError& OutError)
			{
				UeAgentAuthoringWorkspaceTransaction::FScopedDurableConsumerHistory HistoryScope;
				TSharedPtr<FJsonObject> Params;
				if (!Json::TryParseObject(Request.CanonicalParamsJson, Params, OutError)) return false;
				FString Mode(TEXT("committed")), Root, Target, Profile(TEXT("llm_authoring")), RequestedFormat;
				if (Params->HasField(TEXT("workspace_format")) && !Params->TryGetStringField(TEXT("workspace_format"), RequestedFormat)) {
					OutError = Contract::FDiagnostic::Failure(TEXT("workspace_format_invalid"), TEXT("workspace_format must be v2, v3 or v4."), TEXT("input_decode"));
					return false;
				}
				if ((Params->HasField(TEXT("export_mode")) && !Params->TryGetStringField(TEXT("export_mode"), Mode))
					|| (Mode != TEXT("committed") && Mode != TEXT("incremental") && Mode != TEXT("full"))) {
					OutError = Contract::FDiagnostic::Failure(TEXT("workspace_export_mode_invalid"),
						TEXT("export_mode must be committed, incremental or full."), TEXT("input_decode"));
					OutError.PropertyPath = TEXT("export_mode"); return false;
				}
				if (!Params->TryGetStringField(TEXT("workspace_path"), Root) || Root.IsEmpty() || FPaths::IsRelative(Root)
					|| !Params->TryGetStringField(TEXT("target"), Target) || Target.IsEmpty())
					return Inner->Execute(Request, OutResponse, OutError); // Preserve domain parameter diagnostics.
				if (Params->HasField(TEXT("profile_id")) && !Params->TryGetStringField(TEXT("profile_id"), Profile))
					return Inner->Execute(Request, OutResponse, OutError);
				const double PublicationSelectionStarted = FPlatformTime::Seconds();
				FPaths::NormalizeDirectoryName(Root);
				if (!ArtifactPublisher::TryRecoverWorkspacePublication(Root, OutError)) return false;
				TArray<FString> ExistingChildren;
				if (IFileManager::Get().DirectoryExists(*Root))
					IFileManager::Get().FindFiles(ExistingChildren, *(Root / TEXT("*")), true, true);
				const bool bInitialize = !IFileManager::Get().FileExists(*Root) && ExistingChildren.IsEmpty();
				HjsonWorkspace::FWorkspacePublication Publication;
				HjsonWorkspace::FWorkspaceError FormatError;
				if (!HjsonWorkspace::TrySelectWorkspacePublication(Root, RequestedFormat, bInitialize, Publication, FormatError)) {
					OutError = FormatError.GetDiagnostic(); return false;
				}
				FString ActualMode = Mode;
				const double PublicationSelectedAt = FPlatformTime::Seconds();
				double FullObservationSeconds = 0, FullSourceSeconds = 0, FullDomainSeconds = 0;
				FString ExportReason;
				int32 ConsumedCommitCount = 0;
				TSharedPtr<const HjsonWorkspace::FCommittedExportFence> CaptureFence;
				if ((Mode == TEXT("full") || bInitialize)
					&& !HjsonWorkspace::TryCaptureCommittedExportFence(CaptureFence, FormatError)) {
					OutError = FormatError.GetDiagnostic(); return false;
				}
				const double CaptureFenceFinished = FPlatformTime::Seconds();
				const double FullBranchStarted = CaptureFenceFinished;
				if (Mode == TEXT("full") || bInitialize) {
					FUeAgentUObjectDataAssetCommandRequest ExportRequest = Request;
					ExportRequest.WorkspaceExportPreparation = MakeShared<HjsonWorkspace::FPreparedWorkspaceExport>();
					ExportRequest.WorkspaceExportPreparation->Publication = Publication;
					ExportRequest.WorkspaceExportPreparation->bPrepareOnly = false;
					ExportRequest.WorkspaceExportPreparation->bInitializePublication = bInitialize;
					ExportRequest.WorkspaceExportPreparation->ActualExportMode = TEXT("full");
					ExportRequest.WorkspaceExportPreparation->CommittedCaptureFence = CaptureFence;
					ExportRequest.WorkspaceExportPreparation->CommittedTarget = Target;
					HjsonWorkspace::FSemanticWorkspaceReadScope FullReadScope;
					if (!HjsonWorkspace::BeginExplicitWorkspaceFileObservation(Root, FormatError)) {
						OutError = FormatError.GetDiagnostic(); return false;
					}
					const double SourcePreparationStarted = FPlatformTime::Seconds();
					FullObservationSeconds = SourcePreparationStarted - FullBranchStarted;
					if (!bInitialize) {
						auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
						HjsonWorkspace::FWorkspaceError SourceError;
						if (!HjsonWorkspace::TryPrepareIncrementalWorkspaceExport(Root, Target, Profile, Resources,
							ExportRequest.WorkspaceExportPreparation->ManagedSourcePaths,
							ExportRequest.WorkspaceExportPreparation->SourceCheckpoint, SourceError, true)) {
							OutError = SourceError.GetDiagnostic(); return false;
						}
						const FString AcceptedRoot = HjsonWorkspace::GetAcceptedSemanticWorkspaceRoot();
						if (!AcceptedRoot.Equals(Root, ESearchCase::IgnoreCase)) {
							HjsonWorkspace::FailWorkspaceDataflowHandoff(SourceError, TEXT("workspace.full_source"),
								TEXT("same-request accepted Root"), Root, AcceptedRoot.IsEmpty() ? TEXT("missing") : AcceptedRoot,
								TEXT("requested existing destination"), TEXT("TryPrepareIncrementalWorkspaceExport / formal ReadScope"));
							OutError = SourceError.GetDiagnostic(); return false;
						}
						ExportRequest.WorkspaceExportPreparation->AcceptedWorkspaceRoot = AcceptedRoot;
					}
					const double DomainStarted = FPlatformTime::Seconds();
					FullSourceSeconds = DomainStarted - SourcePreparationStarted;
					if (!Inner->Execute(ExportRequest, OutResponse, OutError)) return false;
					FullDomainSeconds = FPlatformTime::Seconds() - DomainStarted;
					ActualMode = TEXT("full");
				} else {
					if (Params->HasField(TEXT("source"))) {
						OutError = Contract::FDiagnostic::Failure(TEXT("workspace_incremental_intent_conflict"),
							TEXT("An external authoring intent requires a new workspace or explicit full export."), TEXT("workspace_refresh"));
						return false;
					}
					auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
					HjsonWorkspace::FSemanticWorkspaceReadScope ReadScope;
					HjsonWorkspace::FWorkspaceError Error;
					HjsonWorkspace::FHjsonOperationCheckpoint SourceCheckpoint;
					TArray<FString> ManagedPaths;
					if (!HjsonWorkspace::TryPrepareIncrementalWorkspaceExport(Root, Target, Profile,
						Resources, ManagedPaths, SourceCheckpoint, Error, false, Mode == TEXT("committed"))) {
						OutError = Error.GetDiagnostic(); return false;
					}
					HjsonWorkspace::FCommittedExportSelection Selection;
					HjsonWorkspace::FAuthoringModelPtr PublishedBaseline;
					bool bSelectedCommitted = false;
					if (Mode == TEXT("committed")) {
						HjsonWorkspace::FResolvedSemanticWorkspacePtr Workspace;
						if (!HjsonWorkspace::TryLoadHjsonImportSource(HjsonWorkspace::FWorkspaceSource::Directory(Root),
							HjsonWorkspace::ESemanticWorkspaceMode::State, Resources, Workspace, Error)) {
							OutError = Error.GetDiagnostic(); return false;
						}
						const auto& SourceModel = Workspace->GetModel();
						const auto* RootUnit = SourceModel->GetTargetUnitIds().Num() == 1
							? SourceModel->FindUnit(SourceModel->GetTargetUnitIds()[0]) : nullptr;
						if (!RootUnit || !HjsonWorkspace::HasCommittedExportConsumer(RootUnit->GetSemanticTypeRef())) {
							Error.SetSourceFailure(TEXT("workspace_committed_domain_unsupported"),
								TEXT("This domain has no committed-change evidence. The workspace remains pending; explicit incremental/full export is available without replaying native work."), Root);
						} else {
							if (!HjsonWorkspace::TryBuildCommittedExportBaseline(*Workspace, Resources, PublishedBaseline, Error)) {
								OutError = Error.GetDiagnostic(); return false;
							}
							bSelectedCommitted = HjsonWorkspace::TrySelectCommittedWorkspaceExport(Root, Target,
								PublishedBaseline.ToSharedRef(), Workspace->GetPublishedSemanticFingerprint(), Resources, Selection, Error);
						}
						if (!bSelectedCommitted) {
							if (Error.GetCode() != TEXT("workspace_committed_baseline_missing")
								&& Error.GetCode() != TEXT("workspace_committed_domain_unsupported")
								&& Error.GetCode() != TEXT("workspace_committed_observation_pending")) {
								OutError = Error.GetDiagnostic(); return false;
							}
							auto Notice = Error.GetDiagnostic();
							Notice.Severity = Contract::ESeverity::Warning;
							Notice.RetryImport = false;
							const auto Payload = MakeShared<FJsonObject>();
							Payload->SetStringField(TEXT("status"), TEXT("succeeded"));
							Payload->SetStringField(TEXT("export_mode"), TEXT("committed"));
							const auto Refresh = MakeShared<FJsonObject>();
							Refresh->SetStringField(TEXT("status"), TEXT("pending"));
							Refresh->SetBoolField(TEXT("retry_import"), false);
							Payload->SetObjectField(TEXT("workspace_refresh"), Refresh);
							Payload->SetArrayField(TEXT("warnings"), {MakeShared<FJsonValueObject>(Notice.ToJson())});
							OutResponse.bReadOnly = true;
							return Json::TryCanonicalize(Payload, OutResponse.PayloadJson, OutError);
						}
						Error = {};
					}
					TArray<Contract::FDiagnostic> Warnings;
					const bool bNoPendingCommits = bSelectedCommitted && Selection.AppliedRecords == 0
						&& !Selection.ActualChanges
						&& !Selection.ConsumerStateUpgradeDocuments.IsSet()
						&& Selection.SourcePublication.bDirectDataflow == Publication.bDirectDataflow
						&& Selection.SourcePublication.bGenerationPublication == Publication.bGenerationPublication;
					const bool bRefreshed = bNoPendingCommits
						? SourceCheckpoint(TEXT("committed_no_pending_changes"), Error)
						: HjsonWorkspace::TryRefreshWorkspace(Root, ManagedPaths,
						[&](const FString& Destination, HjsonWorkspace::FPreparedWorkspaceExport& Prepared,
							HjsonWorkspace::FWorkspaceError& ExportError) {
							if (bSelectedCommitted) return HjsonWorkspace::TryPrepareCommittedWorkspacePlan(
								Selection, Destination, Prepared, Resources, ExportError);
							if (!CaptureFence && !HjsonWorkspace::TryCaptureCommittedExportFence(CaptureFence, ExportError)) return false;
							Prepared.CommittedCaptureFence = CaptureFence;
							Prepared.CommittedTarget = Target;
							Prepared.bCommittedRequested = Mode == TEXT("committed");
							Prepared.ActualExportMode = Mode;
							Params->SetStringField(TEXT("workspace_path"), Destination);
							FUeAgentUObjectDataAssetCommandRequest ExportRequest = Request;
							ExportRequest.WorkspaceExportPreparation = MakeShared<HjsonWorkspace::FPreparedWorkspaceExport>(Prepared);
							FUeAgentUObjectDataAssetCommandResponse Response;
							Contract::FDiagnostic Diagnostic;
							if (!Json::TryCanonicalize(Params, ExportRequest.CanonicalParamsJson, Diagnostic)
								|| !Inner->Execute(ExportRequest, Response, Diagnostic)) {
								ExportError.SetFailure(MoveTemp(Diagnostic)); return false;
							}
							Prepared = *ExportRequest.WorkspaceExportPreparation;
							if (!Prepared.ActualExportMode.IsEmpty()) ActualMode = Prepared.ActualExportMode;
							OutResponse.Warnings.Append(Response.Warnings);
							return true;
						}, Resources, Error, Warnings, MoveTemp(SourceCheckpoint));
					if (!bRefreshed) { OutError = Error.GetDiagnostic(); return false; }
					if (bSelectedCommitted) ConsumedCommitCount = Selection.AppliedRecords;
					if (bNoPendingCommits) ExportReason = TEXT("no_pending_commits");
					if (!Json::TryCanonicalize(UeAgentExportWorkspaceWriter::MakePublicWorkspaceSuccessResult(Warnings),
						OutResponse.PayloadJson, OutError)) return false;
				}
				const double ResultStarted = FPlatformTime::Seconds();
				TSharedPtr<FJsonObject> Payload;
				if (!Json::TryParseObject(OutResponse.PayloadJson, Payload, OutError)) return false;
				Payload->SetStringField(TEXT("export_mode"), ActualMode);
				if (ActualMode == TEXT("committed")) Payload->SetNumberField(TEXT("consumed_commit_count"), ConsumedCommitCount);
				Payload->SetStringField(TEXT("workspace_format"), Publication.bDirectDataflow ? TEXT("v4") : Publication.bGenerationPublication ? TEXT("v3") : TEXT("v2"));
				if (!ExportReason.IsEmpty()) Payload->SetStringField(TEXT("export_reason"), ExportReason);
				if (Mode != TEXT("full") && bInitialize)
					Payload->SetStringField(TEXT("export_reason"), TEXT("baseline_initialized"));
				const bool bProjected = Json::TryCanonicalize(Payload, OutResponse.PayloadJson, OutError);
				if (Mode == TEXT("full") || bInitialize) UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=workspace_full_dispatch publication_selection_s=%.6f capture_fence_s=%.6f observation_setup_s=%.6f source_prepare_s=%.6f domain_execute_s=%.6f scope_and_branch_retire_s=%.6f result_projection_s=%.6f"),
					PublicationSelectedAt - PublicationSelectionStarted, CaptureFenceFinished - PublicationSelectedAt,
					FullObservationSeconds, FullSourceSeconds, FullDomainSeconds,
					ResultStarted - FullBranchStarted - FullObservationSeconds - FullSourceSeconds - FullDomainSeconds,
					FPlatformTime::Seconds() - ResultStarted);
				return bProjected;
			}

			TSharedRef<IUeAgentUObjectDataAssetCommandService> Inner;
		};

		class FContribution final : public IUeAgentActiveCommandContribution
		{
		public:
			FContribution() = default;
			explicit FContribution(
				TSharedRef<IUeAgentUObjectDataAssetCommandService> InService)
				: Service(MoveTemp(InService))
			{
			}

			virtual const FString& GetContributionId() const override
			{
				static const FString Id = TEXT("uobject-dataasset");
				return Id;
			}

			virtual void CollectDeclarations(
				TArray<FUeAgentCommandDeclaration>& OutDeclarations) const
				override
			{
				for (const FUeAgentUObjectDataAssetCommandRoute& Route
					: GetUObjectDataAssetCommandRoutes())
				{
					OutDeclarations.Add(Declaration(Route));
				}
			}

			virtual bool CreateHandlers(
				TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
				TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
				UE::UAI::Contract::FDiagnostic& OutError) const override
			{
				TSharedPtr<IUeAgentUObjectDataAssetCommandService> CommandService =
					Service;
				if (!CommandService.IsValid())
				{
					CommandService = MakeShared<FLazyProductionService>();
				}
				CommandService = MakeShared<FMachineContextCustodyService>(
					CommandService.ToSharedRef());
				TArray<TSharedRef<IUeAgentExactCommandHandler>> Handlers;
				if (!TryCreateUObjectDataAssetCommandHandlers(
						CommandService.ToSharedRef(), Handlers, OutError))
				{
					return false;
				}
				OutHandlers.Append(Handlers);
				return true;
			}

		private:
			TSharedPtr<IUeAgentUObjectDataAssetCommandService> Service;
		};

		bool TryCreateDomainCapability(
			TSharedPtr<const DomainCapability::
				FDomainCapabilityContribution>& OutContribution,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			TSharedPtr<IUeAgentActiveCommandContribution> CommandContribution;
			if (!TryCreateProductionUObjectDataAssetContribution(
				CommandContribution,
				OutError))
			{
				return false;
			}
			DomainCapability::FDomainCapabilityDeclaration Declaration =
				DomainCapability::
					MakeExplicitlyUnsupportedDomainCapabilityDeclaration(
						TEXT("uobject-dataasset"));
			Declaration.Command.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Command.Contributions.Add(
				CommandContribution.ToSharedRef());
			TSharedPtr<HjsonWorkspace::
				IImportInitializationDomainProvider> HjsonProvider;
			HjsonWorkspace::FWorkspaceError WorkspaceError;
			if (!TryCreateUObjectDataAssetDomainProvider(
				HjsonProvider,
				WorkspaceError))
			{
				OutError = WorkspaceError.GetDiagnostic();
				return false;
			}
			Declaration.Hjson.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Hjson.Providers.Add(HjsonProvider.ToSharedRef());
			Declaration.Hjson.Routes.Add({
				HjsonProvider->GetProviderId(),
				TEXT("content_graph.import_workspace"),
				TEXT("uai.uobject_dataasset.recursive/1"),
				TEXT("uobject_dataasset"),
				TEXT("/Script/UeAgentInterface.UeAgentRecursiveDataAsset"),
				HjsonWorkspace::EInitializationNodeRole::RootTarget,
				TEXT("uai.init.uobject_dataasset.root/1"),
				true,
			});
			Declaration.Initialization.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Initialization.ContractIds.Add(
				TEXT("uai.init.uobject_dataasset.root/1"));
			return DomainCapability::FDomainCapabilityContribution::TryCreate(
				MoveTemp(Declaration),
				OutContribution,
				OutError);
		}

		const DomainCapability::
			FDomainCapabilityContributionFactoryRegistration
			GUObjectDataAssetDomainCapabilityRegistration(
				TEXT("uobject-dataasset"),
				&TryCreateDomainCapability);
	}

	bool TryCreateUObjectDataAssetContribution(
		TSharedRef<IUeAgentUObjectDataAssetCommandService> Service,
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutContribution = MakeShared<FContribution>(Service);
		OutError.Reset();
		return true;
	}

	bool TryCreateProductionUObjectDataAssetContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutContribution.Reset();
		OutError.Reset();
		OutContribution = MakeShared<FContribution>();
		return true;
	}

	bool TryCreateUObjectDataAssetRegistration(
		TSharedRef<IUeAgentUObjectDataAssetCommandService> Service,
		TSharedPtr<const FUeAgentImmutableCommandRegistry>& OutRegistry,
		TSharedPtr<const FUeAgentExactCommandDispatch>& OutDispatch,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutRegistry.Reset();
		OutDispatch.Reset();
		OutError.Reset();
		TSharedPtr<IUeAgentActiveCommandContribution> Contribution;
		FUeAgentActiveCommandComposition Composition;
		if (!TryCreateUObjectDataAssetContribution(
				Service, Contribution, OutError)
			|| !Composition.TryAdd(Contribution.ToSharedRef(), OutError)
			|| !Composition.TryCompose(OutRegistry, OutDispatch, OutError))
		{
			OutRegistry.Reset();
			OutDispatch.Reset();
			return false;
		}
		return true;
	}
}

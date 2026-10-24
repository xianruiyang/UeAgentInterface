#include "Module/CommandRegistration/UObjectDataAsset/UeAgentUObjectDataAssetRegistration.h"

#include "Utilities/UeAgentJsonDiagnostics.h"

#include "Dom/JsonValue.h"

#include "CommandHandlers/UObjectDataAsset/UeAgentUObjectDataAssetCommandHandlers.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "Core/HjsonWorkspace/SourceLoader/UeAgentHjsonSourceLoader.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceResolver.h"
#include "Core/HjsonWorkspace/UeAgentWorkspaceRefresh.h"

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
				});
			}
			else if (Route.Kind == EUeAgentUObjectDataAssetRouteKind::Export)
			{
				Result.InputFields.Append({
					Field(TEXT("workspace_path"), TEXT("string"), true,
						TEXT("Absolute output workspace path.")),
					Field(TEXT("profile_id"), TEXT("string"), false,
						TEXT("Optional export profile identity.")),
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
					return Inner->Execute(Request, OutResponse, OutError);
				}

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
				HjsonWorkspace::FWorkspaceError WorkspaceError;
				if (!HjsonWorkspace::TryRecoverContentGraphImportMachineContext(
					*Source,
					Target,
					Resources,
					ResolvedSource,
					ExpectedRevision,
						AuthorizationId,
						WorkspaceError))
				{
					OutResponse = {};
					OutError = WorkspaceError.GetDiagnostic();
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
				if (!CommandContract::Json::TryCanonicalize(
						Params, InternalRequest.CanonicalParamsJson, JsonError))
				{
					OutResponse = {};
					OutError = MoveTemp(JsonError);
					return false;
				}
				TArray<FString> ManagedSourcePaths;
				if (ResolvedSource->IsSemanticWorkspaceDirectory()
					&& !HjsonWorkspace::TryCollectSemanticWorkspaceManagedPaths(
						ResolvedSource->GetSourceUnit(), Resources, ManagedSourcePaths, WorkspaceError))
				{
					OutError = WorkspaceError.GetDiagnostic();
					return false;
				}
				if (!Inner->Execute(InternalRequest, OutResponse, OutError)) return false;
				if (!ResolvedSource->IsSemanticWorkspaceDirectory()) return true;

				TArray<UE::UAI::Contract::FDiagnostic> RefreshWarnings;
				const bool bRefreshed = HjsonWorkspace::TryRefreshImportedWorkspace(
					ResolvedSource->GetSourceUnit(), ManagedSourcePaths,
					[&](const FString& Destination, HjsonWorkspace::FWorkspaceError& RefreshError)
					{
						const TSharedRef<FJsonObject> ExportParams = MakeShared<FJsonObject>();
						ExportParams->SetStringField(TEXT("target"), Target);
						ExportParams->SetStringField(TEXT("workspace_path"), Destination);
						ExportParams->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
						FUeAgentUObjectDataAssetCommandRequest ExportRequest = Request;
						ExportRequest.CommandId = TEXT("content_graph.export_workspace");
						FUeAgentUObjectDataAssetCommandResponse ExportResponse;
						FUeAgentUObjectDataAssetCommandError ExportError;
						if (!CommandContract::Json::TryCanonicalize(
							ExportParams, ExportRequest.CanonicalParamsJson, JsonError))
						{
							RefreshError.SetFailure(JsonError);
							return false;
						}
						if (!Inner->Execute(ExportRequest, ExportResponse, ExportError))
						{
							RefreshError.SetFailure(ExportError);
							return false;
						}
						RefreshWarnings.Append(ExportResponse.Warnings);
						return true;
					}, Resources, WorkspaceError, RefreshWarnings);
				if (!bRefreshed)
				{
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
				if (!RefreshWarnings.IsEmpty()) {
					TArray<TSharedPtr<FJsonValue>> Warnings;
					for (const auto& Warning : RefreshWarnings)
						Warnings.Add(MakeShared<FJsonValueObject>(Warning.ToPublicJson()));
					Refresh->SetArrayField(TEXT("warnings"), MoveTemp(Warnings));
				}
				Payload->SetObjectField(TEXT("workspace_refresh"), Refresh);
				if (!CommandContract::Json::TryCanonicalize(Payload, OutResponse.PayloadJson, JsonError))
				{
					WorkspaceError.SetFailure(JsonError);
					OutError = HjsonWorkspace::MakeAppliedWorkspaceFailure(
							TEXT("content_graph_import_applied_result_invalid"), WorkspaceError);
					for (const auto& Warning : RefreshWarnings) OutError.Append(Warning);
					return false;
				}
				return true;
			}

		private:
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

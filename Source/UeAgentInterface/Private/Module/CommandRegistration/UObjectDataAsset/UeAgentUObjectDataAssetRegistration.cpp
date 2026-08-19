#include "Module/CommandRegistration/UObjectDataAsset/UeAgentUObjectDataAssetRegistration.h"

#include "CommandHandlers/UObjectDataAsset/UeAgentUObjectDataAssetCommandHandlers.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"

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
			TEXT("uobject-dataasset-recursive-initialization-owner");

		FUeAgentCommandFieldDeclaration Field(
			const TCHAR* Name,
			const TCHAR* WireType,
			const bool bRequired,
			const TCHAR* Description)
		{
			return {Name, WireType, bRequired, false, Description};
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
			Result.PythonNamespace = TEXT("uobject_dataasset");
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
						TEXT("Exact discriminated source: {kind:inline,document} or {kind:single_file|directory,path}.")),
					Field(TEXT("expected_revision"), TEXT("string"), true,
						TEXT("Expected persisted target revision; create uses absent.")),
					Field(TEXT("authorization_id"), TEXT("string"), true,
						TEXT("Explicit impact authorization for the exact target.")),
					Field(TEXT("limits"), TEXT("object"), false,
						TEXT("Optional bounded node and work-unit budgets.")),
				});
			}
			else if (Route.Kind == EUeAgentUObjectDataAssetRouteKind::Export)
			{
				Result.InputFields.Append({
					Field(TEXT("workspace_path"), TEXT("string"), true,
						TEXT("Absolute output workspace path.")),
					Field(TEXT("profile_id"), TEXT("string"), false,
						TEXT("Optional export profile identity.")),
				});
			}
			else
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
			Result.ResultFields = {
				Field(TEXT("command_id"), TEXT("string"), true,
					TEXT("The unchanged exact public command identity.")),
			};
			if (Route.Kind == EUeAgentUObjectDataAssetRouteKind::Import)
			{
				Result.ResultFields.Append({
					Field(TEXT("terminal_status"), TEXT("string"), true,
						TEXT("Succeeded only after lifecycle persistence and independent verification.")),
					Field(TEXT("independent_readback"), TEXT("object"), true,
						TEXT("Unload/reload readback of the persisted recursive object graph.")),
				});
			}
			else if (Route.Kind == EUeAgentUObjectDataAssetRouteKind::Export)
			{
				Result.ResultFields.Append({
					Field(TEXT("status"), TEXT("string"), true,
						TEXT("Published only after semantic verification.")),
					Field(TEXT("semantic_verification"), TEXT("object"), true,
						TEXT("Independent staged-workspace semantic verification.")),
				});
			}
			else
			{
				Result.ResultFields.Append({
					Field(TEXT("read_only"), TEXT("boolean"), true,
						TEXT("Always true for content graph queries.")),
					Field(TEXT("query_result"), TEXT("object"), true,
						TEXT("Readback from the real persisted UObject/DataAsset graph.")),
				});
			}
			Result.Errors = {
				Error(TEXT("uobject_dataasset_authority_required"),
					TEXT("The route-specific public authority is missing.")),
				Error(TEXT("uobject_dataasset_game_thread_required"),
					TEXT("The public ingress did not enter the UE game thread.")),
				Error(TEXT("content_graph_read_target_unresolved"),
					TEXT("The exact persisted target could not be resolved.")),
				Error(TEXT("resource_budget_exceeded"),
					TEXT("The validated recursive plan exceeds a declared resource bound.")),
				Error(TEXT("uobject_dataasset_type_unknown"),
					TEXT("An exact UObject type identity is unsupported.")),
				Error(TEXT("content_graph_workspace_manifest_missing"),
					TEXT("A directory source is missing .uai/workspace.hjson.")),
				Error(TEXT("content_graph_workspace_digest_mismatch"),
					TEXT("A declared directory document does not match its SHA-256 digest.")),
				Error(TEXT("workspace_path_invalid"),
					TEXT("A source path is relative, escapes the workspace root, or is otherwise invalid.")),
			};
			Result.Purpose.Summary = Route.Kind ==
					EUeAgentUObjectDataAssetRouteKind::Import
				? TEXT("Create a recursive UObject/DataAsset graph through unified HJSON initialization.")
				: Route.Kind == EUeAgentUObjectDataAssetRouteKind::Export
					? TEXT("Publish a semantically verified HJSON workspace for a recursive UObject/DataAsset graph.")
					: TEXT("Read one exact facet of a persisted recursive UObject/DataAsset graph.");
			Result.Purpose.Target =
				TEXT("One exact recursive UObject/DataAsset package.object identity.");
			Result.Purpose.Preconditions = {
				TEXT("Use the public exact command identity and contract hash unchanged."),
				TEXT("The Editor service dispatches the request on the UE game thread."),
			};
			Result.Purpose.ParameterGuidance =
				TEXT("Preserve exact type, target, field and candidate identities; do not alias them in the SDK or caller.");
			Result.Purpose.ResultGuidance =
				TEXT("Treat persistence, independent readback and semantic verification fields as part of the result contract.");
			Result.Purpose.FailureGuidance =
				TEXT("Typed validation and lifecycle failures never fall back to a legacy provider or partial success.");
			Result.Purpose.SideEffectGuidance = Route.Kind ==
					EUeAgentUObjectDataAssetRouteKind::Import
				? TEXT("Creates and saves one authorized asset; confirmed rollback removes partial work.")
				: Route.Kind == EUeAgentUObjectDataAssetRouteKind::Export
					? TEXT("Writes only the requested verified external workspace.")
					: TEXT("Read-only with respect to UE and external state.");
			Result.Purpose.MinimumExample = Route.Kind ==
					EUeAgentUObjectDataAssetRouteKind::Import
				? TEXT("content_graph_import_workspace(client, target, {'kind':'inline','document':document}, 'absent', authorization_id)")
				: Route.Kind == EUeAgentUObjectDataAssetRouteKind::Export
					? TEXT("content_graph_export_workspace(client, target, workspace_path)")
					: Route.PythonFunction + TEXT("(client, target)");
			Result.Purpose.RelatedFlows = {
				TEXT("query.command.search"),
				TEXT("query.command.detail"),
				TEXT("content_graph.import_workspace"),
				TEXT("content_graph.export_workspace"),
			};
			Result.Purpose.Keywords = {
				TEXT("uobject"), TEXT("dataasset"), TEXT("recursive"),
				TEXT("hjson"), TEXT("content graph"),
			};
			Result.Purpose.Synonyms = {
				TEXT("recursive data asset"),
				TEXT("initialize nested UObject graph"),
			};
			return Result;
		}

		class FContribution final : public IUeAgentActiveCommandContribution
		{
		public:
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
				FString& OutError) const override
			{
				TArray<TSharedRef<IUeAgentExactCommandHandler>> Handlers;
				if (!TryCreateUObjectDataAssetCommandHandlers(
						Service, Handlers, OutError))
				{
					return false;
				}
				OutHandlers.Append(Handlers);
				return true;
			}

		private:
			TSharedRef<IUeAgentUObjectDataAssetCommandService> Service;
		};

		const FUeAgentActiveCommandContributionFactoryRegistration
			GUObjectDataAssetContributionRegistration(
				TEXT("uobject-dataasset"),
				&TryCreateProductionUObjectDataAssetContribution);

		bool TryCreateDomainCapability(
			TSharedPtr<const DomainCapability::
				FDomainCapabilityContribution>& OutContribution,
			FString& OutError)
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
				OutError = WorkspaceError.GetCode();
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
			Declaration.Migration.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Migration.OwnerId =
				TEXT("T13-UOBJECT-RECURSIVE-INIT");
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
		FString& OutError)
	{
		OutContribution = MakeShared<FContribution>(Service);
		OutError.Reset();
		return true;
	}

	bool TryCreateProductionUObjectDataAssetContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError)
	{
		OutContribution.Reset();
		TSharedPtr<IUeAgentUObjectDataAssetCommandService> Service;
		if (!TryCreateProductionUObjectDataAssetCommandService(
				Service, OutError))
		{
			return false;
		}
		return TryCreateUObjectDataAssetContribution(
			Service.ToSharedRef(), OutContribution, OutError);
	}

	bool TryCreateUObjectDataAssetRegistration(
		TSharedRef<IUeAgentUObjectDataAssetCommandService> Service,
		TSharedPtr<const FUeAgentImmutableCommandRegistry>& OutRegistry,
		TSharedPtr<const FUeAgentExactCommandDispatch>& OutDispatch,
		FString& OutError)
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

#include "Module/CommandRegistration/UmgWidget/UeAgentUmgWidgetRegistration.h"

#include "CommandHandlers/UmgWidget/UeAgentUmgWidgetCommandHandlers.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "OperationOwners/UmgWidget/UeAgentUmgWidgetDomainService.h"

namespace UE::UAI::Module::CommandRegistration::UmgWidget
{
	using namespace Active;
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandHandlers::UmgWidget;
	using namespace CommandRegistry;
	using namespace OperationOwners::UmgWidget;

	namespace
	{
		const FString DomainId = TEXT("umg-command-migration");
		const FString ProviderRegistrationId = TEXT("umg-widget-domain-provider");
		const FString CandidateCommandId = TEXT("umg.widget.candidates");
		const FString CandidateOwner = TEXT("umg-widget-domain-owner");

		FUeAgentCommandFieldDeclaration Field(
			const TCHAR* Name,
			const TCHAR* WireType,
			const bool bRequired,
			const TCHAR* Description)
		{
			return {Name, WireType, bRequired, false, Description};
		}

		FUeAgentCommandDeclaration CandidateDeclaration()
		{
			FUeAgentCommandDeclaration Result;
			Result.CommandId = CandidateCommandId;
			Result.PythonNamespace = TEXT("umg");
			Result.PythonFunction = TEXT("widget_candidates");
			Result.ContractVersion = 1;
			Result.PublicOwner = CandidateOwner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = EUeAgentCommandSideEffect::None;
			Result.StateLayers = {TEXT("asset_persisted_state")};
			Result.InputFields = {
				Field(TEXT("target"), TEXT("string"), true,
					TEXT("Exact persisted WidgetBlueprint package.object identity.")),
				Field(TEXT("query"), TEXT("string"), false,
					TEXT("Optional bounded text filter over the partial Widget catalog.")),
				Field(TEXT("id"), TEXT("string"), false,
					TEXT("Optional exact compact candidate id returned by this target.")),
				Field(TEXT("limit"), TEXT("integer"), false,
					TEXT("Optional page size from 1 through 64; defaults to 64.")),
				Field(TEXT("cursor"), TEXT("integer"), false,
					TEXT("Optional non-negative result offset; defaults to zero.")),
			};
			Result.ResultFields = {
				Field(TEXT("command_id"), TEXT("string"), true,
					TEXT("The unchanged exact public command identity.")),
				Field(TEXT("read_only"), TEXT("boolean"), true,
					TEXT("Always true.")),
				Field(TEXT("target"), TEXT("string"), true,
					TEXT("The resolved exact WidgetBlueprint identity.")),
				Field(TEXT("target_revision"), TEXT("string"), true,
					TEXT("The current persisted target revision.")),
				Field(TEXT("candidates"), TEXT("array"), true,
					TEXT("Compact target-bound Widget class/template candidates.")),
				Field(TEXT("total_matches"), TEXT("integer"), true,
					TEXT("Total candidates matching this query.")),
				Field(TEXT("next_cursor"), TEXT("integer"), true,
					TEXT("Next result offset, or -1 when complete.")),
				Field(TEXT("complete"), TEXT("boolean"), true,
					TEXT("True only on the final pagination page.")),
				Field(TEXT("has_more"), TEXT("boolean"), true,
					TEXT("True only while another page remains.")),
				Field(TEXT("coverage_scope"), TEXT("string"), true,
					TEXT("The exact public partial catalog covered by this query.")),
				Field(TEXT("coverage_complete"), TEXT("boolean"), true,
					TEXT("False while the private Designer Palette catalog is unavailable headless.")),
				Field(TEXT("creation_coverage_complete"), TEXT("boolean"), true,
					TEXT("False while asset-backed and special templates remain search-only.")),
			};
			Result.Errors = {
				{TEXT("umg_widget_candidate_query_authority_required"),
					TEXT("The uai:query authority is missing.")},
				{TEXT("umg_widget_candidate_target_unresolved"),
					TEXT("The exact WidgetBlueprint target could not be loaded.")},
				{TEXT("umg_widget_candidate_page_invalid"),
					TEXT("The requested page is outside the bounded contract.")},
				{TEXT("umg_widget_candidate_not_found"),
					TEXT("The name/id selector no longer resolves in this target context.")},
				{TEXT("umg_widget_candidate_ambiguous"),
					TEXT("The name resolves more than one candidate; use exact id.")},
				{TEXT("umg_widget_candidate_not_writable"),
					TEXT("The candidate is visible but outside the current native class-template write scope.")},
			};
			Result.Purpose.Summary = TEXT(
				"Search target-bound UMG class/template candidates and expose exact compact ids with truthful partial Designer Palette coverage.");
			Result.Purpose.Target = TEXT("One exact persisted WidgetBlueprint.");
			Result.Purpose.Preconditions = {
				TEXT("Copy the target unchanged from public content search or readback."),
				TEXT("Run in the Editor on the UE game thread with uai:query."),
			};
			Result.Purpose.ParameterGuidance = TEXT(
				"Put only candidate.name or candidate.id in the widget HJSON item; keep the stable widget key, parent, slot, index and properties outside candidate.");
			Result.Purpose.ResultGuidance = TEXT(
				"Continue pagination while has_more is true, inspect supported/write_support, and do not treat the partial class/asset approximation as the complete Designer Palette.");
			Result.Purpose.FailureGuidance = TEXT(
				"Unresolved targets, missing or ambiguous selectors, unsupported templates and invalid pages fail before WidgetTree mutation.");
			Result.Purpose.SideEffectGuidance = TEXT(
				"Read-only; it creates no widget, package, file, window or external state.");
			Result.Purpose.MinimumExample = TEXT(
				"client.umg.widget_candidates(target, query='Button')");
			Result.Purpose.RelatedFlows = {
				TEXT("query.command.search"),
				TEXT("query.command.detail"),
				TEXT("content_graph.summary"),
				TEXT("content_graph.import_workspace"),
			};
			Result.Purpose.Keywords = {
				TEXT("umg"), TEXT("widget"), TEXT("palette"),
				TEXT("candidate"), TEXT("hjson")};
			Result.Purpose.Synonyms = {
				TEXT("find widget"), TEXT("add widget"),
				TEXT("designer palette")};
			return Result;
		}

		class FUmgWidgetCandidateContribution final
			: public IUeAgentActiveCommandContribution
		{
		public:
			explicit FUmgWidgetCandidateContribution(
				TSharedRef<IUeAgentUmgWidgetCandidateCommandService> InService)
				: Service(MoveTemp(InService))
			{
			}

			virtual const FString& GetContributionId() const override
			{
				static const FString Id = TEXT("umg-widget-domain-candidates");
				return Id;
			}
			virtual void CollectDeclarations(
				TArray<FUeAgentCommandDeclaration>& OutDeclarations) const override
			{
				OutDeclarations.Add(CandidateDeclaration());
			}
			virtual bool CreateHandlers(
				TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
				TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
				FString& OutError) const override
			{
				(void)Registry;
				return TryCreateUmgWidgetCandidateCommandHandlers(
					Service, OutHandlers, OutError);
			}

		private:
			TSharedRef<IUeAgentUmgWidgetCandidateCommandService> Service;
		};

		bool TryCreateDomainCapability(
			TSharedPtr<const DomainCapability::
				FDomainCapabilityContribution>& OutContribution,
			FString& OutError)
		{
			TSharedPtr<HjsonWorkspace::IImportInitializationDomainProvider>
				Provider;
			HjsonWorkspace::FWorkspaceError WorkspaceError;
			if (!OperationOwners::UmgWidget::TryCreateUmgWidgetDomainProvider(
					Provider, WorkspaceError)
				|| !Provider.IsValid())
			{
				OutError = WorkspaceError.GetCode();
				return false;
			}
			DomainCapability::FDomainCapabilityDeclaration Declaration =
				DomainCapability::
					MakeExplicitlyUnsupportedDomainCapabilityDeclaration(DomainId);
			Declaration.Hjson.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Hjson.Providers.Add(Provider.ToSharedRef());
			Declaration.Hjson.Routes = {{
				ProviderRegistrationId,
				TEXT("content_graph.import_workspace"),
				TEXT("uai.umg.edit/1"),
				TEXT("ue.widget_blueprint.asset"),
				TEXT("/Script/UMGEditor.WidgetBlueprint"),
				HjsonWorkspace::EInitializationNodeRole::RootTarget,
				TEXT("uai.init.umg.widget_blueprint/1"),
				true,
			}};
			Declaration.Initialization.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Initialization.ContractIds = {
				TEXT("uai.init.umg.widget_blueprint/1"),
				TEXT("uai.init.umg.widget/1"),
				TEXT("uai.init.umg.slot/1"),
				TEXT("uai.init.umg.binding/1"),
				TEXT("uai.init.umg.animation/1"),
			};
			Declaration.Migration.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Migration.OwnerId = TEXT("T15-UMG-WIDGET");
			return DomainCapability::FDomainCapabilityContribution::TryCreate(
				MoveTemp(Declaration), OutContribution, OutError);
		}

		const DomainCapability::
			FDomainCapabilityContributionFactoryRegistration
			DomainCapabilityRegistration(*DomainId, &TryCreateDomainCapability);

		const FUeAgentActiveCommandContributionFactoryRegistration
			CandidateRegistration(
				TEXT("umg-widget-domain-candidates"),
				&TryCreateProductionUmgWidgetCandidateContribution);
	}

	const FString& GetProviderRegistrationId()
	{
		return ProviderRegistrationId;
	}

	bool TryCreateProductionUmgWidgetCandidateContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError)
	{
		TSharedPtr<IUeAgentUmgWidgetCandidateCommandService> Service;
		if (!TryCreateProductionUmgWidgetCandidateCommandService(
				Service, OutError)
			|| !Service.IsValid())
		{
			OutContribution.Reset();
			return false;
		}
		OutContribution = MakeShared<FUmgWidgetCandidateContribution>(
			Service.ToSharedRef());
		OutError.Reset();
		return true;
	}
}

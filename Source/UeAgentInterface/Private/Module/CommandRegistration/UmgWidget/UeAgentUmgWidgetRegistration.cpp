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
					TEXT("Persisted WidgetBlueprint name from content search, or its exact package.object path.")),
				Field(TEXT("query"), TEXT("string"), false,
					TEXT("Optional text filter over the partial Widget catalog.")),
				Field(TEXT("describe"), TEXT("object"), false,
					TEXT("Optional selected native widget {name, select?, fields?}; returns editable defaults in workspace property shape, excluding slot and structural fields.")),
			};
			Result.ResultFields = { Field(TEXT("names"), TEXT("array"), true,
				TEXT("Distinct readable Widget template names.")),
				Field(TEXT("scope"), TEXT("object"), true, TEXT("Public target used for discovery; response metadata, not a selector.")),
				Field(TEXT("description"), TEXT("object"), false, TEXT("Selected native widget defaults and referenced types; only for describe.")) };
			Result.Errors = {
				{TEXT("umg_widget_candidate_query_authority_required"),
					TEXT("The uai:query authority is missing.")},
				{TEXT("umg_widget_candidate_target_unresolved"),
					TEXT("The exact WidgetBlueprint target could not be loaded.")},
				{TEXT("umg_widget_candidate_not_found"),
					TEXT("The readable name no longer resolves in this target context.")},
				{TEXT("umg_widget_candidate_ambiguous"),
					TEXT("The readable name resolves more than one candidate; refine the semantic context.")},
				{TEXT("umg_widget_candidate_not_writable"),
					TEXT("The candidate is visible but outside the current native class-template write scope.")},
			};
			Result.Purpose.Summary = TEXT(
				"Search target-bound UMG class/template candidates and expose readable names with truthful partial Designer Palette coverage.");
			Result.Purpose.Target = TEXT("One exact persisted WidgetBlueprint.");
			Result.Purpose.Preconditions = {
				TEXT("Copy the target unchanged from public content search or readback."),
				TEXT("Run in the Editor on the UE game thread with uai:query."),
			};
			Result.Purpose.ParameterGuidance = TEXT(
				"Use a returned readable widget name; keep the stable widget key, parent, slot, index and properties outside candidate.");
			Result.Purpose.ResultGuidance = TEXT(
				"Search returns names and one scope; re-query needed names when the target changes. describe returns editable defaults. scope is response metadata, not a selector; ambiguous names require the returned semantic selection.");
			Result.Purpose.FailureGuidance = TEXT(
				"Unresolved targets, missing or ambiguous selectors, unsupported templates and invalid pages fail before WidgetTree mutation.");
			Result.Purpose.SideEffectGuidance = TEXT(
				"Read-only; it creates no widget, package, file, window or external state.");
			Result.Purpose.MinimumExample = TEXT(
				"client.umg.widget_candidates(target, query='Button')");
			Result.Purpose.RelatedFlows = {
				TEXT("query.command.search"),
				TEXT("query.command.usage"),
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
				UE::UAI::Contract::FDiagnostic& OutError) const override
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
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			TSharedPtr<HjsonWorkspace::IImportInitializationDomainProvider>
				Provider;
			HjsonWorkspace::FWorkspaceError WorkspaceError;
			if (!OperationOwners::UmgWidget::TryCreateUmgWidgetDomainProvider(
					Provider, WorkspaceError)
				|| !Provider.IsValid())
			{
				OutError = WorkspaceError.GetDiagnostic();
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
		UE::UAI::Contract::FDiagnostic& OutError)
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

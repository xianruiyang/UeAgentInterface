#include "Module/CommandRegistration/AnimationBlueprint/UeAgentAnimationBlueprintRegistration.h"

#include "CommandHandlers/AnimationBlueprint/UeAgentAnimationBlueprintDomainCommandHandlers.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "OperationOwners/AnimationBlueprint/UeAgentAnimationBlueprintDomainService.h"

namespace UE::UAI::Module::CommandRegistration::AnimationBlueprint
{
	using namespace Active;
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandHandlers::AnimationBlueprint;
	using namespace CommandRegistry;
	using namespace OperationOwners::AnimationBlueprint;

	namespace
	{
		const FString DomainId = TEXT("animation-blueprint-command-migration");
		const FString ProviderRegistrationId =
			TEXT("animation-blueprint-domain-provider");
		const FString CandidateCommandId =
			TEXT("animation.state_machine.candidates");
		const FString CandidateOwner =
			TEXT("animation-blueprint-domain-owner");

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
			Result.PythonNamespace = TEXT("animation");
			Result.PythonFunction = TEXT("state_machine_candidates");
			Result.ContractVersion = 1;
			Result.PublicOwner = CandidateOwner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = EUeAgentCommandSideEffect::None;
			Result.StateLayers = {TEXT("editor_transient_state")};
			Result.InputFields = {
				Field(TEXT("target"), TEXT("string"), true,
					TEXT("Exact future AnimBlueprint package.object identity; the target must not already exist.")),
				Field(TEXT("surface"), TEXT("string"), true,
					TEXT("Exact native creation surface: animation_graph or state_machine_graph.")),
				Field(TEXT("class_settings"), TEXT("object"), false,
					TEXT("Optional T16 class settings used by the same UAnimBlueprintFactory scratch context as creation.")),
				Field(TEXT("query"), TEXT("string"), false,
					TEXT("Optional bounded text filter over native menu actions.")),
				Field(TEXT("id"), TEXT("string"), false,
					TEXT("Optional exact compact candidate id returned by this context.")),
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
					TEXT("The unchanged future AnimBlueprint identity.")),
				Field(TEXT("target_revision"), TEXT("string"), true,
					TEXT("Always missing for this create-only T16 context.")),
				Field(TEXT("surface"), TEXT("string"), true,
					TEXT("The exact native surface used for discovery.")),
				Field(TEXT("candidates"), TEXT("array"), true,
					TEXT("Compact candidates projected from the native surface owner.")),
				Field(TEXT("total_matches"), TEXT("integer"), true,
					TEXT("Total candidates matching this query.")),
				Field(TEXT("next_cursor"), TEXT("integer"), true,
					TEXT("Next result offset, or -1 when complete.")),
				Field(TEXT("complete"), TEXT("boolean"), true,
					TEXT("True only on the final pagination page.")),
				Field(TEXT("has_more"), TEXT("boolean"), true,
					TEXT("True only while another page remains.")),
				Field(TEXT("coverage_scope"), TEXT("string"), true,
					TEXT("The exact native action catalog covered by this surface.")),
				Field(TEXT("coverage_complete"), TEXT("boolean"), true,
					TEXT("Whether search covers the complete claimed native surface.")),
				Field(TEXT("creation_coverage_complete"), TEXT("boolean"), true,
					TEXT("Whether every returned native action is writable by T16 HJSON.")),
			};
			Result.Errors = {
				{TEXT("animation_blueprint_candidate_query_authority_required"),
					TEXT("The uai:query authority is missing.")},
				{TEXT("animation_blueprint_candidate_target_required"),
					TEXT("The future exact AnimBlueprint target is missing.")},
				{TEXT("animation_blueprint_candidate_target_exists"),
					TEXT("T16 is create-only and the requested target already exists.")},
				{TEXT("animation_blueprint_candidate_surface_invalid"),
					TEXT("The surface is not animation_graph or state_machine_graph.")},
				{TEXT("animation_blueprint_candidate_page_invalid"),
					TEXT("The requested page is outside the bounded contract.")},
				{TEXT("animation_blueprint_candidate_not_found"),
					TEXT("The name/id selector no longer resolves in the recreated context.")},
				{TEXT("animation_blueprint_candidate_ambiguous"),
					TEXT("The name resolves more than one action; use exact id.")},
				{TEXT("animation_blueprint_candidate_not_writable"),
					TEXT("The native action is searchable but outside the current HJSON write scope.")},
			};
			Result.Purpose.Summary = TEXT(
				"Search the exact native AnimGraph or StateMachine creation surface in a same-factory transient T16 context, then expose stable compact ids for HJSON creation.");
			Result.Purpose.Target = TEXT(
				"One future create-only AnimBlueprint target and one exact native surface.");
			Result.Purpose.Preconditions = {
				TEXT("Use the exact future target that content_graph.import_workspace will create."),
				TEXT("Use the same class_settings in query and HJSON creation."),
				TEXT("Run in the Editor on the UE game thread with uai:query."),
			};
			Result.Purpose.ParameterGuidance = TEXT(
				"Put only candidate.name or candidate.id in the state_machine or state HJSON item; keep instance name, graph, position and relations outside candidate. If both selectors are present they must resolve the same native action.");
			Result.Purpose.ResultGuidance = TEXT(
				"Use animation_graph for the parent StateMachine node and state_machine_graph for child State actions; inspect supported/write_support, and treat transitions as graph relations rather than menu candidates.");
			Result.Purpose.FailureGuidance = TEXT(
				"Changed factory context, stale ids, ambiguity, existing targets, unsupported native actions and invalid pages fail before the first persistent write.");
			Result.Purpose.SideEffectGuidance = TEXT(
				"Read-only; scratch assets and graphs are transient and no package, file, window or external state is created.");
			Result.Purpose.MinimumExample = TEXT(
				"client.animation.state_machine_candidates(target, surface='animation_graph', query='State Machine')");
			Result.Purpose.RelatedFlows = {
				TEXT("query.command.search"),
				TEXT("query.command.detail"),
				TEXT("content_graph.import_workspace"),
			};
			Result.Purpose.Keywords = {
				TEXT("animation blueprint"), TEXT("state machine"),
				TEXT("state"), TEXT("candidate"), TEXT("hjson")};
			Result.Purpose.Synonyms = {
				TEXT("find state machine"), TEXT("add state"),
				TEXT("animation graph menu")};
			return Result;
		}

		class FAnimationBlueprintCandidateContribution final
			: public IUeAgentActiveCommandContribution
		{
		public:
			explicit FAnimationBlueprintCandidateContribution(
				TSharedRef<IUeAgentAnimationBlueprintCandidateCommandService>
					InService)
				: Service(MoveTemp(InService))
			{
			}

			virtual const FString& GetContributionId() const override
			{
				static const FString Id =
					TEXT("animation-blueprint-domain-candidates");
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
				return TryCreateAnimationBlueprintCandidateCommandHandlers(
					Service, OutHandlers, OutError);
			}

		private:
			TSharedRef<IUeAgentAnimationBlueprintCandidateCommandService> Service;
		};

		bool TryCreateDomainCapability(
			TSharedPtr<const DomainCapability::
				FDomainCapabilityContribution>& OutContribution,
			FString& OutError)
		{
			TSharedPtr<HjsonWorkspace::IImportInitializationDomainProvider>
				Provider;
			HjsonWorkspace::FWorkspaceError WorkspaceError;
			if (!OperationOwners::AnimationBlueprint::
					TryCreateAnimationBlueprintDomainProvider(
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
				TEXT("uai.animation_blueprint.edit/1"),
				TEXT("ue.anim_blueprint.asset"),
				TEXT("/Script/Engine.AnimBlueprint"),
				HjsonWorkspace::EInitializationNodeRole::RootTarget,
				TEXT("uai.init.animation_blueprint.asset/1"),
				true,
			}};
			Declaration.Initialization.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Initialization.ContractIds = {
				TEXT("uai.init.animation_blueprint.asset/1"),
				TEXT("uai.init.animation_blueprint.graph/1"),
				TEXT("uai.init.animation_blueprint.state_machine/1"),
				TEXT("uai.init.animation_blueprint.state/1"),
				TEXT("uai.init.animation_blueprint.transition/1"),
				TEXT("uai.init.animation_blueprint.class_settings/1"),
			};
			Declaration.Migration.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Migration.OwnerId =
				TEXT("T16-ANIMATION-BLUEPRINT");
			return DomainCapability::FDomainCapabilityContribution::TryCreate(
				MoveTemp(Declaration), OutContribution, OutError);
		}

		const DomainCapability::
			FDomainCapabilityContributionFactoryRegistration
			DomainCapabilityRegistration(*DomainId, &TryCreateDomainCapability);

		const FUeAgentActiveCommandContributionFactoryRegistration
			CandidateRegistration(
				TEXT("animation-blueprint-domain-candidates"),
				&TryCreateProductionAnimationBlueprintCandidateContribution);
	}

	const FString& GetProviderRegistrationId()
	{
		return ProviderRegistrationId;
	}

	bool TryCreateProductionAnimationBlueprintCandidateContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError)
	{
		TSharedPtr<IUeAgentAnimationBlueprintCandidateCommandService> Service;
		if (!TryCreateProductionAnimationBlueprintCandidateCommandService(
				Service, OutError)
			|| !Service.IsValid())
		{
			OutContribution.Reset();
			return false;
		}
		OutContribution =
			MakeShared<FAnimationBlueprintCandidateContribution>(
				Service.ToSharedRef());
		OutError.Reset();
		return true;
	}
}

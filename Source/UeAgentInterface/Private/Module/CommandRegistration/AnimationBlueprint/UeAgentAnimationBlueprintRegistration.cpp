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
					TEXT("Optional text filter over native menu actions.")),
				Field(TEXT("describe"), TEXT("object"), false,
					TEXT("Optional {name, fields?} in the same future target, surface and class_settings. Returns creation.supported/reason; editable fields require a created graph node.")),
			};
			Result.ResultFields = {
				Field(TEXT("names"), TEXT("array"), true, TEXT("Distinct readable names from the selected animation surface.")),
				Field(TEXT("scope"), TEXT("object"), true, TEXT("Public future target, surface and supplied class_settings used for discovery; response metadata, not a selector.")),
				Field(TEXT("description"), TEXT("object"), false, TEXT("Selected action's current HJSON creation support, only when describe is requested.")),
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
				{TEXT("animation_blueprint_candidate_not_found"),
					TEXT("The readable name no longer resolves in the recreated context.")},
				{TEXT("animation_blueprint_candidate_ambiguous"),
					TEXT("The readable name resolves more than one action; refine the semantic context.")},
				{TEXT("animation_blueprint_candidate_not_writable"),
					TEXT("The native action is searchable but outside the current HJSON write scope.")},
			};
			Result.Purpose.Summary = TEXT(
				"Search the exact native AnimGraph or StateMachine creation surface in a same-factory transient T16 context, then expose stable readable names for HJSON creation.");
			Result.Purpose.Target = TEXT(
				"One future create-only AnimBlueprint target and one exact native surface.");
			Result.Purpose.Preconditions = {
				TEXT("Use the exact future target that content_graph.import_workspace will create."),
				TEXT("Use the same class_settings in query and HJSON creation."),
				TEXT("Run in the Editor on the UE game thread with uai:query."),
			};
			Result.Purpose.ParameterGuidance = TEXT(
				"Use a returned readable name for creation; keep surface, instance name, graph, position and relations outside candidate.");
			Result.Purpose.ResultGuidance = TEXT(
				"Default discovery contains names and one scope, including search-only actions. For an unfamiliar action request describe={name: returned_name} in the same context for creation.supported/reason. Editable fields resolve after creation. Re-query when target, surface or class_settings changes; scope is response metadata.");
			Result.Purpose.FailureGuidance = TEXT(
				"Changed factory context, stale ids, ambiguity, existing targets, unsupported native actions and invalid pages fail before the first persistent write.");
			Result.Purpose.SideEffectGuidance = TEXT(
				"Read-only; scratch assets and graphs are transient and no package, file, window or external state is created.");
			Result.Purpose.MinimumExample = TEXT(
				"client.animation.state_machine_candidates(target, surface='animation_graph', query='State Machine')");
			Result.Purpose.RelatedFlows = {
				TEXT("query.command.search"),
				TEXT("query.command.usage"),
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
				UE::UAI::Contract::FDiagnostic& OutError) const override
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
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			TSharedPtr<HjsonWorkspace::IImportInitializationDomainProvider>
				Provider;
			HjsonWorkspace::FWorkspaceError WorkspaceError;
			if (!OperationOwners::AnimationBlueprint::
					TryCreateAnimationBlueprintDomainProvider(
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
		UE::UAI::Contract::FDiagnostic& OutError)
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

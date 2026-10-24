#include "Module/CommandRegistration/AnimationRigGraph/UeAgentAnimationRigGraphDomainRegistration.h"

#include "CommandHandlers/AnimationRigGraph/UeAgentAnimationRigGraphDomainCommandHandlers.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "OperationOwners/AnimationRigGraph/UeAgentAnimationRigGraphDomainService.h"

namespace UE::UAI::Module::CommandRegistration::AnimationRigGraph
{
	using namespace Active;
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandHandlers::AnimationRigGraph;
	using namespace CommandRegistry;
	using namespace OperationOwners::AnimationRigGraph;

	namespace
	{
		const FString DomainId = TEXT("animation-rig-graph-domain-candidates");
		const FString ProviderRegistrationId = TEXT("animation-rig-graph-domain-provider");
		const FString CandidateCommandId =
			TEXT("animation_rig_graph.candidates");
		const FString CandidateOwner = TEXT("animation-rig-graph-domain-owner");

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
			Result.PythonNamespace = TEXT("animation_rig_graph");
			Result.PythonFunction = TEXT("candidates");
			Result.ContractVersion = 1;
			Result.PublicOwner = CandidateOwner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = EUeAgentCommandSideEffect::None;
			Result.StateLayers = {TEXT("asset_persisted_state")};
			Result.InputFields = {
				Field(TEXT("target"), TEXT("string"), true, TEXT("Persisted ControlRigBlueprint name from content search, or its exact package.object path; its default RigVM graph supplies the native blank-context menu.")),
				Field(TEXT("query"), TEXT("string"), false, TEXT("Optional text filter over the native menu name, category, tooltip, keywords, and spawner type.")),
				Field(TEXT("describe"), TEXT("object"), false, TEXT("Optional {name, select?, fields?} selected from this target. Returns creation.supported/reason without creating a node; pin fields require a created graph node.")),
			};
			Result.ResultFields = {
				Field(TEXT("names"), TEXT("array"), true, TEXT("Distinct readable native action names.")),
				Field(TEXT("scope"), TEXT("object"), true, TEXT("Public target used for discovery; response metadata, not a selector.")),
				Field(TEXT("description"), TEXT("object"), false, TEXT("Selected native action's current HJSON creation support, only when describe is requested.")),
			};
			Result.Errors = {
				{TEXT("animation_rig_graph_candidate_query_authority_required"), TEXT("The uai:query authority is missing.")},
				{TEXT("animation_rig_graph_candidate_target_unresolved"), TEXT("The exact target could not be loaded.")},
				{TEXT("animation_rig_graph_candidate_target_type_unsupported"), TEXT("The target is not an exact supported animation rig graph root.")},
			};
			Result.Purpose.Summary = TEXT("Search the native default Control Rig action menu; optionally describe one action's current workspace creation support.");
			Result.Purpose.Target = TEXT("One exact persisted ControlRigBlueprint default RigVM graph.");
			Result.Purpose.Preconditions = {
				TEXT("Copy the target unchanged from public content search or readback."),
				TEXT("Run in the Editor on the UE game thread with uai:query."),
			};
			Result.Purpose.ParameterGuidance = TEXT("Search by query. For an unfamiliar action, request describe={name: returned_name} in the same target to check creation.supported/reason; use a supported name in HJSON candidate.name.");
			Result.Purpose.ResultGuidance = TEXT("Default discovery contains names and one scope, including search-only native actions. Optional description reports current HJSON creation support. Pin fields resolve after creation. scope is response metadata, not a selector; re-query when the target changes.");
			Result.Purpose.FailureGuidance = TEXT("Unresolved targets and unsupported types fail before any write.");
			Result.Purpose.SideEffectGuidance = TEXT("Read-only; it creates no node, package, file, or external state.");
			Result.Purpose.MinimumExample = TEXT("client.animation_rig_graph.candidates(target, query='Add Spaces')");
			Result.Purpose.RelatedFlows = {
				TEXT("query.command.search"),
				TEXT("query.command.usage"),
				TEXT("content_graph.summary"),
				TEXT("content_graph.import_workspace"),
				TEXT("animation.control_rig.hierarchy_graph.apply"),
				TEXT("animation.control_rig.shape_library.apply"),
				TEXT("animation.ik_rig.definition.apply"),
				TEXT("animation.ik_rig.definition.auto_generate"),
				TEXT("animation.ik_retargeter.definition.apply"),
				TEXT("animation.ik_retargeter.settings.apply"),
				TEXT("animation.skeleton.definition.apply"),
				TEXT("animation.skeleton.compatibility.apply"),
				TEXT("animation.skeleton.virtual_bone.apply"),
			};
			Result.Purpose.Keywords = {TEXT("animation"), TEXT("control rig"), TEXT("ik rig"), TEXT("skeleton"), TEXT("space"), TEXT("candidate"), TEXT("hjson")};
			Result.Purpose.Synonyms = {TEXT("find rig unit"), TEXT("resolve control rig node"), TEXT("resolve ik graph operation")};
			return Result;
		}

		class FAnimationRigGraphCandidateContribution final : public IUeAgentActiveCommandContribution
		{
		public:
			explicit FAnimationRigGraphCandidateContribution(
				TSharedRef<IUeAgentAnimationRigGraphCandidateCommandService> InService)
				: Service(MoveTemp(InService))
			{
			}

			virtual const FString& GetContributionId() const override
			{
				static const FString Id = TEXT("animation-rig-graph-domain-candidates");
				return Id;
			}

			virtual void CollectDeclarations(TArray<FUeAgentCommandDeclaration>& OutDeclarations) const override
			{
				OutDeclarations.Add(CandidateDeclaration());
			}

			virtual bool CreateHandlers(
				TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
				TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
				UE::UAI::Contract::FDiagnostic& OutError) const override
			{
				(void)Registry;
				return TryCreateAnimationRigGraphCandidateCommandHandlers(Service, OutHandlers, OutError);
			}

		private:
			TSharedRef<IUeAgentAnimationRigGraphCandidateCommandService> Service;
		};

		bool TryCreateDomainCapability(
			TSharedPtr<const DomainCapability::FDomainCapabilityContribution>&
				OutContribution,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			TSharedPtr<HjsonWorkspace::IImportInitializationDomainProvider>
				Provider;
			HjsonWorkspace::FWorkspaceError WorkspaceError;
			if (!OperationOwners::AnimationRigGraph::
					TryCreateAnimationRigGraphDomainProvider(Provider, WorkspaceError)
				|| !Provider.IsValid())
			{
				OutError = WorkspaceError.GetDiagnostic();
				return false;
			}
			DomainCapability::FDomainCapabilityDeclaration Declaration =
				DomainCapability::MakeExplicitlyUnsupportedDomainCapabilityDeclaration(
					DomainId);
			Declaration.Hjson.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Hjson.Providers.Add(Provider.ToSharedRef());
			Declaration.Hjson.Routes = {
				{ProviderRegistrationId, TEXT("content_graph.import_workspace"),
					TEXT("uai.animation_rig_graph.edit/1"),
					TEXT("animation_rig_graph"),
					TEXT("/Script/ControlRigDeveloper.ControlRigBlueprint"),
					HjsonWorkspace::EInitializationNodeRole::RootTarget,
					TEXT("uai.init.rig.control_rig.root/1"), true},
				{ProviderRegistrationId, TEXT("content_graph.import_workspace"),
					TEXT("uai.animation_rig_graph.edit/1"),
					TEXT("animation_rig_graph"),
					TEXT("/Script/IKRig.IKRigDefinition"),
					HjsonWorkspace::EInitializationNodeRole::RootTarget,
					TEXT("uai.init.rig.ik_rig.root/1"), true},
				{ProviderRegistrationId, TEXT("content_graph.import_workspace"),
					TEXT("uai.animation_rig_graph.edit/1"),
					TEXT("animation_rig_graph"),
					TEXT("/Script/IKRig.IKRetargeter"),
					HjsonWorkspace::EInitializationNodeRole::RootTarget,
					TEXT("uai.init.rig.ik_retargeter.root/1"), true},
				{ProviderRegistrationId, TEXT("content_graph.import_workspace"),
					TEXT("uai.animation_rig_graph.edit/1"),
					TEXT("animation_rig_graph"),
					TEXT("/Script/Engine.Skeleton"),
					HjsonWorkspace::EInitializationNodeRole::RootTarget,
					TEXT("uai.init.rig.skeleton.root/1"), true},
				{ProviderRegistrationId, TEXT("content_graph.import_workspace"),
					TEXT("uai.animation_rig_graph.edit/1"),
					TEXT("animation_rig_graph"),
					TEXT("/Script/OptimusCore.OptimusDeformer"),
					HjsonWorkspace::EInitializationNodeRole::RootTarget,
					TEXT("uai.init.rig.deformer_graph.root/1"), true},
			};
			Declaration.Initialization.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Initialization.ContractIds = {
				TEXT("uai.init.rig.control_rig.root/1"),
				TEXT("uai.init.rig.ik_rig.root/1"),
				TEXT("uai.init.rig.ik_retargeter.root/1"),
				TEXT("uai.init.rig.skeleton.root/1"),
				TEXT("uai.init.rig.deformer_graph.root/1"),
				TEXT("uai.init.rig.control/1"),
				TEXT("uai.init.rig.bone/1"),
				TEXT("uai.init.rig.constraint/1"),
				TEXT("uai.init.rig.space/1"),
			};
			return DomainCapability::FDomainCapabilityContribution::TryCreate(
				MoveTemp(Declaration), OutContribution, OutError);
		}

		const DomainCapability::FDomainCapabilityContributionFactoryRegistration
			DomainCapabilityRegistration(*DomainId, &TryCreateDomainCapability);

		const FUeAgentActiveCommandContributionFactoryRegistration CandidateRegistration(
			TEXT("animation-rig-graph-domain-candidates"),
			&TryCreateProductionAnimationRigGraphCandidateContribution);
	}

	const FString& GetProviderRegistrationId()
	{
		return ProviderRegistrationId;
	}

	bool TryCreateProductionAnimationRigGraphCandidateContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		TSharedPtr<IUeAgentAnimationRigGraphCandidateCommandService> Service;
		if (!TryCreateProductionAnimationRigGraphCandidateCommandService(Service, OutError)
			|| !Service.IsValid())
		{
			OutContribution.Reset();
			return false;
		}
		OutContribution = MakeShared<FAnimationRigGraphCandidateContribution>(Service.ToSharedRef());
		OutError.Reset();
		return true;
	}
}

#include "Module/CommandRegistration/AnimationRigGraph/UeAgentAnimationRigGraphDomainRegistration.h"

#include "CommandHandlers/AnimationRigGraph/UeAgentAnimationRigGraphDomainCommandHandlers.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
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
		const FString ProviderRegistrationId = TEXT("animation-rig-graph-domain-provider");
		const FString CandidateCommandId =
			TEXT("animation_rig_graph.candidates");
		const FString CandidateOwner = TEXT("animation-rig-graph-domain-owner");
		const HjsonWorkspace::FImportInitializationDomainProviderFactoryRegistration ProviderRegistration(
			TEXT("animation-rig-graph-domain-provider"),
			&OperationOwners::AnimationRigGraph::TryCreateAnimationRigGraphDomainProvider);

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
				Field(TEXT("target"), TEXT("string"), true, TEXT("Exact persisted ControlRigBlueprint, IKRigDefinition, IKRetargeter, or Skeleton package.object identity.")),
				Field(TEXT("query"), TEXT("string"), false, TEXT("Optional bounded class identity filter.")),
				Field(TEXT("space_ref"), TEXT("string"), false, TEXT("Optional exact explicit placement-space identity used to narrow authoring guidance.")),
				Field(TEXT("limit"), TEXT("integer"), false, TEXT("Optional page size from 1 through 64; defaults to 64.")),
				Field(TEXT("cursor"), TEXT("integer"), false, TEXT("Optional non-negative result offset; defaults to zero.")),
			};
			Result.ResultFields = {
				Field(TEXT("command_id"), TEXT("string"), true, TEXT("The unchanged exact public command identity.")),
				Field(TEXT("read_only"), TEXT("boolean"), true, TEXT("Always true.")),
				Field(TEXT("target"), TEXT("string"), true, TEXT("The unchanged exact target identity.")),
				Field(TEXT("target_revision"), TEXT("string"), true, TEXT("The current persisted target revision.")),
				Field(TEXT("candidates"), TEXT("array"), true, TEXT("Exact concrete native Control Rig unit struct identities with explicit space placement contracts.")),
				Field(TEXT("total_matches"), TEXT("integer"), true, TEXT("Total candidates matching this query.")),
				Field(TEXT("next_cursor"), TEXT("integer"), true, TEXT("Next result offset, or -1 when complete.")),
				Field(TEXT("complete"), TEXT("boolean"), true, TEXT("True only on the final page.")),
				Field(TEXT("has_more"), TEXT("boolean"), true, TEXT("True only while another page remains.")),
			};
			Result.Errors = {
				{TEXT("animation_rig_graph_candidate_query_authority_required"), TEXT("The uai:query authority is missing.")},
				{TEXT("animation_rig_graph_candidate_target_unresolved"), TEXT("The exact target could not be loaded.")},
				{TEXT("animation_rig_graph_candidate_target_type_unsupported"), TEXT("The target is not an exact supported animation rig graph root.")},
				{TEXT("animation_rig_graph_candidate_page_invalid"), TEXT("The requested page is outside the bounded contract.")},
			};
			Result.Purpose.Summary = TEXT("Resolve exact AnimationRigGraph unit creation candidates from loaded native RigUnit struct identities.");
			Result.Purpose.Target = TEXT("One exact persisted Control Rig, IK Rig, IK Retargeter, or Skeleton root.");
			Result.Purpose.Preconditions = {
				TEXT("Copy the target unchanged from public content search or readback."),
				TEXT("Run in the Editor on the UE game thread with uai:query."),
			};
			Result.Purpose.ParameterGuidance = TEXT("Copy candidate.name and binding_revision unchanged into the matching animation rig graph HJSON entry.");
			Result.Purpose.ResultGuidance = TEXT("Treat name, kind, binding_revision, target_revision, and placement contract as opaque exact values; continue paging while has_more is true.");
			Result.Purpose.FailureGuidance = TEXT("Unresolved targets, unsupported types, and invalid pages fail before any write.");
			Result.Purpose.SideEffectGuidance = TEXT("Read-only; it creates no node, package, file, or external state.");
			Result.Purpose.MinimumExample = TEXT("client.animation_rig_graph.candidates(target, query='Transform', space_ref='root')");
			Result.Purpose.RelatedFlows = {
				TEXT("query.command.search"),
				TEXT("query.command.detail"),
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
				FString& OutError) const override
			{
				(void)Registry;
				return TryCreateAnimationRigGraphCandidateCommandHandlers(Service, OutHandlers, OutError);
			}

		private:
			TSharedRef<IUeAgentAnimationRigGraphCandidateCommandService> Service;
		};

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
		FString& OutError)
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

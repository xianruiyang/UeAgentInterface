#include "Module/CommandRegistration/AudioGraph/UeAgentAudioGraphDomainRegistration.h"

#include "CommandHandlers/AudioGraph/UeAgentAudioGraphDomainCommandHandlers.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "OperationOwners/AudioGraph/UeAgentAudioGraphDomainService.h"

namespace UE::UAI::Module::CommandRegistration::AudioGraph
{
	using namespace Active;
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandHandlers::AudioGraph;
	using namespace CommandRegistry;
	using namespace OperationOwners::AudioGraph;

	namespace
	{
		const FString ProviderRegistrationId = TEXT("audio-graph-domain-provider");
		const FString CandidateCommandId = TEXT("audio.graph.candidates");
		const FString CandidateOwner = TEXT("audio-graph-domain-owner");
		const HjsonWorkspace::FImportInitializationDomainProviderFactoryRegistration ProviderRegistration(
			TEXT("audio-graph-domain-candidates"),
			TEXT("audio-graph-domain-provider"),
			&OperationOwners::AudioGraph::TryCreateAudioGraphDomainProvider);

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
			Result.PythonNamespace = TEXT("audio");
			Result.PythonFunction = TEXT("graph_candidates");
			Result.ContractVersion = 1;
			Result.PublicOwner = CandidateOwner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = EUeAgentCommandSideEffect::None;
			Result.StateLayers = {TEXT("asset_persisted_state")};
			Result.InputFields = {
				Field(TEXT("target"), TEXT("string"), true, TEXT("Exact persisted MetaSoundSource or SoundCue package.object identity.")),
				Field(TEXT("query"), TEXT("string"), false, TEXT("Optional bounded class identity filter.")),
				Field(TEXT("limit"), TEXT("integer"), false, TEXT("Optional page size from 1 through 64; defaults to 64.")),
				Field(TEXT("cursor"), TEXT("integer"), false, TEXT("Optional non-negative result offset; defaults to zero.")),
			};
			Result.ResultFields = {
				Field(TEXT("command_id"), TEXT("string"), true, TEXT("The unchanged exact public command identity.")),
				Field(TEXT("read_only"), TEXT("boolean"), true, TEXT("Always true.")),
				Field(TEXT("target"), TEXT("string"), true, TEXT("The unchanged exact target identity.")),
				Field(TEXT("target_revision"), TEXT("string"), true, TEXT("The current persisted target revision.")),
				Field(TEXT("candidates"), TEXT("array"), true, TEXT("Exact concrete SoundCue node classes or registered MetaSound frontend class names and versions.")),
				Field(TEXT("total_matches"), TEXT("integer"), true, TEXT("Total candidates matching this query.")),
				Field(TEXT("next_cursor"), TEXT("integer"), true, TEXT("Next result offset, or -1 when complete.")),
				Field(TEXT("complete"), TEXT("boolean"), true, TEXT("True only on the final page.")),
				Field(TEXT("has_more"), TEXT("boolean"), true, TEXT("True only while another page remains.")),
			};
			Result.Errors = {
				{TEXT("audio_graph_candidate_query_authority_required"), TEXT("The uai:query authority is missing.")},
				{TEXT("audio_graph_candidate_target_unresolved"), TEXT("The exact target could not be loaded.")},
				{TEXT("audio_graph_candidate_target_type_unsupported"), TEXT("The target is not an exact supported audio graph root.")},
				{TEXT("audio_graph_candidate_page_invalid"), TEXT("The requested page is outside the bounded contract.")},
			};
			Result.Purpose.Summary = TEXT("Resolve exact SoundCue and MetaSound graph creation candidates from loaded UE class and frontend registry identities.");
			Result.Purpose.Target = TEXT("One exact persisted MetaSoundSource or SoundCue.");
			Result.Purpose.Preconditions = {
				TEXT("Copy the target unchanged from public content search or readback."),
				TEXT("Run in the Editor on the UE game thread with uai:query."),
			};
			Result.Purpose.ParameterGuidance = TEXT("Copy candidate.name and binding_revision unchanged into the matching audio graph HJSON entry.");
			Result.Purpose.ResultGuidance = TEXT("Treat name, kind, binding_revision, target_revision, and placement contract as opaque exact values; continue paging while has_more is true.");
			Result.Purpose.FailureGuidance = TEXT("Unresolved targets, unsupported types, and invalid pages fail before any write.");
			Result.Purpose.SideEffectGuidance = TEXT("Read-only; it creates no node, package, file, or external state.");
			Result.Purpose.MinimumExample = TEXT("client.audio.graph_candidates(target, query='Wave')");
			Result.Purpose.RelatedFlows = {
				TEXT("query.command.search"),
				TEXT("query.command.detail"),
				TEXT("content_graph.summary"),
				TEXT("content_graph.import_workspace"),
				TEXT("metasound.graph.interface.mutate"),
				TEXT("metasound.graph.node.mutate"),
				TEXT("metasound.graph.link.mutate"),
				TEXT("metasound.graph.default.mutate"),
				TEXT("metasound.graph.compile_settle"),
				TEXT("sound_cue.graph.node.mutate"),
				TEXT("sound_cue.graph.relation.mutate"),
				TEXT("sound_cue.graph.root.mutate"),
				TEXT("sound_cue.graph.compile_settle"),
			};
			Result.Purpose.Keywords = {TEXT("audio"), TEXT("metasound"), TEXT("sound cue"), TEXT("candidate"), TEXT("hjson")};
			Result.Purpose.Synonyms = {TEXT("find audio node"), TEXT("resolve metasound class"), TEXT("resolve sound cue node")};
			return Result;
		}

		class FAudioGraphCandidateContribution final : public IUeAgentActiveCommandContribution
		{
		public:
			explicit FAudioGraphCandidateContribution(
				TSharedRef<IUeAgentAudioGraphCandidateCommandService> InService)
				: Service(MoveTemp(InService))
			{
			}

			virtual const FString& GetContributionId() const override
			{
				static const FString Id = TEXT("audio-graph-domain-candidates");
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
				return TryCreateAudioGraphCandidateCommandHandlers(Service, OutHandlers, OutError);
			}

		private:
			TSharedRef<IUeAgentAudioGraphCandidateCommandService> Service;
		};

		const FUeAgentActiveCommandContributionFactoryRegistration CandidateRegistration(
			TEXT("audio-graph-domain-candidates"),
			&TryCreateProductionAudioGraphCandidateContribution);
	}

	const FString& GetProviderRegistrationId()
	{
		return ProviderRegistrationId;
	}

	bool TryCreateProductionAudioGraphCandidateContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError)
	{
		TSharedPtr<IUeAgentAudioGraphCandidateCommandService> Service;
		if (!TryCreateProductionAudioGraphCandidateCommandService(Service, OutError)
			|| !Service.IsValid())
		{
			OutContribution.Reset();
			return false;
		}
		OutContribution = MakeShared<FAudioGraphCandidateContribution>(Service.ToSharedRef());
		OutError.Reset();
		return true;
	}
}

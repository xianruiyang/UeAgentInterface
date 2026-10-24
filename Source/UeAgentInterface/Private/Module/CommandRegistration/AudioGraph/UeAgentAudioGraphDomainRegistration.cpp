#include "Module/CommandRegistration/AudioGraph/UeAgentAudioGraphDomainRegistration.h"

#include "CommandHandlers/AudioGraph/UeAgentAudioGraphDomainCommandHandlers.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
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
		const FString DomainId = TEXT("audio-graph-domain-candidates");
		const FString ProviderRegistrationId = TEXT("audio-graph-domain-provider");
		const FString CandidateCommandId = TEXT("audio.graph.candidates");
		const FString CandidateOwner = TEXT("audio-graph-domain-owner");

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
				Field(TEXT("target"), TEXT("string"), true, TEXT("Persisted MetaSoundSource or SoundCue name from content search, or its exact package.object path.")),
				Field(TEXT("query"), TEXT("string"), false, TEXT("Optional candidate text filter.")),
				Field(TEXT("describe"), TEXT("object"), false, TEXT("Optional selected {name, select?, fields?}; SoundCue returns editable native defaults, MetaSound reports its created-instance dependency.")),
			};
			Result.ResultFields = { Field(TEXT("names"), TEXT("array"), true, TEXT("Distinct readable audio graph action names.")),
				Field(TEXT("scope"), TEXT("object"), true, TEXT("Public target used for discovery; response metadata, not a selector.")),
				Field(TEXT("description"), TEXT("object"), false, TEXT("Selected candidate defaults or explicit created-instance dependency; only for describe.")) };
			Result.Errors = {
				{TEXT("audio_graph_candidate_query_authority_required"), TEXT("The uai:query authority is missing.")},
				{TEXT("audio_graph_candidate_target_unresolved"), TEXT("The exact target could not be loaded.")},
				{TEXT("audio_graph_candidate_target_type_unsupported"), TEXT("The target is not an exact supported audio graph root.")},
				{TEXT("audio_graph_candidate_not_found"), TEXT("The readable MetaSound name no longer resolves in the current frontend registry.")},
				{TEXT("audio_graph_candidate_ambiguous"), TEXT("The readable MetaSound name resolves more than one current version; refine the semantic context.")},
			};
			Result.Purpose.Summary = TEXT("Resolve compact MetaSound frontend candidates and legacy SoundCue class candidates with truthful native-menu coverage diagnostics.");
			Result.Purpose.Target = TEXT("One exact persisted MetaSoundSource or SoundCue.");
			Result.Purpose.Preconditions = {
				TEXT("Copy the target unchanged from public content search or readback."),
				TEXT("Run in the Editor on the UE game thread with uai:query."),
			};
			Result.Purpose.ParameterGuidance = TEXT("Use a returned readable name for creation; source kind and version are short semantic selection only when needed.");
			Result.Purpose.ResultGuidance = TEXT("Search returns names and one scope; re-query needed names when the target changes. describe returns defaults or an explicit instance dependency. scope is response metadata, not a selector.");
			Result.Purpose.FailureGuidance = TEXT("Unresolved targets and unsupported types fail before any write.");
			Result.Purpose.SideEffectGuidance = TEXT("Read-only; it creates no node, package, file, or external state.");
			Result.Purpose.MinimumExample = TEXT("client.audio.graph_candidates(target, query='UE.Sine.Audio')");
			Result.Purpose.RelatedFlows = {
				TEXT("query.command.search"),
				TEXT("query.command.usage"),
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
				UE::UAI::Contract::FDiagnostic& OutError) const override
			{
				(void)Registry;
				return TryCreateAudioGraphCandidateCommandHandlers(Service, OutHandlers, OutError);
			}

		private:
			TSharedRef<IUeAgentAudioGraphCandidateCommandService> Service;
		};

		bool TryCreateDomainCapability(
			TSharedPtr<const DomainCapability::FDomainCapabilityContribution>&
				OutContribution,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			TSharedPtr<HjsonWorkspace::IImportInitializationDomainProvider>
				Provider;
			HjsonWorkspace::FWorkspaceError WorkspaceError;
			if (!OperationOwners::AudioGraph::TryCreateAudioGraphDomainProvider(
					Provider, WorkspaceError)
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
					TEXT("uai.audio_graph.edit/1"), TEXT("audio_graph"),
					TEXT("/Script/MetasoundEngine.MetaSoundSource"),
					HjsonWorkspace::EInitializationNodeRole::RootTarget,
					TEXT("uai.init.audio.metasound_source.root/1"), true},
				{ProviderRegistrationId, TEXT("content_graph.import_workspace"),
					TEXT("uai.audio_graph.edit/1"), TEXT("audio_graph"),
					TEXT("/Script/Engine.SoundCue"),
					HjsonWorkspace::EInitializationNodeRole::RootTarget,
					TEXT("uai.init.audio.sound_cue.root/1"), true},
			};
			Declaration.Initialization.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Initialization.ContractIds = {
				TEXT("uai.init.audio.metasound_source.root/1"),
				TEXT("uai.init.audio.sound_cue.root/1"),
				TEXT("uai.init.audio.sound_cue_node/1"),
				TEXT("uai.init.audio.endpoint/1"),
				TEXT("uai.init.audio.connection/1"),
				TEXT("uai.init.audio.parameter/1"),
				TEXT("uai.init.audio.external_reference/1"),
			};
			return DomainCapability::FDomainCapabilityContribution::TryCreate(
				MoveTemp(Declaration), OutContribution, OutError);
		}

		const DomainCapability::FDomainCapabilityContributionFactoryRegistration
			DomainCapabilityRegistration(*DomainId, &TryCreateDomainCapability);

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
		UE::UAI::Contract::FDiagnostic& OutError)
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

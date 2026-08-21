#include "Misc/AutomationTest.h"

#include "MetasoundSource.h"
#include "Sound/SoundCue.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"

#if __has_include("OperationOwners/AudioGraph/UeAgentAudioGraphDomainService.h")
#define UAI_T21_AUDIO_DOMAIN_AVAILABLE 1
#include "OperationOwners/AudioGraph/UeAgentAudioGraphDomainService.h"
#else
#define UAI_T21_AUDIO_DOMAIN_AVAILABLE 0
#endif

#if __has_include("QueryOwners/AudioGraph/UeAgentAudioGraphCandidateQuery.h")
#define UAI_T21_AUDIO_CANDIDATE_QUERY_AVAILABLE 1
#include "QueryOwners/AudioGraph/UeAgentAudioGraphCandidateQuery.h"
#else
#define UAI_T21_AUDIO_CANDIDATE_QUERY_AVAILABLE 0
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T21AudioDomainTest,
	"UAI.StrictV2.T21.AUDIO.DOMAIN",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T21AudioDomainTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
#if !UAI_T21_AUDIO_DOMAIN_AVAILABLE
	TestTrue(TEXT("T21 audio domain authority is available"), false);
	return false;
#elif !UAI_T21_AUDIO_CANDIDATE_QUERY_AVAILABLE
	TestTrue(TEXT("T21 audio candidate query is available"), false);
	return false;
#else
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::OperationOwners::AudioGraph;

	TSharedPtr<IImportInitializationDomainProvider> Provider;
	FWorkspaceError Error;
	if (!TestTrue(
		TEXT("the audio graph domain provider is created"),
		TryCreateAudioGraphDomainProvider(Provider, Error))
		|| !Provider.IsValid())
	{
		AddError(Error.GetCode());
		return false;
	}

	struct FCase
	{
		const TCHAR* Type;
		EInitializationNodeRole Role;
		const TCHAR* Contract;
	};
	const TArray<FCase> Cases = {
		{TEXT("/Script/MetasoundEngine.MetaSoundSource"),
			EInitializationNodeRole::RootTarget,
			TEXT("uai.init.audio.metasound_source.root/1")},
		{TEXT("/Script/Engine.SoundCue"),
			EInitializationNodeRole::RootTarget,
			TEXT("uai.init.audio.sound_cue.root/1")},
		{TEXT("/Script/Engine.SoundNode"),
			EInitializationNodeRole::OwnedObject,
			TEXT("uai.init.audio.sound_cue_node/1")},
		{TEXT("uai.audio.endpoint"), EInitializationNodeRole::ContainerItem,
			TEXT("uai.init.audio.endpoint/1")},
		{TEXT("uai.audio.connection"), EInitializationNodeRole::ContainerItem,
			TEXT("uai.init.audio.connection/1")},
		{TEXT("uai.audio.parameter"), EInitializationNodeRole::ContainerItem,
			TEXT("uai.init.audio.parameter/1")},
		{TEXT("uai.audio.external_reference"), EInitializationNodeRole::ContainerItem,
			TEXT("uai.init.audio.external_reference/1")},
	};
	for (const FCase& Case : Cases)
	{
		const FImportInitializationDomainRequest Request(
			TEXT("content_graph.import_workspace"),
			TEXT("uai.audio_graph.edit/1"),
			TEXT("/Game/UAI/StrictV2/T21/Target.Target"),
			Case.Type,
			Case.Role,
			TEXT("{}"));
		FString Contract;
		TestTrue(
			*FString::Printf(TEXT("the audio initialization surface resolves: %s"), Case.Type),
			Provider->Matches(Request)
				&& Provider->TryResolveInitializationContract(Request, Contract, Error));
		TestEqual(TEXT("the audio initialization contract is exact"), Contract,
			FString(Case.Contract));
	}

	const FImportInitializationDomainRequest Unsupported(
		TEXT("content_graph.import_workspace"),
		TEXT("uai.audio_graph.edit/1"),
		TEXT("/Game/UAI/StrictV2/T21/Unsupported.Unsupported"),
		TEXT("/Script/Engine.SoundWave"),
		EInitializationNodeRole::RootTarget,
		TEXT("{}"));
	TestFalse(TEXT("an unregistered audio root is rejected before write"),
		Provider->Matches(Unsupported));

	using namespace UE::UAI::QueryOwners::AudioGraph;
	USoundCue* SoundCueTarget = NewObject<USoundCue>(GetTransientPackage());
	FAudioGraphCandidateQueryResult SoundCueResult;
	FAudioGraphCandidateQueryError CandidateError;
	if (!TestTrue(
		TEXT("the audio owner resolves real Sound Cue candidates"),
		TryQueryAudioGraphCandidates(
			*SoundCueTarget, TEXT("Wave"), SoundCueResult, CandidateError)))
	{
		AddError(CandidateError.Code + TEXT(":") + CandidateError.Detail);
		return false;
	}
	TestTrue(TEXT("the Sound Cue catalog is non-empty"),
		!SoundCueResult.Candidates.IsEmpty());

	UMetaSoundSource* MetaSoundTarget = NewObject<UMetaSoundSource>(GetTransientPackage());
	FAudioGraphCandidateQueryResult MetaSoundResult;
	if (!TestTrue(
		TEXT("the audio owner resolves real MetaSound candidates"),
		TryQueryAudioGraphCandidates(
			*MetaSoundTarget, TEXT(""), MetaSoundResult, CandidateError)))
	{
		AddError(CandidateError.Code + TEXT(":") + CandidateError.Detail);
		return false;
	}
	TestTrue(TEXT("the MetaSound registry catalog is non-empty"),
		!MetaSoundResult.Candidates.IsEmpty());

	for (const FAudioGraphCandidateQueryResult* Result : {&SoundCueResult, &MetaSoundResult})
	{
		UObject& BoundTarget = Result == &SoundCueResult
			? static_cast<UObject&>(*SoundCueTarget)
			: static_cast<UObject&>(*MetaSoundTarget);
		for (const FAudioGraphCandidate& Candidate : Result->Candidates)
		{
			TestFalse(TEXT("candidate name is present"), Candidate.Name.IsEmpty());
			TestTrue(TEXT("candidate kind is a real audio graph kind"),
				Candidate.Kind == TEXT("sound_cue_node")
					|| Candidate.Kind == TEXT("metasound_node"));
			TestTrue(TEXT("candidate declares create support"),
				Candidate.SupportedOperations.Contains(TEXT("create")));
			TestFalse(TEXT("candidate input contract is present"),
				Candidate.InputContract.IsEmpty());
			const bool bSoundCueCandidate =
				Candidate.Kind == TEXT("sound_cue_node");
			TestEqual(TEXT("candidate placement contract matches its native graph"),
				Candidate.PlacementContract,
				bSoundCueCandidate
					? FString(TEXT("uai.audio.sound-cue-node-identity/1"))
					: FString(TEXT("uai.audio.metasound-node-identity/1")));
			TestEqual(TEXT("candidate required context matches its native identity"),
				Candidate.RequiredContext,
				bSoundCueCandidate
					? FString(TEXT("target_ref:object_path"))
					: FString(TEXT("target_ref:node_guid")));
			TestEqual(TEXT("candidate is bound to the queried target"),
				Candidate.TargetRef, Result->Target);
			TestEqual(TEXT("candidate carries the queried target revision"),
				Candidate.TargetRevision, Result->TargetRevision);
			TestTrue(TEXT("candidate binding revision is a sha256 identity"),
				Candidate.BindingRevision.StartsWith(TEXT("sha256:")));
			TestTrue(TEXT("candidate binding is current and directly usable"),
				IsCurrentAudioGraphCandidateBinding(
					BoundTarget,
					Candidate.TargetRef,
					Candidate.TargetRevision,
					Candidate.Name,
					Candidate.Kind,
					Candidate.SupportedOperations,
					Candidate.InputContract,
					Candidate.PlacementContract,
					Candidate.RequiredContext,
					Candidate.MajorVersion,
					Candidate.MinorVersion,
					Candidate.BindingRevision,
					CandidateError));
		}
		TestEqual(TEXT("complete is the inverse of has_more"),
			Result->bComplete, !Result->bHasMore);
		TestEqual(TEXT("next_cursor truthfully describes the next page"),
			Result->NextCursor,
			Result->bHasMore ? Result->Candidates.Num() : INDEX_NONE);
	}
	return !HasAnyErrors();
#endif
}

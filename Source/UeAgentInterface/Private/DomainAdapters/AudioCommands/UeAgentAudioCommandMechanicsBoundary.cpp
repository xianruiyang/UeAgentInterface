#include "DomainAdapters/AudioCommands/UeAgentAudioCommandMechanicsBoundary.h"

#include "Dom/JsonValue.h"

namespace UE::UAI::DomainAdapters::AudioCommands
{
	const TArray<FString>& GetCommandIds()
	{
		static const TArray<FString> Ids = {
			TEXT("audio_media.asset.create"),
			TEXT("metasound.graph.compile_settle"),
			TEXT("metasound.graph.default.mutate"),
			TEXT("metasound.graph.interface.mutate"),
			TEXT("metasound.graph.link.mutate"),
			TEXT("metasound.graph.node.mutate"),
			TEXT("sound_cue.graph.compile_settle"),
			TEXT("sound_cue.graph.node.mutate"),
			TEXT("sound_cue.graph.relation.mutate"),
			TEXT("sound_cue.graph.root.mutate"),
		};
		return Ids;
	}

	const TArray<FString>& GetPublicCommandIds()
	{
		static const TArray<FString> Ids = {
			TEXT("audio_media.asset.create"),
			TEXT("metasound.graph.compile_settle"),
			TEXT("sound_cue.graph.compile_settle"),
		};
		return Ids;
	}

	bool IsQuery(const FString& CommandId) { return false; }
	bool TryGetActivationProfiles(
		TArray<Composition::GraphCompound::FUeAgentGraphCompoundActivationProfile>& OutProfiles,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutProfiles.Reset();
		OutError.Reset();
		const TArray<Composition::GraphCompound::FUeAgentGraphCompoundActivationProfile>* GraphProfiles = nullptr;
		if (!Composition::GraphCompound::TryGetGraphCompoundActivationProfiles(GraphProfiles, OutError)) return false;
		const auto& Canonical = *GraphProfiles;
		for (const FString& CommandId : GetCommandIds())
		{
			const auto* Match = Canonical.FindByPredicate(
				[&CommandId](const auto& Profile) { return Profile.GetCommandId() == CommandId; });
			if (!Match || !Match->IsValid())
			{
				OutProfiles.Reset();
				if (Match) {
					OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("audio_command_profile_missing"),
						Match->GetValidationDiagnostic(), TEXT("runtime_factory"),
						TEXT("A required audio activation profile failed validation."));
				} else {
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("audio_command_profile_missing"),
						TEXT("No activation profile is registered for a required audio command."), TEXT("runtime_factory"));
					OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
					OutError.PropertyPath = TEXT("activation_profile_registered");
				}
				OutError.InternalContext = CommandId;
				OutError.SourceKind = TEXT("native_result");
				if (GetPublicCommandIds().Contains(CommandId))
					OutError.PublicMessage += TEXT(" Command: ") + CommandId + TEXT(".");
				return false;
			}
			OutProfiles.Add(*Match);
		}
		return OutProfiles.Num() == GetCommandIds().Num();
	}
}

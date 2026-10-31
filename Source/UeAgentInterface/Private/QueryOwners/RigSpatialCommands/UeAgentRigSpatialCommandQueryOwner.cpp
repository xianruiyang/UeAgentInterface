#include "QueryOwners/RigSpatialCommands/UeAgentRigSpatialCommandQueryOwner.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::QueryOwners::RigSpatialCommands
{
	const TArray<FString>& GetCommandIds()
	{
		static const TArray<FString> Ids = {
			TEXT("animation.ik_probe_candidate.apply"),
			TEXT("animation.subject.morph_weights.update"),
			TEXT("animation.subject.pose.update"),
			TEXT("rig.control.bake.animation"),
			TEXT("rig.control.bake.context"),
			TEXT("rig.control.bake.controls"),
			TEXT("rig.ik.definition.update"),
			TEXT("rig.retarget.assets.generate"),
			TEXT("rig.retarget.definition.update"),
			TEXT("rig.retarget.pose.auto_align"),
			TEXT("rig.retarget.pose.update"),
			TEXT("rig.skeleton.preview_mesh.bind"),
			TEXT("rig.skeleton.socket.update"),
		};
		return Ids;
	}

	bool IsCommand(const FString& CommandId) { return GetCommandIds().Contains(CommandId); }

	bool TryGetTimelineProfiles(
		TArray<Composition::TimelineSpatial::FUeAgentTimelineSpatialActivationProfile>& OutProfiles,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutProfiles.Reset();
		OutError.Reset();
		const TArray<Composition::TimelineSpatial::FUeAgentTimelineSpatialActivationProfile>* Canonical = nullptr;
		if (!Composition::TimelineSpatial::TryGetTimelineSpatialActivationProfiles(Canonical, OutError)) return false;
		for (const FString& CommandId : GetCommandIds())
		{
			const auto* Match = Canonical->FindByPredicate(
				[&CommandId](const auto& Profile) { return Profile.CommandId == CommandId; });
			if (!Match)
			{
				OutProfiles.Reset();
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("rig_spatial_profile_missing"),
					FString::Printf(TEXT("Command '%s' has no registered Timeline/Spatial activation profile."), *CommandId),
					TEXT("registration"), FString(), CommandId);
				OutError.SourceKind = TEXT("native_result");
				OutError.ActualValuePresent = false;
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("registered Timeline/Spatial activation profile"));
				return false;
			}
			if (!Match->TryValidate(OutError))
			{
				OutProfiles.Reset();
				OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(
					TEXT("rig_spatial_profile_missing"), OutError, TEXT("registration"));
				return false;
			}
			OutProfiles.Add(*Match);
		}
		return OutProfiles.Num() == GetCommandIds().Num();
	}

	bool IsQuery(const FString& CommandId)
	{
		const TArray<Composition::TimelineSpatial::FUeAgentTimelineSpatialActivationProfile>* Canonical = nullptr;
		Contract::FDiagnostic ProfileError;
		if (!Composition::TimelineSpatial::TryGetTimelineSpatialActivationProfiles(Canonical, ProfileError)) return false;
		const auto* Match = Canonical->FindByPredicate(
			[&CommandId](const auto& Profile) { return Profile.CommandId == CommandId; });
		return Match && Match->OperationKind == TEXT("query");
	}

	const TArray<FString>& GetReadbackFlows()
	{
		static const TArray<FString> Values = {
			TEXT("query.command.usage"),
			TEXT("animation.skeleton.slots.read"),
		};
		return Values;
	}
}

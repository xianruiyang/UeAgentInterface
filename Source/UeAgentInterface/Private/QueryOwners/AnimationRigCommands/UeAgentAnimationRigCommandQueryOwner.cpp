#include "QueryOwners/AnimationRigCommands/UeAgentAnimationRigCommandQueryOwner.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::QueryOwners::AnimationRigCommands
{
	const TArray<FString>& GetCommandIds()
	{
		static const TArray<FString> Ids = {
			TEXT("animation.control_rig.asset.create"),
			TEXT("animation.control_rig.hierarchy_graph.apply"),
			TEXT("animation.control_rig.shape_library.apply"),
			TEXT("animation.control_rig.shape_library.asset.create"),
			TEXT("animation.deformer_graph.topology.apply"),
			TEXT("animation.ik_retargeter.asset.create"),
			TEXT("animation.ik_retargeter.definition.apply"),
			TEXT("animation.ik_retargeter.settings.apply"),
			TEXT("animation.ik_rig.asset.create"),
			TEXT("animation.ik_rig.definition.apply"),
			TEXT("animation.ik_rig.definition.auto_generate"),
			TEXT("animation.skeleton.compatibility.apply"),
			TEXT("animation.skeleton.definition.apply"),
			TEXT("animation.skeleton.slots.read"),
			TEXT("animation.skeleton.virtual_bone.apply"),
		};
		return Ids;
	}

	bool IsCommand(const FString& CommandId) { return GetCommandIds().Contains(CommandId); }

	bool TryGetGraphProfiles(
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
			if (CommandId == TEXT("animation.skeleton.slots.read")) continue;
			const auto* Match = Canonical.FindByPredicate(
				[&CommandId](const auto& Profile) { return Profile.GetCommandId() == CommandId; });
			if (!Match)
			{
				OutProfiles.Reset();
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_rig_graph_profile_missing"),
					FString::Printf(TEXT("Command '%s' has no registered Graph activation profile."), *CommandId),
					TEXT("registration"), FString(), CommandId);
				OutError.SourceKind = TEXT("native_result");
				OutError.ActualValuePresent = false;
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("registered Graph activation profile"));
				return false;
			}
			if (!Match->IsValid())
			{
				OutProfiles.Reset();
				OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("animation_rig_graph_profile_missing"),
					Match->GetValidationDiagnostic(), TEXT("registration"));
				return false;
			}
			OutProfiles.Add(*Match);
		}
		return OutProfiles.Num() == GetCommandIds().Num() - 1;
	}

	bool TryGetTimelineProfiles(
		TArray<Composition::TimelineSpatial::FUeAgentTimelineSpatialActivationProfile>& OutProfiles,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutProfiles.Reset();
		OutError.Reset();
		const FString CommandId(TEXT("animation.skeleton.slots.read"));
		const TArray<Composition::TimelineSpatial::FUeAgentTimelineSpatialActivationProfile>* Canonical = nullptr;
		if (!Composition::TimelineSpatial::TryGetTimelineSpatialActivationProfiles(Canonical, OutError)) return false;
		const auto* Match = Canonical->FindByPredicate(
			[&CommandId](const auto& Profile) { return Profile.CommandId == CommandId; });
		if (!Match)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_rig_timeline_profile_missing"),
				FString::Printf(TEXT("Command '%s' has no registered Timeline/Spatial activation profile."), *CommandId),
				TEXT("registration"), FString(), CommandId);
			OutError.SourceKind = TEXT("native_result");
			OutError.ActualValuePresent = false;
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("registered Timeline/Spatial activation profile"));
			return false;
		}
		if (!Match->TryValidate(OutError))
		{
			OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(
				TEXT("animation_rig_timeline_profile_missing"), OutError, TEXT("registration"));
			return false;
		}
		OutProfiles.Add(*Match);
		return true;
	}

	bool IsQuery(const FString& CommandId)
	{
		const TArray<Composition::GraphCompound::FUeAgentGraphCompoundActivationProfile>* GraphProfiles = nullptr;
		Contract::FDiagnostic GraphProfileError;
		const auto* GraphMatch = Composition::GraphCompound::TryGetGraphCompoundActivationProfiles(GraphProfiles, GraphProfileError)
			? GraphProfiles->FindByPredicate(
				[&CommandId](const auto& Profile) { return Profile.GetCommandId() == CommandId; }) : nullptr;
		if (GraphMatch) return GraphMatch->GetOperationKind() == TEXT("query");
		const TArray<Composition::TimelineSpatial::FUeAgentTimelineSpatialActivationProfile>* Canonical = nullptr;
		Contract::FDiagnostic ProfileError;
		if (!Composition::TimelineSpatial::TryGetTimelineSpatialActivationProfiles(Canonical, ProfileError)) return false;
		const auto* TimelineMatch = Canonical->FindByPredicate(
			[&CommandId](const auto& Profile) { return Profile.CommandId == CommandId; });
		return TimelineMatch && TimelineMatch->OperationKind == TEXT("query");
	}

	const TArray<FString>& GetReadbackFlows()
	{
		static const TArray<FString> Values = {
			TEXT("animation.skeleton.slots.read"),
			TEXT("query.command.usage"),
		};
		return Values;
	}
}

#include "QueryOwners/AnimationCurveCommands/UeAgentAnimationCurveCommandQueryOwner.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::QueryOwners::AnimationCurveCommands
{
	const TArray<FString>& GetCommandIds()
	{
		static const TArray<FString> Ids = {
			TEXT("animation.blend_space.asset.create"),
			TEXT("animation.blend_space.definition.apply"),
			TEXT("animation.blend_space.sample"),
			TEXT("animation.montage.create"),
			TEXT("animation.montage.read"),
			TEXT("animation.montage.structure.replace"),
			TEXT("animation.sequence.bone_tracks.replace"),
			TEXT("animation.sequence.curves.update"),
			TEXT("animation.sequence.events.update"),
			TEXT("animation.sequence.metadata.update"),
			TEXT("animation.sequence.pose.sample"),
			TEXT("animation.sequence.read"),
			TEXT("animation.sequence.root_motion.analyze"),
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
		static const TArray<FString> GraphCommandIds = {
			TEXT("animation.blend_space.asset.create"),
			TEXT("animation.blend_space.definition.apply"),
		};
		const TArray<Composition::GraphCompound::FUeAgentGraphCompoundActivationProfile>* GraphProfiles = nullptr;
		if (!Composition::GraphCompound::TryGetGraphCompoundActivationProfiles(GraphProfiles, OutError)) return false;
		const auto& Canonical = *GraphProfiles;
		for (const FString& CommandId : GraphCommandIds)
		{
			const auto* Match = Canonical.FindByPredicate(
				[&CommandId](const auto& Profile) { return Profile.GetCommandId() == CommandId; });
			if (!Match)
			{
				OutProfiles.Reset();
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_curve_graph_profile_missing"),
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
				OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("animation_curve_graph_profile_missing"),
					Match->GetValidationDiagnostic(), TEXT("registration"));
				return false;
			}
			OutProfiles.Add(*Match);
		}
		return OutProfiles.Num() == GraphCommandIds.Num();
	}

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
			if (CommandId == TEXT("animation.blend_space.asset.create")
				|| CommandId == TEXT("animation.blend_space.definition.apply"))
			{
				continue;
			}
			const auto* Match = Canonical->FindByPredicate(
				[&CommandId](const auto& Profile) { return Profile.CommandId == CommandId; });
			if (!Match)
			{
				OutProfiles.Reset();
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_curve_profile_missing"),
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
					TEXT("animation_curve_profile_missing"), OutError, TEXT("registration"));
				return false;
			}
			OutProfiles.Add(*Match);
		}
		return OutProfiles.Num() == GetCommandIds().Num() - 2;
	}

	bool IsQuery(const FString& CommandId)
	{
		const TArray<Composition::GraphCompound::FUeAgentGraphCompoundActivationProfile>* GraphProfiles = nullptr;
		Contract::FDiagnostic GraphProfileError;
		const auto* GraphMatch = Composition::GraphCompound::TryGetGraphCompoundActivationProfiles(GraphProfiles, GraphProfileError)
			? GraphProfiles->FindByPredicate(
				[&CommandId](const auto& Profile) { return Profile.GetCommandId() == CommandId; }) : nullptr;
		if (GraphMatch)
		{
			return GraphMatch->GetOperationKind() == TEXT("query");
		}
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
			TEXT("animation.sequence.read"),
			TEXT("animation.montage.read"),
			TEXT("animation.blend_space.sample"),
		};
		return Values;
	}
}

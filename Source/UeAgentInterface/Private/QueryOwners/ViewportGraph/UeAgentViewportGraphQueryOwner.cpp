#include "QueryOwners/ViewportGraph/UeAgentViewportGraphQueryOwner.h"

#include "Dom/JsonValue.h"

namespace UE::UAI::QueryOwners::ViewportGraph
{
	const TArray<FString>& GetCommandIds()
	{
		static const TArray<FString> Ids = {
			TEXT("graph.view.read"),
			TEXT("graph.view.update"),
			TEXT("viewport.actor.pick"),
			TEXT("viewport.actor.select"),
			TEXT("viewport.camera.frame"),
			TEXT("viewport.instance.list"),
			TEXT("viewport.pixel_scale.read"),
			TEXT("viewport.ray.deproject"),
			TEXT("viewport.ray.trace"),
			TEXT("viewport.state.read"),
			TEXT("viewport.state.restore"),
			TEXT("viewport.state.update"),
			TEXT("viewport.view_mode.list"),
			TEXT("viewport.warning.list"),
		};
		return Ids;
	}

	bool IsCommand(const FString& CommandId)
	{
		return GetCommandIds().Contains(CommandId);
	}

	bool TryGetProfiles(
		TArray<Composition::TimelineSpatial::FUeAgentTimelineSpatialActivationProfile>&
			OutProfiles,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutProfiles.Reset();
		OutError.Reset();
		const TArray<Composition::TimelineSpatial::FUeAgentTimelineSpatialActivationProfile>* Canonical = nullptr;
		if (!Composition::TimelineSpatial::TryGetTimelineSpatialActivationProfiles(Canonical, OutError)) return false;
		for (const FString& CommandId : GetCommandIds())
		{
			const auto* Match = Canonical->FindByPredicate(
				[&CommandId](const auto& Profile)
				{
					return Profile.CommandId == CommandId;
				});
			if (!Match)
			{
				OutProfiles.Reset();
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("viewport_graph_profile_missing"),
					TEXT("No Timeline/Spatial activation profile is registered for this command"), TEXT("registration"));
				OutError.SourceKind = TEXT("native_result");
				OutError.CandidateName = CommandId;
				OutError.PropertyPath = TEXT("activation_profile_present");
				OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
				OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				return false;
			}
			if (!Match->TryValidate(OutError))
			{
				OutProfiles.Reset();
				OutError = Contract::FDiagnostic::WrapFailure(TEXT("viewport_graph_profile_missing"), OutError, TEXT("registration"));
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
				[&CommandId](const auto& Profile)
				{
					return Profile.CommandId == CommandId;
				});
		return Match && Match->OperationKind == TEXT("query");
	}

	const TArray<FString>& GetReadbackFlows()
	{
		static const TArray<FString> Values = {
			TEXT("viewport.state.read"),
			TEXT("graph.view.read"),
			TEXT("viewport.warning.list"),
		};
		return Values;
	}
}

#include "QueryOwners/SpatialContent/UeAgentSpatialContentQueryOwner.h"

#include "Dom/JsonValue.h"

namespace UE::UAI::QueryOwners::SpatialContent
{
	const TArray<FString>& GetCommandIds()
	{
		static const TArray<FString> Ids = {
			TEXT("mesh.element.select_by_ray"),
			TEXT("mesh.pivot.update"),
			TEXT("mesh.transform.bake"),
			TEXT("world.object_align"),
			TEXT("world.object_snap_to_surface"),
			TEXT("world.object_transform.read"),
			TEXT("world.object_transform.update"),
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
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("spatial_content_profile_missing"),
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
				OutError = Contract::FDiagnostic::WrapFailure(TEXT("spatial_content_profile_missing"), OutError, TEXT("registration"));
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
			TEXT("content_graph.items"),
		};
		return Values;
	}
}

#include "DomainAdapters/AnimationBlueprintCommands/UeAgentAnimationBlueprintCommandMechanicsBoundary.h"

#include "Dom/JsonValue.h"

namespace UE::UAI::DomainAdapters::AnimationBlueprintCommands
{
	using Composition::GraphCompound::FUeAgentGraphCompoundActivationProfile;

	const TArray<FString>& GetCommandIds()
	{
		static const TArray<FString> Ids = {
			TEXT("animation.anim_blueprint.asset.create"),
			TEXT("animation.anim_blueprint.graph.apply"),
			TEXT("animation.anim_blueprint.layer_interface.asset.create"),
		};
		return Ids;
	}

	const TArray<FString>& GetPublicCommandIds()
	{
		static const TArray<FString> Ids = {
			TEXT("animation.anim_blueprint.asset.create"),
			TEXT("animation.anim_blueprint.layer_interface.asset.create"),
		};
		return Ids;
	}

	bool IsQuery(const FString& CommandId)
	{
		return false;
	}

	bool TryGetActivationProfiles(
		TArray<FUeAgentGraphCompoundActivationProfile>& OutProfiles,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutProfiles.Reset();
		OutError.Reset();
		const TArray<Composition::GraphCompound::FUeAgentGraphCompoundActivationProfile>* GraphProfiles = nullptr;
		if (!Composition::GraphCompound::TryGetGraphCompoundActivationProfiles(GraphProfiles, OutError)) return false;
		const auto& Canonical = *GraphProfiles;
		for (const FString& CommandId : GetCommandIds())
		{
			const FUeAgentGraphCompoundActivationProfile* Match =
				Canonical.FindByPredicate(
					[&CommandId](const FUeAgentGraphCompoundActivationProfile& Profile)
					{
						return Profile.GetCommandId() == CommandId;
					});
			if (!Match || !Match->IsValid())
			{
				OutProfiles.Reset();
				if (Match) {
					OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("animation_blueprint_command_profile_missing"),
						Match->GetValidationDiagnostic(), TEXT("runtime_factory"),
						TEXT("A required Animation Blueprint activation profile failed validation."));
				} else {
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_blueprint_command_profile_missing"),
						TEXT("No activation profile is registered for a required Animation Blueprint command."), TEXT("runtime_factory"));
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


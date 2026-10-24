#include "DomainAdapters/MaterialCommands/UeAgentMaterialCommandMechanicsBoundary.h"

#include "Dom/JsonValue.h"

namespace UE::UAI::DomainAdapters::MaterialCommands
{
	using Composition::GraphCompound::FUeAgentGraphCompoundActivationProfile;

	const TArray<FString>& GetCommandIds()
	{
		static const TArray<FString> Ids = {
			TEXT("material.asset.create"),
			TEXT("material_instance.asset.create"),
			TEXT("material_instance.parameters.apply"),
		};
		return Ids;
	}

	const TArray<FString>& GetPublicCommandIds()
	{
		static const TArray<FString> Ids = {
			TEXT("material_instance.asset.create"),
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
					OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("material_command_profile_missing"),
						Match->GetValidationDiagnostic(), TEXT("runtime_factory"),
						TEXT("A required material activation profile failed validation."));
				} else {
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("material_command_profile_missing"),
						TEXT("No activation profile is registered for a required material command."), TEXT("runtime_factory"));
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



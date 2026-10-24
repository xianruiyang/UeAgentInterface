#include "DomainAdapters/PcgCommands/UeAgentPcgCommandMechanicsBoundary.h"

#include "Dom/JsonValue.h"

namespace UE::UAI::DomainAdapters::PcgCommands
{
	const TArray<FString>& GetCommandIds()
	{
		static const TArray<FString> Ids = {
			TEXT("pcg.component.apply"),
			TEXT("pcg.component.world_binding.apply"),
			TEXT("pcg.data_asset.apply"),
			TEXT("pcg.data_asset.create_from_level"),
			TEXT("pcg.graph.asset.create"),
			TEXT("pcg.graph.apply"),
			TEXT("pcg.graph_instance.apply"),
			TEXT("pcg.graph_parameters.apply"),
		};
		return Ids;
	}

	const TArray<FString>& GetPublicCommandIds()
	{
		static const TArray<FString> Ids = []
		{
			TArray<FString> Result = GetCommandIds();
			Result.Remove(TEXT("pcg.graph.apply"));
			Result.Remove(TEXT("pcg.graph_parameters.apply"));
			return Result;
		}();
		return Ids;
	}

	bool IsCommand(const FString& CommandId)
	{
		return GetCommandIds().Contains(CommandId);
	}

	bool TryGetGraphProfiles(
		TArray<Composition::GraphCompound::FUeAgentGraphCompoundActivationProfile>&
			OutProfiles,
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
				[&CommandId](const auto& Profile)
				{
					return Profile.GetCommandId() == CommandId;
				});
			if (!Match || !Match->IsValid())
			{
				OutProfiles.Reset();
				if (Match) {
					OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("pcg_command_graph_profile_missing"),
						Match->GetValidationDiagnostic(), TEXT("runtime_factory"),
						TEXT("A required PCG activation profile failed validation."));
				} else {
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("pcg_command_graph_profile_missing"),
						TEXT("No activation profile is registered for a required PCG command."), TEXT("runtime_factory"));
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
		return true;
	}


}

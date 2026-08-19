#include "Module/CommandRegistration/HjsonExternalInput/UeAgentHjsonExternalInputRegistration.h"

#include "CommandHandlers/HjsonExternalInput/UeAgentHjsonExternalInputCommandHandlers.h"

namespace UE::UAI::Module::CommandRegistration::HjsonExternalInput
{
	const FString& GetHjsonExternalInputProviderContributionId()
	{
		static const FString Id(TEXT("hjson-external-input-domain-provider"));
		return Id;
	}

	bool ValidateHjsonExternalInputContribution(FString& OutError)
	{
		OutError.Reset();
		const TArray<FString> Commands = CommandHandlers::HjsonExternalInput::
			GetHjsonExternalInputDelegatedCommandIds();
		if (Commands.Num() != 13)
		{
			OutError = TEXT("hjson_external_input_importer_disposition_incomplete");
			return false;
		}
		for (int32 Index = 1; Index < Commands.Num(); ++Index)
		{
			if (Commands[Index - 1] == Commands[Index])
			{
				OutError = TEXT("hjson_external_input_importer_disposition_duplicate");
				return false;
			}
		}
		return true;
	}
}

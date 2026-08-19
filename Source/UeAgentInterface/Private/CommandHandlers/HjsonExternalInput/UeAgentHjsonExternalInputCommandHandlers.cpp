#include "CommandHandlers/HjsonExternalInput/UeAgentHjsonExternalInputCommandHandlers.h"

#include "DomainAdapters/HjsonExternalInput/UeAgentHjsonExternalInputDomain.h"

namespace UE::UAI::CommandHandlers::HjsonExternalInput
{
	TArray<FString> GetHjsonExternalInputDelegatedCommandIds()
	{
		TArray<FString> Result;
		for (const DomainAdapters::HjsonExternalInput::
			FHjsonExternalInputFamilyContract& Contract
			: DomainAdapters::HjsonExternalInput::
				GetHjsonExternalInputFamilyContracts())
		{
			Result.Append(Contract.ImporterCommandIds);
		}
		Result.Sort();
		return Result;
	}
}

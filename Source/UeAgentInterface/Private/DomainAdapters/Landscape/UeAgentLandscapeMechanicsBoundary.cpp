#include "DomainAdapters/Landscape/UeAgentLandscapeMechanicsBoundary.h"

namespace UE::UAI::DomainAdapters::Landscape
{
	const TArray<FString>& GetCommandIds()
	{
		static const TArray<FString> Ids = {
			TEXT("landscape.create"),
			TEXT("landscape.height_delta"),
		};
		return Ids;
	}

	bool IsCommand(const FString& CommandId)
	{
		return GetCommandIds().Contains(CommandId);
	}

	const TArray<FString>& GetRequiredAuthorityScopes()
	{
		static const TArray<FString> Scopes = {
			TEXT("uai:landscape:write"),
		};
		return Scopes;
	}
}

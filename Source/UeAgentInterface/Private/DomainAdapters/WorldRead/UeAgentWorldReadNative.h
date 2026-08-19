#pragma once

#include "CoreMinimal.h"
#include "DomainAdapters/WorldRead/UeAgentWorldReadProvider.h"
#include "Templates/SharedPointer.h"

class UObject;
class UWorld;

namespace UE::UAI::DomainAdapters::WorldRead
{
	class UEAGENTINTERFACE_API IUeAgentWorldReadEnvironment
	{
	public:
		virtual ~IUeAgentWorldReadEnvironment() = default;

		virtual bool TryResolveWorld(
			const UE::UAI::ContextResource::
				FUeAgentExecutionContextRef& Context,
			UWorld*& OutWorld,
			FUeAgentWorldReadError& OutError) = 0;

		virtual bool TryResolveTarget(
			const UE::UAI::Discovery::FUeAgentCandidateBinding& Target,
			UObject*& OutObject,
			FUeAgentWorldReadError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API
		FUeAgentWorldReadNativeHookFactory final
	{
	public:
		static bool TryCreate(
			const FString& ProviderBinding,
			TSharedRef<IUeAgentWorldReadEnvironment> Environment,
			TSharedPtr<IUeAgentWorldReadHook>& OutHook,
			FUeAgentWorldReadError& OutError);
	};
}

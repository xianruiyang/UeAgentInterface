#pragma once

#include "CoreMinimal.h"
#include "DomainAdapters/WorldRead/UeAgentWorldReadNative.h"

namespace UE::UAI::DomainAdapters::WorldRead
{
	class UEAGENTINTERFACE_API
		FUeAgentWorldReadProductionEnvironment final
		: public IUeAgentWorldReadEnvironment
	{
	public:
		virtual bool TryResolveWorld(
			const UE::UAI::ContextResource::
				FUeAgentExecutionContextRef& Context,
			UWorld*& OutWorld,
			FUeAgentWorldReadError& OutError) override;

		virtual bool TryResolveTarget(
			const UE::UAI::Discovery::FUeAgentCandidateBinding& Target,
			UObject*& OutObject,
			FUeAgentWorldReadError& OutError) override;
	};
}

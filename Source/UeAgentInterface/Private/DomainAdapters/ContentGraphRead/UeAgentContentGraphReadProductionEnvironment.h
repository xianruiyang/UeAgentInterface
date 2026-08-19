#pragma once

#include "CoreMinimal.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadNative.h"

namespace UE::UAI::DomainAdapters::ContentGraphRead
{
	class UEAGENTINTERFACE_API
		FUeAgentContentGraphReadProductionEnvironment final
		: public IUeAgentContentGraphReadEnvironment
	{
	public:
		static bool TryCaptureOwner(
			UObject& Owner,
			FUeAgentContentGraphCapture& OutCapture,
			FUeAgentContentGraphReadError& OutError);

		virtual bool TryCapture(
			const UE::UAI::Discovery::FUeAgentCandidateBinding& Target,
			const UE::UAI::ContextResource::
				FUeAgentExecutionContextRef& Context,
			FUeAgentContentGraphCapture& OutCapture,
			FUeAgentContentGraphReadError& OutError) override;
	};
}

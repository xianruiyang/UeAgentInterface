#pragma once

#include "CoreMinimal.h"
#include "Core/ContextResource/UeAgentContextResource.h"

namespace UE::UAI::BindingGateway::Query
{
	struct UEAGENTINTERFACE_API FUeAgentQueryBindingError
	{
		FString Code;
		FString SubjectId;
		FString Detail;
		UE::UAI::ContextResource::EUeAgentResourceDimension
			ResourceDimension =
				UE::UAI::ContextResource::
					EUeAgentResourceDimension::WorkUnits;
		int64 ResourceLimit = 0;
		int64 ResourceObserved = 0;

		bool IsValid() const
		{
			return !Code.IsEmpty();
		}
	};
}

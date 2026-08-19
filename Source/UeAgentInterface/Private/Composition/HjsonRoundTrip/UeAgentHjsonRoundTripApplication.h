#pragma once

#include "CoreMinimal.h"

namespace UE::UAI::Composition::HjsonRoundTrip
{
	UEAGENTINTERFACE_API const FString& GetHjsonImportCapabilityId();
	UEAGENTINTERFACE_API
	const FString& GetHjsonImportRequiredAuthorityScope();
	UEAGENTINTERFACE_API bool IsSupportedHjsonImportSourceKind(
		const FString& SourceKind);
}

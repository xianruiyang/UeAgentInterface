#include "Composition/HjsonRoundTrip/UeAgentHjsonRoundTripApplication.h"

namespace UE::UAI::Composition::HjsonRoundTrip
{
	const FString& GetHjsonImportCapabilityId()
	{
		static const FString CapabilityId(TEXT("uai.hjson.import"));
		return CapabilityId;
	}

	const FString& GetHjsonImportRequiredAuthorityScope()
	{
		static const FString AuthorityScope(
			TEXT("uai.authoring.hjson.import"));
		return AuthorityScope;
	}

	bool IsSupportedHjsonImportSourceKind(const FString& SourceKind)
	{
		return SourceKind == TEXT("directory")
			|| SourceKind == TEXT("single_file")
			|| SourceKind == TEXT("inline");
	}
}

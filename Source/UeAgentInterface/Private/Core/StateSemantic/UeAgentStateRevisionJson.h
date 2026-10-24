#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Dom/JsonObject.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::StateSemantic
{
	class UEAGENTINTERFACE_API FUeAgentStateRevisionJson final
	{
	public:
		static constexpr int64 MaxExactJsonSequence =
			9007199254740991LL;

		static bool TryParse(
			const TSharedPtr<FJsonObject>& Object,
			TOptional<FUeAgentStateRevision>& OutRevision,
			Contract::FDiagnostic& OutError);

		static bool TryReadCanonical(
			const FUeAgentCanonicalValuePtr& Value,
			TOptional<FUeAgentStateRevision>& OutRevision,
			Contract::FDiagnostic& OutError);

		static TSharedRef<FJsonObject> ToJson(
			const FUeAgentStateRevision& Revision);
	};
}

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class FJsonObject;

namespace UE::UAI::QueryOwners::Pcg
{
	UEAGENTINTERFACE_API bool IsReadOnlyContentGraphCommand(const FString& CommandId);

	bool IsCurrentSpatialQuery(const FString& CommandId);
	bool TryReadCurrentSpatialState(
		const FString& CommandId,
		const TSharedPtr<FJsonObject>& Params,
		TSharedPtr<FJsonObject>& OutPayload,
		UE::UAI::Contract::FDiagnostic& OutDiagnostic);
}

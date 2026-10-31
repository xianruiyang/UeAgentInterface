#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;
namespace UE::UAI::Contract { struct FDiagnostic; }
namespace UE::UAI::CommandContract { class FUeAgentFrozenCommandContract; }

namespace UE::UAI::CommandEdge
{
bool IsCommandWireValue(const FString& WireType, bool bNullable, const TSharedPtr<FJsonValue>& Value);
bool TryValidateCommandParameters(const CommandContract::FUeAgentFrozenCommandContract& Command,
    const TSharedPtr<FJsonObject>& Params, Contract::FDiagnostic& Error);
}

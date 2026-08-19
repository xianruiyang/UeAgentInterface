#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/Command/UeAgentCommandContractModel.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::CommandRegistry
{
	class UEAGENTINTERFACE_API FUeAgentImmutableCommandRegistry final
	{
	public:
		static bool TryFreeze(
			TArray<CommandContract::FUeAgentCommandDeclaration>
				Declarations,
			TSharedPtr<const FUeAgentImmutableCommandRegistry>&
				OutRegistry,
			FString& OutError);

		const TArray<CommandContract::FUeAgentFrozenCommandContract>&
		GetContracts() const;
		const CommandContract::FUeAgentFrozenCommandContract*
		FindExact(const FString& CommandId) const;
		const FString& GetRegistryHash() const;

		bool BuildProjection(
			FString& OutProjection,
			FString& OutError) const;

	private:
		FUeAgentImmutableCommandRegistry(
			TArray<CommandContract::FUeAgentFrozenCommandContract>
				InContracts,
			FString InRegistryHash);

		TArray<CommandContract::FUeAgentFrozenCommandContract>
			Contracts;
		TMap<FString, int32> IndexByCommandId;
		FString RegistryHash;
	};
}

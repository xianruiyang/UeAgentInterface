#pragma once

#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Core/Contract/Command/UeAgentCommandContractModel.h"

namespace UE::UAI::Module::CommandRegistration::Core
{
	class UEAGENTINTERFACE_API FUeAgentCoreCommandRegistration final
	{
	public:
		static bool TryCompose(
			TArray<CommandContract::FUeAgentCommandDeclaration>
				Declarations,
			TArray<TSharedRef<CommandEdge::IUeAgentExactCommandHandler>>
				Handlers,
			TSharedPtr<
				const CommandRegistry::FUeAgentImmutableCommandRegistry>&
				OutRegistry,
			TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch>&
				OutDispatch,
			FString& OutError);
	};
}

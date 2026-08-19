#include "Module/CommandRegistration/Core/UeAgentCoreCommandRegistration.h"

namespace UE::UAI::Module::CommandRegistration::Core
{
	using CommandEdge::FUeAgentExactCommandDispatch;
	using CommandRegistry::FUeAgentImmutableCommandRegistry;

	bool FUeAgentCoreCommandRegistration::TryCompose(
		TArray<CommandContract::FUeAgentCommandDeclaration> Declarations,
		TArray<TSharedRef<CommandEdge::IUeAgentExactCommandHandler>>
			Handlers,
		TSharedPtr<const FUeAgentImmutableCommandRegistry>& OutRegistry,
		TSharedPtr<const FUeAgentExactCommandDispatch>& OutDispatch,
		FString& OutError)
	{
		OutRegistry.Reset();
		OutDispatch.Reset();
		OutError.Reset();
		if (!FUeAgentImmutableCommandRegistry::TryFreeze(
			MoveTemp(Declarations),
			OutRegistry,
			OutError))
		{
			return false;
		}
		if (!FUeAgentExactCommandDispatch::TryCreate(
			OutRegistry.ToSharedRef(),
			MoveTemp(Handlers),
			OutDispatch,
			OutError))
		{
			OutRegistry.Reset();
			return false;
		}
		return true;
	}
}

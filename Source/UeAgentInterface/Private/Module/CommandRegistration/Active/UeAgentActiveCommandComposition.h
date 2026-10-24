#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Core/Contract/Command/UeAgentCommandContractModel.h"
#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::Module::CommandRegistration::Active
{
	class UEAGENTINTERFACE_API IUeAgentActiveCommandContribution
	{
	public:
		virtual ~IUeAgentActiveCommandContribution() = default;

		virtual const FString& GetContributionId() const = 0;
		virtual void CollectDeclarations(
			TArray<CommandContract::FUeAgentCommandDeclaration>&
				OutDeclarations) const = 0;
		virtual bool CreateHandlers(
			TSharedRef<
				const CommandRegistry::FUeAgentImmutableCommandRegistry>
				Registry,
			TArray<TSharedRef<CommandEdge::IUeAgentExactCommandHandler>>&
				OutHandlers,
			UE::UAI::Contract::FDiagnostic& OutError) const = 0;
	};

	using FUeAgentActiveCommandContributionFactory = bool (*)(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError);

	class UEAGENTINTERFACE_API
		FUeAgentActiveCommandContributionFactoryRegistration final
	{
	public:
		FUeAgentActiveCommandContributionFactoryRegistration(
			const TCHAR* ContributionId,
			FUeAgentActiveCommandContributionFactory Factory);
		FUeAgentActiveCommandContributionFactoryRegistration(
			const TCHAR* DomainId,
			const TCHAR* ContributionId,
			FUeAgentActiveCommandContributionFactory Factory);
	};

	class UEAGENTINTERFACE_API FUeAgentActiveCommandComposition final
	{
	public:
		bool TryAdd(
			TSharedRef<IUeAgentActiveCommandContribution> Contribution,
			UE::UAI::Contract::FDiagnostic& OutError);

		bool TryCompose(
			TSharedPtr<
				const CommandRegistry::FUeAgentImmutableCommandRegistry>&
				OutRegistry,
			TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch>&
				OutDispatch,
			UE::UAI::Contract::FDiagnostic& OutError);

		bool IsSealed() const;

	private:
		bool bSealed = false;
		TSet<FString> ContributionIds;
		TArray<TSharedRef<IUeAgentActiveCommandContribution>>
			Contributions;
	};

	UEAGENTINTERFACE_API bool TryComposeProcessActiveCommandComposition(
		TSharedPtr<
			const CommandRegistry::FUeAgentImmutableCommandRegistry>&
			OutRegistry,
		TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch>&
			OutDispatch,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool TryComposeActiveCommandContributions(
		const TArray<TSharedRef<IUeAgentActiveCommandContribution>>&
			Contributions,
		TSharedPtr<
			const CommandRegistry::FUeAgentImmutableCommandRegistry>&
			OutRegistry,
		TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch>&
			OutDispatch,
		UE::UAI::Contract::FDiagnostic& OutError);

}

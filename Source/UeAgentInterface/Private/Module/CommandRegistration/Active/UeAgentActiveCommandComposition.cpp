#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

#include "Core/Contract/Command/UeAgentCommandLifecyclePolicy.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"

namespace UE::UAI::Module::CommandRegistration::Active
{
	using CommandEdge::FUeAgentExactCommandDispatch;
	using CommandEdge::IUeAgentExactCommandHandler;
	using CommandRegistry::FUeAgentImmutableCommandRegistry;

	namespace
	{
		void AssignCanonicalPythonBinding(
			CommandContract::FUeAgentCommandDeclaration& Declaration)
		{
			const int32 NamespaceSeparator =
				Declaration.CommandId.Find(TEXT("."));
			if (NamespaceSeparator == INDEX_NONE)
			{
				Declaration.PythonNamespace.Reset();
				Declaration.PythonFunction.Reset();
				return;
			}
			Declaration.PythonNamespace =
				Declaration.CommandId.Left(NamespaceSeparator);
			Declaration.PythonFunction =
				Declaration.CommandId.Mid(NamespaceSeparator + 1);
			Declaration.PythonFunction.ReplaceInline(
				TEXT("."),
				TEXT("_"));
		}

	}

	FUeAgentActiveCommandContributionFactoryRegistration::
		FUeAgentActiveCommandContributionFactoryRegistration(
			const TCHAR* ContributionId,
			const FUeAgentActiveCommandContributionFactory Factory)
		: FUeAgentActiveCommandContributionFactoryRegistration(
			ContributionId,
			ContributionId,
			Factory)
	{
	}

	FUeAgentActiveCommandContributionFactoryRegistration::
		FUeAgentActiveCommandContributionFactoryRegistration(
			const TCHAR* DomainId,
			const TCHAR* ContributionId,
			const FUeAgentActiveCommandContributionFactory Factory)
	{
		DomainCapability::RegisterProcessDomainCommandCapabilityFactory(
			DomainId ? FString(DomainId) : FString(),
			ContributionId ? FString(ContributionId) : FString(),
			Factory);
	}

	bool FUeAgentActiveCommandComposition::TryAdd(
		TSharedRef<IUeAgentActiveCommandContribution> Contribution,
		FString& OutError)
	{
		OutError.Reset();
		const FString& ContributionId = Contribution->GetContributionId();
		if (bSealed)
		{
			OutError = TEXT("active_command_composition_sealed:")
				+ ContributionId;
			return false;
		}
		FString NormalizedId = ContributionId;
		NormalizedId.TrimStartAndEndInline();
		if (NormalizedId.IsEmpty() || NormalizedId != ContributionId)
		{
			OutError = TEXT("active_command_contribution_id_invalid:")
				+ ContributionId;
			return false;
		}
		if (ContributionIds.Contains(ContributionId))
		{
			OutError = TEXT("active_command_contribution_duplicate:")
				+ ContributionId;
			return false;
		}
		ContributionIds.Add(ContributionId);
		Contributions.Add(Contribution);
		return true;
	}

	bool FUeAgentActiveCommandComposition::TryCompose(
		TSharedPtr<const FUeAgentImmutableCommandRegistry>& OutRegistry,
		TSharedPtr<const FUeAgentExactCommandDispatch>& OutDispatch,
		FString& OutError)
	{
		OutRegistry.Reset();
		OutDispatch.Reset();
		OutError.Reset();
		if (bSealed)
		{
			OutError = TEXT("active_command_composition_sealed:compose");
			return false;
		}
		bSealed = true;
		if (Contributions.IsEmpty())
		{
			OutError = TEXT("active_command_contribution_empty");
			return false;
		}

		TArray<TSharedRef<IUeAgentActiveCommandContribution>>
			SortedContributions = Contributions;
		SortedContributions.Sort(
			[](const TSharedRef<IUeAgentActiveCommandContribution>& Left,
			   const TSharedRef<IUeAgentActiveCommandContribution>& Right)
			{
				return Left->GetContributionId()
					< Right->GetContributionId();
			});

		TArray<CommandContract::FUeAgentCommandDeclaration> Declarations;
		for (const TSharedRef<IUeAgentActiveCommandContribution>&
			Contribution : SortedContributions)
		{
			Contribution->CollectDeclarations(Declarations);
		}
		for (CommandContract::FUeAgentCommandDeclaration& Declaration :
			Declarations)
		{
			AssignCanonicalPythonBinding(Declaration);
		}
		if (!CommandLifecycle::TryApply(Declarations, OutError))
		{
			return false;
		}

		TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
		if (!FUeAgentImmutableCommandRegistry::TryFreeze(
			MoveTemp(Declarations),
			Registry,
			OutError))
		{
			return false;
		}

		TArray<TSharedRef<IUeAgentExactCommandHandler>> Handlers;
		for (const TSharedRef<IUeAgentActiveCommandContribution>&
			Contribution : SortedContributions)
		{
			TArray<TSharedRef<IUeAgentExactCommandHandler>>
				ContributionHandlers;
			FString ContributionError;
			if (!Contribution->CreateHandlers(
				Registry.ToSharedRef(),
				ContributionHandlers,
				ContributionError))
			{
				OutError = TEXT("active_command_handler_creation_failed:")
					+ Contribution->GetContributionId()
					+ TEXT(":")
					+ ContributionError;
				return false;
			}
			Handlers.Append(ContributionHandlers);
		}

		TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
		if (!FUeAgentExactCommandDispatch::TryCreate(
			Registry.ToSharedRef(),
			MoveTemp(Handlers),
			Dispatch,
			OutError))
		{
			return false;
		}

		OutRegistry = MoveTemp(Registry);
		OutDispatch = MoveTemp(Dispatch);
		return true;
	}

	bool FUeAgentActiveCommandComposition::IsSealed() const
	{
		return bSealed;
	}

	bool TryComposeActiveCommandContributions(
		const TArray<TSharedRef<IUeAgentActiveCommandContribution>>&
			ExactContributions,
		TSharedPtr<const FUeAgentImmutableCommandRegistry>& OutRegistry,
		TSharedPtr<const FUeAgentExactCommandDispatch>& OutDispatch,
		FString& OutError)
	{
		OutRegistry.Reset();
		OutDispatch.Reset();
		OutError.Reset();

		FUeAgentActiveCommandComposition Composition;
		for (const TSharedRef<IUeAgentActiveCommandContribution>& Contribution :
			ExactContributions)
		{
			if (!Composition.TryAdd(Contribution, OutError))
			{
				return false;
			}
		}
		return Composition.TryCompose(
			OutRegistry,
			OutDispatch,
			OutError);
	}

	bool TryComposeProcessActiveCommandComposition(
		TSharedPtr<const FUeAgentImmutableCommandRegistry>& OutRegistry,
		TSharedPtr<const FUeAgentExactCommandDispatch>& OutDispatch,
		FString& OutError)
	{
		OutRegistry.Reset();
		OutDispatch.Reset();
		OutError.Reset();
		DomainCapability::FDomainCapabilityAuthorities Authorities;
		if (!DomainCapability::TryGetOrBuildProcessDomainCapabilityAuthorities(
				Authorities,
				OutError))
		{
			return false;
		}
		OutRegistry = Authorities.GetCommandRegistry();
		OutDispatch = Authorities.GetCommandDispatch();
		return OutRegistry.IsValid() && OutDispatch.IsValid();
	}
}

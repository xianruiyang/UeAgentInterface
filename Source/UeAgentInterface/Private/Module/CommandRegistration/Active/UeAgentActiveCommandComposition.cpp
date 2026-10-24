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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		const FString& ContributionId = Contribution->GetContributionId();
		if (bSealed)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("active_command_composition_sealed"), TEXT("Cannot add a contribution after command composition has been sealed."), TEXT("registration"), FString(), ContributionId);
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = ContributionId;
			return false;
		}
		FString NormalizedId = ContributionId;
		NormalizedId.TrimStartAndEndInline();
		if (NormalizedId.IsEmpty() || NormalizedId != ContributionId)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("active_command_contribution_id_invalid"), TEXT("A contribution ID must be non-empty and have no surrounding whitespace."), TEXT("registration"), FString(), ContributionId);
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = ContributionId;
			return false;
		}
		if (ContributionIds.Contains(ContributionId))
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("active_command_contribution_duplicate"), TEXT("The contribution ID is already registered."), TEXT("registration"), FString(), ContributionId);
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = ContributionId;
			return false;
		}
		ContributionIds.Add(ContributionId);
		Contributions.Add(Contribution);
		return true;
	}

	bool FUeAgentActiveCommandComposition::TryCompose(
		TSharedPtr<const FUeAgentImmutableCommandRegistry>& OutRegistry,
		TSharedPtr<const FUeAgentExactCommandDispatch>& OutDispatch,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutRegistry.Reset();
		OutDispatch.Reset();
		OutError.Reset();
		if (bSealed)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("active_command_composition_sealed"), TEXT("Command composition has already been sealed and cannot be composed again."), TEXT("registration"));
			return false;
		}
		bSealed = true;
		if (Contributions.IsEmpty())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("active_command_contribution_empty"), TEXT("No command contributions were registered before composition."), TEXT("registration"));
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
			UE::UAI::Contract::FDiagnostic ContributionError;
			if (!Contribution->CreateHandlers(
				Registry.ToSharedRef(),
				ContributionHandlers,
				ContributionError))
			{
				OutError = ContributionError.IsEmpty() ? UE::UAI::Contract::FDiagnostic::Failure(
					TEXT("active_command_handler_creation_failed"), TEXT("The command contributor failed to create its handlers without returning a diagnostic."),
					TEXT("command_composition")) : ContributionError;
                if (ContributionError.IsEmpty()) OutError.SourceKind = TEXT("native_result");
                if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Contribution->GetContributionId();
				return false;
			}
			ContributionHandlers.RemoveAll(
				[&Registry](
					const TSharedRef<IUeAgentExactCommandHandler>& Handler)
				{
					const CommandContract::FUeAgentFrozenCommandContract* Contract =
						Registry->FindExact(Handler->GetCommandId());
					return Contract
						&& !CommandLifecycle::IsExecutable(
							Contract->GetAvailability());
				});
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
		UE::UAI::Contract::FDiagnostic& OutError)
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
		UE::UAI::Contract::FDiagnostic& OutError)
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

#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"

namespace UE::UAI::Module::CommandRegistration::Active
{
	using CommandEdge::FUeAgentExactCommandDispatch;
	using CommandEdge::IUeAgentExactCommandHandler;
	using CommandRegistry::FUeAgentImmutableCommandRegistry;

	namespace
	{
		struct FProcessContributionFactoryEntry final
		{
			FString ContributionId;
			FUeAgentActiveCommandContributionFactory Factory = nullptr;
		};

		struct FProcessContributionCatalog final
		{
			FCriticalSection Mutex;
			bool bSealed = false;
			TArray<FProcessContributionFactoryEntry> Entries;
			TArray<FString> RegistrationErrors;
		};

		FProcessContributionCatalog& GetProcessContributionCatalog()
		{
			static FProcessContributionCatalog Catalog;
			return Catalog;
		}

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

		bool RegisterProcessContributionFactory(
			const FString& ContributionId,
			const FUeAgentActiveCommandContributionFactory Factory)
		{
			FProcessContributionCatalog& Catalog =
				GetProcessContributionCatalog();
			FScopeLock Lock(&Catalog.Mutex);
			if (Catalog.bSealed)
			{
				Catalog.RegistrationErrors.AddUnique(
					TEXT("active_command_composition_sealed:")
					+ ContributionId);
				return false;
			}
			if (ContributionId.IsEmpty() || !Factory)
			{
				Catalog.RegistrationErrors.AddUnique(
					TEXT("active_command_contribution_factory_invalid:")
					+ ContributionId);
				return false;
			}
			for (const FProcessContributionFactoryEntry& Entry :
				Catalog.Entries)
			{
				if (Entry.ContributionId == ContributionId)
				{
					Catalog.RegistrationErrors.AddUnique(
						TEXT("active_command_contribution_duplicate:")
						+ ContributionId);
					return false;
				}
			}
			Catalog.Entries.Add({ContributionId, Factory});
			return true;
		}
	}

	FUeAgentActiveCommandContributionFactoryRegistration::
		FUeAgentActiveCommandContributionFactoryRegistration(
			const TCHAR* ContributionId,
			const FUeAgentActiveCommandContributionFactory Factory)
	{
		RegisterProcessContributionFactory(
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

	bool TryComposeProcessActiveCommandComposition(
		const TArray<TSharedRef<IUeAgentActiveCommandContribution>>&
			DomainOverrides,
		TSharedPtr<const FUeAgentImmutableCommandRegistry>& OutRegistry,
		TSharedPtr<const FUeAgentExactCommandDispatch>& OutDispatch,
		FString& OutError)
	{
		OutRegistry.Reset();
		OutDispatch.Reset();
		OutError.Reset();

		TArray<FProcessContributionFactoryEntry> Entries;
		{
			FProcessContributionCatalog& Catalog =
				GetProcessContributionCatalog();
			FScopeLock Lock(&Catalog.Mutex);
			Catalog.bSealed = true;
			if (!Catalog.RegistrationErrors.IsEmpty())
			{
				OutError = Catalog.RegistrationErrors[0];
				return false;
			}
			Entries = Catalog.Entries;
		}

		Entries.Sort(
			[](const FProcessContributionFactoryEntry& Left,
			   const FProcessContributionFactoryEntry& Right)
			{
				return Left.ContributionId < Right.ContributionId;
			});

		TMap<FString, TSharedRef<IUeAgentActiveCommandContribution>>
			OverridesById;
		for (const TSharedRef<IUeAgentActiveCommandContribution>& Override :
			DomainOverrides)
		{
			const FString& OverrideId = Override->GetContributionId();
			FString NormalizedId = OverrideId;
			NormalizedId.TrimStartAndEndInline();
			if (NormalizedId.IsEmpty() || NormalizedId != OverrideId)
			{
				OutError = TEXT("active_command_override_identity_invalid:")
					+ OverrideId;
				return false;
			}
			if (OverridesById.Contains(OverrideId))
			{
				OutError = TEXT("active_command_override_duplicate:")
					+ OverrideId;
				return false;
			}
			OverridesById.Add(OverrideId, Override);
		}

		FUeAgentActiveCommandComposition Composition;
		for (const FProcessContributionFactoryEntry& Entry : Entries)
		{
			if (const TSharedRef<IUeAgentActiveCommandContribution>* Override =
				OverridesById.Find(Entry.ContributionId))
			{
				if (!Composition.TryAdd(*Override, OutError))
				{
					return false;
				}
				OverridesById.Remove(Entry.ContributionId);
				continue;
			}
			TSharedPtr<IUeAgentActiveCommandContribution> Contribution;
			FString FactoryError;
			if (!Entry.Factory(Contribution, FactoryError)
				|| !Contribution.IsValid())
			{
				OutError = TEXT("active_command_contribution_factory_failed:")
					+ Entry.ContributionId
					+ TEXT(":")
					+ FactoryError;
				return false;
			}
			if (Contribution->GetContributionId()
				!= Entry.ContributionId)
			{
				OutError =
					TEXT("active_command_contribution_identity_mismatch:")
					+ Entry.ContributionId
					+ TEXT(":")
					+ Contribution->GetContributionId();
				return false;
			}
			if (!Composition.TryAdd(
				Contribution.ToSharedRef(),
				OutError))
			{
				return false;
			}
		}
		TArray<FString> RemainingOverrideIds;
		OverridesById.GetKeys(RemainingOverrideIds);
		RemainingOverrideIds.Sort();
		for (const FString& OverrideId : RemainingOverrideIds)
		{
			if (!Composition.TryAdd(
				OverridesById.FindChecked(OverrideId),
				OutError))
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
		return TryComposeProcessActiveCommandComposition(
			{},
			OutRegistry,
			OutDispatch,
			OutError);
	}
}

#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentContract.h"
#include "Templates/SharedPointer.h"

class UWorld;

namespace UE::UAI::DomainAdapters::WorldDestructive
{
	struct UEAGENTINTERFACE_API FUeAgentWorldDestructiveRequest final
	{
		FString RequestId;
		FString CommandId;
		FString TargetRef;
		FString OperandRef;
		FString ManagedId;
		FString ComponentName;
		FString Space;
		FString CallerId;
		FVector BoundsMin = FVector::ZeroVector;
		FVector BoundsMax = FVector::ZeroVector;
		int32 Index = INDEX_NONE;
	};

	struct UEAGENTINTERFACE_API FUeAgentWorldDestructiveResult final
	{
		FString CommandId;
		FString TargetRef;
		FString ResultRef;
		FString TerminalState = TEXT("rolled_back");
		FString PersistenceScope;
		FString ErrorCode;
		FString ErrorDetail;
		FString JournalId;
		FString VerificationId;
		bool bChanged = false;
		bool bPersisted = false;
		bool bReadbackVerified = false;

		bool IsSuccess() const
		{
			return TerminalState == TEXT("committed")
				&& ErrorCode.IsEmpty()
				&& bReadbackVerified;
		}
	};

	/**
	 * Native mutation mechanics supplied by the domain adapter to the
	 * composition-owned durable runner. Persistence, fencing, journals and
	 * independent persisted readback do not belong to this interface.
	 */
	class UEAGENTINTERFACE_API IUeAgentWorldDestructiveDurableMutation
	{
	public:
		virtual ~IUeAgentWorldDestructiveDurableMutation() = default;

		/** Primary Map package required to reconstruct an isolated world. */
		virtual const FString& GetPackageName() const = 0;
		/** Package that actually carries the native mutation. */
		virtual const FString& GetMutationPackageName() const = 0;
		virtual const FString& GetPersistenceScope() const = 0;
		virtual bool Apply(FString& OutError) = 0;
		virtual bool Rollback(FString& OutError) = 0;
		virtual bool VerifyPersisted(
			UWorld& PersistedWorld,
			FString& OutError) const = 0;
	};

	/**
	 * Composition port that owns the durable lifecycle around one native
	 * WorldDestructive mutation.
	 */
	class UEAGENTINTERFACE_API IUeAgentWorldDestructiveDurableRunner
	{
	public:
		virtual ~IUeAgentWorldDestructiveDurableRunner() = default;

		virtual FUeAgentWorldDestructiveResult Execute(
			UWorld& ExplicitWorld,
			const FUeAgentWorldDestructiveRequest& Request,
			TSharedRef<IUeAgentWorldDestructiveDurableMutation> Mutation,
			const TSharedRef<Contract::IUeAgentCancellationSignal>&
				Cancellation) = 0;
	};

	class UEAGENTINTERFACE_API IUeAgentWorldDestructiveOwner
	{
	public:
		virtual ~IUeAgentWorldDestructiveOwner() = default;

		virtual FUeAgentWorldDestructiveResult Execute(
			const FUeAgentWorldDestructiveRequest& Request,
			const TSharedRef<Contract::IUeAgentCancellationSignal>&
				Cancellation) = 0;
	};

	UEAGENTINTERFACE_API bool TryCreateWorldDestructiveOwner(
		UWorld& ExplicitWorld,
		FString JournalRoot,
		FString JournalOwnerId,
		TSharedRef<IUeAgentWorldDestructiveDurableRunner> DurableRunner,
		TSharedPtr<IUeAgentWorldDestructiveOwner>& OutOwner,
		FString& OutError);
}

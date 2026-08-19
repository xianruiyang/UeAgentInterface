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
		TSharedPtr<IUeAgentWorldDestructiveOwner>& OutOwner,
		FString& OutError);
}

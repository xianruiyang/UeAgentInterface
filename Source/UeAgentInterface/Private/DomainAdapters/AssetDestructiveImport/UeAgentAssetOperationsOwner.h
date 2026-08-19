#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentContract.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::DomainAdapters::AssetDestructiveImport
{
	struct UEAGENTINTERFACE_API FUeAgentAssetOperationRequest final
	{
		FString RequestId;
		FString CommandId;
		FString TargetRef;
		FString Operand;
		FString CallerId;
	};

	struct UEAGENTINTERFACE_API FUeAgentAssetOperationResult final
	{
		FString CommandId;
		FString TargetRef;
		FString ResultRef;
		FString TerminalState = TEXT("rolled_back");
		FString ErrorCode;
		FString ErrorDetail;
		FString JournalId;
		FString VerificationId;
		bool bChanged = false;
		bool bReadbackVerified = false;

		bool IsSuccess() const
		{
			return TerminalState == TEXT("committed")
				&& ErrorCode.IsEmpty()
				&& bReadbackVerified;
		}
	};

	class UEAGENTINTERFACE_API IUeAgentAssetOperationsOwner
	{
	public:
		virtual ~IUeAgentAssetOperationsOwner() = default;

		virtual FUeAgentAssetOperationResult Execute(
			const FUeAgentAssetOperationRequest& Request,
			const TSharedRef<Contract::IUeAgentCancellationSignal>&
				Cancellation) = 0;
	};

	UEAGENTINTERFACE_API bool TryCreateAssetOperationsOwner(
		FString JournalRoot,
		FString JournalOwnerId,
		TSharedPtr<IUeAgentAssetOperationsOwner>& OutOwner,
		FString& OutError);
}

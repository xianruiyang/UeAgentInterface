#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentContract.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::DomainAdapters::AssetDestructiveImport
{
	struct UEAGENTINTERFACE_API FUeAgentAssetDeleteRequest final
	{
		FString RequestId;
		FString TargetRef;
		FString CallerId;
	};

	struct UEAGENTINTERFACE_API FUeAgentAssetDeleteResult final
	{
		FString TargetRef;
		FString PackageName;
		FString TerminalState = TEXT("rolled_back");
		FString ErrorCode;
		FString ErrorDetail;
		FString JournalId;
		FString VerificationId;
		bool bDeleted = false;
		bool bFileAbsent = false;
		bool bPackageAbsent = false;
		bool bRegistryAbsent = false;
		bool bObjectAbsent = false;

		bool IsSuccess() const
		{
			return bDeleted
				&& TerminalState == TEXT("committed")
				&& ErrorCode.IsEmpty();
		}
	};

	class UEAGENTINTERFACE_API IUeAgentAssetDeleteOwner
	{
	public:
		virtual ~IUeAgentAssetDeleteOwner() = default;

		virtual FUeAgentAssetDeleteResult Delete(
			const FUeAgentAssetDeleteRequest& Request,
			const TSharedRef<Contract::IUeAgentCancellationSignal>&
				Cancellation) = 0;
	};

	UEAGENTINTERFACE_API bool TryCreateAssetDeleteOwner(
		FString JournalRoot,
		FString JournalOwnerId,
		TSharedPtr<IUeAgentAssetDeleteOwner>& OutOwner,
		FString& OutError);
}

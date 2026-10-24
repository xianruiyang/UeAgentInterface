#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentContract.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::DomainAdapters::AssetDestructiveImport
{
	struct UEAGENTINTERFACE_API FUeAgentAssetDeleteRequest final
	{
		FString RequestId;
		TArray<FString> TargetRefs;
		bool bRecursive = false;
		FString CallerId;
	};

	struct UEAGENTINTERFACE_API FUeAgentAssetDeleteTargetResult final
	{
		FString TargetRef;
		FString PackageName;
		FString ObjectPath;
		FString Status = TEXT("remaining");
		FString TerminalState = TEXT("rolled_back");
		UE::UAI::Contract::FDiagnostic Diagnostic;
		FString JournalId;
		FString VerificationId;
		bool bDeleted = false;
		bool bRemaining = true;
		bool bFileAbsent = false;
		bool bPackageAbsent = false;
		bool bPackageEmpty = false;
		bool bRegistryAbsent = false;
		bool bObjectAbsent = false;
		bool bObjectRetired = false;

		bool IsAbsenceVerified() const
		{
			return bFileAbsent && (bPackageAbsent || bPackageEmpty)
				&& bRegistryAbsent && (bObjectAbsent || bObjectRetired);
		}

		bool IsSuccess() const
		{
			return bDeleted
				&& !bRemaining
				&& TerminalState == TEXT("committed")
				&& Diagnostic.IsEmpty();
		}
	};

	struct UEAGENTINTERFACE_API FUeAgentAssetDeleteResult final
	{
		TArray<FString> RequestedTargets;
		bool bRecursive = false;
		FString TerminalState = TEXT("rolled_back");
		TArray<FUeAgentAssetDeleteTargetResult> Targets;
		UE::UAI::Contract::FDiagnostic Diagnostic;

		bool IsSuccess() const
		{
			if (Targets.IsEmpty() || TerminalState != TEXT("committed"))
			{
				return false;
			}
			for (const FUeAgentAssetDeleteTargetResult& Target : Targets)
			{
				if (!Target.IsSuccess())
				{
					return false;
				}
			}
			return true;
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
		UE::UAI::Contract::FDiagnostic& OutError);
}

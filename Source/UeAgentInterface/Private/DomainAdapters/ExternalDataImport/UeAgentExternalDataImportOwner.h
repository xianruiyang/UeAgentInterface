#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentContract.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::DomainAdapters::ExternalDataImport
{
	struct UEAGENTINTERFACE_API FUeAgentExternalDataImportRequest final
	{
		FString RequestId;
		FString CommandId;
		FString SourceRef;
		FString DestinationRef;
		FString TargetRef;
		FString SkeletonRef;
		FString ProfileName;
		FString CallerId;
		int32 LodIndex = INDEX_NONE;
	};

	struct UEAGENTINTERFACE_API FUeAgentExternalDataImportResult final
	{
		FString CommandId;
		FString SourceRef;
		FString SourceSha256;
		FString ResultRef;
		FString ClassPath;
		FString TerminalState = TEXT("rolled_back");
		FString PersistenceScope;
		UE::UAI::Contract::FDiagnostic Diagnostic;
		FString JournalId;
		FString VerificationId;
		bool bChanged = false;
		bool bPersisted = false;
		bool bReadbackVerified = false;

		bool IsSuccess() const
		{
			return TerminalState == TEXT("committed")
				&& Diagnostic.IsEmpty()
				&& bPersisted
				&& bReadbackVerified;
		}
	};

	class UEAGENTINTERFACE_API IUeAgentExternalDataImportOwner
	{
	public:
		virtual ~IUeAgentExternalDataImportOwner() = default;

		virtual FUeAgentExternalDataImportResult Execute(
			const FUeAgentExternalDataImportRequest& Request,
			const TSharedRef<Contract::IUeAgentCancellationSignal>&
				Cancellation) = 0;
	};

	UEAGENTINTERFACE_API bool TryCreateExternalDataImportOwner(
		FString JournalRoot,
		FString JournalOwnerId,
		TSharedPtr<IUeAgentExternalDataImportOwner>& OutOwner,
		UE::UAI::Contract::FDiagnostic& OutError);
}

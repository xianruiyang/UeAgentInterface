#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::DomainAdapters::ExternalImport::Localization
{
	struct UEAGENTINTERFACE_API FLocalizationImportSpec final
	{
		FString RequestId;
		FString SourceRef;
		FString TargetRef;

		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FLocalizationImportReadback final
	{
		FString TargetRef;
		FString TargetIdentity;
		FString Culture;
		FString SourceRef;
		FString SourceSha256;
		FString PersistedPoPath;
		FString PersistedPoSha256;
		FString ArchivePath;
		FString ArchiveSha256;
		FString LocResPath;
		FString LocResSha256;
		FString CollapseMode;
		FString PoFormat;
		int32 RefreshRevisionBefore = 0;
		int32 RefreshRevisionAfter = 0;
		bool bTargetResolved = false;
		bool bFormalImporterCompleted = false;
		bool bFormalCompilerCompleted = false;
		bool bRefreshCompleted = false;
	};

	using FLocalizationImportError = UE::UAI::Contract::FDiagnostic;

	class UEAGENTINTERFACE_API ILocalizationImportSession
	{
	public:
		virtual ~ILocalizationImportSession() = default;

		virtual const FString& GetTargetIdentity() const = 0;
		virtual const FString& GetExpectedRevision() const = 0;
		virtual bool CheckPreExecute(FLocalizationImportError& OutError) = 0;
		virtual bool Execute(FLocalizationImportError& OutError) = 0;
		virtual bool DidMutationStart() const = 0;
		virtual bool Settle(FLocalizationImportError& OutError) = 0;
		virtual bool CheckPrePersist(FLocalizationImportError& OutError) = 0;
		virtual bool Persist(
			FString& OutPersistenceReceiptId,
			FLocalizationImportError& OutError) = 0;
		virtual bool Verify(
			FLocalizationImportReadback& OutReadback,
			FLocalizationImportError& OutError) = 0;
		virtual bool Rollback(
			bool& bOutConfirmed,
			FLocalizationImportError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API ILocalizationImportProvider
	{
	public:
		virtual ~ILocalizationImportProvider() = default;

		virtual bool Prepare(
			const FLocalizationImportSpec& Spec,
			TSharedPtr<ILocalizationImportSession>& OutSession,
			FLocalizationImportError& OutError) = 0;
	};

	UEAGENTINTERFACE_API bool TryCreateLocalizationImportProvider(
		TSharedPtr<ILocalizationImportProvider>& OutProvider,
		UE::UAI::Contract::FDiagnostic& OutError);
}

#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::DomainAdapters::ExternalImport::SkinWeight
{
	struct UEAGENTINTERFACE_API FSkinWeightImportSpec final
	{
		FString RequestId;
		FString SourceRef;
		FString TargetRef;
		FString ProfileName;
		int32 LodIndex = INDEX_NONE;

		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FSkinWeightImportReadback final
	{
		FString TargetIdentity;
		FString AssetClass;
		FString ProfileName;
		FString SourceRef;
		FString SourceSha256;
		int32 LodIndex = INDEX_NONE;
		int32 ProfileCount = 0;
		bool bReloaded = false;
	};

	struct UEAGENTINTERFACE_API FSkinWeightImportError final
	{
		FString Code;
		FString SubjectId;
		FString Detail;

		bool IsPresent() const { return !Code.IsEmpty(); }
	};

	class UEAGENTINTERFACE_API ISkinWeightImportSession
	{
	public:
		virtual ~ISkinWeightImportSession() = default;

		virtual const FString& GetTargetIdentity() const = 0;
		virtual const FString& GetExpectedRevision() const = 0;
		virtual bool CheckPreExecute(FSkinWeightImportError& OutError) = 0;
		virtual bool Execute(FSkinWeightImportError& OutError) = 0;
		virtual bool DidMutationStart() const = 0;
		virtual bool Settle(FSkinWeightImportError& OutError) = 0;
		virtual bool CheckPrePersist(FSkinWeightImportError& OutError) = 0;
		virtual bool Persist(
			FString& OutPersistenceReceiptId,
			FSkinWeightImportError& OutError) = 0;
		virtual bool Verify(
			FSkinWeightImportReadback& OutReadback,
			FSkinWeightImportError& OutError) = 0;
		virtual bool Rollback(
			bool& bOutConfirmed,
			FSkinWeightImportError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API ISkinWeightImportProvider
	{
	public:
		virtual ~ISkinWeightImportProvider() = default;

		virtual bool Prepare(
			const FSkinWeightImportSpec& Spec,
			TSharedPtr<ISkinWeightImportSession>& OutSession,
			FSkinWeightImportError& OutError) = 0;
	};

	UEAGENTINTERFACE_API bool TryCreateSkinWeightImportProvider(
		TSharedPtr<ISkinWeightImportProvider>& OutProvider,
		FString& OutError);
}

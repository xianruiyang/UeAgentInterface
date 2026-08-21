#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::ContentDiscovery
{
	enum class EContentDiscoveryMatchMode : uint8
	{
		Exact,
		Prefix,
		Text,
		Fuzzy,
	};

	struct UEAGENTINTERFACE_API FContentDiscoveryCursorEntry final
	{
		FString ProviderId;
		FString ExpectedRevision;
		int32 Offset = 0;

		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FContentDiscoveryCursor final
	{
		FString RequestFingerprint;
		TArray<FContentDiscoveryCursorEntry> Providers;

		bool IsEmpty() const;
		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FContentDiscoveryRequest final
	{
		FString Query;
		EContentDiscoveryMatchMode MatchMode =
			EContentDiscoveryMatchMode::Text;
		TArray<FString> ContentKinds;
		FString ClassPath;
		FString PathPrefix;
		FString OwnerIdentity;
		TArray<FString> RequiredOperations;
		int32 LimitPerProvider = 20;
		FContentDiscoveryCursor Cursor;

		bool IsValid() const;
		FString GetRequestFingerprint() const;
	};

	struct UEAGENTINTERFACE_API FContentDiscoveryItem final
	{
		FString Identity;
		FString DisplayName;
		FString Domain;
		FString ContentKind;
		FString ClassPath;
		FString OwnerIdentity;
		FString StateLayer;
		FString Revision;
		FString DetailCommandId;
		FString MatchReason;
		TArray<FString> SupportedOperations;
		TArray<FString> SearchTerms;
		int32 MatchScore = 0;

		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FContentDiscoveryProviderPage final
	{
		FString ProviderId;
		FString Revision;
		TArray<FContentDiscoveryItem> Items;
		int32 TotalMatches = 0;
		int32 NextOffset = -1;
		bool bComplete = true;
		bool bReadOnly = true;

		bool IsValid(const int32 Limit) const;
	};

	struct UEAGENTINTERFACE_API FContentDiscoveryError final
	{
		FString Code;
		FString SubjectId;
		FString Detail;

		bool IsPresent() const;
	};

	struct UEAGENTINTERFACE_API FContentDiscoveryProviderResult final
	{
		FString ProviderId;
		TArray<FString> ContentKinds;
		TOptional<FContentDiscoveryProviderPage> Page;
		FContentDiscoveryError Error;

		bool Succeeded() const;
	};

	struct UEAGENTINTERFACE_API FContentDiscoverySearchResult final
	{
		FString RequestFingerprint;
		TArray<FContentDiscoveryProviderResult> Providers;

		bool HasFailures() const;
		bool HasMore() const;
		bool IsComplete() const;
	};

	class UEAGENTINTERFACE_API IUeAgentContentDiscoveryProvider
	{
	public:
		virtual ~IUeAgentContentDiscoveryProvider() = default;

		virtual const FString& GetProviderId() const = 0;
		virtual const TArray<FString>& GetContentKinds() const = 0;
		virtual bool TrySearch(
			const FContentDiscoveryRequest& Request,
			int32 Offset,
			int32 Limit,
			FContentDiscoveryProviderPage& OutPage,
			FContentDiscoveryError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API FUeAgentContentDiscoveryService final
	{
	public:
		static bool TryCreate(
			TArray<TSharedRef<IUeAgentContentDiscoveryProvider>> Providers,
			TSharedPtr<const FUeAgentContentDiscoveryService>& OutService,
			FContentDiscoveryError& OutError);

		bool TrySearch(
			const FContentDiscoveryRequest& Request,
			FContentDiscoverySearchResult& OutResult,
			FContentDiscoveryError& OutError) const;

		const TArray<FString>& GetProviderIds() const;

	private:
		explicit FUeAgentContentDiscoveryService(
			TMap<FString, TSharedRef<IUeAgentContentDiscoveryProvider>>
				InProviders,
			TArray<FString> InProviderIds);

		TMap<FString, TSharedRef<IUeAgentContentDiscoveryProvider>>
			Providers;
		TArray<FString> ProviderIds;
	};

	UEAGENTINTERFACE_API bool TryScoreContentDiscoveryMatch(
		const FContentDiscoveryRequest& Request,
		const FString& Identity,
		const FString& DisplayName,
		const TArray<FString>& ExtraTerms,
		int32& OutScore,
		FString& OutReason);

	UEAGENTINTERFACE_API bool MatchesContentDiscoveryFilters(
		const FContentDiscoveryRequest& Request,
		const FContentDiscoveryItem& Item);
}

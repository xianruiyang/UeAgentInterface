#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

class FJsonObject;

namespace UE::UAI::ContentDiscovery
{
	enum class EContentDiscoveryMatchMode : uint8
	{
		Exact,
		Prefix,
		Text,
		Fuzzy,
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

		bool IsValid() const;
		bool TryValidate(Contract::FDiagnostic& OutError) const;
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
		/** Provider-owned semantic scopes permitted on the model-facing name surface. */
		TArray<FString> PublicDiscoveryScopes;
		FString StateLayer;
		FString Revision;
		FString DetailCommandId;
		FString DetailTargetIdentity;
		TSharedPtr<FJsonObject> DetailInput;
		FString MatchReason;
		TArray<FString> SupportedOperations;
		TArray<FString> SearchTerms;
		int32 MatchScore = 0;

		bool IsValid() const;
		bool TryValidate(Contract::FDiagnostic& OutError, const FString& Path = FString()) const;
	};

	struct UEAGENTINTERFACE_API FContentDiscoveryProviderPage final
	{
		FString ProviderId;
		FString Revision;
		TArray<FContentDiscoveryItem> Items;
		int32 TotalMatches = 0;
		bool bReadOnly = true;

		bool IsValid() const;
		bool TryValidate(Contract::FDiagnostic& OutError) const;
	};

	using FContentDiscoveryError = UE::UAI::Contract::FDiagnostic;

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
		bool IsComplete() const;
		FContentDiscoveryError GetFailureDiagnostic() const;
	};

	class UEAGENTINTERFACE_API IUeAgentContentDiscoveryProvider
	{
	public:
		virtual ~IUeAgentContentDiscoveryProvider() = default;

		virtual const FString& GetProviderId() const = 0;
		virtual const TArray<FString>& GetContentKinds() const = 0;
		virtual bool TrySearch(
			const FContentDiscoveryRequest& Request,
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

		/** Resolve the same readable name/scopes used by content search; identity stays internal. */
		bool TryResolveReadableName(
			const FString& Name,
			FContentDiscoveryItem& OutItem,
			FContentDiscoveryError& OutError) const;

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

	UEAGENTINTERFACE_API bool TryBuildContentDiscoveryProviderPage(
		const FString& ProviderId,
		const FString& Revision,
		const FContentDiscoveryRequest& Request,
		TArray<FContentDiscoveryItem> Items,
		FContentDiscoveryProviderPage& OutPage,
		FContentDiscoveryError& OutError);
}

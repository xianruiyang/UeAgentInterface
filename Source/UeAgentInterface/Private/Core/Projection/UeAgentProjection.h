#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::Projection
{
	using UE::UAI::ContextResource::EUeAgentResourceDimension;
	using UE::UAI::ContextResource::FUeAgentCancellationToken;
	using UE::UAI::ContextResource::FUeAgentOperationDeadline;
	using UE::UAI::ContextResource::FUeAgentResourceTracker;
	using UE::UAI::Contract::FUeAgentDescriptorIdentity;
	using UE::UAI::Discovery::EUeAgentCatalogCompleteness;
	using UE::UAI::Discovery::FUeAgentCandidateBinding;
	using UE::UAI::Discovery::FUeAgentCandidateSummary;
	using UE::UAI::Discovery::FUeAgentCatalogDiagnostic;
	using UE::UAI::Discovery::FUeAgentCatalogRevision;
	using UE::UAI::Discovery::FUeAgentCatalogSnapshot;
	using UE::UAI::StateSemantic::EUeAgentStatePlane;
	using UE::UAI::StateSemantic::FUeAgentCanonicalIdentity;
	using UE::UAI::StateSemantic::FUeAgentCanonicalSnapshot;
	using UE::UAI::StateSemantic::FUeAgentCanonicalType;
	using UE::UAI::StateSemantic::FUeAgentCanonicalValue;
	using UE::UAI::StateSemantic::FUeAgentCanonicalValueRef;
	using UE::UAI::StateSemantic::FUeAgentDefaultValue;
	using UE::UAI::StateSemantic::FUeAgentSemanticComparison;
	using UE::UAI::StateSemantic::FUeAgentStateDetails;
	using UE::UAI::StateSemantic::FUeAgentStateEntry;
	using UE::UAI::StateSemantic::FUeAgentStateProvenance;
	using UE::UAI::StateSemantic::FUeAgentStateRevision;
	using UE::UAI::StateSemantic::FUeAgentTemporalPoint;

	enum class EUeAgentProjectionResultKind : uint8
	{
		Query,
		Operation,
	};

	enum class EUeAgentProjectionPageUnit : uint8
	{
		Entry,
		OwnerRecord,
		SingleRecord,
	};

	enum class EUeAgentTokenEstimateKind : uint8
	{
		Exact,
		Conservative,
	};

	using FUeAgentProjectionError = UE::UAI::Contract::FDiagnostic;

	class UEAGENTINTERFACE_API FUeAgentProjectionProfile final
	{
	public:
		FUeAgentProjectionProfile(
			FString InProfileId,
			bool bInIncludeDefaults,
			bool bInIncludeDetails,
			bool bInIncludeProvenance);

		const FString& GetProfileId() const;
		bool IncludesDefaults() const;
		bool IncludesDetails() const;
		bool IncludesProvenance() const;
		bool IsValid() const;
		const FUeAgentProjectionError& GetValidationDiagnostic() const;

	private:
		FString ProfileId;
		bool bIncludeDefaults = false;
		bool bIncludeDetails = false;
		bool bIncludeProvenance = false;
		FUeAgentProjectionError ValidationDiagnostic;
	};

	class UEAGENTINTERFACE_API FUeAgentProjectionDiagnostic final
	{
	public:
		explicit FUeAgentProjectionDiagnostic(FUeAgentProjectionError InDiagnostic);
		const FUeAgentProjectionError& GetDiagnostic() const;
		bool IsValid() const;
		const FUeAgentProjectionError& GetValidationDiagnostic() const;

	private:
		FUeAgentProjectionError Diagnostic;
		FUeAgentProjectionError ValidationDiagnostic;
	};

	class UEAGENTINTERFACE_API FUeAgentArtifactDescriptor final
	{
	public:
		FUeAgentArtifactDescriptor(
			FString InArtifactId,
			FString InKind,
			FString InLocator,
			FString InMediaType,
			FString InContentHash,
			int64 InByteCount,
			FString InReceiptId,
			bool bInImportable);

		const FString& GetArtifactId() const;
		const FString& GetKind() const;
		const FString& GetLocator() const;
		const FString& GetMediaType() const;
		const FString& GetContentHash() const;
		int64 GetByteCount() const;
		const FString& GetReceiptId() const;
		bool IsImportable() const;
		bool IsValid() const;
		const FUeAgentProjectionError& GetValidationDiagnostic() const;

	private:
		FString ArtifactId;
		FString Kind;
		FString Locator;
		FString MediaType;
		FString ContentHash;
		int64 ByteCount = -1;
		FString ReceiptId;
		bool bImportable = false;
		FUeAgentProjectionError ValidationDiagnostic;
	};

	class UEAGENTINTERFACE_API FUeAgentTokenEstimate final
	{
	public:
		FUeAgentTokenEstimate();
		FUeAgentTokenEstimate(
			EUeAgentTokenEstimateKind InKind,
			int64 InTokens,
			int64 InUtf8Bytes);

		EUeAgentTokenEstimateKind GetKind() const;
		int64 GetTokens() const;
		int64 GetUtf8Bytes() const;
		bool IsValid() const;
		const FUeAgentProjectionError& GetValidationDiagnostic() const;

	private:
		EUeAgentTokenEstimateKind Kind =
			EUeAgentTokenEstimateKind::Exact;
		int64 Tokens = -1;
		int64 Utf8Bytes = -1;
		FUeAgentProjectionError ValidationDiagnostic;
	};

	class UEAGENTINTERFACE_API FUeAgentProjectedEntry final
	{
	public:
		FUeAgentProjectedEntry(
			FUeAgentCanonicalIdentity InIdentity,
			FUeAgentCanonicalType InType,
			FUeAgentCanonicalValueRef InValue,
			FUeAgentStateRevision InObservedRevision,
			TOptional<FUeAgentDefaultValue> InDefault,
			TOptional<FUeAgentStateDetails> InDetails);

		const FUeAgentCanonicalIdentity& GetIdentity() const;
		const FUeAgentCanonicalType& GetType() const;
		const FUeAgentCanonicalValueRef& GetValue() const;
		const FUeAgentStateRevision& GetObservedRevision() const;
		const TOptional<FUeAgentDefaultValue>& GetDefault() const;
		const TOptional<FUeAgentStateDetails>& GetDetails() const;
		bool IsValid() const;
		const FUeAgentProjectionError& GetValidationDiagnostic() const;

	private:
		FUeAgentCanonicalIdentity Identity;
		FUeAgentCanonicalType Type;
		FUeAgentCanonicalValueRef Value;
		FUeAgentStateRevision ObservedRevision;
		TOptional<FUeAgentDefaultValue> Default;
		TOptional<FUeAgentStateDetails> Details;
		FUeAgentProjectionError ValidationDiagnostic;
	};

	class UEAGENTINTERFACE_API FUeAgentPageInfo final
	{
	public:
		FUeAgentPageInfo(
			int32 InReturnedItems,
			int32 InTotalItems);

		int32 GetReturnedItems() const;
		int32 GetTotalItems() const;
		bool IsValid() const;
		const FUeAgentProjectionError& GetValidationDiagnostic() const;

	private:
		int32 ReturnedItems = -1;
		int32 TotalItems = -1;
		FUeAgentProjectionError ValidationDiagnostic;
	};

	class UEAGENTINTERFACE_API FUeAgentProjectedSnapshot final
	{
	public:
		FUeAgentProjectedSnapshot(
			FString InSnapshotId,
			EUeAgentStatePlane InStatePlane,
			FUeAgentStateRevision InRevision,
			FUeAgentTemporalPoint InCapturedAt,
			TOptional<FUeAgentStateProvenance> InProvenance,
			TArray<FUeAgentProjectedEntry> InEntries,
			FUeAgentPageInfo InPage,
			TArray<FString> InOmittedSections);

		const FString& GetSnapshotId() const;
		EUeAgentStatePlane GetStatePlane() const;
		const FUeAgentStateRevision& GetRevision() const;
		const FUeAgentTemporalPoint& GetCapturedAt() const;
		const TOptional<FUeAgentStateProvenance>& GetProvenance() const;
		const TArray<FUeAgentProjectedEntry>& GetEntries() const;
		const FUeAgentPageInfo& GetPage() const;
		const TArray<FString>& GetOmittedSections() const;
		bool IsValid() const;
		const FUeAgentProjectionError& GetValidationDiagnostic() const;

	private:
		FString SnapshotId;
		EUeAgentStatePlane StatePlane =
			EUeAgentStatePlane::AuthoringMemory;
		FUeAgentStateRevision Revision;
		FUeAgentTemporalPoint CapturedAt;
		TOptional<FUeAgentStateProvenance> Provenance;
		TArray<FUeAgentProjectedEntry> Entries;
		FUeAgentPageInfo Page;
		TArray<FString> OmittedSections;
		FUeAgentProjectionError ValidationDiagnostic;
	};

	class UEAGENTINTERFACE_API FUeAgentProjectedCatalog final
	{
	public:
		FUeAgentProjectedCatalog(
			FString InCatalogId,
			FString InCatalogKind,
			FUeAgentCatalogRevision InRevision,
			EUeAgentCatalogCompleteness InCompleteness,
			TArray<FUeAgentCandidateSummary> InCandidates,
			TArray<FUeAgentCatalogDiagnostic> InDiagnostics,
			FUeAgentPageInfo InPage);

		const FString& GetCatalogId() const;
		const FString& GetCatalogKind() const;
		const FUeAgentCatalogRevision& GetRevision() const;
		EUeAgentCatalogCompleteness GetCompleteness() const;
		const TArray<FUeAgentCandidateSummary>& GetCandidates() const;
		const TArray<FUeAgentCatalogDiagnostic>& GetDiagnostics() const;
		const FUeAgentPageInfo& GetPage() const;
		bool IsValid() const;
		const FUeAgentProjectionError& GetValidationDiagnostic() const;

	private:
		FString CatalogId;
		FString CatalogKind;
		FUeAgentCatalogRevision Revision;
		EUeAgentCatalogCompleteness Completeness =
			EUeAgentCatalogCompleteness::Complete;
		TArray<FUeAgentCandidateSummary> Candidates;
		TArray<FUeAgentCatalogDiagnostic> Diagnostics;
		FUeAgentPageInfo Page;
		FUeAgentProjectionError ValidationDiagnostic;
	};

	class UEAGENTINTERFACE_API FUeAgentProjectedPayload final
	{
	public:
		explicit FUeAgentProjectedPayload(
			TSharedRef<const FUeAgentProjectedSnapshot> InSnapshot);
		explicit FUeAgentProjectedPayload(
			TSharedRef<const FUeAgentProjectedCatalog> InCatalog);
		explicit FUeAgentProjectedPayload(
			TSharedRef<const FUeAgentCandidateBinding> InCandidateBinding);
		explicit FUeAgentProjectedPayload(
			FUeAgentCanonicalValueRef InValue);
		explicit FUeAgentProjectedPayload(
			TSharedRef<const FUeAgentSemanticComparison> InComparison);

		const TSharedPtr<const FUeAgentProjectedSnapshot>& GetSnapshot() const;
		const TSharedPtr<const FUeAgentProjectedCatalog>& GetCatalog() const;
		const TSharedPtr<const FUeAgentCandidateBinding>& GetCandidateBinding() const;
		const TSharedPtr<const FUeAgentCanonicalValue>& GetValue() const;
		const TSharedPtr<const FUeAgentSemanticComparison>& GetComparison() const;
		bool IsValid() const;

	private:
		TSharedPtr<const FUeAgentProjectedSnapshot> Snapshot;
		TSharedPtr<const FUeAgentProjectedCatalog> Catalog;
		TSharedPtr<const FUeAgentCandidateBinding> CandidateBinding;
		TSharedPtr<const FUeAgentCanonicalValue> Value;
		TSharedPtr<const FUeAgentSemanticComparison> Comparison;
	};

	class UEAGENTINTERFACE_API FUeAgentProjectionRequest final
	{
	public:
		FUeAgentProjectionRequest(
			FUeAgentProjectionProfile InProfile,
			TArray<FUeAgentArtifactDescriptor> InArtifacts,
			TArray<FUeAgentProjectionDiagnostic> InDiagnostics,
			EUeAgentProjectionPageUnit InPageUnit =
				EUeAgentProjectionPageUnit::Entry);

		const FUeAgentProjectionProfile& GetProfile() const;
		EUeAgentProjectionPageUnit GetPageUnit() const;
		const TArray<FUeAgentArtifactDescriptor>& GetArtifacts() const;
		const TArray<FUeAgentProjectionDiagnostic>& GetDiagnostics() const;
		bool IsValid() const;
		const FUeAgentProjectionError& GetValidationDiagnostic() const;

	private:
		FUeAgentProjectionProfile Profile;
		TArray<FUeAgentArtifactDescriptor> Artifacts;
		TArray<FUeAgentProjectionDiagnostic> Diagnostics;
		EUeAgentProjectionPageUnit PageUnit =
			EUeAgentProjectionPageUnit::Entry;
		FUeAgentProjectionError ValidationDiagnostic;
	};

	class UEAGENTINTERFACE_API FUeAgentProjectionSourceValue final
	{
	public:
		explicit FUeAgentProjectionSourceValue(
			TSharedRef<const FUeAgentCanonicalSnapshot> InSnapshot);
		explicit FUeAgentProjectionSourceValue(
			TSharedRef<const FUeAgentCatalogSnapshot> InCatalog);
		explicit FUeAgentProjectionSourceValue(
			TSharedRef<const FUeAgentCandidateBinding> InCandidateBinding);
		explicit FUeAgentProjectionSourceValue(
			FUeAgentCanonicalValueRef InValue);
		explicit FUeAgentProjectionSourceValue(
			TSharedRef<const FUeAgentSemanticComparison> InComparison);

		const TSharedPtr<const FUeAgentCanonicalSnapshot>& GetSnapshot() const;
		const TSharedPtr<const FUeAgentCatalogSnapshot>& GetCatalog() const;
		const TSharedPtr<const FUeAgentCandidateBinding>& GetCandidateBinding() const;
		const TSharedPtr<const FUeAgentCanonicalValue>& GetValue() const;
		const TSharedPtr<const FUeAgentSemanticComparison>& GetComparison() const;
		bool IsValid() const;

	private:
		TSharedPtr<const FUeAgentCanonicalSnapshot> Snapshot;
		TSharedPtr<const FUeAgentCatalogSnapshot> Catalog;
		TSharedPtr<const FUeAgentCandidateBinding> CandidateBinding;
		TSharedPtr<const FUeAgentCanonicalValue> Value;
		TSharedPtr<const FUeAgentSemanticComparison> Comparison;
	};

	using FUeAgentProjectionQueryResult =
		UE::UAI::Contract::TUeAgentQueryResult<FUeAgentProjectionSourceValue>;
	using FUeAgentProjectionOperationResult =
		UE::UAI::Contract::TUeAgentOperationResult<FUeAgentProjectionSourceValue>;

	class UEAGENTINTERFACE_API IUeAgentTokenEstimator
	{
	public:
		virtual ~IUeAgentTokenEstimator() = default;

		virtual bool TryEstimate(
			const FUeAgentProjectedPayload& Payload,
			const TArray<FUeAgentArtifactDescriptor>& Artifacts,
			const TArray<FUeAgentProjectionDiagnostic>& Diagnostics,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			FUeAgentTokenEstimate& OutEstimate,
			FUeAgentProjectionError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API FUeAgentProjectedResponse final
	{
	public:
		const FUeAgentDescriptorIdentity& GetDescriptorIdentity() const;
		const FString& GetRequestId() const;
		EUeAgentProjectionResultKind GetResultKind() const;
		const FString& GetProfileId() const;
		const FUeAgentProjectedPayload& GetPayload() const;
		const TArray<FUeAgentArtifactDescriptor>& GetArtifacts() const;
		const TArray<FUeAgentProjectionDiagnostic>& GetDiagnostics() const;
		const FUeAgentTokenEstimate& GetTokenEstimate() const;
		bool IsValid() const;
		const FUeAgentProjectionError& GetValidationDiagnostic() const;

	private:
		FUeAgentProjectedResponse(
			FUeAgentDescriptorIdentity InDescriptorIdentity,
			FString InRequestId,
			EUeAgentProjectionResultKind InResultKind,
			FString InProfileId,
			FUeAgentProjectedPayload InPayload,
			TArray<FUeAgentArtifactDescriptor> InArtifacts,
			TArray<FUeAgentProjectionDiagnostic> InDiagnostics,
			FUeAgentTokenEstimate InTokenEstimate);

		FUeAgentDescriptorIdentity DescriptorIdentity;
		FString RequestId;
		EUeAgentProjectionResultKind ResultKind =
			EUeAgentProjectionResultKind::Query;
		FString ProfileId;
		FUeAgentProjectedPayload Payload;
		TArray<FUeAgentArtifactDescriptor> Artifacts;
		TArray<FUeAgentProjectionDiagnostic> Diagnostics;
		FUeAgentTokenEstimate TokenEstimate;
		FUeAgentProjectionError ValidationDiagnostic;

		friend class FUeAgentResponseProjector;
	};

	class UEAGENTINTERFACE_API FUeAgentResponseProjector final
	{
	public:
		static bool TryCreate(
			const TSharedRef<IUeAgentTokenEstimator>& Estimator,
			TSharedPtr<const FUeAgentResponseProjector>& OutProjector,
			FUeAgentProjectionError& OutError);

		bool TryProject(
			const FUeAgentProjectionQueryResult& Result,
			const FUeAgentProjectionRequest& Request,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			TSharedPtr<const FUeAgentProjectedResponse>& OutResponse,
			FUeAgentProjectionError& OutError) const;
		bool TryProject(
			const FUeAgentProjectionOperationResult& Result,
			const FUeAgentProjectionRequest& Request,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			TSharedPtr<const FUeAgentProjectedResponse>& OutResponse,
			FUeAgentProjectionError& OutError) const;

	private:
		explicit FUeAgentResponseProjector(
			TSharedRef<IUeAgentTokenEstimator> InEstimator);

		bool TryProjectInternal(
			const FUeAgentDescriptorIdentity& DescriptorIdentity,
			const FString& RequestId,
			EUeAgentProjectionResultKind ResultKind,
			const FUeAgentProjectionSourceValue& Source,
			const FUeAgentProjectionRequest& Request,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			TSharedPtr<const FUeAgentProjectedResponse>& OutResponse,
			FUeAgentProjectionError& OutError) const;

		TSharedRef<IUeAgentTokenEstimator> Estimator;
	};
}

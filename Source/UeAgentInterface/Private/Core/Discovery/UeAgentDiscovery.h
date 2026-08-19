#pragma once

#include "CoreMinimal.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::Discovery
{
	using UE::UAI::ContextResource::EUeAgentResourceDimension;
	using UE::UAI::ContextResource::FUeAgentCancellationToken;
	using UE::UAI::ContextResource::FUeAgentOperationDeadline;
	using UE::UAI::ContextResource::FUeAgentResourceTracker;
	using UE::UAI::Contract::FUeAgentDescriptorIdentity;
	using UE::UAI::StateSemantic::EUeAgentStatePlane;
	using UE::UAI::StateSemantic::FUeAgentCanonicalIdentity;
	using UE::UAI::StateSemantic::FUeAgentCanonicalSnapshot;

	enum class EUeAgentCatalogCompleteness : uint8
	{
		Complete,
		Partial,
		Degraded,
	};

	enum class EUeAgentDiscoveryMatchMode : uint8
	{
		Broad,
		Exact,
	};

	enum class EUeAgentMaterializationPolicy : uint8
	{
		IndexOnly,
		AllowMaterialization,
	};

	enum class EUeAgentCandidateOperation : uint8
	{
		Read,
		Create,
		Update,
		Replace,
		Delete,
	};

	struct UEAGENTINTERFACE_API FUeAgentDiscoveryError
	{
		FString Code;
		FString SubjectId;
		FString Detail;
		EUeAgentResourceDimension ResourceDimension =
			EUeAgentResourceDimension::WorkUnits;
		int64 ResourceLimit = 0;
		int64 ResourceObserved = 0;
	};

	class UEAGENTINTERFACE_API FUeAgentCatalogRevision final
	{
	public:
		FUeAgentCatalogRevision(
			FString InSourceId,
			FString InGeneration,
			int64 InSequence);

		const FString& GetSourceId() const;
		const FString& GetGeneration() const;
		int64 GetSequence() const;
		bool IsValid() const;
		bool operator==(const FUeAgentCatalogRevision& Other) const;
		bool operator!=(const FUeAgentCatalogRevision& Other) const;

	private:
		FString SourceId;
		FString Generation;
		int64 Sequence = -1;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API FUeAgentCatalogDiagnostic final
	{
	public:
		FUeAgentCatalogDiagnostic(
			FString InSourceId,
			FString InCode,
			FString InMessage);

		const FString& GetSourceId() const;
		const FString& GetCode() const;
		const FString& GetMessage() const;
		bool IsValid() const;
		bool operator==(const FUeAgentCatalogDiagnostic& Other) const;

	private:
		FString SourceId;
		FString Code;
		FString Message;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API FUeAgentDiscoveryQuery final
	{
	public:
		FUeAgentDiscoveryQuery(
			FString InCatalogKind,
			FString InSearchText,
			EUeAgentDiscoveryMatchMode InMatchMode,
			EUeAgentMaterializationPolicy InMaterializationPolicy);

		const FString& GetCatalogKind() const;
		const FString& GetSearchText() const;
		EUeAgentDiscoveryMatchMode GetMatchMode() const;
		EUeAgentMaterializationPolicy GetMaterializationPolicy() const;
		bool IsValid() const;

	private:
		FString CatalogKind;
		FString SearchText;
		EUeAgentDiscoveryMatchMode MatchMode =
			EUeAgentDiscoveryMatchMode::Broad;
		EUeAgentMaterializationPolicy MaterializationPolicy =
			EUeAgentMaterializationPolicy::IndexOnly;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API FUeAgentCandidateSummary final
	{
	public:
		FUeAgentCandidateSummary(
			FUeAgentCanonicalIdentity InIdentity,
			FUeAgentDescriptorIdentity InDescriptorIdentity,
			FString InCatalogKind,
			FString InDisplayName,
			FString InSummary,
			TArray<FString> InCategoryPath);

		const FUeAgentCanonicalIdentity& GetIdentity() const;
		const FUeAgentDescriptorIdentity& GetDescriptorIdentity() const;
		const FString& GetCatalogKind() const;
		const FString& GetDisplayName() const;
		const FString& GetSummary() const;
		const TArray<FString>& GetCategoryPath() const;
		bool IsValid() const;
		bool operator==(const FUeAgentCandidateSummary& Other) const;
		bool operator!=(const FUeAgentCandidateSummary& Other) const;

	private:
		FUeAgentCanonicalIdentity Identity;
		FUeAgentDescriptorIdentity DescriptorIdentity;
		FString CatalogKind;
		FString DisplayName;
		FString Summary;
		TArray<FString> CategoryPath;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API FUeAgentCandidateBinding final
	{
	public:
		FUeAgentCandidateBinding(
			FString InBindingId,
			FUeAgentCandidateSummary InSummary,
			FUeAgentCatalogRevision InCatalogRevision,
			FString InPublicRef,
			FString InReadPortId,
			TArray<EUeAgentCandidateOperation> InSupportedOperations,
			FString InBindingEvidence);

		const FString& GetBindingId() const;
		const FUeAgentCandidateSummary& GetSummary() const;
		const FUeAgentCatalogRevision& GetCatalogRevision() const;
		const FString& GetPublicRef() const;
		const FString& GetReadPortId() const;
		const TArray<EUeAgentCandidateOperation>& GetSupportedOperations() const;
		const FString& GetBindingEvidence() const;
		bool Supports(EUeAgentCandidateOperation Operation) const;
		bool IsValid() const;
		bool operator==(const FUeAgentCandidateBinding& Other) const;

	private:
		FString BindingId;
		FUeAgentCandidateSummary Summary;
		FUeAgentCatalogRevision CatalogRevision;
		FString PublicRef;
		FString ReadPortId;
		TArray<EUeAgentCandidateOperation> SupportedOperations;
		FString BindingEvidence;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API FUeAgentCatalogSnapshot final
	{
	public:
		static bool TryCreate(
			const FString& CatalogId,
			const FString& CatalogKind,
			const FUeAgentCatalogRevision& Revision,
			EUeAgentCatalogCompleteness Completeness,
			const TArray<FUeAgentCandidateSummary>& Candidates,
			const TArray<FUeAgentCatalogDiagnostic>& Diagnostics,
			FUeAgentResourceTracker& Resources,
			TSharedPtr<const FUeAgentCatalogSnapshot>& OutCatalog,
			FUeAgentDiscoveryError& OutError);

		const FString& GetCatalogId() const;
		const FString& GetCatalogKind() const;
		const FUeAgentCatalogRevision& GetRevision() const;
		EUeAgentCatalogCompleteness GetCompleteness() const;
		const TArray<FUeAgentCandidateSummary>& GetCandidates() const;
		const TArray<FUeAgentCatalogDiagnostic>& GetDiagnostics() const;
		bool IsValid() const;

	private:
		FUeAgentCatalogSnapshot(
			FString InCatalogId,
			FString InCatalogKind,
			FUeAgentCatalogRevision InRevision,
			EUeAgentCatalogCompleteness InCompleteness,
			TArray<FUeAgentCandidateSummary> InCandidates,
			TArray<FUeAgentCatalogDiagnostic> InDiagnostics);

		FString CatalogId;
		FString CatalogKind;
		FUeAgentCatalogRevision Revision;
		EUeAgentCatalogCompleteness Completeness =
			EUeAgentCatalogCompleteness::Complete;
		TArray<FUeAgentCandidateSummary> Candidates;
		TArray<FUeAgentCatalogDiagnostic> Diagnostics;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API FUeAgentStateReadRequest final
	{
	public:
		FUeAgentStateReadRequest(
			FUeAgentCandidateBinding InBinding,
			EUeAgentStatePlane InStatePlane);

		const FUeAgentCandidateBinding& GetBinding() const;
		EUeAgentStatePlane GetStatePlane() const;
		bool IsValid() const;

	private:
		FUeAgentCandidateBinding Binding;
		EUeAgentStatePlane StatePlane = EUeAgentStatePlane::AuthoringMemory;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API IUeAgentCatalogSource
	{
	public:
		virtual ~IUeAgentCatalogSource() = default;

		virtual const FString& GetSourceId() const = 0;
		virtual const TArray<FString>& GetCatalogKinds() const = 0;
		virtual bool TrySearch(
			const FUeAgentDiscoveryQuery& Query,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			TSharedPtr<const FUeAgentCatalogSnapshot>& OutCatalog,
			FUeAgentDiscoveryError& OutError) = 0;
		virtual bool TryResolveExact(
			const FString& CandidateId,
			const FUeAgentCatalogRevision& ExpectedRevision,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			TSharedPtr<const FUeAgentCandidateBinding>& OutBinding,
			FUeAgentDiscoveryError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API IUeAgentStateReadPort
	{
	public:
		virtual ~IUeAgentStateReadPort() = default;

		virtual const FString& GetPortId() const = 0;
		virtual bool TryRead(
			const FUeAgentStateReadRequest& Request,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
			FUeAgentDiscoveryError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API FUeAgentDiscoveryService final
	{
	public:
		static bool TryCreate(
			const TArray<TSharedRef<IUeAgentCatalogSource>>& CatalogSources,
			const TArray<TSharedRef<IUeAgentStateReadPort>>& ReadPorts,
			TSharedPtr<const FUeAgentDiscoveryService>& OutService,
			FUeAgentDiscoveryError& OutError);

		bool TrySearch(
			const FUeAgentDiscoveryQuery& Query,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			TSharedPtr<const FUeAgentCatalogSnapshot>& OutCatalog,
			FUeAgentDiscoveryError& OutError) const;
		bool TryResolveExact(
			const FUeAgentCatalogSnapshot& Catalog,
			const FString& CandidateId,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			TSharedPtr<const FUeAgentCandidateBinding>& OutBinding,
			FUeAgentDiscoveryError& OutError) const;
		bool TryRead(
			const FUeAgentCandidateBinding& Binding,
			EUeAgentStatePlane StatePlane,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
			FUeAgentDiscoveryError& OutError) const;

	private:
		FUeAgentDiscoveryService(
			TMap<FString, TSharedPtr<IUeAgentCatalogSource>> InSourcesByKind,
			TMap<FString, TSharedPtr<IUeAgentStateReadPort>> InReadPorts);

		TMap<FString, TSharedPtr<IUeAgentCatalogSource>> SourcesByKind;
		TMap<FString, TSharedPtr<IUeAgentStateReadPort>> ReadPorts;
	};
}

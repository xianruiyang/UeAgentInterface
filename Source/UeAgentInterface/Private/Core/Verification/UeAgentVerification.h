#pragma once

#include "CoreMinimal.h"
#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"

namespace UE::UAI::Verification
{
	using UE::UAI::AssetDurable::FDurableArtifact;
	using UE::UAI::AssetDurable::FDurableInventory;
	using UE::UAI::AssetDurable::FDurableReceipt;
	using UE::UAI::AssetDurable::FDurableVerificationAcknowledgement;
	using UE::UAI::AssetDurable::IDurableVerificationAcknowledgerPort;
	using UE::UAI::ContextResource::FUeAgentCancellationToken;
	using UE::UAI::ContextResource::FUeAgentOperationDeadline;
	using UE::UAI::ContextResource::FUeAgentResourceTracker;
	using UE::UAI::Contract::FUeAgentDescriptorIdentity;
	using UE::UAI::Discovery::FUeAgentCandidateBinding;
	using UE::UAI::Discovery::IUeAgentStateReadPort;
	using UE::UAI::StateSemantic::FUeAgentCanonicalIdentity;
	using UE::UAI::StateSemantic::FUeAgentCanonicalSnapshotPtr;
	using UE::UAI::StateSemantic::FUeAgentSemanticComparison;

	using FVerificationAcknowledgement =
		FDurableVerificationAcknowledgement;
	using IDurableVerificationAcknowledger =
		IDurableVerificationAcknowledgerPort;

	class UEAGENTINTERFACE_API FVerificationError final
	{
	public:
		FVerificationError() = default;
		explicit FVerificationError(
			FString InCode,
			FString InDetail = FString());

		const FString& GetCode() const;
		const FString& GetDetail() const;
		bool IsPresent() const;

	private:
		FString Code;
		FString Detail;
	};

	enum class EVerificationOracleKind : uint8
	{
		Persisted,
		Absence,
		Reference,
	};

	enum class EVerificationStatus : uint8
	{
		Verified,
		Mismatch,
		Unavailable,
	};

	enum class EEffectOutcome : uint8
	{
		Complete,
		RolledBack,
		Partial,
		StateUnknown,
	};

	UEAGENTINTERFACE_API FString LexToString(
		EVerificationOracleKind Value);
	UEAGENTINTERFACE_API FString LexToString(
		EVerificationStatus Value);
	UEAGENTINTERFACE_API FString LexToString(
		EEffectOutcome Value);

	class UEAGENTINTERFACE_API FVerificationTarget final
	{
	public:
		FVerificationTarget(
			FUeAgentCanonicalIdentity InIdentity,
			FUeAgentCandidateBinding InBinding,
			EVerificationOracleKind InOracleKind,
			FUeAgentCanonicalSnapshotPtr InExpectedSnapshot,
			TArray<FString> InArtifactIds,
			TArray<FString> InExpectedReferenceKeys);

		const FUeAgentCanonicalIdentity& GetIdentity() const;
		const FUeAgentCandidateBinding& GetBinding() const;
		EVerificationOracleKind GetOracleKind() const;
		const FUeAgentCanonicalSnapshotPtr& GetExpectedSnapshot() const;
		const TArray<FString>& GetArtifactIds() const;
		const TArray<FString>& GetExpectedReferenceKeys() const;
		bool IsValid() const;

	private:
		FUeAgentCanonicalIdentity Identity;
		FUeAgentCandidateBinding Binding;
		EVerificationOracleKind OracleKind =
			EVerificationOracleKind::Persisted;
		FUeAgentCanonicalSnapshotPtr ExpectedSnapshot;
		TArray<FString> ArtifactIds;
		TArray<FString> ExpectedReferenceKeys;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API FVerificationRequest final
	{
	public:
		FVerificationRequest(
			FString InOperationId,
			FUeAgentDescriptorIdentity InDescriptorIdentity,
			FDurableReceipt InDurableReceipt,
			FDurableInventory InInventory,
			FString InJournalOwnerId,
			int64 InFencingToken,
			TArray<FVerificationTarget> InTargets);

		const FString& GetOperationId() const;
		const FUeAgentDescriptorIdentity& GetDescriptorIdentity() const;
		const FDurableReceipt& GetDurableReceipt() const;
		const FDurableInventory& GetInventory() const;
		const FString& GetJournalOwnerId() const;
		int64 GetFencingToken() const;
		const TArray<FVerificationTarget>& GetTargets() const;
		bool IsValid() const;

	private:
		FString OperationId;
		FUeAgentDescriptorIdentity DescriptorIdentity;
		FDurableReceipt DurableReceipt;
		FDurableInventory Inventory;
		FString JournalOwnerId;
		int64 FencingToken = -1;
		TArray<FVerificationTarget> Targets;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API FAbsenceObservation final
	{
	public:
		FAbsenceObservation();
		FAbsenceObservation(
			FUeAgentCanonicalIdentity InIdentity,
			bool bInAbsent,
			bool bInUnavailable,
			FString InEvidenceId);

		const FUeAgentCanonicalIdentity& GetIdentity() const;
		bool IsAbsent() const;
		bool IsUnavailable() const;
		const FString& GetEvidenceId() const;
		bool IsValid() const;

	private:
		FUeAgentCanonicalIdentity Identity;
		bool bAbsent = false;
		bool bUnavailable = true;
		FString EvidenceId;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API FReferenceObservation final
	{
	public:
		FReferenceObservation();
		FReferenceObservation(
			FUeAgentCanonicalIdentity InIdentity,
			TArray<FString> InReferenceKeys,
			bool bInUnavailable,
			FString InEvidenceId);

		const FUeAgentCanonicalIdentity& GetIdentity() const;
		const TArray<FString>& GetReferenceKeys() const;
		bool IsUnavailable() const;
		const FString& GetEvidenceId() const;
		bool IsValid() const;

	private:
		FUeAgentCanonicalIdentity Identity;
		TArray<FString> ReferenceKeys;
		bool bUnavailable = true;
		FString EvidenceId;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API FTargetVerification final
	{
	public:
		FTargetVerification(
			FUeAgentCanonicalIdentity InIdentity,
			EVerificationOracleKind InOracleKind,
			EVerificationStatus InStatus,
			FString InExpectedSnapshotId,
			FString InActualSnapshotId,
			FUeAgentSemanticComparison InComparison,
			bool bInHasComparison,
			TArray<FString> InEvidenceIds);

		const FUeAgentCanonicalIdentity& GetIdentity() const;
		EVerificationOracleKind GetOracleKind() const;
		EVerificationStatus GetStatus() const;
		const FString& GetExpectedSnapshotId() const;
		const FString& GetActualSnapshotId() const;
		const FUeAgentSemanticComparison& GetComparison() const;
		bool HasComparison() const;
		const TArray<FString>& GetEvidenceIds() const;

	private:
		FUeAgentCanonicalIdentity Identity;
		EVerificationOracleKind OracleKind =
			EVerificationOracleKind::Persisted;
		EVerificationStatus Status =
			EVerificationStatus::Unavailable;
		FString ExpectedSnapshotId;
		FString ActualSnapshotId;
		FUeAgentSemanticComparison Comparison;
		bool bHasComparison = false;
		TArray<FString> EvidenceIds;
	};

	class UEAGENTINTERFACE_API FVerificationOutcome final
	{
	public:
		FVerificationOutcome() = default;
		FVerificationOutcome(
			FString InVerificationId,
			EVerificationStatus InStatus,
			EEffectOutcome InEffectOutcome,
			FString InDiagnosticCode,
			bool bInRetrySafe,
			TArray<FTargetVerification> InTargets,
			TArray<FString> InEvidenceIds,
			TOptional<FVerificationAcknowledgement> InAcknowledgement,
			FString InDiagnosticDetail = FString());

		const FString& GetVerificationId() const;
		EVerificationStatus GetStatus() const;
		EEffectOutcome GetEffectOutcome() const;
		const FString& GetDiagnosticCode() const;
		const FString& GetDiagnosticDetail() const;
		bool IsRetrySafe() const;
		const TArray<FTargetVerification>& GetTargets() const;
		const TArray<FString>& GetEvidenceIds() const;
		const FVerificationAcknowledgement* GetAcknowledgement() const;

	private:
		FString VerificationId;
		EVerificationStatus Status =
			EVerificationStatus::Unavailable;
		EEffectOutcome EffectOutcome =
			EEffectOutcome::StateUnknown;
		FString DiagnosticCode;
		FString DiagnosticDetail;
		bool bRetrySafe = false;
		TArray<FTargetVerification> Targets;
		TArray<FString> EvidenceIds;
		TOptional<FVerificationAcknowledgement> Acknowledgement;
	};

	class UEAGENTINTERFACE_API IAbsenceOracle
	{
	public:
		virtual ~IAbsenceOracle() = default;

		virtual bool TryObserve(
			const FUeAgentCanonicalIdentity& Identity,
			const TArray<FDurableArtifact>& Artifacts,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			FAbsenceObservation& OutObservation,
			FVerificationError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API IReferenceOracle
	{
	public:
		virtual ~IReferenceOracle() = default;

		virtual bool TryObserve(
			const FUeAgentCanonicalIdentity& Identity,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			FReferenceObservation& OutObservation,
			FVerificationError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API IPersistedStateOracle
	{
	public:
		virtual ~IPersistedStateOracle() = default;

		virtual const FString& GetReadPortId() const = 0;
		virtual bool TryObserve(
			const FVerificationTarget& Target,
			const TArray<FDurableArtifact>& Artifacts,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			FUeAgentCanonicalSnapshotPtr& OutSnapshot,
			FVerificationError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API
		FStateReadPortPersistedStateOracle final
		: public IPersistedStateOracle
	{
	public:
		explicit FStateReadPortPersistedStateOracle(
			IUeAgentStateReadPort& InStateReadPort);
		explicit FStateReadPortPersistedStateOracle(
			TSharedRef<
				IUeAgentStateReadPort,
				ESPMode::ThreadSafe> InStateReadPort);

		const FString& GetReadPortId() const override;
		bool TryObserve(
			const FVerificationTarget& Target,
			const TArray<FDurableArtifact>& Artifacts,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			FUeAgentCanonicalSnapshotPtr& OutSnapshot,
			FVerificationError& OutError) override;

	private:
		TSharedPtr<
			IUeAgentStateReadPort,
			ESPMode::ThreadSafe> OwnedStateReadPort;
		IUeAgentStateReadPort* StateReadPort = nullptr;
	};

	class UEAGENTINTERFACE_API FVerificationService final
	{
	public:
		FVerificationService(
			IPersistedStateOracle& InPersistedStateOracle,
			IAbsenceOracle& InAbsenceOracle,
			IReferenceOracle& InReferenceOracle,
			IDurableVerificationAcknowledger& InAcknowledger);

		bool TryVerify(
			const FVerificationRequest& Request,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			FVerificationOutcome& OutOutcome,
			FVerificationError& OutError) const;

	private:
		IPersistedStateOracle* PersistedStateOracle = nullptr;
		IAbsenceOracle* AbsenceOracle = nullptr;
		IReferenceOracle* ReferenceOracle = nullptr;
		IDurableVerificationAcknowledger* Acknowledger = nullptr;
	};
}

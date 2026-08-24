#pragma once

#include "CoreMinimal.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/AssetDurable/UeAgentPackageHeader.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Templates/SharedPointer.h"

#define UAI_DURABLE_ARTIFACT_KIND_V1 1
#define UAI_PROJECT_CONFIG_DURABLE_BACKEND_V1 1

namespace UE::UAI::AssetDurable
{
	using UE::UAI::ContextResource::FUeAgentResourceTracker;
	using UE::UAI::PlanningPreflight::FPlanningPreflightPlanRef;
	using UE::UAI::StateSemantic::FUeAgentCanonicalIdentity;

	class UEAGENTINTERFACE_API FAssetDurableError final
	{
	public:
		FAssetDurableError() = default;
		explicit FAssetDurableError(
			FString InCode,
			FString InDetail = FString());

		const FString& GetCode() const;
		const FString& GetDetail() const;
		bool IsPresent() const;

	private:
		FString Code;
		FString Detail;
	};

	enum class EAssetDurableIntent : uint8
	{
		Save,
		Delete,
	};

	enum class EDurableArtifactKind : uint8
	{
		Package,
		ProjectConfig,
	};

	enum class ERollbackGrade : uint8
	{
		Full,
		DurableBackup,
		Compensatable,
		None,
	};

	enum class EAtomicityMode : uint8
	{
		SingleOwnerAtomic,
		MultiOwnerAtomicSupported,
		CompensatedSaga,
		NonAtomicRejected,
	};

	enum class EDurableStatus : uint8
	{
		Prepared,
		PersistedUnverified,
		Verified,
		RolledBack,
		RecoveryRequired,
		StateUnknown,
	};

	enum class EDurableStatePresence : uint8
	{
		Unknown,
		Absent,
		Present,
	};

	enum class ERecoveryAction : uint8
	{
		None,
		AbortedBeforeMutation,
		RestoredOriginal,
		VerifyPersisted,
		ManualIntervention,
	};

	UEAGENTINTERFACE_API FString LexToString(
		EAssetDurableIntent Value);
	UEAGENTINTERFACE_API FString LexToString(
		EDurableArtifactKind Value);
	UEAGENTINTERFACE_API FString LexToString(
		ERollbackGrade Value);
	UEAGENTINTERFACE_API FString LexToString(
		EAtomicityMode Value);
	UEAGENTINTERFACE_API FString LexToString(
		EDurableStatus Value);
	UEAGENTINTERFACE_API FString LexToString(
		EDurableStatePresence Value);
	UEAGENTINTERFACE_API FString LexToString(
		ERecoveryAction Value);

	class UEAGENTINTERFACE_API FAssetDurableTarget final
	{
	public:
		FAssetDurableTarget(
			FUeAgentCanonicalIdentity InIdentity,
			FString InPackageName,
			EPackageHeaderKind InHeaderKind,
			EAssetDurableIntent InIntent,
			ERollbackGrade InRollbackGrade);
		FAssetDurableTarget(
			FUeAgentCanonicalIdentity InIdentity,
			EDurableArtifactKind InArtifactKind,
			FString InDurableLocation,
			EAssetDurableIntent InIntent,
			ERollbackGrade InRollbackGrade);

		const FUeAgentCanonicalIdentity& GetIdentity() const;
		EDurableArtifactKind GetArtifactKind() const;
		const FString& GetDurableLocation() const;
		const FString& GetPackageName() const;
		EPackageHeaderKind GetPackageHeaderKind() const;
		FString GetDurableTargetKey() const;
		EAssetDurableIntent GetIntent() const;
		ERollbackGrade GetRollbackGrade() const;
		bool IsValid() const;

	private:
		FUeAgentCanonicalIdentity Identity;
		EDurableArtifactKind ArtifactKind =
			EDurableArtifactKind::Package;
		FString DurableLocation;
		FString PackageName;
		EPackageHeaderKind PackageHeaderKind =
			EPackageHeaderKind::EvidenceRequired;
		EAssetDurableIntent Intent = EAssetDurableIntent::Save;
		ERollbackGrade RollbackGrade = ERollbackGrade::None;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API FAssetDurableRequest final
	{
	public:
		FAssetDurableRequest(
			FPlanningPreflightPlanRef InPlan,
			TArray<FAssetDurableTarget> InTargets,
			int64 InFencingToken,
			FString InJournalOwnerId,
			EAtomicityMode InAtomicityMode);

		const FPlanningPreflightPlanRef& GetPlan() const;
		const TArray<FAssetDurableTarget>& GetTargets() const;
		int64 GetFencingToken() const;
		const FString& GetJournalOwnerId() const;
		EAtomicityMode GetAtomicityMode() const;
		bool IsValid() const;

	private:
		FPlanningPreflightPlanRef Plan;
		TArray<FAssetDurableTarget> Targets;
		int64 FencingToken = -1;
		FString JournalOwnerId;
		EAtomicityMode AtomicityMode =
			EAtomicityMode::NonAtomicRejected;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API FDurableArtifact final
	{
	public:
		FDurableArtifact(
			FString InArtifactId,
			FUeAgentCanonicalIdentity InTargetIdentity,
			FString InPackageName,
			FString InFinalPath,
			EAssetDurableIntent InIntent,
			FString InRole);
		FDurableArtifact(
			FString InArtifactId,
			FUeAgentCanonicalIdentity InTargetIdentity,
			EDurableArtifactKind InArtifactKind,
			FString InDurableLocation,
			FString InFinalPath,
			EAssetDurableIntent InIntent,
			FString InRole);

		const FString& GetArtifactId() const;
		const FUeAgentCanonicalIdentity& GetTargetIdentity() const;
		EDurableArtifactKind GetArtifactKind() const;
		const FString& GetDurableLocation() const;
		const FString& GetPackageName() const;
		const FString& GetFinalPath() const;
		EAssetDurableIntent GetIntent() const;
		const FString& GetRole() const;
		bool IsValid() const;

	private:
		FString ArtifactId;
		FUeAgentCanonicalIdentity TargetIdentity;
		EDurableArtifactKind ArtifactKind =
			EDurableArtifactKind::Package;
		FString DurableLocation;
		FString PackageName;
		FString FinalPath;
		EAssetDurableIntent Intent = EAssetDurableIntent::Save;
		FString Role;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API FDurableInventory final
	{
	public:
		FDurableInventory(
			TArray<FDurableArtifact> InArtifacts = {},
			FString InInventoryHash = FString());

		const TArray<FDurableArtifact>& GetArtifacts() const;
		const FString& GetInventoryHash() const;
		bool IsValid() const;

	private:
		TArray<FDurableArtifact> Artifacts;
		FString InventoryHash;
	};

	class UEAGENTINTERFACE_API FDurableArtifactState final
	{
	public:
		FDurableArtifactState() = default;
		FDurableArtifactState(
			FString InArtifactId,
			EDurableStatePresence InPresence,
			FString InContentHash = FString());

		const FString& GetArtifactId() const;
		EDurableStatePresence GetPresence() const;
		const FString& GetContentHash() const;
		bool IsKnown() const;
		bool IsValid() const;
		bool Matches(const FDurableArtifactState& Other) const;

	private:
		FString ArtifactId;
		EDurableStatePresence Presence =
			EDurableStatePresence::Unknown;
		FString ContentHash;
	};

	class FAssetDurableSession;
	using FAssetDurableSessionPtr =
		TSharedPtr<const FAssetDurableSession>;
	using FAssetDurableSessionRef =
		TSharedRef<const FAssetDurableSession>;

	class UEAGENTINTERFACE_API FAssetDurableSession final
	{
	public:
		FAssetDurableSession(
			FAssetDurableRequest InRequest,
			FDurableInventory InInventory,
			FString InJournalId,
			FString InJournalPath,
			FString InJournalRoot,
			FString InTransactionRoot,
			FString InStagingRoot,
			TArray<FString> InPhaseHistory,
			TArray<FDurableArtifactState> InPreMutationStates = {},
			TArray<FDurableArtifactState> InExpectedPersistedStates = {});

		const FAssetDurableRequest& GetRequest() const;
		const FDurableInventory& GetInventory() const;
		const FString& GetJournalId() const;
		const FString& GetJournalPath() const;
		const FString& GetJournalRoot() const;
		const FString& GetTransactionRoot() const;
		const FString& GetStagingRoot() const;
		const TArray<FString>& GetPhaseHistory() const;
		const FString& GetCurrentPhase() const;
		const TArray<FDurableArtifactState>& GetPreMutationStates() const;
		const TArray<FDurableArtifactState>& GetExpectedPersistedStates() const;

	private:
		FAssetDurableRequest Request;
		FDurableInventory Inventory;
		FString JournalId;
		FString JournalPath;
		FString JournalRoot;
		FString TransactionRoot;
		FString StagingRoot;
		TArray<FString> PhaseHistory;
		TArray<FDurableArtifactState> PreMutationStates;
		TArray<FDurableArtifactState> ExpectedPersistedStates;
	};

	class UEAGENTINTERFACE_API FDurableReceipt final
	{
	public:
		FDurableReceipt() = default;
		FDurableReceipt(
			FString InJournalId,
			EDurableStatus InStatus,
			FString InPlanFingerprint,
			FString InInventoryHash,
			TArray<FString> InPhaseHistory,
			ERecoveryAction InRecoveryAction,
			bool bInRequiresIndependentVerification,
			bool bInReplayed,
			FString InFailureCode = FString(),
			FString InFailureDetail = FString(),
			FString InVerificationId = FString(),
			TArray<FDurableArtifactState> InPreMutationStates = {},
			TArray<FDurableArtifactState> InExpectedPersistedStates = {});

		const FString& GetJournalId() const;
		EDurableStatus GetStatus() const;
		const FString& GetPlanFingerprint() const;
		const FString& GetInventoryHash() const;
		const TArray<FString>& GetPhaseHistory() const;
		ERecoveryAction GetRecoveryAction() const;
		bool RequiresIndependentVerification() const;
		bool IsReplayed() const;
		const FString& GetFailureCode() const;
		const FString& GetFailureDetail() const;
		const FString& GetVerificationId() const;
		const TArray<FDurableArtifactState>& GetPreMutationStates() const;
		const TArray<FDurableArtifactState>& GetExpectedPersistedStates() const;

	private:
		FString JournalId;
		EDurableStatus Status = EDurableStatus::Prepared;
		FString PlanFingerprint;
		FString InventoryHash;
		TArray<FString> PhaseHistory;
		ERecoveryAction RecoveryAction = ERecoveryAction::None;
		bool bRequiresIndependentVerification = false;
		bool bReplayed = false;
		FString FailureCode;
		FString FailureDetail;
		FString VerificationId;
		TArray<FDurableArtifactState> PreMutationStates;
		TArray<FDurableArtifactState> ExpectedPersistedStates;
	};

	class UEAGENTINTERFACE_API FDurableVerificationAcknowledgement final
	{
	public:
		FDurableVerificationAcknowledgement() = default;
		FDurableVerificationAcknowledgement(
			FString InJournalId,
			FString InPlanFingerprint,
			FString InInventoryHash,
			FString InVerificationId,
			FString InJournalOwnerId,
			int64 InFencingToken);

		const FString& GetJournalId() const;
		const FString& GetPlanFingerprint() const;
		const FString& GetInventoryHash() const;
		const FString& GetVerificationId() const;
		const FString& GetJournalOwnerId() const;
		int64 GetFencingToken() const;
		bool IsValid() const;

	private:
		FString JournalId;
		FString PlanFingerprint;
		FString InventoryHash;
		FString VerificationId;
		FString JournalOwnerId;
		int64 FencingToken = -1;
	};

	class UEAGENTINTERFACE_API IDurableVerificationAcknowledgerPort
	{
	public:
		virtual ~IDurableVerificationAcknowledgerPort() = default;

		virtual bool TryAcknowledge(
			const FDurableVerificationAcknowledgement& Acknowledgement,
			FUeAgentResourceTracker& Resources,
			FAssetDurableError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API IAssetDurableBackend
	{
	public:
		virtual ~IAssetDurableBackend() = default;

		virtual TArray<FString> GetAllowedArtifactRoots() const = 0;
		virtual bool SupportsAtomicity(
			EAtomicityMode Mode) const = 0;
		virtual bool BuildInventory(
			const FAssetDurableRequest& Request,
			FUeAgentResourceTracker& Resources,
			FDurableInventory& OutInventory,
			FAssetDurableError& OutError) = 0;
		virtual bool CaptureBackups(
			const FAssetDurableSessionRef& Session,
			FUeAgentResourceTracker& Resources,
			FAssetDurableError& OutError) = 0;
		virtual bool Stage(
			const FAssetDurableSessionRef& Session,
			FUeAgentResourceTracker& Resources,
			FAssetDurableError& OutError) = 0;
		virtual bool Publish(
			const FAssetDurableSessionRef& Session,
			FUeAgentResourceTracker& Resources,
			FAssetDurableError& OutError) = 0;
		virtual bool Restore(
			const FAssetDurableSessionRef& Session,
			FUeAgentResourceTracker& Resources,
			bool& bOutConfirmed,
			FAssetDurableError& OutError) = 0;
		virtual bool Inspect(
			const FAssetDurableSessionRef& Session,
			FUeAgentResourceTracker& Resources,
			FString& OutObservedState,
			FAssetDurableError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API IAssetDurableReconciliationOracle
	{
	public:
		virtual ~IAssetDurableReconciliationOracle() = default;

		virtual bool TryConfirm(
			const FAssetDurableSessionRef& Session,
			const TArray<FDurableArtifactState>& ObservedStates,
			EDurableStatus ProposedStatus,
			FUeAgentResourceTracker& Resources,
			FAssetDurableError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API FPackageAssetDurableBackend final
		: public IAssetDurableBackend
	{
	public:
		explicit FPackageAssetDurableBackend(
			FString InAllowedContentRoot = FString());

		virtual TArray<FString> GetAllowedArtifactRoots() const override;
		virtual bool SupportsAtomicity(
			EAtomicityMode Mode) const override;
		virtual bool BuildInventory(
			const FAssetDurableRequest& Request,
			FUeAgentResourceTracker& Resources,
			FDurableInventory& OutInventory,
			FAssetDurableError& OutError) override;
		virtual bool CaptureBackups(
			const FAssetDurableSessionRef& Session,
			FUeAgentResourceTracker& Resources,
			FAssetDurableError& OutError) override;
		virtual bool Stage(
			const FAssetDurableSessionRef& Session,
			FUeAgentResourceTracker& Resources,
			FAssetDurableError& OutError) override;
		virtual bool Publish(
			const FAssetDurableSessionRef& Session,
			FUeAgentResourceTracker& Resources,
			FAssetDurableError& OutError) override;
		virtual bool Restore(
			const FAssetDurableSessionRef& Session,
			FUeAgentResourceTracker& Resources,
			bool& bOutConfirmed,
			FAssetDurableError& OutError) override;
		virtual bool Inspect(
			const FAssetDurableSessionRef& Session,
			FUeAgentResourceTracker& Resources,
			FString& OutObservedState,
			FAssetDurableError& OutError) override;

	private:
		FString AllowedContentRoot;
	};

	class UEAGENTINTERFACE_API
		IProjectConfigDurableStageSource
	{
	public:
		virtual ~IProjectConfigDurableStageSource() = default;

		virtual bool TryWriteStagedArtifact(
			const FAssetDurableTarget& Target,
			const FString& StagedPath,
			FUeAgentResourceTracker& Resources,
			FAssetDurableError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API
		FProjectConfigAssetDurableBackend final
		: public IAssetDurableBackend
	{
	public:
		FProjectConfigAssetDurableBackend(
			TArray<FString> InAllowedConfigRoots,
			TSharedRef<
				IProjectConfigDurableStageSource,
				ESPMode::ThreadSafe> InStageSource);

		virtual TArray<FString> GetAllowedArtifactRoots() const override;
		virtual bool SupportsAtomicity(
			EAtomicityMode Mode) const override;
		virtual bool BuildInventory(
			const FAssetDurableRequest& Request,
			FUeAgentResourceTracker& Resources,
			FDurableInventory& OutInventory,
			FAssetDurableError& OutError) override;
		virtual bool CaptureBackups(
			const FAssetDurableSessionRef& Session,
			FUeAgentResourceTracker& Resources,
			FAssetDurableError& OutError) override;
		virtual bool Stage(
			const FAssetDurableSessionRef& Session,
			FUeAgentResourceTracker& Resources,
			FAssetDurableError& OutError) override;
		virtual bool Publish(
			const FAssetDurableSessionRef& Session,
			FUeAgentResourceTracker& Resources,
			FAssetDurableError& OutError) override;
		virtual bool Restore(
			const FAssetDurableSessionRef& Session,
			FUeAgentResourceTracker& Resources,
			bool& bOutConfirmed,
			FAssetDurableError& OutError) override;
		virtual bool Inspect(
			const FAssetDurableSessionRef& Session,
			FUeAgentResourceTracker& Resources,
			FString& OutObservedState,
			FAssetDurableError& OutError) override;

	private:
		TArray<FString> AllowedConfigRoots;
		TSharedRef<
			IProjectConfigDurableStageSource,
			ESPMode::ThreadSafe> StageSource;
	};

	UEAGENTINTERFACE_API bool TryComputeDurableInventoryHash(
		const TArray<FDurableArtifact>& Artifacts,
		FUeAgentResourceTracker& Resources,
		FString& OutInventoryHash,
		FAssetDurableError& OutError);

	UEAGENTINTERFACE_API bool TryAcquireAssetDurableFence(
		const FString& JournalRoot,
		const FString& StableJournalOwnerId,
		ContextResource::FUeAgentResourceTracker& Resources,
		int64& OutFencingToken,
		FAssetDurableError& OutError);

	UEAGENTINTERFACE_API bool TryPrepareAssetDurability(
		const FAssetDurableRequest& Request,
		IAssetDurableBackend& Backend,
		const FString& JournalRoot,
		FUeAgentResourceTracker& Resources,
		FAssetDurableSessionPtr& OutSession,
		FAssetDurableError& OutError);

	UEAGENTINTERFACE_API bool TryPersistAssetDurability(
		const FAssetDurableSessionRef& Session,
		IAssetDurableBackend& Backend,
		int64 CurrentFencingToken,
		FUeAgentResourceTracker& Resources,
		FDurableReceipt& OutReceipt,
		FAssetDurableError& OutError);

	UEAGENTINTERFACE_API bool TryEnlistAssetDurableTargets(
		const FAssetDurableSessionRef& Session,
		const TArray<FAssetDurableTarget>& MutationTargets,
		IAssetDurableBackend& Backend,
		int64 CurrentFencingToken,
		FUeAgentResourceTracker& Resources,
		FAssetDurableSessionPtr& OutSession,
		FAssetDurableError& OutError);

	UEAGENTINTERFACE_API bool TryRollbackAssetDurability(
		const FAssetDurableSessionRef& Session,
		IAssetDurableBackend& Backend,
		int64 CurrentFencingToken,
		bool bMemoryRollbackConfirmed,
		FUeAgentResourceTracker& Resources,
		FDurableReceipt& OutReceipt,
		FAssetDurableError& OutError);

	UEAGENTINTERFACE_API bool TryAcknowledgeAssetDurability(
		const FAssetDurableSessionRef& Session,
		const FDurableVerificationAcknowledgement& Acknowledgement,
		int64 CurrentFencingToken,
		FUeAgentResourceTracker& Resources,
		FAssetDurableError& OutError);

	UEAGENTINTERFACE_API bool TryRecoverAssetDurability(
		const FString& JournalRoot,
		IAssetDurableBackend& Backend,
		int64 CurrentFencingToken,
		const FString& CurrentJournalOwnerId,
		FUeAgentResourceTracker& Resources,
		TArray<FDurableReceipt>& OutReceipts,
		FAssetDurableError& OutError);

	UEAGENTINTERFACE_API bool TryInspectAssetDurableJournal(
		const FString& JournalRoot,
		const FString& JournalId,
		const TArray<FString>& AllowedArtifactRoots,
		FUeAgentResourceTracker& Resources,
		FAssetDurableSessionPtr& OutSession,
		TArray<FDurableArtifactState>& OutObservedStates,
		FAssetDurableError& OutError);

	UEAGENTINTERFACE_API bool TryReconcileAssetDurability(
		const FString& JournalRoot,
		const FString& JournalId,
		const TArray<FString>& AllowedArtifactRoots,
		const FString& ExpectedJournalOwnerId,
		int64 ExpectedFencingToken,
		const FString& VerificationId,
		IAssetDurableReconciliationOracle& ReconciliationOracle,
		FUeAgentResourceTracker& Resources,
		FDurableReceipt& OutReceipt,
		FAssetDurableError& OutError);
}

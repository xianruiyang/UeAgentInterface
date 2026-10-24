#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::BindingGateway::Operation
{
	enum class EOperationLifecycleStage : uint8
	{
		Planning,
		Authorization,
		PreExecuteFence,
		DomainExecute,
		Settle,
		PrePersistFence,
		Persist,
		IndependentVerify,
		Rollback,
		Terminal,
	};

	enum class EOperationLifecycleStatus : uint8
	{
		Succeeded,
		Rejected,
		RolledBack,
		StateUnknown,
	};

	UEAGENTINTERFACE_API FString LexToString(EOperationLifecycleStage Stage);
	UEAGENTINTERFACE_API FString LexToString(EOperationLifecycleStatus Status);

	using FOperationLifecycleError = UE::UAI::Contract::FDiagnostic;

	class UEAGENTINTERFACE_API FOperationLifecyclePlan final
	{
	public:
		FOperationLifecyclePlan(
			FString InPlanId,
			FString InPlanFingerprint,
			TArray<FString> InImpactScope,
			TMap<FString, FString> InExpectedRevisions,
			TArray<FString> InRequiredSettleSignals);

		const FString& GetPlanId() const;
		const FString& GetPlanFingerprint() const;
		const TArray<FString>& GetImpactScope() const;
		const TMap<FString, FString>& GetExpectedRevisions() const;
		const TArray<FString>& GetRequiredSettleSignals() const;
		bool IsValid() const;
		const FOperationLifecycleError& GetValidationDiagnostic() const { return ValidationDiagnostic; }

	private:
		FString PlanId;
		FString PlanFingerprint;
		TArray<FString> ImpactScope;
		TMap<FString, FString> ExpectedRevisions;
		TArray<FString> RequiredSettleSignals;
		FOperationLifecycleError ValidationDiagnostic;
	};

	class UEAGENTINTERFACE_API FOperationImpactAuthorization final
	{
	public:
		FOperationImpactAuthorization(
			FString InPlanFingerprint,
			TArray<FString> InAuthorizedImpactScope);

		const FString& GetPlanFingerprint() const;
		const TArray<FString>& GetAuthorizedImpactScope() const;
		bool Authorizes(const FOperationLifecyclePlan& Plan) const;
		bool TryAuthorize(const FOperationLifecyclePlan& Plan, FOperationLifecycleError& OutError) const;
		bool IsValid() const;
		const FOperationLifecycleError& GetValidationDiagnostic() const { return ValidationDiagnostic; }

	private:
		FString PlanFingerprint;
		TArray<FString> AuthorizedImpactScope;
		FOperationLifecycleError ValidationDiagnostic;
	};

	class UEAGENTINTERFACE_API FOperationMutationFacts final
	{
	public:
		FOperationMutationFacts();
		FOperationMutationFacts(
			FString InMutationId,
			TArray<FString> InMutatedIdentityKeys,
			bool bInRollbackSupported);

		const FString& GetMutationId() const;
		const TArray<FString>& GetMutatedIdentityKeys() const;
		bool IsRollbackSupported() const;
		bool IsProvided() const { return bProvided; }
		bool IsValid() const;
		bool TryValidate(FOperationLifecycleError& OutError) const;
		const FOperationLifecycleError& GetValidationDiagnostic() const { return ValidationDiagnostic; }

	private:
		FString MutationId;
		TArray<FString> MutatedIdentityKeys;
		bool bRollbackSupported = false;
		bool bProvided = false;
		FOperationLifecycleError ValidationDiagnostic;
	};

	class UEAGENTINTERFACE_API IOperationVersionFencePort
	{
	public:
		virtual ~IOperationVersionFencePort() = default;
		virtual bool Check(
			const FOperationLifecyclePlan& Plan,
			EOperationLifecycleStage Stage,
			bool& bOutCurrent,
			FOperationLifecycleError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API IOperationDomainMutationPort
	{
	public:
		virtual ~IOperationDomainMutationPort() = default;
		virtual bool Execute(
			const FOperationLifecyclePlan& Plan,
			FOperationMutationFacts& OutFacts,
			FOperationLifecycleError& OutError) = 0;
		virtual bool Rollback(
			const FOperationLifecyclePlan& Plan,
			const FOperationMutationFacts& Facts,
			bool& bOutConfirmed,
			FOperationLifecycleError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API IOperationStabilityPort
	{
	public:
		virtual ~IOperationStabilityPort() = default;
		virtual bool Settle(
			const FOperationLifecyclePlan& Plan,
			const FOperationMutationFacts& Facts,
			FOperationLifecycleError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API IOperationPersistencePort
	{
	public:
		virtual ~IOperationPersistencePort() = default;
		virtual bool Persist(
			const FOperationLifecyclePlan& Plan,
			const FOperationMutationFacts& Facts,
			FString& OutPersistenceReceiptId,
			FOperationLifecycleError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API IOperationIndependentVerificationPort
	{
	public:
		virtual ~IOperationIndependentVerificationPort() = default;
		virtual bool Verify(
			const FOperationLifecyclePlan& Plan,
			const FOperationMutationFacts& Facts,
			const FString& PersistenceReceiptId,
			bool& bOutEquivalent,
			FOperationLifecycleError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API FOperationLifecycleReceipt final
	{
	public:
		FOperationLifecycleReceipt() = default;
		FOperationLifecycleReceipt(
			FString InPlanId,
			EOperationLifecycleStatus InStatus,
			UE::UAI::Contract::FDiagnostic InDiagnostic,
			bool bInMutationStarted,
			bool bInRetrySafe,
			FString InPersistenceReceiptId,
			TArray<FString> InPhaseHistory);

		const FString& GetPlanId() const;
		EOperationLifecycleStatus GetStatus() const;
		const UE::UAI::Contract::FDiagnostic& GetDiagnostic() const;
		const FString& GetErrorCode() const { return Diagnostic.Code; }
		bool DidMutationStart() const;
		bool IsRetrySafe() const;
		const FString& GetPersistenceReceiptId() const;
		const TArray<FString>& GetPhaseHistory() const;
		bool IsValid() const;

	private:
		FString PlanId;
		EOperationLifecycleStatus Status =
			EOperationLifecycleStatus::Rejected;
		UE::UAI::Contract::FDiagnostic Diagnostic;
		bool bMutationStarted = false;
		bool bRetrySafe = false;
		FString PersistenceReceiptId;
		TArray<FString> PhaseHistory;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API FOperationLifecycleService final
	{
	public:
		FOperationLifecycleService(
			TSharedRef<IOperationVersionFencePort> InVersionFence,
			TSharedRef<IOperationDomainMutationPort> InDomainMutation,
			TSharedRef<IOperationStabilityPort> InStability,
			TSharedRef<IOperationPersistencePort> InPersistence,
			TSharedRef<IOperationIndependentVerificationPort>
				InIndependentVerification);

		bool Run(
			const FOperationLifecyclePlan& Plan,
			const FOperationImpactAuthorization& Authorization,
			FOperationLifecycleReceipt& OutReceipt) const;

	private:
		bool Rollback(
			const FOperationLifecyclePlan& Plan,
			const FOperationMutationFacts& Facts,
			UE::UAI::Contract::FDiagnostic OriginalDiagnostic,
			TArray<FString> PhaseHistory,
			FOperationLifecycleReceipt& OutReceipt) const;

		TSharedRef<IOperationVersionFencePort> VersionFence;
		TSharedRef<IOperationDomainMutationPort> DomainMutation;
		TSharedRef<IOperationStabilityPort> Stability;
		TSharedRef<IOperationPersistencePort> Persistence;
		TSharedRef<IOperationIndependentVerificationPort>
			IndependentVerification;
	};
}

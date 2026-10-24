#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"

namespace UE::UAI::BindingGateway::Ticket
{
	class FUeAgentBindingTicketJsonCodec;
	using UE::UAI::ContextResource::FUeAgentMonotonicClock;
	using UE::UAI::Contract::FUeAgentCallerContext;
	using UE::UAI::Contract::FUeAgentDescriptorIdentity;
	using UE::UAI::Discovery::FUeAgentCandidateBinding;
	using UE::UAI::Discovery::FUeAgentCatalogRevision;
	using UE::UAI::StateSemantic::FUeAgentStateRevision;

	using FUeAgentOperationTicketError = UE::UAI::Contract::FDiagnostic;

	class UEAGENTINTERFACE_API FUeAgentOperationSessionIdentity final
	{
	public:
		FUeAgentOperationSessionIdentity(
			FString InServerSessionId,
			int64 InRegistryEpoch,
			FString InRegistryHash);

		const FString& GetServerSessionId() const;
		int64 GetRegistryEpoch() const;
		const FString& GetRegistryHash() const;
		bool IsValid() const;
		const FUeAgentOperationTicketError& GetValidationDiagnostic() const;
		bool operator==(
			const FUeAgentOperationSessionIdentity& Other) const;

	private:
		FString ServerSessionId;
		int64 RegistryEpoch = -1;
		FString RegistryHash;
		FUeAgentOperationTicketError ValidationDiagnostic;
	};

	class UEAGENTINTERFACE_API FUeAgentOperationTicket final
	{
	public:
		const FString& GetTicketId() const;
		const FUeAgentDescriptorIdentity& GetDescriptorIdentity() const;
		const UE::UAI::StateSemantic::FUeAgentCanonicalIdentity&
			GetCandidateIdentity() const;
		const FString& GetCandidateBindingId() const;
		const FString& GetCandidateFingerprint() const;
		const FUeAgentCatalogRevision& GetCatalogRevision() const;
		const FString& GetServerSessionId() const;
		int64 GetRegistryEpoch() const;
		const FString& GetRegistryHash() const;
		const FString& GetContextFingerprint() const;
		const FString& GetSignature() const;
		double GetIssuedAt() const;
		double GetExpiresAt() const;
		const TOptional<FUeAgentStateRevision>&
			GetTargetRevision() const;
		bool IsValid() const;
		const FUeAgentOperationTicketError& GetValidationDiagnostic() const;

	private:
		FUeAgentOperationTicket(
			FString InTicketId,
			FUeAgentDescriptorIdentity InDescriptorIdentity,
			UE::UAI::StateSemantic::FUeAgentCanonicalIdentity
				InCandidateIdentity,
			FString InCandidateBindingId,
			FString InCandidateFingerprint,
			FUeAgentCatalogRevision InCatalogRevision,
			FString InServerSessionId,
			int64 InRegistryEpoch,
			FString InRegistryHash,
			FString InContextFingerprint,
			FString InSignature,
			double InIssuedAt,
			double InExpiresAt,
			TOptional<FUeAgentStateRevision> InTargetRevision);

		FString TicketId;
		FUeAgentDescriptorIdentity DescriptorIdentity;
		UE::UAI::StateSemantic::FUeAgentCanonicalIdentity
			CandidateIdentity;
		FString CandidateBindingId;
		FString CandidateFingerprint;
		FUeAgentCatalogRevision CatalogRevision;
		FString ServerSessionId;
		int64 RegistryEpoch = -1;
		FString RegistryHash;
		FString ContextFingerprint;
		FString Signature;
		double IssuedAt = -1.0;
		double ExpiresAt = -1.0;
		TOptional<FUeAgentStateRevision> TargetRevision;
		FUeAgentOperationTicketError ValidationDiagnostic;

		friend class FUeAgentOperationTicketService;
		friend class FUeAgentBindingTicketJsonCodec;
	};

	using FUeAgentOperationTicketIdFactory = TFunction<FString()>;

	class UEAGENTINTERFACE_API FUeAgentOperationTicketService final
	{
	public:
		static bool TryCreate(
			const FString& IntegrityKey,
			double TtlSeconds,
			FUeAgentMonotonicClock Clock,
			FUeAgentOperationTicketIdFactory TicketIdFactory,
			TSharedPtr<const FUeAgentOperationTicketService>& OutService,
			FUeAgentOperationTicketError& OutError);

		bool TryIssue(
			const FUeAgentCandidateBinding& Binding,
			const FUeAgentDescriptorIdentity& DescriptorIdentity,
			const FUeAgentOperationSessionIdentity& Session,
			const FUeAgentCallerContext& CallerContext,
			TOptional<FUeAgentStateRevision> TargetRevision,
			TOptional<FUeAgentOperationTicket>& OutTicket,
			FUeAgentOperationTicketError& OutError) const;

		bool TryValidate(
			const FUeAgentOperationTicket& Ticket,
			const FUeAgentCandidateBinding& Binding,
			const FUeAgentDescriptorIdentity& DescriptorIdentity,
			const FUeAgentOperationSessionIdentity& Session,
			const FUeAgentCallerContext& CallerContext,
			FUeAgentOperationTicketError& OutError) const;

	private:
		FUeAgentOperationTicketService(
			FString InIntegrityKey,
			double InTtlSeconds,
			FUeAgentMonotonicClock InClock,
			FUeAgentOperationTicketIdFactory InTicketIdFactory);

		FString CandidateFingerprint(
			const FUeAgentCandidateBinding& Binding,
			const FUeAgentDescriptorIdentity& DescriptorIdentity) const;
		FString ContextFingerprint(
			const FUeAgentCallerContext& CallerContext) const;
		FString Sign(const FUeAgentOperationTicket& Ticket) const;

		FString IntegrityKey;
		double TtlSeconds = 0.0;
		FUeAgentMonotonicClock Clock;
		FUeAgentOperationTicketIdFactory TicketIdFactory;
	};
}

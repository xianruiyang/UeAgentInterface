#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::DomainAdapters::GenericRead
{
	using UE::UAI::ContextResource::FUeAgentCancellationToken;
	using UE::UAI::ContextResource::FUeAgentOperationDeadline;
	using UE::UAI::ContextResource::FUeAgentResourceTracker;
	using UE::UAI::Discovery::FUeAgentDiscoveryError;
	using UE::UAI::Discovery::FUeAgentStateReadRequest;
	using UE::UAI::Discovery::IUeAgentStateReadPort;
	using UE::UAI::StateSemantic::EUeAgentStatePlane;
	using UE::UAI::StateSemantic::FUeAgentCanonicalIdentity;
	using UE::UAI::StateSemantic::FUeAgentCanonicalSnapshot;
	using UE::UAI::StateSemantic::FUeAgentCanonicalType;
	using UE::UAI::StateSemantic::FUeAgentCanonicalValueRef;
	using UE::UAI::StateSemantic::FUeAgentDefaultValue;
	using UE::UAI::StateSemantic::FUeAgentStateDetails;
	using UE::UAI::StateSemantic::FUeAgentStateRevision;
	using UE::UAI::StateSemantic::FUeAgentTemporalPoint;

	using FUeAgentGenericReadError = UE::UAI::Contract::FDiagnostic;

	class UEAGENTINTERFACE_API FUeAgentUeTypeBinding final
	{
	public:
		FUeAgentUeTypeBinding(
			FString InSourceTypeId,
			FUeAgentCanonicalType InCanonicalType,
			FString InBindingEvidence);

		const FString& GetSourceTypeId() const;
		const FUeAgentCanonicalType& GetCanonicalType() const;
		const FString& GetBindingEvidence() const;
		bool IsValid() const;
		const FUeAgentGenericReadError& GetValidationDiagnostic() const;

	private:
		FString SourceTypeId;
		FUeAgentCanonicalType CanonicalType;
		FString BindingEvidence;
		bool bValid = false;
		TSharedPtr<const FUeAgentGenericReadError> ValidationDiagnostic;
	};

	class UEAGENTINTERFACE_API FUeAgentUeReadEntry final
	{
	public:
		FUeAgentUeReadEntry(
			FUeAgentCanonicalIdentity InIdentity,
			FUeAgentUeTypeBinding InTypeBinding,
			FUeAgentCanonicalValueRef InValue,
			FUeAgentDefaultValue InDefault,
			FUeAgentStateDetails InDetails,
			FUeAgentStateRevision InObservedRevision);

		const FUeAgentCanonicalIdentity& GetIdentity() const;
		const FUeAgentUeTypeBinding& GetTypeBinding() const;
		const FUeAgentCanonicalValueRef& GetValue() const;
		const FUeAgentDefaultValue& GetDefault() const;
		const FUeAgentStateDetails& GetDetails() const;
		const FUeAgentStateRevision& GetObservedRevision() const;
		bool IsValid() const;
		const FUeAgentGenericReadError& GetValidationDiagnostic() const;

	private:
		FUeAgentCanonicalIdentity Identity;
		FUeAgentUeTypeBinding TypeBinding;
		FUeAgentCanonicalValueRef Value;
		FUeAgentDefaultValue Default;
		FUeAgentStateDetails Details;
		FUeAgentStateRevision ObservedRevision;
		bool bValid = false;
		TSharedPtr<const FUeAgentGenericReadError> ValidationDiagnostic;
	};

	class UEAGENTINTERFACE_API FUeAgentUeReadCapture final
	{
	public:
		FUeAgentUeReadCapture(
			FString InSnapshotId,
			EUeAgentStatePlane InStatePlane,
			FUeAgentStateRevision InRevision,
			FUeAgentTemporalPoint InCapturedAt,
			FString InEvidenceId,
			bool bInSettled,
			TArray<FUeAgentUeReadEntry> InEntries);

		const FString& GetSnapshotId() const;
		EUeAgentStatePlane GetStatePlane() const;
		const FUeAgentStateRevision& GetRevision() const;
		const FUeAgentTemporalPoint& GetCapturedAt() const;
		const FString& GetEvidenceId() const;
		bool IsSettled() const;
		const TArray<FUeAgentUeReadEntry>& GetEntries() const;
		bool IsValid() const;
		const FUeAgentGenericReadError& GetValidationDiagnostic() const;

	private:
		FString SnapshotId;
		EUeAgentStatePlane StatePlane = EUeAgentStatePlane::AuthoringMemory;
		FUeAgentStateRevision Revision;
		FUeAgentTemporalPoint CapturedAt;
		FString EvidenceId;
		bool bSettled = false;
		TArray<FUeAgentUeReadEntry> Entries;
		bool bValid = false;
		TSharedPtr<const FUeAgentGenericReadError> ValidationDiagnostic;
	};

	class UEAGENTINTERFACE_API IUeAgentUeReadHook
	{
	public:
		virtual ~IUeAgentUeReadHook() = default;

		virtual bool TryCapture(
			const FUeAgentStateReadRequest& Request,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			TSharedPtr<const FUeAgentUeReadCapture>& OutCapture,
			FUeAgentGenericReadError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API FUeAgentGenericReadAdapter final
		: public IUeAgentStateReadPort
	{
	public:
		FUeAgentGenericReadAdapter(
			FString InPortId,
			TSharedRef<IUeAgentUeReadHook> InHook);

		const FString& GetPortId() const override;
		bool TryRead(
			const FUeAgentStateReadRequest& Request,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
			FUeAgentDiscoveryError& OutError) override;

	private:
		FString PortId;
		TSharedRef<IUeAgentUeReadHook> Hook;
	};
}

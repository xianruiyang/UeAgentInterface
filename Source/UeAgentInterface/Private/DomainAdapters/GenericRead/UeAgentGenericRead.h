#pragma once

#include "CoreMinimal.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "DomainAdapters/GenericRead/UeAgentGenericReadAdapter.h"

class UObject;

namespace UE::UAI::DomainAdapters::GenericRead
{
	enum class EUeAgentGenericValueKind : uint8
	{
		DecimalLexeme,
		SpatialPoint,
	};

	struct FUeAgentGenericReadDiagnostic
	{
		FString Code;
		FString Message;
		FString SubjectId;
	};

	struct FUeAgentGenericIdentity
	{
		FString SemanticId;
		FString TargetKind;
		FString OwnerId;
		FString Selector;
		FString ObjectRole;
		FString Generation;
	};

	struct FUeAgentGenericSpatialBinding
	{
		FString Subject;
		FString FrameId;
		FString BasisFrameId;
		FString Unit;
		FString BasisEvidence;
	};

	struct FUeAgentGenericPropertyBinding
	{
		FString PropertyPath;
		FUeAgentGenericIdentity Identity;
		FString CanonicalTypeId;
		EUeAgentGenericValueKind ValueKind =
			EUeAgentGenericValueKind::DecimalLexeme;
		FString BindingEvidence;
		FString DefaultSourceIdentity;
		FString DefaultProvenance;
		FUeAgentGenericSpatialBinding Spatial;
	};

	struct FUeAgentGenericReadRequest
	{
		UObject* RootObject = nullptr;
		FString SnapshotId;
		FString StatePlane;
		FString SourceId;
		FString Generation;
		int64 Sequence = 0;
		TArray<FUeAgentGenericPropertyBinding> Properties;
	};

	struct FUeAgentGenericTypeBinding
	{
		FString SourceTypeId;
		FString CanonicalTypeId;
		EUeAgentGenericValueKind ValueKind =
			EUeAgentGenericValueKind::DecimalLexeme;
		FString BindingEvidence;
	};

	struct FUeAgentGenericSpatialValue
	{
		FString Subject;
		FString FrameId;
		FString BasisFrameId;
		FString Unit;
		FString BasisEvidence;
		TArray<FString> Components;
		TArray<FString> DefaultComponents;
	};

	struct FUeAgentGenericReadEntry
	{
		FUeAgentGenericIdentity Identity;
		FUeAgentGenericTypeBinding TypeBinding;
		FString ValueLexeme;
		FString DefaultLexeme;
		FString DefaultSourceIdentity;
		FString DefaultProvenance;
		FString DisplayName;
		FString Tooltip;
		TArray<FString> CategoryPath;
		bool bEditable = false;
		FString Availability;
		FUeAgentGenericSpatialValue Spatial;
	};

	struct FUeAgentGenericReadCapture
	{
		FString SnapshotId;
		FString StatePlane;
		FString SourceId;
		FString Generation;
		int64 Sequence = 0;
		uint64 CapturedAtCycles = 0;
		FString EvidenceId;
		bool bSettled = false;
		TArray<FUeAgentGenericReadEntry> Entries;
	};

	class UEAGENTINTERFACE_API FUeAgentReflectionGenericReadHook final
		: public IUeAgentUeReadHook
	{
	public:
		FUeAgentReflectionGenericReadHook() = default;
		explicit FUeAgentReflectionGenericReadHook(
			FUeAgentGenericReadRequest InReadTemplate);

		bool Capture(
			const FUeAgentGenericReadRequest& Request,
			const ContextResource::FUeAgentOperationDeadline& Deadline,
			const ContextResource::FUeAgentCancellationToken& Cancellation,
			FUeAgentGenericReadCapture& OutCapture,
			FUeAgentGenericReadDiagnostic& OutDiagnostic) const;

		bool TryCapture(
			const Discovery::FUeAgentStateReadRequest& Request,
			ContextResource::FUeAgentResourceTracker& Resources,
			const ContextResource::FUeAgentOperationDeadline& Deadline,
			const ContextResource::FUeAgentCancellationToken& Cancellation,
			TSharedPtr<const FUeAgentUeReadCapture>& OutCapture,
			FUeAgentGenericReadError& OutError) override;

	private:
		TOptional<FUeAgentGenericReadRequest> ReadTemplate;
	};
}

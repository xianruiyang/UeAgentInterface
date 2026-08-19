#pragma once

#include "CoreMinimal.h"

namespace UE::UAI::DomainAdapters::AudioMediaGraph
{
	enum class EAudioMediaGraphMutationIntent : uint8
	{
		Create,
		Update,
		Replace,
		Delete,
	};

	class UEAGENTINTERFACE_API FAudioMediaGraphError final
	{
	public:
		const FString& GetCode() const { return Code; }
		const FString& GetMessage() const { return Message; }
		void Reset();
		void Set(FString InCode, FString InMessage = FString());

	private:
		FString Code;
		FString Message;
	};

	class UEAGENTINTERFACE_API FAudioMediaGraphStableIdentity final
	{
	public:
		FAudioMediaGraphStableIdentity() = default;
		FAudioMediaGraphStableIdentity(
			FString InIdentitySource,
			FString InNativeKey,
			FString InTypeKey);

		const FString& GetIdentitySource() const { return IdentitySource; }
		const FString& GetNativeKey() const { return NativeKey; }
		const FString& GetTypeKey() const { return TypeKey; }

	private:
		FString IdentitySource;
		FString NativeKey;
		FString TypeKey;
	};

	class UEAGENTINTERFACE_API FAudioMediaGraphRelation final
	{
	public:
		FAudioMediaGraphRelation() = default;
		FAudioMediaGraphRelation(
			FString InFromNativeKey,
			FString InToNativeKey,
			FString InRelationKind,
			int32 InOrder);

		const FString& GetFromNativeKey() const { return FromNativeKey; }
		const FString& GetToNativeKey() const { return ToNativeKey; }
		const FString& GetRelationKind() const { return RelationKind; }
		int32 GetOrder() const { return Order; }

	private:
		FString FromNativeKey;
		FString ToNativeKey;
		FString RelationKind;
		int32 Order = INDEX_NONE;
	};

	enum class EAudioMediaGraphBarrierKind : uint8
	{
		Refresh,
		Compile,
		Evaluate,
		ArtifactCapture,
	};

	class UEAGENTINTERFACE_API FAudioMediaGraphLifecycleEvidence final
	{
	public:
		FAudioMediaGraphLifecycleEvidence(
			EAudioMediaGraphBarrierKind InBarrierKind,
			bool bInBarrierComplete,
			bool bInSettleComplete,
			bool bInIndependentCapture,
			FString InBeforeRevision,
			FString InApplyEvidenceId,
			FString InBarrierEvidenceId,
			FString InSettleEvidenceId,
			FString InReadbackRevision,
			FString InReadbackEvidenceId);

		EAudioMediaGraphBarrierKind GetBarrierKind() const
		{
			return BarrierKind;
		}
		bool IsBarrierComplete() const { return bBarrierComplete; }
		bool IsSettleComplete() const { return bSettleComplete; }
		bool HasIndependentCapture() const { return bIndependentCapture; }
		const FString& GetBeforeRevision() const { return BeforeRevision; }
		const FString& GetApplyEvidenceId() const { return ApplyEvidenceId; }
		const FString& GetBarrierEvidenceId() const { return BarrierEvidenceId; }
		const FString& GetSettleEvidenceId() const { return SettleEvidenceId; }
		const FString& GetReadbackRevision() const { return ReadbackRevision; }
		const FString& GetReadbackEvidenceId() const { return ReadbackEvidenceId; }

	private:
		EAudioMediaGraphBarrierKind BarrierKind;
		bool bBarrierComplete = false;
		bool bSettleComplete = false;
		bool bIndependentCapture = false;
		FString BeforeRevision;
		FString ApplyEvidenceId;
		FString BarrierEvidenceId;
		FString SettleEvidenceId;
		FString ReadbackRevision;
		FString ReadbackEvidenceId;
	};

	UEAGENTINTERFACE_API bool TryValidateAudioMediaGraphStableIdentity(
		const FAudioMediaGraphStableIdentity& Identity,
		FAudioMediaGraphError& OutError);

	UEAGENTINTERFACE_API bool TryValidateAudioMediaGraphAcyclicRelations(
		const TArray<FAudioMediaGraphStableIdentity>& Items,
		const TArray<FAudioMediaGraphRelation>& Relations,
		int32 OperationBudget,
		FAudioMediaGraphError& OutError);

	UEAGENTINTERFACE_API bool TryValidateAudioMediaGraphLifecycleEvidence(
		const FAudioMediaGraphLifecycleEvidence& Evidence,
		FAudioMediaGraphError& OutError);
}

#include "DomainAdapters/AudioMediaGraph/UeAgentAudioMediaGraphAdapter.h"

namespace UE::UAI::DomainAdapters::AudioMediaGraph
{
	void FAudioMediaGraphError::Reset()
	{
		Code.Reset();
		Message.Reset();
	}

	void FAudioMediaGraphError::Set(FString InCode, FString InMessage)
	{
		Code = MoveTemp(InCode);
		Message = MoveTemp(InMessage);
	}

	FAudioMediaGraphStableIdentity::FAudioMediaGraphStableIdentity(
		FString InIdentitySource,
		FString InNativeKey,
		FString InTypeKey)
		: IdentitySource(MoveTemp(InIdentitySource))
		, NativeKey(MoveTemp(InNativeKey))
		, TypeKey(MoveTemp(InTypeKey))
	{
	}

	FAudioMediaGraphRelation::FAudioMediaGraphRelation(
		FString InFromNativeKey,
		FString InToNativeKey,
		FString InRelationKind,
		const int32 InOrder)
		: FromNativeKey(MoveTemp(InFromNativeKey))
		, ToNativeKey(MoveTemp(InToNativeKey))
		, RelationKind(MoveTemp(InRelationKind))
		, Order(InOrder)
	{
	}

	FAudioMediaGraphLifecycleEvidence::FAudioMediaGraphLifecycleEvidence(
		const EAudioMediaGraphBarrierKind InBarrierKind,
		const bool bInBarrierComplete,
		const bool bInSettleComplete,
		const bool bInIndependentCapture,
		FString InBeforeRevision,
		FString InApplyEvidenceId,
		FString InBarrierEvidenceId,
		FString InSettleEvidenceId,
		FString InReadbackRevision,
		FString InReadbackEvidenceId)
		: BarrierKind(InBarrierKind)
		, bBarrierComplete(bInBarrierComplete)
		, bSettleComplete(bInSettleComplete)
		, bIndependentCapture(bInIndependentCapture)
		, BeforeRevision(MoveTemp(InBeforeRevision))
		, ApplyEvidenceId(MoveTemp(InApplyEvidenceId))
		, BarrierEvidenceId(MoveTemp(InBarrierEvidenceId))
		, SettleEvidenceId(MoveTemp(InSettleEvidenceId))
		, ReadbackRevision(MoveTemp(InReadbackRevision))
		, ReadbackEvidenceId(MoveTemp(InReadbackEvidenceId))
	{
	}

	bool TryValidateAudioMediaGraphStableIdentity(
		const FAudioMediaGraphStableIdentity& Identity,
		FAudioMediaGraphError& OutError)
	{
		OutError.Reset();
		if (Identity.GetIdentitySource().IsEmpty())
		{
			OutError.Set(TEXT("audio_media_graph_identity_source_missing"));
			return false;
		}
		if (Identity.GetNativeKey().IsEmpty())
		{
			OutError.Set(TEXT("audio_media_graph_native_identity_missing"));
			return false;
		}
		if (Identity.GetTypeKey().IsEmpty())
		{
			OutError.Set(TEXT("audio_media_graph_native_type_missing"));
			return false;
		}
		return true;
	}

	bool TryValidateAudioMediaGraphAcyclicRelations(
		const TArray<FAudioMediaGraphStableIdentity>& Items,
		const TArray<FAudioMediaGraphRelation>& Relations,
		const int32 OperationBudget,
		FAudioMediaGraphError& OutError)
	{
		OutError.Reset();
		TMap<FString, int32> IndexByKey;
		for (int32 Index = 0; Index < Items.Num(); ++Index)
		{
			if (!TryValidateAudioMediaGraphStableIdentity(Items[Index], OutError))
			{
				return false;
			}
			if (IndexByKey.Contains(Items[Index].GetNativeKey()))
			{
				OutError.Set(TEXT("audio_media_graph_duplicate_identity"));
				return false;
			}
			IndexByKey.Add(Items[Index].GetNativeKey(), Index);
		}

		if (OperationBudget < Items.Num() + Relations.Num())
		{
			OutError.Set(TEXT("audio_media_graph_operation_budget_exhausted"));
			return false;
		}

		TArray<TArray<int32>> Outgoing;
		Outgoing.SetNum(Items.Num());
		TArray<int32> InDegree;
		InDegree.Init(0, Items.Num());
		int32 Operations = Items.Num();
		for (const FAudioMediaGraphRelation& Relation : Relations)
		{
			const int32* From = IndexByKey.Find(Relation.GetFromNativeKey());
			const int32* To = IndexByKey.Find(Relation.GetToNativeKey());
			if (From == nullptr || To == nullptr)
			{
				OutError.Set(TEXT("audio_media_graph_endpoint_missing"));
				return false;
			}
			if (Relation.GetOrder() < 0)
			{
				OutError.Set(TEXT("audio_media_graph_relation_order_invalid"));
				return false;
			}
			Outgoing[*From].Add(*To);
			++InDegree[*To];
			if (++Operations > OperationBudget)
			{
				OutError.Set(TEXT("audio_media_graph_operation_budget_exhausted"));
				return false;
			}
		}

		TArray<int32> Queue;
		for (int32 Index = 0; Index < InDegree.Num(); ++Index)
		{
			if (InDegree[Index] == 0)
			{
				Queue.Add(Index);
			}
		}
		int32 Head = 0;
		int32 Visited = 0;
		while (Head < Queue.Num())
		{
			const int32 Current = Queue[Head++];
			++Visited;
			for (const int32 Next : Outgoing[Current])
			{
				if (++Operations > OperationBudget)
				{
					OutError.Set(TEXT("audio_media_graph_operation_budget_exhausted"));
					return false;
				}
				if (--InDegree[Next] == 0)
				{
					Queue.Add(Next);
				}
			}
		}
		if (Visited != Items.Num())
		{
			OutError.Set(TEXT("audio_media_graph_cycle"));
			return false;
		}
		return true;
	}

	bool TryValidateAudioMediaGraphLifecycleEvidence(
		const FAudioMediaGraphLifecycleEvidence& Evidence,
		FAudioMediaGraphError& OutError)
	{
		OutError.Reset();
		if (!Evidence.IsBarrierComplete())
		{
			OutError.Set(Evidence.GetBarrierKind()
				== EAudioMediaGraphBarrierKind::Evaluate
				? TEXT("audio_media_graph_evaluate_incomplete")
				: TEXT("audio_media_graph_compile_incomplete"));
			return false;
		}
		if (!Evidence.IsSettleComplete())
		{
			OutError.Set(TEXT("audio_media_graph_settle_incomplete"));
			return false;
		}
		if (!Evidence.HasIndependentCapture()
			|| Evidence.GetReadbackEvidenceId().IsEmpty())
		{
			OutError.Set(TEXT("audio_media_graph_capture_failed"));
			return false;
		}
		if (Evidence.GetBeforeRevision().IsEmpty()
			|| Evidence.GetReadbackRevision().IsEmpty()
			|| Evidence.GetBeforeRevision() == Evidence.GetReadbackRevision()
			|| Evidence.GetApplyEvidenceId().IsEmpty()
			|| Evidence.GetBarrierEvidenceId().IsEmpty()
			|| Evidence.GetSettleEvidenceId().IsEmpty())
		{
			OutError.Set(TEXT("audio_media_graph_capture_failed"));
			return false;
		}
		const TSet<FString> EvidenceIds = {
			Evidence.GetApplyEvidenceId(), Evidence.GetBarrierEvidenceId(),
			Evidence.GetSettleEvidenceId(), Evidence.GetReadbackEvidenceId()};
		if (EvidenceIds.Num() != 4)
		{
			OutError.Set(TEXT("audio_media_graph_echo_readback_forbidden"));
			return false;
		}
		return true;
	}
}

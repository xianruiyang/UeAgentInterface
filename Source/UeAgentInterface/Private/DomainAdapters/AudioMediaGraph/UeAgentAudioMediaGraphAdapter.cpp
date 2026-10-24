#include "DomainAdapters/AudioMediaGraph/UeAgentAudioMediaGraphAdapter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::DomainAdapters::AudioMediaGraph
{
	namespace
	{
		FAudioMediaGraphError AdapterFact(const FString& Code, const FString& Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("native_graph"));
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Path;
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			return Error;
		}

		TSharedPtr<FJsonValue> LifecycleIdentityEvidence(const FAudioMediaGraphLifecycleEvidence& Evidence)
		{
			auto Facts = MakeShared<FJsonObject>();
			Facts->SetStringField(TEXT("before_revision"), Evidence.GetBeforeRevision());
			Facts->SetStringField(TEXT("readback_revision"), Evidence.GetReadbackRevision());
			Facts->SetStringField(TEXT("apply_evidence"), Evidence.GetApplyEvidenceId());
			Facts->SetStringField(TEXT("barrier_evidence"), Evidence.GetBarrierEvidenceId());
			Facts->SetStringField(TEXT("settle_evidence"), Evidence.GetSettleEvidenceId());
			Facts->SetStringField(TEXT("readback_evidence"), Evidence.GetReadbackEvidenceId());
			return MakeShared<FJsonValueObject>(Facts);
		}
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
			OutError = AdapterFact(TEXT("audio_media_graph_identity_source_missing"), TEXT("The native graph item has no identity-source declaration."),
				TEXT("/identity_source/available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}
		if (Identity.GetNativeKey().IsEmpty())
		{
			OutError = AdapterFact(TEXT("audio_media_graph_native_identity_missing"), TEXT("The native graph item has no native identity key."),
				TEXT("/native_key/available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.InternalContext = Identity.GetIdentitySource();
			return false;
		}
		if (Identity.GetTypeKey().IsEmpty())
		{
			OutError = AdapterFact(TEXT("audio_media_graph_native_type_missing"), TEXT("The native graph item has no type key."),
				TEXT("/type_key/available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.SubjectId = Identity.GetNativeKey();
			return false;
		}
		return true;
	}

	bool TryValidateAudioMediaGraphAcyclicRelations(
		const TArray<FAudioMediaGraphStableIdentity>& Items,
		const TArray<FAudioMediaGraphRelation>& Relations,
		FAudioMediaGraphError& OutError)
	{
		OutError.Reset();
		TMap<FString, int32> IndexByKey;
		for (int32 Index = 0; Index < Items.Num(); ++Index)
		{
			if (!TryValidateAudioMediaGraphStableIdentity(Items[Index], OutError))
			{
				OutError.PropertyPath = FString::Printf(TEXT("/items/%d"), Index) + OutError.PropertyPath;
				return false;
			}
			if (IndexByKey.Contains(Items[Index].GetNativeKey()))
			{
				auto Actual = MakeShared<FJsonObject>();
				Actual->SetNumberField(TEXT("first_index"), IndexByKey.FindChecked(Items[Index].GetNativeKey()));
				Actual->SetNumberField(TEXT("duplicate_index"), Index);
				OutError = AdapterFact(TEXT("audio_media_graph_duplicate_identity"), TEXT("Two native graph items use the same identity key."),
					FString::Printf(TEXT("/items/%d/native_key"), Index), MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("unique native identity")));
				OutError.SubjectId = Items[Index].GetNativeKey();
				return false;
			}
			IndexByKey.Add(Items[Index].GetNativeKey(), Index);
		}

		TArray<TArray<int32>> Outgoing;
		Outgoing.SetNum(Items.Num());
		TArray<int32> InDegree;
		InDegree.Init(0, Items.Num());
		for (int32 RelationIndex = 0; RelationIndex < Relations.Num(); ++RelationIndex)
		{
			const FAudioMediaGraphRelation& Relation = Relations[RelationIndex];
			const int32* From = IndexByKey.Find(Relation.GetFromNativeKey());
			const int32* To = IndexByKey.Find(Relation.GetToNativeKey());
			if (From == nullptr || To == nullptr)
			{
				auto Actual = MakeShared<FJsonObject>();
				Actual->SetBoolField(TEXT("from_resolved"), From != nullptr);
				Actual->SetBoolField(TEXT("to_resolved"), To != nullptr);
				auto Expected = MakeShared<FJsonObject>();
				Expected->SetBoolField(TEXT("from_resolved"), true);
				Expected->SetBoolField(TEXT("to_resolved"), true);
				OutError = AdapterFact(TEXT("audio_media_graph_endpoint_missing"), TEXT("A relation refers to a native item absent from the graph."),
					FString::Printf(TEXT("/relations/%d/endpoints"), RelationIndex), MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueObject>(Expected));
				auto Internal = MakeShared<FJsonObject>();
				Internal->SetStringField(TEXT("from"), Relation.GetFromNativeKey());
				Internal->SetStringField(TEXT("to"), Relation.GetToNativeKey());
				OutError.ActualEvidence = MakeShared<FJsonValueObject>(Internal);
				return false;
			}
			if (Relation.GetOrder() < 0)
			{
				OutError = AdapterFact(TEXT("audio_media_graph_relation_order_invalid"), TEXT("Native graph relation order must be nonnegative."),
					FString::Printf(TEXT("/relations/%d/order"), RelationIndex), MakeShared<FJsonValueNumber>(Relation.GetOrder()), MakeShared<FJsonValueString>(TEXT("nonnegative integer")));
				return false;
			}
			Outgoing[*From].Add(*To);
			++InDegree[*To];
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
				if (--InDegree[Next] == 0)
				{
					Queue.Add(Next);
				}
			}
		}
		if (Visited != Items.Num())
		{
			OutError = AdapterFact(TEXT("audio_media_graph_cycle"), TEXT("The native graph contains a cycle; not all items can be visited in dependency order."),
				TEXT("/relations/visited_item_count"), MakeShared<FJsonValueNumber>(Visited), MakeShared<FJsonValueNumber>(Items.Num()));
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
			const bool bEvaluate = Evidence.GetBarrierKind() == EAudioMediaGraphBarrierKind::Evaluate;
			OutError = AdapterFact(bEvaluate ? TEXT("audio_media_graph_evaluate_incomplete") : TEXT("audio_media_graph_compile_incomplete"),
				bEvaluate ? TEXT("The native graph evaluation barrier did not complete.") : TEXT("The native graph compile barrier did not complete."),
				TEXT("/barrier/complete"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}
		if (!Evidence.IsSettleComplete())
		{
			OutError = AdapterFact(TEXT("audio_media_graph_settle_incomplete"), TEXT("The native graph did not settle after its barrier."),
				TEXT("/settle/complete"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}
		if (!Evidence.HasIndependentCapture()
			|| Evidence.GetReadbackEvidenceId().IsEmpty())
		{
			OutError = AdapterFact(TEXT("audio_media_graph_capture_failed"),
				!Evidence.HasIndependentCapture() ? TEXT("Native readback did not provide an independent capture.") : TEXT("Native readback has no capture evidence identity."),
				!Evidence.HasIndependentCapture() ? TEXT("/capture/independent") : TEXT("/capture/evidence/available"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}
		if (Evidence.GetBeforeRevision().IsEmpty()
			|| Evidence.GetReadbackRevision().IsEmpty()
			|| Evidence.GetBeforeRevision() == Evidence.GetReadbackRevision()
			|| Evidence.GetApplyEvidenceId().IsEmpty()
			|| Evidence.GetBarrierEvidenceId().IsEmpty()
			|| Evidence.GetSettleEvidenceId().IsEmpty())
		{
			const FString Path = Evidence.GetBeforeRevision().IsEmpty() ? TEXT("/before_revision/available")
				: Evidence.GetReadbackRevision().IsEmpty() ? TEXT("/readback_revision/available")
				: Evidence.GetBeforeRevision() == Evidence.GetReadbackRevision() ? TEXT("/readback_revision/advanced")
				: Evidence.GetApplyEvidenceId().IsEmpty() ? TEXT("/apply_evidence/available")
				: Evidence.GetBarrierEvidenceId().IsEmpty() ? TEXT("/barrier_evidence/available") : TEXT("/settle_evidence/available");
			OutError = AdapterFact(TEXT("audio_media_graph_capture_failed"),
				Path.EndsWith(TEXT("/advanced")) ? TEXT("The readback revision is unchanged from before the native mutation.")
					: FString::Printf(TEXT("Native lifecycle evidence is missing %s."), *Path),
				Path, MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.ActualEvidence = LifecycleIdentityEvidence(Evidence);
			return false;
		}
		const TSet<FString> EvidenceIds = {
			Evidence.GetApplyEvidenceId(), Evidence.GetBarrierEvidenceId(),
			Evidence.GetSettleEvidenceId(), Evidence.GetReadbackEvidenceId()};
		if (EvidenceIds.Num() != 4)
		{
			OutError = AdapterFact(TEXT("audio_media_graph_echo_readback_forbidden"), TEXT("Apply, barrier, settle and readback reused evidence identities instead of independent observations."),
				TEXT("/evidence/distinct_identity_count"), MakeShared<FJsonValueNumber>(EvidenceIds.Num()), MakeShared<FJsonValueNumber>(4));
			OutError.ActualEvidence = LifecycleIdentityEvidence(Evidence);
			return false;
		}
		return true;
	}
}

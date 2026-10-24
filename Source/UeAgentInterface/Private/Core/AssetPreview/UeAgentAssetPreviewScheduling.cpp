#include "Core/AssetPreview/UeAgentAssetPreviewScheduling.h"

#include "Core/ExecutionMutation/UeAgentExecutionMutation.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringAssetIdentity.h"

namespace UE::UAI::AssetPreview
{
	namespace
	{
		struct FPending
		{
			int64 Ticket = 0;
			TFunction<void(bool)> Completion;
			bool bScheduled = false;
		};
		TMap<FString, TArray<FPending>> Queues;
		int64 NextTicket = 0;
		FString Key(const FString& Asset) { return UeAgentAuthoringAssetIdentity::ToObjectPath(Asset); }
		void Dispatch(const FString& Asset)
		{
			auto* Queue = Queues.Find(Asset);
			if (!Queue || Queue->IsEmpty() || !(*Queue)[0].Completion || (*Queue)[0].bScheduled) return;
			auto& Front = (*Queue)[0];
			Front.bScheduled = true;
			const int64 Ticket = Front.Ticket;
			ExecutionMutation::FGameThreadExecutionScheduler Scheduler;
			ExecutionMutation::FExecutionMutationError Error;
			Scheduler.Schedule(ExecutionMutation::EExecutionThreadAffinity::GameThread,
				FString::Printf(TEXT("asset-preview-%lld"), Ticket), [Asset, Ticket]()
				{
					auto* Current = Queues.Find(Asset);
					if (!Current || Current->IsEmpty() || (*Current)[0].Ticket != Ticket) return;
					auto Completion = MoveTemp((*Current)[0].Completion);
					Completion(false);
					FinishPreviewWork(Asset, Ticket);
				}, Error);
		}
	}
	int64 EnqueuePreviewWork(const FString& AssetPath)
	{
		const int64 Ticket = ++NextTicket;
		Queues.FindOrAdd(Key(AssetPath)).Add({Ticket, {}, false});
		return Ticket;
	}
	bool IsPreviewWorkReady(const FString& AssetPath, int64 Ticket)
	{
		const auto* Queue = Queues.Find(Key(AssetPath));
		return Queue && !Queue->IsEmpty() && (*Queue)[0].Ticket == Ticket;
	}
	void FinishPreviewWork(const FString& AssetPath, int64 Ticket)
	{
		if (!Ticket) return;
		const FString Asset = Key(AssetPath);
		auto* Queue = Queues.Find(Asset);
		if (!Queue) return;
		Queue->RemoveAll([Ticket](const FPending& Item) { return Item.Ticket == Ticket; });
		if (Queue->IsEmpty()) Queues.Remove(Asset);
		else Dispatch(Asset);
	}
	void DeferPreviewWork(const FString& AssetPath, TFunction<void(bool)> Completion)
	{
		if (!IsInGameThread())
		{
			ExecutionMutation::FGameThreadExecutionScheduler Scheduler;
			ExecutionMutation::FExecutionMutationError Error;
			Scheduler.Schedule(ExecutionMutation::EExecutionThreadAffinity::GameThread, TEXT("asset-preview-enqueue"),
				[AssetPath, Completion = MoveTemp(Completion)]() mutable { DeferPreviewWork(AssetPath, MoveTemp(Completion)); }, Error);
			return;
		}
		const FString Asset = Key(AssetPath);
		EnqueuePreviewWork(Asset);
		Queues.FindChecked(Asset).Last().Completion = MoveTemp(Completion);
		Dispatch(Asset);
	}
	void CancelDeferredPreviewWork()
	{
		auto Pending = MoveTemp(Queues);
		Queues.Reset();
		for (auto& Pair : Pending)
			for (auto& Item : Pair.Value)
				if (Item.Completion) Item.Completion(true);
	}
}

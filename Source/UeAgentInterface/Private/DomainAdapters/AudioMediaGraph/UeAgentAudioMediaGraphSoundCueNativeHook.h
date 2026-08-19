#pragma once

#include "CoreMinimal.h"
#include "DomainAdapters/AudioMediaGraph/UeAgentAudioMediaGraphAdapter.h"

class USoundCue;

namespace UE::UAI::DomainAdapters::AudioMediaGraph
{
	class FAudioMediaGraphSoundCueNodeMutation final
	{
	public:
		FAudioMediaGraphSoundCueNodeMutation(
			FString InObjectPath,
			FString InNativeClassRef,
			EAudioMediaGraphMutationIntent InIntent)
			: ObjectPath(MoveTemp(InObjectPath))
			, NativeClassRef(MoveTemp(InNativeClassRef))
			, Intent(InIntent)
		{
		}

		const FString& GetObjectPath() const { return ObjectPath; }
		const FString& GetNativeClassRef() const { return NativeClassRef; }
		EAudioMediaGraphMutationIntent GetIntent() const { return Intent; }

	private:
		FString ObjectPath;
		FString NativeClassRef;
		EAudioMediaGraphMutationIntent Intent;
	};

	class FAudioMediaGraphSoundCueRelationMutation final
	{
	public:
		FAudioMediaGraphSoundCueRelationMutation(
			FString InParentObjectPath,
			FString InChildObjectPath,
			int32 InOrder,
			EAudioMediaGraphMutationIntent InIntent)
			: ParentObjectPath(MoveTemp(InParentObjectPath))
			, ChildObjectPath(MoveTemp(InChildObjectPath))
			, Order(InOrder)
			, Intent(InIntent)
		{
		}

		const FString& GetParentObjectPath() const { return ParentObjectPath; }
		const FString& GetChildObjectPath() const { return ChildObjectPath; }
		int32 GetOrder() const { return Order; }
		EAudioMediaGraphMutationIntent GetIntent() const { return Intent; }

	private:
		FString ParentObjectPath;
		FString ChildObjectPath;
		int32 Order = INDEX_NONE;
		EAudioMediaGraphMutationIntent Intent;
	};

	class FAudioMediaGraphSoundCueChildRelation final
	{
	public:
		FAudioMediaGraphSoundCueChildRelation() = default;
		FAudioMediaGraphSoundCueChildRelation(
			FString InParentObjectPath,
			FString InChildObjectPath,
			int32 InOrder)
			: ParentObjectPath(MoveTemp(InParentObjectPath))
			, ChildObjectPath(MoveTemp(InChildObjectPath))
			, Order(InOrder)
		{
		}

		const FString& GetParentObjectPath() const { return ParentObjectPath; }
		const FString& GetChildObjectPath() const { return ChildObjectPath; }
		int32 GetOrder() const { return Order; }

	private:
		FString ParentObjectPath;
		FString ChildObjectPath;
		int32 Order = INDEX_NONE;
	};

	class FAudioMediaGraphSoundCueScenarioResult final
	{
	public:
		bool HasAppliedMutation() const { return bAppliedMutation; }
		bool HasCompileBarrier() const { return bCompileBarrier; }
		bool HasSettled() const { return bSettled; }
		bool HasIndependentCapture() const { return bIndependentCapture; }
		const FString& GetRootObjectPath() const { return RootObjectPath; }
		const TArray<FString>& GetNodeObjectPaths() const { return NodeObjectPaths; }
		const TArray<FAudioMediaGraphSoundCueChildRelation>&
		GetOrderedChildRelations() const { return OrderedChildRelations; }
		const FString& GetBeforeRevision() const { return BeforeRevision; }
		const FString& GetReadbackRevision() const { return ReadbackRevision; }
		const FString& GetApplyEvidenceId() const { return ApplyEvidenceId; }
		const FString& GetBarrierEvidenceId() const { return BarrierEvidenceId; }
		const FString& GetReadbackEvidenceId() const { return ReadbackEvidenceId; }

	private:
		friend class FAudioMediaGraphSoundCueNativeHook;
		bool bAppliedMutation = false;
		bool bCompileBarrier = false;
		bool bSettled = false;
		bool bIndependentCapture = false;
		FString RootObjectPath;
		TArray<FString> NodeObjectPaths;
		TArray<FAudioMediaGraphSoundCueChildRelation> OrderedChildRelations;
		FString BeforeRevision;
		FString ReadbackRevision;
		FString ApplyEvidenceId;
		FString BarrierEvidenceId;
		FString ReadbackEvidenceId;
	};

	class UEAGENTINTERFACE_API FAudioMediaGraphSoundCueNativeHook final
	{
	public:
		bool TryApplyNodeMutations(
			USoundCue& Cue,
			const TArray<FAudioMediaGraphSoundCueNodeMutation>& Mutations,
			FAudioMediaGraphError& OutError) const;

		bool TryApplyRelationMutations(
			USoundCue& Cue,
			const TArray<FAudioMediaGraphSoundCueRelationMutation>& Mutations,
			FAudioMediaGraphError& OutError) const;

		bool TrySetRoot(
			USoundCue& Cue,
			const FString& RootObjectPath,
			FAudioMediaGraphError& OutError) const;

		bool TrySynchronizeAndCompile(
			USoundCue& Cue,
			FAudioMediaGraphError& OutError) const;

		bool TryMutateCompileAndCaptureOwnedCue(
			USoundCue& Cue,
			FAudioMediaGraphSoundCueScenarioResult& OutResult,
			FAudioMediaGraphError& OutError) const;
	};
}

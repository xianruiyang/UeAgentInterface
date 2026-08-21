#include "DomainAdapters/AudioMediaGraph/UeAgentAudioMediaGraphSoundCueNativeHook.h"

#include "EdGraph/EdGraphNode.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundNode.h"
#include "Sound/SoundNodeMixer.h"
#include "Sound/SoundNodeWavePlayer.h"

namespace UE::UAI::DomainAdapters::AudioMediaGraph
{
	namespace
	{
		FString MakeSoundCueEvidenceId(const TCHAR* Stage)
		{
			return FString::Printf(TEXT("sound-cue:%s:%s"), Stage,
				*FGuid::NewGuid().ToString(EGuidFormats::Digits));
		}

		FString OwnedNodeName(
			const USoundCue& Cue,
			const FString& ObjectPath)
		{
			const FString Prefix = Cue.GetPathName() + TEXT(":");
			return ObjectPath.StartsWith(Prefix)
				? ObjectPath.RightChop(Prefix.Len()) : FString();
		}

		USoundNode* FindOwnedNode(
			USoundCue& Cue,
			const FString& ObjectPath)
		{
			const FString Name = OwnedNodeName(Cue, ObjectPath);
			return Name.IsEmpty() ? nullptr
				: FindObject<USoundNode>(&Cue, *Name);
		}

		void RemoveNodeFromTopology(USoundCue& Cue, USoundNode& Node)
		{
			for (USoundNode* Parent : Cue.AllNodes)
			{
				if (Parent != nullptr)
				{
					TArray<USoundNode*> Children = Parent->ChildNodes;
					Children.Remove(&Node);
					Parent->SetChildNodes(Children);
				}
			}
			if (Cue.FirstNode == &Node)
			{
				Cue.FirstNode = nullptr;
			}
			Cue.AllNodes.Remove(&Node);
		}
	}

	bool FAudioMediaGraphSoundCueNativeHook::TryApplyNodeMutations(
		USoundCue& Cue,
		const TArray<FAudioMediaGraphSoundCueNodeMutation>& Mutations,
		FAudioMediaGraphError& OutError) const
	{
		OutError.Reset();
		for (const FAudioMediaGraphSoundCueNodeMutation& Mutation : Mutations)
		{
			const FString Name = OwnedNodeName(Cue, Mutation.GetObjectPath());
			USoundNode* Existing = FindOwnedNode(Cue, Mutation.GetObjectPath());
			if (Name.IsEmpty())
			{
				OutError.Set(TEXT("audio_media_graph_sound_cue_node_path_invalid"),
					Mutation.GetObjectPath());
				return false;
			}
			if (Mutation.GetIntent() == EAudioMediaGraphMutationIntent::Delete)
			{
				if (Existing == nullptr)
				{
					OutError.Set(TEXT("audio_media_graph_sound_cue_node_missing"),
						Mutation.GetObjectPath());
					return false;
				}
				RemoveNodeFromTopology(Cue, *Existing);
				Existing->Rename(nullptr, GetTransientPackage(),
					REN_DontCreateRedirectors | REN_NonTransactional);
				Existing->MarkAsGarbage();
				continue;
			}
			UClass* NativeClass = LoadObject<UClass>(
				nullptr, *Mutation.GetNativeClassRef());
			if (NativeClass == nullptr
				|| !NativeClass->IsChildOf(USoundNode::StaticClass()))
			{
				OutError.Set(TEXT("audio_media_graph_sound_cue_class_invalid"),
					Mutation.GetNativeClassRef());
				return false;
			}
			if (Mutation.GetIntent() == EAudioMediaGraphMutationIntent::Update)
			{
				if (Existing == nullptr || !Existing->IsA(NativeClass))
				{
					OutError.Set(TEXT("audio_media_graph_sound_cue_node_mismatch"),
						Mutation.GetObjectPath());
					return false;
				}
				continue;
			}
			if (Existing != nullptr)
			{
				if (Mutation.GetIntent() == EAudioMediaGraphMutationIntent::Create)
				{
					OutError.Set(TEXT("audio_media_graph_sound_cue_node_exists"),
						Mutation.GetObjectPath());
					return false;
				}
				RemoveNodeFromTopology(Cue, *Existing);
				Existing->Rename(nullptr, GetTransientPackage(),
					REN_DontCreateRedirectors | REN_NonTransactional);
				Existing->MarkAsGarbage();
			}
			USoundNode* Created = NewObject<USoundNode>(
				&Cue, NativeClass, FName(*Name), RF_Transactional);
			if (Created == nullptr)
			{
				OutError.Set(TEXT("audio_media_graph_sound_cue_apply_failed"),
					Mutation.GetObjectPath());
				return false;
			}
			Cue.AllNodes.Add(Created);
			Cue.SetupSoundNode(Created, false);
		}
		Cue.PostEditChange();
		return true;
	}

	bool FAudioMediaGraphSoundCueNativeHook::TryApplyRelationMutations(
		USoundCue& Cue,
		const TArray<FAudioMediaGraphSoundCueRelationMutation>& Mutations,
		FAudioMediaGraphError& OutError) const
	{
		OutError.Reset();
		for (const FAudioMediaGraphSoundCueRelationMutation& Mutation : Mutations)
		{
			USoundNode* Parent = FindOwnedNode(Cue, Mutation.GetParentObjectPath());
			USoundNode* Child = FindOwnedNode(Cue, Mutation.GetChildObjectPath());
			if (Parent == nullptr || Child == nullptr)
			{
				OutError.Set(TEXT("audio_media_graph_sound_cue_relation_invalid"),
					Mutation.GetParentObjectPath());
				return false;
			}
			TArray<USoundNode*> Children = Parent->ChildNodes;
			const int32 ExistingIndex = Children.IndexOfByKey(Child);
			if (Mutation.GetIntent() == EAudioMediaGraphMutationIntent::Delete)
			{
				if (ExistingIndex == INDEX_NONE)
				{
					OutError.Set(TEXT("audio_media_graph_sound_cue_relation_missing"));
					return false;
				}
				Children.RemoveAt(ExistingIndex);
			}
			else
			{
				if (Mutation.GetOrder() < 0)
				{
					OutError.Set(
						TEXT("audio_media_graph_sound_cue_relation_order_invalid"),
						Mutation.GetParentObjectPath());
					return false;
				}
				if (Mutation.GetIntent() == EAudioMediaGraphMutationIntent::Create
					&& ExistingIndex != INDEX_NONE)
				{
					OutError.Set(TEXT("audio_media_graph_sound_cue_relation_exists"));
					return false;
				}
				Children.Remove(Child);
				Children.Insert(Child,
					FMath::Clamp(Mutation.GetOrder(), 0, Children.Num()));
			}
			Parent->SetChildNodes(Children);
			if (UEdGraphNode* const GraphNode = Parent->GetGraphNode())
			{
				GraphNode->ReconstructNode();
			}
		}
		Cue.PostEditChange();
		return true;
	}

	bool FAudioMediaGraphSoundCueNativeHook::TrySetRoot(
		USoundCue& Cue,
		const FString& RootObjectPath,
		FAudioMediaGraphError& OutError) const
	{
		OutError.Reset();
		USoundNode* Root = FindOwnedNode(Cue, RootObjectPath);
		if (Root == nullptr)
		{
			OutError.Set(TEXT("audio_media_graph_sound_cue_root_missing"),
				RootObjectPath);
			return false;
		}
		Cue.FirstNode = Root;
		Cue.PostEditChange();
		return true;
	}

	bool FAudioMediaGraphSoundCueNativeHook::TrySynchronizeAndCompile(
		USoundCue& Cue,
		FAudioMediaGraphError& OutError) const
	{
		OutError.Reset();
		if (Cue.GetGraph() == nullptr)
		{
			Cue.CreateGraph();
		}
		for (USoundNode* Node : Cue.AllNodes)
		{
			if (Node != nullptr && Node->GetGraphNode() == nullptr)
			{
				Cue.SetupSoundNode(Node, false);
			}
		}
		Cue.LinkGraphNodesFromSoundNodes();
		Cue.CompileSoundNodesFromGraphNodes();
		Cue.PostEditChange();
		if (Cue.GetGraph() == nullptr)
		{
			OutError.Set(TEXT("audio_media_graph_compile_incomplete"));
			return false;
		}
		return true;
	}

	bool FAudioMediaGraphSoundCueNativeHook::
	TryMutateCompileAndCaptureOwnedCue(
		USoundCue& Cue,
		FAudioMediaGraphSoundCueScenarioResult& OutResult,
		FAudioMediaGraphError& OutError) const
	{
		OutResult = FAudioMediaGraphSoundCueScenarioResult();
		OutError.Reset();
		OutResult.BeforeRevision = FString::Printf(
			TEXT("sound-cue-nodes:%d"), Cue.AllNodes.Num());

		Cue.CreateGraph();
		USoundNodeMixer* Root = NewObject<USoundNodeMixer>(
			&Cue, USoundNodeMixer::StaticClass(), NAME_None, RF_Transactional);
		USoundNodeWavePlayer* Child = NewObject<USoundNodeWavePlayer>(
			&Cue, USoundNodeWavePlayer::StaticClass(), NAME_None,
			RF_Transactional);
		if (Root == nullptr || Child == nullptr)
		{
			OutError.Set(TEXT("audio_media_graph_sound_cue_apply_failed"));
			return false;
		}
		TArray<USoundNode*> Children = {Child};
		Root->SetChildNodes(Children);
		Cue.AllNodes.Add(Root);
		Cue.AllNodes.Add(Child);
		// SoundCue graph input pins are created from ChildNodes. Establish the
		// native relation first, then create the editor nodes from that topology.
		Cue.SetupSoundNode(Root, false);
		Cue.SetupSoundNode(Child, false);
		Cue.FirstNode = Root;
		OutResult.bAppliedMutation = true;
		OutResult.ApplyEvidenceId = MakeSoundCueEvidenceId(TEXT("apply"));

		Cue.LinkGraphNodesFromSoundNodes();
		Cue.CompileSoundNodesFromGraphNodes();
		Cue.PostEditChange();
		OutResult.bCompileBarrier = Cue.GetGraph() != nullptr;
		OutResult.bSettled = OutResult.bCompileBarrier;
		OutResult.BarrierEvidenceId =
			MakeSoundCueEvidenceId(TEXT("graph-sync-compile"));
		if (!OutResult.bCompileBarrier)
		{
			OutError.Set(TEXT("audio_media_graph_compile_incomplete"));
			return false;
		}

		USoundNode* CapturedRoot = Cue.FirstNode;
		if (CapturedRoot == nullptr || Cue.AllNodes.Num() < 2)
		{
			OutError.Set(TEXT("audio_media_graph_capture_failed"));
			return false;
		}
		OutResult.RootObjectPath = CapturedRoot->GetPathName();
		for (const USoundNode* Node : Cue.AllNodes)
		{
			if (Node != nullptr)
			{
				OutResult.NodeObjectPaths.Add(Node->GetPathName());
			}
		}
		for (int32 Index = 0; Index < CapturedRoot->ChildNodes.Num(); ++Index)
		{
			const USoundNode* CapturedChild = CapturedRoot->ChildNodes[Index];
			if (CapturedChild != nullptr)
			{
				OutResult.OrderedChildRelations.Emplace(
					CapturedRoot->GetPathName(), CapturedChild->GetPathName(), Index);
			}
		}
		if (OutResult.OrderedChildRelations.IsEmpty())
		{
			OutError.Set(TEXT("audio_media_graph_capture_failed"));
			return false;
		}
		OutResult.bIndependentCapture = true;
		OutResult.ReadbackRevision = FString::Printf(
			TEXT("sound-cue-nodes:%d-relations:%d"),
			OutResult.NodeObjectPaths.Num(),
			OutResult.OrderedChildRelations.Num());
		OutResult.ReadbackEvidenceId =
			MakeSoundCueEvidenceId(TEXT("native-readback"));
		return true;
	}
}

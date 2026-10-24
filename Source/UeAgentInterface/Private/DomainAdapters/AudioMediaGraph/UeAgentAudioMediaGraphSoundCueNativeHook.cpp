#include "DomainAdapters/AudioMediaGraph/UeAgentAudioMediaGraphSoundCueNativeHook.h"
#include "Misc/ScopeExit.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "EdGraph/EdGraphNode.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundNode.h"
#include "Sound/SoundNodeMixer.h"
#include "Sound/SoundNodeWavePlayer.h"

namespace UE::UAI::DomainAdapters::AudioMediaGraph
{
	namespace
	{
		FAudioMediaGraphError SoundCueFact(const USoundCue& Cue, const FString& Code,
			const FString& Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("native_graph"), Cue.GetPathName());
			Error.SourceKind = TEXT("native_result");
			if (Cue.IsAsset()) Error.TargetPath = Cue.GetPathName();
			Error.PropertyPath = Path;
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			return Error;
		}

		TSharedPtr<FJsonValue> SoundCueFlags(const TCHAR* First, bool bFirst, const TCHAR* Second, bool bSecond)
		{
			auto Facts = MakeShared<FJsonObject>();
			Facts->SetBoolField(First, bFirst);
			Facts->SetBoolField(Second, bSecond);
			return MakeShared<FJsonValueObject>(Facts);
		}

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
		for (int32 MutationIndex = 0; MutationIndex < Mutations.Num(); ++MutationIndex)
		{
			const auto& Mutation = Mutations[MutationIndex];
			ON_SCOPE_EXIT
			{
				if (OutError.IsValid() && !OutError.OperationIndex.IsSet())
				{
					OutError.OperationIndex = MutationIndex;
				}
			};
			const FString Name = OwnedNodeName(Cue, Mutation.GetObjectPath());
			USoundNode* Existing = FindOwnedNode(Cue, Mutation.GetObjectPath());
			if (Name.IsEmpty())
			{
				OutError = SoundCueFact(Cue, TEXT("audio_media_graph_sound_cue_node_path_invalid"),
					TEXT("The node object path does not identify a node owned by this SoundCue."), TEXT("/object_path"), MakeShared<FJsonValueString>(Mutation.GetObjectPath()), MakeShared<FJsonValueString>(Cue.GetPathName() + TEXT(":<node-name>")));
				return false;
			}
			if (Mutation.GetIntent() == EAudioMediaGraphMutationIntent::Delete)
			{
				if (Existing == nullptr)
				{
					OutError = SoundCueFact(Cue, TEXT("audio_media_graph_sound_cue_node_missing"),
						TEXT("The SoundCue node selected for deletion was not found."), Mutation.GetObjectPath(), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return false;
				}
				RemoveNodeFromTopology(Cue, *Existing);
				Existing->Rename(nullptr, GetTransientPackage(),
					REN_DontCreateRedirectors | REN_NonTransactional);
				// Native SoundCue deletion releases graph ownership, not UE's GC roots.
				continue;
			}
			UClass* NativeClass = LoadObject<UClass>(
				nullptr, *Mutation.GetNativeClassRef());
			if (NativeClass == nullptr
				|| !NativeClass->IsChildOf(USoundNode::StaticClass()))
			{
				OutError = SoundCueFact(Cue, TEXT("audio_media_graph_sound_cue_class_invalid"),
					TEXT("The requested native class could not be loaded as a SoundNode class."), TEXT("/native_class"), MakeShared<FJsonValueString>(NativeClass ? NativeClass->GetPathName() : Mutation.GetNativeClassRef()), MakeShared<FJsonValueString>(USoundNode::StaticClass()->GetPathName()));
				return false;
			}
			if (Mutation.GetIntent() == EAudioMediaGraphMutationIntent::Update)
			{
				if (Existing == nullptr || !Existing->IsA(NativeClass))
				{
					OutError = SoundCueFact(Cue, TEXT("audio_media_graph_sound_cue_node_mismatch"),
						TEXT("The node selected for update is missing or has a different native class."), Mutation.GetObjectPath(), MakeShared<FJsonValueString>(Existing ? Existing->GetClass()->GetPathName() : TEXT("<missing>")), MakeShared<FJsonValueString>(Mutation.GetNativeClassRef()));
					return false;
				}
				continue;
			}
			if (Existing != nullptr)
			{
				if (Mutation.GetIntent() == EAudioMediaGraphMutationIntent::Create)
				{
					OutError = SoundCueFact(Cue, TEXT("audio_media_graph_sound_cue_node_exists"),
						TEXT("A node already exists at the requested SoundCue object path."), Mutation.GetObjectPath(), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
					return false;
				}
				RemoveNodeFromTopology(Cue, *Existing);
				Existing->Rename(nullptr, GetTransientPackage(),
					REN_DontCreateRedirectors | REN_NonTransactional);
			}
			USoundNode* Created = NewObject<USoundNode>(
				&Cue, NativeClass, FName(*Name), RF_Transactional);
			if (Created == nullptr)
			{
				OutError = SoundCueFact(Cue, TEXT("audio_media_graph_sound_cue_apply_failed"),
					TEXT("Unreal did not create the requested SoundCue node."), Mutation.GetObjectPath(), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.CandidateName = Mutation.GetNativeClassRef();
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
		for (int32 MutationIndex = 0; MutationIndex < Mutations.Num(); ++MutationIndex)
		{
			const auto& Mutation = Mutations[MutationIndex];
			ON_SCOPE_EXIT
			{
				if (OutError.IsValid() && !OutError.OperationIndex.IsSet())
				{
					OutError.OperationIndex = MutationIndex;
				}
			};
			USoundNode* Parent = FindOwnedNode(Cue, Mutation.GetParentObjectPath());
			USoundNode* Child = FindOwnedNode(Cue, Mutation.GetChildObjectPath());
			if (Parent == nullptr || Child == nullptr)
			{
				OutError = SoundCueFact(Cue, TEXT("audio_media_graph_sound_cue_relation_invalid"),
					TEXT("The SoundCue relation parent or child node was not found."), Mutation.GetParentObjectPath(), SoundCueFlags(TEXT("parent_present"), Parent != nullptr, TEXT("child_present"), Child != nullptr), SoundCueFlags(TEXT("parent_present"), true, TEXT("child_present"), true));
				OutError.CandidateName = Mutation.GetChildObjectPath();
				return false;
			}
			TArray<USoundNode*> Children = Parent->ChildNodes;
			const int32 ExistingIndex = Children.IndexOfByKey(Child);
			if (Mutation.GetIntent() == EAudioMediaGraphMutationIntent::Delete)
			{
				if (ExistingIndex == INDEX_NONE)
				{
					OutError = SoundCueFact(Cue, TEXT("audio_media_graph_sound_cue_relation_missing"),
						TEXT("The child is not attached to the selected SoundCue parent."), Mutation.GetParentObjectPath(), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					OutError.CandidateName = Mutation.GetChildObjectPath();
					return false;
				}
				Children.RemoveAt(ExistingIndex);
			}
			else
			{
				if (Mutation.GetOrder() < 0)
				{
					OutError = SoundCueFact(Cue, TEXT("audio_media_graph_sound_cue_relation_order_invalid"),
						TEXT("The SoundCue child order must be nonnegative."), TEXT("/order"), MakeShared<FJsonValueNumber>(Mutation.GetOrder()), MakeShared<FJsonValueString>(TEXT("nonnegative integer")));
					return false;
				}
				if (Mutation.GetIntent() == EAudioMediaGraphMutationIntent::Create
					&& ExistingIndex != INDEX_NONE)
				{
					OutError = SoundCueFact(Cue, TEXT("audio_media_graph_sound_cue_relation_exists"),
						TEXT("The child is already attached to the selected SoundCue parent."), Mutation.GetParentObjectPath(), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
					OutError.CandidateName = Mutation.GetChildObjectPath();
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
			OutError = SoundCueFact(Cue, TEXT("audio_media_graph_sound_cue_root_missing"),
				TEXT("The selected SoundCue root node was not found."), RootObjectPath, MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
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
			OutError = SoundCueFact(Cue, TEXT("audio_media_graph_compile_incomplete"),
				TEXT("The SoundCue editor graph is missing after synchronization and compilation."), TEXT("/editor_graph"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
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
			OutError = SoundCueFact(Cue, TEXT("audio_media_graph_sound_cue_apply_failed"),
				TEXT("Unreal did not create both SoundCue scenario nodes."), TEXT("/nodes"), SoundCueFlags(TEXT("root_present"), Root != nullptr, TEXT("child_present"), Child != nullptr), SoundCueFlags(TEXT("root_present"), true, TEXT("child_present"), true));
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
			OutError = SoundCueFact(Cue, TEXT("audio_media_graph_compile_incomplete"),
				TEXT("The SoundCue editor graph is missing after synchronization and compilation."), TEXT("/editor_graph"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}

		USoundNode* CapturedRoot = Cue.FirstNode;
		if (CapturedRoot == nullptr || Cue.AllNodes.Num() < 2)
		{
			auto Actual = MakeShared<FJsonObject>();
			Actual->SetBoolField(TEXT("root_present"), CapturedRoot != nullptr);
			Actual->SetNumberField(TEXT("node_count"), Cue.AllNodes.Num());
			OutError = SoundCueFact(Cue, TEXT("audio_media_graph_capture_failed"),
				TEXT("SoundCue readback did not find a root and at least two nodes."), TEXT("/capture"),
				MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("a root and at least two nodes")));
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
			OutError = SoundCueFact(Cue, TEXT("audio_media_graph_capture_failed"),
				TEXT("SoundCue readback found no non-null child relation on the root."), TEXT("/capture/child_relations"), MakeShared<FJsonValueNumber>(OutResult.OrderedChildRelations.Num()), MakeShared<FJsonValueString>(TEXT("at least one relation")));
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

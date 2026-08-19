// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/Audio/UeAgentHttpServer_AudioOps.h"
#include "Utilities/UeAgentJsonDiagnostics.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Paths.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundNode.h"
#include "Sound/SoundNodeAttenuation.h"
#include "Sound/SoundNodeBranch.h"
#include "Sound/SoundNodeConcatenator.h"
#include "Sound/SoundNodeDelay.h"
#include "Sound/SoundNodeDialoguePlayer.h"
#include "Sound/SoundNodeDistanceCrossFade.h"
#include "Sound/SoundNodeDoppler.h"
#include "Sound/SoundNodeEnveloper.h"
#include "Sound/SoundNodeLooping.h"
#include "Sound/SoundNodeMixer.h"
#include "Sound/SoundNodeModulator.h"
#include "Sound/SoundNodeModulatorContinuous.h"
#include "Sound/SoundNodeParamCrossFade.h"
#include "Sound/SoundNodeQualityLevel.h"
#include "Sound/SoundNodeRandom.h"
#include "Sound/SoundNodeSoundClass.h"
#include "Sound/SoundNodeSwitch.h"
#include "Sound/SoundNodeWaveParam.h"
#include "Sound/SoundNodeWavePlayer.h"
#include "SoundCueGraph/SoundCueGraphNode.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"

namespace UeAgentAudioOps
{
	FString SoundNodeTypeName(USoundNode* Node)
	{
		if (!Node)
		{
			return TEXT("unknown");
		}
		if (Node->IsA<USoundNodeWavePlayer>()) return TEXT("wave_player");
		if (Node->IsA<USoundNodeRandom>()) return TEXT("random");
		if (Node->IsA<USoundNodeMixer>()) return TEXT("mixer");
		if (Node->IsA<USoundNodeModulator>()) return TEXT("modulator");
		if (Node->IsA<USoundNodeModulatorContinuous>()) return TEXT("continuous_modulator");
		if (Node->IsA<USoundNodeDelay>()) return TEXT("delay");
		if (Node->IsA<USoundNodeLooping>()) return TEXT("looping");
		if (Node->IsA<USoundNodeConcatenator>()) return TEXT("concatenator");
		if (Node->IsA<USoundNodeDistanceCrossFade>()) return TEXT("distance_crossfade");
		if (Node->IsA<USoundNodeParamCrossFade>()) return TEXT("param_crossfade");
		if (Node->IsA<USoundNodeSwitch>()) return TEXT("switch");
		if (Node->IsA<USoundNodeBranch>()) return TEXT("branch");
		if (Node->IsA<USoundNodeAttenuation>()) return TEXT("attenuation");
		if (Node->IsA<USoundNodeSoundClass>()) return TEXT("sound_class");
		if (Node->IsA<USoundNodeEnveloper>()) return TEXT("enveloper");
		if (Node->IsA<USoundNodeDoppler>()) return TEXT("doppler");
		if (Node->IsA<USoundNodeQualityLevel>()) return TEXT("quality_level");
		if (Node->IsA<USoundNodeWaveParam>()) return TEXT("wave_param");
		if (Node->IsA<USoundNodeDialoguePlayer>()) return TEXT("dialogue_player");
		return Node->GetClass() ? Node->GetClass()->GetName() : TEXT("unknown");
	}
	UClass* ResolveSoundNodeClass(const FString& TypeInput, const FString& ClassPathInput = FString())
	{
		FString Type = TypeInput.ToLower();
		Type.TrimStartAndEndInline();
		if (!ClassPathInput.IsEmpty())
		{
			if (UClass* ExplicitClass = LoadClassByPath(ClassPathInput))
			{
				return ExplicitClass->IsChildOf(USoundNode::StaticClass()) ? ExplicitClass : nullptr;
			}
		}
		if (Type == TEXT("wave_player")) return USoundNodeWavePlayer::StaticClass();
		if (Type == TEXT("random")) return USoundNodeRandom::StaticClass();
		if (Type == TEXT("mixer")) return USoundNodeMixer::StaticClass();
		if (Type == TEXT("modulator")) return USoundNodeModulator::StaticClass();
		if (Type == TEXT("continuous_modulator") || Type == TEXT("modulator_continuous")) return USoundNodeModulatorContinuous::StaticClass();
		if (Type == TEXT("delay")) return USoundNodeDelay::StaticClass();
		if (Type == TEXT("looping")) return USoundNodeLooping::StaticClass();
		if (Type == TEXT("concatenator")) return USoundNodeConcatenator::StaticClass();
		if (Type == TEXT("distance_crossfade")) return USoundNodeDistanceCrossFade::StaticClass();
		if (Type == TEXT("param_crossfade")) return USoundNodeParamCrossFade::StaticClass();
		if (Type == TEXT("switch")) return USoundNodeSwitch::StaticClass();
		if (Type == TEXT("branch")) return USoundNodeBranch::StaticClass();
		if (Type == TEXT("attenuation")) return USoundNodeAttenuation::StaticClass();
		if (Type == TEXT("sound_class")) return USoundNodeSoundClass::StaticClass();
		if (Type == TEXT("enveloper")) return USoundNodeEnveloper::StaticClass();
		if (Type == TEXT("doppler")) return USoundNodeDoppler::StaticClass();
		if (Type == TEXT("quality_level")) return USoundNodeQualityLevel::StaticClass();
		if (Type == TEXT("wave_param")) return USoundNodeWaveParam::StaticClass();
		if (Type == TEXT("dialogue_player")) return USoundNodeDialoguePlayer::StaticClass();
		return nullptr;
	}
	FString GetNodeId(USoundNode* Node, int32 Index)
	{
		if (!Node)
		{
			return FString::Printf(TEXT("node_%d"), Index);
		}
		FString Name = Node->GetName();
		if (Name.StartsWith(TEXT("SoundNode")))
		{
			return FString::Printf(TEXT("node_%d"), Index);
		}
		return Name;
	}
	TSharedPtr<FJsonObject> BuildSoundCueInfo(USoundCue* Cue, bool bIncludeNodes, bool bIncludeProperties, bool bIncludeValidation)
	{
		TSharedPtr<FJsonObject> Info = MakeShared<FJsonObject>();
		FillAssetIdentity(Cue, Info);
		if (!Cue)
		{
			return Info;
		}
		Info->SetNumberField(TEXT("duration"), Cue->GetDuration());
		Info->SetStringField(TEXT("root_node_id"), Cue->FirstNode ? GetNodeId(Cue->FirstNode, Cue->AllNodes.Find(Cue->FirstNode)) : FString());
		Info->SetNumberField(TEXT("node_count"), Cue->AllNodes.Num());

		if (bIncludeNodes)
		{
			TMap<USoundNode*, FString> NodeIds;
			for (int32 Index = 0; Index < Cue->AllNodes.Num(); ++Index)
			{
				NodeIds.Add(Cue->AllNodes[Index], GetNodeId(Cue->AllNodes[Index], Index));
			}
			TArray<TSharedPtr<FJsonValue>> Nodes;
			TArray<TSharedPtr<FJsonValue>> Edges;
			for (int32 Index = 0; Index < Cue->AllNodes.Num(); ++Index)
			{
				USoundNode* Node = Cue->AllNodes[Index];
				if (!Node)
				{
					continue;
				}
				TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
				NodeObj->SetStringField(TEXT("id"), NodeIds.FindRef(Node));
				NodeObj->SetStringField(TEXT("type"), SoundNodeTypeName(Node));
				NodeObj->SetStringField(TEXT("class"), Node->GetClass() ? Node->GetClass()->GetPathName() : FString());
				if (USoundNodeWavePlayer* WavePlayer = Cast<USoundNodeWavePlayer>(Node))
				{
					NodeObj->SetStringField(TEXT("sound_wave"), ObjectPathOrEmpty(WavePlayer->GetSoundWave()));
					NodeObj->SetBoolField(TEXT("looping"), WavePlayer->bLooping != 0);
				}
				else if (USoundNodeRandom* RandomNode = Cast<USoundNodeRandom>(Node))
				{
					TArray<TSharedPtr<FJsonValue>> Weights;
					for (float Weight : RandomNode->Weights)
					{
						Weights.Add(MakeShared<FJsonValueNumber>(Weight));
					}
					NodeObj->SetArrayField(TEXT("weights"), Weights);
					NodeObj->SetBoolField(TEXT("randomize_without_replacement"), RandomNode->bRandomizeWithoutReplacement != 0);
				}
				else if (USoundNodeMixer* MixerNode = Cast<USoundNodeMixer>(Node))
				{
					TArray<TSharedPtr<FJsonValue>> Volumes;
					for (float Volume : MixerNode->InputVolume)
					{
						Volumes.Add(MakeShared<FJsonValueNumber>(Volume));
					}
					NodeObj->SetArrayField(TEXT("input_volume"), Volumes);
				}

				TArray<TSharedPtr<FJsonValue>> Children;
				for (int32 ChildIndex = 0; ChildIndex < Node->ChildNodes.Num(); ++ChildIndex)
				{
					USoundNode* Child = Node->ChildNodes[ChildIndex];
					const FString ChildId = NodeIds.FindRef(Child);
					if (!ChildId.IsEmpty())
					{
						Children.Add(JsonStringValue(ChildId));
						TSharedPtr<FJsonObject> EdgeObj = MakeShared<FJsonObject>();
						EdgeObj->SetStringField(TEXT("from"), NodeIds.FindRef(Node));
						EdgeObj->SetStringField(TEXT("to"), ChildId);
						EdgeObj->SetNumberField(TEXT("child_index"), ChildIndex);
						Edges.Add(MakeShared<FJsonValueObject>(EdgeObj));
					}
				}
				NodeObj->SetArrayField(TEXT("children"), Children);
				Nodes.Add(MakeShared<FJsonValueObject>(NodeObj));
			}
			Info->SetArrayField(TEXT("nodes"), Nodes);
			Info->SetArrayField(TEXT("edges"), Edges);
		}
		if (bIncludeValidation)
		{
			TArray<TSharedPtr<FJsonValue>> Issues;
			if (!Cue->FirstNode && Cue->AllNodes.Num() > 0)
			{
				AddIssue(Issues, TEXT("missing_root_node"), TEXT("error"), TEXT("SoundCue has nodes but no FirstNode."));
			}
			Info->SetArrayField(TEXT("json_issues"), Issues);
		}
		if (bIncludeProperties)
		{
			Info->SetObjectField(TEXT("raw_properties"), BuildGenericAssetJson(Cue, SoundCueProfile, true));
		}
		return Info;
	}
	bool ValidateSoundCueFolderJson(const TSharedPtr<FJsonObject>& NodesJson, const TSharedPtr<FJsonObject>& EdgesJson, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		if (!NodesJson.IsValid() || !NodesJson->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes)
		{
			AddIssue(Issues, TEXT("missing_nodes_array"), TEXT("error"), TEXT("Sound Cue graph must contain nodes[]."), TEXT("nodes"));
			return false;
		}
		TSet<FString> NodeIds;
		for (const TSharedPtr<FJsonValue>& Value : *Nodes)
		{
			const TSharedPtr<FJsonObject>* NodeObj = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(NodeObj) || !NodeObj || !NodeObj->IsValid())
			{
				AddIssue(Issues, TEXT("invalid_node_entry"), TEXT("error"), TEXT("Each nodes[] entry must be an object."), TEXT("nodes"));
				continue;
			}
			FString Id;
			FString Type;
			FString ClassPath;
			(*NodeObj)->TryGetStringField(TEXT("id"), Id);
			(*NodeObj)->TryGetStringField(TEXT("type"), Type);
			(*NodeObj)->TryGetStringField(TEXT("class"), ClassPath);
			if (Id.IsEmpty())
			{
				AddIssue(Issues, TEXT("missing_node_id"), TEXT("error"), TEXT("Node entry is missing id."), TEXT("nodes.id"));
			}
			else if (NodeIds.Contains(Id))
			{
				AddIssue(Issues, TEXT("duplicate_node_id"), TEXT("error"), FString::Printf(TEXT("Duplicate node id '%s'."), *Id), TEXT("nodes.id"));
			}
			NodeIds.Add(Id);
			if (!ResolveSoundNodeClass(Type, ClassPath))
			{
				AddIssue(Issues, TEXT("unsupported_node_type"), TEXT("error"), FString::Printf(TEXT("Unsupported SoundCue node type '%s'."), *Type), TEXT("nodes.type"));
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Edges = nullptr;
		if (EdgesJson.IsValid() && EdgesJson->TryGetArrayField(TEXT("edges"), Edges) && Edges)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Edges)
			{
				const TSharedPtr<FJsonObject>* EdgeObj = nullptr;
				if (!Value.IsValid() || !Value->TryGetObject(EdgeObj) || !EdgeObj || !EdgeObj->IsValid())
				{
					AddIssue(Issues, TEXT("invalid_edge_entry"), TEXT("error"), TEXT("Each edges[] entry must be an object."), TEXT("edges"));
					continue;
				}
				FString From;
				FString To;
				(*EdgeObj)->TryGetStringField(TEXT("from"), From);
				(*EdgeObj)->TryGetStringField(TEXT("to"), To);
				if (!NodeIds.Contains(From) || !NodeIds.Contains(To))
				{
					AddIssue(Issues, TEXT("edge_endpoint_not_found"), TEXT("error"), FString::Printf(TEXT("Edge endpoint not found: %s -> %s."), *From, *To), TEXT("edges"));
				}
			}
		}
		return !UeAgentJsonDiagnostics::HasErrorIssue(Issues);
	}
	void ApplySoundNodeStructuredFields(USoundNode* Node, const TSharedPtr<FJsonObject>& NodeObj)
	{
		if (!Node || !NodeObj.IsValid())
		{
			return;
		}
		if (USoundNodeWavePlayer* WavePlayer = Cast<USoundNodeWavePlayer>(Node))
		{
			FString WavePath;
			if (JsonTryGetStringAny(NodeObj, { TEXT("sound_wave"), TEXT("wave"), TEXT("asset_path") }, WavePath))
			{
				if (USoundWave* Wave = Cast<USoundWave>(LoadAssetObject(WavePath)))
				{
					WavePlayer->SetSoundWave(Wave);
				}
			}
			bool bLooping = WavePlayer->bLooping != 0;
			if (GetOptionalBool(NodeObj, TEXT("looping"), bLooping, WavePath))
			{
				WavePlayer->bLooping = bLooping;
			}
		}
		else if (USoundNodeRandom* RandomNode = Cast<USoundNodeRandom>(Node))
		{
			const TArray<TSharedPtr<FJsonValue>>* Weights = nullptr;
			if (NodeObj->TryGetArrayField(TEXT("weights"), Weights) && Weights)
			{
				RandomNode->Weights.Reset();
				for (const TSharedPtr<FJsonValue>& Weight : *Weights)
				{
					RandomNode->Weights.Add(Weight.IsValid() ? static_cast<float>(Weight->AsNumber()) : 1.0f);
				}
			}
			bool bWithoutReplacement = RandomNode->bRandomizeWithoutReplacement != 0;
			FString Error;
			if (GetOptionalBool(NodeObj, TEXT("randomize_without_replacement"), bWithoutReplacement, Error))
			{
				RandomNode->bRandomizeWithoutReplacement = bWithoutReplacement;
			}
			while (RandomNode->Weights.Num() < RandomNode->ChildNodes.Num())
			{
				RandomNode->Weights.Add(1.0f);
			}
			if (RandomNode->Weights.Num() > RandomNode->ChildNodes.Num())
			{
				RandomNode->Weights.SetNum(RandomNode->ChildNodes.Num());
			}
		}
		else if (USoundNodeMixer* MixerNode = Cast<USoundNodeMixer>(Node))
		{
			const TArray<TSharedPtr<FJsonValue>>* Volumes = nullptr;
			if (NodeObj->TryGetArrayField(TEXT("input_volume"), Volumes) && Volumes)
			{
				MixerNode->InputVolume.Reset();
				for (const TSharedPtr<FJsonValue>& Volume : *Volumes)
				{
					MixerNode->InputVolume.Add(Volume.IsValid() ? static_cast<float>(Volume->AsNumber()) : 1.0f);
				}
			}
		}
	}
	bool ApplySoundCueGraph(USoundCue* Cue, const TSharedPtr<FJsonObject>& NodesJson, const TSharedPtr<FJsonObject>& EdgesJson, TArray<TSharedPtr<FJsonValue>>& OutPropertyResults, int32& OutNodesCreated, int32& OutEdgesCreated, bool bDryRun, FString& OutError)
	{
		OutNodesCreated = 0;
		OutEdgesCreated = 0;
		if (!Cue)
		{
			OutError = TEXT("sound_cue_not_found");
			return false;
		}
		TArray<TSharedPtr<FJsonValue>> Issues;
		if (!ValidateSoundCueFolderJson(NodesJson, EdgesJson, Issues))
		{
			OutError = TEXT("sound_cue_folder_validation_failed");
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* NodesForClassCheck = nullptr;
		if (NodesJson->TryGetArrayField(TEXT("nodes"), NodesForClassCheck) && NodesForClassCheck)
		{
			for (const TSharedPtr<FJsonValue>& Value : *NodesForClassCheck)
			{
				const TSharedPtr<FJsonObject>* NodeObj = nullptr;
				if (!Value.IsValid() || !Value->TryGetObject(NodeObj) || !NodeObj || !NodeObj->IsValid())
				{
					continue;
				}
				FString Type;
				FString ClassPath;
				(*NodeObj)->TryGetStringField(TEXT("type"), Type);
				(*NodeObj)->TryGetStringField(TEXT("class"), ClassPath);
				if (!ResolveSoundNodeClass(Type, ClassPath))
				{
					OutError = TEXT("unsupported_node_type");
					return false;
				}
			}
		}
		if (bDryRun)
		{
			const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
			NodesJson->TryGetArrayField(TEXT("nodes"), Nodes);
			OutNodesCreated = Nodes ? Nodes->Num() : 0;
			const TArray<TSharedPtr<FJsonValue>>* Edges = nullptr;
			EdgesJson->TryGetArrayField(TEXT("edges"), Edges);
			OutEdgesCreated = Edges ? Edges->Num() : 0;
			return true;
		}

		Cue->Modify();
		Cue->CreateGraph();
		Cue->ResetGraph();
		TArray<UObject*> ExistingChildObjects;
		GetObjectsWithOuter(Cue, ExistingChildObjects, false);
		for (UObject* ExistingChildObject : ExistingChildObjects)
		{
			if (!ExistingChildObject || !ExistingChildObject->IsA<USoundNode>())
			{
				continue;
			}
			const FName TrashName = MakeUniqueObjectName(
				GetTransientPackage(),
				ExistingChildObject->GetClass(),
				*FString::Printf(TEXT("UAI_StaleSoundCueNode_%s"), *ExistingChildObject->GetName()));
			ExistingChildObject->Rename(
				*TrashName.ToString(),
				GetTransientPackage(),
				REN_DontCreateRedirectors | REN_ForceNoResetLoaders | REN_NonTransactional);
			ExistingChildObject->MarkAsGarbage();
		}
		TMap<FString, USoundNode*> NodeMap;
		TMap<FString, TArray<FString>> ChildIds;
		FString RootNodeId;
		NodesJson->TryGetStringField(TEXT("root_node_id"), RootNodeId);

		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		NodesJson->TryGetArrayField(TEXT("nodes"), Nodes);
		for (const TSharedPtr<FJsonValue>& Value : *Nodes)
		{
			const TSharedPtr<FJsonObject>* NodeObj = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(NodeObj) || !NodeObj || !NodeObj->IsValid())
			{
				continue;
			}
			FString Id;
			FString Type;
			FString ClassPath;
			(*NodeObj)->TryGetStringField(TEXT("id"), Id);
			(*NodeObj)->TryGetStringField(TEXT("type"), Type);
			(*NodeObj)->TryGetStringField(TEXT("class"), ClassPath);
			UClass* NodeClass = ResolveSoundNodeClass(Type, ClassPath);
			if (!NodeClass)
			{
				OutError = TEXT("unsupported_node_type");
				return false;
			}
			USoundNode* Node = Cue->ConstructSoundNode<USoundNode>(NodeClass, false);
			if (!Node)
			{
				OutError = TEXT("create_sound_node_failed");
				return false;
			}
			Node->CreateStartingConnectors();
			if (!Id.IsEmpty())
			{
				Node->Rename(*Id, Cue, REN_DontCreateRedirectors | REN_ForceNoResetLoaders);
			}
			ApplySoundNodeStructuredFields(Node, *NodeObj);
			const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
			if ((*NodeObj)->TryGetArrayField(TEXT("children"), Children) && Children)
			{
				TArray<FString>& TargetChildren = ChildIds.FindOrAdd(Id);
				for (const TSharedPtr<FJsonValue>& ChildValue : *Children)
				{
					if (ChildValue.IsValid())
					{
						FString ChildId = ChildValue->AsString();
						ChildId.TrimStartAndEndInline();
						if (!ChildId.IsEmpty())
						{
							TargetChildren.Add(ChildId);
						}
					}
				}
			}
			NodeMap.Add(Id, Node);
			++OutNodesCreated;
		}

		const TArray<TSharedPtr<FJsonValue>>* Edges = nullptr;
		if (EdgesJson.IsValid() && EdgesJson->TryGetArrayField(TEXT("edges"), Edges) && Edges)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Edges)
			{
				const TSharedPtr<FJsonObject>* EdgeObj = nullptr;
				if (!Value.IsValid() || !Value->TryGetObject(EdgeObj) || !EdgeObj || !EdgeObj->IsValid())
				{
					continue;
				}
				FString From;
				FString To;
				(*EdgeObj)->TryGetStringField(TEXT("from"), From);
				(*EdgeObj)->TryGetStringField(TEXT("to"), To);
				if (!From.IsEmpty() && !To.IsEmpty())
				{
					ChildIds.FindOrAdd(From).Add(To);
				}
			}
		}

		for (const TPair<FString, TArray<FString>>& Pair : ChildIds)
		{
			USoundNode* Parent = NodeMap.FindRef(Pair.Key);
			if (!Parent)
			{
				continue;
			}
			TArray<USoundNode*> Children;
			for (const FString& ChildId : Pair.Value)
			{
				if (USoundNode* Child = NodeMap.FindRef(ChildId))
				{
					Children.Add(Child);
					++OutEdgesCreated;
				}
			}
			Parent->SetChildNodes(Children);
			if (USoundNodeRandom* RandomNode = Cast<USoundNodeRandom>(Parent))
			{
				while (RandomNode->Weights.Num() < Children.Num())
				{
					RandomNode->Weights.Add(1.0f);
				}
				if (RandomNode->Weights.Num() > Children.Num())
				{
					RandomNode->Weights.SetNum(Children.Num());
				}
			}
		}
		if (RootNodeId.IsEmpty())
		{
			for (const TPair<FString, USoundNode*>& Pair : NodeMap)
			{
				RootNodeId = Pair.Key;
				break;
			}
		}
		Cue->FirstNode = NodeMap.FindRef(RootNodeId);
		if (!Cue->FirstNode && Cue->AllNodes.Num() > 0)
		{
			Cue->FirstNode = Cue->AllNodes[0];
		}
		for (USoundNode* SoundNode : Cue->AllNodes)
		{
			if (!SoundNode)
			{
				continue;
			}
			USoundCueGraphNode* GraphNode = Cast<USoundCueGraphNode>(SoundNode->GetGraphNode());
			if (!GraphNode)
			{
				OutError = TEXT("sound_cue_graph_node_missing");
				return false;
			}
			TArray<UEdGraphPin*> InputPins;
			GraphNode->GetInputPins(InputPins);
			while (InputPins.Num() < SoundNode->ChildNodes.Num())
			{
				const int32 PreviousCount = InputPins.Num();
				USoundCue::GetSoundCueAudioEditor()->CreateInputPin(GraphNode);
				InputPins.Reset();
				GraphNode->GetInputPins(InputPins);
				if (InputPins.Num() <= PreviousCount)
				{
					OutError = TEXT("sound_cue_create_input_pin_failed");
					return false;
				}
			}
			while (InputPins.Num() > SoundNode->ChildNodes.Num())
			{
				UEdGraphPin* Pin = InputPins.Last();
				if (Pin)
				{
					Pin->BreakAllPinLinks();
					GraphNode->RemovePin(Pin);
				}
				InputPins.Reset();
				GraphNode->GetInputPins(InputPins);
			}
			USoundCue::GetSoundCueAudioEditor()->RenameNodePins(SoundNode);
		}
		Cue->LinkGraphNodesFromSoundNodes();
		Cue->CompileSoundNodesFromGraphNodes();
		Cue->PostEditChange();
		Cue->MarkPackageDirty();
		return true;
	}
}

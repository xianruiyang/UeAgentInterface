// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Audio/Authoring/UeAgentAudioSoundCueAuthoring.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Utilities/EditorResources/UeAgentEditorResourcePersistence.h"
#include "Utilities/UeAgentUObjectProperties.h"
#include "FileHelpers.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
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

namespace UeAgentAudioAuthoring
{
	namespace
	{
		FString ObjectPathOrEmpty(const UObject* Object) { return Object ? Object->GetPathName() : FString(); }
		void FillAssetIdentity(UObject* Asset, const TSharedPtr<FJsonObject>& Out)
		{
			if (!Out.IsValid()) return;
			Out->SetStringField(TEXT("asset_path"), Asset && Asset->GetOutermost() ? Asset->GetOutermost()->GetName() : FString());
			Out->SetStringField(TEXT("object_path"), ObjectPathOrEmpty(Asset));
			Out->SetStringField(TEXT("class"), Asset && Asset->GetClass() ? Asset->GetClass()->GetName() : FString());
			Out->SetStringField(TEXT("class_path"), Asset && Asset->GetClass() ? Asset->GetClass()->GetPathName() : FString());
		}
		TSharedPtr<FJsonValue> JsonStringValue(const FString& Value) { return MakeShared<FJsonValueString>(Value); }
		void AddIssue(TArray<TSharedPtr<FJsonValue>>& Issues, const FString& Code, const FString& Severity, const FString& Message, const FString& Path = FString())
		{
			TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("code"), Code); Issue->SetStringField(TEXT("severity"), Severity); Issue->SetStringField(TEXT("message"), Message);
			if (!Path.IsEmpty()) Issue->SetStringField(TEXT("path"), Path);
			Issues.Add(MakeShared<FJsonValueObject>(Issue));
		}
		TSharedPtr<FJsonObject> BuildGenericAssetJson(UObject* Asset, bool bIncludeProperties)
		{
			UeAgentUObjectProperties::FExportOptions Options;
			Options.Profile = bIncludeProperties ? UeAgentUObjectProperties::EExportProfile::Authoring : UeAgentUObjectProperties::EExportProfile::Index;
			return UeAgentUObjectProperties::ExportObjectProperties(Asset, Options);
		}
	}
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
			Info->SetObjectField(TEXT("raw_properties"), BuildGenericAssetJson(Cue, true));
		}
		return Info;
	}
	bool SaveSoundCuePackages(const TArray<UObject*>& Objects, UE::UAI::Contract::FDiagnostic& OutError)
	{
		TArray<UPackage*> PackagesToSave;
		for (UObject* Object : Objects)
		{
			if (Object && Object->GetOutermost()) PackagesToSave.AddUnique(Object->GetOutermost());
		}
		if (PackagesToSave.Num() > 0 && !UE::UAI::EditorResources::SavePackages(PackagesToSave, true, OutError))
		{
			OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("save_asset_failed"), OutError, TEXT(""));
			return false;
		}
		return true;
	}
}

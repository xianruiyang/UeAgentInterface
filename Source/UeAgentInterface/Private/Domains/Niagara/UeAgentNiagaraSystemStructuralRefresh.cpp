// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Niagara/UeAgentNiagaraSystemStructuralRefresh.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Dom/JsonValue.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Module/UeAgentInterfaceLogger.h"
#include "Misc/App.h"
#include "Modules/ModuleManager.h"
#include "NiagaraEditorModule.h"
#include "NiagaraComponent.h"
#include "EdGraphSchema_Niagara.h"
#include "Editor.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNode.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemEditorData.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"
#include "ViewModels/NiagaraSystemViewModel.h"
#include "Subsystems/AssetEditorSubsystem.h"

namespace UeAgentNiagaraOps
{
	static TSharedRef<FNiagaraSystemViewModel> CreateDataProcessingViewModel(UNiagaraSystem& System)
	{
		const TSharedRef<FNiagaraSystemViewModel> ViewModel = MakeShared<FNiagaraSystemViewModel>();
		FNiagaraSystemViewModelOptions Options;
		Options.bCanModifyEmittersFromTimeline = false;
		Options.bCanAutoCompile = false;
		Options.bCanSimulate = false;
		Options.bIsForDataProcessingOnly = true;
		Options.EditMode = ENiagaraSystemViewModelEditMode::SystemAsset;
		Options.MessageLogGuid = System.GetAssetGuid();
		ViewModel->Initialize(System, Options);
		return ViewModel;
	}

	void ReleaseSystemInstancesForEmitterHandleEdit(UNiagaraSystem& System)
	{
		// NiagaraEditorUtilities::KillSystemInstances is not DLL-exported.
		// Use its native component operation with the same reference scope.
		TArray<UNiagaraComponent*> Components;
		for (TObjectIterator<UNiagaraComponent> It; It; ++It)
		{
			if (It->GetAsset() == &System) Components.Add(*It);
		}
		for (UNiagaraComponent* Component : Components) Component->DestroyInstance();
	}

	void RemoveEmitterHandlesForStructuralEdit(UNiagaraSystem& System, const TSet<FGuid>& HandleIds)
	{
		FNiagaraEditorModule& Module = FModuleManager::LoadModuleChecked<FNiagaraEditorModule>(TEXT("NiagaraEditor"));
		TSharedPtr<FNiagaraSystemViewModel> ViewModel = Module.GetExistingViewModelForSystem(&System);
		if (!ViewModel.IsValid()) ViewModel = CreateDataProcessingViewModel(System);
		// This exported editor entry owns instance teardown, compiled-data and
		// merge-cache cleanup, and both system graphs. Do not duplicate them.
		ViewModel->DeleteEmitters(HandleIds);
	}

	bool PrepareAssetForStructuralNiagaraEdit(
		UObject& Asset,
		int32& OutClosedEditorCount,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutClosedEditorCount = 0;
		OutError.Reset();
		if (IsRunningCommandlet() || !FApp::CanEverRender() || !GEditor)
		{
			return true;
		}

		UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		if (!AssetEditorSubsystem || !AssetEditorSubsystem->FindEditorForAsset(&Asset, false))
		{
			return true;
		}

		OutClosedEditorCount = AssetEditorSubsystem->CloseAllEditorsForAsset(&Asset);
		FUeAgentInterfaceLogger::Log(
			TEXT("Niagara structural edit closed open asset editors: asset=%s closed=%d"),
			*Asset.GetPathName(),
			OutClosedEditorCount);

		if (AssetEditorSubsystem->FindEditorForAsset(&Asset, false))
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("niagara_structural_edit_asset_editor_close_failed"),
				FString::Printf(TEXT("An editor for this Niagara asset remains open after closing %d editor(s)."), OutClosedEditorCount), TEXT("niagara_structural_edit"));
			OutError.TargetPath = Asset.GetPathName();
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = TEXT("asset_editor_open");
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(true);
			OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(false);
			return false;
		}
		if (UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(&Asset))
		{
			if (FNiagaraEditorModule* NiagaraEditorModule = FModuleManager::GetModulePtr<FNiagaraEditorModule>(TEXT("NiagaraEditor")))
			{
				if (NiagaraEditorModule->GetExistingViewModelForSystem(NiagaraSystem).IsValid())
				{
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("niagara_structural_edit_view_model_still_active"),
						TEXT("The Niagara system view model remains active after its asset editors were closed."), TEXT("niagara_structural_edit"));
					OutError.TargetPath = Asset.GetPathName();
					OutError.SourceKind = TEXT("native_result");
					OutError.PropertyPath = TEXT("system_view_model_active");
					OutError.ActualValue = MakeShared<FJsonValueBoolean>(true);
					OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(false);
					return false;
				}
			}
		}
		return true;
	}

	static void LinkGraphPins(UEdGraphPin* SourcePin, UEdGraphPin* TargetPin)
	{
		if (!SourcePin || !TargetPin)
		{
			return;
		}
		SourcePin->MakeLinkTo(TargetPin);
		if (UEdGraphNode* SourceNode = SourcePin->GetOwningNode())
		{
			SourceNode->PinConnectionListChanged(SourcePin);
		}
		if (UEdGraphNode* TargetNode = TargetPin->GetOwningNode())
		{
			TargetNode->PinConnectionListChanged(TargetPin);
		}
	}

	static UEdGraphPin* FindParameterMapPin(UNiagaraNode& Node, const EEdGraphPinDirection Direction)
	{
		const UEdGraphSchema_Niagara* NiagaraSchema = GetDefault<UEdGraphSchema_Niagara>();
		if (!NiagaraSchema)
		{
			return nullptr;
		}

		for (UEdGraphPin* Pin : Node.Pins)
		{
			if (Pin &&
				Pin->Direction == Direction &&
				NiagaraSchema->PinToTypeDefinition(Pin) == FNiagaraTypeDefinition::GetParameterMapDef())
			{
				return Pin;
			}
		}
		return nullptr;
	}

	static UClass* GetNiagaraNodeEmitterClass()
	{
		return StaticLoadClass(
			UNiagaraNode::StaticClass(),
			nullptr,
			TEXT("/Script/NiagaraEditor.NiagaraNodeEmitter"));
	}

	static bool SetObjectPropertyByName(UObject& Object, const FName PropertyName, UObject* Value)
	{
		if (FObjectProperty* ObjectProperty = FindFProperty<FObjectProperty>(Object.GetClass(), PropertyName))
		{
			ObjectProperty->SetObjectPropertyValue_InContainer(&Object, Value);
			return true;
		}
		return false;
	}

	static bool SetGuidPropertyByName(UObject& Object, const FName PropertyName, const FGuid& Value)
	{
		FStructProperty* StructProperty = FindFProperty<FStructProperty>(Object.GetClass(), PropertyName);
		if (!StructProperty || StructProperty->Struct != TBaseStructure<FGuid>::Get())
		{
			return false;
		}

		if (FGuid* PropertyValue = StructProperty->ContainerPtrToValuePtr<FGuid>(&Object))
		{
			*PropertyValue = Value;
			return true;
		}
		return false;
	}

	static bool SetNiagaraScriptUsagePropertyByName(UObject& Object, const FName PropertyName, const ENiagaraScriptUsage Value)
	{
		FProperty* Property = FindFProperty<FProperty>(Object.GetClass(), PropertyName);
		if (!Property)
		{
			return false;
		}

		if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			if (void* PropertyValue = EnumProperty->ContainerPtrToValuePtr<void>(&Object))
			{
				EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(PropertyValue, static_cast<int64>(Value));
				return true;
			}
		}
		if (FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			ByteProperty->SetPropertyValue_InContainer(&Object, static_cast<uint8>(Value));
			return true;
		}
		if (FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
		{
			if (void* PropertyValue = NumericProperty->ContainerPtrToValuePtr<void>(&Object))
			{
				NumericProperty->SetIntPropertyValue(PropertyValue, static_cast<int64>(Value));
				return true;
			}
		}
		return false;
	}

	static UNiagaraNodeOutput* FindSystemOutputNodeByUsage(UNiagaraGraph& Graph, const ENiagaraScriptUsage Usage)
	{
		TArray<UNiagaraNodeOutput*> OutputNodes;
		Graph.GetNodesOfClass<UNiagaraNodeOutput>(OutputNodes);
		for (UNiagaraNodeOutput* OutputNode : OutputNodes)
		{
			if (OutputNode && OutputNode->GetUsage() == Usage)
			{
				return OutputNode;
			}
		}
		return nullptr;
	}

	static bool RebuildSystemEmitterNodesForCompile(UNiagaraSystem& NiagaraSystem, int32& OutEmitterNodeCount)
	{
		OutEmitterNodeCount = 0;

		UNiagaraScript* SystemSpawnScript = NiagaraSystem.GetSystemSpawnScript();
		UNiagaraScriptSource* SystemScriptSource = SystemSpawnScript ? Cast<UNiagaraScriptSource>(SystemSpawnScript->GetLatestSource()) : nullptr;
		UNiagaraGraph* SystemGraph = SystemScriptSource ? SystemScriptSource->NodeGraph : nullptr;
		if (!SystemGraph)
		{
			return false;
		}
		UClass* NodeEmitterClass = GetNiagaraNodeEmitterClass();
		if (!NodeEmitterClass)
		{
			return false;
		}

		SystemGraph->Modify();

		TArray<UNiagaraNode*> ExistingNodes;
		SystemGraph->GetNodesOfClass<UNiagaraNode>(ExistingNodes);
		for (UNiagaraNode* ExistingEmitterNode : ExistingNodes)
		{
			if (!ExistingEmitterNode || !ExistingEmitterNode->IsA(NodeEmitterClass))
			{
				continue;
			}

			ExistingEmitterNode->Modify();
			UEdGraphPin* InputPin = FindParameterMapPin(*ExistingEmitterNode, EGPD_Input);
			UEdGraphPin* OutputPin = FindParameterMapPin(*ExistingEmitterNode, EGPD_Output);
			UEdGraphPin* PreviousPin = InputPin && InputPin->LinkedTo.Num() == 1 ? InputPin->LinkedTo[0] : nullptr;
			UEdGraphPin* NextPin = OutputPin && OutputPin->LinkedTo.Num() == 1 ? OutputPin->LinkedTo[0] : nullptr;
			ExistingEmitterNode->DestroyNode();
			if (PreviousPin && NextPin)
			{
				LinkGraphPins(PreviousPin, NextPin);
			}
		}

		TArray<UNiagaraNodeInput*> AllInputNodes;
		SystemGraph->GetNodesOfClass<UNiagaraNodeInput>(AllInputNodes);
		TArray<UNiagaraNodeInput*> ParameterMapInputNodes;
		for (UNiagaraNodeInput* InputNode : AllInputNodes)
		{
			if (InputNode && FindParameterMapPin(*InputNode, EGPD_Output))
			{
				ParameterMapInputNodes.Add(InputNode);
			}
		}

		FNiagaraVariable SharedInputVar(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("InputMap"));
		for (int32 StageIndex = 0; StageIndex < 2; ++StageIndex)
		{
			const ENiagaraScriptUsage SystemUsage = static_cast<ENiagaraScriptUsage>(static_cast<int32>(ENiagaraScriptUsage::SystemSpawnScript) + StageIndex);
			UNiagaraNodeOutput* OutputNode = FindSystemOutputNodeByUsage(*SystemGraph, SystemUsage);
			if (!OutputNode)
			{
				FGraphNodeCreator<UNiagaraNodeOutput> OutputNodeCreator(*SystemGraph);
				OutputNode = OutputNodeCreator.CreateNode();
				OutputNode->SetUsage(SystemUsage);
				OutputNode->Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("Out")));
				OutputNode->NodePosX = 0;
				OutputNode->NodePosY = StageIndex * 120;
				OutputNodeCreator.Finalize();
			}

			UEdGraphPin* OutputInputPin = FindParameterMapPin(*OutputNode, EGPD_Input);
			if (!OutputInputPin)
			{
				OutputNode->AllocateDefaultPins();
				OutputInputPin = FindParameterMapPin(*OutputNode, EGPD_Input);
			}
			if (!OutputInputPin)
			{
				continue;
			}

			UEdGraphPin* PreviousOutputPin = nullptr;
			if (OutputInputPin->LinkedTo.Num() == 1)
			{
				PreviousOutputPin = OutputInputPin->LinkedTo[0];
			}
			if (!PreviousOutputPin)
			{
				UNiagaraNodeInput* InputNode = ParameterMapInputNodes.IsValidIndex(StageIndex) ? ParameterMapInputNodes[StageIndex] : nullptr;

				if (!InputNode)
				{
					FGraphNodeCreator<UNiagaraNodeInput> InputNodeCreator(*SystemGraph);
					InputNode = InputNodeCreator.CreateNode();
					InputNode->Input = SharedInputVar;
					InputNode->Usage = ENiagaraInputNodeUsage::Parameter;
					InputNode->NodePosX = -350;
					InputNode->NodePosY = StageIndex * 120;
					InputNodeCreator.Finalize();
					ParameterMapInputNodes.Add(InputNode);
				}

				PreviousOutputPin = FindParameterMapPin(*InputNode, EGPD_Output);
				LinkGraphPins(PreviousOutputPin, OutputInputPin);
			}

			for (const FNiagaraEmitterHandle& EmitterHandle : NiagaraSystem.GetEmitterHandles())
			{
				if (!EmitterHandle.GetInstance().Emitter)
				{
					continue;
				}

				UNiagaraNode* EmitterNode = NewObject<UNiagaraNode>(SystemGraph, NodeEmitterClass, NAME_None, RF_Transactional);
				if (!EmitterNode)
				{
					continue;
				}
				EmitterNode->CreateNewGuid();
				SetObjectPropertyByName(*EmitterNode, TEXT("OwnerSystem"), &NiagaraSystem);
				SetGuidPropertyByName(*EmitterNode, TEXT("EmitterHandleId"), EmitterHandle.GetId());
				SetNiagaraScriptUsagePropertyByName(
					*EmitterNode,
					TEXT("ScriptType"),
					static_cast<ENiagaraScriptUsage>(static_cast<int32>(ENiagaraScriptUsage::EmitterSpawnScript) + StageIndex));
				EmitterNode->SetEnabledState(EmitterHandle.GetIsEnabled() ? ENodeEnabledState::Enabled : ENodeEnabledState::Disabled, false);
				EmitterNode->NodePosX = -220 + (OutEmitterNodeCount * 20);
				EmitterNode->NodePosY = StageIndex * 120;
				EmitterNode->AllocateDefaultPins();
				SystemGraph->AddNode(EmitterNode, true, false);

				UEdGraphPin* EmitterInputPin = FindParameterMapPin(*EmitterNode, EGPD_Input);
				UEdGraphPin* EmitterOutputPin = FindParameterMapPin(*EmitterNode, EGPD_Output);
				if (!EmitterInputPin || !EmitterOutputPin || !PreviousOutputPin)
				{
					continue;
				}

				OutputInputPin->BreakAllPinLinks(true);
				LinkGraphPins(PreviousOutputPin, EmitterInputPin);
				LinkGraphPins(EmitterOutputPin, OutputInputPin);
				PreviousOutputPin = EmitterOutputPin;
				++OutEmitterNodeCount;
			}
		}

		SystemGraph->NotifyGraphChanged();
		return true;
	}

	bool SynchronizeSystemEditorData(UNiagaraSystem& NiagaraSystem, int32& OutOverviewNodeCountBefore, int32& OutOverviewNodeCountAfter)
	{
		OutOverviewNodeCountBefore = INDEX_NONE;
		OutOverviewNodeCountAfter = INDEX_NONE;

		UNiagaraSystemEditorData* SystemEditorData = Cast<UNiagaraSystemEditorData>(NiagaraSystem.GetEditorData());
		if (!SystemEditorData)
		{
			return false;
		}

		UEdGraph* OverviewGraph = SystemEditorData->GetSystemOverviewGraph();
		OutOverviewNodeCountBefore = OverviewGraph ? OverviewGraph->Nodes.Num() : 0;

		NiagaraSystem.Modify();
		SystemEditorData->Modify();
		if (OverviewGraph)
		{
			OverviewGraph->Modify();
		}

		SystemEditorData->SynchronizeOverviewGraphWithSystem(NiagaraSystem);
		NiagaraSystem.ComputeEmittersExecutionOrder();
		NiagaraSystem.InvalidateCachedData();

		OverviewGraph = SystemEditorData->GetSystemOverviewGraph();
		OutOverviewNodeCountAfter = OverviewGraph ? OverviewGraph->Nodes.Num() : 0;
		if (OutOverviewNodeCountAfter != OutOverviewNodeCountBefore)
		{
			NiagaraSystem.MarkPackageDirty();
		}
		return true;
	}

	FNiagaraSystemRefreshResult RefreshNiagaraSystemAfterStructuralEdit(
		UNiagaraSystem& NiagaraSystem,
		const bool bBroadcastPostEditChange,
		const bool bMarkDirty)
	{
		FNiagaraSystemRefreshResult Result;
		Result.bPerformed = true;
		Result.EmitterCount = NiagaraSystem.GetEmitterHandles().Num();

		NiagaraSystem.Modify();
		int32 RebuiltEmitterNodeCount = 0;
		Result.bRebuiltEmitterNodes = RebuildSystemEmitterNodesForCompile(NiagaraSystem, RebuiltEmitterNodeCount);
		Result.bSynchronizedOverviewGraph = SynchronizeSystemEditorData(
			NiagaraSystem,
			Result.OverviewNodeCountBefore,
			Result.OverviewNodeCountAfter);
		Result.bComputedExecutionOrder = Result.bSynchronizedOverviewGraph;
		Result.bInvalidatedCachedData = Result.bSynchronizedOverviewGraph;

		if (bBroadcastPostEditChange)
		{
			if (FNiagaraEditorModule* NiagaraEditorModule = FModuleManager::GetModulePtr<FNiagaraEditorModule>(TEXT("NiagaraEditor")))
			{
				const TSharedPtr<FNiagaraSystemViewModel> ExistingViewModel = NiagaraEditorModule->GetExistingViewModelForSystem(&NiagaraSystem);
				Result.bHadExistingSystemViewModel = ExistingViewModel.IsValid();
				if (!ExistingViewModel.IsValid())
				{
					TSharedPtr<FNiagaraSystemViewModel> TemporaryViewModel = CreateDataProcessingViewModel(NiagaraSystem);
					Result.bCreatedDataProcessingViewModel = TemporaryViewModel.IsValid();
				}
			}
			NiagaraSystem.OnSystemPostEditChange().Broadcast(&NiagaraSystem);
			Result.bBroadcastPostEditChange = true;
		}

		if (bMarkDirty)
		{
			NiagaraSystem.MarkPackageDirty();
			Result.bMarkedDirty = true;
		}

		FUeAgentInterfaceLogger::Log(
			TEXT("Niagara RefreshSystemAfterStructuralEdit: system=%s emitters=%d overview_before=%d overview_after=%d sync=%s post_edit=%s dirty=%s"),
			*NiagaraSystem.GetPathName(),
			Result.EmitterCount,
			Result.OverviewNodeCountBefore,
			Result.OverviewNodeCountAfter,
			Result.bSynchronizedOverviewGraph ? TEXT("true") : TEXT("false"),
			Result.bBroadcastPostEditChange ? TEXT("true") : TEXT("false"),
			Result.bMarkedDirty ? TEXT("true") : TEXT("false"));

		return Result;
	}
}

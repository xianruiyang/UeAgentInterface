// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Animation/UeAgentAnimBlueprintDomainBackend.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAssetLifecycle.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "Engine/Blueprint.h"
#include "Utilities/EditorResources/UeAgentEditorResourcePersistence.h"

#include "Domains/Animation/UeAgentAnimBlueprintDomainBackend.h"
#include "Utilities/UeAgentDetailsAuthoring.h"
#include "Utilities/UeAgentJsonDiagnostics.h"
#include "Animation/AnimClassInterface.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimLayerInterface.h"
#include "Animation/Skeleton.h"
#include "AnimationGraph.h"
#include "AnimationGraphSchema.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AnimGraphNode_LinkedAnimLayer.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimGraphNode_StateMachineBase.h"
#include "AnimStateAliasNode.h"
#include "AnimStateConduitNode.h"
#include "AnimStateEntryNode.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "AnimationStateMachineGraph.h"
#include "BlueprintEditorLibrary.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Factories/AnimBlueprintFactory.h"
#include "FileHelpers.h"
#include "AnimationEditorUtils.h"
#include "Engine/SkeletalMesh.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Subsystems/AssetEditorSubsystem.h"

namespace UeAgentAnimBlueprintOps
{
	static UE::UAI::Contract::FDiagnostic BackendFailure(
		const TCHAR* Code, const FString& Reason, const UObject* Context = nullptr)
	{
		auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(
			Code, Reason, TEXT("animation_backend"), Context ? Context->GetPathName() : FString());
		Diagnostic.SourceKind = TEXT("native_result");
		const UBlueprint* Blueprint = Cast<UBlueprint>(Context);
		if (!Blueprint && Context) Blueprint = Context->GetTypedOuter<UBlueprint>();
		if (Blueprint) Diagnostic.TargetPath = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(*Blueprint);
		return Diagnostic;
	}

	FString NormalizeAssetPath(const FString& InPath)
	{
		FString OutPath = InPath;
		OutPath.TrimStartAndEndInline();

		int32 DotIndex = INDEX_NONE;
		if (OutPath.FindChar(TEXT('.'), DotIndex) && DotIndex > 0)
		{
			OutPath = OutPath.Left(DotIndex);
		}
		return OutPath;
	}

	static FString ToObjectPath(const FString& InPath)
	{
		FString InObjectPath = InPath;
		InObjectPath.TrimStartAndEndInline();
		if (InObjectPath.IsEmpty())
		{
			return FString();
		}
		if (InObjectPath.Contains(TEXT(".")))
		{
			return InObjectPath;
		}

		const FString AssetPath = NormalizeAssetPath(InObjectPath);
		const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		if (AssetName.IsEmpty())
		{
			return FString();
		}
		return FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName);
	}

	static UObject* LoadAssetObject(const FString& InPath)
	{
		const FString ObjectPath = ToObjectPath(InPath);
		if (ObjectPath.IsEmpty())
		{
			return nullptr;
		}
		if (UObject* Existing = FindObject<UObject>(nullptr, *ObjectPath))
		{
			return Existing;
		}
		return LoadObject<UObject>(nullptr, *ObjectPath);
	}

	bool AssetExists(const FString& InPath)
	{
		const FString ObjectPath = ToObjectPath(InPath);
		if (ObjectPath.IsEmpty())
		{
			return false;
		}

		if (FindObject<UObject>(nullptr, *ObjectPath))
		{
			return true;
		}

		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		return AssetRegistryModule.Get().GetAssetByObjectPath(FSoftObjectPath(ObjectPath)).IsValid();
	}

	UAnimBlueprint* LoadAnimBlueprint(const FString& InPath)
	{
		return Cast<UAnimBlueprint>(LoadAssetObject(InPath));
	}

	USkeleton* LoadSkeleton(const FString& InPath)
	{
		return Cast<USkeleton>(LoadAssetObject(InPath));
	}

	USkeletalMesh* LoadSkeletalMesh(const FString& InPath)
	{
		return Cast<USkeletalMesh>(LoadAssetObject(InPath));
	}

	UClass* ResolveClassByPath(const FString& InClassPath)
	{
		FString ClassPath = InClassPath;
		ClassPath.TrimStartAndEndInline();
		if (ClassPath.IsEmpty())
		{
			return nullptr;
		}

		if (UClass* Existing = FindObject<UClass>(nullptr, *ClassPath))
		{
			return Existing;
		}
		if (UClass* Loaded = LoadObject<UClass>(nullptr, *ClassPath))
		{
			return Loaded;
		}
		if (!ClassPath.EndsWith(TEXT("_C")))
		{
			const FString GeneratedClassPath = ClassPath + TEXT("_C");
			if (UClass* GeneratedClass = LoadObject<UClass>(nullptr, *GeneratedClassPath))
			{
				return GeneratedClass;
			}
		}
		return nullptr;
	}

	FString BlueprintStatusToString(const EBlueprintStatus InStatus)
	{
		switch (InStatus)
		{
		case BS_Unknown: return TEXT("Unknown");
		case BS_Dirty: return TEXT("Dirty");
		case BS_Error: return TEXT("Error");
		case BS_UpToDate: return TEXT("UpToDate");
		case BS_BeingCreated: return TEXT("BeingCreated");
		case BS_UpToDateWithWarnings: return TEXT("UpToDateWithWarnings");
		default: return TEXT("Unknown");
		}
	}

	static FString CompilerSeverityToString(const EMessageSeverity::Type InSeverity)
	{
		switch (InSeverity)
		{
		case EMessageSeverity::Error:
			return TEXT("Error");
		case EMessageSeverity::Warning:
			return TEXT("Warning");
		case EMessageSeverity::PerformanceWarning:
			return TEXT("PerformanceWarning");
		case EMessageSeverity::Info:
			return TEXT("Info");
		default:
			return TEXT("Unknown");
		}
	}

	bool ParseCompilerSeverityFilter(const FString& InFilterText, ECompilerMessageSeverityFilter& OutFilter)
	{
		FString Normalized = InFilterText.TrimStartAndEnd().ToLower();
		Normalized.ReplaceInline(TEXT("_"), TEXT(""));
		Normalized.ReplaceInline(TEXT("-"), TEXT(""));
		Normalized.ReplaceInline(TEXT(" "), TEXT(""));

		if (Normalized.IsEmpty() || Normalized == TEXT("all"))
		{
			OutFilter = ECompilerMessageSeverityFilter::All;
			return true;
		}
		if (Normalized == TEXT("error") || Normalized == TEXT("errors"))
		{
			OutFilter = ECompilerMessageSeverityFilter::Error;
			return true;
		}
		if (Normalized == TEXT("warning") || Normalized == TEXT("warnings") || Normalized == TEXT("warn"))
		{
			OutFilter = ECompilerMessageSeverityFilter::Warning;
			return true;
		}
		if (Normalized == TEXT("info") || Normalized == TEXT("note") || Normalized == TEXT("notes"))
		{
			OutFilter = ECompilerMessageSeverityFilter::Info;
			return true;
		}
		if (Normalized == TEXT("warningorerror") || Normalized == TEXT("warnorerror") || Normalized == TEXT("errorsandwarnings"))
		{
			OutFilter = ECompilerMessageSeverityFilter::WarningOrError;
			return true;
		}
		return false;
	}

	static bool ShouldIncludeCompilerMessage(const EMessageSeverity::Type InSeverity, const ECompilerMessageSeverityFilter InFilter)
	{
		switch (InFilter)
		{
		case ECompilerMessageSeverityFilter::All:
			return true;
		case ECompilerMessageSeverityFilter::Error:
			return InSeverity == EMessageSeverity::Error;
		case ECompilerMessageSeverityFilter::Warning:
			return InSeverity == EMessageSeverity::Warning || InSeverity == EMessageSeverity::PerformanceWarning;
		case ECompilerMessageSeverityFilter::Info:
			return InSeverity == EMessageSeverity::Info;
		case ECompilerMessageSeverityFilter::WarningOrError:
			return InSeverity == EMessageSeverity::Error || InSeverity == EMessageSeverity::Warning || InSeverity == EMessageSeverity::PerformanceWarning;
		default:
			return true;
		}
	}

	bool SaveAnimBlueprintPackage(UAnimBlueprint* AnimBlueprint, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!AnimBlueprint)
		{
			OutError = BackendFailure(TEXT("anim_blueprint_not_found"),
				TEXT("Saving an AnimBlueprint requires an available AnimBlueprint object."));
			OutError.ActualValue = MakeShared<FJsonValueNull>();
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("AnimBlueprint"));
			return false;
		}

		TArray<UPackage*> PackagesToSave;
		PackagesToSave.Add(AnimBlueprint->GetOutermost());
		if (!UE::UAI::EditorResources::SavePackages(PackagesToSave, false, OutError))
		{
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(*AnimBlueprint);
			if (OutError.SourceKind.IsEmpty()) OutError.SourceKind = TEXT("native_result");
			OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("save_asset_failed"), OutError, TEXT("asset_save"),
				OutError.IsEmpty() ? TEXT("Unreal did not save the AnimBlueprint package and returned no diagnostic.") : FString());
			return false;
		}
		return true;
	}


	FString PreviewApplicationMethodToString(const EPreviewAnimationBlueprintApplicationMethod InMethod)
	{
		switch (InMethod)
		{
		case EPreviewAnimationBlueprintApplicationMethod::LinkedLayers:
			return TEXT("linked_layers");
		case EPreviewAnimationBlueprintApplicationMethod::LinkedAnimGraph:
			return TEXT("linked_anim_graph");
		default:
			return TEXT("linked_layers");
		}
	}

	bool ParsePreviewApplicationMethod(const FString& InText, EPreviewAnimationBlueprintApplicationMethod& OutMethod)
	{
		FString Normalized = InText.TrimStartAndEnd().ToLower();
		Normalized.ReplaceInline(TEXT("_"), TEXT(""));
		Normalized.ReplaceInline(TEXT("-"), TEXT(""));
		Normalized.ReplaceInline(TEXT(" "), TEXT(""));

		if (Normalized.IsEmpty() || Normalized == TEXT("linkedlayers"))
		{
			OutMethod = EPreviewAnimationBlueprintApplicationMethod::LinkedLayers;
			return true;
		}
		if (Normalized == TEXT("linkedanimgraph"))
		{
			OutMethod = EPreviewAnimationBlueprintApplicationMethod::LinkedAnimGraph;
			return true;
		}
		return false;
	}

	void AppendAnimBlueprintGraphItems(
		UAnimBlueprint* AnimBlueprint,
		TArray<TSharedPtr<FJsonValue>>& OutGraphs,
		int32& OutAnimGraphCount,
		int32& OutLogicGraphCount)
	{
		OutAnimGraphCount = 0;
		OutLogicGraphCount = 0;
		OutGraphs.Reset();
		if (!AnimBlueprint)
		{
			return;
		}

		auto AppendGraphs = [&OutGraphs, &OutAnimGraphCount, &OutLogicGraphCount](const TArray<UEdGraph*>& InGraphs, const FString& BaseType)
		{
			for (UEdGraph* Graph : InGraphs)
			{
				if (!Graph)
				{
					continue;
				}

				const bool bIsAnimGraph = (BaseType == TEXT("function")) && AnimationEditorUtils::IsAnimGraph(Graph);
				if (bIsAnimGraph)
				{
					++OutAnimGraphCount;
				}
				else
				{
					++OutLogicGraphCount;
				}

				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("graph_name"), Graph->GetName());
				Item->SetStringField(TEXT("graph_path"), Graph->GetPathName());
				Item->SetStringField(TEXT("graph_type"), BaseType);
				Item->SetBoolField(TEXT("is_anim_graph"), bIsAnimGraph);
				Item->SetStringField(TEXT("graph_kind"), bIsAnimGraph ? TEXT("anim") : TEXT("logic"));
				OutGraphs.Add(MakeShared<FJsonValueObject>(Item));
			}
		};

		AppendGraphs(AnimBlueprint->UbergraphPages, TEXT("ubergraph"));
		AppendGraphs(AnimBlueprint->FunctionGraphs, TEXT("function"));
		AppendGraphs(AnimBlueprint->MacroGraphs, TEXT("macro"));
		AppendGraphs(AnimBlueprint->DelegateSignatureGraphs, TEXT("delegate"));
	}

	const IAnimClassInterface* ResolveAnimClassInterface(const UAnimBlueprint* AnimBlueprint)
	{
		if (!AnimBlueprint)
		{
			return nullptr;
		}

		const UClass* InfoClass = AnimBlueprint->SkeletonGeneratedClass ? AnimBlueprint->SkeletonGeneratedClass : AnimBlueprint->GeneratedClass;
		return InfoClass ? IAnimClassInterface::GetFromClass(InfoClass) : nullptr;
	}

	TArray<UEdGraph*> GatherAnimGraphs(UAnimBlueprint* AnimBlueprint)
	{
		TArray<UEdGraph*> AnimGraphs;
		if (!AnimBlueprint)
		{
			return AnimGraphs;
		}

		for (UEdGraph* Graph : AnimBlueprint->FunctionGraphs)
		{
			if (Graph && AnimationEditorUtils::IsAnimGraph(Graph))
			{
				AnimGraphs.Add(Graph);
			}
		}

		return AnimGraphs;
	}

	FString GuidToString(const FGuid& InGuid)
	{
		return InGuid.IsValid() ? InGuid.ToString() : TEXT("");
	}

	TArray<TSharedPtr<FJsonValue>> MakeStringArray(const TArray<FString>& InValues)
	{
		TArray<TSharedPtr<FJsonValue>> OutValues;
		OutValues.Reserve(InValues.Num());
		for (const FString& Value : InValues)
		{
			OutValues.Add(MakeShared<FJsonValueString>(Value));
		}
		return OutValues;
	}

	FString NodeGuidToString(const UEdGraphNode* Node)
	{
		return Node ? Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower) : TEXT("");
	}

	bool ParseNodeGuid(const FString& InNodeGuid, FGuid& OutGuid)
	{
		FString NodeGuid = InNodeGuid;
		NodeGuid.TrimStartAndEndInline();
		if (NodeGuid.IsEmpty())
		{
			return false;
		}

		return FGuid::Parse(NodeGuid, OutGuid);
	}

	UClass* ResolveAnimLayerInterfaceClass(const FString& InClassPath)
	{
		if (UClass* Class = ResolveClassByPath(InClassPath))
		{
			return Class;
		}

		if (UBlueprint* BlueprintAsset = Cast<UBlueprint>(LoadAssetObject(InClassPath)))
		{
			if (BlueprintAsset->GeneratedClass)
			{
				return BlueprintAsset->GeneratedClass;
			}
			if (BlueprintAsset->SkeletonGeneratedClass)
			{
				return BlueprintAsset->SkeletonGeneratedClass;
			}
		}

		return nullptr;
	}

	bool IsLayerInterfaceBlueprint(const UAnimBlueprint* AnimBlueprint)
	{
		return AnimBlueprint && AnimBlueprint->BlueprintType == BPTYPE_Interface;
	}

	bool IsAnimLayerGraph(const UEdGraph* Graph)
	{
		return Graph && AnimationEditorUtils::IsAnimGraph(const_cast<UEdGraph*>(Graph)) && Graph->GetFName() != UEdGraphSchema_K2::GN_AnimGraph;
	}

	UEdGraph* ResolveAnyAnimBlueprintGraph(UAnimBlueprint* AnimBlueprint, const FString& InGraphName)
	{
		if (!AnimBlueprint)
		{
			return nullptr;
		}

		FString GraphName = InGraphName;
		GraphName.TrimStartAndEndInline();
		if (GraphName.IsEmpty() || GraphName.Equals(TEXT("EventGraph"), ESearchCase::IgnoreCase))
		{
			return UBlueprintEditorLibrary::FindEventGraph(AnimBlueprint);
		}

		TArray<UEdGraph*> AllGraphs;
		AnimBlueprint->GetAllGraphs(AllGraphs);
		for (UEdGraph* Graph : AllGraphs)
		{
			if (Graph && Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
			{
				return Graph;
			}
		}

		return nullptr;
	}

	bool FinalizeAnimBlueprintStructureMutation(UAnimBlueprint* AnimBlueprint, const bool bCompile, const bool bSave, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!AnimBlueprint)
		{
			OutError = BackendFailure(TEXT("anim_blueprint_not_found"),
				TEXT("Finalizing an animation structure mutation requires an available AnimBlueprint object."));
			OutError.ActualValue = MakeShared<FJsonValueNull>();
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("AnimBlueprint"));
			return false;
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBlueprint);
		if (bCompile)
		{
			FKismetEditorUtilities::CompileBlueprint(AnimBlueprint);
		}
		if (bSave)
		{
			return SaveAnimBlueprintPackage(AnimBlueprint, OutError);
		}
		return true;
	}

	FString StateNodeTypeToString(const UAnimStateNodeBase* StateNode)
	{
		if (Cast<const UAnimStateNode>(StateNode))
		{
			return TEXT("state");
		}
		if (Cast<const UAnimStateConduitNode>(StateNode))
		{
			return TEXT("conduit");
		}
		if (Cast<const UAnimStateAliasNode>(StateNode))
		{
			return TEXT("alias");
		}
		return TEXT("state_node");
	}

	TArray<FString> CollectAliasTargetNames(const UAnimStateAliasNode* AliasNode)
	{
		TArray<FString> AliasTargets;
		if (!AliasNode)
		{
			return AliasTargets;
		}

		for (const TWeakObjectPtr<UAnimStateNodeBase>& AliasTarget : AliasNode->GetAliasedStates())
		{
			if (const UAnimStateNodeBase* TargetNode = AliasTarget.Get())
			{
				AliasTargets.Add(TargetNode->GetStateName());
			}
		}

		return AliasTargets;
	}

	TSharedPtr<FJsonObject> BuildStateNodeJson(const UAnimStateNodeBase* StateNode)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("node_type"), StateNodeTypeToString(StateNode));
		Item->SetStringField(TEXT("state_name"), StateNode ? StateNode->GetStateName() : TEXT(""));
		Item->SetStringField(TEXT("bound_graph_path"), (StateNode && StateNode->GetBoundGraph()) ? StateNode->GetBoundGraph()->GetPathName() : TEXT(""));
		Item->SetNumberField(TEXT("node_pos_x"), StateNode ? StateNode->NodePosX : 0);
		Item->SetNumberField(TEXT("node_pos_y"), StateNode ? StateNode->NodePosY : 0);

		if (const UAnimStateAliasNode* AliasNode = Cast<UAnimStateAliasNode>(StateNode))
		{
			Item->SetBoolField(TEXT("global_alias"), AliasNode->bGlobalAlias);
			Item->SetArrayField(TEXT("alias_targets"), MakeStringArray(CollectAliasTargetNames(AliasNode)));
		}

		if (const UAnimStateNode* PlainStateNode = Cast<UAnimStateNode>(StateNode))
		{
			Item->SetBoolField(TEXT("always_reset_on_entry"), PlainStateNode->bAlwaysResetOnEntry);
			Item->SetStringField(
				TEXT("state_type"),
				PlainStateNode->StateType == AST_BlendGraph ? TEXT("blend_graph") : TEXT("single_animation"));
		}

		return Item;
	}

	TSharedPtr<FJsonObject> BuildTransitionJson(const UAnimStateTransitionNode* TransitionNode)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("source_state_name"), (TransitionNode && TransitionNode->GetPreviousState()) ? TransitionNode->GetPreviousState()->GetStateName() : TEXT(""));
		Item->SetStringField(TEXT("target_state_name"), (TransitionNode && TransitionNode->GetNextState()) ? TransitionNode->GetNextState()->GetStateName() : TEXT(""));
		Item->SetStringField(TEXT("bound_graph_path"), (TransitionNode && TransitionNode->GetBoundGraph()) ? TransitionNode->GetBoundGraph()->GetPathName() : TEXT(""));
		Item->SetNumberField(TEXT("priority_order"), TransitionNode ? TransitionNode->PriorityOrder : 0);
		Item->SetNumberField(TEXT("crossfade_duration"), TransitionNode ? TransitionNode->CrossfadeDuration : 0.0);
		Item->SetBoolField(TEXT("bidirectional"), TransitionNode ? TransitionNode->Bidirectional : false);
		Item->SetBoolField(TEXT("disabled"), TransitionNode ? TransitionNode->bDisabled : false);
		Item->SetBoolField(TEXT("automatic_rule"), TransitionNode ? TransitionNode->bAutomaticRuleBasedOnSequencePlayerInState : false);
		return Item;
	}

	UAnimGraphNode_StateMachineBase* FindStateMachineNode(UAnimBlueprint* AnimBlueprint, const FString& StateMachineName)
	{
		if (!AnimBlueprint)
		{
			return nullptr;
		}

		for (UEdGraph* AnimGraph : GatherAnimGraphs(AnimBlueprint))
		{
			TArray<UAnimGraphNode_StateMachineBase*> StateMachineNodes;
			AnimGraph->GetNodesOfClass<UAnimGraphNode_StateMachineBase>(StateMachineNodes);
			for (UAnimGraphNode_StateMachineBase* StateMachineNode : StateMachineNodes)
			{
				if (!StateMachineNode)
				{
					continue;
				}

				const FString GraphName = StateMachineNode->EditorStateMachineGraph ? StateMachineNode->EditorStateMachineGraph->GetName() : StateMachineNode->GetStateMachineName();
				if (GraphName.Equals(StateMachineName, ESearchCase::IgnoreCase))
				{
					return StateMachineNode;
				}
			}
		}

		return nullptr;
	}

	UAnimStateNodeBase* FindStateNodeByName(UAnimationStateMachineGraph* StateMachineGraph, const FString& StateName)
	{
		if (!StateMachineGraph)
		{
			return nullptr;
		}

		TArray<UAnimStateNodeBase*> StateNodes;
		StateMachineGraph->GetNodesOfClass<UAnimStateNodeBase>(StateNodes);
		for (UAnimStateNodeBase* StateNode : StateNodes)
		{
			if (StateNode && StateNode->GetStateName().Equals(StateName, ESearchCase::IgnoreCase))
			{
				return StateNode;
			}
		}

		return nullptr;
	}

	UAnimStateNodeBase* FindStateNodeByGuid(UAnimationStateMachineGraph* StateMachineGraph, const FGuid& NodeGuid)
	{
		if (!StateMachineGraph || !NodeGuid.IsValid())
		{
			return nullptr;
		}

		TArray<UAnimStateNodeBase*> StateNodes;
		StateMachineGraph->GetNodesOfClass<UAnimStateNodeBase>(StateNodes);
		for (UAnimStateNodeBase* StateNode : StateNodes)
		{
			if (StateNode && StateNode->NodeGuid == NodeGuid)
			{
				return StateNode;
			}
		}

		return nullptr;
	}

	UAnimStateTransitionNode* FindTransitionNode(UAnimationStateMachineGraph* StateMachineGraph, const FString& SourceStateName, const FString& TargetStateName)
	{
		if (!StateMachineGraph)
		{
			return nullptr;
		}

		TArray<UAnimStateTransitionNode*> TransitionNodes;
		StateMachineGraph->GetNodesOfClass<UAnimStateTransitionNode>(TransitionNodes);
		for (UAnimStateTransitionNode* TransitionNode : TransitionNodes)
		{
			if (!TransitionNode)
			{
				continue;
			}

			const UAnimStateNodeBase* SourceNode = TransitionNode->GetPreviousState();
			const UAnimStateNodeBase* TargetNode = TransitionNode->GetNextState();
			if (SourceNode && TargetNode &&
				SourceNode->GetStateName().Equals(SourceStateName, ESearchCase::IgnoreCase) &&
				TargetNode->GetStateName().Equals(TargetStateName, ESearchCase::IgnoreCase))
			{
				return TransitionNode;
			}
		}

		return nullptr;
	}

	UAnimStateTransitionNode* FindTransitionNodeByGuid(UAnimationStateMachineGraph* StateMachineGraph, const FGuid& NodeGuid)
	{
		if (!StateMachineGraph || !NodeGuid.IsValid())
		{
			return nullptr;
		}

		TArray<UAnimStateTransitionNode*> TransitionNodes;
		StateMachineGraph->GetNodesOfClass<UAnimStateTransitionNode>(TransitionNodes);
		for (UAnimStateTransitionNode* TransitionNode : TransitionNodes)
		{
			if (TransitionNode && TransitionNode->NodeGuid == NodeGuid)
			{
				return TransitionNode;
			}
		}

		return nullptr;
	}

	UAnimStateNodeBase* GetEntryConnectedState(UAnimationStateMachineGraph* StateMachineGraph)
	{
		if (!StateMachineGraph || !StateMachineGraph->EntryNode)
		{
			return nullptr;
		}

		return Cast<UAnimStateNodeBase>(StateMachineGraph->EntryNode->GetOutputNode());
	}

	bool ClearEntryState(UAnimationStateMachineGraph* StateMachineGraph, int32& OutBrokenLinkCount, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutBrokenLinkCount = 0;
		if (!StateMachineGraph || !StateMachineGraph->EntryNode)
		{
			OutError = BackendFailure(TEXT("state_machine_has_no_entry_node"),
				StateMachineGraph ? TEXT("The native state-machine graph has no entry node.")
					: TEXT("The native state-machine graph is unavailable."), StateMachineGraph);
			OutError.PropertyPath = StateMachineGraph ? TEXT("entry_node") : TEXT("state_machine_graph");
			OutError.ActualValuePresent = false;
			OutError.ExpectedValue = MakeShared<FJsonValueString>(StateMachineGraph ? TEXT("state-machine entry node") : TEXT("AnimationStateMachineGraph"));
			return false;
		}

		UEdGraphPin* EntryPin = StateMachineGraph->EntryNode->Pins.Num() > 0 ? StateMachineGraph->EntryNode->Pins[0] : nullptr;
		if (!EntryPin)
		{
			OutError = BackendFailure(TEXT("state_machine_entry_pin_not_found"),
				TEXT("The native state-machine entry node has no output pin at index 0."), StateMachineGraph);
			OutError.PropertyPath = TEXT("entry_node/pins/0");
			OutError.ActualValuePresent = false;
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("entry output pin"));
			return false;
		}

		OutBrokenLinkCount = EntryPin->LinkedTo.Num();
		if (OutBrokenLinkCount == 0)
		{
			return true;
		}

		for (UEdGraphPin* LinkedPin : EntryPin->LinkedTo)
		{
			if (LinkedPin)
			{
				LinkedPin->Modify();
			}
		}

		EntryPin->Modify();
		EntryPin->BreakAllPinLinks();
		return true;
	}

	bool SetEntryState(UAnimationStateMachineGraph* StateMachineGraph, UAnimStateNodeBase* TargetStateNode, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!StateMachineGraph || !StateMachineGraph->EntryNode)
		{
			OutError = BackendFailure(TEXT("state_machine_has_no_entry_node"),
				StateMachineGraph ? TEXT("The native state-machine graph has no entry node.")
					: TEXT("The native state-machine graph is unavailable."), StateMachineGraph);
			OutError.PropertyPath = StateMachineGraph ? TEXT("entry_node") : TEXT("state_machine_graph");
			OutError.ActualValuePresent = false;
			OutError.ExpectedValue = MakeShared<FJsonValueString>(StateMachineGraph ? TEXT("state-machine entry node") : TEXT("AnimationStateMachineGraph"));
			return false;
		}
		if (!TargetStateNode)
		{
			OutError = BackendFailure(TEXT("entry_state_not_found"),
				TEXT("The requested entry state does not resolve to a state node in this state machine."), StateMachineGraph);
			// The caller owns the selector and its original input location.
			OutError.SourceKind.Reset();
			OutError.PropertyPath = TEXT("entry_state");
			OutError.ActualValuePresent = false;
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("state node in the selected state machine"));
			return false;
		}

		UEdGraphPin* EntryPin = StateMachineGraph->EntryNode->Pins.Num() > 0 ? StateMachineGraph->EntryNode->Pins[0] : nullptr;
		UEdGraphPin* TargetInputPin = TargetStateNode->GetInputPin();
		if (!EntryPin || !TargetInputPin)
		{
			OutError = BackendFailure(TEXT("state_machine_entry_connection_failed"),
				TEXT("Connecting a state-machine entry requires both the entry output pin and the target state's input pin."), StateMachineGraph);
			OutError.PropertyPath = TEXT("entry_connection");
			auto ActualPins = MakeShared<FJsonObject>();
			ActualPins->SetBoolField(TEXT("entry_output_pin_available"), EntryPin != nullptr);
			ActualPins->SetBoolField(TEXT("state_input_pin_available"), TargetInputPin != nullptr);
			OutError.ActualValue = MakeShared<FJsonValueObject>(ActualPins);
			auto ExpectedPins = MakeShared<FJsonObject>();
			ExpectedPins->SetBoolField(TEXT("entry_output_pin_available"), true);
			ExpectedPins->SetBoolField(TEXT("state_input_pin_available"), true);
			OutError.ExpectedValue = MakeShared<FJsonValueObject>(ExpectedPins);
			return false;
		}

		if (EntryPin->LinkedTo.Num() == 1 && EntryPin->LinkedTo[0] == TargetInputPin)
		{
			return true;
		}

		int32 IgnoredBrokenLinkCount = 0;
		if (!ClearEntryState(StateMachineGraph, IgnoredBrokenLinkCount, OutError))
		{
			return false;
		}

		const UEdGraphSchema* Schema = StateMachineGraph->GetSchema();
		if (!Schema)
		{
			OutError = BackendFailure(TEXT("state_machine_schema_not_found"),
				TEXT("The native state-machine graph has no schema to connect the entry state."), StateMachineGraph);
			OutError.PropertyPath = TEXT("schema");
			OutError.ActualValuePresent = false;
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("state-machine graph schema"));
			if (IgnoredBrokenLinkCount > 0) OutError.MutationStarted = true;
			return false;
		}

		EntryPin->Modify();
		TargetInputPin->Modify();
		const FPinConnectionResponse ConnectionResponse = Schema->CanCreateConnection(EntryPin, TargetInputPin);
		if (!Schema->TryCreateConnection(EntryPin, TargetInputPin))
		{
			OutError = BackendFailure(TEXT("set_entry_state_failed"),
				ConnectionResponse.Response == CONNECT_RESPONSE_DISALLOW && !ConnectionResponse.Message.IsEmpty()
					? FString::Printf(TEXT("The native state-machine schema rejected the entry connection: %s"), *ConnectionResponse.Message.ToString())
					: FString(TEXT("The native state-machine schema did not create the entry-to-state connection.")), StateMachineGraph);
			OutError.PropertyPath = TEXT("entry_connection");
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(EntryPin->LinkedTo.Contains(TargetInputPin));
			OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			OutError.MutationMayHaveStarted = true;
			if (IgnoredBrokenLinkCount > 0 || EntryPin->LinkedTo.Contains(TargetInputPin)) OutError.MutationStarted = true;
			return false;
		}

		return true;
	}
}

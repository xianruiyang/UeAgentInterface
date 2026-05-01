// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_AnimBlueprint.h"

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
	enum class ECompilerMessageSeverityFilter : uint8
	{
		All,
		Error,
		Warning,
		Info,
		WarningOrError
	};

	static FString NormalizeAssetPath(const FString& InPath)
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

	static bool AssetExists(const FString& InPath)
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

	static UAnimBlueprint* LoadAnimBlueprint(const FString& InPath)
	{
		return Cast<UAnimBlueprint>(LoadAssetObject(InPath));
	}

	static USkeleton* LoadSkeleton(const FString& InPath)
	{
		return Cast<USkeleton>(LoadAssetObject(InPath));
	}

	static USkeletalMesh* LoadSkeletalMesh(const FString& InPath)
	{
		return Cast<USkeletalMesh>(LoadAssetObject(InPath));
	}

	static UClass* ResolveClassByPath(const FString& InClassPath)
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

	static FString BlueprintStatusToString(const EBlueprintStatus InStatus)
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

	static bool ParseCompilerSeverityFilter(const FString& InFilterText, ECompilerMessageSeverityFilter& OutFilter)
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

	static void BuildCompilerLogJson(
		const FCompilerResultsLog& InCompilerLog,
		const ECompilerMessageSeverityFilter InFilter,
		const int32 InMaxMessages,
		TArray<TSharedPtr<FJsonValue>>& OutMessages,
		int32& OutFilteredErrorCount,
		int32& OutFilteredWarningCount,
		int32& OutFilteredInfoCount)
	{
		OutFilteredErrorCount = 0;
		OutFilteredWarningCount = 0;
		OutFilteredInfoCount = 0;
		OutMessages.Reset();

		const int32 MaxMessages = FMath::Clamp(InMaxMessages, 1, 2000);
		for (const TSharedRef<FTokenizedMessage>& Message : InCompilerLog.Messages)
		{
			const EMessageSeverity::Type Severity = Message->GetSeverity();
			if (!ShouldIncludeCompilerMessage(Severity, InFilter))
			{
				continue;
			}

			if (Severity == EMessageSeverity::Error)
			{
				++OutFilteredErrorCount;
			}
			else if (Severity == EMessageSeverity::Warning || Severity == EMessageSeverity::PerformanceWarning)
			{
				++OutFilteredWarningCount;
			}
			else if (Severity == EMessageSeverity::Info)
			{
				++OutFilteredInfoCount;
			}

			if (OutMessages.Num() >= MaxMessages)
			{
				continue;
			}

			TSharedPtr<FJsonObject> MessageObj = MakeShared<FJsonObject>();
			MessageObj->SetStringField(TEXT("severity"), CompilerSeverityToString(Severity));
			MessageObj->SetStringField(TEXT("message"), Message->ToText().ToString());
			OutMessages.Add(MakeShared<FJsonValueObject>(MessageObj));
		}
	}

	static bool SaveAnimBlueprintPackage(UAnimBlueprint* AnimBlueprint, FString& OutError)
	{
		if (!AnimBlueprint)
		{
			OutError = TEXT("anim_blueprint_not_found");
			return false;
		}

		TArray<UPackage*> PackagesToSave;
		PackagesToSave.Add(AnimBlueprint->GetOutermost());
		if (!UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, false))
		{
			OutError = TEXT("save_asset_failed");
			return false;
		}
		return true;
	}

	static bool ResolveAnimBlueprintParentClass(UClass* ParentClass, const USkeleton* TargetSkeleton, const bool bIsTemplate, FString& OutError)
	{
		if (!ParentClass || !ParentClass->IsChildOf(UAnimInstance::StaticClass()) || !FKismetEditorUtilities::CanCreateBlueprintOfClass(ParentClass))
		{
			OutError = TEXT("invalid_parent_class");
			return false;
		}

		if (!bIsTemplate)
		{
			if (const UAnimBlueprintGeneratedClass* GeneratedParent = Cast<UAnimBlueprintGeneratedClass>(ParentClass))
			{
				if (GeneratedParent->GetTargetSkeleton() != nullptr && TargetSkeleton != nullptr && !TargetSkeleton->IsCompatibleForEditor(GeneratedParent->GetTargetSkeleton()))
				{
					OutError = TEXT("parent_class_target_skeleton_mismatch");
					return false;
				}
			}
		}

		return true;
	}

	static FString PreviewApplicationMethodToString(const EPreviewAnimationBlueprintApplicationMethod InMethod)
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

	static bool ParsePreviewApplicationMethod(const FString& InText, EPreviewAnimationBlueprintApplicationMethod& OutMethod)
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

	static void AppendAnimBlueprintGraphItems(
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

	static const IAnimClassInterface* ResolveAnimClassInterface(const UAnimBlueprint* AnimBlueprint)
	{
		if (!AnimBlueprint)
		{
			return nullptr;
		}

		const UClass* InfoClass = AnimBlueprint->SkeletonGeneratedClass ? AnimBlueprint->SkeletonGeneratedClass : AnimBlueprint->GeneratedClass;
		return InfoClass ? IAnimClassInterface::GetFromClass(InfoClass) : nullptr;
	}

	static TArray<UEdGraph*> GatherAnimGraphs(UAnimBlueprint* AnimBlueprint)
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

	static FString GuidToString(const FGuid& InGuid)
	{
		return InGuid.IsValid() ? InGuid.ToString() : TEXT("");
	}

	static TArray<TSharedPtr<FJsonValue>> MakeStringArray(const TArray<FString>& InValues)
	{
		TArray<TSharedPtr<FJsonValue>> OutValues;
		OutValues.Reserve(InValues.Num());
		for (const FString& Value : InValues)
		{
			OutValues.Add(MakeShared<FJsonValueString>(Value));
		}
		return OutValues;
	}

	static FString NodeGuidToString(const UEdGraphNode* Node)
	{
		return Node ? Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower) : TEXT("");
	}

	static bool ParseNodeGuid(const FString& InNodeGuid, FGuid& OutGuid)
	{
		FString NodeGuid = InNodeGuid;
		NodeGuid.TrimStartAndEndInline();
		if (NodeGuid.IsEmpty())
		{
			return false;
		}

		return FGuid::Parse(NodeGuid, OutGuid);
	}

	static UClass* ResolveAnimLayerInterfaceClass(const FString& InClassPath)
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

	static bool IsLayerInterfaceBlueprint(const UAnimBlueprint* AnimBlueprint)
	{
		return AnimBlueprint && AnimBlueprint->BlueprintType == BPTYPE_Interface;
	}

	static bool IsAnimLayerGraph(const UEdGraph* Graph)
	{
		return Graph && AnimationEditorUtils::IsAnimGraph(const_cast<UEdGraph*>(Graph)) && Graph->GetFName() != UEdGraphSchema_K2::GN_AnimGraph;
	}

	static UEdGraph* ResolveAnyAnimBlueprintGraph(UAnimBlueprint* AnimBlueprint, const FString& InGraphName)
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

	static bool FinalizeAnimBlueprintStructureMutation(UAnimBlueprint* AnimBlueprint, const bool bCompile, const bool bSave, FString& OutError)
	{
		if (!AnimBlueprint)
		{
			OutError = TEXT("anim_blueprint_not_found");
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

	template<typename NodeType>
	static NodeType* AddNodeToGraph(UEdGraph* Graph, const int32 PosX, const int32 PosY)
	{
		if (!Graph)
		{
			return nullptr;
		}

		NodeType* NewNode = NewObject<NodeType>(Graph);
		if (!NewNode)
		{
			return nullptr;
		}

		Graph->AddNode(NewNode, /*bFromUI=*/true, /*bSelectNewNode=*/false);
		NewNode->CreateNewGuid();
		NewNode->PostPlacedNewNode();
		NewNode->AllocateDefaultPins();
		NewNode->NodePosX = PosX;
		NewNode->NodePosY = PosY;
		return NewNode;
	}

	static FString StateNodeTypeToString(const UAnimStateNodeBase* StateNode)
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

	static TArray<FString> CollectAliasTargetNames(const UAnimStateAliasNode* AliasNode)
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

	static TSharedPtr<FJsonObject> BuildStateNodeJson(const UAnimStateNodeBase* StateNode)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("node_guid"), NodeGuidToString(StateNode));
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

	static TSharedPtr<FJsonObject> BuildTransitionJson(const UAnimStateTransitionNode* TransitionNode)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("node_guid"), NodeGuidToString(TransitionNode));
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

	static UAnimGraphNode_StateMachineBase* FindStateMachineNode(UAnimBlueprint* AnimBlueprint, const FString& StateMachineName)
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

	static UAnimStateNodeBase* FindStateNodeByName(UAnimationStateMachineGraph* StateMachineGraph, const FString& StateName)
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

	static UAnimStateNodeBase* FindStateNodeByGuid(UAnimationStateMachineGraph* StateMachineGraph, const FGuid& NodeGuid)
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

	static UAnimStateTransitionNode* FindTransitionNode(UAnimationStateMachineGraph* StateMachineGraph, const FString& SourceStateName, const FString& TargetStateName)
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

	static UAnimStateTransitionNode* FindTransitionNodeByGuid(UAnimationStateMachineGraph* StateMachineGraph, const FGuid& NodeGuid)
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

	static UAnimStateNodeBase* GetEntryConnectedState(UAnimationStateMachineGraph* StateMachineGraph)
	{
		if (!StateMachineGraph || !StateMachineGraph->EntryNode)
		{
			return nullptr;
		}

		return Cast<UAnimStateNodeBase>(StateMachineGraph->EntryNode->GetOutputNode());
	}

	static bool ClearEntryState(UAnimationStateMachineGraph* StateMachineGraph, int32& OutBrokenLinkCount, FString& OutError)
	{
		OutBrokenLinkCount = 0;
		if (!StateMachineGraph || !StateMachineGraph->EntryNode)
		{
			OutError = TEXT("state_machine_has_no_entry_node");
			return false;
		}

		UEdGraphPin* EntryPin = StateMachineGraph->EntryNode->Pins.Num() > 0 ? StateMachineGraph->EntryNode->Pins[0] : nullptr;
		if (!EntryPin)
		{
			OutError = TEXT("state_machine_entry_pin_not_found");
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

	static bool SetEntryState(UAnimationStateMachineGraph* StateMachineGraph, UAnimStateNodeBase* TargetStateNode, FString& OutError)
	{
		if (!StateMachineGraph || !StateMachineGraph->EntryNode)
		{
			OutError = TEXT("state_machine_has_no_entry_node");
			return false;
		}
		if (!TargetStateNode)
		{
			OutError = TEXT("entry_state_not_found");
			return false;
		}

		UEdGraphPin* EntryPin = StateMachineGraph->EntryNode->Pins.Num() > 0 ? StateMachineGraph->EntryNode->Pins[0] : nullptr;
		UEdGraphPin* TargetInputPin = TargetStateNode->GetInputPin();
		if (!EntryPin || !TargetInputPin)
		{
			OutError = TEXT("state_machine_entry_connection_failed");
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
			OutError = TEXT("state_machine_schema_not_found");
			return false;
		}

		EntryPin->Modify();
		TargetInputPin->Modify();
		if (!Schema->TryCreateConnection(EntryPin, TargetInputPin))
		{
			OutError = TEXT("set_entry_state_failed");
			return false;
		}

		return true;
	}
}

bool FUeAgentHttpServer::ExecuteAnimBlueprintCommand(const FString& CommandLower, const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (CommandLower == TEXT("anim_blueprint_create")) return CmdAnimBlueprintCreate(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_create_layer_interface")) return CmdAnimBlueprintCreateLayerInterface(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_compile")) return CmdAnimBlueprintCompile(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_get_compile_log")) return CmdAnimBlueprintGetCompileLog(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_export_folder")) return CmdAnimBlueprintExportFolder(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_apply_folder")) return CmdAnimBlueprintApplyFolder(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_get_info")) return CmdAnimBlueprintGetInfo(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_list_graphs")) return CmdAnimBlueprintListGraphs(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_list_state_machines")) return CmdAnimBlueprintListStateMachines(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_list_anim_layers")) return CmdAnimBlueprintListAnimLayers(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_list_layer_interfaces")) return CmdAnimBlueprintListLayerInterfaces(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_implement_layer_interface")) return CmdAnimBlueprintImplementLayerInterface(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_remove_layer_interface")) return CmdAnimBlueprintRemoveLayerInterface(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_add_anim_layer")) return CmdAnimBlueprintAddAnimLayer(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_remove_anim_layer")) return CmdAnimBlueprintRemoveAnimLayer(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_rename_anim_layer")) return CmdAnimBlueprintRenameAnimLayer(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_add_state_machine")) return CmdAnimBlueprintAddStateMachine(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_remove_state_machine")) return CmdAnimBlueprintRemoveStateMachine(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_rename_state_machine")) return CmdAnimBlueprintRenameStateMachine(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_set_entry_state")) return CmdAnimBlueprintSetEntryState(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_clear_entry_state")) return CmdAnimBlueprintClearEntryState(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_add_state")) return CmdAnimBlueprintAddState(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_set_state_properties")) return CmdAnimBlueprintSetStateProperties(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_add_conduit")) return CmdAnimBlueprintAddConduit(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_add_state_alias")) return CmdAnimBlueprintAddStateAlias(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_remove_state_node")) return CmdAnimBlueprintRemoveStateNode(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_rename_state_node")) return CmdAnimBlueprintRenameStateNode(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_set_state_alias_targets")) return CmdAnimBlueprintSetStateAliasTargets(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_add_transition")) return CmdAnimBlueprintAddTransition(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_set_transition_properties")) return CmdAnimBlueprintSetTransitionProperties(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_remove_transition")) return CmdAnimBlueprintRemoveTransition(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_open_editor")) return CmdOpenAssetEditor(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_inspect_nodes")) return CmdBlueprintInspectNodes(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_add_event_node")) return CmdBlueprintAddEventNode(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_add_custom_event_node")) return CmdBlueprintAddCustomEventNode(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_add_node_by_class")) return CmdBlueprintAddNodeByClass(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_add_call_function_node")) return CmdBlueprintAddCallFunctionNode(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_add_variable_node")) return CmdBlueprintAddVariableNode(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_connect_pins")) return CmdBlueprintConnectPins(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_disconnect_pins")) return CmdBlueprintDisconnectPins(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_set_pin_default_value")) return CmdBlueprintSetPinDefaultValue(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_remove_node")) return CmdBlueprintRemoveNode(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_add_variable")) return CmdBlueprintAddVariable(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_remove_variable")) return CmdBlueprintRemoveVariable(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_add_function_graph")) return CmdBlueprintAddFunctionGraph(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_add_macro_graph")) return CmdBlueprintAddMacroGraph(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_add_event_dispatcher")) return CmdBlueprintAddEventDispatcher(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_graph_get_view")) return CmdBlueprintGraphGetView(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_graph_set_view")) return CmdBlueprintGraphSetView(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_viewport_get_camera")) return CmdBlueprintViewportGetCamera(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_viewport_set_camera")) return CmdBlueprintViewportSetCamera(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_screenshot")) return CmdBlueprintScreenshot(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_set_cdo_property")) return CmdBlueprintSetCdoProperty(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_set_preview_mesh")) return CmdAnimBlueprintSetPreviewMesh(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_set_preview_animation_blueprint")) return CmdAnimBlueprintSetPreviewAnimationBlueprint(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_blueprint_reparent")) return CmdAnimBlueprintReparent(Ctx, OutData, OutError);

	OutError = TEXT("unknown_anim_blueprint_command");
	return false;
}

bool FUeAgentHttpServer::CmdAnimBlueprintCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	const FString PackageName = UeAgentAnimBlueprintOps::NormalizeAssetPath(AssetPathInput);
	if (!FPackageName::IsValidLongPackageName(PackageName))
	{
		OutError = TEXT("invalid_asset_path");
		return false;
	}
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	if (AssetName.IsEmpty())
	{
		OutError = TEXT("invalid_asset_name");
		return false;
	}

	const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
	if (UeAgentAnimBlueprintOps::AssetExists(ObjectPath))
	{
		OutError = TEXT("asset_already_exists");
		return false;
	}

	bool bTemplate = false;
	JsonTryGetBool(Ctx.Params, TEXT("template"), bTemplate);

	FString ParentClassPath = TEXT("/Script/Engine.AnimInstance");
	JsonTryGetString(Ctx.Params, TEXT("parent_class"), ParentClassPath);
	UClass* ParentClass = UeAgentAnimBlueprintOps::ResolveClassByPath(ParentClassPath);

	FString TargetSkeletonPath;
	JsonTryGetString(Ctx.Params, TEXT("target_skeleton"), TargetSkeletonPath);
	TargetSkeletonPath.TrimStartAndEndInline();

	FString PreviewSkeletalMeshPath;
	JsonTryGetString(Ctx.Params, TEXT("preview_skeletal_mesh"), PreviewSkeletalMeshPath);
	PreviewSkeletalMeshPath.TrimStartAndEndInline();

	if (bTemplate && (!TargetSkeletonPath.IsEmpty() || !PreviewSkeletalMeshPath.IsEmpty()))
	{
		OutError = TEXT("template_anim_blueprint_disallows_target_skeleton_or_preview_mesh");
		return false;
	}

	USkeletalMesh* PreviewSkeletalMesh = nullptr;
	if (!PreviewSkeletalMeshPath.IsEmpty())
	{
		PreviewSkeletalMesh = UeAgentAnimBlueprintOps::LoadSkeletalMesh(PreviewSkeletalMeshPath);
		if (!PreviewSkeletalMesh)
		{
			OutError = TEXT("preview_skeletal_mesh_not_found");
			return false;
		}
	}

	USkeleton* TargetSkeleton = nullptr;
	if (!bTemplate)
	{
		if (!TargetSkeletonPath.IsEmpty())
		{
			TargetSkeleton = UeAgentAnimBlueprintOps::LoadSkeleton(TargetSkeletonPath);
			if (!TargetSkeleton)
			{
				OutError = TEXT("target_skeleton_not_found");
				return false;
			}
		}
		else if (PreviewSkeletalMesh)
		{
			TargetSkeleton = PreviewSkeletalMesh->GetSkeleton();
		}

		if (!TargetSkeleton)
		{
			OutError = TEXT("missing_target_skeleton");
			return false;
		}
	}

	if (!UeAgentAnimBlueprintOps::ResolveAnimBlueprintParentClass(ParentClass, TargetSkeleton, bTemplate, OutError))
	{
		return false;
	}

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		OutError = TEXT("create_package_failed");
		return false;
	}

	UAnimBlueprintFactory* Factory = NewObject<UAnimBlueprintFactory>();
	if (!Factory)
	{
		OutError = TEXT("create_anim_blueprint_factory_failed");
		return false;
	}
	Factory->BlueprintType = BPTYPE_Normal;
	Factory->ParentClass = ParentClass;
	Factory->TargetSkeleton = TargetSkeleton;
	Factory->PreviewSkeletalMesh = PreviewSkeletalMesh;
	Factory->bTemplate = bTemplate;

	UAnimBlueprint* AnimBlueprint = Cast<UAnimBlueprint>(Factory->FactoryCreateNew(UAnimBlueprint::StaticClass(), Package, FName(*AssetName), RF_Public | RF_Standalone, nullptr, GWarn, FName(TEXT("UeAgentInterface"))));
	if (!AnimBlueprint)
	{
		OutError = TEXT("create_anim_blueprint_failed");
		return false;
	}

	FAssetRegistryModule::AssetCreated(AnimBlueprint);
	AnimBlueprint->MarkPackageDirty();
	Package->MarkPackageDirty();

	bool bCompileAfterCreate = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_create"), bCompileAfterCreate);
	if (bCompileAfterCreate)
	{
		FKismetEditorUtilities::CompileBlueprint(AnimBlueprint);
	}

	bool bOpenEditor = false;
	JsonTryGetBool(Ctx.Params, TEXT("open_editor"), bOpenEditor);
	if (bOpenEditor)
	{
		if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr)
		{
			AssetEditorSubsystem->OpenEditorForAsset(AnimBlueprint);
		}
	}

	bool bSaveAfterCreate = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_create"), bSaveAfterCreate);
	if (bSaveAfterCreate && !UeAgentAnimBlueprintOps::SaveAnimBlueprintPackage(AnimBlueprint, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), PackageName);
	OutData->SetStringField(TEXT("object_path"), AnimBlueprint->GetPathName());
	OutData->SetStringField(TEXT("generated_class"), AnimBlueprint->GeneratedClass ? AnimBlueprint->GeneratedClass->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("skeleton_generated_class"), AnimBlueprint->SkeletonGeneratedClass ? AnimBlueprint->SkeletonGeneratedClass->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("parent_class"), AnimBlueprint->ParentClass ? AnimBlueprint->ParentClass->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("target_skeleton"), AnimBlueprint->TargetSkeleton ? AnimBlueprint->TargetSkeleton->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("preview_skeletal_mesh"), AnimBlueprint->GetPreviewMesh() ? AnimBlueprint->GetPreviewMesh()->GetPathName() : TEXT(""));
	OutData->SetBoolField(TEXT("is_template"), AnimBlueprint->bIsTemplate);
	OutData->SetStringField(TEXT("status"), UeAgentAnimBlueprintOps::BlueprintStatusToString(AnimBlueprint->Status));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterCreate);
	return true;
}

bool FUeAgentHttpServer::CmdAnimBlueprintCompile(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UAnimBlueprint* AnimBlueprint = UeAgentAnimBlueprintOps::LoadAnimBlueprint(AssetPathInput);
	if (!AnimBlueprint)
	{
		OutError = TEXT("anim_blueprint_not_found");
		return false;
	}

	FCompilerResultsLog CompilerLog;
	FKismetEditorUtilities::CompileBlueprint(AnimBlueprint, EBlueprintCompileOptions::None, &CompilerLog);

	FString SeverityFilterText = TEXT("all");
	JsonTryGetString(Ctx.Params, TEXT("severity_filter"), SeverityFilterText);
	UeAgentAnimBlueprintOps::ECompilerMessageSeverityFilter SeverityFilter = UeAgentAnimBlueprintOps::ECompilerMessageSeverityFilter::All;
	if (!UeAgentAnimBlueprintOps::ParseCompilerSeverityFilter(SeverityFilterText, SeverityFilter))
	{
		OutError = TEXT("invalid_severity_filter");
		return false;
	}

	int32 MaxMessages = 200;
	double MaxMessagesNumber = static_cast<double>(MaxMessages);
	if (JsonTryGetNumber(Ctx.Params, TEXT("max_messages"), MaxMessagesNumber))
	{
		MaxMessages = FMath::Clamp(FMath::RoundToInt(MaxMessagesNumber), 1, 2000);
	}

	bool bIncludeMessages = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_messages"), bIncludeMessages);

	bool bSaveAfterCompile = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_compile"), bSaveAfterCompile);
	if (bSaveAfterCompile && !UeAgentAnimBlueprintOps::SaveAnimBlueprintPackage(AnimBlueprint, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), AnimBlueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("status"), UeAgentAnimBlueprintOps::BlueprintStatusToString(AnimBlueprint->Status));
	OutData->SetStringField(TEXT("generated_class"), AnimBlueprint->GeneratedClass ? AnimBlueprint->GeneratedClass->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("target_skeleton"), AnimBlueprint->TargetSkeleton ? AnimBlueprint->TargetSkeleton->GetPathName() : TEXT(""));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterCompile);
	OutData->SetNumberField(TEXT("error_count"), CompilerLog.NumErrors);
	OutData->SetNumberField(TEXT("warning_count"), CompilerLog.NumWarnings);
	OutData->SetNumberField(TEXT("message_total_count"), CompilerLog.Messages.Num());
	OutData->SetBoolField(TEXT("has_error"), CompilerLog.NumErrors > 0 || AnimBlueprint->Status == BS_Error);

	if (bIncludeMessages)
	{
		TArray<TSharedPtr<FJsonValue>> Messages;
		int32 FilteredErrorCount = 0;
		int32 FilteredWarningCount = 0;
		int32 FilteredInfoCount = 0;
		UeAgentAnimBlueprintOps::BuildCompilerLogJson(CompilerLog, SeverityFilter, MaxMessages, Messages, FilteredErrorCount, FilteredWarningCount, FilteredInfoCount);

		OutData->SetStringField(TEXT("severity_filter"), SeverityFilterText);
		OutData->SetNumberField(TEXT("max_messages"), MaxMessages);
		OutData->SetNumberField(TEXT("filtered_error_count"), FilteredErrorCount);
		OutData->SetNumberField(TEXT("filtered_warning_count"), FilteredWarningCount);
		OutData->SetNumberField(TEXT("filtered_info_count"), FilteredInfoCount);
		OutData->SetNumberField(TEXT("filtered_message_count"), FilteredErrorCount + FilteredWarningCount + FilteredInfoCount);
		OutData->SetArrayField(TEXT("messages"), Messages);
	}

	return true;
}

bool FUeAgentHttpServer::CmdAnimBlueprintGetCompileLog(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UAnimBlueprint* AnimBlueprint = UeAgentAnimBlueprintOps::LoadAnimBlueprint(AssetPathInput);
	if (!AnimBlueprint)
	{
		OutError = TEXT("anim_blueprint_not_found");
		return false;
	}

	FString SeverityFilterText = TEXT("all");
	JsonTryGetString(Ctx.Params, TEXT("severity_filter"), SeverityFilterText);
	UeAgentAnimBlueprintOps::ECompilerMessageSeverityFilter SeverityFilter = UeAgentAnimBlueprintOps::ECompilerMessageSeverityFilter::All;
	if (!UeAgentAnimBlueprintOps::ParseCompilerSeverityFilter(SeverityFilterText, SeverityFilter))
	{
		OutError = TEXT("invalid_severity_filter");
		return false;
	}

	int32 MaxMessages = 200;
	double MaxMessagesNumber = static_cast<double>(MaxMessages);
	if (JsonTryGetNumber(Ctx.Params, TEXT("max_messages"), MaxMessagesNumber))
	{
		MaxMessages = FMath::Clamp(FMath::RoundToInt(MaxMessagesNumber), 1, 2000);
	}

	bool bSaveAfterCompile = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_compile"), bSaveAfterCompile);

	FCompilerResultsLog CompilerLog;
	FKismetEditorUtilities::CompileBlueprint(AnimBlueprint, EBlueprintCompileOptions::None, &CompilerLog);

	if (bSaveAfterCompile && !UeAgentAnimBlueprintOps::SaveAnimBlueprintPackage(AnimBlueprint, OutError))
	{
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Messages;
	int32 FilteredErrorCount = 0;
	int32 FilteredWarningCount = 0;
	int32 FilteredInfoCount = 0;
	UeAgentAnimBlueprintOps::BuildCompilerLogJson(CompilerLog, SeverityFilter, MaxMessages, Messages, FilteredErrorCount, FilteredWarningCount, FilteredInfoCount);

	OutData->SetStringField(TEXT("asset_path"), AnimBlueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("status"), UeAgentAnimBlueprintOps::BlueprintStatusToString(AnimBlueprint->Status));
	OutData->SetStringField(TEXT("generated_class"), AnimBlueprint->GeneratedClass ? AnimBlueprint->GeneratedClass->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("target_skeleton"), AnimBlueprint->TargetSkeleton ? AnimBlueprint->TargetSkeleton->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("severity_filter"), SeverityFilterText);
	OutData->SetNumberField(TEXT("max_messages"), MaxMessages);
	OutData->SetNumberField(TEXT("message_total_count"), CompilerLog.Messages.Num());
	OutData->SetNumberField(TEXT("filtered_error_count"), FilteredErrorCount);
	OutData->SetNumberField(TEXT("filtered_warning_count"), FilteredWarningCount);
	OutData->SetNumberField(TEXT("filtered_info_count"), FilteredInfoCount);
	OutData->SetNumberField(TEXT("filtered_message_count"), FilteredErrorCount + FilteredWarningCount + FilteredInfoCount);
	OutData->SetNumberField(TEXT("error_count"), CompilerLog.NumErrors);
	OutData->SetNumberField(TEXT("warning_count"), CompilerLog.NumWarnings);
	OutData->SetBoolField(TEXT("has_error"), CompilerLog.NumErrors > 0 || AnimBlueprint->Status == BS_Error);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterCompile);
	OutData->SetArrayField(TEXT("messages"), Messages);
	return true;
}

bool FUeAgentHttpServer::CmdAnimBlueprintGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UAnimBlueprint* AnimBlueprint = UeAgentAnimBlueprintOps::LoadAnimBlueprint(AssetPathInput);
	if (!AnimBlueprint)
	{
		OutError = TEXT("anim_blueprint_not_found");
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Graphs;
	int32 AnimGraphCount = 0;
	int32 LogicGraphCount = 0;
	UeAgentAnimBlueprintOps::AppendAnimBlueprintGraphItems(AnimBlueprint, Graphs, AnimGraphCount, LogicGraphCount);

	OutData->SetStringField(TEXT("asset_path"), AnimBlueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), AnimBlueprint->GetPathName());
	OutData->SetStringField(TEXT("generated_class"), AnimBlueprint->GeneratedClass ? AnimBlueprint->GeneratedClass->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("skeleton_generated_class"), AnimBlueprint->SkeletonGeneratedClass ? AnimBlueprint->SkeletonGeneratedClass->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("status"), UeAgentAnimBlueprintOps::BlueprintStatusToString(AnimBlueprint->Status));
	OutData->SetStringField(TEXT("parent_class"), AnimBlueprint->ParentClass ? AnimBlueprint->ParentClass->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("target_skeleton"), AnimBlueprint->TargetSkeleton ? AnimBlueprint->TargetSkeleton->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("preview_skeletal_mesh"), AnimBlueprint->GetPreviewMesh() ? AnimBlueprint->GetPreviewMesh()->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("preview_animation_blueprint"), AnimBlueprint->GetPreviewAnimationBlueprint() ? AnimBlueprint->GetPreviewAnimationBlueprint()->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("preview_application_method"), UeAgentAnimBlueprintOps::PreviewApplicationMethodToString(AnimBlueprint->GetPreviewAnimationBlueprintApplicationMethod()));
	OutData->SetStringField(TEXT("preview_animation_blueprint_tag"), AnimBlueprint->GetPreviewAnimationBlueprintTag().ToString());
	OutData->SetBoolField(TEXT("is_template"), AnimBlueprint->bIsTemplate);
	OutData->SetBoolField(TEXT("use_multi_threaded_animation_update"), AnimBlueprint->bUseMultiThreadedAnimationUpdate);
	OutData->SetBoolField(TEXT("warn_about_blueprint_usage"), AnimBlueprint->bWarnAboutBlueprintUsage);
	OutData->SetBoolField(TEXT("supports_event_graphs"), AnimBlueprint->SupportsEventGraphs());
	OutData->SetBoolField(TEXT("supports_anim_layers"), AnimBlueprint->SupportsAnimLayers());
	OutData->SetBoolField(TEXT("supports_delegates"), AnimBlueprint->SupportsDelegates());
	OutData->SetBoolField(TEXT("supports_macros"), AnimBlueprint->SupportsMacros());
	OutData->SetBoolField(TEXT("supports_input_events"), AnimBlueprint->SupportsInputEvents());
	OutData->SetNumberField(TEXT("graph_count"), Graphs.Num());
	OutData->SetNumberField(TEXT("anim_graph_count"), AnimGraphCount);
	OutData->SetNumberField(TEXT("logic_graph_count"), LogicGraphCount);
	OutData->SetNumberField(TEXT("ubergraph_count"), AnimBlueprint->UbergraphPages.Num());
	OutData->SetNumberField(TEXT("function_graph_count"), AnimBlueprint->FunctionGraphs.Num());
	OutData->SetNumberField(TEXT("macro_graph_count"), AnimBlueprint->MacroGraphs.Num());
	OutData->SetNumberField(TEXT("delegate_graph_count"), AnimBlueprint->DelegateSignatureGraphs.Num());
	OutData->SetArrayField(TEXT("graphs"), Graphs);
	return true;
}

bool FUeAgentHttpServer::CmdAnimBlueprintListGraphs(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UAnimBlueprint* AnimBlueprint = UeAgentAnimBlueprintOps::LoadAnimBlueprint(AssetPathInput);
	if (!AnimBlueprint)
	{
		OutError = TEXT("anim_blueprint_not_found");
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Graphs;
	int32 AnimGraphCount = 0;
	int32 LogicGraphCount = 0;
	UeAgentAnimBlueprintOps::AppendAnimBlueprintGraphItems(AnimBlueprint, Graphs, AnimGraphCount, LogicGraphCount);

	OutData->SetStringField(TEXT("asset_path"), AnimBlueprint->GetOutermost()->GetName());
	OutData->SetArrayField(TEXT("graphs"), Graphs);
	OutData->SetNumberField(TEXT("graph_count"), Graphs.Num());
	OutData->SetNumberField(TEXT("anim_graph_count"), AnimGraphCount);
	OutData->SetNumberField(TEXT("logic_graph_count"), LogicGraphCount);
	return true;
}

bool FUeAgentHttpServer::CmdAnimBlueprintListStateMachines(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UAnimBlueprint* AnimBlueprint = UeAgentAnimBlueprintOps::LoadAnimBlueprint(AssetPathInput);
	if (!AnimBlueprint)
	{
		OutError = TEXT("anim_blueprint_not_found");
		return false;
	}

	const TArray<UEdGraph*> AnimGraphs = UeAgentAnimBlueprintOps::GatherAnimGraphs(AnimBlueprint);
	const IAnimClassInterface* AnimClassInterface = UeAgentAnimBlueprintOps::ResolveAnimClassInterface(AnimBlueprint);

	TMap<FName, const FBakedAnimationStateMachine*> CompiledMachinesByName;
	int32 CompiledStateMachineCount = 0;
	int32 StateMachineNodePropertyCount = 0;
	if (AnimClassInterface)
	{
		const TArray<FBakedAnimationStateMachine>& CompiledMachines = AnimClassInterface->GetBakedStateMachines();
		CompiledStateMachineCount = CompiledMachines.Num();
		StateMachineNodePropertyCount = AnimClassInterface->GetStateMachineNodeProperties().Num();
		for (const FBakedAnimationStateMachine& CompiledMachine : CompiledMachines)
		{
			CompiledMachinesByName.Add(CompiledMachine.MachineName, &CompiledMachine);
		}
	}

	TArray<TSharedPtr<FJsonValue>> StateMachines;
	int32 TotalStateCount = 0;
	int32 TotalConduitCount = 0;
	int32 TotalAliasCount = 0;
	int32 TotalTransitionCount = 0;

	for (UEdGraph* AnimGraph : AnimGraphs)
	{
		if (!AnimGraph)
		{
			continue;
		}

		TArray<UAnimGraphNode_StateMachineBase*> StateMachineNodes;
		AnimGraph->GetNodesOfClass<UAnimGraphNode_StateMachineBase>(StateMachineNodes);

		for (UAnimGraphNode_StateMachineBase* StateMachineNode : StateMachineNodes)
		{
			if (!StateMachineNode)
			{
				continue;
			}

			UAnimationStateMachineGraph* StateMachineGraph = StateMachineNode->EditorStateMachineGraph;
			TArray<UAnimStateNodeBase*> StateNodeBases;
			TArray<UAnimStateNode*> StateNodes;
			TArray<UAnimStateConduitNode*> ConduitNodes;
			TArray<UAnimStateAliasNode*> AliasNodes;
			TArray<UAnimStateTransitionNode*> TransitionNodes;

			if (StateMachineGraph)
			{
				StateMachineGraph->GetNodesOfClass<UAnimStateNodeBase>(StateNodeBases);
				StateMachineGraph->GetNodesOfClass<UAnimStateNode>(StateNodes);
				StateMachineGraph->GetNodesOfClass<UAnimStateConduitNode>(ConduitNodes);
				StateMachineGraph->GetNodesOfClass<UAnimStateAliasNode>(AliasNodes);
				StateMachineGraph->GetNodesOfClass<UAnimStateTransitionNode>(TransitionNodes);
			}

			TArray<FString> StateNames;
			StateNames.Reserve(StateNodes.Num());
			for (const UAnimStateNode* StateNode : StateNodes)
			{
				if (StateNode)
				{
					StateNames.Add(StateNode->GetStateName());
				}
			}

			TArray<FString> ConduitNames;
			ConduitNames.Reserve(ConduitNodes.Num());
			for (const UAnimStateConduitNode* ConduitNode : ConduitNodes)
			{
				if (ConduitNode)
				{
					ConduitNames.Add(ConduitNode->GetStateName());
				}
			}

			TArray<FString> AliasNames;
			AliasNames.Reserve(AliasNodes.Num());
			for (const UAnimStateAliasNode* AliasNode : AliasNodes)
			{
				if (AliasNode)
				{
					AliasNames.Add(AliasNode->GetStateName());
				}
			}

			TotalStateCount += StateNodes.Num();
			TotalConduitCount += ConduitNodes.Num();
			TotalAliasCount += AliasNodes.Num();
			TotalTransitionCount += TransitionNodes.Num();

			const FString StateMachineName = StateMachineGraph ? StateMachineGraph->GetName() : StateMachineNode->GetStateMachineName();
			const FBakedAnimationStateMachine* const* CompiledMachinePtr = CompiledMachinesByName.Find(FName(*StateMachineName));
			const FBakedAnimationStateMachine* CompiledMachine = CompiledMachinePtr ? *CompiledMachinePtr : nullptr;

			TArray<TSharedPtr<FJsonValue>> StateNodeItems;
			for (const UAnimStateNodeBase* StateNodeBase : StateNodeBases)
			{
				StateNodeItems.Add(MakeShared<FJsonValueObject>(UeAgentAnimBlueprintOps::BuildStateNodeJson(StateNodeBase)));
			}

			TArray<TSharedPtr<FJsonValue>> TransitionItems;
			for (const UAnimStateTransitionNode* TransitionNode : TransitionNodes)
			{
				TransitionItems.Add(MakeShared<FJsonValueObject>(UeAgentAnimBlueprintOps::BuildTransitionJson(TransitionNode)));
			}

			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("state_machine_name"), StateMachineName);
			Item->SetStringField(TEXT("graph_name"), StateMachineGraph ? StateMachineGraph->GetName() : TEXT(""));
			Item->SetStringField(TEXT("graph_path"), StateMachineGraph ? StateMachineGraph->GetPathName() : TEXT(""));
			Item->SetStringField(TEXT("owner_anim_graph_name"), AnimGraph->GetName());
			Item->SetStringField(TEXT("owner_anim_graph_path"), AnimGraph->GetPathName());
			Item->SetStringField(TEXT("owner_node_path"), StateMachineNode->GetPathName());
			Item->SetBoolField(TEXT("has_graph"), StateMachineGraph != nullptr);
			Item->SetBoolField(TEXT("has_entry_node"), StateMachineGraph && StateMachineGraph->EntryNode != nullptr);
			Item->SetStringField(TEXT("entry_node_guid"), UeAgentAnimBlueprintOps::NodeGuidToString(StateMachineGraph ? StateMachineGraph->EntryNode : nullptr));
			Item->SetStringField(
				TEXT("entry_connected_state_name"),
				(UeAgentAnimBlueprintOps::GetEntryConnectedState(StateMachineGraph) != nullptr)
					? UeAgentAnimBlueprintOps::GetEntryConnectedState(StateMachineGraph)->GetStateName()
					: TEXT(""));
			Item->SetStringField(
				TEXT("entry_connected_state_guid"),
				UeAgentAnimBlueprintOps::NodeGuidToString(UeAgentAnimBlueprintOps::GetEntryConnectedState(StateMachineGraph)));
			Item->SetNumberField(TEXT("state_count"), StateNodes.Num());
			Item->SetNumberField(TEXT("conduit_count"), ConduitNodes.Num());
			Item->SetNumberField(TEXT("alias_count"), AliasNodes.Num());
			Item->SetNumberField(TEXT("transition_count"), TransitionNodes.Num());
			Item->SetArrayField(TEXT("state_names"), UeAgentAnimBlueprintOps::MakeStringArray(StateNames));
			Item->SetArrayField(TEXT("conduit_names"), UeAgentAnimBlueprintOps::MakeStringArray(ConduitNames));
			Item->SetArrayField(TEXT("alias_names"), UeAgentAnimBlueprintOps::MakeStringArray(AliasNames));
			Item->SetArrayField(TEXT("state_nodes"), StateNodeItems);
			Item->SetArrayField(TEXT("transition_nodes"), TransitionItems);
			Item->SetBoolField(TEXT("compiled_machine_found"), CompiledMachine != nullptr);
			Item->SetNumberField(TEXT("compiled_state_count"), CompiledMachine ? CompiledMachine->States.Num() : 0);
			Item->SetNumberField(TEXT("compiled_transition_count"), CompiledMachine ? CompiledMachine->Transitions.Num() : 0);
			Item->SetStringField(
				TEXT("compiled_initial_state"),
				(CompiledMachine && CompiledMachine->InitialState != INDEX_NONE && CompiledMachine->States.IsValidIndex(CompiledMachine->InitialState))
					? CompiledMachine->States[CompiledMachine->InitialState].StateName.ToString()
					: TEXT(""));
			StateMachines.Add(MakeShared<FJsonValueObject>(Item));
		}
	}

	OutData->SetStringField(TEXT("asset_path"), AnimBlueprint->GetOutermost()->GetName());
	OutData->SetNumberField(TEXT("anim_graph_count"), AnimGraphs.Num());
	OutData->SetNumberField(TEXT("state_machine_count"), StateMachines.Num());
	OutData->SetNumberField(TEXT("compiled_state_machine_count"), CompiledStateMachineCount);
	OutData->SetNumberField(TEXT("state_machine_node_property_count"), StateMachineNodePropertyCount);
	OutData->SetNumberField(TEXT("state_total_count"), TotalStateCount);
	OutData->SetNumberField(TEXT("conduit_total_count"), TotalConduitCount);
	OutData->SetNumberField(TEXT("alias_total_count"), TotalAliasCount);
	OutData->SetNumberField(TEXT("transition_total_count"), TotalTransitionCount);
	OutData->SetArrayField(TEXT("state_machines"), StateMachines);
	return true;
}

bool FUeAgentHttpServer::CmdAnimBlueprintListAnimLayers(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UAnimBlueprint* AnimBlueprint = UeAgentAnimBlueprintOps::LoadAnimBlueprint(AssetPathInput);
	if (!AnimBlueprint)
	{
		OutError = TEXT("anim_blueprint_not_found");
		return false;
	}

	const IAnimClassInterface* AnimClassInterface = UeAgentAnimBlueprintOps::ResolveAnimClassInterface(AnimBlueprint);

	TMap<const UEdGraph*, FString> ImplementedInterfacePathByGraph;
	TMap<FName, FString> ImplementedInterfacePathByLayerName;
	int32 ImplementedAnimLayerInterfaceCount = 0;
	for (const FBPInterfaceDescription& InterfaceDesc : AnimBlueprint->ImplementedInterfaces)
	{
		const UClass* InterfaceClass = InterfaceDesc.Interface.Get();
		if (!InterfaceClass || !InterfaceClass->IsChildOf(UAnimLayerInterface::StaticClass()))
		{
			continue;
		}

		++ImplementedAnimLayerInterfaceCount;
		const FString InterfacePath = InterfaceClass->GetPathName();
		for (UEdGraph* InterfaceGraph : InterfaceDesc.Graphs)
		{
			if (!InterfaceGraph)
			{
				continue;
			}

			ImplementedInterfacePathByGraph.Add(InterfaceGraph, InterfacePath);
			ImplementedInterfacePathByLayerName.Add(InterfaceGraph->GetFName(), InterfacePath);
		}
	}

	TMap<FName, const FAnimBlueprintFunction*> CompiledFunctionsByName;
	const TMap<FName, FGraphAssetPlayerInformation>* GraphAssetPlayerInfoMap = nullptr;
	const TMap<FName, FAnimGraphBlendOptions>* GraphBlendOptionsMap = nullptr;
	int32 CompiledLayerFunctionCount = 0;
	int32 ImplementedLayerCount = 0;
	int32 LinkedAnimLayerNodePropertyCount = 0;
	if (AnimClassInterface)
	{
		GraphAssetPlayerInfoMap = &AnimClassInterface->GetGraphAssetPlayerInformation();
		GraphBlendOptionsMap = &AnimClassInterface->GetGraphBlendOptions();
		LinkedAnimLayerNodePropertyCount = AnimClassInterface->GetLinkedAnimLayerNodeProperties().Num();

		for (const FAnimBlueprintFunction& CompiledFunction : AnimClassInterface->GetAnimBlueprintFunctions())
		{
			if (CompiledFunction.Name == UEdGraphSchema_K2::GN_AnimGraph)
			{
				continue;
			}

			CompiledFunctionsByName.Add(CompiledFunction.Name, &CompiledFunction);
			++CompiledLayerFunctionCount;
			if (CompiledFunction.bImplemented)
			{
				++ImplementedLayerCount;
			}
		}
	}

	TSet<FName> SeenLayerNames;
	TArray<TSharedPtr<FJsonValue>> AnimLayers;
	int32 LayerGraphCount = 0;

	auto AppendLayerItem =
		[&AnimLayers, &ImplementedInterfacePathByGraph, &ImplementedInterfacePathByLayerName, GraphAssetPlayerInfoMap, GraphBlendOptionsMap](
			const FName LayerName,
			const UEdGraph* LayerGraph,
			const FAnimBlueprintFunction* CompiledFunction)
		{
			const FGraphAssetPlayerInformation* AssetPlayerInfo = GraphAssetPlayerInfoMap ? GraphAssetPlayerInfoMap->Find(LayerName) : nullptr;
			const FAnimGraphBlendOptions* BlendOptions = GraphBlendOptionsMap ? GraphBlendOptionsMap->Find(LayerName) : nullptr;

			TArray<FString> InputPoseNames;
			if (CompiledFunction)
			{
				InputPoseNames.Reserve(CompiledFunction->InputPoseNames.Num());
				for (const FName InputPoseName : CompiledFunction->InputPoseNames)
				{
					InputPoseNames.Add(InputPoseName.ToString());
				}
			}

			TArray<UAnimGraphNode_LinkedAnimLayer*> LinkedLayerNodes;
			if (LayerGraph)
			{
				LayerGraph->GetNodesOfClass<UAnimGraphNode_LinkedAnimLayer>(LinkedLayerNodes);
			}

			const FString InterfacePath =
				LayerGraph
					? ImplementedInterfacePathByGraph.FindRef(LayerGraph)
					: ImplementedInterfacePathByLayerName.FindRef(LayerName);

			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("layer_name"), LayerName.ToString());
			Item->SetStringField(TEXT("graph_name"), LayerGraph ? LayerGraph->GetName() : TEXT(""));
			Item->SetStringField(TEXT("graph_path"), LayerGraph ? LayerGraph->GetPathName() : TEXT(""));
			Item->SetStringField(TEXT("graph_guid"), LayerGraph ? UeAgentAnimBlueprintOps::GuidToString(LayerGraph->GraphGuid) : TEXT(""));
			Item->SetStringField(TEXT("interface_guid"), LayerGraph ? UeAgentAnimBlueprintOps::GuidToString(LayerGraph->InterfaceGuid) : TEXT(""));
			Item->SetStringField(TEXT("implemented_interface"), InterfacePath);
			Item->SetBoolField(TEXT("is_interface_layer"), LayerGraph ? LayerGraph->InterfaceGuid.IsValid() : !InterfacePath.IsEmpty());
			Item->SetBoolField(TEXT("has_graph"), LayerGraph != nullptr);
			Item->SetBoolField(TEXT("compiled_function_found"), CompiledFunction != nullptr);
			Item->SetBoolField(TEXT("is_implemented"), CompiledFunction ? CompiledFunction->bImplemented : false);
			Item->SetStringField(TEXT("group"), CompiledFunction ? CompiledFunction->Group.ToString() : TEXT(""));
			Item->SetNumberField(TEXT("input_pose_count"), CompiledFunction ? CompiledFunction->InputPoseNames.Num() : 0);
			Item->SetArrayField(TEXT("input_pose_names"), UeAgentAnimBlueprintOps::MakeStringArray(InputPoseNames));
			Item->SetNumberField(TEXT("output_pose_node_index"), CompiledFunction ? CompiledFunction->OutputPoseNodeIndex : INDEX_NONE);
			Item->SetNumberField(TEXT("asset_player_node_count"), AssetPlayerInfo ? AssetPlayerInfo->PlayerNodeIndices.Num() : 0);
			Item->SetNumberField(TEXT("linked_anim_layer_node_count"), LinkedLayerNodes.Num());
			Item->SetBoolField(TEXT("has_blend_options"), BlendOptions != nullptr);
			Item->SetNumberField(TEXT("blend_in_time"), BlendOptions ? BlendOptions->BlendInTime : -1.0f);
			Item->SetNumberField(TEXT("blend_out_time"), BlendOptions ? BlendOptions->BlendOutTime : -1.0f);
			Item->SetStringField(TEXT("blend_in_profile"), (BlendOptions && BlendOptions->BlendInProfile) ? BlendOptions->BlendInProfile->GetPathName() : TEXT(""));
			Item->SetStringField(TEXT("blend_out_profile"), (BlendOptions && BlendOptions->BlendOutProfile) ? BlendOptions->BlendOutProfile->GetPathName() : TEXT(""));
			AnimLayers.Add(MakeShared<FJsonValueObject>(Item));
		};

	for (UEdGraph* Graph : AnimBlueprint->FunctionGraphs)
	{
		if (!Graph || !AnimationEditorUtils::IsAnimGraph(Graph) || Graph->GetFName() == UEdGraphSchema_K2::GN_AnimGraph)
		{
			continue;
		}

		++LayerGraphCount;
		SeenLayerNames.Add(Graph->GetFName());
		const FAnimBlueprintFunction* const* CompiledFunctionPtr = CompiledFunctionsByName.Find(Graph->GetFName());
		AppendLayerItem(Graph->GetFName(), Graph, CompiledFunctionPtr ? *CompiledFunctionPtr : nullptr);
	}

	for (const TPair<FName, const FAnimBlueprintFunction*>& Pair : CompiledFunctionsByName)
	{
		if (SeenLayerNames.Contains(Pair.Key))
		{
			continue;
		}

		AppendLayerItem(Pair.Key, nullptr, Pair.Value);
	}

	OutData->SetStringField(TEXT("asset_path"), AnimBlueprint->GetOutermost()->GetName());
	OutData->SetBoolField(TEXT("supports_anim_layers"), AnimBlueprint->SupportsAnimLayers());
	OutData->SetNumberField(TEXT("layer_count"), AnimLayers.Num());
	OutData->SetNumberField(TEXT("layer_graph_count"), LayerGraphCount);
	OutData->SetNumberField(TEXT("compiled_layer_function_count"), CompiledLayerFunctionCount);
	OutData->SetNumberField(TEXT("implemented_layer_count"), ImplementedLayerCount);
	OutData->SetNumberField(TEXT("implemented_interface_count"), ImplementedAnimLayerInterfaceCount);
	OutData->SetNumberField(TEXT("linked_anim_layer_node_property_count"), LinkedAnimLayerNodePropertyCount);
	OutData->SetArrayField(TEXT("anim_layers"), AnimLayers);
	return true;
}

bool FUeAgentHttpServer::CmdAnimBlueprintCreateLayerInterface(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	const FString PackageName = UeAgentAnimBlueprintOps::NormalizeAssetPath(AssetPathInput);
	if (!FPackageName::IsValidLongPackageName(PackageName))
	{
		OutError = TEXT("invalid_asset_path");
		return false;
	}

	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	if (AssetName.IsEmpty())
	{
		OutError = TEXT("invalid_asset_name");
		return false;
	}

	const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
	if (UeAgentAnimBlueprintOps::AssetExists(ObjectPath))
	{
		OutError = TEXT("asset_already_exists");
		return false;
	}

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		OutError = TEXT("create_package_failed");
		return false;
	}

	UAnimLayerInterfaceFactory* Factory = NewObject<UAnimLayerInterfaceFactory>();
	if (!Factory)
	{
		OutError = TEXT("create_anim_layer_interface_factory_failed");
		return false;
	}

	UAnimBlueprint* LayerInterface = Cast<UAnimBlueprint>(Factory->FactoryCreateNew(UAnimBlueprint::StaticClass(), Package, FName(*AssetName), RF_Public | RF_Standalone, nullptr, GWarn, FName(TEXT("UeAgentInterface"))));
	if (!LayerInterface)
	{
		OutError = TEXT("create_anim_layer_interface_failed");
		return false;
	}

	FAssetRegistryModule::AssetCreated(LayerInterface);
	LayerInterface->MarkPackageDirty();
	Package->MarkPackageDirty();

	bool bCompileAfterCreate = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_create"), bCompileAfterCreate);
	if (bCompileAfterCreate)
	{
		FKismetEditorUtilities::CompileBlueprint(LayerInterface);
	}

	bool bOpenEditor = false;
	JsonTryGetBool(Ctx.Params, TEXT("open_editor"), bOpenEditor);
	if (bOpenEditor)
	{
		if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr)
		{
			AssetEditorSubsystem->OpenEditorForAsset(LayerInterface);
		}
	}

	bool bSaveAfterCreate = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_create"), bSaveAfterCreate);
	if (bSaveAfterCreate && !UeAgentAnimBlueprintOps::SaveAnimBlueprintPackage(LayerInterface, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), PackageName);
	OutData->SetStringField(TEXT("object_path"), LayerInterface->GetPathName());
	OutData->SetStringField(TEXT("generated_class"), LayerInterface->GeneratedClass ? LayerInterface->GeneratedClass->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("skeleton_generated_class"), LayerInterface->SkeletonGeneratedClass ? LayerInterface->SkeletonGeneratedClass->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("blueprint_type"), TEXT("interface"));
	OutData->SetBoolField(TEXT("is_layer_interface"), true);
	OutData->SetStringField(TEXT("status"), UeAgentAnimBlueprintOps::BlueprintStatusToString(LayerInterface->Status));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterCreate);
	return true;
}

bool FUeAgentHttpServer::CmdAnimBlueprintListLayerInterfaces(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UAnimBlueprint* AnimBlueprint = UeAgentAnimBlueprintOps::LoadAnimBlueprint(AssetPathInput);
	if (!AnimBlueprint)
	{
		OutError = TEXT("anim_blueprint_not_found");
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Interfaces;
	for (const FBPInterfaceDescription& InterfaceDesc : AnimBlueprint->ImplementedInterfaces)
	{
		const UClass* InterfaceClass = InterfaceDesc.Interface.Get();
		if (!InterfaceClass || !InterfaceClass->IsChildOf(UAnimLayerInterface::StaticClass()))
		{
			continue;
		}

		TArray<TSharedPtr<FJsonValue>> Graphs;
		for (const UEdGraph* Graph : InterfaceDesc.Graphs)
		{
			if (!Graph)
			{
				continue;
			}

			TSharedPtr<FJsonObject> GraphItem = MakeShared<FJsonObject>();
			GraphItem->SetStringField(TEXT("graph_name"), Graph->GetName());
			GraphItem->SetStringField(TEXT("graph_path"), Graph->GetPathName());
			Graphs.Add(MakeShared<FJsonValueObject>(GraphItem));
		}

		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("interface_class"), InterfaceClass->GetPathName());
		Item->SetStringField(TEXT("interface_name"), InterfaceClass->GetName());
		Item->SetNumberField(TEXT("graph_count"), Graphs.Num());
		Item->SetArrayField(TEXT("graphs"), Graphs);
		Interfaces.Add(MakeShared<FJsonValueObject>(Item));
	}

	OutData->SetStringField(TEXT("asset_path"), AnimBlueprint->GetOutermost()->GetName());
	OutData->SetBoolField(TEXT("is_layer_interface_blueprint"), UeAgentAnimBlueprintOps::IsLayerInterfaceBlueprint(AnimBlueprint));
	OutData->SetNumberField(TEXT("implemented_interface_count"), Interfaces.Num());
	OutData->SetArrayField(TEXT("interfaces"), Interfaces);
	return true;
}

bool FUeAgentHttpServer::CmdAnimBlueprintImplementLayerInterface(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString InterfaceClassPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("interface_class"), InterfaceClassPath) || InterfaceClassPath.IsEmpty())
	{
		OutError = TEXT("missing_interface_class");
		return false;
	}

	UAnimBlueprint* AnimBlueprint = UeAgentAnimBlueprintOps::LoadAnimBlueprint(AssetPathInput);
	if (!AnimBlueprint)
	{
		OutError = TEXT("anim_blueprint_not_found");
		return false;
	}
	if (UeAgentAnimBlueprintOps::IsLayerInterfaceBlueprint(AnimBlueprint))
	{
		OutError = TEXT("layer_interface_blueprint_cannot_implement_layer_interface");
		return false;
	}

	UClass* InterfaceClass = UeAgentAnimBlueprintOps::ResolveAnimLayerInterfaceClass(InterfaceClassPath);
	if (!InterfaceClass || !InterfaceClass->IsChildOf(UAnimLayerInterface::StaticClass()))
	{
		OutError = TEXT("invalid_layer_interface_class");
		return false;
	}

	const FTopLevelAssetPath InterfaceClassPathName = InterfaceClass->GetClassPathName();
	for (const FBPInterfaceDescription& InterfaceDesc : AnimBlueprint->ImplementedInterfaces)
	{
		const UClass* ExistingInterface = InterfaceDesc.Interface.Get();
		if (ExistingInterface && ExistingInterface->GetClassPathName() == InterfaceClassPathName)
		{
			OutData->SetStringField(TEXT("asset_path"), AnimBlueprint->GetOutermost()->GetName());
			OutData->SetStringField(TEXT("interface_class"), ExistingInterface->GetPathName());
			OutData->SetBoolField(TEXT("changed"), false);
			return true;
		}
	}

	if (!FBlueprintEditorUtils::ImplementNewInterface(AnimBlueprint, InterfaceClassPathName))
	{
		OutError = TEXT("implement_layer_interface_failed");
		return false;
	}

	bool bCompileAfterAdd = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_add"), bCompileAfterAdd);
	bool bSaveAfterAdd = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_add"), bSaveAfterAdd);
	if (!UeAgentAnimBlueprintOps::FinalizeAnimBlueprintStructureMutation(AnimBlueprint, bCompileAfterAdd, bSaveAfterAdd, OutError))
	{
		return false;
	}

	TArray<UEdGraph*> InterfaceGraphs;
	FBlueprintEditorUtils::GetInterfaceGraphs(AnimBlueprint, InterfaceClassPathName, InterfaceGraphs);

	TArray<TSharedPtr<FJsonValue>> Graphs;
	for (UEdGraph* Graph : InterfaceGraphs)
	{
		if (!Graph)
		{
			continue;
		}

		TSharedPtr<FJsonObject> GraphItem = MakeShared<FJsonObject>();
		GraphItem->SetStringField(TEXT("graph_name"), Graph->GetName());
		GraphItem->SetStringField(TEXT("graph_path"), Graph->GetPathName());
		Graphs.Add(MakeShared<FJsonValueObject>(GraphItem));
	}

	OutData->SetStringField(TEXT("asset_path"), AnimBlueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("interface_class"), InterfaceClass->GetPathName());
	OutData->SetNumberField(TEXT("graph_count"), Graphs.Num());
	OutData->SetArrayField(TEXT("graphs"), Graphs);
	OutData->SetBoolField(TEXT("changed"), true);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterAdd);
	return true;
}

bool FUeAgentHttpServer::CmdAnimBlueprintRemoveLayerInterface(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString InterfaceClassPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("interface_class"), InterfaceClassPath) || InterfaceClassPath.IsEmpty())
	{
		OutError = TEXT("missing_interface_class");
		return false;
	}

	UAnimBlueprint* AnimBlueprint = UeAgentAnimBlueprintOps::LoadAnimBlueprint(AssetPathInput);
	if (!AnimBlueprint)
	{
		OutError = TEXT("anim_blueprint_not_found");
		return false;
	}

	UClass* InterfaceClass = UeAgentAnimBlueprintOps::ResolveAnimLayerInterfaceClass(InterfaceClassPath);
	if (!InterfaceClass || !InterfaceClass->IsChildOf(UAnimLayerInterface::StaticClass()))
	{
		OutError = TEXT("invalid_layer_interface_class");
		return false;
	}

	const FTopLevelAssetPath InterfaceClassPathName = InterfaceClass->GetClassPathName();
	bool bFound = false;
	for (const FBPInterfaceDescription& InterfaceDesc : AnimBlueprint->ImplementedInterfaces)
	{
		const UClass* ExistingInterface = InterfaceDesc.Interface.Get();
		if (ExistingInterface && ExistingInterface->GetClassPathName() == InterfaceClassPathName)
		{
			bFound = true;
			break;
		}
	}

	if (!bFound)
	{
		OutData->SetStringField(TEXT("asset_path"), AnimBlueprint->GetOutermost()->GetName());
		OutData->SetStringField(TEXT("interface_class"), InterfaceClass->GetPathName());
		OutData->SetBoolField(TEXT("changed"), false);
		return true;
	}

	bool bPreserveFunctions = false;
	JsonTryGetBool(Ctx.Params, TEXT("preserve_functions"), bPreserveFunctions);
	FBlueprintEditorUtils::RemoveInterface(AnimBlueprint, InterfaceClassPathName, bPreserveFunctions);

	bool bCompileAfterRemove = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_remove"), bCompileAfterRemove);
	bool bSaveAfterRemove = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_remove"), bSaveAfterRemove);
	if (!UeAgentAnimBlueprintOps::FinalizeAnimBlueprintStructureMutation(AnimBlueprint, bCompileAfterRemove, bSaveAfterRemove, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), AnimBlueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("interface_class"), InterfaceClass->GetPathName());
	OutData->SetBoolField(TEXT("preserve_functions"), bPreserveFunctions);
	OutData->SetBoolField(TEXT("changed"), true);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterRemove);
	return true;
}

bool FUeAgentHttpServer::CmdAnimBlueprintAddAnimLayer(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString LayerName;
	if (!JsonTryGetString(Ctx.Params, TEXT("layer_name"), LayerName) || LayerName.IsEmpty())
	{
		OutError = TEXT("missing_layer_name");
		return false;
	}

	UAnimBlueprint* AnimBlueprint = UeAgentAnimBlueprintOps::LoadAnimBlueprint(AssetPathInput);
	if (!AnimBlueprint)
	{
		OutError = TEXT("anim_blueprint_not_found");
		return false;
	}

	if (UEdGraph* ExistingGraph = UeAgentAnimBlueprintOps::ResolveAnyAnimBlueprintGraph(AnimBlueprint, LayerName))
	{
		if (UeAgentAnimBlueprintOps::IsAnimLayerGraph(ExistingGraph))
		{
			OutData->SetStringField(TEXT("asset_path"), AnimBlueprint->GetOutermost()->GetName());
			OutData->SetStringField(TEXT("layer_name"), ExistingGraph->GetName());
			OutData->SetStringField(TEXT("graph_path"), ExistingGraph->GetPathName());
			OutData->SetBoolField(TEXT("changed"), false);
			return true;
		}

		OutError = TEXT("graph_name_conflict");
		return false;
	}

	UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(AnimBlueprint, FName(*LayerName), UAnimationGraph::StaticClass(), UAnimationGraphSchema::StaticClass());
	if (!NewGraph)
	{
		OutError = TEXT("create_anim_layer_graph_failed");
		return false;
	}

	FBlueprintEditorUtils::AddDomainSpecificGraph(AnimBlueprint, NewGraph);

	bool bCompileAfterAdd = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_add"), bCompileAfterAdd);
	bool bSaveAfterAdd = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_add"), bSaveAfterAdd);
	if (!UeAgentAnimBlueprintOps::FinalizeAnimBlueprintStructureMutation(AnimBlueprint, bCompileAfterAdd, bSaveAfterAdd, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), AnimBlueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("layer_name"), NewGraph->GetName());
	OutData->SetStringField(TEXT("graph_path"), NewGraph->GetPathName());
	OutData->SetBoolField(TEXT("is_layer_interface_blueprint"), UeAgentAnimBlueprintOps::IsLayerInterfaceBlueprint(AnimBlueprint));
	OutData->SetBoolField(TEXT("changed"), true);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterAdd);
	return true;
}

bool FUeAgentHttpServer::CmdAnimBlueprintRemoveAnimLayer(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString LayerName;
	if (!JsonTryGetString(Ctx.Params, TEXT("layer_name"), LayerName) || LayerName.IsEmpty())
	{
		OutError = TEXT("missing_layer_name");
		return false;
	}

	UAnimBlueprint* AnimBlueprint = UeAgentAnimBlueprintOps::LoadAnimBlueprint(AssetPathInput);
	if (!AnimBlueprint)
	{
		OutError = TEXT("anim_blueprint_not_found");
		return false;
	}

	UEdGraph* LayerGraph = UeAgentAnimBlueprintOps::ResolveAnyAnimBlueprintGraph(AnimBlueprint, LayerName);
	if (!LayerGraph)
	{
		OutError = TEXT("anim_layer_not_found");
		return false;
	}
	if (!UeAgentAnimBlueprintOps::IsAnimLayerGraph(LayerGraph))
	{
		OutError = TEXT("graph_is_not_anim_layer");
		return false;
	}
	if (LayerGraph->InterfaceGuid.IsValid() && !UeAgentAnimBlueprintOps::IsLayerInterfaceBlueprint(AnimBlueprint))
	{
		OutError = TEXT("anim_layer_owned_by_interface_use_remove_layer_interface");
		return false;
	}

	bool bCompileAfterRemove = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_remove"), bCompileAfterRemove);
	const EGraphRemoveFlags::Type RemoveFlags = bCompileAfterRemove ? EGraphRemoveFlags::Default : EGraphRemoveFlags::MarkTransient;
	FBlueprintEditorUtils::RemoveGraph(AnimBlueprint, LayerGraph, RemoveFlags);
	if (!bCompileAfterRemove)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBlueprint);
	}

	bool bSaveAfterRemove = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_remove"), bSaveAfterRemove);
	if (bSaveAfterRemove && !UeAgentAnimBlueprintOps::SaveAnimBlueprintPackage(AnimBlueprint, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), AnimBlueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("layer_name"), LayerName);
	OutData->SetBoolField(TEXT("changed"), true);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterRemove);
	return true;
}

bool FUeAgentHttpServer::CmdAnimBlueprintRenameAnimLayer(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString LayerName;
	if (!JsonTryGetString(Ctx.Params, TEXT("layer_name"), LayerName) || LayerName.IsEmpty())
	{
		OutError = TEXT("missing_layer_name");
		return false;
	}

	FString NewLayerName;
	if (!JsonTryGetString(Ctx.Params, TEXT("new_layer_name"), NewLayerName) || NewLayerName.IsEmpty())
	{
		OutError = TEXT("missing_new_layer_name");
		return false;
	}

	UAnimBlueprint* AnimBlueprint = UeAgentAnimBlueprintOps::LoadAnimBlueprint(AssetPathInput);
	if (!AnimBlueprint)
	{
		OutError = TEXT("anim_blueprint_not_found");
		return false;
	}

	UEdGraph* LayerGraph = UeAgentAnimBlueprintOps::ResolveAnyAnimBlueprintGraph(AnimBlueprint, LayerName);
	if (!LayerGraph)
	{
		OutError = TEXT("anim_layer_not_found");
		return false;
	}
	if (!UeAgentAnimBlueprintOps::IsAnimLayerGraph(LayerGraph))
	{
		OutError = TEXT("graph_is_not_anim_layer");
		return false;
	}
	if (LayerGraph->InterfaceGuid.IsValid() && !UeAgentAnimBlueprintOps::IsLayerInterfaceBlueprint(AnimBlueprint))
	{
		OutError = TEXT("anim_layer_owned_by_interface_use_rename_interface_source");
		return false;
	}

	if (UEdGraph* ExistingGraph = UeAgentAnimBlueprintOps::ResolveAnyAnimBlueprintGraph(AnimBlueprint, NewLayerName))
	{
		if (ExistingGraph != LayerGraph)
		{
			OutError = TEXT("graph_name_conflict");
			return false;
		}
	}

	FBlueprintEditorUtils::RenameGraph(LayerGraph, NewLayerName);

	bool bCompileAfterRename = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_rename"), bCompileAfterRename);
	bool bSaveAfterRename = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_rename"), bSaveAfterRename);
	if (!UeAgentAnimBlueprintOps::FinalizeAnimBlueprintStructureMutation(AnimBlueprint, bCompileAfterRename, bSaveAfterRename, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), AnimBlueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("layer_name"), LayerName);
	OutData->SetStringField(TEXT("new_layer_name"), LayerGraph->GetName());
	OutData->SetStringField(TEXT("graph_path"), LayerGraph->GetPathName());
	OutData->SetBoolField(TEXT("saved"), bSaveAfterRename);
	return true;
}

bool FUeAgentHttpServer::CmdAnimBlueprintAddStateMachine(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString StateMachineName;
	if (!JsonTryGetString(Ctx.Params, TEXT("state_machine_name"), StateMachineName) || StateMachineName.IsEmpty())
	{
		OutError = TEXT("missing_state_machine_name");
		return false;
	}

	UAnimBlueprint* AnimBlueprint = UeAgentAnimBlueprintOps::LoadAnimBlueprint(AssetPathInput);
	if (!AnimBlueprint)
	{
		OutError = TEXT("anim_blueprint_not_found");
		return false;
	}
	if (UeAgentAnimBlueprintOps::IsLayerInterfaceBlueprint(AnimBlueprint))
	{
		OutError = TEXT("layer_interface_blueprint_does_not_support_state_machines");
		return false;
	}

	if (UAnimGraphNode_StateMachineBase* ExistingNode = UeAgentAnimBlueprintOps::FindStateMachineNode(AnimBlueprint, StateMachineName))
	{
		OutData->SetStringField(TEXT("asset_path"), AnimBlueprint->GetOutermost()->GetName());
		OutData->SetStringField(TEXT("state_machine_name"), ExistingNode->GetStateMachineName());
		OutData->SetStringField(TEXT("graph_path"), ExistingNode->EditorStateMachineGraph ? ExistingNode->EditorStateMachineGraph->GetPathName() : TEXT(""));
		OutData->SetBoolField(TEXT("changed"), false);
		return true;
	}

	if (UeAgentAnimBlueprintOps::ResolveAnyAnimBlueprintGraph(AnimBlueprint, StateMachineName))
	{
		OutError = TEXT("graph_name_conflict");
		return false;
	}

	FString AnimGraphName = TEXT("AnimGraph");
	JsonTryGetString(Ctx.Params, TEXT("anim_graph_name"), AnimGraphName);
	UEdGraph* AnimGraph = UeAgentAnimBlueprintOps::ResolveAnyAnimBlueprintGraph(AnimBlueprint, AnimGraphName);
	if (!AnimGraph || !AnimationEditorUtils::IsAnimGraph(AnimGraph))
	{
		OutError = TEXT("anim_graph_not_found");
		return false;
	}

	double XNum = 0.0;
	double YNum = 0.0;
	JsonTryGetNumber(Ctx.Params, TEXT("pos_x"), XNum);
	JsonTryGetNumber(Ctx.Params, TEXT("pos_y"), YNum);

	UAnimGraphNode_StateMachine* StateMachineNode = UeAgentAnimBlueprintOps::AddNodeToGraph<UAnimGraphNode_StateMachine>(AnimGraph, static_cast<int32>(XNum), static_cast<int32>(YNum));
	if (!StateMachineNode || !StateMachineNode->EditorStateMachineGraph)
	{
		OutError = TEXT("add_state_machine_failed");
		return false;
	}

	FBlueprintEditorUtils::RenameGraph(StateMachineNode->EditorStateMachineGraph, StateMachineName);

	bool bCompileAfterAdd = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_add"), bCompileAfterAdd);
	bool bSaveAfterAdd = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_add"), bSaveAfterAdd);
	if (!UeAgentAnimBlueprintOps::FinalizeAnimBlueprintStructureMutation(AnimBlueprint, bCompileAfterAdd, bSaveAfterAdd, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), AnimBlueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("state_machine_name"), StateMachineNode->EditorStateMachineGraph->GetName());
	OutData->SetStringField(TEXT("graph_path"), StateMachineNode->EditorStateMachineGraph->GetPathName());
	OutData->SetStringField(TEXT("owner_anim_graph_name"), AnimGraph->GetName());
	OutData->SetStringField(TEXT("node_guid"), UeAgentAnimBlueprintOps::NodeGuidToString(StateMachineNode));
	OutData->SetBoolField(TEXT("changed"), true);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterAdd);
	return true;
}

bool FUeAgentHttpServer::CmdAnimBlueprintRemoveStateMachine(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString StateMachineName;
	if (!JsonTryGetString(Ctx.Params, TEXT("state_machine_name"), StateMachineName) || StateMachineName.IsEmpty())
	{
		OutError = TEXT("missing_state_machine_name");
		return false;
	}

	UAnimBlueprint* AnimBlueprint = UeAgentAnimBlueprintOps::LoadAnimBlueprint(AssetPathInput);
	if (!AnimBlueprint)
	{
		OutError = TEXT("anim_blueprint_not_found");
		return false;
	}

	UAnimGraphNode_StateMachineBase* StateMachineNode = UeAgentAnimBlueprintOps::FindStateMachineNode(AnimBlueprint, StateMachineName);
	if (!StateMachineNode)
	{
		OutError = TEXT("state_machine_not_found");
		return false;
	}

	StateMachineNode->DestroyNode();

	bool bCompileAfterRemove = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_remove"), bCompileAfterRemove);
	bool bSaveAfterRemove = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_remove"), bSaveAfterRemove);
	if (!UeAgentAnimBlueprintOps::FinalizeAnimBlueprintStructureMutation(AnimBlueprint, bCompileAfterRemove, bSaveAfterRemove, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), AnimBlueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("state_machine_name"), StateMachineName);
	OutData->SetBoolField(TEXT("changed"), true);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterRemove);
	return true;
}

bool FUeAgentHttpServer::CmdAnimBlueprintRenameStateMachine(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString StateMachineName;
	if (!JsonTryGetString(Ctx.Params, TEXT("state_machine_name"), StateMachineName) || StateMachineName.IsEmpty())
	{
		OutError = TEXT("missing_state_machine_name");
		return false;
	}

	FString NewStateMachineName;
	if (!JsonTryGetString(Ctx.Params, TEXT("new_state_machine_name"), NewStateMachineName) || NewStateMachineName.IsEmpty())
	{
		OutError = TEXT("missing_new_state_machine_name");
		return false;
	}

	UAnimBlueprint* AnimBlueprint = UeAgentAnimBlueprintOps::LoadAnimBlueprint(AssetPathInput);
	if (!AnimBlueprint)
	{
		OutError = TEXT("anim_blueprint_not_found");
		return false;
	}

	UAnimGraphNode_StateMachineBase* StateMachineNode = UeAgentAnimBlueprintOps::FindStateMachineNode(AnimBlueprint, StateMachineName);
	if (!StateMachineNode || !StateMachineNode->EditorStateMachineGraph)
	{
		OutError = TEXT("state_machine_not_found");
		return false;
	}

	if (UEdGraph* ExistingGraph = UeAgentAnimBlueprintOps::ResolveAnyAnimBlueprintGraph(AnimBlueprint, NewStateMachineName))
	{
		if (ExistingGraph != StateMachineNode->EditorStateMachineGraph)
		{
			OutError = TEXT("graph_name_conflict");
			return false;
		}
	}

	FBlueprintEditorUtils::RenameGraph(StateMachineNode->EditorStateMachineGraph, NewStateMachineName);

	bool bCompileAfterRename = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_rename"), bCompileAfterRename);
	bool bSaveAfterRename = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_rename"), bSaveAfterRename);
	if (!UeAgentAnimBlueprintOps::FinalizeAnimBlueprintStructureMutation(AnimBlueprint, bCompileAfterRename, bSaveAfterRename, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), AnimBlueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("state_machine_name"), StateMachineName);
	OutData->SetStringField(TEXT("new_state_machine_name"), StateMachineNode->EditorStateMachineGraph->GetName());
	OutData->SetStringField(TEXT("graph_path"), StateMachineNode->EditorStateMachineGraph->GetPathName());
	OutData->SetBoolField(TEXT("saved"), bSaveAfterRename);
	return true;
}

bool FUeAgentHttpServer::CmdAnimBlueprintSetEntryState(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString StateMachineName;
	if (!JsonTryGetString(Ctx.Params, TEXT("state_machine_name"), StateMachineName) || StateMachineName.IsEmpty())
	{
		OutError = TEXT("missing_state_machine_name");
		return false;
	}

	UAnimBlueprint* AnimBlueprint = UeAgentAnimBlueprintOps::LoadAnimBlueprint(AssetPathInput);
	if (!AnimBlueprint)
	{
		OutError = TEXT("anim_blueprint_not_found");
		return false;
	}

	UAnimGraphNode_StateMachineBase* StateMachineNode = UeAgentAnimBlueprintOps::FindStateMachineNode(AnimBlueprint, StateMachineName);
	UAnimationStateMachineGraph* StateMachineGraph = StateMachineNode ? StateMachineNode->EditorStateMachineGraph : nullptr;
	if (!StateMachineGraph)
	{
		OutError = TEXT("state_machine_not_found");
		return false;
	}

	UAnimStateNodeBase* TargetStateNode = nullptr;
	FString NodeGuidText;
	if (JsonTryGetString(Ctx.Params, TEXT("node_guid"), NodeGuidText) && !NodeGuidText.IsEmpty())
	{
		FGuid NodeGuid;
		if (!UeAgentAnimBlueprintOps::ParseNodeGuid(NodeGuidText, NodeGuid))
		{
			OutError = TEXT("invalid_node_guid");
			return false;
		}
		TargetStateNode = UeAgentAnimBlueprintOps::FindStateNodeByGuid(StateMachineGraph, NodeGuid);
	}
	else
	{
		FString StateName;
		if (!JsonTryGetString(Ctx.Params, TEXT("state_name"), StateName) || StateName.IsEmpty())
		{
			OutError = TEXT("missing_state_name_or_node_guid");
			return false;
		}
		TargetStateNode = UeAgentAnimBlueprintOps::FindStateNodeByName(StateMachineGraph, StateName);
	}

	if (!TargetStateNode)
	{
		OutError = TEXT("entry_state_not_found");
		return false;
	}

	const UAnimStateNodeBase* PreviousEntryState = UeAgentAnimBlueprintOps::GetEntryConnectedState(StateMachineGraph);
	if (!UeAgentAnimBlueprintOps::SetEntryState(StateMachineGraph, TargetStateNode, OutError))
	{
		return false;
	}

	const bool bChanged = PreviousEntryState != TargetStateNode;

	bool bCompileAfterSet = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_set"), bCompileAfterSet);
	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (!UeAgentAnimBlueprintOps::FinalizeAnimBlueprintStructureMutation(AnimBlueprint, bCompileAfterSet, bSaveAfterSet, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), AnimBlueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("state_machine_name"), StateMachineGraph->GetName());
	OutData->SetStringField(TEXT("entry_state_name"), TargetStateNode->GetStateName());
	OutData->SetStringField(TEXT("entry_state_guid"), UeAgentAnimBlueprintOps::NodeGuidToString(TargetStateNode));
	OutData->SetBoolField(TEXT("changed"), bChanged);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdAnimBlueprintClearEntryState(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString StateMachineName;
	if (!JsonTryGetString(Ctx.Params, TEXT("state_machine_name"), StateMachineName) || StateMachineName.IsEmpty())
	{
		OutError = TEXT("missing_state_machine_name");
		return false;
	}

	UAnimBlueprint* AnimBlueprint = UeAgentAnimBlueprintOps::LoadAnimBlueprint(AssetPathInput);
	if (!AnimBlueprint)
	{
		OutError = TEXT("anim_blueprint_not_found");
		return false;
	}

	UAnimGraphNode_StateMachineBase* StateMachineNode = UeAgentAnimBlueprintOps::FindStateMachineNode(AnimBlueprint, StateMachineName);
	UAnimationStateMachineGraph* StateMachineGraph = StateMachineNode ? StateMachineNode->EditorStateMachineGraph : nullptr;
	if (!StateMachineGraph)
	{
		OutError = TEXT("state_machine_not_found");
		return false;
	}

	const UAnimStateNodeBase* PreviousEntryState = UeAgentAnimBlueprintOps::GetEntryConnectedState(StateMachineGraph);
	int32 BrokenLinkCount = 0;
	if (!UeAgentAnimBlueprintOps::ClearEntryState(StateMachineGraph, BrokenLinkCount, OutError))
	{
		return false;
	}

	bool bCompileAfterClear = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_clear"), bCompileAfterClear);
	bool bSaveAfterClear = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_clear"), bSaveAfterClear);
	if (!UeAgentAnimBlueprintOps::FinalizeAnimBlueprintStructureMutation(AnimBlueprint, bCompileAfterClear, bSaveAfterClear, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), AnimBlueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("state_machine_name"), StateMachineGraph->GetName());
	OutData->SetStringField(TEXT("previous_entry_state_name"), PreviousEntryState ? PreviousEntryState->GetStateName() : TEXT(""));
	OutData->SetStringField(TEXT("previous_entry_state_guid"), UeAgentAnimBlueprintOps::NodeGuidToString(PreviousEntryState));
	OutData->SetNumberField(TEXT("broken_link_count"), BrokenLinkCount);
	OutData->SetBoolField(TEXT("changed"), BrokenLinkCount > 0);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterClear);
	return true;
}

bool FUeAgentHttpServer::CmdAnimBlueprintAddState(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString StateMachineName;
	if (!JsonTryGetString(Ctx.Params, TEXT("state_machine_name"), StateMachineName) || StateMachineName.IsEmpty())
	{
		OutError = TEXT("missing_state_machine_name");
		return false;
	}

	FString StateName;
	if (!JsonTryGetString(Ctx.Params, TEXT("state_name"), StateName) || StateName.IsEmpty())
	{
		OutError = TEXT("missing_state_name");
		return false;
	}

	UAnimBlueprint* AnimBlueprint = UeAgentAnimBlueprintOps::LoadAnimBlueprint(AssetPathInput);
	if (!AnimBlueprint)
	{
		OutError = TEXT("anim_blueprint_not_found");
		return false;
	}

	UAnimGraphNode_StateMachineBase* StateMachineNode = UeAgentAnimBlueprintOps::FindStateMachineNode(AnimBlueprint, StateMachineName);
	UAnimationStateMachineGraph* StateMachineGraph = StateMachineNode ? StateMachineNode->EditorStateMachineGraph : nullptr;
	if (!StateMachineGraph)
	{
		OutError = TEXT("state_machine_not_found");
		return false;
	}

	if (UAnimStateNodeBase* ExistingStateNode = UeAgentAnimBlueprintOps::FindStateNodeByName(StateMachineGraph, StateName))
	{
		if (Cast<UAnimStateNode>(ExistingStateNode))
		{
			OutData->SetStringField(TEXT("asset_path"), AnimBlueprint->GetOutermost()->GetName());
			OutData->SetStringField(TEXT("state_machine_name"), StateMachineGraph->GetName());
			OutData->SetStringField(TEXT("state_name"), ExistingStateNode->GetStateName());
			OutData->SetStringField(TEXT("node_guid"), UeAgentAnimBlueprintOps::NodeGuidToString(ExistingStateNode));
			OutData->SetBoolField(TEXT("changed"), false);
			return true;
		}

		OutError = TEXT("state_node_name_conflict");
		return false;
	}

	double XNum = 0.0;
	double YNum = 0.0;
	JsonTryGetNumber(Ctx.Params, TEXT("pos_x"), XNum);
	JsonTryGetNumber(Ctx.Params, TEXT("pos_y"), YNum);

	UAnimStateNode* StateNode = UeAgentAnimBlueprintOps::AddNodeToGraph<UAnimStateNode>(StateMachineGraph, static_cast<int32>(XNum), static_cast<int32>(YNum));
	if (!StateNode || !StateNode->GetBoundGraph())
	{
		OutError = TEXT("add_state_failed");
		return false;
	}

	FBlueprintEditorUtils::RenameGraph(StateNode->GetBoundGraph(), StateName);

	FString StateType;
	if (JsonTryGetString(Ctx.Params, TEXT("state_type"), StateType))
	{
		if (StateType.Equals(TEXT("blend_graph"), ESearchCase::IgnoreCase))
		{
			StateNode->StateType = AST_BlendGraph;
		}
		else if (!StateType.IsEmpty() && !StateType.Equals(TEXT("single_animation"), ESearchCase::IgnoreCase))
		{
			OutError = TEXT("invalid_state_type");
			return false;
		}
	}

	JsonTryGetBool(Ctx.Params, TEXT("always_reset_on_entry"), StateNode->bAlwaysResetOnEntry);

	bool bCompileAfterAdd = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_add"), bCompileAfterAdd);
	bool bSaveAfterAdd = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_add"), bSaveAfterAdd);
	if (!UeAgentAnimBlueprintOps::FinalizeAnimBlueprintStructureMutation(AnimBlueprint, bCompileAfterAdd, bSaveAfterAdd, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), AnimBlueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("state_machine_name"), StateMachineGraph->GetName());
	OutData->SetStringField(TEXT("state_name"), StateNode->GetStateName());
	OutData->SetStringField(TEXT("node_guid"), UeAgentAnimBlueprintOps::NodeGuidToString(StateNode));
	OutData->SetStringField(TEXT("bound_graph_path"), StateNode->GetBoundGraph() ? StateNode->GetBoundGraph()->GetPathName() : TEXT(""));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterAdd);
	return true;
}

bool FUeAgentHttpServer::CmdAnimBlueprintSetStateProperties(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString StateMachineName;
	if (!JsonTryGetString(Ctx.Params, TEXT("state_machine_name"), StateMachineName) || StateMachineName.IsEmpty())
	{
		OutError = TEXT("missing_state_machine_name");
		return false;
	}

	UAnimBlueprint* AnimBlueprint = UeAgentAnimBlueprintOps::LoadAnimBlueprint(AssetPathInput);
	if (!AnimBlueprint)
	{
		OutError = TEXT("anim_blueprint_not_found");
		return false;
	}

	UAnimGraphNode_StateMachineBase* StateMachineNode = UeAgentAnimBlueprintOps::FindStateMachineNode(AnimBlueprint, StateMachineName);
	UAnimationStateMachineGraph* StateMachineGraph = StateMachineNode ? StateMachineNode->EditorStateMachineGraph : nullptr;
	if (!StateMachineGraph)
	{
		OutError = TEXT("state_machine_not_found");
		return false;
	}

	UAnimStateNodeBase* StateNodeBase = nullptr;
	FString NodeGuidText;
	if (JsonTryGetString(Ctx.Params, TEXT("node_guid"), NodeGuidText) && !NodeGuidText.IsEmpty())
	{
		FGuid NodeGuid;
		if (!UeAgentAnimBlueprintOps::ParseNodeGuid(NodeGuidText, NodeGuid))
		{
			OutError = TEXT("invalid_node_guid");
			return false;
		}
		StateNodeBase = UeAgentAnimBlueprintOps::FindStateNodeByGuid(StateMachineGraph, NodeGuid);
	}
	else
	{
		FString StateName;
		if (!JsonTryGetString(Ctx.Params, TEXT("state_name"), StateName) || StateName.IsEmpty())
		{
			OutError = TEXT("missing_state_name_or_node_guid");
			return false;
		}
		StateNodeBase = UeAgentAnimBlueprintOps::FindStateNodeByName(StateMachineGraph, StateName);
	}

	UAnimStateNode* StateNode = Cast<UAnimStateNode>(StateNodeBase);
	if (!StateNode)
	{
		OutError = TEXT("state_node_not_plain_state");
		return false;
	}

	bool bChanged = false;

	double XNum = 0.0;
	if (JsonTryGetNumber(Ctx.Params, TEXT("pos_x"), XNum))
	{
		const int32 NewPosX = static_cast<int32>(XNum);
		if (StateNode->NodePosX != NewPosX)
		{
			StateNode->Modify();
			StateNode->NodePosX = NewPosX;
			bChanged = true;
		}
	}

	double YNum = 0.0;
	if (JsonTryGetNumber(Ctx.Params, TEXT("pos_y"), YNum))
	{
		const int32 NewPosY = static_cast<int32>(YNum);
		if (StateNode->NodePosY != NewPosY)
		{
			StateNode->Modify();
			StateNode->NodePosY = NewPosY;
			bChanged = true;
		}
	}

	bool bAlwaysResetOnEntry = false;
	if (JsonTryGetBool(Ctx.Params, TEXT("always_reset_on_entry"), bAlwaysResetOnEntry) && StateNode->bAlwaysResetOnEntry != bAlwaysResetOnEntry)
	{
		StateNode->Modify();
		StateNode->bAlwaysResetOnEntry = bAlwaysResetOnEntry;
		bChanged = true;
	}

	FString StateType;
	if (JsonTryGetString(Ctx.Params, TEXT("state_type"), StateType) && !StateType.IsEmpty())
	{
		EAnimStateType NewStateType = StateNode->StateType;
		if (StateType.Equals(TEXT("blend_graph"), ESearchCase::IgnoreCase))
		{
			NewStateType = AST_BlendGraph;
		}
		else if (StateType.Equals(TEXT("single_animation"), ESearchCase::IgnoreCase))
		{
			NewStateType = AST_SingleAnimation;
		}
		else
		{
			OutError = TEXT("invalid_state_type");
			return false;
		}

		if (StateNode->StateType != NewStateType)
		{
			StateNode->Modify();
			StateNode->StateType = NewStateType;
			bChanged = true;
		}
	}

	bool bCompileAfterSet = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_set"), bCompileAfterSet);
	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bChanged && !UeAgentAnimBlueprintOps::FinalizeAnimBlueprintStructureMutation(AnimBlueprint, bCompileAfterSet, bSaveAfterSet, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), AnimBlueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("state_machine_name"), StateMachineGraph->GetName());
	OutData->SetStringField(TEXT("state_name"), StateNode->GetStateName());
	OutData->SetStringField(TEXT("node_guid"), UeAgentAnimBlueprintOps::NodeGuidToString(StateNode));
	OutData->SetStringField(TEXT("bound_graph_path"), StateNode->GetBoundGraph() ? StateNode->GetBoundGraph()->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("state_type"), StateNode->StateType == AST_BlendGraph ? TEXT("blend_graph") : TEXT("single_animation"));
	OutData->SetBoolField(TEXT("always_reset_on_entry"), StateNode->bAlwaysResetOnEntry);
	OutData->SetNumberField(TEXT("node_pos_x"), StateNode->NodePosX);
	OutData->SetNumberField(TEXT("node_pos_y"), StateNode->NodePosY);
	OutData->SetBoolField(TEXT("changed"), bChanged);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet && bChanged);
	return true;
}

bool FUeAgentHttpServer::CmdAnimBlueprintAddConduit(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString StateMachineName;
	if (!JsonTryGetString(Ctx.Params, TEXT("state_machine_name"), StateMachineName) || StateMachineName.IsEmpty())
	{
		OutError = TEXT("missing_state_machine_name");
		return false;
	}

	FString ConduitName;
	if (!JsonTryGetString(Ctx.Params, TEXT("conduit_name"), ConduitName) || ConduitName.IsEmpty())
	{
		OutError = TEXT("missing_conduit_name");
		return false;
	}

	UAnimBlueprint* AnimBlueprint = UeAgentAnimBlueprintOps::LoadAnimBlueprint(AssetPathInput);
	if (!AnimBlueprint)
	{
		OutError = TEXT("anim_blueprint_not_found");
		return false;
	}

	UAnimGraphNode_StateMachineBase* StateMachineNode = UeAgentAnimBlueprintOps::FindStateMachineNode(AnimBlueprint, StateMachineName);
	UAnimationStateMachineGraph* StateMachineGraph = StateMachineNode ? StateMachineNode->EditorStateMachineGraph : nullptr;
	if (!StateMachineGraph)
	{
		OutError = TEXT("state_machine_not_found");
		return false;
	}

	if (UAnimStateNodeBase* ExistingStateNode = UeAgentAnimBlueprintOps::FindStateNodeByName(StateMachineGraph, ConduitName))
	{
		if (Cast<UAnimStateConduitNode>(ExistingStateNode))
		{
			OutData->SetStringField(TEXT("asset_path"), AnimBlueprint->GetOutermost()->GetName());
			OutData->SetStringField(TEXT("state_machine_name"), StateMachineGraph->GetName());
			OutData->SetStringField(TEXT("conduit_name"), ExistingStateNode->GetStateName());
			OutData->SetStringField(TEXT("node_guid"), UeAgentAnimBlueprintOps::NodeGuidToString(ExistingStateNode));
			OutData->SetBoolField(TEXT("changed"), false);
			return true;
		}

		OutError = TEXT("state_node_name_conflict");
		return false;
	}

	double XNum = 0.0;
	double YNum = 0.0;
	JsonTryGetNumber(Ctx.Params, TEXT("pos_x"), XNum);
	JsonTryGetNumber(Ctx.Params, TEXT("pos_y"), YNum);

	UAnimStateConduitNode* ConduitNode = UeAgentAnimBlueprintOps::AddNodeToGraph<UAnimStateConduitNode>(StateMachineGraph, static_cast<int32>(XNum), static_cast<int32>(YNum));
	if (!ConduitNode || !ConduitNode->GetBoundGraph())
	{
		OutError = TEXT("add_conduit_failed");
		return false;
	}

	FBlueprintEditorUtils::RenameGraph(ConduitNode->GetBoundGraph(), ConduitName);

	bool bCompileAfterAdd = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_add"), bCompileAfterAdd);
	bool bSaveAfterAdd = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_add"), bSaveAfterAdd);
	if (!UeAgentAnimBlueprintOps::FinalizeAnimBlueprintStructureMutation(AnimBlueprint, bCompileAfterAdd, bSaveAfterAdd, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), AnimBlueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("state_machine_name"), StateMachineGraph->GetName());
	OutData->SetStringField(TEXT("conduit_name"), ConduitNode->GetStateName());
	OutData->SetStringField(TEXT("node_guid"), UeAgentAnimBlueprintOps::NodeGuidToString(ConduitNode));
	OutData->SetStringField(TEXT("bound_graph_path"), ConduitNode->GetBoundGraph() ? ConduitNode->GetBoundGraph()->GetPathName() : TEXT(""));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterAdd);
	return true;
}

bool FUeAgentHttpServer::CmdAnimBlueprintAddStateAlias(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString StateMachineName;
	if (!JsonTryGetString(Ctx.Params, TEXT("state_machine_name"), StateMachineName) || StateMachineName.IsEmpty())
	{
		OutError = TEXT("missing_state_machine_name");
		return false;
	}

	FString AliasName;
	if (!JsonTryGetString(Ctx.Params, TEXT("alias_name"), AliasName) || AliasName.IsEmpty())
	{
		OutError = TEXT("missing_alias_name");
		return false;
	}

	UAnimBlueprint* AnimBlueprint = UeAgentAnimBlueprintOps::LoadAnimBlueprint(AssetPathInput);
	if (!AnimBlueprint)
	{
		OutError = TEXT("anim_blueprint_not_found");
		return false;
	}

	UAnimGraphNode_StateMachineBase* StateMachineNode = UeAgentAnimBlueprintOps::FindStateMachineNode(AnimBlueprint, StateMachineName);
	UAnimationStateMachineGraph* StateMachineGraph = StateMachineNode ? StateMachineNode->EditorStateMachineGraph : nullptr;
	if (!StateMachineGraph)
	{
		OutError = TEXT("state_machine_not_found");
		return false;
	}

	if (UAnimStateNodeBase* ExistingStateNode = UeAgentAnimBlueprintOps::FindStateNodeByName(StateMachineGraph, AliasName))
	{
		if (Cast<UAnimStateAliasNode>(ExistingStateNode))
		{
			OutData->SetStringField(TEXT("asset_path"), AnimBlueprint->GetOutermost()->GetName());
			OutData->SetStringField(TEXT("state_machine_name"), StateMachineGraph->GetName());
			OutData->SetStringField(TEXT("alias_name"), ExistingStateNode->GetStateName());
			OutData->SetStringField(TEXT("node_guid"), UeAgentAnimBlueprintOps::NodeGuidToString(ExistingStateNode));
			OutData->SetBoolField(TEXT("changed"), false);
			return true;
		}

		OutError = TEXT("state_node_name_conflict");
		return false;
	}

	double XNum = 0.0;
	double YNum = 0.0;
	JsonTryGetNumber(Ctx.Params, TEXT("pos_x"), XNum);
	JsonTryGetNumber(Ctx.Params, TEXT("pos_y"), YNum);

	UAnimStateAliasNode* AliasNode = UeAgentAnimBlueprintOps::AddNodeToGraph<UAnimStateAliasNode>(StateMachineGraph, static_cast<int32>(XNum), static_cast<int32>(YNum));
	if (!AliasNode)
	{
		OutError = TEXT("add_state_alias_failed");
		return false;
	}

	AliasNode->OnRenameNode(AliasName);
	JsonTryGetBool(Ctx.Params, TEXT("global_alias"), AliasNode->bGlobalAlias);

	if (const TArray<TSharedPtr<FJsonValue>>* AliasTargetValues = nullptr; Ctx.Params.IsValid() && Ctx.Params->TryGetArrayField(TEXT("alias_target_states"), AliasTargetValues) && AliasTargetValues)
	{
		AliasNode->GetAliasedStates().Reset();
		for (const TSharedPtr<FJsonValue>& AliasTargetValue : *AliasTargetValues)
		{
			const FString AliasTargetName = AliasTargetValue.IsValid() ? AliasTargetValue->AsString() : FString();
			if (AliasTargetName.IsEmpty())
			{
				continue;
			}

			if (UAnimStateNode* TargetStateNode = Cast<UAnimStateNode>(UeAgentAnimBlueprintOps::FindStateNodeByName(StateMachineGraph, AliasTargetName)))
			{
				AliasNode->GetAliasedStates().Add(TargetStateNode);
			}
			else
			{
				OutError = TEXT("alias_target_state_not_found");
				return false;
			}
		}
	}

	bool bCompileAfterAdd = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_add"), bCompileAfterAdd);
	bool bSaveAfterAdd = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_add"), bSaveAfterAdd);
	if (!UeAgentAnimBlueprintOps::FinalizeAnimBlueprintStructureMutation(AnimBlueprint, bCompileAfterAdd, bSaveAfterAdd, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), AnimBlueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("state_machine_name"), StateMachineGraph->GetName());
	OutData->SetStringField(TEXT("alias_name"), AliasNode->GetStateName());
	OutData->SetStringField(TEXT("node_guid"), UeAgentAnimBlueprintOps::NodeGuidToString(AliasNode));
	OutData->SetBoolField(TEXT("global_alias"), AliasNode->bGlobalAlias);
	OutData->SetArrayField(TEXT("alias_targets"), UeAgentAnimBlueprintOps::MakeStringArray(UeAgentAnimBlueprintOps::CollectAliasTargetNames(AliasNode)));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterAdd);
	return true;
}

bool FUeAgentHttpServer::CmdAnimBlueprintRemoveStateNode(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString StateMachineName;
	if (!JsonTryGetString(Ctx.Params, TEXT("state_machine_name"), StateMachineName) || StateMachineName.IsEmpty())
	{
		OutError = TEXT("missing_state_machine_name");
		return false;
	}

	UAnimBlueprint* AnimBlueprint = UeAgentAnimBlueprintOps::LoadAnimBlueprint(AssetPathInput);
	if (!AnimBlueprint)
	{
		OutError = TEXT("anim_blueprint_not_found");
		return false;
	}

	UAnimGraphNode_StateMachineBase* StateMachineNode = UeAgentAnimBlueprintOps::FindStateMachineNode(AnimBlueprint, StateMachineName);
	UAnimationStateMachineGraph* StateMachineGraph = StateMachineNode ? StateMachineNode->EditorStateMachineGraph : nullptr;
	if (!StateMachineGraph)
	{
		OutError = TEXT("state_machine_not_found");
		return false;
	}

	UAnimStateNodeBase* StateNode = nullptr;
	FString NodeGuidText;
	if (JsonTryGetString(Ctx.Params, TEXT("node_guid"), NodeGuidText) && !NodeGuidText.IsEmpty())
	{
		FGuid NodeGuid;
		if (!UeAgentAnimBlueprintOps::ParseNodeGuid(NodeGuidText, NodeGuid))
		{
			OutError = TEXT("invalid_node_guid");
			return false;
		}
		StateNode = UeAgentAnimBlueprintOps::FindStateNodeByGuid(StateMachineGraph, NodeGuid);
	}
	else
	{
		FString StateName;
		if (!JsonTryGetString(Ctx.Params, TEXT("state_name"), StateName) || StateName.IsEmpty())
		{
			OutError = TEXT("missing_state_name_or_node_guid");
			return false;
		}
		StateNode = UeAgentAnimBlueprintOps::FindStateNodeByName(StateMachineGraph, StateName);
	}

	if (!StateNode)
	{
		OutError = TEXT("state_node_not_found");
		return false;
	}

	const FString RemovedNodeType = UeAgentAnimBlueprintOps::StateNodeTypeToString(StateNode);
	const FString RemovedStateName = StateNode->GetStateName();
	StateNode->DestroyNode();

	bool bCompileAfterRemove = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_remove"), bCompileAfterRemove);
	bool bSaveAfterRemove = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_remove"), bSaveAfterRemove);
	if (!UeAgentAnimBlueprintOps::FinalizeAnimBlueprintStructureMutation(AnimBlueprint, bCompileAfterRemove, bSaveAfterRemove, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), AnimBlueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("state_machine_name"), StateMachineGraph->GetName());
	OutData->SetStringField(TEXT("state_name"), RemovedStateName);
	OutData->SetStringField(TEXT("node_type"), RemovedNodeType);
	OutData->SetBoolField(TEXT("changed"), true);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterRemove);
	return true;
}

bool FUeAgentHttpServer::CmdAnimBlueprintRenameStateNode(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString StateMachineName;
	if (!JsonTryGetString(Ctx.Params, TEXT("state_machine_name"), StateMachineName) || StateMachineName.IsEmpty())
	{
		OutError = TEXT("missing_state_machine_name");
		return false;
	}

	FString NewStateName;
	if (!JsonTryGetString(Ctx.Params, TEXT("new_state_name"), NewStateName) || NewStateName.IsEmpty())
	{
		OutError = TEXT("missing_new_state_name");
		return false;
	}

	UAnimBlueprint* AnimBlueprint = UeAgentAnimBlueprintOps::LoadAnimBlueprint(AssetPathInput);
	if (!AnimBlueprint)
	{
		OutError = TEXT("anim_blueprint_not_found");
		return false;
	}

	UAnimGraphNode_StateMachineBase* StateMachineNode = UeAgentAnimBlueprintOps::FindStateMachineNode(AnimBlueprint, StateMachineName);
	UAnimationStateMachineGraph* StateMachineGraph = StateMachineNode ? StateMachineNode->EditorStateMachineGraph : nullptr;
	if (!StateMachineGraph)
	{
		OutError = TEXT("state_machine_not_found");
		return false;
	}

	UAnimStateNodeBase* StateNode = nullptr;
	FString NodeGuidText;
	if (JsonTryGetString(Ctx.Params, TEXT("node_guid"), NodeGuidText) && !NodeGuidText.IsEmpty())
	{
		FGuid NodeGuid;
		if (!UeAgentAnimBlueprintOps::ParseNodeGuid(NodeGuidText, NodeGuid))
		{
			OutError = TEXT("invalid_node_guid");
			return false;
		}
		StateNode = UeAgentAnimBlueprintOps::FindStateNodeByGuid(StateMachineGraph, NodeGuid);
	}
	else
	{
		FString StateName;
		if (!JsonTryGetString(Ctx.Params, TEXT("state_name"), StateName) || StateName.IsEmpty())
		{
			OutError = TEXT("missing_state_name_or_node_guid");
			return false;
		}
		StateNode = UeAgentAnimBlueprintOps::FindStateNodeByName(StateMachineGraph, StateName);
	}

	if (!StateNode)
	{
		OutError = TEXT("state_node_not_found");
		return false;
	}

	if (UEdGraph* ExistingGraph = UeAgentAnimBlueprintOps::ResolveAnyAnimBlueprintGraph(AnimBlueprint, NewStateName))
	{
		if (ExistingGraph != StateNode->GetBoundGraph())
		{
			OutError = TEXT("graph_name_conflict");
			return false;
		}
	}
	if (UAnimStateNodeBase* ExistingStateNode = UeAgentAnimBlueprintOps::FindStateNodeByName(StateMachineGraph, NewStateName))
	{
		if (ExistingStateNode != StateNode)
		{
			OutError = TEXT("state_node_name_conflict");
			return false;
		}
	}

	const FString OldStateName = StateNode->GetStateName();
	if (UAnimStateAliasNode* AliasNode = Cast<UAnimStateAliasNode>(StateNode))
	{
		AliasNode->OnRenameNode(NewStateName);
	}
	else if (UEdGraph* BoundGraph = StateNode->GetBoundGraph())
	{
		FBlueprintEditorUtils::RenameGraph(BoundGraph, NewStateName);
	}
	else
	{
		OutError = TEXT("state_node_has_no_rename_target");
		return false;
	}

	bool bCompileAfterRename = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_rename"), bCompileAfterRename);
	bool bSaveAfterRename = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_rename"), bSaveAfterRename);
	if (!UeAgentAnimBlueprintOps::FinalizeAnimBlueprintStructureMutation(AnimBlueprint, bCompileAfterRename, bSaveAfterRename, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), AnimBlueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("state_machine_name"), StateMachineGraph->GetName());
	OutData->SetStringField(TEXT("state_name"), OldStateName);
	OutData->SetStringField(TEXT("new_state_name"), StateNode->GetStateName());
	OutData->SetStringField(TEXT("node_type"), UeAgentAnimBlueprintOps::StateNodeTypeToString(StateNode));
	OutData->SetStringField(TEXT("bound_graph_path"), StateNode->GetBoundGraph() ? StateNode->GetBoundGraph()->GetPathName() : TEXT(""));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterRename);
	return true;
}

bool FUeAgentHttpServer::CmdAnimBlueprintSetStateAliasTargets(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString StateMachineName;
	if (!JsonTryGetString(Ctx.Params, TEXT("state_machine_name"), StateMachineName) || StateMachineName.IsEmpty())
	{
		OutError = TEXT("missing_state_machine_name");
		return false;
	}

	UAnimBlueprint* AnimBlueprint = UeAgentAnimBlueprintOps::LoadAnimBlueprint(AssetPathInput);
	if (!AnimBlueprint)
	{
		OutError = TEXT("anim_blueprint_not_found");
		return false;
	}

	UAnimGraphNode_StateMachineBase* StateMachineNode = UeAgentAnimBlueprintOps::FindStateMachineNode(AnimBlueprint, StateMachineName);
	UAnimationStateMachineGraph* StateMachineGraph = StateMachineNode ? StateMachineNode->EditorStateMachineGraph : nullptr;
	if (!StateMachineGraph)
	{
		OutError = TEXT("state_machine_not_found");
		return false;
	}

	UAnimStateAliasNode* AliasNode = nullptr;
	FString NodeGuidText;
	if (JsonTryGetString(Ctx.Params, TEXT("node_guid"), NodeGuidText) && !NodeGuidText.IsEmpty())
	{
		FGuid NodeGuid;
		if (!UeAgentAnimBlueprintOps::ParseNodeGuid(NodeGuidText, NodeGuid))
		{
			OutError = TEXT("invalid_node_guid");
			return false;
		}
		AliasNode = Cast<UAnimStateAliasNode>(UeAgentAnimBlueprintOps::FindStateNodeByGuid(StateMachineGraph, NodeGuid));
	}
	else
	{
		FString AliasName;
		if (!JsonTryGetString(Ctx.Params, TEXT("alias_name"), AliasName) || AliasName.IsEmpty())
		{
			OutError = TEXT("missing_alias_name_or_node_guid");
			return false;
		}
		AliasNode = Cast<UAnimStateAliasNode>(UeAgentAnimBlueprintOps::FindStateNodeByName(StateMachineGraph, AliasName));
	}

	if (!AliasNode)
	{
		OutError = TEXT("state_alias_not_found");
		return false;
	}

	bool bGlobalAlias = AliasNode->bGlobalAlias;
	JsonTryGetBool(Ctx.Params, TEXT("global_alias"), bGlobalAlias);
	AliasNode->bGlobalAlias = bGlobalAlias;

	const TArray<TSharedPtr<FJsonValue>>* AliasTargetValues = nullptr;
	if (!Ctx.Params.IsValid() || !Ctx.Params->TryGetArrayField(TEXT("alias_target_states"), AliasTargetValues) || !AliasTargetValues)
	{
		OutError = TEXT("missing_alias_target_states");
		return false;
	}

	AliasNode->GetAliasedStates().Reset();
	for (const TSharedPtr<FJsonValue>& AliasTargetValue : *AliasTargetValues)
	{
		const FString AliasTargetName = AliasTargetValue.IsValid() ? AliasTargetValue->AsString() : FString();
		if (AliasTargetName.IsEmpty())
		{
			continue;
		}

		UAnimStateNode* TargetStateNode = Cast<UAnimStateNode>(UeAgentAnimBlueprintOps::FindStateNodeByName(StateMachineGraph, AliasTargetName));
		if (!TargetStateNode)
		{
			OutError = TEXT("alias_target_state_not_found");
			return false;
		}

		AliasNode->GetAliasedStates().Add(TargetStateNode);
	}

	bool bCompileAfterSet = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_set"), bCompileAfterSet);
	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (!UeAgentAnimBlueprintOps::FinalizeAnimBlueprintStructureMutation(AnimBlueprint, bCompileAfterSet, bSaveAfterSet, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), AnimBlueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("state_machine_name"), StateMachineGraph->GetName());
	OutData->SetStringField(TEXT("alias_name"), AliasNode->GetStateName());
	OutData->SetBoolField(TEXT("global_alias"), AliasNode->bGlobalAlias);
	OutData->SetArrayField(TEXT("alias_targets"), UeAgentAnimBlueprintOps::MakeStringArray(UeAgentAnimBlueprintOps::CollectAliasTargetNames(AliasNode)));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdAnimBlueprintAddTransition(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString StateMachineName;
	if (!JsonTryGetString(Ctx.Params, TEXT("state_machine_name"), StateMachineName) || StateMachineName.IsEmpty())
	{
		OutError = TEXT("missing_state_machine_name");
		return false;
	}

	FString SourceStateName;
	if (!JsonTryGetString(Ctx.Params, TEXT("source_state_name"), SourceStateName) || SourceStateName.IsEmpty())
	{
		OutError = TEXT("missing_source_state_name");
		return false;
	}

	FString TargetStateName;
	if (!JsonTryGetString(Ctx.Params, TEXT("target_state_name"), TargetStateName) || TargetStateName.IsEmpty())
	{
		OutError = TEXT("missing_target_state_name");
		return false;
	}

	UAnimBlueprint* AnimBlueprint = UeAgentAnimBlueprintOps::LoadAnimBlueprint(AssetPathInput);
	if (!AnimBlueprint)
	{
		OutError = TEXT("anim_blueprint_not_found");
		return false;
	}

	UAnimGraphNode_StateMachineBase* StateMachineNode = UeAgentAnimBlueprintOps::FindStateMachineNode(AnimBlueprint, StateMachineName);
	UAnimationStateMachineGraph* StateMachineGraph = StateMachineNode ? StateMachineNode->EditorStateMachineGraph : nullptr;
	if (!StateMachineGraph)
	{
		OutError = TEXT("state_machine_not_found");
		return false;
	}

	UAnimStateNodeBase* SourceStateNode = UeAgentAnimBlueprintOps::FindStateNodeByName(StateMachineGraph, SourceStateName);
	UAnimStateNodeBase* TargetStateNode = UeAgentAnimBlueprintOps::FindStateNodeByName(StateMachineGraph, TargetStateName);
	if (!SourceStateNode)
	{
		OutError = TEXT("source_state_not_found");
		return false;
	}
	if (!TargetStateNode)
	{
		OutError = TEXT("target_state_not_found");
		return false;
	}

	if (UAnimStateTransitionNode* ExistingTransitionNode = UeAgentAnimBlueprintOps::FindTransitionNode(StateMachineGraph, SourceStateName, TargetStateName))
	{
		OutData->SetStringField(TEXT("asset_path"), AnimBlueprint->GetOutermost()->GetName());
		OutData->SetStringField(TEXT("state_machine_name"), StateMachineGraph->GetName());
		OutData->SetStringField(TEXT("source_state_name"), SourceStateName);
		OutData->SetStringField(TEXT("target_state_name"), TargetStateName);
		OutData->SetStringField(TEXT("node_guid"), UeAgentAnimBlueprintOps::NodeGuidToString(ExistingTransitionNode));
		OutData->SetBoolField(TEXT("changed"), false);
		return true;
	}

	double XNum = (SourceStateNode->NodePosX + TargetStateNode->NodePosX) * 0.5;
	double YNum = (SourceStateNode->NodePosY + TargetStateNode->NodePosY) * 0.5;
	JsonTryGetNumber(Ctx.Params, TEXT("pos_x"), XNum);
	JsonTryGetNumber(Ctx.Params, TEXT("pos_y"), YNum);

	UAnimStateTransitionNode* TransitionNode = UeAgentAnimBlueprintOps::AddNodeToGraph<UAnimStateTransitionNode>(StateMachineGraph, static_cast<int32>(XNum), static_cast<int32>(YNum));
	if (!TransitionNode)
	{
		OutError = TEXT("add_transition_failed");
		return false;
	}

	TransitionNode->CreateConnections(SourceStateNode, TargetStateNode);
	JsonTryGetBool(Ctx.Params, TEXT("bidirectional"), TransitionNode->Bidirectional);
	JsonTryGetBool(Ctx.Params, TEXT("disabled"), TransitionNode->bDisabled);
	JsonTryGetBool(Ctx.Params, TEXT("automatic_rule"), TransitionNode->bAutomaticRuleBasedOnSequencePlayerInState);

	double CrossfadeDuration = TransitionNode->CrossfadeDuration;
	if (JsonTryGetNumber(Ctx.Params, TEXT("crossfade_duration"), CrossfadeDuration))
	{
		TransitionNode->CrossfadeDuration = static_cast<float>(FMath::Max(0.0, CrossfadeDuration));
	}

	double PriorityOrder = TransitionNode->PriorityOrder;
	if (JsonTryGetNumber(Ctx.Params, TEXT("priority_order"), PriorityOrder))
	{
		TransitionNode->PriorityOrder = FMath::Max(0, FMath::RoundToInt(PriorityOrder));
	}

	bool bCompileAfterAdd = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_add"), bCompileAfterAdd);
	bool bSaveAfterAdd = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_add"), bSaveAfterAdd);
	if (!UeAgentAnimBlueprintOps::FinalizeAnimBlueprintStructureMutation(AnimBlueprint, bCompileAfterAdd, bSaveAfterAdd, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), AnimBlueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("state_machine_name"), StateMachineGraph->GetName());
	OutData->SetStringField(TEXT("source_state_name"), SourceStateNode->GetStateName());
	OutData->SetStringField(TEXT("target_state_name"), TargetStateNode->GetStateName());
	OutData->SetStringField(TEXT("node_guid"), UeAgentAnimBlueprintOps::NodeGuidToString(TransitionNode));
	OutData->SetStringField(TEXT("bound_graph_path"), TransitionNode->GetBoundGraph() ? TransitionNode->GetBoundGraph()->GetPathName() : TEXT(""));
	OutData->SetBoolField(TEXT("bidirectional"), TransitionNode->Bidirectional);
	OutData->SetNumberField(TEXT("crossfade_duration"), TransitionNode->CrossfadeDuration);
	OutData->SetNumberField(TEXT("priority_order"), TransitionNode->PriorityOrder);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterAdd);
	return true;
}

bool FUeAgentHttpServer::CmdAnimBlueprintSetTransitionProperties(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString StateMachineName;
	if (!JsonTryGetString(Ctx.Params, TEXT("state_machine_name"), StateMachineName) || StateMachineName.IsEmpty())
	{
		OutError = TEXT("missing_state_machine_name");
		return false;
	}

	UAnimBlueprint* AnimBlueprint = UeAgentAnimBlueprintOps::LoadAnimBlueprint(AssetPathInput);
	if (!AnimBlueprint)
	{
		OutError = TEXT("anim_blueprint_not_found");
		return false;
	}

	UAnimGraphNode_StateMachineBase* StateMachineNode = UeAgentAnimBlueprintOps::FindStateMachineNode(AnimBlueprint, StateMachineName);
	UAnimationStateMachineGraph* StateMachineGraph = StateMachineNode ? StateMachineNode->EditorStateMachineGraph : nullptr;
	if (!StateMachineGraph)
	{
		OutError = TEXT("state_machine_not_found");
		return false;
	}

	UAnimStateTransitionNode* TransitionNode = nullptr;
	FString NodeGuidText;
	if (JsonTryGetString(Ctx.Params, TEXT("node_guid"), NodeGuidText) && !NodeGuidText.IsEmpty())
	{
		FGuid NodeGuid;
		if (!UeAgentAnimBlueprintOps::ParseNodeGuid(NodeGuidText, NodeGuid))
		{
			OutError = TEXT("invalid_node_guid");
			return false;
		}
		TransitionNode = UeAgentAnimBlueprintOps::FindTransitionNodeByGuid(StateMachineGraph, NodeGuid);
	}
	else
	{
		FString SourceStateName;
		FString TargetStateName;
		if (!JsonTryGetString(Ctx.Params, TEXT("source_state_name"), SourceStateName) || SourceStateName.IsEmpty())
		{
			OutError = TEXT("missing_source_state_name_or_node_guid");
			return false;
		}
		if (!JsonTryGetString(Ctx.Params, TEXT("target_state_name"), TargetStateName) || TargetStateName.IsEmpty())
		{
			OutError = TEXT("missing_target_state_name");
			return false;
		}

		TransitionNode = UeAgentAnimBlueprintOps::FindTransitionNode(StateMachineGraph, SourceStateName, TargetStateName);
	}

	if (!TransitionNode)
	{
		OutError = TEXT("transition_not_found");
		return false;
	}

	bool bChanged = false;

	double XNum = 0.0;
	if (JsonTryGetNumber(Ctx.Params, TEXT("pos_x"), XNum))
	{
		const int32 NewPosX = static_cast<int32>(XNum);
		if (TransitionNode->NodePosX != NewPosX)
		{
			TransitionNode->Modify();
			TransitionNode->NodePosX = NewPosX;
			bChanged = true;
		}
	}

	double YNum = 0.0;
	if (JsonTryGetNumber(Ctx.Params, TEXT("pos_y"), YNum))
	{
		const int32 NewPosY = static_cast<int32>(YNum);
		if (TransitionNode->NodePosY != NewPosY)
		{
			TransitionNode->Modify();
			TransitionNode->NodePosY = NewPosY;
			bChanged = true;
		}
	}

	double CrossfadeDuration = TransitionNode->CrossfadeDuration;
	if (JsonTryGetNumber(Ctx.Params, TEXT("crossfade_duration"), CrossfadeDuration))
	{
		const float NewCrossfadeDuration = static_cast<float>(FMath::Max(0.0, CrossfadeDuration));
		if (!FMath::IsNearlyEqual(TransitionNode->CrossfadeDuration, NewCrossfadeDuration))
		{
			TransitionNode->Modify();
			TransitionNode->CrossfadeDuration = NewCrossfadeDuration;
			bChanged = true;
		}
	}

	double PriorityOrder = TransitionNode->PriorityOrder;
	if (JsonTryGetNumber(Ctx.Params, TEXT("priority_order"), PriorityOrder))
	{
		const int32 NewPriorityOrder = FMath::Max(0, FMath::RoundToInt(PriorityOrder));
		if (TransitionNode->PriorityOrder != NewPriorityOrder)
		{
			TransitionNode->Modify();
			TransitionNode->PriorityOrder = NewPriorityOrder;
			bChanged = true;
		}
	}

	bool bBidirectional = false;
	if (JsonTryGetBool(Ctx.Params, TEXT("bidirectional"), bBidirectional) && TransitionNode->Bidirectional != bBidirectional)
	{
		TransitionNode->Modify();
		TransitionNode->Bidirectional = bBidirectional;
		bChanged = true;
	}

	bool bDisabled = false;
	if (JsonTryGetBool(Ctx.Params, TEXT("disabled"), bDisabled) && TransitionNode->bDisabled != bDisabled)
	{
		TransitionNode->Modify();
		TransitionNode->bDisabled = bDisabled;
		bChanged = true;
	}

	bool bAutomaticRule = false;
	if (JsonTryGetBool(Ctx.Params, TEXT("automatic_rule"), bAutomaticRule)
		&& TransitionNode->bAutomaticRuleBasedOnSequencePlayerInState != bAutomaticRule)
	{
		TransitionNode->Modify();
		TransitionNode->bAutomaticRuleBasedOnSequencePlayerInState = bAutomaticRule;
		bChanged = true;
	}

	bool bCompileAfterSet = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_set"), bCompileAfterSet);
	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bChanged && !UeAgentAnimBlueprintOps::FinalizeAnimBlueprintStructureMutation(AnimBlueprint, bCompileAfterSet, bSaveAfterSet, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), AnimBlueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("state_machine_name"), StateMachineGraph->GetName());
	OutData->SetStringField(TEXT("source_state_name"), TransitionNode->GetPreviousState() ? TransitionNode->GetPreviousState()->GetStateName() : TEXT(""));
	OutData->SetStringField(TEXT("target_state_name"), TransitionNode->GetNextState() ? TransitionNode->GetNextState()->GetStateName() : TEXT(""));
	OutData->SetStringField(TEXT("node_guid"), UeAgentAnimBlueprintOps::NodeGuidToString(TransitionNode));
	OutData->SetStringField(TEXT("bound_graph_path"), TransitionNode->GetBoundGraph() ? TransitionNode->GetBoundGraph()->GetPathName() : TEXT(""));
	OutData->SetNumberField(TEXT("node_pos_x"), TransitionNode->NodePosX);
	OutData->SetNumberField(TEXT("node_pos_y"), TransitionNode->NodePosY);
	OutData->SetBoolField(TEXT("bidirectional"), TransitionNode->Bidirectional);
	OutData->SetBoolField(TEXT("disabled"), TransitionNode->bDisabled);
	OutData->SetBoolField(TEXT("automatic_rule"), TransitionNode->bAutomaticRuleBasedOnSequencePlayerInState);
	OutData->SetNumberField(TEXT("crossfade_duration"), TransitionNode->CrossfadeDuration);
	OutData->SetNumberField(TEXT("priority_order"), TransitionNode->PriorityOrder);
	OutData->SetBoolField(TEXT("changed"), bChanged);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet && bChanged);
	return true;
}

bool FUeAgentHttpServer::CmdAnimBlueprintRemoveTransition(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString StateMachineName;
	if (!JsonTryGetString(Ctx.Params, TEXT("state_machine_name"), StateMachineName) || StateMachineName.IsEmpty())
	{
		OutError = TEXT("missing_state_machine_name");
		return false;
	}

	UAnimBlueprint* AnimBlueprint = UeAgentAnimBlueprintOps::LoadAnimBlueprint(AssetPathInput);
	if (!AnimBlueprint)
	{
		OutError = TEXT("anim_blueprint_not_found");
		return false;
	}

	UAnimGraphNode_StateMachineBase* StateMachineNode = UeAgentAnimBlueprintOps::FindStateMachineNode(AnimBlueprint, StateMachineName);
	UAnimationStateMachineGraph* StateMachineGraph = StateMachineNode ? StateMachineNode->EditorStateMachineGraph : nullptr;
	if (!StateMachineGraph)
	{
		OutError = TEXT("state_machine_not_found");
		return false;
	}

	UAnimStateTransitionNode* TransitionNode = nullptr;
	FString NodeGuidText;
	if (JsonTryGetString(Ctx.Params, TEXT("node_guid"), NodeGuidText) && !NodeGuidText.IsEmpty())
	{
		FGuid NodeGuid;
		if (!UeAgentAnimBlueprintOps::ParseNodeGuid(NodeGuidText, NodeGuid))
		{
			OutError = TEXT("invalid_node_guid");
			return false;
		}
		TransitionNode = UeAgentAnimBlueprintOps::FindTransitionNodeByGuid(StateMachineGraph, NodeGuid);
	}
	else
	{
		FString SourceStateName;
		FString TargetStateName;
		if (!JsonTryGetString(Ctx.Params, TEXT("source_state_name"), SourceStateName) || SourceStateName.IsEmpty())
		{
			OutError = TEXT("missing_source_state_name_or_node_guid");
			return false;
		}
		if (!JsonTryGetString(Ctx.Params, TEXT("target_state_name"), TargetStateName) || TargetStateName.IsEmpty())
		{
			OutError = TEXT("missing_target_state_name");
			return false;
		}

		TransitionNode = UeAgentAnimBlueprintOps::FindTransitionNode(StateMachineGraph, SourceStateName, TargetStateName);
	}

	if (!TransitionNode)
	{
		OutError = TEXT("transition_not_found");
		return false;
	}

	const FString SourceStateName = TransitionNode->GetPreviousState() ? TransitionNode->GetPreviousState()->GetStateName() : TEXT("");
	const FString TargetStateName = TransitionNode->GetNextState() ? TransitionNode->GetNextState()->GetStateName() : TEXT("");
	const FString TransitionNodeGuid = UeAgentAnimBlueprintOps::NodeGuidToString(TransitionNode);
	TransitionNode->DestroyNode();

	bool bCompileAfterRemove = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_remove"), bCompileAfterRemove);
	bool bSaveAfterRemove = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_remove"), bSaveAfterRemove);
	if (!UeAgentAnimBlueprintOps::FinalizeAnimBlueprintStructureMutation(AnimBlueprint, bCompileAfterRemove, bSaveAfterRemove, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), AnimBlueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("state_machine_name"), StateMachineGraph->GetName());
	OutData->SetStringField(TEXT("source_state_name"), SourceStateName);
	OutData->SetStringField(TEXT("target_state_name"), TargetStateName);
	OutData->SetStringField(TEXT("node_guid"), TransitionNodeGuid);
	OutData->SetBoolField(TEXT("changed"), true);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterRemove);
	return true;
}

bool FUeAgentHttpServer::CmdAnimBlueprintSetPreviewMesh(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UAnimBlueprint* AnimBlueprint = UeAgentAnimBlueprintOps::LoadAnimBlueprint(AssetPathInput);
	if (!AnimBlueprint)
	{
		OutError = TEXT("anim_blueprint_not_found");
		return false;
	}

	bool bClearPreviewMesh = false;
	JsonTryGetBool(Ctx.Params, TEXT("clear_preview_mesh"), bClearPreviewMesh);

	USkeletalMesh* PreviewMesh = nullptr;
	FString PreviewMeshPath;
	JsonTryGetString(Ctx.Params, TEXT("skeletal_mesh_path"), PreviewMeshPath);
	PreviewMeshPath.TrimStartAndEndInline();

	if (!bClearPreviewMesh)
	{
		if (PreviewMeshPath.IsEmpty())
		{
			OutError = TEXT("missing_skeletal_mesh_path");
			return false;
		}

		PreviewMesh = UeAgentAnimBlueprintOps::LoadSkeletalMesh(PreviewMeshPath);
		if (!PreviewMesh)
		{
			OutError = TEXT("skeletal_mesh_not_found");
			return false;
		}

		if (AnimBlueprint->TargetSkeleton && PreviewMesh->GetSkeleton() && !AnimBlueprint->TargetSkeleton->IsCompatibleForEditor(PreviewMesh->GetSkeleton()))
		{
			OutError = TEXT("preview_mesh_skeleton_incompatible");
			return false;
		}
	}

	AnimBlueprint->SetPreviewMesh(PreviewMesh, true);
	AnimBlueprint->MarkPackageDirty();

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentAnimBlueprintOps::SaveAnimBlueprintPackage(AnimBlueprint, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), AnimBlueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("preview_skeletal_mesh"), AnimBlueprint->GetPreviewMesh() ? AnimBlueprint->GetPreviewMesh()->GetPathName() : TEXT(""));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdAnimBlueprintSetPreviewAnimationBlueprint(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UAnimBlueprint* AnimBlueprint = UeAgentAnimBlueprintOps::LoadAnimBlueprint(AssetPathInput);
	if (!AnimBlueprint)
	{
		OutError = TEXT("anim_blueprint_not_found");
		return false;
	}

	bool bClearPreviewAnimationBlueprint = false;
	JsonTryGetBool(Ctx.Params, TEXT("clear_preview_animation_blueprint"), bClearPreviewAnimationBlueprint);

	UAnimBlueprint* PreviewAnimBlueprint = nullptr;
	FString PreviewAnimBlueprintPath;
	JsonTryGetString(Ctx.Params, TEXT("preview_anim_blueprint_path"), PreviewAnimBlueprintPath);
	PreviewAnimBlueprintPath.TrimStartAndEndInline();

	if (!bClearPreviewAnimationBlueprint)
	{
		if (PreviewAnimBlueprintPath.IsEmpty())
		{
			OutError = TEXT("missing_preview_anim_blueprint_path");
			return false;
		}

		PreviewAnimBlueprint = UeAgentAnimBlueprintOps::LoadAnimBlueprint(PreviewAnimBlueprintPath);
		if (!PreviewAnimBlueprint)
		{
			OutError = TEXT("preview_anim_blueprint_not_found");
			return false;
		}
		if (PreviewAnimBlueprint == AnimBlueprint)
		{
			OutError = TEXT("preview_anim_blueprint_self_reference_not_allowed");
			return false;
		}
		if (!AnimBlueprint->IsCompatible(PreviewAnimBlueprint))
		{
			OutError = TEXT("preview_anim_blueprint_incompatible");
			return false;
		}
	}

	AnimBlueprint->SetPreviewAnimationBlueprint(PreviewAnimBlueprint);

	FString PreviewApplicationMethodText;
	if (JsonTryGetString(Ctx.Params, TEXT("preview_application_method"), PreviewApplicationMethodText) && !PreviewApplicationMethodText.TrimStartAndEnd().IsEmpty())
	{
		EPreviewAnimationBlueprintApplicationMethod PreviewApplicationMethod = EPreviewAnimationBlueprintApplicationMethod::LinkedLayers;
		if (!UeAgentAnimBlueprintOps::ParsePreviewApplicationMethod(PreviewApplicationMethodText, PreviewApplicationMethod))
		{
			OutError = TEXT("invalid_preview_application_method");
			return false;
		}
		AnimBlueprint->SetPreviewAnimationBlueprintApplicationMethod(PreviewApplicationMethod);
	}

	FString PreviewTag;
	if (JsonTryGetString(Ctx.Params, TEXT("preview_animation_blueprint_tag"), PreviewTag))
	{
		AnimBlueprint->SetPreviewAnimationBlueprintTag(FName(*PreviewTag));
	}

	AnimBlueprint->MarkPackageDirty();

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentAnimBlueprintOps::SaveAnimBlueprintPackage(AnimBlueprint, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), AnimBlueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("preview_animation_blueprint"), AnimBlueprint->GetPreviewAnimationBlueprint() ? AnimBlueprint->GetPreviewAnimationBlueprint()->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("preview_application_method"), UeAgentAnimBlueprintOps::PreviewApplicationMethodToString(AnimBlueprint->GetPreviewAnimationBlueprintApplicationMethod()));
	OutData->SetStringField(TEXT("preview_animation_blueprint_tag"), AnimBlueprint->GetPreviewAnimationBlueprintTag().ToString());
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdAnimBlueprintReparent(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString ParentClassPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("parent_class"), ParentClassPath) || ParentClassPath.IsEmpty())
	{
		OutError = TEXT("missing_parent_class");
		return false;
	}

	UAnimBlueprint* AnimBlueprint = UeAgentAnimBlueprintOps::LoadAnimBlueprint(AssetPathInput);
	if (!AnimBlueprint)
	{
		OutError = TEXT("anim_blueprint_not_found");
		return false;
	}

	UClass* NewParentClass = UeAgentAnimBlueprintOps::ResolveClassByPath(ParentClassPath);
	if (!UeAgentAnimBlueprintOps::ResolveAnimBlueprintParentClass(NewParentClass, AnimBlueprint->TargetSkeleton, AnimBlueprint->bIsTemplate, OutError))
	{
		return false;
	}

	UBlueprintEditorLibrary::ReparentBlueprint(AnimBlueprint, NewParentClass);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBlueprint);

	bool bCompileAfterReparent = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_reparent"), bCompileAfterReparent);
	if (bCompileAfterReparent)
	{
		FKismetEditorUtilities::CompileBlueprint(AnimBlueprint);
	}

	bool bSaveAfterReparent = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_reparent"), bSaveAfterReparent);
	if (bSaveAfterReparent && !UeAgentAnimBlueprintOps::SaveAnimBlueprintPackage(AnimBlueprint, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), AnimBlueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("parent_class"), AnimBlueprint->ParentClass ? AnimBlueprint->ParentClass->GetPathName() : TEXT(""));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterReparent);
	return true;
}

#include "UeAgentHttpServer_AnimBlueprint_FolderFormat.inl"

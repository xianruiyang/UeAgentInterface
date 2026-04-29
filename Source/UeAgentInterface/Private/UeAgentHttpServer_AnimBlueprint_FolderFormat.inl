#include "AnimGraphNode_BlendSpaceBase.h"
#include "AnimGraphNode_BlendSpacePlayer.h"
#include "AnimGraphNode_ControlRig.h"
#include "AnimGraphNode_LinkedInputPose.h"
#include "AnimGraphNode_RotationOffsetBlendSpace.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_SequenceEvaluator.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_StateResult.h"
#include "AnimGraphNode_TransitionResult.h"
#include "AnimationStateGraph.h"
#include "AnimationTransitionGraph.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_Variable.h"
#include "K2Node_VariableSet.h"

namespace UeAgentAnimBlueprintFolderOps
{
	struct FDesiredStateNode
	{
		FString Id;
		FString NodeKind;
		FString StateName;
		FString StateType = TEXT("single_animation");
		int32 PosX = 0;
		int32 PosY = 0;
		bool bAlwaysResetOnEntry = false;
		bool bGlobalAlias = false;
		TArray<FString> AliasTargetStates;
		FString BoundGraphFile;
	};

	struct FDesiredTransition
	{
		FString Id;
		FString SourceNodeId;
		FString TargetNodeId;
		int32 PosX = 0;
		int32 PosY = 0;
		int32 PriorityOrder = 0;
		double CrossfadeDuration = 0.0;
		bool bBidirectional = false;
		bool bDisabled = false;
		bool bAutomaticRule = false;
		FString RuleGraphFile;
	};

	struct FDesiredStateMachine
	{
		FString StateMachineName;
		FString OwnerAnimGraphName = TEXT("AnimGraph");
		FString GraphPath;
		int32 PosX = 0;
		int32 PosY = 0;
		FString EntryNodeId;
		FString EntryStateName;
		TArray<FDesiredStateNode> Nodes;
		TArray<FDesiredTransition> Transitions;
	};

	struct FAnimGraphSpec
	{
		FString Name;
		FString GraphKind;
		FString GraphRef;
		FString FileRef;
		FString StateMachineName;
		FString StateName;
		FString SourceStateName;
		FString TargetStateName;
		TSharedPtr<FJsonObject> Root;
	};

	static FString GetFolderRootAbsolute()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("AnimBlueprint")));
	}

	static FString GetBlueprintProxyRootAbsolute()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("ActorBlueprint")));
	}

	static FString NormalizeAssetRelativeFolder(const FString& InAssetPath)
	{
		FString AssetPath = UeAgentAnimBlueprintOps::NormalizeAssetPath(InAssetPath);
		while (AssetPath.StartsWith(TEXT("/")))
		{
			AssetPath.RightChopInline(1, EAllowShrinking::No);
		}
		return AssetPath;
	}

	static bool ResolveFolderForAsset(const FString& InAssetPath, FString& OutFolderPath, FString& OutError)
	{
		const FString AssetPath = UeAgentAnimBlueprintOps::NormalizeAssetPath(InAssetPath);
		if (!FPackageName::IsValidLongPackageName(AssetPath))
		{
			OutError = TEXT("invalid_asset_path");
			return false;
		}

		OutFolderPath = FPaths::Combine(GetFolderRootAbsolute(), NormalizeAssetRelativeFolder(AssetPath));
		return true;
	}

	static FString MakeJsonString(const TSharedPtr<FJsonObject>& Obj)
	{
		FString JsonText;
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonText);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
		return JsonText;
	}

	static bool SaveJsonFile(const FString& FilePath, const TSharedPtr<FJsonObject>& Obj)
	{
		if (!Obj.IsValid())
		{
			return false;
		}

		IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), true);
		return FFileHelper::SaveStringToFile(
			MakeJsonString(Obj),
			*FilePath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	static bool LoadJsonFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutObj, FString& OutError)
	{
		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *FilePath))
		{
			OutError = TEXT("json_file_not_found");
			return false;
		}

		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(JsonText);
		if (!FJsonSerializer::Deserialize(Reader, OutObj) || !OutObj.IsValid())
		{
			OutError = TEXT("json_parse_failed");
			return false;
		}

		return true;
	}

	static bool LoadOptionalJsonFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutObj, FString& OutError)
	{
		OutObj.Reset();
		if (!FPaths::FileExists(FilePath))
		{
			return true;
		}

		FString LoadError;
		if (!LoadJsonFile(FilePath, OutObj, LoadError))
		{
			OutError = FString::Printf(TEXT("%s:%s"), *LoadError, *FilePath);
			return false;
		}
		return true;
	}

	static bool TryGetString(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, FString& OutValue)
	{
		return Obj.IsValid() && Obj->TryGetStringField(Key, OutValue);
	}

	static bool TryGetBool(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, bool& OutValue)
	{
		return Obj.IsValid() && Obj->TryGetBoolField(Key, OutValue);
	}

	static bool TryGetNumber(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, double& OutValue)
	{
		return Obj.IsValid() && Obj->TryGetNumberField(Key, OutValue);
	}

	static bool TryGetObjectField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, TSharedPtr<FJsonObject>& OutValue)
	{
		const TSharedPtr<FJsonObject>* FoundObject = nullptr;
		if (!Obj.IsValid() || !Obj->TryGetObjectField(Key, FoundObject) || !FoundObject || !FoundObject->IsValid())
		{
			return false;
		}
		OutValue = *FoundObject;
		return true;
	}

	static FProperty* FindPropertyByNameLoose(UStruct* StructType, const FString& Segment)
	{
		if (!StructType)
		{
			return nullptr;
		}

		const FString Normalized = Segment.Replace(TEXT(" "), TEXT(""));
		const FString NormalizedWithoutB = Normalized.StartsWith(TEXT("b")) ? Normalized.Mid(1) : Normalized;
		for (TFieldIterator<FProperty> It(StructType, EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			if (!Property)
			{
				continue;
			}

			const FString PropertyName = Property->GetName();
			const FString PropertyNameWithoutB = PropertyName.StartsWith(TEXT("b")) ? PropertyName.Mid(1) : PropertyName;
			if (PropertyName.Equals(Normalized, ESearchCase::IgnoreCase) ||
				PropertyNameWithoutB.Equals(NormalizedWithoutB, ESearchCase::IgnoreCase) ||
				Property->GetAuthoredName().Equals(Normalized, ESearchCase::IgnoreCase))
			{
				return Property;
			}
		}

		return nullptr;
	}

	static bool ResolvePropertyPath(UObject* Obj, const FString& PropertyPath, FProperty*& OutProperty, void*& OutValuePtr)
	{
		OutProperty = nullptr;
		OutValuePtr = nullptr;
		if (!Obj || PropertyPath.IsEmpty())
		{
			return false;
		}

		TArray<FString> Segments;
		PropertyPath.ParseIntoArray(Segments, TEXT("."), true);
		if (Segments.Num() == 0)
		{
			return false;
		}

		UStruct* CurrentStruct = Obj->GetClass();
		void* CurrentContainer = Obj;
		for (int32 Index = 0; Index < Segments.Num(); ++Index)
		{
			FProperty* Property = FindPropertyByNameLoose(CurrentStruct, Segments[Index]);
			if (!Property)
			{
				return false;
			}

			void* ValuePtr = Property->ContainerPtrToValuePtr<void>(CurrentContainer);
			if (Index == Segments.Num() - 1)
			{
				OutProperty = Property;
				OutValuePtr = ValuePtr;
				return true;
			}

			FStructProperty* StructProperty = CastField<FStructProperty>(Property);
			if (!StructProperty)
			{
				return false;
			}

			CurrentStruct = StructProperty->Struct;
			CurrentContainer = ValuePtr;
		}

		return false;
	}

	static bool GetObjectPropertyText(UObject* Target, const FString& PropertyPath, FString& OutValueText, FString& OutCppType)
	{
		OutValueText.Reset();
		OutCppType.Reset();

		FProperty* Property = nullptr;
		void* ValuePtr = nullptr;
		if (!ResolvePropertyPath(Target, PropertyPath, Property, ValuePtr) || !Property)
		{
			return false;
		}

		OutCppType = Property->GetCPPType();
		Property->ExportTextItem_Direct(OutValueText, ValuePtr, nullptr, Target, PPF_None);
		return true;
	}

	static bool SetObjectPropertyText(UObject* Target, const FString& PropertyPath, const FString& ValueText, FString* OutAppliedValueText = nullptr, FString* OutCppType = nullptr, FString* OutImportError = nullptr)
	{
		if (OutImportError)
		{
			OutImportError->Reset();
		}

		FProperty* Property = nullptr;
		void* ValuePtr = nullptr;
		if (!ResolvePropertyPath(Target, PropertyPath, Property, ValuePtr))
		{
			if (OutImportError)
			{
				*OutImportError = TEXT("property_not_found");
			}
			return false;
		}

		Target->Modify();
		if (OutCppType)
		{
			*OutCppType = Property ? Property->GetCPPType() : TEXT("");
		}
		if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			if (StructProperty->Struct == TBaseStructure<FGuid>::Get())
			{
				FString GuidText = ValueText.TrimStartAndEnd();
				if (GuidText.StartsWith(TEXT("\"")) && GuidText.EndsWith(TEXT("\"")) && GuidText.Len() >= 2)
				{
					GuidText = GuidText.Mid(1, GuidText.Len() - 2);
				}

				FGuid ParsedGuid;
				if (FGuid::Parse(GuidText, ParsedGuid))
				{
					*reinterpret_cast<FGuid*>(ValuePtr) = ParsedGuid;
					if (OutAppliedValueText)
					{
						Property->ExportTextItem_Direct(*OutAppliedValueText, ValuePtr, nullptr, Target, PPF_None);
					}
					return true;
				}
			}
		}

		if (!Property || Property->ImportText_Direct(*ValueText, ValuePtr, Target, PPF_None, GLog) == nullptr)
		{
			if (OutImportError)
			{
				*OutImportError = FString::Printf(TEXT("property_import_failed:%s:%s"), *PropertyPath, *ValueText);
			}
			return false;
		}
		if (OutAppliedValueText)
		{
			Property->ExportTextItem_Direct(*OutAppliedValueText, ValuePtr, nullptr, Target, PPF_None);
		}
		return true;
	}

	static bool CopyJsonFileIfExists(const FString& SourcePath, const FString& DestPath, bool& bCopied, FString& OutError)
	{
		bCopied = false;
		if (!IFileManager::Get().FileExists(*SourcePath))
		{
			return true;
		}

		TSharedPtr<FJsonObject> JsonObj;
		if (!LoadJsonFile(SourcePath, JsonObj, OutError))
		{
			return false;
		}

		if (!SaveJsonFile(DestPath, JsonObj))
		{
			OutError = TEXT("save_json_copy_failed");
			return false;
		}

		bCopied = true;
		return true;
	}

	static FString MakeSlug(const FString& InText)
	{
		FString Out = InText.TrimStartAndEnd().ToLower();
		for (int32 Index = 0; Index < Out.Len(); ++Index)
		{
			TCHAR& Ch = Out[Index];
			const bool bAlphaNum = (Ch >= TEXT('a') && Ch <= TEXT('z'))
				|| (Ch >= TEXT('0') && Ch <= TEXT('9'));
			if (!bAlphaNum)
			{
				Ch = TEXT('_');
			}
		}
		while (Out.Contains(TEXT("__")))
		{
			Out.ReplaceInline(TEXT("__"), TEXT("_"));
		}
		bool bIgnored = false;
		Out.TrimCharInline(TEXT('_'), &bIgnored);
		return Out.IsEmpty() ? TEXT("item") : Out;
	}

	static FString MakeLogicGraphFileName(const FString& GraphName, const FString& GraphKind)
	{
		if (GraphKind.Equals(TEXT("event_graph"), ESearchCase::IgnoreCase))
		{
			return TEXT("EventGraph.json");
		}

		const FString SafeName = FPaths::MakeValidFileName(GraphName, TEXT('_'));
		if (GraphKind.Equals(TEXT("function_graph"), ESearchCase::IgnoreCase))
		{
			return FString::Printf(TEXT("Function_%s.json"), *SafeName);
		}
		if (GraphKind.Equals(TEXT("macro_graph"), ESearchCase::IgnoreCase))
		{
			return FString::Printf(TEXT("Macro_%s.json"), *SafeName);
		}
		return FString::Printf(TEXT("%s.json"), *SafeName);
	}

	static FString MakeAnimLayerGraphFileName(const FString& LayerName)
	{
		return FString::Printf(TEXT("AnimLayer_%s.json"), *FPaths::MakeValidFileName(LayerName, TEXT('_')));
	}

	static FString MakeStateMachineFileName(const FString& StateMachineName)
	{
		return FString::Printf(TEXT("%s.json"), *FPaths::MakeValidFileName(StateMachineName, TEXT('_')));
	}

	static FString MakeStateGraphFileName(const FString& StateMachineName, const FString& StateName, const FString& NodeKind)
	{
		const FString Prefix = NodeKind.Equals(TEXT("conduit"), ESearchCase::IgnoreCase) ? TEXT("Conduit") : TEXT("State");
		return FString::Printf(
			TEXT("%s_%s_%s.json"),
			*Prefix,
			*FPaths::MakeValidFileName(StateMachineName, TEXT('_')),
			*FPaths::MakeValidFileName(StateName, TEXT('_')));
	}

	static FString MakeTransitionGraphFileName(const FString& StateMachineName, const FString& SourceStateName, const FString& TargetStateName)
	{
		return FString::Printf(
			TEXT("Transition_%s_%s_To_%s.json"),
			*FPaths::MakeValidFileName(StateMachineName, TEXT('_')),
			*FPaths::MakeValidFileName(SourceStateName, TEXT('_')),
			*FPaths::MakeValidFileName(TargetStateName, TEXT('_')));
	}

	static FString MakeStateNodeId(const FString& NodeKind, const FString& StateName)
	{
		return FString::Printf(TEXT("%s_%s"), *MakeSlug(NodeKind), *MakeSlug(StateName));
	}

	static FString MakeTransitionId(const FString& SourceStateName, const FString& TargetStateName)
	{
		return FString::Printf(TEXT("%s_to_%s"), *MakeSlug(SourceStateName), *MakeSlug(TargetStateName));
	}

	static bool IsBuiltinAnimGraphNode(const UEdGraphNode* Node)
	{
		return Node
			&& (Node->IsA<UAnimGraphNode_Root>()
				|| Node->IsA<UAnimGraphNode_StateResult>()
				|| Node->IsA<UAnimGraphNode_TransitionResult>()
				|| Node->IsA<UAnimGraphNode_LinkedInputPose>());
	}

	static FString GetBuiltinAnimGraphNodeId(const UEdGraphNode* Node)
	{
		if (Node->IsA<UAnimGraphNode_Root>())
		{
			return TEXT("__root__");
		}
		if (Node->IsA<UAnimGraphNode_StateResult>())
		{
			return TEXT("__state_result__");
		}
		if (Node->IsA<UAnimGraphNode_TransitionResult>())
		{
			return TEXT("__transition_result__");
		}
		if (const UAnimGraphNode_LinkedInputPose* LinkedInputPose = Cast<UAnimGraphNode_LinkedInputPose>(Node))
		{
			if (LinkedInputPose->InputPoseIndex >= 0)
			{
				return FString::Printf(TEXT("__linked_input_pose_%d__"), LinkedInputPose->InputPoseIndex);
			}
			return TEXT("__linked_input_pose__");
		}
		return FString();
	}

	static FString MakeBaseNodeId(UEdGraphNode* Node)
	{
		if (const UK2Node_CustomEvent* CustomEventNode = Cast<UK2Node_CustomEvent>(Node))
		{
			return FString::Printf(TEXT("custom_%s"), *MakeSlug(CustomEventNode->CustomFunctionName.ToString()));
		}
		if (const UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
		{
			return FString::Printf(TEXT("event_%s"), *MakeSlug(EventNode->EventReference.GetMemberName().ToString()));
		}
		if (const UK2Node_CallFunction* CallFunctionNode = Cast<UK2Node_CallFunction>(Node))
		{
			FString FunctionName = CallFunctionNode->FunctionReference.GetMemberName().ToString();
			if (const UFunction* Function = CallFunctionNode->GetTargetFunction())
			{
				FunctionName = Function->GetName();
			}
			return FString::Printf(TEXT("call_%s"), *MakeSlug(FunctionName));
		}
		if (const UK2Node_Variable* VariableNode = Cast<UK2Node_Variable>(Node))
		{
			const FString Access = Node->IsA<UK2Node_VariableSet>() ? TEXT("set") : TEXT("get");
			return FString::Printf(TEXT("%s_%s"), *Access, *MakeSlug(VariableNode->VariableReference.GetMemberName().ToString()));
		}
		return FString::Printf(TEXT("node_%s"), *MakeSlug(Node->GetClass()->GetName()));
	}

	static TArray<FString> GetAnimGraphNodePropertyPaths(UEdGraphNode* Node)
	{
		TArray<FString> PropertyPaths;
		if (!Node)
		{
			return PropertyPaths;
		}

		if (Node->IsA<UAnimGraphNode_BlendSpaceBase>())
		{
			PropertyPaths.Add(TEXT("Node.BlendSpace"));
		}

		if (Node->IsA<UAnimGraphNode_SequencePlayer>() || Node->IsA<UAnimGraphNode_SequenceEvaluator>())
		{
			PropertyPaths.Add(TEXT("Node.Sequence"));
		}

		if (Node->IsA<UAnimGraphNode_ControlRig>())
		{
			PropertyPaths.Add(TEXT("Node.ControlRigClass"));
			PropertyPaths.Add(TEXT("Node.DefaultControlRigClass"));
			PropertyPaths.Add(TEXT("Node.bExecute"));
			PropertyPaths.Add(TEXT("Node.InputSettings"));
			PropertyPaths.Add(TEXT("Node.InputSettings.bUpdatePose"));
			PropertyPaths.Add(TEXT("Node.InputSettings.bUpdateCurves"));
			PropertyPaths.Add(TEXT("Node.OutputSettings"));
			PropertyPaths.Add(TEXT("Node.OutputSettings.bUpdatePose"));
			PropertyPaths.Add(TEXT("Node.OutputSettings.bUpdateCurves"));
			PropertyPaths.Add(TEXT("Node.Alpha"));
			PropertyPaths.Add(TEXT("Node.AlphaInputType"));
			PropertyPaths.Add(TEXT("Node.bAlphaBoolEnabled"));
			PropertyPaths.Add(TEXT("Node.bSetRefPoseFromSkeleton"));
			PropertyPaths.Add(TEXT("Node.AlphaCurveName"));
			PropertyPaths.Add(TEXT("Node.LODThreshold"));
		}

		return PropertyPaths;
	}

	static UEdGraphNode* FindGraphNodeByGuidText(UEdGraph* Graph, const FString& NodeGuidText)
	{
		if (!Graph || NodeGuidText.IsEmpty())
		{
			return nullptr;
		}

		FGuid NodeGuid;
		if (!FGuid::Parse(NodeGuidText, NodeGuid))
		{
			return nullptr;
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->NodeGuid == NodeGuid)
			{
				return Node;
			}
		}

		return nullptr;
	}

	static UEdGraph* ResolveGraphByPathOrName(UAnimBlueprint* AnimBlueprint, const FString& InGraphRef)
	{
		if (!AnimBlueprint)
		{
			return nullptr;
		}

		FString GraphRef = InGraphRef;
		GraphRef.TrimStartAndEndInline();
		if (GraphRef.IsEmpty())
		{
			return nullptr;
		}

		TArray<UEdGraph*> AllGraphs;
		AnimBlueprint->GetAllGraphs(AllGraphs);
		for (UEdGraph* Graph : AllGraphs)
		{
			if (!Graph)
			{
				continue;
			}
			if (Graph->GetPathName().Equals(GraphRef, ESearchCase::IgnoreCase)
				|| Graph->GetName().Equals(GraphRef, ESearchCase::IgnoreCase))
			{
				return Graph;
			}
		}

		if (GraphRef.Equals(TEXT("EventGraph"), ESearchCase::IgnoreCase))
		{
			return UBlueprintEditorLibrary::FindEventGraph(AnimBlueprint);
		}

		return nullptr;
	}

	static TSharedPtr<FJsonObject> ExportGraphJson(UEdGraph* Graph, const FString& GraphKind)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> GraphObj = MakeShared<FJsonObject>();
		GraphObj->SetStringField(TEXT("name"), Graph ? Graph->GetName() : TEXT(""));
		GraphObj->SetStringField(TEXT("graph_kind"), GraphKind);
		GraphObj->SetStringField(TEXT("graph_path"), Graph ? Graph->GetPathName() : TEXT(""));
		Root->SetObjectField(TEXT("graph"), GraphObj);

		TMap<const UEdGraphNode*, FString> BuiltinIds;
		TMap<FString, int32> IdUseCount;
		TMap<const UEdGraphNode*, FString> ExportNodeIds;

		if (Graph)
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node)
				{
					continue;
				}
				if (IsBuiltinAnimGraphNode(Node))
				{
					const FString BuiltinId = GetBuiltinAnimGraphNodeId(Node);
					if (!BuiltinId.IsEmpty())
					{
						BuiltinIds.Add(Node, BuiltinId);
					}
					continue;
				}

				FString BaseId = MakeBaseNodeId(Node);
				int32& Count = IdUseCount.FindOrAdd(BaseId);
				++Count;
				if (Count > 1)
				{
					BaseId = FString::Printf(TEXT("%s_%d"), *BaseId, Count);
				}
				ExportNodeIds.Add(Node, BaseId);
			}
		}

		TArray<TSharedPtr<FJsonValue>> Nodes;
		if (Graph)
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node || IsBuiltinAnimGraphNode(Node))
				{
					continue;
				}

				TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
				NodeObj->SetStringField(TEXT("id"), ExportNodeIds.FindRef(Node));
				NodeObj->SetStringField(TEXT("ue_node_guid"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));

				TSharedPtr<FJsonObject> PosObj = MakeShared<FJsonObject>();
				PosObj->SetNumberField(TEXT("x"), Node->NodePosX);
				PosObj->SetNumberField(TEXT("y"), Node->NodePosY);
				NodeObj->SetObjectField(TEXT("pos"), PosObj);

				if (const UK2Node_CustomEvent* CustomEventNode = Cast<UK2Node_CustomEvent>(Node))
				{
					NodeObj->SetStringField(TEXT("node_kind"), TEXT("custom_event"));
					NodeObj->SetStringField(TEXT("event_name"), CustomEventNode->CustomFunctionName.ToString());
				}
				else if (const UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
				{
					NodeObj->SetStringField(TEXT("node_kind"), TEXT("event"));
					NodeObj->SetStringField(TEXT("event_name"), EventNode->EventReference.GetMemberName().ToString());
					if (UClass* EventClass = EventNode->EventReference.GetMemberParentClass(EventNode->GetBlueprintClassFromNode()))
					{
						NodeObj->SetStringField(TEXT("event_class"), EventClass->GetPathName());
					}
				}
				else if (const UK2Node_CallFunction* CallFunctionNode = Cast<UK2Node_CallFunction>(Node))
				{
					NodeObj->SetStringField(TEXT("node_kind"), TEXT("call_function"));
					if (UFunction* Function = CallFunctionNode->GetTargetFunction())
					{
						NodeObj->SetStringField(TEXT("function_name"), Function->GetName());
					}
					else
					{
						NodeObj->SetStringField(TEXT("function_name"), CallFunctionNode->FunctionReference.GetMemberName().ToString());
					}
					if (UClass* FunctionOwnerClass = CallFunctionNode->FunctionReference.GetMemberParentClass(CallFunctionNode->GetBlueprintClassFromNode()))
					{
						NodeObj->SetStringField(TEXT("function_owner_class"), FunctionOwnerClass->GetPathName());
					}
				}
				else if (const UK2Node_Variable* VariableNode = Cast<UK2Node_Variable>(Node))
				{
					NodeObj->SetStringField(TEXT("node_kind"), TEXT("variable_node"));
					NodeObj->SetStringField(TEXT("variable_name"), VariableNode->VariableReference.GetMemberName().ToString());
					NodeObj->SetStringField(TEXT("access"), Node->IsA<UK2Node_VariableSet>() ? TEXT("set") : TEXT("get"));
				}
				else
				{
					NodeObj->SetStringField(TEXT("node_kind"), TEXT("node_by_class"));
					NodeObj->SetStringField(TEXT("node_class"), Node->GetClass()->GetPathName());
					if (const UAnimGraphNode_StateMachineBase* StateMachineNode = Cast<UAnimGraphNode_StateMachineBase>(Node))
					{
						const FString StateMachineName =
							StateMachineNode->EditorStateMachineGraph
								? StateMachineNode->EditorStateMachineGraph->GetName()
								: FString();
						if (!StateMachineName.IsEmpty())
						{
							NodeObj->SetStringField(TEXT("state_machine_name"), StateMachineName);
						}
					}
				}

				TArray<TSharedPtr<FJsonValue>> Properties;
				for (const FString& PropertyPath : GetAnimGraphNodePropertyPaths(Node))
				{
					FString ValueText;
					FString CppType;
					if (!GetObjectPropertyText(Node, PropertyPath, ValueText, CppType))
					{
						continue;
					}

					TSharedPtr<FJsonObject> PropertyObj = MakeShared<FJsonObject>();
					PropertyObj->SetStringField(TEXT("property_name"), PropertyPath);
					PropertyObj->SetStringField(TEXT("value_text"), ValueText);
					if (!CppType.IsEmpty())
					{
						PropertyObj->SetStringField(TEXT("cpp_type"), CppType);
					}
					Properties.Add(MakeShared<FJsonValueObject>(PropertyObj));
				}
				if (Properties.Num() > 0)
				{
					NodeObj->SetArrayField(TEXT("properties"), Properties);
				}

				TArray<TSharedPtr<FJsonValue>> PinDefaults;
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (!Pin || Pin->Direction != EGPD_Input || Pin->DefaultValue.IsEmpty())
					{
						continue;
					}

					TSharedPtr<FJsonObject> PinDefaultObj = MakeShared<FJsonObject>();
					PinDefaultObj->SetStringField(TEXT("pin_name"), Pin->PinName.ToString());
					PinDefaultObj->SetStringField(TEXT("default_value"), Pin->DefaultValue);
					PinDefaults.Add(MakeShared<FJsonValueObject>(PinDefaultObj));
				}
				if (PinDefaults.Num() > 0)
				{
					NodeObj->SetArrayField(TEXT("pin_defaults"), PinDefaults);
				}

				Nodes.Add(MakeShared<FJsonValueObject>(NodeObj));
			}
		}
		Root->SetArrayField(TEXT("nodes"), Nodes);

		TMap<const UEdGraphNode*, FString> NodeIds = ExportNodeIds;
		for (const TPair<const UEdGraphNode*, FString>& Pair : BuiltinIds)
		{
			NodeIds.Add(Pair.Key, Pair.Value);
		}

		TSet<FString> SeenEdges;
		TArray<TSharedPtr<FJsonValue>> Edges;
		if (Graph)
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node)
				{
					continue;
				}

				const FString* FromNodeId = NodeIds.Find(Node);
				if (!FromNodeId)
				{
					continue;
				}

				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (!Pin || Pin->Direction != EGPD_Output)
					{
						continue;
					}
					for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
					{
						if (!LinkedPin || !LinkedPin->GetOwningNode())
						{
							continue;
						}

						const FString* ToNodeId = NodeIds.Find(LinkedPin->GetOwningNode());
						if (!ToNodeId)
						{
							continue;
						}

						const FString EdgeKey = FString::Printf(TEXT("%s|%s|%s|%s"), **FromNodeId, *Pin->PinName.ToString(), **ToNodeId, *LinkedPin->PinName.ToString());
						if (SeenEdges.Contains(EdgeKey))
						{
							continue;
						}
						SeenEdges.Add(EdgeKey);

						TSharedPtr<FJsonObject> EdgeObj = MakeShared<FJsonObject>();
						TSharedPtr<FJsonObject> FromObj = MakeShared<FJsonObject>();
						FromObj->SetStringField(TEXT("node"), *FromNodeId);
						FromObj->SetStringField(TEXT("pin"), Pin->PinName.ToString());
						TSharedPtr<FJsonObject> ToObj = MakeShared<FJsonObject>();
						ToObj->SetStringField(TEXT("node"), *ToNodeId);
						ToObj->SetStringField(TEXT("pin"), LinkedPin->PinName.ToString());
						EdgeObj->SetObjectField(TEXT("from"), FromObj);
						EdgeObj->SetObjectField(TEXT("to"), ToObj);
						Edges.Add(MakeShared<FJsonValueObject>(EdgeObj));
					}
				}
			}
		}
		Root->SetArrayField(TEXT("edges"), Edges);
		return Root;
	}

	static void ConvertGraphRootToAnimStyle(const TSharedPtr<FJsonObject>& GraphRoot)
	{
		if (!GraphRoot.IsValid())
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* NodesArray = nullptr;
		if (!GraphRoot->TryGetArrayField(TEXT("nodes"), NodesArray) || !NodesArray)
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& NodeValue : *NodesArray)
		{
			const TSharedPtr<FJsonObject>* NodeObjPtr = nullptr;
			if (!NodeValue.IsValid() || !NodeValue->TryGetObject(NodeObjPtr) || !NodeObjPtr || !NodeObjPtr->IsValid())
			{
				continue;
			}

			FString NodeType;
			if (TryGetString(*NodeObjPtr, TEXT("node_type"), NodeType) && !(*NodeObjPtr)->HasField(TEXT("node_kind")))
			{
				(*NodeObjPtr)->SetStringField(TEXT("node_kind"), NodeType);
			}
			(*NodeObjPtr)->RemoveField(TEXT("node_type"));
		}
	}

	static void ConvertGraphRootToBlueprintProxyStyle(const TSharedPtr<FJsonObject>& GraphRoot)
	{
		if (!GraphRoot.IsValid())
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* NodesArray = nullptr;
		if (!GraphRoot->TryGetArrayField(TEXT("nodes"), NodesArray) || !NodesArray)
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& NodeValue : *NodesArray)
		{
			const TSharedPtr<FJsonObject>* NodeObjPtr = nullptr;
			if (!NodeValue.IsValid() || !NodeValue->TryGetObject(NodeObjPtr) || !NodeObjPtr || !NodeObjPtr->IsValid())
			{
				continue;
			}

			FString NodeKind;
			if (TryGetString(*NodeObjPtr, TEXT("node_kind"), NodeKind) && !(*NodeObjPtr)->HasField(TEXT("node_type")))
			{
				(*NodeObjPtr)->SetStringField(TEXT("node_type"), NodeKind);
			}
			(*NodeObjPtr)->RemoveField(TEXT("node_kind"));
		}
	}

	static FString ResolveRelativePath(const FString& FolderPath, const FString& RelativePath)
	{
		if (RelativePath.IsEmpty())
		{
			return FString();
		}
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FolderPath, RelativePath));
	}

	static int32 GetNodeKindPriority(const FString& NodeKind);

	static bool ExtractDesiredLocalLayers(const TSharedPtr<FJsonObject>& AnimLayersJson, TSet<FString>& OutDesiredLocalLayers, bool& OutHasRootAnimGraphSpec)
	{
		OutDesiredLocalLayers.Reset();
		OutHasRootAnimGraphSpec = false;
		if (!AnimLayersJson.IsValid())
		{
			return true;
		}

		const TArray<TSharedPtr<FJsonValue>>* DesiredLayersArray = nullptr;
		if (!AnimLayersJson->TryGetArrayField(TEXT("anim_layers"), DesiredLayersArray) || !DesiredLayersArray)
		{
			return true;
		}

		for (const TSharedPtr<FJsonValue>& LayerValue : *DesiredLayersArray)
		{
			const TSharedPtr<FJsonObject>* LayerObjPtr = nullptr;
			if (!LayerValue.IsValid() || !LayerValue->TryGetObject(LayerObjPtr) || !LayerObjPtr || !LayerObjPtr->IsValid())
			{
				continue;
			}

			FString LayerName;
			FString LayerKind;
			TryGetString(*LayerObjPtr, TEXT("layer_name"), LayerName);
			TryGetString(*LayerObjPtr, TEXT("layer_kind"), LayerKind);
			if (LayerName.IsEmpty())
			{
				continue;
			}

			if (LayerKind.Equals(TEXT("root_anim_graph"), ESearchCase::IgnoreCase))
			{
				OutHasRootAnimGraphSpec = true;
				continue;
			}
			if (LayerKind.Equals(TEXT("local_anim_layer"), ESearchCase::IgnoreCase))
			{
				OutDesiredLocalLayers.Add(LayerName);
			}
		}

		return true;
	}

	static bool ParseDesiredStateMachinesFromFolder(const FString& FolderPath, TArray<FDesiredStateMachine>& OutStateMachines, FString& OutError)
	{
		OutStateMachines.Reset();

		const FString StateMachinesDir = FPaths::Combine(FolderPath, TEXT("state_machines"));
		TArray<FString> StateMachineFiles;
		IFileManager::Get().FindFiles(StateMachineFiles, *(FPaths::Combine(StateMachinesDir, TEXT("*.json"))), true, false);
		StateMachineFiles.Sort();
		for (const FString& StateMachineFile : StateMachineFiles)
		{
			TSharedPtr<FJsonObject> StateMachineJson;
			if (!LoadJsonFile(FPaths::Combine(StateMachinesDir, StateMachineFile), StateMachineJson, OutError))
			{
				return false;
			}

			FDesiredStateMachine MachineSpec;
			if (!TryGetString(StateMachineJson, TEXT("state_machine_name"), MachineSpec.StateMachineName) || MachineSpec.StateMachineName.IsEmpty())
			{
				OutError = TEXT("missing_state_machine_name");
				return false;
			}
			TryGetString(StateMachineJson, TEXT("owner_anim_graph_name"), MachineSpec.OwnerAnimGraphName);
			TryGetString(StateMachineJson, TEXT("graph_path"), MachineSpec.GraphPath);
			{
				TSharedPtr<FJsonObject> PosObj;
				if (TryGetObjectField(StateMachineJson, TEXT("pos"), PosObj))
				{
					double PosX = 0.0;
					double PosY = 0.0;
					TryGetNumber(PosObj, TEXT("x"), PosX);
					TryGetNumber(PosObj, TEXT("y"), PosY);
					MachineSpec.PosX = static_cast<int32>(PosX);
					MachineSpec.PosY = static_cast<int32>(PosY);
				}
			}

			TMap<FString, FString> NodeIdToStateName;
			const TArray<TSharedPtr<FJsonValue>>* NodesArray = nullptr;
			if (StateMachineJson->TryGetArrayField(TEXT("nodes"), NodesArray) && NodesArray)
			{
				for (const TSharedPtr<FJsonValue>& NodeValue : *NodesArray)
				{
					const TSharedPtr<FJsonObject>* NodeObjPtr = nullptr;
					if (!NodeValue.IsValid() || !NodeValue->TryGetObject(NodeObjPtr) || !NodeObjPtr || !NodeObjPtr->IsValid())
					{
						continue;
					}

					FDesiredStateNode NodeSpec;
					if (!TryGetString(*NodeObjPtr, TEXT("id"), NodeSpec.Id) || NodeSpec.Id.IsEmpty())
					{
						OutError = TEXT("missing_state_node_id");
						return false;
					}
					if (!TryGetString(*NodeObjPtr, TEXT("node_kind"), NodeSpec.NodeKind) || NodeSpec.NodeKind.IsEmpty())
					{
						OutError = TEXT("missing_state_node_kind");
						return false;
					}
					if (NodeSpec.NodeKind.Equals(TEXT("state_node"), ESearchCase::IgnoreCase))
					{
						// Backward compatibility for older exports that serialized transition tunnel placeholders.
						continue;
					}
					if (!TryGetString(*NodeObjPtr, TEXT("state_name"), NodeSpec.StateName) || NodeSpec.StateName.IsEmpty())
					{
						OutError = TEXT("missing_state_node_name");
						return false;
					}

					NodeIdToStateName.Add(NodeSpec.Id, NodeSpec.StateName);

					TSharedPtr<FJsonObject> PosObj;
					if (TryGetObjectField(*NodeObjPtr, TEXT("pos"), PosObj))
					{
						double PosX = 0.0;
						double PosY = 0.0;
						TryGetNumber(PosObj, TEXT("x"), PosX);
						TryGetNumber(PosObj, TEXT("y"), PosY);
						NodeSpec.PosX = static_cast<int32>(PosX);
						NodeSpec.PosY = static_cast<int32>(PosY);
					}

					TryGetString(*NodeObjPtr, TEXT("state_type"), NodeSpec.StateType);
					TryGetBool(*NodeObjPtr, TEXT("always_reset_on_entry"), NodeSpec.bAlwaysResetOnEntry);
					TryGetBool(*NodeObjPtr, TEXT("global_alias"), NodeSpec.bGlobalAlias);
					TryGetString(*NodeObjPtr, TEXT("bound_graph_file"), NodeSpec.BoundGraphFile);

					const TArray<TSharedPtr<FJsonValue>>* AliasTargets = nullptr;
					if ((*NodeObjPtr)->TryGetArrayField(TEXT("alias_target_states"), AliasTargets) && AliasTargets)
					{
						for (const TSharedPtr<FJsonValue>& AliasTargetValue : *AliasTargets)
						{
							const FString AliasTarget = AliasTargetValue.IsValid() ? AliasTargetValue->AsString() : FString();
							if (!AliasTarget.IsEmpty())
							{
								NodeSpec.AliasTargetStates.Add(AliasTarget);
							}
						}
					}

					MachineSpec.Nodes.Add(NodeSpec);
				}
			}

			MachineSpec.Nodes.Sort([](const FDesiredStateNode& A, const FDesiredStateNode& B)
			{
				if (GetNodeKindPriority(A.NodeKind) != GetNodeKindPriority(B.NodeKind))
				{
					return GetNodeKindPriority(A.NodeKind) < GetNodeKindPriority(B.NodeKind);
				}
				return A.StateName < B.StateName;
			});

			const TArray<TSharedPtr<FJsonValue>>* TransitionsArray = nullptr;
			if (StateMachineJson->TryGetArrayField(TEXT("transitions"), TransitionsArray) && TransitionsArray)
			{
				for (const TSharedPtr<FJsonValue>& TransitionValue : *TransitionsArray)
				{
					const TSharedPtr<FJsonObject>* TransitionObjPtr = nullptr;
					if (!TransitionValue.IsValid() || !TransitionValue->TryGetObject(TransitionObjPtr) || !TransitionObjPtr || !TransitionObjPtr->IsValid())
					{
						continue;
					}

					FDesiredTransition TransitionSpec;
					TryGetString(*TransitionObjPtr, TEXT("id"), TransitionSpec.Id);
					if (!TryGetString(*TransitionObjPtr, TEXT("source"), TransitionSpec.SourceNodeId) || TransitionSpec.SourceNodeId.IsEmpty())
					{
						OutError = TEXT("missing_transition_source");
						return false;
					}
					if (!TryGetString(*TransitionObjPtr, TEXT("target"), TransitionSpec.TargetNodeId) || TransitionSpec.TargetNodeId.IsEmpty())
					{
						OutError = TEXT("missing_transition_target");
						return false;
					}

					TSharedPtr<FJsonObject> PosObj;
					if (TryGetObjectField(*TransitionObjPtr, TEXT("pos"), PosObj))
					{
						double PosX = 0.0;
						double PosY = 0.0;
						TryGetNumber(PosObj, TEXT("x"), PosX);
						TryGetNumber(PosObj, TEXT("y"), PosY);
						TransitionSpec.PosX = static_cast<int32>(PosX);
						TransitionSpec.PosY = static_cast<int32>(PosY);
					}

					double PriorityOrder = 0.0;
					double CrossfadeDuration = 0.0;
					TryGetNumber(*TransitionObjPtr, TEXT("priority_order"), PriorityOrder);
					TryGetNumber(*TransitionObjPtr, TEXT("crossfade_duration"), CrossfadeDuration);
					TransitionSpec.PriorityOrder = FMath::RoundToInt(PriorityOrder);
					TransitionSpec.CrossfadeDuration = CrossfadeDuration;
					TryGetBool(*TransitionObjPtr, TEXT("bidirectional"), TransitionSpec.bBidirectional);
					TryGetBool(*TransitionObjPtr, TEXT("disabled"), TransitionSpec.bDisabled);
					TryGetBool(*TransitionObjPtr, TEXT("automatic_rule"), TransitionSpec.bAutomaticRule);
					TryGetString(*TransitionObjPtr, TEXT("rule_graph_file"), TransitionSpec.RuleGraphFile);
					MachineSpec.Transitions.Add(TransitionSpec);
				}
			}

			TSharedPtr<FJsonObject> EntryTargetObj;
			if (TryGetObjectField(StateMachineJson, TEXT("entry_target"), EntryTargetObj))
			{
				TryGetString(EntryTargetObj, TEXT("node_id"), MachineSpec.EntryNodeId);
				TryGetString(EntryTargetObj, TEXT("state_name"), MachineSpec.EntryStateName);
				if (MachineSpec.EntryStateName.IsEmpty() && !MachineSpec.EntryNodeId.IsEmpty())
				{
					MachineSpec.EntryStateName = NodeIdToStateName.FindRef(MachineSpec.EntryNodeId);
				}
			}

			OutStateMachines.Add(MachineSpec);
		}

		return true;
	}

	static void CollectReferencedGraphFiles(const TSharedPtr<FJsonObject>& AnimLayersJson, const TArray<FDesiredStateMachine>& DesiredStateMachines, TSet<FString>& OutReferencedGraphFiles)
	{
		OutReferencedGraphFiles.Reset();

		if (AnimLayersJson.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* LayersArray = nullptr;
			if (AnimLayersJson->TryGetArrayField(TEXT("anim_layers"), LayersArray) && LayersArray)
			{
				for (const TSharedPtr<FJsonValue>& LayerValue : *LayersArray)
				{
					const TSharedPtr<FJsonObject>* LayerObjPtr = nullptr;
					if (!LayerValue.IsValid() || !LayerValue->TryGetObject(LayerObjPtr) || !LayerObjPtr || !LayerObjPtr->IsValid())
					{
						continue;
					}

					FString GraphFile;
					FString LayerKind;
					TryGetString(*LayerObjPtr, TEXT("graph_file"), GraphFile);
					TryGetString(*LayerObjPtr, TEXT("layer_kind"), LayerKind);
					if (!GraphFile.IsEmpty() && (LayerKind.Equals(TEXT("local_anim_layer"), ESearchCase::IgnoreCase)
						|| LayerKind.Equals(TEXT("root_anim_graph"), ESearchCase::IgnoreCase)
						|| LayerKind.Equals(TEXT("interface_anim_layer"), ESearchCase::IgnoreCase)))
					{
						OutReferencedGraphFiles.Add(GraphFile);
					}
				}
			}
		}

		for (const FDesiredStateMachine& MachineSpec : DesiredStateMachines)
		{
			for (const FDesiredStateNode& NodeSpec : MachineSpec.Nodes)
			{
				if (!NodeSpec.BoundGraphFile.IsEmpty())
				{
					OutReferencedGraphFiles.Add(NodeSpec.BoundGraphFile);
				}
			}

			for (const FDesiredTransition& TransitionSpec : MachineSpec.Transitions)
			{
				if (!TransitionSpec.RuleGraphFile.IsEmpty())
				{
					OutReferencedGraphFiles.Add(TransitionSpec.RuleGraphFile);
				}
			}
		}
	}

	static bool LoadAnimGraphSpecsFromFolder(const FString& FolderPath, const TSet<FString>& ReferencedGraphFiles, const TArray<FDesiredStateMachine>& DesiredStateMachines, TArray<FAnimGraphSpec>& OutGraphSpecs, FString& OutError)
	{
		OutGraphSpecs.Reset();

		TArray<FString> SortedReferencedGraphFiles;
		for (const FString& GraphFileRef : ReferencedGraphFiles)
		{
			SortedReferencedGraphFiles.Add(GraphFileRef);
		}
		SortedReferencedGraphFiles.Sort();

		for (const FString& GraphFileRef : SortedReferencedGraphFiles)
		{
			TSharedPtr<FJsonObject> GraphRoot;
			if (!LoadJsonFile(ResolveRelativePath(FolderPath, GraphFileRef), GraphRoot, OutError))
			{
				return false;
			}

			TSharedPtr<FJsonObject> GraphObj;
			if (!TryGetObjectField(GraphRoot, TEXT("graph"), GraphObj))
			{
				OutError = TEXT("missing_graph_header");
				return false;
			}

			FAnimGraphSpec GraphSpec;
			GraphSpec.FileRef = GraphFileRef;
			if (!TryGetString(GraphObj, TEXT("name"), GraphSpec.Name) || GraphSpec.Name.IsEmpty())
			{
				OutError = TEXT("missing_graph_name");
				return false;
			}
			if (!TryGetString(GraphObj, TEXT("graph_kind"), GraphSpec.GraphKind) || GraphSpec.GraphKind.IsEmpty())
			{
				OutError = TEXT("missing_graph_kind");
				return false;
			}
			TryGetString(GraphObj, TEXT("graph_path"), GraphSpec.GraphRef);
			if (GraphSpec.GraphRef.IsEmpty())
			{
				GraphSpec.GraphRef = GraphSpec.Name;
			}

			for (const FDesiredStateMachine& MachineSpec : DesiredStateMachines)
			{
				TMap<FString, FString> NodeIdToStateName;
				for (const FDesiredStateNode& NodeSpec : MachineSpec.Nodes)
				{
					if (!NodeSpec.Id.IsEmpty() && !NodeSpec.StateName.IsEmpty())
					{
						NodeIdToStateName.Add(NodeSpec.Id, NodeSpec.StateName);
					}

					if (!NodeSpec.BoundGraphFile.IsEmpty() && NodeSpec.BoundGraphFile.Equals(GraphFileRef, ESearchCase::IgnoreCase))
					{
						GraphSpec.StateMachineName = MachineSpec.StateMachineName;
						GraphSpec.StateName = NodeSpec.StateName;
					}
				}

				for (const FDesiredTransition& TransitionSpec : MachineSpec.Transitions)
				{
					if (!TransitionSpec.RuleGraphFile.IsEmpty() && TransitionSpec.RuleGraphFile.Equals(GraphFileRef, ESearchCase::IgnoreCase))
					{
						GraphSpec.StateMachineName = MachineSpec.StateMachineName;
						GraphSpec.SourceStateName = NodeIdToStateName.FindRef(TransitionSpec.SourceNodeId);
						GraphSpec.TargetStateName = NodeIdToStateName.FindRef(TransitionSpec.TargetNodeId);
						break;
					}
				}
			}

			GraphSpec.Root = GraphRoot;
			OutGraphSpecs.Add(GraphSpec);
		}

		return true;
	}

	static int32 GetNodeKindPriority(const FString& NodeKind)
	{
		if (NodeKind.Equals(TEXT("state"), ESearchCase::IgnoreCase))
		{
			return 0;
		}
		if (NodeKind.Equals(TEXT("conduit"), ESearchCase::IgnoreCase))
		{
			return 1;
		}
		if (NodeKind.Equals(TEXT("alias"), ESearchCase::IgnoreCase))
		{
			return 2;
		}
		return 3;
	}

	static bool SaveBlueprintProxyFolder(
		const FString& AssetPath,
		const FString& AnimFolderPath,
		const TSharedPtr<FJsonObject>& SettingsJson,
		const bool bIncludeMembers,
		const bool bIncludeLogicGraphs,
		FString& OutProxyFolderPath,
		FString& OutError)
	{
		OutProxyFolderPath = FPaths::Combine(GetBlueprintProxyRootAbsolute(), NormalizeAssetRelativeFolder(AssetPath));
		if (IFileManager::Get().DirectoryExists(*OutProxyFolderPath))
		{
			IFileManager::Get().DeleteDirectory(*OutProxyFolderPath, false, true);
		}

		const FString MembersDir = FPaths::Combine(OutProxyFolderPath, TEXT("members"));
		const FString ComponentsDir = FPaths::Combine(OutProxyFolderPath, TEXT("components"));
		const FString GraphsDir = FPaths::Combine(OutProxyFolderPath, TEXT("graphs"));
		IFileManager::Get().MakeDirectory(*MembersDir, true);
		IFileManager::Get().MakeDirectory(*ComponentsDir, true);
		IFileManager::Get().MakeDirectory(*GraphsDir, true);

		FString ParentClassPath = TEXT("/Script/Engine.AnimInstance");
		TryGetString(SettingsJson, TEXT("parent_class"), ParentClassPath);

		TSharedPtr<FJsonObject> AssetJson = MakeShared<FJsonObject>();
		AssetJson->SetNumberField(TEXT("format_version"), 1);
		AssetJson->SetStringField(TEXT("asset_kind"), TEXT("actor_blueprint"));
		AssetJson->SetStringField(TEXT("asset_path"), AssetPath);
		AssetJson->SetStringField(TEXT("asset_name"), FPackageName::GetLongPackageAssetName(AssetPath));
		AssetJson->SetStringField(TEXT("parent_class"), ParentClassPath);
		if (!SaveJsonFile(FPaths::Combine(OutProxyFolderPath, TEXT("asset.json")), AssetJson))
		{
			OutError = TEXT("save_proxy_asset_json_failed");
			return false;
		}

		TSharedPtr<FJsonObject> ComponentsJson = MakeShared<FJsonObject>();
		ComponentsJson->SetArrayField(TEXT("components"), {});
		if (!SaveJsonFile(FPaths::Combine(ComponentsDir, TEXT("tree.json")), ComponentsJson))
		{
			OutError = TEXT("save_proxy_components_json_failed");
			return false;
		}

		if (bIncludeMembers)
		{
			bool bCopied = false;
			if (!CopyJsonFileIfExists(FPaths::Combine(AnimFolderPath, TEXT("members"), TEXT("variables.json")), FPaths::Combine(MembersDir, TEXT("variables.json")), bCopied, OutError))
			{
				return false;
			}
			if (!CopyJsonFileIfExists(FPaths::Combine(AnimFolderPath, TEXT("members"), TEXT("delegates.json")), FPaths::Combine(MembersDir, TEXT("delegates.json")), bCopied, OutError))
			{
				return false;
			}
			if (!CopyJsonFileIfExists(FPaths::Combine(AnimFolderPath, TEXT("members"), TEXT("defaults.json")), FPaths::Combine(MembersDir, TEXT("defaults.json")), bCopied, OutError))
			{
				return false;
			}
		}

		if (bIncludeLogicGraphs)
		{
			const FString AnimGraphsDir = FPaths::Combine(AnimFolderPath, TEXT("graphs"));
			TArray<FString> GraphFiles;
			IFileManager::Get().FindFiles(GraphFiles, *(FPaths::Combine(AnimGraphsDir, TEXT("*.json"))), true, false);
			GraphFiles.Sort();
			for (const FString& GraphFile : GraphFiles)
			{
				TSharedPtr<FJsonObject> GraphRoot;
				if (!LoadJsonFile(FPaths::Combine(AnimGraphsDir, GraphFile), GraphRoot, OutError))
				{
					return false;
				}

				TSharedPtr<FJsonObject> GraphObj;
				if (!TryGetObjectField(GraphRoot, TEXT("graph"), GraphObj))
				{
					OutError = TEXT("missing_graph_header");
					return false;
				}

				FString GraphKind;
				if (!TryGetString(GraphObj, TEXT("graph_kind"), GraphKind) || GraphKind.IsEmpty())
				{
					OutError = TEXT("missing_graph_kind");
					return false;
				}

				const bool bIsLogicGraph =
					GraphKind.Equals(TEXT("event_graph"), ESearchCase::IgnoreCase)
					|| GraphKind.Equals(TEXT("function_graph"), ESearchCase::IgnoreCase)
					|| GraphKind.Equals(TEXT("macro_graph"), ESearchCase::IgnoreCase);
				if (!bIsLogicGraph)
				{
					continue;
				}

				ConvertGraphRootToBlueprintProxyStyle(GraphRoot);
				if (!SaveJsonFile(FPaths::Combine(GraphsDir, GraphFile), GraphRoot))
				{
					OutError = TEXT("save_proxy_graph_failed");
					return false;
				}
			}
		}

		return true;
	}

	static TSharedPtr<FJsonObject> ExportValidationJson(
		const TArray<FString>& RequiredLayerNames,
		const TArray<FString>& RequiredStateMachineNames,
		const TMap<FString, TArray<FString>>& RequiredStatesByMachine)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Checks;

		TSharedPtr<FJsonObject> CompileCheck = MakeShared<FJsonObject>();
		CompileCheck->SetStringField(TEXT("kind"), TEXT("compile_success"));
		Checks.Add(MakeShared<FJsonValueObject>(CompileCheck));

		if (RequiredLayerNames.Num() > 0)
		{
			TSharedPtr<FJsonObject> LayersCheck = MakeShared<FJsonObject>();
			LayersCheck->SetStringField(TEXT("kind"), TEXT("required_anim_layers"));
			LayersCheck->SetArrayField(TEXT("layers"), UeAgentAnimBlueprintOps::MakeStringArray(RequiredLayerNames));
			Checks.Add(MakeShared<FJsonValueObject>(LayersCheck));
		}

		if (RequiredStateMachineNames.Num() > 0)
		{
			TSharedPtr<FJsonObject> MachinesCheck = MakeShared<FJsonObject>();
			MachinesCheck->SetStringField(TEXT("kind"), TEXT("required_state_machines"));
			MachinesCheck->SetArrayField(TEXT("state_machines"), UeAgentAnimBlueprintOps::MakeStringArray(RequiredStateMachineNames));
			Checks.Add(MakeShared<FJsonValueObject>(MachinesCheck));
		}

		for (const TPair<FString, TArray<FString>>& Pair : RequiredStatesByMachine)
		{
			if (Pair.Value.Num() == 0)
			{
				continue;
			}

			TSharedPtr<FJsonObject> StatesCheck = MakeShared<FJsonObject>();
			StatesCheck->SetStringField(TEXT("kind"), TEXT("required_states"));
			StatesCheck->SetStringField(TEXT("state_machine"), Pair.Key);
			StatesCheck->SetArrayField(TEXT("states"), UeAgentAnimBlueprintOps::MakeStringArray(Pair.Value));
			Checks.Add(MakeShared<FJsonValueObject>(StatesCheck));
		}

		Root->SetArrayField(TEXT("checks"), Checks);
		return Root;
	}
}

bool FUeAgentHttpServer::CmdAnimBlueprintExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

	const FString AssetPath = UeAgentAnimBlueprintOps::NormalizeAssetPath(AssetPathInput);
	FString FolderPath;
	if (!UeAgentAnimBlueprintFolderOps::ResolveFolderForAsset(AssetPath, FolderPath, OutError))
	{
		return false;
	}

	bool bCleanOutputDir = true;
	JsonTryGetBool(Ctx.Params, TEXT("clean_output_dir"), bCleanOutputDir);
	bool bIncludeValidation = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_validation"), bIncludeValidation);

	if (bCleanOutputDir && IFileManager::Get().DirectoryExists(*FolderPath))
	{
		IFileManager::Get().DeleteDirectory(*FolderPath, false, true);
	}

	const FString SettingsDir = FPaths::Combine(FolderPath, TEXT("settings"));
	const FString MembersDir = FPaths::Combine(FolderPath, TEXT("members"));
	const FString LayerInterfacesDir = FPaths::Combine(FolderPath, TEXT("layer_interfaces"));
	const FString AnimLayersDir = FPaths::Combine(FolderPath, TEXT("anim_layers"));
	const FString StateMachinesDir = FPaths::Combine(FolderPath, TEXT("state_machines"));
	const FString GraphsDir = FPaths::Combine(FolderPath, TEXT("graphs"));
	const FString ValidationDir = FPaths::Combine(FolderPath, TEXT("validation"));
	IFileManager::Get().MakeDirectory(*SettingsDir, true);
	IFileManager::Get().MakeDirectory(*MembersDir, true);
	IFileManager::Get().MakeDirectory(*LayerInterfacesDir, true);
	IFileManager::Get().MakeDirectory(*AnimLayersDir, true);
	IFileManager::Get().MakeDirectory(*StateMachinesDir, true);
	IFileManager::Get().MakeDirectory(*GraphsDir, true);
	if (bIncludeValidation)
	{
		IFileManager::Get().MakeDirectory(*ValidationDir, true);
	}

	int32 FileCount = 0;
	auto SaveRequiredJson = [&FileCount, &OutError](const FString& FilePath, const TSharedPtr<FJsonObject>& JsonObj) -> bool
	{
		if (!UeAgentAnimBlueprintFolderOps::SaveJsonFile(FilePath, JsonObj))
		{
			OutError = TEXT("save_export_file_failed");
			return false;
		}
		++FileCount;
		return true;
	};

	auto SaveGraphJson = [&SaveRequiredJson, &GraphsDir](const FString& FileName, UEdGraph* Graph, const FString& GraphKind) -> bool
	{
		return SaveRequiredJson(FPaths::Combine(GraphsDir, FileName), UeAgentAnimBlueprintFolderOps::ExportGraphJson(Graph, GraphKind));
	};

	auto InvokeAnimSubCommand = [this](const FString& Command, const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& SubData, FString& SubError) -> bool
	{
		FUeAgentRequestContext SubCtx;
		SubCtx.RequestId = TEXT("anim_blueprint_folder_export");
		SubCtx.Command = Command;
		SubCtx.Params = Params;
		if (!SubData.IsValid())
		{
			SubData = MakeShared<FJsonObject>();
		}
		return ExecuteAnimBlueprintCommand(Command, SubCtx, SubData, SubError);
	};

	auto InvokeBlueprintSubCommand = [this](const FString& Command, const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& SubData, FString& SubError) -> bool
	{
		FUeAgentRequestContext SubCtx;
		SubCtx.RequestId = TEXT("anim_blueprint_folder_export");
		SubCtx.Command = Command;
		SubCtx.Params = Params;
		if (!SubData.IsValid())
		{
			SubData = MakeShared<FJsonObject>();
		}
		return ExecuteBlueprintCommand(Command, SubCtx, SubData, SubError);
	};

	TSharedPtr<FJsonObject> AssetJson = MakeShared<FJsonObject>();
	AssetJson->SetNumberField(TEXT("format_version"), 1);
	AssetJson->SetStringField(TEXT("asset_kind"), TEXT("anim_blueprint"));
	AssetJson->SetStringField(TEXT("asset_path"), AssetPath);
	AssetJson->SetStringField(TEXT("asset_name"), FPackageName::GetLongPackageAssetName(AssetPath));
	AssetJson->SetNumberField(TEXT("profile_version"), 1);
	AssetJson->SetStringField(
		TEXT("anim_blueprint_subkind"),
		UeAgentAnimBlueprintOps::IsLayerInterfaceBlueprint(AnimBlueprint) ? TEXT("anim_layer_interface") : TEXT("anim_blueprint"));
	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("asset.json")), AssetJson))
	{
		return false;
	}

	TSharedPtr<FJsonObject> SettingsJson = MakeShared<FJsonObject>();
	SettingsJson->SetStringField(TEXT("parent_class"), AnimBlueprint->ParentClass ? AnimBlueprint->ParentClass->GetPathName() : TEXT(""));
	SettingsJson->SetStringField(TEXT("target_skeleton"), AnimBlueprint->TargetSkeleton ? AnimBlueprint->TargetSkeleton->GetPathName() : TEXT(""));
	SettingsJson->SetBoolField(TEXT("template"), AnimBlueprint->bIsTemplate);
	SettingsJson->SetStringField(TEXT("preview_skeletal_mesh"), AnimBlueprint->GetPreviewMesh() ? AnimBlueprint->GetPreviewMesh()->GetPathName() : TEXT(""));
	{
		TSharedPtr<FJsonObject> PreviewAnimBlueprintObj = MakeShared<FJsonObject>();
		const UAnimBlueprint* PreviewAnimBlueprint = AnimBlueprint->GetPreviewAnimationBlueprint();
		PreviewAnimBlueprintObj->SetBoolField(TEXT("enabled"), PreviewAnimBlueprint != nullptr);
		PreviewAnimBlueprintObj->SetStringField(TEXT("path"), PreviewAnimBlueprint ? PreviewAnimBlueprint->GetPathName() : TEXT(""));
		PreviewAnimBlueprintObj->SetStringField(
			TEXT("application_method"),
			UeAgentAnimBlueprintOps::PreviewApplicationMethodToString(AnimBlueprint->GetPreviewAnimationBlueprintApplicationMethod()));
		PreviewAnimBlueprintObj->SetStringField(TEXT("tag"), AnimBlueprint->GetPreviewAnimationBlueprintTag().ToString());
		SettingsJson->SetObjectField(TEXT("preview_animation_blueprint"), PreviewAnimBlueprintObj);
	}
	if (!SaveRequiredJson(FPaths::Combine(SettingsDir, TEXT("anim_blueprint.json")), SettingsJson))
	{
		return false;
	}

	{
		TSharedPtr<FJsonObject> ProxyParams = MakeShared<FJsonObject>();
		ProxyParams->SetStringField(TEXT("asset_path"), AssetPath);
		ProxyParams->SetBoolField(TEXT("clean_output_dir"), true);
		ProxyParams->SetBoolField(TEXT("include_validation"), false);
		TSharedPtr<FJsonObject> ProxyData;
		FString ProxyError;
		if (!InvokeBlueprintSubCommand(TEXT("blueprint_export_folder"), ProxyParams, ProxyData, ProxyError))
		{
			OutError = ProxyError;
			return false;
		}

		const FString ProxyFolderPath = ProxyData->GetStringField(TEXT("folder_path"));
		bool bCopied = false;
		if (!UeAgentAnimBlueprintFolderOps::CopyJsonFileIfExists(FPaths::Combine(ProxyFolderPath, TEXT("members"), TEXT("variables.json")), FPaths::Combine(MembersDir, TEXT("variables.json")), bCopied, OutError))
		{
			return false;
		}
		FileCount += bCopied ? 1 : 0;
		if (!UeAgentAnimBlueprintFolderOps::CopyJsonFileIfExists(FPaths::Combine(ProxyFolderPath, TEXT("members"), TEXT("delegates.json")), FPaths::Combine(MembersDir, TEXT("delegates.json")), bCopied, OutError))
		{
			return false;
		}
		FileCount += bCopied ? 1 : 0;
		if (!UeAgentAnimBlueprintFolderOps::CopyJsonFileIfExists(FPaths::Combine(ProxyFolderPath, TEXT("members"), TEXT("defaults.json")), FPaths::Combine(MembersDir, TEXT("defaults.json")), bCopied, OutError))
		{
			return false;
		}
		FileCount += bCopied ? 1 : 0;

		TArray<FString> ProxyGraphFiles;
		IFileManager::Get().FindFiles(ProxyGraphFiles, *(FPaths::Combine(ProxyFolderPath, TEXT("graphs"), TEXT("*.json"))), true, false);
		ProxyGraphFiles.Sort();
		for (const FString& ProxyGraphFile : ProxyGraphFiles)
		{
			TSharedPtr<FJsonObject> ProxyGraphRoot;
			if (!UeAgentAnimBlueprintFolderOps::LoadJsonFile(FPaths::Combine(ProxyFolderPath, TEXT("graphs"), ProxyGraphFile), ProxyGraphRoot, OutError))
			{
				return false;
			}

			TSharedPtr<FJsonObject> ProxyGraphHeader;
			if (!UeAgentAnimBlueprintFolderOps::TryGetObjectField(ProxyGraphRoot, TEXT("graph"), ProxyGraphHeader))
			{
				OutError = TEXT("missing_graph_header");
				return false;
			}

			FString GraphKind;
			FString GraphName;
			if (!UeAgentAnimBlueprintFolderOps::TryGetString(ProxyGraphHeader, TEXT("graph_kind"), GraphKind) || GraphKind.IsEmpty())
			{
				OutError = TEXT("missing_graph_kind");
				return false;
			}
			if (!UeAgentAnimBlueprintFolderOps::TryGetString(ProxyGraphHeader, TEXT("name"), GraphName) || GraphName.IsEmpty())
			{
				OutError = TEXT("missing_graph_name");
				return false;
			}

			UEdGraph* ActualGraph = UeAgentAnimBlueprintFolderOps::ResolveGraphByPathOrName(AnimBlueprint, GraphName);
			const bool bIsAnimFunctionGraph =
				GraphKind.Equals(TEXT("function_graph"), ESearchCase::IgnoreCase)
				&& ActualGraph
				&& AnimationEditorUtils::IsAnimGraph(ActualGraph);
			const bool bKeepGraph =
				GraphKind.Equals(TEXT("event_graph"), ESearchCase::IgnoreCase)
				|| GraphKind.Equals(TEXT("macro_graph"), ESearchCase::IgnoreCase)
				|| (GraphKind.Equals(TEXT("function_graph"), ESearchCase::IgnoreCase) && !bIsAnimFunctionGraph);
			if (!bKeepGraph)
			{
				continue;
			}

			UeAgentAnimBlueprintFolderOps::ConvertGraphRootToAnimStyle(ProxyGraphRoot);
			if (!ProxyGraphHeader->HasField(TEXT("graph_path")))
			{
				if (ActualGraph)
				{
					ProxyGraphHeader->SetStringField(TEXT("graph_path"), ActualGraph->GetPathName());
				}
			}
			if (!SaveRequiredJson(FPaths::Combine(GraphsDir, ProxyGraphFile), ProxyGraphRoot))
			{
				return false;
			}
		}

		IFileManager::Get().DeleteDirectory(*ProxyFolderPath, false, true);
	}

	TSharedPtr<FJsonObject> InterfacesData;
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		if (!InvokeAnimSubCommand(TEXT("anim_blueprint_list_layer_interfaces"), Params, InterfacesData, OutError))
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* InterfacesArray = nullptr;
		TArray<TSharedPtr<FJsonValue>> ImplementedInterfaces;
		if (InterfacesData->TryGetArrayField(TEXT("interfaces"), InterfacesArray) && InterfacesArray)
		{
			for (const TSharedPtr<FJsonValue>& InterfaceValue : *InterfacesArray)
			{
				const TSharedPtr<FJsonObject>* InterfaceObjPtr = nullptr;
				if (!InterfaceValue.IsValid() || !InterfaceValue->TryGetObject(InterfaceObjPtr) || !InterfaceObjPtr || !InterfaceObjPtr->IsValid())
				{
					continue;
				}

				TSharedPtr<FJsonObject> InterfaceObj = MakeShared<FJsonObject>();
				FString InterfaceClass;
				FString InterfaceName;
				UeAgentAnimBlueprintFolderOps::TryGetString(*InterfaceObjPtr, TEXT("interface_class"), InterfaceClass);
				UeAgentAnimBlueprintFolderOps::TryGetString(*InterfaceObjPtr, TEXT("interface_name"), InterfaceName);
				InterfaceObj->SetStringField(TEXT("interface_class"), InterfaceClass);
				InterfaceObj->SetStringField(TEXT("interface_name"), InterfaceName);
				InterfaceObj->SetBoolField(TEXT("preserve_functions_on_remove"), false);
				ImplementedInterfaces.Add(MakeShared<FJsonValueObject>(InterfaceObj));
			}
		}

		ImplementedInterfaces.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
		{
			const TSharedPtr<FJsonObject>* AObj = nullptr;
			const TSharedPtr<FJsonObject>* BObj = nullptr;
			const FString AClass = (A.IsValid() && A->TryGetObject(AObj) && AObj && AObj->IsValid()) ? (*AObj)->GetStringField(TEXT("interface_class")) : FString();
			const FString BClass = (B.IsValid() && B->TryGetObject(BObj) && BObj && BObj->IsValid()) ? (*BObj)->GetStringField(TEXT("interface_class")) : FString();
			return AClass < BClass;
		});

		TSharedPtr<FJsonObject> InterfacesJson = MakeShared<FJsonObject>();
		InterfacesJson->SetArrayField(TEXT("implemented_interfaces"), ImplementedInterfaces);
		if (!SaveRequiredJson(FPaths::Combine(LayerInterfacesDir, TEXT("interfaces.json")), InterfacesJson))
		{
			return false;
		}
	}

	TArray<FString> RequiredLayerNames;
	{
		TSharedPtr<FJsonObject> LayersData;
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		if (!InvokeAnimSubCommand(TEXT("anim_blueprint_list_anim_layers"), Params, LayersData, OutError))
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* LayersArray = nullptr;
		TArray<TSharedPtr<FJsonValue>> ExportedLayers;
		bool bHasRootAnimGraphLayer = false;
		if (LayersData->TryGetArrayField(TEXT("anim_layers"), LayersArray) && LayersArray)
		{
			for (const TSharedPtr<FJsonValue>& LayerValue : *LayersArray)
			{
				const TSharedPtr<FJsonObject>* LayerObjPtr = nullptr;
				if (!LayerValue.IsValid() || !LayerValue->TryGetObject(LayerObjPtr) || !LayerObjPtr || !LayerObjPtr->IsValid())
				{
					continue;
				}

				FString LayerName;
				FString GraphPath;
				FString ImplementedInterface;
				bool bHasGraph = false;
				UeAgentAnimBlueprintFolderOps::TryGetString(*LayerObjPtr, TEXT("layer_name"), LayerName);
				UeAgentAnimBlueprintFolderOps::TryGetString(*LayerObjPtr, TEXT("graph_path"), GraphPath);
				UeAgentAnimBlueprintFolderOps::TryGetString(*LayerObjPtr, TEXT("implemented_interface"), ImplementedInterface);
				UeAgentAnimBlueprintFolderOps::TryGetBool(*LayerObjPtr, TEXT("has_graph"), bHasGraph);
				if (LayerName.IsEmpty())
				{
					continue;
				}

				TSharedPtr<FJsonObject> ExportLayerObj = MakeShared<FJsonObject>();
				ExportLayerObj->SetStringField(TEXT("layer_name"), LayerName);
				if (LayerName.Equals(UEdGraphSchema_K2::GN_AnimGraph.ToString(), ESearchCase::IgnoreCase))
				{
					bHasRootAnimGraphLayer = true;
					ExportLayerObj->SetStringField(TEXT("layer_kind"), TEXT("root_anim_graph"));
				}
				else if (!ImplementedInterface.IsEmpty())
				{
					ExportLayerObj->SetStringField(TEXT("layer_kind"), TEXT("interface_anim_layer"));
				}
				else
				{
					ExportLayerObj->SetStringField(TEXT("layer_kind"), TEXT("local_anim_layer"));
				}

				ExportLayerObj->SetStringField(TEXT("graph_path"), GraphPath);
				ExportLayerObj->SetStringField(TEXT("implemented_interface"), ImplementedInterface);
				ExportLayerObj->SetBoolField(TEXT("has_graph"), bHasGraph);

				const TArray<TSharedPtr<FJsonValue>>* InputPoseNames = nullptr;
				if ((*LayerObjPtr)->TryGetArrayField(TEXT("input_pose_names"), InputPoseNames) && InputPoseNames)
				{
					ExportLayerObj->SetArrayField(TEXT("input_pose_names"), *InputPoseNames);
				}
				else
				{
					ExportLayerObj->SetArrayField(TEXT("input_pose_names"), {});
				}

				if (bHasGraph)
				{
					const FString GraphFile = UeAgentAnimBlueprintFolderOps::MakeAnimLayerGraphFileName(LayerName);
					ExportLayerObj->SetStringField(TEXT("graph_file"), FString::Printf(TEXT("graphs/%s"), *GraphFile));
					if (UEdGraph* LayerGraph = UeAgentAnimBlueprintFolderOps::ResolveGraphByPathOrName(AnimBlueprint, GraphPath.IsEmpty() ? LayerName : GraphPath))
					{
						if (!SaveGraphJson(GraphFile, LayerGraph, TEXT("anim_layer_graph")))
						{
							return false;
						}
					}
					else
					{
						OutError = TEXT("anim_layer_graph_not_found");
						return false;
					}
				}

				RequiredLayerNames.Add(LayerName);
				ExportedLayers.Add(MakeShared<FJsonValueObject>(ExportLayerObj));
			}
		}

		if (!bHasRootAnimGraphLayer)
		{
			if (UEdGraph* RootAnimGraph = UeAgentAnimBlueprintFolderOps::ResolveGraphByPathOrName(AnimBlueprint, TEXT("AnimGraph")))
			{
				const FString GraphFile = UeAgentAnimBlueprintFolderOps::MakeAnimLayerGraphFileName(TEXT("AnimGraph"));
				TSharedPtr<FJsonObject> RootLayerObj = MakeShared<FJsonObject>();
				RootLayerObj->SetStringField(TEXT("layer_name"), TEXT("AnimGraph"));
				RootLayerObj->SetStringField(TEXT("layer_kind"), TEXT("root_anim_graph"));
				RootLayerObj->SetStringField(TEXT("graph_path"), RootAnimGraph->GetPathName());
				RootLayerObj->SetStringField(TEXT("implemented_interface"), TEXT(""));
				RootLayerObj->SetBoolField(TEXT("has_graph"), true);
				RootLayerObj->SetArrayField(TEXT("input_pose_names"), {});
				RootLayerObj->SetStringField(TEXT("graph_file"), FString::Printf(TEXT("graphs/%s"), *GraphFile));
				if (!SaveGraphJson(GraphFile, RootAnimGraph, TEXT("anim_layer_graph")))
				{
					return false;
				}
				RequiredLayerNames.Add(TEXT("AnimGraph"));
				ExportedLayers.Add(MakeShared<FJsonValueObject>(RootLayerObj));
			}
		}

		ExportedLayers.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
		{
			const TSharedPtr<FJsonObject>* AObj = nullptr;
			const TSharedPtr<FJsonObject>* BObj = nullptr;
			const FString AName = (A.IsValid() && A->TryGetObject(AObj) && AObj && AObj->IsValid()) ? (*AObj)->GetStringField(TEXT("layer_name")) : FString();
			const FString BName = (B.IsValid() && B->TryGetObject(BObj) && BObj && BObj->IsValid()) ? (*BObj)->GetStringField(TEXT("layer_name")) : FString();
			if (AName.Equals(TEXT("AnimGraph"), ESearchCase::IgnoreCase))
			{
				return true;
			}
			if (BName.Equals(TEXT("AnimGraph"), ESearchCase::IgnoreCase))
			{
				return false;
			}
			return AName < BName;
		});

		TSharedPtr<FJsonObject> LayersJson = MakeShared<FJsonObject>();
		LayersJson->SetArrayField(TEXT("anim_layers"), ExportedLayers);
		if (!SaveRequiredJson(FPaths::Combine(AnimLayersDir, TEXT("layers.json")), LayersJson))
		{
			return false;
		}
	}

	TArray<FString> RequiredStateMachineNames;
	TMap<FString, TArray<FString>> RequiredStatesByMachine;
	{
		TSharedPtr<FJsonObject> StateMachinesData;
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		if (!InvokeAnimSubCommand(TEXT("anim_blueprint_list_state_machines"), Params, StateMachinesData, OutError))
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* StateMachinesArray = nullptr;
		if (StateMachinesData->TryGetArrayField(TEXT("state_machines"), StateMachinesArray) && StateMachinesArray)
		{
			for (const TSharedPtr<FJsonValue>& StateMachineValue : *StateMachinesArray)
			{
				const TSharedPtr<FJsonObject>* StateMachineObjPtr = nullptr;
				if (!StateMachineValue.IsValid() || !StateMachineValue->TryGetObject(StateMachineObjPtr) || !StateMachineObjPtr || !StateMachineObjPtr->IsValid())
				{
					continue;
				}

				FString StateMachineName;
				FString OwnerAnimGraphName;
				FString GraphPath;
				UeAgentAnimBlueprintFolderOps::TryGetString(*StateMachineObjPtr, TEXT("state_machine_name"), StateMachineName);
				UeAgentAnimBlueprintFolderOps::TryGetString(*StateMachineObjPtr, TEXT("owner_anim_graph_name"), OwnerAnimGraphName);
				UeAgentAnimBlueprintFolderOps::TryGetString(*StateMachineObjPtr, TEXT("graph_path"), GraphPath);
				if (StateMachineName.IsEmpty())
				{
					continue;
				}

				UAnimGraphNode_StateMachineBase* StateMachineNode = UeAgentAnimBlueprintOps::FindStateMachineNode(AnimBlueprint, StateMachineName);
				UAnimationStateMachineGraph* StateMachineGraph = StateMachineNode ? StateMachineNode->EditorStateMachineGraph : nullptr;
				if (!StateMachineGraph)
				{
					OutError = TEXT("state_machine_graph_not_found");
					return false;
				}

				TSharedPtr<FJsonObject> StateMachineJson = MakeShared<FJsonObject>();
				StateMachineJson->SetStringField(TEXT("state_machine_name"), StateMachineName);
				StateMachineJson->SetStringField(TEXT("owner_anim_graph_name"), OwnerAnimGraphName);
				StateMachineJson->SetStringField(TEXT("graph_path"), GraphPath);
				{
					TSharedPtr<FJsonObject> PosObj = MakeShared<FJsonObject>();
					PosObj->SetNumberField(TEXT("x"), StateMachineNode->NodePosX);
					PosObj->SetNumberField(TEXT("y"), StateMachineNode->NodePosY);
					StateMachineJson->SetObjectField(TEXT("pos"), PosObj);
				}

				TMap<FString, FString> NodeNameToId;
				TArray<TSharedPtr<FJsonValue>> ExportedNodes;
				const TArray<TSharedPtr<FJsonValue>>* StateNodesArray = nullptr;
				if ((*StateMachineObjPtr)->TryGetArrayField(TEXT("state_nodes"), StateNodesArray) && StateNodesArray)
				{
					for (const TSharedPtr<FJsonValue>& StateNodeValue : *StateNodesArray)
					{
						const TSharedPtr<FJsonObject>* StateNodeObjPtr = nullptr;
						if (!StateNodeValue.IsValid() || !StateNodeValue->TryGetObject(StateNodeObjPtr) || !StateNodeObjPtr || !StateNodeObjPtr->IsValid())
						{
							continue;
						}

						FString StateName;
						FString NodeType;
						FString BoundGraphPath;
						UeAgentAnimBlueprintFolderOps::TryGetString(*StateNodeObjPtr, TEXT("state_name"), StateName);
						UeAgentAnimBlueprintFolderOps::TryGetString(*StateNodeObjPtr, TEXT("node_type"), NodeType);
						UeAgentAnimBlueprintFolderOps::TryGetString(*StateNodeObjPtr, TEXT("bound_graph_path"), BoundGraphPath);
						if (StateName.IsEmpty() || NodeType.IsEmpty())
						{
							continue;
						}
						if (NodeType.Equals(TEXT("state_node"), ESearchCase::IgnoreCase))
						{
							// Transition tunnel placeholders are represented by the transition list already.
							// Emitting them as state nodes makes the folder format fail to round-trip.
							continue;
						}

						const FString NodeId = UeAgentAnimBlueprintFolderOps::MakeStateNodeId(NodeType, StateName);
						NodeNameToId.Add(StateName, NodeId);

						TSharedPtr<FJsonObject> ExportNodeObj = MakeShared<FJsonObject>();
						ExportNodeObj->SetStringField(TEXT("id"), NodeId);
						ExportNodeObj->SetStringField(TEXT("node_kind"), NodeType);
						ExportNodeObj->SetStringField(TEXT("state_name"), StateName);
						{
							double NodePosX = 0.0;
							double NodePosY = 0.0;
							UeAgentAnimBlueprintFolderOps::TryGetNumber(*StateNodeObjPtr, TEXT("node_pos_x"), NodePosX);
							UeAgentAnimBlueprintFolderOps::TryGetNumber(*StateNodeObjPtr, TEXT("node_pos_y"), NodePosY);
							TSharedPtr<FJsonObject> PosObj = MakeShared<FJsonObject>();
							PosObj->SetNumberField(TEXT("x"), NodePosX);
							PosObj->SetNumberField(TEXT("y"), NodePosY);
							ExportNodeObj->SetObjectField(TEXT("pos"), PosObj);
						}

						if (NodeType.Equals(TEXT("state"), ESearchCase::IgnoreCase))
						{
							FString StateType;
							bool bAlwaysResetOnEntry = false;
							UeAgentAnimBlueprintFolderOps::TryGetString(*StateNodeObjPtr, TEXT("state_type"), StateType);
							UeAgentAnimBlueprintFolderOps::TryGetBool(*StateNodeObjPtr, TEXT("always_reset_on_entry"), bAlwaysResetOnEntry);
							ExportNodeObj->SetStringField(TEXT("state_type"), StateType);
							ExportNodeObj->SetBoolField(TEXT("always_reset_on_entry"), bAlwaysResetOnEntry);
							RequiredStatesByMachine.FindOrAdd(StateMachineName).Add(StateName);
						}
						else if (NodeType.Equals(TEXT("alias"), ESearchCase::IgnoreCase))
						{
							bool bGlobalAlias = false;
							UeAgentAnimBlueprintFolderOps::TryGetBool(*StateNodeObjPtr, TEXT("global_alias"), bGlobalAlias);
							ExportNodeObj->SetBoolField(TEXT("global_alias"), bGlobalAlias);

							const TArray<TSharedPtr<FJsonValue>>* AliasTargets = nullptr;
							if ((*StateNodeObjPtr)->TryGetArrayField(TEXT("alias_targets"), AliasTargets) && AliasTargets)
							{
								ExportNodeObj->SetArrayField(TEXT("alias_target_states"), *AliasTargets);
							}
							else
							{
								ExportNodeObj->SetArrayField(TEXT("alias_target_states"), {});
							}
						}

						if (!BoundGraphPath.IsEmpty())
						{
							const FString BoundGraphFile = UeAgentAnimBlueprintFolderOps::MakeStateGraphFileName(StateMachineName, StateName, NodeType);
							ExportNodeObj->SetStringField(TEXT("bound_graph_file"), FString::Printf(TEXT("graphs/%s"), *BoundGraphFile));
							if (UEdGraph* BoundGraph = UeAgentAnimBlueprintFolderOps::ResolveGraphByPathOrName(AnimBlueprint, BoundGraphPath))
							{
								if (!SaveGraphJson(BoundGraphFile, BoundGraph, TEXT("state_graph")))
								{
									return false;
								}
							}
							else
							{
								OutError = TEXT("state_bound_graph_not_found");
								return false;
							}
						}

						ExportedNodes.Add(MakeShared<FJsonValueObject>(ExportNodeObj));
					}
				}

				ExportedNodes.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
				{
					const TSharedPtr<FJsonObject>* AObj = nullptr;
					const TSharedPtr<FJsonObject>* BObj = nullptr;
					FString AKind;
					FString BKind;
					FString AName;
					FString BName;
					if (A.IsValid() && A->TryGetObject(AObj) && AObj && AObj->IsValid())
					{
						(*AObj)->TryGetStringField(TEXT("node_kind"), AKind);
						(*AObj)->TryGetStringField(TEXT("state_name"), AName);
					}
					if (B.IsValid() && B->TryGetObject(BObj) && BObj && BObj->IsValid())
					{
						(*BObj)->TryGetStringField(TEXT("node_kind"), BKind);
						(*BObj)->TryGetStringField(TEXT("state_name"), BName);
					}
					if (UeAgentAnimBlueprintFolderOps::GetNodeKindPriority(AKind) != UeAgentAnimBlueprintFolderOps::GetNodeKindPriority(BKind))
					{
						return UeAgentAnimBlueprintFolderOps::GetNodeKindPriority(AKind) < UeAgentAnimBlueprintFolderOps::GetNodeKindPriority(BKind);
					}
					return AName < BName;
				});
				StateMachineJson->SetArrayField(TEXT("nodes"), ExportedNodes);

				TArray<TSharedPtr<FJsonValue>> ExportedTransitions;
				const TArray<TSharedPtr<FJsonValue>>* TransitionNodesArray = nullptr;
				if ((*StateMachineObjPtr)->TryGetArrayField(TEXT("transition_nodes"), TransitionNodesArray) && TransitionNodesArray)
				{
					for (const TSharedPtr<FJsonValue>& TransitionValue : *TransitionNodesArray)
					{
						const TSharedPtr<FJsonObject>* TransitionObjPtr = nullptr;
						if (!TransitionValue.IsValid() || !TransitionValue->TryGetObject(TransitionObjPtr) || !TransitionObjPtr || !TransitionObjPtr->IsValid())
						{
							continue;
						}

						FString SourceStateName;
						FString TargetStateName;
						FString BoundGraphPath;
						FString TransitionGuidText;
						UeAgentAnimBlueprintFolderOps::TryGetString(*TransitionObjPtr, TEXT("source_state_name"), SourceStateName);
						UeAgentAnimBlueprintFolderOps::TryGetString(*TransitionObjPtr, TEXT("target_state_name"), TargetStateName);
						UeAgentAnimBlueprintFolderOps::TryGetString(*TransitionObjPtr, TEXT("bound_graph_path"), BoundGraphPath);
						UeAgentAnimBlueprintFolderOps::TryGetString(*TransitionObjPtr, TEXT("node_guid"), TransitionGuidText);
						if (SourceStateName.IsEmpty() || TargetStateName.IsEmpty())
						{
							continue;
						}

						int32 TransitionPosX = 0;
						int32 TransitionPosY = 0;
						if (!TransitionGuidText.IsEmpty())
						{
							FGuid TransitionGuid;
							if (UeAgentAnimBlueprintOps::ParseNodeGuid(TransitionGuidText, TransitionGuid))
							{
								if (UAnimStateTransitionNode* TransitionNode = UeAgentAnimBlueprintOps::FindTransitionNodeByGuid(StateMachineGraph, TransitionGuid))
								{
									TransitionPosX = TransitionNode->NodePosX;
									TransitionPosY = TransitionNode->NodePosY;
								}
							}
						}

						TSharedPtr<FJsonObject> ExportTransitionObj = MakeShared<FJsonObject>();
						ExportTransitionObj->SetStringField(TEXT("id"), UeAgentAnimBlueprintFolderOps::MakeTransitionId(SourceStateName, TargetStateName));
						ExportTransitionObj->SetStringField(TEXT("source"), NodeNameToId.FindRef(SourceStateName));
						ExportTransitionObj->SetStringField(TEXT("target"), NodeNameToId.FindRef(TargetStateName));
						{
							TSharedPtr<FJsonObject> PosObj = MakeShared<FJsonObject>();
							PosObj->SetNumberField(TEXT("x"), TransitionPosX);
							PosObj->SetNumberField(TEXT("y"), TransitionPosY);
							ExportTransitionObj->SetObjectField(TEXT("pos"), PosObj);
						}

						double PriorityOrder = 0.0;
						double CrossfadeDuration = 0.0;
						bool bBidirectional = false;
						bool bDisabled = false;
						bool bAutomaticRule = false;
						UeAgentAnimBlueprintFolderOps::TryGetNumber(*TransitionObjPtr, TEXT("priority_order"), PriorityOrder);
						UeAgentAnimBlueprintFolderOps::TryGetNumber(*TransitionObjPtr, TEXT("crossfade_duration"), CrossfadeDuration);
						UeAgentAnimBlueprintFolderOps::TryGetBool(*TransitionObjPtr, TEXT("bidirectional"), bBidirectional);
						UeAgentAnimBlueprintFolderOps::TryGetBool(*TransitionObjPtr, TEXT("disabled"), bDisabled);
						UeAgentAnimBlueprintFolderOps::TryGetBool(*TransitionObjPtr, TEXT("automatic_rule"), bAutomaticRule);
						ExportTransitionObj->SetNumberField(TEXT("priority_order"), PriorityOrder);
						ExportTransitionObj->SetNumberField(TEXT("crossfade_duration"), CrossfadeDuration);
						ExportTransitionObj->SetBoolField(TEXT("bidirectional"), bBidirectional);
						ExportTransitionObj->SetBoolField(TEXT("disabled"), bDisabled);
						ExportTransitionObj->SetBoolField(TEXT("automatic_rule"), bAutomaticRule);

						if (!BoundGraphPath.IsEmpty())
						{
							const FString RuleGraphFile = UeAgentAnimBlueprintFolderOps::MakeTransitionGraphFileName(StateMachineName, SourceStateName, TargetStateName);
							ExportTransitionObj->SetStringField(TEXT("rule_graph_file"), FString::Printf(TEXT("graphs/%s"), *RuleGraphFile));
							if (UEdGraph* RuleGraph = UeAgentAnimBlueprintFolderOps::ResolveGraphByPathOrName(AnimBlueprint, BoundGraphPath))
							{
								if (!SaveGraphJson(RuleGraphFile, RuleGraph, TEXT("transition_graph")))
								{
									return false;
								}
							}
							else
							{
								OutError = TEXT("transition_rule_graph_not_found");
								return false;
							}
						}

						ExportedTransitions.Add(MakeShared<FJsonValueObject>(ExportTransitionObj));
					}
				}

				ExportedTransitions.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
				{
					const TSharedPtr<FJsonObject>* AObj = nullptr;
					const TSharedPtr<FJsonObject>* BObj = nullptr;
					const FString AId = (A.IsValid() && A->TryGetObject(AObj) && AObj && AObj->IsValid()) ? (*AObj)->GetStringField(TEXT("id")) : FString();
					const FString BId = (B.IsValid() && B->TryGetObject(BObj) && BObj && BObj->IsValid()) ? (*BObj)->GetStringField(TEXT("id")) : FString();
					return AId < BId;
				});
				StateMachineJson->SetArrayField(TEXT("transitions"), ExportedTransitions);

				FString EntryStateName;
				UeAgentAnimBlueprintFolderOps::TryGetString(*StateMachineObjPtr, TEXT("entry_connected_state_name"), EntryStateName);
				if (!EntryStateName.IsEmpty())
				{
					TSharedPtr<FJsonObject> EntryObj = MakeShared<FJsonObject>();
					EntryObj->SetStringField(TEXT("node_id"), NodeNameToId.FindRef(EntryStateName));
					EntryObj->SetStringField(TEXT("state_name"), EntryStateName);
					StateMachineJson->SetObjectField(TEXT("entry_target"), EntryObj);
				}

				RequiredStateMachineNames.Add(StateMachineName);
				RequiredStatesByMachine.FindOrAdd(StateMachineName).Sort();
				if (!SaveRequiredJson(FPaths::Combine(StateMachinesDir, UeAgentAnimBlueprintFolderOps::MakeStateMachineFileName(StateMachineName)), StateMachineJson))
				{
					return false;
				}
			}
		}
	}

	RequiredLayerNames.Sort();
	RequiredStateMachineNames.Sort();
	for (TPair<FString, TArray<FString>>& Pair : RequiredStatesByMachine)
	{
		Pair.Value.Sort();
	}

	if (bIncludeValidation)
	{
		if (!SaveRequiredJson(
				FPaths::Combine(ValidationDir, TEXT("checks.json")),
				UeAgentAnimBlueprintFolderOps::ExportValidationJson(RequiredLayerNames, RequiredStateMachineNames, RequiredStatesByMachine)))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), AssetPath);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetStringField(TEXT("root_path"), UeAgentAnimBlueprintFolderOps::GetFolderRootAbsolute());
	OutData->SetNumberField(TEXT("file_count"), FileCount);
	OutData->SetBoolField(TEXT("clean_output_dir"), bCleanOutputDir);
	return true;
}

bool FUeAgentHttpServer::CmdAnimBlueprintApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	const FString AssetPath = UeAgentAnimBlueprintOps::NormalizeAssetPath(AssetPathInput);
	FString FolderPath;
	if (!UeAgentAnimBlueprintFolderOps::ResolveFolderForAsset(AssetPath, FolderPath, OutError))
	{
		return false;
	}

	TSharedPtr<FJsonObject> AssetJson;
	if (!UeAgentAnimBlueprintFolderOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("asset.json")), AssetJson, OutError))
	{
		return false;
	}

	FString AssetKind;
	if (!UeAgentAnimBlueprintFolderOps::TryGetString(AssetJson, TEXT("asset_kind"), AssetKind) || !AssetKind.Equals(TEXT("anim_blueprint"), ESearchCase::IgnoreCase))
	{
		OutError = TEXT("unsupported_asset_kind");
		return false;
	}

	FString AssetPathInFile;
	if (UeAgentAnimBlueprintFolderOps::TryGetString(AssetJson, TEXT("asset_path"), AssetPathInFile) && !AssetPathInFile.IsEmpty())
	{
		const FString NormalizedFileAssetPath = UeAgentAnimBlueprintOps::NormalizeAssetPath(AssetPathInFile);
		if (!NormalizedFileAssetPath.Equals(AssetPath, ESearchCase::IgnoreCase))
		{
			OutError = TEXT("asset_path_mismatch");
			return false;
		}
	}

	FString AssetSubkind = TEXT("anim_blueprint");
	UeAgentAnimBlueprintFolderOps::TryGetString(AssetJson, TEXT("anim_blueprint_subkind"), AssetSubkind);

	bool bCompileAfterApply = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_apply"), bCompileAfterApply);
	bool bSaveAfterApply = true;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);
	bool bCreateIfMissing = true;
	JsonTryGetBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing);

	int32 VariablesAdded = 0;
	int32 VariablesRemoved = 0;
	int32 VariableDefaultsUpdated = 0;
	int32 DelegatesAdded = 0;
	int32 LogicGraphsRebuilt = 0;
	int32 AnimGraphsRebuilt = 0;
	int32 LayerInterfacesAdded = 0;
	int32 LayerInterfacesRemoved = 0;
	int32 LocalLayersAdded = 0;
	int32 LocalLayersRemoved = 0;
	int32 StateMachinesRebuilt = 0;
	int32 StateNodesAdded = 0;
	int32 TransitionsAdded = 0;
	int32 NodesCreated = 0;
	int32 EdgesCreated = 0;
	int32 PinDefaultsApplied = 0;
	TArray<TSharedPtr<FJsonValue>> Warnings;

	auto AddWarning = [&Warnings](const FString& Code, const FString& Message)
	{
		TSharedPtr<FJsonObject> WarningObj = MakeShared<FJsonObject>();
		WarningObj->SetStringField(TEXT("code"), Code);
		WarningObj->SetStringField(TEXT("message"), Message);
		Warnings.Add(MakeShared<FJsonValueObject>(WarningObj));
	};

	auto InvokeAnimSubCommand = [this](const FString& Command, const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& SubData, FString& SubError) -> bool
	{
		FUeAgentRequestContext SubCtx;
		SubCtx.RequestId = TEXT("anim_blueprint_folder_apply");
		SubCtx.Command = Command;
		SubCtx.Params = Params;
		if (!SubData.IsValid())
		{
			SubData = MakeShared<FJsonObject>();
		}
		return ExecuteAnimBlueprintCommand(Command, SubCtx, SubData, SubError);
	};

	auto InvokeBlueprintSubCommand = [this](const FString& Command, const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& SubData, FString& SubError) -> bool
	{
		FUeAgentRequestContext SubCtx;
		SubCtx.RequestId = TEXT("anim_blueprint_folder_apply");
		SubCtx.Command = Command;
		SubCtx.Params = Params;
		if (!SubData.IsValid())
		{
			SubData = MakeShared<FJsonObject>();
		}
		return ExecuteBlueprintCommand(Command, SubCtx, SubData, SubError);
	};

	TSharedPtr<FJsonObject> SettingsJson = MakeShared<FJsonObject>();
	FString SettingsError;
	if (!UeAgentAnimBlueprintFolderOps::LoadOptionalJsonFile(FPaths::Combine(FolderPath, TEXT("settings"), TEXT("anim_blueprint.json")), SettingsJson, SettingsError))
	{
		OutError = SettingsError;
		return false;
	}
	if (!SettingsJson.IsValid())
	{
		SettingsJson = MakeShared<FJsonObject>();
	}

	UAnimBlueprint* AnimBlueprint = UeAgentAnimBlueprintOps::LoadAnimBlueprint(AssetPath);
	if (!AnimBlueprint)
	{
		if (!bCreateIfMissing)
		{
			OutError = TEXT("anim_blueprint_not_found");
			return false;
		}

		TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
		CreateParams->SetStringField(TEXT("asset_path"), AssetPath);
		CreateParams->SetBoolField(TEXT("compile_after_create"), false);
		CreateParams->SetBoolField(TEXT("open_editor"), false);
		CreateParams->SetBoolField(TEXT("save_after_create"), false);

		if (AssetSubkind.Equals(TEXT("anim_layer_interface"), ESearchCase::IgnoreCase))
		{
			TSharedPtr<FJsonObject> CreateData;
			if (!InvokeAnimSubCommand(TEXT("anim_blueprint_create_layer_interface"), CreateParams, CreateData, OutError))
			{
				return false;
			}
		}
		else
		{
			FString ParentClassPath = TEXT("/Script/Engine.AnimInstance");
			FString TargetSkeletonPath;
			FString PreviewSkeletalMeshPath;
			bool bTemplate = false;
			UeAgentAnimBlueprintFolderOps::TryGetString(SettingsJson, TEXT("parent_class"), ParentClassPath);
			UeAgentAnimBlueprintFolderOps::TryGetString(SettingsJson, TEXT("target_skeleton"), TargetSkeletonPath);
			UeAgentAnimBlueprintFolderOps::TryGetString(SettingsJson, TEXT("preview_skeletal_mesh"), PreviewSkeletalMeshPath);
			UeAgentAnimBlueprintFolderOps::TryGetBool(SettingsJson, TEXT("template"), bTemplate);

			CreateParams->SetStringField(TEXT("parent_class"), ParentClassPath);
			if (!TargetSkeletonPath.IsEmpty())
			{
				CreateParams->SetStringField(TEXT("target_skeleton"), TargetSkeletonPath);
			}
			if (!PreviewSkeletalMeshPath.IsEmpty())
			{
				CreateParams->SetStringField(TEXT("preview_skeletal_mesh"), PreviewSkeletalMeshPath);
			}
			CreateParams->SetBoolField(TEXT("template"), bTemplate);

			TSharedPtr<FJsonObject> CreateData;
			if (!InvokeAnimSubCommand(TEXT("anim_blueprint_create"), CreateParams, CreateData, OutError))
			{
				return false;
			}
		}

		AnimBlueprint = UeAgentAnimBlueprintOps::LoadAnimBlueprint(AssetPath);
	}

	if (!AnimBlueprint)
	{
		OutError = TEXT("anim_blueprint_load_after_create_failed");
		return false;
	}

	const bool bExpectLayerInterface = AssetSubkind.Equals(TEXT("anim_layer_interface"), ESearchCase::IgnoreCase);
	if (UeAgentAnimBlueprintOps::IsLayerInterfaceBlueprint(AnimBlueprint) != bExpectLayerInterface)
	{
		OutError = TEXT("anim_blueprint_subkind_mismatch");
		return false;
	}

	{
		FString DesiredParentClassPath;
		if (UeAgentAnimBlueprintFolderOps::TryGetString(SettingsJson, TEXT("parent_class"), DesiredParentClassPath) && !DesiredParentClassPath.IsEmpty())
		{
			const FString CurrentParentClassPath = AnimBlueprint->ParentClass ? AnimBlueprint->ParentClass->GetPathName() : FString();
			if (!CurrentParentClassPath.Equals(DesiredParentClassPath, ESearchCase::IgnoreCase))
			{
				TSharedPtr<FJsonObject> ReparentParams = MakeShared<FJsonObject>();
				ReparentParams->SetStringField(TEXT("asset_path"), AssetPath);
				ReparentParams->SetStringField(TEXT("parent_class"), DesiredParentClassPath);
				ReparentParams->SetBoolField(TEXT("compile_after_reparent"), false);
				ReparentParams->SetBoolField(TEXT("save_after_reparent"), false);
				TSharedPtr<FJsonObject> ReparentData;
				if (!InvokeAnimSubCommand(TEXT("anim_blueprint_reparent"), ReparentParams, ReparentData, OutError))
				{
					return false;
				}
				AnimBlueprint = UeAgentAnimBlueprintOps::LoadAnimBlueprint(AssetPath);
			}
		}

		if (!bExpectLayerInterface)
		{
			FString DesiredSkeletonPath;
			UeAgentAnimBlueprintFolderOps::TryGetString(SettingsJson, TEXT("target_skeleton"), DesiredSkeletonPath);
			const FString CurrentSkeletonPath = AnimBlueprint->TargetSkeleton ? AnimBlueprint->TargetSkeleton->GetPathName() : FString();
			if (!DesiredSkeletonPath.IsEmpty() && !CurrentSkeletonPath.Equals(DesiredSkeletonPath, ESearchCase::IgnoreCase))
			{
				AddWarning(TEXT("target_skeleton_update_not_supported"), TEXT("Existing AnimBlueprint target skeleton differs from folder settings; current command surface does not support retargeting in place."));
			}

			bool bTemplate = false;
			if (UeAgentAnimBlueprintFolderOps::TryGetBool(SettingsJson, TEXT("template"), bTemplate) && AnimBlueprint->bIsTemplate != bTemplate)
			{
				AddWarning(TEXT("template_flag_update_not_supported"), TEXT("Existing AnimBlueprint template flag differs from folder settings; current command surface does not support toggling template mode in place."));
			}

			FString DesiredPreviewMeshPath;
			UeAgentAnimBlueprintFolderOps::TryGetString(SettingsJson, TEXT("preview_skeletal_mesh"), DesiredPreviewMeshPath);
			const FString CurrentPreviewMeshPath = AnimBlueprint->GetPreviewMesh() ? AnimBlueprint->GetPreviewMesh()->GetPathName() : FString();
			if (!CurrentPreviewMeshPath.Equals(DesiredPreviewMeshPath, ESearchCase::IgnoreCase))
			{
				TSharedPtr<FJsonObject> PreviewMeshParams = MakeShared<FJsonObject>();
				PreviewMeshParams->SetStringField(TEXT("asset_path"), AssetPath);
				if (DesiredPreviewMeshPath.IsEmpty())
				{
					PreviewMeshParams->SetBoolField(TEXT("clear_preview_mesh"), true);
				}
				else
				{
					PreviewMeshParams->SetStringField(TEXT("skeletal_mesh_path"), DesiredPreviewMeshPath);
				}
				PreviewMeshParams->SetBoolField(TEXT("save_after_set"), false);
				TSharedPtr<FJsonObject> PreviewMeshData;
				if (!InvokeAnimSubCommand(TEXT("anim_blueprint_set_preview_mesh"), PreviewMeshParams, PreviewMeshData, OutError))
				{
					return false;
				}
			}

			TSharedPtr<FJsonObject> PreviewAnimBlueprintObj;
			if (UeAgentAnimBlueprintFolderOps::TryGetObjectField(SettingsJson, TEXT("preview_animation_blueprint"), PreviewAnimBlueprintObj))
			{
				bool bEnabled = false;
				FString PreviewBlueprintPath;
				FString PreviewApplicationMethod = TEXT("linked_layers");
				FString PreviewBlueprintTag;
				UeAgentAnimBlueprintFolderOps::TryGetBool(PreviewAnimBlueprintObj, TEXT("enabled"), bEnabled);
				UeAgentAnimBlueprintFolderOps::TryGetString(PreviewAnimBlueprintObj, TEXT("path"), PreviewBlueprintPath);
				UeAgentAnimBlueprintFolderOps::TryGetString(PreviewAnimBlueprintObj, TEXT("application_method"), PreviewApplicationMethod);
				UeAgentAnimBlueprintFolderOps::TryGetString(PreviewAnimBlueprintObj, TEXT("tag"), PreviewBlueprintTag);

				const FString CurrentPreviewBlueprintPath = AnimBlueprint->GetPreviewAnimationBlueprint() ? AnimBlueprint->GetPreviewAnimationBlueprint()->GetPathName() : FString();
				const FString CurrentPreviewApplicationMethod = UeAgentAnimBlueprintOps::PreviewApplicationMethodToString(AnimBlueprint->GetPreviewAnimationBlueprintApplicationMethod());
				const FString CurrentPreviewBlueprintTag = AnimBlueprint->GetPreviewAnimationBlueprintTag().ToString();
				if (CurrentPreviewBlueprintPath != PreviewBlueprintPath
					|| CurrentPreviewApplicationMethod != PreviewApplicationMethod
					|| CurrentPreviewBlueprintTag != PreviewBlueprintTag
					|| (CurrentPreviewBlueprintPath.IsEmpty() && bEnabled))
				{
					TSharedPtr<FJsonObject> PreviewAnimParams = MakeShared<FJsonObject>();
					PreviewAnimParams->SetStringField(TEXT("asset_path"), AssetPath);
					if (!bEnabled || PreviewBlueprintPath.IsEmpty())
					{
						PreviewAnimParams->SetBoolField(TEXT("clear_preview_animation_blueprint"), true);
					}
					else
					{
						PreviewAnimParams->SetStringField(TEXT("preview_anim_blueprint_path"), PreviewBlueprintPath);
						PreviewAnimParams->SetStringField(TEXT("preview_application_method"), PreviewApplicationMethod);
						if (!PreviewBlueprintTag.IsEmpty())
						{
							PreviewAnimParams->SetStringField(TEXT("preview_animation_blueprint_tag"), PreviewBlueprintTag);
						}
					}
					PreviewAnimParams->SetBoolField(TEXT("save_after_set"), false);
					TSharedPtr<FJsonObject> PreviewAnimData;
					if (!InvokeAnimSubCommand(TEXT("anim_blueprint_set_preview_animation_blueprint"), PreviewAnimParams, PreviewAnimData, OutError))
					{
						return false;
					}
				}
			}
		}
	}

	{
		FString MembersProxyFolderPath;
		if (!UeAgentAnimBlueprintFolderOps::SaveBlueprintProxyFolder(AssetPath, FolderPath, SettingsJson, true, false, MembersProxyFolderPath, OutError))
		{
			return false;
		}

		TSharedPtr<FJsonObject> ProxyApplyParams = MakeShared<FJsonObject>();
		ProxyApplyParams->SetStringField(TEXT("asset_path"), AssetPath);
		ProxyApplyParams->SetBoolField(TEXT("create_if_missing"), false);
		ProxyApplyParams->SetBoolField(TEXT("compile_after_apply"), false);
		ProxyApplyParams->SetBoolField(TEXT("save_after_apply"), false);
		TSharedPtr<FJsonObject> ProxyApplyData;
		if (!InvokeBlueprintSubCommand(TEXT("blueprint_apply_folder"), ProxyApplyParams, ProxyApplyData, OutError))
		{
			OutError = FString::Printf(TEXT("blueprint_members_proxy_apply_failed:%s"), *OutError);
			return false;
		}

		VariablesAdded += static_cast<int32>(ProxyApplyData->GetIntegerField(TEXT("variables_added")));
		VariablesRemoved += static_cast<int32>(ProxyApplyData->GetIntegerField(TEXT("variables_removed")));
		VariableDefaultsUpdated += static_cast<int32>(ProxyApplyData->GetIntegerField(TEXT("variable_defaults_updated")));
		DelegatesAdded += static_cast<int32>(ProxyApplyData->GetIntegerField(TEXT("delegates_added")));
		IFileManager::Get().DeleteDirectory(*MembersProxyFolderPath, false, true);
	}

	TSharedPtr<FJsonObject> AnimLayersJson;
	FString AnimLayersError;
	if (!UeAgentAnimBlueprintFolderOps::LoadOptionalJsonFile(FPaths::Combine(FolderPath, TEXT("anim_layers"), TEXT("layers.json")), AnimLayersJson, AnimLayersError))
	{
		OutError = AnimLayersError;
		return false;
	}
	if (AnimLayersJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* AnimLayerEntries = nullptr;
		if (AnimLayersJson->TryGetArrayField(TEXT("anim_layers"), AnimLayerEntries) && AnimLayerEntries)
		{
			for (const TSharedPtr<FJsonValue>& LayerValue : *AnimLayerEntries)
			{
				const TSharedPtr<FJsonObject>* LayerObjPtr = nullptr;
				if (!LayerValue.IsValid() || !LayerValue->TryGetObject(LayerObjPtr) || !LayerObjPtr || !LayerObjPtr->IsValid())
				{
					AddWarning(TEXT("anim_layer_invalid_item"), TEXT("Skipped anim layer entry because it is not a JSON object."));
					continue;
				}
				FString LayerName;
				if (!UeAgentAnimBlueprintFolderOps::TryGetString(*LayerObjPtr, TEXT("layer_name"), LayerName) || LayerName.IsEmpty())
				{
					AddWarning(TEXT("anim_layer_missing_layer_name"), TEXT("Skipped anim layer entry because layer_name is missing."));
				}
			}
		}
	}
	TSet<FString> DesiredLocalLayers;
	bool bHasRootAnimGraphSpec = false;
	UeAgentAnimBlueprintFolderOps::ExtractDesiredLocalLayers(AnimLayersJson, DesiredLocalLayers, bHasRootAnimGraphSpec);
	if (bHasRootAnimGraphSpec && !UeAgentAnimBlueprintFolderOps::ResolveGraphByPathOrName(AnimBlueprint, TEXT("AnimGraph")))
	{
		OutError = TEXT("root_anim_graph_not_found");
		return false;
	}

	TArray<UeAgentAnimBlueprintFolderOps::FDesiredStateMachine> DesiredStateMachines;
	if (!UeAgentAnimBlueprintFolderOps::ParseDesiredStateMachinesFromFolder(FolderPath, DesiredStateMachines, OutError))
	{
		return false;
	}

	TArray<UeAgentAnimBlueprintFolderOps::FAnimGraphSpec> AnimGraphSpecs;
	{
		TSet<FString> ReferencedGraphFiles;
		UeAgentAnimBlueprintFolderOps::CollectReferencedGraphFiles(AnimLayersJson, DesiredStateMachines, ReferencedGraphFiles);
		if (!UeAgentAnimBlueprintFolderOps::LoadAnimGraphSpecsFromFolder(FolderPath, ReferencedGraphFiles, DesiredStateMachines, AnimGraphSpecs, OutError))
		{
			return false;
		}
	}

	TSet<FString> DesiredInterfaceClasses;
	TMap<FString, bool> PreserveFunctionsOnRemoveByInterface;
	{
		TSharedPtr<FJsonObject> InterfacesJson;
		FString InterfacesError;
		if (!UeAgentAnimBlueprintFolderOps::LoadOptionalJsonFile(FPaths::Combine(FolderPath, TEXT("layer_interfaces"), TEXT("interfaces.json")), InterfacesJson, InterfacesError))
		{
			OutError = InterfacesError;
			return false;
		}
		if (InterfacesJson.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* DesiredInterfacesArray = nullptr;
			if (InterfacesJson->TryGetArrayField(TEXT("implemented_interfaces"), DesiredInterfacesArray) && DesiredInterfacesArray)
			{
				for (const TSharedPtr<FJsonValue>& InterfaceValue : *DesiredInterfacesArray)
				{
					const TSharedPtr<FJsonObject>* InterfaceObjPtr = nullptr;
					if (!InterfaceValue.IsValid() || !InterfaceValue->TryGetObject(InterfaceObjPtr) || !InterfaceObjPtr || !InterfaceObjPtr->IsValid())
					{
						AddWarning(TEXT("layer_interface_invalid_item"), TEXT("Skipped implemented interface entry because it is not a JSON object."));
						continue;
					}

					FString InterfaceClass;
					if (!UeAgentAnimBlueprintFolderOps::TryGetString(*InterfaceObjPtr, TEXT("interface_class"), InterfaceClass) || InterfaceClass.IsEmpty())
					{
						AddWarning(TEXT("layer_interface_missing_interface_class"), TEXT("Skipped implemented interface entry because interface_class is missing."));
						continue;
					}

					bool bPreserveFunctions = false;
					if (!UeAgentAnimBlueprintFolderOps::TryGetBool(*InterfaceObjPtr, TEXT("preserve_functions_on_remove"), bPreserveFunctions))
					{
						UeAgentAnimBlueprintFolderOps::TryGetBool(*InterfaceObjPtr, TEXT("preserve_functions"), bPreserveFunctions);
					}
					DesiredInterfaceClasses.Add(InterfaceClass);
					PreserveFunctionsOnRemoveByInterface.Add(InterfaceClass, bPreserveFunctions);
				}
			}
		}
	}

	AnimBlueprint = UeAgentAnimBlueprintOps::LoadAnimBlueprint(AssetPath);
	if (!AnimBlueprint)
	{
		OutError = TEXT("anim_blueprint_reload_failed");
		return false;
	}

	if (UeAgentAnimBlueprintOps::IsLayerInterfaceBlueprint(AnimBlueprint))
	{
		if (DesiredInterfaceClasses.Num() > 0)
		{
			AddWarning(TEXT("layer_interface_blueprint_ignores_implemented_interfaces"), TEXT("Anim Layer Interface assets cannot implement additional layer interfaces; ignoring layer_interfaces/interfaces.json."));
		}
	}
	else
	{
		TArray<FString> CurrentInterfaceClasses;
		for (const FBPInterfaceDescription& InterfaceDesc : AnimBlueprint->ImplementedInterfaces)
		{
			const UClass* InterfaceClass = InterfaceDesc.Interface.Get();
			if (InterfaceClass && InterfaceClass->IsChildOf(UAnimLayerInterface::StaticClass()))
			{
				CurrentInterfaceClasses.Add(InterfaceClass->GetPathName());
			}
		}
		CurrentInterfaceClasses.Sort();

		for (const FString& CurrentInterfaceClass : CurrentInterfaceClasses)
		{
			if (DesiredInterfaceClasses.Contains(CurrentInterfaceClass))
			{
				continue;
			}

			TSharedPtr<FJsonObject> RemoveParams = MakeShared<FJsonObject>();
			RemoveParams->SetStringField(TEXT("asset_path"), AssetPath);
			RemoveParams->SetStringField(TEXT("interface_class"), CurrentInterfaceClass);
			RemoveParams->SetBoolField(TEXT("preserve_functions"), PreserveFunctionsOnRemoveByInterface.FindRef(CurrentInterfaceClass));
			RemoveParams->SetBoolField(TEXT("compile_after_remove"), false);
			RemoveParams->SetBoolField(TEXT("save_after_remove"), false);
			TSharedPtr<FJsonObject> RemoveData;
			if (!InvokeAnimSubCommand(TEXT("anim_blueprint_remove_layer_interface"), RemoveParams, RemoveData, OutError))
			{
				return false;
			}
			++LayerInterfacesRemoved;
		}

		AnimBlueprint = UeAgentAnimBlueprintOps::LoadAnimBlueprint(AssetPath);
		if (!AnimBlueprint)
		{
			OutError = TEXT("anim_blueprint_reload_failed");
			return false;
		}

		for (const FString& DesiredInterfaceClass : DesiredInterfaceClasses)
		{
			bool bAlreadyImplemented = false;
			for (const FBPInterfaceDescription& InterfaceDesc : AnimBlueprint->ImplementedInterfaces)
			{
				const UClass* InterfaceClass = InterfaceDesc.Interface.Get();
				if (InterfaceClass && InterfaceClass->GetPathName().Equals(DesiredInterfaceClass, ESearchCase::IgnoreCase))
				{
					bAlreadyImplemented = true;
					break;
				}
			}
			if (bAlreadyImplemented)
			{
				continue;
			}

			TSharedPtr<FJsonObject> ImplementParams = MakeShared<FJsonObject>();
			ImplementParams->SetStringField(TEXT("asset_path"), AssetPath);
			ImplementParams->SetStringField(TEXT("interface_class"), DesiredInterfaceClass);
			ImplementParams->SetBoolField(TEXT("compile_after_add"), false);
			ImplementParams->SetBoolField(TEXT("save_after_add"), false);
			TSharedPtr<FJsonObject> ImplementData;
			if (!InvokeAnimSubCommand(TEXT("anim_blueprint_implement_layer_interface"), ImplementParams, ImplementData, OutError))
			{
				return false;
			}
			++LayerInterfacesAdded;
			AnimBlueprint = UeAgentAnimBlueprintOps::LoadAnimBlueprint(AssetPath);
		}
	}

	TArray<FString> CurrentStateMachineNames;
	for (UEdGraph* AnimGraph : UeAgentAnimBlueprintOps::GatherAnimGraphs(AnimBlueprint))
	{
		if (!AnimGraph)
		{
			continue;
		}
		TArray<UAnimGraphNode_StateMachineBase*> StateMachineNodes;
		AnimGraph->GetNodesOfClass<UAnimGraphNode_StateMachineBase>(StateMachineNodes);
		for (UAnimGraphNode_StateMachineBase* StateMachineNode : StateMachineNodes)
		{
			if (StateMachineNode)
			{
				const FString StateMachineName = StateMachineNode->EditorStateMachineGraph ? StateMachineNode->EditorStateMachineGraph->GetName() : StateMachineNode->GetStateMachineName();
				CurrentStateMachineNames.AddUnique(StateMachineName);
			}
		}
	}
	CurrentStateMachineNames.Sort();
	for (const FString& StateMachineName : CurrentStateMachineNames)
	{
		TSharedPtr<FJsonObject> RemoveParams = MakeShared<FJsonObject>();
		RemoveParams->SetStringField(TEXT("asset_path"), AssetPath);
		RemoveParams->SetStringField(TEXT("state_machine_name"), StateMachineName);
		RemoveParams->SetBoolField(TEXT("save_after_remove"), false);
		TSharedPtr<FJsonObject> RemoveData;
		if (!InvokeAnimSubCommand(TEXT("anim_blueprint_remove_state_machine"), RemoveParams, RemoveData, OutError))
		{
			return false;
		}
	}

	AnimBlueprint = UeAgentAnimBlueprintOps::LoadAnimBlueprint(AssetPath);
	if (!AnimBlueprint)
	{
		OutError = TEXT("anim_blueprint_reload_failed");
		return false;
	}

	TArray<FString> CurrentLocalLayers;
	for (UEdGraph* Graph : AnimBlueprint->FunctionGraphs)
	{
		if (!Graph || !UeAgentAnimBlueprintOps::IsAnimLayerGraph(Graph))
		{
			continue;
		}
		if (Graph->GetName().Equals(TEXT("AnimGraph"), ESearchCase::IgnoreCase))
		{
			continue;
		}
		if (Graph->InterfaceGuid.IsValid())
		{
			continue;
		}
		CurrentLocalLayers.Add(Graph->GetName());
	}
	CurrentLocalLayers.Sort();

	for (const FString& CurrentLocalLayer : CurrentLocalLayers)
	{
		if (DesiredLocalLayers.Contains(CurrentLocalLayer))
		{
			continue;
		}

		TSharedPtr<FJsonObject> RemoveParams = MakeShared<FJsonObject>();
		RemoveParams->SetStringField(TEXT("asset_path"), AssetPath);
		RemoveParams->SetStringField(TEXT("layer_name"), CurrentLocalLayer);
		RemoveParams->SetBoolField(TEXT("compile_after_remove"), false);
		RemoveParams->SetBoolField(TEXT("save_after_remove"), false);
		TSharedPtr<FJsonObject> RemoveData;
		if (!InvokeAnimSubCommand(TEXT("anim_blueprint_remove_anim_layer"), RemoveParams, RemoveData, OutError))
		{
			return false;
		}
		++LocalLayersRemoved;
	}

	AnimBlueprint = UeAgentAnimBlueprintOps::LoadAnimBlueprint(AssetPath);
	if (!AnimBlueprint)
	{
		OutError = TEXT("anim_blueprint_reload_failed");
		return false;
	}

	for (const FString& DesiredLocalLayer : DesiredLocalLayers)
	{
		if (UeAgentAnimBlueprintFolderOps::ResolveGraphByPathOrName(AnimBlueprint, DesiredLocalLayer))
		{
			continue;
		}

		TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
		AddParams->SetStringField(TEXT("asset_path"), AssetPath);
		AddParams->SetStringField(TEXT("layer_name"), DesiredLocalLayer);
		AddParams->SetBoolField(TEXT("compile_after_add"), false);
		AddParams->SetBoolField(TEXT("save_after_add"), false);
		TSharedPtr<FJsonObject> AddData;
		if (!InvokeAnimSubCommand(TEXT("anim_blueprint_add_anim_layer"), AddParams, AddData, OutError))
		{
			return false;
		}
		++LocalLayersAdded;
		AnimBlueprint = UeAgentAnimBlueprintOps::LoadAnimBlueprint(AssetPath);
	}

	if (UeAgentAnimBlueprintOps::IsLayerInterfaceBlueprint(AnimBlueprint) && DesiredStateMachines.Num() > 0)
	{
		OutError = TEXT("layer_interface_blueprint_does_not_support_state_machines");
		return false;
	}

	for (const UeAgentAnimBlueprintFolderOps::FDesiredStateMachine& MachineSpec : DesiredStateMachines)
	{
		TSharedPtr<FJsonObject> AddMachineParams = MakeShared<FJsonObject>();
		AddMachineParams->SetStringField(TEXT("asset_path"), AssetPath);
		AddMachineParams->SetStringField(TEXT("state_machine_name"), MachineSpec.StateMachineName);
		AddMachineParams->SetStringField(TEXT("anim_graph_name"), MachineSpec.OwnerAnimGraphName);
		AddMachineParams->SetNumberField(TEXT("pos_x"), MachineSpec.PosX);
		AddMachineParams->SetNumberField(TEXT("pos_y"), MachineSpec.PosY);
		AddMachineParams->SetBoolField(TEXT("compile_after_add"), false);
		AddMachineParams->SetBoolField(TEXT("save_after_add"), false);
		TSharedPtr<FJsonObject> AddMachineData;
		if (!InvokeAnimSubCommand(TEXT("anim_blueprint_add_state_machine"), AddMachineParams, AddMachineData, OutError))
		{
			return false;
		}

		for (const UeAgentAnimBlueprintFolderOps::FDesiredStateNode& NodeSpec : MachineSpec.Nodes)
		{
			TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
			NodeParams->SetStringField(TEXT("asset_path"), AssetPath);
			NodeParams->SetStringField(TEXT("state_machine_name"), MachineSpec.StateMachineName);
			NodeParams->SetNumberField(TEXT("pos_x"), NodeSpec.PosX);
			NodeParams->SetNumberField(TEXT("pos_y"), NodeSpec.PosY);
			NodeParams->SetBoolField(TEXT("compile_after_add"), false);
			NodeParams->SetBoolField(TEXT("save_after_add"), false);
			TSharedPtr<FJsonObject> NodeData;

			if (NodeSpec.NodeKind.Equals(TEXT("state"), ESearchCase::IgnoreCase))
			{
				NodeParams->SetStringField(TEXT("state_name"), NodeSpec.StateName);
				NodeParams->SetStringField(TEXT("state_type"), NodeSpec.StateType);
				NodeParams->SetBoolField(TEXT("always_reset_on_entry"), NodeSpec.bAlwaysResetOnEntry);
				if (!InvokeAnimSubCommand(TEXT("anim_blueprint_add_state"), NodeParams, NodeData, OutError))
				{
					return false;
				}
			}
			else if (NodeSpec.NodeKind.Equals(TEXT("conduit"), ESearchCase::IgnoreCase))
			{
				NodeParams->SetStringField(TEXT("conduit_name"), NodeSpec.StateName);
				if (!InvokeAnimSubCommand(TEXT("anim_blueprint_add_conduit"), NodeParams, NodeData, OutError))
				{
					return false;
				}
			}
			else if (NodeSpec.NodeKind.Equals(TEXT("alias"), ESearchCase::IgnoreCase))
			{
				NodeParams->SetStringField(TEXT("alias_name"), NodeSpec.StateName);
				NodeParams->SetBoolField(TEXT("global_alias"), NodeSpec.bGlobalAlias);
				TArray<TSharedPtr<FJsonValue>> AliasTargets;
				for (const FString& AliasTargetState : NodeSpec.AliasTargetStates)
				{
					AliasTargets.Add(MakeShared<FJsonValueString>(AliasTargetState));
				}
				NodeParams->SetArrayField(TEXT("alias_target_states"), AliasTargets);
				if (!InvokeAnimSubCommand(TEXT("anim_blueprint_add_state_alias"), NodeParams, NodeData, OutError))
				{
					return false;
				}
			}
			else
			{
				OutError = TEXT("unsupported_state_node_kind");
				return false;
			}

			++StateNodesAdded;
		}

		for (const UeAgentAnimBlueprintFolderOps::FDesiredTransition& TransitionSpec : MachineSpec.Transitions)
		{
			FString SourceStateName;
			FString TargetStateName;
			for (const UeAgentAnimBlueprintFolderOps::FDesiredStateNode& NodeSpec : MachineSpec.Nodes)
			{
				if (NodeSpec.Id == TransitionSpec.SourceNodeId)
				{
					SourceStateName = NodeSpec.StateName;
				}
				if (NodeSpec.Id == TransitionSpec.TargetNodeId)
				{
					TargetStateName = NodeSpec.StateName;
				}
			}
			if (SourceStateName.IsEmpty() || TargetStateName.IsEmpty())
			{
				OutError = TEXT("transition_endpoint_not_found");
				return false;
			}

			TSharedPtr<FJsonObject> AddTransitionParams = MakeShared<FJsonObject>();
			AddTransitionParams->SetStringField(TEXT("asset_path"), AssetPath);
			AddTransitionParams->SetStringField(TEXT("state_machine_name"), MachineSpec.StateMachineName);
			AddTransitionParams->SetStringField(TEXT("source_state_name"), SourceStateName);
			AddTransitionParams->SetStringField(TEXT("target_state_name"), TargetStateName);
			AddTransitionParams->SetNumberField(TEXT("pos_x"), TransitionSpec.PosX);
			AddTransitionParams->SetNumberField(TEXT("pos_y"), TransitionSpec.PosY);
			AddTransitionParams->SetNumberField(TEXT("priority_order"), TransitionSpec.PriorityOrder);
			AddTransitionParams->SetNumberField(TEXT("crossfade_duration"), TransitionSpec.CrossfadeDuration);
			AddTransitionParams->SetBoolField(TEXT("bidirectional"), TransitionSpec.bBidirectional);
			AddTransitionParams->SetBoolField(TEXT("disabled"), TransitionSpec.bDisabled);
			AddTransitionParams->SetBoolField(TEXT("automatic_rule"), TransitionSpec.bAutomaticRule);
			AddTransitionParams->SetBoolField(TEXT("compile_after_add"), false);
			AddTransitionParams->SetBoolField(TEXT("save_after_add"), false);
			TSharedPtr<FJsonObject> AddTransitionData;
			if (!InvokeAnimSubCommand(TEXT("anim_blueprint_add_transition"), AddTransitionParams, AddTransitionData, OutError))
			{
				return false;
			}

			++TransitionsAdded;
		}

		if (!MachineSpec.EntryStateName.IsEmpty())
		{
			TSharedPtr<FJsonObject> EntryParams = MakeShared<FJsonObject>();
			EntryParams->SetStringField(TEXT("asset_path"), AssetPath);
			EntryParams->SetStringField(TEXT("state_machine_name"), MachineSpec.StateMachineName);
			EntryParams->SetStringField(TEXT("state_name"), MachineSpec.EntryStateName);
			EntryParams->SetBoolField(TEXT("compile_after_set"), false);
			EntryParams->SetBoolField(TEXT("save_after_set"), false);
			TSharedPtr<FJsonObject> EntryData;
			if (!InvokeAnimSubCommand(TEXT("anim_blueprint_set_entry_state"), EntryParams, EntryData, OutError))
			{
				return false;
			}
		}
		else
		{
			TSharedPtr<FJsonObject> ClearEntryParams = MakeShared<FJsonObject>();
			ClearEntryParams->SetStringField(TEXT("asset_path"), AssetPath);
			ClearEntryParams->SetStringField(TEXT("state_machine_name"), MachineSpec.StateMachineName);
			ClearEntryParams->SetBoolField(TEXT("compile_after_clear"), false);
			ClearEntryParams->SetBoolField(TEXT("save_after_clear"), false);
			TSharedPtr<FJsonObject> ClearEntryData;
			if (!InvokeAnimSubCommand(TEXT("anim_blueprint_clear_entry_state"), ClearEntryParams, ClearEntryData, OutError))
			{
				return false;
			}
		}

		++StateMachinesRebuilt;
	}

	{
		FString LogicGraphsProxyFolderPath;
		if (!UeAgentAnimBlueprintFolderOps::SaveBlueprintProxyFolder(AssetPath, FolderPath, SettingsJson, false, true, LogicGraphsProxyFolderPath, OutError))
		{
			return false;
		}

		TSharedPtr<FJsonObject> ProxyApplyParams = MakeShared<FJsonObject>();
		ProxyApplyParams->SetStringField(TEXT("asset_path"), AssetPath);
		ProxyApplyParams->SetBoolField(TEXT("create_if_missing"), false);
		ProxyApplyParams->SetBoolField(TEXT("compile_after_apply"), false);
		ProxyApplyParams->SetBoolField(TEXT("save_after_apply"), false);
		TSharedPtr<FJsonObject> ProxyApplyData;
		if (!InvokeBlueprintSubCommand(TEXT("blueprint_apply_folder"), ProxyApplyParams, ProxyApplyData, OutError))
		{
			OutError = FString::Printf(TEXT("blueprint_logic_graph_proxy_apply_failed:%s"), *OutError);
			return false;
		}

		LogicGraphsRebuilt += static_cast<int32>(ProxyApplyData->GetIntegerField(TEXT("graphs_rebuilt")));
		NodesCreated += static_cast<int32>(ProxyApplyData->GetIntegerField(TEXT("nodes_created")));
		EdgesCreated += static_cast<int32>(ProxyApplyData->GetIntegerField(TEXT("edges_created")));
		PinDefaultsApplied += static_cast<int32>(ProxyApplyData->GetIntegerField(TEXT("pin_defaults_applied")));
		IFileManager::Get().DeleteDirectory(*LogicGraphsProxyFolderPath, false, true);
	}

	auto ResolveBuiltinAnimNodeIds = [](UEdGraph* Graph, TMap<FString, FString>& OutBuiltinIds)
	{
		OutBuiltinIds.Reset();
		if (!Graph)
		{
			return;
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node || !UeAgentAnimBlueprintFolderOps::IsBuiltinAnimGraphNode(Node))
			{
				continue;
			}
			const FString BuiltinId = UeAgentAnimBlueprintFolderOps::GetBuiltinAnimGraphNodeId(Node);
			if (!BuiltinId.IsEmpty())
			{
				OutBuiltinIds.Add(BuiltinId, Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
			}
		}
	};

	auto ResolveReusableAnimNodeIds = [](UEdGraph* Graph, const UeAgentAnimBlueprintFolderOps::FAnimGraphSpec& GraphSpec, TMap<FString, FString>& OutReusableNodeIds, TSet<const UEdGraphNode*>& OutPreservedNodes)
	{
		OutReusableNodeIds.Reset();
		OutPreservedNodes.Reset();
		if (!Graph || !GraphSpec.Root.IsValid())
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* NodesArray = nullptr;
		if (!GraphSpec.Root->TryGetArrayField(TEXT("nodes"), NodesArray) || !NodesArray)
		{
			return;
		}

		TArray<UAnimGraphNode_StateMachineBase*> ExistingStateMachineNodes;
		Graph->GetNodesOfClass<UAnimGraphNode_StateMachineBase>(ExistingStateMachineNodes);

		auto FindStateMachineNodeByName = [&ExistingStateMachineNodes](const FString& DesiredStateMachineName) -> UAnimGraphNode_StateMachineBase*
		{
			for (UAnimGraphNode_StateMachineBase* StateMachineNode : ExistingStateMachineNodes)
			{
				if (!StateMachineNode)
				{
					continue;
				}

				const FString CurrentStateMachineName =
					StateMachineNode->EditorStateMachineGraph
						? StateMachineNode->EditorStateMachineGraph->GetName()
						: StateMachineNode->GetStateMachineName();
				if (CurrentStateMachineName.Equals(DesiredStateMachineName, ESearchCase::IgnoreCase))
				{
					return StateMachineNode;
				}
			}
			return nullptr;
		};

		for (const TSharedPtr<FJsonValue>& NodeValue : *NodesArray)
		{
			const TSharedPtr<FJsonObject>* NodeObjPtr = nullptr;
			if (!NodeValue.IsValid() || !NodeValue->TryGetObject(NodeObjPtr) || !NodeObjPtr || !NodeObjPtr->IsValid())
			{
				continue;
			}
			const TSharedPtr<FJsonObject>& NodeObj = *NodeObjPtr;

			FString NodeId;
			FString NodeKind;
			if (!UeAgentAnimBlueprintFolderOps::TryGetString(NodeObj, TEXT("id"), NodeId) || NodeId.IsEmpty())
			{
				continue;
			}
			if (!UeAgentAnimBlueprintFolderOps::TryGetString(NodeObj, TEXT("node_kind"), NodeKind) || !NodeKind.Equals(TEXT("node_by_class"), ESearchCase::IgnoreCase))
			{
				continue;
			}

			FString NodeClassPath;
			if (!UeAgentAnimBlueprintFolderOps::TryGetString(NodeObj, TEXT("node_class"), NodeClassPath) || NodeClassPath.IsEmpty())
			{
				continue;
			}

			UClass* NodeClass = FindObject<UClass>(nullptr, *NodeClassPath);
			if (!NodeClass)
			{
				NodeClass = LoadObject<UClass>(nullptr, *NodeClassPath);
			}
			if (!NodeClass || !NodeClass->IsChildOf(UAnimGraphNode_StateMachineBase::StaticClass()))
			{
				continue;
			}

			FString DesiredStateMachineName;
			UeAgentAnimBlueprintFolderOps::TryGetString(NodeObj, TEXT("state_machine_name"), DesiredStateMachineName);

			UAnimGraphNode_StateMachineBase* MatchedStateMachineNode = nullptr;
			if (!DesiredStateMachineName.IsEmpty())
			{
				MatchedStateMachineNode = FindStateMachineNodeByName(DesiredStateMachineName);
			}
			if (!MatchedStateMachineNode && ExistingStateMachineNodes.Num() == 1)
			{
				MatchedStateMachineNode = ExistingStateMachineNodes[0];
			}
			if (!MatchedStateMachineNode)
			{
				continue;
			}

			OutReusableNodeIds.Add(NodeId, MatchedStateMachineNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
			OutPreservedNodes.Add(MatchedStateMachineNode);
		}
	};

	auto ResolveGraphFromStructuredContext = [this, &InvokeAnimSubCommand, &AssetPath](UAnimBlueprint* WorkingAnimBlueprint, const UeAgentAnimBlueprintFolderOps::FAnimGraphSpec& GraphSpec, FString& OutResolveError) -> UEdGraph*
	{
		if (!WorkingAnimBlueprint || GraphSpec.StateMachineName.IsEmpty())
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> ListParams = MakeShared<FJsonObject>();
		ListParams->SetStringField(TEXT("asset_path"), AssetPath);
		TSharedPtr<FJsonObject> ListData;
		if (!InvokeAnimSubCommand(TEXT("anim_blueprint_list_state_machines"), ListParams, ListData, OutResolveError))
		{
			return nullptr;
		}

		const TArray<TSharedPtr<FJsonValue>>* StateMachinesArray = nullptr;
		if (!ListData->TryGetArrayField(TEXT("state_machines"), StateMachinesArray) || !StateMachinesArray)
		{
			OutResolveError = TEXT("state_machine_list_missing");
			return nullptr;
		}

		for (const TSharedPtr<FJsonValue>& StateMachineValue : *StateMachinesArray)
		{
			const TSharedPtr<FJsonObject>* StateMachineObjPtr = nullptr;
			if (!StateMachineValue.IsValid() || !StateMachineValue->TryGetObject(StateMachineObjPtr) || !StateMachineObjPtr || !StateMachineObjPtr->IsValid())
			{
				continue;
			}

			FString StateMachineName;
			UeAgentAnimBlueprintFolderOps::TryGetString(*StateMachineObjPtr, TEXT("state_machine_name"), StateMachineName);
			if (!StateMachineName.Equals(GraphSpec.StateMachineName, ESearchCase::IgnoreCase))
			{
				continue;
			}

			if (GraphSpec.GraphKind.Equals(TEXT("state_graph"), ESearchCase::IgnoreCase) && !GraphSpec.StateName.IsEmpty())
			{
				const TArray<TSharedPtr<FJsonValue>>* StateNodesArray = nullptr;
				if (!(*StateMachineObjPtr)->TryGetArrayField(TEXT("state_nodes"), StateNodesArray) || !StateNodesArray)
				{
					break;
				}

				for (const TSharedPtr<FJsonValue>& StateNodeValue : *StateNodesArray)
				{
					const TSharedPtr<FJsonObject>* StateNodeObjPtr = nullptr;
					if (!StateNodeValue.IsValid() || !StateNodeValue->TryGetObject(StateNodeObjPtr) || !StateNodeObjPtr || !StateNodeObjPtr->IsValid())
					{
						continue;
					}

					FString StateName;
					FString BoundGraphPath;
					UeAgentAnimBlueprintFolderOps::TryGetString(*StateNodeObjPtr, TEXT("state_name"), StateName);
					UeAgentAnimBlueprintFolderOps::TryGetString(*StateNodeObjPtr, TEXT("bound_graph_path"), BoundGraphPath);
					if (StateName.Equals(GraphSpec.StateName, ESearchCase::IgnoreCase) && !BoundGraphPath.IsEmpty())
					{
						return UeAgentAnimBlueprintFolderOps::ResolveGraphByPathOrName(WorkingAnimBlueprint, BoundGraphPath);
					}
				}
			}

			if (GraphSpec.GraphKind.Equals(TEXT("transition_graph"), ESearchCase::IgnoreCase)
				&& !GraphSpec.SourceStateName.IsEmpty()
				&& !GraphSpec.TargetStateName.IsEmpty())
			{
				const TArray<TSharedPtr<FJsonValue>>* TransitionNodesArray = nullptr;
				if (!(*StateMachineObjPtr)->TryGetArrayField(TEXT("transition_nodes"), TransitionNodesArray) || !TransitionNodesArray)
				{
					break;
				}

				for (const TSharedPtr<FJsonValue>& TransitionValue : *TransitionNodesArray)
				{
					const TSharedPtr<FJsonObject>* TransitionObjPtr = nullptr;
					if (!TransitionValue.IsValid() || !TransitionValue->TryGetObject(TransitionObjPtr) || !TransitionObjPtr || !TransitionObjPtr->IsValid())
					{
						continue;
					}

					FString SourceStateName;
					FString TargetStateName;
					FString BoundGraphPath;
					UeAgentAnimBlueprintFolderOps::TryGetString(*TransitionObjPtr, TEXT("source_state_name"), SourceStateName);
					UeAgentAnimBlueprintFolderOps::TryGetString(*TransitionObjPtr, TEXT("target_state_name"), TargetStateName);
					UeAgentAnimBlueprintFolderOps::TryGetString(*TransitionObjPtr, TEXT("bound_graph_path"), BoundGraphPath);
					if (SourceStateName.Equals(GraphSpec.SourceStateName, ESearchCase::IgnoreCase)
						&& TargetStateName.Equals(GraphSpec.TargetStateName, ESearchCase::IgnoreCase)
						&& !BoundGraphPath.IsEmpty())
					{
						return UeAgentAnimBlueprintFolderOps::ResolveGraphByPathOrName(WorkingAnimBlueprint, BoundGraphPath);
					}
				}
			}

			break;
		}

		return nullptr;
	};

	auto ApplySingleAnimGraph = [this, &InvokeAnimSubCommand, &AssetPath, &OutError, &AnimGraphsRebuilt, &NodesCreated, &EdgesCreated, &PinDefaultsApplied, &ResolveBuiltinAnimNodeIds, &ResolveReusableAnimNodeIds, &ResolveGraphFromStructuredContext, &AddWarning](const UeAgentAnimBlueprintFolderOps::FAnimGraphSpec& GraphSpec) -> bool
	{
		UAnimBlueprint* WorkingAnimBlueprint = UeAgentAnimBlueprintOps::LoadAnimBlueprint(AssetPath);
		if (!WorkingAnimBlueprint)
		{
			OutError = TEXT("anim_blueprint_not_found");
			return false;
		}

		UEdGraph* Graph = UeAgentAnimBlueprintFolderOps::ResolveGraphByPathOrName(WorkingAnimBlueprint, GraphSpec.GraphRef);
		if (!Graph)
		{
			FString ResolveGraphError;
			Graph = ResolveGraphFromStructuredContext(WorkingAnimBlueprint, GraphSpec, ResolveGraphError);
		}
		if (!Graph)
		{
			if (!GraphSpec.FileRef.IsEmpty())
			{
				OutError = FString::Printf(TEXT("graph_not_found:%s"), *GraphSpec.FileRef);
			}
			else
			{
			OutError = TEXT("graph_not_found");
			}
			return false;
		}

		TMap<FString, FString> BuiltinNodeIds;
		ResolveBuiltinAnimNodeIds(Graph, BuiltinNodeIds);
		TMap<FString, FString> ReusableNodeIds;
		TSet<const UEdGraphNode*> PreservedNodes;
		ResolveReusableAnimNodeIds(Graph, GraphSpec, ReusableNodeIds, PreservedNodes);

		TArray<UEdGraphNode*> ExistingNodes = Graph->Nodes;
		for (UEdGraphNode* ExistingNode : ExistingNodes)
		{
			if (!ExistingNode || UeAgentAnimBlueprintFolderOps::IsBuiltinAnimGraphNode(ExistingNode) || PreservedNodes.Contains(ExistingNode))
			{
				continue;
			}
			Graph->RemoveNode(ExistingNode);
		}
		FBlueprintEditorUtils::MarkBlueprintAsModified(WorkingAnimBlueprint);

		TMap<FString, FString> NodeIdToGuid = BuiltinNodeIds;
		for (const TPair<FString, FString>& Pair : ReusableNodeIds)
		{
			NodeIdToGuid.Add(Pair.Key, Pair.Value);
		}
		const TArray<TSharedPtr<FJsonValue>>* NodesArray = nullptr;
		if (GraphSpec.Root->TryGetArrayField(TEXT("nodes"), NodesArray) && NodesArray)
		{
			for (const TSharedPtr<FJsonValue>& NodeValue : *NodesArray)
			{
				const TSharedPtr<FJsonObject>* NodeObjPtr = nullptr;
				if (!NodeValue.IsValid() || !NodeValue->TryGetObject(NodeObjPtr) || !NodeObjPtr || !NodeObjPtr->IsValid())
				{
					continue;
				}
				const TSharedPtr<FJsonObject>& NodeObj = *NodeObjPtr;

				FString NodeId;
				FString NodeKind;
				if (!UeAgentAnimBlueprintFolderOps::TryGetString(NodeObj, TEXT("id"), NodeId) || NodeId.IsEmpty())
				{
					OutError = TEXT("missing_node_id");
					return false;
				}
				if (!UeAgentAnimBlueprintFolderOps::TryGetString(NodeObj, TEXT("node_kind"), NodeKind))
				{
					UeAgentAnimBlueprintFolderOps::TryGetString(NodeObj, TEXT("node_type"), NodeKind);
				}
				if (NodeKind.IsEmpty())
				{
					OutError = TEXT("missing_node_kind");
					return false;
				}

				if (NodeIdToGuid.Contains(NodeId))
				{
					continue;
				}

				double PosX = 0.0;
				double PosY = 0.0;
				TSharedPtr<FJsonObject> PosObj;
				if (UeAgentAnimBlueprintFolderOps::TryGetObjectField(NodeObj, TEXT("pos"), PosObj))
				{
					UeAgentAnimBlueprintFolderOps::TryGetNumber(PosObj, TEXT("x"), PosX);
					UeAgentAnimBlueprintFolderOps::TryGetNumber(PosObj, TEXT("y"), PosY);
				}

				TSharedPtr<FJsonObject> AddNodeParams = MakeShared<FJsonObject>();
				AddNodeParams->SetStringField(TEXT("asset_path"), AssetPath);
				AddNodeParams->SetStringField(TEXT("graph_name"), Graph->GetPathName());
				AddNodeParams->SetNumberField(TEXT("pos_x"), PosX);
				AddNodeParams->SetNumberField(TEXT("pos_y"), PosY);
				TSharedPtr<FJsonObject> AddNodeData;

				if (NodeKind.Equals(TEXT("event"), ESearchCase::IgnoreCase))
				{
					FString EventName;
					FString EventClass = TEXT("/Script/Engine.Actor");
					if (!UeAgentAnimBlueprintFolderOps::TryGetString(NodeObj, TEXT("event_name"), EventName) || EventName.IsEmpty())
					{
						OutError = TEXT("missing_event_name");
						return false;
					}
					UeAgentAnimBlueprintFolderOps::TryGetString(NodeObj, TEXT("event_class"), EventClass);
					AddNodeParams->SetStringField(TEXT("event_name"), EventName);
					AddNodeParams->SetStringField(TEXT("event_class"), EventClass);
					AddNodeParams->SetBoolField(TEXT("compile_after_add"), false);
					AddNodeParams->SetBoolField(TEXT("save_after_add"), false);
					if (!InvokeAnimSubCommand(TEXT("anim_blueprint_add_event_node"), AddNodeParams, AddNodeData, OutError))
					{
						return false;
					}
				}
				else if (NodeKind.Equals(TEXT("custom_event"), ESearchCase::IgnoreCase))
				{
					FString EventName;
					if (!UeAgentAnimBlueprintFolderOps::TryGetString(NodeObj, TEXT("event_name"), EventName) || EventName.IsEmpty())
					{
						OutError = TEXT("missing_event_name");
						return false;
					}
					AddNodeParams->SetStringField(TEXT("event_name"), EventName);
					AddNodeParams->SetBoolField(TEXT("compile_after_add"), false);
					AddNodeParams->SetBoolField(TEXT("save_after_add"), false);
					if (!InvokeAnimSubCommand(TEXT("anim_blueprint_add_custom_event_node"), AddNodeParams, AddNodeData, OutError))
					{
						return false;
					}
				}
				else if (NodeKind.Equals(TEXT("call_function"), ESearchCase::IgnoreCase))
				{
					FString FunctionOwnerClass;
					FString FunctionName;
					if (!UeAgentAnimBlueprintFolderOps::TryGetString(NodeObj, TEXT("function_owner_class"), FunctionOwnerClass) || FunctionOwnerClass.IsEmpty())
					{
						OutError = TEXT("missing_function_owner_class");
						return false;
					}
					if (!UeAgentAnimBlueprintFolderOps::TryGetString(NodeObj, TEXT("function_name"), FunctionName) || FunctionName.IsEmpty())
					{
						OutError = TEXT("missing_function_name");
						return false;
					}
					AddNodeParams->SetStringField(TEXT("function_owner_class"), FunctionOwnerClass);
					AddNodeParams->SetStringField(TEXT("function_name"), FunctionName);
					AddNodeParams->SetBoolField(TEXT("compile_after_add"), false);
					AddNodeParams->SetBoolField(TEXT("save_after_add"), false);
					if (!InvokeAnimSubCommand(TEXT("anim_blueprint_add_call_function_node"), AddNodeParams, AddNodeData, OutError))
					{
						return false;
					}
				}
				else if (NodeKind.Equals(TEXT("variable_node"), ESearchCase::IgnoreCase))
				{
					FString VariableName;
					FString Access = TEXT("get");
					if (!UeAgentAnimBlueprintFolderOps::TryGetString(NodeObj, TEXT("variable_name"), VariableName) || VariableName.IsEmpty())
					{
						OutError = TEXT("missing_variable_name");
						return false;
					}
					UeAgentAnimBlueprintFolderOps::TryGetString(NodeObj, TEXT("access"), Access);
					AddNodeParams->SetStringField(TEXT("variable_name"), VariableName);
					AddNodeParams->SetStringField(TEXT("node_type"), Access);
					AddNodeParams->SetBoolField(TEXT("compile_after_add"), false);
					AddNodeParams->SetBoolField(TEXT("save_after_add"), false);
					if (!InvokeAnimSubCommand(TEXT("anim_blueprint_add_variable_node"), AddNodeParams, AddNodeData, OutError))
					{
						return false;
					}
				}
				else if (NodeKind.Equals(TEXT("node_by_class"), ESearchCase::IgnoreCase))
				{
					FString NodeClass;
					if (!UeAgentAnimBlueprintFolderOps::TryGetString(NodeObj, TEXT("node_class"), NodeClass) || NodeClass.IsEmpty())
					{
						OutError = TEXT("missing_node_class");
						return false;
					}
					AddNodeParams->SetStringField(TEXT("node_class"), NodeClass);
					AddNodeParams->SetBoolField(TEXT("compile_after_add"), false);
					AddNodeParams->SetBoolField(TEXT("save_after_add"), false);
					if (!InvokeAnimSubCommand(TEXT("anim_blueprint_add_node_by_class"), AddNodeParams, AddNodeData, OutError))
					{
						return false;
					}
				}
				else
				{
					OutError = TEXT("unsupported_graph_node_kind");
					return false;
				}

				const FString NodeGuid = AddNodeData->GetStringField(TEXT("node_guid"));
				NodeIdToGuid.Add(NodeId, NodeGuid);
				++NodesCreated;
			}

			for (const TSharedPtr<FJsonValue>& NodeValue : *NodesArray)
			{
				const TSharedPtr<FJsonObject>* NodeObjPtr = nullptr;
				if (!NodeValue.IsValid() || !NodeValue->TryGetObject(NodeObjPtr) || !NodeObjPtr || !NodeObjPtr->IsValid())
				{
					continue;
				}
				const TSharedPtr<FJsonObject>& NodeObj = *NodeObjPtr;

				FString NodeId;
				if (!UeAgentAnimBlueprintFolderOps::TryGetString(NodeObj, TEXT("id"), NodeId) || NodeId.IsEmpty())
				{
					continue;
				}

				const FString* NodeGuid = NodeIdToGuid.Find(NodeId);
				if (!NodeGuid)
				{
					continue;
				}

				UEdGraphNode* CurrentNode = UeAgentAnimBlueprintFolderOps::FindGraphNodeByGuidText(Graph, *NodeGuid);
				const TArray<TSharedPtr<FJsonValue>>* PropertiesArray = nullptr;
				if (CurrentNode && NodeObj->TryGetArrayField(TEXT("properties"), PropertiesArray) && PropertiesArray)
				{
					bool bNodeReconstructed = false;
					for (const TSharedPtr<FJsonValue>& PropertyValue : *PropertiesArray)
					{
						const TSharedPtr<FJsonObject>* PropertyObjPtr = nullptr;
						if (!PropertyValue.IsValid() || !PropertyValue->TryGetObject(PropertyObjPtr) || !PropertyObjPtr || !PropertyObjPtr->IsValid())
						{
							continue;
						}

						FString PropertyName;
						FString ValueText;
						if (!UeAgentAnimBlueprintFolderOps::TryGetString(*PropertyObjPtr, TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
						{
							continue;
						}
						if (!UeAgentAnimBlueprintFolderOps::TryGetString(*PropertyObjPtr, TEXT("value_text"), ValueText))
						{
							continue;
						}

						FString AppliedValueText;
						FString CppType;
						FString ImportError;
						if (!UeAgentAnimBlueprintFolderOps::SetObjectPropertyText(CurrentNode, PropertyName, ValueText, &AppliedValueText, &CppType, &ImportError))
						{
							OutError = ImportError.IsEmpty()
								? FString::Printf(TEXT("set_node_property_failed:%s:%s:%s:%s"), *Graph->GetName(), *NodeId, *PropertyName, *ValueText)
								: FString::Printf(TEXT("set_node_property_failed:%s:%s:%s:%s:%s"), *Graph->GetName(), *NodeId, *PropertyName, *ValueText, *ImportError);
							return false;
						}
						if (!AppliedValueText.Equals(ValueText, ESearchCase::CaseSensitive))
						{
							AddWarning(
								TEXT("node_property_value_changed_after_import"),
								FString::Printf(TEXT("Node property '%s' on '%s/%s' read back a different value. requested='%s' applied='%s' cpp_type='%s'."),
									*PropertyName,
									*Graph->GetName(),
									*NodeId,
									*ValueText,
									*AppliedValueText,
									*CppType));
						}

						bNodeReconstructed = true;
					}

					if (bNodeReconstructed)
					{
						CurrentNode->ReconstructNode();
						FBlueprintEditorUtils::MarkBlueprintAsModified(WorkingAnimBlueprint);
					}
				}

				const TArray<TSharedPtr<FJsonValue>>* PinDefaultsArray = nullptr;
				if (!NodeObj->TryGetArrayField(TEXT("pin_defaults"), PinDefaultsArray) || !PinDefaultsArray)
				{
					continue;
				}

				for (const TSharedPtr<FJsonValue>& PinDefaultValue : *PinDefaultsArray)
				{
					const TSharedPtr<FJsonObject>* PinDefaultObjPtr = nullptr;
					if (!PinDefaultValue.IsValid() || !PinDefaultValue->TryGetObject(PinDefaultObjPtr) || !PinDefaultObjPtr || !PinDefaultObjPtr->IsValid())
					{
						continue;
					}

					FString PinName;
					FString DefaultValue;
					if (!UeAgentAnimBlueprintFolderOps::TryGetString(*PinDefaultObjPtr, TEXT("pin_name"), PinName) || PinName.IsEmpty())
					{
						continue;
					}
					if (!UeAgentAnimBlueprintFolderOps::TryGetString(*PinDefaultObjPtr, TEXT("default_value"), DefaultValue))
					{
						continue;
					}

					TSharedPtr<FJsonObject> PinParams = MakeShared<FJsonObject>();
					PinParams->SetStringField(TEXT("asset_path"), AssetPath);
					PinParams->SetStringField(TEXT("graph_name"), Graph->GetPathName());
					PinParams->SetStringField(TEXT("node_guid"), *NodeGuid);
					PinParams->SetStringField(TEXT("pin_name"), PinName);
					PinParams->SetStringField(TEXT("default_value"), DefaultValue);
					PinParams->SetBoolField(TEXT("compile_after_set"), false);
					PinParams->SetBoolField(TEXT("save_after_set"), false);
					TSharedPtr<FJsonObject> PinData;
					if (!InvokeAnimSubCommand(TEXT("anim_blueprint_set_pin_default_value"), PinParams, PinData, OutError))
					{
						if (OutError == TEXT("pin_not_found"))
						{
							AddWarning(
								TEXT("pin_default_skipped"),
								FString::Printf(TEXT("Skipped missing pin default while applying graph '%s': node='%s' pin='%s'."), *Graph->GetName(), *NodeId, *PinName));
							continue;
						}
						OutError = FString::Printf(TEXT("set_pin_default_failed:%s:%s:%s"), *Graph->GetName(), *NodeId, *PinName);
						return false;
					}
					++PinDefaultsApplied;
				}
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* EdgesArray = nullptr;
		if (GraphSpec.Root->TryGetArrayField(TEXT("edges"), EdgesArray) && EdgesArray)
		{
			for (const TSharedPtr<FJsonValue>& EdgeValue : *EdgesArray)
			{
				const TSharedPtr<FJsonObject>* EdgeObjPtr = nullptr;
				if (!EdgeValue.IsValid() || !EdgeValue->TryGetObject(EdgeObjPtr) || !EdgeObjPtr || !EdgeObjPtr->IsValid())
				{
					continue;
				}

				TSharedPtr<FJsonObject> FromObj;
				TSharedPtr<FJsonObject> ToObj;
				if (!UeAgentAnimBlueprintFolderOps::TryGetObjectField(*EdgeObjPtr, TEXT("from"), FromObj)
					|| !UeAgentAnimBlueprintFolderOps::TryGetObjectField(*EdgeObjPtr, TEXT("to"), ToObj))
				{
					continue;
				}

				FString FromNodeId;
				FString FromPinName;
				FString ToNodeId;
				FString ToPinName;
				if (!UeAgentAnimBlueprintFolderOps::TryGetString(FromObj, TEXT("node"), FromNodeId) || FromNodeId.IsEmpty()
					|| !UeAgentAnimBlueprintFolderOps::TryGetString(FromObj, TEXT("pin"), FromPinName) || FromPinName.IsEmpty()
					|| !UeAgentAnimBlueprintFolderOps::TryGetString(ToObj, TEXT("node"), ToNodeId) || ToNodeId.IsEmpty()
					|| !UeAgentAnimBlueprintFolderOps::TryGetString(ToObj, TEXT("pin"), ToPinName) || ToPinName.IsEmpty())
				{
					OutError = TEXT("invalid_edge_definition");
					return false;
				}

				const FString* FromNodeGuid = NodeIdToGuid.Find(FromNodeId);
				const FString* ToNodeGuid = NodeIdToGuid.Find(ToNodeId);
				if (!FromNodeGuid || !ToNodeGuid)
				{
					OutError = TEXT("edge_node_not_found");
					return false;
				}

				TSharedPtr<FJsonObject> ConnectParams = MakeShared<FJsonObject>();
				ConnectParams->SetStringField(TEXT("asset_path"), AssetPath);
				ConnectParams->SetStringField(TEXT("graph_name"), Graph->GetPathName());
				ConnectParams->SetStringField(TEXT("from_node_guid"), *FromNodeGuid);
				ConnectParams->SetStringField(TEXT("from_pin"), FromPinName);
				ConnectParams->SetStringField(TEXT("to_node_guid"), *ToNodeGuid);
				ConnectParams->SetStringField(TEXT("to_pin"), ToPinName);
				ConnectParams->SetBoolField(TEXT("compile_after_connect"), false);
				ConnectParams->SetBoolField(TEXT("save_after_connect"), false);
				TSharedPtr<FJsonObject> ConnectData;
				if (!InvokeAnimSubCommand(TEXT("anim_blueprint_connect_pins"), ConnectParams, ConnectData, OutError))
				{
					OutError = FString::Printf(TEXT("connect_edge_failed:%s:%s.%s->%s.%s"), *Graph->GetName(), *FromNodeId, *FromPinName, *ToNodeId, *ToPinName);
					return false;
				}
				++EdgesCreated;
			}
		}

		++AnimGraphsRebuilt;
		return true;
	};

	for (const UeAgentAnimBlueprintFolderOps::FAnimGraphSpec& GraphSpec : AnimGraphSpecs)
	{
		if (!ApplySingleAnimGraph(GraphSpec))
		{
			return false;
		}
	}

	AnimBlueprint = UeAgentAnimBlueprintOps::LoadAnimBlueprint(AssetPath);
	if (!AnimBlueprint)
	{
		OutError = TEXT("anim_blueprint_not_found");
		return false;
	}

	if (bCompileAfterApply)
	{
		FKismetEditorUtilities::CompileBlueprint(AnimBlueprint);
	}
	if (bSaveAfterApply && !UeAgentAnimBlueprintOps::SaveAnimBlueprintPackage(AnimBlueprint, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), AssetPath);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetStringField(TEXT("root_path"), UeAgentAnimBlueprintFolderOps::GetFolderRootAbsolute());
	OutData->SetNumberField(TEXT("variables_added"), VariablesAdded);
	OutData->SetNumberField(TEXT("variables_removed"), VariablesRemoved);
	OutData->SetNumberField(TEXT("variable_defaults_updated"), VariableDefaultsUpdated);
	OutData->SetNumberField(TEXT("delegates_added"), DelegatesAdded);
	OutData->SetNumberField(TEXT("logic_graphs_rebuilt"), LogicGraphsRebuilt);
	OutData->SetNumberField(TEXT("anim_graphs_rebuilt"), AnimGraphsRebuilt);
	OutData->SetNumberField(TEXT("layer_interfaces_added"), LayerInterfacesAdded);
	OutData->SetNumberField(TEXT("layer_interfaces_removed"), LayerInterfacesRemoved);
	OutData->SetNumberField(TEXT("local_layers_added"), LocalLayersAdded);
	OutData->SetNumberField(TEXT("local_layers_removed"), LocalLayersRemoved);
	OutData->SetNumberField(TEXT("state_machines_rebuilt"), StateMachinesRebuilt);
	OutData->SetNumberField(TEXT("state_nodes_added"), StateNodesAdded);
	OutData->SetNumberField(TEXT("transitions_added"), TransitionsAdded);
	OutData->SetNumberField(TEXT("nodes_created"), NodesCreated);
	OutData->SetNumberField(TEXT("edges_created"), EdgesCreated);
	OutData->SetNumberField(TEXT("pin_defaults_applied"), PinDefaultsApplied);
	OutData->SetNumberField(TEXT("warning_count"), Warnings.Num());
	OutData->SetArrayField(TEXT("warnings"), Warnings);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterApply);
	OutData->SetBoolField(TEXT("compiled"), bCompileAfterApply);
	return true;
}

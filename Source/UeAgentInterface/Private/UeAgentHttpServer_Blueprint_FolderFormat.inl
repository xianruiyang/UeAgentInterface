namespace UeAgentBlueprintFolderOps
{
	static FString GetFolderRootAbsolute()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("ActorBlueprint")));
	}

	static FString NormalizeAssetRelativeFolder(const FString& InAssetPath)
	{
		FString AssetPath = UeAgentBlueprintOps::NormalizeAssetPath(InAssetPath);
		AssetPath.TrimStartAndEndInline();
		while (AssetPath.StartsWith(TEXT("/")))
		{
			AssetPath.RightChopInline(1, EAllowShrinking::No);
		}
		return AssetPath;
	}

	static bool ResolveFolderForAsset(const FString& InAssetPath, FString& OutFolderPath, FString& OutError)
	{
		const FString AssetPath = UeAgentBlueprintOps::NormalizeAssetPath(InAssetPath);
		if (!FPackageName::IsValidLongPackageName(AssetPath))
		{
			OutError = TEXT("invalid_asset_path");
			return false;
		}

		const FString RelativeFolder = NormalizeAssetRelativeFolder(AssetPath);
		OutFolderPath = FPaths::Combine(GetFolderRootAbsolute(), RelativeFolder);
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

	static FString PinContainerTypeToString(const EPinContainerType InContainerType)
	{
		switch (InContainerType)
		{
		case EPinContainerType::Array:
			return TEXT("array");
		case EPinContainerType::Set:
			return TEXT("set");
		case EPinContainerType::Map:
			return TEXT("map");
		default:
			return TEXT("");
		}
	}

	static bool IsBuiltinGraphNode(const UEdGraphNode* Node)
	{
		return Node
			&& (Node->IsA<UK2Node_FunctionEntry>()
				|| Node->IsA<UK2Node_FunctionResult>()
				|| Node->IsA<UK2Node_Tunnel>());
	}

	static FString GetBuiltinNodeId(const UEdGraphNode* Node)
	{
		if (Node->IsA<UK2Node_FunctionEntry>())
		{
			return TEXT("__entry__");
		}
		if (Node->IsA<UK2Node_FunctionResult>())
		{
			return TEXT("__result__");
		}
		if (const UK2Node_Tunnel* Tunnel = Cast<UK2Node_Tunnel>(Node))
		{
			bool bHasInputPin = false;
			bool bHasOutputPin = false;
			for (const UEdGraphPin* Pin : Tunnel->Pins)
			{
				if (!Pin)
				{
					continue;
				}
				bHasInputPin |= (Pin->Direction == EGPD_Input);
				bHasOutputPin |= (Pin->Direction == EGPD_Output);
			}
			if (bHasOutputPin && !bHasInputPin)
			{
				return TEXT("__entry__");
			}
			if (bHasInputPin && !bHasOutputPin)
			{
				return TEXT("__result__");
			}
		}
		return FString();
	}

	static FString MakeGraphFileName(const FString& GraphName, const FString& GraphKind)
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
		return Out.IsEmpty() ? TEXT("node") : Out;
	}

	static FString MakeBaseNodeId(const UBlueprint* Blueprint, UEdGraphNode* Node)
	{
		if (const UK2Node_ComponentBoundEvent* BoundEventNode = Cast<UK2Node_ComponentBoundEvent>(Node))
		{
			return FString::Printf(TEXT("bound_%s_%s"),
				*MakeSlug(BoundEventNode->ComponentPropertyName.ToString()),
				*MakeSlug(BoundEventNode->DelegatePropertyName.ToString()));
		}
		if (const UK2Node_EnhancedInputAction* EnhancedInputNode = Cast<UK2Node_EnhancedInputAction>(Node))
		{
			const FString ActionName = EnhancedInputNode->InputAction ? EnhancedInputNode->InputAction->GetName() : TEXT("input_action");
			return FString::Printf(TEXT("enhanced_input_%s"), *MakeSlug(ActionName));
		}
		if (const UK2Node_DynamicCast* DynamicCastNode = Cast<UK2Node_DynamicCast>(Node))
		{
			const FString TargetName = DynamicCastNode->TargetType ? DynamicCastNode->TargetType->GetName() : TEXT("cast_target");
			return FString::Printf(TEXT("cast_%s"), *MakeSlug(TargetName));
		}
		if (const UK2Node_CustomEvent* CustomEventNode = Cast<UK2Node_CustomEvent>(Node))
		{
			return FString::Printf(TEXT("custom_%s"), *MakeSlug(CustomEventNode->CustomFunctionName.ToString()));
		}
		if (const UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
		{
			const FString EventName = EventNode->EventReference.GetMemberName().ToString();
			return FString::Printf(TEXT("event_%s"), *MakeSlug(EventName));
		}
		if (const UK2Node_CallFunction* CallFunctionNode = Cast<UK2Node_CallFunction>(Node))
		{
			FString FunctionName = CallFunctionNode->FunctionReference.GetMemberName().ToString();
			UClass* FunctionOwnerClass = CallFunctionNode->FunctionReference.GetMemberParentClass(CallFunctionNode->GetBlueprintClassFromNode());
			if (const UFunction* Function = CallFunctionNode->GetTargetFunction())
			{
				FunctionName = Function->GetName();
				FunctionOwnerClass = Function->GetOwnerClass();
			}

			if (FunctionName.Equals(TEXT("GetLocalPlayerSubSystemFromPlayerController"), ESearchCase::CaseSensitive)
				&& FunctionOwnerClass == USubsystemBlueprintLibrary::StaticClass())
			{
				if (const UEdGraphPin* ClassPin = UeAgentBlueprintOps::FindNodePinByName(const_cast<UK2Node_CallFunction*>(CallFunctionNode), TEXT("Class")))
				{
					const FString ExpectedClassPath = UEnhancedInputLocalPlayerSubsystem::StaticClass()->GetPathName();
					const FString ConfiguredClassPath = ClassPin->DefaultObject ? ClassPin->DefaultObject->GetPathName() : ClassPin->DefaultValue;
					if (ConfiguredClassPath.Equals(ExpectedClassPath, ESearchCase::CaseSensitive))
					{
						return TEXT("get_enhanced_input_local_player_subsystem");
					}
				}
			}

			if (FunctionName.Equals(TEXT("AddMappingContext"), ESearchCase::CaseSensitive)
				&& FunctionOwnerClass == UEnhancedInputLocalPlayerSubsystem::StaticClass())
			{
				if (const UEdGraphPin* MappingContextPin = UeAgentBlueprintOps::FindNodePinByName(const_cast<UK2Node_CallFunction*>(CallFunctionNode), TEXT("MappingContext")))
				{
					const UObject* MappingContextObject = MappingContextPin->DefaultObject;
					const FString MappingContextName = MappingContextObject ? MappingContextObject->GetName() : TEXT("mapping_context");
					return FString::Printf(TEXT("enhanced_input_add_mapping_context_%s"), *MakeSlug(MappingContextName));
				}
				return TEXT("enhanced_input_add_mapping_context");
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

	static bool ExportPropertyText(UObject* Obj, const FString& PropertyPath, FString& OutValue)
	{
		FProperty* Property = nullptr;
		void* ValuePtr = nullptr;
		if (!UeAgentBlueprintOps::ResolvePropertyPath(Obj, PropertyPath, Property, ValuePtr) || !Property || !ValuePtr)
		{
			return false;
		}
		Property->ExportTextItem_Direct(OutValue, ValuePtr, ValuePtr, Obj, PPF_None);
		return true;
	}

	static FString ExportPinSubcategoryObjectPath(const FEdGraphPinType& PinType)
	{
		if (!PinType.PinSubCategoryObject.IsValid())
		{
			return FString();
		}
		if (const UObject* Obj = PinType.PinSubCategoryObject.Get())
		{
			return Obj->GetPathName();
		}
		return FString();
	}

	static FString ExportBlueprintVariableDefaultValue(UBlueprint* Blueprint, const FBPVariableDescription& VarDesc)
	{
		if (Blueprint && Blueprint->GeneratedClass)
		{
			UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject();
			if (CDO)
			{
				if (FProperty* Property = FindFProperty<FProperty>(Blueprint->GeneratedClass, VarDesc.VarName))
				{
					void* ValuePtr = Property->ContainerPtrToValuePtr<void>(CDO);
					if (ValuePtr)
					{
						FString ExportedValue;
						Property->ExportTextItem_Direct(ExportedValue, ValuePtr, ValuePtr, CDO, PPF_None);
						return ExportedValue;
					}
				}
			}
		}
		return VarDesc.DefaultValue;
	}

	static FString ExtractObjectPathFromImportText(const FString& ValueText)
	{
		const int32 FirstQuoteIndex = ValueText.Find(TEXT("'"));
		if (FirstQuoteIndex != INDEX_NONE)
		{
			const int32 SecondQuoteIndex = ValueText.Find(TEXT("'"), ESearchCase::CaseSensitive, ESearchDir::FromStart, FirstQuoteIndex + 1);
			if (SecondQuoteIndex != INDEX_NONE && SecondQuoteIndex > FirstQuoteIndex + 1)
			{
				return ValueText.Mid(FirstQuoteIndex + 1, SecondQuoteIndex - FirstQuoteIndex - 1);
			}
		}
		return ValueText.TrimStartAndEnd();
	}

	static TSharedPtr<FJsonObject> ExportVariablesJson(UBlueprint* Blueprint)
	{
		TArray<TSharedPtr<FJsonValue>> Variables;
		TArray<FBPVariableDescription> SortedVariables = Blueprint ? Blueprint->NewVariables : TArray<FBPVariableDescription>();
		SortedVariables.Sort([](const FBPVariableDescription& A, const FBPVariableDescription& B)
		{
			return A.VarName.LexicalLess(B.VarName);
		});

		for (const FBPVariableDescription& VarDesc : SortedVariables)
		{
			if (VarDesc.VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate)
			{
				continue;
			}

			TSharedPtr<FJsonObject> TypeObj = MakeShared<FJsonObject>();
			TypeObj->SetStringField(TEXT("pin_category"), VarDesc.VarType.PinCategory.ToString());
			if (!VarDesc.VarType.PinSubCategory.IsNone())
			{
				TypeObj->SetStringField(TEXT("pin_subcategory"), VarDesc.VarType.PinSubCategory.ToString());
			}
			const FString PinSubCategoryObjectPath = ExportPinSubcategoryObjectPath(VarDesc.VarType);
			if (!PinSubCategoryObjectPath.IsEmpty())
			{
				TypeObj->SetStringField(TEXT("pin_subcategory_object"), PinSubCategoryObjectPath);
			}
			const FString ContainerType = PinContainerTypeToString(VarDesc.VarType.ContainerType);
			if (!ContainerType.IsEmpty())
			{
				TypeObj->SetStringField(TEXT("container_type"), ContainerType);
			}

			TSharedPtr<FJsonObject> VarObj = MakeShared<FJsonObject>();
			VarObj->SetStringField(TEXT("name"), VarDesc.VarName.ToString());
			VarObj->SetObjectField(TEXT("type"), TypeObj);
			VarObj->SetStringField(TEXT("default_value"), ExportBlueprintVariableDefaultValue(Blueprint, VarDesc));
			VarObj->SetBoolField(TEXT("instance_editable"), (VarDesc.PropertyFlags & CPF_DisableEditOnInstance) == 0);
			Variables.Add(MakeShared<FJsonValueObject>(VarObj));
		}

		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetArrayField(TEXT("variables"), Variables);
		return Root;
	}

	static TSharedPtr<FJsonObject> ExportDelegatesJson(UBlueprint* Blueprint)
	{
		TArray<TSharedPtr<FJsonValue>> Delegates;
		TArray<FString> Names;
		if (Blueprint)
		{
			for (UEdGraph* Graph : Blueprint->DelegateSignatureGraphs)
			{
				if (Graph)
				{
					Names.Add(Graph->GetName());
				}
			}
		}
		Names.Sort();
		for (const FString& Name : Names)
		{
			TSharedPtr<FJsonObject> DelegateObj = MakeShared<FJsonObject>();
			DelegateObj->SetStringField(TEXT("name"), Name);
			Delegates.Add(MakeShared<FJsonValueObject>(DelegateObj));
		}

		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetArrayField(TEXT("delegates"), Delegates);
		return Root;
	}

	static TSharedPtr<FJsonObject> ExportDefaultsJson()
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetArrayField(TEXT("defaults"), {});
		return Root;
	}

	static void BuildSCSParentMap(USCS_Node* Node, const FString& ParentName, TMap<const USCS_Node*, FString>& OutParentMap)
	{
		if (!Node)
		{
			return;
		}
		OutParentMap.Add(Node, ParentName);
		for (USCS_Node* ChildNode : Node->GetChildNodes())
		{
			BuildSCSParentMap(ChildNode, Node->GetVariableName().ToString(), OutParentMap);
		}
	}

	static TSharedPtr<FJsonObject> ExportComponentsJson(UBlueprint* Blueprint)
	{
		TArray<TSharedPtr<FJsonValue>> Components;
		USimpleConstructionScript* SCS = Blueprint ? Blueprint->SimpleConstructionScript : nullptr;
		if (SCS)
		{
			TMap<const USCS_Node*, FString> ParentMap;
			for (USCS_Node* RootNode : SCS->GetRootNodes())
			{
				BuildSCSParentMap(RootNode, FString(), ParentMap);
			}

			TArray<USCS_Node*> Nodes = SCS->GetAllNodes();
			Nodes.Sort([](const USCS_Node& A, const USCS_Node& B)
			{
				return A.GetVariableName().LexicalLess(B.GetVariableName());
			});

			for (USCS_Node* Node : Nodes)
			{
				if (!Node)
				{
					continue;
				}
				TSharedPtr<FJsonObject> ComponentObj = MakeShared<FJsonObject>();
				ComponentObj->SetStringField(TEXT("name"), Node->GetVariableName().ToString());
				ComponentObj->SetStringField(TEXT("class"), Node->ComponentClass ? Node->ComponentClass->GetPathName() : TEXT(""));
				ComponentObj->SetStringField(TEXT("parent"), ParentMap.FindRef(Node));

				TArray<TSharedPtr<FJsonValue>> Properties;
				if (UActorComponent* Template = Node->ComponentTemplate)
				{
					static const TArray<FString> CommonProperties = {
						TEXT("RelativeLocation"),
						TEXT("RelativeRotation"),
						TEXT("RelativeScale3D")
					};
					for (const FString& PropertyPath : CommonProperties)
					{
						FString ExportedValue;
						if (ExportPropertyText(Template, PropertyPath, ExportedValue))
						{
							TSharedPtr<FJsonObject> PropertyObj = MakeShared<FJsonObject>();
							PropertyObj->SetStringField(TEXT("property_name"), PropertyPath);
							PropertyObj->SetStringField(TEXT("value_text"), ExportedValue);
							Properties.Add(MakeShared<FJsonValueObject>(PropertyObj));
						}
					}

					if (Template->IsA<UStaticMeshComponent>())
					{
						FString ExportedValue;
						if (ExportPropertyText(Template, TEXT("StaticMesh"), ExportedValue) && !ExportedValue.IsEmpty() && ExportedValue != TEXT("None"))
						{
							TSharedPtr<FJsonObject> PropertyObj = MakeShared<FJsonObject>();
							PropertyObj->SetStringField(TEXT("property_name"), TEXT("StaticMesh"));
							PropertyObj->SetStringField(TEXT("value_text"), ExportedValue);
							Properties.Add(MakeShared<FJsonValueObject>(PropertyObj));
						}
					}
				}

				ComponentObj->SetArrayField(TEXT("properties"), Properties);
				Components.Add(MakeShared<FJsonValueObject>(ComponentObj));
			}
		}

		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetArrayField(TEXT("components"), Components);
		return Root;
	}

	static TSharedPtr<FJsonObject> ExportGraphJson(UBlueprint* Blueprint, UEdGraph* Graph, const FString& GraphKind)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> GraphObj = MakeShared<FJsonObject>();
		GraphObj->SetStringField(TEXT("name"), Graph->GetName());
		GraphObj->SetStringField(TEXT("graph_kind"), GraphKind);
		Root->SetObjectField(TEXT("graph"), GraphObj);

		TMap<const UEdGraphNode*, FString> BuiltinIds;
		TMap<FString, int32> IdUseCount;
		TMap<const UEdGraphNode*, FString> ExportNodeIds;

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}
			if (IsBuiltinGraphNode(Node))
			{
				const FString BuiltinId = GetBuiltinNodeId(Node);
				if (!BuiltinId.IsEmpty())
				{
					BuiltinIds.Add(Node, BuiltinId);
				}
				continue;
			}

			FString BaseId = MakeBaseNodeId(Blueprint, Node);
			int32& Count = IdUseCount.FindOrAdd(BaseId);
			++Count;
			if (Count > 1)
			{
				BaseId = FString::Printf(TEXT("%s_%d"), *BaseId, Count);
			}
			ExportNodeIds.Add(Node, BaseId);
		}

		TArray<TSharedPtr<FJsonValue>> Nodes;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node || IsBuiltinGraphNode(Node))
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

			if (const UK2Node_ComponentBoundEvent* BoundEventNode = Cast<UK2Node_ComponentBoundEvent>(Node))
			{
				NodeObj->SetStringField(TEXT("node_type"), TEXT("component_bound_event"));
				NodeObj->SetStringField(TEXT("component_name"), BoundEventNode->ComponentPropertyName.ToString());
				NodeObj->SetStringField(TEXT("delegate_property_name"), BoundEventNode->DelegatePropertyName.ToString());
				if (UClass* OwnerClass = BoundEventNode->DelegateOwnerClass.Get())
				{
					NodeObj->SetStringField(TEXT("delegate_owner_class"), OwnerClass->GetPathName());
				}
			}
			else if (const UK2Node_EnhancedInputAction* EnhancedInputNode = Cast<UK2Node_EnhancedInputAction>(Node))
			{
				NodeObj->SetStringField(TEXT("node_type"), TEXT("enhanced_input_action_event"));
				if (EnhancedInputNode->InputAction)
				{
					NodeObj->SetStringField(TEXT("input_action"), EnhancedInputNode->InputAction->GetPathName());
				}
			}
			else if (const UK2Node_DynamicCast* DynamicCastNode = Cast<UK2Node_DynamicCast>(Node))
			{
				NodeObj->SetStringField(TEXT("node_type"), TEXT("dynamic_cast"));
				if (DynamicCastNode->TargetType)
				{
					NodeObj->SetStringField(TEXT("target_class"), DynamicCastNode->TargetType->GetPathName());
				}
				NodeObj->SetBoolField(TEXT("pure"), DynamicCastNode->IsNodePure());
			}
			else if (const UK2Node_CustomEvent* CustomEventNode = Cast<UK2Node_CustomEvent>(Node))
			{
				NodeObj->SetStringField(TEXT("node_type"), TEXT("custom_event"));
				NodeObj->SetStringField(TEXT("event_name"), CustomEventNode->CustomFunctionName.ToString());
			}
			else if (const UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
			{
				NodeObj->SetStringField(TEXT("node_type"), TEXT("event"));
				NodeObj->SetStringField(TEXT("event_name"), EventNode->EventReference.GetMemberName().ToString());
				if (UClass* EventClass = EventNode->EventReference.GetMemberParentClass(EventNode->GetBlueprintClassFromNode()))
				{
					NodeObj->SetStringField(TEXT("event_class"), EventClass->GetPathName());
				}
			}
			else if (const UK2Node_CallFunction* CallFunctionNode = Cast<UK2Node_CallFunction>(Node))
			{
				FString FunctionName = CallFunctionNode->FunctionReference.GetMemberName().ToString();
				UClass* FunctionOwnerClass = CallFunctionNode->FunctionReference.GetMemberParentClass(CallFunctionNode->GetBlueprintClassFromNode());
				if (UFunction* Function = CallFunctionNode->GetTargetFunction())
				{
					FunctionName = Function->GetName();
					FunctionOwnerClass = Function->GetOwnerClass();
				}

				const UEdGraphPin* ClassPin = UeAgentBlueprintOps::FindNodePinByName(const_cast<UK2Node_CallFunction*>(CallFunctionNode), TEXT("Class"));
				const FString ExpectedSubsystemClassPath = UEnhancedInputLocalPlayerSubsystem::StaticClass()->GetPathName();
				const FString ConfiguredSubsystemClassPath = ClassPin ? (ClassPin->DefaultObject ? ClassPin->DefaultObject->GetPathName() : ClassPin->DefaultValue) : FString();
				if (FunctionName.Equals(TEXT("GetLocalPlayerSubSystemFromPlayerController"), ESearchCase::CaseSensitive)
					&& FunctionOwnerClass == USubsystemBlueprintLibrary::StaticClass()
					&& ConfiguredSubsystemClassPath.Equals(ExpectedSubsystemClassPath, ESearchCase::CaseSensitive))
				{
					NodeObj->SetStringField(TEXT("node_type"), TEXT("enhanced_input_get_local_player_subsystem"));
				}
				else if (FunctionName.Equals(TEXT("AddMappingContext"), ESearchCase::CaseSensitive)
					&& FunctionOwnerClass == UEnhancedInputLocalPlayerSubsystem::StaticClass())
				{
					NodeObj->SetStringField(TEXT("node_type"), TEXT("enhanced_input_add_mapping_context"));
					if (const UEdGraphPin* MappingContextPin = UeAgentBlueprintOps::FindNodePinByName(const_cast<UK2Node_CallFunction*>(CallFunctionNode), TEXT("MappingContext")))
					{
						const FString MappingContextPath = MappingContextPin->DefaultObject ? MappingContextPin->DefaultObject->GetPathName() : MappingContextPin->DefaultValue;
						if (!MappingContextPath.IsEmpty())
						{
							NodeObj->SetStringField(TEXT("mapping_context"), MappingContextPath);
						}
					}
					if (const UEdGraphPin* PriorityPin = UeAgentBlueprintOps::FindNodePinByName(const_cast<UK2Node_CallFunction*>(CallFunctionNode), TEXT("Priority")))
					{
						double PriorityValue = 0.0;
						if (!PriorityPin->DefaultValue.IsEmpty() && LexTryParseString(PriorityValue, *PriorityPin->DefaultValue))
						{
							NodeObj->SetNumberField(TEXT("priority"), PriorityValue);
						}
					}
					if (const UEdGraphPin* OptionsPin = UeAgentBlueprintOps::FindNodePinByName(const_cast<UK2Node_CallFunction*>(CallFunctionNode), TEXT("Options")))
					{
						if (!OptionsPin->DefaultValue.IsEmpty())
						{
							NodeObj->SetStringField(TEXT("options_default_value"), OptionsPin->DefaultValue);
						}
					}
				}
				else
				{
					NodeObj->SetStringField(TEXT("node_type"), TEXT("call_function"));
					NodeObj->SetStringField(TEXT("function_name"), FunctionName);
					if (FunctionOwnerClass)
					{
						NodeObj->SetStringField(TEXT("function_owner_class"), FunctionOwnerClass->GetPathName());
					}
				}
			}
			else if (const UK2Node_Variable* VariableNode = Cast<UK2Node_Variable>(Node))
			{
				NodeObj->SetStringField(TEXT("node_type"), TEXT("variable_node"));
				NodeObj->SetStringField(TEXT("variable_name"), VariableNode->VariableReference.GetMemberName().ToString());
				NodeObj->SetStringField(TEXT("access"), Node->IsA<UK2Node_VariableSet>() ? TEXT("set") : TEXT("get"));
				if (!VariableNode->VariableReference.IsSelfContext())
				{
					if (UClass* MemberParentClass = VariableNode->VariableReference.GetMemberParentClass(VariableNode->GetBlueprintClassFromNode()))
					{
						NodeObj->SetStringField(TEXT("member_parent_class"), MemberParentClass->GetPathName());
					}
				}
			}
			else
			{
				NodeObj->SetStringField(TEXT("node_type"), TEXT("node_by_class"));
				NodeObj->SetStringField(TEXT("node_class"), Node->GetClass()->GetPathName());
			}

			TArray<TSharedPtr<FJsonValue>> PinDefaults;
			const FString NodeTypeValue = NodeObj->GetStringField(TEXT("node_type"));
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Input || Pin->DefaultValue.IsEmpty())
				{
					continue;
				}
				if (NodeTypeValue.Equals(TEXT("enhanced_input_get_local_player_subsystem"), ESearchCase::CaseSensitive)
					&& Pin->PinName == TEXT("Class"))
				{
					continue;
				}
				if (NodeTypeValue.Equals(TEXT("enhanced_input_add_mapping_context"), ESearchCase::CaseSensitive)
					&& (Pin->PinName == TEXT("MappingContext") || Pin->PinName == TEXT("Priority") || Pin->PinName == TEXT("Options")))
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
		Root->SetArrayField(TEXT("nodes"), Nodes);

		TMap<const UEdGraphNode*, FString> NodeIds = ExportNodeIds;
		for (const TPair<const UEdGraphNode*, FString>& Pair : BuiltinIds)
		{
			NodeIds.Add(Pair.Key, Pair.Value);
		}

		TSet<FString> SeenEdges;
		TArray<TSharedPtr<FJsonValue>> Edges;
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
		Root->SetArrayField(TEXT("edges"), Edges);
		return Root;
	}

	static TSharedPtr<FJsonObject> ExportValidationJson()
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Checks;
		TSharedPtr<FJsonObject> CheckObj = MakeShared<FJsonObject>();
		CheckObj->SetStringField(TEXT("kind"), TEXT("compile_success"));
		Checks.Add(MakeShared<FJsonValueObject>(CheckObj));
		Root->SetArrayField(TEXT("checks"), Checks);
		return Root;
	}

	static bool BuildPinTypeFromJson(const TSharedPtr<FJsonObject>& TypeObj, FEdGraphPinType& OutPinType, FString& OutError)
	{
		FString PinCategoryText;
		if (!TryGetString(TypeObj, TEXT("pin_category"), PinCategoryText) || PinCategoryText.IsEmpty())
		{
			OutError = TEXT("missing_pin_category");
			return false;
		}

		OutPinType = FEdGraphPinType();
		OutPinType.PinCategory = UeAgentBlueprintOps::NormalizePinCategoryName(PinCategoryText);
		if (OutPinType.PinCategory.IsNone())
		{
			OutError = TEXT("invalid_pin_category");
			return false;
		}

		FString PinSubCategoryText;
		if (TryGetString(TypeObj, TEXT("pin_subcategory"), PinSubCategoryText) && !PinSubCategoryText.IsEmpty())
		{
			OutPinType.PinSubCategory = UeAgentBlueprintOps::NormalizeRealPinSubCategoryName(PinSubCategoryText);
		}

		FString PinSubCategoryObjectPath;
		if (TryGetString(TypeObj, TEXT("pin_subcategory_object"), PinSubCategoryObjectPath) && !PinSubCategoryObjectPath.IsEmpty())
		{
			UObject* SubCategoryObject = UeAgentBlueprintOps::ResolvePinSubCategoryObject(PinSubCategoryObjectPath);
			if (!SubCategoryObject)
			{
				OutError = TEXT("invalid_pin_subcategory_object");
				return false;
			}
			OutPinType.PinSubCategoryObject = SubCategoryObject;
		}

		FString ContainerTypeText;
		if (TryGetString(TypeObj, TEXT("container_type"), ContainerTypeText) && !ContainerTypeText.IsEmpty())
		{
			OutPinType.ContainerType = UeAgentBlueprintOps::ParsePinContainerType(ContainerTypeText);
		}

		if (OutPinType.PinCategory == UEdGraphSchema_K2::PC_Real)
		{
			if (OutPinType.PinSubCategory.IsNone())
			{
				OutPinType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
			}
			if (OutPinType.PinSubCategory != UEdGraphSchema_K2::PC_Float && OutPinType.PinSubCategory != UEdGraphSchema_K2::PC_Double)
			{
				OutError = TEXT("invalid_real_pin_subcategory");
				return false;
			}
		}

		return true;
	}

	static bool PinTypesEqual(const FEdGraphPinType& A, const FEdGraphPinType& B)
	{
		const UObject* ASubObj = A.PinSubCategoryObject.Get();
		const UObject* BSubObj = B.PinSubCategoryObject.Get();
		return A.PinCategory == B.PinCategory
			&& A.PinSubCategory == B.PinSubCategory
			&& A.ContainerType == B.ContainerType
			&& ASubObj == BSubObj;
	}

	static bool SetBlueprintVariableInstanceEditable(UBlueprint* Blueprint, const FName VariableName, const bool bInstanceEditable)
	{
		if (!Blueprint)
		{
			return false;
		}
		FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Blueprint, VariableName, !bInstanceEditable);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		return true;
	}
}

bool FUeAgentHttpServer::CmdBlueprintExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UBlueprint* Blueprint = UeAgentBlueprintOps::LoadBlueprintAsset(AssetPathInput);
	if (!Blueprint)
	{
		OutError = TEXT("blueprint_not_found");
		return false;
	}

	const FString AssetPath = UeAgentBlueprintOps::NormalizeAssetPath(AssetPathInput);
	FString FolderPath;
	if (!UeAgentBlueprintFolderOps::ResolveFolderForAsset(AssetPath, FolderPath, OutError))
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

	const FString MembersDir = FPaths::Combine(FolderPath, TEXT("members"));
	const FString ComponentsDir = FPaths::Combine(FolderPath, TEXT("components"));
	const FString GraphsDir = FPaths::Combine(FolderPath, TEXT("graphs"));
	const FString ValidationDir = FPaths::Combine(FolderPath, TEXT("validation"));
	IFileManager::Get().MakeDirectory(*MembersDir, true);
	IFileManager::Get().MakeDirectory(*ComponentsDir, true);
	IFileManager::Get().MakeDirectory(*GraphsDir, true);
	if (bIncludeValidation)
	{
		IFileManager::Get().MakeDirectory(*ValidationDir, true);
	}

	TSharedPtr<FJsonObject> AssetJson = MakeShared<FJsonObject>();
	AssetJson->SetNumberField(TEXT("format_version"), 1);
	AssetJson->SetStringField(TEXT("asset_kind"), TEXT("actor_blueprint"));
	AssetJson->SetStringField(TEXT("asset_path"), AssetPath);
	AssetJson->SetStringField(TEXT("asset_name"), FPackageName::GetLongPackageAssetName(AssetPath));
	AssetJson->SetStringField(TEXT("parent_class"), Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : TEXT(""));
	AssetJson->SetStringField(TEXT("generated_class_name"), Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetName() : TEXT(""));

	int32 FileCount = 0;
	auto SaveRequiredJson = [&FileCount, &OutError](const FString& FilePath, const TSharedPtr<FJsonObject>& JsonObj) -> bool
	{
		if (!UeAgentBlueprintFolderOps::SaveJsonFile(FilePath, JsonObj))
		{
			OutError = TEXT("save_export_file_failed");
			return false;
		}
		++FileCount;
		return true;
	};

	if (!SaveRequiredJson(FPaths::Combine(FolderPath, TEXT("asset.json")), AssetJson)) return false;
	if (!SaveRequiredJson(FPaths::Combine(MembersDir, TEXT("variables.json")), UeAgentBlueprintFolderOps::ExportVariablesJson(Blueprint))) return false;
	if (!SaveRequiredJson(FPaths::Combine(MembersDir, TEXT("delegates.json")), UeAgentBlueprintFolderOps::ExportDelegatesJson(Blueprint))) return false;
	if (!SaveRequiredJson(FPaths::Combine(MembersDir, TEXT("defaults.json")), UeAgentBlueprintFolderOps::ExportDefaultsJson())) return false;
	if (!SaveRequiredJson(FPaths::Combine(ComponentsDir, TEXT("tree.json")), UeAgentBlueprintFolderOps::ExportComponentsJson(Blueprint))) return false;

	auto ExportGraphsByKind = [this, &FileCount, &OutError, &GraphsDir, Blueprint](const TArray<UEdGraph*>& InGraphs, const FString& GraphKind) -> bool
	{
		for (UEdGraph* Graph : InGraphs)
		{
			if (!Graph)
			{
				continue;
			}
			const FString GraphFileName = UeAgentBlueprintFolderOps::MakeGraphFileName(Graph->GetName(), GraphKind);
			const FString GraphFilePath = FPaths::Combine(GraphsDir, GraphFileName);
			if (!UeAgentBlueprintFolderOps::SaveJsonFile(GraphFilePath, UeAgentBlueprintFolderOps::ExportGraphJson(Blueprint, Graph, GraphKind)))
			{
				OutError = TEXT("save_export_graph_failed");
				return false;
			}
			++FileCount;
		}
		return true;
	};

	if (!ExportGraphsByKind(Blueprint->UbergraphPages, TEXT("event_graph"))) return false;
	if (!ExportGraphsByKind(Blueprint->FunctionGraphs, TEXT("function_graph"))) return false;
	if (!ExportGraphsByKind(Blueprint->MacroGraphs, TEXT("macro_graph"))) return false;

	if (bIncludeValidation)
	{
		if (!SaveRequiredJson(FPaths::Combine(ValidationDir, TEXT("checks.json")), UeAgentBlueprintFolderOps::ExportValidationJson())) return false;
	}

	OutData->SetStringField(TEXT("asset_path"), AssetPath);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetStringField(TEXT("root_path"), UeAgentBlueprintFolderOps::GetFolderRootAbsolute());
	OutData->SetNumberField(TEXT("file_count"), FileCount);
	OutData->SetBoolField(TEXT("clean_output_dir"), bCleanOutputDir);
	return true;
}

bool FUeAgentHttpServer::CmdBlueprintApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	const FString AssetPath = UeAgentBlueprintOps::NormalizeAssetPath(AssetPathInput);
	FString FolderPath;
	if (!UeAgentBlueprintFolderOps::ResolveFolderForAsset(AssetPath, FolderPath, OutError))
	{
		return false;
	}

	TSharedPtr<FJsonObject> AssetJson;
	if (!UeAgentBlueprintFolderOps::LoadJsonFile(FPaths::Combine(FolderPath, TEXT("asset.json")), AssetJson, OutError))
	{
		return false;
	}

	FString AssetKind;
	if (!UeAgentBlueprintFolderOps::TryGetString(AssetJson, TEXT("asset_kind"), AssetKind) || !AssetKind.Equals(TEXT("actor_blueprint"), ESearchCase::IgnoreCase))
	{
		OutError = TEXT("unsupported_asset_kind");
		return false;
	}

	FString AssetPathInFile;
	if (UeAgentBlueprintFolderOps::TryGetString(AssetJson, TEXT("asset_path"), AssetPathInFile) && !AssetPathInFile.IsEmpty())
	{
		const FString NormalizedFileAssetPath = UeAgentBlueprintOps::NormalizeAssetPath(AssetPathInFile);
		if (!NormalizedFileAssetPath.Equals(AssetPath, ESearchCase::IgnoreCase))
		{
			OutError = TEXT("asset_path_mismatch");
			return false;
		}
	}

	bool bCompileAfterApply = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_apply"), bCompileAfterApply);
	bool bSaveAfterApply = true;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);
	bool bCreateIfMissing = true;
	JsonTryGetBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing);

	TArray<TSharedPtr<FJsonValue>> Warnings;
	int32 VariablesAdded = 0;
	int32 VariablesRemoved = 0;
	int32 VariableDefaultsUpdated = 0;
	int32 ComponentsAdded = 0;
	int32 ComponentsRemoved = 0;
	int32 ComponentPropertiesApplied = 0;
	int32 DelegatesAdded = 0;
	int32 GraphsCreated = 0;
	int32 GraphsRebuilt = 0;
	int32 NodesCreated = 0;
	int32 EdgesCreated = 0;
	int32 PinDefaultsApplied = 0;
	int32 DefaultsApplied = 0;

	auto AddWarning = [&Warnings](const FString& Code, const FString& Message)
	{
		TSharedPtr<FJsonObject> WarningObj = MakeShared<FJsonObject>();
		WarningObj->SetStringField(TEXT("code"), Code);
		WarningObj->SetStringField(TEXT("message"), Message);
		Warnings.Add(MakeShared<FJsonValueObject>(WarningObj));
	};

	auto InvokeBlueprintSubCommand = [this](const FString& Command, const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& SubData, FString& SubError) -> bool
	{
		FUeAgentRequestContext SubCtx;
		SubCtx.RequestId = TEXT("blueprint_folder_apply");
		SubCtx.Command = Command;
		SubCtx.Params = Params;
		if (!SubData.IsValid())
		{
			SubData = MakeShared<FJsonObject>();
		}
		return ExecuteBlueprintCommand(Command, SubCtx, SubData, SubError);
	};

	UBlueprint* Blueprint = UeAgentBlueprintOps::LoadBlueprintAsset(AssetPath);
	if (!Blueprint)
	{
		if (!bCreateIfMissing)
		{
			OutError = TEXT("blueprint_not_found");
			return false;
		}

		FString ParentClassPath = TEXT("/Script/Engine.Actor");
		UeAgentBlueprintFolderOps::TryGetString(AssetJson, TEXT("parent_class"), ParentClassPath);
		TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
		CreateParams->SetStringField(TEXT("asset_path"), AssetPath);
		CreateParams->SetStringField(TEXT("parent_class"), ParentClassPath);
		CreateParams->SetBoolField(TEXT("compile_after_create"), false);
		CreateParams->SetBoolField(TEXT("save_after_create"), false);
		TSharedPtr<FJsonObject> CreateData;
		if (!InvokeBlueprintSubCommand(TEXT("blueprint_create"), CreateParams, CreateData, OutError))
		{
			return false;
		}
		Blueprint = UeAgentBlueprintOps::LoadBlueprintAsset(AssetPath);
	}

	if (!Blueprint)
	{
		OutError = TEXT("blueprint_load_after_create_failed");
		return false;
	}

	FString DesiredParentClassPath;
	if (UeAgentBlueprintFolderOps::TryGetString(AssetJson, TEXT("parent_class"), DesiredParentClassPath) && !DesiredParentClassPath.IsEmpty())
	{
		const FString CurrentParentClassPath = Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : FString();
		if (!CurrentParentClassPath.Equals(DesiredParentClassPath, ESearchCase::IgnoreCase))
		{
			TSharedPtr<FJsonObject> ReparentParams = MakeShared<FJsonObject>();
			ReparentParams->SetStringField(TEXT("asset_path"), AssetPath);
			ReparentParams->SetStringField(TEXT("parent_class"), DesiredParentClassPath);
			TSharedPtr<FJsonObject> ReparentData;
			if (!InvokeBlueprintSubCommand(TEXT("blueprint_reparent"), ReparentParams, ReparentData, OutError))
			{
				return false;
			}
			Blueprint = UeAgentBlueprintOps::LoadBlueprintAsset(AssetPath);
		}
	}

	TSharedPtr<FJsonObject> VariablesJson;
	FString VariablesError;
	if (!UeAgentBlueprintFolderOps::LoadOptionalJsonFile(FPaths::Combine(FolderPath, TEXT("members"), TEXT("variables.json")), VariablesJson, VariablesError))
	{
		OutError = VariablesError;
		return false;
	}
	if (VariablesJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* DesiredVariablesArray = nullptr;
		if (VariablesJson->TryGetArrayField(TEXT("variables"), DesiredVariablesArray) && DesiredVariablesArray)
		{
			TMap<FString, TSharedPtr<FJsonObject>> DesiredVariables;
			for (const TSharedPtr<FJsonValue>& Value : *DesiredVariablesArray)
			{
				const TSharedPtr<FJsonObject>* VariableObjPtr = nullptr;
				if (!Value.IsValid() || !Value->TryGetObject(VariableObjPtr) || !VariableObjPtr || !VariableObjPtr->IsValid())
				{
					AddWarning(TEXT("variable_invalid_item"), TEXT("Skipped variable entry because it is not a JSON object."));
					continue;
				}
				FString VariableName;
				if (!UeAgentBlueprintFolderOps::TryGetString(*VariableObjPtr, TEXT("name"), VariableName) || VariableName.IsEmpty())
				{
					AddWarning(TEXT("variable_missing_name"), TEXT("Skipped variable entry because name is missing."));
					continue;
				}
				DesiredVariables.Add(VariableName, *VariableObjPtr);
			}

			TMap<FString, FBPVariableDescription> CurrentVariables;
			for (const FBPVariableDescription& VarDesc : Blueprint->NewVariables)
			{
				if (VarDesc.VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate)
				{
					continue;
				}
				CurrentVariables.Add(VarDesc.VarName.ToString(), VarDesc);
			}

			for (const TPair<FString, FBPVariableDescription>& CurrentPair : CurrentVariables)
			{
				if (!DesiredVariables.Contains(CurrentPair.Key))
				{
					TSharedPtr<FJsonObject> RemoveParams = MakeShared<FJsonObject>();
					RemoveParams->SetStringField(TEXT("asset_path"), AssetPath);
					RemoveParams->SetStringField(TEXT("variable_name"), CurrentPair.Key);
					RemoveParams->SetBoolField(TEXT("compile_after_remove"), false);
					RemoveParams->SetBoolField(TEXT("save_after_remove"), false);
					TSharedPtr<FJsonObject> RemoveData;
					if (!InvokeBlueprintSubCommand(TEXT("blueprint_remove_variable"), RemoveParams, RemoveData, OutError))
					{
						return false;
					}
					++VariablesRemoved;
				}
			}

			Blueprint = UeAgentBlueprintOps::LoadBlueprintAsset(AssetPath);
			CurrentVariables.Reset();
			for (const FBPVariableDescription& VarDesc : Blueprint->NewVariables)
			{
				if (VarDesc.VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate)
				{
					continue;
				}
				CurrentVariables.Add(VarDesc.VarName.ToString(), VarDesc);
			}

			for (const TPair<FString, TSharedPtr<FJsonObject>>& DesiredPair : DesiredVariables)
			{
				const FString& VariableName = DesiredPair.Key;
				const TSharedPtr<FJsonObject>& VariableObj = DesiredPair.Value;
				TSharedPtr<FJsonObject> TypeObj;
				if (!UeAgentBlueprintFolderOps::TryGetObjectField(VariableObj, TEXT("type"), TypeObj))
				{
					OutError = TEXT("missing_variable_type");
					return false;
				}

				FEdGraphPinType DesiredPinType;
				if (!UeAgentBlueprintFolderOps::BuildPinTypeFromJson(TypeObj, DesiredPinType, OutError))
				{
					return false;
				}

				FString DefaultValue;
				UeAgentBlueprintFolderOps::TryGetString(VariableObj, TEXT("default_value"), DefaultValue);
				bool bInstanceEditable = false;
				UeAgentBlueprintFolderOps::TryGetBool(VariableObj, TEXT("instance_editable"), bInstanceEditable);

				const FBPVariableDescription* CurrentVarDesc = CurrentVariables.Find(VariableName);
				if (!CurrentVarDesc)
				{
					TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
					AddParams->SetStringField(TEXT("asset_path"), AssetPath);
					AddParams->SetStringField(TEXT("variable_name"), VariableName);
					AddParams->SetStringField(TEXT("pin_category"), DesiredPinType.PinCategory.ToString());
					if (!DesiredPinType.PinSubCategory.IsNone())
					{
						AddParams->SetStringField(TEXT("pin_subcategory"), DesiredPinType.PinSubCategory.ToString());
					}
					if (const UObject* SubCategoryObj = DesiredPinType.PinSubCategoryObject.Get())
					{
						AddParams->SetStringField(TEXT("pin_subcategory_object"), SubCategoryObj->GetPathName());
					}
					const FString ContainerTypeText = UeAgentBlueprintFolderOps::PinContainerTypeToString(DesiredPinType.ContainerType);
					if (!ContainerTypeText.IsEmpty())
					{
						AddParams->SetStringField(TEXT("container_type"), ContainerTypeText);
					}
					AddParams->SetStringField(TEXT("default_value"), DefaultValue);
					AddParams->SetBoolField(TEXT("instance_editable"), bInstanceEditable);
					AddParams->SetBoolField(TEXT("compile_after_add"), false);
					AddParams->SetBoolField(TEXT("save_after_add"), false);
					TSharedPtr<FJsonObject> AddData;
					if (!InvokeBlueprintSubCommand(TEXT("blueprint_add_variable"), AddParams, AddData, OutError))
					{
						return false;
					}
					++VariablesAdded;
					continue;
				}

				if (!UeAgentBlueprintFolderOps::PinTypesEqual(CurrentVarDesc->VarType, DesiredPinType))
				{
					OutError = TEXT("variable_type_update_not_supported");
					return false;
				}

				const bool bCurrentInstanceEditable = (CurrentVarDesc->PropertyFlags & CPF_DisableEditOnInstance) == 0;
				if (bCurrentInstanceEditable != bInstanceEditable)
				{
					UeAgentBlueprintFolderOps::SetBlueprintVariableInstanceEditable(Blueprint, FName(*VariableName), bInstanceEditable);
				}

				const FString CurrentDefaultValue = UeAgentBlueprintFolderOps::ExportBlueprintVariableDefaultValue(Blueprint, *CurrentVarDesc);
				if (!CurrentDefaultValue.Equals(DefaultValue, ESearchCase::CaseSensitive))
				{
					const bool bMissingGeneratedProperty =
						!Blueprint->GeneratedClass ||
						!FindFProperty<FProperty>(Blueprint->GeneratedClass, FName(*VariableName));
					if (bMissingGeneratedProperty)
					{
						FKismetEditorUtilities::CompileBlueprint(Blueprint);
					}

					TSharedPtr<FJsonObject> DefaultParams = MakeShared<FJsonObject>();
					DefaultParams->SetStringField(TEXT("asset_path"), AssetPath);
					DefaultParams->SetStringField(TEXT("property_name"), VariableName);
					DefaultParams->SetStringField(TEXT("value_text"), DefaultValue);
					TSharedPtr<FJsonObject> DefaultData;
					if (!InvokeBlueprintSubCommand(TEXT("blueprint_set_cdo_property"), DefaultParams, DefaultData, OutError))
					{
						OutError = FString::Printf(TEXT("set_variable_default_failed:%s:%s"), *VariableName, *OutError);
						return false;
					}
					++VariableDefaultsUpdated;
				}
			}
		}
	}

	TSharedPtr<FJsonObject> DelegatesJson;
	FString DelegatesError;
	if (!UeAgentBlueprintFolderOps::LoadOptionalJsonFile(FPaths::Combine(FolderPath, TEXT("members"), TEXT("delegates.json")), DelegatesJson, DelegatesError))
	{
		OutError = DelegatesError;
		return false;
	}
	if (DelegatesJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* DesiredDelegatesArray = nullptr;
		if (DelegatesJson->TryGetArrayField(TEXT("delegates"), DesiredDelegatesArray) && DesiredDelegatesArray)
		{
			TSet<FString> DesiredDelegateNames;
			for (const TSharedPtr<FJsonValue>& Value : *DesiredDelegatesArray)
			{
				const TSharedPtr<FJsonObject>* DelegateObjPtr = nullptr;
				if (!Value.IsValid() || !Value->TryGetObject(DelegateObjPtr) || !DelegateObjPtr || !DelegateObjPtr->IsValid())
				{
					AddWarning(TEXT("delegate_invalid_item"), TEXT("Skipped delegate entry because it is not a JSON object."));
					continue;
				}
				FString DelegateName;
				if (UeAgentBlueprintFolderOps::TryGetString(*DelegateObjPtr, TEXT("name"), DelegateName) && !DelegateName.IsEmpty())
				{
					DesiredDelegateNames.Add(DelegateName);
				}
				else
				{
					AddWarning(TEXT("delegate_missing_name"), TEXT("Skipped delegate entry because name is missing."));
				}
			}

			TSet<FString> CurrentDelegateNames;
			for (UEdGraph* Graph : Blueprint->DelegateSignatureGraphs)
			{
				if (Graph)
				{
					CurrentDelegateNames.Add(Graph->GetName());
				}
			}

			for (const FString& CurrentDelegateName : CurrentDelegateNames)
			{
				if (!DesiredDelegateNames.Contains(CurrentDelegateName))
				{
					AddWarning(TEXT("delegate_removal_not_supported"), FString::Printf(TEXT("Delegate '%s' exists in asset but is not removable through current command surface."), *CurrentDelegateName));
				}
			}

			for (const FString& DesiredDelegateName : DesiredDelegateNames)
			{
				if (!CurrentDelegateNames.Contains(DesiredDelegateName))
				{
					TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
					AddParams->SetStringField(TEXT("asset_path"), AssetPath);
					AddParams->SetStringField(TEXT("dispatcher_name"), DesiredDelegateName);
					AddParams->SetBoolField(TEXT("compile_after_add"), false);
					AddParams->SetBoolField(TEXT("save_after_add"), false);
					TSharedPtr<FJsonObject> AddData;
					if (!InvokeBlueprintSubCommand(TEXT("blueprint_add_event_dispatcher"), AddParams, AddData, OutError))
					{
						return false;
					}
					++DelegatesAdded;
				}
			}
		}
	}

	TSharedPtr<FJsonObject> ComponentsJson;
	FString ComponentsError;
	if (!UeAgentBlueprintFolderOps::LoadOptionalJsonFile(FPaths::Combine(FolderPath, TEXT("components"), TEXT("tree.json")), ComponentsJson, ComponentsError))
	{
		OutError = ComponentsError;
		return false;
	}
	if (ComponentsJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* DesiredComponentsArray = nullptr;
		if (ComponentsJson->TryGetArrayField(TEXT("components"), DesiredComponentsArray) && DesiredComponentsArray)
		{
			TMap<FString, TSharedPtr<FJsonObject>> DesiredComponents;
			TArray<FString> DesiredOrder;
			for (const TSharedPtr<FJsonValue>& Value : *DesiredComponentsArray)
			{
				const TSharedPtr<FJsonObject>* ComponentObjPtr = nullptr;
				if (!Value.IsValid() || !Value->TryGetObject(ComponentObjPtr) || !ComponentObjPtr || !ComponentObjPtr->IsValid())
				{
					AddWarning(TEXT("component_invalid_item"), TEXT("Skipped component entry because it is not a JSON object."));
					continue;
				}
				FString ComponentName;
				if (!UeAgentBlueprintFolderOps::TryGetString(*ComponentObjPtr, TEXT("name"), ComponentName) || ComponentName.IsEmpty())
				{
					AddWarning(TEXT("component_missing_name"), TEXT("Skipped component entry because name is missing."));
					continue;
				}
				DesiredComponents.Add(ComponentName, *ComponentObjPtr);
				DesiredOrder.Add(ComponentName);
			}

			USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
			if (!SCS)
			{
				if (DesiredOrder.Num() == 0)
				{
					// Some blueprint types, such as WidgetBlueprint, do not have an SCS.
					// Allow the folder workflow to reuse variables/graphs/delegates while treating
					// component application as a no-op when no desired components are present.
				}
				else
				{
					OutError = TEXT("blueprint_has_no_scs");
					return false;
				}
			}
			else
			{
				TArray<USCS_Node*> CurrentNodes = SCS->GetAllNodes();
				TMap<const USCS_Node*, FString> CurrentParentMap;
				for (USCS_Node* RootNode : SCS->GetRootNodes())
				{
					UeAgentBlueprintFolderOps::BuildSCSParentMap(RootNode, FString(), CurrentParentMap);
				}

				for (USCS_Node* CurrentNode : CurrentNodes)
				{
					if (!CurrentNode)
					{
						continue;
					}
					const FString CurrentComponentName = CurrentNode->GetVariableName().ToString();
					if (!DesiredComponents.Contains(CurrentComponentName))
					{
						if (CurrentComponentName.Equals(TEXT("DefaultSceneRoot"), ESearchCase::IgnoreCase))
						{
							AddWarning(TEXT("default_root_preserved"), TEXT("DefaultSceneRoot not present in desired spec; preserving existing root component."));
							continue;
						}

						TSharedPtr<FJsonObject> RemoveParams = MakeShared<FJsonObject>();
						RemoveParams->SetStringField(TEXT("asset_path"), AssetPath);
						RemoveParams->SetStringField(TEXT("component_name"), CurrentComponentName);
						RemoveParams->SetBoolField(TEXT("compile_after_remove"), false);
						RemoveParams->SetBoolField(TEXT("save_after_remove"), false);
						TSharedPtr<FJsonObject> RemoveData;
						if (!InvokeBlueprintSubCommand(TEXT("blueprint_remove_component"), RemoveParams, RemoveData, OutError))
						{
							return false;
						}
						++ComponentsRemoved;
					}
				}

				Blueprint = UeAgentBlueprintOps::LoadBlueprintAsset(AssetPath);
				SCS = Blueprint ? Blueprint->SimpleConstructionScript : nullptr;
				if (!SCS)
				{
					OutError = TEXT("blueprint_has_no_scs");
					return false;
				}

				TSet<FString> ProcessedComponents;
				int32 RemainingComponents = DesiredOrder.Num();
				while (RemainingComponents > 0)
				{
					bool bMadeProgress = false;
					for (const FString& ComponentName : DesiredOrder)
					{
						if (ProcessedComponents.Contains(ComponentName))
						{
							continue;
						}

						const TSharedPtr<FJsonObject>* ComponentObjPtr = DesiredComponents.Find(ComponentName);
						if (!ComponentObjPtr || !ComponentObjPtr->IsValid())
						{
							ProcessedComponents.Add(ComponentName);
							--RemainingComponents;
							continue;
						}

						FString ParentComponentName;
						UeAgentBlueprintFolderOps::TryGetString(*ComponentObjPtr, TEXT("parent"), ParentComponentName);
						if (!ParentComponentName.IsEmpty() && !ProcessedComponents.Contains(ParentComponentName) && !UeAgentBlueprintOps::FindSCSNodeByAnyName(SCS, ParentComponentName))
						{
							continue;
						}

						FString DesiredComponentClassPath;
						if (!UeAgentBlueprintFolderOps::TryGetString(*ComponentObjPtr, TEXT("class"), DesiredComponentClassPath) || DesiredComponentClassPath.IsEmpty())
						{
							OutError = TEXT("missing_component_class");
							return false;
						}

						USCS_Node* ExistingNode = UeAgentBlueprintOps::FindSCSNodeByAnyName(SCS, ComponentName);
						const FString ExistingClassPath = (ExistingNode && ExistingNode->ComponentClass) ? ExistingNode->ComponentClass->GetPathName() : FString();
						const FString ExistingParentName = ExistingNode ? CurrentParentMap.FindRef(ExistingNode) : FString();

						const bool bNeedsRecreate = ExistingNode
							&& (!ExistingClassPath.Equals(DesiredComponentClassPath, ESearchCase::IgnoreCase)
								|| ExistingParentName != ParentComponentName);
						if (bNeedsRecreate && !ComponentName.Equals(TEXT("DefaultSceneRoot"), ESearchCase::IgnoreCase))
						{
							TSharedPtr<FJsonObject> RemoveParams = MakeShared<FJsonObject>();
							RemoveParams->SetStringField(TEXT("asset_path"), AssetPath);
							RemoveParams->SetStringField(TEXT("component_name"), ComponentName);
							RemoveParams->SetBoolField(TEXT("compile_after_remove"), false);
							RemoveParams->SetBoolField(TEXT("save_after_remove"), false);
							TSharedPtr<FJsonObject> RemoveData;
							if (!InvokeBlueprintSubCommand(TEXT("blueprint_remove_component"), RemoveParams, RemoveData, OutError))
							{
								return false;
							}
							++ComponentsRemoved;
							Blueprint = UeAgentBlueprintOps::LoadBlueprintAsset(AssetPath);
							SCS = Blueprint ? Blueprint->SimpleConstructionScript : nullptr;
							ExistingNode = nullptr;
						}

						if (!ExistingNode)
						{
							TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
							AddParams->SetStringField(TEXT("asset_path"), AssetPath);
							AddParams->SetStringField(TEXT("component_class"), DesiredComponentClassPath);
							AddParams->SetStringField(TEXT("component_name"), ComponentName);
							if (!ParentComponentName.IsEmpty())
							{
								AddParams->SetStringField(TEXT("parent_component"), ParentComponentName);
							}
							AddParams->SetBoolField(TEXT("compile_after_add"), false);
							AddParams->SetBoolField(TEXT("save_after_add"), false);
							TSharedPtr<FJsonObject> AddData;
							if (!InvokeBlueprintSubCommand(TEXT("blueprint_add_component"), AddParams, AddData, OutError))
							{
								return false;
							}
							++ComponentsAdded;
							Blueprint = UeAgentBlueprintOps::LoadBlueprintAsset(AssetPath);
							SCS = Blueprint ? Blueprint->SimpleConstructionScript : nullptr;
						}

						ProcessedComponents.Add(ComponentName);
						--RemainingComponents;
						bMadeProgress = true;
					}

					if (!bMadeProgress)
					{
						OutError = TEXT("component_dependency_cycle_or_missing_parent");
						return false;
					}
				}

				for (const FString& ComponentName : DesiredOrder)
				{
					const TSharedPtr<FJsonObject>* ComponentObjPtr = DesiredComponents.Find(ComponentName);
					if (!ComponentObjPtr || !ComponentObjPtr->IsValid())
					{
						continue;
					}

					const TArray<TSharedPtr<FJsonValue>>* PropertiesArray = nullptr;
					if (!(*ComponentObjPtr)->TryGetArrayField(TEXT("properties"), PropertiesArray) || !PropertiesArray)
					{
						continue;
					}

					for (const TSharedPtr<FJsonValue>& PropertyValue : *PropertiesArray)
					{
						const TSharedPtr<FJsonObject>* PropertyObjPtr = nullptr;
						if (!PropertyValue.IsValid() || !PropertyValue->TryGetObject(PropertyObjPtr) || !PropertyObjPtr || !PropertyObjPtr->IsValid())
						{
							continue;
						}
						FString PropertyName;
						FString ValueText;
						if (!UeAgentBlueprintFolderOps::TryGetString(*PropertyObjPtr, TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
						{
							AddWarning(TEXT("component_property_missing_name"), FString::Printf(TEXT("Skipped component property on '%s' because property_name is missing."), *ComponentName));
							continue;
						}
						if (!UeAgentBlueprintFolderOps::TryGetString(*PropertyObjPtr, TEXT("value_text"), ValueText))
						{
							AddWarning(TEXT("component_property_missing_value_text"), FString::Printf(TEXT("Skipped component property '%s' on '%s' because value_text is missing."), *PropertyName, *ComponentName));
							continue;
						}

						USCS_Node* TargetNode = UeAgentBlueprintOps::FindSCSNodeByAnyName(Blueprint->SimpleConstructionScript, ComponentName);
						if (TargetNode && TargetNode->ComponentTemplate && PropertyName.Equals(TEXT("StaticMesh"), ESearchCase::IgnoreCase))
						{
							if (UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(TargetNode->ComponentTemplate))
							{
								const FString ObjectPath = UeAgentBlueprintFolderOps::ExtractObjectPathFromImportText(ValueText);
								UStaticMesh* StaticMesh = LoadObject<UStaticMesh>(nullptr, *ObjectPath);
								if (!StaticMesh)
								{
									OutError = FString::Printf(TEXT("static_mesh_not_found:%s:%s"), *ComponentName, *ValueText);
									return false;
								}
								Blueprint->Modify();
								StaticMeshComponent->Modify();
								StaticMeshComponent->SetStaticMesh(StaticMesh);
								FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
								++ComponentPropertiesApplied;
								continue;
							}
						}

						TSharedPtr<FJsonObject> SetParams = MakeShared<FJsonObject>();
						SetParams->SetStringField(TEXT("asset_path"), AssetPath);
						SetParams->SetStringField(TEXT("component_name"), ComponentName);
						SetParams->SetStringField(TEXT("property_name"), PropertyName);
						SetParams->SetStringField(TEXT("value_text"), ValueText);
						SetParams->SetBoolField(TEXT("compile_after_set"), false);
						SetParams->SetBoolField(TEXT("save_after_set"), false);
						TSharedPtr<FJsonObject> SetData;
						if (!InvokeBlueprintSubCommand(TEXT("blueprint_set_component_property"), SetParams, SetData, OutError))
						{
							return false;
						}
						if (SetData.IsValid() && SetData->HasTypedField<EJson::Boolean>(TEXT("value_text_changed_after_import")) && SetData->GetBoolField(TEXT("value_text_changed_after_import")))
						{
							FString AppliedValueText;
							FString CppType;
							SetData->TryGetStringField(TEXT("applied_value_text"), AppliedValueText);
							SetData->TryGetStringField(TEXT("cpp_type"), CppType);
							AddWarning(
								TEXT("component_property_value_changed_after_import"),
								FString::Printf(TEXT("Component property '%s' on '%s' read back a different value. requested='%s' applied='%s' cpp_type='%s'."),
									*PropertyName,
									*ComponentName,
									*ValueText,
									*AppliedValueText,
									*CppType));
						}
						++ComponentPropertiesApplied;
					}
				}
			}
		}
	}

	struct FGraphSpec
	{
		FString Name;
		FString Kind;
		TSharedPtr<FJsonObject> Root;
	};

	TArray<FGraphSpec> GraphSpecs;
	{
		const FString GraphsDir = FPaths::Combine(FolderPath, TEXT("graphs"));
		TArray<FString> GraphFiles;
		IFileManager::Get().FindFiles(GraphFiles, *(FPaths::Combine(GraphsDir, TEXT("*.json"))), true, false);
		GraphFiles.Sort();
		for (const FString& GraphFile : GraphFiles)
		{
			TSharedPtr<FJsonObject> GraphRoot;
			FString GraphError;
			if (!UeAgentBlueprintFolderOps::LoadJsonFile(FPaths::Combine(GraphsDir, GraphFile), GraphRoot, GraphError))
			{
				OutError = GraphError;
				return false;
			}
			TSharedPtr<FJsonObject> GraphObj;
			if (!UeAgentBlueprintFolderOps::TryGetObjectField(GraphRoot, TEXT("graph"), GraphObj))
			{
				OutError = TEXT("missing_graph_header");
				return false;
			}
			FGraphSpec GraphSpec;
			if (!UeAgentBlueprintFolderOps::TryGetString(GraphObj, TEXT("name"), GraphSpec.Name) || GraphSpec.Name.IsEmpty())
			{
				OutError = TEXT("missing_graph_name");
				return false;
			}
			if (!UeAgentBlueprintFolderOps::TryGetString(GraphObj, TEXT("graph_kind"), GraphSpec.Kind) || GraphSpec.Kind.IsEmpty())
			{
				OutError = TEXT("missing_graph_kind");
				return false;
			}
			GraphSpec.Root = GraphRoot;
			GraphSpecs.Add(GraphSpec);
		}
	}

	auto ResolveBuiltinNodeIds = [](UEdGraph* Graph, TMap<FString, FString>& OutBuiltinIds)
	{
		OutBuiltinIds.Reset();
		if (!Graph)
		{
			return;
		}
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node || !UeAgentBlueprintFolderOps::IsBuiltinGraphNode(Node))
			{
				continue;
			}
			const FString BuiltinId = UeAgentBlueprintFolderOps::GetBuiltinNodeId(Node);
			if (!BuiltinId.IsEmpty())
			{
				OutBuiltinIds.Add(BuiltinId, Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
			}
		}
	};

	auto ApplySingleGraph = [this, &InvokeBlueprintSubCommand, &AssetPath, &OutError, &NodesCreated, &EdgesCreated, &PinDefaultsApplied, &GraphsRebuilt, &ResolveBuiltinNodeIds, &AddWarning](const FGraphSpec& GraphSpec) -> bool
	{
		UBlueprint* WorkingBlueprint = UeAgentBlueprintOps::LoadBlueprintAsset(AssetPath);
		if (!WorkingBlueprint)
		{
			OutError = TEXT("blueprint_not_found");
			return false;
		}

		FString GraphRef = GraphSpec.Name;
		if (GraphSpec.Kind.Equals(TEXT("function_graph"), ESearchCase::IgnoreCase))
		{
			if (!UeAgentBlueprintOps::ResolveBlueprintGraph(WorkingBlueprint, GraphSpec.Name))
			{
				TSharedPtr<FJsonObject> AddGraphParams = MakeShared<FJsonObject>();
				AddGraphParams->SetStringField(TEXT("asset_path"), AssetPath);
				AddGraphParams->SetStringField(TEXT("function_name"), GraphSpec.Name);
				AddGraphParams->SetBoolField(TEXT("compile_after_add"), false);
				AddGraphParams->SetBoolField(TEXT("save_after_add"), false);
				TSharedPtr<FJsonObject> AddGraphData;
				if (!InvokeBlueprintSubCommand(TEXT("blueprint_add_function_graph"), AddGraphParams, AddGraphData, OutError))
				{
					return false;
				}
				GraphRef = AddGraphData->GetStringField(TEXT("graph_path"));
			}
		}
		else if (GraphSpec.Kind.Equals(TEXT("macro_graph"), ESearchCase::IgnoreCase))
		{
			if (!UeAgentBlueprintOps::ResolveBlueprintGraph(WorkingBlueprint, GraphSpec.Name))
			{
				TSharedPtr<FJsonObject> AddGraphParams = MakeShared<FJsonObject>();
				AddGraphParams->SetStringField(TEXT("asset_path"), AssetPath);
				AddGraphParams->SetStringField(TEXT("macro_name"), GraphSpec.Name);
				AddGraphParams->SetBoolField(TEXT("compile_after_add"), false);
				AddGraphParams->SetBoolField(TEXT("save_after_add"), false);
				TSharedPtr<FJsonObject> AddGraphData;
				if (!InvokeBlueprintSubCommand(TEXT("blueprint_add_macro_graph"), AddGraphParams, AddGraphData, OutError))
				{
					return false;
				}
				GraphRef = AddGraphData->GetStringField(TEXT("graph_path"));
			}
		}

		WorkingBlueprint = UeAgentBlueprintOps::LoadBlueprintAsset(AssetPath);
		UEdGraph* Graph = UeAgentBlueprintOps::ResolveBlueprintGraph(WorkingBlueprint, GraphRef);
		if (!Graph)
		{
			OutError = TEXT("graph_not_found");
			return false;
		}

		TMap<FString, FString> BuiltinNodeIds;
		ResolveBuiltinNodeIds(Graph, BuiltinNodeIds);

		TArray<UEdGraphNode*> ExistingNodes = Graph->Nodes;
		for (UEdGraphNode* ExistingNode : ExistingNodes)
		{
			if (!ExistingNode || UeAgentBlueprintFolderOps::IsBuiltinGraphNode(ExistingNode))
			{
				continue;
			}
			Graph->RemoveNode(ExistingNode);
		}
		FBlueprintEditorUtils::MarkBlueprintAsModified(WorkingBlueprint);

		TMap<FString, FString> NodeIdToGuid = BuiltinNodeIds;
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
				FString NodeType;
				if (!UeAgentBlueprintFolderOps::TryGetString(NodeObj, TEXT("id"), NodeId) || NodeId.IsEmpty())
				{
					OutError = TEXT("missing_node_id");
					return false;
				}
				if (!UeAgentBlueprintFolderOps::TryGetString(NodeObj, TEXT("node_type"), NodeType) || NodeType.IsEmpty())
				{
					OutError = TEXT("missing_node_type");
					return false;
				}

				if (NodeIdToGuid.Contains(NodeId))
				{
					continue;
				}

				double PosX = 0.0;
				double PosY = 0.0;
				TSharedPtr<FJsonObject> PosObj;
				if (UeAgentBlueprintFolderOps::TryGetObjectField(NodeObj, TEXT("pos"), PosObj))
				{
					UeAgentBlueprintFolderOps::TryGetNumber(PosObj, TEXT("x"), PosX);
					UeAgentBlueprintFolderOps::TryGetNumber(PosObj, TEXT("y"), PosY);
				}

				TSharedPtr<FJsonObject> AddNodeParams = MakeShared<FJsonObject>();
				AddNodeParams->SetStringField(TEXT("asset_path"), AssetPath);
				AddNodeParams->SetStringField(TEXT("graph_name"), Graph->GetPathName());
				AddNodeParams->SetNumberField(TEXT("pos_x"), PosX);
				AddNodeParams->SetNumberField(TEXT("pos_y"), PosY);
				TSharedPtr<FJsonObject> AddNodeData;

				if (NodeType.Equals(TEXT("event"), ESearchCase::IgnoreCase))
				{
					FString EventName;
					FString EventClass = TEXT("/Script/Engine.Actor");
					if (!UeAgentBlueprintFolderOps::TryGetString(NodeObj, TEXT("event_name"), EventName) || EventName.IsEmpty())
					{
						OutError = TEXT("missing_event_name");
						return false;
					}
					UeAgentBlueprintFolderOps::TryGetString(NodeObj, TEXT("event_class"), EventClass);
					AddNodeParams->SetStringField(TEXT("event_name"), EventName);
					AddNodeParams->SetStringField(TEXT("event_class"), EventClass);
					AddNodeParams->SetBoolField(TEXT("compile_after_add"), false);
					AddNodeParams->SetBoolField(TEXT("save_after_add"), false);
					if (!InvokeBlueprintSubCommand(TEXT("blueprint_add_event_node"), AddNodeParams, AddNodeData, OutError))
					{
						return false;
					}
				}
				else if (NodeType.Equals(TEXT("custom_event"), ESearchCase::IgnoreCase))
				{
					FString EventName;
					if (!UeAgentBlueprintFolderOps::TryGetString(NodeObj, TEXT("event_name"), EventName) || EventName.IsEmpty())
					{
						OutError = TEXT("missing_event_name");
						return false;
					}
					AddNodeParams->SetStringField(TEXT("event_name"), EventName);
					AddNodeParams->SetBoolField(TEXT("compile_after_add"), false);
					AddNodeParams->SetBoolField(TEXT("save_after_add"), false);
					if (!InvokeBlueprintSubCommand(TEXT("blueprint_add_custom_event_node"), AddNodeParams, AddNodeData, OutError))
					{
						return false;
					}
				}
				else if (NodeType.Equals(TEXT("component_bound_event"), ESearchCase::IgnoreCase))
				{
					FString ComponentName;
					FString DelegatePropertyName;
					FString DelegateOwnerClass = TEXT("/Script/Engine.PrimitiveComponent");
					if (!UeAgentBlueprintFolderOps::TryGetString(NodeObj, TEXT("component_name"), ComponentName) || ComponentName.IsEmpty())
					{
						OutError = TEXT("missing_component_name");
						return false;
					}
					if (!UeAgentBlueprintFolderOps::TryGetString(NodeObj, TEXT("delegate_property_name"), DelegatePropertyName) || DelegatePropertyName.IsEmpty())
					{
						OutError = TEXT("missing_delegate_property_name");
						return false;
					}
					UeAgentBlueprintFolderOps::TryGetString(NodeObj, TEXT("delegate_owner_class"), DelegateOwnerClass);
					AddNodeParams->SetStringField(TEXT("component_name"), ComponentName);
					AddNodeParams->SetStringField(TEXT("delegate_property_name"), DelegatePropertyName);
					AddNodeParams->SetStringField(TEXT("delegate_owner_class"), DelegateOwnerClass);
					AddNodeParams->SetBoolField(TEXT("compile_after_add"), false);
					AddNodeParams->SetBoolField(TEXT("save_after_add"), false);
					if (!InvokeBlueprintSubCommand(TEXT("blueprint_add_component_bound_event"), AddNodeParams, AddNodeData, OutError))
					{
						return false;
					}
				}
				else if (NodeType.Equals(TEXT("enhanced_input_action_event"), ESearchCase::IgnoreCase))
				{
					FString InputActionPath;
					if (!UeAgentBlueprintFolderOps::TryGetString(NodeObj, TEXT("input_action"), InputActionPath) || InputActionPath.IsEmpty())
					{
						OutError = TEXT("missing_input_action");
						return false;
					}
					AddNodeParams->SetStringField(TEXT("input_action"), InputActionPath);
					AddNodeParams->SetBoolField(TEXT("compile_after_add"), false);
					AddNodeParams->SetBoolField(TEXT("save_after_add"), false);
					if (!InvokeBlueprintSubCommand(TEXT("blueprint_add_enhanced_input_action_event"), AddNodeParams, AddNodeData, OutError))
					{
						return false;
					}
				}
				else if (NodeType.Equals(TEXT("enhanced_input_get_local_player_subsystem"), ESearchCase::IgnoreCase))
				{
					AddNodeParams->SetBoolField(TEXT("compile_after_add"), false);
					AddNodeParams->SetBoolField(TEXT("save_after_add"), false);
					if (!InvokeBlueprintSubCommand(TEXT("blueprint_add_enhanced_input_local_player_subsystem_node"), AddNodeParams, AddNodeData, OutError))
					{
						return false;
					}
				}
				else if (NodeType.Equals(TEXT("enhanced_input_add_mapping_context"), ESearchCase::IgnoreCase))
				{
					FString MappingContextPath;
					double PriorityValue = 0.0;
					FString OptionsDefaultValue = TEXT("(bIgnoreAllPressedKeysUntilRelease=True,bForceImmediately=False,bNotifyUserSettings=False)");
					if (!UeAgentBlueprintFolderOps::TryGetString(NodeObj, TEXT("mapping_context"), MappingContextPath) || MappingContextPath.IsEmpty())
					{
						OutError = TEXT("missing_mapping_context");
						return false;
					}
					UeAgentBlueprintFolderOps::TryGetNumber(NodeObj, TEXT("priority"), PriorityValue);
					UeAgentBlueprintFolderOps::TryGetString(NodeObj, TEXT("options_default_value"), OptionsDefaultValue);
					AddNodeParams->SetStringField(TEXT("mapping_context"), MappingContextPath);
					AddNodeParams->SetNumberField(TEXT("priority"), PriorityValue);
					AddNodeParams->SetStringField(TEXT("options_default_value"), OptionsDefaultValue);
					AddNodeParams->SetBoolField(TEXT("compile_after_add"), false);
					AddNodeParams->SetBoolField(TEXT("save_after_add"), false);
					if (!InvokeBlueprintSubCommand(TEXT("blueprint_add_enhanced_input_add_mapping_context_node"), AddNodeParams, AddNodeData, OutError))
					{
						return false;
					}
				}
				else if (NodeType.Equals(TEXT("dynamic_cast"), ESearchCase::IgnoreCase))
				{
					FString TargetClass;
					bool bPureCast = true;
					if (!UeAgentBlueprintFolderOps::TryGetString(NodeObj, TEXT("target_class"), TargetClass) || TargetClass.IsEmpty())
					{
						OutError = TEXT("missing_target_class");
						return false;
					}
					UeAgentBlueprintFolderOps::TryGetBool(NodeObj, TEXT("pure"), bPureCast);
					AddNodeParams->SetStringField(TEXT("target_class"), TargetClass);
					AddNodeParams->SetBoolField(TEXT("pure"), bPureCast);
					AddNodeParams->SetBoolField(TEXT("compile_after_add"), false);
					AddNodeParams->SetBoolField(TEXT("save_after_add"), false);
					if (!InvokeBlueprintSubCommand(TEXT("blueprint_add_dynamic_cast_node"), AddNodeParams, AddNodeData, OutError))
					{
						return false;
					}
				}
				else if (NodeType.Equals(TEXT("call_function"), ESearchCase::IgnoreCase))
				{
					FString FunctionOwnerClass;
					FString FunctionName;
					if (!UeAgentBlueprintFolderOps::TryGetString(NodeObj, TEXT("function_owner_class"), FunctionOwnerClass) || FunctionOwnerClass.IsEmpty())
					{
						OutError = TEXT("missing_function_owner_class");
						return false;
					}
					if (!UeAgentBlueprintFolderOps::TryGetString(NodeObj, TEXT("function_name"), FunctionName) || FunctionName.IsEmpty())
					{
						OutError = TEXT("missing_function_name");
						return false;
					}
					AddNodeParams->SetStringField(TEXT("function_owner_class"), FunctionOwnerClass);
					AddNodeParams->SetStringField(TEXT("function_name"), FunctionName);
					AddNodeParams->SetBoolField(TEXT("compile_after_add"), false);
					AddNodeParams->SetBoolField(TEXT("save_after_add"), false);
					if (!InvokeBlueprintSubCommand(TEXT("blueprint_add_call_function_node"), AddNodeParams, AddNodeData, OutError))
					{
						return false;
					}
				}
				else if (NodeType.Equals(TEXT("variable_node"), ESearchCase::IgnoreCase))
				{
					FString VariableName;
					FString Access = TEXT("get");
					FString MemberParentClass;
					if (!UeAgentBlueprintFolderOps::TryGetString(NodeObj, TEXT("variable_name"), VariableName) || VariableName.IsEmpty())
					{
						OutError = TEXT("missing_variable_name");
						return false;
					}
					UeAgentBlueprintFolderOps::TryGetString(NodeObj, TEXT("access"), Access);
					UeAgentBlueprintFolderOps::TryGetString(NodeObj, TEXT("member_parent_class"), MemberParentClass);
					AddNodeParams->SetStringField(TEXT("variable_name"), VariableName);
					AddNodeParams->SetStringField(TEXT("node_type"), Access);
					if (!MemberParentClass.IsEmpty())
					{
						AddNodeParams->SetStringField(TEXT("member_parent_class"), MemberParentClass);
					}
					AddNodeParams->SetBoolField(TEXT("compile_after_add"), false);
					AddNodeParams->SetBoolField(TEXT("save_after_add"), false);
					if (!InvokeBlueprintSubCommand(TEXT("blueprint_add_variable_node"), AddNodeParams, AddNodeData, OutError))
					{
						return false;
					}
				}
				else if (NodeType.Equals(TEXT("node_by_class"), ESearchCase::IgnoreCase))
				{
					FString NodeClass;
					if (!UeAgentBlueprintFolderOps::TryGetString(NodeObj, TEXT("node_class"), NodeClass) || NodeClass.IsEmpty())
					{
						OutError = TEXT("missing_node_class");
						return false;
					}
					AddNodeParams->SetStringField(TEXT("node_class"), NodeClass);
					AddNodeParams->SetBoolField(TEXT("compile_after_add"), false);
					AddNodeParams->SetBoolField(TEXT("save_after_add"), false);
					if (!InvokeBlueprintSubCommand(TEXT("blueprint_add_node_by_class"), AddNodeParams, AddNodeData, OutError))
					{
						return false;
					}
				}
				else
				{
					OutError = TEXT("unsupported_graph_node_type");
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
				if (!UeAgentBlueprintFolderOps::TryGetString(NodeObj, TEXT("id"), NodeId) || NodeId.IsEmpty())
				{
					continue;
				}
				const FString* NodeGuid = NodeIdToGuid.Find(NodeId);
				if (!NodeGuid)
				{
					continue;
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
					if (!UeAgentBlueprintFolderOps::TryGetString(*PinDefaultObjPtr, TEXT("pin_name"), PinName) || PinName.IsEmpty())
					{
						continue;
					}
					if (!UeAgentBlueprintFolderOps::TryGetString(*PinDefaultObjPtr, TEXT("default_value"), DefaultValue))
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
					if (!InvokeBlueprintSubCommand(TEXT("blueprint_set_pin_default_value"), PinParams, PinData, OutError))
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
				if (!UeAgentBlueprintFolderOps::TryGetObjectField(*EdgeObjPtr, TEXT("from"), FromObj)
					|| !UeAgentBlueprintFolderOps::TryGetObjectField(*EdgeObjPtr, TEXT("to"), ToObj))
				{
					continue;
				}

				FString FromNodeId;
				FString FromPinName;
				FString ToNodeId;
				FString ToPinName;
				if (!UeAgentBlueprintFolderOps::TryGetString(FromObj, TEXT("node"), FromNodeId) || FromNodeId.IsEmpty()
					|| !UeAgentBlueprintFolderOps::TryGetString(FromObj, TEXT("pin"), FromPinName) || FromPinName.IsEmpty()
					|| !UeAgentBlueprintFolderOps::TryGetString(ToObj, TEXT("node"), ToNodeId) || ToNodeId.IsEmpty()
					|| !UeAgentBlueprintFolderOps::TryGetString(ToObj, TEXT("pin"), ToPinName) || ToPinName.IsEmpty())
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
				if (!InvokeBlueprintSubCommand(TEXT("blueprint_connect_pins"), ConnectParams, ConnectData, OutError))
				{
					OutError = FString::Printf(TEXT("connect_edge_failed:%s:%s.%s->%s.%s"), *Graph->GetName(), *FromNodeId, *FromPinName, *ToNodeId, *ToPinName);
					return false;
				}
				++EdgesCreated;
			}
		}

		++GraphsRebuilt;
		return true;
	};

	for (const FGraphSpec& GraphSpec : GraphSpecs)
	{
		if (!ApplySingleGraph(GraphSpec))
		{
			return false;
		}
	}

	TSharedPtr<FJsonObject> DefaultsJson;
	FString DefaultsError;
	if (!UeAgentBlueprintFolderOps::LoadOptionalJsonFile(FPaths::Combine(FolderPath, TEXT("members"), TEXT("defaults.json")), DefaultsJson, DefaultsError))
	{
		OutError = DefaultsError;
		return false;
	}
	if (DefaultsJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* DefaultsArray = nullptr;
		if (DefaultsJson->TryGetArrayField(TEXT("defaults"), DefaultsArray) && DefaultsArray)
		{
			for (const TSharedPtr<FJsonValue>& DefaultValue : *DefaultsArray)
			{
				const TSharedPtr<FJsonObject>* DefaultObjPtr = nullptr;
				if (!DefaultValue.IsValid() || !DefaultValue->TryGetObject(DefaultObjPtr) || !DefaultObjPtr || !DefaultObjPtr->IsValid())
				{
					AddWarning(TEXT("default_invalid_item"), TEXT("Skipped CDO default entry because it is not a JSON object."));
					continue;
				}
				FString PropertyName;
				FString ValueText;
				if (!UeAgentBlueprintFolderOps::TryGetString(*DefaultObjPtr, TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
				{
					AddWarning(TEXT("default_missing_property_name"), TEXT("Skipped CDO default entry because property_name is missing."));
					continue;
				}
				if (!UeAgentBlueprintFolderOps::TryGetString(*DefaultObjPtr, TEXT("value_text"), ValueText))
				{
					AddWarning(TEXT("default_missing_value_text"), FString::Printf(TEXT("Skipped CDO default '%s' because value_text is missing."), *PropertyName));
					continue;
				}

				TSharedPtr<FJsonObject> DefaultParams = MakeShared<FJsonObject>();
				DefaultParams->SetStringField(TEXT("asset_path"), AssetPath);
				DefaultParams->SetStringField(TEXT("property_name"), PropertyName);
				DefaultParams->SetStringField(TEXT("value_text"), ValueText);
				TSharedPtr<FJsonObject> DefaultData;
				if (!InvokeBlueprintSubCommand(TEXT("blueprint_set_cdo_property"), DefaultParams, DefaultData, OutError))
				{
					return false;
				}
				++DefaultsApplied;
			}
		}
	}

	Blueprint = UeAgentBlueprintOps::LoadBlueprintAsset(AssetPath);
	if (!Blueprint)
	{
		OutError = TEXT("blueprint_not_found");
		return false;
	}

	if (bCompileAfterApply)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
	}
	if (bSaveAfterApply && !UeAgentBlueprintOps::SaveBlueprintPackage(Blueprint, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), AssetPath);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetStringField(TEXT("root_path"), UeAgentBlueprintFolderOps::GetFolderRootAbsolute());
	OutData->SetNumberField(TEXT("variables_added"), VariablesAdded);
	OutData->SetNumberField(TEXT("variables_removed"), VariablesRemoved);
	OutData->SetNumberField(TEXT("variable_defaults_updated"), VariableDefaultsUpdated);
	OutData->SetNumberField(TEXT("delegates_added"), DelegatesAdded);
	OutData->SetNumberField(TEXT("components_added"), ComponentsAdded);
	OutData->SetNumberField(TEXT("components_removed"), ComponentsRemoved);
	OutData->SetNumberField(TEXT("component_properties_applied"), ComponentPropertiesApplied);
	OutData->SetNumberField(TEXT("graphs_created"), GraphsCreated);
	OutData->SetNumberField(TEXT("graphs_rebuilt"), GraphsRebuilt);
	OutData->SetNumberField(TEXT("nodes_created"), NodesCreated);
	OutData->SetNumberField(TEXT("pin_defaults_applied"), PinDefaultsApplied);
	OutData->SetNumberField(TEXT("edges_created"), EdgesCreated);
	OutData->SetNumberField(TEXT("defaults_applied"), DefaultsApplied);
	OutData->SetNumberField(TEXT("warning_count"), Warnings.Num());
	OutData->SetArrayField(TEXT("warnings"), Warnings);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterApply);
	OutData->SetBoolField(TEXT("compiled"), bCompileAfterApply);
	return true;
}

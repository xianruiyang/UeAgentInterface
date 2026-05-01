namespace UeAgentBlueprintOps
{
	static USCS_Node* FindSCSNodeByAnyName(USimpleConstructionScript* SCS, const FString& InComponentName)
	{
		if (!SCS || InComponentName.IsEmpty())
		{
			return nullptr;
		}
		if (USCS_Node* Node = SCS->FindSCSNode(FName(*InComponentName)))
		{
			return Node;
		}
		for (USCS_Node* Node : SCS->GetAllNodes())
		{
			if (!Node)
			{
				continue;
			}
			if (Node->GetVariableName().ToString().Equals(InComponentName, ESearchCase::IgnoreCase))
			{
				return Node;
			}
			if (Node->ComponentTemplate && Node->ComponentTemplate->GetName().Equals(InComponentName, ESearchCase::IgnoreCase))
			{
				return Node;
			}
		}
		return nullptr;
	}

	static UEdGraphNode* FindGraphNodeByGuid(UEdGraph* Graph, const FString& InNodeGuid)
	{
		if (!Graph || InNodeGuid.IsEmpty())
		{
			return nullptr;
		}
		FGuid Guid;
		if (!FGuid::Parse(InNodeGuid, Guid))
		{
			return nullptr;
		}
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->NodeGuid == Guid)
			{
				return Node;
			}
		}
		return nullptr;
	}

	static UEdGraphPin* FindNodePinByName(UEdGraphNode* Node, const FString& InPinName)
	{
		if (!Node || InPinName.IsEmpty())
		{
			return nullptr;
		}
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->PinName.ToString().Equals(InPinName, ESearchCase::IgnoreCase))
			{
				return Pin;
			}
		}
		return nullptr;
	}

	static FProperty* FindPropertyByNameLoose(UStruct* StructType, const FString& Segment)
	{
		if (!StructType || Segment.IsEmpty())
		{
			return nullptr;
		}
		const FString Normalized = Segment.TrimStartAndEnd();
		const FString NormalizedWithoutB = Normalized.StartsWith(TEXT("b")) ? Normalized.Mid(1) : Normalized;
		for (TFieldIterator<FProperty> It(StructType, EFieldIterationFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
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
			if (StructProperty)
			{
				CurrentStruct = StructProperty->Struct;
				CurrentContainer = ValuePtr;
				continue;
			}

			FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property);
			if (ObjectProperty)
			{
				UObject* NestedObject = ObjectProperty->GetObjectPropertyValue(ValuePtr);
				if (!NestedObject)
				{
					return false;
				}
				CurrentStruct = NestedObject->GetClass();
				CurrentContainer = NestedObject;
				continue;
			}

			return false;
		}
		return false;
	}
}

bool FUeAgentHttpServer::CmdBlueprintInspectComponents(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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
	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
	if (!SCS)
	{
		OutError = TEXT("blueprint_has_no_scs");
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Components;
	for (USCS_Node* Node : SCS->GetAllNodes())
	{
		if (!Node)
		{
			continue;
		}
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("source"), TEXT("scs"));
		Item->SetStringField(TEXT("variable_name"), Node->GetVariableName().ToString());
		Item->SetStringField(TEXT("component_class"), Node->ComponentClass ? Node->ComponentClass->GetPathName() : TEXT(""));
		Item->SetStringField(TEXT("parent_component"), TEXT("None"));
		Item->SetStringField(TEXT("parent_owner_class"), TEXT("None"));
		Item->SetNumberField(TEXT("child_count"), Node->ChildNodes.Num());
		Item->SetStringField(TEXT("template_name"), Node->ComponentTemplate ? Node->ComponentTemplate->GetName() : TEXT(""));
		Components.Add(MakeShared<FJsonValueObject>(Item));
	}

	OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());
	OutData->SetNumberField(TEXT("component_count"), Components.Num());
	OutData->SetArrayField(TEXT("components"), Components);
	return true;
}

bool FUeAgentHttpServer::CmdBlueprintInspectNodes(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

	int32 LimitPerGraph = 200;
	double LimitNum = 0.0;
	if (JsonTryGetNumber(Ctx.Params, TEXT("limit_per_graph"), LimitNum))
	{
		LimitPerGraph = FMath::Max(1, (int32)LimitNum);
	}
	bool bIncludePins = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_pins"), bIncludePins);
	FString GraphNameFilter;
	JsonTryGetString(Ctx.Params, TEXT("graph_name"), GraphNameFilter);
	GraphNameFilter.TrimStartAndEndInline();
	UEdGraph* ResolvedGraphFilter = nullptr;

	struct FGraphInfo
	{
		UEdGraph* Graph = nullptr;
		FString Type;
	};
	TArray<FGraphInfo> GraphInfos;
	auto AddGraphList = [&GraphInfos](const TArray<UEdGraph*>& InGraphs, const FString& TypeName)
	{
		for (UEdGraph* Graph : InGraphs)
		{
			if (Graph)
			{
				GraphInfos.Add({ Graph, TypeName });
			}
		}
	};
	AddGraphList(Blueprint->UbergraphPages, TEXT("ubergraph"));
	AddGraphList(Blueprint->FunctionGraphs, TEXT("function"));
	AddGraphList(Blueprint->MacroGraphs, TEXT("macro"));
	AddGraphList(Blueprint->DelegateSignatureGraphs, TEXT("delegate"));

	if (!GraphNameFilter.IsEmpty())
	{
		ResolvedGraphFilter = UeAgentBlueprintOps::ResolveBlueprintGraph(Blueprint, GraphNameFilter);
		if (!ResolvedGraphFilter)
		{
			OutError = TEXT("graph_not_found");
			return false;
		}

		bool bAlreadyAdded = false;
		for (const FGraphInfo& GraphInfo : GraphInfos)
		{
			if (GraphInfo.Graph == ResolvedGraphFilter)
			{
				bAlreadyAdded = true;
				break;
			}
		}

		if (!bAlreadyAdded)
		{
			GraphInfos.Reset();
			GraphInfos.Add({ ResolvedGraphFilter, TEXT("subgraph") });
		}
	}

	int32 TotalNodeCount = 0;
	TArray<TSharedPtr<FJsonValue>> GraphArray;
	for (const FGraphInfo& GraphInfo : GraphInfos)
	{
		UEdGraph* Graph = GraphInfo.Graph;
		if (!Graph)
		{
			continue;
		}
		if (ResolvedGraphFilter && Graph != ResolvedGraphFilter)
		{
			continue;
		}

		TotalNodeCount += Graph->Nodes.Num();
		TSharedPtr<FJsonObject> GraphObj = MakeShared<FJsonObject>();
		GraphObj->SetStringField(TEXT("graph_name"), Graph->GetName());
		GraphObj->SetStringField(TEXT("graph_type"), GraphInfo.Type);
		GraphObj->SetStringField(TEXT("graph_path"), Graph->GetPathName());
		GraphObj->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());

		TArray<TSharedPtr<FJsonValue>> NodeArray;
		for (int32 NodeIdx = 0; NodeIdx < Graph->Nodes.Num() && NodeIdx < LimitPerGraph; ++NodeIdx)
		{
			UEdGraphNode* Node = Graph->Nodes[NodeIdx];
			if (!Node)
			{
				continue;
			}
			TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
			NodeObj->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
			NodeObj->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
			NodeObj->SetStringField(TEXT("class"), Node->GetClass()->GetPathName());
			NodeObj->SetStringField(TEXT("comment"), Node->NodeComment);
			NodeObj->SetNumberField(TEXT("pos_x"), Node->NodePosX);
			NodeObj->SetNumberField(TEXT("pos_y"), Node->NodePosY);
			NodeObj->SetNumberField(TEXT("pin_count"), Node->Pins.Num());

			if (bIncludePins)
			{
				TArray<TSharedPtr<FJsonValue>> Pins;
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (!Pin)
					{
						continue;
					}
					TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
					PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
					PinObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
					PinObj->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
					PinObj->SetStringField(TEXT("subcategory"), Pin->PinType.PinSubCategory.ToString());
					PinObj->SetStringField(TEXT("default_value"), Pin->DefaultValue);
					PinObj->SetNumberField(TEXT("link_count"), Pin->LinkedTo.Num());
					Pins.Add(MakeShared<FJsonValueObject>(PinObj));
				}
				NodeObj->SetArrayField(TEXT("pins"), Pins);
			}
			NodeArray.Add(MakeShared<FJsonValueObject>(NodeObj));
		}

		GraphObj->SetNumberField(TEXT("returned_node_count"), NodeArray.Num());
		GraphObj->SetArrayField(TEXT("nodes"), NodeArray);
		GraphArray.Add(MakeShared<FJsonValueObject>(GraphObj));
	}

	OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());
	OutData->SetNumberField(TEXT("graph_count"), GraphArray.Num());
	OutData->SetNumberField(TEXT("total_node_count"), TotalNodeCount);
	OutData->SetNumberField(TEXT("limit_per_graph"), LimitPerGraph);
	OutData->SetArrayField(TEXT("graphs"), GraphArray);
	return true;
}

bool FUeAgentHttpServer::CmdBlueprintAddComponent(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	FString ComponentClassPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("component_class"), ComponentClassPath) || ComponentClassPath.IsEmpty())
	{
		OutError = TEXT("missing_component_class");
		return false;
	}
	FString ComponentName;
	if (!JsonTryGetString(Ctx.Params, TEXT("component_name"), ComponentName) || ComponentName.IsEmpty())
	{
		OutError = TEXT("missing_component_name");
		return false;
	}

	UBlueprint* Blueprint = UeAgentBlueprintOps::LoadBlueprintAsset(AssetPathInput);
	if (!Blueprint)
	{
		OutError = TEXT("blueprint_not_found");
		return false;
	}
	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
	if (!SCS)
	{
		OutError = TEXT("blueprint_has_no_scs");
		return false;
	}
	UClass* ComponentClass = UeAgentBlueprintOps::ResolveClassByPath(ComponentClassPath);
	if (!ComponentClass || !ComponentClass->IsChildOf(UActorComponent::StaticClass()))
	{
		OutError = TEXT("invalid_component_class");
		return false;
	}
	if (UeAgentBlueprintOps::FindSCSNodeByAnyName(SCS, ComponentName))
	{
		OutError = TEXT("component_already_exists");
		return false;
	}

	USCS_Node* NewNode = SCS->CreateNode(ComponentClass, FName(*ComponentName));
	if (!NewNode)
	{
		OutError = TEXT("create_scs_node_failed");
		return false;
	}
	FString ParentComponentName;
	JsonTryGetString(Ctx.Params, TEXT("parent_component"), ParentComponentName);
	USCS_Node* ParentNode = UeAgentBlueprintOps::FindSCSNodeByAnyName(SCS, ParentComponentName);
	if (ParentNode)
	{
		ParentNode->AddChildNode(NewNode);
	}
	else
	{
		SCS->AddNode(NewNode);
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	bool bCompileAfterAdd = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_add"), bCompileAfterAdd);
	if (bCompileAfterAdd)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
	}
	bool bSaveAfterAdd = true;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_add"), bSaveAfterAdd);
	if (bSaveAfterAdd && !UeAgentBlueprintOps::SaveBlueprintPackage(Blueprint, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("component_name"), NewNode->GetVariableName().ToString());
	OutData->SetStringField(TEXT("component_class"), ComponentClass->GetPathName());
	OutData->SetBoolField(TEXT("saved"), bSaveAfterAdd);
	return true;
}

bool FUeAgentHttpServer::CmdBlueprintSetComponentProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	FString ComponentName;
	if (!JsonTryGetString(Ctx.Params, TEXT("component_name"), ComponentName) || ComponentName.IsEmpty())
	{
		OutError = TEXT("missing_component_name");
		return false;
	}
	FString PropertyName;
	if (!JsonTryGetString(Ctx.Params, TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		OutError = TEXT("missing_property_name");
		return false;
	}
	FString ValueText;
	if (!JsonTryGetString(Ctx.Params, TEXT("value_text"), ValueText))
	{
		OutError = TEXT("missing_value_text");
		return false;
	}

	UBlueprint* Blueprint = UeAgentBlueprintOps::LoadBlueprintAsset(AssetPathInput);
	if (!Blueprint)
	{
		OutError = TEXT("blueprint_not_found");
		return false;
	}
	USCS_Node* TargetNode = UeAgentBlueprintOps::FindSCSNodeByAnyName(Blueprint->SimpleConstructionScript, ComponentName);
	if (!TargetNode || !TargetNode->ComponentTemplate)
	{
		OutError = TEXT("component_not_found_in_scs");
		return false;
	}

	FProperty* Property = nullptr;
	void* ValuePtr = nullptr;
	UObject* Template = TargetNode->ComponentTemplate;
	if (!UeAgentBlueprintOps::ResolvePropertyPath(Template, PropertyName, Property, ValuePtr) || !Property || !ValuePtr)
	{
		OutError = TEXT("property_not_found");
		return false;
	}

	Blueprint->Modify();
	Template->Modify();
	if (!Property->ImportText_Direct(*ValueText, ValuePtr, Template, PPF_None))
	{
		const FString ImportError = FString::Printf(TEXT("property_import_failed:%s:%s"), *PropertyName, *ValueText);
		OutData->SetStringField(TEXT("component_name"), ComponentName);
		OutData->SetStringField(TEXT("property_name"), PropertyName);
		OutData->SetStringField(TEXT("value_text"), ValueText);
		SetPropertyImportResultFields(OutData, Property, ValueText, TEXT(""), TEXT("import_failed"), ImportError);
		OutError = ImportError;
		return false;
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	bool bCompileAfterSet = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_set"), bCompileAfterSet);
	if (bCompileAfterSet)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
	}
	bool bSaveAfterSet = true;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentBlueprintOps::SaveBlueprintPackage(Blueprint, OutError))
	{
		return false;
	}

	FString ExportedValue;
	Property->ExportTextItem_Direct(ExportedValue, ValuePtr, ValuePtr, Template, PPF_None);
	OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("component_name"), ComponentName);
	OutData->SetStringField(TEXT("property_name"), PropertyName);
	OutData->SetStringField(TEXT("value_text"), ValueText);
	OutData->SetStringField(TEXT("applied_value_text"), ExportedValue);
	SetPropertyImportResultFields(OutData, Property, ValueText, ExportedValue, TEXT("imported_and_read_back"));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}


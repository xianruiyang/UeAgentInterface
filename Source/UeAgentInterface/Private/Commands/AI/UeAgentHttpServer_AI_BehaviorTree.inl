namespace UeAgentAIOps
{
	static UClass* ResolveBTClass(const FString& ClassOrKind, const UClass* RequiredBase)
	{
		FString Lower = ClassOrKind.ToLower();
		UClass* Class = nullptr;
		if (Lower == TEXT("sequence"))
		{
			Class = UBTComposite_Sequence::StaticClass();
		}
		else if (Lower == TEXT("selector"))
		{
			Class = UBTComposite_Selector::StaticClass();
		}
		else if (Lower == TEXT("wait"))
		{
			Class = UBTTask_Wait::StaticClass();
		}
		else
		{
			Class = ResolveClass(ClassOrKind);
		}
		return (Class && Class->IsChildOf(RequiredBase)) ? Class : nullptr;
	}

	static FString BTKindForNode(const UBTNode* Node)
	{
		if (Cast<const UBTCompositeNode>(Node)) return TEXT("composite");
		if (Cast<const UBTTaskNode>(Node)) return TEXT("task");
		if (Cast<const UBTDecorator>(Node)) return TEXT("decorator");
		if (Cast<const UBTService>(Node)) return TEXT("service");
		return TEXT("node");
	}

	static TSharedPtr<FJsonObject> ExportBTNode(const UBTNode* Node, const FString& Id)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!Node)
		{
			return Obj;
		}
		Obj->SetStringField(TEXT("id"), Id);
		Obj->SetStringField(TEXT("kind"), BTKindForNode(Node));
		Obj->SetStringField(TEXT("class"), Node->GetClass()->GetPathName());
		Obj->SetStringField(TEXT("name"), Node->NodeName);
		Obj->SetStringField(TEXT("static_description"), Node->GetStaticDescription());
		return Obj;
	}

	static void TraverseBTComposite(const UBTCompositeNode* Composite, const FString& Id, TArray<TSharedPtr<FJsonValue>>& Nodes, TArray<TSharedPtr<FJsonValue>>& Edges, int32& Counter)
	{
		if (!Composite)
		{
			return;
		}
		Nodes.Add(MakeShared<FJsonValueObject>(ExportBTNode(Composite, Id)));

		for (int32 ServiceIndex = 0; ServiceIndex < Composite->Services.Num(); ++ServiceIndex)
		{
			const FString ServiceId = FString::Printf(TEXT("%s_service_%d"), *Id, ServiceIndex);
			Nodes.Add(MakeShared<FJsonValueObject>(ExportBTNode(Composite->Services[ServiceIndex], ServiceId)));
			TSharedPtr<FJsonObject> Edge = MakeShared<FJsonObject>();
			Edge->SetStringField(TEXT("parent_id"), Id);
			Edge->SetStringField(TEXT("child_id"), ServiceId);
			Edge->SetStringField(TEXT("relation"), TEXT("service"));
			Edges.Add(MakeShared<FJsonValueObject>(Edge));
		}

		for (int32 ChildIndex = 0; ChildIndex < Composite->Children.Num(); ++ChildIndex)
		{
			const FBTCompositeChild& Child = Composite->Children[ChildIndex];
			const UBTNode* ChildNode = Child.ChildComposite ? static_cast<const UBTNode*>(Child.ChildComposite.Get()) : static_cast<const UBTNode*>(Child.ChildTask.Get());
			const FString ChildId = FString::Printf(TEXT("node_%d"), Counter++);
			TSharedPtr<FJsonObject> Edge = MakeShared<FJsonObject>();
			Edge->SetStringField(TEXT("parent_id"), Id);
			Edge->SetStringField(TEXT("child_id"), ChildId);
			Edge->SetStringField(TEXT("relation"), TEXT("child"));
			Edge->SetNumberField(TEXT("order"), ChildIndex);
			TArray<TSharedPtr<FJsonValue>> DecoratorIds;
			for (int32 DecoratorIndex = 0; DecoratorIndex < Child.Decorators.Num(); ++DecoratorIndex)
			{
				const FString DecoratorId = FString::Printf(TEXT("%s_decorator_%d"), *ChildId, DecoratorIndex);
				DecoratorIds.Add(MakeShared<FJsonValueString>(DecoratorId));
				Nodes.Add(MakeShared<FJsonValueObject>(ExportBTNode(Child.Decorators[DecoratorIndex], DecoratorId)));
			}
			Edge->SetArrayField(TEXT("decorator_ids"), DecoratorIds);
			Edges.Add(MakeShared<FJsonValueObject>(Edge));

			if (const UBTCompositeNode* ChildComposite = Cast<UBTCompositeNode>(ChildNode))
			{
				TraverseBTComposite(ChildComposite, ChildId, Nodes, Edges, Counter);
			}
			else
			{
				Nodes.Add(MakeShared<FJsonValueObject>(ExportBTNode(ChildNode, ChildId)));
				if (const UBTTaskNode* Task = Cast<UBTTaskNode>(ChildNode))
				{
					for (int32 ServiceIndex = 0; ServiceIndex < Task->Services.Num(); ++ServiceIndex)
					{
						const FString ServiceId = FString::Printf(TEXT("%s_service_%d"), *ChildId, ServiceIndex);
						Nodes.Add(MakeShared<FJsonValueObject>(ExportBTNode(Task->Services[ServiceIndex], ServiceId)));
						TSharedPtr<FJsonObject> ServiceEdge = MakeShared<FJsonObject>();
						ServiceEdge->SetStringField(TEXT("parent_id"), ChildId);
						ServiceEdge->SetStringField(TEXT("child_id"), ServiceId);
						ServiceEdge->SetStringField(TEXT("relation"), TEXT("service"));
						Edges.Add(MakeShared<FJsonValueObject>(ServiceEdge));
					}
				}
			}
		}
	}

	static TSharedPtr<FJsonObject> ExportBehaviorTree(UBehaviorTree* Tree, const bool bIncludeNodes)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("profile"), BehaviorTreeProfile);
		Root->SetNumberField(TEXT("schema_version"), 1);
		Root->SetStringField(TEXT("asset_path"), Tree ? Tree->GetOutermost()->GetName() : FString());
		Root->SetStringField(TEXT("object_path"), Tree ? Tree->GetPathName() : FString());
		Root->SetStringField(TEXT("blackboard_asset"), (Tree && Tree->BlackboardAsset) ? Tree->BlackboardAsset->GetPathName() : FString());
		Root->SetStringField(TEXT("root_id"), Tree && Tree->RootNode ? TEXT("root") : FString());
		Root->SetBoolField(TEXT("dirty"), Tree && Tree->GetOutermost()->IsDirty());
		if (bIncludeNodes && Tree && Tree->RootNode)
		{
			TArray<TSharedPtr<FJsonValue>> Nodes;
			TArray<TSharedPtr<FJsonValue>> Edges;
			int32 Counter = 1;
			TraverseBTComposite(Tree->RootNode, TEXT("root"), Nodes, Edges, Counter);
			Root->SetArrayField(TEXT("nodes"), Nodes);
			Root->SetArrayField(TEXT("edges"), Edges);
			Root->SetNumberField(TEXT("node_count"), Nodes.Num());
			Root->SetNumberField(TEXT("edge_count"), Edges.Num());
		}
		return Root;
	}

	static TSharedPtr<FJsonObject> MakeBTAssetJson(UBehaviorTree* Tree)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("profile"), BehaviorTreeProfile);
		Obj->SetNumberField(TEXT("schema_version"), 1);
		Obj->SetStringField(TEXT("asset_path"), Tree ? Tree->GetOutermost()->GetName() : FString());
		Obj->SetStringField(TEXT("object_path"), Tree ? Tree->GetPathName() : FString());
		Obj->SetStringField(TEXT("blackboard_asset"), (Tree && Tree->BlackboardAsset) ? Tree->BlackboardAsset->GetPathName() : FString());
		return Obj;
	}

	static void ValidateBTBlackboardKeyReferences(const TSharedPtr<FJsonObject>& AssetJson, const TSharedPtr<FJsonObject>& NodesJson, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		FString BlackboardPath;
		if (!AssetJson.IsValid() || !AssetJson->TryGetStringField(TEXT("blackboard_asset"), BlackboardPath) || BlackboardPath.TrimStartAndEnd().IsEmpty())
		{
			AddIssue(Issues, TEXT("blackboard_asset_missing"), TEXT("Missing blackboard_asset for blackboard key validation."), TEXT("asset.json.blackboard_asset"));
			return;
		}

		UBlackboardData* BlackboardData = LoadAsset<UBlackboardData>(BlackboardPath);
		if (!BlackboardData)
		{
			AddIssue(Issues, TEXT("blackboard_not_found"), FString::Printf(TEXT("Blackboard asset '%s' could not be loaded."), *BlackboardPath), TEXT("asset.json.blackboard_asset"));
			return;
		}

		auto IsKnownKey = [BlackboardData](const FString& InKeyName) -> bool
		{
			const FString KeyName = InKeyName.TrimStartAndEnd();
			if (KeyName.IsEmpty() || KeyName.Equals(TEXT("None"), ESearchCase::IgnoreCase))
			{
				return true;
			}
			return BlackboardData->GetKeyID(FName(*KeyName)) != FBlackboard::InvalidKey;
		};

		auto ValidateKeyText = [&Issues, &IsKnownKey](const FString& KeyText, const FString& JsonPath)
		{
			if (!IsKnownKey(KeyText))
			{
				AddIssue(Issues, TEXT("blackboard_key_not_found"), FString::Printf(TEXT("Blackboard key '%s' is not defined."), *KeyText), JsonPath);
			}
		};

		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		if (!NodesJson.IsValid() || !NodesJson->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes)
		{
			return;
		}
		for (int32 NodeIndex = 0; NodeIndex < Nodes->Num(); ++NodeIndex)
		{
			const TSharedPtr<FJsonObject>* NodeObjPtr = nullptr;
			if (!(*Nodes)[NodeIndex].IsValid() || !(*Nodes)[NodeIndex]->TryGetObject(NodeObjPtr) || !NodeObjPtr || !NodeObjPtr->IsValid())
			{
				continue;
			}
			const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
			if (!(*NodeObjPtr)->TryGetArrayField(TEXT("properties"), Properties) || !Properties)
			{
				continue;
			}
			for (int32 PropIndex = 0; PropIndex < Properties->Num(); ++PropIndex)
			{
				const TSharedPtr<FJsonObject>* PropObjPtr = nullptr;
				if (!(*Properties)[PropIndex].IsValid() || !(*Properties)[PropIndex]->TryGetObject(PropObjPtr) || !PropObjPtr || !PropObjPtr->IsValid())
				{
					continue;
				}
				const FString PropPath = FString::Printf(TEXT("nodes.json.nodes[%d].properties[%d]"), NodeIndex, PropIndex);
				FString PropertyName;
				if (!(*PropObjPtr)->TryGetStringField(TEXT("property_name"), PropertyName))
				{
					(*PropObjPtr)->TryGetStringField(TEXT("name"), PropertyName);
				}
				const bool bLooksLikeBlackboardKey =
					PropertyName.Contains(TEXT("BlackboardKey"), ESearchCase::IgnoreCase) ||
					PropertyName.Contains(TEXT("SelectedKeyName"), ESearchCase::IgnoreCase);

				FString KeyText;
				if ((*PropObjPtr)->TryGetStringField(TEXT("selected_key_name"), KeyText) || (*PropObjPtr)->TryGetStringField(TEXT("key_name"), KeyText))
				{
					ValidateKeyText(KeyText, PropPath);
					continue;
				}

				const TSharedPtr<FJsonObject>* ValueObject = nullptr;
				if ((*PropObjPtr)->TryGetObjectField(TEXT("value"), ValueObject) && ValueObject && ValueObject->IsValid())
				{
					if ((*ValueObject)->TryGetStringField(TEXT("selected_key_name"), KeyText) || (*ValueObject)->TryGetStringField(TEXT("key_name"), KeyText))
					{
						ValidateKeyText(KeyText, PropPath + TEXT(".value"));
					}
					continue;
				}

				if (bLooksLikeBlackboardKey && ((*PropObjPtr)->TryGetStringField(TEXT("value_text"), KeyText) || (*PropObjPtr)->TryGetStringField(TEXT("value"), KeyText)))
				{
					ValidateKeyText(KeyText, PropPath);
				}
			}
		}
	}

	static bool ValidateBTFolderJson(const TSharedPtr<FJsonObject>& AssetJson, const TSharedPtr<FJsonObject>& NodesJson, TArray<TSharedPtr<FJsonValue>>& Issues, const bool bStrict = false, const bool bValidateBlackboardKeys = false)
	{
		if (!AssetJson.IsValid())
		{
			AddIssue(Issues, TEXT("missing_required_file"), TEXT("Missing asset.json."), TEXT("asset.json"));
		}
		else if (bStrict)
		{
			StrictCheckFields(AssetJson, { TEXT("profile"), TEXT("schema_version"), TEXT("asset_path"), TEXT("object_path"), TEXT("blackboard_asset"), TEXT("root_id"), TEXT("dirty"), TEXT("valid") }, TEXT("asset.json"), Issues);
		}
		if (NodesJson.IsValid() && bStrict)
		{
			StrictCheckFields(NodesJson, { TEXT("root_id"), TEXT("nodes"), TEXT("edges"), TEXT("node_count"), TEXT("edge_count") }, TEXT("nodes.json"), Issues);
		}
		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		if (!NodesJson.IsValid() || !NodesJson->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes)
		{
			AddIssue(Issues, TEXT("missing_required_field"), TEXT("Missing nodes array."), TEXT("nodes.json.nodes"));
			return false;
		}

		TSet<FString> Ids;
		for (int32 Index = 0; Index < Nodes->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>* NodeObjPtr = nullptr;
			if (!(*Nodes)[Index].IsValid() || !(*Nodes)[Index]->TryGetObject(NodeObjPtr) || !NodeObjPtr || !NodeObjPtr->IsValid())
			{
				AddIssue(Issues, TEXT("invalid_node_item"), TEXT("Node item must be an object."), FString::Printf(TEXT("nodes.json.nodes[%d]"), Index));
				continue;
			}
			if (bStrict)
			{
				StrictCheckFields(*NodeObjPtr, { TEXT("id"), TEXT("kind"), TEXT("class"), TEXT("name"), TEXT("static_description"), TEXT("properties") }, FString::Printf(TEXT("nodes.json.nodes[%d]"), Index), Issues);
			}
			FString Id;
			FString Kind;
			FString ClassPath;
			(*NodeObjPtr)->TryGetStringField(TEXT("id"), Id);
			(*NodeObjPtr)->TryGetStringField(TEXT("kind"), Kind);
			(*NodeObjPtr)->TryGetStringField(TEXT("class"), ClassPath);
			if (Id.IsEmpty())
			{
				AddIssue(Issues, TEXT("missing_required_field"), TEXT("Missing node id."), FString::Printf(TEXT("nodes.json.nodes[%d].id"), Index));
			}
			else if (Ids.Contains(Id))
			{
				AddIssue(Issues, TEXT("duplicate_id"), FString::Printf(TEXT("Duplicate node id '%s'."), *Id), FString::Printf(TEXT("nodes.json.nodes[%d].id"), Index));
			}
			Ids.Add(Id);
			const UClass* BaseClass = UBTNode::StaticClass();
			if (Kind == TEXT("composite")) BaseClass = UBTCompositeNode::StaticClass();
			else if (Kind == TEXT("task")) BaseClass = UBTTaskNode::StaticClass();
			else if (Kind == TEXT("decorator")) BaseClass = UBTDecorator::StaticClass();
			else if (Kind == TEXT("service")) BaseClass = UBTService::StaticClass();
			if (!ResolveBTClass(ClassPath.IsEmpty() ? Kind : ClassPath, BaseClass))
			{
				AddIssue(Issues, TEXT("bt_node_class_not_found"), FString::Printf(TEXT("Invalid BT node class '%s'."), *ClassPath), FString::Printf(TEXT("nodes.json.nodes[%d].class"), Index));
			}
		}
		if (bValidateBlackboardKeys)
		{
			ValidateBTBlackboardKeyReferences(AssetJson, NodesJson, Issues);
		}
		return Issues.Num() == 0;
	}

	static UBTNode* CreateBTNodeFromJson(UBehaviorTree* Tree, const TSharedPtr<FJsonObject>& NodeObj, TArray<TSharedPtr<FJsonValue>>& PropertyResults, FString& OutError)
	{
		FString Kind;
		FString ClassPath;
		FString Name;
		NodeObj->TryGetStringField(TEXT("kind"), Kind);
		NodeObj->TryGetStringField(TEXT("class"), ClassPath);
		NodeObj->TryGetStringField(TEXT("name"), Name);
		const UClass* BaseClass = UBTNode::StaticClass();
		if (Kind == TEXT("composite")) BaseClass = UBTCompositeNode::StaticClass();
		else if (Kind == TEXT("task")) BaseClass = UBTTaskNode::StaticClass();
		else if (Kind == TEXT("decorator")) BaseClass = UBTDecorator::StaticClass();
		else if (Kind == TEXT("service")) BaseClass = UBTService::StaticClass();
		UClass* NodeClass = ResolveBTClass(ClassPath.IsEmpty() ? Kind : ClassPath, BaseClass);
		if (!NodeClass)
		{
			OutError = TEXT("bt_node_class_not_found");
			return nullptr;
		}
		UBTNode* Node = NewObject<UBTNode>(Tree, NodeClass, NAME_None, RF_Transactional);
		if (!Node)
		{
			OutError = TEXT("bt_node_create_failed");
			return nullptr;
		}
		Node->NodeName = Name;
		const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
		if (NodeObj->TryGetArrayField(TEXT("properties"), Properties) && Properties)
		{
			if (!ApplyProperties(Node, Properties, PropertyResults, OutError))
			{
				return nullptr;
			}
		}
		return Node;
	}
}

bool FUeAgentHttpServer::CmdBehaviorTreeCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UBehaviorTree* Tree = nullptr;
	if (!UeAgentAIOps::CreateAsset<UBehaviorTree>(AssetPath, Tree, OutError) || !Tree)
	{
		return false;
	}

	FString BlackboardPath;
	if (JsonTryGetString(Ctx.Params, TEXT("blackboard_asset"), BlackboardPath) && !BlackboardPath.IsEmpty())
	{
		Tree->BlackboardAsset = UeAgentAIOps::LoadAsset<UBlackboardData>(BlackboardPath);
		if (!Tree->BlackboardAsset)
		{
			OutError = TEXT("blackboard_not_found");
			return false;
		}
	}
	FString RootComposite;
	if (!JsonTryGetString(Ctx.Params, TEXT("root_composite"), RootComposite) || RootComposite.IsEmpty())
	{
		RootComposite = TEXT("sequence");
	}
	UClass* RootClass = UeAgentAIOps::ResolveBTClass(RootComposite, UBTCompositeNode::StaticClass());
	if (!RootClass)
	{
		OutError = TEXT("bt_node_class_not_found");
		return false;
	}
	Tree->RootNode = NewObject<UBTCompositeNode>(Tree, RootClass, NAME_None, RF_Transactional);
	Tree->RootNode->NodeName = TEXT("Root");
	Tree->MarkPackageDirty();
	Tree->PostEditChange();

	bool bSave = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_create"), bSave);
	if (bSave && !UeAgentAIOps::SaveAssetPackage(Tree, OutError))
	{
		return false;
	}
	OutData = UeAgentAIOps::ExportBehaviorTree(Tree, true);
	OutData->SetBoolField(TEXT("created_asset"), true);
	OutData->SetBoolField(TEXT("saved"), bSave);
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	return true;
}

bool FUeAgentHttpServer::CmdBehaviorTreeGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UBehaviorTree* Tree = UeAgentAIOps::LoadAsset<UBehaviorTree>(AssetPath);
	if (!Tree)
	{
		OutError = TEXT("behavior_tree_not_found");
		return false;
	}
	bool bIncludeNodes = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_nodes"), bIncludeNodes);
	OutData = UeAgentAIOps::ExportBehaviorTree(Tree, bIncludeNodes);
	return true;
}

bool FUeAgentHttpServer::CmdBehaviorTreeExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UBehaviorTree* Tree = UeAgentAIOps::LoadAsset<UBehaviorTree>(AssetPath);
	if (!Tree)
	{
		OutError = TEXT("behavior_tree_not_found");
		return false;
	}
	FString FolderInput;
	JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderInput);
	const FString FolderPath = UeAgentAIOps::ResolveFolderPath(FolderInput, TEXT("BehaviorTree"), AssetPath);
	bool bClean = false;
	JsonTryGetBool(Ctx.Params, TEXT("clean_output_dir"), bClean);
	if (bClean)
	{
		IFileManager::Get().DeleteDirectory(*FolderPath, false, true);
	}
	TArray<TSharedPtr<FJsonValue>> WrittenFiles;
	if (!UeAgentAIOps::SaveBehaviorTreeFolderContents(FolderPath, Tree, WrittenFiles, OutError))
	{
		return false;
	}
	OutData = UeAgentAIOps::ExportBehaviorTree(Tree, true);
	OutData->SetArrayField(TEXT("written_files"), WrittenFiles);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	UeAgentAIOps::AttachCoverageReport(OutData);
	return true;
}

bool FUeAgentHttpServer::CmdBehaviorTreeValidateFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath);
	FString FolderInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderInput) || FolderInput.IsEmpty())
	{
		OutError = TEXT("missing_folder_path");
		return false;
	}
	const FString FolderPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FolderInput);
	TSharedPtr<FJsonObject> AssetJson;
	TSharedPtr<FJsonObject> NodesJson;
	if (!UeAgentAIOps::LoadFolderJson(FolderPath, TEXT("asset.json"), AssetJson, OutError)) return false;
	if (!UeAgentAIOps::LoadFolderJson(FolderPath, TEXT("nodes.json"), NodesJson, OutError)) return false;
	TArray<TSharedPtr<FJsonValue>> Issues;
	TSharedPtr<FJsonObject> EffectiveAssetJson = AssetJson;
	if (!AssetPath.TrimStartAndEnd().IsEmpty())
	{
		const FString NormalizedAssetPath = UeAgentAIOps::NormalizeAssetPath(AssetPath);
		if (!FPackageName::IsValidLongPackageName(NormalizedAssetPath))
		{
			UeAgentAIOps::AddIssue(Issues, TEXT("invalid_asset_path"), FString::Printf(TEXT("Invalid asset_path '%s'."), *AssetPath), TEXT("$.asset_path"));
		}
		EffectiveAssetJson = MakeShared<FJsonObject>();
		if (AssetJson.IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : AssetJson->Values)
			{
				EffectiveAssetJson->SetField(Pair.Key, Pair.Value);
			}
		}
		EffectiveAssetJson->SetStringField(TEXT("asset_path"), NormalizedAssetPath);
		AssetPath = NormalizedAssetPath;
	}
	else if (AssetJson.IsValid())
	{
		AssetJson->TryGetStringField(TEXT("asset_path"), AssetPath);
	}
	bool bStrict = false;
	bool bValidateBlackboardKeys = false;
	JsonTryGetBool(Ctx.Params, TEXT("strict"), bStrict);
	JsonTryGetBool(Ctx.Params, TEXT("validate_blackboard_keys"), bValidateBlackboardKeys);
	const bool bValid = UeAgentAIOps::ValidateBTFolderJson(EffectiveAssetJson, NodesJson, Issues, bStrict, bValidateBlackboardKeys);
	OutData->SetStringField(TEXT("profile"), UeAgentAIOps::BehaviorTreeProfile);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetStringField(TEXT("asset_path"), AssetPath);
	OutData->SetBoolField(TEXT("strict"), bStrict);
	OutData->SetBoolField(TEXT("validate_blackboard_keys"), bValidateBlackboardKeys);
	OutData->SetBoolField(TEXT("valid"), bValid);
	OutData->SetStringField(TEXT("status"), bValid ? TEXT("ok") : TEXT("validate_failed"));
	UeAgentAIOps::SetDiagnostics(OutData, Issues);
	return bValid;
}

bool FUeAgentHttpServer::CmdBehaviorTreeApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString FolderInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderInput) || FolderInput.IsEmpty())
	{
		OutError = TEXT("missing_folder_path");
		return false;
	}
	const FString FolderPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FolderInput);
	TSharedPtr<FJsonObject> AssetJson;
	TSharedPtr<FJsonObject> BlackboardJson;
	TSharedPtr<FJsonObject> TreeJson;
	TSharedPtr<FJsonObject> NodesJson;
	if (!UeAgentAIOps::LoadFolderJson(FolderPath, TEXT("asset.json"), AssetJson, OutError)) return false;
	if (!UeAgentAIOps::LoadFolderJson(FolderPath, TEXT("blackboard.json"), BlackboardJson, OutError, false)) return false;
	if (!UeAgentAIOps::LoadFolderJson(FolderPath, TEXT("tree.json"), TreeJson, OutError)) return false;
	if (!UeAgentAIOps::LoadFolderJson(FolderPath, TEXT("nodes.json"), NodesJson, OutError)) return false;
	TArray<TSharedPtr<FJsonValue>> Issues;
	if (!UeAgentAIOps::ValidateBTFolderJson(AssetJson, NodesJson, Issues))
	{
		OutData->SetStringField(TEXT("status"), TEXT("validate_failed"));
		UeAgentAIOps::SetDiagnostics(OutData, Issues);
		return false;
	}

	FString AssetPath;
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		AssetJson->TryGetStringField(TEXT("asset_path"), AssetPath);
	}
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	bool bCreateIfMissing = false;
	bool bDryRun = false;
	bool bValidateOnly = false;
	bool bAllowDestructive = false;
	JsonTryGetBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing);
	JsonTryGetBool(Ctx.Params, TEXT("dry_run"), bDryRun);
	JsonTryGetBool(Ctx.Params, TEXT("validate_only"), bValidateOnly);
	JsonTryGetBool(Ctx.Params, TEXT("allow_destructive"), bAllowDestructive);
	if (bDryRun || bValidateOnly)
	{
		OutData->SetStringField(TEXT("profile"), UeAgentAIOps::BehaviorTreeProfile);
		OutData->SetStringField(TEXT("asset_path"), AssetPath);
		OutData->SetStringField(TEXT("folder_path"), FolderPath);
		OutData->SetStringField(TEXT("status"), TEXT("ok"));
		OutData->SetBoolField(TEXT("dry_run"), bDryRun);
		OutData->SetBoolField(TEXT("validate_only"), bValidateOnly);
		OutData->SetBoolField(TEXT("changed"), false);
		UeAgentAIOps::SetDiagnostics(OutData, Issues);
		return true;
	}

	UBehaviorTree* Tree = UeAgentAIOps::LoadAsset<UBehaviorTree>(AssetPath);
	bool bCreated = false;
	if (!Tree)
	{
		if (!bCreateIfMissing)
		{
			OutError = TEXT("behavior_tree_not_found");
			return false;
		}
		if (!UeAgentAIOps::CreateAsset<UBehaviorTree>(AssetPath, Tree, OutError) || !Tree)
		{
			return false;
		}
		bCreated = true;
	}
	if (Tree->RootNode && !bCreated && !bAllowDestructive)
	{
		OutError = TEXT("bt_destructive_change_requires_opt_in");
		return false;
	}

	Tree->Modify();
	FString BlackboardPath;
	if ((BlackboardJson.IsValid() && BlackboardJson->TryGetStringField(TEXT("blackboard_asset"), BlackboardPath)) || AssetJson->TryGetStringField(TEXT("blackboard_asset"), BlackboardPath))
	{
		if (!BlackboardPath.IsEmpty())
		{
			Tree->BlackboardAsset = UeAgentAIOps::LoadAsset<UBlackboardData>(BlackboardPath);
			if (!Tree->BlackboardAsset)
			{
				OutError = TEXT("blackboard_not_found");
				return false;
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
	NodesJson->TryGetArrayField(TEXT("nodes"), Nodes);
	TMap<FString, UBTNode*> NodeMap;
	TArray<TSharedPtr<FJsonValue>> PropertyResults;
	for (const TSharedPtr<FJsonValue>& Value : *Nodes)
	{
		const TSharedPtr<FJsonObject>* NodeObjPtr = nullptr;
		Value->TryGetObject(NodeObjPtr);
		FString Id;
		(*NodeObjPtr)->TryGetStringField(TEXT("id"), Id);
		if (UBTNode* Node = UeAgentAIOps::CreateBTNodeFromJson(Tree, *NodeObjPtr, PropertyResults, OutError))
		{
			NodeMap.Add(Id, Node);
		}
		else
		{
			return false;
		}
	}

	FString RootId;
	TreeJson->TryGetStringField(TEXT("root_id"), RootId);
	if (RootId.IsEmpty())
	{
		RootId = TEXT("root");
	}
	Tree->RootNode = Cast<UBTCompositeNode>(NodeMap.FindRef(RootId));
	if (!Tree->RootNode)
	{
		OutError = TEXT("bt_root_composite_not_found");
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Edges = nullptr;
	TreeJson->TryGetArrayField(TEXT("edges"), Edges);
	if (Edges)
	{
		for (const TSharedPtr<FJsonValue>& EdgeValue : *Edges)
		{
			const TSharedPtr<FJsonObject>* EdgeObjPtr = nullptr;
			if (!EdgeValue.IsValid() || !EdgeValue->TryGetObject(EdgeObjPtr) || !EdgeObjPtr || !EdgeObjPtr->IsValid())
			{
				continue;
			}
			FString ParentId;
			FString ChildId;
			FString Relation;
			(*EdgeObjPtr)->TryGetStringField(TEXT("parent_id"), ParentId);
			(*EdgeObjPtr)->TryGetStringField(TEXT("child_id"), ChildId);
			(*EdgeObjPtr)->TryGetStringField(TEXT("relation"), Relation);
			UBTNode* ParentNode = NodeMap.FindRef(ParentId);
			UBTNode* ChildNode = NodeMap.FindRef(ChildId);
			if (Relation == TEXT("service"))
			{
				if (UBTService* Service = Cast<UBTService>(ChildNode))
				{
					if (UBTCompositeNode* Composite = Cast<UBTCompositeNode>(ParentNode))
					{
						Composite->Services.Add(Service);
					}
					else if (UBTTaskNode* Task = Cast<UBTTaskNode>(ParentNode))
					{
						Task->Services.Add(Service);
					}
				}
				continue;
			}
			UBTCompositeNode* ParentComposite = Cast<UBTCompositeNode>(ParentNode);
			if (!ParentComposite || !ChildNode)
			{
				continue;
			}
			FBTCompositeChild& Child = ParentComposite->Children.AddDefaulted_GetRef();
			Child.ChildComposite = Cast<UBTCompositeNode>(ChildNode);
			Child.ChildTask = Cast<UBTTaskNode>(ChildNode);
			const TArray<TSharedPtr<FJsonValue>>* DecoratorIds = nullptr;
			if ((*EdgeObjPtr)->TryGetArrayField(TEXT("decorator_ids"), DecoratorIds) && DecoratorIds)
			{
				for (const TSharedPtr<FJsonValue>& DecoratorValue : *DecoratorIds)
				{
					FString DecoratorId;
					if (DecoratorValue.IsValid() && DecoratorValue->TryGetString(DecoratorId))
					{
						if (UBTDecorator* Decorator = Cast<UBTDecorator>(NodeMap.FindRef(DecoratorId)))
						{
							Child.Decorators.Add(Decorator);
							Child.DecoratorOps.Add(FBTDecoratorLogic(EBTDecoratorLogic::Test, 0));
						}
					}
				}
			}
		}
	}

	Tree->MarkPackageDirty();
	Tree->PostEditChange();
	bool bSave = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSave);
	if (bSave && !UeAgentAIOps::SaveAssetPackage(Tree, OutError))
	{
		return false;
	}

	OutData = UeAgentAIOps::ExportBehaviorTree(Tree, true);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetBoolField(TEXT("created_asset"), bCreated);
	OutData->SetBoolField(TEXT("changed"), true);
	OutData->SetBoolField(TEXT("saved"), bSave);
	OutData->SetArrayField(TEXT("property_results"), PropertyResults);
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	UeAgentAIOps::AttachCoverageReport(OutData);
	return true;
}

bool FUeAgentHttpServer::CmdBehaviorTreeOpenEditor(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UObject* Asset = UeAgentAIOps::LoadAssetObject(AssetPath);
	if (!Asset)
	{
		OutError = TEXT("behavior_tree_not_found");
		return false;
	}
	if (GEditor)
	{
		GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Asset);
	}
	OutData->SetStringField(TEXT("asset_path"), Asset->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
	OutData->SetBoolField(TEXT("opened"), true);
	return true;
}

bool FUeAgentHttpServer::CmdBehaviorTreeGraphGetView(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return CmdBehaviorTreeGetInfo(Ctx, OutData, OutError);
}

bool FUeAgentHttpServer::CmdBehaviorTreeGraphSetView(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!CmdBehaviorTreeGetInfo(Ctx, OutData, OutError))
	{
		return false;
	}
	double X = 0.0;
	double Y = 0.0;
	double Zoom = 1.0;
	JsonTryGetNumber(Ctx.Params, TEXT("view_x"), X);
	JsonTryGetNumber(Ctx.Params, TEXT("view_y"), Y);
	JsonTryGetNumber(Ctx.Params, TEXT("zoom"), Zoom);
	OutData->SetNumberField(TEXT("view_x"), X);
	OutData->SetNumberField(TEXT("view_y"), Y);
	OutData->SetNumberField(TEXT("zoom"), Zoom);
	OutData->SetBoolField(TEXT("view_requested"), true);
	return true;
}

bool FUeAgentHttpServer::CmdBehaviorTreeScreenshot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UObject* Asset = UeAgentAIOps::LoadAssetObject(AssetPath);
	if (!Asset)
	{
		OutError = TEXT("behavior_tree_not_found");
		return false;
	}
	bool bOpenIfNeeded = true;
	JsonTryGetBool(Ctx.Params, TEXT("open_editor_if_needed"), bOpenIfNeeded);
	TSharedPtr<SWindow> Window;
	if (!UeAgentAIOps::ResolveGenericAssetEditorWindow(Asset, bOpenIfNeeded, Window, OutError))
	{
		return false;
	}
	FString BackbufferUnsafeReason;
	const bool bBackbufferCaptureUnsafe = UeAgentAIOps::IsSlateWindowBackbufferCaptureUnsafe(Window, BackbufferUnsafeReason);
	TArray<FColor> Pixels;
	FIntPoint Size;
	if (!UeAgentAIOps::ScreenshotSlateWindow(Window, Pixels, Size, OutError))
	{
		return false;
	}
	int32 MaxSize = 1600;
	double MaxSizeNumber = MaxSize;
	JsonTryGetNumber(Ctx.Params, TEXT("max_size"), MaxSizeNumber);
	MaxSize = (int32)FMath::Clamp(MaxSizeNumber, 64.0, 8192.0);
	TArray<FColor> ResizedPixels;
	FIntPoint ResizedSize;
	if (!ResizePixelsMaxSize(Pixels, Size, MaxSize, ResizedPixels, ResizedSize, OutError))
	{
		return false;
	}
	bool bRequireNonBlack = true;
	JsonTryGetBool(Ctx.Params, TEXT("require_non_black"), bRequireNonBlack);
	if (bRequireNonBlack && UeAgentAIOps::CountNonBlackPixels(ResizedPixels) <= 0)
	{
		OutError = TEXT("asset_editor_screenshot_black");
		return false;
	}
	FString Format = TEXT("png");
	JsonTryGetString(Ctx.Params, TEXT("format"), Format);
	Format = Format.ToLower();
	const FString OutPath = MakeShotPath(Format);
	if (!WriteCompressedImage(ResizedPixels, ResizedSize, Format, 90, OutPath, OutError))
	{
		return false;
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("asset_path"), Asset->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("file_path"), OutPath);
	OutData->SetNumberField(TEXT("width"), ResizedSize.X);
	OutData->SetNumberField(TEXT("height"), ResizedSize.Y);
	OutData->SetStringField(TEXT("format"), Format);
	OutData->SetStringField(TEXT("capture_mode"), TEXT("offscreen_widget_renderer"));
	OutData->SetStringField(TEXT("legacy_backbuffer_capture"), TEXT("disabled"));
	OutData->SetBoolField(TEXT("backbuffer_capture_would_be_unsafe"), bBackbufferCaptureUnsafe);
	if (!BackbufferUnsafeReason.IsEmpty())
	{
		OutData->SetStringField(TEXT("backbuffer_unsafe_reason"), BackbufferUnsafeReason);
	}
	UeAgentAIOps::AddPixelStats(ResizedPixels, OutData);
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	return true;
}

bool FUeAgentHttpServer::CmdBehaviorTreeRuntimeSnapshot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	FString ControllerOrPawn;
	JsonTryGetString(Ctx.Params, TEXT("controller"), ControllerOrPawn);
	if (ControllerOrPawn.IsEmpty())
	{
		JsonTryGetString(Ctx.Params, TEXT("pawn"), ControllerOrPawn);
	}
	AActor* Actor = ControllerOrPawn.IsEmpty() ? nullptr : FindActorByNameOrLabel(World, ControllerOrPawn);
	UBehaviorTreeComponent* BTComp = nullptr;
	if (Actor)
	{
		BTComp = Actor->FindComponentByClass<UBehaviorTreeComponent>();
		if (!BTComp)
		{
			if (APawn* Pawn = Cast<APawn>(Actor))
			{
				if (AAIController* Controller = Cast<AAIController>(Pawn->GetController()))
				{
					BTComp = Controller->FindComponentByClass<UBehaviorTreeComponent>();
				}
			}
		}
	}
	else
	{
		for (TActorIterator<AAIController> It(World); It; ++It)
		{
			BTComp = It->FindComponentByClass<UBehaviorTreeComponent>();
			if (BTComp)
			{
				Actor = *It;
				break;
			}
		}
	}
	if (!BTComp)
	{
		OutError = TEXT("behavior_tree_component_not_found");
		return false;
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("owner"), Actor ? Actor->GetName() : FString());
	OutData->SetBoolField(TEXT("is_running"), BTComp->IsRunning());
	OutData->SetStringField(TEXT("current_tree"), BTComp->GetRootTree() ? BTComp->GetRootTree()->GetPathName() : FString());
	OutData->SetArrayField(TEXT("active_path"), TArray<TSharedPtr<FJsonValue>>());
	OutData->SetStringField(TEXT("running_task"), FString());
	OutData->SetArrayField(TEXT("service_status"), TArray<TSharedPtr<FJsonValue>>());
	OutData->SetStringField(TEXT("runtime_introspection"), TEXT("component_status_blackboard_values"));
	if (UBlackboardComponent* BlackboardComp = BTComp->GetBlackboardComponent())
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		if (UBlackboardData* BBAsset = BlackboardComp->GetBlackboardAsset())
		{
			for (int32 KeyIndex = 0; KeyIndex < BBAsset->GetNumKeys(); ++KeyIndex)
			{
				const FName KeyName = BBAsset->GetKeyName(static_cast<FBlackboard::FKey>(KeyIndex));
				TSharedPtr<FJsonObject> ValueObj = MakeShared<FJsonObject>();
				ValueObj->SetStringField(TEXT("name"), KeyName.ToString());
				ValueObj->SetStringField(TEXT("value"), BlackboardComp->DescribeKeyValue(KeyName, EBlackboardDescription::OnlyValue));
				Values.Add(MakeShared<FJsonValueObject>(ValueObj));
			}
		}
		OutData->SetArrayField(TEXT("blackboard"), Values);
	}
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	return true;
}

bool FUeAgentHttpServer::CmdBTNodeBlueprintGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UObject* Asset = UeAgentAIOps::LoadAssetObject(AssetPath);
	if (!Asset)
	{
		OutError = TEXT("blueprint_not_found");
		return false;
	}
	OutData->SetStringField(TEXT("asset_path"), Asset->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
	OutData->SetStringField(TEXT("class"), Asset->GetClass()->GetPathName());
	UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
	UClass* GeneratedClass = nullptr;
	if (Blueprint)
	{
		GeneratedClass = Blueprint->GeneratedClass;
	}
	else
	{
		GeneratedClass = Cast<UClass>(Asset);
	}
	OutData->SetStringField(TEXT("generated_class"), GeneratedClass ? GeneratedClass->GetPathName() : FString());
	OutData->SetStringField(TEXT("parent_class"), (GeneratedClass && GeneratedClass->GetSuperClass()) ? GeneratedClass->GetSuperClass()->GetPathName() : FString());
	OutData->SetStringField(TEXT("introspection_level"), TEXT("class_hierarchy_only"));
	OutData->SetBoolField(TEXT("is_bt_task"), GeneratedClass && GeneratedClass->IsChildOf(UBTTask_BlueprintBase::StaticClass()));
	OutData->SetBoolField(TEXT("is_bt_decorator"), GeneratedClass && GeneratedClass->IsChildOf(UBTDecorator_BlueprintBase::StaticClass()));
	OutData->SetBoolField(TEXT("is_bt_service"), GeneratedClass && GeneratedClass->IsChildOf(UBTService_BlueprintBase::StaticClass()));
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	return true;
}

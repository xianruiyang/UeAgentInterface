namespace UeAgentAIOps
{
	static TSharedPtr<FJsonObject> ExportEditorNode(const FStateTreeEditorNode& Node)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("id"), Node.ID.IsValid() ? Node.ID.ToString(EGuidFormats::DigitsWithHyphens) : FString());
		Obj->SetStringField(TEXT("name"), Node.GetName().ToString());
		Obj->SetStringField(TEXT("node_struct"), Node.Node.GetScriptStruct() ? Node.Node.GetScriptStruct()->GetPathName() : FString());
		Obj->SetStringField(TEXT("instance_struct"), Node.Instance.GetScriptStruct() ? Node.Instance.GetScriptStruct()->GetPathName() : FString());
		Obj->SetStringField(TEXT("instance_object_class"), Node.InstanceObject ? Node.InstanceObject->GetClass()->GetPathName() : FString());
		Obj->SetStringField(TEXT("instance_object_path"), Node.InstanceObject ? Node.InstanceObject->GetPathName() : FString());
		Obj->SetNumberField(TEXT("expression_indent"), Node.ExpressionIndent);
		Obj->SetStringField(TEXT("expression_operand"), EnumToString(Node.ExpressionOperand));
		return Obj;
	}

	static TSharedPtr<FJsonObject> ExportTransition(const FStateTreeTransition& Transition)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("id"), Transition.ID.IsValid() ? Transition.ID.ToString(EGuidFormats::DigitsWithHyphens) : FString());
		Obj->SetStringField(TEXT("trigger"), EnumToString(Transition.Trigger));
		Obj->SetStringField(TEXT("priority"), EnumToString(Transition.Priority));
#if WITH_EDITORONLY_DATA
		Obj->SetStringField(TEXT("type"), EnumToString(Transition.State.LinkType));
		Obj->SetStringField(TEXT("target_state_name"), Transition.State.Name.ToString());
		Obj->SetStringField(TEXT("target_state_id"), Transition.State.ID.IsValid() ? Transition.State.ID.ToString(EGuidFormats::DigitsWithHyphens) : FString());
#endif
		Obj->SetStringField(TEXT("fallback"), EnumToString(Transition.State.Fallback));
		Obj->SetBoolField(TEXT("enabled"), Transition.bTransitionEnabled);
		Obj->SetBoolField(TEXT("delay_enabled"), Transition.bDelayTransition);
		Obj->SetNumberField(TEXT("delay_duration"), Transition.DelayDuration);
		Obj->SetNumberField(TEXT("delay_random_variance"), Transition.DelayRandomVariance);
		TArray<TSharedPtr<FJsonValue>> Conditions;
		for (const FStateTreeEditorNode& Condition : Transition.Conditions)
		{
			Conditions.Add(MakeShared<FJsonValueObject>(ExportEditorNode(Condition)));
		}
		Obj->SetArrayField(TEXT("conditions"), Conditions);
		return Obj;
	}

	static TSharedPtr<FJsonObject> ExportStateTreeState(const UStateTreeState* State, const FString& ParentId, TArray<TSharedPtr<FJsonValue>>& OutFlatStates)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!State)
		{
			return Obj;
		}
		const FString StateId = State->ID.IsValid() ? State->ID.ToString(EGuidFormats::DigitsWithHyphens) : State->GetName();
		Obj->SetStringField(TEXT("id"), StateId);
		Obj->SetStringField(TEXT("parent_id"), ParentId);
		Obj->SetStringField(TEXT("name"), State->Name.ToString());
		Obj->SetStringField(TEXT("description"), State->Description);
		Obj->SetStringField(TEXT("path"), State->GetPath());
		Obj->SetStringField(TEXT("type"), EnumToString(State->Type));
		Obj->SetStringField(TEXT("selection_behavior"), EnumToString(State->SelectionBehavior));
		Obj->SetStringField(TEXT("tasks_completion"), EnumToString(State->TasksCompletion));
		Obj->SetBoolField(TEXT("enabled"), State->bEnabled);
		Obj->SetStringField(TEXT("tag"), State->Tag.IsValid() ? State->Tag.ToString() : FString());

		TArray<TSharedPtr<FJsonValue>> Conditions;
		for (const FStateTreeEditorNode& Condition : State->EnterConditions)
		{
			Conditions.Add(MakeShared<FJsonValueObject>(ExportEditorNode(Condition)));
		}
		Obj->SetArrayField(TEXT("enter_conditions"), Conditions);

		TArray<TSharedPtr<FJsonValue>> Tasks;
		for (const FStateTreeEditorNode& Task : State->Tasks)
		{
			Tasks.Add(MakeShared<FJsonValueObject>(ExportEditorNode(Task)));
		}
		Obj->SetArrayField(TEXT("tasks"), Tasks);

		TArray<TSharedPtr<FJsonValue>> Transitions;
		for (const FStateTreeTransition& Transition : State->Transitions)
		{
			Transitions.Add(MakeShared<FJsonValueObject>(ExportTransition(Transition)));
		}
		Obj->SetArrayField(TEXT("transitions"), Transitions);

		TArray<TSharedPtr<FJsonValue>> ChildIds;
		for (const UStateTreeState* Child : State->Children)
		{
			if (Child)
			{
				ChildIds.Add(MakeShared<FJsonValueString>(Child->ID.IsValid() ? Child->ID.ToString(EGuidFormats::DigitsWithHyphens) : Child->GetName()));
			}
		}
		Obj->SetArrayField(TEXT("children"), ChildIds);
		OutFlatStates.Add(MakeShared<FJsonValueObject>(Obj));

		for (const UStateTreeState* Child : State->Children)
		{
			ExportStateTreeState(Child, StateId, OutFlatStates);
		}
		return Obj;
	}

	static UStateTreeEditorData* GetStateTreeEditorData(UStateTree* Tree)
	{
#if WITH_EDITORONLY_DATA
		return Tree ? Cast<UStateTreeEditorData>(Tree->EditorData) : nullptr;
#else
		return nullptr;
#endif
	}

	static bool EnsureStateTreeEditorData(UStateTree* Tree, UClass* SchemaClass, FString& OutError)
	{
		if (!Tree)
		{
			OutError = TEXT("state_tree_not_found");
			return false;
		}
#if WITH_EDITORONLY_DATA
		UStateTreeEditorData* EditorData = GetStateTreeEditorData(Tree);
		if (!EditorData)
		{
			EditorData = NewObject<UStateTreeEditorData>(Tree, UStateTreeEditorData::StaticClass(), FName(), RF_Transactional);
			Tree->EditorData = EditorData;
		}
		if (!SchemaClass)
		{
			SchemaClass = UStateTreeComponentSchema::StaticClass();
		}
		if (!SchemaClass->IsChildOf(UStateTreeSchema::StaticClass()))
		{
			OutError = TEXT("invalid_state_tree_schema_class");
			return false;
		}
		if (!EditorData->Schema || EditorData->Schema->GetClass() != SchemaClass)
		{
			EditorData->Schema = NewObject<UStateTreeSchema>(EditorData, SchemaClass, FName(), RF_Transactional);
		}
		if (EditorData->SubTrees.Num() == 0)
		{
			UStateTreeState& Root = EditorData->AddRootState();
			Root.ID = FGuid::NewGuid();
		}
		return true;
#else
		OutError = TEXT("state_tree_editor_data_unavailable");
		return false;
#endif
	}

	static bool CompileStateTree(UStateTree* Tree, TSharedPtr<FJsonObject>& OutCompile)
	{
		OutCompile = MakeShared<FJsonObject>();
#if WITH_EDITOR
		FStateTreeCompilerLog CompileLog;
		const bool bCompiled = Tree ? UStateTreeEditingSubsystem::CompileStateTree(Tree, CompileLog) : false;
		OutCompile->SetBoolField(TEXT("compiled"), bCompiled);
		OutCompile->SetBoolField(TEXT("ready_to_run"), Tree && Tree->IsReadyToRun());
		return bCompiled;
#else
		OutCompile->SetBoolField(TEXT("compiled"), false);
		OutCompile->SetStringField(TEXT("reason"), TEXT("editor_unavailable"));
		return false;
#endif
	}

	static TSharedPtr<FJsonObject> ExportStateTree(UStateTree* Tree, const bool bIncludeStates)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("profile"), StateTreeProfile);
		Root->SetNumberField(TEXT("schema_version"), 1);
		Root->SetStringField(TEXT("asset_path"), Tree ? Tree->GetOutermost()->GetName() : FString());
		Root->SetStringField(TEXT("object_path"), Tree ? Tree->GetPathName() : FString());
		Root->SetBoolField(TEXT("ready_to_run"), Tree && Tree->IsReadyToRun());
		Root->SetBoolField(TEXT("dirty"), Tree && Tree->GetOutermost()->IsDirty());
		UStateTreeEditorData* EditorData = GetStateTreeEditorData(Tree);
		Root->SetStringField(TEXT("schema_class"), (EditorData && EditorData->Schema) ? EditorData->Schema->GetClass()->GetPathName() : FString());
		if (!EditorData)
		{
			Root->SetNumberField(TEXT("state_count"), 0);
			return Root;
		}

		TArray<TSharedPtr<FJsonValue>> Evaluators;
		for (const FStateTreeEditorNode& Evaluator : EditorData->Evaluators)
		{
			Evaluators.Add(MakeShared<FJsonValueObject>(ExportEditorNode(Evaluator)));
		}
		Root->SetArrayField(TEXT("evaluators"), Evaluators);

		TArray<TSharedPtr<FJsonValue>> GlobalTasks;
		for (const FStateTreeEditorNode& Task : EditorData->GlobalTasks)
		{
			GlobalTasks.Add(MakeShared<FJsonValueObject>(ExportEditorNode(Task)));
		}
		Root->SetArrayField(TEXT("global_tasks"), GlobalTasks);

		if (bIncludeStates)
		{
			TArray<TSharedPtr<FJsonValue>> States;
			for (const UStateTreeState* SubTree : EditorData->SubTrees)
			{
				ExportStateTreeState(SubTree, FString(), States);
			}
			Root->SetArrayField(TEXT("states"), States);
			Root->SetNumberField(TEXT("state_count"), States.Num());
		}
		return Root;
	}

	static bool ValidateStateTreeFolderJson(const TSharedPtr<FJsonObject>& AssetJson, const TSharedPtr<FJsonObject>& StatesJson, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		if (!AssetJson.IsValid())
		{
			AddIssue(Issues, TEXT("missing_required_file"), TEXT("Missing asset.json."), TEXT("asset.json"));
		}
		const TArray<TSharedPtr<FJsonValue>>* States = nullptr;
		if (!StatesJson.IsValid() || !StatesJson->TryGetArrayField(TEXT("states"), States) || !States)
		{
			AddIssue(Issues, TEXT("missing_required_field"), TEXT("Missing states array."), TEXT("states.json.states"));
			return false;
		}
		TSet<FString> Ids;
		for (int32 Index = 0; Index < States->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>* StateObjPtr = nullptr;
			if (!(*States)[Index].IsValid() || !(*States)[Index]->TryGetObject(StateObjPtr) || !StateObjPtr || !StateObjPtr->IsValid())
			{
				AddIssue(Issues, TEXT("invalid_state_item"), TEXT("State item must be an object."), FString::Printf(TEXT("states.json.states[%d]"), Index));
				continue;
			}
			FString Id;
			FString Name;
			(*StateObjPtr)->TryGetStringField(TEXT("id"), Id);
			(*StateObjPtr)->TryGetStringField(TEXT("name"), Name);
			if (Id.IsEmpty())
			{
				AddIssue(Issues, TEXT("missing_required_field"), TEXT("Missing state id."), FString::Printf(TEXT("states.json.states[%d].id"), Index));
			}
			else if (Ids.Contains(Id))
			{
				AddIssue(Issues, TEXT("duplicate_id"), FString::Printf(TEXT("Duplicate state id '%s'."), *Id), FString::Printf(TEXT("states.json.states[%d].id"), Index));
			}
			Ids.Add(Id);
			if (Name.IsEmpty())
			{
				AddIssue(Issues, TEXT("missing_required_field"), TEXT("Missing state name."), FString::Printf(TEXT("states.json.states[%d].name"), Index));
			}
		}
		return Issues.Num() == 0;
	}

	static bool ConfigureStateTreeEditorNode(
		FStateTreeEditorNode& EditorNode,
		UObject* Outer,
		const TSharedPtr<FJsonObject>& NodeObj,
		UScriptStruct* RequiredBaseStruct,
		UClass* BlueprintBaseClass,
		TArray<TSharedPtr<FJsonValue>>& PropertyResults,
		FString& OutError)
	{
		if (!NodeObj.IsValid())
		{
			OutError = TEXT("invalid_state_tree_node");
			return false;
		}
		FString ClassOrStruct;
		NodeObj->TryGetStringField(TEXT("node_struct"), ClassOrStruct);
		if (ClassOrStruct.IsEmpty())
		{
			NodeObj->TryGetStringField(TEXT("class"), ClassOrStruct);
		}
		if (ClassOrStruct.IsEmpty())
		{
			OutError = TEXT("missing_state_tree_node_class");
			return false;
		}

		if (UScriptStruct* NodeStruct = ResolveScriptStruct(ClassOrStruct))
		{
			if (!NodeStruct->IsChildOf(RequiredBaseStruct))
			{
				OutError = TEXT("state_tree_node_struct_base_mismatch");
				return false;
			}
			EditorNode.Reset();
			EditorNode.ID = FGuid::NewGuid();
			EditorNode.Node.InitializeAs(NodeStruct);
			if (const FStateTreeNodeBase* NodeBase = reinterpret_cast<const FStateTreeNodeBase*>(EditorNode.Node.GetMemory()))
			{
				if (const UScriptStruct* InstanceType = Cast<const UScriptStruct>(NodeBase->GetInstanceDataType()))
				{
					EditorNode.Instance.InitializeAs(InstanceType);
				}
			}
			const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
			if (NodeObj->TryGetArrayField(TEXT("properties"), Properties) && Properties)
			{
				if (!ApplyStructProperties(NodeStruct, EditorNode.Node.GetMutableMemory(), Outer, Properties, PropertyResults, OutError))
				{
					return false;
				}
			}
			return true;
		}

		UClass* BlueprintClass = ResolveClass(ClassOrStruct);
		if (!BlueprintClass || !BlueprintClass->IsChildOf(BlueprintBaseClass))
		{
			OutError = TEXT("state_tree_node_class_not_found");
			return false;
		}
		EditorNode.Reset();
		EditorNode.ID = FGuid::NewGuid();
		if (BlueprintClass->IsChildOf(UStateTreeTaskBlueprintBase::StaticClass()))
		{
			EditorNode.Node.InitializeAs<FStateTreeBlueprintTaskWrapper>();
			EditorNode.Node.GetMutable<FStateTreeBlueprintTaskWrapper>().TaskClass = BlueprintClass;
			EditorNode.InstanceObject = NewObject<UStateTreeTaskBlueprintBase>(Outer, BlueprintClass, NAME_None, RF_Transactional);
		}
		else if (BlueprintClass->IsChildOf(UStateTreeConditionBlueprintBase::StaticClass()))
		{
			EditorNode.Node.InitializeAs<FStateTreeBlueprintConditionWrapper>();
			EditorNode.Node.GetMutable<FStateTreeBlueprintConditionWrapper>().ConditionClass = BlueprintClass;
			EditorNode.InstanceObject = NewObject<UStateTreeConditionBlueprintBase>(Outer, BlueprintClass, NAME_None, RF_Transactional);
		}
		else if (BlueprintClass->IsChildOf(UStateTreeEvaluatorBlueprintBase::StaticClass()))
		{
			EditorNode.Node.InitializeAs<FStateTreeBlueprintEvaluatorWrapper>();
			EditorNode.Node.GetMutable<FStateTreeBlueprintEvaluatorWrapper>().EvaluatorClass = BlueprintClass;
			EditorNode.InstanceObject = NewObject<UStateTreeEvaluatorBlueprintBase>(Outer, BlueprintClass, NAME_None, RF_Transactional);
		}
		const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
		if (EditorNode.InstanceObject && NodeObj->TryGetArrayField(TEXT("properties"), Properties) && Properties)
		{
			if (!ApplyProperties(EditorNode.InstanceObject, Properties, PropertyResults, OutError))
			{
				return false;
			}
		}
		return true;
	}

	static EStateTreeStateType ParseStateType(const FString& Text)
	{
		return EnumFromString(Text, EStateTreeStateType::State);
	}

	static TSharedPtr<FJsonObject> ExportEqs(UEnvQuery* Query, const bool bIncludeOptions)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("profile"), EqsProfile);
		Root->SetNumberField(TEXT("schema_version"), 1);
		Root->SetStringField(TEXT("asset_path"), Query ? Query->GetOutermost()->GetName() : FString());
		Root->SetStringField(TEXT("object_path"), Query ? Query->GetPathName() : FString());
		Root->SetBoolField(TEXT("dirty"), Query && Query->GetOutermost()->IsDirty());
		TArray<TSharedPtr<FJsonValue>> Options;
		if (Query && bIncludeOptions)
		{
			const TArray<UEnvQueryOption*>& QueryOptions = Query->GetOptions();
			for (int32 OptionIndex = 0; OptionIndex < QueryOptions.Num(); ++OptionIndex)
			{
				const UEnvQueryOption* Option = QueryOptions[OptionIndex];
				TSharedPtr<FJsonObject> OptionObj = MakeShared<FJsonObject>();
				OptionObj->SetStringField(TEXT("id"), FString::Printf(TEXT("option_%d"), OptionIndex));
				OptionObj->SetStringField(TEXT("generator_class"), (Option && Option->Generator) ? Option->Generator->GetClass()->GetPathName() : FString());
				OptionObj->SetStringField(TEXT("generator_name"), (Option && Option->Generator) ? Option->Generator->GetName() : FString());
				TArray<TSharedPtr<FJsonValue>> Tests;
				if (Option)
				{
					for (int32 TestIndex = 0; TestIndex < Option->Tests.Num(); ++TestIndex)
					{
						const UEnvQueryTest* Test = Option->Tests[TestIndex];
						TSharedPtr<FJsonObject> TestObj = MakeShared<FJsonObject>();
						TestObj->SetStringField(TEXT("id"), FString::Printf(TEXT("test_%d"), TestIndex));
						TestObj->SetStringField(TEXT("class"), Test ? Test->GetClass()->GetPathName() : FString());
						TestObj->SetStringField(TEXT("name"), Test ? Test->GetName() : FString());
						Tests.Add(MakeShared<FJsonValueObject>(TestObj));
					}
				}
				OptionObj->SetArrayField(TEXT("tests"), Tests);
				Options.Add(MakeShared<FJsonValueObject>(OptionObj));
			}
		}
		Root->SetArrayField(TEXT("options"), Options);
		Root->SetNumberField(TEXT("option_count"), Options.Num());
		return Root;
	}

	static bool ValidateEqsFolderJson(const TSharedPtr<FJsonObject>& AssetJson, const TSharedPtr<FJsonObject>& OptionsJson, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		if (!AssetJson.IsValid())
		{
			AddIssue(Issues, TEXT("missing_required_file"), TEXT("Missing asset.json."), TEXT("asset.json"));
		}
		const TArray<TSharedPtr<FJsonValue>>* Options = nullptr;
		if (!OptionsJson.IsValid() || !OptionsJson->TryGetArrayField(TEXT("options"), Options) || !Options)
		{
			AddIssue(Issues, TEXT("missing_required_field"), TEXT("Missing options array."), TEXT("options.json.options"));
			return false;
		}
		for (int32 Index = 0; Index < Options->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>* OptionObjPtr = nullptr;
			if (!(*Options)[Index].IsValid() || !(*Options)[Index]->TryGetObject(OptionObjPtr) || !OptionObjPtr || !OptionObjPtr->IsValid())
			{
				AddIssue(Issues, TEXT("invalid_option_item"), TEXT("Option item must be an object."), FString::Printf(TEXT("options.json.options[%d]"), Index));
				continue;
			}
			FString GeneratorClass;
			(*OptionObjPtr)->TryGetStringField(TEXT("generator_class"), GeneratorClass);
			if (!GeneratorClass.IsEmpty())
			{
				UClass* Class = ResolveClass(GeneratorClass);
				if (!Class || !Class->IsChildOf(UEnvQueryGenerator::StaticClass()))
				{
					AddIssue(Issues, TEXT("eqs_generator_class_not_found"), FString::Printf(TEXT("Invalid EQS generator '%s'."), *GeneratorClass), FString::Printf(TEXT("options.json.options[%d].generator_class"), Index));
				}
			}
			const TArray<TSharedPtr<FJsonValue>>* Tests = nullptr;
			if ((*OptionObjPtr)->TryGetArrayField(TEXT("tests"), Tests) && Tests)
			{
				for (int32 TestIndex = 0; TestIndex < Tests->Num(); ++TestIndex)
				{
					const TSharedPtr<FJsonObject>* TestObjPtr = nullptr;
					if (!(*Tests)[TestIndex].IsValid() || !(*Tests)[TestIndex]->TryGetObject(TestObjPtr) || !TestObjPtr || !TestObjPtr->IsValid())
					{
						AddIssue(Issues, TEXT("invalid_test_item"), TEXT("Test item must be an object."), FString::Printf(TEXT("options.json.options[%d].tests[%d]"), Index, TestIndex));
						continue;
					}
					FString TestClass;
					(*TestObjPtr)->TryGetStringField(TEXT("class"), TestClass);
					if (!TestClass.IsEmpty())
					{
						UClass* Class = ResolveClass(TestClass);
						if (!Class || !Class->IsChildOf(UEnvQueryTest::StaticClass()))
						{
							AddIssue(Issues, TEXT("eqs_test_class_not_found"), FString::Printf(TEXT("Invalid EQS test '%s'."), *TestClass), FString::Printf(TEXT("options.json.options[%d].tests[%d].class"), Index, TestIndex));
						}
					}
				}
			}
		}
		return Issues.Num() == 0;
	}

	static EEnvQueryRunMode::Type ParseEqsRunMode(const FString& Text)
	{
		const FString Lower = Text.ToLower();
		if (Lower == TEXT("single") || Lower == TEXT("single_result")) return EEnvQueryRunMode::SingleResult;
		if (Lower == TEXT("random_best_5_pct") || Lower == TEXT("randombest5pct")) return EEnvQueryRunMode::RandomBest5Pct;
		if (Lower == TEXT("random_best_25_pct") || Lower == TEXT("randombest25pct")) return EEnvQueryRunMode::RandomBest25Pct;
		return EEnvQueryRunMode::AllMatching;
	}

	static TSharedPtr<FJsonObject> ExportSmartObjectDefinition(USmartObjectDefinition* Definition, const bool bIncludeSlots)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("profile"), SmartObjectDefinitionProfile);
		Root->SetNumberField(TEXT("schema_version"), 1);
		Root->SetStringField(TEXT("asset_path"), Definition ? Definition->GetOutermost()->GetName() : FString());
		Root->SetStringField(TEXT("object_path"), Definition ? Definition->GetPathName() : FString());
		Root->SetBoolField(TEXT("dirty"), Definition && Definition->GetOutermost()->IsDirty());
		Root->SetArrayField(TEXT("activity_tags"), Definition ? GameplayTagsToJson(Definition->GetActivityTags()) : TArray<TSharedPtr<FJsonValue>>());
		Root->SetArrayField(TEXT("tags"), Definition ? GameplayTagsToJson(Definition->GetActivityTags()) : TArray<TSharedPtr<FJsonValue>>());
		auto ExportBehaviorDefinition = [](const USmartObjectBehaviorDefinition* Behavior, const FString& Id)
		{
			TSharedPtr<FJsonObject> BehaviorObj = MakeShared<FJsonObject>();
			BehaviorObj->SetStringField(TEXT("id"), Id);
			BehaviorObj->SetStringField(TEXT("class"), Behavior ? Behavior->GetClass()->GetPathName() : FString());
			BehaviorObj->SetStringField(TEXT("name"), Behavior ? Behavior->GetName() : FString());
			return BehaviorObj;
		};
		TArray<TSharedPtr<FJsonValue>> DefaultBehaviors;
		if (Definition)
		{
			if (FArrayProperty* DefaultsProperty = FindFProperty<FArrayProperty>(Definition->GetClass(), TEXT("DefaultBehaviorDefinitions")))
			{
				if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(DefaultsProperty->Inner))
				{
					void* DefaultsPtr = DefaultsProperty->ContainerPtrToValuePtr<void>(Definition);
					FScriptArrayHelper Helper(DefaultsProperty, DefaultsPtr);
					for (int32 Index = 0; Index < Helper.Num(); ++Index)
					{
						const USmartObjectBehaviorDefinition* Behavior = Cast<USmartObjectBehaviorDefinition>(ObjectProperty->GetObjectPropertyValue(Helper.GetRawPtr(Index)));
						DefaultBehaviors.Add(MakeShared<FJsonValueObject>(ExportBehaviorDefinition(Behavior, FString::Printf(TEXT("default_behavior_%d"), Index))));
					}
				}
			}
		}
		Root->SetArrayField(TEXT("default_behavior_definitions"), DefaultBehaviors);
		TArray<TSharedPtr<FJsonValue>> Slots;
		int32 SlotBehaviorCount = 0;
		if (Definition && bIncludeSlots)
		{
			const TConstArrayView<FSmartObjectSlotDefinition> DefinitionSlots = Definition->GetSlots();
			for (int32 Index = 0; Index < DefinitionSlots.Num(); ++Index)
			{
				const FSmartObjectSlotDefinition& Slot = DefinitionSlots[Index];
				TSharedPtr<FJsonObject> SlotObj = MakeShared<FJsonObject>();
				SlotObj->SetStringField(TEXT("id"), FString::Printf(TEXT("slot_%d"), Index));
#if WITH_EDITORONLY_DATA
				SlotObj->SetStringField(TEXT("name"), Slot.Name.ToString());
#endif
				SlotObj->SetObjectField(TEXT("offset"), VecToJson(FVector(Slot.Offset)));
				SlotObj->SetObjectField(TEXT("rotation"), RotToJson(FRotator(Slot.Rotation)));
				SlotObj->SetObjectField(TEXT("local_transform"), TransformToJson(FTransform(FRotator(Slot.Rotation), FVector(Slot.Offset), FVector::OneVector)));
				SlotObj->SetBoolField(TEXT("enabled"), Slot.bEnabled);
				SlotObj->SetArrayField(TEXT("activity_tags"), GameplayTagsToJson(Slot.ActivityTags));
				SlotObj->SetArrayField(TEXT("tags"), GameplayTagsToJson(Slot.ActivityTags));
				SlotObj->SetArrayField(TEXT("runtime_tags"), GameplayTagsToJson(Slot.RuntimeTags));
				TArray<TSharedPtr<FJsonValue>> Behaviors;
				for (int32 BehaviorIndex = 0; BehaviorIndex < Slot.BehaviorDefinitions.Num(); ++BehaviorIndex)
				{
					const USmartObjectBehaviorDefinition* Behavior = Slot.BehaviorDefinitions[BehaviorIndex];
					Behaviors.Add(MakeShared<FJsonValueObject>(ExportBehaviorDefinition(Behavior, FString::Printf(TEXT("slot_%d_behavior_%d"), Index, BehaviorIndex))));
				}
				SlotBehaviorCount += Behaviors.Num();
				SlotObj->SetArrayField(TEXT("behavior_definitions"), Behaviors);
				SlotObj->SetNumberField(TEXT("behavior_definition_count"), Behaviors.Num());
				Slots.Add(MakeShared<FJsonValueObject>(SlotObj));
			}
		}
		Root->SetArrayField(TEXT("slots"), Slots);
		Root->SetNumberField(TEXT("slot_count"), Slots.Num());
		Root->SetNumberField(TEXT("default_behavior_definition_count"), DefaultBehaviors.Num());
		Root->SetNumberField(TEXT("behavior_definition_count"), DefaultBehaviors.Num() + SlotBehaviorCount);
		if (Definition)
		{
			TArray<TPair<EMessageSeverity::Type, FText>> Errors;
			const bool bValid = Definition->Validate(&Errors);
			TArray<TSharedPtr<FJsonValue>> ValidationErrors;
			for (const TPair<EMessageSeverity::Type, FText>& Pair : Errors)
			{
				TSharedPtr<FJsonObject> ErrorObj = MakeShared<FJsonObject>();
				ErrorObj->SetNumberField(TEXT("severity"), static_cast<int32>(Pair.Key));
				ErrorObj->SetStringField(TEXT("message"), Pair.Value.ToString());
				ValidationErrors.Add(MakeShared<FJsonValueObject>(ErrorObj));
			}
			Root->SetBoolField(TEXT("definition_valid"), bValid);
			Root->SetArrayField(TEXT("validation_messages"), ValidationErrors);
		}
		return Root;
	}

	static bool ValidateSmartObjectDefinitionJson(const TSharedPtr<FJsonObject>& Root, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		if (!Root.IsValid())
		{
			AddIssue(Issues, TEXT("invalid_json_root"), TEXT("Root must be an object."), TEXT("$"));
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Slots = nullptr;
		if (!Root->TryGetArrayField(TEXT("slots"), Slots) || !Slots)
		{
			AddIssue(Issues, TEXT("missing_required_field"), TEXT("Missing slots array."), TEXT("$.slots"));
			return false;
		}
		TSet<FString> Names;
		auto ValidateBehaviorDefinitions = [&Issues](const TArray<TSharedPtr<FJsonValue>>* Behaviors, const FString& Path)
		{
			if (!Behaviors)
			{
				return;
			}
			for (int32 BehaviorIndex = 0; BehaviorIndex < Behaviors->Num(); ++BehaviorIndex)
			{
				const TSharedPtr<FJsonObject>* BehaviorObjPtr = nullptr;
				if (!(*Behaviors)[BehaviorIndex].IsValid() || !(*Behaviors)[BehaviorIndex]->TryGetObject(BehaviorObjPtr) || !BehaviorObjPtr || !BehaviorObjPtr->IsValid())
				{
					AddIssue(Issues, TEXT("invalid_behavior_definition_item"), TEXT("Behavior definition item must be an object."), FString::Printf(TEXT("%s[%d]"), *Path, BehaviorIndex));
					continue;
				}
				FString BehaviorClassPath;
				if (!(*BehaviorObjPtr)->TryGetStringField(TEXT("class"), BehaviorClassPath) || BehaviorClassPath.IsEmpty())
				{
					AddIssue(Issues, TEXT("missing_required_field"), TEXT("Missing behavior definition class."), FString::Printf(TEXT("%s[%d].class"), *Path, BehaviorIndex));
					continue;
				}
				UClass* BehaviorClass = ResolveClass(BehaviorClassPath);
				if (!BehaviorClass || !BehaviorClass->IsChildOf(USmartObjectBehaviorDefinition::StaticClass()) || BehaviorClass->HasAnyClassFlags(CLASS_Abstract))
				{
					AddIssue(Issues, TEXT("smart_object_behavior_definition_invalid"), FString::Printf(TEXT("Invalid behavior definition class '%s'."), *BehaviorClassPath), FString::Printf(TEXT("%s[%d].class"), *Path, BehaviorIndex));
				}
			}
		};
		const TArray<TSharedPtr<FJsonValue>>* DefaultBehaviors = nullptr;
		if (Root->TryGetArrayField(TEXT("default_behavior_definitions"), DefaultBehaviors))
		{
			ValidateBehaviorDefinitions(DefaultBehaviors, TEXT("$.default_behavior_definitions"));
		}
		for (int32 Index = 0; Index < Slots->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>* SlotObjPtr = nullptr;
			if (!(*Slots)[Index].IsValid() || !(*Slots)[Index]->TryGetObject(SlotObjPtr) || !SlotObjPtr || !SlotObjPtr->IsValid())
			{
				AddIssue(Issues, TEXT("invalid_slot_item"), TEXT("Slot item must be an object."), FString::Printf(TEXT("$.slots[%d]"), Index));
				continue;
			}
			FString Name;
			(*SlotObjPtr)->TryGetStringField(TEXT("name"), Name);
			if (!Name.IsEmpty())
			{
				if (Names.Contains(Name))
				{
					AddIssue(Issues, TEXT("duplicate_id"), FString::Printf(TEXT("Duplicate slot name '%s'."), *Name), FString::Printf(TEXT("$.slots[%d].name"), Index));
				}
				Names.Add(Name);
			}
			const TArray<TSharedPtr<FJsonValue>>* Behaviors = nullptr;
			if ((*SlotObjPtr)->TryGetArrayField(TEXT("behavior_definitions"), Behaviors))
			{
				ValidateBehaviorDefinitions(Behaviors, FString::Printf(TEXT("$.slots[%d].behavior_definitions"), Index));
			}
		}
		return Issues.Num() == 0;
	}

	static void BuildStateTreeSplitJson(
		const TSharedPtr<FJsonObject>& Exported,
		TSharedPtr<FJsonObject>& OutSchema,
		TSharedPtr<FJsonObject>& OutParameters,
		TSharedPtr<FJsonObject>& OutTransitions,
		TSharedPtr<FJsonObject>& OutTasks,
		TSharedPtr<FJsonObject>& OutConditions,
		TSharedPtr<FJsonObject>& OutEvaluators,
		TSharedPtr<FJsonObject>& OutBindings,
		TSharedPtr<FJsonObject>& OutGlobalTasks)
	{
		const FString Profile(StateTreeProfile);
		TArray<TSharedPtr<FJsonValue>> Empty;
		FString AssetPath;
		FString ObjectPath;
		FString SchemaClass;
		if (Exported.IsValid())
		{
			Exported->TryGetStringField(TEXT("asset_path"), AssetPath);
			Exported->TryGetStringField(TEXT("object_path"), ObjectPath);
			Exported->TryGetStringField(TEXT("schema_class"), SchemaClass);
		}
		OutSchema = MakeShared<FJsonObject>();
		OutSchema->SetStringField(TEXT("profile"), Profile);
		OutSchema->SetNumberField(TEXT("schema_version"), 1);
		OutSchema->SetStringField(TEXT("asset_path"), AssetPath);
		OutSchema->SetStringField(TEXT("object_path"), ObjectPath);
		OutSchema->SetStringField(TEXT("schema_class"), SchemaClass);
		OutSchema->SetArrayField(TEXT("context_data"), Empty);

		TArray<TSharedPtr<FJsonValue>> Transitions;
		TArray<TSharedPtr<FJsonValue>> Tasks;
		TArray<TSharedPtr<FJsonValue>> Conditions;
		const TArray<TSharedPtr<FJsonValue>>* States = nullptr;
		if (Exported.IsValid() && Exported->TryGetArrayField(TEXT("states"), States) && States)
		{
			for (const TSharedPtr<FJsonValue>& StateValue : *States)
			{
				const TSharedPtr<FJsonObject>* StateObjPtr = nullptr;
				if (!StateValue.IsValid() || !StateValue->TryGetObject(StateObjPtr) || !StateObjPtr || !StateObjPtr->IsValid())
				{
					continue;
				}
				const TSharedPtr<FJsonObject>& StateObj = *StateObjPtr;
				FString StateId;
				StateObj->TryGetStringField(TEXT("id"), StateId);
				const TArray<TSharedPtr<FJsonValue>>* StateTasks = nullptr;
				if (StateObj->TryGetArrayField(TEXT("tasks"), StateTasks) && StateTasks)
				{
					for (const TSharedPtr<FJsonValue>& TaskValue : *StateTasks)
					{
						const TSharedPtr<FJsonObject>* TaskObjPtr = nullptr;
						if (TaskValue.IsValid() && TaskValue->TryGetObject(TaskObjPtr) && TaskObjPtr && TaskObjPtr->IsValid())
						{
							TSharedPtr<FJsonObject> TaskCopy = MakeShared<FJsonObject>(**TaskObjPtr);
							TaskCopy->SetStringField(TEXT("state_id"), StateId);
							Tasks.Add(MakeShared<FJsonValueObject>(TaskCopy));
						}
					}
				}
				const TArray<TSharedPtr<FJsonValue>>* EnterConditions = nullptr;
				if (StateObj->TryGetArrayField(TEXT("enter_conditions"), EnterConditions) && EnterConditions)
				{
					for (const TSharedPtr<FJsonValue>& ConditionValue : *EnterConditions)
					{
						const TSharedPtr<FJsonObject>* ConditionObjPtr = nullptr;
						if (ConditionValue.IsValid() && ConditionValue->TryGetObject(ConditionObjPtr) && ConditionObjPtr && ConditionObjPtr->IsValid())
						{
							TSharedPtr<FJsonObject> ConditionCopy = MakeShared<FJsonObject>(**ConditionObjPtr);
							ConditionCopy->SetStringField(TEXT("state_id"), StateId);
							ConditionCopy->SetStringField(TEXT("condition_scope"), TEXT("enter"));
							Conditions.Add(MakeShared<FJsonValueObject>(ConditionCopy));
						}
					}
				}
				const TArray<TSharedPtr<FJsonValue>>* StateTransitions = nullptr;
				if (StateObj->TryGetArrayField(TEXT("transitions"), StateTransitions) && StateTransitions)
				{
					for (const TSharedPtr<FJsonValue>& TransitionValue : *StateTransitions)
					{
						const TSharedPtr<FJsonObject>* TransitionObjPtr = nullptr;
						if (!TransitionValue.IsValid() || !TransitionValue->TryGetObject(TransitionObjPtr) || !TransitionObjPtr || !TransitionObjPtr->IsValid())
						{
							continue;
						}
						TSharedPtr<FJsonObject> TransitionCopy = MakeShared<FJsonObject>(**TransitionObjPtr);
						TransitionCopy->SetStringField(TEXT("source_state"), StateId);
						FString TransitionId;
						TransitionCopy->TryGetStringField(TEXT("id"), TransitionId);
						Transitions.Add(MakeShared<FJsonValueObject>(TransitionCopy));

						const TArray<TSharedPtr<FJsonValue>>* TransitionConditions = nullptr;
						if ((*TransitionObjPtr)->TryGetArrayField(TEXT("conditions"), TransitionConditions) && TransitionConditions)
						{
							for (const TSharedPtr<FJsonValue>& ConditionValue : *TransitionConditions)
							{
								const TSharedPtr<FJsonObject>* ConditionObjPtr = nullptr;
								if (ConditionValue.IsValid() && ConditionValue->TryGetObject(ConditionObjPtr) && ConditionObjPtr && ConditionObjPtr->IsValid())
								{
									TSharedPtr<FJsonObject> ConditionCopy = MakeShared<FJsonObject>(**ConditionObjPtr);
									ConditionCopy->SetStringField(TEXT("state_id"), StateId);
									ConditionCopy->SetStringField(TEXT("transition_id"), TransitionId);
									ConditionCopy->SetStringField(TEXT("condition_scope"), TEXT("transition"));
									Conditions.Add(MakeShared<FJsonValueObject>(ConditionCopy));
								}
							}
						}
					}
				}
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Evaluators = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* GlobalTasks = nullptr;
		OutParameters = MakeProfileArrayJson(Profile, TEXT("parameters"), Empty);
		OutTransitions = MakeProfileArrayJson(Profile, TEXT("transitions"), Transitions);
		OutTasks = MakeProfileArrayJson(Profile, TEXT("tasks"), Tasks);
		OutConditions = MakeProfileArrayJson(Profile, TEXT("conditions"), Conditions);
		OutEvaluators = MakeProfileArrayJson(Profile, TEXT("evaluators"), (Exported.IsValid() && Exported->TryGetArrayField(TEXT("evaluators"), Evaluators) && Evaluators) ? *Evaluators : Empty);
		OutBindings = MakeProfileArrayJson(Profile, TEXT("bindings"), Empty);
		OutGlobalTasks = MakeProfileArrayJson(Profile, TEXT("global_tasks"), (Exported.IsValid() && Exported->TryGetArrayField(TEXT("global_tasks"), GlobalTasks) && GlobalTasks) ? *GlobalTasks : Empty);
	}

	static void BuildEqsSplitJson(
		const TSharedPtr<FJsonObject>& Exported,
		TSharedPtr<FJsonObject>& OutGenerators,
		TSharedPtr<FJsonObject>& OutTests,
		TSharedPtr<FJsonObject>& OutContexts)
	{
		const FString Profile(EqsProfile);
		TArray<TSharedPtr<FJsonValue>> Generators;
		TArray<TSharedPtr<FJsonValue>> Tests;
		TArray<TSharedPtr<FJsonValue>> Contexts;
		const TArray<TSharedPtr<FJsonValue>>* Options = nullptr;
		if (Exported.IsValid() && Exported->TryGetArrayField(TEXT("options"), Options) && Options)
		{
			for (int32 OptionIndex = 0; OptionIndex < Options->Num(); ++OptionIndex)
			{
				const TSharedPtr<FJsonObject>* OptionObjPtr = nullptr;
				if (!(*Options)[OptionIndex].IsValid() || !(*Options)[OptionIndex]->TryGetObject(OptionObjPtr) || !OptionObjPtr || !OptionObjPtr->IsValid())
				{
					continue;
				}
				const TSharedPtr<FJsonObject>& OptionObj = *OptionObjPtr;
				FString OptionId;
				OptionObj->TryGetStringField(TEXT("id"), OptionId);
				if (OptionId.IsEmpty())
				{
					OptionId = FString::Printf(TEXT("option_%d"), OptionIndex);
				}
				FString GeneratorClass;
				FString GeneratorName;
				OptionObj->TryGetStringField(TEXT("generator_class"), GeneratorClass);
				OptionObj->TryGetStringField(TEXT("generator_name"), GeneratorName);
				TSharedPtr<FJsonObject> GeneratorObj = MakeShared<FJsonObject>();
				GeneratorObj->SetStringField(TEXT("id"), OptionId + TEXT("_generator"));
				GeneratorObj->SetStringField(TEXT("option_id"), OptionId);
				GeneratorObj->SetStringField(TEXT("class"), GeneratorClass);
				GeneratorObj->SetStringField(TEXT("name"), GeneratorName);
				Generators.Add(MakeShared<FJsonValueObject>(GeneratorObj));

				const TArray<TSharedPtr<FJsonValue>>* OptionTests = nullptr;
				if (OptionObj->TryGetArrayField(TEXT("tests"), OptionTests) && OptionTests)
				{
					for (int32 TestIndex = 0; TestIndex < OptionTests->Num(); ++TestIndex)
					{
						const TSharedPtr<FJsonObject>* TestObjPtr = nullptr;
						if (OptionTests->IsValidIndex(TestIndex) && (*OptionTests)[TestIndex].IsValid() && (*OptionTests)[TestIndex]->TryGetObject(TestObjPtr) && TestObjPtr && TestObjPtr->IsValid())
						{
							TSharedPtr<FJsonObject> TestCopy = MakeShared<FJsonObject>(**TestObjPtr);
							FString TestId;
							TestCopy->TryGetStringField(TEXT("id"), TestId);
							if (TestId.IsEmpty())
							{
								TestId = FString::Printf(TEXT("test_%d"), TestIndex);
								TestCopy->SetStringField(TEXT("id"), TestId);
							}
							TestCopy->SetStringField(TEXT("option_id"), OptionId);
							Tests.Add(MakeShared<FJsonValueObject>(TestCopy));
						}
					}
				}
			}
		}
		OutGenerators = MakeProfileArrayJson(Profile, TEXT("generators"), Generators);
		OutTests = MakeProfileArrayJson(Profile, TEXT("tests"), Tests);
		OutContexts = MakeProfileArrayJson(Profile, TEXT("contexts"), Contexts);
	}

	static bool SaveBehaviorTreeFolderContents(const FString& FolderPath, UBehaviorTree* Tree, TArray<TSharedPtr<FJsonValue>>& WrittenFiles, FString& OutError)
	{
		if (!Tree)
		{
			OutError = TEXT("behavior_tree_not_found");
			return false;
		}
		IFileManager::Get().MakeDirectory(*FolderPath, true);
		TSharedPtr<FJsonObject> Exported = ExportBehaviorTree(Tree, true);
		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Edges = nullptr;
		Exported->TryGetArrayField(TEXT("nodes"), Nodes);
		Exported->TryGetArrayField(TEXT("edges"), Edges);
		TSharedPtr<FJsonObject> TreeJson = MakeShared<FJsonObject>();
		TreeJson->SetStringField(TEXT("profile"), BehaviorTreeProfile);
		TreeJson->SetNumberField(TEXT("schema_version"), 1);
		TreeJson->SetStringField(TEXT("root_id"), TEXT("root"));
		TreeJson->SetArrayField(TEXT("edges"), Edges ? *Edges : TArray<TSharedPtr<FJsonValue>>());
		TSharedPtr<FJsonObject> NodesJson = MakeProfileArrayJson(BehaviorTreeProfile, TEXT("nodes"), Nodes ? *Nodes : TArray<TSharedPtr<FJsonValue>>());
		TSharedPtr<FJsonObject> BlackboardJson = MakeShared<FJsonObject>();
		BlackboardJson->SetStringField(TEXT("profile"), BehaviorTreeProfile);
		BlackboardJson->SetNumberField(TEXT("schema_version"), 1);
		BlackboardJson->SetStringField(TEXT("blackboard_asset"), Tree->BlackboardAsset ? Tree->BlackboardAsset->GetPathName() : FString());
		if (!SaveTrackedFolderJson(FolderPath, TEXT("asset.json"), MakeBTAssetJson(Tree), WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("blackboard.json"), BlackboardJson, WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("tree.json"), TreeJson, WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("nodes.json"), NodesJson, WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("decorators.json"), MakeShared<FJsonObject>(), WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("services.json"), MakeShared<FJsonObject>(), WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("layout.json"), MakeShared<FJsonObject>(), WrittenFiles, OutError)) return false;
		IFileManager::Get().MakeDirectory(*FPaths::Combine(FolderPath, TEXT("validation")), true);
		TArray<TSharedPtr<FJsonValue>> Issues;
		ValidateBTFolderJson(MakeBTAssetJson(Tree), NodesJson, Issues);
		if (!SaveTrackedFolderJson(FolderPath, TEXT("validation/checks.json"), MakeValidationChecks(Issues), WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("validation/coverage_report.json"), MakeCoverageReport(), WrittenFiles, OutError)) return false;
		return true;
	}

	static bool SaveStateTreeFolderContents(const FString& FolderPath, UStateTree* Tree, TArray<TSharedPtr<FJsonValue>>& WrittenFiles, FString& OutError)
	{
		if (!Tree)
		{
			OutError = TEXT("state_tree_not_found");
			return false;
		}
		IFileManager::Get().MakeDirectory(*FolderPath, true);
		TSharedPtr<FJsonObject> Exported = ExportStateTree(Tree, true);
		TSharedPtr<FJsonObject> AssetJson = ExportStateTree(Tree, false);
		const TArray<TSharedPtr<FJsonValue>>* States = nullptr;
		Exported->TryGetArrayField(TEXT("states"), States);
		TSharedPtr<FJsonObject> StatesJson = MakeProfileArrayJson(StateTreeProfile, TEXT("states"), States ? *States : TArray<TSharedPtr<FJsonValue>>());
		TSharedPtr<FJsonObject> SchemaJson;
		TSharedPtr<FJsonObject> ParametersJson;
		TSharedPtr<FJsonObject> TransitionsJson;
		TSharedPtr<FJsonObject> TasksJson;
		TSharedPtr<FJsonObject> ConditionsJson;
		TSharedPtr<FJsonObject> EvaluatorsJson;
		TSharedPtr<FJsonObject> BindingsJson;
		TSharedPtr<FJsonObject> GlobalTasksJson;
		BuildStateTreeSplitJson(Exported, SchemaJson, ParametersJson, TransitionsJson, TasksJson, ConditionsJson, EvaluatorsJson, BindingsJson, GlobalTasksJson);
		if (!SaveTrackedFolderJson(FolderPath, TEXT("asset.json"), AssetJson, WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("schema.json"), SchemaJson, WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("parameters.json"), ParametersJson, WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("states.json"), StatesJson, WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("transitions.json"), TransitionsJson, WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("tasks.json"), TasksJson, WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("conditions.json"), ConditionsJson, WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("evaluators.json"), EvaluatorsJson, WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("bindings.json"), BindingsJson, WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("global_tasks.json"), GlobalTasksJson, WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("property_bag.json"), MakeShared<FJsonObject>(), WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("graph_layout.json"), MakeShared<FJsonObject>(), WrittenFiles, OutError)) return false;
		IFileManager::Get().MakeDirectory(*FPaths::Combine(FolderPath, TEXT("validation")), true);
		TArray<TSharedPtr<FJsonValue>> Issues;
		ValidateStateTreeFolderJson(AssetJson, StatesJson, Issues);
		if (!SaveTrackedFolderJson(FolderPath, TEXT("validation/checks.json"), MakeValidationChecks(Issues), WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("validation/coverage_report.json"), MakeCoverageReport(), WrittenFiles, OutError)) return false;
		return true;
	}

	static bool SaveEqsFolderContents(const FString& FolderPath, UEnvQuery* Query, TArray<TSharedPtr<FJsonValue>>& WrittenFiles, FString& OutError)
	{
		if (!Query)
		{
			OutError = TEXT("eqs_query_not_found");
			return false;
		}
		IFileManager::Get().MakeDirectory(*FolderPath, true);
		TSharedPtr<FJsonObject> Exported = ExportEqs(Query, true);
		TSharedPtr<FJsonObject> AssetJson = ExportEqs(Query, false);
		const TArray<TSharedPtr<FJsonValue>>* Options = nullptr;
		Exported->TryGetArrayField(TEXT("options"), Options);
		TSharedPtr<FJsonObject> OptionsJson = MakeProfileArrayJson(EqsProfile, TEXT("options"), Options ? *Options : TArray<TSharedPtr<FJsonValue>>());
		TSharedPtr<FJsonObject> GeneratorsJson;
		TSharedPtr<FJsonObject> TestsJson;
		TSharedPtr<FJsonObject> ContextsJson;
		BuildEqsSplitJson(Exported, GeneratorsJson, TestsJson, ContextsJson);
		if (!SaveTrackedFolderJson(FolderPath, TEXT("asset.json"), AssetJson, WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("options.json"), OptionsJson, WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("generators.json"), GeneratorsJson, WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("tests.json"), TestsJson, WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("contexts.json"), ContextsJson, WrittenFiles, OutError)) return false;
		IFileManager::Get().MakeDirectory(*FPaths::Combine(FolderPath, TEXT("validation")), true);
		TArray<TSharedPtr<FJsonValue>> Issues;
		ValidateEqsFolderJson(AssetJson, OptionsJson, Issues);
		if (!SaveTrackedFolderJson(FolderPath, TEXT("validation/checks.json"), MakeValidationChecks(Issues), WrittenFiles, OutError)) return false;
		if (!SaveTrackedFolderJson(FolderPath, TEXT("validation/coverage_report.json"), MakeCoverageReport(), WrittenFiles, OutError)) return false;
		return true;
	}

	static TSharedPtr<FJsonObject> SmartObjectRequestResultToJson(const FSmartObjectRequestResult& Result, USmartObjectSubsystem* Subsystem, const FString& ResultId)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("result_id"), ResultId);
		Obj->SetStringField(TEXT("smart_object_handle"), LexToString(Result.SmartObjectHandle));
		Obj->SetStringField(TEXT("slot_handle"), LexToString(Result.SlotHandle));
		Obj->SetBoolField(TEXT("valid"), Result.IsValid());
		if (Subsystem && Result.IsValid())
		{
			if (TOptional<FTransform> SlotTransform = Subsystem->GetSlotTransform(Result))
			{
				Obj->SetObjectField(TEXT("slot_transform"), TransformToJson(*SlotTransform));
			}
			if (USmartObjectComponent* Component = Subsystem->GetSmartObjectComponentByRequestResult(Result))
			{
				Obj->SetStringField(TEXT("component"), Component->GetName());
				Obj->SetStringField(TEXT("owner"), Component->GetOwner() ? Component->GetOwner()->GetName() : FString());
				Obj->SetStringField(TEXT("definition"), Component->GetDefinition() ? Component->GetDefinition()->GetPathName() : FString());
			}
		}
		return Obj;
	}

	static bool ResolveGenericAssetEditorWindow(UObject* Asset, const bool bOpenIfNeeded, TSharedPtr<SWindow>& OutWindow, FString& OutError)
	{
		OutWindow.Reset();
		if (!GEditor)
		{
			OutError = TEXT("editor_unavailable");
			return false;
		}
		UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		if (!AssetEditorSubsystem)
		{
			OutError = TEXT("asset_editor_subsystem_unavailable");
			return false;
		}
		if (bOpenIfNeeded)
		{
			AssetEditorSubsystem->OpenEditorForAsset(Asset);
		}
		IAssetEditorInstance* AssetEditor = AssetEditorSubsystem->FindEditorForAsset(Asset, false);
		if (!AssetEditor)
		{
			OutError = bOpenIfNeeded ? TEXT("asset_editor_open_failed") : TEXT("asset_editor_not_opened");
			return false;
		}
		const TSharedPtr<FTabManager> TabManager = AssetEditor->GetAssociatedTabManager();
		if (!TabManager.IsValid())
		{
			OutError = TEXT("asset_editor_tab_manager_not_found");
			return false;
		}
		const TSharedPtr<SDockTab> OwnerTab = TabManager->GetOwnerTab();
		if (!OwnerTab.IsValid())
		{
			OutError = TEXT("asset_editor_owner_tab_not_found");
			return false;
		}
		OutWindow = OwnerTab->GetParentWindow();
		if (!OutWindow.IsValid())
		{
			OutError = TEXT("asset_editor_window_not_found");
			return false;
		}
		return true;
	}

	static FVector2D ResolveOffscreenWindowDrawSize(const TSharedPtr<SWindow>& Window)
	{
		FVector2D DrawSize(0.0, 0.0);
		if (Window.IsValid())
		{
			DrawSize = Window->GetClientSizeInScreen();
			if (DrawSize.X <= 0.0 || DrawSize.Y <= 0.0)
			{
				DrawSize = Window->GetCachedGeometry().GetLocalSize();
			}
			if (DrawSize.X <= 0.0 || DrawSize.Y <= 0.0)
			{
				DrawSize = Window->GetDesiredSize();
			}
		}
		if (DrawSize.X <= 0.0 || DrawSize.Y <= 0.0)
		{
			DrawSize = FVector2D(1440.0, 900.0);
		}

		const double MaxDimension = 4096.0;
		if (DrawSize.X > MaxDimension || DrawSize.Y > MaxDimension)
		{
			const double Scale = FMath::Min(MaxDimension / DrawSize.X, MaxDimension / DrawSize.Y);
			DrawSize *= Scale;
		}
		DrawSize.X = FMath::Clamp(FMath::RoundToDouble(DrawSize.X), 64.0, MaxDimension);
		DrawSize.Y = FMath::Clamp(FMath::RoundToDouble(DrawSize.Y), 64.0, MaxDimension);
		return DrawSize;
	}

	static bool IsSlateWindowBackbufferCaptureUnsafe(const TSharedPtr<SWindow>& Window, FString& OutUnsafeReason)
	{
		OutUnsafeReason.Reset();
		if (!Window.IsValid())
		{
			OutUnsafeReason = TEXT("window_invalid");
			return true;
		}
		if (!FSlateApplication::IsInitialized())
		{
			OutUnsafeReason = TEXT("slate_not_initialized");
			return true;
		}
		if (!FApp::CanEverRender())
		{
			OutUnsafeReason = TEXT("rendering_not_available");
			return true;
		}
		if (!Window->IsVisible())
		{
			OutUnsafeReason = TEXT("window_not_visible");
			return true;
		}
		if (Window->IsWindowMinimized())
		{
			OutUnsafeReason = TEXT("window_minimized");
			return true;
		}
		if (const TSharedPtr<FGenericWindow> NativeWindow = Window->GetNativeWindow())
		{
			if (!NativeWindow->IsVisible())
			{
				OutUnsafeReason = TEXT("native_window_not_visible");
				return true;
			}
			if (NativeWindow->IsMinimized())
			{
				OutUnsafeReason = TEXT("native_window_minimized");
				return true;
			}
		}
		const FVector2D ClientSize = Window->GetClientSizeInScreen();
		if (ClientSize.X < 64.0 || ClientSize.Y < 64.0)
		{
			OutUnsafeReason = FString::Printf(TEXT("window_too_small:%.0fx%.0f"), ClientSize.X, ClientSize.Y);
			return true;
		}
		return false;
	}

	static bool ScreenshotSlateWindow(const TSharedPtr<SWindow>& Window, TArray<FColor>& OutPixels, FIntPoint& OutSize, FString& OutError)
	{
		if (!Window.IsValid())
		{
			OutError = TEXT("window_not_found");
			return false;
		}
		if (!FSlateApplication::IsInitialized())
		{
			OutError = TEXT("slate_not_initialized");
			return false;
		}
		if (!FApp::CanEverRender())
		{
			OutError = TEXT("rendering_not_available");
			return false;
		}

		FSlateApplication::Get().PumpMessages();
		const float WindowScale = FSlateApplication::Get().GetApplicationScale() * Window->GetDPIScaleFactor();
		Window->SlatePrepass(WindowScale);
		FSlateApplication::Get().InvalidateAllViewports();

		const FVector2D DrawSize = ResolveOffscreenWindowDrawSize(Window);
		FWidgetRenderer* WidgetRenderer = new FWidgetRenderer(true, true);
		if (!WidgetRenderer)
		{
			OutError = TEXT("widget_renderer_create_failed");
			return false;
		}

		UTextureRenderTarget2D* RenderTarget = FWidgetRenderer::CreateTargetFor(DrawSize, TF_Bilinear, true);
		if (!RenderTarget)
		{
			BeginCleanup(WidgetRenderer);
			OutError = TEXT("offscreen_render_target_create_failed");
			return false;
		}

		WidgetRenderer->DrawWidget(RenderTarget, Window.ToSharedRef(), 1.0f, DrawSize, 0.0f, false);
		FlushRenderingCommands();
		BeginCleanup(WidgetRenderer);

		FRenderTarget* RenderTargetResource = RenderTarget->GameThread_GetRenderTargetResource();
		if (!RenderTargetResource)
		{
			OutError = TEXT("offscreen_render_target_resource_missing");
			return false;
		}

		FReadSurfaceDataFlags ReadFlags(RCM_UNorm);
		ReadFlags.SetLinearToGamma(false);
		if (!RenderTargetResource->ReadPixels(OutPixels, ReadFlags) || OutPixels.Num() <= 0)
		{
			OutError = TEXT("offscreen_read_pixels_failed");
			return false;
		}

		OutSize = FIntPoint(RenderTarget->SizeX, RenderTarget->SizeY);
		if (OutSize.X <= 0 || OutSize.Y <= 0 || OutPixels.Num() < OutSize.X * OutSize.Y)
		{
			OutError = TEXT("offscreen_screenshot_empty");
			return false;
		}
		return true;
	}

	static int32 CountNonBlackPixels(const TArray<FColor>& Pixels)
	{
		int32 NonBlack = 0;
		for (const FColor& Pixel : Pixels)
		{
			const int32 Luma = (static_cast<int32>(Pixel.R) + static_cast<int32>(Pixel.G) + static_cast<int32>(Pixel.B)) / 3;
			if (Luma > 4 || Pixel.A > 4)
			{
				++NonBlack;
			}
		}
		return NonBlack;
	}

	static void AddPixelStats(const TArray<FColor>& Pixels, TSharedPtr<FJsonObject>& OutData)
	{
		double LumaSum = 0.0;
		for (const FColor& Pixel : Pixels)
		{
			const int32 Luma = (static_cast<int32>(Pixel.R) + static_cast<int32>(Pixel.G) + static_cast<int32>(Pixel.B)) / 3;
			LumaSum += Luma;
		}
		const int32 NonBlack = CountNonBlackPixels(Pixels);
		OutData->SetNumberField(TEXT("non_black_pixel_count"), NonBlack);
		OutData->SetNumberField(TEXT("average_luma"), Pixels.Num() > 0 ? LumaSum / static_cast<double>(Pixels.Num()) : 0.0);
		OutData->SetBoolField(TEXT("appears_non_black"), NonBlack > 0);
	}

	static AActor* ResolveActorOrFirst(UWorld* World, const FString& ActorName, UClass* RequiredComponentClass, UActorComponent*& OutComponent)
	{
		OutComponent = nullptr;
		if (!World || !RequiredComponentClass)
		{
			return nullptr;
		}
		if (!ActorName.IsEmpty())
		{
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* Actor = *It;
				if (!Actor)
				{
					continue;
				}
				const FString Label = Actor->GetActorLabel();
				if (Actor->GetName().Equals(ActorName, ESearchCase::IgnoreCase) || Label.Equals(ActorName, ESearchCase::IgnoreCase))
				{
					OutComponent = Actor->GetComponentByClass(RequiredComponentClass);
					return Actor;
				}
			}
		}
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (UActorComponent* Component = It->GetComponentByClass(RequiredComponentClass))
			{
				OutComponent = Component;
				return *It;
			}
		}
		return nullptr;
	}
}

bool FUeAgentHttpServer::CmdStateTreeCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UStateTree* Tree = nullptr;
	if (!UeAgentAIOps::CreateAsset<UStateTree>(AssetPath, Tree, OutError) || !Tree)
	{
		return false;
	}
	FString SchemaClassPath;
	JsonTryGetString(Ctx.Params, TEXT("schema_class"), SchemaClassPath);
	UClass* SchemaClass = UStateTreeComponentSchema::StaticClass();
	if (!SchemaClassPath.IsEmpty())
	{
		SchemaClass = UeAgentAIOps::ResolveClass(SchemaClassPath);
		if (!SchemaClass)
		{
			OutError = TEXT("invalid_state_tree_schema_class");
			return false;
		}
	}
	if (!UeAgentAIOps::EnsureStateTreeEditorData(Tree, SchemaClass, OutError))
	{
		return false;
	}
	TSharedPtr<FJsonObject> CompileReport;
	UeAgentAIOps::CompileStateTree(Tree, CompileReport);
	bool bSave = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_create"), bSave);
	if (bSave && !UeAgentAIOps::SaveAssetPackage(Tree, OutError))
	{
		return false;
	}
	OutData = UeAgentAIOps::ExportStateTree(Tree, true);
	OutData->SetObjectField(TEXT("compile_report"), CompileReport);
	OutData->SetBoolField(TEXT("created_asset"), true);
	OutData->SetBoolField(TEXT("saved"), bSave);
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	return true;
}

bool FUeAgentHttpServer::CmdStateTreeGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UStateTree* Tree = UeAgentAIOps::LoadAsset<UStateTree>(AssetPath);
	if (!Tree)
	{
		OutError = TEXT("state_tree_not_found");
		return false;
	}
	bool bIncludeStates = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_states"), bIncludeStates);
	OutData = UeAgentAIOps::ExportStateTree(Tree, bIncludeStates);
	return true;
}

bool FUeAgentHttpServer::CmdStateTreeExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UStateTree* Tree = UeAgentAIOps::LoadAsset<UStateTree>(AssetPath);
	if (!Tree)
	{
		OutError = TEXT("state_tree_not_found");
		return false;
	}
	FString FolderInput;
	JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderInput);
	const FString FolderPath = UeAgentAIOps::ResolveFolderPath(FolderInput, UeAgentAIOps::StateTreeProfile, AssetPath);
	bool bClean = false;
	JsonTryGetBool(Ctx.Params, TEXT("clean_output_dir"), bClean);
	if (bClean && IFileManager::Get().DirectoryExists(*FolderPath))
	{
		IFileManager::Get().DeleteDirectory(*FolderPath, false, true);
	}
	IFileManager::Get().MakeDirectory(*FolderPath, true);
	TArray<TSharedPtr<FJsonValue>> WrittenFiles;
	if (!UeAgentAIOps::SaveStateTreeFolderContents(FolderPath, Tree, WrittenFiles, OutError))
	{
		return false;
	}
	OutData = UeAgentAIOps::ExportStateTree(Tree, true);
	OutData->SetArrayField(TEXT("written_files"), WrittenFiles);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	UeAgentAIOps::AttachCoverageReport(OutData);
	return true;
}

bool FUeAgentHttpServer::CmdStateTreeValidateFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString FolderInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderInput) || FolderInput.IsEmpty())
	{
		OutError = TEXT("missing_folder_path");
		return false;
	}
	const FString FolderPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FolderInput);
	TSharedPtr<FJsonObject> AssetJson;
	TSharedPtr<FJsonObject> StatesJson;
	if (!UeAgentAIOps::LoadFolderJson(FolderPath, TEXT("asset.json"), AssetJson, OutError) ||
		!UeAgentAIOps::LoadFolderJson(FolderPath, TEXT("states.json"), StatesJson, OutError))
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	UeAgentAIOps::ValidateStateTreeFolderJson(AssetJson, StatesJson, Issues);
	OutData->SetStringField(TEXT("profile"), UeAgentAIOps::StateTreeProfile);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetStringField(TEXT("status"), Issues.Num() == 0 ? TEXT("ok") : TEXT("validate_failed"));
	UeAgentAIOps::SetDiagnostics(OutData, Issues);
	return Issues.Num() == 0;
}

bool FUeAgentHttpServer::CmdStateTreeApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString FolderInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderInput) || FolderInput.IsEmpty())
	{
		OutError = TEXT("missing_folder_path");
		return false;
	}
	const FString FolderPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FolderInput);
	TSharedPtr<FJsonObject> AssetJson;
	TSharedPtr<FJsonObject> StatesJson;
	if (!UeAgentAIOps::LoadFolderJson(FolderPath, TEXT("asset.json"), AssetJson, OutError) ||
		!UeAgentAIOps::LoadFolderJson(FolderPath, TEXT("states.json"), StatesJson, OutError))
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	if (!UeAgentAIOps::ValidateStateTreeFolderJson(AssetJson, StatesJson, Issues))
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
		OutData->SetStringField(TEXT("profile"), UeAgentAIOps::StateTreeProfile);
		OutData->SetStringField(TEXT("folder_path"), FolderPath);
		OutData->SetStringField(TEXT("status"), TEXT("ok"));
		OutData->SetBoolField(TEXT("dry_run"), bDryRun);
		OutData->SetBoolField(TEXT("validate_only"), bValidateOnly);
		UeAgentAIOps::SetDiagnostics(OutData, Issues);
		return true;
	}
	UStateTree* Tree = UeAgentAIOps::LoadAsset<UStateTree>(AssetPath);
	bool bCreated = false;
	if (!Tree)
	{
		if (!bCreateIfMissing)
		{
			OutError = TEXT("state_tree_not_found");
			return false;
		}
		if (!UeAgentAIOps::CreateAsset<UStateTree>(AssetPath, Tree, OutError) || !Tree)
		{
			return false;
		}
		bCreated = true;
	}
	FString SchemaClassPath;
	AssetJson->TryGetStringField(TEXT("schema_class"), SchemaClassPath);
	UClass* SchemaClass = UStateTreeComponentSchema::StaticClass();
	if (!SchemaClassPath.IsEmpty())
	{
		SchemaClass = UeAgentAIOps::ResolveClass(SchemaClassPath);
		if (!SchemaClass)
		{
			OutError = TEXT("invalid_state_tree_schema_class");
			return false;
		}
	}
	if (!UeAgentAIOps::EnsureStateTreeEditorData(Tree, SchemaClass, OutError))
	{
		return false;
	}
#if WITH_EDITORONLY_DATA
	UStateTreeEditorData* EditorData = UeAgentAIOps::GetStateTreeEditorData(Tree);
	if (!EditorData)
	{
		OutError = TEXT("state_tree_editor_data_unavailable");
		return false;
	}
	if (EditorData->SubTrees.Num() > 0 && !bCreated && !bAllowDestructive)
	{
		OutError = TEXT("state_tree_rebuild_requires_allow_destructive");
		return false;
	}
	Tree->Modify();
	EditorData->Modify();
	EditorData->SubTrees.Reset();
	EditorData->Evaluators.Reset();
	EditorData->GlobalTasks.Reset();
	TArray<TSharedPtr<FJsonValue>> PropertyResults;
	const TArray<TSharedPtr<FJsonValue>>* Evaluators = nullptr;
	if (StatesJson->TryGetArrayField(TEXT("evaluators"), Evaluators) && Evaluators)
	{
		for (const TSharedPtr<FJsonValue>& EvaluatorValue : *Evaluators)
		{
			const TSharedPtr<FJsonObject>* EvaluatorObjPtr = nullptr;
			if (EvaluatorValue->TryGetObject(EvaluatorObjPtr))
			{
				FStateTreeEditorNode& Node = EditorData->Evaluators.AddDefaulted_GetRef();
				if (!UeAgentAIOps::ConfigureStateTreeEditorNode(Node, EditorData, *EvaluatorObjPtr, FStateTreeEvaluatorBase::StaticStruct(), UStateTreeEvaluatorBlueprintBase::StaticClass(), PropertyResults, OutError))
				{
					return false;
				}
			}
		}
	}
	const TArray<TSharedPtr<FJsonValue>>* GlobalTasks = nullptr;
	if (StatesJson->TryGetArrayField(TEXT("global_tasks"), GlobalTasks) && GlobalTasks)
	{
		for (const TSharedPtr<FJsonValue>& TaskValue : *GlobalTasks)
		{
			const TSharedPtr<FJsonObject>* TaskObjPtr = nullptr;
			if (TaskValue->TryGetObject(TaskObjPtr))
			{
				FStateTreeEditorNode& Node = EditorData->GlobalTasks.AddDefaulted_GetRef();
				if (!UeAgentAIOps::ConfigureStateTreeEditorNode(Node, EditorData, *TaskObjPtr, FStateTreeTaskBase::StaticStruct(), UStateTreeTaskBlueprintBase::StaticClass(), PropertyResults, OutError))
				{
					return false;
				}
			}
		}
	}
	TMap<FString, UStateTreeState*> StateMap;
	TMap<FString, UStateTreeState*> StateNameMap;
	TMultiMap<FString, TSharedPtr<FJsonObject>> ChildrenByParent;
	const TArray<TSharedPtr<FJsonValue>>* States = nullptr;
	StatesJson->TryGetArrayField(TEXT("states"), States);
	for (const TSharedPtr<FJsonValue>& StateValue : *States)
	{
		const TSharedPtr<FJsonObject>* StateObjPtr = nullptr;
		StateValue->TryGetObject(StateObjPtr);
		FString ParentId;
		(*StateObjPtr)->TryGetStringField(TEXT("parent_id"), ParentId);
		ChildrenByParent.Add(ParentId, *StateObjPtr);
	}
	TFunction<UStateTreeState*(const TSharedPtr<FJsonObject>&, UStateTreeState*)> BuildState =
		[&](const TSharedPtr<FJsonObject>& StateObj, UStateTreeState* Parent) -> UStateTreeState*
		{
			FString Id;
			FString Name;
			FString TypeText;
			StateObj->TryGetStringField(TEXT("id"), Id);
			StateObj->TryGetStringField(TEXT("name"), Name);
			StateObj->TryGetStringField(TEXT("type"), TypeText);
			UStateTreeState* State = nullptr;
			if (Parent)
			{
				State = &Parent->AddChildState(FName(*Name), UeAgentAIOps::ParseStateType(TypeText));
			}
			else
			{
				State = &EditorData->AddSubTree(FName(*Name));
				State->Type = UeAgentAIOps::ParseStateType(TypeText);
			}
			State->ID = FGuid::NewGuid();
			State->Description = StateObj->GetStringField(TEXT("description"));
			FString SelectionText;
			FString CompletionText;
			StateObj->TryGetStringField(TEXT("selection_behavior"), SelectionText);
			StateObj->TryGetStringField(TEXT("tasks_completion"), CompletionText);
			State->SelectionBehavior = UeAgentAIOps::EnumFromString(SelectionText, EStateTreeStateSelectionBehavior::TrySelectChildrenInOrder);
			State->TasksCompletion = UeAgentAIOps::EnumFromString(CompletionText, EStateTreeTaskCompletionType::Any);
			bool bEnabled = true;
			StateObj->TryGetBoolField(TEXT("enabled"), bEnabled);
			State->bEnabled = bEnabled;
			FString TagText;
			if (StateObj->TryGetStringField(TEXT("tag"), TagText) && !TagText.IsEmpty())
			{
				State->Tag = FGameplayTag::RequestGameplayTag(FName(*TagText), false);
			}
			const TArray<TSharedPtr<FJsonValue>>* Conditions = nullptr;
			if (StateObj->TryGetArrayField(TEXT("enter_conditions"), Conditions) && Conditions)
			{
				for (const TSharedPtr<FJsonValue>& ConditionValue : *Conditions)
				{
					const TSharedPtr<FJsonObject>* ConditionObjPtr = nullptr;
					if (ConditionValue->TryGetObject(ConditionObjPtr))
					{
						FStateTreeEditorNode& Node = State->EnterConditions.AddDefaulted_GetRef();
						if (!UeAgentAIOps::ConfigureStateTreeEditorNode(Node, State, *ConditionObjPtr, FStateTreeConditionBase::StaticStruct(), UStateTreeConditionBlueprintBase::StaticClass(), PropertyResults, OutError))
						{
							return nullptr;
						}
					}
				}
			}
			const TArray<TSharedPtr<FJsonValue>>* Tasks = nullptr;
			if (StateObj->TryGetArrayField(TEXT("tasks"), Tasks) && Tasks)
			{
				for (const TSharedPtr<FJsonValue>& TaskValue : *Tasks)
				{
					const TSharedPtr<FJsonObject>* TaskObjPtr = nullptr;
					if (TaskValue->TryGetObject(TaskObjPtr))
					{
						FStateTreeEditorNode& Node = State->Tasks.AddDefaulted_GetRef();
						if (!UeAgentAIOps::ConfigureStateTreeEditorNode(Node, State, *TaskObjPtr, FStateTreeTaskBase::StaticStruct(), UStateTreeTaskBlueprintBase::StaticClass(), PropertyResults, OutError))
						{
							return nullptr;
						}
					}
				}
			}
			StateMap.Add(Id, State);
			StateNameMap.Add(Name, State);
			TArray<TSharedPtr<FJsonObject>> Children;
			ChildrenByParent.MultiFind(Id, Children);
			Algo::Reverse(Children);
			for (const TSharedPtr<FJsonObject>& ChildObj : Children)
			{
				if (!BuildState(ChildObj, State))
				{
					return nullptr;
				}
			}
			return State;
		};
	TArray<TSharedPtr<FJsonObject>> RootStates;
	ChildrenByParent.MultiFind(FString(), RootStates);
	Algo::Reverse(RootStates);
	for (const TSharedPtr<FJsonObject>& StateObj : RootStates)
	{
		if (!BuildState(StateObj, nullptr))
		{
			return false;
		}
	}
	for (const TSharedPtr<FJsonValue>& StateValue : *States)
	{
		const TSharedPtr<FJsonObject>* StateObjPtr = nullptr;
		if (!StateValue->TryGetObject(StateObjPtr) || !StateObjPtr || !StateObjPtr->IsValid())
		{
			continue;
		}
		FString SourceId;
		(*StateObjPtr)->TryGetStringField(TEXT("id"), SourceId);
		UStateTreeState* SourceState = StateMap.FindRef(SourceId);
		if (!SourceState)
		{
			continue;
		}
		const TArray<TSharedPtr<FJsonValue>>* Transitions = nullptr;
		if (!(*StateObjPtr)->TryGetArrayField(TEXT("transitions"), Transitions) || !Transitions)
		{
			continue;
		}
		for (const TSharedPtr<FJsonValue>& TransitionValue : *Transitions)
		{
			const TSharedPtr<FJsonObject>* TransitionObjPtr = nullptr;
			if (!TransitionValue->TryGetObject(TransitionObjPtr) || !TransitionObjPtr || !TransitionObjPtr->IsValid())
			{
				continue;
			}
			FString TriggerText;
			FString PriorityText;
			FString TypeText;
			FString FallbackText;
			FString TargetId;
			FString TargetName;
			(*TransitionObjPtr)->TryGetStringField(TEXT("trigger"), TriggerText);
			(*TransitionObjPtr)->TryGetStringField(TEXT("priority"), PriorityText);
			(*TransitionObjPtr)->TryGetStringField(TEXT("type"), TypeText);
			(*TransitionObjPtr)->TryGetStringField(TEXT("fallback"), FallbackText);
			(*TransitionObjPtr)->TryGetStringField(TEXT("target_state_id"), TargetId);
			(*TransitionObjPtr)->TryGetStringField(TEXT("target_state_name"), TargetName);

			const EStateTreeTransitionType LinkType = UeAgentAIOps::EnumFromString(TypeText, EStateTreeTransitionType::GotoState);
			UStateTreeState* TargetState = !TargetId.IsEmpty() ? StateMap.FindRef(TargetId) : nullptr;
			if (!TargetState && !TargetName.IsEmpty())
			{
				TargetState = StateNameMap.FindRef(TargetName);
			}
			if (LinkType == EStateTreeTransitionType::GotoState && !TargetState)
			{
				OutError = FString::Printf(TEXT("state_tree_transition_target_not_found:%s"), *TargetId);
				return false;
			}

			FStateTreeTransition& Transition = SourceState->Transitions.AddDefaulted_GetRef();
			Transition.ID = FGuid::NewGuid();
			Transition.Trigger = UeAgentAIOps::EnumFromString(TriggerText, EStateTreeTransitionTrigger::OnStateCompleted);
			Transition.Priority = UeAgentAIOps::EnumFromString(PriorityText, EStateTreeTransitionPriority::Normal);
			Transition.State.LinkType = LinkType;
			Transition.State.Fallback = UeAgentAIOps::EnumFromString(FallbackText, EStateTreeSelectionFallback::None);
			Transition.State.ID = TargetState ? TargetState->ID : FGuid();
			Transition.State.Name = TargetState ? TargetState->Name : FName(*TargetName);
			bool bTransitionEnabled = true;
			(*TransitionObjPtr)->TryGetBoolField(TEXT("enabled"), bTransitionEnabled);
			Transition.bTransitionEnabled = bTransitionEnabled;
			bool bDelay = false;
			(*TransitionObjPtr)->TryGetBoolField(TEXT("delay_enabled"), bDelay);
			Transition.bDelayTransition = bDelay;
			double DelayValue = 0.0;
			if ((*TransitionObjPtr)->TryGetNumberField(TEXT("delay_duration"), DelayValue))
			{
				Transition.DelayDuration = static_cast<float>(DelayValue);
			}
			if ((*TransitionObjPtr)->TryGetNumberField(TEXT("delay_random_variance"), DelayValue))
			{
				Transition.DelayRandomVariance = static_cast<float>(DelayValue);
			}

			const TArray<TSharedPtr<FJsonValue>>* TransitionConditions = nullptr;
			if ((*TransitionObjPtr)->TryGetArrayField(TEXT("conditions"), TransitionConditions) && TransitionConditions)
			{
				for (const TSharedPtr<FJsonValue>& ConditionValue : *TransitionConditions)
				{
					const TSharedPtr<FJsonObject>* ConditionObjPtr = nullptr;
					if (ConditionValue->TryGetObject(ConditionObjPtr))
					{
						FStateTreeEditorNode& Node = Transition.Conditions.AddDefaulted_GetRef();
						if (!UeAgentAIOps::ConfigureStateTreeEditorNode(Node, SourceState, *ConditionObjPtr, FStateTreeConditionBase::StaticStruct(), UStateTreeConditionBlueprintBase::StaticClass(), PropertyResults, OutError))
						{
							return false;
						}
					}
				}
			}
		}
	}
	TSharedPtr<FJsonObject> CompileReport;
	UeAgentAIOps::CompileStateTree(Tree, CompileReport);
	Tree->MarkPackageDirty();
	Tree->PostEditChange();
	bool bSave = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSave);
	if (bSave && !UeAgentAIOps::SaveAssetPackage(Tree, OutError))
	{
		return false;
	}
	OutData = UeAgentAIOps::ExportStateTree(Tree, true);
	OutData->SetObjectField(TEXT("compile_report"), CompileReport);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetBoolField(TEXT("created_asset"), bCreated);
	OutData->SetBoolField(TEXT("changed"), true);
	OutData->SetBoolField(TEXT("saved"), bSave);
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	UeAgentAIOps::AttachCoverageReport(OutData);
	return true;
#else
	OutError = TEXT("state_tree_editor_data_unavailable");
	return false;
#endif
}

bool FUeAgentHttpServer::CmdStateTreeOpenEditor(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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
		OutError = TEXT("state_tree_not_found");
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

bool FUeAgentHttpServer::CmdStateTreeScreenshot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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
		OutError = TEXT("state_tree_not_found");
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

bool FUeAgentHttpServer::CmdStateTreeRuntimeSnapshot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	FString ActorName;
	JsonTryGetString(Ctx.Params, TEXT("actor"), ActorName);
	UActorComponent* FoundComponent = nullptr;
	AActor* Actor = UeAgentAIOps::ResolveActorOrFirst(World, ActorName, UStateTreeComponent::StaticClass(), FoundComponent);
	UStateTreeComponent* Component = Cast<UStateTreeComponent>(FoundComponent);
	if (!Component)
	{
		OutError = TEXT("state_tree_component_not_found");
		return false;
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("actor"), Actor ? Actor->GetName() : FString());
	OutData->SetStringField(TEXT("component"), Component->GetName());
	OutData->SetBoolField(TEXT("is_running"), Component->IsRunning());
	OutData->SetBoolField(TEXT("is_paused"), Component->IsPaused());
	OutData->SetStringField(TEXT("run_status"), UeAgentAIOps::EnumToString(Component->GetStateTreeRunStatus()));
	OutData->SetArrayField(TEXT("active_states"), TArray<TSharedPtr<FJsonValue>>());
	OutData->SetArrayField(TEXT("running_tasks"), TArray<TSharedPtr<FJsonValue>>());
	OutData->SetArrayField(TEXT("queued_events"), TArray<TSharedPtr<FJsonValue>>());
	OutData->SetArrayField(TEXT("instance_data"), TArray<TSharedPtr<FJsonValue>>());
	OutData->SetStringField(TEXT("runtime_introspection"), TEXT("component_run_status_state_tree_ref"));
	FProperty* RefProperty = FindFProperty<FProperty>(Component->GetClass(), TEXT("StateTreeRef"));
	if (FStructProperty* StructProperty = CastField<FStructProperty>(RefProperty))
	{
		if (StructProperty->Struct == FStateTreeReference::StaticStruct())
		{
			const FStateTreeReference* Ref = StructProperty->ContainerPtrToValuePtr<FStateTreeReference>(Component);
			OutData->SetStringField(TEXT("state_tree"), (Ref && Ref->GetStateTree()) ? Ref->GetStateTree()->GetPathName() : FString());
		}
	}
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	return true;
}

bool FUeAgentHttpServer::CmdStateTreeNodeBlueprintGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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
	OutData->SetStringField(TEXT("asset_path"), Asset->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
	OutData->SetStringField(TEXT("class"), Asset->GetClass()->GetPathName());
	OutData->SetStringField(TEXT("generated_class"), GeneratedClass ? GeneratedClass->GetPathName() : FString());
	OutData->SetStringField(TEXT("parent_class"), (GeneratedClass && GeneratedClass->GetSuperClass()) ? GeneratedClass->GetSuperClass()->GetPathName() : FString());
	OutData->SetStringField(TEXT("introspection_level"), TEXT("class_hierarchy_only"));
	OutData->SetBoolField(TEXT("is_state_tree_task"), GeneratedClass && GeneratedClass->IsChildOf(UStateTreeTaskBlueprintBase::StaticClass()));
	OutData->SetBoolField(TEXT("is_state_tree_condition"), GeneratedClass && GeneratedClass->IsChildOf(UStateTreeConditionBlueprintBase::StaticClass()));
	OutData->SetBoolField(TEXT("is_state_tree_evaluator"), GeneratedClass && GeneratedClass->IsChildOf(UStateTreeEvaluatorBlueprintBase::StaticClass()));
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	return true;
}

bool FUeAgentHttpServer::CmdEqsCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UEnvQuery* Query = nullptr;
	if (!UeAgentAIOps::CreateAsset<UEnvQuery>(AssetPath, Query, OutError) || !Query)
	{
		return false;
	}
	FString Template;
	JsonTryGetString(Ctx.Params, TEXT("template"), Template);
	const bool bCreateDefaultOption = Template.IsEmpty() || Template.Equals(TEXT("simple_grid"), ESearchCase::IgnoreCase);
	if (bCreateDefaultOption)
	{
		UEnvQueryOption* Option = NewObject<UEnvQueryOption>(Query, UEnvQueryOption::StaticClass(), NAME_None, RF_Transactional);
		Option->Generator = NewObject<UEnvQueryGenerator_SimpleGrid>(Option, UEnvQueryGenerator_SimpleGrid::StaticClass(), NAME_None, RF_Transactional);
		Query->GetOptionsMutable().Add(Option);
	}
	Query->MarkPackageDirty();
	bool bSave = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_create"), bSave);
	if (bSave && !UeAgentAIOps::SaveAssetPackage(Query, OutError))
	{
		return false;
	}
	OutData = UeAgentAIOps::ExportEqs(Query, true);
	OutData->SetBoolField(TEXT("created_asset"), true);
	OutData->SetBoolField(TEXT("saved"), bSave);
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	return true;
}

bool FUeAgentHttpServer::CmdEqsGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UEnvQuery* Query = UeAgentAIOps::LoadAsset<UEnvQuery>(AssetPath);
	if (!Query)
	{
		OutError = TEXT("eqs_query_not_found");
		return false;
	}
	bool bIncludeOptions = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_tests"), bIncludeOptions);
	OutData = UeAgentAIOps::ExportEqs(Query, bIncludeOptions);
	return true;
}

bool FUeAgentHttpServer::CmdEqsExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UEnvQuery* Query = UeAgentAIOps::LoadAsset<UEnvQuery>(AssetPath);
	if (!Query)
	{
		OutError = TEXT("eqs_query_not_found");
		return false;
	}
	FString FolderInput;
	JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderInput);
	const FString FolderPath = UeAgentAIOps::ResolveFolderPath(FolderInput, UeAgentAIOps::EqsProfile, AssetPath);
	bool bClean = false;
	JsonTryGetBool(Ctx.Params, TEXT("clean_output_dir"), bClean);
	if (bClean && IFileManager::Get().DirectoryExists(*FolderPath))
	{
		IFileManager::Get().DeleteDirectory(*FolderPath, false, true);
	}
	IFileManager::Get().MakeDirectory(*FolderPath, true);
	TArray<TSharedPtr<FJsonValue>> WrittenFiles;
	if (!UeAgentAIOps::SaveEqsFolderContents(FolderPath, Query, WrittenFiles, OutError))
	{
		return false;
	}
	OutData = UeAgentAIOps::ExportEqs(Query, true);
	OutData->SetArrayField(TEXT("written_files"), WrittenFiles);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	UeAgentAIOps::AttachCoverageReport(OutData);
	return true;
}

bool FUeAgentHttpServer::CmdEqsValidateFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString FolderInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderInput) || FolderInput.IsEmpty())
	{
		OutError = TEXT("missing_folder_path");
		return false;
	}
	const FString FolderPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FolderInput);
	TSharedPtr<FJsonObject> AssetJson;
	TSharedPtr<FJsonObject> OptionsJson;
	if (!UeAgentAIOps::LoadFolderJson(FolderPath, TEXT("asset.json"), AssetJson, OutError) ||
		!UeAgentAIOps::LoadFolderJson(FolderPath, TEXT("options.json"), OptionsJson, OutError))
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	UeAgentAIOps::ValidateEqsFolderJson(AssetJson, OptionsJson, Issues);
	OutData->SetStringField(TEXT("profile"), UeAgentAIOps::EqsProfile);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetStringField(TEXT("status"), Issues.Num() == 0 ? TEXT("ok") : TEXT("validate_failed"));
	UeAgentAIOps::SetDiagnostics(OutData, Issues);
	return Issues.Num() == 0;
}

bool FUeAgentHttpServer::CmdEqsApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString FolderInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderInput) || FolderInput.IsEmpty())
	{
		OutError = TEXT("missing_folder_path");
		return false;
	}
	const FString FolderPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FolderInput);
	TSharedPtr<FJsonObject> AssetJson;
	TSharedPtr<FJsonObject> OptionsJson;
	if (!UeAgentAIOps::LoadFolderJson(FolderPath, TEXT("asset.json"), AssetJson, OutError) ||
		!UeAgentAIOps::LoadFolderJson(FolderPath, TEXT("options.json"), OptionsJson, OutError))
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	if (!UeAgentAIOps::ValidateEqsFolderJson(AssetJson, OptionsJson, Issues))
	{
		OutData->SetStringField(TEXT("status"), TEXT("validate_failed"));
		UeAgentAIOps::SetDiagnostics(OutData, Issues);
		return false;
	}
	bool bDryRun = false;
	bool bValidateOnly = false;
	JsonTryGetBool(Ctx.Params, TEXT("dry_run"), bDryRun);
	JsonTryGetBool(Ctx.Params, TEXT("validate_only"), bValidateOnly);
	if (bDryRun || bValidateOnly)
	{
		OutData->SetStringField(TEXT("profile"), UeAgentAIOps::EqsProfile);
		OutData->SetStringField(TEXT("folder_path"), FolderPath);
		OutData->SetStringField(TEXT("status"), TEXT("ok"));
		OutData->SetBoolField(TEXT("dry_run"), bDryRun);
		OutData->SetBoolField(TEXT("validate_only"), bValidateOnly);
		UeAgentAIOps::SetDiagnostics(OutData, Issues);
		return true;
	}
	FString AssetPath;
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		AssetJson->TryGetStringField(TEXT("asset_path"), AssetPath);
	}
	bool bCreateIfMissing = false;
	bool bAllowDestructive = false;
	JsonTryGetBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing);
	JsonTryGetBool(Ctx.Params, TEXT("allow_destructive"), bAllowDestructive);
	UEnvQuery* Query = UeAgentAIOps::LoadAsset<UEnvQuery>(AssetPath);
	bool bCreated = false;
	if (!Query)
	{
		if (!bCreateIfMissing)
		{
			OutError = TEXT("eqs_query_not_found");
			return false;
		}
		if (!UeAgentAIOps::CreateAsset<UEnvQuery>(AssetPath, Query, OutError) || !Query)
		{
			return false;
		}
		bCreated = true;
	}
	if (Query->GetOptions().Num() > 0 && !bCreated && !bAllowDestructive)
	{
		OutError = TEXT("eqs_rebuild_requires_allow_destructive");
		return false;
	}
	Query->Modify();
	Query->GetOptionsMutable().Reset();
	TArray<TSharedPtr<FJsonValue>> PropertyResults;
	const TArray<TSharedPtr<FJsonValue>>* Options = nullptr;
	OptionsJson->TryGetArrayField(TEXT("options"), Options);
	for (const TSharedPtr<FJsonValue>& OptionValue : *Options)
	{
		const TSharedPtr<FJsonObject>* OptionObjPtr = nullptr;
		if (!OptionValue->TryGetObject(OptionObjPtr))
		{
			continue;
		}
		UEnvQueryOption* Option = NewObject<UEnvQueryOption>(Query, UEnvQueryOption::StaticClass(), NAME_None, RF_Transactional);
		FString GeneratorClassPath;
		(*OptionObjPtr)->TryGetStringField(TEXT("generator_class"), GeneratorClassPath);
		UClass* GeneratorClass = GeneratorClassPath.IsEmpty() ? UEnvQueryGenerator_SimpleGrid::StaticClass() : UeAgentAIOps::ResolveClass(GeneratorClassPath);
		if (!GeneratorClass || !GeneratorClass->IsChildOf(UEnvQueryGenerator::StaticClass()))
		{
			OutError = TEXT("eqs_generator_class_not_found");
			return false;
		}
		Option->Generator = NewObject<UEnvQueryGenerator>(Option, GeneratorClass, NAME_None, RF_Transactional);
		const TArray<TSharedPtr<FJsonValue>>* GeneratorProperties = nullptr;
		if ((*OptionObjPtr)->TryGetArrayField(TEXT("generator_properties"), GeneratorProperties) && GeneratorProperties)
		{
			if (!UeAgentAIOps::ApplyProperties(Option->Generator, GeneratorProperties, PropertyResults, OutError))
			{
				return false;
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* Tests = nullptr;
		if ((*OptionObjPtr)->TryGetArrayField(TEXT("tests"), Tests) && Tests)
		{
			for (const TSharedPtr<FJsonValue>& TestValue : *Tests)
			{
				const TSharedPtr<FJsonObject>* TestObjPtr = nullptr;
				if (!TestValue->TryGetObject(TestObjPtr))
				{
					continue;
				}
				FString TestClassPath;
				(*TestObjPtr)->TryGetStringField(TEXT("class"), TestClassPath);
				UClass* TestClass = TestClassPath.IsEmpty() ? UEnvQueryTest_Distance::StaticClass() : UeAgentAIOps::ResolveClass(TestClassPath);
				if (!TestClass || !TestClass->IsChildOf(UEnvQueryTest::StaticClass()))
				{
					OutError = TEXT("eqs_test_class_not_found");
					return false;
				}
				UEnvQueryTest* Test = NewObject<UEnvQueryTest>(Option, TestClass, NAME_None, RF_Transactional);
				const TArray<TSharedPtr<FJsonValue>>* TestProperties = nullptr;
				if ((*TestObjPtr)->TryGetArrayField(TEXT("properties"), TestProperties) && TestProperties)
				{
					if (!UeAgentAIOps::ApplyProperties(Test, TestProperties, PropertyResults, OutError))
					{
						return false;
					}
				}
				Option->Tests.Add(Test);
			}
		}
		Query->GetOptionsMutable().Add(Option);
	}
	Query->MarkPackageDirty();
	Query->PostEditChange();
	bool bSave = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSave);
	if (bSave && !UeAgentAIOps::SaveAssetPackage(Query, OutError))
	{
		return false;
	}
	OutData = UeAgentAIOps::ExportEqs(Query, true);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetBoolField(TEXT("created_asset"), bCreated);
	OutData->SetBoolField(TEXT("changed"), true);
	OutData->SetBoolField(TEXT("saved"), bSave);
	OutData->SetArrayField(TEXT("property_results"), PropertyResults);
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	UeAgentAIOps::AttachCoverageReport(OutData);
	return true;
}

bool FUeAgentHttpServer::CmdEqsRunQuery(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UEnvQuery* Query = UeAgentAIOps::LoadAsset<UEnvQuery>(AssetPath);
	if (!Query)
	{
		OutError = TEXT("eqs_query_not_found");
		return false;
	}
	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	(void)NavSys;
	UEnvQueryManager* Manager = UEnvQueryManager::GetCurrent(World);
	if (!Manager)
	{
		OutError = TEXT("eqs_manager_not_found");
		return false;
	}
	FString QuerierName;
	JsonTryGetString(Ctx.Params, TEXT("querier"), QuerierName);
	UObject* Querier = QuerierName.IsEmpty() ? static_cast<UObject*>(World) : static_cast<UObject*>(FindActorByNameOrLabel(World, QuerierName));
	if (!Querier)
	{
		OutError = TEXT("querier_not_found");
		return false;
	}
	FString RunModeText;
	JsonTryGetString(Ctx.Params, TEXT("run_mode"), RunModeText);
	FEnvQueryRequest Request(Query, Querier);
	Request.SetWorldOverride(World);
	TSharedPtr<FEnvQueryResult> Result = Manager->RunInstantQuery(Request, UeAgentAIOps::ParseEqsRunMode(RunModeText));
	if (!Result.IsValid())
	{
		OutError = TEXT("eqs_run_failed");
		return false;
	}
	OutData = MakeShared<FJsonObject>();
	int32 MaxResults = 16;
	double MaxResultsNumber = MaxResults;
	JsonTryGetNumber(Ctx.Params, TEXT("max_results"), MaxResultsNumber);
	MaxResults = (int32)FMath::Clamp(MaxResultsNumber, 1.0, 1024.0);
	TArray<TSharedPtr<FJsonValue>> Items;
	const int32 EmitCount = FMath::Min(MaxResults, Result->Items.Num());
	for (int32 Index = 0; Index < EmitCount; ++Index)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetNumberField(TEXT("index"), Index);
		Item->SetNumberField(TEXT("score"), Result->GetItemScore(Index));
		Item->SetObjectField(TEXT("location"), VecToJson(Result->GetItemAsLocation(Index)));
		if (AActor* Actor = Result->GetItemAsActor(Index))
		{
			Item->SetStringField(TEXT("actor"), Actor->GetName());
			Item->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
		}
		Items.Add(MakeShared<FJsonValueObject>(Item));
	}
	OutData->SetStringField(TEXT("asset_path"), Query->GetOutermost()->GetName());
	OutData->SetBoolField(TEXT("successful"), Result->IsSuccessful());
	OutData->SetNumberField(TEXT("item_count"), Result->Items.Num());
	OutData->SetBoolField(TEXT("truncated"), Result->Items.Num() > EmitCount);
	OutData->SetArrayField(TEXT("items"), Items);
	OutData->SetStringField(TEXT("item_type"), Result->ItemType ? Result->ItemType->GetPathName() : FString());
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	return true;
}

bool FUeAgentHttpServer::CmdEqsDebugSnapshot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return CmdEqsRunQuery(Ctx, OutData, OutError);
}

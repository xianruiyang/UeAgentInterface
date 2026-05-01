
	static UNiagaraNodeInput* FindStageParameterMapInputNode(UNiagaraNodeOutput& OutputNode)
	{
		UEdGraphNode* CurrentNode = &OutputNode;
		TSet<UEdGraphNode*> VisitedNodes;

		while (CurrentNode && !VisitedNodes.Contains(CurrentNode))
		{
			VisitedNodes.Add(CurrentNode);

			UEdGraphPin* InputPin = FindParameterMapInputPin(*CurrentNode);
			if (!InputPin || InputPin->LinkedTo.Num() == 0)
			{
				return nullptr;
			}

			UEdGraphPin* LinkedPin = nullptr;
			for (UEdGraphPin* CandidatePin : InputPin->LinkedTo)
			{
				if (IsParameterMapPin(CandidatePin))
				{
					LinkedPin = CandidatePin;
					break;
				}
			}
			if (!LinkedPin)
			{
				return nullptr;
			}

			UEdGraphNode* PreviousNode = LinkedPin->GetOwningNode();
			if (UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(PreviousNode))
			{
				return InputNode;
			}

			CurrentNode = PreviousNode;
		}

		return nullptr;
	}

	static void GetLocalStackNodeGroups(UNiagaraNodeOutput& OutputNode, TArray<FLocalNiagaraStackNodeGroup>& OutStackNodeGroups)
	{
		OutStackNodeGroups.Reset();

		UNiagaraNodeInput* InputNode = FindStageParameterMapInputNode(OutputNode);
		if (!InputNode)
		{
			return;
		}

		FLocalNiagaraStackNodeGroup InputGroup;
		InputGroup.StartNodes.Add(InputNode);
		InputGroup.EndNode = InputNode;
		OutStackNodeGroups.Add(InputGroup);

		TArray<UNiagaraNodeFunctionCall*> ModuleNodes;
		GetStageModuleNodes(OutputNode, ModuleNodes);
		for (UNiagaraNodeFunctionCall* ModuleNode : ModuleNodes)
		{
			if (!ModuleNode)
			{
				continue;
			}

			FLocalNiagaraStackNodeGroup ModuleGroup;
			UEdGraphPin* PreviousOutputPin = FindParameterMapOutputPin(*OutStackNodeGroups.Last().EndNode);
			if (!PreviousOutputPin)
			{
				return;
			}
			for (UEdGraphPin* LinkedPin : PreviousOutputPin->LinkedTo)
			{
				if (LinkedPin)
				{
					if (UNiagaraNode* LinkedNode = Cast<UNiagaraNode>(LinkedPin->GetOwningNode()))
					{
						ModuleGroup.StartNodes.Add(LinkedNode);
					}
				}
			}
			ModuleGroup.EndNode = ModuleNode;
			OutStackNodeGroups.Add(ModuleGroup);
		}

		FLocalNiagaraStackNodeGroup OutputGroup;
		UEdGraphPin* PreviousOutputPin = FindParameterMapOutputPin(*OutStackNodeGroups.Last().EndNode);
		if (!PreviousOutputPin)
		{
			return;
		}
		for (UEdGraphPin* LinkedPin : PreviousOutputPin->LinkedTo)
		{
			if (LinkedPin)
			{
				if (UNiagaraNode* LinkedNode = Cast<UNiagaraNode>(LinkedPin->GetOwningNode()))
				{
					OutputGroup.StartNodes.Add(LinkedNode);
				}
			}
		}
		OutputGroup.EndNode = &OutputNode;
		OutStackNodeGroups.Add(OutputGroup);
	}

	static void DisconnectLocalStackNodeGroup(const FLocalNiagaraStackNodeGroup& DisconnectGroup, const FLocalNiagaraStackNodeGroup& PreviousGroup, const FLocalNiagaraStackNodeGroup& NextGroup)
	{
		UEdGraphPin* PreviousOutputPin = FindParameterMapOutputPin(*PreviousGroup.EndNode);
		UEdGraphPin* DisconnectOutputPin = FindParameterMapOutputPin(*DisconnectGroup.EndNode);
		if (!PreviousOutputPin || !DisconnectOutputPin)
		{
			return;
		}

		PreviousOutputPin->BreakAllPinLinks(true);
		DisconnectOutputPin->BreakAllPinLinks(true);

		for (UNiagaraNode* NextStartNode : NextGroup.StartNodes)
		{
			if (!NextStartNode)
			{
				continue;
			}
			if (UEdGraphPin* NextInputPin = FindParameterMapInputPin(*NextStartNode))
			{
				PreviousOutputPin->MakeLinkTo(NextInputPin);
			}
		}
	}

	static void ConnectLocalStackNodeGroup(const FLocalNiagaraStackNodeGroup& ConnectGroup, const FLocalNiagaraStackNodeGroup& NewPreviousGroup, const FLocalNiagaraStackNodeGroup& NewNextGroup)
	{
		UEdGraphPin* PreviousOutputPin = FindParameterMapOutputPin(*NewPreviousGroup.EndNode);
		UEdGraphPin* ConnectOutputPin = FindParameterMapOutputPin(*ConnectGroup.EndNode);
		if (!PreviousOutputPin || !ConnectOutputPin)
		{
			return;
		}

		PreviousOutputPin->BreakAllPinLinks(true);
		for (UNiagaraNode* ConnectStartNode : ConnectGroup.StartNodes)
		{
			if (!ConnectStartNode)
			{
				continue;
			}
			if (UEdGraphPin* ConnectInputPin = FindParameterMapInputPin(*ConnectStartNode))
			{
				PreviousOutputPin->MakeLinkTo(ConnectInputPin);
			}
		}

		for (UNiagaraNode* NextStartNode : NewNextGroup.StartNodes)
		{
			if (!NextStartNode)
			{
				continue;
			}
			if (UEdGraphPin* NextInputPin = FindParameterMapInputPin(*NextStartNode))
			{
				ConnectOutputPin->MakeLinkTo(NextInputPin);
			}
		}
	}

	static UNiagaraNodeOutput* EnsureStageInputOutputBridge(UNiagaraGraph& Graph, const ENiagaraScriptUsage Usage, const FGuid& UsageId)
	{
		Graph.Modify();
		NormalizeGraphNodeGuids(&Graph);

		UNiagaraNodeOutput* OutputNode = nullptr;
		TArray<UNiagaraNodeOutput*> ExistingOutputs;
		GetOutputNodesFromGraph(Graph, ExistingOutputs);
		for (UNiagaraNodeOutput* ExistingOutput : ExistingOutputs)
		{
			if (ExistingOutput && ExistingOutput->GetUsage() == Usage && ExistingOutput->GetUsageId() == UsageId)
			{
				OutputNode = ExistingOutput;
				break;
			}
		}

		UEdGraphPin* OutputNodeInputPin = OutputNode ? FindFirstInputPin(*OutputNode) : nullptr;
		if (OutputNode && !OutputNodeInputPin)
		{
			FUeAgentInterfaceLogger::Log(TEXT("Niagara EnsureStageBridge: remove invalid output node usage=%s usage_id=%s reason=no_input_pin"),
				*ScriptUsageToString(Usage),
				*UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
			Graph.RemoveNode(OutputNode);
			OutputNode = nullptr;
		}

		if (!OutputNode)
		{
			OutputNode = NewObject<UNiagaraNodeOutput>(&Graph, NAME_None, RF_Transactional);
			if (!OutputNode)
			{
				return nullptr;
			}
			OutputNode->CreateNewGuid();
			OutputNode->SetUsage(Usage);
			OutputNode->SetUsageId(UsageId);
			OutputNode->Outputs.Empty();
			OutputNode->Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("Out")));
			OutputNode->AllocateDefaultPins();
			Graph.AddNode(OutputNode, true, false);
			OutputNodeInputPin = FindFirstInputPin(*OutputNode);
		}
		else
		{
			OutputNode->Modify();
			OutputNodeInputPin = FindFirstInputPin(*OutputNode);
		}

		if (!OutputNodeInputPin)
		{
			FUeAgentInterfaceLogger::Log(TEXT("Niagara EnsureStageBridge: failed usage=%s usage_id=%s reason=output_input_pin_missing"),
				*ScriptUsageToString(Usage),
				*UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
			return nullptr;
		}

		if (OutputNodeInputPin->LinkedTo.Num() > 0 && FindStageParameterMapInputNode(*OutputNode) == nullptr)
		{
			FUeAgentInterfaceLogger::Log(TEXT("Niagara EnsureStageBridge: reset invalid stack usage=%s usage_id=%s reason=input_node_unreachable"),
				*ScriptUsageToString(Usage),
				*UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
			OutputNodeInputPin->BreakAllPinLinks();
		}

		if (OutputNodeInputPin->LinkedTo.Num() == 0)
		{
			UNiagaraNodeInput* InputNode = NewObject<UNiagaraNodeInput>(&Graph, NAME_None, RF_Transactional);
			if (!InputNode)
			{
				FUeAgentInterfaceLogger::Log(TEXT("Niagara EnsureStageBridge: failed usage=%s usage_id=%s reason=create_input_node_failed"),
					*ScriptUsageToString(Usage),
					*UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
				return nullptr;
			}

			InputNode->Input = FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("InputMap"));
			InputNode->Usage = ENiagaraInputNodeUsage::Parameter;
			InputNode->CreateNewGuid();
			InputNode->AllocateDefaultPins();
			Graph.AddNode(InputNode, true, false);

			UEdGraphPin* InputNodeOutputPin = FindFirstOutputPin(*InputNode);
			if (!InputNodeOutputPin)
			{
				FUeAgentInterfaceLogger::Log(TEXT("Niagara EnsureStageBridge: failed usage=%s usage_id=%s reason=input_output_pin_missing"),
					*ScriptUsageToString(Usage),
					*UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
				return nullptr;
			}

			bool bLinked = false;
			if (const UEdGraphSchema* Schema = Graph.GetSchema())
			{
				bLinked = Schema->TryCreateConnection(OutputNodeInputPin, InputNodeOutputPin);
			}
			if (!bLinked)
			{
				OutputNodeInputPin->MakeLinkTo(InputNodeOutputPin);
				bLinked = OutputNodeInputPin->LinkedTo.Contains(InputNodeOutputPin) || InputNodeOutputPin->LinkedTo.Contains(OutputNodeInputPin);
			}
			if (!bLinked)
			{
				FUeAgentInterfaceLogger::Log(TEXT("Niagara EnsureStageBridge: failed usage=%s usage_id=%s reason=link_failed"),
					*ScriptUsageToString(Usage),
					*UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
				return nullptr;
			}
		}

		return OutputNode;
	}

	static void GatherUpstreamNodesRecursive(UEdGraphNode* Node, TSet<UEdGraphNode*>& VisitedNodes, TArray<UEdGraphNode*>& OutNodes)
	{
		if (!Node || VisitedNodes.Contains(Node))
		{
			return;
		}

		VisitedNodes.Add(Node);
		OutNodes.Add(Node);

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Input)
			{
				continue;
			}

			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				if (LinkedPin)
				{
					GatherUpstreamNodesRecursive(LinkedPin->GetOwningNode(), VisitedNodes, OutNodes);
				}
			}
		}
	}

	static void GetStageModuleNodes(UNiagaraNodeOutput& OutputNode, TArray<UNiagaraNodeFunctionCall*>& OutModules)
	{
		OutModules.Reset();

		TSet<UEdGraphNode*> VisitedNodes;
		TArray<UEdGraphNode*> StageNodes;
		GatherUpstreamNodesRecursive(&OutputNode, VisitedNodes, StageNodes);

		TArray<UNiagaraNodeFunctionCall*> ModulesFromOutputToInput;
		for (UEdGraphNode* StageNode : StageNodes)
		{
			UNiagaraNodeFunctionCall* ModuleNode = Cast<UNiagaraNodeFunctionCall>(StageNode);
			if (!ModuleNode)
			{
				continue;
			}

			const ENiagaraScriptUsage CalledUsage = ModuleNode->GetCalledUsage();
			if (CalledUsage == ENiagaraScriptUsage::Module || CalledUsage == ENiagaraScriptUsage::Function)
			{
				ModulesFromOutputToInput.Add(ModuleNode);
			}
		}

		for (int32 Index = ModulesFromOutputToInput.Num() - 1; Index >= 0; --Index)
		{
			OutModules.Add(ModulesFromOutputToInput[Index]);
		}
	}

	static bool ResolveStageFromParams(UNiagaraGraph& Graph, const TSharedPtr<FJsonObject>& Params, FResolvedNiagaraStage& OutStage, FString& OutError)
	{
		if (!Params.IsValid())
		{
			OutError = TEXT("missing_params");
			return false;
		}

		ENiagaraScriptUsage TargetUsage = ENiagaraScriptUsage::Function;
		FGuid TargetUsageId;
		bool bHasUsage = false;
		bool bHasUsageId = false;

		FString StageKey;
		if (Params->TryGetStringField(TEXT("stage_key"), StageKey) && !StageKey.IsEmpty())
		{
			if (!TryParseStageKey(StageKey, TargetUsage, TargetUsageId))
			{
				OutError = TEXT("invalid_stage_key");
				return false;
			}
			bHasUsage = true;
			bHasUsageId = true;
		}
		else
		{
			FString UsageText;
			if (Params->TryGetStringField(TEXT("script_usage"), UsageText) && !UsageText.IsEmpty())
			{
				if (!TryParseScriptUsage(UsageText, TargetUsage))
				{
					OutError = TEXT("invalid_script_usage");
					return false;
				}
				bHasUsage = true;
			}

			FString UsageIdText;
			if (Params->TryGetStringField(TEXT("script_usage_id"), UsageIdText) && !UsageIdText.IsEmpty())
			{
				if (!FGuid::Parse(UsageIdText, TargetUsageId))
				{
					OutError = TEXT("invalid_script_usage_id");
					return false;
				}
				bHasUsageId = true;
			}
		}

		TArray<UNiagaraNodeOutput*> OutputNodes;
		GetOutputNodesFromGraph(Graph, OutputNodes);
		if (OutputNodes.Num() == 0)
		{
			OutError = TEXT("emitter_has_no_output_nodes");
			return false;
		}

		TArray<UNiagaraNodeOutput*> Matches;
		for (UNiagaraNodeOutput* OutputNode : OutputNodes)
		{
			if (!OutputNode)
			{
				continue;
			}
			if (bHasUsage && OutputNode->GetUsage() != TargetUsage)
			{
				continue;
			}
			if (bHasUsageId && OutputNode->GetUsageId() != TargetUsageId)
			{
				continue;
			}
			Matches.Add(OutputNode);
		}

		if (Matches.Num() == 0)
		{
			OutError = TEXT("stage_not_found");
			return false;
		}
		if (Matches.Num() > 1 && !bHasUsageId)
		{
			OutError = TEXT("stage_is_ambiguous_require_script_usage_id");
			return false;
		}

		OutStage.OutputNode = Matches[0];
		OutStage.Usage = Matches[0]->GetUsage();
		OutStage.UsageId = Matches[0]->GetUsageId();
		OutStage.StageKey = MakeStageKey(OutStage.Usage, OutStage.UsageId);
		return true;
	}

	static void GetModuleInputPins(UNiagaraNodeFunctionCall& ModuleNode, TArray<UEdGraphPin*>& OutInputPins)
	{
		OutInputPins.Reset();
		for (UEdGraphPin* Pin : ModuleNode.Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Input || Pin->ParentPin != nullptr)
			{
				continue;
			}

			const FNiagaraTypeDefinition PinType = UEdGraphSchema_Niagara::PinToTypeDefinition(Pin);
			if (PinType == FNiagaraTypeDefinition::GetParameterMapDef())
			{
				continue;
			}

			OutInputPins.Add(Pin);
		}
	}

	struct FResolvedNiagaraModuleInput
	{
		FNiagaraVariable InputVariable;
		FString FullInputName;
		FString ShortInputName;
		FGuid InputScriptVariableGuid;
		UEdGraphPin* VisiblePin = nullptr;
	};

	static FString MakeLowerTrimmed(const FString& InText)
	{
		FString OutText = InText.TrimStartAndEnd();
		OutText.ToLowerInline();
		return OutText;
	}

	static bool IsNiagaraParameterMapSetNode(const UEdGraphNode* Node)
	{
		for (const UClass* Class = Node ? Node->GetClass() : nullptr; Class; Class = Class->GetSuperClass())
		{
			if (Class->GetName().Equals(TEXT("NiagaraNodeParameterMapSet"), ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		return false;
	}

	static UEdGraphPin* FindOverridePinByAliasedInputName(UNiagaraGraph* Graph, const FName AliasedInputName)
	{
		if (!Graph || AliasedInputName.IsNone())
		{
			return nullptr;
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}

			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin)
				{
					continue;
				}
				if (Pin->Direction != EGPD_Input)
				{
					continue;
				}
				if (!IsNiagaraParameterMapSetNode(Pin->GetOwningNode()))
				{
					continue;
				}
				if (Pin->PinName == AliasedInputName)
				{
					return Pin;
				}
			}
		}

		return nullptr;
	}

	static void BuildResolvedModuleInputs(UNiagaraNodeFunctionCall& ModuleNode, TArray<FResolvedNiagaraModuleInput>& OutInputs)
	{
		OutInputs.Reset();

		TMap<FString, int32> IndexByLowerFullName;

		TArray<UEdGraphPin*> VisiblePins;
		GetModuleInputPins(ModuleNode, VisiblePins);
		for (UEdGraphPin* InputPin : VisiblePins)
		{
			if (!InputPin)
			{
				continue;
			}

			FResolvedNiagaraModuleInput InputData;
			InputData.FullInputName = InputPin->PinName.ToString();
			InputData.ShortInputName = InputData.FullInputName;
			int32 DotIndex = INDEX_NONE;
			if (InputData.ShortInputName.FindLastChar(TEXT('.'), DotIndex))
			{
				InputData.ShortInputName = InputData.ShortInputName.Mid(DotIndex + 1);
			}
			InputData.InputVariable = UEdGraphSchema_Niagara::PinToNiagaraVariable(InputPin);
			if (!InputData.InputVariable.IsValid())
			{
				InputData.InputVariable = FNiagaraVariable(UEdGraphSchema_Niagara::PinToTypeDefinition(InputPin), InputPin->PinName);
			}
			InputData.VisiblePin = InputPin;

			const int32 NewIndex = OutInputs.Add(MoveTemp(InputData));
			IndexByLowerFullName.Add(MakeLowerTrimmed(OutInputs[NewIndex].FullInputName), NewIndex);
		}

		TArray<FNiagaraVariable> StackInputVariables;
		TSet<FNiagaraVariable> HiddenInputVariables;
		FCompileConstantResolver ConstantResolver;
		FNiagaraStackGraphUtilities::GetStackFunctionInputs(
			ModuleNode,
			StackInputVariables,
			HiddenInputVariables,
			ConstantResolver,
			FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly,
			false);

		for (const FNiagaraVariable& InputVariable : StackInputVariables)
		{
			if (!InputVariable.IsValid())
			{
				continue;
			}

			const FString FullInputName = InputVariable.GetName().ToString();
			const FString LowerFullInputName = MakeLowerTrimmed(FullInputName);
			int32* ExistingIndex = IndexByLowerFullName.Find(LowerFullInputName);
			if (ExistingIndex)
			{
				FResolvedNiagaraModuleInput& ExistingInput = OutInputs[*ExistingIndex];
				if (!ExistingInput.InputVariable.IsValid())
				{
					ExistingInput.InputVariable = InputVariable;
				}
				continue;
			}

			FResolvedNiagaraModuleInput InputData;
			InputData.InputVariable = InputVariable;
			InputData.FullInputName = FullInputName;
			InputData.ShortInputName = FullInputName;
			int32 DotIndex = INDEX_NONE;
			if (InputData.ShortInputName.FindLastChar(TEXT('.'), DotIndex))
			{
				InputData.ShortInputName = InputData.ShortInputName.Mid(DotIndex + 1);
			}

			const int32 NewIndex = OutInputs.Add(MoveTemp(InputData));
			IndexByLowerFullName.Add(LowerFullInputName, NewIndex);
		}
	}

	static const FResolvedNiagaraModuleInput* ResolveModuleInputByName(const TArray<FResolvedNiagaraModuleInput>& ModuleInputs, const FString& InputName, FString& OutResolvedInputName)
	{
		const FString InputNameNormalized = InputName.TrimStartAndEnd();
		if (InputNameNormalized.IsEmpty())
		{
			return nullptr;
		}

		for (const FResolvedNiagaraModuleInput& InputData : ModuleInputs)
		{
			if (InputData.FullInputName.Equals(InputNameNormalized, ESearchCase::IgnoreCase))
			{
				OutResolvedInputName = InputData.FullInputName;
				return &InputData;
			}
		}

		const FResolvedNiagaraModuleInput* ShortMatch = nullptr;
		for (const FResolvedNiagaraModuleInput& InputData : ModuleInputs)
		{
			if (!InputData.ShortInputName.Equals(InputNameNormalized, ESearchCase::IgnoreCase))
			{
				continue;
			}
			if (ShortMatch != nullptr)
			{
				return nullptr;
			}
			ShortMatch = &InputData;
		}

		if (ShortMatch)
		{
			OutResolvedInputName = ShortMatch->FullInputName;
		}
		return ShortMatch;
	}

	static bool InitializeDynamicInputStackInputs(
		UNiagaraEmitter* Emitter,
		const FGuid& VersionGuid,
		UNiagaraGraph* Graph,
		UNiagaraNodeFunctionCall& ModuleNode,
		UNiagaraNodeFunctionCall& DynamicInputNode,
		FString& OutInitializationSource)
	{
		OutInitializationSource.Reset();
		if (!Graph)
		{
			OutInitializationSource = TEXT("invalid_graph_context");
			return false;
		}

		TArray<FResolvedNiagaraModuleInput> DynamicInputInputs;
		BuildResolvedModuleInputs(DynamicInputNode, DynamicInputInputs);

		int32 DataInterfaceInputCount = 0;
		int32 InitializedInputCount = 0;
		TArray<FString> Errors;
		for (const FResolvedNiagaraModuleInput& DynamicInput : DynamicInputInputs)
		{
			if (!DynamicInput.InputVariable.IsValid())
			{
				continue;
			}

			const FNiagaraTypeDefinition InputType = DynamicInput.InputVariable.GetType();
			UClass* InputClass = InputType.GetClass();
			if (!InputType.IsDataInterface() || !InputClass || !InputClass->IsChildOf(UNiagaraDataInterface::StaticClass()))
			{
				continue;
			}

			++DataInterfaceInputCount;
			const FNiagaraParameterHandle InputHandle(DynamicInput.InputVariable.GetName());
			const FNiagaraParameterHandle AliasedInputHandle = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(InputHandle, &DynamicInputNode);

			UEdGraphPin* DynamicInputParameterMapPin = FindParameterMapInputPin(DynamicInputNode);
			if (!DynamicInputParameterMapPin || DynamicInputParameterMapPin->LinkedTo.Num() != 1)
			{
				Errors.Add(FString::Printf(
					TEXT("%s:dynamic_input_parameter_map_input_link_count:%d"),
					*DynamicInput.FullInputName,
					DynamicInputParameterMapPin ? DynamicInputParameterMapPin->LinkedTo.Num() : 0));
				continue;
			}

			UEdGraphPin* OverridePin = &FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
				DynamicInputNode,
				AliasedInputHandle,
				InputType,
				DynamicInput.InputScriptVariableGuid,
				FGuid());
			if (!IsNiagaraParameterMapSetNode(OverridePin->GetOwningNode()))
			{
				Errors.Add(FString::Printf(TEXT("%s:override_pin_owner_not_parameter_map_set"), *DynamicInput.FullInputName));
				continue;
			}

			if (OverridePin->LinkedTo.Num() > 0)
			{
				++InitializedInputCount;
				continue;
			}

			const FString InputNodeInputName = FString::Printf(TEXT("%s.%s"), *DynamicInputNode.GetFunctionName(), *DynamicInput.ShortInputName);
			UNiagaraDataInterface* DataInterface = nullptr;
			FNiagaraStackGraphUtilities::SetDataInterfaceValueForFunctionInput(
				*OverridePin,
				InputClass,
				InputNodeInputName,
				DataInterface);
			if (DataInterface)
			{
				++InitializedInputCount;
			}
			else
			{
				Errors.Add(FString::Printf(TEXT("%s:data_interface_create_failed"), *DynamicInput.FullInputName));
			}
		}

		if (Graph)
		{
			Graph->NotifyGraphChanged();
		}

		OutInitializationSource = FString::Printf(
			TEXT("data_interface_inputs:%d;initialized:%d"),
			DataInterfaceInputCount,
			InitializedInputCount);
		if (Errors.Num() > 0)
		{
			OutInitializationSource += FString::Printf(TEXT(";errors:%s"), *FString::Join(Errors, TEXT("|")));
		}
		return DataInterfaceInputCount == InitializedInputCount;
	}

	static UNiagaraNodeFunctionCall* FindLinkedDynamicInputNode(const UEdGraphPin* OverridePin)
	{
		if (!OverridePin)
		{
			return nullptr;
		}

		for (UEdGraphPin* LinkedPin : OverridePin->LinkedTo)
		{
			if (!LinkedPin)
			{
				continue;
			}
			if (UNiagaraNodeFunctionCall* LinkedFunctionCall = Cast<UNiagaraNodeFunctionCall>(LinkedPin->GetOwningNode()))
			{
				return LinkedFunctionCall;
			}
		}
		return nullptr;
	}

	static bool ResolveDataInterfaceFromInputNode(UNiagaraNodeInput* InputNode, UNiagaraDataInterface*& OutDataInterface)
	{
		OutDataInterface = nullptr;
		if (!InputNode)
		{
			return false;
		}

		FProperty* DataInterfaceProperty = nullptr;
		void* DataInterfaceValuePtr = nullptr;
		if (!ResolvePropertyPath(InputNode, TEXT("DataInterface"), DataInterfaceProperty, DataInterfaceValuePtr))
		{
			return false;
		}

		FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(DataInterfaceProperty);
		OutDataInterface = ObjectProperty
			? Cast<UNiagaraDataInterface>(ObjectProperty->GetObjectPropertyValue(DataInterfaceValuePtr))
			: nullptr;
		return OutDataInterface != nullptr;
	}

	static TArray<TSharedPtr<FJsonValue>> BuildObjectPropertiesJsonForUObject(UObject* Object)
	{
		TArray<TSharedPtr<FJsonValue>> PropertiesJson;
		if (!Object)
		{
			return PropertiesJson;
		}

		for (TFieldIterator<FProperty> It(Object->GetClass(), EFieldIterationFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			if (!Property)
			{
				continue;
			}
			void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
			PropertiesJson.Add(MakeShared<FJsonValueObject>(BuildPropertyValueJson(*Object, *Property, ValuePtr)));
		}
		return PropertiesJson;
	}

	static TSharedPtr<FJsonObject> BuildDataInterfaceNodeJson(UNiagaraNodeInput& InputNode)
	{
		UNiagaraDataInterface* DataInterface = nullptr;
		if (!ResolveDataInterfaceFromInputNode(&InputNode, DataInterface) || !DataInterface)
		{
			return nullptr;
		}

		TArray<TSharedPtr<FJsonValue>> RawProperties = BuildObjectPropertiesJsonForUObject(DataInterface);
		TSharedPtr<FJsonObject> DataInterfaceObj = MakeShared<FJsonObject>();
		DataInterfaceObj->SetStringField(TEXT("node_guid"), InputNode.NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		DataInterfaceObj->SetStringField(TEXT("input_name"), InputNode.Input.GetName().ToString());
		DataInterfaceObj->SetStringField(TEXT("input_type"), InputNode.Input.GetType().GetNameText().ToString());
		DataInterfaceObj->SetStringField(TEXT("data_interface_class"), DataInterface->GetClass()->GetPathName());
		DataInterfaceObj->SetStringField(TEXT("data_interface_object_path"), DataInterface->GetPathName());
		DataInterfaceObj->SetArrayField(TEXT("raw_properties"), RawProperties);
		DataInterfaceObj->SetNumberField(TEXT("raw_properties_count"), RawProperties.Num());
		return DataInterfaceObj;
	}

	static void CollectLinkedDataInterfaceInputNodesForDynamicInput(UNiagaraNodeFunctionCall& DynamicInputNode, TArray<UNiagaraNodeInput*>& OutInputNodes)
	{
		OutInputNodes.Reset();
		TSet<UNiagaraNodeInput*> SeenInputNodes;

		auto AddInputNodeIfDataInterface = [&SeenInputNodes, &OutInputNodes](UNiagaraNodeInput* InputNode)
		{
			UNiagaraDataInterface* DataInterface = nullptr;
			if (!InputNode || SeenInputNodes.Contains(InputNode) || !ResolveDataInterfaceFromInputNode(InputNode, DataInterface))
			{
				return;
			}
			SeenInputNodes.Add(InputNode);
			OutInputNodes.Add(InputNode);
		};

		for (UEdGraphPin* Pin : DynamicInputNode.Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Input || IsParameterMapPin(Pin))
			{
				continue;
			}

			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				UNiagaraNodeInput* InputNode = LinkedPin ? Cast<UNiagaraNodeInput>(LinkedPin->GetOwningNode()) : nullptr;
				AddInputNodeIfDataInterface(InputNode);
			}
		}

		UNiagaraGraph* Graph = Cast<UNiagaraGraph>(DynamicInputNode.GetGraph());
		const FString DynamicInputFunctionName = DynamicInputNode.GetFunctionName();
		if (!Graph || DynamicInputFunctionName.IsEmpty())
		{
			return;
		}

		const FString DynamicInputPrefix = DynamicInputFunctionName + TEXT(".");
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(Node);
			if (!InputNode)
			{
				continue;
			}

			const FString InputName = InputNode->Input.GetName().ToString();
			if (InputName.StartsWith(DynamicInputPrefix, ESearchCase::CaseSensitive))
			{
				AddInputNodeIfDataInterface(InputNode);
			}
		}
	}

	static TSharedPtr<FJsonObject> BuildModuleInputJson(UNiagaraNodeFunctionCall& ModuleNode, const FResolvedNiagaraModuleInput& InputData, const int32 RecursionDepth = 0)
	{
		TSharedPtr<FJsonObject> InputObj = MakeShared<FJsonObject>();
		const FNiagaraTypeDefinition InputType = InputData.InputVariable.GetType();
		bool bHasLinks = false;
		bool bHasDefaultOverride = false;
		FString OverrideDefaultValue;
		FString AutogeneratedDefaultValue;
		UEdGraphPin* EffectiveInputPin = nullptr;

		if (InputData.VisiblePin)
		{
			EffectiveInputPin = InputData.VisiblePin;
			bHasLinks = InputData.VisiblePin->LinkedTo.Num() > 0;
			bHasDefaultOverride = InputData.VisiblePin->DefaultValue != InputData.VisiblePin->AutogeneratedDefaultValue;
			OverrideDefaultValue = InputData.VisiblePin->DefaultValue;
			AutogeneratedDefaultValue = InputData.VisiblePin->AutogeneratedDefaultValue;
		}
		else if (InputData.InputVariable.IsValid())
		{
			const FNiagaraParameterHandle InputHandle(InputData.InputVariable.GetName());
			const FNiagaraParameterHandle AliasedHandle = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(InputHandle, &ModuleNode);
			UNiagaraGraph* Graph = Cast<UNiagaraGraph>(ModuleNode.GetGraph());
			if (UEdGraphPin* OverridePin = FindOverridePinByAliasedInputName(Graph, AliasedHandle.GetParameterHandleString()))
			{
				EffectiveInputPin = OverridePin;
				bHasLinks = OverridePin->LinkedTo.Num() > 0;
				bHasDefaultOverride = !OverridePin->DefaultValue.IsEmpty() ||
					OverridePin->DefaultValue != OverridePin->AutogeneratedDefaultValue;
				OverrideDefaultValue = OverridePin->DefaultValue;
				AutogeneratedDefaultValue = OverridePin->AutogeneratedDefaultValue;
			}
		}

		InputObj->SetStringField(TEXT("input_name"), InputData.FullInputName);
		InputObj->SetStringField(TEXT("input_short_name"), InputData.ShortInputName);
		InputObj->SetStringField(TEXT("input_type"), InputType.GetNameText().ToString());
		InputObj->SetBoolField(TEXT("has_override"), bHasLinks || bHasDefaultOverride);
		InputObj->SetBoolField(TEXT("has_links"), bHasLinks);
		InputObj->SetStringField(TEXT("override_default_value"), OverrideDefaultValue);
		InputObj->SetStringField(TEXT("autogenerated_default_value"), AutogeneratedDefaultValue);
		InputObj->SetBoolField(TEXT("has_visible_pin"), InputData.VisiblePin != nullptr);
		if (InputType.IsEnum())
		{
			if (UEnum* EnumType = InputType.GetEnum())
			{
				InputObj->SetStringField(TEXT("enum_type"), EnumType->GetPathName());

				auto AppendEnumValueFields = [EnumType](const TSharedPtr<FJsonObject>& TargetObj, const FString& Prefix, const FString& ValueText) -> bool
				{
					int64 EnumValue = 0;
					if (ValueText.TrimStartAndEnd().IsEmpty() || !TryResolveNiagaraEnumValueFromText(*EnumType, ValueText, EnumValue))
					{
						return false;
					}

					TargetObj->SetStringField(Prefix + TEXT("enum_value_name"), EnumType->GetNameStringByValue(EnumValue));
					TargetObj->SetStringField(Prefix + TEXT("enum_value_display_name"), EnumType->GetDisplayNameTextByValue(EnumValue).ToString());
					TargetObj->SetNumberField(Prefix + TEXT("enum_value_int"), static_cast<double>(EnumValue));
					return true;
				};

				const bool bWroteOverrideEnumFields = AppendEnumValueFields(InputObj, TEXT("override_"), OverrideDefaultValue);
				AppendEnumValueFields(InputObj, TEXT("autogenerated_"), AutogeneratedDefaultValue);
				if (bWroteOverrideEnumFields)
				{
					int64 EnumValue = 0;
					if (TryResolveNiagaraEnumValueFromText(*EnumType, OverrideDefaultValue, EnumValue))
					{
						InputObj->SetStringField(TEXT("enum_value_name"), EnumType->GetNameStringByValue(EnumValue));
						InputObj->SetStringField(TEXT("enum_value_display_name"), EnumType->GetDisplayNameTextByValue(EnumValue).ToString());
						InputObj->SetNumberField(TEXT("enum_value_int"), static_cast<double>(EnumValue));
					}
				}
				else
				{
					int64 EnumValue = 0;
					if (TryResolveNiagaraEnumValueFromText(*EnumType, AutogeneratedDefaultValue, EnumValue))
					{
						InputObj->SetStringField(TEXT("enum_value_name"), EnumType->GetNameStringByValue(EnumValue));
						InputObj->SetStringField(TEXT("enum_value_display_name"), EnumType->GetDisplayNameTextByValue(EnumValue).ToString());
						InputObj->SetNumberField(TEXT("enum_value_int"), static_cast<double>(EnumValue));
					}
				}

				TArray<TSharedPtr<FJsonValue>> OptionsJson;
				for (int32 EnumIndex = 0; EnumIndex < EnumType->NumEnums(); ++EnumIndex)
				{
					if (EnumType->HasMetaData(TEXT("Hidden"), EnumIndex))
					{
						continue;
					}

					const int64 EnumValue = EnumType->GetValueByIndex(EnumIndex);
					TSharedPtr<FJsonObject> OptionObj = MakeShared<FJsonObject>();
					OptionObj->SetNumberField(TEXT("index"), EnumIndex);
					OptionObj->SetNumberField(TEXT("value"), static_cast<double>(EnumValue));
					OptionObj->SetStringField(TEXT("name"), EnumType->GetNameStringByIndex(EnumIndex));
					OptionObj->SetStringField(TEXT("display_name"), EnumType->GetDisplayNameTextByIndex(EnumIndex).ToString());
					OptionsJson.Add(MakeShared<FJsonValueObject>(OptionObj));
				}
				InputObj->SetArrayField(TEXT("enum_options"), OptionsJson);
			}
		}

		if (bHasLinks && EffectiveInputPin && RecursionDepth < 4)
		{
			if (UNiagaraNodeFunctionCall* DynamicInputNode = FindLinkedDynamicInputNode(EffectiveInputPin))
			{
				TSharedPtr<FJsonObject> DynamicInputObj = MakeShared<FJsonObject>();
				DynamicInputObj->SetStringField(TEXT("node_guid"), DynamicInputNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
				DynamicInputObj->SetStringField(TEXT("node_name"), DynamicInputNode->GetFunctionName());
				DynamicInputObj->SetStringField(TEXT("script_asset_path"), GetModuleScriptPath(*DynamicInputNode));

				TArray<FResolvedNiagaraModuleInput> DynamicInputs;
				BuildResolvedModuleInputs(*DynamicInputNode, DynamicInputs);
				TArray<TSharedPtr<FJsonValue>> DynamicInputsJson;
				DynamicInputsJson.Reserve(DynamicInputs.Num());
				for (const FResolvedNiagaraModuleInput& DynamicInput : DynamicInputs)
				{
					DynamicInputsJson.Add(MakeShared<FJsonValueObject>(BuildModuleInputJson(*DynamicInputNode, DynamicInput, RecursionDepth + 1)));
				}
				DynamicInputObj->SetArrayField(TEXT("inputs"), DynamicInputsJson);
				DynamicInputObj->SetNumberField(TEXT("input_count"), DynamicInputsJson.Num());

				TArray<UNiagaraNodeInput*> DataInterfaceInputNodes;
				CollectLinkedDataInterfaceInputNodesForDynamicInput(*DynamicInputNode, DataInterfaceInputNodes);
				TArray<TSharedPtr<FJsonValue>> DataInterfacesJson;
				DataInterfacesJson.Reserve(DataInterfaceInputNodes.Num());
				for (UNiagaraNodeInput* DataInterfaceInputNode : DataInterfaceInputNodes)
				{
					TSharedPtr<FJsonObject> DataInterfaceObj = DataInterfaceInputNode ? BuildDataInterfaceNodeJson(*DataInterfaceInputNode) : nullptr;
					if (DataInterfaceObj.IsValid())
					{
						DataInterfacesJson.Add(MakeShared<FJsonValueObject>(DataInterfaceObj));
					}
				}
				DynamicInputObj->SetArrayField(TEXT("data_interfaces"), DataInterfacesJson);
				DynamicInputObj->SetNumberField(TEXT("data_interface_count"), DataInterfacesJson.Num());

				InputObj->SetStringField(TEXT("link_kind"), TEXT("dynamic_input"));
				InputObj->SetObjectField(TEXT("dynamic_input"), DynamicInputObj);
			}
		}
		return InputObj;
	}

	static TSharedPtr<FJsonObject> BuildModuleNodeJson(UNiagaraNodeFunctionCall& ModuleNode, const int32 ModuleIndex, const bool bIncludeInputs)
	{
		TSharedPtr<FJsonObject> ModuleObj = MakeShared<FJsonObject>();
		ModuleObj->SetNumberField(TEXT("module_index"), ModuleIndex);
		ModuleObj->SetStringField(TEXT("module_node_guid"), ModuleNode.NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		ModuleObj->SetStringField(TEXT("module_name"), ModuleNode.GetFunctionName());
		ModuleObj->SetStringField(TEXT("module_script_path"), GetModuleScriptPath(ModuleNode));
		ModuleObj->SetStringField(TEXT("module_kind"), Cast<UNiagaraNodeAssignment>(&ModuleNode) ? TEXT("assignment") : TEXT("script_module"));
		ModuleObj->SetBoolField(TEXT("module_has_enabled_state"), true);
		ModuleObj->SetBoolField(TEXT("module_enabled"), ModuleNode.IsNodeEnabled());

		if (bIncludeInputs)
		{
			TArray<FResolvedNiagaraModuleInput> Inputs;
			BuildResolvedModuleInputs(ModuleNode, Inputs);
			TArray<TSharedPtr<FJsonValue>> InputsJson;
			InputsJson.Reserve(Inputs.Num());
			for (const FResolvedNiagaraModuleInput& InputData : Inputs)
			{
				InputsJson.Add(MakeShared<FJsonValueObject>(BuildModuleInputJson(ModuleNode, InputData)));
			}
			ModuleObj->SetArrayField(TEXT("inputs"), InputsJson);
			ModuleObj->SetNumberField(TEXT("input_count"), InputsJson.Num());
		}

		return ModuleObj;
	}

	static TSharedPtr<FJsonObject> BuildStageNodeJson(UEdGraphNode& StageNode, const int32 StageNodeIndex, const bool bIncludeProperties, const bool bIncludeModuleInputs)
	{
		TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
		NodeObj->SetNumberField(TEXT("stage_node_index"), StageNodeIndex);
		NodeObj->SetStringField(TEXT("node_guid"), StageNode.NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		NodeObj->SetStringField(TEXT("node_class"), StageNode.GetClass()->GetPathName());
		NodeObj->SetStringField(TEXT("node_title"), StageNode.GetNodeTitle(ENodeTitleType::ListView).ToString());
		NodeObj->SetNumberField(TEXT("node_pos_x"), StageNode.NodePosX);
		NodeObj->SetNumberField(TEXT("node_pos_y"), StageNode.NodePosY);
		NodeObj->SetBoolField(TEXT("is_output_node"), Cast<UNiagaraNodeOutput>(&StageNode) != nullptr);
		NodeObj->SetBoolField(TEXT("is_module_node"), Cast<UNiagaraNodeFunctionCall>(&StageNode) != nullptr);

		if (UNiagaraNodeFunctionCall* ModuleNode = Cast<UNiagaraNodeFunctionCall>(&StageNode))
		{
			NodeObj->SetStringField(TEXT("module_name"), ModuleNode->GetFunctionName());
			NodeObj->SetStringField(TEXT("module_script_path"), GetModuleScriptPath(*ModuleNode));
			NodeObj->SetStringField(TEXT("module_kind"), Cast<UNiagaraNodeAssignment>(ModuleNode) ? TEXT("assignment") : TEXT("script_module"));
			NodeObj->SetBoolField(TEXT("module_has_enabled_state"), true);
			NodeObj->SetBoolField(TEXT("module_enabled"), ModuleNode->IsNodeEnabled());

			if (bIncludeModuleInputs)
			{
				TArray<FResolvedNiagaraModuleInput> Inputs;
				BuildResolvedModuleInputs(*ModuleNode, Inputs);

				TArray<TSharedPtr<FJsonValue>> InputsJson;
				InputsJson.Reserve(Inputs.Num());
				for (const FResolvedNiagaraModuleInput& InputData : Inputs)
				{
					InputsJson.Add(MakeShared<FJsonValueObject>(BuildModuleInputJson(*ModuleNode, InputData)));
				}
				NodeObj->SetArrayField(TEXT("inputs"), InputsJson);
				NodeObj->SetNumberField(TEXT("input_count"), InputsJson.Num());
			}
		}

		if (bIncludeProperties)
		{
			TArray<TSharedPtr<FJsonValue>> PropertiesJson;
			for (TFieldIterator<FProperty> It(StageNode.GetClass(), EFieldIterationFlags::IncludeSuper); It; ++It)
			{
				FProperty* Property = *It;
				if (!Property)
				{
					continue;
				}

				void* ValuePtr = Property->ContainerPtrToValuePtr<void>(&StageNode);
				PropertiesJson.Add(MakeShared<FJsonValueObject>(BuildPropertyValueJson(StageNode, *Property, ValuePtr)));
			}
			NodeObj->SetArrayField(TEXT("properties"), PropertiesJson);
			NodeObj->SetNumberField(TEXT("property_count"), PropertiesJson.Num());
		}

		return NodeObj;
	}

	static TSharedPtr<FJsonObject> BuildRendererJson(UNiagaraRendererProperties& Renderer, const int32 RendererIndex)
	{
		TSharedPtr<FJsonObject> RendererObj = MakeShared<FJsonObject>();
		RendererObj->SetNumberField(TEXT("renderer_index"), RendererIndex);
		RendererObj->SetStringField(TEXT("renderer_name"), Renderer.GetFName().ToString());
		RendererObj->SetStringField(TEXT("renderer_class"), Renderer.GetClass()->GetPathName());
		RendererObj->SetStringField(TEXT("renderer_object_path"), Renderer.GetPathName());
		return RendererObj;
	}

	static UClass* ResolveRendererClass(const FString& RendererClassOrAlias)
	{
		FString ClassText = RendererClassOrAlias.TrimStartAndEnd();
		if (ClassText.IsEmpty())
		{
			return nullptr;
		}

		const FString AliasLower = ClassText.ToLower();
		if (AliasLower == TEXT("sprite"))
		{
			ClassText = TEXT("/Script/Niagara.NiagaraSpriteRendererProperties");
		}
		else if (AliasLower == TEXT("ribbon"))
		{
			ClassText = TEXT("/Script/Niagara.NiagaraRibbonRendererProperties");
		}
		else if (AliasLower == TEXT("mesh"))
		{
			ClassText = TEXT("/Script/Niagara.NiagaraMeshRendererProperties");
		}
		else if (AliasLower == TEXT("light"))
		{
			ClassText = TEXT("/Script/Niagara.NiagaraLightRendererProperties");
		}
		else if (AliasLower == TEXT("decal"))
		{
			ClassText = TEXT("/Script/Niagara.NiagaraDecalRendererProperties");
		}

		UClass* RendererClass = ResolveClassByPath(ClassText);
		if (!RendererClass || !RendererClass->IsChildOf(UNiagaraRendererProperties::StaticClass()) || RendererClass->HasAnyClassFlags(CLASS_Abstract))
		{
			return nullptr;
		}

		return RendererClass;
	}

	static bool ResolveRendererFromParams(FNiagaraEmitterEditContext& EditContext, const TSharedPtr<FJsonObject>& Params, int32& OutRendererIndex, UNiagaraRendererProperties*& OutRenderer, FString& OutError)
	{
		OutRendererIndex = INDEX_NONE;
		OutRenderer = nullptr;

		if (!EditContext.EmitterData)
		{
			OutError = TEXT("missing_emitter_version_data");
			return false;
		}

		const TArray<UNiagaraRendererProperties*>& Renderers = EditContext.EmitterData->GetRenderers();
		if (Renderers.Num() == 0)
		{
			OutError = TEXT("emitter_has_no_renderers");
			return false;
		}

		double RendererIndexNumber = 0.0;
		if (Params.IsValid() && Params->TryGetNumberField(TEXT("renderer_index"), RendererIndexNumber))
		{
			const int32 Index = FMath::RoundToInt(RendererIndexNumber);
			if (!Renderers.IsValidIndex(Index))
			{
				OutError = TEXT("invalid_renderer_index");
				return false;
			}

			OutRendererIndex = Index;
			OutRenderer = Renderers[Index];
			return OutRenderer != nullptr;
		}

		FString RendererName;
		if (Params.IsValid() && Params->TryGetStringField(TEXT("renderer_name"), RendererName) && !RendererName.IsEmpty())
		{
			for (int32 Index = 0; Index < Renderers.Num(); ++Index)
			{
				UNiagaraRendererProperties* Renderer = Renderers[Index];
				if (Renderer && Renderer->GetFName().ToString().Equals(RendererName, ESearchCase::IgnoreCase))
				{
					OutRendererIndex = Index;
					OutRenderer = Renderer;
					return true;
				}
			}
		}

		FString RendererClass;
		if (Params.IsValid() && Params->TryGetStringField(TEXT("renderer_class"), RendererClass) && !RendererClass.IsEmpty())
		{
			UClass* ResolvedClass = ResolveRendererClass(RendererClass);
			for (int32 Index = 0; Index < Renderers.Num(); ++Index)
			{
				UNiagaraRendererProperties* Renderer = Renderers[Index];
				if (!Renderer)
				{
					continue;
				}
				if (ResolvedClass)
				{
					if (Renderer->GetClass() == ResolvedClass)
					{
						OutRendererIndex = Index;
						OutRenderer = Renderer;
						return true;
					}
				}
				else if (Renderer->GetClass()->GetPathName().Equals(RendererClass, ESearchCase::IgnoreCase))
				{
					OutRendererIndex = Index;
					OutRenderer = Renderer;
					return true;
				}
			}
		}

		OutError = TEXT("renderer_not_found");
		return false;
	}

	static FString ExecutionModeToString(const EScriptExecutionMode ExecutionMode)
	{
		if (const UEnum* Enum = StaticEnum<EScriptExecutionMode>())
		{
			return Enum->GetNameStringByValue((int64)ExecutionMode);
		}
		return TEXT("");
	}

	static bool ParseExecutionMode(const FString& ExecutionModeText, EScriptExecutionMode& OutExecutionMode)
	{
		const FString Input = ExecutionModeText.TrimStartAndEnd();
		if (Input.IsEmpty())
		{
			return false;
		}

		if (const UEnum* Enum = StaticEnum<EScriptExecutionMode>())
		{
			int64 Value = Enum->GetValueByNameString(Input, EGetByNameFlags::CheckAuthoredName);
			if (Value == INDEX_NONE)
			{
				const FString QualifiedName = FString::Printf(TEXT("EScriptExecutionMode::%s"), *Input);
				Value = Enum->GetValueByNameString(QualifiedName, EGetByNameFlags::CheckAuthoredName);
			}
			if (Value == INDEX_NONE)
			{
				Value = Enum->GetValueByNameString(Input, EGetByNameFlags::None);
			}
			if (Value != INDEX_NONE)
			{
				OutExecutionMode = static_cast<EScriptExecutionMode>(Value);
				return true;
			}
		}
		return false;
	}

	static TSharedPtr<FJsonObject> BuildEventHandlerJson(FNiagaraEventScriptProperties& EventHandler, const int32 EventIndex)
	{
		TSharedPtr<FJsonObject> EventObj = MakeShared<FJsonObject>();
		const FGuid UsageId = EventHandler.Script ? EventHandler.Script->GetUsageId() : FGuid();
		EventObj->SetNumberField(TEXT("event_index"), EventIndex);
		EventObj->SetStringField(TEXT("script_usage_id"), UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
		EventObj->SetStringField(TEXT("stage_key"), MakeStageKey(ENiagaraScriptUsage::ParticleEventScript, UsageId));
		EventObj->SetStringField(TEXT("execution_mode"), ExecutionModeToString(EventHandler.ExecutionMode));
		EventObj->SetNumberField(TEXT("spawn_number"), EventHandler.SpawnNumber);
		EventObj->SetNumberField(TEXT("min_spawn_number"), EventHandler.MinSpawnNumber);
		EventObj->SetBoolField(TEXT("random_spawn_number"), EventHandler.bRandomSpawnNumber);
		EventObj->SetNumberField(TEXT("max_events_per_frame"), EventHandler.MaxEventsPerFrame);
		EventObj->SetBoolField(TEXT("update_attribute_initial_values"), EventHandler.UpdateAttributeInitialValues);
		EventObj->SetStringField(TEXT("source_event_name"), EventHandler.SourceEventName.ToString());
		EventObj->SetStringField(TEXT("source_emitter_id"), EventHandler.SourceEmitterID.ToString(EGuidFormats::DigitsWithHyphensLower));
		if (EventHandler.Script)
		{
			EventObj->SetStringField(TEXT("script_object_path"), EventHandler.Script->GetPathName());
		}
		return EventObj;
	}

	static bool ResolveEventHandlerFromParams(FNiagaraEmitterEditContext& EditContext, const TSharedPtr<FJsonObject>& Params, int32& OutEventIndex, FNiagaraEventScriptProperties*& OutEventHandler, FString& OutError)
	{
		OutEventIndex = INDEX_NONE;
		OutEventHandler = nullptr;

		if (!EditContext.EmitterData)
		{
			OutError = TEXT("missing_emitter_version_data");
			return false;
		}

		TArray<FNiagaraEventScriptProperties>& EventHandlers = EditContext.EmitterData->EventHandlerScriptProps;
		if (EventHandlers.Num() == 0)
		{
			OutError = TEXT("emitter_has_no_event_handlers");
			return false;
		}

		double EventIndexNumber = 0.0;
		if (Params.IsValid() && Params->TryGetNumberField(TEXT("event_index"), EventIndexNumber))
		{
			const int32 Index = FMath::RoundToInt(EventIndexNumber);
			if (!EventHandlers.IsValidIndex(Index))
			{
				OutError = TEXT("invalid_event_index");
				return false;
			}

			OutEventIndex = Index;
			OutEventHandler = &EventHandlers[Index];
			return true;
		}

		FString ScriptUsageIdText;
		if (Params.IsValid() && Params->TryGetStringField(TEXT("script_usage_id"), ScriptUsageIdText) && !ScriptUsageIdText.IsEmpty())
		{
			FGuid ScriptUsageId;
			if (!FGuid::Parse(ScriptUsageIdText, ScriptUsageId))
			{
				OutError = TEXT("invalid_script_usage_id");
				return false;
			}

			for (int32 Index = 0; Index < EventHandlers.Num(); ++Index)
			{
				FNiagaraEventScriptProperties& EventHandler = EventHandlers[Index];
				if (EventHandler.Script && EventHandler.Script->GetUsageId() == ScriptUsageId)
				{
					OutEventIndex = Index;
					OutEventHandler = &EventHandler;
					return true;
				}
			}

			OutError = TEXT("event_handler_not_found");
			return false;
		}

		FString SourceEventName;
		if (Params.IsValid() && Params->TryGetStringField(TEXT("source_event_name"), SourceEventName) && !SourceEventName.IsEmpty())
		{
			for (int32 Index = 0; Index < EventHandlers.Num(); ++Index)
			{
				FNiagaraEventScriptProperties& EventHandler = EventHandlers[Index];
				if (EventHandler.SourceEventName.ToString().Equals(SourceEventName, ESearchCase::IgnoreCase))
				{
					OutEventIndex = Index;
					OutEventHandler = &EventHandler;
					return true;
				}
			}
		}

		OutError = TEXT("event_handler_not_found");
		return false;
	}

	static UEdGraphNode* FindGraphNodeByGuid(UNiagaraGraph& Graph, const FGuid& NodeGuid)
	{
		for (UEdGraphNode* Node : Graph.Nodes)
		{
			if (Node && Node->NodeGuid == NodeGuid)
			{
				return Node;
			}
		}
		return nullptr;
	}

	static bool EnsureGraphNodeGuid(UEdGraphNode* Node, TSet<FGuid>* ExistingGuids)
	{
		if (!Node)
		{
			return false;
		}

		if (Node->NodeGuid.IsValid() && (!ExistingGuids || !ExistingGuids->Contains(Node->NodeGuid)))
		{
			if (ExistingGuids)
			{
				ExistingGuids->Add(Node->NodeGuid);
			}
			return false;
		}

		Node->Modify();
		for (int32 Attempt = 0; Attempt < 16; ++Attempt)
		{
			Node->CreateNewGuid();
			if (Node->NodeGuid.IsValid() && (!ExistingGuids || !ExistingGuids->Contains(Node->NodeGuid)))
			{
				if (ExistingGuids)
				{
					ExistingGuids->Add(Node->NodeGuid);
				}
				return true;
			}
		}
		return false;
	}

	static int32 NormalizeGraphNodeGuids(UEdGraph* Graph)
	{
		if (!Graph)
		{
			return 0;
		}

		TSet<FGuid> ExistingGuids;
		int32 FixedCount = 0;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (EnsureGraphNodeGuid(Node, &ExistingGuids))
			{
				++FixedCount;
			}
		}
		return FixedCount;
	}

	static UEdGraphPin* ResolvePinByName(UEdGraphNode& Node, const FString& PinName)
	{
		const FString Wanted = PinName.TrimStartAndEnd();
		if (Wanted.IsEmpty())
		{
			return nullptr;
		}

		for (UEdGraphPin* Pin : Node.Pins)
		{
			if (!Pin)
			{
				continue;
			}
			if (Pin->PinName.ToString().Equals(Wanted, ESearchCase::IgnoreCase))
			{
				return Pin;
			}
		}
		return nullptr;
	}

	static bool ResolveStageNodeFromParams(UNiagaraGraph& Graph, const TSharedPtr<FJsonObject>& Params, FResolvedNiagaraStage& OutStage, UEdGraphNode*& OutStageNode, FString& OutError)
	{
		OutStageNode = nullptr;

		if (!ResolveStageFromParams(Graph, Params, OutStage, OutError))
		{
			return false;
		}

		FString NodeGuidText;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("node_guid"), NodeGuidText) || NodeGuidText.IsEmpty())
		{
			OutError = TEXT("missing_node_guid");
			return false;
		}

		FGuid NodeGuid;
		if (!FGuid::Parse(NodeGuidText, NodeGuid))
		{
			OutError = TEXT("invalid_node_guid");
			return false;
		}

		TSet<UEdGraphNode*> VisitedNodes;
		TArray<UEdGraphNode*> StageNodes;
		GatherUpstreamNodesRecursive(OutStage.OutputNode, VisitedNodes, StageNodes);

		for (UEdGraphNode* StageNode : StageNodes)
		{
			if (StageNode && StageNode->NodeGuid == NodeGuid)
			{
				OutStageNode = StageNode;
				break;
			}
		}

		if (!OutStageNode)
		{
			OutError = TEXT("node_not_found_in_stage");
			return false;
		}

		return true;
	}

	static bool FindOwningOutputNodeForModule(UNiagaraGraph& Graph, UNiagaraNodeFunctionCall& ModuleNode, UNiagaraNodeOutput*& OutOutputNode)
	{
		OutOutputNode = nullptr;
		TArray<UNiagaraNodeOutput*> OutputNodes;
		GetOutputNodesFromGraph(Graph, OutputNodes);
		for (UNiagaraNodeOutput* OutputNode : OutputNodes)
		{
			if (!OutputNode)
			{
				continue;
			}

			TArray<UNiagaraNodeFunctionCall*> Modules;
			GetStageModuleNodes(*OutputNode, Modules);
			for (UNiagaraNodeFunctionCall* CandidateModule : Modules)
			{
				if (CandidateModule == &ModuleNode)
				{
					OutOutputNode = OutputNode;
					return true;
				}
			}
		}
		return false;
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

	static void DestroyGraphNodesForStage(UNiagaraGraph& Graph, const ENiagaraScriptUsage Usage, const FGuid& UsageId)
	{
		TArray<UNiagaraNodeOutput*> OutputNodes;
		GetOutputNodesFromGraph(Graph, OutputNodes);
		for (UNiagaraNodeOutput* OutputNode : OutputNodes)
		{
			if (!OutputNode || OutputNode->GetUsage() != Usage || OutputNode->GetUsageId() != UsageId)
			{
				continue;
			}

			TSet<UEdGraphNode*> VisitedNodes;
			TArray<UEdGraphNode*> StageNodes;
			GatherUpstreamNodesRecursive(OutputNode, VisitedNodes, StageNodes);
			for (UEdGraphNode* StageNode : StageNodes)
			{
				if (!StageNode)
				{
					continue;
				}
				StageNode->Modify();
				StageNode->DestroyNode();
			}
			return;
		}
	}

	static UNiagaraScript* ResolveOwningScriptForStage(FVersionedNiagaraEmitterData& EmitterData, const ENiagaraScriptUsage Usage, const FGuid& UsageId)
	{
		if (Usage == ENiagaraScriptUsage::EmitterSpawnScript)
		{
			return EmitterData.EmitterSpawnScriptProps.Script;
		}
		if (Usage == ENiagaraScriptUsage::EmitterUpdateScript)
		{
			return EmitterData.EmitterUpdateScriptProps.Script;
		}
		if (Usage == ENiagaraScriptUsage::ParticleSpawnScript)
		{
			return EmitterData.SpawnScriptProps.Script;
		}
		if (Usage == ENiagaraScriptUsage::ParticleUpdateScript)
		{
			return EmitterData.UpdateScriptProps.Script;
		}
		if (Usage == ENiagaraScriptUsage::ParticleEventScript)
		{
			for (FNiagaraEventScriptProperties& EventHandler : EmitterData.EventHandlerScriptProps)
			{
				if (EventHandler.Script && EventHandler.Script->GetUsageId() == UsageId)
				{
					return EventHandler.Script;
				}
			}
			return nullptr;
		}
		if (Usage == ENiagaraScriptUsage::ParticleSimulationStageScript)
		{
			if (UNiagaraSimulationStageBase* SimulationStage = EmitterData.GetSimulationStageById(UsageId))
			{
				return SimulationStage->Script;
			}
			return nullptr;
		}
		return nullptr;
	}

	static FNiagaraSystemRefreshResult MarkEmitterGraphEdited(FNiagaraEmitterEditContext& Context)
	{
		FNiagaraSystemRefreshResult RefreshResult;
		if (Context.Graph)
		{
			Context.Graph->NotifyGraphChanged();
		}
		if (Context.Emitter)
		{
			Context.Emitter->MarkPackageDirty();

			if (UNiagaraSystem* OwningSystem = Context.Emitter->GetTypedOuter<UNiagaraSystem>())
			{
				RefreshResult = RefreshNiagaraSystemAfterStructuralEdit(*OwningSystem, true, true);
				FUeAgentInterfaceLogger::Log(
					TEXT("Niagara MarkEmitterGraphEdited: refreshed system=%s emitter=%s"),
					*OwningSystem->GetPathName(),
					*Context.Emitter->GetPathName());
			}
		}
		return RefreshResult;
	}

	static FNiagaraSystemRefreshResult MarkSystemGraphEdited(FNiagaraSystemEditContext& Context)
	{
		FNiagaraSystemRefreshResult RefreshResult;
		if (Context.Graph)
		{
			Context.Graph->NotifyGraphChanged();
		}
		if (Context.System)
		{
			Context.System->MarkPackageDirty();
			RefreshResult = RefreshNiagaraSystemAfterStructuralEdit(*Context.System, true, true);
			FUeAgentInterfaceLogger::Log(
				TEXT("Niagara MarkSystemGraphEdited: refreshed system=%s"),
				*Context.System->GetPathName());
		}
		return RefreshResult;
	}

	static int32 RepairSystemInvalidEmitterVersions(UNiagaraSystem& NiagaraSystem)
	{
		int32 RepairedCount = 0;
		TArray<FNiagaraEmitterHandle>& Handles = NiagaraSystem.GetEmitterHandles();
		for (FNiagaraEmitterHandle& Handle : Handles)
		{
			FVersionedNiagaraEmitter Instance = Handle.GetInstance();
			if (!Instance.Emitter)
			{
				continue;
			}

			const bool bVersionGuidInvalid = !Instance.Version.IsValid();
			const bool bMissingVersionData = Instance.GetEmitterData() == nullptr;
			if (!bVersionGuidInvalid && !bMissingVersionData)
			{
				continue;
			}

			const FGuid ExposedVersionGuid = Instance.Emitter->GetExposedVersion().VersionGuid;
			if (!ExposedVersionGuid.IsValid())
			{
				continue;
			}

			const bool bChangedByApi = NiagaraSystem.ChangeEmitterVersion(Instance, ExposedVersionGuid);
			if (!bChangedByApi)
			{
				FVersionedNiagaraEmitter& MutableInstance = Handle.GetInstance();
				MutableInstance.Version = ExposedVersionGuid;
			}

			++RepairedCount;
		}
		return RepairedCount;
	}

	static bool ResolveEmitterHandleFromParams(UNiagaraSystem& NiagaraSystem, const TSharedPtr<FJsonObject>& Params, int32& OutEmitterIndex, FNiagaraEmitterHandle*& OutEmitterHandle, FString& OutError)
	{
		OutEmitterIndex = INDEX_NONE;
		OutEmitterHandle = nullptr;

		TArray<FNiagaraEmitterHandle>& Handles = NiagaraSystem.GetEmitterHandles();
		if (Handles.Num() == 0)
		{
			OutError = TEXT("system_has_no_emitters");
			return false;
		}

		double HandleIndexNumber = 0.0;
		if (Params.IsValid() && Params->TryGetNumberField(TEXT("emitter_index"), HandleIndexNumber))
		{
			const int32 Index = FMath::RoundToInt(HandleIndexNumber);
			if (!Handles.IsValidIndex(Index))
			{
				OutError = TEXT("invalid_emitter_index");
				return false;
			}

			OutEmitterIndex = Index;
			OutEmitterHandle = &Handles[Index];
			return true;
		}

		FString EmitterIdString;
		if (Params.IsValid() && Params->TryGetStringField(TEXT("emitter_id"), EmitterIdString) && !EmitterIdString.IsEmpty())
		{
			FGuid EmitterGuid;
			if (!FGuid::Parse(EmitterIdString, EmitterGuid))
			{
				OutError = TEXT("invalid_emitter_id");
				return false;
			}

			for (int32 Index = 0; Index < Handles.Num(); ++Index)
			{
				if (Handles[Index].GetId() == EmitterGuid)
				{
					OutEmitterIndex = Index;
					OutEmitterHandle = &Handles[Index];
					return true;
				}
			}
			OutError = TEXT("emitter_not_found");
			return false;
		}

		FString EmitterName;
		if (Params.IsValid() && Params->TryGetStringField(TEXT("emitter_name"), EmitterName) && !EmitterName.IsEmpty())
		{
			for (int32 Index = 0; Index < Handles.Num(); ++Index)
			{
				if (Handles[Index].GetName().ToString().Equals(EmitterName, ESearchCase::IgnoreCase))
				{
					OutEmitterIndex = Index;
					OutEmitterHandle = &Handles[Index];
					return true;
				}
			}

			OutError = TEXT("emitter_not_found");
			return false;
		}

		OutError = TEXT("missing_emitter_identifier");
		return false;
	}

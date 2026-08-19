// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentNiagaraStageModuleAuthoring.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Misc/PackageName.h"
#include "NiagaraEmitter.h"
#include "NiagaraConstants.h"
#include "NiagaraDataInterface.h"
#include "NiagaraGraph.h"
#include "NiagaraNode.h"
#include "NiagaraNodeAssignment.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "EdGraphSchema_Niagara.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"

namespace UeAgentNiagaraStageModuleAuthoring
{
	namespace
	{
		struct FStackNodeGroup
		{
			TArray<UNiagaraNode*> StartNodes;
			UNiagaraNode* EndNode = nullptr;
		};

		bool IsParameterMapPin(const UEdGraphPin* Pin)
		{
			return Pin != nullptr
				&& UEdGraphSchema_Niagara::PinToTypeDefinition(Pin) == FNiagaraTypeDefinition::GetParameterMapDef();
		}

		UEdGraphPin* FindParameterMapPin(UEdGraphNode& Node, EEdGraphPinDirection Direction);

		FString NormalizePackagePath(FString Path)
		{
			Path.TrimStartAndEndInline();
			return Path.Contains(TEXT(".")) ? FPackageName::ObjectPathToPackageName(Path) : Path;
		}

		UObject* LoadAsset(const FString& Path)
		{
			const FString PackagePath = NormalizePackagePath(Path);
			const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
			return AssetName.IsEmpty()
				? nullptr
				: StaticLoadObject(UObject::StaticClass(), nullptr, *FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName));
		}

		void GetOutputNodes(UNiagaraGraph& Graph, TArray<UNiagaraNodeOutput*>& OutNodes)
		{
			OutNodes.Reset();
			for (UEdGraphNode* Node : Graph.Nodes)
			{
				if (UNiagaraNodeOutput* Output = Cast<UNiagaraNodeOutput>(Node)) OutNodes.Add(Output);
			}
		}

		void AppendStagesFromSource(
			UObject& RootAsset,
			UNiagaraSystem* System,
			UNiagaraEmitter* Emitter,
			const FGuid& EmitterVersion,
			const FGuid& EmitterHandleId,
			const FString& EmitterName,
			UNiagaraScriptSource* Source,
			TArray<FResolvedStage>& OutStages)
		{
			UNiagaraGraph* Graph = Source ? Source->NodeGraph : nullptr;
			if (!Graph) return;
			TArray<UNiagaraNodeOutput*> Outputs;
			GetOutputNodes(*Graph, Outputs);
			for (UNiagaraNodeOutput* Output : Outputs)
			{
				if (!Output) continue;
				FResolvedStage& Stage = OutStages.AddDefaulted_GetRef();
				Stage.Asset = &RootAsset;
				Stage.System = System;
				Stage.Emitter = Emitter;
				Stage.EmitterVersion = EmitterVersion;
				Stage.EmitterHandleId = EmitterHandleId;
				Stage.ScriptSource = Source;
				Stage.Graph = Graph;
				Stage.OutputNode = Output;
				Stage.Usage = Output->GetUsage();
				Stage.UsageId = Output->GetUsageId();
				Stage.EmitterName = EmitterName;
				Stage.StageKey = MakeStageKey(Stage.Usage, Stage.UsageId);
			}
		}

		FString LowerTrimmed(const FString& Text)
		{
			FString Result = Text.TrimStartAndEnd();
			Result.ToLowerInline();
			return Result;
		}

		bool ParseBool(const FString& Text, bool& OutValue)
		{
			const FString Normalized = LowerTrimmed(Text);
			if (Normalized == TEXT("true") || Normalized == TEXT("1"))
			{
				OutValue = true;
				return true;
			}
			if (Normalized == TEXT("false") || Normalized == TEXT("0"))
			{
				OutValue = false;
				return true;
			}
			return false;
		}

		bool ParseComponents(const FString& Text, const int32 ExpectedCount, TArray<double>& OutValues)
		{
			FString Clean = Text;
			for (const TCHAR* Token : { TEXT("("), TEXT(")"), TEXT("{"), TEXT("}"), TEXT("["), TEXT("]"), TEXT(","), TEXT(";") })
			{
				Clean.ReplaceInline(Token, TEXT(" "));
			}
			for (const TCHAR* Token : {
				TEXT("X="), TEXT("Y="), TEXT("Z="), TEXT("W="),
				TEXT("R="), TEXT("G="), TEXT("B="), TEXT("A="),
				TEXT("x="), TEXT("y="), TEXT("z="), TEXT("w="),
				TEXT("r="), TEXT("g="), TEXT("b="), TEXT("a=") })
			{
				Clean.ReplaceInline(Token, TEXT(" "));
			}
			TArray<FString> Parts;
			Clean.ParseIntoArrayWS(Parts);
			if (Parts.Num() != ExpectedCount) return false;
			OutValues.Reset(ExpectedCount);
			for (const FString& Part : Parts)
			{
				double Value = 0.0;
				if (!LexTryParseString(Value, *Part) || !FMath::IsFinite(Value)) return false;
				OutValues.Add(Value);
			}
			return true;
		}

		bool ResolveEnumValue(const UEnum& Enum, FString Text, int64& OutValue)
		{
			Text.TrimStartAndEndInline();
			if (Text.IsEmpty()) return false;
			if (Text.StartsWith(TEXT("\"")) && Text.EndsWith(TEXT("\"")) && Text.Len() > 1)
			{
				Text = Text.Mid(1, Text.Len() - 2);
			}
			if (LexTryParseString(OutValue, *Text) && Enum.IsValidEnumValue(OutValue)) return true;
			OutValue = Enum.GetValueByNameString(Text, EGetByNameFlags::CheckAuthoredName);
			if (OutValue == INDEX_NONE) OutValue = Enum.GetValueByNameString(Text, EGetByNameFlags::None);
			if (OutValue == INDEX_NONE)
			{
				int32 ScopeIndex = INDEX_NONE;
				if (Text.FindLastChar(TEXT(':'), ScopeIndex) && ScopeIndex + 1 < Text.Len())
				{
					const FString ShortName = Text.Mid(ScopeIndex + 1);
					OutValue = Enum.GetValueByNameString(ShortName, EGetByNameFlags::CheckAuthoredName);
					if (OutValue == INDEX_NONE) OutValue = Enum.GetValueByNameString(ShortName, EGetByNameFlags::None);
				}
			}
			return OutValue != INDEX_NONE && Enum.IsValidEnumValue(OutValue);
		}

		FString GetLiteralValueKind(const FNiagaraTypeDefinition& Type)
		{
			if (Type == FNiagaraTypeDefinition::GetBoolDef()) return TEXT("bool");
			if (Type == FNiagaraTypeDefinition::GetIntDef()) return TEXT("int");
			if (Type == FNiagaraTypeDefinition::GetFloatDef()) return TEXT("float");
			if (Type == FNiagaraTypeHelper::GetDoubleDef()) return TEXT("double");
			if (Type == FNiagaraTypeDefinition::GetVec2Def()) return TEXT("vector2");
			if (Type == FNiagaraTypeDefinition::GetVec3Def() || Type == FNiagaraTypeDefinition::GetPositionDef()) return TEXT("vector3");
			if (Type == FNiagaraTypeDefinition::GetVec4Def()) return TEXT("vector4");
			if (Type == FNiagaraTypeDefinition::GetQuatDef()) return TEXT("quat");
			if (Type == FNiagaraTypeDefinition::GetColorDef()) return TEXT("linear_color");
			if (Type.IsEnum()) return TEXT("enum");
			return FString();
		}

		int32 GetComponentCount(const FString& ValueKind)
		{
			if (ValueKind == TEXT("vector2")) return 2;
			if (ValueKind == TEXT("vector3")) return 3;
			if (ValueKind == TEXT("vector4") || ValueKind == TEXT("quat") || ValueKind == TEXT("linear_color")) return 4;
			return 0;
		}

		bool EncodeLiteralValue(
			const FNiagaraTypeDefinition& Type,
			const FString& DefaultText,
			TSharedPtr<FJsonValue>& OutValue,
			FString& OutKind,
			FString& OutError)
		{
			OutKind = GetLiteralValueKind(Type);
			if (OutKind.IsEmpty()) return false;
			if (OutKind == TEXT("bool"))
			{
				bool Value = false;
				if (!ParseBool(DefaultText, Value)) { OutError = TEXT("niagara_literal_bool_invalid"); return false; }
				OutValue = MakeShared<FJsonValueBoolean>(Value);
				return true;
			}
			if (OutKind == TEXT("int"))
			{
				int32 Value = 0;
				if (!LexTryParseString(Value, *DefaultText)) { OutError = TEXT("niagara_literal_int_invalid"); return false; }
				OutValue = MakeShared<FJsonValueNumber>(Value);
				return true;
			}
			if (OutKind == TEXT("float") || OutKind == TEXT("double"))
			{
				double Value = 0.0;
				if (!LexTryParseString(Value, *DefaultText) || !FMath::IsFinite(Value)) { OutError = TEXT("niagara_literal_number_invalid"); return false; }
				OutValue = MakeShared<FJsonValueNumber>(Value);
				return true;
			}
			if (OutKind == TEXT("enum"))
			{
				const UEnum* Enum = Type.GetEnum();
				int64 Value = 0;
				if (!Enum || !ResolveEnumValue(*Enum, DefaultText, Value)) { OutError = TEXT("niagara_literal_enum_invalid"); return false; }
				TSharedRef<FJsonObject> Selected = MakeShared<FJsonObject>();
				Selected->SetStringField(TEXT("selected"), Enum->GetNameStringByValue(Value));
				OutValue = MakeShared<FJsonValueObject>(Selected);
				return true;
			}
			TArray<double> Components;
			if (!ParseComponents(DefaultText, GetComponentCount(OutKind), Components))
			{
				OutError = TEXT("niagara_literal_components_invalid");
				return false;
			}
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const double Component : Components) Values.Add(MakeShared<FJsonValueNumber>(Component));
			OutValue = MakeShared<FJsonValueArray>(Values);
			return true;
		}

		bool DecodeLiteralValue(
			const FNiagaraTypeDefinition& Type,
			const TSharedPtr<const FJsonObject>& Payload,
			FString& OutDefaultText,
			FString& OutError)
		{
			const TSharedPtr<FJsonValue> Value = Payload.IsValid() ? Payload->TryGetField(TEXT("value")) : nullptr;
			if (!Value.IsValid()) { OutError = TEXT("niagara_literal_value_missing"); return false; }
			const FString Kind = GetLiteralValueKind(Type);
			if (Kind.IsEmpty()) { OutError = TEXT("niagara_literal_type_unsupported"); return false; }
			FString DeclaredKind;
			if (Payload->TryGetStringField(TEXT("value_kind"), DeclaredKind)
				&& !DeclaredKind.IsEmpty() && !DeclaredKind.Equals(Kind, ESearchCase::IgnoreCase))
			{
				OutError = TEXT("niagara_literal_value_kind_mismatch");
				return false;
			}
			if (Kind == TEXT("bool"))
			{
				if (Value->Type != EJson::Boolean) { OutError = TEXT("niagara_literal_bool_expected"); return false; }
				OutDefaultText = Value->AsBool() ? TEXT("true") : TEXT("false");
				return true;
			}
			if (Kind == TEXT("int"))
			{
				if (Value->Type != EJson::Number) { OutError = TEXT("niagara_literal_int_expected"); return false; }
				const double Number = Value->AsNumber();
				const int32 Integer = FMath::RoundToInt(Number);
				if (!FMath::IsNearlyEqual(Number, static_cast<double>(Integer))) { OutError = TEXT("niagara_literal_int_fractional"); return false; }
				OutDefaultText = LexToString(Integer);
				return true;
			}
			if (Kind == TEXT("float") || Kind == TEXT("double"))
			{
				if (Value->Type != EJson::Number || !FMath::IsFinite(Value->AsNumber())) { OutError = TEXT("niagara_literal_number_expected"); return false; }
				OutDefaultText = FString::SanitizeFloat(Value->AsNumber());
				return true;
			}
			if (Kind == TEXT("enum"))
			{
				if (Value->Type != EJson::Object) { OutError = TEXT("niagara_literal_enum_object_expected"); return false; }
				FString Selected;
				if (!Value->AsObject()->TryGetStringField(TEXT("selected"), Selected)) { OutError = TEXT("niagara_literal_enum_selected_missing"); return false; }
				const UEnum* Enum = Type.GetEnum();
				int64 EnumValue = 0;
				if (!Enum || !ResolveEnumValue(*Enum, Selected, EnumValue)) { OutError = TEXT("niagara_literal_enum_invalid"); return false; }
				OutDefaultText = Enum->GetNameStringByValue(EnumValue);
				return true;
			}
			if (Value->Type != EJson::Array || Value->AsArray().Num() != GetComponentCount(Kind))
			{
				OutError = TEXT("niagara_literal_component_count_invalid");
				return false;
			}
			const TArray<const TCHAR*> Names = Kind == TEXT("linear_color")
				? TArray<const TCHAR*>{ TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A") }
				: TArray<const TCHAR*>{ TEXT("X"), TEXT("Y"), TEXT("Z"), TEXT("W") };
			TArray<FString> Parts;
			for (int32 Index = 0; Index < Value->AsArray().Num(); ++Index)
			{
				const TSharedPtr<FJsonValue>& Component = Value->AsArray()[Index];
				if (!Component.IsValid() || Component->Type != EJson::Number || !FMath::IsFinite(Component->AsNumber()))
				{
					OutError = TEXT("niagara_literal_component_number_expected");
					return false;
				}
				Parts.Add(FString::Printf(TEXT("%s=%s"), Names[Index], *FString::SanitizeFloat(Component->AsNumber())));
			}
			OutDefaultText = TEXT("(") + FString::Join(Parts, TEXT(",")) + TEXT(")");
			return true;
		}

		bool IsParameterMapSetNode(const UEdGraphNode* Node)
		{
			for (const UClass* Class = Node ? Node->GetClass() : nullptr; Class; Class = Class->GetSuperClass())
			{
				if (Class->GetName() == TEXT("NiagaraNodeParameterMapSet")) return true;
			}
			return false;
		}

		bool IsParameterMapGetNode(const UEdGraphNode* Node)
		{
			for (const UClass* Class = Node ? Node->GetClass() : nullptr; Class; Class = Class->GetSuperClass())
			{
				if (Class->GetName() == TEXT("NiagaraNodeParameterMapGet")) return true;
			}
			return false;
		}

		void RemoveValueNodesForOverridePin(UEdGraphPin& OverridePin)
		{
			if (OverridePin.LinkedTo.Num() != 1 || !OverridePin.LinkedTo[0])
			{
				OverridePin.BreakAllPinLinks();
				return;
			}
			UEdGraphNode* ValueNode = OverridePin.LinkedTo[0]->GetOwningNode();
			UEdGraph* Graph = ValueNode ? ValueNode->GetGraph() : nullptr;
			if (!ValueNode || !Graph)
			{
				OverridePin.BreakAllPinLinks();
				return;
			}
			if (ValueNode->IsA<UNiagaraNodeInput>() || IsParameterMapGetNode(ValueNode))
			{
				Graph->RemoveNode(ValueNode);
				return;
			}
			UNiagaraNodeFunctionCall* DynamicInputNode = Cast<UNiagaraNodeFunctionCall>(ValueNode);
			if (!DynamicInputNode)
			{
				OverridePin.BreakAllPinLinks();
				return;
			}

			UEdGraphPin* DynamicParameterMapInput = FindParameterMapPin(*DynamicInputNode, EGPD_Input);
			UEdGraphNode* OverrideNode = DynamicParameterMapInput && DynamicParameterMapInput->LinkedTo.Num() == 1
				? DynamicParameterMapInput->LinkedTo[0]->GetOwningNode()
				: nullptr;
			if (IsParameterMapSetNode(OverrideNode))
			{
				TArray<UEdGraphPin*> DynamicOverridePins;
				for (UEdGraphPin* Pin : OverrideNode->Pins)
				{
					if (!Pin || Pin->Direction != EGPD_Input || IsParameterMapPin(Pin)) continue;
					const FNiagaraParameterHandle Handle(Pin->PinName);
					if (Handle.GetNamespace().ToString() == DynamicInputNode->GetFunctionName())
					{
						DynamicOverridePins.Add(Pin);
					}
				}
				for (UEdGraphPin* Pin : DynamicOverridePins)
				{
					RemoveValueNodesForOverridePin(*Pin);
					OverrideNode->RemovePin(Pin);
				}

				TArray<UEdGraphPin*> RemainingInputs;
				for (UEdGraphPin* Pin : OverrideNode->Pins)
				{
					if (Pin && Pin->Direction == EGPD_Input) RemainingInputs.Add(Pin);
				}
				UEdGraphPin* ParameterMapInput = FindParameterMapPin(*OverrideNode, EGPD_Input);
				UEdGraphPin* ParameterMapOutput = FindParameterMapPin(*OverrideNode, EGPD_Output);
				if (RemainingInputs.Num() == 2 && ParameterMapInput && ParameterMapInput->LinkedTo.Num() == 1
					&& ParameterMapOutput && ParameterMapOutput->LinkedTo.Num() >= 2)
				{
					UEdGraphPin* PreviousMapOutput = ParameterMapInput->LinkedTo[0];
					TArray<UEdGraphPin*> DownstreamInputs;
					for (UEdGraphPin* LinkedPin : ParameterMapOutput->LinkedTo)
					{
						if (LinkedPin && LinkedPin->GetOwningNode() != DynamicInputNode) DownstreamInputs.Add(LinkedPin);
					}
					ParameterMapInput->BreakAllPinLinks();
					ParameterMapOutput->BreakAllPinLinks();
					Graph->RemoveNode(OverrideNode);
					for (UEdGraphPin* DownstreamInput : DownstreamInputs)
					{
						PreviousMapOutput->MakeLinkTo(DownstreamInput);
					}
				}
			}
			Graph->RemoveNode(DynamicInputNode);
		}

		UEdGraphPin* FindOverridePin(UNiagaraGraph* Graph, const FName AliasedInputName)
		{
			if (!Graph || AliasedInputName.IsNone()) return nullptr;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node) continue;
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (Pin && Pin->Direction == EGPD_Input && Pin->PinName == AliasedInputName
						&& IsParameterMapSetNode(Pin->GetOwningNode()))
					{
						return Pin;
					}
				}
			}
			return nullptr;
		}

		void GetModuleInputPins(UNiagaraNodeFunctionCall& ModuleNode, TArray<UEdGraphPin*>& OutPins)
		{
			OutPins.Reset();
			for (UEdGraphPin* Pin : ModuleNode.Pins)
			{
				if (Pin && Pin->Direction == EGPD_Input && Pin->ParentPin == nullptr && !IsParameterMapPin(Pin))
				{
					OutPins.Add(Pin);
				}
			}
		}

		FNiagaraParameterHandle BuildInputOverrideHandle(
			const FNiagaraVariable& InputVariable,
			UNiagaraNodeFunctionCall& FunctionNode)
		{
			const FNiagaraParameterHandle InputHandle(InputVariable.GetName());
			const UNiagaraGraph* Graph = FunctionNode.GetNiagaraGraph();
			const UNiagaraScript* OwningScript = Graph ? Graph->GetTypedOuter<UNiagaraScript>() : nullptr;
			if (FunctionNode.GetCalledUsage() == ENiagaraScriptUsage::DynamicInput
				&& OwningScript
				&& OwningScript->GetUsage() == ENiagaraScriptUsage::Module)
			{
				// A Dynamic Input authored inside a standalone Module Script is compiled inside
				// the caller's Module namespace.  Stack helpers normally author directly in an
				// emitter graph, where Function.Input is sufficient.  In a standalone module
				// that global handle leaves the callable input unbound and Niagara reads its
				// zero default after aliasing.  Keep Module as the outer namespace so the
				// compiler produces CallerModule.Function.Input at the invocation site.
				const FString ScopedName = FString::Printf(
					TEXT("Module.%s.%s"),
					*FunctionNode.GetFunctionName(),
					*InputHandle.GetName().ToString());
				return FNiagaraParameterHandle(*ScopedName);
			}
			return FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(
				InputHandle, &FunctionNode);
		}

		bool ResolveInputPin(
			UNiagaraNodeFunctionCall& ModuleNode,
			const FString& InputName,
			FString& OutResolvedInputName,
			FNiagaraVariable& OutVariable,
			FGuid& OutScriptVariableGuid,
			UEdGraphPin*& OutPin,
			FString& OutError)
		{
			TArray<FResolvedModuleInput> Inputs;
			BuildResolvedModuleInputs(ModuleNode, Inputs);
			const FResolvedModuleInput* Input = ResolveModuleInputByName(Inputs, InputName, OutResolvedInputName);
			if (!Input || !Input->InputVariable.IsValid())
			{
				TArray<FString> AvailableInputNames;
				AvailableInputNames.Reserve(Inputs.Num());
				for (const FResolvedModuleInput& Candidate : Inputs)
				{
					if (!Candidate.FullInputName.IsEmpty()) AvailableInputNames.Add(Candidate.FullInputName);
				}
				OutError = BuildInputNameNotFoundError(
					InputName,
					GetModuleScriptPath(ModuleNode),
					AvailableInputNames);
				return false;
			}
			OutVariable = Input->InputVariable;
			OutScriptVariableGuid = Input->InputScriptVariableGuid;
			OutPin = Input->VisiblePin;
			if (!OutPin)
			{
				const FNiagaraParameterHandle Aliased = BuildInputOverrideHandle(
					Input->InputVariable, ModuleNode);
				OutPin = FindOverridePin(ModuleNode.GetNiagaraGraph(), Aliased.GetParameterHandleString());
			}
			return true;
		}

		bool GetOrCreateInputPin(
			UNiagaraNodeFunctionCall& ModuleNode,
			const FNiagaraVariable& Variable,
			const FGuid& ScriptVariableGuid,
			UEdGraphPin*& InOutPin,
			FString& OutError)
		{
			if (InOutPin) return true;
			UNiagaraGraph* Graph = ModuleNode.GetNiagaraGraph();
			if (!Graph)
			{
				OutError = TEXT("module_graph_missing");
				return false;
			}
			UEdGraphPin* ParameterMapInput = FindParameterMapPin(ModuleNode, EGPD_Input);
			if (!ParameterMapInput || ParameterMapInput->LinkedTo.Num() != 1)
			{
				OutError = TEXT("module_parameter_map_input_invalid");
				return false;
			}
			const FNiagaraParameterHandle Aliased = BuildInputOverrideHandle(Variable, ModuleNode);
			if (ModuleNode.GetCalledUsage() == ENiagaraScriptUsage::DynamicInput)
			{
				UEdGraphPin* PreviousMapOutput = ParameterMapInput->LinkedTo[0];
				UEdGraphNode* PreviousNode = PreviousMapOutput
					? PreviousMapOutput->GetOwningNode()
					: nullptr;
				bool bHasDedicatedOverride = false;
				if (IsParameterMapSetNode(PreviousNode))
				{
					for (const UEdGraphPin* Pin : PreviousNode->Pins)
					{
						if (!Pin || Pin->Direction != EGPD_Input || IsParameterMapPin(Pin)) continue;
						const FNiagaraParameterHandle ExistingHandle(Pin->PinName);
						if (ExistingHandle.GetNamespace().ToString() == ModuleNode.GetFunctionName())
						{
							bHasDedicatedOverride = true;
							break;
						}
					}
				}
				if (!bHasDedicatedOverride)
				{
					UClass* SetNodeClass = StaticLoadClass(
						UNiagaraNode::StaticClass(),
						nullptr,
						TEXT("/Script/NiagaraEditor.NiagaraNodeParameterMapSet"));
					UNiagaraNode* OverrideNode = SetNodeClass
						? NewObject<UNiagaraNode>(Graph, SetNodeClass, NAME_None, RF_Transactional)
						: nullptr;
					if (!OverrideNode)
					{
						OutError = TEXT("niagara_dynamic_input_override_node_create_failed");
						return false;
					}
					OverrideNode->CreateNewGuid();
					OverrideNode->AllocateDefaultPins();
					Graph->AddNode(OverrideNode, true, false);
					UEdGraphPin* OverrideInput = FindParameterMapPin(*OverrideNode, EGPD_Input);
					UEdGraphPin* OverrideOutput = FindParameterMapPin(*OverrideNode, EGPD_Output);
					if (!PreviousMapOutput || !OverrideInput || !OverrideOutput)
					{
						Graph->RemoveNode(OverrideNode);
						OutError = TEXT("niagara_dynamic_input_override_node_invalid");
						return false;
					}
					ParameterMapInput->BreakLinkTo(PreviousMapOutput);
					PreviousMapOutput->MakeLinkTo(OverrideInput);
					OverrideOutput->MakeLinkTo(ParameterMapInput);
					if (!PreviousMapOutput->LinkedTo.Contains(OverrideInput)
						|| !OverrideOutput->LinkedTo.Contains(ParameterMapInput))
					{
						Graph->RemoveNode(OverrideNode);
						OutError = TEXT("niagara_dynamic_input_override_link_failed");
						return false;
					}
				}
			}
			InOutPin = &FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
				ModuleNode, Aliased, Variable.GetType(), ScriptVariableGuid, FGuid());
			return InOutPin != nullptr;
		}

		UEdGraphPin* FindParameterMapPin(UEdGraphNode& Node, const EEdGraphPinDirection Direction)
		{
			for (UEdGraphPin* Pin : Node.Pins)
			{
				if (Pin && Pin->Direction == Direction && IsParameterMapPin(Pin))
				{
					return Pin;
				}
			}
			return nullptr;
		}

		void GatherUpstreamNodes(UEdGraphNode* Node, TSet<UEdGraphNode*>& VisitedNodes, TArray<UEdGraphNode*>& OutNodes)
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
						GatherUpstreamNodes(LinkedPin->GetOwningNode(), VisitedNodes, OutNodes);
					}
				}
			}
		}

		UNiagaraNodeInput* FindStackInputNode(UNiagaraNodeOutput& OutputNode)
		{
			UEdGraphNode* CurrentNode = &OutputNode;
			TSet<UEdGraphNode*> VisitedNodes;
			while (CurrentNode && !VisitedNodes.Contains(CurrentNode))
			{
				VisitedNodes.Add(CurrentNode);
				UEdGraphPin* InputPin = FindParameterMapPin(*CurrentNode, EGPD_Input);
				if (!InputPin)
				{
					return nullptr;
				}
				UEdGraphPin* LinkedParameterMapPin = nullptr;
				for (UEdGraphPin* LinkedPin : InputPin->LinkedTo)
				{
					if (IsParameterMapPin(LinkedPin))
					{
						LinkedParameterMapPin = LinkedPin;
						break;
					}
				}
				if (!LinkedParameterMapPin)
				{
					return nullptr;
				}
				CurrentNode = LinkedParameterMapPin->GetOwningNode();
				if (UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(CurrentNode))
				{
					return InputNode;
				}
			}
			return nullptr;
		}

		void GetStackNodeGroups(UNiagaraNodeOutput& OutputNode, TArray<FStackNodeGroup>& OutGroups)
		{
			OutGroups.Reset();
			UNiagaraNodeInput* InputNode = FindStackInputNode(OutputNode);
			if (!InputNode) return;

			FStackNodeGroup InputGroup;
			InputGroup.StartNodes.Add(InputNode);
			InputGroup.EndNode = InputNode;
			OutGroups.Add(InputGroup);

			TArray<UNiagaraNodeFunctionCall*> Modules;
			GetModules(OutputNode, Modules);
			for (UNiagaraNodeFunctionCall* Module : Modules)
			{
				if (!Module) continue;
				UEdGraphPin* PreviousOutputPin = FindParameterMapPin(*OutGroups.Last().EndNode, EGPD_Output);
				if (!PreviousOutputPin)
				{
					OutGroups.Reset();
					return;
				}
				FStackNodeGroup ModuleGroup;
				for (UEdGraphPin* LinkedPin : PreviousOutputPin->LinkedTo)
				{
					if (!LinkedPin) continue;
					if (UNiagaraNode* LinkedNode = Cast<UNiagaraNode>(LinkedPin->GetOwningNode()))
					{
						ModuleGroup.StartNodes.Add(LinkedNode);
					}
				}
				ModuleGroup.EndNode = Module;
				OutGroups.Add(ModuleGroup);
			}

			UEdGraphPin* PreviousOutputPin = FindParameterMapPin(*OutGroups.Last().EndNode, EGPD_Output);
			if (!PreviousOutputPin)
			{
				OutGroups.Reset();
				return;
			}
			FStackNodeGroup OutputGroup;
			for (UEdGraphPin* LinkedPin : PreviousOutputPin->LinkedTo)
			{
				if (!LinkedPin) continue;
				if (UNiagaraNode* LinkedNode = Cast<UNiagaraNode>(LinkedPin->GetOwningNode()))
				{
					OutputGroup.StartNodes.Add(LinkedNode);
				}
			}
			OutputGroup.EndNode = &OutputNode;
			OutGroups.Add(OutputGroup);
		}

		bool ValidateStageStackForMutation(UNiagaraNodeOutput& OutputNode, FString& OutError)
		{
			TArray<UNiagaraNode*> ChainNodes;
			FString ChainError;
			if (!GetParameterMapChainNodes(OutputNode, ChainNodes, ChainError))
			{
				OutError = TEXT("niagara_stage_stack_invalid:") + ChainError;
				return false;
			}

			TArray<FStackNodeGroup> Groups;
			GetStackNodeGroups(OutputNode, Groups);
			if (Groups.Num() < 2)
			{
				OutError = FString::Printf(
					TEXT("niagara_stage_stack_invalid:stack_node_groups=%d"),
					Groups.Num());
				return false;
			}
			return true;
		}

		void DisconnectGroup(const FStackNodeGroup& Group, const FStackNodeGroup& PreviousGroup, const FStackNodeGroup& NextGroup)
		{
			UEdGraphPin* PreviousOutputPin = FindParameterMapPin(*PreviousGroup.EndNode, EGPD_Output);
			UEdGraphPin* GroupOutputPin = FindParameterMapPin(*Group.EndNode, EGPD_Output);
			if (!PreviousOutputPin || !GroupOutputPin) return;
			PreviousOutputPin->BreakAllPinLinks(true);
			GroupOutputPin->BreakAllPinLinks(true);
			for (UNiagaraNode* NextStartNode : NextGroup.StartNodes)
			{
				if (!NextStartNode) continue;
				if (UEdGraphPin* NextInputPin = FindParameterMapPin(*NextStartNode, EGPD_Input))
				{
					PreviousOutputPin->MakeLinkTo(NextInputPin);
				}
			}
		}

		void ConnectGroup(const FStackNodeGroup& Group, const FStackNodeGroup& PreviousGroup, const FStackNodeGroup& NextGroup)
		{
			UEdGraphPin* PreviousOutputPin = FindParameterMapPin(*PreviousGroup.EndNode, EGPD_Output);
			UEdGraphPin* GroupOutputPin = FindParameterMapPin(*Group.EndNode, EGPD_Output);
			if (!PreviousOutputPin || !GroupOutputPin) return;
			PreviousOutputPin->BreakAllPinLinks(true);
			for (UNiagaraNode* StartNode : Group.StartNodes)
			{
				if (!StartNode) continue;
				if (UEdGraphPin* InputPin = FindParameterMapPin(*StartNode, EGPD_Input))
				{
					PreviousOutputPin->MakeLinkTo(InputPin);
				}
			}
			for (UNiagaraNode* NextStartNode : NextGroup.StartNodes)
			{
				if (!NextStartNode) continue;
				if (UEdGraphPin* NextInputPin = FindParameterMapPin(*NextStartNode, EGPD_Input))
				{
					GroupOutputPin->MakeLinkTo(NextInputPin);
				}
			}
		}

		bool ScriptPathMatches(const UNiagaraNodeFunctionCall& ModuleNode, const FString& RequestedPath)
		{
			const FString TrimmedPath = RequestedPath.TrimStartAndEnd();
			const FString ActualObjectPath = ModuleNode.FunctionScript
				? ModuleNode.FunctionScript->GetPathName()
				: ModuleNode.FunctionScriptAssetObjectPath.ToString();
			if (ActualObjectPath.Equals(TrimmedPath, ESearchCase::IgnoreCase))
			{
				return true;
			}
			const FString RequestedPackagePath = FPackageName::ObjectPathToPackageName(TrimmedPath);
			const FString ActualPackagePath = ModuleNode.FunctionScript
				? ModuleNode.FunctionScript->GetOutermost()->GetName()
				: FPackageName::ObjectPathToPackageName(ActualObjectPath);
			return !RequestedPackagePath.IsEmpty()
				&& RequestedPackagePath.Equals(ActualPackagePath, ESearchCase::IgnoreCase);
		}
	}

	FString ScriptUsageToString(const ENiagaraScriptUsage Usage)
	{
		const UEnum* UsageEnum = StaticEnum<ENiagaraScriptUsage>();
		return UsageEnum ? UsageEnum->GetNameStringByValue(static_cast<int64>(Usage)) : TEXT("Unknown");
	}

	bool TryParseScriptUsage(const FString& UsageText, ENiagaraScriptUsage& OutUsage)
	{
		const FString Trimmed = UsageText.TrimStartAndEnd();
		const UEnum* UsageEnum = StaticEnum<ENiagaraScriptUsage>();
		if (Trimmed.IsEmpty() || !UsageEnum) return false;
		int64 Value = UsageEnum->GetValueByNameString(Trimmed, EGetByNameFlags::CheckAuthoredName);
		if (Value == INDEX_NONE)
		{
			Value = UsageEnum->GetValueByNameString(TEXT("ENiagaraScriptUsage::") + Trimmed, EGetByNameFlags::CheckAuthoredName);
		}
		if (Value == INDEX_NONE) Value = UsageEnum->GetValueByNameString(Trimmed, EGetByNameFlags::None);
		if (Value == INDEX_NONE) return false;
		OutUsage = static_cast<ENiagaraScriptUsage>(Value);
		return true;
	}

	FString MakeStageKey(const ENiagaraScriptUsage Usage, const FGuid& UsageId)
	{
		return FString::Printf(
			TEXT("%s|%s"),
			*ScriptUsageToString(Usage),
			*UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
	}

	bool ResolveStage(const FStageTarget& Target, FResolvedStage& OutStage, FString& OutError)
	{
		OutStage = FResolvedStage();
		OutError.Reset();
		UObject* Asset = LoadAsset(Target.AssetPath);
		if (!Asset)
		{
			OutError = TEXT("niagara_stage_asset_not_found");
			return false;
		}

		UNiagaraScriptSource* Source = nullptr;
		if (UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Asset))
		{
			FVersionedNiagaraEmitterData* Data = Emitter->GetLatestEmitterData();
			if (Data)
			{
				Source = Cast<UNiagaraScriptSource>(Data->GraphSource);
				OutStage.EmitterVersion = Data->Version.VersionGuid;
			}
			OutStage.Emitter = Emitter;
		}
		else if (UNiagaraSystem* System = Cast<UNiagaraSystem>(Asset))
		{
			OutStage.System = System;
			if (Target.bEmitterHandleIdExplicit || !Target.EmitterName.IsEmpty())
			{
				bool bMatchedHandle = false;
				for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
				{
					if (Target.bEmitterHandleIdExplicit && Handle.GetId() != Target.EmitterHandleId) continue;
					if (!Target.EmitterName.IsEmpty()
						&& !Handle.GetName().ToString().Equals(Target.EmitterName, ESearchCase::IgnoreCase)) continue;
					if (bMatchedHandle)
					{
						OutError = TEXT("niagara_emitter_handle_identity_ambiguous");
						return false;
					}
					const FVersionedNiagaraEmitter Instance = Handle.GetInstance();
					const FVersionedNiagaraEmitterData* Data = Instance.GetEmitterData();
					OutStage.Emitter = Instance.Emitter;
					OutStage.EmitterVersion = Instance.Version;
					OutStage.EmitterHandleId = Handle.GetId();
					OutStage.EmitterName = Handle.GetName().ToString();
					Source = Data ? Cast<UNiagaraScriptSource>(Data->GraphSource) : nullptr;
					bMatchedHandle = true;
				}
				if (!OutStage.Emitter)
				{
					OutError = TEXT("niagara_emitter_handle_not_found");
					return false;
				}
			}
			else
			{
				UNiagaraScript* SpawnScript = System->GetSystemSpawnScript();
				Source = SpawnScript ? Cast<UNiagaraScriptSource>(SpawnScript->GetLatestSource()) : nullptr;
			}
		}
		else
		{
			OutError = TEXT("niagara_stage_asset_type_invalid");
			return false;
		}

		UNiagaraGraph* Graph = Source ? Source->NodeGraph : nullptr;
		if (!Graph)
		{
			OutError = TEXT("niagara_stage_graph_missing");
			return false;
		}
		TArray<UNiagaraNodeOutput*> Outputs;
		GetOutputNodes(*Graph, Outputs);
		TArray<UNiagaraNodeOutput*> Matches;
		for (UNiagaraNodeOutput* Output : Outputs)
		{
			if (!Output) continue;
			if (Target.bUsageExplicit && Output->GetUsage() != Target.Usage) continue;
			if (Target.bUsageIdExplicit && Output->GetUsageId() != Target.UsageId) continue;
			Matches.Add(Output);
		}
		if (Matches.IsEmpty())
		{
			OutError = TEXT("niagara_stage_not_found");
			return false;
		}
		if (Matches.Num() != 1)
		{
			OutError = TEXT("niagara_stage_identity_ambiguous");
			return false;
		}
		OutStage.Asset = Asset;
		OutStage.ScriptSource = Source;
		OutStage.Graph = Graph;
		OutStage.OutputNode = Matches[0];
		OutStage.Usage = Matches[0]->GetUsage();
		OutStage.UsageId = Matches[0]->GetUsageId();
		OutStage.StageKey = MakeStageKey(OutStage.Usage, OutStage.UsageId);
		return true;
	}

	bool EnumerateStages(UObject& Asset, TArray<FResolvedStage>& OutStages, FString& OutError)
	{
		OutStages.Reset();
		OutError.Reset();
		if (UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(&Asset))
		{
			FVersionedNiagaraEmitterData* Data = Emitter->GetLatestEmitterData();
			AppendStagesFromSource(
				Asset,
				nullptr,
				Emitter,
				Data ? Data->Version.VersionGuid : FGuid(),
				FGuid(),
				FString(),
				Data ? Cast<UNiagaraScriptSource>(Data->GraphSource) : nullptr,
				OutStages);
		}
		else if (UNiagaraSystem* System = Cast<UNiagaraSystem>(&Asset))
		{
			UNiagaraScript* SpawnScript = System->GetSystemSpawnScript();
			AppendStagesFromSource(
				Asset,
				System,
				nullptr,
				FGuid(),
				FGuid(),
				FString(),
				SpawnScript ? Cast<UNiagaraScriptSource>(SpawnScript->GetLatestSource()) : nullptr,
				OutStages);
			for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
			{
				const FVersionedNiagaraEmitter Instance = Handle.GetInstance();
				const FVersionedNiagaraEmitterData* Data = Instance.GetEmitterData();
				AppendStagesFromSource(
					Asset,
					System,
					Instance.Emitter,
					Instance.Version,
					Handle.GetId(),
					Handle.GetName().ToString(),
					Data ? Cast<UNiagaraScriptSource>(Data->GraphSource) : nullptr,
					OutStages);
			}
		}
		else if (UNiagaraScript* Script = Cast<UNiagaraScript>(&Asset))
		{
			AppendStagesFromSource(
				Asset,
				nullptr,
				nullptr,
				Script->GetExposedVersion().VersionGuid,
				FGuid(),
				FString(),
				Cast<UNiagaraScriptSource>(Script->GetLatestSource()),
				OutStages);
		}
		else
		{
			OutError = TEXT("niagara_stage_asset_type_invalid");
			return false;
		}

		OutStages.Sort([](const FResolvedStage& Left, const FResolvedStage& Right)
		{
			const int32 EmitterCompare = Left.EmitterName.Compare(Right.EmitterName, ESearchCase::CaseSensitive);
			if (EmitterCompare != 0) return EmitterCompare < 0;
			const FString LeftHandleId = Left.EmitterHandleId.ToString(EGuidFormats::Digits);
			const FString RightHandleId = Right.EmitterHandleId.ToString(EGuidFormats::Digits);
			if (LeftHandleId != RightHandleId) return LeftHandleId < RightHandleId;
			if (Left.Usage != Right.Usage) return static_cast<int32>(Left.Usage) < static_cast<int32>(Right.Usage);
			return Left.UsageId.ToString(EGuidFormats::Digits) < Right.UsageId.ToString(EGuidFormats::Digits);
		});
		return true;
	}

	bool EnumerateEmitterOwners(
		UObject& Asset,
		TArray<FResolvedEmitterOwner>& OutOwners,
		FString& OutError)
	{
		OutOwners.Reset();
		OutError.Reset();
		if (UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(&Asset))
		{
			FVersionedNiagaraEmitterData* Data = Emitter->GetLatestEmitterData();
			if (!Data)
			{
				OutError = TEXT("niagara_emitter_version_data_missing");
				return false;
			}
			FResolvedEmitterOwner& Owner = OutOwners.AddDefaulted_GetRef();
			Owner.Asset = &Asset;
			Owner.Emitter = Emitter;
			Owner.EmitterData = Data;
			Owner.EmitterVersion = Data->Version.VersionGuid;
			Owner.ScriptSource = Cast<UNiagaraScriptSource>(Data->GraphSource);
			Owner.Graph = Owner.ScriptSource ? Owner.ScriptSource->NodeGraph : nullptr;
			return true;
		}
		UNiagaraSystem* System = Cast<UNiagaraSystem>(&Asset);
		if (!System)
		{
			OutError = TEXT("niagara_emitter_owner_asset_type_invalid");
			return false;
		}
		for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
		{
			const FVersionedNiagaraEmitter Instance = Handle.GetInstance();
			if (!Instance.Emitter) continue;
			const FVersionedNiagaraEmitterData* ConstData = Instance.GetEmitterData();
			if (!ConstData)
			{
				OutError = TEXT("niagara_emitter_version_data_missing:") + Handle.GetName().ToString();
				return false;
			}
			FResolvedEmitterOwner& Owner = OutOwners.AddDefaulted_GetRef();
			Owner.Asset = &Asset;
			Owner.System = System;
			Owner.Emitter = Instance.Emitter;
			Owner.EmitterData = const_cast<FVersionedNiagaraEmitterData*>(ConstData);
			Owner.EmitterVersion = Instance.Version;
			Owner.EmitterHandleId = Handle.GetId();
			Owner.EmitterName = Handle.GetName().ToString();
			Owner.ScriptSource = Cast<UNiagaraScriptSource>(Owner.EmitterData->GraphSource);
			Owner.Graph = Owner.ScriptSource ? Owner.ScriptSource->NodeGraph : nullptr;
		}
		return true;
	}

	bool ResolveEmitterOwner(
		const FEmitterOwnerTarget& Target,
		FResolvedEmitterOwner& OutOwner,
		FString& OutError,
		UObject* RootObject)
	{
		OutOwner = FResolvedEmitterOwner();
		OutError.Reset();
		UObject* Asset = RootObject ? RootObject : LoadAsset(Target.AssetPath);
		if (!Asset)
		{
			OutError = TEXT("niagara_emitter_owner_asset_not_found");
			return false;
		}
		TArray<FResolvedEmitterOwner> Owners;
		if (!EnumerateEmitterOwners(*Asset, Owners, OutError)) return false;
		bool bMatched = false;
		for (const FResolvedEmitterOwner& Candidate : Owners)
		{
			if (Target.bEmitterHandleIdExplicit && Candidate.EmitterHandleId != Target.EmitterHandleId) continue;
			if (!Target.EmitterName.IsEmpty()
				&& !Candidate.EmitterName.Equals(Target.EmitterName, ESearchCase::IgnoreCase)) continue;
			if (bMatched)
			{
				OutError = TEXT("niagara_emitter_owner_identity_ambiguous");
				return false;
			}
			OutOwner = Candidate;
			bMatched = true;
		}
		if (!bMatched)
		{
			OutError = TEXT("niagara_emitter_owner_not_found");
			return false;
		}
		return true;
	}

	UNiagaraNodeOutput* EnsureStageBridge(
		UNiagaraGraph& Graph,
		const ENiagaraScriptUsage Usage,
		const FGuid& UsageId,
		FString& OutError)
	{
		OutError.Reset();
		Graph.Modify();
		TArray<UNiagaraNodeOutput*> Outputs;
		GetOutputNodes(Graph, Outputs);
		UNiagaraNodeOutput* OutputNode = nullptr;
		for (UNiagaraNodeOutput* Candidate : Outputs)
		{
			if (Candidate && Candidate->GetUsage() == Usage && Candidate->GetUsageId() == UsageId)
			{
				OutputNode = Candidate;
				break;
			}
		}
		if (!OutputNode)
		{
			OutputNode = NewObject<UNiagaraNodeOutput>(&Graph, NAME_None, RF_Transactional);
			if (!OutputNode)
			{
				OutError = TEXT("niagara_stage_output_create_failed");
				return nullptr;
			}
			OutputNode->CreateNewGuid();
			OutputNode->SetUsage(Usage);
			OutputNode->SetUsageId(UsageId);
			OutputNode->Outputs = { FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("Out")) };
			OutputNode->AllocateDefaultPins();
			Graph.AddNode(OutputNode, true, false);
		}
		UEdGraphPin* OutputInput = FindParameterMapPin(*OutputNode, EGPD_Input);
		if (!OutputInput)
		{
			OutError = TEXT("niagara_stage_output_input_missing");
			return nullptr;
		}
		if (OutputInput->LinkedTo.IsEmpty())
		{
			UNiagaraNodeInput* InputNode = NewObject<UNiagaraNodeInput>(&Graph, NAME_None, RF_Transactional);
			if (!InputNode)
			{
				OutError = TEXT("niagara_stage_input_create_failed");
				return nullptr;
			}
			InputNode->Input = FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("InputMap"));
			InputNode->Usage = ENiagaraInputNodeUsage::Parameter;
			InputNode->CreateNewGuid();
			InputNode->AllocateDefaultPins();
			Graph.AddNode(InputNode, true, false);
			UEdGraphPin* InputOutput = FindParameterMapPin(*InputNode, EGPD_Output);
			if (!InputOutput)
			{
				OutError = TEXT("niagara_stage_input_output_missing");
				return nullptr;
			}
			if (const UEdGraphSchema* Schema = Graph.GetSchema())
			{
				Schema->TryCreateConnection(OutputInput, InputOutput);
			}
			if (!OutputInput->LinkedTo.Contains(InputOutput)) OutputInput->MakeLinkTo(InputOutput);
			if (!OutputInput->LinkedTo.Contains(InputOutput))
			{
				OutError = TEXT("niagara_stage_bridge_link_failed");
				return nullptr;
			}
		}
		Graph.NotifyGraphChanged();
		return OutputNode;
	}

	bool RemoveStageGraph(
		UNiagaraGraph& Graph,
		const ENiagaraScriptUsage Usage,
		const FGuid& UsageId,
		FString& OutError)
	{
		OutError.Reset();
		TArray<UNiagaraNodeOutput*> Outputs;
		GetOutputNodes(Graph, Outputs);
		for (UNiagaraNodeOutput* Output : Outputs)
		{
			if (!Output || Output->GetUsage() != Usage || Output->GetUsageId() != UsageId) continue;
			TSet<UEdGraphNode*> Visited;
			TArray<UEdGraphNode*> Nodes;
			GatherUpstreamNodes(Output, Visited, Nodes);
			for (UEdGraphNode* Node : Nodes)
			{
				if (Node) Graph.RemoveNode(Node);
			}
			Graph.NotifyGraphChanged();
			return true;
		}
		OutError = TEXT("niagara_stage_not_found");
		return false;
	}

	bool FModuleSelector::IsEmpty() const
	{
		return !NodeGuid.IsValid() && ScriptPath.IsEmpty() && Name.IsEmpty();
	}

	bool GetParameterMapChainNodes(
		UNiagaraNodeOutput& OutputNode,
		TArray<UNiagaraNode*>& OutNodes,
		FString& OutError)
	{
		OutNodes.Reset();
		OutError.Reset();
		UEdGraphNode* CurrentNode = &OutputNode;
		TSet<UEdGraphNode*> VisitedNodes;
		while (CurrentNode && !VisitedNodes.Contains(CurrentNode))
		{
			VisitedNodes.Add(CurrentNode);
			UEdGraphPin* InputPin = FindParameterMapPin(*CurrentNode, EGPD_Input);
			if (!InputPin || InputPin->LinkedTo.Num() != 1 || !IsParameterMapPin(InputPin->LinkedTo[0]))
			{
				OutError = FString::Printf(
					TEXT("niagara_parameter_map_chain_invalid:{\"node\":\"%s\",\"input_links\":%d}"),
					*CurrentNode->GetName(),
					InputPin ? InputPin->LinkedTo.Num() : 0);
				OutNodes.Reset();
				return false;
			}
			UEdGraphNode* PreviousNode = InputPin->LinkedTo[0]->GetOwningNode();
			if (Cast<UNiagaraNodeInput>(PreviousNode)) return true;
			UNiagaraNode* NiagaraNode = Cast<UNiagaraNode>(PreviousNode);
			if (!NiagaraNode)
			{
				OutError = TEXT("niagara_parameter_map_chain_node_invalid");
				OutNodes.Reset();
				return false;
			}
			OutNodes.Insert(NiagaraNode, 0);
			CurrentNode = NiagaraNode;
		}
		OutError = TEXT("niagara_parameter_map_chain_cycle_detected");
		OutNodes.Reset();
		return false;
	}

	bool ValidateGraphInputDependenciesAcyclic(UNiagaraGraph& Graph, FString& OutError)
	{
		OutError.Reset();
		TMap<const UEdGraphNode*, uint8> VisitState;
		TArray<const UEdGraphNode*> VisitStack;
		TFunction<bool(const UEdGraphNode*)> VisitNode = [&](const UEdGraphNode* Node)
		{
			if (!Node || Node->GetGraph() != &Graph) return true;
			const uint8 State = VisitState.FindRef(Node);
			if (State == 2) return true;
			if (State == 1)
			{
				const int32 CycleStart = VisitStack.IndexOfByKey(Node);
				TArray<FString> CycleNodes;
				for (int32 Index = FMath::Max(0, CycleStart); Index < VisitStack.Num(); ++Index)
				{
					CycleNodes.Add(FString::Printf(
						TEXT("%s[%s]"),
						*VisitStack[Index]->GetName(),
						*VisitStack[Index]->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
				}
				CycleNodes.Add(FString::Printf(
					TEXT("%s[%s]"),
					*Node->GetName(),
					*Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
				OutError = TEXT("niagara_graph_dependency_cycle:")
					+ FString::Join(CycleNodes, TEXT("->"));
				return false;
			}

			VisitState.Add(Node, 1);
			VisitStack.Add(Node);
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Input) continue;
				for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					if (LinkedPin && !VisitNode(LinkedPin->GetOwningNode())) return false;
				}
			}
			VisitStack.Pop();
			VisitState.Add(Node, 2);
			return true;
		};

		for (const UEdGraphNode* Node : Graph.Nodes)
		{
			if (!VisitNode(Node)) return false;
		}
		return true;
	}

	void GetModules(UNiagaraNodeOutput& OutputNode, TArray<UNiagaraNodeFunctionCall*>& OutModules)
	{
		OutModules.Reset();
		TArray<UNiagaraNode*> ChainNodes;
		FString ChainError;
		if (!GetParameterMapChainNodes(OutputNode, ChainNodes, ChainError)) return;
		for (UNiagaraNode* ChainNode : ChainNodes)
		{
			UNiagaraNodeFunctionCall* ModuleNode = Cast<UNiagaraNodeFunctionCall>(ChainNode);
			if (!ModuleNode) continue;
			const ENiagaraScriptUsage CalledUsage = ModuleNode->GetCalledUsage();
			if (CalledUsage == ENiagaraScriptUsage::Module || CalledUsage == ENiagaraScriptUsage::Function)
			{
				OutModules.Add(ModuleNode);
			}
		}
	}

	bool ResolveModule(
		UNiagaraNodeOutput& OutputNode,
		const FModuleSelector& Selector,
		UNiagaraNodeFunctionCall*& OutModuleNode,
		int32& OutModuleIndex,
		FString& OutError)
	{
		OutModuleNode = nullptr;
		OutModuleIndex = INDEX_NONE;
		OutError.Reset();
		if (Selector.IsEmpty())
		{
			OutError = TEXT("module_selector_missing");
			return false;
		}

		TArray<UNiagaraNodeFunctionCall*> Modules;
		GetModules(OutputNode, Modules);
		TArray<int32> Matches;
		for (int32 Index = 0; Index < Modules.Num(); ++Index)
		{
			UNiagaraNodeFunctionCall* Module = Modules[Index];
			if (!Module)
			{
				continue;
			}
			const bool bMatches = Selector.NodeGuid.IsValid()
				? Module->NodeGuid == Selector.NodeGuid
				: !Selector.ScriptPath.IsEmpty()
					? ScriptPathMatches(*Module, Selector.ScriptPath)
					: Module->GetFunctionName().Equals(Selector.Name, ESearchCase::IgnoreCase);
			if (bMatches)
			{
				Matches.Add(Index);
			}
		}
		if (!Matches.IsValidIndex(Selector.Occurrence))
		{
			OutError = TEXT("module_selector_not_found");
			return false;
		}
		if (Matches.Num() > 1 && !Selector.bOccurrenceExplicit)
		{
			OutError = TEXT("module_selector_ambiguous");
			return false;
		}
		OutModuleIndex = Matches[Selector.Occurrence];
		OutModuleNode = Modules[OutModuleIndex];
		return OutModuleNode != nullptr;
	}

	UNiagaraNodeFunctionCall* AddModule(
		UNiagaraNodeOutput& OutputNode,
		UNiagaraScript& ModuleScript,
		const FString& SuggestedName,
		const int32 TargetIndex,
		FString& OutError)
	{
		OutError.Reset();
		if (!ValidateStageStackForMutation(OutputNode, OutError)) return nullptr;
		UNiagaraNodeFunctionCall* AddedModule = FNiagaraStackGraphUtilities::AddScriptModuleToStack(
			&ModuleScript,
			OutputNode,
			TargetIndex,
			SuggestedName,
			ModuleScript.GetExposedVersion().VersionGuid);
		if (!AddedModule)
		{
			OutError = TEXT("add_stage_module_failed");
		}
		return AddedModule;
	}

	UNiagaraNodeFunctionCall* AddAssignmentModule(
		UNiagaraNodeOutput& OutputNode,
		const TArray<FNiagaraVariable>& AssignmentTargets,
		const int32 TargetIndex,
		FString& OutError)
	{
		OutError.Reset();
		if (!ValidateStageStackForMutation(OutputNode, OutError)) return nullptr;
		if (AssignmentTargets.IsEmpty())
		{
			OutError = TEXT("niagara_assignment_targets_missing");
			return nullptr;
		}
		TArray<FString> DefaultValues;
		DefaultValues.Reserve(AssignmentTargets.Num());
		for (const FNiagaraVariable& Target : AssignmentTargets)
		{
			if (!Target.IsValid())
			{
				OutError = TEXT("niagara_assignment_target_invalid");
				return nullptr;
			}
			DefaultValues.Add(FNiagaraConstants::GetAttributeDefaultValue(Target));
		}
		UNiagaraNodeAssignment* AddedModule = FNiagaraStackGraphUtilities::AddParameterModuleToStack(
			AssignmentTargets,
			OutputNode,
			TargetIndex,
			DefaultValues);
		if (!AddedModule)
		{
			OutError = TEXT("add_assignment_module_failed");
		}
		return AddedModule;
	}

	bool MoveModule(
		UNiagaraNodeOutput& OutputNode,
		UNiagaraNodeFunctionCall& ModuleNode,
		const int32 TargetIndex,
		int32& OutNewIndex,
		FString& OutError)
	{
		OutNewIndex = INDEX_NONE;
		OutError.Reset();
		TArray<UNiagaraNodeFunctionCall*> Modules;
		GetModules(OutputNode, Modules);
		const int32 SourceIndex = Modules.IndexOfByKey(&ModuleNode);
		if (SourceIndex == INDEX_NONE)
		{
			OutError = TEXT("stage_module_not_found");
			return false;
		}
		const int32 ClampedTargetIndex = FMath::Clamp(TargetIndex, 0, FMath::Max(0, Modules.Num() - 1));
		if (SourceIndex == ClampedTargetIndex)
		{
			OutNewIndex = SourceIndex;
			return true;
		}

		TArray<FStackNodeGroup> Groups;
		GetStackNodeGroups(OutputNode, Groups);
		const int32 SourceGroupIndex = Groups.IndexOfByPredicate([&ModuleNode](const FStackNodeGroup& Group)
		{
			return Group.EndNode == &ModuleNode;
		});
		if (SourceGroupIndex <= 0 || SourceGroupIndex >= Groups.Num() - 1)
		{
			OutError = TEXT("stage_module_group_not_found");
			return false;
		}

		const FStackNodeGroup ModuleGroup = Groups[SourceGroupIndex];
		DisconnectGroup(ModuleGroup, Groups[SourceGroupIndex - 1], Groups[SourceGroupIndex + 1]);
		TArray<FStackNodeGroup> RebuiltGroups;
		GetStackNodeGroups(OutputNode, RebuiltGroups);
		if (RebuiltGroups.Num() < 2)
		{
			ConnectGroup(ModuleGroup, Groups[SourceGroupIndex - 1], Groups[SourceGroupIndex + 1]);
			OutError = TEXT("stage_stack_invalid_after_disconnect");
			return false;
		}
		const int32 InsertGroupIndex = FMath::Clamp(ClampedTargetIndex + 1, 1, RebuiltGroups.Num() - 1);
		ConnectGroup(ModuleGroup, RebuiltGroups[InsertGroupIndex - 1], RebuiltGroups[InsertGroupIndex]);

		TArray<UNiagaraNodeFunctionCall*> UpdatedModules;
		GetModules(OutputNode, UpdatedModules);
		OutNewIndex = UpdatedModules.IndexOfByKey(&ModuleNode);
		if (OutNewIndex == INDEX_NONE)
		{
			OutError = TEXT("stage_module_missing_after_move");
			return false;
		}
		return true;
	}

	bool RemoveModule(
		UNiagaraNodeOutput& OutputNode,
		UNiagaraNodeFunctionCall& ModuleNode,
		FString& OutError)
	{
		OutError.Reset();
		TArray<FStackNodeGroup> Groups;
		GetStackNodeGroups(OutputNode, Groups);
		const int32 ModuleGroupIndex = Groups.IndexOfByPredicate([&ModuleNode](const FStackNodeGroup& Group)
		{
			return Group.EndNode == &ModuleNode;
		});
		if (ModuleGroupIndex <= 0 || ModuleGroupIndex >= Groups.Num() - 1)
		{
			OutError = TEXT("stage_module_group_not_found");
			return false;
		}

		UNiagaraGraph* Graph = ModuleNode.GetNiagaraGraph();
		if (!Graph || Graph != OutputNode.GetNiagaraGraph())
		{
			OutError = TEXT("stage_module_graph_mismatch");
			return false;
		}
		Graph->Modify();
		const FStackNodeGroup ModuleGroup = Groups[ModuleGroupIndex];
		DisconnectGroup(ModuleGroup, Groups[ModuleGroupIndex - 1], Groups[ModuleGroupIndex + 1]);

		TSet<UEdGraphNode*> VisitedNodes;
		TArray<UEdGraphNode*> TraversedNodes;
		GatherUpstreamNodes(ModuleGroup.EndNode, VisitedNodes, TraversedNodes);
		TArray<UNiagaraNode*> NodesToRemove;
		for (UEdGraphNode* TraversedNode : TraversedNodes)
		{
			if (UNiagaraNode* NiagaraNode = Cast<UNiagaraNode>(TraversedNode))
			{
				NodesToRemove.Add(NiagaraNode);
			}
		}
		if (!NodesToRemove.Contains(&ModuleNode))
		{
			ConnectGroup(ModuleGroup, Groups[ModuleGroupIndex - 1], Groups[ModuleGroupIndex + 1]);
			OutError = TEXT("stage_module_remove_traversal_invalid");
			return false;
		}
		for (UNiagaraNode* NodeToRemove : NodesToRemove)
		{
			if (NodeToRemove)
			{
				NodeToRemove->Modify();
				Graph->RemoveNode(NodeToRemove);
			}
		}
		Graph->NotifyGraphChanged();
		return true;
	}

	FString GetModuleScriptPath(const UNiagaraNodeFunctionCall& ModuleNode)
	{
		return ModuleNode.FunctionScript
			? ModuleNode.FunctionScript->GetPathName()
			: ModuleNode.FunctionScriptAssetObjectPath.ToString();
	}

	bool ResolveNiagaraTypeDefinition(
		const FString& TypeText,
		FNiagaraTypeDefinition& OutTypeDefinition,
		FString& OutError)
	{
		OutError.Reset();
		FString Normalized = TypeText.TrimStartAndEnd();
		if (Normalized.IsEmpty())
		{
			OutError = TEXT("niagara_type_missing");
			return false;
		}
		FString Token = Normalized;
		Token.ReplaceInline(TEXT(" "), TEXT(""));
		Token.ReplaceInline(TEXT("_"), TEXT(""));
		Token.ReplaceInline(TEXT("-"), TEXT(""));
		Token.ReplaceInline(TEXT("'"), TEXT(""));
		Token.ToLowerInline();
		if (Token == TEXT("bool") || Token.EndsWith(TEXT("niagarabool")))
		{
			OutTypeDefinition = FNiagaraTypeDefinition::GetBoolDef();
		}
		else if (Token == TEXT("int") || Token == TEXT("int32") || Token.EndsWith(TEXT("niagaraint")) || Token.EndsWith(TEXT("niagaraint32")))
		{
			OutTypeDefinition = FNiagaraTypeDefinition::GetIntDef();
		}
		else if (Token == TEXT("float") || Token == TEXT("float32") || Token.EndsWith(TEXT("niagarafloat")))
		{
			OutTypeDefinition = FNiagaraTypeDefinition::GetFloatDef();
		}
		else if (Token == TEXT("double") || Token == TEXT("float64") || Token.EndsWith(TEXT("niagaradouble")))
		{
			OutTypeDefinition = FNiagaraTypeHelper::GetDoubleDef();
		}
		else if (Token == TEXT("vec2") || Token == TEXT("vector2") || Token == TEXT("vector2d") || Token.EndsWith(TEXT("niagaravector2")))
		{
			OutTypeDefinition = FNiagaraTypeDefinition::GetVec2Def();
		}
		else if (Token == TEXT("vec3") || Token == TEXT("vector") || Token == TEXT("vector3") || Token.EndsWith(TEXT("niagaravector3")))
		{
			OutTypeDefinition = FNiagaraTypeDefinition::GetVec3Def();
		}
		else if (Token == TEXT("position") || Token == TEXT("niagaraposition"))
		{
			OutTypeDefinition = FNiagaraTypeDefinition::GetPositionDef();
		}
		else if (Token == TEXT("vec4") || Token == TEXT("vector4") || Token.EndsWith(TEXT("niagaravector4")))
		{
			OutTypeDefinition = FNiagaraTypeDefinition::GetVec4Def();
		}
		else if (Token == TEXT("quat") || Token == TEXT("quaternion"))
		{
			OutTypeDefinition = FNiagaraTypeDefinition::GetQuatDef();
		}
		else if (Token == TEXT("color") || Token == TEXT("linearcolor") || Token.EndsWith(TEXT("niagaracolor")))
		{
			OutTypeDefinition = FNiagaraTypeDefinition::GetColorDef();
		}
		else if (Token == TEXT("niagaraid") || Token == TEXT("id"))
		{
			OutTypeDefinition = FNiagaraTypeDefinition::GetIDDef();
		}
		else if (UClass* TypeClass = LoadObject<UClass>(nullptr, *Normalized))
		{
			OutTypeDefinition = FNiagaraTypeDefinition(TypeClass);
		}
		else if (UScriptStruct* TypeStruct = LoadObject<UScriptStruct>(nullptr, *Normalized))
		{
			OutTypeDefinition = FNiagaraTypeDefinition(TypeStruct);
		}
		else if (UEnum* TypeEnum = LoadObject<UEnum>(nullptr, *Normalized))
		{
			OutTypeDefinition = FNiagaraTypeDefinition(TypeEnum);
		}
		else
		{
			OutError = TEXT("niagara_type_unsupported:") + Normalized;
			return false;
		}
		return OutTypeDefinition.IsValid();
	}

	bool ResolveAssignmentTargetVariable(
		const FString& VariableName,
		const FString& OptionalTypeText,
		FNiagaraVariable& OutVariable,
		FString& OutError)
	{
		OutError.Reset();
		const FString NormalizedName = VariableName.TrimStartAndEnd();
		if (NormalizedName.IsEmpty())
		{
			OutError = TEXT("niagara_assignment_target_name_missing");
			return false;
		}
		const FName TargetName(*NormalizedName);
		const FNiagaraVariable* NativeVariable = nullptr;
		for (const FNiagaraVariable& Candidate : FNiagaraConstants::GetCommonParticleAttributes())
		{
			if (Candidate.GetName() == TargetName)
			{
				NativeVariable = &Candidate;
				break;
			}
		}
		if (!NativeVariable)
		{
			NativeVariable = FNiagaraConstants::GetKnownConstant(TargetName, false);
		}

		FNiagaraTypeDefinition RequestedType;
		if (!OptionalTypeText.TrimStartAndEnd().IsEmpty())
		{
			if (!ResolveNiagaraTypeDefinition(OptionalTypeText, RequestedType, OutError)) return false;
			if (NativeVariable && NativeVariable->GetType() != RequestedType)
			{
				OutError = TEXT("niagara_assignment_target_type_conflict:") + NormalizedName;
				return false;
			}
		}
		else if (NativeVariable)
		{
			RequestedType = NativeVariable->GetType();
		}
		else
		{
			OutError = TEXT("niagara_assignment_target_type_missing:") + NormalizedName;
			return false;
		}
		OutVariable = FNiagaraVariable(RequestedType, TargetName);
		return OutVariable.IsValid();
	}

	FString GetNiagaraTypeToken(const FNiagaraTypeDefinition& TypeDefinition)
	{
		if (TypeDefinition == FNiagaraTypeDefinition::GetBoolDef()) return TEXT("bool");
		if (TypeDefinition == FNiagaraTypeDefinition::GetIntDef()) return TEXT("int");
		if (TypeDefinition == FNiagaraTypeDefinition::GetFloatDef()) return TEXT("float");
		if (TypeDefinition == FNiagaraTypeHelper::GetDoubleDef()) return TEXT("double");
		if (TypeDefinition == FNiagaraTypeDefinition::GetVec2Def()) return TEXT("vector2");
		if (TypeDefinition == FNiagaraTypeDefinition::GetVec3Def()) return TEXT("vector3");
		if (TypeDefinition == FNiagaraTypeDefinition::GetPositionDef()) return TEXT("position");
		if (TypeDefinition == FNiagaraTypeDefinition::GetVec4Def()) return TEXT("vector4");
		if (TypeDefinition == FNiagaraTypeDefinition::GetQuatDef()) return TEXT("quat");
		if (TypeDefinition == FNiagaraTypeDefinition::GetColorDef()) return TEXT("linear_color");
		if (TypeDefinition == FNiagaraTypeDefinition::GetIDDef()) return TEXT("niagara_id");
		if (TypeDefinition.IsEnum() && TypeDefinition.GetEnum()) return TypeDefinition.GetEnum()->GetPathName();
		if (TypeDefinition.GetStruct()) return TypeDefinition.GetStruct()->GetPathName();
		if (TypeDefinition.GetClass()) return TypeDefinition.GetClass()->GetPathName();
		return FString();
	}

	FString NormalizeAssignmentTargetName(const FString& VariableName)
	{
		FString Normalized = VariableName.TrimStartAndEnd();
		if (Normalized.StartsWith(TEXT("Module."), ESearchCase::IgnoreCase))
		{
			Normalized.RightChopInline(7);
		}
		static const TCHAR* Namespaces[] =
		{
			TEXT("Particles"), TEXT("Emitter"), TEXT("System"), TEXT("User"),
			TEXT("Engine"), TEXT("Transient"), TEXT("Output"), TEXT("Local")
		};
		for (const TCHAR* Namespace : Namespaces)
		{
			const FString Prefix = FString(Namespace) + TEXT(".");
			if (Normalized.StartsWith(Prefix, ESearchCase::IgnoreCase)) return Normalized;
			const FString Infix = TEXT(".") + Prefix;
			const int32 InfixIndex = Normalized.Find(Infix, ESearchCase::IgnoreCase);
			if (InfixIndex != INDEX_NONE) return Normalized.Mid(InfixIndex + 1);
		}
		return Normalized;
	}

	bool BuildAssignmentTargets(
		UNiagaraNodeFunctionCall& ModuleNode,
		TSharedRef<FJsonObject> OutTargets,
		FString& OutError)
	{
		OutError.Reset();
		UNiagaraNodeAssignment* Assignment = Cast<UNiagaraNodeAssignment>(&ModuleNode);
		if (!Assignment)
		{
			OutError = TEXT("niagara_module_not_assignment");
			return false;
		}
		for (const FNiagaraVariable& Target : Assignment->GetAssignmentTargets())
		{
			const FString TypeToken = GetNiagaraTypeToken(Target.GetType());
			if (TypeToken.IsEmpty())
			{
				OutError = TEXT("niagara_assignment_target_type_unsupported:") + Target.GetName().ToString();
				return false;
			}
			OutTargets->SetStringField(NormalizeAssignmentTargetName(Target.GetName().ToString()), TypeToken);
		}
		return true;
	}

	void BuildResolvedModuleInputs(
		UNiagaraNodeFunctionCall& ModuleNode,
		TArray<FResolvedModuleInput>& OutInputs)
	{
		OutInputs.Reset();
		const UNiagaraNodeAssignment* Assignment = Cast<UNiagaraNodeAssignment>(&ModuleNode);
		TSet<FString> AssignmentTargetNames;
		TMap<FString, FString> AssignmentTargetByShortName;
		TSet<FString> AmbiguousAssignmentTargetShortNames;
		if (Assignment)
		{
			for (const FNiagaraVariable& Target : Assignment->GetAssignmentTargets())
			{
				const FString TargetName = NormalizeAssignmentTargetName(Target.GetName().ToString());
				AssignmentTargetNames.Add(LowerTrimmed(TargetName));
				FString ShortName = TargetName;
				int32 DotIndex = INDEX_NONE;
				if (ShortName.FindLastChar(TEXT('.'), DotIndex))
				{
					ShortName = ShortName.Mid(DotIndex + 1);
				}
				const FString ShortKey = LowerTrimmed(ShortName);
				if (const FString* Existing = AssignmentTargetByShortName.Find(ShortKey);
					Existing && !Existing->Equals(TargetName, ESearchCase::IgnoreCase))
				{
					AmbiguousAssignmentTargetShortNames.Add(ShortKey);
					AssignmentTargetByShortName.Remove(ShortKey);
				}
				else if (!AmbiguousAssignmentTargetShortNames.Contains(ShortKey))
				{
					AssignmentTargetByShortName.Add(ShortKey, TargetName);
				}
			}
		}
		auto NormalizeInputName = [
			&AssignmentTargetNames,
			&AssignmentTargetByShortName](const FString& InputName)
		{
			if (AssignmentTargetNames.IsEmpty()) return InputName;
			const FString TargetName = NormalizeAssignmentTargetName(InputName);
			if (AssignmentTargetNames.Contains(LowerTrimmed(TargetName)))
			{
				return TEXT("Module.") + TargetName;
			}
			FString ShortName = TargetName;
			int32 DotIndex = INDEX_NONE;
			if (ShortName.FindLastChar(TEXT('.'), DotIndex))
			{
				ShortName = ShortName.Mid(DotIndex + 1);
			}
			if (const FString* FullTarget =
					AssignmentTargetByShortName.Find(LowerTrimmed(ShortName)))
			{
				return TEXT("Module.") + *FullTarget;
			}
			return InputName;
		};
		TMap<FString, int32> IndexByFullName;
		TArray<UEdGraphPin*> VisiblePins;
		GetModuleInputPins(ModuleNode, VisiblePins);
		for (UEdGraphPin* Pin : VisiblePins)
		{
			if (!Pin) continue;
			FResolvedModuleInput Input;
			Input.FullInputName = NormalizeInputName(Pin->PinName.ToString());
			Input.ShortInputName = Input.FullInputName;
			int32 DotIndex = INDEX_NONE;
			if (Input.ShortInputName.FindLastChar(TEXT('.'), DotIndex)) Input.ShortInputName = Input.ShortInputName.Mid(DotIndex + 1);
			Input.InputVariable = UEdGraphSchema_Niagara::PinToNiagaraVariable(Pin);
			if (!Input.InputVariable.IsValid())
			{
				Input.InputVariable = FNiagaraVariable(UEdGraphSchema_Niagara::PinToTypeDefinition(Pin), Pin->PinName);
			}
			Input.VisiblePin = Pin;
			const int32 Index = OutInputs.Add(MoveTemp(Input));
			IndexByFullName.Add(LowerTrimmed(OutInputs[Index].FullInputName), Index);
		}

		TArray<FNiagaraVariable> StackInputs;
		TSet<FNiagaraVariable> HiddenInputs;
		FCompileConstantResolver ConstantResolver;
		FNiagaraStackGraphUtilities::GetStackFunctionInputs(
			ModuleNode,
			StackInputs,
			HiddenInputs,
			ConstantResolver,
			FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly,
			false);
		for (const FNiagaraVariable& Variable : StackInputs)
		{
			if (!Variable.IsValid()) continue;
			const FString FullName = NormalizeInputName(Variable.GetName().ToString());
			if (int32* ExistingIndex = IndexByFullName.Find(LowerTrimmed(FullName)))
			{
				if (!OutInputs[*ExistingIndex].InputVariable.IsValid()) OutInputs[*ExistingIndex].InputVariable = Variable;
				continue;
			}
			FResolvedModuleInput Input;
			Input.InputVariable = Variable;
			Input.FullInputName = FullName;
			Input.ShortInputName = FullName;
			int32 DotIndex = INDEX_NONE;
			if (Input.ShortInputName.FindLastChar(TEXT('.'), DotIndex)) Input.ShortInputName = Input.ShortInputName.Mid(DotIndex + 1);
			const int32 Index = OutInputs.Add(MoveTemp(Input));
			IndexByFullName.Add(LowerTrimmed(FullName), Index);
		}
		if (Assignment)
		{
			for (const FNiagaraVariable& Target : Assignment->GetAssignmentTargets())
			{
				if (!Target.IsValid()) continue;
				const FString TargetName =
					NormalizeAssignmentTargetName(Target.GetName().ToString());
				const FString FullName = TEXT("Module.") + TargetName;
				const FString FullKey = LowerTrimmed(FullName);
				if (int32* ExistingIndex = IndexByFullName.Find(FullKey))
				{
					if (!OutInputs[*ExistingIndex].InputVariable.IsValid())
					{
						OutInputs[*ExistingIndex].InputVariable = Target;
					}
					continue;
				}
				FResolvedModuleInput Input;
				Input.InputVariable = Target;
				Input.FullInputName = FullName;
				Input.ShortInputName = TargetName;
				int32 DotIndex = INDEX_NONE;
				if (Input.ShortInputName.FindLastChar(TEXT('.'), DotIndex))
				{
					Input.ShortInputName = Input.ShortInputName.Mid(DotIndex + 1);
				}
				const int32 Index = OutInputs.Add(MoveTemp(Input));
				IndexByFullName.Add(FullKey, Index);
			}
			for (FResolvedModuleInput& Input : OutInputs)
			{
				const FString InputTargetName =
					NormalizeAssignmentTargetName(Input.FullInputName);
				const FNiagaraVariable* MatchingTarget =
					Assignment->GetAssignmentTargets().FindByPredicate(
						[&InputTargetName](const FNiagaraVariable& Target)
						{
							return NormalizeAssignmentTargetName(
									   Target.GetName().ToString())
								.Equals(InputTargetName,
										ESearchCase::IgnoreCase);
						});
				if (MatchingTarget && MatchingTarget->IsValid())
				{
					Input.InputVariable = *MatchingTarget;
				}
			}
		}
	}

	UEdGraphPin* FindResolvedInputValuePin(
		UNiagaraNodeFunctionCall& ModuleNode,
		const FResolvedModuleInput& Input)
	{
		if (Input.VisiblePin)
		{
			return Input.VisiblePin;
		}
		if (!Input.InputVariable.IsValid())
		{
			return nullptr;
		}
		const FNiagaraParameterHandle Aliased =
			BuildInputOverrideHandle(
				Input.InputVariable,
				ModuleNode);
		return FindOverridePin(
			ModuleNode.GetNiagaraGraph(),
			Aliased.GetParameterHandleString());
	}

	const FResolvedModuleInput* ResolveModuleInputByName(
		const TArray<FResolvedModuleInput>& ModuleInputs,
		const FString& InputName,
		FString& OutResolvedInputName)
	{
		OutResolvedInputName.Reset();
		const FString Requested = InputName.TrimStartAndEnd();
		if (Requested.IsEmpty()) return nullptr;
		for (const FResolvedModuleInput& Input : ModuleInputs)
		{
			if (Input.FullInputName.Equals(Requested, ESearchCase::IgnoreCase))
			{
				OutResolvedInputName = Input.FullInputName;
				return &Input;
			}
		}
		const FString RequestedAssignmentName =
			NormalizeAssignmentTargetName(Requested);
		for (const FResolvedModuleInput& Input : ModuleInputs)
		{
			if (NormalizeAssignmentTargetName(Input.FullInputName).Equals(
					RequestedAssignmentName, ESearchCase::IgnoreCase))
			{
				OutResolvedInputName = Input.FullInputName;
				return &Input;
			}
		}
		const FResolvedModuleInput* Match = nullptr;
		for (const FResolvedModuleInput& Input : ModuleInputs)
		{
			if (!Input.ShortInputName.Equals(Requested, ESearchCase::IgnoreCase)) continue;
			if (Match) return nullptr;
			Match = &Input;
		}
		if (Match) OutResolvedInputName = Match->FullInputName;
		return Match;
	}

	FString BuildInputNameNotFoundError(
		const FString& InputName,
		const FString& OwnerScriptPath,
		const TArray<FString>& AvailableInputNames)
	{
		auto Quote = [](FString Value)
		{
			Value.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
			Value.ReplaceInline(TEXT("\""), TEXT("\\\""));
			return FString::Printf(TEXT("\"%s\""), *Value);
		};
		TArray<FString> Names = AvailableInputNames;
		Names.RemoveAll([](const FString& Name) { return Name.IsEmpty(); });
		Names.Sort([](const FString& Left, const FString& Right)
		{
			return Left.Compare(Right, ESearchCase::IgnoreCase) < 0;
		});
		for (int32 Index = Names.Num() - 1; Index > 0; --Index)
		{
			if (Names[Index].Equals(Names[Index - 1], ESearchCase::IgnoreCase)) Names.RemoveAt(Index);
		}
		const bool bTruncated = Names.Num() > 32;
		if (bTruncated) Names.SetNum(32);
		for (FString& Name : Names) Name = Quote(Name);
		return FString::Printf(
			TEXT("input_name_not_found:{\"requested\":%s,\"owner_script\":%s,\"available\":[%s],\"available_truncated\":%s}"),
			*Quote(InputName),
			*Quote(OwnerScriptPath),
			*FString::Join(Names, TEXT(",")),
			bTruncated ? TEXT("true") : TEXT("false"));
	}

	namespace
	{
		bool TryResolveLinkedInputParameterName(const UEdGraphPin& OverridePin, FString& OutParameterName)
		{
			OutParameterName.Reset();
			for (const UEdGraphPin* LinkedPin : OverridePin.LinkedTo)
			{
				if (!LinkedPin) continue;
				const UEdGraphNode* LinkedNode = LinkedPin->GetOwningNode();
				if (const UNiagaraNodeInput* LinkedInput = Cast<UNiagaraNodeInput>(LinkedNode))
				{
					if (!LinkedInput->Input.IsValid() || LinkedInput->Input.GetType().IsDataInterface()) continue;
					OutParameterName = LinkedInput->Input.GetName().ToString();
					if (!OutParameterName.IsEmpty()) return true;
				}
				bool bIsParameterMapGet = false;
				for (const UClass* Class = LinkedNode ? LinkedNode->GetClass() : nullptr; Class; Class = Class->GetSuperClass())
				{
					if (Class->GetName().Equals(TEXT("NiagaraNodeParameterMapGet"), ESearchCase::CaseSensitive))
					{
						bIsParameterMapGet = true;
						break;
					}
				}
				if (bIsParameterMapGet)
				{
					const FNiagaraVariable LinkedVariable = UEdGraphSchema_Niagara::PinToNiagaraVariable(LinkedPin);
					OutParameterName = LinkedVariable.IsValid()
						? LinkedVariable.GetName().ToString()
						: LinkedPin->PinName.ToString();
					if (!OutParameterName.IsEmpty()) return true;
				}
			}
			return false;
		}

		bool BuildInputOverridesInternal(
			UNiagaraNodeFunctionCall& ModuleNode,
			TSharedRef<FJsonObject> OutInputs,
			const bool bIncludeLinkedInputs,
			FString& OutError)
		{
			OutError.Reset();
			TArray<FResolvedModuleInput> Inputs;
			BuildResolvedModuleInputs(ModuleNode, Inputs);
			TMap<FString, FString> AssignmentDefaults;
			if (const UNiagaraNodeAssignment* Assignment =
					Cast<UNiagaraNodeAssignment>(&ModuleNode))
			{
				const TArray<FNiagaraVariable>& Targets =
					Assignment->GetAssignmentTargets();
				const TArray<FString>& Defaults =
					Assignment->GetAssignmentDefaults();
				for (int32 Index = 0;
					 Index < Targets.Num() && Defaults.IsValidIndex(Index);
					 ++Index)
				{
					if (!Targets[Index].IsValid()) continue;
					const FString FullName =
						TEXT("Module.") +
						NormalizeAssignmentTargetName(
							Targets[Index].GetName().ToString());
					AssignmentDefaults.Add(LowerTrimmed(FullName),
											 Defaults[Index]);
				}
			}
			UNiagaraGraph* Graph = ModuleNode.GetNiagaraGraph();
			for (const FResolvedModuleInput& Input : Inputs)
			{
				if (!Graph || !Input.InputVariable.IsValid()) continue;
				const FString OutputInputName =
					AssignmentDefaults.IsEmpty()
						? Input.FullInputName
						: NormalizeAssignmentTargetName(Input.FullInputName);
				const FNiagaraParameterHandle Aliased = BuildInputOverrideHandle(
					Input.InputVariable, ModuleNode);
				UEdGraphPin* OverridePin = FindOverridePin(Graph, Aliased.GetParameterHandleString());
				if (!OverridePin && !AssignmentDefaults.IsEmpty())
				{
					const FString TargetName =
						NormalizeAssignmentTargetName(Input.FullInputName);
					const FNiagaraVariable ModuleScopedVariable(
						Input.InputVariable.GetType(),
						FName(*(TEXT("Module.") + TargetName)));
					const FNiagaraParameterHandle ModuleScopedAliased =
						BuildInputOverrideHandle(ModuleScopedVariable, ModuleNode);
					OverridePin = FindOverridePin(
						Graph,
						ModuleScopedAliased.GetParameterHandleString());
					FString TargetShortName = TargetName;
					int32 DotIndex = INDEX_NONE;
					if (TargetShortName.FindLastChar(TEXT('.'), DotIndex))
					{
						TargetShortName = TargetShortName.Mid(DotIndex + 1);
					}
					UEdGraphPin* ShortMatch = nullptr;
					bool bShortMatchAmbiguous = false;
					for (UEdGraphNode* Node : Graph->Nodes)
					{
						if (!IsParameterMapSetNode(Node)) continue;
						for (UEdGraphPin* Pin : Node->Pins)
						{
							if (!Pin || Pin->Direction != EGPD_Input ||
								IsParameterMapPin(Pin)) continue;
							const FString CandidateName =
								NormalizeAssignmentTargetName(
									Pin->PinName.ToString());
							if (CandidateName.Equals(
									TargetName, ESearchCase::IgnoreCase))
							{
								OverridePin = Pin;
								break;
							}
							FString CandidateShortName = CandidateName;
							int32 CandidateDotIndex = INDEX_NONE;
							if (CandidateShortName.FindLastChar(
									TEXT('.'), CandidateDotIndex))
							{
								CandidateShortName =
									CandidateShortName.Mid(CandidateDotIndex + 1);
							}
							if (CandidateShortName.Equals(
									TargetShortName,
									ESearchCase::IgnoreCase))
							{
								if (ShortMatch && ShortMatch != Pin)
								{
									bShortMatchAmbiguous = true;
								}
								else
								{
									ShortMatch = Pin;
								}
							}
						}
						if (OverridePin) break;
					}
					if (!OverridePin && !bShortMatchAmbiguous)
					{
						OverridePin = ShortMatch;
					}
				}
				UEdGraphPin* ValuePin = OverridePin ? OverridePin : Input.VisiblePin;
				const FString* AssignmentDefault =
					AssignmentDefaults.Find(LowerTrimmed(Input.FullInputName));
				if (!ValuePin && !AssignmentDefault) continue;

				if (ValuePin && !ValuePin->LinkedTo.IsEmpty())
				{
					FString LinkedParameterName;
					if (!bIncludeLinkedInputs || !TryResolveLinkedInputParameterName(*ValuePin, LinkedParameterName)) continue;
					TSharedRef<FJsonObject> SourceValue = MakeShared<FJsonObject>();
					SourceValue->SetStringField(TEXT("source"), TEXT("linked_input"));
					SourceValue->SetStringField(TEXT("linked_parameter"), LinkedParameterName);
					TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
					Payload->SetObjectField(TEXT("value"), SourceValue);
					OutInputs->SetObjectField(OutputInputName, Payload);
					continue;
				}

				FString DefaultValue;
				if (OverridePin ||
					(ValuePin &&
					 ValuePin->DefaultValue != ValuePin->AutogeneratedDefaultValue))
				{
					DefaultValue = ValuePin->DefaultValue;
				}
				else if (AssignmentDefault && !AssignmentDefault->IsEmpty())
				{
					DefaultValue = *AssignmentDefault;
				}
				else
				{
					continue;
				}
				TSharedPtr<FJsonValue> Value;
				FString ValueKind;
				FString EncodeError;
				if (!EncodeLiteralValue(Input.InputVariable.GetType(), DefaultValue, Value, ValueKind, EncodeError))
				{
					if (GetLiteralValueKind(Input.InputVariable.GetType()).IsEmpty()) continue;
					OutError = EncodeError + TEXT(":") + Input.FullInputName;
					return false;
				}
				TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
				Payload->SetStringField(TEXT("value_kind"), ValueKind);
				Payload->SetField(TEXT("value"), Value);
				OutInputs->SetObjectField(OutputInputName, Payload);
			}
			return true;
		}
	}

	bool BuildLiteralInputOverrides(
		UNiagaraNodeFunctionCall& ModuleNode,
		TSharedRef<FJsonObject> OutInputs,
		FString& OutError)
	{
		return BuildInputOverridesInternal(ModuleNode, OutInputs, false, OutError);
	}

	bool BuildInputOverrides(
		UNiagaraNodeFunctionCall& ModuleNode,
		TSharedRef<FJsonObject> OutInputs,
		FString& OutError)
	{
		return BuildInputOverridesInternal(ModuleNode, OutInputs, true, OutError);
	}

	bool BuildInputValueForPin(
		const UEdGraphPin& InputPin,
		const FNiagaraTypeDefinition& InputType,
		TSharedPtr<FJsonObject>& OutPayload,
		bool& bOutHasValue,
		FString& OutError)
	{
		OutPayload.Reset();
		bOutHasValue = false;
		OutError.Reset();
		if (!InputPin.LinkedTo.IsEmpty())
		{
			FString LinkedParameterName;
			if (!TryResolveLinkedInputParameterName(InputPin, LinkedParameterName))
			{
				return true;
			}
			TSharedRef<FJsonObject> SourceValue = MakeShared<FJsonObject>();
			SourceValue->SetStringField(TEXT("source"), TEXT("linked_input"));
			SourceValue->SetStringField(TEXT("linked_parameter"), LinkedParameterName);
			TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
			Payload->SetObjectField(TEXT("value"), SourceValue);
			OutPayload = Payload;
			bOutHasValue = true;
			return true;
		}
		if (InputPin.DefaultValue == InputPin.AutogeneratedDefaultValue)
		{
			return true;
		}

		TSharedPtr<FJsonValue> Value;
		FString ValueKind;
		if (!EncodeLiteralValue(InputType, InputPin.DefaultValue, Value, ValueKind, OutError))
		{
			return false;
		}
		TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("value_kind"), ValueKind);
		Payload->SetField(TEXT("value"), Value);
		OutPayload = Payload;
		bOutHasValue = true;
		return true;
	}

	bool DoesInputValueMatchPin(
		const UEdGraphPin& InputPin,
		const FNiagaraTypeDefinition& InputType,
		const TSharedPtr<const FJsonObject>& Payload,
		bool& bOutMatches,
		FString& OutError)
	{
		bOutMatches = false;
		OutError.Reset();
		const TSharedPtr<FJsonObject>* Value = nullptr;
		FString Source;
		if (Payload.IsValid()
			&& Payload->TryGetObjectField(TEXT("value"), Value)
			&& Value && Value->IsValid()
			&& (*Value)->TryGetStringField(TEXT("source"), Source))
		{
			if (!Source.Equals(TEXT("linked_input"), ESearchCase::IgnoreCase))
			{
				OutError = TEXT("niagara_input_source_unsupported:") + Source;
				return false;
			}
			FString DesiredParameterName;
			if (!(*Value)->TryGetStringField(TEXT("linked_parameter"), DesiredParameterName)
				|| DesiredParameterName.TrimStartAndEnd().IsEmpty())
			{
				OutError = TEXT("niagara_linked_input_parameter_missing:") + InputPin.PinName.ToString();
				return false;
			}
			FString ExistingParameterName;
			bOutMatches = TryResolveLinkedInputParameterName(InputPin, ExistingParameterName)
				&& ExistingParameterName.Equals(DesiredParameterName.TrimStartAndEnd(), ESearchCase::IgnoreCase);
			return true;
		}

		FString DesiredDefaultText;
		if (!DecodeLiteralValue(InputType, Payload, DesiredDefaultText, OutError)) return false;
		bOutMatches = InputPin.LinkedTo.IsEmpty() && InputPin.DefaultValue == DesiredDefaultText;
		return true;
	}

	bool DoesInputValueMatch(
		UNiagaraNodeFunctionCall& ModuleNode,
		const FString& InputName,
		const TSharedPtr<const FJsonObject>& Payload,
		FString& OutResolvedInputName,
		bool& bOutMatches,
		FString& OutError)
	{
		bOutMatches = false;
		FNiagaraVariable Variable;
		FGuid ScriptVariableGuid;
		UEdGraphPin* InputPin = nullptr;
		if (!ResolveInputPin(
				ModuleNode,
				InputName,
				OutResolvedInputName,
				Variable,
				ScriptVariableGuid,
				InputPin,
				OutError))
		{
			return false;
		}
		if (!InputPin)
		{
			return true;
		}
		return DoesInputValueMatchPin(
			*InputPin, Variable.GetType(), Payload, bOutMatches, OutError);
	}

	bool SetLiteralInputValueForPin(
		UEdGraphPin& InputPin,
		const FNiagaraTypeDefinition& InputType,
		const TSharedPtr<const FJsonObject>& Payload,
		bool& bOutChanged,
		FString& OutError)
	{
		bOutChanged = false;
		OutError.Reset();
		FString DefaultText;
		if (!DecodeLiteralValue(InputType, Payload, DefaultText, OutError)) return false;
		if (InputPin.LinkedTo.IsEmpty() && InputPin.DefaultValue == DefaultText) return true;

		InputPin.Modify();
		if (!InputPin.LinkedTo.IsEmpty()) RemoveValueNodesForOverridePin(InputPin);
		const UEdGraphSchema_Niagara* Schema = GetDefault<UEdGraphSchema_Niagara>();
		if (!Schema)
		{
			OutError = TEXT("niagara_literal_set_default_failed:") + InputPin.PinName.ToString();
			return false;
		}
		Schema->TrySetDefaultValue(InputPin, DefaultText, true);

		TSharedPtr<FJsonValue> ReadbackValue;
		FString ReadbackKind;
		if (!EncodeLiteralValue(InputType, InputPin.DefaultValue, ReadbackValue, ReadbackKind, OutError))
		{
			OutError = TEXT("niagara_literal_readback_failed:") + InputPin.PinName.ToString() + TEXT(":") + OutError;
			return false;
		}
		if (UNiagaraGraph* Graph = Cast<UNiagaraGraph>(InputPin.GetOwningNode()->GetGraph())) Graph->NotifyGraphChanged();
		bOutChanged = true;
		return true;
	}

	bool SetLinkedInputValueForPin(
		UEdGraphPin& InputPin,
		const FNiagaraTypeDefinition& InputType,
		const FString& LinkedParameterName,
		bool& bOutChanged,
		FString& OutError)
	{
		bOutChanged = false;
		OutError.Reset();
		const FString NormalizedLinkedParameter = LinkedParameterName.TrimStartAndEnd();
		if (NormalizedLinkedParameter.IsEmpty())
		{
			OutError = TEXT("niagara_linked_input_parameter_missing");
			return false;
		}
		FString ExistingLinkedParameter;
		if (TryResolveLinkedInputParameterName(InputPin, ExistingLinkedParameter)
			&& ExistingLinkedParameter.Equals(NormalizedLinkedParameter, ESearchCase::IgnoreCase))
		{
			return true;
		}

		InputPin.Modify();
		if (!InputPin.LinkedTo.IsEmpty()) RemoveValueNodesForOverridePin(InputPin);
		const UEdGraphSchema_Niagara* Schema = GetDefault<UEdGraphSchema_Niagara>();
		if (Schema) Schema->TrySetDefaultValue(InputPin, FString(), true);
		else InputPin.DefaultValue.Reset();

		TSet<FNiagaraVariableBase> KnownParameters;
		const FNiagaraVariableBase LinkedParameter(InputType, *NormalizedLinkedParameter);
		FNiagaraStackGraphUtilities::SetLinkedParameterValueForFunctionInput(
			InputPin, LinkedParameter, KnownParameters);
		UNiagaraGraph* Graph = Cast<UNiagaraGraph>(InputPin.GetOwningNode()->GetGraph());
		if (Graph && !ValidateGraphInputDependenciesAcyclic(*Graph, OutError))
		{
			RemoveValueNodesForOverridePin(InputPin);
			return false;
		}
		FString ReadbackLinkedParameter;
		if (!TryResolveLinkedInputParameterName(InputPin, ReadbackLinkedParameter)
			|| !ReadbackLinkedParameter.Equals(NormalizedLinkedParameter, ESearchCase::IgnoreCase))
		{
			OutError = FString::Printf(
				TEXT("niagara_linked_input_apply_verification_failed:%s:readback=%s"),
				*NormalizedLinkedParameter,
				*ReadbackLinkedParameter);
			return false;
		}
		if (Graph) Graph->NotifyGraphChanged();
		bOutChanged = true;
		return true;
	}

	bool SetInputValueForPin(
		UEdGraphPin& InputPin,
		const FNiagaraTypeDefinition& InputType,
		const TSharedPtr<const FJsonObject>& Payload,
		bool& bOutChanged,
		FString& OutError)
	{
		const TSharedPtr<FJsonObject>* Value = nullptr;
		FString Source;
		if (Payload.IsValid()
			&& Payload->TryGetObjectField(TEXT("value"), Value)
			&& Value && Value->IsValid()
			&& (*Value)->TryGetStringField(TEXT("source"), Source))
		{
			if (!Source.Equals(TEXT("linked_input"), ESearchCase::IgnoreCase))
			{
				OutError = TEXT("niagara_input_source_unsupported:") + Source;
				return false;
			}
			FString LinkedParameterName;
			if (!(*Value)->TryGetStringField(TEXT("linked_parameter"), LinkedParameterName)
				|| LinkedParameterName.TrimStartAndEnd().IsEmpty())
			{
				OutError = TEXT("niagara_linked_input_parameter_missing:") + InputPin.PinName.ToString();
				return false;
			}
			return SetLinkedInputValueForPin(
				InputPin, InputType, LinkedParameterName, bOutChanged, OutError);
		}
		return SetLiteralInputValueForPin(InputPin, InputType, Payload, bOutChanged, OutError);
	}

	bool SetLiteralInputValue(
		UNiagaraNodeFunctionCall& ModuleNode,
		const FString& InputName,
		const TSharedPtr<const FJsonObject>& Payload,
		FString& OutResolvedInputName,
		bool& bOutChanged,
		FString& OutError)
	{
		bOutChanged = false;
		OutError.Reset();
		FNiagaraVariable Variable;
		FGuid ScriptVariableGuid;
		UEdGraphPin* InputPin = nullptr;
		if (!ResolveInputPin(
				ModuleNode,
				InputName,
				OutResolvedInputName,
				Variable,
				ScriptVariableGuid,
				InputPin,
				OutError))
		{
			return false;
		}

		if (!GetOrCreateInputPin(ModuleNode, Variable, ScriptVariableGuid, InputPin, OutError) || !InputPin)
		{
			return false;
		}
		return SetLiteralInputValueForPin(*InputPin, Variable.GetType(), Payload, bOutChanged, OutError);
	}

	bool SetLinkedInputValue(
		UNiagaraNodeFunctionCall& ModuleNode,
		const FString& InputName,
		const FString& LinkedParameterName,
		FString& OutResolvedInputName,
		bool& bOutChanged,
		FString& OutError)
	{
		bOutChanged = false;
		OutError.Reset();
		FNiagaraVariable Variable;
		FGuid ScriptVariableGuid;
		UEdGraphPin* InputPin = nullptr;
		if (!ResolveInputPin(
				ModuleNode,
				InputName,
				OutResolvedInputName,
				Variable,
				ScriptVariableGuid,
				InputPin,
				OutError))
		{
			return false;
		}
		if (!GetOrCreateInputPin(ModuleNode, Variable, ScriptVariableGuid, InputPin, OutError) || !InputPin)
		{
			return false;
		}
		if (Cast<UNiagaraNodeFunctionCall>(InputPin->GetOwningNode()) != nullptr)
		{
			OutError = TEXT("niagara_linked_input_not_supported_for_static_switch:") + OutResolvedInputName;
			return false;
		}

		return SetLinkedInputValueForPin(
			*InputPin, Variable.GetType(), LinkedParameterName, bOutChanged, OutError);
	}

	bool SetInputValue(
		UNiagaraNodeFunctionCall& ModuleNode,
		const FString& InputName,
		const TSharedPtr<const FJsonObject>& Payload,
		FString& OutResolvedInputName,
		bool& bOutChanged,
		FString& OutError)
	{
		const TSharedPtr<FJsonObject>* Value = nullptr;
		FString Source;
		if (Payload.IsValid()
			&& Payload->TryGetObjectField(TEXT("value"), Value)
			&& Value && Value->IsValid()
			&& (*Value)->TryGetStringField(TEXT("source"), Source))
		{
			if (!Source.Equals(TEXT("linked_input"), ESearchCase::IgnoreCase))
			{
				OutError = TEXT("niagara_input_source_unsupported:") + Source;
				return false;
			}
			FString LinkedParameterName;
			if (!(*Value)->TryGetStringField(TEXT("linked_parameter"), LinkedParameterName)
				|| LinkedParameterName.TrimStartAndEnd().IsEmpty())
			{
				OutError = TEXT("niagara_linked_input_parameter_missing:") + InputName;
				return false;
			}
			return SetLinkedInputValue(
				ModuleNode,
				InputName,
				LinkedParameterName,
				OutResolvedInputName,
				bOutChanged,
				OutError);
		}
		return SetLiteralInputValue(
			ModuleNode,
			InputName,
			Payload,
			OutResolvedInputName,
			bOutChanged,
			OutError);
	}

	UNiagaraNodeFunctionCall* FindDynamicInputForModuleInput(
		UNiagaraNodeFunctionCall& ModuleNode,
		const FString& InputName,
		FString& OutResolvedInputName)
	{
		FNiagaraVariable Variable;
		FGuid ScriptVariableGuid;
		UEdGraphPin* InputPin = nullptr;
		FString Error;
		if (!ResolveInputPin(
			ModuleNode,
			InputName,
			OutResolvedInputName,
			Variable,
			ScriptVariableGuid,
			InputPin,
			Error)
			|| !InputPin
			|| InputPin->LinkedTo.Num() != 1)
		{
			return nullptr;
		}
		UNiagaraNodeFunctionCall* DynamicInput = Cast<UNiagaraNodeFunctionCall>(InputPin->LinkedTo[0]->GetOwningNode());
		return DynamicInput && DynamicInput->GetCalledUsage() == ENiagaraScriptUsage::DynamicInput
			? DynamicInput
			: nullptr;
	}

	UNiagaraNodeFunctionCall* FindDynamicInputForPin(const UEdGraphPin& InputPin)
	{
		if (InputPin.LinkedTo.Num() != 1) return nullptr;
		UNiagaraNodeFunctionCall* DynamicInput = Cast<UNiagaraNodeFunctionCall>(InputPin.LinkedTo[0]->GetOwningNode());
		return DynamicInput && DynamicInput->GetCalledUsage() == ENiagaraScriptUsage::DynamicInput
			? DynamicInput
			: nullptr;
	}

	bool ClearInputValueNodesForPin(UEdGraphPin& InputPin, FString& OutError)
	{
		OutError.Reset();
		InputPin.Modify();
		if (!InputPin.LinkedTo.IsEmpty()) RemoveValueNodesForOverridePin(InputPin);
		if (UNiagaraGraph* Graph = Cast<UNiagaraGraph>(InputPin.GetOwningNode()->GetGraph())) Graph->NotifyGraphChanged();
		return InputPin.LinkedTo.IsEmpty();
	}

	bool ResolveDynamicInput(
		FResolvedStage& Stage,
		const FDynamicInputSelector& Selector,
		UNiagaraNodeFunctionCall*& OutDynamicInputNode,
		FString& OutResolvedInputName,
		FString& OutError)
	{
		OutDynamicInputNode = nullptr;
		OutResolvedInputName.Reset();
		OutError.Reset();
		if (!Stage.Graph || !Stage.OutputNode)
		{
			OutError = TEXT("niagara_stage_context_invalid");
			return false;
		}
		if (Selector.NodeGuid.IsValid())
		{
			for (UEdGraphNode* Node : Stage.Graph->Nodes)
			{
				UNiagaraNodeFunctionCall* Function = Cast<UNiagaraNodeFunctionCall>(Node);
				if (Function && Function->NodeGuid == Selector.NodeGuid)
				{
					OutDynamicInputNode = Function;
					break;
				}
			}
		}
		else
		{
			UNiagaraNodeFunctionCall* Module = nullptr;
			int32 ModuleIndex = INDEX_NONE;
			if (!ResolveModule(*Stage.OutputNode, Selector.Module, Module, ModuleIndex, OutError)) return false;
			FNiagaraVariable Variable;
			FGuid ScriptVariableGuid;
			UEdGraphPin* InputPin = nullptr;
			if (!ResolveInputPin(*Module, Selector.InputName, OutResolvedInputName, Variable, ScriptVariableGuid, InputPin, OutError)) return false;
			if (!InputPin || InputPin->LinkedTo.Num() != 1)
			{
				OutError = TEXT("niagara_dynamic_input_not_found");
				return false;
			}
			OutDynamicInputNode = Cast<UNiagaraNodeFunctionCall>(InputPin->LinkedTo[0]->GetOwningNode());
		}
		if (!OutDynamicInputNode || OutDynamicInputNode->GetCalledUsage() != ENiagaraScriptUsage::DynamicInput)
		{
			OutError = TEXT("niagara_dynamic_input_not_found");
			return false;
		}
		return true;
	}

	bool SetDynamicInputForPin(
		UEdGraphPin& InputPin,
		UNiagaraScript& DynamicInputScript,
		const FString& SuggestedName,
		UNiagaraNodeFunctionCall*& OutDynamicInputNode,
		FString& OutError)
	{
		OutDynamicInputNode = nullptr;
		UEdGraphNode* OwnerNode = InputPin.GetOwningNode();
		UClass* ParameterMapSetClass = StaticLoadClass(
			UNiagaraNode::StaticClass(),
			nullptr,
			TEXT(
				"/Script/NiagaraEditor."
				"NiagaraNodeParameterMapSet"));
		if (!OwnerNode
			|| !ParameterMapSetClass
			|| !OwnerNode->IsA(ParameterMapSetClass))
		{
			OutError = FString::Printf(
				TEXT(
					"niagara_dynamic_input_override_pin_invalid:"
					"owner_class=%s"),
				OwnerNode
					? *OwnerNode->GetClass()->GetPathName()
					: TEXT("null"));
			return false;
		}
		InputPin.Modify();
		if (!InputPin.LinkedTo.IsEmpty())
		{
			RemoveValueNodesForOverridePin(InputPin);
		}
		FNiagaraStackGraphUtilities::SetDynamicInputForFunctionInput(
			InputPin,
			&DynamicInputScript,
			OutDynamicInputNode,
			FGuid(),
			SuggestedName,
			DynamicInputScript.GetExposedVersion().VersionGuid);
		if (!OutDynamicInputNode)
		{
			OutError = TEXT("niagara_set_dynamic_input_failed");
			return false;
		}
		if (UNiagaraGraph* Graph = OutDynamicInputNode->GetNiagaraGraph())
		{
			if (!ValidateGraphInputDependenciesAcyclic(*Graph, OutError))
			{
				RemoveValueNodesForOverridePin(InputPin);
				OutDynamicInputNode = nullptr;
				return false;
			}
			Graph->NotifyGraphChanged();
		}
		return true;
	}

	bool ClearDynamicInputForPin(UEdGraphPin& InputPin, FString& OutError)
	{
		if (InputPin.LinkedTo.IsEmpty())
		{
			OutError = TEXT("niagara_dynamic_input_not_found");
			return false;
		}
		UEdGraphNode* OverrideNode = InputPin.GetOwningNode();
		if (!IsParameterMapSetNode(OverrideNode))
		{
			OutError = TEXT("niagara_dynamic_input_override_pin_invalid");
			return false;
		}
		UNiagaraGraph* Graph = Cast<UNiagaraGraph>(OverrideNode->GetGraph());
		InputPin.Modify();
		RemoveValueNodesForOverridePin(InputPin);
		OverrideNode->Modify();
		OverrideNode->RemovePin(&InputPin);
		if (Graph) Graph->NotifyGraphChanged();
		return true;
	}

	bool SetDynamicInput(
		UNiagaraNodeFunctionCall& ModuleNode,
		const FString& InputName,
		UNiagaraScript& DynamicInputScript,
		const FString& SuggestedName,
		UNiagaraNodeFunctionCall*& OutDynamicInputNode,
		FString& OutResolvedInputName,
		FString& OutError)
	{
		OutDynamicInputNode = nullptr;
		FNiagaraVariable Variable;
		FGuid ScriptVariableGuid;
		UEdGraphPin* InputPin = nullptr;
		if (!ResolveInputPin(ModuleNode, InputName, OutResolvedInputName, Variable, ScriptVariableGuid, InputPin, OutError)
			)
		{
			return false;
		}
		UClass* ParameterMapSetClass = StaticLoadClass(
			UNiagaraNode::StaticClass(),
			nullptr,
			TEXT(
				"/Script/NiagaraEditor."
				"NiagaraNodeParameterMapSet"));
		if (InputPin
			&& (!ParameterMapSetClass
				|| !InputPin->GetOwningNode()
					->IsA(ParameterMapSetClass)))
		{
			InputPin = nullptr;
		}
		if (!GetOrCreateInputPin(
				ModuleNode,
				Variable,
				ScriptVariableGuid,
				InputPin,
				OutError))
		{
			return false;
		}
		return SetDynamicInputForPin(*InputPin, DynamicInputScript, SuggestedName, OutDynamicInputNode, OutError);
	}

	bool InitializeDynamicInputDataInterfaces(
		UNiagaraNodeFunctionCall& DynamicInputNode,
		FString& OutInitializationSummary,
		FString& OutError)
	{
		OutInitializationSummary.Reset();
		OutError.Reset();
		UNiagaraGraph* Graph = DynamicInputNode.GetNiagaraGraph();
		if (!Graph)
		{
			OutError = TEXT("niagara_dynamic_input_graph_missing");
			return false;
		}

		TArray<FResolvedModuleInput> Inputs;
		BuildResolvedModuleInputs(DynamicInputNode, Inputs);
		int32 DataInterfaceCount = 0;
		int32 InitializedCount = 0;
		TArray<FString> Errors;
		for (const FResolvedModuleInput& Input : Inputs)
		{
			if (!Input.InputVariable.IsValid()) continue;
			const FNiagaraTypeDefinition InputType = Input.InputVariable.GetType();
			UClass* InputClass = InputType.GetClass();
			if (!InputType.IsDataInterface() || !InputClass
				|| !InputClass->IsChildOf(UNiagaraDataInterface::StaticClass())) continue;

			++DataInterfaceCount;
			const FNiagaraParameterHandle Aliased = BuildInputOverrideHandle(
				Input.InputVariable, DynamicInputNode);
			UEdGraphPin* ParameterMapInput = FindParameterMapPin(DynamicInputNode, EGPD_Input);
			if (!ParameterMapInput || ParameterMapInput->LinkedTo.Num() != 1)
			{
				Errors.Add(Input.FullInputName + TEXT(":parameter_map_input_invalid"));
				continue;
			}

			UEdGraphPin& OverridePin = FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
				DynamicInputNode, Aliased, InputType, Input.InputScriptVariableGuid, FGuid());
			if (!IsParameterMapSetNode(OverridePin.GetOwningNode()))
			{
				Errors.Add(Input.FullInputName + TEXT(":override_pin_owner_invalid"));
				continue;
			}
			if (!OverridePin.LinkedTo.IsEmpty())
			{
				++InitializedCount;
				continue;
			}

			UNiagaraDataInterface* DataInterface = nullptr;
			const FString NodeInputName = FString::Printf(
				TEXT("%s.%s"), *DynamicInputNode.GetFunctionName(), *Input.ShortInputName);
			FNiagaraStackGraphUtilities::SetDataInterfaceValueForFunctionInput(
				OverridePin, InputClass, NodeInputName, DataInterface);
			if (DataInterface) ++InitializedCount;
			else Errors.Add(Input.FullInputName + TEXT(":data_interface_create_failed"));
		}

		Graph->NotifyGraphChanged();
		OutInitializationSummary = FString::Printf(
			TEXT("data_interface_inputs:%d;initialized:%d"), DataInterfaceCount, InitializedCount);
		if (!Errors.IsEmpty())
		{
			OutInitializationSummary += TEXT(";errors:") + FString::Join(Errors, TEXT("|"));
			OutError = TEXT("niagara_dynamic_input_data_interface_initialization_failed:") + OutInitializationSummary;
			return false;
		}
		return true;
	}

	bool ResolveDynamicInputDataInterface(
		UNiagaraNodeFunctionCall& DynamicInputNode,
		const FString& InputName,
		UNiagaraNodeInput*& OutInputNode,
		UNiagaraDataInterface*& OutDataInterface,
		FString& OutResolvedInputName,
		FString& OutError)
	{
		OutInputNode = nullptr;
		OutDataInterface = nullptr;
		OutResolvedInputName.Reset();
		OutError.Reset();
		TArray<FResolvedModuleInput> Inputs;
		BuildResolvedModuleInputs(DynamicInputNode, Inputs);
		const FResolvedModuleInput* Input = ResolveModuleInputByName(Inputs, InputName, OutResolvedInputName);
		if (!Input || !Input->InputVariable.IsValid() || !Input->InputVariable.GetType().IsDataInterface())
		{
			OutError = TEXT("niagara_dynamic_input_data_interface_input_not_found:") + InputName;
			return false;
		}

		const FNiagaraParameterHandle Aliased = BuildInputOverrideHandle(
			Input->InputVariable, DynamicInputNode);
		UEdGraphPin* InputPin = Input->VisiblePin;
		if (!InputPin)
		{
			InputPin = FindOverridePin(DynamicInputNode.GetNiagaraGraph(), Aliased.GetParameterHandleString());
		}
		if (!InputPin)
		{
			OutError = TEXT("niagara_dynamic_input_data_interface_pin_not_found:") + OutResolvedInputName;
			return false;
		}
		for (UEdGraphPin* LinkedPin : InputPin->LinkedTo)
		{
			UNiagaraNodeInput* Candidate = LinkedPin
				? Cast<UNiagaraNodeInput>(LinkedPin->GetOwningNode())
				: nullptr;
			const FObjectPropertyBase* DataInterfaceProperty = Candidate
				? FindFProperty<FObjectPropertyBase>(UNiagaraNodeInput::StaticClass(), TEXT("DataInterface"))
				: nullptr;
			UNiagaraDataInterface* DataInterface = DataInterfaceProperty
				? Cast<UNiagaraDataInterface>(DataInterfaceProperty->GetObjectPropertyValue_InContainer(Candidate))
				: nullptr;
			if (!DataInterface) continue;
			OutInputNode = Candidate;
			OutDataInterface = DataInterface;
			return true;
		}
		OutError = TEXT("niagara_dynamic_input_data_interface_not_initialized:") + OutResolvedInputName;
		return false;
	}

	bool ClearDynamicInput(
		UNiagaraNodeFunctionCall& ModuleNode,
		const FString& InputName,
		FString& OutResolvedInputName,
		FString& OutError)
	{
		FNiagaraVariable Variable;
		FGuid ScriptVariableGuid;
		UEdGraphPin* InputPin = nullptr;
		if (!ResolveInputPin(ModuleNode, InputName, OutResolvedInputName, Variable, ScriptVariableGuid, InputPin, OutError)) return false;
		if (!InputPin || InputPin->LinkedTo.IsEmpty())
		{
			OutError = TEXT("niagara_dynamic_input_not_found");
			return false;
		}
		return ClearDynamicInputForPin(*InputPin, OutError);
	}
}

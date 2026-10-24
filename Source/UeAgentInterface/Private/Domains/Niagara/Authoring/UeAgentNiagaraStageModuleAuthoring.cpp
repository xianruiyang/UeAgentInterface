// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentNiagaraStageModuleAuthoring.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTypeDescriptors.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringEncodedTypes.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringJsonFields.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"

#include "AssetRegistry/AssetData.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Math/UnitConversion.h"
#include "NiagaraEmitter.h"
#include "NiagaraConstants.h"
#include "NiagaraDataInterface.h"
#include "NiagaraGraph.h"
#include "NiagaraNode.h"
#include "NiagaraNodeAssignment.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraParameterMapHistory.h"
#include "NiagaraParameterDefinitions.h"
#include "NiagaraParameterDefinitionsSubscriber.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraScriptVariable.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "NiagaraEditorModule.h"
#include "INiagaraEditorTypeUtilities.h"
#include "Modules/ModuleManager.h"
#include "EdGraphSchema_Niagara.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"

namespace UeAgentNiagaraStageModuleAuthoring
{
	namespace
	{
		UE::UAI::Contract::FDiagnostic StageFailure(const TCHAR* Code, const FString& Reason,
			const FString& Field, const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected)
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("niagara_authoring"));
			Error.PropertyPath = Field;
			if (Field.StartsWith(TEXT("/"))) Error.Source.JsonPath = Field;
			Error.ActualValue = Actual;
			Error.ActualValuePresent = Actual.IsValid();
			Error.ExpectedValue = Expected;
			Error.ExpectedValuePresent = Expected.IsValid();
			return Error;
		}

		TSharedPtr<FJsonValue> DiagnosticNumber(const double Value)
		{
			if (FMath::IsFinite(Value)) return MakeShared<FJsonValueNumber>(Value);
			return MakeShared<FJsonValueString>(FMath::IsNaN(Value) ? TEXT("NaN") : Value < 0 ? TEXT("-Infinity") : TEXT("Infinity"));
		}

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

		bool ResolveEnumValue(const UEnum& Enum, FString Text, int64& OutValue)
		{
			Text.TrimStartAndEndInline();
			if (Text.IsEmpty()) return false;
			if (Text.StartsWith(TEXT("\"")) && Text.EndsWith(TEXT("\"")) && Text.Len() > 1)
			{
				Text = Text.Mid(1, Text.Len() - 2);
			}
			int64 Value = 0;
			bool bResolved = UE::UAI::StateSemantic::TryParseInt64Decimal(Text, Value) && Enum.IsValidEnumValue(Value);
			if (!bResolved) bResolved = UeAgentAuthoringTypeDescriptors::TryResolveEnumValueName(&Enum, Text, Value, true);
			if (!bResolved)
			{
				int32 ScopeIndex = INDEX_NONE;
				if (Text.FindLastChar(TEXT(':'), ScopeIndex) && ScopeIndex + 1 < Text.Len())
				{
					const FString ShortName = Text.Mid(ScopeIndex + 1);
					bResolved = UeAgentAuthoringTypeDescriptors::TryResolveEnumValueName(&Enum, ShortName, Value, true);
				}
			}
			if (!bResolved || !Enum.IsValidEnumValue(Value)) return false;
			OutValue = Value;
			return true;
		}

		FString GetLiteralValueKind(const FNiagaraTypeDefinition& DeclaredType)
		{
			const FNiagaraTypeDefinition Type = DeclaredType.RemoveStaticDef();
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

		bool FormatLiteralNumber(const TSharedPtr<FJsonValue>& Value, bool bFloat32,
			FString& OutText, UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (!Value.IsValid() || Value->Type != EJson::Number || !FMath::IsFinite(Value->AsNumber()))
			{
				OutError = StageFailure(TEXT("niagara_literal_number_expected"), TEXT("A Niagara numeric literal requires a finite number."),
					FString(), Value && Value->Type == EJson::Number ? DiagnosticNumber(Value->AsNumber()) : Value,
					MakeShared<FJsonValueString>(TEXT("finite number")));
				return false;
			}
			double Number = Value->AsNumber();
			if (bFloat32)
			{
				float NativeValue;
				if (!UeAgentPropertyValueCodec::TryConvertFloat32(Number, NativeValue, OutError)) return false;
				Number = NativeValue;
			}
			// Use the existing round-trip formatter, not SanitizeFloat's fixed six decimals.
			return UE::UAI::StateSemantic::CanonicalJson::TryGetNumberLexeme(FJsonValueNumber(Number), OutText);
		}

		bool EncodeLiteralValue(
			const FNiagaraTypeDefinition& Type,
			const FString& DefaultText,
			TSharedPtr<FJsonValue>& OutValue,
			FString& OutKind,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutKind = GetLiteralValueKind(Type);
			if (OutKind.IsEmpty()) return false;
			ON_SCOPE_EXIT { if (!OutError.IsEmpty()) OutError.SourceKind = TEXT("native_result"); };
			// Validate the native consumer, not only our inverse text projection.
			// Preallocate: several native type utilities otherwise accept malformed text as zero.
			FNiagaraVariable NativeParsedValue(Type, NAME_None);
			NativeParsedValue.AllocateData();
			const auto Utilities = FModuleManager::LoadModuleChecked<FNiagaraEditorModule>(TEXT("NiagaraEditor")).GetTypeUtilities(Type);
			const bool bHandlesDefaults = Utilities && Utilities->CanHandlePinDefaults();
			if (!bHandlesDefaults
				|| !Utilities->SetValueFromPinDefaultString(DefaultText, NativeParsedValue))
			{
				OutError = StageFailure(TEXT("niagara_literal_native_default_invalid"),
					!Utilities ? TEXT("Niagara has no type utilities to read this native pin default.")
						: !bHandlesDefaults ? TEXT("This Niagara type does not support native pin defaults.")
						: TEXT("The Niagara type utility rejected the native pin default text."),
					TEXT("default_text"), MakeShared<FJsonValueString>(DefaultText), MakeShared<FJsonValueString>(Type.GetName()));
				return false;
			}
			if (OutKind == TEXT("bool"))
			{
				OutValue = MakeShared<FJsonValueBoolean>(NativeParsedValue.GetValue<FNiagaraBool>().GetValue());
				UeAgentAuthoringTypeDescriptors::FDescriptor NativeType;
				NativeType.Kind = TEXT("bool");
				UeAgentAuthoringEncodedTypes::FScope::Record(OutValue, NativeType, nullptr);
				return true;
			}
			if (OutKind == TEXT("int"))
			{
				OutValue = MakeShared<FJsonValueNumber>(NativeParsedValue.GetValue<FNiagaraInt32>().Value);
				UeAgentAuthoringTypeDescriptors::FDescriptor NativeType;
				NativeType.Kind = TEXT("int32");
				UeAgentAuthoringEncodedTypes::FScope::Record(OutValue, NativeType, nullptr);
				return true;
			}
			if (OutKind == TEXT("float") || OutKind == TEXT("double"))
			{
				const double Value = OutKind == TEXT("float") ? NativeParsedValue.GetValue<FNiagaraFloat>().Value
					: NativeParsedValue.GetValue<FNiagaraDouble>().Value;
				if (!FMath::IsFinite(Value)) {
					OutError = StageFailure(TEXT("niagara_literal_number_invalid"), TEXT("The native Niagara scalar default is not finite."),
					TEXT("/value"), DiagnosticNumber(Value), MakeShared<FJsonValueString>(TEXT("finite ") + OutKind));
					return false;
				}
				OutValue = MakeShared<FJsonValueNumber>(Value);
				UeAgentAuthoringTypeDescriptors::FDescriptor NativeType;
				NativeType.Kind = OutKind;
				UeAgentAuthoringEncodedTypes::FScope::Record(OutValue, NativeType, nullptr);
				return true;
			}
			if (OutKind == TEXT("enum"))
			{
				const UEnum* Enum = Type.GetEnum();
				const int64 Value = NativeParsedValue.GetValue<FNiagaraInt32>().Value;
				if (!Enum || !Enum->IsValidEnumValue(Value))
				{
					OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(
						TEXT("niagara_literal_enum_invalid"), TEXT("/value"),
						MakeShared<FJsonValueString>(Enum ? Enum->GetPathName() : TEXT("an available enum type")),
						MakeShared<FJsonValueNumber>(static_cast<double>(Value)), TEXT("niagara_authoring"));
					OutError.bRetrySafe = false;
					OutError.PublicMessage = Enum
						? TEXT("The native value is not a member of the required enum.")
						: TEXT("The enum type required to read this native value is unavailable.");
					return false;
				}
				TSharedRef<FJsonObject> Selected = MakeShared<FJsonObject>();
				Selected->SetStringField(TEXT("selected"), Enum->GetNameStringByValue(Value));
				OutValue = MakeShared<FJsonValueObject>(Selected);
				UeAgentAuthoringTypeDescriptors::FDescriptor NativeType;
				NativeType.Kind = TEXT("enum");
				NativeType.EnumPath = Enum->GetPathName();
				UeAgentAuthoringEncodedTypes::FScope::Record(OutValue, NativeType, nullptr);
				return true;
			}
			TArray<float> Components;
			if (OutKind == TEXT("vector2"))
			{
				const auto Value = NativeParsedValue.GetValue<FVector2f>(); Components = {Value.X, Value.Y};
			}
			else if (OutKind == TEXT("vector3"))
			{
				const auto Value = NativeParsedValue.GetValue<FVector3f>(); Components = {Value.X, Value.Y, Value.Z};
			}
			else if (OutKind == TEXT("vector4"))
			{
				const auto Value = NativeParsedValue.GetValue<FVector4f>(); Components = {Value.X, Value.Y, Value.Z, Value.W};
			}
			else if (OutKind == TEXT("quat"))
			{
				const auto Value = NativeParsedValue.GetValue<FQuat4f>(); Components = {Value.X, Value.Y, Value.Z, Value.W};
			}
			else if (OutKind == TEXT("linear_color"))
			{
				const auto Value = NativeParsedValue.GetValue<FLinearColor>(); Components = {Value.R, Value.G, Value.B, Value.A};
			}
			if (Components.Num() != GetComponentCount(OutKind))
			{
				OutError = StageFailure(TEXT("niagara_literal_components_invalid"),
					FString::Printf(TEXT("The native %s default has an unexpected component count."), *OutKind), TEXT("component_count"),
					MakeShared<FJsonValueNumber>(Components.Num()), MakeShared<FJsonValueNumber>(GetComponentCount(OutKind)));
				return false;
			}
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const float Component : Components)
			{
				if (!FMath::IsFinite(Component)) {
					OutError = StageFailure(TEXT("niagara_literal_components_invalid"), TEXT("A native Niagara vector component is not finite."),
						TEXT("/value/") + LexToString(Values.Num()), DiagnosticNumber(Component), MakeShared<FJsonValueString>(TEXT("finite float")));
					return false;
				}
				const auto Encoded = MakeShared<FJsonValueNumber>(Component);
				UeAgentAuthoringTypeDescriptors::FDescriptor NativeType;
				NativeType.Kind = TEXT("float");
				UeAgentAuthoringEncodedTypes::FScope::Record(Encoded, NativeType, nullptr);
				Values.Add(Encoded);
			}
			OutValue = MakeShared<FJsonValueArray>(Values);
			if (const UScriptStruct* Struct = Type.GetScriptStruct())
				UeAgentAuthoringEncodedTypes::FScope::Record(OutValue,
					UeAgentAuthoringTypeDescriptors::DescribeStruct(Struct), nullptr);
			return true;
		}

		bool DecodeLiteralValue(
			const FNiagaraTypeDefinition& Type,
			const TSharedPtr<const FJsonObject>& Payload,
			FString& OutDefaultText,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			auto FailValue = [&](const FString& Code, const FString& Field, const FString& Expected,
				const TSharedPtr<FJsonValue>& Actual) {
				OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Field,
					Expected.IsEmpty() ? TSharedPtr<FJsonValue>() : MakeShared<FJsonValueString>(Expected), Actual, TEXT("literal_decode"),
					FString::Printf(TEXT("Niagara literal field '%s' requires %s."), *Field, Expected.IsEmpty() ? TEXT("a supported literal value") : *Expected));
				return false;
			};
			const TSharedPtr<FJsonValue> Value = Payload.IsValid() ? Payload->TryGetField(TEXT("value")) : nullptr;
			if (!Value.IsValid()) { return FailValue(TEXT("niagara_literal_value_missing"), TEXT("/value"), GetLiteralValueKind(Type), nullptr); }
			const FString Kind = GetLiteralValueKind(Type);
			if (Kind.IsEmpty()) {
				OutError = StageFailure(TEXT("niagara_literal_type_unsupported"), TEXT("The input type does not support a Niagara literal value."),
					TEXT("/value"), MakeShared<FJsonValueString>(Type.GetName()), MakeShared<FJsonValueString>(TEXT("boolean, numeric, vector, quaternion, color or enum literal type")));
				return false;
			}
			FString DeclaredKind;
			if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(*Payload, {{TEXT("kind"), EJson::String}}, OutError)) return false;
			if (Payload->TryGetStringField(TEXT("kind"), DeclaredKind)
				&& !DeclaredKind.IsEmpty() && !DeclaredKind.Equals(Kind, ESearchCase::IgnoreCase))
			{
				return FailValue(TEXT("niagara_literal_value_kind_mismatch"), TEXT("/kind"), Kind, MakeShared<FJsonValueString>(DeclaredKind));
			}
			if (Kind == TEXT("bool"))
			{
				if (Value->Type != EJson::Boolean) { return FailValue(TEXT("niagara_literal_bool_expected"), TEXT("/value"), TEXT("boolean"), Value); }
				OutDefaultText = Value->AsBool() ? TEXT("true") : TEXT("false");
				return true;
			}
			if (Kind == TEXT("int"))
			{
				if (Value->Type != EJson::Number) { return FailValue(TEXT("niagara_literal_int_expected"), TEXT("/value"), TEXT("integer"), Value); }
				int64 Integer;
				if (!UeAgentPropertyValueCodec::TryReadIntegerNumber(Value, MIN_int32, MAX_int32, Integer, OutError))
					{ UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, TEXT("/value")); return false; }
				OutDefaultText = LexToString(Integer);
				return true;
			}
			if (Kind == TEXT("float") || Kind == TEXT("double"))
			{
				if (!FormatLiteralNumber(Value, Kind == TEXT("float"), OutDefaultText, OutError))
					{ UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, TEXT("/value")); return false; }
				return true;
			}
			if (Kind == TEXT("enum"))
			{
				if (Value->Type != EJson::Object) { return FailValue(TEXT("niagara_literal_enum_object_expected"), TEXT("/value"), TEXT("enum object"), Value); }
				FString Selected;
				if (!Value->AsObject()->TryGetStringField(TEXT("selected"), Selected)) { return FailValue(TEXT("niagara_literal_enum_selected_missing"), TEXT("/value/selected"), TEXT("enum member name"), Value->AsObject()->TryGetField(TEXT("selected"))); }
				const UEnum* Enum = Type.GetEnum();
				int64 EnumValue = 0;
				if (!Enum || !ResolveEnumValue(*Enum, Selected, EnumValue))
				{
					OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(
						TEXT("niagara_literal_enum_invalid"), TEXT("/value/selected"),
						MakeShared<FJsonValueString>(Enum ? Enum->GetPathName() : TEXT("an available enum type")),
						MakeShared<FJsonValueString>(Selected), TEXT("niagara_authoring"));
					OutError.PublicMessage = Enum
						? TEXT("The selected member is not defined by the required enum.")
						: TEXT("The enum type required to resolve the selected member is unavailable.");
					return false;
				}
				OutDefaultText = Enum->GetNameStringByValue(EnumValue);
				return true;
			}
			if (Value->Type != EJson::Array || Value->AsArray().Num() != GetComponentCount(Kind))
			{
				return FailValue(TEXT("niagara_literal_component_count_invalid"), TEXT("/value"),
					FString::Printf(TEXT("%s: %d components"), *Kind, GetComponentCount(Kind)), Value);
			}
			const TArray<const TCHAR*> Names = Kind == TEXT("linear_color")
				? TArray<const TCHAR*>{ TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A") }
				: TArray<const TCHAR*>{ TEXT("X"), TEXT("Y"), TEXT("Z"), TEXT("W") };
			TArray<FString> Parts;
			for (int32 Index = 0; Index < Value->AsArray().Num(); ++Index)
			{
				const TSharedPtr<FJsonValue>& Component = Value->AsArray()[Index];
				FString ComponentText;
				if (!FormatLiteralNumber(Component, true, ComponentText, OutError))
				{
					UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, TEXT("/value/") + LexToString(Index));
					return false;
				}
				Parts.Add(Kind == TEXT("vector2") || Kind == TEXT("linear_color")
					? FString::Printf(TEXT("%s=%s"), Names[Index], *ComponentText) : ComponentText);
			}
			OutDefaultText = FString::Join(Parts, TEXT(","));
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

		UEdGraphPin* FindOverridePin(UNiagaraNodeFunctionCall& ModuleNode, const FName AliasedInputName)
		{
			// Niagara's read helper is not exported. Match its owner-local lookup,
			// never a same-named pin elsewhere in the graph.
			UEdGraphPin* MapInput = FindParameterMapPin(ModuleNode, EGPD_Input);
			UEdGraphNode* OverrideNode = MapInput && MapInput->LinkedTo.Num() == 1
				? MapInput->LinkedTo[0]->GetOwningNode() : nullptr;
			if (!IsParameterMapSetNode(OverrideNode)) return nullptr;
			return OverrideNode->FindPin(AliasedInputName, EGPD_Input);
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
			UE::UAI::Contract::FDiagnostic& OutError)
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
				OutPin = FindOverridePin(ModuleNode, Aliased.GetParameterHandleString());
			}
			return true;
		}

		bool GetOrCreateInputPin(
			UNiagaraNodeFunctionCall& ModuleNode,
			const FNiagaraVariable& Variable,
			const FGuid& ScriptVariableGuid,
			UEdGraphPin*& InOutPin,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (InOutPin) return true;
			ON_SCOPE_EXIT {
				if (!OutError.IsEmpty()) {
					if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = ModuleNode.GetFunctionName();
					if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = Variable.GetName().ToString();
				}
			};
			UNiagaraGraph* Graph = ModuleNode.GetNiagaraGraph();
			if (!Graph)
			{
				OutError = StageFailure(TEXT("module_graph_missing"), TEXT("The module has no Niagara graph in which to create its input override."),
					TEXT("graph_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return false;
			}
			UEdGraphPin* ParameterMapInput = FindParameterMapPin(ModuleNode, EGPD_Input);
			if (!ParameterMapInput || ParameterMapInput->LinkedTo.Num() != 1)
			{
				OutError = StageFailure(TEXT("module_parameter_map_input_invalid"),
					ParameterMapInput ? TEXT("The module's parameter-map input must have exactly one link before creating an override.")
						: TEXT("The module has no parameter-map input pin for an override."), TEXT("parameter_map_input_links"),
					MakeShared<FJsonValueNumber>(ParameterMapInput ? ParameterMapInput->LinkedTo.Num() : 0), MakeShared<FJsonValueNumber>(1));
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
						OutError = StageFailure(TEXT("niagara_dynamic_input_override_node_create_failed"),
							SetNodeClass ? TEXT("UE could not create the dynamic input's parameter-map override node.") : TEXT("The Niagara parameter-map override node class could not be loaded."),
							TEXT("override_node_created"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
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
						OutError = StageFailure(TEXT("niagara_dynamic_input_override_node_invalid"),
							!PreviousMapOutput ? TEXT("The module's preceding parameter-map output pin is missing.")
								: !OverrideInput ? TEXT("The new override node has no parameter-map input pin.") : TEXT("The new override node has no parameter-map output pin."),
							TEXT("override_pins_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						return false;
					}
					ParameterMapInput->BreakLinkTo(PreviousMapOutput);
					PreviousMapOutput->MakeLinkTo(OverrideInput);
					OverrideOutput->MakeLinkTo(ParameterMapInput);
					if (!PreviousMapOutput->LinkedTo.Contains(OverrideInput)
						|| !OverrideOutput->LinkedTo.Contains(ParameterMapInput))
					{
						Graph->RemoveNode(OverrideNode);
						OutError = StageFailure(TEXT("niagara_dynamic_input_override_link_failed"), TEXT("The dynamic input override node could not be connected into the parameter-map chain."),
							TEXT("override_connected"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
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

		bool GetStackNodeGroups(UNiagaraNodeOutput& OutputNode, TArray<FStackNodeGroup>& OutGroups, UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutGroups.Reset();
			UNiagaraNodeInput* InputNode = FindStackInputNode(OutputNode);
			if (!InputNode) {
				OutError = StageFailure(TEXT("niagara_stage_input_missing"), TEXT("The Niagara stack has no reachable parameter-map input node."),
					TEXT("stack_input_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.Stage = TEXT("stage_validation");
				return false;
			}

			FStackNodeGroup InputGroup;
			InputGroup.StartNodes.Add(InputNode);
			InputGroup.EndNode = InputNode;
			OutGroups.Add(InputGroup);

			TArray<UNiagaraNodeFunctionCall*> Modules;
			if (!GetModules(OutputNode, Modules, OutError)) return false;
			for (UNiagaraNodeFunctionCall* Module : Modules)
			{
				if (!Module) continue;
				UEdGraphPin* PreviousOutputPin = FindParameterMapPin(*OutGroups.Last().EndNode, EGPD_Output);
				if (!PreviousOutputPin)
				{
					const FString GroupNode = OutGroups.Last().EndNode->GetName();
					OutGroups.Reset();
					OutError = StageFailure(TEXT("niagara_stage_output_pin_missing"), TEXT("A Niagara stack group has no parameter-map output pin."),
						TEXT("parameter_map_output_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					OutError.CandidateName = GroupNode;
					OutError.Stage = TEXT("stage_validation");
					return false;
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
				const FString GroupNode = OutGroups.Last().EndNode->GetName();
				OutGroups.Reset();
				OutError = StageFailure(TEXT("niagara_stage_output_pin_missing"), TEXT("The last Niagara stack group has no parameter-map output pin."),
					TEXT("parameter_map_output_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.CandidateName = GroupNode;
				OutError.Stage = TEXT("stage_validation");
				return false;
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
        return true;
		}

		bool ValidateStageStackForMutation(UNiagaraNodeOutput& OutputNode, UE::UAI::Contract::FDiagnostic& OutError)
		{
			TArray<UNiagaraNode*> ChainNodes;
			UE::UAI::Contract::FDiagnostic ChainError;
			if (!GetParameterMapChainNodes(OutputNode, ChainNodes, ChainError))
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("niagara_stage_stack_invalid"), TEXT("The Niagara stage parameter-map chain is invalid."), TEXT("stage_validation"));
				OutError.PublicCause = MakeShared<UE::UAI::Contract::FDiagnostic>(ChainError);
				return false;
			}

			TArray<FStackNodeGroup> Groups;
			if (!GetStackNodeGroups(OutputNode, Groups, OutError)) return false;
			if (Groups.Num() < 2)
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("niagara_stage_stack_invalid"), FString::Printf(TEXT("A Niagara stage requires at least two stack groups; found %d."), Groups.Num()), TEXT("stage_validation"));
				OutError.PropertyPath = TEXT("stack_group_count");
				OutError.ActualValue = MakeShared<FJsonValueNumber>(Groups.Num());
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("at least 2"));
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
		int64 Value = 0;
		if (!UeAgentAuthoringTypeDescriptors::TryResolveEnumValueName(UsageEnum, Trimmed, Value, true)
			&& !UeAgentAuthoringTypeDescriptors::TryResolveEnumValueName(UsageEnum, TEXT("ENiagaraScriptUsage::") + Trimmed, Value, true)) return false;
		OutUsage = static_cast<ENiagaraScriptUsage>(Value);
		return true;
	}

	FString GetStageOwnerId(const FResolvedStage& Stage)
	{
		return Stage.Emitter
			? (Stage.EmitterHandleId.IsValid()
				? Stage.EmitterHandleId.ToString(EGuidFormats::DigitsWithHyphensLower)
				: FString(TEXT("standalone")))
			: FString(TEXT("system"));
	}

	TMap<FString, FString> BuildStageIdentitySelectors(const FResolvedStage& Stage, const FString& CanonicalAssetPath)
	{
		TMap<FString, FString> Selectors = {
			{TEXT("asset_path"), CanonicalAssetPath},
			{TEXT("stage_owner_id"), GetStageOwnerId(Stage)},
			{TEXT("usage"), ScriptUsageToString(Stage.Usage)},
			{TEXT("usage_id"), Stage.UsageId.ToString(EGuidFormats::DigitsWithHyphensLower)}
		};
		if (!Stage.EmitterName.IsEmpty()) Selectors.Add(TEXT("emitter_name"), Stage.EmitterName);
		if (Stage.EmitterHandleId.IsValid())
			Selectors.Add(TEXT("emitter_handle_id"), Stage.EmitterHandleId.ToString(EGuidFormats::DigitsWithHyphensLower));
		return Selectors;
	}

	FString MakeStageKey(const ENiagaraScriptUsage Usage, const FGuid& UsageId)
	{
		return FString::Printf(
			TEXT("%s|%s"),
			*ScriptUsageToString(Usage),
			*UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
	}

	bool ResolveStage(const FStageTarget& Target, FResolvedStage& OutStage, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutStage = FResolvedStage();
		OutError.Reset();
		ON_SCOPE_EXIT {
			if (!OutError.IsEmpty()) {
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Target.AssetPath;
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Target.EmitterName;
			}
		};
		UObject* Asset = LoadAsset(Target.AssetPath);
		if (!Asset)
		{
			OutError = StageFailure(TEXT("niagara_stage_asset_not_found"), TEXT("The asset containing the requested Niagara stage could not be loaded."),
				TEXT("asset_path"), MakeShared<FJsonValueString>(Target.AssetPath), MakeShared<FJsonValueString>(TEXT("loadable Niagara system or emitter")));
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
						OutError = StageFailure(TEXT("niagara_emitter_handle_identity_ambiguous"), TEXT("At least two emitter handles match the requested emitter selector."),
							TEXT("observed_matching_emitters"), MakeShared<FJsonValueNumber>(2), MakeShared<FJsonValueNumber>(1));
						OutError.InternalContext = Target.EmitterHandleId.ToString();
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
					OutError = StageFailure(bMatchedHandle ? TEXT("niagara_emitter_instance_missing") : TEXT("niagara_emitter_handle_not_found"),
						bMatchedHandle ? TEXT("The selected emitter handle has no emitter instance.") : TEXT("No emitter handle matches the requested emitter selector."),
						bMatchedHandle ? TEXT("emitter_instance_available") : TEXT("matching_emitters"),
						MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueNumber>(1));
					if (bMatchedHandle) OutError.SourceKind = TEXT("native_result");
					OutError.InternalContext = Target.EmitterHandleId.ToString();
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
			OutError = StageFailure(TEXT("niagara_stage_asset_type_invalid"), TEXT("Stage resolution requires a Niagara system or emitter asset."),
				TEXT("asset_type"), MakeShared<FJsonValueString>(Asset->GetClass()->GetName()), MakeShared<FJsonValueString>(TEXT("NiagaraSystem or NiagaraEmitter")));
			return false;
		}

		UNiagaraGraph* Graph = Source ? Source->NodeGraph : nullptr;
		if (!Graph)
		{
			OutError = StageFailure(TEXT("niagara_stage_graph_missing"), TEXT("The selected Niagara stage owner has no editable Niagara graph."),
				TEXT("graph_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.SourceKind = TEXT("native_result");
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
			OutError = StageFailure(TEXT("niagara_stage_not_found"),
				FString::Printf(TEXT("No Niagara stage matches usage '%s'%s."), Target.bUsageExplicit ? *StaticEnum<ENiagaraScriptUsage>()->GetNameStringByValue(static_cast<int64>(Target.Usage)) : TEXT("any"),
					Target.bUsageIdExplicit ? TEXT(" and the supplied usage_id") : TEXT("")), TEXT("matching_stages"),
				MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueNumber>(1));
			OutError.InternalContext = Target.UsageId.ToString();
			return false;
		}
		if (Matches.Num() != 1)
		{
			OutError = StageFailure(TEXT("niagara_stage_identity_ambiguous"), TEXT("The stage selector must identify exactly one Niagara stage."),
				TEXT("matching_stages"), MakeShared<FJsonValueNumber>(Matches.Num()), MakeShared<FJsonValueNumber>(1));
			OutError.InternalContext = Target.UsageId.ToString();
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

	bool EnumerateStages(UObject& Asset, TArray<FResolvedStage>& OutStages, UE::UAI::Contract::FDiagnostic& OutError)
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
			OutError = StageFailure(TEXT("niagara_stage_asset_type_invalid"), TEXT("Stage enumeration requires a Niagara system, emitter or script asset."),
				TEXT("asset_type"), MakeShared<FJsonValueString>(Asset.GetClass()->GetName()), MakeShared<FJsonValueString>(TEXT("NiagaraSystem, NiagaraEmitter or NiagaraScript")));
			OutError.SourceKind = TEXT("native_result");
			OutError.InternalContext = Asset.GetPathName();
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutOwners.Reset();
		OutError.Reset();
		if (UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(&Asset))
		{
			FVersionedNiagaraEmitterData* Data = Emitter->GetLatestEmitterData();
			if (!Data)
			{
				OutError = StageFailure(TEXT("niagara_emitter_version_data_missing"), TEXT("The emitter has no latest-version data to enumerate its stage owners."),
					TEXT("emitter_version_data_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.SourceKind = TEXT("native_result");
				OutError.InternalContext = Asset.GetPathName();
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
			OutError = StageFailure(TEXT("niagara_emitter_owner_asset_type_invalid"), TEXT("Emitter owner enumeration requires a Niagara system or emitter asset."),
				TEXT("asset_type"), MakeShared<FJsonValueString>(Asset.GetClass()->GetName()), MakeShared<FJsonValueString>(TEXT("NiagaraSystem or NiagaraEmitter")));
			OutError.SourceKind = TEXT("native_result");
			OutError.InternalContext = Asset.GetPathName();
			return false;
		}
		for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
		{
			const FVersionedNiagaraEmitter Instance = Handle.GetInstance();
			if (!Instance.Emitter) continue;
			const FVersionedNiagaraEmitterData* ConstData = Instance.GetEmitterData();
			if (!ConstData)
			{
				OutError = StageFailure(TEXT("niagara_emitter_version_data_missing"), TEXT("The selected system emitter instance has no version data."),
					TEXT("emitter_version_data_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.CandidateName = Handle.GetName().ToString();
				OutError.SourceKind = TEXT("native_result");
				OutError.InternalContext = Asset.GetPathName();
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
		UE::UAI::Contract::FDiagnostic& OutError,
		UObject* RootObject)
	{
		OutOwner = FResolvedEmitterOwner();
		OutError.Reset();
		ON_SCOPE_EXIT {
			if (!OutError.IsEmpty()) {
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Target.AssetPath;
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Target.EmitterName;
			}
		};
		UObject* Asset = RootObject ? RootObject : LoadAsset(Target.AssetPath);
		if (!Asset)
		{
			OutError = StageFailure(TEXT("niagara_emitter_owner_asset_not_found"), TEXT("The asset containing the requested emitter owner could not be loaded."),
				TEXT("asset_path"), MakeShared<FJsonValueString>(Target.AssetPath), MakeShared<FJsonValueString>(TEXT("loadable Niagara system or emitter")));
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
				OutError = StageFailure(TEXT("niagara_emitter_owner_identity_ambiguous"), TEXT("At least two emitter owners match the requested selector."),
					TEXT("observed_matching_emitters"), MakeShared<FJsonValueNumber>(2), MakeShared<FJsonValueNumber>(1));
				OutError.InternalContext = Target.EmitterHandleId.ToString();
				return false;
			}
			OutOwner = Candidate;
			bMatched = true;
		}
		if (!bMatched)
		{
			OutError = StageFailure(TEXT("niagara_emitter_owner_not_found"), TEXT("No emitter owner matches the requested selector."),
				TEXT("matching_emitters"), MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueNumber>(1));
			OutError.InternalContext = Target.EmitterHandleId.ToString();
			return false;
		}
		return true;
	}

	UNiagaraNodeOutput* EnsureStageBridge(
		UNiagaraGraph& Graph,
		const ENiagaraScriptUsage Usage,
		const FGuid& UsageId,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		ON_SCOPE_EXIT {
			if (!OutError.IsEmpty()) {
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = StaticEnum<ENiagaraScriptUsage>()->GetNameStringByValue(static_cast<int64>(Usage));
				if (OutError.InternalContext.IsEmpty()) OutError.InternalContext = Graph.GetPathName() + TEXT(" usage_id=") + UsageId.ToString();
			}
		};
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
				OutError = StageFailure(TEXT("niagara_stage_output_create_failed"), TEXT("UE could not create the Niagara stage output node."),
					TEXT("stage_output_created"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
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
			OutError = StageFailure(TEXT("niagara_stage_output_input_missing"), TEXT("The Niagara stage output node has no parameter-map input pin."),
				TEXT("parameter_map_input_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return nullptr;
		}
		if (OutputInput->LinkedTo.IsEmpty())
		{
			UNiagaraNodeInput* InputNode = NewObject<UNiagaraNodeInput>(&Graph, NAME_None, RF_Transactional);
			if (!InputNode)
			{
				OutError = StageFailure(TEXT("niagara_stage_input_create_failed"), TEXT("UE could not create the Niagara stage parameter-map input node."),
					TEXT("stage_input_created"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
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
				OutError = StageFailure(TEXT("niagara_stage_input_output_missing"), TEXT("The new Niagara input node has no parameter-map output pin."),
					TEXT("parameter_map_output_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return nullptr;
			}
			if (const UEdGraphSchema* Schema = Graph.GetSchema())
			{
				Schema->TryCreateConnection(OutputInput, InputOutput);
			}
			if (!OutputInput->LinkedTo.Contains(InputOutput)) OutputInput->MakeLinkTo(InputOutput);
			if (!OutputInput->LinkedTo.Contains(InputOutput))
			{
				OutError = StageFailure(TEXT("niagara_stage_bridge_link_failed"), TEXT("The parameter-map input and output nodes could not be connected."),
					TEXT("stage_bridge_connected"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
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
		UE::UAI::Contract::FDiagnostic& OutError)
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
		OutError = StageFailure(TEXT("niagara_stage_not_found"), TEXT("The Niagara graph contains no stage with the requested usage and usage_id to remove."),
			TEXT("matching_stages"), MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueNumber>(1));
		OutError.CandidateName = StaticEnum<ENiagaraScriptUsage>()->GetNameStringByValue(static_cast<int64>(Usage));
		OutError.InternalContext = Graph.GetPathName() + TEXT(" usage_id=") + UsageId.ToString();
		return false;
	}

	bool FModuleSelector::IsEmpty() const
	{
		return !NodeGuid.IsValid() && ScriptPath.IsEmpty() && Name.IsEmpty();
	}

	bool GetParameterMapChainNodes(
		UNiagaraNodeOutput& OutputNode,
		TArray<UNiagaraNode*>& OutNodes,
		UE::UAI::Contract::FDiagnostic& OutError)
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
				OutError = StageFailure(TEXT("niagara_parameter_map_chain_invalid"),
					!InputPin ? TEXT("The stack node has no parameter-map input pin.")
						: InputPin->LinkedTo.Num() != 1 ? TEXT("The stack node must have exactly one parameter-map input link.")
						: TEXT("The linked stack pin is not a parameter-map pin."), TEXT("parameter_map_input_links"),
					MakeShared<FJsonValueNumber>(InputPin ? InputPin->LinkedTo.Num() : 0), MakeShared<FJsonValueNumber>(1));
				if (InputPin && InputPin->LinkedTo.Num() == 1) {
					OutError.PropertyPath = TEXT("linked_pin_is_parameter_map");
					OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
					OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				}
				OutError.CandidateName = CurrentNode->GetName();
				OutError.Stage = TEXT("stage_validation");
				OutNodes.Reset();
				return false;
			}
			UEdGraphNode* PreviousNode = InputPin->LinkedTo[0]->GetOwningNode();
			if (Cast<UNiagaraNodeInput>(PreviousNode)) return true;
			UNiagaraNode* NiagaraNode = Cast<UNiagaraNode>(PreviousNode);
			if (!NiagaraNode)
			{
				OutError = StageFailure(TEXT("niagara_parameter_map_chain_node_invalid"), TEXT("The parameter-map chain links to a missing or non-Niagara node."),
					TEXT("linked_node_type"), PreviousNode ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(PreviousNode->GetClass()->GetName())) : nullptr,
					MakeShared<FJsonValueString>(TEXT("NiagaraNode")));
				OutError.CandidateName = CurrentNode->GetName();
				OutNodes.Reset();
				return false;
			}
			OutNodes.Insert(NiagaraNode, 0);
			CurrentNode = NiagaraNode;
		}
		OutError = StageFailure(TEXT("niagara_parameter_map_chain_cycle_detected"),
			CurrentNode ? TEXT("The parameter-map chain revisits a node before reaching a stack input.") : TEXT("The parameter-map chain ends before reaching a stack input."),
			TEXT("acyclic_chain_reaches_input"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
		if (CurrentNode) OutError.CandidateName = CurrentNode->GetName();
		OutNodes.Reset();
		return false;
	}

	bool ValidateGraphInputDependenciesAcyclic(UNiagaraGraph& Graph, UE::UAI::Contract::FDiagnostic& OutError)
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
				TArray<TSharedPtr<FJsonValue>> PublicCycle;
				for (int32 Index = FMath::Max(0, CycleStart); Index < VisitStack.Num(); ++Index)
				{
					CycleNodes.Add(FString::Printf(
						TEXT("%s[%s]"),
						*VisitStack[Index]->GetName(),
						*VisitStack[Index]->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
					const auto* Function = Cast<UNiagaraNodeFunctionCall>(VisitStack[Index]);
					PublicCycle.Add(MakeShared<FJsonValueString>(Function ? Function->GetFunctionName() : VisitStack[Index]->GetName()));
				}
				CycleNodes.Add(FString::Printf(
					TEXT("%s[%s]"),
					*Node->GetName(),
					*Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
				const auto* ClosingFunction = Cast<UNiagaraNodeFunctionCall>(Node);
				PublicCycle.Add(MakeShared<FJsonValueString>(ClosingFunction ? ClosingFunction->GetFunctionName() : Node->GetName()));
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("niagara_graph_dependency_cycle"), TEXT("The Niagara graph contains a cyclic input dependency."), TEXT("graph_validation"));
				OutError.PropertyPath = TEXT("dependency_path");
				OutError.ActualValue = MakeShared<FJsonValueArray>(MoveTemp(PublicCycle));
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("acyclic input dependencies"));
				OutError.CyclePath = MoveTemp(CycleNodes);
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

	bool GetModules(UNiagaraNodeOutput& OutputNode, TArray<UNiagaraNodeFunctionCall*>& OutModules, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutModules.Reset();
		TArray<UNiagaraNode*> ChainNodes;
		
		if (!GetParameterMapChainNodes(OutputNode, ChainNodes, OutError)) return false;
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
        return true;
	}

	bool ResolveModule(
		UNiagaraNodeOutput& OutputNode,
		const FModuleSelector& Selector,
		UNiagaraNodeFunctionCall*& OutModuleNode,
		int32& OutModuleIndex,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutModuleNode = nullptr;
		OutModuleIndex = INDEX_NONE;
		OutError.Reset();
		ON_SCOPE_EXIT {
			if (!OutError.IsEmpty()) {
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Selector.Name.IsEmpty() ? Selector.ScriptPath : Selector.Name;
				if (OutError.InternalContext.IsEmpty()) OutError.InternalContext = Selector.NodeGuid.ToString();
			}
		};
		if (Selector.IsEmpty())
		{
			OutError = StageFailure(TEXT("module_selector_missing"), TEXT("A module selector requires a node identity, script path or module name."),
				TEXT("module_selector_present"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}

		TArray<UNiagaraNodeFunctionCall*> Modules;
		if (!GetModules(OutputNode, Modules, OutError)) return false;
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
			OutError = StageFailure(TEXT("module_selector_not_found"),
				FString::Printf(TEXT("Requested module occurrence %d is outside the %d matching module(s)."), Selector.Occurrence, Matches.Num()),
				TEXT("occurrence"), MakeShared<FJsonValueNumber>(Selector.Occurrence),
				MakeShared<FJsonValueString>(Matches.IsEmpty() ? TEXT("an existing matching module") : FString::Printf(TEXT("0 through %d"), Matches.Num() - 1)));
			return false;
		}
		if (Matches.Num() > 1 && !Selector.bOccurrenceExplicit)
		{
			OutError = StageFailure(TEXT("module_selector_ambiguous"), TEXT("Multiple modules match; supply an occurrence to select one."),
				TEXT("matching_modules"), MakeShared<FJsonValueNumber>(Matches.Num()), MakeShared<FJsonValueNumber>(1));
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
		UE::UAI::Contract::FDiagnostic& OutError)
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
			OutError = StageFailure(TEXT("add_stage_module_failed"),
				FString::Printf(TEXT("UE could not add module '%s' at requested stack index %d."), *ModuleScript.GetName(), TargetIndex),
				TEXT("module_created"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.CandidateName = SuggestedName.IsEmpty() ? ModuleScript.GetName() : SuggestedName;
			OutError.InternalContext = ModuleScript.GetPathName();
		}
		return AddedModule;
	}

	UNiagaraNodeFunctionCall* AddAssignmentModule(
		UNiagaraNodeOutput& OutputNode,
		const TArray<FNiagaraVariable>& AssignmentTargets,
		const int32 TargetIndex,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		if (!ValidateStageStackForMutation(OutputNode, OutError)) return nullptr;
		if (AssignmentTargets.IsEmpty())
		{
			OutError = StageFailure(TEXT("niagara_assignment_targets_missing"), TEXT("An assignment module requires at least one parameter target."),
				TEXT("assignment_targets"), MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("one or more targets")));
			return nullptr;
		}
		TArray<FString> DefaultValues;
		DefaultValues.Reserve(AssignmentTargets.Num());
		for (const FNiagaraVariable& Target : AssignmentTargets)
		{
			if (!Target.IsValid())
			{
				OutError = StageFailure(TEXT("niagara_assignment_target_invalid"), TEXT("An assignment target requires a valid Niagara parameter name and type."),
					TEXT("type"), MakeShared<FJsonValueString>(Target.GetType().GetName()), MakeShared<FJsonValueString>(TEXT("valid Niagara parameter type")));
				OutError.CandidateName = Target.GetName().ToString();
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
			OutError = StageFailure(TEXT("add_assignment_module_failed"),
				FString::Printf(TEXT("UE could not create an assignment module with %d target(s) at stack index %d."), AssignmentTargets.Num(), TargetIndex),
				TEXT("assignment_module_created"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
		}
		return AddedModule;
	}

	bool MoveModule(
		UNiagaraNodeOutput& OutputNode,
		UNiagaraNodeFunctionCall& ModuleNode,
		const int32 TargetIndex,
		int32& OutNewIndex,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutNewIndex = INDEX_NONE;
		OutError.Reset();
		ON_SCOPE_EXIT { if (!OutError.IsEmpty() && OutError.CandidateName.IsEmpty()) OutError.CandidateName = ModuleNode.GetFunctionName(); };
		TArray<UNiagaraNodeFunctionCall*> Modules;
		if (!GetModules(OutputNode, Modules, OutError)) return false;
		const int32 SourceIndex = Modules.IndexOfByKey(&ModuleNode);
		if (SourceIndex == INDEX_NONE)
		{
			OutError = StageFailure(TEXT("stage_module_not_found"), TEXT("The module to move is absent from this stage stack."),
				TEXT("module_in_stage"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}
		const int32 ClampedTargetIndex = FMath::Clamp(TargetIndex, 0, FMath::Max(0, Modules.Num() - 1));
		if (SourceIndex == ClampedTargetIndex)
		{
			OutNewIndex = SourceIndex;
			return true;
		}

		TArray<FStackNodeGroup> Groups;
		if (!GetStackNodeGroups(OutputNode, Groups, OutError)) return false;
		const int32 SourceGroupIndex = Groups.IndexOfByPredicate([&ModuleNode](const FStackNodeGroup& Group)
		{
			return Group.EndNode == &ModuleNode;
		});
		if (SourceGroupIndex <= 0 || SourceGroupIndex >= Groups.Num() - 1)
		{
			OutError = StageFailure(TEXT("stage_module_group_not_found"), TEXT("The module to move has no valid interior stack group."),
				TEXT("module_group_index"), MakeShared<FJsonValueNumber>(SourceGroupIndex),
				MakeShared<FJsonValueString>(FString::Printf(TEXT("interior group index greater than 0 and less than %d"), Groups.Num() - 1)));
			return false;
		}

		const FStackNodeGroup ModuleGroup = Groups[SourceGroupIndex];
		DisconnectGroup(ModuleGroup, Groups[SourceGroupIndex - 1], Groups[SourceGroupIndex + 1]);
		TArray<FStackNodeGroup> RebuiltGroups;
		if (!GetStackNodeGroups(OutputNode, RebuiltGroups, OutError)) { ConnectGroup(ModuleGroup, Groups[SourceGroupIndex - 1], Groups[SourceGroupIndex + 1]); return false; }
		if (RebuiltGroups.Num() < 2)
		{
			ConnectGroup(ModuleGroup, Groups[SourceGroupIndex - 1], Groups[SourceGroupIndex + 1]);
			OutError = StageFailure(TEXT("stage_stack_invalid_after_disconnect"), TEXT("Disconnecting the module left fewer than the two required stack boundary groups."),
				TEXT("remaining_stack_groups"), MakeShared<FJsonValueNumber>(RebuiltGroups.Num()), MakeShared<FJsonValueString>(TEXT("at least 2")));
			return false;
		}
		const int32 InsertGroupIndex = FMath::Clamp(ClampedTargetIndex + 1, 1, RebuiltGroups.Num() - 1);
		ConnectGroup(ModuleGroup, RebuiltGroups[InsertGroupIndex - 1], RebuiltGroups[InsertGroupIndex]);

		TArray<UNiagaraNodeFunctionCall*> UpdatedModules;
		if (!GetModules(OutputNode, UpdatedModules, OutError)) return false;
		OutNewIndex = UpdatedModules.IndexOfByKey(&ModuleNode);
		if (OutNewIndex == INDEX_NONE)
		{
			OutError = StageFailure(TEXT("stage_module_missing_after_move"),
				FString::Printf(TEXT("The module disappeared from the stack after moving from index %d to %d."), SourceIndex, ClampedTargetIndex),
				TEXT("module_in_stage"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}
		return true;
	}

	bool RemoveModule(
		UNiagaraNodeOutput& OutputNode,
		UNiagaraNodeFunctionCall& ModuleNode,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		ON_SCOPE_EXIT { if (!OutError.IsEmpty() && OutError.CandidateName.IsEmpty()) OutError.CandidateName = ModuleNode.GetFunctionName(); };
		TArray<FStackNodeGroup> Groups;
		if (!GetStackNodeGroups(OutputNode, Groups, OutError)) return false;
		const int32 ModuleGroupIndex = Groups.IndexOfByPredicate([&ModuleNode](const FStackNodeGroup& Group)
		{
			return Group.EndNode == &ModuleNode;
		});
		if (ModuleGroupIndex <= 0 || ModuleGroupIndex >= Groups.Num() - 1)
		{
			OutError = StageFailure(TEXT("stage_module_group_not_found"), TEXT("The module to remove has no valid interior stack group."),
				TEXT("module_group_index"), MakeShared<FJsonValueNumber>(ModuleGroupIndex),
				MakeShared<FJsonValueString>(FString::Printf(TEXT("interior group index greater than 0 and less than %d"), Groups.Num() - 1)));
			return false;
		}

		UNiagaraGraph* Graph = ModuleNode.GetNiagaraGraph();
		if (!Graph || Graph != OutputNode.GetNiagaraGraph())
		{
			OutError = StageFailure(TEXT("stage_module_graph_mismatch"),
				Graph ? TEXT("The module and stage output belong to different Niagara graphs.") : TEXT("The module has no Niagara graph to remove it from."),
				TEXT("module_graph_matches_stage"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
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
			OutError = StageFailure(TEXT("stage_module_remove_traversal_invalid"), TEXT("The stack removal traversal did not include the selected module."),
				TEXT("module_in_removal_set"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		FString Normalized = TypeText.TrimStartAndEnd();
		if (Normalized.IsEmpty())
		{
			OutError = StageFailure(TEXT("niagara_type_missing"), TEXT("A non-empty Niagara type name is required."),
				TEXT("/type"), MakeShared<FJsonValueString>(TypeText), MakeShared<FJsonValueString>(TEXT("non-empty Niagara type name")));
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
			OutTypeDefinition = FNiagaraTypeDefinition(
				FNiagaraTypeHelper::FindNiagaraFriendlyTopLevelStruct(TypeStruct, ENiagaraStructConversion::UserFacing));
		}
		else if (UEnum* TypeEnum = LoadObject<UEnum>(nullptr, *Normalized))
		{
			OutTypeDefinition = FNiagaraTypeDefinition(TypeEnum);
		}
		else
		{
			OutError = StageFailure(TEXT("niagara_type_unsupported"), TEXT("The Niagara type name or reflected type path could not be resolved."),
				TEXT("/type"), MakeShared<FJsonValueString>(TypeText),
				MakeShared<FJsonValueString>(TEXT("bool, int32, float, double, vector2/3/4, position, quat, color, niagaraid, or a loadable class/struct/enum path")));
			return false;
		}
		// Match native load/editor semantics before decoding bytes. Unfriendly
		// scalar/vector types otherwise change declaration on reload while their
		// old byte layout remains in the parameter store.
		if (UScriptStruct* Struct = OutTypeDefinition.GetScriptStruct(); Struct && !OutTypeDefinition.IsEnum())
			OutTypeDefinition = FNiagaraTypeDefinition(
				FNiagaraTypeHelper::FindNiagaraFriendlyTopLevelStruct(Struct, ENiagaraStructConversion::UserFacing));
		if (!OutTypeDefinition.IsValid()) OutError = StageFailure(TEXT("niagara_type_unsupported"),
			TEXT("The resolved reflected type has no valid Niagara type representation."), TEXT("/type"),
			MakeShared<FJsonValueString>(TypeText), MakeShared<FJsonValueString>(TEXT("valid Niagara type")));
		return OutTypeDefinition.IsValid();
	}

	bool ResolveAssignmentTargetVariable(
		const FString& VariableName,
		const FString& OptionalTypeText,
		FNiagaraVariable& OutVariable,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		const FString NormalizedName = VariableName.TrimStartAndEnd();
		ON_SCOPE_EXIT { if (!OutError.IsEmpty() && OutError.CandidateName.IsEmpty()) OutError.CandidateName = NormalizedName; };
		if (NormalizedName.IsEmpty())
		{
			OutError = StageFailure(TEXT("niagara_assignment_target_name_missing"), TEXT("An assignment target requires a non-empty parameter name."),
				TEXT("name"), MakeShared<FJsonValueString>(VariableName), MakeShared<FJsonValueString>(TEXT("non-empty parameter name")));
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
				OutError = StageFailure(TEXT("niagara_assignment_target_type_conflict"), TEXT("The requested assignment type differs from the built-in parameter type."),
					TEXT("/type"), MakeShared<FJsonValueString>(RequestedType.GetName()), MakeShared<FJsonValueString>(NativeVariable->GetType().GetName()));
				return false;
			}
		}
		else if (NativeVariable)
		{
			RequestedType = NativeVariable->GetType();
		}
		else
		{
			OutError = StageFailure(TEXT("niagara_assignment_target_type_missing"), TEXT("A parameter without a known built-in declaration requires an explicit Niagara type."),
				TEXT("/type"), MakeShared<FJsonValueString>(OptionalTypeText), MakeShared<FJsonValueString>(TEXT("explicit Niagara type")));
			return false;
		}
		OutVariable = FNiagaraVariable(RequestedType, TargetName);
		if (!OutVariable.IsValid()) OutError = StageFailure(TEXT("niagara_assignment_target_invalid"),
			TEXT("The resolved assignment parameter has no valid Niagara name/type identity."), TEXT("/type"),
			MakeShared<FJsonValueString>(RequestedType.GetName()), MakeShared<FJsonValueString>(TEXT("valid Niagara parameter type")));
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		UNiagaraNodeAssignment* Assignment = Cast<UNiagaraNodeAssignment>(&ModuleNode);
		if (!Assignment)
		{
			OutError = StageFailure(TEXT("niagara_module_not_assignment"), TEXT("Assignment targets can be read only from an assignment module."),
				TEXT("module_type"), MakeShared<FJsonValueString>(ModuleNode.GetClass()->GetName()), MakeShared<FJsonValueString>(TEXT("NiagaraNodeAssignment")));
			OutError.CandidateName = ModuleNode.GetFunctionName();
			OutError.SourceKind = TEXT("native_result");
			return false;
		}
		for (const FNiagaraVariable& Target : Assignment->GetAssignmentTargets())
		{
			const FString TypeToken = GetNiagaraTypeToken(Target.GetType());
			if (TypeToken.IsEmpty())
			{
				OutError = StageFailure(TEXT("niagara_assignment_target_type_unsupported"), TEXT("The assignment target's native type has no serializable Niagara type token."),
					TEXT("type"), MakeShared<FJsonValueString>(Target.GetType().GetName()), MakeShared<FJsonValueString>(TEXT("representable Niagara type")));
				OutError.CandidateName = Target.GetName().ToString();
				OutError.SourceKind = TEXT("native_result");
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
						OutInputs[*ExistingIndex].InputVariable = FNiagaraVariable(Target.GetType(), FName(*FullName));
					}
					continue;
				}
				FResolvedModuleInput Input;
				Input.InputVariable = FNiagaraVariable(Target.GetType(), FName(*FullName));
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
					// Assignment targets name their outputs; the callable inputs live in
					// Module scope. Preserve that scope for native override aliasing.
					Input.InputVariable = FNiagaraVariable(MatchingTarget->GetType(), FName(*Input.FullInputName));
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
		return FindOverridePin(ModuleNode, Aliased.GetParameterHandleString());
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

	UE::UAI::Contract::FDiagnostic BuildInputNameNotFoundError(
		const FString& InputName, const FString& OwnerScriptPath, const TArray<FString>& AvailableInputNames)
	{
		auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("input_name_not_found"),
			FString::Printf(TEXT("Input '%s' was not found on the Niagara script."), *InputName), TEXT("input_resolution"));
		Diagnostic.CandidateName = InputName;
		Diagnostic.PropertyPath = InputName;
		Diagnostic.TargetPath = OwnerScriptPath;
		TArray<FString> Names = AvailableInputNames;
		Names.RemoveAll([](const FString& Name) { return Name.IsEmpty(); });
		Names.Sort([](const FString& Left, const FString& Right) { return Left.Compare(Right, ESearchCase::IgnoreCase) < 0; });
		for (int32 Index = Names.Num() - 1; Index > 0; --Index)
			if (Names[Index].Equals(Names[Index - 1], ESearchCase::IgnoreCase)) Names.RemoveAt(Index);
		TArray<TSharedPtr<FJsonValue>> Choices;
		for (const FString& Name : Names) Choices.Add(MakeShared<FJsonValueString>(Name));
		Diagnostic.CandidateChoices = MakeShared<FJsonValueArray>(MoveTemp(Choices));
		return Diagnostic;
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

		const UNiagaraScriptVariable* FindInputScriptVariable(UNiagaraNodeFunctionCall& Module, const FResolvedModuleInput& Input)
		{
			const auto* Source = Module.GetFunctionScriptSource();
			const auto* Metadata = Source && Source->NodeGraph ? Source->NodeGraph->GetAllMetaData().Find(Input.InputVariable) : nullptr;
			return Metadata ? Metadata->Get() : nullptr;
		}

		const FNiagaraVariableMetaData* FindInputMetadata(
			const UNiagaraNodeAssignment* Assignment, const FResolvedModuleInput& Input,
			const UNiagaraScriptVariable* ScriptVariable, const UObject* OwningAsset)
		{
			if (!Assignment) return ScriptVariable ? &ScriptVariable->Metadata : nullptr;
			const FNiagaraVariable* Target = Assignment->GetAssignmentTargets().FindByPredicate(
				[&](const FNiagaraVariable& Variable) {
					return NormalizeAssignmentTargetName(Variable.GetName().ToString()) == NormalizeAssignmentTargetName(Input.FullInputName);
				});
			if (!Target) return nullptr;
			if (FNiagaraConstants::IsNiagaraConstant(*Target)) return FNiagaraConstants::GetConstantMetaData(*Target);
			// Match the native stack's SystemAsset/EmitterAsset subscriber branch.
			// Ordinary function inputs always use their selected defining graph above.
			const INiagaraParameterDefinitionsSubscriber* Subscriber = nullptr;
			if (const auto* System = Cast<UNiagaraSystem>(OwningAsset)) Subscriber = System;
			else if (const auto* Emitter = Cast<UNiagaraEmitter>(OwningAsset)) Subscriber = Emitter;
			if (!Subscriber) return nullptr;
			for (auto* Base : Subscriber->GetSubscribedParameterDefinitions())
			{
				auto* Definitions = Cast<UNiagaraParameterDefinitions>(Base);
				if (!Definitions) continue;
				if (const auto* Variable = Definitions->GetScriptVariable(*Target)) return &Variable->Metadata;
			}
			return nullptr;
		}

		bool TryGetLiteralScriptDefault(const FResolvedModuleInput& Input, const UNiagaraScriptVariable* Variable, FString& Text)
		{
			if (!Variable) return false;
			if (Variable->GetIsStaticSwitch())
			{
				const int32 Default = Variable->GetStaticSwitchDefaultValue();
				Text = GetLiteralValueKind(Input.InputVariable.GetType()) == TEXT("bool")
					? (Default != 0 ? TEXT("true") : TEXT("false")) : LexToString(Default);
				return true;
			}
			if (Variable->DefaultMode != ENiagaraDefaultMode::Value || !Variable->GetDefaultValueData()
				|| Variable->GetDefaultValueVariant().GetNumBytes() != Input.InputVariable.GetSizeInBytes()) return false;
			FNiagaraVariable Default = Input.InputVariable;
			Default.SetData(Variable->GetDefaultValueData());
			return GetDefault<UEdGraphSchema_Niagara>()->TryGetPinDefaultValueFromNiagaraVariable(Default, Text);
		}

		bool BuildInputOverridesInternal(
			UNiagaraNodeFunctionCall& ModuleNode,
			TSharedRef<FJsonObject> OutInputs,
			const bool bIncludeLinkedInputs,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutError.Reset();
			TArray<FResolvedModuleInput> Inputs;
			BuildResolvedModuleInputs(ModuleNode, Inputs);
			UeAgentAuthoringEncodedTypes::FScope NativeProjection;
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
				UEdGraphPin* OverridePin = FindOverridePin(ModuleNode, Aliased.GetParameterHandleString());
				UEdGraphPin* ValuePin = OverridePin ? OverridePin : Input.VisiblePin;
				const FString* AssignmentDefault =
					AssignmentDefaults.Find(LowerTrimmed(Input.FullInputName));

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
				bool bOmitted = false;
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
					// Native script literals prove the effective value even when
					// public authoring exports only overrides. Computed/unknown
					// defaults remain absent and acquire no comparison permission.
					const auto* Variable = FindInputScriptVariable(ModuleNode, Input);
					if (!TryGetLiteralScriptDefault(Input, Variable, DefaultValue)) continue;
					if (Variable->GetIsStaticSwitch() && ValuePin && !ValuePin->DefaultValue.IsEmpty())
						DefaultValue = ValuePin->DefaultValue;
					bOmitted = true;
				}
				TSharedPtr<FJsonValue> Value;
				FString ValueKind;
				UE::UAI::Contract::FDiagnostic EncodeError;
				if (!EncodeLiteralValue(Input.InputVariable.GetType(), DefaultValue, Value, ValueKind, EncodeError))
				{
					if (GetLiteralValueKind(Input.InputVariable.GetType()).IsEmpty()) continue;
					OutError = EncodeError;
					if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Input.FullInputName;
					if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = Input.FullInputName;
					return false;
				}
				TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
				Payload->SetStringField(TEXT("kind"), ValueKind);
				Payload->SetField(TEXT("value"), Value);
				UeAgentAuthoringEncodedTypes::FScope::SetNativeField(OutInputs, OutputInputName,
					MakeShared<FJsonValueObject>(Payload), bOmitted
						? UeAgentAuthoringEncodedTypes::EFieldEmission::NativeDefault
						: UeAgentAuthoringEncodedTypes::EFieldEmission::Always);
			}
			// The native read retains every proven literal. The shared projector
			// derives the sparse view for both authoring capture and graph queries.
			const TSharedPtr<FJsonValue> Native = MakeShared<FJsonValueObject>(OutInputs);
			const auto Projected = NativeProjection.ProjectAuthoringValue(Native);
			if (Projected != Native)
			{
				OutInputs->Values = Projected->AsObject()->Values;
				UeAgentAuthoringEncodedTypes::FScope::Copy(Projected, Native);
			}
			return true;
		}
	}

	TSharedRef<FJsonObject> BuildModuleInputSchema(
		UNiagaraNodeFunctionCall& ModuleNode, const UObject* OwningAsset)
	{
		const auto Schema = MakeShared<FJsonObject>();
		TArray<FResolvedModuleInput> Inputs;
		BuildResolvedModuleInputs(ModuleNode, Inputs);
		const auto* Assignment = Cast<UNiagaraNodeAssignment>(&ModuleNode);
		for (const auto& Input : Inputs)
		{
			const auto& Type = Input.InputVariable.GetType();
			const auto Descriptor = MakeShared<FJsonObject>();
			Descriptor->SetStringField(TEXT("type"), GetNiagaraTypeToken(Type));
			const auto Contract = MakeShared<FJsonObject>();
			const FString Kind = GetLiteralValueKind(Type);
			Contract->SetBoolField(TEXT("literal_supported"), !Kind.IsEmpty());
			if (!Kind.IsEmpty()) Contract->SetStringField(TEXT("kind"), Kind);
			if (const auto Candidates = UeAgentAuthoringTypeDescriptors::BuildEnumCandidates(Type.GetEnum(), true))
			{
				for (const auto& Field : Candidates->Values) Contract->SetField(Field.Key, Field.Value);
			}
			FString DefaultText;
			bool bHasLiteral = false;
			const UNiagaraScriptVariable* ScriptVariable = FindInputScriptVariable(ModuleNode, Input);
			if (const auto* Metadata = FindInputMetadata(Assignment, Input, ScriptVariable, OwningAsset))
			{
				const FString Description = Metadata->Description.ToString();
				if (!Description.IsEmpty()) Contract->SetStringField(TEXT("description"), Description);
				if (Metadata->DisplayUnit != EUnit::Unspecified)
					Contract->SetStringField(TEXT("display_unit"), FUnitConversion::GetUnitDisplayString(Metadata->DisplayUnit));
			}
			if (Assignment)
			{
				const auto& Targets = Assignment->GetAssignmentTargets();
				const auto& Defaults = Assignment->GetAssignmentDefaults();
				for (int32 Index = 0; Index < Targets.Num(); ++Index)
					if (Defaults.IsValidIndex(Index) && NormalizeAssignmentTargetName(Input.FullInputName)
						== NormalizeAssignmentTargetName(Targets[Index].GetName().ToString()))
					{
						DefaultText = Defaults[Index];
						bHasLiteral = !DefaultText.IsEmpty();
						break;
					}
			}
			else bHasLiteral = TryGetLiteralScriptDefault(Input, ScriptVariable, DefaultText);
			if (!Assignment && ScriptVariable && ScriptVariable->DefaultMode == ENiagaraDefaultMode::Binding && ScriptVariable->DefaultBinding.IsValid())
			{
				const auto Source = MakeShared<FJsonObject>();
				Source->SetStringField(TEXT("source"), TEXT("linked_input"));
				Source->SetStringField(TEXT("linked_parameter"), ScriptVariable->DefaultBinding.GetName().ToString());
				const auto Payload = MakeShared<FJsonObject>();
				Payload->SetObjectField(TEXT("value"), Source);
				Contract->SetObjectField(TEXT("default"), Payload);
				Contract->SetStringField(TEXT("default_source"), TEXT("script_binding"));
				Descriptor->SetObjectField(TEXT("contract"), Contract);
				Schema->SetObjectField(Input.FullInputName, Descriptor);
				continue;
			}
			TSharedPtr<FJsonValue> DefaultValue;
			FString DefaultKind;
			UE::UAI::Contract::FDiagnostic EncodeError;
			if (bHasLiteral && EncodeLiteralValue(Type, DefaultText, DefaultValue, DefaultKind, EncodeError))
			{
				const auto Payload = MakeShared<FJsonObject>();
				Payload->SetStringField(TEXT("kind"), DefaultKind);
				Payload->SetField(TEXT("value"), DefaultValue);
				Contract->SetObjectField(TEXT("default"), Payload);
				Contract->SetStringField(TEXT("default_source"), Assignment ? TEXT("assignment") : TEXT("script_literal"));
			}
			else
			{
				// A computed script default is not a zero-valued literal template.
				Contract->SetStringField(TEXT("default_source"), ScriptVariable && ScriptVariable->DefaultMode == ENiagaraDefaultMode::Custom
					? TEXT("script_graph") : TEXT("script_default"));
			}
			Descriptor->SetObjectField(TEXT("contract"), Contract);
			Schema->SetObjectField(Assignment ? NormalizeAssignmentTargetName(Input.FullInputName) : Input.FullInputName, Descriptor);
		}
		return Schema;
	}

	bool BuildLiteralInputOverrides(
		UNiagaraNodeFunctionCall& ModuleNode,
		TSharedRef<FJsonObject> OutInputs,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		return BuildInputOverridesInternal(ModuleNode, OutInputs, false, OutError);
	}

	bool BuildInputOverrides(
		UNiagaraNodeFunctionCall& ModuleNode,
		TSharedRef<FJsonObject> OutInputs,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		return BuildInputOverridesInternal(ModuleNode, OutInputs, true, OutError);
	}

	bool BuildInputValueForPin(
		const UEdGraphPin& InputPin,
		const FNiagaraTypeDefinition& InputType,
		TSharedPtr<FJsonObject>& OutPayload,
		bool& bOutHasValue,
		UE::UAI::Contract::FDiagnostic& OutError)
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
		const bool bOmitted = InputPin.DefaultValue == InputPin.AutogeneratedDefaultValue;
		if (bOmitted && InputPin.DefaultValue.IsEmpty()) return true;

		TSharedPtr<FJsonValue> Value;
		FString ValueKind;
		if (!EncodeLiteralValue(InputType, InputPin.DefaultValue, Value, ValueKind, OutError))
		{
			if (OutError.IsEmpty()) OutError = StageFailure(TEXT("niagara_literal_type_unsupported"),
				TEXT("This pin's native type cannot be represented as a Niagara literal."), TEXT("type"),
				MakeShared<FJsonValueString>(InputType.GetName()), MakeShared<FJsonValueString>(TEXT("supported literal type")));
			OutError.SourceKind = TEXT("native_result");
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = InputPin.PinName.ToString();
			return false;
		}
		TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("kind"), ValueKind);
		Payload->SetField(TEXT("value"), Value);
		OutPayload = Payload;
		bOutHasValue = !bOmitted;
		return true;
	}

	bool DoesInputValueMatchPin(
		const UEdGraphPin& InputPin,
		const FNiagaraTypeDefinition& InputType,
		const TSharedPtr<const FJsonObject>& Payload,
		bool& bOutMatches,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		bOutMatches = false;
		OutError.Reset();
		ON_SCOPE_EXIT {
			if (Payload && !OutError.IsEmpty()) UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Payload, TEXT("input_verify"));
		};
		const TSharedPtr<FJsonObject>* Value = nullptr;
		FString Source;
		if (Payload.IsValid()
			&& Payload->TryGetObjectField(TEXT("value"), Value)
			&& Value && Value->IsValid()
			&& (*Value)->TryGetStringField(TEXT("source"), Source))
		{
			if (!Source.Equals(TEXT("linked_input"), ESearchCase::IgnoreCase))
			{
				OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("niagara_input_source_unsupported"),
					TEXT("/value/source"), MakeShared<FJsonValueString>(TEXT("linked_input")),
					MakeShared<FJsonValueString>(Source), TEXT("niagara_authoring"));
				OutError.PublicMessage = TEXT("Unsupported Niagara input source.");
				OutError.bRetrySafe = false;
				return false;
			}
			FString DesiredParameterName;
			if (!(*Value)->TryGetStringField(TEXT("linked_parameter"), DesiredParameterName)
				|| DesiredParameterName.TrimStartAndEnd().IsEmpty())
			{
				OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("niagara_linked_input_parameter_missing"),
					TEXT("/value/linked_parameter"), MakeShared<FJsonValueString>(TEXT("non-empty parameter name")),
					(*Value)->TryGetField(TEXT("linked_parameter")), TEXT("niagara_authoring"));
				OutError.PublicMessage = TEXT("A linked input requires a non-empty parameter name.");
				OutError.ActualValuePresent = (*Value)->HasField(TEXT("linked_parameter"));
				OutError.bRetrySafe = false;
				return false;
			}
			FString ExistingParameterName;
			bOutMatches = TryResolveLinkedInputParameterName(InputPin, ExistingParameterName)
				&& ExistingParameterName.Equals(DesiredParameterName.TrimStartAndEnd(), ESearchCase::IgnoreCase);
			return true;
		}

		FString DesiredDefaultText;
		if (!DecodeLiteralValue(InputType, Payload, DesiredDefaultText, OutError))
		{
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = InputPin.PinName.ToString();
			return false;
		}
		bOutMatches = InputPin.LinkedTo.IsEmpty() && InputPin.DefaultValue == DesiredDefaultText;
		return true;
	}

	bool DoesInputValueMatch(
		UNiagaraNodeFunctionCall& ModuleNode,
		const FString& InputName,
		const TSharedPtr<const FJsonObject>& Payload,
		FString& OutResolvedInputName,
		bool& bOutMatches,
		UE::UAI::Contract::FDiagnostic& OutError)
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		bOutChanged = false;
		OutError.Reset();
		ON_SCOPE_EXIT {
			if (Payload && !OutError.IsEmpty()) UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Payload, TEXT("literal_apply"));
		};
		FString DefaultText;
		if (!DecodeLiteralValue(InputType, Payload, DefaultText, OutError))
		{
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = InputPin.PinName.ToString();
			return false;
		}
		if (InputPin.LinkedTo.IsEmpty() && InputPin.DefaultValue == DefaultText) return true;

		InputPin.Modify();
		if (!InputPin.LinkedTo.IsEmpty()) RemoveValueNodesForOverridePin(InputPin);
		const UEdGraphSchema_Niagara* Schema = GetDefault<UEdGraphSchema_Niagara>();
		if (!Schema)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("niagara_literal_set_default_failed"),
				TEXT("The Niagara graph schema is unavailable; the input default cannot be set."), TEXT("niagara_authoring"));
			OutError.CandidateName = InputPin.PinName.ToString();
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = TEXT("niagara_schema_available");
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
			OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			return false;
		}
		Schema->TrySetDefaultValue(InputPin, DefaultText, true);

		TSharedPtr<FJsonValue> ReadbackValue;
		FString ReadbackKind;
		if (!EncodeLiteralValue(InputType, InputPin.DefaultValue, ReadbackValue, ReadbackKind, OutError))
		{
			auto Failure = UE::UAI::Contract::FDiagnostic::Failure(TEXT("niagara_literal_readback_failed"), TEXT("The Niagara input literal could not be read back."), TEXT("literal_readback"));
			Failure.CandidateName = InputPin.PinName.ToString();
			Failure.SourceKind = TEXT("native_result");
			Failure.PropertyPath = TEXT("type");
			Failure.ActualValue = MakeShared<FJsonValueString>(InputType.GetName());
			Failure.ExpectedValue = MakeShared<FJsonValueString>(TEXT("type with readable native literal default"));
			if (!OutError.IsEmpty()) Failure.PublicCause = MakeShared<UE::UAI::Contract::FDiagnostic>(OutError);
			OutError = MoveTemp(Failure);
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		bOutChanged = false;
		OutError.Reset();
		const FString NormalizedLinkedParameter = LinkedParameterName.TrimStartAndEnd();
		if (NormalizedLinkedParameter.IsEmpty())
		{
			OutError = StageFailure(TEXT("niagara_linked_input_parameter_missing"), TEXT("A linked input requires a non-empty parameter name."),
				TEXT("/value/linked_parameter"), MakeShared<FJsonValueString>(LinkedParameterName), MakeShared<FJsonValueString>(TEXT("non-empty parameter name")));
			OutError.CandidateName = InputPin.PinName.ToString();
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
			UeAgentAuthoringDiagnostics::FDiagnostic Warning;
			Warning.Code = TEXT("niagara_linked_input_apply_verification_failed");
			Warning.PublicMessage = TEXT("The input link read back after applying it does not match the requested parameter.");
			Warning.SourceKind = TEXT("native_result");
			Warning.Stage = TEXT("niagara_input_observation");
			Warning.PropertyPath = InputPin.GetName();
			Warning.ExpectedValue = MakeShared<FJsonValueString>(NormalizedLinkedParameter);
			Warning.ActualValue = MakeShared<FJsonValueString>(ReadbackLinkedParameter);
			UeAgentAuthoringDiagnostics::ReportConsistencyWarning(Warning);
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		ON_SCOPE_EXIT {
			if (Payload && !OutError.IsEmpty()) UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Payload, TEXT("input_apply"));
		};
		const TSharedPtr<FJsonObject>* Value = nullptr;
		FString Source;
		if (Payload.IsValid()
			&& Payload->TryGetObjectField(TEXT("value"), Value)
			&& Value && Value->IsValid()
			&& (*Value)->TryGetStringField(TEXT("source"), Source))
		{
			if (!Source.Equals(TEXT("linked_input"), ESearchCase::IgnoreCase))
			{
				OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("niagara_input_source_unsupported"),
					TEXT("/value/source"), MakeShared<FJsonValueString>(TEXT("linked_input")),
					MakeShared<FJsonValueString>(Source), TEXT("niagara_authoring"));
				OutError.PublicMessage = TEXT("Unsupported Niagara input source.");
				OutError.bRetrySafe = false;
				return false;
			}
			FString LinkedParameterName;
			if (!(*Value)->TryGetStringField(TEXT("linked_parameter"), LinkedParameterName)
				|| LinkedParameterName.TrimStartAndEnd().IsEmpty())
			{
				OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("niagara_linked_input_parameter_missing"),
					TEXT("/value/linked_parameter"), MakeShared<FJsonValueString>(TEXT("non-empty parameter name")),
					(*Value)->TryGetField(TEXT("linked_parameter")), TEXT("niagara_authoring"));
				OutError.PublicMessage = TEXT("A linked input requires a non-empty parameter name.");
				OutError.ActualValuePresent = (*Value)->HasField(TEXT("linked_parameter"));
				OutError.bRetrySafe = false;
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
		UE::UAI::Contract::FDiagnostic& OutError)
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
		UE::UAI::Contract::FDiagnostic& OutError)
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
			OutError = StageFailure(TEXT("niagara_linked_input_not_supported_for_static_switch"), TEXT("A static-switch input requires a literal value and cannot use a linked parameter."),
				TEXT("/value/source"), MakeShared<FJsonValueString>(TEXT("linked_input")), MakeShared<FJsonValueString>(TEXT("literal")));
			OutError.CandidateName = OutResolvedInputName;
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		ON_SCOPE_EXIT {
			if (Payload && !OutError.IsEmpty()) UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Payload, TEXT("input_apply"));
		};
		const TSharedPtr<FJsonObject>* Value = nullptr;
		FString Source;
		if (Payload.IsValid()
			&& Payload->TryGetObjectField(TEXT("value"), Value)
			&& Value && Value->IsValid()
			&& (*Value)->TryGetStringField(TEXT("source"), Source))
		{
			if (!Source.Equals(TEXT("linked_input"), ESearchCase::IgnoreCase))
			{
				OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("niagara_input_source_unsupported"),
					TEXT("/value/source"), MakeShared<FJsonValueString>(TEXT("linked_input")),
					MakeShared<FJsonValueString>(Source), TEXT("niagara_authoring"));
				OutError.PublicMessage = TEXT("Unsupported Niagara input source.");
				OutError.bRetrySafe = false;
				return false;
			}
			FString LinkedParameterName;
			if (!(*Value)->TryGetStringField(TEXT("linked_parameter"), LinkedParameterName)
				|| LinkedParameterName.TrimStartAndEnd().IsEmpty())
			{
				OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("niagara_linked_input_parameter_missing"),
					TEXT("/value/linked_parameter"), MakeShared<FJsonValueString>(TEXT("non-empty parameter name")),
					(*Value)->TryGetField(TEXT("linked_parameter")), TEXT("niagara_authoring"));
				OutError.PublicMessage = TEXT("A linked input requires a non-empty parameter name.");
				OutError.ActualValuePresent = (*Value)->HasField(TEXT("linked_parameter"));
				OutError.bRetrySafe = false;
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
		FString& OutResolvedInputName, UE::UAI::Contract::FDiagnostic& OutError)
	{
		FNiagaraVariable Variable;
		FGuid ScriptVariableGuid;
		UEdGraphPin* InputPin = nullptr;
		OutError.Reset();
        OutResolvedInputName.Reset();
		if (!ResolveInputPin(
			ModuleNode,
			InputName,
			OutResolvedInputName,
			Variable,
			ScriptVariableGuid,
			InputPin,
			OutError)
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

	bool ClearInputValueNodesForPin(UEdGraphPin& InputPin, UE::UAI::Contract::FDiagnostic& OutError)
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutDynamicInputNode = nullptr;
		OutResolvedInputName.Reset();
		OutError.Reset();
		ON_SCOPE_EXIT {
			if (!OutError.IsEmpty() && OutError.CandidateName.IsEmpty())
				OutError.CandidateName = OutResolvedInputName.IsEmpty() ? Selector.InputName : OutResolvedInputName;
		};
		if (!Stage.Graph || !Stage.OutputNode)
		{
			OutError = StageFailure(TEXT("niagara_stage_context_invalid"),
				!Stage.Graph ? TEXT("Dynamic input resolution requires a Niagara graph.") : TEXT("Dynamic input resolution requires a stage output node."),
				TEXT("stage_context_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
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
				OutError = StageFailure(InputPin && InputPin->LinkedTo.Num() > 1
				? TEXT("niagara_dynamic_input_links_invalid") : TEXT("niagara_dynamic_input_not_found"),
				InputPin ? TEXT("The selected input must have exactly one link to a dynamic input node.") : TEXT("The selected module input has no override pin."),
					TEXT("input_links"), MakeShared<FJsonValueNumber>(InputPin ? InputPin->LinkedTo.Num() : 0), MakeShared<FJsonValueNumber>(1));
				return false;
			}
			OutDynamicInputNode = Cast<UNiagaraNodeFunctionCall>(InputPin->LinkedTo[0]->GetOwningNode());
		}
		if (!OutDynamicInputNode || OutDynamicInputNode->GetCalledUsage() != ENiagaraScriptUsage::DynamicInput)
		{
				OutError = StageFailure(OutDynamicInputNode ? TEXT("niagara_dynamic_input_usage_invalid") : TEXT("niagara_dynamic_input_not_found"),
				OutDynamicInputNode ? TEXT("The selected function node is not a dynamic input.") : TEXT("No function node matches the dynamic input selector or input link."), TEXT("called_usage"),
				OutDynamicInputNode ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(StaticEnum<ENiagaraScriptUsage>()->GetNameStringByValue(static_cast<int64>(OutDynamicInputNode->GetCalledUsage())))) : nullptr,
				MakeShared<FJsonValueString>(TEXT("DynamicInput")));
			OutError.InternalContext = Selector.NodeGuid.ToString();
			return false;
		}
		return true;
	}

	bool SetDynamicInputForPin(
		UEdGraphPin& InputPin,
		UNiagaraScript& DynamicInputScript,
		const FString& SuggestedName,
		UNiagaraNodeFunctionCall*& OutDynamicInputNode,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutDynamicInputNode = nullptr;
		UEdGraphNode* OwnerNode = InputPin.GetOwningNode();
		ON_SCOPE_EXIT { if (!OutError.IsEmpty() && OutError.CandidateName.IsEmpty()) OutError.CandidateName = InputPin.PinName.ToString(); };
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
			OutError = StageFailure(TEXT("niagara_dynamic_input_override_pin_invalid"),
				!ParameterMapSetClass ? TEXT("The Niagara parameter-map override node class could not be loaded.")
					: TEXT("A dynamic input must be attached to a parameter-map override pin."), TEXT("pin_owner_type"),
				OwnerNode ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(OwnerNode->GetClass()->GetName())) : nullptr,
				MakeShared<FJsonValueString>(TEXT("NiagaraNodeParameterMapSet")));
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
			OutError = StageFailure(TEXT("niagara_set_dynamic_input_failed"),
				FString::Printf(TEXT("UE could not create a dynamic input from script '%s' for this pin."), *DynamicInputScript.GetName()),
				TEXT("dynamic_input_created"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.InternalContext = DynamicInputScript.GetPathName();
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

	bool ClearDynamicInputForPin(UEdGraphPin& InputPin, UE::UAI::Contract::FDiagnostic& OutError)
	{
		ON_SCOPE_EXIT { if (!OutError.IsEmpty() && OutError.CandidateName.IsEmpty()) OutError.CandidateName = InputPin.PinName.ToString(); };
		if (InputPin.LinkedTo.IsEmpty())
		{
			OutError = StageFailure(TEXT("niagara_dynamic_input_not_found"), TEXT("The input pin has no linked dynamic input to clear."),
				TEXT("input_links"), MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("one or more links")));
			return false;
		}
		UEdGraphNode* OverrideNode = InputPin.GetOwningNode();
		if (!IsParameterMapSetNode(OverrideNode))
		{
			OutError = StageFailure(TEXT("niagara_dynamic_input_override_pin_invalid"), TEXT("Only a parameter-map override pin can have its dynamic input cleared."),
				TEXT("pin_owner_type"), OverrideNode ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(OverrideNode->GetClass()->GetName())) : nullptr,
				MakeShared<FJsonValueString>(TEXT("NiagaraNodeParameterMapSet")));
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
		UE::UAI::Contract::FDiagnostic& OutError)
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		UNiagaraGraph* Graph = DynamicInputNode.GetNiagaraGraph();
		if (!Graph)
		{
			OutError = StageFailure(TEXT("niagara_dynamic_input_graph_missing"), TEXT("The dynamic input node has no Niagara graph for data-interface initialization."),
				TEXT("graph_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.CandidateName = DynamicInputNode.GetFunctionName();
			return false;
		}

		TArray<FResolvedModuleInput> Inputs;
		BuildResolvedModuleInputs(DynamicInputNode, Inputs);
		int32 DataInterfaceCount = 0;
		int32 InitializedCount = 0;
		TArray<UE::UAI::Contract::FDiagnostic> Errors;
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
				auto Error = StageFailure(TEXT("niagara_data_interface_parameter_map_input_invalid"),
					ParameterMapInput ? TEXT("Data-interface initialization requires exactly one parameter-map input link.") : TEXT("The dynamic input node has no parameter-map input pin."),
					TEXT("parameter_map_input_links"), MakeShared<FJsonValueNumber>(ParameterMapInput ? ParameterMapInput->LinkedTo.Num() : 0), MakeShared<FJsonValueNumber>(1));
				Error.CandidateName = Input.FullInputName;
				Errors.Add(MoveTemp(Error));
				continue;
			}

			UEdGraphPin& OverridePin = FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
				DynamicInputNode, Aliased, InputType, Input.InputScriptVariableGuid, FGuid());
			if (!IsParameterMapSetNode(OverridePin.GetOwningNode()))
			{
				const auto* PinOwner = OverridePin.GetOwningNode();
				auto Error = StageFailure(TEXT("niagara_data_interface_override_pin_owner_invalid"), TEXT("The data-interface override pin must belong to a parameter-map set node."),
					TEXT("pin_owner_type"), PinOwner ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(PinOwner->GetClass()->GetName())) : nullptr,
					MakeShared<FJsonValueString>(TEXT("NiagaraNodeParameterMapSet")));
				Error.CandidateName = Input.FullInputName;
				Errors.Add(MoveTemp(Error));
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
			else {
				auto Error = StageFailure(TEXT("niagara_data_interface_create_failed"),
					FString::Printf(TEXT("UE could not create the data interface of type '%s' for this input."), *InputClass->GetName()),
					TEXT("data_interface_created"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				Error.CandidateName = Input.FullInputName;
				Errors.Add(MoveTemp(Error));
			}
		}

		Graph->NotifyGraphChanged();
		if (!Errors.IsEmpty())
		{
			OutError = StageFailure(TEXT("niagara_dynamic_input_data_interface_initialization_failed"), TEXT("Some dynamic-input data interfaces could not be initialized."),
				TEXT("initialized_data_interfaces"), MakeShared<FJsonValueNumber>(InitializedCount), MakeShared<FJsonValueNumber>(DataInterfaceCount));
			OutError.CandidateName = DynamicInputNode.GetFunctionName();
			OutError.Diagnostics = MoveTemp(Errors);
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
		UE::UAI::Contract::FDiagnostic& OutError)
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
			OutError = StageFailure(TEXT("niagara_dynamic_input_data_interface_input_not_found"),
				!Input ? TEXT("The dynamic input has no parameter matching the requested input name.")
					: !Input->InputVariable.IsValid() ? TEXT("The selected data-interface input has an invalid native parameter identity.")
					: TEXT("The selected input is not a data-interface parameter."), TEXT("input_type"),
				Input ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Input->InputVariable.GetType().GetName())) : nullptr,
				MakeShared<FJsonValueString>(TEXT("data-interface parameter")));
			OutError.CandidateName = InputName;
			return false;
		}

		const FNiagaraParameterHandle Aliased = BuildInputOverrideHandle(
			Input->InputVariable, DynamicInputNode);
		UEdGraphPin* InputPin = Input->VisiblePin;
		if (!InputPin)
		{
			InputPin = FindOverridePin(DynamicInputNode, Aliased.GetParameterHandleString());
		}
		if (!InputPin)
		{
			OutError = StageFailure(TEXT("niagara_dynamic_input_data_interface_pin_not_found"), TEXT("The selected data-interface parameter has neither a visible pin nor an override pin."),
				TEXT("input_pin_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.CandidateName = OutResolvedInputName;
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
		OutError = StageFailure(TEXT("niagara_dynamic_input_data_interface_not_initialized"),
			FString::Printf(TEXT("None of the input's %d link(s) provides an initialized data interface."), InputPin->LinkedTo.Num()),
			TEXT("data_interface_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
		OutError.CandidateName = OutResolvedInputName;
		return false;
	}

	bool ClearDynamicInput(
		UNiagaraNodeFunctionCall& ModuleNode,
		const FString& InputName,
		FString& OutResolvedInputName,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		FNiagaraVariable Variable;
		FGuid ScriptVariableGuid;
		UEdGraphPin* InputPin = nullptr;
		if (!ResolveInputPin(ModuleNode, InputName, OutResolvedInputName, Variable, ScriptVariableGuid, InputPin, OutError)) return false;
		if (!InputPin || InputPin->LinkedTo.IsEmpty())
		{
			OutError = StageFailure(TEXT("niagara_dynamic_input_not_found"),
				InputPin ? TEXT("The module input has no linked dynamic input to clear.") : TEXT("The module input has no override pin to clear."),
				TEXT("input_links"), MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("one or more links")));
			OutError.CandidateName = OutResolvedInputName;
			return false;
		}
		return ClearDynamicInputForPin(*InputPin, OutError);
	}
}

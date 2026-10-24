#include "DomainAdapters/AudioMediaGraph/UeAgentAudioMediaGraphTextureGraphNativeHook.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Blueprint/TG_AsyncRenderTask.h"
#include "Expressions/Input/TG_Expression_Scalar.h"
#include "Expressions/Output/TG_Expression_Output.h"
#include "Expressions/TG_Expression.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Job/JobBatch.h"
#include "Model/Mix/MixSettings.h"
#include "Model/Mix/MixUpdateCycle.h"
#include "TG_Graph.h"
#include "TG_GraphEvaluation.h"
#include "TG_HelperFunctions.h"
#include "TG_Node.h"
#include "TG_OutputSettings.h"
#include "TG_Pin.h"
#include "TG_Variant.h"
#include "TextureGraph.h"
#include "RenderingThread.h"
#include "UObject/UnrealType.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Dom/JsonValue.h"
#include "Dom/JsonObject.h"
#include "Misc/ScopeExit.h"
#include "TG_Var.h"

namespace UE::UAI::DomainAdapters::AudioMediaGraph
{
	namespace
	{
		FAudioMediaGraphError TextureGraphFact(const FString& Code, const FString& Reason,
			const FString& Path, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("native_graph"));
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Path;
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			return Error;
		}

		void SupplementTextureGraphContext(const UTextureGraph& Graph, FAudioMediaGraphError& Error)
		{
			if (Error.InternalContext.IsEmpty()) Error.InternalContext = Graph.GetPathName();
			if (Error.TargetPath.IsEmpty() && Graph.IsAsset()) Error.TargetPath = Graph.GetPathName();
		}

		TSharedPtr<FJsonValue> TextureGraphFlags(const TCHAR* First, bool bFirst, const TCHAR* Second, bool bSecond)
		{
			auto Facts = MakeShared<FJsonObject>();
			Facts->SetBoolField(First, bFirst);
			Facts->SetBoolField(Second, bSecond);
			return MakeShared<FJsonValueObject>(Facts);
		}

		TSharedPtr<FJsonValue> TextureGraphInputValue(const FAudioMediaGraphTextureGraphValue& Value)
		{
			switch (Value.GetKind())
			{
			case EAudioMediaGraphTextureGraphValueKind::Number: return Value.GetNumberValue();
			case EAudioMediaGraphTextureGraphValueKind::Boolean: return MakeShared<FJsonValueBoolean>(Value.GetBoolean());
			case EAudioMediaGraphTextureGraphValueKind::String: return MakeShared<FJsonValueString>(Value.GetString());
			case EAudioMediaGraphTextureGraphValueKind::NumberArray:
			{
				TArray<TSharedPtr<FJsonValue>> Values;
				for (double Item : Value.GetNumberArray()) Values.Add(MakeShared<FJsonValueNumber>(Item));
				return MakeShared<FJsonValueArray>(MoveTemp(Values));
			}
			default: return MakeShared<FJsonValueNull>();
			}
		}

		bool RecordTextureGraphPinWrite(const UTG_Pin& Pin, const FAudioMediaGraphTextureGraphValue& Value,
			bool bApplied, FAudioMediaGraphError& OutError)
		{
			if (bApplied) return true;
			OutError = TextureGraphFact(TEXT("audio_media_graph_texture_graph_parameter_apply_failed"),
				TEXT("The native input pin rejected the supplied value."),
				Pin.GetArgumentName().ToString(), TextureGraphInputValue(Value),
				MakeShared<FJsonValueString>(Pin.GetArgumentCPPTypeName().ToString()));
			return false;
		}

		void SupplementTextureGraphInputSource(const UE::UAI::Contract::FSourceLocation& Source,
			FAudioMediaGraphError& Error)
		{
			if (Source.SourceFile.IsEmpty() && Source.JsonPath.IsEmpty()) return;
			if (Error.SourceKind != TEXT("native_result") && Error.SourceKind != TEXT("command_input")
				&& Error.Source.SourceFile.IsEmpty() && Error.Source.JsonPath.IsEmpty())
			{
				Error.Source = Source;
				if (Error.SourceKind.IsEmpty()) Error.SourceKind = TEXT("related_input");
				return;
			}
			auto Related = MakeShared<FJsonObject>();
			Related->SetStringField(TEXT("source_kind"), TEXT("related_input"));
			if (!Source.SourceFile.IsEmpty()) Related->SetStringField(TEXT("source_file"), Source.SourceFile);
			if (!Source.JsonPath.IsEmpty()) Related->SetStringField(TEXT("json_pointer"), Source.JsonPath);
			if (Source.Line > 0) Related->SetNumberField(TEXT("line"), Source.Line);
			if (Source.Column > 0) Related->SetNumberField(TEXT("column"), Source.Column);
			Error.RelatedSources.Add(MakeShared<FJsonValueObject>(Related));
		}
	}

	bool FAudioMediaGraphTextureGraphNativeHook::UsesCompactParameterValue(const UTG_Pin& Pin)
	{
		// Settings keep their reflected descriptors, constraints and notifications.
		if (!Pin.IsInput() || Pin.IsSetting()) return false;
		// UE's FString accessor would reinterpret FText storage.
		const FName Type = Pin.GetArgumentCPPTypeName();
		if (Type == TEXT("FText")) return false;
		return Pin.IsArgScalar() || Pin.IsArgBool() || Pin.IsArgString()
			|| Pin.IsArgColor() || Pin.IsArgVector();
	}

	bool FAudioMediaGraphTextureGraphNativeHook::TryReadPinNumber(const UTG_Pin& Pin, double& OutValue)
	{
		if (!Pin.IsArgScalar() || !Pin.GetSelfVar()) return false;
		const FName Type = Pin.GetArgumentCPPTypeName();
		if (Type == TEXT("int32")) OutValue = Pin.GetSelfVar()->GetAs<int32>();
		else if (Type == TEXT("uint32")) OutValue = Pin.GetSelfVar()->GetAs<uint32>();
		else
		{
			float Value;
			if (!Pin.GetValue(Value)) return false;
			OutValue = Value;
		}
		return true;
	}

	bool FAudioMediaGraphTextureGraphNativeHook::TryNormalizePinNumber(const UTG_Pin& Pin,
		const FAudioMediaGraphTextureGraphValue& Value, double& OutValue, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!Pin.IsArgScalar())
		{
			OutError = TextureGraphFact(TEXT("texture_graph_input_number_type_mismatch"),
				TEXT("The selected TextureGraph input is not a scalar pin."), Pin.GetArgumentName().ToString(),
				MakeShared<FJsonValueString>(Pin.GetArgumentCPPTypeName().ToString()), MakeShared<FJsonValueString>(TEXT("scalar")));
			OutError.ProposedValue = TextureGraphInputValue(Value);
			return false;
		}
		const FName Type = Pin.GetArgumentCPPTypeName();
		if (Type == TEXT("int32") || Type == TEXT("uint32"))
		{
			int64 Integer;
			if (!UeAgentPropertyValueCodec::TryReadIntegerNumber(Value.GetNumberValue(),
				Type == TEXT("int32") ? MIN_int32 : 0,
				Type == TEXT("int32") ? int64(MAX_int32) : int64(MAX_uint32), Integer, OutError)) return false;
			OutValue = static_cast<double>(Integer);
			return true;
		}
		float NativeValue;
		if (!UeAgentPropertyValueCodec::TryConvertFloat32(Value.GetNumber(), NativeValue, OutError)) return false;
		OutValue = NativeValue;
		return true;
	}

	bool FAudioMediaGraphTextureGraphNativeHook::UsesReflectedExpressionProperty(
		UTG_Expression& Expression, const FProperty& Property)
	{
		const FString Access = Property.GetMetaData(TEXT("TGType"));
		if (Access == TEXT("TG_Output") || Access == TEXT("TG_OutputParam") || Access == TEXT("TG_Private")) return false;
		const UTG_Node* Node = Expression.GetParentNode();
		const UTG_Pin* Pin = Node ? Node->GetPin(Property.GetFName()) : nullptr;
		return !Pin || !UsesCompactParameterValue(*Pin);
	}

	namespace
	{
		FString MakeTextureGraphEvidenceId(const TCHAR* Stage)
		{
			return FString::Printf(TEXT("texture-graph:%s:%s"), Stage,
				*FGuid::NewGuid().ToString(EGuidFormats::Digits));
		}

		FString DescribeVariantType(const FTG_Variant& Value)
		{
			if (Value.IsScalar())
			{
				return TEXT("scalar");
			}
			if (Value.IsColor())
			{
				return TEXT("color");
			}
			if (Value.IsVector())
			{
				return TEXT("vector");
			}
			if (Value.IsTexture())
			{
				return TEXT("texture");
			}
			return TEXT("variant");
		}

		UTG_Node* FindSemanticNode(UTG_Graph& Graph, const FString& ElementId)
		{
			UTG_Node* Result = nullptr;
			Graph.ForEachNodes(
				[&Result, &ElementId](const UTG_Node* Node, const uint32)
				{
					if (Result != nullptr || Node == nullptr
						|| Node->GetExpression() == nullptr)
					{
						return;
					}
					const bool bBuiltinOutput = ElementId == TEXT("builtin_output")
						&& Cast<UTG_Expression_Output>(Node->GetExpression()) != nullptr;
					if (bBuiltinOutput || Node->GetExpression()->GetName() == ElementId)
					{
						Result = const_cast<UTG_Node*>(Node);
					}
				});
			return Result;
		}

		bool TryApplyPinValue(
			UTG_Pin& Pin,
			const FAudioMediaGraphTextureGraphValue& Value, UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (!FAudioMediaGraphTextureGraphNativeHook::UsesCompactParameterValue(Pin))
			{
				OutError = TextureGraphFact(TEXT("texture_graph_input_value_requires_reflected_property"),
					TEXT("This input requires its reflected expression property rather than a compact pin value."),
					Pin.GetArgumentName().ToString(), MakeShared<FJsonValueString>(Pin.GetArgumentCPPTypeName().ToString()),
					MakeShared<FJsonValueString>(TEXT("reflected expression property")));
				OutError.ProposedValue = TextureGraphInputValue(Value);
				return false;
			}
			switch (Value.GetKind())
			{
			case EAudioMediaGraphTextureGraphValueKind::Number:
			{
				double NativeValue;
				if (!FAudioMediaGraphTextureGraphNativeHook::TryNormalizePinNumber(Pin, Value, NativeValue, OutError)) return false;
				const FName Type = Pin.GetArgumentCPPTypeName();
				// Explicit templates avoid UE's integer overloads narrowing through float.
				if (Type == TEXT("int32")) return RecordTextureGraphPinWrite(Pin, Value, Pin.SetValue<int32>(static_cast<int32>(NativeValue)), OutError);
				if (Type == TEXT("uint32")) return RecordTextureGraphPinWrite(Pin, Value, Pin.SetValue<uint32>(static_cast<uint32>(NativeValue)), OutError);
				return RecordTextureGraphPinWrite(Pin, Value, Pin.SetValue(static_cast<float>(NativeValue)), OutError);
			}
			case EAudioMediaGraphTextureGraphValueKind::Boolean:
				return RecordTextureGraphPinWrite(Pin, Value, Pin.SetValue(Value.GetBoolean()), OutError);
			case EAudioMediaGraphTextureGraphValueKind::String:
				return RecordTextureGraphPinWrite(Pin, Value, Pin.SetValue(Value.GetString()), OutError);
			case EAudioMediaGraphTextureGraphValueKind::NumberArray:
			{
				const TArray<double>& Values = Value.GetNumberArray();
				if (Values.Num() != 4)
				{
					OutError = TextureGraphFact(TEXT("texture_graph_input_value_length_invalid"),
						TEXT("A TextureGraph color or vector input requires four numeric components."),
						Pin.GetArgumentName().ToString(), MakeShared<FJsonValueNumber>(Values.Num()), MakeShared<FJsonValueNumber>(4));
					OutError.ProposedValue = TextureGraphInputValue(Value);
					return false;
				}
				float NativeValues[4];
				for (int32 Index = 0; Index < 4; ++Index)
				{
					if (!UeAgentPropertyValueCodec::TryConvertFloat32(Values[Index], NativeValues[Index], OutError))
					{
						if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = TEXT("/value/") + LexToString(Index);
						if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Pin.GetArgumentName().ToString();
						return false;
					}
				}
				if (Pin.IsArgColor())
				{
					return RecordTextureGraphPinWrite(Pin, Value, Pin.SetValue(FLinearColor(
						NativeValues[0], NativeValues[1], NativeValues[2], NativeValues[3])), OutError);
				}
				return RecordTextureGraphPinWrite(Pin, Value, Pin.SetValue(FVector4f(
					NativeValues[0], NativeValues[1], NativeValues[2], NativeValues[3])), OutError);
			}
			default:
				OutError = TextureGraphFact(TEXT("texture_graph_input_value_kind_unsupported"),
					TEXT("The input value has no supported compact TextureGraph representation."), Pin.GetArgumentName().ToString(),
					MakeShared<FJsonValueNumber>(static_cast<int32>(Value.GetKind())),
					MakeShared<FJsonValueString>(TEXT("number, boolean, string or four-component number array")));
				return false;
			}
		}

		bool TryEvaluateNativeGraph(UTG_Graph& NativeGraph, UTextureGraph& Graph, FAudioMediaGraphError& OutError)
		{
			UMixSettings* Settings = Graph.GetSettings();
			if (Settings == nullptr)
			{
				OutError = TextureGraphFact(TEXT("audio_media_graph_evaluate_incomplete"),
					TEXT("TextureGraph evaluation cannot start because the graph has no Mix settings."),
					TEXT("/settings"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				SupplementTextureGraphContext(Graph, OutError);
				return false;
			}
			if (Settings->NumTargets() == 0)
			{
				// Native initialization also installs the per-Mix error reporter.
				// Allocating only the target array leaves Validate/Evaluate unable
				// to report diagnostics for loaded or duplicated assets.
				FTG_HelperFunctions::InitTargets(&Graph);
			}
			FInvalidationDetails Details = FInvalidationDetails().All();
			Details.Mix = &Graph;
			const std::shared_ptr<JobBatch> Batch = JobBatch::Create(Details);
			const MixUpdateCyclePtr Cycle = Batch ? Batch->GetCycle() : nullptr;
			if (!Cycle)
			{
				OutError = TextureGraphFact(TEXT("audio_media_graph_evaluate_incomplete"),
					TEXT("TextureGraph did not create an evaluation cycle."),
					TEXT("/evaluation_cycle"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				SupplementTextureGraphContext(Graph, OutError);
				return false;
			}
			Cycle->AddTarget(std::make_shared<MixTargetUpdate>(&Graph, 0));
			FTG_EvaluationContext Context;
			Context.Cycle = Cycle;
			Context.TargetId = 0;
			Context.GraphDepth = 1;
			Cycle->PushMix(&Graph);
			const bool bValidated = NativeGraph.Validate(Cycle);
			if (bValidated)
			{
				NativeGraph.Evaluate(&Context);
			}
			Cycle->PopMix();
			if (!bValidated)
			{
				OutError = TextureGraphFact(TEXT("audio_media_graph_evaluate_incomplete"),
					TEXT("The native TextureGraph validation rejected the graph before evaluation."),
					TEXT("/validation"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				SupplementTextureGraphContext(Graph, OutError);
				return false;
			}
			TArray<FTG_Variant> Values;
			const int32 OutputCount = NativeGraph.GetAllOutputParamValues(Values);
			if (OutputCount <= 0)
			{
				OutError = TextureGraphFact(TEXT("audio_media_graph_evaluate_incomplete"),
					TEXT("TextureGraph evaluation returned no output parameter values."),
					TEXT("/output_parameters"), MakeShared<FJsonValueNumber>(OutputCount), MakeShared<FJsonValueString>(TEXT("at least one output")));
				SupplementTextureGraphContext(Graph, OutError);
				return false;
			}
			return true;
		}
	}

	FAudioMediaGraphTextureGraphValue
	FAudioMediaGraphTextureGraphValue::Number(const double InValue)
	{
		FAudioMediaGraphTextureGraphValue Value;
		Value.Kind = EAudioMediaGraphTextureGraphValueKind::Number;
		Value.NumberValue = InValue;
		Value.NumberSource = MakeShared<FJsonValueNumber>(InValue);
		return Value;
	}

	FAudioMediaGraphTextureGraphValue
	FAudioMediaGraphTextureGraphValue::Number(TSharedPtr<FJsonValue> InValue)
	{
		FAudioMediaGraphTextureGraphValue Value;
		Value.Kind = EAudioMediaGraphTextureGraphValueKind::Number;
		Value.NumberValue = InValue->AsNumber();
		Value.NumberSource = MoveTemp(InValue);
		return Value;
	}

	FAudioMediaGraphTextureGraphValue
	FAudioMediaGraphTextureGraphValue::Boolean(const bool bInValue)
	{
		FAudioMediaGraphTextureGraphValue Value;
		Value.Kind = EAudioMediaGraphTextureGraphValueKind::Boolean;
		Value.bBooleanValue = bInValue;
		return Value;
	}

	FAudioMediaGraphTextureGraphValue
	FAudioMediaGraphTextureGraphValue::String(FString InValue)
	{
		FAudioMediaGraphTextureGraphValue Value;
		Value.Kind = EAudioMediaGraphTextureGraphValueKind::String;
		Value.StringValue = MoveTemp(InValue);
		return Value;
	}

	FAudioMediaGraphTextureGraphValue
	FAudioMediaGraphTextureGraphValue::NumberArray(TArray<double> InValues)
	{
		FAudioMediaGraphTextureGraphValue Value;
		Value.Kind = EAudioMediaGraphTextureGraphValueKind::NumberArray;
		Value.NumberArrayValue = MoveTemp(InValues);
		return Value;
	}

	UTG_Node* FAudioMediaGraphTextureGraphNativeHook::ResolveNode(
		UTG_Graph& Graph, const FString& ElementId,
		const FAudioMediaGraphTextureGraphNodeBindings* Bindings) const
	{
		if (!Bindings) return FindSemanticNode(Graph, ElementId);
		const int32* Index = Bindings->NodeIndices.Find(ElementId);
		if (!Index) return nullptr;
		UTG_Node* Result = nullptr;
		Graph.ForEachNodes([&](const UTG_Node* Node, const uint32)
		{
			if (Node && Node->GetId().NodeIdx() == *Index)
				Result = const_cast<UTG_Node*>(Node);
		});
		return Result;
	}

	void FAudioMediaGraphTextureGraphNativeHook::InitializeCreatedNode(
		const UTG_Node& Node, const FString& TargetAssetPath)
	{
		if (UTG_Expression_Output* Output = Cast<UTG_Expression_Output>(Node.GetExpression()))
		{
			Output->InitializeOutputSettings();
			if (!TargetAssetPath.IsEmpty())
			{
				FTG_OutputSettings Settings = Output->OutputSettings;
				Settings.Initialize(TargetAssetPath, Output->GetTitleName());
				Node.GetGraph()->SetOutputSettings({{Node.GetId(), MoveTemp(Settings)}});
			}
		}
	}

	bool FAudioMediaGraphTextureGraphNativeHook::TryApplyNodeMutations(
		UTextureGraph& Graph,
		const TArray<FAudioMediaGraphTextureGraphNodeMutation>& Mutations,
		FAudioMediaGraphError& OutError,
		FAudioMediaGraphTextureGraphNodeBindings* Bindings,
		const FString& TargetAssetPath) const
	{
		OutError.Reset();
		UTG_Graph* const NativeGraph = Graph.Graph();
		if (NativeGraph == nullptr)
		{
			OutError = TextureGraphFact(TEXT("audio_media_graph_texture_graph_missing"),
				TEXT("The TextureGraph asset has no native graph."), TEXT("/graph"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			SupplementTextureGraphContext(Graph, OutError);
			return false;
		}
		for (const FAudioMediaGraphTextureGraphNodeMutation& Mutation : Mutations)
		{
			UTG_Node* Existing = ResolveNode(*NativeGraph, Mutation.GetElementId(), Bindings);
			if (Mutation.GetIntent() == EAudioMediaGraphMutationIntent::Delete)
			{
				if (Existing == nullptr)
				{
					OutError = TextureGraphFact(TEXT("audio_media_graph_texture_graph_node_missing"),
						TEXT("The TextureGraph node selected for deletion was not found."), TEXT("/node"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					SupplementTextureGraphContext(Graph, OutError);
					OutError.CandidateName = Mutation.GetElementId();
					return false;
				}
				NativeGraph->RemoveNode(Existing);
				if (Bindings) Bindings->NodeIndices.Remove(Mutation.GetElementId());
				continue;
			}
			UClass* const ExpressionClass = LoadObject<UClass>(
				nullptr, *Mutation.GetExpressionClassRef());
			if (ExpressionClass == nullptr
				|| !ExpressionClass->IsChildOf(UTG_Expression::StaticClass()))
			{
				OutError = TextureGraphFact(TEXT("audio_media_graph_texture_graph_class_missing"),
					TEXT("The requested expression class could not be loaded as a TextureGraph expression."), TEXT("/expression_class"), MakeShared<FJsonValueString>(ExpressionClass ? ExpressionClass->GetPathName() : Mutation.GetExpressionClassRef()), MakeShared<FJsonValueString>(UTG_Expression::StaticClass()->GetPathName()));
				SupplementTextureGraphContext(Graph, OutError);
				OutError.CandidateName = Mutation.GetElementId();
				return false;
			}
			if (Mutation.GetIntent() == EAudioMediaGraphMutationIntent::Create
				&& Existing != nullptr)
			{
				OutError = TextureGraphFact(TEXT("audio_media_graph_texture_graph_node_exists"),
					TEXT("The TextureGraph node already exists."), TEXT("/node"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
				SupplementTextureGraphContext(Graph, OutError);
				OutError.CandidateName = Mutation.GetElementId();
				return false;
			}
			if (Mutation.GetIntent() == EAudioMediaGraphMutationIntent::Update)
			{
				if (Existing == nullptr || Existing->GetExpression() == nullptr
					|| Existing->GetExpression()->GetClass() != ExpressionClass)
				{
					OutError = TextureGraphFact(TEXT("audio_media_graph_texture_graph_node_mismatch"),
						TEXT("The TextureGraph node selected for update is missing or has a different expression class."), TEXT("/expression_class"), MakeShared<FJsonValueString>(Existing && Existing->GetExpression() ? Existing->GetExpression()->GetClass()->GetPathName() : TEXT("<missing>")), MakeShared<FJsonValueString>(Mutation.GetExpressionClassRef()));
					SupplementTextureGraphContext(Graph, OutError);
					OutError.CandidateName = Mutation.GetElementId();
					return false;
				}
				continue;
			}
			if (Existing != nullptr)
			{
				NativeGraph->RemoveNode(Existing);
				if (Bindings) Bindings->NodeIndices.Remove(Mutation.GetElementId());
			}
			UTG_Node* Created = nullptr;
			if (Bindings)
			{
				Created = NativeGraph->CreateExpressionNode(ExpressionClass);
			}
			else
			{
				UTG_Expression* const Expression = NewObject<UTG_Expression>(
					NativeGraph, ExpressionClass, FName(*Mutation.GetElementId()), RF_Transactional);
				Created = Expression ? NativeGraph->CreateExpressionNode(Expression) : nullptr;
			}
			if (Created == nullptr || Created->GetExpression() == nullptr
				|| (!Bindings && Created->GetExpression()->GetName() != Mutation.GetElementId()))
			{
				OutError = TextureGraphFact(TEXT("audio_media_graph_texture_graph_node_apply_failed"),
					TEXT("TextureGraph did not create the requested node with the expected expression identity."), TEXT("/node"), MakeShared<FJsonValueString>(Created && Created->GetExpression() ? Created->GetExpression()->GetName() : TEXT("<missing>")), MakeShared<FJsonValueString>(Mutation.GetElementId()));
				SupplementTextureGraphContext(Graph, OutError);
				OutError.CandidateName = Mutation.GetExpressionClassRef();
				return false;
			}
			InitializeCreatedNode(*Created, TargetAssetPath);
			if (Bindings) Bindings->NodeIndices.Add(Mutation.GetElementId(), Created->GetId().NodeIdx());
		}
		Graph.PostEditChange();
		return true;
	}

	bool FAudioMediaGraphTextureGraphNativeHook::TryApplyLinkMutations(
		UTextureGraph& Graph,
		const TArray<FAudioMediaGraphTextureGraphLinkMutation>& Mutations,
		FAudioMediaGraphError& OutError,
		const FAudioMediaGraphTextureGraphNodeBindings* Bindings) const
	{
		OutError.Reset();
		UTG_Graph* const NativeGraph = Graph.Graph();
		if (NativeGraph == nullptr)
		{
			OutError = TextureGraphFact(TEXT("audio_media_graph_texture_graph_missing"),
				TEXT("The TextureGraph asset has no native graph."), TEXT("/graph"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			SupplementTextureGraphContext(Graph, OutError);
			return false;
		}
		for (const FAudioMediaGraphTextureGraphLinkMutation& Mutation : Mutations)
		{
			UTG_Node* const From = ResolveNode(
				*NativeGraph, Mutation.GetFrom().GetElementId(), Bindings);
			UTG_Node* const To = ResolveNode(
				*NativeGraph, Mutation.GetTo().GetElementId(), Bindings);
			FTG_Name FromPinName(*Mutation.GetFrom().GetPinName());
			FTG_Name ToPinName(*Mutation.GetTo().GetPinName());
			UTG_Pin* const FromPin = From ? From->GetOutputPin(FromPinName) : nullptr;
			UTG_Pin* const ToPin = To ? To->GetInputPin(ToPinName) : nullptr;
			if (From == nullptr || To == nullptr || FromPin == nullptr || ToPin == nullptr)
			{
				OutError = TextureGraphFact(TEXT("audio_media_graph_texture_graph_link_endpoint_missing"),
					TEXT("The requested TextureGraph link source or destination pin was not found."), TEXT("/link"), TextureGraphFlags(TEXT("output_pin_present"), FromPin != nullptr, TEXT("input_pin_present"), ToPin != nullptr), TextureGraphFlags(TEXT("output_pin_present"), true, TEXT("input_pin_present"), true));
				SupplementTextureGraphContext(Graph, OutError);
				OutError.CandidateName = Mutation.GetFrom().GetElementId() + TEXT(".") + Mutation.GetFrom().GetPinName() + TEXT(" -> ") + Mutation.GetTo().GetElementId() + TEXT(".") + Mutation.GetTo().GetPinName();
				return false;
			}
			const bool bExists = FromPin->GetEdges().Contains(ToPin->GetId());
			if (Mutation.GetIntent() == EAudioMediaGraphMutationIntent::Delete)
			{
				if (!bExists)
				{
					OutError = TextureGraphFact(TEXT("audio_media_graph_texture_graph_link_missing"),
						TEXT("The selected TextureGraph link does not exist."), TEXT("/link"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					SupplementTextureGraphContext(Graph, OutError);
					OutError.CandidateName = Mutation.GetFrom().GetElementId() + TEXT(".") + Mutation.GetFrom().GetPinName() + TEXT(" -> ") + Mutation.GetTo().GetElementId() + TEXT(".") + Mutation.GetTo().GetPinName();
					return false;
				}
				NativeGraph->RemoveEdge(*From, FromPinName, *To, ToPinName);
			}
			else
			{
				if (bExists)
				{
					OutError = TextureGraphFact(TEXT("audio_media_graph_texture_graph_link_apply_failed"),
						TEXT("The requested TextureGraph link already exists."), TEXT("/link"),
						MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
					SupplementTextureGraphContext(Graph, OutError);
					OutError.CandidateName = Mutation.GetFrom().GetElementId() + TEXT(".") + Mutation.GetFrom().GetPinName() + TEXT(" -> ") + Mutation.GetTo().GetElementId() + TEXT(".") + Mutation.GetTo().GetPinName();
					return false;
				}
				if (!NativeGraph->Connect(*From, FromPinName, *To, ToPinName))
				{
					OutError = TextureGraphFact(TEXT("audio_media_graph_texture_graph_link_apply_failed"),
						TEXT("The native TextureGraph rejected the requested connection."), TEXT("/link"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					SupplementTextureGraphContext(Graph, OutError);
					OutError.CandidateName = Mutation.GetFrom().GetElementId() + TEXT(".") + Mutation.GetFrom().GetPinName() + TEXT(" -> ") + Mutation.GetTo().GetElementId() + TEXT(".") + Mutation.GetTo().GetPinName();
					return false;
				}
			}
		}
		Graph.PostEditChange();
		return true;
	}

	bool FAudioMediaGraphTextureGraphNativeHook::TryApplyParameterMutations(
		UTextureGraph& Graph,
		const TArray<FAudioMediaGraphTextureGraphParameterMutation>& Mutations,
		FAudioMediaGraphError& OutError,
		const FAudioMediaGraphTextureGraphNodeBindings* Bindings) const
	{
		OutError.Reset();
		UTG_Graph* const NativeGraph = Graph.Graph();
		if (NativeGraph == nullptr)
		{
			OutError = TextureGraphFact(TEXT("audio_media_graph_texture_graph_missing"),
				TEXT("The TextureGraph asset has no native graph."), TEXT("/graph"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			SupplementTextureGraphContext(Graph, OutError);
			return false;
		}
		for (const FAudioMediaGraphTextureGraphParameterMutation& Mutation : Mutations)
		{
			UTG_Node* const Node = ResolveNode(*NativeGraph, Mutation.GetElementId(), Bindings);
			FTG_Name PinName(*Mutation.GetPinName());
			UTG_Pin* const Pin = Node ? Node->GetInputPin(PinName) : nullptr;
			UE::UAI::Contract::FDiagnostic ValueError;
			if (Pin == nullptr)
			{
				OutError = TextureGraphFact(TEXT("audio_media_graph_texture_graph_parameter_apply_failed"),
					TEXT("The selected TextureGraph input pin was not found."), Mutation.GetPinName(),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.CandidateName = Mutation.GetElementId();
				SupplementTextureGraphContext(Graph, OutError);
				SupplementTextureGraphInputSource(Mutation.GetSource(), OutError);
				return false;
			}
			if (Mutation.HasValue() && !TryApplyPinValue(*Pin, Mutation.GetValue(), ValueError))
			{
				OutError = MoveTemp(ValueError);
				if (OutError.Stage.IsEmpty()) OutError.Stage = TEXT("texture_graph_input");
				if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = Mutation.GetPinName();
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Mutation.GetElementId() + TEXT(".") + Mutation.GetPinName();
				SupplementTextureGraphContext(Graph, OutError);
				SupplementTextureGraphInputSource(Mutation.GetSource(), OutError);
				return false;
			}
			Pin->SetAliasName(FName(*Mutation.GetAlias()));
		}
		Graph.PostEditChange();
		return true;
	}

	bool FAudioMediaGraphTextureGraphNativeHook::TryValidateEvaluateSettle(
		UTextureGraph& Graph,
		FAudioMediaGraphError& OutError) const
	{
		OutError.Reset();
		if (Graph.Graph() == nullptr)
		{
			OutError = TextureGraphFact(TEXT("audio_media_graph_evaluate_incomplete"),
				TEXT("TextureGraph evaluation cannot start because the native graph is missing."), TEXT("/graph"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			SupplementTextureGraphContext(Graph, OutError);
			return false;
		}
		if (!TryEvaluateNativeGraph(*Graph.Graph(), Graph, OutError)) return false;
		Graph.PostEditChange();
		return true;
	}

	bool FAudioMediaGraphTextureGraphNativeHook::TryCaptureArtifacts(
		UTextureGraph& Graph,
		const TArray<FAudioMediaGraphTextureGraphArtifactSelector>& Selectors,
		TArray<FAudioMediaGraphTextureGraphCapturedOutput>& OutOutputs,
		FAudioMediaGraphError& OutError) const
	{
		OutOutputs.Reset();
		OutError.Reset();
		UTG_Graph* const NativeGraph = Graph.Graph();
		if (NativeGraph == nullptr)
		{
			OutError = TextureGraphFact(TEXT("audio_media_graph_texture_graph_missing"),
				TEXT("The TextureGraph asset has no native graph."), TEXT("/graph"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			SupplementTextureGraphContext(Graph, OutError);
			return false;
		}
		if (Selectors.IsEmpty())
		{
			OutError = TextureGraphFact(TEXT("audio_media_graph_artifact_output_missing"),
				TEXT("No TextureGraph output selectors were supplied for capture."), TEXT("/selectors"),
				MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("at least one output selector")));
			SupplementTextureGraphContext(Graph, OutError);
			return false;
		}
		for (int32 SelectorIndex = 0; SelectorIndex < Selectors.Num(); ++SelectorIndex)
		{
			const auto& Selector = Selectors[SelectorIndex];
			UTG_Node* const Node = FindSemanticNode(*NativeGraph, Selector.GetElementId());
			FTG_Name PinName(*Selector.GetOutputName());
			UTG_Pin* const Pin = Node ? Node->GetOutputPin(PinName) : nullptr;
			UTG_Expression* const Expression = Node ? Node->GetExpression() : nullptr;
			if (Node == nullptr || Pin == nullptr || Expression == nullptr)
			{
				OutError = TextureGraphFact(TEXT("audio_media_graph_artifact_output_missing"),
					TEXT("The selected TextureGraph output node, expression or output pin was not found."), TEXT("/output"), TextureGraphFlags(TEXT("expression_present"), Expression != nullptr, TEXT("output_pin_present"), Pin != nullptr), TextureGraphFlags(TEXT("expression_present"), true, TEXT("output_pin_present"), true));
				SupplementTextureGraphContext(Graph, OutError);
				OutError.CandidateName = Selector.GetElementId() + TEXT(".") + Selector.GetOutputName();
				OutError.OperationIndex = SelectorIndex;
				return false;
			}
			FString SettingsFingerprint = TEXT("native-output-settings:none");
			if (const UTG_Expression_Output* const Output =
				Cast<UTG_Expression_Output>(Expression))
			{
				SettingsFingerprint = Output->OutputSettings.ToString();
			}
			OutOutputs.Emplace(
				Expression->GetPathName(), Selector.GetOutputName(),
				Node->GetId().NodeIdx(), Pin->GetId().PinIdx(),
				Pin->GetArgumentCPPTypeName().ToString(),
				MoveTemp(SettingsFingerprint));
		}
		return true;
	}

	bool FAudioMediaGraphTextureGraphNativeHook::TryRenderAndReadOutputs(
		UObject& WorldContextObject,
		UTextureGraph& Graph,
		TArray<FAudioMediaGraphTextureGraphRenderedOutput>& OutOutputs,
		FAudioMediaGraphError& OutError) const
	{
		OutOutputs.Reset();
		OutError.Reset();
		(void)WorldContextObject;
		if (!FApp::CanEverRender())
		{
			OutError = TextureGraphFact(TEXT("texture_graph_render_unavailable_null_rhi"),
				TEXT("The current Unreal process cannot render TextureGraph outputs."), TEXT("/render_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			SupplementTextureGraphContext(Graph, OutError);
			return false;
		}
		UTG_AsyncRenderTask* RenderTask =
			UTG_AsyncRenderTask::TG_AsyncRenderTask(&Graph);
		if (!RenderTask)
		{
			OutError = TextureGraphFact(TEXT("texture_graph_render_function_unavailable"),
				TEXT("Unreal did not create the TextureGraph asynchronous render task."), TEXT("/render_task"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			SupplementTextureGraphContext(Graph, OutError);
			return false;
		}
		auto ReleaseRenderTask = [RenderTask]()
		{
			FWorldDelegates::OnWorldCleanup.RemoveAll(RenderTask);
			RenderTask->SetReadyToDestroy();
		};
		const TArray<UTextureRenderTarget2D*> RenderTargets =
			RenderTask->ActivateBlocking(nullptr);
		if (RenderTargets.IsEmpty())
		{
			ReleaseRenderTask();
			OutError = TextureGraphFact(TEXT("texture_graph_render_output_missing"),
				TEXT("The TextureGraph render task returned no render targets."), TEXT("/render_outputs"), MakeShared<FJsonValueNumber>(RenderTargets.Num()), MakeShared<FJsonValueString>(TEXT("at least one render target")));
			SupplementTextureGraphContext(Graph, OutError);
			return false;
		}
		FlushRenderingCommands();
		for (int32 OutputIndex = 0; OutputIndex < RenderTargets.Num(); ++OutputIndex)
		{
			UTextureRenderTarget2D* RenderTarget = RenderTargets[OutputIndex];
			FRenderTarget* Resource = RenderTarget
				? RenderTarget->GameThread_GetRenderTargetResource() : nullptr;
			TArray<FColor> Pixels;
			FReadSurfaceDataFlags ReadFlags(RCM_UNorm);
			ReadFlags.SetLinearToGamma(false);
			const bool bReadSucceeded = RenderTarget && Resource && Resource->ReadPixels(Pixels, ReadFlags);
			if (!RenderTarget || !Resource || !bReadSucceeded || Pixels.IsEmpty())
			{
				auto Actual = MakeShared<FJsonObject>();
				Actual->SetBoolField(TEXT("render_target_present"), RenderTarget != nullptr);
				Actual->SetBoolField(TEXT("render_resource_present"), Resource != nullptr);
				if (RenderTarget && Resource) Actual->SetBoolField(TEXT("read_succeeded"), bReadSucceeded);
				Actual->SetNumberField(TEXT("pixel_count"), Pixels.Num());
				OutOutputs.Reset();
				ReleaseRenderTask();
				OutError = TextureGraphFact(TEXT("texture_graph_render_readback_failed"),
					TEXT("A TextureGraph render output is missing its render target/resource or did not return readable pixels."), TEXT("/render_outputs"),
					MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("a render target/resource and successful readback of at least one pixel")));
				SupplementTextureGraphContext(Graph, OutError);
				OutError.OperationIndex = OutputIndex;
				return false;
			}
			FAudioMediaGraphTextureGraphRenderedOutput Output;
			Output.RenderTarget = RenderTarget;
			Output.Stats.Width = RenderTarget->SizeX;
			Output.Stats.Height = RenderTarget->SizeY;
			Output.Stats.SampledPixelCount = Pixels.Num();
			Output.Stats.Minimum = FLinearColor(1.f, 1.f, 1.f, 1.f);
			Output.Stats.Maximum = FLinearColor(0.f, 0.f, 0.f, 0.f);
			FLinearColor Sum(0.f, 0.f, 0.f, 0.f);
			TSet<uint32> Unique;
			for (const FColor& Pixel : Pixels)
			{
				const FLinearColor Color = Pixel.ReinterpretAsLinear();
				Output.Stats.Minimum.R = FMath::Min(
					Output.Stats.Minimum.R, Color.R);
				Output.Stats.Minimum.G = FMath::Min(
					Output.Stats.Minimum.G, Color.G);
				Output.Stats.Minimum.B = FMath::Min(
					Output.Stats.Minimum.B, Color.B);
				Output.Stats.Minimum.A = FMath::Min(
					Output.Stats.Minimum.A, Color.A);
				Output.Stats.Maximum.R = FMath::Max(
					Output.Stats.Maximum.R, Color.R);
				Output.Stats.Maximum.G = FMath::Max(
					Output.Stats.Maximum.G, Color.G);
				Output.Stats.Maximum.B = FMath::Max(
					Output.Stats.Maximum.B, Color.B);
				Output.Stats.Maximum.A = FMath::Max(
					Output.Stats.Maximum.A, Color.A);
				Sum += Color;
				if ((Pixel.R + Pixel.G + Pixel.B) > 12)
				{
					++Output.Stats.NonBlackPixelCount;
				}
				if (Pixel.A < 250)
				{
					++Output.Stats.TransparentPixelCount;
				}
				if (Unique.Num() < 65536)
				{
					Unique.Add(Pixel.ToPackedARGB());
				}
			}
			Output.Stats.UniqueColorEstimate = Unique.Num();
			Output.Stats.Mean = Sum / static_cast<float>(Pixels.Num());
			OutOutputs.Add(MoveTemp(Output));
		}
		ReleaseRenderTask();
		return true;
	}

	bool FAudioMediaGraphTextureGraphNativeHook::
	TryMutateEvaluateAndCaptureOwnedGraph(
		UTextureGraph& Graph,
		FAudioMediaGraphTextureGraphScenarioResult& OutResult,
		FAudioMediaGraphError& OutError) const
	{
		OutResult = FAudioMediaGraphTextureGraphScenarioResult();
		OutError.Reset();
		UTG_Graph* NativeGraph = Graph.Graph();
		if (NativeGraph == nullptr)
		{
			OutError = TextureGraphFact(TEXT("audio_media_graph_texture_graph_missing"),
				TEXT("The TextureGraph asset has no native graph."), TEXT("/graph"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			SupplementTextureGraphContext(Graph, OutError);
			return false;
		}
		int32 BeforeNodeCount = 0;
		NativeGraph->ForEachNodes(
			[&BeforeNodeCount](const UTG_Node*, const uint32)
			{
				++BeforeNodeCount;
			});
		OutResult.BeforeRevision = FString::Printf(
			TEXT("texture-graph-nodes:%d"), BeforeNodeCount);

		UTG_Node* ScalarNode = NativeGraph->CreateExpressionNode(
			UTG_Expression_Scalar::StaticClass());
		UTG_Node* OutputNode = nullptr;
		NativeGraph->ForEachNodes(
			[&OutputNode](const UTG_Node* Node, const uint32)
			{
				if (OutputNode == nullptr
					&& Cast<UTG_Expression_Output>(Node->GetExpression()) != nullptr)
				{
					OutputNode = const_cast<UTG_Node*>(Node);
				}
			});
		UTG_Expression_Scalar* ScalarExpression = ScalarNode
			? Cast<UTG_Expression_Scalar>(ScalarNode->GetExpression())
			: nullptr;
		UTG_Expression_Output* OutputExpression = OutputNode
			? Cast<UTG_Expression_Output>(OutputNode->GetExpression())
			: nullptr;
		if (ScalarNode == nullptr || ScalarExpression == nullptr
			|| OutputNode == nullptr || OutputExpression == nullptr)
		{
			OutError = TextureGraphFact(TEXT("audio_media_graph_texture_graph_apply_failed"),
				TEXT("TextureGraph did not create a scalar expression and locate an output expression."), TEXT("/nodes"), TextureGraphFlags(TEXT("scalar_expression_present"), ScalarExpression != nullptr, TEXT("output_expression_present"), OutputExpression != nullptr), TextureGraphFlags(TEXT("scalar_expression_present"), true, TEXT("output_expression_present"), true));
			SupplementTextureGraphContext(Graph, OutError);
			return false;
		}
		ScalarExpression->Scalar = 0.75f;
		ScalarExpression->SetTitleName(TEXT("GC010_Gain"));
		OutputExpression->OutputSettings.FolderPath =
			TEXT("/Game/UAI/Owned/AudioMediaGraph");
		OutputExpression->SetShouldExport(false);
		FTG_Name SourcePin = TEXT("ValueOut");
		FTG_Name TargetPin = TEXT("Source");
		if (!NativeGraph->Connect(
			*ScalarNode, SourcePin, *OutputNode, TargetPin))
		{
			OutError = TextureGraphFact(TEXT("audio_media_graph_texture_graph_link_failed"),
				TEXT("The native TextureGraph rejected the scalar-to-output connection."), TEXT("/link"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			SupplementTextureGraphContext(Graph, OutError);
			OutError.CandidateName = TEXT("Scalar.ValueOut -> Output.Source");
			return false;
		}
		OutResult.bAppliedMutation = true;
		OutResult.ApplyEvidenceId = MakeTextureGraphEvidenceId(TEXT("apply"));

		// Share native context initialization with ordinary workspace evaluation.
		OutResult.bEvaluateBarrier = TryEvaluateNativeGraph(*NativeGraph, Graph, OutError);
		OutResult.bSettled = OutResult.bEvaluateBarrier;
		OutResult.BarrierEvidenceId =
			MakeTextureGraphEvidenceId(TEXT("validate-evaluate-settle"));
		if (!OutResult.bEvaluateBarrier)
		{

			return false;
		}

		NativeGraph->ForEachNodes(
			[&OutResult](const UTG_Node* Node, const uint32)
			{
				const UTG_Expression* Expression = Node->GetExpression();
				if (Expression != nullptr)
				{
					OutResult.NodeIdentities.Emplace(
						Expression->GetPathName(), Node->GetId().NodeIdx(),
						Expression->GetClass()->GetPathName());
				}
			});
		NativeGraph->ForEachPins(
			[&OutResult](const UTG_Pin* Pin, const uint32, const uint32)
			{
				OutResult.PinIdentities.Emplace(
					Pin->GetId().NodeIdx(), Pin->GetId().PinIdx(),
					Pin->GetArgumentName().ToString(),
					Pin->IsInput() ? TEXT("input") : TEXT("output"),
					Pin->GetArgumentCPPTypeName().ToString());
			});
		OutResult.ParameterNames = NativeGraph->GetParamNames();
		if (OutResult.NodeIdentities.IsEmpty()
			|| OutResult.PinIdentities.IsEmpty()
			|| OutResult.ParameterNames.IsEmpty())
		{
			auto Actual = MakeShared<FJsonObject>();
			Actual->SetNumberField(TEXT("node_count"), OutResult.NodeIdentities.Num());
			Actual->SetNumberField(TEXT("pin_count"), OutResult.PinIdentities.Num());
			Actual->SetNumberField(TEXT("parameter_count"), OutResult.ParameterNames.Num());
			OutError = TextureGraphFact(TEXT("audio_media_graph_capture_failed"),
				TEXT("TextureGraph readback is missing node identities, pin identities or parameter names."), TEXT("/capture"),
				MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("at least one node, pin and parameter")));
			SupplementTextureGraphContext(Graph, OutError);
			return false;
		}
		OutResult.bIndependentCapture = true;
		OutResult.ReadbackRevision = FString::Printf(
			TEXT("texture-graph-nodes:%d-pins:%d-params:%d"),
			OutResult.NodeIdentities.Num(), OutResult.PinIdentities.Num(),
			OutResult.ParameterNames.Num());
		OutResult.ReadbackEvidenceId =
			MakeTextureGraphEvidenceId(TEXT("native-topology-readback"));
		return true;
	}

	bool FAudioMediaGraphTextureGraphNativeHook::
	TryCaptureSettledArtifactState(
		UTextureGraph& Graph,
		const FAudioMediaGraphTextureGraphScenarioResult& SettledResult,
		FAudioMediaGraphTextureGraphArtifactState& OutState,
		FAudioMediaGraphError& OutError) const
	{
		OutState = FAudioMediaGraphTextureGraphArtifactState();
		OutError.Reset();
		if (!SettledResult.HasEvaluateBarrier() || !SettledResult.HasSettled())
		{
			OutError = TextureGraphFact(TEXT("audio_media_graph_artifact_before_settle"),
				TEXT("TextureGraph artifact capture requires a completed evaluation barrier and settled graph."), TEXT("/settle"), TextureGraphFlags(TEXT("evaluate_complete"), SettledResult.HasEvaluateBarrier(), TEXT("settled"), SettledResult.HasSettled()), TextureGraphFlags(TEXT("evaluate_complete"), true, TEXT("settled"), true));
			SupplementTextureGraphContext(Graph, OutError);
			return false;
		}
		UTG_Graph* NativeGraph = Graph.Graph();
		if (NativeGraph == nullptr)
		{
			OutError = TextureGraphFact(TEXT("audio_media_graph_texture_graph_missing"),
				TEXT("The TextureGraph asset has no native graph."), TEXT("/graph"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			SupplementTextureGraphContext(Graph, OutError);
			return false;
		}

		TArray<FTG_Variant> Values;
		TArray<FName> Names;
		NativeGraph->GetAllOutputParamValues(Values, &Names);
		for (int32 Index = 0; Index < Values.Num(); ++Index)
		{
			const FName OutputName = Names.IsValidIndex(Index)
				? Names[Index] : FName(*FString::Printf(TEXT("Output%d"), Index));
			const UTG_Pin* OutputPin = NativeGraph->FindParamPin(OutputName);
			FString SettingsFingerprint = TEXT("native-output-settings:missing");
			if (OutputPin != nullptr)
			{
				if (UTG_Expression_Output* OutputExpression =
					Cast<UTG_Expression_Output>(
						OutputPin->GetNodePtr()->GetExpression()))
				{
					SettingsFingerprint =
						OutputExpression->OutputSettings.ToString();
				}
			}
			const FString OutputIdentity = FString::Printf(
				TEXT("owner:%s|output_pin:%s|name:%s"),
				*Graph.GetPathName(),
				OutputPin ? *OutputPin->GetId().ToString() : TEXT("unknown"),
				*OutputName.ToString());
			OutState.Outputs.Emplace(
				OutputIdentity, SettingsFingerprint,
				DescribeVariantType(Values[Index]));
		}
		if (OutState.Outputs.IsEmpty())
		{
			OutError = TextureGraphFact(TEXT("audio_media_graph_artifact_output_missing"),
				TEXT("TextureGraph artifact readback returned no output parameter values."), TEXT("/output_parameters"), MakeShared<FJsonValueNumber>(OutState.Outputs.Num()), MakeShared<FJsonValueString>(TEXT("at least one output")));
			SupplementTextureGraphContext(Graph, OutError);
			return false;
		}
		OutState.bIndependentCapture = true;
		OutState.bPublished = false;
		OutState.EvidenceId =
			MakeTextureGraphEvidenceId(TEXT("native-artifact-state"));
		return true;
	}
}

// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringProfileStateTextureGraph.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringDescriptors.h"
#include "Expressions/Input/TG_Expression_Scalar.h"
#include "Expressions/Output/TG_Expression_Output.h"
#include "TextureGraph.h"
#include "TG_Graph.h"
#include "TG_Node.h"
#include "TG_Pin.h"

#include <initializer_list>

namespace UeAgentReflectionAuthoring::ProfileStateTextureGraph
{
	namespace
	{
		const FName ScalarOutputPinName = GET_MEMBER_NAME_CHECKED(UTG_Expression_Scalar, ValueOut);
		const FName OutputSourcePinName = GET_MEMBER_NAME_CHECKED(UTG_Expression_Output, Source);

		struct FParameterSpec
		{
			FString Name;
			float Value = 0.0f;
		};

		struct FEdgeSpec
		{
			FString Parameter;
			FString FromPin;
			FString Node;
			FString ToPin;
		};

		struct FReadParameter
		{
			FString Name;
			float Value = 0.0f;
		};

		struct FResolvedParameter
		{
			FParameterSpec Spec;
			UTG_Node* Node = nullptr;
			UTG_Pin* ParameterPin = nullptr;
			UTG_Pin* OutputPin = nullptr;
			float PreviousValue = 0.0f;
			bool bCreated = false;
			bool bValueApplied = false;
		};

		struct FResolvedEdge
		{
			FEdgeSpec Spec;
			int32 ParameterIndex = INDEX_NONE;
			UTG_Node* TargetNode = nullptr;
			UTG_Pin* TargetPin = nullptr;
			bool bAlreadyConnected = false;
			bool bConnectedByApply = false;
		};

		bool ValidateFields(
			const FJsonObject& Object,
			const std::initializer_list<const TCHAR*>& AllowedFields,
			const TCHAR* ErrorCode,
			FString& OutError)
		{
			TSet<FString> Allowed;
			for (const TCHAR* Field : AllowedFields) Allowed.Add(Field);
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Object.Values)
			{
				if (!Allowed.Contains(Field.Key))
				{
					OutError = ErrorCode;
					return false;
				}
			}
			return true;
		}

		bool ReadRequiredString(
			const FJsonObject& Object,
			const TCHAR* FieldName,
			const TCHAR* MissingCode,
			const TCHAR* InvalidCode,
			FString& OutValue,
			FString& OutError)
		{
			if (!Object.HasField(FieldName))
			{
				OutError = MissingCode;
				return false;
			}
			if (!Object.TryGetStringField(FieldName, OutValue)
				|| OutValue.IsEmpty()
				|| OutValue != OutValue.TrimStartAndEnd())
			{
				OutError = InvalidCode;
				return false;
			}
			return true;
		}

		bool ParseGraph(
			const TSharedPtr<FJsonValue>& GraphValue,
			TArray<FParameterSpec>& OutParameters,
			TArray<FEdgeSpec>& OutEdges,
			FString& OutError)
		{
			OutParameters.Reset();
			OutEdges.Reset();
			OutError.Reset();
			if (!GraphValue.IsValid() || GraphValue->Type != EJson::Object)
			{
				OutError = TEXT("texture_graph_graph_invalid");
				return false;
			}
			const TSharedPtr<FJsonObject> Graph = GraphValue->AsObject();
			if (!Graph.IsValid())
			{
				OutError = TEXT("texture_graph_graph_invalid");
				return false;
			}
			if (!ValidateFields(*Graph, {TEXT("parameters"), TEXT("edges")}, TEXT("texture_graph_graph_unknown_field"), OutError))
			{
				return false;
			}

			const TArray<TSharedPtr<FJsonValue>>* Parameters = nullptr;
			if (!Graph->HasField(TEXT("parameters")))
			{
				OutError = TEXT("texture_graph_parameters_missing");
				return false;
			}
			if (!Graph->TryGetArrayField(TEXT("parameters"), Parameters) || !Parameters)
			{
				OutError = TEXT("texture_graph_parameters_invalid");
				return false;
			}
			TSet<FString> ParameterNames;
			TSet<FName> ParameterKeys;
			for (const TSharedPtr<FJsonValue>& ParameterValue : *Parameters)
			{
				if (!ParameterValue.IsValid() || ParameterValue->Type != EJson::Object)
				{
					OutError = TEXT("texture_graph_parameter_invalid");
					return false;
				}
				const TSharedPtr<FJsonObject> Parameter = ParameterValue->AsObject();
				if (!Parameter.IsValid()
					|| !ValidateFields(
						*Parameter,
						{TEXT("name"), TEXT("expression_class"), TEXT("value")},
						TEXT("texture_graph_parameter_unknown_field"),
						OutError))
				{
					if (OutError.IsEmpty()) OutError = TEXT("texture_graph_parameter_invalid");
					return false;
				}
				FParameterSpec Spec;
				if (!ReadRequiredString(
						*Parameter,
						TEXT("name"),
						TEXT("texture_graph_parameter_name_missing"),
						TEXT("texture_graph_parameter_name_invalid"),
						Spec.Name,
						OutError))
				{
					return false;
				}
				if (FName(*Spec.Name).IsNone())
				{
					OutError = TEXT("texture_graph_parameter_name_invalid");
					return false;
				}
				const FName ParameterKey(*Spec.Name);
				if (ParameterNames.Contains(Spec.Name) || ParameterKeys.Contains(ParameterKey))
				{
					OutError = TEXT("texture_graph_parameter_duplicate");
					return false;
				}
				ParameterNames.Add(Spec.Name);
				ParameterKeys.Add(ParameterKey);

				FString ExpressionClass;
				if (!ReadRequiredString(
						*Parameter,
						TEXT("expression_class"),
						TEXT("texture_graph_parameter_class_missing"),
						TEXT("texture_graph_parameter_class_invalid"),
						ExpressionClass,
						OutError))
				{
					return false;
				}
				if (ExpressionClass != UTG_Expression_Scalar::StaticClass()->GetPathName())
				{
					OutError = TEXT("texture_graph_parameter_class_unsupported");
					return false;
				}

				if (!Parameter->HasField(TEXT("value")))
				{
					OutError = TEXT("texture_graph_parameter_missing");
					return false;
				}
				const TSharedPtr<FJsonObject>* Value = nullptr;
				if (!Parameter->TryGetObjectField(TEXT("value"), Value) || !Value || !Value->IsValid())
				{
					OutError = TEXT("texture_graph_parameter_value_invalid");
					return false;
				}
				if (!ValidateFields(**Value, {TEXT("literal")}, TEXT("texture_graph_parameter_value_unknown_field"), OutError))
				{
					return false;
				}
				if (!(*Value)->HasField(TEXT("literal")))
				{
					OutError = TEXT("texture_graph_parameter_literal_missing");
					return false;
				}
				double Literal = 0.0;
				if (!(*Value)->TryGetNumberField(TEXT("literal"), Literal)
					|| !FMath::IsFinite(Literal)
					|| FMath::Abs(Literal) > static_cast<double>(TNumericLimits<float>::Max()))
				{
					OutError = TEXT("texture_graph_parameter_literal_invalid");
					return false;
				}
				Spec.Value = static_cast<float>(Literal);
				OutParameters.Add(MoveTemp(Spec));
			}

			const TArray<TSharedPtr<FJsonValue>>* Edges = nullptr;
			if (!Graph->HasField(TEXT("edges")))
			{
				OutError = TEXT("texture_graph_edges_missing");
				return false;
			}
			if (!Graph->TryGetArrayField(TEXT("edges"), Edges) || !Edges)
			{
				OutError = TEXT("texture_graph_edges_invalid");
				return false;
			}
			TSet<FString> EdgeKeys;
			TMap<FString, FString> TargetOwners;
			for (const TSharedPtr<FJsonValue>& EdgeValue : *Edges)
			{
				if (!EdgeValue.IsValid() || EdgeValue->Type != EJson::Object)
				{
					OutError = TEXT("texture_graph_edge_invalid");
					return false;
				}
				const TSharedPtr<FJsonObject> Edge = EdgeValue->AsObject();
				if (!Edge.IsValid()
					|| !ValidateFields(*Edge, {TEXT("from"), TEXT("to")}, TEXT("texture_graph_edge_unknown_field"), OutError))
				{
					if (OutError.IsEmpty()) OutError = TEXT("texture_graph_edge_invalid");
					return false;
				}
				if (!Edge->HasField(TEXT("from")) || !Edge->HasField(TEXT("to")))
				{
					OutError = TEXT("texture_graph_edge_endpoint_missing");
					return false;
				}
				const TSharedPtr<FJsonObject>* From = nullptr;
				const TSharedPtr<FJsonObject>* To = nullptr;
				if (!Edge->TryGetObjectField(TEXT("from"), From) || !From || !From->IsValid()
					|| !Edge->TryGetObjectField(TEXT("to"), To) || !To || !To->IsValid())
				{
					OutError = TEXT("texture_graph_edge_endpoint_invalid");
					return false;
				}
				if (!ValidateFields(**From, {TEXT("parameter"), TEXT("pin")}, TEXT("texture_graph_edge_source_unknown_field"), OutError)
					|| !ValidateFields(**To, {TEXT("node"), TEXT("pin")}, TEXT("texture_graph_edge_target_unknown_field"), OutError))
				{
					return false;
				}
				FEdgeSpec Spec;
				if (!ReadRequiredString(
						**From,
						TEXT("parameter"),
						TEXT("texture_graph_edge_parameter_missing"),
						TEXT("texture_graph_edge_parameter_invalid"),
						Spec.Parameter,
						OutError)
					|| !ReadRequiredString(
						**From,
						TEXT("pin"),
						TEXT("texture_graph_edge_source_pin_missing"),
						TEXT("texture_graph_edge_source_pin_invalid"),
						Spec.FromPin,
						OutError)
					|| !ReadRequiredString(
						**To,
						TEXT("node"),
						TEXT("texture_graph_edge_target_node_missing"),
						TEXT("texture_graph_edge_target_node_invalid"),
						Spec.Node,
						OutError)
					|| !ReadRequiredString(
						**To,
						TEXT("pin"),
						TEXT("texture_graph_edge_target_pin_missing"),
						TEXT("texture_graph_edge_target_pin_invalid"),
						Spec.ToPin,
						OutError))
				{
					return false;
				}
				if (!ParameterNames.Contains(Spec.Parameter))
				{
					OutError = TEXT("texture_graph_edge_parameter_unknown");
					return false;
				}
				if (Spec.FromPin != ScalarOutputPinName.ToString())
				{
					OutError = TEXT("texture_graph_edge_source_pin_unsupported");
					return false;
				}
				if (Spec.ToPin != OutputSourcePinName.ToString())
				{
					OutError = TEXT("texture_graph_edge_target_pin_unsupported");
					return false;
				}
				const FString EdgeKey = Spec.Parameter + TEXT("\n") + Spec.FromPin + TEXT("\n") + Spec.Node + TEXT("\n") + Spec.ToPin;
				if (EdgeKeys.Contains(EdgeKey))
				{
					OutError = TEXT("texture_graph_edge_duplicate");
					return false;
				}
				EdgeKeys.Add(EdgeKey);
				const FString TargetKey = Spec.Node + TEXT("\n") + Spec.ToPin;
				if (const FString* ExistingOwner = TargetOwners.Find(TargetKey))
				{
					if (*ExistingOwner != Spec.Parameter)
					{
						OutError = TEXT("texture_graph_consumer_conflict");
						return false;
					}
				}
				else
				{
					TargetOwners.Add(TargetKey, Spec.Parameter);
				}
				OutEdges.Add(MoveTemp(Spec));
			}
			return true;
		}

		bool ResolveOutputNode(
			UTG_Graph& Graph,
			const FString& NodeName,
			UTG_Node*& OutNode,
			FString& OutError)
		{
			OutNode = nullptr;
			int32 MatchCount = 0;
			Graph.ForEachNodes(
				[&OutNode, &MatchCount, &NodeName](const UTG_Node* Candidate, uint32)
				{
					const UTG_Expression_Output* Output = Candidate
						? Cast<UTG_Expression_Output>(Candidate->GetExpression())
						: nullptr;
					if (Output && Output->GetTitleName().ToString() == NodeName)
					{
						OutNode = const_cast<UTG_Node*>(Candidate);
						++MatchCount;
					}
				});
			if (MatchCount == 0)
			{
				OutError = TEXT("texture_graph_consumer_missing");
				return false;
			}
			if (MatchCount > 1)
			{
				OutNode = nullptr;
				OutError = TEXT("texture_graph_consumer_ambiguous");
				return false;
			}
			return true;
		}

		TSharedPtr<FJsonValue> MakeGraphValue(
			const TArray<FReadParameter>& Parameters,
			const TArray<FEdgeSpec>& Edges)
		{
			TArray<TSharedPtr<FJsonValue>> ParameterValues;
			for (const FReadParameter& Parameter : Parameters)
			{
				TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
				Value->SetNumberField(TEXT("literal"), Parameter.Value);
				TSharedRef<FJsonObject> ParameterObject = MakeShared<FJsonObject>();
				ParameterObject->SetStringField(TEXT("name"), Parameter.Name);
				ParameterObject->SetStringField(
					TEXT("expression_class"),
					UTG_Expression_Scalar::StaticClass()->GetPathName());
				ParameterObject->SetObjectField(TEXT("value"), Value);
				ParameterValues.Add(MakeShared<FJsonValueObject>(ParameterObject));
			}

			TArray<TSharedPtr<FJsonValue>> EdgeValues;
			for (const FEdgeSpec& Edge : Edges)
			{
				TSharedRef<FJsonObject> From = MakeShared<FJsonObject>();
				From->SetStringField(TEXT("parameter"), Edge.Parameter);
				From->SetStringField(TEXT("pin"), Edge.FromPin);
				TSharedRef<FJsonObject> To = MakeShared<FJsonObject>();
				To->SetStringField(TEXT("node"), Edge.Node);
				To->SetStringField(TEXT("pin"), Edge.ToPin);
				TSharedRef<FJsonObject> EdgeObject = MakeShared<FJsonObject>();
				EdgeObject->SetObjectField(TEXT("from"), From);
				EdgeObject->SetObjectField(TEXT("to"), To);
				EdgeValues.Add(MakeShared<FJsonValueObject>(EdgeObject));
			}

			TSharedRef<FJsonObject> Graph = MakeShared<FJsonObject>();
			Graph->SetArrayField(TEXT("parameters"), MoveTemp(ParameterValues));
			Graph->SetArrayField(TEXT("edges"), MoveTemp(EdgeValues));
			return MakeShared<FJsonValueObject>(Graph);
		}
	}

	bool Supports(const FProfile& Profile)
	{
		return Profile.TypeId == TEXT("texture_graph");
	}

	bool Validate(
		const FProfile& Profile,
		const TSharedPtr<FJsonValue>& GraphValue,
		FString& OutError)
	{
		if (!Supports(Profile))
		{
			OutError = TEXT("texture_graph_profile_unsupported");
			return false;
		}
		TArray<FParameterSpec> Parameters;
		TArray<FEdgeSpec> Edges;
		return ParseGraph(GraphValue, Parameters, Edges, OutError);
	}

	bool Read(
		const FProfile& Profile,
		UObject& Asset,
		TSharedPtr<FJsonValue>& OutGraphValue,
		FString& OutError)
	{
		OutGraphValue.Reset();
		OutError.Reset();
		if (!Supports(Profile))
		{
			OutError = TEXT("texture_graph_profile_unsupported");
			return false;
		}
		const UTextureGraph* TextureGraph = Cast<UTextureGraph>(&Asset);
		if (!TextureGraph)
		{
			OutError = TEXT("texture_graph_asset_type_mismatch");
			return false;
		}
		const UTG_Graph* Graph = TextureGraph->Graph();
		if (!Graph)
		{
			OutError = TEXT("texture_graph_model_missing");
			return false;
		}

		TArray<FReadParameter> Parameters;
		TMap<const UTG_Node*, FString> ParameterNamesByNode;
		Graph->ForEachParams(
			[&Parameters, &ParameterNamesByNode, &OutError](const UTG_Pin* ParameterPin, uint32)
			{
				if (!OutError.IsEmpty() || !ParameterPin) return;
				const UTG_Node* Node = ParameterPin->GetNodePtr();
				if (!Node || !Cast<UTG_Expression_Scalar>(Node->GetExpression())) return;
				float Value = 0.0f;
				if (!ParameterPin->GetValue(Value) || !FMath::IsFinite(Value))
				{
					OutError = TEXT("texture_graph_parameter_value_unreadable");
					return;
				}
				const FString Name = ParameterPin->GetAliasName().ToString();
				if (Name.IsEmpty())
				{
					OutError = TEXT("texture_graph_parameter_name_invalid");
					return;
				}
				FReadParameter Parameter;
				Parameter.Name = Name;
				Parameter.Value = Value;
				Parameters.Add(MoveTemp(Parameter));
				ParameterNamesByNode.Add(Node, Name);
			});
		if (!OutError.IsEmpty()) return false;
		Parameters.Sort(
			[](const FReadParameter& A, const FReadParameter& B)
			{
				return A.Name.Compare(B.Name, ESearchCase::CaseSensitive) < 0;
			});

		TMap<FString, int32> OutputTitleCounts;
		Graph->ForEachNodes(
			[&OutputTitleCounts](const UTG_Node* Node, uint32)
			{
				const UTG_Expression_Output* Output = Node
					? Cast<UTG_Expression_Output>(Node->GetExpression())
					: nullptr;
				if (Output) ++OutputTitleCounts.FindOrAdd(Output->GetTitleName().ToString());
			});

		TArray<FEdgeSpec> Edges;
		Graph->ForEachEdges(
			[&Edges, &OutError, &OutputTitleCounts, &ParameterNamesByNode](const UTG_Pin* PinFrom, const UTG_Pin* PinTo)
			{
				if (!OutError.IsEmpty() || !PinFrom || !PinTo) return;
				const UTG_Node* SourceNode = PinFrom->GetNodePtr();
				const FString* ParameterName = ParameterNamesByNode.Find(SourceNode);
				if (!ParameterName || PinFrom->GetArgumentName() != ScalarOutputPinName) return;
				const UTG_Node* TargetNode = PinTo->GetNodePtr();
				const UTG_Expression_Output* Output = TargetNode
					? Cast<UTG_Expression_Output>(TargetNode->GetExpression())
					: nullptr;
				if (!Output || PinTo->GetArgumentName() != OutputSourcePinName) return;
				const FString NodeName = Output->GetTitleName().ToString();
				if (NodeName.IsEmpty())
				{
					OutError = TEXT("texture_graph_consumer_name_missing");
					return;
				}
				if (OutputTitleCounts.FindRef(NodeName) != 1)
				{
					OutError = TEXT("texture_graph_consumer_ambiguous");
					return;
				}
				FEdgeSpec Edge;
				Edge.Parameter = *ParameterName;
				Edge.FromPin = ScalarOutputPinName.ToString();
				Edge.Node = NodeName;
				Edge.ToPin = OutputSourcePinName.ToString();
				Edges.Add(MoveTemp(Edge));
			});
		if (!OutError.IsEmpty()) return false;
		Edges.Sort(
			[](const FEdgeSpec& A, const FEdgeSpec& B)
			{
				const FString AKey = A.Parameter + TEXT("\n") + A.FromPin + TEXT("\n") + A.Node + TEXT("\n") + A.ToPin;
				const FString BKey = B.Parameter + TEXT("\n") + B.FromPin + TEXT("\n") + B.Node + TEXT("\n") + B.ToPin;
				return AKey.Compare(BKey, ESearchCase::CaseSensitive) < 0;
			});

		OutGraphValue = MakeGraphValue(Parameters, Edges);
		TArray<FParameterSpec> ParsedParameters;
		TArray<FEdgeSpec> ParsedEdges;
		if (!ParseGraph(OutGraphValue, ParsedParameters, ParsedEdges, OutError))
		{
			OutGraphValue.Reset();
			return false;
		}
		return true;
	}

	bool Apply(
		const FProfile& Profile,
		UObject& Asset,
		const TSharedPtr<FJsonValue>& GraphValue,
		FString& OutError)
	{
		TArray<FParameterSpec> ParameterSpecs;
		TArray<FEdgeSpec> EdgeSpecs;
		if (!Supports(Profile))
		{
			OutError = TEXT("texture_graph_profile_unsupported");
			return false;
		}
		if (!ParseGraph(GraphValue, ParameterSpecs, EdgeSpecs, OutError)) return false;
		UTextureGraph* TextureGraph = Cast<UTextureGraph>(&Asset);
		if (!TextureGraph)
		{
			OutError = TEXT("texture_graph_asset_type_mismatch");
			return false;
		}
		UTG_Graph* Graph = TextureGraph->Graph();
		if (!Graph)
		{
			OutError = TEXT("texture_graph_model_missing");
			return false;
		}

		TArray<FResolvedParameter> Parameters;
		TMap<FString, int32> ParameterIndices;
		for (const FParameterSpec& Spec : ParameterSpecs)
		{
			FResolvedParameter Resolved;
			Resolved.Spec = Spec;
			Resolved.ParameterPin = Graph->FindParamPin(FName(*Spec.Name));
			if (Resolved.ParameterPin)
			{
				Resolved.Node = Resolved.ParameterPin->GetNodePtr();
				if (!Resolved.Node || !Cast<UTG_Expression_Scalar>(Resolved.Node->GetExpression()))
				{
					OutError = TEXT("texture_graph_parameter_type_conflict");
					return false;
				}
				Resolved.OutputPin = Resolved.Node->GetPin(ScalarOutputPinName);
				if (!Resolved.OutputPin)
				{
					OutError = TEXT("texture_graph_parameter_output_missing");
					return false;
				}
				if (!Resolved.ParameterPin->GetValue(Resolved.PreviousValue))
				{
					OutError = TEXT("texture_graph_parameter_value_unreadable");
					return false;
				}
			}
			ParameterIndices.Add(Spec.Name, Parameters.Num());
			Parameters.Add(MoveTemp(Resolved));
		}

		TArray<FResolvedEdge> Edges;
		for (const FEdgeSpec& Spec : EdgeSpecs)
		{
			const int32* ParameterIndex = ParameterIndices.Find(Spec.Parameter);
			if (!ParameterIndex)
			{
				OutError = TEXT("texture_graph_edge_parameter_unknown");
				return false;
			}
			FResolvedEdge Resolved;
			Resolved.Spec = Spec;
			Resolved.ParameterIndex = *ParameterIndex;
			if (!ResolveOutputNode(*Graph, Spec.Node, Resolved.TargetNode, OutError)) return false;
			Resolved.TargetPin = Resolved.TargetNode->GetPin(FName(*Spec.ToPin));
			if (!Resolved.TargetPin || !Resolved.TargetPin->IsInput())
			{
				OutError = TEXT("texture_graph_consumer_pin_missing");
				return false;
			}
			FResolvedParameter& Parameter = Parameters[Resolved.ParameterIndex];
			if (Resolved.TargetPin->IsConnected())
			{
				if (!Parameter.OutputPin
					|| Resolved.TargetPin->GetEdges().Num() != 1
					|| Resolved.TargetPin->GetEdges()[0] != Parameter.OutputPin->GetId())
				{
					OutError = TEXT("texture_graph_consumer_conflict");
					return false;
				}
				Resolved.bAlreadyConnected = true;
			}
			if (Parameter.OutputPin)
			{
				FName ConverterKey;
				if (!UTG_Graph::ArePinsCompatible(Parameter.OutputPin, Resolved.TargetPin, ConverterKey))
				{
					OutError = TEXT("texture_graph_connection_incompatible");
					return false;
				}
			}
			Edges.Add(MoveTemp(Resolved));
		}

		if (Parameters.IsEmpty() && Edges.IsEmpty())
		{
			OutError.Reset();
			return true;
		}
		TextureGraph->Modify();
		Graph->Modify();

		auto Rollback = [&]()
		{
			for (int32 Index = Edges.Num() - 1; Index >= 0; --Index)
			{
				FResolvedEdge& Edge = Edges[Index];
				if (!Edge.bConnectedByApply || !Edge.TargetNode) continue;
				FResolvedParameter& Parameter = Parameters[Edge.ParameterIndex];
				if (!Parameter.Node) continue;
				FName FromPin(*Edge.Spec.FromPin);
				FName ToPin(*Edge.Spec.ToPin);
				Graph->RemoveEdge(*Parameter.Node, FromPin, *Edge.TargetNode, ToPin);
			}
			for (int32 Index = Parameters.Num() - 1; Index >= 0; --Index)
			{
				FResolvedParameter& Parameter = Parameters[Index];
				if (!Parameter.bCreated && Parameter.bValueApplied && Parameter.ParameterPin)
				{
					Parameter.ParameterPin->SetValue(Parameter.PreviousValue);
				}
			}
			for (int32 Index = Parameters.Num() - 1; Index >= 0; --Index)
			{
				FResolvedParameter& Parameter = Parameters[Index];
				if (Parameter.bCreated && Parameter.Node) Graph->RemoveNode(Parameter.Node);
			}
		};

		for (FResolvedParameter& Parameter : Parameters)
		{
			if (Parameter.Node) continue;
			Parameter.Node = Graph->CreateExpressionNode(UTG_Expression_Scalar::StaticClass());
			Parameter.bCreated = Parameter.Node != nullptr;
			UTG_Expression_Scalar* Expression = Parameter.Node
				? Cast<UTG_Expression_Scalar>(Parameter.Node->GetExpression())
				: nullptr;
			if (!Expression)
			{
				OutError = TEXT("texture_graph_parameter_create_failed");
				Rollback();
				return false;
			}
			Expression->SetTitleName(FName(*Parameter.Spec.Name));
			Parameter.ParameterPin = Graph->FindParamPin(FName(*Parameter.Spec.Name));
			Parameter.OutputPin = Parameter.Node->GetPin(ScalarOutputPinName);
			if (!Parameter.ParameterPin
				|| Parameter.ParameterPin->GetNodePtr() != Parameter.Node
				|| !Parameter.OutputPin)
			{
				OutError = TEXT("texture_graph_parameter_name_conflict");
				Rollback();
				return false;
			}
		}

		for (FResolvedEdge& Edge : Edges)
		{
			if (Edge.bAlreadyConnected) continue;
			FResolvedParameter& Parameter = Parameters[Edge.ParameterIndex];
			FName ConverterKey;
			if (!UTG_Graph::ArePinsCompatible(Parameter.OutputPin, Edge.TargetPin, ConverterKey))
			{
				OutError = TEXT("texture_graph_connection_incompatible");
				Rollback();
				return false;
			}
			FName FromPin(*Edge.Spec.FromPin);
			FName ToPin(*Edge.Spec.ToPin);
			if (!Graph->Connect(*Parameter.Node, FromPin, *Edge.TargetNode, ToPin))
			{
				OutError = TEXT("texture_graph_connect_failed");
				Rollback();
				return false;
			}
			Edge.bConnectedByApply = true;
		}

		for (FResolvedParameter& Parameter : Parameters)
		{
			if (!Parameter.ParameterPin->SetValue(Parameter.Spec.Value))
			{
				OutError = TEXT("texture_graph_parameter_value_apply_failed");
				Rollback();
				return false;
			}
			Parameter.bValueApplied = true;
		}

		TextureGraph->MarkPackageDirty();
		Graph->MarkPackageDirty();
		OutError.Reset();
		return true;
	}
}

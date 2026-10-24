// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Material/Authoring/UeAgentMaterialAuthoringState.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Domains/Material/Authoring/UeAgentMaterialAuthoringDescriptors.h"
#include "Domains/Material/Authoring/UeAgentMaterialGraphAuthoring.h"
#include "Domains/Material/Authoring/UeAgentMaterialInstanceAuthoring.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialFunctionFactoryNew.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionComment.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionMaterialAttributeLayers.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/PackageName.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringNativeDefaults.h"

namespace UeAgentMaterialAuthoring
{
	UeAgentAuthoringState::FEditablePropertyReadOptions ExpressionPropertyPolicy(const UMaterialExpression& Expression)
	{
		UeAgentAuthoringState::FEditablePropertyReadOptions Options;
		Options.SemanticId = ExportExpressionPropertySemanticId;
		Options.ExcludedProperties = {TEXT("MaterialFunction"), TEXT("Inputs"), TEXT("MaterialExpressionGuid")};
		if (Expression.IsA<UMaterialExpressionMaterialAttributeLayers>()) Options.ExcludedProperties.Add(TEXT("DefaultLayers"));
		if (Expression.IsA<UMaterialExpressionComment>())
			Options.Limits.AuthorizeProperty = [](const FProperty* Property) {
				return Property && Property->GetOwnerStruct() == UMaterialExpressionComment::StaticClass()
					&& (Property->GetFName() == GET_MEMBER_NAME_CHECKED(UMaterialExpressionComment, SizeX)
						|| Property->GetFName() == GET_MEMBER_NAME_CHECKED(UMaterialExpressionComment, SizeY));
			};
		return Options;
	}

	namespace
	{
		UE::UAI::Contract::FDiagnostic StateFailure(const TCHAR* Code, const FString& Reason,
			const FString& Field, const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected,
			const UObject* Owner = nullptr)
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("state_capture"));
			Error.PropertyPath = Field;
			Error.ActualValue = Actual;
			Error.ActualValuePresent = Actual.IsValid();
			Error.ExpectedValue = Expected;
			Error.SourceKind = TEXT("native_result");
			if (Owner) Error.TargetPath = Owner->GetPathName();
			return Error;
		}
		bool BuildIdentity(
			const FString& ModelId,
			const FString& TargetKind,
			const TMap<FString, FString>& Selectors,
			UeAgentAuthoringIdentity::FCanonicalIdentity& OutIdentity,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			return UeAgentAuthoringIdentity::BuildCanonicalIdentity(
				ModelId, TargetKind, Selectors, OutIdentity, OutError);
		}

		bool AddReadRequest(
			const FString& SemanticId,
			const FString& ModelId,
			const FString& TargetKind,
			const TMap<FString, FString>& Selectors,
			const UObject* RootObject,
			const TMap<FString, FString>& Context,
			TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			UeAgentAuthoringState::FReadRequest Request;
			Request.SemanticId = SemanticId;
			if (!BuildIdentity(ModelId, TargetKind, Selectors, Request.TargetIdentity, OutError)) return false;
			Request.TargetQuery.ModelId = Request.TargetIdentity.ModelId;
			Request.TargetQuery.TargetKind = Request.TargetIdentity.TargetKind;
			Request.TargetQuery.CanonicalKey = Request.TargetIdentity.CanonicalKey;
			Request.RootObject = RootObject;
			Request.Context = Context;
			OutRequests.Add(MoveTemp(Request));
			return true;
		}

		UObject* ResolveRequestObject(const UeAgentAuthoringState::FReadRequest& Request)
		{
			UE::UAI::Contract::FDiagnostic ResolveError;
			UObject* Object = UeAgentAuthoringState::ResolveRequestRootObject(Request, ResolveError);
			if (!IsValid(Object) || GetAssetKind(Object).IsEmpty()) return nullptr;
			return Object;
		}

		TSharedRef<FJsonObject> BuildExportTargetState(const UObject& Asset, const FString& AssetPath)
		{
			TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
			State->SetStringField(TEXT("asset_path"), AssetPath);
			State->SetStringField(TEXT("asset_kind"), GetAssetKind(&Asset));
			if (const UMaterialInstance* Instance =
				Cast<UMaterialInstance>(&Asset))
			{
				State->SetStringField(
					TEXT("parent_path"),
					GetPathNameSafe(Instance->Parent));
			}
			return State;
		}


		TSharedRef<FJsonObject> BuildExportExpressionState(const UMaterialExpression& Expression, UE::UAI::Contract::FDiagnostic& OutError)
		{
			TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
			const FString ExpressionGuid = Expression.MaterialExpressionGuid.ToString(
				EGuidFormats::DigitsWithHyphensLower);
			State->SetStringField(TEXT("id"), ExpressionGuid);
			FString ExpressionName = Expression.GetClass()->GetName();
			constexpr const TCHAR* ExpressionClassPrefix = TEXT("MaterialExpression");
			if (ExpressionName.StartsWith(ExpressionClassPrefix))
			{
				ExpressionName.RightChopInline(
					FCString::Strlen(ExpressionClassPrefix), EAllowShrinking::No);
			}
			State->SetStringField(TEXT("name"), ExpressionName);
			TSharedRef<FJsonObject> CandidateContext = MakeShared<FJsonObject>();
			CandidateContext->SetStringField(TEXT("expression_class"), Expression.GetClass()->GetPathName());
			State->SetObjectField(TEXT("candidate_context"), CandidateContext);
			TSharedRef<FJsonObject> Position = MakeShared<FJsonObject>();
			Position->SetNumberField(TEXT("x"), Expression.MaterialExpressionEditorX);
			Position->SetNumberField(TEXT("y"), Expression.MaterialExpressionEditorY);
			State->SetObjectField(TEXT("pos"), Position);
			if (const UMaterialExpressionMaterialFunctionCall* FunctionCall =
				Cast<UMaterialExpressionMaterialFunctionCall>(&Expression))
			{
				if (FunctionCall->MaterialFunction)
				{
						CandidateContext->SetStringField(
							TEXT("material_function_path"), FunctionCall->MaterialFunction->GetPathName());
				}
			}
			if (const UMaterialExpressionCustom* Custom = Cast<UMaterialExpressionCustom>(&Expression))
			{
				TArray<TSharedPtr<FJsonValue>> Inputs;
				for (const FCustomInput& Input : Custom->Inputs)
				{
					if (Input.InputName.IsNone()) continue;
					Inputs.Add(MakeShared<FJsonValueString>(Input.InputName.ToString()));
				}
				if (!Inputs.IsEmpty()) State->SetArrayField(TEXT("custom_inputs"), Inputs);
			}
			if (const auto* LayerExpression = Cast<UMaterialExpressionMaterialAttributeLayers>(&Expression))
			{
				auto Layers = LayerExpression->DefaultLayers;
				TSharedPtr<FJsonValue> Value;
				if (CaptureLayerStack(const_cast<UMaterialExpression&>(Expression), Layers, Value, OutError))
					State->SetField(TEXT("layer_stack"), Value);
			}
			return State;
		}

		TSharedRef<FJsonObject> BuildExportPinEndpoint(
			const FString& Node, const FString& Pin, const int32 Occurrence = 0)
		{
			auto Endpoint = MakeShared<FJsonObject>();
			Endpoint->SetStringField(TEXT("node"), Node);
			Endpoint->SetStringField(TEXT("pin"), Pin);
			Endpoint->SetNumberField(TEXT("pin_occurrence"), Occurrence);
			return Endpoint;
		}

		TSharedRef<FJsonObject> BuildExportEdgeState(
			const UeAgentAuthoringState::FReadRequest& Request)
		{
			TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
			const auto From = BuildExportPinEndpoint(Request.Context.FindRef(TEXT("from_guid")),
				Request.Context.FindRef(TEXT("output")), FCString::Atoi(*Request.Context.FindRef(TEXT("output_occurrence"))));
			const auto To = BuildExportPinEndpoint(Request.Context.FindRef(TEXT("to_guid")),
				Request.Context.FindRef(TEXT("input")), FCString::Atoi(*Request.Context.FindRef(TEXT("input_occurrence"))));
			State->SetObjectField(TEXT("from"), From);
			State->SetObjectField(TEXT("to"), To);
			return State;
		}

		TSharedRef<FJsonObject> BuildExportRootInputState(
			const UeAgentAuthoringState::FReadRequest& Request)
		{
			TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
			const auto From = BuildExportPinEndpoint(Request.Context.FindRef(TEXT("from_guid")),
				Request.Context.FindRef(TEXT("output")), FCString::Atoi(*Request.Context.FindRef(TEXT("output_occurrence"))));
			State->SetObjectField(TEXT("from"), From);
			const auto To = BuildExportPinEndpoint(TEXT("material_root"), Request.Context.FindRef(TEXT("material_property")));
			State->SetObjectField(TEXT("to"), To);
			return State;
		}

		TSharedRef<FJsonObject> BuildAssetState(
			const UObject& Asset,
			const FString& CanonicalAssetPath,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
			const FString AssetPath = CanonicalAssetPath.IsEmpty() ? Asset.GetOutermost()->GetName() : CanonicalAssetPath;
			State->SetStringField(
				TEXT("asset_path"),
				AssetPath);
			State->SetStringField(TEXT("asset_kind"), GetAssetKind(&Asset));
			if (const auto* Instance = Cast<UMaterialInstanceConstant>(&Asset))
			{
				TArray<TSharedPtr<FJsonValue>> Parameters;
				if (!CaptureInstanceParameters(*const_cast<UMaterialInstanceConstant*>(Instance), Parameters, OutError))
				{
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = AssetPath;
					return State;
				}
				State->SetStringField(TEXT("parent_path"), GetPathNameSafe(Instance->Parent));
				State->SetArrayField(TEXT("parameters"), MoveTemp(Parameters));
				TSharedPtr<FJsonValue> Layers;
				if (!CaptureInstanceLayers(*const_cast<UMaterialInstanceConstant*>(Instance), Layers, OutError)) return State;
				State->SetField(TEXT("layer_stack"), Layers);
				return State;
			}
			if (const UMaterialFunction* Function = Cast<UMaterialFunction>(&Asset))
			{
				State->SetStringField(TEXT("description"), Function->Description);
				State->SetBoolField(TEXT("expose_to_library"), Function->bExposeToLibrary);
			}
			TSharedPtr<FJsonObject> Graph;
			if (!BuildGraphState(Asset, Graph, OutError)) return State;
			State->SetObjectField(TEXT("graph"), Graph);
			return State;
		}

		TSharedRef<FJsonObject> BuildExportRootNodeState(const UMaterial& Material)
		{
			TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
			State->SetStringField(TEXT("id"), TEXT("material_root"));
			State->SetStringField(TEXT("name"), TEXT("MaterialOutput"));
			TSharedRef<FJsonObject> Position = MakeShared<FJsonObject>();
			Position->SetNumberField(TEXT("x"), Material.EditorX);
			Position->SetNumberField(TEXT("y"), Material.EditorY);
			State->SetObjectField(TEXT("pos"), Position);
			return State;
		}

		TSharedRef<FJsonObject> BuildExportGraphState(
			const UObject& Asset,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			TSharedRef<FJsonObject> Graph = MakeShared<FJsonObject>();
			TSharedPtr<FJsonObject> Raw;
			if (!BuildGraphState(Asset, Raw, OutError)) return Graph;
			const TArray<TSharedPtr<FJsonValue>>* Expressions = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* RawEdges = nullptr;
			if (!Raw->TryGetArrayField(TEXT("expressions"), Expressions)
				|| !Raw->TryGetArrayField(TEXT("edges"), RawEdges)
				|| !Expressions || !RawEdges)
			{
				const bool bExpressionsInvalid = !Expressions;
				OutError = StateFailure(TEXT("material_export_graph_state_invalid"),
					TEXT("Native material graph export did not provide the required expression and edge arrays."),
					bExpressionsInvalid ? TEXT("expressions") : TEXT("edges"),
					Raw->TryGetField(bExpressionsInvalid ? TEXT("expressions") : TEXT("edges")), MakeShared<FJsonValueString>(TEXT("array")), &Asset);
				return Graph;
			}
			TSharedRef<FJsonObject> Nodes = MakeShared<FJsonObject>();
			const FMaterialExpressionCollection* Collection =
				GetExpressionCollection(Asset);
			for (const TSharedPtr<FJsonValue>& Value : *Expressions)
			{
				const TSharedPtr<FJsonObject> RawNode = Value.IsValid()
					&& Value->Type == EJson::Object ? Value->AsObject() : nullptr;
				if (!RawNode.IsValid()) {
					OutError = StateFailure(TEXT("material_export_graph_node_invalid"),
						FString::Printf(TEXT("Native material graph expression %d is not an object."), Expressions->IndexOfByKey(Value)),
						TEXT("expressions"), Value, MakeShared<FJsonValueString>(TEXT("object")), &Asset);
					return Graph;
				}
				FString Guid, ClassPath;
				if (!RawNode->TryGetStringField(TEXT("expression_guid"), Guid)
					|| Guid.IsEmpty()) {
					OutError = StateFailure(TEXT("material_export_graph_node_identity_invalid"),
						FString::Printf(TEXT("Native material graph expression %d has no non-empty expression GUID."), Expressions->IndexOfByKey(Value)),
						TEXT("expression_guid"), RawNode->TryGetField(TEXT("expression_guid")), MakeShared<FJsonValueString>(TEXT("non-empty GUID string")), &Asset);
					return Graph;
				}
				RawNode->TryGetStringField(TEXT("class_path"), ClassPath);
				FString Name = ClassPath;
				if (Name.StartsWith(TEXT("/Script/"))) Name = FPackageName::GetShortFName(*Name).ToString();
				if (Name.IsEmpty()) Name = TEXT("expression");
				TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
				Node->SetStringField(TEXT("id"), Guid);
				Node->SetStringField(TEXT("name"), Name);
				for (const TCHAR* Field : {TEXT("x"), TEXT("y"), TEXT("custom_inputs"), TEXT("layer_stack")})
				{
					if (const TSharedPtr<FJsonValue>* Found = RawNode->Values.Find(Field)) Node->SetField(Field, *Found);
				}
				TSharedRef<FJsonObject> Context = MakeShared<FJsonObject>();
				Context->SetStringField(TEXT("expression_class"), ClassPath);
				FString FunctionPath;
				RawNode->TryGetStringField(TEXT("material_function_path"), FunctionPath);
				if (!FunctionPath.IsEmpty()) Context->SetStringField(TEXT("material_function_path"), FunctionPath);
				Node->SetObjectField(TEXT("candidate_context"), Context);
				if (Collection)
				{
					for (UMaterialExpression* Expression : GetGraphExpressions(*Collection))
					{
						if (Expression
							&& Expression->MaterialExpressionGuid.ToString(
								EGuidFormats::DigitsWithHyphensLower) == Guid)
						{
							Node->SetObjectField(
								TEXT("pin_interface"),
								BuildExpressionPinInterface(*Expression));
							break;
						}
					}
				}
				Nodes->SetObjectField(Guid, Node);
			}
			if (const UMaterial* Material = Cast<UMaterial>(&Asset))
			{
				TSharedRef<FJsonObject> RootNode = BuildExportRootNodeState(*Material);
				TSharedRef<FJsonObject> RootInterface = MakeShared<FJsonObject>();
				TArray<TSharedPtr<FJsonValue>> RootPins;
				for (const EMaterialProperty Property : GetSupportedMaterialProperties())
				{
					TSharedRef<FJsonObject> Pin = MakeShared<FJsonObject>();
					Pin->SetStringField(
						TEXT("name"), MaterialPropertyToString(Property));
					Pin->SetStringField(TEXT("direction"), TEXT("input"));
					Pin->SetNumberField(TEXT("occurrence"), 0);
					RootPins.Add(MakeShared<FJsonValueObject>(Pin));
				}
				RootInterface->SetArrayField(TEXT("pins"), MoveTemp(RootPins));
				RootNode->SetObjectField(TEXT("pin_interface"), RootInterface);
				Nodes->SetObjectField(TEXT("material_root"), RootNode);
			}
			Graph->SetObjectField(TEXT("nodes"), Nodes);
			TSharedRef<FJsonObject> Edges = MakeShared<FJsonObject>();
			for (int32 Index = 0; Index < RawEdges->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> RawEdge = (*RawEdges)[Index].IsValid()
					&& (*RawEdges)[Index]->Type == EJson::Object ? (*RawEdges)[Index]->AsObject() : nullptr;
				if (!RawEdge.IsValid()) {
					OutError = StateFailure(TEXT("material_export_graph_edge_invalid"),
						FString::Printf(TEXT("Native material graph edge %d is not an object."), Index),
						TEXT("edges"), (*RawEdges)[Index], MakeShared<FJsonValueString>(TEXT("object")), &Asset);
					return Graph;
				}
				FString FromGuid, FromOutput, ToGuid, ToInput;
				RawEdge->TryGetStringField(TEXT("from_guid"), FromGuid);
				RawEdge->TryGetStringField(TEXT("output"), FromOutput);
				RawEdge->TryGetStringField(TEXT("to_guid"), ToGuid);
				RawEdge->TryGetStringField(TEXT("input"), ToInput);
				int32 FromOccurrence = 0, ToOccurrence = 0;
				RawEdge->TryGetNumberField(TEXT("output_occurrence"), FromOccurrence);
				RawEdge->TryGetNumberField(TEXT("input_occurrence"), ToOccurrence);
				const auto From = BuildExportPinEndpoint(FromGuid, FromOutput, FromOccurrence);
				const auto To = BuildExportPinEndpoint(ToGuid, ToInput, ToOccurrence);
				TSharedRef<FJsonObject> Edge = MakeShared<FJsonObject>();
				Edge->SetObjectField(TEXT("from"), From); Edge->SetObjectField(TEXT("to"), To);
				Edges->SetObjectField(FString::Printf(TEXT("edge_%d"), Index), Edge);
			}
			const TArray<TSharedPtr<FJsonValue>>* RawRootInputs = nullptr;
			if (Raw->TryGetArrayField(TEXT("root_inputs"), RawRootInputs)
				&& RawRootInputs)
			{
				for (int32 Index = 0; Index < RawRootInputs->Num(); ++Index)
				{
					const TSharedPtr<FJsonObject> RawRoot =
						(*RawRootInputs)[Index].IsValid()
						&& (*RawRootInputs)[Index]->Type == EJson::Object
							? (*RawRootInputs)[Index]->AsObject() : nullptr;
					if (!RawRoot.IsValid())
					{
						OutError = StateFailure(TEXT("material_export_root_input_invalid"),
							FString::Printf(TEXT("Native material root input %d is not an object."), Index),
							TEXT("root_inputs"), (*RawRootInputs)[Index], MakeShared<FJsonValueString>(TEXT("object")), &Asset);
						return Graph;
					}
					FString FromGuid;
					FString Output;
					FString Property;
					RawRoot->TryGetStringField(TEXT("from_guid"), FromGuid);
					RawRoot->TryGetStringField(TEXT("output"), Output);
					RawRoot->TryGetStringField(TEXT("material_property"), Property);
					int32 OutputOccurrence = 0;
					RawRoot->TryGetNumberField(TEXT("output_occurrence"), OutputOccurrence);
					const auto From = BuildExportPinEndpoint(FromGuid, Output, OutputOccurrence);
					const auto To = BuildExportPinEndpoint(TEXT("material_root"), Property);
					TSharedRef<FJsonObject> Edge = MakeShared<FJsonObject>();
					Edge->SetObjectField(TEXT("from"), From);
					Edge->SetObjectField(TEXT("to"), To);
					Edges->SetObjectField(
						FString::Printf(TEXT("root_input_%d"), Index), Edge);
				}
			}
			Graph->SetObjectField(TEXT("edges"), Edges);
			return Graph;
		}

		UeAgentAuthoringState::EReadOutcome ReadState(
			const UeAgentAuthoringState::FReadRequest& Request,
			UeAgentAuthoringState::FStateValue& OutValue,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutValue = UeAgentAuthoringState::FStateValue();
			OutValue.TargetIdentity = Request.TargetIdentity;
			OutValue.Type.Kind = TEXT("object");
			// Inherit the registered semantic capability. Export metadata is
			// read-only there; graph/document mutations have a native owner.
			// A blanket false would override the writable graph contract.
			OutValue.Provenance = StateAccessorId;
			OutError.Reset();

			const FString* ExpectedAbsent = Request.Context.Find(TEXT("expected_absent"));
			if (ExpectedAbsent && ExpectedAbsent->Equals(TEXT("true"), ESearchCase::IgnoreCase))
			{
				OutValue.bAvailable = false;
				OutValue.AvailabilityReason = TEXT("target_not_found");
				return UeAgentAuthoringState::EReadOutcome::Unavailable;
			}

			if (Request.SemanticId == ExportExpressionSemanticId)
			{
				const UMaterialExpression* Expression = Cast<UMaterialExpression>(Request.RootObject);
				if (!Expression)
				{
					OutError = StateFailure(TEXT("material_export_expression_target_invalid"),
						TEXT("Expression export requires a live MaterialExpression target."), TEXT("target_class"),
						Request.RootObject ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Request.RootObject->GetClass()->GetPathName())) : nullptr,
						MakeShared<FJsonValueString>(TEXT("MaterialExpression")), Request.RootObject);
					return UeAgentAuthoringState::EReadOutcome::Failed;
				}
				OutValue.Value = MakeShared<FJsonValueObject>(BuildExportExpressionState(*Expression, OutError));
				if (!OutError.IsEmpty()) return UeAgentAuthoringState::EReadOutcome::Failed;
				OutValue.bAvailable = true;
				return UeAgentAuthoringState::EReadOutcome::Resolved;
			}
			if (Request.SemanticId == ExportGraphSemanticId)
			{
				if (!Request.RootObject)
				{
					OutError = StateFailure(TEXT("material_export_graph_target_invalid"),
						TEXT("Material graph export target is no longer available."), TEXT("target_present"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return UeAgentAuthoringState::EReadOutcome::Failed;
				}
				OutValue.Value = MakeShared<FJsonValueObject>(
					BuildExportGraphState(*Request.RootObject, OutError));
				if (!OutError.IsEmpty()) return UeAgentAuthoringState::EReadOutcome::Failed;
				OutValue.bAvailable = true;
				return UeAgentAuthoringState::EReadOutcome::Resolved;
			}
			if (Request.SemanticId == ExportRootNodeSemanticId)
			{
				const UMaterial* Material = Cast<UMaterial>(Request.RootObject);
				if (!Material)
				{
					OutError = StateFailure(TEXT("material_export_root_node_target_invalid"),
						TEXT("Material root-node export requires a Material target."), TEXT("target_class"),
						Request.RootObject ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Request.RootObject->GetClass()->GetPathName())) : nullptr,
						MakeShared<FJsonValueString>(TEXT("Material")), Request.RootObject);
					return UeAgentAuthoringState::EReadOutcome::Failed;
				}
				OutValue.Value = MakeShared<FJsonValueObject>(BuildExportRootNodeState(*Material));
				OutValue.bAvailable = true;
				return UeAgentAuthoringState::EReadOutcome::Resolved;
			}
			if (Request.SemanticId == ExportInstanceLayersSemanticId)
			{
				auto* Instance = Cast<UMaterialInstanceConstant>(const_cast<UObject*>(Request.RootObject));
				if (!Instance || !CaptureInstanceLayers(*Instance, OutValue.Value, OutError))
					return UeAgentAuthoringState::EReadOutcome::Failed;
				auto LayersDefinition = MakeShared<FJsonObject>();
				auto LayersContract = MakeShared<FJsonObject>();
				LayersContract->SetStringField(TEXT("collection_semantics"), TEXT("keyed_desired_state"));
				LayersDefinition->SetStringField(TEXT("type"), TEXT("object[]"));
				LayersDefinition->SetObjectField(TEXT("contract"), LayersContract);
				auto Fields = MakeShared<FJsonObject>();
				Fields->SetObjectField(TEXT("layers"), LayersDefinition);
				auto Definition = MakeShared<FJsonObject>();
				Definition->SetObjectField(TEXT("fields"), Fields);
				OutValue.ValueDefinition = MakeShared<FJsonValueObject>(Definition);
				OutValue.bEditable = true;
				OutValue.bAvailable = true;
				return UeAgentAuthoringState::EReadOutcome::Resolved;
			}
			if (Request.SemanticId
				== ExportInstanceParameterSemanticId)
			{
				TArray<TSharedPtr<FJsonValue>> Parameters, Definitions;
				auto* Instance = Cast<UMaterialInstanceConstant>(const_cast<UObject*>(Request.RootObject));
				if (!Instance || !CaptureInstanceParameters(*Instance, Parameters, OutError, &Definitions))
				{
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Request.RootOwnerPath;
					return UeAgentAuthoringState::EReadOutcome::Failed;
				}
				for (int32 Index = 0; Index < Parameters.Num(); ++Index)
				{
					const auto State = Parameters[Index]->AsObject();
					if (State->GetStringField(TEXT("parameter_kind")) != Request.Context.FindRef(TEXT("parameter_kind"))
						|| State->GetStringField(TEXT("parameter_name")) != Request.Context.FindRef(TEXT("parameter_name"))
						|| State->GetStringField(TEXT("association")) != Request.Context.FindRef(TEXT("association"))
						|| State->GetIntegerField(TEXT("parameter_index")) != FCString::Atoi(*Request.Context.FindRef(TEXT("parameter_index")))) continue;
					OutValue.Value = Parameters[Index];
					auto Fields = MakeShared<FJsonObject>();
					Fields->SetField(TEXT("value"), Definitions[Index]);
					auto Definition = MakeShared<FJsonObject>();
					Definition->SetObjectField(TEXT("fields"), Fields);
					OutValue.ValueDefinition = MakeShared<FJsonValueObject>(Definition);
					OutValue.bEditable = true;
					OutValue.bAvailable = true;
					return UeAgentAuthoringState::EReadOutcome::Resolved;
				}
				TArray<TSharedPtr<FJsonValue>> Available;
				for (const auto& Parameter : Parameters)
				{
					const auto State = Parameter->AsObject();
					auto Choice = MakeShared<FJsonObject>();
					for (const TCHAR* Field : {TEXT("parameter_kind"), TEXT("parameter_name"), TEXT("association"), TEXT("parameter_index")})
						Choice->SetField(Field, State->TryGetField(Field));
					Available.Add(MakeShared<FJsonValueObject>(Choice));
				}
				OutError = StateFailure(TEXT("material_instance_parameter_identity_unresolved"),
					FString::Printf(TEXT("No material parameter matches '%s' (%s, %s, index %s)."),
						*Request.Context.FindRef(TEXT("parameter_name")), *Request.Context.FindRef(TEXT("parameter_kind")),
						*Request.Context.FindRef(TEXT("association")), *Request.Context.FindRef(TEXT("parameter_index"))),
					TEXT("parameter_name"), MakeShared<FJsonValueString>(Request.Context.FindRef(TEXT("parameter_name"))),
					MakeShared<FJsonValueString>(TEXT("existing parameter identity")), Request.RootObject);
				OutError.CandidateChoices = MakeShared<FJsonValueArray>(MoveTemp(Available));
				return UeAgentAuthoringState::EReadOutcome::Failed;
			}
			if (Request.SemanticId == ExportEdgeSemanticId)
			{
				OutValue.Value = MakeShared<FJsonValueObject>(BuildExportEdgeState(Request));
				OutValue.bAvailable = true;
				return UeAgentAuthoringState::EReadOutcome::Resolved;
			}
			if (Request.SemanticId == ExportRootInputSemanticId)
			{
				OutValue.Value = MakeShared<FJsonValueObject>(BuildExportRootInputState(Request));
				OutValue.bAvailable = true;
				return UeAgentAuthoringState::EReadOutcome::Resolved;
			}

			UObject* Object = ResolveRequestObject(Request);
			if (!Object)
			{
				OutValue.bAvailable = false;
				OutValue.AvailabilityReason = TEXT("target_not_found");
				return UeAgentAuthoringState::EReadOutcome::Unavailable;
			}
			const FString ExpectedKind = Request.SemanticId == MaterialSemanticId
				? TEXT("material")
				: Request.SemanticId == MaterialFunctionSemanticId
					|| Request.SemanticId == ExportFunctionTargetSemanticId
					|| Request.SemanticId == ExportFunctionSettingsSemanticId
					? TEXT("material_function")
					: Request.SemanticId == MaterialInstanceSemanticId
						|| Request.SemanticId
							== ExportInstanceTargetSemanticId
						? TEXT("material_instance")
					: Request.SemanticId == ExportTargetSemanticId
						? TEXT("material")
						: FString();
			if (ExpectedKind.IsEmpty() || GetAssetKind(Object) != ExpectedKind)
			{
				OutError = StateFailure(TEXT("material_state_target_type_invalid"),
					ExpectedKind.IsEmpty() ? TEXT("The requested material state has no supported asset kind.") : TEXT("Material state target does not have the required asset kind."),
					TEXT("asset_kind"), MakeShared<FJsonValueString>(GetAssetKind(Object)), MakeShared<FJsonValueString>(ExpectedKind), Object);
				OutError.SemanticId = Request.SemanticId;
				return UeAgentAuthoringState::EReadOutcome::Failed;
			}
			const FString AssetPath = Request.TargetIdentity.GetSelector(TEXT("asset_path"));
			TSharedRef<FJsonObject> State =
				Request.SemanticId == ExportTargetSemanticId
					|| Request.SemanticId == ExportFunctionTargetSemanticId
					|| Request.SemanticId
						== ExportInstanceTargetSemanticId
				? BuildExportTargetState(*Object, AssetPath)
				: Request.SemanticId == ExportFunctionSettingsSemanticId
					? [&Object]()
					{
						TSharedRef<FJsonObject> Settings = MakeShared<FJsonObject>();
						const UMaterialFunction* Function = CastChecked<UMaterialFunction>(Object);
						Settings->SetStringField(TEXT("description"), Function->Description);
						Settings->SetBoolField(TEXT("expose_to_library"), Function->bExposeToLibrary);
						return Settings;
					}()
					: BuildAssetState(*Object, AssetPath, OutError);
			if (!OutError.IsEmpty()) return UeAgentAuthoringState::EReadOutcome::Failed;
			OutValue.Value = MakeShared<FJsonValueObject>(State);
			OutValue.bAvailable = true;
			return UeAgentAuthoringState::EReadOutcome::Resolved;
		}

		UeAgentAuthoringNativeDefaults::EProbeOutcome ProbeNativeDefault(
			const UeAgentAuthoringNativeDefaults::FRequest& Request,
			UeAgentAuthoringNativeDefaults::FScratchScope& Scratch,
			UeAgentAuthoringNativeDefaults::FProbeValue& OutValue,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutValue = UeAgentAuthoringNativeDefaults::FProbeValue();
			OutError.Reset();
			const FString TargetKind = Request.TargetIdentity.TargetKind;
			const FString AssetKind = TargetKind == TEXT("material_asset")
				? TEXT("material")
				: TargetKind == TEXT("material_function")
					? TEXT("material_function")
					: FString();
			if (AssetKind.IsEmpty()) return UeAgentAuthoringNativeDefaults::EProbeOutcome::Unavailable;
			UPackage* Package = Scratch.CreateScratchPackage(OutError);
			if (!Package) return UeAgentAuthoringNativeDefaults::EProbeOutcome::Failed;
			UObject* Seed = CreateFactorySeed(
				AssetKind, Package, NAME_None, RF_Transactional, OutError);
			if (!Seed) return UeAgentAuthoringNativeDefaults::EProbeOutcome::Failed;
			TSharedRef<FJsonObject> State = BuildAssetState(*Seed, FString(), OutError);
			if (!OutError.IsEmpty()) return UeAgentAuthoringNativeDefaults::EProbeOutcome::Failed;
			OutValue.Value = MakeShared<FJsonValueObject>(State);
			OutValue.Provenance = FactoryDefaultProviderId;
			return UeAgentAuthoringNativeDefaults::EProbeOutcome::Resolved;
		}
	}

	UObject* CreateFactorySeed(
		const FString& AssetKind,
		UObject* Outer,
		const FName Name,
		const EObjectFlags Flags,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		if (!Outer)
		{
			OutError = StateFailure(TEXT("material_factory_outer_missing"),
				FString::Printf(TEXT("Cannot create %s '%s' without an outer package."), *AssetKind, *Name.ToString()),
				TEXT("outer_present"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return nullptr;
		}
		if (AssetKind == TEXT("material"))
		{
			UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();
			UObject* Result = Factory->FactoryCreateNew(
				UMaterial::StaticClass(), Outer, Name, Flags, nullptr, GWarn);
			if (!Result) OutError = StateFailure(TEXT("material_factory_create_failed"),
				FString::Printf(TEXT("UE's material factory could not create '%s'."), *Name.ToString()),
				TEXT("asset_created"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			if (!Result) OutError.InternalContext = Outer->GetPathName();
			return Result;
		}
		if (AssetKind == TEXT("material_function"))
		{
			UMaterialFunctionFactoryNew* Factory = NewObject<UMaterialFunctionFactoryNew>();
			UObject* Result = Factory->FactoryCreateNew(
				UMaterialFunction::StaticClass(), Outer, Name, Flags, nullptr, GWarn);
			if (!Result) OutError = StateFailure(TEXT("material_function_factory_create_failed"),
				FString::Printf(TEXT("UE's material function factory could not create '%s'."), *Name.ToString()),
				TEXT("asset_created"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			if (!Result) OutError.InternalContext = Outer->GetPathName();
			return Result;
		}
		OutError = StateFailure(TEXT("material_factory_kind_invalid"), TEXT("Material factory requires material or material_function asset kind."),
			TEXT("asset_kind"), MakeShared<FJsonValueString>(AssetKind), MakeShared<FJsonValueString>(TEXT("material or material_function")));
		OutError.InternalContext = Outer->GetPathName();
		return nullptr;
	}

	bool BuildAssetSnapshotReadRequest(
		UObject* Asset,
		const FString& CanonicalAssetPath,
		const FString& AssetKind,
		const bool bExpectedAbsent,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		FString Kind = GetAssetKind(Asset);
		if (Kind.IsEmpty()) Kind = AssetKind;
		const FString SemanticId = Kind == TEXT("material")
			? MaterialSemanticId
			: Kind == TEXT("material_function")
				? MaterialFunctionSemanticId
				: Kind == TEXT("material_instance") ? MaterialInstanceSemanticId : FString();
		const FString IdentityModelId = Kind == TEXT("material")
			? MaterialIdentityModelId
			: Kind == TEXT("material_function")
				? MaterialFunctionIdentityModelId
				: Kind == TEXT("material_instance") ? MaterialInstanceIdentityModelId : FString();
		const FString TargetKind = Kind == TEXT("material")
			? TEXT("material_asset")
			: Kind == TEXT("material_function")
				? TEXT("material_function")
				: Kind == TEXT("material_instance") ? TEXT("material_instance") : FString();
		if (SemanticId.IsEmpty() || CanonicalAssetPath.IsEmpty())
		{
			OutError = StateFailure(TEXT("material_snapshot_identity_invalid"),
				SemanticId.IsEmpty() ? TEXT("Material snapshot requires a supported material asset kind.") : TEXT("Material snapshot requires a non-empty asset path."),
				SemanticId.IsEmpty() ? TEXT("asset_kind") : TEXT("asset_path"),
				MakeShared<FJsonValueString>(SemanticId.IsEmpty() ? Kind : CanonicalAssetPath),
				MakeShared<FJsonValueString>(SemanticId.IsEmpty() ? TEXT("material, material_function, or material_instance") : TEXT("non-empty asset path")), Asset);
			return false;
		}
		UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
		if (!UeAgentAuthoringIdentity::BuildCanonicalIdentity(
			IdentityModelId,
			TargetKind,
			{ { TEXT("asset_path"), CanonicalAssetPath } },
			Identity,
			OutError))
		{
			return false;
		}
		UeAgentAuthoringState::FReadRequest& Request = OutRequests.AddDefaulted_GetRef();
		Request.SemanticId = SemanticId;
		Request.TargetIdentity = MoveTemp(Identity);
		Request.TargetQuery.ModelId = Request.TargetIdentity.ModelId;
		Request.TargetQuery.TargetKind = TargetKind;
		Request.TargetQuery.CanonicalKey = Request.TargetIdentity.CanonicalKey;
		Request.RootObject = Asset;
		Request.RootOwnerPath = CanonicalAssetPath;
		if (bExpectedAbsent) Request.Context.Add(TEXT("expected_absent"), TEXT("true"));
		return true;
	}

	bool BuildAssetExportSnapshotReadRequests(
		UObject& Asset,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutRequests.Reset();
		OutError.Reset();
		const FString AssetKind = GetAssetKind(&Asset);
		const FString AssetPath = Asset.GetOutermost()->GetName();
		const bool bFunction = AssetKind == TEXT("material_function");
		const bool bInstance = AssetKind == TEXT("material_instance");
		if (AssetKind != TEXT("material") && !bFunction && !bInstance)
		{
			OutError = StateFailure(TEXT("material_export_asset_type_invalid"),
				TEXT("Material export requires a Material, MaterialFunction, or MaterialInstanceConstant asset."),
				TEXT("asset_class"), MakeShared<FJsonValueString>(Asset.GetClass()->GetPathName()),
				MakeShared<FJsonValueString>(TEXT("Material, MaterialFunction, or MaterialInstanceConstant")), &Asset);
			return false;
		}

		const FString AssetModel = bFunction
			? MaterialFunctionIdentityModelId
			: bInstance
				? MaterialInstanceIdentityModelId
				: MaterialIdentityModelId;
		const FString AssetTargetKind = bFunction
			? TEXT("material_function")
			: bInstance
				? TEXT("material_instance")
				: TEXT("material_asset");
		const FString TargetSemantic = bFunction
			? ExportFunctionTargetSemanticId
			: bInstance
				? ExportInstanceTargetSemanticId
				: ExportTargetSemanticId;
		if (!AddReadRequest(
			TargetSemantic, AssetModel, AssetTargetKind,
			{ { TEXT("asset_path"), AssetPath } }, &Asset, {}, OutRequests, OutError))
		{
			return false;
		}
		if (!bInstance)
		{
			if (!AddReadRequest(
				ExportGraphSemanticId,
				MaterialGraphIdentityModelId,
				TEXT("material_graph"),
				{ { TEXT("asset_path"), AssetPath } },
				&Asset, {}, OutRequests, OutError))
			{
				return false;
			}
			if (!bFunction && !AddReadRequest(
				ExportRootNodeSemanticId,
				MaterialRootNodeIdentityModelId,
				TEXT("material_root_node"),
				{ { TEXT("asset_path"), AssetPath } },
				&Asset, {}, OutRequests, OutError))
			{
				return false;
			}
		}

		UeAgentAuthoringState::FEditablePropertyReadOptions OwnerPropertyOptions;
		OwnerPropertyOptions.SemanticId = TEXT("core.property");
		if (bInstance)
		{
			OwnerPropertyOptions.ExcludedProperties = {
				TEXT("ScalarParameterValues"),
				TEXT("VectorParameterValues"),
				TEXT("DoubleVectorParameterValues"),
				TEXT("TextureParameterValues"),
				TEXT("TextureCollectionParameterValues"),
				TEXT("RuntimeVirtualTextureParameterValues"),
				TEXT("SparseVolumeTextureParameterValues"),
				TEXT("FontParameterValues")};
		}
		if (!UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(
			&Asset, OwnerPropertyOptions, OutRequests, OutError))
		{
			return false;
		}

		if (bInstance)
		{
			UMaterialInstanceConstant* Instance = CastChecked<UMaterialInstanceConstant>(&Asset);
			if (!AddReadRequest(ExportInstanceLayersSemanticId, MaterialInstanceLayersIdentityModelId,
				TEXT("material_instance_layers"), {{TEXT("asset_path"), AssetPath}, {TEXT("selector"), TEXT("layer_stack")}},
				&Asset, {}, OutRequests, OutError)) return false;
			auto AddParameter = [
				&AssetPath,
				&Asset,
				&OutRequests,
				&OutError](
					const FString& Kind,
					const FMaterialParameterInfo& Info,
					TMap<FString, FString> Context)
			{
				const FString Association = Info.Association == EMaterialParameterAssociation::GlobalParameter
					? TEXT("global") : Info.Association == EMaterialParameterAssociation::LayerParameter ? TEXT("layer") : TEXT("blend");
				const FString Index = FString::FromInt(Info.Index);
				Context.Add(TEXT("parameter_kind"), Kind);
				Context.Add(
					TEXT("parameter_name"),
					Info.Name.ToString());
				Context.Add(TEXT("association"), Association);
				Context.Add(TEXT("parameter_index"), Index);
				return AddReadRequest(
					ExportInstanceParameterSemanticId,
					MaterialInstanceParameterIdentityModelId,
					TEXT("material_instance_parameter"),
					{
						{TEXT("asset_path"), AssetPath},
						{TEXT("parameter_kind"), Kind},
						{TEXT("parameter_name"), Info.Name.ToString()},
						{TEXT("association"), Association},
						{TEXT("parameter_index"), Index}},
					&Asset,
					Context,
					OutRequests,
					OutError);
			};
			TArray<TSharedPtr<FJsonValue>> Parameters;
			if (!CaptureInstanceParameters(*Instance, Parameters, OutError))
			{
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = AssetPath;
				return false;
			}
			for (const auto& Parameter : Parameters)
			{
				const auto Value = Parameter->AsObject();
				const FString Kind = Value->GetStringField(TEXT("parameter_kind"));
				const FString Association = Value->GetStringField(TEXT("association"));
				const FMaterialParameterInfo Info(FName(*Value->GetStringField(TEXT("parameter_name"))),
					Association == TEXT("global") ? EMaterialParameterAssociation::GlobalParameter
					: Association == TEXT("layer") ? EMaterialParameterAssociation::LayerParameter : EMaterialParameterAssociation::BlendParameter,
					static_cast<int32>(Value->GetNumberField(TEXT("parameter_index"))));
				TMap<FString, FString> Context;

				if (!AddParameter(Kind, Info, MoveTemp(Context))) return false;
			}
			return true;
		}

		TSharedPtr<FJsonObject> GraphState;
		if (!BuildGraphState(Asset, GraphState, OutError)) return false;
		const TArray<TSharedPtr<FJsonValue>>* Expressions = nullptr;
		if (!GraphState->TryGetArrayField(TEXT("expressions"), Expressions) || !Expressions)
		{
			OutError = StateFailure(TEXT("material_export_expression_state_missing"),
				TEXT("Native material graph state has no expression array."),
				TEXT("expressions"), GraphState->TryGetField(TEXT("expressions")), MakeShared<FJsonValueString>(TEXT("array")), &Asset);
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Expressions)
		{
			const TSharedPtr<FJsonObject>* ExpressionState = nullptr;
			FString GuidText;
			FGuid Guid;
			if (!Value.IsValid() || !Value->TryGetObject(ExpressionState)
				|| !ExpressionState || !ExpressionState->IsValid()
				|| !(*ExpressionState)->TryGetStringField(TEXT("expression_guid"), GuidText)
				|| !FGuid::Parse(GuidText, Guid))
			{
				OutError = StateFailure(TEXT("material_export_expression_identity_invalid"),
					FString::Printf(TEXT("Native material expression %d requires an object with a parseable expression GUID."), Expressions->IndexOfByKey(Value)),
					TEXT("expression_guid"), ExpressionState && ExpressionState->IsValid() ? (*ExpressionState)->TryGetField(TEXT("expression_guid")) : Value,
					MakeShared<FJsonValueString>(TEXT("expression object with a GUID")), &Asset);
				return false;
			}
			FExpressionSelector Selector;
			Selector.ExpressionGuid = Guid;
			UMaterialExpression* Expression = ResolveExpressionUnique(Asset, Selector, OutError);
			if (!Expression) return false;
			if (!AddReadRequest(
				ExportExpressionSemanticId,
				MaterialExpressionIdentityModelId,
				TEXT("material_expression"),
				{ { TEXT("asset_path"), AssetPath }, { TEXT("expression_guid"), GuidText } },
				Expression,
				{},
				OutRequests,
				OutError))
			{
				return false;
			}

			auto PropertyOptions = ExpressionPropertyPolicy(*Expression);
			PropertyOptions.BuildSnapshotIdentity =
				[AssetPath, GuidText](
					const FString& PropertyPath,
					UeAgentAuthoringIdentity::FCanonicalIdentity& OutIdentity,
					UE::UAI::Contract::FDiagnostic& IdentityError)
				{
					return BuildIdentity(
						MaterialExpressionPropertyIdentityModelId,
						TEXT("material_expression_property"),
						{ { TEXT("asset_path"), AssetPath },
							{ TEXT("expression_guid"), GuidText },
							{ TEXT("property_path"), PropertyPath } },
						OutIdentity,
						IdentityError);
				};
			if (!UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(
				Expression, PropertyOptions, OutRequests, OutError))
			{
				return false;
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Edges = nullptr;
		if (GraphState->TryGetArrayField(TEXT("edges"), Edges) && Edges)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Edges)
			{
				const TSharedPtr<FJsonObject>* Edge = nullptr;
				if (!Value.IsValid() || !Value->TryGetObject(Edge) || !Edge || !Edge->IsValid())
				{
					OutError = StateFailure(TEXT("material_export_edge_state_invalid"),
						FString::Printf(TEXT("Native material edge %d is not an object."), Edges->IndexOfByKey(Value)),
						TEXT("edges"), Value, MakeShared<FJsonValueString>(TEXT("object")), &Asset);
					return false;
				}
				TMap<FString, FString> Context;
				for (const TCHAR* Field : { TEXT("from_guid"), TEXT("output"), TEXT("to_guid"), TEXT("input") })
				{
					FString FieldValue;
					(*Edge)->TryGetStringField(Field, FieldValue);
					Context.Add(Field, FieldValue);
				}
				for (const TCHAR* Field : { TEXT("output_occurrence"), TEXT("input_occurrence") })
				{
					int32 Occurrence = 0;
					(*Edge)->TryGetNumberField(Field, Occurrence);
					Context.Add(Field, FString::FromInt(Occurrence));
				}
				const FString EdgeKey = Context[TEXT("from_guid")] + TEXT(":") + Context[TEXT("output")]
					+ TEXT("#") + Context[TEXT("output_occurrence")]
					+ TEXT("->") + Context[TEXT("to_guid")] + TEXT(":") + Context[TEXT("input")]
					+ TEXT("#") + Context[TEXT("input_occurrence")];
				if (!AddReadRequest(
					ExportEdgeSemanticId, MaterialEdgeIdentityModelId, TEXT("material_edge"),
					{ { TEXT("asset_path"), AssetPath }, { TEXT("edge_key"), EdgeKey } },
					&Asset, Context, OutRequests, OutError))
				{
					return false;
				}
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* RootInputs = nullptr;
		if (GraphState->TryGetArrayField(TEXT("root_inputs"), RootInputs) && RootInputs)
		{
			for (const TSharedPtr<FJsonValue>& Value : *RootInputs)
			{
				const TSharedPtr<FJsonObject>* RootInput = nullptr;
				FString MaterialProperty;
				TMap<FString, FString> Context;
				if (!Value.IsValid() || !Value->TryGetObject(RootInput)
					|| !RootInput || !RootInput->IsValid()
					|| !(*RootInput)->TryGetStringField(TEXT("material_property"), MaterialProperty))
				{
					OutError = StateFailure(TEXT("material_export_root_input_state_invalid"),
						FString::Printf(TEXT("Native material root input %d requires an object with a string material_property."), RootInputs->IndexOfByKey(Value)),
						TEXT("material_property"), RootInput && RootInput->IsValid() ? (*RootInput)->TryGetField(TEXT("material_property")) : Value,
						MakeShared<FJsonValueString>(TEXT("root input object with material_property string")), &Asset);
					return false;
				}
				FString FromGuid;
				FString Output;
				(*RootInput)->TryGetStringField(TEXT("from_guid"), FromGuid);
				(*RootInput)->TryGetStringField(TEXT("output"), Output);
				Context.Add(TEXT("from_guid"), FromGuid);
				Context.Add(TEXT("output"), Output);
				int32 OutputOccurrence = 0;
				(*RootInput)->TryGetNumberField(TEXT("output_occurrence"), OutputOccurrence);
				Context.Add(TEXT("output_occurrence"), FString::FromInt(OutputOccurrence));
				Context.Add(TEXT("material_property"), MaterialProperty);
				if (!AddReadRequest(
					ExportRootInputSemanticId,
					MaterialRootInputIdentityModelId,
					TEXT("material_root_input"),
					{ { TEXT("asset_path"), AssetPath }, { TEXT("material_property"), MaterialProperty } },
					&Asset,
					Context,
					OutRequests,
					OutError))
				{
					return false;
				}
			}
		}
		return true;
	}

	bool RegisterStateAccessor(UE::UAI::Contract::FDiagnostic& OutError)
	{
		UeAgentAuthoringState::FStateAccessorDescriptor Descriptor;
		Descriptor.AccessorId = StateAccessorId;
		Descriptor.TargetKinds = {
			TEXT("material_asset"), TEXT("material_function"), TEXT("material_instance"),
			TEXT("material_graph"), TEXT("material_instance_parameter"), TEXT("material_instance_layers"),
			TEXT("material_expression"),
			TEXT("material_edge"), TEXT("material_root_node"),
			TEXT("material_root_input")
		};
		Descriptor.CapabilityTags = { TEXT("asset"), TEXT("graph"), TEXT("expression") };
		UeAgentAuthoringState::ERegistrationResult Result;
		return UeAgentAuthoringState::FStateAccessorRegistry::Get().RegisterAccessor(
			Descriptor, ReadState, Result, OutError);
	}

	bool RegisterNativeDefaultProvider(UE::UAI::Contract::FDiagnostic& OutError)
	{
		UeAgentAuthoringNativeDefaults::FProviderDescriptor Descriptor;
		Descriptor.ProviderId = FactoryDefaultProviderId;
		Descriptor.SourceKind = UeAgentAuthoringSemanticRegistry::EDefaultSourceKind::Factory;
		Descriptor.TargetKinds = { TEXT("material_asset"), TEXT("material_function") };
		Descriptor.CapabilityTags = { TEXT("factory"), TEXT("asset"), TEXT("graph") };
		Descriptor.bRequiresScratch = true;
		UeAgentAuthoringNativeDefaults::ERegistrationResult Result;
		return UeAgentAuthoringNativeDefaults::FProviderRegistry::Get().RegisterProvider(
			Descriptor, ProbeNativeDefault, Result, OutError);
	}
}

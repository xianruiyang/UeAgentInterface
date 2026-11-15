// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/PCG/Authoring/UeAgentPCGGraphAuthoring.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/UeAgentJsonDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringEncodedTypes.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringJsonFields.h"

#include "Domains/PCG/UeAgentPCGDomainBackend.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceIO.h"
#include "Core/HjsonWorkspace/UeAgentGraphAuthoringProjection.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Domains/PCG/Authoring/UeAgentPCGAuthoringDescriptors.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/ScopeExit.h"
#include "PCGEdge.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGSettings.h"
#include "QueryOwners/Pcg/UeAgentPcgNodeCandidateQuery.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringDirectApply.h"
#include "Utilities/UeAgentDetailsAuthoring.h"
#include "UObject/UObjectGlobals.h"

namespace UeAgentPCGAuthoring
{
	namespace
	{
		void BindLogicalGraphTarget(UE::UAI::Contract::FDiagnostic& Error, const UPCGGraph& Graph, const FString& LogicalTarget)
		{
			if (Error.IsEmpty() || LogicalTarget.IsEmpty()) return;
			const FString NativeTarget = Graph.GetPathName();
			for (auto& Child : Error.Diagnostics) BindLogicalGraphTarget(Child, Graph, LogicalTarget);
			if (Error.PublicCause)
			{
				auto Cause = MakeShared<UE::UAI::Contract::FDiagnostic>(*Error.PublicCause);
				BindLogicalGraphTarget(*Cause, Graph, LogicalTarget);
				Error.PublicCause = MoveTemp(Cause);
			}
			if (Error.TargetPath.IsEmpty()) Error.TargetPath = LogicalTarget;
			else if (Error.TargetPath == NativeTarget || Error.TargetPath.StartsWith(NativeTarget + TEXT("."))
				|| Error.TargetPath.StartsWith(NativeTarget + TEXT(":")))
			{
				if (Error.TargetPath != LogicalTarget)
					Error.InternalContext += (Error.InternalContext.IsEmpty() ? FString() : TEXT("\n")) + Error.TargetPath;
				Error.TargetPath = LogicalTarget;
			}
		}

		UE::UAI::Contract::FDiagnostic GraphAuthoringFailure(const TCHAR* Code, const FString& Reason,
			const FString& Field, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const bool bNativeResult = false)
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("graph_authoring"));
			Error.PropertyPath = Field;
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			Error.ActualValuePresent = Error.ActualValue.IsValid();
			Error.ExpectedValuePresent = Error.ExpectedValue.IsValid();
			if (bNativeResult) Error.SourceKind = TEXT("native_result");
			else if (Field.StartsWith(TEXT("/"))) Error.Source.JsonPath = Field;
			return Error;
		}

		UE::UAI::Contract::FDiagnostic GraphInputFailure(const TCHAR* Code, const FString& Reason,
			const FJsonObject& Object, const FString& Field, const FString& Expected)
		{
			auto Error = GraphAuthoringFailure(Code, Reason, Field, Object.TryGetField(Field), MakeShared<FJsonValueString>(Expected));
			UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, Object, Field);
			return Error;
		}
	}

	struct FGraphPropertyIntent
	{
		TMap<FName, TArray<UeAgentAuthoringObjectApply::FPropertyIntent>> Nodes;
		TMap<FName, FString> AuthoredNodeIds;
		FString TargetPath;
		TSharedPtr<const FJsonObject> Document;
		TMap<FString, UE::UAI::Contract::FSourceLocation> Sources;
	};

	bool VerifyGraphPropertyIntent(UPCGGraph& Graph, const FGraphApplyResult& Result, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!Result.PropertyIntent) return true;
		ON_SCOPE_EXIT { BindLogicalGraphTarget(OutError, Graph, Result.PropertyIntent->TargetPath); };
		UeAgentAuthoringDiagnostics::FScopedSourceLocations SourceScope(Result.PropertyIntent->Document.Get(),
			[&](const FString& Pointer) { return UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(Result.PropertyIntent->Sources, Pointer); });
		for (const auto& Pair : Result.PropertyIntent->Nodes)
		{
			UPCGNode* Node = FindObjectFast<UPCGNode>(&Graph, Pair.Key);
			UPCGSettings* Settings = Node ? Node->GetSettings() : nullptr;
			if (!Settings)
			{
				OutError = GraphAuthoringFailure(TEXT("pcg_node_properties_target_missing"), TEXT("The node and its settings must exist to verify the applied properties."),
					!Node ? TEXT("node_available") : TEXT("settings_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), true);
				OutError.InternalContext = Pair.Key.ToString();
				OutError.CandidateName = Result.PropertyIntent->AuthoredNodeIds.FindRef(Pair.Key);
				OutError.TargetPath = Result.PropertyIntent->TargetPath;
				if (OutError.TargetPath.IsEmpty() && Graph.IsAsset()) OutError.TargetPath = Graph.GetPathName();
				return false;
			}
			for (const auto& Intent : Pair.Value)
			{
				UeAgentAuthoringObjectApply::FApplyResult Verified;
				if (!Intent.VerifyObject(Settings, Verified))
				{
					OutError = Verified.Diagnostic;
					return false;
				}
			}
		}
		return true;
	}

	namespace
	{
		bool IsValidIdentityKey(const FString& Value)
		{
			if (Value.IsEmpty()) return false;
			for (const TCHAR Character : Value)
			{
				const bool bAllowed = FChar::IsAlnum(Character)
					|| Character == TEXT('_')
					|| Character == TEXT('-')
					|| Character == TEXT('.');
				if (!bAllowed) return false;
			}
			return true;
		}

		// A generated field retains the original semantic coordinate. Physical
		// positions remain those supplied by the workspace and are refined there.
		void RemapSourceField(FJsonObject& Destination, const FString& DestinationField,
			const FJsonObject& Source, const FString& SourceField, const FString& Suffix = FString())
		{
			const TSharedPtr<FJsonObject>* Location = nullptr;
			if (!Source.TryGetObjectField(TEXT("authoring_source"), Location) || !Location || !*Location) return;
			const TSharedPtr<FJsonObject>* Fields = nullptr;
			const TSharedPtr<FJsonObject>* FieldLocation = nullptr;
			const FJsonObject* Selected = Location->Get();
			if ((*Location)->TryGetObjectField(TEXT("fields"), Fields) && Fields && *Fields
				&& (*Fields)->TryGetObjectField(SourceField, FieldLocation) && FieldLocation && *FieldLocation)
				Selected = FieldLocation->Get();
			auto Alias = MakeShared<FJsonObject>();
			Alias->Values = Selected->Values;
			FString Unit, Pointer;
			if (!Selected->TryGetStringField(TEXT("source_unit"), Unit)
				|| !Selected->TryGetStringField(TEXT("internal_json_path"), Pointer))
			{
				if (!(*Location)->TryGetStringField(TEXT("source_unit"), Unit)
					|| !(*Location)->TryGetStringField(TEXT("internal_json_path"), Pointer)) return;
				Pointer += TEXT("/") + SourceField.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
			}
			Alias->SetStringField(TEXT("source_unit"), Unit);
			Alias->SetStringField(TEXT("internal_json_path"), Pointer + Suffix);
			const TSharedPtr<FJsonObject>* Existing = nullptr;
			auto NewLocation = MakeShared<FJsonObject>();
			if (Destination.TryGetObjectField(TEXT("authoring_source"), Existing) && Existing && *Existing)
				NewLocation->Values = (*Existing)->Values;
			else NewLocation->Values = (*Location)->Values;
			auto NewFields = MakeShared<FJsonObject>();
			if (NewLocation->TryGetObjectField(TEXT("fields"), Existing) && Existing && *Existing)
				NewFields->Values = (*Existing)->Values;
			NewFields->SetObjectField(DestinationField, Alias);
			NewLocation->SetObjectField(TEXT("fields"), NewFields);
			Destination.SetObjectField(TEXT("authoring_source"), NewLocation);
		}

		TSharedRef<FJsonObject> CloneObject(const TSharedPtr<FJsonObject>& Source)
		{
			return Source.IsValid()
				? UeAgentDetailsAuthoring::CloneObjectTree(Source).ToSharedRef()
				: MakeShared<FJsonObject>();
		}

		TSharedRef<FJsonObject> BuildPinMap(const TArray<TObjectPtr<UPCGPin>>& Pins)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			for (const TObjectPtr<UPCGPin>& PinPtr : Pins)
			{
				const UPCGPin* Pin = PinPtr.Get();
				if (!Pin || Pin->Properties.Label.IsNone()) continue;
				TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
				Value->SetStringField(
					TEXT("allowed_types"),
					StaticEnum<EPCGDataType>()->GetNameStringByValue(
						static_cast<int64>(Pin->Properties.AllowedTypes)));
				Value->SetStringField(
					TEXT("usage"),
					StaticEnum<EPCGPinUsage>()->GetNameStringByValue(
						static_cast<int64>(Pin->Properties.Usage)));
				Value->SetStringField(
					TEXT("status"),
					StaticEnum<EPCGPinStatus>()->GetNameStringByValue(
						static_cast<int64>(Pin->Properties.PinStatus)));
				Value->SetBoolField(TEXT("allow_multiple_data"), Pin->Properties.bAllowMultipleData);
				Value->SetBoolField(
					TEXT("allow_multiple_connections"),
					Pin->Properties.AllowsMultipleConnections());
				Value->SetBoolField(TEXT("advanced"), Pin->Properties.IsAdvancedPin());
				Value->SetBoolField(TEXT("required"), Pin->Properties.IsRequiredPin());
				Result->SetObjectField(Pin->Properties.Label.ToString(), Value);
			}
			UeAgentAuthoringEncodedTypes::FScope::RecordNameKeyedObject(Result);
			return Result;
		}


		TSharedRef<FJsonObject> PinMapToArrayContainer(
			const TSharedPtr<FJsonObject>& Node,
			const FJsonObject& Source,
			const TCHAR* Field,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			const TSharedPtr<FJsonObject>* Pins = nullptr;
			if (!Node->TryGetObjectField(Field, Pins)) return Result;
			if (!Pins || !Pins->IsValid())
			{
				OutError = GraphInputFailure(TEXT("pcg_field_invalid"), TEXT("The pin map must be an object."), Source, Field, TEXT("an object mapping pin names to pin descriptors"));
				return Result;
			}
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Pins)->Values)
			{
				const TSharedPtr<FJsonObject> Pin = Pair.Value.IsValid() ? Pair.Value->AsObject() : nullptr;
				if (!Pin.IsValid() || Pair.Key.IsEmpty())
				{
					OutError = GraphAuthoringFailure(TEXT("pcg_field_entry_invalid"), !Pin.IsValid() ? TEXT("The pin map entry must be an object.") : TEXT("The pin map key must be a non-empty pin name."),
						Pair.Key, !Pin.IsValid() ? Pair.Value : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Pair.Key)),
						MakeShared<FJsonValueString>(!Pin.IsValid() ? TEXT("a pin descriptor object") : TEXT("a non-empty pin name")));
					OutError.Source.JsonPath = FString(TEXT("/")) + Field + TEXT("/") + Pair.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
					UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, Source, Field);
					return Result;
				}
				TSharedRef<FJsonObject> Copy = CloneObject(Pin);
				Copy->SetStringField(TEXT("label"), Pair.Key);
				Values.Add(MakeShared<FJsonValueObject>(Copy));
			}
			Result->SetArrayField(Field, Values);
			return Result;
		}

		UeAgentPCG::FGraphNodeCreationOverrideResult FailCandidateCreation(UE::UAI::Contract::FDiagnostic Diagnostic)
        {
            UeAgentPCG::FGraphNodeCreationOverrideResult Result;
            Result.Disposition = UeAgentPCG::EGraphNodeCreationOverrideDisposition::Failed;
            Result.Diagnostic = MoveTemp(Diagnostic);
            return Result;
        }

		UeAgentPCG::FGraphNodeCreationOverrideResult CreateCandidateNode(
			UPCGGraph& Graph,
			const TSharedPtr<FJsonObject>& NodeJson)
		{
			UeAgentPCG::FGraphNodeCreationOverrideResult Result;
			const TSharedPtr<FJsonObject>* Candidate = nullptr;
			if (!NodeJson.IsValid()
				|| !NodeJson->HasField(TEXT("candidate")))
			{
				return Result;
			}
			if (!NodeJson->TryGetObjectField(TEXT("candidate"), Candidate)
				|| !Candidate || !Candidate->IsValid())
			{
				auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("pcg_candidate_selector_invalid"),
					TEXT("The PCG node candidate selector must be an object."), TEXT("candidate_create"));
				Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("object"));
				const auto Actual = NodeJson->TryGetField(TEXT("candidate"));
				Diagnostic.ActualValuePresent = Actual.IsValid();
				if (Actual.IsValid() && Actual->Type != EJson::Array && Actual->Type != EJson::Object)
					Diagnostic.ActualValue = Actual;
				else if (Actual.IsValid())
				{
					auto Summary = MakeShared<FJsonObject>();
					Summary->SetStringField(TEXT("type"), UeAgentJsonDiagnostics::JsonValueTypeToString(Actual));
					if (Actual->Type == EJson::Array) Summary->SetNumberField(TEXT("count"), Actual->AsArray().Num());
					Diagnostic.ActualValue = MakeShared<FJsonValueObject>(Summary);
				}
				UeAgentAuthoringDiagnostics::LocateFailure(Diagnostic, *NodeJson, TEXT("candidate_create"), FString(), TEXT("candidate"));
				return FailCandidateCreation(MoveTemp(Diagnostic));
			}
			UE::UAI::HjsonWorkspace::DomainAuthoring::FDomainHjsonCandidateSelector Selector;
			UE::UAI::HjsonWorkspace::FWorkspaceError SelectorError;
			if (!UE::UAI::HjsonWorkspace::DomainAuthoring::TryReadCandidateSelector(
					*Candidate, TEXT("/candidate"), Selector, SelectorError))
			{
				auto Diagnostic = SelectorError.GetDiagnostic();
				UeAgentAuthoringDiagnostics::LocateFailure(Diagnostic, *NodeJson, TEXT("candidate_create"), FString(), TEXT("candidate"));
				return FailCandidateCreation(MoveTemp(Diagnostic));
			}
			double PosX = 0.0;
			double PosY = 0.0;
			NodeJson->TryGetNumberField(TEXT("pos_x"), PosX);
			NodeJson->TryGetNumberField(TEXT("pos_y"), PosY);
			UPCGNode* Node = nullptr;
			UE::UAI::QueryOwners::Pcg::FPcgNodeCandidate ResolvedCandidate;
			UE::UAI::QueryOwners::Pcg::FPcgNodeCandidateQueryError CandidateError;
			if (!UE::UAI::QueryOwners::Pcg::TryPerformPcgNodeCandidate(
					Graph,
					Selector.Name,
					FString(),
					FVector2f(
						static_cast<float>(PosX),
						static_cast<float>(PosY)),
					Node,
					ResolvedCandidate,
					CandidateError, Selector.Select))
			{
				auto Diagnostic = MoveTemp(CandidateError);
				if (Diagnostic.SourceKind.IsEmpty() && (Diagnostic.PropertyPath == TEXT("x") || Diagnostic.PropertyPath == TEXT("y")))
				{
					const FString Field = Diagnostic.PropertyPath == TEXT("x") ? TEXT("pos_x") : TEXT("pos_y");
					Diagnostic.Source.JsonPath = TEXT("/") + Field;
					Diagnostic.PropertyPath = Field;
					UeAgentAuthoringDiagnostics::LocateFailure(Diagnostic, *NodeJson, TEXT("candidate_create"), FString(), Field);
				}
				else UeAgentAuthoringDiagnostics::LocateFailure(Diagnostic, *NodeJson, TEXT("candidate_create"), FString(), TEXT("candidate"));
				return FailCandidateCreation(MoveTemp(Diagnostic));
			}
			const UPCGSettings* const Settings =
				Node ? Node->GetSettings() : nullptr;
			if (!Node || !Settings)
			{
				if (Node)
				{
					TArray<UPCGNode*> NodesToRemove = { Node };
					Graph.RemoveNodes(NodesToRemove);
				}
				auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("pcg_candidate_native_action_result_invalid"),
					!Node ? TEXT("The PCG candidate action completed without returning a node.")
						: TEXT("The PCG candidate action returned a node without settings."),
					TEXT("candidate_create"), FString(), ResolvedCandidate.Id);
				Diagnostic.SourceKind = TEXT("native_result");
				Diagnostic.CandidateName = ResolvedCandidate.Name;
				Diagnostic.PropertyPath = Node ? TEXT("settings") : TEXT("node");
				Diagnostic.ActualValuePresent = false;
				Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(Node ? TEXT("PCG node settings") : TEXT("PCG node"));
				Diagnostic.MutationMayHaveStarted = true;
				if (Node) Diagnostic.MutationStarted = true;
				return FailCandidateCreation(MoveTemp(Diagnostic));
			}
			FString DeclaredSettingsClass;
			if (NodeJson->TryGetStringField(
					TEXT("settings_class"), DeclaredSettingsClass)
				&& DeclaredSettingsClass !=
					Settings->GetClass()->GetPathName())
			{
				const FString ActualSettingsClass = Settings->GetClass()->GetPathName();
				TArray<UPCGNode*> NodesToRemove = { Node };
				Graph.RemoveNodes(NodesToRemove);
				auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("pcg_candidate_settings_class_conflict"),
					TEXT("The created PCG candidate settings class differs from the authored settings_class assertion."), TEXT("candidate_create"));
				Diagnostic.CandidateName = ResolvedCandidate.Name;
				Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(DeclaredSettingsClass);
				Diagnostic.ActualValue = MakeShared<FJsonValueString>(ActualSettingsClass);
				Diagnostic.MutationStarted = true;
				UeAgentAuthoringDiagnostics::LocateFailure(Diagnostic, *NodeJson, TEXT("candidate_create"), FString(), TEXT("settings_class"));
				return FailCandidateCreation(MoveTemp(Diagnostic));
			}
			Result.Disposition =
				UeAgentPCG::EGraphNodeCreationOverrideDisposition::Created;
			Result.Node = Node;
			return Result;
		}

		FString PointerToken(const FString& Value)
		{
			return Value.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
		}

		bool ReadEdgeEndpoints(const FJsonObject& Edge, const TCHAR* Code, const FString& EdgeKey,
			const TSharedPtr<FJsonObject>*& From, const TSharedPtr<FJsonObject>*& To,
			FString& FromNode, FString& FromPin, FString& ToNode, FString& ToPin,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			// Preserve the existing first-failure order: both endpoint objects,
			// then source node/pin and destination node/pin.
			for (const TCHAR* Side : { TEXT("from"), TEXT("to") })
			{
				auto& Endpoint = FString(Side) == TEXT("from") ? From : To;
				if (!Edge.TryGetObjectField(Side, Endpoint) || !Endpoint || !*Endpoint)
				{
					OutError = GraphInputFailure(Code, FString::Printf(TEXT("The edge %s endpoint must be an object."), Side),
						Edge, Side, TEXT("an object containing node and pin strings"));
					OutError.CandidateName = EdgeKey;
					return false;
				}
			}
			for (const TCHAR* Side : { TEXT("from"), TEXT("to") })
			{
				const bool bFrom = FString(Side) == TEXT("from");
				const auto& Endpoint = bFrom ? *From : *To;
				for (const TCHAR* Field : { TEXT("node"), TEXT("pin") })
				{
					FString& Value = bFrom ? (FString(Field) == TEXT("node") ? FromNode : FromPin)
						: (FString(Field) == TEXT("node") ? ToNode : ToPin);
					if (!Endpoint->TryGetStringField(Field, Value))
					{
						OutError = GraphAuthoringFailure(Code, FString::Printf(TEXT("The edge %s.%s must be a string."), Side, Field),
							FString(TEXT("/")) + Side + TEXT("/") + Field, Endpoint->TryGetField(Field), MakeShared<FJsonValueString>(TEXT("a string")));
						OutError.CandidateName = EdgeKey;
						UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, Edge, Side);
						return false;
					}
				}
			}
			return true;
		}

		bool ConvertNode(
			const FString& NodeId,
			const TSharedPtr<FJsonObject>& Source,
			TSharedPtr<FJsonObject>& OutNode,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (!IsValidIdentityKey(NodeId) || !Source.IsValid())
			{
				OutError = GraphAuthoringFailure(TEXT("pcg_node_identity_invalid"), !IsValidIdentityKey(NodeId)
					? TEXT("The node key must be non-empty and contain only letters, digits, underscores, hyphens or dots.") : TEXT("The node declaration must be an object."),
					FString(), !IsValidIdentityKey(NodeId) ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(NodeId)) : nullptr,
					MakeShared<FJsonValueString>(!IsValidIdentityKey(NodeId) ? TEXT("a non-empty alphanumeric, _, - or . node key") : TEXT("a node object")));
				OutError.CandidateName = NodeId;
				return false;
			}
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Source->Values)
			{
				if (Field.Key != TEXT("role")
					&& Field.Key != TEXT("settings_class")
					&& Field.Key != TEXT("candidate")
					&& Field.Key != TEXT("authored_title")
					&& Field.Key != TEXT("position")
					&& Field.Key != TEXT("properties")
					&& Field.Key != TEXT("authoring_source")
					&& Field.Key != TEXT("pin_interface")
					&& Field.Key != TEXT("input_pins")
					&& Field.Key != TEXT("output_pins"))
				{
					OutError = GraphAuthoringFailure(TEXT("pcg_node_unknown_field"), TEXT("The node declaration contains an unsupported field."),
						Field.Key, MakeShared<FJsonValueString>(Field.Key), MakeShared<FJsonValueString>(TEXT("role, settings_class, candidate, authored_title, position, properties, pin_interface, input_pins, output_pins or authoring_source")));
					OutError.CandidateName = NodeId;
					UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Source, Field.Key);
					return false;
				}
			}

			TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
			Node->Values = Source->Values;
			if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(*Source,
				{{TEXT("role"), EJson::String}, {TEXT("settings_class"), EJson::String},
				 {TEXT("authored_title"), EJson::String}, {TEXT("position"), EJson::Array}},
				OutError)) return false;
			Node->SetStringField(TEXT("id"), NodeId);
			if (const TSharedPtr<FJsonObject>* Interface = nullptr;
				Node->TryGetObjectField(TEXT("pin_interface"), Interface)
				&& Interface && Interface->IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* InterfacePins = nullptr;
				if (!(*Interface)->TryGetArrayField(TEXT("pins"), InterfacePins) || !InterfacePins)
				{
					OutError = GraphAuthoringFailure(TEXT("pcg_pin_interface_missing_pins"), TEXT("The pin interface requires a pins array."),
						TEXT("/pin_interface/pins"), (*Interface)->TryGetField(TEXT("pins")), MakeShared<FJsonValueString>(TEXT("an array of pin descriptors")));
					UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Source, TEXT("pin_interface"));
					OutError.CandidateName = NodeId;
					return false;
				}
				for (int32 PinIndex = 0; PinIndex < InterfacePins->Num(); ++PinIndex)
				{
					const auto& InterfaceValue = (*InterfacePins)[PinIndex];
					const TSharedPtr<FJsonObject> Pin = InterfaceValue.IsValid() && InterfaceValue->Type == EJson::Object ? InterfaceValue->AsObject() : nullptr;
					FString Direction;
					FString PinName;
					if (!Pin.IsValid() || !Pin->TryGetStringField(TEXT("name"), PinName)
						|| !Pin->TryGetStringField(TEXT("direction"), Direction)
						|| (Direction != TEXT("input") && Direction != TEXT("output")))
					{
const bool bNameInvalid = Pin && !Pin->TryGetStringField(TEXT("name"), PinName);
						const FString Pointer = TEXT("/pin_interface/pins/") + LexToString(PinIndex)
							+ (!Pin ? FString() : bNameInvalid ? TEXT("/name") : TEXT("/direction"));
						OutError = GraphAuthoringFailure(TEXT("pcg_pin_interface_entry_invalid"), !Pin
							? TEXT("A pin interface entry must be an object.") : bNameInvalid
							? TEXT("A pin interface entry requires a string name.") : TEXT("A pin direction must be input or output."),
							Pointer, !Pin ? InterfaceValue : Pin->TryGetField(bNameInvalid ? TEXT("name") : TEXT("direction")),
							MakeShared<FJsonValueString>(!Pin ? TEXT("a pin descriptor object") : bNameInvalid ? TEXT("a string pin name") : TEXT("input or output")));
						OutError.CandidateName = NodeId;
						UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Source, TEXT("pin_interface"));
						return false;
					}
					TSharedRef<FJsonObject> PinCopy = CloneObject(Pin);
					PinCopy->SetStringField(TEXT("label"), PinName);
					PinCopy->RemoveField(TEXT("name"));
					PinCopy->RemoveField(TEXT("direction"));
					const TCHAR* NativeField = Direction == TEXT("input") ? TEXT("input_pins") : TEXT("output_pins");
					const TSharedPtr<FJsonObject>* ExistingPins = nullptr;
				if (!Node->TryGetObjectField(NativeField, ExistingPins) || !ExistingPins || !ExistingPins->IsValid())
					{
						Node->SetObjectField(NativeField, MakeShared<FJsonObject>());
						Node->TryGetObjectField(NativeField, ExistingPins);
					}
					// The node copy shares source values; copy only the map being changed.
					auto UpdatedPins = MakeShared<FJsonObject>();
					UpdatedPins->Values = (*ExistingPins)->Values;
					UpdatedPins->SetObjectField(PinName, PinCopy);
					Node->SetObjectField(NativeField, UpdatedPins);
				}
				Node->RemoveField(TEXT("pin_interface"));
			}
			FString Role;
			Node->TryGetStringField(TEXT("role"), Role);
			if (Role != TEXT("input") && Role != TEXT("output"))
			{
				const bool bUsesCandidateCreation =
					Node->HasField(TEXT("candidate"));
				FString SettingsClass;
				if (!bUsesCandidateCreation
					&& (!Node->TryGetStringField(
						TEXT("settings_class"), SettingsClass)
						|| !UeAgentPCG::ResolvePCGSettingsClass(SettingsClass)))
				{
					OutError = GraphInputFailure(TEXT("pcg_node_settings_class_invalid"), TEXT("The node settings class must resolve to a PCG settings class."),
						*Source, TEXT("settings_class"), TEXT("a resolvable PCG settings class path"));
					OutError.CandidateName = NodeId;
					return false;
				}
			}

			const TArray<TSharedPtr<FJsonValue>>* Position = nullptr;
			if (Node->TryGetArrayField(TEXT("position"), Position))
			{
				if (!Position || Position->Num() != 2
					|| !(*Position)[0].IsValid() || !(*Position)[1].IsValid()
					|| (*Position)[0]->Type != EJson::Number || (*Position)[1]->Type != EJson::Number)
				{
					FString Pointer = TEXT("/position");
					TSharedPtr<FJsonValue> Actual = Source->TryGetField(TEXT("position"));
					if (Position && Position->Num() == 2)
					{
						const int32 Index = !(*Position)[0].IsValid() ? 0 : !(*Position)[1].IsValid() ? 1
							: (*Position)[0]->Type != EJson::Number ? 0 : 1;
						Pointer += TEXT("/") + LexToString(Index);
						Actual = (*Position)[Index];
					}
					OutError = GraphAuthoringFailure(TEXT("pcg_node_position_invalid"), TEXT("The node position requires exactly two numeric coordinates."),
						Pointer, Actual, MakeShared<FJsonValueString>(Pointer == TEXT("/position") ? TEXT("[number, number]") : TEXT("a number")));
					OutError.CandidateName = NodeId;
					UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Source, TEXT("position"));
					return false;
				}
				Node->SetField(TEXT("pos_x"), (*Position)[0]);
				Node->SetField(TEXT("pos_y"), (*Position)[1]);
				const TSharedPtr<FJsonObject>* Location = nullptr;
				const TSharedPtr<FJsonObject>* Fields = nullptr;
				const bool bSemanticPosition = Source->TryGetObjectField(TEXT("authoring_source"), Location) && Location && *Location
					&& (*Location)->TryGetObjectField(TEXT("fields"), Fields) && Fields && *Fields && (*Fields)->HasField(TEXT("pos_x"));
				if (!bSemanticPosition)
				{
					RemapSourceField(*Node, TEXT("pos_x"), *Source, TEXT("position"), TEXT("/0"));
					RemapSourceField(*Node, TEXT("pos_y"), *Source, TEXT("position"), TEXT("/1"));
				}
				Node->RemoveField(TEXT("position"));
			}

			for (const TCHAR* Field : { TEXT("input_pins"), TEXT("output_pins") })
			{
				TSharedRef<FJsonObject> Container = PinMapToArrayContainer(Node, *Source, Field, OutError);
				if (!OutError.IsEmpty()) return false;
				const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
				if (Container->TryGetArrayField(Field, Pins) && Pins)
				{
					Node->SetArrayField(Field, *Pins);
				}
			}

			Node->RemoveField(TEXT("properties"));
			OutNode = Node;
			return true;
		}

		bool ApplyNodeProperties(
			UPCGGraph& Graph,
			const TSharedPtr<const FJsonObject>& CanonicalDocument,
			const TMap<FString, UPCGNode*>& NodeById,
			FGraphPropertyIntent& Intent,
			int32& OutApplied,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutApplied = 0;
			const TSharedPtr<FJsonObject>* Nodes = nullptr;
			if (!CanonicalDocument->TryGetObjectField(TEXT("nodes"), Nodes)) return true;
			if (!Nodes || !Nodes->IsValid())
			{
				OutError = GraphInputFailure(TEXT("pcg_nodes_invalid"), TEXT("Graph node properties require an object mapping node keys to declarations."), *CanonicalDocument, TEXT("nodes"), TEXT("a node map object"));
				return false;
			}

			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Nodes)->Values)
			{
				const TSharedPtr<FJsonObject> NodeDocument = Pair.Value.IsValid()
					? Pair.Value->AsObject()
					: nullptr;
				TSharedPtr<FJsonObject> Properties;
				if (!UeAgentDetailsAuthoringService::ReadPropertyPayload(NodeDocument, TEXT("properties"), Properties, OutError)) return false;
				if (!Properties) continue;
				UPCGNode* const* NodePtr = NodeById.Find(Pair.Key);
				UPCGSettings* Settings = NodePtr && *NodePtr ? (*NodePtr)->GetSettings() : nullptr;
				if (!Settings)
				{
					OutError = GraphAuthoringFailure(TEXT("pcg_node_properties_target_missing"), TEXT("The node and its settings must exist before applying its properties."),
						!NodePtr || !*NodePtr ? TEXT("node_available") : TEXT("settings_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), true);
					OutError.CandidateName = Pair.Key;
					OutError.TargetPath = Intent.TargetPath;
					if (OutError.TargetPath.IsEmpty() && Graph.IsAsset()) OutError.TargetPath = Graph.GetPathName();
					return false;
				}
				UeAgentAuthoringObjectApply::FOptions Options;
				Options.bAllowDomainAuthorizedOwners = true;
				Options.bCompactValues = true;
				UeAgentAuthoringDirectApply::FResult ApplyResult;
				if (!UeAgentDetailsAuthoringService::ApplyObjectProperties(Settings, Properties, Options, ApplyResult))
				{
					OutError = ApplyResult.Diagnostic;
					return false;
				}
				OutApplied += ApplyResult.PropertiesSeen;
				Intent.Nodes.Add((*NodePtr)->GetFName(), MoveTemp(ApplyResult.PropertyIntents));
				Intent.AuthoredNodeIds.Add((*NodePtr)->GetFName(), Pair.Key);
				(*NodePtr)->UpdateAfterSettingsChangeDuringCreation();
			}
			return true;
		}
	}

	TSharedRef<FJsonObject> BuildNodeState(
		UPCGGraph& Graph,
		UPCGNode& Node)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		if (&Node == Graph.GetInputNode())
		{
			Result->SetStringField(TEXT("role"), TEXT("input"));
		}
		else if (&Node == Graph.GetOutputNode())
		{
			Result->SetStringField(TEXT("role"), TEXT("output"));
		}
		else if (const UPCGSettings* Settings = Node.GetSettings())
		{
			Result->SetStringField(TEXT("settings_class"), Settings->GetClass()->GetPathName());
		}

		const FString AuthoredTitle = Node.GetAuthoredTitleName().ToString();
		if (!AuthoredTitle.IsEmpty() && AuthoredTitle != TEXT("None"))
		{
			Result->SetStringField(TEXT("authored_title"), AuthoredTitle);
		}
#if WITH_EDITOR
		Result->SetArrayField(TEXT("position"), {
			MakeShared<FJsonValueNumber>(Node.PositionX),
			MakeShared<FJsonValueNumber>(Node.PositionY),
		});
#endif
		Result->SetObjectField(TEXT("input_pins"), BuildPinMap(Node.GetInputPins()));
		Result->SetObjectField(TEXT("output_pins"), BuildPinMap(Node.GetOutputPins()));
		return Result;
	}

	FString MakeEdgeKey(
		const FString& FromNode,
		const FString& FromPin,
		const FString& ToNode,
		const FString& ToPin)
	{
		return FromNode + TEXT(":") + FromPin + TEXT("->") + ToNode + TEXT(":") + ToPin;
	}

	bool BuildGraphDocument(
		UPCGGraph& Graph,
		TSharedPtr<FJsonObject>& OutDocument,
		UE::UAI::Contract::FDiagnostic& OutError,
		const FString& LogicalTarget)
	{
		OutDocument = MakeShared<FJsonObject>();
		OutError.Reset();
		ON_SCOPE_EXIT { BindLogicalGraphTarget(OutError, Graph, LogicalTarget); };
		TSharedRef<FJsonObject> Nodes = MakeShared<FJsonObject>();
		TSharedRef<FJsonObject> Edges = MakeShared<FJsonObject>();

		const TMap<FString, UPCGNode*> NodeById = UeAgentPCG::BuildExportNodeMapById(&Graph);
		TMap<UPCGNode*, FString> IdByNode;
		for (const TPair<FString, UPCGNode*>& Pair : NodeById)
		{
			if (!Pair.Value || !IsValidIdentityKey(Pair.Key))
			{
				OutError = GraphAuthoringFailure(TEXT("pcg_export_node_identity_invalid"), !Pair.Value ? TEXT("The native export node map contains an unavailable node.") : TEXT("The native export node key is not a supported graph identity."),
					!Pair.Value ? TEXT("node_available") : TEXT("node_id"), !Pair.Value ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false)) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Pair.Key)),
					!Pair.Value ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(true)) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(TEXT("a non-empty alphanumeric, _, - or . key"))), true);
				OutError.TargetPath = LogicalTarget;
				if (OutError.TargetPath.IsEmpty() && Graph.IsAsset()) OutError.TargetPath = Graph.GetPathName();
				return false;
			}
			IdByNode.Add(Pair.Value, Pair.Key);
			Nodes->SetObjectField(Pair.Key, BuildNodeState(Graph, *Pair.Value));
		}

		for (const TPair<FString, UPCGNode*>& Pair : NodeById)
		{
			UPCGNode* FromNode = Pair.Value;
			if (!FromNode) continue;
			for (const TObjectPtr<UPCGPin>& PinPtr : FromNode->GetOutputPins())
			{
				UPCGPin* FromPin = PinPtr.Get();
				if (!FromPin) continue;
				for (const TObjectPtr<UPCGEdge>& EdgePtr : FromPin->Edges)
				{
					UPCGEdge* Edge = EdgePtr.Get();
					if (!Edge || Edge->InputPin != FromPin || !Edge->OutputPin || !Edge->OutputPin->Node)
					{
						continue;
					}
					const FString* ToId = IdByNode.Find(Edge->OutputPin->Node);
					if (!ToId) continue;
					const FString FromLabel = FromPin->Properties.Label.ToString();
					const FString ToLabel = Edge->OutputPin->Properties.Label.ToString();
					TSharedRef<FJsonObject> From = MakeShared<FJsonObject>();
					From->SetStringField(TEXT("node"), Pair.Key);
					From->SetStringField(TEXT("pin"), FromLabel);
					TSharedRef<FJsonObject> To = MakeShared<FJsonObject>();
					To->SetStringField(TEXT("node"), *ToId);
					To->SetStringField(TEXT("pin"), ToLabel);
					TSharedRef<FJsonObject> EdgeState = MakeShared<FJsonObject>();
					EdgeState->SetObjectField(TEXT("from"), From);
					EdgeState->SetObjectField(TEXT("to"), To);
					Edges->SetObjectField(
						MakeEdgeKey(Pair.Key, FromLabel, *ToId, ToLabel),
						EdgeState);
				}
			}
		}

		OutDocument->SetObjectField(TEXT("nodes"), Nodes);
		OutDocument->SetObjectField(TEXT("edges"), Edges);
		OutDocument->SetObjectField(
			TEXT("parameters"),
			UeAgentPCG::ExportParameterBagToJson(Graph.GetUserParametersStruct()));
		return true;
	}

	bool BuildBackendGraphDocument(
		const TSharedPtr<const FJsonObject>& CanonicalDocument,
		TSharedPtr<FJsonObject>& OutBackendDocument,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutBackendDocument = MakeShared<FJsonObject>();
		OutError.Reset();
		if (!CanonicalDocument.IsValid())
		{
			OutError = GraphAuthoringFailure(TEXT("pcg_graph_document_invalid"), TEXT("A canonical graph document is required."),
				FString(), nullptr, MakeShared<FJsonValueString>(TEXT("a graph document object")));
			return false;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : CanonicalDocument->Values)
		{
			if (Field.Key != TEXT("nodes") && Field.Key != TEXT("edges") && Field.Key != TEXT("parameters"))
			{
				OutError = GraphAuthoringFailure(TEXT("pcg_graph_unknown_field"), TEXT("The graph document contains an unsupported field."),
					Field.Key, MakeShared<FJsonValueString>(Field.Key), MakeShared<FJsonValueString>(TEXT("nodes, edges or parameters")));
				UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *CanonicalDocument, Field.Key);
				return false;
			}
		}

		const TSharedPtr<FJsonObject>* NodeMap = nullptr;
		if (!CanonicalDocument->TryGetObjectField(TEXT("nodes"), NodeMap) || !NodeMap || !NodeMap->IsValid())
		{
			OutError = GraphInputFailure(TEXT("pcg_nodes_required"), TEXT("The graph document requires a nodes object."), *CanonicalDocument, TEXT("nodes"), TEXT("an object mapping node keys to declarations"));
			return false;
		}
		TArray<TSharedPtr<FJsonValue>> Nodes;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*NodeMap)->Values)
		{
			TSharedPtr<FJsonObject> Converted;
			if (!ConvertNode(Pair.Key, Pair.Value.IsValid() ? Pair.Value->AsObject() : nullptr, Converted, OutError))
			{
				if (Pair.Value.IsValid() && Pair.Value->Type == EJson::Object)
					UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Pair.Value->AsObject(), TEXT("node_prepare"));
				else
				{
					if (IsValidIdentityKey(Pair.Key))
					{
						OutError.ActualValue = Pair.Value;
						OutError.ActualValuePresent = Pair.Value.IsValid();
					}
					OutError.Source.JsonPath = TEXT("/nodes/") + PointerToken(Pair.Key);
					UeAgentAuthoringDiagnostics::LocateFailure(OutError, *CanonicalDocument, TEXT("node_prepare"), FString(), TEXT("nodes"));
				}
				return false;
			}
			Nodes.Add(MakeShared<FJsonValueObject>(Converted));
		}

		const TSharedPtr<FJsonObject>* EdgeMap = nullptr;
		if (!CanonicalDocument->TryGetObjectField(TEXT("edges"), EdgeMap) || !EdgeMap || !EdgeMap->IsValid())
		{
			OutError = GraphInputFailure(TEXT("pcg_edges_required"), TEXT("The graph document requires an edges object."), *CanonicalDocument, TEXT("edges"), TEXT("an object mapping edge keys to endpoint declarations"));
			return false;
		}
		TArray<TSharedPtr<FJsonValue>> Edges;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*EdgeMap)->Values)
		{
			const TSharedPtr<FJsonObject> Edge = Pair.Value.IsValid() ? Pair.Value->AsObject() : nullptr;
			const TSharedPtr<FJsonObject>* From = nullptr;
			const TSharedPtr<FJsonObject>* To = nullptr;
			FString FromNode;
			FString FromPin;
			FString ToNode;
			FString ToPin;
			if (!Edge.IsValid())
			{
				OutError = GraphAuthoringFailure(TEXT("pcg_edge_invalid"), TEXT("The graph edge declaration must be an object."),
					TEXT("/edges/") + PointerToken(Pair.Key), Pair.Value, MakeShared<FJsonValueString>(TEXT("an edge object")));
				OutError.CandidateName = Pair.Key;
				UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *CanonicalDocument, TEXT("edges"));
				return false;
			}
			if (!ReadEdgeEndpoints(*Edge, TEXT("pcg_edge_invalid"), Pair.Key, From, To, FromNode, FromPin, ToNode, ToPin, OutError)) return false;
			const bool bFromMissing = !(*NodeMap)->HasField(FromNode) && FromNode != UeAgentPCG::InputNodeId && FromNode != UeAgentPCG::OutputNodeId;
			const bool bToMissing = !(*NodeMap)->HasField(ToNode) && ToNode != UeAgentPCG::InputNodeId && ToNode != UeAgentPCG::OutputNodeId;
			const FString ExpectedKey = MakeEdgeKey(FromNode, FromPin, ToNode, ToPin);
			if (bFromMissing || bToMissing || Pair.Key != ExpectedKey)
			{
				const FString Side = bFromMissing ? TEXT("from") : TEXT("to");
				OutError = GraphAuthoringFailure(TEXT("pcg_edge_invalid"), bFromMissing || bToMissing
					? TEXT("The edge endpoint references a node absent from the graph.") : TEXT("The edge key does not match its declared endpoints."),
					bFromMissing || bToMissing ? TEXT("/") + Side + TEXT("/node") : FString(),
					MakeShared<FJsonValueString>(bFromMissing ? FromNode : bToMissing ? ToNode : Pair.Key),
					MakeShared<FJsonValueString>(bFromMissing || bToMissing ? TEXT("an existing graph node key") : ExpectedKey));
				OutError.CandidateName = Pair.Key;
				UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Edge, bFromMissing || bToMissing ? Side : FString());
				return false;
			}
			TSharedRef<FJsonObject> Converted = MakeShared<FJsonObject>();
			Converted->SetField(TEXT("from"), (*From)->TryGetField(TEXT("node")));
			Converted->SetField(TEXT("from_pin"), (*From)->TryGetField(TEXT("pin")));
			Converted->SetField(TEXT("to"), (*To)->TryGetField(TEXT("node")));
			Converted->SetField(TEXT("to_pin"), (*To)->TryGetField(TEXT("pin")));
			if (const auto Source = Edge->TryGetField(TEXT("authoring_source")); Source.IsValid())
				Converted->SetField(TEXT("authoring_source"), Source);
			RemapSourceField(*Converted, TEXT("from"), *Edge, TEXT("from"), TEXT("/node"));
			RemapSourceField(*Converted, TEXT("from_pin"), *Edge, TEXT("from"), TEXT("/pin"));
			RemapSourceField(*Converted, TEXT("to"), *Edge, TEXT("to"), TEXT("/node"));
			RemapSourceField(*Converted, TEXT("to_pin"), *Edge, TEXT("to"), TEXT("/pin"));
			Edges.Add(MakeShared<FJsonValueObject>(Converted));
		}

		OutBackendDocument->SetArrayField(TEXT("nodes"), Nodes);
		OutBackendDocument->SetArrayField(TEXT("edges"), Edges);
		const TSharedPtr<FJsonObject>* Parameters = nullptr;
		if (CanonicalDocument->TryGetObjectField(TEXT("parameters"), Parameters))
		{
			if (!Parameters || !Parameters->IsValid())
			{
				OutError = GraphInputFailure(TEXT("pcg_parameters_invalid"), TEXT("The graph parameters must be a canonical property bag object."), *CanonicalDocument, TEXT("parameters"), TEXT("a property bag object"));
				return false;
			}
			OutBackendDocument->SetObjectField(TEXT("parameters"), *Parameters);
		}
		return true;
		}

		bool UnitToObject(
			const UE::UAI::HjsonWorkspace::FSemanticUnit& Unit,
			TSharedPtr<FJsonObject>& OutObject,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutObject = MakeShared<FJsonObject>();
			for (const auto& Pair : Unit.GetValues())
			{
				TSharedPtr<FJsonValue> Value;
				UE::UAI::Contract::FDiagnostic Error;
				if (!UE::UAI::StateSemantic::CanonicalJson::TryToJsonValue(
					Pair.Value.Get(), Value, Error))
				{
					OutError = MoveTemp(Error);
					OutError.Source.SourceUnit = Unit.GetUnitId();
					const FString Relative = OutError.Source.InternalJsonPath.IsEmpty() ? OutError.SemanticPointer : OutError.Source.InternalJsonPath;
					OutError.Source.InternalJsonPath = TEXT("/") + PointerToken(Pair.Key)
						+ (Relative == TEXT("/") ? FString() : Relative);
					return false;
				}
				OutObject->SetField(Pair.Key, Value);
			}
			return true;
		}

		bool ApplyGraphDocument(
		UPCGGraph& Graph,
		const TSharedPtr<const FJsonObject>& CanonicalDocument,
		const bool bReplaceNodes,
		FGraphApplyResult& OutResult,
		UE::UAI::Contract::FDiagnostic& OutError,
		const bool bPreserveExistingNodes,
		const UE::UAI::HjsonWorkspace::FGraphAuthoringConnectionContext* Connections,
		const FString& LogicalTarget)
	{
		OutResult = FGraphApplyResult();
		OutError.Reset();
		const FString TargetPath = !LogicalTarget.IsEmpty() ? LogicalTarget : Graph.IsAsset() ? Graph.GetPathName() : FString();
		ON_SCOPE_EXIT { BindLogicalGraphTarget(OutError, Graph, TargetPath); };
		TSharedPtr<FJsonObject> BackendDocument;
		if (!BuildBackendGraphDocument(CanonicalDocument, BackendDocument, OutError)) return false;
		// Unchanged values retain identity across projection, including renamed
		// coordinates and flattened edge endpoints. Bind copied containers too.
		auto BackendSources = UeAgentAuthoringDiagnostics::FScopedSourceLocations::CaptureProjection(*BackendDocument);
		const auto CanonicalSources = UeAgentAuthoringDiagnostics::FScopedSourceLocations::CaptureProjection(*CanonicalDocument);
		for (const TCHAR* Field : { TEXT("nodes"), TEXT("edges") })
		{
			const auto& Entries = CanonicalDocument->GetObjectField(Field)->Values;
			int32 Index = 0;
			for (const auto& Pair : Entries)
			{
				const FString Pointer = FString(TEXT("/")) + Field + TEXT("/") + Pair.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
				const auto Source = UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(CanonicalSources, Pointer);
				if (!Source.SourceFile.IsEmpty() || Source.bCommandInput)
					BackendSources.Add(FString(TEXT("/")) + Field + TEXT("/") + LexToString(Index), Source);
				++Index;
			}
		}
		UeAgentAuthoringDiagnostics::FScopedSourceLocations BackendSourceScope(BackendDocument.Get(),
			[&BackendSources](const FString& Pointer) { return UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(BackendSources, Pointer); });
		const TSharedPtr<FJsonObject>* SeedNodes = nullptr;
		if (CanonicalDocument->TryGetObjectField(TEXT("nodes"), SeedNodes))
			for (const auto& Pair : (*SeedNodes)->Values)
			{
				TSharedPtr<FJsonObject> Properties;
				if (!UeAgentDetailsAuthoringService::ReadPropertyPayload(Pair.Value->AsObject(), TEXT("properties"), Properties, OutError)) return false;
			}
		TArray<UE::UAI::Contract::FDiagnostic> Issues;
		const UeAgentPCG::FGraphNodeCreationOverride NodeCreationOverride =
			CreateCandidateNode;
		TMap<FString, UPCGNode*> AppliedNodeBindings;
		auto Intent = MakeShared<FGraphPropertyIntent>();
		Intent->Document = CanonicalDocument;
		Intent->TargetPath = TargetPath;
		Intent->Sources = CanonicalSources;
		OutResult.PropertyIntent = Intent;
		const UeAgentPCG::FGraphNodePropertiesApply ApplyProperties =
			[&](const TMap<FString, UPCGNode*>& Bindings) {
				return ApplyNodeProperties(Graph, CanonicalDocument, Bindings, *Intent,
					OutResult.NodePropertiesApplied, OutError);
			};
		if (!UeAgentPCG::ApplyGraphJson(
			&Graph,
			BackendDocument,
			bReplaceNodes,
			Issues,
			OutResult.NodeCount,
			OutResult.EdgeCount,
			&NodeCreationOverride, bPreserveExistingNodes, &AppliedNodeBindings, &ApplyProperties, Connections, TargetPath))
		{
			if (!OutError.IsEmpty()) return false;
			for (auto& Issue : Issues)
            {
                if (Issue.Severity != UE::UAI::Contract::ESeverity::Error) continue;
                if (Issue.TargetPath.IsEmpty()) Issue.TargetPath = TargetPath;
                OutError.Append(Issue);
            }
			return false;
		}
		return VerifyGraphPropertyIntent(Graph, OutResult, OutError);
	}

	bool PreflightGraphDocument(
		UPCGGraph* ExistingGraph,
		const TSharedPtr<const FJsonObject>& CanonicalDocument,
		const bool bReplaceNodes,
		FGraphApplyResult& OutResult,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		TStrongObjectPtr<UPCGGraph> Preview(
			ExistingGraph
				? DuplicateObject<UPCGGraph>(ExistingGraph, GetTransientPackage())
				: NewObject<UPCGGraph>(GetTransientPackage(), NAME_None, RF_Transient | RF_Transactional));
		if (!Preview.IsValid())
		{
				OutError = GraphAuthoringFailure(TEXT("pcg_preflight_graph_create_failed"), TEXT("The PCG preview graph could not be created."),
					TEXT("preview_graph_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), true);
			return false;
		}
		return ApplyGraphDocument(
			*Preview.Get(), CanonicalDocument, bReplaceNodes, OutResult, OutError, false, nullptr,
			ExistingGraph && ExistingGraph->IsAsset() ? ExistingGraph->GetPathName() : FString());
	}

	bool TryBuildNativeGraphDocumentFromSemanticModel(
		const UE::UAI::HjsonWorkspace::FAuthoringModelRef& Model,
		TSharedPtr<FJsonObject>& OutDocument,
		UE::UAI::Contract::FDiagnostic& OutError,
		const UE::UAI::HjsonWorkspace::FResolvedSemanticWorkspace* Workspace,
		UE::UAI::HjsonWorkspace::FGraphAuthoringConnectionContext* OutConnections)
	{
		if (OutConnections)
		{
			*OutConnections = {};
			OutConnections->bHasResolvedIntent = Workspace != nullptr;
		}
		OutDocument = MakeShared<FJsonObject>();
		OutError.Reset();
		const UE::UAI::HjsonWorkspace::FSemanticUnit* GraphUnit = nullptr;
		for (const UE::UAI::HjsonWorkspace::FSemanticUnit& Unit : Model->GetUnits())
		{
			if (Unit.GetSemanticTypeRef() == GraphSemanticId)
			{
				GraphUnit = &Unit;
				break;
			}
		}
		if (GraphUnit == nullptr)
		{
			OutError = GraphAuthoringFailure(TEXT("pcg_semantic_graph_unit_missing"), TEXT("The semantic model has no PCG graph unit."),
				TEXT("semantic_type"), nullptr, MakeShared<FJsonValueString>(GraphSemanticId));
			return false;
		}

		TSharedPtr<FJsonObject> GraphState;
		if (!UnitToObject(*GraphUnit, GraphState, OutError)) return false;
		if (Workspace) Workspace->AttachInlinePropertySources(GraphUnit->GetUnitId(), FString(), *GraphState, FString());
		const TSharedPtr<FJsonObject>* SemanticNodes = nullptr;
		const TSharedPtr<FJsonObject>* SemanticEdges = nullptr;
		if (!GraphState->TryGetObjectField(TEXT("nodes"), SemanticNodes)
			|| SemanticNodes == nullptr || !SemanticNodes->IsValid()
			|| !GraphState->TryGetObjectField(TEXT("edges"), SemanticEdges)
			|| SemanticEdges == nullptr || !SemanticEdges->IsValid())
		{
			const TCHAR* Field = !SemanticNodes || !*SemanticNodes ? TEXT("nodes") : TEXT("edges");
			OutError = GraphInputFailure(TEXT("pcg_semantic_graph_shape_invalid"), TEXT("The semantic graph requires nodes and edges objects."),
				*GraphState, Field, TEXT("an object"));
			return false;
		}

		TSharedRef<FJsonObject> Nodes = MakeShared<FJsonObject>();
		TSharedRef<FJsonObject> Edges = MakeShared<FJsonObject>();
		TMap<FString, FString> NativeNodeByResolvedRef;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*SemanticNodes)->Values)
		{
			const TSharedPtr<FJsonObject> Source = Pair.Value.IsValid()
				&& Pair.Value->Type == EJson::Object ? Pair.Value->AsObject() : nullptr;
			if (!Source.IsValid())
			{
				OutError = GraphAuthoringFailure(TEXT("pcg_semantic_node_invalid"), TEXT("The semantic node declaration must be an object."),
					TEXT("/nodes/") + PointerToken(Pair.Key), Pair.Value, MakeShared<FJsonValueString>(TEXT("a node object")));
				OutError.CandidateName = Pair.Key;
				UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *GraphState, TEXT("nodes"));
				return false;
			}
			FString Operation;
			Source->TryGetStringField(TEXT("operation"), Operation);
			if (Operation == TEXT("remove")) continue;
			FString NativeNodeId = Pair.Key;
			Source->TryGetStringField(TEXT("id"), NativeNodeId);
			if (!IsValidIdentityKey(NativeNodeId))
			{
				OutError = GraphAuthoringFailure(TEXT("pcg_semantic_node_id_invalid"), TEXT("The resolved native node id must contain only letters, digits, underscores, hyphens or dots and must not be empty."),
					TEXT("/nodes/") + PointerToken(Pair.Key) + (Source->HasField(TEXT("id")) ? TEXT("/id") : TEXT("")),
					MakeShared<FJsonValueString>(NativeNodeId), MakeShared<FJsonValueString>(TEXT("a non-empty alphanumeric, _, - or . node id")));
				OutError.CandidateName = Pair.Key;
				UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *GraphState, TEXT("nodes"));
				return false;
			}
			NativeNodeByResolvedRef.Add(Pair.Key, NativeNodeId);
			NativeNodeByResolvedRef.Add(NativeNodeId, NativeNodeId);
			TSharedRef<FJsonObject> Node = CloneObject(Source);
			if (Workspace)
			{
				FString Token = Pair.Key;
				Token.ReplaceInline(TEXT("~"), TEXT("~0"));
				Token.ReplaceInline(TEXT("/"), TEXT("~1"));
				Workspace->AttachInlinePropertySources(GraphUnit->GetUnitId(), TEXT("/nodes/") + Token, *Node);
			}
			Node->RemoveField(TEXT("operation"));
			Node->RemoveField(TEXT("id"));
			Node->RemoveField(TEXT("pin_interface"));
			if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(*Node,
				{{TEXT("pos"), EJson::Object}}, OutError)) return false;
			if (const TSharedPtr<FJsonObject>* Position = nullptr;
				Node->TryGetObjectField(TEXT("pos"), Position)
					&& Position != nullptr && Position->IsValid())
			{
				double X = 0.0;
				double Y = 0.0;
				if (!(*Position)->TryGetNumberField(TEXT("x"), X)
					|| !(*Position)->TryGetNumberField(TEXT("y"), Y))
				{
const TCHAR* Field = !(*Position)->TryGetNumberField(TEXT("x"), X) ? TEXT("x") : TEXT("y");
					OutError = GraphAuthoringFailure(TEXT("pcg_semantic_node_position_invalid"), TEXT("The semantic node position requires numeric x and y coordinates."),
						FString(TEXT("/pos/")) + Field, (*Position)->TryGetField(Field), MakeShared<FJsonValueString>(TEXT("a number")));
					OutError.CandidateName = Pair.Key;
					UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Node, TEXT("pos"));
					return false;
				}
				// Position borrows storage from pos. Retain its values before
				// removing that field and releasing the object that owns the pointer.
				TArray<TSharedPtr<FJsonValue>> NativePosition = {
					(*Position)->TryGetField(TEXT("x")),
					(*Position)->TryGetField(TEXT("y"))};
				Node->RemoveField(TEXT("pos"));
				Node->SetArrayField(TEXT("position"), MoveTemp(NativePosition));
				RemapSourceField(*Node, TEXT("pos_x"), *Node, TEXT("pos"), TEXT("/x"));
				RemapSourceField(*Node, TEXT("pos_y"), *Node, TEXT("pos"), TEXT("/y"));
			}
			Nodes->SetObjectField(NativeNodeId, Node);
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*SemanticEdges)->Values)
		{
			const TSharedPtr<FJsonObject> Source = Pair.Value.IsValid()
				&& Pair.Value->Type == EJson::Object ? Pair.Value->AsObject() : nullptr;
			FString Operation;
			if (!Source.IsValid())
			{
				OutError = GraphAuthoringFailure(TEXT("pcg_semantic_edge_invalid"), TEXT("The semantic edge declaration must be an object."),
					TEXT("/edges/") + PointerToken(Pair.Key), Pair.Value, MakeShared<FJsonValueString>(TEXT("an edge object")));
				OutError.CandidateName = Pair.Key;
				UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *GraphState, TEXT("edges"));
				return false;
			}
			if (Workspace)
			{
				FString Token = Pair.Key;
				Token.ReplaceInline(TEXT("~"), TEXT("~0"));
				Token.ReplaceInline(TEXT("/"), TEXT("~1"));
				Workspace->AttachInlinePropertySources(GraphUnit->GetUnitId(), TEXT("/edges/") + Token, *Source);
			}
			Source->TryGetStringField(TEXT("operation"), Operation);
			if (Operation == TEXT("remove")) continue;
			const TSharedPtr<FJsonObject>* From = nullptr;
			const TSharedPtr<FJsonObject>* To = nullptr;
			FString FromRef, FromPin, ToRef, ToPin;
			if (!ReadEdgeEndpoints(*Source, TEXT("pcg_semantic_edge_endpoint_invalid"), Pair.Key, From, To, FromRef, FromPin, ToRef, ToPin, OutError)) return false;
			const FString FromNode = NativeNodeByResolvedRef.FindRef(FromRef);
			const FString ToNode = NativeNodeByResolvedRef.FindRef(ToRef);
			if (FromNode.IsEmpty() || ToNode.IsEmpty())
			{
				const TCHAR* Side = FromNode.IsEmpty() ? TEXT("from") : TEXT("to");
				OutError = GraphAuthoringFailure(TEXT("pcg_semantic_edge_node_unresolved"), TEXT("The semantic edge references a node that did not resolve to an active native node."),
					FString(TEXT("/")) + Side + TEXT("/node"), MakeShared<FJsonValueString>(FromNode.IsEmpty() ? FromRef : ToRef),
					MakeShared<FJsonValueString>(TEXT("an active resolved node reference")));
				OutError.CandidateName = Pair.Key;
				UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Source, TEXT("edge_prepare"), FString(), Side);
				return false;
			}
			TSharedRef<FJsonObject> NativeFrom = MakeShared<FJsonObject>();
			if (OutConnections && Workspace && Operation == TEXT("update"))
				OutConnections->RecordUnchanged(FromNode, FromPin, ToNode, ToPin);
			NativeFrom->SetStringField(TEXT("node"), FromNode);
			NativeFrom->SetStringField(TEXT("pin"), FromPin);
			TSharedRef<FJsonObject> NativeTo = MakeShared<FJsonObject>();
			NativeTo->SetStringField(TEXT("node"), ToNode);
			NativeTo->SetStringField(TEXT("pin"), ToPin);
			TSharedRef<FJsonObject> Edge = CloneObject(Source);
			Edge->RemoveField(TEXT("operation"));
			Edge->SetObjectField(TEXT("from"), NativeFrom);
			Edge->SetObjectField(TEXT("to"), NativeTo);
			Edges->SetObjectField(
				MakeEdgeKey(FromNode, FromPin, ToNode, ToPin), Edge);
		}

		for (const UE::UAI::HjsonWorkspace::FSemanticUnit& Unit : Model->GetUnits())
		{
			if (Unit.GetSemanticTypeRef() == ExportParametersSemanticId)
			{
				TSharedPtr<FJsonObject> Value;
				if (!UnitToObject(Unit, Value, OutError)) return false;
				OutDocument->SetObjectField(TEXT("parameters"), Value);
				break;
			}
		}
		OutDocument->SetObjectField(TEXT("nodes"), Nodes);
		OutDocument->SetObjectField(TEXT("edges"), Edges);
		return true;
	}
}

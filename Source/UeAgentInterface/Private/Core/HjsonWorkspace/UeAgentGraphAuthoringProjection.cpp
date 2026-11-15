#include "Core/HjsonWorkspace/UeAgentGraphAuthoringProjection.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/HjsonWorkspace/UeAgentKeyedItemAuthoringProjection.h"
#include "Utilities/UeAgentReadableName.h"
#include "Core/HjsonWorkspace/UeAgentSemanticLayout.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceIO.h"

#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Core/StateSemantic/UeAgentStateSemanticEvidence.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"
#include <initializer_list>

namespace UE::UAI::HjsonWorkspace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::StateSemantic;
	namespace
	{
		bool Fail(FWorkspaceError& OutError, const FString& Code, const FString& Reason,
			const FString& SourceUnit = FString(), const FString& Pointer = TEXT("/"),
			TSharedPtr<FJsonValue> Actual = nullptr, TSharedPtr<FJsonValue> Expected = nullptr,
			bool bNative = false)
		{
			auto Diagnostic = Contract::FDiagnostic::Failure(Code, Reason, TEXT("graph_authoring"));
			Diagnostic.PropertyPath = Pointer == TEXT("/") ? FString() : Pointer;
			Diagnostic.ActualValue = MoveTemp(Actual);
			Diagnostic.ExpectedValue = MoveTemp(Expected);
			if (bNative)
			{
				Diagnostic.SourceKind = TEXT("native_result");
				Diagnostic.Source.SourceUnit = SourceUnit;
				Diagnostic.Source.InternalJsonPath = Pointer;
				Diagnostic.SemanticPointer = Pointer;
				OutError.SetFailure(MoveTemp(Diagnostic));
			}
			else OutError.SetFailure(MoveTemp(Diagnostic), SourceUnit, Pointer);
			return false;
		}

		bool ForwardFailure(FWorkspaceError& OutError, Contract::FDiagnostic Error,
			const FString& SourceUnit, const FString& Pointer)
		{
			OutError.SetFailure(MoveTemp(Error), SourceUnit);
			OutError.PrependPath(Pointer);
			return false;
		}

		bool ForwardCapturedFailure(FWorkspaceError& OutError, const FString& PointerPrefix)
		{
			auto Diagnostic = OutError.GetDiagnostic();
			TFunction<void(Contract::FDiagnostic&)> Bind = [&](Contract::FDiagnostic& Item)
			{
				Item.SourceKind = TEXT("native_result");
				for (auto& Child : Item.Diagnostics) Bind(Child);
				if (Item.PublicCause)
				{
					auto Cause = MakeShared<Contract::FDiagnostic>(*Item.PublicCause);
					Bind(*Cause); Item.PublicCause = MoveTemp(Cause);
				}
			};
			Bind(Diagnostic);
			OutError.SetFailure(MoveTemp(Diagnostic));
			OutError.PrependPath(PointerPrefix);
			return false;
		}

		// Callers select public scalar fields; captured identity values never enter this helper.
		TSharedPtr<FJsonValue> PublicScalar(const FUeAgentCanonicalValue& Value)
		{
			switch (Value.GetKind())
			{
			case EUeAgentValueKind::String: return MakeShared<FJsonValueString>(Value.GetText());
			case EUeAgentValueKind::Boolean: return MakeShared<FJsonValueBoolean>(Value.GetBoolean());
			case EUeAgentValueKind::Integer:
			case EUeAgentValueKind::Decimal: return MakeShared<FJsonValueNumberString>(Value.GetLexeme());
			case EUeAgentValueKind::Null: return MakeShared<FJsonValueNull>();
			default: return nullptr;
			}
		}

		TSharedPtr<FJsonValue> PublicScalar(const TSharedPtr<FJsonValue>& Value)
		{
			return Value && Value->Type != EJson::Object && Value->Type != EJson::Array ? Value : nullptr;
		}

		class FScopedGraphDiagnosticSource final
		{
		public:
			FScopedGraphDiagnosticSource(FWorkspaceError& InError, const FHjsonNode* InRoot)
				: Error(InError), Root(InRoot) {}
			~FScopedGraphDiagnosticSource() { if (Root && Error.HasErrors()) Error.Locate(*Root); }
		private:
			FWorkspaceError& Error;
			const FHjsonNode* Root;
		};

		bool FailGraphConnection(FWorkspaceError& OutError, const FString& SourceUnit,
			int32 ConnectionIndex, const FString& Field, const FString& Reason,
			TSharedPtr<FJsonValue> Actual = nullptr,
			const FString& Code = TEXT("semantic_workspace_graph_connection_invalid"))
		{
			return Fail(OutError, Code, Reason, SourceUnit,
				FString::Printf(TEXT("/connections/%d"), ConnectionIndex)
					+ (Field.IsEmpty() ? FString() : TEXT("/") + EscapeJsonPointerSegment(Field)),
				MoveTemp(Actual));
		}

		FString MakeReadableToken(FString Value)
		{
			Value.TrimStartAndEndInline();
			FString Token;
			Token.Reserve(Value.Len());
			bool bStartWord = true;
			for (const TCHAR Character : Value)
			{
				if (!FChar::IsAlnum(Character))
				{
					bStartWord = true;
					continue;
				}

				Token.AppendChar(
					bStartWord ? FChar::ToUpper(Character) : Character);
				bStartWord = false;
			}
			return Token;
		}

		FString MakeIndexedName(
			const FString& PreferredName,
			const FString& FallbackName,
			int32 Occurrence,
			bool bAlwaysSuffix)
		{
			FString Name = MakeReadableToken(PreferredName);
			if (Name.IsEmpty())
			{
				Name = FallbackName;
			}
			if (bAlwaysSuffix || Occurrence > 0)
			{
				Name = MakeNumberedReadableName(Name, Occurrence + 1);
			}
			return Name;
		}

		bool TryMakePinAlias(
			const FString& PinName,
			const int32 PinOccurrence,
			const EGraphAuthoringEndpointDirection Direction,
			FString& OutAlias,
			FWorkspaceError& OutError)
		{
			OutAlias.Reset();
			if (PinOccurrence < 0)
			{
				return Fail(OutError, TEXT("graph_authoring_pin_occurrence_invalid"),
					TEXT("Pin occurrence must be a non-negative integer."),
					FString(),
					TEXT("/occurrence"),
					MakeShared<FJsonValueNumber>(PinOccurrence),
					MakeShared<FJsonValueString>(TEXT("integer >= 0")));
			}

			OutAlias = PinName;
			OutAlias.TrimStartAndEndInline();
			if (OutAlias.IsEmpty())
			{
				OutAlias = Direction == EGraphAuthoringEndpointDirection::Input
					? TEXT("Input")
					: TEXT("Output");
			}
			if (PinOccurrence > 0)
			{
				OutAlias = MakeNumberedReadableName(OutAlias, PinOccurrence + 1);
			}
			return true;
		}

		FString MakePinSelectorKey(
			FString Name,
			const int32 Occurrence,
			const EGraphAuthoringEndpointDirection Direction)
		{
			Name.TrimStartAndEndInline();
			return Name.ToLower() + TEXT("\n") + FString::FromInt(Occurrence)
				+ (Direction == EGraphAuthoringEndpointDirection::Input
					? TEXT("\ninput") : TEXT("\noutput"));
		}

		bool TryReadJsonPinDirection(
			const TSharedPtr<FJsonObject>& Pin,
			EGraphAuthoringEndpointDirection& OutDirection)
		{
			FString Direction;
			if (!Pin.IsValid()
				|| !Pin->TryGetStringField(TEXT("direction"), Direction))
			{
				return false;
			}
			if (Direction.Equals(TEXT("input"), ESearchCase::IgnoreCase))
			{
				OutDirection = EGraphAuthoringEndpointDirection::Input;
				return true;
			}
			if (Direction.Equals(TEXT("output"), ESearchCase::IgnoreCase))
			{
				OutDirection = EGraphAuthoringEndpointDirection::Output;
				return true;
			}
			return false;
		}

		bool TryReadJsonPinOccurrence(
			const TSharedPtr<FJsonObject>& Endpoint,
			int32& OutOccurrence)
		{
			OutOccurrence = 0;
			if (!Endpoint.IsValid())
			{
				return true;
			}
			const bool bHasEndpointOccurrence = Endpoint->HasField(TEXT("pin_occurrence"));
			const bool bHasInterfaceOccurrence = Endpoint->HasField(TEXT("occurrence"));
			if (bHasEndpointOccurrence && bHasInterfaceOccurrence)
			{
				return false;
			}
			if (!bHasEndpointOccurrence && !bHasInterfaceOccurrence)
			{
				return true;
			}
			const TCHAR* Field = bHasEndpointOccurrence
				? TEXT("pin_occurrence") : TEXT("occurrence");
			FString Lexeme;
			int64 Value = 0;
			const TSharedPtr<FJsonValue> JsonValue = Endpoint->TryGetField(Field);
			if (!JsonValue.IsValid() || !CanonicalJson::TryGetNumberLexeme(*JsonValue, Lexeme)
				|| !TryParseInt64Number(Lexeme, Value)
				|| Value < 0 || Value > MAX_int32)
			{
				return false;
			}
			OutOccurrence = static_cast<int32>(Value);
			return true;
		}

		bool TryCanonicalStringValue(
			const FString& Text,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FWorkspaceError& OutError)
		{
			FUeAgentStateSemanticError SemanticError;
			if (!FUeAgentCanonicalValue::TryString(
				Text, Resources, OutValue, SemanticError))
			{
				OutError.SetFailure(SemanticError);
				return false;
			}
			return true;
		}

		bool TryCanonicalIntegerValue(
			const int32 Value,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FWorkspaceError& OutError)
		{
			FUeAgentStateSemanticError SemanticError;
			if (!FUeAgentCanonicalValue::TryInteger(
				FString::FromInt(Value), Resources, OutValue, SemanticError))
			{
				OutError.SetFailure(SemanticError);
				return false;
			}
			return true;
		}

		const FUeAgentCanonicalValueRef* FindCanonicalField(
			const FUeAgentCanonicalValue& Object,
			const FString& Field)
		{
			if (Object.GetKind() != EUeAgentValueKind::Object)
			{
				return nullptr;
			}
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
				: Object.GetObjectItems())
			{
				if (Item.Key == Field)
				{
					return &Item.Value;
				}
			}
			return nullptr;
		}

		TSharedPtr<FJsonValue> PublicScalarField(const FUeAgentCanonicalValue& Object, const FString& Field)
		{
			const auto* Value = FindCanonicalField(Object, Field);
			return Value ? PublicScalar(Value->Get()) : nullptr;
		}

		bool TryReadCanonicalPinOccurrence(
			const FUeAgentCanonicalValue& Endpoint,
			int32& OutOccurrence)
		{
			OutOccurrence = 0;
			const FUeAgentCanonicalValueRef* EndpointOccurrence =
				FindCanonicalField(Endpoint, TEXT("pin_occurrence"));
			const FUeAgentCanonicalValueRef* InterfaceOccurrence =
				FindCanonicalField(Endpoint, TEXT("occurrence"));
			if (EndpointOccurrence != nullptr && InterfaceOccurrence != nullptr)
			{
				return false;
			}
			const FUeAgentCanonicalValueRef* Value = EndpointOccurrence
				? EndpointOccurrence : InterfaceOccurrence;
			if (Value == nullptr)
			{
				return true;
			}
			if ((*Value)->GetKind() != EUeAgentValueKind::Integer
				&& (*Value)->GetKind() != EUeAgentValueKind::Decimal)
			{
				return false;
			}
			int64 Parsed = 0;
			if (!TryParseInt64Number((*Value)->GetLexeme(), Parsed)
				|| Parsed < 0 || Parsed > MAX_int32)
			{
				return false;
			}
			OutOccurrence = static_cast<int32>(Parsed);
			return true;
		}

		bool TryCanonicalObjectFromMap(
			const TMap<FString, FUeAgentCanonicalValueRef>& Fields,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FWorkspaceError& OutError)
		{
			TArray<FString> Keys;
			Fields.GetKeys(Keys);
			Keys.Sort();
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Ordered;
			for (const FString& Key : Keys)
			{
				Ordered.Emplace(Key, Fields.FindChecked(Key));
			}
			FUeAgentStateSemanticError SemanticError;
			if (!FUeAgentCanonicalValue::TryObject(
				Ordered, Resources, OutValue, SemanticError))
			{
				OutError.SetFailure(SemanticError);
				return false;
			}
			return true;
		}

		TMap<FString, FUeAgentCanonicalValueRef> CanonicalObjectMap(
			const FUeAgentCanonicalValue& Object)
		{
			TMap<FString, FUeAgentCanonicalValueRef> Result;
			if (Object.GetKind() == EUeAgentValueKind::Object)
			{
				for (const TPair<FString, FUeAgentCanonicalValueRef>& Field
					: Object.GetObjectItems())
				{
					Result.Add(Field.Key, Field.Value);
				}
			}
			return Result;
		}

		bool TryReadCanonicalString(
			const FUeAgentCanonicalValue& Object,
			const FString& Field,
			FString& OutValue)
		{
			OutValue.Reset();
			const FUeAgentCanonicalValueRef* Value =
				FindCanonicalField(Object, Field);
			if (Value == nullptr
				|| (*Value)->GetKind() != EUeAgentValueKind::String)
			{
				return false;
			}
			OutValue = (*Value)->GetText();
			return true;
		}

		bool TryReadCanonicalPinDirection(
			const FUeAgentCanonicalValue& Pin,
			EGraphAuthoringEndpointDirection& OutDirection)
		{
			FString Direction;
			if (!TryReadCanonicalString(Pin, TEXT("direction"), Direction))
			{
				return false;
			}
			if (Direction.Equals(TEXT("input"), ESearchCase::IgnoreCase))
			{
				OutDirection = EGraphAuthoringEndpointDirection::Input;
				return true;
			}
			if (Direction.Equals(TEXT("output"), ESearchCase::IgnoreCase))
			{
				OutDirection = EGraphAuthoringEndpointDirection::Output;
				return true;
			}
			return false;
		}

		bool TryBuildCanonicalPinAliases(
			const FUeAgentCanonicalValue& Node,
			TMap<FString, FString>& OutAliasBySelector,
			TMap<FString, FGraphAuthoringPinAliasInput>& OutPinByAlias,
			FWorkspaceError& OutError)
		{
			OutAliasBySelector.Reset();
			OutPinByAlias.Reset();
			const FUeAgentCanonicalValueRef* Interface =
				FindCanonicalField(Node, TEXT("pin_interface"));
			if (Interface == nullptr)
			{
				return true;
			}
			if ((*Interface)->GetKind() != EUeAgentValueKind::Object)
			{
				return true;
			}
			const FUeAgentCanonicalValueRef* Pins =
				FindCanonicalField((*Interface).Get(), TEXT("pins"));
			if (Pins == nullptr || (*Pins)->GetKind() != EUeAgentValueKind::Array)
			{
				return true;
			}

			TArray<FGraphAuthoringPinAliasInput> Inputs;
			TMap<FString, FString> SelectorBySourceId;
			const TArray<FUeAgentCanonicalValueRef>& PinItems = (*Pins)->GetArrayItems();
			for (int32 PinIndex = 0; PinIndex < PinItems.Num(); ++PinIndex)
			{
				const FUeAgentCanonicalValueRef& Pin = PinItems[PinIndex];
				FString Name;
				int32 Occurrence = 0;
				EGraphAuthoringEndpointDirection Direction =
					EGraphAuthoringEndpointDirection::Input;
				const bool bObject = Pin->GetKind() == EUeAgentValueKind::Object;
				const bool bName = bObject
					&& TryReadCanonicalString(Pin.Get(), TEXT("name"), Name);
				if (bObject && !bName && FindCanonicalField(Pin.Get(), TEXT("alias")))
					return Fail(OutError, TEXT("semantic_workspace_graph_pin_capture_reexport_required"),
						TEXT("Captured pin has an alias but no native name; export this graph again."),
						FString(),
						FString::Printf(TEXT("/pin_interface/pins/%d/name"), PinIndex),
						nullptr,
						nullptr,
						true);
				const bool bOccurrence = bObject
					&& TryReadCanonicalPinOccurrence(Pin.Get(), Occurrence);
				const bool bDirection = bObject
					&& TryReadCanonicalPinDirection(Pin.Get(), Direction);
				if (!bObject || !bName || !bOccurrence || !bDirection)
				{
					const bool bBothOccurrences = bObject && FindCanonicalField(Pin.Get(), TEXT("occurrence"))
						&& FindCanonicalField(Pin.Get(), TEXT("pin_occurrence"));
					const FString Field = !bObject ? FString() : !bName ? TEXT("name")
						: !bOccurrence ? (FindCanonicalField(Pin.Get(), TEXT("pin_occurrence")) ? TEXT("pin_occurrence") : TEXT("occurrence"))
						: TEXT("direction");
					return Fail(OutError, TEXT("semantic_workspace_graph_pin_interface_invalid"),
						!bObject ? TEXT("Captured pin must be an object.")
						: !bName ? TEXT("Captured pin name must be a string.")
						: !bOccurrence ? (bBothOccurrences ? TEXT("Specify only one of occurrence and pin_occurrence.")
							: TEXT("Pin occurrence must be an integer between 0 and 2147483647."))
						: TEXT("Pin direction must be input or output."), FString(),
						FString::Printf(TEXT("/pin_interface/pins/%d"), PinIndex) + (Field.IsEmpty() ? FString() : TEXT("/") + Field),
						bObject ? PublicScalarField(Pin.Get(), Field) : PublicScalar(Pin.Get()), nullptr, true);
				}
				const FString SourceId = FString::FromInt(PinIndex);
				Inputs.Add({SourceId, Name, Occurrence, Direction});
				SelectorBySourceId.Add(
					SourceId, MakePinSelectorKey(Name, Occurrence, Direction));
			}

			FGraphAuthoringPinAliasProjection Projection;
			if (!TryBuildGraphAuthoringPinAliasProjection(Inputs, Projection, OutError))
			{
				return false;
			}
			for (const TPair<FString, FString>& Selector : SelectorBySourceId)
			{
				const FString& Alias =
					Projection.AliasBySourceId.FindChecked(Selector.Key);
				OutAliasBySelector.Add(Selector.Value, Alias);
				const int32 PinIndex = FCString::Atoi(*Selector.Key);
				if (Inputs.IsValidIndex(PinIndex))
				{
					OutPinByAlias.Add(Alias, Inputs[PinIndex]);
				}
			}
			return true;
		}

		bool TrySplitGraphEndpoint(
			const FString& Endpoint,
			FString& OutAlias,
			FString& OutPin)
		{
			const int32 Separator = Endpoint.Find(TEXT("."));
			if (Separator <= 0 || Separator >= Endpoint.Len() - 1)
			{
				return false;
			}
			OutAlias = Endpoint.Left(Separator).TrimStartAndEnd();
			OutPin = Endpoint.Mid(Separator + 1).TrimStartAndEnd();
			return !OutAlias.IsEmpty() && !OutPin.IsEmpty();
		}

		bool IsMachineField(const FString& Field)
		{
			const FString Normalized = Field.ToLower();
			return Normalized == TEXT("id") || Normalized == TEXT("guid")
				|| Normalized == TEXT("graph_declaration_owned")
				|| Normalized.EndsWith(TEXT("_guid"))
				|| Normalized.EndsWith(TEXT("_uuid"))
				|| Normalized == TEXT("hash")
				|| Normalized.EndsWith(TEXT("_hash"))
				|| Normalized == TEXT("asset_path")
				|| Normalized == TEXT("expression_path")
				|| Normalized == TEXT("native_object_path")
				|| Normalized == TEXT("native_key")
				|| Normalized == TEXT("parent_native_key")
				|| Normalized == TEXT("node_id")
				|| Normalized == TEXT("pin_id")
				|| Normalized == TEXT("edge_id")
				|| Normalized == TEXT("element_id")
				|| Normalized == TEXT("class_path")
				|| Normalized == TEXT("class_ref")
				|| Normalized == TEXT("settings_class")
				|| Normalized == TEXT("expression_class")
				|| Normalized == TEXT("expression_class_ref")
				|| Normalized == TEXT("material_function_path")
				|| Normalized == TEXT("script_struct_ref")
				|| Normalized == TEXT("node_index")
				|| Normalized == TEXT("pin_index")
				|| Normalized == TEXT("occurrence")
				|| Normalized == TEXT("pin_occurrence")
				|| Normalized == TEXT("operation");
		}

		bool IsRemovedGraphElement(const TSharedPtr<FJsonObject>& Element)
		{
			FString Operation;
			return Element.IsValid()
				&& Element->TryGetStringField(TEXT("operation"), Operation)
				&& Operation.Equals(TEXT("remove"), ESearchCase::IgnoreCase);
		}
	}

	bool TryResolveCapturedGraphNodeReference(
		const FUeAgentCanonicalValue& Nodes, const FString& Reference,
		FString& OutNodeId, FWorkspaceError& OutError)
	{
		OutNodeId.Reset();
		OutError.Reset();
		if (Nodes.GetKind() != EUeAgentValueKind::Object)
			return Fail(OutError, TEXT("semantic_workspace_graph_nodes_invalid"),
				TEXT("Captured nodes must be an object."),
				Reference,
				TEXT("/nodes"),
				nullptr,
				nullptr,
				true);
		const auto* Node = FindCanonicalField(Nodes, Reference);
		if (!Node) return Fail(OutError, TEXT("semantic_workspace_graph_node_unresolved"),
			TEXT("The captured node reference does not resolve to a node."),
			Reference,
			TEXT("/nodes"),
			nullptr,
			nullptr,
			true);
		if ((*Node)->GetKind() != EUeAgentValueKind::Object
			|| !TryReadCanonicalString(Node->Get(), TEXT("id"), OutNodeId) || OutNodeId.IsEmpty())
			return Fail(OutError, TEXT("semantic_workspace_graph_node_identity_missing"),
				TEXT("The captured node must be an object with a non-empty native identity."),
				Reference,
				TEXT("/nodes"),
				nullptr,
				nullptr,
				true);
		return true;
	}

	bool IsGraphNodeNativeNameProperty(const FString& PropertyName)
	{
		return PropertyName == TEXT("ParameterName") || PropertyName == TEXT("InputName")
			|| PropertyName == TEXT("OutputName") || PropertyName == TEXT("Name");
	}

	bool IsGraphItemContentOnlyField(const FString& SemanticType, const FString& FieldName)
	{
		// TryCaptureTextureGraph produces id/name/direction/cpp_type beside this
		// evaluated string. Those structural fields still invalidate projection;
		// only the evaluated payload belongs to the existing content writer.
		return SemanticType == TEXT("uai.semantic.texture_graph.pin") && FieldName == TEXT("value");
	}

	const FUeAgentCanonicalValueRef* FindGraphNodeNativeNameProperty(
		const FString& PropertyName, const TMap<FString, FUeAgentCanonicalValueRef>& Values)
	{
		if (!IsGraphNodeNativeNameProperty(PropertyName)) return nullptr;
		for (const TCHAR* Field : {TEXT("literal"), TEXT("value")})
		{
			const auto* Value = Values.Find(Field);
			if (!Value) continue;
			if ((*Value)->GetKind() == EUeAgentValueKind::String) return Value;
			if ((*Value)->GetKind() == EUeAgentValueKind::Object)
			{
				const auto* Literal = FindCanonicalField(Value->Get(), TEXT("literal"));
				if (Literal && (*Literal)->GetKind() == EUeAgentValueKind::String) return Literal;
			}
		}
		return nullptr;
	}

	FString ReadGraphNodeSemanticName(
		const TMap<FString, FUeAgentCanonicalValueRef>& Values,
		const TMap<FString, FUeAgentCanonicalValueRef>* NativePropertyValues)
	{
		auto ReadName = [](const TMap<FString, FUeAgentCanonicalValueRef>& Source, const TCHAR* Field)
		{
			const FUeAgentCanonicalValueRef* Entry = Source.Find(Field);
			FString Value = Entry && (*Entry)->GetKind() == EUeAgentValueKind::String ? (*Entry)->GetText() : FString();
			Value.TrimStartAndEndInline();
			return Value;
		};
		if (NativePropertyValues)
		{
			for (const TCHAR* Field : {TEXT("ParameterName"), TEXT("InputName"), TEXT("OutputName")})
			{
				const FString Value = ReadName(*NativePropertyValues, Field);
				if (!Value.IsEmpty() && !Value.Equals(TEXT("None"), ESearchCase::IgnoreCase)) return Value;
			}
		}
		for (const TCHAR* Field : {TEXT("parameter_name"), TEXT("input_name"), TEXT("output_name"),
			TEXT("event_name"), TEXT("function_name"), TEXT("variable_name"), TEXT("node_name"),
			TEXT("operation_name"), TEXT("authored_title"), TEXT("label"), TEXT("title")})
		{
			const FString Value = ReadName(Values, Field);
			if (!Value.IsEmpty()) return Value;
		}
		if (NativePropertyValues)
		{
			const FString Value = ReadName(*NativePropertyValues, TEXT("Name"));
			if (!Value.IsEmpty() && !Value.Equals(TEXT("None"), ESearchCase::IgnoreCase)) return Value;
		}
		for (const TCHAR* Field : {TEXT("name"), TEXT("candidate_name"), TEXT("role")})
		{
			const FString Value = ReadName(Values, Field);
			if (!Value.IsEmpty()) return Value;
		}
		return FString();
	}

	FString ReadGraphNodeNativeIdentity(const TMap<FString, FUeAgentCanonicalValueRef>& Values)
	{
		for (const TCHAR* Field : {TEXT("id"), TEXT("builtin_id")})
			if (const auto* Value = Values.Find(Field);
				Value && (*Value)->GetKind() == EUeAgentValueKind::String && !(*Value)->GetText().IsEmpty())
				return (*Value)->GetText();
		return FString();
	}

FString ResolveGraphModelItemOwner(const FAuthoringModel& Model,
	const FString& ItemSemanticType, const FString& ItemOwner)
{
	FString GraphOwner = ItemOwner;
	// Ordinary owned property objects stay on their direct owner. Nested
	// GraphItem routes use .node; an owned graph starts a new name scope.
	if (ItemSemanticType.EndsWith(TEXT(".node")))
	{
		auto Parent = Model.FindDeclaration(GraphOwner);
		while (Parent && Parent->GetUnitKind() == ESemanticUnitKind::ItemState)
		{
			GraphOwner = Parent->GetOwnerRef();
			Parent = Model.FindDeclaration(GraphOwner);
		}
	}
	return GraphOwner;
}

bool TryBuildGraphModelNodeAliases(const FAuthoringModel& Model, const FString& GraphId,
		FGraphAuthoringAliasProjection& OutProjection, FWorkspaceError& OutError)
	{
		TSet<FString> NodeIds;
		for (const auto& Node : Model.GetDeclarations())
		{
			if (Node.GetUnitKind() != ESemanticUnitKind::ItemState) continue;
		if (ResolveGraphModelItemOwner(Model, Node.GetSemanticTypeRef(), Node.GetOwnerRef()) == GraphId)
			NodeIds.Add(Node.GetUnitId());
		}
		TMap<FString, TMap<FString, FUeAgentCanonicalValueRef>> PropertiesByOwner;
		int32 NamingProperties = 0;
		for (const auto& Declaration : Model.GetDeclarations())
		{
			if (Declaration.GetUnitKind() != ESemanticUnitKind::PropertySet
				|| !NodeIds.Contains(Declaration.GetOwnerRef()) || !IsGraphNodeNativeNameProperty(Declaration.GetFieldSetRef())) continue;
			const auto* Property = Model.FindUnit(Declaration.GetUnitId());
			if (!Property) { Model.CheckValueReadResult(OutError); return false; }
			++NamingProperties;
			if (const auto* Value = FindGraphNodeNativeNameProperty(Property->GetFieldSetRef(), Property->GetValues()))
				PropertiesByOwner.FindOrAdd(Property->GetOwnerRef()).Add(Property->GetFieldSetRef(), *Value);
		}
		TArray<FGraphAuthoringNodeAliasInput> Inputs;
		for (const auto& Declaration : Model.GetDeclarations()) if (NodeIds.Contains(Declaration.GetUnitId()))
		{
			const auto* Node = Model.FindUnit(Declaration.GetUnitId());
			if (!Node) { Model.CheckValueReadResult(OutError); return false; }
			Inputs.Add({Node->GetUnitId(), ReadGraphNodeSemanticName(
				Node->GetValues(), PropertiesByOwner.Find(Node->GetUnitId())),
				ReadGraphNodeNativeIdentity(Node->GetValues())});
		}
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=graph_name_consumers nodes=%d naming_properties=%d complete_value_array=false"), Inputs.Num(), NamingProperties);
		return TryBuildGraphAuthoringAliasProjection(Inputs, OutProjection, OutError);
	}

	bool TryBuildGraphAuthoringAliasProjection(
		const TArray<FGraphAuthoringNodeAliasInput>& Nodes,
		FGraphAuthoringAliasProjection& OutProjection,
		FWorkspaceError& OutError)
	{
		OutProjection = {};
		OutError.Reset();
		FGraphAuthoringAliasProjection Projection;
		TArray<FGraphAuthoringNodeAliasInput> Ordered = Nodes;
		Ordered.Sort([](
			const FGraphAuthoringNodeAliasInput& Left,
			const FGraphAuthoringNodeAliasInput& Right)
		{
			FString LeftName = MakeReadableToken(Left.SemanticName).ToLower();
			FString RightName = MakeReadableToken(Right.SemanticName).ToLower();
			const FString& LeftIdentity = Left.NativeIdentity.IsEmpty() ? Left.SourceId : Left.NativeIdentity;
			const FString& RightIdentity = Right.NativeIdentity.IsEmpty() ? Right.SourceId : Right.NativeIdentity;
			return LeftName == RightName
				? LeftIdentity < RightIdentity
				: LeftName < RightName;
		});
		TMap<FString, int32> Occurrences;
		TSet<FString> UsedAliases;
		TSet<FString> SemanticAliases;
		for (const FGraphAuthoringNodeAliasInput& Node : Ordered)
			if (!MakeReadableToken(Node.SemanticName).IsEmpty())
				SemanticAliases.Add(MakeIndexedName(Node.SemanticName, TEXT("Node"), 0, false).ToLower());

		for (const FGraphAuthoringNodeAliasInput& Node : Ordered)
		{
			FString TrimmedSourceId = Node.SourceId;
			TrimmedSourceId.TrimStartAndEndInline();
			if (TrimmedSourceId.IsEmpty())
			{
				return Fail(OutError, TEXT("graph_authoring_source_id_invalid"),
					FString::Printf(TEXT("Node '%s' has an empty captured identity."), *Node.SemanticName),
					FString(),
					TEXT("/"),
					nullptr,
					nullptr,
					true);
			}
			if (Projection.AliasBySourceId.Contains(Node.SourceId))
			{
				return Fail(OutError, TEXT("graph_authoring_source_id_collision"),
					FString::Printf(TEXT("Node '%s' repeats a captured identity."), *Node.SemanticName),
					Node.SourceId,
					TEXT("/"),
					nullptr,
					nullptr,
					true);
			}

			FString SemanticName = Node.SemanticName;
			SemanticName.TrimStartAndEndInline();
			FString OccurrenceKey = MakeReadableToken(SemanticName).ToLower();
			if (OccurrenceKey.IsEmpty())
			{
				OccurrenceKey = TEXT("node");
			}
			int32& Occurrence = Occurrences.FindOrAdd(OccurrenceKey);
			const FString Base = MakeIndexedName(SemanticName, TEXT("Node"), 0, false);
			const FString Alias = AllocateReadableName(Base, [&](const FString& Candidate) {
				return UsedAliases.Contains(Candidate.ToLower())
					|| ((!Candidate.Equals(Base, ESearchCase::IgnoreCase) || MakeReadableToken(SemanticName).IsEmpty())
						&& SemanticAliases.Contains(Candidate.ToLower()));
			}, ++Occurrence);
			UsedAliases.Add(Alias.ToLower());

			// Keep the caller's lookup key. Its wrapper format must not determine
			// which native node receives a duplicate-name suffix.
			Projection.AliasBySourceId.Add(Node.SourceId, Alias);
			Projection.SourceIdByAlias.Add(Alias, Node.SourceId);
		}
		OutProjection = MoveTemp(Projection);
		return true;
	}

	bool TryParseGraphAuthoringEndpoint(
		const FString& Endpoint,
		FString& OutNodeAlias,
		FString& OutPinAlias,
		FWorkspaceError& OutError)
	{
		OutNodeAlias.Reset();
		OutPinAlias.Reset();
		OutError.Reset();
		if (!TrySplitGraphEndpoint(Endpoint, OutNodeAlias, OutPinAlias))
		{
			return Fail(OutError, TEXT("graph_authoring_endpoint_invalid"),
				TEXT("Graph endpoint must contain a non-empty node alias and pin alias separated by a dot."),
				FString(),
				TEXT("/"),
				MakeShared<FJsonValueString>(Endpoint),
				MakeShared<FJsonValueString>(TEXT("NodeAlias.PinAlias")));
		}
		return true;
	}

	bool TryBuildGraphAuthoringPinAliasProjection(
		const TArray<FGraphAuthoringPinAliasInput>& Pins,
		FGraphAuthoringPinAliasProjection& OutProjection,
		FWorkspaceError& OutError)
	{
		OutProjection = {};
		OutError.Reset();
		FGraphAuthoringPinAliasProjection Projection;
		TSet<FString> UsedAliases;
		TSet<FString> SemanticAliases;
		for (const auto& Pin : Pins)
			if (!Pin.SemanticName.TrimStartAndEnd().IsEmpty()) SemanticAliases.Add(Pin.SemanticName.TrimStartAndEnd().ToLower());

		for (const FGraphAuthoringPinAliasInput& Pin : Pins)
		{
			const FString& SourceId = Pin.SourceId;
			if (SourceId.TrimStartAndEnd().IsEmpty())
			{
				return Fail(OutError, TEXT("graph_authoring_pin_source_id_invalid"),
					FString::Printf(TEXT("Pin '%s' has an empty captured identity."), *Pin.SemanticName),
					FString(),
					TEXT("/"),
					nullptr,
					nullptr,
					true);
			}
			if (Projection.AliasBySourceId.Contains(SourceId))
			{
				return Fail(OutError, TEXT("graph_authoring_pin_source_id_collision"),
					FString::Printf(TEXT("Pin '%s' repeats a captured identity."), *Pin.SemanticName),
					SourceId,
					TEXT("/"),
					nullptr,
					nullptr,
					true);
			}

			FString Alias;
			if (!TryMakePinAlias(
					Pin.SemanticName,
					Pin.Occurrence,
					Pin.Direction,
					Alias,
					OutError))
			{
				return false;
			}
			FString Base;
			if (!TryMakePinAlias(Pin.SemanticName, 0, Pin.Direction, Base, OutError)) return false;
			Alias = AllocateReadableName(Base, [&](const FString& Candidate) {
				return UsedAliases.Contains(Candidate.ToLower())
					|| ((!Candidate.Equals(Base, ESearchCase::IgnoreCase) || Pin.SemanticName.TrimStartAndEnd().IsEmpty())
						&& SemanticAliases.Contains(Candidate.ToLower()));
			}, Pin.Occurrence + 1);
			UsedAliases.Add(Alias.ToLower());

			Projection.AliasBySourceId.Add(SourceId, Alias);
			Projection.SourceIdByAlias.Add(Alias, SourceId);
		}

		OutProjection = MoveTemp(Projection);
		return true;
	}

	bool TryProjectGraphAuthoringPinInterface(
		const TSharedRef<FJsonObject>& Captured,
		TSharedRef<FJsonObject>& OutInterface,
		FWorkspaceError& OutError,
		TMap<FString, FString>* OutAliasBySelector)
	{
		OutInterface = MakeShared<FJsonObject>();
		OutInterface->Values = Captured->Values;
		if (OutAliasBySelector) OutAliasBySelector->Reset();
		const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
		if (!Captured->TryGetArrayField(TEXT("pins"), Pins)) return true;
		TArray<FGraphAuthoringPinAliasInput> Inputs;
		for (int32 Index = 0; Index < Pins->Num(); ++Index)
		{
			const auto& Value = (*Pins)[Index];
			if (!Value.IsValid() || Value->Type != EJson::Object)
				return Fail(OutError, TEXT("semantic_workspace_graph_pin_interface_invalid"),
					TEXT("Captured pin interface entry must be an object."),
					FString(),
					FString::Printf(TEXT("/pins/%d"), Index),
					nullptr,
					nullptr,
					true);
			const auto Pin = Value->AsObject();
			FString Name;
			int32 Occurrence = 0;
			EGraphAuthoringEndpointDirection Direction;
			// Already projected interfaces are consumed unchanged, not re-numbered.
			if (!Pin->HasField(TEXT("name")) && Pin->HasField(TEXT("alias"))) continue;
			const FString PinPath = FString::Printf(TEXT("/pins/%d"), Index);
			if (!Pin->TryGetStringField(TEXT("name"), Name))
				return Fail(OutError, TEXT("semantic_workspace_graph_pin_interface_invalid"),
					TEXT("Captured pin name must be a string."), FString(), PinPath + TEXT("/name"), nullptr, nullptr, true);
			if (!TryReadJsonPinOccurrence(Pin, Occurrence))
				return Fail(OutError, TEXT("semantic_workspace_graph_pin_interface_invalid"),
					Pin->HasField(TEXT("occurrence")) && Pin->HasField(TEXT("pin_occurrence"))
						? TEXT("Specify only one of occurrence and pin_occurrence.") : TEXT("Pin occurrence must be an integer between 0 and 2147483647."),
					FString(), PinPath + (Pin->HasField(TEXT("pin_occurrence")) ? TEXT("/pin_occurrence") : TEXT("/occurrence")),
					PublicScalar(Pin->TryGetField(Pin->HasField(TEXT("pin_occurrence")) ? TEXT("pin_occurrence") : TEXT("occurrence"))), nullptr, true);
			if (!TryReadJsonPinDirection(Pin, Direction))
				return Fail(OutError, TEXT("semantic_workspace_graph_pin_interface_invalid"),
					TEXT("Pin direction must be input or output."), FString(), PinPath + TEXT("/direction"),
					PublicScalar(Pin->TryGetField(TEXT("direction"))), nullptr, true);
			Inputs.Add({FString::FromInt(Index), Name, Occurrence, Direction});
		}
		if (Inputs.IsEmpty()) return true;
		if (Inputs.Num() != Pins->Num())
			return Fail(OutError, TEXT("semantic_workspace_graph_pin_interface_mixed_projection"),
				TEXT("A pin interface cannot mix native pin names with already projected aliases."),
				FString(),
				TEXT("/pins"),
				MakeShared<FJsonValueNumber>(Inputs.Num()),
				MakeShared<FJsonValueNumber>(Pins->Num()),
				true);
		FGraphAuthoringPinAliasProjection Projection;
		if (!TryBuildGraphAuthoringPinAliasProjection(Inputs, Projection, OutError)) return false;
		TArray<TSharedPtr<FJsonValue>> Visible;
		for (int32 Index = 0; Index < Pins->Num(); ++Index)
		{
			const auto Pin = (*Pins)[Index]->AsObject();
			const FString& Alias = Projection.AliasBySourceId.FindChecked(FString::FromInt(Index));
			const auto Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("alias"), Alias);
			for (const auto& Field : Pin->Values)
				if (Field.Key != TEXT("name") && !IsMachineField(Field.Key))
					Row->SetField(Field.Key, Field.Value);
			Visible.Add(MakeShared<FJsonValueObject>(Row));
			if (OutAliasBySelector)
				OutAliasBySelector->Add(MakePinSelectorKey(Inputs[Index].SemanticName,
					Inputs[Index].Occurrence, Inputs[Index].Direction), Alias);
		}
		OutInterface->SetArrayField(TEXT("pins"), MoveTemp(Visible));
		return true;
	}

	TSharedRef<FJsonObject> ProjectGraphAuthoringPinDefaults(
		const TSharedRef<FJsonObject>& Captured,
		const TMap<FString, FString>& AliasBySelector)
	{
		auto Visible = MakeShared<FJsonObject>();
		for (const auto& Entry : Captured->Values)
		{
			const auto Default = Entry.Value.IsValid() && Entry.Value->Type == EJson::Object
				? Entry.Value->AsObject() : nullptr;
			FString PinName;
			int32 Occurrence = 0;
			if (!Default || !Default->TryGetStringField(TEXT("pin_name"), PinName)
				|| !TryReadJsonPinOccurrence(Default, Occurrence)) continue;
			const FString Alias = AliasBySelector.FindRef(MakePinSelectorKey(
				PinName, Occurrence, EGraphAuthoringEndpointDirection::Input));
			if (Alias.IsEmpty()) continue;
			auto Value = MakeShared<FJsonObject>();
			for (const auto& Field : Default->Values)
				if (Field.Key != TEXT("pin_name") && !IsMachineField(Field.Key))
					Value->SetField(Field.Key, Field.Value);
			Visible->SetObjectField(Alias, Value);
		}
		return Visible;
	}

	bool TryFormatGraphAuthoringEndpoint(
		const FString& NodeAlias,
		const FString& PinName,
		int32 PinOccurrence,
		EGraphAuthoringEndpointDirection Direction,
		FString& OutEndpoint,
		FWorkspaceError& OutError)
	{
		OutEndpoint.Reset();
		OutError.Reset();
		FString NormalizedAlias = NodeAlias;
		NormalizedAlias.TrimStartAndEndInline();
		if (NormalizedAlias.IsEmpty())
		{
			return Fail(OutError, TEXT("graph_authoring_node_alias_invalid"),
				TEXT("Node alias cannot be empty or whitespace."),
				FString(),
				TEXT("/node"),
				MakeShared<FJsonValueString>(NodeAlias),
				nullptr);
		}
		FString Pin;
		if (!TryMakePinAlias(
				PinName,
				PinOccurrence,
				Direction,
				Pin,
				OutError))
		{
			return false;
		}
		OutEndpoint = NormalizedAlias + TEXT(".") + Pin;
		return true;
	}

	bool IsGraphAuthoringStructureType(const FString& SemanticTypeRef, bool bHasCapturedNodesAndEdges)
	{
		return SemanticTypeRef.EndsWith(TEXT(".graph"))
			|| SemanticTypeRef.Contains(TEXT(".graph.")) || bHasCapturedNodesAndEdges;
	}

	bool FGraphAuthoringConnectionContext::IsUnchanged(const FString& From, const FString& To) const
	{
		return bHasResolvedIntent && UnchangedConnections.Contains(From + TEXT("\n") + To);
	}

	static FString MakeNativeConnectionKey(const FString& FromNode, const FString& FromPin,
		const FString& ToNode, const FString& ToPin)
	{
		// Length framing also supports native pin names containing punctuation.
		return FString::Printf(TEXT("%d:%s%d:%s%d:%s%d:%s"), FromNode.Len(), *FromNode,
			FromPin.Len(), *FromPin, ToNode.Len(), *ToNode, ToPin.Len(), *ToPin);
	}

	void FGraphAuthoringConnectionContext::RecordUnchanged(const FString& FromNode, const FString& FromPin,
		const FString& ToNode, const FString& ToPin)
	{
		UnchangedConnections.Add(MakeNativeConnectionKey(FromNode, FromPin, ToNode, ToPin));
	}

	bool FGraphAuthoringConnectionContext::IsUnchanged(const FString& FromNode, const FString& FromPin,
		const FString& ToNode, const FString& ToPin) const
	{
		return bHasResolvedIntent && UnchangedConnections.Contains(
			MakeNativeConnectionKey(FromNode, FromPin, ToNode, ToPin));
	}

	bool TryProjectGraphAuthoringVisibleRow(
		const FString& UnitId,
		const TSharedRef<FJsonObject>& CompactRow,
		TSharedRef<FJsonObject>& OutRow,
		TSharedPtr<FJsonObject>& OutInterfaceRow,
		FWorkspaceError& OutError,
		FGraphAuthoringConnectionContext* OutConnectionContext)
	{
		OutError.Reset();
		OutInterfaceRow.Reset();
		if (OutConnectionContext) { *OutConnectionContext = {}; OutConnectionContext->bHasResolvedIntent = true; }
		const TSharedPtr<FJsonObject>* Values = nullptr;
		if (!CompactRow->TryGetObjectField(TEXT("values"), Values)
			|| Values == nullptr || !Values->IsValid())
		{
			return Fail(OutError, TEXT("composition_graph_projection_invalid"),
				TEXT("Captured graph row is missing its values object."),
				UnitId,
				TEXT("/values"),
				nullptr,
				nullptr,
				true);
		}

		OutRow = MakeShared<FJsonObject>();
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : (*Values)->Values)
		{
			if (Field.Key == TEXT("graph") && Field.Value.IsValid()
				&& Field.Value->Type == EJson::Object)
			{
				TSharedRef<FJsonObject> VisibleGraph = MakeShared<FJsonObject>();
				for (const TPair<FString, TSharedPtr<FJsonValue>>& GraphField
					: Field.Value->AsObject()->Values)
				{
					if (!IsMachineField(GraphField.Key))
					{
						VisibleGraph->SetField(GraphField.Key, GraphField.Value);
					}
				}
				OutRow->SetObjectField(Field.Key, VisibleGraph);
			}
			else if (Field.Key != TEXT("nodes") && Field.Key != TEXT("edges")
				&& !IsMachineField(Field.Key))
			{
				OutRow->SetField(Field.Key, Field.Value);
			}
		}

		const TSharedPtr<FJsonObject>* Nodes = nullptr;
		if (!(*Values)->TryGetObjectField(TEXT("nodes"), Nodes)
			|| Nodes == nullptr || !Nodes->IsValid())
		{
			return Fail(OutError, TEXT("composition_graph_nodes_invalid"), TEXT("Captured graph nodes must be an object."), UnitId, TEXT("/nodes"), nullptr, nullptr, true);
		}
		TSharedRef<FJsonObject> VisibleNodes = MakeShared<FJsonObject>();
		TSharedRef<FJsonObject> InterfaceNodes = MakeShared<FJsonObject>();
		TMap<FString, FString> AliasByNodeId;
		TMap<FString, TMap<FString, FString>> PinAliasesByNodeId;
		TArray<FString> NodeAliases;
		(*Nodes)->Values.GetKeys(NodeAliases);
		NodeAliases.Sort();
		for (const FString& Alias : NodeAliases)
		{
			const TSharedPtr<FJsonObject>* Spec = nullptr;
			if (!(*Nodes)->TryGetObjectField(Alias, Spec)
				|| Spec == nullptr || !Spec->IsValid())
			{
				return Fail(OutError, TEXT("composition_graph_node_invalid"),
					FString::Printf(TEXT("Captured node '%s' must be an object."), *Alias),
					UnitId,
					TEXT("/nodes/") + EscapeJsonPointerSegment(Alias),
					nullptr,
					nullptr,
					true);
			}
			if (IsRemovedGraphElement(*Spec))
			{
				continue;
			}
			FString NodeId;
			if ((*Spec)->TryGetStringField(TEXT("id"), NodeId) && !NodeId.IsEmpty())
			{
				AliasByNodeId.Add(NodeId, Alias);
			}
			TSharedRef<FJsonObject> VisibleSpec = MakeShared<FJsonObject>();
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : (*Spec)->Values)
			{
				if (Field.Key == TEXT("pin_interface") && Field.Value.IsValid()
					&& Field.Value->Type == EJson::Object)
				{
					TSharedRef<FJsonObject> InterfaceSpec = MakeShared<FJsonObject>();
					TMap<FString, FString> AliasBySelector;
					if (!TryProjectGraphAuthoringPinInterface(Field.Value->AsObject().ToSharedRef(),
						InterfaceSpec, OutError, &AliasBySelector))
						return ForwardCapturedFailure(OutError, TEXT("/nodes/") + EscapeJsonPointerSegment(Alias) + TEXT("/pin_interface"));
					if (!NodeId.IsEmpty()) PinAliasesByNodeId.Add(NodeId, MoveTemp(AliasBySelector));
					InterfaceNodes->SetObjectField(Alias, InterfaceSpec);
				}
				else if (Field.Key != TEXT("pin_interface")
					&& Field.Key != TEXT("candidate_context")
					&& !IsMachineField(Field.Key))
				{
					VisibleSpec->SetField(Field.Key, Field.Value);
				}
			}

			// Pin defaults are part of the writable graph, but their captured map
			// keys and selectors are machine state. Project them through the same
			// node-local aliases as the read-only interface companion.
			if (const TSharedPtr<FJsonObject>* CapturedPinDefaults = nullptr;
				(*Spec)->TryGetObjectField(TEXT("pin_defaults"), CapturedPinDefaults)
				&& CapturedPinDefaults != nullptr && CapturedPinDefaults->IsValid())
			{
				VisibleSpec->RemoveField(TEXT("pin_defaults"));
				TSharedRef<FJsonObject> VisiblePinDefaults = MakeShared<FJsonObject>();
				const TMap<FString, FString>* AliasBySelector = NodeId.IsEmpty()
					? nullptr : PinAliasesByNodeId.Find(NodeId);
				if (AliasBySelector != nullptr)
				{
					VisiblePinDefaults = ProjectGraphAuthoringPinDefaults(
						CapturedPinDefaults->ToSharedRef(), *AliasBySelector);
				}
				if (!VisiblePinDefaults->Values.IsEmpty())
				{
					VisibleSpec->SetObjectField(
						TEXT("pin_defaults"), VisiblePinDefaults);
				}
			}
			VisibleNodes->SetObjectField(Alias, VisibleSpec);
		}
		for (const auto& Pair : VisibleNodes->Values)
		{
			const auto Spec = Pair.Value->AsObject();
			FString Parent;
			if (!Spec->HasField(TEXT("parent"))) continue;
			if (!Spec->TryGetStringField(TEXT("parent"), Parent) || !AliasByNodeId.Contains(Parent))
				return Fail(OutError, TEXT("composition_graph_parent_unresolved"),
					!Spec->HasTypedField<EJson::String>(TEXT("parent")) ? TEXT("Captured parent reference must be a string.") : TEXT("Captured parent reference does not resolve to a node."),
					UnitId,
					TEXT("/nodes/") + EscapeJsonPointerSegment(Pair.Key) + TEXT("/parent"),
					nullptr,
					nullptr,
					true);
			Spec->SetStringField(TEXT("parent"), AliasByNodeId.FindChecked(Parent));
		}
		OutRow->SetObjectField(TEXT("nodes"), VisibleNodes);
		const TSharedPtr<FJsonObject>* VisibleGraph = nullptr;
		if (OutRow->TryGetObjectField(TEXT("graph"), VisibleGraph)
			&& (*VisibleGraph)->HasField(TEXT("node_references")))
		{
			const TSharedPtr<FJsonObject>* References = nullptr;
			if (!(*VisibleGraph)->TryGetObjectField(TEXT("node_references"), References))
				return Fail(OutError, TEXT("composition_graph_node_references_invalid"),
					TEXT("Captured graph node_references must be an object."),
					UnitId,
					TEXT("/graph/node_references"),
					nullptr,
					nullptr,
					true);
			auto Projected = MakeShared<FJsonObject>();
			for (const auto& Reference : (*References)->Values)
			{
				FString NodeId;
				if (!Reference.Value.IsValid() || !Reference.Value->TryGetString(NodeId)
					|| (!NodeId.IsEmpty() && !AliasByNodeId.Contains(NodeId)))
					return Fail(OutError, TEXT("composition_graph_node_reference_unresolved"),
						!Reference.Value.IsValid() || Reference.Value->Type != EJson::String ? TEXT("Captured node reference must be a string.") : TEXT("Captured node reference does not resolve to a node."),
						UnitId,
						TEXT("/graph/node_references/") + EscapeJsonPointerSegment(Reference.Key),
						nullptr,
						nullptr,
						true);
				Projected->SetStringField(Reference.Key,
					NodeId.IsEmpty() ? FString() : AliasByNodeId.FindChecked(NodeId));
			}
			(*VisibleGraph)->SetObjectField(TEXT("node_references"), Projected);
		}
		if (!InterfaceNodes->Values.IsEmpty())
		{
			TSharedRef<FJsonObject> InterfaceGraph = MakeShared<FJsonObject>();
			const TSharedPtr<FJsonObject>* CapturedGraph = nullptr;
			if ((*Values)->TryGetObjectField(TEXT("graph"), CapturedGraph)
				&& CapturedGraph != nullptr && CapturedGraph->IsValid())
			{
				for (const TCHAR* Field : {TEXT("name"), TEXT("graph_kind")})
				{
					if ((*CapturedGraph)->HasField(Field))
					{
						InterfaceGraph->SetField(
							Field, (*CapturedGraph)->TryGetField(Field));
					}
				}
			}
			OutInterfaceRow = MakeShared<FJsonObject>();
			OutInterfaceRow->SetObjectField(TEXT("graph"), InterfaceGraph);
			OutInterfaceRow->SetObjectField(TEXT("nodes"), InterfaceNodes);
		}

		const TSharedPtr<FJsonObject>* Edges = nullptr;
		if (!(*Values)->TryGetObjectField(TEXT("edges"), Edges)
			|| Edges == nullptr || !Edges->IsValid())
		{
			return Fail(OutError, TEXT("composition_graph_edges_invalid"),
				TEXT("Captured graph edges must be an object."),
				UnitId,
				TEXT("/connections"),
				nullptr,
				nullptr,
				true);
		}
		TArray<FString> EdgeKeys;
		(*Edges)->Values.GetKeys(EdgeKeys);
		EdgeKeys.Sort();
		TArray<TSharedPtr<FJsonValue>> Connections;
		auto TryFormatCapturedEndpoint = [&](const FString& NodeId,
			const FString& NodeAlias, const FString& PinName,
			const int32 PinOccurrence,
			const EGraphAuthoringEndpointDirection Direction,
			FString& OutEndpoint)
		{
			FString PinAlias;
			if (const TMap<FString, FString>* PinAliases =
				PinAliasesByNodeId.Find(NodeId))
			{
				PinAlias = PinAliases->FindRef(
					MakePinSelectorKey(PinName, PinOccurrence, Direction));
			}
			return TryFormatGraphAuthoringEndpoint(
				NodeAlias,
				PinAlias.IsEmpty() ? PinName : PinAlias,
				PinAlias.IsEmpty() ? PinOccurrence : 0,
				Direction,
				OutEndpoint,
				OutError);
		};
		for (const FString& EdgeKey : EdgeKeys)
		{
			const TSharedPtr<FJsonObject>* Edge = nullptr;
			if (!(*Edges)->TryGetObjectField(EdgeKey, Edge)
				|| Edge == nullptr || !Edge->IsValid())
			{
				return Fail(OutError, TEXT("composition_graph_edge_invalid"), TEXT("Captured edge must be an object."), EdgeKey, TEXT("/connections"), nullptr, nullptr, true);
			}
			if (IsRemovedGraphElement(*Edge))
			{
				continue;
			}
			FString FromEndpoint;
			FString ToEndpoint;
			if (!(*Edge)->TryGetStringField(TEXT("from"), FromEndpoint)
				|| !(*Edge)->TryGetStringField(TEXT("to"), ToEndpoint))
			{
				const TSharedPtr<FJsonObject>* From = nullptr;
				const TSharedPtr<FJsonObject>* To = nullptr;
				FString FromNode;
				FString FromPin;
				FString ToNode;
				FString ToPin;
				int32 FromPinOccurrence = 0;
				int32 ToPinOccurrence = 0;
				auto ReadEndpoint = [&](const TCHAR* Field, const TSharedPtr<FJsonObject>*& Endpoint,
					FString& Node, FString& Pin, int32& Occurrence)
				{
					const FString Path = TEXT("/connections/") + FString(Field);
					if (!(*Edge)->TryGetObjectField(Field, Endpoint) || !Endpoint || !Endpoint->IsValid())
						return Fail(OutError, TEXT("composition_graph_edge_invalid"),
							TEXT("Captured edge endpoint must be an object with node and pin selectors."), EdgeKey, Path, nullptr, nullptr, true);
					if (!(*Endpoint)->TryGetStringField(TEXT("node"), Node))
						return Fail(OutError, TEXT("composition_graph_edge_invalid"),
							TEXT("Captured endpoint node selector is missing or is not a string."), EdgeKey, Path + TEXT("/node"), nullptr, nullptr, true);
					if (!(*Endpoint)->TryGetStringField(TEXT("pin"), Pin))
						return Fail(OutError, TEXT("composition_graph_edge_invalid"),
							TEXT("Captured endpoint pin selector is missing or is not a string."), EdgeKey, Path + TEXT("/pin"), nullptr, nullptr, true);
					if (!TryReadJsonPinOccurrence(*Endpoint, Occurrence))
					{
						const bool bBoth = (*Endpoint)->HasField(TEXT("occurrence")) && (*Endpoint)->HasField(TEXT("pin_occurrence"));
						const TCHAR* OccurrenceField = (*Endpoint)->HasField(TEXT("pin_occurrence")) ? TEXT("pin_occurrence") : TEXT("occurrence");
						return Fail(OutError, TEXT("composition_graph_edge_invalid"),
							bBoth ? TEXT("Specify only one of occurrence and pin_occurrence.") : TEXT("Pin occurrence must be an integer between 0 and 2147483647."),
							EdgeKey, Path + TEXT("/") + OccurrenceField, PublicScalar((*Endpoint)->TryGetField(OccurrenceField)), nullptr, true);
					}
					return true;
				};
				if (!ReadEndpoint(TEXT("from"), From, FromNode, FromPin, FromPinOccurrence)
					|| !ReadEndpoint(TEXT("to"), To, ToNode, ToPin, ToPinOccurrence)) return false;
				const FString* FromAlias = AliasByNodeId.Find(FromNode);
				const FString* ToAlias = AliasByNodeId.Find(ToNode);
				if (FromAlias == nullptr || ToAlias == nullptr)
				{
					return Fail(OutError, TEXT("composition_graph_edge_node_unresolved"),
						FromAlias == nullptr ? TEXT("Captured edge source node does not resolve to a node alias.") : TEXT("Captured edge destination node does not resolve to a node alias."),
						EdgeKey,
						FromAlias == nullptr ? TEXT("/connections/from") : TEXT("/connections/to"),
						nullptr,
						nullptr,
						true);
				}
				if (!TryFormatCapturedEndpoint(
						FromNode, *FromAlias, FromPin, FromPinOccurrence,
						EGraphAuthoringEndpointDirection::Output, FromEndpoint))
					return ForwardCapturedFailure(OutError, TEXT("/connections/from"));
				if (!TryFormatCapturedEndpoint(
						ToNode, *ToAlias, ToPin, ToPinOccurrence,
						EGraphAuthoringEndpointDirection::Input, ToEndpoint))
				{
					return ForwardCapturedFailure(OutError, TEXT("/connections/to"));
				}
			}
			TSharedRef<FJsonObject> Connection = MakeShared<FJsonObject>();
			Connection->SetStringField(TEXT("from"), FromEndpoint);
			Connection->SetStringField(TEXT("to"), ToEndpoint);
			FString EdgeOperation;
			if (OutConnectionContext && (*Edge)->TryGetStringField(TEXT("operation"), EdgeOperation)
				&& EdgeOperation == TEXT("update"))
				OutConnectionContext->UnchangedConnections.Add(FromEndpoint + TEXT("\n") + ToEndpoint);
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : (*Edge)->Values)
			{
				if (Field.Key != TEXT("from") && Field.Key != TEXT("to")
					&& !IsMachineField(Field.Key))
				{
					Connection->SetField(Field.Key, Field.Value);
				}
			}
			Connections.Add(MakeShared<FJsonValueObject>(Connection));
		}
		OutRow->SetArrayField(TEXT("connections"), MoveTemp(Connections));
		return true;
	}

	void ProjectGraphAuthoringPinDefaultChanges(const FJsonObject& CapturedNode, FJsonObject& DesiredNode)
	{
		const TSharedPtr<FJsonObject>* Before = nullptr;
		const TSharedPtr<FJsonObject>* After = nullptr;
		if (!CapturedNode.TryGetObjectField(TEXT("pin_defaults"), Before)
			|| !DesiredNode.TryGetObjectField(TEXT("pin_defaults"), After)) return;
		auto Changes = MakeShared<FJsonObject>(**After);
		for (const auto& Pin : (*Before)->Values)
			if (CanonicalJson::AreJsonValuesEquivalent(Pin.Value, Changes->TryGetField(Pin.Key)))
				Changes->RemoveField(Pin.Key);
		DesiredNode.SetObjectField(TEXT("pin_defaults"), Changes);
	}

	bool TryProjectGraphAuthoringNodeChanges(
		const FString& UnitId,
		const TMap<FString, FUeAgentCanonicalValueRef>& CapturedValues,
		const TMap<FString, FUeAgentCanonicalValueRef>& ResolvedValues,
		TSharedPtr<FJsonObject>& OutNodes, FWorkspaceError& OutError, bool bRetainContextNodes)
	{
		OutNodes = MakeShared<FJsonObject>();
		const auto* BeforeNodes = CapturedValues.Find(TEXT("nodes"));
		const auto* AfterNodes = ResolvedValues.Find(TEXT("nodes"));
		if (!AfterNodes) return true;
		if ((*AfterNodes)->GetKind() != EUeAgentValueKind::Object
			|| (BeforeNodes && (*BeforeNodes)->GetKind() != EUeAgentValueKind::Object))
			return Fail(OutError, TEXT("semantic_workspace_graph_nodes_invalid"),
				(*AfterNodes)->GetKind() != EUeAgentValueKind::Object ? TEXT("Resolved graph nodes must be an object.") : TEXT("Captured graph nodes must be an object."),
				UnitId,
				TEXT("/nodes"),
				nullptr,
				nullptr,
				true);
		auto Resources = MakeSemanticWorkspaceResourceTracker();
		TMap<FString, FUeAgentCanonicalValueRef> BeforeById;
		if (BeforeNodes)
			for (const auto& Entry : (*BeforeNodes)->GetObjectItems())
			{
				FString Id;
				if (TryReadCanonicalString(Entry.Value.Get(), TEXT("id"), Id)) BeforeById.Add(Id, Entry.Value);
			}
		for (const auto& Entry : (*AfterNodes)->GetObjectItems())
		{
			FString Id, Operation;
			TryReadCanonicalString(Entry.Value.Get(), TEXT("id"), Id);
			TryReadCanonicalString(Entry.Value.Get(), TEXT("operation"), Operation);
			auto Fields = CanonicalObjectMap(Entry.Value.Get());
			if (Operation == TEXT("update"))
				if (const auto* Before = BeforeById.Find(Id))
				{
					auto OldFields = CanonicalObjectMap(Before->Get());
					auto NewFields = Fields;
					OldFields.Remove(TEXT("operation")); NewFields.Remove(TEXT("operation"));
					if (!TryNormalizeGraphAuthoringNodePosition(OldFields, UnitId, Resources, OutError))
						return ForwardCapturedFailure(OutError, TEXT("/nodes/") + EscapeJsonPointerSegment(Entry.Key));
					if (!TryNormalizeGraphAuthoringNodePosition(NewFields, UnitId, Resources, OutError))
					{
						OutError.PrependPath(TEXT("/nodes/") + EscapeJsonPointerSegment(Entry.Key));
						return false;
					}
					FUeAgentCanonicalValuePtr OldValue, NewValue;
					if (!TryCanonicalObjectFromMap(OldFields, Resources, OldValue, OutError)
						|| !TryCanonicalObjectFromMap(NewFields, Resources, NewValue, OutError)) return false;
					FUeAgentSemanticComparison Comparison;
					FUeAgentStateSemanticError Error;
					if (!TryCompareValues(OldValue.ToSharedRef(), NewValue.ToSharedRef(), TEXT("/"), Resources, Comparison, Error))
						{ OutError.SetFailure(Error, UnitId); return false; }
					if (Comparison.IsEquivalent())
					{
						if (!bRetainContextNodes) continue;
						Fields.Remove(TEXT("pin_defaults"));
					}
				}
			FUeAgentCanonicalValuePtr Changed;
			if (!TryCanonicalObjectFromMap(Fields, Resources, Changed, OutError)) return false;
			TSharedPtr<FJsonValue> Json;
			UE::UAI::Contract::FDiagnostic Detail;
			if (!CanonicalJson::TryToJsonValue(*Changed, Json, Detail))
				return ForwardFailure(OutError, MoveTemp(Detail), UnitId, TEXT("/nodes/") + EscapeJsonPointerSegment(Entry.Key));
			if (Operation == TEXT("update"))
				if (const auto* Before = BeforeById.Find(Id))
				{
					TSharedPtr<FJsonValue> CapturedJson;
					if (!CanonicalJson::TryToJsonValue(Before->Get(), CapturedJson, Detail))
						return ForwardFailure(OutError, MoveTemp(Detail), UnitId, TEXT("/nodes/") + EscapeJsonPointerSegment(Entry.Key));
					ProjectGraphAuthoringPinDefaultChanges(*CapturedJson->AsObject(), *Json->AsObject());
				}
			OutNodes->SetField(Entry.Key, Json);
		}
		return true;
	}

	bool TryProjectGraphAuthoringConsumerDocument(
		const FString& UnitId,
		const TMap<FString, FUeAgentCanonicalValueRef>& ResolvedValues,
		TSharedRef<FJsonObject>& OutDocument,
		FWorkspaceError& OutError,
		const FResolvedSemanticWorkspace* Workspace,
		FGraphAuthoringConnectionContext* OutConnectionContext)
	{
		OutError.Reset();
		TSharedRef<FJsonObject> Values = MakeShared<FJsonObject>();
		for (const TPair<FString, FUeAgentCanonicalValueRef>& Field : ResolvedValues)
		{
			TSharedPtr<FJsonValue> JsonValue;
			UE::UAI::Contract::FDiagnostic ConversionError;
			if (!CanonicalJson::TryToJsonValue(
					*Field.Value, JsonValue, ConversionError)
				|| !JsonValue.IsValid())
			{
				OutError.SetFailure(ConversionError, UnitId, TEXT("/") + EscapeJsonPointerSegment(Field.Key)); return false;
			}
			Values->SetField(Field.Key, MoveTemp(JsonValue));
		}
		if (Workspace)
		{
			for (const TCHAR* CollectionName : {TEXT("nodes"), TEXT("edges")})
			{
				const TSharedPtr<FJsonObject>* Collection = nullptr;
				if (!Values->TryGetObjectField(CollectionName, Collection)) continue;
				for (const auto& Item : (*Collection)->Values)
				{
					if (!Item.Value.IsValid() || Item.Value->Type != EJson::Object) continue;
					FString Token = Item.Key;
					Token.ReplaceInline(TEXT("~"), TEXT("~0"));
					Token.ReplaceInline(TEXT("/"), TEXT("~1"));
					Workspace->AttachInlinePropertySources(UnitId,
						TEXT("/") + FString(CollectionName) + TEXT("/") + Token, *Item.Value->AsObject());
				}
			}
		}

		TSharedRef<FJsonObject> CompactRow = MakeShared<FJsonObject>();
		CompactRow->SetObjectField(TEXT("values"), Values);
		TSharedPtr<FJsonObject> IgnoredInterface;
		if (OutConnectionContext) *OutConnectionContext = {};
		return TryProjectGraphAuthoringVisibleRow(
			UnitId, CompactRow, OutDocument, IgnoredInterface, OutError, Workspace ? OutConnectionContext : nullptr);
	}

	void FindGraphAuthoringDiagnosticSources(const FResolvedSemanticWorkspace& Workspace,
		const FString& DocumentPointer, const FSourceProvenance*& OutExactSource,
		TArray<const FSourceProvenance*>& OutRelatedSources, const FString& GraphUnitId)
	{
		OutExactSource = nullptr;
		OutRelatedSources.Reset();
		TArray<FString> Parts;
		DocumentPointer.ParseIntoArray(Parts, TEXT("/"), false);
		for (FString& Part : Parts) Part = Part.Replace(TEXT("~1"), TEXT("/")).Replace(TEXT("~0"), TEXT("~"));
		int32 GraphIndex = INDEX_NONE;
		for (int32 Index = 0; Index + 3 < Parts.Num(); ++Index)
			if ((Parts[Index] == TEXT("graphs") || Parts[Index] == TEXT("owned_graphs"))
				&& Parts[Index + 2] == TEXT("nodes")) GraphIndex = Index;
		const int32 NodeIndex = !Parts.IsEmpty() && Parts[0].IsEmpty() ? 1 : 0;
		const bool bScoped = !GraphUnitId.IsEmpty() && Parts.Num() > NodeIndex + 1 && Parts[NodeIndex] == TEXT("nodes");
		if (GraphIndex == INDEX_NONE && !bScoped) return;
		const FString GraphId = bScoped ? FString() : Parts[GraphIndex + 1];
		const FString& NodeId = Parts[bScoped ? NodeIndex + 1 : GraphIndex + 3];
		auto Escape = [](const FString& Value) { return Value.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")); };
		auto SameIdentity = [](const FString& A, const FString& B)
		{
			if (A.Equals(B, ESearchCase::IgnoreCase)) return true;
			// Native diagnostics and captured graph state may format the same GUID
			// differently. Match its value without changing ordinary named identities.
			FGuid GuidA, GuidB;
			return FGuid::Parse(A, GuidA) && FGuid::Parse(B, GuidB) && GuidA == GuidB;
		};
		auto Matches = [&SameIdentity](const FUeAgentCanonicalValue& Object, const FString& Id, std::initializer_list<const TCHAR*> Fields)
		{
			for (const TCHAR* Field : Fields)
			{
				FString Value;
				if (TryReadCanonicalString(Object, Field, Value) && SameIdentity(Value, Id)) return true;
			}
			return false;
		};
		FString Suffix;
		for (int32 Index = bScoped ? NodeIndex + 2 : GraphIndex + 4; Index < Parts.Num(); ++Index) Suffix += TEXT("/") + Escape(Parts[Index]);
		TArray<const FSourceProvenance*> ExactSources;
		for (const auto& Unit : Workspace.GetModel()->GetUnits())
		{
			if (bScoped && Unit.GetUnitId() != GraphUnitId) continue;
			const auto* Graph = Unit.FindValue(TEXT("graph"));
			const auto* Nodes = Unit.FindValue(TEXT("nodes"));
			if (!Nodes || (*Nodes)->GetKind() != EUeAgentValueKind::Object
				|| !IsGraphAuthoringStructureType(Unit.GetSemanticTypeRef(), Unit.HasValue(TEXT("edges")))) continue;
			bool bGraphMatches = bScoped || (Graph && Matches(**Graph, GraphId, {TEXT("graph_guid"), TEXT("id"), TEXT("name")}));
			// Graph entries keep their native identity fields at the value root;
			// document projections may instead carry a nested graph record.
			for (const TCHAR* Field : {TEXT("graph_guid"), TEXT("id"), TEXT("name")})
				if (const auto* Value = Unit.FindValue(Field))
					bGraphMatches |= (*Value)->GetKind() == EUeAgentValueKind::String
						&& SameIdentity((*Value)->GetText(), GraphId);
			if (!bGraphMatches) continue;
			for (const auto& Node : (*Nodes)->GetObjectItems())
			{
				if (!SameIdentity(Node.Key, NodeId)
					&& !Matches(*Node.Value, NodeId, {TEXT("node_guid"), TEXT("id")})) continue;
				const FString NodePointer = TEXT("/nodes/") + Escape(Node.Key);
				if (const auto* Source = Workspace.FindValueSource(Unit.GetUnitId(), NodePointer + Suffix)) ExactSources.AddUnique(Source);
				if (const auto* Source = Workspace.FindValueSource(Unit.GetUnitId(), NodePointer)) OutRelatedSources.AddUnique(Source);
				const auto* Edges = Unit.FindValue(TEXT("edges"));
				if (!Edges || (*Edges)->GetKind() != EUeAgentValueKind::Object) continue;
				FString NativeId;
				if (!TryReadCanonicalString(*Node.Value, TEXT("id"), NativeId))
					TryReadCanonicalString(*Node.Value, TEXT("node_guid"), NativeId);
				if (NativeId.IsEmpty()) NativeId = Node.Key;
				for (const auto& Edge : (*Edges)->GetObjectItems())
				{
					bool bIncident = false;
					for (const TCHAR* Endpoint : {TEXT("from"), TEXT("to")})
						if (const auto* Value = FindCanonicalField(*Edge.Value, Endpoint))
							bIncident |= Matches(**Value, NativeId, {TEXT("node")});
					if (bIncident)
						if (const auto* Source = Workspace.FindValueSource(Unit.GetUnitId(), TEXT("/edges/") + Escape(Edge.Key)))
							OutRelatedSources.AddUnique(Source);
				}
			}
		}
		if (ExactSources.Num() == 1) OutExactSource = ExactSources[0];
		else OutRelatedSources.Append(ExactSources);
	}

	bool TryNormalizeGraphAuthoringNodePosition(
		TMap<FString, FUeAgentCanonicalValueRef>& InOutFields,
		const FString& SourceUnit, FUeAgentResourceTracker& Resources, FWorkspaceError& OutError)
	{
		OutError.Reset();
		const TCHAR* PositionAliases[] = {TEXT("pos"), TEXT("position"), TEXT("editor_position")};
		const FUeAgentCanonicalValueRef* Position = nullptr;
		FString PositionField;
		for (const TCHAR* Alias : PositionAliases)
		{
			if (const auto* Candidate = InOutFields.Find(Alias))
			{
				if (Position)
					return Fail(OutError, TEXT("semantic_workspace_graph_position_ambiguous"),
						FString::Printf(TEXT("Node position is specified by both '%s' and '%s'; use one position field."), *PositionField, Alias),
						SourceUnit, TEXT("/") + FString(Alias));
				Position = Candidate;
				PositionField = Alias;
			}
		}
		if (!Position) return true;
		const FString PositionPath = TEXT("/") + PositionField;
		FUeAgentCanonicalValueRef X = *Position, Y = *Position;
		FString XPath, YPath;
		if ((*Position)->GetKind() == EUeAgentValueKind::Array)
		{
			const auto& Items = (*Position)->GetArrayItems();
			if (Items.Num() != 2)
				return Fail(OutError, TEXT("semantic_workspace_graph_position_invalid"),
					TEXT("Node position array must contain exactly two coordinates [x, y]."), SourceUnit, PositionPath,
					MakeShared<FJsonValueNumber>(Items.Num()), MakeShared<FJsonValueNumber>(2));
			X = Items[0]; Y = Items[1];
			XPath = PositionPath + TEXT("/0"); YPath = PositionPath + TEXT("/1");
		}
		else if ((*Position)->GetKind() == EUeAgentValueKind::Object)
		{
			const auto* ObjectX = FindCanonicalField(Position->Get(), TEXT("x"));
			const auto* ObjectY = FindCanonicalField(Position->Get(), TEXT("y"));
			if (!ObjectX || !ObjectY)
				return Fail(OutError, TEXT("semantic_workspace_graph_position_invalid"),
					!ObjectX ? TEXT("Node position is missing coordinate x.") : TEXT("Node position is missing coordinate y."),
					SourceUnit, PositionPath + (!ObjectX ? TEXT("/x") : TEXT("/y")));
			if ((*Position)->GetObjectItems().Num() != 2)
			{
				for (const auto& Field : (*Position)->GetObjectItems())
					if (Field.Key != TEXT("x") && Field.Key != TEXT("y"))
						return Fail(OutError, TEXT("semantic_workspace_graph_position_invalid"),
							FString::Printf(TEXT("Node position permits only x and y; '%s' is not a coordinate."), *Field.Key),
							SourceUnit, PositionPath + TEXT("/") + EscapeJsonPointerSegment(Field.Key));
			}
			X = *ObjectX; Y = *ObjectY;
			XPath = PositionPath + TEXT("/x"); YPath = PositionPath + TEXT("/y");
		}
		else
			return Fail(OutError, TEXT("semantic_workspace_graph_position_invalid"),
				TEXT("Node position must be [x, y] or an object containing x and y."), SourceUnit, PositionPath, PublicScalar(Position->Get()));

		auto NormalizeCoordinate = [&](FUeAgentCanonicalValueRef& Coordinate, const FString& Path)
		{
			double Parsed = 0.0;
			if ((Coordinate->GetKind() != EUeAgentValueKind::Integer && Coordinate->GetKind() != EUeAgentValueKind::Decimal)
				|| !TryParseFiniteDecimal(Coordinate->GetLexeme(), Parsed))
				return Fail(OutError, TEXT("semantic_workspace_graph_position_invalid"),
					TEXT("Node position coordinate must be a finite number."), SourceUnit, Path,
					PublicScalar(Coordinate.Get()), MakeShared<FJsonValueString>(TEXT("finite number")));
			FUeAgentCanonicalValuePtr Decimal;
			FUeAgentStateSemanticError Error;
			// Preserve the exact lexeme and the composed model's decimal representation.
			FString Lexeme = Coordinate->GetLexeme();
			if (!Lexeme.Contains(TEXT(".")) && !Lexeme.Contains(TEXT("e")) && !Lexeme.Contains(TEXT("E"))) Lexeme += TEXT(".0");
			if (!FUeAgentCanonicalValue::TryDecimal(Lexeme, Resources, Decimal, Error))
				return ForwardFailure(OutError, MoveTemp(Error), SourceUnit, Path);
			Coordinate = Decimal.ToSharedRef();
			return true;
		};
		if (!NormalizeCoordinate(X, XPath) || !NormalizeCoordinate(Y, YPath)) return false;
		FUeAgentCanonicalValuePtr Normalized;
		FUeAgentStateSemanticError Error;
		if (!FUeAgentCanonicalValue::TryObject({{TEXT("x"), X}, {TEXT("y"), Y}}, Resources, Normalized, Error))
			return ForwardFailure(OutError, MoveTemp(Error), SourceUnit, PositionPath);
		for (const TCHAR* Alias : PositionAliases) InOutFields.Remove(Alias);
		InOutFields.Add(TEXT("pos"), Normalized.ToSharedRef());
		return true;
	}

	bool TryResolveGraphAuthoringProjection(
		const FUeAgentCanonicalValue& VisibleRoot,
		const FUeAgentCanonicalValue& CapturedRoot,
		const FString& SourceUnit,
		FUeAgentResourceTracker& Resources,
		TMap<FString, FUeAgentCanonicalValueRef>& OutValues,
		FWorkspaceError& OutError,
		const FHjsonNode* SourceRoot,
		FHjsonNodePtr* OutSourceRoot)
	{
		OutValues.Reset();
		OutError.Reset();
		if (OutSourceRoot) OutSourceRoot->Reset();
		const FScopedGraphDiagnosticSource DiagnosticSource(OutError, SourceRoot);
		if (CapturedRoot.GetKind() != EUeAgentValueKind::Object
			|| VisibleRoot.GetKind() != EUeAgentValueKind::Object)
		{
			return Fail(OutError, TEXT("semantic_workspace_graph_shape_invalid"), VisibleRoot.GetKind() != EUeAgentValueKind::Object ? TEXT("Authored graph root must be an object.") : TEXT("Captured graph root must be an object."), SourceUnit, TEXT("/"), nullptr, nullptr,
				VisibleRoot.GetKind() == EUeAgentValueKind::Object);
		}

		OutValues = CanonicalObjectMap(CapturedRoot);
		const TMap<FString, FUeAgentCanonicalValueRef> VisibleFields =
			CanonicalObjectMap(VisibleRoot);
		for (const TPair<FString, FUeAgentCanonicalValueRef>& Field : VisibleFields)
		{
			if (Field.Key == TEXT("graph"))
			{
				const FUeAgentCanonicalValueRef* CapturedGraph =
					FindCanonicalField(CapturedRoot, TEXT("graph"));
				if (CapturedGraph == nullptr
					|| (*CapturedGraph)->GetKind() != EUeAgentValueKind::Object
					|| Field.Value->GetKind() != EUeAgentValueKind::Object)
				{
					return Fail(OutError, TEXT("semantic_workspace_graph_shape_invalid"), Field.Value->GetKind() != EUeAgentValueKind::Object ? TEXT("Authored graph header must be an object.") : TEXT("Captured graph header is missing or is not an object."), SourceUnit, TEXT("/graph"), nullptr, nullptr,
						Field.Value->GetKind() == EUeAgentValueKind::Object);
				}
				auto GraphFields = CanonicalObjectMap((*CapturedGraph).Get());
				const auto* DeclarationOwned = GraphFields.Find(TEXT("graph_declaration_owned"));
				for (const auto& GraphField : Field.Value->GetObjectItems())
				{
					if (IsMachineField(GraphField.Key))
					{
						return Fail(OutError, TEXT("semantic_workspace_graph_machine_field_forbidden"),
							FString::Printf(TEXT("Graph field '%s' is maintained by UAI and cannot be authored."), *GraphField.Key),
							SourceUnit,
							TEXT("/graph/") + EscapeJsonPointerSegment(GraphField.Key));
					}
					const bool bDisplayMetadata = GraphField.Key == TEXT("display_name")
						|| GraphField.Key == TEXT("parent_graph") || GraphField.Key == TEXT("query_name");
					if (bDisplayMetadata || (DeclarationOwned && (*DeclarationOwned)->GetKind() == EUeAgentValueKind::Boolean
						&& (*DeclarationOwned)->GetBoolean()
						&& (GraphField.Key == TEXT("name") || GraphField.Key == TEXT("graph_kind"))))
					{
						const auto* Original = GraphFields.Find(GraphField.Key);
						const FString Path = TEXT("/graph/") + EscapeJsonPointerSegment(GraphField.Key);
						const FString Code = bDisplayMetadata ? TEXT("semantic_workspace_graph_header_read_only")
							: TEXT("semantic_workspace_graph_declaration_read_only");
						if (!Original || GraphField.Value->GetKind() != EUeAgentValueKind::String
							|| (*Original)->GetKind() != EUeAgentValueKind::String || (*Original)->GetText() != GraphField.Value->GetText())
						{
							if (!Original)
								return Fail(OutError, Code, TEXT("This read-only graph field is absent from the captured header and cannot be added."),
									SourceUnit, Path, PublicScalar(GraphField.Value.Get()));
							if ((*Original)->GetKind() != EUeAgentValueKind::String)
								return Fail(OutError, Code, TEXT("Captured read-only graph header field must be a string."),
									SourceUnit, Path, nullptr, nullptr, true);
							FUeAgentSemanticComparison Comparison;
							FUeAgentStateSemanticError Error;
							if (!TryCompareValues(*Original, GraphField.Value, Path, Resources, Comparison, Error))
								return ForwardFailure(OutError, MoveTemp(Error), SourceUnit, TEXT("/"));
							OutError.SetFailure(MakeComparisonDiagnostic(Comparison, Code,
								FString::Printf(TEXT("Graph field '%s' is read-only."), *GraphField.Key), TEXT("graph_authoring")), SourceUnit, Path);
							return false;
						}
					}
					GraphFields.Add(GraphField.Key, GraphField.Value);
				}
				FUeAgentCanonicalValuePtr ResolvedGraph;
				if (!TryCanonicalObjectFromMap(
					GraphFields, Resources, ResolvedGraph, OutError))
				{
					return false;
				}
				OutValues.Add(TEXT("graph"), ResolvedGraph.ToSharedRef());
			}
			else if (Field.Key != TEXT("nodes")
				&& Field.Key != TEXT("connections")
				&& Field.Key != TEXT("interface_ref"))
			{
				OutValues.Add(Field.Key, Field.Value);
			}
		}

		const FUeAgentCanonicalValueRef* CapturedNodesValue =
			FindCanonicalField(CapturedRoot, TEXT("nodes"));
		const FUeAgentCanonicalValueRef* VisibleNodesValue =
			FindCanonicalField(VisibleRoot, TEXT("nodes"));
		const FUeAgentCanonicalValueRef* CapturedEdgesValue =
			FindCanonicalField(CapturedRoot, TEXT("edges"));
		const FUeAgentCanonicalValueRef* VisibleConnectionsValue =
			FindCanonicalField(VisibleRoot, TEXT("connections"));
		if (!CapturedNodesValue || (*CapturedNodesValue)->GetKind() != EUeAgentValueKind::Object)
			return Fail(OutError, TEXT("semantic_workspace_graph_shape_invalid"),
				TEXT("Captured nodes are missing or are not an object."), SourceUnit, TEXT("/nodes"), nullptr, nullptr, true);
		if (!CapturedEdgesValue || (*CapturedEdgesValue)->GetKind() != EUeAgentValueKind::Object)
			return Fail(OutError, TEXT("semantic_workspace_graph_shape_invalid"),
				TEXT("Captured edges are missing or are not an object."), SourceUnit, TEXT("/connections"), nullptr, nullptr, true);
		if (VisibleNodesValue && (*VisibleNodesValue)->GetKind() != EUeAgentValueKind::Object)
			return Fail(OutError, TEXT("semantic_workspace_graph_shape_invalid"),
				TEXT("nodes must be an object keyed by node alias."), SourceUnit, TEXT("/nodes"));
		if (VisibleConnectionsValue && (*VisibleConnectionsValue)->GetKind() != EUeAgentValueKind::Array)
			return Fail(OutError, TEXT("semantic_workspace_graph_shape_invalid"),
				TEXT("connections must be an array."), SourceUnit, TEXT("/connections"));

		const auto CapturedNodes = CanonicalObjectMap((*CapturedNodesValue).Get());
		TMap<FString, FUeAgentCanonicalValueRef> VisibleNodes;
		if (VisibleNodesValue)
		{
			VisibleNodes = CanonicalObjectMap((*VisibleNodesValue).Get());
		}
		else
		{
			// Omission retains membership and all captured fields. Empty visible
			// rows avoid reinterpreting captured machine identities as public aliases.
			FUeAgentCanonicalValuePtr UnchangedNode;
			if (!TryCanonicalObjectFromMap({}, Resources, UnchangedNode, OutError)) return false;
			for (const auto& Captured : CapturedNodes)
				VisibleNodes.Add(Captured.Key, UnchangedNode.ToSharedRef());
		}
		TMap<FString, FUeAgentCanonicalValueRef> ResolvedNodes;
		TMap<FString, FString> AliasByNodeId;
		TMap<FString, FString> NodeIdByAlias;
		TMap<FString, TMap<FString, FString>> PinAliasesByNodeId;
		TMap<FString, TMap<FString, FGraphAuthoringPinAliasInput>>
			PinsByAliasByNodeId;
		FUeAgentCanonicalValuePtr UpdateOperation;
		FUeAgentCanonicalValuePtr CreateOperation;
		FUeAgentCanonicalValuePtr RemoveOperation;
		if (!TryCanonicalStringValue(TEXT("update"), Resources, UpdateOperation, OutError)
			|| !TryCanonicalStringValue(TEXT("create"), Resources, CreateOperation, OutError)
			|| !TryCanonicalStringValue(TEXT("remove"), Resources, RemoveOperation, OutError))
		{
			return false;
		}

		FWorkspaceError NodeErrors;
		auto CollectNodeError = [&](const FString& Alias)
		{
			const FHjsonNode* Nodes = SourceRoot ? SourceRoot->Get(TEXT("nodes")) : nullptr;
			const FHjsonNode* Node = Nodes ? Nodes->Get(Alias) : nullptr;
			if (Node) OutError.Locate(*Node);
			NodeErrors.Append(OutError);
			OutError.Reset();
		};
		for (const auto& Captured : CapturedNodes)
		{
			FString NodeId;
			if (Captured.Value->GetKind() != EUeAgentValueKind::Object
				|| !TryReadCanonicalString(Captured.Value.Get(), TEXT("id"), NodeId)
				|| NodeId.IsEmpty())
			{
				return Fail(OutError, TEXT("semantic_workspace_graph_node_identity_missing"),
					FString::Printf(TEXT("Captured node '%s' must be an object with a non-empty native identity."), *Captured.Key),
					SourceUnit,
					TEXT("/nodes/") + EscapeJsonPointerSegment(Captured.Key),
					nullptr,
					nullptr,
					true);
			}
			AliasByNodeId.Add(NodeId, Captured.Key);
			NodeIdByAlias.Add(Captured.Key, NodeId);
			TMap<FString, FString> PinAliases;
			TMap<FString, FGraphAuthoringPinAliasInput> PinsByAlias;
			if (!TryBuildCanonicalPinAliases(
					Captured.Value.Get(), PinAliases, PinsByAlias, OutError))
			{
				return ForwardCapturedFailure(OutError, TEXT("/nodes/") + EscapeJsonPointerSegment(Captured.Key));
			}
			PinAliasesByNodeId.Add(NodeId, MoveTemp(PinAliases));
			PinsByAliasByNodeId.Add(NodeId, MoveTemp(PinsByAlias));
		}
		for (const auto& Visible : VisibleNodes)
		{
			if (!CapturedNodes.Contains(Visible.Key))
			{
				NodeIdByAlias.Add(Visible.Key, Visible.Key);
				AliasByNodeId.Add(Visible.Key, Visible.Key);
			}
		}

		auto TryResolveCandidateContext = [&](
			const FUeAgentCanonicalValue& VisibleContext,
			FUeAgentCanonicalValuePtr& OutContext)
		{
			if (VisibleContext.GetKind() != EUeAgentValueKind::Object)
			{
				return Fail(OutError, TEXT("semantic_workspace_graph_candidate_context_invalid"),
					TEXT("candidate_context must be an object."),
					SourceUnit,
					TEXT("/candidate_context"),
					PublicScalar(VisibleContext));
			}
			FString NodeAlias;
			if (!TryReadCanonicalString(VisibleContext, TEXT("node"), NodeAlias))
			{
				return Fail(OutError, TEXT("semantic_workspace_graph_candidate_context_node_missing"),
					TEXT("candidate_context.node is required and must be a string containing a node alias."),
					SourceUnit,
					TEXT("/candidate_context/node"),
					PublicScalarField(VisibleContext, TEXT("node")));
			}
			const FString* NodeId = NodeIdByAlias.Find(NodeAlias);
			if (NodeId == nullptr)
			{
				return Fail(OutError, TEXT("semantic_workspace_graph_candidate_context_node_unresolved"),
					TEXT("candidate_context.node does not name a node in this graph."),
					SourceUnit,
					TEXT("/candidate_context/node"),
					MakeShared<FJsonValueString>(NodeAlias),
					nullptr);
			}
			auto ContextFields = CanonicalObjectMap(VisibleContext);
			for (const auto& ContextField : ContextFields)
			{
				if (IsMachineField(ContextField.Key))
				{
					return Fail(OutError, TEXT("semantic_workspace_graph_machine_field_forbidden"),
						FString::Printf(TEXT("Candidate context field '%s' is maintained by UAI and cannot be authored."), *ContextField.Key),
						SourceUnit,
						TEXT("/candidate_context/") + EscapeJsonPointerSegment(ContextField.Key));
				}
			}
			FUeAgentCanonicalValuePtr ResolvedNode;
			FUeAgentCanonicalValuePtr SourceAlias;
			if (!TryCanonicalStringValue(*NodeId, Resources, ResolvedNode, OutError)
				|| !TryCanonicalStringValue(NodeAlias, Resources, SourceAlias, OutError))
			{
				return false;
			}
			ContextFields.Add(TEXT("node"), ResolvedNode.ToSharedRef());
			ContextFields.Add(TEXT("alias"), SourceAlias.ToSharedRef());
			return TryCanonicalObjectFromMap(
				ContextFields, Resources, OutContext, OutError);
		};

		auto TryResolvePinDefaults = [&] (
			const FUeAgentCanonicalValue& VisiblePinDefaults,
			const FString& NodeId,
			const FUeAgentCanonicalValue* CapturedNode,
			FUeAgentCanonicalValuePtr& OutPinDefaults)
		{
			if (VisiblePinDefaults.GetKind() != EUeAgentValueKind::Object)
			{
				return Fail(OutError, TEXT("semantic_workspace_graph_pin_defaults_invalid"),
					TEXT("pin_defaults must be an object keyed by pin alias."),
					SourceUnit,
					TEXT("/pin_defaults"),
					PublicScalar(VisiblePinDefaults));
			}
			const TMap<FString, FGraphAuthoringPinAliasInput>* PinsByAlias =
				PinsByAliasByNodeId.Find(NodeId);
			TMap<FString, FString> ExistingKeyByAlias;
			TMap<FString, FUeAgentCanonicalValueRef> ExistingDefaultByAlias;
			if (CapturedNode != nullptr)
			{
				const FUeAgentCanonicalValueRef* CapturedPinDefaults =
					FindCanonicalField(*CapturedNode, TEXT("pin_defaults"));
				const TMap<FString, FString>* AliasesBySelector =
					PinAliasesByNodeId.Find(NodeId);
				if (CapturedPinDefaults != nullptr
					&& (*CapturedPinDefaults)->GetKind() == EUeAgentValueKind::Object
					&& AliasesBySelector != nullptr)
				{
					for (const auto& CapturedDefault :
						(*CapturedPinDefaults)->GetObjectItems())
					{
						FString PinName;
						int32 PinOccurrence = 0;
						if (CapturedDefault.Value->GetKind() != EUeAgentValueKind::Object
							|| !TryReadCanonicalString(
								CapturedDefault.Value.Get(), TEXT("pin_name"), PinName)
							|| !TryReadCanonicalPinOccurrence(
								CapturedDefault.Value.Get(), PinOccurrence))
						{
							continue;
						}
						const FString Alias = AliasesBySelector->FindRef(
							MakePinSelectorKey(
								PinName, PinOccurrence,
								EGraphAuthoringEndpointDirection::Input));
						if (!Alias.IsEmpty())
						{
							ExistingKeyByAlias.Add(Alias, CapturedDefault.Key);
							ExistingDefaultByAlias.Add(Alias, CapturedDefault.Value);
						}
					}
				}
			}
			TMap<FString, FUeAgentCanonicalValueRef> ResolvedPinDefaults;
			for (const auto& PinDefault : VisiblePinDefaults.GetObjectItems())
			{
				if (PinDefault.Value->GetKind() != EUeAgentValueKind::Object)
				{
					return Fail(OutError, TEXT("semantic_workspace_graph_pin_default_invalid"),
						FString::Printf(TEXT("Default for pin '%s' must be an object."), *PinDefault.Key),
						SourceUnit,
						TEXT("/pin_defaults/") + EscapeJsonPointerSegment(PinDefault.Key));
				}
				FString PinName = PinDefault.Key;
				int32 PinOccurrence = 0;
				if (PinsByAlias != nullptr)
				{
					if (const FGraphAuthoringPinAliasInput* Pin =
						PinsByAlias->Find(PinDefault.Key))
					{
						if (Pin->Direction != EGraphAuthoringEndpointDirection::Input)
						{
							return Fail(OutError, TEXT("semantic_workspace_graph_pin_default_direction_invalid"),
								TEXT("A default value can only be authored for an input pin."),
								SourceUnit,
								TEXT("/pin_defaults/") + EscapeJsonPointerSegment(PinDefault.Key),
								MakeShared<FJsonValueString>(TEXT("output")),
								MakeShared<FJsonValueString>(TEXT("input")));
						}
						PinName = Pin->SemanticName;
						PinOccurrence = Pin->Occurrence;
					}
				}

				TMap<FString, FUeAgentCanonicalValueRef> PinFields;
				if (const FUeAgentCanonicalValueRef* ExistingDefault =
					ExistingDefaultByAlias.Find(PinDefault.Key))
				{
					PinFields = CanonicalObjectMap((*ExistingDefault).Get());
				}
				for (const auto& PinField : PinDefault.Value->GetObjectItems())
				{
					if (IsMachineField(PinField.Key))
					{
						return Fail(OutError, TEXT("semantic_workspace_graph_machine_field_forbidden"),
							FString::Printf(TEXT("Pin default field '%s' is maintained by UAI and cannot be authored."), *PinField.Key),
							SourceUnit,
							TEXT("/pin_defaults/") + EscapeJsonPointerSegment(PinDefault.Key) + TEXT("/") + EscapeJsonPointerSegment(PinField.Key));
					}
					PinFields.Add(PinField.Key, PinField.Value);
				}
				FUeAgentCanonicalValuePtr PinNameValue;
				FUeAgentCanonicalValuePtr PinOccurrenceValue;
				if (!TryCanonicalStringValue(
						PinName, Resources, PinNameValue, OutError)
					|| !TryCanonicalIntegerValue(
						PinOccurrence, Resources, PinOccurrenceValue, OutError))
				{
					return false;
				}
				PinFields.Add(TEXT("pin_name"), PinNameValue.ToSharedRef());
				PinFields.Add(
					TEXT("pin_occurrence"), PinOccurrenceValue.ToSharedRef());
				FUeAgentCanonicalValuePtr ResolvedPinDefault;
				if (!TryCanonicalObjectFromMap(
						PinFields, Resources, ResolvedPinDefault, OutError))
				{
					return false;
				}
				const FString* ExistingKey =
					ExistingKeyByAlias.Find(PinDefault.Key);
				const FString StableKey = ExistingKey != nullptr
					? *ExistingKey
					: TEXT("pin:") + PinName + TEXT(":")
						+ FString::FromInt(PinOccurrence);
				ResolvedPinDefaults.Add(
					StableKey, ResolvedPinDefault.ToSharedRef());
			}
			return TryCanonicalObjectFromMap(
				ResolvedPinDefaults, Resources, OutPinDefaults, OutError);
		};

		for (const auto& Captured : CapturedNodes)
		{
			const auto ResolveCapturedNode = [&]() -> bool {
			if (Captured.Value->GetKind() != EUeAgentValueKind::Object)
			{
				return Fail(OutError, TEXT("semantic_workspace_graph_node_invalid"),
					FString::Printf(TEXT("Captured node '%s' must be an object."), *Captured.Key),
					SourceUnit,
					TEXT("/"),
					nullptr,
					nullptr,
					true);
			}
			FString NodeId;
			if (!TryReadCanonicalString(Captured.Value.Get(), TEXT("id"), NodeId)
				|| NodeId.IsEmpty())
			{
				return Fail(OutError, TEXT("semantic_workspace_graph_node_identity_missing"),
					FString::Printf(TEXT("Captured node '%s' has no non-empty native identity."), *Captured.Key),
					SourceUnit,
					TEXT("/"),
					nullptr,
					nullptr,
					true);
			}
			auto Fields = CanonicalObjectMap(Captured.Value.Get());
			if (!TryNormalizeGraphAuthoringNodePosition(
					Fields, FString(), Resources, OutError))
			{
				return ForwardCapturedFailure(OutError, TEXT("/nodes/") + EscapeJsonPointerSegment(Captured.Key));
			}
			if (const FUeAgentCanonicalValueRef* Visible = VisibleNodes.Find(Captured.Key))
			{
				if ((*Visible)->GetKind() != EUeAgentValueKind::Object)
				{
					return Fail(OutError, TEXT("semantic_workspace_graph_node_invalid"), FString::Printf(TEXT("Node '%s' must be an object."), *Captured.Key), SourceUnit, TEXT("/"));
				}
				auto VisibleNodeFields = CanonicalObjectMap((*Visible).Get());
				VisibleNodeFields.Remove(TEXT("properties_ref"));
				if (!TryNormalizeGraphAuthoringNodePosition(
						VisibleNodeFields, SourceUnit, Resources, OutError))
				{
					return false;
				}
				for (const auto& Field : VisibleNodeFields)
				{
					if (IsMachineField(Field.Key))
					{
						return Fail(OutError, TEXT("semantic_workspace_graph_machine_field_forbidden"),
							FString::Printf(TEXT("Node field '%s' is maintained by UAI and cannot be authored."), *Field.Key),
							SourceUnit,
							TEXT("/") + EscapeJsonPointerSegment(Field.Key));
					}
					if (Field.Key == TEXT("candidate_context"))
					{
						FUeAgentCanonicalValuePtr ResolvedContext;
						if (!TryResolveCandidateContext(
								Field.Value.Get(), ResolvedContext))
						{
							return false;
						}
						Fields.Add(Field.Key, ResolvedContext.ToSharedRef());
					}
					else if (Field.Key == TEXT("pin_defaults"))
					{
						FUeAgentCanonicalValuePtr ResolvedPinDefaults;
						if (!TryResolvePinDefaults(
								Field.Value.Get(), NodeId, &Captured.Value.Get(),
								ResolvedPinDefaults))
						{
							return false;
						}
						Fields.Add(Field.Key, ResolvedPinDefaults.ToSharedRef());
					}
					else
					{
						Fields.Add(Field.Key, Field.Value);
					}
				}
				Fields.Add(TEXT("operation"), UpdateOperation.ToSharedRef());
			}
			else
			{
				Fields.Add(TEXT("operation"), RemoveOperation.ToSharedRef());
			}
			FUeAgentCanonicalValuePtr Resolved;
			if (!TryCanonicalObjectFromMap(Fields, Resources, Resolved, OutError))
			{
				return false;
			}
			ResolvedNodes.Add(Captured.Key, Resolved.ToSharedRef());
			return true;
			};
			if (!ResolveCapturedNode()) CollectNodeError(Captured.Key);
		}

		for (const auto& Visible : VisibleNodes)
		{
			if (CapturedNodes.Contains(Visible.Key))
			{
				continue;
			}
			const auto ResolveNewNode = [&]() -> bool {
			if (Visible.Value->GetKind() != EUeAgentValueKind::Object)
			{
				return Fail(OutError, TEXT("semantic_workspace_graph_node_invalid"), FString::Printf(TEXT("Node '%s' must be an object."), *Visible.Key), SourceUnit, TEXT("/"));
			}
			auto Fields = CanonicalObjectMap(Visible.Value.Get());
			Fields.Remove(TEXT("properties_ref"));
			if (!TryNormalizeGraphAuthoringNodePosition(
					Fields, SourceUnit, Resources, OutError))
			{
				return false;
			}
			for (const auto& Field : Fields)
			{
				if (IsMachineField(Field.Key))
				{
					return Fail(OutError, TEXT("semantic_workspace_graph_machine_field_forbidden"),
						FString::Printf(TEXT("Node field '%s' is maintained by UAI and cannot be authored."), *Field.Key),
						SourceUnit,
						TEXT("/") + EscapeJsonPointerSegment(Field.Key));
				}
				if (Field.Key == TEXT("candidate"))
				{
					if (Field.Value->GetKind() != EUeAgentValueKind::Object)
					{
						return Fail(OutError, TEXT("semantic_workspace_graph_candidate_invalid"),
							TEXT("Node candidate must be an object."),
							SourceUnit,
							TEXT("/candidate"),
							PublicScalar(Field.Value.Get()));
					}
					for (const auto& CandidateField : Field.Value->GetObjectItems())
					{
						if (IsMachineField(CandidateField.Key))
						{
							return Fail(OutError, TEXT("semantic_workspace_graph_machine_field_forbidden"),
								FString::Printf(TEXT("Candidate field '%s' is maintained by UAI and cannot be authored."), *CandidateField.Key),
								SourceUnit,
								TEXT("/candidate/") + EscapeJsonPointerSegment(CandidateField.Key));
						}
					}
				}
			}
			if (const FUeAgentCanonicalValueRef* VisiblePinDefaults =
				Fields.Find(TEXT("pin_defaults")))
			{
				FUeAgentCanonicalValuePtr ResolvedPinDefaults;
				if (!TryResolvePinDefaults(
						(*VisiblePinDefaults).Get(), Visible.Key, nullptr,
						ResolvedPinDefaults))
				{
					return false;
				}
				Fields.Add(
					TEXT("pin_defaults"), ResolvedPinDefaults.ToSharedRef());
			}
			if (const FUeAgentCanonicalValueRef* CandidateContext =
				Fields.Find(TEXT("candidate_context")))
			{
				FUeAgentCanonicalValuePtr ResolvedContext;
				if (!TryResolveCandidateContext(
						(*CandidateContext).Get(), ResolvedContext))
				{
					return false;
				}
				Fields.Add(TEXT("candidate_context"), ResolvedContext.ToSharedRef());
			}
			// Intrinsic roles are creation selectors owned by the graph domain,
			// just like node_type. Do not fabricate a candidate for a root row;
			// the native owner still validates which roles it can create. Preserve
			// the existing name-to-candidate conversion when a name is supplied.
			const FUeAgentCanonicalValueRef* Role = Fields.Find(TEXT("role"));
			const bool bUsesIntrinsicRole = Role && (*Role)->GetKind() == EUeAgentValueKind::String
				&& !(*Role)->GetText().IsEmpty() && !Fields.Contains(TEXT("name"));
			if (!Fields.Contains(TEXT("candidate")) && !Fields.Contains(TEXT("node_type")) && !bUsesIntrinsicRole)
			{
				const FUeAgentCanonicalValueRef* Candidate = Fields.Find(TEXT("name"));
				if (Candidate == nullptr
					|| (*Candidate)->GetKind() != EUeAgentValueKind::String)
				{
					return Fail(OutError, TEXT("semantic_workspace_graph_candidate_missing"),
						TEXT("A new node requires candidate, node_type, an intrinsic role, or a string name."),
						SourceUnit,
						TEXT("/name"),
						Candidate ? PublicScalar(Candidate->Get()) : nullptr);
				}
				TMap<FString, FUeAgentCanonicalValueRef> CandidateFields;
				CandidateFields.Add(TEXT("name"), *Candidate);
				if (const FUeAgentCanonicalValueRef* Selection = Fields.Find(TEXT("select")))
				{
					CandidateFields.Add(TEXT("select"), *Selection);
				}
				FUeAgentCanonicalValuePtr CandidateObject;
				if (!TryCanonicalObjectFromMap(
					CandidateFields, Resources, CandidateObject, OutError))
				{
					return false;
				}
				Fields.Remove(TEXT("name"));
				Fields.Remove(TEXT("select"));
				Fields.Add(TEXT("candidate"), CandidateObject.ToSharedRef());
			}
			FUeAgentCanonicalValuePtr LocalId;
			if (!TryCanonicalStringValue(Visible.Key, Resources, LocalId, OutError))
			{
				return false;
			}
			Fields.Add(TEXT("id"), LocalId.ToSharedRef());
			Fields.Add(TEXT("operation"), CreateOperation.ToSharedRef());
			FUeAgentCanonicalValuePtr Resolved;
			if (!TryCanonicalObjectFromMap(Fields, Resources, Resolved, OutError))
			{
				return false;
			}
			ResolvedNodes.Add(Visible.Key, Resolved.ToSharedRef());
			return true;
			};
			if (!ResolveNewNode()) CollectNodeError(Visible.Key);
		}
		if (NodeErrors.HasErrors())
		{
			NodeErrors.MarkUnchecked(TEXT("graph_references"), SourceUnit, TEXT("invalid_nodes"));
			OutError = MoveTemp(NodeErrors);
			return false;
		}

		// Parent denotes ownership in the common graph contract. Unedited owned
		// rows leave with their deleted owner; an edited child still requires a
		// surviving owner and is never silently discarded.
		TArray<FString> RemovedOwners, RemovedAliases;
		TArray<FOwnedItemRemovalCandidate> OwnedCandidates;
		for (const auto& Captured : CapturedNodes)
		{
			if (!VisibleNodes.Contains(Captured.Key)) { RemovedOwners.Add(Captured.Key); continue; }
			FString BeforeParent;
			if (TryReadCanonicalString(Captured.Value.Get(), TEXT("parent"), BeforeParent))
				if (const FString* BeforeAlias = AliasByNodeId.Find(BeforeParent))
					OwnedCandidates.Add({Captured.Key, *BeforeAlias});
		}
		if (!TryCollectOwnedItemRemovals(OwnedCandidates, RemovedOwners,
			[&](const FString& Alias, bool& bUnchanged) {
				auto BeforeFields = CanonicalObjectMap(CapturedNodes.FindChecked(Alias).Get());
				auto AfterFields = CanonicalObjectMap(ResolvedNodes.FindChecked(Alias).Get());
				BeforeFields.Remove(TEXT("operation"));
				AfterFields.Remove(TEXT("operation"));
				if (!TryNormalizeGraphAuthoringNodePosition(BeforeFields, FString(), Resources, OutError))
					return ForwardCapturedFailure(OutError, TEXT("/nodes/") + EscapeJsonPointerSegment(Alias));
				FString Parent;
				if (const auto* Value = AfterFields.Find(TEXT("parent")); Value && (*Value)->GetKind() == EUeAgentValueKind::String)
				{
					Parent = (*Value)->GetText();
					if (const FString* Id = NodeIdByAlias.Find(Parent))
					{
						FUeAgentCanonicalValuePtr ResolvedParent;
						if (!TryCanonicalStringValue(*Id, Resources, ResolvedParent, OutError)) return false;
						AfterFields.Add(TEXT("parent"), ResolvedParent.ToSharedRef());
					}
				}
				FUeAgentCanonicalValuePtr BeforeValue, AfterValue;
				if (!TryCanonicalObjectFromMap(BeforeFields, Resources, BeforeValue, OutError)
					|| !TryCanonicalObjectFromMap(AfterFields, Resources, AfterValue, OutError)) return false;
				FUeAgentSemanticComparison Comparison;
				FUeAgentStateSemanticError Error;
				if (!TryCompareValues(BeforeValue.ToSharedRef(), AfterValue.ToSharedRef(), TEXT("/"), Resources, Comparison, Error))
					{ OutError.SetFailure(Error, SourceUnit); return false; }
				bUnchanged = Comparison.IsEquivalent();
				return true;
			}, RemovedAliases)) return false;
		for (const FString& Alias : RemovedAliases)
		{
			auto Fields = CanonicalObjectMap(CapturedNodes.FindChecked(Alias).Get());
			Fields.Add(TEXT("operation"), RemoveOperation.ToSharedRef());
			FUeAgentCanonicalValuePtr Removed;
			if (!TryCanonicalObjectFromMap(Fields, Resources, Removed, OutError)) return false;
			ResolvedNodes.Add(Alias, Removed.ToSharedRef());
			VisibleNodes.Remove(Alias);
		}
		// Ownership references use the same frozen graph-local identities as connections.
		for (const auto& Visible : VisibleNodes)
		{
			const auto* Parent = FindCanonicalField(Visible.Value.Get(), TEXT("parent"));
			if (!Parent) continue;
			if ((*Parent)->GetKind() != EUeAgentValueKind::String
				|| !VisibleNodes.Contains((*Parent)->GetText()) || (*Parent)->GetText() == Visible.Key)
			{
				Fail(OutError, TEXT("semantic_workspace_graph_parent_unresolved"),
					(*Parent)->GetKind() != EUeAgentValueKind::String ? TEXT("Node parent must be a node alias string.") : (*Parent)->GetText() == Visible.Key ? TEXT("A node cannot be its own parent.") : TEXT("Node parent does not name a surviving node in this graph."),
					SourceUnit,
					TEXT("/parent"),
					PublicScalar(Parent->Get()),
					nullptr);
				CollectNodeError(Visible.Key);
				continue;
			}
			auto Fields = CanonicalObjectMap(ResolvedNodes.FindChecked(Visible.Key).Get());
			FUeAgentCanonicalValuePtr ParentId;
			if (!TryCanonicalStringValue(NodeIdByAlias.FindChecked((*Parent)->GetText()), Resources, ParentId, OutError)) return false;
			Fields.Add(TEXT("parent"), ParentId.ToSharedRef());
			FUeAgentCanonicalValuePtr Resolved;
			if (!TryCanonicalObjectFromMap(Fields, Resources, Resolved, OutError)) return false;
			ResolvedNodes.Add(Visible.Key, Resolved.ToSharedRef());
		}
		if (NodeErrors.HasErrors())
		{
			NodeErrors.MarkUnchecked(TEXT("graph_references"), SourceUnit, TEXT("invalid_parents"));
			OutError = MoveTemp(NodeErrors);
			return false;
		}
		FUeAgentCanonicalValuePtr ResolvedNodeMap;
		if (!TryCanonicalObjectFromMap(ResolvedNodes, Resources, ResolvedNodeMap, OutError))
		{
			return false;
		}
		OutValues.Add(TEXT("nodes"), ResolvedNodeMap.ToSharedRef());

		// Graph-level slots share the same frozen identities as node parents and connections.
		const auto* CapturedGraph = FindCanonicalField(CapturedRoot, TEXT("graph"));
		const auto* VisibleGraph = FindCanonicalField(VisibleRoot, TEXT("graph"));
		const auto* CapturedReferences = CapturedGraph
			? FindCanonicalField(CapturedGraph->Get(), TEXT("node_references")) : nullptr;
		const auto* VisibleReferences = VisibleGraph
			? FindCanonicalField(VisibleGraph->Get(), TEXT("node_references")) : nullptr;
		if (CapturedReferences || VisibleReferences)
		{
			if (!CapturedReferences || (*CapturedReferences)->GetKind() != EUeAgentValueKind::Object
				|| (VisibleReferences && (*VisibleReferences)->GetKind() != EUeAgentValueKind::Object))
				return Fail(OutError, TEXT("semantic_workspace_graph_node_references_invalid"), VisibleReferences && (*VisibleReferences)->GetKind() != EUeAgentValueKind::Object ? TEXT("graph.node_references must be an object.") : TEXT("Captured graph node_references is missing or is not an object."), SourceUnit, TEXT("/graph/node_references"), nullptr, nullptr,
					!VisibleReferences || (*VisibleReferences)->GetKind() == EUeAgentValueKind::Object);
			auto References = CanonicalObjectMap(CapturedReferences->Get());
			if (VisibleReferences)
			{
				for (const auto& Reference : (*VisibleReferences)->GetObjectItems())
				{
					if (!References.Contains(Reference.Key)
						|| Reference.Value->GetKind() != EUeAgentValueKind::String)
						return Fail(OutError, TEXT("semantic_workspace_graph_node_reference_invalid"),
							!References.Contains(Reference.Key) ? TEXT("This graph has no declared node reference with this name.") : TEXT("Graph node reference must be a node alias string."),
							SourceUnit,
							TEXT("/graph/node_references/") + EscapeJsonPointerSegment(Reference.Key));
					const FString& Alias = Reference.Value->GetText();
					const FString BeforeId = References.FindChecked(Reference.Key)->GetText();
					const FString* BeforeAlias = AliasByNodeId.Find(BeforeId);
					const bool bRemovedContext = BeforeAlias && *BeforeAlias == Alias && !VisibleNodes.Contains(Alias);
					if (!Alias.IsEmpty() && !VisibleNodes.Contains(Alias) && !bRemovedContext)
						return Fail(OutError, TEXT("semantic_workspace_graph_node_reference_unresolved"),
							TEXT("Graph node reference does not name a surviving node."),
							SourceUnit,
							TEXT("/graph/node_references/") + EscapeJsonPointerSegment(Reference.Key),
							MakeShared<FJsonValueString>(Alias),
							nullptr);
					FUeAgentCanonicalValuePtr NodeId;
					if (!TryCanonicalStringValue(Alias.IsEmpty() || bRemovedContext ? FString() : NodeIdByAlias.FindChecked(Alias),
						Resources, NodeId, OutError)) return false;
					References.Add(Reference.Key, NodeId.ToSharedRef());
				}
			}
			for (auto& Reference : References)
			{
				if (Reference.Value->GetKind() != EUeAgentValueKind::String)
					return Fail(OutError, TEXT("semantic_workspace_graph_node_reference_invalid"),
						TEXT("Captured graph node reference must be a string."),
						SourceUnit,
						TEXT("/graph/node_references/") + EscapeJsonPointerSegment(Reference.Key),
						nullptr,
						nullptr,
						true);
				const FString& NodeId = Reference.Value->GetText();
				const FString* Alias = AliasByNodeId.Find(NodeId);
				if (!NodeId.IsEmpty() && !Alias)
					return Fail(OutError, TEXT("semantic_workspace_graph_node_reference_unresolved"),
						TEXT("Captured graph node reference does not resolve to a node."),
						SourceUnit,
						TEXT("/graph/node_references/") + EscapeJsonPointerSegment(Reference.Key),
						nullptr,
						nullptr,
						true);
				if (Alias && !VisibleNodes.Contains(*Alias))
				{
					FUeAgentCanonicalValuePtr Cleared;
					if (!TryCanonicalStringValue(FString(), Resources, Cleared, OutError)) return false;
					Reference.Value = Cleared.ToSharedRef();
				}
			}
			FUeAgentCanonicalValuePtr ResolvedReferences;
			if (!TryCanonicalObjectFromMap(References, Resources, ResolvedReferences, OutError)) return false;
			auto GraphFields = CanonicalObjectMap(OutValues.FindChecked(TEXT("graph")).Get());
			GraphFields.Add(TEXT("node_references"), ResolvedReferences.ToSharedRef());
			FUeAgentCanonicalValuePtr ResolvedGraph;
			if (!TryCanonicalObjectFromMap(GraphFields, Resources, ResolvedGraph, OutError)) return false;
			OutValues.Add(TEXT("graph"), ResolvedGraph.ToSharedRef());
		}

		auto ReadEdgeEndpoint = [&](const FUeAgentCanonicalValue& Edge, const FString& EdgeKey,
			const FString& Field, FString& OutNode, FString& OutPin, int32& OutPinOccurrence)
		{
			const FString Path = TEXT("/connections/") + Field;
			const auto* Endpoint = FindCanonicalField(Edge, Field);
			if (!Endpoint || (*Endpoint)->GetKind() != EUeAgentValueKind::Object)
				return Fail(OutError, TEXT("semantic_workspace_graph_edge_invalid"),
					TEXT("Captured edge endpoint must be an object with node and pin selectors."), EdgeKey, Path, nullptr, nullptr, true);
			if (!TryReadCanonicalString(Endpoint->Get(), TEXT("node"), OutNode))
				return Fail(OutError, TEXT("semantic_workspace_graph_edge_invalid"),
					TEXT("Captured endpoint node selector is missing or is not a string."), EdgeKey, Path + TEXT("/node"), nullptr, nullptr, true);
			if (!TryReadCanonicalString(Endpoint->Get(), TEXT("pin"), OutPin))
				return Fail(OutError, TEXT("semantic_workspace_graph_edge_invalid"),
					TEXT("Captured endpoint pin selector is missing or is not a string."), EdgeKey, Path + TEXT("/pin"), nullptr, nullptr, true);
			if (!TryReadCanonicalPinOccurrence(Endpoint->Get(), OutPinOccurrence))
			{
				const auto* Occurrence = FindCanonicalField(Endpoint->Get(), TEXT("occurrence"));
				const auto* PinOccurrence = FindCanonicalField(Endpoint->Get(), TEXT("pin_occurrence"));
				const auto* Value = PinOccurrence ? PinOccurrence : Occurrence;
				return Fail(OutError, TEXT("semantic_workspace_graph_edge_invalid"),
					Occurrence && PinOccurrence ? TEXT("Specify only one of occurrence and pin_occurrence.")
					: TEXT("Pin occurrence must be an integer between 0 and 2147483647."), EdgeKey,
					Path + (PinOccurrence ? TEXT("/pin_occurrence") : TEXT("/occurrence")),
					Value ? PublicScalar(Value->Get()) : nullptr, nullptr, true);
			}
			return true;
		};
		auto EdgeSignature = [&](const FUeAgentCanonicalValue& Edge, const FString& EdgeKey, FString& Signature)
		{
			FString ReadableFrom;
			FString ReadableTo;
			if (TryReadCanonicalString(Edge, TEXT("from"), ReadableFrom)
				&& TryReadCanonicalString(Edge, TEXT("to"), ReadableTo))
			{
				Signature = ReadableFrom + TEXT("\n") + ReadableTo;
				return true;
			}
			FString FromNode, FromPin, ToNode, ToPin;
			int32 FromPinOccurrence = 0;
			int32 ToPinOccurrence = 0;
			if (!ReadEdgeEndpoint(
					Edge, EdgeKey, TEXT("from"), FromNode, FromPin, FromPinOccurrence)
				|| !ReadEdgeEndpoint(
					Edge, EdgeKey, TEXT("to"), ToNode, ToPin, ToPinOccurrence))
			{
				return false;
			}
			const FString* FromAlias = AliasByNodeId.Find(FromNode);
			const FString* ToAlias = AliasByNodeId.Find(ToNode);
			FString FromPublicPin;
			FString ToPublicPin;
			if (const TMap<FString, FString>* PinAliases =
				PinAliasesByNodeId.Find(FromNode))
			{
				FromPublicPin = PinAliases->FindRef(MakePinSelectorKey(
					FromPin, FromPinOccurrence,
					EGraphAuthoringEndpointDirection::Output));
			}
			if (const TMap<FString, FString>* PinAliases =
				PinAliasesByNodeId.Find(ToNode))
			{
				ToPublicPin = PinAliases->FindRef(MakePinSelectorKey(
					ToPin, ToPinOccurrence,
					EGraphAuthoringEndpointDirection::Input));
			}
			FString FromEndpoint, ToEndpoint;
			if (!FromAlias || !ToAlias)
				return Fail(OutError, TEXT("semantic_workspace_graph_edge_node_unresolved"),
					!FromAlias ? TEXT("Captured edge source node does not resolve to a node alias.")
					: TEXT("Captured edge destination node does not resolve to a node alias."), EdgeKey,
					!FromAlias ? TEXT("/connections/from/node") : TEXT("/connections/to/node"), nullptr, nullptr, true);
			if (!TryFormatGraphAuthoringEndpoint(
					*FromAlias,
					FromPublicPin.IsEmpty() ? FromPin : FromPublicPin,
					FromPublicPin.IsEmpty() ? FromPinOccurrence : 0,
					EGraphAuthoringEndpointDirection::Output, FromEndpoint, OutError))
				return ForwardCapturedFailure(OutError, TEXT("/connections/from"));
			if (!TryFormatGraphAuthoringEndpoint(
					*ToAlias,
					ToPublicPin.IsEmpty() ? ToPin : ToPublicPin,
					ToPublicPin.IsEmpty() ? ToPinOccurrence : 0,
					EGraphAuthoringEndpointDirection::Input, ToEndpoint, OutError))
			{
				return ForwardCapturedFailure(OutError, TEXT("/connections/to"));
			}
			Signature = FromEndpoint + TEXT("\n") + ToEndpoint;
			return true;
		};

		const auto CapturedEdges = CanonicalObjectMap((*CapturedEdgesValue).Get());
		TSet<FString> CapturedConnectionSignatures;
		for (const auto& Captured : CapturedEdges)
		{
			FString Signature;
			if (!EdgeSignature(Captured.Value.Get(), Captured.Key, Signature))
				return false;
			CapturedConnectionSignatures.Add(MoveTemp(Signature));
		}
		TMap<FString, FUeAgentCanonicalValueRef> DesiredConnections;
		TMap<FString, int32> ConnectionIndexBySignature;
		const TArray<FUeAgentCanonicalValueRef> NoConnections;
		const TArray<FUeAgentCanonicalValueRef>& VisibleConnections =
			VisibleConnectionsValue ? (*VisibleConnectionsValue)->GetArrayItems() : NoConnections;
		FWorkspaceError ConnectionErrors;
		for (int32 ConnectionIndex = 0; ConnectionIndex < VisibleConnections.Num();
			++ConnectionIndex)
		{
			const FUeAgentCanonicalValueRef& Connection =
				VisibleConnections[ConnectionIndex];
			auto ResolveConnection = [&]() -> bool
			{
			FString From, To, FromAlias, FromPin, ToAlias, ToPin;
			if (Connection->GetKind() != EUeAgentValueKind::Object)
			{
				return FailGraphConnection(
					OutError, SourceUnit, ConnectionIndex, TEXT(""),
					TEXT("Connection must be an object."), PublicScalar(Connection.Get()));
			}
			if (!TryReadCanonicalString(Connection.Get(), TEXT("from"), From))
			{
				return FailGraphConnection(
					OutError, SourceUnit, ConnectionIndex, TEXT("from"),
					TEXT("from endpoint is required and must be a string."), PublicScalarField(Connection.Get(), TEXT("from")));
			}
			if (!TryReadCanonicalString(Connection.Get(), TEXT("to"), To))
			{
				return FailGraphConnection(
					OutError, SourceUnit, ConnectionIndex, TEXT("to"),
					TEXT("to endpoint is required and must be a string."), PublicScalarField(Connection.Get(), TEXT("to")));
			}
			if (!TrySplitGraphEndpoint(From, FromAlias, FromPin))
			{
				return FailGraphConnection(
					OutError, SourceUnit, ConnectionIndex, TEXT("from"),
					TEXT("from endpoint must use alias.pin format."), MakeShared<FJsonValueString>(From));
			}
			if (!TrySplitGraphEndpoint(To, ToAlias, ToPin))
			{
				return FailGraphConnection(
					OutError, SourceUnit, ConnectionIndex, TEXT("to"),
					TEXT("to endpoint must use alias.pin format."), MakeShared<FJsonValueString>(To));
			}
			// The visible connection array also contains unchanged context. Native
			// node deletion removes those incident edges; only a newly authored
			// dangling edge is an invalid request.
			const FString Signature = From + TEXT("\n") + To;
			if ((!VisibleNodes.Contains(FromAlias) || !VisibleNodes.Contains(ToAlias))
				&& CapturedConnectionSignatures.Contains(Signature)
				&& Connection->GetObjectItems().Num() == 2)
				return true;
			if (!VisibleNodes.Contains(FromAlias))
			{
				return FailGraphConnection(
					OutError, SourceUnit, ConnectionIndex, TEXT("from"),
					TEXT("from alias is not present in visible nodes."), MakeShared<FJsonValueString>(FromAlias));
			}
			if (!VisibleNodes.Contains(ToAlias))
			{
				return FailGraphConnection(
					OutError, SourceUnit, ConnectionIndex, TEXT("to"),
					TEXT("to alias is not present in visible nodes."), MakeShared<FJsonValueString>(ToAlias));
			}
			for (const auto& Field : Connection->GetObjectItems())
			{
				if (IsMachineField(Field.Key))
				{
					return Fail(OutError, TEXT("semantic_workspace_graph_machine_field_forbidden"),
						FString::Printf(TEXT("Connection field '%s' is maintained by UAI and cannot be authored."), *Field.Key),
						SourceUnit,
						FString::Printf(TEXT("/connections/%d/"), ConnectionIndex) + EscapeJsonPointerSegment(Field.Key));
				}
			}
			if (DesiredConnections.Contains(Signature))
			{
				return FailGraphConnection(OutError, SourceUnit, ConnectionIndex, TEXT(""),
					TEXT("The graph contains the same connection more than once."), MakeShared<FJsonValueString>(From + TEXT(" -> ") + To),
					TEXT("semantic_workspace_graph_connection_duplicate"));
			}
			DesiredConnections.Add(Signature, Connection);
			if (SourceRoot && OutSourceRoot)
				ConnectionIndexBySignature.Add(Signature, ConnectionIndex);
			return true;
			};
			if (!ResolveConnection())
			{
				if (SourceRoot) OutError.Locate(*SourceRoot);
				ConnectionErrors.Append(OutError);
				OutError.Reset();
			}
		}
		if (ConnectionErrors.HasErrors())
		{
			ConnectionErrors.MarkUnchecked(TEXT("graph_connection_resolution"), SourceUnit, TEXT("invalid_connections"));
			OutError = MoveTemp(ConnectionErrors);
			return false;
		}

		// Keep the original parser nodes under the normalized semantic keys. This
		// is a diagnostic view only; neither the authored document nor its values
		// acquire machine fields or a second persistent source map.
		TArray<TPair<FString, FHjsonNodeRef>> EdgeSources;
		auto RecordEdgeSource = [&](const FString& Key, const FString& Signature)
		{
			if (!SourceRoot || !OutSourceRoot) return;
			const FHjsonNode* Connections = SourceRoot->Get(TEXT("connections"));
			const int32* Index = ConnectionIndexBySignature.Find(Signature);
			if (Connections && Connections->GetKind() == EHjsonNodeKind::Array
				&& Index && Connections->GetItems().IsValidIndex(*Index))
			{
				EdgeSources.Emplace(Key, Connections->GetItems()[*Index]);
			}
			else
			{
				// A removed connection has no remaining item to point at. Attribute
				// its deletion to the edited collection, never a fabricated item.
				EdgeSources.Emplace(Key, MakeShared<FHjsonNode>(Connections ? *Connections : *SourceRoot));
			}
		};

		TMap<FString, FUeAgentCanonicalValueRef> ResolvedEdges;
		for (const auto& Captured : CapturedEdges)
		{
			if (Captured.Value->GetKind() != EUeAgentValueKind::Object)
			{
				return Fail(OutError, TEXT("semantic_workspace_graph_edge_invalid"),
					TEXT("Captured edge must be an object."),
					Captured.Key,
					TEXT("/connections"),
					nullptr,
					nullptr,
					true);
			}
			FString Signature;
			if (!EdgeSignature(Captured.Value.Get(), Captured.Key, Signature))
			{
				return false;
			}
			auto Fields = CanonicalObjectMap(Captured.Value.Get());
			bool bRetainConnection = DesiredConnections.Remove(Signature) > 0;
			if (!VisibleConnectionsValue)
			{
				FString From, To, FromAlias, FromPin, ToAlias, ToPin;
				if (!Signature.Split(TEXT("\n"), &From, &To)
					|| !TrySplitGraphEndpoint(From, FromAlias, FromPin)
					|| !TrySplitGraphEndpoint(To, ToAlias, ToPin))
					return Fail(OutError, TEXT("semantic_workspace_graph_edge_invalid"),
						TEXT("Captured connection endpoints must both use NodeAlias.PinAlias format."),
						Captured.Key,
						TEXT("/connections"),
						nullptr,
						nullptr,
						true);
				// Removing a node also removes its incident edges; other omitted
				// connections retain their original identities and fields.
				bRetainConnection = VisibleNodes.Contains(FromAlias) && VisibleNodes.Contains(ToAlias);
			}
			Fields.Add(TEXT("operation"), bRetainConnection
				? UpdateOperation.ToSharedRef() : RemoveOperation.ToSharedRef());
			FUeAgentCanonicalValuePtr Resolved;
			if (!TryCanonicalObjectFromMap(Fields, Resources, Resolved, OutError))
			{
				return false;
			}
			ResolvedEdges.Add(Captured.Key, Resolved.ToSharedRef());
			RecordEdgeSource(Captured.Key, Signature);
		}

		int32 NewConnectionIndex = 1;
		for (const auto& Desired : DesiredConnections)
		{
			FString From, To, FromAlias, FromPin, ToAlias, ToPin;
			if (!TryReadCanonicalString(Desired.Value.Get(), TEXT("from"), From)
				|| !TryReadCanonicalString(Desired.Value.Get(), TEXT("to"), To)
				|| !TrySplitGraphEndpoint(From, FromAlias, FromPin)
				|| !TrySplitGraphEndpoint(To, ToAlias, ToPin))
			{
				return Fail(OutError, TEXT("semantic_workspace_graph_connection_invalid"),
					TEXT("Resolved connection lost a valid from or to endpoint before edge construction."),
					SourceUnit,
					TEXT("/connections"));
			}
			TMap<FString, FUeAgentCanonicalValueRef> FromFields, ToFields;
			FUeAgentCanonicalValuePtr FromNode, FromPinValue, ToNode, ToPinValue;
			if (!TryCanonicalStringValue(NodeIdByAlias.FindChecked(FromAlias),
					Resources, FromNode, OutError)
				|| !TryCanonicalStringValue(FromPin, Resources, FromPinValue, OutError)
				|| !TryCanonicalStringValue(NodeIdByAlias.FindChecked(ToAlias),
					Resources, ToNode, OutError)
				|| !TryCanonicalStringValue(ToPin, Resources, ToPinValue, OutError))
			{
				return false;
			}
			FromFields.Add(TEXT("node"), FromNode.ToSharedRef());
			FromFields.Add(TEXT("pin"), FromPinValue.ToSharedRef());
			ToFields.Add(TEXT("node"), ToNode.ToSharedRef());
			ToFields.Add(TEXT("pin"), ToPinValue.ToSharedRef());
			FUeAgentCanonicalValuePtr FromObject, ToObject;
			if (!TryCanonicalObjectFromMap(FromFields, Resources, FromObject, OutError)
				|| !TryCanonicalObjectFromMap(ToFields, Resources, ToObject, OutError))
			{
				return false;
			}
			auto EdgeFields = CanonicalObjectMap(Desired.Value.Get());
			EdgeFields.Remove(TEXT("from"));
			EdgeFields.Remove(TEXT("to"));
			EdgeFields.Add(TEXT("from"), FromObject.ToSharedRef());
			EdgeFields.Add(TEXT("to"), ToObject.ToSharedRef());
			EdgeFields.Add(TEXT("operation"), CreateOperation.ToSharedRef());
			FUeAgentCanonicalValuePtr EdgeObject;
			if (!TryCanonicalObjectFromMap(EdgeFields, Resources, EdgeObject, OutError))
			{
				return false;
			}
			const FString EdgeKey = FString::Printf(TEXT("connection:%d"), NewConnectionIndex++);
			ResolvedEdges.Add(EdgeKey, EdgeObject.ToSharedRef());
			RecordEdgeSource(EdgeKey, Desired.Key);
		}

		FUeAgentCanonicalValuePtr ResolvedEdgeMap;
		if (!TryCanonicalObjectFromMap(ResolvedEdges, Resources, ResolvedEdgeMap, OutError))
		{
			return false;
		}
		OutValues.Add(TEXT("edges"), ResolvedEdgeMap.ToSharedRef());
		if (SourceRoot && OutSourceRoot)
		{
			const FHjsonNode* Connections = SourceRoot->Get(TEXT("connections"));
			const FHjsonNode* Nodes = SourceRoot->Get(TEXT("nodes"));
			TArray<TPair<FString, FHjsonNodeRef>> NodeSources = Nodes
				? Nodes->GetEntries() : TArray<TPair<FString, FHjsonNodeRef>>{};
			for (const auto& Node : ResolvedNodes)
			{
				if (!Nodes || !Nodes->Get(Node.Key))
					NodeSources.Emplace(Node.Key, MakeShared<FHjsonNode>(Nodes ? *Nodes : *SourceRoot));
			}
			TArray<TPair<FString, FHjsonNodeRef>> Sources = SourceRoot->GetEntries();
			Sources.RemoveAll([](const auto& Entry) { return Entry.Key == TEXT("edges") || Entry.Key == TEXT("nodes"); });
			Sources.Emplace(TEXT("nodes"), MakeShared<FHjsonNode>(EHjsonNodeKind::Object,
				(Nodes ? *Nodes : *SourceRoot).GetProvenance(), FString(), FString(), false,
				TArray<FHjsonNodeRef>{}, MoveTemp(NodeSources)));
			Sources.Emplace(TEXT("edges"), MakeShared<FHjsonNode>(EHjsonNodeKind::Object,
				(Connections ? *Connections : *SourceRoot).GetProvenance(), FString(), FString(), false,
				TArray<FHjsonNodeRef>{}, MoveTemp(EdgeSources)));
			*OutSourceRoot = MakeShared<FHjsonNode>(EHjsonNodeKind::Object,
				SourceRoot->GetProvenance(), FString(), FString(), false,
				TArray<FHjsonNodeRef>{}, MoveTemp(Sources));
		}
		return true;
	}
}

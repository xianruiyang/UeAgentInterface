#include "Domains/Blueprint/Authoring/UeAgentBlueprintSemanticProjection.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/HjsonWorkspace/UeAgentGraphAuthoringProjection.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"

#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringDescriptors.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringOps.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringProjection.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"

namespace UeAgentBlueprintAuthoring
{
	using namespace UE::UAI::HjsonWorkspace;
	using UE::UAI::StateSemantic::CanonicalJson::TryToJsonValue;
	using UE::UAI::StateSemantic::FUeAgentCanonicalValue;

	namespace
	{
		bool Fail(
			FWorkspaceError& OutError,
			const FString& Code,
			const FString& Reason,
			const FString& UnitId = FString(),
			const FString& Pointer = TEXT("/"),
            const TSharedPtr<FJsonValue>& Actual = nullptr,
            const TSharedPtr<FJsonValue>& Expected = nullptr,
            const bool bNativeResult = false)
		{
			auto Detail = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("document_restore"));
            Detail.Source.InternalJsonPath = Pointer;
            Detail.ActualValue = Actual;
            Detail.ExpectedValue = Expected;
            if (Expected.IsValid()) Detail.ActualValuePresent = Actual.IsValid();
            if (bNativeResult) Detail.SourceKind = TEXT("native_result");
            OutError.SetFailure(MoveTemp(Detail), UnitId);
			return false;
		}

		TSharedPtr<FJsonValue> CloneJsonValue(
			const TSharedPtr<FJsonValue>& Value)
		{
			const TSharedPtr<FJsonValue> Cloned =
				UeAgentAuthoringState::CloneCanonicalValue(Value);
			return Cloned.IsValid()
				? Cloned
				: MakeShared<FJsonValueNull>();
		}

		TSharedPtr<FJsonObject> CloneJsonObject(
			const TSharedPtr<FJsonObject>& Source)
		{
			const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			if (Source.IsValid())
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Field :
					Source->Values)
				{
					Result->SetField(Field.Key, CloneJsonValue(Field.Value));
				}
			}
			return Result;
		}

		bool CanonicalToJson(
			const FUeAgentCanonicalValue& Value,
			TSharedPtr<FJsonValue>& OutValue,
			FWorkspaceError& OutError,
			const FString& UnitId,
			const FString& Pointer)
		{
			UE::UAI::Contract::FDiagnostic Error;
			if (!TryToJsonValue(Value, OutValue, Error))
			{
				if (Error.Source.SourceUnit.IsEmpty())
					UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(Error, Pointer);
				OutError.SetFailure(MoveTemp(Error), UnitId, Pointer);
				return false;
			}
			return true;
		}

		bool UnitValuesToObject(
			const FSemanticUnit& Unit,
			TSharedPtr<FJsonObject>& OutObject,
			FWorkspaceError& OutError)
		{
			OutObject = MakeShared<FJsonObject>();
			for (const TPair<FString,
				UE::UAI::StateSemantic::FUeAgentCanonicalValueRef>& Field :
				Unit.GetValues())
			{
				TSharedPtr<FJsonValue> JsonValue;
				// Unit source documents are rooted at their values, without the model's /values envelope.
				if (!CanonicalToJson(
						Field.Value.Get(), JsonValue, OutError,
						Unit.GetUnitId(), TEXT("/") + Field.Key))
				{
					return false;
				}
				OutObject->SetField(Field.Key, JsonValue);
			}
			return true;
		}

		FString ReadableMapKey(
			const FString& Prefix,
			const FString& SourceKey,
			const TSharedPtr<FJsonObject>& Spec,
			const bool bNodes,
			const TSharedRef<FJsonObject>& Existing)
		{
			FString Base;
			for (const TCHAR* Field : {
				TEXT("name"), TEXT("label"), TEXT("title"),
				TEXT("node_name"), TEXT("event_name"), TEXT("function_name")})
			{
				if (Spec->TryGetStringField(Field, Base))
				{
					Base.TrimStartAndEndInline();
					if (!Base.IsEmpty()) break;
				}
			}
			if (Base.IsEmpty() && bNodes)
			{
				const TSharedPtr<FJsonObject>* Candidate = nullptr;
				if (Spec->TryGetObjectField(TEXT("candidate"), Candidate)
					&& Candidate && Candidate->IsValid())
				{
					(*Candidate)->TryGetStringField(TEXT("name"), Base);
					Base.TrimStartAndEndInline();
				}
			}
			if (Base.IsEmpty()
				&& !SourceKey.Contains(TEXT(":"))
				&& !IsOpaqueSemanticIdentity(SourceKey))
			{
				Base = SourceKey;
			}
			Base = MakeReadableSemanticAlias(Base);
			if (Base.IsEmpty())
			{
				Base = Prefix;
			}
			FString Key = Prefix + TEXT(":") + Base;
			int32 Ordinal = 1;
			while (Existing->HasField(Key))
			{
				Key = Prefix + TEXT(":") + Base
					+ FString::Printf(TEXT("-%d"), ++Ordinal);
			}
			return Key;
		}

		void NormalizeCandidate(TSharedPtr<FJsonObject>& Spec)
		{
			if (!Spec.IsValid())
			{
				return;
			}
			const TSharedPtr<FJsonValue> CandidateValue =
				Spec->TryGetField(TEXT("candidate"));
			if (CandidateValue.IsValid()
				&& CandidateValue->Type == EJson::String)
			{
				const TSharedPtr<FJsonObject> Candidate = MakeShared<FJsonObject>();
				Candidate->SetStringField(TEXT("name"), CandidateValue->AsString());
				Spec->SetObjectField(TEXT("candidate"), Candidate);
				return;
			}
			if (CandidateValue.IsValid())
			{
				return;
			}

			FString CandidateName;
			const bool bMinimalSelector =
				!Spec->HasField(TEXT("node_type"))
				&& !Spec->HasField(TEXT("node_class"))
				&& Spec->TryGetStringField(TEXT("name"), CandidateName);
			if (!bMinimalSelector)
			{
				return;
			}
			const TSharedPtr<FJsonObject> Candidate = MakeShared<FJsonObject>();
			if (!CandidateName.IsEmpty())
			{
				Candidate->SetStringField(TEXT("name"), CandidateName);
			}
			Spec->RemoveField(TEXT("name"));
			Spec->SetObjectField(TEXT("candidate"), Candidate);
		}

		void NormalizeEdge(TSharedPtr<FJsonObject>& Spec)
		{
			if (!Spec.IsValid())
			{
				return;
			}
			auto BuildEndpoint = [&Spec](
				const TCHAR* ObjectField,
				const TCHAR* NodeField,
				const TCHAR* PinField,
				const TCHAR* PinOccurrenceField)
			{
				if (Spec->HasField(ObjectField))
				{
					return;
				}
				FString Node;
				FString Pin;
				if (Spec->TryGetStringField(NodeField, Node)
					&& Spec->TryGetStringField(PinField, Pin))
				{
					const TSharedPtr<FJsonObject> Endpoint = MakeShared<FJsonObject>();
					Endpoint->SetStringField(TEXT("node"), Node);
					Endpoint->SetStringField(TEXT("pin"), Pin);
					FString PinOccurrence;
					if (Spec->TryGetStringField(PinOccurrenceField, PinOccurrence))
					{
						Endpoint->SetNumberField(
							TEXT("pin_occurrence"), FCString::Atoi(*PinOccurrence));
					}
					Spec->SetObjectField(ObjectField, Endpoint);
					Spec->RemoveField(NodeField);
					Spec->RemoveField(PinField);
					Spec->RemoveField(PinOccurrenceField);
				}
			};
			BuildEndpoint(
				TEXT("from"), TEXT("from_node"), TEXT("from_pin"),
				TEXT("from_pin_occurrence"));
			BuildEndpoint(
				TEXT("to"), TEXT("to_node"), TEXT("to_pin"),
				TEXT("to_pin_occurrence"));
		}

		FString EscapePointerToken(FString Token)
		{
			Token.ReplaceInline(TEXT("~"), TEXT("~0"));
			Token.ReplaceInline(TEXT("/"), TEXT("~1"));
			return Token;
		}

		void AddContainerEntries(
			const TSharedPtr<FJsonValue>& Container,
			const FString& Prefix,
			const FString& Operation,
			TSharedRef<FJsonObject> OutMap,
			const bool bNodes,
			const TFunction<void(const FString&, const TSharedPtr<FJsonObject>&)>& AttachSource = {},
			const FString& SourcePointerPrefix = FString())
		{
			if (!Container.IsValid())
			{
				return;
			}
			auto AddOne = [&](const FString& SourceKey,
				const TSharedPtr<FJsonValue>& Item)
			{
				TSharedPtr<FJsonObject> Spec;
				if (Item.IsValid() && Item->Type == EJson::Object)
				{
					Spec = CloneJsonObject(Item->AsObject());
				}
				else if (bNodes && Item.IsValid()
					&& Item->Type == EJson::String)
				{
					Spec = MakeShared<FJsonObject>();
					Spec->SetStringField(TEXT("candidate"), Item->AsString());
				}
				if (!Spec.IsValid())
				{
					return;
				}
				bNodes ? NormalizeCandidate(Spec) : NormalizeEdge(Spec);
				if (AttachSource) AttachSource(SourcePointerPrefix + EscapePointerToken(SourceKey), Spec);
				if (!Spec->HasField(TEXT("operation")))
				{
					Spec->SetStringField(TEXT("operation"), Operation);
				}
				const FString Key = ReadableMapKey(
					Prefix, SourceKey, Spec, bNodes, OutMap);
				OutMap->SetObjectField(Key, Spec);
			};

			if (Container->Type == EJson::Array)
			{
				const TArray<TSharedPtr<FJsonValue>>& Items = Container->AsArray();
				for (int32 Index = 0; Index < Items.Num(); ++Index)
				{
					AddOne(LexToString(Index), Items[Index]);
				}
			}
			else if (Container->Type == EJson::Object)
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Item :
					Container->AsObject()->Values)
				{
					if (!bNodes && Item.Value.IsValid()
						&& Item.Value->Type == EJson::Object)
					{
						const TSharedPtr<FJsonObject> Candidate = Item.Value->AsObject();
						const bool bEdgeLeaf = Candidate->HasField(TEXT("from"))
							|| Candidate->HasField(TEXT("from_node"));
						if (!bEdgeLeaf)
						{
							AddContainerEntries(
								Item.Value,
								Prefix,
								Operation,
								OutMap,
								false, AttachSource,
								SourcePointerPrefix + EscapePointerToken(Item.Key) + TEXT("/"));
							continue;
						}
					}
					AddOne(Item.Key, Item.Value);
				}
			}
		}

		FString OperationFor(
			const FString& UnitId,
			const FString& RelationId,
			const TArray<FStructuralEdit>& StructuralEdits)
		{
			for (const FStructuralEdit& Edit : StructuralEdits)
			{
				if ((Edit.GetSubjectUnitId() == UnitId
						|| Edit.GetRelationId() == RelationId))
				{
					switch (Edit.GetKind())
					{
					case EStructuralEditKind::Remove:
					case EStructuralEditKind::RemoveRelation:
						return TEXT("remove");
					case EStructuralEditKind::Replace:
						return Edit.GetReplacementUnitId() == UnitId
							? TEXT("create") : TEXT("remove");
					case EStructuralEditKind::Rename:
						return TEXT("update");
					default:
						return TEXT("create");
					}
				}
			}
			// A semantic unit already belongs to the exported workspace. Creating a
			// new graph, node, or relation is expressed by an explicit structural
			// edit; an edited existing unit therefore defaults to update.
			return TEXT("update");
		}

		bool TryParseUnitIdentity(
			const FSemanticUnit& Unit,
			UeAgentAuthoringIdentity::FCanonicalIdentity& OutIdentity,
			FWorkspaceError& OutError)
		{
			UE::UAI::Contract::FDiagnostic Error;
			if (Unit.GetTargetIdentityKey().IsEmpty())
			{
				Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("blueprint_semantic_identity_invalid"),
					TEXT("The Blueprint semantic unit has no captured target identity."), TEXT("document_restore"));
				Error.SourceKind = TEXT("native_result");
				Error.PropertyPath = TEXT("target_identity_present");
				Error.InternalContext = Unit.GetUnitId();
				Error.ActualValue = MakeShared<FJsonValueBoolean>(false);
				Error.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				OutError.SetFailure(MoveTemp(Error), Unit.GetUnitId());
				return false;
			}
			if (!UeAgentAuthoringIdentity::ParseCanonicalKey(Unit.GetTargetIdentityKey(), OutIdentity, Error))
			{
				Error.SourceKind = TEXT("native_result");
				Error.SubjectId = Unit.GetTargetIdentityKey();
				OutError.SetFailure(MoveTemp(Error), Unit.GetUnitId());
				return false;
			}
			return true;
		}

		bool TryFindOrAddObject(
			const TSharedRef<FJsonObject>& Parent,
			const FString& Field,
			TSharedPtr<FJsonObject>& OutObject,
			FWorkspaceError& OutError,
			const FString& UnitId)
		{
			if (const TSharedPtr<FJsonValue> Existing = Parent->TryGetField(Field))
			{
				if (Existing->Type != EJson::Object)
				{
					return Fail(
						OutError,
						TEXT("blueprint_semantic_provider_shape_invalid"), TEXT("A Blueprint provider extension field must be an object."),
						UnitId,
						TEXT("/provider_extension/") + EscapeJsonPointerSegment(Field), Existing, MakeShared<FJsonValueString>(TEXT("object")));
				}
				OutObject = Existing->AsObject();
				return true;
			}
			OutObject = MakeShared<FJsonObject>();
			Parent->SetObjectField(Field, OutObject);
			return true;
		}

		bool TryFindComponentSpecByName(
			const TSharedRef<FJsonObject>& Components,
			const FString& ComponentName,
			TSharedPtr<FJsonObject>& OutComponent,
			FWorkspaceError& OutError,
			const FString& UnitId)
		{
			OutComponent.Reset();
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : Components->Values)
			{
				if (!Entry.Value.IsValid() || Entry.Value->Type != EJson::Object)
				{
					return Fail(
						OutError,
						TEXT("blueprint_semantic_component_spec_invalid"), TEXT("Each component declaration must be an object."),
						UnitId,
						TEXT("/provider_extension/components/") + EscapeJsonPointerSegment(Entry.Key), Entry.Value, MakeShared<FJsonValueString>(TEXT("object")));
				}

				const TSharedPtr<FJsonObject> Candidate = Entry.Value->AsObject();
				FString CandidateName;
				Candidate->TryGetStringField(TEXT("name"), CandidateName);
				if (CandidateName.IsEmpty() && Entry.Key.StartsWith(TEXT("component:")))
				{
					CandidateName = Entry.Key.RightChop(10);
				}
				if (!CandidateName.Equals(ComponentName, ESearchCase::IgnoreCase)
					&& !Entry.Key.Equals(TEXT("component:") + ComponentName, ESearchCase::IgnoreCase))
				{
					continue;
				}
				if (OutComponent.IsValid())
				{
					return Fail(
						OutError,
						TEXT("blueprint_semantic_component_identity_ambiguous"), TEXT("More than one component matches the requested name."),
						UnitId,
						TEXT("/provider_extension/components"), MakeShared<FJsonValueString>(ComponentName), MakeShared<FJsonValueString>(TEXT("unique component name")));
				}
				OutComponent = Candidate;
			}
			return true;
		}

		bool TryAppendPropertyDescriptor(
			const TSharedRef<FJsonObject>& OwnerSpec,
			const TSharedPtr<FJsonValue>& Descriptor,
			FWorkspaceError& OutError,
			const FString& UnitId,
			const FResolvedSemanticWorkspace* Workspace)
		{
			TArray<TSharedPtr<FJsonValue>> Properties;
			if (const TSharedPtr<FJsonValue> Existing =
					OwnerSpec->TryGetField(TEXT("properties")))
			{
				if (Existing->Type != EJson::Array)
				{
					return Fail(
						OutError,
						TEXT("blueprint_semantic_property_list_invalid"), TEXT("The owner property list must be an array."),
						UnitId,
						TEXT("/properties"), Existing, MakeShared<FJsonValueString>(TEXT("array")));
				}
				Properties = Existing->AsArray();
			}
			const auto LocatedDescriptor = CloneJsonValue(Descriptor);
			if (Workspace && LocatedDescriptor && LocatedDescriptor->Type == EJson::Object)
				Workspace->AttachInlinePropertySources(UnitId, TEXT(""), *LocatedDescriptor->AsObject(), TEXT("value"));
			Properties.Add(LocatedDescriptor);
			OwnerSpec->SetArrayField(TEXT("properties"), MoveTemp(Properties));
			return true;
		}

		bool TryPinDefaultText(
			const TSharedPtr<FJsonValue>& Value,
			FString& OutText)
		{
			if (!Value.IsValid()) return false;
			if (Value->Type == EJson::String)
			{
				OutText = Value->AsString();
				return true;
			}
			if (Value->Type == EJson::Boolean)
			{
				OutText = Value->AsBool() ? TEXT("true") : TEXT("false");
				return true;
			}
			if (Value->Type == EJson::Number)
			{
				// CanonicalJson keeps the original integer/decimal lexeme in
				// FJsonValueNumberString; converting through double loses it.
				return Value->TryGetString(OutText);
			}
			return false;
		}
	}

	bool TryProjectSemanticProviderExtension(
		const FAuthoringModelRef& ProposedModel,
		const TArray<FString>& ChangedSemanticUnitIds,
		const TMap<FString, TSharedPtr<FJsonValue>>& PropertyChangesByUnitId,
		const TArray<FStructuralEdit>& StructuralEdits,
		TSharedPtr<FJsonObject>& OutProviderExtension,
		FWorkspaceError& OutError,
		const FResolvedSemanticWorkspace* Workspace,
		const FAuthoringModel* CurrentModel)
	{
		OutProviderExtension.Reset();
		OutError.Reset();
		for (const FStructuralEdit& Edit : StructuralEdits)
		{
			if (!Edit.IsValid())
			{
				OutError.SetFailure(Edit.GetValidationDiagnostic(), Edit.GetOwnerUnitId());
				return false;
			}
			if (Edit.GetKind() == EStructuralEditKind::SetOrder)
			{
				return Fail(
					OutError,
					TEXT("blueprint_semantic_set_order_unsupported"), TEXT("Blueprint semantic import does not support the SetOrder structural edit."),
					Edit.GetOwnerUnitId(),
					TEXT("/structural_edits/") + Edit.GetEditId());
			}
		}

		TSet<FString> Changed;
		for (const FString& UnitId : ChangedSemanticUnitIds)
		{
			Changed.Add(UnitId);
		}
		const TSharedRef<FJsonObject> Graphs = MakeShared<FJsonObject>();
		const TSharedRef<FJsonObject> Components = MakeShared<FJsonObject>();
		const TSharedRef<FJsonObject> Members = MakeShared<FJsonObject>();
		const TSharedRef<FJsonObject> Interfaces = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> ClassDefaults;
		auto CapturedName = [&](const FSemanticUnit& Unit,
			const UeAgentAuthoringIdentity::FCanonicalIdentity& Identity, const TCHAR* Selector)
		{
			FString Name = Identity.GetSelector(Selector);
			const FSemanticUnit* Captured = CurrentModel ? CurrentModel->FindUnit(Unit.GetUnitId()) : nullptr;
			if (!Captured) Captured = &Unit;
			if (Name.IsEmpty())
				if (const auto* Value = Captured->GetValues().Find(TEXT("name"))) Name = (*Value)->GetText();
			return Name;
		};
		TMap<FString, FString> CapturedComponentKeys;
		TMap<FString, const FSemanticUnit*> CapturedComponentUnits;
		if (CurrentModel)
			for (const FSemanticUnit& Unit : CurrentModel->GetUnits())
			{
				if (Unit.GetSemanticTypeRef() != ComponentSemanticId) continue;
				UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
				if (!TryParseUnitIdentity(Unit, Identity, OutError)) return false;
				const FString Name = CapturedName(Unit, Identity, TEXT("component_name"));
				if (!Name.IsEmpty())
				{
					CapturedComponentKeys.Add(Name, GetReusableComponentKey(Identity));
					CapturedComponentUnits.Add(Name, &Unit);
				}
			}
		TSet<FString> RemovedGraphUnits;
		for (const FSemanticUnit& Unit : ProposedModel->GetUnits())
		{
			if (!Changed.Contains(Unit.GetUnitId()) || Unit.GetSemanticTypeRef() != TEXT("uai.authoring.surface.graph_collection")) continue;
			TSharedPtr<FJsonObject> Values;
			if (!UnitValuesToObject(Unit, Values, OutError)) return false;
			const TSharedPtr<FJsonObject>* Declarations = nullptr;
			if (!Values->TryGetObjectField(TEXT("graph_declarations"), Declarations))
				return Fail(OutError, TEXT("blueprint_graph_declarations_missing"), TEXT("The graph collection requires a graph_declarations object."), Unit.GetUnitId(), TEXT("/graph_declarations"), Values->TryGetField(TEXT("graph_declarations")), MakeShared<FJsonValueString>(TEXT("object")));
			for (const auto& Entry : (*Declarations)->Values)
			{
				if (!Entry.Value.IsValid() || Entry.Value->Type != EJson::Object)
					return Fail(OutError, TEXT("blueprint_graph_declaration_invalid"), TEXT("Each graph declaration must be an object."), Unit.GetUnitId(), TEXT("/graph_declarations/") + EscapeJsonPointerSegment(Entry.Key), Entry.Value, MakeShared<FJsonValueString>(TEXT("object")));
				auto Spec = CloneJsonObject(Entry.Value->AsObject());
				FString Operation;
				if (Spec->TryGetStringField(TEXT("operation"), Operation) && Operation == TEXT("remove"))
				{
					for (const FSemanticUnit& Existing : ProposedModel->GetUnits())
					{
						if (Existing.GetSemanticTypeRef() != GraphSemanticId) continue;
						TSharedPtr<FJsonObject> ExistingValues;
						if (!UnitValuesToObject(Existing, ExistingValues, OutError)) return false;
						const TSharedPtr<FJsonObject>* Header = nullptr;
						if (ExistingValues->TryGetObjectField(TEXT("graph"), Header)
							&& (*Header)->GetStringField(TEXT("graph_guid")) == Spec->GetStringField(TEXT("graph_guid")))
							RemovedGraphUnits.Add(Existing.GetUnitId());
					}
				}
				if (Spec->TryGetStringField(TEXT("operation"), Operation) && Operation == TEXT("update"))
				{
					bool bMatched = false;
					for (const FSemanticUnit& Existing : ProposedModel->GetUnits())
					{
						if (Existing.GetSemanticTypeRef() != TEXT("uai.semantic.blueprint.graph")) continue;
						TSharedPtr<FJsonObject> ExistingValues;
						if (!UnitValuesToObject(Existing, ExistingValues, OutError)) return false;
						const TSharedPtr<FJsonObject>* Header = nullptr;
						if (!ExistingValues->TryGetObjectField(TEXT("graph"), Header)) continue;
						if ((*Header)->GetStringField(TEXT("graph_guid")) != Spec->GetStringField(TEXT("graph_guid"))) continue;
						if ((*Header)->GetStringField(TEXT("graph_kind")) != Spec->GetStringField(TEXT("graph_kind")))
							return Fail(OutError, TEXT("blueprint_graph_declaration_identity_change_unsupported"), TEXT("An existing graph cannot change its graph_kind."), Unit.GetUnitId(), TEXT("/graph_declarations/") + EscapeJsonPointerSegment(Entry.Key) + TEXT("/graph_kind"), Spec->TryGetField(TEXT("graph_kind")), (*Header)->TryGetField(TEXT("graph_kind")));
						if ((*Header)->GetStringField(TEXT("name")) != Spec->GetStringField(TEXT("name")))
						{
							FGuid Guid;
							if (!FGuid::Parse(Spec->GetStringField(TEXT("graph_guid")), Guid))
								return Fail(OutError, TEXT("blueprint_graph_declaration_identity_missing"), TEXT("The graph declaration has no valid identity matching a captured graph."), Unit.GetUnitId());
							Graphs->SetObjectField(Guid.ToString(EGuidFormats::DigitsWithHyphens), Spec);
						}
						bMatched = true;
						break;
					}
					if (!bMatched) return Fail(OutError, TEXT("blueprint_graph_declaration_identity_missing"), TEXT("The graph declaration has no valid identity matching a captured graph."), Unit.GetUnitId());
					continue;
				}
				if (!Spec->HasField(TEXT("name"))) Spec->SetStringField(TEXT("name"), Entry.Key);
				FString Kind, Guid;
				if (!Spec->TryGetStringField(TEXT("graph_kind"), Kind))
					return Fail(OutError, TEXT("blueprint_graph_kind_missing"), TEXT("The graph declaration requires a graph_kind string."), Unit.GetUnitId(), TEXT("/graph_declarations/") + EscapeJsonPointerSegment(Entry.Key) + TEXT("/graph_kind"), Spec->TryGetField(TEXT("graph_kind")), MakeShared<FJsonValueString>(TEXT("graph kind string")));
				Spec->TryGetStringField(TEXT("graph_guid"), Guid);
				Graphs->SetObjectField(Guid.IsEmpty() ? Kind + TEXT(":") + Entry.Key : Guid, Spec);
			}
		}
		auto IsRemovedGraphBody = [&](FString UnitId)
		{
			TSet<FString> Visited;
			while (!UnitId.IsEmpty() && !Visited.Contains(UnitId))
			{
				if (RemovedGraphUnits.Contains(UnitId)) return true;
				Visited.Add(UnitId);
				const FSemanticUnit* Unit = ProposedModel->FindUnit(UnitId);
				if (!Unit) break;
				UnitId = Unit->GetOwnerRef();
			}
			return false;
		};
		for (const FString& UnitId : Changed)
			if (IsRemovedGraphBody(UnitId))
				return Fail(OutError, TEXT("blueprint_graph_remove_body_edit_conflict"), TEXT("A graph cannot be removed while its body is also being edited."), UnitId);
		for (const FStructuralEdit& Edit : StructuralEdits)
			if (IsRemovedGraphBody(Edit.GetOwnerUnitId()))
				return Fail(OutError, TEXT("blueprint_graph_remove_body_edit_conflict"), TEXT("A graph cannot be removed while its body is also being edited."), Edit.GetOwnerUnitId());
		for (const FSemanticUnit& Unit : ProposedModel->GetUnits())
		{
			if (!Changed.Contains(Unit.GetUnitId())
				|| Unit.GetUnitKind() != ESemanticUnitKind::StructureState
				|| Unit.GetRole() != TEXT("member_collection"))
			{
				continue;
			}
			TSharedPtr<FJsonObject> Values;
			if (!UnitValuesToObject(Unit, Values, OutError))
			{
				return false;
			}
			for (const TCHAR* Field : {TEXT("variables"), TEXT("delegates")})
			{
				const TSharedPtr<FJsonObject>* Map = nullptr;
				if (Values->TryGetObjectField(Field, Map) && Map && Map->IsValid())
				{
					const auto Declarations = MakeShared<FJsonObject>();
					for (const auto& Entry : (*Map)->Values)
					{
						if (!Entry.Value.IsValid() || Entry.Value->Type != EJson::Object)
							return Fail(OutError, TEXT("blueprint_member_declaration_invalid"), TEXT("Each Blueprint member declaration must be an object."), Unit.GetUnitId(), TEXT("/") + FString(Field) + TEXT("/") + EscapeJsonPointerSegment(Entry.Key), Entry.Value, MakeShared<FJsonValueString>(TEXT("object")));
						auto Spec = CloneJsonObject(Entry.Value->AsObject());
						FString Guid;
						Spec->TryGetStringField(TEXT("guid"), Guid);
						if (!Spec->HasField(TEXT("name"))) Spec->SetStringField(TEXT("name"), Entry.Key);
						Declarations->SetObjectField(Guid.IsEmpty() ? Entry.Key : Guid, Spec);
					}
					Members->SetObjectField(Field, Declarations);
				}
			}
			const TSharedPtr<FJsonObject>* InterfaceMap = nullptr;
			if (Values->TryGetObjectField(TEXT("interfaces"), InterfaceMap)
				&& InterfaceMap && InterfaceMap->IsValid())
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Field
					: (*InterfaceMap)->Values)
				{
					Interfaces->SetField(Field.Key, CloneJsonValue(Field.Value));
				}
			}
		}
		for (const FSemanticUnit& Unit : ProposedModel->GetUnits())
		{
			if (Unit.GetSemanticTypeRef() != ClassDefaultPropertySemanticId
				|| !Changed.Contains(Unit.GetUnitId()))
			{
				continue;
			}
			const TSharedPtr<FJsonValue>* Descriptor =
				PropertyChangesByUnitId.Find(Unit.GetUnitId());
			if (!Descriptor || !Descriptor->IsValid()
				|| (*Descriptor)->Type != EJson::Object)
			{
				return Fail(
					OutError,
					TEXT("blueprint_semantic_property_change_missing"), TEXT("The changed property has no projected descriptor object."),
					Unit.GetUnitId(),
					TEXT("/property_changes"), nullptr, nullptr, true);
			}
			ClassDefaults.Add(CloneJsonValue(*Descriptor));
		}
		for (const FSemanticUnit& GraphUnit : ProposedModel->GetUnits())
		{
			if (GraphUnit.GetSemanticTypeRef()
				!= TEXT("uai.semantic.blueprint.graph"))
			{
				continue;
			}
			bool bGraphChanged = Changed.Contains(GraphUnit.GetUnitId());
			for (const FStructuralEdit& Edit : StructuralEdits)
			{
				bGraphChanged |= Edit.GetOwnerUnitId() == GraphUnit.GetUnitId();
			}
			for (const FSemanticUnit& Unit : ProposedModel->GetUnits())
			{
				bGraphChanged |= Unit.GetOwnerRef() == GraphUnit.GetUnitId()
					&& Changed.Contains(Unit.GetUnitId());
			}
			if (!bGraphChanged)
			{
				continue;
			}

			TSharedPtr<FJsonObject> Values;
			if (!UnitValuesToObject(GraphUnit, Values, OutError))
			{
				return false;
			}
			TSharedPtr<FJsonObject> GraphSpec;
			const TSharedPtr<FJsonValue> GraphValue = Values->TryGetField(TEXT("graph"));
			if (GraphValue.IsValid() && GraphValue->Type == EJson::Object)
			{
				GraphSpec = CloneJsonObject(GraphValue->AsObject());
			}
			else
			{
				GraphSpec = MakeShared<FJsonObject>();
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Values->Values)
				{
					if (Field.Key != TEXT("nodes") && Field.Key != TEXT("edges"))
					{
						GraphSpec->SetField(Field.Key, CloneJsonValue(Field.Value));
					}
				}
			}
			GraphSpec->RemoveField(TEXT("graph_declaration_owned"));
			if (Workspace)
				if (const auto* Baseline = Workspace->GetBaselineUnits().Find(GraphUnit.GetUnitId()))
				{
					TSharedPtr<FJsonObject> OldValues;
					if (!UnitValuesToObject(*Baseline, OldValues, OutError)) return false;
					const auto OldGraph = OldValues->TryGetField(TEXT("graph"));
					const auto OldHeader = OldGraph && OldGraph->Type == EJson::Object ? OldGraph->AsObject() : OldValues;
					const auto BeforeSignature = OldHeader->TryGetField(TEXT("signature"));
					const auto Signature = GraphSpec->TryGetField(TEXT("signature"));
					if (BeforeSignature && Signature && FJsonValue::CompareEqual(*BeforeSignature, *Signature))
						GraphSpec->RemoveField(TEXT("signature"));
					else if (BeforeSignature && Signature && BeforeSignature->Type == EJson::Object && Signature->Type == EJson::Object)
					{
						// Signature shape is shared; unchanged default text is context,
						// not a request to overwrite each return node's own literal.
						for (const TCHAR* Group : { TEXT("inputs"), TEXT("outputs") })
						{
							const TArray<TSharedPtr<FJsonValue>>* OldPins = nullptr;
							const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
							if (!BeforeSignature->AsObject()->TryGetArrayField(Group, OldPins)
								|| !Signature->AsObject()->TryGetArrayField(Group, Pins)) continue;
							for (const auto& Pin : *Pins)
							{
								if (!Pin || Pin->Type != EJson::Object) continue;
								FString Name, Default;
								if (!Pin->AsObject()->TryGetStringField(TEXT("name"), Name)
									|| !Pin->AsObject()->TryGetStringField(TEXT("default_value"), Default)) continue;
								for (const auto& OldPin : *OldPins)
								{
									FString OldName, OldDefault;
									if (OldPin && OldPin->Type == EJson::Object
										&& OldPin->AsObject()->TryGetStringField(TEXT("name"), OldName) && OldName == Name
										&& OldPin->AsObject()->TryGetStringField(TEXT("default_value"), OldDefault) && OldDefault == Default)
										Pin->AsObject()->RemoveField(TEXT("default_value"));
								}
							}
						}
					}
				}
			if (!GraphSpec->HasField(TEXT("operation")))
			{
				GraphSpec->SetStringField(
					TEXT("operation"),
					OperationFor(GraphUnit.GetUnitId(), FString(), StructuralEdits));
			}

			const TSharedRef<FJsonObject> Nodes = MakeShared<FJsonObject>();
			TSharedPtr<FJsonValue> NodeChanges = Values->TryGetField(TEXT("nodes"));
			if (Workspace)
				if (const auto* Baseline = Workspace->GetBaselineUnits().Find(GraphUnit.GetUnitId()))
				{
					TSharedPtr<FJsonObject> Changes;
					if (!TryProjectGraphAuthoringNodeChanges(GraphUnit.GetUnitId(), Baseline->GetValues(),
						GraphUnit.GetValues(), Changes, OutError)) return false;
					NodeChanges = MakeShared<FJsonValueObject>(Changes);
				}
			AddContainerEntries(
				NodeChanges,
				TEXT("node"), TEXT("create"),
				Nodes, true, [&](const FString& SourcePointer, const TSharedPtr<FJsonObject>& Spec)
				{
					if (Workspace) Workspace->AttachInlinePropertySources(GraphUnit.GetUnitId(), TEXT("/nodes/") + SourcePointer, *Spec);
				});
			const TSharedRef<FJsonObject> Edges = MakeShared<FJsonObject>();
			AddContainerEntries(
				Values->TryGetField(TEXT("edges")),
				TEXT("edge"), TEXT("create"),
				Edges, false, [&](const FString& SourcePointer, const TSharedPtr<FJsonObject>& Spec)
				{
					if (Workspace) Workspace->AttachInlinePropertySources(GraphUnit.GetUnitId(), TEXT("/edges/") + SourcePointer, *Spec);
				});
			// The common graph resolver emits update for captured, unchanged edges.
			// They are context, not a request to recreate links removed by native
			// node/variable reconstruction. Explicit relation edits below retain
			// their own operation and intent verification.
			for (auto It = Edges->Values.CreateIterator(); Workspace && It; ++It)
			{
				FString EdgeOperation;
				if (It.Value()->AsObject()->TryGetStringField(TEXT("operation"), EdgeOperation)
					&& EdgeOperation == TEXT("update")) It.RemoveCurrent();
			}

			for (const FSemanticUnit& Unit : ProposedModel->GetUnits())
			{
				if (Unit.GetOwnerRef() != GraphUnit.GetUnitId())
				{
					continue;
				}
				bool bUnitSelected = Changed.Contains(Unit.GetUnitId());
				for (const FStructuralEdit& Edit : StructuralEdits)
				{
					bUnitSelected |= Edit.GetSubjectUnitId() == Unit.GetUnitId()
						|| Edit.GetReplacementUnitId() == Unit.GetUnitId()
						|| Edit.GetRelationId() == Unit.GetUnitId();
				}
				if (!bUnitSelected)
				{
					continue;
				}
				TSharedPtr<FJsonObject> UnitValues;
				if (!UnitValuesToObject(Unit, UnitValues, OutError))
				{
					return false;
				}
				if (Unit.GetUnitKind() == ESemanticUnitKind::ItemState)
				{
					TSharedPtr<FJsonObject> NodeSpec = UnitValues;
					NormalizeCandidate(NodeSpec);
					if (Workspace) Workspace->AttachInlinePropertySources(Unit.GetUnitId(), FString(), *NodeSpec);
					if (!NodeSpec->HasField(TEXT("operation")))
					{
						NodeSpec->SetStringField(
							TEXT("operation"),
							OperationFor(Unit.GetUnitId(), FString(), StructuralEdits));
					}
					UeAgentAuthoringIdentity::FCanonicalIdentity NodeIdentity;
					if (!TryParseUnitIdentity(Unit, NodeIdentity, OutError))
					{
						return false;
					}
					const FString NodeKey = GetReusableNodeKey(NodeIdentity);
					if (NodeKey.IsEmpty())
					{
						return Fail(
							OutError,
							TEXT("blueprint_semantic_node_identity_missing"), TEXT("The captured node identity does not contain the graph or node selector needed for restoration."),
							Unit.GetUnitId(),
							TEXT("/target_identity_key"), nullptr, nullptr, true);
					}
					const FString NodeGuid = NodeIdentity.GetSelector(TEXT("node_guid"));
					if (!NodeGuid.IsEmpty())
					{
						NodeSpec->SetStringField(TEXT("node_guid"), NodeGuid);
					}
					Nodes->SetObjectField(NodeKey, NodeSpec);
				}
				else if (Unit.GetUnitKind() == ESemanticUnitKind::RelationSet)
				{
					TSharedPtr<FJsonValue> Relations =
						UnitValues->TryGetField(TEXT("relations"));
					if (!Relations.IsValid())
					{
						Relations = MakeShared<FJsonValueObject>(UnitValues);
					}
					AddContainerEntries(
						Relations,
						TEXT("edge"),
						OperationFor(FString(), Unit.GetUnitId(), StructuralEdits),
						Edges, false, [&](const FString& SourcePointer, const TSharedPtr<FJsonObject>& Spec)
						{
							const FString Prefix = UnitValues->HasField(TEXT("relations")) ? TEXT("/relations/") : TEXT("/");
							if (Workspace) Workspace->AttachInlinePropertySources(Unit.GetUnitId(), Prefix + SourcePointer, *Spec);
						});
				}
			}
			if (!Nodes->Values.IsEmpty())
			{
				GraphSpec->SetObjectField(TEXT("nodes"), Nodes);
			}
			if (!Edges->Values.IsEmpty())
			{
				GraphSpec->SetObjectField(TEXT("edges"), Edges);
			}

			FString GraphName;
			FString GraphKind;
			GraphSpec->TryGetStringField(TEXT("graph_name"), GraphName);
			if (GraphName.IsEmpty())
			{
				GraphSpec->TryGetStringField(TEXT("name"), GraphName);
			}
			GraphSpec->TryGetStringField(TEXT("graph_kind"), GraphKind);
			if (GraphKind.IsEmpty())
			{
				GraphKind = GraphUnit.GetRole().IsEmpty()
					? TEXT("event_graph") : GraphUnit.GetRole();
			}
			if (GraphName.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("blueprint_semantic_graph_name_missing"), TEXT("The graph declaration requires a non-empty name."),
					GraphUnit.GetUnitId(),
					TEXT("/graph/name"));
			}
			GraphSpec->SetStringField(TEXT("name"), GraphName);
			GraphSpec->SetStringField(TEXT("graph_kind"), GraphKind);
			FString GraphKey = GraphKind + TEXT(":") + GraphName;
			FString GraphGuid;
			if (!GraphSpec->TryGetStringField(TEXT("guid"), GraphGuid))
			{
				GraphSpec->TryGetStringField(TEXT("graph_guid"), GraphGuid);
			}
			FGuid ParsedGraphGuid;
			if (FGuid::Parse(GraphGuid, ParsedGraphGuid)
				&& ParsedGraphGuid.IsValid())
			{
				GraphKey = ParsedGraphGuid.ToString(EGuidFormats::DigitsWithHyphens);
			}
			// The collection owns names; graph body headers remain captured references.
			// Merge its rename into body edits rather than replaying the old header name.
			const TSharedPtr<FJsonObject>* DeclarationSpec = nullptr;
			if (Graphs->TryGetObjectField(GraphKey, DeclarationSpec))
				GraphSpec->SetStringField(TEXT("name"), (*DeclarationSpec)->GetStringField(TEXT("name")));
			Graphs->SetObjectField(GraphKey, GraphSpec);
		}

		for (const FSemanticUnit& ComponentTreeUnit : ProposedModel->GetUnits())
		{
			if (!Changed.Contains(ComponentTreeUnit.GetUnitId())
				|| ComponentTreeUnit.GetUnitKind() != ESemanticUnitKind::StructureState
				|| ComponentTreeUnit.GetRole() != TEXT("component_tree")
				|| !ComponentTreeUnit.GetValues().Contains(TEXT("components")))
			{
				continue;
			}

			TSharedPtr<FJsonObject> Values;
			if (!UnitValuesToObject(ComponentTreeUnit, Values, OutError))
			{
				return false;
			}
			const TSharedPtr<FJsonValue> ComponentMapValue =
				Values->TryGetField(TEXT("components"));
			if (!ComponentMapValue.IsValid()
				|| ComponentMapValue->Type != EJson::Object)
			{
				return Fail(
					OutError,
					TEXT("blueprint_semantic_component_map_invalid"), TEXT("The components collection must be an object."),
					ComponentTreeUnit.GetUnitId(),
					TEXT("/components"), ComponentMapValue, MakeShared<FJsonValueString>(TEXT("object")));
			}
			for (const TPair<FString, TSharedPtr<FJsonValue>>& ComponentRow :
				ComponentMapValue->AsObject()->Values)
			{
				if (!ComponentRow.Value.IsValid()
					|| ComponentRow.Value->Type != EJson::Object)
				{
					return Fail(
						OutError,
						TEXT("blueprint_semantic_component_spec_invalid"), TEXT("Each component declaration must be an object."),
						ComponentTreeUnit.GetUnitId(),
						TEXT("/components/") + EscapeJsonPointerSegment(ComponentRow.Key), ComponentRow.Value, MakeShared<FJsonValueString>(TEXT("object")));
				}
				const TSharedPtr<FJsonObject> SourceSpec = ComponentRow.Value->AsObject();
				FString ComponentKey = ComponentRow.Key;
				FGuid ComponentGuid;
				if (const FString* CapturedKey = CapturedComponentKeys.Find(ComponentRow.Key))
				{
					// Names are editable aliases, not recoverable selectors in a GUID key.
					// All views of a captured component must share its canonical key.
					ComponentKey = *CapturedKey;
				}
				else if (FGuid::Parse(ComponentKey, ComponentGuid)
					&& ComponentGuid.IsValid())
				{
					ComponentKey = ComponentGuid.ToString(EGuidFormats::DigitsWithHyphens);
				}
				else if (!ComponentKey.Contains(TEXT(":")))
				{
					FString InstanceName;
					SourceSpec->TryGetStringField(TEXT("name"), InstanceName);
					ComponentKey = TEXT("component:")
						+ (InstanceName.IsEmpty() ? ComponentRow.Key : InstanceName);
				}

				TSharedPtr<FJsonObject> ComponentSpec;
				if (!TryFindOrAddObject(
						Components, ComponentKey, ComponentSpec, OutError,
						ComponentTreeUnit.GetUnitId()))
				{
					return false;
				}
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Field :
					SourceSpec->Values)
				{
					ComponentSpec->SetField(Field.Key, CloneJsonValue(Field.Value));
				}
				if (const FSemanticUnit* Captured = CapturedComponentUnits.FindRef(ComponentRow.Key))
				{
					// A GUID key no longer supplies the name that the former alias key
					// supplied to the native stable-map parser.
					if (!ComponentSpec->HasField(TEXT("name")))
						ComponentSpec->SetStringField(TEXT("name"), ComponentRow.Key);
					FString DesiredClass;
					FString ComponentOperation;
					ComponentSpec->TryGetStringField(TEXT("operation"), ComponentOperation);
					const auto* OriginalClass = Captured->GetValues().Find(TEXT("class"));
					if ((ComponentOperation.IsEmpty() || ComponentOperation == TEXT("update")
						|| ComponentOperation == TEXT("upsert")) && OriginalClass
						&& SourceSpec->TryGetStringField(TEXT("class"), DesiredClass)
						&& !DesiredClass.Equals((*OriginalClass)->GetText(), ESearchCase::IgnoreCase))
						ComponentSpec->SetStringField(TEXT("operation"), TEXT("replace"));
				}
				if (Workspace)
					Workspace->AttachInlinePropertySources(ComponentTreeUnit.GetUnitId(),
						TEXT("/components/") + ComponentRow.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")),
						*ComponentSpec);
			}
		}

		// Item state and its collection are two views of the same native identity.
		// Merge only authored deltas; captured siblings must not undo collection edits.
		auto DeltaResources = MakeSemanticWorkspaceResourceTracker();
		for (const FSemanticUnit& Unit : ProposedModel->GetUnits())
		{
			const FString& Semantic = Unit.GetSemanticTypeRef();
			if (!Changed.Contains(Unit.GetUnitId())
				|| (Semantic != ComponentSemanticId && Semantic != VariableSemanticId
					&& Semantic != DelegateSemanticId && Semantic != InterfaceSemanticId)) continue;
			TSet<FString> Fields;
			if (!TryCollectChangedUnitValueFields(
				CurrentModel ? CurrentModel->FindUnit(Unit.GetUnitId()) : nullptr,
				Unit, DeltaResources, Fields, OutError, &ProposedModel.Get())) return false;
			if (Fields.IsEmpty()) continue;
			UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
			TSharedPtr<FJsonObject> Values;
			if (!TryParseUnitIdentity(Unit, Identity, OutError)
				|| !UnitValuesToObject(Unit, Values, OutError)) return false;
			TSharedPtr<FJsonObject> Spec;
			if (Semantic == ComponentSemanticId)
			{
				const FString Key = GetReusableComponentKey(Identity);
				const FString OriginalName = CapturedName(Unit, Identity, TEXT("component_name"));
				if (Components->HasField(Key))
				{
					if (!TryFindOrAddObject(Components, Key, Spec, OutError, Unit.GetUnitId())) return false;
				}
				else if (!TryFindComponentSpecByName(Components, OriginalName, Spec, OutError, Unit.GetUnitId())) return false;
				if (!Spec.IsValid() && !TryFindOrAddObject(Components, Key, Spec, OutError, Unit.GetUnitId())) return false;
				if (!Spec->HasField(TEXT("name"))) Spec->SetStringField(TEXT("name"), OriginalName);
				const FString Guid = Identity.GetSelector(TEXT("component_guid"));
				if (!Guid.IsEmpty()) Spec->SetStringField(TEXT("guid"), Guid);
				if (Fields.Contains(TEXT("class")) && CurrentModel
					&& CurrentModel->FindUnit(Unit.GetUnitId()) && !Fields.Contains(TEXT("operation")))
					Spec->SetStringField(TEXT("operation"), TEXT("replace"));
			}
			else if (Semantic == VariableSemanticId || Semantic == DelegateSemanticId)
			{
				const FString Field = Semantic == VariableSemanticId ? TEXT("variables") : TEXT("delegates");
				TSharedPtr<FJsonObject> Map;
				TArray<FString> Keys;
				UE::UAI::Contract::FDiagnostic KeyError;
				if (!GetReusableProjectionKeys(Field, Identity, Keys, KeyError)) {
					KeyError.SourceKind = TEXT("related_input");
OutError.SetFailure(MoveTemp(KeyError), Unit.GetUnitId());
					return false;
				}
				if (Keys.Num() != 1) {
					KeyError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("blueprint_semantic_projection_key_count_invalid"),
						TEXT("A variable or delegate semantic unit must produce exactly one projection key."), TEXT("semantic_projection"));
					KeyError.SourceKind = TEXT("related_input");
					KeyError.TargetPath = Identity.GetSelector(TEXT("asset_path"));
					KeyError.PropertyPath = TEXT("/projection_keys/count");
					KeyError.ActualValue = MakeShared<FJsonValueNumber>(Keys.Num());
					KeyError.ExpectedValue = MakeShared<FJsonValueNumber>(1);
OutError.SetFailure(MoveTemp(KeyError), Unit.GetUnitId());
					return false;
				}
				if (!TryFindOrAddObject(Members, Field, Map, OutError, Unit.GetUnitId())
					|| !TryFindOrAddObject(Map.ToSharedRef(), Keys[0], Spec, OutError, Unit.GetUnitId())) return false;
				if (!Spec->HasField(TEXT("name"))) Spec->SetStringField(TEXT("name"), CapturedName(Unit, Identity, TEXT("member_name")));
				const FString Guid = Identity.GetSelector(TEXT("member_guid"));
				if (!Guid.IsEmpty()) Spec->SetStringField(TEXT("guid"), Guid);
			}
			else
			{
				const FString OriginalClass = Identity.GetSelector(TEXT("interface_class"));
				const FString DesiredClass = Values->GetStringField(TEXT("class_path"));
				if (OriginalClass != DesiredClass)
				{
					auto Removed = MakeShared<FJsonObject>();
					Removed->SetStringField(TEXT("class_path"), OriginalClass);
					Removed->SetStringField(TEXT("operation"), TEXT("remove"));
					Interfaces->SetObjectField(OriginalClass, Removed);
				}
				if (!TryFindOrAddObject(Interfaces, DesiredClass, Spec, OutError, Unit.GetUnitId())) return false;
				Spec->SetStringField(TEXT("class_path"), DesiredClass);
			}
			if (!Spec->HasField(TEXT("operation")))
				Spec->SetStringField(TEXT("operation"), Semantic == InterfaceSemanticId ? TEXT("upsert")
					: OperationFor(Unit.GetUnitId(), FString(), StructuralEdits));
			for (const FString& Field : Fields)
			{
				const TSharedPtr<FJsonValue> Value = Values->TryGetField(Field);
				if (Value.IsValid()) Spec->SetField(Field, CloneJsonValue(Value));
				else Spec->RemoveField(Field);
			}
		}

		for (const FSemanticUnit& PinUnit : ProposedModel->GetUnits())
		{
			if (!Changed.Contains(PinUnit.GetUnitId())
				|| PinUnit.GetSemanticTypeRef() != PinDefaultSemanticId)
			{
				continue;
			}
			const FSemanticUnit* NodeUnit =
				ProposedModel->FindUnit(PinUnit.GetOwnerRef());
			const FSemanticUnit* GraphUnit = NodeUnit
				? ProposedModel->FindUnit(NodeUnit->GetOwnerRef()) : nullptr;
			if (!NodeUnit || !GraphUnit)
			{
				return Fail(OutError, TEXT("blueprint_semantic_pin_owner_missing"), TEXT("The pin owner node or its graph is missing from the semantic model."),
					PinUnit.GetUnitId(), TEXT("/owner_ref"), nullptr, nullptr, true);
			}
			UeAgentAuthoringIdentity::FCanonicalIdentity PinIdentity;
			UeAgentAuthoringIdentity::FCanonicalIdentity NodeIdentity;
			UeAgentAuthoringIdentity::FCanonicalIdentity GraphIdentity;
			if (!TryParseUnitIdentity(PinUnit, PinIdentity, OutError)
				|| !TryParseUnitIdentity(*NodeUnit, NodeIdentity, OutError)
				|| !TryParseUnitIdentity(*GraphUnit, GraphIdentity, OutError))
			{
				return false;
			}
			TSharedPtr<FJsonObject> Values;
			if (!UnitValuesToObject(PinUnit, Values, OutError)) return false;
			FString DefaultText;
			TSharedPtr<FJsonValue> DirectValue = Values->TryGetField(TEXT("value"));
			if (!DirectValue.IsValid())
			{
				DirectValue = MakeShared<FJsonValueObject>(Values);
			}
			FString StructPath;
			for (const FString& Dependency : PinUnit.GetDependencyRefs())
			{
				const FSemanticUnit* Definition = ProposedModel->FindUnit(Dependency);
				if (!Definition || Definition->GetRole() != TEXT("property_definition")) continue;
				const auto* Type = Definition->GetValues().Find(TEXT("type"));
				TSharedPtr<FJsonValue> TypeJson;
				UE::UAI::Contract::FDiagnostic TypeError;
				if (Type)
                {
                    if (!TryToJsonValue(**Type, TypeJson, TypeError))
                    {
                        TypeError.SourceKind = TEXT("native_result");
                        OutError.SetFailure(MoveTemp(TypeError), Definition->GetUnitId());
                        return false;
                    }
                    if (TypeJson.IsValid() && TypeJson->Type == EJson::Object)
                        TypeJson->AsObject()->TryGetStringField(TEXT("struct_path"), StructPath);
                }
			}
			UE::UAI::Contract::FDiagnostic ValueError;
			const bool bEncoded = StructPath.IsEmpty()
				? TryPinDefaultText(DirectValue, DefaultText)
				: UeAgentBlueprintAuthoringOps::TryEncodeStructPinDefaultText(
					StructPath, DirectValue, DefaultText, ValueError);
			if (!bEncoded)
			{
				if (StructPath.IsEmpty())
                {
                    ValueError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("blueprint_semantic_pin_value_invalid"),
                        TEXT("A scalar pin default must be a string, boolean, or number."), TEXT("document_restore"));
                    ValueError.ActualValue = DirectValue;
                    ValueError.ActualValuePresent = DirectValue.IsValid();
                    ValueError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("string, boolean, or number"));
                }
                UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(ValueError, TEXT("/value"));
                OutError.SetFailure(MoveTemp(ValueError), PinUnit.GetUnitId());
                return false;
			}
			const FString GraphKey = GetReusableGraphKey(GraphIdentity);
			const FString NodeKey = GetReusableNodeKey(NodeIdentity);
			const FString PinName = PinIdentity.GetSelector(TEXT("pin_name"));
			const FString Occurrence = PinIdentity.GetSelector(TEXT("pin_occurrence"));
			if (GraphKey.IsEmpty() || NodeKey.IsEmpty() || PinName.IsEmpty())
			{
				return Fail(OutError, TEXT("blueprint_semantic_pin_identity_missing"), TEXT("The captured pin identity does not contain its graph, node, or pin name."),
					PinUnit.GetUnitId(), TEXT("/target_identity_key"), nullptr, nullptr, true);
			}
			TSharedPtr<FJsonObject> GraphSpec;
			TSharedPtr<FJsonObject> Nodes;
			TSharedPtr<FJsonObject> NodeSpec;
			TSharedPtr<FJsonObject> PinDefaults;
			if (!TryFindOrAddObject(Graphs, GraphKey, GraphSpec, OutError, PinUnit.GetUnitId())
				|| !TryFindOrAddObject(GraphSpec.ToSharedRef(), TEXT("nodes"), Nodes, OutError, PinUnit.GetUnitId())
				|| !TryFindOrAddObject(Nodes.ToSharedRef(), NodeKey, NodeSpec, OutError, PinUnit.GetUnitId())
				|| !TryFindOrAddObject(NodeSpec.ToSharedRef(), TEXT("pin_defaults"), PinDefaults, OutError, PinUnit.GetUnitId()))
			{
				return false;
			}
			GraphSpec->SetStringField(TEXT("operation"), TEXT("update"));
			NodeSpec->SetStringField(TEXT("operation"), TEXT("update"));
			if (Workspace && !NodeSpec->HasField(TEXT("authoring_source")))
				Workspace->AttachInlinePropertySources(NodeUnit->GetUnitId(), FString(), *NodeSpec);
			const FString NodeGuid = NodeIdentity.GetSelector(TEXT("node_guid"));
			if (!NodeGuid.IsEmpty())
			{
				NodeSpec->SetStringField(TEXT("node_guid"), NodeGuid);
			}
			TSharedRef<FJsonObject> PinSpec = MakeShared<FJsonObject>();
			PinSpec->SetStringField(TEXT("pin_name"), PinName);
			PinSpec->SetNumberField(TEXT("pin_occurrence"), FCString::Atoi(*Occurrence));
			PinSpec->SetStringField(TEXT("default_value"), DefaultText);
			if (Workspace)
				Workspace->AttachInlinePropertySources(PinUnit.GetUnitId(),
					Values->HasField(TEXT("value")) ? TEXT("/value") : FString(), *PinSpec);
			FString PinGuid = PinIdentity.GetSelector(TEXT("pin_guid"));
			const FString PinKey = PinGuid.IsEmpty()
				? TEXT("pin:") + PinName + TEXT(":") + Occurrence
				: PinGuid;
			PinDefaults->SetObjectField(PinKey, PinSpec);
		}

		for (const FSemanticUnit& PropertyUnit : ProposedModel->GetUnits())
		{
			if (!Changed.Contains(PropertyUnit.GetUnitId())
				|| (PropertyUnit.GetSemanticTypeRef() != ComponentPropertySemanticId
					&& PropertyUnit.GetSemanticTypeRef() != NodePropertySemanticId))
			{
				continue;
			}
			const TSharedPtr<FJsonValue>* Descriptor =
				PropertyChangesByUnitId.Find(PropertyUnit.GetUnitId());
			if (!Descriptor || !Descriptor->IsValid()
				|| (*Descriptor)->Type != EJson::Object)
			{
				return Fail(
					OutError,
					TEXT("blueprint_semantic_property_change_missing"), TEXT("The changed property has no projected descriptor object."),
					PropertyUnit.GetUnitId(),
					TEXT("/property_changes"), nullptr, nullptr, true);
			}
			const FSemanticUnit* OwnerUnit =
				ProposedModel->FindUnit(PropertyUnit.GetOwnerRef());
			if (!OwnerUnit)
			{
				return Fail(
					OutError,
					TEXT("blueprint_semantic_property_owner_missing"), TEXT("The changed property owner is missing from the semantic model."),
					PropertyUnit.GetUnitId(),
					TEXT("/owner_ref"), nullptr, nullptr, true);
			}

			UeAgentAuthoringIdentity::FCanonicalIdentity OwnerIdentity;
			if (!TryParseUnitIdentity(*OwnerUnit, OwnerIdentity, OutError))
			{
				return false;
			}
			if (PropertyUnit.GetSemanticTypeRef() == ComponentPropertySemanticId)
			{
				FString ComponentName = CapturedName(*OwnerUnit, OwnerIdentity, TEXT("component_name"));
				if (ComponentName.IsEmpty())
				{
					TSharedPtr<FJsonObject> ComponentValues;
					if (!UnitValuesToObject(*OwnerUnit, ComponentValues, OutError))
					{
						return false;
					}
					ComponentValues->TryGetStringField(TEXT("name"), ComponentName);
				}
				if (ComponentName.IsEmpty())
				{
					return Fail(
						OutError,
						TEXT("blueprint_semantic_component_name_missing"), TEXT("The component has no captured or declared name."),
						OwnerUnit->GetUnitId(),
						TEXT("/name"));
				}

				const FString ComponentKey = GetReusableComponentKey(OwnerIdentity);
				if (ComponentKey.IsEmpty())
				{
					return Fail(
						OutError,
						TEXT("blueprint_semantic_component_identity_missing"), TEXT("The captured component identity has no reusable component selector."),
						OwnerUnit->GetUnitId(),
						TEXT("/target_identity_key"), nullptr, nullptr, true);
				}

				TSharedPtr<FJsonObject> ComponentSpec;
				if (Components->HasField(ComponentKey))
				{
					if (!TryFindOrAddObject(
							Components, ComponentKey, ComponentSpec, OutError,
							PropertyUnit.GetUnitId()))
					{
						return false;
					}
				}
				else if (!TryFindComponentSpecByName(
						Components, ComponentName, ComponentSpec, OutError,
						PropertyUnit.GetUnitId()))
				{
					return false;
				}
				if (!ComponentSpec.IsValid()
					&& !TryFindOrAddObject(
						Components, ComponentKey, ComponentSpec, OutError,
						PropertyUnit.GetUnitId()))
				{
					return false;
				}
				if (!ComponentSpec->HasField(TEXT("operation")))
					ComponentSpec->SetStringField(TEXT("operation"), TEXT("update"));
				if (!ComponentSpec->HasField(TEXT("name")))
					ComponentSpec->SetStringField(TEXT("name"), ComponentName);
				if (!TryAppendPropertyDescriptor(
						ComponentSpec.ToSharedRef(), *Descriptor, OutError,
						PropertyUnit.GetUnitId(), Workspace))
				{
					return false;
				}
				continue;
			}

			const FSemanticUnit* GraphUnit =
				ProposedModel->FindUnit(OwnerUnit->GetOwnerRef());
			if (!GraphUnit)
			{
				return Fail(
					OutError,
					TEXT("blueprint_semantic_node_graph_missing"), TEXT("The node owner graph is missing from the semantic model."),
					OwnerUnit->GetUnitId(),
					TEXT("/owner_ref"), nullptr, nullptr, true);
			}
			UeAgentAuthoringIdentity::FCanonicalIdentity GraphIdentity;
			if (!TryParseUnitIdentity(*GraphUnit, GraphIdentity, OutError))
			{
				return false;
			}
			const FString GraphKey = GetReusableGraphKey(GraphIdentity);
			const FString NodeKey = GetReusableNodeKey(OwnerIdentity);
			if (GraphKey.IsEmpty() || NodeKey.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("blueprint_semantic_node_identity_missing"), TEXT("The captured node identity does not contain the graph or node selector needed for restoration."),
					OwnerUnit->GetUnitId(),
					TEXT("/target_identity_key"), nullptr, nullptr, true);
			}
			TSharedPtr<FJsonObject> GraphSpec;
			if (!TryFindOrAddObject(
					Graphs, GraphKey, GraphSpec, OutError,
					PropertyUnit.GetUnitId()))
			{
				return false;
			}
			GraphSpec->SetStringField(TEXT("operation"), TEXT("update"));
			TSharedPtr<FJsonObject> Nodes;
			if (!TryFindOrAddObject(
					GraphSpec.ToSharedRef(), TEXT("nodes"), Nodes, OutError,
					PropertyUnit.GetUnitId()))
			{
				return false;
			}
			TSharedPtr<FJsonObject> NodeSpec;
			if (!TryFindOrAddObject(
					Nodes.ToSharedRef(), NodeKey, NodeSpec, OutError,
					PropertyUnit.GetUnitId()))
			{
				return false;
			}
			NodeSpec->SetStringField(TEXT("operation"), TEXT("update"));
			const FString NodeGuid = OwnerIdentity.GetSelector(TEXT("node_guid"));
			if (!NodeGuid.IsEmpty())
			{
				NodeSpec->SetStringField(TEXT("node_guid"), NodeGuid);
			}
			if (!TryAppendPropertyDescriptor(
					NodeSpec.ToSharedRef(), *Descriptor, OutError,
					PropertyUnit.GetUnitId(), Workspace))
			{
				return false;
			}
		}

		OutProviderExtension = MakeShared<FJsonObject>();
		if (!Graphs->Values.IsEmpty())
		{
			OutProviderExtension->SetObjectField(TEXT("graphs"), Graphs);
		}
		if (!Components->Values.IsEmpty())
		{
			OutProviderExtension->SetObjectField(TEXT("components"), Components);
		}
		if (!Members->Values.IsEmpty())
		{
			OutProviderExtension->SetObjectField(TEXT("members"), Members);
		}
		if (!Interfaces->Values.IsEmpty())
		{
			OutProviderExtension->SetObjectField(TEXT("interfaces"), Interfaces);
		}
		if (!ClassDefaults.IsEmpty())
		{
			OutProviderExtension->SetArrayField(
				TEXT("class_defaults"), MoveTemp(ClassDefaults));
		}
		return true;
	}
}

// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringEncodedTypes.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/UeAgentReflectedPropertyRows.h"
#include "Core/HjsonWorkspace/UeAgentKeyedItemAuthoringProjection.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "JsonUtils/JsonPointer.h"
#include "String/LexFromString.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace UeAgentDetailsAuthoringService
{
	namespace
	{
		FString RefineReadableValuePointer(TSharedPtr<FJsonValue> Value, const FString& Base,
			const FString& NativeSuffix, const FDiagnostic& Issue, bool& bOutExact)
		{
			bOutExact = false;
			if (!Value) return Base;
			// Keep a producer's existing encoded-value leaf whenever it resolves here.
			for (const FString* Existing : {&Issue.Source.InternalJsonPath, &Issue.Source.JsonPath})
			{
				const FString Relative = Existing->StartsWith(Base + TEXT("/")) ? Existing->RightChop(Base.Len()) : *Existing;
				TSharedPtr<FJsonValue> Found;
				if (!Relative.IsEmpty() && Relative != TEXT("/") && Relative.StartsWith(TEXT("/"))
					&& UE::Json::FJsonPointer(Relative).TryGet(Value, Found))
				{
					// A producer-relative pointer may be an ancestor of a more specific native path.
					bOutExact = NativeSuffix.IsEmpty();
					return Base + Relative;
				}
			}
			FString Pointer = Base;
			auto Descend = [&](const FString& Relative)
			{
				TSharedPtr<FJsonValue> Found;
				if (!UE::Json::FJsonPointer(Relative).TryGet(Value, Found)) return false;
				Value = MoveTemp(Found);
				Pointer += Relative;
				return true;
			};
			int32 Cursor = 0;
			bool bComplete = true;
			while (Cursor < NativeSuffix.Len())
			{
				if (NativeSuffix[Cursor] == TCHAR('.'))
				{
					const int32 Start = ++Cursor;
					while (Cursor < NativeSuffix.Len() && NativeSuffix[Cursor] != TCHAR('.') && NativeSuffix[Cursor] != TCHAR('[')) ++Cursor;
					if (Cursor == Start || !Value || Value->Type != EJson::Object || !Value->AsObject()) { bComplete = false; break; }
					const FString Field = UE::Json::FJsonPointer::EscapePart(NativeSuffix.Mid(Start, Cursor - Start));
					if (Descend(TEXT("/fields/") + Field)) continue;
					// Untagged compact structs use their actual member names directly.
					if (!Value->AsObject()->HasField(TEXT("kind")) && Descend(TEXT("/") + Field)) continue;
					bComplete = false;
					break;
				}
				if (NativeSuffix[Cursor] == TCHAR('['))
				{
					const int32 End = NativeSuffix.Find(TEXT("]"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Cursor + 1);
					if (End == INDEX_NONE) { bComplete = false; break; }
					const FString Text = NativeSuffix.Mid(Cursor + 1, End - Cursor - 1);
					int32 Index = INDEX_NONE;
					if (Text.IsEmpty() || !Text.IsNumeric() || !LexTryParseString(Index, *Text) || Index < 0 || !Value) { bComplete = false; break; }
					const FString Relative = (Value->Type == EJson::Array ? TEXT("/") : TEXT("/items/")) + FString::FromInt(Index);
					if (!Descend(Relative)) { bComplete = false; break; }
					Cursor = End + 1;
					continue;
				}
				bComplete = false;
				break;
			}
			bOutExact = bComplete && Cursor == NativeSuffix.Len();
			return Pointer;
		}

		bool BuildReadableSchema(const UeAgentAuthoringState::FStateSnapshot& Snapshot,
			TArray<UE::UAI::HjsonWorkspace::FReadablePropertySchema>& OutSchema,
			TMap<int32, TSharedPtr<FJsonObject>>& OrderedOwnedSchemas, FDiagnostic& OutError)
		{
			using namespace UE::UAI::HjsonWorkspace;
			using namespace UE::UAI::StateSemantic;
			auto Resources = MakeSemanticWorkspaceResourceTracker();
			for (const auto& Captured : Snapshot.CopyEntries())
			{
				if (!Captured.bAvailable || !Captured.bEditable || !Captured.bAuthoringVisible) continue;
				TSharedPtr<FJsonValue> Value, Definition;
				if (!UeAgentPropertyValueCodec::ProjectAuthoringValue(Captured.Type.ToJsonObject(), Captured.Value,
					Captured.ValueDefinition, Value, Definition)) continue;
				FUeAgentCanonicalValuePtr Type;
				if (!CanonicalJson::TryFromJsonValue(MakeShared<FJsonValueObject>(Captured.Type.ToJsonObject()), Resources, Type, OutError)) return false;
				FString ElementKind;
				if (Definition && Definition->Type == EJson::Object
					&& Definition->AsObject()->TryGetStringField(TEXT("element_kind"), ElementKind) && ElementKind == TEXT("owned_object"))
					OrderedOwnedSchemas.Add(OutSchema.Num(), Definition->AsObject());
				OutSchema.Add({Captured.TargetIdentity.GetSelector(TEXT("property_path")),
					ReadablePropertyCategory(Captured.CategoryPath), ReadablePropertyTypeName(*Type)});
			}
			return true;
		}

		bool BindReadableOwnedItems(const UE::UAI::StateSemantic::FUeAgentCanonicalValue& Proposed,
			const FJsonObject& Definition, UE::UAI::ContextResource::FUeAgentResourceTracker& Resources,
			TSharedPtr<FJsonValue>& Value, FDiagnostic& Error)
		{
			using namespace UE::UAI::HjsonWorkspace;
			using namespace UE::UAI::StateSemantic;
			if (Value->Type != EJson::Array) return true;
			const auto& Items = Value->AsArray();
			const bool bNeedsBinding = Items.ContainsByPredicate([](const TSharedPtr<FJsonValue>& Item) {
				const TSharedPtr<FJsonObject>* Owned = nullptr;
				return Item && Item->Type == EJson::Object && Item->AsObject()->TryGetObjectField(TEXT("owned_object"), Owned)
					&& !(*Owned)->HasField(TEXT("owner_relative_path")) && ((*Owned)->HasField(TEXT("instance")) || (*Owned)->HasField(TEXT("name")));
			});
			if (!bNeedsBinding) return true;

			const TArray<TSharedPtr<FJsonValue>>* CapturedItems = nullptr;
			if (!Definition.TryGetArrayField(TEXT("items"), CapturedItems)) {
				Error = FDiagnostic::Failure(TEXT("hjson_owned_property_schema_missing"),
					TEXT("The captured owned-item array has no item definitions."), TEXT("hjson_import"));
				Error.SourceKind = TEXT("native_result");
				return false;
			}
			FUeAgentCanonicalValuePtr Captured;
			if (!CanonicalJson::TryFromJsonValue(MakeShared<FJsonValueArray>(*CapturedItems), Resources, Captured, Error)) return false;
			FWorkspaceError Failure;
			TArray<FOrderedOwnedItemBinding> Bindings;
			if (!TryResolveOrderedOwnedItems(Proposed, *Captured, Resources, Bindings, Failure)) {
				Error = Failure.GetDiagnostic();
				return false;
			}
			TArray<TSharedPtr<FJsonValue>> Bound;
			Bound.Reserve(Bindings.Num());
			for (int32 Index = 0; Index < Bindings.Num(); ++Index) Bound.Add(Bindings[Index].BuildReadableApplyItem(Items[Index]));
			Value = MakeShared<FJsonValueArray>(Bound);
			return true;
		}

		TSharedPtr<FJsonValue> CloneValue(const TSharedPtr<FJsonValue>& Value)
		{
			return UeAgentAuthoringState::CloneCanonicalValue(Value);
		}

		UeAgentAuthoringState::FEditablePropertyReadOptions MakeReadOptions(
			const FCapturePolicy& Policy)
		{
			UeAgentAuthoringState::FEditablePropertyReadOptions Result;
			Result.SemanticId = Policy.SemanticId;
			Result.Limits.bShowDefaultsOnly = Policy.bShowDefaultsOnly;
			Result.Limits.bUseInstanceEditability = Policy.bUseInstanceEditability;
			Result.Limits.StructFieldFilter = Policy.StructFieldFilter;
			Result.Limits.AuthorizeProperty = Policy.AuthorizeProperty;
			Result.ExcludedProperties = Policy.ExcludedProperties;
			Result.IncludeProperty = Policy.IncludeProperty;
			Result.BuildSnapshotIdentity = Policy.BuildSnapshotIdentity;
			return Result;
		}

		bool CaptureRequests(
			const TArray<UeAgentAuthoringState::FReadRequest>& Requests,
			UeAgentAuthoringState::FStateSnapshot& OutSnapshot,
			FDiagnostic& OutError)
		{
			UE::UAI::Contract::FDiagnostic RegistrationError;
			if (!UeAgentAuthoringState::RegisterBuiltInStateAccessors(
					UeAgentAuthoringState::FStateAccessorRegistry::Get(), RegistrationError))
			{
				OutError = RegistrationError;
				return false;
			}
			UeAgentAuthoringState::FCaptureResult Capture;
			if (!UeAgentAuthoringState::CaptureSnapshot(Requests, Capture))
			{
				OutError = Capture.Diagnostic;
				return false;
			}
			OutSnapshot = MoveTemp(Capture.Snapshot);
			OutError.Reset();
			return true;
		}

		void SetApplyFailure(FDiagnostic Diagnostic, UeAgentAuthoringDirectApply::FResult& OutResult)
		{
			OutResult = UeAgentAuthoringDirectApply::FResult();
			OutResult.Status = TEXT("failed");
			OutResult.Phase = TEXT("decode");
			OutResult.Diagnostic = MoveTemp(Diagnostic);
		}
	}

	TSharedPtr<FJsonObject> BuildPropertyDescriptor(
		const UeAgentAuthoringState::FStateEntry& Entry,
		const FProjectionPolicy& Policy)
	{
		if (Policy.bRequireEditable && (!Entry.bAvailable || !Entry.bEditable))
		{
			return nullptr;
		}
		const FString PropertyPath = Entry.TargetIdentity.GetSelector(TEXT("property_path"));
		TSharedPtr<FJsonValue> ProjectedValue = Entry.Value, ProjectedDefinition = Entry.ValueDefinition;
		if (Policy.bRequireEditable && !UeAgentPropertyValueCodec::ProjectAuthoringValue(
			Entry.Type.ToJsonObject(), Entry.Value, Entry.ValueDefinition, ProjectedValue, ProjectedDefinition)) return nullptr;

		TSharedRef<FJsonObject> Descriptor = MakeShared<FJsonObject>();
		if (!PropertyPath.IsEmpty()) Descriptor->SetStringField(TEXT("property_path"), PropertyPath);
		if (Policy.bIncludeDisplayName
			&& !Entry.DisplayName.IsEmpty() && Entry.DisplayName != PropertyPath)
		{
			Descriptor->SetStringField(TEXT("display_name"), Entry.DisplayName);
		}
		if (Policy.bIncludeTooltip && !Entry.Tooltip.IsEmpty())
		{
			Descriptor->SetStringField(TEXT("tooltip"), Entry.Tooltip);
		}
		if (TSharedPtr<FJsonObject> Type = Entry.Type.ToJsonObject(); Type.IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Type->Values)
			{
				Descriptor->SetField(Pair.Key, CloneValue(Pair.Value));
			}
		}

		if (Policy.bIncludeAvailability)
		{
			TSharedRef<FJsonObject> Availability = MakeShared<FJsonObject>();
			Availability->SetBoolField(TEXT("available"), Entry.bAvailable);
			Availability->SetBoolField(TEXT("editable"), Entry.bEditable);
			if (!Entry.AvailabilityReason.IsEmpty())
			{
				Availability->SetStringField(TEXT("reason"), Entry.AvailabilityReason);
			}
			Descriptor->SetObjectField(TEXT("availability"), Availability);
		}

		if (!Entry.Constraints.IsEmpty())
		{
			TSharedRef<FJsonObject> Constraints = MakeShared<FJsonObject>();
			TArray<FString> Keys;
			Entry.Constraints.GenerateKeyArray(Keys);
			Keys.Sort();
			for (const FString& Key : Keys)
			{
				Constraints->SetStringField(Key, Entry.Constraints.FindChecked(Key));
			}
			Descriptor->SetObjectField(TEXT("constraints"), Constraints);
		}
		if (Policy.bIncludeCategoryPath && !Entry.CategoryPath.IsEmpty())
		{
			TArray<TSharedPtr<FJsonValue>> Category;
			for (const FString& Part : Entry.CategoryPath)
			{
				Category.Add(MakeShared<FJsonValueString>(Part));
			}
			Descriptor->SetArrayField(TEXT("category_path"), Category);
		}
		if (Entry.CandidatesHeader.IsValid())
		{
			Descriptor->SetField(TEXT("candidates_header"), CloneValue(Entry.CandidatesHeader));
		}
		if (Entry.Candidates.IsValid())
		{
			Descriptor->SetField(TEXT("candidates"), CloneValue(Entry.Candidates));
		}
		const bool bStructured = Entry.bAvailable && Entry.Value.IsValid();
		if (bStructured)
		{
			auto Value = CloneValue(ProjectedValue);
			const auto ComparisonDefinition = Entry.ComparisonValueDefinition && Entry.ComparisonValueDefinition->Type == EJson::Object
				? Entry.ComparisonValueDefinition->AsObject() : nullptr;
			UeAgentAuthoringEncodedTypes::FScope::Record(Value, Entry.Type,
				ComparisonDefinition ? ComparisonDefinition : ProjectedDefinition && ProjectedDefinition->Type == EJson::Object ? ProjectedDefinition->AsObject() : nullptr);
			Descriptor->SetField(TEXT("value"), Value);
		}
		if ((!bStructured || Policy.bPreserveValueText) && !Entry.ValueText.IsEmpty())
		{
			Descriptor->SetStringField(TEXT("value_text"), Entry.ValueText);
		}
		return Descriptor;
	}

	bool BuildPropertiesDocument(
		const TArray<UeAgentAuthoringState::FStateEntry>& Entries,
		const FProjectionPolicy& Policy,
		TSharedPtr<FJsonObject>& OutDocument,
		FDiagnostic& OutError)
	{
		TArray<UeAgentAuthoringState::FStateEntry> Sorted = Entries;
		Sorted.Sort([](
			const UeAgentAuthoringState::FStateEntry& Left,
			const UeAgentAuthoringState::FStateEntry& Right)
		{
			return Left.TargetIdentity.GetSelector(TEXT("property_path"))
				< Right.TargetIdentity.GetSelector(TEXT("property_path"));
		});
		TArray<TSharedPtr<FJsonValue>> Properties;
		for (const UeAgentAuthoringState::FStateEntry& Entry : Sorted)
		{
			if (TSharedPtr<FJsonObject> Descriptor = BuildPropertyDescriptor(Entry, Policy))
			{
				Properties.Add(MakeShared<FJsonValueObject>(Descriptor));
			}
		}
		OutDocument = MakeShared<FJsonObject>();
		OutDocument->SetStringField(TEXT("schema"), TEXT("uai.uobject_properties.v2"));
		OutDocument->SetStringField(TEXT("profile"), Policy.Profile);
		OutDocument->SetNumberField(TEXT("property_count"), Properties.Num());
		OutDocument->SetArrayField(TEXT("properties"), Properties);
		OutError.Reset();
		return true;
	}

	bool ProjectReadableProperties(const UeAgentAuthoringState::FStateSnapshot& Snapshot,
		TSharedPtr<FJsonObject>& OutCategories, FDiagnostic& OutError, TSharedPtr<FJsonObject>* OutDefinitions)
	{
		using namespace UE::UAI::HjsonWorkspace;
		TSharedPtr<FJsonObject> Definitions;
		FWorkspaceError Error;
		// Existing-instance readers use native scope. A caller requesting definitions
		// needs the self-contained readable type catalog, including enum members.
		if (!TryProjectReflectedRecord(Snapshot, OutCategories, Definitions, Error, true, OutDefinitions == nullptr))
		{ OutError = Error.GetDiagnostic(); return false; }
		if (OutDefinitions) *OutDefinitions = Definitions;
		return true;
	}

	bool DecodeReadableProperties(const UeAgentAuthoringState::FStateSnapshot& Snapshot,
		const TSharedPtr<FJsonValue>& Categories, TArray<TSharedPtr<FJsonValue>>& OutDescriptors, FDiagnostic& OutError)
	{
		return DecodeReadableProperties(Snapshot, Categories, OutDescriptors, OutError, nullptr);
	}

	bool DecodeReadableProperties(const UeAgentAuthoringState::FStateSnapshot& Snapshot,
		const TSharedPtr<FJsonValue>& Categories, TArray<TSharedPtr<FJsonValue>>& OutDescriptors, FDiagnostic& OutError,
		TSharedPtr<FJsonObject>* BoundRows)
	{
		using namespace UE::UAI::HjsonWorkspace;
		using namespace UE::UAI::StateSemantic;
		TArray<FReadablePropertySchema> Schema;
		TMap<int32, TSharedPtr<FJsonObject>> OrderedOwnedSchemas;
		if (!BuildReadableSchema(Snapshot, Schema, OrderedOwnedSchemas, OutError)) return false;
		auto Resources = MakeSemanticWorkspaceResourceTracker();
		FUeAgentCanonicalValuePtr Canonical;
		if (!CanonicalJson::TryFromJsonValue(Categories, Resources, Canonical, OutError)) return false;
		TArray<FResolvedReadableProperty> Rows;
		FUeAgentStateSemanticError Error;
		if (!TryReadReadablePropertyRows(*Canonical, Schema, Rows, Error))
		{ OutError = Error; return false; }
		if (BoundRows) *BoundRows = MakeShared<FJsonObject>();
		for (const auto& Row : Rows)
		{
			const auto& Item = Schema[Row.SchemaIndex];
			auto Descriptor = MakeShared<FJsonObject>();
			Descriptor->SetStringField(TEXT("property_path"), Item.Name);
			// Preserve the source value identity for deferred text identity edits.
			const auto& Bound = Categories->AsObject()->GetObjectField(Item.Category)->GetObjectField(Item.Name);
			auto Value = Bound->TryGetField(TEXT("value"));
			if (const auto* Definition = OrderedOwnedSchemas.Find(Row.SchemaIndex))
				if (!BindReadableOwnedItems(*Row.Value, **Definition, Resources, Value, OutError)) {
					OutError.PropertyPath = Item.Name;
					return false;
				}
			Descriptor->SetField(TEXT("value"), Value);
			if (BoundRows) {
				if (!(*BoundRows)->HasField(Item.Category)) (*BoundRows)->SetObjectField(Item.Category, MakeShared<FJsonObject>());
				(*BoundRows)->GetObjectField(Item.Category)->SetObjectField(Item.Name, Bound);
			}
			OutDescriptors.Add(MakeShared<FJsonValueObject>(Descriptor));
		}
		return true;
	}

	void LocateReadablePropertyFailure(FDiagnostic& Error, const TSharedPtr<FJsonValue>& Categories,
		const FString& DocumentPointer)
	{
		LocateReadablePropertyFailure(Error, Categories, DocumentPointer, {});
	}

	void LocateReadablePropertyFailure(FDiagnostic& Error, const TSharedPtr<FJsonValue>& Categories,
		const FString& DocumentPointer, const TFunction<void(FDiagnostic&, bool)>& OnLocated)
	{
		auto& Diagnostic = Error;
		if (Diagnostic.Stage.IsEmpty()) Diagnostic.Stage = TEXT("document_apply");
		auto PointerFor = [&](const FDiagnostic& Issue)
		{
			const FString& Property = Issue.PropertyPath;
			FString Selected;
			bool bSelectedExact = false;
			int32 Longest = -1;
			if (Categories.IsValid() && Categories->Type == EJson::Object)
				for (const auto& Category : Categories->AsObject()->Values)
				{
					if (!Category.Value.IsValid() || Category.Value->Type != EJson::Object) continue;
					for (const auto& Row : Category.Value->AsObject()->Values)
					{
						if (Property != Row.Key && !Property.StartsWith(Row.Key + TEXT("."))
							&& !Property.StartsWith(Row.Key + TEXT("["))) continue;
					const FString Base = DocumentPointer + TEXT("/")
							+ Category.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"))
							+ TEXT("/") + Row.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")) + TEXT("/value");
						const TSharedPtr<FJsonValue> Value = Row.Value && Row.Value->Type == EJson::Object
							? Row.Value->AsObject()->TryGetField(TEXT("value")) : nullptr;
						bool bExact = false;
						const FString Pointer = RefineReadableValuePointer(Value, Base, Property.RightChop(Row.Key.Len()), Issue, bExact);
						if (Row.Key.Len() > Longest) { Longest = Row.Key.Len(); Selected = Pointer; bSelectedExact = bExact; }
						else if (Row.Key.Len() == Longest && Selected != Pointer) { Selected.Reset(); bSelectedExact = false; }
					}
				}
			return TPair<FString, bool>(Selected.IsEmpty() ? DocumentPointer : Selected, !Selected.IsEmpty() && bSelectedExact);
		};
		TFunction<void(FDiagnostic&)> Locate = [&](FDiagnostic& Issue)
		{
			for (auto& Child : Issue.Diagnostics) Locate(Child);
			if (Issue.PublicCause)
			{
				auto Cause = MakeShared<FDiagnostic>(*Issue.PublicCause);
				Locate(*Cause);
				Issue.PublicCause = MoveTemp(Cause);
			}
			if (Issue.SourceKind == TEXT("native_result") || Issue.SourceKind == TEXT("command_input")) return;
			if (!Issue.Source.SourceFile.IsEmpty() && !Issue.Source.SourceFile.StartsWith(TEXT("inline://"))) return;
			// Descriptor-bound unit coordinates already have their own authoritative map.
			if (!Issue.Source.SourceUnit.IsEmpty() && !Issue.Source.InternalJsonPath.IsEmpty()) return;
			const auto Location = PointerFor(Issue);
			Issue.Source.JsonPath = Location.Key;
			// Unbound codec paths are relative to value, not to the containing document.
			Issue.Source.InternalJsonPath = Location.Key;
			if (OnLocated) OnLocated(Issue, Location.Value);
		};
		Locate(Diagnostic);
	}

	bool CaptureObjectProperties(
		const UObject* Object,
		const FCapturePolicy& Policy,
		UeAgentAuthoringState::FStateSnapshot& OutSnapshot,
		TSharedPtr<FJsonObject>& OutDocument,
		FDiagnostic& OutError)
	{
		if (!CaptureObjectSnapshot(Object, Policy, OutSnapshot, OutError)) return false;
		return BuildPropertiesDocument(
			OutSnapshot.CopyEntries(), FProjectionPolicy(), OutDocument, OutError);
	}

	bool CaptureObjectSnapshot(const UObject* Object, const FCapturePolicy& Policy,
		UeAgentAuthoringState::FStateSnapshot& OutSnapshot, FDiagnostic& OutError)
	{
		TArray<UeAgentAuthoringState::FReadRequest> Requests;
		if (!UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(
				Object, MakeReadOptions(Policy), Requests, OutError)) return false;
		return CaptureRequests(Requests, OutSnapshot, OutError);
	}

	bool CaptureStructProperties(
		UStruct* StructType,
		const void* StructData,
		const FString& RootOwnerPath,
		const UObject* NotificationOwner,
		const FCapturePolicy& Policy,
		UeAgentAuthoringState::FStateSnapshot& OutSnapshot,
		TSharedPtr<FJsonObject>& OutDocument,
		FDiagnostic& OutError)
	{
		if (!CaptureStructSnapshot(StructType, StructData, RootOwnerPath, NotificationOwner,
			Policy, OutSnapshot, OutError)) return false;
		return BuildPropertiesDocument(
			OutSnapshot.CopyEntries(), FProjectionPolicy(), OutDocument, OutError);
	}

	bool CaptureStructSnapshot(UStruct* StructType, const void* StructData, const FString& RootOwnerPath,
		const UObject* NotificationOwner, const FCapturePolicy& Policy,
		UeAgentAuthoringState::FStateSnapshot& OutSnapshot, FDiagnostic& OutError)
	{
		TArray<UeAgentAuthoringState::FReadRequest> Requests;
		if (!UeAgentAuthoringState::AppendEditableStructPropertyReadRequests(
				StructType, StructData, RootOwnerPath, NotificationOwner,
				MakeReadOptions(Policy), Requests, OutError)) return false;
		return CaptureRequests(Requests, OutSnapshot, OutError);
	}

	bool AppendStructPropertyReadRequests(
		UStruct* StructType,
		const void* StructData,
		const FString& RootOwnerPath,
		const UObject* NotificationOwner,
		const FCapturePolicy& Policy,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		FDiagnostic& OutError)
	{
		return UeAgentAuthoringState::AppendEditableStructPropertyReadRequests(
			StructType, StructData, RootOwnerPath, NotificationOwner,
			MakeReadOptions(Policy), OutRequests, OutError);
	}

	bool CaptureObjectProperty(
		const UObject* Object,
		const FString& PropertyPath,
		const FCapturePolicy& Policy,
		UeAgentAuthoringState::FStateEntry& OutEntry,
		TSharedPtr<FJsonObject>& OutDescriptor,
		FDiagnostic& OutError)
	{
		UeAgentAuthoringState::FReadRequest Request;
		UeAgentAuthoringTargetLocator::FResolveResult TargetResult;
		if (!UeAgentAuthoringState::BuildPropertyReadRequest(
				Object, PropertyPath, Policy.SemanticId, Request, TargetResult))
		{
			OutError = TargetResult.Diagnostic;
			return false;
		}
		Request.Limits = MakeReadOptions(Policy).Limits;
		if (Policy.BuildSnapshotIdentity)
		{
			UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
			if (!Policy.BuildSnapshotIdentity(Request.PropertyPath, Identity, OutError)) return false;
			Request.SnapshotIdentityOverride = MoveTemp(Identity);
		}
		UeAgentAuthoringState::FStateSnapshot Snapshot;
		if (!CaptureRequests({ Request }, Snapshot, OutError)) return false;
		const TArray<UeAgentAuthoringState::FStateEntry> Entries = Snapshot.CopyEntries();
		if (Entries.Num() != 1)
		{
			OutError = FDiagnostic::Failure(TEXT("details_single_property_cardinality_invalid"),
				TEXT("Capturing one property did not produce exactly one state entry."), TEXT("property_capture"));
			OutError.PropertyPath = PropertyPath;
			OutError.ExpectedValue = MakeShared<FJsonValueNumber>(1);
			OutError.ActualValue = MakeShared<FJsonValueNumber>(Entries.Num());
			OutError.SourceKind = TEXT("native_result");
			return false;
		}
		OutEntry = Entries[0];
		OutDescriptor = BuildPropertyDescriptor(OutEntry, FProjectionPolicy());
		if (!OutDescriptor.IsValid())
		{
			OutError = FDiagnostic::Failure(TEXT("details_single_property_descriptor_unavailable"),
				TEXT("The captured property could not be projected to a property descriptor."), TEXT("property_capture"));
			OutError.PropertyPath = PropertyPath;
			OutError.SourceKind = TEXT("native_result");
			return false;
		}
		OutError.Reset();
		return true;
	}

	bool CaptureStructPropertyPaths(
		UStruct* StructType,
		const void* StructData,
		const FString& RootOwnerPath,
		const UObject* NotificationOwner,
		const TArray<FString>& PropertyPaths,
		const FCapturePolicy& Policy,
		TSharedPtr<FJsonObject>& OutDocument,
		FDiagnostic& OutError,
		UeAgentAuthoringState::FStateSnapshot* OutSnapshot)
	{
		TArray<UeAgentAuthoringState::FReadRequest> Requests;
		for (const FString& PropertyPath : PropertyPaths)
		{
			UeAgentAuthoringState::FReadRequest Request;
			UeAgentAuthoringTargetLocator::FResolveResult TargetResult;
			if (!UeAgentAuthoringState::BuildStructPropertyReadRequest(
					StructType, StructData, RootOwnerPath, NotificationOwner,
					PropertyPath, Policy.SemanticId, Request, TargetResult))
			{
				OutError = TargetResult.Diagnostic;
				return false;
			}
			Request.Limits = MakeReadOptions(Policy).Limits;
			Requests.Add(MoveTemp(Request));
		}
		UeAgentAuthoringState::FStateSnapshot Snapshot;
		if (!CaptureRequests(Requests, Snapshot, OutError)) return false;
		if (!BuildPropertiesDocument(
			Snapshot.CopyEntries(), FProjectionPolicy(), OutDocument, OutError)) return false;
		if (OutSnapshot) *OutSnapshot = MoveTemp(Snapshot);
		return true;
	}

	bool CaptureStructDeltaFields(
		UScriptStruct* StructType,
		const void* StructData,
		const void* DefaultData,
		const FString& RootOwnerPath,
		const UObject* NotificationOwner,
		TSharedPtr<FJsonObject>& OutFields,
		FDiagnostic& OutError)
	{
		OutFields = MakeShared<FJsonObject>();
		if (!StructType || !StructData)
		{
			OutError = FDiagnostic::Failure(TEXT("details_struct_delta_input_invalid"),
				!StructType ? TEXT("The struct type required for delta capture is unavailable.")
					: TEXT("The struct data required for delta capture is unavailable."), TEXT("property_capture"), RootOwnerPath);
			OutError.SourceKind = TEXT("native_result");
			return false;
		}
		TArray<FString> Paths;
		for (TFieldIterator<FProperty> It(StructType, EFieldIterationFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			if (!Property || Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated)) continue;
			Paths.Add(Property->GetName());
		}
		Paths.Sort();
		FCapturePolicy Policy;
		TSharedPtr<FJsonObject> Document;
		if (!CaptureStructPropertyPaths(
				StructType, StructData, RootOwnerPath, NotificationOwner,
				Paths, Policy, Document, OutError)) return false;
		const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
		if (!Document->TryGetArrayField(TEXT("properties"), Properties) || !Properties)
		{
			OutError = FDiagnostic::Failure(TEXT("details_struct_delta_properties_missing"),
				TEXT("The captured struct document is missing its properties array."), TEXT("property_capture"), RootOwnerPath);
			OutError.PropertyPath = TEXT("properties");
			OutError.SourceKind = TEXT("native_result");
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Properties)
		{
			const TSharedPtr<FJsonObject> Descriptor = Value.IsValid() && Value->Type == EJson::Object
				? Value->AsObject() : nullptr;
			FString PropertyPath;
			if (!Descriptor.IsValid() || !Descriptor->TryGetStringField(TEXT("property_path"), PropertyPath)) continue;
			if (const TSharedPtr<FJsonValue> CanonicalValue = Descriptor->TryGetField(TEXT("value")); CanonicalValue.IsValid())
			{
				OutFields->SetField(PropertyPath, CloneValue(CanonicalValue));
			}
		}
		OutFields = UeAgentPropertyValueCodec::ProjectNativeDefaultStructFields(
			StructType, StructData, DefaultData, OutFields);
		OutError.Reset();
		return true;
	}

	bool DecodePropertiesDocument(
		const TSharedPtr<FJsonObject>& Document,
		TArray<TSharedPtr<FJsonValue>>& OutDescriptors,
		FDiagnostic& OutError)
	{
		OutDescriptors.Reset();
		FString Schema;
		if (!Document.IsValid() || !Document->TryGetStringField(TEXT("schema"), Schema)
			|| Schema != TEXT("uai.uobject_properties.v2"))
		{
			UeAgentAuthoringDiagnostics::FDiagnostic Diagnostic;
			Diagnostic.Code = TEXT("property_payload_schema_invalid");
			Diagnostic.Stage = TEXT("decode");
			Diagnostic.Source.JsonPath = TEXT("/schema");
			Diagnostic.PublicMessage = TEXT("Expected uai.uobject_properties.v2.");
			Diagnostic.ActualValue = Document ? Document->Values.FindRef(TEXT("schema")) : nullptr;
			Diagnostic.ActualValuePresent = Document && Document->HasField(TEXT("schema"));
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("uai.uobject_properties.v2"));
			OutError = MoveTemp(Diagnostic);
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
		if (!Document->TryGetArrayField(TEXT("properties"), Properties) || !Properties)
		{
			UeAgentAuthoringDiagnostics::FDiagnostic Diagnostic;
			Diagnostic.Code = TEXT("property_payload_invalid");
			Diagnostic.Stage = TEXT("decode");
			Diagnostic.Source.JsonPath = TEXT("/properties");
			Diagnostic.PublicMessage = TEXT("Expected a properties descriptor array.");
			Diagnostic.ActualValue = Document->Values.FindRef(TEXT("properties"));
			Diagnostic.ActualValuePresent = Document->HasField(TEXT("properties"));
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("array"));
			OutError = MoveTemp(Diagnostic);
			return false;
		}
		OutDescriptors.Reserve(Properties->Num());
		UeAgentAuthoringDiagnostics::FDiagnostic Failure;
		for (int32 Index = 0; Index < Properties->Num(); ++Index)
		{
			const TSharedPtr<FJsonValue>& Value = (*Properties)[Index];
			const TSharedPtr<FJsonObject> Descriptor = Value.IsValid() && Value->Type == EJson::Object
				? Value->AsObject() : nullptr;
			const TSharedPtr<FJsonObject> Projected =
				UeAgentAuthoringObjectApply::ProjectDescriptorForApply(Descriptor);
			FString PropertyPath;
			if (Descriptor.IsValid()) Descriptor->TryGetStringField(TEXT("property_path"), PropertyPath);
			if (!Projected.IsValid() || PropertyPath.TrimStartAndEnd().IsEmpty())
			{
				UeAgentAuthoringDiagnostics::FDiagnostic Item;
				Item.Code = TEXT("property_descriptor_invalid");
				Item.Stage = TEXT("decode");
				Item.PropertyPath = PropertyPath;
				Item.Source.JsonPath = FString::Printf(TEXT("/properties/%d"), Index);
				if (Descriptor.IsValid()) Item.Source.JsonPath += PropertyPath.TrimStartAndEnd().IsEmpty()
					? TEXT("/property_path") : TEXT("/value");
				if (!Descriptor)
				{
					Item.PublicMessage = TEXT("A property descriptor must be an object.");
					Item.ActualValue = Value;
					Item.ActualValuePresent = Value.IsValid();
					Item.ExpectedValue = MakeShared<FJsonValueString>(TEXT("object"));
				}
				else if (PropertyPath.TrimStartAndEnd().IsEmpty())
				{
					Item.PublicMessage = TEXT("A property descriptor requires a non-empty property_path string.");
					Item.ActualValue = Descriptor->Values.FindRef(TEXT("property_path"));
					Item.ActualValuePresent = Descriptor->HasField(TEXT("property_path"));
					Item.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty string"));
				}
				else
				{
					Item.PublicMessage = TEXT("A property descriptor is missing its value field.");
					Item.ActualValuePresent = false;
				}
				if (Failure.Code.IsEmpty()) Failure = Item;
				Failure.Diagnostics.Add(MoveTemp(Item));
				continue;
			}
			OutDescriptors.Add(MakeShared<FJsonValueObject>(Projected));
		}
		if (!Failure.Code.IsEmpty())
		{
			OutDescriptors.Reset();
			OutError = MoveTemp(Failure);
			return false;
		}
		OutError.Reset();
		return true;
	}

	bool ProjectPropertyDocumentChanges(const TSharedPtr<FJsonObject>& Captured,
		const TSharedPtr<FJsonObject>& Desired, TSharedPtr<FJsonObject>& OutChanges, FDiagnostic& OutError)
	{
		using namespace UE::UAI::StateSemantic;
		TArray<TSharedPtr<FJsonValue>> Before, After, Changes;
		if (!DecodePropertiesDocument(Captured, Before, OutError)
			|| !DecodePropertiesDocument(Desired, After, OutError)) return false;
		TMap<FString, int32> BeforeByPath;
		for (int32 Index = 0; Index < Before.Num(); ++Index)
			BeforeByPath.Add(Before[Index]->AsObject()->GetStringField(TEXT("property_path")), Index);
		auto Resources = UE::UAI::HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
		for (int32 Index = 0; Index < After.Num(); ++Index)
		{
			const auto& Descriptor = After[Index];
			const auto Object = Descriptor->AsObject();
			if (const auto* OldIndex = BeforeByPath.Find(Object->GetStringField(TEXT("property_path"))))
			{
				FUeAgentCanonicalValuePtr OldValue, NewValue;
				if (!CanonicalJson::TryFromJsonValue(Before[*OldIndex]->AsObject()->TryGetField(TEXT("value")), Resources, OldValue, OutError))
				{
					UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError,
						FString::Printf(TEXT("/properties/%d/value"), *OldIndex), false, true);
					return false;
				}
				const FString ValuePointer = FString::Printf(TEXT("/properties/%d/value"), Index);
				if (!CanonicalJson::TryFromJsonValue(Object->TryGetField(TEXT("value")), Resources, NewValue, OutError))
				{
					UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, ValuePointer, false, true);
					return false;
				}
				FUeAgentSemanticComparison Comparison;
				FUeAgentStateSemanticError Error;
				if (!TryCompareValues(OldValue.ToSharedRef(), NewValue.ToSharedRef(), TEXT("/"), Resources, Comparison, Error))
				{
					OutError = MoveTemp(Error);
					UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, ValuePointer, false, true);
					return false;
				}
				if (Comparison.IsEquivalent()) continue;
			}
			Changes.Add(Descriptor);
		}
		OutChanges = MakeShared<FJsonObject>();
		OutChanges->SetStringField(TEXT("schema"), TEXT("uai.uobject_properties.v2"));
		OutChanges->SetArrayField(TEXT("properties"), Changes);
		return true;
	}

	bool NormalizePropertyPayload(const TSharedPtr<FJsonValue>& Payload,
		TSharedPtr<FJsonObject>& OutDocument, FDiagnostic& OutError)
	{
		OutDocument.Reset();
		const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
		const bool bArray = Payload && Payload->Type == EJson::Array;
		if (bArray) Properties = &Payload->AsArray();
		else if (Payload && Payload->Type == EJson::Object)
		{
			const auto Object = Payload->AsObject();
			if (Object->HasField(TEXT("schema")))
			{
				TArray<TSharedPtr<FJsonValue>> Descriptors;
				if (!DecodePropertiesDocument(Object, Descriptors, OutError)) return false;
				OutDocument = Object;
				return true;
			}
			Object->TryGetArrayField(TEXT("properties"), Properties);
		}
		if (!Properties)
		{
			UeAgentAuthoringDiagnostics::FDiagnostic Diagnostic;
			Diagnostic.Code = TEXT("property_payload_invalid");
			Diagnostic.Stage = TEXT("decode");
			Diagnostic.PublicMessage = TEXT("Expected properties[] descriptors or a uai.uobject_properties.v2 document; category rows belong to the exported property file.");
			OutError = MoveTemp(Diagnostic);
			return false;
		}
		auto Document = MakeShared<FJsonObject>();
		Document->SetStringField(TEXT("schema"), TEXT("uai.uobject_properties.v2"));
		Document->SetArrayField(TEXT("properties"), *Properties);
		TArray<TSharedPtr<FJsonValue>> Descriptors;
		if (!DecodePropertiesDocument(Document, Descriptors, OutError))
		{
			if (bArray)
			{
				OutError.Source.JsonPath.RemoveFromStart(TEXT("/properties"));
				for (auto& Item : OutError.Diagnostics)
					Item.Source.JsonPath.RemoveFromStart(TEXT("/properties"));
			}
			return false;
		}
		OutDocument = MoveTemp(Document);
		OutError.Reset();
		return true;
	}

	bool ReadPropertyPayload(const TSharedPtr<const FJsonObject>& Owner, const TCHAR* Field,
		TSharedPtr<FJsonObject>& OutDocument, FDiagnostic& OutError)
	{
		OutDocument.Reset();
		OutError.Reset();
		if (!Owner || !Owner->HasField(Field)) return true;
		return NormalizePropertyPayload(Owner->TryGetField(Field), OutDocument, OutError);
	}

	bool ValidatePropertyPayload(const UE::UAI::HjsonWorkspace::FHjsonNode& Payload,
		UE::UAI::HjsonWorkspace::FWorkspaceError& OutError)
	{
		using namespace UE::UAI::HjsonWorkspace;
		auto Resources = MakeSemanticWorkspaceResourceTracker();
		TSharedPtr<FJsonValue> Value;
		if (!TryProjectHjsonNodeToJsonValue(Payload, Resources, Value, OutError)) return false;
		TSharedPtr<FJsonObject> Document;
		FDiagnostic Diagnostic;
		if (NormalizePropertyPayload(Value, Document, Diagnostic)) return true;
		{
			auto Bind = [&Payload](UeAgentAuthoringDiagnostics::FDiagnostic& Item)
			{
				const FString Pointer = Item.Source.JsonPath;
				TArray<FString> Parts;
				Pointer.ParseIntoArray(Parts, TEXT("/"), true);
				const FHjsonNode* Node = &Payload;
				for (FString Part : Parts)
				{
					Part.ReplaceInline(TEXT("~1"), TEXT("/")); Part.ReplaceInline(TEXT("~0"), TEXT("~"));
					const FHjsonNode* Child = nullptr;
					if (Node->GetKind() == EHjsonNodeKind::Object) Child = Node->Get(Part);
					else if (Node->GetKind() == EHjsonNodeKind::Array && Part.IsNumeric())
					{ const int32 Index = FCString::Atoi(*Part); if (Node->GetItems().IsValidIndex(Index)) Child = &Node->GetItems()[Index].Get(); }
					if (!Child) break;
					Node = Child;
				}
				const auto& Source = Node->GetProvenance();
				Item.Source.SourceFile = Source.GetSourceUnit();
				Item.Source.JsonPath = Payload.GetProvenance().GetJsonPointer() + Pointer;
				Item.Source.Line = Source.GetLine();
				Item.Source.Column = Source.GetColumn();
			};
			for (auto& Item : Diagnostic.Diagnostics) Bind(Item);
			Bind(Diagnostic);
			OutError.SetFailure(MoveTemp(Diagnostic), Payload.GetProvenance().GetSourceUnit());
		}
		OutError.Locate(Payload.GetProvenance());
		return false;
	}

	static void LocatePropertyDocumentFailure(FDiagnostic& Diagnostic,
		const TMap<FString, UeAgentAuthoringDiagnostics::FSourceLocation>& Sources)
	{
		for (auto& Child : Diagnostic.Diagnostics) LocatePropertyDocumentFailure(Child, Sources);
		if (Diagnostic.PublicCause)
		{
			auto Cause = MakeShared<FDiagnostic>(*Diagnostic.PublicCause);
			LocatePropertyDocumentFailure(*Cause, Sources);
			Diagnostic.PublicCause = MoveTemp(Cause);
		}
		if (!Diagnostic.Source.SourceFile.IsEmpty() || Diagnostic.Source.bCommandInput || Diagnostic.Source.JsonPath.IsEmpty()) return;
		auto Source = UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(Sources, Diagnostic.Source.JsonPath);
		if (!Source.SourceFile.IsEmpty() || Source.bCommandInput)
		{
			Diagnostic.Source = MoveTemp(Source);
			Diagnostic.SourceKind = Diagnostic.Source.SourceFile.IsEmpty() ? TEXT("command_input")
				: Diagnostic.Source.InternalJsonPath == Diagnostic.Source.JsonPath ? TEXT("authored_value") : TEXT("related_input");
		}
	}

	bool ApplyObjectProperties(
		UObject* Object,
		const TSharedPtr<FJsonObject>& Document,
		const UeAgentAuthoringObjectApply::FOptions& Options,
		UeAgentAuthoringDirectApply::FResult& OutResult)
	{
		using UeAgentAuthoringDiagnostics::FScopedSourceLocations;
		const auto SourceLocations = Document ? FScopedSourceLocations::CaptureProjection(*Document)
			: TMap<FString, UeAgentAuthoringDiagnostics::FSourceLocation>();
		TArray<TSharedPtr<FJsonValue>> Descriptors;
		FDiagnostic DecodeError;
		if (!DecodePropertiesDocument(Document, Descriptors, DecodeError))
		{
			LocatePropertyDocumentFailure(DecodeError, SourceLocations);
			SetApplyFailure(MoveTemp(DecodeError), OutResult);
			return false;
		}
		if (Descriptors.IsEmpty())
		{
			OutResult = UeAgentAuthoringDirectApply::FResult();
			OutResult.Status = TEXT("applied");
			OutResult.Phase = TEXT("complete");
			return true;
		}
		// Descriptor projection deep-copies values but retains their JSON paths.
		// Carry the original mapping across that copy before the direct apply projects again.
		TSharedPtr<FJsonObject> ProjectedDocument = MakeShared<FJsonObject>();
		ProjectedDocument->SetArrayField(TEXT("properties"), Descriptors);
		FScopedSourceLocations Sources(ProjectedDocument.Get(), [&SourceLocations](const FString& Pointer)
			{ return FScopedSourceLocations::ResolveProjection(SourceLocations, Pointer); });
		if (!UeAgentAuthoringDirectApply::ApplyObjectPatch(Object, Descriptors, Options, OutResult))
		{
			return false;
		}
		return true;
	}

	bool ApplyStructProperties(
		UStruct* StructType,
		void* StructData,
		UObject* NotificationOwner,
		const TSharedPtr<FJsonObject>& Document,
		const UeAgentAuthoringObjectApply::FOptions& Options,
		UeAgentAuthoringDirectApply::FResult& OutResult)
	{
		using UeAgentAuthoringDiagnostics::FScopedSourceLocations;
		const auto SourceLocations = Document ? FScopedSourceLocations::CaptureProjection(*Document)
			: TMap<FString, UeAgentAuthoringDiagnostics::FSourceLocation>();
		TArray<TSharedPtr<FJsonValue>> Descriptors;
		FDiagnostic DecodeError;
		if (!DecodePropertiesDocument(Document, Descriptors, DecodeError))
		{
			LocatePropertyDocumentFailure(DecodeError, SourceLocations);
			SetApplyFailure(MoveTemp(DecodeError), OutResult);
			return false;
		}
		if (Descriptors.IsEmpty())
		{
			OutResult = UeAgentAuthoringDirectApply::FResult();
			OutResult.Status = TEXT("applied");
			OutResult.Phase = TEXT("complete");
			return true;
		}
		TSharedPtr<FJsonObject> ProjectedDocument = MakeShared<FJsonObject>();
		ProjectedDocument->SetArrayField(TEXT("properties"), Descriptors);
		FScopedSourceLocations Sources(ProjectedDocument.Get(), [&SourceLocations](const FString& Pointer)
			{ return FScopedSourceLocations::ResolveProjection(SourceLocations, Pointer); });
		if (!UeAgentAuthoringDirectApply::ApplyStructPatch(
				StructType, StructData, NotificationOwner, Descriptors, Options, OutResult))
		{
			return false;
		}
		return true;
	}

	bool ApplyScopedProperties(
		const TMap<FString, TSharedPtr<FJsonObject>>& PropertiesByOwner,
		const TArray<UeAgentAuthoringState::FReadRequest>& Requests,
		const FString& PropertySemanticId, const FString& OwnerSelector, const FString& ErrorPrefix,
		TFunctionRef<UeAgentAuthoringObjectApply::FOptions(const UeAgentAuthoringState::FReadRequest&)> OptionsForTarget,
		FDiagnostic& OutError, UPackage* TextIdentityPackage, const TArray<FString>& OwnerApplyOrder,
		TArray<FScopedPropertyIntent>* OutIntents)
	{
		OutError.Reset();
		struct FPropertyGroup
		{
			const UeAgentAuthoringState::FReadRequest* Binding = nullptr;
			TArray<TSharedPtr<FJsonValue>> Descriptors;
			TArray<TSharedPtr<FJsonObject>> DocumentDescriptors;
			UeAgentAuthoringObjectApply::FPreparedApply Prepared;
		};
		TArray<FPropertyGroup> Groups;
		TMap<FString, TMap<FString, const UeAgentAuthoringState::FReadRequest*>> BindingsByOwner;
		for (const auto& Request : Requests)
		{
			if (Request.SemanticId != PropertySemanticId || !Request.SnapshotIdentityOverride.IsSet()) continue;
			const auto& Identity = Request.SnapshotIdentityOverride.GetValue();
			auto& Paths = BindingsByOwner.FindOrAdd(Identity.GetSelector(OwnerSelector));
			const FString Path = Identity.GetSelector(TEXT("property_path"));
			if (!Paths.Contains(Path)) Paths.Add(Path, &Request);
		}
		TMap<const void*, TArray<int32>> GroupsByRoot;
		TSet<FString> Seen;
		TArray<FString> OwnerKeys = OwnerApplyOrder;
		if (OwnerKeys.IsEmpty()) PropertiesByOwner.GetKeys(OwnerKeys);
		TSet<FString> OrderedOwners;
		for (const FString& Key : OwnerKeys)
		{
			if (!PropertiesByOwner.Contains(Key) || OrderedOwners.Contains(Key))
			{
				OutError = FDiagnostic::Failure(ErrorPrefix + TEXT("_property_owner_order_invalid"),
					PropertiesByOwner.Contains(Key) ? TEXT("The native property owner occurs more than once in the apply order.")
						: TEXT("The apply order names a native owner without a property document."), TEXT("property_prepare"));
				OutError.SubjectId = Key;
				OutError.SourceKind = TEXT("native_result");
				return false;
			}
			OrderedOwners.Add(Key);
		}
		if (OrderedOwners.Num() != PropertiesByOwner.Num())
		{
			OutError = FDiagnostic::Failure(ErrorPrefix + TEXT("_property_owner_order_incomplete"),
				TEXT("The apply order does not include every native property owner."), TEXT("property_prepare"));
			OutError.ExpectedValue = MakeShared<FJsonValueNumber>(PropertiesByOwner.Num());
			OutError.ActualValue = MakeShared<FJsonValueNumber>(OrderedOwners.Num());
			OutError.SourceKind = TEXT("native_result");
			return false;
		}
		for (const FString& OwnerKey : OwnerKeys)
		{
			const auto& PropertyDocument = PropertiesByOwner.FindChecked(OwnerKey);
			TArray<TSharedPtr<FJsonValue>> Descriptors;
			if (!UeAgentDetailsAuthoringService::DecodePropertiesDocument(PropertyDocument, Descriptors, OutError)) return false;
			for (const auto& Descriptor : Descriptors)
			{
				FString Path;
				if (!Descriptor.IsValid() || Descriptor->Type != EJson::Object
					|| !Descriptor->AsObject()->TryGetStringField(TEXT("property_path"), Path))
				{
					OutError = FDiagnostic::Failure(ErrorPrefix + TEXT("_property_path_invalid"),
						TEXT("A property descriptor requires a string property_path."), TEXT("property_prepare"));
					OutError.ActualValue = Descriptor && Descriptor->Type == EJson::Object
						? Descriptor->AsObject()->TryGetField(TEXT("property_path")) : Descriptor;
					OutError.ActualValuePresent = OutError.ActualValue.IsValid();
					OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("string property_path"));
					if (Descriptor && Descriptor->Type == EJson::Object)
						UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Descriptor->AsObject(), TEXT("property_path"));
					return false;
				}
				auto FailProperty = [&](const FString& Code, const FString& Reason) {
					OutError = FDiagnostic::Failure(Code, Reason, TEXT("property_prepare"));
					OutError.PropertyPath = Path;
					OutError.ActualValue = MakeShared<FJsonValueString>(Path);
					OutError.SubjectId = OwnerKey;
					UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Descriptor->AsObject(), TEXT("property_path"));
					return false;
				};
				const auto* Paths = BindingsByOwner.Find(OwnerKey);
				const auto* Binding = Paths ? Paths->FindRef(Path) : nullptr;
				if (!Binding) return FailProperty(ErrorPrefix + TEXT("_property_binding_missing"),
					TEXT("The property path has no binding in the selected native owner."));
				const FString Key = Binding->SnapshotIdentityOverride->CanonicalKey;
				if (Seen.Contains(Key)) return FailProperty(ErrorPrefix + TEXT("_property_duplicate"),
					TEXT("The same native property is addressed more than once in this edit."));
				Seen.Add(Key);
				const void* Root = Binding->RootStruct ? Binding->RootData : Binding->RootObject;
				auto& RootGroups = GroupsByRoot.FindOrAdd(Root);
				int32 GroupIndex = INDEX_NONE;
				for (const int32 Index : RootGroups)
				{
					const auto& Candidate = *Groups[Index].Binding;
					if (Candidate.RootObject == Binding->RootObject && Candidate.RootStruct == Binding->RootStruct
						&& Candidate.RootData == Binding->RootData) { GroupIndex = Index; break; }
				}
				if (GroupIndex == INDEX_NONE)
				{
					GroupIndex = Groups.AddDefaulted();
					Groups[GroupIndex].Binding = Binding;
					RootGroups.Add(GroupIndex);
				}
				auto* Group = &Groups[GroupIndex];
				const auto NativeDescriptor = UeAgentAuthoringObjectApply::ProjectDescriptorForApply(Descriptor->AsObject());
				NativeDescriptor->SetStringField(TEXT("property_path"), Binding->PropertyPath);
				Group->Descriptors.Add(MakeShared<FJsonValueObject>(NativeDescriptor));
				const auto* Original = PropertyDocument->GetArrayField(TEXT("properties")).FindByPredicate([&](const auto& Value) {
					return Value->AsObject()->GetStringField(TEXT("property_path")) == Path;
				});
				if (!Original) return FailProperty(ErrorPrefix + TEXT("_property_descriptor_missing"),
					TEXT("The prepared property has no matching descriptor in the source document."));
				Group->DocumentDescriptors.Add((*Original)->AsObject());
			}
		}
		// Resolve and decode every property before the first mutation. Structure is
		// already fixed by the domain caller; no display-path parsing chooses owners.
		for (auto& Group : Groups)
		{
			const auto& Binding = *Group.Binding;
			const auto Options = OptionsForTarget(Binding);
			UeAgentAuthoringObjectApply::FFailure Failure;
			const bool bPrepared = Binding.RootStruct
				? UeAgentAuthoringObjectApply::PrepareStruct(Binding.RootStruct, const_cast<void*>(Binding.RootData),
					const_cast<UObject*>(Binding.NotificationOwner), Group.Descriptors, Options, Group.Prepared, Failure)
				: UeAgentAuthoringObjectApply::PrepareObject(const_cast<UObject*>(Binding.RootObject),
					Group.Descriptors, Options, Group.Prepared, Failure);
			if (!bPrepared) { OutError = Failure.Diagnostic; return false; }
		}
		for (auto& Group : Groups)
		{
			if (TextIdentityPackage) Group.Prepared.SetTextIdentityPackage(TextIdentityPackage);
			UeAgentAuthoringObjectApply::FApplyResult Result;
			if (!UeAgentAuthoringObjectApply::ApplyPrepared(Group.Prepared, Result))
			{ OutError = Result.Diagnostic; return false; }
			if (TextIdentityPackage && Group.Prepared.HasTextSourceEdits())
			{
				const auto Resolved = Group.Prepared.GetResolvedPropertyDescriptors();
				if (Resolved.Num() != Group.DocumentDescriptors.Num())
				{
					OutError = FDiagnostic::Failure(ErrorPrefix + TEXT("_resolved_property_count_mismatch"),
						TEXT("The resolved property count differs from the descriptors prepared for text identity updates."), TEXT("property_apply"));
					OutError.ExpectedValue = MakeShared<FJsonValueNumber>(Group.DocumentDescriptors.Num());
					OutError.ActualValue = MakeShared<FJsonValueNumber>(Resolved.Num());
					OutError.SourceKind = TEXT("native_result");
					return false;
				}
				for (int32 Index = 0; Index < Resolved.Num(); ++Index)
					Group.DocumentDescriptors[Index]->SetField(TEXT("value"),
						UeAgentAuthoringState::CloneCanonicalValue(Resolved[Index]->AsObject()->TryGetField(TEXT("value"))));
			}
			if (OutIntents)
			{
				FScopedPropertyIntent Intent;
				Intent.SemanticId = PropertySemanticId;
				Intent.OwnerSelector = OwnerSelector;
				Intent.OwnerKey = Group.Binding->SnapshotIdentityOverride->GetSelector(OwnerSelector);
				Intent.PropertyPath = Group.Binding->SnapshotIdentityOverride->GetSelector(TEXT("property_path"));
				UeAgentAuthoringObjectApply::FFailure Failure;
				if (!Group.Prepared.CaptureIntent(Intent.Intent, Failure))
				{ OutError = Failure.Diagnostic; return false; }
				OutIntents->Add(MoveTemp(Intent));
			}
		}
		return true;
	}

	void AppendScopedObjectPropertyIntents(
		const TArray<UeAgentAuthoringObjectApply::FPropertyIntent>& Intents,
		const FString& SemanticId, const FString& OwnerSelector, const FString& OwnerKey,
		TArray<FScopedPropertyIntent>& OutIntents, const TMap<FString, FString>& OwnerContext)
	{
		for (const auto& PropertyIntent : Intents)
		{
			if (PropertyIntent.IsEmpty()) continue;
			FScopedPropertyIntent Intent;
			Intent.SemanticId = SemanticId;
			Intent.OwnerSelector = OwnerSelector;
			Intent.OwnerKey = OwnerKey;
			Intent.OwnerContext = OwnerContext;
			Intent.Intent = PropertyIntent;
			OutIntents.Add(MoveTemp(Intent));
		}
	}

	bool VerifyScopedPropertyIntents(const TArray<FScopedPropertyIntent>& Intents,
		const TArray<UeAgentAuthoringState::FReadRequest>& CurrentRequests, FDiagnostic& OutError)
	{
		for (const auto& Intent : Intents)
		{
			const auto* Current = CurrentRequests.FindByPredicate([&](const auto& Request) {
				if (!Request.SnapshotIdentityOverride.IsSet()) return false;
				for (const auto& Context : Intent.OwnerContext)
					if (Request.SnapshotIdentityOverride->GetSelector(Context.Key) != Context.Value) return false;
				return Request.SemanticId == Intent.SemanticId && Request.SnapshotIdentityOverride.IsSet()
					&& Request.SnapshotIdentityOverride->GetSelector(Intent.OwnerSelector) == Intent.OwnerKey
					&& (Intent.PropertyPath.IsEmpty()
						? Request.RootObject && !Request.RootStruct
						: Request.SnapshotIdentityOverride->GetSelector(TEXT("property_path")) == Intent.PropertyPath);
			});
			if (!Current)
			{
				OutError = FDiagnostic::Failure(TEXT("property_intent_binding_missing"),
					TEXT("The captured property intent no longer has a matching native owner and property binding."), TEXT("property_intent_verify"));
				OutError.PropertyPath = Intent.PropertyPath;
				OutError.SubjectId = Intent.OwnerKey;
				OutError.SourceKind = TEXT("native_result");
				return false;
			}
			UeAgentAuthoringObjectApply::FApplyResult Result;
			const bool bVerified = Current->RootStruct
				? Intent.Intent.VerifyStruct(Current->RootStruct, const_cast<void*>(Current->RootData),
					const_cast<UObject*>(Current->NotificationOwner), Result)
				: Intent.Intent.VerifyObject(const_cast<UObject*>(Current->RootObject), Result);
			if (!bVerified)
			{ OutError = Result.Diagnostic; return false; }
		}
		OutError.Reset();
		return true;
	}
}

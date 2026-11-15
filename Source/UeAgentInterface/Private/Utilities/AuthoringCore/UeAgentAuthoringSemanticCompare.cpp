// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticCompare.h"

#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringEncodedTypes.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Utilities/AuthoringCore/UeAgentMovieSceneOriginalIntent.h"
#include "Utilities/UeAgentCurveOriginalIntent.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/ScopeRWLock.h"
#include "Misc/ScopeExit.h"
#include "UObject/UnrealType.h"

namespace UeAgentAuthoringSemanticCompare
{
	namespace
	{
		FString NormalizeId(FString Value)
		{
			Value.TrimStartAndEndInline();
			Value.ToLowerInline();
			return Value;
		}

		void NormalizeList(TArray<FString>& Values)
		{
			for (FString& Value : Values) Value = NormalizeId(MoveTemp(Value));
			Values.RemoveAll([](const FString& Value) { return Value.IsEmpty(); });
			Values.Sort();
			for (int32 Index = Values.Num() - 1; Index > 0; --Index)
			{
				if (Values[Index] == Values[Index - 1]) Values.RemoveAt(Index);
			}
		}

		FComparatorDescriptor Normalize(FComparatorDescriptor Descriptor)
		{
			Descriptor.ComparatorId = NormalizeId(MoveTemp(Descriptor.ComparatorId));
			NormalizeList(Descriptor.ValueKinds);
			NormalizeList(Descriptor.CapabilityTags);
			return Descriptor;
		}

		using UE::UAI::Contract::FDiagnostic;
		FDiagnostic MissingInput(const TCHAR* Code, const TCHAR* Reason, const FString& Field)
		{
			auto Error = FDiagnostic::Failure(Code, Reason, TEXT("semantic_compare"));
			Error.SourceKind = TEXT("native_result"); Error.PropertyPath = Field;
			Error.ExpectedValuePresent = true; Error.ActualValuePresent = false;
			return Error;
		}
		TSharedPtr<FJsonValue> DescriptorValue(const FString& Value) { return MakeShared<FJsonValueString>(Value); }
		TSharedPtr<FJsonValue> DescriptorValue(int32 Value) { return MakeShared<FJsonValueNumber>(Value); }
		template<typename T>
		bool DescriptorEqual(const T& Expected, const T& Actual, const FString& Path, FDiagnostic* Error) {
			if (Expected == Actual) return true;
			if (Error) { Error->PropertyPath = Path; Error->ExpectedValue = DescriptorValue(Expected); Error->ActualValue = DescriptorValue(Actual); }
			return false;
		}
		bool DescriptorEqual(const TArray<FString>& Expected, const TArray<FString>& Actual, const FString& Path, FDiagnostic* Error) {
			if (!DescriptorEqual(Expected.Num(), Actual.Num(), Path + TEXT("/count"), Error)) return false;
			for (int32 Index = 0; Index < Expected.Num(); ++Index)
				if (!DescriptorEqual(Expected[Index], Actual[Index], Path + TEXT("/") + FString::FromInt(Index), Error)) return false;
			return true;
		}
		bool ComparatorsEqual(const FComparatorDescriptor& Expected, const FComparatorDescriptor& Actual, FDiagnostic* Error) {
			return DescriptorEqual(Expected.ComparatorId, Actual.ComparatorId, TEXT("/comparator_id"), Error)
				&& DescriptorEqual(Expected.ValueKinds, Actual.ValueKinds, TEXT("/value_kinds"), Error)
				&& DescriptorEqual(Expected.CapabilityTags, Actual.CapabilityTags, TEXT("/capability_tags"), Error);
		}

		bool ValidateComparator(
			const FComparatorDescriptor& Descriptor,
			const FComparator& Comparator,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (Descriptor.ComparatorId.IsEmpty()) OutError = MissingInput(TEXT("semantic_comparator_id_missing"), TEXT("The semantic comparator identifier is empty."), TEXT("comparator_id"));
			else if (Descriptor.ValueKinds.IsEmpty()) OutError = MissingInput(TEXT("semantic_comparator_value_kinds_missing"), TEXT("The semantic comparator declares no supported value kinds."), TEXT("value_kinds"));
			else if (!Comparator) OutError = MissingInput(TEXT("semantic_comparator_callback_missing"), TEXT("The semantic comparator has no comparison callback."), TEXT("callback"));
			else
			{
				OutError.Reset();
				return true;
			}
			OutError.Stage = TEXT("semantic_registry"); OutError.CandidateName = Descriptor.ComparatorId;
			return false;
		}

		struct FDeclaredValue
		{
			TSharedPtr<FJsonObject> Type;
			TSharedPtr<FJsonObject> Definition;
			bool bStoredValues = false;
			TSharedPtr<FJsonObject> AdditionalDefinition;
		};

		TSharedPtr<FJsonObject> ObjectField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name)
		{
			const TSharedPtr<FJsonObject>* Field = nullptr;
			return Object && Object->TryGetObjectField(Name, Field) && Field ? *Field : nullptr;
		}

		FString ChildPointer(const FString& Path, FString Key)
		{
			return Path + TEXT("/") + Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
		}

		TSharedPtr<FJsonObject> ObjectKeyType(const FDeclaredValue* Declaration)
		{
			FString Kind, KeyKind;
			const auto KeyType = Declaration ? ObjectField(Declaration->Type, TEXT("key_type")) : nullptr;
			return Declaration && Declaration->Type && Declaration->Type->TryGetStringField(TEXT("kind"), Kind)
				&& Kind == TEXT("object") && KeyType && KeyType->TryGetStringField(TEXT("kind"), KeyKind)
				&& (KeyKind == TEXT("name") || KeyKind == TEXT("tuple")) ? KeyType : nullptr;
		}

		bool ObjectKeysEquivalent(const FString& A, const FString& B, const FDeclaredValue* Declaration)
		{
			const auto Type = ObjectKeyType(Declaration);
			if (!Type) return A.Equals(B, ESearchCase::CaseSensitive);
			if (Type->GetStringField(TEXT("kind")) == TEXT("name")) return FName(*A) == FName(*B);
			FString Separator;
			const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
			if (!Type->TryGetStringField(TEXT("separator"), Separator) || Separator.IsEmpty()
				|| !Type->TryGetArrayField(TEXT("components"), Components) || Components->IsEmpty()) return false;
			FString Left = A, Right = B;
			for (int32 Index = 0; Index < Components->Num(); ++Index) {
				FString L = Left, R = Right, Kind;
				if (Index + 1 < Components->Num()) {
					FString LeftTail, RightTail;
					if (!Left.Split(Separator, &L, &LeftTail, ESearchCase::CaseSensitive)
						|| !Right.Split(Separator, &R, &RightTail, ESearchCase::CaseSensitive)) return false;
					Left = MoveTemp(LeftTail); Right = MoveTemp(RightTail);
				}
				const auto& Component = (*Components)[Index];
				if (!Component || Component->Type != EJson::Object || !Component->AsObject()->TryGetStringField(TEXT("kind"), Kind)) return false;
				if (Kind == TEXT("name")) { if (FName(*L) != FName(*R)) return false; }
				else if (Kind != TEXT("string") || !L.Equals(R, ESearchCase::CaseSensitive)) return false;
			}
			return true;
		}

		bool HasDistinctObjectKeys(const TSharedPtr<FJsonObject>& Object, const FDeclaredValue& Declaration)
		{
			const auto Type = ObjectKeyType(&Declaration);
			if (Type && Type->GetStringField(TEXT("kind")) == TEXT("name")) {
				TSet<FName> Names;
				for (const auto& Field : Object->Values) Names.Add(FName(*Field.Key));
				return Names.Num() == Object->Values.Num();
			}
			TArray<FString> Keys;
			for (const auto& Field : Object->Values) {
				for (const auto& Key : Keys) if (ObjectKeysEquivalent(Key, Field.Key, &Declaration)) return false;
				Keys.Add(Field.Key);
			}
			return true;
		}

		TSharedPtr<FJsonValue> FindObjectMember(const TSharedPtr<FJsonObject>& Object,
			const FString& Key, const FDeclaredValue* Declaration, FString* ResolvedKey = nullptr)
		{
			if (!Object) return nullptr;
			if (const auto Exact = Object->TryGetField(Key))
			{
				if (ResolvedKey) *ResolvedKey = Key;
				return Exact;
			}
			if (ObjectKeyType(Declaration))
			{
				for (const auto& Field : Object->Values)
					if (ObjectKeysEquivalent(Field.Key, Key, Declaration))
					{
						if (ResolvedKey) *ResolvedKey = Field.Key;
						return Field.Value;
					}
			}
			return nullptr;
		}

		// Older descriptors may omit optional metadata that current native capture
		// supplies. Merge missing members recursively, but never reconcile conflicts.
		bool MergeDeclaredType(const TSharedPtr<FJsonObject>& A, const TSharedPtr<FJsonObject>& B,
			TSharedPtr<FJsonObject>& Out)
		{
			Out = MakeShared<FJsonObject>();
			if (A) Out->Values = A->Values;
			if (!B) return true;
			for (const auto& Field : B->Values)
			{
				const auto Existing = Out->TryGetField(Field.Key);
				if (Existing && Field.Value && Existing->Type == EJson::Object && Field.Value->Type == EJson::Object)
				{
					TSharedPtr<FJsonObject> Nested;
					if (!MergeDeclaredType(Existing->AsObject(), Field.Value->AsObject(), Nested)) return false;
					Out->SetObjectField(Field.Key, Nested);
				}
				else
				{
					if (Existing && !UE::UAI::StateSemantic::CanonicalJson::AreJsonValuesEquivalent(Existing, Field.Value)) return false;
					Out->SetField(Field.Key, Field.Value);
				}
			}
			return true;
		}

		FDeclaredValue CollectionItemType(const FDeclaredValue& Parent, int32 Index, const TCHAR* Field);

		// ValueDefinition is the existing field/container contract. Undeclared members
		// remain exact JSON; neither field names nor numeric-looking values imply a type.
		void CollectDeclaredTypes(const TSharedPtr<FJsonObject>& Type,
			const TSharedPtr<FJsonObject>& Definition, const TSharedPtr<FJsonValue>& Value,
			const FString& Path, TMap<FString, FDeclaredValue>& Out, bool bStoredValues = false)
		{
			if (!Type || !Value) return;
			if (const auto CapturedType = ObjectField(Definition, TEXT("type")))
			{
				const auto Body = MakeShared<FJsonObject>();
				const auto Nested = ObjectField(Definition, TEXT("value_definition"));
				Body->Values = Nested ? Nested->Values : Definition->Values;
				Body->RemoveField(TEXT("type"));
				CollectDeclaredTypes(CapturedType, Body, Value, Path, Out, bStoredValues);
				return;
			}
			FString Kind;
			Type->TryGetStringField(TEXT("kind"), Kind);
			auto& Captured = Out.FindOrAdd(Path);
			auto CompleteType = MakeShared<FJsonObject>();
			if (Captured.Type && Captured.Type->GetStringField(TEXT("kind")) == Kind) CompleteType->Values = Captured.Type->Values;
			for (const auto& Field : Type->Values) CompleteType->SetField(Field.Key, Field.Value);
			auto CompleteDefinition = MakeShared<FJsonObject>();
			if (Captured.Definition) CompleteDefinition->Values = Captured.Definition->Values;
			if (Definition) for (const auto& Field : Definition->Values) CompleteDefinition->SetField(Field.Key, Field.Value);
			Captured = {CompleteType, CompleteDefinition, bStoredValues};
			if (const auto Fields = ObjectField(Definition, TEXT("fields")); Fields && Value->Type == EJson::Object
				&& Kind != TEXT("set") && Kind != TEXT("map"))
			{
				auto Values = Value->AsObject();
				FString FieldsPath = Path;
				if ((Kind == TEXT("struct") || Kind == TEXT("instanced_struct") || Kind == TEXT("property_bag")) && ObjectField(Values, TEXT("fields")))
				{
					Values = ObjectField(Values, TEXT("fields"));
					FieldsPath = ChildPointer(Path, TEXT("fields"));
				}
				const auto FieldDeclaration = Captured;
				for (const auto& Field : Fields->Values)
				{
					if (!Field.Value || Field.Value->Type != EJson::Object) continue;
					const auto Descriptor = Field.Value->AsObject();
					FString Key = Field.Key;
					const auto Member = FindObjectMember(Values, Key, &FieldDeclaration, &Key);
					CollectDeclaredTypes(ObjectField(Descriptor, TEXT("type")), ObjectField(Descriptor, TEXT("value_definition")),
						Member, ChildPointer(FieldsPath, Key), Out, bStoredValues);
				}
			}
			// Set elements and map keys are identities; tolerance must not merge distinct keys.
			if (Kind == TEXT("array") || Kind == TEXT("fixed_array"))
			{
				const auto Items = Value->Type == EJson::Array ? Value
					: Value->Type == EJson::Object ? Value->AsObject()->TryGetField(TEXT("items")) : nullptr;
				if (!Items || Items->Type != EJson::Array) return;
				const FString ItemsPath = Value->Type == EJson::Array ? Path : ChildPointer(Path, TEXT("items"));
				for (int32 Index = 0; Index < Items->AsArray().Num(); ++Index)
				{
					const auto Item = CollectionItemType({Type, Definition, bStoredValues}, Index, TEXT("items"));
					CollectDeclaredTypes(Item.Type, Item.Definition, Items->AsArray()[Index],
						ChildPointer(ItemsPath, LexToString(Index)), Out, bStoredValues);
				}
			}
			if (Kind == TEXT("optional") && Value->Type == EJson::Object)
				CollectDeclaredTypes(ObjectField(Type, TEXT("inner_type")), ObjectField(ObjectField(Definition, TEXT("fields")), TEXT("value")),
					Value->AsObject()->TryGetField(TEXT("value")), ChildPointer(Path, TEXT("value")), Out, bStoredValues);
		}

		TSharedPtr<FJsonValue> ProjectOmittedNativeValues(const TSharedPtr<FJsonValue>& Value,
			const TSharedPtr<FJsonObject>& Definition)
		{
			if (!Value || !Definition) return Value;
			if (Value->Type == EJson::Object)
			{
				const auto Fields = ObjectField(Definition, TEXT("fields"));
				if (!Fields) return Value;
				TSharedPtr<FJsonObject> Result;
				for (const auto& Field : Fields->Values)
				{
					if (!Field.Value || Field.Value->Type != EJson::Object) continue;
					const auto Descriptor = Field.Value->AsObject();
					const auto Existing = Value->AsObject()->TryGetField(Field.Key);
					auto Effective = Existing ? Existing : Descriptor->TryGetField(TEXT("omitted_value"));
					Effective = ProjectOmittedNativeValues(Effective, ObjectField(Descriptor, TEXT("value_definition")));
					if (Effective && Effective != Existing)
					{
						if (!Result) { Result = MakeShared<FJsonObject>(); Result->Values = Value->AsObject()->Values; }
						Result->SetField(Field.Key, Effective);
					}
				}
				return Result ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueObject>(Result)) : Value;
			}
			const auto Items = Definition->TryGetField(TEXT("items"));
			if (Value->Type == EJson::Array && Items && Items->Type == EJson::Array)
			{
				auto Result = Value->AsArray();
				bool bChanged = false;
				for (int32 Index = 0; Index < Result.Num() && Items->AsArray().IsValidIndex(Index); ++Index)
				{
					const auto Descriptor = Items->AsArray()[Index];
					if (!Descriptor || Descriptor->Type != EJson::Object) continue;
					const auto Effective = ProjectOmittedNativeValues(Result[Index], ObjectField(Descriptor->AsObject(), TEXT("value_definition")));
					bChanged |= Effective != Result[Index];
					Result[Index] = Effective;
				}
				if (bChanged) return MakeShared<FJsonValueArray>(MoveTemp(Result));
			}
			return Value;
		}

		using UE::UAI::StateSemantic::CanonicalJson::FValueComparison;
		using UE::UAI::StateSemantic::CanonicalJson::CompareJsonValues;

		FValueComparison CompareTypedJson(const FDeclaredValue& LeftType, const TSharedPtr<FJsonValue>& Left,
			const FDeclaredValue& RightType, const TSharedPtr<FJsonValue>& Right);

		FDeclaredValue CollectionItemType(const FDeclaredValue& Parent, int32 Index, const TCHAR* Field)
		{
			FDeclaredValue Result;
			const FString Kind = Parent.Type->GetStringField(TEXT("kind"));
			Result.Type = ObjectField(Parent.Type, Kind == TEXT("map")
				? (FCString::Strcmp(Field, TEXT("key")) == 0 ? TEXT("key_type") : TEXT("mapped_type")) : TEXT("element_type"));
			Result.bStoredValues = Parent.bStoredValues || Kind == TEXT("set") || FCString::Strcmp(Field, TEXT("key")) == 0;
			auto ItemsDefinition = Parent.Definition;
			if (Kind == TEXT("map"))
			{
				ItemsDefinition = ObjectField(ObjectField(ItemsDefinition, TEXT("fields")), TEXT("entries"));
				if (const auto Nested = ObjectField(ItemsDefinition, TEXT("value_definition"))) ItemsDefinition = Nested;
			}
			else if (ItemsDefinition && !ItemsDefinition->HasField(TEXT("items")))
			{
				if (const auto ItemsField = ObjectField(ObjectField(ItemsDefinition, TEXT("fields")), TEXT("items")))
				{
					ItemsDefinition = ItemsField;
					if (const auto Nested = ObjectField(ItemsDefinition, TEXT("value_definition"))) ItemsDefinition = Nested;
				}
			}
			// The element contract survives an empty native collection; per-item
			// capture may refine it, but absence of an item must not erase its type.
			Result.Definition = ObjectField(ItemsDefinition, TEXT("element_definition"));
			const TSharedPtr<FJsonValue> Items = ItemsDefinition ? ItemsDefinition->TryGetField(TEXT("items")) : nullptr;
			if (Items && Items->Type == EJson::Array && Items->AsArray().IsValidIndex(Index))
			{
				const auto Item = Items->AsArray()[Index];
				if (Item && Item->Type == EJson::Object)
				{
					Result.Definition = Item->AsObject();
				}
			}
			if (Kind == TEXT("map"))
			{
				if (const auto Nested = ObjectField(Result.Definition, TEXT("value_definition"))) Result.Definition = Nested;
				Result.Definition = ObjectField(ObjectField(Result.Definition, TEXT("fields")), Field);
			}
			if (const auto Type = ObjectField(Result.Definition, TEXT("type")))
			{
				Result.Type = Type;
				Result.Definition = ObjectField(Result.Definition, TEXT("value_definition"));
			}
			return Result;
		}

		TOptional<FValueComparison> CompareCollection(const FDeclaredValue& AType, const TSharedPtr<FJsonValue>& A,
			const FDeclaredValue& BType, const TSharedPtr<FJsonValue>& B)
		{
			// A public type_id-only declaration still supplies child policies, but
			// does not select one of the native codec collection representations.
			FString TypeId;
			if (!AType.Type->HasField(TEXT("kind")) && AType.Type->TryGetStringField(TEXT("type_id"), TypeId) && !TypeId.IsEmpty())
				return {};
			const FString Kind = AType.Type->GetStringField(TEXT("kind"));
			const auto JsonKind = [](const TSharedPtr<FJsonValue>& Value) -> FString {
				if (!Value) return TEXT("absent");
				switch (Value->Type) {
				case EJson::Null: return TEXT("null");
				case EJson::String: return TEXT("string");
				case EJson::Number: return TEXT("number");
				case EJson::Boolean: return TEXT("boolean");
				case EJson::Array: return TEXT("array");
				case EJson::Object: return TEXT("object");
				default: return TEXT("none");
				}
			};
			const auto InvalidShape = [&](const TCHAR* Side, const TSharedPtr<FJsonValue>& Value,
				const FString& Required, const FString& Path = FString()) {
				auto Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("semantic_collection_shape_invalid"),
					FString::Printf(TEXT("The %s value cannot be compared using the declared %s collection representation."), Side, *Kind), TEXT("semantic_compare"));
				Error.Source.JsonPath = Path;
				Error.SourceKind = TEXT("comparison_contract");
				Error.ExpectedValue = MakeShared<FJsonValueString>(Required);
				auto Observed = MakeShared<FJsonObject>();
				Observed->SetStringField(TEXT("side"), Side);
				Observed->SetStringField(TEXT("json_kind"), JsonKind(Value));
				Error.ActualValue = MakeShared<FJsonValueObject>(Observed);
				return FValueComparison::Failed(MoveTemp(Error));
			};
			if (ObjectKeyType(&AType))
			{
				if (A->Type != EJson::Object) return InvalidShape(TEXT("expected"), A, TEXT("object"));
				if (B->Type != EJson::Object) return InvalidShape(TEXT("actual"), B, TEXT("object"));
				const auto Left = A->AsObject(), Right = B->AsObject();
				for (const auto& Side : {TPair<const TCHAR*, TSharedPtr<FJsonObject>>(TEXT("expected"), Left),
					TPair<const TCHAR*, TSharedPtr<FJsonObject>>(TEXT("actual"), Right)})
					if (!HasDistinctObjectKeys(Side.Value, FCString::Strcmp(Side.Key, TEXT("expected")) == 0 ? AType : BType)) {
						auto Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("semantic_collection_keys_ambiguous"),
							FString::Printf(TEXT("The %s object has multiple keys with the same declared identity."), Side.Key), TEXT("semantic_compare"));
						TArray<FString> Keys; Side.Value->Values.GetKeys(Keys); Keys.Sort();
						TArray<TSharedPtr<FJsonValue>> Values;
						for (const auto& Key : Keys) Values.Add(MakeShared<FJsonValueString>(Key));
						Error.ActualValue = MakeShared<FJsonValueArray>(MoveTemp(Values));
						Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("distinct keys under the declared key type"));
						return FValueComparison::Failed(MoveTemp(Error));
					}
				const auto MemberType = [](const FDeclaredValue& Parent, const FString& Key) {
					FDeclaredValue Result;
					Result.bStoredValues = Parent.bStoredValues;
					const auto Descriptor = FindObjectMember(ObjectField(Parent.Definition, TEXT("fields")), Key, &Parent);
					if (Descriptor && Descriptor->Type == EJson::Object) {
						Result.Type = ObjectField(Descriptor->AsObject(), TEXT("type"));
						Result.Definition = ObjectField(Descriptor->AsObject(), TEXT("value_definition"));
					}
					return Result;
				};
				TArray<FString> Keys;
				Left->Values.GetKeys(Keys); Keys.Sort();
				for (const FString& Key : Keys)
				{
					FString RightKey = Key;
					const auto Native = FindObjectMember(Right, Key, &AType, &RightKey);
					if (!Native) return FValueComparison::Different(ChildPointer(FString(), Key), Left->TryGetField(Key), nullptr);
					const auto Compared = CompareTypedJson(MemberType(AType, Key), Left->TryGetField(Key), MemberType(BType, RightKey), Native);
					if (!Compared.bEquivalent) return Compared.WithPrefix(ChildPointer(FString(), Key));
				}
				Keys.Reset(); Right->Values.GetKeys(Keys); Keys.Sort();
				for (const FString& Key : Keys)
					if (!FindObjectMember(Left, Key, &AType))
						return FValueComparison::Different(ChildPointer(FString(), Key), nullptr, Right->TryGetField(Key));
				return FValueComparison();
			}
			const bool bOrdered = Kind == TEXT("array") || Kind == TEXT("fixed_array");
			if (!bOrdered && Kind != TEXT("set") && Kind != TEXT("map") && Kind != TEXT("gameplay_tags")) return {};
			const TCHAR* Field = Kind == TEXT("map") ? TEXT("entries") : TEXT("items");
			const auto Items = [Field](const TSharedPtr<FJsonValue>& Value) {
				return Value->Type == EJson::Array ? Value : Value->Type == EJson::Object ? Value->AsObject()->TryGetField(Field) : nullptr;
			};
			const auto ItemPath = [Field](const TSharedPtr<FJsonValue>& Value, int32 Index) {
				return (Value->Type == EJson::Array ? FString() : FString(TEXT("/")) + Field) + TEXT("/") + LexToString(Index);
			};
			const auto LeftItems = Items(A), RightItems = Items(B);
			const FString Required = FString::Printf(TEXT("array or object containing an array field '%s'"), Field);
			if (!LeftItems || LeftItems->Type != EJson::Array)
				return InvalidShape(TEXT("expected"), A, Required);
			if (!RightItems || RightItems->Type != EJson::Array)
				return InvalidShape(TEXT("actual"), B, Required);
			const auto& L = LeftItems->AsArray(); const auto& R = RightItems->AsArray();
			const auto Extras = [Field](const TSharedPtr<FJsonValue>& Value) {
				const auto Result = MakeShared<FJsonObject>();
				if (Value->Type == EJson::Object) Result->Values = Value->AsObject()->Values;
				Result->RemoveField(Field);
				return MakeShared<FJsonValueObject>(Result);
			};
			const auto ExtraDifference = CompareJsonValues(Extras(A), Extras(B));
			if (!ExtraDifference.bEquivalent) return ExtraDifference;
			if (bOrdered) {
				for (int32 Index = 0; Index < FMath::Min(L.Num(), R.Num()); ++Index) {
					const auto Compared = CompareTypedJson(CollectionItemType(AType, Index, TEXT("items")), L[Index],
						CollectionItemType(BType, Index, TEXT("items")), R[Index]);
					if (!Compared.bEquivalent) return Compared.WithPrefix(ItemPath(A, Index));
				}
				if (L.Num() != R.Num()) {
					const int32 Index = FMath::Min(L.Num(), R.Num());
					return FValueComparison::Different(ItemPath(L.IsValidIndex(Index) ? A : B, Index),
						L.IsValidIndex(Index) ? L[Index] : nullptr, R.IsValidIndex(Index) ? R[Index] : nullptr);
				}
				return FValueComparison();
			}
			TSet<int32> Matched;
			for (int32 I = 0; I < L.Num(); ++I)
			{
				bool bFound = false;
				for (int32 J = 0; J < R.Num(); ++J)
				{
					if (Matched.Contains(J)) continue;
					if (Kind == TEXT("map"))
					{
						if (!L[I] || L[I]->Type != EJson::Object) return InvalidShape(TEXT("expected"), L[I], TEXT("map entry object"), ItemPath(A, I));
						if (!R[J] || R[J]->Type != EJson::Object) return InvalidShape(TEXT("actual"), R[J], TEXT("map entry object"), ItemPath(B, J));
						const auto KeyComparison = CompareTypedJson(CollectionItemType(AType, I, TEXT("key")), L[I]->AsObject()->TryGetField(TEXT("key")),
							CollectionItemType(BType, J, TEXT("key")), R[J]->AsObject()->TryGetField(TEXT("key")));
						if (!KeyComparison.IsComparable()) return KeyComparison.WithPrefix(ItemPath(A, I) + TEXT("/key"));
						if (!KeyComparison.bEquivalent) continue;
						const auto Compared = CompareTypedJson(CollectionItemType(AType, I, TEXT("value")), L[I]->AsObject()->TryGetField(TEXT("value")),
							CollectionItemType(BType, J, TEXT("value")), R[J]->AsObject()->TryGetField(TEXT("value")));
						if (!Compared.bEquivalent) return Compared.WithPrefix(ItemPath(A, I) + TEXT("/value"));
						const auto RowExtras = [](const TSharedPtr<FJsonValue>& Row) {
							auto Result = MakeShared<FJsonObject>(); Result->Values = Row->AsObject()->Values;
							Result->RemoveField(TEXT("key")); Result->RemoveField(TEXT("value"));
							return MakeShared<FJsonValueObject>(Result);
						};
						const auto RowDifference = CompareJsonValues(RowExtras(L[I]), RowExtras(R[J]));
						if (!RowDifference.bEquivalent) return RowDifference.WithPrefix(ItemPath(A, I));
					}
					else
					{
						auto LT = CollectionItemType(AType, I, TEXT("items")), RT = CollectionItemType(BType, J, TEXT("items"));
						if (Kind == TEXT("gameplay_tags")) {
							LT.Type = RT.Type = MakeShared<FJsonObject>(); LT.Type->SetStringField(TEXT("kind"), TEXT("name"));
						}
						const auto Compared = CompareTypedJson(LT, L[I], RT, R[J]);
						if (!Compared.IsComparable()) return Compared.WithPrefix(ItemPath(A, I));
						if (!Compared.bEquivalent) continue;
					}
					Matched.Add(J); bFound = true; break;
				}
				if (!bFound) {
					if (Kind == TEXT("map") && L[I] && L[I]->Type == EJson::Object)
						return FValueComparison::Different(ItemPath(A, I) + TEXT("/key"), L[I]->AsObject()->TryGetField(TEXT("key")), nullptr);
					return FValueComparison::Different(ItemPath(A, I), L[I], nullptr);
				}
			}
			for (int32 J = 0; J < R.Num(); ++J)
				if (!Matched.Contains(J)) {
					if (Kind == TEXT("map") && R[J] && R[J]->Type == EJson::Object)
						return FValueComparison::Different(ItemPath(B, J) + TEXT("/key"), nullptr, R[J]->AsObject()->TryGetField(TEXT("key")));
					return FValueComparison::Different(ItemPath(B, J), nullptr, R[J]);
				}
			return FValueComparison();
		}

		FValueComparison CompareTypedJson(const FDeclaredValue& LeftType, const TSharedPtr<FJsonValue>& Left,
			const FDeclaredValue& RightType, const TSharedPtr<FJsonValue>& Right)
		{
			TMap<FString, FDeclaredValue> LeftTypes, RightTypes;
			TMap<FString, FString> CurveTypes;
			UeAgentCurveJson::CollectNativeCurveComparisonTypes(Left, FString(), CurveTypes);
			for (const auto& Entry : CurveTypes) {
				auto Type = MakeShared<FJsonObject>(); Type->SetStringField(TEXT("kind"), Entry.Value);
				LeftTypes.Add(Entry.Key, {Type, nullptr, LeftType.bStoredValues});
			}
			CurveTypes.Reset();
			UeAgentCurveJson::CollectNativeCurveComparisonTypes(Right, FString(), CurveTypes);
			for (const auto& Entry : CurveTypes) {
				auto Type = MakeShared<FJsonObject>(); Type->SetStringField(TEXT("kind"), Entry.Value);
				RightTypes.Add(Entry.Key, {Type, nullptr, RightType.bStoredValues});
			}
			CollectDeclaredTypes(LeftType.Type, LeftType.Definition, Left, FString(), LeftTypes, LeftType.bStoredValues);
			CollectDeclaredTypes(RightType.Type, RightType.Definition, Right, FString(), RightTypes, RightType.bStoredValues);
			if (LeftType.AdditionalDefinition) CollectDeclaredTypes(LeftType.Type, LeftType.AdditionalDefinition, Left, FString(), LeftTypes, LeftType.bStoredValues);
			if (RightType.AdditionalDefinition) CollectDeclaredTypes(RightType.Type, RightType.AdditionalDefinition, Right, FString(), RightTypes, RightType.bStoredValues);
			const UE::UAI::StateSemantic::CanonicalJson::FDeclaredValueComparison Compare =
				[&](const auto& A, const auto& B, const FString& Path) -> TOptional<FValueComparison>
				{
					const auto* LT = LeftTypes.Find(Path); const auto* RT = RightTypes.Find(Path);
					if (!LT && !RT) return {};
					if (!LT) LT = RT;
					if (!RT) RT = LT;
					TSharedPtr<FJsonObject> Type;
					if (!MergeDeclaredType(LT->Type, RT->Type, Type))
						return CompareJsonValues(MakeShared<FJsonValueObject>(LT->Type), MakeShared<FJsonValueObject>(RT->Type)).WithPrefix(TEXT("/type"));
					auto LA = *LT, RB = *RT; LA.Type = RB.Type = Type;
					const auto Definition = MakeShared<FJsonObject>();
					if (LT->Definition) Definition->Values = LT->Definition->Values;
					if (RT->Definition) for (const auto& Field : RT->Definition->Values) Definition->SetField(Field.Key, Field.Value);
					if (const auto Typed = UeAgentPropertyValueCodec::CompareCanonicalValues(Type, Definition, A, B,
						LT->bStoredValues || RT->bStoredValues); Typed.IsSet()) return Typed;
					return CompareCollection(LA, A, RB, B);
				};
			return CompareJsonValues(Left, Right, FString(), &Compare);
		}

		FValueComparison CompareDeclaredEntries(const UeAgentAuthoringState::FStateEntry& Left,
			const UeAgentAuthoringState::FStateEntry& Right)
		{
			// Only native capture provenance can explain a sparse value. Public
			// schema defaults do not authorize treating arbitrary missing fields as values.
			auto LeftValue = ProjectOmittedNativeValues(Left.Value,
				Left.ComparisonValueDefinition && Left.ComparisonValueDefinition->Type == EJson::Object ? Left.ComparisonValueDefinition->AsObject() : nullptr);
			auto RightValue = ProjectOmittedNativeValues(Right.Value,
				Right.ComparisonValueDefinition && Right.ComparisonValueDefinition->Type == EJson::Object ? Right.ComparisonValueDefinition->AsObject() : nullptr);
			FDeclaredValue LeftType{Left.Type.ToJsonObject(), Left.ValueDefinition && Left.ValueDefinition->Type == EJson::Object
				? Left.ValueDefinition->AsObject() : nullptr};
			FDeclaredValue RightType{Right.Type.ToJsonObject(), Right.ValueDefinition && Right.ValueDefinition->Type == EJson::Object
				? Right.ValueDefinition->AsObject() : nullptr};
			LeftType.AdditionalDefinition = Left.ComparisonValueDefinition && Left.ComparisonValueDefinition->Type == EJson::Object
				? Left.ComparisonValueDefinition->AsObject() : nullptr;
			RightType.AdditionalDefinition = Right.ComparisonValueDefinition && Right.ComparisonValueDefinition->Type == EJson::Object
				? Right.ComparisonValueDefinition->AsObject() : nullptr;
			return CompareTypedJson(LeftType, LeftValue, RightType, RightValue);
		}

		bool CanonicalValuesEquivalent(
			const UeAgentAuthoringState::FStateEntry& Left,
			const UeAgentAuthoringState::FStateEntry& Right,
			bool& bOutEquivalent,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (!Left.Value.IsValid() || !Right.Value.IsValid())
			{
				OutError = MissingInput(TEXT("semantic_compare_value_missing"), !Left.Value ? TEXT("The expected comparison value is missing.") : TEXT("The actual comparison value is missing."), !Left.Value ? TEXT("expected.value") : TEXT("actual.value"));
				return false;
			}
			const auto Compared = CompareDeclaredEntries(Left, Right);
			bOutEquivalent = Compared.bEquivalent;
			OutError.Reset();
			if (!Compared.IsComparable()) {
				Compared.ApplyTo(OutError);
				return false;
			}
			if (!bOutEquivalent)
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("canonical_value_mismatch"), TEXT("The compared values differ."), TEXT("semantic_compare"));
				Compared.ApplyTo(OutError);
			}
			return true;
		}

		bool EntryIdentityLess(
			const UeAgentAuthoringState::FStateEntry& Left,
			const UeAgentAuthoringState::FStateEntry& Right)
		{
			// CaptureSnapshot orders entries by these fields, not by their serialized key.
			return Left.SemanticId != Right.SemanticId
				? Left.SemanticId < Right.SemanticId
				: Left.TargetIdentity.CanonicalKey < Right.TargetIdentity.CanonicalKey;
		}

		FEntryComparison MakeMissingComparison(
			const UeAgentAuthoringState::FStateEntry& Entry,
			const FString& ErrorCode)
		{
			FEntryComparison Result;
			Result.Status = TEXT("different");
			Result.Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(ErrorCode, TEXT("An entry is missing from one of the compared snapshots."), TEXT("semantic_compare"));
			Result.SemanticId = Entry.SemanticId;
			Result.TargetIdentity = Entry.TargetIdentity;
			Result.Diagnostic.SubjectId = Entry.TargetIdentity.CanonicalKey;
			Result.Diagnostic.SemanticId = Entry.SemanticId;
			Result.Diagnostic.PropertyPath = Entry.TargetIdentity.GetSelector(TEXT("property_path"));
			return Result;
		}

		void AddBindingIssue(
			FBindingReport& Report,
			const FString& SemanticId,
			UE::UAI::Contract::FDiagnostic Diagnostic)
		{
			Diagnostic.SemanticId = SemanticId;
			Report.Issues.Add({MoveTemp(Diagnostic)});
		}
	}

	bool FComparatorDescriptor::SupportsValueKind(const FString& ValueKind) const
	{
		const FString CanonicalValueKind = NormalizeId(ValueKind);
		return ValueKinds.Contains(TEXT("*")) || ValueKinds.Contains(CanonicalValueKind);
	}

	bool FComparatorDescriptor::operator==(const FComparatorDescriptor& Other) const
	{
		return ComparatorsEqual(*this, Other, nullptr);
	}

	FComparatorRegistry& FComparatorRegistry::Get()
	{
		static FComparatorRegistry Registry;
		static const bool bBuiltInsRegistered = []()
		{
			UE::UAI::Contract::FDiagnostic Error;
			return RegisterBuiltInComparators(Registry, Error);
		}();
		ensureAlwaysMsgf(bBuiltInsRegistered, TEXT("Semantic comparator built-in registration failed."));
		return Registry;
	}

	bool FComparatorRegistry::RegisterComparator(
		const FComparatorDescriptor& Descriptor,
		FComparator Comparator,
		ERegistrationResult& OutResult,
		UE::UAI::Contract::FDiagnostic& OutError,
		FOriginalIntentProjection OriginalIntentProjection)
	{
		const FComparatorDescriptor Canonical = Normalize(Descriptor);
		if (!ValidateComparator(Canonical, Comparator, OutError)) return false;
		FWriteScopeLock Lock(RegistryLock);
		if (const FEntry* Existing = Comparators.Find(Canonical.ComparatorId))
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("semantic_comparator_conflict"), TEXT("The semantic comparator differs from its existing registration."), TEXT("semantic_registry"));
			OutError.CandidateName = Canonical.ComparatorId; OutError.SourceKind = TEXT("native_result");
			if (!ComparatorsEqual(Existing->Descriptor, Canonical, &OutError))
			{
				return false;
			}
			OutResult = ERegistrationResult::AlreadyRegistered;
			OutError.Reset();
			return true;
		}
		FEntry& Entry = Comparators.Add(Canonical.ComparatorId);
		Entry.Descriptor = Canonical;
		Entry.Comparator = MoveTemp(Comparator);
		Entry.OriginalIntentProjection = MoveTemp(OriginalIntentProjection);
		OutResult = ERegistrationResult::Registered;
		OutError.Reset();
		return true;
	}

	bool FComparatorRegistry::FindComparator(const FString& ComparatorId, FComparatorDescriptor& OutDescriptor) const
	{
		FReadScopeLock Lock(RegistryLock);
		if (const FEntry* Entry = Comparators.Find(NormalizeId(ComparatorId)))
		{
			OutDescriptor = Entry->Descriptor;
			return true;
		}
		return false;
	}

	bool FComparatorRegistry::CopyComparator(const FString& ComparatorId, FComparator& OutComparator) const
	{
		FReadScopeLock Lock(RegistryLock);
		if (const FEntry* Entry = Comparators.Find(NormalizeId(ComparatorId)))
		{
			OutComparator = Entry->Comparator;
			return true;
		}
		return false;
	}

	bool FComparatorRegistry::ProjectOriginalIntent(const FString& ComparatorId,
		UeAgentAuthoringState::FStateEntry& NativeEntry,
		TSharedPtr<FJsonValue>& Before, TSharedPtr<FJsonValue>& Desired,
		TSharedPtr<FJsonValue>& Actual, TMap<FString, FString>& SourcePaths,
		UE::UAI::Contract::FDiagnostic& OutError) const
	{
		FOriginalIntentProjection Projection;
		{
			FReadScopeLock Lock(RegistryLock);
			const FEntry* Entry = Comparators.Find(NormalizeId(ComparatorId));
			if (!Entry) {
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("semantic_comparator_unknown"), TEXT("The original-intent comparator is not registered."), TEXT("semantic_compare"));
				OutError.PropertyPath = TEXT("comparator_id"); OutError.CandidateName = ComparatorId;
				OutError.ActualValue = DescriptorValue(ComparatorId); OutError.SourceKind = TEXT("native_result"); return false;
			}
			Projection = Entry->OriginalIntentProjection;
		}
		OutError.Reset();
		return !Projection || Projection(NativeEntry, Before, Desired, Actual, SourcePaths, OutError);
	}

	TArray<FComparatorDescriptor> FComparatorRegistry::ListComparators() const
	{
		FReadScopeLock Lock(RegistryLock);
		TArray<FComparatorDescriptor> Result;
		for (const TPair<FString, FEntry>& Pair : Comparators) Result.Add(Pair.Value.Descriptor);
		Result.Sort([](const FComparatorDescriptor& Left, const FComparatorDescriptor& Right)
		{
			return Left.ComparatorId < Right.ComparatorId;
		});
		return Result;
	}

	int32 FComparatorRegistry::GetComparatorCount() const
	{
		FReadScopeLock Lock(RegistryLock);
		return Comparators.Num();
	}

	void FEntryComparison::AppendDifferenceValues(FJsonObject& Object) const
	{
		Diagnostic.AppendDifferenceValues(Object, true);
	}

	TSharedRef<FJsonObject> FEntryComparison::ToJsonObject() const
	{
		auto Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("status"), Status);
		Object->SetStringField(TEXT("error_code"), bEquivalent ? FString(TEXT("semantic_equivalent")) : Diagnostic.Code);
		Object->SetBoolField(TEXT("equivalent"), bEquivalent);
		if (!Diagnostic.Source.JsonPath.IsEmpty()) Object->SetStringField(TEXT("difference_path"), Diagnostic.Source.JsonPath);
		AppendDifferenceValues(*Object);
		if (!SemanticId.IsEmpty()) Object->SetStringField(TEXT("semantic_id"), SemanticId);
		if (!ComparatorId.IsEmpty()) Object->SetStringField(TEXT("comparator_id"), ComparatorId);
		if (TargetIdentity.IsValid()) Object->SetObjectField(TEXT("target_identity"), TargetIdentity.ToJsonObject());
		if (!Diagnostic.IsEmpty()) Object->SetObjectField(TEXT("diagnostic"), Diagnostic.ToJson());
		return Object;
	}

	TSharedRef<FJsonObject> FComparisonReport::ToJsonObject() const
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("status"), Status);
		Object->SetStringField(TEXT("error_code"), bEquivalent ? FString(TEXT("semantic_snapshots_equivalent")) : Diagnostic.Code);
		Object->SetBoolField(TEXT("equivalent"), bEquivalent);
		if (!Diagnostic.IsEmpty()) Object->SetObjectField(TEXT("diagnostic"), Diagnostic.ToJson());
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FEntryComparison& Entry : Entries) Values.Add(MakeShared<FJsonValueObject>(Entry.ToJsonObject()));
		Object->SetArrayField(TEXT("entries"), Values);
		return Object;
	}

	TSharedRef<FJsonObject> FBindingIssue::ToJsonObject() const
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("semantic_id"), Diagnostic.SemanticId);
		Object->SetStringField(TEXT("error_code"), Diagnostic.Code);
		if (!Diagnostic.PublicMessage.IsEmpty()) Object->SetStringField(TEXT("error_detail"), Diagnostic.PublicMessage);
		Object->SetObjectField(TEXT("diagnostic"), Diagnostic.ToPublicJson());
		return Object;
	}

	TSharedRef<FJsonObject> FBindingReport::ToJsonObject() const
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("status"), Status);
		Object->SetStringField(TEXT("error_code"), bValid ? FString(TEXT("shared_core_bindings_valid")) : Diagnostic.Code);
		if (!Diagnostic.IsEmpty()) Object->SetObjectField(TEXT("diagnostic"), Diagnostic.ToPublicJson());
		Object->SetBoolField(TEXT("valid"), bValid);
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FBindingIssue& Issue : Issues) Values.Add(MakeShared<FJsonValueObject>(Issue.ToJsonObject()));
		Object->SetArrayField(TEXT("issues"), Values);
		return Object;
	}

	namespace
	{
		FOriginalIntentProjection GraphPositionProjection(bool bInteger, bool bRound, double UnchangedTolerance = 0.0)
		{
			return [bInteger, bRound, UnchangedTolerance](const auto&, auto& Before, auto& Desired,
				auto&, auto&, UE::UAI::Contract::FDiagnostic& Error)
			{
				Before = UeAgentAuthoringState::CloneCanonicalValue(Before);
				Desired = UeAgentAuthoringState::CloneCanonicalValue(Desired);
				auto ProjectNode = [&](const TSharedPtr<FJsonValue>& Old, const TSharedPtr<FJsonValue>& Wanted, bool bIndexBinding = false)
				{
					if (!Wanted || Wanted->Type != EJson::Object) return true;
					const TSharedPtr<FJsonObject>* Position = nullptr;
					if (!Wanted->AsObject()->TryGetObjectField(TEXT("pos"), Position)) return true;
					const TSharedPtr<FJsonObject>* OldPosition = nullptr;
					if (Old && Old->Type == EJson::Object)
					{
						bool bSameNode = true;
						if (bIndexBinding)
							for (const TCHAR* Identity : {TEXT("id"), TEXT("name")})
								if (const auto Id = Wanted->AsObject()->TryGetField(Identity))
									if (const auto OldId = Old->AsObject()->TryGetField(Identity))
									{ bSameNode = UE::UAI::StateSemantic::CanonicalJson::AreJsonValuesEquivalent(Id, OldId); break; }
						if (bSameNode) Old->AsObject()->TryGetObjectField(TEXT("pos"), OldPosition);
					}
					FVector2D OldXY, WantedXY;
					const bool bUnchangedNative = UnchangedTolerance > 0.0 && OldPosition
						&& (*OldPosition)->TryGetNumberField(TEXT("x"), OldXY.X)
						&& (*OldPosition)->TryGetNumberField(TEXT("y"), OldXY.Y)
						&& (*Position)->TryGetNumberField(TEXT("x"), WantedXY.X)
						&& (*Position)->TryGetNumberField(TEXT("y"), WantedXY.Y)
						&& OldXY.Equals(WantedXY, UnchangedTolerance);
					for (const TCHAR* Axis : {TEXT("x"), TEXT("y")})
					{
						const auto Requested = (*Position)->TryGetField(Axis);
						if (!Requested) continue;
						const auto OldValue = OldPosition ? (*OldPosition)->TryGetField(Axis) : nullptr;
						if (UE::UAI::StateSemantic::CanonicalJson::AreJsonValuesEquivalent(OldValue, Requested)) continue;
						double Number;
						if (!Requested->TryGetNumber(Number)) {
							Error = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("graph_position_number_required"), FString(TEXT("/")) + Axis,
								MakeShared<FJsonValueString>(TEXT("number")), Requested, TEXT("original_intent_projection"), TEXT("Graph coordinates must be numeric."));
							UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, **Position); return false;
						}
						if (bInteger)
						{
							int32 Native;
							if (!UeAgentPropertyValueCodec::TryConvertInt32(Number, Native, Error, bRound)) {
								Error.SemanticPointer = FString(TEXT("/")) + Axis;
								UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, **Position); return false;
							}
							(*Position)->SetNumberField(Axis, Native);
						}
						else if (bUnchangedNative) (*Position)->SetField(Axis, UeAgentAuthoringState::CloneCanonicalValue(OldValue));
						// Retain the original write mask even if normalization yields
						// the old value. An unrelated native change must still fail.
						if (OldPosition) (*OldPosition)->RemoveField(Axis);
					}
					return true;
				};
				TFunction<bool(const TSharedPtr<FJsonValue>&, const TSharedPtr<FJsonValue>&)> WalkGraph;
				WalkGraph = [&](const auto& Old, const auto& Wanted)
				{
					if (!Wanted || Wanted->Type != EJson::Object) return true;
					if (!ProjectNode(Old, Wanted)) return false;
					for (const TCHAR* Container : {TEXT("nodes"), TEXT("expressions")})
					{
						const auto Items = Wanted->AsObject()->TryGetField(Container);
						const auto OldItems = Old && Old->Type == EJson::Object ? Old->AsObject()->TryGetField(Container) : nullptr;
						if (!Items) continue;
						if (Items->Type == EJson::Object)
						{
							for (const auto& Node : Items->AsObject()->Values)
								if (!ProjectNode(OldItems && OldItems->Type == EJson::Object
									? OldItems->AsObject()->TryGetField(Node.Key) : nullptr, Node.Value)) return false;
						}
						else if (Items->Type == EJson::Array)
						{
							for (int32 Index = 0; Index < Items->AsArray().Num(); ++Index)
								if (!ProjectNode(OldItems && OldItems->Type == EJson::Array && OldItems->AsArray().IsValidIndex(Index)
									? OldItems->AsArray()[Index] : nullptr, Items->AsArray()[Index], true)) return false;
						}
					}
					// Traverse graph envelopes only, never arbitrary property payloads.
					for (const TCHAR* Container : {TEXT("graphs"), TEXT("audio_graph"), TEXT("rig_graph"), TEXT("graph")})
					{
						const auto Child = Wanted->AsObject()->TryGetField(Container);
						const auto OldChild = Old && Old->Type == EJson::Object ? Old->AsObject()->TryGetField(Container) : nullptr;
						if (!Child || Child->Type != EJson::Object) continue;
						if (FCString::Strcmp(Container, TEXT("graphs")) != 0) { if (!WalkGraph(OldChild, Child)) return false; }
						else for (const auto& Graph : Child->AsObject()->Values)
							if (!WalkGraph(OldChild && OldChild->Type == EJson::Object
								? OldChild->AsObject()->TryGetField(Graph.Key) : nullptr, Graph.Value)) return false;
					}
					return true;
				};
				return WalkGraph(Before, Desired);
			};
		}
	}

	bool RegisterBuiltInComparators(FComparatorRegistry& Registry, UE::UAI::Contract::FDiagnostic& OutError)
	{
		FComparatorDescriptor Descriptor;
		Descriptor.ComparatorId = TEXT("uai.compare.canonical_value");
		Descriptor.ValueKinds = { TEXT("*") };
		Descriptor.CapabilityTags = { TEXT("canonical_value"), TEXT("native_property"), TEXT("semantic_tolerance") };
		ERegistrationResult Result = ERegistrationResult::Registered;
		if (!Registry.RegisterComparator(Descriptor, CanonicalValuesEquivalent, Result, OutError)) return false;
		Descriptor.ComparatorId = TEXT("uai.compare.movie_scene");
		if (!Registry.RegisterComparator(Descriptor, CanonicalValuesEquivalent, Result, OutError,
			[](const auto&, auto& Before, auto& Desired, auto& Actual, auto& SourcePaths, UE::UAI::Contract::FDiagnostic& Error) {
				return UE::UAI::Authoring::Timeline::ProjectMovieSceneOriginalIntent(Before, Desired, Actual, SourcePaths, Error);
			})) return false;
		for (const auto& Policy : {
			TPair<const TCHAR*, FOriginalIntentProjection>(GraphInt32PositionComparator, GraphPositionProjection(true, false)),
			TPair<const TCHAR*, FOriginalIntentProjection>(GraphRoundedInt32PositionComparator, GraphPositionProjection(true, true)),
			TPair<const TCHAR*, FOriginalIntentProjection>(GraphVectorPositionComparator, GraphPositionProjection(false, false, 0.01)),
			TPair<const TCHAR*, FOriginalIntentProjection>(TEXT("uai.compare.graph_vector_small_movement"), GraphPositionProjection(false, false, UE_KINDA_SMALL_NUMBER))})
		{
			Descriptor.ComparatorId = Policy.Key;
			if (!Registry.RegisterComparator(Descriptor, CanonicalValuesEquivalent, Result, OutError, Policy.Value)) return false;
		}
		return true;
	}

	FValueComparison CompareDeclaredValues(const TSharedPtr<FJsonObject>& Type,
		const TSharedPtr<FJsonObject>& Definition, const TSharedPtr<FJsonValue>& Left,
		const TSharedPtr<FJsonValue>& Right)
	{
		return CompareTypedJson({Type, Definition, true}, Left, {Type, Definition, true}, Right);
	}

	bool AreDeclaredValuesEquivalent(const TSharedPtr<FJsonObject>& Type,
		const TSharedPtr<FJsonObject>& Definition, const TSharedPtr<FJsonValue>& Left,
		const TSharedPtr<FJsonValue>& Right)
	{
		return CompareDeclaredValues(Type, Definition, Left, Right).bEquivalent;
	}

	void BindComparisonObjectRows(UeAgentAuthoringState::FStateEntry& Entry, const FString& Field,
		const TSharedPtr<FJsonObject>& RowTypes)
	{
		const auto Type = MakeShared<FJsonObject>();
		Type->SetStringField(TEXT("kind"), TEXT("object"));
		const auto Definition = MakeShared<FJsonObject>();
		Definition->SetObjectField(TEXT("fields"), RowTypes);
		const auto Descriptor = MakeShared<FJsonObject>();
		Descriptor->SetObjectField(TEXT("type"), Type);
		Descriptor->SetObjectField(TEXT("value_definition"), Definition);
		Entry.ComparisonValueDefinition = Entry.ComparisonValueDefinition
			? UeAgentAuthoringState::CloneCanonicalValue(Entry.ComparisonValueDefinition)
			: MakeShared<FJsonValueObject>(MakeShared<FJsonObject>());
		auto Root = Entry.ComparisonValueDefinition->AsObject();
		if (const auto Body = ObjectField(Root, TEXT("value_definition"))) Root = Body;
		if (!ObjectField(Root, TEXT("fields"))) Root->SetObjectField(TEXT("fields"), MakeShared<FJsonObject>());
		Root->GetObjectField(TEXT("fields"))->SetObjectField(Field, Descriptor);
	}

	bool BindComparisonCollectionKeys(UeAgentAuthoringState::FStateEntry& Entry, const FString& Field,
		const TMap<FString, int32>& SourceToNativeIndex, UE::UAI::Contract::FDiagnostic& OutError)
	{
		TMap<FString, FDeclaredValue> Types;
		for (const auto& Definition : {Entry.ValueDefinition, Entry.ComparisonValueDefinition})
			CollectDeclaredTypes(Entry.Type.ToJsonObject(),
				Definition && Definition->Type == EJson::Object ? Definition->AsObject() : nullptr,
				Entry.Value, FString(), Types);
		const FString Path = ChildPointer(FString(), Field);
		const auto* Collection = Types.Find(Path);
		FString Kind;
		if (!Collection || !Collection->Type || !Collection->Type->TryGetStringField(TEXT("kind"), Kind)
			|| (Kind != TEXT("array") && Kind != TEXT("fixed_array"))) {
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("semantic_projection_collection_type_invalid"),
				TEXT("A source-keyed projection requires the native ordered collection declaration."), TEXT("semantic_compare"));
			OutError.Source.JsonPath = Path;
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("array or fixed_array"));
			OutError.ActualValue = MakeShared<FJsonValueString>(Kind.IsEmpty() ? TEXT("absent") : Kind);
			return false;
		}
		const auto Rows = MakeShared<FJsonObject>();
		for (const auto& Binding : SourceToNativeIndex) {
			const auto Item = CollectionItemType(*Collection, Binding.Value, TEXT("items"));
			const auto Descriptor = MakeShared<FJsonObject>();
			if (Item.Type) Descriptor->SetObjectField(TEXT("type"), Item.Type);
			if (Item.Definition) Descriptor->SetObjectField(TEXT("value_definition"), Item.Definition);
			Rows->SetObjectField(Binding.Key, Descriptor);
		}
		BindComparisonObjectRows(Entry, Field, Rows);
		return true;
	}

	TSharedPtr<FJsonValue> FindDeclaredValuePath(const UeAgentAuthoringState::FStateEntry& Declaration,
		const TSharedPtr<FJsonValue>& Value, const FString& Path, FString* OutResolvedPath)
	{
		TMap<FString, FDeclaredValue> Types;
		for (const auto& Definition : {Declaration.ValueDefinition, Declaration.ComparisonValueDefinition})
			CollectDeclaredTypes(Declaration.Type.ToJsonObject(),
				Definition && Definition->Type == EJson::Object ? Definition->AsObject() : nullptr,
				Value, FString(), Types);
		TSharedPtr<FJsonValue> Current = Value;
		FString Resolved;
		if (!Path.IsEmpty() && !Path.StartsWith(TEXT("/"))) return nullptr;
		TArray<FString> Parts;
		if (!Path.IsEmpty()) Path.RightChop(1).ParseIntoArray(Parts, TEXT("/"), false);
		for (const auto& Part : Parts)
		{
			FString Key = Part.Replace(TEXT("~1"), TEXT("/")).Replace(TEXT("~0"), TEXT("~"));
			if (Current && Current->Type == EJson::Object)
				Current = FindObjectMember(Current->AsObject(), Key, Types.Find(Resolved), &Key);
			else if (Current && Current->Type == EJson::Array)
			{
				int32 Index;
				Current = LexTryParseString(Index, *Key) && Current->AsArray().IsValidIndex(Index)
					? Current->AsArray()[Index] : nullptr;
			}
			else Current.Reset();
			Resolved = ChildPointer(Resolved, Key);
		}
		if (OutResolvedPath) *OutResolvedPath = Resolved;
		return Current;
	}

	TSharedPtr<FJsonValue> OverlayValueDelta(const TSharedPtr<FJsonValue>& Before,
		const TSharedPtr<FJsonValue>& Desired, const TSharedPtr<FJsonValue>& Actual,
		const UeAgentAuthoringState::FStateEntry* Declaration)
	{
		TMap<FString, FDeclaredValue> Types;
		if (Declaration) {
			const auto Type = Declaration->Type.ToJsonObject();
			for (const auto& Value : {Before, Desired, Actual})
				for (const auto& Definition : {Declaration->ValueDefinition, Declaration->ComparisonValueDefinition})
					CollectDeclaredTypes(Type, Definition && Definition->Type == EJson::Object ? Definition->AsObject() : nullptr,
						Value, FString(), Types, true);
		}
		// Selecting authored changes is a stored-value decision. Intent tolerance
		// must not erase a small requested edit or replace it with arbitrary readback.
		const UE::UAI::StateSemantic::CanonicalJson::FDeclaredValueComparison Compare =
			[&](const auto& A, const auto& B, const FString& Path) -> TOptional<FValueComparison> {
				if (const auto* Type = Types.Find(Path)) return CompareTypedJson(*Type, A, *Type, B);
				return {};
			};
		const auto Equal = [&](const auto& A, const auto& B, const FString& Path) {
			return CompareJsonValues(A, B, Path, &Compare).bEquivalent;
		};
		const auto PreservesOmittedMembers = [&](const FString& Path) {
			const auto* Declared = Types.Find(Path);
			FString Policy;
			return Declared && Declared->Definition
				&& Declared->Definition->TryGetStringField(TEXT("omitted_patch_field_policy"), Policy)
				&& Policy == TEXT("preserve_current");
		};
		TFunction<TSharedPtr<FJsonValue>(const TSharedPtr<FJsonValue>&, const TSharedPtr<FJsonValue>&,
			const TSharedPtr<FJsonValue>&, const FString&)> Overlay;
		Overlay = [&](const auto& Base, const auto& Input, const auto& Native, const FString& Path) -> TSharedPtr<FJsonValue> {
			if (Equal(Base, Input, Path)) return UeAgentAuthoringState::CloneCanonicalValue(Native);
			if ((!Base || Base->Type == EJson::Object) && Input && Native
				&& Input->Type == EJson::Object && Native->Type == EJson::Object) {
				const auto Result = UeAgentAuthoringState::CloneCanonicalValue(Native)->AsObject();
				const auto* KeyDeclaration = Types.Find(Path);
				if (ObjectKeyType(KeyDeclaration)) {
					// Preserve a conflicting input for the comparator; never turn two
					// requested members into one apparently successful native edit.
					if (!HasDistinctObjectKeys(Input->AsObject(), *KeyDeclaration)) return UeAgentAuthoringState::CloneCanonicalValue(Input);
				}
				// Patch maps edit only supplied members; they are not inventories.
				// The native owner declares this independently of export sparsity.
				if (Base && !PreservesOmittedMembers(Path)) for (const auto& Field : Base->AsObject()->Values)
					if (!FindObjectMember(Input->AsObject(), Field.Key, KeyDeclaration) && !PreservesOmittedMembers(ChildPointer(Path, Field.Key))) {
						FString NativeKey = Field.Key;
						FindObjectMember(Result, Field.Key, KeyDeclaration, &NativeKey);
						Result->RemoveField(NativeKey);
					}
				for (const auto& Field : Input->AsObject()->Values) {
					const auto BaseField = Base ? FindObjectMember(Base->AsObject(), Field.Key, KeyDeclaration) : nullptr;
					const FString FieldPath = ChildPointer(Path, Field.Key);
					FString NativeKey = Field.Key;
					const auto NativeField = FindObjectMember(Native->AsObject(), Field.Key, KeyDeclaration, &NativeKey);
					if (!Equal(BaseField, Field.Value, FieldPath)) Result->SetField(NativeKey,
						Overlay(BaseField, Field.Value, NativeField, FieldPath));
				}
				return MakeShared<FJsonValueObject>(Result);
			}
			if (Input && Native && (!Base || Base->Type == EJson::Array)
				&& Input->Type == EJson::Array && Native->Type == EJson::Array
				&& Input->AsArray().Num() == Native->AsArray().Num()) {
				TArray<TSharedPtr<FJsonValue>> Items;
				for (int32 Index = 0; Index < Input->AsArray().Num(); ++Index)
					Items.Add(Overlay(nullptr, Input->AsArray()[Index], Native->AsArray()[Index], ChildPointer(Path, LexToString(Index))));
				return MakeShared<FJsonValueArray>(MoveTemp(Items));
			}
			return UeAgentAuthoringState::CloneCanonicalValue(Input);
		};
		return Overlay(Before, Desired, Actual, FString());
	}

	bool BuildExpectedPropertyEntry(
		const FString& SemanticId,
		const UeAgentAuthoringIdentity::FCanonicalIdentity& TargetIdentity,
		FProperty* Property,
		const TSharedPtr<FJsonValue>& CanonicalValue,
		UObject* Owner,
		UeAgentAuthoringState::FStateEntry& OutEntry,
		UE::UAI::Contract::FDiagnostic& OutError,
		const UeAgentAuthoringState::FReadLimits* CaptureLimits)
	{
		// Inputs may be members of OutEntry. Retain them before resetting the
		// result, including the identity and semantic string, not only the value.
		const FString ExpectedSemanticId = SemanticId;
		const auto ExpectedIdentity = TargetIdentity;
		const auto ExpectedValue = CanonicalValue;
		TSharedPtr<const UeAgentAuthoringState::FReadLimits> ExpectedLimits;
		if (CaptureLimits) ExpectedLimits = MakeShared<UeAgentAuthoringState::FReadLimits>(*CaptureLimits);
		OutEntry = UeAgentAuthoringState::FStateEntry();
		if (NormalizeId(ExpectedSemanticId).IsEmpty() || !ExpectedIdentity.IsValid() || !Property || !ExpectedValue.IsValid())
		{
			if (NormalizeId(ExpectedSemanticId).IsEmpty()) OutError = MissingInput(TEXT("semantic_expected_property_invalid"), TEXT("The expected property has no semantic identifier."), TEXT("semantic_id"));
			else if (!ExpectedIdentity.IsValid()) OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("semantic_expected_property_invalid"), ExpectedIdentity.GetValidationDiagnostic(), TEXT("semantic_compare"));
			else OutError = MissingInput(TEXT("semantic_expected_property_invalid"), !Property ? TEXT("The expected property descriptor is missing.") : TEXT("The expected property value is missing."), !Property ? TEXT("property") : TEXT("value"));
			OutError.SourceKind = TEXT("native_result");
			return false;
		}
		TSharedPtr<UeAgentAuthoringPropertyValues::FPropertyValueStorage> Storage =
			UeAgentAuthoringPropertyValues::FPropertyValueStorage::CreateInitialized(Property, OutError);
		if (!Storage.IsValid()) return false;
		UeAgentPropertyValueCodec::FOptions CodecOptions;
		// A captured property can intentionally expose only editable struct
		// members. Normalize through that exact read scope, including any
		// domain-authorized members; do not demand a wider native record.
		if (ExpectedLimits) CodecOptions.StructFieldFilter = ExpectedLimits->StructFieldFilter;
		if (!UeAgentPropertyValueCodec::Decode(
			Property,
			ExpectedValue,
			Storage->GetMutableValuePtr(),
			Owner,
			CodecOptions,
			OutError))
		{
			return false;
		}
		UeAgentAuthoringEncodedTypes::FScope EncodedTypes;
		const TSharedPtr<FJsonValue> Reencoded = UeAgentPropertyValueCodec::Encode(
			Property, Storage->GetValuePtr(), Owner, CodecOptions);
		UE::UAI::Contract::FDiagnostic ReliabilityReason;
		if (!UeAgentPropertyValueCodec::IsReliableCanonicalValue(Property, Reencoded, &ReliabilityReason))
		{
			OutError = !ReliabilityReason.IsEmpty() ? ReliabilityReason : UE::UAI::Contract::FDiagnostic::Failure(TEXT("semantic_expected_property_unreliable"), TEXT("The expected property value cannot be encoded reliably."), TEXT("semantic_compare"));
			return false;
		}
		OutEntry.SemanticId = NormalizeId(ExpectedSemanticId);
		OutEntry.TargetIdentity = ExpectedIdentity;
		OutEntry.Type = UeAgentAuthoringTypeDescriptors::Describe(Property);
		OutEntry.Value = UeAgentAuthoringState::CloneCanonicalValue(Reencoded);
		if (const auto Definition = EncodedTypes.BuildDefinition(OutEntry.Value))
			OutEntry.ComparisonValueDefinition = MakeShared<FJsonValueObject>(Definition);
		OutEntry.NativeValue = Storage;
		OutEntry.NativeReadLimits = ExpectedLimits;
		OutEntry.bAvailable = true;
		OutEntry.bEditable = false;
		OutEntry.AccessorId = TEXT("expected.canonical_property");
		Property->ExportTextItem_Direct(
			OutEntry.ValueText, Storage->GetValuePtr(), nullptr, Owner, PPF_None);
		OutEntry.Provenance = TEXT("expected_canonical_value");
		OutError.Reset();
		return true;
	}

	bool BuildExpectedPropertyEntry(
		const UeAgentAuthoringState::FStateEntry& Capture,
		const TSharedPtr<FJsonValue>& CanonicalValue,
		UObject* Owner,
		UeAgentAuthoringState::FStateEntry& OutEntry,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		// Retain storage/scope when a caller normalizes an entry in place.
		const auto Native = Capture.NativeValue;
		const auto Limits = Capture.NativeReadLimits;
		return BuildExpectedPropertyEntry(Capture.SemanticId, Capture.TargetIdentity,
			Native ? Native->GetProperty() : nullptr, CanonicalValue, Owner, OutEntry, OutError, Limits.Get());
	}

	bool ArePropertyValuesEquivalent(
		FProperty* Property,
		const void* LeftValuePtr,
		const void* RightValuePtr,
		UObject* Owner,
		bool& bOutEquivalent,
		UE::UAI::Contract::FDiagnostic& OutError, bool bStoredValues)
	{
		bOutEquivalent = false;
		if (!Property || !LeftValuePtr || !RightValuePtr)
		{
			OutError = MissingInput(TEXT("semantic_property_compare_input_invalid"), !Property ? TEXT("The compared property descriptor is missing.") : !LeftValuePtr ? TEXT("The expected native property storage is missing.") : TEXT("The actual native property storage is missing."), !Property ? TEXT("property") : !LeftValuePtr ? TEXT("expected.value") : TEXT("actual.value"));
			return false;
		}
		UeAgentPropertyValueCodec::FOptions CompareOptions;
		CompareOptions.bCompareStoredValues = bStoredValues;
		const auto Compared = UeAgentPropertyValueCodec::ComparePropertyValues(
			Property,
			RightValuePtr,
			LeftValuePtr,
			Owner,
			CompareOptions);
		if (!Compared.IsComparable()) {
			Compared.ApplyTo(OutError);
			return false;
		}
		bOutEquivalent = Compared.bEquivalent;
		OutError.Reset();
		return true;
	}

	bool CompareEntries(
		const UeAgentAuthoringState::FStateEntry& Left,
		const UeAgentAuthoringState::FStateEntry& Right,
		const FCompareOptions& Options,
		FEntryComparison& OutResult,
		const UeAgentAuthoringSemanticRegistry::FRegistry* SemanticRegistry,
		const FComparatorRegistry* ComparatorRegistry)
	{
		OutResult = FEntryComparison();
		OutResult.SemanticId = Left.SemanticId;
		OutResult.TargetIdentity = Left.TargetIdentity;
		ON_SCOPE_EXIT
		{
			if (!OutResult.Diagnostic.IsEmpty())
			{
				OutResult.Diagnostic.SemanticId = Left.SemanticId;
				if (OutResult.Diagnostic.SubjectId.IsEmpty()) OutResult.Diagnostic.SubjectId = Left.TargetIdentity.CanonicalKey;
				if (OutResult.Diagnostic.PropertyPath.IsEmpty()) OutResult.Diagnostic.PropertyPath = Left.TargetIdentity.GetSelector(TEXT("property_path"));
			}
		};
		if (!Left.IsValid() || !Right.IsValid())
		{
			OutResult.Status = TEXT("failed");
			const bool bExpectedInvalid = !Left.IsValid();
			OutResult.Diagnostic = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("semantic_compare_entry_invalid"),
				(bExpectedInvalid ? Left : Right).GetValidationDiagnostic(), TEXT("semantic_compare"));
			OutResult.Diagnostic.PropertyPath = (bExpectedInvalid ? TEXT("expected.") : TEXT("actual.")) + OutResult.Diagnostic.PropertyPath;
			return false;
		}
		if (Left.SemanticId != Right.SemanticId)
		{
			OutResult.Status = TEXT("different");
			OutResult.Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("semantic_id_mismatch"), TEXT("The compared entries refer to different semantics."), TEXT("semantic_compare"));
			FValueComparison::Different(TEXT("/semantic_id"), MakeShared<FJsonValueString>(Left.SemanticId),
				MakeShared<FJsonValueString>(Right.SemanticId)).ApplyTo(OutResult.Diagnostic);
			return true;
		}
		if (Left.TargetIdentity.CanonicalKey != Right.TargetIdentity.CanonicalKey)
		{
			OutResult.Status = TEXT("different");
			OutResult.Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("semantic_identity_mismatch"), TEXT("The compared entries refer to different targets."), TEXT("semantic_compare"));
			OutResult.Diagnostic.PropertyPath = TEXT("target_identity");
			OutResult.Diagnostic.ExpectedEvidence = MakeShared<FJsonValueString>(Left.TargetIdentity.CanonicalKey);
			OutResult.Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(Right.TargetIdentity.CanonicalKey);
			return true;
		}

		const UeAgentAuthoringSemanticRegistry::FRegistry& Semantics = SemanticRegistry
			? *SemanticRegistry
			: UeAgentAuthoringSemanticRegistry::FRegistry::Get();
		UeAgentAuthoringSemanticRegistry::FSemanticDescriptor Semantic;
		if (!Semantics.FindSemantic(Left.SemanticId, Semantic))
		{
			OutResult.Status = TEXT("failed");
			OutResult.Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("semantic_compare_semantic_unknown"), TEXT("The compared semantic is not registered."), TEXT("semantic_compare"));
			OutResult.Diagnostic.PropertyPath = TEXT("semantic_id"); OutResult.Diagnostic.ActualValue = DescriptorValue(Left.SemanticId);
			return false;
		}
		OutResult.ComparatorId = Semantic.Comparator;
		if (Options.bCompareAvailability
			&& (Left.bAvailable != Right.bAvailable || Left.bEditable != Right.bEditable))
		{
			OutResult.Status = TEXT("different");
			OutResult.Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("semantic_availability_mismatch"), TEXT("The compared entries differ in availability or editability."), TEXT("semantic_compare"));
			const bool bAvailability = Left.bAvailable != Right.bAvailable;
			FValueComparison::Different(bAvailability ? TEXT("/available") : TEXT("/editable"),
				MakeShared<FJsonValueBoolean>(bAvailability ? Left.bAvailable : Left.bEditable),
				MakeShared<FJsonValueBoolean>(bAvailability ? Right.bAvailable : Right.bEditable)).ApplyTo(OutResult.Diagnostic);
			return true;
		}
		if (!Options.bCompareValue || (!Left.bAvailable && !Right.bAvailable))
		{
			OutResult.bEquivalent = true;
			OutResult.Status = TEXT("equivalent");
			OutResult.Diagnostic.Reset();
			return true;
		}

		const FComparatorRegistry& Comparators = ComparatorRegistry ? *ComparatorRegistry : FComparatorRegistry::Get();
		FComparatorDescriptor Descriptor;
		FComparator Comparator;
		if (!Comparators.FindComparator(Semantic.Comparator, Descriptor)
			|| !Comparators.CopyComparator(Semantic.Comparator, Comparator))
		{
			OutResult.Status = TEXT("failed");
			OutResult.Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("semantic_comparator_unknown"), TEXT("The semantic comparator is not registered or has no callable implementation."), TEXT("semantic_compare"));
			OutResult.Diagnostic.PropertyPath = TEXT("comparator_id"); OutResult.Diagnostic.ActualValue = DescriptorValue(Semantic.Comparator);
			return false;
		}
		if (!Descriptor.SupportsValueKind(Semantic.ValueType))
		{
			OutResult.Status = TEXT("failed");
			OutResult.Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("semantic_comparator_value_kind_unsupported"), TEXT("The comparator does not support this semantic value kind."), TEXT("semantic_compare"));
			OutResult.Diagnostic.PropertyPath = TEXT("value_kind"); OutResult.Diagnostic.ActualValue = DescriptorValue(Semantic.ValueType);
			TArray<TSharedPtr<FJsonValue>> Allowed; for (const auto& Kind : Descriptor.ValueKinds) Allowed.Add(DescriptorValue(Kind));
			OutResult.Diagnostic.ExpectedValue = MakeShared<FJsonValueArray>(Allowed); OutResult.Diagnostic.CandidateName = Descriptor.ComparatorId;
			return false;
		}
		UE::UAI::Contract::FDiagnostic ComparatorError;
		if (!Comparator(Left, Right, OutResult.bEquivalent, ComparatorError))
		{
			OutResult.Status = TEXT("failed");
			OutResult.Diagnostic = ComparatorError.IsEmpty()
				? UE::UAI::Contract::FDiagnostic::Failure(TEXT("semantic_comparator_failed"), TEXT("The comparator reported failure without a diagnostic."), TEXT("semantic_compare"))
				: UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("semantic_comparator_failed"), ComparatorError, TEXT("semantic_compare"));
			if (OutResult.Diagnostic.CandidateName.IsEmpty()) OutResult.Diagnostic.CandidateName = Descriptor.ComparatorId;
			return false;
		}
		OutResult.Status = OutResult.bEquivalent ? TEXT("equivalent") : TEXT("different");
		if (!OutResult.bEquivalent)
		{
			OutResult.Diagnostic = ComparatorError;
			if (OutResult.Diagnostic.IsEmpty())
				OutResult.Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("semantic_value_mismatch"), TEXT("The semantic values differ."), TEXT("semantic_compare"));
			else if (OutResult.Diagnostic.Code != TEXT("semantic_value_mismatch"))
			{
				OutResult.Diagnostic = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("semantic_value_mismatch"), ComparatorError, TEXT("semantic_compare"));
			}
		}
		return true;
	}

	bool CompareSnapshots(
		const UeAgentAuthoringState::FStateSnapshot& Left,
		const UeAgentAuthoringState::FStateSnapshot& Right,
		const FCompareOptions& Options,
		FComparisonReport& OutReport,
		const UeAgentAuthoringSemanticRegistry::FRegistry* SemanticRegistry,
		const FComparatorRegistry* ComparatorRegistry)
	{
		OutReport = FComparisonReport();
		const TArray<UeAgentAuthoringState::FStateEntry> LeftEntries = Left.CopyEntries();
		const TArray<UeAgentAuthoringState::FStateEntry> RightEntries = Right.CopyEntries();
		int32 LeftIndex = 0;
		int32 RightIndex = 0;
		while (LeftIndex < LeftEntries.Num() || RightIndex < RightEntries.Num())
		{
			if (LeftIndex >= LeftEntries.Num())
			{
				OutReport.Entries.Add(MakeMissingComparison(RightEntries[RightIndex++], TEXT("semantic_missing_left")));
				continue;
			}
			if (RightIndex >= RightEntries.Num())
			{
				OutReport.Entries.Add(MakeMissingComparison(LeftEntries[LeftIndex++], TEXT("semantic_missing_right")));
				continue;
			}
			if (EntryIdentityLess(LeftEntries[LeftIndex], RightEntries[RightIndex]))
			{
				OutReport.Entries.Add(MakeMissingComparison(LeftEntries[LeftIndex++], TEXT("semantic_missing_right")));
				continue;
			}
			if (EntryIdentityLess(RightEntries[RightIndex], LeftEntries[LeftIndex]))
			{
				OutReport.Entries.Add(MakeMissingComparison(RightEntries[RightIndex++], TEXT("semantic_missing_left")));
				continue;
			}
			FEntryComparison Comparison;
			if (!CompareEntries(
				LeftEntries[LeftIndex++],
				RightEntries[RightIndex++],
				Options,
				Comparison,
				SemanticRegistry,
				ComparatorRegistry))
			{
				OutReport.Entries.Add(Comparison);
				OutReport.Status = TEXT("failed");
				OutReport.Diagnostic = Comparison.Diagnostic;
				return false;
			}
			OutReport.Entries.Add(Comparison);
		}
		OutReport.bEquivalent = !OutReport.Entries.ContainsByPredicate(
			[](const FEntryComparison& Entry) { return !Entry.bEquivalent; });
		OutReport.Status = OutReport.bEquivalent ? TEXT("equivalent") : TEXT("different");
		if (!OutReport.bEquivalent)
		{
			OutReport.Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("semantic_snapshots_different"), TEXT("The semantic snapshots differ."), TEXT("semantic_compare"));
			for (const FEntryComparison& Entry : OutReport.Entries)
				if (!Entry.Diagnostic.IsEmpty()) OutReport.Diagnostic.Diagnostics.Add(Entry.Diagnostic);
		}
		return true;
	}

	bool ValidateSharedCoreBindings(
		FBindingReport& OutReport,
		const UeAgentAuthoringSemanticRegistry::FRegistry* SemanticRegistry,
		const UeAgentAuthoringIdentity::FRegistry* IdentityRegistry,
		const UeAgentAuthoringState::FStateAccessorRegistry* AccessorRegistry,
		const FComparatorRegistry* ComparatorRegistry)
	{
		OutReport = FBindingReport();
		const UeAgentAuthoringSemanticRegistry::FRegistry& Semantics = SemanticRegistry
			? *SemanticRegistry
			: UeAgentAuthoringSemanticRegistry::FRegistry::Get();
		const UeAgentAuthoringIdentity::FRegistry& Identities = IdentityRegistry
			? *IdentityRegistry
			: UeAgentAuthoringIdentity::FRegistry::Get();
		const UeAgentAuthoringState::FStateAccessorRegistry& Accessors = AccessorRegistry
			? *AccessorRegistry
			: UeAgentAuthoringState::FStateAccessorRegistry::Get();
		const FComparatorRegistry& Comparators = ComparatorRegistry
			? *ComparatorRegistry
			: FComparatorRegistry::Get();
		for (const UeAgentAuthoringSemanticRegistry::FSemanticDescriptor& Semantic : Semantics.ListSemantics())
		{
			UeAgentAuthoringSemanticRegistry::FDomainDescriptor Domain;
			if (!Semantics.FindDomain(Semantic.DomainId, Domain))
			{
				AddBindingIssue(OutReport, Semantic.SemanticId, UE::UAI::Contract::FDiagnostic::Failure(
					TEXT("binding_domain_unknown"), FString::Printf(TEXT("The semantic binding references unknown domain '%s'."), *Semantic.DomainId),
					TEXT("semantic_binding"), Semantic.DomainId));
			}
			UeAgentAuthoringIdentity::FModelDescriptor IdentityModel;
			if (!Identities.FindModel(Semantic.IdentityModel, IdentityModel))
			{
				AddBindingIssue(OutReport, Semantic.SemanticId, UE::UAI::Contract::FDiagnostic::Failure(
					TEXT("binding_identity_model_unknown"), FString::Printf(TEXT("The semantic binding references unknown identity model '%s'."), *Semantic.IdentityModel),
					TEXT("semantic_binding"), Semantic.IdentityModel));
			}
			else if (!IdentityModel.TargetKind.IsEmpty() && IdentityModel.TargetKind != Semantic.TargetKind)
			{
				AddBindingIssue(OutReport, Semantic.SemanticId, UE::UAI::Contract::FDiagnostic::Failure(
					TEXT("binding_identity_target_kind_mismatch"), FString::Printf(TEXT("The identity model target kind '%s' differs from the semantic binding."), *IdentityModel.TargetKind),
					TEXT("semantic_binding"), IdentityModel.TargetKind));
			}
			UeAgentAuthoringState::FStateAccessorDescriptor Accessor;
			if (!Accessors.FindAccessor(Semantic.StateAccessor, Accessor))
			{
				AddBindingIssue(OutReport, Semantic.SemanticId, UE::UAI::Contract::FDiagnostic::Failure(
					TEXT("binding_state_accessor_unknown"), FString::Printf(TEXT("The semantic binding references unknown state accessor '%s'."), *Semantic.StateAccessor),
					TEXT("semantic_binding"), Semantic.StateAccessor));
			}
			else if (!Accessor.SupportsTargetKind(Semantic.TargetKind))
			{
				AddBindingIssue(OutReport, Semantic.SemanticId, UE::UAI::Contract::FDiagnostic::Failure(
					TEXT("binding_state_accessor_target_kind_unsupported"), FString::Printf(TEXT("The state accessor does not support target kind '%s'."), *Semantic.TargetKind),
					TEXT("semantic_binding"), Semantic.TargetKind));
			}
			FComparatorDescriptor Comparator;
			if (!Comparators.FindComparator(Semantic.Comparator, Comparator))
			{
				AddBindingIssue(OutReport, Semantic.SemanticId, UE::UAI::Contract::FDiagnostic::Failure(
					TEXT("binding_comparator_unknown"), FString::Printf(TEXT("The semantic binding references unknown comparator '%s'."), *Semantic.Comparator),
					TEXT("semantic_binding"), Semantic.Comparator));
			}
			else if (!Comparator.SupportsValueKind(Semantic.ValueType))
			{
				AddBindingIssue(OutReport, Semantic.SemanticId, UE::UAI::Contract::FDiagnostic::Failure(
					TEXT("binding_comparator_value_kind_unsupported"), FString::Printf(TEXT("The comparator does not support value kind '%s'."), *Semantic.ValueType),
					TEXT("semantic_binding"), Semantic.ValueType));
			}
		}
		OutReport.Issues.Sort([](const FBindingIssue& Left, const FBindingIssue& Right)
		{
			if (Left.Diagnostic.SemanticId != Right.Diagnostic.SemanticId) return Left.Diagnostic.SemanticId < Right.Diagnostic.SemanticId;
			if (Left.Diagnostic.Code != Right.Diagnostic.Code) return Left.Diagnostic.Code < Right.Diagnostic.Code;
			return Left.Diagnostic.InternalContext < Right.Diagnostic.InternalContext;
		});
		OutReport.bValid = OutReport.Issues.IsEmpty();
		OutReport.Status = OutReport.bValid ? TEXT("valid") : TEXT("invalid");
		if (!OutReport.bValid)
			OutReport.Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(
				TEXT("shared_core_bindings_invalid"), TEXT("One or more semantic bindings are invalid; see issues."), TEXT("semantic_binding"));
		return OutReport.bValid;
	}
}

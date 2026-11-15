#include "Core/HjsonWorkspace/UeAgentSemanticLayout.h"
#include "Core/HjsonWorkspace/UeAgentSemanticPublicReferences.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/UeAgentCommittedWorkspaceDelta.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceIO.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceResolver.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Utilities/UeAgentReadableName.h"

#include "Core/HjsonWorkspace/UeAgentKeyedItemAuthoringProjection.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringContentLayout.h"
#include "Core/HjsonWorkspace/UeAgentGraphAuthoringProjection.h"
#include "Core/HjsonWorkspace/UeAgentReflectedPropertyRows.h"
#include "Misc/Paths.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformProcess.h"

#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

#include "Containers/StringConv.h"
#include "Containers/StaticArray.h"
#include "Misc/ScopeExit.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include <atomic>

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace UE::UAI::HjsonWorkspace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::StateSemantic;

	namespace
	{
		thread_local int64 ContentDigestHits = 0, ContentDigestMisses = 0;
		struct FRowDigestWork { double Prepare = 0, Writer = 0, EncodingDigest = 0; int64 Rows = 0, Tokens = 0, Utf8Bytes = 0; };
		thread_local FRowDigestWork* ActiveRowDigestWork = nullptr;
		struct FCompiledMaterialBlock
		{
			int32 First = 0;
			FString FirstUnitId;
			TArray<FSemanticUnit::FStorageIdentity> Units;
			TArray<uint8> Bytes;
			TArray<int32> UnitByteOffsets;
			int64 Items = 0;
			int64 Charge() const
			{
				return sizeof(FCompiledMaterialBlock) + 160 + Units.GetAllocatedSize()
					+ FirstUnitId.GetAllocatedSize() + Bytes.GetAllocatedSize() + UnitByteOffsets.GetAllocatedSize()
					+ int64(Units.Num()) * 32;
			}
		};
		thread_local TWeakPtr<const FAuthoringModel> PreviousCompiledModel;
		thread_local FWorkspaceLayoutPlanPtr PreviousLayout;
		thread_local TMap<EUeAgentResourceDimension, int64> PreviousLayoutCharges;
		// Resolver and capture own different live immutable units. A single last
		// model slot would evict each other; keep only bounded weak-storage memos.
		thread_local TMap<const void*, TSharedPtr<const FCompiledMaterialBlock>> CompiledMaterialBlocks;
		constexpr int64 MaterialProofBudget = 64ll * 1024 * 1024;
		// Identical immutable declaration text is common to many property units.
		// Only weak candidates are indexed: live units own the backing, and a
		// collision or capacity miss simply owns a separate identical string.
		using FUnitStringRef = TSharedRef<const FString, ESPMode::ThreadSafe>;
		thread_local TMap<uint32, TWeakPtr<const FString, ESPMode::ThreadSafe>> UnitStringCandidates;
		thread_local uint32 UnitStringAdmissionMisses = 0;
		FUnitStringRef ShareUnitString(FString Text)
		{
			const uint32 Hash = GetTypeHash(Text);
			if (auto* Candidate = UnitStringCandidates.Find(Hash)) {
				if (const auto Existing = Candidate->Pin()) {
					if (Existing->Equals(Text, ESearchCase::CaseSensitive)) return Existing.ToSharedRef();
					return MakeShared<const FString, ESPMode::ThreadSafe>(MoveTemp(Text));
				}
				const auto Result = MakeShared<const FString, ESPMode::ThreadSafe>(MoveTemp(Text));
				*Candidate = Result; return Result;
			}
			if (UnitStringCandidates.Num() >= 8192 && (++UnitStringAdmissionMisses & 127u) == 0) {
				for (auto It = UnitStringCandidates.CreateIterator(); It; ++It)
					if (!It.Value().IsValid()) It.RemoveCurrent();
			}
			const auto Result = MakeShared<const FString, ESPMode::ThreadSafe>(MoveTemp(Text));
			// Bound the optional weak index independently of the complete model.
			// The 128-byte reserve includes the next sparse-map entry/control block.
			if (UnitStringCandidates.Num() < 8192
				&& UnitStringCandidates.GetAllocatedSize() + 32LL * UnitStringCandidates.Num() + 128 < 2LL * 1024 * 1024)
				UnitStringCandidates.Add(Hash, Result);
			return Result;
		}
		using FUnitStrings = TArray<FString>;
		using FUnitValues = TMap<FString, FUeAgentCanonicalValueRef>;
		template <typename T> struct TUnitContainerCandidate {
			uint32 Hash = 0;
			TWeakPtr<const T> Value;
		};
		constexpr uint32 UnitStringsSlotCount = 1024;
		constexpr uint32 UnitValuesSlotCount = 2048;
		struct FUnitContainerCandidates {
			TStaticArray<TUnitContainerCandidate<FUnitStrings>, UnitStringsSlotCount> Strings;
			TStaticArray<TUnitContainerCandidate<FUnitValues>, UnitValuesSlotCount> Values;
		};
		static_assert(sizeof(FUnitContainerCandidates)
			+ UnitStringsSlotCount * (sizeof(FUnitStrings) + 32)
			+ UnitValuesSlotCount * (sizeof(FUnitValues) + 32) < 512 * 1024,
			"Weak unit-container slots and their coallocated control storage must remain bounded.");
		// Fixed weak indices (including all possible weak control blocks <512KiB).
		// Unit storage owns the containers; a collision only declines admission.
		thread_local TUniquePtr<FUnitContainerCandidates> UnitContainerCandidates;
		TSharedRef<const FUnitStrings> ShareUnitStrings(FUnitStrings&& Values)
		{
			if (Values.Num() > 32) return MakeShared<const FUnitStrings>(MoveTemp(Values));
			uint32 Hash = GetTypeHash(Values.Num());
			for (const auto& Value : Values) Hash = HashCombineFast(Hash, GetTypeHash(Value));
			if (!UnitContainerCandidates) UnitContainerCandidates = MakeUnique<FUnitContainerCandidates>();
			auto& Slot = UnitContainerCandidates->Strings[Hash % UnitStringsSlotCount];
			const auto Previous = Slot.Value.Pin();
			if (Previous && Slot.Hash == Hash && Previous->Num() == Values.Num()) {
				bool bEqual = true;
				for (int32 I = 0; I < Values.Num(); ++I)
					if (!(*Previous)[I].Equals(Values[I], ESearchCase::CaseSensitive)) { bEqual = false; break; }
				if (bEqual) return Previous.ToSharedRef();
			}
			const auto Result = MakeShared<const FUnitStrings>(MoveTemp(Values));
			if (!Previous) { Slot.Hash = Hash; Slot.Value = Result; }
			return Result;
		}
		TSharedRef<const FUnitValues> ShareUnitValues(FUnitValues&& Values)
		{
			if (Values.Num() > 8) return MakeShared<const FUnitValues>(MoveTemp(Values));
			uint32 Hash = GetTypeHash(Values.Num());
			for (const auto& Pair : Values)
				Hash = HashCombineFast(Hash, HashCombineFast(GetTypeHash(Pair.Key), PointerHash(&Pair.Value.Get())));
			if (!UnitContainerCandidates) UnitContainerCandidates = MakeUnique<FUnitContainerCandidates>();
			auto& Slot = UnitContainerCandidates->Values[Hash % UnitValuesSlotCount];
			const auto Previous = Slot.Value.Pin();
			if (Previous && Slot.Hash == Hash && Previous->Num() == Values.Num()) {
				bool bEqual = true;
				auto Prior = Previous->CreateConstIterator();
				for (const auto& Pair : Values) {
					if (!Prior || !Prior.Key().Equals(Pair.Key, ESearchCase::CaseSensitive)
						|| &Prior.Value().Get() != &Pair.Value.Get()) { bEqual = false; break; }
					++Prior;
				}
				if (bEqual) return Previous.ToSharedRef();
			}
			const auto Result = MakeShared<const FUnitValues>(MoveTemp(Values));
			if (!Previous) { Slot.Hash = Hash; Slot.Value = Result; }
			return Result;
		}

		const FString WorkspaceSchema =
			TEXT("uai.authoring.workspace/2");
		const FString ManifestPath =
			TEXT(".uai/workspace.hjson");

		void EncodeSerializedText(const FString& Text, TArray<uint8>& OutBytes)
		{
			// Use the same converter/terminated input as FTCHARToUTF8, but write
			// into the final byte array instead of allocating and copying a second one.
			const int32 SourceLength = FCString::Strlen(*Text);
			const int32 ByteLength = FTCHARToUTF8_Convert::ConvertedLength(*Text, SourceLength);
			OutBytes.SetNumUninitialized(ByteLength);
			FTCHARToUTF8_Convert::Convert(reinterpret_cast<UTF8CHAR*>(OutBytes.GetData()),
				ByteLength, *Text, SourceLength);
		}

		FString Normalize(FString Value)
		{
			Value.TrimStartAndEndInline();
			return Value;
		}

		const TCHAR* UnitKindToken(ESemanticUnitKind Kind)
		{
			switch (Kind)
			{
			case ESemanticUnitKind::WorkspaceIndex:
				return TEXT("workspace_index");
			case ESemanticUnitKind::TargetState:
				return TEXT("target_state");
			case ESemanticUnitKind::EntityState:
				return TEXT("entity_state");
			case ESemanticUnitKind::PropertySet:
				return TEXT("property_set");
			case ESemanticUnitKind::StructureState:
				return TEXT("structure_state");
			case ESemanticUnitKind::ItemState:
				return TEXT("item_state");
			case ESemanticUnitKind::RelationSet:
				return TEXT("relation_set");
			case ESemanticUnitKind::OrderingState:
				return TEXT("ordering_state");
			case ESemanticUnitKind::LayoutState:
				return TEXT("layout_state");
			case ESemanticUnitKind::CollectionState:
				return TEXT("collection_state");
			case ESemanticUnitKind::DependencyState:
				return TEXT("dependency_state");
			case ESemanticUnitKind::ExternalPayloadState:
				return TEXT("external_payload_state");
			case ESemanticUnitKind::OpaqueExtension:
				return TEXT("opaque_extension");
			default:
				return TEXT("");
			}
		}

		const FString& UnitKindName(ESemanticUnitKind Kind)
		{
			static const TArray<FString> Names = [] {
				TArray<FString> Result;
				for (uint8 Index = 0; Index <= static_cast<uint8>(ESemanticUnitKind::OpaqueExtension); ++Index)
					Result.Add(UnitKindToken(static_cast<ESemanticUnitKind>(Index)));
				return Result;
			}();
			static const FString Empty;
			const int32 Index = static_cast<int32>(Kind);
			return Names.IsValidIndex(Index) ? Names[Index] : Empty;
		}

		bool TrySemanticLeafProjection(const FUeAgentCanonicalValue& Value,
			TFunctionRef<bool(int64)> ChargeMemory, TSharedPtr<FJsonValue>& OutValue, UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutValue.Reset(); OutError.Reset();
			const auto Kind = Value.GetKind();
			if (Kind == EUeAgentValueKind::Decimal)
			{
				const FString& Lexeme = Value.GetLexeme();
				if (!CanonicalJson::NeedsDecimalPoint(Lexeme)) return true;
				if (!ChargeMemory(2048 + 4ll * (static_cast<int64>(Lexeme.Len()) + 1) * sizeof(TCHAR))) return false;
				// Keep the decimal kind without introducing a transport object into
				// readable property JSON, which is also consumed directly by codecs.
				OutValue = MakeShared<FJsonValueNumberString>(Lexeme + TEXT(".0"));
				return true;
			}
			if (Kind != EUeAgentValueKind::Temporal && Kind != EUeAgentValueKind::Spatial) return true;
			if (!ChargeMemory(1024) || !CanonicalJson::TryProjectTypedLeaf(Value, ChargeMemory, OutValue, OutError)) return false;
			OutValue->AsObject()->SetStringField(TEXT("$uai"), Kind == EUeAgentValueKind::Temporal ? TEXT("temporal") : TEXT("spatial"));
			return true;
		}

		TSharedPtr<FJsonValue> JsonFromCanonical(
			const FUeAgentCanonicalValue& Root, Contract::FDiagnostic& OutError, const FString& RootPath)
		{
			OutError.Reset();
			struct FVisit
			{
				const FUeAgentCanonicalValue* Value = nullptr;
				bool bExpanded = false;
				FString Path;
			};
			TArray<FVisit> Stack;
			TMap<
				const FUeAgentCanonicalValue*,
				TSharedPtr<FJsonValue>> Converted;
			Stack.Add({&Root, false, RootPath});
			while (!Stack.IsEmpty())
			{
				const FVisit Visit =
					Stack.Pop(EAllowShrinking::No);
				if (Converted.Contains(Visit.Value))
				{
					continue;
				}
				if (!Visit.bExpanded)
				{
					switch (Visit.Value->GetKind())
					{
					case EUeAgentValueKind::Null:
						Converted.Add(
							Visit.Value,
							MakeShared<FJsonValueNull>());
						break;
					case EUeAgentValueKind::Boolean:
						Converted.Add(
							Visit.Value,
							MakeShared<FJsonValueBoolean>(
								Visit.Value->GetBoolean()));
						break;
					case EUeAgentValueKind::String:
						Converted.Add(
							Visit.Value,
							MakeShared<FJsonValueString>(
								Visit.Value->GetText()));
						break;
					case EUeAgentValueKind::Integer:
						Converted.Add(
							Visit.Value,
							MakeShared<FJsonValueNumberString>(
								Visit.Value->GetLexeme()));
						break;
					case EUeAgentValueKind::Decimal:
					{
						TSharedPtr<FJsonValue> Leaf;
						UE::UAI::Contract::FDiagnostic Error;
						if (!TrySemanticLeafProjection(*Visit.Value, [](int64) { return true; }, Leaf, Error))
						{
							UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error, Visit.Path);
							OutError = MoveTemp(Error);
							return nullptr;
						}
						if (!Leaf.IsValid()) Leaf = MakeShared<FJsonValueNumberString>(Visit.Value->GetLexeme());
						Converted.Add(Visit.Value, Leaf);
						break;
					}
					case EUeAgentValueKind::Array:
					case EUeAgentValueKind::Object:
						Stack.Add({Visit.Value, true, Visit.Path});
						if (Visit.Value->GetKind()
							== EUeAgentValueKind::Array)
						{
							const TArray<FUeAgentCanonicalValueRef>&
								Items =
									Visit.Value->GetArrayItems();
							for (int32 Index = Items.Num() - 1;
								Index >= 0;
								--Index)
							{
								Stack.Add(
									{&Items[Index].Get(), false, Visit.Path + TEXT("/") + FString::FromInt(Index)});
							}
						}
						else
						{
							const TArray<TPair<
								FString,
								FUeAgentCanonicalValueRef>>& Items =
									Visit.Value->GetObjectItems();
							for (int32 Index = Items.Num() - 1;
								Index >= 0;
								--Index)
							{
								Stack.Add(
									{&Items[Index].Value.Get(), false, Visit.Path + TEXT("/") + EscapeJsonPointerSegment(Items[Index].Key)});
							}
						}
						break;
					case EUeAgentValueKind::Temporal:
					case EUeAgentValueKind::Spatial:
					{
						TSharedPtr<FJsonValue> Leaf;
						UE::UAI::Contract::FDiagnostic Error;
						if (!TrySemanticLeafProjection(*Visit.Value, [](int64) { return true; }, Leaf, Error))
						{
							UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error, Visit.Path);
							OutError = MoveTemp(Error);
							return nullptr;
						}
						Converted.Add(Visit.Value, Leaf);
						break;
					}
					default:
						OutError = Contract::FDiagnostic::Failure(TEXT("semantic_value_kind_unsupported"),
							TEXT("This canonical value kind cannot be projected into a semantic HJSON row."), TEXT("semantic_projection"));
						OutError.Source.JsonPath = Visit.Path;
						OutError.ActualValue = MakeShared<FJsonValueNumber>(static_cast<int32>(Visit.Value->GetKind()));
						return nullptr;
					}
					continue;
				}
				if (Visit.Value->GetKind()
					== EUeAgentValueKind::Array)
				{
					TArray<TSharedPtr<FJsonValue>> Items;
					for (const FUeAgentCanonicalValueRef& Item :
						Visit.Value->GetArrayItems())
					{
						Items.Add(Converted.FindChecked(&Item.Get()));
					}
					Converted.Add(
						Visit.Value,
						MakeShared<FJsonValueArray>(
							MoveTemp(Items)));
				}
				else
				{
					TSharedRef<FJsonObject> Object =
						MakeShared<FJsonObject>();
					for (const TPair<
						FString,
						FUeAgentCanonicalValueRef>& Item :
						Visit.Value->GetObjectItems())
					{
						if (Object->Values.Contains(Item.Key))
						{
							OutError = Contract::FDiagnostic::Failure(TEXT("semantic_object_key_duplicate"),
								TEXT("A canonical object contains the same field name more than once."), TEXT("semantic_projection"));
							OutError.Source.JsonPath = Visit.Path + TEXT("/") + EscapeJsonPointerSegment(Item.Key);
							OutError.ActualValue = MakeShared<FJsonValueString>(Item.Key);
							return nullptr;
						}
						Object->SetField(
							Item.Key,
							Converted.FindChecked(
								&Item.Value.Get()));
					}
					Converted.Add(
						Visit.Value,
						MakeShared<FJsonValueObject>(Object));
				}
			}
			return Converted.FindChecked(&Root);
		}

		TArray<TSharedPtr<FJsonValue>> JsonStringArray(
			const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Result;
			Result.Reserve(Values.Num());
			for (const FString& Value : Values)
			{
				Result.Add(MakeShared<FJsonValueString>(Value));
			}
			return Result;
		}

		template<typename RefMap> const FString* FindProjectedReference(const RefMap& Refs, const FString& Id) { return Refs.Find(Id); }
		const FString* FindProjectedReference(const FWorkspacePlacementView& Refs, const FString& Id) { return Refs.FindReference(Id); }

		template<typename RefMap> TArray<TSharedPtr<FJsonValue>> PublicRefArray(
			const TArray<FString>& Values,
			const RefMap* PublicRefs)
		{
			TArray<TSharedPtr<FJsonValue>> Result;
			Result.Reserve(Values.Num());
			for (const FString& Value : Values)
			{
				const FString* Public = PublicRefs == nullptr
					? nullptr : FindProjectedReference(*PublicRefs, Value);
				Result.Add(MakeShared<FJsonValueString>(
					Public == nullptr ? Value : *Public));
			}
			return Result;
		}

		template<typename RefMap> void ProjectPublicRefs(
			TSharedPtr<FJsonValue>& Value,
			const RefMap& PublicRefs)
		{
			if (!Value.IsValid())
			{
				return;
			}
			if (Value->Type == EJson::String)
			{
				if (const FString* Public = FindProjectedReference(PublicRefs, Value->AsString()))
				{
					Value = MakeShared<FJsonValueString>(*Public);
				}
				return;
			}
			if (Value->Type == EJson::Array)
			{
				TArray<TSharedPtr<FJsonValue>> Items = Value->AsArray();
				for (TSharedPtr<FJsonValue>& Item : Items)
				{
					ProjectPublicRefs(Item, PublicRefs);
				}
				Value = MakeShared<FJsonValueArray>(MoveTemp(Items));
				return;
			}
			if (Value->Type == EJson::Object)
			{
				const TSharedPtr<FJsonObject> Source = Value->AsObject();
				const TSharedRef<FJsonObject> Projected = MakeShared<FJsonObject>();
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Field
					: Source->Values)
				{
					TSharedPtr<FJsonValue> ProjectedValue = Field.Value;
					ProjectPublicRefs(ProjectedValue, PublicRefs);
					const FString* PublicKey = FindProjectedReference(PublicRefs, Field.Key);
					Projected->SetField(
						PublicKey == nullptr ? Field.Key : *PublicKey,
						MoveTemp(ProjectedValue));
				}
				Value = MakeShared<FJsonValueObject>(Projected);
			}
		}

		// Only the writer consumes this row envelope. Borrow the sealed unit's
		// fields instead of copying value maps and interning temporary headers.
		struct FSemanticUnitRowView
		{
			const FString& Kind;
			TArray<FUeAgentStructuredHjsonObjectFieldView, TInlineAllocator<16>> Fields;
			TOptional<FUeAgentStructuredHjsonObjectFieldView> InlineValueField;
			FUeAgentStructuredHjsonObjectView ValueObject;
			FUeAgentStructuredHjsonObjectView Object;
			explicit FSemanticUnitRowView(const FSemanticUnit& Unit) : Kind(UnitKindName(Unit.GetUnitKind()))
			{
				static const FString Keys[] = {
					TEXT("unit_id"), TEXT("unit_kind"), TEXT("owner_ref"), TEXT("semantic_type_ref"),
					TEXT("role"), TEXT("field_set_ref"), TEXT("item_refs"), TEXT("relation_refs"),
					TEXT("ordering_ref"), TEXT("layout_ref"), TEXT("dependency_refs"),
					TEXT("profile_visibility"), TEXT("target_identity_key"), TEXT("values")};
				// Supply the original formatter's exact final lexical order.
				if (!Unit.GetDependencyRefs().IsEmpty()) Fields.Emplace(Keys[10], Unit.GetDependencyRefs());
				Fields.Emplace(Keys[5], Unit.GetFieldSetRef());
				if (!Unit.GetItemRefs().IsEmpty()) Fields.Emplace(Keys[6], Unit.GetItemRefs());
				if (!Unit.GetLayoutRef().IsEmpty()) Fields.Emplace(Keys[9], Unit.GetLayoutRef());
				if (!Unit.GetOrderingRef().IsEmpty()) Fields.Emplace(Keys[8], Unit.GetOrderingRef());
				Fields.Emplace(Keys[2], Unit.GetOwnerRef());
				Fields.Emplace(Keys[11], Unit.GetProfileVisibility());
				if (!Unit.GetRelationRefs().IsEmpty()) Fields.Emplace(Keys[7], Unit.GetRelationRefs());
				Fields.Emplace(Keys[4], Unit.GetRole());
				Fields.Emplace(Keys[3], Unit.GetSemanticTypeRef());
				if (!Unit.GetTargetIdentityKey().IsEmpty()) Fields.Emplace(Keys[12], Unit.GetTargetIdentityKey());
				Fields.Emplace(Keys[0], Unit.GetUnitId()); Fields.Emplace(Keys[1], Kind);
				if (Unit.HasInlinePropertyValue()) {
					Unit.VisitValues([&](const FString& Key, const auto& Value) { InlineValueField.Emplace(Key, Value.Get()); return true; });
					ValueObject.Fields = MakeArrayView(&InlineValueField.GetValue(), 1);
					Fields.Emplace(Keys[13], ValueObject);
				} else Fields.Emplace(Keys[13], Unit.GetValues());
				Object.Fields = MakeArrayView(Fields.GetData(), Fields.Num());
			}
			FSemanticUnitRowView(const FSemanticUnitRowView&) = delete;
			FSemanticUnitRowView& operator=(const FSemanticUnitRowView&) = delete;
		};

		template<typename RefMap> TSharedPtr<FJsonObject> SemanticModelUnitJson(
			const FSemanticUnit& Unit, FWorkspaceError& OutError,
			const RefMap* PublicRefs)
		{
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			TSharedRef<FJsonObject> Values = MakeShared<FJsonObject>();
			TArray<FString> ValueKeys;
			Unit.VisitValues([&](const FString& Key, const auto&) { ValueKeys.Add(Key); return true; });
			ValueKeys.Sort();
			for (const FString& Key : ValueKeys)
			{
				Contract::FDiagnostic Error;
				TSharedPtr<FJsonValue> Value = JsonFromCanonical(
					Unit.FindValue(Key)->Get(), Error, TEXT("/values/") + EscapeJsonPointerSegment(Key));
				if (!Value.IsValid())
				{
					OutError.SetFailure(MoveTemp(Error), Unit.GetUnitId());
					return nullptr;
				}
				if (PublicRefs != nullptr)
				{
					ProjectPublicRefs(Value, *PublicRefs);
				}
				Values->SetField(Key, MoveTemp(Value));
			}
			Row->SetObjectField(TEXT("values"), Values);
			if (Unit.GetUnitKind() == ESemanticUnitKind::ItemState && !Unit.GetFieldSetRef().IsEmpty())
			{
				const FString* PublicRef = PublicRefs ? FindProjectedReference(*PublicRefs, Unit.GetFieldSetRef()) : nullptr;
				Row->SetStringField(TEXT("definition"), PublicRef ? *PublicRef : Unit.GetFieldSetRef());
			}
			if (Unit.GetUnitKind() == ESemanticUnitKind::PropertySet
				&& !Unit.GetFieldSetRef().IsEmpty())
			{
				Row->SetStringField(TEXT("field_set_ref"), Unit.GetFieldSetRef());
			}
			if (Unit.GetUnitKind() == ESemanticUnitKind::PropertySet
				&& !Unit.GetDependencyRefs().IsEmpty())
			{
				Row->SetArrayField(
					TEXT("dependency_refs"),
					PublicRefArray(Unit.GetDependencyRefs(), PublicRefs));
			}
			return Row;
		}

		struct FSemanticStringRefs : DefaultKeyFuncs<const FString*>
		{
			static bool Matches(const FString* A, const FString* B) { return *A == *B; }
			static uint32 GetKeyHash(const FString* Key) { return GetTypeHash(*Key); }
		};

		bool NormalizeSet(TArray<FString>& Values, Contract::FDiagnostic& Error,
			const FString& Code, const FString& Field, bool bPublicValues = false)
		{
			// The input array does not grow or move while its references are read.
			// Sorting follows the last lookup; destruction never dereferences keys.
			TSet<const FString*, FSemanticStringRefs, TInlineSetAllocator<8>> Seen;
			for (int32 Index = 0; Index < Values.Num(); ++Index)
			{
				FString& Value = Values[Index];
				Value = Normalize(MoveTemp(Value));
				bool bDuplicate = false;
				if (!Value.IsEmpty()) Seen.Add(&Value, &bDuplicate);
				if (Value.IsEmpty() || bDuplicate) {
					Error = Contract::FDiagnostic::Failure(Code, Value.IsEmpty()
						? TEXT("The reference list contains an empty item.") : TEXT("The reference list contains a duplicate item."), TEXT("semantic_model"));
					Error.SemanticPointer = TEXT("/") + Field + TEXT("/") + FString::FromInt(Index);
					if (bPublicValues || Value.IsEmpty()) Error.ActualValue = MakeShared<FJsonValueString>(Value);
					Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("nonempty unique reference"));
					return false;
				}
			}
			Values.Sort();
			return true;
		}

		// The original material grammar can write directly to UTF-8. Token
		// length is UTF-8 bytes, and every existing delimiter/order is retained.
		struct FUtf8Material
		{
			TArray<uint8> Bytes;
			void AppendConverted(const TCHAR* Text, int32 SourceLength, int32 ByteLength) {
				if (ByteLength == 0) return;
				const int32 Offset = Bytes.AddUninitialized(ByteLength);
				FTCHARToUTF8_Convert::Convert(reinterpret_cast<UTF8CHAR*>(Bytes.GetData() + Offset),
					ByteLength, Text, SourceLength);
			}
			void Append(const FString& Text) {
				AppendConverted(*Text, Text.Len(), FTCHARToUTF8_Convert::ConvertedLength(*Text, Text.Len()));
			}
			void Append(const TCHAR* Text) {
				const int32 SourceLength = FCString::Strlen(Text);
				AppendConverted(Text, SourceLength, FTCHARToUTF8_Convert::ConvertedLength(Text, SourceLength));
			}
			void AppendChar(TCHAR Character) { check(Character >= 0 && Character < 128); Bytes.Add(uint8(Character)); }
			void AppendInt(int32 Value) {
				uint8 Digits[10]; int32 Count = 0;
				uint32 Magnitude = Value < 0 ? uint32(-int64(Value)) : uint32(Value);
				do { Digits[Count++] = uint8('0' + Magnitude % 10); Magnitude /= 10; } while (Magnitude);
				if (Value < 0) Bytes.Add(uint8('-'));
				while (Count) Bytes.Add(Digits[--Count]);
			}
			void Reserve(int32 Size) { Bytes.Reserve(Size); }
			void Reset() { Bytes.Reset(); }
			int32 Len() const { return Bytes.Num(); }
		};
		void AppendToken(FUtf8Material& Material, const TCHAR* Prefix, const FString& Value)
		{
			const int32 ByteLength = FTCHARToUTF8_Convert::ConvertedLength(*Value, Value.Len());
			Material.Append(Prefix); Material.AppendInt(ByteLength); Material.AppendChar(TEXT(':'));
			Material.AppendConverted(*Value, Value.Len(), ByteLength);
			Material.AppendChar(TEXT(';'));
		}

		void AppendToken(
			FString& Material,
			const TCHAR* Prefix,
			const FString& Value)
		{
			const FTCHARToUTF8 Utf8(*Value, Value.Len());
			Material.Append(Prefix);
			Material.AppendInt(Utf8.Length());
			Material.AppendChar(TEXT(':'));
			Material.Append(Value);
			Material.AppendChar(TEXT(';'));
		}

		template<typename TMaterial> void AppendStringArray(
			TMaterial& Material,
			const TArray<FString>& Values)
		{
			Material.AppendChar(TEXT('a'));
			Material.AppendInt(Values.Num());
			Material.AppendChar(TEXT('['));
			for (const FString& Value : Values)
			{
				AppendToken(Material, TEXT("s"), Value);
			}
			Material.AppendChar(TEXT(']'));
		}

		struct FValueEvent
		{
			const FUeAgentCanonicalValue* Value = nullptr;
			const FString* Key = nullptr;
			FString Literal;

			static FValueEvent ForValue(
				const FUeAgentCanonicalValue& InValue)
			{
				FValueEvent Event;
				Event.Value = &InValue;
				return Event;
			}

			static FValueEvent ForLiteral(FString InLiteral)
			{
				FValueEvent Event;
				Event.Literal = MoveTemp(InLiteral);
				return Event;
			}
			static FValueEvent ForKey(const FString& InKey) {
				FValueEvent Event; Event.Key = &InKey; return Event;
			}
		};

		template<typename TMaterial> void AppendTemporalMaterial(
			TMaterial& Material,
			const FUeAgentTemporalPoint& Value)
		{
			Material.Append(TEXT("temporal{"));
			AppendToken(Material, TEXT("s"), Value.GetClockId());
			AppendToken(Material, TEXT("s"), Value.GetTick());
			AppendToken(Material, TEXT("s"), Value.GetRateNumerator());
			AppendToken(Material, TEXT("s"), Value.GetRateDenominator());
			AppendToken(Material, TEXT("s"), Value.GetState());
			Material.AppendChar(TEXT('}'));
		}

		template<typename TMaterial> void AppendSpatialMaterial(
			TMaterial& Material,
			const FUeAgentSpatialValue& Value)
		{
			Material.Append(TEXT("spatial{"));
			AppendToken(Material, TEXT("s"), Value.GetSubject());
			Material.AppendInt(
				static_cast<int32>(Value.GetQuantityKind()));
			const FUeAgentSpatialFrame& Frame = Value.GetFrame();
			AppendToken(Material, TEXT("s"), Frame.GetFrameId());
			AppendToken(Material, TEXT("s"), Frame.GetBasisFrameId());
			Material.AppendInt(Frame.GetDimension());
			Material.Append(TEXT("basis["));
			for (const TArray<FString>& Row : Frame.GetBasis())
			{
				AppendStringArray(Material, Row);
			}
			Material.AppendChar(TEXT(']'));
			AppendStringArray(Material, Frame.GetOrigin());
			AppendToken(Material, TEXT("s"), Frame.GetUnit());
			AppendToken(Material, TEXT("s"), Frame.GetBasisEvidence());
			AppendTemporalMaterial(Material, Frame.GetObservedAt());
			AppendStringArray(Material, Value.GetComponents());
			AppendTemporalMaterial(Material, Value.GetObservedAt());
			AppendStringArray(
				Material,
				Value.GetConversionEvidence());
			Material.AppendChar(TEXT('}'));
		}

		template<typename TMaterial> void AppendCanonicalValueMaterial(
			TMaterial& Material,
			const FUeAgentCanonicalValue& Root)
		{
			// Most property values fit this traversal without a heap allocation.
			// Deep/wide values retain the same dynamic growth and event order.
			TArray<FValueEvent, TInlineAllocator<32>> Stack;
			Stack.Reserve(32);
			Stack.Add(FValueEvent::ForValue(Root));
			while (!Stack.IsEmpty())
			{
				FValueEvent Event = Stack.Pop(EAllowShrinking::No);
				if (Event.Key) { AppendToken(Material, TEXT("s"), *Event.Key); continue; }
				if (Event.Value == nullptr)
				{
					Material.Append(Event.Literal);
					continue;
				}
				const FUeAgentCanonicalValue& Value = *Event.Value;
				switch (Value.GetKind())
				{
				case EUeAgentValueKind::Null:
					Material.Append(TEXT("n;"));
					break;
				case EUeAgentValueKind::Boolean:
					Material.Append(
						Value.GetBoolean()
							? TEXT("b1;")
							: TEXT("b0;"));
					break;
				case EUeAgentValueKind::String:
					AppendToken(
						Material,
						TEXT("s"),
						Value.GetText());
					break;
				case EUeAgentValueKind::Integer:
					AppendToken(
						Material,
						TEXT("i"),
						Value.GetLexeme());
					break;
				case EUeAgentValueKind::Decimal:
					AppendToken(
						Material,
						TEXT("d"),
						Value.GetLexeme());
					break;
				case EUeAgentValueKind::Array:
				{
					const TArray<FUeAgentCanonicalValueRef>& Items =
						Value.GetArrayItems();
					Material.AppendChar(TEXT('a'));
					Material.AppendInt(Items.Num());
					Material.AppendChar(TEXT('['));
					Stack.Add(FValueEvent::ForLiteral(TEXT("]")));
					for (int32 Index = Items.Num() - 1;
						Index >= 0;
						--Index)
					{
						Stack.Add(
							FValueEvent::ForValue(Items[Index].Get()));
						if (Index > 0)
						{
							Stack.Add(
								FValueEvent::ForLiteral(TEXT(",")));
						}
					}
					break;
				}
				case EUeAgentValueKind::Object:
				{
					// Canonical objects are case-sensitive sorted, whereas the
					// established material uses FString's case-insensitive order.
					// Reuse only a strictly ordered sequence under that exact old
					// comparator; ties and different order retain the original sort.
					const auto& CanonicalItems = Value.GetObjectItems();
					bool bMaterialOrdered = true;
					for (int32 I = 1; I < CanonicalItems.Num(); ++I)
						if (!(CanonicalItems[I - 1].Key < CanonicalItems[I].Key)) { bMaterialOrdered = false; break; }
					TArray<const TPair<FString, FUeAgentCanonicalValueRef>*> SortedItems;
					if (!bMaterialOrdered) {
						for (const auto& Item : CanonicalItems) SortedItems.Add(&Item);
						SortedItems.Sort([](const auto& Left, const auto& Right) { return Left.Key < Right.Key; });
					}
					Material.AppendChar(TEXT('o'));
					Material.AppendInt(CanonicalItems.Num());
					Material.AppendChar(TEXT('{'));
					Stack.Add(FValueEvent::ForLiteral(TEXT("}")));
					for (int32 Index = CanonicalItems.Num() - 1;
						Index >= 0;
						--Index)
					{
						const auto& Item = bMaterialOrdered ? CanonicalItems[Index] : *SortedItems[Index];
						Stack.Add(FValueEvent::ForValue(
							Item.Value.Get()));
						Stack.Add(FValueEvent::ForKey(Item.Key));
						if (Index > 0)
						{
							Stack.Add(
								FValueEvent::ForLiteral(TEXT(",")));
						}
					}
					break;
				}
				case EUeAgentValueKind::Temporal:
					AppendTemporalMaterial(
						Material,
						*Value.GetTemporal());
					break;
				case EUeAgentValueKind::Spatial:
					AppendSpatialMaterial(
						Material,
						*Value.GetSpatial());
					break;
				default:
					Material.Append(TEXT("invalid;"));
					break;
				}
			}
		}

		FString FormatSha256Digest(const uint8* Digest)
		{
			FString Hex;
			Hex.Reserve(SHA256_DIGEST_LENGTH * 2);
			for (int32 Index = 0; Index < SHA256_DIGEST_LENGTH; ++Index)
			{
				Hex.Appendf(TEXT("%02x"), Digest[Index]);
			}
			return TEXT("sha256:") + Hex;
		}
		FString HashBytes(const uint8* Data, int32 Size)
		{
			uint8 Digest[SHA256_DIGEST_LENGTH] = {};
			SHA256(Data, Size, Digest);
			return FormatSha256Digest(Digest);
		}
		FString HashText(const FString& Value)
		{
			const FTCHARToUTF8 Utf8(*Value, Value.Len());
			return HashBytes(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		}

		template<typename TMaterial> void AppendUnitMaterial(TMaterial& Material, const FSemanticUnit& Unit)
		{
			AppendToken(Material, TEXT("s"), Unit.GetUnitId());
			AppendToken(
				Material,
				TEXT("s"),
				UnitKindName(Unit.GetUnitKind()));
			AppendToken(Material, TEXT("s"), Unit.GetOwnerRef());
			AppendToken(
				Material,
				TEXT("s"),
				Unit.GetSemanticTypeRef());
			AppendToken(Material, TEXT("s"), Unit.GetRole());
			AppendToken(Material, TEXT("s"), Unit.GetFieldSetRef());
			Material.Append(TEXT("values{"));
			if (Unit.GetValueCount() == 1) {
				const auto& Value = *Unit.GetValues().CreateConstIterator();
				AppendToken(Material, TEXT("s"), Value.Key);
				AppendCanonicalValueMaterial(Material, Value.Value.Get());
			} else {
				TArray<FString> Keys;
				Unit.GetValues().GetKeys(Keys);
				Keys.Sort();
				for (const FString& Key : Keys) {
					AppendToken(Material, TEXT("s"), Key);
					AppendCanonicalValueMaterial(Material, Unit.GetValues().FindChecked(Key).Get());
				}
			}
			Material.AppendChar(TEXT('}'));
			AppendStringArray(Material, Unit.GetItemRefs());
			AppendStringArray(Material, Unit.GetRelationRefs());
			AppendToken(Material, TEXT("s"), Unit.GetOrderingRef());
			AppendToken(Material, TEXT("s"), Unit.GetLayoutRef());
			AppendStringArray(Material, Unit.GetDependencyRefs());
			AppendStringArray(
				Material,
				Unit.GetProfileVisibility());
			AppendToken(
				Material,
				TEXT("s"),
				Unit.GetTargetIdentityKey());
		}

		bool Fail(
			FWorkspaceError& OutError,
			const FString& Code,
			const FString& Reason,
			const FString& SourceUnit = FString(),
			TArray<FString> CyclePath = {})
		{
			OutError.SetSourceFailure(
				Code, Reason,
				SourceUnit,
				TEXT("/"),
				0,
				false,
				MoveTemp(CyclePath));
			return false;
		}

		bool FailResource(
			FWorkspaceError& OutError,
			const FUeAgentResourceChargeError& Error)
		{
			OutError.SetFailure(Error);
			return false;
		}

		void VisitUnitReferences(const FSemanticUnit& Unit, TFunctionRef<void(const FString&)> Visit)
		{
			if (!Unit.GetOwnerRef().IsEmpty())
			{
				Visit(Unit.GetOwnerRef());
			}
			for (const FString& Ref : Unit.GetItemRefs()) Visit(Ref);
			for (const FString& Ref : Unit.GetRelationRefs()) Visit(Ref);
			if (!Unit.GetOrderingRef().IsEmpty())
			{
				Visit(Unit.GetOrderingRef());
			}
			if (!Unit.GetLayoutRef().IsEmpty())
			{
				Visit(Unit.GetLayoutRef());
			}
			for (const FString& Ref : Unit.GetDependencyRefs()) Visit(Ref);
		}

		FString SafeSegment(const FString& Value)
		{
			const FString Lower = Value.ToLower();
			FString Result;
			bool bLastWasDash = false;
			for (TCHAR Character : Lower)
			{
				const bool bAllowed =
				FChar::IsAlnum(Character)
					|| Character == TEXT('.')
					|| Character == TEXT('_')
					|| Character == TEXT('-');
				if (bAllowed)
				{
					Result.AppendChar(Character);
					bLastWasDash = Character == TEXT('-');
				}
				else if (!bLastWasDash)
				{
					Result.AppendChar(TEXT('-'));
					bLastWasDash = true;
				}
			}
			Result.TrimStartAndEndInline();
			Result.TrimCharInline(TEXT('.'), nullptr);
			Result.TrimCharInline(TEXT('-'), nullptr);
			Result.TrimCharInline(TEXT('_'), nullptr);
			return Result.IsEmpty() ? Result : MakeAuthoringContentPathSegment(Result);
		}

		bool IsExplicitGuidName(const FString& Candidate)
		{
			FGuid Guid;
			return (FGuid::ParseExact(Candidate, EGuidFormats::Digits, Guid)
				|| FGuid::ParseExact(Candidate, EGuidFormats::DigitsWithHyphens, Guid)) && Guid.IsValid();
		}

		FString ReadObjectString(
			const FUeAgentCanonicalValue& Value,
			const TCHAR* Field)
		{
			if (Value.GetKind() != EUeAgentValueKind::Object)
			{
				return FString();
			}
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
				: Value.GetObjectItems())
			{
				if (Item.Key == Field
					&& Item.Value->GetKind() == EUeAgentValueKind::String)
				{
					return Item.Value->GetText().TrimStartAndEnd();
				}
			}
			return FString();
		}

		FString ReadSemanticString(
			const FSemanticUnit& Unit,
			const TCHAR* Field)
		{
			if (const FUeAgentCanonicalValueRef* Direct =
				Unit.FindValue(Field))
			{
				if ((*Direct)->GetKind() == EUeAgentValueKind::String)
				{
					const FString Name = (*Direct)->GetText().TrimStartAndEnd();
					if (!Name.IsEmpty()) return Name;
				}
			}

			TArray<FString> RootFields;
			Unit.VisitValues([&](const FString& Key, const auto&) { RootFields.Add(Key); return true; });
			RootFields.Sort();
			for (const FString& RootField : RootFields)
			{
				const FUeAgentCanonicalValueRef& Value =
					*Unit.FindValue(RootField);
				FString Result = ReadObjectString(Value.Get(), Field);
				if (!Result.IsEmpty())
				{
					return Result;
				}
			}
			return FString();
		}

		FString ReadPathLeaf(FString Value)
		{
			Value.TrimStartAndEndInline();
			Value.ReplaceInline(TEXT("\\"), TEXT("/"));
			int32 Separator = INDEX_NONE;
			if (Value.FindLastChar(TEXT('/'), Separator))
			{
				Value = Value.Mid(Separator + 1);
			}
			int32 ObjectSeparator = INDEX_NONE;
			int32 SubobjectSeparator = INDEX_NONE;
			if (Value.FindChar(TEXT(':'), SubobjectSeparator))
			{
				// A subobject path ends in its object name, not the owning asset.
				Value = Value.Mid(SubobjectSeparator + 1);
				if (Value.FindLastChar(TEXT('.'), ObjectSeparator))
					Value = Value.Mid(ObjectSeparator + 1);
				return Value.TrimStartAndEnd();
			}
			if (Value.FindChar(TEXT('.'), ObjectSeparator)
				&& ObjectSeparator > 0)
			{
				Value = Value.Left(ObjectSeparator);
			}
			return Value.TrimStartAndEnd();
		}

		FString SemanticBusinessName(const FSemanticUnit& Unit)
		{
			// Aggregated item files and their semantic references use one naming owner.
			if (Unit.GetUnitKind() == ESemanticUnitKind::ItemState)
			{
				for (const FKeyedItemProjectionContract& Contract : GetKeyedItemProjectionContracts())
				{
					if (Contract.AggregatedItemSemanticSuffix.IsEmpty()
						|| !Unit.GetSemanticTypeRef().EndsWith(Contract.AggregatedItemSemanticSuffix)) continue;
					for (const FString& Field : Contract.NamingPolicy.Fields)
					{
						const FString Candidate = MakeReadableSemanticAlias(ReadSemanticString(Unit, *Field));
						if (!Candidate.IsEmpty()) return Candidate;
					}
					break;
				}
			}
			for (const TCHAR* Field : {
				TEXT("name"), TEXT("label"), TEXT("display_name"),
				TEXT("asset_name"), TEXT("component_name"),
				TEXT("event_name"), TEXT("function_name"),
				TEXT("variable_name"), TEXT("node_name"),
				TEXT("candidate_name"), TEXT("renderer_name"), TEXT("title")})
			{
				const FString Candidate = MakeReadableSemanticAlias(
					ReadSemanticString(Unit, Field));
				if (!Candidate.IsEmpty())
				{
					return Candidate;
				}
			}
			return FString();
		}

		FString SemanticAliasBase(const FSemanticUnit& Unit)
		{
			const FString BusinessName = SemanticBusinessName(Unit);
			if (!BusinessName.IsEmpty()) return BusinessName;
			if (Unit.GetRole() == TEXT("property_definition"))
			{
				// Definitions share their captured type name across every consumer;
				// the interned identity remains solely in the machine ref mapping.
				for (const TCHAR* Field : {TEXT("struct_path"), TEXT("enum_path"),
					TEXT("class_path"), TEXT("type_id"), TEXT("kind")})
				{
					const FString TypeName = ReadPathLeaf(ReadSemanticString(Unit, Field));
					if (!TypeName.IsEmpty() && !IsOpaqueSemanticIdentity(TypeName))
						return MakeReadableSemanticAlias(TypeName);
				}
			}
			for (const TCHAR* Field : {
				TEXT("asset_path"), TEXT("object_path"), TEXT("target_ref")})
			{
				const FString Source = ReadPathLeaf(ReadSemanticString(Unit, Field));
				if (IsOpaqueSemanticIdentity(Source)) continue;
				const FString Candidate = MakeReadableSemanticAlias(Source);
				if (!Candidate.IsEmpty())
				{
					return Candidate;
				}
			}

			for (const FString& Fallback : {
				Unit.GetFieldSetRef(), Unit.GetRole(),
				Unit.GetSemanticTypeRef(), UnitKindName(Unit.GetUnitKind())})
			{
				if (IsOpaqueSemanticIdentity(Fallback)) continue;
				const FString Candidate = MakeReadableSemanticAlias(Fallback);
				if (!Candidate.IsEmpty())
				{
					return Candidate;
				}
			}
			return TEXT("unit");
		}

		template <typename TUnit> FString SemanticAliasScope(const TUnit& Unit)
		{
			if (Unit.GetUnitKind() == ESemanticUnitKind::StructureState
				&& IsGraphAuthoringStructureType(Unit.GetSemanticTypeRef()))
			{
				return TEXT("graphs");
			}
			switch (Unit.GetUnitKind())
			{
			case ESemanticUnitKind::TargetState:
				return TEXT("targets");
			case ESemanticUnitKind::EntityState:
				return TEXT("entities");
			case ESemanticUnitKind::StructureState:
				return TEXT("structures");
			case ESemanticUnitKind::ExternalPayloadState:
				return TEXT("external");
			case ESemanticUnitKind::OpaqueExtension:
				return TEXT("extensions");
			default:
				return UnitKindName(Unit.GetUnitKind())
					+ TEXT("/") + Unit.GetOwnerRef();
			}
		}

		bool IsAllocatedReadableName(const FString& Name, const FString& Base)
		{
			if (Name.Equals(Base, ESearchCase::CaseSensitive)) return true;
			const FString Prefix = Base + TEXT("#");
			if (!Name.StartsWith(Prefix, ESearchCase::CaseSensitive)) return false;
			const FString Ordinal = Name.Mid(Prefix.Len());
			if (Ordinal.IsEmpty() || Ordinal[0] == TEXT('0')) return false;
			for (TCHAR Character : Ordinal)
				if (Character < TEXT('0') || Character > TEXT('9')) return false;
			return true;
		}

		FString StableProjectionName(const FString& Base, const FString& UnitId)
		{
			// Identity, not content or rank. A singleton receives its final suffix
			// immediately; later members cannot take its name or move its path.
			return Base.Left(24) + GetStableProjectionIdentitySuffix(UnitId);
		}

		bool IsStableOrLegacyProjectionName(const FString& Name, const FString& Base, const FString& UnitId)
		{
			return IsAllocatedReadableName(Name, Base)
				|| Name.Equals(StableProjectionName(Base, UnitId), ESearchCase::CaseSensitive);
		}

		bool ReadBoundProjectionAlias(const FSemanticUnit& Unit, const FString& PublicRef, FString& Alias)
		{
			const FString Prefix = UnitKindName(Unit.GetUnitKind()).Replace(TEXT("_state"), TEXT("")) + TEXT(":");
			if (!PublicRef.StartsWith(Prefix, ESearchCase::CaseSensitive)) return false;
			Alias = PublicRef.Mid(Prefix.Len());
			const FString Base = SemanticAliasBase(Unit);
			if (IsStableOrLegacyProjectionName(Alias, Base, Unit.GetUnitId())) return true;
			// The old global allocator appended -N after the scoped alias. Read
			// only that grammar beside its accepted unit, never by matching a name.
			int32 Dash = INDEX_NONE;
			if (!Alias.FindLastChar(TEXT('-'), Dash)) return false;
			const FString Ordinal = Alias.Mid(Dash + 1);
			if (Ordinal.IsEmpty() || Ordinal[0] == TEXT('0') || Ordinal == TEXT("1")) return false;
			for (TCHAR Character : Ordinal)
				if (Character < TEXT('0') || Character > TEXT('9')) return false;
			Alias.LeftInline(Dash);
			return IsAllocatedReadableName(Alias, Base);
		}

		bool SameProjectionBindingIdentity(const FSemanticUnit& Unit, const FSemanticUnit& Before)
		{
			if (Unit.SharesStorageWith(Before)) return true;
			return Unit.GetUnitId().Equals(Before.GetUnitId(), ESearchCase::CaseSensitive)
				&& Unit.GetUnitKind() == Before.GetUnitKind()
				&& Unit.GetSemanticTypeRef().Equals(Before.GetSemanticTypeRef(), ESearchCase::CaseSensitive)
				&& Unit.GetTargetIdentityKey().Equals(Before.GetTargetIdentityKey(), ESearchCase::CaseSensitive)
				&& SemanticAliasScope(Unit).Equals(SemanticAliasScope(Before), ESearchCase::CaseSensitive)
				&& SemanticAliasBase(Unit).Equals(SemanticAliasBase(Before), ESearchCase::CaseSensitive);
		}

		bool BindingScopeMatches(const FAuthoringModel& Model, const FSemanticProjectionBindings* Bindings)
		{
			if (!Bindings) return false;
			const auto& Before = Bindings->GetModel();
			return Model.GetContractId().Equals(Before.GetContractId(), ESearchCase::CaseSensitive)
				&& Model.GetContractVersion() == Before.GetContractVersion()
				&& Model.GetProfileId().Equals(Before.GetProfileId(), ESearchCase::CaseSensitive)
				&& Model.GetTargetUnitIds() == Before.GetTargetUnitIds();
		}

		bool TryBuildReadableAliases(
			const FAuthoringModel& Model,
			TMap<FString, FString>& OutAliases,
			FWorkspaceError& OutError,
			TMap<FString, TSharedPtr<const TSet<FString>>>* OutScopeNames = nullptr)
		{
			struct FAliasInput
			{
				const FSemanticUnit* Unit = nullptr;
				FString Scope;
				FString Base;
			};
			TArray<FAliasInput> Inputs;
			for (const FSemanticUnit& Unit : Model.GetUnits())
			{
				const FString Scope = SemanticAliasScope(Unit);
				// Accepted aliases already proved their naming inputs. Fresh names
				// are identity-qualified and cannot claim a retained bare base.
				const FString Base = OutAliases.Contains(Unit.GetUnitId()) ? FString() : SemanticAliasBase(Unit);
				Inputs.Add({&Unit, Scope, Base});
			}
			TMap<FString, TSet<FString>> UsedByScope;
			for (const FAliasInput& Input : Inputs)
				if (const FString* Bound = OutAliases.Find(Input.Unit->GetUnitId())) {
					auto& Used = UsedByScope.FindOrAdd(Input.Scope);
					if (Used.Contains(Bound->ToLower()))
						return Fail(OutError, TEXT("semantic_alias_collision"), TEXT("Two accepted semantic identities have the same alias in one scope."), Input.Unit->GetUnitId());
					Used.Add(Bound->ToLower());
				}
			for (const FAliasInput& Input : Inputs)
			{
				if (OutAliases.Contains(Input.Unit->GetUnitId())) continue;
				const auto* Preferred = Model.GetPreferredDefinitionBindings().Find(Input.Unit->GetUnitId());
				if (!Preferred || !IsStableOrLegacyProjectionName(Preferred->ReadableAlias, Input.Base, Input.Unit->GetUnitId())) continue;
				const FString Folded = Preferred->ReadableAlias.ToLower();
				auto& Used = UsedByScope.FindOrAdd(Input.Scope);
				if (Used.Contains(Folded)) continue;
				Used.Add(Folded);
				OutAliases.Add(Input.Unit->GetUnitId(), Preferred->ReadableAlias);
			}
			for (const FAliasInput& Input : Inputs)
			{
				if (OutAliases.Contains(Input.Unit->GetUnitId())) continue;
				TSet<FString>& Used = UsedByScope.FindOrAdd(Input.Scope);
				FString Alias = StableProjectionName(Input.Base, Input.Unit->GetUnitId());
				if (Alias.IsEmpty() || Used.Contains(Alias.ToLower()))
				{
					return Fail(
						OutError,
						TEXT("semantic_alias_collision"), TEXT("A stable identity name conflicts with another bound identity; existing names were not reassigned."),
						Input.Unit->GetUnitId());
				}
				Used.Add(Alias.ToLower());
				OutAliases.Add(Input.Unit->GetUnitId(), MoveTemp(Alias));
			}
			if (OutScopeNames) for (auto& Scope : UsedByScope)
				OutScopeNames->Add(Scope.Key, MakeShared<const TSet<FString>>(MoveTemp(Scope.Value)));
			return true;
		}

		template<typename TUnit, typename AliasMap> FString OwnerBase(
			const TUnit& Unit,
			const FAuthoringModel& Model,
			const AliasMap& Aliases,
			TMap<FString, FString>& OwnerBases,
			int32& ReusedOwnerBases,
			FWorkspaceError& OutError)
		{
			if (Unit.GetOwnerRef().IsEmpty())
			{
				return TEXT("units/")
					+ Aliases.FindChecked(Unit.GetUnitId());
			}
			// The owner chain and this alias map are immutable in this layout.
			// Logical and physical alias maps keep separate request-local facts.
			if (const FString* Existing = OwnerBases.Find(Unit.GetOwnerRef())) {
				++ReusedOwnerBases;
				return *Existing;
			}
			const auto Owner = Model.FindDeclaration(Unit.GetOwnerRef());
			check(Owner.IsSet());
			const FString OwnerHandle =
				Aliases.FindChecked(Owner->GetUnitId());
			auto Anchor = Owner;
			TArray<FSemanticUnit::FDeclarationView> OwnerChain;
			TSet<FString> Seen;
			while (Anchor->GetUnitKind()
				!= ESemanticUnitKind::TargetState
				&& Anchor->GetUnitKind()
					!= ESemanticUnitKind::EntityState
				&& Anchor->GetUnitKind()
					!= ESemanticUnitKind::StructureState)
			{
				if (Seen.Contains(Anchor->GetUnitId()))
				{
					Fail(
						OutError,
						TEXT("semantic_owner_cycle"), TEXT("The semantic unit owner references contain a dependency cycle."),
						Unit.GetUnitId());
					return FString();
				}
				Seen.Add(Anchor->GetUnitId());
				OwnerChain.Add(Anchor.GetValue());
				if (Anchor->GetOwnerRef().IsEmpty())
				{
					const FString Base = TEXT("items/") + OwnerHandle;
					OwnerBases.Add(Unit.GetOwnerRef(), Base);
					return Base;
				}
				Anchor = Model.FindDeclaration(Anchor->GetOwnerRef());
				check(Anchor.IsSet());
			}

			const FString AnchorHandle =
				Aliases.FindChecked(Anchor->GetUnitId());
			FString Base;
			if (Anchor->GetUnitKind()
				== ESemanticUnitKind::TargetState)
			{
				Base = TEXT("targets/") + AnchorHandle;
			}
			else if (Anchor->GetUnitKind()
				== ESemanticUnitKind::EntityState)
			{
				Base = TEXT("entities/") + AnchorHandle;
			}
			else
			{
				const bool bGraphAnchor = IsGraphAuthoringStructureType(Anchor->GetSemanticTypeRef());
				Base = bGraphAnchor
					? TEXT("graphs/") + AnchorHandle
					: TEXT("structures/") + AnchorHandle;
			}
			if (Owner->GetUnitId() != Anchor->GetUnitId())
			{
				for (int32 Index = OwnerChain.Num() - 1; Index >= 0; --Index)
				{
					Base += TEXT("/items/")
						+ Aliases.FindChecked(OwnerChain[Index].GetUnitId());
				}
			}
			OwnerBases.Add(Unit.GetOwnerRef(), Base);
			return Base;
		}

		template <typename TUnit> bool HasGraphStructureAncestor(
			const TUnit& Unit,
			const FAuthoringModel& Model)
		{
			FString CurrentId = Unit.GetUnitId(), OwnerRef = Unit.GetOwnerRef();
			TSet<FString> Seen;
			while (!OwnerRef.IsEmpty())
			{
				if (Seen.Contains(CurrentId))
				{
					return false;
				}
				Seen.Add(CurrentId);
				const auto Current = Model.FindDeclaration(OwnerRef);
				if (!Current)
				{
					return false;
				}
				if (Current->GetUnitKind() == ESemanticUnitKind::StructureState
					&& IsGraphAuthoringStructureType(Current->GetSemanticTypeRef()))
				{
					return true;
				}
				CurrentId = Current->GetUnitId(); OwnerRef = Current->GetOwnerRef();
			}
			return false;
		}

		template <typename TUnit> bool IsGraphStructureUnit(const TUnit& Unit)
		{
			return Unit.GetUnitKind() == ESemanticUnitKind::StructureState
				&& IsGraphAuthoringStructureType(Unit.GetSemanticTypeRef());
		}

		// Request-local path lookup over the actual accepted naming owner. Only
		// a name really read by a path consumer is expanded from its public ref.
		class FLayoutNameLookup
		{
			const TMap<FString, FString>& Values;
			const FWorkspacePlacementView* AcceptedRefs;
			const TMap<FString, FString>* Exceptions;
			const FLayoutNameLookup* Fallback;
			mutable TMap<FString, FString> Expanded;
		public:
			FLayoutNameLookup(const TMap<FString, FString>& InValues,
				const FWorkspacePlacementView* InRefs = nullptr, const TMap<FString, FString>* InExceptions = nullptr,
				const FLayoutNameLookup* InFallback = nullptr)
				: Values(InValues), AcceptedRefs(InRefs), Exceptions(InExceptions), Fallback(InFallback) {}
			const FString* Find(const FString& Id) const {
				if (const auto* Value = Values.Find(Id)) return Value;
				if (Fallback) return Fallback->Find(Id);
				if (Exceptions) if (const auto* Value = Exceptions->Find(Id)) return Value;
				if (const auto* Value = Expanded.Find(Id)) return Value;
				const auto* Ref = AcceptedRefs ? AcceptedRefs->Find(Id) : nullptr;
				if (!Ref) return nullptr;
				int32 Colon = INDEX_NONE;
				if (!Ref->FindChar(TEXT(':'), Colon)) return nullptr;
				return &Expanded.Add(Id, Ref->Mid(Colon + 1));
			}
			const FString& FindChecked(const FString& Id) const { const auto* Value = Find(Id); check(Value); return *Value; }
			int32 NumExpanded() const { return Expanded.Num(); }
		};

		template<typename AliasMap> TMap<FString, FString> BuildGraphPhysicalPaths(
			const FAuthoringModel& Model,
			const AliasMap& Aliases)
		{
			TMap<FString, FString> Result;
			for (const auto& Unit : Model.GetDeclarations())
			{
				if (IsGraphStructureUnit(Unit))
				{
					Result.Add(
						Unit.GetUnitId(),
						TEXT("graphs/")
							+ Aliases.FindChecked(Unit.GetUnitId())
							+ TEXT(".hjson"));
				}
			}
			return Result;
		}

		template<typename TUnit, typename AliasMap> FString LogicalPath(
			const TUnit& Unit,
			const FAuthoringModel& Model,
			const AliasMap& Aliases,
			TMap<FString, FString>& OwnerBases,
			int32& ReusedOwnerBases,
			FWorkspaceError& OutError)
		{
			const FString Handle =
				Aliases.FindChecked(Unit.GetUnitId());
			switch (Unit.GetUnitKind())
			{
			case ESemanticUnitKind::WorkspaceIndex:
				return TEXT("index.hjson");
			case ESemanticUnitKind::TargetState:
				return TEXT("targets/") + Handle
					+ TEXT("/state.hjson");
			case ESemanticUnitKind::EntityState:
				return TEXT("entities/") + Handle
					+ TEXT("/state.hjson");
			case ESemanticUnitKind::StructureState:
				return IsGraphAuthoringStructureType(Unit.GetSemanticTypeRef())
						? TEXT("graphs/") + Handle + TEXT(".hjson")
						: TEXT("structures/") + Handle + TEXT("/structure.hjson");
			case ESemanticUnitKind::ItemState:
				return OwnerBase(Unit, Model, Aliases, OwnerBases, ReusedOwnerBases, OutError)
					+ TEXT("/items/")
					+ Handle
					+ TEXT("/state.hjson");
			case ESemanticUnitKind::PropertySet:
				if (Unit.GetRole().StartsWith(TEXT("preview/")))
					return Unit.GetRole() + TEXT("/") + SafeSegment(Unit.GetFieldSetRef()) + TEXT(".hjson");
				if (Unit.GetRole() == TEXT("authored_inputs"))
				{
					return OwnerBase(Unit, Model, Aliases, OwnerBases, ReusedOwnerBases, OutError)
						+ TEXT("/inputs.hjson");
				}
				return OwnerBase(Unit, Model, Aliases, OwnerBases, ReusedOwnerBases, OutError)
					+ (Unit.GetSemanticTypeRef() == TEXT("uai.semantic.umg.slot.property")
						? TEXT("/slot/properties/")
						: TEXT("/properties/"))
					+ SafeSegment(
						Unit.GetFieldSetRef().IsEmpty()
							? Unit.GetRole()
							: Unit.GetFieldSetRef())
					+ TEXT(".hjson");
			case ESemanticUnitKind::RelationSet:
			{
				int32 SiblingRelationCount = 0;
			for (const auto& Candidate : Model.GetDeclarations())
				{
					SiblingRelationCount += Candidate.GetUnitKind()
						== ESemanticUnitKind::RelationSet
						&& Candidate.GetOwnerRef() == Unit.GetOwnerRef();
				}
				const FString Base = OwnerBase(Unit, Model, Aliases, OwnerBases, ReusedOwnerBases, OutError);
				return SiblingRelationCount > 1
					? Base + TEXT("/relations/") + Handle + TEXT(".hjson")
					: Base + TEXT("/relations.hjson");
			}
			case ESemanticUnitKind::OrderingState:
				return OwnerBase(Unit, Model, Aliases, OwnerBases, ReusedOwnerBases, OutError)
					+ TEXT("/ordering.hjson");
			case ESemanticUnitKind::LayoutState:
				return OwnerBase(Unit, Model, Aliases, OwnerBases, ReusedOwnerBases, OutError)
					+ TEXT("/layout.hjson");
			case ESemanticUnitKind::CollectionState:
				if (Unit.GetRole() == TEXT("property_definition"))
				{
					return OwnerBase(Unit, Model, Aliases, OwnerBases, ReusedOwnerBases, OutError)
						+ TEXT("/collections/") + Handle + TEXT(".hjson");
				}
				return OwnerBase(Unit, Model, Aliases, OwnerBases, ReusedOwnerBases, OutError)
					+ TEXT("/collections/")
					+ Handle
					+ TEXT(".hjson");
			case ESemanticUnitKind::DependencyState:
				return OwnerBase(Unit, Model, Aliases, OwnerBases, ReusedOwnerBases, OutError)
					+ TEXT("/dependencies/")
					+ Handle
					+ TEXT(".hjson");
			case ESemanticUnitKind::ExternalPayloadState:
				return TEXT("external/") + Handle
					+ TEXT("/state.hjson");
			case ESemanticUnitKind::OpaqueExtension:
			default:
				return TEXT("extensions/") + Handle
					+ TEXT("/state.hjson");
			}
		}
	}

	FString GetStableProjectionIdentitySuffix(const FString& UnitId)
	{
		return TEXT("--") + HashText(UnitId).Right(24);
	}

	static bool ValidateSemanticValueNames(const FUnitValues& Values, const FString& UnitId,
		TSharedPtr<Contract::FDiagnostic>& Diagnostic)
	{
		// Sorted names have no successful consumer here. Inspect the original
		// map once, selecting the same first invalid spelling only on failure.
		const FString* InvalidName = nullptr;
		for (const auto& Value : Values)
			if (FStringView(Value.Key).TrimStartAndEnd().IsEmpty()
				&& (!InvalidName || Value.Key < *InvalidName)) InvalidName = &Value.Key;
		if (InvalidName) {
			Diagnostic = MakeShared<Contract::FDiagnostic>(Contract::FDiagnostic::Failure(TEXT("invalid_semantic_unit"),
				TEXT("A semantic value field has an empty name after trimming whitespace."), TEXT("semantic_model")));
			Diagnostic->SemanticPointer = TEXT("/values"); Diagnostic->Source.SourceUnit = UnitId;
			Diagnostic->ActualValue = MakeShared<FJsonValueString>(*InvalidName);
			return false;
		}
		return true;
	}

	struct FSemanticUnit::FDeclarationRecord
	{
			FDeclarationRecord(FUnitStringRef InUnitId, ESemanticUnitKind InUnitKind, FString InOwnerRef,
				FString InSemanticTypeRef, FString InRole, FString InFieldSetRef,
				FString InOrderingRef, FString InLayoutRef, FString InSourceProvenance,
				FString InTargetIdentityKey, TArray<FString> InCategoryPath)
				: UnitId(MoveTemp(InUnitId)), UnitKind(InUnitKind),
				  OwnerRef(ShareUnitString(Normalize(MoveTemp(InOwnerRef)))),
				  SemanticTypeRef(ShareUnitString(Normalize(MoveTemp(InSemanticTypeRef)))),
				  Role(ShareUnitString(Normalize(MoveTemp(InRole)))),
				  FieldSetRef(ShareUnitString(Normalize(MoveTemp(InFieldSetRef)))),
				  OrderingRef(Normalize(MoveTemp(InOrderingRef))), LayoutRef(Normalize(MoveTemp(InLayoutRef))),
				  SourceProvenance(ShareUnitString(Normalize(MoveTemp(InSourceProvenance)))),
				  TargetIdentityKey(Normalize(MoveTemp(InTargetIdentityKey))), CategoryPath(MoveTemp(InCategoryPath)) {}
			FUnitStringRef UnitId;
			ESemanticUnitKind UnitKind = ESemanticUnitKind::OpaqueExtension;
			FUnitStringRef OwnerRef, SemanticTypeRef, Role, FieldSetRef;
			TSharedPtr<const FUnitStrings> ItemRefs, RelationRefs;
			FString OrderingRef, LayoutRef;
			TSharedPtr<const FUnitStrings> DependencyRefs, ProfileVisibility;
			FUnitStringRef SourceProvenance;
			FString TargetIdentityKey;
			TArray<FString> CategoryPath;
	};
	bool HaveSameSemanticProjectionIdentity(const FSemanticUnit& Unit, const FSemanticUnit& Before)
	{
		return SameProjectionBindingIdentity(Unit, Before);
	}

	const FString& FSemanticUnit::FSinglePropertyValue::GetFieldName(EField Field)
	{
		static const FString Value(TEXT("value")), Literal(TEXT("literal")), Selected(TEXT("selected"));
		return Field == FSemanticUnit::FSinglePropertyValue::EField::Literal ? Literal
			: Field == FSemanticUnit::FSinglePropertyValue::EField::Selected ? Selected : Value;
	}
	struct FSemanticUnit::FStorage
	{
		using FDeclaration = FDeclarationRecord;
		FStorage(FUnitStringRef InUnitId, ESemanticUnitKind InUnitKind, FString InOwnerRef,
			FString InSemanticTypeRef, FString InRole, FString InFieldSetRef,
			TMap<FString, FUeAgentCanonicalValueRef> InValues, TArray<FString> InItemRefs,
			TArray<FString> InRelationRefs, FString InOrderingRef, FString InLayoutRef,
			TArray<FString> InDependencyRefs, TArray<FString> InProfileVisibility,
			FString InSourceProvenance, FString InTargetIdentityKey, TArray<FString> InCategoryPath,
			const FSinglePropertyValue* InSingleValue = nullptr);
		FStorage(const FStorage& Previous, FUnitValues InValues);
		FStorage(TSharedRef<const FDeclarationRecord, ESPMode::ThreadSafe> InDeclaration, FUnitValues InValues);
		~FStorage() { delete ExpandedInlineValues; }
		// Written only by initial construction. A valid WithValues derivative
		// shares this declaration directly, not its parent's values or storage.
		TSharedPtr<const FDeclaration, ESPMode::ThreadSafe> Declaration;
		TSharedPtr<const FUnitValues> Values;
		TOptional<FUeAgentCanonicalValueRef> InlinePropertyValue;
		FSinglePropertyValue::EField InlinePropertyField = FSinglePropertyValue::EField::Value;
		// Only an explicit concrete-map reader asks for this representation.
		// It belongs to this storage, never another generation or a cache owner.
		mutable FUnitValues* ExpandedInlineValues = nullptr;
		mutable std::atomic<uint8> ExpandedInlineState{0};
		// Valid units do not each own the full (normally empty) diagnostic tree.
		TSharedPtr<Contract::FDiagnostic> ValidationDiagnostic;
		// A digest of this exact immutable unit's original UTF-8 material. The
		// compiler already produces that material for the complete model SHA.
		// WithValues creates fresh storage unless every value is identical.
		mutable uint8 ContentDigest[SHA256_DIGEST_LENGTH] = {};
		mutable std::atomic<uint8> ContentDigestState{0};
		// The same row-hash result formerly owned by the complete manifest
		// array. Fresh WithValues storage starts empty; declarations alone never
		// prove a value row. No layout/profile/alias context is represented here.
		mutable FString PublicRowHash;
		mutable std::atomic<uint8> PublicRowHashState{0};
	};

	struct FSemanticUnitContentAccess
	{
		static const FString& ReadRow(const FSemanticUnit& Unit)
		{
			const auto& Storage = Unit.Storage.Get();
			uint8 State = Storage.PublicRowHashState.load(std::memory_order_acquire);
			while (State == 1) {
				FPlatformProcess::YieldThread();
				State = Storage.PublicRowHashState.load(std::memory_order_acquire);
			}
			static const FString Empty;
			return State == 2 ? Storage.PublicRowHash : Empty;
		}
		static void SealRow(const FSemanticUnit& Unit, FString Hash)
		{
			const auto& Storage = Unit.Storage.Get();
			uint8 Empty = 0;
			if (!Storage.PublicRowHashState.compare_exchange_strong(Empty, 1, std::memory_order_acquire)) return;
			Storage.PublicRowHash = MoveTemp(Hash);
			Storage.PublicRowHashState.store(2, std::memory_order_release);
		}
		static bool Read(const FSemanticUnit& Unit, FString& Out)
		{
			const auto& Storage = Unit.Storage.Get();
			if (Storage.ContentDigestState.load(std::memory_order_acquire) != 2) return false;
			static const TCHAR Hex[] = TEXT("0123456789abcdef");
			Out = TEXT("sha256:"); Out.Reserve(7 + SHA256_DIGEST_LENGTH * 2);
			for (const uint8 Byte : Storage.ContentDigest) { Out.AppendChar(Hex[Byte >> 4]); Out.AppendChar(Hex[Byte & 15]); }
			return true;
		}
		static bool Seal(const FSemanticUnit& Unit, const uint8* Bytes, int32 Count, double* OutSeconds = nullptr)
		{
			const auto& Storage = Unit.Storage.Get();
			if (Storage.ContentDigestState.load(std::memory_order_acquire) != 0) return false;
			uint8 Empty = 0;
			if (!Storage.ContentDigestState.compare_exchange_strong(Empty, 1, std::memory_order_acquire)) return false;
			const double Started = OutSeconds ? FPlatformTime::Seconds() : 0;
			SHA256(Bytes, Count, Storage.ContentDigest);
			Storage.ContentDigestState.store(2, std::memory_order_release);
			if (OutSeconds) *OutSeconds += FPlatformTime::Seconds() - Started;
			return true;
		}
	};

	FSemanticUnit::FStorage::FStorage(
		FUnitStringRef InUnitId,
		ESemanticUnitKind InUnitKind,
		FString InOwnerRef,
		FString InSemanticTypeRef,
		FString InRole,
		FString InFieldSetRef,
		TMap<FString, FUeAgentCanonicalValueRef> InValues,
		TArray<FString> InItemRefs,
		TArray<FString> InRelationRefs,
		FString InOrderingRef,
		FString InLayoutRef,
		TArray<FString> InDependencyRefs,
		TArray<FString> InProfileVisibility,
		FString InSourceProvenance,
		FString InTargetIdentityKey,
		TArray<FString> InCategoryPath, const FSinglePropertyValue* InSingleValue)
	{
		auto MutableDeclaration = MakeShared<FDeclaration, ESPMode::ThreadSafe>(MoveTemp(InUnitId), InUnitKind, MoveTemp(InOwnerRef),
			MoveTemp(InSemanticTypeRef), MoveTemp(InRole), MoveTemp(InFieldSetRef), MoveTemp(InOrderingRef),
			MoveTemp(InLayoutRef), MoveTemp(InSourceProvenance), MoveTemp(InTargetIdentityKey), MoveTemp(InCategoryPath));
		const FString& UnitId = MutableDeclaration->UnitId.Get();
		auto& SemanticTypeRef = MutableDeclaration->SemanticTypeRef;
		auto& Role = MutableDeclaration->Role;
		auto& CategoryPath = MutableDeclaration->CategoryPath;
		if (InSingleValue) { InlinePropertyValue.Emplace(InSingleValue->Value); InlinePropertyField = InSingleValue->Field; }
		// Validate exactly as before, then seal only the actual normalized data.
		// Invalid units keep their own partial input and original diagnostic.
		ON_SCOPE_EXIT {
			if (!InlinePropertyValue) Values = ValidationDiagnostic ? MakeShared<const FUnitValues>(MoveTemp(InValues)) : ShareUnitValues(MoveTemp(InValues));
			MutableDeclaration->ItemRefs = ValidationDiagnostic ? MakeShared<const FUnitStrings>(MoveTemp(InItemRefs)) : ShareUnitStrings(MoveTemp(InItemRefs));
			MutableDeclaration->RelationRefs = ValidationDiagnostic ? MakeShared<const FUnitStrings>(MoveTemp(InRelationRefs)) : ShareUnitStrings(MoveTemp(InRelationRefs));
			MutableDeclaration->DependencyRefs = ValidationDiagnostic ? MakeShared<const FUnitStrings>(MoveTemp(InDependencyRefs)) : ShareUnitStrings(MoveTemp(InDependencyRefs));
			MutableDeclaration->ProfileVisibility = ValidationDiagnostic ? MakeShared<const FUnitStrings>(MoveTemp(InProfileVisibility)) : ShareUnitStrings(MoveTemp(InProfileVisibility));
			Declaration = MoveTemp(MutableDeclaration);
		};
		for (FString& Segment : CategoryPath)
		{
			Segment = Normalize(MoveTemp(Segment));
		}
		CategoryPath.RemoveAll([](const FString& Segment)
		{
			return Segment.IsEmpty();
		});
		auto InvalidField = [&](const TCHAR* Field, const TCHAR* Reason) {
			ValidationDiagnostic = MakeShared<Contract::FDiagnostic>(Contract::FDiagnostic::Failure(TEXT("invalid_semantic_unit"), Reason, TEXT("semantic_model")));
			ValidationDiagnostic->SemanticPointer = TEXT("/") + FString(Field);
			ValidationDiagnostic->Source.SourceUnit = UnitId;
		};
		if (UnitId.IsEmpty()) { InvalidField(TEXT("unit_id"), TEXT("The semantic unit requires a nonempty unit identity.")); return; }
		if (SemanticTypeRef->IsEmpty()) { InvalidField(TEXT("semantic_type_ref"), TEXT("The semantic unit requires a semantic type reference.")); return; }
		if (Role->IsEmpty()) { InvalidField(TEXT("role"), TEXT("The semantic unit requires a nonempty role.")); return; }
		if (!ValidateSemanticValueNames(InValues, UnitId, ValidationDiagnostic)) return;
		Contract::FDiagnostic SetDiagnostic;
		if (!NormalizeSet(InItemRefs, SetDiagnostic, TEXT("invalid_semantic_unit"), TEXT("item_refs"))
			|| !NormalizeSet(InRelationRefs, SetDiagnostic, TEXT("invalid_semantic_unit"), TEXT("relation_refs"))
			|| !NormalizeSet(InDependencyRefs, SetDiagnostic, TEXT("invalid_semantic_unit"), TEXT("dependency_refs"))
			|| !NormalizeSet(InProfileVisibility, SetDiagnostic, TEXT("invalid_semantic_unit"), TEXT("profile_visibility"), true)) {
			SetDiagnostic.Source.SourceUnit = UnitId;
			ValidationDiagnostic = MakeShared<Contract::FDiagnostic>(MoveTemp(SetDiagnostic));
		}

	}

	FSemanticUnit::FSemanticUnit(FString InUnitId, ESemanticUnitKind InUnitKind, FString InOwnerRef,
		FString InSemanticTypeRef, FString InRole, FString InFieldSetRef,
		TMap<FString, FUeAgentCanonicalValueRef> InValues, TArray<FString> InItemRefs,
		TArray<FString> InRelationRefs, FString InOrderingRef, FString InLayoutRef,
		TArray<FString> InDependencyRefs, TArray<FString> InProfileVisibility,
		FString InSourceProvenance, FString InTargetIdentityKey, TArray<FString> InCategoryPath)
		: FSemanticUnit(MakeShared<const FString, ESPMode::ThreadSafe>(Normalize(MoveTemp(InUnitId))), InUnitKind, MoveTemp(InOwnerRef),
			MoveTemp(InSemanticTypeRef), MoveTemp(InRole), MoveTemp(InFieldSetRef), MoveTemp(InValues),
			MoveTemp(InItemRefs), MoveTemp(InRelationRefs), MoveTemp(InOrderingRef), MoveTemp(InLayoutRef),
			MoveTemp(InDependencyRefs), MoveTemp(InProfileVisibility), MoveTemp(InSourceProvenance),
			MoveTemp(InTargetIdentityKey), MoveTemp(InCategoryPath))
	{
	}

	static FUnitStringRef NormalizeUnitIdentity(FUnitStringRef Identity)
	{
		if (!Identity->IsEmpty() && (FChar::IsWhitespace((*Identity)[0]) || FChar::IsWhitespace((*Identity)[Identity->Len() - 1])))
			return MakeShared<const FString, ESPMode::ThreadSafe>(Normalize(Identity.Get()));
		return Identity;
	}

	FSemanticUnit::FSemanticUnit(FUnitStringRef InUnitId, ESemanticUnitKind InUnitKind, FString InOwnerRef,
		FString InSemanticTypeRef, FString InRole, FString InFieldSetRef,
		TMap<FString, FUeAgentCanonicalValueRef> InValues, TArray<FString> InItemRefs,
		TArray<FString> InRelationRefs, FString InOrderingRef, FString InLayoutRef,
		TArray<FString> InDependencyRefs, TArray<FString> InProfileVisibility,
		FString InSourceProvenance, FString InTargetIdentityKey, TArray<FString> InCategoryPath)
		: Storage(MakeShared<FStorage, ESPMode::ThreadSafe>(NormalizeUnitIdentity(MoveTemp(InUnitId)), InUnitKind, MoveTemp(InOwnerRef),
			MoveTemp(InSemanticTypeRef), MoveTemp(InRole), MoveTemp(InFieldSetRef), MoveTemp(InValues),
			MoveTemp(InItemRefs), MoveTemp(InRelationRefs), MoveTemp(InOrderingRef), MoveTemp(InLayoutRef),
			MoveTemp(InDependencyRefs), MoveTemp(InProfileVisibility), MoveTemp(InSourceProvenance),
			MoveTemp(InTargetIdentityKey), MoveTemp(InCategoryPath)))
	{
	}

	FSemanticUnit::FSemanticUnit(FString InUnitId, ESemanticUnitKind InUnitKind, FString InOwnerRef,
		FString InSemanticTypeRef, FString InRole, FString InFieldSetRef, FSinglePropertyValue InValue,
		TArray<FString> InItemRefs, TArray<FString> InRelationRefs, FString InOrderingRef, FString InLayoutRef,
		TArray<FString> InDependencyRefs, TArray<FString> InProfileVisibility, FString InSourceProvenance,
		FString InTargetIdentityKey, TArray<FString> InCategoryPath)
		: Storage(MakeShared<FStorage, ESPMode::ThreadSafe>(
			MakeShared<const FString, ESPMode::ThreadSafe>(Normalize(MoveTemp(InUnitId))), InUnitKind,
			MoveTemp(InOwnerRef), MoveTemp(InSemanticTypeRef), MoveTemp(InRole), MoveTemp(InFieldSetRef), FUnitValues(),
			MoveTemp(InItemRefs), MoveTemp(InRelationRefs), MoveTemp(InOrderingRef), MoveTemp(InLayoutRef),
			MoveTemp(InDependencyRefs), MoveTemp(InProfileVisibility), MoveTemp(InSourceProvenance),
			MoveTemp(InTargetIdentityKey), MoveTemp(InCategoryPath), &InValue))
	{}

	FSemanticUnit::FStorage::FStorage(const FStorage& Previous, FUnitValues InValues)
		: FStorage(Previous.Declaration.ToSharedRef(), MoveTemp(InValues))
	{}

	FSemanticUnit::FStorage::FStorage(TSharedRef<const FDeclarationRecord, ESPMode::ThreadSafe> InDeclaration, FUnitValues InValues)
		: Declaration(MoveTemp(InDeclaration))
	{
		ValidateSemanticValueNames(InValues, Declaration->UnitId.Get(), ValidationDiagnostic);
		Values = ValidationDiagnostic ? MakeShared<const FUnitValues>(MoveTemp(InValues)) : ShareUnitValues(MoveTemp(InValues));
	}

	FSemanticUnit::FDeclarationView FSemanticUnit::GetDeclaration() const
	{
		// This producer seals exactly one value/literal/selected field. None of the
		// graph/keyed/type/usage shape probes can be present in this storage.
		if (Storage->InlinePropertyValue)
			return FDeclarationView(Storage->Declaration.ToSharedRef(), IsValid(), 1, false, 0, false, false, false);
		uint64 Roots = 0;
		if (GetUnitKind() == ESemanticUnitKind::StructureState) {
			int32 Index = 0;
			for (const auto& Contract : GetKeyedItemProjectionContracts()) {
				if (Index < 64 && FindValue(Contract.RootField)) Roots |= uint64(1) << Index;
				++Index;
			}
		}
		const auto* Type = FindValue(TEXT("type"));
		bool bDirectAssetPathField = false;
		VisitValues([&](const FString& Key, const auto&) {
			bDirectAssetPathField = IsCompositionDependencyPathField(Key);
			return !bDirectAssetPathField;
		});
		return FDeclarationView(Storage->Declaration.ToSharedRef(), IsValid(), GetValueCount(),
			FindValue(TEXT("nodes")) && FindValue(TEXT("edges")), Roots,
			Type && (*Type)->GetKind() == EUeAgentValueKind::Object,
			FindValue(TEXT("emitter_name")) || FindValue(TEXT("usage")), bDirectAssetPathField);
	}
	FSemanticUnit::FDeclarationView FSemanticUnit::GetDeclarationForCapturedShape(const TArray<FString>& ValueKeys, bool bOwnTypeDescriptor) const
	{
		uint64 Roots = 0;
		int32 Index = 0;
		for (const auto& Contract : GetKeyedItemProjectionContracts()) {
			if (Index < 64 && ValueKeys.Contains(Contract.RootField)) Roots |= uint64(1) << Index;
			++Index;
		}
		return FDeclarationView(Storage->Declaration.ToSharedRef(), IsValid(), ValueKeys.Num(),
			ValueKeys.Contains(TEXT("nodes")) && ValueKeys.Contains(TEXT("edges")), Roots, bOwnTypeDescriptor,
			ValueKeys.Contains(TEXT("emitter_name")) || ValueKeys.Contains(TEXT("usage")), ValueKeys.ContainsByPredicate(IsCompositionDependencyPathField));
	}
	bool FSemanticUnit::FDeclarationView::HasKeyedRoot(const FString& Root) const
	{
		int32 Index = 0;
		for (const auto& Contract : GetKeyedItemProjectionContracts()) {
			if (Index < 64 && Contract.RootField == Root) return (KeyedRoots & (uint64(1) << Index)) != 0;
			++Index;
		}
		return false;
	}
	FSemanticUnit FSemanticUnit::FromDeclaration(FDeclarationView Declaration, FUnitValues Values)
	{
		if (!Declaration.bValidated) {
			// An invalid original unit cannot bless its declaration. Reuse the
			// original constructor/error precedence when its values are replaced.
			return FSemanticUnit(Declaration.GetSharedUnitId(), Declaration.GetUnitKind(), Declaration.GetOwnerRef(),
				Declaration.GetSemanticTypeRef(), Declaration.GetRole(), Declaration.GetFieldSetRef(), MoveTemp(Values),
				Declaration.GetItemRefs(), Declaration.GetRelationRefs(), Declaration.GetOrderingRef(), Declaration.GetLayoutRef(),
				Declaration.GetDependencyRefs(), Declaration.GetProfileVisibility(), Declaration.GetSourceProvenance(),
				Declaration.GetTargetIdentityKey(), Declaration.GetCategoryPath());
		}
		return FSemanticUnit(MakeShared<FStorage, ESPMode::ThreadSafe>(MoveTemp(Declaration.Declaration), MoveTemp(Values)));
	}
	const FString& FSemanticUnit::FDeclarationView::GetUnitId() const { return Declaration->UnitId.Get(); }
	const FUnitStringRef& FSemanticUnit::FDeclarationView::GetSharedUnitId() const { return Declaration->UnitId; }
	ESemanticUnitKind FSemanticUnit::FDeclarationView::GetUnitKind() const { return Declaration->UnitKind; }
	const FString& FSemanticUnit::FDeclarationView::GetOwnerRef() const { return Declaration->OwnerRef.Get(); }
	const FString& FSemanticUnit::FDeclarationView::GetSemanticTypeRef() const { return Declaration->SemanticTypeRef.Get(); }
	const FString& FSemanticUnit::FDeclarationView::GetRole() const { return Declaration->Role.Get(); }
	const FString& FSemanticUnit::FDeclarationView::GetFieldSetRef() const { return Declaration->FieldSetRef.Get(); }
	const TArray<FString>& FSemanticUnit::FDeclarationView::GetItemRefs() const { return *Declaration->ItemRefs; }
	const TArray<FString>& FSemanticUnit::FDeclarationView::GetRelationRefs() const { return *Declaration->RelationRefs; }
	const FString& FSemanticUnit::FDeclarationView::GetOrderingRef() const { return Declaration->OrderingRef; }
	const FString& FSemanticUnit::FDeclarationView::GetLayoutRef() const { return Declaration->LayoutRef; }
	const TArray<FString>& FSemanticUnit::FDeclarationView::GetDependencyRefs() const { return *Declaration->DependencyRefs; }
	const TArray<FString>& FSemanticUnit::FDeclarationView::GetProfileVisibility() const { return *Declaration->ProfileVisibility; }
	const FString& FSemanticUnit::FDeclarationView::GetSourceProvenance() const { return Declaration->SourceProvenance.Get(); }
	const FString& FSemanticUnit::FDeclarationView::GetTargetIdentityKey() const { return Declaration->TargetIdentityKey; }
	const TArray<FString>& FSemanticUnit::FDeclarationView::GetCategoryPath() const { return Declaration->CategoryPath; }

	FSemanticUnit FSemanticUnit::WithValues(FUnitValues NewValues) const
	{
		if (!IsValid()) {
			// Preserve the original full-constructor error precedence for an
			// invalid declaration, including partially normalized set inputs.
			return FromDeclaration(GetDeclaration(), MoveTemp(NewValues));
		}
		// Values are already immutable canonical nodes. Preserve the successful
		// storage proof when this transformation returns the exact same ordered
		// fields, rather than making every downstream compiler see a new unit.
		// This is identity reuse, not a weaker numeric/text equality operation.
		if (Storage->InlinePropertyValue) {
			if (NewValues.Num() == 1) {
				const FString& Name = FSinglePropertyValue::GetFieldName(Storage->InlinePropertyField);
				const auto Field = NewValues.CreateConstIterator();
				if (Field.Key().Equals(Name, ESearchCase::CaseSensitive)
					&& &Field.Value().Get() == &Storage->InlinePropertyValue.GetValue().Get()) return *this;
			}
		} else if (NewValues.Num() == Storage->Values->Num()) {
			auto Before = Storage->Values->CreateConstIterator();
			bool bSame = true;
			for (const auto& Pair : NewValues) {
				if (!Before || !Before.Key().Equals(Pair.Key, ESearchCase::CaseSensitive)
					|| &Before.Value().Get() != &Pair.Value.Get()) { bSame = false; break; }
				++Before;
			}
			if (bSame) return *this;
		}
		return FSemanticUnit(MakeShared<FStorage, ESPMode::ThreadSafe>(Storage.Get(), MoveTemp(NewValues)));
	}

	const FString& FSemanticUnit::GetUnitId() const { return Storage->Declaration->UnitId.Get(); }
	const TSharedRef<const FString, ESPMode::ThreadSafe>& FSemanticUnit::GetSharedUnitId() const { return Storage->Declaration->UnitId; }
	const FString& FSemanticUnit::GetPublicRowHash() const { return FSemanticUnitContentAccess::ReadRow(*this); }
	int64 FSemanticUnit::GetStorageAllocatedSize() const
	{
		return sizeof(FStorage) + 32 + GetPublicRowHash().GetAllocatedSize();
	}
	void FSemanticUnit::VisitSharedStorageStrings(TFunctionRef<void(const FString&)> Visit) const
	{
		Visit(Storage->Declaration->OwnerRef.Get()); Visit(Storage->Declaration->SemanticTypeRef.Get()); Visit(Storage->Declaration->Role.Get());
		Visit(Storage->Declaration->SourceProvenance.Get()); Visit(Storage->Declaration->FieldSetRef.Get());
		Visit(Storage->Declaration->UnitId.Get());
	}
	void FSemanticUnit::VisitSharedStorageContainers(TFunctionRef<void(const void*, int64)> Visit) const
	{
		VisitSharedStorageContainers([](const void*) { return true; }, Visit);
	}
	void FSemanticUnit::VisitSharedStorageContainers(TFunctionRef<bool(const void*)> ShouldVisit,
		TFunctionRef<void(const void*, int64)> Visit) const
	{
		const auto& Declaration = *Storage->Declaration;
		if (ShouldVisit(&Declaration)) {
			int64 Bytes = sizeof(FStorage::FDeclaration) + 32 + Declaration.CategoryPath.GetAllocatedSize();
			for (const FString* Value : {&Declaration.OrderingRef, &Declaration.LayoutRef, &Declaration.TargetIdentityKey})
				Bytes += Value->GetAllocatedSize();
			for (const FString& Value : Declaration.CategoryPath) Bytes += Value.GetAllocatedSize();
			Visit(&Declaration, Bytes);
		}
		if (Storage->Values && ShouldVisit(Storage->Values.Get())) {
			int64 Bytes = sizeof(FUnitValues) + 32 + Storage->Values->GetAllocatedSize();
			for (const auto& Pair : *Storage->Values) Bytes += Pair.Key.GetAllocatedSize();
			Visit(Storage->Values.Get(), Bytes);
		}
		if (const auto* Expanded = Storage->ExpandedInlineState.load(std::memory_order_acquire) == 2 ? Storage->ExpandedInlineValues : nullptr;
			Expanded && ShouldVisit(Expanded)) {
			int64 Bytes = sizeof(FUnitValues) + Expanded->GetAllocatedSize();
			for (const auto& Pair : *Expanded) Bytes += Pair.Key.GetAllocatedSize();
			Visit(Expanded, Bytes);
		}
		for (const auto* Values : {&Storage->Declaration->ItemRefs, &Storage->Declaration->RelationRefs, &Storage->Declaration->DependencyRefs, &Storage->Declaration->ProfileVisibility}) {
			if (!ShouldVisit(Values->Get())) continue;
			int64 Bytes = sizeof(FUnitStrings) + 32 + (*Values)->GetAllocatedSize();
			for (const FString& Value : **Values) Bytes += Value.GetAllocatedSize();
			Visit(Values->Get(), Bytes);
		}
	}
	ESemanticUnitKind FSemanticUnit::GetUnitKind() const { return Storage->Declaration->UnitKind; }
	const FString& FSemanticUnit::GetOwnerRef() const { return Storage->Declaration->OwnerRef.Get(); }
	const FString& FSemanticUnit::GetSemanticTypeRef() const
	{
		return Storage->Declaration->SemanticTypeRef.Get();
	}
	const FString& FSemanticUnit::GetRole() const { return Storage->Declaration->Role.Get(); }
	const FString& FSemanticUnit::GetFieldSetRef() const
	{
		return Storage->Declaration->FieldSetRef.Get();
	}
	const TMap<FString, FUeAgentCanonicalValueRef>&
	FSemanticUnit::GetValues() const
	{
		if (Storage->Values) return *Storage->Values;
		uint8 State = Storage->ExpandedInlineState.load(std::memory_order_acquire);
		if (State == 0 && Storage->ExpandedInlineState.compare_exchange_strong(State, 1, std::memory_order_acq_rel)) {
			auto* Map = new FUnitValues;
			Map->Add(FSinglePropertyValue::GetFieldName(Storage->InlinePropertyField), Storage->InlinePropertyValue.GetValue());
			Storage->ExpandedInlineValues = Map;
			Storage->ExpandedInlineState.store(2, std::memory_order_release);
			return *Map;
		}
		while (Storage->ExpandedInlineState.load(std::memory_order_acquire) != 2) FPlatformProcess::YieldThread();
		return *Storage->ExpandedInlineValues;
	}
	const FUeAgentCanonicalValueRef* FSemanticUnit::FindValue(const FString& Key, const FString** MatchedKey) const
	{
		if (MatchedKey) *MatchedKey = nullptr;
		if (Storage->InlinePropertyValue) {
			const FString& Name = FSinglePropertyValue::GetFieldName(Storage->InlinePropertyField);
			if (!Key.Equals(Name, ESearchCase::IgnoreCase)) return nullptr;
			if (MatchedKey) *MatchedKey = &Name;
			return &Storage->InlinePropertyValue.GetValue();
		}
		if (MatchedKey) {
			const auto Id = Storage->Values->FindId(Key); if (!Id.IsValidId()) return nullptr;
			const auto& Pair = Storage->Values->Get(Id); *MatchedKey = &Pair.Key; return &Pair.Value;
		}
		return Storage->Values->Find(Key);
	}
	const FUeAgentCanonicalValueRef* FSemanticUnit::FindValue(const TCHAR* Key, const FString** MatchedKey) const
	{
		if (!Storage->InlinePropertyValue) return FindValue(FString(Key), MatchedKey);
		if (MatchedKey) *MatchedKey = nullptr;
		const FString& Name = FSinglePropertyValue::GetFieldName(Storage->InlinePropertyField);
		if (!Key || !FStringView(Key).Equals(FStringView(Name), ESearchCase::IgnoreCase)) return nullptr;
		if (MatchedKey) *MatchedKey = &Name;
		return &Storage->InlinePropertyValue.GetValue();
	}
	int32 FSemanticUnit::GetValueCount() const { return Storage->InlinePropertyValue ? 1 : Storage->Values->Num(); }
	bool FSemanticUnit::VisitValues(TFunctionRef<bool(const FString&, const FUeAgentCanonicalValueRef&)> Visitor) const
	{
		if (Storage->InlinePropertyValue) {
			return Visitor(FSinglePropertyValue::GetFieldName(Storage->InlinePropertyField), Storage->InlinePropertyValue.GetValue());
		}
		for (const auto& Pair : *Storage->Values) if (!Visitor(Pair.Key, Pair.Value)) return false;
		return true;
	}
	bool FSemanticUnit::HasInlinePropertyValue() const { return Storage->InlinePropertyValue.IsSet(); }
	bool FSemanticUnit::HasExpandedInlineValueMap() const { return Storage->ExpandedInlineState.load(std::memory_order_acquire) == 2; }
	const TArray<FString>& FSemanticUnit::GetItemRefs() const
	{
		return *Storage->Declaration->ItemRefs;
	}
	const TArray<FString>& FSemanticUnit::GetRelationRefs() const
	{
		return *Storage->Declaration->RelationRefs;
	}
	const FString& FSemanticUnit::GetOrderingRef() const
	{
		return Storage->Declaration->OrderingRef;
	}
	const FString& FSemanticUnit::GetLayoutRef() const { return Storage->Declaration->LayoutRef; }
	const TArray<FString>& FSemanticUnit::GetDependencyRefs() const
	{
		return *Storage->Declaration->DependencyRefs;
	}
	const TArray<FString>& FSemanticUnit::GetProfileVisibility() const
	{
		return *Storage->Declaration->ProfileVisibility;
	}
	const FString& FSemanticUnit::GetSourceProvenance() const
	{
		return Storage->Declaration->SourceProvenance.Get();
	}
	const FString& FSemanticUnit::GetTargetIdentityKey() const
	{
		return Storage->Declaration->TargetIdentityKey;
	}
	const TArray<FString>& FSemanticUnit::GetCategoryPath() const
	{
		return Storage->Declaration->CategoryPath;
	}
	bool FSemanticUnit::IsValid() const { return !Storage->ValidationDiagnostic; }
	FSemanticUnit::FDeclarationProof FSemanticUnit::GetDeclarationProof() const
	{
		FDeclarationProof Proof;
		Proof.Identity = Storage->Declaration;
		return Proof;
	}
	FSemanticUnit::FDeclarationProof FSemanticUnit::FDeclarationView::GetDeclarationProof() const
	{
		FDeclarationProof Proof;
		if (bValidated) Proof.Identity = Declaration;
		return Proof;
	}
	bool FSemanticUnit::MatchesCompiledDeclaration(const FDeclarationProof& Proof) const
	{
		// Even a value-only derivative must have passed its own value validation.
		if (!IsValid()) return false;
		const auto Previous = Proof.Identity.Pin();
		if (!Previous) return false;
		const auto& Current = *Storage->Declaration;
		if (Previous.Get() == &Current) return true;
		const auto& Before = *static_cast<const FStorage::FDeclaration*>(Previous.Get());
		const auto SameRefs = [](const TSharedPtr<const FUnitStrings>& A, const TSharedPtr<const FUnitStrings>& B) {
			if (A == B) return true;
			return A && B ? *A == *B : (!A || A->IsEmpty()) && (!B || B->IsEmpty());
		};
		return *Current.UnitId == *Before.UnitId && Current.UnitKind == Before.UnitKind
			&& Current.TargetIdentityKey == Before.TargetIdentityKey && *Current.OwnerRef == *Before.OwnerRef
			&& Current.SemanticTypeRef->Equals(*Before.SemanticTypeRef, ESearchCase::CaseSensitive)
			&& SameRefs(Current.ItemRefs, Before.ItemRefs) && SameRefs(Current.RelationRefs, Before.RelationRefs)
			&& Current.OrderingRef == Before.OrderingRef && Current.LayoutRef == Before.LayoutRef
			&& SameRefs(Current.DependencyRefs, Before.DependencyRefs);
	}
	const Contract::FDiagnostic& FSemanticUnit::GetValidationDiagnostic() const {
		static const Contract::FDiagnostic Empty;
		return Storage->ValidationDiagnostic ? *Storage->ValidationDiagnostic : Empty;
	}

	// Only the compiler constructs this proof, after identity, target, reference
	// and owner-cycle validation. It contains indexes, never unit values or UObjects.
	struct FCompiledUnitIndexKey
	{
		// Stored keys share the declaration's existing immutable identity. A
		// lookup borrows its argument for the duration of the existing map call.
		// No per-unit string object or old model/value graph is created here.
		TSharedPtr<const FString, ESPMode::ThreadSafe> Storage;
		const FString* Text;
		FCompiledUnitIndexKey(const FString& InText) : Text(&InText) {}
		FCompiledUnitIndexKey(const TSharedRef<const FString, ESPMode::ThreadSafe>& InText)
			: Storage(InText), Text(&InText.Get()) {}
		friend uint32 GetTypeHash(const FCompiledUnitIndexKey& Key) { return GetTypeHash(*Key.Text); }
		bool operator==(const FCompiledUnitIndexKey& Other) const { return *Text == *Other.Text; }
	};
	struct FCompiledAuthoringStructure
	{
		TMap<FCompiledUnitIndexKey, int32> UnitIndex;
		mutable TMap<uint32, TArray<int32>> TargetIdentityIndex;
		mutable bool bTargetIdentityIndexBuilt = false;
		TSet<FString> SemanticTypePrefixes;
		int64 SemanticTypePrefixBytes = 0;
		int64 ReferenceCount = 0;
		void AddSemanticType(const FString& Type) {
			for (int32 Position = Type.Find(TEXT("#")); Position != INDEX_NONE;
				Position = Type.Find(TEXT("#"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Position + 1))
				SemanticTypePrefixes.Add(Type.Left(Position).ToLower());
		}
	};
	int32 FindCompiledUnitIndex(const FCompiledAuthoringStructure& Structure, const FString& UnitId)
	{
		const auto* Index = Structure.UnitIndex.Find(UnitId);
		return Index ? *Index : INDEX_NONE;
	}

	int64 FCompiledAuthoringReuseProof::GetAllocatedSize() const
	{
		int64 Bytes = sizeof(*this) + 32 + Declarations.GetAllocatedSize() + TargetUnitIds.GetAllocatedSize();
		for (const FString& Id : TargetUnitIds) Bytes += Id.GetAllocatedSize();
		return Bytes;
	}
	TSharedRef<const FCompiledAuthoringReuseProof> FAuthoringModel::CreateCompilationReuseProof() const
	{
		auto Proof = MakeShared<FCompiledAuthoringReuseProof>();
		if (!bCompiled) return Proof;
		Proof->Structure = Structure;
		Proof->TargetUnitIds = TargetUnitIds;
		Proof->Declarations.Reserve(Units.Num());
		for (const auto& Unit : Units) Proof->Declarations.Add(Unit.GetDeclarationProof());
		return Proof;
	}
	const TSet<FString>& FAuthoringModel::GetSemanticTypePrefixes() const { return Structure->SemanticTypePrefixes; }
	bool TryReadPersistedDefinitionProjectionBinding(const FSemanticUnit& Unit,
		const FString& UnitPointer, const FString& PublicRef, FDefinitionProjectionBinding& OutBinding)
	{
		OutBinding = {};
		if (Unit.GetUnitKind() != ESemanticUnitKind::CollectionState
			|| Unit.GetRole() != TEXT("property_definition")
			|| !UnitPointer.StartsWith(TEXT("/definitions/"), ESearchCase::CaseSensitive)) return false;
		FString Name = UnitPointer.Mid(13);
		if (Name.IsEmpty() || Name.Contains(TEXT("/"))) return false;
		// Decode one JSON-pointer token, rejecting malformed escapes rather than
		// interpreting a legacy/unknown physical layout as a current name.
		FString Decoded;
		for (int32 I = 0; I < Name.Len(); ++I) {
			if (Name[I] != TEXT('~')) { Decoded.AppendChar(Name[I]); continue; }
			if (++I >= Name.Len() || (Name[I] != TEXT('0') && Name[I] != TEXT('1'))) return false;
			Decoded.AppendChar(Name[I] == TEXT('0') ? TEXT('~') : TEXT('/'));
		}
		const auto* Type = Unit.FindValue(TEXT("type"));
		if (!Type || (*Type)->GetKind() != EUeAgentValueKind::Object
			|| !IsStableOrLegacyProjectionName(Decoded, ReadablePropertyTypeName(Type->Get()), Unit.GetUnitId())) return false;
		FWorkspaceError Error;
		// Definition bindings cross the source/native representation boundary.
		// Use the existing exact canonical-row byte grammar on both sides, as
		// inventory does; do not compare that SHA with an in-memory material SHA.
		if (!TryHashSemanticUnitRow(Unit, OutBinding.ContentKey, Error)) return false;
		OutBinding.DefinitionName = MoveTemp(Decoded);
		// Global handles can append -N to a scoped readable alias. Only this
		// owner's exact allocation grammar can recover that optional hint.
		if (PublicRef.StartsWith(TEXT("collection:"), ESearchCase::CaseSensitive)) {
			FString Alias = PublicRef.Mid(11);
			const FString Base = SemanticAliasBase(Unit);
			if (!IsStableOrLegacyProjectionName(Alias, Base, Unit.GetUnitId())) {
				int32 Dash = INDEX_NONE;
				if (Alias.FindLastChar(TEXT('-'), Dash)) {
					const FString Ordinal = Alias.Mid(Dash + 1);
					bool bOrdinal = !Ordinal.IsEmpty() && Ordinal[0] != TEXT('0') && Ordinal != TEXT("1");
					for (TCHAR Character : Ordinal) bOrdinal &= Character >= TEXT('0') && Character <= TEXT('9');
					if (bOrdinal) Alias.LeftInline(Dash);
				}
			}
			if (IsStableOrLegacyProjectionName(Alias, Base, Unit.GetUnitId())) {
				OutBinding.ReadableAlias = MoveTemp(Alias);
				OutBinding.PublicRef = PublicRef;
			}
		}
		return true;
	}

	FAuthoringModel::FAuthoringModel(
		FString InContractId,
		int32 InContractVersion,
		FString InProfileId,
		FString InBaseRevision,
		TArray<FSemanticUnit> InUnits,
		TArray<FString> InTargetUnitIds,
		FString InSemanticFingerprint,
		TSharedRef<const FCompiledAuthoringStructure> InStructure,
		const FDefinitionProjectionHints* ProjectionHints, bool bInCompiled)
		: ContractId(MoveTemp(InContractId))
		, ContractVersion(InContractVersion)
		, ProfileId(MoveTemp(InProfileId))
		, BaseRevision(MoveTemp(InBaseRevision))
		, Units(MoveTemp(InUnits))
		, TargetUnitIds(MoveTemp(InTargetUnitIds))
		, Structure(MoveTemp(InStructure))
		, SemanticFingerprint(MoveTemp(InSemanticFingerprint))
		, bCompiled(bInCompiled)
	{
		if (!bCompiled) return;
		InitializeDefinitionProjection(ProjectionHints);
	}
	void FAuthoringModel::InitializeDefinitionProjection(const FDefinitionProjectionHints* ProjectionHints,
		const TMap<FString, FDefinitionProjectionBinding>* AcceptedBindings)
	{
		ReadablePropertyDefinitionNames.Reset(); PreferredDefinitionBindings.Reset(); DefinitionProjectionCollision.Reset();
		if (ProjectionHints && ProjectionHints->ContractId.Equals(ContractId, ESearchCase::CaseSensitive)
			&& ProjectionHints->ContractVersion == ContractVersion
			&& ProjectionHints->ProfileId.Equals(ProfileId, ESearchCase::CaseSensitive)
			&& ProjectionHints->TargetUnitIds == TargetUnitIds)
			AcceptedBindings = &ProjectionHints->Definitions;
		if (AcceptedBindings) {
			VisitDefinitionDeclarations([&](const FSemanticUnit::FDeclarationView& Declaration) {
				if (Declaration.GetUnitKind() != ESemanticUnitKind::CollectionState
					|| Declaration.GetRole() != TEXT("property_definition")) return true;
				const auto* Unit = FindUnit(Declaration.GetUnitId());
				if (!Unit) return false;
				const auto* Hint = AcceptedBindings->Find(Declaration.GetUnitId());
				FString ContentKey;
				FWorkspaceError HintError;
				if (Hint && TryHashSemanticUnitRow(*Unit, ContentKey, HintError)
					&& ContentKey.Equals(Hint->ContentKey, ESearchCase::CaseSensitive))
					PreferredDefinitionBindings.Add(Declaration.GetUnitId(), *Hint);
				return true;
			});
		}
		// Definition aliases are local to their owning catalog. They are not type names.
		TMap<FString, TMap<FString, TArray<FString>>> DefinitionsByOwner;
		VisitDefinitionDeclarations([&](const FSemanticUnit::FDeclarationView& Declaration) {
			if (Declaration.GetRole() != TEXT("property_definition")) return true;
			const auto* Unit = FindUnit(Declaration.GetUnitId());
			if (!Unit) return false;
			const auto* Type = Unit->FindValue(TEXT("type"));
			if (Type && (*Type)->GetKind() == EUeAgentValueKind::Object)
				DefinitionsByOwner.FindOrAdd(Declaration.GetOwnerRef())
					.FindOrAdd(ReadablePropertyTypeName(Type->Get())).Add(Declaration.GetUnitId());
			return true;
		});
		for (auto& Owner : DefinitionsByOwner)
		{
			TArray<FString> Names;
			Owner.Value.GetKeys(Names); Names.Sort();
			TSet<FString> Reserved;
			for (const FString& Name : Names) Reserved.Add(Name.ToLower());
			TSet<FString> Used;
			for (const FString& Name : Names)
			{
				auto& Definitions = Owner.Value.FindChecked(Name);
				Definitions.Sort();
				for (const FString& Definition : Definitions)
				{
					const auto* Preferred = PreferredDefinitionBindings.Find(Definition);
					if (!Preferred || !IsStableOrLegacyProjectionName(Preferred->DefinitionName, Name, Definition)) continue;
					const FString Folded = Preferred->DefinitionName.ToLower();
					if (Used.Contains(Folded) || (!Preferred->DefinitionName.Equals(Name, ESearchCase::IgnoreCase)
						&& Reserved.Contains(Folded))) continue;
					Used.Add(Folded);
					ReadablePropertyDefinitionNames.Add(Definition, Preferred->DefinitionName);
				}
				for (const FString& Definition : Definitions)
				{
					if (ReadablePropertyDefinitionNames.Contains(Definition)) continue;
					const FString Alias = StableProjectionName(Name, Definition);
					if (Used.Contains(Alias.ToLower())) { DefinitionProjectionCollision = Definition; return; }
					Used.Add(Alias.ToLower());
					ReadablePropertyDefinitionNames.Add(Definition, Alias);
				}
			}
		}
	}

	bool FAuthoringModel::VisitDefinitionDeclarations(TFunctionRef<bool(const FSemanticUnit::FDeclarationView&)> Visitor) const
	{
		const auto Visit = [&](const FSemanticUnit::FDeclarationView& Declaration) {
			if (RemovedUnitIds.Contains(Declaration.GetUnitId())) return true;
			const auto* Replacement = RebasedValues.Find(Declaration.GetUnitId());
			const auto Current = Replacement ? Replacement->GetDeclaration() : Declaration;
			return Current.GetRole() != TEXT("property_definition") || Visitor(Current);
		};
		if (bScopedOutputEnumeration)
			return VisitDeclarations([&](const FSemanticUnit::FDeclarationView& Declaration, int32) { return Visit(Declaration); });
		if (SourceRows) {
			if (!SourceRows->VisitDefinitionDeclarations(Visit)) return false;
			for (const auto& Id : AddedUnitIds) {
				const auto* Unit = RebasedValues.Find(Id);
				if (Unit && Unit->GetRole() == TEXT("property_definition") && !Visitor(Unit->GetDeclaration())) return false;
			}
			return true;
		}
		if (PublicationValues && PublicationValues->GetPublishedUnitCount().IsSet())
			return PublicationValues->VisitPublishedDefinitionDeclarations(Visit);
		return VisitDeclarations([&](const FSemanticUnit::FDeclarationView& Declaration, int32) { return Visit(Declaration); });
	}

	bool FAuthoringModel::TryRebindDefinitionProjection(const FDefinitionProjectionHints* ProjectionHints,
		FAuthoringModelPtr& OutModel, FWorkspaceError& OutError) const
	{
		if (PublicationValues || SourceRows) {
			TSharedPtr<FAuthoringModel> Rebound = MakeShareable(new FAuthoringModel(ContractId, ContractVersion, ProfileId,
				BaseRevision, {}, TargetUnitIds, SemanticFingerprint, Structure, nullptr, false));
			Rebound->bCompiled = bCompiled;
			Rebound->DeclarationDirectory = DeclarationDirectory;
			Rebound->PublicationValues = PublicationValues;
			Rebound->SourceRows = SourceRows;
			Rebound->RebasedValues = RebasedValues;
			Rebound->DeltaUnitCount = DeltaUnitCount;
			Rebound->AddedUnitIds = AddedUnitIds; Rebound->RemovedUnitIds = RemovedUnitIds;
			Rebound->bScopedOutputEnumeration = bScopedOutputEnumeration;
			Rebound->InitializeDefinitionProjection(ProjectionHints);
			if (!Rebound->CheckValueReadResult(OutError)) return false;
			if (!Rebound->DefinitionProjectionCollision.IsEmpty()) return Fail(OutError, TEXT("semantic_definition_alias_collision"),
				TEXT("A stable definition name conflicts with another bound identity; no existing definition was renamed."), Rebound->DefinitionProjectionCollision);
			OutModel = MoveTemp(Rebound); return true;
		}
		// Unit handles share their immutable material; no native observation,
		// declaration validation or flat semantic digest is recomputed here.
		OutModel = MakeShareable(new FAuthoringModel(ContractId, ContractVersion, ProfileId,
			BaseRevision, GetUnits(), TargetUnitIds, SemanticFingerprint, Structure, ProjectionHints, bCompiled));
		if (!OutModel->DefinitionProjectionCollision.IsEmpty()) {
			const FString UnitId = OutModel->DefinitionProjectionCollision;
			OutModel.Reset();
			return Fail(OutError, TEXT("semantic_definition_alias_collision"),
				TEXT("A stable definition name conflicts with another bound identity; no existing definition was renamed."), UnitId);
		}
		return true;
	}

	const FString* FAuthoringModel::FindReadablePropertyDefinitionName(const FString& UnitId) const
	{
		if (const auto* Name = ReadablePropertyDefinitionNames.Find(UnitId)) return Name;
		if (SourceRows) return SourceRows->FindReadablePropertyDefinitionName(UnitId);
		if (PublicationValues) {
			if (const auto* Existing = PublishedDefinitionBindings.Find(UnitId)) return Existing->IsValid() ? &(*Existing)->DefinitionName : nullptr;
			const auto Binding = PublicationValues->FindPublishedDefinitionBinding(UnitId);
			PublishedDefinitionBindings.Add(UnitId, Binding);
			if (Binding) return &Binding->DefinitionName;
		}
		return nullptr;
	}
	const TMap<FString, FDefinitionProjectionBinding>& FAuthoringModel::GetPreferredDefinitionBindings() const
	{
		if (!PreferredDefinitionBindings.IsEmpty()) return PreferredDefinitionBindings;
		if (SourceRows) return SourceRows->GetPreferredDefinitionBindings();
		if (PublicationValues) {
			if (!PublishedDefinitionHints) PublishedDefinitionHints = PublicationValues->ReadPublishedDefinitionBindings();
			if (PublishedDefinitionHints) return PublishedDefinitionHints->Definitions;
		}
		return PreferredDefinitionBindings;
	}

	const FString& FAuthoringModel::GetContractId() const
	{
		return ContractId;
	}
	int32 FAuthoringModel::GetContractVersion() const
	{
		return ContractVersion;
	}
	const FString& FAuthoringModel::GetProfileId() const
	{
		return ProfileId;
	}
	const FString& FAuthoringModel::GetBaseRevision() const
	{
		return BaseRevision;
	}
	const TArray<FSemanticUnit>& FAuthoringModel::GetUnits() const
	{
		if (SourceRows && DeltaUnitCount.IsSet() && Units.IsEmpty()) {
			TArray<FSemanticUnit> Complete; Complete.Reserve(GetUnitCount());
			if (!VisitUnits([&](const FSemanticUnit& Unit) { Complete.Add(Unit); return true; })) return Units;
			Units = MoveTemp(Complete);
			UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=publication_values_materialized units=%d reason=explicit_complete_view_consumer"), Units.Num());
			return Units;
		}
		if ((PublicationValues || (SourceRows && DeclarationDirectory)) && Units.IsEmpty()) {
			TArray<FSemanticUnit> Complete;
			Complete.Reserve(GetDeclarations().Num());
			for (const auto& Declaration : GetDeclarations()) {
				const auto* Unit = FindUnit(Declaration.GetUnitId());
				if (!Unit) return Units; // No partially complete array escapes.
				Complete.Add(*Unit);
			}
			Units = MoveTemp(Complete);
			UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=publication_values_materialized units=%d reason=explicit_complete_view_consumer"), Units.Num());
		}
		if (SourceRows && !DeclarationDirectory && Units.IsEmpty()) {
			Units = SourceRows->GetUnits();
			for (auto& Unit : Units) if (const auto* Value = RebasedValues.Find(Unit.GetUnitId())) Unit = *Value;
			UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=property_source_materialized units=%d reason=complete_view_consumer"), Units.Num());
		}
		return Units;
	}
	bool FAuthoringModel::VisitUnits(TFunctionRef<bool(const FSemanticUnit&)> Visitor) const
	{
		if (SourceRows && DeltaUnitCount.IsSet()) return VisitDeclarations([&](const FSemanticUnit::FDeclarationView& Declaration, int32) {
			const auto* Unit = FindUnit(Declaration.GetUnitId()); return Unit && Visitor(*Unit);
		});
		if (PublicationValues || (SourceRows && DeclarationDirectory)) {
			for (const auto& Declaration : GetDeclarations()) {
				const auto* Unit = FindUnit(Declaration.GetUnitId());
				if (!Unit || !Visitor(*Unit)) return false;
			}
			return true;
		}
		if (SourceRows) return SourceRows->VisitUnits([&](const FSemanticUnit& Unit) {
			const auto* Replacement = RebasedValues.Find(Unit.GetUnitId());
			return Visitor(Replacement ? *Replacement : Unit);
		});
		for (const auto& Unit : Units) if (!Visitor(Unit)) return false;
		return true;
	}
	bool FAuthoringModel::VisitDeclarations(TFunctionRef<bool(const FSemanticUnit::FDeclarationView&, int32)> Visitor) const
	{
		const auto Visit = [&](const FSemanticUnit::FDeclarationView& Declaration, int32 Index) {
			if (const auto* Replacement = RebasedValues.Find(Declaration.GetUnitId()))
				return Visitor(Replacement->GetDeclaration(), Index);
			return Visitor(Declaration, Index);
		};
		if (DeclarationDirectory && (PublicationValues || SourceRows)) {
			for (int32 Index = 0; Index < DeclarationDirectory->Num(); ++Index)
				if (!Visit((*DeclarationDirectory)[Index], Index)) return false;
			return true;
		}
		if (SourceRows && DeltaUnitCount.IsSet()) {
			int32 Index = 0;
			if (!SourceRows->VisitDeclarations([&](const FSemanticUnit::FDeclarationView& Declaration, int32) {
				return RemovedUnitIds.Contains(Declaration.GetUnitId()) || Visit(Declaration, Index++);
			})) return false;
			for (const auto& Id : AddedUnitIds) if (!Visitor(RebasedValues.FindChecked(Id).GetDeclaration(), Index++)) return false;
			return true;
		}
		if (SourceRows) return SourceRows->VisitDeclarations(Visit);
		if (PublicationValues && PublicationValues->GetPublishedUnitCount().IsSet())
			return PublicationValues->VisitPublishedDeclarations(Visit);
		if (!Units.IsEmpty()) {
			for (int32 Index = 0; Index < Units.Num(); ++Index)
				if (!Visit(Units[Index].GetDeclaration(), Index)) return false;
		} else if (DeclarationDirectory) for (int32 Index = 0; Index < DeclarationDirectory->Num(); ++Index)
			if (!Visit((*DeclarationDirectory)[Index], Index)) return false;
		return true;
	}
	const TArray<FSemanticUnit::FDeclarationView>& FAuthoringModel::GetDeclarations() const
	{
		return GetSharedDeclarations().Get();
	}
	TSharedRef<const TArray<FSemanticUnit::FDeclarationView>> FAuthoringModel::GetSharedDeclarations() const
	{
		if (!DeclarationDirectory) {
			auto Rows = MakeShared<TArray<FSemanticUnit::FDeclarationView>>();
			Rows->Reserve(GetUnitCount());
			VisitDeclarations([&](const FSemanticUnit::FDeclarationView& Declaration, int32) {
				Rows->Add(Declaration); return true;
			});
			DeclarationDirectory = MoveTemp(Rows);
		}
		return DeclarationDirectory.ToSharedRef();
	}
	TSharedRef<const FCompiledAuthoringStructure> FAuthoringModel::GetSharedUnitIndex() const
	{
		if (SourceRows && !DeclarationDirectory && !DeltaUnitCount.IsSet()) return SourceRows->GetSharedUnitIndex();
		if (((PublicationValues && PublicationValues->GetPublishedUnitCount().IsSet()) || DeltaUnitCount.IsSet()) && !bPublicationIndexMaterialized) {
			auto Index = MakeShared<FCompiledAuthoringStructure>();
			const auto& Declarations = GetDeclarations();
			for (int32 Position = 0; Position < Declarations.Num(); ++Position) {
				Index->UnitIndex.Add(Declarations[Position].GetSharedUnitId(), Position);
				Index->AddSemanticType(Declarations[Position].GetSemanticTypeRef());
			}
			Structure = MoveTemp(Index); bPublicationIndexMaterialized = true;
		}
		return Structure;
	}
	FString FAuthoringModel::GetRowHashAt(int32 Index) const
	{
		const auto& Declaration = GetDeclarations()[Index];
		return ReadRowHashByIdentity(Declaration.GetUnitId());
	}
	FString FAuthoringModel::ReadRowHashByIdentity(const FString& UnitId) const
	{
		if (const auto* Replacement = RebasedValues.Find(UnitId)) return Replacement->GetPublicRowHash();
		if (SourceRows) return SourceRows->ReadRowHashByIdentity(UnitId);
		if (!PublicationValues) {
			const auto* Index = Structure->UnitIndex.Find(UnitId);
			return Index && Units.IsValidIndex(*Index) ? Units[*Index].GetPublicRowHash() : FString();
		}
		FString Hash = PublicationValues->ReadBaselineHash(UnitId);
		if (!Hash.IsEmpty()) return Hash;
		FWorkspaceError Error;
		Error.SetSourceFailure(TEXT("workspace_captured_hash_unavailable"),
			TEXT("The required published row has no accepted content digest."), UnitId);
		RecordScopedCapturedSemanticReadFailure(*PublicationValues, Error);
		return FString();
	}
	bool FAuthoringModel::CheckValueReadResult(FWorkspaceError& OutError) const
	{
		if (PublicationValues) if (const auto* Failure = FindScopedCapturedSemanticReadFailure(*PublicationValues)) {
			OutError = *Failure; return false;
		}
		if (PublicationValues) if (const auto* Failure = PublicationValues->GetReadFailure(); Failure && Failure->HasErrors()) {
			OutError = *Failure; return false;
		}
		return !SourceRows || SourceRows->CheckValueReadResult(OutError);
	}
	TOptional<FSemanticUnit::FDeclarationView> FAuthoringModel::FindDeclaration(const FString& Id) const
	{
		if (RemovedUnitIds.Contains(Id)) return {};
		if (const auto* Replacement = RebasedValues.Find(Id)) return Replacement->GetDeclaration();
		if (SourceRows && (!DeclarationDirectory || DeltaUnitCount.IsSet() || bScopedOutputEnumeration)) return SourceRows->FindDeclaration(Id);
		if (PublicationValues && PublicationValues->GetPublishedUnitCount().IsSet())
			return PublicationValues->FindPublishedDeclaration(Id);
		const auto* Index = Structure->UnitIndex.Find(Id);
		if (!Index) return {};
		return !PublicationValues && !SourceRows && !Units.IsEmpty() ? TOptional<FSemanticUnit::FDeclarationView>(Units[*Index].GetDeclaration())
			: TOptional<FSemanticUnit::FDeclarationView>(GetDeclarations()[*Index]);
	}
	int32 FAuthoringModel::GetUnitCount() const {
		if (DeclarationDirectory) return DeclarationDirectory->Num();
		if (DeltaUnitCount.IsSet()) return DeltaUnitCount.GetValue();
		if (SourceRows) return SourceRows->GetUnitCount();
		if (PublicationValues) if (const auto Count = PublicationValues->GetPublishedUnitCount(); Count.IsSet()) return Count.GetValue();
		return Units.Num();
	}
	int32 FAuthoringModel::FindUnitIndex(const FString& UnitId) const
	{
		if (SourceRows && !DeclarationDirectory && !DeltaUnitCount.IsSet()) return SourceRows->FindUnitIndex(UnitId);
		const int32* Index = GetSharedUnitIndex()->UnitIndex.Find(UnitId);
		return Index ? *Index : INDEX_NONE;
	}
	const TArray<FString>& FAuthoringModel::GetTargetUnitIds() const
	{
		return TargetUnitIds;
	}
	const FSemanticUnit* FAuthoringModel::FindUnit(
		const FString& UnitId) const
	{
		if (RemovedUnitIds.Contains(UnitId)) return nullptr;
		if (const auto* Value = RebasedValues.Find(UnitId)) return Value;
		if (SourceRows) return (!DeclarationDirectory || DeltaUnitCount.IsSet() || bScopedOutputEnumeration || Structure->UnitIndex.Contains(UnitId)) ? SourceRows->FindUnit(UnitId) : nullptr;
		if (PublicationValues && PublicationValues->GetPublishedUnitCount().IsSet()) return PublicationValues->FindBaseline(UnitId);
		const int32* Index = Structure->UnitIndex.Find(UnitId);
		if (!Index) return nullptr;
		if (!PublicationValues) return &Units[*Index];
		const auto* Unit = PublicationValues->FindBaseline(UnitId);
		if (!Unit) {
			FWorkspaceError Error;
			if (const auto* Failure = PublicationValues->GetReadFailure(); Failure && Failure->HasErrors()) Error = *Failure;
			else Error.SetSourceFailure(TEXT("workspace_captured_unit_unavailable"),
				TEXT("The required published value row is unavailable; unrelated rows were not reconstructed."), UnitId);
			RecordScopedCapturedSemanticReadFailure(*PublicationValues, Error);
		}
		return Unit;
	}
	TArray<const FSemanticUnit*> FAuthoringModel::FindUnitsForTargetIdentity(const FString& TargetIdentity) const
	{
		TArray<const FSemanticUnit*> Matches;
		if (bScopedOutputEnumeration) {
			for (const auto& Declaration : *DeclarationDirectory) if (Declaration.GetTargetIdentityKey() == TargetIdentity)
				if (const auto* Unit = FindUnit(Declaration.GetUnitId())) Matches.Add(Unit);
			return Matches;
		}
		if (PublicationValues) {
			if (PublicationValues->GetPublishedUnitCount().IsSet()) {
				PublicationValues->VisitPublishedTargetIdentity(TargetIdentity, [&](const FSemanticUnit::FDeclarationView& Declaration) {
					if (const auto* Unit = FindUnit(Declaration.GetUnitId())) Matches.Add(Unit);
				});
				return Matches;
			}
			for (const auto& Declaration : GetDeclarations()) if (Declaration.GetTargetIdentityKey() == TargetIdentity)
				if (const auto* Unit = FindUnit(Declaration.GetUnitId())) Matches.Add(Unit);
			return Matches;
		}
		if (SourceRows) {
			for (const auto* Unit : SourceRows->FindUnitsForTargetIdentity(TargetIdentity))
				if (const auto* Current = FindUnit(Unit->GetUnitId()); Current && Current->GetTargetIdentityKey() == TargetIdentity) Matches.Add(Current);
			for (const auto& Pair : RebasedValues) if (Pair.Value.GetTargetIdentityKey() == TargetIdentity) Matches.AddUnique(&Pair.Value);
			return Matches;
		}
		// Index positions, not copies of long native identity strings. Compare the
		// complete key within the hash bucket and retain the model's unit order.
		// A complete export does not consume this native-lookup index. Build it
		// only when a real identity query needs it, within its existing owner.
		if (!Structure->bTargetIdentityIndexBuilt) {
			const double Started = FPlatformTime::Seconds();
			for (int32 I = 0; I < Units.Num(); ++I)
				Structure->TargetIdentityIndex.FindOrAdd(GetTypeHash(Units[I].GetTargetIdentityKey())).Add(I);
			Structure->bTargetIdentityIndexBuilt = true;
			UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=model_target_identity_index rows=%d buckets=%d consumer=native_identity_lookup total_s=%.6f"),
				Units.Num(), Structure->TargetIdentityIndex.Num(), FPlatformTime::Seconds() - Started);
		}
		if (const auto* Indices = Structure->TargetIdentityIndex.Find(GetTypeHash(TargetIdentity)))
			for (const int32 Index : *Indices)
				if (Units[Index].GetTargetIdentityKey() == TargetIdentity) Matches.Add(&Units[Index]);
		return Matches;
	}
	TArray<const FSemanticUnit*> FAuthoringModel::FindUnitsForSemanticType(const FString& SemanticType) const
	{
		TArray<const FSemanticUnit*> Matches;
		if (bScopedOutputEnumeration) {
			for (const auto& Declaration : *DeclarationDirectory) if (Declaration.GetSemanticTypeRef() == SemanticType)
				if (const auto* Unit = FindUnit(Declaration.GetUnitId())) Matches.Add(Unit);
			return Matches;
		}
		if (PublicationValues) {
			for (const auto& Declaration : GetDeclarations()) if (Declaration.GetSemanticTypeRef() == SemanticType)
				if (const auto* Unit = FindUnit(Declaration.GetUnitId())) Matches.Add(Unit);
			return Matches;
		}
		if (SourceRows) {
			for (const auto* Unit : SourceRows->FindUnitsForSemanticType(SemanticType))
				if (const auto* Current = FindUnit(Unit->GetUnitId()); Current && Current->GetSemanticTypeRef() == SemanticType) Matches.Add(Current);
			for (const auto& Pair : RebasedValues) if (Pair.Value.GetSemanticTypeRef() == SemanticType) Matches.AddUnique(&Pair.Value);
			return Matches;
		}
		// Read existing declaration metadata, without materializing a rebased
		// source's full Units array, values, compiler proof or export model.
		for (const auto& Unit : Units) if (Unit.GetSemanticTypeRef() == SemanticType) Matches.Add(&Unit);
		return Matches;
	}

	const FString& FAuthoringModel::GetSemanticFingerprint() const
	{
		return SemanticFingerprint;
	}
	FAuthoringModelRef FAuthoringModel::MakePropertySourceView(FString InContractId, int32 InContractVersion,
		FString InProfileId, FString InBaseRevision, TArray<FSemanticUnit> InUnits,
		TArray<FString> InTargetUnitIds, FString SourceStamp)
	{
		auto Index = MakeShared<FCompiledAuthoringStructure>();
		Index->UnitIndex.Reserve(InUnits.Num());
		for (int32 I = 0; I < InUnits.Num(); ++I) {
			Index->UnitIndex.Add(InUnits[I].GetSharedUnitId(), I);
			Index->AddSemanticType(InUnits[I].GetSemanticTypeRef());
		}
		FAuthoringModelPtr View = MakeShareable(new FAuthoringModel(MoveTemp(InContractId), InContractVersion,
			MoveTemp(InProfileId), MoveTemp(InBaseRevision), MoveTemp(InUnits), MoveTemp(InTargetUnitIds),
			TEXT("source:") + SourceStamp, Index, nullptr, false));
		return View.ToSharedRef();
	}
	FAuthoringModelRef FAuthoringModel::RebasePropertySourceView(FAuthoringModelRef Source,
		FString InBaseRevision, TMap<FString, FSemanticUnit> Replacements)
	{
		TSharedPtr<FAuthoringModel> View = MakeShareable(new FAuthoringModel(Source->ContractId, Source->ContractVersion,
			Source->ProfileId, MoveTemp(InBaseRevision), {}, Source->TargetUnitIds, Source->SemanticFingerprint,
			Source->Structure, nullptr, false));
		View->SourceRows = Source;
		View->RebasedValues = MoveTemp(Replacements);
		return View.ToSharedRef();
	}

	FAuthoringModelRef FAuthoringModel::MakePropertySourceView(FAuthoringModelRef PublishedRows,
		FString InBaseRevision, TMap<FString, FSemanticUnit> Replacements, FString SourceStamp)
	{
		TSharedPtr<FAuthoringModel> View = MakeShareable(new FAuthoringModel(PublishedRows->ContractId, PublishedRows->ContractVersion,
			PublishedRows->ProfileId, MoveTemp(InBaseRevision), {}, PublishedRows->TargetUnitIds,
			TEXT("source:") + SourceStamp, PublishedRows->Structure, nullptr, false));
		View->SourceRows = MoveTemp(PublishedRows); View->RebasedValues = MoveTemp(Replacements);
		View->ReadablePropertyDefinitionNames = View->SourceRows->ReadablePropertyDefinitionNames;
		View->PreferredDefinitionBindings = View->SourceRows->PreferredDefinitionBindings;
		// A definition edit changes this catalog's naming input. Reuse the
		// existing definition projection; its declaration filter only requests
		// definition values, never the complete publication value array.
		for (const auto& Pair : View->RebasedValues) if (Pair.Value.GetRole() == TEXT("property_definition")) {
			View->InitializeDefinitionProjection(nullptr); break;
		}
		return View.ToSharedRef();
	}
	FAuthoringModelRef FAuthoringModel::MakePublicationDirectory(const FAuthoringModel& Producer,
		TSharedRef<const FResolvedWorkspaceUnitBindings> Values,
		TSharedPtr<const TArray<FSemanticUnit::FDeclarationView>> CapturedDeclarations)
	{
		TSharedPtr<FAuthoringModel> View = MakeShareable(new FAuthoringModel(Producer.ContractId, Producer.ContractVersion,
			Producer.ProfileId, Producer.BaseRevision, {}, Producer.TargetUnitIds, Producer.SemanticFingerprint, Producer.Structure, nullptr, Producer.bCompiled));
		if (CapturedDeclarations) View->DeclarationDirectory = MoveTemp(CapturedDeclarations);
		else if (!Values->GetPublishedUnitCount().IsSet()) View->DeclarationDirectory = Producer.GetSharedDeclarations();
		View->ReadablePropertyDefinitionNames = Producer.ReadablePropertyDefinitionNames;
		View->PreferredDefinitionBindings = Producer.PreferredDefinitionBindings;
		View->PublicationValues = MoveTemp(Values);
		return View.ToSharedRef();
	}

	bool FAuthoringModel::TryMakeCommittedPublicationView(FAuthoringModelRef Published,
		const FCommittedWorkspaceDelta& Changes, FAuthoringModelPtr& OutModel, FWorkspaceError& OutError)
	{
		OutModel.Reset();
		bool bSameDeclarations = true;
		for (const auto& Change : Changes.Changes) {
			if (!Change.Before || !Change.After || !Change.After->MatchesCompiledDeclaration(Change.Before->GetDeclaration().GetDeclarationProof())
				|| Change.After->GetRole() == TEXT("property_definition")) { bSameDeclarations = false; break; }
		}
		if (bSameDeclarations) {
			TMap<FString, FSemanticUnit> Replacements;
			for (const auto& Change : Changes.Changes) Replacements.Add(Change.UnitId, *Change.After);
			// Same destination directory and a flat K actual overlay. This is
			// neither an N declaration copy/index nor a retained commit chain.
			OutModel = RebasePropertySourceView(Published, Changes.AfterRevision, MoveTemp(Replacements));
			return OutModel->CheckValueReadResult(OutError);
		}
		TSharedPtr<FAuthoringModel> View = MakeShareable(new FAuthoringModel(Changes.ContractId, Changes.ContractVersion,
			Changes.ProfileId, Changes.AfterRevision, {}, Changes.AfterTargets, FString(), Published->Structure, nullptr, false));
		int32 Count = Published->GetUnitCount();
		for (const auto& Change : Changes.Changes) {
			if (Change.After) {
				View->RebasedValues.Add(Change.UnitId, *Change.After);
				if (!Change.Before) { View->AddedUnitIds.Add(Change.UnitId); ++Count; }
			} else if (Change.Before) { View->RemovedUnitIds.Add(Change.UnitId); --Count; }
		}
		View->DeltaUnitCount = Count;
		View->SourceRows = MoveTemp(Published);
		View->bCompiled = true; // Actual rows already passed their original producer's validation.
		const auto& Bindings = View->SourceRows->GetPreferredDefinitionBindings();
		const bool bSameCatalog = View->ContractId == View->SourceRows->ContractId
			&& View->ContractVersion == View->SourceRows->ContractVersion && View->ProfileId == View->SourceRows->ProfileId
			&& View->TargetUnitIds == View->SourceRows->TargetUnitIds;
		View->InitializeDefinitionProjection(nullptr, bSameCatalog ? &Bindings : nullptr);
		if (!View->CheckValueReadResult(OutError)) return false;
		if (!View->DefinitionProjectionCollision.IsEmpty()) return Fail(OutError, TEXT("semantic_definition_alias_collision"),
			TEXT("A committed definition conflicts with an existing destination name."), View->DefinitionProjectionCollision);
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=committed_publication_view destination_units=%d changed_rows=%d added_rows=%d removed_rows=%d complete_declarations=false complete_index=false"),
			Count, View->RebasedValues.Num(), View->AddedUnitIds.Num(), View->RemovedUnitIds.Num());
		OutModel = MoveTemp(View);
		return true;
	}

	bool FAuthoringModel::TryMakeOutputScope(FAuthoringModelRef Context, const TArray<FString>& UnitIds,
		FAuthoringModelPtr& OutModel, FWorkspaceError& OutError)
	{
		OutModel.Reset();
		auto Declarations = MakeShared<TArray<FSemanticUnit::FDeclarationView>>();
		auto Index = MakeShared<FCompiledAuthoringStructure>();
		Declarations->Reserve(UnitIds.Num()); Index->UnitIndex.Reserve(UnitIds.Num());
		for (const auto& Id : UnitIds) {
			if (Index->UnitIndex.Contains(Id)) continue;
			const auto Declaration = Context->FindDeclaration(Id);
			if (!Declaration) {
				if (!Context->CheckValueReadResult(OutError)) return false;
				return Fail(OutError, TEXT("composition_output_unit_unavailable"),
					TEXT("The required output or naming context row has no accepted actual/publication fact."), Id);
			}
			Index->UnitIndex.Add(Declaration->GetSharedUnitId(), Declarations->Num());
			Index->AddSemanticType(Declaration->GetSemanticTypeRef()); Declarations->Add(Declaration.GetValue());
		}
		TSharedPtr<FAuthoringModel> View = MakeShareable(new FAuthoringModel(Context->ContractId, Context->ContractVersion,
			Context->ProfileId, Context->BaseRevision, {}, Context->TargetUnitIds, FString(), Index, nullptr, false));
		View->SourceRows = MoveTemp(Context); View->DeclarationDirectory = MoveTemp(Declarations);
		View->bScopedOutputEnumeration = true;
		OutModel = MoveTemp(View); return true;
	}

	bool FWorkspaceFileMembers::operator==(const FWorkspaceFileMembers& Other) const
	{
		if (Document == Other.Document) return true;
		if (Num() != Other.Num()) return false;
		for (int32 Index = 0; Index < Num(); ++Index)
			if (!(*this)[Index].Equals(Other[Index], ESearchCase::CaseSensitive)) return false;
		return true;
	}
	const FWorkspacePlacementStorage::FLocation* FWorkspacePlacementStorage::Find(const FString& Id) const
	{
		const auto* Index = UnitIndex ? UnitIndex->UnitIndex.Find(Id) : nullptr;
		return Index && Locations.IsValidIndex(*Index) && Locations[*Index].Document ? &Locations[*Index] : nullptr;
	}
	const FString& FWorkspacePlacementView::ValueAt(const FWorkspacePlacementStorage::FLocation& Row) const
	{
		if (Column == EColumn::Physical) return Row.Document->PhysicalAt(Row.Index);
		const auto& Item = Row.Document->Rows[Row.Index];
		return Column == EColumn::PublicRef ? Item.PublicRef : Item.LogicalPath;
	}
	const FString* FWorkspacePlacementView::Find(const FString& Id) const
	{
		const auto* Row = Storage->Find(Id);
		return Row ? &ValueAt(*Row) : nullptr;
	}
	const FString* FWorkspacePlacementView::FindReference(const FString& Id) const
	{
		if (const auto* Member = Find(Id)) return Member;
		return Column == EColumn::PublicRef ? Storage->ReferencedPublicRefs.Find(Id) : nullptr;
	}
	bool FWorkspacePlacementView::OrderIndependentCompareEqual(const FWorkspacePlacementView& Other) const
	{
		if (Storage == Other.Storage && Column == Other.Column) return true;
		if (Num() != Other.Num()) return false;
		for (const auto& Pair : *this) {
			const auto* Before = Other.Storage->Find(Pair.Key);
			if (!Before) return false;
			const FString& Value = Other.ValueAt(*Before);
			if (!Before->Document->Rows[Before->Index].UnitId.Get().Equals(Pair.Key, ESearchCase::CaseSensitive)
				|| !Value.Equals(Pair.Value, ESearchCase::CaseSensitive)) return false;
		}
		return true;
	}
	FWorkspacePlacementView::FConstIterator::FConstIterator(const FWorkspacePlacementView& InOwner, bool bInEnd)
		: Owner(&InOwner), Inner(InOwner.Storage->Files.CreateConstIterator()), bEnd(bInEnd) { if (!bEnd) Seek(); }
	void FWorkspacePlacementView::FConstIterator::Seek()
	{
		while (Inner && RowIndex >= Inner.Value().Num()) { ++Inner; RowIndex = 0; }
	}
	FWorkspacePlacementView::FConstIterator& FWorkspacePlacementView::FConstIterator::operator++()
	{
		++RowIndex; Seek(); return *this;
	}
	FWorkspacePlacementView::FReadPair FWorkspacePlacementView::FConstIterator::operator*() const
	{
		const auto& Document = Inner.Value().GetDocument();
		return {Document->Rows[RowIndex].UnitId.Get(), Owner->ValueAt({Document.Get(), RowIndex})};
	}
	bool FWorkspacePlacementView::FConstIterator::operator!=(const FConstIterator& Other) const
	{
		if (Owner != Other.Owner) return true;
		const bool bHere = !bEnd && !!Inner, bThere = !Other.bEnd && !!Other.Inner;
		if (!bHere || !bThere) return bHere != bThere;
		return &Inner.Key() != &Other.Inner.Key() || RowIndex != Other.RowIndex;
	}
	FWorkspaceLayoutPlan::FProjection::FProjection(TSharedRef<const FWorkspacePlacementStorage> InPlacements,
		FDefinitionProjectionHints InHints, TSharedPtr<const FSemanticReferenceNamespace> InNamespace)
		: Placements(MoveTemp(InPlacements)), LogicalPlacements(Placements, false), PhysicalPlacements(Placements, true),
		  PhysicalFiles(Placements->Files), DefinitionHints(MoveTemp(InHints)), ReferenceNamespace(MoveTemp(InNamespace)) {}

	FWorkspaceLayoutPlan::FWorkspaceLayoutPlan(
		FAuthoringModelRef InModel,
		int32 InLayoutVersion,
			EWorkspaceSourceMode InSourceMode,
			TSharedRef<const FWorkspacePlacementStorage> InPlacements,
		FMachineWorkspaceManifest InMachineManifest,
		TMap<FString, int64> InTelemetry,
		FDefinitionProjectionHints InDefinitionProjectionHints,
		TSharedPtr<const FSemanticReferenceNamespace> InReferenceNamespace)
		: Model(MoveTemp(InModel))
		, LayoutVersion(InLayoutVersion)
		, SourceMode(InSourceMode)
		, Projection(MakeShared<const FProjection>(MoveTemp(InPlacements), MoveTemp(InDefinitionProjectionHints), MoveTemp(InReferenceNamespace)))
		, MachineManifest(MoveTemp(InMachineManifest))
		, Telemetry(MoveTemp(InTelemetry))
	{
	}
	FWorkspaceLayoutPlan::FWorkspaceLayoutPlan(FAuthoringModelRef InModel,
		FMachineWorkspaceManifest InManifest, TMap<FString, int64> InTelemetry,
		const FWorkspaceLayoutPlan& ProvenProjection)
		: Model(MoveTemp(InModel)), LayoutVersion(ProvenProjection.LayoutVersion), SourceMode(ProvenProjection.SourceMode)
		, Projection(ProvenProjection.Projection), MachineManifest(MoveTemp(InManifest)), Telemetry(MoveTemp(InTelemetry))
	{
	}

	const FAuthoringModelRef& FWorkspaceLayoutPlan::GetModel() const
	{
		return Model;
	}
	TSharedRef<const FWorkspaceLayoutPlan> FWorkspaceLayoutPlan::WithPublicationDirectory(FAuthoringModelRef Directory) const
	{
		auto Manifest = MachineManifest;
		Manifest.UnitInventory = FWorkspaceUnitInventoryView(Directory);
		TSharedRef<const FWorkspaceLayoutPlan> Result = MakeShareable(new FWorkspaceLayoutPlan(MoveTemp(Directory),
			MoveTemp(Manifest), Telemetry, *this));
		if (PreviousLayout.Get() == this) PreviousLayout = Result;
		return Result;
	}
	void FWorkspaceLayoutPlan::RetireOptionalRetention() const
	{
		if (PreviousLayout.Get() == this) { PreviousLayout.Reset(); PreviousLayoutCharges.Reset(); }
	}
	bool FWorkspaceLayoutPlan::TryRestorePublicationDirectory(FAuthoringModelRef Published,
		const FAcceptedWorkspaceInventory& Inventory, const FDefinitionProjectionHints* Hints,
		TSharedPtr<const FWorkspaceLayoutPlan>& OutLayout, FWorkspaceError& OutError)
	{
		OutLayout.Reset();
		auto Storage = MakeShared<FWorkspacePlacementStorage>();
		Storage->UnitIndex = Published->GetSharedUnitIndex();
		Storage->Locations.SetNum(Published->GetUnitCount());
		TMap<FString, TSharedRef<FWorkspacePlacementDocument>> Documents;
		auto Names = MakeShared<FSemanticReferenceNamespace>();
		auto Handles = MakeShared<TSet<FString>>();
		Names->bAliasesFromPublishedRows = true;
		int32 Rows = 0;
		if (!Inventory.Visit([&](const FAcceptedWorkspaceInventoryRow& Row) {
			const int32 Index = Published->FindUnitIndex(Row.UnitId);
			if (Index == INDEX_NONE) return Fail(OutError, TEXT("semantic_publication_member_missing"),
				TEXT("An accepted publication member has no captured declaration."), Row.UnitId);
			const auto& Declaration = Published->GetDeclarations()[Index];
			auto* Document = Documents.Find(Row.DocumentPath);
			if (!Document) Document = &Documents.Add(Row.DocumentPath,
				MakeShared<FWorkspacePlacementDocument>(MakeShared<const FString>(Row.DocumentPath)));
			const int32 Position = (*Document)->Rows.Num();
			(*Document)->Rows.Add({Declaration.GetSharedUnitId(), Row.LogicalPath, FString(), false, Row.PublicRef});
			Storage->Locations[Index] = {&Document->Get(), Position};
			int32 Colon = INDEX_NONE;
			if (!Row.PublicRef.FindChar(TEXT(':'), Colon)) return Fail(OutError, TEXT("semantic_publication_ref_invalid"),
				TEXT("An accepted public reference has no kind prefix."), Row.UnitId);
			const FString Handle = Row.PublicRef.Mid(Colon + 1);
			Handles->Add(Handle.ToLower());
			if (Hints) if (const auto* Definition = Hints->Definitions.Find(Row.UnitId);
				Definition && Definition->ReadableAlias != Handle) Names->AliasExceptions.Add(Row.UnitId, Definition->ReadableAlias);
			++Rows; return true;
		})) return false;
		if (Rows != Published->GetUnitCount()) return Fail(OutError, TEXT("semantic_publication_membership_invalid"),
			TEXT("The accepted publication directory does not cover its captured declarations."), FString());
		for (const auto& Document : Documents) Storage->Files.Add(Document.Key, FWorkspaceFileMembers(Document.Value));
		Storage->Files.FindOrAdd(ManifestPath); Storage->Files.FindOrAdd(TEXT("index.hjson"));
		Names->Handles = MoveTemp(Handles);
		FMachineWorkspaceManifest Manifest;
		Manifest.WorkspaceSchema = GetSemanticWorkspaceSchemaId(); Manifest.LayoutVersion = 2;
		Manifest.ContractId = Published->GetContractId(); Manifest.ContractVersion = Published->GetContractVersion();
		Manifest.ProfileId = Published->GetProfileId(); Manifest.BaseRevision = Published->GetBaseRevision();
		Manifest.SemanticFingerprint = Published->GetSemanticFingerprint();
		Manifest.UnitInventory = FWorkspaceUnitInventoryView(Published);
		Manifest.PublicRefMapping = FWorkspacePlacementView(Storage, FWorkspacePlacementView::EColumn::PublicRef);
		FDefinitionProjectionHints ProjectionHints;
		if (Hints) ProjectionHints = *Hints;
		OutLayout = MakeShareable(new FWorkspaceLayoutPlan(Published, 2, EWorkspaceSourceMode::Directory,
			Storage, MoveTemp(Manifest), {}, MoveTemp(ProjectionHints), Names));
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=publication_layout_directory declarations=%d value_rows_read=0 allocator_not_run=true"), Rows);
		return true;
	}
	int32 FWorkspaceLayoutPlan::GetLayoutVersion() const
	{
		return LayoutVersion;
	}
	EWorkspaceSourceMode FWorkspaceLayoutPlan::GetSourceMode() const
	{
		return SourceMode;
	}
	const FString& FWorkspaceLayoutPlan::GetSemanticFingerprint() const
	{
		return Model->GetSemanticFingerprint();
	}
	const FWorkspacePlacementView&
	FWorkspaceLayoutPlan::GetLogicalPlacements() const
	{
		return Projection->LogicalPlacements;
	}
	const FWorkspacePlacementView&
	FWorkspaceLayoutPlan::GetPhysicalPlacements() const
	{
		return Projection->PhysicalPlacements;
	}
	const FWorkspacePhysicalFiles&
	FWorkspaceLayoutPlan::GetPhysicalFiles() const
	{
		return Projection->PhysicalFiles;
	}
	const FMachineWorkspaceManifest&
	FWorkspaceLayoutPlan::GetMachineManifest() const
	{
		return MachineManifest;
	}
	const TMap<FString, int64>&
	FWorkspaceLayoutPlan::GetTelemetry() const
	{
		return Telemetry;
	}
	const FDefinitionProjectionHints& FWorkspaceLayoutPlan::GetDefinitionProjectionHints() const
	{
		return Projection->DefinitionHints;
	}

	FString GetSemanticWorkspaceSchemaId()
	{
		return WorkspaceSchema;
	}

	FString LexToString(ESemanticUnitKind UnitKind)
	{
		return UnitKindName(UnitKind);
	}

	bool IsOpaqueSemanticIdentity(const FString& Value)
	{
		int32 PrefixEnd = INDEX_NONE;
		if (Value.FindLastChar(TEXT(':'), PrefixEnd) && PrefixEnd > 0)
		{
			const FString Suffix = Value.Mid(PrefixEnd + 1);
			bool bStableId = Suffix.Len() == 24;
			for (TCHAR Character : Suffix) bStableId &= FChar::IsHexDigit(Character);
			if (bStableId) return true;
		}
		const FString Candidate = SafeSegment(Value);
		// Parse also accepts arbitrary 22-character base64 names as Short GUIDs,
		// including UserConstructionScript. UUID detection must use explicit forms;
		// complete digest detection below applies only to fallback identity fields.
		if (IsExplicitGuidName(Candidate))
		{
			return true;
		}
		FString Digest = Value.TrimStartAndEnd();
		Digest.RemoveFromStart(TEXT("sha256:"), ESearchCase::IgnoreCase);
		Digest.RemoveFromStart(TEXT("sha1:"), ESearchCase::IgnoreCase);
		if (Digest.Len() != 40 && Digest.Len() != 64) return false;
		for (TCHAR Character : Digest) if (!FChar::IsHexDigit(Character)) return false;
		return true;
	}

	FString MakeReadableSemanticAlias(const FString& Value)
	{
		const FString Candidate = SafeSegment(Value);
		if (IsExplicitGuidName(Candidate))
			return FString();
		return Candidate;
	}

	class FMutableLayoutPlacements
	{
		struct FSlot
		{
			const FWorkspacePlacementDocument* Document = nullptr;
			int32 Row = INDEX_NONE;
			FString Logical, Physical;
			bool bLogical = false, bPhysical = false;
		};
		const FAuthoringModel& Model;
		TSharedPtr<const FWorkspaceLayoutPlan::FProjection> Before;
		TArray<FSlot> Slots;
		TBitArray<> Bound;
		template <typename TUnit> int32 Index(const TUnit& Unit) const { return Model.FindUnitIndex(Unit.GetUnitId()); }
		const FString* Value(int32 At, bool bPhysical) const {
			const auto& Slot = Slots[At];
			if (Slot.Document) return bPhysical ? &Slot.Document->PhysicalAt(Slot.Row) : &Slot.Document->Rows[Slot.Row].LogicalPath;
			return bPhysical ? (Slot.bPhysical ? &Slot.Physical : nullptr) : (Slot.bLogical ? &Slot.Logical : nullptr);
		}
		template <typename TUnit, typename StringType> void SetValue(const TUnit& Unit, bool bPhysical, StringType&& Text) {
			const int32 At = Index(Unit);
			if (const auto* Old = Value(At, bPhysical); Old && (Old == &Text || Old->Equals(Text, ESearchCase::CaseSensitive))) return;
			auto& Slot = Slots[At];
			if (Slot.Document) {
				Slot.Logical = Slot.Document->Rows[Slot.Row].LogicalPath;
				Slot.Physical = Slot.Document->PhysicalAt(Slot.Row);
				Slot.Document = nullptr; Slot.bLogical = Slot.bPhysical = true;
			}
			if (bPhysical) { Slot.Physical = Forward<StringType>(Text); Slot.bPhysical = true; }
			else { Slot.Logical = Forward<StringType>(Text); Slot.bLogical = true; }
		}
	public:
		enum class EColumn { Logical, Physical, BoundPhysical };
		class FColumn
		{
			FMutableLayoutPlacements& Owner;
			EColumn Kind;
			const FString* At(int32 Index) const {
				if (Kind == EColumn::BoundPhysical && !Owner.Bound[Index]) return nullptr;
				return Owner.Value(Index, Kind != EColumn::Logical);
			}
		public:
			FColumn(FMutableLayoutPlacements& InOwner, EColumn InKind) : Owner(InOwner), Kind(InKind) {}
			const FString* Find(const FString& Id) const { const int32 Index = Owner.Model.FindUnitIndex(Id); return Index == INDEX_NONE ? nullptr : At(Index); }
			template <typename TUnit> const FString* Find(const TUnit& Unit) const { return At(Owner.Index(Unit)); }
			const FString& FindChecked(const FString& Id) const { const auto* Text = Find(Id); check(Text); return *Text; }
			template <typename TUnit> const FString& FindChecked(const TUnit& Unit) const { const auto* Text = Find(Unit); check(Text); return *Text; }
			bool Contains(const FString& Id) const { return Find(Id) != nullptr; }
			template <typename TUnit> bool Contains(const TUnit& Unit) const { return Find(Unit) != nullptr; }
			template <typename TUnit> void Set(const TUnit& Unit, const FString& Text) { Owner.SetValue(Unit, Kind != EColumn::Logical, Text); }
			template <typename TUnit> void Set(const TUnit& Unit, FString&& Text) { Owner.SetValue(Unit, Kind != EColumn::Logical, MoveTemp(Text)); }
			struct FPair { const FString& Key; const FString& Value; };
			struct FIterator
			{
				const FColumn* Column; int32 Position;
				void Seek() { while (Position < Column->Owner.Slots.Num() && !Column->At(Position)) ++Position; }
				FPair operator*() const { return {Column->Owner.Model.GetDeclarations()[Position].GetUnitId(), *Column->At(Position)}; }
				FIterator& operator++() { ++Position; Seek(); return *this; }
				bool operator!=(const FIterator& Other) const { return Column != Other.Column || Position != Other.Position; }
			};
			FIterator begin() const { FIterator It{this, 0}; It.Seek(); return It; }
			FIterator end() const { return {this, Owner.Slots.Num()}; }
		};
		FMutableLayoutPlacements(const FAuthoringModel& InModel, TSharedPtr<const FWorkspaceLayoutPlan::FProjection> InBefore)
			: Model(InModel), Before(MoveTemp(InBefore)), Bound(false, InModel.GetUnitCount()) { Slots.SetNum(InModel.GetUnitCount()); }
		template <typename TUnit> void Bind(const TUnit& Unit, const FString& Logical, const FString& Physical) {
			const int32 At = Index(Unit);
			const auto* Old = Before ? Before->Placements->Find(Unit.GetUnitId()) : nullptr;
			if (Old && Old->Document->Rows[Old->Index].LogicalPath.Equals(Logical, ESearchCase::CaseSensitive)
				&& Old->Document->PhysicalAt(Old->Index).Equals(Physical, ESearchCase::CaseSensitive)) {
				Slots[At].Document = Old->Document; Slots[At].Row = Old->Index;
			} else { SetValue(Unit, false, Logical); SetValue(Unit, true, Physical); }
			Bound[At] = true;
		}
		TSharedRef<const FWorkspacePlacementStorage> Freeze(const TMap<FString, TArray<int32>>& Groups,
			TMap<FString, FString>& CurrentRefs, bool bBorrowedRefs,
			int32& ReusedDocuments, int32& ReusedRows, int32& BuiltRows) {
			auto Result = MakeShared<FWorkspacePlacementStorage>();
			Result->Files.Reserve(Groups.Num()); Result->UnitIndex = Model.GetSharedUnitIndex();
			Result->Locations.SetNum(Model.GetUnitCount());
			const auto ReadRef = [&](const FString& Id) -> const FString& {
				if (const auto* Value = CurrentRefs.Find(Id)) return *Value;
				const auto* Old = bBorrowedRefs && Before ? Before->Placements->Find(Id) : nullptr;
				check(Old); return Old->Document->Rows[Old->Index].PublicRef;
			};
			for (const auto& Group : Groups) {
				// Manifest/index entries own file membership, not semantic rows.
				// The cold publication directory represents them without a row
				// allocation; freezing that same empty set must not dereference or
				// recreate optional row backing.
				if (Group.Value.IsEmpty()) { Result->Files.FindOrAdd(Group.Key); continue; }
				const auto* Old = Before ? Before->PhysicalFiles.Find(Group.Key) : nullptr;
				const auto OldDocument = Old ? Old->GetDocument() : TSharedPtr<const FWorkspacePlacementDocument>();
				bool bReuse = OldDocument && OldDocument->Rows.Num() == Group.Value.Num()
					&& OldDocument->PhysicalPath->Equals(Group.Key, ESearchCase::CaseSensitive);
				if (bReuse) for (int32 Position = 0; Position < Group.Value.Num(); ++Position) {
					const int32 At = Group.Value[Position];
					const auto& Unit = Model.GetDeclarations()[At];
					const auto& Document = *OldDocument;
					if (!Document.Rows[Position].UnitId.Get().Equals(Unit.GetUnitId(), ESearchCase::CaseSensitive)
						|| !Document.Rows[Position].LogicalPath.Equals(*Value(At, false), ESearchCase::CaseSensitive)
						|| !Document.PhysicalAt(Position).Equals(*Value(At, true), ESearchCase::CaseSensitive)
						|| !Document.Rows[Position].PublicRef.Equals(ReadRef(Unit.GetUnitId()), ESearchCase::CaseSensitive)) { bReuse = false; break; }
				}
				TSharedPtr<const FWorkspacePlacementDocument> Document;
				if (bReuse) { Document = OldDocument; ++ReusedDocuments; ReusedRows += Group.Value.Num(); }
				else {
					// The document path has an independent immutable lifetime. A
					// changed row block need not duplicate it or retain retired rows.
					const auto Path = OldDocument && OldDocument->PhysicalPath->Equals(Group.Key, ESearchCase::CaseSensitive)
						? OldDocument->PhysicalPath : MakeShared<const FString>(Group.Key);
					auto Built = MakeShared<FWorkspacePlacementDocument>(Path); Built->Rows.Reserve(Group.Value.Num());
					for (int32 At : Group.Value) {
						auto& Slot = Slots[At]; const auto& Unit = Model.GetDeclarations()[At];
						FString Logical = Slot.Document ? Slot.Document->Rows[Slot.Row].LogicalPath : MoveTemp(Slot.Logical);
						const FString& Physical = *Value(At, true);
						const bool bOverride = !Physical.Equals(Group.Key, ESearchCase::CaseSensitive);
						FString Ref;
						if (auto* Current = CurrentRefs.Find(Unit.GetUnitId())) Ref = MoveTemp(*Current);
						else Ref = ReadRef(Unit.GetUnitId());
						Built->Rows.Add({Unit.GetSharedUnitId(), MoveTemp(Logical), bOverride ? Physical : FString(), bOverride, MoveTemp(Ref)});
					}
					BuiltRows += Group.Value.Num(); Document = MoveTemp(Built);
				}
				Result->Files.Add(Group.Key, FWorkspaceFileMembers(Document.ToSharedRef()));
				for (int32 Index = 0; Index < Document->Rows.Num(); ++Index)
					Result->Locations[Group.Value[Index]] = {Document.Get(), Index};
			}
			if (Model.IsOutputScope()) for (auto& Ref : CurrentRefs)
				if (Model.FindUnitIndex(Ref.Key) == INDEX_NONE) Result->ReferencedPublicRefs.Add(Ref.Key, MoveTemp(Ref.Value));
			return Result;
		}
	};

	template <typename PlacementType, typename AliasMap> bool TryBuildAuthoringContentDirectoriesImpl(const FAuthoringModel& Model,
		const PlacementType& Placements, TMap<FString, FString>& OutDirectories, FWorkspaceError& OutError,
		const PlacementType* ProvenPlacements, const AliasMap* ReadableAliases, bool bOnlyUnprovenUnits,
		const FSemanticUnit::FDeclarationView* Selected = nullptr)
	{
		OutDirectories.Reset();
		TMap<FString, FString> ProvenDirectories;
		TMap<FString, bool> GraphOwners;
		if (ProvenPlacements) for (const auto& Placement : *ProvenPlacements) {
			const auto Unit = Model.FindDeclaration(Placement.Key);
			if (!Unit || Placement.Value.StartsWith(TEXT(".uai/")) || IsGraphStructureUnit(*Unit)) continue;
			const bool* KnownGraph = GraphOwners.Find(Unit->GetOwnerRef());
			const bool bGraph = KnownGraph ? *KnownGraph : HasGraphStructureAncestor(*Unit, Model);
			if (!KnownGraph) GraphOwners.Add(Unit->GetOwnerRef(), bGraph);
			if (bGraph) continue;
			FString DirectoryOwner;
			switch (Unit->GetUnitKind()) {
			case ESemanticUnitKind::TargetState:
			case ESemanticUnitKind::EntityState:
			case ESemanticUnitKind::ItemState: DirectoryOwner = Unit->GetUnitId(); break;
			case ESemanticUnitKind::PropertySet:
				if (!Unit->GetRole().StartsWith(TEXT("preview/"))) DirectoryOwner = Unit->GetOwnerRef();
				break;
			case ESemanticUnitKind::OrderingState:
			case ESemanticUnitKind::RelationSet:
			case ESemanticUnitKind::LayoutState: DirectoryOwner = Unit->GetOwnerRef(); break;
			case ESemanticUnitKind::CollectionState:
				if (Unit->GetRole() == TEXT("property_definition")) DirectoryOwner = Unit->GetOwnerRef();
				break;
			default: break;
			}
			if (DirectoryOwner.IsEmpty()) continue;
			FString Directory = FPaths::GetPath(Placement.Value);
			if (Unit->GetSemanticTypeRef() == TEXT("uai.semantic.umg.slot.property")) Directory = FPaths::GetPath(Directory);
			if (const auto* Existing = ProvenDirectories.Find(DirectoryOwner)) {
				if (!Existing->Equals(Directory, ESearchCase::CaseSensitive))
					return Fail(OutError, TEXT("semantic_owner_directory_conflict"), TEXT("Accepted documents disagree about their owner's directory."), DirectoryOwner);
			} else ProvenDirectories.Add(DirectoryOwner, MoveTemp(Directory));
		}
		TSet<FString> Visiting;
		TSet<FString> UsedDirectories;
		for (const auto& Proven : ProvenDirectories) UsedDirectories.Add(Proven.Value.ToLower());
		TFunction<bool(const FSemanticUnit::FDeclarationView&)> Resolve = [&](const FSemanticUnit::FDeclarationView& Unit)
		{
			if (OutDirectories.Contains(Unit.GetUnitId())) return true;
			if (Visiting.Contains(Unit.GetUnitId())) return Fail(OutError, TEXT("semantic_owner_cycle"), TEXT("The semantic unit owner references contain a dependency cycle."), Unit.GetUnitId());
			Visiting.Add(Unit.GetUnitId());
			const auto Owner = Model.FindDeclaration(Unit.GetOwnerRef());
			const auto Role = GetAuthoringContentDirectoryRole(Unit.GetUnitKind(), Unit.GetSemanticTypeRef());
			FString Directory;
			if (const FString* Proven = ProvenDirectories.Find(Unit.GetUnitId())) Directory = *Proven;
			else if (Role == EAuthoringContentDirectory::Placement || !Owner)
			{
				const FString* Placement = Placements.Find(Unit.GetUnitId());
				Directory = Placement ? FPaths::GetPath(*Placement) : FString();
			}
			else
			{
				if (!Resolve(*Owner)) return false;
				Directory = OutDirectories.FindChecked(Owner->GetUnitId());
				FString OwnedPath;
				if (Role == EAuthoringContentDirectory::NamedChild)
				{
					FString Emitter, Usage;
					if (Unit.GetRole() == TEXT("stack_collection") || Unit.GetSemanticTypeRef().EndsWith(TEXT(".emitter_owner"))) {
						const auto* Value = Model.FindUnit(Unit.GetUnitId());
						if (!Value) { Model.CheckValueReadResult(OutError); return false; }
						Emitter = ReadSemanticString(*Value, TEXT("emitter_name"));
						Usage = ReadSemanticString(*Value, TEXT("usage"));
					}
					if (Unit.GetRole() == TEXT("stack_collection") && !Usage.IsEmpty())
					{
						// Stack capture carries the emitter relation even when its semantic owner is a surface.
						if (!Emitter.IsEmpty() && !Directory.EndsWith(TEXT("/emitters/") + MakeAuthoringContentPathSegment(Emitter)))
							OwnedPath = TEXT("emitters/") + MakeAuthoringContentPathSegment(Emitter) + TEXT("/");
						OwnedPath += TEXT("stacks/") + MakeAuthoringContentPathSegment(Usage);
					}
					else if (Unit.GetSemanticTypeRef().EndsWith(TEXT(".emitter_owner")) && !Emitter.IsEmpty())
						OwnedPath = TEXT("emitters/") + MakeAuthoringContentPathSegment(Emitter);
					else
					{
						// Every owned container needs its own directory, even when no
						// display name was captured. Reuse the public alias fallback;
						// inheriting the parent's directory merges unrelated properties.
						const FString* BoundAlias = ReadableAliases ? ReadableAliases->Find(Unit.GetUnitId()) : nullptr;
						FString Name;
						if (BoundAlias) Name = *BoundAlias;
						else {
							const auto* Value = Model.FindUnit(Unit.GetUnitId());
							if (!Value) { Model.CheckValueReadResult(OutError); return false; }
							Name = StableProjectionName(SemanticAliasBase(*Value), Unit.GetUnitId());
						}
						OwnedPath = (IsGraphStructureUnit(Unit) ? TEXT("graphs/") : TEXT("items/"))
							+ MakeAuthoringContentPathSegment(Name);
					}
				}
				if (!OwnedPath.IsEmpty())
				{
					Directory /= OwnedPath;
					if (UsedDirectories.Contains(Directory.ToLower()))
						return Fail(OutError, TEXT("semantic_owner_directory_collision"), TEXT("Distinct semantic identities cannot share a content directory."), Unit.GetUnitId());
					UsedDirectories.Add(Directory.ToLower());
				}
			}
			OutDirectories.Add(Unit.GetUnitId(), MoveTemp(Directory));
			Visiting.Remove(Unit.GetUnitId());
			return true;
		};
		if (Selected) return Resolve(*Selected);
		for (const auto& Declaration : Model.GetDeclarations()) {
			if (bOnlyUnprovenUnits && ProvenPlacements && ProvenPlacements->Contains(Declaration.GetUnitId())) continue;
			if (!Resolve(Declaration)) return false;
		}
		return true;
	}

	bool TryGetAcceptedNamedChildDirectory(const FAuthoringModel& Model, const FAcceptedWorkspaceInventory& Inventory,
		const FString& Id, FString& OutDirectory, FWorkspaceError& OutError)
	{
		OutDirectory.Reset();
		const auto Child = Model.FindDeclaration(Id);
		if (!Child) return Fail(OutError, TEXT("semantic_owner_directory_missing"), TEXT("The moved directory has no accepted owner declaration."), Id);
		if (IsGraphStructureUnit(*Child) || HasGraphStructureAncestor(*Child, Model)) return true;
		TMap<FString, FString> Placements, Aliases, Directories;
		TSet<FString> Visited;
		for (auto Unit = Child; Unit; Unit = Model.FindDeclaration(Unit->GetOwnerRef())) {
			const FString UnitId = Unit->GetUnitId();
			if (Visited.Contains(UnitId)) return Fail(OutError, TEXT("semantic_owner_cycle"), TEXT("The semantic unit owner references contain a dependency cycle."), UnitId);
			Visited.Add(UnitId);
			const auto Role = GetAuthoringContentDirectoryRole(Unit->GetUnitKind(), Unit->GetSemanticTypeRef());
			if (Role == EAuthoringContentDirectory::Inherited) continue;
			if (!Inventory.VisitUnit(UnitId, [&](const FAcceptedWorkspaceInventoryRow& Row) {
				if (Role == EAuthoringContentDirectory::Placement) Placements.Add(UnitId, Row.DocumentPath);
				else {
					const auto* Value = Model.FindUnit(UnitId);
					if (!Value || !ReadBoundProjectionAlias(*Value, Row.PublicRef, Aliases.FindOrAdd(UnitId))) return false;
				}
				return true;
			})) {
				if (!Model.CheckValueReadResult(OutError)) return false;
				return Fail(OutError, TEXT("semantic_owner_directory_missing"), TEXT("The moved directory's accepted placement or readable name is unavailable."), UnitId);
			}
			if (Role == EAuthoringContentDirectory::Placement) break;
		}
		if (!TryBuildAuthoringContentDirectoriesImpl(Model, Placements, Directories, OutError,
			static_cast<const TMap<FString, FString>*>(nullptr), &Aliases, false, &Child.GetValue())) return false;
		OutDirectory = Directories.FindChecked(Id);
		return true;
	}

	bool TryBuildAuthoringContentDirectories(const FAuthoringModel& Model,
		const FWorkspacePlacementView& Placements, TMap<FString, FString>& OutDirectories, FWorkspaceError& OutError,
		const FWorkspacePlacementView* ProvenPlacements, const TMap<FString, FString>* ReadableAliases, bool bOnlyUnprovenUnits)
	{
		return TryBuildAuthoringContentDirectoriesImpl(Model, Placements, OutDirectories, OutError,
			ProvenPlacements, ReadableAliases, bOnlyUnprovenUnits);
	}

	bool TryHashSemanticUnitContent(const FSemanticUnit& Unit, FString& OutHash, FWorkspaceError& OutError)
	{
		OutHash.Reset();
		OutError.Reset();
		if (!Unit.IsValid())
		{
			OutError.SetFailure(Unit.GetValidationDiagnostic(), Unit.GetUnitId());
			return false;
		}
		if (FSemanticUnitContentAccess::Read(Unit, OutHash)) { ++ContentDigestHits; return true; }
		++ContentDigestMisses;
		FUtf8Material Material;
		AppendUnitMaterial(Material, Unit);
		FSemanticUnitContentAccess::Seal(Unit, Material.Bytes.GetData(), Material.Bytes.Num());
		// A simultaneous producer need not be waited on: its digest has the
		// same immutable input, while this call already has the exact bytes.
		if (!FSemanticUnitContentAccess::Read(Unit, OutHash))
			OutHash = HashBytes(Material.Bytes.GetData(), Material.Bytes.Num());
		return true;
	}

	static bool IsSemanticObjectRoot(const FUeAgentCanonicalValue& Object) { return Object.GetKind() == EUeAgentValueKind::Object; }
	static bool IsSemanticObjectRoot(const FUeAgentStructuredHjsonObjectView&) { return true; }

	template<typename ObjectInput>
	static bool SerializeSemanticHjsonObject(
		const ObjectInput& Object,
		TArray<uint8>& OutCanonicalUtf8, FWorkspaceError& OutError, const FString& SourceUnit,
		const FString& RowArrayField, TMap<FString, FIntPoint>* OutByteSpans)
	{
		OutCanonicalUtf8.Reset();
		OutError.Reset();
		if (!IsSemanticObjectRoot(Object)) {
			auto Failure = Contract::FDiagnostic::Failure(TEXT("semantic_hjson_root_not_object"),
				TEXT("A semantic HJSON document must have an object root."), TEXT("hjson_write"));
			Failure.ExpectedValue = MakeShared<FJsonValueString>(TEXT("object"));
			OutError.SetFailure(MoveTemp(Failure), SourceUnit);
			return false;
		}
		FString Text;
		Contract::FDiagnostic Error;
		FUeAgentStructuredHjsonWriteOptions Options;
		Options.StableCanonicalLeafProjection = &TrySemanticLeafProjection;
		Options.CanonicalFragmentContext = SourceUnit;
		TMap<FString, FIntPoint> CharacterSpans;
		if (OutByteSpans) {
			OutByteSpans->Reset(); Options.WrittenValueCharacterSpans = &CharacterSpans;
			Options.WrittenValueArrayField = RowArrayField;
		}
		if (!FUeAgentStructuredHjsonFormatter::TryWriteValue(Object, Options, Text, Error)) {
			OutError.SetFailure(MoveTemp(Error), SourceUnit); return false;
		}
		Text.AppendChar(TEXT('\n'));
		EncodeSerializedText(Text, OutCanonicalUtf8);
		if (OutByteSpans) {
			TArray<TPair<FString, FIntPoint>> Ordered;
			for (const auto& Pair : CharacterSpans) Ordered.Add(Pair);
			Ordered.Sort([](const auto& A, const auto& B) { return A.Value.X < B.Value.X; });
			int32 Character = 0, Byte = 0;
			for (const auto& Pair : Ordered) {
				const auto Span = Pair.Value;
				const FTCHARToUTF8 Prefix(*Text + Character, Span.X - Character);
				Byte += Prefix.Length(); const int32 Begin = Byte;
				const FTCHARToUTF8 Body(*Text + Span.X, Span.Y - Span.X);
				Byte += Body.Length(); Character = Span.Y;
				OutByteSpans->Add(Pair.Key, FIntPoint(Begin, Byte));
			}
		}
		return true;
	}

	bool TrySerializeSemanticHjsonObject(const FUeAgentCanonicalValue& Object,
		TArray<uint8>& OutCanonicalUtf8, FWorkspaceError& OutError, const FString& SourceUnit)
	{
		return SerializeSemanticHjsonObject(Object, OutCanonicalUtf8, OutError, SourceUnit, FString(), nullptr);
	}

	bool TrySerializeSemanticHjsonObject(const FUeAgentStructuredHjsonObjectView& Object,
		TArray<uint8>& OutCanonicalUtf8, FWorkspaceError& OutError, const FString& SourceUnit)
	{
		return SerializeSemanticHjsonObject(Object, OutCanonicalUtf8, OutError, SourceUnit, FString(), nullptr);
	}

	bool TrySerializeSemanticHjsonObject(const FUeAgentCanonicalValue& Object,
		TArray<uint8>& OutCanonicalUtf8, FWorkspaceError& OutError, const FString& SourceUnit,
		const FString& RowArrayField, TMap<FString, FIntPoint>& OutByteSpans)
	{
		return SerializeSemanticHjsonObject(Object, OutCanonicalUtf8, OutError, SourceUnit, RowArrayField, &OutByteSpans);
	}

	bool TrySerializeSemanticHjsonObject(
		const TSharedRef<FJsonObject>& Object,
		TArray<uint8>& OutCanonicalUtf8, FWorkspaceError& OutError, const FString& SourceUnit)
	{
		OutCanonicalUtf8.Reset();
		OutError.Reset();
		FString Text;
		Contract::FDiagnostic Error;
		const TSharedRef<FJsonValue> RootValue = MakeShared<FJsonValueObject>(Object);
		if (!FUeAgentStructuredHjsonFormatter::TryWriteValue(RootValue, Text, Error)) {
			OutError.SetFailure(MoveTemp(Error), SourceUnit); return false;
		}
		Text.AppendChar(TEXT('\n'));
		EncodeSerializedText(Text, OutCanonicalUtf8);
		return true;
	}

	static bool TryWriteSemanticUnitRowText(const FSemanticUnit& Unit,
		FString& OutText, FWorkspaceError& OutError, const FString& SourceFile)
	{
		OutText.Reset();
		OutError.Reset();
		if (!Unit.IsValid())
		{
			OutError.SetFailure(Unit.GetValidationDiagnostic(), SourceFile);
			return false;
		}
		FUeAgentStructuredHjsonWriteOptions Options;
		Options.StableCanonicalLeafProjection = &TrySemanticLeafProjection;
		Options.bRequireJsonCompatibleObjectKeys = true;
		const FSemanticUnitRowView Row(Unit);
		Contract::FDiagnostic Error;
		if (!FUeAgentStructuredHjsonFormatter::TryWriteValue(Row.Object, Options, OutText, Error)) {
			// A failed current writer is not permission to select a different
			// representation and hide that failure behind a successful row proof.
			OutError.SetFailure(MoveTemp(Error), SourceFile); return false;
		}
		OutText.AppendChar(TEXT('\n'));
		return true;
	}

	bool TrySerializeSemanticUnitRow(const FSemanticUnit& Unit,
		TArray<uint8>& OutCanonicalUtf8, FWorkspaceError& OutError, const FString& SourceFile)
	{
		OutCanonicalUtf8.Reset();
		FString Text;
		if (!TryWriteSemanticUnitRowText(Unit, Text, OutError, SourceFile)) return false;
		EncodeSerializedText(Text, OutCanonicalUtf8);
		return true;
	}

	static bool TryEnsureSemanticUnitRowHash(const FSemanticUnit& Unit, FWorkspaceError& OutError, const FString& SourceFile)
	{
		if (!Unit.GetPublicRowHash().IsEmpty()) return true;
		if (!Unit.IsValid()) { OutError.SetFailure(Unit.GetValidationDiagnostic(), SourceFile); return false; }
		auto* Work = ActiveRowDigestWork;
		const double PrepareStarted = Work ? FPlatformTime::Seconds() : 0;
		FUeAgentStructuredHjsonWriteOptions Options;
		Options.StableCanonicalLeafProjection = &TrySemanticLeafProjection;
		Options.bRequireJsonCompatibleObjectKeys = true;
		const FSemanticUnitRowView Row(Unit);
		if (Work) Work->Prepare += FPlatformTime::Seconds() - PrepareStarted;
		const double WriterStarted = Work ? FPlatformTime::Seconds() : 0, EncodingBefore = Work ? Work->EncodingDigest : 0;
		SHA256_CTX Context; SHA256_Init(&Context);
		bool bTerminated = false;
		FString PendingText;
		const auto Flush = [&]() {
			if (PendingText.IsEmpty()) return;
			const double EncodingStarted = Work ? FPlatformTime::Seconds() : 0;
			const FTCHARToUTF8 Utf8(*PendingText, PendingText.Len());
			SHA256_Update(&Context, reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
			if (Work) { Work->Utf8Bytes += Utf8.Length(); Work->EncodingDigest += FPlatformTime::Seconds() - EncodingStarted; }
			PendingText.Reset();
		};
		const auto Consume = [&](FStringView Text) {
			if (Work) ++Work->Tokens;
			if (bTerminated) return;
			int32 Length = 0; while (Length < Text.Len() && Text[Length] != TCHAR(0)) ++Length;
			PendingText.AppendChars(Text.GetData(), Length);
			if (PendingText.Len() >= 4096) Flush();
			bTerminated = Length != Text.Len();
		};
		Contract::FDiagnostic Error;
		if (!FUeAgentStructuredHjsonFormatter::TryVisitValueText(Row.Object, Options, Consume, Error)) {
			OutError.SetFailure(MoveTemp(Error), SourceFile); return false;
		}
		Consume(FStringView(TEXT("\n")));
		Flush();
		if (Work) { Work->Writer += FPlatformTime::Seconds() - WriterStarted - (Work->EncodingDigest - EncodingBefore); ++Work->Rows; }
		const double FinishStarted = Work ? FPlatformTime::Seconds() : 0;
		uint8 Digest[SHA256_DIGEST_LENGTH]; SHA256_Final(Digest, &Context);
		FSemanticUnitContentAccess::SealRow(Unit, FormatSha256Digest(Digest));
		if (Work) Work->EncodingDigest += FPlatformTime::Seconds() - FinishStarted;
		return true;
	}
	bool TryPrepareSemanticUnitRowHash(const FSemanticUnit& Unit, FWorkspaceError& OutError, const FString& SourceFile)
	{
		OutError.Reset();
		return TryEnsureSemanticUnitRowHash(Unit, OutError, SourceFile);
	}
	bool TryHashSemanticUnitRow(const FSemanticUnit& Unit, FString& OutHash, FWorkspaceError& OutError, const FString& SourceFile)
	{
		OutHash.Reset(); OutError.Reset();
		if (!TryEnsureSemanticUnitRowHash(Unit, OutError, SourceFile)) return false;
		OutHash = Unit.GetPublicRowHash();
		return true;
	}

	template<typename RefMap> static bool TryBuildSemanticModelUnitRowImpl(const FSemanticUnit& Unit,
		TSharedPtr<FJsonObject>& OutRow, FWorkspaceError& OutError, const RefMap* PublicRefs,
		const FString& SourceFile)
	{
		OutRow.Reset();
		OutError.Reset();
		if (!Unit.IsValid())
		{
			OutError.SetFailure(Unit.GetValidationDiagnostic(), SourceFile);
			return false;
		}
		OutRow = SemanticModelUnitJson(Unit, OutError, PublicRefs);
		if (!OutRow.IsValid()) OutError.SetFailure(OutError.GetDiagnostic(), SourceFile);
		return OutRow.IsValid();
	}
	bool TryBuildSemanticModelUnitRow(const FSemanticUnit& Unit,
		TSharedPtr<FJsonObject>& OutRow, FWorkspaceError& OutError, const TMap<FString, FString>* PublicRefs,
		const FString& SourceFile)
	{
		return TryBuildSemanticModelUnitRowImpl(Unit, OutRow, OutError, PublicRefs, SourceFile);
	}
	bool TryBuildSemanticModelUnitRow(const FSemanticUnit& Unit,
		TSharedPtr<FJsonObject>& OutRow, FWorkspaceError& OutError, const FWorkspacePlacementView& PublicRefs,
		const FString& SourceFile)
	{
		return TryBuildSemanticModelUnitRowImpl(Unit, OutRow, OutError, &PublicRefs, SourceFile);
	}

	bool TrySerializeSemanticModelUnitRow(const FSemanticUnit& Unit,
		TArray<uint8>& OutCanonicalUtf8, FWorkspaceError& OutError, const FString& SourceFile)
	{
		OutCanonicalUtf8.Reset();
		TSharedPtr<FJsonObject> Row;
		return TryBuildSemanticModelUnitRow(Unit, Row, OutError, nullptr, SourceFile)
			&& TrySerializeSemanticHjsonObject(Row.ToSharedRef(), OutCanonicalUtf8, OutError, SourceFile);
	}

	bool TrySerializeSemanticModelUnitRow(const FSemanticUnit& Unit,
		const TMap<FString, FString>& PublicRefs,
		TArray<uint8>& OutCanonicalUtf8, FWorkspaceError& OutError, const FString& SourceFile)
	{
		OutCanonicalUtf8.Reset();
		TSharedPtr<FJsonObject> Row;
		return TryBuildSemanticModelUnitRow(Unit, Row, OutError, &PublicRefs, SourceFile)
			&& TrySerializeSemanticHjsonObject(Row.ToSharedRef(), OutCanonicalUtf8, OutError, SourceFile);
	}

	bool TrySerializeSemanticUnitDocument(const FSemanticUnit& Unit,
		TArray<uint8>& OutCanonicalUtf8, FWorkspaceError& OutError, const FString& SourceFile)
	{
		OutCanonicalUtf8.Reset();
		TSharedPtr<FJsonObject> Row;
		if (!TryBuildSemanticModelUnitRow(Unit, Row, OutError, nullptr, SourceFile)) return false;
		const TSharedRef<FJsonObject> Wrapper = MakeShared<FJsonObject>();
		Wrapper->SetObjectField(TEXT("unit"), Row);
		return TrySerializeSemanticHjsonObject(Wrapper, OutCanonicalUtf8, OutError, SourceFile);
	}
	bool TrySerializeSemanticModelUnitRow(const FSemanticUnit& Unit,
		const FWorkspacePlacementView& PublicRefs,
		TArray<uint8>& OutCanonicalUtf8, FWorkspaceError& OutError, const FString& SourceFile)
	{
		OutCanonicalUtf8.Reset();
		TSharedPtr<FJsonObject> Row;
		return TryBuildSemanticModelUnitRow(Unit, Row, OutError, PublicRefs, SourceFile)
			&& TrySerializeSemanticHjsonObject(Row.ToSharedRef(), OutCanonicalUtf8, OutError, SourceFile);
	}
	bool TrySerializeSemanticUnitDocument(const FSemanticUnit& Unit,
		const FWorkspacePlacementView& PublicRefs,
		TArray<uint8>& OutCanonicalUtf8, FWorkspaceError& OutError, const FString& SourceFile)
	{
		OutCanonicalUtf8.Reset();
		TSharedPtr<FJsonObject> Row;
		if (!TryBuildSemanticModelUnitRow(Unit, Row, OutError, PublicRefs, SourceFile)) return false;
		const TSharedRef<FJsonObject> Wrapper = MakeShared<FJsonObject>();
		Wrapper->SetObjectField(TEXT("unit"), Row);
		return TrySerializeSemanticHjsonObject(Wrapper, OutCanonicalUtf8, OutError, SourceFile);
	}

	bool TrySerializeSemanticUnitDocument(const FSemanticUnit& Unit,
		const TMap<FString, FString>& PublicRefs,
		TArray<uint8>& OutCanonicalUtf8, FWorkspaceError& OutError, const FString& SourceFile)
	{
		OutCanonicalUtf8.Reset();
		TSharedPtr<FJsonObject> Row;
		if (!TryBuildSemanticModelUnitRow(Unit, Row, OutError, &PublicRefs, SourceFile)) return false;
		const TSharedRef<FJsonObject> Wrapper = MakeShared<FJsonObject>();
		Wrapper->SetObjectField(TEXT("unit"), Row);
		return TrySerializeSemanticHjsonObject(Wrapper, OutCanonicalUtf8, OutError, SourceFile);
	}

	void ResetCompiledAuthoringReuse()
	{
		PreviousCompiledModel.Reset();
		CompiledMaterialBlocks.Empty();
		PreviousLayout.Reset();
		PreviousLayoutCharges.Empty();
		UnitStringCandidates.Empty(); UnitStringAdmissionMisses = 0;
		UnitContainerCandidates.Reset();
		UE::UAI::StateSemantic::ResetCanonicalValueReuse();
	}

	bool TryCompileAuthoringModel(
		FString ContractId,
		int32 ContractVersion,
		FString ProfileId,
		FString BaseRevision,
		TArray<FSemanticUnit> Units,
		TArray<FString> TargetUnitIds,
		FUeAgentResourceTracker& Resources,
		FAuthoringModelPtr& OutModel,
		FWorkspaceError& OutError)
	{
		return TryCompileAuthoringModel(MoveTemp(ContractId), ContractVersion, MoveTemp(ProfileId),
			MoveTemp(BaseRevision), MoveTemp(Units), MoveTemp(TargetUnitIds), Resources, OutModel, OutError, nullptr);
	}

	bool TryCompileAuthoringModel(
		FString ContractId, int32 ContractVersion, FString ProfileId, FString BaseRevision,
		TArray<FSemanticUnit> Units, TArray<FString> TargetUnitIds,
		FUeAgentResourceTracker& Resources, FAuthoringModelPtr& OutModel,
		FWorkspaceError& OutError, const FDefinitionProjectionHints* ProjectionHints)
	{
		return TryCompileAuthoringModel(MoveTemp(ContractId), ContractVersion, MoveTemp(ProfileId),
			MoveTemp(BaseRevision), MoveTemp(Units), MoveTemp(TargetUnitIds), Resources, OutModel, OutError,
			ProjectionHints, nullptr, nullptr);
	}

	bool TryCompileAuthoringModelRaw(
		FString ContractId, int32 ContractVersion, FString ProfileId, FString BaseRevision,
		TArray<FSemanticUnit> Units, TArray<FString> TargetUnitIds,
		FUeAgentResourceTracker& Resources, FAuthoringModelPtr& OutModel,
		FWorkspaceError& OutError, const FDefinitionProjectionHints* ProjectionHints,
		const FAuthoringModel* ModelCandidate, const FCompiledAuthoringReuseProof* StructureCandidate, bool bBuildLegacyFingerprint)
	{
		OutModel.Reset();
		OutError.Reset();
		ContractId = Normalize(MoveTemp(ContractId));
		ProfileId = Normalize(MoveTemp(ProfileId));
		BaseRevision = Normalize(MoveTemp(BaseRevision));
		if (ContractId.IsEmpty()
			|| ContractVersion < 1
			|| ProfileId.IsEmpty()
			|| BaseRevision.IsEmpty()
			|| TargetUnitIds.IsEmpty())
		{
			const FString Field = ContractId.IsEmpty() ? TEXT("contract_id") : ContractVersion < 1 ? TEXT("contract_version")
				: ProfileId.IsEmpty() ? TEXT("profile_id") : BaseRevision.IsEmpty() ? TEXT("base_revision") : TEXT("target_unit_ids");
			auto Failure = Contract::FDiagnostic::Failure(TEXT("invalid_authoring_model"),
				Field == TEXT("contract_version") ? TEXT("The authoring model contract version must be positive.") : TEXT("The authoring model is missing a required field."), TEXT("semantic_model"));
			Failure.SemanticPointer = TEXT("/") + Field;
			if (Field == TEXT("contract_version")) Failure.ActualValue = MakeShared<FJsonValueNumber>(ContractVersion);
			else Failure.ActualValuePresent = false;
			OutError.SetFailure(MoveTemp(Failure));
			return false;
		}
		Contract::FDiagnostic TargetError;
		if (!NormalizeSet(TargetUnitIds, TargetError, TEXT("invalid_target_unit_ids"), TEXT("target_unit_ids")))
		{
			OutError.SetFailure(MoveTemp(TargetError));
			return false;
		}
		const double StructureStarted = FPlatformTime::Seconds();
		bool bOrdered = true;
		for (int32 Index = 1; Index < Units.Num(); ++Index) {
			if (Units[Index].GetUnitId() < Units[Index - 1].GetUnitId()) { bOrdered = false; break; }
		}
		if (!bOrdered) Units.Sort(
			[](const FSemanticUnit& Left, const FSemanticUnit& Right)
			{
				return Left.GetUnitId() < Right.GetUnitId();
			});
		// Consumers may hand back this compiler's successful immutable proof.
		// Otherwise retain the existing weak last-model fallback; neither path
		// retains old value graphs. Headers and fingerprinting remain current.
		const auto LastModel = PreviousCompiledModel.Pin();
		const FAuthoringModel* Previous = ModelCandidate ? ModelCandidate : LastModel.Get();
		TSharedPtr<const FCompiledAuthoringStructure> CompiledStructure;
		int32 ProvenDeclarations = 0;
		if (StructureCandidate && StructureCandidate->TargetUnitIds == TargetUnitIds
			&& StructureCandidate->Declarations.Num() == Units.Num()) {
			CompiledStructure = StructureCandidate->Structure.Pin();
			for (int32 Index = 0; CompiledStructure && Index < Units.Num(); ++Index) {
				if (!Units[Index].MatchesCompiledDeclaration(StructureCandidate->Declarations[Index])) CompiledStructure.Reset();
				else ++ProvenDeclarations;
			}
		}
		const bool bOwnerProofReused = CompiledStructure.IsValid();
		bool bSameStructure = Previous && Previous->IsCompiled() && Previous->TargetUnitIds == TargetUnitIds
			&& Previous->Units.Num() == Units.Num();
		for (int32 Index = 0; !CompiledStructure && bSameStructure && Index < Units.Num(); ++Index) {
			const auto& Current = Units[Index];
			const auto& Before = Previous->Units[Index];
			if (Current.SharesStorageWith(Before)) continue;
			// These are exactly the inputs of the indexes, target-kind check,
			// VisitUnitReferences and owner traversal. A changed value must still
			// have passed its own immutable unit validation.
			bSameStructure = Current.MatchesCompiledDeclaration(Before.GetDeclarationProof());
		}
		if (!CompiledStructure && bSameStructure) CompiledStructure = Previous->Structure;
		bSameStructure = CompiledStructure.IsValid();
		if (!CompiledStructure) {
		const auto NewStructure = MakeShared<FCompiledAuthoringStructure>();
		auto& UnitIndex = NewStructure->UnitIndex;
		UnitIndex.Reserve(Units.Num());
		FWorkspaceError Diagnostics;
		const double IndexStarted = FPlatformTime::Seconds();
		TSet<FString> IndexedSemanticTypes;
		const auto* PreviousUnits = Previous && Previous->IsCompiled() ? &Previous->GetUnits() : nullptr;
		int32 PreviousPosition = 0;
		TBitArray<> ChangedOwnerEdges(true, Units.Num());
		for (int32 Index = 0; Index < Units.Num(); ++Index)
		{
			const FSemanticUnit& Unit = Units[Index];
			if (!Unit.IsValid())
			{
				OutError.SetFailure(Unit.GetValidationDiagnostic(), Unit.GetUnitId());
				Diagnostics.Append(OutError);
				continue;
			}
			const uint32 IdentityHash = GetTypeHash(Unit.GetUnitId());
			if (UnitIndex.ContainsByHash(IdentityHash, Unit.GetUnitId()))
			{
				Fail(OutError, TEXT("duplicate_semantic_unit"), TEXT("More than one semantic unit has the same unit identity."), Unit.GetUnitId());
				Diagnostics.Append(OutError);
				continue;
			}
			UnitIndex.AddByHash(IdentityHash, Unit.GetSharedUnitId(), Index);
			if (PreviousUnits) {
				while (PreviousPosition < PreviousUnits->Num()
					&& (*PreviousUnits)[PreviousPosition].GetUnitId() < Unit.GetUnitId()) ++PreviousPosition;
				if (PreviousPosition < PreviousUnits->Num()) {
					const auto& Before = (*PreviousUnits)[PreviousPosition];
					ChangedOwnerEdges[Index] = !(Before.GetUnitId().Equals(Unit.GetUnitId(), ESearchCase::CaseSensitive)
						&& Before.GetOwnerRef().Equals(Unit.GetOwnerRef(), ESearchCase::CaseSensitive));
				}
			}
			const FString& Type = Unit.GetSemanticTypeRef();
			if (!IndexedSemanticTypes.Contains(Type)) {
				IndexedSemanticTypes.Add(Type);
				NewStructure->AddSemanticType(Type);
			}
		}
		if (Diagnostics.HasErrors())
		{
			Diagnostics.MarkUnchecked(TEXT("model_references"), FString(), TEXT("invalid_unit_identities"));
			OutError = MoveTemp(Diagnostics);
			return false;
		}
		for (const FString& TargetId : TargetUnitIds)
		{
			const int32* Index = UnitIndex.Find(TargetId);
			if (Index == nullptr)
			{
				Fail(OutError, TEXT("missing_target_unit"), TEXT("A requested target unit is absent from the authoring model."), TargetId);
				Diagnostics.Append(OutError);
				continue;
			}
			if (Units[*Index].GetUnitKind()
				!= ESemanticUnitKind::TargetState)
			{
				Fail(OutError, TEXT("invalid_target_unit_kind"), TEXT("A requested target unit must have the TargetState kind."), TargetId);
				Diagnostics.Append(OutError);
				continue;
			}
		}

		const double IndexFinished = FPlatformTime::Seconds();
		int64& ReferenceCount = NewStructure->ReferenceCount;
		for (const FSemanticUnit& Unit : Units)
		{
			VisitUnitReferences(Unit, [&](const FString& Reference) {
				++ReferenceCount;
				if (!UnitIndex.Contains(Reference))
				{
					Fail(OutError, TEXT("unresolved_semantic_unit_reference"), TEXT("The semantic unit refers to a unit that is absent from the authoring model."), Unit.GetUnitId());
				Diagnostics.Append(OutError);
				}
			});
		}

		const double ReferencesFinished = FPlatformTime::Seconds();
		// These are positions in the already validated current UnitIndex. Only
		// an actual cycle diagnostic consumes identity text; successful traversal
		// does not need a second copy of every unit ID and every owner path.
		TArray<int32> OwnerPositions;
		OwnerPositions.Init(INDEX_NONE, Units.Num());
		TBitArray<> CompleteOwners(false, Units.Num());
		TArray<int32> OwnerPath;
		int32 OwnerTraversalStarts = 0;
		for (int32 StartIndex = 0; StartIndex < Units.Num(); ++StartIndex)
		{
			// Removing vertices cannot create a cycle. In the previously valid
			// graph every new cycle must include a newly added/changed owner edge.
			// Follow the complete current path from each such edge, including old
			// ancestors; do not stop merely because an ancestor was unchanged.
			if (!ChangedOwnerEdges[StartIndex] || CompleteOwners[StartIndex]) continue;
			++OwnerTraversalStarts;
			OwnerPath.Reset();
			int32 CurrentIndex = StartIndex;
			while (CurrentIndex != INDEX_NONE)
			{
				if (CompleteOwners[CurrentIndex]) break;
				if (OwnerPositions[CurrentIndex] != INDEX_NONE)
				{
					TArray<FString> Cycle;
					for (int32 Index = OwnerPositions[CurrentIndex]; Index < OwnerPath.Num(); ++Index)
						Cycle.Add(Units[OwnerPath[Index]].GetUnitId());
					Cycle.Add(Units[CurrentIndex].GetUnitId());
					Fail(OutError, TEXT("semantic_owner_cycle"), TEXT("The semantic unit owner references contain a dependency cycle."), Units[StartIndex].GetUnitId(), MoveTemp(Cycle));
					Diagnostics.Append(OutError);
					break;
				}
				OwnerPositions[CurrentIndex] = OwnerPath.Num();
				OwnerPath.Add(CurrentIndex);
				const auto& Current = Units[CurrentIndex];
				if (Current.GetOwnerRef().IsEmpty()) break;
				const int32* OwnerIndex = UnitIndex.Find(Current.GetOwnerRef());
				if (!OwnerIndex) break; // Unresolved reference already reported above.
				CurrentIndex = *OwnerIndex;
			}
			for (const int32 Index : OwnerPath) {
				CompleteOwners[Index] = true;
				OwnerPositions[Index] = INDEX_NONE;
			}
		}
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=compiler_structure units=%d index_s=%.6f references_s=%.6f owners_s=%.6f temporary_owner_ids=false distinct_types=%d identity_strings=original_declaration owner_starts=%d previous_graph=%s"),
			Units.Num(), IndexFinished - IndexStarted, ReferencesFinished - IndexFinished,
			FPlatformTime::Seconds() - ReferencesFinished, IndexedSemanticTypes.Num(), OwnerTraversalStarts,
			PreviousUnits ? TEXT("true") : TEXT("false"));

		if (Diagnostics.HasErrors())
		{
			Diagnostics.MarkUnchecked(TEXT("domain_preflight"), FString(), TEXT("model_references_invalid"));
			OutError = MoveTemp(Diagnostics);
			return false;
		}
		NewStructure->SemanticTypePrefixBytes = NewStructure->SemanticTypePrefixes.GetAllocatedSize();
		for (const FString& Prefix : NewStructure->SemanticTypePrefixes)
			NewStructure->SemanticTypePrefixBytes += Prefix.GetAllocatedSize();
		CompiledStructure = NewStructure;
		}
		const int64 ReferenceCount = CompiledStructure->ReferenceCount;
		const double StructureFinished = FPlatformTime::Seconds();
		if (!bBuildLegacyFingerprint) {
			OutModel = MakeShareable(new FAuthoringModel(MoveTemp(ContractId), ContractVersion,
				MoveTemp(ProfileId), MoveTemp(BaseRevision), MoveTemp(Units), MoveTemp(TargetUnitIds),
				FString(), CompiledStructure.ToSharedRef(), ProjectionHints));
			if (!OutModel->DefinitionProjectionCollision.IsEmpty()) {
				const FString UnitId = OutModel->DefinitionProjectionCollision; OutModel.Reset();
				return Fail(OutError, TEXT("semantic_definition_alias_collision"),
					TEXT("A stable definition name conflicts with another bound identity; no existing definition was renamed."), UnitId);
			}
			PreviousCompiledModel = OutModel;
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=model_compile units=%d graph_reused=%s structure_s=%.6f legacy_fingerprint=false material_bytes=0"),
				OutModel->GetUnitCount(), bSameStructure ? TEXT("true") : TEXT("false"), StructureFinished - StructureStarted);
			return true;
		}

		FUtf8Material Material;
		Material.Reserve(64 * 1024);
		AppendToken(Material, TEXT("s"), ContractId);
		Material.AppendInt(ContractVersion);
		AppendToken(Material, TEXT("s"), ProfileId);
		AppendToken(Material, TEXT("s"), BaseRevision);
		AppendStringArray(Material, TargetUnitIds);
		SHA256_CTX HashContext;
		SHA256_Init(&HashContext);
		int64 MaterialBytes = 0;
		int64 ItemCount = 0, RetainedMaterialBytes = 0;
		TMultiMap<uint32, const void*> MaterialCandidates;
		for (auto It = CompiledMaterialBlocks.CreateIterator(); It; ++It)
		{
			bool bLive = false;
			for (const auto& Identity : It.Value()->Units) if (Identity.IsValid()) { bLive = true; break; }
			if (!bLive) It.RemoveCurrent();
			else {
				RetainedMaterialBytes += It.Value()->Charge();
				MaterialCandidates.Add(GetTypeHash(It.Value()->FirstUnitId), It.Key());
			}
		}
		RetainedMaterialBytes += MaterialCandidates.GetAllocatedSize();
		for (auto It = CompiledMaterialBlocks.CreateIterator(); It && RetainedMaterialBytes > MaterialProofBudget; ++It) {
			RetainedMaterialBytes -= It.Value()->Charge();
			It.RemoveCurrent();
		}
		int32 ReusedMaterialBlocks = 0, ReusedMaterialUnits = 0, PartiallyReusedMaterialUnits = 0;
		TMap<const void*, TSharedPtr<const FCompiledMaterialBlock>> PendingMaterialBlocks;
		TSharedPtr<FCompiledMaterialBlock> BuildingMaterialBlock;
		int64 BuildingReplacementCharge = 0;
		double MaterialBuildSeconds = 0, MaterialHashSeconds = 0, MaterialAdmissionSeconds = 0;
		double ContentSealSeconds = 0;
		int32 ContentSealedUnits = 0;
		int32 AdmittedMaterialBlocks = 0;
		const auto FlushMaterial = [&]() {
			const double HashStarted = FPlatformTime::Seconds();
			SHA256_Update(&HashContext, Material.Bytes.GetData(), Material.Bytes.Num());
			MaterialHashSeconds += FPlatformTime::Seconds() - HashStarted;
			MaterialBytes += Material.Bytes.Num();
			if (BuildingMaterialBlock)
			{
				if (RetainedMaterialBytes - BuildingReplacementCharge + BuildingMaterialBlock->Charge()
					+ Material.Bytes.Num() <= MaterialProofBudget)
					BuildingMaterialBlock->Bytes.Append(Material.Bytes);
				else BuildingMaterialBlock.Reset();
			}
			Material.Reset();
		};
		// Header stays current. Every unit ends with an ASCII token delimiter,
		// so these block boundaries preserve exactly the original UTF-8 stream.
		FlushMaterial();
		for (int32 First = 0; First < Units.Num(); First += 256)
		{
			const int32 Count = FMath::Min(256, Units.Num() - First);
			const void* BlockKey = Units[First].GetStorageIdentity().Pin().Get();
			const auto* Existing = CompiledMaterialBlocks.Find(BlockKey);
			TSharedPtr<const FCompiledMaterialBlock> Prior;
			const void* PriorKey = nullptr;
			int32 ReusableCount = 0;
			bool ReusableUnits[256] = {};
			TArray<const void*> Candidates;
			MaterialCandidates.MultiFind(GetTypeHash(Units[First].GetUnitId()), Candidates);
			for (const void* CandidateKey : Candidates) {
				const auto* Found = CompiledMaterialBlocks.Find(CandidateKey);
				if (!Found) continue;
				const auto& Candidate = **Found;
				if (Candidate.First != First || Candidate.Units.Num() != Count
					|| Candidate.UnitByteOffsets.Num() != Count + 1
					|| !Candidate.FirstUnitId.Equals(Units[First].GetUnitId(), ESearchCase::CaseSensitive)) continue;
				bool Matches[256] = {}; int32 MatchesCount = 0;
				for (int32 Offset = 0; Offset < Count; ++Offset) {
					Matches[Offset] = Units[First + Offset].SharesStorageIdentity(Candidate.Units[Offset]);
					MatchesCount += Matches[Offset] ? 1 : 0;
				}
				if (MatchesCount > ReusableCount) {
					Prior = *Found; PriorKey = CandidateKey; ReusableCount = MatchesCount;
					for (int32 Offset = 0; Offset < Count; ++Offset) ReusableUnits[Offset] = Matches[Offset];
				}
				if (ReusableCount == Count) break;
			}
			if (ReusableCount == Count)
			{
				const double HashStarted = FPlatformTime::Seconds();
				SHA256_Update(&HashContext, Prior->Bytes.GetData(), Prior->Bytes.Num());
				MaterialHashSeconds += FPlatformTime::Seconds() - HashStarted;
				MaterialBytes += Prior->Bytes.Num(); ItemCount += Prior->Items;
				++ReusedMaterialBlocks; ReusedMaterialUnits += Count;
				continue;
			}
			// A new first-unit storage can replace a partially live block. Retain
			// only its actual surviving unit fragments, never a whole-block proof
			// inferred from a hash or the unchanged majority of its neighbours.
			const int64 ReplacedMaterialCharge = (Prior ? Prior->Charge() : 0)
				+ (Existing && PriorKey != BlockKey ? (*Existing)->Charge() : 0);
			BuildingReplacementCharge = ReplacedMaterialCharge;
			if (RetainedMaterialBytes - ReplacedMaterialCharge < MaterialProofBudget
				&& (Prior || CompiledMaterialBlocks.Num() + PendingMaterialBlocks.Num() < 8192))
			{
				BuildingMaterialBlock = MakeShared<FCompiledMaterialBlock>();
				BuildingMaterialBlock->First = First;
				BuildingMaterialBlock->FirstUnitId = Units[First].GetUnitId();
				BuildingMaterialBlock->Units.Reserve(Count);
				BuildingMaterialBlock->UnitByteOffsets.Reserve(Count + 1);
				BuildingMaterialBlock->UnitByteOffsets.Add(0);
				for (int32 Offset = 0; Offset < Count; ++Offset)
					BuildingMaterialBlock->Units.Add(Units[First + Offset].GetStorageIdentity());
			}
			const double BlockStarted = FPlatformTime::Seconds();
			const double HashBeforeBlock = MaterialHashSeconds;
			int64 BlockItems = 0;
			for (int32 Offset = 0; Offset < Count; ++Offset)
			{
				const auto& Unit = Units[First + Offset];
				BlockItems += Unit.GetValueCount();
				const int32 UnitMaterialStart = Material.Bytes.Num();
				if (Prior && ReusableUnits[Offset]) {
					const int32 Begin = Prior->UnitByteOffsets[Offset], End = Prior->UnitByteOffsets[Offset + 1];
					Material.Bytes.Append(Prior->Bytes.GetData() + Begin, End - Begin);
					++PartiallyReusedMaterialUnits;
				} else AppendUnitMaterial(Material, Unit);
				ContentSealedUnits += FSemanticUnitContentAccess::Seal(Unit, Material.Bytes.GetData() + UnitMaterialStart,
					Material.Bytes.Num() - UnitMaterialStart, &ContentSealSeconds) ? 1 : 0;
				if (BuildingMaterialBlock)
					BuildingMaterialBlock->UnitByteOffsets.Add(BuildingMaterialBlock->Bytes.Num() + Material.Len());
				if (Material.Len() >= 64 * 1024) FlushMaterial();
			}
			FlushMaterial(); ItemCount += BlockItems;
			const double BlockBuildSeconds = FPlatformTime::Seconds() - BlockStarted - (MaterialHashSeconds - HashBeforeBlock);
			MaterialBuildSeconds += BlockBuildSeconds;
			if (BuildingMaterialBlock)
			{
				BuildingMaterialBlock->Items = BlockItems;
				const double AdmissionStarted = FPlatformTime::Seconds();
				const int64 Charge = BuildingMaterialBlock->Charge();
				if (RetainedMaterialBytes - ReplacedMaterialCharge + Charge <= MaterialProofBudget) {
					RetainedMaterialBytes += Charge - ReplacedMaterialCharge;
					// The new proof is installed only after successful full compile.
					// Dropping an old optimization before failure changes no authority.
					if (Prior) CompiledMaterialBlocks.Remove(PriorKey);
					if (Existing && PriorKey != BlockKey) CompiledMaterialBlocks.Remove(BlockKey);
					PendingMaterialBlocks.Add(BlockKey, BuildingMaterialBlock); ++AdmittedMaterialBlocks;
				}
				MaterialAdmissionSeconds += FPlatformTime::Seconds() - AdmissionStarted;
			}
			BuildingMaterialBlock.Reset();
		}
		FUeAgentResourceChargeError ResourceError;
		if (!Resources.TryChargeMany(
			{
				{EUeAgentResourceDimension::InputBytes,
					MaterialBytes},
				{EUeAgentResourceDimension::StringBytes,
					MaterialBytes},
				{EUeAgentResourceDimension::Nodes, Units.Num()},
				{EUeAgentResourceDimension::Items, ItemCount},
				{EUeAgentResourceDimension::ReferenceEdges,
					ReferenceCount},
				{EUeAgentResourceDimension::WorkUnits,
					Units.Num() + ReferenceCount},
				{EUeAgentResourceDimension::EstimatedMemoryBytes,
					MaterialBytes * 2 + CompiledStructure->SemanticTypePrefixBytes},
			},
			TEXT("semantic_layout.compile"),
			ResourceError))
		{
			return FailResource(OutError, ResourceError);
		}
		uint8 Digest[SHA256_DIGEST_LENGTH] = {};
		SHA256_Final(Digest, &HashContext);
		FString Fingerprint = TEXT("sha256:");
		for (uint8 Byte : Digest) Fingerprint.Appendf(TEXT("%02x"), Byte);
		OutModel = MakeShareable(new FAuthoringModel(
			MoveTemp(ContractId),
			ContractVersion,
			MoveTemp(ProfileId),
			MoveTemp(BaseRevision),
			MoveTemp(Units),
			MoveTemp(TargetUnitIds),
			MoveTemp(Fingerprint), CompiledStructure.ToSharedRef(), ProjectionHints));
		if (!OutModel->DefinitionProjectionCollision.IsEmpty()) {
			const FString UnitId = OutModel->DefinitionProjectionCollision;
			OutModel.Reset();
			return Fail(OutError, TEXT("semantic_definition_alias_collision"),
				TEXT("A stable definition name conflicts with another bound identity; no existing definition was renamed."), UnitId);
		}
		PreviousCompiledModel = OutModel;
		for (auto& Pending : PendingMaterialBlocks) CompiledMaterialBlocks.Add(Pending.Key, MoveTemp(Pending.Value));
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=compiler_material_blocks reused_blocks=%d reused_units=%d partial_reused_units=%d material_bytes=%lld retained_bytes=%lld admitted_blocks=%d build_s=%.6f hash_s=%.6f admission_s=%.6f content_sealed_units=%d content_seal_s=%.6f"),
			ReusedMaterialBlocks, ReusedMaterialUnits, PartiallyReusedMaterialUnits, MaterialBytes, RetainedMaterialBytes, AdmittedMaterialBlocks,
			MaterialBuildSeconds, MaterialHashSeconds, MaterialAdmissionSeconds, ContentSealedUnits, ContentSealSeconds);
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=model_compile units=%d graph_reused=%s structure_s=%.6f fingerprint_and_model_s=%.6f owner_proof_reused=%s proven_declarations=%d explicit_model_candidate=%s"),
			OutModel->GetUnits().Num(), bSameStructure ? TEXT("true") : TEXT("false"),
			StructureFinished - StructureStarted, FPlatformTime::Seconds() - StructureFinished,
			bOwnerProofReused ? TEXT("true") : TEXT("false"), ProvenDeclarations, ModelCandidate ? TEXT("true") : TEXT("false"));
		return true;
	}

	bool TryNormalizeAuthoringUnit(FSemanticUnit& Unit, FUeAgentResourceTracker& Resources, FWorkspaceError& OutError)
	{
		if (Unit.GetUnitKind() != ESemanticUnitKind::CollectionState
			|| !Unit.GetRole().Equals(TEXT("property_definition"), ESearchCase::CaseSensitive)
			|| !Unit.GetSemanticTypeRef().Equals(TEXT("uai.authoring.property_definition/1"), ESearchCase::CaseSensitive)) return true;
		TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields;
		for (const auto& Field : Unit.GetValues()) Fields.Emplace(Field.Key, Field.Value);
		FUeAgentCanonicalValuePtr Declaration, Normalized;
		FUeAgentStateSemanticError Error;
		if (!FUeAgentCanonicalValue::TryObject(Fields, Resources, Declaration, Error)
			|| !TryNormalizePropertyDefinition(Declaration.ToSharedRef(), Resources, Normalized, Error))
		{ OutError.SetFailure(MoveTemp(Error)); return false; }
		if (Declaration != Normalized) {
			TMap<FString, FUeAgentCanonicalValueRef> Values;
			for (const auto& Field : Normalized->GetObjectItems()) Values.Add(Field.Key, Field.Value);
			Unit = Unit.WithValues(MoveTemp(Values));
		}
		return true;
	}

	bool TryCompileAuthoringSourceModel(
		FString ContractId, int32 ContractVersion, FString ProfileId, FString BaseRevision,
		TArray<FSemanticUnit> Units, TArray<FString> TargetUnitIds,
		FUeAgentResourceTracker& Resources, FAuthoringModelPtr& OutModel,
		FWorkspaceError& OutError, const FDefinitionProjectionHints* ProjectionHints,
		const FAuthoringModel* ModelCandidate, const FCompiledAuthoringReuseProof* StructureCandidate,
		const FString* ExpectedSourceFingerprint, bool bBuildLegacyFingerprint)
	{
		// Only source admission requests the old raw fingerprint. Keep that
		// proof separate from the schema-normalized model used by all consumers.
		FAuthoringModelPtr RawSourceModel;
		if (ExpectedSourceFingerprint) {
			if (!TryCompileAuthoringModelRaw(ContractId, ContractVersion, ProfileId, BaseRevision,
				Units, TargetUnitIds, Resources, RawSourceModel, OutError, ProjectionHints, ModelCandidate, StructureCandidate, true)) return false;
			if (!RawSourceModel->GetSemanticFingerprint().Equals(*ExpectedSourceFingerprint, ESearchCase::CaseSensitive)) {
				OutError.SetSourceFailure(TEXT("semantic_workspace_fingerprint_mismatch"),
					TEXT("The semantic workspace content differs from its recorded fingerprint."), FString(), TEXT("/semantic_fingerprint"));
				return false;
			}
		}
		bool bChanged = false;
		for (auto& Unit : Units) {
			const auto Identity = Unit.GetStorageIdentity();
			if (!TryNormalizeAuthoringUnit(Unit, Resources, OutError)) return false;
			bChanged |= !Unit.SharesStorageIdentity(Identity);
		}
		if (RawSourceModel && !bChanged) { OutModel = MoveTemp(RawSourceModel); return true; }
		return TryCompileAuthoringModelRaw(MoveTemp(ContractId), ContractVersion, MoveTemp(ProfileId), MoveTemp(BaseRevision),
			MoveTemp(Units), MoveTemp(TargetUnitIds), Resources, OutModel, OutError, ProjectionHints,
			RawSourceModel ? RawSourceModel.Get() : ModelCandidate, StructureCandidate, bBuildLegacyFingerprint);
	}

	bool TryCompileAuthoringModel(
		FString ContractId, int32 ContractVersion, FString ProfileId, FString BaseRevision,
		TArray<FSemanticUnit> Units, TArray<FString> TargetUnitIds,
		FUeAgentResourceTracker& Resources, FAuthoringModelPtr& OutModel,
		FWorkspaceError& OutError, const FDefinitionProjectionHints* ProjectionHints,
		const FAuthoringModel* ModelCandidate, const FCompiledAuthoringReuseProof* StructureCandidate, bool bBuildLegacyFingerprint)
	{
		return TryCompileAuthoringSourceModel(MoveTemp(ContractId), ContractVersion, MoveTemp(ProfileId), MoveTemp(BaseRevision),
			MoveTemp(Units), MoveTemp(TargetUnitIds), Resources, OutModel, OutError, ProjectionHints, ModelCandidate, StructureCandidate, nullptr, bBuildLegacyFingerprint);
	}

	static bool BuildReferenceMembershipDelta(const FAuthoringModel& Model, const FWorkspaceLayoutPlan& Previous,
		const TBitArray<>& Preserved, FSemanticReferenceProjection& Out, FWorkspaceError& Error,
		const FCommittedWorkspaceDelta* AppliedChanges)
	{
		const auto& Prior = *Previous.GetSharedProjection()->ReferenceNamespace;
		auto Next = MakeShared<FSemanticReferenceNamespace>(); Next->AliasesByScope = Prior.AliasesByScope;
		Next->bAliasesFromPublishedRows = Prior.bAliasesFromPublishedRows;
		Next->Previous = Previous.GetSharedProjection()->ReferenceNamespace;
		auto Global = MakeShared<TSet<FString>>(*Prior.Handles);
		TMap<FString, TSharedPtr<TSet<FString>>> ChangedScopes;
		bool bAliasReadFailed = false;
		auto ScopeNames = [&](const FString& Scope) -> TSet<FString>& {
			if (const auto* Known = ChangedScopes.Find(Scope)) return **Known;
			const auto* Before = Prior.AliasesByScope.Find(Scope);
			auto Names = Before ? MakeShared<TSet<FString>>(**Before) : MakeShared<TSet<FString>>();
			if (!Before && Prior.bAliasesFromPublishedRows) {
				Previous.GetModel()->VisitDeclarations([&](const FSemanticUnit::FDeclarationView& Declaration, int32) {
					if (SemanticAliasScope(Declaration) != Scope) return true;
					const auto* Unit = Previous.GetModel()->FindUnit(Declaration.GetUnitId());
					const auto* Ref = Previous.GetMachineManifest().PublicRefMapping.Find(Declaration.GetUnitId());
					FString Alias;
					if (!Unit || !Ref || !ReadBoundProjectionAlias(*Unit, *Ref, Alias)) {
						bAliasReadFailed = true;
						if (!Previous.GetModel()->CheckValueReadResult(Error)) return false;
						return Fail(Error, TEXT("semantic_publication_alias_unavailable"),
							TEXT("The affected naming scope has an invalid captured alias."), Declaration.GetUnitId());
					}
					Names->Add(Alias.ToLower());
					int32 Colon = INDEX_NONE; Ref->FindChar(TEXT(':'), Colon);
					if (Ref->Mid(Colon + 1) != Alias) Next->AliasExceptions.Add(Declaration.GetUnitId(), MoveTemp(Alias));
					return true;
				});
			}
			Next->AliasesByScope.Add(Scope, Names); ChangedScopes.Add(Scope, Names); return *Names;
		};
		const auto& Units = Model.GetDeclarations();
		const auto RemoveOldName = [&](const FString& Id, const FString& PublicRef) {
			const int32 Current = Model.FindUnitIndex(Id);
			if (Current != INDEX_NONE && Preserved[Current]) return;
			int32 Colon = INDEX_NONE; PublicRef.FindChar(TEXT(':'), Colon);
			const FString Handle = PublicRef.Mid(Colon + 1);
			Global->Remove(Handle.ToLower());
			if (const auto* Old = Previous.GetModel()->FindUnit(Id)) {
				Next->ChangedNames.Add(Old->GetSharedUnitId());
				const auto* Exception = Prior.AliasExceptions.Find(Id);
				ScopeNames(SemanticAliasScope(*Old)).Remove((Exception ? *Exception : Handle).ToLower());
			}
		};
		if (AppliedChanges) {
			for (const auto& Change : AppliedChanges->Changes)
				if (const auto* Ref = Previous.GetMachineManifest().PublicRefMapping.Find(Change.UnitId)) RemoveOldName(Change.UnitId, *Ref);
		} else for (const auto& Ref : Previous.GetMachineManifest().PublicRefMapping) RemoveOldName(Ref.Key, Ref.Value);
		for (const auto& Exception : Prior.AliasExceptions) {
			const int32 Current = Model.FindUnitIndex(Exception.Key);
			if (Current != INDEX_NONE && Preserved[Current]) Next->AliasExceptions.Add(Exception.Key, Exception.Value);
		}
		struct FInput { const FSemanticUnit* Unit; FString Scope, Base; };
		TArray<FInput> Inputs;
		const auto AddInput = [&](const FSemanticUnit& Unit) {
			Inputs.Add({&Unit, SemanticAliasScope(Unit), Out.ReadableAliases.Contains(Unit.GetUnitId()) ? FString() : SemanticAliasBase(Unit)});
			Next->ChangedNames.Add(Unit.GetSharedUnitId());
		};
		if (AppliedChanges) {
			for (const auto& Change : AppliedChanges->Changes)
				if (const int32 Position = Model.FindUnitIndex(Change.UnitId); Position != INDEX_NONE && !Preserved[Position]) {
					const auto* Unit = Model.FindUnit(Change.UnitId); if (!Unit) return false; AddInput(*Unit);
				}
			// The original allocator consumes current model order, independent of
			// the merge map's iteration order.
			Inputs.Sort([](const FInput& A, const FInput& B) { return A.Unit->GetUnitId() < B.Unit->GetUnitId(); });
		} else for (int32 I = 0; I < Units.Num(); ++I) if (!Preserved[I]) {
			const auto* Unit = Model.FindUnit(Units[I].GetUnitId()); if (!Unit) return false; AddInput(*Unit);
		}
		// Keep the original allocator's order: accepted names, preferred names,
		// then stable names. Only these changed scopes enter the allocator.
		for (const auto& Input : Inputs) {
			const FString& Id = Input.Unit->GetUnitId();
			if (const auto* Alias = Out.ReadableAliases.Find(Id)) {
				auto& Names = ScopeNames(Input.Scope); const FString Folded = Alias->ToLower();
				if (Names.Contains(Folded)) return Fail(Error, TEXT("semantic_alias_collision"), TEXT("Two accepted semantic identities have the same alias in one scope."), Id);
				Names.Add(Folded);
			}
			if (const auto* Handle = Out.Handles.Find(Id)) {
				const FString Folded = Handle->ToLower();
				if (Global->Contains(Folded)) return Fail(Error, TEXT("semantic_handle_collision"), TEXT("Two accepted semantic identities have the same public handle."), Id);
				Global->Add(Folded);
			}
		}
		for (const auto& Input : Inputs) {
			const FString& Id = Input.Unit->GetUnitId();
			if (Out.ReadableAliases.Contains(Id)) continue;
			const auto* Preferred = Model.GetPreferredDefinitionBindings().Find(Id);
			if (!Preferred || !IsStableOrLegacyProjectionName(Preferred->ReadableAlias, Input.Base, Id)) continue;
			auto& Names = ScopeNames(Input.Scope); const FString Folded = Preferred->ReadableAlias.ToLower();
			if (Names.Contains(Folded)) continue;
			Names.Add(Folded); Out.ReadableAliases.Add(Id, Preferred->ReadableAlias);
		}
		for (const auto& Input : Inputs) {
			const FString& Id = Input.Unit->GetUnitId();
			if (Out.ReadableAliases.Contains(Id)) continue;
			FString Alias = StableProjectionName(Input.Base, Id); auto& Names = ScopeNames(Input.Scope);
			if (Alias.IsEmpty() || Names.Contains(Alias.ToLower())) return Fail(Error, TEXT("semantic_alias_collision"),
				TEXT("A stable identity name conflicts with another bound identity; existing names were not reassigned."), Id);
			Names.Add(Alias.ToLower()); Out.ReadableAliases.Add(Id, MoveTemp(Alias));
		}
		for (const auto& Input : Inputs) {
			const FString& Id = Input.Unit->GetUnitId();
			if (Out.Handles.Contains(Id)) continue;
			const auto* Preferred = Model.GetPreferredDefinitionBindings().Find(Id);
			const FString& Alias = Out.ReadableAliases.FindChecked(Id);
			if (!Preferred || !Alias.Equals(Preferred->ReadableAlias, ESearchCase::CaseSensitive)) continue;
			const FString Prefix = UnitKindName(Input.Unit->GetUnitKind()).Replace(TEXT("_state"), TEXT("")) + TEXT(":");
			if (!Preferred->PublicRef.StartsWith(Prefix, ESearchCase::CaseSensitive)) continue;
			FString Handle = Preferred->PublicRef.Mid(Prefix.Len());
			if (Handle.IsEmpty() || Global->Contains(Handle.ToLower())) continue;
			Global->Add(Handle.ToLower()); Out.Handles.Add(Id, MoveTemp(Handle));
		}
		for (const auto& Input : Inputs) {
			const FString& Id = Input.Unit->GetUnitId();
			if (Out.Handles.Contains(Id)) continue;
			FString Handle = StableProjectionName(Input.Base, Id);
			if (Global->Contains(Handle.ToLower())) return Fail(Error, TEXT("semantic_handle_collision"),
				TEXT("A stable public handle conflicts with another identity; no existing handle was reassigned."), Id);
			Global->Add(Handle.ToLower()); Out.Handles.Add(Id, MoveTemp(Handle));
		}
		for (const auto& Scope : ChangedScopes) if (Scope.Value->IsEmpty()) Next->AliasesByScope.Remove(Scope.Key);
		if (bAliasReadFailed) return false;
		Next->Handles = MoveTemp(Global);
		for (const auto& Alias : Out.ReadableAliases)
			if (!Alias.Value.Equals(Out.Handles.FindChecked(Alias.Key), ESearchCase::CaseSensitive)) Next->AliasExceptions.Add(Alias.Key, Alias.Value);
		Out.Namespace = MoveTemp(Next); Out.AllocatedNames = Inputs.Num(); Out.ReusedNames = Units.Num() - Inputs.Num();
		return true;
	}

	static bool BuildSemanticReferenceProjection(
		const FAuthoringModel& Model, FSemanticReferenceProjection& OutProjection,
		FWorkspaceError& OutError, const FSemanticProjectionBindings* Bindings,
		const FWorkspaceLayoutPlan* Previous, bool bRetainNamespace, const FCommittedWorkspaceDelta* ActualChanges = nullptr)
	{
		OutProjection = {};
		OutError.Reset();
		OutProjection.AcceptedIdentityUnits.Init(false, Model.GetUnitCount());
		const auto PriorNamespace = Previous ? Previous->GetSharedProjection()->ReferenceNamespace : nullptr;
		const bool bMembershipDelta = PriorNamespace && PriorNamespace->Handles && BindingScopeMatches(Model, Bindings);
		OutProjection.bBorrowedAcceptedNames = bMembershipDelta && bRetainNamespace;
		TBitArray<> Preserved(false, Model.GetUnitCount());
		// The caller supplies actual changes only beside the destination's
		// accepted publication. Model allocation and last-cache lifetime are
		// not evidence of the origin of a cumulative difference.
		const auto* AppliedChanges = bMembershipDelta ? ActualChanges : nullptr;
		if (AppliedChanges) {
			Preserved.Init(true, Model.GetUnitCount());
			OutProjection.AcceptedIdentityUnits.Init(true, Model.GetUnitCount());
			for (const auto& Change : AppliedChanges->Changes) {
				const auto* Unit = Model.FindUnit(Change.UnitId);
				if (!Unit) continue;
				const auto* Before = Change.Before.IsSet() ? &Change.Before.GetValue() : nullptr;
				const bool bSame = Before && SameProjectionBindingIdentity(*Unit, *Before);
				const int32 Index = Model.FindUnitIndex(Unit->GetUnitId());
				Preserved[Index] = bSame;
				OutProjection.AcceptedIdentityUnits[Index] = bSame;
			}
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=projection_actual_delta changed_identities=%d complete_units=%d unchanged_bindings=original_owner"),
				AppliedChanges->Changes.Num(), Model.GetUnitCount());
		} else if (BindingScopeMatches(Model, Bindings)
			&& !Bindings->Visit([&](const FSemanticUnit& Before, const FString& PublicRef, const FString&, const FString&) {
				const auto* Unit = Model.FindUnit(Before.GetUnitId());
				FString Alias;
				if (!Unit || !SameProjectionBindingIdentity(*Unit, Before)) return true;
				const auto* Old = bMembershipDelta ? Previous->GetModel()->FindUnit(Unit->GetUnitId()) : nullptr;
				const auto* OldRef = bMembershipDelta ? Previous->GetMachineManifest().PublicRefMapping.Find(Unit->GetUnitId()) : nullptr;
				const bool bSamePrior = Old && OldRef && OldRef->Equals(PublicRef, ESearchCase::CaseSensitive)
					&& SameProjectionBindingIdentity(*Unit, *Old);
				if (bSamePrior) {
					if (!OutProjection.bBorrowedAcceptedNames) {
						const auto* Exception = PriorNamespace->AliasExceptions.Find(Unit->GetUnitId());
						int32 Colon = INDEX_NONE; PublicRef.FindChar(TEXT(':'), Colon);
						Alias = Exception ? *Exception : PublicRef.Mid(Colon + 1);
					}
					Preserved[Model.FindUnitIndex(Unit->GetUnitId())] = true;
				} else if (!ReadBoundProjectionAlias(Before, PublicRef, Alias)) return true;
				if (!bSamePrior || !OutProjection.bBorrowedAcceptedNames) OutProjection.ReadableAliases.Add(Unit->GetUnitId(), MoveTemp(Alias));
				OutProjection.AcceptedIdentityUnits[Model.FindUnitIndex(Unit->GetUnitId())] = true;
				if (!bSamePrior || !OutProjection.bBorrowedAcceptedNames) {
					const FString Prefix = UnitKindName(Unit->GetUnitKind()).Replace(TEXT("_state"), TEXT("")) + TEXT(":");
					OutProjection.Handles.Add(Unit->GetUnitId(), PublicRef.Mid(Prefix.Len()));
				}
				return true;
			})) return false;
		if (Bindings && !Bindings->GetModel().CheckValueReadResult(OutError)) return false;
		if (bMembershipDelta) {
			if (!BuildReferenceMembershipDelta(Model, *Previous, Preserved, OutProjection, OutError, AppliedChanges)) return false;
			for (int32 I = 0; I < Model.GetUnitCount(); ++I) {
				const auto& Unit = Model.GetDeclarations()[I];
				if (!(OutProjection.bBorrowedAcceptedNames && Preserved[I])) OutProjection.PublicRefs.Add(Unit.GetUnitId(),
					UnitKindName(Unit.GetUnitKind()).Replace(TEXT("_state"), TEXT("")) + TEXT(":") + OutProjection.Handles.FindChecked(Unit.GetUnitId()));
			}
			return true;
		}
		TSharedPtr<FSemanticReferenceNamespace> Namespace;
		if (bRetainNamespace) Namespace = MakeShared<FSemanticReferenceNamespace>();
		if (!TryBuildReadableAliases(Model, OutProjection.ReadableAliases, OutError, Namespace ? &Namespace->AliasesByScope : nullptr)) return false;
		TSet<FString> UsedHandles;
		for (const auto& Bound : OutProjection.Handles) {
			if (UsedHandles.Contains(Bound.Value.ToLower()))
				return Fail(OutError, TEXT("semantic_handle_collision"), TEXT("Two accepted semantic identities have the same public handle."), Bound.Key);
			UsedHandles.Add(Bound.Value.ToLower());
		}
		const auto IsReserved = [&](const FString& Handle, bool& Reserved) {
			Reserved = UsedHandles.Contains(Handle.ToLower());
			return Reserved || !Bindings || Bindings->IsReservedHandle(Handle, Reserved, OutError);
		};
		for (const auto& Preferred : Model.GetPreferredDefinitionBindings())
		{
			if (OutProjection.Handles.Contains(Preferred.Key)) continue;
			const FSemanticUnit* Unit = Model.FindUnit(Preferred.Key);
			const FString* Alias = OutProjection.ReadableAliases.Find(Preferred.Key);
			if (!Unit || !Alias || !Alias->Equals(Preferred.Value.ReadableAlias, ESearchCase::CaseSensitive)) continue;
			const FString Prefix = UnitKindName(Unit->GetUnitKind()).Replace(TEXT("_state"), TEXT("")) + TEXT(":");
			if (!Preferred.Value.PublicRef.StartsWith(Prefix, ESearchCase::CaseSensitive)) continue;
			FString Handle = Preferred.Value.PublicRef.Mid(Prefix.Len());
			bool Reserved = false;
			if (!IsReserved(Handle, Reserved)) return false;
			if (Handle.IsEmpty() || Reserved) continue;
			UsedHandles.Add(Handle.ToLower());
			OutProjection.Handles.Add(Preferred.Key, MoveTemp(Handle));
		}
		for (const FSemanticUnit& Unit : Model.GetUnits())
		{
			if (OutProjection.Handles.Contains(Unit.GetUnitId())) continue;
			FString Handle = StableProjectionName(SemanticAliasBase(Unit), Unit.GetUnitId());
			bool Reserved = false;
			if (!IsReserved(Handle, Reserved)) return false;
			if (Reserved)
				return Fail(OutError, TEXT("semantic_handle_collision"), TEXT("A stable public handle conflicts with another identity; no existing handle was reassigned."), Unit.GetUnitId());
			UsedHandles.Add(Handle.ToLower());
			OutProjection.Handles.Add(Unit.GetUnitId(), MoveTemp(Handle));
		}
		for (const FSemanticUnit& Unit : Model.GetUnits())
		{
			const FString Kind = UnitKindName(Unit.GetUnitKind()).Replace(TEXT("_state"), TEXT(""));
			OutProjection.PublicRefs.Add(Unit.GetUnitId(), Kind + TEXT(":")
				+ OutProjection.Handles.FindChecked(Unit.GetUnitId()));
		}
		if (Bindings && !Bindings->VisitReferencedPublicReferences([&](const FString& Id, const FString& Ref) {
			OutProjection.PublicRefs.Add(Id, Ref); return true;
		}, OutError)) return false;
		OutProjection.AllocatedNames = Model.GetUnitCount();
		if (Namespace) {
			Namespace->Handles = MakeShared<const TSet<FString>>(MoveTemp(UsedHandles));
			for (const auto& Alias : OutProjection.ReadableAliases)
				if (!Alias.Value.Equals(OutProjection.Handles.FindChecked(Alias.Key), ESearchCase::CaseSensitive)) Namespace->AliasExceptions.Add(Alias.Key, Alias.Value);
			OutProjection.Namespace = MoveTemp(Namespace);
		}
		return true;
	}

	bool TryBuildSemanticReferenceProjection(const FAuthoringModel& Model, FSemanticReferenceProjection& OutProjection,
		FWorkspaceError& OutError, const FSemanticProjectionBindings* Bindings)
	{
		return BuildSemanticReferenceProjection(Model, OutProjection, OutError, Bindings, nullptr, false);
	}

	// A bounded source proof may be rebuilt while its exact canonical material
	// stays unchanged. Storage identity is sufficient, but is not necessary, for
	// reusing layout. Keep this stricter than semantic numeric equivalence: the
	// inventory content hash still describes the original kind and lexeme.
	bool HaveOnlyNumericOrBooleanValueChanges(const FUeAgentCanonicalValue& A, const FUeAgentCanonicalValue& B)
	{
		const auto IsNumber = [](EUeAgentValueKind Kind) { return Kind == EUeAgentValueKind::Integer || Kind == EUeAgentValueKind::Decimal; };
		if (IsNumber(A.GetKind()) && IsNumber(B.GetKind())) return true;
		if (A.GetKind() != B.GetKind()) return false;
		if (A.GetKind() == EUeAgentValueKind::Boolean || A.GetKind() == EUeAgentValueKind::Null) return true;
		if (A.GetKind() == EUeAgentValueKind::String) return A.GetText().Equals(B.GetText(), ESearchCase::CaseSensitive);
		if (A.GetKind() == EUeAgentValueKind::Array) {
			if (A.GetArrayItems().Num() != B.GetArrayItems().Num()) return false;
			for (int32 I = 0; I < A.GetArrayItems().Num(); ++I)
				if (!HaveOnlyNumericOrBooleanValueChanges(A.GetArrayItems()[I].Get(), B.GetArrayItems()[I].Get())) return false;
			return true;
		}
		if (A.GetKind() == EUeAgentValueKind::Object) {
			if (A.GetObjectItems().Num() != B.GetObjectItems().Num()) return false;
			for (int32 I = 0; I < A.GetObjectItems().Num(); ++I) {
				const auto& Left = A.GetObjectItems()[I]; const auto& Right = B.GetObjectItems()[I];
				if (!Left.Key.Equals(Right.Key, ESearchCase::CaseSensitive)
					|| !HaveOnlyNumericOrBooleanValueChanges(Left.Value.Get(), Right.Value.Get())) return false;
			}
			return true;
		}
		return false;
	}

	bool AreLayoutValuesIdentical(const FUeAgentCanonicalValue& A, const FUeAgentCanonicalValue& B)
	{
		if (&A == &B) return true;
		if (A.GetKind() != B.GetKind()) return false;
		switch (A.GetKind()) {
		case EUeAgentValueKind::Null: return true;
		case EUeAgentValueKind::Boolean: return A.GetBoolean() == B.GetBoolean();
		case EUeAgentValueKind::String: return A.GetText().Equals(B.GetText(), ESearchCase::CaseSensitive);
		case EUeAgentValueKind::Integer:
		case EUeAgentValueKind::Decimal: return A.GetLexeme().Equals(B.GetLexeme(), ESearchCase::CaseSensitive);
		case EUeAgentValueKind::Array:
			if (A.GetArrayItems().Num() != B.GetArrayItems().Num()) return false;
			for (int32 Index = 0; Index < A.GetArrayItems().Num(); ++Index)
				if (!AreLayoutValuesIdentical(A.GetArrayItems()[Index].Get(), B.GetArrayItems()[Index].Get())) return false;
			return true;
		case EUeAgentValueKind::Object:
			if (A.GetObjectItems().Num() != B.GetObjectItems().Num()) return false;
			for (int32 Index = 0; Index < A.GetObjectItems().Num(); ++Index) {
				const auto& Left = A.GetObjectItems()[Index]; const auto& Right = B.GetObjectItems()[Index];
				if (!Left.Key.Equals(Right.Key, ESearchCase::CaseSensitive)
					|| !AreLayoutValuesIdentical(Left.Value.Get(), Right.Value.Get())) return false;
			}
			return true;
		case EUeAgentValueKind::Temporal:
		case EUeAgentValueKind::Spatial:
		{
			// Consume the existing compiler's complete typed material, including
			// frame/clock/evidence fields. Comparing projected components alone
			// would omit inputs; semantic numeric equality is also too weak here.
			FString LeftMaterial, RightMaterial;
			AppendCanonicalValueMaterial(LeftMaterial, A);
			AppendCanonicalValueMaterial(RightMaterial, B);
			return LeftMaterial.Equals(RightMaterial, ESearchCase::CaseSensitive);
		}
		default:
			// Unknown future kinds retain the established full path.
			return false;
		}
	}

	bool TryPlanWorkspaceLayout(
		FAuthoringModelRef Model,
		int32 LayoutVersion,
		EWorkspaceSourceMode SourceMode,
		TMap<FString, int64> Telemetry,
		FUeAgentResourceTracker& Resources,
		FWorkspaceLayoutPlanPtr& OutPlan,
		FWorkspaceError& OutError, const FSemanticProjectionBindings* Bindings,
		const FCommittedWorkspaceDelta* ActualChanges, const FWorkspaceLayoutPlan* AcceptedLayout)
	{
		auto& PreviousCharges = PreviousLayoutCharges;
		const auto UsageBefore = Resources.Snapshot();
		const double Started = FPlatformTime::Seconds();
		double ProjectionSeconds = 0, PlacementSeconds = 0, RelocationSeconds = 0, CollisionSeconds = 0, InventorySeconds = 0;
		int32 OwnedRowHashReuses = 0, GeneratedRowHashes = 0;
		const int64 DigestHitsBefore = ContentDigestHits, DigestMissesBefore = ContentDigestMisses;
		FRowDigestWork RowWork;
		TGuardValue<FRowDigestWork*> DigestScope(ActiveRowDigestWork, &RowWork);
		ON_SCOPE_EXIT {
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=layout_content_material reused=%lld generated=%lld total_layout_s=%.6f"),
				ContentDigestHits - DigestHitsBefore, ContentDigestMisses - DigestMissesBefore, FPlatformTime::Seconds() - Started);
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=layout_parts projection_s=%.6f placement_s=%.6f relocation_s=%.6f collision_s=%.6f inventory_s=%.6f"),
				ProjectionSeconds, PlacementSeconds, RelocationSeconds, CollisionSeconds, InventorySeconds);
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=layout_inventory_hashes immutable_owner_rows=%d generated_rows=%d inventory_materialized=false temporary_row_text=false formatter=original_token_stream"),
				OwnedRowHashReuses, GeneratedRowHashes);
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=layout_row_digest_parts rows=%lld row_prepare_s=%.6f writer_stream_exclusive_s=%.6f utf8_digest_s=%.6f tokens=%lld utf8_bytes=%lld json_row_bridge=false private_usage=false envelope=borrowed_unit_fields temporary_row_nodes=0"),
				RowWork.Rows, RowWork.Prepare, RowWork.Writer, RowWork.EncodingDigest, RowWork.Tokens, RowWork.Utf8Bytes);
		};
		OutPlan.Reset();
		OutError.Reset();
		if (LayoutVersion < 1)
		{
			auto Failure = Contract::FDiagnostic::Failure(TEXT("invalid_layout_version"), TEXT("The workspace layout version must be positive."), TEXT("workspace_layout"));
			Failure.PropertyPath = TEXT("layout_version");
			Failure.ActualValue = MakeShared<FJsonValueNumber>(LayoutVersion);
			Failure.ExpectedValue = MakeShared<FJsonValueString>(TEXT("integer >= 1"));
			OutError.SetFailure(MoveTemp(Failure));
			return false;
		}
		TMap<FString, int64> NormalizedTelemetry;
		for (const TPair<FString, int64>& Entry : Telemetry)
		{
			const FString Key = Normalize(Entry.Key);
			if (Key.IsEmpty()
				|| Entry.Value < 0
				|| NormalizedTelemetry.Contains(Key))
			{
				auto Failure = Contract::FDiagnostic::Failure(TEXT("invalid_layout_telemetry"), Key.IsEmpty()
					? TEXT("The layout telemetry field name is empty.") : Entry.Value < 0 ? TEXT("The layout telemetry count must be nonnegative.")
					: TEXT("Two layout telemetry field names are equal after normalization."), TEXT("workspace_layout"));
				Failure.CandidateName = Entry.Key;
				Failure.ActualValue = MakeShared<FJsonValueNumber>(Entry.Value);
				Failure.PropertyPath = TEXT("telemetry");
				OutError.SetFailure(MoveTemp(Failure));
				return false;
			}
			NormalizedTelemetry.Add(Key, Entry.Value);
		}
		Telemetry = MoveTemp(NormalizedTelemetry);
		// A formatter slice is not a complete publication and must neither
		// borrow nor replace the complete layout memo.
		// A target unit ID is local to a model (for example authoring_target),
		// not the identity of a workspace/asset. Only the caller's destination
		// publication may supply an old layout for a different model. The last
		// layout memo can otherwise belong to another workspace, whose lazy
		// values must never become inputs of this request merely to retire names.
		const auto* Previous = Model->IsOutputScope() ? nullptr : AcceptedLayout ? AcceptedLayout
			: PreviousLayout && PreviousLayout->GetModel() == Model ? PreviousLayout.Get() : nullptr;
		const bool bReusableScope = Previous && Previous->GetLayoutVersion() == LayoutVersion && Previous->GetSourceMode() == SourceMode
			&& Previous->GetModel()->GetContractId() == Model->GetContractId()
			&& Previous->GetModel()->GetContractVersion() == Model->GetContractVersion()
			&& Previous->GetModel()->GetProfileId() == Model->GetProfileId()
			&& Previous->GetModel()->GetTargetUnitIds() == Model->GetTargetUnitIds();
		bool bReuse = bReusableScope && Previous->GetModel()->GetUnitCount() == Model->GetUnitCount();
		int32 ChangedHashCount = 0;
		int32 ReusedUnitPositions = 0;
		int32 UnitIdentityLookups = 0;
		int32 ExactValueReuses = 0;
		int32 PropertyContentLayoutReuses = 0;
		const TCHAR* FallbackReason = bReuse ? TEXT("none") : bReusableScope ? TEXT("membership_delta") : TEXT("model_scope");
		int32 FallbackUnitIndex = INDEX_NONE;
		const auto* AppliedLayoutChanges = AcceptedLayout && bReusableScope ? ActualChanges : nullptr;
		if (AppliedLayoutChanges) {
			// These bindings were produced beside this exact accepted layout. The
			// committed merge already identifies every replacement/removal, including
			// native side effects. Only those identities can invalidate the inherited
			// namespace; the value/layout and model-level definition checks below
			// still decide whether the complete projection itself is reusable.
			for (const auto& Change : AppliedLayoutChanges->Changes) {
				const auto* Unit = Model->FindUnit(Change.UnitId);
				const auto* Before = Change.Before.IsSet() ? &Change.Before.GetValue() : nullptr;
				if (!Unit || !Before || !SameProjectionBindingIdentity(*Unit, *Before)) {
					bReuse = false; FallbackReason = TEXT("accepted_projection_delta"); break;
				}
			}
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=layout_binding_handoff complete_units=%d checked_bindings=%d unchanged_bindings=original_owner"),
				Model->GetUnitCount(), AppliedLayoutChanges->Changes.Num());
		} else if (bReuse && BindingScopeMatches(Model.Get(), Bindings)) {
			int32 MatchedBindings = 0;
			bReuse = Bindings->Visit([&](const FSemanticUnit& Before, const FString& PublicRef, const FString&, const FString&) {
				const auto* Unit = Model->FindUnit(Before.GetUnitId());
				FString Alias;
				if (!Unit || !SameProjectionBindingIdentity(*Unit, Before) || !ReadBoundProjectionAlias(Before, PublicRef, Alias)) return true;
				const auto* Prior = Previous->GetMachineManifest().PublicRefMapping.Find(Unit->GetUnitId());
				if (!Prior || !Prior->Equals(PublicRef, ESearchCase::CaseSensitive)) return false;
				++MatchedBindings; return true;
			}) && MatchedBindings == Model->GetUnitCount();
			if (!Bindings->GetModel().CheckValueReadResult(OutError)) return false;
			if (!bReuse) FallbackReason = TEXT("accepted_projection_bindings");
		} else if (bReuse) {
			// A fresh workspace must not inherit a different workspace's legacy
			// ordinal policy from the optional last-layout optimization.
			for (const auto& Unit : Model->GetUnits()) {
				const auto* Prior = Previous->GetMachineManifest().PublicRefMapping.Find(Unit.GetUnitId());
				const auto* Preferred = Model->GetPreferredDefinitionBindings().Find(Unit.GetUnitId());
				const FString Expected = Preferred && !Preferred->PublicRef.IsEmpty() ? Preferred->PublicRef
					: UnitKindName(Unit.GetUnitKind()).Replace(TEXT("_state"), TEXT("")) + TEXT(":")
						+ StableProjectionName(SemanticAliasBase(Unit), Unit.GetUnitId());
				if (!Prior || !Prior->Equals(Expected, ESearchCase::CaseSensitive)) { bReuse = false; FallbackReason = TEXT("fresh_projection_identity"); break; }
			}
		}
		if (bReuse && Previous->GetModel() != Model) {
			TArray<int32> CandidateIndices;
			TMap<int32, const FSemanticUnit*> PublishedBefore;
			if (AppliedLayoutChanges) {
				CandidateIndices.Reserve(AppliedLayoutChanges->Changes.Num());
				for (const auto& Change : AppliedLayoutChanges->Changes) {
					const int32 Index = Model->FindUnitIndex(Change.UnitId);
					if (Index == INDEX_NONE) continue;
					CandidateIndices.Add(Index);
					PublishedBefore.Add(Index, Change.Before.IsSet() ? &Change.Before.GetValue() : nullptr);
				}
			} else {
				CandidateIndices.Reserve(Model->GetUnitCount());
				for (int32 Index = 0; Index < Model->GetUnitCount(); ++Index) CandidateIndices.Add(Index);
			}
			for (const int32 UnitIndex : CandidateIndices) {
				const auto* UnitValue = Model->FindUnit(Model->GetDeclarations()[UnitIndex].GetUnitId());
				if (!UnitValue) { Model->CheckValueReadResult(OutError); return false; }
				const auto& Unit = *UnitValue;
				const FSemanticUnit* Old = AppliedLayoutChanges ? PublishedBefore.FindChecked(UnitIndex)
					: Previous->GetModel()->FindUnit(Unit.GetUnitId());
				++UnitIdentityLookups;
				// A sealed unit includes provenance, category and every layout
				// field. Definition naming also reads model-level inputs below.
				if (Old && Unit.GetRole() != TEXT("property_definition")
					&& Old->SharesStorageWith(Unit)) continue;
				// Prove the complete layout/reference invariants. The old collision,
				// alias and grouping result is reusable only for this exact structure.
				if (!Old || Old->GetUnitKind() != Unit.GetUnitKind() || Old->GetOwnerRef() != Unit.GetOwnerRef()
					|| Old->GetSemanticTypeRef() != Unit.GetSemanticTypeRef() || Old->GetRole() != Unit.GetRole()
					|| Old->GetFieldSetRef() != Unit.GetFieldSetRef() || Old->GetItemRefs() != Unit.GetItemRefs()
					|| Old->GetRelationRefs() != Unit.GetRelationRefs() || Old->GetOrderingRef() != Unit.GetOrderingRef()
					|| Old->GetLayoutRef() != Unit.GetLayoutRef() || Old->GetDependencyRefs() != Unit.GetDependencyRefs()
					|| Old->GetProfileVisibility() != Unit.GetProfileVisibility() || Old->GetSourceProvenance() != Unit.GetSourceProvenance()
					|| Old->GetTargetIdentityKey() != Unit.GetTargetIdentityKey() || Old->GetCategoryPath() != Unit.GetCategoryPath()
					|| Old->GetValueCount() != Unit.GetValueCount()) { bReuse = false; FallbackReason = TEXT("unit_layout_fields"); FallbackUnitIndex = UnitIndex; break; }
				if (Unit.GetRole() == TEXT("property_definition")) {
					const FString* Name = Model->FindReadablePropertyDefinitionName(Unit.GetUnitId());
					const FString* OldName = Previous->GetModel()->FindReadablePropertyDefinitionName(Unit.GetUnitId());
					if ((Name != nullptr) != (OldName != nullptr) || (Name && !Name->Equals(*OldName, ESearchCase::CaseSensitive)))
					{ bReuse = false; FallbackReason = TEXT("definition_name"); FallbackUnitIndex = UnitIndex; break; }
					const auto* Preferred = Model->GetPreferredDefinitionBindings().Find(Unit.GetUnitId());
					const auto* Prior = Previous->GetDefinitionProjectionHints().Definitions.Find(Unit.GetUnitId());
					if (Preferred ? (!Prior || !Preferred->ReadableAlias.Equals(Prior->ReadableAlias, ESearchCase::CaseSensitive)
						|| !Preferred->PublicRef.Equals(Prior->PublicRef, ESearchCase::CaseSensitive))
						: Previous->GetModel()->GetPreferredDefinitionBindings().Contains(Unit.GetUnitId()))
					{ bReuse = false; FallbackReason = TEXT("definition_binding"); FallbackUnitIndex = UnitIndex; break; }
				}
				bool bChanged = false;
				bool bPropertyLayoutChecked = false, bPropertyLayoutProven = false;
				for (const auto& Value : Unit.GetValues()) {
					// FString's map lookup is case-insensitive; a case-only value
					// member rename changes the canonical material and its hash.
					const FUeAgentCanonicalValueRef* OldValue = nullptr;
					for (const auto& OldMember : Old->GetValues())
						if (OldMember.Key.Equals(Value.Key, ESearchCase::CaseSensitive)) { OldValue = &OldMember.Value; break; }
					if (!OldValue) { bReuse = false; FallbackReason = TEXT("value_membership"); FallbackUnitIndex = UnitIndex; break; }
					if (&OldValue->Get() == &Value.Value.Get()) continue;
					if (AreLayoutValuesIdentical(OldValue->Get(), Value.Value.Get())) { ++ExactValueReuses; continue; }
					if (Unit.GetUnitKind() == ESemanticUnitKind::ItemState
						&& IsGraphItemContentOnlyField(Unit.GetSemanticTypeRef(), Value.Key)) {
						// The graph projection owner identifies this payload precisely.
						// Keep checking every other field; a pin rename/type/identity or
						// membership change must still rebuild its actual projection.
						bChanged = true;
						continue;
					}
					if (!bPropertyLayoutChecked) {
						bPropertyLayoutChecked = true;
						// Ordinary property payload is not a placement input. Alias
						// allocation is; consume its actual naming owner. Only the
						// native naming properties read by that owner can change a
						// node alias. Other native effects have their own actual rows.
						// Definition content owns binding/collision semantics.
						bPropertyLayoutProven = Unit.GetUnitKind() == ESemanticUnitKind::PropertySet
							&& Unit.GetRole() != TEXT("property_definition")
							&& ((!HasGraphStructureAncestor(Unit, Model.Get())
									&& !HasGraphStructureAncestor(*Old, Previous->GetModel().Get()))
								|| !IsGraphNodeNativeNameProperty(Unit.GetFieldSetRef()))
							&& SemanticAliasBase(Unit).Equals(SemanticAliasBase(*Old), ESearchCase::CaseSensitive);
					}
					if (!bPropertyLayoutProven) { bReuse = false; FallbackReason = TEXT("value_layout_dependency"); FallbackUnitIndex = UnitIndex; break; }
					bChanged = true;
				}
				if (!bReuse) break;
				if (bChanged) {
					++PropertyContentLayoutReuses;
					FString Hash;
					if (!TryHashSemanticUnitRow(Unit, Hash, OutError)) return false;
					++ChangedHashCount;
				}
			}
		}
		if (bReuse) {
			FUeAgentResourceChargeError ResourceError;
			if (!Resources.TryChargeMany(PreviousCharges, TEXT("semantic_layout.plan"), ResourceError)) return FailResource(OutError, ResourceError);
			bool bSameTelemetry = Telemetry.Num() == Previous->GetTelemetry().Num();
			if (bSameTelemetry) for (const auto& Pair : Telemetry) {
				bool bFound = false;
				for (const auto& Prior : Previous->GetTelemetry())
					if (Pair.Key.Equals(Prior.Key, ESearchCase::CaseSensitive) && Pair.Value == Prior.Value) { bFound = true; break; }
				if (!bFound) { bSameTelemetry = false; break; }
			}
			// The authoritative capture returned this exact immutable model. The
			// manifest, complete membership and telemetry therefore need no copy or
			// empty per-unit update; resource charges still belong to this request.
			if (Previous == PreviousLayout.Get() && Previous->GetModel() == Model && bSameTelemetry) {
				OutPlan = PreviousLayout;
				UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=layout_reuse units=%d changed_units=0 complete_plan_shared=true total_s=%.6f"),
					Model->GetUnitCount(), FPlatformTime::Seconds() - Started);
				return true;
			}
			FMachineWorkspaceManifest Manifest = Previous->GetMachineManifest();
			Manifest.SemanticFingerprint = Model->GetSemanticFingerprint(); Manifest.BaseRevision = Model->GetBaseRevision();
			// Exact value/layout reuse can still have fresh unit storage. Its row
			// result belongs to that storage, not the previous manifest position.
			if (AppliedLayoutChanges) {
				for (const auto& Change : AppliedLayoutChanges->Changes)
					if (Change.After && !TryEnsureSemanticUnitRowHash(*Change.After, OutError, FString())) return false;
			} else for (const auto& Unit : Model->GetUnits())
				if (!TryEnsureSemanticUnitRowHash(Unit, OutError, FString())) return false;
			Manifest.UnitInventory = FWorkspaceUnitInventoryView(Model);
			OutPlan = MakeShareable(new FWorkspaceLayoutPlan(Model, MoveTemp(Manifest), MoveTemp(Telemetry), *Previous));
			PreviousLayout = OutPlan;
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=layout_reuse units=%d changed_units=%d reused_unit_positions=%d unit_identity_lookups=%d exact_value_reuses=%d property_content_reuses=%d total_s=%.6f"),
				Model->GetUnitCount(), ChangedHashCount, ReusedUnitPositions, UnitIdentityLookups, ExactValueReuses, PropertyContentLayoutReuses, FPlatformTime::Seconds() - Started);
			return true;
		}
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=layout_fallback units=%d reason=%s unit_index=%d exact_value_reuses=%d proof_s=%.6f"),
			Model->GetUnitCount(), FallbackReason, FallbackUnitIndex, ExactValueReuses, FPlatformTime::Seconds() - Started);
		if (Model->GetDeclarations().IsValidIndex(FallbackUnitIndex)) {
			const auto& Declaration = Model->GetDeclarations()[FallbackUnitIndex];
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=layout_actual_dependency kind=%s role=%s semantic=%s field_set=%s"),
				*LexToString(Declaration.GetUnitKind()), *Declaration.GetRole(), *Declaration.GetSemanticTypeRef(), *Declaration.GetFieldSetRef());
		}

		double PartStarted = FPlatformTime::Seconds();
		FSemanticReferenceProjection References;
		if (!BuildSemanticReferenceProjection(Model.Get(), References, OutError, Bindings, bReusableScope ? Previous : nullptr, true, AppliedLayoutChanges)) return false;
		const int32 ReusedReferenceNames = References.ReusedNames, AllocatedReferenceNames = References.AllocatedNames;
		const auto ReferenceNamespace = MoveTemp(References.Namespace);
		TMap<FString, FString> ReadableAliases = MoveTemp(References.ReadableAliases);
		TMap<FString, FString> Handles = MoveTemp(References.Handles);
		const auto* AcceptedRefs = References.bBorrowedAcceptedNames ? &Previous->GetMachineManifest().PublicRefMapping : nullptr;
		const FLayoutNameLookup AliasLookup(ReadableAliases, AcceptedRefs, ReferenceNamespace ? &ReferenceNamespace->AliasExceptions : nullptr);
		const FLayoutNameLookup HandleLookup(Handles, AcceptedRefs);
		FDefinitionProjectionHints DefinitionHints;
		DefinitionHints.ContractId = Model->GetContractId();
		DefinitionHints.ContractVersion = Model->GetContractVersion();
		DefinitionHints.ProfileId = Model->GetProfileId();
		DefinitionHints.TargetUnitIds = Model->GetTargetUnitIds();
		for (const auto& Declaration : Model->GetDeclarations()) {
			if (Declaration.GetUnitKind() != ESemanticUnitKind::CollectionState || Declaration.GetRole() != TEXT("property_definition")) continue;
			const auto* Value = Model->FindUnit(Declaration.GetUnitId()); if (!Value) { Model->CheckValueReadResult(OutError); return false; }
			const auto& Unit = *Value;
			const FString* Name = Model->FindReadablePropertyDefinitionName(Unit.GetUnitId());
			if (!Name) continue;
			FDefinitionProjectionBinding Binding;
			if (!TryHashSemanticUnitRow(Unit, Binding.ContentKey, OutError)) return false;
			Binding.DefinitionName = *Name;
			Binding.ReadableAlias = AliasLookup.FindChecked(Unit.GetUnitId());
			const auto* PublicRef = References.PublicRefs.Find(Unit.GetUnitId());
			Binding.PublicRef = PublicRef ? *PublicRef : AcceptedRefs->FindChecked(Unit.GetUnitId());
			DefinitionHints.Definitions.Add(Unit.GetUnitId(), MoveTemp(Binding));
		}
		const TBitArray<> AcceptedIdentityUnits = MoveTemp(References.AcceptedIdentityUnits);
		ProjectionSeconds = FPlatformTime::Seconds() - PartStarted; PartStarted = FPlatformTime::Seconds();

		TSharedPtr<const FWorkspaceLayoutPlan::FProjection> PriorPlacements;
		if (bReusableScope) PriorPlacements = Previous->GetSharedProjection();
		FMutableLayoutPlacements PlacementBuilder(*Model, MoveTemp(PriorPlacements));
		FMutableLayoutPlacements::FColumn Placements(PlacementBuilder, FMutableLayoutPlacements::EColumn::Logical);
		FMutableLayoutPlacements::FColumn PhysicalPlacements(PlacementBuilder, FMutableLayoutPlacements::EColumn::Physical);
		FMutableLayoutPlacements::FColumn BoundPhysicalPlacements(PlacementBuilder, FMutableLayoutPlacements::EColumn::BoundPhysical);
		TMap<FString, FString> LogicalOwnerBases, PhysicalOwnerBases;
		int32 ReusedLogicalOwnerBases = 0, ReusedPhysicalOwnerBases = 0, ReusedGraphAncestors = 0;
		int32 InheritedLayoutContexts = 0, ComparedLayoutContexts = 0;
		int32 InheritedLogicalPaths = 0, LogicalCollisionTextChecks = 0;
		TMap<FString, bool> GraphAncestorsByOwner;
		auto HasGraphAncestor = [&](const auto& Unit) {
			if (Unit.GetOwnerRef().IsEmpty()) return false;
			if (const bool* Known = GraphAncestorsByOwner.Find(Unit.GetOwnerRef())) {
				++ReusedGraphAncestors;
				return *Known;
			}
			const bool bHasGraph = HasGraphStructureAncestor(Unit, Model.Get());
			GraphAncestorsByOwner.Add(Unit.GetOwnerRef(), bHasGraph);
			return bHasGraph;
		};
		if (SourceMode == EWorkspaceSourceMode::Directory && AppliedLayoutChanges) {
			TMap<FString, const FCommittedUnitChange*> Changes;
			for (const auto& Change : AppliedLayoutChanges->Changes) Changes.Add(Change.UnitId, &Change);
			// This array indexes only the current request's actual compiled directory.
			// It is not a positional proof shared across insertions or publications.
			TArray<int8> Contexts;
			Contexts.Init(-1, Model->GetUnitCount());
			TFunction<bool(const FSemanticUnit::FDeclarationView&)> SameContext = [&](const FSemanticUnit::FDeclarationView& Declaration) {
				const int32 Index = Model->FindUnitIndex(Declaration.GetUnitId());
				if (Contexts[Index] != -1) return Contexts[Index] != 0;
				const auto Before = Previous->GetModel()->FindDeclaration(Declaration.GetUnitId());
				bool bSame = Before && AcceptedIdentityUnits[Index];
				const auto* Change = Changes.Find(Declaration.GetUnitId());
				if (bSame && Change) {
					++ComparedLayoutContexts;
					bSame = Declaration.GetOwnerRef() == Before->GetOwnerRef() && Declaration.GetRole() == Before->GetRole()
						&& Declaration.GetFieldSetRef() == Before->GetFieldSetRef()
						&& Declaration.GetValueCount() == Before->GetValueCount();
					for (const auto& Contract : GetKeyedItemProjectionContracts()) if (bSame)
						bSame = Declaration.HasKeyedRoot(Contract.RootField) == Before->HasKeyedRoot(Contract.RootField);
					if (bSame && (*Change)->Before && (*Change)->After)
						for (const TCHAR* Field : {TEXT("emitter_name"), TEXT("usage")})
							bSame &= ReadSemanticString(*(*Change)->Before, Field) == ReadSemanticString(*(*Change)->After, Field);
				} else if (bSame) {
					// The committed directory inherits this exact unmodified declaration.
					// Its owner/name context still has to pass below.
					++InheritedLayoutContexts;
				}
				if (bSame && !Declaration.GetOwnerRef().IsEmpty()) {
					const auto Owner = Model->FindDeclaration(Declaration.GetOwnerRef()); bSame = Owner && SameContext(*Owner);
				}
				Contexts[Index] = bSame ? 1 : 0; return bSame;
			};
			for (const auto& Declaration : Model->GetDeclarations()) if (SameContext(Declaration)
				&& !IsGraphStructureUnit(Declaration)) {
				const auto* Logical = Previous->GetLogicalPlacements().Find(Declaration.GetUnitId());
				const auto* Physical = Previous->GetPhysicalPlacements().Find(Declaration.GetUnitId());
				// Graph child paths use the graph allocator; keep that domain's
				// actual graph consumer, rather than restoring unrelated value rows.
				if (Logical && Physical && !HasGraphAncestor(Declaration)) PlacementBuilder.Bind(Declaration, *Logical, *Physical);
			}
		}
		// A cold explicit complete input has no inherited layout. Its accepted
		// public aliases/handles were consumed by the original allocator above;
		// construct paths from those names and the current owner/shape below.
		// Do not restore every old declaration solely to prove that copying its
		// old path would also be possible. Scoped actual/ordering bindings above
		// retain their own direct path and reference handoff.
		// Public identities have a global namespace; physical navigation has owner
		// and directory scopes. Hidden state must not rename an unrelated graph or
		// asset folder merely because its readable base happens to match.
		const TMap<FString, FString> GraphPhysicalPaths =
			BuildGraphPhysicalPaths(Model.Get(), AliasLookup);
		TMap<FString, FString> GraphPhysicalHandles;
		for (const auto& Graph : Model->GetDeclarations())
		{
			if (!IsGraphStructureUnit(Graph)) continue;
			FGraphAuthoringAliasProjection NodeAliases;
			if (!TryBuildGraphModelNodeAliases(Model.Get(), Graph.GetUnitId(), NodeAliases, OutError)) return false;
			for (const auto& Alias : NodeAliases.AliasBySourceId)
				GraphPhysicalHandles.Add(Alias.Key, MakeAuthoringContentPathSegment(Alias.Value));
		}
		const FLayoutNameLookup PhysicalHandles(GraphPhysicalHandles, nullptr, nullptr, &AliasLookup);
		// Accepted bound paths are an unchanged subset of the prior validated
		// namespace. Only new/recomputed paths can introduce a collision.
		// Keep the original full check for callers without that actual origin.
		const bool bInheritedLogicalNamespace = SourceMode == EWorkspaceSourceMode::Directory && AppliedLayoutChanges;
		TMultiMap<uint32, int32> UnitPaths;
		UnitPaths.Reserve(bInheritedLogicalNamespace ? AppliedLayoutChanges->Changes.Num() : Model->GetUnitCount());
		for (const auto& Declaration : Model->GetDeclarations())
		{
			const FString* BoundLogical = Placements.Find(Declaration);
			FString ComputedPath;
			if (!BoundLogical) {
				ComputedPath = LogicalPath(Declaration, Model.Get(), HandleLookup, LogicalOwnerBases, ReusedLogicalOwnerBases, OutError);
			}
			const FString& Path = BoundLogical ? *BoundLogical : ComputedPath;
			if (Path.IsEmpty())
			{
				return false;
			}
			const bool bBoundPlacement = BoundPhysicalPlacements.Contains(Declaration);
			if (bInheritedLogicalNamespace && BoundLogical && bBoundPlacement) {
				++InheritedLogicalPaths;
			} else {
				TArray<int32, TInlineAllocator<1>> PathCandidates;
				UnitPaths.MultiFind(GetTypeHash(Path), PathCandidates);
				for (const int32 Candidate : PathCandidates) {
					++LogicalCollisionTextChecks;
					if (Placements.FindChecked(Model->GetDeclarations()[Candidate]).Equals(Path, ESearchCase::IgnoreCase))
						return Fail(OutError, TEXT("logical_path_collision"), TEXT("More than one semantic unit maps to the same workspace file path."), Path);
				}
				UnitPaths.Add(GetTypeHash(Path), Model->FindUnitIndex(Declaration.GetUnitId()));
			}
			if (!BoundLogical) Placements.Set(Declaration, MoveTemp(ComputedPath));
			// Bind already installed this physical value; setting it again only
			// repeats the lookup and equality check for every inherited row.
			if (bBoundPlacement) continue;
			const auto& Unit = Declaration;
			FString PhysicalPath = LogicalPath(Unit, Model.Get(), PhysicalHandles, PhysicalOwnerBases, ReusedPhysicalOwnerBases, OutError);
			if (PhysicalPath.IsEmpty()) return false;
			const auto SurfaceOwner = Unit.GetUnitKind() == ESemanticUnitKind::OrderingState
				? Model->FindDeclaration(Unit.GetOwnerRef()) : TOptional<FSemanticUnit::FDeclarationView>(Unit);
			const bool bInternalStructureSurface = SurfaceOwner && IsInternalAuthoringStructureSurface(
				SurfaceOwner->GetUnitKind(), SurfaceOwner->GetSemanticTypeRef(), SurfaceOwner->GetValueCount());
			bool bAggregatedItem = false;
			if (Unit.GetUnitKind() == ESemanticUnitKind::ItemState)
			{
				const auto Parent = Model->FindDeclaration(Unit.GetOwnerRef());
				if (Parent && Parent->GetUnitKind() == ESemanticUnitKind::StructureState)
					for (const FKeyedItemProjectionContract& Contract : GetKeyedItemProjectionContracts())
						if (!Contract.AggregatedItemSemanticSuffix.IsEmpty()
							&& Unit.GetSemanticTypeRef().EndsWith(Contract.AggregatedItemSemanticSuffix)
							&& Parent->HasKeyedRoot(Contract.RootField))
							bAggregatedItem = true;
			}
			if (bInternalStructureSurface)
			{
				PhysicalPath = TEXT(".uai/surface-state/")
					+ HandleLookup.FindChecked(Unit.GetUnitId()) + TEXT(".hjson");
			}
			else if (bAggregatedItem)
			{
				PhysicalPath = TEXT(".uai/keyed-state/")
					+ HandleLookup.FindChecked(Unit.GetUnitId()) + TEXT(".hjson");
			}
			else if (const FString* GraphPath = GraphPhysicalPaths.Find(Unit.GetUnitId()))
			{
				PhysicalPath = *GraphPath;
			}
			else if ((Unit.GetUnitKind() == ESemanticUnitKind::ItemState
					|| Unit.GetUnitKind() == ESemanticUnitKind::RelationSet
					|| Unit.GetUnitKind() == ESemanticUnitKind::OrderingState
					|| Unit.GetUnitKind() == ESemanticUnitKind::LayoutState)
				&& HasGraphAncestor(Unit))
			{
				PhysicalPath = TEXT(".uai/graph-state/")
					+ HandleLookup.FindChecked(Unit.GetUnitId())
					+ TEXT(".hjson");
			}
			else if (Unit.GetUnitKind() == ESemanticUnitKind::PropertySet
				&& Unit.GetRole() != TEXT("authored_inputs"))
			{
				const int32 PropertiesMarker = PhysicalPath.Find(
					TEXT("/properties/"), ESearchCase::CaseSensitive,
					ESearchDir::FromEnd);
				if (PropertiesMarker != INDEX_NONE)
				{
					PhysicalPath = PhysicalPath.Left(PropertiesMarker)
						+ TEXT("/properties.hjson");
				}
			}
			else if (Unit.GetUnitKind() == ESemanticUnitKind::CollectionState
				&& Unit.GetRole() == TEXT("property_definition"))
			{
				const int32 CollectionsMarker = PhysicalPath.Find(
					TEXT("/collections/"), ESearchCase::CaseSensitive,
					ESearchDir::FromEnd);
				if (CollectionsMarker != INDEX_NONE)
				{
					PhysicalPath = PhysicalPath.Left(CollectionsMarker)
						+ TEXT("/definitions.hjson");
				}
			}
			PhysicalPlacements.Set(Unit, MoveTemp(PhysicalPath));
		}
		if (bInheritedLogicalNamespace && InheritedLogicalPaths > 0 && UnitPaths.Num() > 0) {
			// Check new-vs-retained paths without rebuilding an N-entry lookup
			// for the old-old pairs which the accepted producer already proved.
			for (const auto& Declaration : Model->GetDeclarations()) {
				if (!BoundPhysicalPlacements.Contains(Declaration)) continue;
				const FString& Path = Placements.FindChecked(Declaration);
				TArray<int32, TInlineAllocator<1>> PathCandidates;
				UnitPaths.MultiFind(GetTypeHash(Path), PathCandidates);
				for (const int32 Candidate : PathCandidates) {
					++LogicalCollisionTextChecks;
					if (Placements.FindChecked(Model->GetDeclarations()[Candidate]).Equals(Path, ESearchCase::IgnoreCase))
						return Fail(OutError, TEXT("logical_path_collision"), TEXT("More than one semantic unit maps to the same workspace file path."), Path);
				}
			}
		}

		PlacementSeconds = FPlatformTime::Seconds() - PartStarted; PartStarted = FPlatformTime::Seconds();
		TMap<FString, FString> ContentDirectories;
		if (!TryBuildAuthoringContentDirectoriesImpl(Model.Get(), PhysicalPlacements, ContentDirectories, OutError,
			&BoundPhysicalPlacements, &AliasLookup, true)) return false;
		TSet<FString> RelocatedOwnerDocuments;
		for (const auto& Declaration : Model->GetDeclarations())
		{
			if (BoundPhysicalPlacements.Contains(Declaration)) continue;
			const auto& Unit = Declaration;
			if (PhysicalPlacements.FindChecked(Unit).StartsWith(TEXT(".uai/"))
				|| IsGraphStructureUnit(Unit) || HasGraphAncestor(Unit)) continue;
			if (Unit.GetUnitKind() == ESemanticUnitKind::PropertySet
				&& Unit.GetRole().StartsWith(TEXT("preview/")))
			{
				const FString Path = Unit.GetRole() + TEXT(".hjson");
				PhysicalPlacements.Set(Unit, Path);
				RelocatedOwnerDocuments.Add(Unit.GetUnitId());
				continue;
			}
			FString OwnerFile;
			switch (Unit.GetUnitKind())
			{
			case ESemanticUnitKind::ItemState: OwnerFile = TEXT("state.hjson"); break;
			case ESemanticUnitKind::PropertySet:
				OwnerFile = Unit.GetRole() == TEXT("authored_inputs")
					? PropertyDocumentName::Inputs
					: Unit.GetSemanticTypeRef() == TEXT("uai.semantic.umg.slot.property")
						? PropertyDocumentName::SlotProperties
						: PropertyDocumentName::Properties;
				break;
			case ESemanticUnitKind::OrderingState: OwnerFile = TEXT("ordering.hjson"); break;
			case ESemanticUnitKind::RelationSet: OwnerFile = TEXT("relations.hjson"); break;
			case ESemanticUnitKind::LayoutState: OwnerFile = TEXT("layout.hjson"); break;
			case ESemanticUnitKind::CollectionState:
				if (Unit.GetRole() == TEXT("property_definition")) OwnerFile = TEXT("definitions.hjson");
				break;
			default: break;
			}
			if (!OwnerFile.IsEmpty())
			{
				// All documents of an owner share its physical directory, including the
				// ordering and properties beside its content fragments. Logical identity is unchanged.
				const FString Path = ContentDirectories.FindChecked(Unit.GetUnitId()) / OwnerFile;
				PhysicalPlacements.Set(Unit, Path);
				RelocatedOwnerDocuments.Add(Unit.GetUnitId());
				continue;
			}
			if (Unit.GetUnitKind() != ESemanticUnitKind::StructureState || IsGraphStructureUnit(Unit)
				|| PhysicalPlacements.FindChecked(Unit).StartsWith(TEXT(".uai/"))) continue;
			bool bKeyedContent = Unit.GetRole() == TEXT("timeline_collection");
			for (const auto& Contract : GetKeyedItemProjectionContracts())
				bKeyedContent |= Unit.HasKeyedRoot(Contract.RootField);
			if (!bKeyedContent) continue;
			FString Name = Unit.GetRole();
			Name.RemoveFromEnd(TEXT("_collection"));
			const FString Stem = ContentDirectories.FindChecked(Unit.GetUnitId())
				/ StableProjectionName(MakeAuthoringContentPathSegment(Name), Unit.GetUnitId());
			FString Path = Stem + TEXT(".hjson");
			PhysicalPlacements.Set(Unit, MoveTemp(Path));
		}

		RelocationSeconds = FPlatformTime::Seconds() - PartStarted; PartStarted = FPlatformTime::Seconds();
		// Bound paths were installed during placement; relocation skipped them.
		// Several logical surfaces can share a real owner. Only property/definition
		// containers support multiple units in one document; other documents remain separate.
		// The final member list already needs these indices. Do not construct a
		// second all-unit pointer table merely to revisit inherited documents.
		TMap<FString, TArray<int32>> PhysicalGroups;
		TSet<FString> CollisionCandidates;
		for (const auto& Unit : Model->GetDeclarations())
		{
			const FString& Path = PhysicalPlacements.FindChecked(Unit);
			PhysicalGroups.FindOrAdd(Path).Add(Model->FindUnitIndex(Unit.GetUnitId()));
			if (RelocatedOwnerDocuments.Contains(Unit.GetUnitId()) && !BoundPhysicalPlacements.Contains(Unit))
				CollisionCandidates.Add(Path);
		}
		struct FPhysicalRelocation { FString Before; FString After; int32 UnitIndex; };
		TArray<FPhysicalRelocation> Relocations;
		TSet<FString> RelocationPaths;
		for (const FString& Candidate : CollisionCandidates)
		{
			const auto& Members = PhysicalGroups.FindChecked(Candidate);
			if (Members.Num() < 2) continue;
			const auto& First = Model->GetDeclarations()[Members[0]];
			const ESemanticUnitKind Kind = First.GetUnitKind();
			bool bSharedContainer = Kind == ESemanticUnitKind::PropertySet
				|| (Kind == ESemanticUnitKind::CollectionState && First.GetRole() == TEXT("property_definition"));
			for (const int32 Index : Members) {
				const auto& Unit = Model->GetDeclarations()[Index];
				bSharedContainer &= Unit.GetUnitKind() == Kind
					&& (Kind != ESemanticUnitKind::CollectionState || Unit.GetRole() == TEXT("property_definition"));
			}
			if (bSharedContainer) continue;
			for (const int32 Index : Members)
			{
				const auto& Unit = Model->GetDeclarations()[Index];
				if (!RelocatedOwnerDocuments.Contains(Unit.GetUnitId()) || BoundPhysicalPlacements.Contains(Unit)) continue;
				const FString Original = PhysicalPlacements.FindChecked(Unit);
				const FString Name = Unit.GetFieldSetRef().IsEmpty() ? Unit.GetRole() : Unit.GetFieldSetRef();
				const FString Stem = FPaths::GetPath(Original) / StableProjectionName(MakeAuthoringContentPathSegment(Name)
					+ TEXT("-") + FPaths::GetBaseFilename(Original), Unit.GetUnitId());
				FString Path = Stem + TEXT(".hjson");
				if (PhysicalGroups.Contains(Path) || RelocationPaths.Contains(Path))
					return Fail(OutError, TEXT("physical_path_collision"), TEXT("A stable document path conflicts with an accepted identity."), Path);
				RelocationPaths.Add(Path); Relocations.Add({Original, Path, Index});
				PhysicalPlacements.Set(Unit, MoveTemp(Path));
			}
		}
		for (const auto& Relocation : Relocations) {
			auto& Before = PhysicalGroups.FindChecked(Relocation.Before);
			Before.RemoveSingle(Relocation.UnitIndex);
			if (Before.IsEmpty()) PhysicalGroups.Remove(Relocation.Before);
			PhysicalGroups.FindOrAdd(Relocation.After).Add(Relocation.UnitIndex);
		}
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=physical_namespace_consumption complete_member_rows=%d candidate_documents=%d relocated_rows=%d duplicate_owner_table=false"),
			Model->GetUnitCount(), CollisionCandidates.Num(), Relocations.Num());

		CollisionSeconds = FPlatformTime::Seconds() - PartStarted; PartStarted = FPlatformTime::Seconds();
		if (SourceMode == EWorkspaceSourceMode::Directory)
		{
			PhysicalGroups.FindOrAdd(TEXT("index.hjson"));
			PhysicalGroups.FindOrAdd(ManifestPath);
		}
		else
		{
			PhysicalGroups.Reset();
			TArray<int32> UnitIds;
			for (int32 Index = 0; Index < Model->GetUnitCount(); ++Index) UnitIds.Add(Index);
			PhysicalGroups.Add(
				SourceMode == EWorkspaceSourceMode::SingleFile
					? TEXT("workspace.hjson")
					: TEXT("inline"),
				MoveTemp(UnitIds));
		}

		const double PlacementFreezeStarted = FPlatformTime::Seconds();
		int32 ReusedPlacementDocuments = 0, ReusedPlacementRows = 0, BuiltPlacementRows = 0;
		const int32 MaterializedPublicRefs = References.PublicRefs.Num();
		const auto PlacementStorage = PlacementBuilder.Freeze(PhysicalGroups, References.PublicRefs, References.bBorrowedAcceptedNames,
			ReusedPlacementDocuments, ReusedPlacementRows, BuiltPlacementRows);
		const double PlacementFreezeSeconds = FPlatformTime::Seconds() - PlacementFreezeStarted;
		const auto& PhysicalFiles = PlacementStorage->Files;
		const FWorkspacePlacementView FrozenLogicalPlacements(PlacementStorage, false);
		FMachineWorkspaceManifest Manifest;
		Manifest.WorkspaceSchema = WorkspaceSchema;
		Manifest.ContractId = Model->GetContractId();
		Manifest.ContractVersion = Model->GetContractVersion();
		Manifest.LayoutVersion = LayoutVersion;
		Manifest.ProfileId = Model->GetProfileId();
		Manifest.SemanticFingerprint =
			Model->GetSemanticFingerprint();
		Manifest.BaseRevision = Model->GetBaseRevision();
		Manifest.PublicRefMapping = FWorkspacePlacementView(PlacementStorage, FWorkspacePlacementView::EColumn::PublicRef);
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=layout_name_consumption borrowed=%s alias_rows=%d handle_rows=%d expanded_path_aliases=%d expanded_path_handles=%d public_refs=%d retained_handle_map=false"),
			References.bBorrowedAcceptedNames ? TEXT("true") : TEXT("false"), ReadableAliases.Num(), Handles.Num(),
			AliasLookup.NumExpanded(), HandleLookup.NumExpanded(), Manifest.PublicRefMapping.Num());
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=layout_reference_owner rows=%d materialized_names=%d reference_only_names=%d placement_index=compiled_owner"),
			Manifest.PublicRefMapping.Num(), MaterializedPublicRefs, PlacementStorage->ReferencedPublicRefs.Num());
		const auto& CurrentUnits = Model->GetDeclarations();
		Manifest.UnitInventory = FWorkspaceUnitInventoryView(Model);

		FString OutputMaterial;
		TSet<FString> ActualRows;
		if (AppliedLayoutChanges) for (const auto& Change : AppliedLayoutChanges->Changes) if (Change.After) ActualRows.Add(Change.UnitId);
		for (const auto& Declaration : CurrentUnits)
		{
			// This inventory is the layout-to-publication handoff. Its content
			// proof uses the exact public row grammar, not the stronger internal
			// typed/lexical material used by the independent semantic fingerprint.
			// Original accepted SHA is reusable only for this same immutable
			// complete unit. Placement, declaration and dependency projection are
			// still current above; this is not an edited-ID or old-native shortcut.
			if (!AppliedLayoutChanges || ActualRows.Contains(Declaration.GetUnitId())) {
				const auto* Unit = Model->FindUnit(Declaration.GetUnitId()); if (!Unit) { Model->CheckValueReadResult(OutError); return false; }
				if (Unit->GetPublicRowHash().IsEmpty()) {
					if (!TryEnsureSemanticUnitRowHash(*Unit, OutError, FString())) return false;
					++GeneratedRowHashes;
				} else ++OwnedRowHashReuses;
			} else ++OwnedRowHashReuses;
			if (Resources.CollectsUsage()) AppendToken(OutputMaterial, TEXT("s"), FrozenLogicalPlacements.FindChecked(Declaration.GetUnitId()));
		}
		if (Resources.CollectsUsage()) {
		const FTCHARToUTF8 OutputUtf8(
			*OutputMaterial,
			OutputMaterial.Len());
		FUeAgentResourceChargeError ResourceError;
		if (!Resources.TryChargeMany(
			{
				{EUeAgentResourceDimension::OutputBytes,
					OutputUtf8.Length()},
				{EUeAgentResourceDimension::StringBytes,
					OutputUtf8.Length()},
				{EUeAgentResourceDimension::Files,
					PhysicalFiles.Num()},
				{EUeAgentResourceDimension::WorkUnits,
					Model->GetUnitCount()
						+ PhysicalFiles.Num()},
				{EUeAgentResourceDimension::EstimatedMemoryBytes,
					static_cast<int64>(OutputUtf8.Length()) * 2},
			},
			TEXT("semantic_layout.plan"),
			ResourceError))
		{
			return FailResource(OutError, ResourceError);
		}
		}
		OutPlan = MakeShareable(new FWorkspaceLayoutPlan(
			MoveTemp(Model),
			LayoutVersion,
			SourceMode,
			PlacementStorage,
			MoveTemp(Manifest),
			MoveTemp(Telemetry), MoveTemp(DefinitionHints), ReferenceNamespace));
		// Keep the one complete successful result, sharing its immutable model
		// and projection with current consumers. This is not an additional copy
		// per unit or an unbounded history; reset it with the existing owner.
		const bool bCompletePublication = !OutPlan->GetModel()->IsOutputScope();
		if (bCompletePublication) { PreviousLayout = OutPlan; PreviousCharges.Reset(); }
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=layout_placement_storage documents=%d reused_documents=%d reused_rows=%d built_rows=%d index_rows=%d freeze_s=%.6f owner=physical_document shared_unit_ids=true"),
			PhysicalFiles.Num(), ReusedPlacementDocuments, ReusedPlacementRows, BuiltPlacementRows, PlacementStorage->Locations.Num(), PlacementFreezeSeconds);
		const auto UsageAfter = Resources.Snapshot();
		for (uint8 D = 0; D <= static_cast<uint8>(EUeAgentResourceDimension::EstimatedMemoryBytes); ++D) {
			const auto Dimension = static_cast<EUeAgentResourceDimension>(D);
			const int64 Delta = UsageAfter.Consumed(Dimension) - UsageBefore.Consumed(Dimension);
			if (bCompletePublication && Delta > 0) PreviousCharges.Add(Dimension, Delta);
		}
		InventorySeconds = FPlatformTime::Seconds() - PartStarted;
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=layout_owner_inputs logical_computed=%d logical_reused=%d physical_computed=%d physical_reused=%d graph_computed=%d graph_reused=%d alias_folded_inputs=%d context_inherited=%d context_compared=%d logical_namespace_inherited=%d logical_namespace_indexed=%d logical_collision_text_checks=%d"),
			LogicalOwnerBases.Num(), ReusedLogicalOwnerBases, PhysicalOwnerBases.Num(), ReusedPhysicalOwnerBases,
			GraphAncestorsByOwner.Num(), ReusedGraphAncestors, AllocatedReferenceNames, InheritedLayoutContexts, ComparedLayoutContexts,
			InheritedLogicalPaths, UnitPaths.Num(), LogicalCollisionTextChecks);
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=layout_membership_projection reused_names=%d allocated_names=%d complete_current_membership=%s measurement_material=%s"),
			ReusedReferenceNames, AllocatedReferenceNames, bCompletePublication ? TEXT("true") : TEXT("false"), Resources.CollectsUsage() ? TEXT("collected") : TEXT("not_requested"));
		return true;
	}

	bool IsModelVisibleSemanticWorkspacePath(const FString& RelativePath)
	{
		return !RelativePath.StartsWith(
			TEXT(".uai/"), ESearchCase::IgnoreCase);
	}
}

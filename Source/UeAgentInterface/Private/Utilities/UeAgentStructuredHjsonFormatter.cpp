#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "Utilities/UeAgentNativeNumberFormat.h"

#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Core/HjsonWorkspace/UeAgentHjsonPointerText.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"

#include "Containers/StringConv.h"
#include "Containers/ContainerAllocationPolicies.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include <charconv>
#include <cmath>
#include <cstring>
#include <string>

namespace
{
	static bool IsAsciiAlpha(const TCHAR Ch)
	{
		return (Ch >= TEXT('A') && Ch <= TEXT('Z')) || (Ch >= TEXT('a') && Ch <= TEXT('z'));
	}

	static bool IsAsciiDigit(const TCHAR Ch)
	{
		return Ch >= TEXT('0') && Ch <= TEXT('9');
	}

	static void NormalizeExponent(std::string& Text)
	{
		const size_t ExponentIndex = Text.find_first_of("eE");
		if (ExponentIndex == std::string::npos)
		{
			return;
		}
		Text[ExponentIndex] = 'e';
		size_t Digits = ExponentIndex + 1;
		bool bNegative = false;
		if (Digits < Text.size() && (Text[Digits] == '+' || Text[Digits] == '-'))
		{
			bNegative = Text[Digits] == '-';
			++Digits;
		}
		while (Digits + 1 < Text.size() && Text[Digits] == '0')
		{
			++Digits;
		}
		const std::string ExponentDigits = Text.substr(Digits);
		Text.resize(ExponentIndex + 1);
		if (bNegative)
		{
			Text.push_back('-');
		}
		Text += ExponentDigits.empty() ? "0" : ExponentDigits;
	}

	template<typename T>
	static bool TryFormatFiniteNative(
		const T Value,
		const bool bPreserveFloatKind,
		FString& OutText)
	{
		if (!std::isfinite(Value))
		{
			OutText.Reset();
			return false;
		}

		char Buffer[128];
		std::to_chars_result Result;
		const bool bNegativeZero = Value == 0.0 && std::signbit(Value);
		const bool bIntegralDouble = !bPreserveFloatKind && !bNegativeZero && std::trunc(Value) == Value;
		if (bIntegralDouble)
		{
			Result = std::to_chars(Buffer, Buffer + UE_ARRAY_COUNT(Buffer), Value, std::chars_format::fixed, 0);
		}
		else
		{
			Result = std::to_chars(Buffer, Buffer + UE_ARRAY_COUNT(Buffer), Value, std::chars_format::general);
		}
		if (Result.ec != std::errc())
		{
			OutText.Reset();
			return false;
		}

		std::string Canonical(Buffer, Result.ptr);
		NormalizeExponent(Canonical);
		if ((bPreserveFloatKind || bNegativeZero)
			&& Canonical.find('.') == std::string::npos
			&& Canonical.find('e') == std::string::npos)
		{
			Canonical += ".0";
		}
		OutText = UTF8_TO_TCHAR(Canonical.c_str());
		return true;
	}

	static bool IsDigitSequence(const FString& Text, int32 Start)
	{
		if (Start >= Text.Len())
		{
			return false;
		}
		for (int32 Index = Start; Index < Text.Len(); ++Index)
		{
			if (!IsAsciiDigit(Text[Index]))
			{
				return false;
			}
		}
		return true;
	}

	static bool IsExactIntegerToken(const FString& Token)
	{
		const int32 Start = Token.StartsWith(TEXT("-")) ? 1 : 0;
		if (!IsDigitSequence(Token, Start))
		{
			return false;
		}
		return Token.Len() - Start == 1 || Token[Start] != TEXT('0');
	}

	static bool IsFiniteFloatToken(const FString& Token)
	{
		int32 Index = Token.StartsWith(TEXT("-")) ? 1 : 0;
		bool bDigitsBeforeDecimal = false;
		while (Index < Token.Len() && IsAsciiDigit(Token[Index]))
		{
			bDigitsBeforeDecimal = true;
			++Index;
		}
		bool bHasDecimal = false;
		bool bDigitsAfterDecimal = false;
		if (Index < Token.Len() && Token[Index] == TEXT('.'))
		{
			bHasDecimal = true;
			++Index;
			while (Index < Token.Len() && IsAsciiDigit(Token[Index]))
			{
				bDigitsAfterDecimal = true;
				++Index;
			}
		}
		if (!bDigitsBeforeDecimal && !bDigitsAfterDecimal)
		{
			return false;
		}
		bool bHasExponent = false;
		if (Index < Token.Len() && (Token[Index] == TEXT('e') || Token[Index] == TEXT('E')))
		{
			bHasExponent = true;
			++Index;
			if (Index < Token.Len() && (Token[Index] == TEXT('+') || Token[Index] == TEXT('-')))
			{
				++Index;
			}
			const int32 ExponentStart = Index;
			while (Index < Token.Len() && IsAsciiDigit(Token[Index]))
			{
				++Index;
			}
			if (Index == ExponentStart)
			{
				return false;
			}
		}
		return Index == Token.Len() && (bHasDecimal || bHasExponent);
	}

	static bool TryCanonicalizeNumberToken(
		const FString& Token,
		FString& OutCanonical,
		bool& bOutNonFinite)
	{
		bOutNonFinite = false;
		const int32 Head = Token.Len() && (Token[0] == TEXT('+') || Token[0] == TEXT('-')) ? 1 : 0;
		if (Token.Len() > Head && (Token[Head] == TEXT('n') || Token[Head] == TEXT('N')
			|| Token[Head] == TEXT('i') || Token[Head] == TEXT('I'))
			&& (Token.Equals(TEXT("nan"), ESearchCase::IgnoreCase)
				|| Token.Equals(TEXT("infinity"), ESearchCase::IgnoreCase)
				|| Token.Equals(TEXT("+infinity"), ESearchCase::IgnoreCase)
				|| Token.Equals(TEXT("-infinity"), ESearchCase::IgnoreCase)
				|| Token.Equals(TEXT("inf"), ESearchCase::IgnoreCase)
				|| Token.Equals(TEXT("+inf"), ESearchCase::IgnoreCase)
				|| Token.Equals(TEXT("-inf"), ESearchCase::IgnoreCase)))
		{
			bOutNonFinite = true;
			return false;
		}
		if (IsExactIntegerToken(Token))
		{
			OutCanonical = Token == TEXT("-0") ? TEXT("0") : Token;
			return true;
		}

		if (!IsFiniteFloatToken(Token))
		{
			return false;
		}
		// Number-string values carry the exact authoring lexeme. Converting to
		// double here rounds edits before the native type owner can validate them.
		OutCanonical = Token;
		return true;
	}

	static FString Padding(const int32 Level)
	{
		return FString::ChrN(FMath::Max(0, Level * 2), TEXT(' '));
	}

	static int64 EscapedQuotedStringLength(const FString& Value, int64 StopAt = MAX_int64)
	{
		int64 Length = 2;
		for (const TCHAR Ch : Value)
		{
			switch (Ch)
			{
			case TEXT('"'):
			case TEXT('\\'):
			case TEXT('\b'):
			case TEXT('\f'):
			case TEXT('\n'):
			case TEXT('\r'):
			case TEXT('\t'):
				Length += 2;
				break;
			default:
				Length += Ch < 0x20 ? 6 : 1;
				break;
			}
			if (Length >= StopAt) return StopAt;
		}
		return Length;
	}

	static int64 EscapedJsonPointerSegmentLength(const FString& Segment)
	{
		int64 Length = Segment.Len();
		for (const TCHAR Ch : Segment)
		{
			if (Ch == TEXT('~') || Ch == TEXT('/'))
			{
				++Length;
			}
		}
		return Length;
	}

	static int64 AppendedJsonPointerLength(
		const FString& Parent,
		const FString& Segment)
	{
		return (Parent == TEXT("/") ? 1ll : Parent.Len() + 1ll)
			+ EscapedJsonPointerSegmentLength(Segment);
	}

	static FString AppendJsonPointer(const FString& Parent, const FString& Segment, int64 KnownLength = -1)
	{
		return UE::UAI::HjsonWorkspace::PointerText::Child(Parent, Segment,
			UE::UAI::HjsonWorkspace::PointerText::ENulMode::Omit, KnownLength);
	}

	// This is the existing field order, carrying the actual borrowed member rather
	// than searching its source object again for every analysis/render task.
	struct FOrderedObjectKey
	{
		const FString* Name;
		union {
			const FUeAgentStructuredHjsonObjectFieldView* Field = nullptr;
			const FUeAgentStructuredHjsonObjectView::FEntries::ElementType* Entry;
		};
		FOrderedObjectKey(const FString* InName, const FUeAgentStructuredHjsonObjectFieldView* InField = nullptr)
			: Name(InName), Field(InField) {}
		operator const FString*() const { return Name; }
		const FString& operator*() const { return *Name; }
		const FString* operator->() const { return Name; }
	};

	using FOrderedObjectKeys = TArray<FOrderedObjectKey, TInlineAllocator<16>>;
	struct FJsonKeyRefs : DefaultKeyFuncs<const FString*>
	{
		// Keep TSet<FString>'s original comparison/hash, borrowing only storage.
		static bool Matches(const FString* A, const FString* B) { return *A == *B; }
		static uint32 GetKeyHash(const FString* Key) { return GetTypeHash(*Key); }
	};

	// A borrowed view keeps both supported inputs on the same writer and accounting path.
	// It never materializes a second tree (FJsonObject cannot represent exact-case keys).
	class FWriterNodeView
	{
	public:
		using FCanonical = UE::UAI::StateSemantic::FUeAgentCanonicalValue;
		using FCanonicalSubtrees = TMap<const FJsonValue*, const FCanonical*>;
		using FBorrowedObject = FUeAgentStructuredHjsonObjectView;
		using FBorrowedField = FUeAgentStructuredHjsonObjectFieldView;
		using FBorrowedValues = FBorrowedField::FValues;
		enum class EBorrowed : uint8 { None, Object, Entries, Values, Text, Strings };
		FWriterNodeView() = default;
		FWriterNodeView(const TSharedPtr<FJsonValue>& Value) : FWriterNodeView(Value.Get()) {}
		FWriterNodeView(const TSharedRef<FJsonValue>& Value) : FWriterNodeView(&Value.Get()) {}
		FWriterNodeView(const TSharedPtr<FJsonObject>& Object) : FWriterNodeView(Object.Get()) {}
		FWriterNodeView(const TSharedRef<FJsonObject>& Object) : FWriterNodeView(&Object.Get()) {}
		FWriterNodeView(const FJsonValue* Value, const FCanonicalSubtrees* InSubtrees = nullptr)
			: Json(Value), Subtrees(InSubtrees), Type(Value ? Value->Type : EJson::None)
		{
			if (Value && Subtrees) {
				if (const FCanonical* const* Subtree = Subtrees->Find(Value); Subtree && *Subtree)
					*this = FWriterNodeView(**Subtree);
			}
		}
		FWriterNodeView(const FJsonObject* Object, const FCanonicalSubtrees* InSubtrees = nullptr)
			: JsonObject(Object), Subtrees(InSubtrees), Type(Object ? EJson::Object : EJson::None) {}
		FWriterNodeView(const FCanonical& Value) : Canonical(&Value)
		{
			using UE::UAI::StateSemantic::EUeAgentValueKind;
			switch (Value.GetKind())
			{
			case EUeAgentValueKind::Null: Type = EJson::Null; break;
			case EUeAgentValueKind::Boolean: Type = EJson::Boolean; break;
			case EUeAgentValueKind::String: Type = EJson::String; break;
			case EUeAgentValueKind::Integer:
			case EUeAgentValueKind::Decimal: Type = EJson::Number; break;
			case EUeAgentValueKind::Object: Type = EJson::Object; break;
			case EUeAgentValueKind::Array: Type = EJson::Array; break;
			default: break;
			}
		}
		FWriterNodeView(const FBorrowedObject& Object)
			: Borrowed(Object.Entries ? static_cast<const void*>(Object.Entries) : &Object),
			  BorrowedKind(Object.Entries ? EBorrowed::Entries : EBorrowed::Object), Type(EJson::Object) {}
		FWriterNodeView(const FBorrowedField& Field)
		{
			if (Field.Canonical) { *this = FWriterNodeView(*Field.Canonical); return; }
			if (Field.Object) { *this = FWriterNodeView(*Field.Object); return; }
			if (Field.Text) { Borrowed = Field.Text; BorrowedKind = EBorrowed::Text; Type = EJson::String; }
			else if (Field.Strings) { Borrowed = Field.Strings; BorrowedKind = EBorrowed::Strings; Type = EJson::Array; }
			else if (Field.Values) { Borrowed = Field.Values; BorrowedKind = EBorrowed::Values; Type = EJson::Object; }
		}
		static FWriterNodeView String(const FString& Text)
		{
			FWriterNodeView View; View.Borrowed = &Text; View.BorrowedKind = EBorrowed::Text; View.Type = EJson::String; return View;
		}
		bool IsBorrowed() const { return BorrowedKind != EBorrowed::None; }
		const FBorrowedObject& BorrowedObject() const { return *static_cast<const FBorrowedObject*>(Borrowed); }
		const FBorrowedObject::FEntries& BorrowedEntries() const { return *static_cast<const FBorrowedObject::FEntries*>(Borrowed); }
		const FBorrowedValues& BorrowedValues() const { return *static_cast<const FBorrowedValues*>(Borrowed); }
		const TArray<FString>& BorrowedStrings() const { return *static_cast<const TArray<FString>*>(Borrowed); }
		const FWriterNodeView* operator->() const { return this; }
		bool IsValid() const { return IsBorrowed() || Json || JsonObject || Canonical; }
		bool IsUnsupported() const { return Canonical && Type == EJson::None; }
		const void* Get() const { return IsBorrowed() ? Borrowed : Canonical ? static_cast<const void*>(Canonical) : JsonObject ? static_cast<const void*>(JsonObject) : Json; }
		FWriterNodeView AsObject() const { return IsBorrowed() || Canonical || JsonObject ? *this : FWriterNodeView(Json->AsObject().Get(), Subtrees); }
		const FString& AsString(FString& JsonStorage) const
		{
			if (BorrowedKind == EBorrowed::Text) return *static_cast<const FString*>(Borrowed);
			if (Canonical) return Canonical->GetText();
			JsonStorage = Json->AsString(); return JsonStorage;
		}
		bool AsBool() const { return Canonical ? Canonical->GetBoolean() : Json->AsBool(); }
		double AsNumber() const { return Json ? Json->AsNumber() : 0.0; }
		bool PreferStringRepresentation() const { return Canonical || (Json && Json->PreferStringRepresentation()); }
		bool TryGetString(FString& Out) const { if (Canonical) { Out = Canonical->GetLexeme(); return Type == EJson::Number; } return Json && Json->TryGetString(Out); }
		SIZE_T GetMemoryFootprint() const { return BorrowedKind == EBorrowed::Text ? sizeof(FString) + static_cast<const FString*>(Borrowed)->GetAllocatedSize() : Canonical ? sizeof(FCanonical) + Canonical->GetText().GetAllocatedSize() + Canonical->GetLexeme().GetAllocatedSize() : Json->GetMemoryFootprint(); }
		int32 NumFields() const { return BorrowedKind == EBorrowed::Object ? BorrowedObject().Fields.Num() : BorrowedKind == EBorrowed::Entries ? BorrowedEntries().Num() : BorrowedKind == EBorrowed::Values ? BorrowedValues().Num() : Canonical ? Canonical->GetObjectItems().Num() : JsonObject->Values.Num(); }
		void GetKeys(FOrderedObjectKeys& Out) const
		{
			if (BorrowedKind == EBorrowed::Object) { for (const auto& Field : BorrowedObject().Fields) Out.Emplace(Field.Key, &Field); }
			else if (BorrowedKind == EBorrowed::Entries) { for (const auto& Field : BorrowedEntries()) { Out.Emplace(&Field.Key); Out.Last().Entry = &Field; } }
			else if (BorrowedKind == EBorrowed::Values) { for (const auto& Field : BorrowedValues()) Out.Add(&Field.Key); }
			else if (Canonical) { for (const auto& Field : Canonical->GetObjectItems()) { Out.Add(&Field.Key); } }
			else { for (const auto& Field : JsonObject->Values) { Out.Add(&Field.Key); } }
		}
		FWriterNodeView FindField(const FString& Key) const
		{
			if (BorrowedKind == EBorrowed::Object) {
				for (const auto& Field : BorrowedObject().Fields)
					if (Field.Key->Equals(Key, ESearchCase::CaseSensitive)) return FWriterNodeView(Field);
				return {};
			}
			if (BorrowedKind == EBorrowed::Entries) {
				for (const auto& Field : BorrowedEntries())
					if (Field.Key.Equals(Key, ESearchCase::CaseSensitive)) return FWriterNodeView(Field.Value.Get());
				return {};
			}
			if (BorrowedKind == EBorrowed::Values) {
				const auto* Field = BorrowedValues().Find(Key);
				return Field ? FWriterNodeView(Field->Get()) : FWriterNodeView();
			}
			if (Canonical)
			{
				const auto& Fields = Canonical->GetObjectItems();
				int32 Begin = 0, End = Fields.Num();
				while (Begin < End)
				{
					const int32 Mid = Begin + (End - Begin) / 2;
					if (Fields[Mid].Key.Compare(Key, ESearchCase::CaseSensitive) < 0) Begin = Mid + 1;
					else End = Mid;
				}
				return Begin < Fields.Num() && Fields[Begin].Key.Equals(Key, ESearchCase::CaseSensitive)
					? FWriterNodeView(Fields[Begin].Value.Get()) : FWriterNodeView();
			}
			return FWriterNodeView(JsonObject->Values.FindRef(Key).Get(), Subtrees);
		}
		FWriterNodeView FindField(const FOrderedObjectKey& Key) const
		{
			// The original borrowed-member consumer already carries its actual
			// member. Sorting/reordering keeps that same pair until the join.
			// General canonical input retains its original key-lookup semantics.
			if (BorrowedKind == EBorrowed::Entries) return FWriterNodeView(Key.Entry->Value.Get());
			return Key.Field ? FWriterNodeView(*Key.Field) : FindField(*Key.Name);
		}
		bool HasField(const FString& Key) const { return FindField(Key).IsValid(); }
		struct FFields;
		struct FArray;
		FFields Fields() const;
		FArray AsArray() const;
		const FJsonValue* Json = nullptr;
		const FJsonObject* JsonObject = nullptr;
		const FCanonical* Canonical = nullptr;
		// JSON subtree lookup and a synchronous borrowed input are disjoint.
		// Reuse the pointer slot instead of enlarging every analysis/write task.
		union { const FCanonicalSubtrees* Subtrees = nullptr; const void* Borrowed; };
		EBorrowed BorrowedKind = EBorrowed::None;
		EJson Type = EJson::None;
	};

	struct FWriterNodeView::FFields
	{
		FWriterNodeView Object;
		struct FField { const FString& Key; FWriterNodeView Value; };
		struct FIterator
		{
			FWriterNodeView Object;
			int32 Index;
			TOptional<TMap<FString, TSharedPtr<FJsonValue>>::TConstIterator> JsonIterator;
			TOptional<FBorrowedValues::TConstIterator> ValuesIterator;
			FIterator(FWriterNodeView InObject, int32 InIndex) : Object(InObject), Index(InIndex)
			{
				if (Index < Object.NumFields()) {
					if (Object.BorrowedKind == EBorrowed::Values) ValuesIterator.Emplace(Object.BorrowedValues().CreateConstIterator());
					else if (!Object.IsBorrowed() && !Object.Canonical) JsonIterator.Emplace(Object.JsonObject->Values.CreateConstIterator());
				}
			}
			bool operator!=(const FIterator& Other) const { return Index != Other.Index; }
			void operator++() { ++Index; if (JsonIterator.IsSet()) { ++JsonIterator.GetValue(); } if (ValuesIterator.IsSet()) ++ValuesIterator.GetValue(); }
			FField operator*() const
			{
				if (Object.BorrowedKind == EBorrowed::Object) { const auto& Field = Object.BorrowedObject().Fields[Index]; return {*Field.Key, FWriterNodeView(Field)}; }
				if (Object.BorrowedKind == EBorrowed::Entries) { const auto& Field = Object.BorrowedEntries()[Index]; return {Field.Key, FWriterNodeView(Field.Value.Get())}; }
				if (Object.BorrowedKind == EBorrowed::Values) { const auto& Field = *ValuesIterator.GetValue(); return {Field.Key, FWriterNodeView(Field.Value.Get())}; }
				if (Object.Canonical) { const auto& Field = Object.Canonical->GetObjectItems()[Index]; return {Field.Key, FWriterNodeView(Field.Value.Get())}; }
				const auto& Field = *JsonIterator.GetValue(); return {Field.Key, FWriterNodeView(Field.Value.Get(), Object.Subtrees)};
			}
		};
		FIterator begin() const { return {Object, 0}; }
		FIterator end() const { return {Object, Object.NumFields()}; }
	};
	struct FWriterNodeView::FArray
	{
		FWriterNodeView Value;
		int32 Num() const { return Value.BorrowedKind == EBorrowed::Strings ? Value.BorrowedStrings().Num() : Value.Canonical ? Value.Canonical->GetArrayItems().Num() : Value.Json->AsArray().Num(); }
		FWriterNodeView operator[](int32 Index) const { return Value.BorrowedKind == EBorrowed::Strings ? FWriterNodeView::String(Value.BorrowedStrings()[Index]) : Value.Canonical ? FWriterNodeView(Value.Canonical->GetArrayItems()[Index].Get()) : FWriterNodeView(Value.Json->AsArray()[Index].Get(), Value.Subtrees); }
		struct FIterator
		{
			const FArray* Array;
			int32 Index;
			bool operator!=(const FIterator& Other) const { return Index != Other.Index; }
			void operator++() { ++Index; }
			FWriterNodeView operator*() const { return (*Array)[Index]; }
		};
		FIterator begin() const { return {this, 0}; }
		FIterator end() const { return {this, Num()}; }
	};
	FWriterNodeView::FFields FWriterNodeView::Fields() const { return {*this}; }
	FWriterNodeView::FArray FWriterNodeView::AsArray() const { return {*this}; }

	static bool IsScalarValue(const FWriterNodeView& Value)
	{
		return !Value.IsValid()
			|| Value->Type == EJson::None
			|| Value->Type == EJson::Null
			|| Value->Type == EJson::String
			|| Value->Type == EJson::Number
			|| Value->Type == EJson::Boolean;
	}

	static constexpr int32 InlineObjectFieldLimit = 6;
	static constexpr int32 InlineContainerCharacterLimit = 120;
	static constexpr int32 InlineContainerDepthLimit = 2;

	static bool HasExactKeySet(TConstArrayView<FOrderedObjectKey> Keys, const TArray<FString>& ExpectedKeys)
	{
		if (Keys.Num() != ExpectedKeys.Num())
		{
			return false;
		}

		for (const FString& ExpectedKey : ExpectedKeys)
		{
			if (!Keys.ContainsByPredicate([&](const FString* Key) { return Key->Equals(ExpectedKey, ESearchCase::CaseSensitive); }))
			{
				return false;
			}
		}
		return true;
	}

	static bool TryGetTupleKeyOrder(TConstArrayView<FOrderedObjectKey> Keys, FOrderedObjectKeys& OutKeys)
	{
		static const TArray<FString> Pos2DKeys = { TEXT("x"), TEXT("y") };
		static const TArray<FString> Pos3DKeys = { TEXT("x"), TEXT("y"), TEXT("z") };
		static const TArray<FString> RotationKeys = { TEXT("pitch"), TEXT("yaw"), TEXT("roll") };
		static const TArray<FString> ColorKeys = { TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a") };
		const auto Assign = [&](const TArray<FString>& Order) {
			for (const FString& Expected : Order)
				OutKeys.Add(*Keys.FindByPredicate([&](const FString* Key) { return Key->Equals(Expected, ESearchCase::CaseSensitive); }));
		};

		if (HasExactKeySet(Keys, Pos2DKeys))
		{
			Assign(Pos2DKeys);
			return true;
		}
		if (HasExactKeySet(Keys, Pos3DKeys))
		{
			Assign(Pos3DKeys);
			return true;
		}
		if (HasExactKeySet(Keys, RotationKeys))
		{
			Assign(RotationKeys);
			return true;
		}
		if (HasExactKeySet(Keys, ColorKeys))
		{
			Assign(ColorKeys);
			return true;
		}
		return false;
	}

	struct FCanonicalFragmentProof
	{
		// The caller owns the immutable node. A fragment must not keep a whole
		// canonical subtree alive merely to prove pointer identity next time.
		TWeakPtr<const UE::UAI::StateSemantic::FUeAgentCanonicalValue> Value;
		FString Text;
		int32 Level = 0;
		FString FieldName;
		int32 InlineCount = 0;
		int32 InlineLength = 0;
		int32 InlineDepth = 0;
		bool bNonInline = false;
		FUeAgentStructuredResourceUsage AnalysisCharges;
		FUeAgentStructuredResourceUsage RenderCharges;
	};
	struct FCanonicalFragmentCache
	{
		FUeAgentStructuredHjsonWriteOptions::FStableLeafProjection Projection = nullptr;
		TMap<FString, TArray<FString>> KeyOrder;
		TMap<FString, TSharedPtr<const FCanonicalFragmentProof>> Fragments;
		int64 TextChars = 0;
	};
	struct FCanonicalFragmentRegistry
	{
		TMap<FString, TSharedPtr<const FCanonicalFragmentCache>> Documents;
		int32 FragmentCount = 0;
		int64 TextChars = 0;
	};
	static thread_local FCanonicalFragmentRegistry PreviousCanonicalFragments;

	// A digest visitor has no resource-usage consumer. Normal writers retain the
	// original tracker, including limits/checkpoints and fragment replay charges.
	struct FWriterResourceUsage
	{
		FUeAgentStructuredResourceTracker* Tracker = nullptr;
		template<typename... A> bool ChargeInputBytes(A&&... Args) const { return !Tracker || Tracker->ChargeInputBytes(Forward<A>(Args)...); }
		template<typename... A> bool ChargeNodes(A&&... Args) const { return !Tracker || Tracker->ChargeNodes(Forward<A>(Args)...); }
		template<typename... A> bool ChargeItems(A&&... Args) const { return !Tracker || Tracker->ChargeItems(Forward<A>(Args)...); }
		template<typename... A> bool ChargeWorkUnits(A&&... Args) const { return !Tracker || Tracker->ChargeWorkUnits(Forward<A>(Args)...); }
		template<typename... A> bool ChargeOutputChars(A&&... Args) const { return !Tracker || Tracker->ChargeOutputChars(Forward<A>(Args)...); }
		template<typename... A> bool ChargeEstimatedMemoryBytes(A&&... Args) const { return !Tracker || Tracker->ChargeEstimatedMemoryBytes(Forward<A>(Args)...); }
		template<typename... A> bool Checkpoint(A&&... Args) const { return !Tracker || Tracker->Checkpoint(Forward<A>(Args)...); }
		FUeAgentStructuredResourceUsage GetUsage() const { return Tracker ? Tracker->GetUsage() : FUeAgentStructuredResourceUsage{}; }
	};

	class FHjsonWriter
	{
		struct FContainerAnalysis
		{
			int32 Count = 0, Length = 0, Depth = 0;
			bool bHasCount = false, bHasDimensions = false, bHasKeys = false, bNonInline = false;
		};
		struct FObjectAnalysis : FContainerAnalysis
		{
			FOrderedObjectKeys Keys;
		};
	public:
		explicit FHjsonWriter(
			const FUeAgentStructuredHjsonWriteOptions* InOptions = nullptr,
			FUeAgentStructuredResourceTracker* InResourceTracker = nullptr,
			bool bTrackResourceUsage = true)
			: Options(InOptions)
			, OwnedResourceTracker(
				InResourceTracker || !bTrackResourceUsage
					? nullptr
					: MakeUnique<FUeAgentStructuredResourceTracker>())
			, ResourceTracker{InResourceTracker ? InResourceTracker : OwnedResourceTracker.Get()}
		{
		}

		bool TryWriteRootObject(
			FWriterNodeView Object,
			FString& OutText,
			UE::UAI::Contract::FDiagnostic& OutError) const
		{
			if (Options && !Options->CanonicalSubtrees.IsEmpty() && !Object.Canonical && !Object.IsBorrowed())
				Object = Object.JsonObject ? FWriterNodeView(Object.JsonObject, &Options->CanonicalSubtrees)
					: FWriterNodeView(Object.Json, &Options->CanonicalSubtrees);
			Error.Reset();
			if (Object.Type != EJson::Object)
			{
				OutText.Reset();
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("hjson_root_not_object"), TEXT("The HJSON root must be an object."), TEXT("hjson_write"));
				return false;
			}
			BeginFragmentReuse(Object, true);
			if (!AnalyzeRootObject(Object))
			{
				OutText.Reset();
				OutError = Error;
				return false;
			}
			if (Object->NumFields() == 0)
			{
				if (!ResourceTracker.ChargeWorkUnits(
						2,
						TEXT("/"),
						Error)
					|| !ResourceTracker.ChargeOutputChars(
						2,
						TEXT("/"),
						Error)
					|| !ResourceTracker.ChargeEstimatedMemoryBytes(
						2 * sizeof(TCHAR),
						TEXT("/"),
						Error)
					|| !ResourceTracker.Checkpoint(TEXT("/"), Error))
				{
					OutText.Reset();
					OutError = Error;
					return false;
				}
				OutText = TEXT("{}");
				OutError.Reset();
				return true;
			}
			FWriteStack Tasks;
			Tasks.Add(FWriteTask::ObjectBody(Object, 0, TEXT("/")));
			if (!RenderChecked(Tasks, OutText))
			{
				OutText.Reset();
				OutError = Error;
				return false;
			}
			if (Error.IsEmpty())
			{
				ResourceTracker.Checkpoint(TEXT("/"), Error);
			}
			OutError = Error;
			if (!Error.IsEmpty())
			{
				OutText.Reset();
				return false;
			}
			CommitFragments();
			return true;
		}

		bool TryWriteValue(
			FWriterNodeView Value,
			FString& OutText,
			UE::UAI::Contract::FDiagnostic& OutError) const
		{
			if (Options && !Options->CanonicalSubtrees.IsEmpty() && !Value.Canonical && !Value.IsBorrowed())
				Value = Value.JsonObject ? FWriterNodeView(Value.JsonObject, &Options->CanonicalSubtrees)
					: FWriterNodeView(Value.Json, &Options->CanonicalSubtrees);
			Error.Reset();
			BeginFragmentReuse(Value, false);
			if (!AnalyzeRootValue(Value))
			{
				OutText.Reset();
				OutError = Error;
				return false;
			}
			FWriteStack Tasks;
			Tasks.Add(FWriteTask::ValueTask(Value, 0, FString(), TEXT("/")));
			if (!RenderChecked(Tasks, OutText))
			{
				OutText.Reset();
				OutError = Error;
				return false;
			}
			if (Error.IsEmpty())
			{
				ResourceTracker.Checkpoint(TEXT("/"), Error);
			}
			OutError = Error;
			if (!Error.IsEmpty())
			{
				OutText.Reset();
				return false;
			}
			CommitFragments();
			return true;
		}

		bool TryVisitValueText(FWriterNodeView Value, TFunctionRef<void(FStringView)> Consumer,
			UE::UAI::Contract::FDiagnostic& OutError) const
		{
			if (Options && !Options->CanonicalSubtrees.IsEmpty() && !Value.Canonical && !Value.IsBorrowed())
				Value = Value.JsonObject ? FWriterNodeView(Value.JsonObject, &Options->CanonicalSubtrees)
					: FWriterNodeView(Value.Json, &Options->CanonicalSubtrees);
			Error.Reset();
			// Digest consumers do not retain fragment text. They use the same
			// analysis/tasks/render and diagnostics, without a fragment archive.
			if (!AnalyzeRootValue(Value)) { OutError = Error; return false; }
			FWriteStack Tasks;
			Tasks.Add(FWriteTask::ValueTask(Value, 0, FString(), TEXT("/")));
			FString Unused;
			const bool bWritten = RenderChecked(Tasks, Unused, &Consumer);
			OutError = Error;
			return bWritten;
		}

	private:
		struct FPendingFragment
		{
			TSharedPtr<FCanonicalFragmentProof> Proof;
			FUeAgentStructuredResourceUsage AnalysisBefore;
			FUeAgentStructuredResourceUsage RenderBefore;
			int32 OutputStart = 0;
		};

		static bool UsageDelta(const FUeAgentStructuredResourceUsage& Before,
			const FUeAgentStructuredResourceUsage& After, FUeAgentStructuredResourceUsage& Delta)
		{
			const auto Copy = [](int64 A, int64 B, int64& Out) {
				if (B == MAX_int64 || B < A) return false;
				Out = B - A; return true;
			};
			return Copy(Before.InputBytes, After.InputBytes, Delta.InputBytes)
				&& Copy(Before.Nodes, After.Nodes, Delta.Nodes)
				&& Copy(Before.Items, After.Items, Delta.Items)
				&& Copy(Before.WorkUnits, After.WorkUnits, Delta.WorkUnits)
				&& Copy(Before.OutputChars, After.OutputChars, Delta.OutputChars)
				&& Copy(Before.EstimatedMemoryBytes, After.EstimatedMemoryBytes, Delta.EstimatedMemoryBytes);
		}

		bool ReplayUsage(const FUeAgentStructuredResourceUsage& Usage, const FString& Pointer) const
		{
			return ResourceTracker.ChargeInputBytes(Usage.InputBytes, Pointer, Error)
				&& ResourceTracker.ChargeNodes(Usage.Nodes, Pointer, Error)
				&& ResourceTracker.ChargeItems(Usage.Items, Pointer, Error)
				&& ResourceTracker.ChargeWorkUnits(Usage.WorkUnits, Pointer, Error)
				&& ResourceTracker.ChargeOutputChars(Usage.OutputChars, Pointer, Error)
				&& ResourceTracker.ChargeEstimatedMemoryBytes(Usage.EstimatedMemoryBytes, Pointer, Error)
				&& ResourceTracker.Checkpoint(Pointer, Error);
		}

		void BeginFragmentReuse(const FWriterNodeView& Root, bool bRootObject) const
		{
			if (!Root.Canonical || Root.Type != EJson::Object || (Options && Options->CanonicalLeafProjection)) return;
			const auto& RootFields = Root.Canonical->GetObjectItems();
			bool bObjectFields = RootFields.Num() >= 32 && RootFields.Num() > InlineObjectFieldLimit;
			for (const auto& Field : RootFields)
				if (Field.Value->GetKind() != UE::UAI::StateSemantic::EUeAgentValueKind::Object) bObjectFields = false;
			CandidateLevel = bObjectFields ? (bRootObject ? 0 : 1) : (bRootObject ? 1 : 2);
			if (bObjectFields)
			{
				// The field count alone proves this parent cannot become inline.
				// Baseline envelopes are direct object fields rather than array rows.
				for (const auto& Field : RootFields)
				{
					if (FragmentCandidates.Num() >= 200000) break;
					const FString Pointer = AppendJsonPointer(TEXT("/"), Field.Key);
					FragmentCandidates.Add(Pointer, Field.Value);
					FragmentFieldNames.Add(Pointer, Field.Key);
				}
			}
			else for (const auto& Field : RootFields)
			{
				if (Field.Value->GetKind() != UE::UAI::StateSemantic::EUeAgentValueKind::Array) continue;
				const auto& Items = Field.Value->GetArrayItems();
				// A direct array of objects is always block-form in this writer.
				// Its parent root consequently cannot become inline either.
				if (Items.Num() < 32 || Items.Num() + FragmentCandidates.Num() > 200000) continue;
				bool bObjects = true;
				for (const auto& Item : Items)
					if (Item->GetKind() != UE::UAI::StateSemantic::EUeAgentValueKind::Object) { bObjects = false; break; }
				if (!bObjects) continue;
				const FString Parent = AppendJsonPointer(TEXT("/"), Field.Key);
				for (int32 Index = 0; Index < Items.Num(); ++Index)
					FragmentCandidates.Add(AppendJsonPointer(Parent, FString::FromInt(Index)), Items[Index]);
			}
			if (FragmentCandidates.IsEmpty()) return;
			NextFragments = MakeShared<FCanonicalFragmentCache>();
			NextFragments->Projection = Options ? Options->StableCanonicalLeafProjection : nullptr;
			if (Options) NextFragments->KeyOrder = Options->PreferredKeyOrderByJsonPointer;
			FragmentContext = Options ? Options->CanonicalFragmentContext : FString();
			const auto* Previous = PreviousCanonicalFragments.Documents.Find(FragmentContext);
			if (Previous && (*Previous)->Projection == NextFragments->Projection
				&& (*Previous)->KeyOrder.OrderIndependentCompareEqual(NextFragments->KeyOrder))
				PriorFragments = *Previous;
		}

		void KeepFragment(const FString& Pointer, const TSharedPtr<const FCanonicalFragmentProof>& Proof) const
		{
			if (NextFragments && CachedFragmentChars + Proof->Text.Len() <= 256LL * 1024 * 1024)
			{
				CachedFragmentChars += Proof->Text.Len();
				NextFragments->Fragments.Add(Pointer, Proof);
			}
		}

		void CommitFragments() const
		{
			if (!NextFragments) return;
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=hjson_fragment_format candidates=%d reused=%d retained=%d"),
				FragmentCandidates.Num(), ReusedFragmentCount, NextFragments->Fragments.Num());
			NextFragments->TextChars = CachedFragmentChars;
			if (const auto* Previous = PreviousCanonicalFragments.Documents.Find(FragmentContext))
			{
				PreviousCanonicalFragments.FragmentCount -= (*Previous)->Fragments.Num();
				PreviousCanonicalFragments.TextChars -= (*Previous)->TextChars;
				PreviousCanonicalFragments.Documents.Remove(FragmentContext);
			}
			// Keep the total previous proof set bounded. Do not evict a sequential
			// shard on every new shard: that would make every next export miss.
			if (PreviousCanonicalFragments.Documents.Num() < 512
				&& PreviousCanonicalFragments.FragmentCount + NextFragments->Fragments.Num() <= 200000
				&& PreviousCanonicalFragments.TextChars + CachedFragmentChars <= 256LL * 1024 * 1024)
			{
				PreviousCanonicalFragments.FragmentCount += NextFragments->Fragments.Num();
				PreviousCanonicalFragments.TextChars += CachedFragmentChars;
				PreviousCanonicalFragments.Documents.Add(FragmentContext, MoveTemp(NextFragments));
			}
		}

		enum class EWriteTaskType : uint8
		{
			Text,
			LiteralText,
			Value,
			ObjectBody,
			FragmentExit,
			SpanExit,
		};

		struct FWriteTask
		{
			EWriteTaskType Type = EWriteTaskType::Text;
			FString Text;
			FWriterNodeView Value;
			FWriterNodeView Object;
			int32 Level = 0;
			FString FieldName;
			FString JsonPointer;
			bool bInlineContext = false;
			bool bTextResourcesPrecharged = false;
			int32 SpanStart = 0;

			static FWriteTask TextTask(
				FString InText,
				FString InJsonPointer,
				const bool bInTextResourcesPrecharged = false)
			{
				FWriteTask Task;
				Task.Type = EWriteTaskType::Text;
				Task.Text = MoveTemp(InText);
				Task.JsonPointer = MoveTemp(InJsonPointer);
				Task.bTextResourcesPrecharged =
					bInTextResourcesPrecharged;
				return Task;
			}
			// Only module-lifetime literals use this constructor. Text tasks do
			// not otherwise consume Value/Level, so keep the view in those slots
			// rather than enlarging every task or allocating a literal FString.
			static FWriteTask LiteralTask(FStringView InText, FString InJsonPointer)
			{
				FWriteTask Task;
				Task.Type = EWriteTaskType::LiteralText;
				Task.Value.Borrowed = InText.GetData();
				Task.Level = InText.Len();
				Task.JsonPointer = MoveTemp(InJsonPointer);
				return Task;
			}
			FStringView GetText() const
			{
				return Type == EWriteTaskType::LiteralText
					? FStringView(static_cast<const TCHAR*>(Value.Borrowed), Level) : FStringView(Text);
			}

			static FWriteTask ValueTask(
				FWriterNodeView InValue,
				const int32 InLevel,
				FString InFieldName,
				FString InJsonPointer,
				const bool bInInlineContext = false)
			{
				FWriteTask Task;
				Task.Type = EWriteTaskType::Value;
				Task.Value = MoveTemp(InValue);
				Task.Level = InLevel;
				Task.FieldName = MoveTemp(InFieldName);
				Task.JsonPointer = MoveTemp(InJsonPointer);
				Task.bInlineContext = bInInlineContext;
				return Task;
			}

			static FWriteTask ObjectBody(
				FWriterNodeView InObject,
				const int32 InLevel,
				FString InJsonPointer)
			{
				FWriteTask Task;
				Task.Type = EWriteTaskType::ObjectBody;
				Task.Object = MoveTemp(InObject);
				Task.Level = InLevel;
				Task.JsonPointer = MoveTemp(InJsonPointer);
				return Task;
			}
		};

		enum class EAnalysisTaskType : uint8
		{
			ValueEnter,
			ObjectEnter,
			ObjectExit,
			ArrayEnter,
			ArrayExit,
			FragmentExit,
		};

		struct FAnalysisTask
		{
			EAnalysisTaskType Type = EAnalysisTaskType::ValueEnter;
			FWriterNodeView Value;
			FWriterNodeView Object;
			FString JsonPointer = TEXT("/");

			static FAnalysisTask ValueEnter(
				FWriterNodeView InValue,
				FString InJsonPointer)
			{
				FAnalysisTask Task;
				Task.Type = EAnalysisTaskType::ValueEnter;
				Task.Value = MoveTemp(InValue);
				Task.JsonPointer = MoveTemp(InJsonPointer);
				return Task;
			}

			static FAnalysisTask ObjectTask(
				const EAnalysisTaskType InType,
				FWriterNodeView InObject,
				FString InJsonPointer)
			{
				FAnalysisTask Task;
				Task.Type = InType;
				Task.Object = MoveTemp(InObject);
				Task.JsonPointer = MoveTemp(InJsonPointer);
				return Task;
			}

			static FAnalysisTask ArrayTask(
				const EAnalysisTaskType InType,
				FWriterNodeView InValue,
				FString InJsonPointer)
			{
				FAnalysisTask Task;
				Task.Type = InType;
				Task.Value = MoveTemp(InValue);
				Task.JsonPointer = MoveTemp(InJsonPointer);
				return Task;
			}
		};

		using FWriteStack = TArray<FWriteTask, TInlineAllocator<64>>;
		using FAnalysisStack = TArray<FAnalysisTask, TInlineAllocator<32>>;

		bool AnalyzeRootObject(const FWriterNodeView& Object) const
		{
			ResetAnalysis();
			FAnalysisStack Stack;
			Stack.Add(FAnalysisTask::ObjectTask(
				EAnalysisTaskType::ObjectEnter,
				Object,
				TEXT("/")));
			return AnalyzeTasks(Stack);
		}

		bool AnalyzeRootValue(const FWriterNodeView& Value) const
		{
			ResetAnalysis();
			FAnalysisStack Stack;
			Stack.Add(FAnalysisTask::ValueEnter(Value, TEXT("/")));
			return AnalyzeTasks(Stack);
		}

		void ResetAnalysis() const
		{
			ObjectAnalysis.Reset();
			ArrayAnalysis.Reset();
			FormattedNumbers.Reset();
			VisitingObjects.Reset();
			VisitingArrays.Reset();
		}

		bool TryAppendTrackedJsonPointer(
			const FString& Parent,
			const FString& Segment,
			const int32 EstimatedCopies,
			const bool bChargeResources,
			FString& OutPointer) const
		{
			const int64 PointerChars = ResourceTracker.Tracker
				? AppendedJsonPointerLength(Parent, Segment) : 0;
			const int64 CopyCount =
				FMath::Max<int64>(1, EstimatedCopies);
			if (bChargeResources
				&& (!ResourceTracker.ChargeWorkUnits(
						PointerChars * CopyCount,
						Parent,
						Error)
					|| !ResourceTracker.ChargeEstimatedMemoryBytes(
						PointerChars * CopyCount * sizeof(TCHAR),
						Parent,
						Error)))
			{
				OutPointer.Reset();
				return false;
			}
			OutPointer = AppendJsonPointer(Parent, Segment, ResourceTracker.Tracker ? PointerChars : -1);
			return true;
		}

		bool ChargeCanonicalTextResources(
			const int64 CanonicalChars,
			const FString& JsonPointer) const
		{
			return ResourceTracker.ChargeWorkUnits(
					FMath::Max<int64>(1, CanonicalChars),
					JsonPointer,
					Error)
				&& ResourceTracker.ChargeOutputChars(
					CanonicalChars,
					JsonPointer,
					Error)
				&& ResourceTracker.ChargeEstimatedMemoryBytes(
					CanonicalChars * sizeof(TCHAR),
					JsonPointer,
					Error);
		}

		bool TryFormatKey(
			const FString& Key,
			const FString& JsonPointer,
			const bool bPreflight,
			FWriteTask& OutTask) const
		{
			const bool bBare =
				FUeAgentStructuredHjsonFormatter::IsBareKey(Key);
			FString CanonicalText = bBare
				? Key
				: FUeAgentStructuredHjsonFormatter::EscapeQuotedString(Key);
			OutTask = FWriteTask::TextTask(
				MoveTemp(CanonicalText),
				JsonPointer,
				bPreflight);
			return true;
		}

		bool TryMakeStringValueTask(
			const FString& StringValue,
			const FString& JsonPointer,
			const bool bInlineContext,
			const bool bPreflight,
			FWriteTask& OutTask) const
		{
			const bool bBare =
				!bInlineContext && FUeAgentStructuredHjsonFormatter::IsBareString(StringValue);
			FString CanonicalText = bBare
				? StringValue
				: FUeAgentStructuredHjsonFormatter::EscapeQuotedString(StringValue);
			const int64 CanonicalChars = CanonicalText.Len();
			if (bPreflight && !ChargeCanonicalTextResources(CanonicalChars, JsonPointer))
			{
				return false;
			}
			OutTask = FWriteTask::TextTask(
				MoveTemp(CanonicalText),
				JsonPointer,
				bPreflight);
			return true;
		}

		bool AnalyzeTasks(FAnalysisStack& Stack) const
		{
			while (Stack.Num() > 0 && Error.IsEmpty())
			{
				FAnalysisTask Task = Stack.Pop(EAllowShrinking::No);
				if (Task.Type == EAnalysisTaskType::FragmentExit)
				{
					auto* Pending = PendingFragments.Find(Task.JsonPointer);
					const auto* Summary = ObjectAnalysis.Find(Task.Value.Get());
					if (Pending && Summary && (Summary->bNonInline || (Summary->bHasCount && Summary->bHasDimensions))
						&& UsageDelta(Pending->AnalysisBefore, ResourceTracker.GetUsage(), Pending->Proof->AnalysisCharges))
					{
						Pending->Proof->InlineCount = Summary->Count;
						Pending->Proof->InlineLength = Summary->Length;
						Pending->Proof->InlineDepth = Summary->Depth;
						Pending->Proof->bNonInline = Summary->bNonInline;
					}
					else PendingFragments.Remove(Task.JsonPointer);
					continue;
				}
				if (Task.Type == EAnalysisTaskType::ValueEnter)
				{
					if (const auto* Candidate = FragmentCandidates.Find(Task.JsonPointer);
						Candidate && Candidate->Get() == Task.Value.Canonical)
					{
						const auto* Prior = PriorFragments ? PriorFragments->Fragments.Find(Task.JsonPointer) : nullptr;
						if (Prior && (*Prior)->Value.Pin().Get() == Task.Value.Canonical && (*Prior)->Level == CandidateLevel
							&& (*Prior)->FieldName == FragmentFieldNames.FindRef(Task.JsonPointer))
						{
							if (!ReplayUsage((*Prior)->AnalysisCharges, Task.JsonPointer)) return false;
							auto& Summary = ObjectAnalysis.FindOrAdd(Task.Value.Get());
							Summary.Count = (*Prior)->InlineCount;
							Summary.Length = (*Prior)->InlineLength;
							Summary.Depth = (*Prior)->InlineDepth;
							Summary.bNonInline = (*Prior)->bNonInline;
							Summary.bHasCount = true; Summary.bHasDimensions = !Summary.bNonInline;
							AnalyzedFragments.Add(Task.JsonPointer, *Prior);
							continue;
						}
						FPendingFragment Pending;
						Pending.Proof = MakeShared<FCanonicalFragmentProof>();
						Pending.Proof->Value = *Candidate;
						Pending.Proof->Level = CandidateLevel;
						Pending.Proof->FieldName = FragmentFieldNames.FindRef(Task.JsonPointer);
						Pending.AnalysisBefore = ResourceTracker.GetUsage();
						PendingFragments.Add(Task.JsonPointer, MoveTemp(Pending));
						FAnalysisTask Exit = Task;
						Exit.Type = EAnalysisTaskType::FragmentExit;
						Stack.Add(MoveTemp(Exit));
					}
				}
				switch (Task.Type)
				{
				case EAnalysisTaskType::ValueEnter:
					if (!AnalyzeValue(Stack, Task.Value, Task.JsonPointer))
					{
						return false;
					}
					break;
				case EAnalysisTaskType::ObjectEnter:
					if (!AnalyzeObjectEnter(Stack, Task.Object, Task.JsonPointer))
					{
						return false;
					}
					break;
				case EAnalysisTaskType::ObjectExit:
					AnalyzeObjectExit(Task.Object, Task.JsonPointer);
					break;
				case EAnalysisTaskType::ArrayEnter:
					if (!AnalyzeArrayEnter(Stack, Task.Value, Task.JsonPointer))
					{
						return false;
					}
					break;
				case EAnalysisTaskType::ArrayExit:
					AnalyzeArrayExit(Task.Value, Task.JsonPointer);
					break;
				case EAnalysisTaskType::FragmentExit:
					break; // Consumed before dispatch.
				}
			}
			return Error.IsEmpty()
				&& ResourceTracker.Checkpoint(TEXT("/"), Error);
		}

		FWriterNodeView ProjectedLeaf(const FWriterNodeView& Value) const
		{
			if (Value.Canonical)
				if (const auto* Found = LeafProjections.Find(Value.Canonical)) return *Found;
			return Value;
		}

		bool AnalyzeValue(
			FAnalysisStack& Stack,
			const FWriterNodeView& Input,
			const FString& JsonPointer) const
		{
			if (Input.Canonical && Options && (Options->CanonicalLeafProjection || Options->StableCanonicalLeafProjection)
				&& !LeafProjections.Contains(Input.Canonical))
			{
				TSharedPtr<FJsonValue> Projected;
				UE::UAI::Contract::FDiagnostic ProjectionError;
				const auto ChargeProjection = [&](int64 Bytes) {
					return ResourceTracker.ChargeEstimatedMemoryBytes(Bytes, JsonPointer, Error)
						&& ResourceTracker.ChargeWorkUnits(1, JsonPointer, Error)
						&& ResourceTracker.Checkpoint(JsonPointer, Error);
				};
				const bool bProjected = Options->CanonicalLeafProjection
					? Options->CanonicalLeafProjection(*Input.Canonical, ChargeProjection, Projected, ProjectionError)
					: Options->StableCanonicalLeafProjection(*Input.Canonical, ChargeProjection, Projected, ProjectionError);
				if (!bProjected)
				{
					if (Error.IsEmpty())
					{
						Error = ProjectionError;
						if (Error.IsEmpty()) SetWriteError(TEXT("hjson_leaf_projection_failed"), JsonPointer, TEXT("The canonical leaf projection failed without a diagnostic."));
						if (Error.Source.JsonPath.IsEmpty()) Error.Source.JsonPath = JsonPointer;
					}
					return false;
				}
				if (Projected.IsValid()) LeafProjections.Add(Input.Canonical, Projected);
			}
			const FWriterNodeView Value = ProjectedLeaf(Input);
			if (Value.IsUnsupported())
			{
				SetWriteError(TEXT("hjson_value_kind_unsupported"), JsonPointer, TEXT("canonical value is not an HJSON value"));
				return false;
			}
			if (Value.IsValid() && Value->Type == EJson::Object)
			{
				const FWriterNodeView Object = Value->AsObject();
				if (Object.IsValid())
				{
					Stack.Add(FAnalysisTask::ObjectTask(
						EAnalysisTaskType::ObjectEnter,
						Object,
						JsonPointer));
					return true;
				}
			}
			if (Value.IsValid() && Value->Type == EJson::Array)
			{
				Stack.Add(FAnalysisTask::ArrayTask(
					EAnalysisTaskType::ArrayEnter,
					Value,
					JsonPointer));
				return true;
			}

			if (!ResourceTracker.Tracker) return true; // Storage usage has no digest consumer.
			int64 DynamicChars = 0;
			bool bValueMemoryPrecharged = false;
			if (Value.IsValid() && Value->Type == EJson::String)
			{
				const uint64 MemoryFootprint =
					static_cast<uint64>(Value->GetMemoryFootprint());
				const int64 CopyAwareMemoryBytes =
					MemoryFootprint > static_cast<uint64>(MAX_int64 / 2)
					? MAX_int64
					: static_cast<int64>(MemoryFootprint * 2);
				if (!ResourceTracker.ChargeEstimatedMemoryBytes(
						CopyAwareMemoryBytes,
						JsonPointer,
						Error))
				{
					return false;
				}
				bValueMemoryPrecharged = true;
				FString JsonStorage;
				const FString& StringValue = Value->AsString(JsonStorage);
				DynamicChars = StringValue.Len();
			}
			else if (Value.IsValid()
				&& Value->Type == EJson::Number
				&& Value->PreferStringRepresentation())
			{
				const uint64 MemoryFootprint =
					static_cast<uint64>(Value->GetMemoryFootprint());
				const int64 CopyAwareMemoryBytes =
					MemoryFootprint > static_cast<uint64>(MAX_int64 / 2)
					? MAX_int64
					: static_cast<int64>(MemoryFootprint * 2);
				if (!ResourceTracker.ChargeEstimatedMemoryBytes(
						CopyAwareMemoryBytes,
						JsonPointer,
						Error))
				{
					return false;
				}
				bValueMemoryPrecharged = true;
				FString NumberToken;
				if (Value->TryGetString(NumberToken))
				{
					DynamicChars = NumberToken.Len();
				}
			}
			if (!ResourceTracker.ChargeNodes(1, JsonPointer, Error)
				|| !ResourceTracker.ChargeWorkUnits(
					1 + DynamicChars,
					JsonPointer,
					Error)
				|| (!bValueMemoryPrecharged
					&& !ResourceTracker.ChargeEstimatedMemoryBytes(
						sizeof(FJsonValue)
							+ DynamicChars * sizeof(TCHAR),
						JsonPointer,
						Error)))
			{
				return false;
			}
			return true;
		}

		bool AnalyzeObjectEnter(
			FAnalysisStack& Stack,
			const FWriterNodeView& Object,
			const FString& JsonPointer) const
		{
			if (!Object.IsValid())
			{
				return ResourceTracker.ChargeNodes(1, JsonPointer, Error)
					&& ResourceTracker.ChargeWorkUnits(1, JsonPointer, Error)
					&& ResourceTracker.ChargeEstimatedMemoryBytes(
						sizeof(FJsonObject),
						JsonPointer,
						Error);
			}

			const void* Identity = Object.Get();
			if (VisitingObjects.Contains(Identity))
			{
				SetWriteError(
					TEXT("hjson_value_cycle"),
					JsonPointer,
					TEXT("cyclic authoring object"));
				return false;
			}
			int64 KeyChars = 0;
			TSet<const FString*, FJsonKeyRefs, TInlineSetAllocator<16>> JsonKeys;
			if (ResourceTracker.Tracker || (Options && Options->bRequireJsonCompatibleObjectKeys))
			for (const auto& Field : Object->Fields())
			{
				if (Options && Options->bRequireJsonCompatibleObjectKeys) {
					bool bDuplicate = false; JsonKeys.Add(&Field.Key, &bDuplicate);
					if (bDuplicate) {
						SetWriteError(TEXT("canonical_json_object_key_collision"), AppendJsonPointer(JsonPointer, Field.Key),
							TEXT("The projected JSON object contains a duplicate member key."));
						return false;
					}
				}
				if (ResourceTracker.Tracker) KeyChars += Field.Key.Len();
			}
			const int64 ItemCount = Object->NumFields();
			const int64 EstimatedBytes =
				sizeof(FJsonObject)
				+ ItemCount
					* (sizeof(TPair<FString, FWriterNodeView>)
						+ sizeof(FAnalysisTask))
				+ (ItemCount * 5 + 3) * sizeof(FWriteTask)
				+ KeyChars * sizeof(TCHAR);
			if (!ResourceTracker.ChargeNodes(1, JsonPointer, Error)
				|| !ResourceTracker.ChargeItems(
					ItemCount,
					JsonPointer,
					Error)
				|| !ResourceTracker.ChargeWorkUnits(
					1 + ItemCount + KeyChars,
					JsonPointer,
					Error)
				|| !ResourceTracker.ChargeEstimatedMemoryBytes(
					EstimatedBytes,
					JsonPointer,
					Error))
			{
				return false;
			}
			if (ResourceTracker.Tracker) for (const auto& Field : Object->Fields())
			{
				const int64 CanonicalKeyChars =
					FUeAgentStructuredHjsonFormatter::IsBareKey(Field.Key)
					? Field.Key.Len()
					: EscapedQuotedStringLength(Field.Key);
				if (!ChargeCanonicalTextResources(
						CanonicalKeyChars,
						JsonPointer))
				{
					return false;
				}
			}

			VisitingObjects.Add(Identity);
			Stack.Add(FAnalysisTask::ObjectTask(
				EAnalysisTaskType::ObjectExit,
				Object,
				JsonPointer));

			// This exact object analysis owns its only key order. No local
			// array is copied into a second table before analysis/rendering.
			auto& Summary = ObjectAnalysis.FindOrAdd(Identity);
			auto& Keys = Summary.Keys;
			Keys.Reset();
			Object->GetKeys(Keys);
			bool bSorted = Object.Canonical != nullptr;
			if (Object.BorrowedKind == FWriterNodeView::EBorrowed::Object
				|| Object.BorrowedKind == FWriterNodeView::EBorrowed::Entries) {
				bSorted = true;
				for (int32 Index = 1; Index < Keys.Num(); ++Index)
					if (Keys[Index - 1]->Compare(*Keys[Index], ESearchCase::CaseSensitive) > 0) { bSorted = false; break; }
			}
			if (!bSorted) Keys.Sort([](const FOrderedObjectKey& A, const FOrderedObjectKey& B) { return A->Compare(*B, ESearchCase::CaseSensitive) < 0; });
			if (Object.BorrowedKind == FWriterNodeView::EBorrowed::Object
				&& !(Options && Options->bRequireJsonCompatibleObjectKeys)) {
				// The existing non-JSON mode permits duplicate names and reads the
				// first source member for every occurrence. Keep that exact result.
				for (int32 Begin = 0; Begin < Keys.Num();) {
					int32 End = Begin + 1;
					const auto* First = Keys[Begin].Field;
					while (End < Keys.Num() && Keys[End]->Equals(*Keys[Begin], ESearchCase::CaseSensitive)) {
						if (Keys[End].Field < First) First = Keys[End].Field;
						++End;
					}
					for (int32 Index = Begin; Index < End; ++Index) Keys[Index].Field = First;
					Begin = End;
				}
			}
			if (Object.BorrowedKind == FWriterNodeView::EBorrowed::Entries
				&& !(Options && Options->bRequireJsonCompatibleObjectKeys)) {
				for (int32 Begin = 0; Begin < Keys.Num();) {
					int32 End = Begin + 1;
					const auto* First = Keys[Begin].Entry;
					while (End < Keys.Num() && Keys[End]->Equals(*Keys[Begin], ESearchCase::CaseSensitive)) {
						if (Keys[End].Entry < First) First = Keys[End].Entry;
						++End;
					}
					for (int32 Index = Begin; Index < End; ++Index) Keys[Index].Entry = First;
					Begin = End;
				}
			}
			Summary.bHasKeys = true;
			for (int32 Index = Keys.Num() - 1; Index >= 0; --Index)
			{
				const FString& Key = *Keys[Index];
				FString ValuePointer;
				if (!TryAppendTrackedJsonPointer(
						JsonPointer,
						Key,
						1,
						true,
						ValuePointer))
				{
					return false;
				}
				Stack.Add(FAnalysisTask::ValueEnter(
					Object->FindField(Keys[Index]),
					MoveTemp(ValuePointer)));
			}
			return true;
		}

		void AnalyzeObjectExit(
			const FWriterNodeView& Object,
			const FString& JsonPointer) const
		{
			if (!Object.IsValid())
			{
				return;
			}
			auto& Summary = ObjectAnalysis.FindOrAdd(Object.Get());
			if (Summary.bNonInline) { VisitingObjects.Remove(Object.Get()); return; }
			if (!Summary.bHasCount)
			{
				// Every valid field contributes at least one to the old saturated
				// count. Its existing format limit can decide without child lookup.
				int32 Count = Object->NumFields() > InlineObjectFieldLimit ? InlineObjectFieldLimit + 1 : Object->NumFields() == 0 ? 1 : 0;
				if (Count <= InlineObjectFieldLimit) for (const auto& Field : Object->Fields())
				{
					Count = FMath::Min(
						InlineObjectFieldLimit + 1,
						Count + InlineCountForValue(Field.Value, JsonPointer));
					if (Count > InlineObjectFieldLimit)
					{
						break;
					}
				}
				Summary.Count = Count; Summary.bHasCount = true;
			}
			if (Summary.Count > InlineObjectFieldLimit) {
				Summary.bNonInline = true; VisitingObjects.Remove(Object.Get()); return;
				}
			if (!Summary.bHasDimensions)
			{
				int32 Length = Object->NumFields() == 0 ? 2 : 4;
				int32 Depth = 1;
				for (const auto& Field : Object->Fields())
				{
					const int32 KeyLength = static_cast<int32>(FMath::Min<int64>(
						InlineContainerCharacterLimit + 1,
						FUeAgentStructuredHjsonFormatter::IsBareKey(Field.Key)
						? Field.Key.Len()
						: EscapedQuotedStringLength(Field.Key, InlineContainerCharacterLimit + 1)));
					Length = static_cast<int32>(FMath::Min<int64>(
						InlineContainerCharacterLimit + 1,
						static_cast<int64>(Length) + KeyLength + 2
							+ InlineLengthForValue(Field.Value, JsonPointer)));
					Depth = FMath::Max(
						Depth,
						1 + InlineDepthForValue(Field.Value, JsonPointer));
					if (Length > InlineContainerCharacterLimit || Depth > InlineContainerDepthLimit) {
						Summary.bNonInline = true; break;
						}
				}
				if (Object->NumFields() > 1)
				{
					Length = static_cast<int32>(FMath::Min<int64>(
						InlineContainerCharacterLimit + 1,
						static_cast<int64>(Length)
							+ 2LL * (Object->NumFields() - 1)));
				}
				Summary.Length = Length; Summary.Depth = Depth;
				Summary.bNonInline |= Length > InlineContainerCharacterLimit || Depth > InlineContainerDepthLimit;
				Summary.bHasDimensions = !Summary.bNonInline;
			}
			VisitingObjects.Remove(Object.Get());
		}

		bool AnalyzeArrayEnter(
			FAnalysisStack& Stack,
			const FWriterNodeView& ArrayValue,
			const FString& JsonPointer) const
		{
			if (!ArrayValue.IsValid() || ArrayValue->Type != EJson::Array)
			{
				return AnalyzeValue(Stack, ArrayValue, JsonPointer);
			}
			const void* Identity = ArrayValue.Get();
			if (VisitingArrays.Contains(Identity))
			{
				SetWriteError(
					TEXT("hjson_value_cycle"),
					JsonPointer,
					TEXT("cyclic authoring array"));
				return false;
			}
			const auto Values = ArrayValue->AsArray();
			const int64 ItemCount = Values.Num();
			const int64 EstimatedBytes =
				sizeof(FJsonValue)
				+ ItemCount
					* (sizeof(FWriterNodeView)
						+ sizeof(FAnalysisTask))
				+ (ItemCount * 2 + 2) * sizeof(FWriteTask);
			if (!ResourceTracker.ChargeNodes(1, JsonPointer, Error)
				|| !ResourceTracker.ChargeItems(
					ItemCount,
					JsonPointer,
					Error)
				|| !ResourceTracker.ChargeWorkUnits(
					1 + ItemCount,
					JsonPointer,
					Error)
				|| !ResourceTracker.ChargeEstimatedMemoryBytes(
					EstimatedBytes,
					JsonPointer,
					Error))
			{
				return false;
			}

			VisitingArrays.Add(Identity);
			Stack.Add(FAnalysisTask::ArrayTask(
				EAnalysisTaskType::ArrayExit,
				ArrayValue,
				JsonPointer));
			for (int32 Index = Values.Num() - 1; Index >= 0; --Index)
			{
				const FString IndexSegment = FString::FromInt(Index);
				FString ValuePointer;
				if (!TryAppendTrackedJsonPointer(
						JsonPointer,
						IndexSegment,
						1,
						true,
						ValuePointer))
				{
					return false;
				}
				Stack.Add(FAnalysisTask::ValueEnter(
					Values[Index],
					MoveTemp(ValuePointer)));
			}
			return true;
		}

		void AnalyzeArrayExit(
			const FWriterNodeView& ArrayValue,
			const FString& JsonPointer) const
		{
			if (!ArrayValue.IsValid() || ArrayValue->Type != EJson::Array)
			{
				return;
			}
			auto& Summary = ArrayAnalysis.FindOrAdd(ArrayValue.Get());
			if (Summary.bNonInline) { VisitingArrays.Remove(ArrayValue.Get()); return; }
			// '[' + N nonempty values + 2*(N-1) separators + ']' is at least
			// 3*N characters. This selects the existing multiline format; it is
			// not an input quota and all children were still analyzed above.
			if (ArrayValue->AsArray().Num() > InlineContainerCharacterLimit / 3) {
				Summary.bNonInline = true; VisitingArrays.Remove(ArrayValue.Get()); return;
				}
			if (!Summary.bHasCount)
			{
				bool bContainsObject = false;
				for (const FWriterNodeView& Value : ArrayValue->AsArray())
				{
					if (!Value.IsValid())
					{
						continue;
					}
					if (Value->Type == EJson::Object)
					{
						bContainsObject = true;
						break;
					}
					if (Value->Type == EJson::Array)
					{
						const auto* Child = ArrayAnalysis.Find(Value.Get());
						if (!Child || (!Child->bHasCount && !Child->bNonInline))
						{
							SetWriteError(
								TEXT("hjson_internal_error"),
								JsonPointer,
								TEXT("missing inline array summary"));
							return;
						}
						if (Child->bNonInline || Child->Count > InlineObjectFieldLimit)
						{
							bContainsObject = true;
							break;
						}
					}
				}
				Summary.Count = bContainsObject ? InlineObjectFieldLimit + 1 : 1;
				Summary.bHasCount = true;
			}
			if (Summary.Count > InlineObjectFieldLimit) {
				Summary.bNonInline = true; VisitingArrays.Remove(ArrayValue.Get()); return;
				}
			if (!Summary.bHasDimensions)
			{
				const auto Values = ArrayValue->AsArray();
				int32 Length = Values.Num() == 0 ? 2 : 2;
				int32 Depth = 1;
				for (const FWriterNodeView& Value : Values)
				{
					Length = static_cast<int32>(FMath::Min<int64>(
						InlineContainerCharacterLimit + 1,
						static_cast<int64>(Length) + InlineLengthForValue(Value, JsonPointer)));
					Depth = FMath::Max(
						Depth,
						1 + InlineDepthForValue(Value, JsonPointer));
					if (Length > InlineContainerCharacterLimit || Depth > InlineContainerDepthLimit) {
						Summary.bNonInline = true; break;
						}
				}
				if (Values.Num() > 1)
				{
					Length = static_cast<int32>(FMath::Min<int64>(
						InlineContainerCharacterLimit + 1,
						static_cast<int64>(Length) + 2LL * (Values.Num() - 1)));
				}
				Summary.Length = Length; Summary.Depth = Depth;
				Summary.bNonInline |= Length > InlineContainerCharacterLimit || Depth > InlineContainerDepthLimit;
				Summary.bHasDimensions = !Summary.bNonInline;
			}
			VisitingArrays.Remove(ArrayValue.Get());
		}

		bool FormatNumber(const FWriterNodeView& Value, FString& Out, bool& bNonFinite) const
		{
			if (const FString* Text = FormattedNumbers.Find(Value.Get())) { Out = *Text; return true; }
			bNonFinite = false;
			if (Value->PreferStringRepresentation()) {
				if (Value.Canonical) {
					if (!TryCanonicalizeNumberToken(Value.Canonical->GetLexeme(), Out, bNonFinite)) return false;
				} else {
					FString Token;
					if (!Value->TryGetString(Token) || !TryCanonicalizeNumberToken(Token, Out, bNonFinite)) return false;
				}
			} else if (!TryFormatFiniteNative(Value->AsNumber(), false, Out)) { bNonFinite = true; return false; }
			FormattedNumbers.Add(Value.Get(), Out);
			return true;
		}

		int32 InlineLengthForValue(
			const FWriterNodeView& Input,
			const FString& JsonPointer) const
		{
			const FWriterNodeView Value = ProjectedLeaf(Input);
			if (!Value.IsValid() || Value->Type == EJson::None || Value->Type == EJson::Null)
			{
				return 4;
			}
			switch (Value->Type)
			{
			case EJson::String:
			{
				FString JsonStorage;
				const FString& Text = Value->AsString(JsonStorage);
				return static_cast<int32>(FMath::Min<int64>(
					InlineContainerCharacterLimit + 1,
					EscapedQuotedStringLength(Text, InlineContainerCharacterLimit + 1)));
			}
			case EJson::Number:
			{
				FString NumberText;
				bool bNonFinite = false;
				if (!FormatNumber(Value, NumberText, bNonFinite))
				{
					return InlineContainerCharacterLimit + 1;
				}
				return NumberText.Len();
			}
			case EJson::Boolean:
				return Value->AsBool() ? 4 : 5;
			case EJson::Array:
				if (const auto* Summary = ArrayAnalysis.Find(Value.Get()); Summary && (Summary->bHasDimensions || Summary->bNonInline))
				{
					return Summary->bNonInline ? InlineContainerCharacterLimit + 1 : Summary->Length;
				}
				break;
			case EJson::Object:
			{
				const FWriterNodeView Object = Value->AsObject();
				if (!Object.IsValid())
				{
					return 4;
				}
				if (const auto* Summary = ObjectAnalysis.Find(Object.Get()); Summary && (Summary->bHasDimensions || Summary->bNonInline))
				{
					return Summary->bNonInline ? InlineContainerCharacterLimit + 1 : Summary->Length;
				}
				break;
			}
			default:
				return 4;
			}
			SetWriteError(
				TEXT("hjson_internal_error"),
				JsonPointer,
				TEXT("missing inline container length summary"));
			return InlineContainerCharacterLimit + 1;
		}

		int32 InlineDepthForValue(
			const FWriterNodeView& Input,
			const FString& JsonPointer) const
		{
			const FWriterNodeView Value = ProjectedLeaf(Input);
			if (IsScalarValue(Value))
			{
				return 0;
			}
			if (Value->Type == EJson::Array)
			{
				if (const auto* Summary = ArrayAnalysis.Find(Value.Get()); Summary && (Summary->bHasDimensions || Summary->bNonInline))
				{
					return Summary->bNonInline ? InlineContainerDepthLimit + 1 : Summary->Depth;
				}
			}
			else if (Value->Type == EJson::Object)
			{
				const FWriterNodeView Object = Value->AsObject();
				if (!Object.IsValid())
				{
					return 0;
				}
				if (const auto* Summary = ObjectAnalysis.Find(Object.Get()); Summary && (Summary->bHasDimensions || Summary->bNonInline))
				{
					return Summary->bNonInline ? InlineContainerDepthLimit + 1 : Summary->Depth;
				}
			}
			SetWriteError(
				TEXT("hjson_internal_error"),
				JsonPointer,
				TEXT("missing inline container depth summary"));
			return InlineContainerDepthLimit + 1;
		}

		int32 InlineCountForValue(
			const FWriterNodeView& Input,
			const FString& JsonPointer) const
		{
			const FWriterNodeView Value = ProjectedLeaf(Input);
			if (IsScalarValue(Value))
			{
				return 1;
			}
			if (Value->Type == EJson::Array)
			{
				if (const auto* Summary = ArrayAnalysis.Find(Value.Get()); Summary && (Summary->bHasCount || Summary->bNonInline))
				{
					return Summary->bNonInline ? InlineObjectFieldLimit + 1 : Summary->Count;
				}
			}
			else if (Value->Type == EJson::Object)
			{
				const FWriterNodeView Object = Value->AsObject();
				if (!Object.IsValid())
				{
					return 1;
				}
				if (const auto* Summary = ObjectAnalysis.Find(Object.Get()); Summary && (Summary->bHasCount || Summary->bNonInline))
				{
					return Summary->bNonInline ? InlineObjectFieldLimit + 1 : Summary->Count;
				}
			}
			SetWriteError(
				TEXT("hjson_internal_error"),
				JsonPointer,
				TEXT("missing inline container summary"));
			return InlineObjectFieldLimit + 1;
		}

		bool RenderChecked(FWriteStack& Stack, FString& Out,
			TFunctionRef<void(FStringView)>* Consumer = nullptr) const
		{
			// AnalyzeRootValue has already validated the complete tree. Keep every
			// render check and resource charge, then consume each charged text token
			// directly instead of discarding it and formatting the tree a second time.
			// Callers clear the private partial result on any failure.
			Out.Reset();
			int32 OutputCharacters = 0;
			const auto Emit = [&](FStringView Text) {
				if (Consumer) (*Consumer)(Text); else Out.AppendChars(Text.GetData(), Text.Len());
				OutputCharacters += Text.Len();
			};
			int32 TasksUntilCheckpoint = 1024;
			const FString SpanPrefix = Options && Options->WrittenValueCharacterSpans
				? (Options->WrittenValueArrayField.IsEmpty() ? TEXT("/")
					: AppendJsonPointer(TEXT("/"), Options->WrittenValueArrayField) + TEXT("/")) : FString();
			while (Stack.Num() > 0 && Error.IsEmpty())
			{
				FWriteTask Task = Stack.Pop(EAllowShrinking::No);
				if (Task.Type == EWriteTaskType::SpanExit) {
					Options->WrittenValueCharacterSpans->Add(Task.JsonPointer, FIntPoint(Task.SpanStart, OutputCharacters));
					continue;
				}
				if (Task.Type == EWriteTaskType::Value && Task.Value.Type == EJson::Object
					&& Options && Options->WrittenValueCharacterSpans) {
					if (Task.JsonPointer.StartsWith(SpanPrefix, ESearchCase::CaseSensitive)
						&& Task.JsonPointer.Len() > SpanPrefix.Len()
						&& Task.JsonPointer.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, SpanPrefix.Len()) == INDEX_NONE) {
						FWriteTask Exit = Task; Exit.Type = EWriteTaskType::SpanExit; Exit.SpanStart = OutputCharacters;
						Stack.Add(MoveTemp(Exit));
					}
				}
				if (Task.Type == EWriteTaskType::FragmentExit)
				{
					if (auto* Pending = PendingFragments.Find(Task.JsonPointer))
					{
						if (UsageDelta(Pending->RenderBefore, ResourceTracker.GetUsage(), Pending->Proof->RenderCharges))
						{
							Pending->Proof->Text = Out.Mid(Pending->OutputStart);
							KeepFragment(Task.JsonPointer, Pending->Proof);
						}
						PendingFragments.Remove(Task.JsonPointer);
					}
					continue;
				}
				if (Task.Type == EWriteTaskType::Value)
				{
					if (const auto* Prior = AnalyzedFragments.Find(Task.JsonPointer))
					{
						if ((*Prior)->Value.Pin().Get() == Task.Value.Canonical && (*Prior)->Level == Task.Level
							&& (*Prior)->FieldName == Task.FieldName && !Task.bInlineContext)
						{
							if (!ReplayUsage((*Prior)->RenderCharges, Task.JsonPointer)) return false;
							Emit((*Prior)->Text);
							KeepFragment(Task.JsonPointer, *Prior);
							++ReusedFragmentCount;
							continue;
						}
						// This request selected a proved fragment/context. Missing that
						// handoff is not permission to rerun a different production path.
						SetWriteError(TEXT("hjson_fragment_context_unavailable"), Task.JsonPointer,
							TEXT("The selected formatter fragment no longer matches its actual field/level/inline context. The formatter producer must supply the matching fragment before rendering; no automatic subtree reanalysis was selected."));
						return false;
					}
					if (auto* Pending = PendingFragments.Find(Task.JsonPointer))
					{
						if (Pending->Proof->Level == Task.Level && Pending->Proof->FieldName == Task.FieldName && !Task.bInlineContext)
						{
							Pending->RenderBefore = ResourceTracker.GetUsage();
							Pending->OutputStart = Out.Len();
							FWriteTask Exit = Task;
							Exit.Type = EWriteTaskType::FragmentExit;
							Stack.Add(MoveTemp(Exit));
						}
						else PendingFragments.Remove(Task.JsonPointer);
					}
				}
				if (--TasksUntilCheckpoint == 0)
				{
					TasksUntilCheckpoint = 1024;
					if (!ResourceTracker.Checkpoint(Task.JsonPointer, Error)) break;
				}
				if (Task.Type == EWriteTaskType::Text || Task.Type == EWriteTaskType::LiteralText)
				{
					if (!Task.bTextResourcesPrecharged
						&& !ChargeCanonicalTextResources(
							Task.GetText().Len(),
							Task.JsonPointer))
					{
						break;
					}
				}
				else if (!ResourceTracker.ChargeWorkUnits(
						1,
						Task.JsonPointer,
						Error))
				{
					break;
				}
				switch (Task.Type)
				{
				case EWriteTaskType::Text:
				case EWriteTaskType::LiteralText:
					Emit(Task.GetText());
					break;
				case EWriteTaskType::ObjectBody:
					PushObjectBody(
						Stack,
						Task.Object,
						Task.Level,
						Task.JsonPointer,
						true);
					break;
				case EWriteTaskType::Value:
					PushValue(
						Stack,
						Task.Value,
						Task.Level,
						Task.FieldName,
						Task.JsonPointer,
						Task.bInlineContext,
						true);
					break;
				case EWriteTaskType::FragmentExit:
				case EWriteTaskType::SpanExit:
					break; // Consumed before dispatch.
				}
			}
			return Error.IsEmpty()
				&& ResourceTracker.Checkpoint(TEXT("/"), Error);
		}

		void PushValue(
			FWriteStack& Stack,
			const FWriterNodeView& Input,
			const int32 Level,
			const FString& FieldName,
			const FString& JsonPointer,
			const bool bInlineContext,
			const bool bPreflight) const
		{
			const FWriterNodeView Value = ProjectedLeaf(Input);
			if (!Value.IsValid() || Value->Type == EJson::None || Value->Type == EJson::Null)
			{
				Stack.Add(FWriteTask::LiteralTask(TEXT("null"), JsonPointer));
				return;
			}
			switch (Value->Type)
			{
			case EJson::String:
			{
				FString JsonStorage;
				const FString& StringValue = Value->AsString(JsonStorage);
				FWriteTask StringTask;
				if (!TryMakeStringValueTask(
						StringValue,
						JsonPointer,
						bInlineContext,
						bPreflight,
						StringTask))
				{
					return;
				}
				Stack.Add(MoveTemp(StringTask));
				return;
			}
			case EJson::Number:
			{
				FString NumberText;
				bool bNonFinite = false;
				if (!FormatNumber(Value, NumberText, bNonFinite))
				{
					SetWriteError(
						bNonFinite ? TEXT("hjson_non_finite_number") : TEXT("hjson_invalid_number"),
						JsonPointer,
						bNonFinite ? TEXT("non-finite authoring number") : TEXT("invalid authoring number"));
					return;
				}
				Stack.Add(FWriteTask::TextTask(
					MoveTemp(NumberText),
					JsonPointer));
				return;
			}
			case EJson::Boolean:
				Stack.Add(FWriteTask::LiteralTask(
					Value->AsBool() ? TEXT("true") : TEXT("false"),
					JsonPointer));
				return;
			case EJson::Array:
				PushArray(
					Stack,
					Value,
					Level,
					JsonPointer,
					bPreflight);
				return;
			case EJson::Object:
				PushObject(
					Stack,
					Value->AsObject(),
					Level,
					FieldName,
					JsonPointer,
					bPreflight);
				return;
			default:
				Stack.Add(FWriteTask::LiteralTask(TEXT("null"), JsonPointer));
				return;
			}
		}

		TConstArrayView<FOrderedObjectKey> OrderedKeys(
			const FWriterNodeView& Object,
			const FString& JsonPointer,
			FOrderedObjectKeys& Reordered) const
		{
			Reordered.Reset();
			if (!Object.IsValid())
			{
				return {};
			}
			const auto* Summary = ObjectAnalysis.Find(Object.Get());
			if (!Summary || !Summary->bHasKeys) {
				SetWriteError(TEXT("hjson_object_analysis_unavailable"), JsonPointer,
					TEXT("Object rendering requires the completed field-order handoff from this write call."));
				return {};
			}
			const TConstArrayView<FOrderedObjectKey> Keys(Summary->Keys);
			if (TryGetTupleKeyOrder(Keys, Reordered))
			{
				return Reordered;
			}
			if (Options)
			{
				if (const TArray<FString>* Preferred = Options->PreferredKeyOrderByJsonPointer.Find(JsonPointer))
				{
					for (const FString& Key : *Preferred)
					{
						const auto* ExistingKey = Keys.FindByPredicate([&](const FOrderedObjectKey& Existing) { return Existing->Equals(Key, ESearchCase::CaseSensitive); });
						if (ExistingKey && !Reordered.ContainsByPredicate([&](const FOrderedObjectKey& Existing) { return Existing->Equals(Key, ESearchCase::CaseSensitive); }))
						{
							Reordered.Add(*ExistingKey);
						}
					}
					if (!Reordered.IsEmpty()) {
						const int32 PreferredCount = Reordered.Num();
						for (const FOrderedObjectKey& Key : Keys)
							if (!MakeArrayView(Reordered).Left(PreferredCount).ContainsByPredicate(
								[&](const FString* Existing) { return Existing->Equals(*Key, ESearchCase::CaseSensitive); })) Reordered.Add(Key);
						return Reordered;
						}
				}
			}
			return Keys;
		}

		void PushObjectBody(
			FWriteStack& Stack,
			const FWriterNodeView& Object,
			const int32 Level,
			const FString& JsonPointer,
			const bool bPreflight) const
		{
			FOrderedObjectKeys Reordered;
			const auto Keys = OrderedKeys(Object, JsonPointer, Reordered);
			const int32 FirstTask = Stack.Num();
			FWriteStack& Forward = Stack;
			for (int32 Index = 0; Index < Keys.Num(); ++Index)
			{
				if (Index > 0)
				{
					Forward.Add(FWriteTask::LiteralTask(
						TEXT("\n"),
						JsonPointer));
				}
				const FString& Key = *Keys[Index];
				FString ValuePointer;
				if (!TryAppendTrackedJsonPointer(
						JsonPointer,
						Key,
						5,
						bPreflight,
						ValuePointer))
				{
					return;
				}
				Forward.Add(FWriteTask::TextTask(
					Padding(Level),
					ValuePointer));
				FWriteTask KeyTask;
				if (!TryFormatKey(
						Key,
						ValuePointer,
						bPreflight,
						KeyTask))
				{
					return;
				}
				Forward.Add(MoveTemp(KeyTask));
				Forward.Add(FWriteTask::LiteralTask(
					TEXT(": "),
					ValuePointer));
				Forward.Add(FWriteTask::ValueTask(
					Object->FindField(Keys[Index]),
					Level,
					Key,
					ValuePointer));
			}
			ReverseAddedTasks(Stack, FirstTask);
		}

		void PushObject(
			FWriteStack& Stack,
			const FWriterNodeView& Object,
			const int32 Level,
			const FString& FieldName,
			const FString& JsonPointer,
			const bool bPreflight) const
		{
			(void)FieldName;
			if (!Object.IsValid() || Object->NumFields() == 0)
			{
				Stack.Add(FWriteTask::LiteralTask(TEXT("{}"), JsonPointer));
				return;
			}
			const int32 FirstTask = Stack.Num();
			FWriteStack& Forward = Stack;
			if (CanInlineObject(Object, JsonPointer))
			{
				FOrderedObjectKeys Reordered;
				const auto Keys = OrderedKeys(Object, JsonPointer, Reordered);
				Forward.Add(FWriteTask::LiteralTask(
					TEXT("{ "),
					JsonPointer));
				for (int32 Index = 0; Index < Keys.Num(); ++Index)
				{
					if (Index > 0)
					{
						Forward.Add(FWriteTask::LiteralTask(
							TEXT(", "),
							JsonPointer));
					}
					const FString& Key = *Keys[Index];
					FString ValuePointer;
					if (!TryAppendTrackedJsonPointer(
							JsonPointer,
							Key,
							4,
							bPreflight,
							ValuePointer))
					{
						return;
					}
					FWriteTask KeyTask;
					if (!TryFormatKey(
							Key,
							ValuePointer,
							bPreflight,
							KeyTask))
					{
						return;
					}
					Forward.Add(MoveTemp(KeyTask));
					Forward.Add(FWriteTask::LiteralTask(
						TEXT(": "),
						ValuePointer));
					Forward.Add(FWriteTask::ValueTask(
						Object->FindField(Keys[Index]),
						0,
						Key,
						ValuePointer,
						true));
				}
				Forward.Add(FWriteTask::LiteralTask(
					TEXT(" }"),
					JsonPointer));
			}
			else
			{
				Forward.Add(FWriteTask::LiteralTask(
					TEXT("{\n"),
					JsonPointer));
				Forward.Add(FWriteTask::ObjectBody(Object, Level + 1, JsonPointer));
				Forward.Add(FWriteTask::TextTask(
					TEXT("\n") + Padding(Level) + TEXT("}"),
					JsonPointer));
			}
			ReverseAddedTasks(Stack, FirstTask);
		}

		void PushArray(
			FWriteStack& Stack,
			const FWriterNodeView& ArrayValue,
			const int32 Level,
			const FString& JsonPointer,
			const bool bPreflight) const
		{
			const auto Values =
				ArrayValue->AsArray();
			if (Values.Num() == 0)
			{
				Stack.Add(FWriteTask::LiteralTask(TEXT("[]"), JsonPointer));
				return;
			}
			const int32 FirstTask = Stack.Num();
			FWriteStack& Forward = Stack;
			Forward.Add(FWriteTask::LiteralTask(TEXT("["), JsonPointer));
			const bool bInline = CanInlineArray(ArrayValue, JsonPointer);
			for (int32 Index = 0; Index < Values.Num(); ++Index)
			{
				const FString IndexSegment = FString::FromInt(Index);
				FString ValuePointer;
				if (!TryAppendTrackedJsonPointer(
						JsonPointer,
						IndexSegment,
						bInline ? 2 : 3,
						bPreflight,
						ValuePointer))
				{
					return;
				}
				if (bInline)
				{
					if (Index > 0)
					{
						Forward.Add(FWriteTask::LiteralTask(
							TEXT(", "),
							JsonPointer));
					}
				}
				else
				{
					Forward.Add(FWriteTask::TextTask(
						TEXT("\n") + Padding(Level + 1),
						ValuePointer));
				}
				Forward.Add(FWriteTask::ValueTask(
					Values[Index],
					bInline ? 0 : Level + 1,
					FString(),
					ValuePointer,
					bInline));
			}
			if (bInline) Forward.Add(FWriteTask::LiteralTask(TEXT("]"), JsonPointer));
			else Forward.Add(FWriteTask::TextTask(TEXT("\n") + Padding(Level) + TEXT("]"), JsonPointer));
			ReverseAddedTasks(Stack, FirstTask);
		}

		static void ReverseAddedTasks(FWriteStack& Stack, int32 First)
		{
			// Build/check in the original diagnostic order, directly in the actual
			// work stack. Reverse only this append; no temporary Forward allocation
			// or second traversal that moves every task into another container.
			for (int32 Last = Stack.Num() - 1; First < Last; ++First, --Last)
				Swap(Stack[First], Stack[Last]);
		}

		bool CanInlineObject(
			const FWriterNodeView& Object,
			const FString& JsonPointer) const
		{
			if (!Object.IsValid() || Object->NumFields() == 0)
			{
				return true;
			}
			if (const auto* Summary = ObjectAnalysis.Find(Object.Get()); Summary && (Summary->bHasCount || Summary->bNonInline))
			{
				return !Summary->bNonInline && Summary->bHasDimensions
					&& Summary->Count <= InlineObjectFieldLimit
					&& Summary->Length <= InlineContainerCharacterLimit
					&& Summary->Depth <= InlineContainerDepthLimit;
			}
			SetWriteError(
				TEXT("hjson_internal_error"),
				JsonPointer,
				TEXT("missing inline object summary"));
			return false;
		}

		bool CanInlineArray(
			const FWriterNodeView& ArrayValue,
			const FString& JsonPointer) const
		{
			if (const auto* Summary = ArrayAnalysis.Find(ArrayValue.Get()); Summary && (Summary->bHasCount || Summary->bNonInline))
			{
				return !Summary->bNonInline && Summary->bHasDimensions
					&& Summary->Count <= InlineObjectFieldLimit
					&& Summary->Length <= InlineContainerCharacterLimit
					&& Summary->Depth <= InlineContainerDepthLimit;
			}
			SetWriteError(
				TEXT("hjson_internal_error"),
				JsonPointer,
				TEXT("missing inline array summary"));
			return false;
		}

		void SetWriteError(
			const FString& Code,
			const FString& JsonPointer,
			const FString& Message) const
		{
			if (!Error.IsEmpty())
			{
				return;
			}
			Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Message, TEXT("hjson_write"));
			Error.DialectId = FUeAgentStructuredHjsonFormatter::DialectId();
			Error.Source.JsonPath = JsonPointer;
		}

		const FUeAgentStructuredHjsonWriteOptions* Options = nullptr;
		TUniquePtr<FUeAgentStructuredResourceTracker> OwnedResourceTracker;
		FWriterResourceUsage ResourceTracker;
		// Borrow original immutable member names through this writer only. The
		// input/leaf projection owns their storage; growing this writer's lookup
		// map cannot move those input member names. Inputs remain immutable.
		mutable TMap<const void*, FObjectAnalysis, TInlineSetAllocator<4>> ObjectAnalysis;
		mutable TMap<const void*, FContainerAnalysis, TInlineSetAllocator<4>> ArrayAnalysis;
		mutable TMap<const void*, FString, TInlineSetAllocator<4>> FormattedNumbers;
		mutable TSet<const void*, DefaultKeyFuncs<const void*>, TInlineSetAllocator<4>> VisitingObjects;
		mutable TSet<const void*, DefaultKeyFuncs<const void*>, TInlineSetAllocator<4>> VisitingArrays;
		mutable UE::UAI::Contract::FDiagnostic Error;
		mutable TMap<const FWriterNodeView::FCanonical*, TSharedPtr<FJsonValue>> LeafProjections;
		mutable TMap<FString, TSharedPtr<const FWriterNodeView::FCanonical>> FragmentCandidates;
		mutable TMap<FString, FString> FragmentFieldNames;
		mutable FString FragmentContext;
		mutable TSharedPtr<const FCanonicalFragmentCache> PriorFragments;
		mutable TSharedPtr<FCanonicalFragmentCache> NextFragments;
		mutable TMap<FString, FPendingFragment> PendingFragments;
		mutable TMap<FString, TSharedPtr<const FCanonicalFragmentProof>> AnalyzedFragments;
		mutable int32 CandidateLevel = 0;
		mutable int32 ReusedFragmentCount = 0;
		mutable int64 CachedFragmentChars = 0;
	};

}



const TCHAR* FUeAgentStructuredHjsonFormatter::DialectId()
{
	return TEXT("uai-hjson-authoring/1");
}

bool FUeAgentStructuredHjsonFormatter::TryVisitValueText(const TSharedRef<FJsonValue>& Value,
	const FUeAgentStructuredHjsonWriteOptions& Options, TFunctionRef<void(FStringView)> Consumer,
	UE::UAI::Contract::FDiagnostic& OutError)
{
	return FHjsonWriter(&Options, nullptr, false).TryVisitValueText(FWriterNodeView(&Value.Get()), Consumer, OutError);
}

bool FUeAgentStructuredHjsonFormatter::TryVisitValueText(const UE::UAI::StateSemantic::FUeAgentCanonicalValue& Value,
	const FUeAgentStructuredHjsonWriteOptions& Options, TFunctionRef<void(FStringView)> Consumer,
	UE::UAI::Contract::FDiagnostic& OutError)
{
	return FHjsonWriter(&Options, nullptr, false).TryVisitValueText(FWriterNodeView(Value), Consumer, OutError);
}

bool FUeAgentStructuredHjsonFormatter::TryVisitValueText(const FUeAgentStructuredHjsonObjectView& Object,
	const FUeAgentStructuredHjsonWriteOptions& Options, TFunctionRef<void(FStringView)> Consumer,
	UE::UAI::Contract::FDiagnostic& OutError)
{
	return FHjsonWriter(&Options, nullptr, false).TryVisitValueText(FWriterNodeView(Object), Consumer, OutError);
}

bool FUeAgentStructuredHjsonFormatter::TryWriteValue(const FUeAgentStructuredHjsonObjectView& Object,
	const FUeAgentStructuredHjsonWriteOptions& Options, FString& OutText,
	UE::UAI::Contract::FDiagnostic& OutError)
{
	return FHjsonWriter(&Options).TryWriteValue(FWriterNodeView(Object), OutText, OutError);
}

bool UeAgentNumericFormatting::TryFormatNative(float Value, bool PreserveDecimalKind, FString& OutText)
{
	return TryFormatFiniteNative(Value, PreserveDecimalKind, OutText);
}

bool UeAgentNumericFormatting::TryFormatNative(double Value, bool PreserveDecimalKind, FString& OutText)
{
	return TryFormatFiniteNative(Value, PreserveDecimalKind, OutText);
}

namespace
{
	template<typename T>
	TSharedPtr<FJsonValue> MakeNativeFloatingValue(T Value)
	{
		const T Number = Value == T(0) ? T(0) : Value;
		FString Text;
		if (!UeAgentNumericFormatting::TryFormatNative(Number, true, Text))
			return MakeShared<FJsonValueNumber>(static_cast<double>(Number));
		return MakeShared<FJsonValueNumberString>(MoveTemp(Text));
	}
}

TSharedPtr<FJsonValue> UeAgentNumericFormatting::MakeNativeValue(float Value)
{
	return MakeNativeFloatingValue(Value);
}

TSharedPtr<FJsonValue> UeAgentNumericFormatting::MakeNativeValue(double Value)
{
	return MakeNativeFloatingValue(Value);
}

FString FUeAgentStructuredHjsonFormatter::WriteObject(const TSharedRef<FJsonObject>& Object)
{
	FString Text;
	UE::UAI::Contract::FDiagnostic Error;
	TryWriteObject(Object, Text, Error);
	return Text;
}

FString FUeAgentStructuredHjsonFormatter::WriteObject(
	const TSharedRef<FJsonObject>& Object,
	const FUeAgentStructuredHjsonWriteOptions& Options)
{
	FString Text;
	UE::UAI::Contract::FDiagnostic Error;
	TryWriteObject(Object, Options, Text, Error);
	return Text;
}

FString FUeAgentStructuredHjsonFormatter::WriteValue(const TSharedRef<FJsonValue>& Value)
{
	FString Text;
	UE::UAI::Contract::FDiagnostic Error;
	TryWriteValue(Value, Text, Error);
	return Text;
}

FString FUeAgentStructuredHjsonFormatter::WriteValue(
	const TSharedRef<FJsonValue>& Value,
	const FUeAgentStructuredHjsonWriteOptions& Options)
{
	FString Text;
	UE::UAI::Contract::FDiagnostic Error;
	TryWriteValue(Value, Options, Text, Error);
	return Text;
}

bool FUeAgentStructuredHjsonFormatter::TryWriteObject(
	const TSharedRef<FJsonObject>& Object,
	FString& OutText,
	UE::UAI::Contract::FDiagnostic& OutError)
{
	return FHjsonWriter().TryWriteRootObject(Object, OutText, OutError);
}

bool FUeAgentStructuredHjsonFormatter::TryWriteObject(
	const TSharedRef<FJsonObject>& Object,
	const FUeAgentStructuredHjsonWriteOptions& Options,
	FString& OutText,
	UE::UAI::Contract::FDiagnostic& OutError)
{
	return FHjsonWriter(&Options).TryWriteRootObject(Object, OutText, OutError);
}

bool FUeAgentStructuredHjsonFormatter::TryWriteObject(
	const TSharedRef<FJsonObject>& Object,
	const FUeAgentStructuredHjsonWriteOptions& Options,
	FUeAgentStructuredResourceTracker& ResourceTracker,
	FString& OutText,
	UE::UAI::Contract::FDiagnostic& OutError)
{
	return FHjsonWriter(&Options, &ResourceTracker).TryWriteRootObject(
		Object,
		OutText,
		OutError);
}

bool FUeAgentStructuredHjsonFormatter::TryWriteObjectFragments(
	TConstArrayView<FUeAgentStructuredHjsonObjectFragment> Fragments, const FString& ArrayField,
	TArray<uint8>& OutUtf8, UE::UAI::Contract::FDiagnostic& OutError)
{
	return TryWriteObjectFragments(Fragments, ArrayField, OutUtf8, OutError, nullptr);
}

bool FUeAgentStructuredHjsonFormatter::TryWriteObjectFragments(
	TConstArrayView<FUeAgentStructuredHjsonObjectFragment> Fragments, const FString& ArrayField,
	TArray<uint8>& OutUtf8, UE::UAI::Contract::FDiagnostic& OutError, TArray<FIntPoint>* OutFragmentByteSpans)
{
	OutUtf8.Reset(); OutError.Reset();
	if (OutFragmentByteSpans) { OutFragmentByteSpans->Reset(); OutFragmentByteSpans->SetNum(Fragments.Num()); }
	const bool bArray = !ArrayField.IsEmpty();
	TArray<int32> Order; Order.Reserve(Fragments.Num());
	int64 Bytes = 32;
	for (int32 I = 0; I < Fragments.Num(); ++I) {
		const auto& Part = Fragments[I];
		if (Part.Utf8.Num() < 3 || Part.Utf8[0] != '{' || Part.Utf8.Last() != '}'
			|| Part.Depth < 0 || Part.Depth > 2 || (!bArray && Part.Key.IsEmpty())
			|| !std::memchr(Part.Utf8.GetData(), '\n', Part.Utf8.Num())) {
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("hjson_fragment_shape_invalid"),
				TEXT("A reusable fragment must be a complete multiline object at its recorded block depth."), TEXT("hjson_write"));
			return false;
		}
		Order.Add(I); Bytes += int64(Part.Utf8.Num()) * 3 + int64(Part.Key.Len()) * 6 + 16;
	}
	if (!bArray) Order.Sort([&](int32 A, int32 B) { return Fragments[A].Key.Compare(Fragments[B].Key, ESearchCase::CaseSensitive) < 0; });
	for (int32 I = 1; !bArray && I < Order.Num(); ++I)
		if (Fragments[Order[I - 1]].Key.Equals(Fragments[Order[I]].Key, ESearchCase::CaseSensitive)) {
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("hjson_fragment_key_duplicate"),
				TEXT("A reusable object member key must be unique."), TEXT("hjson_write")); return false;
		}
	FUeAgentStructuredResourceTracker Resources;
	if (Bytes > MAX_int32) {
		OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("hjson_fragment_output_too_large"),
			TEXT("The fragment output cannot fit the writer's byte container."), TEXT("hjson_write")); return false;
	}
	if (!Resources.ChargeEstimatedMemoryBytes(Bytes, TEXT("/"), OutError)
		|| !Resources.ChargeItems(Fragments.Num(), TEXT("/"), OutError)) return false;
	auto AppendText = [&](const FString& Text) { const FTCHARToUTF8 Utf8(*Text); OutUtf8.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()); };
	auto KeyText = [](const FString& Key) { return IsBareKey(Key) ? Key : EscapeQuotedString(Key); };
	AppendText(TEXT("{\n"));
	if (bArray) AppendText(TEXT("  ") + KeyText(ArrayField) + TEXT(": [\n"));
	const int32 Depth = bArray ? 2 : 1;
	for (int32 Index : Order) {
		const auto& Part = Fragments[Index];
		if (!Resources.ChargeInputBytes(Part.Utf8.Num(), TEXT("/"), OutError)
			|| !Resources.ChargeWorkUnits(Part.Utf8.Num(), TEXT("/"), OutError)) { OutUtf8.Reset(); return false; }
		AppendText(FString::ChrN(Depth * 2, TEXT(' ')));
		if (!bArray) AppendText(KeyText(Part.Key) + TEXT(": "));
		const int32 SpanStart = OutUtf8.Num();
		if (Part.Depth == Depth) OutUtf8.Append(Part.Utf8.GetData(), Part.Utf8.Num());
		else {
			// Fresh standalone values have depth zero. Existing accepted spans
			// already have the correct block depth and take the direct-copy path.
			int32 Start = 0;
			for (int32 Pos = 0; Pos < Part.Utf8.Num(); ++Pos) if (Part.Utf8[Pos] == '\n') {
				OutUtf8.Append(Part.Utf8.GetData() + Start, Pos - Start + 1);
				Start = Pos + 1;
				for (int32 N = 0; N < Part.Depth * 2; ++N) {
					if (Start >= Part.Utf8.Num() || Part.Utf8[Start] != ' ') {
						OutUtf8.Reset(); OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("hjson_fragment_indent_invalid"),
							TEXT("A reusable fragment does not have its recorded block indentation."), TEXT("hjson_write")); return false;
					}
					++Start;
				}
				AppendText(FString::ChrN(Depth * 2, TEXT(' '))); Pos = Start - 1;
			}
			OutUtf8.Append(Part.Utf8.GetData() + Start, Part.Utf8.Num() - Start);
		}
		if (OutFragmentByteSpans) (*OutFragmentByteSpans)[Index] = FIntPoint(SpanStart, OutUtf8.Num());
		AppendText(TEXT("\n"));
	}
	if (bArray) AppendText(TEXT("  ]\n"));
	AppendText(TEXT("}\n"));
	return Resources.Checkpoint(TEXT("/"), OutError);
}

bool FUeAgentStructuredHjsonFormatter::TryWriteValue(
	const TSharedRef<FJsonValue>& Value,
	FString& OutText,
	UE::UAI::Contract::FDiagnostic& OutError)
{
	return FHjsonWriter().TryWriteValue(Value, OutText, OutError);
}

bool FUeAgentStructuredHjsonFormatter::TryWriteValue(
	const TSharedRef<FJsonValue>& Value,
	const FUeAgentStructuredHjsonWriteOptions& Options,
	FString& OutText,
	UE::UAI::Contract::FDiagnostic& OutError)
{
	return FHjsonWriter(&Options).TryWriteValue(Value, OutText, OutError);
}

bool FUeAgentStructuredHjsonFormatter::TryWriteValue(
	const TSharedRef<FJsonValue>& Value,
	const FUeAgentStructuredHjsonWriteOptions& Options,
	FUeAgentStructuredResourceTracker& ResourceTracker,
	FString& OutText,
	UE::UAI::Contract::FDiagnostic& OutError)
{
	return FHjsonWriter(&Options, &ResourceTracker).TryWriteValue(
		Value,
		OutText,
		OutError);
}

bool FUeAgentStructuredHjsonFormatter::TryWriteObject(
	const UE::UAI::StateSemantic::FUeAgentCanonicalValue& Object,
	FString& OutText,
	UE::UAI::Contract::FDiagnostic& OutError)
{
	return FHjsonWriter().TryWriteRootObject(FWriterNodeView(Object), OutText, OutError);
}

bool FUeAgentStructuredHjsonFormatter::TryWriteObject(
	const UE::UAI::StateSemantic::FUeAgentCanonicalValue& Object,
	const FUeAgentStructuredHjsonWriteOptions& Options,
	FUeAgentStructuredResourceTracker& ResourceTracker,
	FString& OutText,
	UE::UAI::Contract::FDiagnostic& OutError)
{
	return FHjsonWriter(&Options, &ResourceTracker).TryWriteRootObject(FWriterNodeView(Object), OutText, OutError);
}

bool FUeAgentStructuredHjsonFormatter::TryWriteValue(
	const UE::UAI::StateSemantic::FUeAgentCanonicalValue& Value,
	FString& OutText,
	UE::UAI::Contract::FDiagnostic& OutError)
{
	return FHjsonWriter().TryWriteValue(FWriterNodeView(Value), OutText, OutError);
}

bool FUeAgentStructuredHjsonFormatter::TryWriteValue(
	const UE::UAI::StateSemantic::FUeAgentCanonicalValue& Value,
	const FUeAgentStructuredHjsonWriteOptions& Options,
	FString& OutText,
	UE::UAI::Contract::FDiagnostic& OutError)
{
	return FHjsonWriter(&Options).TryWriteValue(FWriterNodeView(Value), OutText, OutError);
}

bool FUeAgentStructuredHjsonFormatter::IsBareKey(const FString& Key)
{
	if (Key.IsEmpty())
	{
		return false;
	}

	const TCHAR First = Key[0];
	if (!IsAsciiAlpha(First) && First != TEXT('_'))
	{
		return false;
	}

	for (int32 Index = 1; Index < Key.Len(); ++Index)
	{
		const TCHAR Ch = Key[Index];
		if (!IsAsciiAlpha(Ch) && !IsAsciiDigit(Ch) && Ch != TEXT('_'))
		{
			return false;
		}
	}
	return true;
}

bool FUeAgentStructuredHjsonFormatter::IsBareString(const FString& Value)
{
	if (Value.IsEmpty())
	{
		return false;
	}

	if (Value.Equals(TEXT("true"), ESearchCase::IgnoreCase)
		|| Value.Equals(TEXT("false"), ESearchCase::IgnoreCase)
		|| Value.Equals(TEXT("null"), ESearchCase::IgnoreCase))
	{
		return false;
	}

	const TCHAR First = Value[0];
	if (!IsAsciiAlpha(First) && First != TEXT('_'))
	{
		return false;
	}

	for (int32 Index = 1; Index < Value.Len(); ++Index)
	{
		const TCHAR Ch = Value[Index];
		if (!IsAsciiAlpha(Ch) && !IsAsciiDigit(Ch) && Ch != TEXT('_') && Ch != TEXT('.') && Ch != TEXT('-'))
		{
			return false;
		}
	}
	return true;
}

FString FUeAgentStructuredHjsonFormatter::EscapeQuotedString(const FString& Value)
{
	FString Out;
	const int64 PlainLength = int64(Value.Len()) + 2;
	if (PlainLength <= MAX_int32)
	{
		Out.Reserve(static_cast<int32>(PlainLength));
	}
	Out.AppendChar(TEXT('"'));
	// Ordinary text needs one scan and one append per unescaped run, rather
	// than a length pre-scan followed by one FString mutation per character.
	int32 RunStart = 0;
	for (int32 Index = 0; Index < Value.Len(); ++Index)
	{
		const TCHAR Ch = Value[Index];
		if (Ch >= 0x20 && Ch != TEXT('"') && Ch != TEXT('\\')) continue;
		Out.AppendChars(*Value + RunStart, Index - RunStart);
		switch (Ch)
		{
		case TEXT('"'):
			Out += TEXT("\\\"");
			break;
		case TEXT('\\'):
			Out += TEXT("\\\\");
			break;
		case TEXT('\b'):
			Out += TEXT("\\b");
			break;
		case TEXT('\f'):
			Out += TEXT("\\f");
			break;
		case TEXT('\n'):
			Out += TEXT("\\n");
			break;
		case TEXT('\r'):
			Out += TEXT("\\r");
			break;
		case TEXT('\t'):
			Out += TEXT("\\t");
			break;
		default:
			{
				static constexpr TCHAR Hex[] = TEXT("0123456789abcdef");
				const TCHAR Escape[] {TEXT('\\'), TEXT('u'), TEXT('0'), TEXT('0'),
					Hex[static_cast<uint32>(Ch) >> 4], Hex[static_cast<uint32>(Ch) & 0xf]};
				Out.AppendChars(Escape, UE_ARRAY_COUNT(Escape));
			}
			break;
		}
		RunStart = Index + 1;
	}
	Out.AppendChars(*Value + RunStart, Value.Len() - RunStart);
	Out.AppendChar(TEXT('"'));
	return Out;
}

const FUeAgentHjsonSourceLocation* FUeAgentStructuredHjsonDocument::FindSourceLocation(
	const FString& JsonPointer) const
{
	return SourceLocations.Find(JsonPointer);
}

bool FUeAgentStructuredHjsonParser::ParseDocument(
	const FString& Text,
	const FString& FilePath,
	FUeAgentStructuredHjsonDocument& OutDocument,
	UE::UAI::Contract::FDiagnostic& OutError)
{
	FUeAgentStructuredResourceTracker ResourceTracker;
	return ParseDocument(
		Text,
		FilePath,
		ResourceTracker,
		false,
		OutDocument,
		OutError);
}

bool FUeAgentStructuredHjsonParser::ParseDocument(
	const FString& Text,
	const FString& FilePath,
	FUeAgentStructuredResourceTracker& ResourceTracker,
	const bool bInputPrecharged,
	FUeAgentStructuredHjsonDocument& OutDocument,
	UE::UAI::Contract::FDiagnostic& OutError)
{
	OutDocument = FUeAgentStructuredHjsonDocument();
	OutError.Reset();
	if (!ResourceTracker.Checkpoint(TEXT("/"), OutError))
	{
		return false;
	}

	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::HjsonWorkspace;
	FUeAgentResourceTracker CoreResources;
	FHjsonDocumentPtr CoreDocument;
	FWorkspaceError CoreError;
	UE::UAI::Contract::FDiagnostic CheckpointError;
	const FHjsonOperationCheckpoint CoreCheckpoint =
		[&](const FString& Stage, FWorkspaceError& Error)
		{
			if (ResourceTracker.Checkpoint(Stage, CheckpointError))
			{
				return true;
			}
			CheckpointError.StoppedEarly = true;
			Error.SetFailure(CheckpointError, FilePath, Stage);
			return false;
		};
	const bool bParsed = TryParseHjsonDocument(
		Text, FilePath, CoreResources, CoreDocument, CoreError, &CoreCheckpoint);
	TSharedPtr<FJsonValue> RootValue;
	const bool bProjected = bParsed && CoreDocument.IsValid()
		&& TryProjectHjsonNodeToJsonValue(
			CoreDocument->GetRoot().Get(), CoreResources, RootValue, CoreError,
			&CoreCheckpoint);

	const FUeAgentResourceUsage CoreUsage = CoreResources.Snapshot();
	auto ChargeMapped = [&](const EUeAgentResourceDimension Dimension,
		const int64 Amount, UE::UAI::Contract::FDiagnostic& MappingError) -> bool
	{
		if (Amount <= 0 || (bInputPrecharged
			&& Dimension == EUeAgentResourceDimension::InputBytes))
		{
			return true;
		}
		switch (Dimension)
		{
		case EUeAgentResourceDimension::InputBytes:
			return ResourceTracker.ChargeInputBytes(Amount, TEXT("/"), MappingError);
		case EUeAgentResourceDimension::Nodes:
			return ResourceTracker.ChargeNodes(Amount, TEXT("/"), MappingError);
		case EUeAgentResourceDimension::Items:
			return ResourceTracker.ChargeItems(Amount, TEXT("/"), MappingError);
		case EUeAgentResourceDimension::WorkUnits:
			return ResourceTracker.ChargeWorkUnits(Amount, TEXT("/"), MappingError);
		case EUeAgentResourceDimension::EstimatedMemoryBytes:
			return ResourceTracker.ChargeEstimatedMemoryBytes(Amount, TEXT("/"), MappingError);
		default:
			return true;
		}
	};
	UE::UAI::Contract::FDiagnostic AccountingError;
	for (const EUeAgentResourceDimension Dimension : {
		EUeAgentResourceDimension::InputBytes,
		EUeAgentResourceDimension::Nodes,
		EUeAgentResourceDimension::Items,
		EUeAgentResourceDimension::WorkUnits,
		EUeAgentResourceDimension::EstimatedMemoryBytes})
	{
		// A deadline or exceeded dimension must not hide later consumed dimensions.
		UE::UAI::Contract::FDiagnostic MappingError;
		if (!ChargeMapped(Dimension, CoreUsage.Consumed(Dimension), MappingError))
			AccountingError.Append(MappingError);
	}
	if (!bParsed || !bProjected || !CoreDocument.IsValid() || !RootValue.IsValid())
	{
		OutError = CoreError.GetDiagnostic();
		if (OutError.IsEmpty()) OutError = CheckpointError;
		if (OutError.IsEmpty()) OutError = UE::UAI::Contract::FDiagnostic::Failure(
			TEXT("hjson_parse_failed"), TEXT("The HJSON document could not be parsed."), TEXT("hjson_parse"));
		OutError.DialectId = FUeAgentStructuredHjsonFormatter::DialectId();
		OutError.Append(AccountingError);
		return false;
	}
	if (!AccountingError.IsEmpty()) { OutError = MoveTemp(AccountingError); return false; }
	TSharedPtr<FJsonObject> ProjectedRoot;
	const TSharedPtr<FJsonObject>* ProjectedRootPtr = nullptr;
	if (CoreDocument->GetRoot()->GetKind() != EHjsonNodeKind::Object
		|| !RootValue->TryGetObject(ProjectedRootPtr)
		|| ProjectedRootPtr == nullptr || !ProjectedRootPtr->IsValid())
	{
		OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("hjson_root_must_be_object"),
			TEXT("The authoring document root must be an object."), TEXT("hjson_parse"));
		OutError.DialectId = FUeAgentStructuredHjsonFormatter::DialectId();
		OutError.Source.SourceFile = FilePath;
		OutError.Source.JsonPath = TEXT("/");
		OutError.Source.Line = CoreDocument->GetRoot()->GetProvenance().GetLine();
		OutError.Source.Column = CoreDocument->GetRoot()->GetProvenance().GetColumn();
		return false;
	}
	ProjectedRoot = *ProjectedRootPtr;
	TMap<FString, FUeAgentHjsonSourceLocation> ProjectedLocations;
	TArray<const FHjsonNode*> Pending;
	if (!ResourceTracker.ChargeEstimatedMemoryBytes(
			sizeof(FHjsonNode*), TEXT("/"), OutError))
	{
		return false;
	}
	Pending.Add(&CoreDocument->GetRoot().Get());
	while (!Pending.IsEmpty())
	{
		if (!ResourceTracker.Checkpoint(TEXT("hjson_source_locations"), OutError))
		{
			return false;
		}
		const FHjsonNode* Node = Pending.Pop(EAllowShrinking::No);
		const FSourceProvenance& Provenance = Node->GetProvenance();
		const int64 EntryBytes = sizeof(TPair<FString, FUeAgentHjsonSourceLocation>)
			+ static_cast<int64>(Provenance.GetJsonPointer().Len()
				+ Provenance.GetSourceUnit().Len()) * sizeof(TCHAR);
		const int64 ChildCount = Node->GetItems().Num() + Node->GetEntries().Num();
		if (!ResourceTracker.ChargeEstimatedMemoryBytes(
				EntryBytes + ChildCount * static_cast<int64>(sizeof(FHjsonNode*)),
				Provenance.GetJsonPointer(), OutError)
			|| !ResourceTracker.ChargeWorkUnits(
				1 + ChildCount, Provenance.GetJsonPointer(), OutError))
		{
			return false;
		}
		ProjectedLocations.Add(Provenance.GetJsonPointer(), {
			Provenance.GetSourceUnit(), Provenance.GetLine(), Provenance.GetColumn()});
		for (const FHjsonNodeRef& Item : Node->GetItems()) Pending.Add(&Item.Get());
		for (const TPair<FString, FHjsonNodeRef>& Entry : Node->GetEntries()) Pending.Add(&Entry.Value.Get());
	}
	if (!ResourceTracker.Checkpoint(TEXT("/"), OutError))
	{
		return false;
	}
	if (!ResourceTracker.ChargeEstimatedMemoryBytes(
			static_cast<int64>(Text.Len()) * sizeof(TCHAR), TEXT("/"), OutError))
	{
		return false;
	}
	OutDocument.Root = MoveTemp(ProjectedRoot);
	OutDocument.SourceFile = FilePath;
	OutDocument.SourceText = Text;
	OutDocument.SourceLocations = MoveTemp(ProjectedLocations);
	return true;
}

bool FUeAgentStructuredHjsonParser::ParseObject(
	const FString& Text,
	const FString& FilePath,
	TSharedPtr<FJsonObject>& OutObject,
	UE::UAI::Contract::FDiagnostic& OutError)
{
	FUeAgentStructuredHjsonDocument Document;
	if (!ParseDocument(
			Text,
			FilePath,
			Document,
			OutError))
	{
		OutObject.Reset();
		return false;
	}
	OutObject = Document.Root;
	return true;
}

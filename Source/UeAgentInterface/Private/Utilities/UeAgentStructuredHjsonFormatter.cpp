#include "Utilities/UeAgentStructuredHjsonFormatter.h"

#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"

#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include <charconv>
#include <cmath>
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

	static bool TryFormatFiniteDouble(
		const double Value,
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
		const FString Lower = Token.ToLower();
		if (Lower == TEXT("nan") || Lower == TEXT("infinity") || Lower == TEXT("+infinity")
			|| Lower == TEXT("-infinity") || Lower == TEXT("inf") || Lower == TEXT("+inf")
			|| Lower == TEXT("-inf"))
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

	static int64 EscapedQuotedStringLength(const FString& Value)
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

	static FString AppendJsonPointer(const FString& Parent, const FString& Segment)
	{
		const int64 ExpectedLength =
			AppendedJsonPointerLength(Parent, Segment);
		FString Result;
		if (ExpectedLength > 0 && ExpectedLength <= MAX_int32)
		{
			Result.Reserve(static_cast<int32>(ExpectedLength));
		}
		if (Parent != TEXT("/"))
		{
			Result += Parent;
		}
		Result.AppendChar(TEXT('/'));
		for (const TCHAR Ch : Segment)
		{
			if (Ch == TEXT('~'))
			{
				Result += TEXT("~0");
			}
			else if (Ch == TEXT('/'))
			{
				Result += TEXT("~1");
			}
			else
			{
				Result.AppendChar(Ch);
			}
		}
		return Result;
	}

	// A borrowed view keeps both supported inputs on the same writer and accounting path.
	// It never materializes a second tree (FJsonObject cannot represent exact-case keys).
	class FWriterNodeView
	{
	public:
		using FCanonical = UE::UAI::StateSemantic::FUeAgentCanonicalValue;
		FWriterNodeView() = default;
		FWriterNodeView(const TSharedPtr<FJsonValue>& Value) : FWriterNodeView(Value.Get()) {}
		FWriterNodeView(const TSharedRef<FJsonValue>& Value) : FWriterNodeView(&Value.Get()) {}
		FWriterNodeView(const TSharedPtr<FJsonObject>& Object) : FWriterNodeView(Object.Get()) {}
		FWriterNodeView(const TSharedRef<FJsonObject>& Object) : FWriterNodeView(&Object.Get()) {}
		FWriterNodeView(const FJsonValue* Value) : Json(Value), Type(Value ? Value->Type : EJson::None) {}
		FWriterNodeView(const FJsonObject* Object) : JsonObject(Object), Type(Object ? EJson::Object : EJson::None) {}
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
		const FWriterNodeView* operator->() const { return this; }
		bool IsValid() const { return Json || JsonObject || Canonical; }
		bool IsUnsupported() const { return Canonical && Type == EJson::None; }
		const void* Get() const { return Canonical ? static_cast<const void*>(Canonical) : JsonObject ? static_cast<const void*>(JsonObject) : Json; }
		FWriterNodeView AsObject() const { return Canonical || JsonObject ? *this : FWriterNodeView(Json->AsObject()); }
		FString AsString() const { return Canonical ? Canonical->GetText() : Json->AsString(); }
		bool AsBool() const { return Canonical ? Canonical->GetBoolean() : Json->AsBool(); }
		double AsNumber() const { return Json ? Json->AsNumber() : 0.0; }
		bool PreferStringRepresentation() const { return Canonical || (Json && Json->PreferStringRepresentation()); }
		bool TryGetString(FString& Out) const { if (Canonical) { Out = Canonical->GetLexeme(); return Type == EJson::Number; } return Json && Json->TryGetString(Out); }
		SIZE_T GetMemoryFootprint() const { return Canonical ? sizeof(FCanonical) + Canonical->GetText().GetAllocatedSize() + Canonical->GetLexeme().GetAllocatedSize() : Json->GetMemoryFootprint(); }
		int32 NumFields() const { return Canonical ? Canonical->GetObjectItems().Num() : JsonObject->Values.Num(); }
		void GetKeys(TArray<FString>& Out) const
		{
			if (Canonical) { for (const auto& Field : Canonical->GetObjectItems()) { Out.Add(Field.Key); } }
			else { JsonObject->Values.GetKeys(Out); }
		}
		FWriterNodeView FindField(const FString& Key) const
		{
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
			return JsonObject->Values.FindRef(Key);
		}
		bool HasField(const FString& Key) const { return FindField(Key).IsValid(); }
		struct FFields;
		struct FArray;
		FFields Fields() const;
		FArray AsArray() const;
		const FJsonValue* Json = nullptr;
		const FJsonObject* JsonObject = nullptr;
		const FCanonical* Canonical = nullptr;
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
			FIterator(FWriterNodeView InObject, int32 InIndex) : Object(InObject), Index(InIndex)
			{
				if (!Object.Canonical && Index < Object.NumFields()) { JsonIterator.Emplace(Object.JsonObject->Values.CreateConstIterator()); }
			}
			bool operator!=(const FIterator& Other) const { return Index != Other.Index; }
			void operator++() { ++Index; if (JsonIterator.IsSet()) { ++JsonIterator.GetValue(); } }
			FField operator*() const
			{
				if (Object.Canonical) { const auto& Field = Object.Canonical->GetObjectItems()[Index]; return {Field.Key, FWriterNodeView(Field.Value.Get())}; }
				const auto& Field = *JsonIterator.GetValue(); return {Field.Key, FWriterNodeView(Field.Value)};
			}
		};
		FIterator begin() const { return {Object, 0}; }
		FIterator end() const { return {Object, Object.NumFields()}; }
	};
	struct FWriterNodeView::FArray
	{
		FWriterNodeView Value;
		int32 Num() const { return Value.Canonical ? Value.Canonical->GetArrayItems().Num() : Value.Json->AsArray().Num(); }
		FWriterNodeView operator[](int32 Index) const { return Value.Canonical ? FWriterNodeView(Value.Canonical->GetArrayItems()[Index].Get()) : FWriterNodeView(Value.Json->AsArray()[Index]); }
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

	static bool HasExactKeySet(const TArray<FString>& Keys, const TArray<FString>& ExpectedKeys)
	{
		if (Keys.Num() != ExpectedKeys.Num())
		{
			return false;
		}

		for (const FString& ExpectedKey : ExpectedKeys)
		{
			if (!Keys.ContainsByPredicate([&](const FString& Key) { return Key.Equals(ExpectedKey, ESearchCase::CaseSensitive); }))
			{
				return false;
			}
		}
		return true;
	}

	static bool TryGetTupleKeyOrder(const TArray<FString>& Keys, TArray<FString>& OutKeys)
	{
		static const TArray<FString> Pos2DKeys = { TEXT("x"), TEXT("y") };
		static const TArray<FString> Pos3DKeys = { TEXT("x"), TEXT("y"), TEXT("z") };
		static const TArray<FString> RotationKeys = { TEXT("pitch"), TEXT("yaw"), TEXT("roll") };
		static const TArray<FString> ColorKeys = { TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a") };

		if (HasExactKeySet(Keys, Pos2DKeys))
		{
			OutKeys = Pos2DKeys;
			return true;
		}
		if (HasExactKeySet(Keys, Pos3DKeys))
		{
			OutKeys = Pos3DKeys;
			return true;
		}
		if (HasExactKeySet(Keys, RotationKeys))
		{
			OutKeys = RotationKeys;
			return true;
		}
		if (HasExactKeySet(Keys, ColorKeys))
		{
			OutKeys = ColorKeys;
			return true;
		}
		return false;
	}

	struct FCanonicalFragmentProof
	{
		TSharedPtr<const UE::UAI::StateSemantic::FUeAgentCanonicalValue> Value;
		FString Text;
		int32 Level = 0;
		int32 InlineCount = 0;
		int32 InlineLength = 0;
		int32 InlineDepth = 0;
		FUeAgentStructuredResourceUsage AnalysisCharges;
		FUeAgentStructuredResourceUsage RenderCharges;
	};
	struct FCanonicalFragmentCache
	{
		FUeAgentStructuredHjsonWriteOptions::FStableLeafProjection Projection = nullptr;
		TMap<FString, TArray<FString>> KeyOrder;
		TMap<FString, TSharedPtr<const FCanonicalFragmentProof>> Fragments;
	};
	static thread_local TSharedPtr<const FCanonicalFragmentCache> PreviousCanonicalFragments;

	class FHjsonWriter
	{
	public:
		explicit FHjsonWriter(
			const FUeAgentStructuredHjsonWriteOptions* InOptions = nullptr,
			FUeAgentStructuredResourceTracker* InResourceTracker = nullptr)
			: Options(InOptions)
			, OwnedResourceTracker(
				InResourceTracker
					? nullptr
					: MakeUnique<FUeAgentStructuredResourceTracker>())
			, ResourceTracker(
				InResourceTracker
					? *InResourceTracker
					: *OwnedResourceTracker)
		{
		}

		bool TryWriteRootObject(
			const FWriterNodeView& Object,
			FString& OutText,
			UE::UAI::Contract::FDiagnostic& OutError) const
		{
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
			TArray<FWriteTask> Tasks;
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
			const FWriterNodeView& Value,
			FString& OutText,
			UE::UAI::Contract::FDiagnostic& OutError) const
		{
			Error.Reset();
			BeginFragmentReuse(Value, false);
			if (!AnalyzeRootValue(Value))
			{
				OutText.Reset();
				OutError = Error;
				return false;
			}
			TArray<FWriteTask> Tasks;
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
			CandidateLevel = bRootObject ? 1 : 2;
			for (const auto& Field : Root.Canonical->GetObjectItems())
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
			if (PreviousCanonicalFragments && PreviousCanonicalFragments->Projection == NextFragments->Projection
				&& PreviousCanonicalFragments->KeyOrder.OrderIndependentCompareEqual(NextFragments->KeyOrder))
				PriorFragments = PreviousCanonicalFragments;
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
			PreviousCanonicalFragments = MoveTemp(NextFragments);
		}

		enum class EWriteTaskType : uint8
		{
			Text,
			Value,
			ObjectBody,
			FragmentExit,
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

		bool AnalyzeRootObject(const FWriterNodeView& Object) const
		{
			ResetAnalysis();
			TArray<FAnalysisTask> Stack;
			Stack.Add(FAnalysisTask::ObjectTask(
				EAnalysisTaskType::ObjectEnter,
				Object,
				TEXT("/")));
			return AnalyzeTasks(Stack);
		}

		bool AnalyzeRootValue(const FWriterNodeView& Value) const
		{
			ResetAnalysis();
			TArray<FAnalysisTask> Stack;
			Stack.Add(FAnalysisTask::ValueEnter(Value, TEXT("/")));
			return AnalyzeTasks(Stack);
		}

		void ResetAnalysis() const
		{
			ObjectInlineCountCache.Reset();
			ArrayInlineCountCache.Reset();
			ObjectInlineLengthCache.Reset();
			ArrayInlineLengthCache.Reset();
			ObjectInlineDepthCache.Reset();
			ArrayInlineDepthCache.Reset();
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
			const int64 PointerChars =
				AppendedJsonPointerLength(Parent, Segment);
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
			OutPointer = AppendJsonPointer(Parent, Segment);
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
			FString& StringValue,
			const FString& JsonPointer,
			const bool bInlineContext,
			const bool bPreflight,
			FWriteTask& OutTask) const
		{
			const bool bBare =
				!bInlineContext && FUeAgentStructuredHjsonFormatter::IsBareString(StringValue);
			const int64 CanonicalChars = bBare
				? StringValue.Len()
				: EscapedQuotedStringLength(StringValue);
			if (bPreflight && !ChargeCanonicalTextResources(CanonicalChars, JsonPointer))
			{
				return false;
			}
			FString CanonicalText = bBare
				? MoveTemp(StringValue)
				: FUeAgentStructuredHjsonFormatter::EscapeQuotedString(
					StringValue);
			OutTask = FWriteTask::TextTask(
				MoveTemp(CanonicalText),
				JsonPointer,
				bPreflight);
			return true;
		}

		bool AnalyzeTasks(TArray<FAnalysisTask>& Stack) const
		{
			while (Stack.Num() > 0 && Error.IsEmpty())
			{
				FAnalysisTask Task = Stack.Pop(EAllowShrinking::No);
				if (Task.Type == EAnalysisTaskType::FragmentExit)
				{
					auto* Pending = PendingFragments.Find(Task.JsonPointer);
					const auto* Count = ObjectInlineCountCache.Find(Task.Value.Get());
					const auto* Length = ObjectInlineLengthCache.Find(Task.Value.Get());
					const auto* Depth = ObjectInlineDepthCache.Find(Task.Value.Get());
					if (Pending && Count && Length && Depth
						&& UsageDelta(Pending->AnalysisBefore, ResourceTracker.GetUsage(), Pending->Proof->AnalysisCharges))
					{
						Pending->Proof->InlineCount = *Count;
						Pending->Proof->InlineLength = *Length;
						Pending->Proof->InlineDepth = *Depth;
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
						if (Prior && (*Prior)->Value.Get() == Task.Value.Canonical && (*Prior)->Level == CandidateLevel)
						{
							if (!ReplayUsage((*Prior)->AnalysisCharges, Task.JsonPointer)) return false;
							ObjectInlineCountCache.Add(Task.Value.Get(), (*Prior)->InlineCount);
							ObjectInlineLengthCache.Add(Task.Value.Get(), (*Prior)->InlineLength);
							ObjectInlineDepthCache.Add(Task.Value.Get(), (*Prior)->InlineDepth);
							AnalyzedFragments.Add(Task.JsonPointer, *Prior);
							continue;
						}
						FPendingFragment Pending;
						Pending.Proof = MakeShared<FCanonicalFragmentProof>();
						Pending.Proof->Value = *Candidate;
						Pending.Proof->Level = CandidateLevel;
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
			TArray<FAnalysisTask>& Stack,
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
				const FString StringValue = Value->AsString();
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
			TArray<FAnalysisTask>& Stack,
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
			for (const auto& Field : Object->Fields())
			{
				KeyChars += Field.Key.Len();
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
			for (const auto& Field : Object->Fields())
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

			TArray<FString> Keys;
			Object->GetKeys(Keys);
			Keys.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });
			for (int32 Index = Keys.Num() - 1; Index >= 0; --Index)
			{
				const FString& Key = Keys[Index];
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
					Object->FindField(Key),
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
			if (!ObjectInlineCountCache.Contains(Object.Get()))
			{
				int32 Count = Object->NumFields() == 0 ? 1 : 0;
				for (const auto& Field : Object->Fields())
				{
					Count = FMath::Min(
						InlineObjectFieldLimit + 1,
						Count + InlineCountForValue(Field.Value, JsonPointer));
					if (Count > InlineObjectFieldLimit)
					{
						break;
					}
				}
				ObjectInlineCountCache.Add(Object.Get(), Count);
			}
			if (!ObjectInlineLengthCache.Contains(Object.Get()))
			{
				int32 Length = Object->NumFields() == 0 ? 2 : 4;
				int32 Depth = 1;
				for (const auto& Field : Object->Fields())
				{
					const int32 KeyLength = static_cast<int32>(FMath::Min<int64>(
						InlineContainerCharacterLimit + 1,
						FUeAgentStructuredHjsonFormatter::IsBareKey(Field.Key)
						? Field.Key.Len()
						: EscapedQuotedStringLength(Field.Key)));
					Length = static_cast<int32>(FMath::Min<int64>(
						InlineContainerCharacterLimit + 1,
						static_cast<int64>(Length) + KeyLength + 2
							+ InlineLengthForValue(Field.Value, JsonPointer)));
					Depth = FMath::Max(
						Depth,
						1 + InlineDepthForValue(Field.Value, JsonPointer));
				}
				if (Object->NumFields() > 1)
				{
					Length = static_cast<int32>(FMath::Min<int64>(
						InlineContainerCharacterLimit + 1,
						static_cast<int64>(Length)
							+ 2LL * (Object->NumFields() - 1)));
				}
				ObjectInlineLengthCache.Add(Object.Get(), Length);
				ObjectInlineDepthCache.Add(Object.Get(), Depth);
			}
			VisitingObjects.Remove(Object.Get());
		}

		bool AnalyzeArrayEnter(
			TArray<FAnalysisTask>& Stack,
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
			if (!ArrayInlineCountCache.Contains(ArrayValue.Get()))
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
						const int32* ChildCount =
							ArrayInlineCountCache.Find(Value.Get());
						if (!ChildCount)
						{
							SetWriteError(
								TEXT("hjson_internal_error"),
								JsonPointer,
								TEXT("missing inline array summary"));
							return;
						}
						if (*ChildCount > InlineObjectFieldLimit)
						{
							bContainsObject = true;
							break;
						}
					}
				}
				ArrayInlineCountCache.Add(
					ArrayValue.Get(),
					bContainsObject ? InlineObjectFieldLimit + 1 : 1);
			}
			if (!ArrayInlineLengthCache.Contains(ArrayValue.Get()))
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
				}
				if (Values.Num() > 1)
				{
					Length = static_cast<int32>(FMath::Min<int64>(
						InlineContainerCharacterLimit + 1,
						static_cast<int64>(Length) + 2LL * (Values.Num() - 1)));
				}
				ArrayInlineLengthCache.Add(ArrayValue.Get(), Length);
				ArrayInlineDepthCache.Add(ArrayValue.Get(), Depth);
			}
			VisitingArrays.Remove(ArrayValue.Get());
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
				const FString Text = Value->AsString();
				return static_cast<int32>(FMath::Min<int64>(
					InlineContainerCharacterLimit + 1,
					EscapedQuotedStringLength(Text)));
			}
			case EJson::Number:
			{
				FString NumberText;
				if (Value->PreferStringRepresentation())
				{
					FString Token;
					bool bNonFinite = false;
					if (!Value->TryGetString(Token)
						|| !TryCanonicalizeNumberToken(Token, NumberText, bNonFinite))
					{
						return InlineContainerCharacterLimit + 1;
					}
				}
				else if (!TryFormatFiniteDouble(Value->AsNumber(), false, NumberText))
				{
					return InlineContainerCharacterLimit + 1;
				}
				return NumberText.Len();
			}
			case EJson::Boolean:
				return Value->AsBool() ? 4 : 5;
			case EJson::Array:
				if (const int32* Length = ArrayInlineLengthCache.Find(Value.Get()))
				{
					return *Length;
				}
				break;
			case EJson::Object:
			{
				const FWriterNodeView Object = Value->AsObject();
				if (!Object.IsValid())
				{
					return 4;
				}
				if (const int32* Length = ObjectInlineLengthCache.Find(Object.Get()))
				{
					return *Length;
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
				if (const int32* Depth = ArrayInlineDepthCache.Find(Value.Get()))
				{
					return *Depth;
				}
			}
			else if (Value->Type == EJson::Object)
			{
				const FWriterNodeView Object = Value->AsObject();
				if (!Object.IsValid())
				{
					return 0;
				}
				if (const int32* Depth = ObjectInlineDepthCache.Find(Object.Get()))
				{
					return *Depth;
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
				if (const int32* Count = ArrayInlineCountCache.Find(Value.Get()))
				{
					return *Count;
				}
			}
			else if (Value->Type == EJson::Object)
			{
				const FWriterNodeView Object = Value->AsObject();
				if (!Object.IsValid())
				{
					return 1;
				}
				if (const int32* Count = ObjectInlineCountCache.Find(Object.Get()))
				{
					return *Count;
				}
			}
			SetWriteError(
				TEXT("hjson_internal_error"),
				JsonPointer,
				TEXT("missing inline container summary"));
			return InlineObjectFieldLimit + 1;
		}

		bool RenderChecked(TArray<FWriteTask>& Stack, FString& Out) const
		{
			// AnalyzeRootValue has already validated the complete tree. Keep every
			// render check and resource charge, then consume each charged text token
			// directly instead of discarding it and formatting the tree a second time.
			// Callers clear the private partial result on any failure.
			Out.Reset();
			int32 TasksUntilCheckpoint = 1024;
			while (Stack.Num() > 0 && Error.IsEmpty())
			{
				FWriteTask Task = Stack.Pop(EAllowShrinking::No);
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
						if ((*Prior)->Value.Get() == Task.Value.Canonical && (*Prior)->Level == Task.Level
							&& Task.FieldName.IsEmpty() && !Task.bInlineContext)
						{
							if (!ReplayUsage((*Prior)->RenderCharges, Task.JsonPointer)) return false;
							Out += (*Prior)->Text;
							KeepFragment(Task.JsonPointer, *Prior);
							++ReusedFragmentCount;
							continue;
						}
						// A future writer layout change may invalidate the predicted
						// context. Rebuild that subtree's summaries before normal render.
						FragmentCandidates.Remove(Task.JsonPointer);
						AnalyzedFragments.Remove(Task.JsonPointer);
						TArray<FAnalysisTask> Analysis;
						Analysis.Add(FAnalysisTask::ValueEnter(Task.Value, Task.JsonPointer));
						if (!AnalyzeTasks(Analysis)) return false;
					}
					if (auto* Pending = PendingFragments.Find(Task.JsonPointer))
					{
						if (Pending->Proof->Level == Task.Level && Task.FieldName.IsEmpty() && !Task.bInlineContext)
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
				if (Task.Type == EWriteTaskType::Text)
				{
					if (!Task.bTextResourcesPrecharged
						&& !ChargeCanonicalTextResources(
							Task.Text.Len(),
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
					Out += Task.Text;
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
					break; // Consumed before dispatch.
				}
			}
			return Error.IsEmpty()
				&& ResourceTracker.Checkpoint(TEXT("/"), Error);
		}

		void PushValue(
			TArray<FWriteTask>& Stack,
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
				Stack.Add(FWriteTask::TextTask(TEXT("null"), JsonPointer));
				return;
			}
			switch (Value->Type)
			{
			case EJson::String:
			{
				FString StringValue = Value->AsString();
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
				if (Value->PreferStringRepresentation())
				{
					FString Token;
					bool bNonFinite = false;
					if (!Value->TryGetString(Token)
						|| !TryCanonicalizeNumberToken(Token, NumberText, bNonFinite))
					{
						SetWriteError(
							bNonFinite ? TEXT("hjson_non_finite_number") : TEXT("hjson_invalid_number"),
							JsonPointer,
							bNonFinite ? TEXT("non-finite authoring number") : TEXT("invalid authoring number"));
						return;
					}
				}
				else if (!TryFormatFiniteDouble(Value->AsNumber(), false, NumberText))
				{
					SetWriteError(
						TEXT("hjson_non_finite_number"),
						JsonPointer,
						TEXT("non-finite authoring number"));
					return;
				}
				Stack.Add(FWriteTask::TextTask(
					MoveTemp(NumberText),
					JsonPointer));
				return;
			}
			case EJson::Boolean:
				Stack.Add(FWriteTask::TextTask(
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
				Stack.Add(FWriteTask::TextTask(TEXT("null"), JsonPointer));
				return;
			}
		}

		void OrderedKeys(
			const FWriterNodeView& Object,
			const FString& JsonPointer,
			TArray<FString>& OutKeys) const
		{
			OutKeys.Reset();
			if (!Object.IsValid())
			{
				return;
			}
			Object->GetKeys(OutKeys);
			TArray<FString> TupleKeys;
			if (TryGetTupleKeyOrder(OutKeys, TupleKeys))
			{
				OutKeys = MoveTemp(TupleKeys);
				return;
			}
			TArray<FString> Remaining = MoveTemp(OutKeys);
			Remaining.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });
			OutKeys.Reset();
			if (Options)
			{
				if (const TArray<FString>* Preferred = Options->PreferredKeyOrderByJsonPointer.Find(JsonPointer))
				{
					for (const FString& Key : *Preferred)
					{
						if (Remaining.ContainsByPredicate([&](const FString& Existing) { return Existing.Equals(Key, ESearchCase::CaseSensitive); }))
						{
							OutKeys.Add(Key);
							Remaining.RemoveAll([&](const FString& Existing) { return Existing.Equals(Key, ESearchCase::CaseSensitive); });
						}
					}
				}
			}
			OutKeys.Append(Remaining);
		}

		void PushObjectBody(
			TArray<FWriteTask>& Stack,
			const FWriterNodeView& Object,
			const int32 Level,
			const FString& JsonPointer,
			const bool bPreflight) const
		{
			TArray<FString> Keys;
			OrderedKeys(Object, JsonPointer, Keys);
			TArray<FWriteTask> Forward;
			for (int32 Index = 0; Index < Keys.Num(); ++Index)
			{
				if (Index > 0)
				{
					Forward.Add(FWriteTask::TextTask(
						TEXT("\n"),
						JsonPointer));
				}
				const FString& Key = Keys[Index];
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
				Forward.Add(FWriteTask::TextTask(
					TEXT(": "),
					ValuePointer));
				Forward.Add(FWriteTask::ValueTask(
					Object->FindField(Key),
					Level,
					Key,
					ValuePointer));
			}
			PushForwardTasks(Stack, Forward);
		}

		void PushObject(
			TArray<FWriteTask>& Stack,
			const FWriterNodeView& Object,
			const int32 Level,
			const FString& FieldName,
			const FString& JsonPointer,
			const bool bPreflight) const
		{
			(void)FieldName;
			if (!Object.IsValid() || Object->NumFields() == 0)
			{
				Stack.Add(FWriteTask::TextTask(TEXT("{}"), JsonPointer));
				return;
			}
			TArray<FWriteTask> Forward;
			if (CanInlineObject(Object, JsonPointer))
			{
				TArray<FString> Keys;
				OrderedKeys(Object, JsonPointer, Keys);
				Forward.Add(FWriteTask::TextTask(
					TEXT("{ "),
					JsonPointer));
				for (int32 Index = 0; Index < Keys.Num(); ++Index)
				{
					if (Index > 0)
					{
						Forward.Add(FWriteTask::TextTask(
							TEXT(", "),
							JsonPointer));
					}
					const FString& Key = Keys[Index];
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
					Forward.Add(FWriteTask::TextTask(
						TEXT(": "),
						ValuePointer));
					Forward.Add(FWriteTask::ValueTask(
						Object->FindField(Key),
						0,
						Key,
						ValuePointer,
						true));
				}
				Forward.Add(FWriteTask::TextTask(
					TEXT(" }"),
					JsonPointer));
			}
			else
			{
				Forward.Add(FWriteTask::TextTask(
					TEXT("{\n"),
					JsonPointer));
				Forward.Add(FWriteTask::ObjectBody(Object, Level + 1, JsonPointer));
				Forward.Add(FWriteTask::TextTask(
					TEXT("\n") + Padding(Level) + TEXT("}"),
					JsonPointer));
			}
			PushForwardTasks(Stack, Forward);
		}

		void PushArray(
			TArray<FWriteTask>& Stack,
			const FWriterNodeView& ArrayValue,
			const int32 Level,
			const FString& JsonPointer,
			const bool bPreflight) const
		{
			const auto Values =
				ArrayValue->AsArray();
			if (Values.Num() == 0)
			{
				Stack.Add(FWriteTask::TextTask(TEXT("[]"), JsonPointer));
				return;
			}
			TArray<FWriteTask> Forward;
			Forward.Add(FWriteTask::TextTask(TEXT("["), JsonPointer));
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
						Forward.Add(FWriteTask::TextTask(
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
			Forward.Add(FWriteTask::TextTask(
				bInline ? TEXT("]") : TEXT("\n") + Padding(Level) + TEXT("]"),
				JsonPointer));
			PushForwardTasks(Stack, Forward);
		}

		static void PushForwardTasks(TArray<FWriteTask>& Stack, TArray<FWriteTask>& Forward)
		{
			for (int32 Index = Forward.Num() - 1; Index >= 0; --Index)
			{
				Stack.Add(MoveTemp(Forward[Index]));
			}
		}

		bool CanInlineObject(
			const FWriterNodeView& Object,
			const FString& JsonPointer) const
		{
			if (!Object.IsValid() || Object->NumFields() == 0)
			{
				return true;
			}
			if (const int32* Count =
					ObjectInlineCountCache.Find(Object.Get()))
			{
				const int32* Length = ObjectInlineLengthCache.Find(Object.Get());
				const int32* Depth = ObjectInlineDepthCache.Find(Object.Get());
				return Length && Depth
					&& *Count <= InlineObjectFieldLimit
					&& *Length <= InlineContainerCharacterLimit
					&& *Depth <= InlineContainerDepthLimit;
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
			if (const int32* Count =
					ArrayInlineCountCache.Find(ArrayValue.Get()))
			{
				const int32* Length = ArrayInlineLengthCache.Find(ArrayValue.Get());
				const int32* Depth = ArrayInlineDepthCache.Find(ArrayValue.Get());
				return Length && Depth
					&& *Count <= InlineObjectFieldLimit
					&& *Length <= InlineContainerCharacterLimit
					&& *Depth <= InlineContainerDepthLimit;
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
		FUeAgentStructuredResourceTracker& ResourceTracker;
		mutable TMap<const void*, int32> ObjectInlineCountCache;
		mutable TMap<const void*, int32> ArrayInlineCountCache;
		mutable TMap<const void*, int32> ObjectInlineLengthCache;
		mutable TMap<const void*, int32> ArrayInlineLengthCache;
		mutable TMap<const void*, int32> ObjectInlineDepthCache;
		mutable TMap<const void*, int32> ArrayInlineDepthCache;
		mutable TSet<const void*> VisitingObjects;
		mutable TSet<const void*> VisitingArrays;
		mutable UE::UAI::Contract::FDiagnostic Error;
		mutable TMap<const FWriterNodeView::FCanonical*, TSharedPtr<FJsonValue>> LeafProjections;
		mutable TMap<FString, TSharedPtr<const FWriterNodeView::FCanonical>> FragmentCandidates;
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
	const int64 ExpectedLength = EscapedQuotedStringLength(Value);
	if (ExpectedLength > 0 && ExpectedLength <= MAX_int32)
	{
		Out.Reserve(static_cast<int32>(ExpectedLength));
	}
	Out.AppendChar(TEXT('"'));
	for (const TCHAR Ch : Value)
	{
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
			if (Ch < 0x20)
			{
				Out += FString::Printf(TEXT("\\u%04x"), static_cast<uint32>(Ch));
			}
			else
			{
				Out.AppendChar(Ch);
			}
			break;
		}
	}
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

#include "Utilities/UeAgentStructuredHjsonFormatter.h"

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
		const double Value = FCString::Atod(*Token);
		if (!std::isfinite(Value))
		{
			bOutNonFinite = true;
			return false;
		}
		return TryFormatFiniteDouble(Value, true, OutCanonical);
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

	static bool IsScalarValue(const TSharedPtr<FJsonValue>& Value)
	{
		return !Value.IsValid()
			|| Value->Type == EJson::None
			|| Value->Type == EJson::Null
			|| Value->Type == EJson::String
			|| Value->Type == EJson::Number
			|| Value->Type == EJson::Boolean;
	}

	static constexpr int32 InlineObjectFieldLimit = 6;

	static bool HasExactKeySet(const TArray<FString>& Keys, const TArray<FString>& ExpectedKeys)
	{
		if (Keys.Num() != ExpectedKeys.Num())
		{
			return false;
		}

		for (const FString& ExpectedKey : ExpectedKeys)
		{
			if (!Keys.Contains(ExpectedKey))
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
					: MakeUnique<FUeAgentStructuredResourceTracker>(
						InOptions
							? InOptions->ResourceBudget
							: FUeAgentStructuredResourceBudget()))
			, ResourceTracker(
				InResourceTracker
					? *InResourceTracker
					: *OwnedResourceTracker)
		{
		}

		bool TryWriteRootObject(
			const TSharedRef<FJsonObject>& Object,
			FString& OutText,
			FString& OutError) const
		{
			Error.Reset();
			if (!AnalyzeRootObject(Object))
			{
				OutText.Reset();
				OutError = Error;
				return false;
			}
			if (Object->Values.Num() == 0)
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
			if (!PreflightRender(Tasks))
			{
				OutText.Reset();
				OutError = Error;
				return false;
			}
			Tasks.Add(FWriteTask::ObjectBody(Object, 0, TEXT("/")));
			OutText = Render(Tasks);
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
			return true;
		}

		bool TryWriteValue(
			const TSharedRef<FJsonValue>& Value,
			FString& OutText,
			FString& OutError) const
		{
			Error.Reset();
			if (!AnalyzeRootValue(Value))
			{
				OutText.Reset();
				OutError = Error;
				return false;
			}
			TArray<FWriteTask> Tasks;
			Tasks.Add(FWriteTask::ValueTask(Value, 0, FString(), TEXT("/")));
			if (!PreflightRender(Tasks))
			{
				OutText.Reset();
				OutError = Error;
				return false;
			}
			Tasks.Add(FWriteTask::ValueTask(Value, 0, FString(), TEXT("/")));
			OutText = Render(Tasks);
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
			return true;
		}

	private:
		enum class EWriteTaskType : uint8
		{
			Text,
			Value,
			ObjectBody,
		};

		struct FWriteTask
		{
			EWriteTaskType Type = EWriteTaskType::Text;
			FString Text;
			TSharedPtr<FJsonValue> Value;
			TSharedPtr<FJsonObject> Object;
			int32 Level = 0;
			FString FieldName;
			FString JsonPointer;
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
				TSharedPtr<FJsonValue> InValue,
				const int32 InLevel,
				FString InFieldName,
				FString InJsonPointer)
			{
				FWriteTask Task;
				Task.Type = EWriteTaskType::Value;
				Task.Value = MoveTemp(InValue);
				Task.Level = InLevel;
				Task.FieldName = MoveTemp(InFieldName);
				Task.JsonPointer = MoveTemp(InJsonPointer);
				return Task;
			}

			static FWriteTask ObjectBody(
				TSharedPtr<FJsonObject> InObject,
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
		};

		struct FAnalysisTask
		{
			EAnalysisTaskType Type = EAnalysisTaskType::ValueEnter;
			TSharedPtr<FJsonValue> Value;
			TSharedPtr<FJsonObject> Object;
			FString JsonPointer = TEXT("/");

			static FAnalysisTask ValueEnter(
				TSharedPtr<FJsonValue> InValue,
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
				TSharedPtr<FJsonObject> InObject,
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
				TSharedPtr<FJsonValue> InValue,
				FString InJsonPointer)
			{
				FAnalysisTask Task;
				Task.Type = InType;
				Task.Value = MoveTemp(InValue);
				Task.JsonPointer = MoveTemp(InJsonPointer);
				return Task;
			}
		};

		bool AnalyzeRootObject(const TSharedRef<FJsonObject>& Object) const
		{
			ResetAnalysis();
			TArray<FAnalysisTask> Stack;
			Stack.Add(FAnalysisTask::ObjectTask(
				EAnalysisTaskType::ObjectEnter,
				Object,
				TEXT("/")));
			return AnalyzeTasks(Stack);
		}

		bool AnalyzeRootValue(const TSharedRef<FJsonValue>& Value) const
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
			const bool bPreflight,
			FWriteTask& OutTask) const
		{
			const bool bBare =
				FUeAgentStructuredHjsonFormatter::IsBareString(StringValue);
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
				}
			}
			return Error.IsEmpty()
				&& ResourceTracker.Checkpoint(TEXT("/"), Error);
		}

		bool AnalyzeValue(
			TArray<FAnalysisTask>& Stack,
			const TSharedPtr<FJsonValue>& Value,
			const FString& JsonPointer) const
		{
			if (Value.IsValid() && Value->Type == EJson::Object)
			{
				const TSharedPtr<FJsonObject> Object = Value->AsObject();
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
			int64 CanonicalStringChars = INDEX_NONE;
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
				CanonicalStringChars =
					FUeAgentStructuredHjsonFormatter::IsBareString(
						StringValue)
					? DynamicChars
					: EscapedQuotedStringLength(StringValue);
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
			return CanonicalStringChars == INDEX_NONE
				|| ChargeCanonicalTextResources(
					CanonicalStringChars,
					JsonPointer);
		}

		bool AnalyzeObjectEnter(
			TArray<FAnalysisTask>& Stack,
			const TSharedPtr<FJsonObject>& Object,
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

			const FJsonObject* Identity = Object.Get();
			if (VisitingObjects.Contains(Identity))
			{
				SetWriteError(
					TEXT("hjson_value_cycle"),
					JsonPointer,
					TEXT("cyclic authoring object"));
				return false;
			}
			int64 KeyChars = 0;
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Object->Values)
			{
				KeyChars += Field.Key.Len();
			}
			const int64 ItemCount = Object->Values.Num();
			const int64 EstimatedBytes =
				sizeof(FJsonObject)
				+ ItemCount
					* (sizeof(TPair<FString, TSharedPtr<FJsonValue>>)
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
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Object->Values)
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
			Object->Values.GetKeys(Keys);
			Keys.Sort();
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
					Object->Values.FindRef(Key),
					MoveTemp(ValuePointer)));
			}
			return true;
		}

		void AnalyzeObjectExit(
			const TSharedPtr<FJsonObject>& Object,
			const FString& JsonPointer) const
		{
			if (!Object.IsValid())
			{
				return;
			}
			if (!ObjectInlineCountCache.Contains(Object.Get()))
			{
				int32 Count = Object->Values.Num() == 0 ? 1 : 0;
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Object->Values)
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
			VisitingObjects.Remove(Object.Get());
		}

		bool AnalyzeArrayEnter(
			TArray<FAnalysisTask>& Stack,
			const TSharedPtr<FJsonValue>& ArrayValue,
			const FString& JsonPointer) const
		{
			if (!ArrayValue.IsValid() || ArrayValue->Type != EJson::Array)
			{
				return AnalyzeValue(Stack, ArrayValue, JsonPointer);
			}
			const FJsonValue* Identity = ArrayValue.Get();
			if (VisitingArrays.Contains(Identity))
			{
				SetWriteError(
					TEXT("hjson_value_cycle"),
					JsonPointer,
					TEXT("cyclic authoring array"));
				return false;
			}
			const TArray<TSharedPtr<FJsonValue>>& Values = ArrayValue->AsArray();
			const int64 ItemCount = Values.Num();
			const int64 EstimatedBytes =
				sizeof(FJsonValue)
				+ ItemCount
					* (sizeof(TSharedPtr<FJsonValue>)
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
			const TSharedPtr<FJsonValue>& ArrayValue,
			const FString& JsonPointer) const
		{
			if (!ArrayValue.IsValid() || ArrayValue->Type != EJson::Array)
			{
				return;
			}
			if (!ArrayInlineCountCache.Contains(ArrayValue.Get()))
			{
				bool bContainsObject = false;
				for (const TSharedPtr<FJsonValue>& Value : ArrayValue->AsArray())
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
			VisitingArrays.Remove(ArrayValue.Get());
		}

		int32 InlineCountForValue(
			const TSharedPtr<FJsonValue>& Value,
			const FString& JsonPointer) const
		{
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
				const TSharedPtr<FJsonObject> Object = Value->AsObject();
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

		bool PreflightRender(TArray<FWriteTask>& Stack) const
		{
			while (Stack.Num() > 0 && Error.IsEmpty())
			{
				FWriteTask Task = Stack.Pop(EAllowShrinking::No);
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
						true);
					break;
				}
			}
			return Error.IsEmpty()
				&& ResourceTracker.Checkpoint(TEXT("/"), Error);
		}

		FString Render(TArray<FWriteTask>& Stack) const
		{
			FString Out;
			const int64 ExpectedOutputChars =
				ResourceTracker.GetUsage().OutputChars;
			if (ExpectedOutputChars > 0 && ExpectedOutputChars <= MAX_int32)
			{
				Out.Reserve(static_cast<int32>(ExpectedOutputChars));
			}
			int32 TasksUntilCheckpoint = 1024;
			while (Stack.Num() > 0 && Error.IsEmpty())
			{
				FWriteTask Task = Stack.Pop(EAllowShrinking::No);
				if (--TasksUntilCheckpoint == 0)
				{
					TasksUntilCheckpoint = 1024;
					if (!ResourceTracker.Checkpoint(
							Task.JsonPointer,
							Error))
					{
						break;
					}
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
						false);
					break;
				case EWriteTaskType::Value:
					PushValue(
						Stack,
						Task.Value,
						Task.Level,
						Task.FieldName,
						Task.JsonPointer,
						false);
					break;
				}
			}
			return Out;
		}

		void PushValue(
			TArray<FWriteTask>& Stack,
			const TSharedPtr<FJsonValue>& Value,
			const int32 Level,
			const FString& FieldName,
			const FString& JsonPointer,
			const bool bPreflight) const
		{
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
			const TSharedPtr<FJsonObject>& Object,
			const FString& JsonPointer,
			TArray<FString>& OutKeys) const
		{
			OutKeys.Reset();
			if (!Object.IsValid())
			{
				return;
			}
			Object->Values.GetKeys(OutKeys);
			TArray<FString> TupleKeys;
			if (TryGetTupleKeyOrder(OutKeys, TupleKeys))
			{
				OutKeys = MoveTemp(TupleKeys);
				return;
			}
			TArray<FString> Remaining = MoveTemp(OutKeys);
			Remaining.Sort();
			OutKeys.Reset();
			if (Options)
			{
				if (const TArray<FString>* Preferred = Options->PreferredKeyOrderByJsonPointer.Find(JsonPointer))
				{
					for (const FString& Key : *Preferred)
					{
						if (Object->HasField(Key) && !OutKeys.Contains(Key))
						{
							OutKeys.Add(Key);
							Remaining.Remove(Key);
						}
					}
				}
			}
			OutKeys.Append(Remaining);
		}

		void PushObjectBody(
			TArray<FWriteTask>& Stack,
			const TSharedPtr<FJsonObject>& Object,
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
					Object->Values.FindRef(Key),
					Level,
					Key,
					ValuePointer));
			}
			PushForwardTasks(Stack, Forward);
		}

		void PushObject(
			TArray<FWriteTask>& Stack,
			const TSharedPtr<FJsonObject>& Object,
			const int32 Level,
			const FString& FieldName,
			const FString& JsonPointer,
			const bool bPreflight) const
		{
			(void)FieldName;
			if (!Object.IsValid() || Object->Values.Num() == 0)
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
						Object->Values.FindRef(Key),
						0,
						Key,
						ValuePointer));
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
			const TSharedPtr<FJsonValue>& ArrayValue,
			const int32 Level,
			const FString& JsonPointer,
			const bool bPreflight) const
		{
			const TArray<TSharedPtr<FJsonValue>>& Values =
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
					ValuePointer));
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
			const TSharedPtr<FJsonObject>& Object,
			const FString& JsonPointer) const
		{
			if (!Object.IsValid() || Object->Values.Num() == 0)
			{
				return true;
			}
			if (const int32* Count =
					ObjectInlineCountCache.Find(Object.Get()))
			{
				return *Count <= InlineObjectFieldLimit;
			}
			SetWriteError(
				TEXT("hjson_internal_error"),
				JsonPointer,
				TEXT("missing inline object summary"));
			return false;
		}

		bool CanInlineArray(
			const TSharedPtr<FJsonValue>& ArrayValue,
			const FString& JsonPointer) const
		{
			if (const int32* Count =
					ArrayInlineCountCache.Find(ArrayValue.Get()))
			{
				return *Count <= InlineObjectFieldLimit;
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
			FUeAgentHjsonDiagnostic Diagnostic;
			Diagnostic.Phase = TEXT("write");
			Diagnostic.Code = Code;
			Diagnostic.JsonPointer = JsonPointer;
			Diagnostic.Message = Message;
			Error = Diagnostic.ToString();
		}

		const FUeAgentStructuredHjsonWriteOptions* Options = nullptr;
		TUniquePtr<FUeAgentStructuredResourceTracker> OwnedResourceTracker;
		FUeAgentStructuredResourceTracker& ResourceTracker;
		mutable TMap<const FJsonObject*, int32> ObjectInlineCountCache;
		mutable TMap<const FJsonValue*, int32> ArrayInlineCountCache;
		mutable TSet<const FJsonObject*> VisitingObjects;
		mutable TSet<const FJsonValue*> VisitingArrays;
		mutable FString Error;
	};
	class FHjsonParser
	{
	public:
		FHjsonParser(
			const FString& InText,
			const FString& InFilePath,
			TMap<FString, FUeAgentHjsonSourceLocation>& InSourceLocations,
			FUeAgentStructuredResourceTracker& InResourceTracker,
			const bool bInInputPrecharged)
			: Text(InText)
			, FilePath(InFilePath)
			, SourceLocations(InSourceLocations)
			, ResourceTracker(InResourceTracker)
			, bInputPrecharged(bInInputPrecharged)
		{
		}

		bool ParseRootObject(TSharedPtr<FJsonObject>& OutObject)
		{
			OutObject.Reset();
			const int64 Utf8Length =
				FTCHARToUTF8_Convert::ConvertedLength(*Text, Text.Len());
			if ((!bInputPrecharged
					&& !ResourceTracker.ChargeInputBytes(
						Utf8Length,
						TEXT("/"),
						Error))
				|| !ResourceTracker.ChargeWorkUnits(
					Text.Len(),
					TEXT("/"),
					Error)
				|| !ResourceTracker.ChargeNodes(1, TEXT("/"), Error)
				|| !ResourceTracker.ChargeEstimatedMemoryBytes(
					static_cast<int64>(Text.Len()) * sizeof(TCHAR) * 2
						+ sizeof(FJsonObject)
						+ sizeof(FParseFrame),
					TEXT("/"),
					Error))
			{
				return false;
			}
			OutObject = MakeShared<FJsonObject>();
			if (!SkipWhitespaceAndComments())
			{
				return false;
			}
			if (IsAtEnd())
			{
				return SetError(TEXT("empty authoring document"), TEXT("hjson_empty_document"));
			}
			if (!RecordLocation(TEXT("/")))
			{
				return false;
			}
			const bool bBraced = Match(TEXT('{'));
			TArray<FParseFrame> Frames;
			Frames.Add(FParseFrame::ObjectFrame(OutObject, TEXT("/"), bBraced));

			while (Frames.Num() > 0)
			{
				FParseFrame& Frame = Frames.Last();
				CurrentJsonPointer = Frame.JsonPointer;
				LastField = Frame.LastField;
				if (!ResourceTracker.ChargeWorkUnits(
						1,
						CurrentJsonPointer,
						Error))
				{
					return false;
				}
				if (Frame.State == EFrameState::WaitingForChild)
				{
					if (!FinishValue(Frame))
					{
						return false;
					}
					Frame.State = EFrameState::ExpectValue;
					continue;
				}
				if (!SkipWhitespaceAndComments())
				{
					return false;
				}

				if (Frame.Kind == EContainerKind::Object)
				{
					if (Frame.bBraced && Match(TEXT('}')))
					{
						Frames.Pop(EAllowShrinking::No);
						continue;
					}
					if (!Frame.bBraced && IsAtEnd())
					{
						Frames.Pop(EAllowShrinking::No);
						continue;
					}
					if (IsAtEnd())
					{
						return SetError(TEXT("unterminated object"), TEXT("hjson_unterminated_object"));
					}
					if (!RequireSeparator(Frame, TEXT("object fields")))
					{
						return false;
					}

					FString Key;
					if (!ParseKey(Key))
					{
						return false;
					}
					Frame.LastField = Key;
					LastField = Key;
					if (Frame.Object->HasField(Key))
					{
						return SetError(TEXT("duplicate key"), TEXT("hjson_duplicate_key"));
					}
					if (!SkipWhitespaceAndComments())
					{
						return false;
					}
					if (!Match(TEXT(':')))
					{
						return SetError(TEXT("expected colon after key"), TEXT("hjson_expected_colon"));
					}

					FString ValuePointer;
					if (!TryAppendTrackedJsonPointer(
							Frame.JsonPointer,
							Key,
							ValuePointer))
					{
						return false;
					}
					if (!ResourceTracker.ChargeItems(
							1,
							ValuePointer,
							Error)
						|| !ResourceTracker.ChargeEstimatedMemoryBytes(
							static_cast<int64>(Key.Len())
								* sizeof(TCHAR)
								+ sizeof(TPair<FString, TSharedPtr<FJsonValue>>),
							ValuePointer,
							Error))
					{
						return false;
					}
					TSharedPtr<FJsonValue> Value;
					FParseFrame Child;
					bool bHasChild = false;
					if (!ParseValueStart(Value, Child, bHasChild, ValuePointer))
					{
						return false;
					}
					Frame.Object->SetField(Key, Value);
					if (bHasChild)
					{
						Frame.State = EFrameState::WaitingForChild;
						Frames.Add(MoveTemp(Child));
					}
					else if (!FinishValue(Frame))
					{
						return false;
					}
				}
				else
				{
					if (Match(TEXT(']')))
					{
						Frames.Pop(EAllowShrinking::No);
						continue;
					}
					if (IsAtEnd())
					{
						return SetError(TEXT("unterminated array"), TEXT("hjson_unterminated_array"));
					}
					if (!RequireSeparator(Frame, TEXT("array items")))
					{
						return false;
					}

					const FString IndexSegment =
						FString::FromInt(Frame.Array->Num());
					FString ValuePointer;
					if (!TryAppendTrackedJsonPointer(
							Frame.JsonPointer,
							IndexSegment,
							ValuePointer))
					{
						return false;
					}
					if (!ResourceTracker.ChargeItems(
							1,
							ValuePointer,
							Error)
						|| !ResourceTracker.ChargeEstimatedMemoryBytes(
							sizeof(TSharedPtr<FJsonValue>),
							ValuePointer,
							Error))
					{
						return false;
					}
					TSharedPtr<FJsonValue> Value;
					FParseFrame Child;
					bool bHasChild = false;
					if (!ParseValueStart(Value, Child, bHasChild, ValuePointer))
					{
						return false;
					}
					Frame.Array->Add(Value);
					if (bHasChild)
					{
						Frame.State = EFrameState::WaitingForChild;
						Frames.Add(MoveTemp(Child));
					}
					else if (!FinishValue(Frame))
					{
						return false;
					}
				}
			}

			CurrentJsonPointer = TEXT("/");
			LastField.Reset();
			if (!SkipWhitespaceAndComments())
			{
				return false;
			}
			if (!IsAtEnd())
			{
				return SetError(TEXT("unexpected trailing token"), TEXT("hjson_trailing_token"));
			}
			return ResourceTracker.Checkpoint(TEXT("/"), Error);
		}

		const FString& GetError() const
		{
			return Error;
		}

	private:
		enum class EContainerKind : uint8
		{
			Object,
			Array,
		};

		enum class EFrameState : uint8
		{
			ExpectValue,
			WaitingForChild,
		};

		struct FParseFrame
		{
			EContainerKind Kind = EContainerKind::Object;
			EFrameState State = EFrameState::ExpectValue;
			TSharedPtr<FJsonObject> Object;
			TSharedPtr<FJsonValue> ContainerValue;
			TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
			FString JsonPointer = TEXT("/");
			FString LastField;
			bool bBraced = true;
			bool bHasValue = false;
			bool bHadComma = false;
			int32 PreviousValueEndLine = -1;

			static FParseFrame ObjectFrame(
				TSharedPtr<FJsonObject> InObject,
				FString InJsonPointer,
				const bool bInBraced = true)
			{
				FParseFrame Frame;
				Frame.Kind = EContainerKind::Object;
				Frame.Object = MoveTemp(InObject);
				Frame.JsonPointer = MoveTemp(InJsonPointer);
				Frame.bBraced = bInBraced;
				return Frame;
			}

			static FParseFrame ArrayFrame(
				TSharedPtr<FJsonValue> InContainerValue,
				TArray<TSharedPtr<FJsonValue>>* InArray,
				FString InJsonPointer,
				FString InContextField)
			{
				FParseFrame Frame;
				Frame.Kind = EContainerKind::Array;
				Frame.ContainerValue = MoveTemp(InContainerValue);
				Frame.Array = InArray;
				Frame.JsonPointer = MoveTemp(InJsonPointer);
				Frame.LastField = MoveTemp(InContextField);
				return Frame;
			}
		};

		bool TryAppendTrackedJsonPointer(
			const FString& Parent,
			const FString& Segment,
			FString& OutPointer)
		{
			const int64 PointerChars =
				AppendedJsonPointerLength(Parent, Segment);
			if (!ResourceTracker.ChargeWorkUnits(
					PointerChars,
					Parent,
					Error)
				|| !ResourceTracker.ChargeEstimatedMemoryBytes(
					PointerChars * sizeof(TCHAR),
					Parent,
					Error))
			{
				OutPointer.Reset();
				return false;
			}
			OutPointer = AppendJsonPointer(Parent, Segment);
			return true;
		}

		bool RequireSeparator(const FParseFrame& Frame, const TCHAR* Label)
		{
			if (Frame.bHasValue && !Frame.bHadComma && Line == Frame.PreviousValueEndLine)
			{
				return SetError(
					FString::Printf(TEXT("inline %s require comma"), Label),
					TEXT("hjson_inline_comma_required"));
			}
			return true;
		}

		bool FinishValue(FParseFrame& Frame)
		{
			Frame.bHasValue = true;
			Frame.PreviousValueEndLine = Line;
			if (!SkipWhitespaceAndComments())
			{
				return false;
			}
			Frame.bHadComma = Match(TEXT(','));
			return true;
		}

		bool ParseValueStart(
			TSharedPtr<FJsonValue>& OutValue,
			FParseFrame& OutChild,
			bool& bOutHasChild,
			const FString& CurrentPath)
		{
			bOutHasChild = false;
			if (!SkipWhitespaceAndComments())
			{
				return false;
			}
			if (IsAtEnd())
			{
				return SetError(TEXT("missing value"), TEXT("hjson_missing_value"));
			}
			CurrentJsonPointer = CurrentPath;
			const TCHAR Ch = Peek();
			int64 EstimatedValueBytes = sizeof(FJsonValue);
			if (Ch == TEXT('{'))
			{
				EstimatedValueBytes =
					sizeof(FJsonValueObject)
					+ sizeof(FJsonObject)
					+ sizeof(FParseFrame)
					+ static_cast<int64>(CurrentPath.Len()) * sizeof(TCHAR);
			}
			else if (Ch == TEXT('['))
			{
				EstimatedValueBytes =
					sizeof(FJsonValueArray)
					+ sizeof(TArray<TSharedPtr<FJsonValue>>)
					+ sizeof(FParseFrame)
					+ static_cast<int64>(CurrentPath.Len()) * sizeof(TCHAR);
			}
			if (!ResourceTracker.ChargeNodes(
					1,
					CurrentPath,
					Error)
				|| !ResourceTracker.ChargeEstimatedMemoryBytes(
					EstimatedValueBytes,
					CurrentPath,
					Error))
			{
				return false;
			}
			if (!RecordLocation(CurrentPath))
			{
				return false;
			}
			if (Ch == TEXT('{'))
			{
				Advance();
				TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
				OutValue = MakeShared<FJsonValueObject>(Object);
				OutChild = FParseFrame::ObjectFrame(Object, CurrentPath);
				bOutHasChild = true;
				return true;
			}
			if (Ch == TEXT('['))
			{
				Advance();
				TSharedPtr<FJsonValue> ArrayValue = MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>());
				TArray<TSharedPtr<FJsonValue>>* MutableArray = nullptr;
				if (!ArrayValue->TryGetArray(MutableArray) || !MutableArray)
				{
					return SetError(TEXT("failed to create array value"), TEXT("hjson_internal_error"));
				}
				OutValue = ArrayValue;
				OutChild = FParseFrame::ArrayFrame(ArrayValue, MutableArray, CurrentPath, LastField);
				bOutHasChild = true;
				return true;
			}
			if (Ch == TEXT('"'))
			{
				FString StringValue;
				if (!ParseQuotedString(StringValue))
				{
					return false;
				}
				OutValue = MakeShared<FJsonValueString>(MoveTemp(StringValue));
				return true;
			}
			if (Ch == TEXT('\''))
			{
				if (Index + 2 < Text.Len()
					&& Text[Index + 1] == TEXT('\'')
					&& Text[Index + 2] == TEXT('\''))
				{
					FString StringValue;
					if (!ParseMultilineString(StringValue))
					{
						return false;
					}
					OutValue = MakeShared<FJsonValueString>(MoveTemp(StringValue));
					return true;
				}
				return SetError(TEXT("unsupported single-quoted string"), TEXT("hjson_unsupported_feature"));
			}
			return ParseBareValue(OutValue);
		}

		bool ParseKey(FString& OutKey)
		{
			if (!SkipWhitespaceAndComments())
			{
				return false;
			}
			if (IsAtEnd())
			{
				return SetError(TEXT("missing key"), TEXT("hjson_missing_key"));
			}
			if (Peek() == TEXT('"'))
			{
				return ParseQuotedString(OutKey);
			}
			FString Token;
			while (!IsAtEnd())
			{
				const TCHAR Ch = Peek();
				if (Ch == TEXT(':'))
				{
					break;
				}
				if (IsWhitespace(Ch) || Ch == TEXT('{') || Ch == TEXT('}') || Ch == TEXT('[')
					|| Ch == TEXT(']') || Ch == TEXT(',') || Ch == TEXT('#'))
				{
					break;
				}
				Token.AppendChar(Advance());
			}
			if (!FUeAgentStructuredHjsonFormatter::IsBareKey(Token))
			{
				return SetError(TEXT("unsafe key must be quoted"), TEXT("hjson_unsafe_key"));
			}
			OutKey = MoveTemp(Token);
			return true;
		}

		bool ParseBareValue(TSharedPtr<FJsonValue>& OutValue)
		{
			const int32 TokenLine = Line;
			const int32 TokenColumn = Column;
			FString Token;
			while (!IsAtEnd())
			{
				const TCHAR Ch = Peek();
				if (IsWhitespace(Ch) || Ch == TEXT(',') || Ch == TEXT('}') || Ch == TEXT(']') || Ch == TEXT('#'))
				{
					break;
				}
				Token.AppendChar(Advance());
			}
			if (Token.IsEmpty())
			{
				return SetError(TEXT("missing value"), TEXT("hjson_missing_value"));
			}
			if (Token.Contains(TEXT(":")))
			{
				return SetError(TEXT("bare string contains colon"), TEXT("hjson_unsafe_bare_string"));
			}
			if (Token.Contains(TEXT("//")) || Token.Contains(TEXT("/*"))
				|| Token.Contains(TEXT("{")) || Token.Contains(TEXT("[")))
			{
				return SetError(TEXT("unsafe bare string must be quoted"), TEXT("hjson_unsafe_bare_string"));
			}

			const FString Lower = Token.ToLower();
			if (Lower == TEXT("true"))
			{
				if (Token != TEXT("true"))
				{
					return SetError(TEXT("ambiguous literal must be quoted"), TEXT("hjson_ambiguous_literal"));
				}
				OutValue = MakeShared<FJsonValueBoolean>(true);
				return true;
			}
			if (Lower == TEXT("false"))
			{
				if (Token != TEXT("false"))
				{
					return SetError(TEXT("ambiguous literal must be quoted"), TEXT("hjson_ambiguous_literal"));
				}
				OutValue = MakeShared<FJsonValueBoolean>(false);
				return true;
			}
			if (Lower == TEXT("null"))
			{
				if (Token != TEXT("null"))
				{
					return SetError(TEXT("ambiguous literal must be quoted"), TEXT("hjson_ambiguous_literal"));
				}
				OutValue = MakeShared<FJsonValueNull>();
				return true;
			}
			if (Lower == TEXT("nan") || Lower == TEXT("infinity") || Lower == TEXT("+infinity")
				|| Lower == TEXT("-infinity") || Lower == TEXT("inf") || Lower == TEXT("+inf")
				|| Lower == TEXT("-inf"))
			{
				return SetError(
					TEXT("non-finite number"),
					TEXT("hjson_non_finite_number"),
					TokenLine,
					TokenColumn);
			}
			if (IsNumberCandidate(Token))
			{
				FString Canonical;
				bool bNonFinite = false;
				if (!TryCanonicalizeNumberToken(Token, Canonical, bNonFinite))
				{
					return SetError(
						bNonFinite ? TEXT("non-finite number") : TEXT("invalid number or unsafe bare string must be quoted"),
						bNonFinite ? TEXT("hjson_non_finite_number") : TEXT("hjson_invalid_number"),
						TokenLine,
						TokenColumn);
				}
				OutValue = MakeShared<FJsonValueNumberString>(MoveTemp(Canonical));
				return true;
			}
			if (!FUeAgentStructuredHjsonFormatter::IsBareString(Token))
			{
				return SetError(TEXT("unsafe bare string must be quoted"), TEXT("hjson_unsafe_bare_string"));
			}
			OutValue = MakeShared<FJsonValueString>(MoveTemp(Token));
			return true;
		}

		bool ParseQuotedString(FString& OutValue)
		{
			if (!Match(TEXT('"')))
			{
				return SetError(TEXT("expected quoted string"), TEXT("hjson_expected_string"));
			}
			FString Result;
			while (!IsAtEnd())
			{
				const TCHAR Ch = Peek();
				if (Ch == TEXT('"'))
				{
					Advance();
					OutValue = MoveTemp(Result);
					return true;
				}
				if (Ch == TEXT('\n') || Ch == TEXT('\r'))
				{
					return SetError(TEXT("unterminated string"), TEXT("hjson_unterminated_string"));
				}
				Advance();
				if (Ch != TEXT('\\'))
				{
					if (Ch < 0x20)
					{
						return SetError(TEXT("unescaped control character in string"), TEXT("hjson_unescaped_control"));
					}
					Result.AppendChar(Ch);
					continue;
				}
				if (IsAtEnd())
				{
					return SetError(TEXT("unterminated string"), TEXT("hjson_unterminated_string"));
				}
				const TCHAR Escaped = Advance();
				switch (Escaped)
				{
				case TEXT('"'): Result.AppendChar(TEXT('"')); break;
				case TEXT('\\'): Result.AppendChar(TEXT('\\')); break;
				case TEXT('/'): Result.AppendChar(TEXT('/')); break;
				case TEXT('b'): Result.AppendChar(TEXT('\b')); break;
				case TEXT('f'): Result.AppendChar(TEXT('\f')); break;
				case TEXT('n'): Result.AppendChar(TEXT('\n')); break;
				case TEXT('r'): Result.AppendChar(TEXT('\r')); break;
				case TEXT('t'): Result.AppendChar(TEXT('\t')); break;
				case TEXT('u'):
				{
					int32 Codepoint = 0;
					for (int32 HexIndex = 0; HexIndex < 4; ++HexIndex)
					{
						if (IsAtEnd() || !ReadHexDigit(Codepoint))
						{
							return SetError(TEXT("invalid unicode escape"), TEXT("hjson_invalid_unicode_escape"));
						}
					}
					Result.AppendChar(static_cast<TCHAR>(Codepoint));
					break;
				}
				default:
					return SetError(TEXT("invalid string escape"), TEXT("hjson_invalid_string_escape"));
				}
			}
			return SetError(TEXT("unterminated string"), TEXT("hjson_unterminated_string"));
		}

		bool ParseMultilineString(FString& OutValue)
		{
			const int32 OpeningLine = Line;
			const int32 OpeningColumn = Column;
			Advance();
			Advance();
			Advance();
			while (!IsAtEnd() && (Peek() == TEXT(' ') || Peek() == TEXT('\t')))
			{
				Advance();
			}
			if (IsAtEnd())
			{
				return SetError(
					TEXT("unterminated multiline string"),
					TEXT("hjson_unterminated_multiline"),
					OpeningLine,
					OpeningColumn);
			}
			if (Peek() != TEXT('\r') && Peek() != TEXT('\n'))
			{
				return SetError(
					TEXT("multiline opening delimiter must end the line"),
					TEXT("hjson_multiline_trailing_token"));
			}
			ConsumeLineBreak();

			struct FMultilineLine
			{
				FString Text;
				int32 Line = 0;
				int32 Indent = 0;
			};
			TArray<FMultilineLine> Lines;
			while (!IsAtEnd())
			{
				const int32 ContentLine = Line;
				const int32 LineStart = Index;
				int32 LineEnd = LineStart;
				while (LineEnd < Text.Len() && Text[LineEnd] != TEXT('\r') && Text[LineEnd] != TEXT('\n'))
				{
					++LineEnd;
				}
				const FString LineText = Text.Mid(LineStart, LineEnd - LineStart);
				int32 Indent = 0;
				while (Indent < LineText.Len()
					&& (LineText[Indent] == TEXT(' ') || LineText[Indent] == TEXT('\t')))
				{
					++Indent;
				}
				const FString Stripped = LineText.Mid(Indent);
				if (Stripped.StartsWith(TEXT("'''"), ESearchCase::CaseSensitive))
				{
					const FString Trailing = Stripped.Mid(3);
					int32 ExtraSpaces = 0;
					while (ExtraSpaces < Trailing.Len()
						&& (Trailing[ExtraSpaces] == TEXT(' ') || Trailing[ExtraSpaces] == TEXT('\t')))
					{
						++ExtraSpaces;
					}
					if (ExtraSpaces < Trailing.Len())
					{
						return SetError(
							TEXT("trailing token after multiline delimiter"),
							TEXT("hjson_multiline_trailing_token"),
							ContentLine,
							Indent + 3 + ExtraSpaces + 1);
					}
					for (const FMultilineLine& Content : Lines)
					{
						FString Trimmed = Content.Text;
						Trimmed.TrimStartAndEndInline();
						if (!Trimmed.IsEmpty() && Content.Indent < Indent)
						{
							return SetError(
								TEXT("multiline content indentation is less than closing delimiter"),
								TEXT("hjson_multiline_indentation"),
								Content.Line,
								Content.Indent + 1);
						}
					}
					while (Index < LineEnd)
					{
						Advance();
					}
					OutValue.Reset();
					for (int32 LineIndex = 0; LineIndex < Lines.Num(); ++LineIndex)
					{
						if (LineIndex > 0)
						{
							OutValue += TEXT("\n");
						}
						if (!Lines[LineIndex].Text.IsEmpty())
						{
							OutValue += Lines[LineIndex].Text.Mid(Indent);
						}
					}
					return true;
				}

				FMultilineLine& Content = Lines.AddDefaulted_GetRef();
				Content.Text = LineText;
				Content.Line = ContentLine;
				Content.Indent = Indent;
				while (Index < LineEnd)
				{
					Advance();
				}
				if (!IsAtEnd())
				{
					ConsumeLineBreak();
				}
			}
			return SetError(
				TEXT("unterminated multiline string"),
				TEXT("hjson_unterminated_multiline"),
				OpeningLine,
				OpeningColumn);
		}

		bool SkipWhitespaceAndComments()
		{
			while (!IsAtEnd())
			{
				const TCHAR Ch = Peek();
				if (IsWhitespace(Ch))
				{
					Advance();
					continue;
				}
				if (Ch == TEXT('#'))
				{
					SkipLine();
					continue;
				}
				if (Ch == TEXT('/') && PeekNext() == TEXT('/'))
				{
					SkipLine();
					continue;
				}
				if (Ch == TEXT('/') && PeekNext() == TEXT('*'))
				{
					return SetError(TEXT("unsupported block comment"), TEXT("hjson_unsupported_feature"));
				}
				break;
			}
			return true;
		}

		void SkipLine()
		{
			while (!IsAtEnd() && Peek() != TEXT('\n') && Peek() != TEXT('\r'))
			{
				Advance();
			}
		}

		void ConsumeLineBreak()
		{
			if (Peek() == TEXT('\r'))
			{
				Advance();
				if (!IsAtEnd() && Peek() == TEXT('\n'))
				{
					Advance();
				}
			}
			else if (Peek() == TEXT('\n'))
			{
				Advance();
			}
		}

		bool ReadHexDigit(int32& InOutCodepoint)
		{
			const TCHAR Ch = Advance();
			int32 Value = 0;
			if (Ch >= TEXT('0') && Ch <= TEXT('9'))
			{
				Value = Ch - TEXT('0');
			}
			else if (Ch >= TEXT('a') && Ch <= TEXT('f'))
			{
				Value = 10 + Ch - TEXT('a');
			}
			else if (Ch >= TEXT('A') && Ch <= TEXT('F'))
			{
				Value = 10 + Ch - TEXT('A');
			}
			else
			{
				return false;
			}
			InOutCodepoint = (InOutCodepoint << 4) | Value;
			return true;
		}

		bool IsNumberCandidate(const FString& Token) const
		{
			if (Token.IsEmpty())
			{
				return false;
			}
			const TCHAR Ch = Token[0];
			return IsAsciiDigit(Ch) || Ch == TEXT('-') || Ch == TEXT('.') || Ch == TEXT('+');
		}

		bool IsWhitespace(const TCHAR Ch) const
		{
			return Ch == TEXT(' ') || Ch == TEXT('\t') || Ch == TEXT('\r') || Ch == TEXT('\n');
		}

		bool Match(const TCHAR Expected)
		{
			if (IsAtEnd() || Peek() != Expected)
			{
				return false;
			}
			Advance();
			return true;
		}

		TCHAR Peek() const
		{
			return IsAtEnd() ? TEXT('\0') : Text[Index];
		}

		TCHAR PeekNext() const
		{
			return Index + 1 < Text.Len() ? Text[Index + 1] : TEXT('\0');
		}

		TCHAR Advance()
		{
			const TCHAR Ch = Text[Index++];
			if (Ch == TEXT('\r'))
			{
				++Line;
				Column = 1;
				bLastWasCr = true;
			}
			else if (Ch == TEXT('\n'))
			{
				if (!bLastWasCr)
				{
					++Line;
				}
				Column = 1;
				bLastWasCr = false;
			}
			else
			{
				++Column;
				bLastWasCr = false;
			}
			if ((Index & 1023) == 0
				&& !ResourceTracker.Checkpoint(
					CurrentJsonPointer,
					Error))
			{
				Index = Text.Len();
			}
			return Ch;
		}

		bool IsAtEnd() const
		{
			return Index >= Text.Len();
		}

		bool RecordLocation(const FString& JsonPointer)
		{
			const int64 EstimatedBytes =
				sizeof(FUeAgentHjsonSourceLocation)
				+ sizeof(TPair<FString, FUeAgentHjsonSourceLocation>)
				+ (static_cast<int64>(JsonPointer.Len())
					+ FilePath.Len())
					* sizeof(TCHAR);
			if (!ResourceTracker.ChargeEstimatedMemoryBytes(
					EstimatedBytes,
					JsonPointer,
					Error))
			{
				return false;
			}
			FUeAgentHjsonSourceLocation Location;
			Location.FilePath = FilePath;
			Location.Line = Line;
			Location.Column = Column;
			SourceLocations.Add(JsonPointer, MoveTemp(Location));
			return true;
		}

		bool SetError(
			const FString& Message,
			const FString& Code = TEXT("hjson_syntax_error"),
			const int32 OverrideLine = INDEX_NONE,
			const int32 OverrideColumn = INDEX_NONE)
		{
			if (!Error.IsEmpty())
			{
				return false;
			}
			FUeAgentHjsonDiagnostic Diagnostic;
			Diagnostic.Phase = TEXT("parse");
			Diagnostic.Code = Code;
			Diagnostic.SourceFile = FilePath;
			Diagnostic.JsonPointer = CurrentJsonPointer;
			Diagnostic.Line = OverrideLine == INDEX_NONE ? Line : OverrideLine;
			Diagnostic.Column = OverrideColumn == INDEX_NONE ? Column : OverrideColumn;
			Diagnostic.Field = LastField.IsEmpty() ? TEXT("<root>") : LastField;
			Diagnostic.Message = Message;
			Error = Diagnostic.ToString();
			return false;
		}

		const FString& Text;
		FString FilePath;
		TMap<FString, FUeAgentHjsonSourceLocation>& SourceLocations;
		int32 Index = 0;
		int32 Line = 1;
		int32 Column = 1;
		FString LastField;
		FString CurrentJsonPointer = TEXT("/");
		bool bLastWasCr = false;
		FUeAgentStructuredResourceTracker& ResourceTracker;
		bool bInputPrecharged = false;
		FString Error;
	};
}

FString FUeAgentHjsonDiagnostic::ToString() const
{
	const FString LegacyPrefix = Phase == TEXT("parse")
		? FString::Printf(
			TEXT("hjson_parse_failed:%s:%d:%d:%s:%s "),
			*SourceFile,
			Line,
			Column,
			*Field,
			*Message)
		: FString();
	return FString::Printf(
		TEXT("%sdialect=%s phase=%s code=%s source=%s pointer=%s line=%d column=%d message=%s"),
		*LegacyPrefix,
		*DialectId,
		*Phase,
		*Code,
		*SourceFile,
		*JsonPointer,
		Line,
		Column,
		*Message);
}

const TCHAR* FUeAgentStructuredHjsonFormatter::DialectId()
{
	return TEXT("uai-hjson-authoring/1");
}

FString FUeAgentStructuredHjsonFormatter::WriteObject(const TSharedRef<FJsonObject>& Object)
{
	FString Text;
	FString Error;
	TryWriteObject(Object, Text, Error);
	return Text;
}

FString FUeAgentStructuredHjsonFormatter::WriteObject(
	const TSharedRef<FJsonObject>& Object,
	const FUeAgentStructuredHjsonWriteOptions& Options)
{
	FString Text;
	FString Error;
	TryWriteObject(Object, Options, Text, Error);
	return Text;
}

FString FUeAgentStructuredHjsonFormatter::WriteValue(const TSharedRef<FJsonValue>& Value)
{
	FString Text;
	FString Error;
	TryWriteValue(Value, Text, Error);
	return Text;
}

FString FUeAgentStructuredHjsonFormatter::WriteValue(
	const TSharedRef<FJsonValue>& Value,
	const FUeAgentStructuredHjsonWriteOptions& Options)
{
	FString Text;
	FString Error;
	TryWriteValue(Value, Options, Text, Error);
	return Text;
}

bool FUeAgentStructuredHjsonFormatter::TryWriteObject(
	const TSharedRef<FJsonObject>& Object,
	FString& OutText,
	FString& OutError)
{
	return FHjsonWriter().TryWriteRootObject(Object, OutText, OutError);
}

bool FUeAgentStructuredHjsonFormatter::TryWriteObject(
	const TSharedRef<FJsonObject>& Object,
	const FUeAgentStructuredHjsonWriteOptions& Options,
	FString& OutText,
	FString& OutError)
{
	return FHjsonWriter(&Options).TryWriteRootObject(Object, OutText, OutError);
}

bool FUeAgentStructuredHjsonFormatter::TryWriteObject(
	const TSharedRef<FJsonObject>& Object,
	const FUeAgentStructuredHjsonWriteOptions& Options,
	FUeAgentStructuredResourceTracker& ResourceTracker,
	FString& OutText,
	FString& OutError)
{
	return FHjsonWriter(&Options, &ResourceTracker).TryWriteRootObject(
		Object,
		OutText,
		OutError);
}

bool FUeAgentStructuredHjsonFormatter::TryWriteValue(
	const TSharedRef<FJsonValue>& Value,
	FString& OutText,
	FString& OutError)
{
	return FHjsonWriter().TryWriteValue(Value, OutText, OutError);
}

bool FUeAgentStructuredHjsonFormatter::TryWriteValue(
	const TSharedRef<FJsonValue>& Value,
	const FUeAgentStructuredHjsonWriteOptions& Options,
	FString& OutText,
	FString& OutError)
{
	return FHjsonWriter(&Options).TryWriteValue(Value, OutText, OutError);
}

bool FUeAgentStructuredHjsonFormatter::TryWriteValue(
	const TSharedRef<FJsonValue>& Value,
	const FUeAgentStructuredHjsonWriteOptions& Options,
	FUeAgentStructuredResourceTracker& ResourceTracker,
	FString& OutText,
	FString& OutError)
{
	return FHjsonWriter(&Options, &ResourceTracker).TryWriteValue(
		Value,
		OutText,
		OutError);
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
	FString& OutError)
{
	return ParseDocument(
		Text,
		FilePath,
		FUeAgentStructuredResourceBudget(),
		OutDocument,
		OutError);
}

bool FUeAgentStructuredHjsonParser::ParseDocument(
	const FString& Text,
	const FString& FilePath,
	const FUeAgentStructuredResourceBudget& ResourceBudget,
	FUeAgentStructuredHjsonDocument& OutDocument,
	FString& OutError)
{
	FUeAgentStructuredResourceTracker ResourceTracker(ResourceBudget);
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
	FString& OutError)
{
	OutDocument = FUeAgentStructuredHjsonDocument();
	OutDocument.SourceFile = FilePath;
	OutError.Reset();

	FHjsonParser Parser(
		Text,
		FilePath,
		OutDocument.SourceLocations,
		ResourceTracker,
		bInputPrecharged);
	if (!Parser.ParseRootObject(OutDocument.Root) || !OutDocument.Root.IsValid())
	{
		OutError = Parser.GetError();
		OutDocument.Root.Reset();
		return false;
	}
	OutDocument.SourceText = Text;
	return true;
}

bool FUeAgentStructuredHjsonParser::ParseObject(
	const FString& Text,
	const FString& FilePath,
	TSharedPtr<FJsonObject>& OutObject,
	FString& OutError)
{
	return ParseObject(
		Text,
		FilePath,
		FUeAgentStructuredResourceBudget(),
		OutObject,
		OutError);
}

bool FUeAgentStructuredHjsonParser::ParseObject(
	const FString& Text,
	const FString& FilePath,
	const FUeAgentStructuredResourceBudget& ResourceBudget,
	TSharedPtr<FJsonObject>& OutObject,
	FString& OutError)
{
	FUeAgentStructuredHjsonDocument Document;
	if (!ParseDocument(
			Text,
			FilePath,
			ResourceBudget,
			Document,
			OutError))
	{
		OutObject.Reset();
		return false;
	}
	OutObject = Document.Root;
	return true;
}

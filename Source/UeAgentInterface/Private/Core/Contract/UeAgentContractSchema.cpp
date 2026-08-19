#include "Core/Contract/UeAgentContractSchema.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UE::UAI::Contract
{
	namespace
	{
		const TSet<FString>& SupportedKeywords()
		{
			static const TSet<FString> Keywords = {
				TEXT("type"),
				TEXT("enum"),
				TEXT("const"),
				TEXT("allOf"),
				TEXT("anyOf"),
				TEXT("oneOf"),
				TEXT("not"),
				TEXT("properties"),
				TEXT("required"),
				TEXT("additionalProperties"),
				TEXT("minProperties"),
				TEXT("maxProperties"),
				TEXT("items"),
				TEXT("minItems"),
				TEXT("maxItems"),
				TEXT("uniqueItems"),
				TEXT("minLength"),
				TEXT("maxLength"),
				TEXT("minimum"),
				TEXT("maximum"),
				TEXT("exclusiveMinimum"),
				TEXT("exclusiveMaximum"),
			};
			return Keywords;
		}

		const TSet<FString>& SupportedTypes()
		{
			static const TSet<FString> Types = {
				TEXT("null"),
				TEXT("boolean"),
				TEXT("object"),
				TEXT("array"),
				TEXT("number"),
				TEXT("integer"),
				TEXT("string"),
			};
			return Types;
		}

		FString EscapePointerToken(FString Token)
		{
			Token.ReplaceInline(TEXT("~"), TEXT("~0"));
			Token.ReplaceInline(TEXT("/"), TEXT("~1"));
			return Token;
		}

		FString AppendPath(
			const FString& Parent,
			const FString& Token)
		{
			return Parent + TEXT("/") + EscapePointerToken(Token);
		}

		bool Fail(
			FUeAgentContractSchemaError& OutError,
			const FString& Code,
			const FString& InstancePath,
			const FString& SchemaPath,
			const FString& Keyword,
			const FString& Detail)
		{
			OutError = {};
			OutError.Code = Code;
			OutError.InstancePath = InstancePath;
			OutError.SchemaPath = SchemaPath;
			OutError.Keyword = Keyword;
			OutError.Detail = Detail;
			return false;
		}

		bool CompileFail(
			FUeAgentContractSchemaError& OutError,
			const FString& SchemaPath,
			const FString& Keyword,
			const FString& Detail,
			const FString& Code =
				TEXT("contract_schema_invalid"))
		{
			return Fail(
				OutError,
				Code,
				FString(),
				SchemaPath,
				Keyword,
				Detail);
		}

		bool ValidationFail(
			FUeAgentContractSchemaError& OutError,
			const FString& InstancePath,
			const FString& SchemaPath,
			const FString& Keyword,
			const FString& Detail)
		{
			return Fail(
				OutError,
				TEXT("contract_input_schema_mismatch"),
				InstancePath,
				SchemaPath,
				Keyword,
				Detail);
		}

		bool TryParseJsonValue(
			const FString& Text,
			TSharedPtr<FJsonValue>& OutValue)
		{
			OutValue.Reset();
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(Text);
			return FJsonSerializer::Deserialize(
					Reader,
					OutValue,
					FJsonSerializer::EFlags::StoreNumbersAsStrings)
				&& OutValue.IsValid()
				&& OutValue->Type != EJson::None;
		}

		bool TryCanonicalizeValue(
			const TSharedPtr<FJsonValue>& Value,
			FString& OutText)
		{
			FString Error;
			return Value.IsValid()
				&& UE::UAI::Contract::Json::TryCanonicalize(
					Value,
					OutText,
					Error);
		}

		bool IsNonNegativeInteger(
			const TSharedPtr<FJsonValue>& Value,
			int64& OutValue)
		{
			OutValue = 0;
			if (!Value.IsValid() || Value->Type != EJson::Number)
			{
				return false;
			}
			const FString Number = Value->AsString();
			if (Number.IsEmpty() || Number.Contains(TEXT("."))
				|| Number.Contains(TEXT("e"))
				|| Number.Contains(TEXT("E")))
			{
				return false;
			}
			return LexTryParseString(OutValue, *Number)
				&& OutValue >= 0;
		}

		bool IsNumber(const TSharedPtr<FJsonValue>& Value)
		{
			return Value.IsValid() && Value->Type == EJson::Number;
		}

		struct FDecimal
		{
			bool bNegative = false;
			bool bZero = true;
			FString Digits;
			int64 DecimalOrder = 0;
			int64 Scale = 0;
		};

		bool TryParseDecimal(const FString& Text, FDecimal& Out)
		{
			Out = {};
			int32 Cursor = 0;
			if (Text.IsEmpty())
			{
				return false;
			}
			if (Text[Cursor] == TEXT('-'))
			{
				Out.bNegative = true;
				++Cursor;
			}
			const int32 IntegerStart = Cursor;
			while (Cursor < Text.Len()
				&& FChar::IsDigit(Text[Cursor]))
			{
				++Cursor;
			}
			if (Cursor == IntegerStart)
			{
				return false;
			}
			FString Digits = Text.Mid(
				IntegerStart,
				Cursor - IntegerStart);
			int64 FractionDigits = 0;
			if (Cursor < Text.Len() && Text[Cursor] == TEXT('.'))
			{
				++Cursor;
				const int32 FractionStart = Cursor;
				while (Cursor < Text.Len()
					&& FChar::IsDigit(Text[Cursor]))
				{
					++Cursor;
				}
				if (Cursor == FractionStart)
				{
					return false;
				}
				FractionDigits = Cursor - FractionStart;
				Digits += Text.Mid(
					FractionStart,
					static_cast<int32>(FractionDigits));
			}
			int64 ExplicitExponent = 0;
			if (Cursor < Text.Len()
				&& (Text[Cursor] == TEXT('e')
					|| Text[Cursor] == TEXT('E')))
			{
				++Cursor;
				bool bExponentNegative = false;
				if (Cursor < Text.Len()
					&& (Text[Cursor] == TEXT('+')
						|| Text[Cursor] == TEXT('-')))
				{
					bExponentNegative = Text[Cursor] == TEXT('-');
					++Cursor;
				}
				const int32 ExponentStart = Cursor;
				while (Cursor < Text.Len()
					&& FChar::IsDigit(Text[Cursor]))
				{
					const int32 Digit = Text[Cursor] - TEXT('0');
					if (ExplicitExponent
						> (MAX_int64 - Digit) / 10)
					{
						return false;
					}
					ExplicitExponent =
						ExplicitExponent * 10 + Digit;
					++Cursor;
				}
				if (Cursor == ExponentStart)
				{
					return false;
				}
				if (bExponentNegative)
				{
					ExplicitExponent = -ExplicitExponent;
				}
			}
			if (Cursor != Text.Len())
			{
				return false;
			}

			int32 FirstNonZero = 0;
			while (FirstNonZero < Digits.Len()
				&& Digits[FirstNonZero] == TEXT('0'))
			{
				++FirstNonZero;
			}
			if (FirstNonZero == Digits.Len())
			{
				Out.bNegative = false;
				Out.bZero = true;
				Out.Digits = TEXT("0");
				return true;
			}
			Digits.RightChopInline(FirstNonZero, EAllowShrinking::No);
			int32 TrailingZeros = 0;
			while (Digits.Len() - TrailingZeros > 1
				&& Digits[Digits.Len() - TrailingZeros - 1]
					== TEXT('0'))
			{
				++TrailingZeros;
			}
			if (TrailingZeros > 0)
			{
				Digits.LeftChopInline(
					TrailingZeros,
					EAllowShrinking::No);
			}
			if ((ExplicitExponent > 0
					&& ExplicitExponent
						> MAX_int64 - TrailingZeros)
				|| (ExplicitExponent < 0
					&& ExplicitExponent
						< MIN_int64 + FractionDigits))
			{
				return false;
			}
			Out.bZero = false;
			Out.Digits = MoveTemp(Digits);
			Out.Scale =
				ExplicitExponent - FractionDigits + TrailingZeros;
			if (Out.Scale > MAX_int64 - Out.Digits.Len())
			{
				return false;
			}
			Out.DecimalOrder =
				Out.Scale + Out.Digits.Len() - 1;
			return true;
		}

		int32 CompareMagnitude(
			const FDecimal& Left,
			const FDecimal& Right)
		{
			if (Left.DecimalOrder != Right.DecimalOrder)
			{
				return Left.DecimalOrder < Right.DecimalOrder ? -1 : 1;
			}
			const int32 Count =
				FMath::Max(Left.Digits.Len(), Right.Digits.Len());
			for (int32 Index = 0; Index < Count; ++Index)
			{
				const TCHAR A = Index < Left.Digits.Len()
					? Left.Digits[Index]
					: TEXT('0');
				const TCHAR B = Index < Right.Digits.Len()
					? Right.Digits[Index]
					: TEXT('0');
				if (A != B)
				{
					return A < B ? -1 : 1;
				}
			}
			return 0;
		}

		bool TryCompareNumbers(
			const TSharedPtr<FJsonValue>& LeftValue,
			const TSharedPtr<FJsonValue>& RightValue,
			int32& OutComparison)
		{
			FDecimal Left;
			FDecimal Right;
			if (!IsNumber(LeftValue) || !IsNumber(RightValue)
				|| !TryParseDecimal(LeftValue->AsString(), Left)
				|| !TryParseDecimal(RightValue->AsString(), Right))
			{
				return false;
			}
			if (Left.bZero && Right.bZero)
			{
				OutComparison = 0;
				return true;
			}
			if (Left.bZero)
			{
				OutComparison = Right.bNegative ? 1 : -1;
				return true;
			}
			if (Right.bZero)
			{
				OutComparison = Left.bNegative ? -1 : 1;
				return true;
			}
			if (Left.bNegative != Right.bNegative)
			{
				OutComparison = Left.bNegative ? -1 : 1;
				return true;
			}
			OutComparison = CompareMagnitude(Left, Right);
			if (Left.bNegative)
			{
				OutComparison = -OutComparison;
			}
			return true;
		}

		bool IsIntegerNumber(const TSharedPtr<FJsonValue>& Value)
		{
			FDecimal Decimal;
			return IsNumber(Value)
				&& TryParseDecimal(Value->AsString(), Decimal)
				&& (Decimal.bZero || Decimal.Scale >= 0);
		}

		int64 UnicodeScalarCount(const FString& Value)
		{
			int64 Count = 0;
			for (int32 Index = 0; Index < Value.Len(); ++Index)
			{
				const TCHAR Unit = Value[Index];
				if (Unit >= 0xD800 && Unit <= 0xDBFF
					&& Index + 1 < Value.Len()
					&& Value[Index + 1] >= 0xDC00
					&& Value[Index + 1] <= 0xDFFF)
				{
					++Index;
				}
				++Count;
			}
			return Count;
		}

		bool ValidateTypeDeclaration(
			const TSharedPtr<FJsonValue>& Value,
			const FString& SchemaPath,
			FUeAgentContractSchemaError& OutError)
		{
			TArray<FString> Types;
			if (Value->Type == EJson::String)
			{
				Types.Add(Value->AsString());
			}
			else if (Value->Type == EJson::Array)
			{
				if (Value->AsArray().IsEmpty())
				{
					return CompileFail(
						OutError,
						SchemaPath,
						TEXT("type"),
						TEXT("type array must not be empty."));
				}
				for (const TSharedPtr<FJsonValue>& Item :
					Value->AsArray())
				{
					if (!Item.IsValid()
						|| Item->Type != EJson::String)
					{
						return CompileFail(
							OutError,
							SchemaPath,
							TEXT("type"),
							TEXT(
								"type array entries must be strings."));
					}
					Types.Add(Item->AsString());
				}
			}
			else
			{
				return CompileFail(
					OutError,
					SchemaPath,
					TEXT("type"),
					TEXT("type must be a string or string array."));
			}
			TSet<FString> Seen;
			for (const FString& Type : Types)
			{
				if (!SupportedTypes().Contains(Type)
					|| Seen.Contains(Type))
				{
					return CompileFail(
						OutError,
						SchemaPath,
						TEXT("type"),
						TEXT(
							"type contains an unsupported or duplicate name."));
				}
				Seen.Add(Type);
			}
			return true;
		}

		struct FSchemaCompileTask
		{
			TSharedPtr<FJsonObject> Schema;
			FString Path;
		};

		bool ValidateSchemaObject(
			const TSharedPtr<FJsonObject>& Root,
			FUeAgentContractSchemaError& OutError)
		{
			TArray<FSchemaCompileTask> Tasks;
			Tasks.Add({Root, FString()});
			while (!Tasks.IsEmpty())
			{
				FSchemaCompileTask Task = Tasks.Pop(
					EAllowShrinking::No);
				if (!Task.Schema.IsValid())
				{
					return CompileFail(
						OutError,
						Task.Path,
						FString(),
						TEXT("Subschema must be an object."));
				}

				TArray<FString> Keys;
				Task.Schema->Values.GetKeys(Keys);
				Keys.Sort();
				for (const FString& Key : Keys)
				{
					const TSharedPtr<FJsonValue>& Value =
						Task.Schema->Values.FindChecked(Key);
					const FString KeywordPath =
						AppendPath(Task.Path, Key);
					if (!SupportedKeywords().Contains(Key))
					{
						return CompileFail(
							OutError,
							KeywordPath,
							Key,
							TEXT(
								"Keyword is not part of ContractSchema v1."),
							TEXT(
								"contract_schema_keyword_unsupported"));
					}
					if (Key == TEXT("type"))
					{
						if (!ValidateTypeDeclaration(
								Value,
								KeywordPath,
								OutError))
						{
							return false;
						}
					}
					else if (Key == TEXT("enum"))
					{
						if (!Value.IsValid()
							|| Value->Type != EJson::Array
							|| Value->AsArray().IsEmpty())
						{
							return CompileFail(
								OutError,
								KeywordPath,
								Key,
								TEXT("enum must be a non-empty array."));
						}
						TSet<FString> Seen;
						for (const TSharedPtr<FJsonValue>& Item :
							Value->AsArray())
						{
							FString Canonical;
							if (!TryCanonicalizeValue(
									Item,
									Canonical)
								|| Seen.Contains(Canonical))
							{
								return CompileFail(
									OutError,
									KeywordPath,
									Key,
									TEXT(
										"enum values must be valid and unique."));
							}
							Seen.Add(MoveTemp(Canonical));
						}
					}
					else if (Key == TEXT("allOf")
						|| Key == TEXT("anyOf")
						|| Key == TEXT("oneOf"))
					{
						if (!Value.IsValid()
							|| Value->Type != EJson::Array
							|| Value->AsArray().IsEmpty())
						{
							return CompileFail(
								OutError,
								KeywordPath,
								Key,
								TEXT(
									"Composition keyword must be a non-empty schema array."));
						}
						for (int32 Index = 0;
							Index < Value->AsArray().Num();
							++Index)
						{
							const TSharedPtr<FJsonValue>& Item =
								Value->AsArray()[Index];
							if (!Item.IsValid()
								|| Item->Type != EJson::Object)
							{
								return CompileFail(
									OutError,
									AppendPath(
										KeywordPath,
										LexToString(Index)),
									Key,
									TEXT(
										"Composition entries must be schema objects."));
							}
							Tasks.Add({
								Item->AsObject(),
								AppendPath(
									KeywordPath,
									LexToString(Index)),
							});
						}
					}
					else if (Key == TEXT("not")
						|| Key == TEXT("items"))
					{
						if (!Value.IsValid()
							|| Value->Type != EJson::Object)
						{
							return CompileFail(
								OutError,
								KeywordPath,
								Key,
								TEXT("Keyword must contain a schema object."));
						}
						Tasks.Add({Value->AsObject(), KeywordPath});
					}
					else if (Key == TEXT("properties"))
					{
						if (!Value.IsValid()
							|| Value->Type != EJson::Object)
						{
							return CompileFail(
								OutError,
								KeywordPath,
								Key,
								TEXT("properties must be an object."));
						}
						for (const TPair<
							FString,
							TSharedPtr<FJsonValue>>& Property :
							Value->AsObject()->Values)
						{
							if (!Property.Value.IsValid()
								|| Property.Value->Type
									!= EJson::Object)
							{
								return CompileFail(
									OutError,
									AppendPath(
										KeywordPath,
										Property.Key),
									Key,
									TEXT(
										"Property schemas must be objects."));
							}
							Tasks.Add({
								Property.Value->AsObject(),
								AppendPath(
									KeywordPath,
									Property.Key),
							});
						}
					}
					else if (Key == TEXT("required"))
					{
						if (!Value.IsValid()
							|| Value->Type != EJson::Array)
						{
							return CompileFail(
								OutError,
								KeywordPath,
								Key,
								TEXT("required must be a string array."));
						}
						TSet<FString> Seen;
						for (const TSharedPtr<FJsonValue>& Item :
							Value->AsArray())
						{
							if (!Item.IsValid()
								|| Item->Type != EJson::String
								|| Item->AsString().IsEmpty()
								|| Seen.Contains(Item->AsString()))
							{
								return CompileFail(
									OutError,
									KeywordPath,
									Key,
									TEXT(
										"required entries must be non-empty unique strings."));
							}
							Seen.Add(Item->AsString());
						}
					}
					else if (Key == TEXT("additionalProperties"))
					{
						if (!Value.IsValid()
							|| (Value->Type != EJson::Boolean
								&& Value->Type != EJson::Object))
						{
							return CompileFail(
								OutError,
								KeywordPath,
								Key,
								TEXT(
									"additionalProperties must be a boolean or schema object."));
						}
						if (Value->Type == EJson::Object)
						{
							Tasks.Add({
								Value->AsObject(),
								KeywordPath,
							});
						}
					}
					else if (Key == TEXT("minProperties")
						|| Key == TEXT("maxProperties")
						|| Key == TEXT("minItems")
						|| Key == TEXT("maxItems")
						|| Key == TEXT("minLength")
						|| Key == TEXT("maxLength"))
					{
						int64 Unused = 0;
						if (!IsNonNegativeInteger(Value, Unused))
						{
							return CompileFail(
								OutError,
								KeywordPath,
								Key,
								TEXT(
									"Bound must be a non-negative integer."));
						}
					}
					else if (Key == TEXT("uniqueItems"))
					{
						if (!Value.IsValid()
							|| Value->Type != EJson::Boolean)
						{
							return CompileFail(
								OutError,
								KeywordPath,
								Key,
								TEXT("uniqueItems must be boolean."));
						}
					}
					else if (Key == TEXT("minimum")
						|| Key == TEXT("maximum")
						|| Key == TEXT("exclusiveMinimum")
						|| Key == TEXT("exclusiveMaximum"))
					{
						FDecimal Decimal;
						if (!IsNumber(Value)
							|| !TryParseDecimal(
								Value->AsString(),
								Decimal))
						{
							return CompileFail(
								OutError,
								KeywordPath,
								Key,
								TEXT("Numeric bound must be a JSON number."));
						}
					}
					// const accepts every JSON value.
				}

				auto ValidateBoundPair =
					[&Task, &OutError](
						const TCHAR* Minimum,
						const TCHAR* Maximum)
					{
						const TSharedPtr<FJsonValue>* Min =
							Task.Schema->Values.Find(Minimum);
						const TSharedPtr<FJsonValue>* Max =
							Task.Schema->Values.Find(Maximum);
						if (!Min || !Max)
						{
							return true;
						}
						int32 Comparison = 0;
						if (!TryCompareNumbers(*Min, *Max, Comparison)
							|| Comparison > 0)
						{
							return CompileFail(
								OutError,
								Task.Path,
								Minimum,
								TEXT(
									"Minimum bound exceeds maximum bound."));
						}
						return true;
					};
				if (!ValidateBoundPair(TEXT("minimum"), TEXT("maximum"))
					|| !ValidateBoundPair(
						TEXT("exclusiveMinimum"),
						TEXT("exclusiveMaximum")))
				{
					return false;
				}
				auto ValidateIntegerBoundPair =
					[&Task, &OutError](
						const TCHAR* Minimum,
						const TCHAR* Maximum)
					{
						const TSharedPtr<FJsonValue>* Min =
							Task.Schema->Values.Find(Minimum);
						const TSharedPtr<FJsonValue>* Max =
							Task.Schema->Values.Find(Maximum);
						if (!Min || !Max)
						{
							return true;
						}
						int64 MinValue = 0;
						int64 MaxValue = 0;
						if (!IsNonNegativeInteger(*Min, MinValue)
							|| !IsNonNegativeInteger(*Max, MaxValue)
							|| MinValue > MaxValue)
						{
							return CompileFail(
								OutError,
								Task.Path,
								Minimum,
								TEXT(
									"Minimum count exceeds maximum count."));
						}
						return true;
					};
				if (!ValidateIntegerBoundPair(
						TEXT("minProperties"),
						TEXT("maxProperties"))
					|| !ValidateIntegerBoundPair(
						TEXT("minItems"),
						TEXT("maxItems"))
					|| !ValidateIntegerBoundPair(
						TEXT("minLength"),
						TEXT("maxLength")))
				{
					return false;
				}
			}
			return true;
		}

		bool InstanceMatchesType(
			const TSharedPtr<FJsonValue>& Instance,
			const FString& Type)
		{
			if (!Instance.IsValid())
			{
				return false;
			}
			if (Type == TEXT("null"))
			{
				return Instance->Type == EJson::Null;
			}
			if (Type == TEXT("boolean"))
			{
				return Instance->Type == EJson::Boolean;
			}
			if (Type == TEXT("object"))
			{
				return Instance->Type == EJson::Object;
			}
			if (Type == TEXT("array"))
			{
				return Instance->Type == EJson::Array;
			}
			if (Type == TEXT("number"))
			{
				return Instance->Type == EJson::Number;
			}
			if (Type == TEXT("integer"))
			{
				return IsIntegerNumber(Instance);
			}
			if (Type == TEXT("string"))
			{
				return Instance->Type == EJson::String;
			}
			return false;
		}

		bool MatchesTypeDeclaration(
			const TSharedPtr<FJsonValue>& Instance,
			const TSharedPtr<FJsonValue>& TypeDeclaration)
		{
			if (TypeDeclaration->Type == EJson::String)
			{
				return InstanceMatchesType(
					Instance,
					TypeDeclaration->AsString());
			}
			for (const TSharedPtr<FJsonValue>& Type :
				TypeDeclaration->AsArray())
			{
				if (InstanceMatchesType(Instance, Type->AsString()))
				{
					return true;
				}
			}
			return false;
		}

		enum class EFramePhase : uint8
		{
			Local,
			AllOf,
			AnyOf,
			OneOf,
			Not,
			ObjectChildren,
			ArrayChildren,
			Done,
		};

		enum class EChildRelation : uint8
		{
			None,
			AllOf,
			AnyOf,
			OneOf,
			Not,
			Object,
			Array,
		};

		struct FValidationResult
		{
			bool bSuccess = false;
			FUeAgentContractSchemaError Error;
		};

		struct FValidationFrame
		{
			TSharedPtr<FJsonObject> Schema;
			TSharedPtr<FJsonValue> Instance;
			FString InstancePath;
			FString SchemaPath;
			EFramePhase Phase = EFramePhase::Local;
			EChildRelation AwaitedRelation = EChildRelation::None;
			int32 ChildIndex = 0;
			int32 SuccessfulBranches = 0;
			TArray<FString> ObjectKeys;
			FUeAgentContractSchemaError FirstBranchError;
		};

		FValidationResult ValidateLocal(
			const FValidationFrame& Frame)
		{
			FValidationResult Result;
			Result.bSuccess = false;

			if (const TSharedPtr<FJsonValue>* Type =
				Frame.Schema->Values.Find(TEXT("type")))
			{
				if (!MatchesTypeDeclaration(Frame.Instance, *Type))
				{
					ValidationFail(
						Result.Error,
						Frame.InstancePath,
						AppendPath(Frame.SchemaPath, TEXT("type")),
						TEXT("type"),
						TEXT("Instance type is not allowed."));
					return Result;
				}
			}

			auto IsCanonicalEqual =
				[&Frame](const TSharedPtr<FJsonValue>& Expected)
				{
					FString ActualText;
					FString ExpectedText;
					return TryCanonicalizeValue(
							Frame.Instance,
							ActualText)
						&& TryCanonicalizeValue(
							Expected,
							ExpectedText)
						&& ActualText == ExpectedText;
				};
			if (const TSharedPtr<FJsonValue>* Const =
				Frame.Schema->Values.Find(TEXT("const")))
			{
				if (!IsCanonicalEqual(*Const))
				{
					ValidationFail(
						Result.Error,
						Frame.InstancePath,
						AppendPath(Frame.SchemaPath, TEXT("const")),
						TEXT("const"),
						TEXT("Instance does not equal const."));
					return Result;
				}
			}
			if (const TSharedPtr<FJsonValue>* Enum =
				Frame.Schema->Values.Find(TEXT("enum")))
			{
				bool bMatched = false;
				for (const TSharedPtr<FJsonValue>& Candidate :
					(*Enum)->AsArray())
				{
					if (IsCanonicalEqual(Candidate))
					{
						bMatched = true;
						break;
					}
				}
				if (!bMatched)
				{
					ValidationFail(
						Result.Error,
						Frame.InstancePath,
						AppendPath(Frame.SchemaPath, TEXT("enum")),
						TEXT("enum"),
						TEXT("Instance is not in enum."));
					return Result;
				}
			}

			if (Frame.Instance->Type == EJson::Object)
			{
				const TSharedPtr<FJsonObject> Object =
					Frame.Instance->AsObject();
				auto ValidateCount =
					[&](const TCHAR* Keyword, const bool bMinimum)
					{
						const TSharedPtr<FJsonValue>* Bound =
							Frame.Schema->Values.Find(Keyword);
						if (!Bound)
						{
							return true;
						}
						int64 Limit = 0;
						IsNonNegativeInteger(*Bound, Limit);
						const int64 Observed = Object->Values.Num();
						if ((bMinimum && Observed < Limit)
							|| (!bMinimum && Observed > Limit))
						{
							return ValidationFail(
								Result.Error,
								Frame.InstancePath,
								AppendPath(
									Frame.SchemaPath,
									Keyword),
								Keyword,
								TEXT("Object property count is outside the bound."));
						}
						return true;
					};
				if (!ValidateCount(TEXT("minProperties"), true)
					|| !ValidateCount(TEXT("maxProperties"), false))
				{
					return Result;
				}
				if (const TSharedPtr<FJsonValue>* Required =
					Frame.Schema->Values.Find(TEXT("required")))
				{
					for (const TSharedPtr<FJsonValue>& RequiredName :
						(*Required)->AsArray())
					{
						const FString Name =
							RequiredName->AsString();
						if (!Object->HasField(Name))
						{
							ValidationFail(
								Result.Error,
								AppendPath(
									Frame.InstancePath,
									Name),
								AppendPath(
									Frame.SchemaPath,
									TEXT("required")),
								TEXT("required"),
								TEXT("Required property is missing."));
							return Result;
						}
					}
				}
				const TSharedPtr<FJsonValue>* Properties =
					Frame.Schema->Values.Find(TEXT("properties"));
				const TSharedPtr<FJsonValue>* Additional =
					Frame.Schema->Values.Find(
						TEXT("additionalProperties"));
				if (Additional
					&& (*Additional)->Type == EJson::Boolean
					&& !(*Additional)->AsBool())
				{
					for (const TPair<
						FString,
						TSharedPtr<FJsonValue>>& Field :
						Object->Values)
					{
						if (!Properties
							|| !(*Properties)->AsObject()
								->HasField(Field.Key))
						{
							ValidationFail(
								Result.Error,
								AppendPath(
									Frame.InstancePath,
									Field.Key),
								AppendPath(
									Frame.SchemaPath,
									TEXT(
										"additionalProperties")),
								TEXT("additionalProperties"),
								TEXT(
									"Additional property is not allowed."));
							return Result;
						}
					}
				}
			}

			if (Frame.Instance->Type == EJson::Array)
			{
				const TArray<TSharedPtr<FJsonValue>>& Array =
					Frame.Instance->AsArray();
				auto ValidateCount =
					[&](const TCHAR* Keyword, const bool bMinimum)
					{
						const TSharedPtr<FJsonValue>* Bound =
							Frame.Schema->Values.Find(Keyword);
						if (!Bound)
						{
							return true;
						}
						int64 Limit = 0;
						IsNonNegativeInteger(*Bound, Limit);
						const int64 Observed = Array.Num();
						if ((bMinimum && Observed < Limit)
							|| (!bMinimum && Observed > Limit))
						{
							return ValidationFail(
								Result.Error,
								Frame.InstancePath,
								AppendPath(
									Frame.SchemaPath,
									Keyword),
								Keyword,
								TEXT("Array item count is outside the bound."));
						}
						return true;
					};
				if (!ValidateCount(TEXT("minItems"), true)
					|| !ValidateCount(TEXT("maxItems"), false))
				{
					return Result;
				}
				const TSharedPtr<FJsonValue>* Unique =
					Frame.Schema->Values.Find(TEXT("uniqueItems"));
				if (Unique && (*Unique)->AsBool())
				{
					TSet<FString> Seen;
					for (int32 Index = 0; Index < Array.Num(); ++Index)
					{
						FString Canonical;
						if (!TryCanonicalizeValue(
								Array[Index],
								Canonical)
							|| Seen.Contains(Canonical))
						{
							ValidationFail(
								Result.Error,
								AppendPath(
									Frame.InstancePath,
									LexToString(Index)),
								AppendPath(
									Frame.SchemaPath,
									TEXT("uniqueItems")),
								TEXT("uniqueItems"),
								TEXT("Array items are not unique."));
							return Result;
						}
						Seen.Add(MoveTemp(Canonical));
					}
				}
			}

			if (Frame.Instance->Type == EJson::String)
			{
				const int64 Length =
					UnicodeScalarCount(Frame.Instance->AsString());
				auto ValidateLength =
					[&](const TCHAR* Keyword, const bool bMinimum)
					{
						const TSharedPtr<FJsonValue>* Bound =
							Frame.Schema->Values.Find(Keyword);
						if (!Bound)
						{
							return true;
						}
						int64 Limit = 0;
						IsNonNegativeInteger(*Bound, Limit);
						if ((bMinimum && Length < Limit)
							|| (!bMinimum && Length > Limit))
						{
							return ValidationFail(
								Result.Error,
								Frame.InstancePath,
								AppendPath(
									Frame.SchemaPath,
									Keyword),
								Keyword,
								TEXT("String length is outside the bound."));
						}
						return true;
					};
				if (!ValidateLength(TEXT("minLength"), true)
					|| !ValidateLength(TEXT("maxLength"), false))
				{
					return Result;
				}
			}

			if (Frame.Instance->Type == EJson::Number)
			{
				auto ValidateNumber =
					[&](const TCHAR* Keyword, const int32 RequiredSign)
					{
						const TSharedPtr<FJsonValue>* Bound =
							Frame.Schema->Values.Find(Keyword);
						if (!Bound)
						{
							return true;
						}
						int32 Comparison = 0;
						if (!TryCompareNumbers(
								Frame.Instance,
								*Bound,
								Comparison)
							|| (RequiredSign < 0
								&& Comparison < 0)
							|| (RequiredSign > 0
								&& Comparison > 0)
							|| (RequiredSign == -2
								&& Comparison <= 0)
							|| (RequiredSign == 2
								&& Comparison >= 0))
						{
							return ValidationFail(
								Result.Error,
								Frame.InstancePath,
								AppendPath(
									Frame.SchemaPath,
									Keyword),
								Keyword,
								TEXT("Number is outside the bound."));
						}
						return true;
					};
				if (!ValidateNumber(TEXT("minimum"), -1)
					|| !ValidateNumber(TEXT("maximum"), 1)
					|| !ValidateNumber(TEXT("exclusiveMinimum"), -2)
					|| !ValidateNumber(TEXT("exclusiveMaximum"), 2))
				{
					return Result;
				}
			}

			Result.bSuccess = true;
			return Result;
		}

		FValidationFrame MakeChildFrame(
			const FValidationFrame& Parent,
			TSharedPtr<FJsonObject> Schema,
			TSharedPtr<FJsonValue> Instance,
			FString InstancePath,
			FString SchemaPath)
		{
			FValidationFrame Child;
			Child.Schema = MoveTemp(Schema);
			Child.Instance = MoveTemp(Instance);
			Child.InstancePath = MoveTemp(InstancePath);
			Child.SchemaPath = MoveTemp(SchemaPath);
			return Child;
		}

		bool ValidateInstance(
			const TSharedRef<const FJsonObject>& Root,
			const TSharedPtr<FJsonValue>& Instance,
			FUeAgentContractSchemaCheckpoint& Checkpoint,
			FUeAgentContractSchemaError& OutError)
		{
			TArray<FValidationFrame> Stack;
			FValidationFrame RootFrame;
			RootFrame.Schema =
				ConstCastSharedRef<FJsonObject>(Root).ToSharedPtr();
			RootFrame.Instance = Instance;
			Stack.Add(MoveTemp(RootFrame));

			TOptional<FValidationResult> ChildResult;
			while (!Stack.IsEmpty())
			{
				FValidationFrame& Frame = Stack.Last();
				if (Frame.AwaitedRelation != EChildRelation::None
					&& ChildResult.IsSet())
				{
					const FValidationResult Completed =
						MoveTemp(ChildResult.GetValue());
					ChildResult.Reset();
					const EChildRelation Relation =
						Frame.AwaitedRelation;
					Frame.AwaitedRelation = EChildRelation::None;
					if (Relation == EChildRelation::AllOf
						|| Relation == EChildRelation::Object
						|| Relation == EChildRelation::Array)
					{
						if (!Completed.bSuccess)
						{
							ChildResult.Emplace(Completed);
							Stack.Pop(EAllowShrinking::No);
							continue;
						}
					}
					else if (Relation == EChildRelation::AnyOf)
					{
						if (Completed.bSuccess)
						{
							Frame.SuccessfulBranches = 1;
						}
						else if (!Frame.FirstBranchError.IsValid())
						{
							Frame.FirstBranchError =
								Completed.Error;
						}
					}
					else if (Relation == EChildRelation::OneOf)
					{
						if (Completed.bSuccess)
						{
							++Frame.SuccessfulBranches;
						}
						else if (!Frame.FirstBranchError.IsValid())
						{
							Frame.FirstBranchError =
								Completed.Error;
						}
					}
					else if (Relation == EChildRelation::Not)
					{
						if (Completed.bSuccess)
						{
							FValidationResult Failure;
							ValidationFail(
								Failure.Error,
								Frame.InstancePath,
								AppendPath(
									Frame.SchemaPath,
									TEXT("not")),
								TEXT("not"),
								TEXT("Instance matches forbidden schema."));
							ChildResult.Emplace(
								MoveTemp(Failure));
							Stack.Pop(EAllowShrinking::No);
							continue;
						}
					}
				}

				if (Frame.Phase == EFramePhase::Local)
				{
					FUeAgentContractSchemaError CheckpointError;
					if (!Checkpoint(
							1,
							Frame.InstancePath,
							CheckpointError))
					{
						if (!CheckpointError.IsValid())
						{
							Fail(
								CheckpointError,
								TEXT(
									"contract_schema_checkpoint_failed"),
								Frame.InstancePath,
								Frame.SchemaPath,
								FString(),
								TEXT(
									"Validation checkpoint rejected work."));
						}
						FValidationResult Failure;
						Failure.Error =
							MoveTemp(CheckpointError);
						ChildResult.Emplace(
							MoveTemp(Failure));
						Stack.Pop(EAllowShrinking::No);
						continue;
					}
					FValidationResult Local = ValidateLocal(Frame);
					if (!Local.bSuccess)
					{
						ChildResult.Emplace(MoveTemp(Local));
						Stack.Pop(EAllowShrinking::No);
						continue;
					}
					Frame.Phase = EFramePhase::AllOf;
					Frame.ChildIndex = 0;
				}

				auto PushCompositionChild =
					[&Stack, &Frame](
						const TCHAR* Keyword,
						const EChildRelation Relation)
					{
						const TSharedPtr<FJsonValue>* Value =
							Frame.Schema->Values.Find(Keyword);
						if (!Value
							|| Frame.ChildIndex
								>= (*Value)->AsArray().Num())
						{
							return false;
						}
						const int32 Index = Frame.ChildIndex++;
						Frame.AwaitedRelation = Relation;
						Stack.Add(MakeChildFrame(
							Frame,
							(*Value)->AsArray()[Index]->AsObject(),
							Frame.Instance,
							Frame.InstancePath,
							AppendPath(
								AppendPath(
									Frame.SchemaPath,
									Keyword),
								LexToString(Index))));
						return true;
					};

				if (Frame.Phase == EFramePhase::AllOf)
				{
					if (PushCompositionChild(
							TEXT("allOf"),
							EChildRelation::AllOf))
					{
						continue;
					}
					Frame.Phase = EFramePhase::AnyOf;
					Frame.ChildIndex = 0;
					Frame.SuccessfulBranches = 0;
					Frame.FirstBranchError = {};
				}
				if (Frame.Phase == EFramePhase::AnyOf)
				{
					const TSharedPtr<FJsonValue>* AnyOf =
						Frame.Schema->Values.Find(TEXT("anyOf"));
					if (AnyOf && Frame.SuccessfulBranches == 0
						&& PushCompositionChild(
							TEXT("anyOf"),
							EChildRelation::AnyOf))
					{
						continue;
					}
					if (AnyOf && Frame.SuccessfulBranches == 0)
					{
						FValidationResult Failure;
						ValidationFail(
							Failure.Error,
							Frame.InstancePath,
							AppendPath(
								Frame.SchemaPath,
								TEXT("anyOf")),
							TEXT("anyOf"),
							TEXT("Instance matches no anyOf branch."));
						ChildResult.Emplace(MoveTemp(Failure));
						Stack.Pop(EAllowShrinking::No);
						continue;
					}
					Frame.Phase = EFramePhase::OneOf;
					Frame.ChildIndex = 0;
					Frame.SuccessfulBranches = 0;
					Frame.FirstBranchError = {};
				}
				if (Frame.Phase == EFramePhase::OneOf)
				{
					if (PushCompositionChild(
							TEXT("oneOf"),
							EChildRelation::OneOf))
					{
						continue;
					}
					if (Frame.Schema->HasField(TEXT("oneOf"))
						&& Frame.SuccessfulBranches != 1)
					{
						FValidationResult Failure;
						ValidationFail(
							Failure.Error,
							Frame.InstancePath,
							AppendPath(
								Frame.SchemaPath,
								TEXT("oneOf")),
							TEXT("oneOf"),
							TEXT(
								"Instance must match exactly one oneOf branch."));
						ChildResult.Emplace(MoveTemp(Failure));
						Stack.Pop(EAllowShrinking::No);
						continue;
					}
					Frame.Phase = EFramePhase::Not;
					Frame.ChildIndex = 0;
				}
				if (Frame.Phase == EFramePhase::Not)
				{
					const TSharedPtr<FJsonValue>* Not =
						Frame.Schema->Values.Find(TEXT("not"));
					if (Not && Frame.ChildIndex == 0)
					{
						Frame.ChildIndex = 1;
						Frame.AwaitedRelation =
							EChildRelation::Not;
						Stack.Add(MakeChildFrame(
							Frame,
							(*Not)->AsObject(),
							Frame.Instance,
							Frame.InstancePath,
							AppendPath(
								Frame.SchemaPath,
								TEXT("not"))));
						continue;
					}
					Frame.Phase =
						EFramePhase::ObjectChildren;
					Frame.ChildIndex = 0;
					Frame.ObjectKeys.Reset();
					if (Frame.Instance->Type == EJson::Object)
					{
						Frame.Instance->AsObject()
							->Values.GetKeys(Frame.ObjectKeys);
						Frame.ObjectKeys.Sort();
					}
				}
				if (Frame.Phase == EFramePhase::ObjectChildren)
				{
					bool bPushed = false;
					if (Frame.Instance->Type == EJson::Object)
					{
						const TSharedPtr<FJsonValue>* Properties =
							Frame.Schema->Values.Find(
								TEXT("properties"));
						const TSharedPtr<FJsonValue>* Additional =
							Frame.Schema->Values.Find(
								TEXT("additionalProperties"));
						while (Frame.ChildIndex
							< Frame.ObjectKeys.Num())
						{
							const FString Key =
								Frame.ObjectKeys[
									Frame.ChildIndex++];
							TSharedPtr<FJsonObject> ChildSchema;
							FString ChildSchemaPath;
							if (Properties)
							{
								const TSharedPtr<FJsonValue>* Property =
									(*Properties)->AsObject()
										->Values.Find(Key);
								if (Property)
								{
									ChildSchema =
										(*Property)->AsObject();
									ChildSchemaPath = AppendPath(
										AppendPath(
											Frame.SchemaPath,
											TEXT("properties")),
										Key);
								}
							}
							if (!ChildSchema.IsValid()
								&& Additional
								&& (*Additional)->Type
									== EJson::Object)
							{
								ChildSchema =
									(*Additional)->AsObject();
								ChildSchemaPath = AppendPath(
									Frame.SchemaPath,
									TEXT("additionalProperties"));
							}
							if (ChildSchema.IsValid())
							{
								Frame.AwaitedRelation =
									EChildRelation::Object;
								Stack.Add(MakeChildFrame(
									Frame,
									MoveTemp(ChildSchema),
									Frame.Instance->AsObject()
										->Values.FindChecked(Key),
									AppendPath(
										Frame.InstancePath,
										Key),
									MoveTemp(
										ChildSchemaPath)));
								bPushed = true;
								break;
							}
						}
					}
					if (bPushed)
					{
						continue;
					}
					Frame.Phase = EFramePhase::ArrayChildren;
					Frame.ChildIndex = 0;
				}
				if (Frame.Phase == EFramePhase::ArrayChildren)
				{
					const TSharedPtr<FJsonValue>* Items =
						Frame.Schema->Values.Find(TEXT("items"));
					if (Items
						&& Frame.Instance->Type == EJson::Array
						&& Frame.ChildIndex
							< Frame.Instance->AsArray().Num())
					{
						const int32 Index = Frame.ChildIndex++;
						Frame.AwaitedRelation =
							EChildRelation::Array;
						Stack.Add(MakeChildFrame(
							Frame,
							(*Items)->AsObject(),
							Frame.Instance->AsArray()[Index],
							AppendPath(
								Frame.InstancePath,
								LexToString(Index)),
							AppendPath(
								Frame.SchemaPath,
								TEXT("items"))));
						continue;
					}
					Frame.Phase = EFramePhase::Done;
				}
				if (Frame.Phase == EFramePhase::Done)
				{
					FValidationResult Success;
					Success.bSuccess = true;
					ChildResult.Emplace(MoveTemp(Success));
					Stack.Pop(EAllowShrinking::No);
				}
			}
			if (!ChildResult.IsSet()
				|| !ChildResult->bSuccess)
			{
				OutError = ChildResult.IsSet()
					? ChildResult->Error
					: FUeAgentContractSchemaError();
				return false;
			}
			return true;
		}
	}

	FUeAgentContractSchema::FUeAgentContractSchema(
		FUeAgentContractValue InCanonicalSchema,
		TSharedRef<const FJsonObject> InRoot)
		: CanonicalSchema(MoveTemp(InCanonicalSchema))
		, Root(MoveTemp(InRoot))
	{
	}

	bool FUeAgentContractSchema::TryCompile(
		const FUeAgentContractValue& Schema,
		TSharedPtr<const FUeAgentContractSchema>& OutSchema,
		FUeAgentContractSchemaError& OutError)
	{
		OutSchema.Reset();
		OutError = {};
		if (!Schema.IsValid())
		{
			return CompileFail(
				OutError,
				FString(),
				FString(),
				TEXT("Schema text is empty."));
		}
		TSharedPtr<FJsonObject> RootObject;
		FString JsonError;
		if (!Json::TryParseObject(
				Schema.GetJsonValue(),
				RootObject,
				JsonError))
		{
			return CompileFail(
				OutError,
				FString(),
				FString(),
				TEXT("Schema root must be a JSON object."));
		}
		if (!ValidateSchemaObject(RootObject, OutError))
		{
			return false;
		}
		FString CanonicalText;
		if (!Json::TryCanonicalize(
				RootObject,
				CanonicalText,
				JsonError))
		{
			return CompileFail(
				OutError,
				FString(),
				FString(),
				TEXT("Schema canonicalization failed."));
		}
		OutSchema = MakeShareable(new FUeAgentContractSchema(
			FUeAgentContractValue(MoveTemp(CanonicalText)),
			StaticCastSharedRef<const FJsonObject>(
				RootObject.ToSharedRef())));
		return true;
	}

	const FUeAgentContractValue&
	FUeAgentContractSchema::GetCanonicalSchema() const
	{
		return CanonicalSchema;
	}

	bool FUeAgentContractSchema::TryValidateAndCanonicalize(
		const FUeAgentContractValue& Input,
		FUeAgentContractSchemaCheckpoint Checkpoint,
		TOptional<FUeAgentContractValue>& OutCanonicalInput,
		FUeAgentContractSchemaError& OutError) const
	{
		OutCanonicalInput.Reset();
		OutError = {};
		if (!Input.IsValid())
		{
			return ValidationFail(
				OutError,
				FString(),
				FString(),
				FString(),
				TEXT("Input JSON is empty."));
		}
		if (!Checkpoint)
		{
			return Fail(
				OutError,
				TEXT("contract_schema_checkpoint_missing"),
				FString(),
				FString(),
				FString(),
				TEXT("Validation requires a work checkpoint."));
		}
		TSharedPtr<FJsonValue> InputValue;
		if (!TryParseJsonValue(
				Input.GetJsonValue(),
				InputValue))
		{
			return ValidationFail(
				OutError,
				FString(),
				FString(),
				FString(),
				TEXT("Input is not valid JSON."));
		}
		if (!ValidateInstance(
				Root,
				InputValue,
				Checkpoint,
				OutError))
		{
			return false;
		}
		FString CanonicalInput;
		if (!TryCanonicalizeValue(InputValue, CanonicalInput))
		{
			return Fail(
				OutError,
				TEXT("contract_input_canonicalization_failed"),
				FString(),
				FString(),
				FString(),
				TEXT("Validated input could not be canonicalized."));
		}
		OutCanonicalInput.Emplace(MoveTemp(CanonicalInput));
		return true;
	}
}

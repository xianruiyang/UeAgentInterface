#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Core/StateSemantic/UeAgentExactStringKeys.h"
#include "Dom/JsonValue.h"

#include "Algo/Sort.h"
#include "Core/StateSemantic/UeAgentStateSemanticEvidence.h"
#include "Core/StateSemantic/UeAgentStateSemanticNormalization.h"

namespace UE::UAI::StateSemantic
{
	namespace
	{
		using namespace UE::UAI::ContextResource;

		struct FExactNumber
		{
			bool bNegative = false;
			FString Digits = TEXT("0");
			int32 PowerOfTen = 0;
		};

		struct FSignedMagnitude
		{
			bool bNegative = false;
			FString Magnitude = TEXT("0");
		};

		struct FExactRational
		{
			FSignedMagnitude Numerator;
			FString Denominator = TEXT("1");
		};

		using FExactVector = TArray<FExactRational>;
		using FExactMatrix = TArray<FExactVector>;

		void ResetError(FUeAgentStateSemanticError& OutError)
		{
			OutError = FUeAgentStateSemanticError{};
		}

		bool Fail(
			FUeAgentStateSemanticError& OutError,
			const FString& Code,
			const FString& Reason,
			const FString& Path,
			TSharedPtr<FJsonValue> Expected = nullptr,
			TSharedPtr<FJsonValue> Actual = nullptr)
		{
			OutError = FUeAgentStateSemanticError::Failure(Code, Reason, TEXT("state_semantic"));
			OutError.Source.JsonPath = Path;
			OutError.ExpectedValue = MoveTemp(Expected); OutError.ActualValue = MoveTemp(Actual);
			return false;
		}

		bool Charge(
			FUeAgentResourceTracker& Resources,
			const TMap<EUeAgentResourceDimension, int64>& Amounts,
			const FString& Path,
			FUeAgentStateSemanticError& OutError)
		{
			FUeAgentResourceChargeError ChargeError;
			if (Resources.TryChargeMany(Amounts, Path, ChargeError))
			{
				return true;
			}
			OutError = ChargeError;
			return false;
		}

		bool IsNonEmpty(const FString& Value)
		{
			return !Value.TrimStartAndEnd().IsEmpty();
		}

		bool IsAsciiDigit(TCHAR Character)
		{
			return Character >= TEXT('0') && Character <= TEXT('9');
		}

		bool TryParseIntegerMagnitude(const FString& Text, bool bSigned, bool& bNegative, uint64& Magnitude)
		{
			if (Text.IsEmpty()) return false;
			int32 Index = 0;
			bNegative = Text[0] == TEXT('-');
			if (bNegative && !bSigned) return false;
			if (bNegative || Text[0] == TEXT('+')) ++Index;
			if (Index == Text.Len()) return false;
			const uint64 Limit = bSigned
				? static_cast<uint64>(MAX_int64) + (bNegative ? 1ull : 0ull) : MAX_uint64;
			uint64 Result = 0;
			for (; Index < Text.Len(); ++Index)
			{
				if (!IsAsciiDigit(Text[Index])) return false;
				const uint64 Digit = Text[Index] - TEXT('0');
				if (Result > (Limit - Digit) / 10) return false;
				Result = Result * 10 + Digit;
			}
			Magnitude = Result;
			return true;
		}

		bool IsCanonicalUnsignedInteger(
			const FString& Value,
			bool bAllowLeadingZeroOnly)
		{
			if (Value.IsEmpty())
			{
				return false;
			}
			if (Value.Len() > 1 && Value[0] == TEXT('0'))
			{
				return false;
			}
			if (!bAllowLeadingZeroOnly && Value == TEXT("0"))
			{
				return false;
			}
			for (const TCHAR Character : Value)
			{
				if (!IsAsciiDigit(Character))
				{
					return false;
				}
			}
			return true;
		}

		bool IsCanonicalIntegerLexeme(const FString& Value)
		{
			int32 Index = 0;
			if (Value.StartsWith(TEXT("-")))
			{
				Index = 1;
			}
			if (Index >= Value.Len())
			{
				return false;
			}
			return IsCanonicalUnsignedInteger(Value.Mid(Index), true);
		}

		bool ParseExponent(const FString& Value, int32& OutExponent)
		{
			if (Value.IsEmpty())
			{
				OutExponent = 0;
				return true;
			}
			int32 Index = 0;
			bool bNegative = false;
			if (Value[Index] == TEXT('+') || Value[Index] == TEXT('-'))
			{
				bNegative = Value[Index] == TEXT('-');
				++Index;
			}
			if (Index >= Value.Len())
			{
				return false;
			}
			// JSON and native float formatters allow zero-padded exponent digits (e-06).
			// Normalize only the exponent; the mantissa's integer rules remain unchanged.
			while (Index + 1 < Value.Len() && Value[Index] == TEXT('0'))
			{
				++Index;
			}
			const FString Magnitude = Value.Mid(Index);
			if (!IsCanonicalUnsignedInteger(Magnitude, true))
			{
				return false;
			}
			int64 Parsed = 0;
			for (const TCHAR Character : Magnitude)
			{
				const int32 Digit = Character - TEXT('0');
				if (Parsed > (static_cast<int64>(MAX_int32) - Digit) / 10)
				{
					return false;
				}
				Parsed = Parsed * 10 + Digit;
			}
			OutExponent = bNegative ? -static_cast<int32>(Parsed) : static_cast<int32>(Parsed);
			return true;
		}

		bool ParseExactNumber(const FString& Lexeme, FExactNumber& OutNumber)
		{
			if (Lexeme.IsEmpty())
			{
				return false;
			}

			int32 Index = 0;
			bool bNegative = false;
			if (Lexeme[Index] == TEXT('-'))
			{
				bNegative = true;
				++Index;
			}
			if (Index >= Lexeme.Len())
			{
				return false;
			}

			const int32 ExponentIndexLower = Lexeme.Find(TEXT("e"), ESearchCase::CaseSensitive);
			const int32 ExponentIndexUpper = Lexeme.Find(TEXT("E"), ESearchCase::CaseSensitive);
			int32 ExponentIndex = INDEX_NONE;
			if (ExponentIndexLower != INDEX_NONE && ExponentIndexUpper != INDEX_NONE)
			{
				return false;
			}
			ExponentIndex = ExponentIndexLower != INDEX_NONE
				? ExponentIndexLower
				: ExponentIndexUpper;
			const FString Mantissa = ExponentIndex == INDEX_NONE
				? Lexeme.Mid(Index)
				: Lexeme.Mid(Index, ExponentIndex - Index);
			const FString ExponentText = ExponentIndex == INDEX_NONE
				? FString()
				: Lexeme.Mid(ExponentIndex + 1);

			int32 Exponent = 0;
			if (!ParseExponent(ExponentText, Exponent))
			{
				return false;
			}

			int32 DotIndex = Mantissa.Find(TEXT("."));
			if (DotIndex != INDEX_NONE
				&& Mantissa.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromStart, DotIndex + 1) != INDEX_NONE)
			{
				return false;
			}
			const FString IntegerPart = DotIndex == INDEX_NONE
				? Mantissa
				: Mantissa.Left(DotIndex);
			const FString FractionPart = DotIndex == INDEX_NONE
				? FString()
				: Mantissa.Mid(DotIndex + 1);
			if (!IsCanonicalUnsignedInteger(IntegerPart, true)
				|| (DotIndex != INDEX_NONE && FractionPart.IsEmpty()))
			{
				return false;
			}
			for (const TCHAR Character : FractionPart)
			{
				if (!IsAsciiDigit(Character))
				{
					return false;
				}
			}

			FString Digits = IntegerPart + FractionPart;
			int32 FirstNonZero = 0;
			while (FirstNonZero < Digits.Len() && Digits[FirstNonZero] == TEXT('0'))
			{
				++FirstNonZero;
			}
			if (FirstNonZero == Digits.Len())
			{
				OutNumber = FExactNumber{};
				return true;
			}
			Digits.RightChopInline(FirstNonZero, EAllowShrinking::No);
			const int64 InitialPower =
				static_cast<int64>(Exponent) - FractionPart.Len();
			if (InitialPower < MIN_int32 || InitialPower > MAX_int32)
			{
				return false;
			}
			int32 Power = static_cast<int32>(InitialPower);
			while (Digits.Len() > 1 && Digits.EndsWith(TEXT("0")))
			{
				Digits.LeftChopInline(1, EAllowShrinking::No);
				if (Power == MAX_int32)
				{
					return false;
				}
				++Power;
			}
			OutNumber.bNegative = bNegative;
			OutNumber.Digits = MoveTemp(Digits);
			OutNumber.PowerOfTen = Power;
			return true;
		}

		bool ExactNumbersEqual(const FString& Left, const FString& Right)
		{
			FExactNumber LeftNumber;
			FExactNumber RightNumber;
			return ParseExactNumber(Left, LeftNumber)
				&& ParseExactNumber(Right, RightNumber)
				&& LeftNumber.bNegative == RightNumber.bNegative
				&& LeftNumber.Digits == RightNumber.Digits
				&& LeftNumber.PowerOfTen == RightNumber.PowerOfTen;
		}

		FString StripLeadingZeros(FString Value)
		{
			int32 Index = 0;
			while (Index + 1 < Value.Len() && Value[Index] == TEXT('0'))
			{
				++Index;
			}
			if (Index > 0)
			{
				Value.RightChopInline(Index, EAllowShrinking::No);
			}
			return Value.IsEmpty() ? FString(TEXT("0")) : Value;
		}

		int32 CompareMagnitude(const FString& Left, const FString& Right)
		{
			const FString A = StripLeadingZeros(Left);
			const FString B = StripLeadingZeros(Right);
			if (A.Len() != B.Len())
			{
				return A.Len() < B.Len() ? -1 : 1;
			}
			const int32 Comparison = A.Compare(B, ESearchCase::CaseSensitive);
			return Comparison < 0 ? -1 : Comparison > 0 ? 1 : 0;
		}

		FString AddMagnitude(const FString& Left, const FString& Right)
		{
			int32 LeftIndex = Left.Len() - 1;
			int32 RightIndex = Right.Len() - 1;
			int32 Carry = 0;
			FString Reversed;
			Reversed.Reserve(FMath::Max(Left.Len(), Right.Len()) + 1);
			while (LeftIndex >= 0 || RightIndex >= 0 || Carry != 0)
			{
				const int32 LeftDigit = LeftIndex >= 0 ? Left[LeftIndex--] - TEXT('0') : 0;
				const int32 RightDigit = RightIndex >= 0 ? Right[RightIndex--] - TEXT('0') : 0;
				const int32 Sum = LeftDigit + RightDigit + Carry;
				Reversed.AppendChar(static_cast<TCHAR>(TEXT('0') + Sum % 10));
				Carry = Sum / 10;
			}
			FString Result;
			Result.Reserve(Reversed.Len());
			for (int32 Index = Reversed.Len() - 1; Index >= 0; --Index)
			{
				Result.AppendChar(Reversed[Index]);
			}
			return StripLeadingZeros(MoveTemp(Result));
		}

		FString SubtractMagnitude(const FString& Left, const FString& Right)
		{
			int32 LeftIndex = Left.Len() - 1;
			int32 RightIndex = Right.Len() - 1;
			int32 Borrow = 0;
			FString Reversed;
			Reversed.Reserve(Left.Len());
			while (LeftIndex >= 0)
			{
				int32 Digit = Left[LeftIndex--] - TEXT('0') - Borrow;
				const int32 Subtrahend =
					RightIndex >= 0 ? Right[RightIndex--] - TEXT('0') : 0;
				if (Digit < Subtrahend)
				{
					Digit += 10;
					Borrow = 1;
				}
				else
				{
					Borrow = 0;
				}
				Reversed.AppendChar(static_cast<TCHAR>(TEXT('0') + Digit - Subtrahend));
			}
			FString Result;
			Result.Reserve(Reversed.Len());
			for (int32 Index = Reversed.Len() - 1; Index >= 0; --Index)
			{
				Result.AppendChar(Reversed[Index]);
			}
			return StripLeadingZeros(MoveTemp(Result));
		}

		FString MultiplyMagnitude(const FString& Left, const FString& Right)
		{
			const FString A = StripLeadingZeros(Left);
			const FString B = StripLeadingZeros(Right);
			if (A == TEXT("0") || B == TEXT("0"))
			{
				return TEXT("0");
			}
			TArray<int32> Digits;
			Digits.Init(0, A.Len() + B.Len());
			for (int32 I = A.Len() - 1; I >= 0; --I)
			{
				for (int32 J = B.Len() - 1; J >= 0; --J)
				{
					const int32 Position = I + J + 1;
					const int32 Product =
						(A[I] - TEXT('0')) * (B[J] - TEXT('0')) + Digits[Position];
					Digits[Position] = Product % 10;
					Digits[Position - 1] += Product / 10;
				}
			}
			FString Result;
			int32 Index = 0;
			while (Index < Digits.Num() - 1 && Digits[Index] == 0)
			{
				++Index;
			}
			for (; Index < Digits.Num(); ++Index)
			{
				Result.AppendChar(static_cast<TCHAR>(TEXT('0') + Digits[Index]));
			}
			return Result;
		}

		FString MultiplyMagnitudeBySmall(const FString& Value, int32 Multiplier)
		{
			check(Multiplier >= 0 && Multiplier <= 10);
			if (Multiplier == 0 || Value == TEXT("0"))
			{
				return TEXT("0");
			}
			int32 Carry = 0;
			FString Reversed;
			for (int32 Index = Value.Len() - 1; Index >= 0; --Index)
			{
				const int32 Product = (Value[Index] - TEXT('0')) * Multiplier + Carry;
				Reversed.AppendChar(static_cast<TCHAR>(TEXT('0') + Product % 10));
				Carry = Product / 10;
			}
			while (Carry > 0)
			{
				Reversed.AppendChar(static_cast<TCHAR>(TEXT('0') + Carry % 10));
				Carry /= 10;
			}
			FString Result;
			for (int32 Index = Reversed.Len() - 1; Index >= 0; --Index)
			{
				Result.AppendChar(Reversed[Index]);
			}
			return StripLeadingZeros(MoveTemp(Result));
		}

		bool DivideMagnitudeBySmall(
			const FString& Value,
			int32 Divisor,
			FString& OutQuotient,
			int32& OutRemainder)
		{
			check(Divisor > 0);
			int32 Remainder = 0;
			FString Quotient;
			for (const TCHAR Character : Value)
			{
				const int32 Current = Remainder * 10 + Character - TEXT('0');
				Quotient.AppendChar(static_cast<TCHAR>(TEXT('0') + Current / Divisor));
				Remainder = Current % Divisor;
			}
			OutQuotient = StripLeadingZeros(MoveTemp(Quotient));
			OutRemainder = Remainder;
			return true;
		}

		void DivideMagnitude(
			const FString& Dividend,
			const FString& Divisor,
			FString& OutQuotient,
			FString& OutRemainder)
		{
			check(Divisor != TEXT("0"));
			FString Quotient;
			FString Remainder = TEXT("0");
			for (const TCHAR Character : StripLeadingZeros(Dividend))
			{
				Remainder = StripLeadingZeros(
					(Remainder == TEXT("0") ? FString() : Remainder)
					+ FString::Chr(Character));
				int32 Digit = 0;
				while (CompareMagnitude(Remainder, Divisor) >= 0)
				{
					Remainder = SubtractMagnitude(Remainder, Divisor);
					++Digit;
				}
				Quotient.AppendChar(static_cast<TCHAR>(TEXT('0') + Digit));
			}
			OutQuotient = StripLeadingZeros(MoveTemp(Quotient));
			OutRemainder = StripLeadingZeros(MoveTemp(Remainder));
		}

		FString GreatestCommonDivisor(FString Left, FString Right)
		{
			Left = StripLeadingZeros(MoveTemp(Left));
			Right = StripLeadingZeros(MoveTemp(Right));
			while (Right != TEXT("0"))
			{
				FString Quotient;
				FString Remainder;
				DivideMagnitude(Left, Right, Quotient, Remainder);
				Left = MoveTemp(Right);
				Right = MoveTemp(Remainder);
			}
			return Left;
		}

		FSignedMagnitude NormalizeSigned(FSignedMagnitude Value)
		{
			Value.Magnitude = StripLeadingZeros(MoveTemp(Value.Magnitude));
			if (Value.Magnitude == TEXT("0"))
			{
				Value.bNegative = false;
			}
			return Value;
		}

		FSignedMagnitude AddSigned(
			const FSignedMagnitude& Left,
			const FSignedMagnitude& Right)
		{
			if (Left.bNegative == Right.bNegative)
			{
				return NormalizeSigned({
					Left.bNegative,
					AddMagnitude(Left.Magnitude, Right.Magnitude),
				});
			}
			const int32 Comparison = CompareMagnitude(Left.Magnitude, Right.Magnitude);
			if (Comparison == 0)
			{
				return {};
			}
			if (Comparison > 0)
			{
				return NormalizeSigned({
					Left.bNegative,
					SubtractMagnitude(Left.Magnitude, Right.Magnitude),
				});
			}
			return NormalizeSigned({
				Right.bNegative,
				SubtractMagnitude(Right.Magnitude, Left.Magnitude),
			});
		}

		FSignedMagnitude NegateSigned(FSignedMagnitude Value)
		{
			if (Value.Magnitude != TEXT("0"))
			{
				Value.bNegative = !Value.bNegative;
			}
			return Value;
		}

		FSignedMagnitude MultiplySigned(
			const FSignedMagnitude& Left,
			const FSignedMagnitude& Right)
		{
			return NormalizeSigned({
				Left.bNegative != Right.bNegative,
				MultiplyMagnitude(Left.Magnitude, Right.Magnitude),
			});
		}

		void NormalizeRational(FExactRational& Value)
		{
			Value.Numerator = NormalizeSigned(MoveTemp(Value.Numerator));
			Value.Denominator = StripLeadingZeros(MoveTemp(Value.Denominator));
			check(Value.Denominator != TEXT("0"));
			if (Value.Numerator.Magnitude == TEXT("0"))
			{
				Value.Denominator = TEXT("1");
				return;
			}
			const FString Gcd = GreatestCommonDivisor(
				Value.Numerator.Magnitude,
				Value.Denominator);
			if (Gcd != TEXT("1"))
			{
				FString Quotient;
				FString Remainder;
				DivideMagnitude(Value.Numerator.Magnitude, Gcd, Quotient, Remainder);
				check(Remainder == TEXT("0"));
				Value.Numerator.Magnitude = MoveTemp(Quotient);
				DivideMagnitude(Value.Denominator, Gcd, Quotient, Remainder);
				check(Remainder == TEXT("0"));
				Value.Denominator = MoveTemp(Quotient);
			}
		}

		FExactRational AddRational(
			const FExactRational& Left,
			const FExactRational& Right)
		{
			FExactRational Result;
			Result.Numerator = AddSigned(
				MultiplySigned(Left.Numerator, {false, Right.Denominator}),
				MultiplySigned(Right.Numerator, {false, Left.Denominator}));
			Result.Denominator = MultiplyMagnitude(Left.Denominator, Right.Denominator);
			NormalizeRational(Result);
			return Result;
		}

		FExactRational SubtractRational(
			const FExactRational& Left,
			const FExactRational& Right)
		{
			FExactRational Negated = Right;
			Negated.Numerator = NegateSigned(MoveTemp(Negated.Numerator));
			return AddRational(Left, Negated);
		}

		FExactRational MultiplyRational(
			const FExactRational& Left,
			const FExactRational& Right)
		{
			FExactRational Result;
			Result.Numerator = MultiplySigned(Left.Numerator, Right.Numerator);
			Result.Denominator = MultiplyMagnitude(Left.Denominator, Right.Denominator);
			NormalizeRational(Result);
			return Result;
		}

		bool DivideRational(
			const FExactRational& Dividend,
			const FExactRational& Divisor,
			FExactRational& OutResult)
		{
			if (Divisor.Numerator.Magnitude == TEXT("0"))
			{
				return false;
			}
			OutResult.Numerator = MultiplySigned(
				Dividend.Numerator,
				{Divisor.Numerator.bNegative, Divisor.Denominator});
			OutResult.Denominator = MultiplyMagnitude(
				Dividend.Denominator,
				Divisor.Numerator.Magnitude);
			NormalizeRational(OutResult);
			return true;
		}

		bool RationalsEqual(
			const FExactRational& Left,
			const FExactRational& Right)
		{
			return Left.Numerator.bNegative == Right.Numerator.bNegative
				&& MultiplyMagnitude(Left.Numerator.Magnitude, Right.Denominator)
					== MultiplyMagnitude(Right.Numerator.Magnitude, Left.Denominator);
		}

		bool ParseRational(const FString& Lexeme, FExactRational& OutValue)
		{
			FExactNumber Number;
			if (!ParseExactNumber(Lexeme, Number))
			{
				return false;
			}
			OutValue.Numerator = {Number.bNegative, Number.Digits};
			OutValue.Denominator = TEXT("1");
			if (Number.PowerOfTen >= 0)
			{
				for (int32 Index = 0; Index < Number.PowerOfTen; ++Index)
				{
					OutValue.Numerator.Magnitude.AppendChar(TEXT('0'));
				}
			}
			else
			{
				for (int32 Index = 0; Index < -Number.PowerOfTen; ++Index)
				{
					OutValue.Denominator.AppendChar(TEXT('0'));
				}
			}
			NormalizeRational(OutValue);
			return true;
		}

		int64 SaturatingAdd(int64 Left, int64 Right)
		{
			if (Right > 0 && Left > MAX_int64 - Right)
			{
				return MAX_int64;
			}
			return Left + Right;
		}

		int64 SaturatingMultiply(int64 Left, int64 Right)
		{
			if (Left <= 0 || Right <= 0)
			{
				return 0;
			}
			if (Left > MAX_int64 / Right)
			{
				return MAX_int64;
			}
			return Left * Right;
		}

		bool AccumulateRationalExpansionCost(
			const FString& Lexeme,
			int64& InOutExpandedDigits,
			int64& InOutMaxExpandedDigits)
		{
			FExactNumber Number;
			if (!ParseExactNumber(Lexeme, Number))
			{
				return false;
			}
			const int64 PositivePower =
				Number.PowerOfTen > 0 ? Number.PowerOfTen : 0;
			const int64 NegativePower =
				Number.PowerOfTen < 0 ? -static_cast<int64>(Number.PowerOfTen) : 0;
			const int64 NumeratorDigits = SaturatingAdd(
				Number.Digits.Len(),
				PositivePower);
			const int64 DenominatorDigits = SaturatingAdd(1, NegativePower);
			const int64 ExpandedDigits = SaturatingAdd(
				NumeratorDigits,
				DenominatorDigits);
			InOutExpandedDigits = SaturatingAdd(
				InOutExpandedDigits,
				ExpandedDigits);
			InOutMaxExpandedDigits =
				FMath::Max(InOutMaxExpandedDigits, ExpandedDigits);
			return true;
		}

		bool AccumulateRationalExpansionCost(
			const TArray<FString>& Lexemes,
			int64& InOutExpandedDigits,
			int64& InOutMaxExpandedDigits)
		{
			for (const FString& Lexeme : Lexemes)
			{
				if (!AccumulateRationalExpansionCost(
					Lexeme,
					InOutExpandedDigits,
					InOutMaxExpandedDigits))
				{
					return false;
				}
			}
			return true;
		}

		bool AccumulateRationalExpansionCost(
			const TArray<TArray<FString>>& Lexemes,
			int64& InOutExpandedDigits,
			int64& InOutMaxExpandedDigits)
		{
			for (const TArray<FString>& Row : Lexemes)
			{
				if (!AccumulateRationalExpansionCost(
					Row,
					InOutExpandedDigits,
					InOutMaxExpandedDigits))
				{
					return false;
				}
			}
			return true;
		}

		bool RationalToDecimalLexeme(
			FExactRational Value,
			FString& OutLexeme)
		{
			NormalizeRational(Value);
			FString Denominator = Value.Denominator;
			int32 Twos = 0;
			int32 Fives = 0;
			while (Denominator != TEXT("1"))
			{
				FString Quotient;
				int32 Remainder = 0;
				DivideMagnitudeBySmall(Denominator, 2, Quotient, Remainder);
				if (Remainder == 0)
				{
					Denominator = MoveTemp(Quotient);
					++Twos;
					continue;
				}
				DivideMagnitudeBySmall(Denominator, 5, Quotient, Remainder);
				if (Remainder == 0)
				{
					Denominator = MoveTemp(Quotient);
					++Fives;
					continue;
				}
				return false;
			}
			const int32 Scale = FMath::Max(Twos, Fives);
			FString Digits = Value.Numerator.Magnitude;
			for (int32 Index = Twos; Index < Scale; ++Index)
			{
				Digits = MultiplyMagnitudeBySmall(Digits, 2);
			}
			for (int32 Index = Fives; Index < Scale; ++Index)
			{
				Digits = MultiplyMagnitudeBySmall(Digits, 5);
			}
			if (Scale > 0)
			{
				while (Digits.Len() <= Scale)
				{
					Digits = TEXT("0") + Digits;
				}
				Digits.InsertAt(Digits.Len() - Scale, TEXT('.'));
				while (Digits.EndsWith(TEXT("0")))
				{
					Digits.LeftChopInline(1, EAllowShrinking::No);
				}
				if (Digits.EndsWith(TEXT(".")))
				{
					Digits.LeftChopInline(1, EAllowShrinking::No);
				}
			}
			if (Value.Numerator.bNegative && Digits != TEXT("0"))
			{
				Digits = TEXT("-") + Digits;
			}
			OutLexeme = MoveTemp(Digits);
			return true;
		}

		FExactRational ZeroRational()
		{
			return {};
		}

		FExactRational OneRational()
		{
			FExactRational Result;
			Result.Numerator.Magnitude = TEXT("1");
			return Result;
		}

		bool ParseVector(
			const TArray<FString>& Values,
			FExactVector& OutVector)
		{
			OutVector.Reset();
			OutVector.Reserve(Values.Num());
			for (const FString& Value : Values)
			{
				FExactRational Parsed;
				if (!ParseRational(Value, Parsed))
				{
					return false;
				}
				OutVector.Add(MoveTemp(Parsed));
			}
			return true;
		}

		bool ParseMatrix(
			const TArray<TArray<FString>>& Values,
			FExactMatrix& OutMatrix)
		{
			OutMatrix.Reset();
			OutMatrix.Reserve(Values.Num());
			for (const TArray<FString>& Row : Values)
			{
				FExactVector Parsed;
				if (!ParseVector(Row, Parsed))
				{
					return false;
				}
				OutMatrix.Add(MoveTemp(Parsed));
			}
			return true;
		}

		FExactMatrix IdentityMatrix(int32 Size)
		{
			FExactMatrix Result;
			Result.SetNum(Size);
			for (int32 Row = 0; Row < Size; ++Row)
			{
				Result[Row].SetNum(Size);
				for (int32 Column = 0; Column < Size; ++Column)
				{
					Result[Row][Column] =
						Row == Column ? OneRational() : ZeroRational();
				}
			}
			return Result;
		}

		FExactMatrix Transpose(const FExactMatrix& Matrix)
		{
			if (Matrix.IsEmpty())
			{
				return {};
			}
			FExactMatrix Result;
			Result.SetNum(Matrix[0].Num());
			for (int32 Column = 0; Column < Matrix[0].Num(); ++Column)
			{
				Result[Column].SetNum(Matrix.Num());
				for (int32 Row = 0; Row < Matrix.Num(); ++Row)
				{
					Result[Column][Row] = Matrix[Row][Column];
				}
			}
			return Result;
		}

		bool MultiplyMatrices(
			const FExactMatrix& Left,
			const FExactMatrix& Right,
			FExactMatrix& OutResult)
		{
			if (Left.IsEmpty() || Right.IsEmpty() || Left[0].Num() != Right.Num())
			{
				return false;
			}
			const int32 RightColumns = Right[0].Num();
			for (const FExactVector& Row : Left)
			{
				if (Row.Num() != Right.Num())
				{
					return false;
				}
			}
			for (const FExactVector& Row : Right)
			{
				if (Row.Num() != RightColumns)
				{
					return false;
				}
			}
			OutResult.SetNum(Left.Num());
			for (int32 Row = 0; Row < Left.Num(); ++Row)
			{
				OutResult[Row].SetNum(RightColumns);
				for (int32 Column = 0; Column < RightColumns; ++Column)
				{
					FExactRational Sum = ZeroRational();
					for (int32 Inner = 0; Inner < Right.Num(); ++Inner)
					{
						Sum = AddRational(
							Sum,
							MultiplyRational(Left[Row][Inner], Right[Inner][Column]));
					}
					OutResult[Row][Column] = MoveTemp(Sum);
				}
			}
			return true;
		}

		bool MultiplyMatrixVector(
			const FExactMatrix& Matrix,
			const FExactVector& Vector,
			FExactVector& OutResult)
		{
			if (Matrix.IsEmpty())
			{
				return false;
			}
			OutResult.SetNum(Matrix.Num());
			for (int32 Row = 0; Row < Matrix.Num(); ++Row)
			{
				if (Matrix[Row].Num() != Vector.Num())
				{
					return false;
				}
				FExactRational Sum = ZeroRational();
				for (int32 Column = 0; Column < Vector.Num(); ++Column)
				{
					Sum = AddRational(
						Sum,
						MultiplyRational(Matrix[Row][Column], Vector[Column]));
				}
				OutResult[Row] = MoveTemp(Sum);
			}
			return true;
		}

		TSharedPtr<FJsonValue> RationalValue(const FExactRational& Value) {
			auto Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("numerator"), (Value.Numerator.bNegative ? TEXT("-") : TEXT("")) + Value.Numerator.Magnitude);
			Result->SetStringField(TEXT("denominator"), Value.Denominator);
			return MakeShared<FJsonValueObject>(Result);
		}
		TSharedPtr<FJsonValue> TemporalObservation(const FUeAgentTemporalPoint& Time) {
			auto Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("tick"), Time.GetTick()); Result->SetStringField(TEXT("rate_numerator"), Time.GetRateNumerator());
			Result->SetStringField(TEXT("rate_denominator"), Time.GetRateDenominator()); Result->SetStringField(TEXT("state"), Time.GetState());
			return MakeShared<FJsonValueObject>(Result);
		}
		bool InvertMatrix(
			const FExactMatrix& Matrix,
			FExactMatrix& OutInverse, const FString& Path, FUeAgentStateSemanticError& Error)
		{
			const int32 Size = Matrix.Num();
			if (Size == 0)
			{
				return Fail(Error, TEXT("spatial_matrix_invalid"), TEXT("An empty matrix cannot be inverted."), Path,
					MakeShared<FJsonValueString>(TEXT("non-empty square matrix")), MakeShared<FJsonValueNumber>(Size));
			}
			FExactMatrix Augmented;
			Augmented.SetNum(Size);
			const FExactMatrix Identity = IdentityMatrix(Size);
			for (int32 Row = 0; Row < Size; ++Row)
			{
				if (Matrix[Row].Num() != Size)
				{
					return Fail(Error, TEXT("spatial_matrix_invalid"), TEXT("Matrix inversion requires the same number of rows and columns."), Path + TEXT("/") + FString::FromInt(Row),
						MakeShared<FJsonValueNumber>(Size), MakeShared<FJsonValueNumber>(Matrix[Row].Num()));
				}
				Augmented[Row] = Matrix[Row];
				Augmented[Row].Append(Identity[Row]);
			}

			for (int32 Column = 0; Column < Size; ++Column)
			{
				int32 Pivot = INDEX_NONE;
				for (int32 Row = Column; Row < Size; ++Row)
				{
					if (Augmented[Row][Column].Numerator.Magnitude != TEXT("0"))
					{
						Pivot = Row;
						break;
					}
				}
				if (Pivot == INDEX_NONE)
				{
					return Fail(Error, TEXT("spatial_matrix_singular"), FString::Printf(TEXT("The matrix is singular: elimination found no nonzero pivot in column %d."), Column), Path,
						MakeShared<FJsonValueString>(TEXT("nonzero pivot")), MakeShared<FJsonValueString>(TEXT("0")));
				}
				if (Pivot != Column)
				{
					Swap(Augmented[Pivot], Augmented[Column]);
				}
				const FExactRational PivotValue = Augmented[Column][Column];
				for (FExactRational& Value : Augmented[Column])
				{
					FExactRational Divided;
					if (!DivideRational(Value, PivotValue, Divided))
					{
						return Fail(Error, TEXT("spatial_matrix_pivot_invalid"), TEXT("The selected matrix pivot could not be used as an exact nonzero divisor."), Path,
							MakeShared<FJsonValueString>(TEXT("nonzero pivot")), RationalValue(PivotValue));
					}
					Value = MoveTemp(Divided);
				}
				for (int32 Row = 0; Row < Size; ++Row)
				{
					if (Row == Column)
					{
						continue;
					}
					const FExactRational Factor = Augmented[Row][Column];
					if (Factor.Numerator.Magnitude == TEXT("0"))
					{
						continue;
					}
					for (int32 Item = 0; Item < Size * 2; ++Item)
					{
						Augmented[Row][Item] = SubtractRational(
							Augmented[Row][Item],
							MultiplyRational(Factor, Augmented[Column][Item]));
					}
				}
			}

			OutInverse.SetNum(Size);
			for (int32 Row = 0; Row < Size; ++Row)
			{
				OutInverse[Row].Append(&Augmented[Row][Size], Size);
			}
			return true;
		}

		bool MatchVector(const FExactVector& Expected, const FExactVector& Actual,
			const FString& Path, FUeAgentStateSemanticError& Error) {
			if (Expected.Num() != Actual.Num()) return Fail(Error, TEXT("spatial_conversion_frame_contract_mismatch"),
				TEXT("The conversion vector length differs from the frame-derived vector."), Path,
				MakeShared<FJsonValueNumber>(Expected.Num()), MakeShared<FJsonValueNumber>(Actual.Num()));
			for (int32 Index = 0; Index < Expected.Num(); ++Index)
				if (!RationalsEqual(Expected[Index], Actual[Index])) return Fail(Error, TEXT("spatial_conversion_frame_contract_mismatch"),
					TEXT("The conversion coefficient differs from the value derived from the source and target frames."), Path + TEXT("/") + FString::FromInt(Index),
					RationalValue(Expected[Index]), RationalValue(Actual[Index]));
			return true;
		}
		bool MatchMatrix(const FExactMatrix& Expected, const FExactMatrix& Actual,
			const FString& Path, FUeAgentStateSemanticError& Error) {
			if (Expected.Num() != Actual.Num()) return Fail(Error, TEXT("spatial_conversion_frame_contract_mismatch"),
				TEXT("The conversion matrix row count differs from the frame-derived matrix."), Path,
				MakeShared<FJsonValueNumber>(Expected.Num()), MakeShared<FJsonValueNumber>(Actual.Num()));
			for (int32 Row = 0; Row < Expected.Num(); ++Row)
				if (!MatchVector(Expected[Row], Actual[Row], Path + TEXT("/") + FString::FromInt(Row), Error)) return false;
			return true;
		}
		bool VectorToLexemes(const FExactVector& Vector, TArray<FString>& OutLexemes,
			const FString& Path, FUeAgentStateSemanticError& Error) {
			OutLexemes.Reset();
			for (int32 Index = 0; Index < Vector.Num(); ++Index) {
				FString Lexeme;
				if (!RationalToDecimalLexeme(Vector[Index], Lexeme)) return Fail(Error, TEXT("spatial_result_non_terminating_decimal"),
					TEXT("The exact result has a non-terminating decimal expansion and cannot be stored as a finite decimal component."), Path + TEXT("/") + FString::FromInt(Index),
					MakeShared<FJsonValueString>(TEXT("terminating decimal")), RationalValue(Vector[Index]));
				OutLexemes.Add(MoveTemp(Lexeme));
			}
			return true;
		}
		bool MatrixToLexemes(const FExactMatrix& Matrix, TArray<FString>& OutLexemes,
			const FString& Path, FUeAgentStateSemanticError& Error) {
			OutLexemes.Reset();
			for (int32 Row = 0; Row < Matrix.Num(); ++Row) {
				for (int32 Column = 0; Column < Matrix[Row].Num(); ++Column) {
					FString Lexeme;
					if (!RationalToDecimalLexeme(Matrix[Row][Column], Lexeme)) return Fail(Error, TEXT("spatial_result_non_terminating_decimal"),
						TEXT("The exact transform component has a non-terminating decimal expansion."),
						Path + TEXT("/") + FString::FromInt(OutLexemes.Num()), MakeShared<FJsonValueString>(TEXT("terminating decimal")), RationalValue(Matrix[Row][Column]));
					OutLexemes.Add(MoveTemp(Lexeme));
				}
			}
			return true;
		}

		FUeAgentStateSemanticError SpatialValueFailure(const TCHAR* Code, const TCHAR* Reason,
			const FString& Path, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			auto Error = FUeAgentStateSemanticError::Failure(Code, Reason, TEXT("state_semantic"));
			Error.PropertyPath = Path;
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			return Error;
		}

		FUeAgentStateSemanticError ValidateExactLexemeVector(const TArray<FString>& Vector, const FString& Path)
		{
			for (int32 Index = 0; Index < Vector.Num(); ++Index)
			{
				FExactNumber Parsed;
				if (!ParseExactNumber(Vector[Index], Parsed))
				{
					return SpatialValueFailure(TEXT("spatial_numeric_value_invalid"),
						TEXT("The spatial coordinate is not a valid exact numeric value."),
						FString::Printf(TEXT("%s[%d]"), *Path, Index), MakeShared<FJsonValueString>(Vector[Index]),
						MakeShared<FJsonValueString>(TEXT("exact numeric value")));
				}
			}
			return {};
		}

		FUeAgentStateSemanticError ValidateSquareExactLexemeMatrix(
			const TArray<TArray<FString>>& Matrix, int32 Dimension, const FString& Path)
		{
			if (Matrix.Num() != Dimension)
				return SpatialValueFailure(TEXT("spatial_matrix_row_count_invalid"),
					TEXT("The spatial basis must contain one row per dimension."), Path,
					MakeShared<FJsonValueNumber>(Matrix.Num()), MakeShared<FJsonValueNumber>(Dimension));
			for (int32 RowIndex = 0; RowIndex < Matrix.Num(); ++RowIndex)
			{
				const auto& Row = Matrix[RowIndex];
				const FString RowPath = FString::Printf(TEXT("%s[%d]"), *Path, RowIndex);
				if (Row.Num() != Dimension)
					return SpatialValueFailure(TEXT("spatial_matrix_column_count_invalid"),
						TEXT("Each spatial basis row must contain one coordinate per dimension."), RowPath,
						MakeShared<FJsonValueNumber>(Row.Num()), MakeShared<FJsonValueNumber>(Dimension));
				auto Error = ValidateExactLexemeVector(Row, RowPath);
				if (!Error.IsEmpty()) return Error;
			}
			return {};
		}

		bool IsSquareExactLexemeMatrix(const TArray<TArray<FString>>& Matrix, int32 Dimension)
		{
			return ValidateSquareExactLexemeMatrix(Matrix, Dimension, TEXT("basis")).IsEmpty();
		}

		bool IsExactLexemeVector(const TArray<FString>& Vector)
		{
			return ValidateExactLexemeVector(Vector, TEXT("vector")).IsEmpty();
		}

		bool ExactLexemeArraysEqual(
			const TArray<FString>& Left,
			const TArray<FString>& Right)
		{
			if (Left.Num() != Right.Num())
			{
				return false;
			}
			for (int32 Index = 0; Index < Left.Num(); ++Index)
			{
				if (!ExactNumbersEqual(Left[Index], Right[Index]))
				{
					return false;
				}
			}
			return true;
		}

		bool ExactLexemeMatricesEqual(
			const TArray<TArray<FString>>& Left,
			const TArray<TArray<FString>>& Right)
		{
			if (Left.Num() != Right.Num())
			{
				return false;
			}
			for (int32 Row = 0; Row < Left.Num(); ++Row)
			{
				if (!ExactLexemeArraysEqual(Left[Row], Right[Row]))
				{
					return false;
				}
			}
			return true;
		}

		FString JsonPointerJoin(const FString& Base, const FString& Segment)
		{
			FString Escaped = Segment.Replace(TEXT("~"), TEXT("~0"));
			Escaped = Escaped.Replace(TEXT("/"), TEXT("~1"));
			if (Base.IsEmpty())
			{
				return TEXT("/") + Escaped;
			}
			return Base + TEXT("/") + Escaped;
		}

		FString JsonPointerJoin(const FString& Base, int32 Index)
		{
			return JsonPointerJoin(Base, FString::FromInt(Index));
		}

		bool ChargeComparison(
			FUeAgentResourceTracker& Resources,
			const FString& Path,
			int32 Items,
			FUeAgentStateSemanticError& OutError)
		{
			return Charge(
				Resources,
				{
					{EUeAgentResourceDimension::Nodes, 1},
					{EUeAgentResourceDimension::Items, Items},
					{EUeAgentResourceDimension::WorkUnits, 1 + Items},
					{EUeAgentResourceDimension::EstimatedMemoryBytes, 48 + Items * 16},
				},
				Path,
				OutError);
		}

		bool AddDifference(TArray<FUeAgentSemanticDifference>& Differences, EUeAgentDifferenceKind Kind,
			const FString& Path, FUeAgentCanonicalValuePtr Left, FUeAgentCanonicalValuePtr Right,
			FUeAgentResourceTracker& Resources, FUeAgentStateSemanticError& OutError)
		{
			FUeAgentCanonicalValuePtr PublicLeft, PublicRight;
			if (!TryCanonicalizeDifferenceEvidence(Left, Resources, PublicLeft, OutError, EDifferenceEvidenceAudience::Public)
				|| !TryCanonicalizeDifferenceEvidence(Right, Resources, PublicRight, OutError, EDifferenceEvidenceAudience::Public)) return false;
			Differences.Emplace(Kind, Path, MoveTemp(Left), MoveTemp(Right), MoveTemp(PublicLeft), MoveTemp(PublicRight));
			return true;
		}

		template <typename EvidenceType>
		bool TryAddTypedDifference(
			TArray<FUeAgentSemanticDifference>& Differences,
			EUeAgentDifferenceKind Kind,
			const FString& Path,
			const EvidenceType* Left,
			const EvidenceType* Right,
			FUeAgentResourceTracker& Resources,
			FUeAgentStateSemanticError& OutError)
		{
			FUeAgentCanonicalValuePtr LeftValue;
			if (Left != nullptr
				&& !TryCanonicalizeDifferenceEvidence(
					*Left,
					Resources,
					LeftValue,
					OutError))
			{
				return false;
			}
			FUeAgentCanonicalValuePtr RightValue;
			if (Right != nullptr
				&& !TryCanonicalizeDifferenceEvidence(
					*Right,
					Resources,
					RightValue,
					OutError))
			{
				return false;
			}
			FUeAgentCanonicalValuePtr PublicLeft, PublicRight;
			if (Left && !TryCanonicalizeDifferenceEvidence(*Left, Resources, PublicLeft, OutError,
				EDifferenceEvidenceAudience::Public)) return false;
			if (Right && !TryCanonicalizeDifferenceEvidence(*Right, Resources, PublicRight, OutError,
				EDifferenceEvidenceAudience::Public)) return false;
			Differences.Emplace(Kind, Path, MoveTemp(LeftValue), MoveTemp(RightValue),
				MoveTemp(PublicLeft), MoveTemp(PublicRight));
			return true;
		}

		bool TryCompareValuesInternal(
			const FUeAgentCanonicalValueRef& Left,
			const FUeAgentCanonicalValueRef& Right,
			const FString& Path,
			FUeAgentResourceTracker& Resources,
			TArray<FUeAgentSemanticDifference>& OutDifferences,
			FUeAgentStateSemanticError& OutError)
		{
			struct FTask
			{
				FUeAgentCanonicalValueRef Left;
				FUeAgentCanonicalValueRef Right;
				FString Path;
			};
			TArray<FTask> Tasks;
			Tasks.Add({Left, Right, Path});
			while (!Tasks.IsEmpty())
			{
				FTask Task = MoveTemp(Tasks.Last());
				Tasks.Pop(EAllowShrinking::No);
				const EUeAgentValueKind Kind = Task.Left->GetKind();
				const EUeAgentValueKind RightKind = Task.Right->GetKind();
				// JSON transports can represent the same number as Integer or
				// Decimal. Compare numeric values exactly below; declared property
				// types remain the responsibility of the state entry/type contract.
				const bool bBothNumeric =
					(Kind == EUeAgentValueKind::Integer || Kind == EUeAgentValueKind::Decimal)
					&& (RightKind == EUeAgentValueKind::Integer || RightKind == EUeAgentValueKind::Decimal);
				if (Kind != RightKind && !bBothNumeric)
				{
					if (!ChargeComparison(Resources, Task.Path, 0, OutError))
					{
						return false;
					}
					if (!AddDifference(
						OutDifferences,
						EUeAgentDifferenceKind::TypeMismatch,
						Task.Path,
						FUeAgentCanonicalValuePtr(Task.Left),
						FUeAgentCanonicalValuePtr(Task.Right), Resources, OutError)) return false;
					continue;
				}

				if (Kind == EUeAgentValueKind::Array)
				{
					const TArray<FUeAgentCanonicalValueRef>& LeftItems =
						Task.Left->GetArrayItems();
					const TArray<FUeAgentCanonicalValueRef>& RightItems =
						Task.Right->GetArrayItems();
					if (!ChargeComparison(
						Resources,
						Task.Path,
						FMath::Max(LeftItems.Num(), RightItems.Num()),
						OutError))
					{
						return false;
					}
					const int32 Common = FMath::Min(LeftItems.Num(), RightItems.Num());
					for (int32 Index = Common - 1; Index >= 0; --Index)
					{
						Tasks.Add({
							LeftItems[Index],
							RightItems[Index],
							JsonPointerJoin(Task.Path, Index),
						});
					}
					for (int32 Index = Common; Index < LeftItems.Num(); ++Index)
					{
						if (!AddDifference(
							OutDifferences,
							EUeAgentDifferenceKind::MissingRight,
							JsonPointerJoin(Task.Path, Index),
							FUeAgentCanonicalValuePtr(LeftItems[Index]), nullptr, Resources, OutError)) return false;
					}
					for (int32 Index = Common; Index < RightItems.Num(); ++Index)
					{
						if (!AddDifference(
							OutDifferences,
							EUeAgentDifferenceKind::MissingLeft,
							JsonPointerJoin(Task.Path, Index),
							FUeAgentCanonicalValuePtr(),
							FUeAgentCanonicalValuePtr(RightItems[Index]), Resources, OutError)) return false;
					}
					continue;
				}
				if (Kind == EUeAgentValueKind::Object)
				{
					const auto& LeftItems = Task.Left->GetObjectItems();
					const auto& RightItems = Task.Right->GetObjectItems();
					if (!ChargeComparison(
						Resources,
						Task.Path,
						FMath::Max(LeftItems.Num(), RightItems.Num()),
						OutError))
					{
						return false;
					}
					int32 LeftIndex = 0;
					int32 RightIndex = 0;
					TArray<FTask> ChildTasks;
					while (LeftIndex < LeftItems.Num() || RightIndex < RightItems.Num())
					{
						if (LeftIndex >= LeftItems.Num())
						{
							if (!AddDifference(
								OutDifferences,
								EUeAgentDifferenceKind::MissingLeft,
								JsonPointerJoin(Task.Path, RightItems[RightIndex].Key),
								FUeAgentCanonicalValuePtr(),
								FUeAgentCanonicalValuePtr(
									RightItems[RightIndex].Value), Resources, OutError)) return false;
							++RightIndex;
							continue;
						}
						if (RightIndex >= RightItems.Num())
						{
							if (!AddDifference(
								OutDifferences,
								EUeAgentDifferenceKind::MissingRight,
								JsonPointerJoin(Task.Path, LeftItems[LeftIndex].Key),
								FUeAgentCanonicalValuePtr(
								LeftItems[LeftIndex].Value), nullptr, Resources, OutError)) return false;
							++LeftIndex;
							continue;
						}
						const int32 Comparison = LeftItems[LeftIndex].Key.Compare(
							RightItems[RightIndex].Key,
							ESearchCase::CaseSensitive);
						if (Comparison < 0)
						{
							if (!AddDifference(
								OutDifferences,
								EUeAgentDifferenceKind::MissingRight,
								JsonPointerJoin(Task.Path, LeftItems[LeftIndex].Key),
								FUeAgentCanonicalValuePtr(
								LeftItems[LeftIndex].Value), nullptr, Resources, OutError)) return false;
							++LeftIndex;
						}
						else if (Comparison > 0)
						{
							if (!AddDifference(
								OutDifferences,
								EUeAgentDifferenceKind::MissingLeft,
								JsonPointerJoin(Task.Path, RightItems[RightIndex].Key),
								FUeAgentCanonicalValuePtr(),
								FUeAgentCanonicalValuePtr(
									RightItems[RightIndex].Value), Resources, OutError)) return false;
							++RightIndex;
						}
						else
						{
							ChildTasks.Add({
								LeftItems[LeftIndex].Value,
								RightItems[RightIndex].Value,
								JsonPointerJoin(Task.Path, LeftItems[LeftIndex].Key),
							});
							++LeftIndex;
							++RightIndex;
						}
					}
					for (int32 Index = ChildTasks.Num() - 1; Index >= 0; --Index)
					{
						Tasks.Add(MoveTemp(ChildTasks[Index]));
					}
					continue;
				}

				if (!ChargeComparison(Resources, Task.Path, 0, OutError))
				{
					return false;
				}
				switch (Kind)
				{
				case EUeAgentValueKind::Null:
					break;
				case EUeAgentValueKind::Boolean:
					if (Task.Left->GetBoolean() != Task.Right->GetBoolean())
					{
						if (!AddDifference(
							OutDifferences,
							EUeAgentDifferenceKind::ValueMismatch,
							Task.Path,
							FUeAgentCanonicalValuePtr(Task.Left),
							FUeAgentCanonicalValuePtr(Task.Right), Resources, OutError)) return false;
					}
					break;
				case EUeAgentValueKind::String:
					if (!Task.Left->GetText().Equals(Task.Right->GetText(), ESearchCase::CaseSensitive))
					{
						if (!AddDifference(
							OutDifferences,
							EUeAgentDifferenceKind::ValueMismatch,
							Task.Path,
							FUeAgentCanonicalValuePtr(Task.Left),
							FUeAgentCanonicalValuePtr(Task.Right), Resources, OutError)) return false;
					}
					break;
				case EUeAgentValueKind::Integer:
				case EUeAgentValueKind::Decimal:
					if (!ExactNumbersEqual(Task.Left->GetLexeme(), Task.Right->GetLexeme()))
					{
						if (!AddDifference(
							OutDifferences,
							EUeAgentDifferenceKind::ValueMismatch,
							Task.Path,
							FUeAgentCanonicalValuePtr(Task.Left),
							FUeAgentCanonicalValuePtr(Task.Right), Resources, OutError)) return false;
					}
					break;
				case EUeAgentValueKind::Temporal:
				{
					const FUeAgentTemporalPoint& LeftTime = *Task.Left->GetTemporal();
					const FUeAgentTemporalPoint& RightTime = *Task.Right->GetTemporal();
					if (LeftTime.GetClockId() != RightTime.GetClockId())
					{
						if (!TryAddTypedDifference(
							OutDifferences,
							EUeAgentDifferenceKind::TimeDomainMismatch,
							JsonPointerJoin(Task.Path, TEXT("time")),
							&LeftTime,
							&RightTime,
							Resources,
							OutError))
						{
							return false;
						}
					}
					else if (LeftTime.GetState() != RightTime.GetState())
					{
						if (!TryAddTypedDifference(
							OutDifferences,
							EUeAgentDifferenceKind::TimeStateMismatch,
							JsonPointerJoin(Task.Path, TEXT("time")),
							&LeftTime,
							&RightTime,
							Resources,
							OutError))
						{
							return false;
						}
					}
					else if (!AreTemporalPointsEquivalent(LeftTime, RightTime))
					{
						if (!TryAddTypedDifference(
							OutDifferences,
							EUeAgentDifferenceKind::ValueMismatch,
							JsonPointerJoin(Task.Path, TEXT("time")),
							&LeftTime,
							&RightTime,
							Resources,
							OutError))
						{
							return false;
						}
					}
					break;
				}
				case EUeAgentValueKind::Spatial:
				{
					const FUeAgentSpatialValue& LeftSpatial = *Task.Left->GetSpatial();
					const FUeAgentSpatialValue& RightSpatial = *Task.Right->GetSpatial();
					if (LeftSpatial.GetSubject() != RightSpatial.GetSubject())
					{
						if (!TryAddTypedDifference(
							OutDifferences,
							EUeAgentDifferenceKind::SubjectMismatch,
							JsonPointerJoin(Task.Path, TEXT("subject")),
							&LeftSpatial,
							&RightSpatial,
							Resources,
							OutError))
						{
							return false;
						}
					}
					else if (LeftSpatial.GetQuantityKind() != RightSpatial.GetQuantityKind())
					{
						if (!TryAddTypedDifference(
							OutDifferences,
							EUeAgentDifferenceKind::SpatialQuantityMismatch,
							JsonPointerJoin(Task.Path, TEXT("quantity_kind")),
							&LeftSpatial,
							&RightSpatial,
							Resources,
							OutError))
						{
							return false;
						}
					}
					else if (!LeftSpatial.GetFrame().SemanticallyEquals(RightSpatial.GetFrame()))
					{
						if (!TryAddTypedDifference(
							OutDifferences,
							EUeAgentDifferenceKind::FrameMismatch,
							JsonPointerJoin(Task.Path, TEXT("frame")),
							&LeftSpatial,
							&RightSpatial,
							Resources,
							OutError))
						{
							return false;
						}
					}
					else if (LeftSpatial.GetObservedAt().GetClockId()
						!= RightSpatial.GetObservedAt().GetClockId())
					{
						if (!TryAddTypedDifference(
							OutDifferences,
							EUeAgentDifferenceKind::TimeDomainMismatch,
							JsonPointerJoin(Task.Path, TEXT("time")),
							&LeftSpatial,
							&RightSpatial,
							Resources,
							OutError))
						{
							return false;
						}
					}
					else if (LeftSpatial.GetObservedAt().GetState()
						!= RightSpatial.GetObservedAt().GetState())
					{
						if (!TryAddTypedDifference(
							OutDifferences,
							EUeAgentDifferenceKind::TimeStateMismatch,
							JsonPointerJoin(Task.Path, TEXT("time")),
							&LeftSpatial,
							&RightSpatial,
							Resources,
							OutError))
						{
							return false;
						}
					}
					else if (!LeftSpatial.SemanticallyEquals(RightSpatial))
					{
						if (!TryAddTypedDifference(
							OutDifferences,
							EUeAgentDifferenceKind::ValueMismatch,
							Task.Path,
							&LeftSpatial,
							&RightSpatial,
							Resources,
							OutError))
						{
							return false;
						}
					}
					break;
				}
				default:
					break;
				}
			}
			return true;
		}
	}

	FUeAgentTemporalPoint::FUeAgentTemporalPoint(
		FString InClockId,
		FString InTick,
		FString InRateNumerator,
		FString InRateDenominator,
		FString InState)
		: ClockId(MoveTemp(InClockId))
		, Tick(MoveTemp(InTick))
		, RateNumerator(MoveTemp(InRateNumerator))
		, RateDenominator(MoveTemp(InRateDenominator))
		, State(MoveTemp(InState))
	{
		auto Reject = [&](const TCHAR* Field, const TCHAR* Reason, const FString& Value, bool bInternal)
		{
			ValidationDiagnostic = FUeAgentStateSemanticError::Failure(TEXT("temporal_point_invalid"), Reason, TEXT("state_semantic"));
			ValidationDiagnostic.PropertyPath = Field;
			if (bInternal) ValidationDiagnostic.ActualEvidence = MakeShared<FJsonValueString>(Value);
			else ValidationDiagnostic.ActualValue = MakeShared<FJsonValueString>(Value);
		};
		FExactNumber Numerator;
		FExactNumber Denominator;
		if (!IsNonEmpty(ClockId)) Reject(TEXT("clock_id"), TEXT("The temporal point requires a non-empty clock identifier."), ClockId, true);
		else if (!IsCanonicalIntegerLexeme(Tick)) Reject(TEXT("tick"), TEXT("The tick must be a canonical integer string."), Tick, false);
		else if (!IsCanonicalIntegerLexeme(RateNumerator)) Reject(TEXT("rate_numerator"), TEXT("The rate numerator must be a canonical integer string."), RateNumerator, false);
		else if (!IsCanonicalIntegerLexeme(RateDenominator)) Reject(TEXT("rate_denominator"), TEXT("The rate denominator must be a canonical integer string."), RateDenominator, false);
		else if (!ParseExactNumber(RateNumerator, Numerator) || Numerator.bNegative || Numerator.Digits == TEXT("0")) Reject(TEXT("rate_numerator"), TEXT("The rate numerator must be a positive integer."), RateNumerator, false);
		else if (!ParseExactNumber(RateDenominator, Denominator) || Denominator.bNegative || Denominator.Digits == TEXT("0")) Reject(TEXT("rate_denominator"), TEXT("The rate denominator must be a positive integer."), RateDenominator, false);
		else if (!IsNonEmpty(State)) Reject(TEXT("state"), TEXT("The temporal point requires a non-empty capture state."), State, false);
	}

	const FString& FUeAgentTemporalPoint::GetClockId() const { return ClockId; }
	const FString& FUeAgentTemporalPoint::GetTick() const { return Tick; }
	const FString& FUeAgentTemporalPoint::GetRateNumerator() const { return RateNumerator; }
	const FString& FUeAgentTemporalPoint::GetRateDenominator() const { return RateDenominator; }
	const FString& FUeAgentTemporalPoint::GetState() const { return State; }
	bool FUeAgentTemporalPoint::IsValid() const { return ValidationDiagnostic.IsEmpty(); }
	const FUeAgentStateSemanticError& FUeAgentTemporalPoint::GetValidationDiagnostic() const { return ValidationDiagnostic; }

	bool FUeAgentTemporalPoint::operator==(const FUeAgentTemporalPoint& Other) const
	{
		return ClockId == Other.ClockId
			&& Tick == Other.Tick
			&& RateNumerator == Other.RateNumerator
			&& RateDenominator == Other.RateDenominator
			&& State == Other.State;
	}

	FUeAgentSpatialFrame::FUeAgentSpatialFrame(
		FString InFrameId,
		FString InBasisFrameId,
		int32 InDimension,
		TArray<TArray<FString>> InBasis,
		TArray<FString> InOrigin,
		FString InUnit,
		FString InBasisEvidence,
		FUeAgentTemporalPoint InObservedAt)
		: FrameId(MoveTemp(InFrameId))
		, BasisFrameId(MoveTemp(InBasisFrameId))
		, Dimension(InDimension)
		, Basis(MoveTemp(InBasis))
		, Origin(MoveTemp(InOrigin))
		, Unit(MoveTemp(InUnit))
		, BasisEvidence(MoveTemp(InBasisEvidence))
		, ObservedAt(MoveTemp(InObservedAt))
	{
		const auto MissingText = [](const TCHAR* Reason, const TCHAR* Path, const FString& Value)
		{
			return SpatialValueFailure(TEXT("spatial_frame_invalid"), Reason, Path,
				MakeShared<FJsonValueString>(Value), MakeShared<FJsonValueString>(TEXT("non-empty string")));
		};
		if (!IsNonEmpty(FrameId))
			ValidationDiagnostic = MissingText(TEXT("The spatial frame requires a non-empty frame identifier."), TEXT("frame_id"), FrameId);
		else if (!IsNonEmpty(BasisFrameId))
			ValidationDiagnostic = MissingText(TEXT("The spatial frame requires a non-empty basis frame identifier."), TEXT("basis_frame_id"), BasisFrameId);
		else if (Dimension != 2 && Dimension != 3)
			ValidationDiagnostic = SpatialValueFailure(TEXT("spatial_frame_invalid"),
				TEXT("The spatial frame dimension must be 2 or 3."), TEXT("dimension"),
				MakeShared<FJsonValueNumber>(Dimension), MakeShared<FJsonValueString>(TEXT("2 or 3")));
		if (!ValidationDiagnostic.IsEmpty()) return;
		ValidationDiagnostic = ValidateSquareExactLexemeMatrix(Basis, Dimension, TEXT("basis"));
		if (!ValidationDiagnostic.IsEmpty()) return;
		if (Origin.Num() != Dimension)
		{
			ValidationDiagnostic = SpatialValueFailure(TEXT("spatial_frame_invalid"),
				TEXT("The spatial frame origin must contain one coordinate per dimension."), TEXT("origin"),
				MakeShared<FJsonValueNumber>(Origin.Num()), MakeShared<FJsonValueNumber>(Dimension));
			return;
		}
		ValidationDiagnostic = ValidateExactLexemeVector(Origin, TEXT("origin"));
		if (!ValidationDiagnostic.IsEmpty()) return;
		if (!IsNonEmpty(Unit))
			ValidationDiagnostic = MissingText(TEXT("The spatial frame requires a non-empty unit."), TEXT("unit"), Unit);
		else if (!IsNonEmpty(BasisEvidence))
		{
			ValidationDiagnostic = MissingText(TEXT("The spatial frame requires basis evidence."), TEXT("basis_evidence"), BasisEvidence);
			ValidationDiagnostic.ActualEvidence = MoveTemp(ValidationDiagnostic.ActualValue);
		}
		else if (!ObservedAt.IsValid())
		{
			ValidationDiagnostic = ObservedAt.GetValidationDiagnostic();
			ValidationDiagnostic.PropertyPath = TEXT("observed_at.") + ValidationDiagnostic.PropertyPath;
		}
	}

	const FString& FUeAgentSpatialFrame::GetFrameId() const { return FrameId; }
	const FString& FUeAgentSpatialFrame::GetBasisFrameId() const { return BasisFrameId; }
	int32 FUeAgentSpatialFrame::GetDimension() const { return Dimension; }
	const TArray<TArray<FString>>& FUeAgentSpatialFrame::GetBasis() const { return Basis; }
	const TArray<FString>& FUeAgentSpatialFrame::GetOrigin() const { return Origin; }
	const FString& FUeAgentSpatialFrame::GetUnit() const { return Unit; }
	const FString& FUeAgentSpatialFrame::GetBasisEvidence() const { return BasisEvidence; }
	const FUeAgentTemporalPoint& FUeAgentSpatialFrame::GetObservedAt() const { return ObservedAt; }
	bool FUeAgentSpatialFrame::IsValid() const { return ValidationDiagnostic.IsEmpty(); }

	bool FUeAgentSpatialFrame::SemanticallyEquals(
		const FUeAgentSpatialFrame& Other) const
	{
		return IsValid()
			&& Other.IsValid()
			&& FrameId == Other.FrameId
			&& BasisFrameId == Other.BasisFrameId
			&& Dimension == Other.Dimension
			&& Unit == Other.Unit
			&& ExactLexemeMatricesEqual(Basis, Other.Basis)
			&& ExactLexemeArraysEqual(Origin, Other.Origin);
	}

	FUeAgentSpatialConversion::FUeAgentSpatialConversion(
		FString InConversionId,
		FString InFromFrame,
		FString InToFrame,
		int32 InSourceDimension,
		int32 InTargetDimension,
		TArray<TArray<FString>> InLinear,
		TArray<FString> InTranslation,
		FUeAgentTemporalPoint InValidAt,
		FString InEvidence)
		: ConversionId(MoveTemp(InConversionId))
		, FromFrame(MoveTemp(InFromFrame))
		, ToFrame(MoveTemp(InToFrame))
		, SourceDimension(InSourceDimension)
		, TargetDimension(InTargetDimension)
		, Linear(MoveTemp(InLinear))
		, Translation(MoveTemp(InTranslation))
		, ValidAt(MoveTemp(InValidAt))
		, Evidence(MoveTemp(InEvidence))
	{
		const auto RequiredText = [&](const FString& Value, const TCHAR* Path, const TCHAR* Reason)
		{
			if (IsNonEmpty(Value)) return true;
			ValidationDiagnostic = SpatialValueFailure(TEXT("spatial_conversion_invalid"), Reason, Path,
				MakeShared<FJsonValueString>(Value), MakeShared<FJsonValueString>(TEXT("non-empty string")));
			return false;
		};
		if (!RequiredText(ConversionId, TEXT("conversion_id"), TEXT("The spatial conversion requires a conversion identifier."))
			|| !RequiredText(FromFrame, TEXT("from_frame"), TEXT("The spatial conversion requires a source frame reference."))
			|| !RequiredText(ToFrame, TEXT("to_frame"), TEXT("The spatial conversion requires a target frame reference."))) return;
		if (SourceDimension != TargetDimension)
		{
			ValidationDiagnostic = SpatialValueFailure(TEXT("spatial_conversion_invalid"),
				TEXT("The target frame dimension must equal the source frame dimension."), TEXT("target_dimension"),
				MakeShared<FJsonValueNumber>(TargetDimension), MakeShared<FJsonValueNumber>(SourceDimension));
			return;
		}
		if (SourceDimension != 2 && SourceDimension != 3)
		{
			ValidationDiagnostic = SpatialValueFailure(TEXT("spatial_conversion_invalid"),
				TEXT("The spatial conversion dimension must be 2 or 3."), TEXT("source_dimension"),
				MakeShared<FJsonValueNumber>(SourceDimension), MakeShared<FJsonValueString>(TEXT("2 or 3")));
			return;
		}
		ValidationDiagnostic = ValidateSquareExactLexemeMatrix(Linear, SourceDimension, TEXT("linear"));
		if (!ValidationDiagnostic.IsEmpty()) return;
		if (Translation.Num() != TargetDimension)
		{
			ValidationDiagnostic = SpatialValueFailure(TEXT("spatial_conversion_invalid"),
				TEXT("The translation must contain one coordinate per target dimension."), TEXT("translation"),
				MakeShared<FJsonValueNumber>(Translation.Num()), MakeShared<FJsonValueNumber>(TargetDimension));
			return;
		}
		ValidationDiagnostic = ValidateExactLexemeVector(Translation, TEXT("translation"));
		if (!ValidationDiagnostic.IsEmpty()) return;
		if (!ValidAt.IsValid())
		{
			ValidationDiagnostic = ValidAt.GetValidationDiagnostic();
			ValidationDiagnostic.PropertyPath = TEXT("valid_at.") + ValidationDiagnostic.PropertyPath;
			return;
		}
		if (!RequiredText(Evidence, TEXT("evidence"), TEXT("The spatial conversion requires conversion evidence.")))
			ValidationDiagnostic.ActualEvidence = MoveTemp(ValidationDiagnostic.ActualValue);
	}

	const FString& FUeAgentSpatialConversion::GetConversionId() const { return ConversionId; }
	const FString& FUeAgentSpatialConversion::GetFromFrame() const { return FromFrame; }
	const FString& FUeAgentSpatialConversion::GetToFrame() const { return ToFrame; }
	int32 FUeAgentSpatialConversion::GetSourceDimension() const { return SourceDimension; }
	int32 FUeAgentSpatialConversion::GetTargetDimension() const { return TargetDimension; }
	const TArray<TArray<FString>>& FUeAgentSpatialConversion::GetLinear() const { return Linear; }
	const TArray<FString>& FUeAgentSpatialConversion::GetTranslation() const { return Translation; }
	const FUeAgentTemporalPoint& FUeAgentSpatialConversion::GetValidAt() const { return ValidAt; }
	const FString& FUeAgentSpatialConversion::GetEvidence() const { return Evidence; }
	bool FUeAgentSpatialConversion::IsValid() const { return ValidationDiagnostic.IsEmpty(); }

	FUeAgentSpatialValue::FUeAgentSpatialValue(
		FString InSubject,
		EUeAgentSpatialQuantityKind InQuantityKind,
		FUeAgentSpatialFrame InFrame,
		TArray<FString> InComponents,
		FUeAgentTemporalPoint InObservedAt,
		TArray<FString> InConversionEvidence)
		: Subject(MoveTemp(InSubject))
		, QuantityKind(InQuantityKind)
		, Frame(MoveTemp(InFrame))
		, Components(MoveTemp(InComponents))
		, ObservedAt(MoveTemp(InObservedAt))
		, ConversionEvidence(MoveTemp(InConversionEvidence))
	{
		const int32 Expected = QuantityKind == EUeAgentSpatialQuantityKind::Transform
			? FMath::Square(Frame.GetDimension() + 1)
			: Frame.GetDimension();
		if (!IsNonEmpty(Subject)) {
			ValidationDiagnostic = SpatialValueFailure(TEXT("spatial_value_invalid"), TEXT("The spatial value requires a non-empty subject identifier."),
				TEXT("subject"), nullptr, MakeShared<FJsonValueString>(TEXT("non-empty subject identifier")));
			ValidationDiagnostic.ActualEvidence = MakeShared<FJsonValueString>(Subject); return;
		}
		if (!Frame.IsValid()) {
			ValidationDiagnostic = Frame.GetValidationDiagnostic();
			ValidationDiagnostic.PropertyPath = TEXT("frame.") + ValidationDiagnostic.PropertyPath; return;
		}
		if (!ObservedAt.IsValid()) {
			ValidationDiagnostic = ObservedAt.GetValidationDiagnostic();
			ValidationDiagnostic.PropertyPath = TEXT("observed_at.") + ValidationDiagnostic.PropertyPath; return;
		}
		if (!AreTemporalPointsEquivalent(ObservedAt, Frame.GetObservedAt())) {
			const auto& FrameTime = Frame.GetObservedAt();
			ValidationDiagnostic = SpatialValueFailure(TEXT("spatial_value_invalid"),
				ObservedAt.GetClockId() != FrameTime.GetClockId() ? TEXT("The spatial value and its frame use different observation clocks.")
				: TEXT("The spatial value and its frame must have the same observation time and state."), TEXT("observed_at"), nullptr, nullptr);
			ValidationDiagnostic.ExpectedValue = TemporalObservation(FrameTime); ValidationDiagnostic.ActualValue = TemporalObservation(ObservedAt);
			ValidationDiagnostic.ExpectedEvidence = MakeShared<FJsonValueString>(FrameTime.GetClockId());
			ValidationDiagnostic.ActualEvidence = MakeShared<FJsonValueString>(ObservedAt.GetClockId()); return;
		}
		if (Components.Num() != Expected) {
			ValidationDiagnostic = SpatialValueFailure(TEXT("spatial_value_invalid"), TEXT("The spatial component count does not match its quantity and frame dimension."),
				TEXT("components"), MakeShared<FJsonValueNumber>(Components.Num()), MakeShared<FJsonValueNumber>(Expected)); return;
		}
		ValidationDiagnostic = ValidateExactLexemeVector(Components, TEXT("components"));
		if (!ValidationDiagnostic.IsEmpty()) return;
		for (int32 Index = 0; Index < ConversionEvidence.Num(); ++Index) {
			if (!IsNonEmpty(ConversionEvidence[Index])) {
				ValidationDiagnostic = SpatialValueFailure(TEXT("spatial_value_invalid"), TEXT("A spatial conversion evidence entry must be non-empty."),
					FString::Printf(TEXT("conversion_evidence[%d]"), Index), nullptr, MakeShared<FJsonValueString>(TEXT("non-empty evidence identifier")));
				ValidationDiagnostic.ActualEvidence = MakeShared<FJsonValueString>(ConversionEvidence[Index]); return;
			}
		}
		if (QuantityKind == EUeAgentSpatialQuantityKind::Transform)
		{
			const int32 Size = Frame.GetDimension() + 1;
			for (int32 Column = 0; Column < Size; ++Column)
			{
				const FString& Value = Components[Components.Num() - Size + Column];
				const FString ExpectedValue = Column == Size - 1 ? TEXT("1") : TEXT("0");
				if (!ExactNumbersEqual(Value, ExpectedValue))
				{
					ValidationDiagnostic = SpatialValueFailure(TEXT("spatial_value_invalid"), TEXT("The transform homogeneous row must contain zeros followed by one."),
						FString::Printf(TEXT("components[%d]"), Components.Num() - Size + Column),
						MakeShared<FJsonValueString>(Value), MakeShared<FJsonValueString>(ExpectedValue));
					return;
				}
			}
		}
	}

	const FString& FUeAgentSpatialValue::GetSubject() const { return Subject; }
	EUeAgentSpatialQuantityKind FUeAgentSpatialValue::GetQuantityKind() const { return QuantityKind; }
	const FUeAgentSpatialFrame& FUeAgentSpatialValue::GetFrame() const { return Frame; }
	const TArray<FString>& FUeAgentSpatialValue::GetComponents() const { return Components; }
	const FUeAgentTemporalPoint& FUeAgentSpatialValue::GetObservedAt() const { return ObservedAt; }
	const TArray<FString>& FUeAgentSpatialValue::GetConversionEvidence() const
	{
		return ConversionEvidence;
	}
	bool FUeAgentSpatialValue::IsValid() const { return ValidationDiagnostic.IsEmpty(); }

	bool FUeAgentSpatialValue::SemanticallyEquals(
		const FUeAgentSpatialValue& Other) const
	{
		return IsValid()
			&& Other.IsValid()
			&& Subject == Other.Subject
			&& QuantityKind == Other.QuantityKind
			&& Frame.SemanticallyEquals(Other.Frame)
			&& AreTemporalPointsEquivalent(ObservedAt, Other.ObservedAt)
			&& ExactLexemeArraysEqual(Components, Other.Components);
	}

	FUeAgentCanonicalValue::FUeAgentCanonicalValue(EUeAgentValueKind InKind)
		: Kind(InKind)
	{
	}

	bool TryNormalizeNumberLexeme(const FString& Lexeme, FString& OutNormalized)
	{
		FExactNumber Number;
		if (!ParseExactNumber(Lexeme, Number)) return false;
		OutNormalized = (Number.bNegative ? TEXT("-") : TEXT("")) + Number.Digits;
		if (Number.PowerOfTen != 0) OutNormalized += TEXT("e") + FString::FromInt(Number.PowerOfTen);
		return true;
	}

	bool IsIntegralNumberLexeme(const FString& Lexeme)
	{
		FExactNumber Number;
		return ParseExactNumber(Lexeme, Number) && Number.PowerOfTen >= 0;
	}

	bool TryCompareNumberLexemes(const FString& Left, const FString& Right, int32& OutComparison)
	{
		FExactNumber A, B;
		if (!ParseExactNumber(Left, A) || !ParseExactNumber(Right, B)) return false;
		const bool AZero = A.Digits == TEXT("0"), BZero = B.Digits == TEXT("0");
		if (AZero || BZero)
		{
			OutComparison = AZero && BZero ? 0 : AZero ? (B.bNegative ? 1 : -1) : (A.bNegative ? -1 : 1);
			return true;
		}
		if (A.bNegative != B.bNegative)
		{
			OutComparison = A.bNegative ? -1 : 1;
			return true;
		}
		const int64 AOrder = static_cast<int64>(A.Digits.Len()) + A.PowerOfTen;
		const int64 BOrder = static_cast<int64>(B.Digits.Len()) + B.PowerOfTen;
		int32 Comparison = AOrder < BOrder ? -1 : AOrder > BOrder ? 1 : 0;
		if (Comparison == 0)
		{
			// Compare significant digits with virtual trailing zeros; do not expand exponents.
			for (int32 Index = 0; Index < FMath::Max(A.Digits.Len(), B.Digits.Len()); ++Index)
			{
				const TCHAR ADigit = Index < A.Digits.Len() ? A.Digits[Index] : TEXT('0');
				const TCHAR BDigit = Index < B.Digits.Len() ? B.Digits[Index] : TEXT('0');
				if (ADigit != BDigit) { Comparison = ADigit < BDigit ? -1 : 1; break; }
			}
		}
		OutComparison = A.bNegative ? -Comparison : Comparison;
		return true;
	}

	bool FUeAgentCanonicalValue::TryNull(
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError)
	{
		ResetError(OutError);
		OutValue.Reset();
		if (!Charge(Resources, {
			{EUeAgentResourceDimension::Nodes, 1},
			{EUeAgentResourceDimension::WorkUnits, 1},
			{EUeAgentResourceDimension::EstimatedMemoryBytes, 48},
		}, TEXT("/value"), OutError))
		{
			return false;
		}
		OutValue = MakeShareable(new FUeAgentCanonicalValue(EUeAgentValueKind::Null));
		return true;
	}

	bool FUeAgentCanonicalValue::TryBoolean(
		bool Value,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError)
	{
		ResetError(OutError);
		OutValue.Reset();
		if (!Charge(Resources, {
			{EUeAgentResourceDimension::Nodes, 1},
			{EUeAgentResourceDimension::WorkUnits, 1},
			{EUeAgentResourceDimension::EstimatedMemoryBytes, 48},
		}, TEXT("/value"), OutError))
		{
			return false;
		}
		TSharedPtr<FUeAgentCanonicalValue> Mutable =
			MakeShareable(new FUeAgentCanonicalValue(EUeAgentValueKind::Boolean));
		Mutable->Kind = EUeAgentValueKind::Boolean;
		Mutable->bBoolean = Value;
		OutValue = Mutable;
		return true;
	}

	bool FUeAgentCanonicalValue::TryString(
		const FString& Value,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError)
	{
		ResetError(OutError);
		OutValue.Reset();
		const int64 StringBytes = MeasureText(Value).Utf8Bytes;
		if (!Charge(Resources, {
			{EUeAgentResourceDimension::Nodes, 1},
			{EUeAgentResourceDimension::StringBytes, StringBytes},
			{EUeAgentResourceDimension::WorkUnits, 1},
			{EUeAgentResourceDimension::EstimatedMemoryBytes, 48 + StringBytes},
		}, TEXT("/value"), OutError))
		{
			return false;
		}
		TSharedPtr<FUeAgentCanonicalValue> Mutable =
			MakeShareable(new FUeAgentCanonicalValue(EUeAgentValueKind::String));
		Mutable->Text = Value;
		OutValue = Mutable;
		return true;
	}

	bool FUeAgentCanonicalValue::TryInteger(
		const FString& Lexeme,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError)
	{
		if (!IsCanonicalIntegerLexeme(Lexeme))
		{
			OutValue.Reset();
			return Fail(OutError, TEXT("integer_lexeme_not_canonical"), TEXT("The integer text is not a canonical signed integer spelling."), TEXT("/value"),
				MakeShared<FJsonValueString>(TEXT("canonical integer spelling")), MakeShared<FJsonValueString>(Lexeme));
		}
		ResetError(OutError);
		OutValue.Reset();
		const int64 StringBytes = MeasureText(Lexeme).Utf8Bytes;
		if (!Charge(Resources, {
			{EUeAgentResourceDimension::Nodes, 1},
			{EUeAgentResourceDimension::StringBytes, StringBytes},
			{EUeAgentResourceDimension::WorkUnits, 1},
			{EUeAgentResourceDimension::EstimatedMemoryBytes, 48 + StringBytes},
		}, TEXT("/value"), OutError))
		{
			return false;
		}
		TSharedPtr<FUeAgentCanonicalValue> Mutable =
			MakeShareable(new FUeAgentCanonicalValue(EUeAgentValueKind::Integer));
		Mutable->Text = Lexeme;
		OutValue = Mutable;
		return true;
	}

	bool FUeAgentCanonicalValue::TryDecimal(
		const FString& Lexeme,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError)
	{
		FExactNumber Parsed;
		if (!ParseExactNumber(Lexeme, Parsed))
		{
			OutValue.Reset();
			return Fail(OutError, TEXT("decimal_lexeme_not_finite"), TEXT("The numeric text is not a finite exact decimal spelling."), TEXT("/value"),
				MakeShared<FJsonValueString>(TEXT("finite exact decimal spelling")), MakeShared<FJsonValueString>(Lexeme));
		}
		ResetError(OutError);
		OutValue.Reset();
		const int64 StringBytes = MeasureText(Lexeme).Utf8Bytes;
		if (!Charge(Resources, {
			{EUeAgentResourceDimension::Nodes, 1},
			{EUeAgentResourceDimension::StringBytes, StringBytes},
			{EUeAgentResourceDimension::WorkUnits, 1},
			{EUeAgentResourceDimension::EstimatedMemoryBytes, 48 + StringBytes},
		}, TEXT("/value"), OutError))
		{
			return false;
		}
		TSharedPtr<FUeAgentCanonicalValue> Mutable =
			MakeShareable(new FUeAgentCanonicalValue(EUeAgentValueKind::Decimal));
		Mutable->Text = Lexeme;
		OutValue = Mutable;
		return true;
	}

	bool FUeAgentCanonicalValue::TryArray(
		const TArray<FUeAgentCanonicalValueRef>& Items,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError)
	{
		ResetError(OutError);
		OutValue.Reset();
		if (!Charge(Resources, {
			{EUeAgentResourceDimension::Nodes, 1},
			{EUeAgentResourceDimension::Items, Items.Num()},
			{EUeAgentResourceDimension::WorkUnits, 1 + Items.Num()},
			{EUeAgentResourceDimension::EstimatedMemoryBytes, 48 + Items.Num() * 16},
		}, TEXT("/value"), OutError))
		{
			return false;
		}
		TSharedPtr<FUeAgentCanonicalValue> Mutable =
			MakeShareable(new FUeAgentCanonicalValue(EUeAgentValueKind::Array));
		Mutable->ArrayItems = Items;
		OutValue = Mutable;
		return true;
	}

	bool FUeAgentCanonicalValue::TryObject(
		const TArray<TPair<FString, FUeAgentCanonicalValueRef>>& Items,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError)
	{
		ResetError(OutError);
		OutValue.Reset();
		int64 StringBytes = 0;
		TSet<FString, FExactStringKeyFuncs> Keys;
		for (const auto& Item : Items)
		{
			if (Item.Key.IsEmpty())
			{
				return Fail(OutError, TEXT("object_key_empty"), TEXT("A canonical object member key must be non-empty."), TEXT("/value"),
					MakeShared<FJsonValueString>(TEXT("non-empty member key")), MakeShared<FJsonValueString>(Item.Key));
			}
			if (Keys.Contains(Item.Key))
			{
				return Fail(
					OutError,
					TEXT("object_key_duplicate"),
					TEXT("The canonical object contains the same member key more than once."),
					JsonPointerJoin(TEXT("/value"), Item.Key), MakeShared<FJsonValueString>(TEXT("unique member key")), MakeShared<FJsonValueString>(Item.Key));
			}
			Keys.Add(Item.Key);
			StringBytes += MeasureText(Item.Key).Utf8Bytes;
		}
		if (!Charge(Resources, {
			{EUeAgentResourceDimension::Nodes, 1},
			{EUeAgentResourceDimension::Items, Items.Num()},
			{EUeAgentResourceDimension::StringBytes, StringBytes},
			{EUeAgentResourceDimension::WorkUnits, 1 + Items.Num()},
			{EUeAgentResourceDimension::EstimatedMemoryBytes, 64 + Items.Num() * 32 + StringBytes},
		}, TEXT("/value"), OutError))
		{
			return false;
		}
		TSharedPtr<FUeAgentCanonicalValue> Mutable =
			MakeShareable(new FUeAgentCanonicalValue(EUeAgentValueKind::Object));
		Mutable->ObjectItems = Items;
		Mutable->ObjectItems.Sort(
			[](const auto& Left, const auto& Right)
			{
				return Left.Key.Compare(Right.Key, ESearchCase::CaseSensitive) < 0;
			});
		OutValue = Mutable;
		return true;
	}

	bool FUeAgentCanonicalValue::TryTemporal(
		const FUeAgentTemporalPoint& Value,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError)
	{
		ResetError(OutError);
		OutValue.Reset();
		if (!Value.IsValid())
		{
			OutError = FUeAgentStateSemanticError::WrapFailure(TEXT("temporal_value_invalid"), Value.GetValidationDiagnostic(), TEXT("state_semantic"));
			OutError.Source.JsonPath = TEXT("/value"); return false;
		}
		if (!Charge(Resources, {
			{EUeAgentResourceDimension::Nodes, 1},
			{EUeAgentResourceDimension::WorkUnits, 1},
			{EUeAgentResourceDimension::EstimatedMemoryBytes, 96},
		}, TEXT("/value"), OutError))
		{
			return false;
		}
		TSharedPtr<FUeAgentCanonicalValue> Mutable =
			MakeShareable(new FUeAgentCanonicalValue(EUeAgentValueKind::Temporal));
		Mutable->Temporal = MakeShared<FUeAgentTemporalPoint>(Value);
		OutValue = Mutable;
		return true;
	}

	bool FUeAgentCanonicalValue::TrySpatial(
		const FUeAgentSpatialValue& Value,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError)
	{
		ResetError(OutError);
		OutValue.Reset();
		if (!Value.IsValid())
		{
			OutError = Value.GetValidationDiagnostic(); OutError.Source.JsonPath = TEXT("/value"); return false;
		}
		if (!Charge(Resources, {
			{EUeAgentResourceDimension::Nodes, 1},
			{EUeAgentResourceDimension::Items, Value.GetComponents().Num()},
			{EUeAgentResourceDimension::WorkUnits, 1 + Value.GetComponents().Num()},
			{EUeAgentResourceDimension::EstimatedMemoryBytes, 128 + Value.GetComponents().Num() * 32},
		}, TEXT("/value"), OutError))
		{
			return false;
		}
		TSharedPtr<FUeAgentCanonicalValue> Mutable =
			MakeShareable(new FUeAgentCanonicalValue(EUeAgentValueKind::Spatial));
		Mutable->Spatial = MakeShared<FUeAgentSpatialValue>(Value);
		OutValue = Mutable;
		return true;
	}

	EUeAgentValueKind FUeAgentCanonicalValue::GetKind() const { return Kind; }
	bool FUeAgentCanonicalValue::GetBoolean() const { return bBoolean; }
	const FString& FUeAgentCanonicalValue::GetText() const { return Text; }
	const FString& FUeAgentCanonicalValue::GetLexeme() const { return Text; }
	const TArray<FUeAgentCanonicalValueRef>& FUeAgentCanonicalValue::GetArrayItems() const
	{
		return ArrayItems;
	}
	const TArray<TPair<FString, FUeAgentCanonicalValueRef>>&
	FUeAgentCanonicalValue::GetObjectItems() const
	{
		return ObjectItems;
	}
	const TSharedPtr<const FUeAgentTemporalPoint>&
	FUeAgentCanonicalValue::GetTemporal() const
	{
		return Temporal;
	}
	const FUeAgentSpatialValuePtr& FUeAgentCanonicalValue::GetSpatial() const
	{
		return Spatial;
	}

	namespace
	{
		TSharedPtr<const FUeAgentStateSemanticError> InvalidState(const FString& Code,
			const FString& Reason, const FString& Path)
		{
			auto Error = FUeAgentStateSemanticError::Failure(Code, Reason, TEXT("canonical_state"));
			Error.SemanticPointer = Path;
			return MakeShared<const FUeAgentStateSemanticError>(MoveTemp(Error));
		}

		const FUeAgentStateSemanticError& ValidationOrEmpty(const TSharedPtr<const FUeAgentStateSemanticError>& Error)
		{
			static const FUeAgentStateSemanticError Empty;
			return Error ? *Error : Empty;
		}

		FString DiagnosticValueKind(EUeAgentValueKind Kind)
		{
			switch (Kind)
			{
			case EUeAgentValueKind::Null: return TEXT("null");
			case EUeAgentValueKind::Boolean: return TEXT("boolean");
			case EUeAgentValueKind::String: return TEXT("string");
			case EUeAgentValueKind::Integer: return TEXT("integer");
			case EUeAgentValueKind::Decimal: return TEXT("decimal");
			case EUeAgentValueKind::Array: return TEXT("array");
			case EUeAgentValueKind::Object: return TEXT("object");
			case EUeAgentValueKind::Temporal: return TEXT("temporal");
			case EUeAgentValueKind::Spatial: return TEXT("spatial");
			default: return FString::FromInt(static_cast<int32>(Kind));
			}
		}

		bool NormalizeIdentityPart(FString& Value, const FString& Field,
			TSharedPtr<const FUeAgentStateSemanticError>& Error)
		{
			const FString Original = Value;
			if (Private::TryNormalizeNfcText(Value, Value)) return true;
			auto Failure = FUeAgentStateSemanticError::Failure(TEXT("canonical_identity_invalid"),
				Original.TrimStartAndEnd().IsEmpty()
					? FString::Printf(TEXT("Identity field '%s' is empty."), *Field)
					: FString::Printf(TEXT("Identity field '%s' could not be normalized to NFC text."), *Field), TEXT("canonical_state"));
			Failure.SemanticPointer = TEXT("/identity/") + Field;
			// These fields may contain opaque object identity or generation data.
			Failure.ActualEvidence = MakeShared<FJsonValueString>(Original);
			if (!Error) Error = MakeShared<const FUeAgentStateSemanticError>(MoveTemp(Failure));
			return false;
		}
	}

	FUeAgentCanonicalIdentity::FUeAgentCanonicalIdentity(
		FString InSemanticId,
		FString InTargetKind,
		FString InOwnerId,
		FString InSelector,
		FString InObjectRole,
		FString InGeneration)
		: SemanticId(MoveTemp(InSemanticId))
		, TargetKind(MoveTemp(InTargetKind))
		, OwnerId(MoveTemp(InOwnerId))
		, Selector(MoveTemp(InSelector))
		, ObjectRole(MoveTemp(InObjectRole))
		, Generation(MoveTemp(InGeneration))
	{
		bValid = NormalizeIdentityPart(SemanticId, TEXT("semantic_id"), ValidationDiagnostic)
			&& NormalizeIdentityPart(TargetKind, TEXT("target_kind"), ValidationDiagnostic)
			&& NormalizeIdentityPart(OwnerId, TEXT("owner"), ValidationDiagnostic)
			&& NormalizeIdentityPart(Selector, TEXT("selector"), ValidationDiagnostic)
			&& NormalizeIdentityPart(ObjectRole, TEXT("object_role"), ValidationDiagnostic)
			&& NormalizeIdentityPart(Generation, TEXT("generation"), ValidationDiagnostic);
		if (bValid && !Selector.IsEmpty())
		{
			Selectors.Add(TEXT("selector"), Selector);
		}
		if (bValid)
		{
			const TArray<FString> Components = {
				SemanticId,
				TargetKind,
				OwnerId,
				Selector,
				ObjectRole,
				Generation,
			};
			TArray<FString> Encoded;
			for (const FString& Component : Components)
			{
				Encoded.Add(FString::Printf(
					TEXT("%lld:%s"),
					MeasureText(Component).Utf8Bytes,
					*Component));
			}
			CanonicalKey = FString::Join(Encoded, TEXT("|"));
		}
	}

	FUeAgentCanonicalIdentity::FUeAgentCanonicalIdentity(
		FString InSemanticId,
		FString InTargetKind,
		FString InOwnerId,
		TMap<FString, FString> InSelectors,
		FString InObjectRole,
		FString InGeneration)
		: SemanticId(MoveTemp(InSemanticId))
		, TargetKind(MoveTemp(InTargetKind))
		, OwnerId(MoveTemp(InOwnerId))
		, Selectors(MoveTemp(InSelectors))
		, ObjectRole(MoveTemp(InObjectRole))
		, Generation(MoveTemp(InGeneration))
	{
		bValid = NormalizeIdentityPart(SemanticId, TEXT("semantic_id"), ValidationDiagnostic)
			&& NormalizeIdentityPart(TargetKind, TEXT("target_kind"), ValidationDiagnostic)
			&& NormalizeIdentityPart(OwnerId, TEXT("owner"), ValidationDiagnostic)
			&& NormalizeIdentityPart(ObjectRole, TEXT("object_role"), ValidationDiagnostic)
			&& NormalizeIdentityPart(Generation, TEXT("generation"), ValidationDiagnostic)
			&& !Selectors.IsEmpty();
		if (!bValid && !ValidationDiagnostic) ValidationDiagnostic = InvalidState(TEXT("canonical_identity_invalid"), TEXT("Identity requires at least one selector."), TEXT("/identity/selectors"));
		TArray<FString> SelectorNames;
		Selectors.GetKeys(SelectorNames);
		SelectorNames.Sort();
		FString SelectorMaterial;
		TMap<FString, FString> NormalizedSelectors;
		for (const FString& Name : SelectorNames)
		{
			FString NormalizedName = Name;
			FString NormalizedValue = Selectors.FindChecked(Name);
			if (!NormalizeIdentityPart(NormalizedName, TEXT("selector_name"), ValidationDiagnostic)
				|| !NormalizeIdentityPart(NormalizedValue, TEXT("selector_value"), ValidationDiagnostic))
			{
				bValid = false;
				break;
			}
			if (NormalizedSelectors.Contains(NormalizedName))
			{
				if (!ValidationDiagnostic) ValidationDiagnostic = InvalidState(TEXT("canonical_identity_invalid"), TEXT("Two selector names become identical after NFC normalization."), TEXT("/identity/selectors"));
				bValid = false;
				break;
			}
			NormalizedSelectors.Add(NormalizedName, NormalizedValue);
			SelectorMaterial += FString::Printf(
				TEXT("%lld:%s=%lld:%s|"),
				MeasureText(NormalizedName).Utf8Bytes,
				*NormalizedName,
				MeasureText(NormalizedValue).Utf8Bytes,
				*NormalizedValue);
		}
		Selectors = MoveTemp(NormalizedSelectors);
		Selector = SelectorMaterial;
		if (bValid)
		{
			const TArray<FString> Components = {
				SemanticId,
				TargetKind,
				OwnerId,
				SelectorMaterial,
				ObjectRole,
				Generation,
			};
			TArray<FString> Encoded;
			for (const FString& Component : Components)
			{
				Encoded.Add(FString::Printf(
					TEXT("%lld:%s"),
					MeasureText(Component).Utf8Bytes,
					*Component));
			}
			CanonicalKey = FString::Join(Encoded, TEXT("|"));
		}
	}

	FUeAgentCanonicalIdentity FUeAgentCanonicalIdentity::WithGeneration(FString InGeneration) const
	{
		FUeAgentCanonicalIdentity Result = *this;
		if (!bValid) return Result;
		Result.Generation = MoveTemp(InGeneration);
		Result.bValid = NormalizeIdentityPart(Result.Generation, TEXT("generation"), Result.ValidationDiagnostic);
		if (!Result.bValid)
		{
			Result.CanonicalKey.Reset();
			return Result;
		}
		// Both constructors frame the final generation by its UTF-8 byte length.
		// Remove that exact suffix, not a delimiter search: generation may contain '|'.
		const FString OldSuffix = FString::Printf(TEXT("%lld:%s"), MeasureText(Generation).Utf8Bytes, *Generation);
		Result.CanonicalKey = CanonicalKey.LeftChop(OldSuffix.Len())
			+ FString::Printf(TEXT("%lld:%s"), MeasureText(Result.Generation).Utf8Bytes, *Result.Generation);
		return Result;
	}

	const FString& FUeAgentCanonicalIdentity::GetSemanticId() const { return SemanticId; }
	const FString& FUeAgentCanonicalIdentity::GetTargetKind() const { return TargetKind; }
	const FString& FUeAgentCanonicalIdentity::GetOwnerId() const { return OwnerId; }
	const FString& FUeAgentCanonicalIdentity::GetSelector() const { return Selector; }
	FString FUeAgentCanonicalIdentity::GetSelector(const FString& Name) const
	{
		const FString* Found = Selectors.Find(Name);
		return Found ? *Found : FString();
	}
	const TMap<FString, FString>& FUeAgentCanonicalIdentity::GetSelectors() const
	{
		return Selectors;
	}
	const FString& FUeAgentCanonicalIdentity::GetObjectRole() const { return ObjectRole; }
	const FString& FUeAgentCanonicalIdentity::GetGeneration() const { return Generation; }
	const FString& FUeAgentCanonicalIdentity::GetCanonicalKey() const { return CanonicalKey; }
	bool FUeAgentCanonicalIdentity::IsValid() const { return bValid; }
	const FUeAgentStateSemanticError& FUeAgentCanonicalIdentity::GetValidationDiagnostic() const { return ValidationOrEmpty(ValidationDiagnostic); }
	bool FUeAgentCanonicalIdentity::operator==(
		const FUeAgentCanonicalIdentity& Other) const
	{
		return CanonicalKey == Other.CanonicalKey;
	}

	FUeAgentCanonicalType::FUeAgentCanonicalType(
		FString InTypeId,
		EUeAgentValueKind InValueKind,
		bool bInNullable,
		FString InContainerKind)
		: TypeId(MoveTemp(InTypeId))
		, ValueKind(InValueKind)
		, bNullable(bInNullable)
		, ContainerKind(MoveTemp(InContainerKind))
	{
		const FString Expected =
			ValueKind == EUeAgentValueKind::Array
				? TEXT("array")
				: ValueKind == EUeAgentValueKind::Object
					? TEXT("object")
					: TEXT("scalar");
		bValid = IsNonEmpty(TypeId) && ContainerKind == Expected;
		if (!bValid)
		{
			auto Error = FUeAgentStateSemanticError::Failure(TEXT("canonical_type_invalid"),
				!IsNonEmpty(TypeId) ? TEXT("The canonical type identifier is empty.")
					: TEXT("The container kind does not match the declared value kind."), TEXT("canonical_state"));
			Error.SemanticPointer = !IsNonEmpty(TypeId) ? TEXT("/type/type_id") : TEXT("/type/container_kind");
			Error.ActualValue = MakeShared<FJsonValueString>(!IsNonEmpty(TypeId) ? TypeId : ContainerKind);
			if (IsNonEmpty(TypeId)) Error.ExpectedValue = MakeShared<FJsonValueString>(Expected);
			ValidationDiagnostic = MakeShared<const FUeAgentStateSemanticError>(MoveTemp(Error));
		}
	}

	const FString& FUeAgentCanonicalType::GetTypeId() const { return TypeId; }
	EUeAgentValueKind FUeAgentCanonicalType::GetValueKind() const { return ValueKind; }
	bool FUeAgentCanonicalType::IsNullable() const { return bNullable; }
	const FString& FUeAgentCanonicalType::GetContainerKind() const { return ContainerKind; }
	bool FUeAgentCanonicalType::IsValid() const { return bValid; }
	const FUeAgentStateSemanticError& FUeAgentCanonicalType::GetValidationDiagnostic() const { return ValidationOrEmpty(ValidationDiagnostic); }
	bool FUeAgentCanonicalType::operator==(const FUeAgentCanonicalType& Other) const
	{
		return TypeId == Other.TypeId
			&& ValueKind == Other.ValueKind
			&& bNullable == Other.bNullable
			&& ContainerKind == Other.ContainerKind;
	}

	FUeAgentDefaultValue::FUeAgentDefaultValue(
		EUeAgentDefaultSourceKind InSourceKind,
		FString InSourceIdentity,
		FString InProvenance,
		FUeAgentCanonicalValueRef InValue)
		: SourceKind(InSourceKind)
		, SourceIdentity(MoveTemp(InSourceIdentity))
		, Provenance(MoveTemp(InProvenance))
		, Value(MoveTemp(InValue))
	{
		bValid = IsNonEmpty(SourceIdentity) && IsNonEmpty(Provenance);
		if (!bValid) ValidationDiagnostic = InvalidState(TEXT("canonical_default_invalid"),
			!IsNonEmpty(SourceIdentity) ? TEXT("The captured default has no source identity.") : TEXT("The captured default has no provenance."),
			!IsNonEmpty(SourceIdentity) ? TEXT("/default/source_identity") : TEXT("/default/provenance"));
	}

	EUeAgentDefaultSourceKind FUeAgentDefaultValue::GetSourceKind() const { return SourceKind; }
	const FString& FUeAgentDefaultValue::GetSourceIdentity() const { return SourceIdentity; }
	const FString& FUeAgentDefaultValue::GetProvenance() const { return Provenance; }
	const FUeAgentCanonicalValueRef& FUeAgentDefaultValue::GetValue() const { return Value; }
	bool FUeAgentDefaultValue::IsValid() const { return bValid; }
	const FUeAgentStateSemanticError& FUeAgentDefaultValue::GetValidationDiagnostic() const { return ValidationOrEmpty(ValidationDiagnostic); }

	FUeAgentStateDetails::FUeAgentStateDetails(
		FString InDisplayName,
		FString InTooltip,
		TArray<FString> InCategoryPath,
		bool bInEditable,
		FString InAvailability,
		TMap<FString, FString> InConstraints)
		: DisplayName(MoveTemp(InDisplayName))
		, Tooltip(MoveTemp(InTooltip))
		, CategoryPath(MoveTemp(InCategoryPath))
		, bEditable(bInEditable)
		, Availability(MoveTemp(InAvailability))
		, Constraints(MoveTemp(InConstraints))
	{
		if (!IsNonEmpty(DisplayName)) ValidationDiagnostic = InvalidState(TEXT("canonical_details_invalid"), TEXT("The captured entry has an empty display name."), TEXT("/details/display_name"));
		else if (!IsNonEmpty(Availability)) ValidationDiagnostic = InvalidState(TEXT("canonical_details_invalid"), TEXT("The captured entry has an empty availability description."), TEXT("/details/availability"));
		for (int32 Index = 0; !ValidationDiagnostic && Index < CategoryPath.Num(); ++Index)
			if (!IsNonEmpty(CategoryPath[Index])) ValidationDiagnostic = InvalidState(TEXT("canonical_details_invalid"), TEXT("Category names must be non-empty."), FString::Printf(TEXT("/details/category_path/%d"), Index));
		for (const auto& Constraint : Constraints)
		{
			if (ValidationDiagnostic) break;
			if (!IsNonEmpty(Constraint.Key) || !IsNonEmpty(Constraint.Value))
				ValidationDiagnostic = InvalidState(TEXT("canonical_details_invalid"),
					!IsNonEmpty(Constraint.Key) ? TEXT("A captured constraint has an empty name.")
						: FString::Printf(TEXT("Constraint '%s' has an empty value."), *Constraint.Key),
					TEXT("/details/constraints/") + Constraint.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")));
		}
		bValid = !ValidationDiagnostic;
	}

	const FString& FUeAgentStateDetails::GetDisplayName() const { return DisplayName; }
	const FString& FUeAgentStateDetails::GetTooltip() const { return Tooltip; }
	const TArray<FString>& FUeAgentStateDetails::GetCategoryPath() const { return CategoryPath; }
	bool FUeAgentStateDetails::IsEditable() const { return bEditable; }
	const FString& FUeAgentStateDetails::GetAvailability() const { return Availability; }
	const TMap<FString, FString>& FUeAgentStateDetails::GetConstraints() const
	{
		return Constraints;
	}
	bool FUeAgentStateDetails::IsValid() const { return bValid; }
	const FUeAgentStateSemanticError& FUeAgentStateDetails::GetValidationDiagnostic() const { return ValidationOrEmpty(ValidationDiagnostic); }
	bool FUeAgentStateDetails::operator==(const FUeAgentStateDetails& Other) const
	{
		return DisplayName == Other.DisplayName
			&& Tooltip == Other.Tooltip
			&& CategoryPath == Other.CategoryPath
			&& bEditable == Other.bEditable
			&& Availability == Other.Availability
			&& Constraints.OrderIndependentCompareEqual(Other.Constraints);
	}

	FUeAgentStateProvenance::FUeAgentStateProvenance(
		FString InSourceKind,
		FString InSourceId,
		FString InEvidenceId)
		: SourceKind(MoveTemp(InSourceKind))
		, SourceId(MoveTemp(InSourceId))
		, EvidenceId(MoveTemp(InEvidenceId))
	{
		const TCHAR* Field = !IsNonEmpty(SourceKind) ? TEXT("source_kind") : !IsNonEmpty(SourceId) ? TEXT("source_id") : !IsNonEmpty(EvidenceId) ? TEXT("evidence_id") : nullptr;
		if (Field)
		{
			ValidationDiagnostic = FUeAgentStateSemanticError::Failure(TEXT("state_provenance_invalid"), FString::Printf(TEXT("State provenance requires a non-empty %s."), Field), TEXT("state_semantic"));
			ValidationDiagnostic.PropertyPath = Field;
			ValidationDiagnostic.ActualEvidence = MakeShared<FJsonValueString>(!IsNonEmpty(SourceKind) ? SourceKind : !IsNonEmpty(SourceId) ? SourceId : EvidenceId);
		}
	}

	const FString& FUeAgentStateProvenance::GetSourceKind() const { return SourceKind; }
	const FString& FUeAgentStateProvenance::GetSourceId() const { return SourceId; }
	const FString& FUeAgentStateProvenance::GetEvidenceId() const { return EvidenceId; }
	bool FUeAgentStateProvenance::IsValid() const { return ValidationDiagnostic.IsEmpty(); }
	const FUeAgentStateSemanticError& FUeAgentStateProvenance::GetValidationDiagnostic() const { return ValidationDiagnostic; }
	bool FUeAgentStateProvenance::operator==(const FUeAgentStateProvenance& Other) const
	{
		return SourceKind == Other.SourceKind
			&& SourceId == Other.SourceId
			&& EvidenceId == Other.EvidenceId;
	}

	FUeAgentStateRevision::FUeAgentStateRevision(
		FString InSourceId,
		FString InGeneration,
		int64 InSequence)
		: SourceId(MoveTemp(InSourceId))
		, Generation(MoveTemp(InGeneration))
		, Sequence(InSequence)
	{
		if (!IsNonEmpty(SourceId) || !IsNonEmpty(Generation) || Sequence < 0)
		{
			const bool bSourceMissing = !IsNonEmpty(SourceId);
			const bool bGenerationMissing = !IsNonEmpty(Generation);
			ValidationDiagnostic = FUeAgentStateSemanticError::Failure(TEXT("state_revision_invalid"),
				bSourceMissing ? TEXT("The state revision has no source identifier.")
					: bGenerationMissing ? TEXT("The state revision has no generation identifier.")
					: TEXT("The state revision sequence must be non-negative."), TEXT("state_semantic"));
			ValidationDiagnostic.PropertyPath = bSourceMissing ? TEXT("source_id") : bGenerationMissing ? TEXT("generation") : TEXT("sequence");
			ValidationDiagnostic.ActualEvidence = MakeShared<FJsonValueString>(
				bSourceMissing ? SourceId : bGenerationMissing ? Generation : FString::Printf(TEXT("%lld"), Sequence));
		}
	}

	const FString& FUeAgentStateRevision::GetSourceId() const { return SourceId; }
	const FString& FUeAgentStateRevision::GetGeneration() const { return Generation; }
	int64 FUeAgentStateRevision::GetSequence() const { return Sequence; }
	bool FUeAgentStateRevision::IsValid() const { return ValidationDiagnostic.IsEmpty(); }
	const FUeAgentStateSemanticError& FUeAgentStateRevision::GetValidationDiagnostic() const { return ValidationDiagnostic; }
	bool FUeAgentStateRevision::operator==(const FUeAgentStateRevision& Other) const
	{
		return SourceId == Other.SourceId
			&& Generation == Other.Generation
			&& Sequence == Other.Sequence;
	}
	bool FUeAgentStateRevision::operator!=(const FUeAgentStateRevision& Other) const
	{
		return !(*this == Other);
	}

	FUeAgentStateEntry::FUeAgentStateEntry(
		FUeAgentCanonicalIdentity InIdentity,
		FUeAgentCanonicalType InType,
		FUeAgentCanonicalValueRef InValue,
		FUeAgentDefaultValue InDefault,
		FUeAgentStateDetails InDetails,
		FUeAgentStateRevision InObservedRevision,
		EUeAgentAuthoredPresence InAuthoredPresence,
		EUeAgentOverrideState InOverrideState,
		FUeAgentCanonicalValuePtr InAuthoringDescriptor)
		: Identity(MoveTemp(InIdentity))
		, Type(MoveTemp(InType))
		, Value(MoveTemp(InValue))
		, Default(MoveTemp(InDefault))
		, Details(MoveTemp(InDetails))
		, AuthoringDescriptor(MoveTemp(InAuthoringDescriptor))
		, ObservedRevision(MoveTemp(InObservedRevision))
		, AuthoredPresence(InAuthoredPresence)
		, OverrideState(InOverrideState)
	{
		const auto MatchesType =
			[](const FUeAgentCanonicalValueRef& Candidate, const FUeAgentCanonicalType& CanonicalType)
			{
				return Candidate->GetKind() == CanonicalType.GetValueKind()
					|| (Candidate->GetKind() == EUeAgentValueKind::Null
						&& CanonicalType.IsNullable());
			};
		if (!Identity.IsValid()) ValidationDiagnostic = MakeShared<const FUeAgentStateSemanticError>(Identity.GetValidationDiagnostic());
		else if (AuthoringDescriptor && AuthoringDescriptor->GetKind() != EUeAgentValueKind::Object)
			ValidationDiagnostic = InvalidState(TEXT("canonical_entry_invalid"), TEXT("The authoring descriptor must be an object."), TEXT("/authoring_descriptor"));
		else if (!Type.IsValid()) ValidationDiagnostic = MakeShared<const FUeAgentStateSemanticError>(Type.GetValidationDiagnostic());
		else if (!Default.IsValid()) ValidationDiagnostic = MakeShared<const FUeAgentStateSemanticError>(Default.GetValidationDiagnostic());
		else if (!Details.IsValid()) ValidationDiagnostic = MakeShared<const FUeAgentStateSemanticError>(Details.GetValidationDiagnostic());
		else if (!ObservedRevision.IsValid()) ValidationDiagnostic = InvalidState(TEXT("canonical_entry_invalid"), TEXT("The captured entry has an invalid observation revision."), TEXT("/observed_revision"));
		else if (OverrideState == EUeAgentOverrideState::Overridden && AuthoredPresence != EUeAgentAuthoredPresence::Present)
			ValidationDiagnostic = InvalidState(TEXT("canonical_entry_invalid"), TEXT("An overridden value must be authored as present."), TEXT("/authored_presence"));
		else if (OverrideState == EUeAgentOverrideState::Inherited && AuthoredPresence != EUeAgentAuthoredPresence::Absent)
			ValidationDiagnostic = InvalidState(TEXT("canonical_entry_invalid"), TEXT("An inherited value must be authored as absent."), TEXT("/authored_presence"));
		else if (!MatchesType(Value, Type) || !MatchesType(Default.GetValue(), Type))
		{
			const bool bCurrentValue = !MatchesType(Value, Type);
			auto Error = FUeAgentStateSemanticError::Failure(TEXT("canonical_entry_invalid"),
				bCurrentValue ? TEXT("The captured value kind does not match its declared type.") : TEXT("The default value kind does not match its declared type."), TEXT("canonical_state"));
			Error.SemanticPointer = bCurrentValue ? TEXT("/value") : TEXT("/default/value");
			Error.ActualValue = MakeShared<FJsonValueString>(DiagnosticValueKind(bCurrentValue ? Value->GetKind() : Default.GetValue()->GetKind()));
			Error.ExpectedValue = MakeShared<FJsonValueString>(DiagnosticValueKind(Type.GetValueKind()));
			ValidationDiagnostic = MakeShared<const FUeAgentStateSemanticError>(MoveTemp(Error));
		}
		bValid = !ValidationDiagnostic;
	}

	const FUeAgentCanonicalValuePtr& FUeAgentStateEntry::GetAuthoringDescriptor() const { return AuthoringDescriptor; }
	const FUeAgentCanonicalIdentity& FUeAgentStateEntry::GetIdentity() const { return Identity; }
	const FUeAgentCanonicalType& FUeAgentStateEntry::GetType() const { return Type; }
	const FUeAgentCanonicalValueRef& FUeAgentStateEntry::GetValue() const { return Value; }
	const FUeAgentDefaultValue& FUeAgentStateEntry::GetDefault() const { return Default; }
	const FUeAgentStateDetails& FUeAgentStateEntry::GetDetails() const { return Details; }
	const FUeAgentStateRevision& FUeAgentStateEntry::GetObservedRevision() const
	{
		return ObservedRevision;
	}
	EUeAgentAuthoredPresence FUeAgentStateEntry::GetAuthoredPresence() const
	{
		return AuthoredPresence;
	}
	EUeAgentOverrideState FUeAgentStateEntry::GetOverrideState() const
	{
		return OverrideState;
	}
	bool FUeAgentStateEntry::IsValid() const { return bValid; }
	const FUeAgentStateSemanticError& FUeAgentStateEntry::GetValidationDiagnostic() const { return ValidationOrEmpty(ValidationDiagnostic); }

	FUeAgentStateEntry FUeAgentStateEntry::WithObservedRevision(
		FUeAgentStateRevision Revision) const
	{
		return FUeAgentStateEntry(
			Identity,
			Type,
			Value,
			Default,
			Details,
			MoveTemp(Revision),
			AuthoredPresence,
			OverrideState,
			AuthoringDescriptor);
	}

	FUeAgentCanonicalSnapshot::FUeAgentCanonicalSnapshot(
		FString InSnapshotId,
		EUeAgentStatePlane InStatePlane,
		FUeAgentStateRevision InRevision,
		FUeAgentTemporalPoint InCapturedAt,
		FUeAgentStateProvenance InProvenance,
		TArray<FUeAgentStateEntry> InEntries)
		: SnapshotId(MoveTemp(InSnapshotId))
		, StatePlane(InStatePlane)
		, Revision(MoveTemp(InRevision))
		, CapturedAt(MoveTemp(InCapturedAt))
		, Provenance(MoveTemp(InProvenance))
		, Entries(MoveTemp(InEntries))
	{
	}

	bool FUeAgentCanonicalSnapshot::TryCreate(
		const FString& SnapshotId,
		EUeAgentStatePlane StatePlane,
		const FUeAgentStateRevision& Revision,
		const FUeAgentTemporalPoint& CapturedAt,
		const FUeAgentStateProvenance& Provenance,
		const TArray<FUeAgentStateEntry>& Entries,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalSnapshotPtr& OutSnapshot,
		FUeAgentStateSemanticError& OutError)
	{
		return TryCreateWithRequiredProvenance(
			SnapshotId,
			StatePlane,
			Revision,
			CapturedAt,
			Provenance,
			TEXT("independent_readback"),
			TEXT("snapshot_requires_independent_readback"),
			Entries,
			Resources,
			OutSnapshot,
			OutError);
	}

	bool FUeAgentCanonicalSnapshot::TryCreateExpected(
		const FString& SnapshotId,
		EUeAgentStatePlane StatePlane,
		const FUeAgentStateRevision& Revision,
		const FUeAgentTemporalPoint& CapturedAt,
		const FUeAgentStateProvenance& Provenance,
		const TArray<FUeAgentStateEntry>& Entries,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalSnapshotPtr& OutSnapshot,
		FUeAgentStateSemanticError& OutError)
	{
		return TryCreateWithRequiredProvenance(
			SnapshotId,
			StatePlane,
			Revision,
			CapturedAt,
			Provenance,
			TEXT("operation_expected_state"),
			TEXT("snapshot_requires_operation_expected_state"),
			Entries,
			Resources,
			OutSnapshot,
			OutError);
	}

	bool FUeAgentCanonicalSnapshot::TryCreateWithRequiredProvenance(
		const FString& SnapshotId,
		EUeAgentStatePlane StatePlane,
		const FUeAgentStateRevision& Revision,
		const FUeAgentTemporalPoint& CapturedAt,
		const FUeAgentStateProvenance& Provenance,
		const FString& RequiredSourceKind,
		const FString& ProvenanceErrorCode,
		const TArray<FUeAgentStateEntry>& Entries,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalSnapshotPtr& OutSnapshot,
		FUeAgentStateSemanticError& OutError)
	{
		ResetError(OutError);
		OutSnapshot.Reset();
		if (!IsNonEmpty(SnapshotId)
			|| !Revision.IsValid()
			|| !CapturedAt.IsValid()
			|| !Provenance.IsValid()
			|| !IsNonEmpty(RequiredSourceKind)
			|| !IsNonEmpty(ProvenanceErrorCode))
		{
			OutError = FUeAgentStateSemanticError::Failure(TEXT("snapshot_contract_invalid"),
				!IsNonEmpty(SnapshotId) ? TEXT("The captured snapshot has no identifier.")
				: !Revision.IsValid() ? TEXT("The captured snapshot has an invalid revision.")
				: !CapturedAt.IsValid() ? TEXT("The captured snapshot has an invalid capture time.")
				: !Provenance.IsValid() ? TEXT("The captured snapshot has incomplete provenance.")
				: !IsNonEmpty(RequiredSourceKind) ? TEXT("Snapshot creation requires a provenance source kind.")
				: TEXT("Snapshot creation requires a provenance failure code."), TEXT("canonical_snapshot"));
			OutError.SemanticPointer = TEXT("/snapshot");
			return false;
		}
		if (Provenance.GetSourceKind() != RequiredSourceKind)
		{
			OutError = FUeAgentStateSemanticError::Failure(ProvenanceErrorCode,
				TEXT("The snapshot provenance source kind does not match the required source."), TEXT("canonical_snapshot"));
			OutError.SemanticPointer = TEXT("/provenance/source_kind");
			OutError.ActualValue = MakeShared<FJsonValueString>(Provenance.GetSourceKind());
			OutError.ExpectedValue = MakeShared<FJsonValueString>(RequiredSourceKind);
			return false;
		}
		if (!Charge(Resources, {
			{EUeAgentResourceDimension::Nodes, 1},
			{EUeAgentResourceDimension::Items, Entries.Num()},
			{EUeAgentResourceDimension::WorkUnits, 1 + Entries.Num()},
			{EUeAgentResourceDimension::EstimatedMemoryBytes, 128 + Entries.Num() * 128},
		}, TEXT("/snapshot"), OutError))
		{
			return false;
		}
		TArray<FUeAgentStateEntry> Ordered = Entries;
		Ordered.Sort(
			[](const FUeAgentStateEntry& Left, const FUeAgentStateEntry& Right)
			{
				return Left.GetIdentity().GetCanonicalKey().Compare(
					Right.GetIdentity().GetCanonicalKey(),
					ESearchCase::CaseSensitive) < 0;
			});
		FString PreviousKey;
		for (const FUeAgentStateEntry& Entry : Ordered)
		{
			if (!Entry.IsValid())
			{
				OutError = Entry.GetValidationDiagnostic();
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Entry.GetDetails().GetDisplayName();
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Entry.GetIdentity().GetCanonicalKey();
				return false;
			}
			const FString& Key = Entry.GetIdentity().GetCanonicalKey();
			if (Key == PreviousKey)
			{
				OutError = FUeAgentStateSemanticError::Failure(TEXT("snapshot_identity_duplicate"),
					TEXT("More than one captured state entry has the same canonical identity."), TEXT("canonical_snapshot"));
				OutError.CandidateName = Entry.GetDetails().GetDisplayName();
				OutError.SubjectId = Key;
				OutError.SemanticPointer = TEXT("/entries");
				return false;
			}
			PreviousKey = Key;
			if (Entry.GetObservedRevision() != Revision)
			{
				OutError = FUeAgentStateSemanticError::Failure(TEXT("snapshot_revision_mismatch"),
					TEXT("The state entry was captured at a different revision from its snapshot."), TEXT("canonical_snapshot"));
				OutError.CandidateName = Entry.GetDetails().GetDisplayName();
				OutError.SubjectId = Key;
				OutError.SemanticPointer = TEXT("/observed_revision");
				return false;
			}
		}
		OutSnapshot = MakeShareable(new FUeAgentCanonicalSnapshot(
			SnapshotId,
			StatePlane,
			Revision,
			CapturedAt,
			Provenance,
			MoveTemp(Ordered)));
		return true;
	}

	const FString& FUeAgentCanonicalSnapshot::GetSnapshotId() const { return SnapshotId; }
	EUeAgentStatePlane FUeAgentCanonicalSnapshot::GetStatePlane() const { return StatePlane; }
	const FUeAgentStateRevision& FUeAgentCanonicalSnapshot::GetRevision() const { return Revision; }
	const FUeAgentTemporalPoint& FUeAgentCanonicalSnapshot::GetCapturedAt() const
	{
		return CapturedAt;
	}
	const FUeAgentStateProvenance& FUeAgentCanonicalSnapshot::GetProvenance() const
	{
		return Provenance;
	}
	const TArray<FUeAgentStateEntry>& FUeAgentCanonicalSnapshot::GetEntries() const
	{
		return Entries;
	}

	FUeAgentSemanticDifference::FUeAgentSemanticDifference(
		EUeAgentDifferenceKind InKind, FString InPath,
		FUeAgentCanonicalValuePtr InLeft, FUeAgentCanonicalValuePtr InRight)
		: FUeAgentSemanticDifference(InKind, MoveTemp(InPath), InLeft, InRight, InLeft, InRight) {}

	FUeAgentSemanticDifference::FUeAgentSemanticDifference(
		EUeAgentDifferenceKind InKind, FString InPath,
		FUeAgentCanonicalValuePtr InLeft, FUeAgentCanonicalValuePtr InRight,
		FUeAgentCanonicalValuePtr InPublicLeft, FUeAgentCanonicalValuePtr InPublicRight)
		: Kind(InKind), Path(MoveTemp(InPath)), Left(MoveTemp(InLeft)), Right(MoveTemp(InRight)),
		  PublicPath(Path), PublicLeft(MoveTemp(InPublicLeft)), PublicRight(MoveTemp(InPublicRight)) {}

	EUeAgentDifferenceKind FUeAgentSemanticDifference::GetKind() const { return Kind; }
	const TCHAR* FUeAgentSemanticDifference::GetPublicReason() const
	{
		switch (Kind)
		{
		case EUeAgentDifferenceKind::TypeMismatch: return TEXT("Expected and observed value types differ.");
		case EUeAgentDifferenceKind::IdentityMismatch: return TEXT("The observed object identity or generation differs from the expected object.");
		case EUeAgentDifferenceKind::DefaultMismatch: return TEXT("The default value or its source differs.");
		case EUeAgentDifferenceKind::DetailsMismatch: return TEXT("The property description or editing constraints differ.");
		case EUeAgentDifferenceKind::AuthoredPresenceMismatch: return TEXT("Whether the property is explicitly authored differs.");
		case EUeAgentDifferenceKind::OverrideStateMismatch: return TEXT("The property's override state differs.");
		case EUeAgentDifferenceKind::StatePlaneMismatch: return TEXT("Expected and observed state planes differ.");
		case EUeAgentDifferenceKind::SnapshotRevisionMismatch: return TEXT("The observed snapshot revision differs from the expected revision.");
		case EUeAgentDifferenceKind::TimeDomainMismatch: return TEXT("The values belong to different clock domains.");
		case EUeAgentDifferenceKind::TimeStateMismatch: return TEXT("The temporal observation states differ.");
		case EUeAgentDifferenceKind::FrameMismatch: return TEXT("The coordinate frames differ.");
		case EUeAgentDifferenceKind::SpatialQuantityMismatch: return TEXT("The spatial quantity kinds differ.");
		case EUeAgentDifferenceKind::SubjectMismatch: return TEXT("The spatial values describe different subjects.");
		case EUeAgentDifferenceKind::MissingLeft: return TEXT("The observed entry has no corresponding expected entry.");
		case EUeAgentDifferenceKind::MissingRight: return TEXT("The expected entry is missing from the observed state.");
		default: return TEXT("Expected and observed values differ.");
		}
	}
	const FString& FUeAgentSemanticDifference::GetPath() const { return Path; }
	const FUeAgentCanonicalValuePtr& FUeAgentSemanticDifference::GetLeft() const
	{
		return Left;
	}
	const FUeAgentCanonicalValuePtr& FUeAgentSemanticDifference::GetRight() const
	{
		return Right;
	}

	FUeAgentSemanticComparison::FUeAgentSemanticComparison(
		TArray<FUeAgentSemanticDifference> InDifferences)
		: Differences(MoveTemp(InDifferences))
	{
	}

	bool FUeAgentSemanticComparison::IsEquivalent() const { return Differences.IsEmpty(); }
	const TArray<FUeAgentSemanticDifference>&
	FUeAgentSemanticComparison::GetDifferences() const
	{
		return Differences;
	}

	bool AreNumericLexemesEquivalent(const FString& Left, const FString& Right)
	{
		return ExactNumbersEqual(Left, Right);
	}

	bool TryParseInt64Decimal(const FString& Text, int64& OutValue)
	{
		bool bNegative = false;
		uint64 Magnitude = 0;
		if (!TryParseIntegerMagnitude(Text, true, bNegative, Magnitude)) return false;
		OutValue = bNegative
			? (Magnitude == static_cast<uint64>(MAX_int64) + 1ull ? MIN_int64 : -static_cast<int64>(Magnitude))
			: static_cast<int64>(Magnitude);
		return true;
	}

	bool TryParseUInt64Decimal(const FString& Text, uint64& OutValue)
	{
		bool bNegative = false;
		uint64 Magnitude = 0;
		if (!TryParseIntegerMagnitude(Text, false, bNegative, Magnitude)) return false;
		OutValue = Magnitude;
		return true;
	}

	namespace
	{
		bool TryIntegralNumberText(const FString& Text, FString& OutDigits)
		{
			FExactNumber Number;
			if (!ParseExactNumber(Text, Number) || Number.PowerOfTen < 0
				|| static_cast<int64>(Number.Digits.Len()) + Number.PowerOfTen > 20) return false;
			// At most the decimal width of uint64 is materialized. This is a native
			// representability check, not a bound on general canonical numbers.
			OutDigits = (Number.bNegative ? TEXT("-") : TEXT("")) + Number.Digits
				+ FString::ChrN(Number.PowerOfTen, TEXT('0'));
			return true;
		}
	}

	bool TryParseInt64Number(const FString& Text, int64& OutValue)
	{
		FString Digits;
		return TryIntegralNumberText(Text, Digits) && TryParseInt64Decimal(Digits, OutValue);
	}

	bool TryParseUInt64Number(const FString& Text, uint64& OutValue)
	{
		FString Digits;
		return TryIntegralNumberText(Text, Digits) && TryParseUInt64Decimal(Digits, OutValue);
	}

	bool TryParseFiniteDecimal(const FString& Text, double& OutValue)
	{
		FExactNumber Exact;
		double Native = 0.0;
		if (!ParseExactNumber(Text, Exact) || !LexTryParseString(Native, *Text) || !FMath::IsFinite(Native))
			return false;
		OutValue = Native;
		return true;
	}

	FUeAgentSemanticComparison CompareValues(
		const FUeAgentCanonicalValueRef& Left,
		const FUeAgentCanonicalValueRef& Right,
		const FString& Path,
		FUeAgentResourceTracker& Resources)
	{
		FUeAgentSemanticComparison Result;
		FUeAgentStateSemanticError Error;
		if (TryCompareValues(
			Left,
			Right,
			Path,
			Resources,
			Result,
			Error))
		{
			return Result;
		}
		return FUeAgentSemanticComparison({
			FUeAgentSemanticDifference(
				EUeAgentDifferenceKind::ValueMismatch,
				Error.Source.JsonPath,
				FUeAgentCanonicalValuePtr(),
				FUeAgentCanonicalValuePtr()),
		});
	}

	bool TryCompareValues(
		const FUeAgentCanonicalValueRef& Left,
		const FUeAgentCanonicalValueRef& Right,
		const FString& Path,
		FUeAgentResourceTracker& Resources,
		FUeAgentSemanticComparison& OutComparison,
		FUeAgentStateSemanticError& OutError)
	{
		ResetError(OutError);
		OutComparison = FUeAgentSemanticComparison();
		if (!Path.StartsWith(TEXT("/")))
		{
			Fail(OutError, TEXT("semantic_compare_path_invalid"), TEXT("The semantic comparison path must be a non-empty absolute JSON pointer."), TEXT("/comparison_path"),
				MakeShared<FJsonValueString>(TEXT("absolute JSON pointer")));
			OutError.ActualEvidence = MakeShared<FJsonValueString>(Path); return false;
		}
		TArray<FUeAgentSemanticDifference> Differences;
		if (!TryCompareValuesInternal(
			Left,
			Right,
			Path,
			Resources,
			Differences,
			OutError))
		{
			return false;
		}
		OutComparison = FUeAgentSemanticComparison(MoveTemp(Differences));
		return true;
	}

	bool TryCompareSnapshots(
		const FUeAgentCanonicalSnapshotRef& Left,
		const FUeAgentCanonicalSnapshotRef& Right,
		FUeAgentResourceTracker& Resources,
		FUeAgentSemanticComparison& OutComparison,
		FUeAgentStateSemanticError& OutError)
	{
		ResetError(OutError);
		OutComparison = FUeAgentSemanticComparison();
		TArray<FUeAgentSemanticDifference> Differences;
		if (!ChargeComparison(
			Resources,
			TEXT("/entries"),
			FMath::Max(Left->GetEntries().Num(), Right->GetEntries().Num()),
			OutError))
		{
			return false;
		}
		if (Left->GetStatePlane() != Right->GetStatePlane())
		{
			const EUeAgentStatePlane LeftPlane =
				Left->GetStatePlane();
			const EUeAgentStatePlane RightPlane =
				Right->GetStatePlane();
			if (!TryAddTypedDifference(
				Differences,
				EUeAgentDifferenceKind::StatePlaneMismatch,
				TEXT("/state_plane"),
				&LeftPlane,
				&RightPlane,
				Resources,
				OutError))
			{
				return false;
			}
		}
		const auto& LeftEntries = Left->GetEntries();
		const auto& RightEntries = Right->GetEntries();
		int32 LeftIndex = 0;
		int32 RightIndex = 0;
		while (LeftIndex < LeftEntries.Num() || RightIndex < RightEntries.Num())
		{
			if (LeftIndex >= LeftEntries.Num())
			{
				if (!TryAddTypedDifference<FUeAgentStateEntry>(
					Differences,
					EUeAgentDifferenceKind::MissingLeft,
					JsonPointerJoin(
						TEXT("/entries"),
						RightEntries[RightIndex].GetIdentity().GetCanonicalKey()),
					nullptr,
					&RightEntries[RightIndex],
					Resources,
					OutError))
				{
					return false;
				}
				++RightIndex;
				continue;
			}
			if (RightIndex >= RightEntries.Num())
			{
				if (!TryAddTypedDifference<FUeAgentStateEntry>(
					Differences,
					EUeAgentDifferenceKind::MissingRight,
					JsonPointerJoin(
						TEXT("/entries"),
						LeftEntries[LeftIndex].GetIdentity().GetCanonicalKey()),
					&LeftEntries[LeftIndex],
					nullptr,
					Resources,
					OutError))
				{
					return false;
				}
				++LeftIndex;
				continue;
			}
			const FString& LeftKey =
				LeftEntries[LeftIndex].GetIdentity().GetCanonicalKey();
			const FString& RightKey =
				RightEntries[RightIndex].GetIdentity().GetCanonicalKey();
			const int32 Comparison = LeftKey.Compare(RightKey, ESearchCase::CaseSensitive);
			if (Comparison < 0)
			{
				if (!TryAddTypedDifference<FUeAgentStateEntry>(
					Differences,
					EUeAgentDifferenceKind::MissingRight,
					JsonPointerJoin(TEXT("/entries"), LeftKey),
					&LeftEntries[LeftIndex],
					nullptr,
					Resources,
					OutError))
				{
					return false;
				}
				++LeftIndex;
				continue;
			}
			if (Comparison > 0)
			{
				if (!TryAddTypedDifference<FUeAgentStateEntry>(
					Differences,
					EUeAgentDifferenceKind::MissingLeft,
					JsonPointerJoin(TEXT("/entries"), RightKey),
					nullptr,
					&RightEntries[RightIndex],
					Resources,
					OutError))
				{
					return false;
				}
				++RightIndex;
				continue;
			}

			const FUeAgentStateEntry& LeftEntry = LeftEntries[LeftIndex++];
			const FUeAgentStateEntry& RightEntry = RightEntries[RightIndex++];
			const FString EntryPath = JsonPointerJoin(TEXT("/entries"), LeftKey);
			if (!ChargeComparison(Resources, EntryPath, 6, OutError))
			{
				return false;
			}
			if (!(LeftEntry.GetIdentity() == RightEntry.GetIdentity()))
			{
				if (!TryAddTypedDifference(
					Differences,
					EUeAgentDifferenceKind::IdentityMismatch,
					JsonPointerJoin(EntryPath, TEXT("identity")),
					&LeftEntry.GetIdentity(),
					&RightEntry.GetIdentity(),
					Resources,
					OutError))
				{
					return false;
				}
			}
			if (!(LeftEntry.GetType() == RightEntry.GetType()))
			{
				if (!TryAddTypedDifference(
					Differences,
					EUeAgentDifferenceKind::TypeMismatch,
					JsonPointerJoin(EntryPath, TEXT("type")),
					&LeftEntry.GetType(),
					&RightEntry.GetType(),
					Resources,
					OutError))
				{
					return false;
				}
			}
			if (!TryCompareValuesInternal(
				LeftEntry.GetValue(),
				RightEntry.GetValue(),
				JsonPointerJoin(EntryPath, TEXT("value")),
				Resources,
				Differences,
				OutError))
			{
				return false;
			}
			const auto& LeftDescriptor = LeftEntry.GetAuthoringDescriptor();
			const auto& RightDescriptor = RightEntry.GetAuthoringDescriptor();
			if (LeftDescriptor.IsValid() && RightDescriptor.IsValid())
			{
				if (!TryCompareValuesInternal(LeftDescriptor.ToSharedRef(), RightDescriptor.ToSharedRef(),
					JsonPointerJoin(EntryPath, TEXT("authoring_descriptor")), Resources, Differences, OutError)) return false;
			}
			else if (LeftDescriptor.IsValid() != RightDescriptor.IsValid())
			{
				Differences.Emplace(EUeAgentDifferenceKind::DetailsMismatch,
					JsonPointerJoin(EntryPath, TEXT("authoring_descriptor")), LeftDescriptor, RightDescriptor);
			}
			const FUeAgentDefaultValue& LeftDefault = LeftEntry.GetDefault();
			const FUeAgentDefaultValue& RightDefault = RightEntry.GetDefault();
			if (LeftDefault.GetSourceKind() != RightDefault.GetSourceKind()
				|| LeftDefault.GetSourceIdentity() != RightDefault.GetSourceIdentity()
				|| LeftDefault.GetProvenance() != RightDefault.GetProvenance())
			{
				if (!TryAddTypedDifference(
					Differences,
					EUeAgentDifferenceKind::DefaultMismatch,
					JsonPointerJoin(EntryPath, TEXT("default")),
					&LeftDefault,
					&RightDefault,
					Resources,
					OutError))
				{
					return false;
				}
			}
			else
			{
				TArray<FUeAgentSemanticDifference> DefaultDifferences;
				if (!TryCompareValuesInternal(
					LeftDefault.GetValue(),
					RightDefault.GetValue(),
					JsonPointerJoin(
						JsonPointerJoin(EntryPath, TEXT("default")),
						TEXT("value")),
					Resources,
					DefaultDifferences,
					OutError))
				{
					return false;
				}
				if (!DefaultDifferences.IsEmpty())
				{
					if (!TryAddTypedDifference(
						Differences,
						EUeAgentDifferenceKind::DefaultMismatch,
						JsonPointerJoin(EntryPath, TEXT("default")),
						&LeftDefault,
						&RightDefault,
						Resources,
						OutError))
					{
						return false;
					}
				}
			}
			if (!(LeftEntry.GetDetails() == RightEntry.GetDetails()))
			{
				if (!TryAddTypedDifference(
					Differences,
					EUeAgentDifferenceKind::DetailsMismatch,
					JsonPointerJoin(EntryPath, TEXT("details")),
					&LeftEntry.GetDetails(),
					&RightEntry.GetDetails(),
					Resources,
					OutError))
				{
					return false;
				}
			}
			if (LeftEntry.GetAuthoredPresence()
				!= RightEntry.GetAuthoredPresence())
			{
				const EUeAgentAuthoredPresence LeftPresence =
					LeftEntry.GetAuthoredPresence();
				const EUeAgentAuthoredPresence RightPresence =
					RightEntry.GetAuthoredPresence();
				if (!TryAddTypedDifference(
					Differences,
					EUeAgentDifferenceKind::AuthoredPresenceMismatch,
					JsonPointerJoin(EntryPath, TEXT("authored_presence")),
					&LeftPresence,
					&RightPresence,
					Resources,
					OutError))
				{
					return false;
				}
			}
			if (LeftEntry.GetOverrideState() != RightEntry.GetOverrideState())
			{
				const EUeAgentOverrideState LeftOverride =
					LeftEntry.GetOverrideState();
				const EUeAgentOverrideState RightOverride =
					RightEntry.GetOverrideState();
				if (!TryAddTypedDifference(
					Differences,
					EUeAgentDifferenceKind::OverrideStateMismatch,
					JsonPointerJoin(EntryPath, TEXT("override_state")),
					&LeftOverride,
					&RightOverride,
					Resources,
					OutError))
				{
					return false;
				}
			}
		}
		TMap<FString, int32> EntryAliases;
		for (auto& Difference : Differences)
		{
			const FString Prefix = TEXT("/entries/");
			const FString& Path = Difference.GetPath();
			if (!Path.StartsWith(Prefix, ESearchCase::CaseSensitive)) continue;
			const int32 Suffix = Path.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Prefix.Len());
			const FString Key = Suffix == INDEX_NONE ? Path.Mid(Prefix.Len()) : Path.Mid(Prefix.Len(), Suffix - Prefix.Len());
			const int32 Index = EntryAliases.FindOrAdd(Key, EntryAliases.Num());
			Difference.SetPublicPath(FString::Printf(TEXT("/entries/%d"), Index)
				+ (Suffix == INDEX_NONE ? FString() : Path.Mid(Suffix)));
		}
		OutComparison = FUeAgentSemanticComparison(MoveTemp(Differences));
		return true;
	}

	FUeAgentSemanticComparison CompareSnapshots(
		const FUeAgentCanonicalSnapshotRef& Left,
		const FUeAgentCanonicalSnapshotRef& Right,
		FUeAgentResourceTracker& Resources)
	{
		FUeAgentSemanticComparison Result;
		FUeAgentStateSemanticError Error;
		if (TryCompareSnapshots(Left, Right, Resources, Result, Error))
		{
			return Result;
		}
		return FUeAgentSemanticComparison({
			FUeAgentSemanticDifference(
				EUeAgentDifferenceKind::ValueMismatch,
				Error.Source.JsonPath,
				FUeAgentCanonicalValuePtr(),
				FUeAgentCanonicalValuePtr()),
		});
	}

	bool TryConvertSpatialValue(
		const FUeAgentSpatialValue& Value,
		const FUeAgentSpatialFrame& TargetFrame,
		const FUeAgentSpatialConversion& Conversion,
		FUeAgentResourceTracker& Resources,
		FUeAgentSpatialValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError)
	{
		ResetError(OutError);
		OutValue.Reset();
		if (!Value.IsValid() || !TargetFrame.IsValid() || !Conversion.IsValid())
		{
			const auto& Cause = !Value.IsValid() ? Value.GetValidationDiagnostic() : !TargetFrame.IsValid() ? TargetFrame.GetValidationDiagnostic() : Conversion.GetValidationDiagnostic();
			OutError = FUeAgentStateSemanticError::WrapFailure(TEXT("spatial_contract_invalid"), Cause, TEXT("state_semantic"));
			const FString Input = !Value.IsValid() ? TEXT("value") : !TargetFrame.IsValid() ? TEXT("target_frame") : TEXT("conversion");
			const auto PrefixInput = [&Input](auto&& Self, FUeAgentStateSemanticError& Error) -> void {
				Error.PropertyPath = Input + (Error.PropertyPath.IsEmpty() ? FString() : TEXT(".") + Error.PropertyPath);
				if (Error.Source.SourceFile.IsEmpty()) {
					const auto PrefixPath = [&Input](const FString& Path) {
						return TEXT("/") + Input + (Path.IsEmpty() || Path == TEXT("/") ? FString() : (Path.StartsWith(TEXT("/")) ? Path : TEXT("/") + Path));
					};
					Error.Source.JsonPath = PrefixPath(Error.Source.JsonPath);
					if (!Error.Source.InternalJsonPath.IsEmpty()) Error.Source.InternalJsonPath = PrefixPath(Error.Source.InternalJsonPath);
					Error.SemanticPointer = PrefixPath(Error.SemanticPointer);
				}
				for (auto& Child : Error.Diagnostics) Self(Self, Child);
				if (Error.PublicCause) { auto Nested = MakeShared<FUeAgentStateSemanticError>(*Error.PublicCause); Self(Self, *Nested); Error.PublicCause = MoveTemp(Nested); }
			};
			PrefixInput(PrefixInput, OutError);
			return false;
		}
		const int32 ItemCount = Value.GetComponents().Num()
			+ Conversion.GetSourceDimension() * Conversion.GetTargetDimension()
			+ Conversion.GetTranslation().Num();
		if (!Charge(Resources, {
			{EUeAgentResourceDimension::Nodes, 1},
			{EUeAgentResourceDimension::Items, ItemCount},
			{EUeAgentResourceDimension::WorkUnits,
				ItemCount + FMath::Cube(Conversion.GetSourceDimension())},
			{EUeAgentResourceDimension::EstimatedMemoryBytes, 64 + ItemCount * 32},
		}, TEXT("/spatial_conversion"), OutError))
		{
			return false;
		}
		if (Value.GetFrame().GetFrameId() != Conversion.GetFromFrame())
		{
			Fail(OutError, TEXT("spatial_conversion_source_frame_mismatch"), TEXT("The value frame does not match the conversion source frame."), TEXT("/spatial_conversion/from_frame"));
			OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Value.GetFrame().GetFrameId());
			OutError.ActualEvidence = MakeShared<FJsonValueString>(Conversion.GetFromFrame()); return false;
		}
		if (TargetFrame.GetFrameId() != Conversion.GetToFrame())
		{
			Fail(OutError, TEXT("spatial_conversion_target_frame_mismatch"), TEXT("The target frame does not match the conversion target frame."), TEXT("/spatial_conversion/to_frame"));
			OutError.ExpectedEvidence = MakeShared<FJsonValueString>(TargetFrame.GetFrameId());
			OutError.ActualEvidence = MakeShared<FJsonValueString>(Conversion.GetToFrame()); return false;
		}
		if (Value.GetFrame().GetDimension() != Conversion.GetSourceDimension()
			|| TargetFrame.GetDimension() != Conversion.GetTargetDimension())
		{
			const bool bSource = Value.GetFrame().GetDimension() != Conversion.GetSourceDimension();
			return Fail(OutError, TEXT("spatial_conversion_dimension_mismatch"), TEXT("The conversion dimension differs from its corresponding frame dimension."),
				bSource ? TEXT("/spatial_conversion/source_dimension") : TEXT("/spatial_conversion/target_dimension"),
				MakeShared<FJsonValueNumber>(bSource ? Value.GetFrame().GetDimension() : TargetFrame.GetDimension()),
				MakeShared<FJsonValueNumber>(bSource ? Conversion.GetSourceDimension() : Conversion.GetTargetDimension()));
		}
		if (!AreTemporalPointsEquivalent(Value.GetObservedAt(), Conversion.GetValidAt())
			|| !AreTemporalPointsEquivalent(
				Value.GetFrame().GetObservedAt(),
				Conversion.GetValidAt())
			|| !AreTemporalPointsEquivalent(
				TargetFrame.GetObservedAt(),
				Conversion.GetValidAt()))
		{
			const bool bValue = !AreTemporalPointsEquivalent(Value.GetObservedAt(), Conversion.GetValidAt());
			const bool bSourceFrame = !bValue && !AreTemporalPointsEquivalent(Value.GetFrame().GetObservedAt(), Conversion.GetValidAt());
			const auto& Actual = bValue ? Value.GetObservedAt() : bSourceFrame ? Value.GetFrame().GetObservedAt() : TargetFrame.GetObservedAt();
			Fail(OutError, TEXT("spatial_conversion_time_mismatch"), Actual.GetClockId() != Conversion.GetValidAt().GetClockId()
				? TEXT("The spatial observation and conversion validity use different clocks.") : TEXT("The spatial observation time or state differs from the conversion validity."),
				bValue ? TEXT("/value/observed_at") : bSourceFrame ? TEXT("/source_frame/observed_at") : TEXT("/target_frame/observed_at"),
				TemporalObservation(Conversion.GetValidAt()), TemporalObservation(Actual));
			OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Conversion.GetValidAt().GetClockId());
			OutError.ActualEvidence = MakeShared<FJsonValueString>(Actual.GetClockId()); return false;
		}
		if (Value.GetFrame().GetBasisFrameId() != TargetFrame.GetBasisFrameId())
		{
			Fail(OutError, TEXT("spatial_frame_reference_mismatch"), TEXT("The source and target frames do not share the same basis frame."), TEXT("/spatial_conversion/frame"));
			OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Value.GetFrame().GetBasisFrameId());
			OutError.ActualEvidence = MakeShared<FJsonValueString>(TargetFrame.GetBasisFrameId()); return false;
		}

		int64 ExpandedDigits = 0;
		int64 MaxExpandedDigits = 0;
		if (!AccumulateRationalExpansionCost(
				Value.GetFrame().GetBasis(),
				ExpandedDigits,
				MaxExpandedDigits)
			|| !AccumulateRationalExpansionCost(
				TargetFrame.GetBasis(),
				ExpandedDigits,
				MaxExpandedDigits)
			|| !AccumulateRationalExpansionCost(
				Conversion.GetLinear(),
				ExpandedDigits,
				MaxExpandedDigits)
			|| !AccumulateRationalExpansionCost(
				Value.GetFrame().GetOrigin(),
				ExpandedDigits,
				MaxExpandedDigits)
			|| !AccumulateRationalExpansionCost(
				TargetFrame.GetOrigin(),
				ExpandedDigits,
				MaxExpandedDigits)
			|| !AccumulateRationalExpansionCost(
				Conversion.GetTranslation(),
				ExpandedDigits,
				MaxExpandedDigits)
			|| !AccumulateRationalExpansionCost(
				Value.GetComponents(),
				ExpandedDigits,
				MaxExpandedDigits))
		{
			return Fail(OutError, TEXT("spatial_numeric_lexeme_invalid"), TEXT("Validated spatial numeric data could not be expanded for exact arithmetic accounting."), TEXT("/spatial_conversion"));
		}
		const int64 ArithmeticWork = SaturatingAdd(
			ExpandedDigits,
			SaturatingMultiply(MaxExpandedDigits, MaxExpandedDigits));
		const int64 ArithmeticMemory =
			SaturatingMultiply(ExpandedDigits, 64);
		if (!Charge(Resources, {
			{EUeAgentResourceDimension::WorkUnits, ArithmeticWork},
			{EUeAgentResourceDimension::EstimatedMemoryBytes, ArithmeticMemory},
		}, TEXT("/spatial_conversion/numeric"), OutError))
		{
			return false;
		}

		FExactMatrix SourceBasisAxes;
		FExactMatrix TargetBasisAxes;
		FExactMatrix Linear;
		FExactVector SourceOrigin;
		FExactVector TargetOrigin;
		FExactVector Translation;
		FExactVector Components;
		if (!ParseMatrix(Value.GetFrame().GetBasis(), SourceBasisAxes)
			|| !ParseMatrix(TargetFrame.GetBasis(), TargetBasisAxes)
			|| !ParseMatrix(Conversion.GetLinear(), Linear)
			|| !ParseVector(Value.GetFrame().GetOrigin(), SourceOrigin)
			|| !ParseVector(TargetFrame.GetOrigin(), TargetOrigin)
			|| !ParseVector(Conversion.GetTranslation(), Translation)
			|| !ParseVector(Value.GetComponents(), Components))
		{
			return Fail(OutError, TEXT("spatial_numeric_lexeme_invalid"), TEXT("Validated spatial numeric data could not be parsed into exact rational matrices and vectors."), TEXT("/spatial_conversion"));
		}

		const FExactMatrix SourceBasisRows = Transpose(SourceBasisAxes);
		const FExactMatrix TargetBasisRows = Transpose(TargetBasisAxes);
		FExactMatrix TargetInverse;
		FExactMatrix ExpectedLinear;
		if (!InvertMatrix(TargetBasisRows, TargetInverse, TEXT("/target_frame/basis"), OutError)) {
			OutError = FUeAgentStateSemanticError::WrapFailure(TEXT("spatial_frame_matrix_invalid"), OutError, TEXT("state_semantic")); return false;
		}
		if (!MultiplyMatrices(TargetInverse, SourceBasisRows, ExpectedLinear))
			return Fail(OutError, TEXT("spatial_frame_matrix_invalid"), TEXT("The source and inverse target basis matrix dimensions cannot be multiplied."), TEXT("/spatial_conversion/frame"),
				MakeShared<FJsonValueNumber>(TargetInverse.IsEmpty() ? 0 : TargetInverse[0].Num()), MakeShared<FJsonValueNumber>(SourceBasisRows.Num()));
		FExactVector OriginDelta;
		OriginDelta.SetNum(SourceOrigin.Num());
		for (int32 Index = 0; Index < SourceOrigin.Num(); ++Index)
		{
			OriginDelta[Index] = SubtractRational(SourceOrigin[Index], TargetOrigin[Index]);
		}
		FExactVector ExpectedTranslation;
		if (!MultiplyMatrixVector(TargetInverse, OriginDelta, ExpectedTranslation))
			return Fail(OutError, TEXT("spatial_conversion_frame_contract_mismatch"), TEXT("The origin delta length cannot be multiplied by the inverse target basis."), TEXT("/spatial_conversion/translation"),
				MakeShared<FJsonValueNumber>(TargetInverse.IsEmpty() ? 0 : TargetInverse[0].Num()), MakeShared<FJsonValueNumber>(OriginDelta.Num()));
		if (!MatchMatrix(ExpectedLinear, Linear, TEXT("/spatial_conversion/linear"), OutError)
			|| !MatchVector(ExpectedTranslation, Translation, TEXT("/spatial_conversion/translation"), OutError)) return false;

		FExactVector ConvertedVector;
		FExactMatrix ConvertedMatrix;
		TArray<FString> OutputComponents;
		switch (Value.GetQuantityKind())
		{
		case EUeAgentSpatialQuantityKind::Point:
			if (!MultiplyMatrixVector(Linear, Components, ConvertedVector))
			{
				return Fail(OutError, TEXT("spatial_matrix_vector_shape_mismatch"), TEXT("The conversion matrix column count does not match the point component count."), TEXT("/value/components"), MakeShared<FJsonValueNumber>(Linear.IsEmpty() ? 0 : Linear[0].Num()), MakeShared<FJsonValueNumber>(Components.Num()));
			}
			for (int32 Index = 0; Index < ConvertedVector.Num(); ++Index)
			{
				ConvertedVector[Index] = AddRational(ConvertedVector[Index], Translation[Index]);
			}
			if (!VectorToLexemes(ConvertedVector, OutputComponents, TEXT("/result/components"), OutError)) return false;
			break;
		case EUeAgentSpatialQuantityKind::Direction:
			if (!MultiplyMatrixVector(Linear, Components, ConvertedVector))
				return Fail(OutError, TEXT("spatial_direction_conversion_failed"), TEXT("The conversion matrix column count does not match the direction component count."), TEXT("/value/components"),
					MakeShared<FJsonValueNumber>(Linear.IsEmpty() ? 0 : Linear[0].Num()), MakeShared<FJsonValueNumber>(Components.Num()));
			if (!VectorToLexemes(ConvertedVector, OutputComponents, TEXT("/result/components"), OutError)) {
				OutError = FUeAgentStateSemanticError::WrapFailure(TEXT("spatial_direction_conversion_failed"), OutError, TEXT("state_semantic")); return false;
			}
			break;
		case EUeAgentSpatialQuantityKind::Normal:
			{
				FExactMatrix LinearInverse;
				if (!InvertMatrix(Linear, LinearInverse, TEXT("/spatial_conversion/linear"), OutError)) {
					OutError = FUeAgentStateSemanticError::WrapFailure(TEXT("spatial_normal_conversion_failed"), OutError, TEXT("state_semantic")); return false;
				}
				if (!MultiplyMatrixVector(Transpose(LinearInverse), Components, ConvertedVector))
					return Fail(OutError, TEXT("spatial_normal_conversion_failed"), TEXT("The inverse-transpose matrix dimension does not match the normal component count."), TEXT("/value/components"),
						MakeShared<FJsonValueNumber>(LinearInverse.Num()), MakeShared<FJsonValueNumber>(Components.Num()));
				if (!VectorToLexemes(ConvertedVector, OutputComponents, TEXT("/result/components"), OutError)) {
					OutError = FUeAgentStateSemanticError::WrapFailure(TEXT("spatial_normal_conversion_failed"), OutError, TEXT("state_semantic")); return false;
				}
				break;
			}
		case EUeAgentSpatialQuantityKind::Transform:
			{
				const int32 Size = Value.GetFrame().GetDimension() + 1;
				FExactMatrix SourceTransform;
				SourceTransform.SetNum(Size);
				for (int32 Row = 0; Row < Size; ++Row)
				{
					SourceTransform[Row].SetNum(Size);
					for (int32 Column = 0; Column < Size; ++Column)
					{
						SourceTransform[Row][Column] = Components[Row * Size + Column];
					}
				}
				FExactMatrix Homogeneous;
				Homogeneous.SetNum(Size);
				for (int32 Row = 0; Row < Size; ++Row)
				{
					Homogeneous[Row].SetNum(Size);
					for (int32 Column = 0; Column < Size; ++Column)
					{
						if (Row < Size - 1 && Column < Size - 1)
						{
							Homogeneous[Row][Column] = Linear[Row][Column];
						}
						else if (Row < Size - 1 && Column == Size - 1)
						{
							Homogeneous[Row][Column] = Translation[Row];
						}
						else
						{
							Homogeneous[Row][Column] =
								Column == Size - 1 ? OneRational() : ZeroRational();
						}
					}
				}
				if (!MultiplyMatrices(Homogeneous, SourceTransform, ConvertedMatrix))
					return Fail(OutError, TEXT("spatial_transform_conversion_failed"), TEXT("The homogeneous conversion and source transform matrix dimensions do not agree."), TEXT("/value/components"),
						MakeShared<FJsonValueNumber>(Homogeneous.IsEmpty() ? 0 : Homogeneous[0].Num()), MakeShared<FJsonValueNumber>(SourceTransform.Num()));
				if (!MatrixToLexemes(ConvertedMatrix, OutputComponents, TEXT("/result/components"), OutError)) {
					OutError = FUeAgentStateSemanticError::WrapFailure(TEXT("spatial_transform_conversion_failed"), OutError, TEXT("state_semantic")); return false;
				}
				break;
			}
		default:
			return Fail(OutError, TEXT("spatial_quantity_kind_unknown"), TEXT("The spatial quantity kind has no conversion rule."), TEXT("/value/quantity_kind"), MakeShared<FJsonValueString>(TEXT("point, direction, normal or transform")), MakeShared<FJsonValueNumber>(static_cast<int32>(Value.GetQuantityKind())));
		}

		TArray<FString> Evidence = Value.GetConversionEvidence();
		Evidence.Add(Conversion.GetConversionId());
		Evidence.Add(Conversion.GetEvidence());
		TSharedPtr<FUeAgentSpatialValue> Result = MakeShared<FUeAgentSpatialValue>(
			Value.GetSubject(),
			Value.GetQuantityKind(),
			TargetFrame,
			MoveTemp(OutputComponents),
			Value.GetObservedAt(),
			MoveTemp(Evidence));
		if (!Result->IsValid())
		{
			OutError = FUeAgentStateSemanticError::WrapFailure(TEXT("spatial_converted_value_invalid"), Result->GetValidationDiagnostic(), TEXT("state_semantic")); return false;
		}
		OutValue = Result;
		return true;
	}
}

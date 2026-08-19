#include "Core/StateSemantic/UeAgentStateSemantic.h"

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
			const FString& Path = FString(),
			const FString& Detail = FString())
		{
			OutError.Code = Code;
			OutError.Path = Path;
			OutError.Detail = Detail;
			return false;
		}

		bool Charge(
			FUeAgentResourceTracker& Resources,
			const TMap<EUeAgentResourceDimension, int64>& Amounts,
			const FString& Path,
			FUeAgentStateSemanticError& OutError)
		{
			FUeAgentResourceBudgetExceeded BudgetError;
			if (Resources.TryChargeMany(Amounts, Path, BudgetError))
			{
				return true;
			}
			OutError.Code = BudgetError.Code;
			OutError.Path = BudgetError.Path;
			OutError.ResourceDimension = BudgetError.Dimension;
			OutError.ResourceLimit = BudgetError.Limit;
			OutError.ResourceObserved = BudgetError.Observed;
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

		bool InvertMatrix(
			const FExactMatrix& Matrix,
			FExactMatrix& OutInverse)
		{
			const int32 Size = Matrix.Num();
			if (Size == 0)
			{
				return false;
			}
			FExactMatrix Augmented;
			Augmented.SetNum(Size);
			const FExactMatrix Identity = IdentityMatrix(Size);
			for (int32 Row = 0; Row < Size; ++Row)
			{
				if (Matrix[Row].Num() != Size)
				{
					return false;
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
					return false;
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
						return false;
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

		bool VectorsEqual(
			const FExactVector& Left,
			const FExactVector& Right)
		{
			if (Left.Num() != Right.Num())
			{
				return false;
			}
			for (int32 Index = 0; Index < Left.Num(); ++Index)
			{
				if (!RationalsEqual(Left[Index], Right[Index]))
				{
					return false;
				}
			}
			return true;
		}

		bool MatricesEqual(
			const FExactMatrix& Left,
			const FExactMatrix& Right)
		{
			if (Left.Num() != Right.Num())
			{
				return false;
			}
			for (int32 Row = 0; Row < Left.Num(); ++Row)
			{
				if (!VectorsEqual(Left[Row], Right[Row]))
				{
					return false;
				}
			}
			return true;
		}

		bool MatrixToLexemes(
			const FExactMatrix& Matrix,
			TArray<FString>& OutLexemes)
		{
			OutLexemes.Reset();
			for (const FExactVector& Row : Matrix)
			{
				for (const FExactRational& Value : Row)
				{
					FString Lexeme;
					if (!RationalToDecimalLexeme(Value, Lexeme))
					{
						return false;
					}
					OutLexemes.Add(MoveTemp(Lexeme));
				}
			}
			return true;
		}

		bool VectorToLexemes(
			const FExactVector& Vector,
			TArray<FString>& OutLexemes)
		{
			OutLexemes.Reset();
			for (const FExactRational& Value : Vector)
			{
				FString Lexeme;
				if (!RationalToDecimalLexeme(Value, Lexeme))
				{
					return false;
				}
				OutLexemes.Add(MoveTemp(Lexeme));
			}
			return true;
		}

		bool IsSquareExactLexemeMatrix(
			const TArray<TArray<FString>>& Matrix,
			int32 Dimension)
		{
			if (Matrix.Num() != Dimension)
			{
				return false;
			}
			for (const TArray<FString>& Row : Matrix)
			{
				if (Row.Num() != Dimension)
				{
					return false;
				}
				for (const FString& Lexeme : Row)
				{
					FExactNumber Parsed;
					if (!ParseExactNumber(Lexeme, Parsed))
					{
						return false;
					}
				}
			}
			return true;
		}

		bool IsExactLexemeVector(const TArray<FString>& Vector)
		{
			for (const FString& Lexeme : Vector)
			{
				FExactNumber Parsed;
				if (!ParseExactNumber(Lexeme, Parsed))
				{
					return false;
				}
			}
			return true;
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

		void AddDifference(
			TArray<FUeAgentSemanticDifference>& Differences,
			EUeAgentDifferenceKind Kind,
			const FString& Path,
			FUeAgentCanonicalValuePtr Left =
				FUeAgentCanonicalValuePtr(),
			FUeAgentCanonicalValuePtr Right =
				FUeAgentCanonicalValuePtr())
		{
			Differences.Emplace(
				Kind,
				Path,
				MoveTemp(Left),
				MoveTemp(Right));
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
			AddDifference(
				Differences,
				Kind,
				Path,
				MoveTemp(LeftValue),
				MoveTemp(RightValue));
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
				if (Task.Left->GetKind() != Task.Right->GetKind())
				{
					if (!ChargeComparison(Resources, Task.Path, 0, OutError))
					{
						return false;
					}
					AddDifference(
						OutDifferences,
						EUeAgentDifferenceKind::TypeMismatch,
						Task.Path,
						FUeAgentCanonicalValuePtr(Task.Left),
						FUeAgentCanonicalValuePtr(Task.Right));
					continue;
				}

				const EUeAgentValueKind Kind = Task.Left->GetKind();
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
						AddDifference(
							OutDifferences,
							EUeAgentDifferenceKind::MissingRight,
							JsonPointerJoin(Task.Path, Index),
							FUeAgentCanonicalValuePtr(LeftItems[Index]));
					}
					for (int32 Index = Common; Index < RightItems.Num(); ++Index)
					{
						AddDifference(
							OutDifferences,
							EUeAgentDifferenceKind::MissingLeft,
							JsonPointerJoin(Task.Path, Index),
							FUeAgentCanonicalValuePtr(),
							FUeAgentCanonicalValuePtr(RightItems[Index]));
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
							AddDifference(
								OutDifferences,
								EUeAgentDifferenceKind::MissingLeft,
								JsonPointerJoin(Task.Path, RightItems[RightIndex].Key),
								FUeAgentCanonicalValuePtr(),
								FUeAgentCanonicalValuePtr(
									RightItems[RightIndex].Value));
							++RightIndex;
							continue;
						}
						if (RightIndex >= RightItems.Num())
						{
							AddDifference(
								OutDifferences,
								EUeAgentDifferenceKind::MissingRight,
								JsonPointerJoin(Task.Path, LeftItems[LeftIndex].Key),
								FUeAgentCanonicalValuePtr(
									LeftItems[LeftIndex].Value));
							++LeftIndex;
							continue;
						}
						const int32 Comparison = LeftItems[LeftIndex].Key.Compare(
							RightItems[RightIndex].Key,
							ESearchCase::CaseSensitive);
						if (Comparison < 0)
						{
							AddDifference(
								OutDifferences,
								EUeAgentDifferenceKind::MissingRight,
								JsonPointerJoin(Task.Path, LeftItems[LeftIndex].Key),
								FUeAgentCanonicalValuePtr(
									LeftItems[LeftIndex].Value));
							++LeftIndex;
						}
						else if (Comparison > 0)
						{
							AddDifference(
								OutDifferences,
								EUeAgentDifferenceKind::MissingLeft,
								JsonPointerJoin(Task.Path, RightItems[RightIndex].Key),
								FUeAgentCanonicalValuePtr(),
								FUeAgentCanonicalValuePtr(
									RightItems[RightIndex].Value));
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
						AddDifference(
							OutDifferences,
							EUeAgentDifferenceKind::ValueMismatch,
							Task.Path,
							FUeAgentCanonicalValuePtr(Task.Left),
							FUeAgentCanonicalValuePtr(Task.Right));
					}
					break;
				case EUeAgentValueKind::String:
					if (Task.Left->GetText() != Task.Right->GetText())
					{
						AddDifference(
							OutDifferences,
							EUeAgentDifferenceKind::ValueMismatch,
							Task.Path,
							FUeAgentCanonicalValuePtr(Task.Left),
							FUeAgentCanonicalValuePtr(Task.Right));
					}
					break;
				case EUeAgentValueKind::Integer:
				case EUeAgentValueKind::Decimal:
					if (!ExactNumbersEqual(Task.Left->GetLexeme(), Task.Right->GetLexeme()))
					{
						AddDifference(
							OutDifferences,
							EUeAgentDifferenceKind::ValueMismatch,
							Task.Path,
							FUeAgentCanonicalValuePtr(Task.Left),
							FUeAgentCanonicalValuePtr(Task.Right));
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
		FExactNumber Numerator;
		FExactNumber Denominator;
		bValid = IsNonEmpty(ClockId)
			&& IsCanonicalIntegerLexeme(Tick)
			&& IsCanonicalIntegerLexeme(RateNumerator)
			&& IsCanonicalIntegerLexeme(RateDenominator)
			&& ParseExactNumber(RateNumerator, Numerator)
			&& ParseExactNumber(RateDenominator, Denominator)
			&& !Numerator.bNegative
			&& Numerator.Digits != TEXT("0")
			&& !Denominator.bNegative
			&& Denominator.Digits != TEXT("0")
			&& IsNonEmpty(State);
	}

	const FString& FUeAgentTemporalPoint::GetClockId() const { return ClockId; }
	const FString& FUeAgentTemporalPoint::GetTick() const { return Tick; }
	const FString& FUeAgentTemporalPoint::GetRateNumerator() const { return RateNumerator; }
	const FString& FUeAgentTemporalPoint::GetRateDenominator() const { return RateDenominator; }
	const FString& FUeAgentTemporalPoint::GetState() const { return State; }
	bool FUeAgentTemporalPoint::IsValid() const { return bValid; }

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
		bValid = IsNonEmpty(FrameId)
			&& IsNonEmpty(BasisFrameId)
			&& (Dimension == 2 || Dimension == 3)
			&& IsSquareExactLexemeMatrix(Basis, Dimension)
			&& Origin.Num() == Dimension
			&& IsExactLexemeVector(Origin)
			&& IsNonEmpty(Unit)
			&& IsNonEmpty(BasisEvidence)
			&& ObservedAt.IsValid();
	}

	const FString& FUeAgentSpatialFrame::GetFrameId() const { return FrameId; }
	const FString& FUeAgentSpatialFrame::GetBasisFrameId() const { return BasisFrameId; }
	int32 FUeAgentSpatialFrame::GetDimension() const { return Dimension; }
	const TArray<TArray<FString>>& FUeAgentSpatialFrame::GetBasis() const { return Basis; }
	const TArray<FString>& FUeAgentSpatialFrame::GetOrigin() const { return Origin; }
	const FString& FUeAgentSpatialFrame::GetUnit() const { return Unit; }
	const FString& FUeAgentSpatialFrame::GetBasisEvidence() const { return BasisEvidence; }
	const FUeAgentTemporalPoint& FUeAgentSpatialFrame::GetObservedAt() const { return ObservedAt; }
	bool FUeAgentSpatialFrame::IsValid() const { return bValid; }

	bool FUeAgentSpatialFrame::SemanticallyEquals(
		const FUeAgentSpatialFrame& Other) const
	{
		return bValid
			&& Other.bValid
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
		bValid = IsNonEmpty(ConversionId)
			&& IsNonEmpty(FromFrame)
			&& IsNonEmpty(ToFrame)
			&& SourceDimension == TargetDimension
			&& (SourceDimension == 2 || SourceDimension == 3)
			&& IsSquareExactLexemeMatrix(Linear, SourceDimension)
			&& Translation.Num() == TargetDimension
			&& IsExactLexemeVector(Translation)
			&& ValidAt.IsValid()
			&& IsNonEmpty(Evidence);
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
	bool FUeAgentSpatialConversion::IsValid() const { return bValid; }

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
		bValid = IsNonEmpty(Subject)
			&& Frame.IsValid()
			&& ObservedAt.IsValid()
			&& AreTemporalPointsEquivalent(ObservedAt, Frame.GetObservedAt())
			&& Components.Num() == Expected
			&& IsExactLexemeVector(Components);
		for (const FString& Evidence : ConversionEvidence)
		{
			bValid = bValid && IsNonEmpty(Evidence);
		}
		if (bValid && QuantityKind == EUeAgentSpatialQuantityKind::Transform)
		{
			const int32 Size = Frame.GetDimension() + 1;
			for (int32 Column = 0; Column < Size; ++Column)
			{
				const FString& Value = Components[Components.Num() - Size + Column];
				const FString ExpectedValue = Column == Size - 1 ? TEXT("1") : TEXT("0");
				if (!ExactNumbersEqual(Value, ExpectedValue))
				{
					bValid = false;
					break;
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
	bool FUeAgentSpatialValue::IsValid() const { return bValid; }

	bool FUeAgentSpatialValue::SemanticallyEquals(
		const FUeAgentSpatialValue& Other) const
	{
		return bValid
			&& Other.bValid
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
			return Fail(OutError, TEXT("integer_lexeme_not_canonical"), TEXT("/value"));
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
			return Fail(OutError, TEXT("decimal_lexeme_not_finite"), TEXT("/value"));
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
		TSet<FString> Keys;
		for (const auto& Item : Items)
		{
			if (!IsNonEmpty(Item.Key))
			{
				return Fail(OutError, TEXT("object_key_empty"), TEXT("/value"));
			}
			if (Keys.Contains(Item.Key))
			{
				return Fail(
					OutError,
					TEXT("object_key_duplicate"),
					JsonPointerJoin(TEXT("/value"), Item.Key));
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
			return Fail(OutError, TEXT("temporal_value_invalid"), TEXT("/value"));
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
			return Fail(OutError, TEXT("spatial_value_invalid"), TEXT("/value"));
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
		bValid = Private::TryNormalizeNfcText(SemanticId, SemanticId)
			&& Private::TryNormalizeNfcText(TargetKind, TargetKind)
			&& Private::TryNormalizeNfcText(OwnerId, OwnerId)
			&& Private::TryNormalizeNfcText(Selector, Selector)
			&& Private::TryNormalizeNfcText(ObjectRole, ObjectRole)
			&& Private::TryNormalizeNfcText(Generation, Generation);
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
		bValid = Private::TryNormalizeNfcText(SemanticId, SemanticId)
			&& Private::TryNormalizeNfcText(TargetKind, TargetKind)
			&& Private::TryNormalizeNfcText(OwnerId, OwnerId)
			&& Private::TryNormalizeNfcText(ObjectRole, ObjectRole)
			&& Private::TryNormalizeNfcText(Generation, Generation)
			&& !Selectors.IsEmpty();
		TArray<FString> SelectorNames;
		Selectors.GetKeys(SelectorNames);
		SelectorNames.Sort();
		FString SelectorMaterial;
		TMap<FString, FString> NormalizedSelectors;
		for (const FString& Name : SelectorNames)
		{
			FString NormalizedName = Name;
			FString NormalizedValue = Selectors.FindChecked(Name);
			if (!Private::TryNormalizeNfcText(
					NormalizedName, NormalizedName)
				|| !Private::TryNormalizeNfcText(
					NormalizedValue, NormalizedValue))
			{
				bValid = false;
				break;
			}
			if (NormalizedSelectors.Contains(NormalizedName))
			{
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
	}

	const FString& FUeAgentCanonicalType::GetTypeId() const { return TypeId; }
	EUeAgentValueKind FUeAgentCanonicalType::GetValueKind() const { return ValueKind; }
	bool FUeAgentCanonicalType::IsNullable() const { return bNullable; }
	const FString& FUeAgentCanonicalType::GetContainerKind() const { return ContainerKind; }
	bool FUeAgentCanonicalType::IsValid() const { return bValid; }
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
	}

	EUeAgentDefaultSourceKind FUeAgentDefaultValue::GetSourceKind() const { return SourceKind; }
	const FString& FUeAgentDefaultValue::GetSourceIdentity() const { return SourceIdentity; }
	const FString& FUeAgentDefaultValue::GetProvenance() const { return Provenance; }
	const FUeAgentCanonicalValueRef& FUeAgentDefaultValue::GetValue() const { return Value; }
	bool FUeAgentDefaultValue::IsValid() const { return bValid; }

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
		bValid = IsNonEmpty(DisplayName) && IsNonEmpty(Availability);
		for (const FString& Category : CategoryPath)
		{
			bValid = bValid && IsNonEmpty(Category);
		}
		for (const auto& Constraint : Constraints)
		{
			bValid = bValid && IsNonEmpty(Constraint.Key) && IsNonEmpty(Constraint.Value);
		}
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
		bValid = IsNonEmpty(SourceKind) && IsNonEmpty(SourceId) && IsNonEmpty(EvidenceId);
	}

	const FString& FUeAgentStateProvenance::GetSourceKind() const { return SourceKind; }
	const FString& FUeAgentStateProvenance::GetSourceId() const { return SourceId; }
	const FString& FUeAgentStateProvenance::GetEvidenceId() const { return EvidenceId; }
	bool FUeAgentStateProvenance::IsValid() const { return bValid; }
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
		bValid = IsNonEmpty(SourceId) && IsNonEmpty(Generation) && Sequence >= 0;
	}

	const FString& FUeAgentStateRevision::GetSourceId() const { return SourceId; }
	const FString& FUeAgentStateRevision::GetGeneration() const { return Generation; }
	int64 FUeAgentStateRevision::GetSequence() const { return Sequence; }
	bool FUeAgentStateRevision::IsValid() const { return bValid; }
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
		EUeAgentOverrideState InOverrideState)
		: Identity(MoveTemp(InIdentity))
		, Type(MoveTemp(InType))
		, Value(MoveTemp(InValue))
		, Default(MoveTemp(InDefault))
		, Details(MoveTemp(InDetails))
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
		bValid = Identity.IsValid()
			&& Type.IsValid()
			&& Default.IsValid()
			&& Details.IsValid()
			&& ObservedRevision.IsValid()
			&& !(OverrideState == EUeAgentOverrideState::Overridden
				&& AuthoredPresence != EUeAgentAuthoredPresence::Present)
			&& !(OverrideState == EUeAgentOverrideState::Inherited
				&& AuthoredPresence != EUeAgentAuthoredPresence::Absent)
			&& MatchesType(Value, Type)
			&& MatchesType(Default.GetValue(), Type);
	}

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
			OverrideState);
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
			return Fail(OutError, TEXT("snapshot_contract_invalid"), TEXT("/snapshot"));
		}
		if (Provenance.GetSourceKind() != RequiredSourceKind)
		{
			return Fail(
				OutError,
				ProvenanceErrorCode,
				TEXT("/provenance"));
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
				return Fail(
					OutError,
					TEXT("snapshot_entry_invalid"),
					JsonPointerJoin(TEXT("/entries"), Entry.GetIdentity().GetCanonicalKey()));
			}
			const FString& Key = Entry.GetIdentity().GetCanonicalKey();
			if (Key == PreviousKey)
			{
				return Fail(
					OutError,
					TEXT("snapshot_identity_duplicate"),
					JsonPointerJoin(TEXT("/entries"), Key));
			}
			PreviousKey = Key;
			if (Entry.GetObservedRevision() != Revision)
			{
				return Fail(
					OutError,
					TEXT("snapshot_revision_mismatch"),
					TEXT("/revision"),
					Key);
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
		EUeAgentDifferenceKind InKind,
		FString InPath,
		FUeAgentCanonicalValuePtr InLeft,
		FUeAgentCanonicalValuePtr InRight)
		: Kind(InKind)
		, Path(MoveTemp(InPath))
		, Left(MoveTemp(InLeft))
		, Right(MoveTemp(InRight))
	{
	}

	EUeAgentDifferenceKind FUeAgentSemanticDifference::GetKind() const { return Kind; }
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
				Error.Path,
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
			return Fail(OutError, TEXT("semantic_compare_path_invalid"), Path);
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
				Error.Path,
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
			return Fail(OutError, TEXT("spatial_contract_invalid"), TEXT("/spatial_conversion"));
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
			return Fail(
				OutError,
				TEXT("spatial_conversion_source_frame_mismatch"),
				TEXT("/spatial_conversion/from_frame"));
		}
		if (TargetFrame.GetFrameId() != Conversion.GetToFrame())
		{
			return Fail(
				OutError,
				TEXT("spatial_conversion_target_frame_mismatch"),
				TEXT("/spatial_conversion/to_frame"));
		}
		if (Value.GetFrame().GetDimension() != Conversion.GetSourceDimension()
			|| TargetFrame.GetDimension() != Conversion.GetTargetDimension())
		{
			return Fail(
				OutError,
				TEXT("spatial_conversion_dimension_mismatch"),
				TEXT("/spatial_conversion/dimension"));
		}
		if (!AreTemporalPointsEquivalent(Value.GetObservedAt(), Conversion.GetValidAt())
			|| !AreTemporalPointsEquivalent(
				Value.GetFrame().GetObservedAt(),
				Conversion.GetValidAt())
			|| !AreTemporalPointsEquivalent(
				TargetFrame.GetObservedAt(),
				Conversion.GetValidAt()))
		{
			return Fail(
				OutError,
				TEXT("spatial_conversion_time_mismatch"),
				TEXT("/spatial_conversion/time"));
		}
		if (Value.GetFrame().GetBasisFrameId() != TargetFrame.GetBasisFrameId())
		{
			return Fail(
				OutError,
				TEXT("spatial_frame_reference_mismatch"),
				TEXT("/spatial_conversion/frame"));
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
			return Fail(
				OutError,
				TEXT("spatial_numeric_lexeme_invalid"),
				TEXT("/spatial_conversion"));
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
			return Fail(
				OutError,
				TEXT("spatial_numeric_lexeme_invalid"),
				TEXT("/spatial_conversion"));
		}

		const FExactMatrix SourceBasisRows = Transpose(SourceBasisAxes);
		const FExactMatrix TargetBasisRows = Transpose(TargetBasisAxes);
		FExactMatrix TargetInverse;
		FExactMatrix ExpectedLinear;
		if (!InvertMatrix(TargetBasisRows, TargetInverse)
			|| !MultiplyMatrices(TargetInverse, SourceBasisRows, ExpectedLinear))
		{
			return Fail(
				OutError,
				TEXT("spatial_frame_matrix_invalid"),
				TEXT("/spatial_conversion/frame"));
		}
		FExactVector OriginDelta;
		OriginDelta.SetNum(SourceOrigin.Num());
		for (int32 Index = 0; Index < SourceOrigin.Num(); ++Index)
		{
			OriginDelta[Index] = SubtractRational(SourceOrigin[Index], TargetOrigin[Index]);
		}
		FExactVector ExpectedTranslation;
		if (!MultiplyMatrixVector(TargetInverse, OriginDelta, ExpectedTranslation)
			|| !MatricesEqual(ExpectedLinear, Linear)
			|| !VectorsEqual(ExpectedTranslation, Translation))
		{
			return Fail(
				OutError,
				TEXT("spatial_conversion_frame_contract_mismatch"),
				TEXT("/spatial_conversion"));
		}

		FExactVector ConvertedVector;
		FExactMatrix ConvertedMatrix;
		TArray<FString> OutputComponents;
		switch (Value.GetQuantityKind())
		{
		case EUeAgentSpatialQuantityKind::Point:
			if (!MultiplyMatrixVector(Linear, Components, ConvertedVector))
			{
				return Fail(OutError, TEXT("spatial_matrix_vector_shape_mismatch"));
			}
			for (int32 Index = 0; Index < ConvertedVector.Num(); ++Index)
			{
				ConvertedVector[Index] = AddRational(ConvertedVector[Index], Translation[Index]);
			}
			if (!VectorToLexemes(ConvertedVector, OutputComponents))
			{
				return Fail(OutError, TEXT("spatial_result_non_terminating_decimal"));
			}
			break;
		case EUeAgentSpatialQuantityKind::Direction:
			if (!MultiplyMatrixVector(Linear, Components, ConvertedVector)
				|| !VectorToLexemes(ConvertedVector, OutputComponents))
			{
				return Fail(OutError, TEXT("spatial_direction_conversion_failed"));
			}
			break;
		case EUeAgentSpatialQuantityKind::Normal:
			{
				FExactMatrix LinearInverse;
				if (!InvertMatrix(Linear, LinearInverse)
					|| !MultiplyMatrixVector(
						Transpose(LinearInverse),
						Components,
						ConvertedVector)
					|| !VectorToLexemes(ConvertedVector, OutputComponents))
				{
					return Fail(OutError, TEXT("spatial_normal_conversion_failed"));
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
				if (!MultiplyMatrices(Homogeneous, SourceTransform, ConvertedMatrix)
					|| !MatrixToLexemes(ConvertedMatrix, OutputComponents))
				{
					return Fail(OutError, TEXT("spatial_transform_conversion_failed"));
				}
				break;
			}
		default:
			return Fail(OutError, TEXT("spatial_quantity_kind_unknown"));
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
			return Fail(OutError, TEXT("spatial_converted_value_invalid"));
		}
		OutValue = Result;
		return true;
	}
}

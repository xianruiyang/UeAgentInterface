#include "Core/StateSemantic/UeAgentTemporalContract.h"

namespace UE::UAI::StateSemantic
{
	using namespace ContextResource;

	namespace
	{
		struct FSignedInteger
		{
			bool bNegative = false;
			FString Magnitude = TEXT("0");
		};

		struct FExactRational
		{
			FSignedInteger Numerator;
			FString Denominator = TEXT("1");
		};

		bool IsNonEmpty(const FString& Value)
		{
			return !Value.TrimStartAndEnd().IsEmpty();
		}

		bool IsCanonicalUnsignedInteger(
			const FString& Value,
			bool bAllowZero)
		{
			if (Value.IsEmpty()
				|| (Value.Len() > 1 && Value[0] == TEXT('0'))
				|| (!bAllowZero && Value == TEXT("0")))
			{
				return false;
			}
			for (const TCHAR Character : Value)
			{
				if (Character < TEXT('0') || Character > TEXT('9'))
				{
					return false;
				}
			}
			return true;
		}

		bool IsCanonicalSignedInteger(
			const FString& Value,
			bool bAllowZero)
		{
			const FString Magnitude = Value.StartsWith(TEXT("-"))
				? Value.Mid(1)
				: Value;
			return IsCanonicalUnsignedInteger(Magnitude, bAllowZero);
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

		FSignedInteger NormalizeSigned(FSignedInteger Value)
		{
			Value.Magnitude = StripLeadingZeros(MoveTemp(Value.Magnitude));
			if (Value.Magnitude == TEXT("0"))
			{
				Value.bNegative = false;
			}
			return Value;
		}

		bool ParseSigned(const FString& Value, FSignedInteger& OutValue)
		{
			if (!IsCanonicalSignedInteger(Value, true))
			{
				return false;
			}
			OutValue = NormalizeSigned({
				Value.StartsWith(TEXT("-")),
				Value.StartsWith(TEXT("-")) ? Value.Mid(1) : Value,
			});
			return true;
		}

		int32 CompareMagnitude(const FString& Left, const FString& Right)
		{
			const FString A = StripLeadingZeros(Left);
			const FString B = StripLeadingZeros(Right);
			if (A.Len() != B.Len())
			{
				return A.Len() < B.Len() ? -1 : 1;
			}
			const int32 Result = A.Compare(B, ESearchCase::CaseSensitive);
			return Result < 0 ? -1 : Result > 0 ? 1 : 0;
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
				const int32 LeftDigit =
					LeftIndex >= 0 ? Left[LeftIndex--] - TEXT('0') : 0;
				const int32 RightDigit =
					RightIndex >= 0 ? Right[RightIndex--] - TEXT('0') : 0;
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
				Reversed.AppendChar(static_cast<TCHAR>(
					TEXT('0') + Digit - Subtrahend));
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
						(A[I] - TEXT('0')) * (B[J] - TEXT('0'))
						+ Digits[Position];
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

		FSignedInteger MultiplySigned(
			const FSignedInteger& Left,
			const FSignedInteger& Right)
		{
			return NormalizeSigned({
				Left.bNegative != Right.bNegative,
				MultiplyMagnitude(Left.Magnitude, Right.Magnitude),
			});
		}

		FSignedInteger AddSigned(
			const FSignedInteger& Left,
			const FSignedInteger& Right)
		{
			if (Left.bNegative == Right.bNegative)
			{
				return NormalizeSigned({
					Left.bNegative,
					AddMagnitude(Left.Magnitude, Right.Magnitude),
				});
			}
			const int32 Compared =
				CompareMagnitude(Left.Magnitude, Right.Magnitude);
			if (Compared == 0)
			{
				return {};
			}
			return Compared > 0
				? NormalizeSigned({
					Left.bNegative,
					SubtractMagnitude(Left.Magnitude, Right.Magnitude)})
				: NormalizeSigned({
					Right.bNegative,
					SubtractMagnitude(Right.Magnitude, Left.Magnitude)});
		}

		FExactRational MultiplyRational(
			const FExactRational& Left,
			const FExactRational& Right)
		{
			return {
				MultiplySigned(Left.Numerator, Right.Numerator),
				MultiplyMagnitude(Left.Denominator, Right.Denominator),
			};
		}

		FExactRational AddRational(
			const FExactRational& Left,
			const FExactRational& Right)
		{
			return {
				AddSigned(
					MultiplySigned(
						Left.Numerator,
						{false, Right.Denominator}),
					MultiplySigned(
						Right.Numerator,
						{false, Left.Denominator})),
				MultiplyMagnitude(Left.Denominator, Right.Denominator),
			};
		}

		int32 CompareRational(
			const FExactRational& Left,
			const FExactRational& Right)
		{
			const FSignedInteger LeftCross = MultiplySigned(
				Left.Numerator,
				{false, Right.Denominator});
			const FSignedInteger RightCross = MultiplySigned(
				Right.Numerator,
				{false, Left.Denominator});
			if (LeftCross.bNegative != RightCross.bNegative)
			{
				return LeftCross.bNegative ? -1 : 1;
			}
			const int32 MagnitudeComparison = CompareMagnitude(
				LeftCross.Magnitude,
				RightCross.Magnitude);
			return LeftCross.bNegative
				? -MagnitudeComparison
				: MagnitudeComparison;
		}

		bool MakePositiveRational(
			const FString& Numerator,
			const FString& Denominator,
			FExactRational& OutValue)
		{
			if (!IsCanonicalUnsignedInteger(Numerator, false)
				|| !IsCanonicalUnsignedInteger(Denominator, false))
			{
				return false;
			}
			OutValue = {{false, Numerator}, Denominator};
			return true;
		}

		bool MakeSignedRational(
			const FString& Numerator,
			const FString& Denominator,
			FExactRational& OutValue)
		{
			FSignedInteger ParsedNumerator;
			if (!ParseSigned(Numerator, ParsedNumerator)
				|| !IsCanonicalUnsignedInteger(Denominator, false))
			{
				return false;
			}
			OutValue = {MoveTemp(ParsedNumerator), Denominator};
			return true;
		}

		bool MakePointInstant(
			const FUeAgentTemporalPoint& Point,
			FExactRational& OutInstant)
		{
			FSignedInteger Tick;
			FExactRational Rate;
			if (!Point.IsValid()
				|| !ParseSigned(Point.GetTick(), Tick)
				|| !MakePositiveRational(
					Point.GetRateNumerator(),
					Point.GetRateDenominator(),
					Rate))
			{
				return false;
			}
			OutInstant = {
				MultiplySigned(Tick, {false, Rate.Denominator}),
				Rate.Numerator.Magnitude,
			};
			return true;
		}

		bool CompareTemporalPointsExact(
			const FUeAgentTemporalPoint& Left,
			const FUeAgentTemporalPoint& Right,
			int32& OutComparison)
		{
			if (!Left.IsValid() || !Right.IsValid()
				|| Left.GetClockId() != Right.GetClockId()
				|| Left.GetState() != Right.GetState())
			{
				return false;
			}
			FExactRational LeftInstant;
			FExactRational RightInstant;
			if (!MakePointInstant(Left, LeftInstant)
				|| !MakePointInstant(Right, RightInstant))
			{
				return false;
			}
			OutComparison = CompareRational(LeftInstant, RightInstant);
			return true;
		}

		int64 SaturatingAdd(int64 Left, int64 Right)
		{
			return Right > 0 && Left > MAX_int64 - Right
				? MAX_int64
				: Left + Right;
		}

		int64 SaturatingMultiply(int64 Left, int64 Right)
		{
			if (Left <= 0 || Right <= 0)
			{
				return 0;
			}
			return Left > MAX_int64 / Right ? MAX_int64 : Left * Right;
		}

		bool ChargeTemporalWork(
			FUeAgentResourceTracker& Resources,
			const TArray<FString>& Inputs,
			const FString& Path,
			FUeAgentStateSemanticError& OutError)
		{
			int64 Units = 0;
			for (const FString& Input : Inputs)
			{
				Units = SaturatingAdd(Units, Input.Len());
			}
			const int64 Work = FMath::Max<int64>(
				1,
				SaturatingMultiply(Units, Units));
			FUeAgentResourceBudgetExceeded BudgetError;
			if (Resources.TryChargeMany(
					{{EUeAgentResourceDimension::Items, 1},
				 {EUeAgentResourceDimension::StringBytes, Units},
				 {EUeAgentResourceDimension::WorkUnits, Work},
				 {EUeAgentResourceDimension::EstimatedMemoryBytes,
					SaturatingMultiply(Units, 8)}},
					Path,
					BudgetError))
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

		bool Fail(
			FUeAgentStateSemanticError& OutError,
			FString Code,
			FString Path = FString(),
			FString Detail = FString())
		{
			OutError.Code = MoveTemp(Code);
			OutError.Path = MoveTemp(Path);
			OutError.Detail = MoveTemp(Detail);
			return false;
		}

		FString SignedToString(const FSignedInteger& Value)
		{
			return Value.bNegative && Value.Magnitude != TEXT("0")
				? TEXT("-") + Value.Magnitude
				: Value.Magnitude;
		}

		bool RoundRational(
			const FExactRational& Value,
			EUeAgentTemporalRoundingPolicy Policy,
			FString& OutInteger,
			FUeAgentStateSemanticError& OutError)
		{
			FString Quotient;
			FString Remainder;
			DivideMagnitude(
				Value.Numerator.Magnitude,
				Value.Denominator,
				Quotient,
				Remainder);
			const bool bInexact = Remainder != TEXT("0");
			if (Policy == EUeAgentTemporalRoundingPolicy::Exact && bInexact)
			{
				return Fail(
					OutError,
					TEXT("temporal_conversion_inexact"),
					TEXT("rounding"));
			}

			bool bIncrementMagnitude = false;
			if (bInexact)
			{
				switch (Policy)
				{
				case EUeAgentTemporalRoundingPolicy::Floor:
					bIncrementMagnitude = Value.Numerator.bNegative;
					break;
				case EUeAgentTemporalRoundingPolicy::Ceiling:
					bIncrementMagnitude = !Value.Numerator.bNegative;
					break;
				case EUeAgentTemporalRoundingPolicy::NearestTiesToEven:
				{
					const int32 TwiceRemainderCompared = CompareMagnitude(
						MultiplyMagnitude(Remainder, TEXT("2")),
						Value.Denominator);
					bIncrementMagnitude = TwiceRemainderCompared > 0
						|| (TwiceRemainderCompared == 0
							&& ((Quotient[Quotient.Len() - 1] - TEXT('0')) % 2 != 0));
					break;
				}
				case EUeAgentTemporalRoundingPolicy::Exact:
				default:
					break;
				}
			}
			if (bIncrementMagnitude)
			{
				Quotient = AddMagnitude(Quotient, TEXT("1"));
			}
			OutInteger = SignedToString(
				NormalizeSigned({Value.Numerator.bNegative, MoveTemp(Quotient)}));
			return true;
		}

		bool BuildConvertedPoint(
			const FUeAgentTemporalPoint& Source,
			const FUeAgentTemporalConversion& Conversion,
			EUeAgentTemporalRoundingPolicy Rounding,
			FUeAgentTemporalPoint& OutPoint,
			FUeAgentStateSemanticError& OutError)
		{
			FExactRational SourceTick;
			FExactRational SourceRate;
			FExactRational TargetRate;
			FExactRational Scale;
			FExactRational Offset;
			FSignedInteger Tick;
			if (!ParseSigned(Source.GetTick(), Tick)
				|| !MakePositiveRational(
					Conversion.GetSourceRate().GetNumerator(),
					Conversion.GetSourceRate().GetDenominator(),
					SourceRate)
				|| !MakePositiveRational(
					Conversion.GetTargetRate().GetNumerator(),
					Conversion.GetTargetRate().GetDenominator(),
					TargetRate)
				|| !MakeSignedRational(
					Conversion.GetScaleNumerator(),
					Conversion.GetScaleDenominator(),
					Scale)
				|| !MakeSignedRational(
					Conversion.GetOffsetTargetTicksNumerator(),
					Conversion.GetOffsetTargetTicksDenominator(),
					Offset))
			{
				return Fail(
					OutError,
					TEXT("temporal_conversion_invalid"));
			}
			SourceTick = {Tick, TEXT("1")};
			const FExactRational SourceSeconds = {
				MultiplySigned(SourceTick.Numerator, {false, SourceRate.Denominator}),
				MultiplyMagnitude(SourceTick.Denominator, SourceRate.Numerator.Magnitude),
			};
			const FExactRational ScaledSeconds =
				MultiplyRational(SourceSeconds, Scale);
			const FExactRational TargetTicks = {
				MultiplySigned(
					ScaledSeconds.Numerator,
					TargetRate.Numerator),
				MultiplyMagnitude(
					ScaledSeconds.Denominator,
					TargetRate.Denominator),
			};
			const FExactRational WithOffset =
				AddRational(TargetTicks, Offset);
			FString RoundedTick;
			if (!RoundRational(WithOffset, Rounding, RoundedTick, OutError))
			{
				return false;
			}
			OutPoint = FUeAgentTemporalPoint(
				Conversion.GetTargetClockId(),
				MoveTemp(RoundedTick),
				Conversion.GetTargetRate().GetNumerator(),
				Conversion.GetTargetRate().GetDenominator(),
				Source.GetState());
			return OutPoint.IsValid()
				|| Fail(OutError, TEXT("temporal_conversion_result_invalid"));
		}
	}

	FUeAgentTemporalRate::FUeAgentTemporalRate(
		FString InNumerator,
		FString InDenominator)
		: Numerator(MoveTemp(InNumerator))
		, Denominator(MoveTemp(InDenominator))
	{
		bValid = IsCanonicalUnsignedInteger(Numerator, false)
			&& IsCanonicalUnsignedInteger(Denominator, false);
	}

	const FString& FUeAgentTemporalRate::GetNumerator() const { return Numerator; }
	const FString& FUeAgentTemporalRate::GetDenominator() const { return Denominator; }
	bool FUeAgentTemporalRate::IsValid() const { return bValid; }

	bool FUeAgentTemporalRate::SemanticallyEquals(
		const FUeAgentTemporalRate& Other) const
	{
		return IsValid() && Other.IsValid()
			&& MultiplyMagnitude(Numerator, Other.Denominator)
				== MultiplyMagnitude(Other.Numerator, Denominator);
	}

	FUeAgentTemporalConversion::FUeAgentTemporalConversion(
		FString InConversionId,
		FString InSourceClockId,
		FString InTargetClockId,
		FUeAgentTemporalRate InSourceRate,
		FUeAgentTemporalRate InTargetRate,
		FString InScaleNumerator,
		FString InScaleDenominator,
		FString InOffsetTargetTicksNumerator,
		FString InOffsetTargetTicksDenominator,
		FString InEvidence)
		: ConversionId(MoveTemp(InConversionId))
		, SourceClockId(MoveTemp(InSourceClockId))
		, TargetClockId(MoveTemp(InTargetClockId))
		, SourceRate(MoveTemp(InSourceRate))
		, TargetRate(MoveTemp(InTargetRate))
		, ScaleNumerator(MoveTemp(InScaleNumerator))
		, ScaleDenominator(MoveTemp(InScaleDenominator))
		, OffsetTargetTicksNumerator(
			MoveTemp(InOffsetTargetTicksNumerator))
		, OffsetTargetTicksDenominator(
			MoveTemp(InOffsetTargetTicksDenominator))
		, Evidence(MoveTemp(InEvidence))
	{
		FSignedInteger Scale;
		FSignedInteger Offset;
		bValid = IsNonEmpty(ConversionId)
			&& IsNonEmpty(SourceClockId)
			&& IsNonEmpty(TargetClockId)
			&& SourceRate.IsValid()
			&& TargetRate.IsValid()
			&& ParseSigned(ScaleNumerator, Scale)
			&& Scale.Magnitude != TEXT("0")
			&& IsCanonicalUnsignedInteger(ScaleDenominator, false)
			&& ParseSigned(OffsetTargetTicksNumerator, Offset)
			&& IsCanonicalUnsignedInteger(
				OffsetTargetTicksDenominator, false)
			&& IsNonEmpty(Evidence);
	}

	const FString& FUeAgentTemporalConversion::GetConversionId() const { return ConversionId; }
	const FString& FUeAgentTemporalConversion::GetSourceClockId() const { return SourceClockId; }
	const FString& FUeAgentTemporalConversion::GetTargetClockId() const { return TargetClockId; }
	const FUeAgentTemporalRate& FUeAgentTemporalConversion::GetSourceRate() const { return SourceRate; }
	const FUeAgentTemporalRate& FUeAgentTemporalConversion::GetTargetRate() const { return TargetRate; }
	const FString& FUeAgentTemporalConversion::GetScaleNumerator() const { return ScaleNumerator; }
	const FString& FUeAgentTemporalConversion::GetScaleDenominator() const { return ScaleDenominator; }
	const FString& FUeAgentTemporalConversion::GetOffsetTargetTicksNumerator() const { return OffsetTargetTicksNumerator; }
	const FString& FUeAgentTemporalConversion::GetOffsetTargetTicksDenominator() const { return OffsetTargetTicksDenominator; }
	const FString& FUeAgentTemporalConversion::GetEvidence() const { return Evidence; }
	bool FUeAgentTemporalConversion::IsReverse() const { return ScaleNumerator.StartsWith(TEXT("-")); }
	bool FUeAgentTemporalConversion::IsValid() const { return bValid; }

	FUeAgentTemporalBound::FUeAgentTemporalBound(
		EUeAgentTemporalBoundKind InKind,
		TOptional<FUeAgentTemporalPoint> InPoint)
		: Kind(InKind)
		, Point(MoveTemp(InPoint))
	{
	}

	FUeAgentTemporalBound FUeAgentTemporalBound::Open()
	{
		return FUeAgentTemporalBound(
			EUeAgentTemporalBoundKind::Open,
			TOptional<FUeAgentTemporalPoint>());
	}

	FUeAgentTemporalBound FUeAgentTemporalBound::Inclusive(
		FUeAgentTemporalPoint Point)
	{
		return FUeAgentTemporalBound(
			EUeAgentTemporalBoundKind::Inclusive,
			MoveTemp(Point));
	}

	FUeAgentTemporalBound FUeAgentTemporalBound::Exclusive(
		FUeAgentTemporalPoint Point)
	{
		return FUeAgentTemporalBound(
			EUeAgentTemporalBoundKind::Exclusive,
			MoveTemp(Point));
	}

	EUeAgentTemporalBoundKind FUeAgentTemporalBound::GetKind() const { return Kind; }
	const TOptional<FUeAgentTemporalPoint>& FUeAgentTemporalBound::GetPoint() const { return Point; }

	bool FUeAgentTemporalBound::IsValid() const
	{
		return Kind == EUeAgentTemporalBoundKind::Open
			? !Point.IsSet()
			: Point.IsSet() && Point->IsValid();
	}

	FUeAgentTemporalRange::FUeAgentTemporalRange(
		FString InClockId,
		FString InState,
		FUeAgentTemporalBound InLowerBound,
		FUeAgentTemporalBound InUpperBound)
		: ClockId(MoveTemp(InClockId))
		, State(MoveTemp(InState))
		, LowerBound(MoveTemp(InLowerBound))
		, UpperBound(MoveTemp(InUpperBound))
	{
		bValid = IsNonEmpty(ClockId)
			&& IsNonEmpty(State)
			&& LowerBound.IsValid()
			&& UpperBound.IsValid();
		for (const FUeAgentTemporalBound* Bound : {&LowerBound, &UpperBound})
		{
			if (Bound->GetKind() != EUeAgentTemporalBoundKind::Open)
			{
				bValid = bValid
					&& Bound->GetPoint()->GetClockId() == ClockId
					&& Bound->GetPoint()->GetState() == State;
			}
		}
		if (bValid
			&& LowerBound.GetPoint().IsSet()
			&& UpperBound.GetPoint().IsSet())
		{
			int32 Compared = 0;
			bValid = CompareTemporalPointsExact(
				*LowerBound.GetPoint(),
				*UpperBound.GetPoint(),
				Compared)
				&& Compared <= 0;
			bEmpty = bValid && Compared == 0
				&& (LowerBound.GetKind()
						== EUeAgentTemporalBoundKind::Exclusive
					|| UpperBound.GetKind()
						== EUeAgentTemporalBoundKind::Exclusive);
		}
	}

	const FString& FUeAgentTemporalRange::GetClockId() const { return ClockId; }
	const FString& FUeAgentTemporalRange::GetState() const { return State; }
	const FUeAgentTemporalBound& FUeAgentTemporalRange::GetLowerBound() const { return LowerBound; }
	const FUeAgentTemporalBound& FUeAgentTemporalRange::GetUpperBound() const { return UpperBound; }
	bool FUeAgentTemporalRange::IsValid() const { return bValid; }
	bool FUeAgentTemporalRange::IsEmpty() const { return bValid && bEmpty; }

	bool AreTemporalPointsEquivalent(
		const FUeAgentTemporalPoint& Left,
		const FUeAgentTemporalPoint& Right)
	{
		int32 Compared = 0;
		return CompareTemporalPointsExact(Left, Right, Compared)
			&& Compared == 0;
	}

	bool TryCompareTemporalPoints(
		const FUeAgentTemporalPoint& Left,
		const FUeAgentTemporalPoint& Right,
		FUeAgentResourceTracker& Resources,
		EUeAgentTemporalComparison& OutComparison,
		FUeAgentStateSemanticError& OutError)
	{
		OutError = FUeAgentStateSemanticError{};
		OutComparison = EUeAgentTemporalComparison::Equivalent;
		if (!Left.IsValid() || !Right.IsValid())
		{
			return Fail(OutError, TEXT("temporal_point_invalid"));
		}
		if (Left.GetClockId() != Right.GetClockId())
		{
			return Fail(
				OutError,
				TEXT("temporal_comparison_clock_mismatch"),
				TEXT("clock_id"));
		}
		if (Left.GetState() != Right.GetState())
		{
			return Fail(
				OutError,
				TEXT("temporal_comparison_state_mismatch"),
				TEXT("state"));
		}
		if (!ChargeTemporalWork(
				Resources,
				{Left.GetTick(), Left.GetRateNumerator(),
				 Left.GetRateDenominator(), Right.GetTick(),
				 Right.GetRateNumerator(), Right.GetRateDenominator()},
				TEXT("temporal.compare"),
				OutError))
		{
			return false;
		}
		int32 Compared = 0;
		if (!CompareTemporalPointsExact(Left, Right, Compared))
		{
			return Fail(OutError, TEXT("temporal_comparison_invalid"));
		}
		OutComparison = Compared < 0
			? EUeAgentTemporalComparison::Before
			: Compared > 0
				? EUeAgentTemporalComparison::After
				: EUeAgentTemporalComparison::Equivalent;
		return true;
	}

	bool TryConvertTemporalPoint(
		const FUeAgentTemporalPoint& Source,
		const FUeAgentTemporalConversion& Conversion,
		EUeAgentTemporalRoundingPolicy Rounding,
		FUeAgentResourceTracker& Resources,
		TOptional<FUeAgentTemporalPoint>& OutPoint,
		FUeAgentStateSemanticError& OutError)
	{
		OutPoint.Reset();
		OutError = FUeAgentStateSemanticError{};
		if (!Source.IsValid())
		{
			return Fail(OutError, TEXT("temporal_point_invalid"));
		}
		if (!Conversion.IsValid())
		{
			return Fail(OutError, TEXT("temporal_conversion_invalid"));
		}
		if (Source.GetClockId() != Conversion.GetSourceClockId())
		{
			return Fail(
				OutError,
				TEXT("temporal_conversion_source_clock_mismatch"),
				TEXT("source.clock_id"));
		}
		const FUeAgentTemporalRate SourceRate(
			Source.GetRateNumerator(),
			Source.GetRateDenominator());
		if (!SourceRate.SemanticallyEquals(Conversion.GetSourceRate()))
		{
			return Fail(
				OutError,
				TEXT("temporal_conversion_source_rate_mismatch"),
				TEXT("source.rate"));
		}
		if (!ChargeTemporalWork(
				Resources,
				{Source.GetTick(), Source.GetRateNumerator(),
				 Source.GetRateDenominator(), Conversion.GetScaleNumerator(),
				 Conversion.GetScaleDenominator(),
				 Conversion.GetTargetRate().GetNumerator(),
				 Conversion.GetTargetRate().GetDenominator(),
				 Conversion.GetOffsetTargetTicksNumerator(),
				 Conversion.GetOffsetTargetTicksDenominator()},
				TEXT("temporal.convert_point"),
				OutError))
		{
			return false;
		}
		FUeAgentTemporalPoint Converted(
			TEXT("invalid"), TEXT("0"), TEXT("1"), TEXT("1"), TEXT("invalid"));
		if (!BuildConvertedPoint(
				Source,
				Conversion,
				Rounding,
				Converted,
				OutError))
		{
			return false;
		}
		OutPoint = MoveTemp(Converted);
		return true;
	}

	bool TryConvertTemporalRange(
		const FUeAgentTemporalRange& Source,
		const FUeAgentTemporalConversion& Conversion,
		EUeAgentTemporalRoundingPolicy LowerRounding,
		EUeAgentTemporalRoundingPolicy UpperRounding,
		FUeAgentResourceTracker& Resources,
		TOptional<FUeAgentTemporalRange>& OutRange,
		FUeAgentStateSemanticError& OutError)
	{
		OutRange.Reset();
		OutError = FUeAgentStateSemanticError{};
		if (!Source.IsValid())
		{
			return Fail(OutError, TEXT("temporal_range_invalid"));
		}
		if (!Conversion.IsValid())
		{
			return Fail(OutError, TEXT("temporal_conversion_invalid"));
		}
		if (Source.GetClockId() != Conversion.GetSourceClockId())
		{
			return Fail(
				OutError,
				TEXT("temporal_conversion_source_clock_mismatch"),
				TEXT("range.clock_id"));
		}

		auto ConvertBound = [&Conversion, &Resources, &OutError](
			const FUeAgentTemporalBound& Bound,
			EUeAgentTemporalRoundingPolicy Rounding,
			FUeAgentTemporalBound& OutBound)
		{
			if (Bound.GetKind() == EUeAgentTemporalBoundKind::Open)
			{
				OutBound = FUeAgentTemporalBound::Open();
				return true;
			}
			TOptional<FUeAgentTemporalPoint> Converted;
			if (!TryConvertTemporalPoint(
					*Bound.GetPoint(),
					Conversion,
					Rounding,
					Resources,
					Converted,
					OutError)
				|| !Converted.IsSet())
			{
				return false;
			}
			OutBound = Bound.GetKind() == EUeAgentTemporalBoundKind::Inclusive
				? FUeAgentTemporalBound::Inclusive(MoveTemp(*Converted))
				: FUeAgentTemporalBound::Exclusive(MoveTemp(*Converted));
			return true;
		};

		FUeAgentTemporalBound ConvertedLower = FUeAgentTemporalBound::Open();
		FUeAgentTemporalBound ConvertedUpper = FUeAgentTemporalBound::Open();
		if (!ConvertBound(
				Source.GetLowerBound(),
				LowerRounding,
				ConvertedLower)
			|| !ConvertBound(
				Source.GetUpperBound(),
				UpperRounding,
				ConvertedUpper))
		{
			return false;
		}
		if (Conversion.IsReverse())
		{
			Swap(ConvertedLower, ConvertedUpper);
		}
		FUeAgentTemporalRange ConvertedRange(
			Conversion.GetTargetClockId(),
			Source.GetState(),
			MoveTemp(ConvertedLower),
			MoveTemp(ConvertedUpper));
		if (!ConvertedRange.IsValid())
		{
			return Fail(
				OutError,
				TEXT("temporal_conversion_result_range_invalid"));
		}
		OutRange = MoveTemp(ConvertedRange);
		return true;
	}
}

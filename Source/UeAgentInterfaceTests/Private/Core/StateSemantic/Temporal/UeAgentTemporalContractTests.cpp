#include "Misc/AutomationTest.h"

#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "Core/StateSemantic/UeAgentTemporalContract.h"

namespace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::StateSemantic;

	FString FixturePath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectPluginsDir(),
			TEXT(
				"UeAgentInterface/Source/UeAgentInterfaceTests/Private/"
				"Core/StateSemantic/Temporal/Fixtures/"
				"temporal_contract_cases.json")));
	}

	TSharedPtr<FJsonObject> LoadFixture(FAutomationTestBase* Test)
	{
		FString Text;
		if (!Test->TestTrue(
				TEXT("load temporal contract fixture"),
				FFileHelper::LoadFileToString(Text, *FixturePath())))
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> Fixture;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Text);
		if (!Test->TestTrue(
				TEXT("parse temporal contract fixture"),
				FJsonSerializer::Deserialize(Reader, Fixture)
					&& Fixture.IsValid()))
		{
			return nullptr;
		}
		return Fixture;
	}

	TArray<FString> ReadStrings(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field)
	{
		TArray<FString> Result;
		for (const TSharedPtr<FJsonValue>& Value
			: Object->GetArrayField(Field))
		{
			Result.Add(Value->AsString());
		}
		return Result;
	}

	FUeAgentResourceTracker MakeTracker()
	{
		return FUeAgentResourceTracker(FUeAgentResourceBudget({
			{EUeAgentResourceDimension::Nodes, 128},
			{EUeAgentResourceDimension::Items, 1024},
			{EUeAgentResourceDimension::StringBytes, 65536},
			{EUeAgentResourceDimension::WorkUnits, 65536},
			{EUeAgentResourceDimension::EstimatedMemoryBytes, 1048576},
		}));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentTemporalContractTest,
	"UeAgentInterface.Core.StateSemantic.Temporal.E0.Contract."
		"RateRangeConversion",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentTemporalContractTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}

	TestEqual(
		TEXT("fixture schema"),
		Fixture->GetStringField(TEXT("schema")),
		FString(TEXT("uai.state_semantic.temporal.test_fixture.v1")));
	TestEqual(
		TEXT("temporal contract identity"),
		Fixture->GetStringField(TEXT("contract_id")),
		FString(TEXT("uai.temporal_contract.v1")));
	TestEqual(
		TEXT("four explicit rounding policies"),
		Fixture->GetArrayField(TEXT("rounding_policies")).Num(),
		4);
	TestEqual(
		TEXT("three typed bound kinds"),
		Fixture->GetArrayField(TEXT("bound_kinds")).Num(),
		3);
	TestEqual(
		TEXT("seven required temporal scenarios"),
		Fixture->GetArrayField(TEXT("scenarios")).Num(),
		7);

	const TArray<FString> Forbidden = ReadStrings(
		Fixture,
		TEXT("forbidden_implicit_behaviors"));
	TestTrue(
		TEXT("float is not temporal identity"),
		Forbidden.Contains(TEXT("double_identity")));
	TestTrue(
		TEXT("30fps fallback is forbidden"),
		Forbidden.Contains(TEXT("implicit_30fps_fallback")));
	TestTrue(
		TEXT("handler rounding is forbidden"),
		Forbidden.Contains(TEXT("handler_local_rounding")));
	TestTrue(
		TEXT("captured time cannot become authored time"),
		Forbidden.Contains(TEXT("captured_time_as_authored_time")));

	const FUeAgentTemporalPoint NtscTicks(
		TEXT("sequence:/Game/Test/LS_Ntsc"),
		TEXT("30000"),
		TEXT("30000"),
		TEXT("1001"),
		TEXT("authored"));
	const FUeAgentTemporalPoint ExactSeconds(
		TEXT("sequence:/Game/Test/LS_Ntsc"),
		TEXT("1001"),
		TEXT("1"),
		TEXT("1"),
		TEXT("authored"));
	TestTrue(
		TEXT("existing C4 preserves 30000/1001 exact identity"),
		AreTemporalPointsEquivalent(NtscTicks, ExactSeconds));
	const FUeAgentTemporalPoint OtherClock(
		TEXT("sequence:/Game/Test/LS_Other"),
		TEXT("1001"),
		TEXT("1"),
		TEXT("1"),
		TEXT("authored"));
	TestFalse(
		TEXT("different clocks are not directly comparable"),
		AreTemporalPointsEquivalent(NtscTicks, OtherClock));

	TestEqual(
		TEXT("production temporal contract version"),
		UAI_TEMPORAL_CONTRACT_V1,
		1);

	const FUeAgentTemporalRate Rate24(TEXT("24"), TEXT("1"));
	const FUeAgentTemporalRate Rate48(TEXT("48"), TEXT("1"));
	const FUeAgentTemporalRate Equivalent24(TEXT("48"), TEXT("2"));
	TestTrue(TEXT("temporal rate is valid"), Rate24.IsValid());
	TestFalse(
		TEXT("zero temporal rate is rejected"),
		FUeAgentTemporalRate(TEXT("0"), TEXT("1")).IsValid());
	TestTrue(
		TEXT("equivalent rational rates compare exactly"),
		Rate24.SemanticallyEquals(Equivalent24));

	const FUeAgentTemporalConversion DisplayToTick(
		TEXT("sequence-display-to-tick:/Game/Test/LS_Temporal"),
		TEXT("sequence:/Game/Test/LS_Temporal"),
		TEXT("sequence:/Game/Test/LS_Temporal"),
		Rate24,
		Rate48,
		TEXT("1"),
		TEXT("1"),
		TEXT("0"),
		TEXT("1"),
		TEXT("movie_scene_rate_readback:revision-7"));
	const FUeAgentTemporalPoint DisplayPoint(
		TEXT("sequence:/Game/Test/LS_Temporal"),
		TEXT("12"),
		TEXT("24"),
		TEXT("1"),
		TEXT("authored"));
	TOptional<FUeAgentTemporalPoint> ConvertedPoint;
	FUeAgentStateSemanticError Error;
	FUeAgentResourceTracker ExactTracker = MakeTracker();
	TestTrue(
		TEXT("exact display-rate to tick-resolution conversion succeeds"),
		TryConvertTemporalPoint(
			DisplayPoint,
			DisplayToTick,
			EUeAgentTemporalRoundingPolicy::Exact,
			ExactTracker,
			ConvertedPoint,
			Error));
	TestTrue(TEXT("exact converted point returned"), ConvertedPoint.IsSet());
	if (ConvertedPoint.IsSet())
	{
		TestEqual(
			TEXT("exact converted tick"),
			ConvertedPoint->GetTick(),
			FString(TEXT("24")));
		TestEqual(
			TEXT("target rate numerator"),
			ConvertedPoint->GetRateNumerator(),
			FString(TEXT("48")));
		EUeAgentTemporalComparison Comparison =
			EUeAgentTemporalComparison::Before;
		FUeAgentResourceTracker CompareTracker = MakeTracker();
		TestTrue(
			TEXT("converted rate views compare through the C4 owner"),
			TryCompareTemporalPoints(
				DisplayPoint,
				*ConvertedPoint,
				CompareTracker,
				Comparison,
				Error));
		TestEqual(
			TEXT("converted rate views are the same instant"),
			Comparison,
			EUeAgentTemporalComparison::Equivalent);
	}

	const FUeAgentTemporalConversion Rate24To30(
		TEXT("sequence-rounding:/Game/Test/LS_Rounding"),
		TEXT("sequence:/Game/Test/LS_Rounding"),
		TEXT("sequence:/Game/Test/LS_Rounding"),
		Rate24,
		FUeAgentTemporalRate(TEXT("30"), TEXT("1")),
		TEXT("1"), TEXT("1"), TEXT("0"), TEXT("1"),
		TEXT("movie_scene_rate_readback:revision-9"));
	const FUeAgentTemporalPoint OneAt24(
		TEXT("sequence:/Game/Test/LS_Rounding"),
		TEXT("1"), TEXT("24"), TEXT("1"), TEXT("authored"));
	FUeAgentResourceTracker InexactTracker = MakeTracker();
	ConvertedPoint.Reset();
	TestFalse(
		TEXT("inexact conversion fails without rounding"),
		TryConvertTemporalPoint(
			OneAt24,
			Rate24To30,
			EUeAgentTemporalRoundingPolicy::Exact,
			InexactTracker,
			ConvertedPoint,
			Error));
	TestEqual(
		TEXT("inexact conversion has stable error"),
		Error.Code,
		FString(TEXT("temporal_conversion_inexact")));

	auto RequireRoundedTick = [this, &OneAt24, &Rate24To30](
		EUeAgentTemporalRoundingPolicy Policy,
		const FString& ExpectedTick,
		const TCHAR* Label)
	{
		FUeAgentResourceTracker Tracker = MakeTracker();
		TOptional<FUeAgentTemporalPoint> Point;
		FUeAgentStateSemanticError LocalError;
		TestTrue(
			Label,
			TryConvertTemporalPoint(
				OneAt24,
				Rate24To30,
				Policy,
				Tracker,
				Point,
				LocalError));
		TestTrue(FString(Label) + TEXT(" returned point"), Point.IsSet());
		if (Point.IsSet())
		{
			TestEqual(FString(Label) + TEXT(" tick"), Point->GetTick(), ExpectedTick);
		}
	};
	RequireRoundedTick(
		EUeAgentTemporalRoundingPolicy::Floor,
		TEXT("1"),
		TEXT("floor rounding is explicit"));
	RequireRoundedTick(
		EUeAgentTemporalRoundingPolicy::Ceiling,
		TEXT("2"),
		TEXT("ceiling rounding is explicit"));
	RequireRoundedTick(
		EUeAgentTemporalRoundingPolicy::NearestTiesToEven,
		TEXT("1"),
		TEXT("nearest rounding is explicit"));

	const FUeAgentTemporalConversion HalfTickConversion(
		TEXT("nearest-ties-to-even"),
		TEXT("sequence:/Game/Test/LS_Tie"),
		TEXT("sequence:/Game/Test/LS_Tie"),
		FUeAgentTemporalRate(TEXT("2"), TEXT("1")),
		FUeAgentTemporalRate(TEXT("1"), TEXT("1")),
		TEXT("1"), TEXT("1"), TEXT("0"), TEXT("1"),
		TEXT("explicit-test-evidence"));
	auto RequireTieToEven = [this, &HalfTickConversion](
		const FString& SourceTick,
		const FString& ExpectedTick)
	{
		FUeAgentResourceTracker Tracker = MakeTracker();
		TOptional<FUeAgentTemporalPoint> Point;
		FUeAgentStateSemanticError LocalError;
		TestTrue(
			TEXT("tie-to-even conversion succeeds: ") + SourceTick,
			TryConvertTemporalPoint(
				FUeAgentTemporalPoint(
					TEXT("sequence:/Game/Test/LS_Tie"),
					SourceTick,
					TEXT("2"), TEXT("1"), TEXT("authored")),
				HalfTickConversion,
				EUeAgentTemporalRoundingPolicy::NearestTiesToEven,
				Tracker,
				Point,
				LocalError));
		TestTrue(
			TEXT("tie-to-even point returned: ") + SourceTick,
			Point.IsSet());
		if (Point.IsSet())
		{
			TestEqual(
				TEXT("tie-to-even tick: ") + SourceTick,
				Point->GetTick(),
				ExpectedTick);
		}
	};
	RequireTieToEven(TEXT("1"), TEXT("0"));
	RequireTieToEven(TEXT("3"), TEXT("2"));

	const FUeAgentTemporalRange SourceRange(
		TEXT("sequence:/Game/Test/LS_Temporal"),
		TEXT("authored"),
		FUeAgentTemporalBound::Inclusive(FUeAgentTemporalPoint(
			TEXT("sequence:/Game/Test/LS_Temporal"),
			TEXT("0"), TEXT("24"), TEXT("1"), TEXT("authored"))),
		FUeAgentTemporalBound::Exclusive(FUeAgentTemporalPoint(
			TEXT("sequence:/Game/Test/LS_Temporal"),
			TEXT("24"), TEXT("24"), TEXT("1"), TEXT("authored"))));
	TestTrue(TEXT("typed range is valid"), SourceRange.IsValid());
	TestFalse(TEXT("inclusive-exclusive range is not empty"), SourceRange.IsEmpty());
	TOptional<FUeAgentTemporalRange> ConvertedRange;
	FUeAgentResourceTracker RangeTracker = MakeTracker();
	TestTrue(
		TEXT("range conversion succeeds"),
		TryConvertTemporalRange(
			SourceRange,
			DisplayToTick,
			EUeAgentTemporalRoundingPolicy::Exact,
			EUeAgentTemporalRoundingPolicy::Exact,
			RangeTracker,
			ConvertedRange,
			Error));
	TestTrue(TEXT("converted range returned"), ConvertedRange.IsSet());
	if (ConvertedRange.IsSet())
	{
		TestEqual(
			TEXT("lower bound remains inclusive"),
			ConvertedRange->GetLowerBound().GetKind(),
			EUeAgentTemporalBoundKind::Inclusive);
		TestEqual(
			TEXT("upper bound remains exclusive"),
			ConvertedRange->GetUpperBound().GetKind(),
			EUeAgentTemporalBoundKind::Exclusive);
		TestEqual(
			TEXT("converted upper tick"),
			ConvertedRange->GetUpperBound().GetPoint()->GetTick(),
			FString(TEXT("48")));
	}

	const FUeAgentTemporalRange OpenUpperRange(
		TEXT("sequence:/Game/Test/LS_Temporal"),
		TEXT("authored"),
		FUeAgentTemporalBound::Inclusive(FUeAgentTemporalPoint(
			TEXT("sequence:/Game/Test/LS_Temporal"),
			TEXT("12"), TEXT("24"), TEXT("1"), TEXT("authored"))),
		FUeAgentTemporalBound::Open());
	FUeAgentResourceTracker OpenRangeTracker = MakeTracker();
	ConvertedRange.Reset();
	TestTrue(
		TEXT("open upper range conversion succeeds"),
		TryConvertTemporalRange(
			OpenUpperRange,
			DisplayToTick,
			EUeAgentTemporalRoundingPolicy::Exact,
			EUeAgentTemporalRoundingPolicy::Exact,
			OpenRangeTracker,
			ConvertedRange,
			Error));
	TestTrue(TEXT("open upper range returned"), ConvertedRange.IsSet());
	if (ConvertedRange.IsSet())
	{
		TestEqual(
			TEXT("open upper bound stays open"),
			ConvertedRange->GetUpperBound().GetKind(),
			EUeAgentTemporalBoundKind::Open);
	}

	const FUeAgentTemporalRange InvalidReverseOrderedRange(
		TEXT("sequence:/Game/Test/LS_Temporal"),
		TEXT("authored"),
		FUeAgentTemporalBound::Inclusive(FUeAgentTemporalPoint(
			TEXT("sequence:/Game/Test/LS_Temporal"),
			TEXT("24"), TEXT("24"), TEXT("1"), TEXT("authored"))),
		FUeAgentTemporalBound::Inclusive(FUeAgentTemporalPoint(
			TEXT("sequence:/Game/Test/LS_Temporal"),
			TEXT("0"), TEXT("24"), TEXT("1"), TEXT("authored"))));
	TestFalse(
		TEXT("lower instant after upper instant is invalid"),
		InvalidReverseOrderedRange.IsValid());
	const FUeAgentTemporalRange EmptyExclusiveRange(
		TEXT("sequence:/Game/Test/LS_Temporal"),
		TEXT("authored"),
		FUeAgentTemporalBound::Inclusive(FUeAgentTemporalPoint(
			TEXT("sequence:/Game/Test/LS_Temporal"),
			TEXT("12"), TEXT("24"), TEXT("1"), TEXT("authored"))),
		FUeAgentTemporalBound::Exclusive(FUeAgentTemporalPoint(
			TEXT("sequence:/Game/Test/LS_Temporal"),
			TEXT("24"), TEXT("48"), TEXT("1"), TEXT("authored"))));
	TestTrue(TEXT("equal exclusive range is valid"), EmptyExclusiveRange.IsValid());
	TestTrue(TEXT("equal exclusive range is empty"), EmptyExclusiveRange.IsEmpty());

	const FUeAgentTemporalConversion ReverseConversion(
		TEXT("reverse-range"),
		TEXT("sequence:/Game/Test/LS_Temporal"),
		TEXT("sequence:/Game/Test/LS_Temporal:reversed"),
		Rate24,
		Rate24,
		TEXT("-1"), TEXT("1"), TEXT("24"), TEXT("1"),
		TEXT("explicit-reverse-evidence"));
	FUeAgentResourceTracker ReverseTracker = MakeTracker();
	ConvertedRange.Reset();
	TestTrue(
		TEXT("negative scale range conversion succeeds"),
		TryConvertTemporalRange(
			SourceRange,
			ReverseConversion,
			EUeAgentTemporalRoundingPolicy::Exact,
			EUeAgentTemporalRoundingPolicy::Exact,
			ReverseTracker,
			ConvertedRange,
			Error));
	TestTrue(TEXT("reversed range returned"), ConvertedRange.IsSet());
	if (ConvertedRange.IsSet())
	{
		TestEqual(
			TEXT("reversed lower carries source upper kind"),
			ConvertedRange->GetLowerBound().GetKind(),
			EUeAgentTemporalBoundKind::Exclusive);
		TestEqual(
			TEXT("reversed upper carries source lower kind"),
			ConvertedRange->GetUpperBound().GetKind(),
			EUeAgentTemporalBoundKind::Inclusive);
	}

	const FUeAgentTemporalConversion MontageToAnimation(
		TEXT("montage-affine"),
		TEXT("montage:/Game/Test/AM_Test:slot:segment-1"),
		TEXT("animation:/Game/Test/A_Test"),
		FUeAgentTemporalRate(TEXT("30"), TEXT("1")),
		FUeAgentTemporalRate(TEXT("60"), TEXT("1")),
		TEXT("2"), TEXT("1"), TEXT("30"), TEXT("1"),
		TEXT("montage_segment_readback:revision-3"));
	FUeAgentResourceTracker MontageTracker = MakeTracker();
	ConvertedPoint.Reset();
	TestTrue(
		TEXT("montage affine conversion succeeds"),
		TryConvertTemporalPoint(
			FUeAgentTemporalPoint(
				TEXT("montage:/Game/Test/AM_Test:slot:segment-1"),
				TEXT("30"), TEXT("30"), TEXT("1"), TEXT("authored")),
			MontageToAnimation,
			EUeAgentTemporalRoundingPolicy::Exact,
			MontageTracker,
			ConvertedPoint,
			Error));
	if (ConvertedPoint.IsSet())
	{
		TestEqual(
			TEXT("montage affine target tick"),
			ConvertedPoint->GetTick(),
			FString(TEXT("150")));
	}

	const FUeAgentTemporalConversion WrongSourceClock(
		TEXT("wrong-source-clock"),
		TEXT("sequence:/Game/Test/LS_Other"),
		TEXT("sequence:/Game/Test/LS_Temporal"),
		Rate24,
		Rate48,
		TEXT("1"), TEXT("1"), TEXT("0"), TEXT("1"),
		TEXT("explicit-test-evidence"));
	FUeAgentResourceTracker WrongClockTracker = MakeTracker();
	ConvertedPoint.Reset();
	TestFalse(
		TEXT("conversion rejects mismatched source clock"),
		TryConvertTemporalPoint(
			DisplayPoint,
			WrongSourceClock,
			EUeAgentTemporalRoundingPolicy::Exact,
			WrongClockTracker,
			ConvertedPoint,
			Error));
	TestEqual(
		TEXT("source clock mismatch has stable error"),
		Error.Code,
		FString(TEXT("temporal_conversion_source_clock_mismatch")));

	EUeAgentTemporalComparison CrossClockComparison =
		EUeAgentTemporalComparison::Equivalent;
	FUeAgentResourceTracker CrossClockTracker = MakeTracker();
	TestFalse(
		TEXT("typed comparison rejects different clocks"),
		TryCompareTemporalPoints(
			DisplayPoint,
			OtherClock,
			CrossClockTracker,
			CrossClockComparison,
			Error));
	TestEqual(
		TEXT("cross-clock comparison has stable error"),
		Error.Code,
		FString(TEXT("temporal_comparison_clock_mismatch")));
	return true;
}

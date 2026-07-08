#include "Utilities/UeAgentJsonDiagnostics.h"

#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentJsonDiagnosticsCompactInputValuesTest,
	"UeAgentInterface.Json.Diagnostics.CompactInputValues",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentJsonDiagnosticsCompactInputValuesTest::RunTest(const FString& Parameters)
{
	TArray<TSharedPtr<FJsonValue>> VectorArray;
	VectorArray.Add(MakeShared<FJsonValueNumber>(1.0));
	VectorArray.Add(MakeShared<FJsonValueNumber>(2.0));
	VectorArray.Add(MakeShared<FJsonValueNumber>(3.0));

	FVector ParsedVector = FVector::ZeroVector;
	TestTrue(TEXT("Vector accepts [x,y,z]"), UeAgentJsonDiagnostics::TryReadVectorValue(MakeShared<FJsonValueArray>(VectorArray), ParsedVector));
	TestEqual(TEXT("Vector X"), ParsedVector.X, 1.0);
	TestEqual(TEXT("Vector Y"), ParsedVector.Y, 2.0);
	TestEqual(TEXT("Vector Z"), ParsedVector.Z, 3.0);

	TArray<TSharedPtr<FJsonValue>> Vector2DArray;
	Vector2DArray.Add(MakeShared<FJsonValueNumber>(4.0));
	Vector2DArray.Add(MakeShared<FJsonValueNumber>(5.0));

	FVector2D ParsedVector2D = FVector2D::ZeroVector;
	TestTrue(TEXT("Vector2D accepts [x,y]"), UeAgentJsonDiagnostics::TryReadVector2DValue(MakeShared<FJsonValueArray>(Vector2DArray), ParsedVector2D));
	TestEqual(TEXT("Vector2D X"), ParsedVector2D.X, 4.0);
	TestEqual(TEXT("Vector2D Y"), ParsedVector2D.Y, 5.0);

	TArray<TSharedPtr<FJsonValue>> RotatorArray;
	RotatorArray.Add(MakeShared<FJsonValueNumber>(10.0));
	RotatorArray.Add(MakeShared<FJsonValueNumber>(20.0));
	RotatorArray.Add(MakeShared<FJsonValueNumber>(30.0));

	FRotator ParsedRotator = FRotator::ZeroRotator;
	TestTrue(TEXT("Rotator accepts [pitch,yaw,roll]"), UeAgentJsonDiagnostics::TryReadRotatorValue(MakeShared<FJsonValueArray>(RotatorArray), ParsedRotator));
	TestEqual(TEXT("Rotator Pitch"), ParsedRotator.Pitch, 10.0);
	TestEqual(TEXT("Rotator Yaw"), ParsedRotator.Yaw, 20.0);
	TestEqual(TEXT("Rotator Roll"), ParsedRotator.Roll, 30.0);

	TArray<TSharedPtr<FJsonValue>> ColorArray;
	ColorArray.Add(MakeShared<FJsonValueNumber>(0.25));
	ColorArray.Add(MakeShared<FJsonValueNumber>(0.5));
	ColorArray.Add(MakeShared<FJsonValueNumber>(0.75));
	ColorArray.Add(MakeShared<FJsonValueNumber>(1.0));

	FLinearColor ParsedColor = FLinearColor::Transparent;
	TestTrue(TEXT("LinearColor accepts [r,g,b,a]"), UeAgentJsonDiagnostics::TryReadLinearColorValue(MakeShared<FJsonValueArray>(ColorArray), ParsedColor));
	TestEqual(TEXT("Color R"), ParsedColor.R, 0.25f);
	TestEqual(TEXT("Color G"), ParsedColor.G, 0.5f);
	TestEqual(TEXT("Color B"), ParsedColor.B, 0.75f);
	TestEqual(TEXT("Color A"), ParsedColor.A, 1.0f);

	TSharedPtr<FJsonObject> ColorObject = MakeShared<FJsonObject>();
	ColorObject->SetNumberField(TEXT("x"), 0.1);
	ColorObject->SetNumberField(TEXT("y"), 0.2);
	ColorObject->SetNumberField(TEXT("z"), 0.3);
	ColorObject->SetNumberField(TEXT("w"), 0.4);
	TestTrue(TEXT("LinearColor accepts x/y/z/w object"), UeAgentJsonDiagnostics::TryReadLinearColorValue(MakeShared<FJsonValueObject>(ColorObject), ParsedColor));
	TestEqual(TEXT("Color object R"), ParsedColor.R, 0.1f);
	TestEqual(TEXT("Color object G"), ParsedColor.G, 0.2f);
	TestEqual(TEXT("Color object B"), ParsedColor.B, 0.3f);
	TestEqual(TEXT("Color object A"), ParsedColor.A, 0.4f);

	return true;
}

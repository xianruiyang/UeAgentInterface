// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Utilities/AuthoringCore/UeAgentAuthoringAssetIdentity.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAuthoringAssetIdentityTest,
	"UeAgentInterface.Core.AuthoringCore.AssetIdentity.PackageAndObjectEquivalence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentAuthoringAssetIdentityTest::RunTest(const FString& Parameters)
{
	using namespace UeAgentAuthoringAssetIdentity;

	TestEqual(
		TEXT("package path remains a package path"),
		NormalizePackagePath(TEXT("/Game/UAI/Assets/DA_Test")),
		FString(TEXT("/Game/UAI/Assets/DA_Test")));
	TestEqual(
		TEXT("object path resolves to its package path"),
		NormalizePackagePath(TEXT(" /Game/UAI/Assets/DA_Test.DA_Test ")),
		FString(TEXT("/Game/UAI/Assets/DA_Test")));
	TestEqual(
		TEXT("backslashes normalize before object-path conversion"),
		NormalizePackagePath(TEXT("\\Game\\UAI\\Assets\\DA_Test.DA_Test")),
		FString(TEXT("/Game/UAI/Assets/DA_Test")));
	TestEqual(
		TEXT("trailing slashes are removed"),
		NormalizePackagePath(TEXT("/Game/UAI/Assets/DA_Test///")),
		FString(TEXT("/Game/UAI/Assets/DA_Test")));
	TestEqual(
		TEXT("empty input remains empty"),
		NormalizePackagePath(TEXT("   ")),
		FString());

	TestEqual(
		TEXT("package path expands to the conventional object path"),
		ToObjectPath(TEXT("/Game/UAI/Assets/DA_Test")),
		FString(TEXT("/Game/UAI/Assets/DA_Test.DA_Test")));
	TestEqual(
		TEXT("object path remains equivalent"),
		ToObjectPath(TEXT(" /Game/UAI/Assets/DA_Test.DA_Test ")),
		FString(TEXT("/Game/UAI/Assets/DA_Test.DA_Test")));
	TestEqual(
		TEXT("backslash package path expands after normalization"),
		ToObjectPath(TEXT("\\Game\\UAI\\Assets\\DA_Test\\")),
		FString(TEXT("/Game/UAI/Assets/DA_Test.DA_Test")));
	TestEqual(
		TEXT("empty object path remains empty"),
		ToObjectPath(FString()),
		FString());
	return true;
}

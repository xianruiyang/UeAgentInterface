#pragma once

#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "PackageTools.h"
#include "TestSupport/UeAgentFixtureDirectoryLifecycle.h"
#include "UObject/Package.h"

namespace UE::UAI::T85::FixtureLifecycle
{
	inline bool TryUnloadPackageForReadback(
		const FString& PackageName,
		FString& OutError)
	{
		UPackage* Package = FindPackage(nullptr, *PackageName);
		if (!Package)
		{
			OutError.Reset();
			return true;
		}
		Package->SetDirtyFlag(false);
		FText UnloadError;
		if (!UPackageTools::UnloadPackages({Package}, UnloadError, true))
		{
			OutError = UnloadError.ToString();
			return false;
		}
		CollectGarbage(RF_NoFlags, true);
		if (FindPackage(nullptr, *PackageName))
		{
			OutError = TEXT("t85_fixture_package_remained_loaded:") + PackageName;
			return false;
		}
		OutError.Reset();
		return true;
	}

	inline bool TryRemoveEmptyRootForPackage(
		FAutomationTestBase& Test,
		const FString& PackageName)
	{
		const FString RootPackage = FPackageName::GetLongPackagePath(PackageName);
		const FString OwnerPackage = TEXT("/Game/UAI/Transient");
		const FString Prefix = OwnerPackage + TEXT("/");
		const FString Leaf = RootPackage.RightChop(Prefix.Len());
		if (!RootPackage.StartsWith(Prefix, ESearchCase::CaseSensitive)
			|| !Leaf.StartsWith(TEXT("T85"), ESearchCase::CaseSensitive)
			|| Leaf.Contains(TEXT("/")))
		{
			Test.AddError(FString::Printf(
				TEXT("T85 fixture root refused: package is outside the fixed transient root: %s"),
				*PackageName));
			return false;
		}
		FString Error;
		if (!UE::UAI::TestSupport::TryRemoveEmptyFixturePackageDirectory(
			OwnerPackage, RootPackage, Error))
		{
			Test.AddError(Error);
			return false;
		}
		return true;
	}
}

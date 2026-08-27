#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Curves/CurveFloat.h"
#include "DomainAdapters/AssetDestructiveImport/UeAgentAssetDeleteOwner.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PackageTools.h"
#include "ObjectTools.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::DomainAdapters::AssetDestructiveImport;
	using namespace UE::UAI::Tests::PublicCommand;

	class FNeverCancelled final : public IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override { return false; }
	};

	class FScopedAssetDeleteFixture final
	{
	public:
		explicit FScopedAssetDeleteFixture(
			FString InAssetName = FString(),
			bool bInCreateSecondAsset = false,
			bool bInCreateBlueprint = false)
			: PackageName(
				TEXT("/Game/UAI/Tests/DestructiveImport/DA_PublicSlice_")
					+ FGuid::NewGuid().ToString(EGuidFormats::Digits))
			, AssetName(InAssetName.IsEmpty()
				? FPackageName::GetLongPackageAssetName(PackageName)
				: MoveTemp(InAssetName))
			, bCreateSecondAsset(bInCreateSecondAsset)
			, bCreateBlueprint(bInCreateBlueprint)
		{
		}

		~FScopedAssetDeleteFixture()
		{
			Cleanup();
		}

		bool Create()
		{
			UPackage* const Package = CreatePackage(*PackageName);
			if (Package == nullptr)
			{
				return false;
			}
			UObject* Asset = nullptr;
			if (bCreateBlueprint)
			{
				Asset = FKismetEditorUtilities::CreateBlueprint(
					UObject::StaticClass(), Package, *AssetName, BPTYPE_Normal,
					UBlueprint::StaticClass(),
					UBlueprintGeneratedClass::StaticClass(),
					TEXT("UAIAssetDeleteTest"));
			}
			else
			{
				Asset = NewObject<UCurveFloat>(
					Package, *AssetName,
					RF_Public | RF_Standalone | RF_Transactional);
			}
			if (Asset == nullptr)
			{
				return false;
			}
			FAssetRegistryModule::AssetCreated(Asset);
			if (bCreateSecondAsset)
			{
				UCurveFloat* const SecondAsset = NewObject<UCurveFloat>(
					Package,
					TEXT("SecondAsset"),
					RF_Public | RF_Standalone | RF_Transactional);
				if (!SecondAsset)
				{
					return false;
				}
				FAssetRegistryModule::AssetCreated(SecondAsset);
			}
			Package->MarkPackageDirty();
			FSavePackageArgs Args;
			Args.TopLevelFlags = RF_Public | RF_Standalone;
			return UPackage::SavePackage(Package, Asset, *Filename(), Args);
		}

		bool Unload() const
		{
			UPackage* const Package = FindPackage(nullptr, *PackageName);
			if (Package == nullptr)
			{
				return true;
			}
			Package->SetDirtyFlag(false);
			FText Error;
			const bool bUnloaded = UPackageTools::UnloadPackages(
				{Package}, Error, true);
			CollectGarbage(RF_NoFlags, true);
			return bUnloaded && FindPackage(nullptr, *PackageName) == nullptr;
		}

		bool IsDurablyPresent() const
		{
			if (!IFileManager::Get().FileExists(*Filename()))
			{
				return false;
			}
			return LoadObject<UObject>(nullptr, *ObjectPath()) != nullptr;
		}

		bool IsDurablyAbsent() const
		{
			FString ExistingFilename;
			TArray<FAssetData> Assets;
			FAssetRegistryModule::GetRegistry().GetAssetsByPackageName(
				FName(*PackageName), Assets, false);
			return !IFileManager::Get().FileExists(*Filename())
				&& !FPackageName::DoesPackageExist(PackageName, &ExistingFilename)
				&& LoadObject<UObject>(nullptr, *ObjectPath()) == nullptr
				&& Assets.IsEmpty();
		}

		bool Cleanup()
		{
			bool bClean = true;
			if (UPackage* const Package = FindPackage(nullptr, *PackageName))
			{
				if (UObject* const Asset = FindObject<UObject>(Package, *AssetName))
				{
					FAssetRegistryModule::AssetDeleted(Asset);
				}
				if (UObject* const SecondAsset = FindObject<UObject>(
						Package, TEXT("SecondAsset")))
				{
					FAssetRegistryModule::AssetDeleted(SecondAsset);
				}
			}
			bClean = Unload() && bClean;
			if (IFileManager::Get().FileExists(*Filename()))
			{
				bClean = IFileManager::Get().Delete(*Filename(), false, true)
					&& bClean;
			}
			FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles(
				{Filename()});
			return IsDurablyAbsent() && bClean;
		}

		const FString& GetPackageName() const { return PackageName; }
		FString GetFilename() const { return Filename(); }

		FString ObjectPath() const
		{
			return PackageName + TEXT(".") + AssetName;
		}

	private:
		FString Filename() const
		{
			return FPackageName::LongPackageNameToFilename(
				PackageName, FPackageName::GetAssetPackageExtension());
		}

		FString PackageName;
		FString AssetName;
		bool bCreateSecondAsset = false;
		bool bCreateBlueprint = false;
	};

	class FScopedStringTableDeleteFixture final
	{
	public:
		FScopedStringTableDeleteFixture()
			: PackageName(
				TEXT("/Game/UAI/Tests/DestructiveImport/ST_PublicSlice_")
					+ FGuid::NewGuid().ToString(EGuidFormats::Digits))
			, AssetName(FPackageName::GetLongPackageAssetName(PackageName))
		{
		}

		~FScopedStringTableDeleteFixture()
		{
			Cleanup();
		}

		bool Create()
		{
			UPackage* const Package = CreatePackage(*PackageName);
			UStringTable* const Asset = NewObject<UStringTable>(
				Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
			if (!Package || !Asset)
			{
				return false;
			}
			Asset->GetMutableStringTable()->SetSourceString(
				TEXT("EvidenceKey"), TEXT("T30 cleanup evidence"));
			FAssetRegistryModule::AssetCreated(Asset);
			Package->MarkPackageDirty();
			FSavePackageArgs Args;
			Args.TopLevelFlags = RF_Public | RF_Standalone;
			return UPackage::SavePackage(Package, Asset, *Filename(), Args);
		}

		bool IsDurablyAbsent() const
		{
			FString ExistingFilename;
			TArray<FAssetData> Assets;
			FAssetRegistryModule::GetRegistry().GetAssetsByPackageName(
				FName(*PackageName), Assets, false);
			return !IFileManager::Get().FileExists(*Filename())
				&& !FPackageName::DoesPackageExist(PackageName, &ExistingFilename)
				&& Assets.IsEmpty();
		}

		bool Cleanup()
		{
			if (UPackage* const Package = FindPackage(nullptr, *PackageName))
			{
				Package->SetDirtyFlag(false);
				if (UObject* const Asset = FindObject<UObject>(Package, *AssetName))
				{
					TGuardValue<bool> UnattendedGuard(
						GIsRunningUnattendedScript, true);
					ObjectTools::ForceDeleteObjects({Asset}, false);
				}
			}
			CollectGarbage(RF_NoFlags, true);
			if (IFileManager::Get().FileExists(*Filename()))
			{
				IFileManager::Get().Delete(*Filename(), false, true);
			}
			return IsDurablyAbsent();
		}

		const FString& GetPackageName() const { return PackageName; }

	private:
		FString Filename() const
		{
			return FPackageName::LongPackageNameToFilename(
				PackageName, FPackageName::GetAssetPackageExtension());
		}

		FString PackageName;
		FString AssetName;
	};

	FString DeleteParams(
		const FString& TargetRef,
		const FString& AuthorizationId = FString())
	{
		return TEXT("{\"target_ref\":\"") + TargetRef
			+ (AuthorizationId.IsEmpty()
				? TEXT("\"}")
				: TEXT("\",\"destructive_authorization_id\":\"")
					+ AuthorizationId + TEXT("\"}"));
	}

	bool TryParsePayload(
		const FUeAgentCommandResult& Result,
		TSharedPtr<FJsonObject>& OutPayload)
	{
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Result.GetPayloadJson());
		return FJsonSerializer::Deserialize(Reader, OutPayload)
			&& OutPayload.IsValid();
	}

	bool TryPlanAndInvokeDelete(
		const FScopedPublicCommandComposition& Harness,
		const FString& TargetRef,
		TOptional<FUeAgentCommandResult>& OutResult,
		FString& OutError)
	{
		if (!TryInvokeCommand(
				Harness.GetRouteHost(), Harness.GetConfig(), TEXT("asset.delete"),
				DeleteParams(TargetRef), OutResult, OutError)
			|| !OutResult.IsSet()
			|| !OutResult->IsOk())
		{
			return false;
		}
		TSharedPtr<FJsonObject> PlanPayload;
		FString TerminalStatus;
		FString AuthorizationId;
		if (!TryParsePayload(OutResult.GetValue(), PlanPayload)
			|| !PlanPayload->TryGetStringField(
				TEXT("terminal_status"), TerminalStatus)
			|| TerminalStatus != TEXT("planned")
			|| !PlanPayload->TryGetStringField(
				TEXT("destructive_authorization_id"), AuthorizationId)
			|| AuthorizationId.IsEmpty())
		{
			OutError = TEXT("asset_delete_plan_contract_invalid");
			return false;
		}
		return TryInvokeCommand(
			Harness.GetRouteHost(), Harness.GetConfig(), TEXT("asset.delete"),
			DeleteParams(TargetRef, AuthorizationId), OutResult, OutError)
			&& OutResult.IsSet();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDestructiveImportPublicAssetDeleteVerticalSliceTest,
	"UeAgentInterface.Composition.DestructiveImport.PublicAssetDeleteVerticalSlice",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDestructiveImportPublicAssetDeleteVerticalSliceTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FScopedAssetDeleteFixture Fixture;
	if (!TestTrue(TEXT("owned asset-delete fixture is created and saved"),
			Fixture.Create()))
	{
		return false;
	}

	AWorldSettings* const WorldSettings = GEditor
		? GEditor->GetEditorWorldContext().World()->GetWorldSettings()
		: nullptr;
	if (!TestNotNull(TEXT("editor world settings"), WorldSettings))
	{
		return false;
	}
	FScopedPublicCommandComposition Harness(TEXT("destructive-public-asset-delete"));
	FString Error;
	if (!TestTrue(TEXT("formal public composition starts"),
			Harness.TryStart(*WorldSettings, Error)))
	{
		AddError(Error);
		return false;
	}

	TestNull(TEXT("retired asset-delete command is absent from formal registry"),
		Harness.GetCommandRegistry()->FindExact(TEXT("asset_delete")));

	TOptional<FUeAgentCommandResult> MissingResult;
	const FString MissingTarget = Fixture.GetPackageName() + TEXT("_Missing");
	if (!TestTrue(TEXT("missing target reaches the formal asset-delete handler"),
			TryPlanAndInvokeDelete(
				Harness, MissingTarget, MissingResult, Error))
		|| !MissingResult.IsSet())
	{
		AddError(Error);
		return false;
	}
	TSharedPtr<FJsonObject> MissingPayload;
	TestFalse(TEXT("missing target fails through the public command envelope"),
		MissingResult->IsOk());
	TestTrue(TEXT("missing target reports rolled_back terminal state"),
		TryParsePayload(MissingResult.GetValue(), MissingPayload)
			&& MissingPayload->GetStringField(TEXT("terminal_status"))
				== TEXT("rolled_back")
			&& MissingResult->GetErrorJson().Contains(
				TEXT("asset_delete_target_missing")));
	TestTrue(TEXT("failed delete leaves the owned fixture durably unchanged"),
		Fixture.Unload() && Fixture.IsDurablyPresent());

	TOptional<FUeAgentCommandResult> DeleteResult;
	if (!TestTrue(TEXT("public asset-delete enters the formal command service"),
			TryPlanAndInvokeDelete(
				Harness, Fixture.GetPackageName(), DeleteResult, Error))
		|| !DeleteResult.IsSet())
	{
		AddError(Error);
		return false;
	}
	TSharedPtr<FJsonObject> DeletePayload;
	if (!TestTrue(TEXT("public asset-delete commits exactly one owned target"),
			DeleteResult->IsOk()
				&& TryParsePayload(DeleteResult.GetValue(), DeletePayload)
				&& DeletePayload->GetStringField(TEXT("terminal_status"))
					== TEXT("committed")
				&& !DeletePayload->GetStringField(
					TEXT("plan_fingerprint")).IsEmpty()))
	{
		AddError(DeleteResult->GetErrorJson());
		return false;
	}
	TestTrue(TEXT("asset-delete independent readback proves durable absence"),
		Fixture.Unload() && Fixture.IsDurablyAbsent());

	FScopedAssetDeleteFixture BlueprintFixture(
		TEXT("BP_InternalGeneratedDelete"), false, true);
	if (!TestTrue(TEXT("Blueprint fixture with generated objects is saved"),
			BlueprintFixture.Create()))
	{
		return false;
	}
	TOptional<FUeAgentCommandResult> BlueprintDeleteResult;
	if (!TestTrue(TEXT("public asset-delete accepts its complete owned delete set"),
			TryPlanAndInvokeDelete(
				Harness, BlueprintFixture.GetPackageName(),
				BlueprintDeleteResult, Error))
		|| !BlueprintDeleteResult.IsSet())
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("same-package Blueprint generated objects do not block delete"),
		BlueprintDeleteResult->IsOk()
			&& BlueprintFixture.Unload()
			&& BlueprintFixture.IsDurablyAbsent());
	TestTrue(TEXT("asset-delete fixture and public composition clean up"),
		Fixture.Cleanup() && BlueprintFixture.Cleanup() && Harness.Stop());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDestructiveImportPublicPackageTargetResolutionTest,
	"UeAgentInterface.Composition.DestructiveImport.PublicPackageTargetResolution",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDestructiveImportPublicPackageTargetResolutionTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	AWorldSettings* const WorldSettings = GEditor
		? GEditor->GetEditorWorldContext().World()->GetWorldSettings()
		: nullptr;
	if (!TestNotNull(TEXT("editor world settings"), WorldSettings))
	{
		return false;
	}
	FScopedPublicCommandComposition Harness(
		TEXT("destructive-package-target-resolution"));
	FString Error;
	if (!TestTrue(TEXT("formal public composition starts"),
			Harness.TryStart(*WorldSettings, Error)))
	{
		AddError(Error);
		return false;
	}

	FScopedAssetDeleteFixture UniqueFixture(TEXT("WorldDataLayers"));
	if (!TestTrue(TEXT("package with a non-leaf asset identity is saved"),
			UniqueFixture.Create()))
	{
		return false;
	}
	TOptional<FUeAgentCommandResult> UniqueResult;
	if (!TestTrue(TEXT("package target resolves its unique registry asset"),
			TryPlanAndInvokeDelete(
				Harness, UniqueFixture.GetPackageName(), UniqueResult, Error))
		|| !UniqueResult.IsSet())
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("resolved non-leaf asset is deleted and verified"),
		UniqueResult->IsOk()
			&& UniqueFixture.Unload()
			&& UniqueFixture.IsDurablyAbsent());

	FScopedAssetDeleteFixture AmbiguousFixture(
		TEXT("PrimaryAsset"), true);
	if (!TestTrue(TEXT("package with two registry assets is saved"),
			AmbiguousFixture.Create()))
	{
		return false;
	}
	TOptional<FUeAgentCommandResult> AmbiguousResult;
	if (!TestTrue(TEXT("ambiguous package target reaches commit owner"),
			TryPlanAndInvokeDelete(
				Harness, AmbiguousFixture.GetPackageName(), AmbiguousResult, Error))
		|| !AmbiguousResult.IsSet())
	{
		AddError(Error);
		return false;
	}
	TestFalse(TEXT("ambiguous package target fails closed"),
		AmbiguousResult->IsOk());
	TestTrue(TEXT("ambiguity has a typed error and preserves the package"),
		AmbiguousResult->GetErrorJson().Contains(
			TEXT("asset_delete_target_ambiguous"))
			&& AmbiguousFixture.Unload()
			&& AmbiguousFixture.IsDurablyPresent());

	TestTrue(TEXT("fixtures and public composition clean up"),
		UniqueFixture.Cleanup()
			&& AmbiguousFixture.Cleanup()
			&& Harness.Stop());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDestructiveImportPackageOnlyDeleteDoesNotLoadTargetTest,
	"UeAgentInterface.Composition.DestructiveImport.PackageOnlyDeleteDoesNotLoadTarget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDestructiveImportPackageOnlyDeleteDoesNotLoadTargetTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FScopedAssetDeleteFixture Fixture;
	if (!TestTrue(TEXT("package-only fixture is created and saved"),
			Fixture.Create())
		|| !TestTrue(TEXT("package-only fixture is unloaded before delete"),
			Fixture.Unload()))
	{
		return false;
	}
	FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles(
		{Fixture.GetFilename()});
	TArray<FAssetData> RegistryAssets;
	if (!TestTrue(TEXT("unloaded fixture Registry query succeeds"),
			FAssetRegistryModule::GetRegistry().GetAssetsByPackageName(
				FName(*Fixture.GetPackageName()), RegistryAssets, false))
		|| !TestEqual(TEXT("unloaded fixture has exactly one Registry asset"),
			RegistryAssets.Num(), 1))
	{
		return false;
	}
	if (!TestEqual(TEXT("unloaded fixture Registry identity is exact"),
			RegistryAssets[0].GetObjectPathString(), Fixture.ObjectPath()))
	{
		return false;
	}
	if (!TestNull(TEXT("target package starts unloaded"),
			FindPackage(nullptr, *Fixture.GetPackageName()))
		|| !TestNull(TEXT("target object starts unloaded"),
			FindObject<UObject>(nullptr, *Fixture.ObjectPath())))
	{
		return false;
	}

	FString Error;
	TSharedPtr<IUeAgentAssetDeleteOwner> Owner;
	if (!TestTrue(TEXT("formal asset-delete owner composes"),
			TryCreateAssetDeleteOwner(
				FPaths::ProjectSavedDir()
					/ TEXT("UAI/Tests/T91/PackageOnlyDeleteJournal"),
				TEXT("t91-package-only-delete"),
				Owner,
				Error))
		|| !Owner.IsValid())
	{
		AddError(Error);
		return false;
	}

	int32 TargetLoadCount = 0;
	const FDelegateHandle AssetLoadedHandle =
		FCoreUObjectDelegates::OnAssetLoaded.AddLambda(
			[&TargetLoadCount, TargetPackage = Fixture.GetPackageName()](
				UObject* Loaded)
			{
				if (Loaded && Loaded->GetOutermost()
					&& Loaded->GetOutermost()->GetName() == TargetPackage)
				{
					++TargetLoadCount;
				}
			});
	ON_SCOPE_EXIT
	{
		FCoreUObjectDelegates::OnAssetLoaded.Remove(AssetLoadedHandle);
	};

	const FUeAgentAssetDeleteResult DeleteResult = Owner->Delete(
		FUeAgentAssetDeleteRequest{
			TEXT("t91-package-only-delete-request"),
			Fixture.GetPackageName(),
			TEXT("t91-package-only-delete-test")},
		MakeShared<FNeverCancelled>());
	if (!TestTrue(TEXT("unloaded package-only delete commits"),
			DeleteResult.IsSuccess()))
	{
		AddError(DeleteResult.ErrorCode + TEXT(":")
			+ DeleteResult.ErrorDetail);
	}
	TestEqual(TEXT("package-only delete never loads the target asset"),
		TargetLoadCount, 0);
	TestTrue(TEXT("package-only delete refreshes durable absence"),
		DeleteResult.bFileAbsent
			&& DeleteResult.bPackageAbsent
			&& DeleteResult.bRegistryAbsent
			&& DeleteResult.bObjectAbsent
			&& Fixture.IsDurablyAbsent());
	TestTrue(TEXT("package-only fixture cleanup is idempotent"),
		Fixture.Cleanup());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDestructiveImportPublicStringTableDeleteTest,
	"UeAgentInterface.Composition.DestructiveImport.PublicStringTableDelete",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDestructiveImportPublicStringTableDeleteTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FScopedStringTableDeleteFixture Fixture;
	if (!TestTrue(TEXT("owned StringTable fixture is created and saved"),
			Fixture.Create()))
	{
		return false;
	}
	FString Error;
	TSharedPtr<IUeAgentAssetDeleteOwner> Owner;
	if (!TestTrue(TEXT("formal asset-delete owner composes"),
			TryCreateAssetDeleteOwner(
				FPaths::ProjectSavedDir()
					/ TEXT("UAI/Tests/T30/StringTableDeleteJournal"),
				TEXT("t30-string-table-delete"),
				Owner,
				Error))
		|| !Owner.IsValid())
	{
		AddError(Error);
		return false;
	}
	const FUeAgentAssetDeleteResult DeleteResult = Owner->Delete(
		FUeAgentAssetDeleteRequest{
			TEXT("t30-string-table-delete-request"),
			Fixture.GetPackageName(),
			TEXT("t30-string-table-delete-test")},
		MakeShared<FNeverCancelled>());
	if (!TestTrue(TEXT("StringTable registry references are released for delete"),
			DeleteResult.IsSuccess()))
	{
		AddError(DeleteResult.ErrorCode + TEXT(":") + DeleteResult.ErrorDetail);
	}
	TestTrue(TEXT("StringTable delete has durable independent absence"),
		Fixture.IsDurablyAbsent());
	TestTrue(TEXT("StringTable fixture and public composition clean up"),
		Fixture.Cleanup());
	return !HasAnyErrors();
}

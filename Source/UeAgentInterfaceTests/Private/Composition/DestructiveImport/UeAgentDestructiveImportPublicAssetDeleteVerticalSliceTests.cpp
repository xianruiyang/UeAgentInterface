#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Curves/CurveFloat.h"
#include "DomainAdapters/AssetDestructiveImport/UeAgentAssetDeleteOwner.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PackageTools.h"
#include "ObjectTools.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Server/UeAgentCommandRegistry.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

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
		FScopedAssetDeleteFixture()
			: PackageName(
				TEXT("/Game/UAI/Tests/DestructiveImport/DA_PublicSlice_")
				+ FGuid::NewGuid().ToString(EGuidFormats::Digits))
			, AssetName(FPackageName::GetLongPackageAssetName(PackageName))
		{
		}

		~FScopedAssetDeleteFixture()
		{
			Cleanup();
		}

		bool Create()
		{
			UPackage* const Package = CreatePackage(*PackageName);
			UCurveFloat* const Asset = NewObject<UCurveFloat>(
				Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
			if (Package == nullptr || Asset == nullptr)
			{
				return false;
			}
			FAssetRegistryModule::AssetCreated(Asset);
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
			return LoadObject<UCurveFloat>(nullptr, *ObjectPath()) != nullptr;
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
			}
			bClean = Unload() && bClean;
			if (IFileManager::Get().FileExists(*Filename()))
			{
				bClean = IFileManager::Get().Delete(*Filename(), false, true)
					&& bClean;
			}
			return IsDurablyAbsent() && bClean;
		}

		const FString& GetPackageName() const { return PackageName; }

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

	FString DeleteParams(const FString& TargetRef)
	{
		return TEXT("{\"target_ref\":\"") + TargetRef + TEXT("\"}");
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

	TestNull(TEXT("retired exact legacy asset-delete owner is absent"),
		UeAgentCommandRegistry::FindExactCommandInvoker(TEXT("asset_delete")));
	TestNull(TEXT("retired prefix legacy asset-delete owner is absent"),
		UeAgentCommandRegistry::FindPrefixedCommandInvoker(TEXT("asset_delete")));
	TestTrue(TEXT("retired legacy asset-delete command fails closed"),
		UeAgentCommandRegistry::IsExplicitlyUnsupportedCommand(TEXT("asset_delete")));

	TOptional<FUeAgentCommandResult> MissingResult;
	const FString MissingTarget = Fixture.GetPackageName() + TEXT("_Missing");
	if (!TestTrue(TEXT("missing target reaches the formal asset-delete handler"),
			TryInvokeCommand(
				Harness.GetRouteHost(), Harness.GetConfig(), TEXT("asset.delete"),
				DeleteParams(MissingTarget), MissingResult, Error))
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
			&& MissingPayload->GetStringField(TEXT("terminal_state"))
				== TEXT("rolled_back")
			&& MissingPayload->GetStringField(TEXT("target_ref")) == MissingTarget
			&& MissingResult->GetErrorJson().Contains(
				TEXT("asset_delete_target_missing")));
	TestTrue(TEXT("failed delete leaves the owned fixture durably unchanged"),
		Fixture.Unload() && Fixture.IsDurablyPresent());

	TOptional<FUeAgentCommandResult> DeleteResult;
	if (!TestTrue(TEXT("public asset-delete enters the formal command service"),
			TryInvokeCommand(
				Harness.GetRouteHost(), Harness.GetConfig(), TEXT("asset.delete"),
				DeleteParams(Fixture.GetPackageName()), DeleteResult, Error))
		|| !DeleteResult.IsSet())
	{
		AddError(Error);
		return false;
	}
	TSharedPtr<FJsonObject> DeletePayload;
	if (!TestTrue(TEXT("public asset-delete commits exactly one owned target"),
			DeleteResult->IsOk()
				&& TryParsePayload(DeleteResult.GetValue(), DeletePayload)
				&& DeletePayload->GetBoolField(TEXT("deleted"))
				&& DeletePayload->GetStringField(TEXT("terminal_state"))
					== TEXT("committed")
				&& DeletePayload->GetStringField(TEXT("target_ref"))
					== Fixture.GetPackageName()))
	{
		AddError(DeleteResult->GetErrorJson());
		return false;
	}
	TestTrue(TEXT("asset-delete independent readback proves durable absence"),
		Fixture.Unload() && Fixture.IsDurablyAbsent());
	TestTrue(TEXT("asset-delete fixture and public composition clean up"),
		Fixture.Cleanup() && Harness.Stop());
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

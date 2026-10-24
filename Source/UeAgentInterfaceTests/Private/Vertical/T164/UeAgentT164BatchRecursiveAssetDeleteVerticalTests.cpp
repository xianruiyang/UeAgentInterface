#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Curves/CurveFloat.h"
#include "Editor.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
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
	using namespace UE::UAI::Tests::PublicCommand;

	struct FAssetIdentity
	{
		FString PackageName;
		FString AssetName;

		FString ObjectPath() const
		{
			return PackageName + TEXT(".") + AssetName;
		}

		FString Filename() const
		{
			return FPackageName::LongPackageNameToFilename(
				PackageName, FPackageName::GetAssetPackageExtension());
		}
	};

	class FScopedT164Assets final
	{
	public:
		FScopedT164Assets()
			: Root(TEXT("/Game/UAI_Automation/T164_")
				+ FGuid::NewGuid().ToString(EGuidFormats::Digits))
			, DirectA{Root + TEXT("/Root/Direct/DirectA"), TEXT("DirectA")}
			, NestedB{Root + TEXT("/Root/Nested/NestedB"), TEXT("NestedB")}
			, BaseMaterial{Root + TEXT("/Root/Blocked/M_Base"), TEXT("M_Base")}
			, OutsideInstance{Root + TEXT("/Outside/MI_Ref"), TEXT("MI_Ref")}
			, SiblingKeep{Root + TEXT("/Sibling/SiblingKeep"), TEXT("SiblingKeep")}
		{
			All = {DirectA, NestedB, BaseMaterial, OutsideInstance, SiblingKeep};
		}

		~FScopedT164Assets()
		{
			Cleanup();
		}

		bool Create()
		{
			UCurveFloat* const Direct = CreateAsset<UCurveFloat>(DirectA);
			UCurveFloat* const Nested = CreateAsset<UCurveFloat>(NestedB);
			UMaterial* const Material = CreateAsset<UMaterial>(BaseMaterial);
			UCurveFloat* const Sibling = CreateAsset<UCurveFloat>(SiblingKeep);
			if (!Direct || !Nested || !Material || !Sibling)
			{
				return false;
			}

			UMaterialInstanceConstant* const Instance =
				CreateAsset<UMaterialInstanceConstant>(OutsideInstance);
			if (!Instance)
			{
				return false;
			}
			Instance->SetParentEditorOnly(Material);
			Instance->PostEditChange();
			Instance->MarkPackageDirty();
			if (!Save(OutsideInstance, Instance))
			{
				return false;
			}

			TArray<FString> Filenames;
			for (const FAssetIdentity& Identity : All)
			{
				Filenames.Add(Identity.Filename());
			}
			FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles(Filenames);
			return true;
		}

		bool IsPresent(const FAssetIdentity& Identity) const
		{
			TArray<FAssetData> Assets;
			FString ExistingFilename;
			FAssetRegistryModule::GetRegistry().GetAssetsByPackageName(
				FName(*Identity.PackageName), Assets, false);
			return IFileManager::Get().FileExists(*Identity.Filename())
				&& FPackageName::DoesPackageExist(
					Identity.PackageName, &ExistingFilename)
				&& LoadObject<UObject>(nullptr, *Identity.ObjectPath()) != nullptr
				&& Assets.ContainsByPredicate(
					[&Identity](const FAssetData& Asset)
					{
						return Asset.GetObjectPathString() == Identity.ObjectPath();
					});
		}

		bool IsAbsent(const FAssetIdentity& Identity) const
		{
			TArray<FAssetData> Assets;
			FAssetRegistryModule::GetRegistry().GetAssetsByPackageName(
				FName(*Identity.PackageName), Assets, false);
			FString ExistingFilename;
			return !IFileManager::Get().FileExists(*Identity.Filename())
				&& !FPackageName::DoesPackageExist(
					Identity.PackageName, &ExistingFilename)
				&& FindObject<UObject>(nullptr, *Identity.ObjectPath()) == nullptr
				&& Assets.IsEmpty();
		}

		bool Cleanup()
		{
			TArray<UObject*> Objects;
			for (const FAssetIdentity& Identity : All)
			{
				if (UObject* const Object = LoadObject<UObject>(
						nullptr, *Identity.ObjectPath()))
				{
					Objects.Add(Object);
				}
			}
			if (!Objects.IsEmpty())
			{
				TGuardValue<bool> UnattendedGuard(GIsRunningUnattendedScript, true);
				ObjectTools::ForceDeleteObjects(Objects, false);
			}
			CollectGarbage(RF_NoFlags, true);

			TArray<FString> Filenames;
			for (const FAssetIdentity& Identity : All)
			{
				const FString Filename = Identity.Filename();
				Filenames.Add(Filename);
				if (IFileManager::Get().FileExists(*Filename))
				{
					IFileManager::Get().Delete(*Filename, false, true);
				}
			}
			FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles(Filenames);
			CollectGarbage(RF_NoFlags, true);
			for (const FAssetIdentity& Identity : All)
			{
				if (!IsAbsent(Identity))
				{
					return false;
				}
			}
			const FString RootDirectory =
				FPackageName::LongPackageNameToFilename(Root);
			return IFileManager::Get().DeleteDirectory(
				*RootDirectory, false, true)
				|| !IFileManager::Get().DirectoryExists(*RootDirectory);
		}

		FString RootFolder() const { return Root + TEXT("/Root"); }

		FString Root;
		FAssetIdentity DirectA;
		FAssetIdentity NestedB;
		FAssetIdentity BaseMaterial;
		FAssetIdentity OutsideInstance;
		FAssetIdentity SiblingKeep;

	private:
		template <typename T>
		T* CreateAsset(const FAssetIdentity& Identity)
		{
			UPackage* const Package = CreatePackage(*Identity.PackageName);
			if (!Package)
			{
				return nullptr;
			}
			T* const Asset = NewObject<T>(
				Package, *Identity.AssetName,
				RF_Public | RF_Standalone | RF_Transactional);
			if (!Asset)
			{
				return nullptr;
			}
			FAssetRegistryModule::AssetCreated(Asset);
			Package->MarkPackageDirty();
			return Save(Identity, Asset) ? Asset : nullptr;
		}

		bool Save(const FAssetIdentity& Identity, UObject* Asset) const
		{
			FSavePackageArgs Args;
			Args.TopLevelFlags = RF_Public | RF_Standalone;
			return UPackage::SavePackage(
				Asset->GetOutermost(), Asset, *Identity.Filename(), Args);
		}

		TArray<FAssetIdentity> All;
	};

	FString DeleteParams(
		const TArray<FString>& Targets,
		const bool bConfirm = false)
	{
		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& Target : Targets)
		{
			Values.Add(MakeShared<FJsonValueString>(Target));
		}
		Root->SetArrayField(TEXT("targets"), MoveTemp(Values));
		Root->SetBoolField(TEXT("recursive"), true);
		Root->SetBoolField(TEXT("confirm"), bConfirm);
		FString Json;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
		FJsonSerializer::Serialize(Root, Writer);
		return Json;
	}

	bool ParsePayload(
		const FJsonObject& Result,
		TSharedPtr<FJsonObject>& OutPayload)
	{
		const TSharedPtr<FJsonObject>* Payload = nullptr;
		if (!Result.TryGetObjectField(TEXT("payload"), Payload)) return false;
		OutPayload = *Payload;
		return OutPayload.IsValid();
	}

	const TSharedPtr<FJsonObject>* FindTarget(
		const TArray<TSharedPtr<FJsonValue>>& Targets,
		const FString& PackageName)
	{
		for (const TSharedPtr<FJsonValue>& Value : Targets)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (Value.IsValid() && Value->TryGetObject(Object) && Object
				&& (*Object)->GetStringField(TEXT("package_name")) == PackageName)
			{
				return Object;
			}
		}
		return nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT164BatchRecursiveAssetDeleteVerticalTest,
	"UeAgentInterface.Vertical.T164.BatchRecursiveAssetDelete",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT164BatchRecursiveAssetDeleteVerticalTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FScopedT164Assets Fixture;
	if (!TestTrue(TEXT("fresh T164 assets and external reference are saved"),
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
	FScopedPublicCommandComposition Harness(TEXT("t164-batch-recursive-delete"));
	FString Error;
	if (!TestTrue(TEXT("formal public composition starts"),
			Harness.TryStart(*WorldSettings, Error)))
	{
		AddError(Error);
		return false;
	}

	const TArray<FString> RequestedTargets = {
		Fixture.DirectA.ObjectPath(), Fixture.RootFolder()};
	TSharedPtr<FJsonObject> PlanResult;
	if (!TestTrue(TEXT("overlapping asset and recursive folder reach planning"),
			TryInvokeCommand(
				Harness.GetRouteHost(), Harness.GetConfig(), TEXT("asset.delete"),
				DeleteParams(RequestedTargets), PlanResult, Error))
		|| !PlanResult.IsValid() || !PlanResult->GetBoolField(TEXT("ok")))
	{
		AddError(Error);
		return false;
	}

	TSharedPtr<FJsonObject> Plan;
	const TArray<TSharedPtr<FJsonValue>>* PlanTargets = nullptr;
	if (!TestTrue(TEXT("plan exposes readable impact and three deduplicated targets"),
			ParsePayload(*PlanResult, Plan)
				&& Plan->GetStringField(TEXT("status")) == TEXT("planned")
				&& !Plan->HasField(TEXT("operation_id"))
				&& !Plan->HasField(TEXT("plan_fingerprint"))
				&& !Plan->HasField(TEXT("destructive_authorization_id"))
				&& !Plan->HasField(TEXT("revision_snapshot"))
				&& Plan->TryGetArrayField(TEXT("targets"), PlanTargets)
				&& PlanTargets && PlanTargets->Num() == 3
				&& Plan->HasTypedField<EJson::Array>(TEXT("impact_scope"))))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* DirectPlan =
		FindTarget(*PlanTargets, Fixture.DirectA.PackageName);
	const TSharedPtr<FJsonObject>* NestedPlan =
		FindTarget(*PlanTargets, Fixture.NestedB.PackageName);
	const TSharedPtr<FJsonObject>* MaterialPlan =
		FindTarget(*PlanTargets, Fixture.BaseMaterial.PackageName);
	TestNotNull(TEXT("plan resolves DirectA once"), DirectPlan);
	TestNotNull(TEXT("plan resolves nested NestedB"), NestedPlan);
	if (!TestNotNull(TEXT("plan resolves blocked M_Base"), MaterialPlan))
	{
		return false;
	}
	TestEqual(TEXT("M_Base plan blocker is its external referencer"),
		(*MaterialPlan)->GetStringField(TEXT("blocker_code")),
		FString(TEXT("asset_delete_package_referenced")));
	TestNull(TEXT("outside MI is not an actual delete target"),
		FindTarget(*PlanTargets, Fixture.OutsideInstance.PackageName));
	TestNull(TEXT("same-prefix sibling is outside the folder boundary"),
		FindTarget(*PlanTargets, Fixture.SiblingKeep.PackageName));

	TArray<FString> ChangedTargets = RequestedTargets;
	ChangedTargets.Add(Fixture.SiblingKeep.ObjectPath());
	TSharedPtr<FJsonObject> StaleResult;
	if (!TestTrue(TEXT("changed parameters reject the pending confirmation"),
			TryInvokeCommand(
				Harness.GetRouteHost(), Harness.GetConfig(), TEXT("asset.delete"),
				DeleteParams(ChangedTargets, true), StaleResult, Error))
		|| !StaleResult.IsValid()
		|| StaleResult->GetBoolField(TEXT("ok"))
		|| !PublicErrorText(StaleResult).Contains(TEXT("destructive_confirmation_stale")))
	{
		AddError(Error);
		return false;
	}

	TSharedPtr<FJsonObject> ReplannedResult;
	if (!TestTrue(TEXT("unchanged parameters can be reviewed again after stale rejection"),
			TryInvokeCommand(
				Harness.GetRouteHost(), Harness.GetConfig(), TEXT("asset.delete"),
				DeleteParams(RequestedTargets), ReplannedResult, Error))
		|| !ReplannedResult.IsValid() || !ReplannedResult->GetBoolField(TEXT("ok")))
	{
		AddError(Error);
		return false;
	}

	TSharedPtr<FJsonObject> CommitResult;
	if (!TestTrue(TEXT("unchanged request commits with confirm=true"),
			TryInvokeCommand(
				Harness.GetRouteHost(), Harness.GetConfig(), TEXT("asset.delete"),
				DeleteParams(RequestedTargets, true),
				CommitResult, Error))
		|| !CommitResult.IsValid())
	{
		AddError(Error);
		return false;
	}
	TestFalse(TEXT("partial delete retains the command failure envelope"),
		CommitResult->GetBoolField(TEXT("ok")));

	TSharedPtr<FJsonObject> Commit;
	const TArray<TSharedPtr<FJsonValue>>* ResultTargets = nullptr;
	if (!TestTrue(TEXT("partial commit exposes only readable batch outcome"),
			ParsePayload(*CommitResult, Commit)
				&& Commit->GetStringField(TEXT("status"))
					== TEXT("partially_committed")
				&& !Commit->HasField(TEXT("operation_id"))
				&& !Commit->HasField(TEXT("plan_fingerprint"))
				&& !Commit->HasField(TEXT("destructive_authorization_id"))
				&& !Commit->HasField(TEXT("revision_snapshot"))
				&& Commit->GetIntegerField(TEXT("deleted_count")) == 2
				&& Commit->GetIntegerField(TEXT("blocked_count")) == 1
				&& Commit->GetIntegerField(TEXT("remaining_count")) == 1
				&& Commit->TryGetArrayField(TEXT("targets"), ResultTargets)
				&& ResultTargets && ResultTargets->Num() == 3))
	{
		AddError(PublicErrorText(CommitResult));
		return false;
	}
	const TSharedPtr<FJsonObject>* MaterialResult =
		FindTarget(*ResultTargets, Fixture.BaseMaterial.PackageName);
	if (!TestNotNull(TEXT("commit projection retains M_Base result"), MaterialResult))
	{
		return false;
	}
	TestEqual(TEXT("blocked target retains its typed blocker"),
		(*MaterialResult)->GetStringField(TEXT("error_code")),
		FString(TEXT("asset_delete_package_referenced")));
	TestTrue(TEXT("blocked target remains present"),
		(*MaterialResult)->GetBoolField(TEXT("remaining")));

	TestTrue(TEXT("DirectA is absent across registry, package, file, and object"),
		Fixture.IsAbsent(Fixture.DirectA));
	TestTrue(TEXT("NestedB is absent across registry, package, file, and object"),
		Fixture.IsAbsent(Fixture.NestedB));
	TestTrue(TEXT("M_Base remains present"),
		Fixture.IsPresent(Fixture.BaseMaterial));
	TestTrue(TEXT("external MI referencer remains present"),
		Fixture.IsPresent(Fixture.OutsideInstance));
	TestTrue(TEXT("same-prefix sibling remains present"),
		Fixture.IsPresent(Fixture.SiblingKeep));
	TestTrue(TEXT("fresh T164 residue and public composition clean up"),
		Fixture.Cleanup() && Harness.Stop());
	return !HasAnyErrors();
}

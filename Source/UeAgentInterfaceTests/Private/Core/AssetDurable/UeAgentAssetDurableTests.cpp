#include "Misc/AutomationTest.h"

#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Core/AssetDurable/UeAgentPackageHeader.h"
#include "Core/AssetDurable/UeAgentPackageAssetRevision.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/LinkerLoad.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"


namespace
{
	using namespace UE::UAI::AssetDurable;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::PlanningPreflight;
	using namespace UE::UAI::StateSemantic;

	static FUeAgentResourceTracker MakeTracker(
		const int64 WorkLimit = 262144,
		const int64 FileLimit = 4096)
	{
		return FUeAgentResourceTracker();
	}

	static FUeAgentCanonicalIdentity MakeIdentity(
		const bool bDelete)
	{
		return FUeAgentCanonicalIdentity(
			bDelete
				? TEXT("uai.reference.asset.presence")
				: TEXT("uai.reference.asset.title"),
			TEXT("reference_asset"),
			bDelete
				? TEXT("/Game/UAI/P4_009/DeleteAsset")
				: TEXT("/Game/UAI/P4_009/SaveAsset"),
			bDelete ? TEXT("presence") : TEXT("title"),
			bDelete ? TEXT("asset") : TEXT("asset_property"),
			TEXT("editor-session-21"));
	}

	static FUeAgentCanonicalValueRef MakeString(
		const FString& Value,
		FUeAgentResourceTracker& Tracker)
	{
		FUeAgentCanonicalValuePtr Result;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryString(
			Value,
			Tracker,
			Result,
			Error));
		return Result.ToSharedRef();
	}

	static FUeAgentCanonicalSnapshotRef MakeSnapshot(
		FUeAgentResourceTracker& Tracker)
	{
		const FUeAgentStateRevision Revision(
			TEXT("fixture"),
			TEXT("editor-session-21"),
			73);
		TArray<FUeAgentStateEntry> Entries;
		for (const bool bDelete : {false, true})
		{
			const FString Current =
				bDelete ? TEXT("present") : TEXT("old-title");
			Entries.Emplace(
				MakeIdentity(bDelete),
				FUeAgentCanonicalType(
					TEXT("string"),
					EUeAgentValueKind::String,
					false,
					TEXT("scalar")),
				MakeString(Current, Tracker),
				FUeAgentDefaultValue(
					EUeAgentDefaultSourceKind::Schema,
					TEXT("fixture:c8d:string-default"),
					TEXT("checked-in C8D fixture"),
					MakeString(TEXT(""), Tracker)),
				FUeAgentStateDetails(
					bDelete ? TEXT("delete_asset") : TEXT("save_asset"),
					TEXT(""),
					{TEXT("P4-009")},
					true,
					TEXT("available"),
					{}),
				Revision);
		}
		FUeAgentCanonicalSnapshotPtr Snapshot;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalSnapshot::TryCreate(
			TEXT("snapshot:c8d:fixture"),
			EUeAgentStatePlane::AuthoringMemory,
			Revision,
			FUeAgentTemporalPoint(
				TEXT("fixture-clock"),
				TEXT("73")),
			FUeAgentStateProvenance(
				TEXT("independent_readback"),
				TEXT("fixture:c8d"),
				TEXT("fixture:c8d:independent-readback")),
			Entries,
			Tracker,
			Snapshot,
			Error));
		return Snapshot.ToSharedRef();
	}

	static FPlanningPreflightPlanRef MakePlan(
		FUeAgentResourceTracker& Tracker)
	{
		TArray<FChangeDraft> Changes;
		Changes.Emplace(
			MakeIdentity(false),
			EChangeIntent::Update,
			MakeString(TEXT("old-title"), Tracker),
			MakeString(TEXT("new-title"), Tracker),
			TArray<FString>{TEXT("uai:asset:write")},
			TEXT("adapter:reference-asset:save"),
			TArray<FUeAgentCanonicalIdentity>{});
		Changes.Emplace(
			MakeIdentity(true),
			EChangeIntent::Delete,
			MakeString(TEXT("present"), Tracker),
			FUeAgentCanonicalValuePtr(),
			TArray<FString>{TEXT("uai:asset:delete")},
			TEXT("adapter:reference-asset:delete"),
			TArray<FUeAgentCanonicalIdentity>{});
		const FPlanningRequest Request(
			FChangeSetDraft(
				FUeAgentDescriptorIdentity(
					TEXT("uai.reference.asset.update"),
					1,
					TEXT(
						"sha256:24b70c881cdd8dc01b925ba4f7d15b630b7e5f178f3623fb54cb38195a3dff11")),
				TEXT("p4-009:asset-durable:fixture"),
				TEXT("p4-009-asset-durable-001"),
				MakeSnapshot(Tracker),
				MoveTemp(Changes)),
			FUeAgentCallerContext(
				TEXT("agent:p4-009"),
				{
					TEXT("uai:asset:write"),
					TEXT("uai:asset:delete")
				},
				TEXT("p4-009:correlation")),
			false);
		FPlanningResult Result;
		FPlanningPreflightError Error;
		check(TryPlanChanges(Request, Tracker, Result, Error));
		return Result.GetPlan().ToSharedRef();
	}

	struct FOwnedDiskFixture
	{
		explicit FOwnedDiskFixture(const FString& Leaf)
		{
			Root = FPaths::ConvertRelativePathToFull(
				FPaths::ProjectIntermediateDir()
				/ TEXT("UAI/P4_009")
				/ Leaf
				/ FGuid::NewGuid().ToString(EGuidFormats::Digits));
			FinalRoot = Root / TEXT("final");
			JournalRoot = Root / TEXT("journal");
			IFileManager::Get().MakeDirectory(*FinalRoot, true);
			SaveHeader = FinalRoot / TEXT("save_asset.uasset");
			SavePayload = FinalRoot / TEXT("save_asset.upayload");
			DeleteHeader = FinalRoot / TEXT("delete_asset.uasset");
			DeletePayload = FinalRoot / TEXT("delete_asset.upayload");
			ExternalHeader = FinalRoot / TEXT("external_actor.uasset");
			ExternalPayload = FinalRoot / TEXT("external_actor.upayload");
			UnrelatedHeader = FinalRoot / TEXT("unrelated.uasset");
			FFileHelper::SaveStringToFile(TEXT("old-save-header"), *SaveHeader);
			FFileHelper::SaveStringToFile(TEXT("old-save-payload"), *SavePayload);
			FFileHelper::SaveStringToFile(
				TEXT("old-delete-header"),
				*DeleteHeader);
			FFileHelper::SaveStringToFile(
				TEXT("old-delete-payload"),
				*DeletePayload);
			FFileHelper::SaveStringToFile(
				TEXT("unrelated"),
				*UnrelatedHeader);
		}

		~FOwnedDiskFixture()
		{
			IFileManager::Get().DeleteDirectory(*Root, false, true);
		}

		FString Root;
		FString FinalRoot;
		FString JournalRoot;
		FString SaveHeader;
		FString SavePayload;
		FString DeleteHeader;
		FString DeletePayload;
		FString ExternalHeader;
		FString ExternalPayload;
		FString UnrelatedHeader;
	};

	struct FNativePackageFixture
	{
		explicit FNativePackageFixture(
			const bool bUseDifferentAssetName = false)
		{
			const FString Suffix =
				FGuid::NewGuid().ToString(EGuidFormats::Digits);
			PackageName =
				TEXT("/Game/UAI/P4_010/Native_") + Suffix;
			AssetName = bUseDifferentAssetName
				? TEXT("ExternalActorLike_") + Suffix
				: FPackageName::GetLongPackageAssetName(PackageName);
			Root = FPaths::ConvertRelativePathToFull(
				FPaths::ProjectIntermediateDir()
					/ TEXT("UAI/P4_010/native")
					/ Suffix);
			Package = CreatePackage(*PackageName);
			Asset = NewObject<UTexture2D>(
				Package,
				*AssetName,
				RF_Public | RF_Standalone);
			check(Package);
			check(Asset);
			Asset->AddToRoot();
			Package->MarkPackageDirty();
			check(FPackageName::TryConvertLongPackageNameToFilename(
				PackageName,
				HeaderPath,
				FPackageName::GetAssetPackageExtension()));
			HeaderPath = FPaths::ConvertRelativePathToFull(
				HeaderPath);
			PayloadPath = FPaths::ChangeExtension(
				HeaderPath,
				TEXT("upayload"));
		}

		~FNativePackageFixture()
		{
			if (Asset)
			{
				Asset->RemoveFromRoot();
			}
			if (Package)
			{
				ResetLoaders(Package);
			}
			IFileManager::Get().Delete(
				*HeaderPath,
				false,
				true,
				true);
			IFileManager::Get().Delete(
				*PayloadPath,
				false,
				true,
				true);
			IFileManager::Get().DeleteDirectory(
				*Root,
				false,
				true);
		}

		FString Root;
		FString PackageName;
		FString AssetName;
		FString HeaderPath;
		FString PayloadPath;
		UPackage* Package = nullptr;
		UObject* Asset = nullptr;
	};

	class FRecordingAssetBackend final : public IAssetDurableBackend
	{
	public:
		explicit FRecordingAssetBackend(FOwnedDiskFixture& InFixture)
			: Fixture(InFixture)
		{
		}

		virtual TArray<FString> GetAllowedArtifactRoots() const override
		{
			return {Fixture.FinalRoot};
		}

		virtual bool SupportsAtomicity(
			const EAtomicityMode Mode) const override
		{
			return bSupportsAtomicity
				&& Mode != EAtomicityMode::NonAtomicRejected;
		}

		virtual bool BuildInventory(
			const FAssetDurableRequest& Request,
			FUeAgentResourceTracker& Resources,
			FDurableInventory& OutInventory,
			FAssetDurableError& OutError) override
		{
			Events.Add(TEXT("build_inventory"));
			if (bEscapeArtifactPath)
			{
				OutInventory = FDurableInventory(
					{
						FDurableArtifact(
							TEXT("escaped"),
							Request.GetTargets()[0].GetIdentity(),
							Request.GetTargets()[0].GetPackageName(),
							Fixture.Root / TEXT("outside.uasset"),
							Request.GetTargets()[0].GetIntent(),
							TEXT("package_header"))
					},
					TEXT("sha256:escaped"));
				return true;
			}
			TArray<FDurableArtifact> Artifacts;
			for (const FAssetDurableTarget& Target : Request.GetTargets())
			{
				FString Prefix;
				if (Target.GetPackageName().EndsWith(TEXT("/SaveAsset")))
				{
					Prefix = TEXT("save_asset");
				}
				else if (Target.GetPackageName().EndsWith(
					TEXT("/DeleteAsset")))
				{
					Prefix = TEXT("delete_asset");
				}
				else if (Target.GetPackageName().EndsWith(
					TEXT("/ExternalActor")))
				{
					Prefix = TEXT("external_actor");
				}
				else
				{
					Prefix = FPackageName::GetLongPackageAssetName(
						Target.GetPackageName()).ToLower();
				}
				const FString ArtifactIdPrefix =
					Target.GetIdentity().GetCanonicalKey()
					+ TEXT(":")
					+ Target.GetDurableTargetKey();
				Artifacts.Emplace(
					ArtifactIdPrefix
						+ TEXT(":package_header"),
					Target.GetIdentity(),
					Target.GetPackageName(),
					Fixture.FinalRoot / (Prefix + TEXT(".uasset")),
					Target.GetIntent(),
					TEXT("package_header"));
				Artifacts.Emplace(
					ArtifactIdPrefix
						+ TEXT(":package_payload"),
					Target.GetIdentity(),
					Target.GetPackageName(),
					Fixture.FinalRoot / (Prefix + TEXT(".upayload")),
					Target.GetIntent(),
					TEXT("package_payload"));
			}
			Artifacts.Sort(
				[](const FDurableArtifact& Left,
					const FDurableArtifact& Right)
				{
					return Left.GetArtifactId() < Right.GetArtifactId();
				});
			FString InventoryHash;
			if (!TryComputeDurableInventoryHash(
				Artifacts,
				Resources,
				InventoryHash,
				OutError))
			{
				return false;
			}
			OutInventory = FDurableInventory(
				MoveTemp(Artifacts),
				MoveTemp(InventoryHash));
			return true;
		}

		virtual bool CaptureBackups(
			const FAssetDurableSessionRef& Session,
			FUeAgentResourceTracker& Resources,
			FAssetDurableError& OutError) override
		{
			Events.Add(TEXT("capture_backups"));
			if (bFailBackup)
			{
				OutError = FAssetDurableError(TEXT("fixture_backup_failed"));
				return false;
			}
			for (const FDurableArtifact& Artifact :
				Session->GetInventory().GetArtifacts())
			{
				FString Text;
				const bool bExists = FFileHelper::LoadFileToString(
					Text,
					*Artifact.GetFinalPath());
				Backups.Add(
					Artifact.GetFinalPath(),
					bExists ? TOptional<FString>(Text) : TOptional<FString>());
			}
			return true;
		}

		virtual bool Stage(
			const FAssetDurableSessionRef& Session,
			FUeAgentResourceTracker& Resources,
			FAssetDurableError& OutError) override
		{
			MutationEvents.Add(TEXT("stage"));
			IFileManager::Get().MakeDirectory(
				*Session->GetStagingRoot(),
				true);
			for (const FDurableArtifact& Artifact :
				Session->GetInventory().GetArtifacts())
			{
				if (Artifact.GetIntent() == EAssetDurableIntent::Delete)
				{
					continue;
				}
				const FString StagePath =
					Session->GetStagingRoot()
					/ FPaths::GetCleanFilename(Artifact.GetFinalPath());
				if (!FFileHelper::SaveStringToFile(
					TEXT("new:") + Artifact.GetArtifactId(),
					*StagePath))
				{
					OutError = FAssetDurableError(
						TEXT("fixture_stage_failed"));
					return false;
				}
			}
			return true;
		}

		virtual bool Publish(
			const FAssetDurableSessionRef& Session,
			FUeAgentResourceTracker& Resources,
			FAssetDurableError& OutError) override
		{
			MutationEvents.Add(TEXT("publish"));
			if (bFailPublish)
			{
				OutError = FAssetDurableError(
					TEXT("fixture_publish_failed"));
				return false;
			}
			for (const FDurableArtifact& Artifact :
				Session->GetInventory().GetArtifacts())
			{
				if (Artifact.GetIntent() == EAssetDurableIntent::Delete)
				{
					IFileManager::Get().Delete(
						*Artifact.GetFinalPath(),
						false,
						true,
						true);
					continue;
				}
				const FString StagePath =
					Session->GetStagingRoot()
					/ FPaths::GetCleanFilename(Artifact.GetFinalPath());
				if (!IFileManager::Get().Move(
					*Artifact.GetFinalPath(),
					*StagePath,
					true,
					true,
					false,
					true))
				{
					OutError = FAssetDurableError(
						TEXT("fixture_publish_move_failed"));
					return false;
				}
			}
			if (bFailPublishAfterMutation)
			{
				OutError = FAssetDurableError(
					TEXT("fixture_publish_failed_after_mutation"));
				return false;
			}
			return true;
		}

		virtual bool Restore(
			const FAssetDurableSessionRef& Session,
			FUeAgentResourceTracker& Resources,
			bool& bOutConfirmed,
			FAssetDurableError& OutError) override
		{
			MutationEvents.Add(TEXT("restore"));
			for (const auto& Pair : Backups)
			{
				if (Pair.Value.IsSet())
				{
					FFileHelper::SaveStringToFile(
						Pair.Value.GetValue(),
						*Pair.Key);
				}
				else
				{
					IFileManager::Get().Delete(
						*Pair.Key,
						false,
						true,
						true);
				}
			}
			bOutConfirmed = bRestoreConfirmed;
			return true;
		}

		virtual bool Inspect(
			const FAssetDurableSessionRef& Session,
			FUeAgentResourceTracker& Resources,
			FString& OutObservedState,
			FAssetDurableError& OutError) override
		{
			Events.Add(TEXT("inspect"));
			OutObservedState = ObservedState;
			return true;
		}

		FOwnedDiskFixture& Fixture;
		TArray<FString> Events;
		TArray<FString> MutationEvents;
		TMap<FString, TOptional<FString>> Backups;
		bool bFailPublish = false;
		bool bFailBackup = false;
		bool bFailPublishAfterMutation = false;
		bool bRestoreConfirmed = true;
		bool bEscapeArtifactPath = false;
		bool bSupportsAtomicity = true;
		FString ObservedState = TEXT("original");
	};

	class FAcceptingReconciliationOracle final
		: public IAssetDurableReconciliationOracle
	{
	public:
		virtual bool TryConfirm(
			const FAssetDurableSessionRef& Session,
			const TArray<FDurableArtifactState>& ObservedStates,
			const EDurableStatus ProposedStatus,
			FUeAgentResourceTracker& Resources,
			FAssetDurableError& OutError) override
		{
			++ConfirmCount;
			LastStatus = ProposedStatus;
			OutError = FAssetDurableError();
			return Session->GetInventory().GetArtifacts().Num()
				== ObservedStates.Num();
		}

		int32 ConfirmCount = 0;
		EDurableStatus LastStatus = EDurableStatus::StateUnknown;
	};

	static FAssetDurableRequest MakeRequest(
		FUeAgentResourceTracker& Tracker,
		const EAtomicityMode Atomicity =
			EAtomicityMode::MultiOwnerAtomicSupported)
	{
		return FAssetDurableRequest(
			MakePlan(Tracker),
			{
				FAssetDurableTarget(
					MakeIdentity(false),
					TEXT("/Game/UAI/P4_009/SaveAsset"),
					EPackageHeaderKind::EvidenceRequired,
					EAssetDurableIntent::Save,
					ERollbackGrade::DurableBackup),
				FAssetDurableTarget(
					MakeIdentity(true),
					TEXT("/Game/UAI/P4_009/DeleteAsset"),
					EPackageHeaderKind::EvidenceRequired,
					EAssetDurableIntent::Delete,
					ERollbackGrade::DurableBackup)
			},
			41,
			TEXT("owned-worker:p4-009"),
			Atomicity);
	}

	static FAssetDurableRequest MakeNativeRequest(
		const FNativePackageFixture& Fixture,
		const bool bDelete,
		FUeAgentResourceTracker& Tracker)
	{
		return FAssetDurableRequest(
			MakePlan(Tracker),
			{
				FAssetDurableTarget(
					MakeIdentity(bDelete),
					Fixture.PackageName,
					EPackageHeaderKind::Asset,
					bDelete
						? EAssetDurableIntent::Delete
						: EAssetDurableIntent::Save,
					ERollbackGrade::DurableBackup)
			},
			41,
			TEXT("owned-worker:p4-010-native"),
			EAtomicityMode::CompensatedSaga);
	}
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAssetDurableModuleAvailableTest,
	"UeAgentInterface.Core.AssetDurable.ModuleAvailable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentAssetDurableModuleAvailableTest::RunTest(
	const FString& Parameters)
{
	TestTrue(TEXT("C8D AssetDurable module is available"), true);
	FPackageAssetDurableBackend PackageBackend;
	TestEqual(
		TEXT("native package backend owns one Content root"),
		PackageBackend.GetAllowedArtifactRoots().Num(),
		1);
	TestTrue(
		TEXT("native package backend declares compensated durability"),
		PackageBackend.SupportsAtomicity(
			EAtomicityMode::CompensatedSaga));
	TestFalse(
		TEXT("native backend does not overclaim multi-owner atomicity"),
		PackageBackend.SupportsAtomicity(
			EAtomicityMode::MultiOwnerAtomicSupported));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAssetDurableAbsentUnloadedMapInitialInventoryTest,
	"UeAgentInterface.Core.AssetDurable.AbsentUnloadedMapInitialInventory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentAssetDurableAbsentUnloadedMapInitialInventoryTest::RunTest(
	const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName =
		TEXT("/Game/UAI/P4_010/AbsentMap_") + Suffix;
	FString AssetPath;
	FString MapPath;
	TestTrue(TEXT("resolve absent asset header path"),
		FPackageName::TryConvertLongPackageNameToFilename(
			PackageName,
			AssetPath,
			FPackageName::GetAssetPackageExtension()));
	TestTrue(TEXT("resolve absent map header path"),
		FPackageName::TryConvertLongPackageNameToFilename(
			PackageName,
			MapPath,
			FPackageName::GetMapPackageExtension()));
	AssetPath = FPaths::ConvertRelativePathToFull(AssetPath);
	MapPath = FPaths::ConvertRelativePathToFull(MapPath);
	IFileManager::Get().Delete(*AssetPath, false, true, true);
	IFileManager::Get().Delete(*MapPath, false, true, true);
	TestFalse(TEXT("fixture asset header is absent"),
		IFileManager::Get().FileExists(*AssetPath));
	TestFalse(TEXT("fixture map header is absent"),
		IFileManager::Get().FileExists(*MapPath));
	TestNull(TEXT("fixture package is not loaded"),
		FindPackage(nullptr, *PackageName));

	FUeAgentResourceTracker Tracker = MakeTracker();
	const FAssetDurableRequest Request(
		MakePlan(Tracker),
		{FAssetDurableTarget(
			MakeIdentity(false),
			PackageName,
			EPackageHeaderKind::Map,
			EAssetDurableIntent::Save,
			ERollbackGrade::DurableBackup)},
		41,
		TEXT("owned-worker:p4-010-absent-map"),
		EAtomicityMode::CompensatedSaga);
	const FString JournalRoot = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectIntermediateDir()
			/ TEXT("UAI/P4_010/absent-map")
			/ Suffix);
	FPackageAssetDurableBackend Backend;
	FAssetDurableSessionPtr Session;
	FAssetDurableError Error;
	if (!TestTrue(TEXT("formal prepare builds absent map inventory"),
		TryPrepareAssetDurability(
			Request,
			Backend,
			JournalRoot,
			Tracker,
			Session,
			Error)))
	{
		AddError(Error.GetCode() + TEXT(":") + Error.GetDetail());
		IFileManager::Get().DeleteDirectory(*JournalRoot, false, true);
		return false;
	}
	if (!TestTrue(TEXT("prepared map session exists"), Session.IsValid()))
	{
		IFileManager::Get().DeleteDirectory(*JournalRoot, false, true);
		return false;
	}
	bool bSawMapHeader = false;
	bool bSawAssetHeader = false;
	for (const FDurableArtifact& Artifact :
		Session->GetInventory().GetArtifacts())
	{
		if (Artifact.GetRole() != TEXT("package_header"))
		{
			continue;
		}
		bSawMapHeader |= Artifact.GetFinalPath().Equals(
			MapPath, ESearchCase::IgnoreCase);
		bSawAssetHeader |= Artifact.GetFinalPath().Equals(
			AssetPath, ESearchCase::IgnoreCase);
	}
	TestTrue(TEXT("initial inventory registers the explicit map header"),
		bSawMapHeader);
	TestFalse(TEXT("initial inventory never invents an asset header"),
		bSawAssetHeader);
	TestFalse(TEXT("prepare does not create the map header"),
		IFileManager::Get().FileExists(*MapPath));
	TestFalse(TEXT("prepare does not create an asset header"),
		IFileManager::Get().FileExists(*AssetPath));
	IFileManager::Get().DeleteDirectory(*JournalRoot, false, true);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAssetDurableStableRevisionTest,
	"UeAgentInterface.Core.AssetDurable.StableAssetRevision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentAssetDurableStableRevisionTest::RunTest(
	const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/P4_Revision/Fixture_") + Suffix;
	FString HeaderPath;
	TestTrue(TEXT("resolve durable revision fixture path"),
		FPackageName::TryConvertLongPackageNameToFilename(
			PackageName,
			HeaderPath,
			FPackageName::GetAssetPackageExtension()));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(HeaderPath), true);
	TestTrue(TEXT("write durable revision fixture"),
		FFileHelper::SaveStringToFile(TEXT("stable-package-bytes"), *HeaderPath));
	UPackage* const Package = CreatePackage(*PackageName);
	UTexture2D* const Asset = NewObject<UTexture2D>(
		Package,
		*FPackageName::GetLongPackageAssetName(PackageName),
		RF_Public | RF_Standalone);
	FString First;
	FString Second;
	UE::UAI::Contract::FDiagnostic RevisionError;
	if (!TryReadStableAssetRevision(Asset, First, RevisionError)) AddError(RevisionError.PublicMessage);
	if (!TryReadStableAssetRevision(Asset, Second, RevisionError)) AddError(RevisionError.PublicMessage);
	TestTrue(TEXT("file-backed object uses durable package revision"),
		First.StartsWith(TEXT("package-md5:")));
	TestEqual(TEXT("repeated file-backed reads are stable"), First, Second);

	UTexture2D* const Transient =
		NewObject<UTexture2D>(GetTransientPackage());
	FString TransientRevision;
	if (!TryReadStableAssetRevision(Transient, TransientRevision, RevisionError)) AddError(RevisionError.PublicMessage);
	TestTrue(TEXT("transient object retains CRC fallback"),
		TransientRevision.StartsWith(TEXT("asset-crc32:")));

	IFileManager::Get().Delete(*HeaderPath, false, true, true);
	Asset->MarkAsGarbage();
	Package->MarkAsGarbage();
	Transient->MarkAsGarbage();
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAssetDurablePackageHeaderExtensionTest,
	"UeAgentInterface.Core.AssetDurable.PackageHeaderExtension",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentAssetDurablePackageHeaderExtensionTest::RunTest(
	const FString& Parameters)
{
	const FString NoEvidencePackage = TEXT("/Game/UAI/P4_010/NoEvidence_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	FString ResolvedPath;
	FString ErrorCode;
	TestFalse(TEXT("header kind evidence is required when no package evidence exists"),
		TryResolvePackageHeaderFilename(
			NoEvidencePackage,
			EPackageHeaderKind::EvidenceRequired,
			ResolvedPath,
			ErrorCode));
	TestEqual(TEXT("missing header kind evidence is typed"), ErrorCode,
		FString(TEXT("durable_package_header_kind_evidence_required")));
	TestTrue(TEXT("explicit asset kind selects uasset without evidence"),
		TryResolvePackageHeaderFilename(
			NoEvidencePackage, EPackageHeaderKind::Asset,
			ResolvedPath, ErrorCode));
	TestTrue(TEXT("explicit asset extension"),
		ResolvedPath.EndsWith(FPackageName::GetAssetPackageExtension()));
	TestTrue(TEXT("explicit map kind selects umap without evidence"),
		TryResolvePackageHeaderFilename(
			NoEvidencePackage, EPackageHeaderKind::Map,
			ResolvedPath, ErrorCode));
	TestTrue(TEXT("explicit map extension"),
		ResolvedPath.EndsWith(FPackageName::GetMapPackageExtension()));

	const FString PackageName = TEXT("/Game/UAI/P4_010/MapHeader_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	FString MapPath;
	FString AssetPath;
	TestTrue(
		TEXT("resolve map test path"),
		FPackageName::TryConvertLongPackageNameToFilename(
			PackageName,
			MapPath,
			FPackageName::GetMapPackageExtension()));
	TestTrue(
		TEXT("resolve asset test path"),
		FPackageName::TryConvertLongPackageNameToFilename(
			PackageName,
			AssetPath,
			FPackageName::GetAssetPackageExtension()));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(MapPath), true);
	IFileManager::Get().Delete(*MapPath, false, true, true);
	IFileManager::Get().Delete(*AssetPath, false, true, true);
	TestTrue(
		TEXT("create exact map-header witness"),
		FFileHelper::SaveStringToFile(TEXT("map"), *MapPath));

	ResolvedPath.Reset();
	ErrorCode.Reset();
	TestTrue(
		TEXT("an unloaded package reuses its existing map extension"),
		TryResolvePackageHeaderFilename(
			PackageName,
			EPackageHeaderKind::EvidenceRequired,
			ResolvedPath,
			ErrorCode));
	TestEqual(
		TEXT("map header is selected exactly"),
		ResolvedPath,
		MapPath);

	UPackage* const LoadedPackage = CreatePackage(*PackageName);
	UWorld* const LoadedWorld = NewObject<UWorld>(
		LoadedPackage,
		*FPackageName::GetLongPackageAssetName(PackageName),
		RF_Public | RF_Standalone);
	LoadedPackage->ClearPackageFlags(PKG_ContainsMap);
	ResolvedPath.Reset();
	ErrorCode.Reset();
	UObject* PreparedTopLevelAsset = LoadedWorld;
	TestTrue(
		TEXT("save preflight recognizes a loaded UWorld as a map"),
		TryPrepareLoadedPackageHeaderForSave(
			*LoadedPackage,
			PreparedTopLevelAsset,
			ResolvedPath,
			ErrorCode));
	TestTrue(
		TEXT("save preflight repairs missing map package metadata"),
		LoadedPackage->ContainsMap());
	TestEqual(
		TEXT("save preflight preserves the map header"),
		ResolvedPath,
		MapPath);
	ResolvedPath.Reset();
	ErrorCode.Reset();
	TestFalse(TEXT("explicit asset kind conflicts with loaded world evidence"),
		TryResolvePackageHeaderFilename(
			PackageName,
			EPackageHeaderKind::Asset,
			ResolvedPath,
			ErrorCode));
	TestEqual(TEXT("loaded opposite kind conflict is typed"), ErrorCode,
		FString(TEXT("durable_package_extension_kind_mismatch")));

	TestTrue(
		TEXT("create conflicting asset-header witness"),
		FFileHelper::SaveStringToFile(TEXT("asset"), *AssetPath));
	ResolvedPath.Reset();
	ErrorCode.Reset();
	TestFalse(
		TEXT("two extensions for one package fail closed"),
		TryResolvePackageHeaderFilename(
			PackageName,
			EPackageHeaderKind::EvidenceRequired,
			ResolvedPath,
			ErrorCode));
	TestEqual(
		TEXT("extension ambiguity is explicit"),
		ErrorCode,
		FString(TEXT("durable_package_extension_ambiguous")));

	IFileManager::Get().Delete(*MapPath, false, true, true);
	IFileManager::Get().Delete(*AssetPath, false, true, true);
	LoadedWorld->MarkAsGarbage();
	LoadedPackage->MarkAsGarbage();
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAssetDurablePrepareJournalTest,
	"UeAgentInterface.Core.AssetDurable.PrepareJournal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentAssetDurablePrepareJournalTest::RunTest(
	const FString& Parameters)
{
	FOwnedDiskFixture Fixture(TEXT("prepare"));
	FRecordingAssetBackend Backend(Fixture);
	FUeAgentResourceTracker Tracker = MakeTracker();
	int64 FirstFence = -1;
	int64 SecondFence = -1;
	FAssetDurableError Error;
	TestTrue(
		TEXT("stable journal owner acquires first durable fence"),
		TryAcquireAssetDurableFence(
			Fixture.JournalRoot,
			TEXT("project:fixture:asset-durable"),
			Tracker,
			FirstFence,
			Error));
	TestTrue(
		TEXT("stable journal owner advances durable fence"),
		TryAcquireAssetDurableFence(
			Fixture.JournalRoot,
			TEXT("project:fixture:asset-durable"),
			Tracker,
			SecondFence,
			Error));
	TestEqual(TEXT("first durable fence starts at zero"), FirstFence, int64(0));
	TestEqual(TEXT("durable fence is monotonic"), SecondFence, int64(1));
	int64 ForeignFence = -1;
	TestFalse(
		TEXT("same fence store rejects a foreign stable owner"),
		TryAcquireAssetDurableFence(
			Fixture.JournalRoot,
			TEXT("project:other:asset-durable"),
			Tracker,
			ForeignFence,
			Error));
	TestEqual(
		TEXT("foreign fence owner failure is typed"),
		Error.GetCode(),
		FString(TEXT("durable_fence_store_integrity_invalid")));
	FAssetDurableSessionPtr Session;
	Error = FAssetDurableError();
	TestTrue(
		TEXT("typed C6 plan prepares C8D"),
		TryPrepareAssetDurability(
			MakeRequest(Tracker),
			Backend,
			Fixture.JournalRoot,
			Tracker,
			Session,
			Error));
	if (!Session.IsValid())
	{
		AddError(TEXT("asset durable session missing"));
		return false;
	}
	TestEqual(
		TEXT("four header/payload artifacts"),
		Session->GetInventory().GetArtifacts().Num(),
		4);
	TestEqual(
		TEXT("prepared phase history"),
		Session->GetPhaseHistory(),
		TArray<FString>({
			TEXT("inventory_ready"),
			TEXT("backups_ready"),
			TEXT("prepared")
		}));
	TestTrue(
		TEXT("write-ahead journal exists"),
		IFileManager::Get().FileExists(*Session->GetJournalPath()));
	FString JournalText;
	TestTrue(
		TEXT("journal independently reads"),
		FFileHelper::LoadFileToString(
			JournalText,
			*Session->GetJournalPath()));
	TestTrue(
		TEXT("journal binds C6 fingerprint"),
		JournalText.Contains(
			Session->GetRequest().GetPlan()->GetFingerprint()));
	TestTrue(
		TEXT("journal uses state-bound v3 schema"),
		JournalText.Contains(
			TEXT("uai.c8d.asset_durable.journal.v3")));
	TestEqual(
		TEXT("prepared session binds one pre-state per artifact"),
		Session->GetPreMutationStates().Num(),
		Session->GetInventory().GetArtifacts().Num());
	TestTrue(
		TEXT("prepared pre-states are known"),
		Session->GetPreMutationStates().ContainsByPredicate(
			[](const FDurableArtifactState& State)
			{
				return !State.IsKnown();
			}) == false);
	TestEqual(
		TEXT("persisted evidence is not guessed before publish"),
		Session->GetExpectedPersistedStates().Num(),
		0);
	TestEqual(
		TEXT("prepare performs no durable mutation"),
		Backend.MutationEvents.Num(),
		0);
	{
		FOwnedDiskFixture FailedFixture(TEXT("prepare-failure"));
		FRecordingAssetBackend FailedBackend(FailedFixture);
		FailedBackend.bFailBackup = true;
		FAssetDurableSessionPtr FailedSession;
		TestFalse(TEXT("backup failure stops preparation"), TryPrepareAssetDurability(
			MakeRequest(Tracker), FailedBackend, FailedFixture.JournalRoot, Tracker, FailedSession, Error));
		TestFalse(TEXT("failed preparation transfers no session"), FailedSession.IsValid());
		TestEqual(TEXT("original preparation error preserved"), Error.GetCode(), FString(TEXT("fixture_backup_failed")));
		TArray<FString> ActiveTransactions;
		IFileManager::Get().FindFiles(ActiveTransactions, *(FailedFixture.JournalRoot / TEXT("active/*")), false, true);
		TestEqual(TEXT("failed preparation leaves no active transaction"), ActiveTransactions.Num(), 0);
		FString Original;
		FFileHelper::LoadFileToString(Original, *FailedFixture.SaveHeader);
		TestEqual(TEXT("failed preparation preserves the original"), Original, FString(TEXT("old-save-header")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAssetDurableDynamicEnrollmentTest,
	"UeAgentInterface.Core.AssetDurable.DynamicEnrollment",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentAssetDurableDynamicEnrollmentTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FOwnedDiskFixture Fixture(TEXT("dynamic_enrollment"));
	FRecordingAssetBackend Backend(Fixture);
	FUeAgentResourceTracker Tracker = MakeTracker();
	FAssetDurableSessionPtr Session;
	FAssetDurableError Error;
	if (!TestTrue(
		TEXT("the frozen request prepares its original inventory"),
		TryPrepareAssetDurability(
			MakeRequest(Tracker),
			Backend,
			Fixture.JournalRoot,
			Tracker,
			Session,
			Error)))
	{
		AddError(Error.GetCode() + TEXT(":") + Error.GetDetail());
		return false;
	}

	const FAssetDurableTarget ExternalTarget(
		MakeIdentity(false),
		TEXT("/Game/UAI/P4_009/ExternalActor"),
		EPackageHeaderKind::EvidenceRequired,
		EAssetDurableIntent::Save,
		ERollbackGrade::DurableBackup);
	TestEqual(TEXT("package target stores explicit header kind"),
		ExternalTarget.GetPackageHeaderKind(),
		EPackageHeaderKind::EvidenceRequired);
	FAssetDurableSessionPtr Rejected;
	TestFalse(
		TEXT("dynamic enrollment rechecks the fencing token"),
		TryEnlistAssetDurableTargets(
			Session.ToSharedRef(),
			{ExternalTarget},
			Backend,
			40,
			Tracker,
			Rejected,
			Error));
	TestEqual(
		TEXT("stale dynamic enrollment has a typed failure"),
		Error.GetCode(),
		FString(TEXT("durable_fence_stale")));

	const FUeAgentCanonicalIdentity UnknownIdentity(
		TEXT("uai.reference.asset.title"),
		TEXT("reference_asset"),
		TEXT("/Game/UAI/P4_009/Unknown"),
		TEXT("title"),
		TEXT("asset_property"),
		TEXT("editor-session-21"));
	TestFalse(
		TEXT("a mutation package cannot escape the frozen plan"),
		TryEnlistAssetDurableTargets(
			Session.ToSharedRef(),
			{
				FAssetDurableTarget(
					UnknownIdentity,
					TEXT("/Game/UAI/P4_009/Unknown"),
					EPackageHeaderKind::EvidenceRequired,
					EAssetDurableIntent::Save,
					ERollbackGrade::DurableBackup),
			},
			Backend,
			41,
			Tracker,
			Rejected,
			Error));
	TestEqual(
		TEXT("out-of-plan enrollment has a typed failure"),
		Error.GetCode(),
		FString(TEXT("durable_target_not_in_plan")));

	TestFalse(
		TEXT("one physical package cannot carry conflicting intents"),
		TryEnlistAssetDurableTargets(
			Session.ToSharedRef(),
			{
				FAssetDurableTarget(
					MakeIdentity(true),
					TEXT("/Game/UAI/P4_009/SaveAsset"),
					EPackageHeaderKind::EvidenceRequired,
					EAssetDurableIntent::Delete,
					ERollbackGrade::DurableBackup),
			},
			Backend,
			41,
			Tracker,
			Rejected,
			Error));
	TestEqual(
		TEXT("physical intent conflicts are typed"),
		Error.GetCode(),
		FString(TEXT("durable_enlist_target_conflict")));
	TestFalse(
		TEXT("one physical package cannot change header kind while enlisted"),
		TryEnlistAssetDurableTargets(
			Session.ToSharedRef(),
			{FAssetDurableTarget(
				MakeIdentity(false),
				TEXT("/Game/UAI/P4_009/SaveAsset"),
				EPackageHeaderKind::Map,
				EAssetDurableIntent::Save,
				ERollbackGrade::DurableBackup)},
			Backend,
			41,
			Tracker,
			Rejected,
			Error));
	TestEqual(TEXT("physical header kind conflicts are typed"),
		Error.GetCode(), FString(TEXT("durable_enlist_target_conflict")));

	FAssetDurableSessionPtr Extended;
	if (!TestTrue(
		TEXT("a new package for an existing planned change is enlisted"),
		TryEnlistAssetDurableTargets(
			Session.ToSharedRef(),
			{ExternalTarget},
			Backend,
			41,
			Tracker,
			Extended,
			Error)))
	{
		AddError(Error.GetCode() + TEXT(":") + Error.GetDetail());
		return false;
	}
	if (!Extended.IsValid())
	{
		AddError(TEXT("the extended durable session is missing"));
		return false;
	}
	TestEqual(
		TEXT("dynamic enrollment preserves the journal identity"),
		Extended->GetJournalId(),
		Session->GetJournalId());
	TestEqual(
		TEXT("dynamic enrollment preserves the transaction root"),
		Extended->GetTransactionRoot(),
		Session->GetTransactionRoot());
	TestEqual(
		TEXT("the extended request contains three physical packages"),
		Extended->GetRequest().GetTargets().Num(),
		3);
	TestEqual(
		TEXT("the extended inventory contains every header and payload"),
		Extended->GetInventory().GetArtifacts().Num(),
		6);
	TestEqual(
		TEXT("the journal records the inventory extension phase"),
		Extended->GetCurrentPhase(),
		FString(TEXT("inventory_extended")));
	const TOptional<FString>* ExternalHeaderBackup =
		Backend.Backups.Find(Fixture.ExternalHeader);
	TestNotNull(
		TEXT("the new external actor header receives a backup marker"),
		ExternalHeaderBackup);
	if (ExternalHeaderBackup)
	{
		TestFalse(
			TEXT("a newly created package is backed up as absent"),
			ExternalHeaderBackup->IsSet());
	}
	TestFalse(
		TEXT("an unrelated file is not absorbed into the transaction"),
		Backend.Backups.Contains(Fixture.UnrelatedHeader));

	Backend.bFailPublishAfterMutation = true;
	FDurableReceipt Receipt;
	TestTrue(
		TEXT("a post-mutation publish failure is represented by rollback"),
		TryPersistAssetDurability(
			Extended.ToSharedRef(),
			Backend,
			41,
			Tracker,
			Receipt,
			Error));
	TestEqual(
		TEXT("the extended transaction rolls back as one unit"),
		Receipt.GetStatus(),
		EDurableStatus::RolledBack);
	TestFalse(
		TEXT("rollback removes the newly published external actor header"),
		IFileManager::Get().FileExists(*Fixture.ExternalHeader));
	FString RestoredSave;
	FString RestoredDelete;
	FString Unrelated;
	TestTrue(
		TEXT("the original save header remains readable"),
		FFileHelper::LoadFileToString(RestoredSave, *Fixture.SaveHeader));
	TestTrue(
		TEXT("the original delete header remains readable"),
		FFileHelper::LoadFileToString(
			RestoredDelete,
			*Fixture.DeleteHeader));
	TestTrue(
		TEXT("the unrelated file remains readable"),
		FFileHelper::LoadFileToString(Unrelated, *Fixture.UnrelatedHeader));
	TestEqual(
		TEXT("rollback restores the original save header"),
		RestoredSave,
		FString(TEXT("old-save-header")));
	TestEqual(
		TEXT("rollback restores the original delete header"),
		RestoredDelete,
		FString(TEXT("old-delete-header")));
	TestEqual(
		TEXT("unrelated state is unchanged"),
		Unrelated,
		FString(TEXT("unrelated")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAssetDurableSaveDeleteTest,
	"UeAgentInterface.Core.AssetDurable.SaveDeletePersistedUnverified",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentAssetDurableSaveDeleteTest::RunTest(
	const FString& Parameters)
{
	FOwnedDiskFixture Fixture(TEXT("save_delete"));
	FRecordingAssetBackend Backend(Fixture);
	FUeAgentResourceTracker Tracker = MakeTracker();
	FAssetDurableSessionPtr Session;
	FAssetDurableError Error;
	if (!TestTrue(
		TEXT("prepare succeeds"),
		TryPrepareAssetDurability(
			MakeRequest(Tracker),
			Backend,
			Fixture.JournalRoot,
			Tracker,
			Session,
			Error)))
	{
		return false;
	}
	FDurableReceipt Receipt;
	TestTrue(
		TEXT("save/delete persistence returns typed durable receipt"),
		TryPersistAssetDurability(
			Session.ToSharedRef(),
			Backend,
			41,
			Tracker,
			Receipt,
			Error));
	const FDurableVerificationAcknowledgement Acknowledgement(
		Receipt.GetJournalId(),
		Receipt.GetPlanFingerprint(),
		Receipt.GetInventoryHash(),
		TEXT("verification:p4-013:save-delete"),
		Session->GetRequest().GetJournalOwnerId(),
		41);
	const FAssetDurableSessionRef PersistedSession =
		MakeShared<const FAssetDurableSession>(
			Session->GetRequest(),
			Session->GetInventory(),
			Session->GetJournalId(),
			Session->GetJournalPath(),
			Session->GetJournalRoot(),
			Session->GetTransactionRoot(),
			Session->GetStagingRoot(),
			Receipt.GetPhaseHistory());
	TestTrue(
		TEXT("C8D owns verification acknowledgement and journal finalization"),
		TryAcknowledgeAssetDurability(
			PersistedSession,
			Acknowledgement,
			41,
			Tracker,
			Error));
	TestFalse(
		TEXT("acknowledged journal leaves active root"),
		IFileManager::Get().FileExists(*Session->GetJournalPath()));
	TestTrue(
		TEXT("acknowledged journal is archived by C8D"),
		IFileManager::Get().FileExists(*(
			Session->GetJournalRoot()
			/ TEXT("completed")
			/ (Session->GetJournalId() + TEXT(".json")))));
	TestEqual(
		TEXT("C8D cannot claim complete"),
		Receipt.GetStatus(),
		EDurableStatus::PersistedUnverified);
	TestTrue(
		TEXT("C9 verification remains required"),
		Receipt.RequiresIndependentVerification());
	TestTrue(
		TEXT("save artifact is published"),
		IFileManager::Get().FileExists(*Fixture.SaveHeader));
	TestFalse(
		TEXT("delete artifact is absent"),
		IFileManager::Get().FileExists(*Fixture.DeleteHeader));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAssetDurableRollbackRecoveryTest,
	"UeAgentInterface.Core.AssetDurable.RollbackRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentAssetDurableRollbackRecoveryTest::RunTest(
	const FString& Parameters)
{
	FOwnedDiskFixture Fixture(TEXT("rollback"));
	FRecordingAssetBackend Backend(Fixture);
	Backend.bFailPublish = true;
	FUeAgentResourceTracker Tracker = MakeTracker();
	FAssetDurableSessionPtr Session;
	FAssetDurableError Error;
	if (!TestTrue(
		TEXT("prepare succeeds"),
		TryPrepareAssetDurability(
			MakeRequest(Tracker),
			Backend,
			Fixture.JournalRoot,
			Tracker,
			Session,
			Error)))
	{
		return false;
	}
	FDurableReceipt Receipt;
	TestTrue(
		TEXT("publish failure is represented, not hidden"),
		TryPersistAssetDurability(
			Session.ToSharedRef(),
			Backend,
			41,
			Tracker,
			Receipt,
			Error));
	TestEqual(
		TEXT("confirmed restore is rolled_back"),
		Receipt.GetStatus(),
		EDurableStatus::RolledBack);
	TestEqual(TEXT("rollback retains the original publish failure"),
		Receipt.GetFailureCode(), FString(TEXT("fixture_publish_failed")));
	const int32 MutationCountAfterRecovery = Backend.MutationEvents.Num();
	bool bMemoryConfirmed = false;
	FDurableReceipt ConfirmedReceipt;
	TestTrue(TEXT("terminal rollback can confirm native memory without restoring disk again"),
		TryRollbackAssetDurability(Session.ToSharedRef(), Backend, 41,
			[&bMemoryConfirmed]() { bMemoryConfirmed = true; return true; },
			Tracker, ConfirmedReceipt, Error));
	TestTrue(TEXT("native restoration callback runs"), bMemoryConfirmed);
	TestEqual(TEXT("completed disk recovery is not replayed"),
		Backend.MutationEvents.Num(), MutationCountAfterRecovery);

	FOwnedDiskFixture ConfirmFixture(TEXT("persist_for_verification"));
	FRecordingAssetBackend ConfirmBackend(ConfirmFixture);
	ConfirmBackend.bFailPublish = true;
	FAssetDurableSessionPtr ConfirmSession;
	if (!TestTrue(TEXT("confirmation fixture prepares"),
		TryPrepareAssetDurability(MakeRequest(Tracker), ConfirmBackend,
			ConfirmFixture.JournalRoot, Tracker, ConfirmSession, Error))) return false;
	TestFalse(TEXT("rolled back persistence never permits verification or acknowledgement"),
		TryPersistAssetDurabilityForVerification(ConfirmSession.ToSharedRef(),
			ConfirmBackend, 41, Tracker, ConfirmedReceipt, Error));
	TestEqual(TEXT("mutation caller receives the original failure"),
		Error.GetCode(), FString(TEXT("fixture_publish_failed")));

	FOwnedDiskFixture UnknownFixture(TEXT("unknown"));
	FRecordingAssetBackend UnknownBackend(UnknownFixture);
	UnknownBackend.bFailPublish = true;
	UnknownBackend.bRestoreConfirmed = false;
	FAssetDurableSessionPtr UnknownSession;
	TestTrue(
		TEXT("unknown prepare succeeds"),
		TryPrepareAssetDurability(
			MakeRequest(Tracker),
			UnknownBackend,
			UnknownFixture.JournalRoot,
			Tracker,
			UnknownSession,
			Error));
	FDurableReceipt UnknownReceipt;
	TestTrue(
		TEXT("unconfirmed restore returns typed receipt"),
		TryPersistAssetDurability(
			UnknownSession.ToSharedRef(),
			UnknownBackend,
			41,
			Tracker,
			UnknownReceipt,
			Error));
	TestEqual(
		TEXT("unconfirmed restore is state_unknown"),
		UnknownReceipt.GetStatus(),
		EDurableStatus::StateUnknown);

	FOwnedDiskFixture MemoryFixture(TEXT("memory_rollback"));
	FRecordingAssetBackend MemoryBackend(MemoryFixture);
	FAssetDurableSessionPtr MemorySession;
	TestTrue(
		TEXT("memory rollback prepare succeeds"),
		TryPrepareAssetDurability(
			MakeRequest(Tracker),
			MemoryBackend,
			MemoryFixture.JournalRoot,
			Tracker,
			MemorySession,
			Error));
	FDurableReceipt MemoryReceipt;
	TestTrue(
		TEXT("unconfirmed memory rollback returns typed receipt"),
		TryRollbackAssetDurability(
			MemorySession.ToSharedRef(),
			MemoryBackend,
			41,
			false,
			Tracker,
			MemoryReceipt,
			Error));
	TestEqual(
		TEXT("memory rollback must be independently confirmed"),
		MemoryReceipt.GetStatus(),
		EDurableStatus::StateUnknown);

	FOwnedDiskFixture PreparedFixture(TEXT("recover_prepared"));
	FRecordingAssetBackend PreparedBackend(PreparedFixture);
	FAssetDurableSessionPtr PreparedSession;
	TestTrue(
		TEXT("prepared recovery fixture succeeds"),
		TryPrepareAssetDurability(
			MakeRequest(Tracker),
			PreparedBackend,
			PreparedFixture.JournalRoot,
			Tracker,
			PreparedSession,
			Error));
	TArray<FDurableReceipt> PreparedReceipts;
	TestTrue(
		TEXT("prepared journal recovers without mutation replay"),
		TryRecoverAssetDurability(
			PreparedFixture.JournalRoot,
			PreparedBackend,
			41,
			TEXT("owned-worker:p4-009"),
			Tracker,
			PreparedReceipts,
			Error));
	TestEqual(
		TEXT("one prepared journal recovered"),
		PreparedReceipts.Num(),
		1);
	if (PreparedReceipts.Num() == 1)
	{
		TestEqual(
			TEXT("prepared recovery is rolled back"),
			PreparedReceipts[0].GetStatus(),
			EDurableStatus::RolledBack);
		TestEqual(
			TEXT("prepared recovery aborts before mutation"),
			PreparedReceipts[0].GetRecoveryAction(),
			ERecoveryAction::AbortedBeforeMutation);
	}
	TestFalse(
		TEXT("prepared transaction root is removed"),
		IFileManager::Get().DirectoryExists(
			*PreparedSession->GetTransactionRoot()));

	FOwnedDiskFixture PersistedFixture(TEXT("recover_persisted"));
	FRecordingAssetBackend PersistedBackend(PersistedFixture);
	PersistedBackend.ObservedState = TEXT("persisted");
	FAssetDurableSessionPtr PersistedSession;
	TestTrue(
		TEXT("persisted recovery prepare succeeds"),
		TryPrepareAssetDurability(
			MakeRequest(Tracker),
			PersistedBackend,
			PersistedFixture.JournalRoot,
			Tracker,
			PersistedSession,
			Error));
	FDurableReceipt PersistedReceipt;
	TestTrue(
		TEXT("persisted recovery publish succeeds"),
		TryPersistAssetDurability(
			PersistedSession.ToSharedRef(),
			PersistedBackend,
			41,
			Tracker,
			PersistedReceipt,
			Error));
	TArray<FDurableReceipt> PersistedReceipts;
	TestTrue(
		TEXT("persisted recovery requests independent readback"),
		TryRecoverAssetDurability(
			PersistedFixture.JournalRoot,
			PersistedBackend,
			41,
			TEXT("owned-worker:p4-009"),
			Tracker,
			PersistedReceipts,
			Error));
	TestEqual(
		TEXT("one persisted journal recovered"),
		PersistedReceipts.Num(),
		1);
	if (PersistedReceipts.Num() == 1)
	{
		TestEqual(
			TEXT("persisted recovery cannot claim complete"),
			PersistedReceipts[0].GetStatus(),
			EDurableStatus::RecoveryRequired);
		TestEqual(
			TEXT("C9 readback is required"),
			PersistedReceipts[0].GetRecoveryAction(),
			ERecoveryAction::VerifyPersisted);
		TestTrue(
			TEXT("recovery receipt requires independent verification"),
			PersistedReceipts[0].RequiresIndependentVerification());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAssetDurableReconciliationTest,
	"UeAgentInterface.Core.AssetDurable.StateBoundReconciliation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentAssetDurableReconciliationTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FUeAgentResourceTracker Tracker = MakeTracker();
	FAssetDurableError Error;
	FAcceptingReconciliationOracle ReconciliationOracle;

	FOwnedDiskFixture PersistedFixture(TEXT("reconcile_persisted"));
	FRecordingAssetBackend PersistedBackend(PersistedFixture);
	FAssetDurableSessionPtr PersistedSession;
	if (!TestTrue(
			TEXT("persisted fixture prepares"),
			TryPrepareAssetDurability(
				MakeRequest(Tracker),
				PersistedBackend,
				PersistedFixture.JournalRoot,
				Tracker,
				PersistedSession,
				Error)))
	{
		return false;
	}
	FDurableReceipt PersistReceipt;
	if (!TestTrue(
			TEXT("persisted fixture publishes"),
			TryPersistAssetDurability(
				PersistedSession.ToSharedRef(),
				PersistedBackend,
				41,
				Tracker,
				PersistReceipt,
				Error)))
	{
		return false;
	}
	const FString CompletedRoot =
		PersistedFixture.JournalRoot / TEXT("completed");
	IFileManager::Get().MakeDirectory(*CompletedRoot, true);
	const FString ExistingCompleted = CompletedRoot
		/ (PersistedSession->GetJournalId() + TEXT(".json"));
	TestTrue(
		TEXT("pre-existing terminal receipt fixture writes"),
		FFileHelper::SaveStringToFile(
			TEXT("prior-terminal-receipt"),
			*ExistingCompleted));
	const int32 PersistMutationCount =
		PersistedBackend.MutationEvents.Num();
	FDurableReceipt VerifiedReceipt;
	TestTrue(
		TEXT("bound persisted state reconciles without replay"),
		TryReconcileAssetDurability(
			PersistedFixture.JournalRoot,
			PersistedSession->GetJournalId(),
			PersistedBackend.GetAllowedArtifactRoots(),
			TEXT("owned-worker:p4-009"),
			41,
			TEXT("verification:persisted"),
			ReconciliationOracle,
			Tracker,
			VerifiedReceipt,
			Error));
	TestEqual(
		TEXT("persisted state becomes verified"),
		VerifiedReceipt.GetStatus(),
		EDurableStatus::Verified);
	TestEqual(
		TEXT("verification receipt is bound"),
		VerifiedReceipt.GetVerificationId(),
		FString(TEXT("verification:persisted")));
	TestEqual(
		TEXT("reconciliation never replays mutation"),
		PersistedBackend.MutationEvents.Num(),
		PersistMutationCount);
	TestFalse(
		TEXT("verified journal leaves active root"),
		IFileManager::Get().DirectoryExists(
			*PersistedSession->GetTransactionRoot()));
	FString ExistingCompletedText;
	TestTrue(
		TEXT("pre-existing same-id receipt remains readable"),
		FFileHelper::LoadFileToString(
			ExistingCompletedText,
			*ExistingCompleted));
	TestEqual(
		TEXT("same-id receipt is never overwritten"),
		ExistingCompletedText,
		FString(TEXT("prior-terminal-receipt")));
	TArray<FString> CollisionArchives;
	IFileManager::Get().FindFiles(
		CollisionArchives,
		*(CompletedRoot
			/ (PersistedSession->GetJournalId()
				+ TEXT(".*.json"))),
		true,
		false);
	TestEqual(
		TEXT("new terminal evidence receives a collision-safe name"),
		CollisionArchives.Num(),
		1);

	FOwnedDiskFixture OriginalFixture(TEXT("reconcile_original"));
	FRecordingAssetBackend OriginalBackend(OriginalFixture);
	OriginalBackend.bFailPublishAfterMutation = true;
	OriginalBackend.bRestoreConfirmed = false;
	FAssetDurableSessionPtr OriginalSession;
	TestTrue(
		TEXT("original fixture prepares"),
		TryPrepareAssetDurability(
			MakeRequest(Tracker),
			OriginalBackend,
			OriginalFixture.JournalRoot,
			Tracker,
			OriginalSession,
			Error));
	FDurableReceipt UnknownReceipt;
	TestTrue(
		TEXT("unconfirmed restore records state_unknown"),
		TryPersistAssetDurability(
			OriginalSession.ToSharedRef(),
			OriginalBackend,
			41,
			Tracker,
			UnknownReceipt,
			Error));
	const int32 OriginalMutationCount =
		OriginalBackend.MutationEvents.Num();
	FDurableReceipt OriginalReceipt;
	TestTrue(
		TEXT("bound original state reconciles without replay"),
		TryReconcileAssetDurability(
			OriginalFixture.JournalRoot,
			OriginalSession->GetJournalId(),
			OriginalBackend.GetAllowedArtifactRoots(),
			TEXT("owned-worker:p4-009"),
			41,
			TEXT("verification:original"),
			ReconciliationOracle,
			Tracker,
			OriginalReceipt,
			Error));
	TestEqual(
		TEXT("original state becomes terminal rolled_back"),
		OriginalReceipt.GetStatus(),
		EDurableStatus::RolledBack);
	TestEqual(
		TEXT("original reconciliation records restoration result"),
		OriginalReceipt.GetRecoveryAction(),
		ERecoveryAction::RestoredOriginal);
	TestEqual(
		TEXT("original reconciliation does not replay restore"),
		OriginalBackend.MutationEvents.Num(),
		OriginalMutationCount);

	FOwnedDiskFixture PreparedFixture(TEXT("reconcile_prepared"));
	FRecordingAssetBackend PreparedBackend(PreparedFixture);
	FAssetDurableSessionPtr PreparedSession;
	TestTrue(
		TEXT("prepared fixture prepares"),
		TryPrepareAssetDurability(
			MakeRequest(Tracker),
			PreparedBackend,
			PreparedFixture.JournalRoot,
			Tracker,
			PreparedSession,
			Error));
	FAssetDurableSessionPtr InspectedPrepared;
	TArray<FDurableArtifactState> PreparedObservedStates;
	if (!TestTrue(
		TEXT("prepared journal can be inspected without its volatile session"),
		TryInspectAssetDurableJournal(
			PreparedFixture.JournalRoot,
			PreparedSession->GetJournalId(),
			PreparedBackend.GetAllowedArtifactRoots(),
			Tracker,
			InspectedPrepared,
			PreparedObservedStates,
			Error)))
	{
		return false;
	}
	TestEqual(
		TEXT("inspection preserves stable owner binding"),
		InspectedPrepared->GetRequest().GetJournalOwnerId(),
		FString(TEXT("owned-worker:p4-009")));
	TestEqual(
		TEXT("inspection preserves fence binding"),
		InspectedPrepared->GetRequest().GetFencingToken(),
		static_cast<int64>(41));
	const int32 PreparedMutationCount =
		PreparedBackend.MutationEvents.Num();
	FDurableReceipt PreparedReceipt;
	TestTrue(
		TEXT("prepared journal aborts from bound original evidence"),
		TryReconcileAssetDurability(
			PreparedFixture.JournalRoot,
			PreparedSession->GetJournalId(),
			PreparedBackend.GetAllowedArtifactRoots(),
			TEXT("owned-worker:p4-009"),
			41,
			TEXT("verification:prepared"),
			ReconciliationOracle,
			Tracker,
			PreparedReceipt,
			Error));
	TestEqual(
		TEXT("prepared journal becomes terminal rolled_back"),
		PreparedReceipt.GetStatus(),
		EDurableStatus::RolledBack);
	TestEqual(
		TEXT("prepared journal records no-mutation abort"),
		PreparedReceipt.GetRecoveryAction(),
		ERecoveryAction::AbortedBeforeMutation);
	TestEqual(
		TEXT("prepared reconciliation never invokes backend mutation"),
		PreparedBackend.MutationEvents.Num(),
		PreparedMutationCount);

	FOwnedDiskFixture TerminalForkFixture(TEXT("reconcile_terminal_fork"));
	FRecordingAssetBackend TerminalForkBackend(TerminalForkFixture);
	FAssetDurableSessionPtr TerminalForkSession;
	if (!TestTrue(
			TEXT("terminal fork fixture prepares"),
			TryPrepareAssetDurability(
				MakeRequest(Tracker),
				TerminalForkBackend,
				TerminalForkFixture.JournalRoot,
				Tracker,
				TerminalForkSession,
				Error)))
	{
		return false;
	}
	FString PreparedJournalText;
	TestTrue(
		TEXT("prepared journal is retained for historical fork simulation"),
		FFileHelper::LoadFileToString(
			PreparedJournalText,
			*TerminalForkSession->GetJournalPath()));
	TArray<FDurableReceipt> TerminalForkRecoveryReceipts;
	TestTrue(
		TEXT("prepared recovery records a terminal receipt"),
		TryRecoverAssetDurability(
			TerminalForkFixture.JournalRoot,
			TerminalForkBackend,
			41,
			TEXT("owned-worker:p4-009"),
			Tracker,
			TerminalForkRecoveryReceipts,
			Error));
	TestEqual(
		TEXT("prepared recovery has one terminal result"),
		TerminalForkRecoveryReceipts.Num(),
		1);
	const FString TerminalForkCompletedPath =
		TerminalForkFixture.JournalRoot
		/ TEXT("completed")
		/ (TerminalForkSession->GetJournalId() + TEXT(".json"));
	FString OriginalTerminalReceipt;
	TestTrue(
		TEXT("terminal receipt remains available as durable evidence"),
		FFileHelper::LoadFileToString(
			OriginalTerminalReceipt,
			*TerminalForkCompletedPath));

	TerminalForkBackend.bRestoreConfirmed = false;
	FDurableReceipt StaleWriterReceipt;
	TestFalse(
		TEXT("stale non-terminal writer cannot resurrect completed journal"),
		TryRollbackAssetDurability(
			TerminalForkSession.ToSharedRef(),
			TerminalForkBackend,
			41,
			false,
			Tracker,
			StaleWriterReceipt,
			Error));
	TestEqual(
		TEXT("stale writer failure is typed"),
		Error.GetCode(),
		FString(TEXT("durable_journal_already_terminal")));
	TestFalse(
		TEXT("blocked stale writer leaves no active transaction"),
		IFileManager::Get().DirectoryExists(
			*TerminalForkSession->GetTransactionRoot()));

	IFileManager::Get().MakeDirectory(
		*TerminalForkSession->GetTransactionRoot(),
		true);
	TestTrue(
		TEXT("historical active fork fixture is recreated"),
		FFileHelper::SaveStringToFile(
			PreparedJournalText,
			*TerminalForkSession->GetJournalPath(),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
	const int32 TerminalForkMutationCount =
		TerminalForkBackend.MutationEvents.Num();
	FDurableReceipt TerminalForkReceipt;
	TestTrue(
		TEXT("bound terminal evidence closes historical active fork"),
		TryReconcileAssetDurability(
			TerminalForkFixture.JournalRoot,
			TerminalForkSession->GetJournalId(),
			TerminalForkBackend.GetAllowedArtifactRoots(),
			TEXT("owned-worker:p4-009"),
			41,
			TEXT("verification:terminal-fork"),
			ReconciliationOracle,
			Tracker,
			TerminalForkReceipt,
			Error));
	TestEqual(
		TEXT("historical fork preserves the completed rollback result"),
		TerminalForkReceipt.GetStatus(),
		EDurableStatus::RolledBack);
	TestEqual(
		TEXT("historical pre-publish fork remains an abort"),
		TerminalForkReceipt.GetRecoveryAction(),
		ERecoveryAction::AbortedBeforeMutation);
	TestEqual(
		TEXT("fork reconciliation never replays a backend mutation"),
		TerminalForkBackend.MutationEvents.Num(),
		TerminalForkMutationCount);
	TestFalse(
		TEXT("reconciled historical fork leaves the active root"),
		IFileManager::Get().DirectoryExists(
			*TerminalForkSession->GetTransactionRoot()));
	FString PreservedTerminalReceipt;
	TestTrue(
		TEXT("original terminal receipt remains readable after fork closure"),
		FFileHelper::LoadFileToString(
			PreservedTerminalReceipt,
			*TerminalForkCompletedPath));
	TestEqual(
		TEXT("historical fork closure never overwrites terminal evidence"),
		PreservedTerminalReceipt,
		OriginalTerminalReceipt);

	FOwnedDiskFixture AmbiguousFixture(TEXT("reconcile_ambiguous"));
	FRecordingAssetBackend AmbiguousBackend(AmbiguousFixture);
	FAssetDurableSessionPtr AmbiguousSession;
	TestTrue(
		TEXT("ambiguous fixture prepares"),
		TryPrepareAssetDurability(
			MakeRequest(Tracker),
			AmbiguousBackend,
			AmbiguousFixture.JournalRoot,
			Tracker,
			AmbiguousSession,
			Error));
	FDurableReceipt AmbiguousPersistReceipt;
	TestTrue(
		TEXT("ambiguous fixture publishes"),
		TryPersistAssetDurability(
			AmbiguousSession.ToSharedRef(),
			AmbiguousBackend,
			41,
			Tracker,
			AmbiguousPersistReceipt,
			Error));
	TestTrue(
		TEXT("third state fixture writes"),
		FFileHelper::SaveStringToFile(
			TEXT("neither-original-nor-persisted"),
			*AmbiguousFixture.SaveHeader));
	FDurableReceipt AmbiguousReceipt;
	TestTrue(
		TEXT("ambiguous observation returns a typed non-terminal receipt"),
		TryReconcileAssetDurability(
			AmbiguousFixture.JournalRoot,
			AmbiguousSession->GetJournalId(),
			AmbiguousBackend.GetAllowedArtifactRoots(),
			TEXT("owned-worker:p4-009"),
			41,
			TEXT("verification:ambiguous"),
			ReconciliationOracle,
			Tracker,
			AmbiguousReceipt,
			Error));
	TestEqual(
		TEXT("ambiguous observation remains state_unknown"),
		AmbiguousReceipt.GetStatus(),
		EDurableStatus::StateUnknown);
	TestEqual(
		TEXT("ambiguous observation is typed"),
		AmbiguousReceipt.GetFailureCode(),
		FString(TEXT("durable_reconcile_evidence_insufficient")));
	TestTrue(
		TEXT("ambiguous journal remains active"),
		IFileManager::Get().DirectoryExists(
			*AmbiguousSession->GetTransactionRoot()));
	TestEqual(
		TEXT("oracle runs only for the four uniquely classified states"),
		ReconciliationOracle.ConfirmCount,
		4);

	FOwnedDiskFixture IsolationFixture(TEXT("recovery_isolation"));
	FRecordingAssetBackend IsolationBackend(IsolationFixture);
	FAssetDurableSessionPtr ValidSession;
	TestTrue(
		TEXT("valid recovery journal prepares"),
		TryPrepareAssetDurability(
			MakeRequest(Tracker),
			IsolationBackend,
			IsolationFixture.JournalRoot,
			Tracker,
			ValidSession,
			Error));
	const FString CorruptPath = IsolationFixture.JournalRoot
		/ TEXT("active/corrupt/journal.json");
	IFileManager::Get().MakeDirectory(
		*FPaths::GetPath(CorruptPath),
		true);
	TestTrue(
		TEXT("corrupt sibling journal fixture writes"),
		FFileHelper::SaveStringToFile(
			TEXT("not-a-journal"),
			*CorruptPath));
	TArray<FDurableReceipt> IsolationReceipts;
	TestTrue(
		TEXT("corrupt sibling does not hide valid recovery"),
		TryRecoverAssetDurability(
			IsolationFixture.JournalRoot,
			IsolationBackend,
			41,
			TEXT("owned-worker:p4-009"),
			Tracker,
			IsolationReceipts,
			Error));
	TestEqual(
		TEXT("valid and isolated failure receipts are both returned"),
		IsolationReceipts.Num(),
		2);
	TestTrue(
		TEXT("corrupt journal has a typed isolation receipt"),
		IsolationReceipts.ContainsByPredicate(
			[](const FDurableReceipt& Receipt)
			{
				return Receipt.GetJournalId() == TEXT("corrupt")
					&& Receipt.GetFailureCode()
						== TEXT("durable_journal_integrity_invalid");
			}));
	TestFalse(
		TEXT("valid prepared journal is still recovered"),
		IFileManager::Get().DirectoryExists(
			*ValidSession->GetTransactionRoot()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAssetDurableFenceBudgetContainmentTest,
	"UeAgentInterface.Core.AssetDurable.FenceBudgetContainment",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentAssetDurableFenceBudgetContainmentTest::RunTest(
	const FString& Parameters)
{
	FOwnedDiskFixture Fixture(TEXT("fence"));
	FRecordingAssetBackend Backend(Fixture);
	FUeAgentResourceTracker Tracker = MakeTracker();
	FAssetDurableSessionPtr Session;
	FAssetDurableError Error;
	if (!TestTrue(
		TEXT("prepare succeeds"),
		TryPrepareAssetDurability(
			MakeRequest(Tracker),
			Backend,
			Fixture.JournalRoot,
			Tracker,
			Session,
			Error)))
	{
		return false;
	}
	FDurableReceipt Receipt;
	TestFalse(
		TEXT("stale fence fails before durable write"),
		TryPersistAssetDurability(
			Session.ToSharedRef(),
			Backend,
			40,
			Tracker,
			Receipt,
			Error));
	TestEqual(
		TEXT("stale fence error is typed"),
		Error.GetCode(),
		FString(TEXT("durable_fence_stale")));
	TestEqual(
		TEXT("stale fence performs no durable mutation"),
		Backend.MutationEvents.Num(),
		0);

	FOwnedDiskFixture EscapeFixture(TEXT("escape"));
	FRecordingAssetBackend EscapeBackend(EscapeFixture);
	EscapeBackend.bEscapeArtifactPath = true;
	FAssetDurableSessionPtr EscapeSession;
	TestFalse(
		TEXT("artifact path outside backend root fails closed"),
		TryPrepareAssetDurability(
			MakeRequest(Tracker),
			EscapeBackend,
			EscapeFixture.JournalRoot,
			Tracker,
			EscapeSession,
			Error));
	TestEqual(
		TEXT("path failure is typed"),
		Error.GetCode(),
		FString(TEXT("durable_artifact_path_invalid")));

	FOwnedDiskFixture IntentFixture(TEXT("intent"));
	FRecordingAssetBackend IntentBackend(IntentFixture);
	FAssetDurableSessionPtr IntentSession;
	const FAssetDurableRequest IntentMismatchRequest(
		MakePlan(Tracker),
		{
			FAssetDurableTarget(
				MakeIdentity(false),
				TEXT("/Game/UAI/P4_009/SaveAsset"),
				EPackageHeaderKind::EvidenceRequired,
				EAssetDurableIntent::Delete,
				ERollbackGrade::DurableBackup)
		},
		41,
		TEXT("owned-worker:p4-009"),
		EAtomicityMode::MultiOwnerAtomicSupported);
	TestFalse(
		TEXT("durable intent must match the frozen plan intent"),
		TryPrepareAssetDurability(
			IntentMismatchRequest,
			IntentBackend,
			IntentFixture.JournalRoot,
			Tracker,
			IntentSession,
			Error));
	TestEqual(
		TEXT("intent mismatch is typed"),
		Error.GetCode(),
		FString(TEXT("durable_intent_plan_mismatch")));
	TestEqual(
		TEXT("intent mismatch performs no durable mutation"),
		IntentBackend.MutationEvents.Num(),
		0);

	FOwnedDiskFixture AtomicityFixture(TEXT("atomicity"));
	FRecordingAssetBackend AtomicityBackend(AtomicityFixture);
	AtomicityBackend.bSupportsAtomicity = false;
	FAssetDurableSessionPtr AtomicitySession;
	TestFalse(
		TEXT("backend atomicity capability is checked before writes"),
		TryPrepareAssetDurability(
			MakeRequest(Tracker),
			AtomicityBackend,
			AtomicityFixture.JournalRoot,
			Tracker,
			AtomicitySession,
			Error));
	TestEqual(
		TEXT("unsupported atomicity is typed"),
		Error.GetCode(),
		FString(TEXT("durable_atomicity_unsupported")));
	TestEqual(
		TEXT("unsupported atomicity performs no durable mutation"),
		AtomicityBackend.MutationEvents.Num(),
		0);

	FOwnedDiskFixture OwnerFixture(TEXT("owner"));
	FRecordingAssetBackend OwnerBackend(OwnerFixture);
	FAssetDurableSessionPtr OwnerSession;
	TestTrue(
		TEXT("owner recovery prepare succeeds"),
		TryPrepareAssetDurability(
			MakeRequest(Tracker),
			OwnerBackend,
			OwnerFixture.JournalRoot,
			Tracker,
			OwnerSession,
			Error));
	TArray<FDurableReceipt> OwnerReceipts;
	TestTrue(
		TEXT("foreign journal owner is isolated before recovery mutation"),
		TryRecoverAssetDurability(
			OwnerFixture.JournalRoot,
			OwnerBackend,
			41,
			TEXT("owned-worker:other"),
			Tracker,
			OwnerReceipts,
			Error));
	TestEqual(TEXT("foreign owner produces one receipt"), OwnerReceipts.Num(), 1);
	if (OwnerReceipts.Num() == 1)
	{
		TestEqual(
			TEXT("journal owner failure is typed"),
			OwnerReceipts[0].GetFailureCode(),
			FString(TEXT("durable_journal_owner_mismatch")));
		TestEqual(
			TEXT("foreign owner receipt is non-terminal"),
			OwnerReceipts[0].GetStatus(),
			EDurableStatus::StateUnknown);
	}
	TestEqual(
		TEXT("journal owner mismatch performs no mutation"),
		OwnerBackend.MutationEvents.Num(),
		0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAssetDurableNativePackageBackendTest,
	"UeAgentInterface.Core.AssetDurable.NativePackageBackend",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentAssetDurableNativePackageBackendTest::RunTest(
	const FString& Parameters)
{
	FNativePackageFixture Fixture;
	FPackageAssetDurableBackend Backend;
	FUeAgentResourceTracker Tracker = MakeTracker();
	FAssetDurableError Error;
	FAssetDurableSessionPtr SaveSession;
	if (!TestTrue(
		TEXT("native save prepare succeeds"),
		TryPrepareAssetDurability(
			MakeNativeRequest(Fixture, false, Tracker),
			Backend,
			Fixture.Root / TEXT("save_journal"),
			Tracker,
			SaveSession,
			Error)))
	{
		AddError(Error.GetCode() + TEXT(":") + Error.GetDetail());
		return false;
	}
	FDurableReceipt SaveReceipt;
	if (!TestTrue(
		TEXT("native package save persists"),
		TryPersistAssetDurability(
			SaveSession.ToSharedRef(),
			Backend,
			41,
			Tracker,
			SaveReceipt,
			Error)))
	{
		AddError(Error.GetCode() + TEXT(":") + Error.GetDetail());
		return false;
	}
	TestEqual(
		TEXT("native save remains unverified"),
		SaveReceipt.GetStatus(),
		EDurableStatus::PersistedUnverified);
	TestTrue(
		TEXT("native package header is written"),
		IFileManager::Get().FileExists(*Fixture.HeaderPath));
	TestTrue(
		TEXT("targeted durable save keeps package dirty"),
		Fixture.Package->IsDirty());

	FAssetDurableSessionPtr DeleteSession;
	if (!TestTrue(
		TEXT("native delete prepare succeeds"),
		TryPrepareAssetDurability(
			MakeNativeRequest(Fixture, true, Tracker),
			Backend,
			Fixture.Root / TEXT("delete_journal"),
			Tracker,
			DeleteSession,
			Error)))
	{
		AddError(Error.GetCode() + TEXT(":") + Error.GetDetail());
		return false;
	}
	FDurableReceipt DeleteReceipt;
	if (!TestTrue(
		TEXT("native package delete persists"),
		TryPersistAssetDurability(
			DeleteSession.ToSharedRef(),
			Backend,
			41,
			Tracker,
			DeleteReceipt,
			Error)))
	{
		AddError(Error.GetCode() + TEXT(":") + Error.GetDetail());
		return false;
	}
	TestEqual(
		TEXT("native delete remains unverified"),
		DeleteReceipt.GetStatus(),
		EDurableStatus::PersistedUnverified);
	TestFalse(
		TEXT("native package header is deleted"),
		IFileManager::Get().FileExists(*Fixture.HeaderPath));

	FNativePackageFixture DifferentNameFixture(true);
	TestNotEqual(
		TEXT("the regression fixture does not mirror its package name"),
		DifferentNameFixture.AssetName,
		FPackageName::GetLongPackageAssetName(
			DifferentNameFixture.PackageName));
	FAssetDurableSessionPtr DifferentNameSession;
	if (!TestTrue(
		TEXT("a package whose top-level asset has a different name prepares"),
		TryPrepareAssetDurability(
			MakeNativeRequest(DifferentNameFixture, false, Tracker),
			Backend,
			DifferentNameFixture.Root / TEXT("save_journal"),
			Tracker,
			DifferentNameSession,
			Error)))
	{
		AddError(Error.GetCode() + TEXT(":") + Error.GetDetail());
		return false;
	}
	FDurableReceipt DifferentNameReceipt;
	if (!TestTrue(
		TEXT("package-owned asset discovery persists the different-name asset"),
		TryPersistAssetDurability(
			DifferentNameSession.ToSharedRef(),
			Backend,
			41,
			Tracker,
			DifferentNameReceipt,
			Error)))
	{
		AddError(Error.GetCode() + TEXT(":") + Error.GetDetail());
		return false;
	}
	TestTrue(
		TEXT("the different-name package header is written"),
		IFileManager::Get().FileExists(*DifferentNameFixture.HeaderPath));
	return true;
}

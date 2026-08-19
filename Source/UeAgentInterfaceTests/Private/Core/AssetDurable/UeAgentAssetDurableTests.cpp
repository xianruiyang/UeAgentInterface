#include "Misc/AutomationTest.h"

#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Engine/Texture2D.h"
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
		return FUeAgentResourceTracker(FUeAgentResourceBudget({
			{EUeAgentResourceDimension::InputBytes, 1048576},
			{EUeAgentResourceDimension::OutputBytes, 1048576},
			{EUeAgentResourceDimension::StringBytes, 1048576},
			{EUeAgentResourceDimension::Nodes, 65536},
			{EUeAgentResourceDimension::Items, 65536},
			{EUeAgentResourceDimension::Files, FileLimit},
			{EUeAgentResourceDimension::ReferenceEdges, 65536},
			{EUeAgentResourceDimension::WorkUnits, WorkLimit},
			{
				EUeAgentResourceDimension::EstimatedMemoryBytes,
				8388608
			},
		}));
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
			FFileHelper::SaveStringToFile(TEXT("old-save-header"), *SaveHeader);
			FFileHelper::SaveStringToFile(TEXT("old-save-payload"), *SavePayload);
			FFileHelper::SaveStringToFile(
				TEXT("old-delete-header"),
				*DeleteHeader);
			FFileHelper::SaveStringToFile(
				TEXT("old-delete-payload"),
				*DeletePayload);
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
	};

	struct FNativePackageFixture
	{
		FNativePackageFixture()
		{
			const FString Suffix =
				FGuid::NewGuid().ToString(EGuidFormats::Digits);
			PackageName =
				TEXT("/Game/UAI/P4_010/Native_") + Suffix;
			AssetName =
				FPackageName::GetLongPackageAssetName(PackageName);
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
				const bool bDelete =
					Target.GetIntent() == EAssetDurableIntent::Delete;
				const FString Prefix = bDelete
					? TEXT("delete_asset")
					: TEXT("save_asset");
				Artifacts.Emplace(
					Target.GetIdentity().GetCanonicalKey()
						+ TEXT(":package_header"),
					Target.GetIdentity(),
					Target.GetPackageName(),
					Fixture.FinalRoot / (Prefix + TEXT(".uasset")),
					Target.GetIntent(),
					TEXT("package_header"));
				Artifacts.Emplace(
					Target.GetIdentity().GetCanonicalKey()
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
		bool bRestoreConfirmed = true;
		bool bEscapeArtifactPath = false;
		bool bSupportsAtomicity = true;
		FString ObservedState = TEXT("original");
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
					EAssetDurableIntent::Save,
					ERollbackGrade::DurableBackup),
				FAssetDurableTarget(
					MakeIdentity(true),
					TEXT("/Game/UAI/P4_009/DeleteAsset"),
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
	FUeAgentAssetDurablePrepareJournalTest,
	"UeAgentInterface.Core.AssetDurable.PrepareJournal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentAssetDurablePrepareJournalTest::RunTest(
	const FString& Parameters)
{
	FOwnedDiskFixture Fixture(TEXT("prepare"));
	FRecordingAssetBackend Backend(Fixture);
	FUeAgentResourceTracker Tracker = MakeTracker();
	FAssetDurableSessionPtr Session;
	FAssetDurableError Error;
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
	TestEqual(
		TEXT("prepare performs no durable mutation"),
		Backend.MutationEvents.Num(),
		0);
	return true;
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

	FUeAgentResourceTracker LowTracker = MakeTracker(1);
	FAssetDurableSessionPtr LowSession;
	TestFalse(
		TEXT("operation budget exhaustion fails closed"),
		TryPrepareAssetDurability(
			MakeRequest(Tracker),
			Backend,
			Fixture.Root / TEXT("low_journal"),
			LowTracker,
			LowSession,
			Error));
	TestEqual(
		TEXT("resource error is preserved"),
		Error.GetCode(),
		FString(TEXT("resource_budget_exceeded")));

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
	TestFalse(
		TEXT("foreign journal owner fails before recovery mutation"),
		TryRecoverAssetDurability(
			OwnerFixture.JournalRoot,
			OwnerBackend,
			41,
			TEXT("owned-worker:other"),
			Tracker,
			OwnerReceipts,
			Error));
	TestEqual(
		TEXT("journal owner failure is typed"),
		Error.GetCode(),
		FString(TEXT("durable_journal_owner_mismatch")));
	TestEqual(
		TEXT("journal owner mismatch performs no mutation"),
		OwnerBackend.MutationEvents.Num(),
		0);

	const FString EmptyJournalRoot =
		Fixture.Root / TEXT("empty_journal");
	IFileManager::Get().MakeDirectory(
		*(EmptyJournalRoot
			/ TEXT("active/one/two/three")),
		true);
	FUeAgentResourceTracker EnumerationTracker = MakeTracker(1);
	TArray<FDurableReceipt> EmptyReceipts;
	TestFalse(
		TEXT("empty recovery tree still consumes operation budget"),
		TryRecoverAssetDurability(
			EmptyJournalRoot,
			Backend,
			41,
			TEXT("owned-worker:p4-009"),
			EnumerationTracker,
			EmptyReceipts,
			Error));
	TestEqual(
		TEXT("enumeration budget error is preserved"),
		Error.GetCode(),
		FString(TEXT("resource_budget_exceeded")));
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
	return true;
}

#include "DomainAdapters/AssetDestructiveImport/UeAgentAssetOperationsOwner.h"

#include "Animation/MorphTarget.h"
#include "Animation/SkinWeightProfile.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "EditorReimportHandler.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "HAL/ThreadSafeCounter.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Sound/SoundWave.h"
#include "PackageTools.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"

namespace UE::UAI::DomainAdapters::AssetDestructiveImport
{
	using namespace AssetDurable;
	using namespace ContextResource;
	using namespace Contract;
	using namespace PlanningPreflight;
	using namespace StateSemantic;

	namespace
	{
		FUeAgentResourceTracker MakeTracker()
		{
			return FUeAgentResourceTracker(FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, 1024 * 1024},
				{EUeAgentResourceDimension::OutputBytes, 1024 * 1024},
				{EUeAgentResourceDimension::StringBytes, 1024 * 1024},
				{EUeAgentResourceDimension::Nodes, 8192},
				{EUeAgentResourceDimension::Items, 8192},
				{EUeAgentResourceDimension::Files, 512},
				{EUeAgentResourceDimension::ReferenceEdges, 8192},
				{EUeAgentResourceDimension::WorkUnits, 131072},
				{EUeAgentResourceDimension::EstimatedMemoryBytes, 32 * 1024 * 1024},
			}));
		}

		bool TryPackageName(FString Ref, FString& OutPackageName)
		{
			Ref.TrimStartAndEndInline();
			OutPackageName = Ref.Contains(TEXT("."))
				? FPackageName::ObjectPathToPackageName(Ref)
				: MoveTemp(Ref);
			return OutPackageName.StartsWith(TEXT("/Game/"))
				&& FPackageName::IsValidLongPackageName(OutPackageName, true);
		}

		FString ObjectPath(const FString& PackageName)
		{
			return PackageName + TEXT(".")
				+ FPackageName::GetLongPackageAssetName(PackageName);
		}

		UObject* ReloadForIndependentReadback(const FString& PackageName)
		{
			if (UPackage* const Package = FindPackage(nullptr, *PackageName))
			{
				Package->SetDirtyFlag(false);
				FText Error;
				if (!UPackageTools::UnloadPackages({Package}, Error, true))
				{
					return nullptr;
				}
			}
			CollectGarbage(RF_NoFlags, true);
			return LoadObject<UObject>(nullptr, *ObjectPath(PackageName));
		}

		FUeAgentCanonicalIdentity IdentityFor(
			const FString& PackageName,
			const FString& Selector)
		{
			return FUeAgentCanonicalIdentity(
				TEXT("uai.asset.") + Selector,
				TEXT("asset"),
				PackageName,
				Selector,
				TEXT("asset"),
				TEXT("asset-destructive-v1"));
		}

		FPlanningPreflightPlanRef MakePlan(
			const FUeAgentAssetOperationRequest& Request,
			const FString& PackageName,
			const FString& OwnerId,
			const FUeAgentCanonicalIdentity& Identity,
			const EChangeIntent Intent)
		{
			TArray<FPlannedChange> Changes;
			Changes.Emplace(
				Identity, Intent, FUeAgentCanonicalValuePtr(),
				FUeAgentCanonicalValuePtr(),
				TArray<FString>{TEXT("uai:asset:write")},
				TEXT("asset-destructive:direct"));
			TArray<FImpact> Impacts;
			Impacts.Emplace(
				Identity, Identity, TEXT("direct"),
				TEXT("asset-destructive:direct"));
			return MakeShared<FPlanningPreflightPlan>(
				FUeAgentDescriptorIdentity(
					TEXT("uai.asset.destructive/v1"), 1,
					Json::Sha256Utf8(TEXT("uai.asset.destructive/v1"))),
				Request.RequestId,
				TEXT("asset-destructive:") + Request.RequestId,
				Request.CallerId,
				TArray<FString>{TEXT("uai:asset:write")},
				TEXT("asset-destructive:preflight:") + Request.RequestId,
				FUeAgentStateRevision(OwnerId, TEXT("asset-destructive-v1"), 1),
				TEXT("asset-destructive:preflight:native"),
				FChangeSet(MoveTemp(Changes)),
				FImpactSet(MoveTemp(Impacts)),
				TArray<FPrecondition>(),
				Json::Sha256Utf8(
					Request.CommandId + TEXT(":") + Request.RequestId
					+ TEXT(":") + PackageName));
		}

		class FDurableMutation final
		{
		public:
			bool Prepare(
				const FUeAgentAssetOperationRequest& Request,
				const FString& PackageName,
				const FString& OwnerId,
				const FString& JournalRoot,
				const EAssetDurableIntent DurableIntent,
				const int64 FencingToken,
				FString& OutError)
			{
				Identity = IdentityFor(PackageName, TEXT("state"));
				Token = FencingToken;
				Plan = MakePlan(
					Request, PackageName, OwnerId, Identity,
					DurableIntent == EAssetDurableIntent::Delete
						? EChangeIntent::Delete : EChangeIntent::Update);
				const FAssetDurableRequest DurableRequest(
					Plan.ToSharedRef(),
					{FAssetDurableTarget(
						Identity, PackageName, DurableIntent,
						ERollbackGrade::DurableBackup)},
					Token, OwnerId, EAtomicityMode::CompensatedSaga);
				FAssetDurableError Error;
				if (!TryPrepareAssetDurability(
						DurableRequest, Backend, JournalRoot,
						Resources, Session, Error)
					|| !Session.IsValid())
				{
					OutError = Error.GetCode() + TEXT(":") + Error.GetDetail();
					return false;
				}
				return true;
			}

			bool Persist(FString& OutError)
			{
				FAssetDurableError Error;
				if (!TryPersistAssetDurability(
						Session.ToSharedRef(), Backend, Token,
						Resources, Receipt, Error))
				{
					OutError = Error.GetCode() + TEXT(":") + Error.GetDetail();
					return false;
				}
				return true;
			}

			bool Acknowledge(
				const FString& VerificationId,
				FString& OutError)
			{
				const FAssetDurableSessionRef Persisted =
					MakeShared<const FAssetDurableSession>(
						Session->GetRequest(), Session->GetInventory(),
						Session->GetJournalId(), Session->GetJournalPath(),
						Session->GetJournalRoot(), Session->GetTransactionRoot(),
						Session->GetStagingRoot(), Receipt.GetPhaseHistory());
				const FDurableVerificationAcknowledgement Ack(
					Receipt.GetJournalId(), Receipt.GetPlanFingerprint(),
					Receipt.GetInventoryHash(), VerificationId,
					Session->GetRequest().GetJournalOwnerId(), Token);
				FAssetDurableError Error;
				if (!TryAcknowledgeAssetDurability(
						Persisted, Ack, Token, Resources, Error))
				{
					OutError = Error.GetCode() + TEXT(":") + Error.GetDetail();
					return false;
				}
				return true;
			}

			void Rollback(const bool bMemoryConfirmed)
			{
				if (!Session.IsValid()) return;
				FDurableReceipt RollbackReceipt;
				FAssetDurableError Error;
				TryRollbackAssetDurability(
					Session.ToSharedRef(), Backend, Token, bMemoryConfirmed,
					Resources, RollbackReceipt, Error);
			}

			FString JournalId() const
			{
				return Session.IsValid() ? Session->GetJournalId() : FString();
			}

		private:
			FUeAgentResourceTracker Resources = MakeTracker();
			FPackageAssetDurableBackend Backend;
			FUeAgentCanonicalIdentity Identity = IdentityFor(
				TEXT("/Game/Invalid"), TEXT("state"));
			FPlanningPreflightPlanPtr Plan;
			FAssetDurableSessionPtr Session;
			FDurableReceipt Receipt;
			int64 Token = -1;
		};

		class FAssetOperationsOwner final : public IUeAgentAssetOperationsOwner
		{
		public:
			FAssetOperationsOwner(FString InJournalRoot, FString InOwnerId)
				: JournalRoot(MoveTemp(InJournalRoot)), OwnerId(MoveTemp(InOwnerId))
			{
			}

			FUeAgentAssetOperationResult Execute(
				const FUeAgentAssetOperationRequest& Request,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation) override
			{
				FUeAgentAssetOperationResult Result;
				Result.CommandId = Request.CommandId;
				Result.TargetRef = Request.TargetRef;
				auto Fail = [&Result](const FString& Code, const FString& Detail)
				{
					Result.ErrorCode = Code;
					Result.ErrorDetail = Detail;
					return Result;
				};
				if (!IsInGameThread())
				{
					return Fail(TEXT("asset_operation_game_thread_required"),
						Request.CommandId);
				}
				if (Cancellation->IsCancelled())
				{
					return Fail(TEXT("asset_operation_cancelled"), Request.RequestId);
				}
				if (Request.CommandId == TEXT("asset.move"))
				{
					return Move(Request);
				}
				if (Request.CommandId == TEXT("asset.fixup_redirectors"))
				{
					return Fixup(Request);
				}
				if (Request.CommandId.StartsWith(TEXT("asset.reimport_")))
				{
					return Reimport(Request);
				}
				if (Request.CommandId.StartsWith(TEXT("skeletal_mesh.remove_")))
				{
					return RemoveSkeletalItem(Request);
				}
				return Fail(TEXT("asset_operation_unknown"), Request.CommandId);
			}

		private:
			FUeAgentAssetOperationResult Move(
				const FUeAgentAssetOperationRequest& Request)
			{
				FUeAgentAssetOperationResult Result;
				Result.CommandId = Request.CommandId;
				Result.TargetRef = Request.TargetRef;
				Result.ResultRef = Request.Operand;
				FString SourcePackage;
				FString DestinationPackage;
				if (!TryPackageName(Request.TargetRef, SourcePackage)
					|| !TryPackageName(Request.Operand, DestinationPackage))
				{
					Result.ErrorCode = TEXT("asset_move_target_invalid");
					return Result;
				}
				UObject* const Asset = LoadObject<UObject>(
					nullptr, *ObjectPath(SourcePackage));
				FString Existing;
				if (!Asset || FPackageName::DoesPackageExist(DestinationPackage, &Existing))
				{
					Result.ErrorCode = Asset
						? TEXT("asset_move_destination_conflict")
						: TEXT("asset_move_target_missing");
					return Result;
				}
				if (Asset->GetOutermost()->IsDirty())
				{
					Result.ErrorCode = TEXT("asset_move_dirty_package");
					Result.ErrorDetail =
						TEXT("save or discard pending source changes before moving the asset");
					return Result;
				}
				FDurableMutation Durable;
				FString Error;
				if (!Durable.Prepare(
						Request, SourcePackage, OwnerId, JournalRoot,
						EAssetDurableIntent::Delete, Counter.Increment(), Error))
				{
					Result.ErrorCode = TEXT("asset_move_prepare_failed");
					Result.ErrorDetail = Error;
					return Result;
				}
				Result.JournalId = Durable.JournalId();
				FAssetRenameData Rename(
					Asset, FPackageName::GetLongPackagePath(DestinationPackage),
					FPackageName::GetLongPackageAssetName(DestinationPackage));
				TGuardValue<bool> Guard(GIsRunningUnattendedScript, true);
				if (!FModuleManager::LoadModuleChecked<FAssetToolsModule>(
						TEXT("AssetTools")).Get().RenameAssets({Rename}))
				{
					Durable.Rollback(true);
					Result.ErrorCode = TEXT("asset_move_native_failed");
					return Result;
				}
				if (!Durable.Persist(Error))
				{
					Durable.Rollback(false);
					Result.TerminalState = TEXT("state_unknown");
					Result.ErrorCode = TEXT("asset_move_persist_failed");
					Result.ErrorDetail = Error;
					return Result;
				}
				FString SourceFilename;
				FString DestinationFilename;
				Result.bReadbackVerified =
					!FPackageName::DoesPackageExist(SourcePackage, &SourceFilename)
					&& FPackageName::DoesPackageExist(
						DestinationPackage, &DestinationFilename)
					&& ReloadForIndependentReadback(DestinationPackage) != nullptr;
				Result.VerificationId = TEXT("asset-move-readback:") + Request.RequestId;
				if (!Result.bReadbackVerified
					|| !Durable.Acknowledge(Result.VerificationId, Error))
				{
					Result.TerminalState = TEXT("state_unknown");
					Result.ErrorCode = TEXT("asset_move_readback_failed");
					Result.ErrorDetail = Error;
					return Result;
				}
				Result.bChanged = true;
				Result.TerminalState = TEXT("committed");
				return Result;
			}

			FUeAgentAssetOperationResult Fixup(
				const FUeAgentAssetOperationRequest& Request)
			{
				FUeAgentAssetOperationResult Result;
				Result.CommandId = Request.CommandId;
				Result.TargetRef = Request.TargetRef;
				FString Root = Request.TargetRef;
				Root.TrimStartAndEndInline();
				if (!Root.StartsWith(TEXT("/Game/")))
				{
					Result.ErrorCode = TEXT("asset_fixup_root_invalid");
					return Result;
				}
				FARFilter Filter;
				Filter.PackagePaths.Add(FName(*Root));
				Filter.ClassPaths.Add(UObjectRedirector::StaticClass()->GetClassPathName());
				Filter.bRecursivePaths = true;
				TArray<FAssetData> Data;
				FAssetRegistryModule::GetRegistry().GetAssets(Filter, Data);
				TArray<UObjectRedirector*> Redirectors;
				for (const FAssetData& Item : Data)
				{
					if (UObjectRedirector* const Redirector =
						Cast<UObjectRedirector>(Item.GetAsset()))
					{
						Redirectors.Add(Redirector);
					}
				}
				FModuleManager::LoadModuleChecked<FAssetToolsModule>(
					TEXT("AssetTools")).Get().FixupReferencers(
						Redirectors, false, ERedirectFixupMode::DeleteFixedUpRedirectors);
				Data.Reset();
				FAssetRegistryModule::GetRegistry().GetAssets(Filter, Data);
				Result.bChanged = !Redirectors.IsEmpty();
				Result.bReadbackVerified = Data.IsEmpty();
				Result.TerminalState = Result.bReadbackVerified
					? TEXT("committed") : TEXT("state_unknown");
				if (!Result.bReadbackVerified)
				{
					Result.ErrorCode = TEXT("asset_fixup_readback_failed");
				}
				return Result;
			}

			FUeAgentAssetOperationResult Reimport(
				const FUeAgentAssetOperationRequest& Request)
			{
				FUeAgentAssetOperationResult Result;
				Result.CommandId = Request.CommandId;
				Result.TargetRef = Request.TargetRef;
				FString PackageName;
				if (!TryPackageName(Request.TargetRef, PackageName))
				{
					Result.ErrorCode = TEXT("asset_reimport_target_invalid");
					return Result;
				}
				UObject* const Asset = LoadObject<UObject>(
					nullptr, *ObjectPath(PackageName));
				if (!Asset)
				{
					Result.ErrorCode = TEXT("asset_reimport_target_missing");
					return Result;
				}
				const bool bTypeMatches =
					(Request.CommandId == TEXT("asset.reimport_texture")
						&& Asset->IsA<UTexture>())
					|| (Request.CommandId == TEXT("asset.reimport_sound_wave")
						&& Asset->IsA<USoundWave>())
					|| (Request.CommandId == TEXT("asset.reimport_static_mesh")
						&& Asset->IsA<UStaticMesh>())
					|| (Request.CommandId
							== TEXT("asset.reimport_sparse_volume_texture")
						&& Asset->GetClass()->GetName().Contains(
							TEXT("SparseVolumeTexture")));
				if (!bTypeMatches || !FPaths::FileExists(Request.Operand))
				{
					Result.ErrorCode = !bTypeMatches
						? TEXT("asset_reimport_type_mismatch")
						: TEXT("asset_reimport_source_missing");
					return Result;
				}
				FDurableMutation Durable;
				FString Error;
				if (!Durable.Prepare(
						Request, PackageName, OwnerId, JournalRoot,
						EAssetDurableIntent::Save, Counter.Increment(), Error))
				{
					Result.ErrorCode = TEXT("asset_reimport_prepare_failed");
					Result.ErrorDetail = Error;
					return Result;
				}
				Result.JournalId = Durable.JournalId();
				if (!FReimportManager::Instance()->Reimport(
						Asset, false, false, Request.Operand))
				{
					Durable.Rollback(true);
					Result.ErrorCode = TEXT("asset_reimport_native_failed");
					return Result;
				}
				if (!Durable.Persist(Error))
				{
					Durable.Rollback(false);
					Result.TerminalState = TEXT("state_unknown");
					Result.ErrorCode = TEXT("asset_reimport_persist_failed");
					Result.ErrorDetail = Error;
					return Result;
				}
				Result.bReadbackVerified =
					ReloadForIndependentReadback(PackageName) != nullptr;
				Result.VerificationId = TEXT("asset-reimport-readback:")
					+ Request.RequestId;
				if (!Result.bReadbackVerified
					|| !Durable.Acknowledge(Result.VerificationId, Error))
				{
					Result.TerminalState = TEXT("state_unknown");
					Result.ErrorCode = TEXT("asset_reimport_readback_failed");
					Result.ErrorDetail = Error;
					return Result;
				}
				Result.bChanged = true;
				Result.TerminalState = TEXT("committed");
				return Result;
			}

			FUeAgentAssetOperationResult RemoveSkeletalItem(
				const FUeAgentAssetOperationRequest& Request)
			{
				FUeAgentAssetOperationResult Result;
				Result.CommandId = Request.CommandId;
				Result.TargetRef = Request.TargetRef;
				FString PackageName;
				if (!TryPackageName(Request.TargetRef, PackageName))
				{
					Result.ErrorCode = TEXT("skeletal_mesh_target_invalid");
					return Result;
				}
				USkeletalMesh* const Mesh = LoadObject<USkeletalMesh>(
					nullptr, *ObjectPath(PackageName));
				const FName ItemName(*Request.Operand);
				const bool bMorph = Request.CommandId
					== TEXT("skeletal_mesh.remove_morph_target");
				UMorphTarget* const Morph = Mesh
					? (Mesh->FindMorphTarget(ItemName)
						? Mesh->FindMorphTarget(ItemName)
						: FindObject<UMorphTarget>(Mesh, *Request.Operand))
					: nullptr;
				const bool bExists = Mesh && (bMorph
					? Morph != nullptr
					: Mesh->GetSkinWeightProfiles().ContainsByPredicate(
						[ItemName](const FSkinWeightProfileInfo& Item)
						{
							return Item.Name == ItemName;
						}));
				if (!bExists)
				{
					Result.ErrorCode = Mesh
						? TEXT("skeletal_mesh_item_missing")
						: TEXT("skeletal_mesh_target_missing");
					return Result;
				}
				FDurableMutation Durable;
				FString Error;
				if (!Durable.Prepare(
						Request, PackageName, OwnerId, JournalRoot,
						EAssetDurableIntent::Save, Counter.Increment(), Error))
				{
					Result.ErrorCode = TEXT("skeletal_mesh_prepare_failed");
					Result.ErrorDetail = Error;
					return Result;
				}
				Result.JournalId = Durable.JournalId();
				if (bMorph)
				{
					if (Mesh->FindMorphTarget(ItemName))
					{
						if (!Mesh->RemoveMorphTargets({ItemName}))
						{
							Durable.Rollback(true);
							Result.ErrorCode = TEXT("skeletal_mesh_remove_failed");
							return Result;
						}
					}
					else
					{
						Morph->Rename(
							nullptr,
							GetTransientPackage(),
							REN_DontCreateRedirectors | REN_NonTransactional);
						Morph->MarkAsGarbage();
					}
				}
				else
				{
					Mesh->GetSkinWeightProfiles().RemoveAll(
						[ItemName](const FSkinWeightProfileInfo& Item)
						{
							return Item.Name == ItemName;
						});
				}
				Mesh->MarkPackageDirty();
				if (!Durable.Persist(Error))
				{
					Durable.Rollback(false);
					Result.TerminalState = TEXT("state_unknown");
					Result.ErrorCode = TEXT("skeletal_mesh_persist_failed");
					Result.ErrorDetail = Error;
					return Result;
				}
				USkeletalMesh* const ReadbackMesh = Cast<USkeletalMesh>(
					ReloadForIndependentReadback(PackageName));
				Result.bReadbackVerified = ReadbackMesh && (bMorph
					? ReadbackMesh->FindMorphTarget(ItemName) == nullptr
					: !ReadbackMesh->GetSkinWeightProfiles().ContainsByPredicate(
						[ItemName](const FSkinWeightProfileInfo& Item)
						{
							return Item.Name == ItemName;
						}));
				Result.VerificationId = TEXT("skeletal-remove-readback:")
					+ Request.RequestId;
				if (!Result.bReadbackVerified
					|| !Durable.Acknowledge(Result.VerificationId, Error))
				{
					Result.TerminalState = TEXT("state_unknown");
					Result.ErrorCode = TEXT("skeletal_mesh_readback_failed");
					Result.ErrorDetail = Error;
					return Result;
				}
				Result.bChanged = true;
				Result.TerminalState = TEXT("committed");
				return Result;
			}

			FString JournalRoot;
			FString OwnerId;
			FThreadSafeCounter64 Counter;
		};
	}

	bool TryCreateAssetOperationsOwner(
		FString JournalRoot,
		FString JournalOwnerId,
		TSharedPtr<IUeAgentAssetOperationsOwner>& OutOwner,
		FString& OutError)
	{
		OutOwner.Reset();
		OutError.Reset();
		JournalRoot = FPaths::ConvertRelativePathToFull(JournalRoot);
		JournalOwnerId.TrimStartAndEndInline();
		if (JournalRoot.IsEmpty() || JournalOwnerId.IsEmpty())
		{
			OutError = TEXT("asset_operations_owner_config_invalid");
			return false;
		}
		OutOwner = MakeShared<FAssetOperationsOwner>(
			MoveTemp(JournalRoot), MoveTemp(JournalOwnerId));
		return true;
	}
}

#include "DomainAdapters/ExternalImport/SkinWeight/UeAgentSkinWeightImportProvider.h"

#include "Animation/SkinWeightProfile.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "PackageTools.h"
#include "SkinWeightsUtilities.h"
#include "UObject/GarbageCollection.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

#include <openssl/sha.h>

namespace UE::UAI::DomainAdapters::ExternalImport::SkinWeight
{
	namespace
	{
		void SetError(
			FSkinWeightImportError& OutError,
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString())
		{
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
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

		FString HashBytes(const uint8* Data, const int64 Size)
		{
			FSHA256Signature Signature{};
			const uint8 Empty = 0;
			if (!SHA256(
					Size > 0 ? Data : &Empty,
					static_cast<size_t>(Size),
					Signature.Signature))
			{
				return FString();
			}
			return TEXT("sha256:") + Signature.ToString().ToLower();
		}

		FString HashFileSha256(const FString& Filename)
		{
			TArray<uint8> Bytes;
			return FFileHelper::LoadFileToArray(Bytes, *Filename)
				? HashBytes(Bytes.GetData(), Bytes.Num())
				: FString();
		}

		FString HashUtf8Sha256(const FString& Value)
		{
			const FTCHARToUTF8 Utf8(*Value);
			return HashBytes(
				reinterpret_cast<const uint8*>(Utf8.Get()),
				Utf8.Length());
		}

		bool TryNormalizeSource(
			FString SourceRef,
			FString& OutSource,
			FSkinWeightImportError& OutError)
		{
			SourceRef.TrimStartAndEndInline();
			if (SourceRef.IsEmpty())
			{
				SetError(OutError, TEXT("skin_weight_import_source_required"));
				return false;
			}
			OutSource = FPaths::IsRelative(SourceRef)
				? FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), SourceRef)
				: FPaths::ConvertRelativePathToFull(SourceRef);
			FPaths::NormalizeFilename(OutSource);
			if (!FPaths::FileExists(OutSource))
			{
				SetError(
					OutError,
					TEXT("skin_weight_import_source_not_found"),
					OutSource);
				return false;
			}
			if (!FPaths::GetExtension(OutSource).Equals(
					TEXT("fbx"),
					ESearchCase::IgnoreCase))
			{
				SetError(
					OutError,
					TEXT("skin_weight_import_source_extension_invalid"),
					OutSource,
					TEXT("expected .fbx"));
				return false;
			}
			return true;
		}

		bool HasProfile(const USkeletalMesh& Mesh, const FName ProfileName)
		{
			return Mesh.GetSkinWeightProfiles().ContainsByPredicate(
				[ProfileName](const FSkinWeightProfileInfo& Profile)
				{
					return Profile.Name == ProfileName;
				});
		}

		FString MetadataPrefix(const FString& ProfileName)
		{
			const FString Hash = HashUtf8Sha256(ProfileName);
			return TEXT("UAI.SkinWeightImport.") + Hash.Mid(7, 16) + TEXT(".");
		}

		FString MetadataKey(
			const FString& ProfileName,
			const TCHAR* Suffix)
		{
			return MetadataPrefix(ProfileName) + Suffix;
		}

		class FSession final : public ISkinWeightImportSession
		{
		public:
			FSession(
				FSkinWeightImportSpec InSpec,
				FString InSource,
				FString InSourceSha256,
				FString InPackageName,
				FString InPackageFilename,
				FString InExpectedRevision,
				FString InBackupFilename,
				USkeletalMesh* InMesh)
				: Spec(MoveTemp(InSpec))
				, Source(MoveTemp(InSource))
				, SourceSha256(MoveTemp(InSourceSha256))
				, PackageName(MoveTemp(InPackageName))
				, TargetObjectPath(ObjectPath(PackageName))
				, PackageFilename(MoveTemp(InPackageFilename))
				, ExpectedRevision(MoveTemp(InExpectedRevision))
				, BackupFilename(MoveTemp(InBackupFilename))
				, Mesh(InMesh)
			{
			}

			virtual ~FSession() override
			{
				DeleteBackup();
			}

			virtual const FString& GetTargetIdentity() const override
			{
				return TargetObjectPath;
			}

			virtual const FString& GetExpectedRevision() const override
			{
				return ExpectedRevision;
			}

			virtual bool CheckPreExecute(
				FSkinWeightImportError& OutError) override
			{
				if (!Mesh
					|| !FPackageName::DoesPackageExist(PackageName)
					|| HashFileSha256(PackageFilename) != ExpectedRevision)
				{
					SetError(
						OutError,
						TEXT("skin_weight_import_version_conflict"),
						TargetObjectPath);
					return false;
				}
				if (HasProfile(*Mesh, FName(*Spec.ProfileName)))
				{
					SetError(
						OutError,
						TEXT("skin_weight_import_profile_conflict"),
						Spec.ProfileName);
					return false;
				}
				return true;
			}

			virtual bool Execute(FSkinWeightImportError& OutError) override
			{
				if (!Mesh)
				{
					SetError(
						OutError,
						TEXT("skin_weight_import_target_missing"),
						TargetObjectPath);
					return false;
				}
				Mesh->Modify();
				bMutationStarted = true;
				if (!FSkinWeightsUtilities::ImportAlternateSkinWeight(
						Mesh,
						Source,
						Spec.LodIndex,
						FName(*Spec.ProfileName),
						false))
				{
					SetError(
						OutError,
						TEXT("skin_weight_import_native_failed"),
						TargetObjectPath,
						Source);
					return false;
				}
				return true;
			}

			virtual bool DidMutationStart() const override
			{
				return bMutationStarted;
			}

			virtual bool Settle(FSkinWeightImportError& OutError) override
			{
				if (!Mesh
					|| !HasProfile(*Mesh, FName(*Spec.ProfileName)))
				{
					SetError(
						OutError,
						TEXT("skin_weight_import_settle_profile_missing"),
						Spec.ProfileName);
					return false;
				}
				Mesh->PostEditChange();
				UPackage* const Package = Mesh->GetOutermost();
				if (!Package || Package->GetName() != PackageName)
				{
					SetError(
						OutError,
						TEXT("skin_weight_import_settle_target_invalid"),
						TargetObjectPath);
					return false;
				}
				FMetaData& Metadata = Package->GetMetaData();
				const FString ProfileKey = MetadataKey(Spec.ProfileName, TEXT("Profile"));
				const FString SourceKey = MetadataKey(Spec.ProfileName, TEXT("Source"));
				const FString HashKey = MetadataKey(Spec.ProfileName, TEXT("SourceSha256"));
				const FString LodKey = MetadataKey(Spec.ProfileName, TEXT("LodIndex"));
				Metadata.SetValue(Mesh, *ProfileKey, *Spec.ProfileName);
				Metadata.SetValue(Mesh, *SourceKey, *Source);
				Metadata.SetValue(Mesh, *HashKey, *SourceSha256);
				Metadata.SetValue(
					Mesh,
					*LodKey,
					*FString::FromInt(Spec.LodIndex));
				Package->SetDirtyFlag(true);
				return true;
			}

			virtual bool CheckPrePersist(
				FSkinWeightImportError& OutError) override
			{
				if (!Mesh
					|| !Mesh->GetOutermost()
					|| !Mesh->GetOutermost()->IsDirty()
					|| !HasProfile(*Mesh, FName(*Spec.ProfileName)))
				{
					SetError(
						OutError,
						TEXT("skin_weight_import_pre_persist_fence_failed"),
						TargetObjectPath);
					return false;
				}
				return true;
			}

			virtual bool Persist(
				FString& OutPersistenceReceiptId,
				FSkinWeightImportError& OutError) override
			{
				if (!Mesh || !Mesh->GetOutermost())
				{
					SetError(
						OutError,
						TEXT("skin_weight_import_persist_target_missing"),
						TargetObjectPath);
					return false;
				}
				FSavePackageArgs Args;
				Args.TopLevelFlags = RF_Public | RF_Standalone;
				Args.SaveFlags = SAVE_NoError;
				if (!UPackage::SavePackage(
						Mesh->GetOutermost(),
						Mesh,
						*PackageFilename,
						Args))
				{
					SetError(
						OutError,
						TEXT("skin_weight_import_persist_failed"),
						TargetObjectPath,
						PackageFilename);
					return false;
				}
				OutPersistenceReceiptId = HashFileSha256(PackageFilename);
				if (OutPersistenceReceiptId.IsEmpty())
				{
					SetError(
						OutError,
						TEXT("skin_weight_import_persistence_receipt_missing"),
						PackageFilename);
					return false;
				}
				return true;
			}

			virtual bool Verify(
				FSkinWeightImportReadback& OutReadback,
				FSkinWeightImportError& OutError) override
			{
				if (Mesh && Mesh->GetOutermost())
				{
					UPackage* const Package = Mesh->GetOutermost();
					Package->SetDirtyFlag(false);
					Mesh = nullptr;
					FText UnloadError;
					if (!UPackageTools::UnloadPackages(
							{Package},
							UnloadError,
							true))
					{
						SetError(
							OutError,
							TEXT("skin_weight_import_unload_failed"),
							TargetObjectPath,
							UnloadError.ToString());
						return false;
					}
				}
				CollectGarbage(RF_NoFlags, true);
				USkeletalMesh* const Reloaded = LoadObject<USkeletalMesh>(
					nullptr,
					*TargetObjectPath);
				if (!Reloaded
					|| !HasProfile(*Reloaded, FName(*Spec.ProfileName)))
				{
					SetError(
						OutError,
						TEXT("skin_weight_import_reload_failed"),
						TargetObjectPath);
					return false;
				}
				FMetaData& Metadata = Reloaded->GetOutermost()->GetMetaData();
				const FString ProfileKey = MetadataKey(Spec.ProfileName, TEXT("Profile"));
				const FString SourceKey = MetadataKey(Spec.ProfileName, TEXT("Source"));
				const FString HashKey = MetadataKey(Spec.ProfileName, TEXT("SourceSha256"));
				const FString LodKey = MetadataKey(Spec.ProfileName, TEXT("LodIndex"));
				OutReadback.TargetIdentity = Reloaded->GetPathName();
				OutReadback.AssetClass = Reloaded->GetClass()->GetPathName();
				OutReadback.ProfileName = Metadata.GetValue(Reloaded, *ProfileKey);
				OutReadback.SourceRef = Metadata.GetValue(Reloaded, *SourceKey);
				OutReadback.SourceSha256 = Metadata.GetValue(Reloaded, *HashKey);
				OutReadback.LodIndex = FCString::Atoi(
					*Metadata.GetValue(Reloaded, *LodKey));
				OutReadback.ProfileCount = Reloaded->GetSkinWeightProfiles().Num();
				OutReadback.bReloaded = true;
				const bool bEquivalent =
					OutReadback.ProfileName == Spec.ProfileName
					&& FPaths::IsSamePath(OutReadback.SourceRef, Source)
					&& OutReadback.SourceSha256 == SourceSha256
					&& OutReadback.LodIndex == Spec.LodIndex;
				if (!bEquivalent)
				{
					SetError(
						OutError,
						TEXT("skin_weight_import_readback_mismatch"),
						TargetObjectPath);
					return false;
				}
				Mesh = Reloaded;
				DeleteBackup();
				return true;
			}

			virtual bool Rollback(
				bool& bOutConfirmed,
				FSkinWeightImportError& OutError) override
			{
				bOutConfirmed = RestoreBackup();
				if (!bOutConfirmed)
				{
					SetError(
						OutError,
						TEXT("skin_weight_import_rollback_unconfirmed"),
						TargetObjectPath,
						BackupFilename);
				}
				return bOutConfirmed;
			}

		private:
			bool RestoreBackup()
			{
				if (BackupFilename.IsEmpty()
					|| !IFileManager::Get().FileExists(*BackupFilename))
				{
					return false;
				}
				if (Mesh && Mesh->GetOutermost())
				{
					UPackage* const Package = Mesh->GetOutermost();
					Package->SetDirtyFlag(false);
					Mesh = nullptr;
					FText Error;
					if (!UPackageTools::UnloadPackages({Package}, Error, true))
					{
						return false;
					}
				}
				CollectGarbage(RF_NoFlags, true);
				if (IFileManager::Get().Copy(
						*PackageFilename,
						*BackupFilename,
						true,
						true)
					!= COPY_OK)
				{
					return false;
				}
				DeleteBackup();
				Mesh = LoadObject<USkeletalMesh>(nullptr, *TargetObjectPath);
				return Mesh
					&& HashFileSha256(PackageFilename) == ExpectedRevision
					&& !HasProfile(*Mesh, FName(*Spec.ProfileName));
			}

			void DeleteBackup()
			{
				if (!BackupFilename.IsEmpty()
					&& IFileManager::Get().FileExists(*BackupFilename))
				{
					IFileManager::Get().Delete(
						*BackupFilename,
						false,
						true,
						true);
				}
				BackupFilename.Reset();
			}

			FSkinWeightImportSpec Spec;
			FString Source;
			FString SourceSha256;
			FString PackageName;
			FString TargetObjectPath;
			FString PackageFilename;
			FString ExpectedRevision;
			FString BackupFilename;
			USkeletalMesh* Mesh = nullptr;
			bool bMutationStarted = false;
		};

		class FProvider final : public ISkinWeightImportProvider
		{
		public:
			virtual bool Prepare(
				const FSkinWeightImportSpec& InSpec,
				TSharedPtr<ISkinWeightImportSession>& OutSession,
				FSkinWeightImportError& OutError) override
			{
				OutSession.Reset();
				OutError = {};
				if (!IsInGameThread())
				{
					SetError(
						OutError,
						TEXT("skin_weight_import_game_thread_required"));
					return false;
				}
				FSkinWeightImportSpec Spec = InSpec;
				Spec.ProfileName.TrimStartAndEndInline();
				if (!Spec.IsValid() || FName(*Spec.ProfileName).IsNone())
				{
					SetError(
						OutError,
						TEXT("skin_weight_import_spec_invalid"));
					return false;
				}
				FString Source;
				if (!TryNormalizeSource(Spec.SourceRef, Source, OutError))
				{
					return false;
				}
				const FString SourceSha256 = HashFileSha256(Source);
				if (SourceSha256.IsEmpty())
				{
					SetError(
						OutError,
						TEXT("skin_weight_import_source_hash_failed"),
						Source);
					return false;
				}
				FString PackageName;
				if (!TryPackageName(Spec.TargetRef, PackageName))
				{
					SetError(
						OutError,
						TEXT("skin_weight_import_target_invalid"),
						Spec.TargetRef);
					return false;
				}
				FString PackageFilename;
				if (!FPackageName::DoesPackageExist(
						PackageName,
						&PackageFilename))
				{
					SetError(
						OutError,
						TEXT("skin_weight_import_target_missing"),
						ObjectPath(PackageName));
					return false;
				}
				USkeletalMesh* const Mesh = LoadObject<USkeletalMesh>(
					nullptr,
					*ObjectPath(PackageName));
				if (!Mesh)
				{
					SetError(
						OutError,
						TEXT("skin_weight_import_target_type_mismatch"),
						ObjectPath(PackageName));
					return false;
				}
				if (Mesh->GetOutermost()->IsDirty())
				{
					SetError(
						OutError,
						TEXT("skin_weight_import_dirty_package"),
						ObjectPath(PackageName));
					return false;
				}
				if (Spec.LodIndex < 0 || Spec.LodIndex >= Mesh->GetLODNum())
				{
					SetError(
						OutError,
						TEXT("skin_weight_import_lod_invalid"),
						FString::FromInt(Spec.LodIndex));
					return false;
				}
				if (HasProfile(*Mesh, FName(*Spec.ProfileName)))
				{
					SetError(
						OutError,
						TEXT("skin_weight_import_profile_conflict"),
						Spec.ProfileName);
					return false;
				}
				const FString ExpectedRevision = HashFileSha256(PackageFilename);
				const FString BackupRoot = FPaths::ProjectSavedDir()
					/ TEXT("UAI/SkinWeightImportRollback");
				IFileManager::Get().MakeDirectory(*BackupRoot, true);
				const FString BackupFilename = BackupRoot
					/ (FGuid::NewGuid().ToString(EGuidFormats::Digits)
						+ FPackageName::GetAssetPackageExtension());
				if (ExpectedRevision.IsEmpty()
					|| IFileManager::Get().Copy(
							*BackupFilename,
							*PackageFilename,
							true,
							true)
						!= COPY_OK)
				{
					SetError(
						OutError,
						TEXT("skin_weight_import_backup_failed"),
						PackageFilename,
						BackupFilename);
					return false;
				}
				OutSession = MakeShared<FSession>(
					MoveTemp(Spec),
					MoveTemp(Source),
					SourceSha256,
					MoveTemp(PackageName),
					MoveTemp(PackageFilename),
					ExpectedRevision,
					BackupFilename,
					Mesh);
				return true;
			}
		};
	}

	bool FSkinWeightImportSpec::IsValid() const
	{
		return !RequestId.IsEmpty()
			&& !SourceRef.IsEmpty()
			&& !TargetRef.IsEmpty()
			&& !ProfileName.IsEmpty()
			&& LodIndex >= 0;
	}

	bool TryCreateSkinWeightImportProvider(
		TSharedPtr<ISkinWeightImportProvider>& OutProvider,
		FString& OutError)
	{
		OutProvider = MakeShared<FProvider>();
		OutError.Reset();
		return true;
	}
}

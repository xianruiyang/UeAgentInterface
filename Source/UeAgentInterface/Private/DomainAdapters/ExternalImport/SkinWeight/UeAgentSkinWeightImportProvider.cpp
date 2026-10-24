#include "DomainAdapters/ExternalImport/SkinWeight/UeAgentSkinWeightImportProvider.h"
#include "Dom/JsonValue.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

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
#include "Core/AssetDurable/UeAgentPackageSave.h"
#include "UObject/UObjectGlobals.h"

#include <openssl/sha.h>

namespace UE::UAI::DomainAdapters::ExternalImport::SkinWeight
{
	namespace
	{
		Contract::FDiagnostic NativeValueFailure(const TCHAR* Code, const FString& Reason,
			const FString& Target, const FString& Field, TSharedPtr<FJsonValue> Actual,
			TSharedPtr<FJsonValue> Expected, const FString& Detail = FString())
		{
			auto Diagnostic = Contract::FDiagnostic::Failure(Code, Reason, TEXT("external_import"), Detail);
			Diagnostic.SourceKind = TEXT("native_result");
			Diagnostic.TargetPath = Target;
			Diagnostic.PropertyPath = Field;
			Diagnostic.ActualValuePresent = Actual.IsValid();
			Diagnostic.ExpectedValuePresent = Expected.IsValid();
			Diagnostic.ActualValue = MoveTemp(Actual);
			Diagnostic.ExpectedValue = MoveTemp(Expected);
			return Diagnostic;
		}

		Contract::FDiagnostic NativeConditionFailure(const TCHAR* Code, const FString& Reason,
			const FString& Target, const FString& Field, const bool Actual, const bool Expected,
			const FString& Detail = FString())
		{
			return NativeValueFailure(Code, Reason, Target, Field, MakeShared<FJsonValueBoolean>(Actual),
				MakeShared<FJsonValueBoolean>(Expected), Detail);
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

		bool TryHashBytes(const uint8* Data, const int64 Size, FString& OutHash,
			FSkinWeightImportError& OutError, const FString& Target = FString())
		{
			OutHash.Reset();
			FSHA256Signature Signature{};
			const uint8 Empty = 0;
			if (!SHA256(Size > 0 ? Data : &Empty, static_cast<size_t>(Size), Signature.Signature))
			{
				OutError = NativeConditionFailure(TEXT("skin_weight_import_sha256_failed"),
					TEXT("OpenSSL could not calculate the SHA256 value."), Target, TEXT("sha256_calculated"), false, true);
				return false;
			}
			OutHash = TEXT("sha256:") + Signature.ToString().ToLower();
			return true;
		}

		bool TryHashFileSha256(const FString& Filename, FString& OutHash,
			FSkinWeightImportError& OutError, const FString& Target = FString())
		{
			TArray<uint8> Bytes;
			FString PublicFilename = Filename;
			FPaths::MakePathRelativeTo(PublicFilename, *FPaths::ProjectDir());
			if (!FFileHelper::LoadFileToArray(Bytes, *Filename))
			{
				OutHash.Reset();
				OutError = NativeConditionFailure(TEXT("skin_weight_import_file_read_failed"),
					TEXT("The import file could not be read for SHA256 calculation."), Target, TEXT("file_read_succeeded"), false, true);
				OutError.Source.SourceFile = PublicFilename;
				OutError.InternalContext = Filename;
				return false;
			}
			if (!TryHashBytes(Bytes.GetData(), Bytes.Num(), OutHash, OutError, Target))
			{
				OutError.Source.SourceFile = PublicFilename;
				OutError.InternalContext = Filename;
				return false;
			}
			return true;
		}

		bool TryHashUtf8Sha256(const FString& Value, FString& OutHash,
			FSkinWeightImportError& OutError, const FString& Target)
		{
			const FTCHARToUTF8 Utf8(*Value);
			return TryHashBytes(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length(), OutHash, OutError, Target);
		}

		bool TryNormalizeSource(
			FString SourceRef,
			FString& OutSource,
			FSkinWeightImportError& OutError)
		{
			SourceRef.TrimStartAndEndInline();
			if (SourceRef.IsEmpty())
			{
				OutError = NativeValueFailure(TEXT("skin_weight_import_source_required"), TEXT("A skin-weight source file is required."),
					FString(), TEXT("source_ref"), MakeShared<FJsonValueString>(SourceRef), MakeShared<FJsonValueString>(TEXT("nonempty FBX source file path")));
				OutError.PropertyPath = TEXT("source_ref");
				OutError.PublicMessage = TEXT("A skin weight source file is required.");
				return false;
			}
			OutSource = FPaths::IsRelative(SourceRef)
				? FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), SourceRef)
				: FPaths::ConvertRelativePathToFull(SourceRef);
			FPaths::NormalizeFilename(OutSource);
			FString PublicSource = OutSource;
			FPaths::MakePathRelativeTo(PublicSource, *FPaths::ProjectDir());
			if (!FPaths::FileExists(OutSource))
			{
				OutError = NativeValueFailure(TEXT("skin_weight_import_source_not_found"), TEXT("The skin-weight source file does not exist."),
					FString(), TEXT("source_file_exists"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.Source.SourceFile = PublicSource;
				OutError.PropertyPath = TEXT("source_ref");
				OutError.PublicMessage = TEXT("The skin weight source file does not exist.");
				return false;
			}
			if (!FPaths::GetExtension(OutSource).Equals(
					TEXT("fbx"),
					ESearchCase::IgnoreCase))
			{
				OutError = NativeValueFailure(TEXT("skin_weight_import_source_extension_invalid"), TEXT("Skin-weight import requires an FBX source file."),
					FString(), TEXT("source_extension"), MakeShared<FJsonValueString>(FPaths::GetExtension(OutSource)), MakeShared<FJsonValueString>(TEXT("fbx")), TEXT("expected .fbx"));
				OutError.Source.SourceFile = PublicSource;
				OutError.PropertyPath = TEXT("source_ref");
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("fbx"));
				OutError.ActualValue = MakeShared<FJsonValueString>(FPaths::GetExtension(OutSource));
				OutError.PublicMessage = TEXT("Skin weight import requires an FBX source file.");
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
				USkeletalMesh* InMesh,
				FString InProfileMetadataPrefix)
				: Spec(MoveTemp(InSpec))
				, Source(MoveTemp(InSource))
				, SourceSha256(MoveTemp(InSourceSha256))
				, PackageName(MoveTemp(InPackageName))
				, TargetObjectPath(ObjectPath(PackageName))
				, PackageFilename(MoveTemp(InPackageFilename))
				, ExpectedRevision(MoveTemp(InExpectedRevision))
				, BackupFilename(MoveTemp(InBackupFilename))
				, ProfileMetadataPrefix(MoveTemp(InProfileMetadataPrefix))
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
				if (!Mesh || !FPackageName::DoesPackageExist(PackageName))
				{
					OutError = NativeConditionFailure(TEXT("skin_weight_import_version_conflict"),
						!Mesh ? TEXT("The skin-weight native mesh is unavailable before execution.") : TEXT("The skin-weight target package is no longer persisted."),
						TargetObjectPath, !Mesh ? TEXT("native_asset_available") : TEXT("persisted_package_available"), false, true);
					return false;
				}
				FString CurrentRevision;
				if (!TryHashFileSha256(PackageFilename, CurrentRevision, OutError, TargetObjectPath)) return false;
				if (CurrentRevision != ExpectedRevision)
				{
					OutError = NativeConditionFailure(TEXT("skin_weight_import_version_conflict"),
						TEXT("The persisted skin-weight target changed before execution."), TargetObjectPath, TEXT("package_revision_matches"), false, true);
					OutError.ActualEvidence = MakeShared<FJsonValueString>(CurrentRevision);
					OutError.ExpectedEvidence = MakeShared<FJsonValueString>(ExpectedRevision);
					return false;
				}
				if (HasProfile(*Mesh, FName(*Spec.ProfileName)))
				{
					OutError = NativeValueFailure(TEXT("skin_weight_import_profile_conflict"), TEXT("A skin-weight profile with this name already exists."),
					Spec.TargetRef, TEXT("profile_name"), MakeShared<FJsonValueString>(Spec.ProfileName), MakeShared<FJsonValueString>(TEXT("profile name not already present on the target")));
					return false;
				}
				return true;
			}

			virtual bool Execute(FSkinWeightImportError& OutError) override
			{
				if (!Mesh)
				{
					OutError = NativeValueFailure(TEXT("skin_weight_import_target_missing"), TEXT("The skin-weight target package or native mesh is unavailable."),
					TargetObjectPath, TEXT("native_target_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.TargetPath = TargetObjectPath;
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
					OutError = NativeValueFailure(TEXT("skin_weight_import_native_failed"), FString::Printf(TEXT("Native skin-weight import failed for profile %s at LOD %d."), *Spec.ProfileName, Spec.LodIndex),
					TargetObjectPath, TEXT("native_import_succeeded"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Source);
					FString PublicSource = Source;
					FPaths::MakePathRelativeTo(PublicSource, *FPaths::ProjectDir());
					OutError.Source.SourceFile = PublicSource;
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
					OutError = NativeValueFailure(TEXT("skin_weight_import_settle_profile_missing"), !Mesh ? TEXT("The imported native mesh is unavailable during settle.") : TEXT("The imported skin-weight profile is missing from the native mesh."),
					TargetObjectPath, !Mesh ? TEXT("native_asset_available") : TEXT("requested_profile_present"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					if (Mesh) OutError.CandidateName = Spec.ProfileName;
					return false;
				}
				Mesh->PostEditChange();
				UPackage* const Package = Mesh->GetOutermost();
				if (!Package || Package->GetName() != PackageName)
				{
					OutError = NativeValueFailure(TEXT("skin_weight_import_settle_target_invalid"), TEXT("The imported native mesh is not in the expected target package."),
					TargetObjectPath, TEXT("native_package"), Package ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Package->GetName())) : nullptr, MakeShared<FJsonValueString>(PackageName));
				OutError.TargetPath = TargetObjectPath;
					return false;
				}
				FMetaData& Metadata = Package->GetMetaData();
				const FString ProfileKey = ProfileMetadataPrefix + TEXT("Profile");
				const FString SourceKey = ProfileMetadataPrefix + TEXT("Source");
				const FString HashKey = ProfileMetadataPrefix + TEXT("SourceSha256");
				const FString LodKey = ProfileMetadataPrefix + TEXT("LodIndex");
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
					OutError = NativeValueFailure(TEXT("skin_weight_import_pre_persist_fence_failed"), !Mesh ? TEXT("The skin-weight mesh is unavailable before persistence.") : !Mesh->GetOutermost() ? TEXT("The skin-weight mesh has no package before persistence.") : !Mesh->GetOutermost()->IsDirty() ? TEXT("The skin-weight package is not dirty before persistence.") : TEXT("The requested skin-weight profile is missing before persistence."),
					TargetObjectPath, !Mesh ? TEXT("native_asset_available") : !Mesh->GetOutermost() ? TEXT("native_package_available") : !Mesh->GetOutermost()->IsDirty() ? TEXT("native_package_dirty") : TEXT("requested_profile_present"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					OutError.CandidateName = Spec.ProfileName;
				OutError.TargetPath = TargetObjectPath;
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
					OutError = NativeValueFailure(TEXT("skin_weight_import_persist_target_missing"), TEXT("The skin-weight mesh or package is unavailable for persistence."),
					TargetObjectPath, !Mesh ? TEXT("native_asset_available") : TEXT("native_package_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.TargetPath = TargetObjectPath;
					return false;
				}
				FSavePackageArgs Args;
				Args.TopLevelFlags = RF_Public | RF_Standalone;
				UE::UAI::Contract::FDiagnostic SaveDetail;
				if (!UE::UAI::AssetDurable::TrySaveLoadedPackage(
						*Mesh->GetOutermost(),
						Mesh,
						PackageFilename,
						Args, SaveDetail))
				{
					OutError = SaveDetail;
					return false;
				}
				if (!TryHashFileSha256(PackageFilename, OutPersistenceReceiptId, OutError, TargetObjectPath))
				{
					OutError = Contract::FDiagnostic::WrapFailure(TEXT("skin_weight_import_persistence_receipt_missing"), OutError);
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
						OutError = NativeValueFailure(TEXT("skin_weight_import_unload_failed"), UnloadError.IsEmpty() ? TEXT("The persisted skin-weight package could not be unloaded.") : UnloadError.ToString(),
					TargetObjectPath, TEXT("native_package_unloaded"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), UnloadError.ToString());
						if (!UnloadError.IsEmpty()) OutError.PublicMessage = UnloadError.ToString();
						OutError.InternalContext.Reset();
						OutError.TargetPath = TargetObjectPath;
						return false;
					}
				}
				CollectGarbage(RF_NoFlags, true);
				USkeletalMesh* const Reloaded = LoadObject<USkeletalMesh>(
					nullptr,
					*TargetObjectPath);
				if (!Reloaded)
				{
					OutError = NativeValueFailure(TEXT("skin_weight_import_reload_failed"), TEXT("The persisted skin-weight target could not be reloaded as a SkeletalMesh."),
					TargetObjectPath, TEXT("native_asset_class"), nullptr, MakeShared<FJsonValueString>(TEXT("SkeletalMesh")));
				OutError.TargetPath = TargetObjectPath;
					return false;
				}
				FMetaData& Metadata = Reloaded->GetOutermost()->GetMetaData();
				const FString ProfileKey = ProfileMetadataPrefix + TEXT("Profile");
				const FString SourceKey = ProfileMetadataPrefix + TEXT("Source");
				const FString HashKey = ProfileMetadataPrefix + TEXT("SourceSha256");
				const FString LodKey = ProfileMetadataPrefix + TEXT("LodIndex");
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
					HasProfile(*Reloaded, FName(*Spec.ProfileName))
					&& OutReadback.ProfileName == Spec.ProfileName
					&& FPaths::IsSamePath(OutReadback.SourceRef, Source)
					&& OutReadback.SourceSha256 == SourceSha256
					&& OutReadback.LodIndex == Spec.LodIndex;
				if (!bEquivalent)
				{
					if (!HasProfile(*Reloaded, FName(*Spec.ProfileName)))
					{
						OutError = NativeConditionFailure(TEXT("skin_weight_import_readback_mismatch"),
							TEXT("The requested skin-weight profile is missing from the reloaded mesh."), TargetObjectPath, TEXT("requested_profile_present"), false, true);
						OutError.CandidateName = Spec.ProfileName;
					}
					else if (OutReadback.ProfileName != Spec.ProfileName)
						OutError = NativeValueFailure(TEXT("skin_weight_import_readback_mismatch"),
							TEXT("The reloaded skin-weight profile metadata has a different name."), TargetObjectPath, TEXT("profile_name"),
							MakeShared<FJsonValueString>(OutReadback.ProfileName), MakeShared<FJsonValueString>(Spec.ProfileName));
					else if (!FPaths::IsSamePath(OutReadback.SourceRef, Source))
					{
						FString ActualSource = OutReadback.SourceRef;
						FString ExpectedSource = Source;
						FPaths::MakePathRelativeTo(ActualSource, *FPaths::ProjectDir());
						FPaths::MakePathRelativeTo(ExpectedSource, *FPaths::ProjectDir());
						OutError = NativeValueFailure(TEXT("skin_weight_import_readback_mismatch"),
							TEXT("The reloaded skin-weight metadata has a different source file."), TargetObjectPath, TEXT("source_ref"),
							MakeShared<FJsonValueString>(ActualSource), MakeShared<FJsonValueString>(ExpectedSource));
					}
					else if (OutReadback.SourceSha256 != SourceSha256)
					{
						OutError = NativeConditionFailure(TEXT("skin_weight_import_readback_mismatch"),
							TEXT("The reloaded skin-weight source fingerprint differs from the imported source."), TargetObjectPath, TEXT("source_fingerprint_matches"), false, true);
						OutError.ActualEvidence = MakeShared<FJsonValueString>(OutReadback.SourceSha256);
						OutError.ExpectedEvidence = MakeShared<FJsonValueString>(SourceSha256);
					}
					else
						OutError = NativeValueFailure(TEXT("skin_weight_import_readback_mismatch"),
							TEXT("The reloaded skin-weight metadata has a different LOD index."), TargetObjectPath, TEXT("lod_index"),
							MakeShared<FJsonValueNumber>(OutReadback.LodIndex), MakeShared<FJsonValueNumber>(Spec.LodIndex));
					UeAgentAuthoringDiagnostics::ReportConsistencyWarning(OutError.Code, OutError, TEXT("skin_weight_import_observation"));
					OutError = {};
				}
				Mesh = Reloaded;
				DeleteBackup();
				return true;
			}

			virtual bool Rollback(
				bool& bOutConfirmed,
				FSkinWeightImportError& OutError) override
			{
				OutError.Reset();
				bOutConfirmed = RestoreBackup(OutError);
				if (!bOutConfirmed && !OutError.IsEmpty())
					OutError = Contract::FDiagnostic::WrapFailure(TEXT("skin_weight_import_rollback_unconfirmed"), OutError);
				return bOutConfirmed;
			}

		private:
			bool RestoreBackup(FSkinWeightImportError& OutError)
			{
				if (BackupFilename.IsEmpty() || !IFileManager::Get().FileExists(*BackupFilename))
				{
					OutError = NativeConditionFailure(TEXT("skin_weight_import_rollback_backup_missing"),
						TEXT("The skin-weight rollback backup is unavailable."), TargetObjectPath, TEXT("rollback_backup_available"), false, true, BackupFilename);
					return false;
				}
				if (Mesh && Mesh->GetOutermost())
				{
					UPackage* const Package = Mesh->GetOutermost();
					Package->SetDirtyFlag(false);
					Mesh = nullptr;
					FText NativeError;
					if (!UPackageTools::UnloadPackages({Package}, NativeError, true))
					{
						OutError = NativeConditionFailure(TEXT("skin_weight_import_rollback_unload_failed"),
							NativeError.IsEmpty() ? TEXT("The skin-weight target package could not be unloaded for rollback.") : NativeError.ToString(),
							TargetObjectPath, TEXT("native_package_unloaded"), false, true);
						return false;
					}
				}
				CollectGarbage(RF_NoFlags, true);
				const int32 CopyResult = IFileManager::Get().Copy(*PackageFilename, *BackupFilename, true, true);
				if (CopyResult != COPY_OK)
				{
					OutError = NativeValueFailure(TEXT("skin_weight_import_rollback_copy_failed"),
						TEXT("The skin-weight rollback backup could not be copied back to the target package."), TargetObjectPath, TEXT("native_copy_result"),
						MakeShared<FJsonValueNumber>(CopyResult), MakeShared<FJsonValueNumber>(COPY_OK), BackupFilename);
					return false;
				}
				DeleteBackup();
				Mesh = LoadObject<USkeletalMesh>(nullptr, *TargetObjectPath);
				if (!Mesh)
				{
					OutError = NativeConditionFailure(TEXT("skin_weight_import_rollback_reload_failed"),
						TEXT("The restored skin-weight mesh could not be reloaded."), TargetObjectPath, TEXT("native_asset_available"), false, true);
					return false;
				}
				FString CurrentRevision;
				if (!TryHashFileSha256(PackageFilename, CurrentRevision, OutError, TargetObjectPath)) return false;
				if (CurrentRevision != ExpectedRevision)
				{
					OutError = NativeConditionFailure(TEXT("skin_weight_import_rollback_revision_mismatch"),
						TEXT("The restored skin-weight package does not match its pre-import revision."), TargetObjectPath, TEXT("package_revision_matches"), false, true);
					OutError.ActualEvidence = MakeShared<FJsonValueString>(CurrentRevision);
					OutError.ExpectedEvidence = MakeShared<FJsonValueString>(ExpectedRevision);
					return false;
				}
				if (HasProfile(*Mesh, FName(*Spec.ProfileName)))
				{
					OutError = NativeConditionFailure(TEXT("skin_weight_import_rollback_profile_present"),
						TEXT("The imported skin-weight profile is still present after rollback."), TargetObjectPath, TEXT("requested_profile_present"), true, false);
					OutError.CandidateName = Spec.ProfileName;
					return false;
				}
				return true;
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
			FString ProfileMetadataPrefix;
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
					OutError = NativeValueFailure(TEXT("skin_weight_import_game_thread_required"), TEXT("Skin-weight import preparation must run on the game thread."),
					InSpec.TargetRef, TEXT("game_thread"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return false;
				}
				FSkinWeightImportSpec Spec = InSpec;
				Spec.ProfileName.TrimStartAndEndInline();
				if (!Spec.IsValid() || FName(*Spec.ProfileName).IsNone())
				{
					if (Spec.RequestId.IsEmpty())
						OutError = NativeConditionFailure(TEXT("skin_weight_import_spec_invalid"),
							TEXT("The skin-weight specification has no request identity."), Spec.TargetRef, TEXT("request_identity_present"), false, true);
					else if (Spec.SourceRef.IsEmpty() || Spec.TargetRef.IsEmpty())
						OutError = NativeValueFailure(TEXT("skin_weight_import_spec_invalid"),
							Spec.SourceRef.IsEmpty() ? TEXT("The skin-weight specification has no source file.") : TEXT("The skin-weight specification has no target."),
							Spec.TargetRef, Spec.SourceRef.IsEmpty() ? TEXT("source_ref") : TEXT("target_ref"),
							MakeShared<FJsonValueString>(FString()), MakeShared<FJsonValueString>(TEXT("nonempty reference")));
					else if (Spec.ProfileName.IsEmpty() || FName(*Spec.ProfileName).IsNone())
						OutError = NativeValueFailure(TEXT("skin_weight_import_spec_invalid"),
							TEXT("The skin-weight profile name must resolve to a non-None Unreal name."), Spec.TargetRef, TEXT("profile_name"),
							MakeShared<FJsonValueString>(Spec.ProfileName), MakeShared<FJsonValueString>(TEXT("nonempty, non-None profile name")));
					else
						OutError = NativeValueFailure(TEXT("skin_weight_import_spec_invalid"),
							TEXT("The skin-weight LOD index cannot be negative."), Spec.TargetRef, TEXT("lod_index"),
							MakeShared<FJsonValueNumber>(Spec.LodIndex), MakeShared<FJsonValueString>(TEXT("nonnegative integer")));
					return false;
				}
				FString ProfileHash;
				if (!TryHashUtf8Sha256(Spec.ProfileName, ProfileHash, OutError, Spec.TargetRef))
				{
					OutError = Contract::FDiagnostic::WrapFailure(TEXT("skin_weight_import_profile_key_hash_failed"), OutError);
					OutError.CandidateName = Spec.ProfileName;
					return false;
				}
				const FString ProfileMetadataPrefix = TEXT("UAI.SkinWeightImport.") + ProfileHash.Mid(7, 16) + TEXT(".");
				FString Source;
				if (!TryNormalizeSource(Spec.SourceRef, Source, OutError))
				{
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Spec.TargetRef;
					return false;
				}
				FString SourceSha256;
				if (!TryHashFileSha256(Source, SourceSha256, OutError, Spec.TargetRef))
				{
					OutError = Contract::FDiagnostic::WrapFailure(TEXT("skin_weight_import_source_hash_failed"), OutError);
					return false;
				}
				FString PackageName;
				if (!TryPackageName(Spec.TargetRef, PackageName))
				{
					OutError = NativeValueFailure(TEXT("skin_weight_import_target_invalid"), TEXT("The skin-weight target must be a valid package or object reference under /Game/."),
					Spec.TargetRef, TEXT("target_ref"), MakeShared<FJsonValueString>(Spec.TargetRef), MakeShared<FJsonValueString>(TEXT("valid /Game/ package or object reference")));
					return false;
				}
				FString PackageFilename;
				if (!FPackageName::DoesPackageExist(
						PackageName,
						&PackageFilename))
				{
					OutError = NativeValueFailure(TEXT("skin_weight_import_target_missing"), TEXT("The skin-weight target package or native mesh is unavailable."),
					ObjectPath(PackageName), TEXT("native_target_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return false;
				}
				USkeletalMesh* const Mesh = LoadObject<USkeletalMesh>(
					nullptr,
					*ObjectPath(PackageName));
				if (!Mesh)
				{
					OutError = NativeValueFailure(TEXT("skin_weight_import_target_type_mismatch"), TEXT("The skin-weight target could not be loaded as a SkeletalMesh."),
					ObjectPath(PackageName), TEXT("native_asset_class"), nullptr, MakeShared<FJsonValueString>(TEXT("SkeletalMesh")));
					OutError.TargetPath = ObjectPath(PackageName);
					OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("SkeletalMesh"));
					return false;
				}
				if (Mesh->GetOutermost()->IsDirty())
				{
					OutError = NativeValueFailure(TEXT("skin_weight_import_dirty_package"), TEXT("The skin-weight target package has unsaved changes."),
					ObjectPath(PackageName), TEXT("native_package_dirty"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
					return false;
				}
				if (Spec.LodIndex < 0 || Spec.LodIndex >= Mesh->GetLODNum())
				{
					OutError = NativeValueFailure(TEXT("skin_weight_import_lod_invalid"), TEXT("The requested skin-weight LOD index is outside the native mesh LOD range."),
					ObjectPath(PackageName), TEXT("lod_index"), MakeShared<FJsonValueNumber>(Spec.LodIndex), MakeShared<FJsonValueString>(FString::Printf(TEXT("0 <= lod_index < %d"), Mesh->GetLODNum())));
					OutError.TargetPath = ObjectPath(PackageName);
					OutError.PropertyPath = TEXT("lod_index");
					OutError.ExpectedValue = MakeShared<FJsonValueString>(FString::Printf(TEXT("0 <= lod_index < %d"), Mesh->GetLODNum()));
					OutError.ActualValue = MakeShared<FJsonValueNumber>(Spec.LodIndex);
					return false;
				}
				if (HasProfile(*Mesh, FName(*Spec.ProfileName)))
				{
					OutError = NativeValueFailure(TEXT("skin_weight_import_profile_conflict"), TEXT("A skin-weight profile with this name already exists."),
					Spec.TargetRef, TEXT("profile_name"), MakeShared<FJsonValueString>(Spec.ProfileName), MakeShared<FJsonValueString>(TEXT("profile name not already present on the target")));
					OutError.TargetPath = ObjectPath(PackageName);
					OutError.PropertyPath = TEXT("profile_name");
					OutError.ActualValue = MakeShared<FJsonValueString>(Spec.ProfileName);
					OutError.PublicMessage = TEXT("A skin-weight profile with this name already exists.");
					return false;
				}
				FString ExpectedRevision;
				if (!TryHashFileSha256(PackageFilename, ExpectedRevision, OutError, ObjectPath(PackageName)))
				{
					OutError = Contract::FDiagnostic::WrapFailure(TEXT("skin_weight_import_backup_failed"), OutError);
					return false;
				}
				const FString BackupRoot = FPaths::ProjectSavedDir()
					/ TEXT("UAI/SkinWeightImportRollback");
				IFileManager::Get().MakeDirectory(*BackupRoot, true);
				const FString BackupFilename = BackupRoot
					/ (FGuid::NewGuid().ToString(EGuidFormats::Digits)
						+ FPackageName::GetAssetPackageExtension());
				const int32 CopyResult = IFileManager::Get().Copy(*BackupFilename, *PackageFilename, true, true);
				if (CopyResult != COPY_OK)
				{
					OutError = NativeValueFailure(TEXT("skin_weight_import_backup_failed"),
						TEXT("The skin-weight target package could not be copied to its rollback backup."), ObjectPath(PackageName), TEXT("native_copy_result"),
						MakeShared<FJsonValueNumber>(CopyResult), MakeShared<FJsonValueNumber>(COPY_OK), BackupFilename);
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
					Mesh, ProfileMetadataPrefix);
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutProvider = MakeShared<FProvider>();
		OutError.Reset();
		return true;
	}
}

#include "DomainAdapters/ExternalImport/Localization/UeAgentLocalizationImportProvider.h"

#include "Commandlets/CommandletHelpers.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Internationalization/Internationalization.h"
#include "Internationalization/TextLocalizationManager.h"
#include "LocalizationConfigurationScript.h"
#include "LocalizationSettings.h"
#include "LocalizationTargetTypes.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "UnrealEdMisc.h"

#include <openssl/sha.h>

namespace UE::UAI::DomainAdapters::ExternalImport::Localization
{
	namespace
	{
		constexpr int64 MaxOwnedFileBytes = 64LL * 1024LL * 1024LL;
		constexpr double CommandletTimeoutSeconds = 300.0;
		constexpr int32 MaxCommandletDetailCharacters = 8192;

		void SetError(
			FLocalizationImportError& OutError,
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString())
		{
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
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

		FString HashUtf8(const FString& Value)
		{
			const FTCHARToUTF8 Utf8(*Value);
			return HashBytes(
				reinterpret_cast<const uint8*>(Utf8.Get()),
				Utf8.Length());
		}

		FString HashFile(const FString& Filename)
		{
			TArray<uint8> Bytes;
			return FFileHelper::LoadFileToArray(Bytes, *Filename)
				? HashBytes(Bytes.GetData(), Bytes.Num())
				: FString();
		}

		bool IsSafeSegment(const FString& Value)
		{
			if (Value.IsEmpty())
			{
				return false;
			}
			for (const TCHAR Character : Value)
			{
				if (!FChar::IsAlnum(Character)
					&& Character != TEXT('_')
					&& Character != TEXT('-'))
				{
					return false;
				}
			}
			return true;
		}

		bool TryNormalizeSource(
			FString SourceRef,
			FString& OutSource,
			FLocalizationImportError& OutError)
		{
			SourceRef.TrimStartAndEndInline();
			if (SourceRef.IsEmpty())
			{
				SetError(OutError, TEXT("localization_import_source_required"));
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
					TEXT("localization_import_source_not_found"),
					OutSource);
				return false;
			}
			if (!FPaths::GetExtension(OutSource).Equals(
					TEXT("po"),
					ESearchCase::IgnoreCase))
			{
				SetError(
					OutError,
					TEXT("localization_import_source_extension_invalid"),
					OutSource,
					TEXT("expected .po"));
				return false;
			}
			const int64 Size = IFileManager::Get().FileSize(*OutSource);
			if (Size < 0 || Size > MaxOwnedFileBytes)
			{
				SetError(
					OutError,
					TEXT("localization_import_source_size_invalid"),
					OutSource,
					FString::Printf(TEXT("size=%lld"), Size));
				return false;
			}
			return true;
		}

		bool TryCultureFromPo(
			const FString& Source,
			FString& OutCulture,
			FLocalizationImportError& OutError)
		{
			FString Text;
			if (!FFileHelper::LoadFileToString(Text, *Source))
			{
				SetError(
					OutError,
					TEXT("localization_import_source_read_failed"),
					Source);
				return false;
			}
			TArray<FString> Lines;
			Text.ParseIntoArrayLines(Lines, false);
			for (FString Line : Lines)
			{
				Line.TrimStartAndEndInline();
				Line.RemoveFromStart(TEXT("\""));
				if (!Line.StartsWith(TEXT("Language:")))
				{
					continue;
				}
				OutCulture = Line.Mid(9).TrimStartAndEnd();
				OutCulture.RemoveFromEnd(TEXT("\\n\""));
				OutCulture.RemoveFromEnd(TEXT("\""));
				OutCulture.TrimStartAndEndInline();
				if (IsSafeSegment(OutCulture)
					&& FInternationalization::Get().GetCulture(OutCulture).IsValid())
				{
					return true;
				}
				break;
			}
			SetError(
				OutError,
				TEXT("localization_import_po_culture_invalid"),
				Source);
			return false;
		}

		ULocalizationTarget* FindProjectTarget(const FString& TargetRef)
		{
			ULocalizationTargetSet* const TargetSet =
				ULocalizationSettings::GetGameTargetSet();
			if (!TargetSet)
			{
				return nullptr;
			}
			const TObjectPtr<ULocalizationTarget>* const Match =
				TargetSet->TargetObjects.FindByPredicate(
					[&TargetRef](const ULocalizationTarget* Candidate)
					{
						return Candidate
							&& Candidate->Settings.Name == TargetRef;
					});
			return Match ? Match->Get() : nullptr;
		}

		bool SupportsCulture(
			const ULocalizationTarget& Target,
			const FString& Culture)
		{
			return Target.Settings.SupportedCulturesStatistics.ContainsByPredicate(
				[&Culture](const FCultureStatistics& Candidate)
				{
					return Candidate.CultureName == Culture;
				});
		}

		void AppendBoundedCommandletOutput(
			FString& InOutOutput,
			FString Chunk)
		{
			if (Chunk.IsEmpty())
			{
				return;
			}
			InOutOutput += MoveTemp(Chunk);
			if (InOutOutput.Len() > MaxCommandletDetailCharacters)
			{
				InOutOutput.RightChopInline(
					InOutOutput.Len() - MaxCommandletDetailCharacters);
			}
		}

		struct FOwnedCommandletProcess final
		{
			~FOwnedCommandletProcess()
			{
				if (Handle.IsValid())
				{
					if (FPlatformProcess::IsProcRunning(Handle))
					{
						FPlatformProcess::TerminateProc(Handle, true);
					}
					FPlatformProcess::CloseProc(Handle);
				}
				if (ReadPipe || WritePipe)
				{
					FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
				}
			}

			bool Launch(const FString& ProcessArguments)
			{
				if (!FPlatformProcess::CreatePipe(ReadPipe, WritePipe))
				{
					return false;
				}
				Handle = FPlatformProcess::CreateProc(
					*FUnrealEdMisc::Get().GetExecutableForCommandlets(),
					*ProcessArguments,
					true,
					true,
					true,
					nullptr,
					0,
					nullptr,
					WritePipe);
				return Handle.IsValid();
			}

			void* ReadPipe = nullptr;
			void* WritePipe = nullptr;
			FProcHandle Handle;
		};

		bool RunLocalizationCommandlet(
			const FString& ConfigPath,
			const bool bUseProjectFile,
			FString& OutDetail)
		{
			OutDetail.Reset();
			const FString RelativeConfig =
				LocalizationConfigurationScript::MakePathRelativeForCommandletProcess(
					ConfigPath,
					bUseProjectFile);
			FString AdditionalArguments = FString::Printf(
				TEXT("-config=\"%s\" -unattended -nosplash -NullRHI ")
				TEXT("-LiveCoding=false -NoDDCCleanup ")
				TEXT("-DDC=InstalledNoZenLocalFallback -SkipZenStore"),
				*RelativeConfig);
			if (FLocalizationSourceControlSettings::IsSourceControlEnabled())
			{
				AdditionalArguments += TEXT(" -EnableSCC");
				if (!FLocalizationSourceControlSettings::
						IsSourceControlAutoSubmitEnabled())
				{
					AdditionalArguments += TEXT(" -DisableSCCSubmit");
				}
			}
			else
			{
				AdditionalArguments += TEXT(" -nop4");
			}
			const FString ProjectFile = FString::Printf(
				TEXT("\"%s\""),
				*FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
			const FString ProcessArguments =
				CommandletHelpers::BuildCommandletProcessArguments(
					TEXT("GatherText"),
					bUseProjectFile ? *ProjectFile : nullptr,
					*AdditionalArguments);
			FOwnedCommandletProcess Process;
			if (!Process.Launch(ProcessArguments))
			{
				OutDetail = TEXT("could not launch GatherText commandlet");
				return false;
			}

			const double Deadline =
				FPlatformTime::Seconds() + CommandletTimeoutSeconds;
			while (FPlatformProcess::IsProcRunning(Process.Handle))
			{
				AppendBoundedCommandletOutput(
					OutDetail,
					FPlatformProcess::ReadPipe(Process.ReadPipe));
				if (FPlatformTime::Seconds() >= Deadline)
				{
					FPlatformProcess::TerminateProc(Process.Handle, true);
					AppendBoundedCommandletOutput(
						OutDetail,
						TEXT("\nGatherText commandlet exceeded the 300 second bound"));
					return false;
				}
				FPlatformProcess::Sleep(0.01f);
			}
			AppendBoundedCommandletOutput(
				OutDetail,
				FPlatformProcess::ReadPipe(Process.ReadPipe));
			int32 ReturnCode = INDEX_NONE;
			if (!FPlatformProcess::GetProcReturnCode(
					Process.Handle,
					&ReturnCode))
			{
				AppendBoundedCommandletOutput(
					OutDetail,
					TEXT("\nGatherText commandlet return code was unavailable"));
				return false;
			}
			if (ReturnCode != 0)
			{
				AppendBoundedCommandletOutput(
					OutDetail,
					FString::Printf(
						TEXT("\nGatherText commandlet returned %d"),
						ReturnCode));
				return false;
			}
			return true;
		}

		bool WriteAndRunLocalizationCommandlet(
			FLocalizationConfigurationScript Script,
			const FString& DefaultConfigPath,
			const bool bUseProjectFile,
			FString& OutDetail)
		{
			IFileManager& Files = IFileManager::Get();
			const FString ConfigDirectory = FPaths::GetPath(DefaultConfigPath);
			if (!Files.MakeDirectory(*ConfigDirectory, true)
				&& !Files.DirectoryExists(*ConfigDirectory))
			{
				OutDetail = TEXT("could not create localization config directory");
				return false;
			}
			const FString TemporaryConfig = FPaths::CreateTempFilename(
				*ConfigDirectory,
				*FPaths::GetBaseFilename(DefaultConfigPath),
				*FPaths::GetExtension(DefaultConfigPath, true));
			if (!Script.Write(TemporaryConfig))
			{
				OutDetail = TEXT("could not write temporary localization config");
				return false;
			}
			const bool bSucceeded = RunLocalizationCommandlet(
				TemporaryConfig,
				bUseProjectFile,
				OutDetail);
			const bool bDeleted = Files.Delete(
				*TemporaryConfig,
				false,
				true,
				true);
			if (!bDeleted && FPaths::FileExists(TemporaryConfig))
			{
				AppendBoundedCommandletOutput(
					OutDetail,
					TEXT("\ntemporary localization config cleanup failed"));
				return false;
			}
			return bSucceeded;
		}

		struct FFileSnapshot final
		{
			FString Path;
			TArray<uint8> Bytes;
			FString Revision;
			bool bExisted = false;

			bool Capture(const FString& InPath)
			{
				Path = InPath;
				bExisted = FPaths::FileExists(Path);
				if (!bExisted)
				{
					Bytes.Reset();
					Revision = TEXT("missing");
					return true;
				}
				const int64 Size = IFileManager::Get().FileSize(*Path);
				if (Size < 0 || Size > MaxOwnedFileBytes
					|| !FFileHelper::LoadFileToArray(Bytes, *Path))
				{
					return false;
				}
				Revision = HashBytes(Bytes.GetData(), Bytes.Num());
				return !Revision.IsEmpty();
			}

			bool MatchesCurrent() const
			{
				return bExisted
					? HashFile(Path) == Revision
					: !FPaths::FileExists(Path);
			}

			bool Restore() const
			{
				if (!bExisted)
				{
					return !FPaths::FileExists(Path)
						|| IFileManager::Get().Delete(*Path, false, true, true);
				}
				IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
				return FFileHelper::SaveArrayToFile(Bytes, *Path)
					&& HashFile(Path) == Revision;
			}
		};

		class FSession final : public ILocalizationImportSession
		{
		public:
			FSession(
				FLocalizationImportSpec InSpec,
				FString InSource,
				FString InSourceSha256,
				FString InCulture,
				FString InTargetIdentity,
				FString InExpectedRevision,
				FString InManifestPath,
				FString InManifestRevision,
				FString InPersistedPoPath,
				FString InArchivePath,
				FString InLocResPath,
				TArray<FFileSnapshot> InSnapshots,
				ULocalizationTarget* InTarget)
				: Spec(MoveTemp(InSpec))
				, Source(MoveTemp(InSource))
				, SourceSha256(MoveTemp(InSourceSha256))
				, Culture(MoveTemp(InCulture))
				, TargetIdentity(MoveTemp(InTargetIdentity))
				, ExpectedRevision(MoveTemp(InExpectedRevision))
				, ManifestPath(MoveTemp(InManifestPath))
				, ManifestRevision(MoveTemp(InManifestRevision))
				, PersistedPoPath(MoveTemp(InPersistedPoPath))
				, ArchivePath(MoveTemp(InArchivePath))
				, LocResPath(MoveTemp(InLocResPath))
				, Snapshots(MoveTemp(InSnapshots))
				, Target(InTarget)
			{
				StagingPath = PersistedPoPath + TEXT(".uai-")
					+ FGuid::NewGuid().ToString(EGuidFormats::Digits)
					+ TEXT(".tmp");
			}

			virtual ~FSession() override
			{
				DeleteStaging();
			}

			virtual const FString& GetTargetIdentity() const override
			{
				return TargetIdentity;
			}

			virtual const FString& GetExpectedRevision() const override
			{
				return ExpectedRevision;
			}

			virtual bool CheckPreExecute(
				FLocalizationImportError& OutError) override
			{
				Target = FindProjectTarget(Spec.TargetRef);
				if (!Target || !SupportsCulture(*Target, Culture))
				{
					SetError(
						OutError,
						TEXT("localization_import_version_conflict"),
						TargetIdentity,
						TEXT("target or culture changed before execution"));
					return false;
				}
				if (HashFile(Source) != SourceSha256
					|| HashFile(ManifestPath) != ManifestRevision
					|| Snapshots.ContainsByPredicate(
						[](const FFileSnapshot& Snapshot)
						{
							return !Snapshot.MatchesCurrent();
						}))
				{
					SetError(
						OutError,
						TEXT("localization_import_version_conflict"),
						TargetIdentity);
					return false;
				}
				return true;
			}

			virtual bool Execute(FLocalizationImportError& OutError) override
			{
				IFileManager& Files = IFileManager::Get();
				Files.MakeDirectory(*FPaths::GetPath(PersistedPoPath), true);
				if (Files.Copy(*StagingPath, *Source, true, true) != COPY_OK)
				{
					SetError(
						OutError,
						TEXT("localization_import_stage_failed"),
						Source,
						StagingPath);
					return false;
				}
				if (!Files.Move(
						*PersistedPoPath,
						*StagingPath,
						true,
						true,
						false,
						true))
				{
					DeleteStaging();
					SetError(
						OutError,
						TEXT("localization_import_publish_failed"),
						PersistedPoPath);
					return false;
				}
				bMutationStarted = true;
				return true;
			}

			virtual bool DidMutationStart() const override
			{
				return bMutationStarted;
			}

			virtual bool Settle(FLocalizationImportError& OutError) override
			{
				if (!Target)
				{
					SetError(
						OutError,
						TEXT("localization_import_target_missing"),
						TargetIdentity);
					return false;
				}
				const bool bUseProjectFile =
					!Target->IsMemberOfEngineTargetSet();
				FString CommandletDetail;
				if (!WriteAndRunLocalizationCommandlet(
						LocalizationConfigurationScript::GenerateImportTextConfigFile(
							Target,
							TOptional<FString>(Culture),
							TOptional<FString>(PersistedPoPath)),
						LocalizationConfigurationScript::GetImportTextConfigPath(
							Target,
							TOptional<FString>(Culture)),
						bUseProjectFile,
						CommandletDetail))
				{
					SetError(
						OutError,
						TEXT("localization_import_native_import_failed"),
						TargetIdentity,
						CommandletDetail);
					return false;
				}
				bFormalImporterCompleted = true;
				CommandletDetail.Reset();
				if (!WriteAndRunLocalizationCommandlet(
						LocalizationConfigurationScript::GenerateCompileTextConfigFile(
							Target,
							TOptional<FString>(Culture)),
						LocalizationConfigurationScript::GetCompileTextConfigPath(
							Target,
							TOptional<FString>(Culture)),
						bUseProjectFile,
						CommandletDetail))
				{
					SetError(
						OutError,
						TEXT("localization_import_compile_failed"),
						TargetIdentity,
						CommandletDetail);
					return false;
				}
				bFormalCompilerCompleted = true;
				FTextLocalizationManager& Manager =
					FTextLocalizationManager::Get();
				RefreshRevisionBefore = Manager.GetTextRevision();
				Manager.RefreshResources();
				Manager.WaitForAsyncTasks();
				RefreshRevisionAfter = Manager.GetTextRevision();
				bRefreshCompleted =
					RefreshRevisionAfter != RefreshRevisionBefore;
				if (!bRefreshCompleted)
				{
					SetError(
						OutError,
						TEXT("localization_import_refresh_failed"),
						TargetIdentity);
					return false;
				}
				return true;
			}

			virtual bool CheckPrePersist(
				FLocalizationImportError& OutError) override
			{
				Target = FindProjectTarget(Spec.TargetRef);
				if (!Target
					|| !SupportsCulture(*Target, Culture)
					|| HashFile(PersistedPoPath) != SourceSha256
					|| !FPaths::FileExists(ArchivePath)
					|| !FPaths::FileExists(LocResPath)
					|| !bFormalImporterCompleted
					|| !bFormalCompilerCompleted
					|| !bRefreshCompleted)
				{
					SetError(
						OutError,
						TEXT("localization_import_pre_persist_fence_failed"),
						TargetIdentity);
					return false;
				}
				return true;
			}

			virtual bool Persist(
				FString& OutPersistenceReceiptId,
				FLocalizationImportError& OutError) override
			{
				PersistedPoSha256 = HashFile(PersistedPoPath);
				ArchiveSha256 = HashFile(ArchivePath);
				LocResSha256 = HashFile(LocResPath);
				if (PersistedPoSha256 != SourceSha256
					|| ArchiveSha256.IsEmpty()
					|| LocResSha256.IsEmpty())
				{
					SetError(
						OutError,
						TEXT("localization_import_persist_failed"),
						TargetIdentity);
					return false;
				}
				OutPersistenceReceiptId = HashUtf8(
					TargetIdentity + TEXT("|")
					+ PersistedPoSha256 + TEXT("|")
					+ ArchiveSha256 + TEXT("|")
					+ LocResSha256 + TEXT("|")
					+ FString::FromInt(RefreshRevisionAfter));
				if (OutPersistenceReceiptId.IsEmpty())
				{
					SetError(
						OutError,
						TEXT("localization_import_persistence_receipt_missing"),
						TargetIdentity);
					return false;
				}
				return true;
			}

			virtual bool Verify(
				FLocalizationImportReadback& OutReadback,
				FLocalizationImportError& OutError) override
			{
				FTextLocalizationManager& Manager =
					FTextLocalizationManager::Get();
				Manager.WaitForAsyncTasks();
				Target = FindProjectTarget(Spec.TargetRef);
				OutReadback.TargetRef = Spec.TargetRef;
				OutReadback.TargetIdentity = TargetIdentity;
				OutReadback.Culture = Culture;
				OutReadback.SourceRef = Source;
				OutReadback.SourceSha256 = SourceSha256;
				OutReadback.PersistedPoPath = PersistedPoPath;
				OutReadback.PersistedPoSha256 = HashFile(PersistedPoPath);
				OutReadback.ArchivePath = ArchivePath;
				OutReadback.ArchiveSha256 = HashFile(ArchivePath);
				OutReadback.LocResPath = LocResPath;
				OutReadback.LocResSha256 = HashFile(LocResPath);
				OutReadback.CollapseMode = Target
					? StaticEnum<ELocalizedTextCollapseMode>()->GetNameStringByValue(
						static_cast<int64>(Target->Settings.ExportSettings.CollapseMode))
					: FString();
				OutReadback.PoFormat = Target
					? StaticEnum<EPortableObjectFormat>()->GetNameStringByValue(
						static_cast<int64>(Target->Settings.ExportSettings.POFormat))
					: FString();
				OutReadback.RefreshRevisionBefore = RefreshRevisionBefore;
				OutReadback.RefreshRevisionAfter = Manager.GetTextRevision();
				OutReadback.bTargetResolved = Target
					&& SupportsCulture(*Target, Culture);
				OutReadback.bFormalImporterCompleted =
					bFormalImporterCompleted;
				OutReadback.bFormalCompilerCompleted =
					bFormalCompilerCompleted;
				OutReadback.bRefreshCompleted = bRefreshCompleted
					&& OutReadback.RefreshRevisionAfter
						!= OutReadback.RefreshRevisionBefore;
				const bool bEquivalent = OutReadback.bTargetResolved
					&& OutReadback.PersistedPoSha256 == SourceSha256
					&& OutReadback.ArchiveSha256 == ArchiveSha256
					&& OutReadback.LocResSha256 == LocResSha256
					&& OutReadback.bFormalImporterCompleted
					&& OutReadback.bFormalCompilerCompleted
					&& OutReadback.bRefreshCompleted;
				if (!bEquivalent)
				{
					SetError(
						OutError,
						TEXT("localization_import_readback_mismatch"),
						TargetIdentity);
					return false;
				}
				return true;
			}

			virtual bool Rollback(
				bool& bOutConfirmed,
				FLocalizationImportError& OutError) override
			{
				DeleteStaging();
				bool bRestored = true;
				for (int32 Index = Snapshots.Num() - 1; Index >= 0; --Index)
				{
					bRestored &= Snapshots[Index].Restore();
				}
				Target = FindProjectTarget(Spec.TargetRef);
				if (Target)
				{
					Target->UpdateWordCountsFromCSV();
				}
				FTextLocalizationManager& Manager =
					FTextLocalizationManager::Get();
				Manager.RefreshResources();
				Manager.WaitForAsyncTasks();
				bOutConfirmed = bRestored
					&& Target
					&& SupportsCulture(*Target, Culture)
					&& Snapshots.FindByPredicate(
						[](const FFileSnapshot& Snapshot)
						{
							return !Snapshot.MatchesCurrent();
						}) == nullptr;
				if (!bOutConfirmed)
				{
					SetError(
						OutError,
						TEXT("localization_import_rollback_unconfirmed"),
						TargetIdentity);
				}
				return bOutConfirmed;
			}

		private:
			void DeleteStaging()
			{
				if (!StagingPath.IsEmpty() && FPaths::FileExists(StagingPath))
				{
					IFileManager::Get().Delete(*StagingPath, false, true, true);
				}
			}

			FLocalizationImportSpec Spec;
			FString Source;
			FString SourceSha256;
			FString Culture;
			FString TargetIdentity;
			FString ExpectedRevision;
			FString ManifestPath;
			FString ManifestRevision;
			FString PersistedPoPath;
			FString ArchivePath;
			FString LocResPath;
			FString StagingPath;
			FString PersistedPoSha256;
			FString ArchiveSha256;
			FString LocResSha256;
			TArray<FFileSnapshot> Snapshots;
			ULocalizationTarget* Target = nullptr;
			uint16 RefreshRevisionBefore = 0;
			uint16 RefreshRevisionAfter = 0;
			bool bMutationStarted = false;
			bool bFormalImporterCompleted = false;
			bool bFormalCompilerCompleted = false;
			bool bRefreshCompleted = false;
		};

		class FProvider final : public ILocalizationImportProvider
		{
		public:
			virtual bool Prepare(
				const FLocalizationImportSpec& InSpec,
				TSharedPtr<ILocalizationImportSession>& OutSession,
				FLocalizationImportError& OutError) override
			{
				OutSession.Reset();
				OutError = {};
				if (!IsInGameThread())
				{
					SetError(
						OutError,
						TEXT("localization_import_game_thread_required"));
					return false;
				}
				FLocalizationImportSpec Spec = InSpec;
				Spec.TargetRef.TrimStartAndEndInline();
				if (!Spec.IsValid() || !IsSafeSegment(Spec.TargetRef))
				{
					SetError(
						OutError,
						TEXT("localization_import_spec_invalid"),
						Spec.TargetRef);
					return false;
				}
				FString Source;
				if (!TryNormalizeSource(Spec.SourceRef, Source, OutError))
				{
					return false;
				}
				const FString SourceSha256 = HashFile(Source);
				if (SourceSha256.IsEmpty())
				{
					SetError(
						OutError,
						TEXT("localization_import_source_hash_failed"),
						Source);
					return false;
				}
				FString Culture;
				if (!TryCultureFromPo(Source, Culture, OutError))
				{
					return false;
				}
				ULocalizationTarget* const Target =
					FindProjectTarget(Spec.TargetRef);
				if (!Target)
				{
					SetError(
						OutError,
						TEXT("localization_import_target_missing"),
						Spec.TargetRef);
					return false;
				}
				if (!SupportsCulture(*Target, Culture))
				{
					SetError(
						OutError,
						TEXT("localization_import_culture_unsupported"),
						Spec.TargetRef,
						Culture);
					return false;
				}
				const FString ManifestPath =
					LocalizationConfigurationScript::GetManifestPath(Target);
				const FString ManifestRevision = HashFile(ManifestPath);
				if (ManifestRevision.IsEmpty())
				{
					SetError(
						OutError,
						TEXT("localization_import_manifest_missing"),
						Spec.TargetRef,
						ManifestPath);
					return false;
				}
				const FString PersistedPoPath =
					LocalizationConfigurationScript::GetDefaultPOPath(
						Target,
						Culture);
				const FString ArchivePath =
					LocalizationConfigurationScript::GetArchivePath(Target, Culture);
				const FString LocResPath =
					LocalizationConfigurationScript::GetLocResPath(Target, Culture);
				TArray<FString> MutablePaths = {
					PersistedPoPath,
					ArchivePath,
					LocResPath,
					LocalizationConfigurationScript::GetWordCountCSVPath(Target),
					LocalizationConfigurationScript::GetWordCountReportConfigPath(Target),
				};
				TArray<FFileSnapshot> Snapshots;
				Snapshots.Reserve(MutablePaths.Num());
				for (const FString& Path : MutablePaths)
				{
					FFileSnapshot Snapshot;
					if (!Snapshot.Capture(Path))
					{
						SetError(
							OutError,
							TEXT("localization_import_backup_failed"),
							Path);
						return false;
					}
					Snapshots.Add(MoveTemp(Snapshot));
				}
				FString RevisionInput = ManifestRevision
					+ TEXT("|") + SourceSha256
					+ TEXT("|") + StaticEnum<ELocalizedTextCollapseMode>()
						->GetNameStringByValue(static_cast<int64>(
							Target->Settings.ExportSettings.CollapseMode))
					+ TEXT("|") + StaticEnum<EPortableObjectFormat>()
						->GetNameStringByValue(static_cast<int64>(
							Target->Settings.ExportSettings.POFormat));
				for (const FFileSnapshot& Snapshot : Snapshots)
				{
					RevisionInput += TEXT("|") + Snapshot.Path
						+ TEXT("=") + Snapshot.Revision;
				}
				const FString ExpectedRevision = HashUtf8(RevisionInput);
				if (ExpectedRevision.IsEmpty())
				{
					SetError(
						OutError,
						TEXT("localization_import_revision_failed"),
						Spec.TargetRef);
					return false;
				}
				const FString TargetIdentity =
					TEXT("localization-target://project/")
					+ Spec.TargetRef + TEXT("/") + Culture;
				OutSession = MakeShared<FSession>(
					MoveTemp(Spec),
					MoveTemp(Source),
					SourceSha256,
					MoveTemp(Culture),
					TargetIdentity,
					ExpectedRevision,
					ManifestPath,
					ManifestRevision,
					PersistedPoPath,
					ArchivePath,
					LocResPath,
					MoveTemp(Snapshots),
					Target);
				return true;
			}
		};
	}

	bool FLocalizationImportSpec::IsValid() const
	{
		return !RequestId.IsEmpty()
			&& !SourceRef.IsEmpty()
			&& !TargetRef.IsEmpty();
	}

	bool TryCreateLocalizationImportProvider(
		TSharedPtr<ILocalizationImportProvider>& OutProvider,
		FString& OutError)
	{
		OutProvider = MakeShared<FProvider>();
		OutError.Reset();
		return true;
	}
}

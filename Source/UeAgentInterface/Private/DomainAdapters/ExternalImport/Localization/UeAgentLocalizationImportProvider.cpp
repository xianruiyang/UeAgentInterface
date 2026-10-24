#include "DomainAdapters/ExternalImport/Localization/UeAgentLocalizationImportProvider.h"
#include "Dom/JsonValue.h"

#include "Commandlets/CommandletHelpers.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
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

		Contract::FDiagnostic NativeFileFailure(const TCHAR* Code, const FString& Reason, const FString& Target,
			const FString& Filename, const FString& Field, TSharedPtr<FJsonValue> Actual,
			TSharedPtr<FJsonValue> Expected, const FString& Detail = FString())
		{
			auto Diagnostic = NativeValueFailure(Code, Reason, Target, Field, MoveTemp(Actual), MoveTemp(Expected), Detail);
			FString PublicFilename = Filename;
			FPaths::MakePathRelativeTo(PublicFilename, *FPaths::ProjectDir());
			Diagnostic.Source.SourceFile = PublicFilename;
			return Diagnostic;
		}

		Contract::FDiagnostic RevisionMismatch(const TCHAR* Code, const FString& Reason, const FString& Target,
			const FString& Filename, const FString& Actual, const FString& Expected)
		{
			auto Diagnostic = NativeFileFailure(Code, Reason, Target, Filename, TEXT("file_revision_matches"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(Actual);
			Diagnostic.ExpectedEvidence = MakeShared<FJsonValueString>(Expected);
			return Diagnostic;
		}

		void SetError(FLocalizationImportError& OutError, Contract::FDiagnostic Diagnostic, const FString& Target)
		{
			OutError = MoveTemp(Diagnostic);
			if (OutError.Stage.IsEmpty()) OutError.Stage = TEXT("external_import");
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Target;
		}

		bool TryHashBytes(const uint8* Data, const int64 Size, FString& OutHash,
			FLocalizationImportError& OutError, const FString& Target)
		{
			OutHash.Reset();
			FSHA256Signature Signature{};
			const uint8 Empty = 0;
			if (!SHA256(Size > 0 ? Data : &Empty, static_cast<size_t>(Size), Signature.Signature))
			{
				OutError = NativeConditionFailure(TEXT("localization_import_sha256_failed"),
					TEXT("OpenSSL could not calculate the localization SHA256 value."), Target, TEXT("sha256_calculated"), false, true);
				return false;
			}
			OutHash = TEXT("sha256:") + Signature.ToString().ToLower();
			return true;
		}

		bool TryHashUtf8(const FString& Value, FString& OutHash, FLocalizationImportError& OutError, const FString& Target)
		{
			const FTCHARToUTF8 Utf8(*Value);
			return TryHashBytes(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length(), OutHash, OutError, Target);
		}

		bool TryHashFile(const FString& Filename, FString& OutHash, FLocalizationImportError& OutError, const FString& Target)
		{
			TArray<uint8> Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, *Filename))
			{
				OutHash.Reset();
				OutError = NativeFileFailure(TEXT("localization_import_file_read_failed"),
					TEXT("The localization file could not be read for SHA256 calculation."), Target, Filename,
					TEXT("file_read_succeeded"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Filename);
				return false;
			}
			if (!TryHashBytes(Bytes.GetData(), Bytes.Num(), OutHash, OutError, Target))
			{
				FString PublicFilename = Filename;
				FPaths::MakePathRelativeTo(PublicFilename, *FPaths::ProjectDir());
				OutError.Source.SourceFile = PublicFilename;
				OutError.InternalContext = Filename;
				return false;
			}
			return true;
		}

		bool CheckFileRevision(const FString& Filename, const FString& Expected, FLocalizationImportError& OutError,
			const FString& Target, const TCHAR* Code, const TCHAR* Reason)
		{
			FString Current;
			if (!TryHashFile(Filename, Current, OutError, Target))
			{
				OutError = Contract::FDiagnostic::WrapFailure(Code, OutError);
				return false;
			}
			if (Current == Expected) return true;
			OutError = RevisionMismatch(Code, Reason, Target, Filename, Current, Expected);
			return false;
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
				OutError = NativeValueFailure(TEXT("localization_import_source_required"),
					TEXT("A localization source file is required."), FString(), TEXT("source_ref"),
					MakeShared<FJsonValueString>(SourceRef), MakeShared<FJsonValueString>(TEXT("nonempty PO source file path")));
				OutError.PropertyPath = TEXT("source_ref");
				OutError.PublicMessage = TEXT("A localization source file is required.");
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
				OutError = NativeConditionFailure(TEXT("localization_import_source_not_found"),
					TEXT("The localization source file does not exist."), FString(), TEXT("source_file_exists"), false, true);
				OutError.Source.SourceFile = PublicSource;
				OutError.PublicMessage = TEXT("The localization source file does not exist.");
				return false;
			}
			if (!FPaths::GetExtension(OutSource).Equals(
					TEXT("po"),
					ESearchCase::IgnoreCase))
			{
				OutError = NativeValueFailure(TEXT("localization_import_source_extension_invalid"),
					TEXT("Localization import requires a PO source file."), FString(), TEXT("source_extension"),
					MakeShared<FJsonValueString>(FPaths::GetExtension(OutSource)), MakeShared<FJsonValueString>(TEXT("po")));
				OutError.Source.SourceFile = PublicSource;
				OutError.PropertyPath = TEXT("source_ref");
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("po"));
				OutError.ActualValue = MakeShared<FJsonValueString>(FPaths::GetExtension(OutSource));
				OutError.PublicMessage = TEXT("Localization import requires a PO source file.");
				return false;
			}
			const int64 Size = IFileManager::Get().FileSize(*OutSource);
			if (Size < 0 || Size > MAX_int32)
			{
				OutError = NativeValueFailure(TEXT("localization_import_source_size_invalid"),
					Size < 0 ? TEXT("The localization source file size could not be read.") : TEXT("The localization source exceeds the supported byte count."),
					FString(), TEXT("file_size_bytes"), MakeShared<FJsonValueNumberString>(LexToString(Size)),
					MakeShared<FJsonValueString>(TEXT("0 <= file size <= 2147483647")));
				OutError.Source.SourceFile = PublicSource;
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("readable file no larger than 2147483647 bytes"));
				OutError.ActualValue = MakeShared<FJsonValueNumberString>(LexToString(Size));
				OutError.PublicMessage = Size < 0 ? TEXT("The source file size could not be read.") : TEXT("The localization source exceeds the importer's size limit.");
				return false;
			}
			return true;
		}

		bool TryCultureFromPo(
			const FString& Source,
			FString& OutCulture,
			Contract::FSourceLocation& OutCultureSource, FLocalizationImportError& OutError)
		{
			FString PublicSource = Source;
			FPaths::MakePathRelativeTo(PublicSource, *FPaths::ProjectDir());
			OutCultureSource = {};
			OutCultureSource.SourceFile = PublicSource;
			FString Text;
			if (!FFileHelper::LoadFileToString(Text, *Source))
			{
				OutError = NativeFileFailure(TEXT("localization_import_source_read_failed"),
					TEXT("The localization PO source could not be read."), FString(), Source,
					TEXT("file_read_succeeded"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.Source.SourceFile = PublicSource;
				return false;
			}
			TArray<FString> Lines;
			Text.ParseIntoArrayLines(Lines, false);
			int32 LanguageLine = 0;
			for (int32 Index = 0; Index < Lines.Num(); ++Index)
			{
				FString Line = Lines[Index];
				Line.TrimStartAndEndInline();
				Line.RemoveFromStart(TEXT("\""));
				if (!Line.StartsWith(TEXT("Language:")))
				{
					continue;
				}
				OutCulture = Line.Mid(9).TrimStartAndEnd();
				LanguageLine = Index + 1;
				OutCultureSource.Line = LanguageLine;
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
			OutError = NativeValueFailure(TEXT("localization_import_po_culture_invalid"),
				LanguageLine > 0 ? TEXT("The PO Language header is not a supported culture.") : TEXT("The PO Language header is missing."),
				FString(), TEXT("Language"), LanguageLine > 0 ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(OutCulture)) : nullptr,
				MakeShared<FJsonValueString>(TEXT("culture recognized by Unreal Internationalization")));
			OutError.Source.SourceFile = PublicSource;
			OutError.Source.Line = LanguageLine;
			OutError.PropertyPath = TEXT("Language");
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("culture recognized by Unreal Internationalization"));
			OutError.ActualValuePresent = LanguageLine > 0;
			if (LanguageLine > 0) OutError.ActualValue = MakeShared<FJsonValueString>(OutCulture);
			OutError.PublicMessage = LanguageLine > 0 ? TEXT("The PO Language header is not a supported culture.") : TEXT("The PO Language header is missing.");
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

		TSharedPtr<FJsonValue> SupportedCultureNames(const ULocalizationTarget& Target)
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const FCultureStatistics& Culture : Target.Settings.SupportedCulturesStatistics)
				Values.Add(MakeShared<FJsonValueString>(Culture.CultureName));
			return MakeShared<FJsonValueArray>(MoveTemp(Values));
		}

		void AppendCommandletOutput(
			FString& InOutOutput,
			FString Chunk)
		{
			if (Chunk.IsEmpty())
			{
				return;
			}
			InOutOutput += MoveTemp(Chunk);
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

			bool Launch(const FString& ProcessArguments, FLocalizationImportError& OutError)
			{
				if (!FPlatformProcess::CreatePipe(ReadPipe, WritePipe))
				{
					OutError = NativeConditionFailure(TEXT("localization_commandlet_pipe_failed"),
						TEXT("The GatherText commandlet output pipe could not be created."), FString(), TEXT("commandlet_output_pipe_available"), false, true);
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
				if (Handle.IsValid()) return true;
				OutError = NativeConditionFailure(TEXT("localization_commandlet_launch_failed"),
					TEXT("The native process launcher did not create a GatherText commandlet process."), FString(), TEXT("commandlet_process_handle_valid"), false, true, ProcessArguments);
				return false;
			}

			void* ReadPipe = nullptr;
			void* WritePipe = nullptr;
			FProcHandle Handle;
		};

		bool RunLocalizationCommandlet(
			const FString& ConfigPath,
			const bool bUseProjectFile,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutError.Reset();
            FString NativeOutput;
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
			if (!Process.Launch(ProcessArguments, OutError))
			{
				return false;
			}

			while (FPlatformProcess::IsProcRunning(Process.Handle))
			{
				AppendCommandletOutput(
					NativeOutput,
					FPlatformProcess::ReadPipe(Process.ReadPipe));
				FPlatformProcess::Sleep(0.01f);
			}
			AppendCommandletOutput(
				NativeOutput,
				FPlatformProcess::ReadPipe(Process.ReadPipe));
			int32 ReturnCode = INDEX_NONE;
			if (!FPlatformProcess::GetProcReturnCode(
					Process.Handle,
					&ReturnCode))
			{
				OutError = NativeConditionFailure(TEXT("localization_commandlet_exit_unavailable"),
					TEXT("The GatherText commandlet return code was unavailable."), FString(), TEXT("commandlet_exit_code_available"), false, true, NativeOutput);
				return false;
			}
			if (ReturnCode != 0)
			{
				OutError = NativeValueFailure(TEXT("localization_commandlet_failed"),
					TEXT("The GatherText commandlet failed."), FString(), TEXT("commandlet_exit_code"),
					MakeShared<FJsonValueNumber>(ReturnCode), MakeShared<FJsonValueNumber>(0), NativeOutput);
				return false;
			}
			return true;
		}

		bool WriteAndRunLocalizationCommandlet(
			FLocalizationConfigurationScript Script,
			const FString& DefaultConfigPath,
			const bool bUseProjectFile,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			IFileManager& Files = IFileManager::Get();
			const FString ConfigDirectory = FPaths::GetPath(DefaultConfigPath);
			if (!Files.MakeDirectory(*ConfigDirectory, true)
				&& !Files.DirectoryExists(*ConfigDirectory))
			{
				OutError = NativeConditionFailure(TEXT("localization_config_directory_failed"),
					TEXT("Could not create the localization config directory."), FString(), TEXT("config_directory_available"), false, true, ConfigDirectory);
				return false;
			}
			const FString TemporaryConfig = FPaths::CreateTempFilename(
				*ConfigDirectory,
				*FPaths::GetBaseFilename(DefaultConfigPath),
				*FPaths::GetExtension(DefaultConfigPath, true));
			if (!Script.Write(TemporaryConfig))
			{
				OutError = NativeConditionFailure(TEXT("localization_config_write_failed"),
					TEXT("Could not write the temporary localization config."), FString(), TEXT("temporary_config_written"), false, true, TemporaryConfig);
				return false;
			}
			const bool bSucceeded = RunLocalizationCommandlet(
				TemporaryConfig,
				bUseProjectFile,
				OutError);
			const bool bDeleted = Files.Delete(
				*TemporaryConfig,
				false,
				true,
				true);
			if (!bDeleted && FPaths::FileExists(TemporaryConfig))
			{
				auto CleanupError = NativeConditionFailure(TEXT("localization_config_cleanup_failed"),
					TEXT("The temporary localization config could not be removed."), FString(), TEXT("temporary_config_removed"), false, true, TemporaryConfig);
				CleanupError.Stage = TEXT("cleanup");
				OutError.Append(CleanupError);
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

			bool Capture(const FString& InPath, FLocalizationImportError& OutError, const FString& Target)
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
				if (Size < 0 || Size > MAX_int32)
				{
					OutError = NativeFileFailure(TEXT("localization_import_snapshot_size_invalid"),
						Size < 0 ? TEXT("The localization backup file size could not be read.") : TEXT("The localization backup file exceeds the supported byte count."),
						Target, Path, TEXT("file_size_bytes"), MakeShared<FJsonValueNumberString>(LexToString(Size)),
						MakeShared<FJsonValueString>(TEXT("0 <= file size <= 2147483647")));
					return false;
				}
				if (!FFileHelper::LoadFileToArray(Bytes, *Path))
				{
					OutError = NativeFileFailure(TEXT("localization_import_snapshot_read_failed"),
						TEXT("The localization file could not be read into its rollback snapshot."), Target, Path, TEXT("file_read_succeeded"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return false;
				}
				if (!TryHashBytes(Bytes.GetData(), Bytes.Num(), Revision, OutError, Target))
				{
					FString PublicPath = Path;
					FPaths::MakePathRelativeTo(PublicPath, *FPaths::ProjectDir());
					OutError.Source.SourceFile = PublicPath;
					return false;
				}
				return true;
			}

			bool CheckCurrent(FLocalizationImportError& OutError, const FString& Target) const
			{
				if (bExisted)
					return CheckFileRevision(Path, Revision, OutError, Target, TEXT("localization_import_version_conflict"),
						TEXT("A tracked localization file changed after its snapshot."));
				if (!FPaths::FileExists(Path)) return true;
				OutError = NativeFileFailure(TEXT("localization_import_version_conflict"),
					TEXT("A previously absent localization file now exists."), Target, Path, TEXT("file_present"),
					MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
				return false;
			}

			bool Restore(FLocalizationImportError& OutError, const FString& Target) const
			{
				if (!bExisted)
				{
					if (!FPaths::FileExists(Path) || IFileManager::Get().Delete(*Path, false, true, true)) return true;
					OutError = NativeFileFailure(TEXT("localization_import_rollback_delete_failed"),
						TEXT("A localization file created by the import could not be removed during rollback."), Target, Path,
						TEXT("file_removed"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return false;
				}
				IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
				if (!FFileHelper::SaveArrayToFile(Bytes, *Path))
				{
					OutError = NativeFileFailure(TEXT("localization_import_rollback_write_failed"),
						TEXT("The localization rollback snapshot could not be written back to its original file."), Target, Path,
						TEXT("file_write_succeeded"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return false;
				}
				return CheckFileRevision(Path, Revision, OutError, Target, TEXT("localization_import_rollback_revision_mismatch"),
					TEXT("The restored localization file does not match its rollback snapshot."));
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

			virtual bool CheckPreExecute(FLocalizationImportError& OutError) override
			{
				Target = FindProjectTarget(Spec.TargetRef);
				if (!Target)
				{
					OutError = NativeConditionFailure(TEXT("localization_import_version_conflict"),
						TEXT("The localization target is unavailable before execution."), TargetIdentity, TEXT("localization_target_available"), false, true);
					return false;
				}
				if (!SupportsCulture(*Target, Culture))
				{
					OutError = NativeValueFailure(TEXT("localization_import_version_conflict"),
						TEXT("The localization target no longer supports the requested culture."), TargetIdentity, TEXT("culture"),
						MakeShared<FJsonValueString>(Culture), SupportedCultureNames(*Target));
					return false;
				}
				if (!CheckFileRevision(Source, SourceSha256, OutError, TargetIdentity, TEXT("localization_import_version_conflict"),
						TEXT("The localization source file changed before execution."))
					|| !CheckFileRevision(ManifestPath, ManifestRevision, OutError, TargetIdentity, TEXT("localization_import_version_conflict"),
						TEXT("The localization manifest changed before execution."))) return false;
				for (const FFileSnapshot& Snapshot : Snapshots)
					if (!Snapshot.CheckCurrent(OutError, TargetIdentity)) return false;
				return true;
			}

			virtual bool Execute(FLocalizationImportError& OutError) override
			{
				IFileManager& Files = IFileManager::Get();
				Files.MakeDirectory(*FPaths::GetPath(PersistedPoPath), true);
				const int32 CopyResult = Files.Copy(*StagingPath, *Source, true, true);
				if (CopyResult != COPY_OK)
				{
					OutError = NativeFileFailure(TEXT("localization_import_stage_failed"),
						TEXT("The localization source could not be copied to its staging file."), TargetIdentity, Source, TEXT("native_copy_result"),
						MakeShared<FJsonValueNumber>(CopyResult), MakeShared<FJsonValueNumber>(COPY_OK), StagingPath);
					return false;
				}
				if (!Files.Move(*PersistedPoPath, *StagingPath, true, true, false, true))
				{
					OutError = NativeFileFailure(TEXT("localization_import_publish_failed"),
						TEXT("The staged localization PO file could not be moved to its persisted destination."), TargetIdentity, PersistedPoPath,
						TEXT("staged_file_published"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), StagingPath);
					DeleteStaging();
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
					OutError = NativeConditionFailure(TEXT("localization_import_target_missing"),
						TEXT("The localization target is unavailable during settle."), TargetIdentity, TEXT("localization_target_available"), false, true);
					return false;
				}
				const bool bUseProjectFile =
					!Target->IsMemberOfEngineTargetSet();
				UE::UAI::Contract::FDiagnostic CommandletError;
				if (!WriteAndRunLocalizationCommandlet(
						LocalizationConfigurationScript::GenerateImportTextConfigFile(
							Target,
							TOptional<FString>(Culture),
							TOptional<FString>(PersistedPoPath)),
						LocalizationConfigurationScript::GetImportTextConfigPath(
							Target,
							TOptional<FString>(Culture)),
						bUseProjectFile,
						CommandletError))
				{
					SetError(OutError, CommandletError, TargetIdentity);
					return false;
				}
				bFormalImporterCompleted = true;
				CommandletError.Reset();
				if (!WriteAndRunLocalizationCommandlet(
						LocalizationConfigurationScript::GenerateCompileTextConfigFile(
							Target,
							TOptional<FString>(Culture)),
						LocalizationConfigurationScript::GetCompileTextConfigPath(
							Target,
							TOptional<FString>(Culture)),
						bUseProjectFile,
						CommandletError))
				{
					SetError(OutError, CommandletError, TargetIdentity);
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
					OutError = NativeConditionFailure(TEXT("localization_import_refresh_failed"),
						TEXT("The localization resource refresh did not advance the text revision."), TargetIdentity, TEXT("text_revision_advanced"), false, true);
					OutError.ActualEvidence = MakeShared<FJsonValueNumber>(RefreshRevisionAfter);
					OutError.ExpectedEvidence = MakeShared<FJsonValueNumber>(RefreshRevisionBefore);
					return false;
				}
				return true;
			}

			virtual bool CheckPrePersist(FLocalizationImportError& OutError) override
			{
				Target = FindProjectTarget(Spec.TargetRef);
				if (!Target)
				{
					OutError = NativeConditionFailure(TEXT("localization_import_pre_persist_fence_failed"),
						TEXT("The localization target is unavailable before persistence."), TargetIdentity, TEXT("localization_target_available"), false, true);
					return false;
				}
				if (!SupportsCulture(*Target, Culture))
				{
					OutError = NativeValueFailure(TEXT("localization_import_pre_persist_fence_failed"),
						TEXT("The localization target no longer supports the imported culture."), TargetIdentity, TEXT("culture"),
						MakeShared<FJsonValueString>(Culture), SupportedCultureNames(*Target));
					return false;
				}
				if (!CheckFileRevision(PersistedPoPath, SourceSha256, OutError, TargetIdentity, TEXT("localization_import_pre_persist_fence_failed"),
					TEXT("The persisted localization PO differs from the imported source."))) return false;
				const bool bArchiveExists = FPaths::FileExists(ArchivePath);
				const bool bLocResExists = bArchiveExists && FPaths::FileExists(LocResPath);
				if (!bArchiveExists || !bLocResExists)
				{
					const bool bArchiveMissing = !bArchiveExists;
					OutError = NativeFileFailure(TEXT("localization_import_pre_persist_fence_failed"),
						bArchiveMissing ? TEXT("The localization archive was not produced.") : TEXT("The compiled localization resource was not produced."),
						TargetIdentity, bArchiveMissing ? ArchivePath : LocResPath, TEXT("file_exists"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return false;
				}
				if (!bFormalImporterCompleted || !bFormalCompilerCompleted || !bRefreshCompleted)
				{
					OutError = NativeConditionFailure(TEXT("localization_import_pre_persist_fence_failed"),
						!bFormalImporterCompleted ? TEXT("The formal localization importer has not completed.")
							: !bFormalCompilerCompleted ? TEXT("The formal localization compiler has not completed.") : TEXT("The localization resource refresh has not completed."),
						TargetIdentity, !bFormalImporterCompleted ? TEXT("formal_importer_completed")
							: !bFormalCompilerCompleted ? TEXT("formal_compiler_completed") : TEXT("resource_refresh_completed"), false, true);
					return false;
				}
				return true;
			}

			virtual bool Persist(
				FString& OutPersistenceReceiptId,
				FLocalizationImportError& OutError) override
			{
				if (!TryHashFile(PersistedPoPath, PersistedPoSha256, OutError, TargetIdentity)
					|| !TryHashFile(ArchivePath, ArchiveSha256, OutError, TargetIdentity)
					|| !TryHashFile(LocResPath, LocResSha256, OutError, TargetIdentity))
				{
					OutError = Contract::FDiagnostic::WrapFailure(TEXT("localization_import_persist_failed"), OutError);
					return false;
				}
				if (PersistedPoSha256 != SourceSha256)
				{
					OutError = RevisionMismatch(TEXT("localization_import_persist_failed"),
						TEXT("The persisted localization PO differs from the imported source."), TargetIdentity, PersistedPoPath, PersistedPoSha256, SourceSha256);
					return false;
				}
				if (!TryHashUtf8(TargetIdentity + TEXT("|") + PersistedPoSha256 + TEXT("|") + ArchiveSha256
					+ TEXT("|") + LocResSha256 + TEXT("|") + FString::FromInt(RefreshRevisionAfter), OutPersistenceReceiptId, OutError, TargetIdentity))
				{
					OutError = Contract::FDiagnostic::WrapFailure(TEXT("localization_import_persistence_receipt_missing"), OutError);
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
				if (!TryHashFile(PersistedPoPath, OutReadback.PersistedPoSha256, OutError, TargetIdentity)) return false;
				OutReadback.ArchivePath = ArchivePath;
				if (!TryHashFile(ArchivePath, OutReadback.ArchiveSha256, OutError, TargetIdentity)) return false;
				OutReadback.LocResPath = LocResPath;
				if (!TryHashFile(LocResPath, OutReadback.LocResSha256, OutError, TargetIdentity)) return false;
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
					if (!OutReadback.bTargetResolved)
					{
						OutError = !Target ? NativeConditionFailure(TEXT("localization_import_readback_mismatch"),
							TEXT("The localization target is unavailable during readback."), TargetIdentity, TEXT("localization_target_available"), false, true)
							: NativeValueFailure(TEXT("localization_import_readback_mismatch"),
								TEXT("The localization target no longer supports the imported culture."), TargetIdentity, TEXT("culture"),
								MakeShared<FJsonValueString>(Culture), SupportedCultureNames(*Target));
					}
					else if (OutReadback.PersistedPoSha256 != SourceSha256)
						OutError = RevisionMismatch(TEXT("localization_import_readback_mismatch"),
							TEXT("The localization PO changed before readback."), TargetIdentity, PersistedPoPath, OutReadback.PersistedPoSha256, SourceSha256);
					else if (OutReadback.ArchiveSha256 != ArchiveSha256)
						OutError = RevisionMismatch(TEXT("localization_import_readback_mismatch"),
							TEXT("The localization archive changed before readback."), TargetIdentity, ArchivePath, OutReadback.ArchiveSha256, ArchiveSha256);
					else if (OutReadback.LocResSha256 != LocResSha256)
						OutError = RevisionMismatch(TEXT("localization_import_readback_mismatch"),
							TEXT("The compiled localization resource changed before readback."), TargetIdentity, LocResPath, OutReadback.LocResSha256, LocResSha256);
					else
						OutError = NativeConditionFailure(TEXT("localization_import_readback_mismatch"),
							!OutReadback.bFormalImporterCompleted ? TEXT("Readback did not confirm completion of the formal localization importer.")
								: !OutReadback.bFormalCompilerCompleted ? TEXT("Readback did not confirm completion of the formal localization compiler.")
								: TEXT("Readback did not confirm advancement of the localization text revision."),
							TargetIdentity, !OutReadback.bFormalImporterCompleted ? TEXT("formal_importer_completed")
								: !OutReadback.bFormalCompilerCompleted ? TEXT("formal_compiler_completed") : TEXT("text_revision_advanced"), false, true);
					return false;
				}
				return true;
			}

			virtual bool Rollback(
				bool& bOutConfirmed,
				FLocalizationImportError& OutError) override
			{
				OutError.Reset();
				DeleteStaging();
				bool bRestored = true;
				for (int32 Index = Snapshots.Num() - 1; Index >= 0; --Index)
				{
					FLocalizationImportError RestoreError;
					if (!Snapshots[Index].Restore(RestoreError, TargetIdentity))
					{
						bRestored = false;
						OutError.Append(RestoreError);
					}
				}
				Target = FindProjectTarget(Spec.TargetRef);
				if (Target) Target->UpdateWordCountsFromCSV();
				FTextLocalizationManager& Manager = FTextLocalizationManager::Get();
				Manager.RefreshResources();
				Manager.WaitForAsyncTasks();
				bOutConfirmed = bRestored && Target && SupportsCulture(*Target, Culture);
				if (!bOutConfirmed && OutError.IsEmpty())
				{
					OutError = !Target ? NativeConditionFailure(TEXT("localization_import_rollback_target_invalid"),
						TEXT("The localization target is unavailable after rollback."), TargetIdentity, TEXT("localization_target_available"), false, true)
						: NativeValueFailure(TEXT("localization_import_rollback_target_invalid"),
							TEXT("The localization target no longer supports the restored culture."), TargetIdentity, TEXT("culture"),
							MakeShared<FJsonValueString>(Culture), SupportedCultureNames(*Target));
				}
				if (bOutConfirmed)
					for (const FFileSnapshot& Snapshot : Snapshots)
						if (!Snapshot.CheckCurrent(OutError, TargetIdentity)) { bOutConfirmed = false; break; }
				if (!bOutConfirmed && !OutError.IsEmpty())
					OutError = Contract::FDiagnostic::WrapFailure(TEXT("localization_import_rollback_unconfirmed"), OutError);
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
					OutError = NativeConditionFailure(TEXT("localization_import_game_thread_required"),
						TEXT("Localization import preparation must run on the game thread."), InSpec.TargetRef, TEXT("game_thread"), false, true);
					return false;
				}
				FLocalizationImportSpec Spec = InSpec;
				Spec.TargetRef.TrimStartAndEndInline();
				if (!Spec.IsValid() || !IsSafeSegment(Spec.TargetRef))
				{
					if (Spec.RequestId.IsEmpty())
						OutError = NativeConditionFailure(TEXT("localization_import_spec_invalid"),
							TEXT("The localization specification has no request identity."), Spec.TargetRef, TEXT("request_identity_present"), false, true);
					else if (Spec.SourceRef.IsEmpty())
						OutError = NativeValueFailure(TEXT("localization_import_spec_invalid"),
							TEXT("The localization specification has no source file."), Spec.TargetRef, TEXT("source_ref"),
							MakeShared<FJsonValueString>(Spec.SourceRef), MakeShared<FJsonValueString>(TEXT("nonempty PO source file path")));
					else
						OutError = NativeValueFailure(TEXT("localization_import_spec_invalid"),
							TEXT("The localization target name must contain only letters, digits, underscores or hyphens."), Spec.TargetRef, TEXT("target_ref"),
							MakeShared<FJsonValueString>(Spec.TargetRef), MakeShared<FJsonValueString>(TEXT("nonempty safe target name")));
					return false;
				}
				FString Source;
				if (!TryNormalizeSource(Spec.SourceRef, Source, OutError))
				{
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Spec.TargetRef;
					return false;
				}
				FString SourceSha256;
				if (!TryHashFile(Source, SourceSha256, OutError, Spec.TargetRef))
				{
					OutError = Contract::FDiagnostic::WrapFailure(TEXT("localization_import_source_hash_failed"), OutError);
					return false;
				}
				FString Culture;
				Contract::FSourceLocation CultureSource;
				if (!TryCultureFromPo(Source, Culture, CultureSource, OutError))
				{
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Spec.TargetRef;
					return false;
				}
				ULocalizationTarget* const Target =
					FindProjectTarget(Spec.TargetRef);
				if (!Target)
				{
					OutError = NativeConditionFailure(TEXT("localization_import_target_missing"),
						TEXT("The requested project localization target could not be found."), Spec.TargetRef, TEXT("localization_target_available"), false, true);
					return false;
				}
				if (!SupportsCulture(*Target, Culture))
				{
					OutError = NativeValueFailure(TEXT("localization_import_culture_unsupported"),
						TEXT("The culture in the PO Language header is not configured on the localization target."), Spec.TargetRef, TEXT("Language"),
						MakeShared<FJsonValueString>(Culture), SupportedCultureNames(*Target));
					OutError.Source = CultureSource;
					return false;
				}
				const FString ManifestPath =
					LocalizationConfigurationScript::GetManifestPath(Target);
				FString ManifestRevision;
				if (!TryHashFile(ManifestPath, ManifestRevision, OutError, Spec.TargetRef))
				{
					OutError = Contract::FDiagnostic::WrapFailure(TEXT("localization_import_manifest_missing"), OutError);
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
					if (!Snapshot.Capture(Path, OutError, Spec.TargetRef))
					{
						OutError = Contract::FDiagnostic::WrapFailure(TEXT("localization_import_backup_failed"), OutError);
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
				FString ExpectedRevision;
				if (!TryHashUtf8(RevisionInput, ExpectedRevision, OutError, Spec.TargetRef))
				{
					OutError = Contract::FDiagnostic::WrapFailure(TEXT("localization_import_revision_failed"), OutError);
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutProvider = MakeShared<FProvider>();
		OutError.Reset();
		return true;
	}
}

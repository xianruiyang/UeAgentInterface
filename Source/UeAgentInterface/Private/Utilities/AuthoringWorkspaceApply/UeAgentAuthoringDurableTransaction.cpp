// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringDurableTransaction.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringDurableDirectory.h"
#include "Core/AssetDurable/UeAgentPackageSave.h"
#include "Utilities/UeAgentFileOperationDiagnostic.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Misc/ScopeExit.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/Archive.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace {
using namespace UeAgentAuthoringWorkspaceTransaction;

constexpr const TCHAR *JournalSchema = TEXT("uai.apply_durable_journal.v1");
constexpr const TCHAR *JournalFileName = TEXT("journal.json");
thread_local FScopedDurableRecoveryNotice* CurrentRecoveryNotice = nullptr;

struct FDurableArtifactState {
  EDurableArtifactKind Kind = EDurableArtifactKind::File;
  FString ArtifactId;
  FString FinalPath;
  FString StagingPath;
  FString BackupPath;
  FString QuarantinePath;
  FString OldHash;
  FString StagedHash;
  FString MemoryCheckpointPath;
  FString MemoryCheckpointHash;
  bool bMemoryCheckpointExists = false;
  bool bOldExists = false;
  bool bStagedExists = false;
};

struct FDurableJournalState {
  FString RequestId;
  FString RequestKey;
  FString RequestFingerprint;
  FString TransactionRoot;
  FString JournalPath;
  FString CompletedPath;
  FString Phase;
  TArray<FString> PhaseHistory;
  TArray<FDurableArtifactState> Artifacts;
};

void SetFailure(FOperationFailure& OutFailure, UE::UAI::Contract::FDiagnostic Diagnostic) {
  OutFailure.Diagnostic = MoveTemp(Diagnostic);
  if (OutFailure.Diagnostic.Stage.IsEmpty())
    OutFailure.Diagnostic.Stage = TEXT("workspace_durable_transaction");
  OutFailure.Diagnostic.bRetrySafe = false;
}

UE::UAI::Contract::FDiagnostic MakeDurableFailure(
    const FString& Code, const FString& Reason, const UE::UAI::Contract::FDiagnostic& Cause,
    const FString& Subject = FString()) {
  auto Diagnostic = UE::UAI::Contract::FDiagnostic::WrapFailure(
      Code, Cause, TEXT("workspace_durable_transaction"), Reason);
  if (!Subject.IsEmpty()) Diagnostic.SubjectId = Subject;
  return Diagnostic;
}

UE::UAI::Contract::FDiagnostic SnapshotMismatch(
    const TCHAR* Code, const FString& Reason, const FString& Subject,
    bool bExpectedExists, const FString& ExpectedHash,
    bool bActualExists, const FString& ActualHash) {
  auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(
      Code, Reason, TEXT("workspace_durable_transaction"), FString(), Subject);
  Diagnostic.SourceKind = TEXT("native_result");
  Diagnostic.PropertyPath = TEXT("artifact");
  Diagnostic.ExpectedValuePresent = bExpectedExists;
  Diagnostic.ActualValuePresent = bActualExists;
  if (bExpectedExists && bActualExists) {
    Diagnostic.PropertyPath = TEXT("artifact/content");
    Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("content matches the recorded snapshot"));
    Diagnostic.ActualValue = MakeShared<FJsonValueString>(TEXT("content differs from the recorded snapshot"));
  }
  Diagnostic.ExpectedEvidence = MakeShared<FJsonValueString>(ExpectedHash);
  Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(ActualHash);
  return Diagnostic;
}

UE::UAI::Contract::FDiagnostic StateFailure(const TCHAR* Code, const FString& Reason,
    bool bPrepared, const FString& Phase, const FString& ExpectedPhase = FString()) {
  auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("workspace_durable_transaction"));
  Diagnostic.SourceKind = TEXT("native_result");
  Diagnostic.PropertyPath = bPrepared ? TEXT("phase") : TEXT("prepared");
  if (bPrepared) {
    Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(ExpectedPhase);
    Diagnostic.ActualValue = MakeShared<FJsonValueString>(Phase);
  } else {
    Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
    Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
  }
  return Diagnostic;
}

void CleanupCompletedTransaction(const FDurableJournalState& State, FDurabilityResult& Result) {
  if (IFileManager::Get().DeleteDirectory(*State.TransactionRoot, false, true)) return;
  auto Warning = UE::UAI::FileDiagnostics::MakeFileOperationFailure(TEXT("durable_completed_cleanup_failed"),
      TEXT("The transaction completed, but removing its temporary storage failed."), TEXT("workspace_durable_cleanup"), State.TransactionRoot);
  Warning.Severity = UE::UAI::Contract::ESeverity::Warning;
  Warning.TransactionStatus = State.Phase;
  UeAgentAuthoringDiagnostics::FScopedWarnings CommandWarnings;
  CommandWarnings.Add(Warning);
  Result.Warnings.Add(MoveTemp(Warning));
}

FString NormalizeAbsolutePath(const FString &Input) {
  FString Result = FPaths::ConvertRelativePathToFull(Input);
  FPaths::NormalizeFilename(Result);
  FPaths::CollapseRelativeDirectories(Result);
  return Result;
}

bool IsWithinRoot(const FString &RootInput, const FString &CandidateInput) {
  const FString Root = NormalizeAbsolutePath(RootInput);
  const FString Candidate = NormalizeAbsolutePath(CandidateInput);
  if (Candidate.Equals(Root, ESearchCase::IgnoreCase))
    return true;
  const FString Prefix = Root.EndsWith(TEXT("/")) ? Root : Root + TEXT("/");
  return Candidate.StartsWith(Prefix, ESearchCase::IgnoreCase);
}

bool EnsureParentDirectory(const FString &Path) {
  const FString Parent = FPaths::GetPath(Path);
  return !Parent.IsEmpty() &&
         IFileManager::Get().MakeDirectory(*Parent, true);
}

bool SerializeJson(const TSharedRef<FJsonObject> &Object, FString &OutText) {
  OutText.Reset();
  const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
      TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(
          &OutText);
  return FJsonSerializer::Serialize(Object, Writer);
}

bool SaveDurableText(const FString &Path, const FString &Text,
                     UE::UAI::Contract::FDiagnostic& OutError) {
  using UE::UAI::FileDiagnostics::MakeFileOperationFailure;
  if (!EnsureParentDirectory(Path)) {
    OutError = MakeFileOperationFailure(TEXT("durable_journal_parent_create_failed"),
        TEXT("Creating the durable journal's parent directory failed."), TEXT("workspace_durable_transaction"), Path);
    return false;
  }
  const FString TemporaryPath = Path + TEXT(".tmp");
  IPlatformFile &PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
  TUniquePtr<IFileHandle> Handle(PlatformFile.OpenWrite(*TemporaryPath, false, false));
  if (!Handle) {
    OutError = MakeFileOperationFailure(TEXT("durable_journal_open_failed"),
        TEXT("Opening the temporary durable journal for writing failed."), TEXT("workspace_durable_transaction"), TemporaryPath);
    return false;
  }
  const auto Cleanup = [&]() {
    Handle.Reset();
    if (!IFileManager::Get().Delete(*TemporaryPath, false, true, true))
      OutError.Append(MakeFileOperationFailure(TEXT("durable_journal_cleanup_failed"),
          TEXT("Removing the failed temporary durable journal also failed."), TEXT("workspace_durable_transaction"), TemporaryPath));
  };
  const FTCHARToUTF8 Utf8(*Text);
  if (!Handle->Write(reinterpret_cast<const uint8 *>(Utf8.Get()), Utf8.Length())) {
    OutError = MakeFileOperationFailure(TEXT("durable_journal_write_failed"),
        TEXT("Writing the temporary durable journal failed."), TEXT("workspace_durable_transaction"), TemporaryPath);
    Cleanup();
    return false;
  }
  if (!Handle->Flush(true)) {
    OutError = MakeFileOperationFailure(TEXT("durable_journal_flush_failed"),
        TEXT("Flushing the temporary durable journal to persistent storage failed."), TEXT("workspace_durable_transaction"), TemporaryPath);
    Cleanup();
    return false;
  }
  Handle.Reset();
  if (!IFileManager::Get().Move(*Path, *TemporaryPath, true, false, false, true)) {
    OutError = MakeFileOperationFailure(TEXT("durable_journal_replace_failed"),
        TEXT("Replacing the durable journal with its newly written version failed."), TEXT("workspace_durable_transaction"), Path);
    Cleanup();
    return false;
  }
  return true;
}

bool LoadJson(const FString &Path, TSharedPtr<FJsonObject> &OutObject,
              UE::UAI::Contract::FDiagnostic& OutError) {
  FString Text;
  if (!FFileHelper::LoadFileToString(Text, *Path)) {
    OutError = UE::UAI::FileDiagnostics::MakeFileOperationFailure(TEXT("durable_journal_read_failed"),
        TEXT("Reading the durable transaction journal failed."), TEXT("workspace_durable_transaction"), Path);
    return false;
  }
  const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
  if (!FJsonSerializer::Deserialize(Reader, OutObject) || !OutObject.IsValid()) {
    OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_journal_json_invalid"),
        TEXT("The durable transaction journal is not a valid JSON object."), TEXT("workspace_durable_transaction"),
        FString::Printf(TEXT("%s:%d:%d: %s"), *Path, Reader->GetLineNumber(), Reader->GetCharacterNumber(), *Reader->GetErrorMessage()));
    OutError.SourceKind = TEXT("native_result");
    return false;
  }
  return true;
}

bool FileHash(const FString &Path, FString& OutHash, UE::UAI::Contract::FDiagnostic& OutError) {
  OutHash.Reset();
  TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Path));
  if (!Reader) {
    OutError = UE::UAI::FileDiagnostics::MakeFileOperationFailure(TEXT("durable_artifact_read_failed"),
        TEXT("Opening the durable artifact to verify its content failed."), TEXT("workspace_durable_transaction"), Path);
    return false;
  }
  const int64 Size = Reader->TotalSize();
  if (Size < 0) {
    OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_artifact_size_invalid"),
        TEXT("The artifact reader returned an invalid file size."), TEXT("workspace_durable_transaction"), Path);
    OutError.PropertyPath = TEXT("file_size");
    OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("nonnegative byte count"));
    OutError.ActualValue = MakeShared<FJsonValueNumber>(static_cast<double>(Size));
    return false;
  }
  const FMD5Hash Hash = FMD5Hash::HashFileFromArchive(Reader.Get());
  if (Reader->IsError() || !Hash.IsValid()) {
    OutError = UE::UAI::FileDiagnostics::MakeFileOperationFailure(TEXT("durable_artifact_read_failed"),
        TEXT("Reading the durable artifact to verify its content failed."), TEXT("workspace_durable_transaction"), Path);
    return false;
  }
  OutHash = LexToString(Hash).ToLower();
  return true;
}

bool ArtifactSnapshot(EDurableArtifactKind Kind, const FString& Path,
                      bool& bExists, FString& Hash, UE::UAI::Contract::FDiagnostic& Error) {
  if (Kind == EDurableArtifactKind::DirectoryTree)
    return DurableDirectory::Snapshot(Path, bExists, Hash, Error);
  if (Kind != EDurableArtifactKind::File) {
    Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_artifact_kind_invalid"),
        TEXT("The artifact kind is not supported by durable snapshotting."), TEXT("workspace_durable_transaction"));
    Error.PropertyPath = TEXT("artifact_kind");
    Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("file or directory_tree"));
    Error.ActualValue = MakeShared<FJsonValueNumber>(static_cast<int32>(Kind));
    return false;
  }
  if (IFileManager::Get().DirectoryExists(*Path)) {
    Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_artifact_type_conflict"),
        TEXT("A declared file artifact resolves to a directory."), TEXT("workspace_durable_transaction"), Path);
    Error.PropertyPath = TEXT("artifact_kind");
    Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("file"));
    Error.ActualValue = MakeShared<FJsonValueString>(TEXT("directory_tree"));
    return false;
  }
  bExists = IFileManager::Get().FileExists(*Path);
  Hash.Reset();
  return !bExists || FileHash(Path, Hash, Error);
}

bool VerifyArtifactSnapshot(EDurableArtifactKind Kind, const FString& Path,
    bool bExpectedExists, const FString& ExpectedHash, const TCHAR* Code,
    const FString& Reason, const FString& Subject, UE::UAI::Contract::FDiagnostic& Error,
    bool bCompareHash = true) {
  bool bExists = false;
  FString Hash;
  if (!ArtifactSnapshot(Kind, Path, bExists, Hash, Error)) {
    Error = MakeDurableFailure(Code, Reason, Error, Subject);
    return false;
  }
  if (bExists != bExpectedExists || (bExists && bCompareHash && Hash != ExpectedHash)) {
    Error = SnapshotMismatch(Code, Reason, Subject, bExpectedExists, ExpectedHash, bExists, Hash);
    return false;
  }
  return true;
}

bool CopyArtifact(EDurableArtifactKind Kind, const FString& Destination,
                  const FString& Source, UE::UAI::Contract::FDiagnostic& Error) {
  if (Kind == EDurableArtifactKind::DirectoryTree)
    return DurableDirectory::Copy(Destination, Source, Error);
  UE::UAI::AssetDurable::ReleaseLoadedPackageFileForReplacement(Destination);
  if (!EnsureParentDirectory(Destination)) {
    Error = UE::UAI::FileDiagnostics::MakeFileOperationFailure(TEXT("durable_artifact_copy_failed"), TEXT("Creating the destination directory for the file copy failed."), TEXT("workspace_durable_transaction"), Destination);
    return false;
  }
  if (IFileManager::Get().Copy(*Destination, *Source, true, true) == COPY_OK) return true;
  Error = UE::UAI::FileDiagnostics::MakeFileOperationFailure(TEXT("durable_artifact_copy_failed"), TEXT("The native file copy operation failed."), TEXT("workspace_durable_transaction"), Destination);
  return false;
}

bool MoveArtifact(EDurableArtifactKind Kind, const FString& Destination,
                  const FString& Source, UE::UAI::Contract::FDiagnostic& Error) {
  if (Kind != EDurableArtifactKind::File) {
    Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_artifact_move_kind_invalid"), TEXT("The durable artifact move operation requires a file artifact."), TEXT("workspace_durable_transaction"));
    Error.SourceKind = TEXT("native_result");
    Error.PropertyPath = TEXT("artifact_kind");
    Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("file"));
    Error.ActualValue = MakeShared<FJsonValueString>(Kind == EDurableArtifactKind::DirectoryTree
        ? FString(TEXT("directory_tree")) : FString::Printf(TEXT("unknown (%d)"), static_cast<int32>(Kind)));
    return false;
  }
  UE::UAI::AssetDurable::ReleaseLoadedPackageFileForReplacement(Source);
  UE::UAI::AssetDurable::ReleaseLoadedPackageFileForReplacement(Destination);
  if (!EnsureParentDirectory(Destination)) {
    Error = UE::UAI::FileDiagnostics::MakeFileOperationFailure(TEXT("durable_artifact_move_failed"), TEXT("Creating the destination directory for the file move failed."), TEXT("workspace_durable_transaction"), Destination);
    return false;
  }
  if (IFileManager::Get().Move(*Destination, *Source, true, true, false, true)) return true;
  Error = UE::UAI::FileDiagnostics::MakeFileOperationFailure(TEXT("durable_artifact_move_failed"), TEXT("The native file move operation failed."), TEXT("workspace_durable_transaction"), Destination);
  return false;
}

bool RemoveArtifact(EDurableArtifactKind Kind, const FString& Path, UE::UAI::Contract::FDiagnostic& Error) {
  if (Kind == EDurableArtifactKind::DirectoryTree)
    return DurableDirectory::Remove(Path, Error);
  UE::UAI::AssetDurable::ReleaseLoadedPackageFileForReplacement(Path);
  if (!IFileManager::Get().FileExists(*Path) || IFileManager::Get().Delete(*Path, false, true, true)) return true;
  Error = UE::UAI::FileDiagnostics::MakeFileOperationFailure(TEXT("durable_artifact_delete_failed"), TEXT("The native file delete operation failed."), TEXT("workspace_durable_transaction"), Path);
  return false;
}

TArray<TSharedPtr<FJsonValue>> StringArrayToJson(
    const TArray<FString> &Values) {
  TArray<TSharedPtr<FJsonValue>> Result;
  Result.Reserve(Values.Num());
  for (const FString &Value : Values)
    Result.Add(MakeShared<FJsonValueString>(Value));
  return Result;
}

TSharedRef<FJsonObject> JournalToJson(const FDurableJournalState &State) {
  TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
  Root->SetStringField(TEXT("schema"), JournalSchema);
  Root->SetStringField(TEXT("request_id"), State.RequestId);
  Root->SetStringField(TEXT("request_key"), State.RequestKey);
  Root->SetStringField(TEXT("request_fingerprint"),
                       State.RequestFingerprint);
  Root->SetStringField(TEXT("transaction_root"), State.TransactionRoot);
  Root->SetStringField(TEXT("phase"), State.Phase);
  Root->SetArrayField(TEXT("phase_history"),
                      StringArrayToJson(State.PhaseHistory));
  TArray<TSharedPtr<FJsonValue>> ArtifactValues;
  ArtifactValues.Reserve(State.Artifacts.Num());
  for (const FDurableArtifactState &Artifact : State.Artifacts) {
    TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetStringField(TEXT("artifact_id"), Artifact.ArtifactId);
    Object->SetStringField(TEXT("kind"), Artifact.Kind == EDurableArtifactKind::DirectoryTree ? TEXT("directory_tree") : TEXT("file"));
    Object->SetStringField(TEXT("final_path"), Artifact.FinalPath);
    Object->SetStringField(TEXT("staging_path"), Artifact.StagingPath);
    Object->SetStringField(TEXT("backup_path"), Artifact.BackupPath);
    Object->SetStringField(TEXT("quarantine_path"),
                           Artifact.QuarantinePath);
    Object->SetBoolField(TEXT("old_exists"), Artifact.bOldExists);
    Object->SetBoolField(TEXT("staged_exists"), Artifact.bStagedExists);
    Object->SetStringField(TEXT("staged_hash"), Artifact.StagedHash);
    if (!Artifact.OldHash.IsEmpty())
      Object->SetStringField(TEXT("old_hash"), Artifact.OldHash);
    if (!Artifact.MemoryCheckpointPath.IsEmpty()) {
      Object->SetStringField(TEXT("memory_checkpoint_path"), Artifact.MemoryCheckpointPath);
      Object->SetStringField(TEXT("memory_checkpoint_hash"), Artifact.MemoryCheckpointHash);
      Object->SetBoolField(TEXT("memory_checkpoint_exists"), Artifact.bMemoryCheckpointExists);
    }
    ArtifactValues.Add(MakeShared<FJsonValueObject>(Object));
  }
  Root->SetArrayField(TEXT("artifacts"), ArtifactValues);
  return Root;
}

bool JsonToJournal(const FString &JournalPath,
                   const TSharedRef<FJsonObject> &Root,
                   FDurableJournalState &OutState, UE::UAI::Contract::FDiagnostic& OutError,
                   const bool bRequireJournalWithinTransactionRoot = true) {
  const auto Fail = [&](const TCHAR* Code, const FString& Reason, const FString& Path,
                        const FString& Expected, const FString& Actual) {
    OutError = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("workspace_durable_transaction"), JournalPath);
    OutError.SourceKind = TEXT("native_result");
    OutError.PropertyPath = Path;
    OutError.ExpectedValue = MakeShared<FJsonValueString>(Expected);
    OutError.ActualValue = MakeShared<FJsonValueString>(Actual);
    return false;
  };
  const auto ReadField = [&](const FJsonObject& Object, const TCHAR* Name, EJson Type,
                             const FString& Path, bool bRequired) {
    const auto* Value = Object.Values.Find(Name);
    if (!Value && !bRequired) return true;
    if (Value && Value->IsValid() && (*Value)->Type == Type) return true;
    Fail(TEXT("durable_journal_field_invalid"), TEXT("A durable journal field is missing or has the wrong JSON type."),
        Path, Type == EJson::String ? TEXT("string") : Type == EJson::Boolean ? TEXT("boolean") : TEXT("array"),
        !Value ? TEXT("missing") : !Value->IsValid() ? TEXT("invalid JSON value") :
        (*Value)->Type == EJson::Null ? TEXT("null") : (*Value)->Type == EJson::String ? TEXT("string") :
        (*Value)->Type == EJson::Number ? TEXT("number") : (*Value)->Type == EJson::Boolean ? TEXT("boolean") :
        (*Value)->Type == EJson::Array ? TEXT("array") : TEXT("object"));
    OutError.ExpectedValuePresent = true;
    OutError.ActualValuePresent = Value != nullptr;
    if (Value) OutError.ActualEvidence = *Value;
    return false;
  };
  const auto ReadString = [&](const FJsonObject& Object, const TCHAR* Name, FString& Value,
                              const FString& Path, bool bRequired = true) {
    if (!ReadField(Object, Name, EJson::String, Path, bRequired)) return false;
    Object.TryGetStringField(Name, Value);
    return true;
  };
  const auto ReadBool = [&](const FJsonObject& Object, const TCHAR* Name, bool& Value,
                            const FString& Path, bool bRequired = true) {
    if (!ReadField(Object, Name, EJson::Boolean, Path, bRequired)) return false;
    Object.TryGetBoolField(Name, Value);
    return true;
  };
  FString Schema;
  if (!ReadString(*Root, TEXT("schema"), Schema, TEXT("/schema"))) return false;
  if (Schema != JournalSchema)
    return Fail(TEXT("durable_journal_header_invalid"), TEXT("The durable journal schema is unsupported."), TEXT("/schema"), JournalSchema, Schema);
  if (!ReadString(*Root, TEXT("request_id"), OutState.RequestId, TEXT("/request_id")) ||
      !ReadString(*Root, TEXT("request_key"), OutState.RequestKey, TEXT("/request_key")) ||
      !ReadString(*Root, TEXT("request_fingerprint"), OutState.RequestFingerprint, TEXT("/request_fingerprint")) ||
      !ReadString(*Root, TEXT("transaction_root"), OutState.TransactionRoot, TEXT("/transaction_root")) ||
      !ReadString(*Root, TEXT("phase"), OutState.Phase, TEXT("/phase"))) return false;
  if (OutState.RequestId.IsEmpty())
    return Fail(TEXT("durable_journal_identity_invalid"), TEXT("The durable journal request ID is empty."), TEXT("/request_id"), TEXT("nonempty string"), TEXT("empty string"));
  const FString ExpectedKey = FMD5::HashAnsiString(*OutState.RequestId).ToLower();
  if (OutState.RequestKey != ExpectedKey) {
    Fail(TEXT("durable_journal_identity_invalid"), TEXT("The durable journal request key does not match its request ID."), TEXT("/request_key"), TEXT("key derived from request_id"), TEXT("different key"));
    OutError.ExpectedEvidence = MakeShared<FJsonValueString>(ExpectedKey);
    OutError.ActualEvidence = MakeShared<FJsonValueString>(OutState.RequestKey);
    return false;
  }
  if (FPaths::IsRelative(OutState.TransactionRoot)) {
    Fail(TEXT("durable_journal_identity_invalid"), TEXT("The durable journal transaction root is not absolute."), TEXT("/transaction_root"), TEXT("absolute path"), TEXT("relative path"));
    OutError.ActualEvidence = MakeShared<FJsonValueString>(OutState.TransactionRoot);
    return false;
  }
  OutState.TransactionRoot = NormalizeAbsolutePath(OutState.TransactionRoot);
  OutState.JournalPath = NormalizeAbsolutePath(JournalPath);
  if (bRequireJournalWithinTransactionRoot &&
      (!IsWithinRoot(OutState.TransactionRoot, OutState.JournalPath) ||
       NormalizeAbsolutePath(FPaths::GetPath(OutState.JournalPath)) != OutState.TransactionRoot))
    return Fail(TEXT("durable_journal_path_escape"), TEXT("The active journal is not directly inside its recorded transaction directory."),
        TEXT("/transaction_root"), TEXT("journal parent equals transaction root"), TEXT("different parent"));
  if (!ReadField(*Root, TEXT("phase_history"), EJson::Array, TEXT("/phase_history"), true)) return false;
  const auto& History = Root->GetArrayField(TEXT("phase_history"));
  for (int32 Index = 0; Index < History.Num(); ++Index) {
    FString Phase;
    if (!History[Index].IsValid() || !History[Index]->TryGetString(Phase)) {
      Fail(TEXT("durable_journal_history_invalid"), TEXT("A durable journal phase-history entry is not a string."),
          FString::Printf(TEXT("/phase_history/%d"), Index), TEXT("string"), TEXT("non-string value"));
      OutError.ActualEvidence = History[Index];
      return false;
    }
    OutState.PhaseHistory.Add(Phase);
  }
  if (OutState.PhaseHistory.IsEmpty() || OutState.PhaseHistory.Last() != OutState.Phase)
    return Fail(TEXT("durable_journal_phase_mismatch"), TEXT("The last phase-history entry must match the current journal phase."),
        TEXT("/phase_history"), OutState.Phase, OutState.PhaseHistory.IsEmpty() ? FString(TEXT("empty history")) : OutState.PhaseHistory.Last());
  if (!ReadField(*Root, TEXT("artifacts"), EJson::Array, TEXT("/artifacts"), true)) return false;
  const auto& Artifacts = Root->GetArrayField(TEXT("artifacts"));
  TSet<FString> ArtifactIds;
  TSet<FString> FinalPaths;
  for (int32 Index = 0; Index < Artifacts.Num(); ++Index) {
    const FString Prefix = FString::Printf(TEXT("/artifacts/%d"), Index);
    const auto& Value = Artifacts[Index];
    const TSharedPtr<FJsonObject> Object = Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
    if (!Object.IsValid()) {
      Fail(TEXT("durable_journal_artifact_invalid"), TEXT("A durable journal artifact entry is not an object."), Prefix, TEXT("object"), TEXT("non-object value"));
      OutError.ActualEvidence = Value;
      return false;
    }
    FDurableArtifactState &Artifact = OutState.Artifacts.AddDefaulted_GetRef();
    FString Kind = TEXT("file");
    if (!ReadString(*Object, TEXT("kind"), Kind, Prefix / TEXT("kind"), false)) return false;
    if (Kind != TEXT("file") && Kind != TEXT("directory_tree"))
      return Fail(TEXT("durable_journal_artifact_kind_invalid"), TEXT("The journal artifact kind is unsupported."), Prefix / TEXT("kind"), TEXT("file or directory_tree"), Kind);
    Artifact.Kind = Kind == TEXT("directory_tree") ? EDurableArtifactKind::DirectoryTree : EDurableArtifactKind::File;
    if (!ReadString(*Object, TEXT("artifact_id"), Artifact.ArtifactId, Prefix / TEXT("artifact_id")) ||
        !ReadString(*Object, TEXT("final_path"), Artifact.FinalPath, Prefix / TEXT("final_path")) ||
        !ReadString(*Object, TEXT("staging_path"), Artifact.StagingPath, Prefix / TEXT("staging_path")) ||
        !ReadString(*Object, TEXT("backup_path"), Artifact.BackupPath, Prefix / TEXT("backup_path")) ||
        !ReadString(*Object, TEXT("quarantine_path"), Artifact.QuarantinePath, Prefix / TEXT("quarantine_path")) ||
        !ReadBool(*Object, TEXT("old_exists"), Artifact.bOldExists, Prefix / TEXT("old_exists")) ||
        !ReadBool(*Object, TEXT("staged_exists"), Artifact.bStagedExists, Prefix / TEXT("staged_exists")) ||
        !ReadString(*Object, TEXT("old_hash"), Artifact.OldHash, Prefix / TEXT("old_hash"), false) ||
        !ReadString(*Object, TEXT("staged_hash"), Artifact.StagedHash, Prefix / TEXT("staged_hash"), false) ||
        !ReadString(*Object, TEXT("memory_checkpoint_path"), Artifact.MemoryCheckpointPath, Prefix / TEXT("memory_checkpoint_path"), false) ||
        !ReadString(*Object, TEXT("memory_checkpoint_hash"), Artifact.MemoryCheckpointHash, Prefix / TEXT("memory_checkpoint_hash"), false) ||
        !ReadBool(*Object, TEXT("memory_checkpoint_exists"), Artifact.bMemoryCheckpointExists, Prefix / TEXT("memory_checkpoint_exists"), false)) return false;
    const auto PathFailure = [&](const TCHAR* Code, const FString& Reason, const TCHAR* Field, const FString& Path,
                                 const FString& Expected, const FString& Actual) {
      Fail(Code, Reason, Prefix / Field, Expected, Actual);
      OutError.SubjectId = Artifact.ArtifactId;
      OutError.ActualEvidence = MakeShared<FJsonValueString>(Path);
      return false;
    };
    if (!Artifact.MemoryCheckpointPath.IsEmpty() &&
        (FPaths::IsRelative(Artifact.MemoryCheckpointPath) || !IsWithinRoot(OutState.TransactionRoot, Artifact.MemoryCheckpointPath)))
      return PathFailure(TEXT("durable_memory_checkpoint_path_invalid"), TEXT("The memory checkpoint must be an absolute path inside this transaction directory."),
          TEXT("memory_checkpoint_path"), Artifact.MemoryCheckpointPath, TEXT("absolute path within transaction root"), TEXT("relative or outside transaction root"));
    if (Artifact.ArtifactId.IsEmpty())
      return Fail(TEXT("durable_journal_artifact_identity_invalid"), TEXT("The durable artifact ID is empty."), Prefix / TEXT("artifact_id"), TEXT("nonempty string"), TEXT("empty string"));
    for (const auto& Entry : {TPair<const TCHAR*, FString*>(TEXT("final_path"), &Artifact.FinalPath),
        {TEXT("staging_path"), &Artifact.StagingPath}, {TEXT("backup_path"), &Artifact.BackupPath}, {TEXT("quarantine_path"), &Artifact.QuarantinePath}}) {
      if (FPaths::IsRelative(*Entry.Value))
        return PathFailure(TEXT("durable_journal_artifact_identity_invalid"), TEXT("A durable artifact path is not absolute."), Entry.Key, *Entry.Value, TEXT("absolute path"), TEXT("relative path"));
      *Entry.Value = NormalizeAbsolutePath(*Entry.Value);
    }
    const FString FoldedId = Artifact.ArtifactId.ToLower();
    const FString FoldedPath = Artifact.FinalPath.ToLower();
    if (ArtifactIds.Contains(FoldedId))
      return PathFailure(TEXT("durable_journal_artifact_path_invalid"), TEXT("The journal repeats an artifact ID (case-insensitive comparison)."), TEXT("artifact_id"), Artifact.ArtifactId, TEXT("unique artifact ID"), TEXT("duplicate ID"));
    if (FinalPaths.Contains(FoldedPath))
      return PathFailure(TEXT("durable_journal_artifact_path_invalid"), TEXT("The journal repeats a final artifact path (case-insensitive comparison)."), TEXT("final_path"), Artifact.FinalPath, TEXT("unique final path"), TEXT("duplicate path"));
    if (IsWithinRoot(OutState.TransactionRoot, Artifact.FinalPath) || IsWithinRoot(Artifact.FinalPath, OutState.TransactionRoot))
      return PathFailure(TEXT("durable_journal_artifact_path_invalid"), TEXT("The final artifact and transaction directory must not contain each other."), TEXT("final_path"), Artifact.FinalPath, TEXT("disjoint from transaction root"), TEXT("overlapping paths"));
    for (const auto& Entry : {TPair<const TCHAR*, const FString*>(TEXT("staging_path"), &Artifact.StagingPath),
        {TEXT("backup_path"), &Artifact.BackupPath}, {TEXT("quarantine_path"), &Artifact.QuarantinePath}})
      if (!IsWithinRoot(OutState.TransactionRoot, *Entry.Value))
        return PathFailure(TEXT("durable_journal_artifact_path_invalid"), TEXT("The artifact's transaction storage path escapes the transaction directory."), Entry.Key, *Entry.Value, TEXT("within transaction root"), TEXT("outside transaction root"));
    ArtifactIds.Add(FoldedId);
    for (const auto& Existing : FinalPaths) {
      if (IsWithinRoot(Existing, FoldedPath) || IsWithinRoot(FoldedPath, Existing)) {
        PathFailure(TEXT("durable_journal_artifact_overlap"), TEXT("Two final artifact paths overlap by containment."), TEXT("final_path"), Artifact.FinalPath, TEXT("disjoint final artifact paths"), TEXT("overlapping paths"));
        OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Existing);
        return false;
      }
    }
    FinalPaths.Add(FoldedPath);
  }
  return true;
}

bool WriteJournal(const FDurableJournalState &State, UE::UAI::Contract::FDiagnostic& OutError,
                  const FString &OverridePath = FString()) {
  FString Text;
  if (!SerializeJson(JournalToJson(State), Text)) {
    OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_journal_serialize_failed"), TEXT("Serializing the durable transaction journal as JSON failed."), TEXT("workspace_durable_transaction"));
    return false;
  }
  return SaveDurableText(
      OverridePath.IsEmpty() ? State.JournalPath : OverridePath, Text,
      OutError);
}

void RefreshPublishedArtifacts(const FDurableJournalState& State) {
  TArray<FString> Files;
  for (const auto& Artifact : State.Artifacts) {
    if (Artifact.Kind == EDurableArtifactKind::File) {
      Files.Add(Artifact.FinalPath);
      continue;
    }
    // The union includes removed files as well as newly published ones. Only
    // this transaction's declared directory artifacts are inspected.
    for (const FString& Root : {Artifact.FinalPath, Artifact.BackupPath,
        Artifact.StagingPath, Artifact.MemoryCheckpointPath}) {
      if (Root.IsEmpty()) continue;
      TArray<FString> Found;
      IFileManager::Get().FindFilesRecursive(Found, *Root, TEXT("*"), true, false);
      for (FString File : Found) {
        if (FPaths::MakePathRelativeTo(File, *(Root + TEXT("/"))))
          Files.AddUnique(Artifact.FinalPath / File);
      }
    }
  }
  UE::UAI::AssetDurable::RefreshPublishedPackageFiles(Files);
}

bool RestoreArtifacts(const FDurableJournalState &State, UE::UAI::Contract::FDiagnostic& OutError) {
  ON_SCOPE_EXIT { RefreshPublishedArtifacts(State); };
  for (const FDurableArtifactState &Artifact : State.Artifacts) {
    bool bFinalExists = false;
    FString FinalHash;
    if (!ArtifactSnapshot(Artifact.Kind, Artifact.FinalPath, bFinalExists, FinalHash, OutError)) return false;
    if (Artifact.bOldExists) {
      if (!VerifyArtifactSnapshot(Artifact.Kind, Artifact.BackupPath, true, Artifact.OldHash,
          TEXT("durable_restore_backup_hash_mismatch"), TEXT("The rollback backup is missing or differs from the original artifact."),
          Artifact.ArtifactId, OutError, !Artifact.OldHash.IsEmpty())) return false;
      const bool bFinalAlreadyRestored = !Artifact.OldHash.IsEmpty() && bFinalExists && FinalHash == Artifact.OldHash;
      if (!bFinalAlreadyRestored && !CopyArtifact(Artifact.Kind, Artifact.FinalPath, Artifact.BackupPath, OutError)) {
        OutError = MakeDurableFailure(TEXT("durable_restore_copy_failed"), TEXT("Restoring the final artifact from its rollback backup failed."), OutError, Artifact.ArtifactId);
        return false;
      }
      if (!VerifyArtifactSnapshot(Artifact.Kind, Artifact.FinalPath, true, Artifact.OldHash,
          TEXT("durable_restore_hash_mismatch"), TEXT("The restored final artifact is missing or differs from its recorded original."),
          Artifact.ArtifactId, OutError, !Artifact.OldHash.IsEmpty())) return false;
    } else if (bFinalExists && !RemoveArtifact(Artifact.Kind, Artifact.FinalPath, OutError)) {
      OutError = MakeDurableFailure(TEXT("durable_restore_delete_failed"), TEXT("Removing an artifact that did not exist before the transaction failed."), OutError, Artifact.ArtifactId);
      return false;
    }
  }
  return true;
}

bool FinalArtifactsMatchOriginal(const FDurableJournalState &State,
                                 UE::UAI::Contract::FDiagnostic& OutError) {
  for (const FDurableArtifactState &Artifact : State.Artifacts) {
    if (!VerifyArtifactSnapshot(Artifact.Kind, Artifact.FinalPath, Artifact.bOldExists, Artifact.OldHash,
        TEXT("durable_stage_modified_final"), TEXT("The final artifact no longer matches its pre-transaction presence or content."),
        Artifact.ArtifactId, OutError)) return false;
  }
  return true;
}

bool AppendPhaseAndWrite(FDurableJournalState &State, const FString &Phase,
                         UE::UAI::Contract::FDiagnostic& OutError) {
  State.Phase = Phase;
  if (State.PhaseHistory.IsEmpty() || State.PhaseHistory.Last() != Phase)
    State.PhaseHistory.Add(Phase);
  return WriteJournal(State, OutError);
}

void SyncResult(const FDurableJournalState &State,
                FDurabilityResult &OutResult) {
  OutResult.JournalSchema = JournalSchema;
  OutResult.JournalPhase = State.Phase;
  OutResult.JournalPath = State.JournalPath;
  OutResult.PhaseHistory = State.PhaseHistory;
  OutResult.ArtifactCount = State.Artifacts.Num();
}

FString CompletedPathFor(const FString &JournalRoot,
                         const FString &RequestKey) {
  return NormalizeAbsolutePath(JournalRoot) / TEXT("completed") /
         (RequestKey + TEXT(".json"));
}

FString RequestKeyFor(const FString &RequestId) {
  return FMD5::HashAnsiString(*RequestId).ToLower();
}

} // namespace

namespace UeAgentAuthoringWorkspaceTransaction {

FScopedDurableRecoveryNotice::FScopedDurableRecoveryNotice()
    : Previous(CurrentRecoveryNotice) {
  CurrentRecoveryNotice = this;
}

FScopedDurableRecoveryNotice::~FScopedDurableRecoveryNotice() {
  if (Previous) Previous->bMemoryEvidencePreserved |= bMemoryEvidencePreserved;
  CurrentRecoveryNotice = Previous;
}

struct FDurableTransactionCoordinator::FImpl {
  FDurableJournalState State;
  TArray<FDurableArtifact> PublicArtifacts;
  FOperationFailure InventoryFailure;
  bool bPrepared = false;
};

FString BuildDurableRequestFingerprint(
    const UeAgentApplyWorkspaceContract::FContract &Contract,
    const UeAgentAuthoringWorkspaceRouter::FWorkspace &Workspace,
    const UeAgentAuthoringWorkspaceDependencyGraph::FDependencyGraph &Graph,
    const FPreflightResult &Prepared) {
  FString Material = Contract.ContractId + TEXT("\n") +
                     Contract.ContractVersion + TEXT("\n");
  for (int32 UnitIndex : Graph.OrderedUnitIndices) {
    if (!Workspace.Units.IsValidIndex(UnitIndex))
      continue;
    const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit &Unit =
        Workspace.Units[UnitIndex];
    Material += Unit.CanonicalUnitKey + TEXT("|") + Unit.RelativePath +
                TEXT("|") + FString::FromInt(static_cast<int32>(Unit.Action)) +
                TEXT("|");
    if (Unit.Document.IsValid()) {
      FString DocumentText;
      const TSharedPtr<FJsonObject> MutableDocument =
          ConstCastSharedPtr<FJsonObject>(Unit.Document);
      SerializeJson(MutableDocument.ToSharedRef(), DocumentText);
      Material += DocumentText;
    }
    Material += TEXT("\n");
  }
  for (const FPreparedUnit &Unit : Prepared.Units) {
    if (!Unit.Mutation.IsValid())
      continue;
    Material += Unit.Mutation->GetBackendId() + TEXT("|") +
                Unit.Mutation->GetUnitKey() + TEXT("|") +
                Unit.Mutation->GetTargetKey() + TEXT("\n");
  }
  return FMD5::HashAnsiString(*Material).ToLower();
}

FDurableTransactionCoordinator::FDurableTransactionCoordinator()
    : Impl(MakeUnique<FImpl>()) {}

FDurableTransactionCoordinator::~FDurableTransactionCoordinator() = default;

bool FDurableTransactionCoordinator::RecoverPending(
    const FExecutionOptions &Options, FDurabilityResult &InOutResult,
    FOperationFailure &OutFailure) {
  OutFailure = FOperationFailure();
  const FString JournalRoot = NormalizeAbsolutePath(Options.JournalRoot);
  const FString ActiveRoot = JournalRoot / TEXT("active");
  InOutResult.JournalSchema = JournalSchema;
  InOutResult.RecoveryStatus = TEXT("none");
  InOutResult.PhaseHistory.Add(TEXT("recovery_checked"));
  if (!IFileManager::Get().DirectoryExists(*ActiveRoot))
    return true;

  TArray<FString> JournalFiles;
  IFileManager::Get().FindFilesRecursive(JournalFiles, *ActiveRoot,
                                         JournalFileName, true, false);
  JournalFiles.Sort();
  bool bRecoveredAny = false;
  bool bMemoryEvidencePreserved = false;
  for (const FString &JournalPath : JournalFiles) {
    TSharedPtr<FJsonObject> Object;
    FDurableJournalState State;
    UE::UAI::Contract::FDiagnostic Error;
    if (!LoadJson(JournalPath, Object, Error) ||
        !JsonToJournal(JournalPath, Object.ToSharedRef(), State, Error)) {
      SetFailure(OutFailure, MakeDurableFailure(TEXT("durable_recovery_journal_invalid"), TEXT("The pending transaction journal cannot be used for recovery."), Error, JournalPath));
      InOutResult.RecoveryStatus = TEXT("failed");
      return false;
    }
    if (State.Phase == TEXT("committed") || State.Phase == TEXT("restored") || State.Phase == TEXT("memory_complete")) {
      CleanupCompletedTransaction(State, InOutResult);
      continue;
    }
    if (State.Phase == TEXT("disk_restored_memory_preserved")) {
      // This transaction is no longer pending publication. Keep both before-
      // images, but never overwrite subsequent edits with its disk backup.
      // Its recovery was already reported when this phase was written. Merely
      // retaining historical evidence is not a recovery event for this write.
      continue;
    }
    if (!RestoreArtifacts(State, Error)) {
      SetFailure(OutFailure, MakeDurableFailure(TEXT("durable_startup_restore_failed"), TEXT("Restoring a pending transaction during startup failed."), Error));
      InOutResult.RecoveryStatus = TEXT("failed");
      return false;
    }
    if (State.Artifacts.ContainsByPredicate([](const FDurableArtifactState &Artifact) {
          if (Artifact.MemoryCheckpointPath.IsEmpty()) return false;
          if (Artifact.bMemoryCheckpointExists != Artifact.bOldExists) return true;
          return Artifact.bMemoryCheckpointExists &&
                 (Artifact.MemoryCheckpointHash.IsEmpty() ||
                  Artifact.MemoryCheckpointHash != Artifact.OldHash);
        })) {
      // Disk recovery is confirmed; memory recovery is not. Retain the complete
      // journal and checkpoint instead of treating unrelated future writes as
      // confirmation or repeatedly restoring this transaction's old disk image.
      if (!AppendPhaseAndWrite(State, TEXT("disk_restored_memory_preserved"), Error)) {
        SetFailure(OutFailure, MakeDurableFailure(TEXT("durable_recovery_journal_write_failed"), TEXT("Recording the recovered transaction state failed."), Error));
        InOutResult.RecoveryStatus = TEXT("failed");
        return false;
      }
      bMemoryEvidencePreserved = true;
      continue;
    }
    if (!AppendPhaseAndWrite(State, TEXT("startup_restored"), Error)) {
      SetFailure(OutFailure, MakeDurableFailure(TEXT("durable_recovery_journal_write_failed"), TEXT("Recording the recovered transaction state failed."), Error));
      InOutResult.RecoveryStatus = TEXT("failed");
      return false;
    }
    if (!IFileManager::Get().DeleteDirectory(*State.TransactionRoot, false,
                                             true)) {
      SetFailure(OutFailure, UE::UAI::FileDiagnostics::MakeFileOperationFailure(TEXT("durable_recovery_cleanup_failed"), TEXT("Removing the recovered transaction storage failed."), TEXT("workspace_durable_transaction"), State.TransactionRoot));
      InOutResult.RecoveryStatus = TEXT("failed");
      return false;
    }
    bRecoveredAny = true;
  }
  if (bRecoveredAny) {
    InOutResult.RecoveryStatus = TEXT("restored");
    InOutResult.PhaseHistory.Add(TEXT("startup_restored"));
  }
  if (bMemoryEvidencePreserved) {
    if (CurrentRecoveryNotice) CurrentRecoveryNotice->bMemoryEvidencePreserved = true;
    InOutResult.RecoveryStatus = TEXT("disk_restored_memory_preserved");
    InOutResult.PhaseHistory.Add(TEXT("disk_restored_memory_preserved"));
  }
  return true;
}

bool FDurableTransactionCoordinator::TryReplay(
    const FExecutionOptions &Options, const FString &RequestFingerprint,
    bool &OutReplayed, FDurabilityResult &InOutResult,
    FOperationFailure &OutFailure) {
  OutReplayed = false;
  OutFailure = FOperationFailure();
  const FString RequestKey = RequestKeyFor(Options.RequestId);
  const FString CompletedPath =
      CompletedPathFor(Options.JournalRoot, RequestKey);
  if (!IFileManager::Get().FileExists(*CompletedPath))
    return true;

  TSharedPtr<FJsonObject> Object;
  FDurableJournalState State;
  UE::UAI::Contract::FDiagnostic Error;
  if (!LoadJson(CompletedPath, Object, Error) ||
      !JsonToJournal(CompletedPath, Object.ToSharedRef(), State, Error,
                     false)) {
    SetFailure(OutFailure, MakeDurableFailure(TEXT("durable_replay_receipt_invalid"), TEXT("The completed transaction receipt cannot be replayed."), Error, CompletedPath));
    return false;
  }
  if (State.RequestId != Options.RequestId || State.RequestFingerprint != RequestFingerprint) {
    const bool bIdMismatch = State.RequestId != Options.RequestId;
    auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_replay_identity_mismatch"),
        bIdMismatch ? TEXT("The completed receipt belongs to a different request ID.") : TEXT("This request ID was previously committed with different inputs."),
        TEXT("workspace_durable_transaction"), CompletedPath, Options.RequestId);
    Diagnostic.PropertyPath = bIdMismatch ? TEXT("request_id") : TEXT("request_fingerprint");
    Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("matches the current request"));
    Diagnostic.ActualValue = MakeShared<FJsonValueString>(TEXT("different request identity or inputs"));
    Diagnostic.ExpectedEvidence = MakeShared<FJsonValueString>(bIdMismatch ? Options.RequestId : RequestFingerprint);
    Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(bIdMismatch ? State.RequestId : State.RequestFingerprint);
    SetFailure(OutFailure, MoveTemp(Diagnostic));
    return false;
  }
  if (State.Phase != TEXT("committed")) {
    SetFailure(OutFailure, StateFailure(TEXT("durable_replay_identity_mismatch"),
        TEXT("Only a committed transaction receipt can be replayed."), true, State.Phase, TEXT("committed")));
    return false;
  }
  State.JournalPath = CompletedPath;
  State.PhaseHistory.Add(TEXT("replayed"));
  SyncResult(State, InOutResult);
  InOutResult.bReplayed = true;
  OutReplayed = true;
  return true;
}

bool FDurableTransactionCoordinator::Prepare(
    const FExecutionOptions &Options, const FString &RequestFingerprint,
    ITransactionBackend &Backend,
    const TArray<IPreparedMutation *> &Mutations,
    FDurabilityResult &InOutResult, FOperationFailure &OutFailure) {
  OutFailure = FOperationFailure();
  Impl->bPrepared = false;
  if (!Backend.SupportsDurableTransactions()) {
    SetFailure(OutFailure, UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_backend_not_supported"), TEXT("The transaction backend does not support durable transactions."), TEXT("workspace_durable_transaction")));
    return false;
  }
  TArray<FDurableArtifact> Artifacts;
  if (!Backend.BuildDurableArtifactInventory(
          Mutations, Artifacts, OutFailure)) {
    if (OutFailure.Diagnostic.Code.IsEmpty())
      SetFailure(OutFailure, UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_inventory_failed"), TEXT("The backend failed to build its durable artifact inventory without reporting a diagnostic."), TEXT("workspace_durable_transaction")));
    OutFailure.Diagnostic.bRetrySafe = false;
    return false;
  }
  return Prepare(Options, RequestFingerprint, MoveTemp(Artifacts), InOutResult, OutFailure);
}

bool FDurableTransactionCoordinator::Prepare(
    const FExecutionOptions &Options, const FString &RequestFingerprint,
    TArray<FDurableArtifact> Artifacts,
    FDurabilityResult &InOutResult, FOperationFailure &OutFailure) {
  OutFailure = FOperationFailure();
  Impl->bPrepared = false;
  Impl->PublicArtifacts.Reset();
  Impl->InventoryFailure = FOperationFailure();
  if (Artifacts.IsEmpty()) {
    SetFailure(OutFailure, UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_inventory_empty"), TEXT("The durable artifact inventory is empty; at least one artifact is required."), TEXT("workspace_durable_transaction")));
    return false;
  }

  FDurableJournalState &State = Impl->State;
  State = FDurableJournalState();
  State.RequestId = Options.RequestId;
  State.RequestKey = RequestKeyFor(Options.RequestId);
  State.RequestFingerprint = RequestFingerprint;
  const FString JournalRoot = NormalizeAbsolutePath(Options.JournalRoot);
  State.TransactionRoot = JournalRoot / TEXT("active") / State.RequestKey;
  if (IFileManager::Get().DirectoryExists(*State.TransactionRoot)) {
    // A retry may reuse a request whose unconfirmed memory evidence is retained.
    // Its backups must never become staging storage for the new transaction.
    State.TransactionRoot += TEXT("-") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
  }
  State.JournalPath = State.TransactionRoot / JournalFileName;
  State.CompletedPath = CompletedPathFor(JournalRoot, State.RequestKey);
  State.PhaseHistory = InOutResult.PhaseHistory;

  UE::UAI::Contract::FDiagnostic Error;
  if (!IFileManager::Get().MakeDirectory(*State.TransactionRoot, true)) {
    SetFailure(OutFailure, UE::UAI::FileDiagnostics::MakeFileOperationFailure(TEXT("durable_transaction_root_unavailable"), TEXT("Creating the durable transaction storage directory failed."), TEXT("workspace_durable_transaction"), State.TransactionRoot)); return false;
  }
  const auto CleanupFailedPrepare = [&]() {
    if (!IFileManager::Get().DeleteDirectory(*State.TransactionRoot, false, true)) {
      auto CleanupFailure = UE::UAI::FileDiagnostics::MakeFileOperationFailure(
          TEXT("durable_prepare_cleanup_failed"), TEXT("The incomplete transaction staging area could not be removed."),
          TEXT("workspace_durable_cleanup"), State.TransactionRoot);
      OutFailure.Diagnostic.Append(CleanupFailure);
      OutFailure.Diagnostic.bRetrySafe = false;
    }
  };
  if (!EnlistArtifacts(MoveTemp(Artifacts), false, OutFailure)) {
    CleanupFailedPrepare();
    return false;
  }
  State.PhaseHistory.Add(TEXT("inventory_ready"));
  if (!AppendPhaseAndWrite(State, TEXT("prepared"), Error)) {
    SetFailure(OutFailure, MakeDurableFailure(TEXT("durable_journal_prepare_failed"), TEXT("Recording the prepared transaction and its artifact inventory failed."), Error));
    CleanupFailedPrepare();
    return false;
  }
  Impl->bPrepared = true;
  SyncResult(State, InOutResult);
  return true;
}

bool FDurableTransactionCoordinator::EnlistArtifacts(
    TArray<FDurableArtifact> Artifacts, bool bMemoryBeforeFromDisk,
    FOperationFailure& OutFailure) {
  auto& State = Impl->State;
  TSet<FString> ArtifactIds, FinalPaths;
  for (const auto& Existing : State.Artifacts) {
    ArtifactIds.Add(Existing.ArtifactId.ToLower());
    FinalPaths.Add(Existing.FinalPath.ToLower());
  }
  TArray<FDurableArtifactState> Added;
  UE::UAI::Contract::FDiagnostic Error;
  for (auto& PublicArtifact : Artifacts) {
    PublicArtifact.ArtifactId.TrimStartAndEndInline();
    const auto FailArtifact = [&](const TCHAR* Code, const FString& Reason, const TCHAR* Field,
                                  const FString& Expected, const FString& Actual, const FString& Evidence) {
      auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("workspace_durable_transaction"), FString(), PublicArtifact.ArtifactId);
      Diagnostic.PropertyPath = FString::Printf(TEXT("/artifacts/%d/%s"), State.Artifacts.Num() + Added.Num(), Field);
      Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(Expected);
      Diagnostic.ActualValue = MakeShared<FJsonValueString>(Actual);
      Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(Evidence);
      SetFailure(OutFailure, MoveTemp(Diagnostic));
      return false;
    };
    if (PublicArtifact.ArtifactId.IsEmpty())
      return FailArtifact(TEXT("durable_artifact_identity_invalid"), TEXT("The durable artifact ID is empty."), TEXT("artifact_id"), TEXT("nonempty string"), TEXT("empty string"), PublicArtifact.ArtifactId);
    if (PublicArtifact.FinalPath.IsEmpty() || FPaths::IsRelative(PublicArtifact.FinalPath))
      return FailArtifact(TEXT("durable_artifact_identity_invalid"), TEXT("The final durable artifact path must be nonempty and absolute."), TEXT("final_path"), TEXT("nonempty absolute path"), PublicArtifact.FinalPath.IsEmpty() ? TEXT("empty path") : TEXT("relative path"), PublicArtifact.FinalPath);
    PublicArtifact.FinalPath = NormalizeAbsolutePath(PublicArtifact.FinalPath);
    const FString Id = PublicArtifact.ArtifactId.ToLower(), Path = PublicArtifact.FinalPath.ToLower();
    if (ArtifactIds.Contains(Id) || FinalPaths.Contains(Path)) {
      const bool bDuplicateId = ArtifactIds.Contains(Id);
      return FailArtifact(TEXT("durable_artifact_duplicate"), bDuplicateId ? TEXT("The durable inventory repeats an artifact ID.") : TEXT("The durable inventory repeats a final path."),
          bDuplicateId ? TEXT("artifact_id") : TEXT("final_path"), TEXT("unique value (case-insensitive)"), TEXT("duplicate value"), bDuplicateId ? PublicArtifact.ArtifactId : PublicArtifact.FinalPath);
    }
    // JournalRoot is the parent of active/<request>.
    const FString JournalRoot = FPaths::GetPath(FPaths::GetPath(State.TransactionRoot));
    if (IsWithinRoot(JournalRoot, Path) || IsWithinRoot(Path, JournalRoot)) {
      return FailArtifact(TEXT("durable_artifact_path_invalid"), TEXT("The final artifact and journal storage must not contain each other."), TEXT("final_path"), TEXT("disjoint from journal root"), TEXT("overlapping paths"), PublicArtifact.FinalPath);
    }
    for (const auto& Existing : FinalPaths) {
      if (IsWithinRoot(Existing, Path) || IsWithinRoot(Path, Existing)) {
        FailArtifact(TEXT("durable_artifact_overlap"), TEXT("Two final artifact paths overlap by containment."), TEXT("final_path"), TEXT("disjoint artifact paths"), TEXT("overlapping paths"), PublicArtifact.FinalPath);
        OutFailure.Diagnostic.ExpectedEvidence = MakeShared<FJsonValueString>(Existing);
        return false;
      }
    }
    ArtifactIds.Add(Id); FinalPaths.Add(Path);
    const FString Leaf = FString::Printf(TEXT("%04d.bin"), State.Artifacts.Num() + Added.Num());
    auto& Artifact = Added.AddDefaulted_GetRef();
    Artifact.Kind = PublicArtifact.Kind; Artifact.ArtifactId = PublicArtifact.ArtifactId;
    Artifact.FinalPath = PublicArtifact.FinalPath;
    Artifact.StagingPath = State.TransactionRoot / TEXT("stage") / Leaf;
    Artifact.BackupPath = State.TransactionRoot / TEXT("backup") / Leaf;
    Artifact.QuarantinePath = State.TransactionRoot / TEXT("quarantine") / Leaf;
    PublicArtifact.StagingPath = Artifact.StagingPath;
    if (!ArtifactSnapshot(Artifact.Kind, Artifact.FinalPath, Artifact.bOldExists, Artifact.OldHash, Error)) {
      SetFailure(OutFailure, MakeDurableFailure(TEXT("durable_artifact_preflight_failed"), TEXT("Reading the original durable artifact before mutation failed."), Error)); return false;
    }
  }
  for (auto& Artifact : Added) {
    if (Artifact.bOldExists) {
      if (!CopyArtifact(Artifact.Kind, Artifact.BackupPath, Artifact.FinalPath, Error)) {
        SetFailure(OutFailure, MakeDurableFailure(TEXT("durable_backup_failed"), TEXT("Creating the original artifact backup failed."), Error, Artifact.ArtifactId)); return false;
      }
      if (!VerifyArtifactSnapshot(Artifact.Kind, Artifact.BackupPath, true, Artifact.OldHash,
          TEXT("durable_backup_verification_failed"), TEXT("The artifact backup is missing or differs from its original."), Artifact.ArtifactId, Error)) {
        SetFailure(OutFailure, MakeDurableFailure(TEXT("durable_backup_verification_failed"), TEXT("The artifact backup could not be verified against its original."), Error)); return false;
      }
    }
    if (bMemoryBeforeFromDisk) {
      Artifact.MemoryCheckpointPath = State.TransactionRoot / TEXT("memory_before") / FPaths::GetCleanFilename(Artifact.StagingPath);
      Artifact.bMemoryCheckpointExists = Artifact.bOldExists;
      Artifact.MemoryCheckpointHash = Artifact.OldHash;
      if (Artifact.bOldExists && !CopyArtifact(Artifact.Kind, Artifact.MemoryCheckpointPath, Artifact.BackupPath, Error)) {
        SetFailure(OutFailure, MakeDurableFailure(TEXT("durable_memory_checkpoint_failed"), TEXT("Capturing or recording the pre-mutation memory checkpoint failed."), Error)); return false;
      }
    }
  }
  State.Artifacts.Append(MoveTemp(Added));
  Impl->PublicArtifacts.Append(MoveTemp(Artifacts));
  return true;
}

bool FDurableTransactionCoordinator::RefreshInventory(
    ITransactionBackend& Backend, FDurabilityResult& InOutResult, FOperationFailure& OutFailure) {
  if (!Impl->bPrepared) return true;
  // Discovery may already have consumed the backend's newly owned packages.
  // Never let a later empty inventory erase a failed enlistment on rollback.
  if (!Impl->InventoryFailure.Diagnostic.Code.IsEmpty()) {
    OutFailure = Impl->InventoryFailure;
    return false;
  }
  TArray<FDurableArtifact> Added;
  if (!Backend.CollectAdditionalDurableArtifacts(Added, OutFailure)) {
    if (OutFailure.Diagnostic.IsEmpty())
      SetFailure(OutFailure, UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_inventory_failed"),
          TEXT("The backend failed to collect additional artifacts without reporting a diagnostic."), TEXT("workspace_durable_transaction")));
    Impl->InventoryFailure = OutFailure;
    return false;
  }
  if (Added.IsEmpty()) return true;
  if (!EnlistArtifacts(MoveTemp(Added), true, OutFailure)) {
    Impl->InventoryFailure = OutFailure;
    return false;
  }
  UE::UAI::Contract::FDiagnostic Error;
  // Inventory growth changes journal contents, not the save/checkpoint phase.
  if (!WriteJournal(Impl->State, Error)) {
    SetFailure(OutFailure, MakeDurableFailure(TEXT("durable_journal_prepare_failed"), TEXT("Recording the prepared transaction and its artifact inventory failed."), Error));
    Impl->InventoryFailure = OutFailure;
    return false;
  }
  SyncResult(Impl->State, InOutResult);
  return true;
}

bool FDurableTransactionCoordinator::RecordNativePublication(
    FDurabilityResult &InOutResult, FOperationFailure &OutFailure) {
  OutFailure = FOperationFailure();
  if (!Impl->bPrepared || Impl->State.Phase != TEXT("prepared")) {
    SetFailure(OutFailure, StateFailure(TEXT("durable_native_publication_without_prepare"), TEXT("Recording native publication requires a prepared transaction."), Impl->bPrepared, Impl->State.Phase, TEXT("prepared")));
    return false;
  }
  UE::UAI::Contract::FDiagnostic Error;
  for (auto &Artifact : Impl->State.Artifacts) {
    if (!ArtifactSnapshot(Artifact.Kind, Artifact.FinalPath,
                          Artifact.bStagedExists, Artifact.StagedHash, Error)) {
      SetFailure(OutFailure, MakeDurableFailure(TEXT("durable_native_publication_snapshot_failed"), TEXT("Reading the artifact produced by native publication failed."), Error));
      return false;
    }
  }
  if (!AppendPhaseAndWrite(Impl->State, TEXT("published"), Error)) {
    SetFailure(OutFailure, MakeDurableFailure(TEXT("durable_journal_published_failed"), TEXT("Recording the published transaction state failed."), Error));
    SyncResult(Impl->State, InOutResult);
    return false;
  }
  SyncResult(Impl->State, InOutResult);
  return true;
}

bool FDurableTransactionCoordinator::Stage(
    ITransactionBackend &Backend, FDurabilityResult &InOutResult,
    FOperationFailure &OutFailure) {
  return Stage([&Backend](TArray<FDurableArtifact>& Artifacts, FOperationFailure& Failure) {
    return Backend.StageDurableArtifacts(Artifacts, Failure);
  }, InOutResult, OutFailure);
}

bool FDurableTransactionCoordinator::Stage(FStageSource Source,
    FDurabilityResult &InOutResult, FOperationFailure &OutFailure) {
  OutFailure = FOperationFailure();
  if (!Impl->bPrepared) {
    SetFailure(OutFailure, StateFailure(TEXT("durable_stage_without_prepare"), TEXT("Staging artifacts requires a prepared transaction."), Impl->bPrepared, Impl->State.Phase, TEXT("")));
    return false;
  }
  UE::UAI::Contract::FDiagnostic Error;
  if (!AppendPhaseAndWrite(Impl->State, TEXT("staging"), Error)) {
    SetFailure(OutFailure, MakeDurableFailure(TEXT("durable_journal_stage_failed"), TEXT("Recording the start of durable artifact staging failed."), Error));
    SyncResult(Impl->State, InOutResult);
    return false;
  }
  if (!Source(Impl->PublicArtifacts, OutFailure)) {
    if (OutFailure.Diagnostic.Code.IsEmpty())
      SetFailure(OutFailure, UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_stage_backend_failed"), TEXT("The backend failed to stage durable artifacts without reporting a diagnostic."), TEXT("workspace_durable_transaction")));
    OutFailure.Diagnostic.bRetrySafe = false;
    SyncResult(Impl->State, InOutResult);
    return false;
  }
  if (!FinalArtifactsMatchOriginal(Impl->State, Error)) {
    SetFailure(OutFailure, MakeDurableFailure(TEXT("durable_stage_modified_final"), TEXT("Staging changed a final artifact before publication."), Error));
    SyncResult(Impl->State, InOutResult);
    return false;
  }
  for (int32 Index = 0; Index < Impl->PublicArtifacts.Num(); ++Index) {
    const FDurableArtifact &PublicArtifact = Impl->PublicArtifacts[Index];
    FDurableArtifactState &Artifact = Impl->State.Artifacts[Index];
    if (NormalizeAbsolutePath(PublicArtifact.StagingPath) !=
        NormalizeAbsolutePath(Artifact.StagingPath) || PublicArtifact.Kind != Artifact.Kind) {
      auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_stage_path_changed"),
          PublicArtifact.Kind != Artifact.Kind ? TEXT("The backend changed the staged artifact kind.") : TEXT("The backend changed the assigned staging path."), TEXT("workspace_durable_transaction"), FString(), Artifact.ArtifactId);
      Diagnostic.PropertyPath = FString::Printf(TEXT("/artifacts/%d/%s"), Index, PublicArtifact.Kind != Artifact.Kind ? TEXT("kind") : TEXT("staging_path"));
      if (PublicArtifact.Kind != Artifact.Kind) {
        Diagnostic.ExpectedValue = MakeShared<FJsonValueNumber>(static_cast<int32>(Artifact.Kind));
        Diagnostic.ActualValue = MakeShared<FJsonValueNumber>(static_cast<int32>(PublicArtifact.Kind));
      } else {
        Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("assigned staging path"));
        Diagnostic.ActualValue = MakeShared<FJsonValueString>(TEXT("different path"));
        Diagnostic.ExpectedEvidence = MakeShared<FJsonValueString>(Artifact.StagingPath);
        Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(PublicArtifact.StagingPath);
      }
      SetFailure(OutFailure, MoveTemp(Diagnostic));
      SyncResult(Impl->State, InOutResult);
      return false;
    }
    Artifact.bStagedExists = PublicArtifact.bStagedExists;
    bool bExists;
    if (!ArtifactSnapshot(Artifact.Kind, Artifact.StagingPath, bExists, Artifact.StagedHash, Error)) {
      SetFailure(OutFailure, MakeDurableFailure(TEXT("durable_stage_artifact_missing"), TEXT("A declared staged artifact is missing or cannot be read."), Error, Artifact.ArtifactId));
      SyncResult(Impl->State, InOutResult);
      return false;
    }
    if (Artifact.bStagedExists && !bExists) {
      SetFailure(OutFailure, SnapshotMismatch(TEXT("durable_stage_artifact_missing"),
          TEXT("The backend declared a staged artifact but did not create it."), Artifact.ArtifactId, true, FString(), false, FString()));
      SyncResult(Impl->State, InOutResult); return false;
    }
  }
  if (!AppendPhaseAndWrite(Impl->State, TEXT("staged"), Error)) {
    SetFailure(OutFailure, MakeDurableFailure(TEXT("durable_journal_staged_failed"), TEXT("Recording the staged artifact state failed."), Error));
    SyncResult(Impl->State, InOutResult);
    return false;
  }
  SyncResult(Impl->State, InOutResult);
  return true;
}

bool FDurableTransactionCoordinator::Publish(
    FDurabilityResult &InOutResult, FOperationFailure &OutFailure) {
  OutFailure = FOperationFailure();
  if (!Impl->bPrepared || Impl->State.Phase != TEXT("staged")) {
    SetFailure(OutFailure, StateFailure(TEXT("durable_publish_without_stage"), TEXT("Publishing artifacts requires a staged transaction."), Impl->bPrepared, Impl->State.Phase, TEXT("staged")));
    return false;
  }
  ON_SCOPE_EXIT { RefreshPublishedArtifacts(Impl->State); };
  UE::UAI::Contract::FDiagnostic Error;
  if (!AppendPhaseAndWrite(Impl->State, TEXT("publish_started"), Error)) {
    SetFailure(OutFailure, MakeDurableFailure(TEXT("durable_journal_publish_failed"), TEXT("Recording the start of artifact publication failed."), Error));
    SyncResult(Impl->State, InOutResult);
    return false;
  }
  for (int32 Index = 0; Index < Impl->State.Artifacts.Num(); ++Index) {
    FDurableArtifactState &Artifact = Impl->State.Artifacts[Index];
    const FString PreparedPhase =
        FString::Printf(TEXT("publish_%04d_prepared"), Index);
    if (!AppendPhaseAndWrite(Impl->State, PreparedPhase, Error)) {
      SetFailure(OutFailure, MakeDurableFailure(TEXT("durable_journal_publish_step_failed"), TEXT("Recording an artifact publication step failed."), Error));
      SyncResult(Impl->State, InOutResult);
      return false;
    }
    bool bExists;
    FString Hash;
    if (!ArtifactSnapshot(Artifact.Kind, Artifact.FinalPath, bExists, Hash, Error)) {
      SetFailure(OutFailure, MakeDurableFailure(TEXT("durable_publish_preflight_failed"), TEXT("Reading the final artifact before publication failed."), Error)); return false;
    }
    if (Artifact.Kind == EDurableArtifactKind::DirectoryTree) {
      if (bExists != Artifact.bOldExists || (bExists && Hash != Artifact.OldHash)) {
        SetFailure(OutFailure, SnapshotMismatch(TEXT("durable_directory_original_unconfirmed"),
            TEXT("The final directory changed before publication."), Artifact.ArtifactId,
            Artifact.bOldExists, Artifact.OldHash, bExists, Hash));
        SyncResult(Impl->State, InOutResult); return false;
      }
      if (!VerifyArtifactSnapshot(Artifact.Kind, Artifact.BackupPath, Artifact.bOldExists, Artifact.OldHash,
          TEXT("durable_directory_original_unconfirmed"), TEXT("The directory backup no longer matches the original snapshot."), Artifact.ArtifactId, Error)) {
        SetFailure(OutFailure, MakeDurableFailure(TEXT("durable_directory_original_unconfirmed"), TEXT("The original directory or its backup no longer matches the pre-transaction snapshot."), Error, Artifact.ArtifactId));
        SyncResult(Impl->State, InOutResult); return false;
      }
      if (!RemoveArtifact(Artifact.Kind, Artifact.FinalPath, Error) ||
          (Artifact.bStagedExists && !CopyArtifact(Artifact.Kind, Artifact.FinalPath, Artifact.StagingPath, Error))) {
        SetFailure(OutFailure, MakeDurableFailure(TEXT("durable_directory_publish_failed"), TEXT("Replacing the final directory with the staged directory failed."), Error, Artifact.ArtifactId));
        SyncResult(Impl->State, InOutResult); return false;
      }
    }
    else {
      if (bExists) {
        if (!MoveArtifact(Artifact.Kind, Artifact.QuarantinePath, Artifact.FinalPath, Error)) {
          SetFailure(OutFailure, MakeDurableFailure(TEXT("durable_quarantine_failed"), TEXT("Moving the previous final artifact into transaction quarantine failed."), Error, Artifact.ArtifactId));
          SyncResult(Impl->State, InOutResult);
          return false;
        }
      }
      if (Artifact.bStagedExists) {
        if (!MoveArtifact(Artifact.Kind, Artifact.FinalPath, Artifact.StagingPath, Error)) {
          SetFailure(OutFailure, MakeDurableFailure(TEXT("durable_publish_artifact_failed"), TEXT("Moving the staged artifact to its final location failed."), Error, Artifact.ArtifactId));
          SyncResult(Impl->State, InOutResult);
          return false;
        }
      }
    }
    if (!VerifyArtifactSnapshot(Artifact.Kind, Artifact.FinalPath, Artifact.bStagedExists, Artifact.StagedHash,
        TEXT("durable_publish_verification_failed"), TEXT("The published artifact does not match the staged presence or content."), Artifact.ArtifactId, Error)) {
      SetFailure(OutFailure, MoveTemp(Error)); return false;
    }
    const FString DonePhase =
        FString::Printf(TEXT("publish_%04d_done"), Index);
    if (!AppendPhaseAndWrite(Impl->State, DonePhase, Error)) {
      SetFailure(OutFailure, MakeDurableFailure(TEXT("durable_journal_publish_step_failed"), TEXT("Recording an artifact publication step failed."), Error));
      SyncResult(Impl->State, InOutResult);
      return false;
    }
  }
  if (!AppendPhaseAndWrite(Impl->State, TEXT("published"), Error)) {
    SetFailure(OutFailure, MakeDurableFailure(TEXT("durable_journal_published_failed"), TEXT("Recording the published transaction state failed."), Error));
    SyncResult(Impl->State, InOutResult);
    return false;
  }
  SyncResult(Impl->State, InOutResult);
  return true;
}

bool FDurableTransactionCoordinator::CaptureMemoryCheckpoint(
    ITransactionBackend &Backend, FDurabilityResult &InOutResult,
    FOperationFailure &OutFailure) {
  return CaptureMemoryCheckpoint([&Backend](TArray<FDurableArtifact>& Artifacts, FOperationFailure& Failure) {
    return Backend.StageMemoryCheckpoint(Artifacts, Failure);
  }, InOutResult, OutFailure);
}

bool FDurableTransactionCoordinator::CaptureMemoryCheckpoint(FStageSource Source,
    FDurabilityResult &InOutResult, FOperationFailure &OutFailure) {
  if (!Impl->bPrepared) {
    SetFailure(OutFailure, StateFailure(TEXT("durable_memory_checkpoint_without_prepare"), TEXT("A memory checkpoint requires a prepared transaction."), Impl->bPrepared, Impl->State.Phase, TEXT("")));
    return false;
  }
  TArray<FDurableArtifact> Checkpoints = Impl->PublicArtifacts;
  for (int32 Index = 0; Index < Checkpoints.Num(); ++Index) {
    auto &State = Impl->State.Artifacts[Index];
    State.MemoryCheckpointPath = Impl->State.TransactionRoot /
        TEXT("memory_before") / FString::Printf(TEXT("%04d.bin"), Index);
    Checkpoints[Index].StagingPath = State.MemoryCheckpointPath;
    Checkpoints[Index].bStagedExists = false;
  }
  UE::UAI::Contract::FDiagnostic Error;
  if (!AppendPhaseAndWrite(Impl->State, TEXT("memory_checkpoint_started"), Error)) {
    SetFailure(OutFailure, MakeDurableFailure(TEXT("durable_memory_checkpoint_failed"), TEXT("Recording the start of memory checkpoint capture failed."), Error));
    SyncResult(Impl->State, InOutResult);
    return false;
  }
  if (!Source(Checkpoints, OutFailure)) {
    if (OutFailure.Diagnostic.IsEmpty())
      SetFailure(OutFailure, UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_memory_checkpoint_failed"),
          TEXT("The backend failed to stage the memory checkpoint without reporting a diagnostic."), TEXT("workspace_durable_transaction")));
    SyncResult(Impl->State, InOutResult);
    return false;
  }
  for (int32 Index = 0; Index < Checkpoints.Num(); ++Index) {
    auto &State = Impl->State.Artifacts[Index];
    State.bMemoryCheckpointExists = Checkpoints[Index].bStagedExists;
    bool bExists;
    if (Checkpoints[Index].Kind != State.Kind || Checkpoints[Index].StagingPath != State.MemoryCheckpointPath) {
      const bool bKindChanged = Checkpoints[Index].Kind != State.Kind;
      auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_memory_checkpoint_invalid"),
          bKindChanged ? TEXT("The backend changed the memory checkpoint artifact kind.") : TEXT("The backend changed the assigned memory checkpoint path."), TEXT("workspace_durable_transaction"), FString(), State.ArtifactId);
      Diagnostic.PropertyPath = FString::Printf(TEXT("/artifacts/%d/%s"), Index, bKindChanged ? TEXT("kind") : TEXT("memory_checkpoint_path"));
      if (bKindChanged) {
        Diagnostic.ExpectedValue = MakeShared<FJsonValueNumber>(static_cast<int32>(State.Kind));
        Diagnostic.ActualValue = MakeShared<FJsonValueNumber>(static_cast<int32>(Checkpoints[Index].Kind));
      } else {
        Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("assigned checkpoint path"));
        Diagnostic.ActualValue = MakeShared<FJsonValueString>(TEXT("different path"));
        Diagnostic.ExpectedEvidence = MakeShared<FJsonValueString>(State.MemoryCheckpointPath);
        Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(Checkpoints[Index].StagingPath);
      }
      SetFailure(OutFailure, MoveTemp(Diagnostic)); return false;
    }
    if (!ArtifactSnapshot(State.Kind, State.MemoryCheckpointPath, bExists, State.MemoryCheckpointHash, Error)) {
      SetFailure(OutFailure, MakeDurableFailure(TEXT("durable_memory_checkpoint_invalid"), TEXT("The memory checkpoint could not be read."), Error, State.ArtifactId)); return false;
    }
    if (State.bMemoryCheckpointExists && !bExists) {
      SetFailure(OutFailure, SnapshotMismatch(TEXT("durable_memory_checkpoint_invalid"), TEXT("The backend declared a memory checkpoint but did not create it."),
          State.ArtifactId, true, FString(), false, FString())); return false;
    }
    if (!State.bMemoryCheckpointExists) State.MemoryCheckpointHash.Reset();
  }
  if (!FinalArtifactsMatchOriginal(Impl->State, Error) ||
      !AppendPhaseAndWrite(Impl->State, TEXT("memory_checkpoint_ready"), Error)) {
    SetFailure(OutFailure, MakeDurableFailure(TEXT("durable_memory_checkpoint_failed"), TEXT("Capturing or recording the pre-mutation memory checkpoint failed."), Error));
    return false;
  }
  SyncResult(Impl->State, InOutResult);
  return true;
}

bool FDurableTransactionCoordinator::PublishMemoryCheckpoint(
    FDurabilityResult &InOutResult, FOperationFailure &OutFailure) {
  if (!Impl->bPrepared) {
    SetFailure(OutFailure, StateFailure(TEXT("durable_memory_checkpoint_without_prepare"), TEXT("A memory checkpoint requires a prepared transaction."), Impl->bPrepared, Impl->State.Phase, TEXT("")));
    return false;
  }
  ON_SCOPE_EXIT { RefreshPublishedArtifacts(Impl->State); };
  UE::UAI::Contract::FDiagnostic Error;
  if (!AppendPhaseAndWrite(Impl->State, TEXT("memory_restore_started"), Error)) {
    SetFailure(OutFailure, MakeDurableFailure(TEXT("durable_memory_restore_failed"), TEXT("Publishing or recording the memory checkpoint failed."), Error));
    return false;
  }
  for (const auto &Artifact : Impl->State.Artifacts) {
    bool bExists;
    FString Hash;
    if (Artifact.MemoryCheckpointPath.IsEmpty()) {
      auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_memory_checkpoint_invalid"),
          TEXT("The artifact has no recorded memory checkpoint path."), TEXT("workspace_durable_transaction"), FString(), Artifact.ArtifactId);
      Diagnostic.PropertyPath = TEXT("memory_checkpoint_path");
      Diagnostic.ExpectedValuePresent = true;
      Diagnostic.ActualValuePresent = false;
      SetFailure(OutFailure, MoveTemp(Diagnostic)); return false;
    }
    if (!ArtifactSnapshot(Artifact.Kind, Artifact.MemoryCheckpointPath, bExists, Hash, Error)) {
      SetFailure(OutFailure, MakeDurableFailure(TEXT("durable_memory_checkpoint_invalid"), TEXT("The memory checkpoint could not be read for restoration."), Error, Artifact.ArtifactId)); return false;
    }
    if (Artifact.bMemoryCheckpointExists && (!bExists || Artifact.MemoryCheckpointHash.IsEmpty() || Hash != Artifact.MemoryCheckpointHash)) {
      SetFailure(OutFailure, SnapshotMismatch(TEXT("durable_memory_checkpoint_invalid"), TEXT("The memory checkpoint is missing or does not match its recorded content."),
          Artifact.ArtifactId, true, Artifact.MemoryCheckpointHash, bExists, Hash)); return false;
    }
    const bool bRestored = Artifact.bMemoryCheckpointExists
        ? CopyArtifact(Artifact.Kind, Artifact.FinalPath, Artifact.MemoryCheckpointPath, Error)
        : RemoveArtifact(Artifact.Kind, Artifact.FinalPath, Error);
    if (!bRestored) {
      SetFailure(OutFailure, MakeDurableFailure(TEXT("durable_memory_restore_failed"), TEXT("Publishing the memory checkpoint to the final artifact failed."), Error, Artifact.ArtifactId)); return false;
    }
    if (!VerifyArtifactSnapshot(Artifact.Kind, Artifact.FinalPath, Artifact.bMemoryCheckpointExists, Artifact.MemoryCheckpointHash,
        TEXT("durable_memory_restore_failed"), TEXT("The final artifact does not match the restored memory checkpoint."), Artifact.ArtifactId, Error)) {
      SetFailure(OutFailure, MoveTemp(Error)); return false;
    }
  }

  if (!AppendPhaseAndWrite(Impl->State, TEXT("memory_restore_published"), Error)) {
    SetFailure(OutFailure, MakeDurableFailure(TEXT("durable_memory_restore_failed"), TEXT("Publishing or recording the memory checkpoint failed."), Error));
    return false;
  }
  SyncResult(Impl->State, InOutResult);
  return true;
}

bool FDurableTransactionCoordinator::Restore(
    bool bMemoryRollbackConfirmed, FDurabilityResult &InOutResult,
    FOperationFailure &OutFailure) {
  if (!RestoreDiskArtifacts(InOutResult, OutFailure))
    return false;
  return FinalizeRestore(bMemoryRollbackConfirmed, InOutResult, OutFailure);
}

bool FDurableTransactionCoordinator::RestoreDiskArtifacts(
    FDurabilityResult &InOutResult, FOperationFailure &OutFailure) {
  OutFailure = FOperationFailure();
  if (!Impl->bPrepared)
    return true;
  InOutResult.bRestoreAttempted = true;
  UE::UAI::Contract::FDiagnostic JournalError;
  const bool bRestoreJournalStarted = AppendPhaseAndWrite(
      Impl->State, TEXT("restore_started"), JournalError);
  UE::UAI::Contract::FDiagnostic RestoreError;
  const bool bArtifactsRestored = RestoreArtifacts(Impl->State, RestoreError);
  SyncResult(Impl->State, InOutResult);
  if (!bRestoreJournalStarted || !bArtifactsRestored) {
    auto Diagnostic = MakeDurableFailure(TEXT("durable_restore_unconfirmed"), TEXT("Transaction rollback could not be fully confirmed."), JournalError);
    Diagnostic.Append(RestoreError);
    SetFailure(OutFailure, MoveTemp(Diagnostic));
    return false;
  }
  return true;
}

bool FDurableTransactionCoordinator::FinalizeRestore(
    bool bMemoryRollbackConfirmed, FDurabilityResult &InOutResult,
    FOperationFailure &OutFailure) {
  OutFailure = FOperationFailure();
  if (!Impl->bPrepared)
    return true;
  UE::UAI::Contract::FDiagnostic RestoreError;
  const bool bArtifactsRestored = FinalArtifactsMatchOriginal(Impl->State, RestoreError);
  const bool bCombinedRestored = bArtifactsRestored && bMemoryRollbackConfirmed;
  const FString FinalPhase = bCombinedRestored ? TEXT("restored")
                                                : TEXT("restore_unconfirmed");
  UE::UAI::Contract::FDiagnostic FinalJournalError;
  const bool bFinalJournalWritten =
      AppendPhaseAndWrite(Impl->State, FinalPhase, FinalJournalError);
  InOutResult.bRestoreSucceeded = bCombinedRestored && bFinalJournalWritten;
  SyncResult(Impl->State, InOutResult);
  if (!bArtifactsRestored ||
      !bMemoryRollbackConfirmed || !bFinalJournalWritten) {
    auto Diagnostic = MakeDurableFailure(TEXT("durable_restore_unconfirmed"), TEXT("Transaction rollback could not be fully confirmed."), RestoreError);
    if (!bMemoryRollbackConfirmed)
      Diagnostic.Append(UE::UAI::Contract::FDiagnostic::Failure(
          TEXT("memory_rollback_unconfirmed"), TEXT("Memory rollback could not be confirmed.")));
    Diagnostic.Append(FinalJournalError);
    SetFailure(OutFailure, MoveTemp(Diagnostic));
    return false;
  }
  if (!IFileManager::Get().DeleteDirectory(*Impl->State.TransactionRoot,
                                           false, true)) {
    SetFailure(OutFailure, UE::UAI::FileDiagnostics::MakeFileOperationFailure(TEXT("durable_restore_cleanup_failed"), TEXT("Removing the restored transaction storage failed."), TEXT("workspace_durable_transaction"), Impl->State.TransactionRoot));
    InOutResult.bRestoreSucceeded = false;
    return false;
  }
  Impl->bPrepared = false;
  return true;
}

bool FDurableTransactionCoordinator::Commit(
    FDurabilityResult &InOutResult, FOperationFailure &OutFailure) {
  OutFailure = FOperationFailure();
  if (!Impl->bPrepared) {
    SetFailure(OutFailure, StateFailure(TEXT("durable_commit_without_prepare"), TEXT("Committing requires a prepared transaction."), Impl->bPrepared, Impl->State.Phase, TEXT("")));
    return false;
  }
  UE::UAI::Contract::FDiagnostic Error;
  for (const auto& Artifact : Impl->State.Artifacts) {
    if (!VerifyArtifactSnapshot(Artifact.Kind, Artifact.FinalPath, Artifact.bStagedExists, Artifact.StagedHash,
        TEXT("durable_commit_verification_failed"), TEXT("The artifact changed after publication and cannot be confirmed for commit."), Artifact.ArtifactId, Error)) {
      SetFailure(OutFailure, MoveTemp(Error)); return false;
    }
  }
  if (!AppendPhaseAndWrite(Impl->State, TEXT("committed"), Error) ||
      !WriteJournal(Impl->State, Error, Impl->State.CompletedPath)) {
    SetFailure(OutFailure, MakeDurableFailure(TEXT("durable_commit_journal_failed"), TEXT("Persisting the committed transaction journal or receipt failed."), Error));
    SyncResult(Impl->State, InOutResult);
    return false;
  }
  Impl->State.JournalPath = Impl->State.CompletedPath;
  SyncResult(Impl->State, InOutResult);
  CleanupCompletedTransaction(Impl->State, InOutResult);
  Impl->bPrepared = false;
  return true;
}

bool FDurableTransactionCoordinator::CompleteMemoryOnly(
    FDurabilityResult& InOutResult, FOperationFailure& OutFailure) {
  OutFailure = FOperationFailure();
  if (!Impl->bPrepared || Impl->State.Phase != TEXT("memory_checkpoint_ready")) {
    SetFailure(OutFailure, StateFailure(TEXT("durable_memory_completion_without_checkpoint"), TEXT("Completing a memory-only transaction requires a ready memory checkpoint."), Impl->bPrepared, Impl->State.Phase, TEXT("memory_checkpoint_ready")));
    return false;
  }
  UE::UAI::Contract::FDiagnostic Error;
  if (!FinalArtifactsMatchOriginal(Impl->State, Error) ||
      !AppendPhaseAndWrite(Impl->State, TEXT("memory_complete"), Error)) {
    SetFailure(OutFailure, MakeDurableFailure(TEXT("durable_memory_completion_failed"), TEXT("Verifying and recording the completed memory-only transaction failed."), Error));
    return false;
  }
  SyncResult(Impl->State, InOutResult);
  CleanupCompletedTransaction(Impl->State, InOutResult);
  Impl->bPrepared = false;
  return true;
}

bool FDurableTransactionCoordinator::IsPrepared() const {
  return Impl->bPrepared;
}

} // namespace UeAgentAuthoringWorkspaceTransaction

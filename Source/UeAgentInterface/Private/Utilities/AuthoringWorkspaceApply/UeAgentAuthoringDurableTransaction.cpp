// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringDurableTransaction.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringDurableDirectory.h"
#include "Core/AssetDurable/UeAgentPackageSave.h"
#include "Core/ArtifactPublisher/UeAgentArtifactGenerationFiles.h"
#include "Core/HjsonWorkspace/UeAgentWorkspaceFileProof.h"
#include "Utilities/UeAgentFileOperationDiagnostic.h"
#include "Utilities/UeAgentThreadCpuTiming.h"

#include "Dom/JsonObject.h"
#include "Async/ParallelFor.h"
#include "Async/TaskGraphInterfaces.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Hash/Blake3.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Misc/ScopeExit.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/Archive.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UeAgentAuthoringWorkspaceTransaction {

FString GetDurableTransactionRoot(bool bWorldImport) {
  return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() /
      (bWorldImport ? TEXT("UeAgentInterface/Authoring/WorldImport") : TEXT("UeAgentInterface/DurableTransactions")));
}

bool IsAuthoringDurableJournalId(const FString& Id) { return Id.StartsWith(TEXT("apply-durable:")); }

FString MakeDurableJournalRecoveryId(const FString& JournalPath) {
  FString Path = FPaths::ConvertRelativePathToFull(JournalPath); FPaths::NormalizeFilename(Path);
  for (bool bWorld : {false, true}) {
    FString Root = GetDurableTransactionRoot(bWorld); FPaths::NormalizeDirectoryName(Root);
    if (!Path.StartsWith(Root + TEXT("/"), ESearchCase::IgnoreCase)) continue;
    const FString Name = Path.Contains(TEXT("/completed/")) ? FPaths::GetBaseFilename(Path) : FPaths::GetCleanFilename(FPaths::GetPath(Path));
    return FString(TEXT("apply-durable:")) + (bWorld ? TEXT("world:") : TEXT("workspace:")) + Name;
  }
  return FString();
}

struct FDurableConsumerHistoryState {
  struct FRead {
    FString Owner, Stream, Schema, RequestId;
    FBlake3Hash TextHash;
    bool bOwner = false, bStream = false, bSequenceValid = false;
    bool bHasIntent = false, bHasVerified = false, bIntentLoaded = false, bVerifiedLoaded = false;
    FDurableConsumerRecord Record;
    UE::UAI::HjsonWorkspace::FWorkspaceFileProofPtr Proof;
    TUniquePtr<UE::UAI::ArtifactPublisher::FGenerationFileGuard> Guard;
    int64 Charge() const {
      return sizeof(FRead) + Owner.GetAllocatedSize() + Stream.GetAllocatedSize() + Schema.GetAllocatedSize() + RequestId.GetAllocatedSize()
          + Record.RequestKey.GetAllocatedSize() + Record.Phase.GetAllocatedSize() + Record.OwnerId.GetAllocatedSize()
          + Record.StreamId.GetAllocatedSize() + Record.IntentJson.GetAllocatedSize() + Record.VerifiedJson.GetAllocatedSize()
          + (Guard ? Guard->GetRetainedBytes() : 0);
    }
  };
  FDurableConsumerHistoryState* Previous = nullptr;
  TMap<FString, FRead> Reads;
  // Native scopes (e.g. Level Apply) can end before the outer import refresh.
  // Ownership moves here only for the current request; this is not history
  // authority and is never carried into PreviousParsedHistory.
  TArray<FDurableConsumerRecord> RequestRecords;
  FString JournalRoot;
  // Only completed, private durable receipts may hold a read guard. Authoring
  // source files and active journals retain their ordinary live read behavior.
  TUniquePtr<UE::UAI::ArtifactPublisher::FGenerationDirectoryScope> ReceiptDirectories;
  int32 ObservedReceipts = 0;
  int64 Bytes = 0;
};
}

namespace {
using namespace UeAgentAuthoringWorkspaceTransaction;

constexpr const TCHAR *JournalSchema = TEXT("uai.apply_durable_journal.v1");
constexpr const TCHAR *JournalFileName = TEXT("journal.json");
thread_local FScopedDurableRecoveryNotice* CurrentRecoveryNotice = nullptr;
thread_local FScopedDurableConsumer* CurrentDurableConsumer = nullptr;
thread_local FDurableConsumerHistoryState* CurrentHistory = nullptr;
// Pure parsed receipt material only. No current-file proof survives a request;
// every subsequent call enumerates and reads the actual legacy source again.
// Ownership moves into the next scope, sharing the existing 64 MiB budget.
thread_local TUniquePtr<FDurableConsumerHistoryState> PreviousParsedHistory;

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
  FString OriginRequestId;
  FString RequestKey;
  FString RequestFingerprint;
  FString TransactionRoot;
  FString JournalPath;
  FString CompletedPath;
  FString Phase;
  TArray<FString> PhaseHistory;
  TArray<FDurableArtifactState> Artifacts;
  FDurableConsumerRecord Consumer;
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

bool HasPendingConsumerObservation(const FDurableJournalState& State) {
  if (State.Consumer.VerifiedJson.IsEmpty()) return false;
  const auto Reader = TJsonReaderFactory<TCHAR>::Create(State.Consumer.VerifiedJson);
  EJsonNotation Notation;
  if (!Reader->ReadNext(Notation) || Notation != EJsonNotation::ObjectStart) return true;
  while (Reader->ReadNext(Notation)) {
    if (Notation == EJsonNotation::Error) return true;
    if (Notation == EJsonNotation::String && Reader->GetIdentifier() == TEXT("observation_status"))
      return Reader->GetValueAsString() == TEXT("pending");
    if (Notation == EJsonNotation::ObjectStart && !Reader->SkipObject()) return true;
    if (Notation == EJsonNotation::ArrayStart && !Reader->SkipArray()) return true;
    if (Notation == EJsonNotation::ObjectEnd) return false;
  }
  return true;
}

void RetainObservationMaterial(const FDurableJournalState& State, FDurabilityResult& Result) {
  auto Warning = UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_observation_material_retained"),
      TEXT("The native transaction committed; its before/stage material is retained because HJSON observation is incomplete."), TEXT("workspace_durable_cleanup"));
  Warning.Severity = UE::UAI::Contract::ESeverity::Warning;
  Warning.TransactionStatus = State.Phase;
  Warning.RetryImport = false;
  UeAgentAuthoringDiagnostics::FScopedWarnings CommandWarnings;
  CommandWarnings.Add(Warning); Result.Warnings.Add(MoveTemp(Warning));
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

bool LoadJournalText(const FString& Path, FString& Text, UE::UAI::Contract::FDiagnostic& OutError) {
  if (!FFileHelper::LoadFileToString(Text, *Path)) {
    OutError = UE::UAI::FileDiagnostics::MakeFileOperationFailure(TEXT("durable_journal_read_failed"),
        TEXT("Reading the durable transaction journal failed."), TEXT("workspace_durable_transaction"), Path);
    return false;
  }
  return true;
}
bool ParseJournalText(const FString& Path, const FString& Text, TSharedPtr<FJsonObject>& OutObject,
    UE::UAI::Contract::FDiagnostic& OutError) {
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

bool LoadJson(const FString& Path, TSharedPtr<FJsonObject>& OutObject, UE::UAI::Contract::FDiagnostic& OutError) {
  FString Text;
  return LoadJournalText(Path, Text, OutError) && ParseJournalText(Path, Text, OutObject, OutError);
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
  if (!State.OriginRequestId.IsEmpty()) Root->SetStringField(TEXT("origin_request_id"), State.OriginRequestId);
  Root->SetStringField(TEXT("request_key"), State.RequestKey);
  Root->SetStringField(TEXT("request_fingerprint"),
                       State.RequestFingerprint);
  Root->SetStringField(TEXT("transaction_root"), State.TransactionRoot);
  Root->SetStringField(TEXT("phase"), State.Phase);
  Root->SetArrayField(TEXT("phase_history"),
                      StringArrayToJson(State.PhaseHistory));
  if (!State.Consumer.OwnerId.IsEmpty()) {
    const auto Consumer = MakeShared<FJsonObject>();
    Consumer->SetStringField(TEXT("owner"), State.Consumer.OwnerId);
    Consumer->SetStringField(TEXT("stream"), State.Consumer.StreamId);
    Consumer->SetStringField(TEXT("sequence"), LexToString(State.Consumer.Sequence));
    Consumer->SetStringField(TEXT("intent"), State.Consumer.IntentJson);
    Consumer->SetStringField(TEXT("verified"), State.Consumer.VerifiedJson);
    Root->SetObjectField(TEXT("consumer_evidence"), Consumer);
  }
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
  if (Root->HasField(TEXT("origin_request_id"))
      && !ReadString(*Root, TEXT("origin_request_id"), OutState.OriginRequestId, TEXT("/origin_request_id"))) return false;
  if (Root->HasField(TEXT("consumer_evidence"))) {
    const TSharedPtr<FJsonObject>* Consumer = nullptr;
    FString Sequence;
    if (!Root->TryGetObjectField(TEXT("consumer_evidence"), Consumer) || !Consumer || !Consumer->IsValid())
      return Fail(TEXT("durable_journal_field_invalid"), TEXT("The consumer evidence must be an object."),
          TEXT("/consumer_evidence"), TEXT("object"), TEXT("invalid"));
    if (!ReadString(**Consumer, TEXT("owner"), OutState.Consumer.OwnerId, TEXT("/consumer_evidence/owner")) ||
        !ReadString(**Consumer, TEXT("stream"), OutState.Consumer.StreamId, TEXT("/consumer_evidence/stream")) ||
        !ReadString(**Consumer, TEXT("sequence"), Sequence, TEXT("/consumer_evidence/sequence")) ||
        !ReadString(**Consumer, TEXT("intent"), OutState.Consumer.IntentJson, TEXT("/consumer_evidence/intent")) ||
        !ReadString(**Consumer, TEXT("verified"), OutState.Consumer.VerifiedJson, TEXT("/consumer_evidence/verified"))) return false;
    if (OutState.Consumer.OwnerId.IsEmpty() || OutState.Consumer.StreamId.IsEmpty() ||
        !LexTryParseString(OutState.Consumer.Sequence, *Sequence) || OutState.Consumer.Sequence <= 0 ||
        !LexToString(OutState.Consumer.Sequence).Equals(Sequence, ESearchCase::CaseSensitive))
      return Fail(TEXT("durable_journal_field_invalid"), TEXT("The consumer evidence identity or sequence is invalid."),
          TEXT("/consumer_evidence"), TEXT("nonempty owner/stream and positive sequence"), Sequence);
  }
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

void RefreshPublishedArtifacts(TConstArrayView<FDurableArtifactState> Artifacts, int32 Confirmed) {
  TArray<FString> Files;
  TArray<FString> UnconfirmedFiles;
  for (int32 Index = 0; Index < Artifacts.Num(); ++Index) {
    const auto& Artifact = Artifacts[Index];
    auto& Destination = Index < Confirmed ? Files : UnconfirmedFiles;
    if (Artifact.Kind == EDurableArtifactKind::File) {
      Destination.Add(Artifact.FinalPath);
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
          Destination.AddUnique(Artifact.FinalPath / File);
      }
    }
  }
  UE::UAI::AssetDurable::RefreshPublishedPackageFiles(Files);
  UE::UAI::AssetDurable::RescanPackageFiles(UnconfirmedFiles);
}

bool RestoreArtifacts(const FDurableJournalState &State, UE::UAI::Contract::FDiagnostic& OutError) {
  int32 Confirmed = 0;
  ON_SCOPE_EXIT { RefreshPublishedArtifacts(State.Artifacts, Confirmed); };
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
    ++Confirmed;
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
  if (CurrentRecoveryNotice) CurrentRecoveryNotice->RetainJournal(MakeDurableJournalRecoveryId(State.JournalPath));
  OutResult.JournalSchema = JournalSchema;
  OutResult.JournalPhase = State.Phase;
  OutResult.JournalPath = State.JournalPath;
  OutResult.PhaseHistory = State.PhaseHistory;
  OutResult.ArtifactCount = State.Artifacts.Num();
}

FString ConsumerReceiptDirectory(const FString& JournalRoot, const FString& Owner, const FString& Stream) {
  const FString Completed = NormalizeAbsolutePath(JournalRoot) / TEXT("completed");
  if (Owner.IsEmpty() || Stream.IsEmpty()) return Completed / TEXT("unscoped");
  // Physical placement only. The record's complete owner/stream fields remain
  // authoritative, and are checked after reading a selected directory.
  return Completed / FMD5::HashAnsiString(*Owner).ToLower() / FMD5::HashAnsiString(*Stream).ToLower();
}

FString CompletedPathFor(const FString& JournalRoot, const FString& RequestKey,
    const FString& Owner = {}, const FString& Stream = {}) {
  return ConsumerReceiptDirectory(JournalRoot, Owner, Stream) / (RequestKey + TEXT(".json"));
}

bool FindCompletedPathFor(const FString& JournalRoot, const FString& RequestKey,
    FString& OutPath, UE::UAI::Contract::FDiagnostic& Error,
    const FString& Owner = {}, const FString& Stream = {}, bool bExplicitRequestRecovery = false) {
  const FString Root = NormalizeAbsolutePath(JournalRoot) / TEXT("completed");
  TArray<FString> Matches;
  int32 ExactCandidates = 0;
  for (const FString& Path : {CompletedPathFor(JournalRoot, RequestKey, Owner, Stream),
      Root / (RequestKey + TEXT(".json")), CompletedPathFor(JournalRoot, RequestKey)}) {
    ++ExactCandidates;
    if (IFileManager::Get().FileExists(*Path)) Matches.AddUnique(Path);
  }
  // Explicit request recovery has an identity, but an old public handle
  // need not know its stream. Enumerate matching names only, never parse other
  // receipts to discover the requested identity. A new request's idempotency
  // probe already has its actual owner/stream: a negative exact lookup must not
  // turn into an unrelated whole-history namespace search.
  const bool bSearchNames = bExplicitRequestRecovery && Matches.IsEmpty();
  if (bSearchNames) IFileManager::Get().FindFilesRecursive(Matches, *Root, *(RequestKey + TEXT(".json")), true, false);
  UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=durable_completed_lookup exact_candidates=%d matches=%d recursive_names=%s explicit_recovery=%s"),
      ExactCandidates, Matches.Num(), bSearchNames ? TEXT("true") : TEXT("false"), bExplicitRequestRecovery ? TEXT("true") : TEXT("false"));
  Matches.Sort();
  FString Expected;
  for (int32 I = 0; I < Matches.Num() && Matches.Num() > 1; ++I) {
    FString Actual;
    if (!FileHash(Matches[I], Actual, Error)) return false;
    if (I == 0) Expected = Actual;
    else if (Actual != Expected) {
      Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_request_evidence_ambiguous"),
          TEXT("The original request has different retained receipts; no action was replayed or restored."),
          TEXT("workspace_durable_transaction"), Matches[I]); return false;
    }
  }
  OutPath = Matches.IsEmpty() ? CompletedPathFor(JournalRoot, RequestKey, Owner, Stream) : Matches[0];
  return true;
}

FString RequestKeyFor(const FString &RequestId) {
  return FMD5::HashAnsiString(*RequestId).ToLower();
}

bool LoadRecoveryJournal(const FString& Id, FDurableJournalState& State,
    FString& Owner, UE::UAI::Contract::FDiagnostic& Error) {
  TArray<FString> Parts; Id.ParseIntoArray(Parts, TEXT(":"), false);
  if (Parts.Num() != 3 || Parts[0] != TEXT("apply-durable")
      || (Parts[1] != TEXT("workspace") && Parts[1] != TEXT("world"))
      || Parts[2].Len() < 32 || Parts[2].Len() > 65) {
    Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_recovery_identity_invalid"),
        TEXT("The recovery identity is not an authoring journal issued by this owner."), TEXT("durable_maintenance")); return false;
  }
  for (TCHAR C : Parts[2]) if (!FChar::IsHexDigit(C) && C != TEXT('-')) {
    Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_recovery_identity_invalid"),
        TEXT("The recovery identity contains an invalid token."), TEXT("durable_maintenance")); return false;
  }
  const FString Root = NormalizeAbsolutePath(GetDurableTransactionRoot(Parts[1] == TEXT("world")));
  FString Path = Root / TEXT("active") / Parts[2] / JournalFileName;
  bool bActive = IFileManager::Get().FileExists(*Path);
  if (!bActive && !FindCompletedPathFor(Root, Parts[2].Left(32), Path, Error, {}, {}, true)) return false;
  TSharedPtr<FJsonObject> Object;
  if (!LoadJson(Path, Object, Error) || !JsonToJournal(Path, Object.ToSharedRef(), State, Error, bActive)) return false;
  if (State.RequestKey != Parts[2].Left(32) || !IsWithinRoot(Root, State.TransactionRoot)) {
    Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_recovery_identity_mismatch"),
        TEXT("The journal belongs to a different request or transaction root."), TEXT("durable_maintenance")); return false;
  }
  return FileHash(Path, Owner, Error);
}

bool RecoveryInspection(const FString& Id, const FDurableJournalState& State, const FString& Owner,
    TSharedPtr<FJsonObject>& Out, UE::UAI::Contract::FDiagnostic& Error) {
  Out = MakeShared<FJsonObject>();
  Out->SetStringField(TEXT("schema"), TEXT("uai.asset_durable.journal_inspection.v1"));
  Out->SetStringField(TEXT("journal_kind"), JournalSchema);
  Out->SetStringField(TEXT("journal_id"), Id);
  Out->SetStringField(TEXT("journal_owner_id"), Owner);
  Out->SetNumberField(TEXT("fencing_token"), State.PhaseHistory.Num());
  Out->SetStringField(TEXT("phase"), State.Phase);
  Out->SetBoolField(TEXT("read_only"), true);
  Out->SetBoolField(TEXT("retry_safe"), false);
  TArray<TSharedPtr<FJsonValue>> Observed;
  bool bMemoryDiffers = false;
  for (const auto& Artifact : State.Artifacts) {
    bool bExists = false; FString Hash;
    if (!ArtifactSnapshot(Artifact.Kind, Artifact.FinalPath, bExists, Hash, Error)) return false;
    const auto Row = MakeShared<FJsonObject>();
    Row->SetStringField(TEXT("artifact_id"), Artifact.ArtifactId);
    Row->SetBoolField(TEXT("present"), bExists);
    Row->SetBoolField(TEXT("matches_disk_before"), bExists == Artifact.bOldExists && (!bExists || Hash == Artifact.OldHash));
    Row->SetBoolField(TEXT("matches_published"), bExists == Artifact.bStagedExists && (!bExists || Hash == Artifact.StagedHash));
    bMemoryDiffers |= !Artifact.MemoryCheckpointPath.IsEmpty()
        && (Artifact.bMemoryCheckpointExists != Artifact.bOldExists
          || (Artifact.bOldExists && Artifact.MemoryCheckpointHash != Artifact.OldHash));
    Observed.Add(MakeShared<FJsonValueObject>(Row));
  }
  Out->SetArrayField(TEXT("observed_states"), MoveTemp(Observed));
  Out->SetBoolField(TEXT("memory_before_differs_from_disk_bytes"), bMemoryDiffers);
  Out->SetStringField(TEXT("recovery_action"), TEXT("inspect_then_reconcile_original_request_without_replay"));
  return true;
}

} // namespace

namespace UeAgentAuthoringWorkspaceTransaction {

bool TryInspectAuthoringJournal(const FString& Id, TSharedPtr<FJsonObject>& Out,
    UE::UAI::Contract::FDiagnostic& Error) {
  FDurableJournalState State; FString Owner;
  return LoadRecoveryJournal(Id, State, Owner, Error) && RecoveryInspection(Id, State, Owner, Out, Error);
}

void DiscoverAuthoringJournals(TArray<FString>& OutIds, TArray<UE::UAI::Contract::FDiagnostic>& Warnings) {
  OutIds.Reset(); Warnings.Reset();
  for (bool bWorld : {false, true}) {
    TArray<FString> Files;
    const FString Root = NormalizeAbsolutePath(GetDurableTransactionRoot(bWorld));
    IFileManager::Get().FindFilesRecursive(Files, *(Root / TEXT("active")), JournalFileName, true, false);
    for (const auto& Path : Files) {
      const FString Id = MakeDurableJournalRecoveryId(Path);
      FDurableJournalState State; FString Owner; UE::UAI::Contract::FDiagnostic Error;
      if (!LoadRecoveryJournal(Id, State, Owner, Error)) {
        Error.Severity = UE::UAI::Contract::ESeverity::Warning;
        Warnings.Add(MoveTemp(Error)); continue;
      }
      // Only this owner's validated retained journals. Inspection/reconcile
      // remains explicit; discovery does not hash backups, replay or take over.
      OutIds.AddUnique(Id);
    }
  }
  OutIds.Sort();
}

bool TryRecoverDurableRequest(const FString& RequestId, TSharedPtr<FJsonObject>& Out,
    UE::UAI::Contract::FDiagnostic& Error) {
  Out.Reset();
  if (RequestId.IsEmpty()) return false;
  TMap<FString, TSharedPtr<FJsonObject>> Inspections;
  TMap<FString, FString> Fingerprints;
  TMap<FString, FString> Phases;
  for (bool bWorld : {false, true}) {
    const FString Root = NormalizeAbsolutePath(GetDurableTransactionRoot(bWorld));
    TArray<FString> Files;
    IFileManager::Get().FindFilesRecursive(Files, *(Root / TEXT("active")), JournalFileName, true, false);
    IFileManager::Get().FindFilesRecursive(Files, *(Root / TEXT("completed")), TEXT("*.json"), true, false, false);
    for (const auto& Path : Files) {
      TSharedPtr<FJsonObject> Json; UE::UAI::Contract::FDiagnostic ReadError;
      FString NativeRequest, Origin;
      if (!LoadJson(Path, Json, ReadError)) continue; // An unrelated unreadable record is not custody.
      Json->TryGetStringField(TEXT("request_id"), NativeRequest);
      Json->TryGetStringField(TEXT("origin_request_id"), Origin);
      if (NativeRequest != RequestId && Origin != RequestId) continue;
      const FString Id = MakeDurableJournalRecoveryId(Path);
      FDurableJournalState State; FString Owner; TSharedPtr<FJsonObject> Inspection;
      if (!LoadRecoveryJournal(Id, State, Owner, Error)
          || (State.RequestId != RequestId && State.OriginRequestId != RequestId)
          || !RecoveryInspection(Id, State, Owner, Inspection, Error)) return false;
      if (const auto* Previous = Fingerprints.Find(NativeRequest)) {
        if (*Previous != State.RequestFingerprint) {
          Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_request_evidence_ambiguous"),
              TEXT("Retained attempts of this native request have different inputs; no action was replayed or restored."), TEXT("operation_recovery")); return false;
        }
        if (Phases.FindChecked(NativeRequest) == TEXT("committed")) continue;
      }
      Fingerprints.Add(NativeRequest, State.RequestFingerprint);
      Phases.Add(NativeRequest, State.Phase);
      Inspections.Add(NativeRequest, MoveTemp(Inspection));
    }
  }
  if (Inspections.IsEmpty()) {
    Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_request_evidence_missing"),
        TEXT("No readable authoring journal can be attributed to this original request; no action was replayed or restored."), TEXT("operation_recovery"));
    Error.bRetrySafe = false; return false;
  }
  Out = MakeShared<FJsonObject>();
  Out->SetStringField(TEXT("schema"), TEXT("uai.long_running.durable_recovery.v1"));
  bool bCommitted = true; TArray<TSharedPtr<FJsonValue>> Journals;
  for (const auto& Pair : Inspections) {
    bCommitted &= Phases.FindChecked(Pair.Key) == TEXT("committed");
    Journals.Add(MakeShared<FJsonValueObject>(Pair.Value));
  }
  Out->SetStringField(TEXT("status"), bCommitted ? TEXT("committed") : TEXT("state_unknown"));
  Out->SetBoolField(TEXT("retry_safe"), false);
  Out->SetBoolField(TEXT("native_replayed"), false);
  Out->SetArrayField(TEXT("durability"), MoveTemp(Journals));
  return true;
}

bool TryReconcileAuthoringJournal(const FString& Id, const FString& ExpectedOwner, int64 ExpectedFence,
    TSharedPtr<FJsonObject>& Out, UE::UAI::Contract::FDiagnostic& Error) {
  FDurableJournalState State; FString Owner;
  if (!LoadRecoveryJournal(Id, State, Owner, Error)) return false;
  if (Owner != ExpectedOwner || State.PhaseHistory.Num() != ExpectedFence) {
    Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_recovery_binding_changed"),
        TEXT("The inspected journal changed; inspect it again before reconciliation."), TEXT("durable_maintenance")); return false;
  }
  if (State.Phase == TEXT("committed") || State.Phase == TEXT("restored")
      || State.Phase == TEXT("startup_restored") || State.Phase == TEXT("memory_complete")
      || State.Phase == TEXT("disk_restored_memory_preserved")) {
    return RecoveryInspection(Id, State, Owner, Out, Error);
  }
  // This is recovery of this one request, not a startup sweep of other work.
  // A newer file or a live native package cannot be overwritten by its backup.
  for (const auto& Artifact : State.Artifacts) {
    FString PackageName;
    if ((Artifact.FinalPath.EndsWith(TEXT(".umap")) || Artifact.FinalPath.EndsWith(TEXT(".uasset")))
        && FPackageName::TryConvertFilenameToLongPackageName(Artifact.FinalPath, PackageName)
        && FindPackage(nullptr, *PackageName)) {
      Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_recovery_package_loaded"),
          TEXT("The recovery package is loaded. Release its Editor context before restoring its recorded file."), TEXT("durable_maintenance"));
      Error.TargetPath = PackageName; return false;
    }
    bool bExists = false; FString Hash;
    if (!ArtifactSnapshot(Artifact.Kind, Artifact.FinalPath, bExists, Hash, Error)) return false;
    const bool bBefore = bExists == Artifact.bOldExists && (!bExists || Hash == Artifact.OldHash);
    const bool bPublished = bExists == Artifact.bStagedExists && (!bExists || Hash == Artifact.StagedHash);
    if (!bBefore && !bPublished) {
      Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_recovery_artifact_changed"),
          TEXT("The current artifact differs from both this request's before and published states; it was preserved for review."), TEXT("durable_maintenance"));
      Error.SubjectId = Artifact.ArtifactId; return false;
    }
    if (Artifact.bOldExists && !VerifyArtifactSnapshot(Artifact.Kind, Artifact.BackupPath, true, Artifact.OldHash,
        TEXT("durable_restore_backup_hash_mismatch"), TEXT("The original backup cannot be confirmed."), Artifact.ArtifactId, Error)) return false;
  }
  if (!RestoreArtifacts(State, Error)) return false;
  const bool bMemoryDiffers = State.Artifacts.ContainsByPredicate([](const auto& Artifact) {
    return !Artifact.MemoryCheckpointPath.IsEmpty() && (Artifact.bMemoryCheckpointExists != Artifact.bOldExists
        || (Artifact.bOldExists && Artifact.MemoryCheckpointHash != Artifact.OldHash));
  });
  if (!AppendPhaseAndWrite(State, bMemoryDiffers ? TEXT("disk_restored_memory_preserved") : TEXT("restored"), Error)
      || !FileHash(State.JournalPath, Owner, Error) || !RecoveryInspection(Id, State, Owner, Out, Error)) return false;
  Out->SetBoolField(TEXT("read_only"), false);
  Out->SetStringField(TEXT("status"), bMemoryDiffers ? TEXT("disk_restored_memory_unconfirmed") : TEXT("rolled_back"));
  // Keep original before/stage/checkpoint material for independent native
  // verification; a filesystem receipt is not a complete semantic readback.
  return true;
}

FScopedDurableConsumer::FScopedDurableConsumer(FString InOwnerId, FString InStreamId,
    FString InIntentJson, FSeal InSeal, FObserve InObserve)
    : Previous(CurrentDurableConsumer), OwnerId(MoveTemp(InOwnerId)), StreamId(MoveTemp(InStreamId)),
      IntentJson(MoveTemp(InIntentJson)), Seal(MoveTemp(InSeal)), Observe(MoveTemp(InObserve)) {
  CurrentDurableConsumer = this;
}

FScopedDurableConsumer::~FScopedDurableConsumer() {
  if (CurrentHistory) CurrentHistory->RequestRecords.Append(MoveTemp(CommittedRecords));
  CurrentDurableConsumer = Previous;
}
bool FScopedDurableConsumer::HasActualStateConsumer() { return CurrentDurableConsumer && !!CurrentDurableConsumer->Observe; }
bool FScopedDurableConsumer::IsObservationPending() { return CurrentDurableConsumer && CurrentDurableConsumer->bObservationPending; }

void FScopedDurableConsumer::ObserveActualState(const UeAgentAuthoringState::FStateSnapshot& Before,
    const UeAgentAuthoringState::FStateSnapshot& Actual) {
  if (CurrentDurableConsumer && CurrentDurableConsumer->Observe) CurrentDurableConsumer->Observe(Before, Actual);
}

void FScopedDurableConsumer::MarkObservationPending() {
  if (CurrentDurableConsumer) CurrentDurableConsumer->bObservationPending = true;
}

TSharedPtr<const IDurableConsumerMaterial> FScopedDurableConsumer::FindCommittedMaterial(const FDurableConsumerRecord& Record,
    bool* OutCurrentRequestCommit) {
  if (OutCurrentRequestCommit) *OutCurrentRequestCommit = false;
  if (Record.Phase != TEXT("committed")) return {};
  const auto Matches = [&](const FDurableConsumerRecord& Sealed) {
    return Sealed.RequestKey == Record.RequestKey && Sealed.OwnerId == Record.OwnerId
        && Sealed.StreamId == Record.StreamId && Sealed.Sequence == Record.Sequence
        && Sealed.Phase == Record.Phase && Sealed.VerifiedJson == Record.VerifiedJson;
  };
  for (auto* Scope = CurrentDurableConsumer; Scope; Scope = Scope->Previous)
    for (const auto& Sealed : Scope->CommittedRecords)
      if (Matches(Sealed)) { if (OutCurrentRequestCommit) *OutCurrentRequestCommit = true; return Sealed.Material; }
  if (CurrentHistory) for (const auto& Sealed : CurrentHistory->RequestRecords)
    if (Matches(Sealed)) { if (OutCurrentRequestCommit) *OutCurrentRequestCommit = true; return Sealed.Material; }
  return {};
}

FScopedDurableConsumerHistory::FScopedDurableConsumerHistory() {
  if (CurrentHistory) return;
  State = PreviousParsedHistory ? MoveTemp(PreviousParsedHistory) : MakeUnique<FDurableConsumerHistoryState>();
  State->Previous = CurrentHistory;
  CurrentHistory = State.Get();
}
FScopedDurableConsumerHistory::~FScopedDurableConsumerHistory() {
  if (State) {
    ReleaseDurableConsumerHistoryReads();
    State->RequestRecords.Empty();
    CurrentHistory = State->Previous;
    State->Previous = nullptr;
    PreviousParsedHistory = MoveTemp(State);
  }
}

void ReleaseDurableConsumerHistoryReads() {
  if (!CurrentHistory || (!CurrentHistory->ObservedReceipts && !CurrentHistory->ReceiptDirectories)) return;
  const double Started = FPlatformTime::Seconds();
  const int32 Released = CurrentHistory->ObservedReceipts;
  for (auto& Pair : CurrentHistory->Reads) {
    auto& Read = Pair.Value;
    if (Read.Guard) CurrentHistory->Bytes -= Read.Guard->GetRetainedBytes();
    Read.Guard.Reset(); Read.Proof.Reset();
  }
  CurrentHistory->ReceiptDirectories.Reset();
  CurrentHistory->ObservedReceipts = 0;
  const auto Usage = UE::UAI::HjsonWorkspace::GetWorkspaceFileProofUsage();
  UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=durable_history_release guards=%d pending_file_proofs=%d proof_path_characters=%lld total_s=%.6f"),
      Released, Usage.PendingOperations, Usage.PathCharacters, FPlatformTime::Seconds() - Started);
}

bool ReadDurableConsumerRecords(const FString& JournalRoot, const FString& OwnerId, const FString& StreamId,
    TArray<FDurableConsumerRecord>& OutRecords, FOperationFailure& OutFailure) {
  return ReadDurableConsumerRecords(JournalRoot, OwnerId, StreamId, OutRecords, OutFailure, EDurableConsumerBody::Complete, 0);
}

void CaptureDurableCompletedRequestKeys(const FString& JournalRoot, TSet<FString>& OutRequestKeys) {
  OutRequestKeys.Reset();
  const double Started = FPlatformTime::Seconds();
  TArray<FString> Paths;
  IFileManager::Get().FindFilesRecursive(Paths, *(NormalizeAbsolutePath(JournalRoot) / TEXT("completed")), TEXT("*.json"), true, false);
  for (const auto& Path : Paths) {
    const FString Key = FPaths::GetBaseFilename(Path);
    bool bValid = Key.Len() == 32;
    for (TCHAR C : Key) bValid &= FChar::IsHexDigit(C);
    if (bValid) OutRequestKeys.Add(Key);
  }
  UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=completed_capture_boundary names=%d receipt_body_reads=0 numeric_global_fence=false total_s=%.6f"),
      OutRequestKeys.Num(), FPlatformTime::Seconds() - Started);
}

bool ReadDurableConsumerRecords(const FString& JournalRoot, const FString& OwnerId, const FString& StreamId,
    TArray<FDurableConsumerRecord>& OutRecords, FOperationFailure& OutFailure,
    EDurableConsumerBody Body, int64 BodyAfterSequence) {
  OutRecords.Reset();
  const double Started = FPlatformTime::Seconds();
  const auto CpuStarted = UE::UAI::Timing::SampleThreadCpu();
  int32 Parsed = 0, Reused = 0, PhysicalReads = 0, LogicalRows = 0, GuardReused = 0, GuardAcquired = 0, GuardRejected = 0;
  double ReadSeconds = 0, ParseSeconds = 0, ReadCpuSeconds = 0, ProofSeconds = 0;
  double AcquireSeconds = 0, GuardReadSeconds = 0, UnguardedReadSeconds = 0;
  int32 ProofCapacityMisses = 0, SkippedProofAcquisitions = 0;
  bool bMayAcquireReceiptProof = true;
  bool bReadCpuValid = true;
  const bool bWantIntent = !StreamId.IsEmpty() && Body != EDurableConsumerBody::Metadata;
  const bool bWantVerified = !StreamId.IsEmpty() && Body == EDurableConsumerBody::Complete;
  const auto NeedsBody = [&](const FDurableConsumerHistoryState::FRead& Read) {
    return Read.bOwner && Read.Owner == OwnerId && Read.Stream == StreamId && Read.Record.Sequence > BodyAfterSequence
      && ((bWantIntent && !Read.bIntentLoaded) || (bWantVerified && !Read.bVerifiedLoaded));
  };
  struct FMetadataProjection {
    FDurableConsumerHistoryState::FRead Read;
    FString Sequence, SyntaxError;
    FBlake3Hash TextHash;
    double WorkSeconds = 0;
    bool bSyntaxValid = false, bShouldParse = false;
  };
  const auto ParseMetadata = [&](const FString& Text, FDurableConsumerHistoryState::FRead& Read,
      FString& Sequence, FString& SyntaxError, bool bReadBody) {
      const auto Reader = TJsonReaderFactory<TCHAR>::Create(Text);
      EJsonNotation Notation;
      int32 Depth = 0, ConsumerDepth = -1;
      // A new metadata row does not yet establish target selection. Decode its
      // body only after selection, from this same already-read text below.
      while (Reader->ReadNext(Notation)) {
        const FString& Field = Reader->GetIdentifier();
        if (Notation == EJsonNotation::ObjectStart || Notation == EJsonNotation::ArrayStart) {
          ++Depth;
          if (Depth == 2 && Field == TEXT("consumer_evidence") && Notation == EJsonNotation::ObjectStart) ConsumerDepth = Depth;
        } else if (Notation == EJsonNotation::ObjectEnd || Notation == EJsonNotation::ArrayEnd) {
          if (ConsumerDepth == Depth) ConsumerDepth = -1;
          --Depth;
        } else if (Notation == EJsonNotation::String) {
          if (Depth == 1 && Field == TEXT("request_key")) Read.Record.RequestKey = Reader->GetValueAsString();
          else if (Depth == 1 && Field == TEXT("schema")) Read.Schema = Reader->GetValueAsString();
          else if (Depth == 1 && Field == TEXT("request_id")) Read.RequestId = Reader->GetValueAsString();
          else if (Depth == 1 && Field == TEXT("phase")) Read.Record.Phase = Reader->GetValueAsString();
          else if (Depth == ConsumerDepth) {
            if (Field == TEXT("owner")) { Read.Owner = Reader->GetValueAsString(); Read.bOwner = true; }
            else if (Field == TEXT("stream")) { Read.Stream = Reader->GetValueAsString(); Read.bStream = true; }
            else if (Field == TEXT("sequence")) Sequence = Reader->GetValueAsString();
            else if (Field == TEXT("intent")) {
              Read.bHasIntent = true;
              if (bReadBody && bWantIntent) { Read.Record.IntentJson = Reader->GetValueAsString(); Read.bIntentLoaded = true; }
            } else if (Field == TEXT("verified")) {
              Read.bHasVerified = true;
              if (bReadBody && bWantVerified) { Read.Record.VerifiedJson = Reader->GetValueAsString(); Read.bVerifiedLoaded = true; }
            }
          }
        }
      }
    SyntaxError = Reader->GetErrorMessage();
    return SyntaxError.IsEmpty() && Depth == 0;
  };
  const FString Root = NormalizeAbsolutePath(JournalRoot);
  if (CurrentHistory && !CurrentHistory->JournalRoot.Equals(Root, ESearchCase::CaseSensitive)) {
    ReleaseDurableConsumerHistoryReads();
    CurrentHistory->Reads.Empty(); CurrentHistory->Bytes = 0;
    CurrentHistory->JournalRoot = Root;
    CurrentHistory->Bytes = sizeof(FDurableConsumerHistoryState) + CurrentHistory->JournalRoot.GetAllocatedSize();
  }
  TArray<FString> Files;
  IFileManager::Get().FindFilesRecursive(Files, *(Root / TEXT("active")), JournalFileName, true, false);
  TSet<FString> ActiveRequests;
  for (const auto& ActivePath : Files)
    ActiveRequests.Add(FPaths::GetCleanFilename(FPaths::GetPath(ActivePath)).Left(32));
  const FString CompletedRoot = Root / TEXT("completed");
  // Normal consumers read only their owner/target receipt directory.
  // Old flat receipts remain available to explicit request-ID recovery;
  // a missing stream record is reported by its publication consumer, never
  // repaired by scanning/migrating every historical target here.
  const FString SelectedDirectory = StreamId.IsEmpty()
      ? CompletedRoot / FMD5::HashAnsiString(*OwnerId).ToLower() : ConsumerReceiptDirectory(Root, OwnerId, StreamId);
  IFileManager::Get().FindFilesRecursive(Files, *SelectedDirectory, TEXT("*.json"), true, false, false);
  const double Enumerated = FPlatformTime::Seconds();
  TMap<FString, int32> Requests;
  TSet<FString> ActiveRecordKeys;
  TSet<FString> CurrentPaths;
  double MigrationSeconds = 0;
  const auto Consume = [&](const FString& Path, FString Text, bool bCompleted, bool bProvenCached, UE::UAI::HjsonWorkspace::FWorkspaceFileProofPtr Proof,
      TUniquePtr<UE::UAI::ArtifactPublisher::FGenerationFileGuard> Guard, FMetadataProjection* Projection = nullptr) {
    ++LogicalRows;
    CurrentPaths.Add(Path);
    auto* Prior = CurrentHistory ? CurrentHistory->Reads.Find(Path) : nullptr;
    const int64 OldCharge = Prior ? Prior->Charge() + Path.GetAllocatedSize() + 96 : 0;
    // Retain the metadata consumed by sequence/stream selection, not another
    // complete journal text. Actual reads (or the existing current guard) still
    // precede this compact equality proof. No persistent format is changed.
    const FBlake3Hash TextHash = Projection ? Projection->TextHash : !CurrentHistory || bProvenCached ? FBlake3Hash() : FBlake3::HashBuffer(*Text, uint64(Text.Len()) * sizeof(TCHAR));
    const bool bSame = Prior && (bProvenCached || Prior->TextHash == TextHash);
    FDurableConsumerHistoryState::FRead Candidate;
    auto& Read = bSame ? *Prior : Candidate;
    if (bSame) ++Reused;
    const auto ValidateOwnerMetadata = [&]() {
      if (Read.bOwner && Read.Owner == OwnerId && (StreamId.IsEmpty() || (Read.bStream && Read.Stream == StreamId))
          && (!Read.bStream || !Read.bSequenceValid)) {
        SetFailure(OutFailure, UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_consumer_metadata_invalid"),
            TEXT("Consumer metadata requires a stream and positive sequence."), TEXT("workspace_durable_transaction"), Path)); return false;
      }
      return true;
    };
    if (!bSame || NeedsBody(Read)) {
      const double ParseStarted = FPlatformTime::Seconds();
      // Sequence allocation and stream selection consume metadata only. Do not
      // materialize every historical artifact/intent/delta as a JSON tree.
      FString Sequence, SyntaxError; bool bSyntaxValid;
      if (Projection && !bSame) {
        Read = MoveTemp(Projection->Read); Sequence = MoveTemp(Projection->Sequence);
        SyntaxError = MoveTemp(Projection->SyntaxError); bSyntaxValid = Projection->bSyntaxValid;
      } else bSyntaxValid = ParseMetadata(Text, Read, Sequence, SyntaxError, bSame && Read.Record.Sequence > BodyAfterSequence);
      if (!bSyntaxValid) {
        if (bCompleted && FPaths::GetPath(Path) == CompletedRoot
            && (!Read.bOwner || Read.Owner != OwnerId || (!StreamId.IsEmpty() && Read.Stream != StreamId))) {
          // Unknown legacy bytes have no attributable effect for this stream.
          // Preserve them for explicit recovery; do not turn them into another
          // target's blocking error or manufacture a committed delta.
          UE_LOG(LogTemp, Warning, TEXT("UAI durable legacy receipt preserved path=%s reason=unattributed_invalid_metadata"), *Path);
          return true;
        }
        SetFailure(OutFailure, UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_consumer_metadata_invalid"),
            SyntaxError, TEXT("workspace_durable_transaction"), Path)); return false;
      }
      Read.bSequenceValid = LexTryParseString(Read.Record.Sequence, *Sequence)
          && Read.Record.Sequence > 0 && LexToString(Read.Record.Sequence) == Sequence;
      if (!ValidateOwnerMetadata()) return false;
      Read.Record.OwnerId = Read.Owner; Read.Record.StreamId = Read.Stream;
      if (NeedsBody(Read)) {
        const auto BodyReader = TJsonReaderFactory<TCHAR>::Create(Text);
        EJsonNotation Notation;
        int32 BodyDepth = 0, EvidenceDepth = -1;
        while (BodyReader->ReadNext(Notation)) {
          const FString& Field = BodyReader->GetIdentifier();
          if (Notation == EJsonNotation::ObjectStart || Notation == EJsonNotation::ArrayStart) {
            ++BodyDepth;
            if (BodyDepth == 2 && Field == TEXT("consumer_evidence") && Notation == EJsonNotation::ObjectStart) EvidenceDepth = BodyDepth;
          } else if (Notation == EJsonNotation::ObjectEnd || Notation == EJsonNotation::ArrayEnd) {
            if (EvidenceDepth == BodyDepth) EvidenceDepth = -1;
            --BodyDepth;
          } else if (Notation == EJsonNotation::String && BodyDepth == EvidenceDepth) {
            if (Field == TEXT("intent") && bWantIntent) { Read.Record.IntentJson = BodyReader->GetValueAsString(); Read.bIntentLoaded = true; }
            if (Field == TEXT("verified") && bWantVerified) { Read.Record.VerifiedJson = BodyReader->GetValueAsString(); Read.bVerifiedLoaded = true; }
          }
        }
      }
      ParseSeconds += Projection ? Projection->WorkSeconds : FPlatformTime::Seconds() - ParseStarted;
      ++Parsed;
      if (!bSame) Read.TextHash = TextHash;
    } else if (!ValidateOwnerMetadata()) return false;
    const bool bSelected = Read.bOwner && Read.Owner.Equals(OwnerId, ESearchCase::CaseSensitive)
        && (StreamId.IsEmpty() || (Read.bStream && Read.Stream.Equals(StreamId, ESearchCase::CaseSensitive)));
    if (bSelected && !StreamId.IsEmpty()) {
      if (!Read.bHasIntent || !Read.bHasVerified || Read.Schema != JournalSchema || Read.RequestId.IsEmpty()
          || Read.Record.RequestKey != FMD5::HashAnsiString(*Read.RequestId).ToLower() || Read.Record.Phase.IsEmpty()) {
        SetFailure(OutFailure, UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_consumer_record_invalid"),
            TEXT("The selected consumer record has incomplete identity or evidence fields."), TEXT("workspace_durable_transaction"), Path)); return false;
      }
    } else if (!Read.Owner.Equals(OwnerId, ESearchCase::CaseSensitive) || (!StreamId.IsEmpty() && Read.Stream != StreamId)) {
      Read.Record.IntentJson.Empty(); Read.Record.VerifiedJson.Empty(); Read.bIntentLoaded = false; Read.bVerifiedLoaded = false;
    }
    // Copy only the selected record; unrelated metadata remains in the same
    // request-local backing and has no retained full-text copy.
    FDurableConsumerRecord Record;
    if (bSelected) {
      Record.OwnerId = Read.Owner; Record.StreamId = Read.Stream; Record.Sequence = Read.Record.Sequence;
      Record.RequestKey = Read.Record.RequestKey; Record.Phase = Read.Record.Phase;
      if (Record.Sequence > BodyAfterSequence) {
        if (bWantIntent) Record.IntentJson = Read.Record.IntentJson;
        if (bWantVerified) Record.VerifiedJson = Read.Record.VerifiedJson;
      }
      if (bWantVerified) Record.Material = FScopedDurableConsumer::FindCommittedMaterial(Record, &Record.bCurrentRequestCommit);
    }
    if (bSelected && !bCompleted) ActiveRecordKeys.Add(Record.RequestKey);
    if (Proof && Guard) { Read.Proof = MoveTemp(Proof); Read.Guard = MoveTemp(Guard); }
    if (CurrentHistory) {
      int64 Charge = Read.Charge() + Path.GetAllocatedSize() + 96;
      if (CurrentHistory->Bytes - OldCharge + Charge > 64ll * 1024 * 1024 && (Read.bIntentLoaded || Read.bVerifiedLoaded)) {
        // The selected result was copied above. If its optional body cannot
        // remain, keep the small metadata; its next body consumer rereads it.
        Read.Record.IntentJson.Empty(); Read.Record.VerifiedJson.Empty(); Read.bIntentLoaded = false; Read.bVerifiedLoaded = false;
        Charge = Read.Charge() + Path.GetAllocatedSize() + 96;
      }
      if (CurrentHistory->Bytes - OldCharge + Charge <= 64ll * 1024 * 1024) {
        CurrentHistory->Bytes += Charge - OldCharge;
        if (!bSame) {
          if (Prior && Prior->Guard) --CurrentHistory->ObservedReceipts;
          if (Candidate.Guard) ++CurrentHistory->ObservedReceipts;
          CurrentHistory->Reads.Add(Path, MoveTemp(Candidate));
        } else if (!bProvenCached && Read.Guard) ++CurrentHistory->ObservedReceipts;
      } else if (Prior) {
        if (Prior->Guard && bProvenCached) --CurrentHistory->ObservedReceipts;
        CurrentHistory->Bytes -= OldCharge; CurrentHistory->Reads.Remove(Path);
      }
    }
    if (!bSelected) return true;
    if (const int32* Existing = Requests.Find(Record.RequestKey)) {
      const auto& ExistingRecord = OutRecords[*Existing];
      if (ExistingRecord.Sequence != Record.Sequence || (!StreamId.IsEmpty() &&
          (!ExistingRecord.IntentJson.Equals(Record.IntentJson, ESearchCase::CaseSensitive) ||
          !ExistingRecord.VerifiedJson.Equals(Record.VerifiedJson, ESearchCase::CaseSensitive)))) {
        SetFailure(OutFailure, UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_consumer_record_conflict"),
            TEXT("Active and completed consumer evidence disagree."), TEXT("workspace_durable_transaction"), Path)); return false;
      }
      // The active file is the owner's latest state, including a deliberate
      // reconciliation after an archived commit. An old completed receipt must
      // not turn restored native content back into an applied delta.
      if (!bCompleted || !ActiveRecordKeys.Contains(Record.RequestKey)) OutRecords[*Existing] = MoveTemp(Record);
    } else {
      Requests.Add(Record.RequestKey, OutRecords.Num());
      OutRecords.Add(MoveTemp(Record));
    }
    return true;
  };
  struct FMetadataWork {
    const FString* Path = nullptr; FString Text; bool bCompleted = false;
    UE::UAI::HjsonWorkspace::FWorkspaceFileProofPtr Proof;
    TUniquePtr<UE::UAI::ArtifactPublisher::FGenerationFileGuard> Guard;
    FMetadataProjection Projection;
  };
  TArray<FMetadataWork> MetadataBatch;
  const bool bPureMetadataMode = !bWantIntent && !bWantVerified;
  const int32 MetadataWidth = FMath::Max(1, FTaskGraphInterface::Get().GetNumWorkerThreads() + 1);
  int32 MetadataBatches = 0, MetadataJobs = 0, PendingReceiptGuards = 0;
  int64 MetadataPeakCharacters = 0; double MetadataWallSeconds = 0, MetadataWorkerSeconds = 0;
  const auto FlushMetadata = [&]() {
    if (MetadataBatch.IsEmpty()) return true;
    ++MetadataBatches;
    int64 Characters = 0; for (const auto& Work : MetadataBatch) Characters += Work.Text.Len();
    MetadataPeakCharacters = FMath::Max(MetadataPeakCharacters, Characters);
    const double BatchStarted = FPlatformTime::Seconds();
    ParallelFor(MetadataBatch.Num(), [&](int32 Index) {
      auto& Work = MetadataBatch[Index]; auto& Projection = Work.Projection;
      if (!Projection.bShouldParse) return;
      const double WorkStarted = FPlatformTime::Seconds();
      Projection.bSyntaxValid = ParseMetadata(Work.Text, Projection.Read, Projection.Sequence, Projection.SyntaxError, false);
      Projection.WorkSeconds = FPlatformTime::Seconds() - WorkStarted;
    });
    MetadataWallSeconds += FPlatformTime::Seconds() - BatchStarted;
    for (auto& Work : MetadataBatch) {
      MetadataJobs += Work.Projection.bShouldParse ? 1 : 0;
      MetadataWorkerSeconds += Work.Projection.WorkSeconds;
      if (Work.Proof && Work.Guard) --PendingReceiptGuards;
      if (!Consume(*Work.Path, MoveTemp(Work.Text), Work.bCompleted, false,
          MoveTemp(Work.Proof), MoveTemp(Work.Guard), &Work.Projection)) return false;
    }
    MetadataBatch.Reset(); return true;
  };
  const auto ConsumeRead = [&](const FString& Path, FString Text, bool bCompleted,
      UE::UAI::HjsonWorkspace::FWorkspaceFileProofPtr Proof,
      TUniquePtr<UE::UAI::ArtifactPublisher::FGenerationFileGuard> Guard) {
    if (!bPureMetadataMode) {
      return Consume(Path, MoveTemp(Text), bCompleted, false, MoveTemp(Proof), MoveTemp(Guard));
    }
    auto& Work = MetadataBatch.AddDefaulted_GetRef(); Work.Path = &Path;
    Work.Text = MoveTemp(Text); Work.bCompleted = bCompleted;
    Work.Proof = MoveTemp(Proof); Work.Guard = MoveTemp(Guard);
    if (Work.Proof && Work.Guard) ++PendingReceiptGuards;
    auto* Prior = CurrentHistory ? CurrentHistory->Reads.Find(Path) : nullptr;
    Work.Projection.TextHash = CurrentHistory ? FBlake3::HashBuffer(*Work.Text, uint64(Work.Text.Len()) * sizeof(TCHAR)) : FBlake3Hash();
    Work.Projection.bShouldParse = !Prior || Prior->TextHash != Work.Projection.TextHash;
    return MetadataBatch.Num() < MetadataWidth || FlushMetadata();
  };
  for (const FString& Path : Files) {
    const bool bCompleted = Path.StartsWith((Root / TEXT("completed")) + TEXT("/"));
    auto* Prior = CurrentHistory ? CurrentHistory->Reads.Find(Path) : nullptr;
    const double ProofStarted = FPlatformTime::Seconds();
    const bool bStableReceipt = bCompleted && !ActiveRequests.Contains(FPaths::GetBaseFilename(Path));
    const bool bNeedsRecord = Prior && NeedsBody(*Prior);
    const bool bGuarded = bStableReceipt && !bNeedsRecord && Prior && Prior->Guard && Prior->Proof
        && Prior->Proof->IsCurrentHandle(Prior->Guard->GetCurrentReadHandle(Path));
    ProofSeconds += FPlatformTime::Seconds() - ProofStarted;
    if (bGuarded) {
      ++GuardReused;
      if (!FlushMetadata() || !Consume(Path, FString(), true, true, {}, {})) return false;
      continue;
    }
    if (Prior && Prior->Guard) {
      CurrentHistory->Bytes -= Prior->Guard->GetRetainedBytes();
      Prior->Guard.Reset(); Prior->Proof.Reset(); --CurrentHistory->ObservedReceipts;
    }
    const double ReadStarted = FPlatformTime::Seconds();
    const auto ReadCpuStarted = UE::UAI::Timing::SampleThreadCpu();
    FString Text;
    UE::UAI::Contract::FDiagnostic Error;
    UE::UAI::HjsonWorkspace::FWorkspaceFileProofPtr Proof;
    TUniquePtr<UE::UAI::ArtifactPublisher::FGenerationFileGuard> Guard;
    bool bRead = false;
    // These are private immutable completed receipts, not user input. Their
    // request-ID recovery retains old paths. The finite request scope keeps
    // their current namespace binding and excludes writers; a granted read
    // lease additionally excludes an already writable mapping. Active journals,
    // unavailable proof and every budget miss keep the original actual read.
    if (bStableReceipt && CurrentHistory && CurrentHistory->ObservedReceipts + PendingReceiptGuards < 2048 && bMayAcquireReceiptProof) {
      bool bBudgetExhausted = false;
      const double AcquireStarted = FPlatformTime::Seconds();
      Proof = UE::UAI::HjsonWorkspace::TryBeginWorkspaceFileObservation(Path, &bBudgetExhausted);
      AcquireSeconds += FPlatformTime::Seconds() - AcquireStarted;
      if (bBudgetExhausted) { ++ProofCapacityMisses; bMayAcquireReceiptProof = false; }
      if (Proof) {
        const double GuardStarted = FPlatformTime::Seconds();
        if (!CurrentHistory->ReceiptDirectories)
          CurrentHistory->ReceiptDirectories = MakeUnique<UE::UAI::ArtifactPublisher::FGenerationDirectoryScope>();
        Guard = MakeUnique<UE::UAI::ArtifactPublisher::FGenerationFileGuard>();
        UE::UAI::ArtifactPublisher::FArtifactPublishError GuardError;
        TArray<uint8> Bytes;
        if (!Guard->Open(Path, false, false, GuardError) || !Guard->Read(Bytes, GuardError)) {
          if (!FlushMetadata()) return false;
          SetFailure(OutFailure, MoveTemp(GuardError)); return false;
        }
        ++PhysicalReads;
        if (!Proof->IsCurrentHandle(Guard->GetCurrentReadHandle(Path))) {
          if (!FlushMetadata()) return false;
          SetFailure(OutFailure, UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_receipt_observation_changed"),
              TEXT("The selected receipt observation changed during its guarded read; the request stopped without selecting another reader."),
              TEXT("workspace_durable_transaction"), Path)); return false;
        }
        FFileHelper::BufferToString(Text, Bytes.GetData(), Bytes.Num()); bRead = true; ++GuardAcquired;
        GuardReadSeconds += FPlatformTime::Seconds() - GuardStarted;
      }
      if (!bRead) { ++GuardRejected; Proof.Reset(); Guard.Reset(); }
    } else if (bStableReceipt && CurrentHistory && CurrentHistory->ObservedReceipts + PendingReceiptGuards < 2048 && !bMayAcquireReceiptProof) ++SkippedProofAcquisitions;
    if (!bRead) {
      const double LoadStarted = FPlatformTime::Seconds();
      ++PhysicalReads; bRead = LoadJournalText(Path, Text, Error);
      UnguardedReadSeconds += FPlatformTime::Seconds() - LoadStarted;
    }
    ReadSeconds += FPlatformTime::Seconds() - ReadStarted;
    const double ReadCpu = UE::UAI::Timing::ThreadCpuSeconds(ReadCpuStarted, UE::UAI::Timing::SampleThreadCpu());
    if (ReadCpu >= 0) ReadCpuSeconds += ReadCpu; else bReadCpuValid = false;
    if (!bRead) { if (!FlushMetadata()) return false; SetFailure(OutFailure, MoveTemp(Error)); return false; }
    if (!ConsumeRead(Path, MoveTemp(Text), bCompleted, MoveTemp(Proof), MoveTemp(Guard))) return false;
  }
  if (!FlushMetadata()) return false;
  UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=durable_metadata_tasks batches=%d jobs=%d width=%d task_wall_s=%.6f worker_elapsed_sum_s=%.6f peak_batch_text_characters=%lld io_and_selection=original_file_order engine_json_grammar=true"),
      MetadataBatches, MetadataJobs, MetadataWidth, MetadataWallSeconds, MetadataWorkerSeconds, MetadataPeakCharacters);
  OutRecords.Sort([](const auto& A, const auto& B) { return A.Sequence < B.Sequence; });
  // Allocation consumes only max(sequence); exact accepted-source lookup only
  // needs matching committed input. Ordering ambiguity belongs to the stream
  // publication consumer, not either of these pre-native decisions.
  if (CurrentHistory) for (auto It = CurrentHistory->Reads.CreateIterator(); It; ++It)
    if (It.Key().StartsWith(Root + TEXT("/")) && !CurrentPaths.Contains(It.Key())) {
      CurrentHistory->Bytes -= It.Value().Charge() + It.Key().GetAllocatedSize() + 96;
      if (It.Value().Guard) --CurrentHistory->ObservedReceipts;
      It.RemoveCurrent();
    }
  const double Total = FPlatformTime::Seconds() - Started;
  UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=durable_consumer_history files=%d actual_reads=%d guard_acquired=%d guard_reused=%d guard_rejected=%d guarded_retained=%d parsed=%d reused=%d selected=%d retained_bytes=%lld enumerate_s=%.6f read_s=%.6f proof_s=%.6f parse_s=%.6f migration_s=%.6f selection_s=%.6f total_s=%.6f read_cpu_s=%.6f total_cpu_s=%.6f body_mode=%d body_after=%lld proof_capacity_misses=%d proof_acquisitions_skipped=%d acquire_s=%.6f guard_read_s=%.6f unguarded_read_s=%.6f parse_timing=%s total_cpu_scope=calling_thread"),
      LogicalRows, PhysicalReads, GuardAcquired, GuardReused, GuardRejected, CurrentHistory ? CurrentHistory->ObservedReceipts : 0,
      Parsed, Reused, OutRecords.Num(), CurrentHistory ? CurrentHistory->Bytes : 0,
      Enumerated - Started, ReadSeconds, ProofSeconds, ParseSeconds, MigrationSeconds,
      Total - (Enumerated - Started) - ReadSeconds - ProofSeconds - (bPureMetadataMode ? MetadataWallSeconds : ParseSeconds) - MigrationSeconds, Total,
      bReadCpuValid ? ReadCpuSeconds : -1.0, UE::UAI::Timing::ThreadCpuSeconds(CpuStarted, UE::UAI::Timing::SampleThreadCpu()),
      static_cast<int32>(StreamId.IsEmpty() ? EDurableConsumerBody::Metadata : Body), BodyAfterSequence,
      ProofCapacityMisses, SkippedProofAcquisitions, AcquireSeconds, GuardReadSeconds, UnguardedReadSeconds,
      bPureMetadataMode ? TEXT("worker_elapsed_sum") : TEXT("serial_wall"));
  return true;
}

FScopedDurableRecoveryNotice::FScopedDurableRecoveryNotice(FString InOriginRequestId)
    : Previous(CurrentRecoveryNotice), OriginRequestId(MoveTemp(InOriginRequestId)) {
  if (OriginRequestId.IsEmpty() && Previous) OriginRequestId = Previous->OriginRequestId;
  CurrentRecoveryNotice = this;
}

FScopedDurableRecoveryNotice::~FScopedDurableRecoveryNotice() {
  if (Previous) {
    Previous->bMemoryEvidencePreserved |= bMemoryEvidencePreserved;
    for (const auto& Id : JournalIds) Previous->RetainJournal(Id);
  }
  CurrentRecoveryNotice = Previous;
}

struct FDurableTransactionCoordinator::FImpl {
  FDurableJournalState State;
  TArray<FDurableArtifact> PublicArtifacts;
  FOperationFailure InventoryFailure;
  bool bPrepared = false;
  FScopedDurableConsumer* Consumer = nullptr; // Synchronous request scope, never retained after commit.
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
  TArray<FString> RequestDirectories;
  IFileManager::Get().FindFiles(RequestDirectories,
      *(ActiveRoot / (RequestKeyFor(Options.RequestId) + TEXT("*"))), false, true);
  for (const auto& Directory : RequestDirectories) {
    const FString Path = ActiveRoot / Directory / JournalFileName;
    if (IFileManager::Get().FileExists(*Path)) JournalFiles.Add(Path);
  }
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
    if (State.RequestId != Options.RequestId) continue;
    if (State.Phase == TEXT("committed") || State.Phase == TEXT("restored") || State.Phase == TEXT("memory_complete")) {
      if (State.Phase == TEXT("committed") && !State.Consumer.OwnerId.IsEmpty() &&
          !WriteJournal(State, Error, CompletedPathFor(JournalRoot, State.RequestKey, State.Consumer.OwnerId, State.Consumer.StreamId))) {
        SetFailure(OutFailure, MoveTemp(Error)); return false;
      }
      if (State.Phase == TEXT("committed") && HasPendingConsumerObservation(State)) RetainObservationMaterial(State, InOutResult);
      else CleanupCompletedTransaction(State, InOutResult);
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
  FString CompletedPath;
  UE::UAI::Contract::FDiagnostic Error;
  if (!FindCompletedPathFor(Options.JournalRoot, RequestKey, CompletedPath, Error,
      CurrentDurableConsumer ? CurrentDurableConsumer->OwnerId : FString(),
      CurrentDurableConsumer ? CurrentDurableConsumer->StreamId : FString())) {
    SetFailure(OutFailure, MoveTemp(Error)); return false;
  }
  TSharedPtr<FJsonObject> Object;
  FDurableJournalState State;
  if (!IFileManager::Get().FileExists(*CompletedPath)) return true;
  FString CompletedText;
  if (!LoadJournalText(CompletedPath, CompletedText, Error)
      || !ParseJournalText(CompletedPath, CompletedText, Object, Error)) {
    SetFailure(OutFailure, MakeDurableFailure(TEXT("durable_replay_receipt_invalid"),
        TEXT("The completed transaction receipt cannot be replayed."), Error, CompletedPath)); return false;
  }
  if (!JsonToJournal(CompletedPath, Object.ToSharedRef(), State, Error,
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
  if (CurrentRecoveryNotice) State.OriginRequestId = CurrentRecoveryNotice->GetOriginRequestId();
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

  Impl->Consumer = CurrentDurableConsumer;
  if (Impl->Consumer) {
    TArray<FDurableConsumerRecord> Records;
    if (!ReadDurableConsumerRecords(JournalRoot, Impl->Consumer->OwnerId, Impl->Consumer->StreamId,
        Records, OutFailure, EDurableConsumerBody::Metadata)) return false;
    if (!Records.IsEmpty() && Records.Last().Sequence == MAX_int64) {
      SetFailure(OutFailure, UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_consumer_sequence_exhausted"),
          TEXT("The consumer sequence cannot advance."), TEXT("workspace_durable_transaction"))); return false;
    }
    State.Consumer.OwnerId = Impl->Consumer->OwnerId;
    State.Consumer.StreamId = Impl->Consumer->StreamId;
    State.Consumer.IntentJson = Impl->Consumer->IntentJson;
    State.Consumer.Sequence = Records.IsEmpty() ? 1 : Records.Last().Sequence + 1;
  }
  State.CompletedPath = CompletedPathFor(JournalRoot, State.RequestKey, State.Consumer.OwnerId, State.Consumer.StreamId);

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
  int32 Confirmed = 0;
  ON_SCOPE_EXIT { RefreshPublishedArtifacts(Impl->State.Artifacts, Confirmed); };
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
    ++Confirmed;
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
  int32 Confirmed = 0;
  ON_SCOPE_EXIT { RefreshPublishedArtifacts(Impl->State.Artifacts, Confirmed); };
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
    ++Confirmed;
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
  // The combined restore is confirmed and its storage was actually removed.
  // Do not advertise the now-unreadable journal as recovery custody. Nested
  // callers may already have seen SyncResult, so retire it in that same scope.
  const FString RestoredId = MakeDurableJournalRecoveryId(Impl->State.JournalPath);
  for (auto* Notice = CurrentRecoveryNotice; Notice; Notice = Notice->Previous)
    Notice->JournalIds.Remove(RestoredId);
  InOutResult.JournalPath.Reset();
  Impl->bPrepared = false;
  return true;
}

bool FDurableTransactionCoordinator::SealConsumerEvidence(FOperationFailure& OutFailure) {
  if (!Impl->bPrepared || !Impl->Consumer) return true;
  FString Verified;
  TSharedPtr<const IDurableConsumerMaterial> Material;
  if (!Impl->Consumer->Seal || !Impl->Consumer->Seal(Impl->State.Consumer.Sequence, Verified, Material, OutFailure)) return false;
  if (Verified.IsEmpty()) {
    SetFailure(OutFailure, UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_consumer_evidence_empty"),
        TEXT("A registered durable consumer returned no verified evidence."), TEXT("workspace_durable_transaction")));
    return false;
  }
  Impl->State.Consumer.VerifiedJson = MoveTemp(Verified);
  Impl->State.Consumer.Material = MoveTemp(Material);
  UE::UAI::Contract::FDiagnostic Error;
  if (!WriteJournal(Impl->State, Error)) { SetFailure(OutFailure, MoveTemp(Error)); return false; }
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
  if (Impl->Consumer && Impl->State.Consumer.VerifiedJson.IsEmpty()) {
    SetFailure(OutFailure, UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_consumer_evidence_missing"),
        TEXT("The registered consumer must seal its evidence before native commit."), TEXT("workspace_durable_transaction")));
    return false;
  }
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
  if (Impl->Consumer) Impl->Consumer->CommittedSequence = Impl->State.Consumer.Sequence;
  SyncResult(Impl->State, InOutResult);
  if (Impl->Consumer && Impl->Consumer->bObservationPending) {
    RetainObservationMaterial(Impl->State, InOutResult);
  } else CleanupCompletedTransaction(Impl->State, InOutResult);
  // The completed receipt remains authoritative. Move, rather than copy, the
  // already sealed body into its active consumer until this request refreshes.
  // A reader only receives the pure material after reading the matching record.
  if (Impl->Consumer) {
    Impl->State.Consumer.RequestKey = Impl->State.RequestKey;
    Impl->State.Consumer.Phase = Impl->State.Phase;
    Impl->Consumer->CommittedRecords.Add(MoveTemp(Impl->State.Consumer));
  }
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

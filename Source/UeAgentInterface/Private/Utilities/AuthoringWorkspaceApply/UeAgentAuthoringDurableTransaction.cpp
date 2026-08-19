// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringDurableTransaction.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace {
using namespace UeAgentAuthoringWorkspaceTransaction;

constexpr const TCHAR *JournalSchema = TEXT("uai.apply_durable_journal.v1");
constexpr const TCHAR *JournalFileName = TEXT("journal.json");

struct FDurableArtifactState {
  FString ArtifactId;
  FString FinalPath;
  FString StagingPath;
  FString BackupPath;
  FString QuarantinePath;
  FString OldHash;
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

void SetFailure(FOperationFailure &OutFailure, const FString &Code,
                const FString &Detail = FString()) {
  OutFailure = FOperationFailure();
  OutFailure.ErrorCode = Code;
  OutFailure.ErrorDetail = Detail.IsEmpty() ? Code : Detail;
  OutFailure.Diagnostic.Code = Code;
  OutFailure.Diagnostic.Stage = TEXT("workspace_durable_transaction");
  OutFailure.Diagnostic.Message = OutFailure.ErrorDetail;
  OutFailure.Diagnostic.bRetrySafe = false;
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
                     FString &OutError) {
  if (!EnsureParentDirectory(Path)) {
    OutError = TEXT("durable_journal_parent_create_failed:") + Path;
    return false;
  }
  const FString TemporaryPath = Path + TEXT(".tmp");
  IPlatformFile &PlatformFile =
      FPlatformFileManager::Get().GetPlatformFile();
  TUniquePtr<IFileHandle> Handle(
      PlatformFile.OpenWrite(*TemporaryPath, false, false));
  if (!Handle) {
    OutError = TEXT("durable_journal_open_failed:") + TemporaryPath;
    return false;
  }
  const FTCHARToUTF8 Utf8(*Text);
  if (!Handle->Write(reinterpret_cast<const uint8 *>(Utf8.Get()),
                     Utf8.Length()) ||
      !Handle->Flush(true)) {
    Handle.Reset();
    IFileManager::Get().Delete(*TemporaryPath, false, true, true);
    OutError = TEXT("durable_journal_flush_failed:") + TemporaryPath;
    return false;
  }
  Handle.Reset();
  if (!IFileManager::Get().Move(*Path, *TemporaryPath, true, false, false,
                                true)) {
    IFileManager::Get().Delete(*TemporaryPath, false, true, true);
    OutError = TEXT("durable_journal_replace_failed:") + Path;
    return false;
  }
  return true;
}

bool LoadJson(const FString &Path, TSharedPtr<FJsonObject> &OutObject) {
  FString Text;
  if (!FFileHelper::LoadFileToString(Text, *Path))
    return false;
  const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
  return FJsonSerializer::Deserialize(Reader, OutObject) &&
         OutObject.IsValid();
}

FString FileHash(const FString &Path) {
  return LexToString(FMD5Hash::HashFile(*Path)).ToLower();
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
    Object->SetStringField(TEXT("final_path"), Artifact.FinalPath);
    Object->SetStringField(TEXT("staging_path"), Artifact.StagingPath);
    Object->SetStringField(TEXT("backup_path"), Artifact.BackupPath);
    Object->SetStringField(TEXT("quarantine_path"),
                           Artifact.QuarantinePath);
    Object->SetBoolField(TEXT("old_exists"), Artifact.bOldExists);
    Object->SetBoolField(TEXT("staged_exists"), Artifact.bStagedExists);
    if (!Artifact.OldHash.IsEmpty())
      Object->SetStringField(TEXT("old_hash"), Artifact.OldHash);
    ArtifactValues.Add(MakeShared<FJsonValueObject>(Object));
  }
  Root->SetArrayField(TEXT("artifacts"), ArtifactValues);
  return Root;
}

bool JsonToJournal(const FString &JournalPath,
                   const TSharedRef<FJsonObject> &Root,
                   FDurableJournalState &OutState, FString &OutError,
                   const bool bRequireJournalWithinTransactionRoot = true) {
  FString Schema;
  if (!Root->TryGetStringField(TEXT("schema"), Schema) ||
      Schema != JournalSchema ||
      !Root->TryGetStringField(TEXT("request_id"), OutState.RequestId) ||
      !Root->TryGetStringField(TEXT("request_key"), OutState.RequestKey) ||
      !Root->TryGetStringField(TEXT("request_fingerprint"),
                               OutState.RequestFingerprint) ||
      !Root->TryGetStringField(TEXT("transaction_root"),
                               OutState.TransactionRoot) ||
      !Root->TryGetStringField(TEXT("phase"), OutState.Phase)) {
    OutError = TEXT("durable_journal_header_invalid:") + JournalPath;
    return false;
  }
  if (OutState.RequestId.IsEmpty() ||
      OutState.RequestKey !=
          FMD5::HashAnsiString(*OutState.RequestId).ToLower() ||
      FPaths::IsRelative(OutState.TransactionRoot)) {
    OutError = TEXT("durable_journal_identity_invalid:") + JournalPath;
    return false;
  }
  OutState.TransactionRoot = NormalizeAbsolutePath(OutState.TransactionRoot);
  OutState.JournalPath = NormalizeAbsolutePath(JournalPath);
  if (bRequireJournalWithinTransactionRoot &&
      (!IsWithinRoot(OutState.TransactionRoot, OutState.JournalPath) ||
       NormalizeAbsolutePath(FPaths::GetPath(OutState.JournalPath)) !=
           OutState.TransactionRoot)) {
    OutError = TEXT("durable_journal_path_escape:") + JournalPath;
    return false;
  }
  const TArray<TSharedPtr<FJsonValue>> *History = nullptr;
  if (!Root->TryGetArrayField(TEXT("phase_history"), History) || !History) {
    OutError = TEXT("durable_journal_history_missing:") + JournalPath;
    return false;
  }
  for (const TSharedPtr<FJsonValue> &Value : *History) {
    FString Phase;
    if (!Value.IsValid() || !Value->TryGetString(Phase)) {
      OutError = TEXT("durable_journal_history_invalid:") + JournalPath;
      return false;
    }
    OutState.PhaseHistory.Add(Phase);
  }
  if (OutState.PhaseHistory.IsEmpty() ||
      OutState.PhaseHistory.Last() != OutState.Phase) {
    OutError = TEXT("durable_journal_phase_mismatch:") + JournalPath;
    return false;
  }
  const TArray<TSharedPtr<FJsonValue>> *Artifacts = nullptr;
  if (!Root->TryGetArrayField(TEXT("artifacts"), Artifacts) || !Artifacts) {
    OutError = TEXT("durable_journal_artifacts_missing:") + JournalPath;
    return false;
  }
  TSet<FString> ArtifactIds;
  TSet<FString> FinalPaths;
  for (const TSharedPtr<FJsonValue> &Value : *Artifacts) {
    const TSharedPtr<FJsonObject> Object =
        Value.IsValid() ? Value->AsObject() : nullptr;
    if (!Object.IsValid()) {
      OutError = TEXT("durable_journal_artifact_invalid:") + JournalPath;
      return false;
    }
    FDurableArtifactState &Artifact = OutState.Artifacts.AddDefaulted_GetRef();
    if (!Object->TryGetStringField(TEXT("artifact_id"),
                                   Artifact.ArtifactId) ||
        !Object->TryGetStringField(TEXT("final_path"), Artifact.FinalPath) ||
        !Object->TryGetStringField(TEXT("staging_path"),
                                   Artifact.StagingPath) ||
        !Object->TryGetStringField(TEXT("backup_path"),
                                   Artifact.BackupPath) ||
        !Object->TryGetStringField(TEXT("quarantine_path"),
                                   Artifact.QuarantinePath) ||
        !Object->TryGetBoolField(TEXT("old_exists"), Artifact.bOldExists) ||
        !Object->TryGetBoolField(TEXT("staged_exists"),
                                 Artifact.bStagedExists)) {
      OutError = TEXT("durable_journal_artifact_fields_invalid:") +
                 JournalPath;
      return false;
    }
    Object->TryGetStringField(TEXT("old_hash"), Artifact.OldHash);
    if (Artifact.ArtifactId.IsEmpty() ||
        FPaths::IsRelative(Artifact.FinalPath) ||
        FPaths::IsRelative(Artifact.StagingPath) ||
        FPaths::IsRelative(Artifact.BackupPath) ||
        FPaths::IsRelative(Artifact.QuarantinePath)) {
      OutError = TEXT("durable_journal_artifact_identity_invalid:") +
                 Artifact.ArtifactId;
      return false;
    }
    Artifact.FinalPath = NormalizeAbsolutePath(Artifact.FinalPath);
    Artifact.StagingPath = NormalizeAbsolutePath(Artifact.StagingPath);
    Artifact.BackupPath = NormalizeAbsolutePath(Artifact.BackupPath);
    Artifact.QuarantinePath = NormalizeAbsolutePath(Artifact.QuarantinePath);
    const FString FoldedId = Artifact.ArtifactId.ToLower();
    const FString FoldedPath = Artifact.FinalPath.ToLower();
    if (ArtifactIds.Contains(FoldedId) || FinalPaths.Contains(FoldedPath) ||
        IsWithinRoot(OutState.TransactionRoot, Artifact.FinalPath) ||
        !IsWithinRoot(OutState.TransactionRoot, Artifact.StagingPath) ||
        !IsWithinRoot(OutState.TransactionRoot, Artifact.BackupPath) ||
        !IsWithinRoot(OutState.TransactionRoot, Artifact.QuarantinePath)) {
      OutError = TEXT("durable_journal_artifact_path_invalid:") +
                 Artifact.ArtifactId;
      return false;
    }
    ArtifactIds.Add(FoldedId);
    FinalPaths.Add(FoldedPath);
  }
  return true;
}

bool WriteJournal(const FDurableJournalState &State, FString &OutError,
                  const FString &OverridePath = FString()) {
  FString Text;
  if (!SerializeJson(JournalToJson(State), Text)) {
    OutError = TEXT("durable_journal_serialize_failed");
    return false;
  }
  return SaveDurableText(
      OverridePath.IsEmpty() ? State.JournalPath : OverridePath, Text,
      OutError);
}

bool RestoreArtifacts(const FDurableJournalState &State, FString &OutError) {
  for (const FDurableArtifactState &Artifact : State.Artifacts) {
    if (Artifact.bOldExists) {
      if (!IFileManager::Get().FileExists(*Artifact.BackupPath) ||
          !EnsureParentDirectory(Artifact.FinalPath) ||
          IFileManager::Get().Copy(*Artifact.FinalPath, *Artifact.BackupPath,
                                   true, true) != COPY_OK) {
        OutError = TEXT("durable_restore_copy_failed:") +
                   Artifact.ArtifactId;
        return false;
      }
      if (!Artifact.OldHash.IsEmpty() &&
          FileHash(Artifact.FinalPath) != Artifact.OldHash) {
        OutError = TEXT("durable_restore_hash_mismatch:") +
                   Artifact.ArtifactId;
        return false;
      }
    } else if (IFileManager::Get().FileExists(*Artifact.FinalPath) &&
               !IFileManager::Get().Delete(*Artifact.FinalPath, false, true,
                                            true)) {
      OutError = TEXT("durable_restore_delete_failed:") +
                 Artifact.ArtifactId;
      return false;
    }
  }
  return true;
}

bool FinalArtifactsMatchOriginal(const FDurableJournalState &State,
                                 FString &OutError) {
  for (const FDurableArtifactState &Artifact : State.Artifacts) {
    const bool bFinalExists =
        IFileManager::Get().FileExists(*Artifact.FinalPath);
    if (bFinalExists != Artifact.bOldExists ||
        (bFinalExists && FileHash(Artifact.FinalPath) != Artifact.OldHash)) {
      OutError = TEXT("durable_stage_modified_final:") +
                 Artifact.ArtifactId;
      return false;
    }
  }
  return true;
}

bool AppendPhaseAndWrite(FDurableJournalState &State, const FString &Phase,
                         FString &OutError) {
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

struct FDurableTransactionCoordinator::FImpl {
  FDurableJournalState State;
  TArray<FDurableArtifact> PublicArtifacts;
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
  for (const FString &JournalPath : JournalFiles) {
    TSharedPtr<FJsonObject> Object;
    FDurableJournalState State;
    FString Error;
    if (!LoadJson(JournalPath, Object) ||
        !JsonToJournal(JournalPath, Object.ToSharedRef(), State, Error)) {
      SetFailure(OutFailure, TEXT("durable_recovery_journal_invalid"),
                 Error.IsEmpty() ? JournalPath : Error);
      InOutResult.RecoveryStatus = TEXT("failed");
      return false;
    }
    if (State.Phase == TEXT("committed") || State.Phase == TEXT("restored")) {
      IFileManager::Get().DeleteDirectory(*State.TransactionRoot, false, true);
      continue;
    }
    if (!RestoreArtifacts(State, Error)) {
      SetFailure(OutFailure, TEXT("durable_startup_restore_failed"), Error);
      InOutResult.RecoveryStatus = TEXT("failed");
      return false;
    }
    if (!AppendPhaseAndWrite(State, TEXT("startup_restored"), Error)) {
      SetFailure(OutFailure, TEXT("durable_recovery_journal_write_failed"),
                 Error);
      InOutResult.RecoveryStatus = TEXT("failed");
      return false;
    }
    if (!IFileManager::Get().DeleteDirectory(*State.TransactionRoot, false,
                                             true)) {
      SetFailure(OutFailure, TEXT("durable_recovery_cleanup_failed"),
                 State.TransactionRoot);
      InOutResult.RecoveryStatus = TEXT("failed");
      return false;
    }
    bRecoveredAny = true;
  }
  if (bRecoveredAny) {
    InOutResult.RecoveryStatus = TEXT("restored");
    InOutResult.PhaseHistory.Add(TEXT("startup_restored"));
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
  FString Error;
  if (!LoadJson(CompletedPath, Object) ||
      !JsonToJournal(CompletedPath, Object.ToSharedRef(), State, Error,
                     false)) {
    SetFailure(OutFailure, TEXT("durable_replay_receipt_invalid"),
               Error.IsEmpty() ? CompletedPath : Error);
    return false;
  }
  if (State.RequestId != Options.RequestId ||
      State.RequestFingerprint != RequestFingerprint ||
      State.Phase != TEXT("committed")) {
    SetFailure(OutFailure, TEXT("durable_replay_identity_mismatch"),
               Options.RequestId);
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
    SetFailure(OutFailure, TEXT("durable_backend_not_supported"));
    return false;
  }
  Impl->PublicArtifacts.Reset();
  if (!Backend.BuildDurableArtifactInventory(
          Mutations, Impl->PublicArtifacts, OutFailure)) {
    if (OutFailure.ErrorCode.IsEmpty())
      SetFailure(OutFailure, TEXT("durable_inventory_failed"));
    OutFailure.Diagnostic.bRetrySafe = false;
    return false;
  }
  if (Impl->PublicArtifacts.IsEmpty()) {
    SetFailure(OutFailure, TEXT("durable_inventory_empty"));
    return false;
  }

  FDurableJournalState &State = Impl->State;
  State = FDurableJournalState();
  State.RequestId = Options.RequestId;
  State.RequestKey = RequestKeyFor(Options.RequestId);
  State.RequestFingerprint = RequestFingerprint;
  const FString JournalRoot = NormalizeAbsolutePath(Options.JournalRoot);
  State.TransactionRoot = JournalRoot / TEXT("active") / State.RequestKey;
  State.JournalPath = State.TransactionRoot / JournalFileName;
  State.CompletedPath = CompletedPathFor(JournalRoot, State.RequestKey);
  State.PhaseHistory = InOutResult.PhaseHistory;

  if (IFileManager::Get().DirectoryExists(*State.TransactionRoot) ||
      !IFileManager::Get().MakeDirectory(*State.TransactionRoot, true)) {
    SetFailure(OutFailure, TEXT("durable_transaction_root_unavailable"),
               State.TransactionRoot);
    return false;
  }
  const auto CleanupFailedPrepare = [&State, &OutFailure]() {
    if (IFileManager::Get().DirectoryExists(*State.TransactionRoot) &&
        !IFileManager::Get().DeleteDirectory(*State.TransactionRoot, false,
                                             true)) {
      if (!OutFailure.ErrorDetail.IsEmpty())
        OutFailure.ErrorDetail += TEXT(";");
      OutFailure.ErrorDetail +=
          TEXT("durable_prepare_cleanup_failed:") + State.TransactionRoot;
      OutFailure.Diagnostic.Message = OutFailure.ErrorDetail;
      OutFailure.Diagnostic.bRetrySafe = false;
    }
  };

  TSet<FString> ArtifactIds;
  TSet<FString> FinalPaths;
  for (int32 Index = 0; Index < Impl->PublicArtifacts.Num(); ++Index) {
    FDurableArtifact &PublicArtifact = Impl->PublicArtifacts[Index];
    PublicArtifact.ArtifactId.TrimStartAndEndInline();
    if (PublicArtifact.ArtifactId.IsEmpty() ||
        PublicArtifact.FinalPath.IsEmpty() ||
        FPaths::IsRelative(PublicArtifact.FinalPath)) {
      SetFailure(OutFailure, TEXT("durable_artifact_identity_invalid"),
                 PublicArtifact.ArtifactId);
      CleanupFailedPrepare();
      return false;
    }
    PublicArtifact.FinalPath =
        NormalizeAbsolutePath(PublicArtifact.FinalPath);
    const FString FoldedId = PublicArtifact.ArtifactId.ToLower();
    const FString FoldedPath = PublicArtifact.FinalPath.ToLower();
    if (ArtifactIds.Contains(FoldedId) || FinalPaths.Contains(FoldedPath)) {
      SetFailure(OutFailure, TEXT("durable_artifact_duplicate"),
                 PublicArtifact.ArtifactId);
      CleanupFailedPrepare();
      return false;
    }
    if (IsWithinRoot(State.TransactionRoot, PublicArtifact.FinalPath)) {
      SetFailure(OutFailure, TEXT("durable_artifact_path_invalid"),
                 PublicArtifact.ArtifactId);
      CleanupFailedPrepare();
      return false;
    }
    ArtifactIds.Add(FoldedId);
    FinalPaths.Add(FoldedPath);

    const FString StableLeaf = FString::Printf(TEXT("%04d"), Index);
    PublicArtifact.StagingPath =
        State.TransactionRoot / TEXT("stage") / (StableLeaf + TEXT(".bin"));
    FDurableArtifactState &Artifact = State.Artifacts.AddDefaulted_GetRef();
    Artifact.ArtifactId = PublicArtifact.ArtifactId;
    Artifact.FinalPath = PublicArtifact.FinalPath;
    Artifact.StagingPath = PublicArtifact.StagingPath;
    Artifact.BackupPath =
        State.TransactionRoot / TEXT("backup") / (StableLeaf + TEXT(".bin"));
    Artifact.QuarantinePath = State.TransactionRoot / TEXT("quarantine") /
                              (StableLeaf + TEXT(".bin"));
    Artifact.bOldExists =
        IFileManager::Get().FileExists(*Artifact.FinalPath);
    if (Artifact.bOldExists) {
      if (!EnsureParentDirectory(Artifact.BackupPath) ||
          IFileManager::Get().Copy(*Artifact.BackupPath, *Artifact.FinalPath,
                                   true, true) != COPY_OK) {
        SetFailure(OutFailure, TEXT("durable_backup_failed"),
                   Artifact.ArtifactId);
        CleanupFailedPrepare();
        return false;
      }
      Artifact.OldHash = FileHash(Artifact.BackupPath);
    }
  }
  State.PhaseHistory.Add(TEXT("inventory_ready"));
  FString Error;
  if (!AppendPhaseAndWrite(State, TEXT("prepared"), Error)) {
    SetFailure(OutFailure, TEXT("durable_journal_prepare_failed"), Error);
    CleanupFailedPrepare();
    return false;
  }
  Impl->bPrepared = true;
  SyncResult(State, InOutResult);
  return true;
}

bool FDurableTransactionCoordinator::Stage(
    ITransactionBackend &Backend, FDurabilityResult &InOutResult,
    FOperationFailure &OutFailure) {
  OutFailure = FOperationFailure();
  if (!Impl->bPrepared) {
    SetFailure(OutFailure, TEXT("durable_stage_without_prepare"));
    return false;
  }
  FString Error;
  if (!AppendPhaseAndWrite(Impl->State, TEXT("staging"), Error)) {
    SetFailure(OutFailure, TEXT("durable_journal_stage_failed"), Error);
    SyncResult(Impl->State, InOutResult);
    return false;
  }
  if (!Backend.StageDurableArtifacts(Impl->PublicArtifacts, OutFailure)) {
    if (OutFailure.ErrorCode.IsEmpty())
      SetFailure(OutFailure, TEXT("durable_stage_backend_failed"));
    OutFailure.Diagnostic.bRetrySafe = false;
    SyncResult(Impl->State, InOutResult);
    return false;
  }
  if (!FinalArtifactsMatchOriginal(Impl->State, Error)) {
    SetFailure(OutFailure, TEXT("durable_stage_modified_final"), Error);
    SyncResult(Impl->State, InOutResult);
    return false;
  }
  for (int32 Index = 0; Index < Impl->PublicArtifacts.Num(); ++Index) {
    const FDurableArtifact &PublicArtifact = Impl->PublicArtifacts[Index];
    FDurableArtifactState &Artifact = Impl->State.Artifacts[Index];
    if (NormalizeAbsolutePath(PublicArtifact.StagingPath) !=
        NormalizeAbsolutePath(Artifact.StagingPath)) {
      SetFailure(OutFailure, TEXT("durable_stage_path_changed"),
                 Artifact.ArtifactId);
      SyncResult(Impl->State, InOutResult);
      return false;
    }
    Artifact.bStagedExists = PublicArtifact.bStagedExists;
    if (Artifact.bStagedExists &&
        !IFileManager::Get().FileExists(*Artifact.StagingPath)) {
      SetFailure(OutFailure, TEXT("durable_stage_artifact_missing"),
                 Artifact.ArtifactId);
      SyncResult(Impl->State, InOutResult);
      return false;
    }
  }
  if (!AppendPhaseAndWrite(Impl->State, TEXT("staged"), Error)) {
    SetFailure(OutFailure, TEXT("durable_journal_staged_failed"), Error);
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
    SetFailure(OutFailure, TEXT("durable_publish_without_stage"));
    return false;
  }
  FString Error;
  if (!AppendPhaseAndWrite(Impl->State, TEXT("publish_started"), Error)) {
    SetFailure(OutFailure, TEXT("durable_journal_publish_failed"), Error);
    SyncResult(Impl->State, InOutResult);
    return false;
  }
  for (int32 Index = 0; Index < Impl->State.Artifacts.Num(); ++Index) {
    FDurableArtifactState &Artifact = Impl->State.Artifacts[Index];
    const FString PreparedPhase =
        FString::Printf(TEXT("publish_%04d_prepared"), Index);
    if (!AppendPhaseAndWrite(Impl->State, PreparedPhase, Error)) {
      SetFailure(OutFailure, TEXT("durable_journal_publish_step_failed"),
                 Error);
      SyncResult(Impl->State, InOutResult);
      return false;
    }
    if (IFileManager::Get().FileExists(*Artifact.FinalPath)) {
      if (!EnsureParentDirectory(Artifact.QuarantinePath) ||
          !IFileManager::Get().Move(*Artifact.QuarantinePath,
                                    *Artifact.FinalPath, true, true, false,
                                    true)) {
        SetFailure(OutFailure, TEXT("durable_quarantine_failed"),
                   Artifact.ArtifactId);
        SyncResult(Impl->State, InOutResult);
        return false;
      }
    }
    if (Artifact.bStagedExists) {
      if (!EnsureParentDirectory(Artifact.FinalPath) ||
          !IFileManager::Get().Move(*Artifact.FinalPath,
                                    *Artifact.StagingPath, true, true, false,
                                    true)) {
        SetFailure(OutFailure, TEXT("durable_publish_artifact_failed"),
                   Artifact.ArtifactId);
        SyncResult(Impl->State, InOutResult);
        return false;
      }
    }
    const FString DonePhase =
        FString::Printf(TEXT("publish_%04d_done"), Index);
    if (!AppendPhaseAndWrite(Impl->State, DonePhase, Error)) {
      SetFailure(OutFailure, TEXT("durable_journal_publish_step_failed"),
                 Error);
      SyncResult(Impl->State, InOutResult);
      return false;
    }
  }
  if (!AppendPhaseAndWrite(Impl->State, TEXT("published"), Error)) {
    SetFailure(OutFailure, TEXT("durable_journal_published_failed"), Error);
    SyncResult(Impl->State, InOutResult);
    return false;
  }
  SyncResult(Impl->State, InOutResult);
  return true;
}

bool FDurableTransactionCoordinator::Restore(
    bool bMemoryRollbackConfirmed, FDurabilityResult &InOutResult,
    FOperationFailure &OutFailure) {
  OutFailure = FOperationFailure();
  if (!Impl->bPrepared)
    return true;
  InOutResult.bRestoreAttempted = true;
  FString JournalError;
  const bool bRestoreJournalStarted = AppendPhaseAndWrite(
      Impl->State, TEXT("restore_started"), JournalError);
  FString RestoreError;
  const bool bArtifactsRestored = RestoreArtifacts(Impl->State, RestoreError);
  const bool bCombinedRestored = bRestoreJournalStarted &&
                                 bArtifactsRestored &&
                                 bMemoryRollbackConfirmed;
  const FString FinalPhase = bCombinedRestored ? TEXT("restored")
                                                : TEXT("restore_unconfirmed");
  FString FinalJournalError;
  const bool bFinalJournalWritten =
      AppendPhaseAndWrite(Impl->State, FinalPhase, FinalJournalError);
  InOutResult.bRestoreSucceeded = bCombinedRestored && bFinalJournalWritten;
  SyncResult(Impl->State, InOutResult);
  if (!bRestoreJournalStarted || !bArtifactsRestored ||
      !bMemoryRollbackConfirmed || !bFinalJournalWritten) {
    FString Detail;
    if (!JournalError.IsEmpty())
      Detail += JournalError;
    if (!RestoreError.IsEmpty()) {
      if (!Detail.IsEmpty())
        Detail += TEXT(";");
      Detail += RestoreError;
    }
    if (!bMemoryRollbackConfirmed) {
      if (!Detail.IsEmpty())
        Detail += TEXT(";");
      Detail += TEXT("memory_rollback_unconfirmed");
    }
    if (!FinalJournalError.IsEmpty()) {
      if (!Detail.IsEmpty())
        Detail += TEXT(";");
      Detail += FinalJournalError;
    }
    SetFailure(OutFailure, TEXT("durable_restore_unconfirmed"), Detail);
    return false;
  }
  if (!IFileManager::Get().DeleteDirectory(*Impl->State.TransactionRoot,
                                           false, true)) {
    SetFailure(OutFailure, TEXT("durable_restore_cleanup_failed"),
               Impl->State.TransactionRoot);
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
    SetFailure(OutFailure, TEXT("durable_commit_without_prepare"));
    return false;
  }
  FString Error;
  if (!AppendPhaseAndWrite(Impl->State, TEXT("committed"), Error) ||
      !WriteJournal(Impl->State, Error, Impl->State.CompletedPath)) {
    SetFailure(OutFailure, TEXT("durable_commit_journal_failed"), Error);
    SyncResult(Impl->State, InOutResult);
    return false;
  }
  Impl->State.JournalPath = Impl->State.CompletedPath;
  SyncResult(Impl->State, InOutResult);
  IFileManager::Get().DeleteDirectory(*Impl->State.TransactionRoot, false,
                                      true);
  Impl->bPrepared = false;
  return true;
}

bool FDurableTransactionCoordinator::IsPrepared() const {
  return Impl->bPrepared;
}

} // namespace UeAgentAuthoringWorkspaceTransaction

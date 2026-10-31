#include "Domains/Fab/UeAgentFabDownload.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"

#include "Algo/AllOf.h"
#include "Async/Async.h"
#include "BuildPatchSettings.h"
#include "BuildPatchVerify.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HttpModule.h"
#include "Interfaces/IBuildInstaller.h"
#include "Interfaces/IBuildManifest.h"
#include "Interfaces/IBuildPatchServicesModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::UAI::Fab
{
namespace
{
using Contract::FDiagnostic;
FDiagnostic Error(const TCHAR* Code, const FString& Reason, const FString& Name = FString())
{
    auto Result = FDiagnostic::Failure(Code, Reason, TEXT("fab_download"));
    Result.CandidateName = Name;
    Result.bRetrySafe = true;
    Result.SuggestedFix = TEXT("Retry fab.download with the same selection to refresh authorization or resume its private cache; no project content has been imported.");
    return Result;
}
FString Text(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key)
{
    FString Value; if (Object) Object->TryGetStringField(Key, Value); return Value;
}
FString NameKey(const FString& Name)
{
    FSHAHash Hash;
    const FTCHARToUTF8 Utf8(*Name);
    FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Hash.Hash);
    return Hash.ToString();
}
bool HashFile(const FString& Filename, FString& Out, TFunctionRef<bool()> IsCancelled)
{
    TUniquePtr<FArchive> Input(IFileManager::Get().CreateFileReader(*Filename));
    if (!Input) return false;
    FSHA1 Hash;
    uint8 Buffer[64 * 1024];
    while (Input->Tell() < Input->TotalSize())
    {
        if (IsCancelled()) return false;
        const int64 Count = FMath::Min<int64>(sizeof(Buffer), Input->TotalSize() - Input->Tell());
        Input->Serialize(Buffer, Count);
        if (Input->IsError()) return false;
        Hash.Update(Buffer, static_cast<uint32>(Count));
    }
    Hash.Final();
    FSHAHash Result; Hash.GetHash(Result.Hash); Out = Result.ToString();
    return Input->Close();
}
bool SaveRecord(const FDownloadRecord& Record)
{
    auto Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("schema"), TEXT("uai.fab.download.v1"));
    Data->SetStringField(TEXT("name"), Record.Name);
    Data->SetStringField(TEXT("asset"), Record.Asset);
    Data->SetStringField(TEXT("publisher"), Record.Publisher);
    Data->SetStringField(TEXT("format"), Record.Format);
    Data->SetStringField(TEXT("filename"), Record.Filename);
    Data->SetStringField(TEXT("listing_id"), Record.ListingId);
    Data->SetStringField(TEXT("file_id"), Record.FileId);
    Data->SetStringField(TEXT("kind"), Record.Kind);
    TArray<TSharedPtr<FJsonValue>> Files;
    for (const auto& File : Record.Files)
    {
        auto Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("path"), File.Relative);
        Item->SetField(TEXT("size"), MakeShared<FJsonValueNumberString>(LexToString(File.Size)));
        Item->SetStringField(TEXT("sha1"), File.Hash);
        Files.Add(MakeShared<FJsonValueObject>(Item));
    }
    Data->SetArrayField(TEXT("files"), Files);
    FString Json;
    FJsonSerializer::Serialize(Data, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json));
    const FString Temp = Record.Directory / TEXT("receipt.pending"), Final = Record.Directory / TEXT("receipt.json");
    return FFileHelper::SaveStringToFile(Json, *Temp, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
        && IFileManager::Get().Move(*Final, *Temp, true, true, false, true);
}
bool ReadRecord(const FString& Directory, FDownloadRecord& Out, FDiagnostic* OutError = nullptr)
{
    FString Json;
    TSharedPtr<FJsonObject> Data;
    const FString Filename = Directory / TEXT("receipt.json");
    if (!IFileManager::Get().FileExists(*Filename)) return false;
    auto Invalid = [&](const FString& Field, const FString& Reason)
    {
        if (OutError)
        {
            *OutError = Error(TEXT("fab_cache_record_invalid"), Reason, Out.Name);
            OutError->PropertyPath = Field;
            OutError->SourceKind = TEXT("native_result");
            OutError->InternalContext = Filename;
        }
        return false;
    };
    if (!FFileHelper::LoadFileToString(Json, *Filename))
        return Invalid(TEXT("receipt"), TEXT("The local completion record cannot be read."));
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Data) || !Data)
        return Invalid(TEXT("receipt"), TEXT("The local completion record is not valid JSON."));
    if (Text(Data, TEXT("schema")) != TEXT("uai.fab.download.v1"))
        return Invalid(TEXT("schema"), TEXT("The local completion record has an unsupported schema."));
    Out.Name = Text(Data, TEXT("name")); Out.Asset = Text(Data, TEXT("asset"));
    Out.Publisher = Text(Data, TEXT("publisher")); Out.Format = Text(Data, TEXT("format"));
    Out.Filename = Text(Data, TEXT("filename")); Out.ListingId = Text(Data, TEXT("listing_id"));
    Out.FileId = Text(Data, TEXT("file_id")); Out.Kind = Text(Data, TEXT("kind")); Out.Directory = Directory;
    const TArray<TSharedPtr<FJsonValue>>* Files = nullptr;
    if (Out.Name.IsEmpty()) return Invalid(TEXT("name"), TEXT("The local completion record has no download name."));
    if (Out.Kind != TEXT("binary") && Out.Kind != TEXT("manifest"))
        return Invalid(TEXT("kind"), TEXT("The local completion record must describe a binary or manifest download."));
    if (!Data->TryGetArrayField(TEXT("files"), Files) || Files->IsEmpty())
        return Invalid(TEXT("files"), TEXT("The local completion record has no verified file inventory."));
    Out.Files.Reset();
    TSet<FString> SeenPaths;
    for (int32 Index = 0; Index < Files->Num(); ++Index)
    {
        const auto& Value = (*Files)[Index];
        const FString Field = FString::Printf(TEXT("files[%d]"), Index);
        if (!Value || Value->Type != EJson::Object) return Invalid(Field, TEXT("A cached file entry must be an object."));
        const auto Item = Value->AsObject();
        FDownloadedFile File;
        File.Relative = Text(Item, TEXT("path")); File.Hash = Text(Item, TEXT("sha1"));
        if (!IsSafeRelativeFile(File.Relative)) return Invalid(Field + TEXT(".path"), TEXT("A cached file path is not a safe relative path: ") + File.Relative);
        const FString PathKey = File.Relative.ToLower();
        if (SeenPaths.Contains(PathKey)) return Invalid(Field + TEXT(".path"), TEXT("The completion record repeats a file path: ") + File.Relative);
        SeenPaths.Add(PathKey);
        const auto Size = Item->TryGetField(TEXT("size"));
        FString SizeLexeme;
        if (!Size || !StateSemantic::CanonicalJson::TryGetNumberLexeme(*Size, SizeLexeme)
            || !StateSemantic::TryParseInt64Number(SizeLexeme, File.Size) || File.Size < 0)
            return Invalid(Field + TEXT(".size"), TEXT("A cached file has no valid non-negative size: ") + File.Relative);
        if (File.Hash.Len() != 40 || !Algo::AllOf(File.Hash, [](TCHAR C) { return FChar::IsHexDigit(C); }))
            return Invalid(Field + TEXT(".sha1"), TEXT("A cached file has no valid integrity digest: ") + File.Relative);
        File.Hash.ToUpperInline();
        Out.Files.Add(MoveTemp(File));
    }
    // All binary consumers read Filename. Bind it to the exact file whose bytes
    // VerifyDownload checks; metadata must never select a second, unchecked ZIP.
    if (Out.Kind == TEXT("binary") && (!IsSafeRelativeFile(Out.Filename) || Out.Filename.Contains(TEXT("/"))
        || Out.Files.Num() != 1 || Out.Files[0].Relative != Out.Filename))
        return Invalid(TEXT("filename"), TEXT("The binary filename must match the single file in the verified download inventory."));
    return true;
}
bool ReserveSource(const FDownloadRecord& Record)
{
    const FString Filename = Record.Directory / TEXT("source.json");
    FString Json;
    TSharedPtr<FJsonObject> Data;
    if (IFileManager::Get().FileExists(*Filename))
        return FFileHelper::LoadFileToString(Json, *Filename)
            && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Data) && Data
            && Text(Data, TEXT("name")) == Record.Name
            && Text(Data, TEXT("listing_id")) == Record.ListingId
            && Text(Data, TEXT("file_id")) == Record.FileId;
    Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("name"), Record.Name);
    Data->SetStringField(TEXT("listing_id"), Record.ListingId);
    Data->SetStringField(TEXT("file_id"), Record.FileId);
    FJsonSerializer::Serialize(Data.ToSharedRef(), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json));
    return IFileManager::Get().MakeDirectory(*Record.Directory, true)
        && FFileHelper::SaveStringToFile(Json, *Filename, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}
}

FString DownloadRoot() { return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("UAI/Fab/Downloads")); }
bool VerifyDownloadCopy(const FString& Source, const FString& Destination, TFunctionRef<bool()> IsCancelled)
{
    FString SourceHash, DestinationHash;
    return IFileManager::Get().FileSize(*Source) == IFileManager::Get().FileSize(*Destination)
        && HashFile(Source, SourceHash, IsCancelled) && HashFile(Destination, DestinationHash, IsCancelled) && SourceHash == DestinationHash;
}
bool IsSafeRelativeFile(const FString& Name)
{
    if (Name.IsEmpty() || !FPaths::IsRelative(Name) || Name.Contains(TEXT(":")) || Name.Contains(TEXT("\\")) || !FPaths::ValidatePath(Name)) return false;
    TArray<FString> Parts; Name.ParseIntoArray(Parts, TEXT("/"), false);
    for (const auto& Part : Parts)
    {
        if (Part.IsEmpty() || Part == TEXT(".") || Part == TEXT("..")) return false;
#if PLATFORM_WINDOWS
        // Win32 strips trailing dots/spaces and recognizes device names even
        // when followed by an extension. Reject aliases before inventory or IO.
        if (Part.EndsWith(TEXT(".")) || Part.EndsWith(TEXT(" "))) return false;
        for (TCHAR Character : Part) if (Character < 32) return false;
        int32 Dot = INDEX_NONE; Part.FindChar(TEXT('.'), Dot);
        const FString Stem = (Dot == INDEX_NONE ? Part : Part.Left(Dot)).TrimEnd();
        if (!FPaths::ValidatePath(Stem)) return false;
        if (Stem.Len() == 4 && (Stem.StartsWith(TEXT("COM"), ESearchCase::IgnoreCase) || Stem.StartsWith(TEXT("LPT"), ESearchCase::IgnoreCase))
            && (Stem[3] == TCHAR(0xB9) || Stem[3] == TCHAR(0xB2) || Stem[3] == TCHAR(0xB3))) return false;
#endif
    }
    return FPaths::IsUnderDirectory(FPaths::ConvertRelativePathToFull(DownloadRoot() / Name), DownloadRoot());
}
bool LoadDownload(const FString& Name, FDownloadRecord& Out, FDiagnostic& Failure)
{
    Out = FDownloadRecord();
    Failure.Reset();
    if (!ReadRecord(DownloadRoot() / NameKey(Name), Out, &Failure) || Out.Name != Name)
    {
        if (!Failure.IsEmpty()) { Failure.CandidateName = Name; return false; }
        Failure = Error(TEXT("fab_download_not_ready"), TEXT("No completed download has this name. Use fab.downloads or finish/recover the original download."), Name);
        Failure.SuggestedFix = TEXT("Select an existing name from fab.downloads, or recover and finish the original download operation before importing.");
        return false;
    }
    return true;
}
bool VerifyDownload(const FDownloadRecord& Record, FDiagnostic& Failure, TFunctionRef<bool()> IsCancelled)
{
    for (const auto& File : Record.Files)
    {
        const FString Filename = Record.Directory / TEXT("payload") / File.Relative;
        FString Hash;
        if (IsCancelled()) { Failure = Error(TEXT("fab_cancelled"), TEXT("Download verification was cancelled."), Record.Name); return false; }
        if (!IsSafeRelativeFile(File.Relative) || IFileManager::Get().FileSize(*Filename) != File.Size || !HashFile(Filename, Hash, IsCancelled) || Hash != File.Hash)
        {
            if (IsCancelled()) { Failure = Error(TEXT("fab_cancelled"), TEXT("Download verification was cancelled."), Record.Name); return false; }
            Failure = Error(TEXT("fab_cache_file_changed"), TEXT("A downloaded cache file is missing or differs from its completed receipt: ") + File.Relative, Record.Name);
            Failure.PropertyPath = File.Relative;
            return false;
        }
    }
    return true;
}
TSharedRef<FJsonObject> ProjectDownload(const FDownloadRecord& Record)
{
    auto Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("download"), Record.Name);
    Out->SetStringField(TEXT("state"), TEXT("ready"));
    Out->SetStringField(TEXT("asset"), Record.Asset);
    Out->SetStringField(TEXT("format"), Record.Format);
    Out->SetStringField(TEXT("file"), Record.Filename);
    Out->SetStringField(TEXT("kind"), Record.Kind);
    int64 Bytes = 0; for (const auto& File : Record.Files) Bytes += File.Size;
    Out->SetField(TEXT("size_bytes"), MakeShared<FJsonValueNumberString>(LexToString(Bytes)));
    Out->SetNumberField(TEXT("file_count"), Record.Files.Num());
    return Out;
}
TSharedRef<FJsonObject> ListDownloads(int64 Page)
{
    TArray<FString> Directories;
    IFileManager::Get().FindFiles(Directories, *(DownloadRoot() / TEXT("*")), false, true);
    Directories.Sort();
    TArray<TSharedPtr<FJsonValue>> Items;
    int64 Count = 0;
    // No multiplication for a page beyond the possible number of records.
    const int64 Begin = Page > 0 && Page <= Directories.Num() / 20 + 1 ? (Page - 1) * 20 : Directories.Num();
    // Directory count is an upper bound even when some receipts are invalid.
    if (Begin < Directories.Num())
    {
        for (const auto& Directory : Directories)
        {
            FDownloadRecord Record;
            if (ReadRecord(DownloadRoot() / Directory, Record))
            {
                if (Count >= Begin && Count < Begin + 20) Items.Add(MakeShared<FJsonValueObject>(ProjectDownload(Record)));
                ++Count;
                // There is no public total: one additional valid receipt proves has_more.
                if (Count > Begin + 20) break;
            }
        }
    }
    auto Out = MakeShared<FJsonObject>();
    Out->SetArrayField(TEXT("items"), Items);
    Out->SetField(TEXT("page"), MakeShared<FJsonValueNumberString>(LexToString(Page)));
    Out->SetBoolField(TEXT("has_more"), Count > Begin + 20);
    if (Count > Begin + 20) Out->SetField(TEXT("next_page"), MakeShared<FJsonValueNumberString>(LexToString(Page + 1)));
    return Out;
}

class FDownload::FState
{
public:
    FDownloadRecord Record;
    TSharedPtr<FJsonObject> Authorization;
    FHttpRequestPtr Http;
#if WITH_DEV_AUTOMATION_TESTS
    FHttpRequestPtr RequestForTesting;
#endif
    TSharedPtr<FArchive> Stream;
    IBuildInstallerPtr Installer;
    TArray<uint8> ManifestData;
    TArray<FString> Clouds;
    TFuture<bool> VerifyFuture;
    FDiagnostic Failure;
    TAtomic<bool> HttpDone{false};
    TAtomic<bool> Cancelled{false};
    bool HttpSuccess = false;
    int32 HttpStatus = 0;
    bool bComplete = false;
    bool bCancelSent = false;
    bool bVerifying = false;
    bool bExisting = false;
    explicit FState(FDownloadRecord InRecord, TSharedPtr<FJsonObject> Info) : Record(MoveTemp(InRecord)), Authorization(MoveTemp(Info)) {}
    void Fail(FDiagnostic InFailure)
    {
        Failure = MoveTemp(InFailure);
        // Called only before starting IO or after its completion signal.
        if (Stream) Stream->Close();
        Stream.Reset(); Http.Reset(); Authorization.Reset(); Clouds.Reset();
        bComplete = true;
    }
    void FinishCancelled()
    {
        Fail(Error(TEXT("fab_cancelled"), TEXT("Download cancelled. Its incomplete private cache is not exposed as a ready resource; retrying this selection can reuse the cache."), Record.Name));
    }
};

FDownload::FDownload(FDownloadRecord Record, TSharedPtr<FJsonObject> Authorization) : State(MakeUnique<FState>(MoveTemp(Record), MoveTemp(Authorization))) {}
FDownload::~FDownload() = default;
TSharedRef<FDownload, ESPMode::ThreadSafe> FDownload::Create(FDownloadRecord Record, TSharedPtr<FJsonObject> Authorization)
{
    return MakeShareable(new FDownload(MoveTemp(Record), MoveTemp(Authorization)));
}
#if WITH_DEV_AUTOMATION_TESTS
TSharedRef<FDownload, ESPMode::ThreadSafe> FDownload::CreateForTesting(FDownloadRecord Record,
    TSharedPtr<FJsonObject> Authorization, TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request)
{
    auto Download = Create(MoveTemp(Record), MoveTemp(Authorization));
    Download->State->RequestForTesting = Request;
    return Download;
}
#endif
void FDownload::Start()
{
    auto& S = *State;
    S.Record.Directory = DownloadRoot() / NameKey(S.Record.Name);
    if (S.Record.Name.IsEmpty() || S.Record.ListingId.IsEmpty() || S.Record.FileId.IsEmpty())
    { S.Fail(Error(TEXT("fab_download_selection_invalid"), TEXT("The selected file lacks its native provenance or download name."))); return; }
    FDownloadRecord Existing;
    if (!S.bExisting && ReadRecord(S.Record.Directory, Existing))
    {
        if (Existing.Name != S.Record.Name || Existing.ListingId != S.Record.ListingId || Existing.FileId != S.Record.FileId)
        { S.Fail(Error(TEXT("fab_download_name_conflict"), TEXT("This download name belongs to a different asset/file. Choose another save_as name."), S.Record.Name)); return; }
        Existing.ExpectedSize = S.Record.ExpectedSize;
        S.Record = MoveTemp(Existing); S.bExisting = true; S.bVerifying = true;
        S.VerifyFuture = Async(EAsyncExecution::ThreadPool, [Self = AsShared()] {
            return VerifyDownload(Self->State->Record, Self->State->Failure, [&Self] { return Self->State->Cancelled.Load(); });
        });
        return;
    }
    if (!ReserveSource(S.Record))
    { S.Fail(Error(TEXT("fab_cache_source_conflict"), TEXT("The private cache has another or unreadable source reservation, or could not be written. Choose a new save_as name."), S.Record.Name)); return; }
    const TArray<TSharedPtr<FJsonValue>>* Infos = nullptr;
    if (!S.Authorization || !S.Authorization->TryGetArrayField(TEXT("downloadInfo"), Infos) || Infos->Num() != 1 || (*Infos)[0]->Type != EJson::Object)
    { S.Fail(Error(TEXT("fab_download_authorization_changed"), TEXT("Fab did not return exactly one supported download source."), S.Record.Name)); return; }
    const auto Info = (*Infos)[0]->AsObject();
    const FString Url = Text(Info, TEXT("downloadUrl"));
    S.Record.Kind = Text(Info, TEXT("type"));
    if (!Url.StartsWith(TEXT("https://")) || (S.Record.Kind != TEXT("binary") && S.Record.Kind != TEXT("manifest")))
    { S.Fail(Error(TEXT("fab_download_source_unsupported"), TEXT("Fab returned an unsupported download source type or transport."), S.Record.Name)); return; }
    const TArray<TSharedPtr<FJsonValue>>* Clouds = nullptr;
    if (Info->TryGetArrayField(TEXT("distributionPointBaseUrls"), Clouds))
        for (const auto& Value : *Clouds) { FString Cloud; if (Value->TryGetString(Cloud) && Cloud.StartsWith(TEXT("https://"))) S.Clouds.Add(Cloud); }
#if WITH_DEV_AUTOMATION_TESTS
    S.Http = MoveTemp(S.RequestForTesting);
#endif
    if (!S.Http) S.Http = FHttpModule::Get().CreateRequest();
    S.Http->SetURL(Url); S.Http->SetVerb(TEXT("GET"));
    S.Http->SetDelegateThreadPolicy(EHttpRequestDelegateThreadPolicy::CompleteOnHttpThread);
    S.Http->SetTimeout(300);
    if (!IFileManager::Get().MakeDirectory(*(S.Record.Directory / TEXT("payload")), true))
    { S.Fail(Error(TEXT("fab_cache_write_failed"), TEXT("Could not create the private Fab download directory."), S.Record.Name)); return; }
    if (S.Record.Kind == TEXT("binary"))
    {
        if (!IsSafeRelativeFile(S.Record.Filename) || S.Record.Filename.Contains(TEXT("/")))
        { S.Fail(Error(TEXT("fab_filename_invalid"), TEXT("Fab returned an unsafe binary filename."), S.Record.Name)); return; }
        S.Stream = MakeShareable(IFileManager::Get().CreateFileWriter(*(S.Record.Directory / TEXT("payload") / S.Record.Filename)));
        if (!S.Stream || !S.Http->SetResponseBodyReceiveStream(S.Stream.ToSharedRef()))
        { S.Fail(Error(TEXT("fab_cache_write_failed"), TEXT("Could not stream the download into its private cache."), S.Record.Name)); return; }
    }
    const TWeakPtr<FDownload, ESPMode::ThreadSafe> Weak = AsShared();
    S.Http->OnProcessRequestComplete().BindLambda([Weak](FHttpRequestPtr, FHttpResponsePtr Response, bool Success)
    {
        if (const auto Self = Weak.Pin())
        {
            auto& State = *Self->State;
            State.HttpStatus = Response ? Response->GetResponseCode() : 0;
            State.HttpSuccess = Success && State.HttpStatus >= 200 && State.HttpStatus < 300;
            if (State.Stream) State.HttpSuccess &= State.Stream->Close() && !State.Stream->IsError();
            else if (State.HttpSuccess && Response) State.ManifestData = Response->GetContent();
            State.HttpDone.Store(true);
        }
    });
    S.Authorization.Reset();
    if (!S.Http->ProcessRequest())
    {
        if (S.Stream) S.Stream->Close();
        S.HttpSuccess = false;
        S.HttpDone.Store(true);
    }
}
bool FDownload::Tick(bool CancelRequested)
{
    auto& S = *State;
    if (S.bComplete) return true;
    if (CancelRequested) S.Cancelled.Store(true);
    if (S.Cancelled.Load() && !S.bCancelSent)
    {
        S.bCancelSent = true;
        if (S.Http && !S.HttpDone.Load()) S.Http->CancelRequest();
        if (S.Installer && !S.Installer->IsComplete()) S.Installer->CancelInstall();
    }
    if (S.bVerifying)
    {
        if (!S.VerifyFuture.IsReady()) return false;
        const bool Valid = S.VerifyFuture.Get();
        if (S.Cancelled.Load()) S.FinishCancelled();
        else if (!Valid && S.bExisting)
        {
            // Reauthorization is still in memory. Repair the same source,
            // retaining native installer staging but retiring its ready receipt.
            if (!IFileManager::Get().Delete(*(S.Record.Directory / TEXT("receipt.json")), false, true))
            { S.Fail(Error(TEXT("fab_cache_write_failed"), TEXT("Could not retire the damaged cache receipt for repair."), S.Record.Name)); return true; }
            S.Failure.Reset(); S.Record.Files.Reset(); S.bVerifying = false; S.bExisting = false;
            Start(); return S.bComplete;
        }
        else if (!Valid) S.bComplete = true;
        else if (!S.bExisting && !SaveRecord(S.Record)) S.Fail(Error(TEXT("fab_cache_receipt_failed"), TEXT("Download completed, but its local completion receipt could not be saved."), S.Record.Name));
        else S.bComplete = true;
        return true;
    }
    if (S.Http && S.HttpDone.Load())
    {
        S.Stream.Reset(); S.Http.Reset();
        if (S.Cancelled.Load()) { S.FinishCancelled(); return true; }
        if (!S.HttpSuccess)
        { S.Fail(Error(TEXT("fab_transfer_failed"), FString::Printf(TEXT("Native download failed (HTTP %d). Retry to obtain fresh authorization."), S.HttpStatus), S.Record.Name)); return true; }
        if (S.Record.Kind == TEXT("manifest"))
        {
            auto& Bps = FModuleManager::LoadModuleChecked<IBuildPatchServicesModule>(TEXT("BuildPatchServices"));
            const auto Manifest = Bps.MakeManifestFromData(S.ManifestData);
            S.ManifestData.Reset();
            if (!Manifest || S.Clouds.IsEmpty())
            { S.Fail(Error(TEXT("fab_manifest_invalid"), TEXT("Fab's content manifest or distribution sources are invalid."), S.Record.Name)); return true; }
            for (const auto& Path : Manifest->GetBuildFileList())
            {
                FSHAHash Hash;
                if (!IsSafeRelativeFile(Path) || !Manifest->GetFileHash(Path, Hash))
                { S.Fail(Error(TEXT("fab_manifest_path_invalid"), TEXT("The manifest contains an unsafe or unverifiable file path: ") + Path, S.Record.Name)); return true; }
                S.Record.Files.Add({Path, Manifest->GetFileSize(Path), Hash.ToString()});
            }
            if (S.Record.Files.IsEmpty()) { S.Fail(Error(TEXT("fab_manifest_empty"), TEXT("The content manifest has no files."), S.Record.Name)); return true; }
            BuildPatchServices::FBuildInstallerConfiguration Config({BuildPatchServices::FInstallerAction::MakeInstall(Manifest.ToSharedRef())});
            Config.InstallDirectory = S.Record.Directory / TEXT("payload");
            Config.StagingDirectory = S.Record.Directory / TEXT("staging");
            Config.CloudDirectories = MoveTemp(S.Clouds);
            Config.InstallMode = BuildPatchServices::EInstallMode::NonDestructiveInstall;
            Config.VerifyMode = BuildPatchServices::EVerifyMode::ShaVerifyAllFiles;
            Config.bRunRequiredPrereqs = false;
            // BPS requires a bound completion delegate. The owned tick below
            // reads completion/cancellation after the native worker has stopped.
            S.Installer = Bps.CreateBuildInstaller(MoveTemp(Config),
                FBuildPatchInstallerDelegate::CreateLambda([](const IBuildInstallerRef&) {}));
            if (!S.Installer || !S.Installer->StartInstallation())
            { S.Installer.Reset(); S.Fail(Error(TEXT("fab_download_start_failed"), TEXT("The native content installer did not start."), S.Record.Name)); return true; }
            return false;
        }
        S.Record.Files.Add({S.Record.Filename, IFileManager::Get().FileSize(*(S.Record.Directory / TEXT("payload") / S.Record.Filename)), FString()});
        if (S.Record.Files[0].Size < 0 || (S.Record.ExpectedSize > 0 && S.Record.Files[0].Size != S.Record.ExpectedSize))
        { S.Fail(Error(TEXT("fab_download_size_mismatch"), FString::Printf(TEXT("Downloaded %lld bytes; Fab declared %lld bytes for %s."), S.Record.Files[0].Size, S.Record.ExpectedSize, *S.Record.Filename), S.Record.Name)); return true; }
        S.bVerifying = true;
        S.VerifyFuture = Async(EAsyncExecution::ThreadPool, [Self = AsShared()]
        {
            auto& State = *Self->State;
            auto& File = State.Record.Files[0];
            if (HashFile(State.Record.Directory / TEXT("payload") / File.Relative, File.Hash, [&State] { return State.Cancelled.Load(); })) return true;
            State.Failure = Error(TEXT("fab_download_readback_failed"), TEXT("The downloaded binary could not be read back."), State.Record.Name);
            return false;
        });
    }
    if (S.Installer && S.Installer->IsComplete())
    {
        if (S.Cancelled.Load() || S.Installer->IsCanceled()) { S.Installer.Reset(); S.FinishCancelled(); return true; }
        if (!S.Installer->CompletedSuccessfully())
        {
            S.Fail(Error(TEXT("fab_content_download_failed"), FString::Printf(TEXT("Native content download failed: %s (%s)."), LexToString(S.Installer->GetErrorType()), *S.Installer->GetErrorCode()), S.Record.Name));
            S.Installer.Reset(); return true;
        }
        S.Installer.Reset();
        for (const auto& File : S.Record.Files)
            if (IFileManager::Get().FileSize(*(S.Record.Directory / TEXT("payload") / File.Relative)) != File.Size)
            { S.Fail(Error(TEXT("fab_download_readback_failed"), TEXT("The installer completed but a declared file is missing or has the wrong size: ") + File.Relative, S.Record.Name)); return true; }
        if (!SaveRecord(S.Record)) S.Fail(Error(TEXT("fab_cache_receipt_failed"), TEXT("The downloaded content receipt could not be saved."), S.Record.Name));
        else S.bComplete = true;
    }
    return S.bComplete;
}
void FDownload::CancelAndDrain()
{
    // Pre-exit no longer runs the normal ticker. This installer has no custom
    // message handler: cancellation and IsComplete run independently of GT
    // message pumping. HTTP callbacks run on the HTTP thread; hashing observes
    // Cancelled. Do not release the job before its owned native work has ended.
    while (!Tick(true))
    {
        FPlatformProcess::SleepNoStats(0.01f);
    }
}
bool FDownload::IsComplete() const { return State->bComplete; }
const FDiagnostic& FDownload::GetError() const { return State->Failure; }
const FDownloadRecord& FDownload::GetRecord() const { return State->Record; }
}

#include "Domains/Fab/UeAgentFabImport.h"

#include "Domains/Fab/UeAgentFabDownload.h"
#include "Domains/Fab/UeAgentFabArchive.h"
#include "AssetImportTask.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Composition/SimpleAuthoring/OperationHost/UeAgentSimpleAuthoringOperationHost.h"
#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Core/AssetDurable/UeAgentPackageSave.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProperties.h"
#include "IAssetTools.h"
#include "InterchangeManager.h"
#include "InterchangeResult.h"
#include "InterchangeResultsContainer.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/EngineVersion.h"
#include "Modules/ModuleManager.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/Archive.h"
#include "Serialization/CustomVersion.h"
#include "UObject/LinkerLoad.h"
#include "UObject/ObjectVersion.h"
#include "UObject/Package.h"
#include "UObject/PackageFileSummary.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectIterator.h"
#include "Utilities/UeAgentNativeOperationFeedback.h"

namespace UE::UAI::Fab
{
namespace
{
using namespace AssetDurable;
using namespace Contract;
using namespace PlanningPreflight;
using namespace StateSemantic;
using namespace CommandEdge;

FString Text(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
    FString Value; if (Object) Object->TryGetStringField(Field, Value); return Value;
}
FString JsonText(const TSharedRef<FJsonObject>& Object)
{
    FString Text; FJsonSerializer::Serialize(Object, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text)); return Text;
}
FDiagnostic Failure(const TCHAR* Code, const FString& Reason, const FString& Path = FString())
{
    auto Error = FDiagnostic::Failure(Code, Reason, TEXT("fab_import"));
    Error.TargetPath = Path;
    Error.SourceKind = TEXT("native_result");
    return Error;
}
FUeAgentCanonicalIdentity Identity(const FString& Package)
{
    return FUeAgentCanonicalIdentity(TEXT("uai.fab.import"), TEXT("asset"), Package,
        TEXT("state"), TEXT("asset"), TEXT("fab-import-v1"));
}
struct FSource
{
    FString File;
    FString Package;
    FString Extension;
    FString Destination;
};
bool Unload(const TArray<FString>& Names, FDiagnostic& Error)
{
    TArray<UPackage*> Packages;
    for (const FString& Name : Names)
        if (UPackage* Package = FindPackage(nullptr, *Name)) { Package->SetDirtyFlag(false); Packages.Add(Package); }
    FText Reason;
    if (!Packages.IsEmpty() && !UPackageTools::UnloadPackages(Packages, Reason, true))
    {
        Error = Failure(TEXT("fab_import_unload_failed"), Reason.IsEmpty() ? TEXT("The imported packages could not be unloaded for readback or rollback.") : Reason.ToString());
        return false;
    }
    CollectGarbage(RF_NoFlags, true);
    for (const auto& Name : Names)
        if (FindPackage(nullptr, *Name)) { Error = Failure(TEXT("fab_import_unload_failed"), TEXT("An imported package is still loaded."), Name); return false; }
    return true;
}
bool NewDestination(const FString& Destination, FDiagnostic& Error)
{
    if (!Destination.StartsWith(TEXT("/Game/")) || !FPackageName::IsValidLongPackageName(Destination))
    { Error = Failure(TEXT("fab_import_destination_invalid"), TEXT("Generic imports require a new folder below /Game."), Destination); return false; }
    const FString Directory = FPackageName::LongPackageNameToFilename(Destination);
    TArray<FString> Files;
    IFileManager::Get().FindFilesRecursive(Files, *Directory, TEXT("*"), true, false);
    if (!Files.IsEmpty())
    { Error = Failure(TEXT("fab_import_destination_conflict"), TEXT("The destination contains existing files. Choose a new folder; Fab import does not overwrite assets."), Destination); return false; }
    for (TObjectIterator<UPackage> It; It; ++It)
        if (It->GetName() == Destination || It->GetName().StartsWith(Destination + TEXT("/")))
        { Error = Failure(TEXT("fab_import_destination_conflict"), TEXT("The destination contains a loaded package."), It->GetName()); return false; }
    return true;
}
bool IsArchive(const FDownloadRecord& Record)
{
    return Record.Kind == TEXT("binary") && Record.Filename.EndsWith(TEXT(".zip"), ESearchCase::IgnoreCase);
}
bool ReadSourceFiles(const FDownloadRecord& Record, TArray<FString>& Files, FDiagnostic& Error)
{
    if (!IsArchive(Record))
    {
        for (const auto& File : Record.Files) Files.Add(File.Relative);
        Files.Sort();
        return true;
    }
    const FString Filename = Record.Directory / TEXT("payload") / Record.Filename;
    FDownloadArchive Archive;
    if (!Archive.Open(Filename, Error)) return false;
    Files = Archive.GetFiles();
    return true;
}
bool ExtractArchive(const FString& Filename, const FString& Directory, TFunctionRef<bool()> Cancel, FDiagnostic& Error)
{
    FDownloadArchive Archive;
    if (!Archive.Open(Filename, Error)) return false;
    const FString FullDirectory = FPaths::ConvertRelativePathToFull(Directory);
    if (!FPaths::IsUnderDirectory(FullDirectory, DownloadRoot())
        || !IFileManager::Get().DeleteDirectory(*FullDirectory, false, true))
    {
        Error = Failure(TEXT("fab_archive_extract_failed"), TEXT("Could not clear the previous private extraction; stale dependencies must not participate in this import."), FPaths::GetCleanFilename(Filename));
        return false;
    }
    return Archive.Extract(FullDirectory, Cancel, Error);
}

class FImport
{
public:
    const FUeAgentCommandRequest& Request;
    const FUeAgentCallerContext& Caller;
    TFunctionRef<bool()> Cancel;
    FDownloadRecord Record;
    FDiagnostic Error;
    TArray<FString> Packages;
    TArray<FSource> Sources;
    TArray<FString> AssetPaths;
    TMap<FString, FString> ExpectedClasses;
    FString Destination;
    FString Root;
    bool bNative = false;
    bool bMemoryChanged = false;
    ContextResource::FUeAgentResourceTracker Resources;
    FPackageAssetDurableBackend Backend;
    FAssetDurableSessionPtr Session;
    FDurableReceipt Receipt;
    int64 Token = -1;
    const FString Owner = TEXT("uai-fab-import-v1");

    FImport(const FUeAgentCommandRequest& InRequest, const FUeAgentCallerContext& InCaller, TFunctionRef<bool()> InCancel)
        : Request(InRequest), Caller(InCaller), Cancel(InCancel) {}

    bool Fail(const TCHAR* Code, const FString& Reason, const FString& Path = FString())
    { Error = Failure(Code, Reason, Path); return false; }

    bool ValidateNativeSourceHeaders()
    {
        // Read the native summary without creating a UObject/linker. UE 5.6
        // detaches a failed summary linker without releasing its file reader;
        // these source errors must be diagnosed before publishing any package.
        for (const auto& Source : Sources)
        {
            if (Source.Extension != TEXT(".uasset") && Source.Extension != TEXT(".umap")) continue;
            if (Cancel()) return Fail(TEXT("fab_cancelled"), TEXT("Import cancelled during native package preflight."), Source.Package);
            const FString PublicFile = Source.Package + Source.Extension;
            TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Source.File));
            if (!Reader) return Fail(TEXT("fab_pack_header_invalid"), TEXT("The verified native package header could not be opened."), PublicFile);
            FPackageFileSummary Summary;
            *Reader << Summary;
            if (Summary.Tag != PACKAGE_FILE_TAG || Reader->IsError())
                return Fail(TEXT("fab_pack_header_invalid"), FString::Printf(TEXT("Native package summary is damaged or truncated (tag 0x%08X; expected 0x%08X; file size %lld bytes)."), uint32(Summary.Tag), uint32(PACKAGE_FILE_TAG), Reader->TotalSize()), PublicFile);
            if (Summary.IsFileVersionTooOld() || Summary.IsFileVersionTooNew() || Summary.GetFileVersionLicenseeUE() > GPackageFileLicenseeUEVersion)
                return Fail(TEXT("fab_pack_version_incompatible"), FString::Printf(TEXT("Native package version is not supported: UE4=%d, UE5=%d, licensee=%d; current UE4=%d, UE5=%d, licensee=%d (oldest loadable UE4=%d)."),
                    Summary.GetFileVersionUE().FileVersionUE4, Summary.GetFileVersionUE().FileVersionUE5, Summary.GetFileVersionLicenseeUE(),
                    GPackageFileUEVersion.FileVersionUE4, GPackageFileUEVersion.FileVersionUE5, GPackageFileLicenseeUEVersion, int32(VER_UE4_OLDEST_LOADABLE_PACKAGE)), PublicFile);
            if (IsEnforcePackageCompatibleVersionCheck() && !FEngineVersion::Current().IsCompatibleWith(Summary.CompatibleWithEngineVersion))
                return Fail(TEXT("fab_pack_version_incompatible"), TEXT("Native package requires engine ") + Summary.CompatibleWithEngineVersion.ToString() + TEXT("; current engine is ") + FEngineVersion::Current().ToString() + TEXT("."), PublicFile);
            const uint32 Flags = Summary.GetPackageFlags();
            if ((!FPlatformProperties::HasEditorOnlyData() && !(Flags & PKG_FilterEditorOnly))
                || (FPlatformProperties::HasEditorOnlyData() && (Flags & PKG_FilterEditorOnly) && !GAllowCookedDataInEditorBuilds))
                return Fail(TEXT("fab_pack_version_incompatible"), TEXT("The package's cooked/editor-only data flags are incompatible with this Editor's loading configuration."), PublicFile);
            if (!FPlatformProperties::RequiresCookedData() && !(Flags & PKG_Cooked))
            {
                uint32 Tag = 0;
                if (Reader->TotalSize() >= sizeof(Tag)) { Reader->Seek(Reader->TotalSize() - sizeof(Tag)); *Reader << Tag; }
                if (Reader->IsError() || Tag != PACKAGE_FILE_TAG)
                    return Fail(TEXT("fab_pack_header_invalid"), FString::Printf(TEXT("Native package end tag is damaged or missing (0x%08X; expected 0x%08X)."), Tag, uint32(PACKAGE_FILE_TAG)), PublicFile);
            }
            if (!Summary.bUnversioned)
                for (const auto& Diff : FCurrentCustomVersions::Compare(Summary.GetCustomVersionContainer().GetAllVersions(), *PublicFile))
                {
                    if (Diff.Type != ECustomVersionDifference::Invalid && Diff.Type != ECustomVersionDifference::Newer) continue;
                    const auto Current = FCurrentCustomVersions::Get(Diff.Version->Key);
                    const FString VersionName = Current.IsSet() ? Current->GetFriendlyName().ToString() : Diff.Version->Key.ToString();
                    return Fail(TEXT("fab_pack_version_incompatible"), FString::Printf(TEXT("Native package custom version %s is %d; this Editor supports %d%s."),
                        *VersionName, Diff.Version->Version, Current.IsSet() ? Current->Version : -1,
                        Diff.Type == ECustomVersionDifference::Invalid ? TEXT(" (the package version is invalid)") : TEXT("")), PublicFile);
                }
        }
        return true;
    }

    bool PrepareSource()
    {
        if (!LoadDownload(Text(Request.GetParams(), TEXT("download")), Record, Error) || !VerifyDownload(Record, Error, Cancel)) return false;
        Root = Record.Directory / TEXT("payload");
        TArray<FString> Files;
        if (!ReadSourceFiles(Record, Files, Error)) return false;
        FString Entry = Text(Request.GetParams(), TEXT("entry"));
        if (IsArchive(Record)) Root = Record.Directory / TEXT("extracted");
        for (const auto& File : Files)
        {
            const FString Ext = FPaths::GetExtension(File, true).ToLower();
            if (Ext == TEXT(".uproject") || Ext == TEXT(".uplugin"))
                return Fail(TEXT("fab_project_install_required"), TEXT("This download is a project or plugin installer, not a content-only asset pack. Install it through Fab/Launcher in the appropriate project or engine."), File);
            if (Ext == TEXT(".uasset") || Ext == TEXT(".umap")) bNative = true;
        }
        Destination = Text(Request.GetParams(), TEXT("destination"));
        if (bNative)
        {
            if (!Entry.IsEmpty())
            {
                Fail(TEXT("fab_native_entry_unsupported"), TEXT("Native UE content packs import as a whole to preserve package references. Omit entry; use fab.files to inspect their contents."), Entry);
                Error.PropertyPath = TEXT("entry"); Error.Source.JsonPath = TEXT("/entry"); Error.SourceKind = TEXT("command_input");
                return false;
            }
            if (!Destination.IsEmpty() && Destination != TEXT("/Game"))
                return Fail(TEXT("fab_native_destination_fixed"), TEXT("UE content packs keep their canonical /Game paths so references remain valid. Omit destination or use /Game; move assets afterwards with asset.move."), Destination);
            Destination = TEXT("/Game");
            for (const auto& File : Files)
            {
                if (!File.StartsWith(TEXT("Content/")))
                    return Fail(TEXT("fab_pack_layout_unsupported"), TEXT("The native pack has a file outside Content. It requires a project/plugin installer rather than asset import."), File);
                FString Extension = FPaths::GetExtension(File, true).ToLower();
                if (File.EndsWith(TEXT(".m.ubulk"), ESearchCase::IgnoreCase)) Extension = TEXT(".m.ubulk");
                if (Extension != TEXT(".uasset") && Extension != TEXT(".umap") && Extension != TEXT(".uexp")
                    && Extension != TEXT(".ubulk") && Extension != TEXT(".uptnl") && Extension != TEXT(".m.ubulk") && Extension != TEXT(".upayload"))
                    return Fail(TEXT("fab_pack_file_unsupported"), TEXT("This native content pack includes a non-package file that asset import cannot publish transactionally."), File);
                const FString Relative = File.Mid(8);
                const FString Package = TEXT("/Game/") + Relative.LeftChop(Extension.Len());
                if (!FPackageName::IsValidLongPackageName(Package)) return Fail(TEXT("fab_pack_path_invalid"), TEXT("The native pack contains an invalid package path."), File);
                const FString Target = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / Relative);
                if (!FPaths::IsUnderDirectory(Target, FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir())))
                    return Fail(TEXT("fab_pack_path_invalid"), TEXT("The native pack destination is outside project Content."), File);
                if (IFileManager::Get().FileExists(*Target) || FindPackage(nullptr, *Package) || FPackageName::DoesPackageExist(Package))
                    return Fail(TEXT("fab_import_destination_conflict"), TEXT("A destination package or file already exists; it will not be overwritten."), Package);
                Sources.Add({Root / File, Package, Extension, Target});
                if (Extension == TEXT(".uasset") || Extension == TEXT(".umap")) Packages.AddUnique(Package);
            }
            for (const auto& Source : Sources)
                if (!Packages.Contains(Source.Package)) return Fail(TEXT("fab_pack_orphan_sidecar"), TEXT("A package sidecar has no matching asset or map header."), Source.Package + Source.Extension);
            return (!IsArchive(Record) || ExtractArchive(Record.Directory / TEXT("payload") / Record.Filename, Root, Cancel, Error)) && ValidateNativeSourceHeaders();
        }
        if (Destination.IsEmpty())
        {
            FString Name = ObjectTools::SanitizeObjectName(Record.Asset);
            if (Name.IsEmpty()) Name = TEXT("ImportedAsset");
            Destination = TEXT("/Game/Fab/") + Name;
        }
        if (!NewDestination(Destination, Error)) return false;
        if (Entry.IsEmpty() && Files.Num() == 1) Entry = Files[0];
        if (Entry.IsEmpty())
        {
            TArray<FString> Primaries;
            for (const auto& File : Files)
            {
                const FString Ext = FPaths::GetExtension(File).ToLower();
                if (Ext == TEXT("glb") || Ext == TEXT("gltf") || Ext == TEXT("fbx") || Ext == TEXT("obj")) Primaries.Add(File);
            }
            if (Primaries.Num() == 1) Entry = Primaries[0];
        }
        if (Entry.IsEmpty() || !Files.Contains(Entry))
        {
            Fail(TEXT("fab_import_entry_required"), TEXT("Choose the file to import from this archive using entry; adjacent dependencies remain available."), Entry);
            TArray<TSharedPtr<FJsonValue>> Choices;
            for (int32 Index = 0; Index < FMath::Min(Files.Num(), 30); ++Index) Choices.Add(MakeShared<FJsonValueString>(Files[Index]));
            Error.CandidateChoices = MakeShared<FJsonValueArray>(Choices);
            Error.PublicMessage += FString::Printf(TEXT(" Archive contains %d files; at most 30 choices are shown."), Files.Num());
            Error.SuggestedFix = TEXT("Use fab.files with this download name, query and next_page to find every entry; copy the chosen entry unchanged into fab.import_asset.");
            Error.CandidateName = Record.Name;
            Error.PropertyPath = TEXT("entry"); Error.Source.JsonPath = TEXT("/entry"); Error.SourceKind = TEXT("command_input");
            return false;
        }
        if (IsArchive(Record) && !ExtractArchive(Record.Directory / TEXT("payload") / Record.Filename, Root, Cancel, Error)) return false;
        Sources.Add({Root / Entry, FString(), FString(), FString()});
        return true;
    }

    FString PublicNativeText(FString Value) const
    {
        const FString PublicRoot = TEXT("download:") + Record.Name;
        Value.ReplaceInline(*Record.Directory, *PublicRoot, ESearchCase::IgnoreCase);
        Value.ReplaceInline(*Record.Directory.Replace(TEXT("/"), TEXT("\\")), *PublicRoot, ESearchCase::IgnoreCase);
        return Value;
    }

    FDiagnostic NativeDetail(const FString& Message, const FString& Target, ESeverity Severity) const
    {
        auto Detail = Failure(TEXT("fab_native_feedback"), PublicNativeText(Message), PublicNativeText(Target));
        Detail.Severity = Severity;
        Detail.InternalContext = Message;
        return Detail;
    }

    void AppendNativeFailure(const NativeFeedback::FNativeOperationFeedback& Feedback)
    {
        for (const auto& Message : Feedback.GetMessages())
        {
            auto Detail = NativeDetail(Message.Text, Error.TargetPath, Message.Verbosity <= ELogVerbosity::Error
                ? ESeverity::Error : ESeverity::Warning);
            if (!Error.Diagnostics.ContainsByPredicate([&](const FDiagnostic& Item) { return Item.PublicMessage == Detail.PublicMessage && Item.Severity == Detail.Severity; }))
                Error.Append(Detail);
        }
    }

    TArray<FDiagnostic> ImportWarnings;

    bool ImportGeneric()
    {
        TStrongObjectPtr<UAssetImportTask> Task(NewObject<UAssetImportTask>());
        Task->Filename = Sources[0].File; Task->DestinationPath = Destination;
        Task->bAutomated = true; Task->bAsync = false; Task->bSave = false;
        Task->bReplaceExisting = false; Task->bReplaceExistingSettings = false;
        TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
        NativeFeedback::FNativeOperationFeedback Feedback;
        FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().ImportAssetTasks({Task.Get()});
        for (TObjectIterator<UPackage> It; It; ++It)
            if (It->GetName().StartsWith(Destination + TEXT("/")) && !It->HasAnyFlags(RF_Transient)) Packages.AddUnique(It->GetName());
        bMemoryChanged = !Packages.IsEmpty();
        for (UObject* Object : Task->GetObjects())
            if (Object && Object->IsAsset() && Packages.Contains(Object->GetOutermost()->GetName()))
                ExpectedClasses.Add(Object->GetPathName(), Object->GetClass()->GetPathName());
        TArray<FDiagnostic> ImportErrors;
        if (Task->AsyncResults.IsValid())
            if (const UInterchangeResultsContainer* Results = Task->AsyncResults->GetResults())
                for (const UInterchangeResult* Result : Results->GetResults())
                {
                    if (!Result || Result->GetResultType() == EInterchangeResultType::Success) continue;
                    const bool IsError = Result->GetResultType() == EInterchangeResultType::Error;
                    auto Detail = NativeDetail(Result->GetText().ToString(), Result->DestinationAssetName.IsEmpty()
                        ? Sources[0].File : Result->DestinationAssetName, IsError ? ESeverity::Error : ESeverity::Warning);
                    if (Detail.PublicMessage.IsEmpty()) Detail.PublicMessage = TEXT("The native importer reported a result without a message.");
                    auto& Diagnostics = IsError ? ImportErrors : ImportWarnings;
                    if (!Diagnostics.ContainsByPredicate([&](const FDiagnostic& Item) { return Item.PublicMessage == Detail.PublicMessage && Item.TargetPath == Detail.TargetPath; }))
                        Diagnostics.Add(MoveTemp(Detail));
                }
        if (!ImportErrors.IsEmpty())
        {
            Fail(TEXT("fab_native_import_failed"), TEXT("UE's native importer reported an error; the new assets will not be published."), PublicNativeText(Sources[0].File));
            for (const auto& Detail : ImportErrors) Error.Append(Detail);
            for (const auto& Detail : ImportWarnings) Error.Append(Detail);
            AppendNativeFailure(Feedback);
            return false;
        }
        if (ExpectedClasses.IsEmpty() || Packages.IsEmpty())
        {
            Fail(TEXT("fab_native_import_failed"), TEXT("UE's native importer produced no assets. Check the selected file format and its required importer plugin."), PublicNativeText(Sources[0].File));
            for (const auto& Detail : ImportWarnings) Error.Append(Detail);
            AppendNativeFailure(Feedback);
            return false;
        }
        return true;
    }

    bool PrepareDurability()
    {
        TArray<FPlannedChange> Changes;
        TArray<FImpact> Impacts;
        TArray<FAssetDurableTarget> Targets;
        Packages.Sort();
        for (const auto& Package : Packages)
        {
            const auto Id = Identity(Package);
            Changes.Emplace(Id, EChangeIntent::Create, FUeAgentCanonicalValuePtr(), FUeAgentCanonicalValuePtr(), TArray<FString>{TEXT("uai:asset:write")}, TEXT("fab-import:direct"));
            Impacts.Emplace(Id, Id, TEXT("direct"), TEXT("fab-import:direct"));
            EPackageHeaderKind Kind = EPackageHeaderKind::Asset;
            TArray<FString> Sidecars;
            if (bNative)
                for (const auto& Source : Sources)
                    if (Source.Package == Package)
                    {
                        if (Source.Extension == TEXT(".umap")) Kind = EPackageHeaderKind::Map;
                        else if (Source.Extension != TEXT(".uasset") && Source.Extension != TEXT(".upayload")) Sidecars.Add(Source.Extension);
                    }
            else Sidecars = {TEXT(".uexp"), TEXT(".ubulk"), TEXT(".uptnl"), TEXT(".m.ubulk")};
            Targets.Emplace(Id, Package, Kind, EAssetDurableIntent::Native, ERollbackGrade::DurableBackup, Sidecars);
        }
        auto Plan = MakeShared<FPlanningPreflightPlan>(
            FUeAgentDescriptorIdentity(TEXT("uai.fab.import/v1"), 1, Json::Sha256Utf8(TEXT("uai.fab.import/v1"))),
            Request.GetRequestId(), TEXT("fab-import:") + Request.GetRequestId(), Caller.GetCallerId(),
            TArray<FString>{TEXT("uai:asset:write")}, TEXT("fab-import:preflight:") + Request.GetRequestId(),
            FUeAgentStateRevision(Owner, TEXT("fab-import-v1"), 1), TEXT("fab-import:native"),
            FChangeSet(MoveTemp(Changes)), FImpactSet(MoveTemp(Impacts)), TArray<FPrecondition>(),
            Json::Sha256Utf8(Request.GetRequestId() + Record.Name + Destination));
        if (!Composition::SimpleAuthoring::OperationHost::TryPrepareDurableJournal(
            FAssetDurableRequest(Plan, MoveTemp(Targets), 0, Owner, EAtomicityMode::CompensatedSaga), Resources, Session, Error)) return false;
        Token = Session->GetRequest().GetFencingToken();
        return true;
    }

    bool Persist()
    {
        return TryPersistNativeAssetDurabilityForVerification(Session, Backend, Token, [&](FDiagnostic& NativeError)
        {
            if (bNative)
            {
                for (const auto& Source : Sources)
                {
                    if (Cancel()) { NativeError = Failure(TEXT("fab_cancelled"), TEXT("Import cancelled while publishing the native content pack.")); return false; }
                    if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Source.Destination), true)
                        || IFileManager::Get().Copy(*Source.Destination, *Source.File, false, false) != COPY_OK)
                    { NativeError = Failure(TEXT("fab_pack_publish_failed"), TEXT("Could not publish the downloaded package file."), Source.Package); return false; }
                    if (!VerifyDownloadCopy(Source.File, Source.Destination, Cancel))
                    { NativeError = Cancel() ? Failure(TEXT("fab_cancelled"), TEXT("Import cancelled during published-file verification."), Source.Package)
                        : Failure(TEXT("fab_pack_publish_content_mismatch"), TEXT("The published package bytes differ from the verified downloaded source."), Source.Package); return false; }
                }
                TArray<FString> Headers;
                for (const auto& Source : Sources)
                    if (Source.Extension == TEXT(".uasset") || Source.Extension == TEXT(".umap")) Headers.Add(Source.Destination);
                RefreshPublishedPackageFiles(Headers);
            }
            else
            {
                for (const auto& Name : Packages)
                {
                    if (Cancel()) { NativeError = Failure(TEXT("fab_cancelled"), TEXT("Import cancelled before package save.")); return false; }
                    UPackage* Package = FindPackage(nullptr, *Name);
                    if (!Package) { NativeError = Failure(TEXT("fab_import_package_missing"), TEXT("The native importer package disappeared before save."), Name); return false; }
                    FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
                    if (!TrySaveLoadedPackage(*Package, nullptr, FPackageName::LongPackageNameToFilename(Name, FPackageName::GetAssetPackageExtension()), Args, NativeError)) return false;
                }
            }
            return true;
        }, Resources, Receipt, Error);
    }

    bool Verify()
    {
        if (!Unload(Packages, Error)) return false;
        IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
        for (const auto& Name : Packages)
        {
            if (Cancel()) return Fail(TEXT("fab_cancelled"), TEXT("Import cancelled during persisted readback."), Name);
            NativeFeedback::FNativeOperationFeedback Feedback;
            UPackage* Package = LoadPackage(nullptr, *Name, LOAD_NoWarn);
            if (!Package)
            {
                Fail(TEXT("fab_import_readback_failed"), TEXT("UE could not reload the persisted package; check engine/plugin compatibility."), Name);
                AppendNativeFailure(Feedback);
                return false;
            }
            bMemoryChanged = true;
            // Match ordinary asset discovery: generated Blueprint classes are
            // package objects, not additional Content Browser assets.
            TArray<FAssetData> PublicAssets;
            Registry.GetAssetsByPackageName(FName(*Name), PublicAssets);
            if (PublicAssets.IsEmpty())
            {
                Fail(TEXT("fab_import_empty_package"), TEXT("The reloaded package contains no registered public asset."), Name);
                AppendNativeFailure(Feedback);
                return false;
            }
            for (const FAssetData& Asset : PublicAssets)
            {
                const FString Path = Asset.GetObjectPathString();
                UObject* Object = FindObject<UObject>(nullptr, *Path);
                if (!Object || !Object->IsAsset())
                {
                    Fail(TEXT("fab_import_readback_asset_missing"), TEXT("The registered asset is missing from its reloaded package."), Path);
                    AppendNativeFailure(Feedback);
                    return false;
                }
                AssetPaths.AddUnique(Path);
            }
        }
        for (const auto& Expected : ExpectedClasses)
        {
            UObject* Object = FindObject<UObject>(nullptr, *Expected.Key);
            if (!Object || Object->GetClass()->GetPathName() != Expected.Value)
                return Fail(TEXT("fab_import_readback_class_mismatch"), TEXT("A native-imported asset did not retain its expected class after reload: ") + Expected.Value, Expected.Key);
        }
        const auto Persisted = MakeShared<const FAssetDurableSession>(Session->GetRequest(), Session->GetInventory(),
            Session->GetJournalId(), Session->GetJournalPath(), Session->GetJournalRoot(), Session->GetTransactionRoot(),
            Session->GetStagingRoot(), Receipt.GetPhaseHistory(), Receipt.GetPreMutationStates(), Receipt.GetExpectedPersistedStates());
        return TryAcknowledgeAssetDurability(Persisted,
            FDurableVerificationAcknowledgement(Receipt.GetJournalId(), Receipt.GetPlanFingerprint(), Receipt.GetInventoryHash(),
                TEXT("fab-import-readback:") + Request.GetRequestId(), Session->GetRequest().GetJournalOwnerId(), Token), Token, Resources, Error);
    }

    FUeAgentCommandResult Run()
    {
        bool Success = PrepareSource();
        if (Success && Cancel()) Success = Fail(TEXT("fab_cancelled"), TEXT("Import cancelled before project mutation."));
        if (Success && !bNative) Success = ImportGeneric();
        if (Success) Success = PrepareDurability();
        if (Success) Success = Persist();
        if (Success) Success = Verify();
        auto Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("download"), Text(Request.GetParams(), TEXT("download")));
        if (!Destination.IsEmpty()) Payload->SetStringField(TEXT("destination"), Destination);
        if (!Success)
        {
            FDiagnostic RollbackError;
            bool MemoryRestored = (!bMemoryChanged && !Session) || Unload(Packages, RollbackError);
            bool Restored = MemoryRestored;
            if (Session)
            {
                FDurableReceipt RollbackReceipt;
                const bool Recorded = TryRollbackAssetDurability(Session.ToSharedRef(), Backend, Token, MemoryRestored, Resources, RollbackReceipt, RollbackError);
                Restored = Recorded && RollbackReceipt.GetStatus() == EDurableStatus::RolledBack;
                if (Recorded)
                {
                    Error.Append(RollbackReceipt.GetFailureDiagnostic());
                    Error.TransactionStatus = LexToString(RollbackReceipt.GetStatus());
                }
                Error.RollbackAttempted = true; Error.RollbackSucceeded = Restored;
            }
            else if (bMemoryChanged) { Error.RollbackAttempted = true; Error.RollbackSucceeded = MemoryRestored; }
            Error.Append(RollbackError);
            Payload->SetStringField(TEXT("state"), Restored ? TEXT("not_imported") : TEXT("recovery_required"));
            if (Session && !Restored)
            {
                Payload->SetStringField(TEXT("journal_id"), Session->GetJournalId());
                Error.SuggestedFix = TEXT("Inspect the returned durable_journals handle and reconcile the same transaction; do not replay import while recovery is unresolved.");
            }
            return FUeAgentCommandResult::Failure(Request, JsonText(Payload), Error);
        }
        AssetPaths.Sort();
        TArray<TSharedPtr<FJsonValue>> Assets;
        for (int32 Index = 0; Index < FMath::Min(AssetPaths.Num(), 20); ++Index) Assets.Add(MakeShared<FJsonValueString>(AssetPaths[Index]));
        Payload->SetStringField(TEXT("state"), TEXT("imported"));
        Payload->SetNumberField(TEXT("asset_count"), AssetPaths.Num());
        Payload->SetNumberField(TEXT("package_count"), Packages.Num());
        Payload->SetArrayField(TEXT("assets"), Assets);
        if (AssetPaths.Num() > Assets.Num()) Payload->SetBoolField(TEXT("assets_truncated"), true);
        return FUeAgentCommandResult::Success(Request, JsonText(Payload), MoveTemp(ImportWarnings));
    }
};
}
CommandEdge::FUeAgentCommandResult ListFiles(const CommandEdge::FUeAgentCommandRequest& Request)
{
    FDownloadRecord Record;
    FDiagnostic Error;
    TArray<FString> Files;
    if (!LoadDownload(Text(Request.GetParams(), TEXT("download")), Record, Error) || !ReadSourceFiles(Record, Files, Error))
        return FUeAgentCommandResult::Failure(Request, TEXT("{}"), Error);
    const FString Query = Text(Request.GetParams(), TEXT("query"));
    if (!Query.IsEmpty()) Files.RemoveAll([&](const FString& File) { return !File.Contains(Query, ESearchCase::IgnoreCase); });
    int64 Page = 1; Request.GetParams()->TryGetNumberField(TEXT("page"), Page);
    if (Page < 1)
    {
        Error = Failure(TEXT("fab_page_invalid"), TEXT("page must be a positive integer."));
        Error.PropertyPath = TEXT("page"); Error.Source.JsonPath = TEXT("/page"); Error.SourceKind = TEXT("command_input");
        return FUeAgentCommandResult::Failure(Request, TEXT("{}"), Error);
    }
    const int32 Begin = Page <= Files.Num() / 20 + 1 ? static_cast<int32>((Page - 1) * 20) : Files.Num();
    TArray<TSharedPtr<FJsonValue>> Items;
    for (int32 Index = Begin; Index < FMath::Min(Begin + 20, Files.Num()); ++Index)
    {
        auto Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("entry"), Files[Index]);
        if (!IsArchive(Record))
            if (const auto* File = Record.Files.FindByPredicate([&](const FDownloadedFile& Value) { return Value.Relative == Files[Index]; }))
                Item->SetField(TEXT("size_bytes"), MakeShared<FJsonValueNumberString>(::LexToString(File->Size)));
        Items.Add(MakeShared<FJsonValueObject>(Item));
    }
    auto Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("download"), Record.Name);
    Payload->SetStringField(TEXT("source_kind"), IsArchive(Record) ? TEXT("archive") : *Record.Kind);
    Payload->SetArrayField(TEXT("items"), Items);
    Payload->SetNumberField(TEXT("total"), Files.Num());
    Payload->SetField(TEXT("page"), MakeShared<FJsonValueNumberString>(::LexToString(Page)));
    Payload->SetBoolField(TEXT("has_more"), Begin + Items.Num() < Files.Num());
    if (Begin + Items.Num() < Files.Num()) Payload->SetField(TEXT("next_page"), MakeShared<FJsonValueNumberString>(::LexToString(Page + 1)));
    return FUeAgentCommandResult::Success(Request, JsonText(Payload));
}
CommandEdge::FUeAgentCommandResult ImportAsset(const CommandEdge::FUeAgentCommandRequest& Request,
    const Contract::FUeAgentCallerContext& Caller, TFunctionRef<bool()> IsCancelled)
{
    return FImport(Request, Caller, IsCancelled).Run();
}
}

#if WITH_DEV_AUTOMATION_TESTS

#include "Domains/Fab/UeAgentFabDownload.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UE::UAI::Fab::Tests
{
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDownloadContractTest, "UeAgentInterface.Fab.Download.Contracts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDownloadContractTest::RunTest(const FString& Parameters)
{
    auto Authorization = [](const FString& Text)
    {
        TSharedPtr<FJsonObject> Result;
        FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Result);
        return Result;
    };
    FDownloadRecord Selection;
    Selection.Name = TEXT("FVT automation ") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    Selection.Asset = TEXT("Fixture"); Selection.Publisher = TEXT("UAI validation"); Selection.Format = TEXT("obj");
    Selection.Filename = TEXT("data.txt"); Selection.ListingId = TEXT("synthetic-listing"); Selection.FileId = TEXT("synthetic-file");
    FTCHARToUTF8 Utf8(*Selection.Name); FSHAHash NameHash;
    FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), NameHash.Hash);
    const FString Directory = DownloadRoot() / NameHash.ToString();
    struct FScopedDirectory
    {
        FString Path;
        ~FScopedDirectory()
        {
            if (FPaths::IsUnderDirectory(FPaths::ConvertRelativePathToFull(Path), FPaths::ConvertRelativePathToFull(DownloadRoot())))
                IFileManager::Get().DeleteDirectory(*Path, false, true);
        }
    } Cleanup{Directory};
    {
        auto Invalid = Selection; Invalid.ListingId.Reset();
        auto Download = FDownload::Create(Invalid, nullptr); Download->Start();
        TestTrue(TEXT("missing provenance rejected before IO"), Download->IsComplete() && Download->GetError().Code == TEXT("fab_download_selection_invalid"));
    }
    struct FCase { const TCHAR* Json; const TCHAR* Expected; };
    for (const FCase& Item : {
        FCase{TEXT("{}"), TEXT("fab_download_authorization_changed")},
        FCase{TEXT("{\"downloadInfo\":[]}"), TEXT("fab_download_authorization_changed")},
        FCase{TEXT("{\"downloadInfo\":[{},{}]}"), TEXT("fab_download_authorization_changed")},
        FCase{TEXT("{\"downloadInfo\":[\"changed\"]}"), TEXT("fab_download_authorization_changed")},
        FCase{TEXT("{\"downloadInfo\":[{\"type\":\"binary\",\"downloadUrl\":\"http://example.invalid/test\"}]}"), TEXT("fab_download_source_unsupported")},
        FCase{TEXT("{\"downloadInfo\":[{\"type\":\"unknown\",\"downloadUrl\":\"https://example.invalid/test\"}]}"), TEXT("fab_download_source_unsupported")}})
    {
        auto Download = FDownload::Create(Selection, Authorization(Item.Json)); Download->Start();
        TestTrue(TEXT("invalid authorization completes without network"), Download->IsComplete());
        TestEqual(TEXT("authorization failure classification"), Download->GetError().Code, FString(Item.Expected));
    }
    {
        auto Unsafe = Selection; Unsafe.Filename = TEXT("../escape.obj");
        auto Download = FDownload::Create(Unsafe, Authorization(TEXT("{\"downloadInfo\":[{\"type\":\"binary\",\"downloadUrl\":\"https://example.invalid/test\"}]}")));
        Download->Start();
        TestTrue(TEXT("binary path rejected without transfer"), Download->IsComplete() && Download->GetError().Code == TEXT("fab_filename_invalid"));
    }
    IFileManager::Get().MakeDirectory(*(Directory / TEXT("payload")), true);
    FFileHelper::SaveStringToFile(TEXT("test"), *(Directory / TEXT("payload/data.txt")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    const FString Receipt = TEXT("{\"schema\":\"uai.fab.download.v1\",\"name\":\"") + Selection.Name
        + TEXT("\",\"asset\":\"Fixture\",\"publisher\":\"UAI validation\",\"format\":\"obj\",\"filename\":\"data.txt\",\"listing_id\":\"synthetic-listing\",\"file_id\":\"synthetic-file\",\"kind\":\"binary\",\"files\":[{\"path\":\"data.txt\",\"size\":4,\"sha1\":\"A94A8FE5CCB19BA61C4C0873D391E987982FBBD3\"}]}");
    FFileHelper::SaveStringToFile(Receipt, *(Directory / TEXT("receipt.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    Contract::FDiagnostic Error;
    FDownloadRecord Cached;
    TestTrue(TEXT("coherent receipt loads"), LoadDownload(Selection.Name, Cached, Error));
    TestTrue(TEXT("integrity verification checks actual payload"), VerifyDownload(Cached, Error));
    TestFalse(TEXT("verification observes cancellation"), VerifyDownload(Cached, Error, [] { return true; }));
    TestEqual(TEXT("verification cancellation remains explicit"), Error.Code, FString(TEXT("fab_cancelled")));
    {
        auto Conflict = Selection; Conflict.FileId = TEXT("different-file");
        auto Download = FDownload::Create(Conflict, nullptr); Download->Start();
        TestTrue(TEXT("same cache name cannot change source"), Download->IsComplete() && Download->GetError().Code == TEXT("fab_download_name_conflict"));
    }
    {
        auto Download = FDownload::Create(Selection, nullptr); Download->Start(); Download->CancelAndDrain();
        TestTrue(TEXT("cache verification cancellation drains owned work"), Download->IsComplete() && Download->GetError().Code == TEXT("fab_cancelled"));
        TestTrue(TEXT("cancellation preserves already completed valid cache"), LoadDownload(Selection.Name, Cached, Error) && VerifyDownload(Cached, Error));
    }
    FFileHelper::SaveStringToFile(TEXT("xxxx"), *(Directory / TEXT("payload/data.txt")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    TestFalse(TEXT("same size different bytes fails digest"), VerifyDownload(Cached, Error));
    TestEqual(TEXT("corruption is specific"), Error.Code, FString(TEXT("fab_cache_file_changed")));
    return !HasAnyErrors();
}
}
#endif

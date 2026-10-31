#if WITH_DEV_AUTOMATION_TESTS
#include "Domains/Fab/UeAgentFabDownload.h"
#include "Domains/Fab/Tests/UeAgentFabTransferFixtures.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

namespace UE::UAI::Fab::Tests
{
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFabTransferContracts, "UeAgentInterface.Fab.Transfer.Contracts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFabTransferContracts::RunTest(const FString& Parameters)
{
    struct FCase { const TCHAR* Name; int32 Code; int32 Bytes; const TCHAR* Error; bool Start=true; bool Transport=true; bool Deferred=false; bool Manifest=false; };
    const FCase Cases[] = {
        {TEXT("verified binary"),200,4,TEXT("")},
        {TEXT("access denied"),403,4,TEXT("fab_transfer_failed")},
        {TEXT("server error"),503,4,TEXT("fab_transfer_failed")},
        {TEXT("transport failure"),0,0,TEXT("fab_transfer_failed"),true,false},
        {TEXT("start failure"),0,0,TEXT("fab_transfer_failed"),false},
        {TEXT("short body"),200,2,TEXT("fab_download_size_mismatch")},
        {TEXT("long body"),200,6,TEXT("fab_download_size_mismatch")},
        {TEXT("cancel owned request"),200,4,TEXT("fab_cancelled"),true,true,true},
        {TEXT("invalid manifest"),200,4,TEXT("fab_manifest_invalid"),true,true,false,true}
    };
    for (const FCase& Case : Cases)
    {
        FDownloadRecord Record;
        Record.Name = TEXT("Fab transfer test ") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
        Record.Asset = TEXT("Fixture"); Record.Publisher = TEXT("UAI tests"); Record.Format = TEXT("fixture");
        Record.ListingId = TEXT("fixture"); Record.FileId = TEXT("fixture-file");
        Record.Filename = TEXT("fixture.bin"); Record.ExpectedSize = 4;
        const auto Source = MakeShared<FJsonObject>();
        Source->SetStringField(TEXT("downloadUrl"), TEXT("https://fab-test.invalid/fixture"));
        Source->SetStringField(TEXT("type"), Case.Manifest ? TEXT("manifest") : TEXT("binary"));
        const auto Authorization = MakeShared<FJsonObject>();
        Authorization->SetArrayField(TEXT("downloadInfo"), {MakeShared<FJsonValueObject>(Source)});
        const auto Request = MakeShared<FTransferRequest>(Case.Code, Case.Bytes);
        Request->bStart = Case.Start; Request->bTransportSuccess = Case.Transport;
        Request->bDeferred = Case.Deferred; Request->bNoResponse = Case.Code == 0;
        const auto Download = FDownload::CreateForTesting(Record, Authorization, Request);
        Download->Start();
        const FString Directory = Download->GetRecord().Directory;
        ON_SCOPE_EXIT
        {
            Download->CancelAndDrain();
            if (FPaths::IsUnderDirectory(Directory, DownloadRoot())) IFileManager::Get().DeleteDirectory(*Directory, false, true);
        };
        if (Case.Deferred) Download->CancelAndDrain();
        const double Deadline = FPlatformTime::Seconds() + 10;
        while (!Download->Tick(false) && FPlatformTime::Seconds() < Deadline) FPlatformProcess::SleepNoStats(0.001f);
        TestTrue(Case.Name, Download->IsComplete());
        TestEqual(FString(Case.Name) + TEXT(" diagnostic"), Download->GetError().Code, FString(Case.Error));
        TestEqual(FString(Case.Name) + TEXT(" completion receipt"), IFileManager::Get().FileExists(*(Directory / TEXT("receipt.json"))), FString(Case.Error).IsEmpty());
        if (Case.Code >= 400) TestTrue(TEXT("HTTP status retained"), Download->GetError().PublicMessage.Contains(FString::FromInt(Case.Code)));
        if (Case.Deferred) TestTrue(TEXT("owned native cancellation reached request"), Request->bCancelled);
        if (FString(Case.Error).IsEmpty())
        {
            FDownloadRecord Loaded; Contract::FDiagnostic Error;
            TestTrue(TEXT("completed binary loads and verifies"), LoadDownload(Record.Name, Loaded, Error) && VerifyDownload(Loaded, Error));
        }
    }
    return true;
}
}
#endif

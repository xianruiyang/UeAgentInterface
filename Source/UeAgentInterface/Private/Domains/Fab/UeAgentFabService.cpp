#include "Domains/Fab/UeAgentFabService.h"
#include "Domains/Fab/UeAgentFabImport.h"
#include "Framework/Docking/TabManager.h"
#include "Widgets/Docking/SDockTab.h"

#include "Domains/Fab/UeAgentFabBrowserBridge.h"
#include "Domains/Fab/UeAgentFabCatalog.h"
#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "Dom/JsonObject.h"
#include "WebBrowserModule.h"
#include "IWebBrowserSingleton.h"
#include "IWebBrowserWindow.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/CoreDelegates.h"
#include "Misc/SecureHash.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/StructOnScope.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Domains/Fab/Tests/UeAgentFabTransferFixtures.h"
#endif

namespace UE::UAI::Fab
{
using namespace CommandEdge;
using Contract::FDiagnostic;

namespace
{
FString Json(const TSharedRef<FJsonObject>& Object)
{
    FString Text;
    FJsonSerializer::Serialize(Object, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));
    return Text;
}

FDiagnostic Failure(const TCHAR* Code, const FString& Reason, const TCHAR* Fix = TEXT("Retry fab.status; if the problem persists, open Fab > My Library in Unreal Editor."))
{
    auto Error = FDiagnostic::Failure(Code, Reason, TEXT("fab"));
    Error.SuggestedFix = Fix;
    Error.bRetrySafe = true;
    return Error;
}

FDiagnostic RemoteFailure(int32 Status, const FString& Code, const FString& Phase, bool bAuthorizingDownload)
{
    const FString SafePhase = Phase == TEXT("session") || Phase == TEXT("csrf") || Phase == TEXT("authenticate")
        ? Phase : TEXT("request");
    auto RemoteError = [&](const TCHAR* ErrorCode, const FString& Reason, const TCHAR* Fix)
    {
        auto Error = FDiagnostic::Failure(ErrorCode, Reason, TEXT("fab.") + SafePhase);
        Error.SuggestedFix = Fix;
        Error.bRetrySafe = true;
        Error.PropertyPath = TEXT("session.http_status");
        Error.ActualValuePresent = true;
        Error.ActualValue = MakeShared<FJsonValueNumber>(Status);
        return Error;
    };
    if (Code == TEXT("browser_challenge"))
        return RemoteError(TEXT("fab_interaction_required"), TEXT("An access challenge is present in UAI's background Fab browser."), TEXT("Keep any existing Fab sign-in. The native Fab tab and UAI background browser are different contexts; signing in again there is not a proven remedy for this challenge. Report this stage to the adapter maintainer."));
    if (Status == 401)
        return RemoteError(TEXT("fab_login_required"), TEXT("Fab rejected the UAI background browser request with HTTP 401; this does not establish that the native Fab tab is signed out."), TEXT("If Fab has just opened, allow its native session restoration to finish. If My Library already works, keep that sign-in and report the failing stage instead of repeatedly signing in."));
    if (Status == 403 && Phase == TEXT("request") && bAuthorizingDownload)
        return RemoteError(TEXT("fab_download_access_required"), TEXT("Fab denied download authorization for this asset/file."), TEXT("Open the asset's Fab page to confirm its license, library entitlement and any required agreement; then retry. UAI does not claim assets or accept agreements automatically."));
    if (Status == 403)
        return RemoteError(TEXT("fab_interaction_required"), TEXT("Fab denied the UAI background browser request with HTTP 403. No browser challenge was observed by this request."), TEXT("Keep the existing native Fab sign-in. Use this failing stage to diagnose the adapter's background request; do not infer logout or require another sign-in from this response."));
    if (Status == 429)
        return RemoteError(TEXT("fab_rate_limited"), TEXT("Fab temporarily limited requests."), TEXT("Wait before retrying the same request; do not start parallel retries."));
    if (Code == TEXT("non_json_response"))
        return RemoteError(TEXT("fab_response_changed"), TEXT("Fab returned a non-JSON response where catalog data was expected."), TEXT("If the native Fab tab works, report this background-browser response stage to the adapter maintainer."));
    return RemoteError(TEXT("fab_request_failed"), FString::Printf(TEXT("Fab request failed (HTTP %d, %s)."), Status, *Code), TEXT("Use the reported request stage and HTTP status to diagnose this failure before retrying."));
}

// Fab 5.6 ships reflected methods, but not exported private C++ classes. Validate
// this adapter at runtime instead of including or modifying the engine plugin.
bool NativePluginAvailable(FDiagnostic& Error)
{
    const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("Fab"));
    if (!Plugin || !Plugin->IsEnabled() || !FModuleManager::Get().IsModuleLoaded(TEXT("Fab")))
    {
        Error = Failure(TEXT("fab_unavailable"), TEXT("The Fab plugin is not enabled and loaded in this Editor."),
            TEXT("Enable the Fab plugin, restart Unreal Editor, then open Fab > My Library and sign in."));
        return false;
    }
    return true;
}

bool NativeAuthentication(FString& Access, FString& Refresh, FDiagnostic& Error)
{
    if (!NativePluginAvailable(Error)) return false;
    UClass* ApiClass = FindObject<UClass>(nullptr, TEXT("/Script/Fab.FabBrowserApi"));
    UClass* SettingsClass = FindObject<UClass>(nullptr, TEXT("/Script/Fab.FabSettings"));
    const FStrProperty* Override = SettingsClass ? FindFProperty<FStrProperty>(SettingsClass, TEXT("CustomAuthToken")) : nullptr;
    if (!ApiClass || !Override)
    {
        Error = Failure(TEXT("fab_adapter_unsupported"), TEXT("This Fab version does not expose the expected native authentication contract."),
            TEXT("Use a supported Fab plugin version; report its version with fab.status."));
        return false;
    }
    // Upstream 5.6 logs the CustomAuthToken value in its getter. Do not invoke that branch.
    if (!Override->GetPropertyValue_InContainer(SettingsClass->GetDefaultObject()).IsEmpty())
    {
        Error = Failure(TEXT("fab_custom_auth_unsupported"), TEXT("Fab has a custom authentication override; its native getter can log the credential."),
            TEXT("Clear Fab's Custom Auth Token override and use normal Fab sign-in, then retry."));
        return false;
    }
    auto Read = [&](const TCHAR* Name, FString& Value)
    {
        UFunction* Function = ApiClass->FindFunctionByName(Name);
        const FStrProperty* Return = Function ? FindFProperty<FStrProperty>(Function, TEXT("ReturnValue")) : nullptr;
        if (!Return || !(Return->PropertyFlags & CPF_ReturnParm)) return false;
        FStructOnScope Parameters(Function);
        ApiClass->GetDefaultObject()->ProcessEvent(Function, Parameters.GetStructMemory());
        Value = Return->GetPropertyValue_InContainer(Parameters.GetStructMemory());
        return true;
    };
    if (!Read(TEXT("GetAuthToken"), Access) || !Read(TEXT("GetRefreshToken"), Refresh))
    {
        Error = Failure(TEXT("fab_adapter_unsupported"), TEXT("Fab's native sign-in methods have an unsupported signature."));
        return false;
    }
    if (Access.IsEmpty())
    {
        Error = FDiagnostic::Failure(TEXT("fab_native_session_unavailable"),
            TEXT("Fab's native session is not available to UAI yet. This does not establish whether the Fab tab is signed in."), TEXT("fab.native_session"));
        Error.SuggestedFix = TEXT("If Fab has just opened, allow its persistent session restoration to finish. If My Library already works, keep that sign-in and report this native-session mismatch; do not repeatedly sign in.");
        Error.bRetrySafe = true;
        return false;
    }
    return true;
}

// Static, same-origin protocol. Credentials are requested through private IPC,
// never embedded in source text, logs, command results or persisted receipts.
const TCHAR* RequestScript = TEXT(R"js(
(async()=>{
 const r=__UAI_FAB_ARGUMENTS__;
 if(window.__uaiFabRequest?.id===r.id)return;
 const state=window.__uaiFabRequest={id:r.id,controller:new AbortController()};
 const send=data=>window.ue.uaifab.receive(JSON.stringify({id:r.id,...data}));
 const csrf=()=>decodeURIComponent(document.cookie.split('; ').find(x=>x.startsWith('fab_csrftoken='))?.slice(14)||'');
 const get=async(path,options={},phase='request')=>{
  const response=await fetch(path,{credentials:'same-origin',signal:state.controller.signal,...options,
   headers:{Accept:'application/json','X-Requested-With':'XMLHttpRequest','X-CsrfToken':csrf(),...options.headers}});
  let data=null;
  if((response.headers.get('content-type')||'').includes('application/json'))data=await response.json();
  if(!response.ok)throw {status:response.status,code:typeof data?.code==='string'?data.code:'',stage:phase};
  if(data===null)throw {status:response.status,code:'non_json_response',stage:phase};
  return data;
 };
 const timeout=setTimeout(()=>state.controller.abort(),45000);
 try{
  if(document.title.includes('Just a moment')||document.querySelector('form#challenge-form'))throw {status:403,code:'browser_challenge',stage:'session'};
  if(r.authenticate){
   await get('/i/csrf',{},'csrf');
   const auth=JSON.parse(await window.ue.uaifab.authentication(r.id));
   if(!auth.access)throw {status:401,code:'native_session_expired',stage:'authenticate'};
   const body=new FormData();body.append('refresh_token',auth.refresh||'');
   await get('/p/authenticate',{method:'POST',headers:{Authorization:auth.access},body},'authenticate');
   auth.access='';auth.refresh='';
  }
  const data=await get(r.path);
  await send({kind:'result',status:200,data});
 }catch(e){await send({kind:'error',status:e.status||0,code:e.name==='AbortError'?'request_aborted':(e.code||'network_error'),stage:e.stage||'request'});}
 finally{clearTimeout(timeout);if(window.__uaiFabRequest===state)window.__uaiFabRequest=null;}
})();
)js");
}

class FService::FState
{
public:
    struct FJob
    {
        FUeAgentCommandRequest Request;
        Contract::FUeAgentCallerContext Caller;
        TFunction<bool()> IsCancelled;
        TFunction<void(FUeAgentCommandResult)> Complete;
        FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
        double Started = 0;
        bool bIssued = false;
        bool bAbortSent = false;
        bool bAuthenticated = false;
        bool bRequiresAuthentication = true;
        TUniquePtr<FCatalogRequest> Catalog;
        TSharedPtr<FDownload, ESPMode::ThreadSafe> Download;
        FJob(FUeAgentCommandRequest InRequest, Contract::FUeAgentCallerContext InCaller, TFunction<bool()> Cancel, TFunction<void(FUeAgentCommandResult)> Done)
            : Request(MoveTemp(InRequest)), Caller(MoveTemp(InCaller)), IsCancelled(MoveTemp(Cancel)), Complete(MoveTemp(Done)) {}
    };
    TArray<TUniquePtr<FJob>> Pending;
    TUniquePtr<FJob> Active;
    TSharedPtr<IWebBrowserWindow> Browser;
    TStrongObjectPtr<UUeAgentFabBrowserBridge> Bridge;
    FTSTicker::FDelegateHandle TickHandle;
    FDelegateHandle ExitHandle;
    FDelegateHandle DocumentHandle;
    bool bDocumentReady = false;
    bool bBridgeReady = false;
    bool bShutdown = false;
    double NextHandshake = 0;
    FString DocumentNonce;
    FDiagnostic AuthenticationError;
    FCatalogCache CatalogCache;
    FString NativeSessionDigest;

    ~FState() { Shutdown(); }
    void CloseBrowser()
    {
        if (Browser)
        {
            Browser->OnDocumentStateChanged().Remove(DocumentHandle);
            Browser->OnBeforeBrowse().Unbind();
            Browser->UnbindUObject(TEXT("uaifab"), Bridge.Get(), true);
            Browser->CloseBrowser(true);
            Browser.Reset();
        }
        if (Bridge.IsValid()) { Bridge->OnReceive = {}; Bridge->OnAuthentication = {}; }
        Bridge.Reset();
        bDocumentReady = false;
        bBridgeReady = false;
    }
    void Shutdown()
    {
        if (bShutdown) return;
        bShutdown = true;
        FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
        FCoreDelegates::OnEnginePreExit.Remove(ExitHandle);
        CloseBrowser();
        const auto Error = Failure(TEXT("fab_editor_exiting"), TEXT("The Editor is closing its Fab session."));
        if (Active && Active->Download) Active->Download->CancelAndDrain();
        FinishError(Error);
        for (auto& Job : Pending) Job->Complete(FUeAgentCommandResult::Failure(Job->Request, TEXT("{}"), Error));
        Pending.Reset();
        Bridge.Reset();
    }
    void FinishError(const FDiagnostic& Error)
    {
        if (!Active) return;
        const FDiagnostic OwnedError = Error;
        auto Job = MoveTemp(Active);
        Job->Download.Reset();
        auto Payload = MakeShared<FJsonObject>();
        if (Job->Request.GetCommandId() == TEXT("fab.status"))
        {
            Payload->SetStringField(TEXT("state"), TEXT("unavailable"));
            const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("Fab"));
            if (Plugin) Payload->SetStringField(TEXT("plugin_version"), Plugin->GetDescriptor().VersionName);
        }
        Job->Complete(FUeAgentCommandResult::Failure(Job->Request, Json(Payload), OwnedError));
    }
    void Finish(const TSharedRef<FJsonObject>& Payload)
    {
        auto Job = MoveTemp(Active);
        Job->Download.Reset();
        Job->Complete(FUeAgentCommandResult::Success(Job->Request, Json(Payload)));
    }
    static bool IsLocalRead(const FUeAgentCommandRequest& Request)
    {
        return Request.GetCommandId() == TEXT("fab.downloads") || Request.GetCommandId() == TEXT("fab.files");
    }
    void CompleteLocalRead(TUniquePtr<FJob> Job)
    {
        if (Job->IsCancelled())
        {
            Job->Complete(FUeAgentCommandResult::Failure(Job->Request, TEXT("{}"),
                Failure(TEXT("fab_cancelled"), TEXT("Fab request cancelled before execution."))));
            return;
        }
        if (Job->Request.GetCommandId() == TEXT("fab.files"))
        {
            Job->Complete(ListFiles(Job->Request));
            return;
        }
        int64 Page = 1; Job->Request.GetParams()->TryGetNumberField(TEXT("page"), Page);
        if (Page < 1)
        {
            auto Error = Failure(TEXT("fab_page_invalid"), TEXT("page must be a positive integer."), TEXT("Pass page 1 or next_page from the previous result."));
            Error.PropertyPath = TEXT("page"); Error.Source.JsonPath = TEXT("/page"); Error.SourceKind = TEXT("command_input");
            Job->Complete(FUeAgentCommandResult::Failure(Job->Request, TEXT("{}"), Error));
            return;
        }
        Job->Complete(FUeAgentCommandResult::Success(Job->Request, Json(ListDownloads(Page))));
    }
    void Advance(FCatalogRequest::EStep Step)
    {
        if (Step == FCatalogRequest::EStep::Failed) { FinishError(Active->Catalog->GetError()); return; }
        if (Step == FCatalogRequest::EStep::DownloadReady)
        {
            Active->Download = FDownload::Create(Active->Catalog->GetDownloadSelection(), Active->Catalog->GetDownloadAuthorization());
            Active->Catalog.Reset();
            Active->Download->Start();
            return;
        }
        if (Step == FCatalogRequest::EStep::Complete)
        {
            const auto Payload = Active->Catalog->GetPayload().ToSharedRef();
            if (Active->Request.GetCommandId() == TEXT("fab.status"))
                if (const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("Fab")))
                    Payload->SetStringField(TEXT("plugin_version"), Plugin->GetDescriptor().VersionName);
            Finish(Payload);
            return;
        }
        // Every HTTP step gets its own nonce; the previous JS promise may still
        // be leaving its finally block when the next step is scheduled.
        Active->Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
        Active->bIssued = false;
        Active->Started = FPlatformTime::Seconds();
    }
    bool EnsureBrowser(FDiagnostic& Error)
    {
        if (Browser && Browser->IsValid() && !Browser->IsClosing()) return true;
        CloseBrowser();
        IWebBrowserModule& Module = FModuleManager::LoadModuleChecked<IWebBrowserModule>(TEXT("WebBrowser"));
        if (!Module.IsWebModuleAvailable())
        {
            Error = Failure(TEXT("fab_browser_unavailable"), TEXT("This Editor has no native WebBrowser backend."));
            return false;
        }
        FWebBrowserInitSettings Init;
        Init.ProductVersion = TEXT("UE_5.6");
        Module.CustomInitialize(Init);
        FCreateBrowserWindowSettings Settings;
        Settings.InitialURL = TEXT("about:blank");
        Settings.BrowserFrameRate = 30;
        Browser = Module.GetSingleton()->CreateBrowserWindow(Settings);
        if (!Browser)
        {
            Error = Failure(TEXT("fab_browser_unavailable"), TEXT("Unreal could not create the background Fab browser."));
            return false;
        }
        Browser->SetViewportSize(FIntPoint(1280, 720));
        Bridge.Reset(NewObject<UUeAgentFabBrowserBridge>());
        Bridge->OnReceive = [this](const FString& Message) { Receive(Message); };
        Bridge->OnAuthentication = [this](const FString& Id)
        {
            if (!Active || Active->Id != Id || !Browser || !Browser->GetUrl().StartsWith(TEXT("https://www.fab.com/"))) return FString();
            FString Access, Refresh;
            if (!NativeAuthentication(Access, Refresh, AuthenticationError)) return FString(TEXT("{}"));
            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("access"), Access);
            Result->SetStringField(TEXT("refresh"), Refresh);
            return Json(Result);
        };
        Browser->OnBeforeBrowse().BindLambda([](const FString& Url, const FWebNavigationRequest&) {
            return !Url.StartsWith(TEXT("https://www.fab.com/")) && Url != TEXT("about:blank");
        });
        DocumentHandle = Browser->OnDocumentStateChanged().AddLambda([this](EWebBrowserDocumentState State)
        {
            bDocumentReady = State == EWebBrowserDocumentState::Completed;
            bBridgeReady = false;
            if (bDocumentReady)
            {
                DocumentNonce = FGuid::NewGuid().ToString(EGuidFormats::Digits);
                NextHandshake = 0;
            }
        });
        Browser->LoadURL(TEXT("https://www.fab.com/plugins/ue5/"));
        return true;
    }
    void Receive(const FString& Text)
    {
        TSharedPtr<FJsonObject> Message;
        if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Message) || !Message) return;
        FString Kind, Id;
        Message->TryGetStringField(TEXT("kind"), Kind);
        Message->TryGetStringField(TEXT("id"), Id);
        if (Kind == TEXT("ready") && Id == DocumentNonce) { bBridgeReady = true; return; }
        if (!Active || Id != Active->Id) return;
        if (Active->IsCancelled())
        {
            FinishError(Failure(TEXT("fab_cancelled"), TEXT("Fab request cancelled; no asset was imported.")));
            return;
        }
        if (Kind == TEXT("error"))
        {
            if (!AuthenticationError.IsEmpty()) { FinishError(AuthenticationError); return; }
            int32 Status = 0;
            Message->TryGetNumberField(TEXT("status"), Status);
            FString Code, Phase;
            Message->TryGetStringField(TEXT("code"), Code);
            Message->TryGetStringField(TEXT("stage"), Phase);
            const FDiagnostic Error = RemoteFailure(Status, Code, Phase, Active->Catalog && Active->Catalog->IsAuthorizingDownload());
            FinishError(Error);
            return;
        }
        if (Kind != TEXT("result")) return;
        const TSharedPtr<FJsonValue>* Data = Message->Values.Find(TEXT("data"));
        if (!Data || !Data->IsValid())
        {
            FinishError(Failure(TEXT("fab_response_changed"), TEXT("Fab returned an unexpected response shape.")));
            return;
        }
        Active->bAuthenticated = Active->bRequiresAuthentication;
        Advance(Active->Catalog->Accept(*Data));
    }
    bool Tick(float)
    {
        if (bShutdown) return false;
        // A queued cancellation must finish independently of the active job,
        // before any catalog cache hit can do work. Local reads check cancellation at dispatch.
        for (int32 Index = Pending.Num() - 1; Index >= 0; --Index)
        {
            if (!Pending[Index]->IsCancelled()) continue;
            auto Job = MoveTemp(Pending[Index]);
            Pending.RemoveAt(Index);
            Job->Complete(FUeAgentCommandResult::Failure(Job->Request, TEXT("{}"),
                Failure(TEXT("fab_cancelled"), TEXT("Fab request cancelled before execution."))));
        }
        if (!Active && !Pending.IsEmpty())
        {
            Active = MoveTemp(Pending[0]);
            Pending.RemoveAt(0);
            if (Active->IsCancelled())
            { FinishError(Failure(TEXT("fab_cancelled"), TEXT("Fab request cancelled before execution."))); return true; }
            Active->Started = FPlatformTime::Seconds();
            AuthenticationError.Reset();
            if (Active->Request.GetCommandId() == TEXT("fab.open"))
            {
                const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("Fab"));
                if (!Plugin || !Plugin->IsEnabled() || !FModuleManager::Get().LoadModule(TEXT("Fab")))
                { FinishError(Failure(TEXT("fab_unavailable"), TEXT("Enable the installed Fab plugin before opening its native tab."))); return true; }
                bool Activate = false; Active->Request.GetParams()->TryGetBoolField(TEXT("activate"), Activate);
                const auto Tab = FGlobalTabmanager::Get()->TryInvokeTab(FTabId(TEXT("FabTab")), !Activate);
                if (!Tab) { FinishError(Failure(TEXT("fab_tab_unavailable"), TEXT("This Fab plugin did not provide its native tab. Open Fab manually from the Editor menu."))); return true; }
                auto Payload = MakeShared<FJsonObject>();
                Payload->SetStringField(TEXT("state"), TEXT("opened"));
                Payload->SetStringField(TEXT("next_action"), TEXT("Wait for native persistent sign-in, or complete login/access checks in Fab, then call fab.status."));
                Finish(Payload); return true;
            }
            if (Active->Request.GetCommandId() == TEXT("fab.import_asset"))
            {
                auto Job = MoveTemp(Active);
                Job->Complete(ImportAsset(Job->Request, Job->Caller, Job->IsCancelled));
                return true;
            }
            FString Access, Refresh;
            FDiagnostic Error;
            const FString Command = Active->Request.GetCommandId();
            FString Scope;
            Active->Request.GetParams()->TryGetStringField(TEXT("scope"), Scope);
            Active->bRequiresAuthentication = Command != TEXT("fab.categories") &&
                !((Command == TEXT("fab.search") || Command == TEXT("fab.asset")) && Scope == TEXT("free"));
            if (Active->bRequiresAuthentication)
            {
                if (!NativeAuthentication(Access, Refresh, Error)) { FinishError(Error); return true; }
                FSHAHash Digest;
                FSHA1::HashBuffer(*Access, Access.Len() * sizeof(TCHAR), Digest.Hash);
                if (NativeSessionDigest != Digest.ToString())
                {
                    CatalogCache = FCatalogCache();
                    NativeSessionDigest = Digest.ToString();
                }
            }
            else if (!NativePluginAvailable(Error)) { FinishError(Error); return true; }
            for (auto It = CatalogCache.Queries.CreateIterator(); It; ++It)
                if (FPlatformTime::Seconds() - It.Value().Created > 60) It.RemoveCurrent();
            Active->Catalog = MakeUnique<FCatalogRequest>(Active->Request.GetCommandId(), Active->Request.GetParams(), CatalogCache);
            Advance(Active->Catalog->Begin());
            if (!Active) return true;
            if (!EnsureBrowser(Error)) { FinishError(Error); return true; }
        }
        if (!Active) return true;
        if (Active->Download)
        {
            if (Active->Download->Tick(Active->IsCancelled()))
            {
                if (!Active->Download->GetError().IsEmpty()) FinishError(Active->Download->GetError());
                else Finish(ProjectDownload(Active->Download->GetRecord()));
            }
            return true;
        }
        if (Active->IsCancelled())
        {
            if (!Active->bIssued) { FinishError(Failure(TEXT("fab_cancelled"), TEXT("Fab request cancelled before execution."))); return true; }
            if (!Active->bAbortSent)
            {
                Active->bAbortSent = true;
                Browser->ExecuteJavascript(TEXT("window.__uaiFabRequest?.controller.abort();"));
            }
        }
        const double Now = FPlatformTime::Seconds();
        if (Now - Active->Started > 90)
        {
            auto Error = Failure(TEXT("fab_session_timeout"), TEXT("The background Fab session did not complete its request."));
            Error.PropertyPath = TEXT("session.phase");
            Error.ActualValue = MakeShared<FJsonValueString>(!bDocumentReady ? TEXT("page_loading") : !bBridgeReady ? TEXT("native_bridge") : TEXT("request"));
            CloseBrowser();
            FinishError(Error);
            return true;
        }
        if (bDocumentReady && !bBridgeReady && Now >= NextHandshake)
        {
            // Bind only after the renderer has a frame, and prove JS visibility.
            // Binding registration or document completion alone is not readiness.
            Browser->BindUObject(TEXT("uaifab"), Bridge.Get(), true);
            auto Message = MakeShared<FJsonObject>();
            Message->SetStringField(TEXT("id"), DocumentNonce);
            Message->SetStringField(TEXT("kind"), TEXT("ready"));
            Browser->ExecuteJavascript(TEXT("if(window.ue?.uaifab?.receive)window.ue.uaifab.receive(JSON.stringify(") + Json(Message) + TEXT("));"));
            NextHandshake = Now + 0.5;
        }
        if (bBridgeReady && !Active->bIssued)
        {
            Active->bIssued = true;
            auto Parameters = MakeShared<FJsonObject>();
            Parameters->SetStringField(TEXT("id"), Active->Id);
            Parameters->SetStringField(TEXT("path"), Active->Catalog->GetPath());
            Parameters->SetBoolField(TEXT("authenticate"), Active->bRequiresAuthentication && !Active->bAuthenticated);
            FString Script(RequestScript);
            Script.ReplaceInline(TEXT("__UAI_FAB_ARGUMENTS__"), *Json(Parameters), ESearchCase::CaseSensitive);
            Browser->ExecuteJavascript(Script);
        }
        return true;
    }
};

FService::FService() : State(MakeUnique<FState>()) {}
FService::~FService() = default;
TSharedRef<FService, ESPMode::ThreadSafe> FService::Create()
{
    TSharedRef<FService, ESPMode::ThreadSafe> Service = MakeShareable(new FService());
    return Service;
}
void FService::Execute(FUeAgentCommandRequest Request, Contract::FUeAgentCallerContext Caller, TFunction<bool()> IsCancelled,
    TFunction<void(FUeAgentCommandResult)> Complete)
{
    AsyncTask(ENamedThreads::GameThread,
        [Self = AsShared(), Request = MoveTemp(Request), Caller = MoveTemp(Caller), IsCancelled = MoveTemp(IsCancelled), Complete = MoveTemp(Complete)]() mutable
        {
            auto& State = *Self->State;
            auto Job = MakeUnique<FState::FJob>(MoveTemp(Request), MoveTemp(Caller), MoveTemp(IsCancelled), MoveTemp(Complete));
            if (State.bShutdown)
            {
                Job->Complete(FUeAgentCommandResult::Failure(Job->Request, TEXT("{}"),
                    Failure(TEXT("fab_editor_exiting"), TEXT("The Editor is closing its Fab session."))));
                return;
            }
            if (!State.TickHandle.IsValid())
            {
                State.TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(&State, &FState::Tick));
                State.ExitHandle = FCoreDelegates::OnEnginePreExit.AddRaw(&State, &FState::Shutdown);
            }
            // Receipt publication/withdrawal and these reads all run on the game thread.
            // Transfer workers write payloads only while no valid receipt is published.
            // Read the published snapshot without waiting for an unrelated active transfer.
            if (FState::IsLocalRead(Job->Request)) State.CompleteLocalRead(MoveTemp(Job));
            else State.Pending.Add(MoveTemp(Job));
        });
}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFabQueuedCancellationTest, "UeAgentInterface.Fab.Queue.Cancellation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFabQueuedCancellationTest::RunTest(const FString& Parameters)
{
    FService::FState State;
    int32 Completed = 0;
    ON_SCOPE_EXIT { State.Shutdown(); };
    auto AddJob = [&](const TCHAR* Command, TFunction<bool()> Cancel, const TCHAR* Expected = TEXT("fab_cancelled"))
    {
        auto Params = MakeShared<FJsonObject>();
        if (FCString::Strcmp(Command, TEXT("fab.asset")) == 0 || FCString::Strcmp(Command, TEXT("fab.download")) == 0)
            Params->SetStringField(TEXT("name"), TEXT("Fixture"));
        if (FCString::Strcmp(Command, TEXT("fab.files")) == 0 || FCString::Strcmp(Command, TEXT("fab.import_asset")) == 0)
            Params->SetStringField(TEXT("download"), TEXT("Fixture"));
        TOptional<FUeAgentCommandRequest> Request; FUeAgentCommandProtocolDiagnostic Diagnostic;
        if (!FUeAgentCommandRequest::TryCreate(TEXT("fab-queue-test"), Command, Command,
            FString::ChrN(64, TEXT('0')), Params, Request, Diagnostic)) return false;
        State.Pending.Add(MakeUnique<FService::FState::FJob>(Request.GetValue(),
            Contract::FUeAgentCallerContext(TEXT("fab-queue-test"), {TEXT("uai:query"), TEXT("uai:operate")}, TEXT("fab-queue-test")),
            MoveTemp(Cancel), [&, Name = FString(Command), ExpectedCode = FString(Expected)](FUeAgentCommandResult Result)
            {
                ++Completed;
                TestEqual(Name + TEXT(" terminal reason"), Result.GetErrorCode(), ExpectedCode);
            }));
        return true;
    };
    for (const TCHAR* Command : {TEXT("fab.status"), TEXT("fab.open"), TEXT("fab.categories"), TEXT("fab.search"), TEXT("fab.asset"),
        TEXT("fab.download"), TEXT("fab.downloads"), TEXT("fab.files"), TEXT("fab.import_asset")})
    {
        if (!TestTrue(TEXT("construct cancelled command"), AddJob(Command, [] { return true; }))) return false;
        if (FService::FState::IsLocalRead(State.Pending.Last()->Request))
        {
            auto Job = MoveTemp(State.Pending.Last());
            State.Pending.RemoveAt(State.Pending.Num() - 1);
            State.CompleteLocalRead(MoveTemp(Job));
        }
    }
    State.Tick(0);
    TestEqual(TEXT("all nine cancellation completions"), Completed, 9);
    TestTrue(TEXT("no native browser or active work created"), !State.Browser && !State.Active && State.Pending.IsEmpty());
    if (!TestTrue(TEXT("construct activation-race command"), AddJob(TEXT("fab.status"), [Calls = 0]() mutable { return ++Calls >= 2; }))) return false;
    State.Tick(0);
    TestEqual(TEXT("activation rechecks cancellation before native work"), Completed, 10);

    FDownloadRecord Record;
    Record.Name = TEXT("Fab shutdown test ") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    Record.ListingId = TEXT("fixture"); Record.FileId = TEXT("fixture-file"); Record.Filename = TEXT("fixture.bin");
    const auto Source = MakeShared<FJsonObject>();
    Source->SetStringField(TEXT("downloadUrl"), TEXT("https://fab-test.invalid/fixture"));
    Source->SetStringField(TEXT("type"), TEXT("binary"));
    const auto Authorization = MakeShared<FJsonObject>();
    Authorization->SetArrayField(TEXT("downloadInfo"), {MakeShared<FJsonValueObject>(Source)});
    const auto Native = MakeShared<Tests::FTransferRequest>(200, 4);
    Native->bDeferred = true;
    const auto Download = FDownload::CreateForTesting(Record, Authorization, Native);
    Download->Start();
    const FString Directory = Download->GetRecord().Directory;
    ON_SCOPE_EXIT
    {
        Download->CancelAndDrain();
        if (FPaths::IsUnderDirectory(Directory, DownloadRoot())) IFileManager::Get().DeleteDirectory(*Directory, false, true);
    };
    if (!TestTrue(TEXT("construct shutdown-owned job"), AddJob(TEXT("fab.download"), [] { return false; }, TEXT("fab_editor_exiting")))) return false;
    State.Active = MoveTemp(State.Pending[0]); State.Pending.RemoveAt(0);
    State.Active->Download = Download;
    State.Shutdown();
    TestTrue(TEXT("service shutdown cancels and drains its actual download owner"), Native->bCancelled && Download->IsComplete()
        && Download->GetError().Code == TEXT("fab_cancelled"));
    TestTrue(TEXT("shutdown does not publish ready or retain jobs"), !IFileManager::Get().FileExists(*(Directory / TEXT("receipt.json"))) && !State.Active && State.Pending.IsEmpty());
    TestEqual(TEXT("one completion after owned shutdown"), Completed, 11);
    State.Shutdown();
    TestEqual(TEXT("repeated shutdown does not repeat completion"), Completed, 11);
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFabRemoteFailureTest, "UeAgentInterface.Fab.RemoteFailure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFabRemoteFailureTest::RunTest(const FString& Parameters)
{
    struct FCase { int32 Status; const TCHAR* Code; const TCHAR* Phase; bool bAuthorization; const TCHAR* Expected; };
    for (const FCase& Item : {
        FCase{401, TEXT("http"), TEXT("request"), false, TEXT("fab_login_required")},
        FCase{403, TEXT("http"), TEXT("request"), true, TEXT("fab_download_access_required")},
        FCase{403, TEXT("http"), TEXT("authentication"), true, TEXT("fab_interaction_required")},
        FCase{403, TEXT("http"), TEXT("request"), false, TEXT("fab_interaction_required")},
        FCase{429, TEXT("http"), TEXT("request"), false, TEXT("fab_rate_limited")},
        FCase{200, TEXT("non_json_response"), TEXT("request"), false, TEXT("fab_response_changed")},
        FCase{200, TEXT("browser_challenge"), TEXT("request"), false, TEXT("fab_interaction_required")},
        FCase{503, TEXT("http"), TEXT("request"), false, TEXT("fab_request_failed")},
        FCase{0, TEXT("network_error"), TEXT("request"), false, TEXT("fab_request_failed")}})
    {
        const auto Error = RemoteFailure(Item.Status, Item.Code, Item.Phase, Item.bAuthorization);
        TestEqual(TEXT("remote failure classification"), Error.Code, FString(Item.Expected));
        TestTrue(TEXT("failure has actionable public reason and recovery advice"), !Error.PublicMessage.IsEmpty() && !Error.SuggestedFix.IsEmpty());
    }
    return !HasAnyErrors();
}
#endif
}

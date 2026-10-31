#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "Domains/Fab/UeAgentFabService.h"
#include "OperationOwners/LongRunning/UeAgentLongRunningLifecycleOwner.h"

namespace UE::UAI::Fab
{
using namespace CommandContract;
using namespace CommandEdge;
using namespace Module::CommandRegistration::Active;
namespace
{
const FString Owner = TEXT("fab-owner");
const TArray<FString> Commands = {TEXT("fab.status"), TEXT("fab.categories"), TEXT("fab.search"), TEXT("fab.asset"),
    TEXT("fab.download"), TEXT("fab.downloads"), TEXT("fab.files"), TEXT("fab.import_asset"), TEXT("fab.open")};

FUeAgentCommandFieldDeclaration Field(const TCHAR* Name, const TCHAR* Type, bool Required, const TCHAR* Description, const TCHAR* Schema = TEXT(""))
{
    return {Name, Type, Required, false, Description, Schema};
}

class FHandler final : public IUeAgentExactCommandHandler
{
public:
    FHandler(TSharedRef<FService, ESPMode::ThreadSafe> InService, FString InCommand) : Service(MoveTemp(InService)), Command(MoveTemp(InCommand)) {}
    const FString& GetCommandId() const override { return Command; }
    const FString& GetPublicOwner() const override { return Owner; }
    FUeAgentCommandResult Submit(const FUeAgentCommandRequest& Request, const Contract::FUeAgentCallerContext& Caller,
        const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation) override
    {
        const bool Mutates = Command == TEXT("fab.download") || Command == TEXT("fab.import_asset") || Command == TEXT("fab.open");
        if (!Caller.GetAuthorityScopes().Contains(Mutates ? TEXT("uai:operate") : TEXT("uai:query")))
            return FUeAgentCommandResult::Failure(Request, TEXT("{}"), Contract::FDiagnostic::Failure(
                TEXT("fab_authority_required"), Mutates ? TEXT("This Fab command requires uai:operate.") : TEXT("This Fab command requires uai:query."), TEXT("fab")));
        TSharedPtr<Contract::IUeAgentOperationApplicationPort> Port;
        TSharedPtr<Composition::LongRunningSession::FUeAgentLongRunningSessionRuntime> Runtime;
        Contract::FDiagnostic Error;
        if (!OperationOwners::LongRunning::TryGetProductionLongRunningLifecyclePort(Port, Runtime, Error))
            return FUeAgentCommandResult::Failure(Request, TEXT("{}"), Error);
        return Runtime->AcceptDeferredCommand(Request, Caller,
            [Service = Service, Request, Caller](TFunction<bool()> IsCancelled, TFunction<void(FUeAgentCommandResult)> Complete) mutable
            { Service->Execute(Request, Caller, MoveTemp(IsCancelled), MoveTemp(Complete)); });
    }
private:
    TSharedRef<FService, ESPMode::ThreadSafe> Service;
    FString Command;
};

class FContribution final : public IUeAgentActiveCommandContribution
{
public:
    const FString& GetContributionId() const override { static const FString Id(TEXT("fab")); return Id; }
    void CollectDeclarations(TArray<FUeAgentCommandDeclaration>& Out) const override
    {
      for (const FString& Command : Commands)
      {
        FUeAgentCommandDeclaration D;
        D.CommandId = Command;
        D.PythonNamespace = TEXT("fab");
        D.PythonFunction = Command.Mid(4);
        D.ContractVersion = 1;
        D.PublicOwner = Owner;
        D.Availability = EUeAgentCommandAvailability::Enabled;
        D.SideEffect = EUeAgentCommandSideEffect::EditorState;
        D.StateLayers = {TEXT("editor_session"), TEXT("server_operation_registry")};
        D.ResultFields = {
            {TEXT("state"), TEXT("string"), true, false, TEXT("Fab service readiness.")},
            {TEXT("session"), TEXT("string"), false, false, TEXT("Native Editor sign-in source.")},
            {TEXT("plugin_version"), TEXT("string"), false, false, TEXT("Installed Fab plugin version.")},
            {TEXT("library_access"), TEXT("boolean"), false, false, TEXT("Verified access to My Library.")}
        };
        D.Purpose.Summary = TEXT("Check Fab plugin, native sign-in and authenticated My Library access without opening a visible window.");
        D.Purpose.Target = TEXT("Current Editor Fab session.");
        D.Purpose.Preconditions = {TEXT("Connect to a running Unreal Editor; this command reports missing Fab or sign-in as a diagnostic.")};
        D.Purpose.ParameterGuidance = TEXT("No parameters.");
        D.Purpose.ResultGuidance = TEXT("Use result.operation.finish_result() for the compact command result; recover the same operation after interruption.");
        D.Purpose.FailureGuidance = TEXT("Follow the diagnostic suggested_fix for login, access checks or unavailable Fab; do not copy cookies or tokens.");
        D.Purpose.SideEffectGuidance = TEXT("Uses a background Fab browser and the existing sign-in; does not acquire or import assets.");
        D.Purpose.MinimumExample = TEXT("client.fab.status()");
        D.Purpose.RelatedFlows = {TEXT("operation.status"), TEXT("query.command.usage")};
        D.Purpose.Keywords = {TEXT("Fab"), TEXT("marketplace"), TEXT("login"), TEXT("library"), TEXT("状态"), TEXT("登录")};
        D.Errors = {{TEXT("fab_login_required"), TEXT("Open Fab and sign in.")}, {TEXT("fab_interaction_required"), TEXT("Complete Fab's interactive access check.")},
            {TEXT("fab_unavailable"), TEXT("Enable the Fab plugin.")}, {TEXT("fab_request_failed"), TEXT("Fab service request failed.")},
            {TEXT("fab_response_changed"), TEXT("The native Fab response contract changed.")},
            {TEXT("fab_session_timeout"), TEXT("The current browser or request step timed out.")}};
        if (Command != TEXT("fab.status"))
        {
            D.Purpose.SideEffectGuidance = TEXT("Reads Fab through the background native session; does not purchase, claim, download or import assets.");
            D.Purpose.RelatedFlows = {TEXT("fab.status"), TEXT("fab.categories"), TEXT("fab.search"), TEXT("fab.asset")};
        }
        if (Command == TEXT("fab.categories"))
        {
            D.InputFields = {
                Field(TEXT("type"), TEXT("string"), false, TEXT("Native product type code returned by this command.")),
                Field(TEXT("parent"), TEXT("string"), false, TEXT("Native category slug whose children to list; copy category unchanged.")),
                Field(TEXT("depth"), TEXT("integer"), false, TEXT("Levels to expand, default 1. Zero explicitly requests the full subtree."), TEXT("{\"type\":\"integer\",\"minimum\":0}"))
            };
            D.ResultFields = {
                Field(TEXT("items"), TEXT("array"), true, TEXT("Native names and reusable type/category filters; has_children marks collapsed subtrees.")),
                Field(TEXT("depth"), TEXT("integer"), true, TEXT("Requested expansion depth.")),
                Field(TEXT("type"), TEXT("string"), false, TEXT("Selected product type.")),
                Field(TEXT("parent"), TEXT("string"), false, TEXT("Selected native parent category."))
            };
            D.Purpose.Summary = TEXT("Browse Fab's current native product types and category tree.");
            D.Purpose.Target = TEXT("Fab's global taxonomy, independent of owned-library counts.");
            D.Purpose.ParameterGuidance = TEXT("Start with no parameters; use returned type and category as type/parent to expand a subtree. Search accepts category unchanged.");
            D.Purpose.MinimumExample = TEXT("client.fab.categories()");
            D.Purpose.Keywords = {TEXT("Fab"), TEXT("categories"), TEXT("taxonomy"), TEXT("分类"), TEXT("类别树")};
        }
        if (Command == TEXT("fab.search") || Command == TEXT("fab.asset") || Command == TEXT("fab.download"))
        {
            D.InputFields.Add(Field(TEXT("scope"), TEXT("string"), false, TEXT("library (default) searches owned resources; free searches Fab's free public catalog without acquiring licenses."), TEXT("{\"type\":\"string\",\"enum\":[\"library\",\"free\"]}")));
            if (Command == TEXT("fab.search"))
            {
                D.InputFields.Append({
                    Field(TEXT("query"), TEXT("string"), false, TEXT("Keywords passed to native Fab search; omit to browse.")),
                    Field(TEXT("type"), TEXT("string"), false, TEXT("Product type code from fab.categories.")),
                    Field(TEXT("category"), TEXT("string"), false, TEXT("Native category slug from fab.categories; its product type is resolved from the same tree.")),
                    Field(TEXT("styles"), TEXT("array"), false, TEXT("Free-catalog Style facet slugs copied from Fab's public search URL, for example stylized and medieval. Rendering style and period/theme share this facet; keywords remain query."), TEXT("{\"type\":\"array\",\"items\":{\"type\":\"string\",\"minLength\":1},\"uniqueItems\":true}")),
                    Field(TEXT("page"), TEXT("integer"), false, TEXT("Native page number, default 1; use next_page without changing the filters."), TEXT("{\"type\":\"integer\",\"minimum\":1}"))
                });
                D.ResultFields = {
                    Field(TEXT("scope"), TEXT("string"), true, TEXT("Actual search scope.")),
                    Field(TEXT("page"), TEXT("integer"), true, TEXT("Returned native page.")),
                    Field(TEXT("items"), TEXT("array"), true, TEXT("Deduplicated readable names/selectors, accepted unchanged by fab.asset.")),
                    Field(TEXT("has_more"), TEXT("boolean"), true, TEXT("Whether Fab has another native page; this page is not the whole catalog.")),
                    Field(TEXT("next_page"), TEXT("integer"), false, TEXT("Next native page for the same filters."))
                };
                D.Purpose.Summary = TEXT("Search My Library by default, or Fab free content, with native type/category filters, free-catalog Style facets and keywords.");
                D.Purpose.Target = TEXT("A native Fab search page with explicit scope and pagination.");
                D.Purpose.ParameterGuidance = TEXT("scope defaults to library. Combine query, type and category; for free search copy Style slugs from Fab's public URL into styles. Follow has_more/next_page with unchanged filters. Copy name/select and, for free details, listing_url into fab.asset.");
                D.Purpose.MinimumExample = TEXT("client.fab.search(scope='free', type='3d-model', query='warrior', styles=['stylized', 'medieval'])");
                D.Purpose.Keywords = {TEXT("Fab"), TEXT("library"), TEXT("free"), TEXT("search"), TEXT("资源库"), TEXT("免费"), TEXT("搜索")};
            }
            else
            {
                D.InputFields.Append({
                    Field(TEXT("name"), TEXT("string"), true, TEXT("Exact original asset name from fab.search.")),
                    Field(TEXT("select"), TEXT("object"), false, TEXT("Readable semantic selector returned by fab.search; do not substitute library or listing IDs."))
                });
                if (Command == TEXT("fab.asset"))
                    D.InputFields.Add(Field(TEXT("listing_url"), TEXT("string"), false, TEXT("Optional public Fab listing URL from free search. Resolves that exact listing without scanning title-search pages; name and select are still checked. Only scope=free accepts this shortcut.")));
                D.ResultFields = {
                    Field(TEXT("name"), TEXT("string"), true, TEXT("Asset title.")),
                    Field(TEXT("scope"), TEXT("string"), true, TEXT("Source scope.")),
                    Field(TEXT("publisher"), TEXT("string"), false, TEXT("Publisher name.")),
                    Field(TEXT("listing_url"), TEXT("string"), false, TEXT("Public listing page for complete descriptions and license terms.")),
                    Field(TEXT("description"), TEXT("string"), false, TEXT("Plain-text description, at most 600 characters.")),
                    Field(TEXT("description_truncated"), TEXT("boolean"), false, TEXT("The public listing contains a longer description.")),
                    Field(TEXT("licenses"), TEXT("array"), false, TEXT("Owned license names when reported by Fab; the listing page holds complete terms.")),
                    Field(TEXT("formats"), TEXT("array"), true, TEXT("Native format codes and named files, size and reported version/platform information. Missing metadata is unknown, not compatibility proof."))
                };
                D.Purpose.Summary = TEXT("Inspect a Fab asset and its actual downloadable format/file choices.");
                D.Purpose.Target = TEXT("One exact native listing, resolved with shared UAI name/selection rules.");
                D.Purpose.ParameterGuidance = TEXT("Copy name and select from search and retain its scope. For fab.asset with scope=free, also copy listing_url for direct, title-checked resolution. Without that URL, resolution checks the complete same-query result set; an ambiguous title is never picked arbitrarily.");
                D.Purpose.MinimumExample = TEXT("client.fab.asset(name='Basic Pickaxe')");
                D.Purpose.Keywords = {TEXT("Fab"), TEXT("asset"), TEXT("detail"), TEXT("formats"), TEXT("详情"), TEXT("格式")};
            }
        }
        if (Command == TEXT("fab.download"))
        {
            D.InputFields.Append({
                Field(TEXT("format"), TEXT("string"), false, TEXT("Native format code from fab.asset; default chooses a unique standalone GLB, native UE pack, or sole available format.")),
                Field(TEXT("file"), TEXT("string"), false, TEXT("Exact native filename from fab.asset. Required if the selected format has several files.")),
                Field(TEXT("file_select"), TEXT("object"), false, TEXT("Readable file selection from fab.asset when native versions share a filename; copy its semantic values unchanged.")),
                Field(TEXT("save_as"), TEXT("string"), false, TEXT("Optional readable local download name; default derives from asset, publisher, format and file."))
            });
            D.ResultFields = {
                Field(TEXT("download"), TEXT("string"), true, TEXT("Readable local cache name accepted by fab.import_asset.")),
                Field(TEXT("state"), TEXT("string"), true, TEXT("ready only after native completion and file verification.")),
                Field(TEXT("asset"), TEXT("string"), true, TEXT("Original Fab title.")),
                Field(TEXT("format"), TEXT("string"), true, TEXT("Selected native format.")),
                Field(TEXT("file"), TEXT("string"), true, TEXT("Selected native filename.")),
                Field(TEXT("kind"), TEXT("string"), true, TEXT("binary or manifest.")),
                Field(TEXT("size_bytes"), TEXT("integer"), true, TEXT("Total completed payload bytes.")),
                Field(TEXT("file_count"), TEXT("integer"), true, TEXT("Completed payload file count."))
            };
            D.Purpose.Summary = TEXT("Download one Fab asset file into the project's private verified cache using native authorization and transfer.");
            D.Purpose.Target = TEXT("One named asset format/file and a local recoverable cache.");
            D.Purpose.ParameterGuidance = TEXT("Reuse name/select/scope from search. Inspect fab.asset for format/file choices; save_as gives the cache a shorter readable name.");
            D.Purpose.SideEffectGuidance = TEXT("Writes private Saved/UAI/Fab cache only. No purchase, license acceptance, claiming or project-content import. Retry the same selection to resume/repair; recover an existing live operation first.");
            D.Purpose.MinimumExample = TEXT("client.fab.download(name='Basic Pickaxe', save_as='Pickaxe')");
            D.Purpose.Keywords = {TEXT("Fab"), TEXT("download"), TEXT("cache"), TEXT("下载")};
            D.Errors.Append({{TEXT("fab_file_selection_required"), TEXT("Choose an exact filename from asset details.")},
                {TEXT("fab_download_access_required"), TEXT("Confirm ownership or license requirements in Fab.")},
                {TEXT("fab_content_download_failed"), TEXT("The native content installer failed; no ready cache was published.")}});
        }
        if (Command == TEXT("fab.downloads"))
        {
            D.InputFields = {Field(TEXT("page"), TEXT("integer"), false, TEXT("Local cache page, default 1; 20 records per page."), TEXT("{\"type\":\"integer\",\"minimum\":1}"))};
            D.ResultFields = {
                Field(TEXT("items"), TEXT("array"), true, TEXT("Local completion records. Import freshly verifies the files before using them.")),
                Field(TEXT("page"), TEXT("integer"), true, TEXT("Returned page.")),
                Field(TEXT("has_more"), TEXT("boolean"), true, TEXT("Whether another cache page exists.")),
                Field(TEXT("next_page"), TEXT("integer"), false, TEXT("Next page."))
            };
            D.Purpose.Summary = TEXT("List completed local Fab downloads without requiring an online Fab session.");
            D.Purpose.Target = TEXT("Project-local completed Fab cache records.");
            D.Purpose.Preconditions = {TEXT("Connect to the Editor; Fab sign-in is not needed for this local query.")};
            D.Purpose.ParameterGuidance = TEXT("Use next_page when has_more is true; copy download into fab.import_asset.");
            D.Purpose.SideEffectGuidance = TEXT("Reads local completion receipts only; does not re-download or import.");
            D.Purpose.MinimumExample = TEXT("client.fab.downloads()");
            D.Purpose.Keywords = {TEXT("Fab"), TEXT("downloads"), TEXT("cache"), TEXT("本地下载")};
            D.Purpose.FailureGuidance = TEXT("Inspect the local cache diagnostic; this command does not require Fab login.");
            D.Errors = {{TEXT("fab_authority_required"), TEXT("This local query requires uai:query authority.")}};
        }
        if (Command == TEXT("fab.files"))
        {
            D.InputFields = {
                Field(TEXT("download"), TEXT("string"), true, TEXT("Completed local download name from fab.downloads.")),
                Field(TEXT("query"), TEXT("string"), false, TEXT("Case-insensitive substring of a relative entry path.")),
                Field(TEXT("page"), TEXT("integer"), false, TEXT("Matching file page, default 1; 20 entries per page."), TEXT("{\"type\":\"integer\",\"minimum\":1}"))};
            D.ResultFields = {
                Field(TEXT("download"), TEXT("string"), true, TEXT("Selected local download.")),
                Field(TEXT("source_kind"), TEXT("string"), true, TEXT("archive, binary or manifest.")),
                Field(TEXT("items"), TEXT("array"), true, TEXT("Relative entry paths; size_bytes where available.")),
                Field(TEXT("total"), TEXT("integer"), true, TEXT("Number of files matching query.")),
                Field(TEXT("page"), TEXT("integer"), true, TEXT("Returned page.")),
                Field(TEXT("has_more"), TEXT("boolean"), true, TEXT("Whether more matching entries remain.")),
                Field(TEXT("next_page"), TEXT("integer"), false, TEXT("Continue with the same download and query."))};
            D.Purpose.Summary = TEXT("Inspect every file of a completed download, including archive entries, without extracting or importing.");
            D.Purpose.Target = TEXT("Local Fab download inventory or ZIP directory.");
            D.Purpose.Preconditions = {TEXT("A completed local download; Fab sign-in is not needed.")};
            D.Purpose.ParameterGuidance = TEXT("Copy download from fab.downloads. Use query to narrow paths and next_page to read all matches.");
            D.Purpose.FailureGuidance = TEXT("Check the download name or damaged archive diagnostic. This offline query does not require Fab login.");
            D.Purpose.SideEffectGuidance = TEXT("Reads local file inventory; import independently verifies cached bytes before mutation.");
            D.Purpose.MinimumExample = TEXT("client.fab.files(download='Pickaxe', query='.fbx')");
            D.Purpose.RelatedFlows = {TEXT("fab.downloads"), TEXT("fab.import_asset")};
            D.Purpose.Keywords = {TEXT("Fab"), TEXT("files"), TEXT("archive"), TEXT("entry"), TEXT("压缩包")};
            D.Errors = {{TEXT("fab_download_not_ready"), TEXT("Choose an existing completed download from fab.downloads.")},
                {TEXT("fab_archive_unsupported"), TEXT("The ZIP reader cannot read the downloaded archive.")},
                {TEXT("fab_archive_unreadable"), TEXT("The downloaded archive cannot be opened.")},
                {TEXT("fab_archive_path_invalid"), TEXT("The archive contains an unsafe entry path.")}};
        }
        if (Command == TEXT("fab.import_asset"))
        {
            D.SideEffect = EUeAgentCommandSideEffect::AssetState;
            D.StateLayers.Add(TEXT("asset_persisted_state"));
            D.InputFields = {
                Field(TEXT("download"), TEXT("string"), true, TEXT("Readable completed local name from fab.download/fab.downloads.")),
                Field(TEXT("destination"), TEXT("string"), false, TEXT("Generic files: a new empty /Game folder, default /Game/Fab/<title>. Native UE content packs: omit or /Game to preserve references.")),
                Field(TEXT("entry"), TEXT("string"), false, TEXT("Generic files only: exact primary entry from fab.files; dependencies remain adjacent. Omit for native UE packs, which import as a whole."))
            };
            D.ResultFields = {
                Field(TEXT("state"), TEXT("string"), true, TEXT("imported only after saving and independent reload; otherwise not_imported or recovery_required.")),
                Field(TEXT("download"), TEXT("string"), true, TEXT("Selected local download.")),
                Field(TEXT("destination"), TEXT("string"), true, TEXT("Actual destination policy.")),
                Field(TEXT("asset_count"), TEXT("integer"), false, TEXT("Actual reloaded asset count.")),
                Field(TEXT("package_count"), TEXT("integer"), false, TEXT("Persisted package count.")),
                Field(TEXT("assets"), TEXT("array"), false, TEXT("Up to 20 usable imported asset paths.")),
                Field(TEXT("assets_truncated"), TEXT("boolean"), false, TEXT("Use normal asset queries to inspect the remaining imported assets."))
            };
            D.Purpose.Summary = TEXT("Import a verified local Fab download through native UE content publication or AssetTools, with shared durability and saved readback.");
            D.Purpose.Target = TEXT("Downloaded content packages, supported generic assets, or a selected archive entry.");
            D.Purpose.Preconditions = {TEXT("A completed local Fab download; uai:operate authority. Fab sign-in is not needed."), TEXT("Required native importer/plugins must be installed; existing project assets are not overwritten.")};
            D.Purpose.ParameterGuidance = TEXT("Copy download from the completion record. Use a new destination folder for generic files; native UE packs preserve canonical /Game paths. entry resolves archive ambiguity.");
            D.Purpose.FailureGuidance = TEXT("Use fab.files for all entry choices. Follow native importer diagnostics for format/plugin failures. Inspect recovery_required before retrying; Fab sign-in is not needed.");
            D.Purpose.SideEffectGuidance = TEXT("Creates and saves project assets, then reloads them for verification. Uses shared backup/journal/rollback. Project/plugin installation is explicitly unsupported by this asset command.");
            D.Purpose.MinimumExample = TEXT("client.fab.import_asset(download='Pickaxe', destination='/Game/Fab/Pickaxe')");
            D.Purpose.Keywords = {TEXT("Fab"), TEXT("import"), TEXT("assets"), TEXT("导入")};
            D.Errors = {{TEXT("fab_import_destination_conflict"), TEXT("The target already exists and will not be overwritten.")},
                {TEXT("fab_import_entry_required"), TEXT("Choose an archive entry using fab.files.")},
                {TEXT("fab_native_entry_unsupported"), TEXT("Native UE packs import together; omit entry.")},
                {TEXT("fab_native_import_failed"), TEXT("Inspect the attached native importer reasons.")},
                {TEXT("fab_project_install_required"), TEXT("This package requires project/plugin installation, not asset import.")}};
        }
        if (Command == TEXT("fab.open"))
        {
            D.InputFields = {Field(TEXT("activate"), TEXT("boolean"), false, TEXT("Explicitly request tab activation. Default false requests an inactive native tab."))};
            D.ResultFields = {Field(TEXT("state"), TEXT("string"), true, TEXT("opened confirms the native tab exists, not that authentication completed.")),
                Field(TEXT("next_action"), TEXT("string"), true, TEXT("How to finish native login and confirm readiness."))};
            D.Purpose.Summary = TEXT("Explicitly open the native Fab tab for persistent sign-in, login or an interactive access challenge.");
            D.Purpose.Target = TEXT("The installed Fab plugin's original Editor tab.");
            D.Purpose.Preconditions = {TEXT("uai:operate authority and an enabled Fab plugin.")};
            D.Purpose.ParameterGuidance = TEXT("activate defaults to false. Use true when the user needs to interact with Fab immediately.");
            D.Purpose.ResultGuidance = TEXT("opened is a UI result. Wait for native persistent login or user interaction, then call fab.status to verify library access.");
            D.Purpose.SideEffectGuidance = TEXT("Explicitly creates/shows the native Fab tab and lets Fab perform its own persistent login. Activation may bring the Editor forward. Status/search never invoke this command implicitly.");
            D.Purpose.MinimumExample = TEXT("client.fab.open()");
            D.Purpose.Keywords = {TEXT("Fab"), TEXT("open"), TEXT("login"), TEXT("登录"), TEXT("窗口")};
        }
        Out.Add(MoveTemp(D));
      }
    }
    bool CreateHandlers(TSharedRef<const CommandRegistry::FUeAgentImmutableCommandRegistry>,
        TArray<TSharedRef<IUeAgentExactCommandHandler>>& Out, Contract::FDiagnostic&) const override
    {
        const auto Service = FService::Create();
        for (const FString& Command : Commands) Out.Add(MakeShared<FHandler>(Service, Command));
        return true;
    }
};
bool CreateContribution(TSharedPtr<IUeAgentActiveCommandContribution>& Out, Contract::FDiagnostic&)
{
    Out = MakeShared<FContribution>();
    return true;
}
const FUeAgentActiveCommandContributionFactoryRegistration Registration(TEXT("fab"), &CreateContribution);
}
}

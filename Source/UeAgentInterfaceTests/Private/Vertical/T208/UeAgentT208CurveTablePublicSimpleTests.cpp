#include "Misc/AutomationTest.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "CommandEdge/UeAgentCommandProtocol.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceIO.h"
#include "Dom/JsonObject.h"
#include "Engine/CurveTable.h"
#include "Curves/SimpleCurve.h"
#include "DomainAdapters/DataTableCurveTable/UeAgentCurveTableAuthoringState.h"
#include "DomainAdapters/DataTableCurveTable/UeAgentCurveTableAuthoringApply.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"
#include "UObject/StrongObjectPtr.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "ObjectTools.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
namespace
{
	using namespace UE::UAI;

	class FNeverCancelled final
		: public Contract::IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override { return false; }
	};

	CommandEdge::FUeAgentCommandResult SubmitPublicCommand(
		const TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch>& Dispatch,
		const FString& CommandId,
		const FString& RequestId,
		const TSharedRef<FJsonObject>& Params)
	{
		const auto* Command = Registry.IsValid()
			? Registry->FindExact(CommandId)
			: nullptr;
		FString ParamsJson;
		FString JsonError;
		if (!Command || !Dispatch.IsValid()
			|| !CommandContract::Json::TryCanonicalize(
				Params, ParamsJson, JsonError))
		{
			return CommandEdge::FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t208-curvetable"),
				RequestId,
				CommandId,
				TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"),
				TEXT("curvetable_public_command_unavailable"),
				TEXT("the public command or request is unavailable"));
		}
		const FString Envelope = FString::Printf(
			TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,")
			TEXT("\"message_kind\":\"command_request\",")
			TEXT("\"correlation_id\":\"corr-t208-curvetable\",")
			TEXT("\"request_id\":\"%s\",\"command_id\":\"%s\",")
			TEXT("\"command_binding_hash\":\"%s\",\"params\":%s}"),
			*RequestId,
			*CommandId,
			*Command->GetContractHash(),
			*ParamsJson);
		FTCHARToUTF8 Encoded(*Envelope);
		TArray<uint8> Bytes;
		Bytes.Append(
			reinterpret_cast<const uint8*>(Encoded.Get()),
			Encoded.Length());
		CommandEdge::FUeAgentCommandProtocolDiagnostic Diagnostic;
		TOptional<CommandEdge::FUeAgentCommandRequest> Request;
		if (!CommandEdge::FUeAgentCommandProtocolCodec().DecodeRequest(
				Bytes, Request, Diagnostic)
			|| !Request.IsSet())
		{
			return CommandEdge::FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t208-curvetable"),
				RequestId,
				CommandId,
				Command->GetContractHash(),
				TEXT("curvetable_public_request_invalid"),
				TEXT("the public request could not be decoded"));
		}
		return Dispatch->Submit(
			Request.GetValue(),
			Contract::FUeAgentCallerContext(
				TEXT("t208-curvetable"),
				{TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")},
				TEXT("corr-t208-curvetable")),
			MakeShared<FNeverCancelled>());
	}

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208CurveTablePublicSimple,
    "UAI.Root.T208.CurveTable.PublicSimple", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208CurveTablePublicSimple::RunTest(const FString& Parameters)
{
    using namespace UE::UAI;
    const FString Name = TEXT("CT_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString PackageName = TEXT("/Game/UAI/Transient/T208CurveTable/") + Name;
    const FString Path = PackageName + TEXT(".") + Name;
    const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
    const FString Workspace = FPaths::ProjectSavedDir() / TEXT("UeAssetFolders/T208CurveTable") / Name;
    ON_SCOPE_EXIT
    {
        if (UObject* Owned = LoadObject<UCurveTable>(nullptr, *Path))
        {
            Owned->GetOutermost()->SetDirtyFlag(false);
            if (ObjectTools::ForceDeleteObjects({Owned}, false) != 1) AddError(TEXT("owned CurveTable deletion failed"));
        }
        if (FindObject<UCurveTable>(nullptr, *Path) || IFileManager::Get().FileExists(*Filename))
            AddError(TEXT("owned CurveTable cleanup did not reach absence"));
        IFileManager::Get().DeleteDirectory(*Workspace, false, true);
    };
    UPackage* Package = CreatePackage(*PackageName);
    UCurveTable* Table = NewObject<UCurveTable>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
    FSimpleCurve& Curve = Table->AddSimpleCurve(TEXT("Mapping"));
    Curve.SetKeyInterpMode(RCIM_Linear);
    Curve.SetDefaultValue(11);
    Curve.PreInfinityExtrap = RCCE_Cycle;
    Curve.PostInfinityExtrap = RCCE_Linear;
    Curve.SetKeys({FSimpleCurveKey(0, 37), FSimpleCurveKey(1, 19)});
    FAssetRegistryModule::AssetCreated(Table);
    FSavePackageArgs Args;
    Args.TopLevelFlags = RF_Public | RF_Standalone;
    if (!TestTrue(TEXT("native Simple baseline saves"), UPackage::SavePackage(Package, Table, *Filename, Args))) return false;
    Package->SetDirtyFlag(false);
    TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
    TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
    FString Error;
    if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error))
    { AddError(Error); return false; }
    const auto Export = MakeShared<FJsonObject>();
    Export->SetStringField(TEXT("target"), Path);
    Export->SetStringField(TEXT("workspace_path"), Workspace / TEXT("edit"));
    Export->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
    auto Result = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.export_workspace"), TEXT("t208-curve-export"), Export);
    if (!Result.IsOk()) { AddError(Result.GetErrorJson()); return false; }
    TArray<FString> Files;
    IFileManager::Get().FindFilesRecursive(Files, *(Workspace / TEXT("edit")), TEXT("*.hjson"), true, false);
    bool bEdited = false;
    for (const FString& File : Files)
    {
        if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
        FString Text;
        if (!FFileHelper::LoadFileToString(Text, *File) || !Text.Contains(TEXT("Mapping"))) continue;
        TestFalse(TEXT("ordinary curve has no diagnostic count or duplicate time"), Text.Contains(TEXT("key_count")) || Text.Contains(TEXT("time_seconds")) || Text.Contains(TEXT("carrier_cpp_type")));
        TestFalse(TEXT("CurveTable does not assert seconds"), Text.Contains(TEXT("seconds")));
        const int32 RowStart = Text.Find(TEXT("Mapping"));
        const FString Prefix = Text.Left(RowStart);
        Text = Text.Mid(RowStart);
        bEdited = Text.ReplaceInline(TEXT("37"), TEXT("73")) == 1
            && Text.ReplaceInline(TEXT("19"), TEXT("29")) == 1
            && Text.ReplaceInline(TEXT("11"), TEXT("17")) == 1;
        // These fields are native global settings; only the row document is edited.
        Text.ReplaceInline(TEXT("Linear"), TEXT("Constant"));
        Text.ReplaceInline(TEXT("Cycle"), TEXT("Oscillate"));
        Text = Prefix + Text;
        if (bEdited) bEdited = FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
        break;
    }
    if (!TestTrue(TEXT("ordinary exported typed row is edited"), bEdited)) return false;
    const auto Source = MakeShared<FJsonObject>();
    Source->SetStringField(TEXT("kind"), TEXT("directory"));
    Source->SetStringField(TEXT("path"), Workspace / TEXT("edit"));
    const auto Import = MakeShared<FJsonObject>();
    Import->SetStringField(TEXT("target"), Path);
    Import->SetObjectField(TEXT("source"), Source);
    Table = nullptr; Package = nullptr;
    Result = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), TEXT("t208-curve-import"), Import);
    if (!Result.IsOk()) { AddError(Result.GetErrorJson()); return false; }
    Table = LoadObject<UCurveTable>(nullptr, *Path);
    if (!TestTrue(TEXT("persistent Simple table reloads clean"), Table && Table->GetCurveTableMode() == ECurveTableMode::SimpleCurves && !Table->GetOutermost()->IsDirty())) return false;
    const FSimpleCurve* Actual = Table->GetSimpleCurveRowMap().FindRef(TEXT("Mapping"));
    if (!TestNotNull(TEXT("native row remains"), Actual)) return false;
    TestEqual(TEXT("native global interpolation"), Actual->GetKeyInterpMode(), RCIM_Constant);
    TestEqual(TEXT("native default"), Actual->GetDefaultValue(), 17.f);
    TestEqual(TEXT("native pre extrapolation"), Actual->PreInfinityExtrap.GetValue(), RCCE_Oscillate);
    TestEqual(TEXT("native post extrapolation"), Actual->PostInfinityExtrap.GetValue(), RCCE_Constant);
    if (TestEqual(TEXT("native key count"), Actual->GetConstRefOfKeys().Num(), 2))
    {
        TestEqual(TEXT("native first value"), Actual->GetConstRefOfKeys()[0].Value, 73.f);
        TestEqual(TEXT("native second value"), Actual->GetConstRefOfKeys()[1].Value, 29.f);
    }
    Export->SetStringField(TEXT("workspace_path"), Workspace / TEXT("readback"));
    Result = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.export_workspace"), TEXT("t208-curve-reexport"), Export);
    if (!Result.IsOk()) { AddError(Result.GetErrorJson()); return false; }
    Files.Reset();
    IFileManager::Get().FindFilesRecursive(Files, *(Workspace / TEXT("readback")), TEXT("*.hjson"), true, false);
    bool bReadback = false;
    for (const FString& File : Files)
    {
        if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
        FString Text;
        if (!FFileHelper::LoadFileToString(Text, *File) || !Text.Contains(TEXT("Mapping"))) continue;
        HjsonWorkspace::FHjsonDocumentPtr Document;
        HjsonWorkspace::FWorkspaceError ParseError;
        auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
        if (!HjsonWorkspace::TryParseHjsonDocument(Text, File, Resources, Document, ParseError)) { AddError(ParseError.GetCode()); return false; }
        const auto* Rows = Document->GetRoot()->Get(TEXT("rows"));
        const auto* Row = Rows ? Rows->Get(TEXT("Mapping")) : nullptr;
        const auto* Props = Row ? Row->Get(TEXT("properties")) : nullptr;
        const auto* Typed = Props ? Props->Get(TEXT("Curve")) : nullptr;
        const auto* Value = Typed ? Typed->Get(TEXT("value")) : nullptr;
        const auto* Channels = Value ? Value->Get(TEXT("channels")) : nullptr;
        const auto* Channel = Channels ? Channels->Get(TEXT("value")) : nullptr;
        const auto* Keys = Channel ? Channel->Get(TEXT("keys")) : nullptr;
        bReadback = Typed && Typed->Get(TEXT("type")) && Typed->Get(TEXT("type"))->GetText() == TEXT("curve")
            && Keys && Keys->GetKind() == HjsonWorkspace::EHjsonNodeKind::Array && Keys->GetItems().Num() == 2;
        if (bReadback)
        {
            const auto* First = Keys->GetItems()[0]->Get(TEXT("value"));
            const auto* Second = Keys->GetItems()[1]->Get(TEXT("value"));
            bReadback = First && Second && FCString::Atod(*First->GetLexeme()) == 73 && FCString::Atod(*Second->GetLexeme()) == 29;
        }
        break;
    }
    TestTrue(TEXT("independent ordinary reexport contains typed native values"), bReadback);
    return !HasAnyErrors();
}

namespace
{
    void SetLifecycleCurve(UCurveTable& Table, int32 Version)
    {
        Table.EmptyTable();
        FSimpleCurve& Curve = Table.AddSimpleCurve(TEXT("Mapping"));
        Curve.SetKeyInterpMode(Version == 1 ? RCIM_Constant : RCIM_Linear);
        Curve.SetDefaultValue(11.f + Version);
        Curve.PreInfinityExtrap = Version == 1 ? RCCE_Oscillate : RCCE_Cycle;
        Curve.PostInfinityExtrap = Version == 1 ? RCCE_CycleWithOffset : RCCE_Linear;
        Curve.SetKeys({FSimpleCurveKey(0, 37.f + Version), FSimpleCurveKey(1.f + Version, 19.f + Version)});
    }

    bool HasLifecycleCurve(const UCurveTable* Table, int32 Version)
    {
        if (!Table || Table->GetCurveTableMode() != ECurveTableMode::SimpleCurves || Table->GetSimpleCurveRowMap().Num() != 1) return false;
        const FSimpleCurve* Curve = Table->GetSimpleCurveRowMap().FindRef(TEXT("Mapping"));
        if (!Curve || Curve->GetKeyInterpMode() != (Version == 1 ? RCIM_Constant : RCIM_Linear)
            || Curve->GetDefaultValue() != 11.f + Version
            || Curve->PreInfinityExtrap != (Version == 1 ? RCCE_Oscillate : RCCE_Cycle)
            || Curve->PostInfinityExtrap != (Version == 1 ? RCCE_CycleWithOffset : RCCE_Linear)) return false;
        const auto& Keys = Curve->GetConstRefOfKeys();
        return Keys.Num() == 2 && Keys[0].Time == 0 && Keys[0].Value == 37.f + Version
            && Keys[1].Time == 1.f + Version && Keys[1].Value == 19.f + Version;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208CurveTableSimpleLifecycle,
    "UAI.Root.T208.CurveTable.SimpleLifecycle", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208CurveTableSimpleLifecycle::RunTest(const FString& Parameters)
{
    using namespace UE::UAI::DomainAdapters::DataTableCurveTable;
    using namespace UeAgentAuthoringOperationApply;
    using UE::UAI::HjsonWorkspace::DomainAuthoring::FRootPropertyApplyOutcome;
    FString Error;
    if (!RegisterCurveTableAuthoringState(Error)) { AddError(Error); return false; }
    const FString Name = TEXT("CT_Lifecycle_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString PackageName = TEXT("/Game/UAI/Transient/T208CurveTable/") + Name;
    const FString Path = PackageName + TEXT(".") + Name;
    const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
    ON_SCOPE_EXIT
    {
        if (UObject* Owned = LoadObject<UCurveTable>(nullptr, *Path))
        {
            Owned->GetOutermost()->SetDirtyFlag(false);
            if (ObjectTools::ForceDeleteObjects({Owned}, false) != 1) AddError(TEXT("owned lifecycle CurveTable deletion failed"));
        }
        if (FindObject<UCurveTable>(nullptr, *Path) || IFileManager::Get().FileExists(*Filename))
            AddError(TEXT("owned lifecycle CurveTable cleanup did not reach absence"));
    };
    UPackage* Package = CreatePackage(*PackageName);
    UCurveTable* Table = NewObject<UCurveTable>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
    SetLifecycleCurve(*Table, 0);
    FAssetRegistryModule::AssetCreated(Table);
    FSavePackageArgs Args;
    Args.TopLevelFlags = RF_Public | RF_Standalone;
    if (!UPackage::SavePackage(Package, Table, *Filename, Args)) { AddError(TEXT("lifecycle baseline save failed")); return false; }
    TArray<uint8> DiskBefore;
    if (!FFileHelper::LoadFileToArray(DiskBefore, *Filename)) return false;
    SetLifecycleCurve(*Table, 1);
    Table->MarkPackageDirty();
    TSharedPtr<FJsonObject> Desired;
    {
        TStrongObjectPtr<UCurveTable> Scratch(NewObject<UCurveTable>(GetTransientPackage()));
        SetLifecycleCurve(*Scratch, 2);
        if (!CaptureCurveTableRows(*Scratch, Desired, Error)) { AddError(Error); return false; }
        Scratch->MarkAsGarbage();
    }
    auto Restored = [&]()
    {
        UCurveTable* Actual = LoadObject<UCurveTable>(nullptr, *Path);
        TArray<uint8> Bytes;
        return HasLifecycleCurve(Actual, 1) && Actual->GetOutermost()->IsDirty()
            && FFileHelper::LoadFileToArray(Bytes, *Filename) && Bytes == DiskBefore;
    };
    const auto Default = Desired->GetObjectField(TEXT("rows"))->GetObjectField(TEXT("Mapping"))
        ->GetObjectField(TEXT("properties"))->GetObjectField(TEXT("Curve"))->GetObjectField(TEXT("value"))
        ->GetObjectField(TEXT("channels"))->GetObjectField(TEXT("value"))->GetObjectField(TEXT("default_value"));
    Default->SetStringField(TEXT("enabled"), TEXT("invalid"));
    FRootPropertyApplyOutcome Invalid;
    if (!TestFalse(TEXT("invalid default enabled rejects before mutation"), ApplyCurveTableAuthoring(Path, TEXT("t208-curve-invalid"), Desired, Invalid))
        || !TestEqual(TEXT("default rejection retains typed cause"), Invalid.Diagnostic.Code, FString(TEXT("curve_field_type_mismatch")))
        || !TestTrue(TEXT("invalid default preserves full dirty native and original disk"), Restored())) return false;
    Default->SetBoolField(TEXT("enabled"), true);
    Desired->SetStringField(TEXT("table_curve_mode"), TEXT("rich"));
    FRootPropertyApplyOutcome Conflict;
    if (!TestFalse(TEXT("mode conflict rejects before native mixed-mode mutation"), ApplyCurveTableAuthoring(Path, TEXT("t208-curve-mode"), Desired, Conflict))
        || !TestEqual(TEXT("mode rejection retains domain cause"), Conflict.Diagnostic.Code, FString(TEXT("curve_table_mode_read_only")))
        || !TestTrue(TEXT("mode rejection preserves full dirty native and original disk"), Restored())) return false;
    Desired->SetStringField(TEXT("table_curve_mode"), TEXT("simple"));

    class FRejectAfterStage final : public IDurablePackageStager
    {
    public:
        FRejectAfterStage(FString InPath, FString InFilename, TArray<uint8> InBefore)
            : Path(MoveTemp(InPath)), Filename(MoveTemp(InFilename)), Before(MoveTemp(InBefore)), Inner(CreateDefaultDurablePackageStager()) {}
        bool BuildArtifactInventory(const TArray<FPackageDurablePlan>& Plans,
            TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact>& Artifacts, FOperationFailure& Failure) override
        { return Inner->BuildArtifactInventory(Plans, Artifacts, Failure); }
        bool StagePackages(const TArray<FPackageDurablePlan>& Plans,
            TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact>& Artifacts, FOperationFailure& Failure) override
        {
            if (!Inner->StagePackages(Plans, Artifacts, Failure)) return false;
            UCurveTable* Actual = LoadObject<UCurveTable>(nullptr, *Path);
            if (!HasLifecycleCurve(Actual, 2)) return true; // The independent memory-before checkpoint stages before mutation.
            TArray<uint8> Bytes;
            bObserved = Actual->GetOutermost()->IsDirty() && FFileHelper::LoadFileToArray(Bytes, *Filename) && Bytes == Before;
            bool bAnyStaged = false;
            for (const auto& Artifact : Artifacts)
            {
                bObserved = bObserved && (IFileManager::Get().FileExists(*Artifact.StagingPath) == Artifact.bStagedExists);
                bAnyStaged |= Artifact.bStagedExists;
            }
            bObserved = bObserved && bAnyStaged;
            Failure.Diagnostic.Code = TEXT("t208_curve_stage_rejected_after_staging");
            Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code;
            return false;
        }
        bool bObserved = false;
    private:
        FString Path, Filename;
        TArray<uint8> Before;
        TSharedRef<IDurablePackageStager> Inner;
    };
    const auto Stager = MakeShared<FRejectAfterStage>(Path, Filename, DiskBefore);
    FBackendOptions Options;
    Options.PackageStager = Stager;
    FRootPropertyApplyOutcome Early;
    if (!TestFalse(TEXT("staged current curve failure rejects"), ApplyCurveTableAuthoring(Path, TEXT("t208-curve-early"), Desired, Early, &Options))
        || !TestTrue(TEXT("early failure follows real native edit and real staging before disk publish"), Stager->bObserved)
        || !TestTrue(TEXT("early failure restores full dirty native and original disk"), Restored()))
    { AddError(Early.Diagnostic.Code + TEXT(":") + Early.Diagnostic.PublicMessage); return false; }
    bool bLateObserved = false;
    FRootPropertyApplyOutcome Late;
    Late.FinalReadback = [&](FOperationFailure& Failure)
    {
        UCurveTable* Actual = LoadObject<UCurveTable>(nullptr, *Path);
        TArray<uint8> Published;
        bLateObserved = HasLifecycleCurve(Actual, 2) && !Actual->GetOutermost()->IsDirty()
            && FFileHelper::LoadFileToArray(Published, *Filename) && Published != DiskBefore
            && FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(FSoftObjectPath(Path)).IsValid();
        Failure.Diagnostic.Code = TEXT("t208_curve_final_readback_rejected");
        Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code;
        return false;
    };
    Table = nullptr; Package = nullptr;
    if (!TestFalse(TEXT("late final readback rejection rolls back"), ApplyCurveTableAuthoring(Path, TEXT("t208-curve-late"), Desired, Late))
        || !TestTrue(TEXT("late rejection observes persisted reloaded native and published bytes"), bLateObserved)
        || !TestTrue(TEXT("late checkpoint restores full dirty native separately from original disk"), Restored()))
    { AddError(Late.Diagnostic.Code + TEXT(":") + Late.Diagnostic.PublicMessage); return false; }
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208CurveTablePublicRich,
    "UAI.Root.T208.CurveTable.PublicRich", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208CurveTablePublicRich::RunTest(const FString& Parameters)
{
    using namespace UE::UAI;
    const FString Name = TEXT("CT_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString PackageName = TEXT("/Game/UAI/Transient/T208CurveTable/") + Name;
    const FString Path = PackageName + TEXT(".") + Name;
    const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
    const FString Workspace = FPaths::ProjectSavedDir() / TEXT("UeAssetFolders/T208CurveTable") / Name;
    ON_SCOPE_EXIT
    {
        if (UObject* Owned = LoadObject<UCurveTable>(nullptr, *Path))
        {
            Owned->GetOutermost()->SetDirtyFlag(false);
            if (ObjectTools::ForceDeleteObjects({Owned}, false) != 1) AddError(TEXT("owned CurveTable deletion failed"));
        }
        if (FindObject<UCurveTable>(nullptr, *Path) || IFileManager::Get().FileExists(*Filename))
            AddError(TEXT("owned CurveTable cleanup did not reach absence"));
        IFileManager::Get().DeleteDirectory(*Workspace, false, true);
    };
    UPackage* Package = CreatePackage(*PackageName);
    UCurveTable* Table = NewObject<UCurveTable>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
    FRichCurve& Curve = Table->AddRichCurve(TEXT("Mapping"));
    Curve.SetDefaultValue(11);
    Curve.PreInfinityExtrap = RCCE_Cycle;
    Curve.PostInfinityExtrap = RCCE_Linear;
    TArray<FRichCurveKey> InitialKeys{FRichCurveKey(0, 37), FRichCurveKey(1, 19), FRichCurveKey(2, 23)};
    for (int32 Index = 0; Index < InitialKeys.Num(); ++Index)
    {
        auto& Key = InitialKeys[Index];
        Key.InterpMode = RCIM_Cubic;
        Key.TangentMode = Index == 1 ? RCTM_Break : RCTM_User;
        Key.TangentWeightMode = RCTWM_WeightedBoth;
        Key.ArriveTangent = Index == 1 ? 7.25f : 5.25f;
        Key.LeaveTangent = Index == 1 ? 8.25f : 5.25f;
        Key.ArriveTangentWeight = Key.LeaveTangentWeight = 2.25f;
    }
    Curve.SetKeys(InitialKeys);
    FAssetRegistryModule::AssetCreated(Table);
    FSavePackageArgs Args;
    Args.TopLevelFlags = RF_Public | RF_Standalone;
    if (!TestTrue(TEXT("native Rich baseline saves"), UPackage::SavePackage(Package, Table, *Filename, Args))) return false;
    Package->SetDirtyFlag(false);
    TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
    TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
    FString Error;
    if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error))
    { AddError(Error); return false; }
    const auto Export = MakeShared<FJsonObject>();
    Export->SetStringField(TEXT("target"), Path);
    Export->SetStringField(TEXT("workspace_path"), Workspace / TEXT("edit"));
    Export->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
    auto Result = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.export_workspace"), TEXT("t208-rich-export"), Export);
    if (!Result.IsOk()) { AddError(Result.GetErrorJson()); return false; }
    TArray<FString> Files;
    IFileManager::Get().FindFilesRecursive(Files, *(Workspace / TEXT("edit")), TEXT("*.hjson"), true, false);
    bool bEdited = false;
    for (const FString& File : Files)
    {
        if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
        FString Text;
        if (!FFileHelper::LoadFileToString(Text, *File) || !Text.Contains(TEXT("Mapping"))) continue;
        TestFalse(TEXT("ordinary curve has no diagnostic count or duplicate time"), Text.Contains(TEXT("key_count")) || Text.Contains(TEXT("time_seconds")) || Text.Contains(TEXT("carrier_cpp_type")));
        TestFalse(TEXT("CurveTable does not assert seconds"), Text.Contains(TEXT("seconds")));
        const int32 RowStart = Text.Find(TEXT("Mapping"));
        const FString Prefix = Text.Left(RowStart);
        Text = Text.Mid(RowStart);
        bEdited = Text.ReplaceInline(TEXT("37"), TEXT("73")) == 1
            && Text.ReplaceInline(TEXT("19"), TEXT("29")) == 1
            && Text.ReplaceInline(TEXT("11"), TEXT("17")) == 1;
        // These fields are native global settings; only the row document is edited.
        Text.ReplaceInline(TEXT("23"), TEXT("32"));
        Text.ReplaceInline(TEXT("5.25"), TEXT("6.25"));
        Text.ReplaceInline(TEXT("7.25"), TEXT("9.25"));
        Text.ReplaceInline(TEXT("8.25"), TEXT("10.25"));
        Text.ReplaceInline(TEXT("2.25"), TEXT("2.5"));
        Text.ReplaceInline(TEXT("Linear"), TEXT("Constant"));
        Text.ReplaceInline(TEXT("Cycle"), TEXT("Oscillate"));
        Text = Prefix + Text;
        if (bEdited) bEdited = FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
        break;
    }
    if (!TestTrue(TEXT("ordinary exported typed row is edited"), bEdited)) return false;
    const auto Source = MakeShared<FJsonObject>();
    Source->SetStringField(TEXT("kind"), TEXT("directory"));
    Source->SetStringField(TEXT("path"), Workspace / TEXT("edit"));
    const auto Import = MakeShared<FJsonObject>();
    Import->SetStringField(TEXT("target"), Path);
    Import->SetObjectField(TEXT("source"), Source);
    Table = nullptr; Package = nullptr;
    Result = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), TEXT("t208-rich-import"), Import);
    if (!Result.IsOk()) { AddError(Result.GetErrorJson()); return false; }
    Table = LoadObject<UCurveTable>(nullptr, *Path);
    if (!TestTrue(TEXT("persistent Rich table reloads clean"), Table && Table->GetCurveTableMode() == ECurveTableMode::RichCurves && !Table->GetOutermost()->IsDirty())) return false;
    const FRichCurve* Actual = Table->GetRichCurveRowMap().FindRef(TEXT("Mapping"));
    if (!TestNotNull(TEXT("native row remains"), Actual)) return false;
    TestEqual(TEXT("native default"), Actual->GetDefaultValue(), 17.f);
    TestEqual(TEXT("native pre extrapolation"), Actual->PreInfinityExtrap.GetValue(), RCCE_Oscillate);
    TestEqual(TEXT("native post extrapolation"), Actual->PostInfinityExtrap.GetValue(), RCCE_Constant);
    if (!TestEqual(TEXT("native Rich key count"), Actual->GetConstRefOfKeys().Num(), 3)) return false;
    const float ExpectedValues[] = {73, 29, 32};
    for (int32 Index = 0; Index < 3; ++Index)
    {
        const auto& Key = Actual->GetConstRefOfKeys()[Index];
        TestEqual(TEXT("native cubic interpolation"), Key.InterpMode.GetValue(), RCIM_Cubic);
        TestEqual(TEXT("native manual tangent mode"), Key.TangentMode.GetValue(), Index == 1 ? RCTM_Break : RCTM_User);
        TestEqual(TEXT("native weighted tangent mode"), Key.TangentWeightMode.GetValue(), RCTWM_WeightedBoth);
        TestEqual(TEXT("native key time"), Key.Time, static_cast<float>(Index));
        TestEqual(TEXT("native key value"), Key.Value, ExpectedValues[Index]);
        TestEqual(TEXT("native manual arrive tangent"), Key.ArriveTangent, Index == 1 ? 9.25f : 6.25f);
        TestEqual(TEXT("native manual leave tangent"), Key.LeaveTangent, Index == 1 ? 10.25f : 6.25f);
        TestEqual(TEXT("native arrive weight"), Key.ArriveTangentWeight, 2.5f);
        TestEqual(TEXT("native leave weight"), Key.LeaveTangentWeight, 2.5f);
    }
    Export->SetStringField(TEXT("workspace_path"), Workspace / TEXT("readback"));
    Result = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.export_workspace"), TEXT("t208-rich-reexport"), Export);
    if (!Result.IsOk()) { AddError(Result.GetErrorJson()); return false; }
    Files.Reset();
    IFileManager::Get().FindFilesRecursive(Files, *(Workspace / TEXT("readback")), TEXT("*.hjson"), true, false);
    bool bReadback = false;
    for (const FString& File : Files)
    {
        if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
        FString Text;
        if (!FFileHelper::LoadFileToString(Text, *File) || !Text.Contains(TEXT("Mapping"))) continue;
        HjsonWorkspace::FHjsonDocumentPtr Document;
        HjsonWorkspace::FWorkspaceError ParseError;
        auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
        if (!HjsonWorkspace::TryParseHjsonDocument(Text, File, Resources, Document, ParseError)) { AddError(ParseError.GetCode()); return false; }
        const auto* Rows = Document->GetRoot()->Get(TEXT("rows"));
        const auto* Row = Rows ? Rows->Get(TEXT("Mapping")) : nullptr;
        const auto* Props = Row ? Row->Get(TEXT("properties")) : nullptr;
        const auto* Typed = Props ? Props->Get(TEXT("Curve")) : nullptr;
        const auto* Value = Typed ? Typed->Get(TEXT("value")) : nullptr;
        const auto* Channels = Value ? Value->Get(TEXT("channels")) : nullptr;
        const auto* Channel = Channels ? Channels->Get(TEXT("value")) : nullptr;
        const auto* Keys = Channel ? Channel->Get(TEXT("keys")) : nullptr;
        const auto* Type = Channel ? Channel->Get(TEXT("value_type")) : nullptr;
        const auto* Schema = Document->GetRoot()->Get(TEXT("row_schema"));
        const auto* Definitions = Schema ? Schema->Get(TEXT("definitions")) : nullptr;
        bReadback = Typed && Typed->Get(TEXT("type")) && Typed->Get(TEXT("type"))->GetText() == TEXT("curve")
            && Type && Type->GetText() == TEXT("float") && Definitions
            && Definitions->Get(TEXT("key_defaults")) && Definitions->Get(TEXT("tangent_mode")) && Definitions->Get(TEXT("tangent_weight_mode"))
            && Keys && Keys->GetKind() == HjsonWorkspace::EHjsonNodeKind::Array && Keys->GetItems().Num() == 3;
        auto Number = [](const HjsonWorkspace::FHjsonNode* Node, double Expected)
        { return Node && (Node->GetKind() == HjsonWorkspace::EHjsonNodeKind::Integer || Node->GetKind() == HjsonWorkspace::EHjsonNodeKind::Decimal) && FCString::Atod(*Node->GetLexeme()) == Expected; };
        if (bReadback)
        {
            for (int32 Index = 0; Index < 3; ++Index)
            {
                const auto& Key = Keys->GetItems()[Index];
                bReadback = bReadback && Number(Key->Get(TEXT("value")), ExpectedValues[Index])
                    && Number(Key->Get(TEXT("arrive_tangent")), Index == 1 ? 9.25 : 6.25)
                    && Number(Key->Get(TEXT("leave_tangent")), Index == 1 ? 10.25 : 6.25)
                    && Number(Key->Get(TEXT("arrive_tangent_weight")), 2.5)
                    && Number(Key->Get(TEXT("leave_tangent_weight")), 2.5)
                    && Key->Get(TEXT("tangent_mode")) && Key->Get(TEXT("tangent_mode"))->GetText() == (Index == 1 ? TEXT("Break") : TEXT("User"));
            }
        }
        break;
    }
    TestTrue(TEXT("independent ordinary reexport contains typed native values"), bReadback);
    return !HasAnyErrors();
}

namespace
{
    void SetRichLifecycleCurve(UCurveTable& Table, int32 Version)
    {
        Table.EmptyTable();
        FRichCurve& Curve = Table.AddRichCurve(TEXT("Mapping"));
        Curve.SetDefaultValue(11.f + Version);
        Curve.PreInfinityExtrap = Version == 1 ? RCCE_Oscillate : RCCE_Cycle;
        Curve.PostInfinityExtrap = Version == 1 ? RCCE_CycleWithOffset : RCCE_Linear;
        TArray<FRichCurveKey> Keys;
        for (int32 Index = 0; Index < 3; ++Index)
        {
            FRichCurveKey Key(static_cast<float>(Index), 37.f + Index + Version);
            Key.InterpMode = RCIM_Cubic;
            Key.TangentMode = Index == 1 ? RCTM_Break : RCTM_User;
            Key.TangentWeightMode = RCTWM_WeightedBoth;
            Key.ArriveTangent = (Index == 1 ? 7.25f : 5.25f) + Version;
            Key.LeaveTangent = (Index == 1 ? 8.25f : 5.25f) + Version;
            Key.ArriveTangentWeight = Key.LeaveTangentWeight = 2.25f + Version;
            Keys.Add(Key);
        }
        Curve.SetKeys(Keys);
    }

    bool HasRichLifecycleCurve(const UCurveTable* Table, int32 Version)
    {
        if (!Table || Table->GetCurveTableMode() != ECurveTableMode::RichCurves || Table->GetRichCurveRowMap().Num() != 1) return false;
        const FRichCurve* Curve = Table->GetRichCurveRowMap().FindRef(TEXT("Mapping"));
        if (!Curve || Curve->GetDefaultValue() != 11.f + Version
            || Curve->PreInfinityExtrap != (Version == 1 ? RCCE_Oscillate : RCCE_Cycle)
            || Curve->PostInfinityExtrap != (Version == 1 ? RCCE_CycleWithOffset : RCCE_Linear)) return false;
        const auto& Keys = Curve->GetConstRefOfKeys();
        if (Keys.Num() != 3) return false;
        for (int32 Index = 0; Index < Keys.Num(); ++Index)
        {
            const auto& Key = Keys[Index];
            if (Key.Time != static_cast<float>(Index) || Key.Value != 37.f + Index + Version
                || Key.InterpMode != RCIM_Cubic || Key.TangentMode != (Index == 1 ? RCTM_Break : RCTM_User)
                || Key.TangentWeightMode != RCTWM_WeightedBoth
                || Key.ArriveTangent != (Index == 1 ? 7.25f : 5.25f) + Version
                || Key.LeaveTangent != (Index == 1 ? 8.25f : 5.25f) + Version
                || Key.ArriveTangentWeight != 2.25f + Version || Key.LeaveTangentWeight != 2.25f + Version) return false;
        }
        return true;
    }
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208CurveTableRichLifecycle,
    "UAI.Root.T208.CurveTable.RichLifecycle", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208CurveTableRichLifecycle::RunTest(const FString& Parameters)
{
    using namespace UE::UAI::DomainAdapters::DataTableCurveTable;
    using namespace UeAgentAuthoringOperationApply;
    using UE::UAI::HjsonWorkspace::DomainAuthoring::FRootPropertyApplyOutcome;
    FString Error;
    if (!RegisterCurveTableAuthoringState(Error)) { AddError(Error); return false; }
    const FString Name = TEXT("CT_RichLifecycle_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString PackageName = TEXT("/Game/UAI/Transient/T208CurveTable/") + Name;
    const FString Path = PackageName + TEXT(".") + Name;
    const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
    ON_SCOPE_EXIT
    {
        if (UObject* Owned = LoadObject<UCurveTable>(nullptr, *Path))
        {
            Owned->GetOutermost()->SetDirtyFlag(false);
            if (ObjectTools::ForceDeleteObjects({Owned}, false) != 1) AddError(TEXT("owned lifecycle CurveTable deletion failed"));
        }
        if (FindObject<UCurveTable>(nullptr, *Path) || IFileManager::Get().FileExists(*Filename))
            AddError(TEXT("owned lifecycle CurveTable cleanup did not reach absence"));
    };
    UPackage* Package = CreatePackage(*PackageName);
    UCurveTable* Table = NewObject<UCurveTable>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
    SetRichLifecycleCurve(*Table, 0);
    FAssetRegistryModule::AssetCreated(Table);
    FSavePackageArgs Args;
    Args.TopLevelFlags = RF_Public | RF_Standalone;
    if (!UPackage::SavePackage(Package, Table, *Filename, Args)) { AddError(TEXT("lifecycle baseline save failed")); return false; }
    TArray<uint8> DiskBefore;
    if (!FFileHelper::LoadFileToArray(DiskBefore, *Filename)) return false;
    SetRichLifecycleCurve(*Table, 1);
    Table->MarkPackageDirty();
    TSharedPtr<FJsonObject> Desired;
    {
        TStrongObjectPtr<UCurveTable> Scratch(NewObject<UCurveTable>(GetTransientPackage()));
        SetRichLifecycleCurve(*Scratch, 2);
        if (!CaptureCurveTableRows(*Scratch, Desired, Error)) { AddError(Error); return false; }
        Scratch->MarkAsGarbage();
    }
    auto Restored = [&]()
    {
        UCurveTable* Actual = LoadObject<UCurveTable>(nullptr, *Path);
        TArray<uint8> Bytes;
        return HasRichLifecycleCurve(Actual, 1) && Actual->GetOutermost()->IsDirty()
            && FFileHelper::LoadFileToArray(Bytes, *Filename) && Bytes == DiskBefore;
    };
    const auto Key = Desired->GetObjectField(TEXT("rows"))->GetObjectField(TEXT("Mapping"))
        ->GetObjectField(TEXT("properties"))->GetObjectField(TEXT("Curve"))->GetObjectField(TEXT("value"))
        ->GetObjectField(TEXT("channels"))->GetObjectField(TEXT("value"))->GetArrayField(TEXT("keys"))[1]->AsObject();
    Key->SetNumberField(TEXT("tangent_mode"), 1);
    FRootPropertyApplyOutcome InvalidEnum;
    if (!TestFalse(TEXT("nonstring tangent enum rejects before mutation"), ApplyCurveTableAuthoring(Path, TEXT("t208-rich-enum"), Desired, InvalidEnum))
        || !TestEqual(TEXT("tangent enum rejection retains typed cause"), InvalidEnum.Diagnostic.Code, FString(TEXT("curve_field_type_mismatch")))
        || !TestTrue(TEXT("enum rejection preserves complete dirty Rich state and disk"), Restored())) return false;
    Key->SetStringField(TEXT("tangent_mode"), TEXT("Break"));
    Key->SetStringField(TEXT("arrive_tangent"), TEXT("9.25"));
    FRootPropertyApplyOutcome InvalidNumber;
    if (!TestFalse(TEXT("numeric string tangent rejects before mutation"), ApplyCurveTableAuthoring(Path, TEXT("t208-rich-number"), Desired, InvalidNumber))
        || !TestEqual(TEXT("tangent number rejection retains typed cause"), InvalidNumber.Diagnostic.Code, FString(TEXT("curve_field_type_mismatch")))
        || !TestTrue(TEXT("number rejection preserves complete dirty Rich state and disk"), Restored())) return false;
    Key->SetNumberField(TEXT("arrive_tangent"), 9.25);
    bool bLateObserved = false;
    FRootPropertyApplyOutcome Late;
    Late.FinalReadback = [&](FOperationFailure& Failure)
    {
        UCurveTable* Actual = LoadObject<UCurveTable>(nullptr, *Path);
        TArray<uint8> Published;
        bLateObserved = HasRichLifecycleCurve(Actual, 2) && !Actual->GetOutermost()->IsDirty()
            && FFileHelper::LoadFileToArray(Published, *Filename) && Published != DiskBefore
            && FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(FSoftObjectPath(Path)).IsValid();
        Failure.Diagnostic.Code = TEXT("t208_curve_final_readback_rejected");
        Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code;
        return false;
    };
    Table = nullptr; Package = nullptr;
    if (!TestFalse(TEXT("late final readback rejection rolls back"), ApplyCurveTableAuthoring(Path, TEXT("t208-rich-late"), Desired, Late))
        || !TestTrue(TEXT("late rejection observes persisted reloaded native and published bytes"), bLateObserved)
        || !TestTrue(TEXT("late checkpoint restores full dirty native separately from original disk"), Restored()))
    { AddError(Late.Diagnostic.Code + TEXT(":") + Late.Diagnostic.PublicMessage); return false; }
    return !HasAnyErrors();
}

namespace
{
    bool ReplaceTypedRowsDocument(const FString& Workspace, const FString& Replacement,
        FString* BeforeRows, bool bRequireEmpty, FString& Error, bool bWrite = true,
        const FString* SelectedMode = nullptr)
    {
        using namespace UE::UAI::HjsonWorkspace;
        TArray<FString> Files;
        IFileManager::Get().FindFilesRecursive(Files, *Workspace, TEXT("*.hjson"), true, false);
        for (const FString& File : Files)
        {
            if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
            FString Text;
            if (!FFileHelper::LoadFileToString(Text, *File)) continue;
            auto Resources = MakeSemanticWorkspaceResourceTracker();
            FHjsonDocumentPtr Document;
            FWorkspaceError ParseError;
            if (!TryParseHjsonDocument(Text, File, Resources, Document, ParseError)) { Error = ParseError.GetCode(); return false; }
            const auto* Rows = Document->GetRoot()->Get(TEXT("rows"));
            if (!Rows) continue;
            if (Rows->GetKind() != EHjsonNodeKind::Object || (bRequireEmpty && !Rows->GetEntries().IsEmpty()))
            { Error = TEXT("typed empty rows document mismatch"); return false; }
            const auto* Schema = Document->GetRoot()->Get(TEXT("row_schema"));
            if (!Schema || !Schema->Get(TEXT("properties"))) { Error = TEXT("typed empty schema missing"); return false; }
            if (SelectedMode)
            {
                const auto* Mode = Document->GetRoot()->Get(TEXT("table_curve_mode"));
                const auto* Definitions = Schema->Get(TEXT("definitions"));
                const auto* Templates = Definitions ? Definitions->Get(TEXT("mode_templates")) : nullptr;
                if (!Mode || Mode->GetText() != TEXT("empty") || !Templates
                    || !Templates->Get(TEXT("simple")) || !Templates->Get(TEXT("rich")))
                { Error = TEXT("native Empty mode selection schema missing"); return false; }
            }
            FTCHARToUTF8 Utf8(*Text);
            const int64 Start = Rows->GetProvenance().GetByteStart();
            const int64 End = Rows->GetProvenance().GetByteEnd();
            if (Start < 0 || End < Start || End > Utf8.Length()) { Error = TEXT("visible rows range invalid"); return false; }
            if (BeforeRows)
            {
                FUTF8ToTCHAR Converted(Utf8.Get() + Start, static_cast<int32>(End - Start));
                *BeforeRows = FString(Converted.Length(), Converted.Get());
            }
            if (!bWrite) return true;
            FTCHARToUTF8 NewRows(*Replacement);
            TArray<uint8> Bytes;
            Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), static_cast<int32>(Start));
            Bytes.Append(reinterpret_cast<const uint8*>(NewRows.Get()), NewRows.Length());
            Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get() + End), Utf8.Length() - static_cast<int32>(End));
            if (SelectedMode)
            {
                // Reparse after replacing rows so source ranges still identify the visible mode only.
                FUTF8ToTCHAR Updated(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
                FHjsonDocumentPtr Edited;
                if (!TryParseHjsonDocument(FString(Updated.Length(), Updated.Get()), File, Resources, Edited, ParseError))
                { Error = ParseError.GetCode(); return false; }
                const auto* Mode = Edited->GetRoot()->Get(TEXT("table_curve_mode"));
                if (!Mode || Mode->GetKind() != EHjsonNodeKind::String) { Error = TEXT("visible curve mode missing"); return false; }
                const int32 ModeStart = static_cast<int32>(Mode->GetProvenance().GetByteStart());
                const int32 ModeEnd = static_cast<int32>(Mode->GetProvenance().GetByteEnd());
                if (ModeStart < 0 || ModeEnd < ModeStart || ModeEnd > Bytes.Num()) return false;
                const FString Quoted = TEXT("\"") + *SelectedMode + TEXT("\"");
                FTCHARToUTF8 ModeUtf8(*Quoted);
                TArray<uint8> Selected;
                Selected.Append(Bytes.GetData(), ModeStart);
                Selected.Append(reinterpret_cast<const uint8*>(ModeUtf8.Get()), ModeUtf8.Length());
                Selected.Append(Bytes.GetData() + ModeEnd, Bytes.Num() - ModeEnd);
                Bytes = MoveTemp(Selected);
            }
            return FFileHelper::SaveArrayToFile(Bytes, *File);
        }
        Error = TEXT("visible typed rows document missing");
        return false;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208CurveTableTypedEmpty,
    "UAI.Root.T208.CurveTable.PublicTypedEmpty", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208CurveTableTypedEmpty::RunTest(const FString& Parameters)
{
    using namespace UE::UAI;
    const FString Name = TEXT("CT_Empty_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString PackageName = TEXT("/Game/UAI/Transient/T208CurveTable/") + Name;
    const FString Path = PackageName + TEXT(".") + Name;
    const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
    const FString Workspace = FPaths::ProjectSavedDir() / TEXT("UeAssetFolders/T208CurveTable") / Name;
    ON_SCOPE_EXIT
    {
        if (UObject* Owned = LoadObject<UCurveTable>(nullptr, *Path))
        {
            Owned->GetOutermost()->SetDirtyFlag(false);
            if (ObjectTools::ForceDeleteObjects({Owned}, false) != 1) AddError(TEXT("typed empty owned deletion failed"));
        }
        if (FindObject<UCurveTable>(nullptr, *Path) || IFileManager::Get().FileExists(*Filename)) AddError(TEXT("typed empty cleanup not absent"));
        IFileManager::Get().DeleteDirectory(*Workspace, false, true);
    };
    UPackage* Package = CreatePackage(*PackageName);
    UCurveTable* Table = NewObject<UCurveTable>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
    SetLifecycleCurve(*Table, 0);
    FAssetRegistryModule::AssetCreated(Table);
    FSavePackageArgs Args;
    Args.TopLevelFlags = RF_Public | RF_Standalone;
    if (!UPackage::SavePackage(Package, Table, *Filename, Args)) return false;
    Package->SetDirtyFlag(false);
    TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
    TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
    FString Error;
    if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error); return false; }
    auto ExportTo = [&](const FString& Leaf)
    {
        const auto Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("target"), Path);
        Params->SetStringField(TEXT("workspace_path"), Workspace / Leaf);
        Params->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
        const auto Result = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.export_workspace"), TEXT("t208-empty-export-") + Leaf, Params);
        if (!Result.IsOk()) { AddError(Result.GetErrorJson()); return false; }
        return true;
    };
    auto ImportFrom = [&](const FString& Leaf)
    {
        const auto Source = MakeShared<FJsonObject>();
        Source->SetStringField(TEXT("kind"), TEXT("directory"));
        Source->SetStringField(TEXT("path"), Workspace / Leaf);
        const auto Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("target"), Path);
        Params->SetObjectField(TEXT("source"), Source);
        const auto Result = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), TEXT("t208-empty-import-") + Leaf, Params);
        if (!Result.IsOk()) { AddError(Result.GetErrorJson()); return false; }
        return true;
    };
    if (!ExportTo(TEXT("remove"))) return false;
    FString OriginalRows;
    if (!ReplaceTypedRowsDocument(Workspace / TEXT("remove"), TEXT("{}"), &OriginalRows, false, Error)) { AddError(Error); return false; }
    Table = nullptr; Package = nullptr;
    if (!ImportFrom(TEXT("remove"))) return false;
    Table = LoadObject<UCurveTable>(nullptr, *Path);
    if (!TestTrue(TEXT("persistent zero-row Simple table preserves its native mode"), Table
        && Table->GetCurveTableMode() == ECurveTableMode::SimpleCurves && Table->GetSimpleCurveRowMap().IsEmpty()
        && !Table->GetOutermost()->IsDirty())) return false;
    if (!ExportTo(TEXT("add"))) return false;
    OriginalRows.ReplaceInline(TEXT("Mapping"), TEXT("Added"));
    OriginalRows.ReplaceInline(TEXT("37"), TEXT("73"));
    if (!ReplaceTypedRowsDocument(Workspace / TEXT("add"), OriginalRows, nullptr, true, Error)) { AddError(Error); return false; }
    Table = nullptr;
    if (!ImportFrom(TEXT("add"))) return false;
    Table = LoadObject<UCurveTable>(nullptr, *Path);
    if (!TestTrue(TEXT("same-mode row is persisted after typed empty"), Table
        && Table->GetCurveTableMode() == ECurveTableMode::SimpleCurves && Table->GetSimpleCurveRowMap().Num() == 1
        && !Table->GetOutermost()->IsDirty())) return false;
    const FSimpleCurve* Curve = Table->GetSimpleCurveRowMap().FindRef(TEXT("Added"));
    if (!TestTrue(TEXT("native assigned row retains complete curve"), Curve && Curve->GetConstRefOfKeys().Num() == 2
        && Curve->GetConstRefOfKeys()[0].Value == 73 && Curve->GetConstRefOfKeys()[1].Value == 19
        && Curve->GetDefaultValue() == 11 && Curve->GetKeyInterpMode() == RCIM_Linear
        && Curve->PreInfinityExtrap == RCCE_Cycle && Curve->PostInfinityExtrap == RCCE_Linear)) return false;
    if (!ExportTo(TEXT("readback"))) return false;
    // Reuse the public AST range reader; no import follows this independent observation.
    FString ReexportRows;
    if (!ReplaceTypedRowsDocument(Workspace / TEXT("readback"), FString(), &ReexportRows, false, Error, false)) { AddError(Error); return false; }
    auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
    HjsonWorkspace::FHjsonDocumentPtr Document;
    HjsonWorkspace::FWorkspaceError ParseError;
    if (!HjsonWorkspace::TryParseHjsonDocument(ReexportRows, TEXT("readback:rows"), Resources, Document, ParseError)) { AddError(ParseError.GetCode()); return false; }
    const auto* Row = Document->GetRoot()->Get(TEXT("Added"));
    const auto* Props = Row ? Row->Get(TEXT("properties")) : nullptr;
    const auto* Typed = Props ? Props->Get(TEXT("Curve")) : nullptr;
    const auto* Value = Typed ? Typed->Get(TEXT("value")) : nullptr;
    const auto* Channels = Value ? Value->Get(TEXT("channels")) : nullptr;
    const auto* Channel = Channels ? Channels->Get(TEXT("value")) : nullptr;
    const auto* Keys = Channel ? Channel->Get(TEXT("keys")) : nullptr;
    const auto* First = Keys && Keys->GetItems().Num() == 2 ? Keys->GetItems()[0]->Get(TEXT("value")) : nullptr;
    TestTrue(TEXT("independent typed reexport preserves added value"), Typed && Typed->Get(TEXT("type"))
        && Typed->Get(TEXT("type"))->GetText() == TEXT("curve") && First && FCString::Atod(*First->GetLexeme()) == 73);
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208CurveTableEmptySelection,
    "UAI.Root.T208.CurveTable.PublicEmptySelection", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208CurveTableEmptySelection::RunTest(const FString& Parameters)
{
    using namespace UE::UAI;
    const FString Name = TEXT("CT_Select_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString PackageName = TEXT("/Game/UAI/Transient/T208CurveTable/") + Name;
    const FString Path = PackageName + TEXT(".") + Name;
    const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
    const FString Workspace = FPaths::ProjectSavedDir() / TEXT("UeAssetFolders/T208CurveTable") / Name;
    ON_SCOPE_EXIT
    {
        if (UObject* Owned = LoadObject<UCurveTable>(nullptr, *Path))
        {
            Owned->GetOutermost()->SetDirtyFlag(false);
            if (ObjectTools::ForceDeleteObjects({Owned}, false) != 1) AddError(TEXT("typed empty owned deletion failed"));
        }
        if (FindObject<UCurveTable>(nullptr, *Path) || IFileManager::Get().FileExists(*Filename)) AddError(TEXT("typed empty cleanup not absent"));
        IFileManager::Get().DeleteDirectory(*Workspace, false, true);
    };
    UPackage* Package = CreatePackage(*PackageName);
    UCurveTable* Table = NewObject<UCurveTable>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
    FAssetRegistryModule::AssetCreated(Table);
    FSavePackageArgs Args;
    Args.TopLevelFlags = RF_Public | RF_Standalone;
    if (!UPackage::SavePackage(Package, Table, *Filename, Args)) return false;
    Package->SetDirtyFlag(false);
    TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
    TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
    FString Error;
    if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error); return false; }
    auto ExportTo = [&](const FString& Leaf)
    {
        const auto Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("target"), Path);
        Params->SetStringField(TEXT("workspace_path"), Workspace / Leaf);
        Params->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
        const auto Result = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.export_workspace"), TEXT("t208-empty-export-") + Leaf, Params);
        if (!Result.IsOk()) { AddError(Result.GetErrorJson()); return false; }
        return true;
    };
    auto ImportFrom = [&](const FString& Leaf)
    {
        const auto Source = MakeShared<FJsonObject>();
        Source->SetStringField(TEXT("kind"), TEXT("directory"));
        Source->SetStringField(TEXT("path"), Workspace / Leaf);
        const auto Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("target"), Path);
        Params->SetObjectField(TEXT("source"), Source);
        const auto Result = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), TEXT("t208-empty-import-") + Leaf, Params);
        if (!Result.IsOk()) { AddError(Result.GetErrorJson()); return false; }
        return true;
    };
    if (!ExportTo(TEXT("add"))) return false;
    const FString Rows = TEXT(R"HJSON({
      Added: {
        properties: {
          Curve: {
            type: curve
            value: {
              interp_mode: Constant
              pre_infinity_extrap: Cycle
              post_infinity_extrap: Linear
              channels: {
                value: {
                  default_value: { enabled: true, value: 17 }
                  keys: [ { time: 0, value: 73 }, { time: 1, value: 29 } ]
                }
              }
            }
          }
        }
      }
    })HJSON");
    const FString SelectedMode = TEXT("simple");
    if (!ReplaceTypedRowsDocument(Workspace / TEXT("add"), Rows, nullptr, true, Error, true, &SelectedMode))
    { AddError(Error); return false; }
    Table = nullptr; Package = nullptr;
    if (!ImportFrom(TEXT("add"))) return false;
    Table = LoadObject<UCurveTable>(nullptr, *Path);
    if (!TestTrue(TEXT("first explicit mode and row are persisted from native Empty"), Table
        && Table->GetCurveTableMode() == ECurveTableMode::SimpleCurves && Table->GetSimpleCurveRowMap().Num() == 1
        && !Table->GetOutermost()->IsDirty())) return false;
    const FSimpleCurve* Curve = Table->GetSimpleCurveRowMap().FindRef(TEXT("Added"));
    if (!TestTrue(TEXT("native assigned row retains complete curve"), Curve && Curve->GetConstRefOfKeys().Num() == 2
        && Curve->GetConstRefOfKeys()[0].Value == 73 && Curve->GetConstRefOfKeys()[1].Value == 29
        && Curve->GetDefaultValue() == 17 && Curve->GetKeyInterpMode() == RCIM_Constant
        && Curve->PreInfinityExtrap == RCCE_Cycle && Curve->PostInfinityExtrap == RCCE_Linear)) return false;
    if (!ExportTo(TEXT("readback"))) return false;
    // Reuse the public AST range reader; no import follows this independent observation.
    FString ReexportRows;
    if (!ReplaceTypedRowsDocument(Workspace / TEXT("readback"), FString(), &ReexportRows, false, Error, false)) { AddError(Error); return false; }
    auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
    HjsonWorkspace::FHjsonDocumentPtr Document;
    HjsonWorkspace::FWorkspaceError ParseError;
    if (!HjsonWorkspace::TryParseHjsonDocument(ReexportRows, TEXT("readback:rows"), Resources, Document, ParseError)) { AddError(ParseError.GetCode()); return false; }
    const auto* Row = Document->GetRoot()->Get(TEXT("Added"));
    const auto* Props = Row ? Row->Get(TEXT("properties")) : nullptr;
    const auto* Typed = Props ? Props->Get(TEXT("Curve")) : nullptr;
    const auto* Value = Typed ? Typed->Get(TEXT("value")) : nullptr;
    const auto* Channels = Value ? Value->Get(TEXT("channels")) : nullptr;
    const auto* Channel = Channels ? Channels->Get(TEXT("value")) : nullptr;
    const auto* Keys = Channel ? Channel->Get(TEXT("keys")) : nullptr;
    const auto* First = Keys && Keys->GetItems().Num() == 2 ? Keys->GetItems()[0]->Get(TEXT("value")) : nullptr;
    TestTrue(TEXT("independent typed reexport preserves added value"), Typed && Typed->Get(TEXT("type"))
        && Typed->Get(TEXT("type"))->GetText() == TEXT("curve") && First && FCString::Atod(*First->GetLexeme()) == 73);
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208CurveTableEmptyLifecycle,
    "UAI.Root.T208.CurveTable.EmptyLifecycle", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208CurveTableEmptyLifecycle::RunTest(const FString& Parameters)
{
    using namespace UE::UAI::DomainAdapters::DataTableCurveTable;
    using namespace UeAgentAuthoringOperationApply;
    using UE::UAI::HjsonWorkspace::DomainAuthoring::FRootPropertyApplyOutcome;
    FString Error;
    if (!RegisterCurveTableAuthoringState(Error)) { AddError(Error); return false; }
    const FString Name = TEXT("CT_EmptyLifecycle_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString PackageName = TEXT("/Game/UAI/Transient/T208CurveTable/") + Name;
    const FString Path = PackageName + TEXT(".") + Name;
    const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
    ON_SCOPE_EXIT
    {
        if (UObject* Owned = LoadObject<UCurveTable>(nullptr, *Path))
        {
            Owned->GetOutermost()->SetDirtyFlag(false);
            if (ObjectTools::ForceDeleteObjects({Owned}, false) != 1) AddError(TEXT("owned Empty lifecycle deletion failed"));
        }
        if (FindObject<UCurveTable>(nullptr, *Path) || IFileManager::Get().FileExists(*Filename))
            AddError(TEXT("owned Empty lifecycle cleanup not absent"));
    };
    UPackage* Package = CreatePackage(*PackageName);
    UCurveTable* Table = NewObject<UCurveTable>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
    SetLifecycleCurve(*Table, 0);
    FAssetRegistryModule::AssetCreated(Table);
    FSavePackageArgs Args;
    Args.TopLevelFlags = RF_Public | RF_Standalone;
    if (!UPackage::SavePackage(Package, Table, *Filename, Args)) { AddError(TEXT("typed baseline save failed")); return false; }
    TArray<uint8> DiskBefore;
    if (!FFileHelper::LoadFileToArray(DiskBefore, *Filename)) return false;
    Table->EmptyTable();
    Table->MarkPackageDirty();
    TSharedPtr<FJsonObject> Desired;
    {
        TStrongObjectPtr<UCurveTable> Scratch(NewObject<UCurveTable>(GetTransientPackage()));
        SetRichLifecycleCurve(*Scratch, 2);
        if (!CaptureCurveTableRows(*Scratch, Desired, Error)) { AddError(Error); return false; }
        Scratch->MarkAsGarbage();
    }
    auto Restored = [&]()
    {
        UCurveTable* Actual = LoadObject<UCurveTable>(nullptr, *Path);
        TArray<uint8> Bytes;
        return Actual && Actual->GetCurveTableMode() == ECurveTableMode::Empty && Actual->GetRowMap().IsEmpty()
            && Actual->GetOutermost()->IsDirty() && FFileHelper::LoadFileToArray(Bytes, *Filename) && Bytes == DiskBefore;
    };
    auto Reject = [&](const FString& Label, const FString& Cause)
    {
        FRootPropertyApplyOutcome Outcome;
        return TestFalse(*Label, ApplyCurveTableAuthoring(Path, Label, Desired, Outcome))
            && TestEqual(TEXT("write rejection retains exact cause"), Outcome.Diagnostic.Code, Cause)
            && TestTrue(TEXT("write rejection preserves dirty Empty and original typed disk"), Restored());
    };
    Desired->SetStringField(TEXT("table_curve_mode"), TEXT("empty"));
    if (!Reject(TEXT("empty mode cannot carry rows"), TEXT("curve_table_first_row_mode_required"))) return false;
    const auto Rows = Desired->GetObjectField(TEXT("rows"));
    Desired->SetObjectField(TEXT("rows"), MakeShared<FJsonObject>());
    for (const FString Mode : {FString(TEXT("simple")), FString(TEXT("rich"))})
    {
        Desired->SetStringField(TEXT("table_curve_mode"), Mode);
        if (!Reject(TEXT("selected mode requires first real row: ") + Mode, TEXT("curve_table_first_row_mode_required"))) return false;
    }
    Desired->SetObjectField(TEXT("rows"), Rows);
    const auto Default = Rows->GetObjectField(TEXT("Mapping"))->GetObjectField(TEXT("properties"))
        ->GetObjectField(TEXT("Curve"))->GetObjectField(TEXT("value"))->GetObjectField(TEXT("channels"))
        ->GetObjectField(TEXT("value"))->GetObjectField(TEXT("default_value"));
    const double BeforeDefault = Default->GetNumberField(TEXT("value"));
    Default->SetBoolField(TEXT("enabled"), true);
    Default->SetNumberField(TEXT("value"), static_cast<double>(MAX_flt));
    if (!Reject(TEXT("enabled default cannot use unset sentinel"), TEXT("curve_default_value_unrepresentable"))) return false;
    Default->SetNumberField(TEXT("value"), BeforeDefault);

    class FRejectAfterStage final : public IDurablePackageStager
    {
    public:
        FRejectAfterStage(FString InPath, FString InFilename, TArray<uint8> InBefore)
            : Path(MoveTemp(InPath)), Filename(MoveTemp(InFilename)), Before(MoveTemp(InBefore)), Inner(CreateDefaultDurablePackageStager()) {}
        bool BuildArtifactInventory(const TArray<FPackageDurablePlan>& Plans,
            TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact>& Artifacts, FOperationFailure& Failure) override
        { return Inner->BuildArtifactInventory(Plans, Artifacts, Failure); }
        bool StagePackages(const TArray<FPackageDurablePlan>& Plans,
            TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact>& Artifacts, FOperationFailure& Failure) override
        {
            if (!Inner->StagePackages(Plans, Artifacts, Failure)) return false;
            UCurveTable* Actual = LoadObject<UCurveTable>(nullptr, *Path);
            if (!HasRichLifecycleCurve(Actual, 2)) return true;
            TArray<uint8> Bytes;
            bObserved = Actual->GetOutermost()->IsDirty() && FFileHelper::LoadFileToArray(Bytes, *Filename) && Bytes == Before;
            bool bAnyStaged = false;
            for (const auto& Artifact : Artifacts)
            {
                bObserved = bObserved && (IFileManager::Get().FileExists(*Artifact.StagingPath) == Artifact.bStagedExists);
                bAnyStaged |= Artifact.bStagedExists;
            }
            bObserved = bObserved && bAnyStaged;
            Failure.Diagnostic.Code = TEXT("t208_empty_stage_rejected_after_staging");
            Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code;
            return false;
        }
        bool bObserved = false;
    private:
        FString Path, Filename;
        TArray<uint8> Before;
        TSharedRef<IDurablePackageStager> Inner;
    };
    const auto Stager = MakeShared<FRejectAfterStage>(Path, Filename, DiskBefore);
    FBackendOptions Options;
    Options.PackageStager = Stager;
    FRootPropertyApplyOutcome Early;
    if (!TestFalse(TEXT("real staged Rich edit is rejected"), ApplyCurveTableAuthoring(Path, TEXT("t208-empty-early"), Desired, Early, &Options))
        || !TestTrue(TEXT("early failure observes changed native and actual staging before publication"), Stager->bObserved)
        || !TestTrue(TEXT("early native rollback restores genuine dirty Empty and typed disk"), Restored()))
    { AddError(Early.Diagnostic.Code + TEXT(":") + Early.Diagnostic.PublicMessage); return false; }
    bool bLateObserved = false;
    FRootPropertyApplyOutcome Late;
    Late.FinalReadback = [&](FOperationFailure& Failure)
    {
        UCurveTable* Actual = LoadObject<UCurveTable>(nullptr, *Path);
        TArray<uint8> Published;
        bLateObserved = HasRichLifecycleCurve(Actual, 2) && !Actual->GetOutermost()->IsDirty()
            && FFileHelper::LoadFileToArray(Published, *Filename) && Published != DiskBefore
            && FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(FSoftObjectPath(Path)).IsValid();
        Failure.Diagnostic.Code = TEXT("t208_empty_final_readback_rejected");
        Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code;
        return false;
    };
    Table = nullptr; Package = nullptr;
    if (!TestFalse(TEXT("persistent Rich final readback is rejected"), ApplyCurveTableAuthoring(Path, TEXT("t208-empty-late"), Desired, Late))
        || !TestTrue(TEXT("late failure observes clean persisted Rich and changed published bytes"), bLateObserved)
        || !TestTrue(TEXT("late checkpoint restores genuine dirty Empty separately from original typed disk"), Restored()))
    { AddError(Late.Diagnostic.Code + TEXT(":") + Late.Diagnostic.PublicMessage); return false; }
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208CurveTablePublicInitialization,
    "UAI.Root.T208.CurveTable.PublicInitialization", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208CurveTablePublicInitialization::RunTest(const FString& Parameters)
{
    using namespace UE::UAI;
    using namespace UE::UAI::DomainAdapters::DataTableCurveTable;
    const FString Name = TEXT("CT_Init_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString PackageName = TEXT("/Game/UAI/Transient/T208CurveTable/") + Name;
    const FString Path = PackageName + TEXT(".") + Name;
    const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
    const FString Workspace = FPaths::ProjectSavedDir() / TEXT("UeAssetFolders/T208CurveTable") / Name;
    ON_SCOPE_EXIT
    {
        if (UObject* Created = FindObject<UObject>(nullptr, *Path))
        {
            const FWeakObjectPtr Witness(Created);
            TestEqual(TEXT("owned initialized curve is deleted through UE"), ObjectTools::ForceDeleteObjects({Created}, false), 1);
            TestFalse(TEXT("owned initialized curve object is gone"), Witness.IsValid(true, true));
        }
        TestFalse(TEXT("owned initialized curve file is gone"), IFileManager::Get().FileExists(*Filename));
        IFileManager::Get().DeleteDirectory(*Workspace, false, true);
    };
    if (!TestTrue(TEXT("target starts independently absent"), IsCurveTableTargetAbsent(Path))) return false;
    TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
    TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
    FString Error;
    if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error))
    { AddError(Error); return false; }
    const auto Source = MakeShared<FJsonObject>();
    Source->SetStringField(TEXT("kind"), TEXT("inline"));
    Source->SetStringField(TEXT("document"), TEXT(R"HJSON(
schema: "uai.datatable_curvetable.edit/1"
root: {
  type: "/Script/Engine.CurveTable"
  table_curve_mode: simple
  rows: {
    First: {
      properties: {
        Curve: {
          type: curve
          value: {
            interp_mode: Constant
            pre_infinity_extrap: Cycle
            post_infinity_extrap: Linear
            channels: {
              value: {
                default_value: { enabled: true, value: 17 }
                keys: [ { time: 0, value: 73 }, { time: 1, value: 29 } ]
              }
            }
          }
        }
      }
    }
  }
}
)HJSON"));
    const auto Import = MakeShared<FJsonObject>();
    Import->SetStringField(TEXT("target"), Path);
    Import->SetObjectField(TEXT("source"), Source);
    const auto Imported = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"),
        TEXT("t208-curvetable-public-initialization"), Import);
    if (!TestTrue(TEXT("public typed initialization creates an absent CurveTable"), Imported.IsOk()))
    { AddError(Imported.GetErrorJson()); return false; }
    UCurveTable* Reloaded = LoadObject<UCurveTable>(nullptr, *Path);
    if (!TestTrue(TEXT("initialized curve is clean and persistent with native defaults"), Reloaded
        && Reloaded->GetCurveTableMode() == ECurveTableMode::SimpleCurves && Reloaded->GetRowMap().Num() == 1
        && !Reloaded->GetOutermost()->IsDirty() && IFileManager::Get().FileExists(*Filename)
        && Reloaded->AssetImportData != nullptr
        && FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(FSoftObjectPath(Path)).IsValid())) return false;
    const FSimpleCurve* Curve = Reloaded->GetSimpleCurveRowMap().FindRef(TEXT("First"));
    if (!TestTrue(TEXT("full initialized native curve survives reload"), Curve && Curve->GetDefaultValue() == 17
        && Curve->GetKeyInterpMode() == RCIM_Constant && Curve->PreInfinityExtrap == RCCE_Cycle
        && Curve->PostInfinityExtrap == RCCE_Linear && Curve->GetConstRefOfKeys().Num() == 2
        && Curve->GetConstRefOfKeys()[0].Time == 0 && Curve->GetConstRefOfKeys()[0].Value == 73
        && Curve->GetConstRefOfKeys()[1].Time == 1 && Curve->GetConstRefOfKeys()[1].Value == 29)) return false;
    TArray<UeAgentAuthoringState::FReadRequest> Editable;
    if (!UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(Reloaded,
        UeAgentAuthoringState::FEditablePropertyReadOptions(), Editable, Error)) { AddError(Error); return false; }
    if (!TestTrue(TEXT("native CurveTable exposes no editable root UProperties"), Editable.IsEmpty())) return false;
    Reloaded = nullptr;
    const auto Export = MakeShared<FJsonObject>();
    Export->SetStringField(TEXT("target"), Path);
    Export->SetStringField(TEXT("workspace_path"), Workspace);
    Export->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
    const auto Exported = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.export_workspace"),
        TEXT("t208-curvetable-initial-reexport"), Export);
    if (!TestTrue(TEXT("initialized CurveTable publicly reexports"), Exported.IsOk())) { AddError(Exported.GetErrorJson()); return false; }
    TArray<FString> Files;
    IFileManager::Get().FindFilesRecursive(Files, *Workspace, TEXT("*.hjson"), true, false);
    bool bVerified = false;
    for (const FString& File : Files)
    {
        if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
        FString Text;
        if (!FFileHelper::LoadFileToString(Text, *File)) continue;
        auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
        HjsonWorkspace::FHjsonDocumentPtr Document;
        HjsonWorkspace::FWorkspaceError ParseError;
        if (!HjsonWorkspace::TryParseHjsonDocument(Text, File, Resources, Document, ParseError)) { AddError(ParseError.GetCode()); return false; }
        const auto* Rows = Document->GetRoot()->Get(TEXT("rows"));
        if (!Rows) continue;
        const auto* Mode = Document->GetRoot()->Get(TEXT("table_curve_mode"));
        const auto* Schema = Document->GetRoot()->Get(TEXT("row_schema"));
        const auto* SchemaType = Schema ? Schema->Get(TEXT("type")) : nullptr;
        const auto* Row = Rows->Get(TEXT("First"));
        const auto* Properties = Row ? Row->Get(TEXT("properties")) : nullptr;
        const auto* Property = Properties ? Properties->Get(TEXT("Curve")) : nullptr;
        const auto* Type = Property ? Property->Get(TEXT("type")) : nullptr;
        const auto* Value = Property ? Property->Get(TEXT("value")) : nullptr;
        const auto* Channels = Value ? Value->Get(TEXT("channels")) : nullptr;
        const auto* Channel = Channels ? Channels->Get(TEXT("value")) : nullptr;
        const auto* Keys = Channel ? Channel->Get(TEXT("keys")) : nullptr;
        const auto* FirstValue = Keys && Keys->GetItems().Num() == 2 ? Keys->GetItems()[0]->Get(TEXT("value")) : nullptr;
        bVerified = Mode && Mode->GetText() == TEXT("simple") && SchemaType && SchemaType->GetText() == TEXT("SimpleCurve")
            && Rows->GetEntries().Num() == 1 && Type && Type->GetText() == TEXT("curve") && FirstValue
            && FCString::Atod(*FirstValue->GetLexeme()) == 73;
    }
    return TestTrue(TEXT("typed initialized row and native schema independently reexport"), bVerified);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208CurveTableInitializationLifecycle,
    "UAI.Root.T208.CurveTable.InitializationLifecycle", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208CurveTableInitializationLifecycle::RunTest(const FString& Parameters)
{
    using namespace UE::UAI;
    using namespace UE::UAI::DomainAdapters::DataTableCurveTable;
    using namespace UeAgentAuthoringOperationApply;
    using HjsonWorkspace::DomainAuthoring::FRootPropertyApplyOutcome;
    const FString Name = TEXT("CT_CreateLifecycle_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString PackageName = TEXT("/Game/UAI/Transient/T208CurveTable/") + Name;
    const FString Path = PackageName + TEXT(".") + Name;
    const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
    auto IsAbsent = [&]()
    {
        return !FindObject<UObject>(nullptr, *Path) && !IFileManager::Get().FileExists(*Filename)
            && !FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(FSoftObjectPath(Path)).IsValid();
    };
    ON_SCOPE_EXIT
    {
        if (UObject* Remaining = FindObject<UObject>(nullptr, *Path))
            TestEqual(TEXT("remaining owned create fixture deletes through UE"), ObjectTools::ForceDeleteObjects({Remaining}, false), 1);
        TestTrue(TEXT("create lifecycle leaves object, file and registry absent"), IsAbsent());
    };
    if (!TestTrue(TEXT("create lifecycle starts absent"), IsAbsent())) return false;
    TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
    TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
    FString Error;
    if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error))
    { AddError(Error); return false; }
    const auto Source = MakeShared<FJsonObject>();
    Source->SetStringField(TEXT("kind"), TEXT("inline"));
    Source->SetStringField(TEXT("document"),
        TEXT("schema: \"uai.datatable_curvetable.edit/1\"\nroot: { type: \"/Script/Engine.CurveTable\" }\n"));
    const auto Import = MakeShared<FJsonObject>();
    Import->SetStringField(TEXT("target"), Path);
    Import->SetObjectField(TEXT("source"), Source);
    const auto DefaultResult = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"),
        TEXT("t208-curvetable-public-defaults"), Import);
    if (!TestTrue(TEXT("public omitted mode and rows initialize native Empty"), DefaultResult.IsOk()))
    { AddError(DefaultResult.GetErrorJson()); return false; }
    UCurveTable* DefaultTable = LoadObject<UCurveTable>(nullptr, *Path);
    if (!TestTrue(TEXT("public defaults are persisted clean Empty with native import data"), DefaultTable
        && DefaultTable->GetCurveTableMode() == ECurveTableMode::Empty && DefaultTable->GetRowMap().IsEmpty()
        && DefaultTable->AssetImportData != nullptr && !DefaultTable->GetOutermost()->IsDirty()
        && IFileManager::Get().FileExists(*Filename)
        && FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(FSoftObjectPath(Path)).IsValid())) return false;
    const FWeakObjectPtr DefaultWitness(DefaultTable);
    if (!TestEqual(TEXT("default initialized fixture deletes through UE"), ObjectTools::ForceDeleteObjects({DefaultTable}, false), 1)
        || !TestFalse(TEXT("default initialized native object is gone"), DefaultWitness.IsValid(true, true))) return false;
    DefaultTable = nullptr;
    FAssetRegistryModule::TickAssetRegistry(0.15f);
    if (!TestTrue(TEXT("public default fixture returns to independent absence"), IsAbsent())) return false;

    TSharedPtr<FJsonObject> Desired;
    {
        TStrongObjectPtr<UCurveTable> Scratch(NewObject<UCurveTable>(GetTransientPackage()));
        SetRichLifecycleCurve(*Scratch, 2);
        if (!CaptureCurveTableRows(*Scratch, Desired, Error)) { AddError(Error); return false; }
        Scratch->MarkAsGarbage();
    }
    const auto Default = Desired->GetObjectField(TEXT("rows"))->GetObjectField(TEXT("Mapping"))->GetObjectField(TEXT("properties"))
        ->GetObjectField(TEXT("Curve"))->GetObjectField(TEXT("value"))->GetObjectField(TEXT("channels"))
        ->GetObjectField(TEXT("value"))->GetObjectField(TEXT("default_value"));
    Default->SetStringField(TEXT("enabled"), TEXT("invalid"));
    FRootPropertyApplyOutcome Invalid;
    if (!TestFalse(TEXT("invalid create curve rejects before target creation"),
            ApplyCurveTableAuthoring(Path, TEXT("t208-curve-create-invalid"), Desired, Invalid, nullptr, {}, true))
        || !TestEqual(TEXT("invalid create retains typed cause"), Invalid.Diagnostic.Code, FString(TEXT("curve_field_type_mismatch")))
        || !TestTrue(TEXT("invalid create preserves original absence"), IsAbsent())) return false;
    Default->SetBoolField(TEXT("enabled"), true);

    class FRejectCreatedStage final : public IDurablePackageStager
    {
    public:
        FRejectCreatedStage(FString InPath, FString InFilename)
            : Path(MoveTemp(InPath)), Filename(MoveTemp(InFilename)), Inner(CreateDefaultDurablePackageStager()) {}
        bool BuildArtifactInventory(const TArray<FPackageDurablePlan>& Plans,
            TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact>& Artifacts, FOperationFailure& Failure) override
        { return Inner->BuildArtifactInventory(Plans, Artifacts, Failure); }
        bool StagePackages(const TArray<FPackageDurablePlan>& Plans,
            TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact>& Artifacts, FOperationFailure& Failure) override
        {
            if (!Inner->StagePackages(Plans, Artifacts, Failure)) return false;
            const UCurveTable* Actual = FindObject<UCurveTable>(nullptr, *Path);
            if (!Actual) return true;
            bObserved = HasRichLifecycleCurve(Actual, 2) && Actual->GetOutermost()->IsDirty()
                && !IFileManager::Get().FileExists(*Filename)
                && FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(FSoftObjectPath(Path)).IsValid();
            bool bAnyStaged = false;
            for (const auto& Artifact : Artifacts)
            {
                bObserved = bObserved && (IFileManager::Get().FileExists(*Artifact.StagingPath) == Artifact.bStagedExists);
                bAnyStaged |= Artifact.bStagedExists;
            }
            bObserved = bObserved && bAnyStaged;
            Failure.Diagnostic.Code = TEXT("t208_curve_create_stage_rejected");
            Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code;
            return false;
        }
        bool bObserved = false;
    private:
        FString Path, Filename;
        TSharedRef<IDurablePackageStager> Inner;
    };
    const auto Stager = MakeShared<FRejectCreatedStage>(Path, Filename);
    FBackendOptions Options;
    Options.PackageStager = Stager;
    FRootPropertyApplyOutcome Early;
    if (!TestFalse(TEXT("created Rich real staging can reject"),
            ApplyCurveTableAuthoring(Path, TEXT("t208-curve-create-early"), Desired, Early, &Options, {}, true))
        || !TestTrue(TEXT("early failure observes native created content and staging before publication"), Stager->bObserved)
        || !TestTrue(TEXT("early create rollback restores object disk registry absence"), IsAbsent()))
    { AddError(Early.Diagnostic.Code + TEXT(":") + Early.Diagnostic.PublicMessage); return false; }
    bool bLateObserved = false;
    FRootPropertyApplyOutcome Late;
    Late.FinalReadback = [&](FOperationFailure& Failure)
    {
        const UCurveTable* Actual = LoadObject<UCurveTable>(nullptr, *Path);
        bLateObserved = HasRichLifecycleCurve(Actual, 2) && !Actual->GetOutermost()->IsDirty()
            && IFileManager::Get().FileExists(*Filename)
            && FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(FSoftObjectPath(Path)).IsValid();
        Failure.Diagnostic.Code = TEXT("t208_curve_create_late_rejected");
        Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code;
        return false;
    };
    if (!TestFalse(TEXT("created Rich persisted final readback can reject"),
            ApplyCurveTableAuthoring(Path, TEXT("t208-curve-create-late"), Desired, Late, nullptr, {}, true))
        || !TestTrue(TEXT("late failure observes clean persisted full Rich and registry"), bLateObserved)
        || !TestTrue(TEXT("late create rollback restores independently observed absence"), IsAbsent()))
    { AddError(Late.Diagnostic.Code + TEXT(":") + Late.Diagnostic.PublicMessage); return false; }
    FAssetRegistryModule::TickAssetRegistry(0.15f);
    return TestTrue(TEXT("restored absence survives native registry tick"), IsAbsent());
}

#include "Misc/AutomationTest.h"
#include "Core/Contract/UeAgentDiagnostic.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "CommandEdge/UeAgentCommandProtocol.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringApplyProvider.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringRegistration.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Misc/Paths.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "ObjectTools.h"
#include "NiagaraScript.h"
#include "NiagaraScriptFactoryNew.h"
#include "NiagaraValidationRules.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/LinkerInstancingContext.h"
#include "UObject/Linker.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringReflectionDescriptors.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "WorldPartition/HLOD/HLODLayer.h"
#include "WorldPartition/HLOD/HLODBuilder.h"

namespace
{
	using namespace UE::UAI;
	class FNeverCancelled final : public Contract::IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override { return false; }
	};

	CommandEdge::FUeAgentCommandResult SubmitPublic(
		const TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch>& Dispatch,
		const FString& CommandId, const FString& RequestId,
		const TSharedRef<FJsonObject>& Params)
	{
		const auto* Command = Registry.IsValid() ? Registry->FindExact(CommandId) : nullptr;
		FString ParamsJson, Error;
		if (!Command || !Dispatch.IsValid()
			|| !CommandContract::Json::TryCanonicalize(Params, ParamsJson, Error))
			return CommandEdge::FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t208-hlod"), RequestId, CommandId, FString(),
				TEXT("t208_hlod_public_command_unavailable"), Error);
		const FString Envelope = FString::Printf(
			TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,")
			TEXT("\"message_kind\":\"command_request\",\"correlation_id\":\"corr-t208-hlod\",")
			TEXT("\"request_id\":\"%s\",\"command_id\":\"%s\",")
			TEXT("\"command_binding_hash\":\"%s\",\"params\":%s}"),
			*RequestId, *CommandId, *Command->GetContractHash(), *ParamsJson);
		FTCHARToUTF8 Encoded(*Envelope);
		TArray<uint8> Bytes;
		Bytes.Append(reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
		CommandEdge::FUeAgentCommandProtocolDiagnostic Diagnostic;
		TOptional<CommandEdge::FUeAgentCommandRequest> Request;
		if (!CommandEdge::FUeAgentCommandProtocolCodec().DecodeRequest(
			Bytes, Request, Diagnostic) || !Request.IsSet())
			return CommandEdge::FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t208-hlod"), RequestId, CommandId, Command->GetContractHash(),
				TEXT("t208_hlod_public_request_invalid"), Diagnostic.Message);
		return Dispatch->Submit(Request.GetValue(),
			Contract::FUeAgentCallerContext(TEXT("t208-hlod"),
				{TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")},
				TEXT("corr-t208-hlod")), MakeShared<FNeverCancelled>());
	}

	TSharedPtr<FJsonValue> Property(const TCHAR* Name, TSharedPtr<FJsonValue> Value)
	{
		auto Descriptor = MakeShared<FJsonObject>();
		Descriptor->SetStringField(TEXT("property_path"), Name);
		Descriptor->SetField(TEXT("value"), MoveTemp(Value));
		return MakeShared<FJsonValueObject>(Descriptor);
	}

	bool Apply(UObject& Object, const TArray<TSharedPtr<FJsonValue>>& Properties,
		UeAgentAuthoringObjectApply::FFailure& Failure)
	{
		UeAgentAuthoringObjectApply::FOptions Options;
		UeAgentAuthoringObjectApply::FPreparedApply Prepared;
		if (!UeAgentAuthoringObjectApply::PrepareObject(
				&Object, Properties, Options, Prepared, Failure))
		{
			return false;
		}
		UeAgentAuthoringObjectApply::FApplyResult Result;
		const bool bApplied = UeAgentAuthoringObjectApply::ApplyPrepared(Prepared, Result)
			&& UeAgentAuthoringObjectApply::VerifyPrepared(Prepared, Result);
		if (!bApplied) { Failure.Diagnostic.Code = Result.Diagnostic.Code; Failure.Diagnostic.InternalContext = Result.Diagnostic.InternalContext; }
		return bApplied;
	}

	TSharedPtr<FJsonObject> FindRow(const TSharedPtr<FJsonObject>& Object, const FString& Name)
	{
		if (!Object) return nullptr;
		for (const auto& Pair : Object->Values) {
			if (Pair.Value->Type != EJson::Object) continue;
			const auto Child = Pair.Value->AsObject();
			if (Pair.Key == Name && Child->HasField(TEXT("value"))) return Child;
			if (auto Found = FindRow(Child, Name)) return Found;
		}
		return nullptr;
	}

	TMap<FString, FString> NativeState(const UObject& Object, const TSet<FName>& Excluded = {})
	{
		TMap<FString, FString> Result;
		for (TFieldIterator<FProperty> It(Object.GetClass()); It; ++It) {
			if (Excluded.Contains(It->GetFName()) || It->HasAnyPropertyFlags(CPF_Transient)) continue;
			FString Value;
			It->ExportTextItem_Direct(Value, It->ContainerPtrToValuePtr<void>(&Object), nullptr,
				const_cast<UObject*>(&Object), PPF_None);
			Result.Add(It->GetName(), Value);
		}
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUeAgentT208MultipleExistingOwnedTest,
    "UAI.Root.T208.HLOD.MultipleExistingOwnedRegression",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208MultipleExistingOwnedTest::RunTest(const FString&)
{
    using namespace UeAgentAuthoringObjectApply;
    auto* Factory = NewObject<UNiagaraModuleScriptFactory>();
    TStrongObjectPtr<UNiagaraScript> Script(Cast<UNiagaraScript>(Factory->FactoryCreateNew(
        UNiagaraScript::StaticClass(), GetTransientPackage(),
        MakeUniqueObjectName(GetTransientPackage(), UNiagaraScript::StaticClass(), TEXT("T208OwnedRules")),
        RF_Transactional, nullptr, GWarn)));
    if (!TestNotNull(TEXT("real native Script factory creates the owned-array consumer"), Script.Get())) return false;
    auto* First = NewObject<UNiagaraValidationRule_NoEvents>(Script.Get(), NAME_None, RF_Transactional);
    auto* Second = NewObject<UNiagaraValidationRule_NoEvents>(Script.Get(), NAME_None, RF_Transactional);
    First->Severity = ENiagaraValidationSeverity::Warning;
    Second->Severity = ENiagaraValidationSeverity::Error;
    First->Platforms.QualityLevelMask = 5;
    Second->Platforms.QualityLevelMask = 9;
    Script->ValidationRules = {First, Second};
    const auto RootBefore = NativeState(*Script);
    const auto FirstBefore = NativeState(*First);
    const auto SecondBefore = NativeState(*Second);
    const auto FirstControl = NativeState(*First, {TEXT("Severity")});
    const auto SecondControl = NativeState(*Second, {TEXT("Severity")});
    auto Owned = [&](UObject* Object, const TCHAR* Severity) -> TSharedPtr<FJsonValue> {
        auto Body = MakeShared<FJsonObject>();
        Body->SetStringField(TEXT("owner_relative_path"), Object->GetPathName(Script.Get()));
        Body->SetStringField(TEXT("class_path"), Object->GetClass()->GetPathName());
        Body->SetArrayField(TEXT("properties"), {Property(TEXT("Severity"), MakeShared<FJsonValueString>(Severity))});
        auto Value = MakeShared<FJsonObject>();
        Value->SetObjectField(TEXT("owned_object"), Body);
        return MakeShared<FJsonValueObject>(Value);
    };
    const TArray<TSharedPtr<FJsonValue>> Edits = {Property(TEXT("ValidationRules"),
        MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{Owned(First, TEXT("Error")), Owned(Second, TEXT("Warning"))}))};
    FOptions Options;
    Options.bCompactValues = true;
    Options.bAllowDomainAuthorizedOwners = true;
    FPreparedApply Prepared;
    FFailure Failure;
    if (!TestTrue(TEXT("one array entry prepares two independent existing-object edits"),
        PrepareObject(Script.Get(), Edits, Options, Prepared, Failure))) {
        AddError(Failure.Diagnostic.Code + TEXT(":") + Failure.Diagnostic.PublicMessage); return false;
    }
    TestTrue(TEXT("prepare preserves both native objects"), NativeState(*First).OrderIndependentCompareEqual(FirstBefore)
        && NativeState(*Second).OrderIndependentCompareEqual(SecondBefore));
    FApplyResult Result;
    if (!TestTrue(TEXT("both existing-owned plans apply and verify"),
        ApplyPrepared(Prepared, Result) && VerifyPrepared(Prepared, Result))) {
        AddError(Result.Diagnostic.Code + TEXT(":") + Result.Diagnostic.PublicMessage); return false;
    }
    TestTrue(TEXT("both distinct edits survive in their original ordered slots"),
        Script->ValidationRules.Num() == 2 && Script->ValidationRules[0] == First && Script->ValidationRules[1] == Second
        && First->Severity == ENiagaraValidationSeverity::Error && Second->Severity == ENiagaraValidationSeverity::Warning);
    TestTrue(TEXT("every unrelated field of each existing object stays unchanged"),
        NativeState(*First, {TEXT("Severity")}).OrderIndependentCompareEqual(FirstControl)
        && NativeState(*Second, {TEXT("Severity")}).OrderIndependentCompareEqual(SecondControl));
    TestTrue(TEXT("one rollback restores both independent owned plans"), RollbackPrepared(Prepared, Result));
    TestTrue(TEXT("rollback restores complete root and both object states"),
        NativeState(*Script).OrderIndependentCompareEqual(RootBefore)
        && NativeState(*First).OrderIndependentCompareEqual(FirstBefore)
        && NativeState(*Second).OrderIndependentCompareEqual(SecondBefore));
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT208HlodExistingInlineAuthoringTest,
	"UAI.Root.T208.HLOD.ExistingInlineAuthoring",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208HlodExistingInlineAuthoringTest::RunTest(const FString&)
{
	const FString PackageName = TEXT("/Game/UAI/Transient/T208HLOD/Inline_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString ObjectPath = PackageName + TEXT(".")
		+ FPackageName::GetLongPackageAssetName(PackageName);
	const FString Filename = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	const FString Workspace = FPaths::ProjectSavedDir()
		/ TEXT("UeAssetFolders/T208HLOD")
		/ FPackageName::GetLongPackageAssetName(PackageName);
	ON_SCOPE_EXIT
	{
		if (UObject* Owned = FindObject<UObject>(nullptr, *ObjectPath))
		{
			Owned->GetOutermost()->SetDirtyFlag(false);
			TGuardValue<bool> Guard(GIsRunningUnattendedScript, true);
			if (ObjectTools::ForceDeleteObjects({Owned}, false) != 1)
				AddError(TEXT("owned HLOD fixture deletion failed"));
		}
		if (FPaths::FileExists(Filename))
			IFileManager::Get().Delete(*Filename, false, true);
		if (!HasAnyErrors()) IFileManager::Get().DeleteDirectory(*Workspace, false, true);
	};
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	UE::UAI::Contract::FDiagnostic CompositionError;
	if (!TestTrue(TEXT("public command surface composes for HLOD"),
		Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(
			Registry, Dispatch, CompositionError))) { AddError(CompositionError.ToPublicDetail()); return false; }
	auto CreateTarget = MakeShared<FJsonObject>();
	CreateTarget->SetStringField(TEXT("asset_path"), PackageName);
	CreateTarget->SetStringField(TEXT("asset_kind"), TEXT("hlod_layer"));
	auto CreateInput = MakeShared<FJsonObject>();
	CreateInput->SetStringField(TEXT("operation"), TEXT("create"));
	CreateInput->SetObjectField(TEXT("target"), CreateTarget);
	CreateInput->SetArrayField(TEXT("properties"), {
		Property(TEXT("LayerType"), MakeShared<FJsonValueString>(TEXT("MeshMerge"))),
		Property(TEXT("CellSize"), MakeShared<FJsonValueNumber>(512.0)),
		Property(TEXT("LoadingRange"), MakeShared<FJsonValueNumber>(12345.0))});
	auto CreateRoot = MakeShared<FJsonObject>();
	CreateRoot->SetStringField(TEXT("type"), TEXT("/Script/Engine.HLODLayer"));
	CreateRoot->SetObjectField(TEXT("document"), CreateInput);
	auto CreateDocument = MakeShared<FJsonObject>();
	CreateDocument->SetStringField(TEXT("schema"), TEXT("uai.world_hlod_layer.edit/1"));
	CreateDocument->SetObjectField(TEXT("root"), CreateRoot);
	auto CreateSource = MakeShared<FJsonObject>();
	CreateSource->SetStringField(TEXT("kind"), TEXT("inline"));
	CreateSource->SetStringField(TEXT("document"), FUeAgentStructuredHjsonFormatter::WriteObject(CreateDocument));
	auto CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("target"), ObjectPath);
	CreateParams->SetObjectField(TEXT("source"), CreateSource);
	const FString ValidCreateText = CreateSource->GetStringField(TEXT("document"));
	CreateInput->SetArrayField(TEXT("properties"), {Property(TEXT("NoSuchHLODProperty"), MakeShared<FJsonValueNumber>(1))});
	CreateSource->SetStringField(TEXT("document"), FUeAgentStructuredHjsonFormatter::WriteObject(CreateDocument));
	auto RejectedCreate = SubmitPublic(Registry, Dispatch, TEXT("content_graph.import_workspace"),
		TEXT("t208-hlod-create-rejected"), CreateParams);
	TestFalse(TEXT("public invalid create rejects"), RejectedCreate.IsOk());
	bool bPresent = true;
	TestTrue(TEXT("public failed create leaves target and disk absent"),
		HjsonWorkspace::DomainAuthoring::TryInspectObjectTargetPresence(ObjectPath, bPresent)
		&& !bPresent && !FPaths::FileExists(Filename));
	CreateSource->SetStringField(TEXT("document"), ValidCreateText);
    {
        bool bObservedCreatedDisk = false;
        const auto CreatedHandle = FCoreUObjectDelegates::OnAssetLoaded.AddLambda([&](UObject* Object) {
            auto* Created = Cast<UHLODLayer>(Object);
            if (!Created || Created->GetPathName() != ObjectPath || bObservedCreatedDisk
                || Created->GetCellSize() != 512 || Created->GetOutermost()->IsDirty()
                || !FPaths::FileExists(Filename)) return;
            bObservedCreatedDisk = true;
            FindFProperty<FIntProperty>(UHLODLayer::StaticClass(), TEXT("CellSize"))->SetPropertyValue_InContainer(Created, 513);
        });
        ON_SCOPE_EXIT { FCoreUObjectDelegates::OnAssetLoaded.Remove(CreatedHandle); };
        RejectedCreate = SubmitPublic(Registry, Dispatch, TEXT("content_graph.import_workspace"),
            TEXT("t208-hlod-create-late-rejected"), CreateParams);
        TestFalse(TEXT("public create rejects a saved and reloaded discrepancy"), RejectedCreate.IsOk());
        TestTrue(TEXT("late create reaches independent native loaded disk state"), bObservedCreatedDisk);
        bPresent = true;
        if (!TestTrue(TEXT("late create recovery restores original memory and disk absence"),
            HjsonWorkspace::DomainAuthoring::TryInspectObjectTargetPresence(ObjectPath, bPresent)
            && !bPresent && !FPaths::FileExists(Filename))) return false;
    }
	auto PublicResult = SubmitPublic(Registry, Dispatch, TEXT("content_graph.import_workspace"),
		TEXT("t208-hlod-create"), CreateParams);
	if (!TestTrue(TEXT("public HJSON creates absent HLOD through Reflection"), PublicResult.IsOk())) {
		AddError(PublicResult.GetErrorJson()); return false;
	}
	UHLODLayer* Layer = LoadObject<UHLODLayer>(nullptr, *ObjectPath);
	if (!TestTrue(TEXT("created HLOD independently reloads native root and derived settings"),
		Layer && Layer->GetCellSize() == 512 && Layer->GetLayerType() == EHLODLayerType::MeshMerge
		&& Layer->GetHLODBuilderSettings() && !Layer->GetOutermost()->IsDirty())) return false;
	UPackage* Package = Layer->GetOutermost();
	FProperty* LayerTypeProperty = FindFProperty<FProperty>(
		UHLODLayer::StaticClass(), UHLODLayer::GetLayerTypePropertyName());
	if (!TestNotNull(TEXT("native LayerType property"), LayerTypeProperty)) return false;

	const UObject* MergeSettings = Layer->GetHLODBuilderSettings();
	if (!TestNotNull(TEXT("native MeshMerge settings exist"), MergeSettings)) return false;

	UeAgentAuthoringObjectApply::FFailure Failure;
	if (!TestTrue(TEXT("common property apply switches LayerType through native notification"),
		Apply(*Layer, {Property(TEXT("LayerType"),
			MakeShared<FJsonValueString>(TEXT("Instancing")))}, Failure)))
	{
		AddError(Failure.Diagnostic.Code + TEXT(":") + Failure.Diagnostic.InternalContext);
		return false;
	}
	UObject* InstancingSettings =
		const_cast<UHLODBuilderSettings*>(Layer->GetHLODBuilderSettings());
	if (!TestTrue(TEXT("native notification replaces settings with the derived type"),
		InstancingSettings && InstancingSettings != MergeSettings
			&& InstancingSettings->GetClass()->GetName().Contains(TEXT("Instancing"))))
	{
		return false;
	}

	FProperty* SettingsProperty = FindFProperty<FProperty>(
		UHLODLayer::StaticClass(), UHLODLayer::GetHLODBuilderSettingsPropertyName());
	const void* SettingsValue = SettingsProperty
		? SettingsProperty->ContainerPtrToValuePtr<void>(Layer)
		: nullptr;
	TestTrue(TEXT("existing EditInline Export object is recursively editable"),
		UeAgentAuthoringReflection::IsExistingInlineObjectProperty(
			SettingsProperty, SettingsValue, Layer));
	TestFalse(TEXT("settings pointer has no create or replacement eligibility"),
		UeAgentAuthoringReflection::IsOwnedObjectProperty(SettingsProperty));

	auto Nested = MakeShared<FJsonObject>();
	Nested->SetArrayField(TEXT("properties"), {
		Property(TEXT("bDisallowNanite"), MakeShared<FJsonValueBoolean>(true))});
	auto Owned = MakeShared<FJsonObject>();
	Owned->SetObjectField(TEXT("owned_object"), Nested);
	Failure = {};
	if (!TestTrue(TEXT("common recursive apply edits the existing settings descendant"),
		Apply(*Layer, {Property(TEXT("HLODBuilderSettings"),
			MakeShared<FJsonValueObject>(Owned))}, Failure)))
	{
		AddError(Failure.Diagnostic.Code + TEXT(":") + Failure.Diagnostic.InternalContext);
		return false;
	}
	const FBoolProperty* DisallowNanite = FindFProperty<FBoolProperty>(
		InstancingSettings->GetClass(), TEXT("bDisallowNanite"));
	TestTrue(TEXT("non-default builder setting reaches the native instance"),
		DisallowNanite
			&& DisallowNanite->GetPropertyValue_InContainer(InstancingSettings));

	auto NullPointer = MakeShared<FJsonObject>();
	NullPointer->SetField(TEXT("path"), MakeShared<FJsonValueNull>());
	UeAgentAuthoringObjectApply::FOptions Options;
	UeAgentAuthoringObjectApply::FPreparedApply Rejected;
	Failure = {};
	TestFalse(TEXT("read-only inline pointer cannot be cleared or replaced"),
		UeAgentAuthoringObjectApply::PrepareObject(
			Layer, {Property(TEXT("HLODBuilderSettings"),
				MakeShared<FJsonValueObject>(NullPointer))},
			Options, Rejected, Failure));
	TestEqual(TEXT("pointer rejection keeps the common read-only contract"),
		Failure.Diagnostic.Code, FString(TEXT("read_only_or_non_editable")));
	TestTrue(TEXT("rejected replacement preserves the derived settings object"),
		Layer->GetHLODBuilderSettings() == InstancingSettings);
	const auto BeforeRejectedRoot = NativeState(*Layer);
	const auto BeforeRejectedSettings = NativeState(*InstancingSettings);
	for (int32 Case = 0; Case < 3; ++Case) {
		auto InvalidInline = MakeShared<FJsonObject>();
		InvalidInline->SetArrayField(TEXT("properties"), {});
		if (Case == 0) InvalidInline->SetStringField(TEXT("owner_relative_path"), TEXT(""));
		if (Case == 1) InvalidInline->SetStringField(TEXT("class_path"), MergeSettings->GetClass()->GetPathName());
		if (Case == 2) {
			InvalidInline->SetStringField(TEXT("owner_relative_path"), TEXT(""));
			InvalidInline->SetStringField(TEXT("class_path"), InstancingSettings->GetClass()->GetPathName());
		}
		auto InvalidValue = MakeShared<FJsonObject>();
		InvalidValue->SetObjectField(TEXT("owned_object"), InvalidInline);
		TestFalse(FString::Printf(TEXT("readonly inline create/reclass payload %d rejected before mutation"), Case),
			UeAgentAuthoringObjectApply::PrepareObject(Layer,
				{Property(TEXT("HLODBuilderSettings"), MakeShared<FJsonValueObject>(InvalidValue))},
				Options, Rejected, Failure));
		TestTrue(TEXT("rejected payload preserves all native root/settings state"),
			Layer->GetHLODBuilderSettings() == InstancingSettings
			&& NativeState(*Layer).OrderIndependentCompareEqual(BeforeRejectedRoot)
			&& NativeState(*InstancingSettings).OrderIndependentCompareEqual(BeforeRejectedSettings));
	}

	FAssetRegistryModule::AssetCreated(Layer);
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	if (!TestTrue(TEXT("native HLOD baseline persists"),
		UPackage::SavePackage(Package, Layer, *Filename, SaveArgs))) return false;
	Package->SetDirtyFlag(false);
	UE::UAI::Contract::FDiagnostic RegistrationError;
	if (!TestTrue(TEXT("common Reflection authoring contracts register"),
		UeAgentReflectionAuthoring::RegisterAuthoring(RegistrationError)))
	{
		AddError(RegistrationError.ToPublicDetail());
		return false;
	}

	UeAgentReflectionAuthoring::FAssetApplyRequest Persist;
	Persist.TypeId = TEXT("hlod_layer");
	Persist.Operation = UeAgentApplyInputContract::EOperation::Update;
	Persist.AssetPath = PackageName;
	Persist.Properties = {
		Property(TEXT("CellSize"), MakeShared<FJsonValueNumber>(512.0))};
	Persist.bSave = true;
	UeAgentReflectionAuthoring::FAssetApplyResult Persisted;
	Layer = nullptr;
	if (!TestTrue(TEXT("common Reflection transaction saves and reloads HLOD"),
		UeAgentReflectionAuthoring::ApplyAsset(
			Persist, Persisted, TEXT("t208-hlod-persist"))))
	{
		AddError(Persisted.Diagnostic.Code + TEXT(":") + Persisted.Diagnostic.PublicMessage);
		return false;
	}
	Layer = LoadObject<UHLODLayer>(nullptr, *ObjectPath);
	if (!TestTrue(TEXT("independent HLOD reload contains the root edit"),
		Layer && Layer->GetCellSize() == 512 && !Layer->GetOutermost()->IsDirty()))
		return false;
	const FBoolProperty* PersistedNanite = FindFProperty<FBoolProperty>(
		Layer->GetHLODBuilderSettings()->GetClass(), TEXT("bDisallowNanite"));
	TestTrue(TEXT("independent reload preserves the nested builder edit"),
		PersistedNanite && PersistedNanite->GetPropertyValue_InContainer(
			Layer->GetHLODBuilderSettings()));

	auto Export = MakeShared<FJsonObject>();
	Export->SetStringField(TEXT("target"), ObjectPath);
	Export->SetStringField(TEXT("workspace_path"), Workspace / TEXT("edit"));
	Export->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
	PublicResult = SubmitPublic(Registry, Dispatch,
		TEXT("content_graph.export_workspace"), TEXT("t208-hlod-export"), Export);
	if (!TestTrue(TEXT("public HLOD semantic workspace exports"), PublicResult.IsOk()))
	{
		AddError(PublicResult.GetErrorJson());
		return false;
	}
	const auto NonTargetBefore = NativeState(*Layer,
        {TEXT("LayerType"), TEXT("HLODBuilderSettings"), TEXT("CellSize")});
	const auto NonTargetSettingsBefore = NativeState(*Layer->GetHLODBuilderSettings(), {TEXT("bDisallowNanite")});
    TSharedPtr<FJsonValue> InstancingInput;
    auto EditWorkspace = [&](const FString& Directory, bool bReturnToInstancing, int32 DesiredCell = 0) -> bool {
        TArray<FString> Files;
        IFileManager::Get().FindFilesRecursive(Files, *Directory, TEXT("*.hjson"), true, false);
        bool bTypeEdited = false, bSettingsEdited = false, bCellEdited = false;
        for (const FString& File : Files) {
            if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
            FString Text, Error;
            TSharedPtr<FJsonObject> Root;
            if (!FFileHelper::LoadFileToString(Text, *File)
                || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Root, Error)) {
                AddError(Error); return false;
            }
            bool bChanged = false;
            if (auto Row = FindRow(Root, TEXT("LayerType"))) {
                Row->SetStringField(TEXT("value"), bReturnToInstancing ? TEXT("Instancing") : TEXT("MeshMerge"));
                bTypeEdited = bChanged = true;
            }
            if (auto Row = FindRow(Root, TEXT("HLODBuilderSettings"))) {
                if (!bReturnToInstancing) {
                    InstancingInput = UeAgentAuthoringState::CloneCanonicalValue(Row->TryGetField(TEXT("value")));
                    const auto OwnedValue = InstancingInput->AsObject()->GetObjectField(TEXT("owned_object"));
                    // Type and object identity are derived by the root notification.
                    OwnedValue->RemoveField(TEXT("class_path"));
                    OwnedValue->RemoveField(TEXT("owner_relative_path"));
                    const auto Nanite = FindRow(OwnedValue, TEXT("bDisallowNanite"));
                    if (!Nanite) { AddError(TEXT("ordinary settings field missing")); return false; }
                    Nanite->SetBoolField(TEXT("value"), false);
                } else {
                    Row->SetField(TEXT("value"), InstancingInput);
                    bSettingsEdited = bChanged = true;
                }
            }
            if (bReturnToInstancing || DesiredCell) if (auto Row = FindRow(Root, TEXT("CellSize"))) {
                Row->SetNumberField(TEXT("value"), DesiredCell ? DesiredCell : 640);
                bCellEdited = bChanged = true;
            }
            if (bChanged && (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Root.ToSharedRef(), Text, Error)
                || !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))) {
                AddError(Error); return false;
            }
        }
        return bTypeEdited && (bReturnToInstancing ? bSettingsEdited && bCellEdited : InstancingInput.IsValid())
            && (!DesiredCell || bCellEdited);
    };
    if (!TestTrue(TEXT("ordinary HJSON can author native root type"),
        EditWorkspace(Workspace / TEXT("edit"), false))) return false;
	auto Source = MakeShared<FJsonObject>();
	Source->SetStringField(TEXT("kind"), TEXT("directory"));
	Source->SetStringField(TEXT("path"), Workspace / TEXT("edit"));
	auto Import = MakeShared<FJsonObject>();
	Import->SetStringField(TEXT("target"), ObjectPath);
	Import->SetObjectField(TEXT("source"), Source);
	Layer = nullptr;
	PublicResult = SubmitPublic(Registry, Dispatch,
		TEXT("content_graph.import_workspace"), TEXT("t208-hlod-import"), Import);
	if (!TestTrue(TEXT("public HLOD semantic workspace imports"), PublicResult.IsOk()))
	{
		AddError(PublicResult.GetErrorJson());
		return false;
	}
    Layer = LoadObject<UHLODLayer>(nullptr, *ObjectPath);
    if (!TestTrue(TEXT("public type change derives and persists MeshMerge settings"),
        Layer && Layer->GetLayerType() == EHLODLayerType::MeshMerge
        && Layer->GetHLODBuilderSettings()->GetClass()->GetName().Contains(TEXT("MeshMerge"))
        && !Layer->GetOutermost()->IsDirty())) return false;
    Export->SetStringField(TEXT("workspace_path"), Workspace / TEXT("switch-back"));
    PublicResult = SubmitPublic(Registry, Dispatch, TEXT("content_graph.export_workspace"),
        TEXT("t208-hlod-export-switch-back"), Export);
    if (!TestTrue(TEXT("public derived state reexports"), PublicResult.IsOk())) {
        AddError(PublicResult.GetErrorJson()); return false;
    }
    if (!TestTrue(TEXT("same public document changes type, root and new derived settings"),
        EditWorkspace(Workspace / TEXT("switch-back"), true))) return false;
    Source->SetStringField(TEXT("path"), Workspace / TEXT("switch-back"));
    // These proposals also change LayerType. Native descendant validation must
    // reject them against the derived class without leaking the preview edits.
    const auto BeforePublicRejectedRoot = NativeState(*Layer);
    const auto BeforePublicRejectedSettings = NativeState(*Layer->GetHLODBuilderSettings());
    TArray<uint8> BeforeRejectedDisk;
    if (!TestTrue(TEXT("capture rejection disk baseline"), FFileHelper::LoadFileToArray(BeforeRejectedDisk, *Filename))) return false;
    TArray<FString> RejectionFiles;
    IFileManager::Get().FindFilesRecursive(RejectionFiles, *(Workspace / TEXT("switch-back")), TEXT("*.hjson"), true, false);
    bool bCheckedRejections = false;
    for (const FString& File : RejectionFiles) {
        if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
        FString Original, Error;
        TSharedPtr<FJsonObject> Document;
        if (!FFileHelper::LoadFileToString(Original, *File)
            || !FUeAgentStructuredHjsonParser::ParseObject(Original, File, Document, Error)) return false;
        if (!FindRow(Document, TEXT("HLODBuilderSettings"))) continue;
        for (int32 Case = 0; Case < 4; ++Case) {
            FUeAgentStructuredHjsonParser::ParseObject(Original, File, Document, Error);
            const auto Value = FindRow(Document, TEXT("HLODBuilderSettings"))->GetObjectField(TEXT("value"));
            const auto Body = Value->GetObjectField(TEXT("owned_object"));
            if (Case == 0) {
                auto Category = MakeShared<FJsonObject>();
                Category->SetObjectField(TEXT("NoSuchSetting"),
                    UeAgentAuthoringState::CloneCanonicalValue(MakeShared<FJsonValueObject>(FindRow(Body, TEXT("bDisallowNanite"))))->AsObject());
                Body->GetObjectField(TEXT("properties"))->SetObjectField(TEXT("T208Unknown"), Category);
            } else if (Case == 1) {
                FindRow(Body, TEXT("bDisallowNanite"))->SetStringField(TEXT("value"), TEXT("invalid_boolean"));
            } else if (Case == 2) {
                Body->SetStringField(TEXT("class_path"), TEXT("/Script/Engine.HLODBuilderSettings"));
            } else {
                Body->SetStringField(TEXT("owner_relative_path"), TEXT(""));
            }
            FString Malformed;
            if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Document.ToSharedRef(), Malformed, Error)
                || !FFileHelper::SaveStringToFile(Malformed, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
            PublicResult = SubmitPublic(Registry, Dispatch, TEXT("content_graph.import_workspace"),
                FString::Printf(TEXT("t208-hlod-public-reject-%d"), Case), Import);
            TestFalse(FString::Printf(TEXT("public native descendant/pointer rejection %d"), Case), PublicResult.IsOk());
            Layer = LoadObject<UHLODLayer>(nullptr, *ObjectPath);
            TArray<uint8> AfterRejectedDisk;
            TestTrue(TEXT("rejected public proposal preserves full memory, identity, dirty state and disk"),
                Layer && !Layer->GetOutermost()->IsDirty()
                && NativeState(*Layer).OrderIndependentCompareEqual(BeforePublicRejectedRoot)
                && NativeState(*Layer->GetHLODBuilderSettings()).OrderIndependentCompareEqual(BeforePublicRejectedSettings)
                && FFileHelper::LoadFileToArray(AfterRejectedDisk, *Filename) && AfterRejectedDisk == BeforeRejectedDisk);
        }
        FFileHelper::SaveStringToFile(Original, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
        bCheckedRejections = true;
    }
    if (!TestTrue(TEXT("public rejection variants reached the settings document"), bCheckedRejections)) return false;
    Layer = nullptr;
    PublicResult = SubmitPublic(Registry, Dispatch, TEXT("content_graph.import_workspace"),
        TEXT("t208-hlod-import-switch-back"), Import);
    if (!TestTrue(TEXT("public simultaneous type and descendant edit imports"), PublicResult.IsOk())) {
        AddError(PublicResult.GetErrorJson()); return false;
    }
	FAssetRegistryModule::GetRegistry().ScanFilesSynchronous({Filename}, true);
	UPackage* ReloadedPackage = LoadPackage(nullptr, *PackageName, LOAD_None);
	Layer = ReloadedPackage ? FindObject<UHLODLayer>(
		ReloadedPackage, *FPackageName::GetLongPackageAssetName(PackageName)) : nullptr;
	if (!TestNotNull(TEXT("public import independently reloads the persistent HLOD"),
		Layer)) return false;
	TestEqual(TEXT("public import preserves the exported root property"),
		Layer->GetCellSize(), 640);
	TestFalse(TEXT("public import reload is clean"),
		Layer->GetOutermost()->IsDirty());
	const FBoolProperty* ImportedNanite = FindFProperty<FBoolProperty>(
		Layer->GetHLODBuilderSettings()->GetClass(), TEXT("bDisallowNanite"));
	TestTrue(TEXT("public import persists the nested builder setting"),
		ImportedNanite && !ImportedNanite->GetPropertyValue_InContainer(
			Layer->GetHLODBuilderSettings()));
    // Load the saved file into a distinct native package, so this observation
    // cannot be satisfied by the command's already resident readback object.
    const FString IndependentPackageName = TEXT("/Temp/UeAgentInterface/T208DiskReadback_")
        + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    FLinkerInstancingContext DiskContext;
    DiskContext.AddPackageMapping(FName(*PackageName), FName(*IndependentPackageName));
    UPackage* IndependentPackage = LoadPackage(CreatePackage(*IndependentPackageName), *Filename, LOAD_None, nullptr, &DiskContext);
    TStrongObjectPtr<UHLODLayer> IndependentLayer(IndependentPackage
        ? FindObject<UHLODLayer>(IndependentPackage, *FPackageName::GetLongPackageAssetName(PackageName)) : nullptr);
    const auto* DiskNanite = IndependentLayer.IsValid() && IndependentLayer->GetHLODBuilderSettings()
        ? FindFProperty<FBoolProperty>(IndependentLayer->GetHLODBuilderSettings()->GetClass(), TEXT("bDisallowNanite")) : nullptr;
    if (!TestTrue(TEXT("independent disk load observes native type, root and derived settings"),
        IndependentLayer.IsValid() && IndependentLayer.Get() != Layer && IndependentPackage != Layer->GetPackage()
        && IndependentLayer->GetLayerType() == EHLODLayerType::Instancing && IndependentLayer->GetCellSize() == 640
        && DiskNanite && !DiskNanite->GetPropertyValue_InContainer(IndependentLayer->GetHLODBuilderSettings()))) return false;
    TestTrue(TEXT("independent disk load preserves all non-target native values"), IndependentLayer.IsValid() && IndependentLayer->GetHLODBuilderSettings()
        && NativeState(*IndependentLayer, {TEXT("LayerType"), TEXT("HLODBuilderSettings"), TEXT("CellSize")}).OrderIndependentCompareEqual(NonTargetBefore)
        && NativeState(*IndependentLayer->GetHLODBuilderSettings(), {TEXT("bDisallowNanite")}).OrderIndependentCompareEqual(NonTargetSettingsBefore));
    IndependentLayer.Reset();
    ResetLoaders(IndependentPackage);
    TestTrue(TEXT("public edits preserve every other persistent root property"),
        NativeState(*Layer, {TEXT("LayerType"), TEXT("HLODBuilderSettings"), TEXT("CellSize")})
            .OrderIndependentCompareEqual(NonTargetBefore));
	TestTrue(TEXT("public edits preserve every other native settings property"),
		NativeState(*Layer->GetHLODBuilderSettings(), {TEXT("bDisallowNanite")})
			.OrderIndependentCompareEqual(NonTargetSettingsBefore));
	TArray<uint8> DiskBeforeLate;
	if (!TestTrue(TEXT("saved HLOD bytes are readable"),
		FFileHelper::LoadFileToArray(DiskBeforeLate, *Filename))) return false;

	FIntProperty* CellSize = FindFProperty<FIntProperty>(
		UHLODLayer::StaticClass(), TEXT("CellSize"));
	Layer->Modify();
	CellSize->SetPropertyValue_InContainer(Layer, 333);
	Layer->MarkPackageDirty();
	const auto MemoryRootBeforeLate = NativeState(*Layer);
	const auto MemorySettingsBeforeLate = NativeState(*Layer->GetHLODBuilderSettings());
    Export->SetStringField(TEXT("workspace_path"), Workspace / TEXT("late"));
    PublicResult = SubmitPublic(Registry, Dispatch, TEXT("content_graph.export_workspace"),
        TEXT("t208-hlod-export-late"), Export);
    if (!TestTrue(TEXT("dirty baseline exports through public HJSON"), PublicResult.IsOk())) {
        AddError(PublicResult.GetErrorJson()); return false;
    }
    if (!TestTrue(TEXT("late candidate edits public root type and size"),
        EditWorkspace(Workspace / TEXT("late"), false, 777))) return false;
    Source->SetStringField(TEXT("path"), Workspace / TEXT("late"));
    bool bLateSawPersisted = false;
    // Inject a discrepancy only when the real target has loaded the saved
    // candidate. No production test hook, alternate apply or rollback is used.
    const FDelegateHandle LoadedHandle = FCoreUObjectDelegates::OnAssetLoaded.AddLambda(
        [&](UObject* Loaded) {
            UHLODLayer* Published = Cast<UHLODLayer>(Loaded);
            if (bLateSawPersisted || !Published || Published->GetPathName() != ObjectPath
                || Published->GetCellSize() != 777 || Published->GetLayerType() != EHLODLayerType::MeshMerge)
                return;
            TArray<uint8> CandidateBytes;
            bLateSawPersisted = !Published->GetOutermost()->IsDirty()
                && Published->GetHLODBuilderSettings()
                && Published->GetHLODBuilderSettings()->GetClass()->GetName().Contains(TEXT("MeshMerge"))
                && FFileHelper::LoadFileToArray(CandidateBytes, *Filename)
                && CandidateBytes != DiskBeforeLate;
            if (bLateSawPersisted) CellSize->SetPropertyValue_InContainer(Published, 778);
        });
    ON_SCOPE_EXIT { FCoreUObjectDelegates::OnAssetLoaded.Remove(LoadedHandle); };
    Layer = nullptr;
    PublicResult = SubmitPublic(Registry, Dispatch, TEXT("content_graph.import_workspace"),
        TEXT("t208-hlod-import-late"), Import);
    TestFalse(TEXT("public late persistent readback discrepancy rejects the import"), PublicResult.IsOk());
    TestTrue(TEXT("late rejection observes saved and reloaded candidate before recovery"), bLateSawPersisted);
	Layer = LoadObject<UHLODLayer>(nullptr, *ObjectPath);
	TestTrue(TEXT("checkpoint restores the distinct dirty memory HLOD"),
		Layer && Layer->GetCellSize() == 333 && Layer->GetOutermost()->IsDirty());
	TestTrue(TEXT("late recovery restores complete native root and settings state"),
		Layer && NativeState(*Layer).OrderIndependentCompareEqual(MemoryRootBeforeLate)
		&& NativeState(*Layer->GetHLODBuilderSettings()).OrderIndependentCompareEqual(MemorySettingsBeforeLate));
	TArray<uint8> DiskAfterLate;
	TestTrue(TEXT("late recovery restores the original disk bytes separately"),
		FFileHelper::LoadFileToArray(DiskAfterLate, *Filename)
			&& DiskAfterLate == DiskBeforeLate);
	return !HasAnyErrors();
}

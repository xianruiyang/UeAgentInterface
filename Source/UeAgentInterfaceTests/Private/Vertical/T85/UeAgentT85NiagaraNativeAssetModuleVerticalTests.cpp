#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "DomainAdapters/Niagara/UeAgentNiagaraDomainMechanics.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/Niagara/Authoring/UeAgentNiagaraStageModuleAuthoring.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
#include "ObjectTools.h"
#include "QueryOwners/Niagara/UeAgentNiagaraModuleCandidateQuery.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Vertical/T85/UeAgentT85FixtureLifecycle.h"

namespace
{
	using namespace UE::UAI;
	using namespace UeAgentNiagaraStageModuleAuthoring;

	constexpr const TCHAR* NiagaraSystemType =
		TEXT("/Script/Niagara.NiagaraSystem");
	constexpr const TCHAR* Fingerprint =
		TEXT("sha256:2222222222222222222222222222222222222222222222222222222222222222");

	void DeleteAssetIfPresent(const FString& ObjectPath)
	{
		if (UObject* Existing = StaticLoadObject(
			UObject::StaticClass(), nullptr, *ObjectPath))
		{
			ObjectTools::DeleteObjectsUnchecked({Existing});
		}
	}

	bool SaveSystem(
		UPackage& Package,
		UNiagaraSystem& System,
		const FString& Filename)
	{
		System.RequestCompile(false);
		Package.MarkPackageDirty();
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		const bool bSaved = UPackage::SavePackage(
			&Package, &System, *Filename, Args);
		if (bSaved)
		{
			Package.SetDirtyFlag(false);
		}
		return bSaved;
	}

	const FResolvedStage* FindSystemUpdateStage(
		UObject& Target,
		const FString& UsageId,
		TArray<FResolvedStage>& OutStages,
		FString& OutError)
	{
		if (!EnumerateStages(Target, OutStages, OutError))
		{
			return nullptr;
		}
		FGuid ParsedUsageId;
		if (!FGuid::Parse(UsageId, ParsedUsageId))
		{
			OutError = TEXT("t85_niagara_usage_id_invalid");
			return nullptr;
		}
		return OutStages.FindByPredicate(
			[&ParsedUsageId](const FResolvedStage& Stage)
			{
				return Stage.Usage == ENiagaraScriptUsage::SystemUpdateScript
					&& Stage.UsageId == ParsedUsageId
					&& !Stage.EmitterHandleId.IsValid();
			});
	}

	bool TryBuildResolvedNiagaraImportParams(
		const FString& Target,
		const FString& ExpectedRevision,
		const FString& CandidateId,
		const QueryOwners::Niagara::FNiagaraModuleCandidateContext& Context,
		FString& OutParams,
		FString& OutError)
	{
		const TSharedRef<FJsonObject> Candidate = MakeShared<FJsonObject>();
		Candidate->SetStringField(TEXT("id"), CandidateId);
		const TSharedRef<FJsonObject> Module = MakeShared<FJsonObject>();
		Module->SetObjectField(TEXT("candidate"), Candidate);
		const TSharedRef<FJsonObject> Modules = MakeShared<FJsonObject>();
		Modules->SetObjectField(TEXT("native-asset-module"), Module);

		const TSharedRef<FJsonObject> StageTarget = MakeShared<FJsonObject>();
		StageTarget->SetStringField(TEXT("usage"), Context.TargetUsage);
		StageTarget->SetStringField(
			TEXT("usage_id"), Context.TargetUsageId);
		const TSharedRef<FJsonObject> Stage = MakeShared<FJsonObject>();
		Stage->SetObjectField(TEXT("target"), StageTarget);
		Stage->SetObjectField(TEXT("modules"), Modules);
		const TSharedRef<FJsonObject> Stages = MakeShared<FJsonObject>();
		Stages->SetObjectField(Context.StageKey, Stage);
		const TSharedRef<FJsonObject> Extension = MakeShared<FJsonObject>();
		Extension->SetObjectField(TEXT("stages"), Stages);

		const TSharedRef<FJsonObject> TargetObject = MakeShared<FJsonObject>();
		TargetObject->SetStringField(TEXT("asset_path"), Target);
		TargetObject->SetStringField(TEXT("asset_kind"), TEXT("system"));
		const TSharedRef<FJsonObject> AuthoringDocument =
			MakeShared<FJsonObject>();
		AuthoringDocument->SetStringField(TEXT("operation"), TEXT("update"));
		AuthoringDocument->SetObjectField(TEXT("target"), TargetObject);
		AuthoringDocument->SetObjectField(
			TEXT("provider_extension"), Extension);

		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("type"), NiagaraSystemType);
		Root->SetObjectField(TEXT("document"), AuthoringDocument);
		const TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
		Document->SetStringField(TEXT("schema"), TEXT("uai.niagara.edit/1"));
		Document->SetObjectField(TEXT("root"), Root);
		FString DocumentText;
		if (!CommandContract::Json::TryCanonicalize(
				Document, DocumentText, OutError))
		{
			return false;
		}

		const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("path"), TEXT("index.hjson"));
		Entry->SetStringField(TEXT("document"), DocumentText);
		Entry->SetStringField(TEXT("content_hash"), Fingerprint);
		const TSharedRef<FJsonObject> Source = MakeShared<FJsonObject>();
		Source->SetStringField(TEXT("kind"), TEXT("resolved"));
		Source->SetStringField(TEXT("carrier"), TEXT("inline"));
		Source->SetStringField(
			TEXT("source_unit"),
			TEXT("t85-niagara-native-asset-module-vertical"));
		Source->SetStringField(TEXT("target_identity"), Target);
		Source->SetStringField(TEXT("base_revision"), TEXT(""));
		Source->SetStringField(TEXT("entry_file"), TEXT("index.hjson"));
		Source->SetStringField(TEXT("source_fingerprint"), Fingerprint);
		Source->SetStringField(TEXT("semantic_fingerprint"), Fingerprint);
		Source->SetArrayField(
			TEXT("documents"), {MakeShared<FJsonValueObject>(Entry)});

		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target"), Target);
		Params->SetStringField(TEXT("expected_revision"), ExpectedRevision);
		Params->SetObjectField(TEXT("source"), Source);
		return CommandContract::Json::TryCanonicalize(
			Params, OutParams, OutError);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT85NiagaraNativeAssetModuleVerticalTest,
	"UAI.Root.T85.Niagara.NativeAssetModuleVertical",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85NiagaraNativeAssetModuleVerticalTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI;
	using namespace UE::UAI::QueryOwners::Niagara;

	const FString PackageName =
		TEXT("/Game/UAI/Transient/T85NiagaraNativeAssetModule/NS_NativeAssetModule_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	const FString Filename = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	DeleteAssetIfPresent(ObjectPath);
	ON_SCOPE_EXIT
	{
		DeleteAssetIfPresent(ObjectPath);
		UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(
			*this, PackageName);
	};

	UPackage* const Package = CreatePackage(*PackageName);
	UNiagaraSystem* const System = Package
		? NewObject<UNiagaraSystem>(
			Package,
			*AssetName,
			RF_Public | RF_Standalone | RF_Transactional)
		: nullptr;
	if (!TestNotNull(
		TEXT("a persistent Niagara System fixture is created"), System))
	{
		return false;
	}
	UNiagaraSystemFactoryNew::InitializeSystem(System, true);
	FAssetRegistryModule::AssetCreated(System);
	if (!TestTrue(
		TEXT("the initialized Niagara System fixture is saved"),
		SaveSystem(*Package, *System, Filename)))
	{
		return false;
	}

	FNiagaraModuleCandidateContext SearchContext;
	SearchContext.TargetUsage = TEXT("SystemUpdateScript");
	SearchContext.TargetUsageId = System->GetSystemUpdateScript()
		->GetUsageId().ToString(EGuidFormats::DigitsWithHyphensLower);
	TArray<FResolvedStage> InitialStages;
	FString StageError;
	const FResolvedStage* const InitialStage = FindSystemUpdateStage(
		*System, SearchContext.TargetUsageId, InitialStages, StageError);
	if (!TestNotNull(
		TEXT("the fixture exposes its real System Update output"), InitialStage))
	{
		AddError(StageError);
		return false;
	}
	TArray<UNiagaraNodeFunctionCall*> InitialModules;
	GetModules(*InitialStage->OutputNode, InitialModules);
	TMap<FString, int32> InitialScriptCounts;
	for (UNiagaraNodeFunctionCall* ModuleNode : InitialModules)
	{
		if (ModuleNode)
		{
			++InitialScriptCounts.FindOrAdd(GetModuleScriptPath(*ModuleNode));
		}
	}

	FNiagaraModuleCandidate Selected;
	FNiagaraModuleCandidateQueryResult CandidateResult;
	FNiagaraModuleCandidateQueryError QueryError;
	int32 Cursor = 0;
	do
	{
		CandidateResult = {};
		QueryError = {};
		if (!TestTrue(
			TEXT("search enumerates native asset-module actions in the exact output context"),
			TryQueryNiagaraModuleCandidates(
				*System,
				FString(),
				SearchContext,
				CandidateResult,
				QueryError,
				20,
				Cursor)))
		{
			AddError(QueryError.Code + TEXT(":") + QueryError.Detail);
			return false;
		}
		for (const FNiagaraModuleCandidate& Candidate
			: CandidateResult.Candidates)
		{
			if (Candidate.bSupported
				&& !InitialScriptCounts.Contains(Candidate.ScriptAssetPath))
			{
				Selected = Candidate;
				break;
			}
		}
		Cursor = CandidateResult.NextCursor;
	}
	while (Selected.Id.IsEmpty() && CandidateResult.bHasMore);
	if (!TestFalse(
		TEXT("a native System Update asset-module action is available"),
		Selected.Id.IsEmpty()))
	{
		return false;
	}
	TestTrue(TEXT("the public selector is compact"),
		Selected.Id.StartsWith(TEXT("candidate:")));
	TestFalse(TEXT("the public name is not a script path"),
		Selected.Name.StartsWith(TEXT("/")));
	TestTrue(TEXT("the query is sourced from the complete native Add Module owner"),
		CandidateResult.bCoverageComplete);
	TestFalse(TEXT("unsupported native action kinds keep creation coverage partial"),
		CandidateResult.bCreationCoverageComplete);
	TestFalse(TEXT("native candidate search leaves the persisted asset clean"),
		Package->IsDirty());

	FString CanonicalParams;
	FString ParamsError;
	if (!TestTrue(
		TEXT("id-only Niagara HJSON canonicalizes without a script path"),
		TryBuildResolvedNiagaraImportParams(
			PackageName,
			CandidateResult.TargetRevision,
			Selected.Id,
			CandidateResult.Context,
			CanonicalParams,
			ParamsError)))
	{
		AddError(ParamsError);
		return false;
	}

	FString ImportPayloadJson;
	bool bReadOnly = true;
	HjsonWorkspace::FWorkspaceError ImportError;
	if (!TestTrue(
		TEXT("id-only HJSON replays the native Niagara asset-module action"),
		DomainAdapters::Niagara::TryExecuteContentGraph(
			TEXT("content_graph.import_workspace"),
			PackageName,
			NiagaraSystemType,
			CanonicalParams,
			ImportPayloadJson,
			bReadOnly,
			ImportError)))
	{
		AddError(ImportError.GetCode() + TEXT(":")
			+ ImportError.GetSourceUnit() + TEXT(":")
			+ ImportError.GetJsonPointer());
		return false;
	}
	TestFalse(TEXT("the Niagara import is a mutation"), bReadOnly);
	TSharedPtr<FJsonObject> ImportPayload;
	FString PayloadError;
	if (!TestTrue(
		TEXT("the Niagara import payload is canonical JSON"),
		CommandContract::Json::TryParseObject(
			ImportPayloadJson, ImportPayload, PayloadError)))
	{
		AddError(PayloadError);
		return false;
	}
	FString TerminalStatus;
	bool bSaved = false;
	bool bReloaded = false;
	TestTrue(TEXT("the import reports the persisted reload lifecycle"),
		ImportPayload.IsValid()
			&& ImportPayload->TryGetStringField(
				TEXT("terminal_status"), TerminalStatus)
			&& TerminalStatus == TEXT("succeeded")
			&& ImportPayload->TryGetBoolField(TEXT("saved"), bSaved)
			&& bSaved
			&& ImportPayload->TryGetBoolField(TEXT("reloaded"), bReloaded)
			&& bReloaded);

	UNiagaraSystem* const Reloaded = LoadObject<UNiagaraSystem>(
		nullptr, *ObjectPath);
	if (!TestNotNull(
		TEXT("the persisted Niagara System independently reloads"), Reloaded))
	{
		return false;
	}
	TArray<FResolvedStage> ReloadedStages;
	StageError.Reset();
	const FResolvedStage* const ReloadedStage = FindSystemUpdateStage(
		*Reloaded,
		CandidateResult.Context.TargetUsageId,
		ReloadedStages,
		StageError);
	if (!TestNotNull(
		TEXT("the exact System Update output survives reload"), ReloadedStage))
	{
		AddError(StageError);
		return false;
	}
	TArray<UNiagaraNodeFunctionCall*> ReloadedModules;
	GetModules(*ReloadedStage->OutputNode, ReloadedModules);
	TArray<UNiagaraNodeFunctionCall*> MatchingModules;
	for (UNiagaraNodeFunctionCall* ModuleNode : ReloadedModules)
	{
		if (ModuleNode && GetModuleScriptPath(*ModuleNode)
				.Equals(Selected.ScriptAssetPath, ESearchCase::CaseSensitive))
		{
			MatchingModules.Add(ModuleNode);
		}
	}
	const int32 InitialCount = InitialScriptCounts.FindRef(
		Selected.ScriptAssetPath);
	TestEqual(
		TEXT("one searched native module persists in the target stack"),
		MatchingModules.Num(),
		InitialCount + 1);
	if (!MatchingModules.IsEmpty())
	{
		UNiagaraNodeFunctionCall* const Created = MatchingModules.Last();
		TestNotNull(TEXT("the persisted node resolves its module script"),
			Created->FunctionScript.Get());
		if (Created->FunctionScript
			&& Created->FunctionScript->IsVersioningEnabled())
		{
			TestEqual(
				TEXT("the native action selects the exposed script version"),
				Created->SelectedScriptVersion,
				Created->FunctionScript->GetExposedVersion().VersionGuid);
		}
	}

	DeleteAssetIfPresent(ObjectPath);
	UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(
		*this, PackageName);
	TestFalse(TEXT("the task-owned Niagara fixture leaves no asset file"),
		IFileManager::Get().FileExists(*Filename));
	return !HasAnyErrors();
}

#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "DomainAdapters/Pcg/UeAgentPcgDomainMechanics.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGSettings.h"
#include "QueryOwners/Pcg/UeAgentPcgNodeCandidateQuery.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
	using namespace UE::UAI;

	constexpr const TCHAR* PcgGraphType = TEXT("/Script/PCG.PCGGraph");
	constexpr const TCHAR* ExpectedSettingsType =
		TEXT("/Script/PCG.PCGCreatePointsGridSettings");
	constexpr const TCHAR* Fingerprint =
		TEXT("sha256:3333333333333333333333333333333333333333333333333333333333333333");

	void DeleteAssetIfPresent(const FString& ObjectPath)
	{
		if (UObject* Existing = StaticLoadObject(
			UObject::StaticClass(), nullptr, *ObjectPath))
		{
			ObjectTools::DeleteObjectsUnchecked({Existing});
		}
	}

	bool SaveGraph(
		UPackage& Package,
		UPCGGraph& Graph,
		const FString& Filename)
	{
		Package.MarkPackageDirty();
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		const bool bSaved = UPackage::SavePackage(
			&Package, &Graph, *Filename, Args);
		if (bSaved)
		{
			Package.SetDirtyFlag(false);
		}
		return bSaved;
	}

	bool TryBuildResolvedPcgImportParams(
		const FString& Target,
		const FString& ExpectedRevision,
		const FString& CandidateId,
		FString& OutParams,
		FString& OutError)
	{
		const TSharedRef<FJsonObject> Candidate = MakeShared<FJsonObject>();
		Candidate->SetStringField(TEXT("id"), CandidateId);
		const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
		Value->SetObjectField(TEXT("candidate"), Candidate);
		Value->SetArrayField(TEXT("position"), {
			MakeShared<FJsonValueNumber>(160.0),
			MakeShared<FJsonValueNumber>(96.0),
		});

		const TSharedRef<FJsonObject> Edit = MakeShared<FJsonObject>();
		Edit->SetArrayField(TEXT("depends_on"), {});
		Edit->SetStringField(TEXT("element_kind"), TEXT("item"));
		Edit->SetStringField(TEXT("identity"), TEXT("pcg_node:T85Native"));
		Edit->SetStringField(TEXT("intent"), TEXT("create"));
		Edit->SetObjectField(TEXT("value"), Value);

		const TSharedRef<FJsonObject> NativeParams = MakeShared<FJsonObject>();
		NativeParams->SetArrayField(
			TEXT("edits"), {MakeShared<FJsonValueObject>(Edit)});
		const TSharedRef<FJsonObject> Operation = MakeShared<FJsonObject>();
		Operation->SetStringField(TEXT("command_id"), TEXT("pcg.graph.apply"));
		Operation->SetObjectField(TEXT("params"), NativeParams);
		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("type"), PcgGraphType);
		Root->SetObjectField(TEXT("operation"), Operation);
		const TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
		Document->SetStringField(TEXT("schema"), TEXT("uai.pcg.edit/1"));
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
			TEXT("source_unit"), TEXT("t85-pcg-native-action-vertical"));
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
	FUeAgentT85PcgNativeActionVerticalTest,
	"UAI.Root.T85.PCG.NativeActionVertical",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85PcgNativeActionVerticalTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI;
	using namespace UE::UAI::QueryOwners::Pcg;

	const FString PackageName =
		TEXT("/Game/UAI/Transient/T85PcgNativeAction/PG_NativeAction_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	const FString Filename = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	DeleteAssetIfPresent(ObjectPath);
	ON_SCOPE_EXIT
	{
		DeleteAssetIfPresent(ObjectPath);
	};

	UPackage* const Package = CreatePackage(*PackageName);
	UPCGGraph* const Graph = Package
		? NewObject<UPCGGraph>(
			Package,
			*AssetName,
			RF_Public | RF_Standalone | RF_Transactional)
		: nullptr;
	if (!TestNotNull(TEXT("a persistent PCG Graph fixture is created"), Graph))
	{
		return false;
	}
	FAssetRegistryModule::AssetCreated(Graph);
	if (!TestTrue(
		TEXT("the initial PCG Graph fixture is saved"),
		SaveGraph(*Package, *Graph, Filename)))
	{
		return false;
	}

	UClass* const ExpectedClass = LoadObject<UClass>(
		nullptr, ExpectedSettingsType);
	const UPCGSettings* const ExpectedDefaults = ExpectedClass
		? Cast<UPCGSettings>(ExpectedClass->GetDefaultObject())
		: nullptr;
	if (!TestNotNull(
		TEXT("the representative native PCG settings class resolves"),
		ExpectedDefaults))
	{
		return false;
	}
	const FString ExpectedMenuName =
		ExpectedDefaults->GetDefaultNodeTitle().ToString();
	FPcgNodeCandidateQueryResult CandidateResult;
	FPcgNodeCandidateQueryError CandidateError;
	if (!TestTrue(
		TEXT("search enumerates the native PCG graph context menu"),
		TryQueryPcgNodeCandidates(
			*Graph,
			ExpectedMenuName,
			CandidateResult,
			CandidateError,
			64,
			0)))
	{
		AddError(CandidateError.Code + TEXT(":") + CandidateError.Detail);
		return false;
	}
	const FPcgNodeCandidate* const Selected =
		CandidateResult.Candidates.FindByPredicate(
			[&ExpectedMenuName](const FPcgNodeCandidate& Candidate)
			{
				return Candidate.bSupported
					&& Candidate.Name.Equals(
						ExpectedMenuName, ESearchCase::CaseSensitive);
			});
	if (!TestNotNull(
		TEXT("the exact native Create Points Grid action is searchable"),
		Selected))
	{
		return false;
	}
	TestTrue(TEXT("the public PCG selector is a compact id"),
		Selected->Id.StartsWith(TEXT("candidate:")));
	TestFalse(TEXT("the public PCG name is not a settings class path"),
		Selected->Name.StartsWith(TEXT("/Script/")));
	TestTrue(TEXT("search covers the complete native PCG owner"),
		CandidateResult.bCoverageComplete
			&& CandidateResult.NativeActionCount > 0);
	TestFalse(TEXT("unverified action kinds keep creation coverage partial"),
		CandidateResult.bCreationCoverageComplete);
	TestFalse(TEXT("native PCG search leaves the asset package clean"),
		Package->IsDirty());

	FString CanonicalParams;
	FString ParamsError;
	if (!TestTrue(
		TEXT("id-only PCG HJSON canonicalizes without a class binding"),
		TryBuildResolvedPcgImportParams(
			PackageName,
			CandidateResult.TargetRevision,
			Selected->Id,
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
		TEXT("id-only HJSON replays the same native PCG schema action"),
		DomainAdapters::Pcg::TryExecuteContentGraph(
			TEXT("content_graph.import_workspace"),
			PackageName,
			PcgGraphType,
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
	TestFalse(TEXT("the PCG import is a mutation"), bReadOnly);
	TSharedPtr<FJsonObject> ImportPayload;
	FString PayloadError;
	if (!TestTrue(
		TEXT("the PCG import payload is canonical JSON"),
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

	UPCGGraph* const Reloaded = LoadObject<UPCGGraph>(nullptr, *ObjectPath);
	if (!TestNotNull(
		TEXT("the persisted PCG Graph independently reloads"), Reloaded))
	{
		return false;
	}
	TArray<UPCGNode*> MatchingNodes;
	for (UPCGNode* Node : Reloaded->GetNodes())
	{
		if (Node && Node->HasAuthoredTitle()
			&& Node->GetAuthoredTitleName() == FName(TEXT("T85Native")))
		{
			MatchingNodes.Add(Node);
		}
	}
	TestEqual(TEXT("one native action-created node survives reload"),
		MatchingNodes.Num(), 1);
	if (MatchingNodes.Num() == 1)
	{
		UPCGNode* const Created = MatchingNodes[0];
		TestNotNull(TEXT("the persisted node owns native settings"),
			Created->GetSettings());
		if (Created->GetSettings())
		{
			TestEqual(TEXT("the searched action created the intended settings type"),
				Created->GetSettings()->GetClass()->GetPathName(),
				FString(ExpectedSettingsType));
		}
		TestEqual(TEXT("placement remains outside candidate and persists on X"),
			Created->PositionX, 160);
		TestEqual(TEXT("placement remains outside candidate and persists on Y"),
			Created->PositionY, 96);
	}

	DeleteAssetIfPresent(ObjectPath);
	TestFalse(TEXT("the task-owned PCG fixture leaves no asset file"),
		IFileManager::Get().FileExists(*Filename));
	return !HasAnyErrors();
}

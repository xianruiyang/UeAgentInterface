#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "DomainAdapters/Material/UeAgentMaterialDomainMechanics.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraphPin.h"
#include "HAL/FileManager.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphNode_Root.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "QueryOwners/Material/UeAgentMaterialExpressionCandidateQuery.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
	using namespace UE::UAI;

	constexpr const TCHAR* MaterialType = TEXT("/Script/Engine.Material");
	constexpr const TCHAR* Fingerprint =
		TEXT("sha256:1111111111111111111111111111111111111111111111111111111111111111");

	void DeleteAssetIfPresent(const FString& ObjectPath)
	{
		if (UObject* Existing = StaticLoadObject(
			UObject::StaticClass(), nullptr, *ObjectPath))
		{
			ObjectTools::DeleteObjectsUnchecked({Existing});
		}
	}

	bool SaveMaterial(
		UPackage& Package,
		UMaterial& Material,
		const FString& Filename)
	{
		Material.PostEditChange();
		Package.MarkPackageDirty();
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		const bool bSaved = UPackage::SavePackage(
			&Package, &Material, *Filename, Args);
		if (bSaved)
		{
			Package.SetDirtyFlag(false);
		}
		return bSaved;
	}

	UEdGraphPin* FindMaterialPropertyPin(
		UMaterialGraph& Graph,
		UMaterialGraphNode_Root& Root,
		const EMaterialProperty Property)
	{
		for (UEdGraphPin* Pin : Root.Pins)
		{
			if (Pin && Graph.MaterialInputs.IsValidIndex(Pin->SourceIndex)
				&& Graph.MaterialInputs[Pin->SourceIndex].GetProperty()
					== Property)
			{
				return Pin;
			}
		}
		return nullptr;
	}

	bool TryBuildResolvedMaterialImportParams(
		const FString& Target,
		const FString& ExpectedRevision,
		const FString& CandidateId,
		const QueryOwners::Material::FMaterialExpressionCandidateContext& Context,
		const FGuid& ExpressionGuid,
		FString& OutParams,
		FString& OutError)
	{
		const TSharedRef<FJsonObject> Candidate = MakeShared<FJsonObject>();
		Candidate->SetStringField(TEXT("id"), CandidateId);
		const TSharedRef<FJsonObject> CandidateContext = MakeShared<FJsonObject>();
		CandidateContext->SetStringField(TEXT("graph_name"), Context.GraphName);
		CandidateContext->SetStringField(TEXT("node_guid"), Context.NodeGuid);
		CandidateContext->SetStringField(TEXT("pin_guid"), Context.PinGuid);
		CandidateContext->SetStringField(TEXT("pin_name"), Context.PinName);
		const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
		Value->SetObjectField(TEXT("candidate"), Candidate);
		Value->SetObjectField(TEXT("candidate_context"), CandidateContext);

		const TSharedRef<FJsonObject> Edit = MakeShared<FJsonObject>();
		Edit->SetArrayField(TEXT("depends_on"), {});
		Edit->SetStringField(TEXT("element_kind"), TEXT("item"));
		Edit->SetStringField(
			TEXT("identity"),
			TEXT("expression:")
				+ ExpressionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		Edit->SetStringField(TEXT("intent"), TEXT("create"));
		Edit->SetObjectField(TEXT("value"), Value);

		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("type"), MaterialType);
		Root->SetArrayField(
			TEXT("edits"), {MakeShared<FJsonValueObject>(Edit)});
		const TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
		Document->SetStringField(TEXT("schema"), TEXT("uai.material.edit/1"));
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
			TEXT("source_unit"), TEXT("t85-material-native-action-vertical"));
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
	FUeAgentT85MaterialNativeActionVerticalTest,
	"UAI.Root.T85.Material.NativeActionVertical",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85MaterialNativeActionVerticalTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI;
	using namespace UE::UAI::QueryOwners::Material;

	const FString PackageName = TEXT("/Game/UAI/StrictV2/T85/M_NativeAction_")
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
	UMaterial* const Material = Package
		? NewObject<UMaterial>(
			Package,
			*AssetName,
			RF_Public | RF_Standalone | RF_Transactional)
		: nullptr;
	if (!TestNotNull(TEXT("a persistent Material fixture is created"), Material))
	{
		return false;
	}
	FAssetRegistryModule::AssetCreated(Material);
	if (!TestTrue(
		TEXT("the initial Material fixture is saved"),
		SaveMaterial(*Package, *Material, Filename)))
	{
		return false;
	}

	FMaterialExpressionCandidateQueryResult BootstrapResult;
	FMaterialExpressionCandidateQueryError QueryError;
	if (!TestTrue(
		TEXT("the native Material catalog initializes the target graph"),
		TryQueryMaterialExpressionCandidates(
			*Material, TEXT("Multiply"), BootstrapResult, QueryError)))
	{
		AddError(QueryError.Code + TEXT(":") + QueryError.Detail);
		return false;
	}
	UMaterialGraph* const Graph = Material->MaterialGraph;
	UMaterialGraphNode_Root* RootNode = nullptr;
	if (Graph)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UMaterialGraphNode_Root* const CandidateRoot =
				Cast<UMaterialGraphNode_Root>(Node))
			{
				RootNode = CandidateRoot;
				break;
			}
		}
	}
	if (!TestNotNull(TEXT("the Material graph has its native root node"), RootNode))
	{
		return false;
	}
	UEdGraphPin* const BaseColorPin =
		FindMaterialPropertyPin(*Graph, *RootNode, MP_BaseColor);
	if (!TestNotNull(TEXT("the root exposes the Base Color input"), BaseColorPin))
	{
		return false;
	}

	FMaterialExpressionCandidateContext SearchContext;
	SearchContext.GraphName = TEXT("MaterialGraph");
	SearchContext.NodeGuid = RootNode->NodeGuid.ToString(
		EGuidFormats::DigitsWithHyphensLower);
	SearchContext.PinGuid = BaseColorPin->PinId.ToString(
		EGuidFormats::DigitsWithHyphensLower);
	SearchContext.PinName = BaseColorPin->PinName.ToString();
	FMaterialExpressionCandidateQueryResult CandidateResult;
	QueryError = {};
	if (!TestTrue(
		TEXT("search enumerates the native Base Color context menu"),
		TryQueryMaterialExpressionCandidates(
			*Material,
			TEXT("Multiply"),
			CandidateResult,
			QueryError,
			20,
			0,
			SearchContext)))
	{
		AddError(QueryError.Code + TEXT(":") + QueryError.Detail);
		return false;
	}
	const FMaterialExpressionCandidate* const Multiply =
		CandidateResult.Candidates.FindByPredicate(
			[](const FMaterialExpressionCandidate& Candidate)
			{
				return Candidate.Name.Equals(
					TEXT("Multiply"), ESearchCase::IgnoreCase)
					&& Candidate.bSupported;
			});
	if (!TestNotNull(
		TEXT("the exact native Multiply action is searchable"), Multiply))
	{
		return false;
	}
	TestTrue(TEXT("the creation selector is a compact candidate id"),
		Multiply->Id.StartsWith(TEXT("candidate:")));

	DomainAdapters::ContentGraphRead::FUeAgentContentGraphCapture Before;
	DomainAdapters::ContentGraphRead::FUeAgentContentGraphReadError ReadError;
	if (!TestTrue(
		TEXT("the existing Material revision is captured before mutation"),
		DomainAdapters::ContentGraphRead::
			FUeAgentContentGraphReadProductionEnvironment::TryCaptureOwner(
				*Material, Before, ReadError)))
	{
		AddError(ReadError.Code + TEXT(":") + ReadError.Detail);
		return false;
	}

	const FGuid ExpressionGuid = FGuid::NewGuid();
	FString CanonicalParams;
	FString ParamsError;
	if (!TestTrue(
		TEXT("id-only Material HJSON canonicalizes"),
		TryBuildResolvedMaterialImportParams(
			PackageName,
			Before.Revision,
			Multiply->Id,
			CandidateResult.Context,
			ExpressionGuid,
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
		TEXT("id-only HJSON invokes the same native Material action"),
		DomainAdapters::Material::TryExecuteContentGraph(
			TEXT("content_graph.import_workspace"),
			PackageName,
			MaterialType,
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
	TestFalse(TEXT("the Material import is a mutation"), bReadOnly);
	TSharedPtr<FJsonObject> ImportPayload;
	FString PayloadError;
	if (!TestTrue(
		TEXT("the Material import payload is canonical JSON"),
		CommandContract::Json::TryParseObject(
			ImportPayloadJson, ImportPayload, PayloadError)))
	{
		AddError(PayloadError);
		return false;
	}
	FString TerminalStatus;
	bool bSaved = false;
	bool bReloaded = false;
	TestTrue(TEXT("the import reports successful terminal lifecycle"),
		ImportPayload.IsValid()
			&& ImportPayload->TryGetStringField(
				TEXT("terminal_status"), TerminalStatus)
			&& TerminalStatus == TEXT("succeeded")
			&& ImportPayload->TryGetBoolField(TEXT("saved"), bSaved)
			&& bSaved
			&& ImportPayload->TryGetBoolField(TEXT("reloaded"), bReloaded)
			&& bReloaded);

	UMaterial* const Reloaded = LoadObject<UMaterial>(nullptr, *ObjectPath);
	if (!TestNotNull(
		TEXT("the persisted Material independently reloads"), Reloaded))
	{
		return false;
	}
	UMaterialExpression* Created = nullptr;
	for (UMaterialExpression* Expression : Reloaded->GetExpressions())
	{
		if (Expression && Expression->MaterialExpressionGuid == ExpressionGuid)
		{
			Created = Expression;
			break;
		}
	}
	if (!TestNotNull(
		TEXT("the native action-created expression survives reload"), Created))
	{
		return false;
	}
	TestEqual(TEXT("the searched action creates a Multiply expression"),
		Created->GetClass()->GetPathName(),
		FString(TEXT("/Script/Engine.MaterialExpressionMultiply")));
	FExpressionInput* const BaseColor =
		Reloaded->GetExpressionInputForProperty(MP_BaseColor);
	TestTrue(TEXT("native action semantics preserve FromPin autowiring"),
		BaseColor && BaseColor->Expression == Created);

	DeleteAssetIfPresent(ObjectPath);
	TestFalse(TEXT("the task-owned Material fixture leaves no asset file"),
		IFileManager::Get().FileExists(*Filename));
	return !HasAnyErrors();
}

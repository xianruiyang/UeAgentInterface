#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/Composites/BTComposite_Selector.h"
#include "BehaviorTree/Tasks/BTTask_Wait.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode_Composite.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "BehaviorTreeGraphNode_Task.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "DomainAdapters/AiBehavior/UeAgentAiBehaviorDomainMechanics.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "QueryOwners/AiBehavior/UeAgentAiBehaviorCandidateQuery.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Vertical/T85/UeAgentT85FixtureLifecycle.h"

namespace
{
	using namespace UE::UAI;

	constexpr const TCHAR* BehaviorTreeType =
		TEXT("/Script/AIModule.BehaviorTree");
	constexpr const TCHAR* CompositeGuidText =
		TEXT("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
	constexpr const TCHAR* TaskGuidText =
		TEXT("11111111-2222-3333-4444-555555555555");
	constexpr const TCHAR* Fingerprint =
		TEXT("sha256:5555555555555555555555555555555555555555555555555555555555555555");
	constexpr const TCHAR* FixtureFolder =
		TEXT("/Game/UAI/Transient/T85BehaviorTreeNativeAction");

	void DeleteAssetIfPresent(const FString& ObjectPath)
	{
		if (UObject* Existing = StaticLoadObject(
			UObject::StaticClass(), nullptr, *ObjectPath))
		{
			ObjectTools::DeleteObjectsUnchecked({Existing});
		}
	}

	void DeleteOwnedFixtureAssets()
	{
		IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
		Registry.ScanPathsSynchronous({FixtureFolder}, true);
		TArray<FAssetData> Assets;
		Registry.GetAssetsByPath(FName(FixtureFolder), Assets);
		TArray<UObject*> Objects;
		for (const FAssetData& Asset : Assets)
		{
			if (UObject* const Object = Asset.GetAsset())
			{
				Objects.Add(Object);
			}
		}
		if (!Objects.IsEmpty())
		{
			ObjectTools::DeleteObjectsUnchecked(Objects);
		}
	}

	bool SaveBehaviorTree(
		UPackage& Package,
		UBehaviorTree& BehaviorTree,
		const FString& Filename)
	{
		Package.MarkPackageDirty();
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		const bool bSaved = UPackage::SavePackage(
			&Package, &BehaviorTree, *Filename, Args);
		if (bSaved)
		{
			Package.SetDirtyFlag(false);
		}
		return bSaved;
	}

	TSharedRef<FJsonObject> CreateEdit(
		const FString& SemanticId,
		const FString& ElementKind,
		const TArray<FString>& Dependencies,
		const TSharedRef<FJsonObject>& Value)
	{
		TArray<TSharedPtr<FJsonValue>> DependencyValues;
		for (const FString& Dependency : Dependencies)
		{
			DependencyValues.Add(MakeShared<FJsonValueString>(Dependency));
		}
		const TSharedRef<FJsonObject> Edit = MakeShared<FJsonObject>();
		Edit->SetArrayField(TEXT("depends_on"), MoveTemp(DependencyValues));
		Edit->SetStringField(TEXT("element_kind"), ElementKind);
		Edit->SetStringField(TEXT("intent"), TEXT("create"));
		Edit->SetStringField(TEXT("identity"), SemanticId);
		Edit->SetObjectField(TEXT("value"), Value);
		return Edit;
	}

	bool TryBuildResolvedBehaviorTreeImportParams(
		const FString& Target,
		const FString& ExpectedRevision,
		const FString& CompositeCandidateId,
		const FString& TaskCandidateId,
		const bool bRootDirectTask,
		FString& OutParams,
		FString& OutError)
	{
		const FString CompositeId =
			TEXT("node:") + FString(CompositeGuidText);
		const FString TaskId = TEXT("node:") + FString(TaskGuidText);
		const FString RootTargetId =
			bRootDirectTask ? TaskId : CompositeId;
		const FString RootRelationId = FString::Printf(
			TEXT("relation:node:root:out:0->%s:in:0"), *RootTargetId);
		const FString TaskRelationId = FString::Printf(
			TEXT("relation:%s:out:0->%s:in:0"), *CompositeId, *TaskId);

		const TSharedRef<FJsonObject> RootValue = MakeShared<FJsonObject>();
		RootValue->SetStringField(
			TEXT("carrier_type_ref"),
			TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode_Root"));

		const TSharedRef<FJsonObject> CompositeCandidate =
			MakeShared<FJsonObject>();
		CompositeCandidate->SetStringField(
			TEXT("id"), CompositeCandidateId);
		const TSharedRef<FJsonObject> CompositeValue = MakeShared<FJsonObject>();
		CompositeValue->SetObjectField(
			TEXT("candidate"), CompositeCandidate);
		CompositeValue->SetArrayField(
			TEXT("position"),
			{MakeShared<FJsonValueNumber>(384.0),
				MakeShared<FJsonValueNumber>(192.0)});

		const TSharedRef<FJsonObject> TaskCandidate = MakeShared<FJsonObject>();
		TaskCandidate->SetStringField(TEXT("id"), TaskCandidateId);
		const TSharedRef<FJsonObject> TaskValue = MakeShared<FJsonObject>();
		TaskValue->SetObjectField(TEXT("candidate"), TaskCandidate);
		TaskValue->SetArrayField(
			TEXT("position"),
			{MakeShared<FJsonValueNumber>(bRootDirectTask ? 384.0 : 640.0),
				MakeShared<FJsonValueNumber>(192.0)});

		const TSharedRef<FJsonObject> RootRelationValue =
			MakeShared<FJsonObject>();
		RootRelationValue->SetStringField(
			TEXT("from_item_ref"), TEXT("node:root"));
		RootRelationValue->SetNumberField(TEXT("from_output_index"), 0);
		RootRelationValue->SetStringField(
			TEXT("to_item_ref"), RootTargetId);
		RootRelationValue->SetNumberField(TEXT("to_input_index"), 0);

		const TSharedRef<FJsonObject> TaskRelationValue =
			MakeShared<FJsonObject>();
		TaskRelationValue->SetStringField(
			TEXT("from_item_ref"), CompositeId);
		TaskRelationValue->SetNumberField(TEXT("from_output_index"), 0);
		TaskRelationValue->SetStringField(TEXT("to_item_ref"), TaskId);
		TaskRelationValue->SetNumberField(TEXT("to_input_index"), 0);

		TArray<TSharedPtr<FJsonValue>> Edits;
		Edits.Add(MakeShared<FJsonValueObject>(CreateEdit(
			TEXT("node:root"), TEXT("item"), {}, RootValue)));
		if (!bRootDirectTask)
		{
			Edits.Add(MakeShared<FJsonValueObject>(CreateEdit(
				CompositeId, TEXT("item"), {}, CompositeValue)));
		}
		Edits.Add(MakeShared<FJsonValueObject>(CreateEdit(
			TaskId, TEXT("item"), {}, TaskValue)));
		Edits.Add(MakeShared<FJsonValueObject>(CreateEdit(
			RootRelationId,
			TEXT("relation"),
			{TEXT("node:root"), RootTargetId},
			RootRelationValue)));
		if (!bRootDirectTask)
		{
			Edits.Add(MakeShared<FJsonValueObject>(CreateEdit(
				TaskRelationId,
				TEXT("relation"),
				{CompositeId, TaskId},
				TaskRelationValue)));
		}

		const TSharedRef<FJsonObject> NativeParams = MakeShared<FJsonObject>();
		NativeParams->SetArrayField(TEXT("edits"), MoveTemp(Edits));
		const TSharedRef<FJsonObject> Operation = MakeShared<FJsonObject>();
		Operation->SetStringField(
			TEXT("command_id"), TEXT("ai.behavior_tree.graph.apply"));
		Operation->SetObjectField(TEXT("params"), NativeParams);
		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("type"), BehaviorTreeType);
		Root->SetObjectField(TEXT("operation"), Operation);
		const TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
		Document->SetStringField(TEXT("schema"), TEXT("uai.ai_behavior.edit/1"));
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
			TEXT("t85-behaviortree-native-action-vertical"));
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
	FUeAgentT85BehaviorTreeRootTargetRejectedTest,
	"UAI.Root.T85.BehaviorTree.RootTargetRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85BehaviorTreeRootTargetRejectedTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI;
	using namespace UE::UAI::QueryOwners::AiBehavior;

	DeleteOwnedFixtureAssets();
	const FString PackageName =
		TEXT("/Game/UAI/Transient/T85BehaviorTreeNativeAction/BT_InvalidRootTarget_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	const FString Filename = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	ON_SCOPE_EXIT
	{
		DeleteAssetIfPresent(ObjectPath);
		DeleteOwnedFixtureAssets();
		UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(
			*this, PackageName);
	};

	UPackage* const Package = CreatePackage(*PackageName);
	UBehaviorTree* const BehaviorTree = Package
		? NewObject<UBehaviorTree>(
			Package,
			*AssetName,
			RF_Public | RF_Standalone | RF_Transactional)
		: nullptr;
	if (!TestNotNull(
			TEXT("an invalid-topology fixture is created"), BehaviorTree))
	{
		return false;
	}
	FAssetRegistryModule::AssetCreated(BehaviorTree);
	if (!TestTrue(
			TEXT("the invalid-topology fixture is initially saved"),
			SaveBehaviorTree(*Package, *BehaviorTree, Filename)))
	{
		return false;
	}

	FAiBehaviorCandidateQueryResult CandidateResult;
	FAiBehaviorCandidateQueryError CandidateError;
	if (!TestTrue(
			TEXT("the native Wait action is available for the negative case"),
			TryQueryAiBehaviorCandidates(
				*BehaviorTree,
				TEXT("Wait"),
				CandidateResult,
				CandidateError,
				64,
				0)))
	{
		AddError(CandidateError.Code + TEXT(":") + CandidateError.Detail);
		return false;
	}
	const TArray<FAiBehaviorCandidate> WaitCandidates =
		CandidateResult.Candidates.FilterByPredicate(
			[](const FAiBehaviorCandidate& Candidate)
			{
				return Candidate.bSupported
					&& Candidate.PayloadType
						== UBTTask_Wait::StaticClass()->GetPathName();
			});
	if (!TestEqual(TEXT("the negative case resolves one native Wait action"),
		WaitCandidates.Num(), 1))
	{
		return false;
	}

	FString CanonicalParams;
	FString ParamsError;
	if (!TestTrue(
			TEXT("the root-to-task HJSON canonicalizes before topology preflight"),
			TryBuildResolvedBehaviorTreeImportParams(
				PackageName,
				CandidateResult.TargetRevision,
				FString(),
				WaitCandidates[0].Id,
				true,
				CanonicalParams,
				ParamsError)))
	{
		AddError(ParamsError);
		return false;
	}

	FString ImportPayloadJson;
	bool bReadOnly = true;
	HjsonWorkspace::FWorkspaceError ImportError;
	TestFalse(
		TEXT("root-to-task is rejected before the BehaviorTree writer"),
		DomainAdapters::AiBehavior::TryExecuteContentGraph(
			TEXT("content_graph.import_workspace"),
			PackageName,
			BehaviorTreeType,
			CanonicalParams,
			ImportPayloadJson,
			bReadOnly,
			ImportError));
	TestEqual(
		TEXT("the invalid root target has a typed error"),
		ImportError.GetCode(),
		FString(TEXT("graph_compound_behavior_tree_root_target_invalid")));
	TestFalse(TEXT("the rejected topology leaves the package clean"),
		Package->IsDirty());
#if WITH_EDITORONLY_DATA
	TestNull(TEXT("the rejected topology creates no BehaviorTree graph"),
		BehaviorTree->BTGraph);
#endif

	DeleteAssetIfPresent(ObjectPath);
	DeleteOwnedFixtureAssets();
	UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(
		*this, PackageName);
	TestFalse(TEXT("the negative fixture leaves no asset file"),
		IFileManager::Get().FileExists(*Filename));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT85BehaviorTreeNativeActionVerticalTest,
	"UAI.Root.T85.BehaviorTree.NativeActionVertical",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85BehaviorTreeNativeActionVerticalTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI;
	using namespace UE::UAI::QueryOwners::AiBehavior;

	DeleteOwnedFixtureAssets();
	const FString PackageName =
		TEXT("/Game/UAI/Transient/T85BehaviorTreeNativeAction/BT_NativeAction_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	const FString Filename = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	DeleteAssetIfPresent(ObjectPath);
	ON_SCOPE_EXIT
	{
		DeleteAssetIfPresent(ObjectPath);
		DeleteOwnedFixtureAssets();
		UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(
			*this, PackageName);
	};

	UPackage* const Package = CreatePackage(*PackageName);
	UBehaviorTree* const BehaviorTree = Package
		? NewObject<UBehaviorTree>(
			Package,
			*AssetName,
			RF_Public | RF_Standalone | RF_Transactional)
		: nullptr;
	if (!TestNotNull(
			TEXT("a persistent Behavior Tree fixture is created"), BehaviorTree))
	{
		return false;
	}
	FAssetRegistryModule::AssetCreated(BehaviorTree);
	if (!TestTrue(
			TEXT("the initial Behavior Tree fixture is saved"),
			SaveBehaviorTree(*Package, *BehaviorTree, Filename)))
	{
		return false;
	}

	FAiBehaviorCandidateQueryResult CandidateResult;
	FAiBehaviorCandidateQueryError CandidateError;
	if (!TestTrue(
			TEXT("search enumerates the native Behavior Tree blank menu"),
			TryQueryAiBehaviorCandidates(
				*BehaviorTree,
				FString(),
				CandidateResult,
				CandidateError,
				64,
				0)))
	{
		AddError(CandidateError.Code + TEXT(":") + CandidateError.Detail);
		return false;
	}
	const TArray<FAiBehaviorCandidate> WaitCandidates =
		CandidateResult.Candidates.FilterByPredicate(
			[](const FAiBehaviorCandidate& Candidate)
			{
				return Candidate.bSupported
					&& Candidate.PayloadType
						== UBTTask_Wait::StaticClass()->GetPathName();
			});
	if (!TestEqual(TEXT("the exact native Wait action is unique"),
		WaitCandidates.Num(), 1))
	{
		return false;
	}
	const TArray<FAiBehaviorCandidate> SelectorCandidates =
		CandidateResult.Candidates.FilterByPredicate(
			[](const FAiBehaviorCandidate& Candidate)
			{
				return Candidate.bSupported
					&& Candidate.PayloadType
						== UBTComposite_Selector::StaticClass()->GetPathName();
			});
	if (!TestEqual(TEXT("the exact native Selector action is unique"),
		SelectorCandidates.Num(), 1))
	{
		return false;
	}
	const FAiBehaviorCandidate& WaitCandidate = WaitCandidates[0];
	const FAiBehaviorCandidate& SelectorCandidate = SelectorCandidates[0];
	TestTrue(TEXT("the public Behavior Tree selector is a compact id"),
		WaitCandidate.Id.StartsWith(TEXT("candidate:"))
			&& SelectorCandidate.Id.StartsWith(TEXT("candidate:")));
	TestTrue(TEXT("search covers the complete native blank context"),
		CandidateResult.bCoverageComplete
			&& CandidateResult.NativeActionCount > 0);
	TestFalse(TEXT("native Behavior Tree search leaves the package clean"),
		Package->IsDirty());

	FString CanonicalParams;
	FString ParamsError;
	if (!TestTrue(
			TEXT("id-only Behavior Tree HJSON canonicalizes"),
			TryBuildResolvedBehaviorTreeImportParams(
				PackageName,
				CandidateResult.TargetRevision,
				SelectorCandidate.Id,
				WaitCandidate.Id,
				false,
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
			TEXT("id-only HJSON replays the native Selector and Wait actions"),
			DomainAdapters::AiBehavior::TryExecuteContentGraph(
				TEXT("content_graph.import_workspace"),
				PackageName,
				BehaviorTreeType,
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
	TestFalse(TEXT("the Behavior Tree import is a mutation"), bReadOnly);
	TSharedPtr<FJsonObject> ImportPayload;
	FString PayloadError;
	if (!TestTrue(
			TEXT("the Behavior Tree import payload is canonical JSON"),
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

	UBehaviorTree* const Reloaded =
		LoadObject<UBehaviorTree>(nullptr, *ObjectPath);
	if (!TestNotNull(
			TEXT("the persisted Behavior Tree independently reloads"), Reloaded))
	{
		return false;
	}
	UBehaviorTreeGraph* const Graph =
		Cast<UBehaviorTreeGraph>(Reloaded->BTGraph);
	if (!TestNotNull(TEXT("the reloaded Behavior Tree owns its graph"), Graph))
	{
		return false;
	}
	FGuid TaskGuid;
	FGuid CompositeGuid;
	FGuid::Parse(TaskGuidText, TaskGuid);
	FGuid::Parse(CompositeGuidText, CompositeGuid);
	UBehaviorTreeGraphNode_Task* TaskNode = nullptr;
	UBehaviorTreeGraphNode_Composite* CompositeNode = nullptr;
	UBehaviorTreeGraphNode_Root* RootNode = nullptr;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node && Node->NodeGuid == TaskGuid)
		{
			TaskNode = Cast<UBehaviorTreeGraphNode_Task>(Node);
		}
		if (Node && Node->NodeGuid == CompositeGuid)
		{
			CompositeNode = Cast<UBehaviorTreeGraphNode_Composite>(Node);
		}
		if (UBehaviorTreeGraphNode_Root* const CandidateRoot =
			Cast<UBehaviorTreeGraphNode_Root>(Node))
		{
			RootNode = CandidateRoot;
		}
	}
	if (!TestNotNull(TEXT("the fixed composite GUID survives reload"),
			CompositeNode)
		|| !TestNotNull(TEXT("the fixed task GUID survives reload"), TaskNode)
		|| !TestNotNull(TEXT("the root node survives reload"), RootNode))
	{
		return false;
	}
	TestTrue(TEXT("the task owns an exact Wait runtime instance"),
		TaskNode->NodeInstance
			&& TaskNode->NodeInstance->GetClass() == UBTTask_Wait::StaticClass());
	TestTrue(TEXT("the composite owns an exact Selector runtime instance"),
		CompositeNode->NodeInstance
			&& CompositeNode->NodeInstance->GetClass()
				== UBTComposite_Selector::StaticClass());
	TestTrue(TEXT("the persisted Selector retains pins"),
		!CompositeNode->Pins.IsEmpty());
	TestTrue(TEXT("the persisted Wait task retains pins"),
		!TaskNode->Pins.IsEmpty());
	TestEqual(TEXT("the persisted composite retains X placement"),
		CompositeNode->NodePosX, 384);
	TestEqual(TEXT("the persisted composite retains Y placement"),
		CompositeNode->NodePosY, 192);
	TestEqual(TEXT("the persisted task retains X placement"),
		TaskNode->NodePosX, 640);
	TestEqual(TEXT("the persisted task retains Y placement"),
		TaskNode->NodePosY, 192);
	UEdGraphPin* const RootOutput = RootNode->GetOutputPin();
	UEdGraphPin* const CompositeInput = CompositeNode->GetInputPin();
	UEdGraphPin* const CompositeOutput = CompositeNode->GetOutputPin();
	UEdGraphPin* const TaskInput = TaskNode->GetInputPin();
	TestTrue(TEXT("the root-to-composite relation survives reload"),
		RootOutput && CompositeInput
			&& RootOutput->LinkedTo.Contains(CompositeInput)
			&& CompositeInput->LinkedTo.Contains(RootOutput));
	TestTrue(TEXT("the composite-to-task relation survives reload"),
		CompositeOutput && TaskInput
			&& CompositeOutput->LinkedTo.Contains(TaskInput)
			&& TaskInput->LinkedTo.Contains(CompositeOutput));

	DeleteAssetIfPresent(ObjectPath);
	DeleteOwnedFixtureAssets();
	UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(
		*this, PackageName);
	TestFalse(TEXT("the task-owned Behavior Tree fixture leaves no asset file"),
		IFileManager::Get().FileExists(*Filename));
	return !HasAnyErrors();
}

#include "Misc/AutomationTest.h"

#include "AnimGraphNode_StateMachine.h"
#include "AnimStateEntryNode.h"
#include "AnimStateNode.h"
#include "Animation/AnimBlueprint.h"
#include "AnimationGraph.h"
#include "AnimationStateMachineGraph.h"
#include "AnimationStateMachineSchema.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "DomainAdapters/AnimationBlueprint/UeAgentAnimationBlueprintDomainMechanics.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "QueryOwners/AnimationBlueprint/UeAgentAnimationBlueprintQueryOwner.h"
#include "Vertical/T85/UeAgentT85FixtureLifecycle.h"

namespace
{
	using namespace UE::UAI;

	constexpr const TCHAR* Fingerprint =
		TEXT("sha256:8585858585858585858585858585858585858585858585858585858585858585");

	void DeleteAssetIfPresent(const FString& ObjectPath)
	{
		if (UObject* Existing = StaticLoadObject(
				UObject::StaticClass(), nullptr, *ObjectPath))
		{
			ObjectTools::DeleteObjectsUnchecked({Existing});
		}
	}

	TSharedRef<FJsonObject> MakeClassSettings()
	{
		const TSharedRef<FJsonObject> Settings = MakeShared<FJsonObject>();
		Settings->SetBoolField(TEXT("template"), true);
		Settings->SetStringField(
			TEXT("parent_class"), TEXT("/Script/Engine.AnimInstance"));
		return Settings;
	}

	bool TryBuildResolvedAnimationBlueprintImportParams(
		const FString& Target,
		const FString& ParentCandidateId,
		const FString& StateCandidateId,
		FString& OutParams,
		FString& OutError)
	{
		const TSharedRef<FJsonObject> ParentCandidate =
			MakeShared<FJsonObject>();
		ParentCandidate->SetStringField(TEXT("id"), ParentCandidateId);
		const TSharedRef<FJsonObject> StateCandidate =
			MakeShared<FJsonObject>();
		StateCandidate->SetStringField(TEXT("id"), StateCandidateId);

		const TSharedRef<FJsonObject> IdleState = MakeShared<FJsonObject>();
		IdleState->SetStringField(TEXT("name"), TEXT("Idle"));
		IdleState->SetObjectField(TEXT("candidate"), StateCandidate);
		IdleState->SetNumberField(TEXT("pos_x"), 240);
		IdleState->SetNumberField(TEXT("pos_y"), 0);

		const TSharedRef<FJsonObject> StateMachine = MakeShared<FJsonObject>();
		StateMachine->SetStringField(TEXT("name"), TEXT("Locomotion"));
		StateMachine->SetStringField(TEXT("animation_graph"), TEXT("AnimGraph"));
		StateMachine->SetObjectField(TEXT("candidate"), ParentCandidate);
		StateMachine->SetNumberField(TEXT("pos_x"), 320);
		StateMachine->SetNumberField(TEXT("pos_y"), 64);
		StateMachine->SetArrayField(
			TEXT("states"), {MakeShared<FJsonValueObject>(IdleState)});
		StateMachine->SetStringField(TEXT("entry_state"), TEXT("Idle"));
		StateMachine->SetArrayField(TEXT("transitions"), {});

		const TSharedRef<FJsonObject> ProviderExtension =
			MakeShared<FJsonObject>();
		ProviderExtension->SetObjectField(
			TEXT("class_settings"), MakeClassSettings());
		ProviderExtension->SetArrayField(
			TEXT("state_machines"),
			{MakeShared<FJsonValueObject>(StateMachine)});

		const TSharedRef<FJsonObject> DocumentTarget = MakeShared<FJsonObject>();
		DocumentTarget->SetStringField(TEXT("asset_path"), Target);
		const TSharedRef<FJsonObject> AuthoringDocument =
			MakeShared<FJsonObject>();
		AuthoringDocument->SetStringField(TEXT("operation"), TEXT("create"));
		AuthoringDocument->SetObjectField(TEXT("target"), DocumentTarget);
		AuthoringDocument->SetObjectField(
			TEXT("provider_extension"), ProviderExtension);

		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(
			TEXT("type"), TEXT("/Script/Engine.AnimBlueprint"));
		Root->SetObjectField(TEXT("document"), AuthoringDocument);
		const TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
		Document->SetStringField(
			TEXT("schema"), TEXT("uai.animation_blueprint.edit/1"));
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
			TEXT("t85-animation-state-machine-candidate-vertical"));
		Source->SetStringField(TEXT("target_identity"), Target);
		Source->SetStringField(TEXT("base_revision"), TEXT(""));
		Source->SetStringField(TEXT("entry_file"), TEXT("index.hjson"));
		Source->SetStringField(TEXT("source_fingerprint"), Fingerprint);
		Source->SetStringField(TEXT("semantic_fingerprint"), Fingerprint);
		Source->SetArrayField(
			TEXT("documents"), {MakeShared<FJsonValueObject>(Entry)});

		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target"), Target);
		Params->SetStringField(TEXT("expected_revision"), TEXT("missing"));
		Params->SetObjectField(TEXT("source"), Source);
		return CommandContract::Json::TryCanonicalize(
			Params, OutParams, OutError);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT85AnimationStateMachineCandidateVerticalTest,
	"UAI.Root.T85.AnimationBlueprint.StateMachineCandidateVertical",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85AnimationStateMachineCandidateVerticalTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI;
	using namespace UE::UAI::QueryOwners::AnimationBlueprint;

	const FString PackageName =
		TEXT("/Game/UAI/Transient/T85AnimationBlueprint/ABP_Candidate_")
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

	const TSharedPtr<FJsonObject> ClassSettings = MakeClassSettings();
	FAnimationBlueprintCandidateQueryResult ParentResult;
	FAnimationBlueprintCandidateQueryError CandidateError;
	if (!TestTrue(TEXT("the future AnimGraph native menu is searched"),
		TryQueryAnimationBlueprintCandidates(
			ObjectPath,
			TEXT("animation_graph"),
			ClassSettings,
			TEXT("State Machine"),
			ParentResult,
			CandidateError)))
	{
		AddError(CandidateError.Code + TEXT(":") + CandidateError.Detail);
		return false;
	}
	const FAnimationBlueprintCandidate* const ParentCandidate =
		ParentResult.Candidates.FindByPredicate(
			[](const FAnimationBlueprintCandidate& Candidate)
			{
				return Candidate.bSupported
					&& Candidate.NodeClassPath
						== UAnimGraphNode_StateMachine::StaticClass()->GetPathName();
			});
	if (!TestNotNull(TEXT("one writable native StateMachine action is exposed"),
		ParentCandidate))
	{
		return false;
	}

	FAnimationBlueprintCandidateQueryResult StateResult;
	CandidateError = {};
	if (!TestTrue(TEXT("the future StateMachine schema menu is searched"),
		TryQueryAnimationBlueprintCandidates(
			ObjectPath,
			TEXT("state_machine_graph"),
			ClassSettings,
			TEXT("State"),
			StateResult,
			CandidateError)))
	{
		AddError(CandidateError.Code + TEXT(":") + CandidateError.Detail);
		return false;
	}
	const FAnimationBlueprintCandidate* const StateCandidate =
		StateResult.Candidates.FindByPredicate(
			[](const FAnimationBlueprintCandidate& Candidate)
			{
				return Candidate.bSupported
					&& Candidate.NodeClassPath
						== UAnimStateNode::StaticClass()->GetPathName();
			});
	if (!TestNotNull(TEXT("one writable native State action is exposed"),
		StateCandidate))
	{
		return false;
	}
	TestTrue(TEXT("the parent candidate exposes a compact id"),
		ParentCandidate->Id.StartsWith(TEXT("candidate:")));
	TestTrue(TEXT("the State candidate exposes a compact id"),
		StateCandidate->Id.StartsWith(TEXT("candidate:")));
	TestFalse(TEXT("the child catalog marks unsupported siblings search-only"),
		StateResult.bCreationCoverageComplete);

	FString CanonicalParams;
	FString ParamsError;
	if (!TestTrue(TEXT("id-only Animation Blueprint HJSON canonicalizes"),
		TryBuildResolvedAnimationBlueprintImportParams(
			ObjectPath,
			ParentCandidate->Id,
			StateCandidate->Id,
			CanonicalParams,
			ParamsError)))
	{
		AddError(ParamsError);
		return false;
	}
	TestFalse(TEXT("the HJSON input contains no hidden node class binding"),
		CanonicalParams.Contains(TEXT("\"class\"")));

	FString ImportPayloadJson;
	bool bReadOnly = true;
	HjsonWorkspace::FWorkspaceError ImportError;
	if (!TestTrue(TEXT("id-only HJSON creates and persists native actions"),
		DomainAdapters::AnimationBlueprint::TryExecuteContentGraph(
			TEXT("content_graph.import_workspace"),
			ObjectPath,
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
	TestFalse(TEXT("the Animation Blueprint import is a mutation"), bReadOnly);

	TSharedPtr<FJsonObject> ImportPayload;
	FString PayloadError;
	if (!TestTrue(TEXT("the import payload is canonical JSON"),
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

	UAnimBlueprint* const Reloaded =
		LoadObject<UAnimBlueprint>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("the persisted AnimBlueprint reloads"), Reloaded))
	{
		return false;
	}
	TestNotNull(TEXT("the native compile generated a class"),
		Reloaded->GeneratedClass.Get());
	TestTrue(TEXT("the reloaded AnimBlueprint is not in compile error"),
		Reloaded->Status != BS_Error);

	TArray<UEdGraph*> Graphs;
	Reloaded->GetAllGraphs(Graphs);
	UEdGraph* AnimGraph = nullptr;
	for (UEdGraph* Graph : Graphs)
	{
		if (Graph
			&& Graph->GetFName() == TEXT("AnimGraph")
			&& Graph->IsA<UAnimationGraph>())
		{
			AnimGraph = Graph;
			break;
		}
	}
	if (!TestNotNull(TEXT("the native AnimGraph survives reload"), AnimGraph))
	{
		return false;
	}
	TArray<UAnimGraphNode_StateMachine*> MachineNodes;
	AnimGraph->GetNodesOfClass<UAnimGraphNode_StateMachine>(MachineNodes);
	UAnimGraphNode_StateMachine* MachineNode = nullptr;
	for (UAnimGraphNode_StateMachine* Node : MachineNodes)
	{
		if (Node
			&& Node->GetClass()
				== UAnimGraphNode_StateMachine::StaticClass()
			&& Node->EditorStateMachineGraph
			&& Node->EditorStateMachineGraph->GetFName()
				== TEXT("Locomotion"))
			{
			MachineNode = Node;
			break;
		}
	}
	if (!TestNotNull(TEXT("the exact native StateMachine node survives reload"),
		MachineNode))
	{
		return false;
	}
	UAnimationStateMachineGraph* const MachineGraph =
		MachineNode->EditorStateMachineGraph;
	TestEqual(TEXT("the StateMachine keeps its native schema"),
		MachineGraph->GetSchema()->GetClass(),
		UAnimationStateMachineSchema::StaticClass());
	TestTrue(TEXT("the StateMachine subgraph remains registered"),
		Graphs.Contains(MachineGraph));

	TArray<UAnimStateEntryNode*> EntryNodes;
	MachineGraph->GetNodesOfClass<UAnimStateEntryNode>(EntryNodes);
	TestEqual(TEXT("the native spawner retains one default Entry"),
		EntryNodes.Num(), 1);
	TArray<UAnimStateNode*> StateNodes;
	MachineGraph->GetNodesOfClass<UAnimStateNode>(StateNodes);
	UAnimStateNode* StateNode = nullptr;
	for (UAnimStateNode* Node : StateNodes)
	{
		if (Node
			&& Node->GetClass() == UAnimStateNode::StaticClass()
			&& Node->GetBoundGraph()
			&& Node->GetBoundGraph()->GetFName() == TEXT("Idle"))
		{
			StateNode = Node;
			break;
		}
	}
	if (!TestNotNull(TEXT("the exact native State action survives reload"),
		StateNode))
	{
		return false;
	}
	TestTrue(TEXT("the State bound graph remains registered"),
		Graphs.Contains(StateNode->GetBoundGraph()));
	TestTrue(TEXT("the State bound graph remains target-owned"),
		StateNode->GetBoundGraph()->GetPathName().StartsWith(ObjectPath));

	DeleteAssetIfPresent(ObjectPath);
	UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(
		*this, PackageName);
	TestFalse(TEXT("the task-owned AnimBlueprint leaves no asset file"),
		IFileManager::Get().FileExists(*Filename));
	return !HasAnyErrors();
}

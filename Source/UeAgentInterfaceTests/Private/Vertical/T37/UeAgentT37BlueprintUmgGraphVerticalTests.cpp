#include "Misc/AutomationTest.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "DomainAdapters/Blueprint/Component/UeAgentBlueprintComponentMechanics.h"
#include "DomainAdapters/Blueprint/Node/UeAgentBlueprintNodeMechanics.h"
#include "DomainAdapters/Blueprint/Surfaces/UeAgentBlueprintCandidateBindingNormalizer.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintMutationModel.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/ScopeExit.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT37BlueprintCandidateBindingNormalizationTest,
	"UAI.Root.T37.BlueprintUmgGraph.CandidateBindingNormalization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT37BlueprintCandidateBindingNormalizationTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI;
	using namespace DomainAdapters::Blueprint;

	const FName BlueprintName(*FString(TEXT("BP_T37_CandidateNormalization_"))
		.Append(FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		GetTransientPackage(),
		BlueprintName,
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(),
		TEXT("T37"));
	ON_SCOPE_EXIT
	{
		if (Blueprint)
		{
			Blueprint->ClearFlags(RF_Public | RF_Standalone);
			Blueprint->MarkAsGarbage();
		}
	};
	if (!TestNotNull(TEXT("a transient Actor Blueprint is created"), Blueprint))
	{
		return false;
	}
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	const FString Target = Blueprint->GetPathName();

	UEdGraph* CandidateGraph = Blueprint->UbergraphPages.IsEmpty()
		? nullptr
		: Blueprint->UbergraphPages[0];
	if (!TestNotNull(TEXT("a readable editable graph is available"), CandidateGraph))
	{
		return false;
	}

	Node::FUeAgentBlueprintNodeMechanicsError NodeQueryError;
	FString NodePayloadJson;
	Node::FUeAgentBlueprintNodeCandidateQuery NodeQuery;
	NodeQuery.Target = Target;
	NodeQuery.GraphName = CandidateGraph->GetName();
	NodeQuery.Query = TEXT("FInterpTo");
	if (!TestTrue(TEXT("the current FInterpTo candidate is queried"),
		Node::TryQueryBlueprintNodeCandidates(
			NodeQuery,
			NodePayloadJson,
			NodeQueryError)))
	{
		AddError(NodeQueryError.Code + TEXT(":") + NodeQueryError.PublicMessage);
		return false;
	}
	Component::FUeAgentBlueprintComponentMechanicsError ComponentQueryError;
	FString ComponentPayloadJson;
	Component::FUeAgentBlueprintComponentCandidateQuery ComponentQuery;
	ComponentQuery.Target = Target;
	ComponentQuery.Parent = TEXT("DefaultSceneRoot");
	ComponentQuery.Query = TEXT("Point Light");
	if (!TestTrue(TEXT("the current PointLight candidate is queried"),
		Component::TryQueryBlueprintComponentCandidates(
			ComponentQuery,
			ComponentPayloadJson,
			ComponentQueryError)))
	{
		AddError(ComponentQueryError.Code + TEXT(":")
			+ ComponentQueryError.PublicMessage);
		return false;
	}

	auto ParseFirstCandidate = [this](
		const FString& PayloadJson,
		const FString& Label,
		TSharedPtr<FJsonObject>& OutCandidate)
	{
		TSharedPtr<FJsonObject> Payload;
		FString JsonError;
		const TArray<TSharedPtr<FJsonValue>>* Candidates = nullptr;
		if (!TestTrue(*Label,
			CommandContract::Json::TryParseObject(
				PayloadJson, Payload, JsonError)
				&& Payload.IsValid()
				&& Payload->TryGetArrayField(TEXT("candidates"), Candidates)
				&& Candidates
				&& Candidates->Num() == 1
				&& (*Candidates)[0].IsValid()
				&& (*Candidates)[0]->Type == EJson::Object))
		{
			AddError(JsonError);
			return false;
		}
		OutCandidate = (*Candidates)[0]->AsObject();
		return OutCandidate.IsValid();
	};
	auto ParseUniqueNamedCandidate = [this](
		const FString& PayloadJson,
		const FString& ExpectedName,
		TSharedPtr<FJsonObject>& OutCandidate)
	{
		TSharedPtr<FJsonObject> Payload;
		FString JsonError;
		const TArray<TSharedPtr<FJsonValue>>* Candidates = nullptr;
		if (!CommandContract::Json::TryParseObject(
				PayloadJson, Payload, JsonError)
			|| !Payload.IsValid()
			|| !Payload->TryGetArrayField(TEXT("candidates"), Candidates)
			|| !Candidates)
		{
			AddError(JsonError);
			return false;
		}
		TArray<TSharedPtr<FJsonObject>> Matches;
		auto NormalizeMenuName = [](const FString& Value)
		{
			FString Result;
			for (const TCHAR Character : Value)
			{
				if (FChar::IsAlnum(Character))
				{
					Result.AppendChar(FChar::ToLower(Character));
				}
			}
			return Result;
		};
		const FString NormalizedExpected = NormalizeMenuName(ExpectedName);
		for (const TSharedPtr<FJsonValue>& Value : *Candidates)
		{
			const TSharedPtr<FJsonObject> Candidate = Value.IsValid()
				&& Value->Type == EJson::Object
				? Value->AsObject()
				: nullptr;
			FString Name;
			FString DisplayName;
			if (Candidate.IsValid())
			{
				Candidate->TryGetStringField(TEXT("name"), Name);
				Candidate->TryGetStringField(TEXT("display_name"), DisplayName);
			}
			if (NormalizeMenuName(Name) == NormalizedExpected
				|| NormalizeMenuName(DisplayName) == NormalizedExpected)
			{
				Matches.Add(Candidate);
			}
		}
		if (!TestEqual(TEXT("the native menu name resolves uniquely"),
			Matches.Num(), 1))
		{
			return false;
		}
		OutCandidate = Matches[0];
		return true;
	};

	FString NodeCandidateName;
	TSharedPtr<FJsonObject> ComponentCandidate;
	TSharedPtr<FJsonObject> NodePayload;
	FString NodeJsonError;
	const TArray<TSharedPtr<FJsonValue>>* NodeNames = nullptr;
	if (!CommandContract::Json::TryParseObject(
			NodePayloadJson, NodePayload, NodeJsonError)
		|| !NodePayload.IsValid()
		|| NodePayload->Values.Num() != 1
		|| !NodePayload->TryGetArrayField(TEXT("names"), NodeNames)
		|| !NodeNames || NodeNames->IsEmpty()
		|| !(*NodeNames)[0].IsValid()
		|| (*NodeNames)[0]->Type != EJson::String)
	{
		AddError(NodeJsonError);
		return false;
	}
	NodeCandidateName = (*NodeNames)[0]->AsString();
	if (!ParseUniqueNamedCandidate(
			ComponentPayloadJson,
			TEXT("Point Light"),
			ComponentCandidate))
	{
		return false;
	}

	const TSharedRef<FJsonObject> ComponentSpec = MakeShared<FJsonObject>();
	ComponentSpec->SetStringField(TEXT("operation"), TEXT("create"));
	FString ComponentCandidateId;
	if (!TestTrue(TEXT("the native component candidate has a compact id"),
		ComponentCandidate->TryGetStringField(
			TEXT("id"), ComponentCandidateId)))
	{
		return false;
	}
	const TSharedRef<FJsonObject> ComponentSelector = MakeShared<FJsonObject>();
	ComponentSelector->SetStringField(TEXT("id"), ComponentCandidateId);
	ComponentSpec->SetObjectField(TEXT("candidate"), ComponentSelector);
	ComponentSpec->SetStringField(TEXT("parent"), TEXT("DefaultSceneRoot"));
	const TSharedRef<FJsonObject> Components = MakeShared<FJsonObject>();
	Components->SetObjectField(TEXT("component:T37PointLight"), ComponentSpec);

	const TSharedRef<FJsonObject> NodeSpec = MakeShared<FJsonObject>();
	const TSharedRef<FJsonObject> NodeSelector = MakeShared<FJsonObject>();
	NodeSelector->SetStringField(TEXT("name"), NodeCandidateName);
	NodeSpec->SetObjectField(TEXT("candidate"), NodeSelector);
	const TSharedRef<FJsonObject> Nodes = MakeShared<FJsonObject>();
	Nodes->SetObjectField(
		TEXT("11111111-1111-1111-1111-111111111111"), NodeSpec);
	const TSharedRef<FJsonObject> GraphSpec = MakeShared<FJsonObject>();
	GraphSpec->SetStringField(TEXT("operation"), TEXT("update"));
	GraphSpec->SetObjectField(TEXT("nodes"), Nodes);
	const TSharedRef<FJsonObject> Graphs = MakeShared<FJsonObject>();
	Graphs->SetObjectField(TEXT("event_graph:EventGraph"), GraphSpec);

	const TSharedRef<FJsonObject> Extension = MakeShared<FJsonObject>();
	Extension->SetObjectField(TEXT("components"), Components);
	Extension->SetObjectField(TEXT("graphs"), Graphs);
	HjsonWorkspace::FWorkspaceError WorkspaceError;
	if (!TestTrue(TEXT("candidate identities normalize before mutation"),
		Surfaces::TryNormalizeBlueprintCandidateBindings(
			Target, Extension, WorkspaceError)))
	{
		AddError(WorkspaceError.GetCode());
		return false;
	}
	FString Normalized;
	TestFalse(TEXT("the component type never overwrites the stable-map instance name"),
		ComponentSpec->HasField(TEXT("name")));
	TestTrue(TEXT("the component candidate class is expanded server-side"),
		ComponentSpec->TryGetStringField(TEXT("class"), Normalized));
	TestEqual(TEXT("the component class remains exact"),
		Normalized, FString(TEXT("/Script/Engine.PointLightComponent")));
	TestTrue(TEXT("the node candidate kind is expanded server-side"),
		NodeSpec->TryGetStringField(TEXT("node_type"), Normalized));
	TestEqual(TEXT("the normalized node kind is native"),
		Normalized, FString(TEXT("native_action")));
	TestFalse(TEXT("the model is not required to copy a node class"),
		NodeSpec->HasField(TEXT("node_class")));
	TestEqual(TEXT("the readable node candidate name remains exact"),
		NodeSelector->GetStringField(TEXT("name")), NodeCandidateName);
	TestEqual(TEXT("the component compact candidate id remains exact"),
		ComponentSelector->GetStringField(TEXT("id")), ComponentCandidateId);

	TUniquePtr<UeAgentBlueprintAuthoring::FPreparedBlueprintMutation>
		ComponentMutation;
	UeAgentBlueprintAuthoring::FMutationCounters ComponentMutationCounters;
	FString ComponentMutationError;
	const TSharedRef<FJsonObject> ComponentOnlyExtension =
		MakeShared<FJsonObject>();
	ComponentOnlyExtension->SetObjectField(TEXT("components"), Components);
	if (!TestTrue(TEXT("id-only HJSON invokes the native Add Component action"),
		UeAgentBlueprintAuthoring::FPreparedBlueprintMutation::Prepare(
			*Blueprint,
			ComponentOnlyExtension,
			ComponentMutation,
			ComponentMutationCounters,
			ComponentMutationError)))
	{
		AddError(ComponentMutationError);
		return false;
	}
	TestEqual(TEXT("one native Add Component action creates one SCS node"),
		ComponentMutationCounters.ComponentsAdded, 1);
	USCS_Node* CreatedPointLight = Blueprint->SimpleConstructionScript
		? Blueprint->SimpleConstructionScript->FindSCSNode(
			FName(TEXT("T37PointLight")))
		: nullptr;
	TestNotNull(TEXT("the requested component instance name is preserved"),
		CreatedPointLight);
	if (CreatedPointLight)
	{
		TestEqual(TEXT("the native action creates the resolved component class"),
			CreatedPointLight->ComponentClass->GetPathName(),
			FString(TEXT("/Script/Engine.PointLightComponent")));
	}
	TestTrue(TEXT("the native node candidate keeps its direct name selector"),
		!NodeCandidateName.IsEmpty());
	const TSharedRef<FJsonObject> NameSelector = MakeShared<FJsonObject>();
	NameSelector->SetStringField(TEXT("name"), NodeCandidateName);
	const TSharedRef<FJsonObject> NameNodeSpec = MakeShared<FJsonObject>();
	NameNodeSpec->SetObjectField(TEXT("candidate"), NameSelector);
	const TSharedRef<FJsonObject> NameNodes = MakeShared<FJsonObject>();
	NameNodes->SetObjectField(
		TEXT("33333333-3333-3333-3333-333333333333"), NameNodeSpec);
	const TSharedRef<FJsonObject> NameGraphSpec = MakeShared<FJsonObject>();
	NameGraphSpec->SetObjectField(TEXT("nodes"), NameNodes);
	const TSharedRef<FJsonObject> NameGraphs = MakeShared<FJsonObject>();
	NameGraphs->SetObjectField(TEXT("event_graph:EventGraph"), NameGraphSpec);
	const TSharedRef<FJsonObject> NameOnlyExtension = MakeShared<FJsonObject>();
	NameOnlyExtension->SetObjectField(TEXT("graphs"), NameGraphs);
	WorkspaceError.Reset();
	if (!TestTrue(TEXT("name-only HJSON resolves in the same graph context"),
		Surfaces::TryNormalizeBlueprintCandidateBindings(
			Target, NameOnlyExtension, WorkspaceError)))
	{
		AddError(WorkspaceError.GetCode());
		return false;
	}
	TestEqual(TEXT("name-only HJSON resolves to the native action kind"),
		NameNodeSpec->GetStringField(TEXT("node_type")),
		FString(TEXT("native_action")));

	TUniquePtr<UeAgentBlueprintAuthoring::FPreparedBlueprintMutation> Mutation;
	UeAgentBlueprintAuthoring::FMutationCounters MutationCounters;
	FString MutationError;
	const TSharedRef<FJsonObject> NodeOnlyExtension = MakeShared<FJsonObject>();
	NodeOnlyExtension->SetObjectField(TEXT("graphs"), Graphs);
	if (!TestTrue(TEXT("id-only HJSON invokes the native action during preflight"),
		UeAgentBlueprintAuthoring::FPreparedBlueprintMutation::Prepare(
			*Blueprint,
			NodeOnlyExtension,
			Mutation,
			MutationCounters,
			MutationError)))
	{
		AddError(MutationError);
		return false;
	}
	TestEqual(TEXT("one physical node is created for one HJSON item"),
		MutationCounters.NodesAdded, 1);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	UEdGraph* EventGraph = Blueprint->UbergraphPages.IsEmpty()
		? nullptr
		: Blueprint->UbergraphPages[0];
	int32 FInterpToCount = 0;
	if (TestNotNull(TEXT("the fixture retains its EventGraph"), EventGraph))
	{
		for (UEdGraphNode* GraphNode : EventGraph->Nodes)
		{
			const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(GraphNode);
			const UFunction* Function = Call ? Call->GetTargetFunction() : nullptr;
			if (Function && Function->GetName() == TEXT("FInterpTo"))
			{
				++FInterpToCount;
			}
		}
	}
	TestEqual(TEXT("the native action created the requested FInterpTo call"),
		FInterpToCount, 1);

	const TSharedRef<FJsonObject> StaleCandidate = MakeShared<FJsonObject>();
	StaleCandidate->SetStringField(
		TEXT("id"), TEXT("candidate:00000000000000000000000000000000"));
	const TSharedRef<FJsonObject> StaleNodeSpec = MakeShared<FJsonObject>();
	StaleNodeSpec->SetStringField(TEXT("operation"), TEXT("create"));
	StaleNodeSpec->SetObjectField(TEXT("candidate"), StaleCandidate);
	const TSharedRef<FJsonObject> StaleNodes = MakeShared<FJsonObject>();
	StaleNodes->SetObjectField(
		TEXT("22222222-2222-2222-2222-222222222222"), StaleNodeSpec);
	const TSharedRef<FJsonObject> StaleGraphSpec = MakeShared<FJsonObject>();
	StaleGraphSpec->SetObjectField(TEXT("nodes"), StaleNodes);
	const TSharedRef<FJsonObject> StaleGraphs = MakeShared<FJsonObject>();
	StaleGraphs->SetObjectField(TEXT("event_graph:EventGraph"), StaleGraphSpec);
	const TSharedRef<FJsonObject> StaleExtension = MakeShared<FJsonObject>();
	StaleExtension->SetObjectField(TEXT("graphs"), StaleGraphs);
	WorkspaceError.Reset();
	TestFalse(TEXT("an unknown compact candidate id is rejected before mutation"),
		Surfaces::TryNormalizeBlueprintCandidateBindings(
			Target, StaleExtension, WorkspaceError));
	TestEqual(TEXT("the unknown candidate error is typed"),
		WorkspaceError.GetCode(),
		FString(TEXT("blueprint_action_candidate_not_found")));
	return !HasAnyErrors();
}

#include "Misc/AutomationTest.h"

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "CommandHandlers/AnimationBlueprint/UeAgentAnimationBlueprintDomainCommandHandlers.h"
#include "CommandHandlers/Blueprint/Surfaces/UeAgentBlueprintSurfaceCommandHandlers.h"
#include "CommandHandlers/UmgWidget/UeAgentUmgWidgetCommandHandlers.h"
#include "Core/Contract/UeAgentContract.h"
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
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "OperationOwners/AnimationBlueprint/UeAgentAnimationBlueprintDomainService.h"
#include "OperationOwners/Blueprint/Surfaces/UeAgentBlueprintSurfaceDomainService.h"
#include "OperationOwners/UmgWidget/UeAgentUmgWidgetDomainService.h"

namespace
{
	using namespace UE::UAI;

	class FNeverCancelled final : public Contract::IUeAgentCancellationSignal
	{
	public:
		virtual bool IsCancelled() const override { return false; }
	};

	class FBlueprintService final
		: public OperationOwners::Blueprint::Surfaces::
			IUeAgentBlueprintSurfaceCommandService
	{
	public:
		virtual bool Execute(
			const OperationOwners::Blueprint::Surfaces::
				FBlueprintSurfaceCommandRequest& Request,
			OperationOwners::Blueprint::Surfaces::
				FBlueprintSurfaceCommandResponse& OutResponse,
			OperationOwners::Blueprint::Surfaces::
				FBlueprintSurfaceCommandError& OutError) override
		{
			(void)Request;
			(void)OutResponse;
			(void)OutError;
			++Calls;
			return false;
		}

		int32 Calls = 0;
	};

	class FUmgService final
		: public OperationOwners::UmgWidget::IUeAgentUmgWidgetCommandService
	{
	public:
		virtual bool Execute(
			const OperationOwners::UmgWidget::FUmgWidgetCommandRequest& Request,
			OperationOwners::UmgWidget::FUmgWidgetCommandResponse& OutResponse,
			OperationOwners::UmgWidget::FUmgWidgetCommandError& OutError) override
		{
			(void)Request;
			(void)OutResponse;
			(void)OutError;
			++Calls;
			return false;
		}

		int32 Calls = 0;
	};

	class FAnimationBlueprintService final
		: public OperationOwners::AnimationBlueprint::
			IUeAgentAnimationBlueprintCommandService
	{
	public:
		virtual bool Execute(
			const OperationOwners::AnimationBlueprint::
				FAnimationBlueprintCommandRequest& Request,
			OperationOwners::AnimationBlueprint::
				FAnimationBlueprintCommandResponse& OutResponse,
			OperationOwners::AnimationBlueprint::
				FAnimationBlueprintCommandError& OutError) override
		{
			(void)Request;
			(void)OutResponse;
			(void)OutError;
			++Calls;
			return false;
		}

		int32 Calls = 0;
	};

	bool DecodeRequest(
		const FString& CommandId,
		TOptional<CommandEdge::FUeAgentCommandRequest>& OutRequest)
	{
		const FString Envelope = FString::Printf(
			TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,")
			TEXT("\"message_kind\":\"command_request\",")
			TEXT("\"correlation_id\":\"corr-t37\",")
			TEXT("\"request_id\":\"req-t37\",")
			TEXT("\"command_id\":\"%s\",")
			TEXT("\"command_binding_hash\":\"sha256:")
			TEXT("0000000000000000000000000000000000000000000000000000000000000000\",")
			TEXT("\"params\":{}}"),
			*CommandId);
		FTCHARToUTF8 Encoded(*Envelope);
		TArray<uint8> Bytes;
		Bytes.Append(
			reinterpret_cast<const uint8*>(Encoded.Get()),
			Encoded.Length());
		CommandEdge::FUeAgentCommandProtocolDiagnostic Diagnostic;
		return CommandEdge::FUeAgentCommandProtocolCodec().DecodeRequest(
			Bytes, OutRequest, Diagnostic);
	}

	template <typename ServiceType, typename FactoryType>
	bool VerifyRetiredHandler(
		FAutomationTestBase& Test,
		const TSharedRef<ServiceType>& Service,
		FactoryType Factory,
		const FString& CommandId,
		const FString& ErrorCode)
	{
		TArray<TSharedRef<CommandEdge::IUeAgentExactCommandHandler>> Handlers;
		FString Error;
		if (!Test.TestTrue(
				*FString::Printf(TEXT("%s legacy factory remains fail-closed"),
					*CommandId),
				Factory(Service, Handlers, Error))
			|| !Test.TestEqual(
				TEXT("one compatibility handler is returned"), Handlers.Num(), 1))
		{
			Test.AddError(Error);
			return false;
		}
		TOptional<CommandEdge::FUeAgentCommandRequest> Request;
		if (!Test.TestTrue(TEXT("retired request decodes"),
				DecodeRequest(CommandId, Request))
			|| !Request.IsSet())
		{
			return false;
		}
		const CommandEdge::FUeAgentCommandResult Result = Handlers[0]->Submit(
			Request.GetValue(),
			Contract::FUeAgentCallerContext(
				TEXT("t37"), {TEXT("uai:operate")}, TEXT("corr-t37")),
			MakeShared<FNeverCancelled>());
		Test.TestFalse(TEXT("retired handler never succeeds"), Result.IsOk());
		Test.TestTrue(TEXT("retired handler returns its typed retirement error"),
			Result.GetErrorJson().Contains(ErrorCode));
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT37LegacyHjsonIngressExitTest,
	"UAI.Root.T37.BlueprintUmgGraph.LegacyHjsonIngressExit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT37LegacyHjsonIngressExitTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI;
	using namespace Module::CommandRegistration::Active;

	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(TEXT("active command composition freezes"),
			TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error))
		|| !Registry.IsValid())
	{
		AddError(Error);
		return false;
	}
	TestNotNull(TEXT("the unique public HJSON import remains active"),
		Registry->FindExact(TEXT("content_graph.import_workspace")));
	TestNull(TEXT("old Blueprint HJSON command is absent from the manifest"),
		Registry->FindExact(TEXT("blueprint.authoring.apply")));
	TestNull(TEXT("old UMG HJSON command is absent from the manifest"),
		Registry->FindExact(TEXT("umg.authoring.apply")));
	TestNull(TEXT("old Animation Blueprint HJSON command is absent"),
		Registry->FindExact(TEXT("animation_blueprint.authoring.apply")));

	const TSharedRef<FBlueprintService> BlueprintService =
		MakeShared<FBlueprintService>();
	VerifyRetiredHandler(
		*this,
		BlueprintService,
		CommandHandlers::Blueprint::Surfaces::
			TryCreateBlueprintSurfaceCommandHandlers,
		TEXT("blueprint.authoring.apply"),
		TEXT("blueprint_legacy_authoring_retired"));
	TestEqual(TEXT("retired Blueprint handler bypasses its old service"),
		BlueprintService->Calls, 0);

	const TSharedRef<FUmgService> UmgService = MakeShared<FUmgService>();
	VerifyRetiredHandler(
		*this,
		UmgService,
		CommandHandlers::UmgWidget::TryCreateUmgWidgetCommandHandlers,
		TEXT("umg.authoring.apply"),
		TEXT("umg_legacy_authoring_retired"));
	TestEqual(TEXT("retired UMG handler bypasses its old service"),
		UmgService->Calls, 0);

	const TSharedRef<FAnimationBlueprintService> AnimationService =
		MakeShared<FAnimationBlueprintService>();
	VerifyRetiredHandler(
		*this,
		AnimationService,
		CommandHandlers::AnimationBlueprint::
			TryCreateAnimationBlueprintCommandHandlers,
		TEXT("animation_blueprint.authoring.apply"),
		TEXT("animation_blueprint_legacy_authoring_retired"));
	TestEqual(TEXT("retired Animation Blueprint handler bypasses its old service"),
		AnimationService->Calls, 0);
	return !HasAnyErrors();
}

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

	FString GraphId;
	FString QueryErrorText;
	if (!TestTrue(TEXT("an exact editable graph identity is available"),
		Node::TryGetFirstEditableGraphIdentity(
			*Blueprint, GraphId, QueryErrorText)))
	{
		AddError(QueryErrorText);
		return false;
	}

	Node::FUeAgentBlueprintNodeMechanicsError NodeQueryError;
	FString NodePayloadJson;
	if (!TestTrue(TEXT("the current FInterpTo candidate is queried"),
		Node::TryQueryBlueprintNodeCandidates(
			Target,
			GraphId,
			TEXT("FInterpTo"),
			NodePayloadJson,
			NodeQueryError)))
	{
		AddError(NodeQueryError.Code + TEXT(":") + NodeQueryError.Detail);
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
			+ ComponentQueryError.Detail);
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

	TSharedPtr<FJsonObject> NodeCandidate;
	TSharedPtr<FJsonObject> ComponentCandidate;
	if (!ParseUniqueNamedCandidate(
			NodePayloadJson,
			TEXT("FInterp To"),
			NodeCandidate)
		|| !ParseUniqueNamedCandidate(
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
	FString NodeCandidateId;
	if (!TestTrue(TEXT("the native node candidate has a compact id"),
		NodeCandidate->TryGetStringField(TEXT("id"), NodeCandidateId)))
	{
		return false;
	}
	Node::FUeAgentBlueprintNodeCandidateQuery ExactNodeQuery;
	ExactNodeQuery.Target = Target;
	ExactNodeQuery.GraphId = GraphId;
	ExactNodeQuery.CandidateId = NodeCandidateId;
	FString ExactNodePayloadJson;
	NodeQueryError = {};
	if (!TestTrue(TEXT("the compact id resolves through the public query"),
		Node::TryQueryBlueprintNodeCandidates(
			ExactNodeQuery, ExactNodePayloadJson, NodeQueryError)))
	{
		AddError(NodeQueryError.Code + TEXT(":") + NodeQueryError.Detail);
		return false;
	}
	TSharedPtr<FJsonObject> ExactNodeCandidate;
	if (!ParseFirstCandidate(
			ExactNodePayloadJson,
			TEXT("the compact id returns one exact native action"),
			ExactNodeCandidate))
	{
		return false;
	}
	TestEqual(TEXT("the exact query preserves the compact id"),
		ExactNodeCandidate->GetStringField(TEXT("id")), NodeCandidateId);
	const TSharedRef<FJsonObject> NodeSelector = MakeShared<FJsonObject>();
	NodeSelector->SetStringField(TEXT("id"), NodeCandidateId);
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
	TestEqual(TEXT("the compact candidate id remains exact"),
		NodeSelector->GetStringField(TEXT("id")), NodeCandidateId);
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
	FString NodeCandidateName;
	if (!TestTrue(TEXT("the native node candidate has a direct name selector"),
		NodeCandidate->TryGetStringField(TEXT("name"), NodeCandidateName)))
	{
		return false;
	}
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

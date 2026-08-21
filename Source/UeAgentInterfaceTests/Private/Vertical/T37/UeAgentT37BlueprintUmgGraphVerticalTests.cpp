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
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
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
	if (!TestTrue(TEXT("the current Branch candidate is queried"),
		Node::TryQueryBlueprintNodeCandidates(
			Target,
			GraphId,
			TEXT("branch"),
			NodePayloadJson,
			NodeQueryError)))
	{
		AddError(NodeQueryError.Code + TEXT(":") + NodeQueryError.Detail);
		return false;
	}
	Component::FUeAgentBlueprintComponentMechanicsError ComponentQueryError;
	FString ComponentPayloadJson;
	if (!TestTrue(TEXT("the current PointLight candidate is queried"),
		Component::TryQueryBlueprintComponentCandidates(
			Target,
			TEXT("/Script/Engine.PointLightComponent"),
			TEXT("T37PointLight"),
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

	TSharedPtr<FJsonObject> NodeCandidate;
	TSharedPtr<FJsonObject> ComponentCandidate;
	if (!ParseFirstCandidate(
			NodePayloadJson,
			TEXT("the node candidate payload is exact"),
			NodeCandidate)
		|| !ParseFirstCandidate(
			ComponentPayloadJson,
			TEXT("the component candidate payload is exact"),
			ComponentCandidate))
	{
		return false;
	}

	const TSharedRef<FJsonObject> ComponentSpec = MakeShared<FJsonObject>();
	ComponentSpec->SetStringField(TEXT("operation"), TEXT("create"));
	ComponentSpec->SetObjectField(TEXT("candidate"), ComponentCandidate);
	ComponentSpec->SetStringField(TEXT("parent"), TEXT("DefaultSceneRoot"));
	const TSharedRef<FJsonObject> Components = MakeShared<FJsonObject>();
	Components->SetObjectField(TEXT("component:T37PointLight"), ComponentSpec);

	const TSharedRef<FJsonObject> NodeSpec = MakeShared<FJsonObject>();
	NodeSpec->SetStringField(TEXT("operation"), TEXT("create"));
	NodeSpec->SetObjectField(TEXT("candidate"), NodeCandidate);
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
	TestTrue(TEXT("the component candidate name is expanded server-side"),
		ComponentSpec->TryGetStringField(TEXT("name"), Normalized));
	TestEqual(TEXT("the component name remains exact"),
		Normalized, FString(TEXT("T37PointLight")));
	TestTrue(TEXT("the component candidate class is expanded server-side"),
		ComponentSpec->TryGetStringField(TEXT("class"), Normalized));
	TestEqual(TEXT("the component class remains exact"),
		Normalized, FString(TEXT("/Script/Engine.PointLightComponent")));
	TestTrue(TEXT("the node candidate kind is expanded server-side"),
		NodeSpec->TryGetStringField(TEXT("node_type"), Normalized));
	TestEqual(TEXT("the normalized node kind is native"),
		Normalized, FString(TEXT("node_by_class")));
	TestTrue(TEXT("the node class is the exact queried identity"),
		NodeSpec->TryGetStringField(TEXT("node_class"), Normalized));
	TestEqual(TEXT("the Branch class remains exact"),
		Normalized, FString(TEXT("/Script/BlueprintGraph.K2Node_IfThenElse")));

	const TSharedRef<FJsonObject> StaleCandidate = MakeShared<FJsonObject>();
	StaleCandidate->Values = NodeCandidate->Values;
	StaleCandidate->SetStringField(TEXT("binding_revision"), TEXT("sha256:stale"));
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
	TestFalse(TEXT("a stale candidate binding is rejected before mutation"),
		Surfaces::TryNormalizeBlueprintCandidateBindings(
			Target, StaleExtension, WorkspaceError));
	TestEqual(TEXT("the stale binding error is typed"),
		WorkspaceError.GetCode(),
		FString(TEXT("blueprint_surface_node_candidate_binding_stale")));
	return !HasAnyErrors();
}

#include "Misc/AutomationTest.h"

#if __has_include("DomainAdapters/Blueprint/Component/UeAgentBlueprintComponentMechanics.h") \
	&& __has_include("OperationOwners/BlueprintComponent/UeAgentBlueprintComponentCommandService.h") \
	&& __has_include("Module/CommandRegistration/BlueprintComponent/UeAgentBlueprintComponentRegistration.h")
#define UAI_T12_BLUEPRINT_COMPONENT_AVAILABLE 1
#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "Commands/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "DomainAdapters/Blueprint/Component/UeAgentBlueprintComponentMechanics.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Module/CommandRegistration/BlueprintComponent/UeAgentBlueprintComponentRegistration.h"
#include "OperationOwners/BlueprintComponent/UeAgentBlueprintComponentCommandService.h"
#else
#define UAI_T12_BLUEPRINT_COMPONENT_AVAILABLE 0
#endif

#if UAI_T12_BLUEPRINT_COMPONENT_AVAILABLE
namespace
{
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::DomainAdapters::Blueprint::Component;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::Module::CommandRegistration::BlueprintComponent;
	using namespace UE::UAI::OperationOwners::BlueprintComponent;

	class FNeverCancelled final : public IUeAgentCancellationSignal
	{
	public:
		virtual bool IsCancelled() const override { return false; }
	};

	class FFixtureService final : public IUeAgentBlueprintComponentCommandService
	{
	public:
		virtual bool Execute(
			const FUeAgentBlueprintComponentCommandRequest& Request,
			FUeAgentBlueprintComponentCommandResponse& OutResponse,
			FUeAgentBlueprintComponentCommandError& OutError) override
		{
			++Calls;
			LastRequest = Request;
			OutResponse = {};
			OutError = {};
			OutResponse.PayloadJson = TEXT(
				"{\"command_id\":\"blueprint.component.candidates\"," 
				"\"read_only\":true,\"candidates\":[]}");
			OutResponse.bReadOnly = true;
			return true;
		}

		int32 Calls = 0;
		FUeAgentBlueprintComponentCommandRequest LastRequest;
	};

	bool DecodeRequest(
		const FString& ContractHash,
		TOptional<FUeAgentCommandRequest>& OutRequest)
	{
		const FString Envelope = FString::Printf(
			TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,")
			TEXT("\"message_kind\":\"command_request\",")
			TEXT("\"correlation_id\":\"corr-t12\",")
			TEXT("\"request_id\":\"req-t12\",")
			TEXT("\"command_id\":\"blueprint.component.candidates\",")
			TEXT("\"command_binding_hash\":\"%s\",")
			TEXT("\"params\":{\"target\":\"/Game/T12.BP_T12\",")
			TEXT("\"parent\":\"DefaultSceneRoot\",")
			TEXT("\"query\":\"Point Light\",\"limit\":10}}"),
			*ContractHash);
		FTCHARToUTF8 Encoded(*Envelope);
		TArray<uint8> Bytes;
		Bytes.Append(
			reinterpret_cast<const uint8*>(Encoded.Get()),
			Encoded.Length());
		FUeAgentCommandProtocolDiagnostic Diagnostic;
		return FUeAgentCommandProtocolCodec().DecodeRequest(
			Bytes, OutRequest, Diagnostic);
	}
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T12BlueprintComponentTest,
	"UAI.StrictV2.T12.BLUEPRINT.COMPONENT",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T12BlueprintComponentTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T12_BLUEPRINT_COMPONENT_AVAILABLE
	TestTrue(TEXT("T12 Blueprint Component authority is available"), false);
	return false;
#else
	FWorkspaceError WorkspaceError;
	TSharedPtr<const FImportInitializationDomainProviderRegistry>
		ProviderRegistry;
	if (!TestTrue(
		TEXT("the process provider catalog freezes with T12"),
		TryFreezeProcessImportInitializationDomainProviders(
			ProviderRegistry, WorkspaceError))
		|| !ProviderRegistry.IsValid())
	{
		AddError(WorkspaceError.GetCode());
		return false;
	}

	FImportInitializationDomainResolution RootResolution;
	TestFalse(
		TEXT("the retired component-only schema has no runtime provider"),
		ProviderRegistry->TryResolve(
			FImportInitializationDomainRequest(
				TEXT("content_graph.import_workspace"),
				TEXT("uai.blueprint.component.edit/1"),
				TEXT("/Game/UAI/T12/BP_Component.BP_Component"),
				TEXT("/Script/Engine.Blueprint"),
				EInitializationNodeRole::RootTarget,
				TEXT("{}")),
			RootResolution,
			WorkspaceError));
	TestFalse(
		TEXT("the retired component-only resolution is invalid"),
		RootResolution.IsValid());
	TestEqual(
		TEXT("the retired component-only schema fails at provider authority"),
		WorkspaceError.GetCode(),
		FString(TEXT("hjson_domain_provider_unresolved")));

	WorkspaceError.Reset();
	TestTrue(
		TEXT("the unified Blueprint root remains the sole write provider"),
		ProviderRegistry->TryResolve(
			FImportInitializationDomainRequest(
				TEXT("content_graph.import_workspace"),
				TEXT("uai.blueprint.edit/1"),
				TEXT("/Game/UAI/T12/BP_Component.BP_Component"),
				TEXT("/Script/Engine.Blueprint"),
				EInitializationNodeRole::RootTarget,
				TEXT("{}")),
			RootResolution,
			WorkspaceError));
	if (RootResolution.IsValid())
	{
		TestEqual(
			TEXT("the unified Blueprint provider identity is exact"),
			RootResolution.GetProvider()->GetProviderId(),
			FString(TEXT("blueprint-surface-domain-provider")));
		TestEqual(
			TEXT("the unified Blueprint root contract is exact"),
			RootResolution.GetInitializationContractId(),
			FString(TEXT("uai.init.blueprint.asset/1")));
	}

	UeAgentPaletteSearch::FCandidate NativeAudioCandidate;
	FString NativeAudioError;
	TestTrue(
		TEXT("the native Add Component registry independently exposes AudioComponent"),
		UeAgentPaletteSearch::TryResolveActorComponentCandidate(
			TEXT("/Script/Engine.AudioComponent"),
			FString(),
			NativeAudioCandidate,
			NativeAudioError));
	if (NativeAudioCandidate.Identity.IsValid())
	{
		TestEqual(
			TEXT("the native AudioComponent selector keeps its exact class"),
			NativeAudioCandidate.AuthoringSelectors.FindRef(
				TEXT("component_class")),
			FString(TEXT("/Script/Engine.AudioComponent")));
	}

	const FString FixtureSuffix =
		FGuid::NewGuid().ToString(EGuidFormats::Digits);
	UBlueprint* CandidateBlueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), GetTransientPackage(),
		*FString(TEXT("BP_T12_Candidates_") + FixtureSuffix), BPTYPE_Normal,
		UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(),
		TEXT("T12"));
	if (!TestNotNull(TEXT("a transient Actor Blueprint candidate fixture is created"),
		CandidateBlueprint))
	{
		return false;
	}
	const FString CandidateTarget = CandidateBlueprint->GetPathName();
	FUeAgentBlueprintComponentCandidateQuery CandidateQuery;
	CandidateQuery.Target = CandidateTarget;
	CandidateQuery.Parent = TEXT("DefaultSceneRoot");
	CandidateQuery.Query = TEXT("Point Light");
	CandidateQuery.Limit = 10;
	FString CandidatePayloadJson;
	FUeAgentBlueprintComponentMechanicsError MechanicsError;
	if (!TestTrue(TEXT("the native Add Component registry query succeeds"),
		TryQueryBlueprintComponentCandidates(
			CandidateQuery,
			CandidatePayloadJson,
			MechanicsError)))
	{
		AddError(MechanicsError.Code + TEXT(":") + MechanicsError.Detail);
		return false;
	}
	TSharedPtr<FJsonObject> CandidatePayload;
	FString JsonError;
	if (!TestTrue(TEXT("the native Component candidate payload is valid JSON"),
		UE::UAI::CommandContract::Json::TryParseObject(
			CandidatePayloadJson, CandidatePayload, JsonError)
			&& CandidatePayload.IsValid()))
	{
		AddError(JsonError);
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Candidates = nullptr;
	TestTrue(TEXT("the bounded query returns a native candidate page"),
		CandidatePayload->TryGetArrayField(TEXT("candidates"), Candidates)
		&& Candidates && Candidates->Num() >= 1);
	bool bFoundPointLight = false;
	if (Candidates)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Candidates)
		{
			const TSharedPtr<FJsonObject> Candidate =
				Value.IsValid() && Value->Type == EJson::Object
				? Value->AsObject()
				: nullptr;
			if (!Candidate.IsValid()
				|| Candidate->GetStringField(TEXT("class_path"))
					!= TEXT("/Script/Engine.PointLightComponent"))
			{
				continue;
			}
			bFoundPointLight = true;
			TestFalse(TEXT("the public candidate has a compact id"),
				Candidate->GetStringField(TEXT("id")).IsEmpty());
			TestFalse(TEXT("the public candidate has a direct name selector"),
				Candidate->GetStringField(TEXT("name")).IsEmpty());
			TestFalse(TEXT("the public candidate omits legacy binding state"),
				Candidate->HasField(TEXT("binding_revision")));
		}
	}
	TestTrue(TEXT("Point Light is present in the real Add Component registry"),
		bFoundPointLight);
	const TSharedPtr<FJsonObject>* Context = nullptr;
	TestTrue(TEXT("the query returns its exact outer parent context"),
		CandidatePayload->TryGetObjectField(TEXT("context"), Context)
		&& Context && Context->IsValid()
		&& (*Context)->GetStringField(TEXT("parent"))
			== TEXT("DefaultSceneRoot"));

	const TSharedRef<FFixtureService> Service = MakeShared<FFixtureService>();
	TSharedPtr<const UE::UAI::CommandRegistry::FUeAgentImmutableCommandRegistry>
		CommandRegistry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(
		TEXT("the isolated T12 contribution composes"),
		TryCreateBlueprintComponentRegistration(
			Service, CommandRegistry, Dispatch, Error))
		|| !CommandRegistry.IsValid() || !Dispatch.IsValid())
	{
		AddError(Error);
		return false;
	}
	const auto* Contract = CommandRegistry->FindExact(
		TEXT("blueprint.component.candidates"));
	if (!TestNotNull(TEXT("the exact Component candidate contract exists"), Contract))
	{
		return false;
	}
	TestEqual(
		TEXT("the Component candidate contract has one owner"),
		Contract->GetPublicOwner(),
		FString(TEXT("blueprint-component-authoring-owner")));

	TOptional<FUeAgentCommandRequest> Request;
	if (!TestTrue(
		TEXT("the exact T12 request decodes"),
		DecodeRequest(Contract->GetContractHash(), Request))
		|| !Request.IsSet())
	{
		return false;
	}
	const FUeAgentCommandResult Result = Dispatch->Submit(
		Request.GetValue(),
		FUeAgentCallerContext(
			TEXT("strict-v2-t12"),
			{TEXT("uai:query")},
			TEXT("corr-t12")),
		MakeShared<FNeverCancelled>());
	TestTrue(TEXT("the exact T12 fixture dispatch succeeds"), Result.IsOk());
	TestEqual(TEXT("the T12 fixture service is called once"), Service->Calls, 1);
	TestEqual(
		TEXT("the native component search context crosses the exact edge unchanged"),
		Service->LastRequest.CanonicalParamsJson,
		FString(TEXT(
			"{\"limit\":10,\"parent\":\"DefaultSceneRoot\","
			"\"query\":\"Point Light\","
			"\"target\":\"/Game/T12.BP_T12\"}")));
	return true;
#endif
}

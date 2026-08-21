#include "Misc/AutomationTest.h"

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundActivation.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundBlueprintMaterialRoutes.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/DomainCapability/OfficialPluginAvailability/UeAgentOfficialPluginAvailability.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "OperationOwners/MaterialCommands/UeAgentMaterialCommandOperationOwner.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	using namespace UE::UAI;
	using namespace CommandEdge;
	using namespace CommandRegistry;

	class FNeverCancelled final : public Contract::IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override { return false; }
	};

	FString Serialize(const TSharedRef<FJsonObject>& Object)
	{
		FString Text;
		const TSharedRef<TJsonWriter<TCHAR,
			TCondensedJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<TCHAR,
					TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Object, Writer);
		return Text;
	}

	FUeAgentCommandResult SubmitPublic(
		const TSharedPtr<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const FUeAgentExactCommandDispatch>& Dispatch,
		const FString& CommandId,
		const TSharedRef<FJsonObject>& Params,
		TArray<FString> AuthorityScopes)
	{
		const auto* const Contract = Registry.IsValid()
			? Registry->FindExact(CommandId)
			: nullptr;
		if (!Contract || !Dispatch.IsValid())
		{
			return FUeAgentCommandResult::ProtocolError(
				TEXT("t38-public"),
				TEXT("t38-public"),
				CommandId,
				TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"),
				TEXT("t38_public_contract_missing"),
				CommandId);
		}
		const FString RequestId = TEXT("t38-public-")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString Envelope = FString::Printf(
			TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,")
			TEXT("\"message_kind\":\"command_request\",")
			TEXT("\"correlation_id\":\"t38-public\",")
			TEXT("\"request_id\":\"%s\",\"command_id\":\"%s\",")
			TEXT("\"command_binding_hash\":\"%s\",\"params\":%s}"),
			*RequestId,
			*CommandId,
			*Contract->GetContractHash(),
			*Serialize(Params));
		FTCHARToUTF8 Encoded(*Envelope);
		TArray<uint8> Bytes;
		Bytes.Append(
			reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
		TOptional<FUeAgentCommandRequest> Request;
		FUeAgentCommandProtocolDiagnostic Diagnostic;
		if (!FUeAgentCommandProtocolCodec().DecodeRequest(
				Bytes, Request, Diagnostic)
			|| !Request.IsSet())
		{
			return FUeAgentCommandResult::ProtocolError(
				TEXT("t38-public"),
				RequestId,
				CommandId,
				Contract->GetContractHash(),
				Diagnostic.Code,
				Diagnostic.Message);
		}
		return Dispatch->Submit(
			Request.GetValue(),
			Contract::FUeAgentCallerContext(
				TEXT("t38-public"),
				MoveTemp(AuthorityScopes),
				TEXT("t38-public")),
			MakeShared<FNeverCancelled>());
	}

	ContextResource::FUeAgentResourceTracker MakeResources()
	{
		return ContextResource::FUeAgentResourceTracker(
			ContextResource::FUeAgentResourceBudget({
				{ContextResource::EUeAgentResourceDimension::Nodes, 256},
				{ContextResource::EUeAgentResourceDimension::Items, 1024},
				{ContextResource::EUeAgentResourceDimension::StringBytes,
					1024 * 1024},
				{ContextResource::EUeAgentResourceDimension::WorkUnits, 4096},
				{ContextResource::EUeAgentResourceDimension::EstimatedMemoryBytes,
					4 * 1024 * 1024},
			}));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT38AssetFactoryCandidatePublicQueryTest,
	"UAI.Root.T38.AssetMaterialTextureMedia.AssetFactoryCandidatePublicQuery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT38AssetFactoryCandidatePublicQueryTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI;
	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(
			TEXT("the process active command composition freezes"),
			Module::CommandRegistration::Active::
				TryComposeProcessActiveCommandComposition(
					Registry, Dispatch, Error))
		|| !Registry.IsValid() || !Dispatch.IsValid())
	{
		AddError(Error);
		return false;
	}
	const auto* Contract = Registry->FindExact(TEXT("asset.factory.candidates"));
	if (!TestNotNull(
			TEXT("the factory candidate query is active"), Contract))
	{
		return false;
	}
	TestEqual(
		TEXT("the query has one stable public owner"),
		Contract->GetPublicOwner(),
		FString(TEXT("asset-simple-query-owner")));

	const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(
		TEXT("create_command"), TEXT("asset.media_source.create"));
	Params->SetNumberField(TEXT("limit"), 20);
	const FUeAgentCommandResult Result = SubmitPublic(
		Registry,
		Dispatch,
		TEXT("asset.factory.candidates"),
		Params,
		{TEXT("uai:query")});
	if (!TestTrue(
			TEXT("the public candidate query succeeds"), Result.IsOk()))
	{
		AddError(Result.GetErrorJson());
		return false;
	}
	TSharedPtr<FJsonObject> Payload;
	if (!TestTrue(
			TEXT("the public candidate payload is JSON"),
			FJsonSerializer::Deserialize(
				TJsonReaderFactory<>::Create(Result.GetPayloadJson()), Payload))
		|| !Payload.IsValid())
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Candidates = nullptr;
	if (!TestTrue(
			TEXT("the media source query returns real candidates"),
			Payload->TryGetArrayField(TEXT("candidates"), Candidates)
				&& Candidates && !Candidates->IsEmpty()))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Candidate = (*Candidates)[0]->AsObject();
	const TSharedPtr<FJsonObject>* FactoryRef = nullptr;
	const TSharedPtr<FJsonObject>* Revision = nullptr;
	FString CandidateRef;
	FString SourceId;
	FString Generation;
	double Sequence = -1.0;
	TestTrue(
		TEXT("factory_ref is directly consumable by the create command"),
		Candidate.IsValid()
			&& Candidate->TryGetObjectField(TEXT("factory_ref"), FactoryRef)
			&& FactoryRef && FactoryRef->IsValid()
			&& (*FactoryRef)->TryGetStringField(
				TEXT("candidate_ref"), CandidateRef)
			&& !CandidateRef.IsEmpty()
			&& (*FactoryRef)->TryGetObjectField(
				TEXT("catalog_revision"), Revision)
			&& Revision && Revision->IsValid()
			&& (*Revision)->TryGetStringField(TEXT("source_id"), SourceId)
			&& (*Revision)->TryGetStringField(TEXT("generation"), Generation)
			&& (*Revision)->TryGetNumberField(TEXT("sequence"), Sequence)
			&& !SourceId.IsEmpty() && !Generation.IsEmpty() && Sequence >= 0.0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT38MaterialFunctionCreateContractTest,
	"UAI.Root.T38.AssetMaterialTextureMedia.MaterialFunctionCreateContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT38MaterialFunctionCreateContractTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI;
	using namespace Composition::GraphCompound;
	const TArray<FUeAgentGraphCompoundActivationProfile>& Profiles =
		GetGraphCompoundActivationProfiles();
	const FUeAgentGraphCompoundActivationProfile* Activation =
		Profiles.FindByPredicate(
			[](const FUeAgentGraphCompoundActivationProfile& Profile)
			{
				return Profile.GetCommandId()
					== TEXT("material.asset.create");
			});
	if (!TestNotNull(
			TEXT("material.asset.create activation exists"), Activation))
	{
		return false;
	}
	const FGraphCompoundDomainRouteProfile* Route =
		FindBlueprintMaterialGraphRouteProfile(Activation->GetCommandId());
	if (!TestNotNull(
			TEXT("the material create route resolves"), Route))
	{
		return false;
	}
	FGraphCompoundRouteError RouteError;
	auto Resources = MakeResources();
	FGraphCompoundAssetCreateRequest Request;
	TestTrue(
		TEXT("material_function is accepted by the existing create command"),
		TryDecodeGraphCompoundAssetCreateRequest(
			*Route,
			Contract::FUeAgentContractValue(
				TEXT("{\"asset_name\":\"MF_T38\",\"material_kind\":\"material_function\",\"package_path\":\"/Game/UAI_Automation/T38\"}")),
			Resources,
			Request,
			RouteError));
	TestEqual(
		TEXT("the typed request preserves the material function discriminator"),
		Request.GetMaterialKind(),
		FString(TEXT("material_function")));

	auto InvalidResources = MakeResources();
	FGraphCompoundAssetCreateRequest Invalid;
	RouteError = {};
	TestFalse(
		TEXT("unknown material kinds fail before mutation"),
		TryDecodeGraphCompoundAssetCreateRequest(
			*Route,
			Contract::FUeAgentContractValue(
				TEXT("{\"asset_name\":\"MF_T38\",\"material_kind\":\"unknown\",\"package_path\":\"/Game/UAI_Automation/T38\"}")),
			InvalidResources,
			Invalid,
			RouteError));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT38SharedOwnersAndPluginAvailabilityTest,
	"UAI.Root.T38.AssetMaterialTextureMedia.SharedOwnersAndPluginAvailability",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT38SharedOwnersAndPluginAvailabilityTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI;
	using namespace DomainCapability;
	using namespace DomainCapability::OfficialPluginAvailability;
	TestTrue(
		TEXT("installed and enabled is available"),
		Evaluate(TEXT("Plugin"), true, true).IsAvailable());
	TestEqual(
		TEXT("installed but disabled remains precise"),
		Evaluate(TEXT("Plugin"), true, false).GetReason(),
		FString(TEXT("disabled")));
	TestEqual(
		TEXT("missing remains precise"),
		Evaluate(TEXT("Plugin"), false, false).GetReason(),
		FString(TEXT("not_installed")));

	TSharedPtr<const FFrozenDomainCapabilitySet> Frozen;
	FString Error;
	if (!TestTrue(
			TEXT("the process domain capability set freezes"),
			TryFreezeProcessDomainCapabilities(Frozen, Error))
		|| !Frozen.IsValid())
	{
		AddError(Error);
		return false;
	}
	TestTrue(
		TEXT("AssetSimple participates in the common operation authority"),
		Frozen->FindOperationFactory(TEXT("asset.media_source.create")).IsValid());
	TestTrue(
		TEXT("Material participates in the common operation authority"),
		Frozen->FindOperationFactory(TEXT("material.asset.create")).IsValid());

	TSharedPtr<CommandEdge::IUeAgentCommandHandler> First;
	TSharedPtr<CommandEdge::IUeAgentCommandHandler> Second;
	TestTrue(
		TEXT("the Material mechanics owner initializes"),
		OperationOwners::MaterialCommands::TryResolveMechanicsHandler(
			TEXT("material.asset.create"), First, Error));
	TestTrue(
		TEXT("the Material mechanics owner resolves again"),
		OperationOwners::MaterialCommands::TryResolveMechanicsHandler(
			TEXT("material.asset.create"), Second, Error));
	TestTrue(
		TEXT("public and HJSON paths share one process-owned mechanics handler"),
		First.IsValid() && First == Second);
	return !HasAnyErrors();
}

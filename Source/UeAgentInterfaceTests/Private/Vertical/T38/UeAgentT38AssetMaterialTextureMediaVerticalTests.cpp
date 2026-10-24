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
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectCapabilities.h"
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
		return ContextResource::FUeAgentResourceTracker();
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
	TestTrue(
		TEXT("the tutorial exposes name selector"),
		Contract->GetPurpose().ResultGuidance.Contains(
			TEXT("returned name")));
	TestTrue(
		TEXT("the tutorial exposes semantic selector"),
		Contract->GetPurpose().ResultGuidance.Contains(
			TEXT("short semantic selection")));
	TestTrue(
		TEXT("the generic create command is an explicit related flow"),
		Contract->GetPurpose().RelatedFlows.Contains(
			TEXT("asset.object.create")));

	const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(
		TEXT("create_command"), TEXT("asset.object.create"));
	Params->SetStringField(TEXT("query"), TEXT("Material"));
	Params->SetNumberField(TEXT("limit"), 100);
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
	TestEqual(TEXT("names-only result has one field"), Payload->Values.Num(), 1);
	TestTrue(TEXT("names-only result contains names"), Payload->HasField(TEXT("names")));
	const TArray<TSharedPtr<FJsonValue>>* Names = nullptr;
	if (!TestTrue(
			TEXT("the factory query returns names"),
			Payload->TryGetArrayField(TEXT("names"), Names)
				&& Names && !Names->IsEmpty()))
	{
		return false;
	}
	TestTrue(TEXT("factory names are strings"), (*Names)[0]->Type == EJson::String);
	// The production factory parser normalizes both public forms through DomainKit.
	FString ParsedName, ParseError;
	TMap<FString, FString> ParsedSelect;
	const auto ParseFactory = [&](const TSharedPtr<FJsonValue>& Factory)
	{
		FJsonObject Input;
		Input.SetField(TEXT("factory"), Factory);
		return DomainAdapters::AssetDataSimpleEffect::TryParseAssetDataSimpleEffectFactorySelector(
			Input, true, ParsedName, ParsedSelect, ParseError);
	};
	TestTrue(TEXT("factory string uses shared name normalization"), ParseFactory(MakeShared<FJsonValueString>(TEXT(" Input Action "))));
	TestEqual(TEXT("factory shorthand name is normalized"), ParsedName, FString(TEXT("Input Action")));
	const auto Selector = MakeShared<FJsonObject>();
	Selector->SetStringField(TEXT("name"), TEXT(" Input Action "));
	const auto Selection = MakeShared<FJsonObject>();
	Selection->SetStringField(TEXT("creation"), TEXT("constructor"));
	Selector->SetObjectField(TEXT("select"), Selection);
	TestTrue(TEXT("factory object uses shared selector rules"), ParseFactory(MakeShared<FJsonValueObject>(Selector)));
	TestEqual(TEXT("factory object name matches shorthand"), ParsedName, FString(TEXT("Input Action")));
	TestEqual(TEXT("factory select is retained"), ParsedSelect.FindRef(TEXT("creation")), FString(TEXT("constructor")));
	Selector->SetStringField(TEXT("select"), TEXT("constructor"));
	TestFalse(TEXT("factory rejects a nonobject select"), ParseFactory(MakeShared<FJsonValueObject>(Selector)));
	TestEqual(TEXT("factory uses shared select error"), ParseError, FString(TEXT("domain_hjson_candidate_selector_select_invalid")));
	Selector->SetObjectField(TEXT("select"), Selection);
	Selection->SetNumberField(TEXT("creation"), 1);
	TestFalse(TEXT("factory rejects a nonstring select value"), ParseFactory(MakeShared<FJsonValueObject>(Selector)));
	TestEqual(TEXT("factory uses shared select value error"), ParseError, FString(TEXT("domain_hjson_candidate_selector_select_invalid")));
	Selector->RemoveField(TEXT("select"));
	Selector->SetStringField(TEXT("identity"), TEXT("internal"));
	TestFalse(TEXT("factory rejects machine identity fields"), ParseFactory(MakeShared<FJsonValueObject>(Selector)));
	TestEqual(TEXT("factory uses shared unsupported field error"), ParseError, FString(TEXT("domain_hjson_candidate_selector_field_unsupported")));
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
	const TArray<FUeAgentGraphCompoundActivationProfile>* CachedGraphProfiles = nullptr;
	UE::UAI::Contract::FDiagnostic ProfileError;
	if (!TryGetGraphCompoundActivationProfiles(CachedGraphProfiles, ProfileError))
	{
		AddError(ProfileError.PublicMessage);
		return false;
	}
	const auto& Profiles = *CachedGraphProfiles;
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
		Frozen->FindOperationFactory(TEXT("asset.object.create")).IsValid());
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

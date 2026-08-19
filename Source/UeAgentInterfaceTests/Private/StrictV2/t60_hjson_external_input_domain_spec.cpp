#include "Misc/AutomationTest.h"

#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "DomainAdapters/HjsonExternalInput/UeAgentHjsonExternalInputDomain.h"
#include "OperationOwners/HjsonExternalInput/UeAgentHjsonExternalInputProvider.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T60HjsonExternalInputDomainTest,
	"UAI.StrictV2.T60.HJSON.EXTERNAL.INPUT.DOMAIN.ContractsAndDelegation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T60HjsonExternalInputDomainTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::DomainAdapters::HjsonExternalInput;
	using namespace UE::UAI::OperationOwners::HjsonExternalInput;

	const TArray<FHjsonExternalInputFamilyContract>& Contracts =
		GetHjsonExternalInputFamilyContracts();
	TestEqual(TEXT("factory, skin-weight and localization are explicit families"),
		Contracts.Num(), 3);
	TSet<FString> Commands;
	for (const FHjsonExternalInputFamilyContract& Contract : Contracts)
	{
		TestTrue(TEXT("each family contract is valid"), Contract.IsValid());
		for (const FString& CommandId : Contract.ImporterCommandIds)
		{
			TestFalse(TEXT("an importer command has one family disposition"),
				Commands.Contains(CommandId));
			Commands.Add(CommandId);
		}
	}
	TestEqual(TEXT("all T28-T30 importer commands have one disposition"),
		Commands.Num(), 13);
	TestTrue(TEXT("factory texture import is delegated"),
		Commands.Contains(TEXT("external.texture.import")));
	TestTrue(TEXT("skin weight import is delegated"),
		Commands.Contains(TEXT("external.skin_weight_profile.import")));
	TestTrue(TEXT("localization import is delegated"),
		Commands.Contains(TEXT("external.localization.po.import")));

	class FGateway final : public IHjsonExternalInputImporterGateway
	{
	public:
		virtual bool Execute(
			const FString& CommandId,
			const FString& CanonicalParamsJson,
			FString& OutPayloadJson,
			UE::UAI::HjsonWorkspace::FWorkspaceError& OutError) override
		{
			ObservedCommandId = CommandId;
			ObservedParamsJson = CanonicalParamsJson;
			OutPayloadJson = TEXT("{\"terminal_status\":\"succeeded\"}");
			OutError.Reset();
			return true;
		}

		FString ObservedCommandId;
		FString ObservedParamsJson;
	};

	const TSharedRef<FGateway> Gateway = MakeShared<FGateway>();
	TSharedPtr<UE::UAI::HjsonWorkspace::IImportInitializationDomainProvider>
		Provider;
	UE::UAI::HjsonWorkspace::FWorkspaceError Error;
	TestTrue(TEXT("the T60 provider composes against an importer gateway"),
		TryCreateHjsonExternalInputDomainProvider(Gateway, Provider, Error));
	TestTrue(TEXT("the T60 provider exists"), Provider.IsValid());
	if (!Provider.IsValid())
	{
		return false;
	}

	const FString CanonicalParams = TEXT(
		"{\"target\":\"/Game/UAI_StrictV2/T60_Texture.T60_Texture\"," 
		"\"source\":{\"kind\":\"resolved\","
		"\"target_identity\":\"/Game/UAI_StrictV2/T60_Texture.T60_Texture\","
		"\"entry_file\":\"inline.hjson\",\"documents\":[{"
		"\"path\":\"inline.hjson\",\"document\":\"{schema:'uai.hjson.external_input/1',root:{"
		"type:'uai.external_input.factory',external_input:{"
		"source:{identity:'D:/fixtures/t60.png',format:'png',"
		"content_sha256:'sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'},"
		"options:{replace_existing:false},target_policy:{"
		"command_id:'external.texture.import',parameters:{"
		"destination_ref:'/Game/UAI_StrictV2/T60_Texture.T60_Texture'}}}}}\"}]},"
		"\"expected_revision\":\"absent\",\"authorization_id\":\"auth:t60\"}");
	const UE::UAI::HjsonWorkspace::FImportInitializationDomainRequest Request(
		TEXT("content_graph.import_workspace"),
		TEXT("uai.hjson.external_input/1"),
		TEXT("/Game/UAI_StrictV2/T60_Texture.T60_Texture"),
		TEXT("uai.external_input.factory"),
		UE::UAI::HjsonWorkspace::EInitializationNodeRole::RootTarget,
		CanonicalParams);
	TestTrue(TEXT("the external-input schema selects T60"),
		Provider->Matches(Request));
	FString InitializationContractId;
	TestTrue(TEXT("the importer-owned normal creation contract resolves"),
		Provider->TryResolveInitializationContract(
			Request, InitializationContractId, Error));
	TestEqual(TEXT("factory creation has one initialization contract"),
		InitializationContractId,
		FString(TEXT("uai.init.hjson_external_input.factory_importer/1")));
	UE::UAI::HjsonWorkspace::FImportInitializationDomainResponse Response;
	TestTrue(TEXT("the provider delegates through the formal importer gateway"),
		Provider->TryExecute(Request, InitializationContractId, Response, Error));
	TestEqual(TEXT("the exact importer command is retained"),
		Gateway->ObservedCommandId, FString(TEXT("external.texture.import")));
	TestTrue(TEXT("the source identity reaches the delegated change set"),
		Gateway->ObservedParamsJson.Contains(TEXT("D:/fixtures/t60.png")));
	return !HasAnyErrors();
}

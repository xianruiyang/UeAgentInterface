#include "Misc/AutomationTest.h"

#if __has_include("DomainAdapters/Query/UeAgentQueryService.h") \
	&& __has_include("Module/CommandRegistration/Query/UeAgentQueryRegistration.h") \
	&& __has_include("Composition/ReadExportCommand/UeAgentReadExportCommandHandlers.h")
#define UAI_T06_NON_HJSON_QUERY_AVAILABLE 1
#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "Composition/ReadExportCommand/UeAgentReadExportCommandHandlers.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "DomainAdapters/Query/UeAgentQueryService.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "GameFramework/Actor.h"
#include "Module/CommandRegistration/Query/UeAgentQueryRegistration.h"
#else
#define UAI_T06_NON_HJSON_QUERY_AVAILABLE 0
#endif

#if UAI_T06_NON_HJSON_QUERY_AVAILABLE
namespace
{
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::CommandRegistry;
	using namespace UE::UAI::Composition::ReadExportCommand;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::DomainAdapters::Query;
	using namespace UE::UAI::Module::CommandRegistration::Query;

	const TArray<FString> ExpectedCommandIds = {
		TEXT("asset.collection"),
		TEXT("asset.properties"),
		TEXT("asset.registry_record"),
		TEXT("asset.registry_relations"),
		TEXT("asset.relations"),
		TEXT("asset.sample"),
		TEXT("asset.summary"),
		TEXT("asset.validation"),
		TEXT("config.resolved_value"),
		TEXT("config.section"),
		TEXT("editor.dirty_resources"),
		TEXT("editor.open_assets"),
		TEXT("landscape.height.read"),
		TEXT("project.platform"),
		TEXT("project.settings"),
		TEXT("project.summary"),
		TEXT("project.validation"),
		TEXT("reference.asset_read"),
		TEXT("reference.origin.resolve"),
		TEXT("world.actor_components"),
		TEXT("world.actors"),
		TEXT("world.capsule_path"),
		TEXT("world.capsule_sweep"),
		TEXT("world.content"),
		TEXT("world.data_layers"),
		TEXT("world.hlod_assignments"),
		TEXT("world.mesh_closest_vertex"),
		TEXT("world.mesh_vertex_position"),
		TEXT("world.nav_connectivity"),
		TEXT("world.nav_find_path"),
		TEXT("world.nav_project_point"),
		TEXT("world.nearby_shapes"),
		TEXT("world.overlap"),
		TEXT("world.partition"),
		TEXT("world.partition_health"),
		TEXT("world.physics_component"),
		TEXT("world.physics_constraint"),
		TEXT("world.property"),
		TEXT("world.ray_trace"),
		TEXT("world.selection"),
		TEXT("world.streaming_levels"),
		TEXT("world.summary"),
	};

	const TMap<FString, FString> HistoricalSuccessors = {
		{TEXT("actor_list_components"), TEXT("content_graph.items")},
		{TEXT("asset_reference_graph"), TEXT("asset.registry_relations")},
		{TEXT("config_export_section"), TEXT("config.section")},
		{TEXT("editor_get_open_assets"), TEXT("editor.open_assets")},
		{TEXT("editor_list_dirty_resources"), TEXT("editor.dirty_resources")},
		{TEXT("get_world_state"), TEXT("world.summary")},
		{TEXT("hlod_query_actors"), TEXT("world.hlod_assignments")},
		{TEXT("level_check_overlaps"), TEXT("world.overlap")},
		{TEXT("level_get_nearby_actor_obbs"), TEXT("world.nearby_shapes")},
		{TEXT("level_get_selection"), TEXT("world.selection")},
		{TEXT("level_list_actor_components"), TEXT("content_graph.items")},
		{TEXT("level_streaming_query"), TEXT("world.streaming_levels")},
		{TEXT("level_sweep_capsule"), TEXT("world.capsule_sweep")},
		{TEXT("level_sweep_capsule_path"), TEXT("world.capsule_path")},
		{TEXT("level_trace_world_ray"), TEXT("world.ray_trace")},
		{TEXT("level_validate_connectivity"), TEXT("world.nav_connectivity")},
		{TEXT("list_actors"), TEXT("content_graph.structure")},
		{TEXT("mesh_get_closest_vertex"), TEXT("world.mesh_closest_vertex")},
		{TEXT("mesh_get_vertex_world_position"), TEXT("world.mesh_vertex_position")},
		{TEXT("navmesh_find_path"), TEXT("world.nav_find_path")},
		{TEXT("navmesh_project_point"), TEXT("world.nav_project_point")},
	};

	class FNeverCancelled final : public IUeAgentCancellationSignal
	{
	public:
		virtual bool IsCancelled() const override
		{
			return false;
		}
	};

	class FFixtureQueryService final : public IUeAgentQueryService
	{
	public:
		virtual bool Execute(
			const FQueryServiceRequest& Request,
			FQueryServiceResponse& OutResponse,
			FQueryServiceError& OutError) const override
		{
			++Calls;
			LastRequest = Request;
			OutResponse = {};
			OutError = {};
			if (bFail)
			{
				OutError.Code = TEXT("fixture_query_failed");
				OutError.SubjectId = Request.CommandId;
				OutError.Detail = TEXT("fixture failure remains typed");
				return false;
			}
			OutResponse.PayloadJson = FString::Printf(
				TEXT("{\"command_id\":\"%s\",\"read_only\":true}"),
				*Request.CommandId);
			OutResponse.bReadOnly = !bPretendMutation;
			return true;
		}

		mutable int32 Calls = 0;
		mutable FQueryServiceRequest LastRequest;
		bool bFail = false;
		bool bPretendMutation = false;
	};

	struct FFixtureRegistration
	{
		TSharedRef<FFixtureQueryService> Service =
			MakeShared<FFixtureQueryService>();
		TSharedPtr<const FUeAgentDescriptorRegistry> DescriptorRegistry;
		TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
		TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
		FString Error;
	};

	FFixtureRegistration MakeRegistration(FAutomationTestBase* Test)
	{
		FFixtureRegistration Fixture;
		FProductionQueryComposition Production;
		Test->TestTrue(
			TEXT("T06 production descriptor authority composes"),
			TryCreateProductionQueryService(Production, Fixture.Error));
		Fixture.DescriptorRegistry = Production.DescriptorRegistry;
		if (!Fixture.DescriptorRegistry.IsValid())
		{
			return Fixture;
		}
		Test->TestTrue(
			TEXT("T06 contribution composes in isolation"),
			TryCreateQueryRegistration(
				Fixture.Service,
				Fixture.DescriptorRegistry.ToSharedRef(),
				Fixture.Registry,
				Fixture.Dispatch,
				Fixture.Error));
		Test->TestTrue(TEXT("T06 immutable registry exists"), Fixture.Registry.IsValid());
		Test->TestTrue(TEXT("T06 exact dispatch exists"), Fixture.Dispatch.IsValid());
		return Fixture;
	}

	bool DecodeRequest(
		const FString& CommandId,
		const FString& ContractHash,
		const FString& ParamsJson,
		TOptional<FUeAgentCommandRequest>& OutRequest)
	{
		const FString Envelope = FString::Printf(
			TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,")
			TEXT("\"message_kind\":\"command_request\",")
			TEXT("\"correlation_id\":\"corr-t06\",")
			TEXT("\"request_id\":\"req-t06\",")
			TEXT("\"command_id\":\"%s\",")
			TEXT("\"command_binding_hash\":\"%s\",\"params\":%s}"),
			*CommandId,
			*ContractHash,
			*ParamsJson);
		FTCHARToUTF8 Encoded(*Envelope);
		TArray<uint8> Bytes;
		Bytes.Append(
			reinterpret_cast<const uint8*>(Encoded.Get()),
			Encoded.Length());
		FUeAgentCommandProtocolDiagnostic Diagnostic;
		return FUeAgentCommandProtocolCodec().DecodeRequest(
			Bytes,
			OutRequest,
			Diagnostic);
	}

	FUeAgentCommandResult Submit(
		const FFixtureRegistration& Fixture,
		const FString& CommandId,
		const FString& ParamsJson,
		TArray<FString> Authorities = {TEXT("uai:query")})
	{
		const auto* Contract = Fixture.Registry.IsValid()
			? Fixture.Registry->FindExact(CommandId)
			: nullptr;
		if (!Contract)
		{
			return FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t06"),
				TEXT("req-t06"),
				CommandId,
				TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"),
				TEXT("fixture_contract_missing"),
				TEXT("T06 fixture could not find the exact contract"));
		}
		TOptional<FUeAgentCommandRequest> Request;
		if (!DecodeRequest(
				CommandId,
				Contract->GetContractHash(),
				ParamsJson,
				Request)
			|| !Request.IsSet())
		{
			return FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t06"),
				TEXT("req-t06"),
				CommandId,
				Contract->GetContractHash(),
				TEXT("fixture_request_decode_failed"),
				TEXT("T06 fixture could not decode its request"));
		}
		return Fixture.Dispatch->Submit(
			Request.GetValue(),
			FUeAgentCallerContext(
				TEXT("strict-v2-t06"),
				MoveTemp(Authorities),
				TEXT("corr-t06")),
			MakeShared<FNeverCancelled>());
	}
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T06InventoryAndLegacyExitTest,
	"UAI.StrictV2.T06.NON.HJSON.QUERY.MIGRATION.InventoryAndLegacyExit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T06InventoryAndLegacyExitTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T06_NON_HJSON_QUERY_AVAILABLE
	TestTrue(TEXT("T06 non-HJSON Query owner is implemented"), false);
	return false;
#else
	TArray<FString> Actual;
	for (const FQueryRoute& Route : GetNonHjsonQueryRoutes())
	{
		Actual.Add(Route.CommandId);
		TestTrue(TEXT("every T06 route has a mechanics command"), !Route.MechanicsCommandId.IsEmpty());
	}
	Actual.Sort();
	TestEqual(TEXT("QueryService exposes the audited base plus reference and Landscape extensions"), Actual, ExpectedCommandIds);
	TestEqual(TEXT("QueryService routes remain unique"), TSet<FString>(Actual).Num(), 42);
	TestEqual(TEXT("T06 freezes all twenty-one historical obligations"), HistoricalSuccessors.Num(), 21);
	for (const TPair<FString, FString>& Pair : HistoricalSuccessors)
	{
		TestTrue(
			TEXT("every historical successor is an active Query or HJSON command"),
			Actual.Contains(Pair.Value)
				|| Pair.Value.StartsWith(TEXT("content_graph.")));
	}

	const FQueryRoute* Reference = GetNonHjsonQueryRoutes().FindByPredicate(
		[](const FQueryRoute& Route)
		{
			return Route.CommandId == TEXT("reference.asset_read");
		});
	TestNotNull(TEXT("reference.asset_read remains an exact public route"), Reference);
	if (Reference)
	{
		TestEqual(
			TEXT("reference asset read composes its dedicated persisted readback mechanics"),
			Reference->MechanicsCommandId,
			FString(TEXT("reference.read")));
		TestTrue(TEXT("reference asset read requires one exact target"), Reference->bRequiresTarget);
		TestEqual(
			TEXT("reference asset read freezes the property written by reference.asset_apply"),
			Reference->RequiredProperty,
			FString(TEXT("BlueprintDisplayName")));
	}

	TSet<FString> Legacy;
	for (const FUeAgentReadExportCommandRoute& Route : GetReadExportQueryCommandRoutes())
	{
		Legacy.Add(Route.CommandId);
	}
	for (const FString& CommandId : ExpectedCommandIds)
	{
		TestFalse(
			TEXT("migrated command is absent from the old read/export route authority"),
			Legacy.Contains(CommandId));
	}
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T06ExactDispatchTest,
	"UAI.StrictV2.T06.NON.HJSON.QUERY.MIGRATION.ExactDispatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T06ExactDispatchTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T06_NON_HJSON_QUERY_AVAILABLE
	TestTrue(TEXT("T06 exact Query dispatch is implemented"), false);
	return false;
#else
	FFixtureRegistration Fixture = MakeRegistration(this);
	if (!Fixture.Registry.IsValid() || !Fixture.Dispatch.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("isolated QueryService registry contains forty-two contracts"), Fixture.Registry->GetContracts().Num(), 42);
	for (const FString& CommandId : ExpectedCommandIds)
	{
		const auto* Contract = Fixture.Registry->FindExact(CommandId);
		TestNotNull(TEXT("every audited command has one exact contract"), Contract);
		if (Contract)
		{
			TestEqual(
				TEXT("all T06 contracts publish the unique Query owner"),
				Contract->GetPublicOwner(),
				FString(TEXT("query-command-owner")));
		}
	}
	const FUeAgentFrozenCommandContract* Relations =
		Fixture.Registry->FindExact(TEXT("asset.registry_relations"));
	if (!TestNotNull(
			TEXT("asset.registry_relations has one exact contract"),
			Relations))
	{
		return false;
	}
	const FUeAgentCommandFieldDeclaration* RelationsInput =
		Relations->GetInputFields().FindByPredicate(
			[](const FUeAgentCommandFieldDeclaration& Field)
			{
				return Field.Name == TEXT("input");
			});
	if (!TestNotNull(
			TEXT("asset.registry_relations exposes command-specific input"),
			RelationsInput))
	{
		return false;
	}
	TestTrue(
		TEXT("nested required fields make the public input object required"),
		RelationsInput->bRequired);
	TSharedPtr<FJsonObject> RelationsInputSchema;
	FString SchemaError;
	TestTrue(
		TEXT("the exact descriptor input schema reaches the frozen contract"),
		UE::UAI::CommandContract::Json::TryParseObject(
			RelationsInput->SchemaJson,
			RelationsInputSchema,
			SchemaError));
	if (RelationsInputSchema.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>& Required =
			RelationsInputSchema->GetArrayField(TEXT("required"));
		TestTrue(
			TEXT("the public input schema requires direction"),
			Required.ContainsByPredicate(
				[](const TSharedPtr<FJsonValue>& Value)
				{
					return Value.IsValid()
						&& Value->Type == EJson::String
						&& Value->AsString() == TEXT("direction");
				}));
		const TSharedPtr<FJsonObject> Direction =
			RelationsInputSchema->GetObjectField(TEXT("properties"))
				->GetObjectField(TEXT("direction"));
		const TArray<TSharedPtr<FJsonValue>>& DirectionValues =
			Direction->GetArrayField(TEXT("enum"));
		TestEqual(
			TEXT("the public input schema exposes all exact direction values"),
			DirectionValues.Num(),
			3);
	}
	TestTrue(
		TEXT("the minimum tutorial includes a directly usable direction"),
		Relations->GetPurpose().MinimumExample.Contains(
			TEXT("input={\"direction\":\"dependencies\"}")));
	const FUeAgentFrozenCommandContract* ProjectSummary =
		Fixture.Registry->FindExact(TEXT("project.summary"));
	const FUeAgentCommandFieldDeclaration* ProjectSummaryInput =
		ProjectSummary
			? ProjectSummary->GetInputFields().FindByPredicate(
				[](const FUeAgentCommandFieldDeclaration& Field)
				{
					return Field.Name == TEXT("input");
				})
			: nullptr;
	TestTrue(
		TEXT("a Query without nested required fields keeps optional input"),
		ProjectSummaryInput && !ProjectSummaryInput->bRequired);

	const FString Params =
		TEXT("{\"context\":{\"state_plane\":\"persisted\"},")
		TEXT("\"input\":{\"include_tags\":true},")
		TEXT("\"object_path\":\"/Game/Test.Test\",")
		TEXT("\"projection\":{\"max_items\":7}}");
	const FUeAgentCommandResult Result = Submit(
		Fixture,
		TEXT("asset.summary"),
		Params);
	TestTrue(TEXT("exact route delegates successfully"), Result.IsOk());
	TestEqual(TEXT("one public request reaches one QueryService call"), Fixture.Service->Calls, 1);
	TestEqual(
		TEXT("QueryService receives the unchanged exact command identity"),
		Fixture.Service->LastRequest.CommandId,
		FString(TEXT("asset.summary")));
	TestEqual(
		TEXT("handler canonicalizes but does not reinterpret public parameters"),
		Fixture.Service->LastRequest.CanonicalParamsJson,
		Params);
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T06FailureSemanticsTest,
	"UAI.StrictV2.T06.NON.HJSON.QUERY.MIGRATION.FailureSemantics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T06FailureSemanticsTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T06_NON_HJSON_QUERY_AVAILABLE
	TestTrue(TEXT("T06 Query failures are implemented"), false);
	return false;
#else
	FFixtureRegistration Fixture = MakeRegistration(this);
	const FUeAgentCommandResult Unauthorized = Submit(
		Fixture,
		TEXT("project.summary"),
		TEXT("{}"),
		{});
	TestTrue(
		TEXT("missing query authority is a business failure"),
		!Unauthorized.IsOk() && !Unauthorized.IsProtocolError());
	TestTrue(TEXT("authority failure stays typed"), Unauthorized.GetErrorJson().Contains(TEXT("query_authority_required")));
	TestEqual(TEXT("unauthorized request never reaches QueryService"), Fixture.Service->Calls, 0);

	Fixture.Service->bFail = true;
	const FUeAgentCommandResult ProviderFailure = Submit(
		Fixture,
		TEXT("project.summary"),
		TEXT("{}"));
	TestTrue(
		TEXT("provider failure remains a business failure"),
		!ProviderFailure.IsOk() && !ProviderFailure.IsProtocolError());
	TestTrue(TEXT("provider code is not collapsed to empty data"), ProviderFailure.GetErrorJson().Contains(TEXT("fixture_query_failed")));

	Fixture.Service->bFail = false;
	Fixture.Service->bPretendMutation = true;
	const FUeAgentCommandResult Mutation = Submit(
		Fixture,
		TEXT("project.summary"),
		TEXT("{}"));
	TestTrue(
		TEXT("a source that cannot attest read-only is rejected"),
		!Mutation.IsOk() && !Mutation.IsProtocolError());
	TestTrue(TEXT("read-only violation stays typed"), Mutation.GetErrorJson().Contains(TEXT("query_source_not_read_only")));
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T06ProductionReadbackTest,
	"UAI.StrictV2.T06.NON.HJSON.QUERY.MIGRATION.ProductionReadback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T06ProductionReadbackTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T06_NON_HJSON_QUERY_AVAILABLE
	TestTrue(TEXT("T06 production Query readback is implemented"), false);
	return false;
#else
	TestTrue(TEXT("production Query readback runs on the game thread"), IsInGameThread());
	FProductionQueryComposition Production;
	FString Error;
	TestTrue(
		TEXT("real QueryService composes from production UE mechanics"),
		TryCreateProductionQueryService(Production, Error));
	if (!Production.IsValid())
	{
		AddError(Error);
		return false;
	}
	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	TestTrue(
		TEXT("production Query contribution composes"),
		TryCreateQueryRegistration(
			Production.Service.ToSharedRef(),
			Production.DescriptorRegistry.ToSharedRef(),
			Registry,
			Dispatch,
			Error));
	if (!Registry.IsValid() || !Dispatch.IsValid())
	{
		AddError(Error);
		return false;
	}

	FFixtureRegistration Public;
	Public.Service = StaticCastSharedRef<FFixtureQueryService>(MakeShared<FFixtureQueryService>());
	Public.Registry = Registry;
	Public.Dispatch = Dispatch;
	const FUeAgentCommandResult Result = Submit(
		Public,
		TEXT("project.summary"),
		TEXT("{\"projection\":{\"include_provenance\":true,\"max_items\":25}}"));
	TestTrue(TEXT("real project query succeeds through exact dispatch"), Result.IsOk());
	TSharedPtr<FJsonObject> Payload;
	FString JsonError;
	TestTrue(
		TEXT("real Query response is canonical JSON"),
		UE::UAI::CommandContract::Json::TryParseObject(
			Result.GetPayloadJson(),
			Payload,
			JsonError));
	if (Payload.IsValid())
	{
		TestEqual(TEXT("public response preserves exact command id"), Payload->GetStringField(TEXT("command_id")), FString(TEXT("project.summary")));
		TestEqual(TEXT("real mechanics identity is explicit"), Payload->GetStringField(TEXT("mechanics_command_id")), FString(TEXT("project.summary")));
		TestTrue(TEXT("production response attests read-only"), Payload->GetBoolField(TEXT("read_only")));
		TestTrue(TEXT("real projection payload is returned"), Payload->HasTypedField<EJson::Object>(TEXT("query_result")));
	}

	TArray<TWeakObjectPtr<AActor>> OriginalSelection;
	if (GEditor && GEditor->GetSelectedActors())
	{
		for (FSelectionIterator It(*GEditor->GetSelectedActors()); It; ++It)
		{
			if (AActor* Actor = Cast<AActor>(*It))
			{
				OriginalSelection.Add(Actor);
			}
		}
		GEditor->SelectNone(false, true, false);
	}

	const FUeAgentCommandResult EmptySelection = Submit(
		Public,
		TEXT("world.selection"),
		TEXT("{}"));
	TestTrue(
		TEXT("an empty current selection is a successful public readback"),
		EmptySelection.IsOk());
	if (!EmptySelection.IsOk())
	{
		AddError(EmptySelection.GetErrorJson());
	}
	else
	{
		TSharedPtr<FJsonObject> SelectionPayload;
		const TSharedPtr<FJsonObject>* QueryResult = nullptr;
		const TSharedPtr<FJsonObject>* Projection = nullptr;
		const TSharedPtr<FJsonObject>* ProjectionPayload = nullptr;
		const TSharedPtr<FJsonObject>* Snapshot = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		TestTrue(
			TEXT("empty selection response exposes a complete snapshot"),
			UE::UAI::CommandContract::Json::TryParseObject(
				EmptySelection.GetPayloadJson(),
				SelectionPayload,
				JsonError)
				&& SelectionPayload.IsValid()
				&& SelectionPayload->TryGetObjectField(
					TEXT("query_result"), QueryResult)
				&& QueryResult && QueryResult->IsValid()
				&& (*QueryResult)->TryGetObjectField(
					TEXT("projection"), Projection)
				&& Projection && Projection->IsValid()
				&& (*Projection)->TryGetObjectField(
					TEXT("payload"), ProjectionPayload)
				&& ProjectionPayload && ProjectionPayload->IsValid()
				&& (*ProjectionPayload)->TryGetObjectField(
					TEXT("snapshot"), Snapshot)
				&& Snapshot && Snapshot->IsValid()
				&& (*Snapshot)->TryGetArrayField(TEXT("entries"), Entries)
				&& Entries
				&& Entries->IsEmpty());
	}

	if (GEditor)
	{
		GEditor->SelectNone(false, true, false);
		for (const TWeakObjectPtr<AActor>& Actor : OriginalSelection)
		{
			if (Actor.IsValid())
			{
				GEditor->SelectActor(Actor.Get(), true, false, true);
			}
		}
	}
	return true;
#endif
}

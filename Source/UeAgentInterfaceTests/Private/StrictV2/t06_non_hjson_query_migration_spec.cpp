#include "Misc/AutomationTest.h"

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "DomainAdapters/Query/UeAgentQueryService.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "GameFramework/Actor.h"
#include "Module/CommandRegistration/Query/UeAgentQueryRegistration.h"

namespace
{
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::CommandRegistry;
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
		TEXT("content_graph.compound"),
		TEXT("content_graph.diagnostics"),
		TEXT("content_graph.items"),
		TEXT("content_graph.properties"),
		TEXT("content_graph.relations"),
		TEXT("content_graph.structure"),
		TEXT("content_graph.summary"),
		TEXT("editor.dirty_resources"),
		TEXT("editor.open_assets"),
		TEXT("landscape.height.read"),
		TEXT("project.platform"),
		TEXT("project.settings"),
		TEXT("project.summary"),
		TEXT("project.validation"),
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
				OutError.PublicMessage = TEXT("fixture failure remains typed");
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T06ExactDispatchTest,
	"UAI.StrictV2.T06.NON.HJSON.QUERY.MIGRATION.ExactDispatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T06ExactDispatchTest::RunTest(
	const FString& Parameters)
{
	FFixtureRegistration Fixture = MakeRegistration(this);
	if (!Fixture.Registry.IsValid() || !Fixture.Dispatch.IsValid())
	{
		return false;
	}
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
		Fixture.Registry->FindExact(TEXT("asset.relations"));
	if (!TestNotNull(
			TEXT("asset.relations has one exact contract"),
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
			TEXT("asset.relations exposes command-specific input"),
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
		const TSharedPtr<FJsonObject> Properties =
			RelationsInputSchema->GetObjectField(TEXT("properties"));
		TestTrue(
			TEXT("asset.relations publishes package scope control"),
			Properties->HasField(TEXT("package_scope")));
	}
	TestTrue(
		TEXT("the minimum tutorial includes a directly usable direction"),
		Relations->GetPurpose().MinimumExample.Contains(
			TEXT("input={\"direction\":\"dependencies\"}")));
	TestTrue(
		TEXT("asset.relations declares a normalized direct public graph"),
		Relations->GetResultFields().Num() == 2
			&& Relations->GetResultFields()[0].Name == TEXT("relations")
			&& Relations->GetResultFields()[0].bRequired
			&& Relations->GetResultFields()[1].Name == TEXT("more")
			&& !Relations->GetResultFields()[1].bRequired
			&& Relations->GetPurpose().ResultGuidance.Contains(
				TEXT("aliases"))
			&& Relations->GetPurpose().ResultGuidance.Contains(
				TEXT("exact UE targets")));
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
	for (const FString& DirectFacet : {
		TEXT("content_graph.summary"),
		TEXT("content_graph.structure"),
		TEXT("content_graph.items"),
		TEXT("content_graph.relations"),
		TEXT("content_graph.properties"),
		TEXT("content_graph.diagnostics"),
	})
	{
		const FUeAgentFrozenCommandContract* Contract =
			Fixture.Registry->FindExact(DirectFacet);
		TestTrue(
			*FString::Printf(
				TEXT("%s declares the direct public result"),
				*DirectFacet),
			Contract
				&& Contract->GetResultFields().Num() == 2
				&& Contract->GetResultFields()[0].Name == TEXT("entries")
				&& Contract->GetResultFields()[0].bRequired
				&& Contract->GetResultFields()[1].Name == TEXT("more")
				&& !Contract->GetResultFields()[1].bRequired);
	}
	const FUeAgentFrozenCommandContract* Compound =
		Fixture.Registry->FindExact(TEXT("content_graph.compound"));
	TestTrue(
		TEXT("content_graph.compound declares section-grouped public results"),
		Compound
			&& Compound->GetResultFields().Num() == 2
			&& Compound->GetResultFields()[0].Name == TEXT("sections")
			&& Compound->GetResultFields()[0].bRequired
			&& Compound->GetResultFields()[1].Name == TEXT("more")
			&& !Compound->GetResultFields()[1].bRequired);
	const FUeAgentFrozenCommandContract* DirtyResources =
		Fixture.Registry->FindExact(TEXT("editor.dirty_resources"));
	TestTrue(
		TEXT("editor.dirty_resources declares its direct grouped public result"),
		DirtyResources
			&& DirtyResources->GetResultFields().Num() == 2
			&& DirtyResources->GetResultFields()[0].Name
				== TEXT("resources")
			&& DirtyResources->GetResultFields()[0].bRequired
			&& DirtyResources->GetResultFields()[1].Name == TEXT("more")
			&& !DirtyResources->GetResultFields()[1].bRequired);
	const FUeAgentFrozenCommandContract* AssetProperties =
		Fixture.Registry->FindExact(TEXT("asset.properties"));
	const FUeAgentFrozenCommandContract* AssetCollection =
		Fixture.Registry->FindExact(TEXT("asset.collection"));
	TestTrue(
		TEXT("asset.collection declares one normalized direct result"),
		AssetCollection
			&& AssetCollection->GetResultFields().Num() == 2
			&& AssetCollection->GetResultFields()[0].Name
				== TEXT("collections")
			&& AssetCollection->GetResultFields()[0].bRequired
			&& AssetCollection->GetResultFields()[1].Name == TEXT("more")
			&& !AssetCollection->GetResultFields()[1].bRequired
			&& AssetCollection->GetPurpose().ResultGuidance.Contains(
				TEXT("values explicitly requested")));
	const FUeAgentFrozenCommandContract* AssetSample =
		Fixture.Registry->FindExact(TEXT("asset.sample"));
	const FUeAgentCommandFieldDeclaration* AssetSampleInput =
		AssetSample
			? AssetSample->GetInputFields().FindByPredicate(
				[](const FUeAgentCommandFieldDeclaration& Field)
				{
					return Field.Name == TEXT("input");
				})
			: nullptr;
	TSharedPtr<FJsonObject> AssetSampleSchema;
	SchemaError.Reset();
	TestTrue(
		TEXT("asset.sample publishes its exact supported input schema"),
		AssetSampleInput
			&& UE::UAI::CommandContract::Json::TryParseObject(
				AssetSampleInput->SchemaJson,
				AssetSampleSchema,
				SchemaError));
	if (AssetSampleSchema.IsValid())
	{
		const TSharedPtr<FJsonObject> SampleProperties =
			AssetSampleSchema->GetObjectField(TEXT("properties"));
		const TArray<TSharedPtr<FJsonValue>>& SampleKinds =
			SampleProperties->GetObjectField(TEXT("sample_kind"))
				->GetArrayField(TEXT("enum"));
		TestTrue(
			TEXT("asset.sample exposes only implemented sample kinds"),
			SampleKinds.Num() == 2
				&& SampleKinds.ContainsByPredicate(
					[](const TSharedPtr<FJsonValue>& Value)
					{
						return Value.IsValid()
							&& Value->AsString() == TEXT("bounds");
					})
				&& SampleKinds.ContainsByPredicate(
					[](const TSharedPtr<FJsonValue>& Value)
					{
						return Value.IsValid()
							&& Value->AsString() == TEXT("vertex");
					}));
		TestFalse(
			TEXT("asset.sample removes inputs with no native owner"),
			SampleProperties->HasField(TEXT("coordinates"))
				|| SampleProperties->HasField(TEXT("channel")));
	}
	TestTrue(
		TEXT("asset.sample declares one normalized direct result"),
		AssetSample
			&& AssetSample->GetResultFields().Num() == 2
			&& AssetSample->GetResultFields()[0].Name == TEXT("samples")
			&& AssetSample->GetResultFields()[0].bRequired
			&& AssetSample->GetResultFields()[1].Name == TEXT("more")
			&& !AssetSample->GetResultFields()[1].bRequired
			&& AssetSample->GetPurpose().ResultGuidance.Contains(
				TEXT("position_cm")));
	TestTrue(
		TEXT("asset.properties declares its direct dynamic public result"),
		AssetProperties
			&& AssetProperties->GetResultFields().Num() == 2
			&& AssetProperties->GetResultFields()[0].Name
				== TEXT("properties")
			&& AssetProperties->GetResultFields()[0].bRequired
			&& AssetProperties->GetResultFields()[1].Name == TEXT("more")
			&& !AssetProperties->GetResultFields()[1].bRequired
			&& AssetProperties->GetPurpose().ResultGuidance.Contains(
				TEXT("field_ids")));
	const FUeAgentFrozenCommandContract* ProjectSettings =
		Fixture.Registry->FindExact(TEXT("project.settings"));
	TestTrue(
		TEXT("project.settings reuses the direct dynamic result contract"),
		ProjectSettings
			&& ProjectSettings->GetResultFields().Num() == 2
			&& ProjectSettings->GetResultFields()[0].Name
				== TEXT("settings")
			&& ProjectSettings->GetResultFields()[0].bRequired
			&& ProjectSettings->GetResultFields()[1].Name == TEXT("more")
			&& !ProjectSettings->GetResultFields()[1].bRequired
			&& ProjectSettings->GetPurpose().ResultGuidance.Contains(
				TEXT("field_ids")));
	const FUeAgentFrozenCommandContract* ConfigSection =
		Fixture.Registry->FindExact(TEXT("config.section"));
	TestTrue(
		TEXT("config.section declares names-only default and explicit fields"),
		ConfigSection
			&& ConfigSection->GetResultFields().Num() == 2
			&& ConfigSection->GetResultFields()[0].Name == TEXT("sections")
			&& ConfigSection->GetResultFields()[0].bRequired
			&& ConfigSection->GetPurpose().ResultGuidance.Contains(
				TEXT("names-only")));
	const FUeAgentFrozenCommandContract* ConfigResolvedValue =
		Fixture.Registry->FindExact(TEXT("config.resolved_value"));
	TestTrue(
		TEXT("config.resolved_value declares one explicit dynamic result"),
		ConfigResolvedValue
			&& ConfigResolvedValue->GetResultFields().Num() == 2
			&& ConfigResolvedValue->GetResultFields()[0].Name
				== TEXT("values")
			&& ConfigResolvedValue->GetResultFields()[0].bRequired
			&& ConfigResolvedValue->GetPurpose().ResultGuidance.Contains(
				TEXT("resolved value")));
	for (const auto& WorldResult : {
		TPair<FString, FString>(TEXT("world.summary"), TEXT("worlds")),
		TPair<FString, FString>(TEXT("world.selection"), TEXT("selected_objects")),
	})
	{
		const FUeAgentFrozenCommandContract* Contract =
			Fixture.Registry->FindExact(WorldResult.Key);
		TestTrue(
			*FString::Printf(
				TEXT("%s declares one direct public object result"),
				*WorldResult.Key),
			Contract
				&& Contract->GetResultFields().Num() == 2
				&& Contract->GetResultFields()[0].Name == WorldResult.Value
				&& Contract->GetResultFields()[0].bRequired
				&& Contract->GetResultFields()[1].Name == TEXT("more")
			&& !Contract->GetResultFields()[1].bRequired);
	}
	const FUeAgentFrozenCommandContract* WorldRayTrace =
		Fixture.Registry->FindExact(TEXT("world.ray_trace"));
	TestTrue(
		TEXT("world.ray_trace declares one minimal record without a result envelope"),
		WorldRayTrace
			&& WorldRayTrace->GetResultFields().Num() == 3
			&& WorldRayTrace->GetResultFields()[0].Name == TEXT("hit")
			&& WorldRayTrace->GetResultFields()[0].bRequired
			&& WorldRayTrace->GetResultFields()[1].Name == TEXT("target_ref")
			&& !WorldRayTrace->GetResultFields()[1].bRequired
			&& WorldRayTrace->GetResultFields()[2].Name == TEXT("impact_point")
			&& !WorldRayTrace->GetResultFields()[2].bRequired);
	struct FSpatialResultContract final
	{
		FString CommandId;
		TArray<FString> Fields;
		int32 RequiredFieldCount = 0;
	};
	const TArray<FSpatialResultContract> SpatialResultContracts = {
		{TEXT("world.overlap"), {TEXT("count"), TEXT("targets")}, 1},
		{TEXT("world.capsule_sweep"), {TEXT("hit_count"), TEXT("hits")}, 1},
		{TEXT("world.capsule_path"), {TEXT("blocked_segments"), TEXT("first_blocking_hit")}, 1},
		{TEXT("world.nearby_shapes"), {TEXT("count"), TEXT("shapes")}, 1},
		{TEXT("world.mesh_closest_vertex"), {TEXT("vertex_index"), TEXT("position")}, 2},
		{TEXT("world.mesh_vertex_position"), {TEXT("vertex_index"), TEXT("position")}, 2},
		{TEXT("world.nav_project_point"), {TEXT("projected_point")}, 1},
		{TEXT("world.nav_connectivity"), {TEXT("pairs")}, 1},
	};
	for (const FSpatialResultContract& SpatialContract
		: SpatialResultContracts)
	{
		const FUeAgentFrozenCommandContract* Contract =
			Fixture.Registry->FindExact(SpatialContract.CommandId);
		bool bExact = Contract
			&& Contract->GetResultFields().Num()
				== SpatialContract.Fields.Num();
		if (bExact)
		{
			for (int32 Index = 0; Index < SpatialContract.Fields.Num(); ++Index)
			{
				bExact = bExact
					&& Contract->GetResultFields()[Index].Name
						== SpatialContract.Fields[Index]
					&& Contract->GetResultFields()[Index].bRequired
						== (Index < SpatialContract.RequiredFieldCount);
			}
		}
		TestTrue(
			*FString::Printf(
				TEXT("%s declares only its normalized business fields"),
				*SpatialContract.CommandId),
			bExact);
	}
	const FUeAgentFrozenCommandContract* NavFindPath =
		Fixture.Registry->FindExact(TEXT("world.nav_find_path"));
	TestTrue(
		TEXT("world.nav_find_path keeps path geometry explicitly optional"),
		NavFindPath
			&& NavFindPath->GetResultFields().Num() == 2
			&& NavFindPath->GetResultFields()[0].Name == TEXT("reachable")
			&& NavFindPath->GetResultFields()[0].bRequired
			&& NavFindPath->GetResultFields()[1].Name == TEXT("path_points")
			&& !NavFindPath->GetResultFields()[1].bRequired
			&& NavFindPath->GetPurpose().ResultGuidance.Contains(
				TEXT("Request path points only")));
	for (const TPair<FString, FString>& GroupedTopology : {
		TPair<FString, FString>(TEXT("world.data_layers"), TEXT("data_layers")),
		TPair<FString, FString>(TEXT("world.hlod_assignments"), TEXT("assignments")),
		TPair<FString, FString>(TEXT("world.streaming_levels"), TEXT("levels")),
	})
	{
		const FUeAgentFrozenCommandContract* Contract =
			Fixture.Registry->FindExact(GroupedTopology.Key);
		TestTrue(
			*FString::Printf(
				TEXT("%s declares one normalized record collection"),
				*GroupedTopology.Key),
			Contract
				&& Contract->GetResultFields().Num() == 2
				&& Contract->GetResultFields()[0].Name
					== GroupedTopology.Value
				&& Contract->GetResultFields()[0].bRequired
				&& Contract->GetResultFields()[1].Name == TEXT("more")
				&& !Contract->GetResultFields()[1].bRequired);
	}
	const FUeAgentFrozenCommandContract* WorldPartition =
		Fixture.Registry->FindExact(TEXT("world.partition"));
	TestTrue(
		TEXT("world.partition keeps region geometry explicitly optional"),
		WorldPartition
			&& WorldPartition->GetResultFields().Num() == 4
			&& WorldPartition->GetResultFields()[0].Name == TEXT("enabled")
			&& WorldPartition->GetResultFields()[0].bRequired
			&& WorldPartition->GetResultFields()[3].Name
				== TEXT("loaded_regions")
			&& !WorldPartition->GetResultFields()[3].bRequired);
	const FUeAgentFrozenCommandContract* PartitionHealth =
		Fixture.Registry->FindExact(TEXT("world.partition_health"));
	TestTrue(
		TEXT("world.partition_health exposes only normalized issue facts"),
		PartitionHealth
			&& PartitionHealth->GetResultFields().Num() == 4
			&& PartitionHealth->GetResultFields()[1].Name == TEXT("status")
			&& PartitionHealth->GetResultFields()[2].Name == TEXT("error_count")
			&& PartitionHealth->GetResultFields()[3].Name == TEXT("issues")
			&& PartitionHealth->GetResultFields()[3].Description.Contains(
				TEXT("without GUID")));
	const FUeAgentFrozenCommandContract* PhysicsComponent =
		Fixture.Registry->FindExact(TEXT("world.physics_component"));
	const FUeAgentFrozenCommandContract* PhysicsConstraint =
		Fixture.Registry->FindExact(TEXT("world.physics_constraint"));
	TestTrue(
		TEXT("physics queries expose minimal defaults with explicit expansions"),
		PhysicsComponent
			&& PhysicsComponent->GetResultFields().Num() == 5
			&& PhysicsComponent->GetResultFields()[0].Name
				== TEXT("collision_enabled")
			&& PhysicsComponent->GetResultFields()[0].bRequired
			&& PhysicsComponent->GetResultFields()[4].Name
				== TEXT("collision_responses")
			&& !PhysicsComponent->GetResultFields()[4].bRequired
			&& PhysicsConstraint
			&& PhysicsConstraint->GetResultFields().Num() == 16
			&& PhysicsConstraint->GetResultFields()[0].Name == TEXT("path")
			&& PhysicsConstraint->GetResultFields()[0].bRequired);
	const FUeAgentFrozenCommandContract* LandscapeHeight =
		Fixture.Registry->FindExact(TEXT("landscape.height.read"));
	TestTrue(
		TEXT("landscape.height.read declares one direct spatially typed result"),
		LandscapeHeight
			&& LandscapeHeight->GetResultFields().Num() == 2
			&& LandscapeHeight->GetResultFields()[0].Name
				== TEXT("height_grids")
			&& LandscapeHeight->GetResultFields()[0].bRequired
			&& LandscapeHeight->GetResultFields()[1].Name == TEXT("more")
			&& !LandscapeHeight->GetResultFields()[1].bRequired
			&& LandscapeHeight->GetPurpose().ResultGuidance.Contains(
				TEXT("scale_cm_per_local_unit")));

	const FString Params =
		TEXT("{\"context\":{\"state_plane\":\"persisted\"},")
		TEXT("\"input\":{\"include_tags\":true},")
		TEXT("\"object_path\":\"/Game/Test.Test\"}");
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T06FailureSemanticsTest,
	"UAI.StrictV2.T06.NON.HJSON.QUERY.MIGRATION.FailureSemantics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T06FailureSemanticsTest::RunTest(
	const FString& Parameters)
{
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T06ProductionReadbackTest,
	"UAI.StrictV2.T06.NON.HJSON.QUERY.MIGRATION.ProductionReadback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T06ProductionReadbackTest::RunTest(
	const FString& Parameters)
{
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
		TEXT("{\"projection\":{\"include_provenance\":true}}"));
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
		const TArray<TSharedPtr<FJsonValue>>* Objects = nullptr;
		TestTrue(
			TEXT("project summary directly returns one business object"),
			Payload->TryGetArrayField(TEXT("objects"), Objects)
				&& Objects
				&& Objects->Num() == 1
				&& (*Objects)[0].IsValid()
				&& (*Objects)[0]->Type == EJson::Object);
		if (Objects && Objects->Num() == 1)
		{
			const TSharedPtr<FJsonObject> Object = (*Objects)[0]->AsObject();
			TestTrue(
				TEXT("project summary keeps only exact path and package business state"),
				Object.IsValid()
					&& Object->Values.Num() == 4
					&& Object->HasTypedField<EJson::String>(TEXT("path"))
					&& Object->HasTypedField<EJson::String>(TEXT("class_path"))
					&& Object->HasTypedField<EJson::String>(TEXT("package_name"))
					&& Object->HasTypedField<EJson::Boolean>(TEXT("package_dirty")));
		}
		for (const FString& InternalField : {
			TEXT("command_id"), TEXT("mechanics_command_id"),
			TEXT("read_only"), TEXT("query_result"), TEXT("projection"),
			TEXT("snapshot_id"), TEXT("revision")})
		{
			TestFalse(
				*FString::Printf(
					TEXT("project summary omits internal field %s"),
					*InternalField),
				Payload->HasField(InternalField));
		}
	}

	const FUeAgentCommandResult WorldSummary = Submit(
		Public,
		TEXT("world.summary"),
		TEXT("{}"));
	TestTrue(
		TEXT("world summary succeeds through exact dispatch"),
		WorldSummary.IsOk());
	TSharedPtr<FJsonObject> WorldSummaryPayload;
	JsonError.Reset();
	TestTrue(
		TEXT("world summary returns direct business records"),
		UE::UAI::CommandContract::Json::TryParseObject(
			WorldSummary.GetPayloadJson(),
			WorldSummaryPayload,
			JsonError));
	if (WorldSummaryPayload.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Worlds = nullptr;
		TestTrue(
			TEXT("world summary contains one exact world path and dirty state"),
			WorldSummaryPayload->Values.Num() == 1
				&& WorldSummaryPayload->TryGetArrayField(TEXT("worlds"), Worlds)
				&& Worlds
				&& Worlds->Num() == 1
				&& (*Worlds)[0]->AsObject().IsValid()
				&& (*Worlds)[0]->AsObject()->Values.Num() == 2
				&& (*Worlds)[0]->AsObject()->HasTypedField<EJson::String>(TEXT("object_path"))
				&& (*Worlds)[0]->AsObject()->HasTypedField<EJson::Boolean>(TEXT("package_dirty")));
	}

	const FUeAgentCommandResult WorldSelection = Submit(
		Public,
		TEXT("world.selection"),
		TEXT("{}"));
	TestTrue(
		TEXT("world selection succeeds through exact dispatch"),
		WorldSelection.IsOk());
	TSharedPtr<FJsonObject> WorldSelectionPayload;
	JsonError.Reset();
	const TArray<TSharedPtr<FJsonValue>>* SelectedObjects = nullptr;
	TestTrue(
		TEXT("world selection returns only the selected object paths"),
		UE::UAI::CommandContract::Json::TryParseObject(
			WorldSelection.GetPayloadJson(),
			WorldSelectionPayload,
			JsonError)
			&& WorldSelectionPayload.IsValid()
			&& WorldSelectionPayload->Values.Num() == 1
			&& WorldSelectionPayload->TryGetArrayField(
				TEXT("selected_objects"),
				SelectedObjects)
			&& SelectedObjects);

	const FUeAgentCommandResult RayMiss = Submit(
		Public,
		TEXT("world.ray_trace"),
		TEXT("{\"input\":{\"start\":{\"frame_id\":\"ue.world\",\"unit\":\"centimeter\",\"components\":[100000,100000,100000]},\"direction\":{\"frame_id\":\"ue.world\",\"components\":[1,0,0]},\"distance_cm\":10}}"));
	TestTrue(
		TEXT("a ray miss succeeds through the exact public Query route"),
		RayMiss.IsOk());
	TSharedPtr<FJsonObject> RayMissPayload;
	JsonError.Reset();
	TestTrue(
		TEXT("a ray miss is the minimal hit-false business result"),
		UE::UAI::CommandContract::Json::TryParseObject(
			RayMiss.GetPayloadJson(),
			RayMissPayload,
			JsonError)
			&& RayMissPayload.IsValid()
			&& RayMissPayload->Values.Num() == 1
			&& !RayMissPayload->GetBoolField(TEXT("hit")));

	struct FEmptySpatialCase final
	{
		FString CommandId;
		FString ParamsJson;
		TArray<FString> Fields;
		FString ArrayField;
		bool bRequireEmptyArray = true;
	};
	const TArray<FEmptySpatialCase> EmptySpatialCases = {
		{
			TEXT("world.overlap"),
			TEXT("{\"input\":{\"shape\":\"sphere\",\"center\":{\"frame_id\":\"ue.world\",\"unit\":\"centimeter\",\"components\":[100000,100000,100000]},\"radius_cm\":1}}"),
			{TEXT("count")},
			FString(),
			true,
		},
		{
			TEXT("world.capsule_sweep"),
			TEXT("{\"input\":{\"start\":{\"frame_id\":\"ue.world\",\"unit\":\"centimeter\",\"components\":[100000,100000,100000]},\"end\":{\"frame_id\":\"ue.world\",\"unit\":\"centimeter\",\"components\":[100010,100000,100000]},\"radius_cm\":1,\"half_height_cm\":2}}"),
			{TEXT("hit_count")},
			FString(),
			true,
		},
		{
			TEXT("world.capsule_path"),
			TEXT("{\"input\":{\"points\":[{\"frame_id\":\"ue.world\",\"unit\":\"centimeter\",\"components\":[100000,100000,100000]},{\"frame_id\":\"ue.world\",\"unit\":\"centimeter\",\"components\":[100010,100000,100000]}],\"radius_cm\":1,\"half_height_cm\":2,\"step_cm\":5}}"),
			{TEXT("blocked_segments")},
			FString(),
			true,
		},
		{
			TEXT("world.nearby_shapes"),
			TEXT("{\"input\":{\"center\":{\"frame_id\":\"ue.world\",\"unit\":\"centimeter\",\"components\":[100000,100000,100000]},\"radius_cm\":1}}"),
			{TEXT("count")},
			FString(),
			true,
		},
	};
	for (const FEmptySpatialCase& SpatialCase : EmptySpatialCases)
	{
		const FUeAgentCommandResult SpatialResult = Submit(
			Public,
			SpatialCase.CommandId,
			SpatialCase.ParamsJson);
		TSharedPtr<FJsonObject> SpatialPayload;
		JsonError.Reset();
		bool bMinimal = SpatialResult.IsOk()
			&& UE::UAI::CommandContract::Json::TryParseObject(
				SpatialResult.GetPayloadJson(),
				SpatialPayload,
				JsonError)
			&& SpatialPayload.IsValid()
			&& SpatialPayload->Values.Num() == SpatialCase.Fields.Num();
		if (bMinimal)
		{
			for (const FString& Field : SpatialCase.Fields)
			{
				bMinimal = bMinimal && SpatialPayload->HasField(Field);
			}
			if (!SpatialCase.ArrayField.IsEmpty())
			{
				const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
				bMinimal = bMinimal
					&& SpatialPayload->TryGetArrayField(
						SpatialCase.ArrayField,
						Values)
					&& Values
					&& (!SpatialCase.bRequireEmptyArray
						|| Values->IsEmpty());
			}
		}
		TestTrue(
			*FString::Printf(
				TEXT("%s returns only its minimal empty business result"),
				*SpatialCase.CommandId),
			bMinimal);
	}
	struct FExpandedSpatialCase final
	{
		FString CommandId;
		FString ParamsJson;
		FString ArrayField;
	};
	const TArray<FExpandedSpatialCase> ExpandedSpatialCases = {
		{
			TEXT("world.overlap"),
			TEXT("{\"input\":{\"shape\":\"sphere\",\"center\":{\"frame_id\":\"ue.world\",\"unit\":\"centimeter\",\"components\":[100000,100000,100000]},\"radius_cm\":1,\"include_targets\":true}}"),
			TEXT("targets"),
		},
		{
			TEXT("world.capsule_sweep"),
			TEXT("{\"input\":{\"start\":{\"frame_id\":\"ue.world\",\"unit\":\"centimeter\",\"components\":[100000,100000,100000]},\"end\":{\"frame_id\":\"ue.world\",\"unit\":\"centimeter\",\"components\":[100010,100000,100000]},\"radius_cm\":1,\"half_height_cm\":2,\"include_hits\":true}}"),
			TEXT("hits"),
		},
		{
			TEXT("world.nearby_shapes"),
			TEXT("{\"input\":{\"center\":{\"frame_id\":\"ue.world\",\"unit\":\"centimeter\",\"components\":[100000,100000,100000]},\"radius_cm\":1,\"include_shapes\":true}}"),
			TEXT("shapes"),
		},
	};
	for (const FExpandedSpatialCase& SpatialCase : ExpandedSpatialCases)
	{
		const FUeAgentCommandResult SpatialResult = Submit(
			Public,
			SpatialCase.CommandId,
			SpatialCase.ParamsJson);
		TSharedPtr<FJsonObject> SpatialPayload;
		JsonError.Reset();
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		TestTrue(
			*FString::Printf(
				TEXT("%s expands its collection only on request"),
				*SpatialCase.CommandId),
			SpatialResult.IsOk()
				&& UE::UAI::CommandContract::Json::TryParseObject(
					SpatialResult.GetPayloadJson(),
					SpatialPayload,
					JsonError)
				&& SpatialPayload.IsValid()
				&& SpatialPayload->Values.Num() == 2
				&& SpatialPayload->TryGetArrayField(
					SpatialCase.ArrayField,
					Values)
				&& Values);
	}

	for (const TPair<FString, FString>& GroupedTopology : {
		TPair<FString, FString>(TEXT("world.data_layers"), TEXT("data_layers")),
		TPair<FString, FString>(TEXT("world.hlod_assignments"), TEXT("assignments")),
		TPair<FString, FString>(TEXT("world.streaming_levels"), TEXT("levels")),
	})
	{
		const FUeAgentCommandResult TopologyResult = Submit(
			Public,
			GroupedTopology.Key,
			TEXT("{}"));
		TSharedPtr<FJsonObject> TopologyPayload;
		JsonError.Reset();
		const TArray<TSharedPtr<FJsonValue>>* Records = nullptr;
		TestTrue(
			*FString::Printf(
				TEXT("%s returns its direct normalized collection"),
				*GroupedTopology.Key),
			TopologyResult.IsOk()
				&& UE::UAI::CommandContract::Json::TryParseObject(
					TopologyResult.GetPayloadJson(),
					TopologyPayload,
					JsonError)
				&& TopologyPayload.IsValid()
				&& TopologyPayload->Values.Num() == 1
				&& TopologyPayload->TryGetArrayField(
					GroupedTopology.Value,
					Records)
				&& Records);
	}

	const FUeAgentCommandResult PartitionDefault = Submit(
		Public,
		TEXT("world.partition"),
		TEXT("{}"));
	TSharedPtr<FJsonObject> PartitionDefaultPayload;
	JsonError.Reset();
	TestTrue(
		TEXT("world.partition defaults to the compact three-field summary"),
		PartitionDefault.IsOk()
			&& UE::UAI::CommandContract::Json::TryParseObject(
				PartitionDefault.GetPayloadJson(),
				PartitionDefaultPayload,
				JsonError)
			&& PartitionDefaultPayload.IsValid()
			&& PartitionDefaultPayload->Values.Num() == 3
			&& PartitionDefaultPayload->HasField(TEXT("enabled"))
			&& PartitionDefaultPayload->HasField(TEXT("streaming_enabled"))
			&& PartitionDefaultPayload->HasField(TEXT("loaded_region_count"))
			&& !PartitionDefaultPayload->HasField(TEXT("loaded_regions")));
	const FUeAgentCommandResult PartitionExpanded = Submit(
		Public,
		TEXT("world.partition"),
		TEXT("{\"input\":{\"include_loaded_regions\":true}}"));
	TSharedPtr<FJsonObject> PartitionExpandedPayload;
	JsonError.Reset();
	const TArray<TSharedPtr<FJsonValue>>* LoadedRegions = nullptr;
	TestTrue(
		TEXT("world.partition expands region geometry only on request"),
		PartitionExpanded.IsOk()
			&& UE::UAI::CommandContract::Json::TryParseObject(
				PartitionExpanded.GetPayloadJson(),
				PartitionExpandedPayload,
				JsonError)
			&& PartitionExpandedPayload.IsValid()
			&& PartitionExpandedPayload->Values.Num() == 4
			&& PartitionExpandedPayload->TryGetArrayField(
				TEXT("loaded_regions"),
				LoadedRegions)
			&& LoadedRegions);

	const FUeAgentCommandResult Health = Submit(
		Public,
		TEXT("world.partition_health"),
		TEXT("{}"));
	TSharedPtr<FJsonObject> HealthPayload;
	JsonError.Reset();
	const TArray<TSharedPtr<FJsonValue>>* Issues = nullptr;
	TestTrue(
		TEXT("world.partition_health returns only status, count and normalized issues"),
		Health.IsOk()
			&& UE::UAI::CommandContract::Json::TryParseObject(
				Health.GetPayloadJson(),
				HealthPayload,
				JsonError)
			&& HealthPayload.IsValid()
			&& HealthPayload->Values.Num() == 4
			&& HealthPayload->HasField(TEXT("partition_enabled"))
			&& HealthPayload->HasField(TEXT("status"))
			&& HealthPayload->HasField(TEXT("error_count"))
			&& HealthPayload->TryGetArrayField(TEXT("issues"), Issues)
			&& Issues);

	const FUeAgentCommandResult PlatformNames = Submit(
		Public,
		TEXT("project.platform"),
		TEXT("{}"));
	TestTrue(
		TEXT("the default platform query succeeds through exact dispatch"),
		PlatformNames.IsOk());
	TSharedPtr<FJsonObject> PlatformNamesPayload;
	if (PlatformNames.IsOk()
		&& UE::UAI::CommandContract::Json::TryParseObject(
			PlatformNames.GetPayloadJson(),
			PlatformNamesPayload,
			JsonError))
	{
		const TArray<TSharedPtr<FJsonValue>>* Platforms = nullptr;
		TestTrue(
			TEXT("the default platform query directly returns a non-empty names list"),
			PlatformNamesPayload.IsValid()
				&& PlatformNamesPayload->Values.Num() == 1
				&& PlatformNamesPayload->TryGetArrayField(TEXT("platforms"), Platforms)
				&& Platforms
				&& !Platforms->IsEmpty());
		if (Platforms)
		{
			for (const TSharedPtr<FJsonValue>& Platform : *Platforms)
			{
				TestTrue(
					TEXT("each default platform result is only its readable name"),
					Platform.IsValid()
						&& Platform->Type == EJson::String
						&& !Platform->AsString().IsEmpty());
			}
		}
	}
	else if (!PlatformNames.IsOk())
	{
		AddError(PlatformNames.GetErrorJson());
	}
	else
	{
		AddError(JsonError);
	}

	const FUeAgentCommandResult PlatformSdkState = Submit(
		Public,
		TEXT("project.platform"),
		TEXT("{\"input\":{\"field_ids\":[\"sdk_installed\"]}}"));
	TestTrue(
		TEXT("an explicit platform field query succeeds through exact dispatch"),
		PlatformSdkState.IsOk());
	TSharedPtr<FJsonObject> PlatformSdkPayload;
	if (PlatformSdkState.IsOk()
		&& UE::UAI::CommandContract::Json::TryParseObject(
			PlatformSdkState.GetPayloadJson(),
			PlatformSdkPayload,
			JsonError))
	{
		const TArray<TSharedPtr<FJsonValue>>* Platforms = nullptr;
		TestTrue(
			TEXT("an explicit platform field query directly returns observations"),
			PlatformSdkPayload.IsValid()
				&& PlatformSdkPayload->Values.Num() == 1
				&& PlatformSdkPayload->TryGetArrayField(TEXT("platforms"), Platforms)
				&& Platforms
				&& !Platforms->IsEmpty());
		if (Platforms)
		{
			for (const TSharedPtr<FJsonValue>& Platform : *Platforms)
			{
				const TSharedPtr<FJsonObject> Observation =
					Platform.IsValid() && Platform->Type == EJson::Object
						? Platform->AsObject()
						: nullptr;
				TestTrue(
					TEXT("an explicit platform observation contains only its name and requested field"),
					Observation.IsValid()
						&& Observation->Values.Num() == 2
						&& Observation->HasTypedField<EJson::String>(TEXT("platform_name"))
						&& Observation->HasTypedField<EJson::Boolean>(TEXT("sdk_installed")));
			}
		}
	}
	else if (!PlatformSdkState.IsOk())
	{
		AddError(PlatformSdkState.GetErrorJson());
	}
	else
	{
		AddError(JsonError);
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
		const TArray<TSharedPtr<FJsonValue>>* EmptySelectedObjects = nullptr;
		TestTrue(
			TEXT("empty selection response exposes one direct empty list"),
			UE::UAI::CommandContract::Json::TryParseObject(
				EmptySelection.GetPayloadJson(),
				SelectionPayload,
				JsonError)
				&& SelectionPayload.IsValid()
				&& SelectionPayload->Values.Num() == 1
				&& SelectionPayload->TryGetArrayField(
					TEXT("selected_objects"),
					EmptySelectedObjects)
				&& EmptySelectedObjects
				&& EmptySelectedObjects->IsEmpty());
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
}

#include "Misc/AutomationTest.h"

#include "Composition/UeAgentCommandServiceComposition.h"
#include "Composition/UeAgentQueryServiceComposition.h"
#include "Composition/LongRunningSession/UeAgentLongRunningSessionActivation.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundActivation.h"
#include "Composition/TimelineSpatial/UeAgentTimelineSpatialActivation.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "GameFramework/WorldSettings.h"
#include "NetworkTransport/UeAgentHttpTransportAdapter.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"

using namespace UE::UAI::CommandContract;
using namespace UE::UAI::CommandEdge;
using namespace UE::UAI::Composition;
using namespace UE::UAI::NetworkTransport;
using namespace UE::UAI::Tests::PublicCommand;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentQueryServiceCompositionTimelineSpatialPublicRouteTest,
	"UeAgentInterface.Composition.TimelineSpatial.E3."
		"QueryServiceProductionActivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentQueryServiceCompositionTimelineSpatialPublicRouteTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Composition::TimelineSpatial;
	UWorld* World = GEditor
		? GEditor->GetEditorWorldContext().World()
		: nullptr;
	AWorldSettings* WorldSettings =
		World ? World->GetWorldSettings() : nullptr;
	if (!TestNotNull(TEXT("editor world settings"), WorldSettings))
	{
		return false;
	}

	const TSharedRef<FFakeRouteHost, ESPMode::ThreadSafe> RouteHost =
		MakeShared<FFakeRouteHost, ESPMode::ThreadSafe>();
	TUniquePtr<FUeAgentQueryServiceComposition> Composition;
	FUeAgentQueryServiceError Error;
	const FUeAgentQueryServiceConfig ServiceConfig = MakeConfig();
	if (!TestTrue(
			TEXT("QueryService composes the 303-command production graph"),
			FUeAgentQueryServiceComposition::TryCreateReference(
				ServiceConfig,
				*WorldSettings,
				RouteHost,
				Composition,
				Error))
		|| !Composition)
	{
		AddError(Error.Code + TEXT(":") + Error.Detail);
		return false;
	}

	FUeAgentTransportDiagnostic TransportError;
	if (!TestTrue(
			TEXT("one shared public route starts"),
			Composition->Start(TransportError)))
	{
		return false;
	}
	TestEqual(
		TEXT("TimelineSpatial adds no second transport route"),
		RouteHost->Handlers.Num(),
		1);

	int32 RecognizedQueries = 0;
	int32 RecognizedOperations = 0;
	for (const FUeAgentTimelineSpatialActivationProfile& Profile
		: GetTimelineSpatialActivationProfiles())
	{
		TSharedRef<FJsonObject> PublicParams = MakeShared<FJsonObject>();
		if (Profile.bRequiresTarget)
		{
			PublicParams->SetStringField(
				TEXT("target"),
				TEXT("/Game/UAI/ExpectedInvalidTarget"));
		}
		PublicParams->SetObjectField(TEXT("input"), MakeShared<FJsonObject>());
		if (Profile.ContextSource == TEXT("public_required"))
		{
			PublicParams->SetObjectField(
				TEXT("context"),
				MakeShared<FJsonObject>());
		}
		FString ParamsJson;
		const TSharedRef<TJsonWriter<
			TCHAR,
			TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<
				TCHAR,
				TCondensedJsonPrintPolicy<TCHAR>>::Create(&ParamsJson);
		FJsonSerializer::Serialize(PublicParams, Writer);

		TOptional<FUeAgentCommandResult> Result;
		FString InvokeError;
		const bool bInvoked = TryInvokeCommand(
			*RouteHost,
			ServiceConfig,
			Profile.CommandId,
			ParamsJson,
			Result,
			InvokeError);
		TestTrue(
			TEXT("production ingress handles exact command: ")
				+ Profile.CommandId,
			bInvoked && Result.IsSet());
		if (!bInvoked || !Result.IsSet())
		{
			AddError(Profile.CommandId + TEXT(":") + InvokeError);
			continue;
		}
		const FString& ErrorJson = Result->GetErrorJson();
		TestFalse(
			TEXT("command id is active: ") + Profile.CommandId,
			ErrorJson.Contains(TEXT("command_id_unknown")));
		TestFalse(
			TEXT("binding hash is active: ") + Profile.CommandId,
			ErrorJson.Contains(TEXT("command_binding_unknown")));
		TestFalse(
			TEXT("one handler owns the command: ") + Profile.CommandId,
			ErrorJson.Contains(TEXT("command_handler_not_found")));
		if (Profile.OperationKind == TEXT("query"))
		{
			++RecognizedQueries;
		}
		else
		{
			++RecognizedOperations;
		}
	}
	TestEqual(TEXT("all C3Q commands are active"), RecognizedQueries, 24);
	TestEqual(TEXT("all C3O commands are active"), RecognizedOperations, 35);

	int32 RecognizedLongRunningCommands = 0;
	for (const LongRunningSession::
		FUeAgentLongRunningSessionActivationProfile& Profile
		: LongRunningSession::GetLongRunningSessionActivationProfiles())
	{
		TOptional<FUeAgentCommandResult> Result;
		FString InvokeError;
		const bool bInvoked = TryInvokeCommand(
			*RouteHost,
			ServiceConfig,
			Profile.CommandId,
			TEXT("{}"),
			Result,
			InvokeError);
		TestTrue(
			TEXT("production ingress handles exact long-running command: ")
				+ Profile.CommandId,
			bInvoked && Result.IsSet());
		if (!bInvoked || !Result.IsSet())
		{
			AddError(Profile.CommandId + TEXT(":") + InvokeError);
			continue;
		}
		TestFalse(
			TEXT("long-running command id is active: ") + Profile.CommandId,
			Result->GetErrorJson().Contains(TEXT("command_id_unknown")));
		TestFalse(
			TEXT("long-running binding is active: ") + Profile.CommandId,
			Result->GetErrorJson().Contains(TEXT("command_binding_unknown")));
		TestFalse(
			TEXT("long-running command has one handler: ") + Profile.CommandId,
			Result->GetErrorJson().Contains(TEXT("command_handler_not_found")));
		++RecognizedLongRunningCommands;
	}
	TestEqual(
		TEXT("all long-running commands are active"),
		RecognizedLongRunningCommands,
		12);

	int32 RecognizedGraphQueries = 0;
	int32 RecognizedGraphOperations = 0;
	for (const GraphCompound::FUeAgentGraphCompoundActivationProfile& Profile
		: GraphCompound::GetGraphCompoundActivationProfiles())
	{
		TOptional<FUeAgentCommandResult> Result;
		FString InvokeError;
		const bool bInvoked = TryInvokeCommand(
			*RouteHost,
			ServiceConfig,
			Profile.GetCommandId(),
			TEXT("{}"),
			Result,
			InvokeError);
		TestTrue(TEXT("production ingress handles exact Graph command: ")
			+ Profile.GetCommandId(), bInvoked && Result.IsSet());
		if (!bInvoked || !Result.IsSet())
		{
			AddError(Profile.GetCommandId() + TEXT(":") + InvokeError);
			continue;
		}
		TestFalse(TEXT("Graph command id is active: ") + Profile.GetCommandId(),
			Result->GetErrorJson().Contains(TEXT("command_id_unknown")));
		TestFalse(TEXT("Graph binding is active: ") + Profile.GetCommandId(),
			Result->GetErrorJson().Contains(TEXT("command_binding_unknown")));
		TestFalse(TEXT("Graph command has one handler: ") + Profile.GetCommandId(),
			Result->GetErrorJson().Contains(TEXT("command_handler_not_found")));
		if (Profile.GetOperationKind() == TEXT("query"))
		{
			++RecognizedGraphQueries;
		}
		else
		{
			++RecognizedGraphOperations;
		}
	}
	TestEqual(TEXT("all Graph C3Q commands are active"), RecognizedGraphQueries, 3);
	TestEqual(TEXT("all Graph C3O commands are active"), RecognizedGraphOperations, 76);

	TOptional<FUeAgentCommandResult> HjsonResult;
	FString HjsonInvokeError;
	TestTrue(
		TEXT("generic HJSON import reaches its unique production handler"),
		TryInvokeCommand(
			*RouteHost,
			ServiceConfig,
			TEXT("content_graph.import_workspace"),
			TEXT("{\"target\":\"/Game/UAI/ExpectedInvalidTarget\","
				 "\"source\":{},\"intent\":\"update\","
				 "\"expected_revision\":\"expected-red\"}"),
			HjsonResult,
			HjsonInvokeError));
	if (HjsonResult.IsSet())
	{
		TestFalse(
			TEXT("HJSON command id is active"),
			HjsonResult->GetErrorJson().Contains(TEXT("command_id_unknown")));
		TestFalse(
			TEXT("HJSON binding hash is active"),
			HjsonResult->GetErrorJson().Contains(TEXT("command_binding_unknown")));
		TestFalse(
			TEXT("HJSON has one handler"),
			HjsonResult->GetErrorJson().Contains(TEXT("command_handler_not_found")));
	}
	else
	{
		AddError(HjsonInvokeError);
	}

	TestTrue(
		TEXT("owned command route stops cleanly"),
		Composition->Stop(TransportError));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentQueryServiceCompositionTimelineSpatialTargetlessCreateTest,
	"UeAgentInterface.Composition.TimelineSpatial.E3."
		"QueryServiceTargetlessCreateThroughUnifiedC3O",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentQueryServiceCompositionTimelineSpatialTargetlessCreateTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = GEditor
		? GEditor->GetEditorWorldContext().World()
		: nullptr;
	AWorldSettings* WorldSettings =
		World ? World->GetWorldSettings() : nullptr;
	if (!TestNotNull(TEXT("editor world settings"), WorldSettings))
	{
		return false;
	}

	const TSharedRef<FFakeRouteHost, ESPMode::ThreadSafe> RouteHost =
		MakeShared<FFakeRouteHost, ESPMode::ThreadSafe>();
	TUniquePtr<FUeAgentQueryServiceComposition> Composition;
	FUeAgentQueryServiceError Error;
	const FUeAgentQueryServiceConfig ServiceConfig = MakeConfig();
	if (!TestTrue(
			TEXT("production composition creates"),
			FUeAgentQueryServiceComposition::TryCreateReference(
				ServiceConfig,
				*WorldSettings,
				RouteHost,
				Composition,
				Error))
		|| !Composition)
	{
		AddError(Error.Code + TEXT(":") + Error.Detail);
		return false;
	}

	FUeAgentTransportDiagnostic TransportError;
	if (!TestTrue(TEXT("public route starts"), Composition->Start(TransportError)))
	{
		AddError(TransportError.Code + TEXT(":") + TransportError.Message);
		return false;
	}

	const FString Destination =
		TEXT("/Game/__UAI_TS012/UnifiedBindingCreate.UnifiedBindingCreate");
	const FString Clock = TEXT("level_sequence:") + Destination;
	const FString Rate =
		TEXT("{\"numerator\":\"30\",\"denominator\":\"1\"}");
	const FString Lower =
		FString(TEXT("{\"kind\":\"inclusive\",\"point\":{"))
		+ TEXT("\"clock_id\":\"") + Clock
		+ TEXT("\",\"tick\":\"0\",\"rate\":") + Rate
		+ TEXT(",\"state\":\"authored\"}}");
	const FString Upper =
		FString(TEXT("{\"kind\":\"exclusive\",\"point\":{"))
		+ TEXT("\"clock_id\":\"") + Clock
		+ TEXT("\",\"tick\":\"60\",\"rate\":") + Rate
		+ TEXT(",\"state\":\"authored\"}}");
	const FString ParamsJson =
		TEXT("{\"input\":{\"destination_ref\":\"") + Destination
		+ TEXT("\",\"initial_structure\":{")
		+ TEXT("\"display_rate\":") + Rate
		+ TEXT(",\"tick_resolution\":") + Rate
		+ TEXT(",\"playback_range\":{\"clock_id\":\"") + Clock
		+ TEXT("\",\"state\":\"authored\",\"lower\":") + Lower
		+ TEXT(",\"upper\":") + Upper
		+ TEXT("},\"bindings\":[],\"root_tracks\":[]}}}");

	TOptional<FUeAgentCommandResult> Result;
	FString InvokeError;
	if (!TestTrue(
			TEXT("targetless create reaches public command edge"),
			TryInvokeCommand(
				*RouteHost,
				ServiceConfig,
				TEXT("timeline.sequence.create"),
				ParamsJson,
				Result,
				InvokeError))
		|| !Result.IsSet())
	{
		AddError(InvokeError);
		return false;
	}
	TestTrue(TEXT("unified C3O create succeeds"), Result->IsOk());
	if (!Result->IsOk())
	{
		AddError(Result->GetErrorJson());
		return false;
	}
	TestTrue(
		TEXT("C3O result includes operation identity"),
		Result->GetPayloadJson().Contains(TEXT("operation_id")));
	TestTrue(
		TEXT("C3O result includes verified projection"),
		Result->GetPayloadJson().Contains(TEXT("projection")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentQueryServiceCompositionPublicReadExportRouteTest,
	"UeAgentInterface.Composition.ReadExport.E3."
		"PublicCommandRouteAndProductionDispatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentQueryServiceCompositionPublicReadExportRouteTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = GEditor
		? GEditor->GetEditorWorldContext().World()
		: nullptr;
	AWorldSettings* WorldSettings =
		World ? World->GetWorldSettings() : nullptr;
	if (!TestNotNull(TEXT("editor world settings"), WorldSettings))
	{
		return false;
	}

	const TSharedRef<FFakeRouteHost, ESPMode::ThreadSafe> RouteHost =
		MakeShared<FFakeRouteHost, ESPMode::ThreadSafe>();
	TUniquePtr<FUeAgentQueryServiceComposition> Composition;
	FUeAgentQueryServiceError Error;
	const FUeAgentQueryServiceConfig ServiceConfig = MakeConfig();
	TestTrue(
		TEXT("production read/export graph composes"),
		FUeAgentQueryServiceComposition::TryCreateReference(
			ServiceConfig,
			*WorldSettings,
			RouteHost,
			Composition,
			Error));
	if (!Composition)
	{
		AddError(Error.Code + TEXT(":") + Error.Detail);
		return false;
	}

	FUeAgentTransportDiagnostic TransportError;
	TestTrue(
		TEXT("composition activates the public command route"),
		Composition->Start(TransportError));
	TestEqual(
		TEXT("only one public transport route is active"),
		RouteHost->Handlers.Num(),
		1);
	TestTrue(
		TEXT("formal command route is bound"),
		RouteHost->Handlers.Contains(GetUaiCommandRoutePath()));
	TestFalse(
		TEXT("legacy reference route is inactive"),
		RouteHost->Handlers.Contains(TEXT("/api/uai/reference")));
	TestFalse(
		TEXT("legacy export route is inactive"),
		RouteHost->Handlers.Contains(TEXT("/api/uai/export")));

	TOptional<FUeAgentCommandResult> Result;
	FString InvokeError;
	TestTrue(
		TEXT("project.summary reaches its production domain provider"),
		TryInvokeCommand(
			*RouteHost,
			ServiceConfig,
			TEXT("project.summary"),
			TEXT("{}"),
			Result,
			InvokeError));
	TestTrue(
		TEXT("production project.summary succeeds"),
		Result.IsSet() && Result->IsOk());
	if (!InvokeError.IsEmpty())
	{
		AddError(InvokeError);
	}
	if (Result.IsSet() && !Result->IsOk())
	{
		AddError(Result->GetErrorJson());
	}

	Result.Reset();
	InvokeError.Reset();
	TestTrue(
		TEXT("a typed Simple Authoring command reaches the same public route"),
		TryInvokeCommand(
			*RouteHost,
			ServiceConfig,
			TEXT("editor_project.project_settings.update"),
			TEXT("{}"),
			Result,
			InvokeError));
	TestTrue(
		TEXT("invalid authoring input is rejected by C3O, not by a missing command owner"),
		Result.IsSet() && !Result->IsOk());
	TestFalse(
		TEXT("the production ingress recognizes the typed command id"),
		Result.IsSet()
			&& Result->GetErrorJson().Contains(TEXT("command_id_unknown")));
	if (!InvokeError.IsEmpty())
	{
		AddError(InvokeError);
	}

	Result.Reset();
	InvokeError.Reset();
	TestTrue(
		TEXT("the shared Query service reaches the EditorProject field catalog"),
		TryInvokeCommand(
			*RouteHost,
			ServiceConfig,
			TEXT("reference.catalog_search"),
			TEXT("{\"catalog_kind\":\"editor_project_settings_field\"," 
				 "\"search_text\":\"\",\"match_mode\":\"broad\"," 
				 "\"materialization_policy\":\"index_only\"}"),
			Result,
			InvokeError));
	TestTrue(
		TEXT("a multi-kind catalog uses the source identity required by C2"),
		Result.IsSet() && Result->IsOk());
	if (!InvokeError.IsEmpty())
	{
		AddError(InvokeError);
	}
	if (Result.IsSet() && !Result->IsOk())
	{
		AddError(Result->GetErrorJson());
	}

	TestTrue(
		TEXT("owned command route stops cleanly"),
		Composition->Stop(TransportError));
	TestTrue(
		TEXT("owned command route is unbound"),
		RouteHost->Handlers.IsEmpty());
	return true;
}

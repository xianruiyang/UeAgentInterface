#include "Misc/AutomationTest.h"

#include "Composition/DestructiveImport/UeAgentDestructiveImportActivation.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Editor.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Server/UeAgentCommandRegistry.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"

namespace
{
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::Composition::DestructiveImport;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::NetworkTransport;
	using namespace UE::UAI::Tests::PublicCommand;

	const TArray<FString> CanonicalIds{
		TEXT("asset.delete"),
		TEXT("asset.move"),
		TEXT("asset.fixup_redirectors"),
		TEXT("asset.reimport_texture"),
		TEXT("asset.reimport_sound_wave"),
		TEXT("asset.reimport_static_mesh"),
		TEXT("asset.reimport_sparse_volume_texture"),
		TEXT("skeletal_mesh.remove_morph_target"),
		TEXT("skeletal_mesh.remove_skin_weight_profile"),
		TEXT("world.data_layer.delete"),
		TEXT("world.level_content.delete_scope"),
		TEXT("world.pcg.cleanup"),
		TEXT("world.level_streaming.move_actors"),
		TEXT("world.level_streaming.remove"),
		TEXT("modeling.actor.mesh.replace"),
		TEXT("modeling.asset.material_slot.remove"),
		TEXT("modeling.voxel.bounds.remove"),
		TEXT("external.fbx.animation.import"),
		TEXT("external.fbx.skeletal_mesh.import"),
		TEXT("external.geometry_cache.import"),
		TEXT("external.texture.import"),
		TEXT("external.light_profile.import"),
		TEXT("external.sparse_volume_texture.import"),
		TEXT("external.sound_wave.import"),
		TEXT("external.localization.po.import"),
		TEXT("external.skin_weight_profile.import"),
	};

	const TArray<FString> LegacyIds{
		TEXT("asset_delete"),
		TEXT("asset_move"),
		TEXT("asset_fixup_redirectors"),
		TEXT("texture_reimport"),
		TEXT("sound_wave_reimport"),
		TEXT("static_mesh_reimport"),
		TEXT("sparse_volume_texture_reimport"),
		TEXT("skeletal_mesh_remove_morph_target"),
		TEXT("skeletal_mesh_remove_skin_weight_profile"),
		TEXT("data_layer_delete_instance"),
		TEXT("level_content_delete_scope"),
		TEXT("pcg_cleanup"),
		TEXT("level_streaming_move_actors"),
		TEXT("level_streaming_remove"),
		TEXT("modeling_replace_actor_mesh"),
		TEXT("modeling_remove_material_slot"),
		TEXT("modeling_voxel_grid_remove_bounds"),
		TEXT("asset_import_fbx_animation"),
		TEXT("asset_import_fbx_skeletal_mesh"),
		TEXT("asset_import_geometry_cache"),
		TEXT("asset_import_texture"),
		TEXT("light_profile_import"),
		TEXT("sparse_volume_texture_import"),
		TEXT("sound_wave_import"),
		TEXT("localization_import_po"),
		TEXT("skeletal_mesh_import_skin_weight_profile"),
	};

	FString Serialize(const TSharedRef<FJsonObject>& Object)
	{
		FString Text;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>>
			Writer = TJsonWriterFactory<
				TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Object, Writer);
		return Text;
	}

	FString InvalidButWellTypedParams(const FUeAgentCommandContract& Contract)
	{
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		for (const FUeAgentCommandParameter& Parameter : Contract.GetParameters())
		{
			if (!Parameter.IsRequired()) continue;
			switch (Parameter.GetWireType())
			{
			case EUeAgentCommandWireType::Boolean:
				Params->SetBoolField(Parameter.GetName(), false);
				break;
			case EUeAgentCommandWireType::Integer:
			case EUeAgentCommandWireType::Number:
				Params->SetNumberField(Parameter.GetName(), 0.0);
				break;
			case EUeAgentCommandWireType::String:
				Params->SetStringField(Parameter.GetName(), TEXT("invalid"));
				break;
			case EUeAgentCommandWireType::Array:
				Params->SetArrayField(Parameter.GetName(), {});
				break;
			case EUeAgentCommandWireType::Object:
			case EUeAgentCommandWireType::Any:
				Params->SetObjectField(
					Parameter.GetName(), MakeShared<FJsonObject>());
				break;
			default:
				checkNoEntry();
				break;
			}
		}
		return Serialize(Params);
	}

	bool TryInvoke(
		const FScopedPublicCommandComposition& Harness,
		const FUeAgentCommandContract& Contract,
		TOptional<FUeAgentCommandResult>& OutResult,
		FString& OutError)
	{
		OutResult.Reset();
		OutError.Reset();
		const FString RequestId = TEXT("destructive-activation-")
			+ Contract.GetCommandId().Replace(TEXT("."), TEXT("-"))
			+ TEXT("-") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString CorrelationId = RequestId + TEXT("-correlation");
		const FString Body =
			TEXT("{\"command_binding_hash\":\"") + Contract.GetBindingHash()
			+ TEXT("\",\"command_id\":\"") + Contract.GetCommandId()
			+ TEXT("\",\"correlation_id\":\"") + CorrelationId
			+ TEXT("\",\"message_kind\":\"command_request\",\"params\":")
			+ InvalidButWellTypedParams(Contract)
			+ TEXT(",\"request_id\":\"") + RequestId
			+ TEXT("\",\"schema\":\"uai.command.v1\",\"version\":1}");
		FUeAgentHttpRequest Request;
		Request.Method = TEXT("POST");
		Request.RoutePath = GetUaiCommandRoutePath();
		Request.Headers.Add(
			TEXT("X-UeAgentInterface-Token"), Harness.GetConfig().AuthToken);
		Request.Headers.Add(TEXT("X-Transport-Correlation"), CorrelationId);
		Request.Body = Utf8Bytes(Body);
		Request.PeerId = TEXT("destructive-activation-test-peer");
		const FUeAgentHttpResponse Response = Harness.GetRouteHost().Invoke(Request);
		if (Response.StatusCode != 200)
		{
			OutError = FString::Printf(
				TEXT("command_http_status:%d"), Response.StatusCode);
			return false;
		}
		FUeAgentCommandProtocolDiagnostic Diagnostic;
		FUeAgentCommandProtocolCodec Codec;
		if (!Codec.DecodeResult(Response.Body, OutResult, Diagnostic)
			|| !OutResult.IsSet())
		{
			OutError = Diagnostic.Code + TEXT(":") + Diagnostic.Message;
			return false;
		}
		return true;
	}

	bool IsMissingOwner(const FString& Error)
	{
		return Error.Contains(TEXT("command_handler"), ESearchCase::IgnoreCase)
			|| Error.Contains(TEXT("command_dispatch"), ESearchCase::IgnoreCase)
			|| Error.Contains(TEXT("command_unknown"), ESearchCase::IgnoreCase)
			|| Error.Contains(TEXT("command_not_registered"), ESearchCase::IgnoreCase)
			|| Error.Contains(TEXT("unsupported_command"), ESearchCase::IgnoreCase);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDestructiveImportPublicActivationAndLegacyAbsenceTest,
	"UeAgentInterface.Composition.DestructiveImport.PublicActivationAndLegacyAbsence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDestructiveImportPublicActivationAndLegacyAbsenceTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	TestEqual(TEXT("destructive/import canonical command count"),
		CanonicalIds.Num(), 26);
	TestEqual(TEXT("destructive/import retired legacy command count"),
		LegacyIds.Num(), 26);
	TSharedPtr<const FUeAgentCommandContractRegistry> Registry;
	FString Error;
	if (!TestTrue(TEXT("outer destructive/import registry is available"),
			TryCreateDestructiveImportProductionCommandContractRegistry(
				Registry, Error))
		|| !Registry.IsValid())
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("outer production registry contains 329 commands"),
		Registry->GetContracts().Num(), 329);

	AWorldSettings* const WorldSettings = GEditor
		? GEditor->GetEditorWorldContext().World()->GetWorldSettings() : nullptr;
	if (!TestNotNull(TEXT("editor world settings"), WorldSettings)) return false;
	FScopedPublicCommandComposition Harness(TEXT("destructive-public-activation"));
	if (!TestTrue(TEXT("formal public composition starts"),
			Harness.TryStart(*WorldSettings, Error)))
	{
		AddError(Error);
		return false;
	}

	TArray<FString> MissingOwners;
	for (const FString& CommandId : CanonicalIds)
	{
		const FUeAgentCommandContract* const Contract = Registry->FindExact(CommandId);
		if (!Contract)
		{
			MissingOwners.Add(CommandId + TEXT(":manifest"));
			continue;
		}
		TOptional<FUeAgentCommandResult> Result;
		if (!TryInvoke(Harness, *Contract, Result, Error) || !Result.IsSet())
		{
			MissingOwners.Add(CommandId + TEXT(":") + Error);
			continue;
		}
		if (!Result->IsOk() && IsMissingOwner(Result->GetErrorJson()))
		{
			MissingOwners.Add(CommandId + TEXT(":") + Result->GetErrorJson());
			continue;
		}
		TSharedPtr<FJsonObject> Payload;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Result->GetPayloadJson());
		if (Result->IsOk()
			|| !FJsonSerializer::Deserialize(Reader, Payload)
			|| !Payload.IsValid()
			|| Payload->GetStringField(TEXT("terminal_state"))
				!= TEXT("rolled_back"))
		{
			MissingOwners.Add(CommandId + TEXT(":failure_semantics"));
		}
	}
	if (!MissingOwners.IsEmpty())
	{
		AddError(FString::Printf(
			TEXT("%d destructive/import commands lack one formal owner or rolled-back failure semantics: %s"),
			MissingOwners.Num(), *FString::Join(MissingOwners, TEXT(" | "))));
	}
	TestEqual(TEXT("all 26 commands enter one formal public owner"),
		MissingOwners.Num(), 0);

	for (const FString& LegacyId : LegacyIds)
	{
		TestNull(TEXT("retired exact owner is absent: ") + LegacyId,
			UeAgentCommandRegistry::FindExactCommandInvoker(LegacyId));
		TestNull(TEXT("retired prefix owner is absent: ") + LegacyId,
			UeAgentCommandRegistry::FindPrefixedCommandInvoker(LegacyId));
		TestTrue(TEXT("retired route fails closed: ") + LegacyId,
			UeAgentCommandRegistry::IsExplicitlyUnsupportedCommand(LegacyId));
	}

	const FString ManifestPath = GetDefaultCommandManifestPath();
	const bool bUpdateManifest = FParse::Param(
		FCommandLine::Get(), TEXT("UaiUpdateCommandManifest"))
		|| FPlatformMisc::GetEnvironmentVariable(
			TEXT("UAI_UPDATE_COMMAND_MANIFEST")) == TEXT("1");
	if (bUpdateManifest)
	{
		Error.Reset();
		TestTrue(TEXT("outer C++ owner writes the 329-command manifest"),
			FUeAgentCommandManifestExporter::TryWrite(
				*Registry, GetUaiCommandManifestPluginVersion(),
				GetUaiCommandManifestBuildId(), ManifestPath, Error));
	}
	Error.Reset();
	TestTrue(TEXT("committed manifest matches outer destructive/import registry"),
		FUeAgentCommandManifestExporter::TryVerify(
			*Registry, GetUaiCommandManifestPluginVersion(),
			GetUaiCommandManifestBuildId(), ManifestPath, Error));
	if (!Error.IsEmpty()) AddError(Error);

	TestTrue(TEXT("formal public composition stops"), Harness.Stop());
	TestEqual(TEXT("command route is released on stop"),
		Harness.GetRouteHost().Handlers.Num(), 0);
	TestEqual(TEXT("instance ownership is released on stop"),
		Harness.GetRouteHost().RouteByInstance.Num(), 0);
	return !HasAnyErrors();
}

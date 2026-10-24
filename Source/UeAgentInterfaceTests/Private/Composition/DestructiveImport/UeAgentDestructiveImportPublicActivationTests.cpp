#include "Misc/AutomationTest.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Editor.h"
#include "GameFramework/WorldSettings.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"

namespace
{
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::NetworkTransport;
	using namespace UE::UAI::Tests::PublicCommand;

	const TArray<FString> CanonicalIds{
		TEXT("asset.delete"),
		TEXT("asset.move"),
		TEXT("asset.fixup_redirectors"),
		TEXT("asset.object.clone"),
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

	FString Serialize(const TSharedRef<FJsonObject>& Object)
	{
		FString Text;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>>
			Writer = TJsonWriterFactory<
				TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Object, Writer);
		return Text;
	}

	FString ExpectedOwner(const FString& CommandId)
	{
		if (CommandId == TEXT("external.localization.po.import"))
		{
			return TEXT("localization-import-owner");
		}
		if (CommandId == TEXT("external.skin_weight_profile.import"))
		{
			return TEXT("skin-weight-import-owner");
		}
		if (CommandId.StartsWith(TEXT("external."))
			|| CommandId.StartsWith(TEXT("asset.reimport_")))
		{
			return TEXT("factory-external-import-owner");
		}
		return TEXT("destructive-content-owner");
	}

	FString InvalidButWellTypedParams(
		const FUeAgentFrozenCommandContract& Contract)
	{
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		for (const FUeAgentCommandFieldDeclaration& Field
			: Contract.GetInputFields())
		{
			if (!Field.bRequired) continue;
			if (Field.WireType == TEXT("boolean"))
			{
				Params->SetBoolField(Field.Name, false);
			}
			else if (Field.WireType == TEXT("integer")
				|| Field.WireType == TEXT("number"))
			{
				Params->SetNumberField(Field.Name, 0.0);
			}
			else if (Field.WireType == TEXT("array"))
			{
				Params->SetArrayField(Field.Name, {});
			}
			else if (Field.WireType == TEXT("object")
				|| Field.WireType == TEXT("any"))
			{
				Params->SetObjectField(
					Field.Name, MakeShared<FJsonObject>());
			}
			else
			{
				Params->SetStringField(Field.Name, TEXT("invalid"));
			}
		}
		return Serialize(Params);
	}

	bool TryInvoke(
		const FScopedPublicCommandComposition& Harness,
		const FUeAgentFrozenCommandContract& Contract,
		TSharedPtr<FJsonObject>& OutResult,
		FString& OutError)
	{
		OutResult.Reset();
		OutError.Reset();
		const FString RequestId = TEXT("destructive-activation-")
			+ Contract.GetCommandId().Replace(TEXT("."), TEXT("-"))
			+ TEXT("-") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString CorrelationId = RequestId + TEXT("-correlation");
		const FString Body =
			TEXT("{\"command_binding_hash\":\"") + Contract.GetContractHash()
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
		return ReadPublicResponse(Response.Body, OutResult, OutError);
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
	FUeAgentDestructiveImportPublicActivationTest,
	"UeAgentInterface.Composition.DestructiveImport.PublicActivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDestructiveImportPublicActivationTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FString Error;
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
	const TSharedPtr<const UE::UAI::CommandRegistry::
		FUeAgentImmutableCommandRegistry>& Registry =
			Harness.GetCommandRegistry();
	if (!TestTrue(TEXT("current immutable command registry is available"),
			Registry.IsValid()))
	{
		return false;
	}

	TArray<FString> MissingOwners;
	for (const FString& CommandId : CanonicalIds)
	{
		const FUeAgentFrozenCommandContract* const Contract =
			Registry->FindExact(CommandId);
		if (!Contract)
		{
			MissingOwners.Add(CommandId + TEXT(":manifest"));
			continue;
		}
		if (Contract->GetPublicOwner() != ExpectedOwner(CommandId))
		{
			MissingOwners.Add(CommandId + TEXT(":owner=")
				+ Contract->GetPublicOwner());
			continue;
		}
		TSharedPtr<FJsonObject> Result;
		if (!TryInvoke(Harness, *Contract, Result, Error) || !Result.IsValid())
		{
			MissingOwners.Add(CommandId + TEXT(":") + Error);
			continue;
		}
		if (!Result->GetBoolField(TEXT("ok")) && IsMissingOwner(PublicErrorText(Result)))
		{
			MissingOwners.Add(CommandId + TEXT(":") + PublicErrorText(Result));
			continue;
		}
		if (Result->GetBoolField(TEXT("ok")))
		{
			const auto Payload = Result->GetObjectField(TEXT("payload"));
			FString TerminalStatus;
			if (ExpectedOwner(CommandId) != TEXT("destructive-content-owner")
				|| !Payload.IsValid()
				|| !Payload->TryGetStringField(
					TEXT("status"), TerminalStatus)
				|| TerminalStatus != TEXT("planned")
				|| Payload->HasField(TEXT("operation_id"))
				|| Payload->HasField(TEXT("plan_fingerprint"))
				|| Payload->HasField(TEXT("destructive_authorization_id"))
				|| Payload->HasField(TEXT("revision_snapshot"))
				|| !Payload->HasTypedField<EJson::Array>(TEXT("impact_scope")))
			{
				MissingOwners.Add(CommandId + TEXT(":planning_semantics"));
			}
			continue;
		}

		const auto ErrorObject = Result->GetObjectField(TEXT("error"));
		FString ErrorCode;
		if (!ErrorObject.IsValid()
			|| !ErrorObject->TryGetStringField(TEXT("code"), ErrorCode)
			|| ErrorCode.IsEmpty())
		{
			MissingOwners.Add(CommandId + TEXT(":failure_semantics"));
		}
	}
	if (!MissingOwners.IsEmpty())
	{
		AddError(FString::Printf(
			TEXT("%d destructive/import commands lack their current formal owner or typed pre-mutation failure semantics: %s"),
			MissingOwners.Num(), *FString::Join(MissingOwners, TEXT(" | "))));
	}
	TestEqual(TEXT("all commands enter their declared formal public owner"),
		MissingOwners.Num(), 0);

	TestTrue(TEXT("formal public composition stops"), Harness.Stop());
	TestEqual(TEXT("command route is released on stop"),
		Harness.GetRouteHost().Handlers.Num(), 0);
	TestEqual(TEXT("instance ownership is released on stop"),
		Harness.GetRouteHost().RouteByInstance.Num(), 0);
	return !HasAnyErrors();
}

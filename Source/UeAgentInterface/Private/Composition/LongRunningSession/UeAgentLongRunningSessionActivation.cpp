#include "Composition/LongRunningSession/UeAgentLongRunningSessionActivation.h"

#include "Composition/TimelineSpatial/UeAgentTimelineSpatialActivation.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::Composition::LongRunningSession
{
	using namespace CommandContract;
	using namespace Contract;

	namespace
	{
		struct FProfileSpec final
		{
			const TCHAR* CommandId;
			const TCHAR* Namespace;
			const TCHAR* Method;
			const TCHAR* DescriptorId;
			const TCHAR* HandlerBinding;
			const TCHAR* CommandKind;
		};

		const TArray<FProfileSpec>& ProfileSpecs()
		{
			static const TArray<FProfileSpec> Specs = {
				{TEXT("job.compile_build"), TEXT("job"), TEXT("compile_build"),
					TEXT("uai.long_running.job.compile_build/v1"),
					TEXT("long-running-session.job.compile-build.v1"), TEXT("start")},
				{TEXT("editor.session"), TEXT("editor"), TEXT("session"),
					TEXT("uai.long_running.editor.session/v1"),
					TEXT("long-running-session.editor.session.v1"), TEXT("start")},
				{TEXT("runtime.scenario.execute"), TEXT("runtime"),
					TEXT("scenario_execute"),
					TEXT("uai.long_running.runtime.scenario/v1"),
					TEXT("long-running-session.runtime.scenario.v1"), TEXT("start")},
				{TEXT("capture.image.execute"), TEXT("capture"), TEXT("image_execute"),
					TEXT("uai.long_running.capture.image/v1"),
					TEXT("long-running-session.capture.image.v1"), TEXT("start")},
				{TEXT("capture.media.execute"), TEXT("capture"), TEXT("media_execute"),
					TEXT("uai.long_running.capture.media/v1"),
					TEXT("long-running-session.capture.media.v1"), TEXT("start")},
				{TEXT("capture.render.execute"), TEXT("capture"), TEXT("render_execute"),
					TEXT("uai.long_running.capture.render/v1"),
					TEXT("long-running-session.capture.render.v1"), TEXT("start")},
				{TEXT("editor.safe_exit"), TEXT("editor"), TEXT("safe_exit"),
					TEXT("uai.long_running.editor.safe_exit/v1"),
					TEXT("long-running-session.editor.safe-exit.v1"), TEXT("start")},
				{TEXT("operation.status"), TEXT("operation"), TEXT("status"),
					TEXT("uai.long_running.operation.status/v1"),
					TEXT("long-running-session.operation.status.v1"), TEXT("lifecycle")},
				{TEXT("operation.resume"), TEXT("operation"), TEXT("resume"),
					TEXT("uai.long_running.operation.resume/v1"),
					TEXT("long-running-session.operation.resume.v1"), TEXT("lifecycle")},
				{TEXT("operation.cancel"), TEXT("operation"), TEXT("cancel"),
					TEXT("uai.long_running.operation.cancel/v1"),
					TEXT("long-running-session.operation.cancel.v1"), TEXT("lifecycle")},
				{TEXT("operation.receipt"), TEXT("operation"), TEXT("receipt"),
					TEXT("uai.long_running.operation.receipt/v1"),
					TEXT("long-running-session.operation.receipt.v1"), TEXT("lifecycle")},
				{TEXT("operation.reconcile"), TEXT("operation"), TEXT("reconcile"),
					TEXT("uai.long_running.operation.reconcile/v1"),
					TEXT("long-running-session.operation.reconcile.v1"), TEXT("lifecycle")},
				{TEXT("operation.release"), TEXT("operation"), TEXT("release"),
					TEXT("uai.long_running.operation.release/v1"),
					TEXT("long-running-session.operation.release.v1"), TEXT("lifecycle")},
			};
			return Specs;
		}

		FUeAgentExecutionContextPolicy LongRunningExecutionContextPolicy()
		{
			return FUeAgentExecutionContextPolicy(
				{
					EUeAgentStatePlane::AuthoringMemory,
					EUeAgentStatePlane::Persisted,
					EUeAgentStatePlane::RuntimeInstance,
					EUeAgentStatePlane::RenderedObservation,
				},
				{},
				EUeAgentContextResolutionPolicy::ExplicitOnly,
				EUeAgentTransientContextPolicy::TemporaryMutationAllowed);
		}

		FString OperationRefSchema()
		{
			return TEXT(
				"{\"additionalProperties\":false,\"properties\":{"
				"\"operation_id\":{\"minLength\":1,\"type\":\"string\"},"
				"\"owner_lease_id\":{\"minLength\":1,\"type\":\"string\"},"
				"\"server_session_id\":{\"minLength\":1,\"type\":\"string\"}},"
				"\"required\":[\"operation_id\",\"owner_lease_id\"," 
				"\"server_session_id\"],\"type\":\"object\"}");
		}

		FString StartSchema(const FProfileSpec& Spec)
		{
			if (FString(Spec.CommandId) == TEXT("job.compile_build"))
			{
				return TEXT(
					"{\"additionalProperties\":false,\"properties\":{"
					"\"family_id\":{\"enum\":[\"asset_compile\",\"world_build\"," 
					"\"external_process\",\"world_partition_resave\",\"packaging\"],\"type\":\"string\"},"
					"\"parameters\":{\"type\":\"object\"},"
					"\"policy_id\":{\"minLength\":1,\"type\":\"string\"},"
					"\"subject_ref\":{\"minLength\":1,\"type\":\"string\"}},"
					"\"required\":[\"family_id\",\"subject_ref\",\"policy_id\"," 
					"\"parameters\"],\"type\":\"object\"}");
			}
			if (FString(Spec.CommandId) == TEXT("editor.session"))
			{
				return TEXT(
					"{\"additionalProperties\":false,\"properties\":{"
					"\"family_id\":{\"enum\":[\"asset_editor\",\"runtime_preview\"," 
					"\"world_context\",\"modeling_tool\",\"voxel_grid\"],\"type\":\"string\"},"
					"\"parameters\":{\"type\":\"object\"},"
					"\"policy_id\":{\"minLength\":1,\"type\":\"string\"},"
					"\"subject_ref\":{\"minLength\":1,\"type\":\"string\"}},"
					"\"required\":[\"family_id\",\"subject_ref\",\"policy_id\"," 
					"\"parameters\"],\"type\":\"object\"}");
			}
			if (FString(Spec.CommandId) == TEXT("editor.safe_exit"))
			{
				return TEXT(
					"{\"additionalProperties\":false,\"properties\":{"
					"\"dirty_resolution\":{\"additionalProperties\":false,"
					"\"properties\":{\"discard_resources\":{\"items\":{\"type\":\"string\"},"
					"\"type\":\"array\"},\"mode\":{\"enum\":[\"reject\",\"save\",\"discard\"],"
					"\"type\":\"string\"},\"save_resources\":{\"items\":{\"type\":\"string\"},"
					"\"type\":\"array\"}},\"required\":[\"mode\"],\"type\":\"object\"},"
					"\"external_witness\":{\"additionalProperties\":false,"
					"\"properties\":{\"exit_attempt_id\":{\"maxLength\":32,\"minLength\":32,"
					"\"type\":\"string\"},\"launch_id\":{\"maxLength\":32,\"minLength\":32,"
					"\"type\":\"string\"},\"nonce\":{\"maxLength\":64,\"minLength\":64,"
					"\"type\":\"string\"},\"schema\":{\"const\":"
					"\"uai.safe_exit.external_witness_ref.v1\"}},"
					"\"required\":[\"schema\",\"launch_id\",\"exit_attempt_id\",\"nonce\"],"
					"\"type\":\"object\"}},\"required\":[\"dirty_resolution\","
					"\"external_witness\"],\"type\":\"object\"}");
			}
			if (FString(Spec.CommandId) == TEXT("runtime.scenario.execute"))
			{
				return TEXT(
					"{\"additionalProperties\":false,\"properties\":{"
					"\"parameters\":{\"additionalProperties\":false,\"properties\":{"
					"\"expected_pause_epoch\":{\"minimum\":1,\"type\":\"integer\"},"
					"\"expected_workspace_revision\":{\"minimum\":1,\"type\":\"integer\"},"
					"\"limits\":{\"additionalProperties\":false,\"properties\":{"
					"\"frame_count\":{\"maximum\":3600,\"minimum\":1,\"type\":\"integer\"},"
					"\"game_seconds\":{\"exclusiveMinimum\":0,\"maximum\":60,\"type\":\"number\"},"
					"\"wall_seconds\":{\"exclusiveMinimum\":0,\"maximum\":60,\"type\":\"number\"}},"
					"\"required\":[\"wall_seconds\",\"game_seconds\",\"frame_count\"],\"type\":\"object\"},"
					"\"observation_fields\":{\"items\":{\"additionalProperties\":false,"
					"\"properties\":{\"object_ref\":{\"minLength\":1,\"type\":\"string\"},"
					"\"property\":{\"enum\":[\"actor.transform\",\"actor.location\","
					"\"actor.rotation\",\"actor.scale\",\"actor.hidden\","
					"\"component.active\",\"component.visible\"],\"type\":\"string\"}},"
					"\"required\":[\"object_ref\",\"property\"],\"type\":\"object\"},"
					"\"maxItems\":64,\"type\":\"array\"},"
					"\"steps\":{\"items\":{\"oneOf\":["
					"{\"additionalProperties\":false,\"properties\":{"
					"\"at_seconds\":{\"minimum\":0,\"type\":\"number\"},"
					"\"type\":{\"const\":\"wait\",\"type\":\"string\"}},"
					"\"required\":[\"type\",\"at_seconds\"],\"type\":\"object\"},"
					"{\"additionalProperties\":false,\"properties\":{"
					"\"amount\":{\"type\":\"number\"},"
					"\"at_seconds\":{\"minimum\":0,\"type\":\"number\"},"
					"\"event\":{\"enum\":[\"pressed\",\"released\",\"repeat\",\"axis\"],\"type\":\"string\"},"
					"\"key\":{\"minLength\":1,\"type\":\"string\"},"
					"\"type\":{\"const\":\"input_key\",\"type\":\"string\"}},"
					"\"required\":[\"type\",\"at_seconds\",\"key\",\"event\"],\"type\":\"object\"},"
					"{\"additionalProperties\":false,\"properties\":{"
					"\"action_ref\":{\"minLength\":1,\"type\":\"string\"},"
					"\"at_seconds\":{\"minimum\":0,\"type\":\"number\"},"
					"\"type\":{\"const\":\"input_action\",\"type\":\"string\"},"
					"\"value\":{\"oneOf\":[{\"type\":\"boolean\"},{\"type\":\"number\"},"
					"{\"items\":{\"type\":\"number\"},\"maxItems\":2,\"minItems\":2,\"type\":\"array\"},"
					"{\"items\":{\"type\":\"number\"},\"maxItems\":3,\"minItems\":3,\"type\":\"array\"}]}},"
					"\"required\":[\"type\",\"at_seconds\",\"action_ref\",\"value\"],\"type\":\"object\"},"
					"{\"additionalProperties\":false,\"properties\":{"
					"\"actor_ref\":{\"minLength\":1,\"type\":\"string\"},"
					"\"at_seconds\":{\"minimum\":0,\"type\":\"number\"},"
					"\"transform\":{\"additionalProperties\":false,\"properties\":{"
					"\"dimension\":{\"const\":3,\"type\":\"integer\"},"
					"\"frame\":{\"const\":\"ue_world\",\"type\":\"string\"},"
					"\"location\":{\"items\":{\"type\":\"number\"},\"maxItems\":3,\"minItems\":3,\"type\":\"array\"},"
					"\"rotation\":{\"items\":{\"type\":\"number\"},\"maxItems\":3,\"minItems\":3,\"type\":\"array\"},"
					"\"rotation_format\":{\"const\":\"pitch_yaw_roll_degrees\",\"type\":\"string\"},"
					"\"scale\":{\"items\":{\"type\":\"number\"},\"maxItems\":3,\"minItems\":3,\"type\":\"array\"},"
					"\"unit\":{\"const\":\"centimeter\",\"type\":\"string\"}},"
					"\"required\":[\"frame\",\"dimension\",\"unit\",\"rotation_format\"],\"type\":\"object\"},"
					"\"type\":{\"const\":\"actor_transform\",\"type\":\"string\"}},"
					"\"required\":[\"type\",\"at_seconds\",\"actor_ref\",\"transform\"],\"type\":\"object\"},"
					"{\"additionalProperties\":false,\"properties\":{"
					"\"at_seconds\":{\"minimum\":0,\"type\":\"number\"},"
					"\"format\":{\"const\":\"png\",\"type\":\"string\"},"
					"\"source\":{\"const\":\"pie_output_surface\",\"type\":\"string\"},"
					"\"type\":{\"const\":\"capture\",\"type\":\"string\"}},"
					"\"required\":[\"type\",\"at_seconds\",\"source\",\"format\"],\"type\":\"object\"},"
					"{\"additionalProperties\":false,\"properties\":{"
					"\"at_seconds\":{\"minimum\":0,\"type\":\"number\"},"
					"\"format\":{\"const\":\"png\",\"type\":\"string\"},"
					"\"source\":{\"const\":\"pie_player_camera\",\"type\":\"string\"},"
					"\"type\":{\"const\":\"capture\",\"type\":\"string\"}},"
					"\"required\":[\"type\",\"at_seconds\",\"source\",\"format\"],\"type\":\"object\"},"
					"{\"additionalProperties\":false,\"oneOf\":["
					"{\"required\":[\"camera_ref\"]},{\"not\":{\"required\":[\"camera_ref\"]},"
					"\"properties\":{\"view_transform\":{\"required\":[\"frame\",\"dimension\",\"unit\","
					"\"rotation_format\",\"location\",\"rotation\"]}},"
					"\"required\":[\"view_transform\",\"fov_degrees\"]}],\"properties\":{"
					"\"at_seconds\":{\"minimum\":0,\"type\":\"number\"},"
					"\"camera_ref\":{\"minLength\":1,\"type\":\"string\"},"
					"\"format\":{\"const\":\"png\",\"type\":\"string\"},"
					"\"fov_degrees\":{\"maximum\":179,\"minimum\":1,\"type\":\"number\"},"
					"\"source\":{\"const\":\"scene_camera\",\"type\":\"string\"},"
					"\"type\":{\"const\":\"capture\",\"type\":\"string\"},"
					"\"view_transform\":{\"additionalProperties\":false,\"anyOf\":["
					"{\"required\":[\"location\"]},{\"required\":[\"rotation\"]}],\"properties\":{"
					"\"dimension\":{\"const\":3,\"type\":\"integer\"},"
					"\"frame\":{\"const\":\"ue_world\",\"type\":\"string\"},"
					"\"location\":{\"items\":{\"type\":\"number\"},\"maxItems\":3,\"minItems\":3,\"type\":\"array\"},"
					"\"rotation\":{\"items\":{\"type\":\"number\"},\"maxItems\":3,\"minItems\":3,\"type\":\"array\"},"
					"\"rotation_format\":{\"const\":\"pitch_yaw_roll_degrees\",\"type\":\"string\"},"
					"\"unit\":{\"const\":\"centimeter\",\"type\":\"string\"}},"
					"\"required\":[\"frame\",\"dimension\",\"unit\",\"rotation_format\"],\"type\":\"object\"}},"
					"\"required\":[\"type\",\"at_seconds\",\"source\",\"format\"],\"type\":\"object\"}]},"
					"\"maxItems\":128,\"type\":\"array\"}},"
					"\"required\":[\"expected_pause_epoch\",\"expected_workspace_revision\",\"limits\",\"steps\"],\"type\":\"object\"},"
					"\"policy_id\":{\"const\":\"policy:runtime-scenario:bounded/v1\"},"
					"\"subject_ref\":{\"minLength\":1,\"type\":\"string\"}},"
					"\"required\":[\"subject_ref\",\"policy_id\",\"parameters\"],\"type\":\"object\"}");
			}
			return TEXT(
				"{\"additionalProperties\":false,\"properties\":{"
				"\"parameters\":{\"type\":\"object\"},"
				"\"policy_id\":{\"minLength\":1,\"type\":\"string\"},"
				"\"subject_ref\":{\"minLength\":1,\"type\":\"string\"}},"
				"\"required\":[\"subject_ref\",\"policy_id\",\"parameters\"],"
				"\"type\":\"object\"}");
		}

		FString LifecycleSchema(const FProfileSpec& Spec)
		{
			const FString CommandId(Spec.CommandId);
			FString Properties = TEXT("\"operation_ref\":") + OperationRefSchema();
			FString Required = TEXT("\"operation_ref\"");
			if (CommandId == TEXT("operation.resume")
				|| CommandId == TEXT("operation.reconcile"))
			{
				Properties = TEXT(
					"\"expected_generation\":{\"minimum\":0,\"type\":\"integer\"},"
					"\"operation_ref\":") + OperationRefSchema();
				Required = TEXT("\"operation_ref\",\"expected_generation\"");
			}
			else if (CommandId == TEXT("operation.cancel"))
			{
				Properties += TEXT(
					",\"reason\":{\"minLength\":1,\"type\":\"string\"}");
				Required += TEXT(",\"reason\"");
			}
			return TEXT("{\"additionalProperties\":false,\"properties\":{")
				+ Properties + TEXT("},\"required\":[") + Required
				+ TEXT("],\"type\":\"object\"}");
		}

		FString SnapshotResultSchema()
		{
			return TEXT(
				"{\"properties\":{\"binding_hash\":{\"type\":\"string\"},"
				"\"descriptor_id\":{\"type\":\"string\"},"
				"\"generation\":{\"type\":\"integer\"},"
				"\"latest_signal\":{\"additionalProperties\":false,"
				"\"properties\":{\"payload\":{},\"revision\":{\"type\":\"integer\"},"
				"\"type_id\":{\"minLength\":1,\"type\":\"string\"}},"
				"\"required\":[\"type_id\",\"revision\",\"payload\"],\"type\":\"object\"},"
				"\"lifecycle_status\":{\"type\":\"string\"},"
				"\"operation_ref\":{\"type\":\"object\"},"
				"\"phase\":{\"type\":\"string\"},"
				"\"retry_safe\":{\"type\":\"boolean\"},"
				"\"schema\":{\"const\":\"uai.long_running.snapshot.v1\"},"
				"\"stop_requested\":{\"type\":\"boolean\"},"
				"\"terminal\":{\"type\":\"boolean\"}},"
				"\"required\":[\"schema\",\"operation_ref\",\"descriptor_id\"," 
				"\"binding_hash\",\"phase\",\"generation\",\"lifecycle_status\"," 
				"\"stop_requested\",\"terminal\",\"retry_safe\"],\"type\":\"object\"}");
		}

		FString ReceiptResultSchema()
		{
			return TEXT(
				"{\"properties\":{\"error_code\":{\"type\":\"string\"},"
				"\"generation\":{\"type\":\"integer\"},"
				"\"lifecycle_status\":{\"type\":\"string\"},"
				"\"operation_ref\":{\"type\":\"object\"},"
				"\"retry_safe\":{\"type\":\"boolean\"},"
				"\"schema\":{\"const\":\"uai.long_running.receipt.v1\"}},"
				"\"required\":[\"schema\",\"operation_ref\",\"generation\"," 
				"\"lifecycle_status\",\"error_code\",\"retry_safe\"],"
				"\"type\":\"object\"}");
		}

		FString InputSchema(const FProfileSpec& Spec)
		{
			return FString(Spec.CommandKind) == TEXT("start")
				? StartSchema(Spec)
				: LifecycleSchema(Spec);
		}

		FString ResultSchema(const FProfileSpec& Spec)
		{
			return FString(Spec.CommandId) == TEXT("operation.receipt")
				|| FString(Spec.CommandId) == TEXT("operation.release")
				? ReceiptResultSchema()
				: SnapshotResultSchema();
		}

		bool TryDescriptorHash(
			const FProfileSpec& Spec,
			const FString& Input,
			const FString& Result,
			FString& OutHash,
			FString& OutError)
		{
			TSharedPtr<FJsonObject> InputObject;
			TSharedPtr<FJsonObject> ResultObject;
			if (!Json::TryParseObject(Input, InputObject, OutError)
				|| !Json::TryParseObject(Result, ResultObject, OutError))
			{
				return false;
			}
			const TSharedRef<FJsonObject> Descriptor = MakeShared<FJsonObject>();
			Descriptor->SetStringField(TEXT("capability_id"), Spec.CommandId);
			Descriptor->SetStringField(TEXT("descriptor_id"), Spec.DescriptorId);
			Descriptor->SetStringField(TEXT("domain_id"), TEXT("long_running_session"));
			const FUeAgentExecutionContextPolicy Policy =
				LongRunningExecutionContextPolicy();
			const TSharedRef<FJsonObject> ContextPolicy = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> StatePlanes;
			for (const EUeAgentStatePlane StatePlane : Policy.GetAllowedStatePlanes())
			{
				StatePlanes.Add(MakeShared<FJsonValueString>(LexToString(StatePlane)));
			}
			ContextPolicy->SetArrayField(TEXT("allowed_state_planes"), MoveTemp(StatePlanes));
			TArray<TSharedPtr<FJsonValue>> RequiredFacets;
			for (const EUeAgentExecutionContextFacet Facet : Policy.GetRequiredFacets())
			{
				RequiredFacets.Add(MakeShared<FJsonValueString>(LexToString(Facet)));
			}
			ContextPolicy->SetArrayField(TEXT("required_facets"), MoveTemp(RequiredFacets));
			ContextPolicy->SetStringField(
				TEXT("resolution_policy"),
				LexToString(Policy.GetResolutionPolicy()));
			ContextPolicy->SetStringField(
				TEXT("transient_access_policy"),
				LexToString(Policy.GetTransientContextPolicy()));
			Descriptor->SetObjectField(TEXT("execution_context_policy"), ContextPolicy);
			Descriptor->SetObjectField(TEXT("input_schema"), InputObject);
			Descriptor->SetStringField(TEXT("operation_kind"), TEXT("operation"));
			Descriptor->SetObjectField(TEXT("result_schema"), ResultObject);
			Descriptor->SetField(
				TEXT("version"), MakeShared<FJsonValueNumberString>(TEXT("1")));
			FString Canonical;
			if (!Json::TryCanonicalize(Descriptor, Canonical, OutError))
			{
				return false;
			}
			OutHash = Json::Sha256Utf8(Canonical);
			return true;
		}

		TArray<FUeAgentCommandParameter> PublicParameters(
			const FString& CommandId)
		{
			TArray<FUeAgentCommandParameter> Parameters;
			auto Add = [&Parameters](
				const TCHAR* Name,
				const EUeAgentCommandWireType Type)
			{
				Parameters.Emplace(Name, Type, true, false);
			};
			if (CommandId == TEXT("job.compile_build")
				|| CommandId == TEXT("editor.session"))
			{
				Add(TEXT("family_id"), EUeAgentCommandWireType::String);
				Add(TEXT("subject_ref"), EUeAgentCommandWireType::String);
				Add(TEXT("policy_id"), EUeAgentCommandWireType::String);
				Add(TEXT("parameters"), EUeAgentCommandWireType::Object);
			}
			else if (CommandId.StartsWith(TEXT("capture."))
				|| CommandId == TEXT("runtime.scenario.execute"))
			{
				Add(TEXT("subject_ref"), EUeAgentCommandWireType::String);
				Add(TEXT("policy_id"), EUeAgentCommandWireType::String);
				Add(TEXT("parameters"), EUeAgentCommandWireType::Object);
			}
			else if (CommandId == TEXT("editor.safe_exit"))
			{
				Add(TEXT("dirty_resolution"), EUeAgentCommandWireType::Object);
				Add(TEXT("external_witness"), EUeAgentCommandWireType::Object);
			}
			else
			{
				Add(TEXT("operation_ref"), EUeAgentCommandWireType::Object);
				if (CommandId == TEXT("operation.resume")
					|| CommandId == TEXT("operation.reconcile"))
				{
					Add(TEXT("expected_generation"),
						EUeAgentCommandWireType::Integer);
				}
				else if (CommandId == TEXT("operation.cancel"))
				{
					Add(TEXT("reason"), EUeAgentCommandWireType::String);
				}
			}
			return Parameters;
		}
	}

	bool FUeAgentLongRunningSessionActivationProfile::IsValid() const
	{
		return !CommandId.IsEmpty()
			&& !Namespace.IsEmpty()
			&& !Method.IsEmpty()
			&& DescriptorIdentity.IsValid()
			&& !HandlerBinding.IsEmpty()
			&& (CommandKind == TEXT("start")
				|| CommandKind == TEXT("lifecycle"));
	}

	bool FUeAgentLongRunningSessionActivationProfile::IsStartCommand() const
	{
		return CommandKind == TEXT("start");
	}

	bool FUeAgentLongRunningSessionActivationProfile::IsLifecycleCommand() const
	{
		return CommandKind == TEXT("lifecycle");
	}

	bool TryCreateLongRunningSessionDescriptorRegistry(
		TSharedPtr<const FUeAgentDescriptorRegistry>& OutRegistry,
		FString& OutError)
	{
		OutRegistry.Reset();
		OutError.Reset();
		TArray<FUeAgentOperationDescriptor> Descriptors;
		Descriptors.Reserve(ProfileSpecs().Num());
		for (const FProfileSpec& Spec : ProfileSpecs())
		{
			const FString Input = InputSchema(Spec);
			const FString Result = ResultSchema(Spec);
			FString Hash;
			if (!TryDescriptorHash(Spec, Input, Result, Hash, OutError))
			{
				return false;
			}
			Descriptors.Emplace(
				FUeAgentDescriptorIdentity(Spec.DescriptorId, 1, MoveTemp(Hash)),
				Spec.CommandId,
				TEXT("long_running_session"),
				EUeAgentDescriptorOperationKind::Operation,
				LongRunningExecutionContextPolicy(),
				FUeAgentContractValue(Input),
				FUeAgentContractValue(Result));
		}
		return FUeAgentDescriptorRegistry::TryCreate(
			MoveTemp(Descriptors), OutRegistry, OutError);
	}

	const TArray<FUeAgentLongRunningSessionActivationProfile>&
	GetLongRunningSessionActivationProfiles()
	{
		static const TArray<FUeAgentLongRunningSessionActivationProfile> Profiles = []()
		{
			TArray<FUeAgentLongRunningSessionActivationProfile> Result;
			TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
			FString Error;
			if (!TryCreateLongRunningSessionDescriptorRegistry(Registry, Error)
				|| !Registry.IsValid())
			{
				return Result;
			}
			for (const FProfileSpec& Spec : ProfileSpecs())
			{
				const FUeAgentOperationDescriptor* Descriptor =
					Registry->FindByDescriptorId(Spec.DescriptorId);
				if (!Descriptor)
				{
					Result.Reset();
					return Result;
				}
				FUeAgentLongRunningSessionActivationProfile Profile(
					Descriptor->GetIdentity());
				Profile.CommandId = Spec.CommandId;
				Profile.Namespace = Spec.Namespace;
				Profile.Method = Spec.Method;
				Profile.HandlerBinding = Spec.HandlerBinding;
				Profile.CommandKind = Spec.CommandKind;
				if (!Profile.IsValid())
				{
					Result.Reset();
					return Result;
				}
				Result.Add(MoveTemp(Profile));
			}
			return Result;
		}();
		return Profiles;
	}

	bool TryCreateLongRunningSessionProductionCommandContractRegistry(
		TSharedPtr<const FUeAgentCommandContractRegistry>& OutRegistry,
		FString& OutError)
	{
		OutRegistry.Reset();
		OutError.Reset();
		TSharedPtr<const FUeAgentCommandContractRegistry> Base;
		if (!TimelineSpatial::
				TryCreateTimelineSpatialProductionCommandContractRegistry(
					Base, OutError)
			|| !Base.IsValid())
		{
			return false;
		}
		const TArray<FUeAgentLongRunningSessionActivationProfile>& Profiles =
			GetLongRunningSessionActivationProfiles();
		if (Profiles.Num() != ProfileSpecs().Num())
		{
			OutError = TEXT("long_running_session_activation_profiles_invalid");
			return false;
		}
		TArray<FUeAgentCommandContract> Contracts = Base->GetContracts();
		for (const FUeAgentLongRunningSessionActivationProfile& Profile : Profiles)
		{
			Contracts.Emplace(
				Profile.CommandId,
				Profile.Namespace,
				Profile.Method,
				PublicParameters(Profile.CommandId),
				1,
				1,
				EUeAgentCommandStatus::Active);
		}
		return FUeAgentCommandContractRegistry::TryCreate(
			MoveTemp(Contracts), OutRegistry, OutError);
	}
}

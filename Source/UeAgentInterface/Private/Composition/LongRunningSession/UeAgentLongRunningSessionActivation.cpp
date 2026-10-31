#include "Composition/LongRunningSession/UeAgentLongRunningSessionActivation.h"

#include "Composition/TimelineSpatial/UeAgentTimelineSpatialActivation.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "DomainAdapters/EditorSessionOperation/UeAgentEditorSessionOperationContract.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentAssetFolderThumbnailCapture.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

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
				{TEXT("asset.folder.preview"), TEXT("asset"), TEXT("folder_preview"),
					TEXT("uai.long_running.asset.folder_preview/v1"),
					TEXT("long-running-session.asset.folder-preview.v1"), TEXT("start")},
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
				{TEXT("operation.discover"), TEXT("operation"), TEXT("discover"),
					TEXT("uai.long_running.operation.discover/v1"),
					TEXT("long-running-session.operation.discover.v1"), TEXT("lifecycle")},
				{TEXT("operation.recover"), TEXT("operation"), TEXT("recover"),
					TEXT("uai.long_running.operation.recover/v1"),
					TEXT("long-running-session.operation.recover.v1"), TEXT("lifecycle")},
				{TEXT("operation.takeover"), TEXT("operation"), TEXT("takeover"),
					TEXT("uai.long_running.operation.takeover/v1"),
					TEXT("long-running-session.operation.takeover.v1"), TEXT("lifecycle")},
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
			if (FString(Spec.CommandId) == TEXT("asset.folder.preview"))
			{
				return FString(TEXT("{\"type\":\"object\",\"additionalProperties\":false,\"required\":[\"folder\"],\"properties\":{\"folder\":{\"type\":\"string\",\"minLength\":1},\"options\":"))
					+ DomainAdapters::CaptureMediaLongJob::GetAssetFolderPreviewOptionsSchema() + TEXT("}}");
			}
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
				TArray<TSharedPtr<FJsonValue>> Families;
				for (const auto& Family : DomainAdapters::EditorSessionOperation::GetEditorSessionOperationFamilyContracts())
				{
					if (DomainAdapters::EditorSessionOperation::IsEditorSessionCommandFamily(Family.GetFamily()))
						Families.Add(MakeShared<FJsonValueString>(Family.GetFamilyId()));
				}
				FString FamilyJson;
				FJsonSerializer::Serialize(Families, TJsonWriterFactory<>::Create(&FamilyJson));
				return FString::Printf(TEXT(
					"{\"additionalProperties\":false,\"properties\":{"
					"\"family_id\":{\"enum\":%s,\"type\":\"string\"},"
					"\"parameters\":{\"type\":\"object\"},"
					"\"policy_id\":{\"minLength\":1,\"type\":\"string\"},"
					"\"subject_ref\":{\"minLength\":1,\"type\":\"string\"}},"
					"\"required\":[\"family_id\",\"subject_ref\",\"policy_id\"," 
					"\"parameters\"],\"type\":\"object\"}"), *FamilyJson);
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
				// The domain parser owns per-operation semantics; this schema exposes transport shapes.
				return TEXT(R"json({"additionalProperties":false,"properties":{"parameters":{"additionalProperties":false,"properties":{"expected_pause_epoch":{"minimum":1,"type":"integer"},"expected_workspace_revision":{"minimum":1,"type":"integer"},"limits":{"additionalProperties":false,"properties":{"frame_count":{"minimum":0,"type":"integer"},"game_seconds":{"minimum":0,"type":"number"},"wall_seconds":{"minimum":0,"type":"number"}},"type":"object"},"observation_fields":{"items":{"type":"object"},"type":"array"},"steps":{"items":{"additionalProperties":false,"properties":{"action_ref":{"type":"string"},"alias":{"type":"string"},"amount":{"type":"number"},"at_seconds":{"minimum":0,"type":"number"},"camera":{"type":"string"},"delta":{},"duration":{"minimum":0,"type":"number"},"duration_ms":{"minimum":0,"type":"number"},"event":{"type":"string"},"format":{"type":"string"},"fov_degrees":{"type":"number"},"interval":{"minimum":0,"type":"number"},"key":{"type":"string"},"name":{"type":"string"},"position":{"items":{"type":"number"},"maxItems":2,"minItems":2,"type":"array"},"property":{"type":"string"},"ref":{"type":"object"},"source":{"type":"string"},"text":{"type":"string"},"transform":{"type":"object"},"type":{"enum":["wait","input_key","input_pointer","input_scroll","input_text","input_action","actor_transform","property_write","property_get","property_set","capture","capture_start","capture_stop"],"type":"string"},"value":{},"view_transform":{"type":"object"}},"required":["type"],"type":"object"},"type":"array"},"unwatch_refs":{"items":{"type":"object"},"type":"array"},"watch_refs":{"items":{"type":"object"},"type":"array"}},"required":["expected_pause_epoch","expected_workspace_revision","steps"],"type":"object"},"policy_id":{"const":"policy:runtime-scenario:bounded/v2"},"subject_ref":{"minLength":1,"type":"string"}},"required":["subject_ref","policy_id","parameters"],"type":"object"})json");
			}
			return FString(TEXT(
				"{\"additionalProperties\":false,\"properties\":{"
				"\"parameters\":{\"type\":\"object\"},"
				"\"policy_id\":{\"minLength\":1,\"type\":\"string\"},"
				"\"subject_ref\":{\"minLength\":1,\"type\":\"string\"}},"))
				+ (FString(Spec.CommandId) == TEXT("capture.image.execute")
					? TEXT("\"required\":[\"subject_ref\",\"parameters\"],")
					: TEXT("\"required\":[\"subject_ref\",\"policy_id\",\"parameters\"],"))
				+ TEXT("\"type\":\"object\"}");
		}

		FString LifecycleSchema(const FProfileSpec& Spec)
		{
			const FString CommandId(Spec.CommandId);
			if (CommandId == TEXT("operation.discover"))
				return TEXT("{\"additionalProperties\":false,\"properties\":{},\"type\":\"object\"}");
			if (CommandId == TEXT("operation.recover"))
				return TEXT("{\"additionalProperties\":false,\"properties\":{\"request_id\":{\"minLength\":1,\"type\":\"string\"},\"command_id\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"request_id\",\"command_id\"],\"type\":\"object\"}");
			if (CommandId == TEXT("operation.takeover"))
				return TEXT("{\"additionalProperties\":false,\"properties\":{\"operation_id\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"operation_id\"],\"type\":\"object\"}");
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
				"\"cleanup_complete\":{\"type\":\"boolean\"},"
				"\"progress_diagnostic\":{\"type\":\"object\"},"
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
				"\"cleanup_complete\":{\"type\":\"boolean\"},"
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
			if (FString(Spec.CommandId) == TEXT("operation.discover"))
				return TEXT("{\"properties\":{\"schema\":{\"const\":\"uai.long_running.discovery.v1\"},\"operations\":{\"type\":\"array\",\"items\":{\"type\":\"object\"}}},\"required\":[\"schema\",\"operations\"],\"type\":\"object\"}");
			return FString(Spec.CommandId) == TEXT("operation.receipt")
				|| FString(Spec.CommandId) == TEXT("operation.release")
				? ReceiptResultSchema()
				: SnapshotResultSchema();
		}

		bool TryDescriptorHash(
			const FProfileSpec& Spec,
			FString& Input,
			FString& Result,
			FString& OutHash,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			TSharedPtr<FJsonObject> InputObject;
			TSharedPtr<FJsonObject> ResultObject;
			if (!Json::TryParseObject(Input, InputObject, OutError)
				|| !Json::TryParseObject(Result, ResultObject, OutError))
			{
				return false;
			}
			// Registry schema values and descriptor hashing must use the same canonical JSON.
			if (!Json::TryCanonicalize(InputObject, Input, OutError)
				|| !Json::TryCanonicalize(ResultObject, Result, OutError))
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
			if (CommandId == TEXT("asset.folder.preview"))
			{
				Add(TEXT("folder"), EUeAgentCommandWireType::String);
				Parameters.Emplace(TEXT("options"), EUeAgentCommandWireType::Object, false, false);
			}
			else if (CommandId == TEXT("job.compile_build")
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
				Parameters.Emplace(TEXT("policy_id"), EUeAgentCommandWireType::String,
					CommandId != TEXT("capture.image.execute"), false);
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
		FDiagnostic Diagnostic;
		return TryValidate(Diagnostic);
	}

	bool FUeAgentLongRunningSessionActivationProfile::TryValidate(FDiagnostic& OutError) const
	{
		OutError.Reset();
		auto MissingField = [&](const TCHAR* Field, const TCHAR* Reason)
		{
			OutError = FDiagnostic::Failure(TEXT("long_running_session_activation_profile_invalid"), Reason,
				TEXT("profile_activation"), FString(), CommandId);
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = Field;
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
			OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			return false;
		};
		if (CommandId.IsEmpty()) return MissingField(TEXT("command_id_present"), TEXT("The long-running command profile requires a command id."));
		if (Namespace.IsEmpty()) return MissingField(TEXT("namespace_present"), TEXT("The long-running command profile requires a command namespace."));
		if (Method.IsEmpty()) return MissingField(TEXT("method_present"), TEXT("The long-running command profile requires an execution method."));
		if (!DescriptorIdentity.IsValid())
		{
			OutError = FDiagnostic::WrapFailure(TEXT("long_running_session_activation_profile_invalid"),
				DescriptorIdentity.GetValidationDiagnostic(), TEXT("profile_activation"));
			OutError.SourceKind = TEXT("native_result");
			if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = CommandId;
			return false;
		}
		if (HandlerBinding.IsEmpty()) return MissingField(TEXT("handler_binding_present"), TEXT("The long-running command profile requires a handler binding."));
		if (CommandKind != TEXT("start") && CommandKind != TEXT("lifecycle"))
		{
			OutError = FDiagnostic::Failure(TEXT("long_running_session_activation_profile_invalid"),
				TEXT("The long-running command profile kind must be start or lifecycle."),
				TEXT("profile_activation"), FString(), CommandId);
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = TEXT("command_kind");
			OutError.ActualValue = MakeShared<FJsonValueString>(CommandKind);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("start or lifecycle"));
			return false;
		}
		return true;
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutRegistry.Reset();
		OutError.Reset();
		TArray<FUeAgentOperationDescriptor> Descriptors;
		Descriptors.Reserve(ProfileSpecs().Num());
		for (const FProfileSpec& Spec : ProfileSpecs())
		{
			FString Input = InputSchema(Spec);
			FString Result = ResultSchema(Spec);
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

	bool TryGetLongRunningSessionActivationProfiles(
		const TArray<FUeAgentLongRunningSessionActivationProfile>*& OutProfiles,
		FDiagnostic& OutError)
	{
		struct FProfileCache
		{
			TArray<FUeAgentLongRunningSessionActivationProfile> Profiles;
			FDiagnostic Diagnostic;
		};
		static const FProfileCache Cache = []()
		{
			FProfileCache Result;
			TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
			if (!TryCreateLongRunningSessionDescriptorRegistry(Registry, Result.Diagnostic)
				|| !Registry.IsValid())
			{
				if (Result.Diagnostic.IsEmpty())
				{
					Result.Diagnostic = FDiagnostic::Failure(TEXT("long_running_session_descriptor_registry_invalid"),
						TEXT("The command profiles require an available operation descriptor registry."), TEXT("profile_activation"));
					Result.Diagnostic.SourceKind = TEXT("native_result");
					Result.Diagnostic.PropertyPath = TEXT("descriptor_registry_ready");
					Result.Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
					Result.Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				}
				return Result;
			}
			for (const FProfileSpec& Spec : ProfileSpecs())
			{
				const FUeAgentOperationDescriptor* Descriptor =
					Registry->FindByDescriptorId(Spec.DescriptorId);
				if (!Descriptor)
				{
					Result.Diagnostic = FDiagnostic::Failure(TEXT("long_running_session_profile_descriptor_missing"),
						FString::Printf(TEXT("No operation descriptor is registered for command '%s'."), Spec.CommandId),
						TEXT("profile_activation"), Spec.DescriptorId);
					Result.Diagnostic.SourceKind = TEXT("native_result");
					Result.Diagnostic.PropertyPath = TEXT("descriptor_registered");
					Result.Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
					Result.Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					return Result;
				}
				FUeAgentLongRunningSessionActivationProfile Profile(
					Descriptor->GetIdentity());
				Profile.CommandId = Spec.CommandId;
				Profile.Namespace = Spec.Namespace;
				Profile.Method = Spec.Method;
				Profile.HandlerBinding = Spec.HandlerBinding;
				Profile.CommandKind = Spec.CommandKind;
				if (!Profile.TryValidate(Result.Diagnostic))
				{
					if (!Profile.CommandId.IsEmpty()) Result.Diagnostic.PublicMessage += FString::Printf(
						TEXT(" Command: '%s'."), *Profile.CommandId);
					return Result;
				}
				Result.Profiles.Add(MoveTemp(Profile));
			}
			return Result;
		}();
		OutProfiles = nullptr;
		OutError = Cache.Diagnostic;
		if (!OutError.IsEmpty()) return false;
		OutProfiles = &Cache.Profiles;
		return true;
	}

	bool TryCreateLongRunningSessionProductionCommandContractRegistry(
		TSharedPtr<const FUeAgentCommandContractRegistry>& OutRegistry,
		UE::UAI::Contract::FDiagnostic& OutError)
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
		const TArray<FUeAgentLongRunningSessionActivationProfile>* Profiles = nullptr;
		if (!TryGetLongRunningSessionActivationProfiles(Profiles, OutError)) return false;
		TArray<FUeAgentCommandContract> Contracts = Base->GetContracts();
		for (const FUeAgentLongRunningSessionActivationProfile& Profile : *Profiles)
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

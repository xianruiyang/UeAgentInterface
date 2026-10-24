#include "Composition/GraphCompound/UeAgentGraphCompoundAudioMediaRoutes.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundAudioMediaIdentity.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundLifecycleResultContract.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Core/Contract/UeAgentContractSchema.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Core/Contract/UeAgentExecutionContextPolicy.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "DomainAdapters/AudioMediaGraph/UeAgentAudioMediaGraphContract.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::Composition::GraphCompound
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::DomainAdapters::AudioMediaGraph;
	using namespace UE::UAI::PlanningPreflight;
	using namespace UE::UAI::StateSemantic;

	namespace
	{

		FDiagnostic RouteFailure(
			const TCHAR* Code, const FString& Reason, const FString& Subject,
			const FString& Field, TSharedPtr<FJsonValue> Actual,
			TSharedPtr<FJsonValue> Expected)
		{
			FDiagnostic Error = FDiagnostic::Failure(Code, Reason, TEXT("graph_compound"), FString(), Subject);
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Field;
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			return Error;
		}

		FDiagnostic InputFailure(
			const TCHAR* Code, const FString& Reason, const FString& Subject,
			const FString& Field, TSharedPtr<FJsonValue> Actual,
			TSharedPtr<FJsonValue> Expected)
		{
			FDiagnostic Error = RouteFailure(Code, Reason, Subject, Field, MoveTemp(Actual), MoveTemp(Expected));
			Error.SourceKind = TEXT("command_input");
			Error.Source.JsonPath = FString(TEXT("/")) + Field;
			return Error;
		}


		struct FRouteSpec final
		{
			const TCHAR* CapabilityId;
			const TCHAR* DescriptorId;
			const TCHAR* FamilyId;
			EGraphCompoundDomainRouteKind RouteKind;
			EAudioMediaGraphInputKind InputKind;
			const TCHAR* TargetKind;
			const TCHAR* NativeActionId;
			const TCHAR* ReadbackId;
			const TCHAR* SettleBarrier;
			bool bRequiresNativeBackendAvailability = false;
		};

		const TArray<FRouteSpec>& RouteSpecs()
		{
			using I = EAudioMediaGraphInputKind;
			using R = EGraphCompoundDomainRouteKind;
			static const TArray<FRouteSpec> Values = {
				{TEXT("audio_media.asset.create"), TEXT("uai.audio_media.asset.create.v1"), TEXT("asset_lifecycle"), R::TypedOperation, I::AssetCreate, TEXT("audio_media_asset"), TEXT("audio_media.asset.create"), TEXT("audio_media.asset.identity.readback"), TEXT("asset_create_refresh_settle")},
				{TEXT("metasound.graph.interface.mutate"), TEXT("uai.metasound.graph.interface.mutate.v1"), TEXT("metasound_document_graph_mutation"), R::TypedOperation, I::MetaSoundInterface, TEXT("metasound_document"), TEXT("audio_media.metasound.interface.apply"), TEXT("audio_media.metasound.frontend_document.readback"), TEXT("metasound_frontend_register_compile_settle")},
				{TEXT("metasound.graph.node.mutate"), TEXT("uai.metasound.graph.node.mutate.v1"), TEXT("metasound_document_graph_mutation"), R::TypedOperation, I::MetaSoundNode, TEXT("metasound_document"), TEXT("audio_media.metasound.node.apply"), TEXT("audio_media.metasound.frontend_document.readback"), TEXT("metasound_frontend_register_compile_settle")},
				{TEXT("metasound.graph.link.mutate"), TEXT("uai.metasound.graph.link.mutate.v1"), TEXT("metasound_document_graph_mutation"), R::TypedOperation, I::MetaSoundLink, TEXT("metasound_document"), TEXT("audio_media.metasound.link.apply"), TEXT("audio_media.metasound.frontend_document.readback"), TEXT("metasound_frontend_register_compile_settle")},
				{TEXT("metasound.graph.default.mutate"), TEXT("uai.metasound.graph.default.mutate.v1"), TEXT("metasound_document_graph_mutation"), R::TypedOperation, I::MetaSoundDefault, TEXT("metasound_document"), TEXT("audio_media.metasound.default.apply"), TEXT("audio_media.metasound.frontend_document.readback"), TEXT("metasound_frontend_register_compile_settle")},
				{TEXT("metasound.graph.compile_settle"), TEXT("uai.metasound.graph.compile_settle.v1"), TEXT("metasound_document_graph_mutation"), R::TypedOperation, I::MetaSoundCompileSettle, TEXT("metasound_document"), TEXT("audio_media.metasound.register_compile_settle"), TEXT("audio_media.metasound.frontend_document.readback"), TEXT("metasound_frontend_register_compile_settle")},
				{TEXT("sound_cue.graph.node.mutate"), TEXT("uai.sound_cue.graph.node.mutate.v1"), TEXT("sound_cue_node_graph_mutation"), R::TypedOperation, I::SoundCueNode, TEXT("sound_cue_graph"), TEXT("audio_media.sound_cue.node.apply"), TEXT("audio_media.sound_cue.graph.readback"), TEXT("sound_cue_graph_sync_compile_settle")},
				{TEXT("sound_cue.graph.relation.mutate"), TEXT("uai.sound_cue.graph.relation.mutate.v1"), TEXT("sound_cue_node_graph_mutation"), R::TypedOperation, I::SoundCueRelation, TEXT("sound_cue_graph"), TEXT("audio_media.sound_cue.relation.apply"), TEXT("audio_media.sound_cue.graph.readback"), TEXT("sound_cue_graph_sync_compile_settle")},
				{TEXT("sound_cue.graph.root.mutate"), TEXT("uai.sound_cue.graph.root.mutate.v1"), TEXT("sound_cue_node_graph_mutation"), R::TypedOperation, I::SoundCueRoot, TEXT("sound_cue_graph"), TEXT("audio_media.sound_cue.root.apply"), TEXT("audio_media.sound_cue.graph.readback"), TEXT("sound_cue_graph_sync_compile_settle")},
				{TEXT("sound_cue.graph.compile_settle"), TEXT("uai.sound_cue.graph.compile_settle.v1"), TEXT("sound_cue_node_graph_mutation"), R::TypedOperation, I::SoundCueCompileSettle, TEXT("sound_cue_graph"), TEXT("audio_media.sound_cue.sync_compile_settle"), TEXT("audio_media.sound_cue.graph.readback"), TEXT("sound_cue_graph_sync_compile_settle")},
				{TEXT("texture_graph.graph.node.mutate"), TEXT("uai.texture_graph.graph.node.mutate.v1"), TEXT("texture_graph_topology_mutation"), R::TypedOperation, I::TextureGraphNode, TEXT("texture_graph"), TEXT("audio_media.texture_graph.node.apply"), TEXT("audio_media.texture_graph.topology.readback"), TEXT("texture_graph_validate_evaluate_settle"), true},
				{TEXT("texture_graph.graph.link.mutate"), TEXT("uai.texture_graph.graph.link.mutate.v1"), TEXT("texture_graph_topology_mutation"), R::TypedOperation, I::TextureGraphLink, TEXT("texture_graph"), TEXT("audio_media.texture_graph.link.apply"), TEXT("audio_media.texture_graph.topology.readback"), TEXT("texture_graph_validate_evaluate_settle"), true},
				{TEXT("texture_graph.graph.parameter.mutate"), TEXT("uai.texture_graph.graph.parameter.mutate.v1"), TEXT("texture_graph_topology_mutation"), R::TypedOperation, I::TextureGraphParameter, TEXT("texture_graph"), TEXT("audio_media.texture_graph.parameter.apply"), TEXT("audio_media.texture_graph.topology.readback"), TEXT("texture_graph_validate_evaluate_settle"), true},
				{TEXT("texture_graph.graph.validate_evaluate_settle"), TEXT("uai.texture_graph.graph.validate_evaluate_settle.v1"), TEXT("texture_graph_topology_mutation"), R::TypedOperation, I::TextureGraphValidateEvaluateSettle, TEXT("texture_graph"), TEXT("audio_media.texture_graph.validate_evaluate_settle"), TEXT("audio_media.texture_graph.topology.readback"), TEXT("texture_graph_validate_evaluate_settle"), true},
				{TEXT("texture_graph.artifact.capture"), TEXT("uai.texture_graph.artifact.capture.v1"), TEXT("texture_graph_artifact_state"), R::Query, I::TextureGraphArtifactCapture, TEXT("texture_graph"), TEXT(""), TEXT("audio_media.texture_graph.artifact_state.readback"), TEXT(""), true},
			};
			return Values;
		}

		FString CanonicalSchema(UE::UAI::Contract::FDiagnostic& Error, const TCHAR* Text)
		{
			TSharedPtr<FJsonObject> Object;
			FString Canonical;
			if (!Json::TryParseObject(Text, Object, Error)
				|| !Json::TryCanonicalize(Object, Canonical, Error))
			{
				MarkGraphCompoundNativeDiagnostic(Error);
				return FString();
			}
			return Canonical;
		}

		FString InputSchema(const FRouteSpec& Spec, UE::UAI::Contract::FDiagnostic& Error)
		{
			using I = EAudioMediaGraphInputKind;
			switch (Spec.InputKind)
			{
			case I::AssetCreate:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"asset_kind\":{\"enum\":[\"metasound_source\",\"sound_cue\",\"texture_graph\"],\"type\":\"string\"},\"asset_name\":{\"minLength\":1,\"type\":\"string\"},\"package_path\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"asset_kind\",\"asset_name\",\"package_path\"],\"type\":\"object\"}"));
			case I::MetaSoundInterface:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"changes\":{\"items\":{\"additionalProperties\":false,\"properties\":{\"access_type\":{\"enum\":[\"value\",\"reference\"],\"type\":\"string\"},\"data_type_ref\":{\"minLength\":1,\"type\":\"string\"},\"default_value\":{},\"intent\":{\"enum\":[\"create\",\"update\",\"replace\",\"delete\"],\"type\":\"string\"},\"interface_kind\":{\"enum\":[\"input\",\"output\"],\"type\":\"string\"},\"name\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"access_type\",\"data_type_ref\",\"intent\",\"interface_kind\",\"name\"],\"type\":\"object\"},\"minItems\":1,\"type\":\"array\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"changes\",\"target_ref\"],\"type\":\"object\"}"));
			case I::MetaSoundNode:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"nodes\":{\"items\":{\"additionalProperties\":false,\"properties\":{\"class_name\":{\"minLength\":1,\"type\":\"string\"},\"intent\":{\"enum\":[\"create\",\"update\",\"replace\",\"delete\"],\"type\":\"string\"},\"major_version\":{\"minimum\":0,\"type\":\"integer\"},\"minor_version\":{\"minimum\":0,\"type\":\"integer\"},\"node_guid\":{\"minLength\":1,\"type\":\"string\"},\"position\":{\"additionalProperties\":false,\"properties\":{\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"}},\"required\":[\"x\",\"y\"],\"type\":\"object\"}},\"required\":[\"class_name\",\"intent\",\"major_version\",\"minor_version\",\"node_guid\"],\"type\":\"object\"},\"minItems\":1,\"type\":\"array\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"nodes\",\"target_ref\"],\"type\":\"object\"}"));
			case I::MetaSoundLink:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"links\":{\"items\":{\"additionalProperties\":false,\"properties\":{\"from\":{\"additionalProperties\":false,\"properties\":{\"data_type_ref\":{\"minLength\":1,\"type\":\"string\"},\"direction\":{\"enum\":[\"output\"],\"type\":\"string\"},\"node_guid\":{\"minLength\":1,\"type\":\"string\"},\"vertex_guid\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"data_type_ref\",\"direction\",\"node_guid\",\"vertex_guid\"],\"type\":\"object\"},\"intent\":{\"enum\":[\"create\",\"delete\"],\"type\":\"string\"},\"to\":{\"additionalProperties\":false,\"properties\":{\"data_type_ref\":{\"minLength\":1,\"type\":\"string\"},\"direction\":{\"enum\":[\"input\"],\"type\":\"string\"},\"node_guid\":{\"minLength\":1,\"type\":\"string\"},\"vertex_guid\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"data_type_ref\",\"direction\",\"node_guid\",\"vertex_guid\"],\"type\":\"object\"}},\"required\":[\"from\",\"intent\",\"to\"],\"type\":\"object\"},\"minItems\":1,\"type\":\"array\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"links\",\"target_ref\"],\"type\":\"object\"}"));
			case I::MetaSoundDefault:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"data_type_ref\":{\"minLength\":1,\"type\":\"string\"},\"intent\":{\"enum\":[\"update\",\"replace\"],\"type\":\"string\"},\"node_guid\":{\"minLength\":1,\"type\":\"string\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"},\"value\":{},\"vertex_guid\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"data_type_ref\",\"intent\",\"node_guid\",\"target_ref\",\"value\",\"vertex_guid\"],\"type\":\"object\"}"));
			case I::MetaSoundCompileSettle:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"registration_policy\":{\"enum\":[\"register_and_compile\"],\"type\":\"string\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"registration_policy\",\"target_ref\"],\"type\":\"object\"}"));
			case I::SoundCueNode:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"nodes\":{\"items\":{\"additionalProperties\":false,\"properties\":{\"intent\":{\"enum\":[\"create\",\"update\",\"replace\",\"delete\"],\"type\":\"string\"},\"native_class_ref\":{\"minLength\":1,\"type\":\"string\"},\"object_path\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"intent\",\"native_class_ref\",\"object_path\"],\"type\":\"object\"},\"minItems\":1,\"type\":\"array\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"nodes\",\"target_ref\"],\"type\":\"object\"}"));
			case I::SoundCueRelation:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"relations\":{\"items\":{\"additionalProperties\":false,\"properties\":{\"child_object_path\":{\"minLength\":1,\"type\":\"string\"},\"intent\":{\"enum\":[\"create\",\"update\",\"replace\",\"delete\"],\"type\":\"string\"},\"order_index\":{\"minimum\":0,\"type\":\"integer\"},\"parent_object_path\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"child_object_path\",\"intent\",\"order_index\",\"parent_object_path\"],\"type\":\"object\"},\"minItems\":1,\"type\":\"array\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"relations\",\"target_ref\"],\"type\":\"object\"}"));
			case I::SoundCueRoot:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"intent\":{\"enum\":[\"update\",\"replace\"],\"type\":\"string\"},\"root_node_object_path\":{\"minLength\":1,\"type\":\"string\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"intent\",\"root_node_object_path\",\"target_ref\"],\"type\":\"object\"}"));
			case I::SoundCueCompileSettle:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"sync_policy\":{\"enum\":[\"sync_and_compile\"],\"type\":\"string\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"sync_policy\",\"target_ref\"],\"type\":\"object\"}"));
			case I::TextureGraphNode:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"nodes\":{\"items\":{\"additionalProperties\":false,\"properties\":{\"element_id\":{\"minLength\":1,\"type\":\"string\"},\"expression_class_ref\":{\"minLength\":1,\"type\":\"string\"},\"intent\":{\"enum\":[\"create\",\"update\",\"replace\",\"delete\"],\"type\":\"string\"}},\"required\":[\"element_id\",\"expression_class_ref\",\"intent\"],\"type\":\"object\"},\"minItems\":1,\"type\":\"array\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"nodes\",\"target_ref\"],\"type\":\"object\"}"));
			case I::TextureGraphLink:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"links\":{\"items\":{\"additionalProperties\":false,\"properties\":{\"from\":{\"additionalProperties\":false,\"properties\":{\"direction\":{\"enum\":[\"output\"],\"type\":\"string\"},\"element_id\":{\"minLength\":1,\"type\":\"string\"},\"pin_name\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"direction\",\"element_id\",\"pin_name\"],\"type\":\"object\"},\"intent\":{\"enum\":[\"create\",\"delete\"],\"type\":\"string\"},\"to\":{\"additionalProperties\":false,\"properties\":{\"direction\":{\"enum\":[\"input\"],\"type\":\"string\"},\"element_id\":{\"minLength\":1,\"type\":\"string\"},\"pin_name\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"direction\",\"element_id\",\"pin_name\"],\"type\":\"object\"}},\"required\":[\"from\",\"intent\",\"to\"],\"type\":\"object\"},\"minItems\":1,\"type\":\"array\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"links\",\"target_ref\"],\"type\":\"object\"}"));
			case I::TextureGraphParameter:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"parameters\":{\"items\":{\"additionalProperties\":false,\"properties\":{\"alias\":{\"minLength\":1,\"type\":\"string\"},\"element_id\":{\"minLength\":1,\"type\":\"string\"},\"intent\":{\"enum\":[\"update\",\"replace\"],\"type\":\"string\"},\"pin_name\":{\"minLength\":1,\"type\":\"string\"},\"value\":{}},\"required\":[\"alias\",\"element_id\",\"intent\",\"pin_name\",\"value\"],\"type\":\"object\"},\"minItems\":1,\"type\":\"array\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"parameters\",\"target_ref\"],\"type\":\"object\"}"));
			case I::TextureGraphValidateEvaluateSettle:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"evaluation_policy\":{\"enum\":[\"validate_and_evaluate\"],\"type\":\"string\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"evaluation_policy\",\"target_ref\"],\"type\":\"object\"}"));
			case I::TextureGraphArtifactCapture:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"outputs\":{\"items\":{\"additionalProperties\":false,\"properties\":{\"element_id\":{\"minLength\":1,\"type\":\"string\"},\"output_name\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"element_id\",\"output_name\"],\"type\":\"object\"},\"minItems\":1,\"type\":\"array\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"outputs\",\"target_ref\"],\"type\":\"object\"}"));
			default:
				Error = RouteFailure(TEXT("graph_compound_input_schema_kind_invalid"),
					TEXT("The registered graph route has no input schema for its input kind."),
					Spec.CapabilityId, TEXT("input_kind"),
					MakeShared<FJsonValueNumber>(static_cast<int32>(Spec.InputKind)),
					MakeShared<FJsonValueString>(TEXT("a supported input schema kind")));
				return FString();
			}
		}

		FString ResultSchema(const FRouteSpec& Spec, UE::UAI::Contract::FDiagnostic& Error)
		{
			if (Spec.RouteKind == EGraphCompoundDomainRouteKind::Query)
			{
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"outputs\":{\"items\":{\"additionalProperties\":false,\"properties\":{\"evaluated_value_type\":{\"type\":\"string\"},\"expression_object_path\":{\"type\":\"string\"},\"node_index\":{\"type\":\"integer\"},\"output_name\":{\"type\":\"string\"},\"pin_index\":{\"type\":\"integer\"}},\"required\":[\"evaluated_value_type\",\"expression_object_path\",\"node_index\",\"output_name\",\"pin_index\"],\"type\":\"object\"},\"type\":\"array\"},\"published\":{\"enum\":[false]},\"target_ref\":{\"type\":\"string\"}},\"required\":[\"outputs\",\"published\",\"target_ref\"],\"type\":\"object\"}"));
			}
			const bool bRevisionIsMachineOnly =
				(FStringView(Spec.CapabilityId).StartsWith(TEXT("metasound.graph."))
					|| FStringView(Spec.CapabilityId).StartsWith(TEXT("sound_cue.graph.")));
			if (bRevisionIsMachineOnly)
			{
				return LifecycleResultContract::BuildMutationLifecycleResultSchema(false, Error);
			}
			return LifecycleResultContract::BuildMutationLifecycleResultSchema(false, Error);
		}

		TSharedPtr<FJsonObject> DescriptorObject(
			const FRouteSpec& Spec,
			const FString& Input,
			const FString& Result,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			TSharedPtr<FJsonObject> InputObject;
			TSharedPtr<FJsonObject> ResultObject;
			if (!Json::TryParseObject(Input, InputObject, OutError)
				|| !Json::TryParseObject(Result, ResultObject, OutError))
			{
				return nullptr;
			}
			const bool bQuery = Spec.RouteKind
				== EGraphCompoundDomainRouteKind::Query;
			const TSharedRef<FJsonObject> Policy = MakeShared<FJsonObject>();
			Policy->SetArrayField(TEXT("allowed_state_planes"), {
				MakeShared<FJsonValueString>(TEXT("persisted"))});
			Policy->SetArrayField(TEXT("required_facets"), {});
			Policy->SetStringField(TEXT("resolution_policy"), TEXT("explicit_only"));
			Policy->SetStringField(TEXT("transient_access_policy"),
				bQuery ? TEXT("read_only") : TEXT("temporary_mutation_allowed"));
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("capability_id"), Spec.CapabilityId);
			Object->SetStringField(TEXT("descriptor_id"), Spec.DescriptorId);
			Object->SetStringField(TEXT("domain_id"), TEXT("audio_media_graph"));
			Object->SetObjectField(TEXT("execution_context_policy"), Policy);
			Object->SetObjectField(TEXT("input_schema"), InputObject);
			Object->SetStringField(TEXT("operation_kind"),
				bQuery ? TEXT("query") : TEXT("operation"));
			if (bQuery)
			{
				const TSharedRef<FJsonObject> Query = MakeShared<FJsonObject>();
				Query->SetStringField(TEXT("materialization_policy"), TEXT("load_readonly"));
				Query->SetStringField(TEXT("provider_binding"),
					TEXT("graph_compound.texture_graph.artifact.capture"));
				Query->SetStringField(TEXT("query_kind"), TEXT("capability"));
				Query->SetStringField(TEXT("side_effect_policy"),
					TEXT("no_persistent_mutation"));
				Object->SetObjectField(TEXT("query"), Query);
			}
			Object->SetObjectField(TEXT("result_schema"), ResultObject);
			Object->SetField(TEXT("version"),
				MakeShared<FJsonValueNumberString>(TEXT("1")));
			return Object;
		}

		bool ValidateSpecs(UE::UAI::Contract::FDiagnostic& OutError)
		{
			const TArray<FAudioMediaGraphCapabilityContract>& Contracts =
				GetAudioMediaGraphCapabilityContracts();
			if (Contracts.Num() != RouteSpecs().Num())
			{
				OutError = RouteFailure(TEXT("audio_media_graph_contract_count_mismatch"),
					TEXT("The capability contract count differs from the registered audio/media route count."),
					TEXT("audio_media_graph"), TEXT("capability_contracts.count"),
					MakeShared<FJsonValueNumber>(Contracts.Num()), MakeShared<FJsonValueNumber>(RouteSpecs().Num()));
				OutError.Stage = TEXT("registration");
				return false;
			}
			for (const FRouteSpec& Spec : RouteSpecs())
			{
				const FAudioMediaGraphCapabilityContract* Match = Contracts.FindByPredicate(
					[&Spec](const FAudioMediaGraphCapabilityContract& Contract)
					{
						return Contract.GetCapabilityId() == Spec.CapabilityId;
					});
				const bool bQuery = Spec.RouteKind
					== EGraphCompoundDomainRouteKind::Query;
				if (!Match || Match->GetDescriptorId() != Spec.DescriptorId
					|| Match->GetPortFamilyId() != Spec.FamilyId
					|| Match->GetVersion() != 1
					|| (Match->GetOperationKind() == TEXT("query")) != bQuery)
				{
					const TCHAR* Field = !Match ? TEXT("capability_contract.present")
						: Match->GetDescriptorId() != Spec.DescriptorId ? TEXT("descriptor_id")
						: Match->GetPortFamilyId() != Spec.FamilyId ? TEXT("port_family_id")
						: Match->GetVersion() != 1 ? TEXT("version") : TEXT("operation_kind");
					TSharedPtr<FJsonValue> Actual;
					TSharedPtr<FJsonValue> Expected;
					if (!Match)
					{
						Actual = MakeShared<FJsonValueBoolean>(false);
						Expected = MakeShared<FJsonValueBoolean>(true);
					}
					else if (Match->GetDescriptorId() != Spec.DescriptorId)
					{
						Actual = MakeShared<FJsonValueString>(Match->GetDescriptorId());
						Expected = MakeShared<FJsonValueString>(Spec.DescriptorId);
					}
					else if (Match->GetPortFamilyId() != Spec.FamilyId)
					{
						Actual = MakeShared<FJsonValueString>(Match->GetPortFamilyId());
						Expected = MakeShared<FJsonValueString>(Spec.FamilyId);
					}
					else if (Match->GetVersion() != 1)
					{
						Actual = MakeShared<FJsonValueNumber>(Match->GetVersion());
						Expected = MakeShared<FJsonValueNumber>(1);
					}
					else
					{
						Actual = MakeShared<FJsonValueString>(Match->GetOperationKind());
						Expected = MakeShared<FJsonValueString>(bQuery ? TEXT("query") : TEXT("non-query"));
					}
					OutError = RouteFailure(TEXT("audio_media_graph_contract_mismatch"),
						!Match ? TEXT("No capability contract is registered for the audio/media graph route.")
							: TEXT("The capability contract does not match the audio/media graph route specification."),
						Spec.CapabilityId, Field, MoveTemp(Actual), MoveTemp(Expected));
					OutError.Stage = TEXT("registration");
					return false;
				}
			}
			return true;
		}

		FString StableSuffix(FString Value)
		{
			Value.ReplaceInline(TEXT("."), TEXT("_"));
			return Value;
		}

		struct FRouteBundle final
		{
			TArray<FAudioMediaGraphRouteProfile> Profiles;
			TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
			UE::UAI::Contract::FDiagnostic Error;
		};

		FRouteBundle BuildBundle()
		{
			FRouteBundle Bundle;
			if (!ValidateSpecs(Bundle.Error))
			{
				return Bundle;
			}
			TArray<FUeAgentOperationDescriptor> Descriptors;
			for (const FRouteSpec& Spec : RouteSpecs())
			{
				auto RejectSchema = [&](const TCHAR* Field)
				{
					MarkGraphCompoundNativeDiagnostic(Bundle.Error);
					if (Bundle.Error.TargetPath.IsEmpty()) Bundle.Error.TargetPath = Spec.DescriptorId;
					if (Bundle.Error.PropertyPath.IsEmpty()) Bundle.Error.PropertyPath = Field;
					Bundle.Profiles.Reset();
				};
				const FString Input = InputSchema(Spec, Bundle.Error);
				if (Input.IsEmpty())
				{
					RejectSchema(TEXT("input_schema"));
					return Bundle;
				}
				const FString Result = ResultSchema(Spec, Bundle.Error);
				if (Result.IsEmpty())
				{
					RejectSchema(TEXT("result_schema"));
					return Bundle;
				}
				TSharedPtr<FJsonObject> Object = DescriptorObject(Spec, Input, Result, Bundle.Error);
				FString CanonicalDescriptor;
				if (!Object.IsValid() || !Json::TryCanonicalize(Object, CanonicalDescriptor, Bundle.Error))
				{
					RejectSchema(TEXT("descriptor"));
					return Bundle;
				}
				const bool bQuery = Spec.RouteKind
					== EGraphCompoundDomainRouteKind::Query;
				const FUeAgentDescriptorIdentity Identity(
					Spec.DescriptorId, 1, Json::Sha256Utf8(CanonicalDescriptor));
				TOptional<FUeAgentQueryBinding> QueryBinding;
				if (bQuery)
				{
					QueryBinding.Emplace(
						TEXT("graph_compound.texture_graph.artifact.capture"),
						EUeAgentQueryKind::Capability,
						EUeAgentQueryMaterializationPolicy::LoadReadonly,
						EUeAgentQuerySideEffectPolicy::NoPersistentMutation);
				}
				const FUeAgentExecutionContextPolicy Policy(
					{EUeAgentStatePlane::Persisted}, {},
					EUeAgentContextResolutionPolicy::ExplicitOnly,
					bQuery ? EUeAgentTransientContextPolicy::ReadOnly
						: EUeAgentTransientContextPolicy::TemporaryMutationAllowed);
				Descriptors.Emplace(
					Identity, Spec.CapabilityId, TEXT("audio_media_graph"),
					bQuery ? EUeAgentDescriptorOperationKind::Query
						: EUeAgentDescriptorOperationKind::Operation,
					Policy, FUeAgentContractValue(Input),
					FUeAgentContractValue(Result), MoveTemp(QueryBinding));

				const FString Suffix = StableSuffix(Spec.CapabilityId);
				Bundle.Profiles.Emplace(
					Spec.CapabilityId, Spec.DescriptorId,
					TEXT("audio_media_graph"), Spec.FamilyId,
					Spec.RouteKind, Spec.InputKind, Spec.TargetKind,
					Spec.bRequiresNativeBackendAvailability,
					Spec.bRequiresNativeBackendAvailability
						? TEXT("audio_media_graph_texture_graph_unavailable")
						: TEXT(""),
					FGraphCompoundExecutionContract(
						Identity,
						bQuery ? EGraphCompoundExecutionLane::Query
							: EGraphCompoundExecutionLane::Operation,
						Spec.FamilyId,
						FString(TEXT("graph_compound.")) + Spec.CapabilityId,
						FString(TEXT("graph_compound.")) + Suffix + TEXT(".request.v1"),
						FString(TEXT("graph_compound.")) + Suffix + TEXT(".result.v1"),
						bQuery ? FString() : TEXT("graph_compound_identity_change"),
						Spec.NativeActionId,
						Spec.ReadbackId,
						Spec.SettleBarrier,
						bQuery
							? TEXT("independent_post_evaluation_artifact_state")
							: TEXT("independent_post_mutation_readback"),
						!bQuery, !bQuery));
			}
			if (!FUeAgentDescriptorRegistry::TryCreate(
					MoveTemp(Descriptors), Bundle.Registry, Bundle.Error))
			{
				MarkGraphCompoundNativeDiagnostic(Bundle.Error);
				Bundle.Profiles.Reset();
			}
			return Bundle;
		}

		const FRouteBundle& RouteBundle()
		{
			static const FRouteBundle Bundle = BuildBundle();
			return Bundle;
		}

		const FRouteSpec* FindSpec(const FAudioMediaGraphRouteProfile& Profile)
		{
			for (const FRouteSpec& Spec : RouteSpecs())
			{
				if (Profile.GetCapabilityId() == Spec.CapabilityId
					&& Profile.GetDescriptorId() == Spec.DescriptorId
					&& Profile.GetInputKind() == Spec.InputKind
					&& Profile.GetRouteKind() == Spec.RouteKind)
				{
					return &Spec;
				}
			}
			return nullptr;
		}

		void SetError(UE::UAI::Contract::FDiagnostic& OutError, UE::UAI::Contract::FDiagnostic Diagnostic, const FString& Subject = FString())
        { OutError = MoveTemp(Diagnostic); if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Subject; }


		bool TryValidatedObject(
			const FAudioMediaGraphRouteProfile& Profile,
			const FUeAgentContractValue& Input,
			FUeAgentResourceTracker& Resources,
			TSharedPtr<FJsonObject>& OutObject,
			FGraphCompoundRouteError& OutError)
		{
			OutObject.Reset();
			OutError = {};
			const FRouteBundle& Bundle = RouteBundle();
			if (!Profile.IsValid())
			{
				OutError = Profile.GetValidationDiagnostic();
				return false;
			}
			if (!Bundle.Registry.IsValid())
			{
				OutError = Bundle.Error.IsValid() ? Bundle.Error : RouteFailure(
					TEXT("graph_compound_registry_missing"),
					TEXT("The graph route registry is unavailable."),
					Profile.GetCapabilityId(), TEXT("registry.present"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return false;
			}
			if (!FindSpec(Profile))
			{
				const TSharedRef<FJsonObject> Actual = MakeShared<FJsonObject>();
				Actual->SetStringField(TEXT("capability_id"), Profile.GetCapabilityId());
				Actual->SetStringField(TEXT("descriptor_id"), Profile.GetDescriptorId());
				Actual->SetNumberField(TEXT("input_kind"), static_cast<int32>(Profile.GetInputKind()));
				Actual->SetNumberField(TEXT("route_kind"), static_cast<int32>(Profile.GetRouteKind()));
				OutError = RouteFailure(TEXT("graph_compound_route_spec_missing"),
					TEXT("No registered graph route specification matches the profile."),
					Profile.GetCapabilityId(), TEXT("route_spec"), MakeShared<FJsonValueObject>(Actual),
					MakeShared<FJsonValueString>(TEXT("registered route specification")));
				return false;
			}
			const FUeAgentOperationDescriptor* Descriptor =
				Bundle.Registry->FindByDescriptorId(Profile.GetDescriptorId());
			const FUeAgentContractSchema* Schema = Descriptor
				? Bundle.Registry->FindCompiledInputSchemaExact(
					Descriptor->GetIdentity()) : nullptr;
			if (!Descriptor || !Schema)
			{
				OutError = RouteFailure(TEXT("audio_media_graph_descriptor_missing"),
					!Descriptor ? TEXT("The graph route descriptor is missing from its registry.")
						: TEXT("The graph route descriptor has no compiled input schema."),
					Profile.GetDescriptorId(), !Descriptor ? TEXT("descriptor.present") : TEXT("input_schema.present"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return false;
			}
			const FUeAgentContractSchemaCheckpoint Checkpoint =
				[&Resources](
					const int64 WorkUnits,
					const FString& InstancePath,
					FUeAgentContractSchemaError& Error)
				{
					FUeAgentResourceChargeError ChargeError;
					if (!Resources.TryCharge(
							EUeAgentResourceDimension::WorkUnits,
							WorkUnits,
							TEXT("graph_compound.gc017.schema"),
							ChargeError))
					{
						Error = ChargeError;
				Error.Source.JsonPath = InstancePath;
						return false;
					}
					return true;
				};
			TOptional<FUeAgentContractValue> Canonical;
			FUeAgentContractSchemaError SchemaError;
			const bool bValidated = Schema->TryValidateAndCanonicalize(Input, Checkpoint, Canonical, SchemaError);
			if (!bValidated || !Canonical.IsSet())
			{
				OutError = SchemaError.IsValid() ? SchemaError : RouteFailure(
					TEXT("audio_media_graph_input_invalid"),
					bValidated ? TEXT("Graph input validation returned without a canonical value.")
						: TEXT("Graph input validation failed without a diagnostic."),
					Profile.GetCapabilityId(), bValidated ? TEXT("canonical_input.present") : TEXT("input_validation.succeeded"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return false;
			}
			UE::UAI::Contract::FDiagnostic ParseError;
			if (!Json::TryParseObject(
					Canonical->GetJsonValue(), OutObject, ParseError))
			{
				SetError(OutError, ParseError, Profile.GetCapabilityId());
				return false;
			}
			return true;
		}

		bool TryIntent(const FString& Text, EChangeIntent& OutIntent)
		{
			if (Text == TEXT("create")) { OutIntent = EChangeIntent::Create; return true; }
			if (Text == TEXT("update")) { OutIntent = EChangeIntent::Update; return true; }
			if (Text == TEXT("replace")) { OutIntent = EChangeIntent::Replace; return true; }
			if (Text == TEXT("delete")) { OutIntent = EChangeIntent::Delete; return true; }
			return false;
		}

		FUeAgentCanonicalIdentity Identity(
			const FString& SemanticId,
			const FString& TargetKind,
			const FString& OwnerId,
			const FString& Generation,
			TMap<FString, FString> Selectors = {})
		{
			return AudioMediaIdentity::MakeIdentity(
				SemanticId,
				TargetKind,
				OwnerId,
				TEXT("graph_compound"),
				Generation,
				MoveTemp(Selectors));
		}

		bool TryCanonical(
			const TSharedPtr<FJsonValue>& JsonValue,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FGraphCompoundRouteError& OutError,
			const FString& Subject)
		{
			UE::UAI::Contract::FDiagnostic Error;
			if (!JsonValue.IsValid()
				|| !CanonicalJson::TryFromJsonValue(
					JsonValue, Resources, OutValue, Error))
			{
				SetError(OutError, Error, Subject);
				return false;
			}
			return true;
		}

		TSharedPtr<FJsonValue> ObjectWithout(
			const FJsonObject& Object,
			const TSet<FString>& Excluded)
		{
			const TSharedRef<FJsonObject> Copy = MakeShared<FJsonObject>();
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object.Values)
			{
				if (!Excluded.Contains(Pair.Key))
				{
					Copy->SetField(Pair.Key, Pair.Value);
				}
			}
			return MakeShared<FJsonValueObject>(Copy);
		}

		FString AssetTargetRef(const FJsonObject& Object)
		{
			FString PackagePath = Object.GetStringField(TEXT("package_path"));
			while (PackagePath.EndsWith(TEXT("/")))
			{
				PackagePath.LeftChopInline(1);
			}
			return PackagePath + TEXT("/")
				+ Object.GetStringField(TEXT("asset_name"));
		}

		FString NativeAssetClass(const FString& AssetKind)
		{
			if (AssetKind == TEXT("metasound_source"))
			{
				return TEXT("/Script/MetasoundEngine.MetaSoundSource");
			}
			if (AssetKind == TEXT("sound_cue"))
			{
				return TEXT("/Script/Engine.SoundCue");
			}
			return TEXT("/Script/TextureGraph.TextureGraph");
		}

		FString TexturePinSemanticId(const FJsonObject& Pin)
		{
			return FString::Printf(
				TEXT("pin:texture:%s:%s:%s"),
				*Pin.GetStringField(TEXT("element_id")),
				*Pin.GetStringField(TEXT("direction")),
				*Pin.GetStringField(TEXT("pin_name")));
		}
	}

	FAudioMediaGraphRouteProfile::FAudioMediaGraphRouteProfile(
		FString InCapabilityId,
		FString InDescriptorId,
		FString InDomainId,
		FString InFamilyId,
		const EGraphCompoundDomainRouteKind InRouteKind,
		const EAudioMediaGraphInputKind InInputKind,
		FString InTargetKind,
		const bool bInRequiresNativeBackendAvailability,
		FString InUnavailableErrorCode,
		FGraphCompoundExecutionContract InExecutionContract)
		: CapabilityId(MoveTemp(InCapabilityId))
		, DescriptorId(MoveTemp(InDescriptorId))
		, DomainId(MoveTemp(InDomainId))
		, FamilyId(MoveTemp(InFamilyId))
		, RouteKind(InRouteKind)
		, InputKind(InInputKind)
		, TargetKind(MoveTemp(InTargetKind))
		, bRequiresNativeBackendAvailability(
			bInRequiresNativeBackendAvailability)
		, UnavailableErrorCode(MoveTemp(InUnavailableErrorCode))
		, ExecutionContract(MoveTemp(InExecutionContract))
	{
		for (FString* Value : {
			&CapabilityId, &DescriptorId, &DomainId, &FamilyId,
			&TargetKind, &UnavailableErrorCode})
		{
			Value->TrimStartAndEndInline();
		}
		const bool bQuery = RouteKind == EGraphCompoundDomainRouteKind::Query;
		if (CapabilityId.IsEmpty())
		{
			ValidationDiagnostic = RouteFailure(TEXT("graph_compound_binding_profile_invalid"),
				TEXT("The graph route capability_id is empty after trimming."),
				CapabilityId, TEXT("capability_id"), MakeShared<FJsonValueString>(CapabilityId),
				MakeShared<FJsonValueString>(TEXT("non-empty string")));
		}
		else if (DescriptorId.IsEmpty())
		{
			ValidationDiagnostic = RouteFailure(TEXT("graph_compound_binding_profile_invalid"),
				TEXT("The graph route descriptor_id is empty after trimming."),
				CapabilityId, TEXT("descriptor_id"), MakeShared<FJsonValueString>(DescriptorId),
				MakeShared<FJsonValueString>(TEXT("non-empty string")));
		}
		else if (DomainId.IsEmpty())
		{
			ValidationDiagnostic = RouteFailure(TEXT("graph_compound_binding_profile_invalid"),
				TEXT("The graph route domain_id is empty after trimming."),
				CapabilityId, TEXT("domain_id"), MakeShared<FJsonValueString>(DomainId),
				MakeShared<FJsonValueString>(TEXT("non-empty string")));
		}
		else if (FamilyId.IsEmpty())
		{
			ValidationDiagnostic = RouteFailure(TEXT("graph_compound_binding_profile_invalid"),
				TEXT("The graph route family_id is empty after trimming."),
				CapabilityId, TEXT("family_id"), MakeShared<FJsonValueString>(FamilyId),
				MakeShared<FJsonValueString>(TEXT("non-empty string")));
		}
		else if (TargetKind.IsEmpty())
		{
			ValidationDiagnostic = RouteFailure(TEXT("graph_compound_binding_profile_invalid"),
				TEXT("The graph route target_kind is empty after trimming."),
				CapabilityId, TEXT("target_kind"), MakeShared<FJsonValueString>(TargetKind),
				MakeShared<FJsonValueString>(TEXT("non-empty string")));
		}
		else if (!bQuery && RouteKind != EGraphCompoundDomainRouteKind::TypedOperation)
		{
			ValidationDiagnostic = RouteFailure(TEXT("graph_compound_binding_profile_invalid"),
				TEXT("The audio/media route kind is unsupported."),
				CapabilityId, TEXT("route_kind"), MakeShared<FJsonValueNumber>(static_cast<int32>(RouteKind)),
				MakeShared<FJsonValueString>(TEXT("Query or TypedOperation")));
		}
		else if (InputKind == EAudioMediaGraphInputKind::Invalid)
		{
			ValidationDiagnostic = RouteFailure(TEXT("graph_compound_binding_profile_invalid"),
				TEXT("The graph route has an invalid input kind."),
				CapabilityId, TEXT("input_kind"), MakeShared<FJsonValueNumber>(static_cast<int32>(InputKind)),
				MakeShared<FJsonValueString>(TEXT("non-Invalid input kind")));
		}
		else if (bRequiresNativeBackendAvailability && UnavailableErrorCode.IsEmpty())
		{
			ValidationDiagnostic = RouteFailure(TEXT("graph_compound_binding_profile_invalid"),
				TEXT("A route requiring native backend availability must define its unavailable error code."),
				CapabilityId, TEXT("unavailable_error_code"), MakeShared<FJsonValueString>(UnavailableErrorCode),
				MakeShared<FJsonValueString>(TEXT("non-empty error code")));
		}
		else if (!ExecutionContract.IsValid())
		{
			ValidationDiagnostic = FDiagnostic::WrapFailure(
				TEXT("graph_compound_binding_profile_invalid"),
				ExecutionContract.GetValidationDiagnostic(), TEXT("graph_compound"),
				TEXT("The graph route execution contract is invalid."));
		}
		else if (ExecutionContract.GetLane() != (bQuery ? EGraphCompoundExecutionLane::Query : EGraphCompoundExecutionLane::Operation))
		{
			ValidationDiagnostic = RouteFailure(TEXT("graph_compound_binding_profile_invalid"),
				TEXT("The execution lane does not match the audio/media route kind."),
				CapabilityId, TEXT("execution_contract.lane"),
				MakeShared<FJsonValueNumber>(static_cast<int32>(ExecutionContract.GetLane())),
				MakeShared<FJsonValueNumber>(static_cast<int32>(bQuery ? EGraphCompoundExecutionLane::Query : EGraphCompoundExecutionLane::Operation)));
		}
	}

	const FString& FAudioMediaGraphRouteProfile::GetCapabilityId() const { return CapabilityId; }
	const FString& FAudioMediaGraphRouteProfile::GetDescriptorId() const { return DescriptorId; }
	const FString& FAudioMediaGraphRouteProfile::GetDomainId() const { return DomainId; }
	const FString& FAudioMediaGraphRouteProfile::GetFamilyId() const { return FamilyId; }
	EGraphCompoundDomainRouteKind FAudioMediaGraphRouteProfile::GetRouteKind() const { return RouteKind; }
	EAudioMediaGraphInputKind FAudioMediaGraphRouteProfile::GetInputKind() const { return InputKind; }
	const FString& FAudioMediaGraphRouteProfile::GetTargetKind() const { return TargetKind; }
	bool FAudioMediaGraphRouteProfile::RequiresNativeBackendAvailability() const { return bRequiresNativeBackendAvailability; }
	const FString& FAudioMediaGraphRouteProfile::GetUnavailableErrorCode() const { return UnavailableErrorCode; }
	const FGraphCompoundExecutionContract& FAudioMediaGraphRouteProfile::GetExecutionContract() const { return ExecutionContract; }
	bool FAudioMediaGraphRouteProfile::IsValid() const { return ValidationDiagnostic.IsEmpty(); }
	const FDiagnostic& FAudioMediaGraphRouteProfile::GetValidationDiagnostic() const { return ValidationDiagnostic; }

	bool TryGetAudioMediaGraphRouteProfiles(
		const TArray<FAudioMediaGraphRouteProfile>*& OutProfiles, FDiagnostic& OutError)
	{
		OutProfiles = nullptr;
		OutError = RouteBundle().Error;
		if (!OutError.IsEmpty()) return false;
		OutProfiles = &RouteBundle().Profiles;
		return true;
	}

	const FAudioMediaGraphRouteProfile* FindAudioMediaGraphRouteProfile(
		const FString& CapabilityId)
	{
		for (const FAudioMediaGraphRouteProfile& Profile : RouteBundle().Profiles)
		{
			if (Profile.GetCapabilityId() == CapabilityId)
			{
				return &Profile;
			}
		}
		return nullptr;
	}

	bool TryCreateAudioMediaGraphRouteDescriptorRegistry(
		TSharedPtr<const FUeAgentDescriptorRegistry>& OutRegistry,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutRegistry = RouteBundle().Registry;
		OutError = RouteBundle().Error;
		return OutRegistry.IsValid() && OutError.IsEmpty();
	}

	FAudioMediaGraphArtifactOutputSelector::FAudioMediaGraphArtifactOutputSelector()
		: FAudioMediaGraphArtifactOutputSelector(FString(), FString())
	{
	}

	FAudioMediaGraphArtifactOutputSelector::
	FAudioMediaGraphArtifactOutputSelector(
		FString InElementId,
		FString InOutputName)
		: ElementId(MoveTemp(InElementId))
		, OutputName(MoveTemp(InOutputName))
	{
		for (FString* Value : {&ElementId, &OutputName})
		{
			Value->TrimStartAndEndInline();
		}
		if (ElementId.IsEmpty() || OutputName.IsEmpty())
		{
			ValidationDiagnostic = InputFailure(TEXT("audio_media_graph_artifact_selector_invalid"),
				ElementId.IsEmpty() ? TEXT("The artifact output element_id is empty after trimming.")
					: TEXT("The artifact output_name is empty after trimming."),
				ElementId, ElementId.IsEmpty() ? TEXT("element_id") : TEXT("output_name"),
				MakeShared<FJsonValueString>(ElementId.IsEmpty() ? ElementId : OutputName),
				MakeShared<FJsonValueString>(TEXT("non-empty string")));
		}
	}

	const FString& FAudioMediaGraphArtifactOutputSelector::GetElementId() const { return ElementId; }
	const FString& FAudioMediaGraphArtifactOutputSelector::GetOutputName() const { return OutputName; }
	bool FAudioMediaGraphArtifactOutputSelector::IsValid() const { return ValidationDiagnostic.IsEmpty(); }
	const FDiagnostic& FAudioMediaGraphArtifactOutputSelector::GetValidationDiagnostic() const { return ValidationDiagnostic; }

	FAudioMediaGraphArtifactQueryRequest::FAudioMediaGraphArtifactQueryRequest()
		: FAudioMediaGraphArtifactQueryRequest(FString(), {})
	{
	}

	FAudioMediaGraphArtifactQueryRequest::FAudioMediaGraphArtifactQueryRequest(
		FString InTargetRef,
		TArray<FAudioMediaGraphArtifactOutputSelector> InOutputs)
		: TargetRef(MoveTemp(InTargetRef))
		, Outputs(MoveTemp(InOutputs))
	{
		TargetRef.TrimStartAndEndInline();
		if (TargetRef.IsEmpty())
		{
			ValidationDiagnostic = InputFailure(TEXT("audio_media_graph_artifact_query_invalid"),
				TEXT("The artifact query target_ref is empty after trimming."),
				TargetRef, TEXT("target_ref"), MakeShared<FJsonValueString>(TargetRef),
				MakeShared<FJsonValueString>(TEXT("non-empty reference")));
		}
		else if (Outputs.IsEmpty())
		{
			ValidationDiagnostic = InputFailure(TEXT("audio_media_graph_artifact_query_invalid"),
				TEXT("The artifact query requires at least one output selector."),
				TargetRef, TEXT("outputs"), MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{}),
				MakeShared<FJsonValueString>(TEXT("non-empty array")));
		}
		for (int32 Index = 0; Index < Outputs.Num(); ++Index)
		{
			if (ValidationDiagnostic.IsEmpty() && !Outputs[Index].IsValid())
			{
				ValidationDiagnostic = Outputs[Index].GetValidationDiagnostic();
				ValidationDiagnostic.Source.JsonPath = FString::Printf(TEXT("/outputs/%d"), Index)
					+ ValidationDiagnostic.Source.JsonPath;
				ValidationDiagnostic.OperationIndex = Index;
			}
		}
	}

	const FString& FAudioMediaGraphArtifactQueryRequest::GetTargetRef() const { return TargetRef; }
	const TArray<FAudioMediaGraphArtifactOutputSelector>& FAudioMediaGraphArtifactQueryRequest::GetOutputs() const { return Outputs; }
	bool FAudioMediaGraphArtifactQueryRequest::IsValid() const { return ValidationDiagnostic.IsEmpty(); }
	const FDiagnostic& FAudioMediaGraphArtifactQueryRequest::GetValidationDiagnostic() const { return ValidationDiagnostic; }

	bool TryDecodeAudioMediaGraphArtifactQueryRequest(
		const FAudioMediaGraphRouteProfile& Profile,
		const FUeAgentContractValue& Input,
		FUeAgentResourceTracker& Resources,
		FAudioMediaGraphArtifactQueryRequest& OutRequest,
		FGraphCompoundRouteError& OutError)
	{
		OutRequest = FAudioMediaGraphArtifactQueryRequest();
		OutError = {};
		if (Profile.GetRouteKind() != EGraphCompoundDomainRouteKind::Query
			|| Profile.GetInputKind()
				!= EAudioMediaGraphInputKind::TextureGraphArtifactCapture)
		{
			const bool bWrongRoute = Profile.GetRouteKind() != EGraphCompoundDomainRouteKind::Query;
			OutError = RouteFailure(TEXT("audio_media_graph_route_kind_mismatch"),
				bWrongRoute ? TEXT("Artifact query decoding requires a query route.")
					: TEXT("Artifact query decoding requires the TextureGraphArtifactCapture input kind."),
				Profile.GetCapabilityId(), bWrongRoute ? TEXT("route_kind") : TEXT("input_kind"),
				MakeShared<FJsonValueNumber>(bWrongRoute ? static_cast<int32>(Profile.GetRouteKind())
					: static_cast<int32>(Profile.GetInputKind())),
				MakeShared<FJsonValueNumber>(bWrongRoute ? static_cast<int32>(EGraphCompoundDomainRouteKind::Query)
					: static_cast<int32>(EAudioMediaGraphInputKind::TextureGraphArtifactCapture)));
			return false;
		}
		TSharedPtr<FJsonObject> Object;
		if (!TryValidatedObject(Profile, Input, Resources, Object, OutError))
		{
			return false;
		}
		TArray<FAudioMediaGraphArtifactOutputSelector> Outputs;
		const TArray<TSharedPtr<FJsonValue>>& OutputValues = Object->GetArrayField(TEXT("outputs"));
		for (int32 Index = 0; Index < OutputValues.Num(); ++Index)
		{
			const TSharedPtr<FJsonValue>& Value = OutputValues[Index];
			const TSharedPtr<FJsonObject> Output = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Output.IsValid())
			{
				OutError = InputFailure(TEXT("audio_media_graph_artifact_selector_invalid"),
					TEXT("Each artifact output selector must be an object."),
					Profile.GetCapabilityId(), FString::Printf(TEXT("outputs/%d"), Index),
					Value, MakeShared<FJsonValueString>(TEXT("object")));
				OutError.ActualValuePresent = Value.IsValid();
				OutError.OperationIndex = Index;
				return false;
			}
			Outputs.Emplace(
				Output->GetStringField(TEXT("element_id")),
				Output->GetStringField(TEXT("output_name")));
		}
		OutRequest = FAudioMediaGraphArtifactQueryRequest(
			Object->GetStringField(TEXT("target_ref")),
			MoveTemp(Outputs));
		if (!OutRequest.IsValid())
		{
			OutError = OutRequest.GetValidationDiagnostic();
			return false;
		}
		return true;
	}

	bool TryDecodeAudioMediaGraphTypedOperation(
		const FAudioMediaGraphRouteProfile& Profile,
		const FUeAgentContractValue& Input,
		const FUeAgentCanonicalSnapshotRef& Snapshot,
		FUeAgentResourceTracker& Resources,
		FGraphCompoundCanonicalPayload& OutPayload,
		FGraphCompoundRouteError& OutError)
	{
		OutPayload = FGraphCompoundCanonicalPayload(
			GetGraphCompoundCanonicalSchemaId(), {});
		OutError = {};
		if (Profile.GetRouteKind()
			!= EGraphCompoundDomainRouteKind::TypedOperation)
		{
			OutError = RouteFailure(TEXT("audio_media_graph_route_kind_mismatch"),
				TEXT("This route cannot decode a typed graph operation."), Profile.GetCapabilityId(), TEXT("route_kind"),
				MakeShared<FJsonValueNumber>(static_cast<int32>(Profile.GetRouteKind())),
				MakeShared<FJsonValueNumber>(static_cast<int32>(EGraphCompoundDomainRouteKind::TypedOperation)));
			return false;
		}
		TSharedPtr<FJsonObject> Object;
		if (!TryValidatedObject(Profile, Input, Resources, Object, OutError))
		{
			return false;
		}
		const FString Generation = Snapshot->GetRevision().GetGeneration();
		const bool bAssetCreate = Profile.GetInputKind()
			== EAudioMediaGraphInputKind::AssetCreate;
		const FString TargetRef = bAssetCreate
			? AssetTargetRef(*Object)
			: Object->GetStringField(TEXT("target_ref"));
		const FString IdentityTargetKind = bAssetCreate
			? Object->GetStringField(TEXT("asset_kind"))
			: Profile.GetTargetKind();
		const TArray<FString> Authority = {TEXT("audio_media_graph.write")};
		const FString Evidence = TEXT("request:") + Profile.GetCapabilityId();
		TArray<FGraphCompoundCanonicalEdit> Edits;
		TMap<FString, FString> InputPointers;
		FString InputPointer;

		auto FailInput = [&](const TCHAR* Code, const TCHAR* Reason,
			const FString& Field, const FString& Pointer,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
			const int32 InputIndex = INDEX_NONE)
		{
			OutError = RouteFailure(Code, Reason, TargetRef, Field, MoveTemp(Actual), MoveTemp(Expected));
			OutError.SourceKind = TEXT("command_input");
			OutError.Source.JsonPath = Pointer;
			if (InputIndex != INDEX_NONE) OutError.OperationIndex = InputIndex;
			OutError.TargetPath = TargetRef;
			OutError.ActualValuePresent = OutError.ActualValue.IsValid();
			return false;
		};
		auto ReadIntent = [&](const TSharedPtr<FJsonObject>& Entry,
			const TSharedPtr<FJsonValue>& Value, const TCHAR* Code,
			const FString& Field, const int32 InputIndex, EChangeIntent& Intent)
		{
			if (!Entry.IsValid())
				return FailInput(Code, TEXT("An edit entry must be an object."),
					Field, InputPointer, Value, MakeShared<FJsonValueString>(TEXT("object")), InputIndex);
			const FString IntentText = Entry->GetStringField(TEXT("intent"));
			if (!TryIntent(IntentText, Intent))
				return FailInput(Code, TEXT("The edit intent is not supported."),
					TEXT("intent"), InputPointer + TEXT("/intent"),
					MakeShared<FJsonValueString>(IntentText),
					MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
						MakeShared<FJsonValueString>(TEXT("create")), MakeShared<FJsonValueString>(TEXT("update")),
						MakeShared<FJsonValueString>(TEXT("replace")), MakeShared<FJsonValueString>(TEXT("delete"))}), InputIndex);
			return true;
		};


		auto MakeIdentity = [&] (
			const FString& SemanticId,
			TMap<FString, FString> Selectors = {})
		{
			return Identity(
				SemanticId,
				IdentityTargetKind,
				TargetRef,
				Generation,
				MoveTemp(Selectors));
		};
		auto AddEdit = [&] (
			const FString& SemanticId,
			const EGraphCompoundElementKind ElementKind,
			const EChangeIntent Intent,
			const TSharedPtr<FJsonValue>& JsonValue,
			TArray<FUeAgentCanonicalIdentity> Dependencies = {},
			TMap<FString, FString> Selectors = {}, const bool bGeneratedValue = false)
		{
			FString ValuePointer = InputPointer;
			if (ValuePointer.IsEmpty())
				for (const auto& Field : Object->Values)
					if (Field.Value == JsonValue) { ValuePointer = TEXT("/") + Field.Key; break; }
			InputPointers.Add(SemanticId, ValuePointer.IsEmpty() ? TEXT("/") : ValuePointer);
			FUeAgentCanonicalValuePtr Proposed;
			if (Intent != EChangeIntent::Delete
				&& !TryCanonical(
					JsonValue, Resources, Proposed, OutError, SemanticId))
			{
				if (bGeneratedValue) MarkGraphCompoundNativeDiagnostic(OutError);
				else BindGraphCompoundInputValueDiagnostic(OutError, ValuePointer.IsEmpty() ? TEXT("/") : ValuePointer);
				return false;
			}
			const FUeAgentCanonicalIdentity EditIdentity =
				MakeIdentity(SemanticId, MoveTemp(Selectors));
			Edits.Emplace(
				EditIdentity, ElementKind, Intent, Proposed,
				MoveTemp(Dependencies), Authority, Evidence,
				TArray<FUeAgentCanonicalIdentity>({EditIdentity}));
			return true;
		};

		using I = EAudioMediaGraphInputKind;
		switch (Profile.GetInputKind())
		{
		case I::AssetCreate:
		{
			const TSharedRef<FJsonObject> Definition = MakeShared<FJsonObject>();
			Definition->SetStringField(TEXT("asset_kind"),
				Object->GetStringField(TEXT("asset_kind")));
			Definition->SetStringField(TEXT("native_class_ref"),
				NativeAssetClass(Object->GetStringField(TEXT("asset_kind"))));
			if (!AddEdit(
					TEXT("target:") + TargetRef,
					EGraphCompoundElementKind::Target,
					EChangeIntent::Create,
					MakeShared<FJsonValueObject>(Definition), {}, {}, true))
			{
				return false;
			}
			break;
		}
		case I::MetaSoundInterface:
			for (int32 InputIndex = 0; InputIndex < Object->GetArrayField(TEXT("changes")).Num(); ++InputIndex)
			{
				const auto& Value = Object->GetArrayField(TEXT("changes"))[InputIndex];
				TGuardValue<FString> InputScope(InputPointer, FString(TEXT("/")) + TEXT("changes") + TEXT("/") + ::LexToString(InputIndex));
				const TSharedPtr<FJsonObject> Change = Value->AsObject();
				EChangeIntent Intent = EChangeIntent::Update;
				if (!ReadIntent(Change, Value, TEXT("audio_media_graph_interface_change_invalid"), TEXT("changes"), InputIndex, Intent))
				{
					return false;
				}
				const FString SemanticId = FString::Printf(
					TEXT("input:metasound_interface:%s:%s"),
					*Change->GetStringField(TEXT("interface_kind")),
					*Change->GetStringField(TEXT("name")));
				if (!AddEdit(
						SemanticId, EGraphCompoundElementKind::Input, Intent,
						ObjectWithout(*Change, {TEXT("intent")}),
						{},
						AudioMediaIdentity::MetaSoundInterface(
							Change->GetStringField(TEXT("interface_kind")),
							Change->GetStringField(TEXT("name")))))
				{
					return false;
				}
			}
			break;
		case I::MetaSoundNode:
			for (int32 InputIndex = 0; InputIndex < Object->GetArrayField(TEXT("nodes")).Num(); ++InputIndex)
			{
				const auto& Value = Object->GetArrayField(TEXT("nodes"))[InputIndex];
				TGuardValue<FString> InputScope(InputPointer, FString(TEXT("/")) + TEXT("nodes") + TEXT("/") + ::LexToString(InputIndex));
				const TSharedPtr<FJsonObject> Node = Value->AsObject();
				EChangeIntent Intent = EChangeIntent::Update;
				if (!ReadIntent(Node, Value, TEXT("audio_media_graph_node_invalid"), TEXT("nodes"), InputIndex, Intent))
				{
					return false;
				}
				const FString NodeGuid =
					Node->GetStringField(TEXT("node_guid"));
				if (!AddEdit(
						TEXT("item:metasound_node:") + NodeGuid,
						EGraphCompoundElementKind::Item, Intent,
						ObjectWithout(*Node, {TEXT("intent")}),
						{},
						AudioMediaIdentity::MetaSoundNode(NodeGuid)))
				{
					return false;
				}
			}
			break;
		case I::MetaSoundLink:
			for (int32 InputIndex = 0; InputIndex < Object->GetArrayField(TEXT("links")).Num(); ++InputIndex)
			{
				const auto& Value = Object->GetArrayField(TEXT("links"))[InputIndex];
				TGuardValue<FString> InputScope(InputPointer, FString(TEXT("/")) + TEXT("links") + TEXT("/") + ::LexToString(InputIndex));
				const TSharedPtr<FJsonObject> Link = Value->AsObject();
				const TSharedPtr<FJsonObject> From = Link.IsValid()
					? Link->GetObjectField(TEXT("from")) : nullptr;
				const TSharedPtr<FJsonObject> To = Link.IsValid()
					? Link->GetObjectField(TEXT("to")) : nullptr;
				EChangeIntent Intent = EChangeIntent::Update;
				if (!From.IsValid() || !To.IsValid())
				{
					const FString Field = !Link.IsValid() ? TEXT("links")
						: !From.IsValid() ? TEXT("from") : TEXT("to");
					return FailInput(TEXT("audio_media_graph_link_invalid"),
						!Link.IsValid() ? TEXT("A graph link must be an object.") : TEXT("A graph link endpoint must be an object."),
						Field, !Link.IsValid() ? InputPointer : InputPointer + TEXT("/") + Field,
						!Link.IsValid() ? Value : Link->Values.FindRef(Field),
						MakeShared<FJsonValueString>(TEXT("object")), InputIndex);
				}
				if (!ReadIntent(Link, Value, TEXT("audio_media_graph_link_invalid"), TEXT("links"), InputIndex, Intent))
					return false;
				const FString SemanticId = FString::Printf(
					TEXT("relation:metasound_link:%s:%s->%s:%s:%s"),
					*From->GetStringField(TEXT("node_guid")),
					*From->GetStringField(TEXT("vertex_guid")),
					*To->GetStringField(TEXT("node_guid")),
					*To->GetStringField(TEXT("vertex_guid")),
					*From->GetStringField(TEXT("data_type_ref")));
				const FString FromNodeGuid =
					From->GetStringField(TEXT("node_guid"));
				const FString FromVertexGuid =
					From->GetStringField(TEXT("vertex_guid"));
				const FString ToNodeGuid =
					To->GetStringField(TEXT("node_guid"));
				const FString ToVertexGuid =
					To->GetStringField(TEXT("vertex_guid"));
				const TArray<FUeAgentCanonicalIdentity> Dependencies = {
					MakeIdentity(
						TEXT("item:metasound_node:") + FromNodeGuid,
						AudioMediaIdentity::MetaSoundNode(FromNodeGuid)),
					MakeIdentity(
						TEXT("item:metasound_node:") + ToNodeGuid,
						AudioMediaIdentity::MetaSoundNode(ToNodeGuid))};
				if (!AddEdit(
						SemanticId, EGraphCompoundElementKind::Relation, Intent,
						ObjectWithout(*Link, {TEXT("intent")}), Dependencies,
						AudioMediaIdentity::MetaSoundLink(
							FromNodeGuid,
							FromVertexGuid,
							ToNodeGuid,
							ToVertexGuid)))
				{
					return false;
				}
			}
			break;
		case I::MetaSoundDefault:
		{
			EChangeIntent Intent = EChangeIntent::Update;
			TryIntent(Object->GetStringField(TEXT("intent")), Intent);
			const FString SemanticId = FString::Printf(
				TEXT("input:metasound_default:%s:%s:%s"),
				*Object->GetStringField(TEXT("node_guid")),
				*Object->GetStringField(TEXT("vertex_guid")),
				*Object->GetStringField(TEXT("data_type_ref")));
			if (!AddEdit(
					SemanticId, EGraphCompoundElementKind::Input, Intent,
					ObjectWithout(*Object, {TEXT("intent"), TEXT("target_ref")}),
					{MakeIdentity(
						TEXT("item:metasound_node:")
							+ Object->GetStringField(TEXT("node_guid")),
						AudioMediaIdentity::MetaSoundNode(
							Object->GetStringField(TEXT("node_guid"))))},
					AudioMediaIdentity::MetaSoundDefault(
						Object->GetStringField(TEXT("node_guid")),
						Object->GetStringField(TEXT("vertex_guid")))))
			{
				return false;
			}
			break;
		}
		case I::MetaSoundCompileSettle:
			if (!AddEdit(
					TEXT("dependency:metasound_compile_settle"),
					EGraphCompoundElementKind::Dependency,
					EChangeIntent::Update,
					ObjectWithout(*Object, {TEXT("target_ref")})))
			{
				return false;
			}
			break;
		case I::SoundCueNode:
			for (int32 InputIndex = 0; InputIndex < Object->GetArrayField(TEXT("nodes")).Num(); ++InputIndex)
			{
				const auto& Value = Object->GetArrayField(TEXT("nodes"))[InputIndex];
				TGuardValue<FString> InputScope(InputPointer, FString(TEXT("/")) + TEXT("nodes") + TEXT("/") + ::LexToString(InputIndex));
				const TSharedPtr<FJsonObject> Node = Value->AsObject();
				EChangeIntent Intent = EChangeIntent::Update;
				if (!ReadIntent(Node, Value, TEXT("audio_media_graph_node_invalid"), TEXT("nodes"), InputIndex, Intent))
				{
					return false;
				}
				const FString ObjectPath =
					Node->GetStringField(TEXT("object_path"));
				if (!AddEdit(
						TEXT("item:sound_node:") + ObjectPath,
						EGraphCompoundElementKind::Item, Intent,
						ObjectWithout(*Node, {TEXT("intent")}),
						{},
						AudioMediaIdentity::SoundCueNode(ObjectPath)))
				{
					return false;
				}
			}
			break;
		case I::SoundCueRelation:
			for (int32 InputIndex = 0; InputIndex < Object->GetArrayField(TEXT("relations")).Num(); ++InputIndex)
			{
				const auto& Value = Object->GetArrayField(TEXT("relations"))[InputIndex];
				TGuardValue<FString> InputScope(InputPointer, FString(TEXT("/")) + TEXT("relations") + TEXT("/") + ::LexToString(InputIndex));
				const TSharedPtr<FJsonObject> Relation = Value->AsObject();
				EChangeIntent Intent = EChangeIntent::Update;
				if (!ReadIntent(Relation, Value, TEXT("audio_media_graph_relation_invalid"), TEXT("relations"), InputIndex, Intent))
				{
					return false;
				}
				const FString Parent = Relation->GetStringField(
					TEXT("parent_object_path"));
				const FString Child = Relation->GetStringField(
					TEXT("child_object_path"));
				if (!AddEdit(
						FString::Printf(TEXT("relation:sound_child:%s->%s"),
							*Parent, *Child),
						EGraphCompoundElementKind::Relation, Intent,
						ObjectWithout(*Relation, {TEXT("intent")}),
						{MakeIdentity(
							TEXT("item:sound_node:") + Parent,
							AudioMediaIdentity::SoundCueNode(Parent)),
						 MakeIdentity(
							TEXT("item:sound_node:") + Child,
							AudioMediaIdentity::SoundCueNode(Child))},
						AudioMediaIdentity::SoundCueRelation(Parent, Child)))
				{
					return false;
				}
			}
			break;
		case I::SoundCueRoot:
		{
			EChangeIntent Intent = EChangeIntent::Update;
			TryIntent(Object->GetStringField(TEXT("intent")), Intent);
			const FString Root = Object->GetStringField(TEXT("root_node_object_path"));
			const TSharedRef<FJsonObject> RootValue = MakeShared<FJsonObject>();
			RootValue->SetStringField(TEXT("root_node_object_path"), Root);
			if (!AddEdit(
					TEXT("binding:sound_root"),
					EGraphCompoundElementKind::Binding, Intent,
					MakeShared<FJsonValueObject>(RootValue),
					{MakeIdentity(
						TEXT("item:sound_node:") + Root,
						AudioMediaIdentity::SoundCueNode(Root))}))
			{
				return false;
			}
			break;
		}
		case I::SoundCueCompileSettle:
			if (!AddEdit(
					TEXT("dependency:sound_compile_settle"),
					EGraphCompoundElementKind::Dependency,
					EChangeIntent::Update,
					ObjectWithout(*Object, {TEXT("target_ref")})))
			{
				return false;
			}
			break;
		case I::TextureGraphNode:
			for (int32 InputIndex = 0; InputIndex < Object->GetArrayField(TEXT("nodes")).Num(); ++InputIndex)
			{
				const auto& Value = Object->GetArrayField(TEXT("nodes"))[InputIndex];
				TGuardValue<FString> InputScope(InputPointer, FString(TEXT("/")) + TEXT("nodes") + TEXT("/") + ::LexToString(InputIndex));
				const TSharedPtr<FJsonObject> Node = Value->AsObject();
				EChangeIntent Intent = EChangeIntent::Update;
				if (!ReadIntent(Node, Value, TEXT("audio_media_graph_node_invalid"), TEXT("nodes"), InputIndex, Intent))
				{
					return false;
				}
				const FString ElementId =
					Node->GetStringField(TEXT("element_id"));
				const FString SemanticId =
					TEXT("item:texture_expression:") + ElementId;
				if (!AddEdit(
						SemanticId, EGraphCompoundElementKind::Item, Intent,
						ObjectWithout(*Node, {TEXT("intent")}),
						{},
						AudioMediaIdentity::TextureGraphNode(ElementId)))
				{
					return false;
				}
			}
			break;
		case I::TextureGraphLink:
			for (int32 InputIndex = 0; InputIndex < Object->GetArrayField(TEXT("links")).Num(); ++InputIndex)
			{
				const auto& Value = Object->GetArrayField(TEXT("links"))[InputIndex];
				TGuardValue<FString> InputScope(InputPointer, FString(TEXT("/")) + TEXT("links") + TEXT("/") + ::LexToString(InputIndex));
				const TSharedPtr<FJsonObject> Link = Value->AsObject();
				const TSharedPtr<FJsonObject> From = Link.IsValid()
					? Link->GetObjectField(TEXT("from")) : nullptr;
				const TSharedPtr<FJsonObject> To = Link.IsValid()
					? Link->GetObjectField(TEXT("to")) : nullptr;
				EChangeIntent Intent = EChangeIntent::Update;
				if (!From.IsValid() || !To.IsValid())
				{
					const FString Field = !Link.IsValid() ? TEXT("links")
						: !From.IsValid() ? TEXT("from") : TEXT("to");
					return FailInput(TEXT("audio_media_graph_link_invalid"),
						!Link.IsValid() ? TEXT("A graph link must be an object.") : TEXT("A graph link endpoint must be an object."),
						Field, !Link.IsValid() ? InputPointer : InputPointer + TEXT("/") + Field,
						!Link.IsValid() ? Value : Link->Values.FindRef(Field),
						MakeShared<FJsonValueString>(TEXT("object")), InputIndex);
				}
				if (!ReadIntent(Link, Value, TEXT("audio_media_graph_link_invalid"), TEXT("links"), InputIndex, Intent))
					return false;
				const FString FromId = TexturePinSemanticId(*From);
				const FString ToId = TexturePinSemanticId(*To);
				const FString FromElementId =
					From->GetStringField(TEXT("element_id"));
				const FString FromPinName =
					From->GetStringField(TEXT("pin_name"));
				const FString ToElementId =
					To->GetStringField(TEXT("element_id"));
				const FString ToPinName =
					To->GetStringField(TEXT("pin_name"));
				if (!AddEdit(
						TEXT("relation:texture_link:") + FromId + TEXT("->") + ToId,
						EGraphCompoundElementKind::Relation, Intent,
						ObjectWithout(*Link, {TEXT("intent")}),
						{MakeIdentity(
							FromId,
							AudioMediaIdentity::TextureGraphPin(
								FromElementId, TEXT("output"), FromPinName)),
						 MakeIdentity(
							ToId,
							AudioMediaIdentity::TextureGraphPin(
								ToElementId, TEXT("input"), ToPinName))},
						AudioMediaIdentity::TextureGraphLink(
							FromElementId,
							FromPinName,
							ToElementId,
							ToPinName)))
				{
					return false;
				}
			}
			break;
		case I::TextureGraphParameter:
			for (int32 InputIndex = 0; InputIndex < Object->GetArrayField(TEXT("parameters")).Num(); ++InputIndex)
			{
				const auto& Value = Object->GetArrayField(TEXT("parameters"))[InputIndex];
				TGuardValue<FString> InputScope(InputPointer, FString(TEXT("/")) + TEXT("parameters") + TEXT("/") + ::LexToString(InputIndex));
				const TSharedPtr<FJsonObject> Parameter = Value->AsObject();
				EChangeIntent Intent = EChangeIntent::Update;
				if (!ReadIntent(Parameter, Value, TEXT("audio_media_graph_parameter_invalid"), TEXT("parameters"), InputIndex, Intent))
				{
					return false;
				}
				const FString ElementId =
					Parameter->GetStringField(TEXT("element_id"));
				const FString PinName =
					Parameter->GetStringField(TEXT("pin_name"));
				const FString PinId = FString::Printf(
					TEXT("pin:texture:%s:input:%s"),
					*ElementId,
					*PinName);
				const FString SemanticId = FString::Printf(
					TEXT("input:texture_parameter:%s:%s"),
					*ElementId,
					*PinName);
				if (!AddEdit(
						SemanticId, EGraphCompoundElementKind::Input, Intent,
						ObjectWithout(*Parameter, {TEXT("intent")}),
						{MakeIdentity(
							PinId,
							AudioMediaIdentity::TextureGraphPin(
								ElementId, TEXT("input"), PinName))},
						AudioMediaIdentity::TextureGraphParameter(
							ElementId, PinName)))
				{
					return false;
				}
			}
			break;
		case I::TextureGraphValidateEvaluateSettle:
			if (!AddEdit(
					TEXT("dependency:texture_graph_settle"),
					EGraphCompoundElementKind::Dependency,
					EChangeIntent::Update,
					ObjectWithout(*Object, {TEXT("target_ref")})))
			{
				return false;
			}
			break;
		default:
			OutError = RouteFailure(TEXT("audio_media_graph_input_kind_invalid"),
				TEXT("The audio/media typed operation input kind is not supported."), Profile.GetCapabilityId(), TEXT("input_kind"),
				MakeShared<FJsonValueNumber>(static_cast<int32>(Profile.GetInputKind())),
				MakeShared<FJsonValueString>(TEXT("a supported audio/media typed operation input kind")));
			return false;
		}

		OutPayload = FGraphCompoundCanonicalPayload(
			GetGraphCompoundCanonicalSchemaId(), MoveTemp(Edits), MoveTemp(InputPointers));
		if (OutPayload.GetEdits().IsEmpty())
			return FailInput(TEXT("audio_media_graph_typed_operation_empty"),
				TEXT("The typed operation contains no edits."), TEXT("edits"), TEXT("/"),
				MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("at least one edit")));
		return true;
	}

	bool TryBuildAudioMediaGraphTypedOperationDraft(
		const FAudioMediaGraphRouteProfile& Profile,
		const FUeAgentDescriptorIdentity& DescriptorIdentity,
		const FUeAgentContractValue& Input,
		const FUeAgentCanonicalSnapshotRef& Snapshot,
		const FString& RequestId,
		const FString& IdempotencyKey,
		FUeAgentResourceTracker& Resources,
		TOptional<FChangeSetDraft>& OutDraft,
		FGraphCompoundRouteError& OutError)
	{
		OutDraft.Reset();
		OutError = {};
		if (DescriptorIdentity
			!= Profile.GetExecutionContract().GetDescriptorIdentity())
		{
			const FUeAgentDescriptorIdentity& Expected = Profile.GetExecutionContract().GetDescriptorIdentity();
			OutError = RouteFailure(TEXT("audio_media_graph_descriptor_identity_mismatch"),
				TEXT("The operation descriptor does not match the graph route."),
				Profile.GetDescriptorId(), TEXT("descriptor_identity"), nullptr, nullptr);
			if (DescriptorIdentity.GetDescriptorId() != Expected.GetDescriptorId())
			{
				OutError.PropertyPath = TEXT("descriptor_identity.descriptor_id");
				OutError.ActualValue = MakeShared<FJsonValueString>(DescriptorIdentity.GetDescriptorId());
				OutError.ExpectedValue = MakeShared<FJsonValueString>(Expected.GetDescriptorId());
			}
			else if (DescriptorIdentity.GetVersion() != Expected.GetVersion())
			{
				OutError.PropertyPath = TEXT("descriptor_identity.version");
				OutError.ActualValue = MakeShared<FJsonValueNumber>(DescriptorIdentity.GetVersion());
				OutError.ExpectedValue = MakeShared<FJsonValueNumber>(Expected.GetVersion());
			}
			else
			{
				OutError.PublicMessage = TEXT("The operation descriptor content does not match the graph route.");
				OutError.PropertyPath = TEXT("descriptor_identity.descriptor_hash");
				OutError.ActualEvidence = MakeShared<FJsonValueString>(DescriptorIdentity.GetDescriptorHash());
				OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Expected.GetDescriptorHash());
			}
			return false;
		}
		FGraphCompoundCanonicalPayload Payload(
			GetGraphCompoundCanonicalSchemaId(), {});
		if (!TryDecodeAudioMediaGraphTypedOperation(
				Profile, Input, Snapshot, Resources, Payload, OutError))
		{
			return false;
		}
		FGraphCompoundCanonicalError CanonicalError;
		if (!TryBuildGraphCompoundChangeSetDraft(
				Payload, Snapshot, DescriptorIdentity,
				RequestId, IdempotencyKey, Resources,
				OutDraft, CanonicalError))
		{
			Payload.LocateDiagnostic(CanonicalError);
			SetError(OutError, CanonicalError);
			return false;
		}
		return OutDraft.IsSet();
	}
}

#include "Composition/GraphCompound/UeAgentGraphCompoundAudioMediaRoutes.h"

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

		FString CanonicalSchema(const TCHAR* Text)
		{
			TSharedPtr<FJsonObject> Object;
			FString Error;
			FString Canonical;
			return Json::TryParseObject(Text, Object, Error)
				&& Json::TryCanonicalize(Object, Canonical, Error)
				? Canonical : FString();
		}

		FString InputSchema(const FRouteSpec& Spec)
		{
			using I = EAudioMediaGraphInputKind;
			switch (Spec.InputKind)
			{
			case I::AssetCreate:
				return CanonicalSchema(TEXT("{\"additionalProperties\":false,\"properties\":{\"asset_kind\":{\"enum\":[\"metasound_source\",\"sound_cue\",\"texture_graph\"],\"type\":\"string\"},\"asset_name\":{\"minLength\":1,\"type\":\"string\"},\"package_path\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"asset_kind\",\"asset_name\",\"package_path\"],\"type\":\"object\"}"));
			case I::MetaSoundInterface:
				return CanonicalSchema(TEXT("{\"additionalProperties\":false,\"properties\":{\"changes\":{\"items\":{\"additionalProperties\":false,\"properties\":{\"access_type\":{\"enum\":[\"value\",\"reference\"],\"type\":\"string\"},\"data_type_ref\":{\"minLength\":1,\"type\":\"string\"},\"default_value\":{},\"intent\":{\"enum\":[\"create\",\"update\",\"replace\",\"delete\"],\"type\":\"string\"},\"interface_kind\":{\"enum\":[\"input\",\"output\"],\"type\":\"string\"},\"name\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"access_type\",\"data_type_ref\",\"intent\",\"interface_kind\",\"name\"],\"type\":\"object\"},\"minItems\":1,\"type\":\"array\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"changes\",\"target_ref\"],\"type\":\"object\"}"));
			case I::MetaSoundNode:
				return CanonicalSchema(TEXT("{\"additionalProperties\":false,\"properties\":{\"nodes\":{\"items\":{\"additionalProperties\":false,\"properties\":{\"class_name\":{\"minLength\":1,\"type\":\"string\"},\"intent\":{\"enum\":[\"create\",\"update\",\"replace\",\"delete\"],\"type\":\"string\"},\"major_version\":{\"minimum\":0,\"type\":\"integer\"},\"minor_version\":{\"minimum\":0,\"type\":\"integer\"},\"node_guid\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"class_name\",\"intent\",\"major_version\",\"minor_version\",\"node_guid\"],\"type\":\"object\"},\"minItems\":1,\"type\":\"array\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"nodes\",\"target_ref\"],\"type\":\"object\"}"));
			case I::MetaSoundLink:
				return CanonicalSchema(TEXT("{\"additionalProperties\":false,\"properties\":{\"links\":{\"items\":{\"additionalProperties\":false,\"properties\":{\"from\":{\"additionalProperties\":false,\"properties\":{\"data_type_ref\":{\"minLength\":1,\"type\":\"string\"},\"direction\":{\"enum\":[\"output\"],\"type\":\"string\"},\"node_guid\":{\"minLength\":1,\"type\":\"string\"},\"vertex_guid\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"data_type_ref\",\"direction\",\"node_guid\",\"vertex_guid\"],\"type\":\"object\"},\"intent\":{\"enum\":[\"create\",\"delete\"],\"type\":\"string\"},\"to\":{\"additionalProperties\":false,\"properties\":{\"data_type_ref\":{\"minLength\":1,\"type\":\"string\"},\"direction\":{\"enum\":[\"input\"],\"type\":\"string\"},\"node_guid\":{\"minLength\":1,\"type\":\"string\"},\"vertex_guid\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"data_type_ref\",\"direction\",\"node_guid\",\"vertex_guid\"],\"type\":\"object\"}},\"required\":[\"from\",\"intent\",\"to\"],\"type\":\"object\"},\"minItems\":1,\"type\":\"array\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"links\",\"target_ref\"],\"type\":\"object\"}"));
			case I::MetaSoundDefault:
				return CanonicalSchema(TEXT("{\"additionalProperties\":false,\"properties\":{\"data_type_ref\":{\"minLength\":1,\"type\":\"string\"},\"intent\":{\"enum\":[\"update\",\"replace\"],\"type\":\"string\"},\"node_guid\":{\"minLength\":1,\"type\":\"string\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"},\"value\":{},\"vertex_guid\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"data_type_ref\",\"intent\",\"node_guid\",\"target_ref\",\"value\",\"vertex_guid\"],\"type\":\"object\"}"));
			case I::MetaSoundCompileSettle:
				return CanonicalSchema(TEXT("{\"additionalProperties\":false,\"properties\":{\"expected_document_revision\":{\"minLength\":1,\"type\":\"string\"},\"registration_policy\":{\"enum\":[\"register_and_compile\"],\"type\":\"string\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"expected_document_revision\",\"registration_policy\",\"target_ref\"],\"type\":\"object\"}"));
			case I::SoundCueNode:
				return CanonicalSchema(TEXT("{\"additionalProperties\":false,\"properties\":{\"nodes\":{\"items\":{\"additionalProperties\":false,\"properties\":{\"intent\":{\"enum\":[\"create\",\"update\",\"replace\",\"delete\"],\"type\":\"string\"},\"native_class_ref\":{\"minLength\":1,\"type\":\"string\"},\"object_path\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"intent\",\"native_class_ref\",\"object_path\"],\"type\":\"object\"},\"minItems\":1,\"type\":\"array\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"nodes\",\"target_ref\"],\"type\":\"object\"}"));
			case I::SoundCueRelation:
				return CanonicalSchema(TEXT("{\"additionalProperties\":false,\"properties\":{\"relations\":{\"items\":{\"additionalProperties\":false,\"properties\":{\"child_object_path\":{\"minLength\":1,\"type\":\"string\"},\"intent\":{\"enum\":[\"create\",\"update\",\"replace\",\"delete\"],\"type\":\"string\"},\"order_index\":{\"minimum\":0,\"type\":\"integer\"},\"parent_object_path\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"child_object_path\",\"intent\",\"order_index\",\"parent_object_path\"],\"type\":\"object\"},\"minItems\":1,\"type\":\"array\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"relations\",\"target_ref\"],\"type\":\"object\"}"));
			case I::SoundCueRoot:
				return CanonicalSchema(TEXT("{\"additionalProperties\":false,\"properties\":{\"intent\":{\"enum\":[\"update\",\"replace\"],\"type\":\"string\"},\"root_node_object_path\":{\"minLength\":1,\"type\":\"string\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"intent\",\"root_node_object_path\",\"target_ref\"],\"type\":\"object\"}"));
			case I::SoundCueCompileSettle:
				return CanonicalSchema(TEXT("{\"additionalProperties\":false,\"properties\":{\"expected_graph_revision\":{\"minLength\":1,\"type\":\"string\"},\"sync_policy\":{\"enum\":[\"sync_and_compile\"],\"type\":\"string\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"expected_graph_revision\",\"sync_policy\",\"target_ref\"],\"type\":\"object\"}"));
			case I::TextureGraphNode:
				return CanonicalSchema(TEXT("{\"additionalProperties\":false,\"properties\":{\"nodes\":{\"items\":{\"additionalProperties\":false,\"properties\":{\"element_id\":{\"minLength\":1,\"type\":\"string\"},\"expression_class_ref\":{\"minLength\":1,\"type\":\"string\"},\"intent\":{\"enum\":[\"create\",\"update\",\"replace\",\"delete\"],\"type\":\"string\"}},\"required\":[\"element_id\",\"expression_class_ref\",\"intent\"],\"type\":\"object\"},\"minItems\":1,\"type\":\"array\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"nodes\",\"target_ref\"],\"type\":\"object\"}"));
			case I::TextureGraphLink:
				return CanonicalSchema(TEXT("{\"additionalProperties\":false,\"properties\":{\"links\":{\"items\":{\"additionalProperties\":false,\"properties\":{\"from\":{\"additionalProperties\":false,\"properties\":{\"direction\":{\"enum\":[\"output\"],\"type\":\"string\"},\"element_id\":{\"minLength\":1,\"type\":\"string\"},\"pin_name\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"direction\",\"element_id\",\"pin_name\"],\"type\":\"object\"},\"intent\":{\"enum\":[\"create\",\"delete\"],\"type\":\"string\"},\"to\":{\"additionalProperties\":false,\"properties\":{\"direction\":{\"enum\":[\"input\"],\"type\":\"string\"},\"element_id\":{\"minLength\":1,\"type\":\"string\"},\"pin_name\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"direction\",\"element_id\",\"pin_name\"],\"type\":\"object\"}},\"required\":[\"from\",\"intent\",\"to\"],\"type\":\"object\"},\"minItems\":1,\"type\":\"array\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"links\",\"target_ref\"],\"type\":\"object\"}"));
			case I::TextureGraphParameter:
				return CanonicalSchema(TEXT("{\"additionalProperties\":false,\"properties\":{\"parameters\":{\"items\":{\"additionalProperties\":false,\"properties\":{\"alias\":{\"minLength\":1,\"type\":\"string\"},\"element_id\":{\"minLength\":1,\"type\":\"string\"},\"intent\":{\"enum\":[\"update\",\"replace\"],\"type\":\"string\"},\"pin_name\":{\"minLength\":1,\"type\":\"string\"},\"value\":{}},\"required\":[\"alias\",\"element_id\",\"intent\",\"pin_name\",\"value\"],\"type\":\"object\"},\"minItems\":1,\"type\":\"array\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"parameters\",\"target_ref\"],\"type\":\"object\"}"));
			case I::TextureGraphValidateEvaluateSettle:
				return CanonicalSchema(TEXT("{\"additionalProperties\":false,\"properties\":{\"evaluation_policy\":{\"enum\":[\"validate_and_evaluate\"],\"type\":\"string\"},\"expected_graph_revision\":{\"minLength\":1,\"type\":\"string\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"evaluation_policy\",\"expected_graph_revision\",\"target_ref\"],\"type\":\"object\"}"));
			case I::TextureGraphArtifactCapture:
				return CanonicalSchema(TEXT("{\"additionalProperties\":false,\"properties\":{\"expected_settled_revision\":{\"minLength\":1,\"type\":\"string\"},\"outputs\":{\"items\":{\"additionalProperties\":false,\"properties\":{\"element_id\":{\"minLength\":1,\"type\":\"string\"},\"output_name\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"element_id\",\"output_name\"],\"type\":\"object\"},\"minItems\":1,\"type\":\"array\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"expected_settled_revision\",\"outputs\",\"target_ref\"],\"type\":\"object\"}"));
			default:
				return FString();
			}
		}

		FString ResultSchema(const FRouteSpec& Spec)
		{
			if (Spec.RouteKind == EGraphCompoundDomainRouteKind::Query)
			{
				return CanonicalSchema(TEXT("{\"additionalProperties\":false,\"properties\":{\"outputs\":{\"items\":{\"additionalProperties\":false,\"properties\":{\"evaluated_value_type\":{\"type\":\"string\"},\"expression_object_path\":{\"type\":\"string\"},\"node_index\":{\"type\":\"integer\"},\"output_name\":{\"type\":\"string\"},\"pin_index\":{\"type\":\"integer\"},\"settings_fingerprint\":{\"type\":\"string\"}},\"required\":[\"evaluated_value_type\",\"expression_object_path\",\"node_index\",\"output_name\",\"pin_index\",\"settings_fingerprint\"],\"type\":\"object\"},\"type\":\"array\"},\"published\":{\"enum\":[false]},\"settled_revision\":{\"type\":\"string\"},\"target_ref\":{\"type\":\"string\"}},\"required\":[\"outputs\",\"published\",\"settled_revision\",\"target_ref\"],\"type\":\"object\"}"));
			}
			return CanonicalSchema(TEXT("{\"additionalProperties\":false,\"properties\":{\"revision\":{\"type\":\"string\"},\"target_ref\":{\"type\":\"string\"}},\"required\":[\"target_ref\",\"revision\"],\"type\":\"object\"}"));
		}

		TSharedPtr<FJsonObject> DescriptorObject(
			const FRouteSpec& Spec,
			const FString& Input,
			const FString& Result,
			FString& OutError)
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

		bool ValidateSpecs(FString& OutError)
		{
			const TArray<FAudioMediaGraphCapabilityContract>& Contracts =
				GetAudioMediaGraphCapabilityContracts();
			if (Contracts.Num() != RouteSpecs().Num())
			{
				OutError = TEXT("audio_media_graph_contract_count_mismatch");
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
					OutError = FString(TEXT("audio_media_graph_contract_mismatch:"))
						+ Spec.CapabilityId;
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
			FString Error;
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
				const FString Input = InputSchema(Spec);
				const FString Result = ResultSchema(Spec);
				TSharedPtr<FJsonObject> Object =
					DescriptorObject(Spec, Input, Result, Bundle.Error);
				FString CanonicalDescriptor;
				if (Input.IsEmpty() || Result.IsEmpty() || !Object.IsValid()
					|| !Json::TryCanonicalize(
						Object, CanonicalDescriptor, Bundle.Error))
				{
					Bundle.Profiles.Reset();
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

		void SetError(
			FGraphCompoundRouteError& OutError,
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString())
		{
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
		}

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
			if (!Profile.IsValid() || !Bundle.Registry.IsValid()
				|| !FindSpec(Profile))
			{
				SetError(OutError, TEXT("audio_media_graph_route_unavailable"),
					Profile.GetCapabilityId(), Bundle.Error);
				return false;
			}
			const FUeAgentOperationDescriptor* Descriptor =
				Bundle.Registry->FindByDescriptorId(Profile.GetDescriptorId());
			const FUeAgentContractSchema* Schema = Descriptor
				? Bundle.Registry->FindCompiledInputSchemaExact(
					Descriptor->GetIdentity()) : nullptr;
			if (!Descriptor || !Schema)
			{
				SetError(OutError, TEXT("audio_media_graph_descriptor_missing"),
					Profile.GetDescriptorId());
				return false;
			}
			const FUeAgentContractSchemaCheckpoint Checkpoint =
				[&Resources](
					const int64 WorkUnits,
					const FString& InstancePath,
					FUeAgentContractSchemaError& Error)
				{
					FUeAgentResourceBudgetExceeded BudgetError;
					if (!Resources.TryCharge(
							EUeAgentResourceDimension::WorkUnits,
							WorkUnits,
							TEXT("graph_compound.gc017.schema"),
							BudgetError))
					{
						Error.Code = BudgetError.Code;
						Error.InstancePath = InstancePath;
						Error.Detail = TEXT("Schema work budget exceeded.");
						return false;
					}
					return true;
				};
			TOptional<FUeAgentContractValue> Canonical;
			FUeAgentContractSchemaError SchemaError;
			if (!Schema->TryValidateAndCanonicalize(
					Input, Checkpoint, Canonical, SchemaError)
				|| !Canonical.IsSet())
			{
				SetError(OutError,
					SchemaError.Code.IsEmpty()
						? TEXT("audio_media_graph_input_invalid")
						: SchemaError.Code,
					SchemaError.InstancePath, SchemaError.Detail);
				return false;
			}
			FString ParseError;
			if (!Json::TryParseObject(
					Canonical->GetJsonValue(), OutObject, ParseError))
			{
				SetError(OutError, TEXT("audio_media_graph_input_parse_failed"),
					Profile.GetCapabilityId(), ParseError);
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
			const FString& Generation)
		{
			return FUeAgentCanonicalIdentity(
				SemanticId, TargetKind, OwnerId, SemanticId,
				TEXT("graph_compound"), Generation);
		}

		bool TryCanonical(
			const TSharedPtr<FJsonValue>& JsonValue,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FGraphCompoundRouteError& OutError,
			const FString& Subject)
		{
			FString Error;
			if (!JsonValue.IsValid()
				|| !CanonicalJson::TryFromJsonValue(
					JsonValue, Resources, OutValue, Error))
			{
				SetError(OutError, TEXT("audio_media_graph_value_invalid"),
					Subject, Error);
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
		bValid = !CapabilityId.IsEmpty() && !DescriptorId.IsEmpty()
			&& !DomainId.IsEmpty() && !FamilyId.IsEmpty()
			&& !TargetKind.IsEmpty()
			&& (bQuery
				|| RouteKind == EGraphCompoundDomainRouteKind::TypedOperation)
			&& InputKind != EAudioMediaGraphInputKind::Invalid
			&& (!bRequiresNativeBackendAvailability
				|| !UnavailableErrorCode.IsEmpty())
			&& ExecutionContract.IsValid()
			&& ExecutionContract.GetLane()
				== (bQuery ? EGraphCompoundExecutionLane::Query
					: EGraphCompoundExecutionLane::Operation);
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
	bool FAudioMediaGraphRouteProfile::IsValid() const { return bValid; }

	const TArray<FAudioMediaGraphRouteProfile>& GetAudioMediaGraphRouteProfiles()
	{
		return RouteBundle().Profiles;
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
		FString& OutError)
	{
		OutRegistry = RouteBundle().Registry;
		OutError = RouteBundle().Error;
		return OutRegistry.IsValid() && OutError.IsEmpty();
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
		bValid = !ElementId.IsEmpty() && !OutputName.IsEmpty();
	}

	const FString& FAudioMediaGraphArtifactOutputSelector::GetElementId() const { return ElementId; }
	const FString& FAudioMediaGraphArtifactOutputSelector::GetOutputName() const { return OutputName; }
	bool FAudioMediaGraphArtifactOutputSelector::IsValid() const { return bValid; }

	FAudioMediaGraphArtifactQueryRequest::FAudioMediaGraphArtifactQueryRequest(
		FString InTargetRef,
		FString InExpectedSettledRevision,
		TArray<FAudioMediaGraphArtifactOutputSelector> InOutputs)
		: TargetRef(MoveTemp(InTargetRef))
		, ExpectedSettledRevision(MoveTemp(InExpectedSettledRevision))
		, Outputs(MoveTemp(InOutputs))
	{
		TargetRef.TrimStartAndEndInline();
		ExpectedSettledRevision.TrimStartAndEndInline();
		bValid = !TargetRef.IsEmpty() && !ExpectedSettledRevision.IsEmpty()
			&& !Outputs.IsEmpty();
		for (const FAudioMediaGraphArtifactOutputSelector& Output : Outputs)
		{
			bValid = bValid && Output.IsValid();
		}
	}

	const FString& FAudioMediaGraphArtifactQueryRequest::GetTargetRef() const { return TargetRef; }
	const FString& FAudioMediaGraphArtifactQueryRequest::GetExpectedSettledRevision() const { return ExpectedSettledRevision; }
	const TArray<FAudioMediaGraphArtifactOutputSelector>& FAudioMediaGraphArtifactQueryRequest::GetOutputs() const { return Outputs; }
	bool FAudioMediaGraphArtifactQueryRequest::IsValid() const { return bValid; }

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
			SetError(OutError, TEXT("audio_media_graph_route_kind_mismatch"),
				Profile.GetCapabilityId(), TEXT("artifact_query"));
			return false;
		}
		TSharedPtr<FJsonObject> Object;
		if (!TryValidatedObject(Profile, Input, Resources, Object, OutError))
		{
			return false;
		}
		TArray<FAudioMediaGraphArtifactOutputSelector> Outputs;
		for (const TSharedPtr<FJsonValue>& Value
			: Object->GetArrayField(TEXT("outputs")))
		{
			const TSharedPtr<FJsonObject> Output = Value->AsObject();
			if (!Output.IsValid())
			{
				SetError(OutError, TEXT("audio_media_graph_artifact_selector_invalid"));
				return false;
			}
			Outputs.Emplace(
				Output->GetStringField(TEXT("element_id")),
				Output->GetStringField(TEXT("output_name")));
		}
		OutRequest = FAudioMediaGraphArtifactQueryRequest(
			Object->GetStringField(TEXT("target_ref")),
			Object->GetStringField(TEXT("expected_settled_revision")),
			MoveTemp(Outputs));
		if (!OutRequest.IsValid())
		{
			SetError(OutError, TEXT("audio_media_graph_artifact_query_invalid"),
				Profile.GetCapabilityId());
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
			SetError(OutError, TEXT("audio_media_graph_route_kind_mismatch"),
				Profile.GetCapabilityId(), TEXT("typed_operation"));
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

		auto MakeIdentity = [&](const FString& SemanticId)
		{
			return Identity(
				SemanticId, IdentityTargetKind, TargetRef, Generation);
		};
		auto AddEdit = [&] (
			const FString& SemanticId,
			const EGraphCompoundElementKind ElementKind,
			const EChangeIntent Intent,
			const TSharedPtr<FJsonValue>& JsonValue,
			TArray<FUeAgentCanonicalIdentity> Dependencies = {})
		{
			FUeAgentCanonicalValuePtr Proposed;
			if (Intent != EChangeIntent::Delete
				&& !TryCanonical(
					JsonValue, Resources, Proposed, OutError, SemanticId))
			{
				return false;
			}
			const FUeAgentCanonicalIdentity EditIdentity =
				MakeIdentity(SemanticId);
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
					MakeShared<FJsonValueObject>(Definition)))
			{
				return false;
			}
			break;
		}
		case I::MetaSoundInterface:
			for (const TSharedPtr<FJsonValue>& Value
				: Object->GetArrayField(TEXT("changes")))
			{
				const TSharedPtr<FJsonObject> Change = Value->AsObject();
				EChangeIntent Intent = EChangeIntent::Update;
				if (!Change.IsValid()
					|| !TryIntent(Change->GetStringField(TEXT("intent")), Intent))
				{
					SetError(OutError,
						TEXT("audio_media_graph_interface_change_invalid"));
					return false;
				}
				const FString SemanticId = FString::Printf(
					TEXT("input:metasound_interface:%s:%s"),
					*Change->GetStringField(TEXT("interface_kind")),
					*Change->GetStringField(TEXT("name")));
				if (!AddEdit(
						SemanticId, EGraphCompoundElementKind::Input, Intent,
						ObjectWithout(*Change, {TEXT("intent")})))
				{
					return false;
				}
			}
			break;
		case I::MetaSoundNode:
			for (const TSharedPtr<FJsonValue>& Value
				: Object->GetArrayField(TEXT("nodes")))
			{
				const TSharedPtr<FJsonObject> Node = Value->AsObject();
				EChangeIntent Intent = EChangeIntent::Update;
				if (!Node.IsValid()
					|| !TryIntent(Node->GetStringField(TEXT("intent")), Intent))
				{
					SetError(OutError, TEXT("audio_media_graph_node_invalid"));
					return false;
				}
				if (!AddEdit(
						TEXT("item:metasound_node:")
							+ Node->GetStringField(TEXT("node_guid")),
						EGraphCompoundElementKind::Item, Intent,
						ObjectWithout(*Node, {TEXT("intent")})))
				{
					return false;
				}
			}
			break;
		case I::MetaSoundLink:
			for (const TSharedPtr<FJsonValue>& Value
				: Object->GetArrayField(TEXT("links")))
			{
				const TSharedPtr<FJsonObject> Link = Value->AsObject();
				const TSharedPtr<FJsonObject> From = Link.IsValid()
					? Link->GetObjectField(TEXT("from")) : nullptr;
				const TSharedPtr<FJsonObject> To = Link.IsValid()
					? Link->GetObjectField(TEXT("to")) : nullptr;
				EChangeIntent Intent = EChangeIntent::Update;
				if (!From.IsValid() || !To.IsValid()
					|| !TryIntent(Link->GetStringField(TEXT("intent")), Intent))
				{
					SetError(OutError, TEXT("audio_media_graph_link_invalid"));
					return false;
				}
				const FString SemanticId = FString::Printf(
					TEXT("relation:metasound_link:%s:%s->%s:%s:%s"),
					*From->GetStringField(TEXT("node_guid")),
					*From->GetStringField(TEXT("vertex_guid")),
					*To->GetStringField(TEXT("node_guid")),
					*To->GetStringField(TEXT("vertex_guid")),
					*From->GetStringField(TEXT("data_type_ref")));
				const TArray<FUeAgentCanonicalIdentity> Dependencies = {
					MakeIdentity(TEXT("item:metasound_node:")
						+ From->GetStringField(TEXT("node_guid"))),
					MakeIdentity(TEXT("item:metasound_node:")
						+ To->GetStringField(TEXT("node_guid")))};
				if (!AddEdit(
						SemanticId, EGraphCompoundElementKind::Relation, Intent,
						ObjectWithout(*Link, {TEXT("intent")}), Dependencies))
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
					{MakeIdentity(TEXT("item:metasound_node:")
						+ Object->GetStringField(TEXT("node_guid"))) }))
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
			for (const TSharedPtr<FJsonValue>& Value
				: Object->GetArrayField(TEXT("nodes")))
			{
				const TSharedPtr<FJsonObject> Node = Value->AsObject();
				EChangeIntent Intent = EChangeIntent::Update;
				if (!Node.IsValid()
					|| !TryIntent(Node->GetStringField(TEXT("intent")), Intent))
				{
					SetError(OutError, TEXT("audio_media_graph_node_invalid"));
					return false;
				}
				if (!AddEdit(
						TEXT("item:sound_node:")
							+ Node->GetStringField(TEXT("object_path")),
						EGraphCompoundElementKind::Item, Intent,
						ObjectWithout(*Node, {TEXT("intent")})))
				{
					return false;
				}
			}
			break;
		case I::SoundCueRelation:
			for (const TSharedPtr<FJsonValue>& Value
				: Object->GetArrayField(TEXT("relations")))
			{
				const TSharedPtr<FJsonObject> Relation = Value->AsObject();
				EChangeIntent Intent = EChangeIntent::Update;
				if (!Relation.IsValid()
					|| !TryIntent(Relation->GetStringField(TEXT("intent")), Intent))
				{
					SetError(OutError, TEXT("audio_media_graph_relation_invalid"));
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
						{MakeIdentity(TEXT("item:sound_node:") + Parent),
						 MakeIdentity(TEXT("item:sound_node:") + Child)}))
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
					{MakeIdentity(TEXT("item:sound_node:") + Root)}))
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
			for (const TSharedPtr<FJsonValue>& Value
				: Object->GetArrayField(TEXT("nodes")))
			{
				const TSharedPtr<FJsonObject> Node = Value->AsObject();
				EChangeIntent Intent = EChangeIntent::Update;
				if (!Node.IsValid()
					|| !TryIntent(Node->GetStringField(TEXT("intent")), Intent))
				{
					SetError(OutError, TEXT("audio_media_graph_node_invalid"));
					return false;
				}
				const FString SemanticId = TEXT("item:texture_expression:")
					+ Node->GetStringField(TEXT("element_id"));
				if (!AddEdit(
						SemanticId, EGraphCompoundElementKind::Item, Intent,
						ObjectWithout(*Node, {TEXT("intent")})))
				{
					return false;
				}
			}
			break;
		case I::TextureGraphLink:
			for (const TSharedPtr<FJsonValue>& Value
				: Object->GetArrayField(TEXT("links")))
			{
				const TSharedPtr<FJsonObject> Link = Value->AsObject();
				const TSharedPtr<FJsonObject> From = Link.IsValid()
					? Link->GetObjectField(TEXT("from")) : nullptr;
				const TSharedPtr<FJsonObject> To = Link.IsValid()
					? Link->GetObjectField(TEXT("to")) : nullptr;
				EChangeIntent Intent = EChangeIntent::Update;
				if (!From.IsValid() || !To.IsValid()
					|| !TryIntent(Link->GetStringField(TEXT("intent")), Intent))
				{
					SetError(OutError, TEXT("audio_media_graph_link_invalid"));
					return false;
				}
				const FString FromId = TexturePinSemanticId(*From);
				const FString ToId = TexturePinSemanticId(*To);
				if (!AddEdit(
						TEXT("relation:texture_link:") + FromId + TEXT("->") + ToId,
						EGraphCompoundElementKind::Relation, Intent,
						ObjectWithout(*Link, {TEXT("intent")}),
						{MakeIdentity(FromId), MakeIdentity(ToId)}))
				{
					return false;
				}
			}
			break;
		case I::TextureGraphParameter:
			for (const TSharedPtr<FJsonValue>& Value
				: Object->GetArrayField(TEXT("parameters")))
			{
				const TSharedPtr<FJsonObject> Parameter = Value->AsObject();
				EChangeIntent Intent = EChangeIntent::Update;
				if (!Parameter.IsValid()
					|| !TryIntent(Parameter->GetStringField(TEXT("intent")), Intent))
				{
					SetError(OutError, TEXT("audio_media_graph_parameter_invalid"));
					return false;
				}
				const FString PinId = FString::Printf(
					TEXT("pin:texture:%s:input:%s"),
					*Parameter->GetStringField(TEXT("element_id")),
					*Parameter->GetStringField(TEXT("pin_name")));
				const FString SemanticId = FString::Printf(
					TEXT("input:texture_parameter:%s:%s"),
					*Parameter->GetStringField(TEXT("element_id")),
					*Parameter->GetStringField(TEXT("pin_name")));
				if (!AddEdit(
						SemanticId, EGraphCompoundElementKind::Input, Intent,
						ObjectWithout(*Parameter, {TEXT("intent")}),
						{MakeIdentity(PinId)}))
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
			SetError(OutError, TEXT("audio_media_graph_input_kind_invalid"),
				Profile.GetCapabilityId());
			return false;
		}

		OutPayload = FGraphCompoundCanonicalPayload(
			GetGraphCompoundCanonicalSchemaId(), MoveTemp(Edits));
		return !OutPayload.GetEdits().IsEmpty();
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
			SetError(OutError,
				TEXT("audio_media_graph_descriptor_identity_mismatch"),
				Profile.GetDescriptorId());
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
			SetError(OutError, CanonicalError.Code,
				CanonicalError.IdentityKey, CanonicalError.Detail);
			return false;
		}
		return OutDraft.IsSet();
	}
}

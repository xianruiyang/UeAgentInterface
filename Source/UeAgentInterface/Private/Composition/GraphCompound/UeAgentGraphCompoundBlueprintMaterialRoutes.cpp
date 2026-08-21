#include "Composition/GraphCompound/UeAgentGraphCompoundBlueprintMaterialRoutes.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Core/Contract/UeAgentContractSchema.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Core/Contract/UeAgentExecutionContextPolicy.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"

namespace UE::UAI::Composition::GraphCompound
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::PlanningPreflight;
	using namespace UE::UAI::StateSemantic;

	namespace
	{
		struct FRouteSpec final
		{
			const TCHAR* CapabilityId;
			const TCHAR* DescriptorId;
			const TCHAR* DomainId;
			const TCHAR* FamilyId;
			EGraphCompoundDomainRouteKind RouteKind;
			EGraphCompoundTypedInputKind InputKind;
			const TCHAR* TargetKind;
			TArray<FString> CompositionKinds;
		};

		const TArray<FRouteSpec>& RouteSpecs()
		{
			static const TArray<FRouteSpec> Values = {
				{TEXT("blueprint.asset.create"), TEXT("uai.operation.blueprint.asset.create"), TEXT("blueprint_umg_graph"), TEXT("asset_lifecycle"), EGraphCompoundDomainRouteKind::TypedOperation, EGraphCompoundTypedInputKind::BlueprintAssetCreate, TEXT("blueprint"), {}},
				{TEXT("blueprint.authoring.apply"), TEXT("uai.operation.blueprint.authoring.apply"), TEXT("blueprint_umg_graph"), TEXT("graph_mutation"), EGraphCompoundDomainRouteKind::HjsonImport, EGraphCompoundTypedInputKind::None, TEXT("blueprint"), {TEXT("ue.blueprint.asset")}},
				{TEXT("blueprint.component.events.discover"), TEXT("uai.query.blueprint.component.events.discover"), TEXT("blueprint_umg_graph"), TEXT("event_discovery"), EGraphCompoundDomainRouteKind::Query, EGraphCompoundTypedInputKind::None, TEXT("blueprint"), {}},
				{TEXT("umg.asset.create"), TEXT("uai.operation.umg.asset.create"), TEXT("blueprint_umg_graph"), TEXT("asset_lifecycle"), EGraphCompoundDomainRouteKind::TypedOperation, EGraphCompoundTypedInputKind::UmgAssetCreate, TEXT("widget_blueprint"), {}},
				{TEXT("umg.authoring.apply"), TEXT("uai.operation.umg.authoring.apply"), TEXT("blueprint_umg_graph"), TEXT("hierarchy_mutation"), EGraphCompoundDomainRouteKind::HjsonImport, EGraphCompoundTypedInputKind::None, TEXT("widget_blueprint"), {TEXT("ue.widget_blueprint.asset")}},
				{TEXT("umg.widget.events.discover"), TEXT("uai.query.umg.widget.events.discover"), TEXT("blueprint_umg_graph"), TEXT("event_discovery"), EGraphCompoundDomainRouteKind::Query, EGraphCompoundTypedInputKind::None, TEXT("widget_blueprint"), {}},
				{TEXT("material.asset.create"), TEXT("uai.operation.material.asset.create"), TEXT("material_niagara_graph"), TEXT("asset_lifecycle"), EGraphCompoundDomainRouteKind::TypedOperation, EGraphCompoundTypedInputKind::MaterialAssetCreate, TEXT("material"), {}},
				{TEXT("material.graph.apply"), TEXT("uai.operation.material.graph.apply"), TEXT("material_niagara_graph"), TEXT("material_graph_mutation"), EGraphCompoundDomainRouteKind::TypedOperation, EGraphCompoundTypedInputKind::GraphEdits, TEXT("material"), {}},
				{TEXT("material.function.graph.apply"), TEXT("uai.operation.material.function.graph.apply"), TEXT("material_niagara_graph"), TEXT("material_graph_mutation"), EGraphCompoundDomainRouteKind::TypedOperation, EGraphCompoundTypedInputKind::GraphEdits, TEXT("material_function"), {}},
				{TEXT("material_instance.asset.create"), TEXT("uai.operation.material_instance.asset.create"), TEXT("material_niagara_graph"), TEXT("asset_lifecycle"), EGraphCompoundDomainRouteKind::TypedOperation, EGraphCompoundTypedInputKind::MaterialInstanceAssetCreate, TEXT("material_instance"), {}},
				{TEXT("material_instance.parameters.apply"), TEXT("uai.operation.material_instance.parameters.apply"), TEXT("material_niagara_graph"), TEXT("material_instance_parameter_mutation"), EGraphCompoundDomainRouteKind::TypedOperation, EGraphCompoundTypedInputKind::MaterialInstanceParameters, TEXT("material_instance"), {}},
				{TEXT("niagara.emitter.asset.create"), TEXT("uai.operation.niagara.emitter.asset.create"), TEXT("material_niagara_graph"), TEXT("asset_lifecycle"), EGraphCompoundDomainRouteKind::TypedOperation, EGraphCompoundTypedInputKind::NiagaraEmitterAssetCreate, TEXT("niagara_emitter"), {}},
				{TEXT("niagara.system.asset.create"), TEXT("uai.operation.niagara.system.asset.create"), TEXT("material_niagara_graph"), TEXT("asset_lifecycle"), EGraphCompoundDomainRouteKind::TypedOperation, EGraphCompoundTypedInputKind::NiagaraSystemAssetCreate, TEXT("niagara_system"), {}},
				{TEXT("niagara.asset.duplicate"), TEXT("uai.operation.niagara.asset.duplicate"), TEXT("material_niagara_graph"), TEXT("asset_lifecycle"), EGraphCompoundDomainRouteKind::TypedOperation, EGraphCompoundTypedInputKind::NiagaraAssetDuplicate, TEXT("niagara_asset"), {}},
				{TEXT("niagara.stack.reorder"), TEXT("uai.operation.niagara.stack.reorder"), TEXT("material_niagara_graph"), TEXT("niagara_stack_mutation"), EGraphCompoundDomainRouteKind::TypedOperation, EGraphCompoundTypedInputKind::NiagaraStackReorder, TEXT("niagara_stack"), {}},
				{TEXT("niagara.stack.issue_fix"), TEXT("uai.operation.niagara.stack.issue_fix"), TEXT("material_niagara_graph"), TEXT("niagara_stack_mutation"), EGraphCompoundDomainRouteKind::TypedOperation, EGraphCompoundTypedInputKind::NiagaraStackIssueFix, TEXT("niagara_stack"), {}},
				{TEXT("niagara.compound.apply"), TEXT("uai.operation.niagara.compound.apply"), TEXT("material_niagara_graph"), TEXT("niagara_compound_input"), EGraphCompoundDomainRouteKind::TypedOperation, EGraphCompoundTypedInputKind::NiagaraCompoundInput, TEXT("niagara_compound"), {}},
				{TEXT("niagara.renderer_event_parameter.apply"), TEXT("uai.operation.niagara.renderer_event_parameter.apply"), TEXT("material_niagara_graph"), TEXT("niagara_renderer_event_parameter_mutation"), EGraphCompoundDomainRouteKind::TypedOperation, EGraphCompoundTypedInputKind::NiagaraRendererEventParameter, TEXT("niagara_renderer"), {}},
				{TEXT("niagara.compile_refresh_settle"), TEXT("uai.operation.niagara.compile_refresh_settle"), TEXT("material_niagara_graph"), TEXT("compile_refresh_settle"), EGraphCompoundDomainRouteKind::TypedOperation, EGraphCompoundTypedInputKind::NiagaraCompileRefreshSettle, TEXT("niagara_asset"), {}},
			};
			return Values;
		}

		FString CanonicalSchema(const TCHAR* Text)
		{
			TSharedPtr<FJsonObject> Object;
			FString Error;
			FString Canonical;
			if (!Json::TryParseObject(Text, Object, Error)
				|| !Json::TryCanonicalize(Object, Canonical, Error))
			{
				return FString();
			}
			return Canonical;
		}

		FString HjsonInputSchema(const TArray<FString>& CompositionKinds)
		{
			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("type"), TEXT("object"));
			Root->SetBoolField(TEXT("additionalProperties"), false);
			const TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
			auto StringSchema = []()
			{
				const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
				Result->SetStringField(TEXT("type"), TEXT("string"));
				Result->SetNumberField(TEXT("minLength"), 1);
				return Result;
			};
			const TSharedRef<FJsonObject> Composition = StringSchema();
			TArray<TSharedPtr<FJsonValue>> CompositionEnum;
			for (const FString& Value : CompositionKinds)
			{
				CompositionEnum.Add(MakeShared<FJsonValueString>(Value));
			}
			Composition->SetArrayField(TEXT("enum"), MoveTemp(CompositionEnum));
			Properties->SetObjectField(TEXT("composition_kind"), Composition);
			Properties->SetObjectField(TEXT("target_ref"), StringSchema());
			const TSharedRef<FJsonObject> SourceKind = StringSchema();
			SourceKind->SetArrayField(TEXT("enum"), {
				MakeShared<FJsonValueString>(TEXT("directory")),
				MakeShared<FJsonValueString>(TEXT("single_file")),
				MakeShared<FJsonValueString>(TEXT("inline"))});
			Properties->SetObjectField(TEXT("source_kind"), SourceKind);
			Properties->SetObjectField(TEXT("source_locator"), StringSchema());
			Properties->SetObjectField(TEXT("inline_hjson"), StringSchema());
			const TSharedRef<FJsonObject> Intent = StringSchema();
			Intent->SetArrayField(TEXT("enum"), {
				MakeShared<FJsonValueString>(TEXT("update")),
				MakeShared<FJsonValueString>(TEXT("replace"))});
			Properties->SetObjectField(TEXT("intent"), Intent);
			Properties->SetObjectField(TEXT("expected_revision"), StringSchema());
			const TSharedRef<FJsonObject> Boolean = MakeShared<FJsonObject>();
			Boolean->SetStringField(TEXT("type"), TEXT("boolean"));
			Properties->SetObjectField(TEXT("dry_run"), Boolean);
			Root->SetObjectField(TEXT("properties"), Properties);
			Root->SetArrayField(TEXT("required"), {
				MakeShared<FJsonValueString>(TEXT("composition_kind")),
				MakeShared<FJsonValueString>(TEXT("target_ref")),
				MakeShared<FJsonValueString>(TEXT("source_kind")),
				MakeShared<FJsonValueString>(TEXT("intent")),
				MakeShared<FJsonValueString>(TEXT("expected_revision")),
				MakeShared<FJsonValueString>(TEXT("dry_run"))});

			auto SourceVariant = [](const TCHAR* Kind, const TCHAR* RequiredField,
				const TCHAR* ForbiddenField)
			{
				const TSharedRef<FJsonObject> Variant = MakeShared<FJsonObject>();
				const TSharedRef<FJsonObject> VariantProperties =
					MakeShared<FJsonObject>();
				const TSharedRef<FJsonObject> KindSchema = MakeShared<FJsonObject>();
				KindSchema->SetStringField(TEXT("const"), Kind);
				VariantProperties->SetObjectField(TEXT("source_kind"), KindSchema);
				Variant->SetObjectField(TEXT("properties"), VariantProperties);
				Variant->SetArrayField(TEXT("required"), {
					MakeShared<FJsonValueString>(TEXT("source_kind")),
					MakeShared<FJsonValueString>(RequiredField)});
				const TSharedRef<FJsonObject> Not = MakeShared<FJsonObject>();
				Not->SetArrayField(TEXT("required"), {
					MakeShared<FJsonValueString>(ForbiddenField)});
				Variant->SetObjectField(TEXT("not"), Not);
				return MakeShared<FJsonValueObject>(Variant);
			};
			Root->SetArrayField(TEXT("oneOf"), {
				SourceVariant(TEXT("directory"), TEXT("source_locator"), TEXT("inline_hjson")),
				SourceVariant(TEXT("single_file"), TEXT("source_locator"), TEXT("inline_hjson")),
				SourceVariant(TEXT("inline"), TEXT("inline_hjson"), TEXT("source_locator"))});
			FString Canonical;
			FString Error;
			return Json::TryCanonicalize(Root, Canonical, Error)
				? Canonical : FString();
		}

		FString InputSchema(const FRouteSpec& Spec)
		{
			if (Spec.RouteKind == EGraphCompoundDomainRouteKind::Query)
			{
				if (FString(Spec.CapabilityId)
					== TEXT("blueprint.component.events.discover"))
				{
					return CanonicalSchema(TEXT(
						"{\"additionalProperties\":false,\"properties\":{" 
						"\"asset_path\":{\"minLength\":1,\"type\":\"string\"},"
						"\"compile_before_query\":{\"type\":\"boolean\"},"
						"\"component_name\":{\"minLength\":1,\"type\":\"string\"},"
						"\"include_hidden\":{\"type\":\"boolean\"},"
						"\"limit\":{\"maximum\":1000,\"minimum\":0,\"type\":\"integer\"},"
						"\"only_unbound\":{\"type\":\"boolean\"},"
						"\"query\":{\"type\":\"string\"}},"
						"\"required\":[\"asset_path\",\"component_name\"],"
						"\"type\":\"object\"}"));
				}
				return CanonicalSchema(TEXT(
					"{\"additionalProperties\":false,\"properties\":{"
					"\"owner_item_ref\":{\"minLength\":1,\"type\":\"string\"},"
					"\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},"
					"\"required\":[\"owner_item_ref\",\"target_ref\"],"
					"\"type\":\"object\"}"));
			}
			if (Spec.RouteKind == EGraphCompoundDomainRouteKind::HjsonImport)
			{
				return HjsonInputSchema(Spec.CompositionKinds);
			}
			switch (Spec.InputKind)
			{
			case EGraphCompoundTypedInputKind::BlueprintAssetCreate:
				return CanonicalSchema(TEXT("{\"additionalProperties\":false,\"properties\":{\"asset_name\":{\"minLength\":1,\"type\":\"string\"},\"package_path\":{\"minLength\":1,\"type\":\"string\"},\"parent_class_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"asset_name\",\"package_path\",\"parent_class_ref\"],\"type\":\"object\"}"));
			case EGraphCompoundTypedInputKind::UmgAssetCreate:
				return CanonicalSchema(TEXT("{\"additionalProperties\":false,\"properties\":{\"asset_name\":{\"minLength\":1,\"type\":\"string\"},\"package_path\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"asset_name\",\"package_path\"],\"type\":\"object\"}"));
			case EGraphCompoundTypedInputKind::MaterialAssetCreate:
				return CanonicalSchema(TEXT("{\"additionalProperties\":false,\"properties\":{\"asset_name\":{\"minLength\":1,\"type\":\"string\"},\"material_kind\":{\"enum\":[\"material\",\"material_function\"],\"type\":\"string\"},\"package_path\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"asset_name\",\"package_path\"],\"type\":\"object\"}"));
			case EGraphCompoundTypedInputKind::MaterialInstanceAssetCreate:
				return CanonicalSchema(TEXT("{\"additionalProperties\":false,\"properties\":{\"asset_name\":{\"minLength\":1,\"type\":\"string\"},\"package_path\":{\"minLength\":1,\"type\":\"string\"},\"parent_material_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"asset_name\",\"package_path\",\"parent_material_ref\"],\"type\":\"object\"}"));
			case EGraphCompoundTypedInputKind::NiagaraEmitterAssetCreate:
			case EGraphCompoundTypedInputKind::NiagaraSystemAssetCreate:
				return CanonicalSchema(TEXT("{\"additionalProperties\":false,\"properties\":{\"asset_name\":{\"minLength\":1,\"type\":\"string\"},\"package_path\":{\"minLength\":1,\"type\":\"string\"},\"source_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"asset_name\",\"package_path\",\"source_ref\"],\"type\":\"object\"}"));
			case EGraphCompoundTypedInputKind::GraphEdits:
				return CanonicalSchema(TEXT("{\"additionalProperties\":false,\"properties\":{\"edits\":{\"items\":{\"additionalProperties\":false,\"properties\":{\"depends_on\":{\"items\":{\"minLength\":1,\"type\":\"string\"},\"type\":\"array\",\"uniqueItems\":true},\"element_kind\":{\"enum\":[\"target\",\"structure\",\"item\",\"pin\",\"relation\",\"property\",\"input\",\"hierarchy\",\"order\",\"binding\",\"dependency\"],\"type\":\"string\"},\"identity\":{\"minLength\":1,\"type\":\"string\"},\"intent\":{\"enum\":[\"create\",\"update\",\"replace\",\"delete\"],\"type\":\"string\"},\"value\":{}},\"required\":[\"identity\",\"element_kind\",\"intent\"],\"type\":\"object\"},\"minItems\":1,\"type\":\"array\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"edits\",\"target_ref\"],\"type\":\"object\"}"));
			case EGraphCompoundTypedInputKind::MaterialInstanceParameters:
				return CanonicalSchema(TEXT("{\"additionalProperties\":false,\"properties\":{\"parameters\":{\"items\":{\"additionalProperties\":false,\"properties\":{\"association\":{\"minLength\":1,\"type\":\"string\"},\"index\":{\"type\":\"integer\"},\"name\":{\"minLength\":1,\"type\":\"string\"},\"type\":{\"enum\":[\"scalar\",\"vector\",\"texture\",\"static_switch\",\"static_component_mask\"],\"type\":\"string\"},\"value\":{}},\"required\":[\"name\",\"association\",\"index\",\"type\",\"value\"],\"type\":\"object\"},\"minItems\":1,\"type\":\"array\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"parameters\",\"target_ref\"],\"type\":\"object\"}"));
			case EGraphCompoundTypedInputKind::NiagaraAssetDuplicate:
				return CanonicalSchema(TEXT("{\"additionalProperties\":false,\"properties\":{\"source_ref\":{\"minLength\":1,\"type\":\"string\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"source_ref\",\"target_ref\"],\"type\":\"object\"}"));
			case EGraphCompoundTypedInputKind::NiagaraStackReorder:
				return CanonicalSchema(TEXT("{\"additionalProperties\":false,\"properties\":{\"emitter_handle_id\":{\"minLength\":1,\"type\":\"string\"},\"item_ref\":{\"minLength\":1,\"type\":\"string\"},\"predecessor_ref\":{\"type\":\"string\"},\"stage_ref\":{\"minLength\":1,\"type\":\"string\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"emitter_handle_id\",\"item_ref\",\"predecessor_ref\",\"stage_ref\",\"target_ref\"],\"type\":\"object\"}"));
			case EGraphCompoundTypedInputKind::NiagaraStackIssueFix:
				return CanonicalSchema(TEXT("{\"additionalProperties\":false,\"properties\":{\"emitter_handle_id\":{\"minLength\":1,\"type\":\"string\"},\"fix_id\":{\"minLength\":1,\"type\":\"string\"},\"issue_id\":{\"minLength\":1,\"type\":\"string\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"emitter_handle_id\",\"fix_id\",\"issue_id\",\"target_ref\"],\"type\":\"object\"}"));
			case EGraphCompoundTypedInputKind::NiagaraCompoundInput:
				return CanonicalSchema(TEXT("{\"additionalProperties\":false,\"properties\":{\"emitter_handle_id\":{\"minLength\":1,\"type\":\"string\"},\"input_name\":{\"minLength\":1,\"type\":\"string\"},\"module_ref\":{\"minLength\":1,\"type\":\"string\"},\"stage_ref\":{\"minLength\":1,\"type\":\"string\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"},\"value\":{}},\"required\":[\"emitter_handle_id\",\"input_name\",\"module_ref\",\"stage_ref\",\"target_ref\",\"value\"],\"type\":\"object\"}"));
			case EGraphCompoundTypedInputKind::NiagaraRendererEventParameter:
				return CanonicalSchema(TEXT("{\"additionalProperties\":false,\"properties\":{\"emitter_handle_id\":{\"minLength\":1,\"type\":\"string\"},\"parameter_name\":{\"minLength\":1,\"type\":\"string\"},\"renderer_ref\":{\"minLength\":1,\"type\":\"string\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"},\"type_ref\":{\"minLength\":1,\"type\":\"string\"},\"value\":{}},\"required\":[\"emitter_handle_id\",\"parameter_name\",\"renderer_ref\",\"target_ref\",\"type_ref\",\"value\"],\"type\":\"object\"}"));
			case EGraphCompoundTypedInputKind::NiagaraCompileRefreshSettle:
				return CanonicalSchema(TEXT("{\"additionalProperties\":false,\"properties\":{\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"target_ref\"],\"type\":\"object\"}"));
			default:
				return FString();
			}
		}

		FString ResultSchema(const FRouteSpec& Spec)
		{
			if (Spec.RouteKind == EGraphCompoundDomainRouteKind::Query)
			{
				if (FString(Spec.CapabilityId)
					== TEXT("blueprint.component.events.discover"))
				{
					return CanonicalSchema(TEXT(
						"{\"additionalProperties\":false,\"properties\":{" 
						"\"asset_path\":{\"type\":\"string\"},"
						"\"can_author_component_bound_event\":{\"type\":\"boolean\"},"
						"\"compiled_before_query\":{\"type\":\"boolean\"},"
						"\"component_name\":{\"type\":\"string\"},"
						"\"component_property_found\":{\"type\":\"boolean\"},"
						"\"events\":{\"items\":{\"additionalProperties\":false,\"properties\":{" 
						"\"already_bound\":{\"type\":\"boolean\"},\"category\":{\"type\":\"string\"},"
						"\"display_name\":{\"type\":\"string\"},\"event_ref\":{\"type\":\"string\"},"
						"\"hidden\":{\"type\":\"boolean\"},\"native_field_name\":{\"type\":\"string\"},"
						"\"signature_ref\":{\"type\":\"string\"}},"
						"\"required\":[\"event_ref\",\"native_field_name\",\"signature_ref\",\"display_name\",\"category\",\"hidden\",\"already_bound\"],\"type\":\"object\"},\"type\":\"array\"},"
						"\"matched_event_count\":{\"type\":\"integer\"},\"returned_event_count\":{\"type\":\"integer\"},"
						"\"target_class\":{\"type\":\"string\"},\"total_event_count\":{\"type\":\"integer\"},"
						"\"truncated\":{\"type\":\"boolean\"}},"
						"\"required\":[\"asset_path\",\"component_name\",\"target_class\",\"component_property_found\",\"can_author_component_bound_event\",\"compiled_before_query\",\"total_event_count\",\"matched_event_count\",\"returned_event_count\",\"truncated\",\"events\"],\"type\":\"object\"}"));
				}
				return CanonicalSchema(TEXT("{\"additionalProperties\":false,\"properties\":{\"events\":{\"items\":{\"additionalProperties\":false,\"properties\":{\"event_ref\":{\"type\":\"string\"},\"native_field_name\":{\"type\":\"string\"},\"signature_ref\":{\"type\":\"string\"}},\"required\":[\"event_ref\",\"native_field_name\",\"signature_ref\"],\"type\":\"object\"},\"type\":\"array\"}},\"required\":[\"events\"],\"type\":\"object\"}"));
			}
			if (Spec.RouteKind == EGraphCompoundDomainRouteKind::HjsonImport)
			{
				return CanonicalSchema(TEXT(
					"{\"additionalProperties\":false,\"properties\":{"
					"\"import\":{\"additionalProperties\":false,\"properties\":{"
					"\"compile_settled\":{\"type\":\"boolean\"},"
					"\"composition_kind\":{\"minLength\":1,\"type\":\"string\"},"
					"\"dry_run\":{\"type\":\"boolean\"},"
					"\"mutation_applied\":{\"type\":\"boolean\"},"
					"\"operation_id\":{\"minLength\":1,\"type\":\"string\"},"
					"\"package_saved\":{\"type\":\"boolean\"},"
					"\"persisted_revision\":{\"minLength\":1,\"type\":\"string\"},"
					"\"plan_fingerprint\":{\"minLength\":1,\"type\":\"string\"},"
					"\"pre_revision\":{\"minLength\":1,\"type\":\"string\"},"
					"\"target\":{\"minLength\":1,\"type\":\"string\"},"
					"\"unloaded_reloaded\":{\"type\":\"boolean\"},"
					"\"verification_status\":{\"const\":\"verified\"},"
					"\"workspace_fingerprint\":{\"minLength\":1,\"type\":\"string\"}},"
					"\"required\":[\"compile_settled\",\"composition_kind\","
					"\"dry_run\",\"mutation_applied\",\"operation_id\","
					"\"package_saved\",\"persisted_revision\",\"plan_fingerprint\","
					"\"pre_revision\",\"target\",\"unloaded_reloaded\","
					"\"verification_status\",\"workspace_fingerprint\"],"
					"\"type\":\"object\"}},\"required\":[\"import\"],"
					"\"type\":\"object\"}"));
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
			const TSharedRef<FJsonObject> Policy = MakeShared<FJsonObject>();
			Policy->SetArrayField(TEXT("allowed_state_planes"), {
				MakeShared<FJsonValueString>(TEXT("persisted"))});
			Policy->SetArrayField(TEXT("required_facets"), {});
			Policy->SetStringField(TEXT("resolution_policy"), TEXT("explicit_only"));
			Policy->SetStringField(
				TEXT("transient_access_policy"),
				Spec.RouteKind == EGraphCompoundDomainRouteKind::Query
					? TEXT("read_only") : TEXT("temporary_mutation_allowed"));
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("capability_id"), Spec.CapabilityId);
			Object->SetStringField(TEXT("descriptor_id"), Spec.DescriptorId);
			Object->SetStringField(TEXT("domain_id"), Spec.DomainId);
			Object->SetObjectField(TEXT("execution_context_policy"), Policy);
			Object->SetObjectField(TEXT("input_schema"), InputObject);
			Object->SetStringField(
				TEXT("operation_kind"),
				Spec.RouteKind == EGraphCompoundDomainRouteKind::Query
					? TEXT("query") : TEXT("operation"));
			if (Spec.RouteKind == EGraphCompoundDomainRouteKind::Query)
			{
				const TSharedRef<FJsonObject> Query = MakeShared<FJsonObject>();
				Query->SetStringField(TEXT("materialization_policy"), TEXT("load_readonly"));
				Query->SetStringField(TEXT("provider_binding"),
					FString(TEXT("graph_compound.")) + Spec.CapabilityId);
				Query->SetStringField(TEXT("query_kind"), TEXT("capability"));
				Query->SetStringField(TEXT("side_effect_policy"), TEXT("no_persistent_mutation"));
				Object->SetObjectField(TEXT("query"), Query);
			}
			Object->SetObjectField(TEXT("result_schema"), ResultObject);
			Object->SetField(TEXT("version"),
				MakeShared<FJsonValueNumberString>(TEXT("1")));
			return Object;
		}

		FString StableSuffix(FString Value)
		{
			Value.ReplaceInline(TEXT("."), TEXT("_"));
			return Value;
		}

		struct FRouteBundle final
		{
			TArray<FGraphCompoundDomainRouteProfile> Profiles;
			TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
			FString Error;
		};

		FRouteBundle BuildBundle()
		{
			FRouteBundle Bundle;
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
				const FUeAgentDescriptorIdentity Identity(
					Spec.DescriptorId, 1, Json::Sha256Utf8(CanonicalDescriptor));
				const bool bQuery =
					Spec.RouteKind == EGraphCompoundDomainRouteKind::Query;
				TOptional<FUeAgentQueryBinding> QueryBinding;
				if (bQuery)
				{
					QueryBinding.Emplace(
						FString(TEXT("graph_compound.")) + Spec.CapabilityId,
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
					Identity, Spec.CapabilityId, Spec.DomainId,
					bQuery ? EUeAgentDescriptorOperationKind::Query
						: EUeAgentDescriptorOperationKind::Operation,
					Policy, FUeAgentContractValue(Input),
					FUeAgentContractValue(Result), MoveTemp(QueryBinding));

				const FString Suffix = StableSuffix(Spec.CapabilityId);
				const bool bHjson =
					Spec.RouteKind == EGraphCompoundDomainRouteKind::HjsonImport;
				Bundle.Profiles.Emplace(
					Spec.CapabilityId, Spec.DescriptorId, Spec.DomainId,
					Spec.FamilyId, Spec.RouteKind, Spec.InputKind,
					Spec.TargetKind, Spec.CompositionKinds,
					FGraphCompoundExecutionContract(
						Identity,
						bQuery ? EGraphCompoundExecutionLane::Query
							: EGraphCompoundExecutionLane::Operation,
						Spec.FamilyId,
						FString(TEXT("graph_compound.")) + Spec.CapabilityId,
						FString(TEXT("graph_compound.")) + Suffix + TEXT(".request.v1"),
						FString(TEXT("graph_compound.")) + Suffix + TEXT(".result.v1"),
						bQuery ? FString() : (bHjson
							? TEXT("hjson_workspace_change_set")
							: TEXT("graph_compound_identity_change")),
						bQuery ? FString() : (bHjson
							? TEXT("c5.semantic_workspace_import")
							: FString(TEXT("native.")) + Spec.CapabilityId),
						FString(TEXT("readback.")) + Spec.CapabilityId,
						bQuery ? FString() : (bHjson
							? TEXT("compile_settle_save_reload")
							: TEXT("domain_compile_refresh_settle")),
						bQuery
							? TEXT("independent_native_event_discovery")
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
			const FGraphCompoundDomainRouteProfile& Profile,
			const FUeAgentContractValue& Input,
			FUeAgentResourceTracker& Resources,
			TSharedPtr<FJsonObject>& OutObject,
			FGraphCompoundRouteError& OutError)
		{
			OutObject.Reset();
			OutError = {};
			const FRouteBundle& Bundle = RouteBundle();
			if (!Profile.IsValid() || !Bundle.Registry.IsValid())
			{
				SetError(OutError, TEXT("graph_compound_route_unavailable"),
					Profile.GetCapabilityId(), Bundle.Error);
				return false;
			}
			const FUeAgentOperationDescriptor* Descriptor =
				Bundle.Registry->FindByDescriptorId(Profile.GetDescriptorId());
			if (!Descriptor)
			{
				SetError(OutError, TEXT("graph_compound_descriptor_missing"),
					Profile.GetDescriptorId());
				return false;
			}
			const FUeAgentContractSchema* Schema =
				Bundle.Registry->FindCompiledInputSchemaExact(
					Descriptor->GetIdentity());
			if (!Schema)
			{
				SetError(OutError, TEXT("graph_compound_schema_missing"),
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
							TEXT("graph_compound.gc015.schema"),
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
						? TEXT("graph_compound_input_invalid")
						: SchemaError.Code,
					SchemaError.InstancePath,
					SchemaError.Detail);
				return false;
			}
			FString ParseError;
			if (!Json::TryParseObject(
					Canonical->GetJsonValue(), OutObject, ParseError))
			{
				SetError(OutError, TEXT("graph_compound_input_parse_failed"),
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

		bool TryElementKind(
			const FString& Text,
			EGraphCompoundElementKind& OutKind)
		{
			static const TMap<FString, EGraphCompoundElementKind> Values = {
				{TEXT("target"), EGraphCompoundElementKind::Target},
				{TEXT("structure"), EGraphCompoundElementKind::Structure},
				{TEXT("item"), EGraphCompoundElementKind::Item},
				{TEXT("pin"), EGraphCompoundElementKind::Pin},
				{TEXT("relation"), EGraphCompoundElementKind::Relation},
				{TEXT("property"), EGraphCompoundElementKind::Property},
				{TEXT("input"), EGraphCompoundElementKind::Input},
				{TEXT("hierarchy"), EGraphCompoundElementKind::Hierarchy},
				{TEXT("order"), EGraphCompoundElementKind::Order},
				{TEXT("binding"), EGraphCompoundElementKind::Binding},
				{TEXT("dependency"), EGraphCompoundElementKind::Dependency},
			};
			const EGraphCompoundElementKind* Found = Values.Find(Text);
			if (!Found) { return false; }
			OutKind = *Found;
			return true;
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
			if (!CanonicalJson::TryFromJsonValue(
					JsonValue, Resources, OutValue, Error))
			{
				SetError(OutError, TEXT("graph_compound_value_invalid"),
					Subject, Error);
				return false;
			}
			return true;
		}

		bool TryCanonicalString(
			const FString& Text,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FGraphCompoundRouteError& OutError,
			const FString& Subject)
		{
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalValue::TryString(
					Text, Resources, OutValue, Error))
			{
				SetError(OutError,
					Error.Code.IsEmpty()
						? TEXT("graph_compound_value_invalid") : Error.Code,
					Subject, Error.Path);
				return false;
			}
			return true;
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

		bool IsNiagaraMutationKind(
			const EGraphCompoundTypedInputKind InputKind)
		{
			return InputKind == EGraphCompoundTypedInputKind::NiagaraStackReorder
				|| InputKind == EGraphCompoundTypedInputKind::NiagaraStackIssueFix
				|| InputKind == EGraphCompoundTypedInputKind::NiagaraCompoundInput
				|| InputKind
					== EGraphCompoundTypedInputKind::NiagaraRendererEventParameter;
		}

		bool TryNormalizeGuidText(
			FString Text,
			FString& OutText)
		{
			Text.TrimStartAndEndInline();
			FGuid Guid;
			if (!FGuid::Parse(Text, Guid) || !Guid.IsValid())
			{
				OutText.Reset();
				return false;
			}
			OutText = Guid.ToString(EGuidFormats::DigitsWithHyphensLower);
			return true;
		}

		bool TryNormalizeModuleRef(
			FString Text,
			const bool bAllowEmpty,
			FString& OutText)
		{
			Text.TrimStartAndEndInline();
			if (Text.IsEmpty())
			{
				OutText.Reset();
				return bAllowEmpty;
			}
			if (!Text.StartsWith(TEXT("module:"), ESearchCase::IgnoreCase))
			{
				OutText.Reset();
				return false;
			}
			FString GuidText;
			if (!TryNormalizeGuidText(Text.RightChop(7), GuidText))
			{
				OutText.Reset();
				return false;
			}
			OutText = TEXT("module:") + GuidText;
			return true;
		}

		bool TryBuildNiagaraMutationSelector(
			const FGraphCompoundDomainRouteProfile& Profile,
			const FJsonObject& Object,
			FGraphCompoundNiagaraMutationSelector& OutSelector,
			FGraphCompoundRouteError& OutError)
		{
			OutSelector = FGraphCompoundNiagaraMutationSelector();
			const EGraphCompoundTypedInputKind InputKind =
				Profile.GetTypedInputKind();
			if (!IsNiagaraMutationKind(InputKind))
			{
				SetError(OutError,
					TEXT("graph_compound_niagara_selector_kind_invalid"),
					Profile.GetCapabilityId());
				return false;
			}

			FString EmitterHandleId;
			if (!TryNormalizeGuidText(
					Object.GetStringField(TEXT("emitter_handle_id")),
					EmitterHandleId))
			{
				SetError(OutError,
					TEXT("graph_compound_niagara_emitter_handle_invalid"),
					Profile.GetCapabilityId(), TEXT("/emitter_handle_id"));
				return false;
			}

			auto OptionalString = [&Object](const TCHAR* Field)
			{
				FString Result;
				Object.TryGetStringField(Field, Result);
				Result.TrimStartAndEndInline();
				return Result;
			};
			FString ItemRef = OptionalString(TEXT("item_ref"));
			FString PredecessorRef = OptionalString(TEXT("predecessor_ref"));
			FString ModuleRef = OptionalString(TEXT("module_ref"));
			if ((InputKind == EGraphCompoundTypedInputKind::NiagaraStackReorder
					&& (!TryNormalizeModuleRef(ItemRef, false, ItemRef)
						|| !TryNormalizeModuleRef(
							PredecessorRef, true, PredecessorRef)))
				|| (InputKind == EGraphCompoundTypedInputKind::NiagaraCompoundInput
					&& !TryNormalizeModuleRef(ModuleRef, false, ModuleRef)))
			{
				SetError(OutError,
					TEXT("graph_compound_niagara_module_ref_invalid"),
					Profile.GetCapabilityId());
				return false;
			}

			FString RendererRef = OptionalString(TEXT("renderer_ref"));
			if (InputKind
					== EGraphCompoundTypedInputKind::NiagaraRendererEventParameter
				&& (!RendererRef.StartsWith(
						TEXT("renderer:"), ESearchCase::IgnoreCase)
					|| RendererRef.Len() <= 9))
			{
				SetError(OutError,
					TEXT("graph_compound_niagara_renderer_ref_invalid"),
					Profile.GetCapabilityId(), TEXT("/renderer_ref"));
				return false;
			}
			if (!RendererRef.IsEmpty())
			{
				RendererRef = TEXT("renderer:") + RendererRef.RightChop(9);
			}

			FString ParameterName = OptionalString(TEXT("parameter_name"));
			FString TypeRef = OptionalString(TEXT("type_ref"));
			ParameterName.ToLowerInline();
			TypeRef.ToLowerInline();
			OutSelector = FGraphCompoundNiagaraMutationSelector(
				InputKind,
				Object.GetStringField(TEXT("target_ref")),
				MoveTemp(EmitterHandleId),
				OptionalString(TEXT("stage_ref")),
				MoveTemp(ItemRef),
				MoveTemp(PredecessorRef),
				OptionalString(TEXT("issue_id")),
				OptionalString(TEXT("fix_id")),
				MoveTemp(ModuleRef),
				OptionalString(TEXT("input_name")),
				MoveTemp(RendererRef),
				MoveTemp(ParameterName),
				MoveTemp(TypeRef));
			if (!OutSelector.IsValid())
			{
				SetError(OutError,
					TEXT("graph_compound_niagara_selector_invalid"),
					Profile.GetCapabilityId());
				return false;
			}
			return true;
		}
	}

	FGraphCompoundDomainRouteProfile::FGraphCompoundDomainRouteProfile(
		FString InCapabilityId,
		FString InDescriptorId,
		FString InDomainId,
		FString InFamilyId,
		const EGraphCompoundDomainRouteKind InRouteKind,
		const EGraphCompoundTypedInputKind InTypedInputKind,
		FString InTargetKind,
		TArray<FString> InAllowedCompositionKinds,
		FGraphCompoundExecutionContract InExecutionContract)
		: CapabilityId(MoveTemp(InCapabilityId))
		, DescriptorId(MoveTemp(InDescriptorId))
		, DomainId(MoveTemp(InDomainId))
		, FamilyId(MoveTemp(InFamilyId))
		, RouteKind(InRouteKind)
		, TypedInputKind(InTypedInputKind)
		, TargetKind(MoveTemp(InTargetKind))
		, AllowedCompositionKinds(MoveTemp(InAllowedCompositionKinds))
		, ExecutionContract(MoveTemp(InExecutionContract))
	{
		for (FString* Value : {
			&CapabilityId, &DescriptorId, &DomainId, &FamilyId, &TargetKind})
		{
			Value->TrimStartAndEndInline();
		}
		for (FString& Value : AllowedCompositionKinds)
		{
			Value.TrimStartAndEndInline();
		}
		const bool bKindValid =
			(RouteKind == EGraphCompoundDomainRouteKind::Query
				&& TypedInputKind == EGraphCompoundTypedInputKind::None
				&& AllowedCompositionKinds.IsEmpty())
			|| (RouteKind == EGraphCompoundDomainRouteKind::TypedOperation
				&& TypedInputKind != EGraphCompoundTypedInputKind::None
				&& AllowedCompositionKinds.IsEmpty())
			|| (RouteKind == EGraphCompoundDomainRouteKind::HjsonImport
				&& TypedInputKind == EGraphCompoundTypedInputKind::None
				&& !AllowedCompositionKinds.IsEmpty());
		bValid = !CapabilityId.IsEmpty() && !DescriptorId.IsEmpty()
			&& !DomainId.IsEmpty() && !FamilyId.IsEmpty()
			&& !TargetKind.IsEmpty() && bKindValid
			&& ExecutionContract.IsValid();
	}

	const FString& FGraphCompoundDomainRouteProfile::GetCapabilityId() const { return CapabilityId; }
	const FString& FGraphCompoundDomainRouteProfile::GetDescriptorId() const { return DescriptorId; }
	const FString& FGraphCompoundDomainRouteProfile::GetDomainId() const { return DomainId; }
	const FString& FGraphCompoundDomainRouteProfile::GetFamilyId() const { return FamilyId; }
	EGraphCompoundDomainRouteKind FGraphCompoundDomainRouteProfile::GetRouteKind() const { return RouteKind; }
	EGraphCompoundTypedInputKind FGraphCompoundDomainRouteProfile::GetTypedInputKind() const { return TypedInputKind; }
	const FString& FGraphCompoundDomainRouteProfile::GetTargetKind() const { return TargetKind; }
	const TArray<FString>& FGraphCompoundDomainRouteProfile::GetAllowedCompositionKinds() const { return AllowedCompositionKinds; }
	const FGraphCompoundExecutionContract& FGraphCompoundDomainRouteProfile::GetExecutionContract() const { return ExecutionContract; }
	bool FGraphCompoundDomainRouteProfile::IsValid() const { return bValid; }

	const TArray<FGraphCompoundDomainRouteProfile>&
	GetBlueprintMaterialGraphRouteProfiles()
	{
		return RouteBundle().Profiles;
	}

	const FGraphCompoundDomainRouteProfile*
	FindBlueprintMaterialGraphRouteProfile(const FString& CapabilityId)
	{
		for (const FGraphCompoundDomainRouteProfile& Profile
			: RouteBundle().Profiles)
		{
			if (Profile.GetCapabilityId() == CapabilityId)
			{
				return &Profile;
			}
		}
		return nullptr;
	}

	bool TryCreateBlueprintMaterialGraphDescriptorRegistry(
		TSharedPtr<const FUeAgentDescriptorRegistry>& OutRegistry,
		FString& OutError)
	{
		OutRegistry = RouteBundle().Registry;
		OutError = RouteBundle().Error;
		return OutRegistry.IsValid() && OutError.IsEmpty();
	}

	FGraphCompoundEventQueryRequest::FGraphCompoundEventQueryRequest(
		FString InTargetRef,
		FString InOwnerItemRef)
		: FGraphCompoundEventQueryRequest(
			MoveTemp(InTargetRef), MoveTemp(InOwnerItemRef), FString(),
			200, false, false, true)
	{
	}

	FGraphCompoundEventQueryRequest::FGraphCompoundEventQueryRequest(
		FString InTargetRef,
		FString InOwnerItemRef,
		FString InQuery,
		const int32 InLimit,
		const bool bInOnlyUnbound,
		const bool bInIncludeHidden,
		const bool bInCompileBeforeQuery)
		: TargetRef(MoveTemp(InTargetRef))
		, OwnerItemRef(MoveTemp(InOwnerItemRef))
		, Query(MoveTemp(InQuery))
		, Limit(InLimit)
		, bOnlyUnbound(bInOnlyUnbound)
		, bIncludeHidden(bInIncludeHidden)
		, bCompileBeforeQuery(bInCompileBeforeQuery)
	{
		TargetRef.TrimStartAndEndInline();
		OwnerItemRef.TrimStartAndEndInline();
		Query.TrimStartAndEndInline();
		bValid = !TargetRef.IsEmpty() && !OwnerItemRef.IsEmpty()
			&& Limit >= 0 && Limit <= 1000;
	}

	const FString& FGraphCompoundEventQueryRequest::GetTargetRef() const { return TargetRef; }
	const FString& FGraphCompoundEventQueryRequest::GetOwnerItemRef() const { return OwnerItemRef; }
	const FString& FGraphCompoundEventQueryRequest::GetQuery() const { return Query; }
	int32 FGraphCompoundEventQueryRequest::GetLimit() const { return Limit; }
	bool FGraphCompoundEventQueryRequest::IsOnlyUnbound() const { return bOnlyUnbound; }
	bool FGraphCompoundEventQueryRequest::IsIncludeHidden() const { return bIncludeHidden; }
	bool FGraphCompoundEventQueryRequest::IsCompileBeforeQuery() const { return bCompileBeforeQuery; }
	bool FGraphCompoundEventQueryRequest::IsValid() const { return bValid; }

	bool TryDecodeGraphCompoundEventQueryRequest(
		const FGraphCompoundDomainRouteProfile& Profile,
		const FUeAgentContractValue& Input,
		FUeAgentResourceTracker& Resources,
		FGraphCompoundEventQueryRequest& OutRequest,
		FGraphCompoundRouteError& OutError)
	{
		OutRequest = FGraphCompoundEventQueryRequest();
		OutError = {};
		if (Profile.GetRouteKind() != EGraphCompoundDomainRouteKind::Query)
		{
			SetError(OutError, TEXT("graph_compound_route_kind_mismatch"),
				Profile.GetCapabilityId(), TEXT("query"));
			return false;
		}
		TSharedPtr<FJsonObject> Object;
		if (!TryValidatedObject(Profile, Input, Resources, Object, OutError))
		{
			return false;
		}
		if (Profile.GetCapabilityId()
			== TEXT("blueprint.component.events.discover"))
		{
			FString Query;
			Object->TryGetStringField(TEXT("query"), Query);
			double Limit = 200.0;
			Object->TryGetNumberField(TEXT("limit"), Limit);
			bool bOnlyUnbound = false;
			Object->TryGetBoolField(TEXT("only_unbound"), bOnlyUnbound);
			bool bIncludeHidden = false;
			Object->TryGetBoolField(TEXT("include_hidden"), bIncludeHidden);
			bool bCompileBeforeQuery = true;
			Object->TryGetBoolField(
				TEXT("compile_before_query"), bCompileBeforeQuery);
			const FString ComponentName =
				Object->GetStringField(TEXT("component_name"));
			OutRequest = FGraphCompoundEventQueryRequest(
				Object->GetStringField(TEXT("asset_path")),
				TEXT("component:") + ComponentName,
				MoveTemp(Query), static_cast<int32>(Limit), bOnlyUnbound,
				bIncludeHidden, bCompileBeforeQuery);
		}
		else
		{
			OutRequest = FGraphCompoundEventQueryRequest(
				Object->GetStringField(TEXT("target_ref")),
				Object->GetStringField(TEXT("owner_item_ref")));
		}
		return OutRequest.IsValid();
	}

	FGraphCompoundAssetCreateRequest::FGraphCompoundAssetCreateRequest(
		const EGraphCompoundTypedInputKind InInputKind,
		FString InAssetName,
		FString InPackagePath,
		FString InTargetRef,
		FString InMaterialKind,
		FString InParentClassRef,
		FString InParentMaterialRef,
		FString InSourceRef)
		: InputKind(InInputKind)
		, AssetName(MoveTemp(InAssetName))
		, PackagePath(MoveTemp(InPackagePath))
		, TargetRef(MoveTemp(InTargetRef))
		, MaterialKind(MoveTemp(InMaterialKind))
		, ParentClassRef(MoveTemp(InParentClassRef))
		, ParentMaterialRef(MoveTemp(InParentMaterialRef))
		, SourceRef(MoveTemp(InSourceRef))
	{
		for (FString* Value : {
			&AssetName, &PackagePath, &TargetRef, &MaterialKind, &ParentClassRef,
			&ParentMaterialRef, &SourceRef})
		{
			Value->TrimStartAndEndInline();
		}
		while (PackagePath.EndsWith(TEXT("/")))
		{
			PackagePath.LeftChopInline(1);
		}
		const bool bKindSpecificFieldsValid =
			(InputKind == EGraphCompoundTypedInputKind::BlueprintAssetCreate
				&& !ParentClassRef.IsEmpty()
				&& ParentMaterialRef.IsEmpty() && SourceRef.IsEmpty())
			|| (((InputKind == EGraphCompoundTypedInputKind::UmgAssetCreate
						&& MaterialKind.IsEmpty())
					|| (InputKind
							== EGraphCompoundTypedInputKind::MaterialAssetCreate
						&& (MaterialKind == TEXT("material")
							|| MaterialKind == TEXT("material_function"))))
				&& ParentClassRef.IsEmpty()
				&& ParentMaterialRef.IsEmpty() && SourceRef.IsEmpty())
			|| (InputKind
					== EGraphCompoundTypedInputKind::MaterialInstanceAssetCreate
				&& ParentClassRef.IsEmpty()
				&& !ParentMaterialRef.IsEmpty() && SourceRef.IsEmpty())
			|| ((InputKind
						== EGraphCompoundTypedInputKind::NiagaraEmitterAssetCreate
					|| InputKind
						== EGraphCompoundTypedInputKind::NiagaraSystemAssetCreate
					|| InputKind
						== EGraphCompoundTypedInputKind::NiagaraAssetDuplicate)
				&& ParentClassRef.IsEmpty()
				&& ParentMaterialRef.IsEmpty() && !SourceRef.IsEmpty());
		bValid = !AssetName.IsEmpty() && !PackagePath.IsEmpty()
			&& TargetRef == PackagePath + TEXT("/") + AssetName
			&& bKindSpecificFieldsValid;
	}

	EGraphCompoundTypedInputKind
	FGraphCompoundAssetCreateRequest::GetInputKind() const
	{
		return InputKind;
	}

	const FString& FGraphCompoundAssetCreateRequest::GetAssetName() const
	{
		return AssetName;
	}

	const FString& FGraphCompoundAssetCreateRequest::GetPackagePath() const
	{
		return PackagePath;
	}

	const FString& FGraphCompoundAssetCreateRequest::GetTargetRef() const
	{
		return TargetRef;
	}

	const FString& FGraphCompoundAssetCreateRequest::GetMaterialKind() const
	{
		return MaterialKind;
	}

	const FString&
	FGraphCompoundAssetCreateRequest::GetParentClassRef() const
	{
		return ParentClassRef;
	}

	const FString&
	FGraphCompoundAssetCreateRequest::GetParentMaterialRef() const
	{
		return ParentMaterialRef;
	}

	const FString& FGraphCompoundAssetCreateRequest::GetSourceRef() const
	{
		return SourceRef;
	}

	bool FGraphCompoundAssetCreateRequest::IsValid() const
	{
		return bValid;
	}

	bool TryDecodeGraphCompoundAssetCreateRequest(
		const FGraphCompoundDomainRouteProfile& Profile,
		const FUeAgentContractValue& Input,
		FUeAgentResourceTracker& Resources,
		FGraphCompoundAssetCreateRequest& OutRequest,
		FGraphCompoundRouteError& OutError)
	{
		OutRequest = FGraphCompoundAssetCreateRequest();
		OutError = {};
		if (Profile.GetRouteKind()
			!= EGraphCompoundDomainRouteKind::TypedOperation)
		{
			SetError(OutError, TEXT("graph_compound_route_kind_mismatch"),
				Profile.GetCapabilityId(), TEXT("typed_operation"));
			return false;
		}
		const EGraphCompoundTypedInputKind InputKind =
			Profile.GetTypedInputKind();
		if (InputKind != EGraphCompoundTypedInputKind::BlueprintAssetCreate
			&& InputKind != EGraphCompoundTypedInputKind::UmgAssetCreate
			&& InputKind != EGraphCompoundTypedInputKind::MaterialAssetCreate
			&& InputKind
				!= EGraphCompoundTypedInputKind::MaterialInstanceAssetCreate
			&& InputKind
				!= EGraphCompoundTypedInputKind::NiagaraEmitterAssetCreate
			&& InputKind
				!= EGraphCompoundTypedInputKind::NiagaraSystemAssetCreate
			&& InputKind
				!= EGraphCompoundTypedInputKind::NiagaraAssetDuplicate)
		{
			SetError(OutError,
				TEXT("graph_compound_asset_create_route_mismatch"),
				Profile.GetCapabilityId());
			return false;
		}
		TSharedPtr<FJsonObject> Object;
		if (!TryValidatedObject(Profile, Input, Resources, Object, OutError))
		{
			return false;
		}
		auto OptionalString = [&Object](const TCHAR* Field)
		{
			FString Value;
			Object->TryGetStringField(Field, Value);
			return Value;
		};
		const bool bDuplicate = InputKind
			== EGraphCompoundTypedInputKind::NiagaraAssetDuplicate;
		const FString TargetRef = bDuplicate
			? Object->GetStringField(TEXT("target_ref"))
			: AssetTargetRef(*Object);
		OutRequest = FGraphCompoundAssetCreateRequest(
			InputKind,
			bDuplicate
				? FPackageName::GetLongPackageAssetName(TargetRef)
				: Object->GetStringField(TEXT("asset_name")),
			bDuplicate
				? FPackageName::GetLongPackagePath(TargetRef)
				: Object->GetStringField(TEXT("package_path")),
			TargetRef,
			InputKind == EGraphCompoundTypedInputKind::MaterialAssetCreate
				? (Object->HasField(TEXT("material_kind"))
					? Object->GetStringField(TEXT("material_kind"))
					: FString(TEXT("material")))
				: FString(),
			OptionalString(TEXT("parent_class_ref")),
			OptionalString(TEXT("parent_material_ref")),
			OptionalString(TEXT("source_ref")));
		if (!OutRequest.IsValid())
		{
			SetError(OutError, TEXT("graph_compound_asset_create_invalid"),
				Profile.GetCapabilityId());
			return false;
		}
		return true;
	}

	FGraphCompoundHjsonSourceRequest::FGraphCompoundHjsonSourceRequest(
		FString InCompositionKind,
		FString InTargetRef,
		const EGraphCompoundHjsonSourceKind InSourceKind,
		FString InSourceLocator,
		FString InInlineHjson,
		const EChangeIntent InIntent,
		FString InExpectedRevision,
		const bool bInDryRun)
		: CompositionKind(MoveTemp(InCompositionKind))
		, TargetRef(MoveTemp(InTargetRef))
		, SourceKind(InSourceKind)
		, SourceLocator(MoveTemp(InSourceLocator))
		, InlineHjson(MoveTemp(InInlineHjson))
		, Intent(InIntent)
		, ExpectedRevision(MoveTemp(InExpectedRevision))
		, bDryRun(bInDryRun)
	{
		for (FString* Value : {
			&CompositionKind, &TargetRef, &SourceLocator,
			&InlineHjson, &ExpectedRevision})
		{
			Value->TrimStartAndEndInline();
		}
		const bool bSourceValid =
			((SourceKind == EGraphCompoundHjsonSourceKind::Directory
				|| SourceKind == EGraphCompoundHjsonSourceKind::SingleFile)
				&& !SourceLocator.IsEmpty() && InlineHjson.IsEmpty())
			|| (SourceKind == EGraphCompoundHjsonSourceKind::Inline
				&& SourceLocator.IsEmpty() && !InlineHjson.IsEmpty());
		bValid = !CompositionKind.IsEmpty() && !TargetRef.IsEmpty()
			&& !ExpectedRevision.IsEmpty() && bSourceValid
			&& (Intent == EChangeIntent::Update
				|| Intent == EChangeIntent::Replace);
	}

	const FString& FGraphCompoundHjsonSourceRequest::GetCompositionKind() const { return CompositionKind; }
	const FString& FGraphCompoundHjsonSourceRequest::GetTargetRef() const { return TargetRef; }
	EGraphCompoundHjsonSourceKind FGraphCompoundHjsonSourceRequest::GetSourceKind() const { return SourceKind; }
	const FString& FGraphCompoundHjsonSourceRequest::GetSourceLocator() const { return SourceLocator; }
	const FString& FGraphCompoundHjsonSourceRequest::GetInlineHjson() const { return InlineHjson; }
	EChangeIntent FGraphCompoundHjsonSourceRequest::GetIntent() const { return Intent; }
	const FString& FGraphCompoundHjsonSourceRequest::GetExpectedRevision() const { return ExpectedRevision; }
	bool FGraphCompoundHjsonSourceRequest::IsDryRun() const { return bDryRun; }
	bool FGraphCompoundHjsonSourceRequest::IsValid() const { return bValid; }

	bool TryDecodeGraphCompoundHjsonSourceRequest(
		const FGraphCompoundDomainRouteProfile& Profile,
		const FUeAgentContractValue& Input,
		FUeAgentResourceTracker& Resources,
		FGraphCompoundHjsonSourceRequest& OutRequest,
		FGraphCompoundRouteError& OutError)
	{
		OutRequest = FGraphCompoundHjsonSourceRequest();
		OutError = {};
		if (Profile.GetRouteKind() != EGraphCompoundDomainRouteKind::HjsonImport)
		{
			SetError(OutError, TEXT("graph_compound_route_kind_mismatch"),
				Profile.GetCapabilityId(), TEXT("hjson_import"));
			return false;
		}
		TSharedPtr<FJsonObject> Object;
		if (!TryValidatedObject(Profile, Input, Resources, Object, OutError))
		{
			return false;
		}
		const FString CompositionKind =
			Object->GetStringField(TEXT("composition_kind"));
		if (!Profile.GetAllowedCompositionKinds().Contains(CompositionKind))
		{
			SetError(OutError,
				TEXT("graph_compound_hjson_composition_mismatch"),
				CompositionKind, Profile.GetCapabilityId());
			return false;
		}
		EGraphCompoundHjsonSourceKind SourceKind =
			EGraphCompoundHjsonSourceKind::Invalid;
		const FString SourceKindText =
			Object->GetStringField(TEXT("source_kind"));
		if (SourceKindText == TEXT("directory"))
		{
			SourceKind = EGraphCompoundHjsonSourceKind::Directory;
		}
		else if (SourceKindText == TEXT("single_file"))
		{
			SourceKind = EGraphCompoundHjsonSourceKind::SingleFile;
		}
		else if (SourceKindText == TEXT("inline"))
		{
			SourceKind = EGraphCompoundHjsonSourceKind::Inline;
		}
		EChangeIntent Intent = EChangeIntent::Update;
		if (!TryIntent(Object->GetStringField(TEXT("intent")), Intent))
		{
			SetError(OutError, TEXT("graph_compound_hjson_intent_invalid"),
				Profile.GetCapabilityId());
			return false;
		}
		FString SourceLocator;
		FString InlineHjson;
		Object->TryGetStringField(TEXT("source_locator"), SourceLocator);
		Object->TryGetStringField(TEXT("inline_hjson"), InlineHjson);
		OutRequest = FGraphCompoundHjsonSourceRequest(
			CompositionKind,
			Object->GetStringField(TEXT("target_ref")),
			SourceKind,
			MoveTemp(SourceLocator),
			MoveTemp(InlineHjson),
			Intent,
			Object->GetStringField(TEXT("expected_revision")),
			Object->GetBoolField(TEXT("dry_run")));
		if (!OutRequest.IsValid())
		{
			SetError(OutError, TEXT("graph_compound_hjson_source_invalid"),
				Profile.GetCapabilityId());
			return false;
		}
		return true;
	}

	FGraphCompoundNiagaraMutationSelector::
	FGraphCompoundNiagaraMutationSelector(
		const EGraphCompoundTypedInputKind InInputKind,
		FString InTargetRef,
		FString InEmitterHandleId,
		FString InStageRef,
		FString InItemRef,
		FString InPredecessorRef,
		FString InIssueId,
		FString InFixId,
		FString InModuleRef,
		FString InInputName,
		FString InRendererRef,
		FString InParameterName,
		FString InTypeRef)
		: InputKind(InInputKind)
		, TargetRef(MoveTemp(InTargetRef))
		, EmitterHandleId(MoveTemp(InEmitterHandleId))
		, StageRef(MoveTemp(InStageRef))
		, ItemRef(MoveTemp(InItemRef))
		, PredecessorRef(MoveTemp(InPredecessorRef))
		, IssueId(MoveTemp(InIssueId))
		, FixId(MoveTemp(InFixId))
		, ModuleRef(MoveTemp(InModuleRef))
		, InputName(MoveTemp(InInputName))
		, RendererRef(MoveTemp(InRendererRef))
		, ParameterName(MoveTemp(InParameterName))
		, TypeRef(MoveTemp(InTypeRef))
	{
		for (FString* Value : {
			&TargetRef, &EmitterHandleId, &StageRef, &ItemRef,
			&PredecessorRef, &IssueId, &FixId, &ModuleRef, &InputName,
			&RendererRef, &ParameterName, &TypeRef})
		{
			Value->TrimStartAndEndInline();
		}
		const bool bCommon = IsNiagaraMutationKind(InputKind)
			&& !TargetRef.IsEmpty() && !EmitterHandleId.IsEmpty();
		switch (InputKind)
		{
		case EGraphCompoundTypedInputKind::NiagaraStackReorder:
			bValid = bCommon && !StageRef.IsEmpty() && !ItemRef.IsEmpty();
			break;
		case EGraphCompoundTypedInputKind::NiagaraStackIssueFix:
			bValid = bCommon && !IssueId.IsEmpty() && !FixId.IsEmpty();
			break;
		case EGraphCompoundTypedInputKind::NiagaraCompoundInput:
			bValid = bCommon && !StageRef.IsEmpty() && !ModuleRef.IsEmpty()
				&& !InputName.IsEmpty();
			break;
		case EGraphCompoundTypedInputKind::NiagaraRendererEventParameter:
			bValid = bCommon && !RendererRef.IsEmpty()
				&& !ParameterName.IsEmpty() && !TypeRef.IsEmpty();
			break;
		default:
			break;
		}
	}

	EGraphCompoundTypedInputKind
	FGraphCompoundNiagaraMutationSelector::GetInputKind() const
	{
		return InputKind;
	}
	const FString& FGraphCompoundNiagaraMutationSelector::GetTargetRef() const { return TargetRef; }
	const FString& FGraphCompoundNiagaraMutationSelector::GetEmitterHandleId() const { return EmitterHandleId; }
	const FString& FGraphCompoundNiagaraMutationSelector::GetStageRef() const { return StageRef; }
	const FString& FGraphCompoundNiagaraMutationSelector::GetItemRef() const { return ItemRef; }
	const FString& FGraphCompoundNiagaraMutationSelector::GetPredecessorRef() const { return PredecessorRef; }
	const FString& FGraphCompoundNiagaraMutationSelector::GetIssueId() const { return IssueId; }
	const FString& FGraphCompoundNiagaraMutationSelector::GetFixId() const { return FixId; }
	const FString& FGraphCompoundNiagaraMutationSelector::GetModuleRef() const { return ModuleRef; }
	const FString& FGraphCompoundNiagaraMutationSelector::GetInputName() const { return InputName; }
	const FString& FGraphCompoundNiagaraMutationSelector::GetRendererRef() const { return RendererRef; }
	const FString& FGraphCompoundNiagaraMutationSelector::GetParameterName() const { return ParameterName; }
	const FString& FGraphCompoundNiagaraMutationSelector::GetTypeRef() const { return TypeRef; }
	bool FGraphCompoundNiagaraMutationSelector::IsValid() const { return bValid; }

	FString FGraphCompoundNiagaraMutationSelector::GetStableSemanticId() const
	{
		switch (InputKind)
		{
		case EGraphCompoundTypedInputKind::NiagaraStackReorder:
			return TEXT("order:") + EmitterHandleId + TEXT(":")
				+ StageRef + TEXT(":") + ItemRef;
		case EGraphCompoundTypedInputKind::NiagaraStackIssueFix:
			return TEXT("property:issue:") + EmitterHandleId
				+ TEXT(":") + IssueId;
		case EGraphCompoundTypedInputKind::NiagaraCompoundInput:
			return TEXT("input:") + EmitterHandleId + TEXT(":")
				+ StageRef + TEXT(":") + ModuleRef + TEXT(":") + InputName;
		case EGraphCompoundTypedInputKind::NiagaraRendererEventParameter:
			return TEXT("binding:") + EmitterHandleId + TEXT(":")
				+ RendererRef.ToLower() + TEXT(":") + ParameterName
				+ TEXT(":") + TypeRef;
		default:
			return FString();
		}
	}

	bool TryDecodeGraphCompoundNiagaraMutationSelector(
		const FGraphCompoundDomainRouteProfile& Profile,
		const FUeAgentContractValue& Input,
		FUeAgentResourceTracker& Resources,
		FGraphCompoundNiagaraMutationSelector& OutSelector,
		FGraphCompoundRouteError& OutError)
	{
		OutSelector = FGraphCompoundNiagaraMutationSelector();
		OutError = {};
		if (Profile.GetRouteKind()
				!= EGraphCompoundDomainRouteKind::TypedOperation
			|| !IsNiagaraMutationKind(Profile.GetTypedInputKind()))
		{
			SetError(OutError,
				TEXT("graph_compound_niagara_selector_kind_invalid"),
				Profile.GetCapabilityId());
			return false;
		}
		TSharedPtr<FJsonObject> Object;
		return TryValidatedObject(Profile, Input, Resources, Object, OutError)
			&& TryBuildNiagaraMutationSelector(
				Profile, *Object, OutSelector, OutError);
	}

	bool TryDecodeGraphCompoundTypedOperation(
		const FGraphCompoundDomainRouteProfile& Profile,
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
			SetError(OutError, TEXT("graph_compound_route_kind_mismatch"),
				Profile.GetCapabilityId(), TEXT("typed_operation"));
			return false;
		}
		TSharedPtr<FJsonObject> Object;
		if (!TryValidatedObject(Profile, Input, Resources, Object, OutError))
		{
			return false;
		}
		const FString Generation = Snapshot->GetRevision().GetGeneration();
		const TArray<FString> Authority = {
			Profile.GetDomainId() + TEXT(".write")};
		const FString Evidence =
			TEXT("request:") + Profile.GetCapabilityId();
		TArray<FGraphCompoundCanonicalEdit> Edits;

		auto AddStringEdit = [&] (
			const FString& SemanticId,
			const FString& OwnerId,
			const EGraphCompoundElementKind ElementKind,
			const EChangeIntent Intent,
			const FString& Text,
			TArray<FUeAgentCanonicalIdentity> Dependencies = {})
		{
			FUeAgentCanonicalValuePtr Value;
			if (!TryCanonicalString(
					Text, Resources, Value, OutError, SemanticId))
			{
				return false;
			}
			const FUeAgentCanonicalIdentity EditIdentity = Identity(
				SemanticId, Profile.GetTargetKind(), OwnerId, Generation);
			Edits.Emplace(
				EditIdentity, ElementKind, Intent, Value,
				MoveTemp(Dependencies), Authority, Evidence,
				TArray<FUeAgentCanonicalIdentity>({EditIdentity}));
			return true;
		};

		switch (Profile.GetTypedInputKind())
		{
		case EGraphCompoundTypedInputKind::BlueprintAssetCreate:
		case EGraphCompoundTypedInputKind::UmgAssetCreate:
		case EGraphCompoundTypedInputKind::MaterialAssetCreate:
		case EGraphCompoundTypedInputKind::MaterialInstanceAssetCreate:
		case EGraphCompoundTypedInputKind::NiagaraEmitterAssetCreate:
		case EGraphCompoundTypedInputKind::NiagaraSystemAssetCreate:
		{
			const FString TargetRef = AssetTargetRef(*Object);
			FString Definition = Profile.GetTargetKind();
			if (Profile.GetTypedInputKind()
				== EGraphCompoundTypedInputKind::MaterialAssetCreate)
			{
				Object->TryGetStringField(TEXT("material_kind"), Definition);
				if (Definition.IsEmpty())
				{
					Definition = TEXT("material");
				}
			}
			for (const TCHAR* Field : {
				TEXT("parent_class_ref"), TEXT("parent_material_ref"),
				TEXT("source_ref")})
			{
				FString Value;
				if (Object->TryGetStringField(Field, Value))
				{
					Definition += TEXT(":") + Value;
				}
			}
			TArray<FUeAgentCanonicalIdentity> Dependencies;
			if (Profile.GetTypedInputKind()
					== EGraphCompoundTypedInputKind::NiagaraEmitterAssetCreate
				|| Profile.GetTypedInputKind()
					== EGraphCompoundTypedInputKind::NiagaraSystemAssetCreate)
			{
				const FString SourceRef =
					Object->GetStringField(TEXT("source_ref"));
				Dependencies.Add(Identity(
					TEXT("target:") + SourceRef,
					Profile.GetTargetKind(), SourceRef, Generation));
			}
			if (!AddStringEdit(
					TEXT("target:") + TargetRef,
					TargetRef,
					EGraphCompoundElementKind::Target,
					EChangeIntent::Create,
					Definition,
					MoveTemp(Dependencies)))
			{
				return false;
			}
			break;
		}
		case EGraphCompoundTypedInputKind::GraphEdits:
		{
			const FString TargetRef = Object->GetStringField(TEXT("target_ref"));
			for (const TSharedPtr<FJsonValue>& EditValue
				: Object->GetArrayField(TEXT("edits")))
			{
				const TSharedPtr<FJsonObject> Edit = EditValue->AsObject();
				if (!Edit.IsValid())
				{
					SetError(OutError, TEXT("graph_compound_edit_invalid"),
						Profile.GetCapabilityId());
					return false;
				}
				EGraphCompoundElementKind ElementKind =
					EGraphCompoundElementKind::Item;
				EChangeIntent Intent = EChangeIntent::Update;
				if (!TryElementKind(
						Edit->GetStringField(TEXT("element_kind")), ElementKind)
					|| !TryIntent(Edit->GetStringField(TEXT("intent")), Intent))
				{
					SetError(OutError, TEXT("graph_compound_edit_kind_invalid"),
						Profile.GetCapabilityId());
					return false;
				}
				const FString SemanticId = Edit->GetStringField(TEXT("identity"));
				FUeAgentCanonicalValuePtr Proposed;
				const TSharedPtr<FJsonValue>* ProposedJson =
					Edit->Values.Find(TEXT("value"));
				if (Intent != EChangeIntent::Delete)
				{
					if (!ProposedJson || !TryCanonical(
							*ProposedJson, Resources, Proposed,
							OutError, SemanticId))
					{
						if (!OutError.IsValid())
						{
							SetError(OutError,
								TEXT("graph_compound_edit_value_missing"),
								SemanticId);
						}
						return false;
					}
				}
				TArray<FUeAgentCanonicalIdentity> Dependencies;
				const TArray<TSharedPtr<FJsonValue>>* DependencyValues = nullptr;
				if (Edit->TryGetArrayField(TEXT("depends_on"), DependencyValues)
					&& DependencyValues)
				{
					for (const TSharedPtr<FJsonValue>& Dependency : *DependencyValues)
					{
						Dependencies.Add(Identity(
							Dependency->AsString(), Profile.GetTargetKind(),
							TargetRef, Generation));
					}
				}
				const FUeAgentCanonicalIdentity EditIdentity = Identity(
					SemanticId, Profile.GetTargetKind(), TargetRef, Generation);
				Edits.Emplace(
					EditIdentity, ElementKind, Intent, Proposed,
					MoveTemp(Dependencies), Authority, Evidence,
					TArray<FUeAgentCanonicalIdentity>({EditIdentity}));
			}
			break;
		}
		case EGraphCompoundTypedInputKind::MaterialInstanceParameters:
		{
			const FString TargetRef = Object->GetStringField(TEXT("target_ref"));
			for (const TSharedPtr<FJsonValue>& ParameterValue
				: Object->GetArrayField(TEXT("parameters")))
			{
				const TSharedPtr<FJsonObject> Parameter = ParameterValue->AsObject();
				const FString SemanticId = FString::Printf(
					TEXT("property:%s:%s:%d:%s"),
					*Parameter->GetStringField(TEXT("name")),
					*Parameter->GetStringField(TEXT("association")),
					static_cast<int32>(Parameter->GetIntegerField(TEXT("index"))),
					*Parameter->GetStringField(TEXT("type")));
				FUeAgentCanonicalValuePtr Proposed;
				if (!TryCanonical(
						Parameter->Values.FindChecked(TEXT("value")),
						Resources, Proposed, OutError, SemanticId))
				{
					return false;
				}
				FUeAgentCanonicalValuePtr OverrideState;
				FUeAgentCanonicalValuePtr ProposedState;
				FUeAgentStateSemanticError StateError;
				if (!FUeAgentCanonicalValue::TryString(
						TEXT("overridden"), Resources,
						OverrideState, StateError)
					|| !FUeAgentCanonicalValue::TryObject(
						{{TEXT("effective_value"), Proposed.ToSharedRef()},
							{TEXT("override_state"), OverrideState.ToSharedRef()}},
						Resources, ProposedState, StateError))
				{
					SetError(OutError, StateError.Code,
						SemanticId, StateError.Path);
					return false;
				}
				const FUeAgentCanonicalIdentity EditIdentity = Identity(
					SemanticId, Profile.GetTargetKind(), TargetRef, Generation);
				Edits.Emplace(
					EditIdentity, EGraphCompoundElementKind::Property,
					EChangeIntent::Update, ProposedState,
					TArray<FUeAgentCanonicalIdentity>(),
					Authority, Evidence,
					TArray<FUeAgentCanonicalIdentity>({EditIdentity}));
			}
			break;
		}
		case EGraphCompoundTypedInputKind::NiagaraAssetDuplicate:
		{
			const FString SourceRef = Object->GetStringField(TEXT("source_ref"));
			const FString TargetRef = Object->GetStringField(TEXT("target_ref"));
			const FUeAgentCanonicalIdentity SourceIdentity = Identity(
				TEXT("target:") + SourceRef, Profile.GetTargetKind(),
				SourceRef, Generation);
			if (!AddStringEdit(
					TEXT("target:") + TargetRef, TargetRef,
					EGraphCompoundElementKind::Target,
					EChangeIntent::Create, SourceRef, {SourceIdentity}))
			{
				return false;
			}
			break;
		}
		case EGraphCompoundTypedInputKind::NiagaraStackReorder:
		{
			FGraphCompoundNiagaraMutationSelector Selector;
			if (!TryBuildNiagaraMutationSelector(
					Profile, *Object, Selector, OutError))
			{
				return false;
			}
			if (!AddStringEdit(
					Selector.GetStableSemanticId(), Selector.GetTargetRef(),
					EGraphCompoundElementKind::Order,
					EChangeIntent::Update,
					Selector.GetPredecessorRef()))
			{
				return false;
			}
			break;
		}
		case EGraphCompoundTypedInputKind::NiagaraStackIssueFix:
		{
			FGraphCompoundNiagaraMutationSelector Selector;
			if (!TryBuildNiagaraMutationSelector(
					Profile, *Object, Selector, OutError))
			{
				return false;
			}
			if (!AddStringEdit(
					Selector.GetStableSemanticId(), Selector.GetTargetRef(),
					EGraphCompoundElementKind::Property,
					EChangeIntent::Update,
					TEXT("resolved")))
			{
				return false;
			}
			break;
		}
		case EGraphCompoundTypedInputKind::NiagaraCompoundInput:
		case EGraphCompoundTypedInputKind::NiagaraRendererEventParameter:
		{
			FGraphCompoundNiagaraMutationSelector Selector;
			if (!TryBuildNiagaraMutationSelector(
					Profile, *Object, Selector, OutError))
			{
				return false;
			}
			const bool bCompound = Profile.GetTypedInputKind()
				== EGraphCompoundTypedInputKind::NiagaraCompoundInput;
			const FString SemanticId = Selector.GetStableSemanticId();
			FUeAgentCanonicalValuePtr Proposed;
			if (!TryCanonical(
					Object->Values.FindChecked(TEXT("value")),
					Resources, Proposed, OutError, SemanticId))
			{
				return false;
			}
			const FUeAgentCanonicalIdentity EditIdentity = Identity(
				SemanticId, Profile.GetTargetKind(),
				Selector.GetTargetRef(), Generation);
			Edits.Emplace(
				EditIdentity,
				bCompound ? EGraphCompoundElementKind::Input
					: EGraphCompoundElementKind::Binding,
				EChangeIntent::Update, Proposed,
				TArray<FUeAgentCanonicalIdentity>(), Authority, Evidence,
				TArray<FUeAgentCanonicalIdentity>({EditIdentity}));
			break;
		}
		case EGraphCompoundTypedInputKind::NiagaraCompileRefreshSettle:
		{
			const FString TargetRef = Object->GetStringField(TEXT("target_ref"));
			if (!AddStringEdit(
					TEXT("property:lifecycle:compile_refresh_settle"),
					TargetRef, EGraphCompoundElementKind::Property,
					EChangeIntent::Update, TEXT("requested")))
			{
				return false;
			}
			break;
		}
		default:
			SetError(OutError, TEXT("graph_compound_typed_input_kind_invalid"),
				Profile.GetCapabilityId());
			return false;
		}

		OutPayload = FGraphCompoundCanonicalPayload(
			GetGraphCompoundCanonicalSchemaId(), MoveTemp(Edits));
		return !OutPayload.GetEdits().IsEmpty();
	}

	bool TryDecodeGraphCompoundTypedOperationTargetRef(
		const FGraphCompoundDomainRouteProfile& Profile,
		const FUeAgentContractValue& Input,
		FUeAgentResourceTracker& Resources,
		FString& OutTargetRef,
		FGraphCompoundRouteError& OutError)
	{
		OutTargetRef.Reset();
		OutError = {};
		if (Profile.GetRouteKind()
				!= EGraphCompoundDomainRouteKind::TypedOperation
			|| (Profile.GetTypedInputKind()
					!= EGraphCompoundTypedInputKind::GraphEdits
				&& Profile.GetTypedInputKind()
					!= EGraphCompoundTypedInputKind::MaterialInstanceParameters
				&& Profile.GetTypedInputKind()
					!= EGraphCompoundTypedInputKind::NiagaraCompileRefreshSettle))
		{
			SetError(OutError,
				TEXT("graph_compound_typed_target_kind_invalid"),
				Profile.GetCapabilityId());
			return false;
		}
		TSharedPtr<FJsonObject> Object;
		if (!TryValidatedObject(Profile, Input, Resources, Object, OutError))
		{
			return false;
		}
		if (!Object->TryGetStringField(TEXT("target_ref"), OutTargetRef)
			|| OutTargetRef.TrimStartAndEnd().IsEmpty())
		{
			SetError(OutError,
				TEXT("graph_compound_typed_target_missing"),
				Profile.GetCapabilityId());
			return false;
		}
		return true;
	}

	bool TryBuildGraphCompoundTypedOperationDraft(
		const FGraphCompoundDomainRouteProfile& Profile,
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
				TEXT("graph_compound_descriptor_identity_mismatch"),
				Profile.GetDescriptorId());
			return false;
		}
		FGraphCompoundCanonicalPayload Payload(
			GetGraphCompoundCanonicalSchemaId(), {});
		if (!TryDecodeGraphCompoundTypedOperation(
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

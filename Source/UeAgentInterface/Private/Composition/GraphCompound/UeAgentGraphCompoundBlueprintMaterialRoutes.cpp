#include "Composition/GraphCompound/UeAgentGraphCompoundBlueprintMaterialRoutes.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Core/Contract/UeAgentContractSchema.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Core/Contract/UeAgentExecutionContextPolicy.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"

namespace UE::UAI::Composition::GraphCompound
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
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
			const TCHAR* DomainId;
			const TCHAR* FamilyId;
			EGraphCompoundDomainRouteKind RouteKind;
			EGraphCompoundTypedInputKind InputKind;
			const TCHAR* TargetKind;
		};

		const TArray<FRouteSpec>& RouteSpecs()
		{
			static const TArray<FRouteSpec> Values = {
				{TEXT("blueprint.asset.create"), TEXT("uai.operation.blueprint.asset.create"), TEXT("blueprint_umg_graph"), TEXT("asset_lifecycle"), EGraphCompoundDomainRouteKind::TypedOperation, EGraphCompoundTypedInputKind::BlueprintAssetCreate, TEXT("blueprint")},
				{TEXT("blueprint.component.events.discover"), TEXT("uai.query.blueprint.component.events.discover"), TEXT("blueprint_umg_graph"), TEXT("event_discovery"), EGraphCompoundDomainRouteKind::Query, EGraphCompoundTypedInputKind::None, TEXT("blueprint")},
				{TEXT("umg.asset.create"), TEXT("uai.operation.umg.asset.create"), TEXT("blueprint_umg_graph"), TEXT("asset_lifecycle"), EGraphCompoundDomainRouteKind::TypedOperation, EGraphCompoundTypedInputKind::UmgAssetCreate, TEXT("widget_blueprint")},
				{TEXT("umg.widget.events.discover"), TEXT("uai.query.umg.widget.events.discover"), TEXT("blueprint_umg_graph"), TEXT("event_discovery"), EGraphCompoundDomainRouteKind::Query, EGraphCompoundTypedInputKind::None, TEXT("widget_blueprint")},
				{TEXT("material.asset.create"), TEXT("uai.operation.material.asset.create"), TEXT("material_niagara_graph"), TEXT("asset_lifecycle"), EGraphCompoundDomainRouteKind::TypedOperation, EGraphCompoundTypedInputKind::MaterialAssetCreate, TEXT("material")},
				{TEXT("material_instance.asset.create"), TEXT("uai.operation.material_instance.asset.create"), TEXT("material_niagara_graph"), TEXT("asset_lifecycle"), EGraphCompoundDomainRouteKind::TypedOperation, EGraphCompoundTypedInputKind::MaterialInstanceAssetCreate, TEXT("material_instance")},
				{TEXT("material_instance.parameters.apply"), TEXT("uai.operation.material_instance.parameters.apply"), TEXT("material_niagara_graph"), TEXT("material_instance_parameter_mutation"), EGraphCompoundDomainRouteKind::TypedOperation, EGraphCompoundTypedInputKind::MaterialInstanceParameters, TEXT("material_instance")},
				{TEXT("niagara.emitter.asset.create"), TEXT("uai.operation.niagara.emitter.asset.create"), TEXT("material_niagara_graph"), TEXT("asset_lifecycle"), EGraphCompoundDomainRouteKind::TypedOperation, EGraphCompoundTypedInputKind::NiagaraEmitterAssetCreate, TEXT("niagara_emitter")},
				{TEXT("niagara.system.asset.create"), TEXT("uai.operation.niagara.system.asset.create"), TEXT("material_niagara_graph"), TEXT("asset_lifecycle"), EGraphCompoundDomainRouteKind::TypedOperation, EGraphCompoundTypedInputKind::NiagaraSystemAssetCreate, TEXT("niagara_system")},
				{TEXT("niagara.asset.duplicate"), TEXT("uai.operation.niagara.asset.duplicate"), TEXT("material_niagara_graph"), TEXT("asset_lifecycle"), EGraphCompoundDomainRouteKind::TypedOperation, EGraphCompoundTypedInputKind::NiagaraAssetDuplicate, TEXT("niagara_asset")},
				{TEXT("niagara.stack.reorder"), TEXT("uai.operation.niagara.stack.reorder"), TEXT("material_niagara_graph"), TEXT("niagara_stack_mutation"), EGraphCompoundDomainRouteKind::TypedOperation, EGraphCompoundTypedInputKind::NiagaraStackReorder, TEXT("niagara_stack")},
				{TEXT("niagara.stack.issue_fix"), TEXT("uai.operation.niagara.stack.issue_fix"), TEXT("material_niagara_graph"), TEXT("niagara_stack_mutation"), EGraphCompoundDomainRouteKind::TypedOperation, EGraphCompoundTypedInputKind::NiagaraStackIssueFix, TEXT("niagara_stack")},
				{TEXT("niagara.compound.apply"), TEXT("uai.operation.niagara.compound.apply"), TEXT("material_niagara_graph"), TEXT("niagara_compound_input"), EGraphCompoundDomainRouteKind::TypedOperation, EGraphCompoundTypedInputKind::NiagaraCompoundInput, TEXT("niagara_compound")},
				{TEXT("niagara.renderer_event_parameter.apply"), TEXT("uai.operation.niagara.renderer_event_parameter.apply"), TEXT("material_niagara_graph"), TEXT("niagara_renderer_event_parameter_mutation"), EGraphCompoundDomainRouteKind::TypedOperation, EGraphCompoundTypedInputKind::NiagaraRendererEventParameter, TEXT("niagara_renderer")},
				{TEXT("niagara.compile_refresh_settle"), TEXT("uai.operation.niagara.compile_refresh_settle"), TEXT("material_niagara_graph"), TEXT("compile_refresh_settle"), EGraphCompoundDomainRouteKind::TypedOperation, EGraphCompoundTypedInputKind::NiagaraCompileRefreshSettle, TEXT("niagara_asset")},
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
			if (Spec.RouteKind == EGraphCompoundDomainRouteKind::Query)
			{
				if (FString(Spec.CapabilityId)
					== TEXT("blueprint.component.events.discover"))
				{
					return CanonicalSchema(Error, TEXT(
						"{\"additionalProperties\":false,\"properties\":{" 
						"\"asset_path\":{\"minLength\":1,\"type\":\"string\"},"
						"\"compile_before_query\":{\"type\":\"boolean\"},"
						"\"component_name\":{\"minLength\":1,\"type\":\"string\"},"
						"\"include_hidden\":{\"type\":\"boolean\"},"
						"\"only_unbound\":{\"type\":\"boolean\"},"
						"\"query\":{\"type\":\"string\"}},"
						"\"required\":[\"asset_path\",\"component_name\"],"
						"\"type\":\"object\"}"));
				}
				return CanonicalSchema(Error, TEXT(
					"{\"additionalProperties\":false,\"properties\":{"
					"\"owner_item_ref\":{\"minLength\":1,\"type\":\"string\"},"
					"\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},"
					"\"required\":[\"owner_item_ref\",\"target_ref\"],"
					"\"type\":\"object\"}"));
			}
			switch (Spec.InputKind)
			{
			case EGraphCompoundTypedInputKind::BlueprintAssetCreate:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"asset_name\":{\"minLength\":1,\"type\":\"string\"},\"package_path\":{\"minLength\":1,\"type\":\"string\"},\"parent_class_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"asset_name\",\"package_path\",\"parent_class_ref\"],\"type\":\"object\"}"));
			case EGraphCompoundTypedInputKind::UmgAssetCreate:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"asset_name\":{\"minLength\":1,\"type\":\"string\"},\"package_path\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"asset_name\",\"package_path\"],\"type\":\"object\"}"));
			case EGraphCompoundTypedInputKind::MaterialAssetCreate:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"asset_name\":{\"minLength\":1,\"type\":\"string\"},\"material_kind\":{\"enum\":[\"material\",\"material_function\"],\"type\":\"string\"},\"package_path\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"asset_name\",\"package_path\"],\"type\":\"object\"}"));
			case EGraphCompoundTypedInputKind::MaterialInstanceAssetCreate:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"asset_name\":{\"minLength\":1,\"type\":\"string\"},\"package_path\":{\"minLength\":1,\"type\":\"string\"},\"parent_material_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"asset_name\",\"package_path\",\"parent_material_ref\"],\"type\":\"object\"}"));
			case EGraphCompoundTypedInputKind::NiagaraEmitterAssetCreate:
			case EGraphCompoundTypedInputKind::NiagaraSystemAssetCreate:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"asset_name\":{\"minLength\":1,\"type\":\"string\"},\"package_path\":{\"minLength\":1,\"type\":\"string\"},\"source_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"asset_name\",\"package_path\",\"source_ref\"],\"type\":\"object\"}"));
			case EGraphCompoundTypedInputKind::GraphEdits:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"edits\":{\"items\":{\"additionalProperties\":false,\"properties\":{\"depends_on\":{\"items\":{\"minLength\":1,\"type\":\"string\"},\"type\":\"array\",\"uniqueItems\":true},\"element_kind\":{\"enum\":[\"target\",\"structure\",\"item\",\"pin\",\"relation\",\"property\",\"input\",\"hierarchy\",\"order\",\"binding\",\"dependency\"],\"type\":\"string\"},\"identity\":{\"minLength\":1,\"type\":\"string\"},\"intent\":{\"enum\":[\"create\",\"update\",\"replace\",\"delete\"],\"type\":\"string\"},\"value\":{}},\"required\":[\"identity\",\"element_kind\",\"intent\"],\"type\":\"object\"},\"minItems\":1,\"type\":\"array\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"edits\",\"target_ref\"],\"type\":\"object\"}"));
			case EGraphCompoundTypedInputKind::MaterialInstanceParameters:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"parameters\":{\"items\":{\"additionalProperties\":false,\"properties\":{\"association\":{\"minLength\":1,\"type\":\"string\"},\"index\":{\"type\":\"integer\"},\"name\":{\"minLength\":1,\"type\":\"string\"},\"type\":{\"enum\":[\"scalar\",\"vector\",\"texture\",\"static_switch\",\"static_component_mask\"],\"type\":\"string\"},\"value\":{}},\"required\":[\"name\",\"association\",\"index\",\"type\",\"value\"],\"type\":\"object\"},\"minItems\":1,\"type\":\"array\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"parameters\",\"target_ref\"],\"type\":\"object\"}"));
			case EGraphCompoundTypedInputKind::NiagaraAssetDuplicate:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"source_ref\":{\"minLength\":1,\"type\":\"string\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"source_ref\",\"target_ref\"],\"type\":\"object\"}"));
			case EGraphCompoundTypedInputKind::NiagaraStackReorder:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"emitter_handle_id\":{\"minLength\":1,\"type\":\"string\"},\"item_ref\":{\"minLength\":1,\"type\":\"string\"},\"predecessor_ref\":{\"type\":\"string\"},\"stage_ref\":{\"minLength\":1,\"type\":\"string\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"emitter_handle_id\",\"item_ref\",\"predecessor_ref\",\"stage_ref\",\"target_ref\"],\"type\":\"object\"}"));
			case EGraphCompoundTypedInputKind::NiagaraStackIssueFix:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"emitter_handle_id\":{\"minLength\":1,\"type\":\"string\"},\"fix_id\":{\"minLength\":1,\"type\":\"string\"},\"issue_id\":{\"minLength\":1,\"type\":\"string\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"emitter_handle_id\",\"fix_id\",\"issue_id\",\"target_ref\"],\"type\":\"object\"}"));
			case EGraphCompoundTypedInputKind::NiagaraCompoundInput:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"emitter_handle_id\":{\"minLength\":1,\"type\":\"string\"},\"input_name\":{\"minLength\":1,\"type\":\"string\"},\"module_ref\":{\"minLength\":1,\"type\":\"string\"},\"stage_ref\":{\"minLength\":1,\"type\":\"string\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"},\"value\":{}},\"required\":[\"emitter_handle_id\",\"input_name\",\"module_ref\",\"stage_ref\",\"target_ref\",\"value\"],\"type\":\"object\"}"));
			case EGraphCompoundTypedInputKind::NiagaraRendererEventParameter:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"emitter_handle_id\":{\"minLength\":1,\"type\":\"string\"},\"parameter_name\":{\"minLength\":1,\"type\":\"string\"},\"renderer_ref\":{\"minLength\":1,\"type\":\"string\"},\"target_ref\":{\"minLength\":1,\"type\":\"string\"},\"type_ref\":{\"minLength\":1,\"type\":\"string\"},\"value\":{}},\"required\":[\"emitter_handle_id\",\"parameter_name\",\"renderer_ref\",\"target_ref\",\"type_ref\",\"value\"],\"type\":\"object\"}"));
			case EGraphCompoundTypedInputKind::NiagaraCompileRefreshSettle:
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"target_ref\":{\"minLength\":1,\"type\":\"string\"}},\"required\":[\"target_ref\"],\"type\":\"object\"}"));
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
				if (FString(Spec.CapabilityId)
					== TEXT("umg.widget.events.discover"))
				{
					return CanonicalSchema(Error, TEXT(
						"{\"additionalProperties\":false,\"properties\":{"
						"\"names\":{\"items\":{\"type\":\"string\"},\"type\":\"array\"}},"
						"\"required\":[\"names\"],\"type\":\"object\"}"));
				}
				if (FString(Spec.CapabilityId)
					== TEXT("blueprint.component.events.discover"))
				{
					return CanonicalSchema(Error, TEXT(
						"{\"additionalProperties\":false,\"properties\":{" 
						"\"names\":{\"items\":{\"type\":\"string\"},"
						"\"type\":\"array\"}},"
						"\"required\":[\"names\"],\"type\":\"object\"}"));
				}
				return CanonicalSchema(Error, TEXT("{\"additionalProperties\":false,\"properties\":{\"events\":{\"items\":{\"additionalProperties\":false,\"properties\":{\"event_ref\":{\"type\":\"string\"},\"native_field_name\":{\"type\":\"string\"},\"signature_ref\":{\"type\":\"string\"}},\"required\":[\"event_ref\",\"native_field_name\",\"signature_ref\"],\"type\":\"object\"},\"type\":\"array\"}},\"required\":[\"events\"],\"type\":\"object\"}"));
			}
			TSharedPtr<FJsonObject> Schema;
			FString Canonical;
			if (!Json::TryParseObject(TEXT("{\"additionalProperties\":false,\"properties\":{\"target_ref\":{\"type\":\"string\"}},\"required\":[\"target_ref\"],\"type\":\"object\"}"), Schema, Error))
			{
				MarkGraphCompoundNativeDiagnostic(Error);
				return FString();
			}
			// The receipt layout comes from the common operation evidence owner.
			// It is optional for older producers, never an asset-specific success flag.
			const auto Stages = MakeShared<FJsonObject>();
			const auto StageProperties = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> Required;
			const auto EvidenceShape = UeAgentAuthoringWorkspaceTransaction::FFinalReadbackEvidence().ToJsonObject();
			for (const auto& Field : EvidenceShape->Values)
			{
				const auto Property = MakeShared<FJsonObject>();
				Property->SetStringField(TEXT("type"), TEXT("boolean"));
				StageProperties->SetObjectField(Field.Key, Property);
				Required.Add(MakeShared<FJsonValueString>(Field.Key));
			}
			Stages->SetStringField(TEXT("type"), TEXT("object"));
			Stages->SetBoolField(TEXT("additionalProperties"), false);
			Stages->SetObjectField(TEXT("properties"), StageProperties);
			Stages->SetArrayField(TEXT("required"), Required);
			Schema->GetObjectField(TEXT("properties"))->SetObjectField(TEXT("operation_stages"), Stages);
			if (!Json::TryCanonicalize(Schema, Canonical, Error))
			{
				MarkGraphCompoundNativeDiagnostic(Error);
				return FString();
			}
			return Canonical;
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
			UE::UAI::Contract::FDiagnostic Error;
		};

		FRouteBundle BuildBundle()
		{
			FRouteBundle Bundle;
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
				Bundle.Profiles.Emplace(
					Spec.CapabilityId, Spec.DescriptorId, Spec.DomainId,
					Spec.FamilyId, Spec.RouteKind, Spec.InputKind,
					Spec.TargetKind,
					FGraphCompoundExecutionContract(
						Identity,
						bQuery ? EGraphCompoundExecutionLane::Query
							: EGraphCompoundExecutionLane::Operation,
						Spec.FamilyId,
						FString(TEXT("graph_compound.")) + Spec.CapabilityId,
						FString(TEXT("graph_compound.")) + Suffix + TEXT(".request.v1"),
						FString(TEXT("graph_compound.")) + Suffix + TEXT(".result.v1"),
						bQuery ? FString() : TEXT("graph_compound_identity_change"),
						bQuery ? FString() : FString(TEXT("native.")) + Spec.CapabilityId,
						FString(TEXT("readback.")) + Spec.CapabilityId,
						bQuery ? FString() : TEXT("domain_compile_refresh_settle"),
						bQuery
							? TEXT("independent_native_event_discovery")
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

		void SetError(FGraphCompoundRouteError& OutError, UE::UAI::Contract::FDiagnostic Diagnostic, const FString& SubjectId = FString())
        {
            OutError = MoveTemp(Diagnostic);
            if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = SubjectId;
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
			const FUeAgentOperationDescriptor* Descriptor =
				Bundle.Registry->FindByDescriptorId(Profile.GetDescriptorId());
			if (!Descriptor)
			{
				OutError = RouteFailure(TEXT("graph_compound_descriptor_missing"), TEXT("The graph route descriptor is missing from its registry."),
					Profile.GetDescriptorId(), TEXT("descriptor.present"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return false;
			}
			const FUeAgentContractSchema* Schema =
				Bundle.Registry->FindCompiledInputSchemaExact(
					Descriptor->GetIdentity());
			if (!Schema)
			{
				OutError = RouteFailure(TEXT("graph_compound_schema_missing"), TEXT("The graph route descriptor has no compiled input schema."),
					Profile.GetDescriptorId(), TEXT("input_schema.present"),
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
							TEXT("graph_compound.gc015.schema"),
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
					TEXT("graph_compound_input_invalid"),
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
			UE::UAI::Contract::FDiagnostic Error;
			if (!CanonicalJson::TryFromJsonValue(
					JsonValue, Resources, OutValue, Error))
			{
				SetError(OutError, Error, Subject);
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
				OutError = Error;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Subject;
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

		bool TryNormalizeAssetCreationTarget(const FString& Input, const TCHAR* InputPointer,
			FString& OutPackageName, FGraphCompoundRouteError& OutError)
		{
			const FString Identity = Input.TrimStartAndEnd();
			OutPackageName = FPackageName::ObjectPathToPackageName(Identity);
			FText Reason;
			if (!FPackageName::IsValidLongPackageName(OutPackageName, true, &Reason))
			{
				OutError = RouteFailure(TEXT("graph_compound_asset_target_invalid"),
					Reason.ToString(), Identity, InputPointer == FString(TEXT("/target_ref")) ? TEXT("target_ref") : TEXT("asset_target"),
					MakeShared<FJsonValueString>(OutPackageName),
					MakeShared<FJsonValueString>(TEXT("valid long package name")));
				OutError.SourceKind = TEXT("command_input");
				OutError.Source.JsonPath = InputPointer;
				return false;
			}
			if (Identity != OutPackageName
				&& Identity != OutPackageName + TEXT(".")
					+ FPackageName::GetLongPackageAssetName(OutPackageName))
			{
				const TArray<TSharedPtr<FJsonValue>> Expected = {
					MakeShared<FJsonValueString>(OutPackageName),
					MakeShared<FJsonValueString>(OutPackageName + TEXT(".") + FPackageName::GetLongPackageAssetName(OutPackageName))};
				OutError = RouteFailure(TEXT("graph_compound_asset_target_invalid"),
					TEXT("An asset creation target must be a package path or its matching top-level object path; subobjects and a different object name are not creation targets."),
					Identity, InputPointer == FString(TEXT("/target_ref")) ? TEXT("target_ref") : TEXT("asset_target"), MakeShared<FJsonValueString>(Identity),
					MakeShared<FJsonValueArray>(Expected));
				OutError.SourceKind = TEXT("command_input");
				OutError.Source.JsonPath = InputPointer;
				return false;
			}
			return true;
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
			FString& OutText,
			const TCHAR* Field,
			FGraphCompoundRouteError& OutError)
		{
			const FString Original = Text;
			Text.TrimStartAndEndInline();
			if (Text.IsEmpty())
			{
				OutText.Reset();
				if (!bAllowEmpty)
				{
					OutError = InputFailure(TEXT("graph_compound_niagara_module_ref_invalid"),
						TEXT("The required module reference is empty after trimming."), FString(), Field,
						MakeShared<FJsonValueString>(Original), MakeShared<FJsonValueString>(TEXT("module:<non-zero GUID>")));
				}
				return bAllowEmpty;
			}
			if (!Text.StartsWith(TEXT("module:"), ESearchCase::IgnoreCase))
			{
				OutText.Reset();
				OutError = InputFailure(TEXT("graph_compound_niagara_module_ref_invalid"),
					TEXT("The module reference must begin with module:."), FString(), Field,
					MakeShared<FJsonValueString>(Original), MakeShared<FJsonValueString>(TEXT("module:<non-zero GUID>")));
				return false;
			}
			FString GuidText;
			if (!TryNormalizeGuidText(Text.RightChop(7), GuidText))
			{
				OutText.Reset();
				OutError = InputFailure(TEXT("graph_compound_niagara_module_ref_invalid"),
					TEXT("The module reference suffix is not a valid non-zero GUID."), FString(), Field,
					MakeShared<FJsonValueString>(Original), MakeShared<FJsonValueString>(TEXT("module:<non-zero GUID>")));
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
				OutError = RouteFailure(TEXT("graph_compound_niagara_selector_kind_invalid"),
					TEXT("The route input kind does not support a Niagara mutation selector."),
					Profile.GetCapabilityId(), TEXT("typed_input_kind"),
					MakeShared<FJsonValueNumber>(static_cast<int32>(InputKind)),
					MakeShared<FJsonValueString>(TEXT("NiagaraStackReorder, NiagaraStackIssueFix, NiagaraCompoundInput or NiagaraRendererEventParameter")));
				return false;
			}

			FString EmitterHandleId = Object.GetStringField(TEXT("emitter_handle_id")).TrimStartAndEnd();
			FString NormalizedEmitterId;
			if (TryNormalizeGuidText(EmitterHandleId, NormalizedEmitterId))
			{
				EmitterHandleId = MoveTemp(NormalizedEmitterId);
			}
			if (EmitterHandleId.IsEmpty())
			{
				OutError = InputFailure(TEXT("graph_compound_niagara_emitter_handle_invalid"),
					TEXT("emitter_handle_id is empty after trimming."), Profile.GetCapabilityId(),
					TEXT("emitter_handle_id"), Object.TryGetField(TEXT("emitter_handle_id")),
					MakeShared<FJsonValueString>(TEXT("non-empty emitter handle identifier")));
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
					&& (!TryNormalizeModuleRef(ItemRef, false, ItemRef, TEXT("item_ref"), OutError)
						|| !TryNormalizeModuleRef(
							PredecessorRef, true, PredecessorRef, TEXT("predecessor_ref"), OutError)))
				|| (InputKind == EGraphCompoundTypedInputKind::NiagaraCompoundInput
					&& !TryNormalizeModuleRef(ModuleRef, false, ModuleRef, TEXT("module_ref"), OutError)))
			{
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Profile.GetCapabilityId();
				return false;
			}

			FString RendererRef = OptionalString(TEXT("renderer_ref"));
			if (InputKind
					== EGraphCompoundTypedInputKind::NiagaraRendererEventParameter
				&& (!RendererRef.StartsWith(
						TEXT("renderer:"), ESearchCase::IgnoreCase)
					|| RendererRef.Len() <= 9))
			{
				OutError = InputFailure(TEXT("graph_compound_niagara_renderer_ref_invalid"),
					!RendererRef.StartsWith(TEXT("renderer:"), ESearchCase::IgnoreCase)
						? TEXT("renderer_ref must begin with renderer:.")
						: TEXT("renderer_ref must include an identifier after renderer:."),
					Profile.GetCapabilityId(), TEXT("renderer_ref"), MakeShared<FJsonValueString>(RendererRef),
					MakeShared<FJsonValueString>(TEXT("renderer:<identifier>")));
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
				OutError = OutSelector.GetValidationDiagnostic();
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
		FGraphCompoundExecutionContract InExecutionContract)
		: CapabilityId(MoveTemp(InCapabilityId))
		, DescriptorId(MoveTemp(InDescriptorId))
		, DomainId(MoveTemp(InDomainId))
		, FamilyId(MoveTemp(InFamilyId))
		, RouteKind(InRouteKind)
		, TypedInputKind(InTypedInputKind)
		, TargetKind(MoveTemp(InTargetKind))
		, ExecutionContract(MoveTemp(InExecutionContract))
	{
		for (FString* Value : {
			&CapabilityId, &DescriptorId, &DomainId, &FamilyId, &TargetKind})
		{
			Value->TrimStartAndEndInline();
		}
		const bool bKindValid =
			(RouteKind == EGraphCompoundDomainRouteKind::Query
				&& TypedInputKind == EGraphCompoundTypedInputKind::None)
			|| (RouteKind == EGraphCompoundDomainRouteKind::TypedOperation
				&& TypedInputKind != EGraphCompoundTypedInputKind::None);
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
		else if (!bKindValid)
		{
			const TSharedRef<FJsonObject> Actual = MakeShared<FJsonObject>();
			Actual->SetNumberField(TEXT("route_kind"), static_cast<int32>(RouteKind));
			Actual->SetNumberField(TEXT("typed_input_kind"), static_cast<int32>(TypedInputKind));
			ValidationDiagnostic = RouteFailure(TEXT("graph_compound_binding_profile_invalid"),
				TEXT("The route kind and typed input kind are incompatible."),
				CapabilityId, TEXT("route_kind"), MakeShared<FJsonValueObject>(Actual),
				MakeShared<FJsonValueString>(TEXT("Query requires None; TypedOperation requires a non-None input kind")));
		}
		else if (!ExecutionContract.IsValid())
		{
			ValidationDiagnostic = FDiagnostic::WrapFailure(
				TEXT("graph_compound_binding_profile_invalid"),
				ExecutionContract.GetValidationDiagnostic(), TEXT("graph_compound"),
				TEXT("The graph route execution contract is invalid."));
		}
	}

	const FString& FGraphCompoundDomainRouteProfile::GetCapabilityId() const { return CapabilityId; }
	const FString& FGraphCompoundDomainRouteProfile::GetDescriptorId() const { return DescriptorId; }
	const FString& FGraphCompoundDomainRouteProfile::GetDomainId() const { return DomainId; }
	const FString& FGraphCompoundDomainRouteProfile::GetFamilyId() const { return FamilyId; }
	EGraphCompoundDomainRouteKind FGraphCompoundDomainRouteProfile::GetRouteKind() const { return RouteKind; }
	EGraphCompoundTypedInputKind FGraphCompoundDomainRouteProfile::GetTypedInputKind() const { return TypedInputKind; }
	const FString& FGraphCompoundDomainRouteProfile::GetTargetKind() const { return TargetKind; }
	const FGraphCompoundExecutionContract& FGraphCompoundDomainRouteProfile::GetExecutionContract() const { return ExecutionContract; }
	bool FGraphCompoundDomainRouteProfile::IsValid() const { return ValidationDiagnostic.IsEmpty(); }
	const FDiagnostic& FGraphCompoundDomainRouteProfile::GetValidationDiagnostic() const { return ValidationDiagnostic; }

	bool TryGetBlueprintMaterialGraphRouteProfiles(
		const TArray<FGraphCompoundDomainRouteProfile>*& OutProfiles, FDiagnostic& OutError)
	{
		OutProfiles = nullptr;
		OutError = RouteBundle().Error;
		if (!OutError.IsEmpty()) return false;
		OutProfiles = &RouteBundle().Profiles;
		return true;
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutRegistry = RouteBundle().Registry;
		OutError = RouteBundle().Error;
		return OutRegistry.IsValid() && OutError.IsEmpty();
	}

	FGraphCompoundEventQueryRequest::FGraphCompoundEventQueryRequest()
		: FGraphCompoundEventQueryRequest(FString(), FString())
	{
	}

	FGraphCompoundEventQueryRequest::FGraphCompoundEventQueryRequest(
		FString InTargetRef,
		FString InOwnerItemRef)
		: FGraphCompoundEventQueryRequest(
			MoveTemp(InTargetRef), MoveTemp(InOwnerItemRef), FString(),
			false, false, true)
	{
	}

	FGraphCompoundEventQueryRequest::FGraphCompoundEventQueryRequest(
		FString InTargetRef,
		FString InOwnerItemRef,
		FString InQuery,
		const bool bInOnlyUnbound,
		const bool bInIncludeHidden,
		const bool bInCompileBeforeQuery)
		: TargetRef(MoveTemp(InTargetRef))
		, OwnerItemRef(MoveTemp(InOwnerItemRef))
		, Query(MoveTemp(InQuery))
		, bOnlyUnbound(bInOnlyUnbound)
		, bIncludeHidden(bInIncludeHidden)
		, bCompileBeforeQuery(bInCompileBeforeQuery)
	{
		TargetRef.TrimStartAndEndInline();
		OwnerItemRef.TrimStartAndEndInline();
		Query.TrimStartAndEndInline();
		if (TargetRef.IsEmpty() || OwnerItemRef.IsEmpty())
		{
			ValidationDiagnostic = InputFailure(TEXT("graph_compound_event_query_invalid"),
				TargetRef.IsEmpty() ? TEXT("The event query target reference is empty after trimming.")
					: TEXT("The event query owner item reference is empty after trimming."),
				TargetRef, TargetRef.IsEmpty() ? TEXT("target_ref") : TEXT("owner_item_ref"),
				MakeShared<FJsonValueString>(TargetRef.IsEmpty() ? TargetRef : OwnerItemRef),
				MakeShared<FJsonValueString>(TEXT("non-empty reference")));
		}
	}

	const FString& FGraphCompoundEventQueryRequest::GetTargetRef() const { return TargetRef; }
	const FString& FGraphCompoundEventQueryRequest::GetOwnerItemRef() const { return OwnerItemRef; }
	const FString& FGraphCompoundEventQueryRequest::GetQuery() const { return Query; }
	bool FGraphCompoundEventQueryRequest::IsOnlyUnbound() const { return bOnlyUnbound; }
	bool FGraphCompoundEventQueryRequest::IsIncludeHidden() const { return bIncludeHidden; }
	bool FGraphCompoundEventQueryRequest::IsCompileBeforeQuery() const { return bCompileBeforeQuery; }
	bool FGraphCompoundEventQueryRequest::IsValid() const { return ValidationDiagnostic.IsEmpty(); }
	const FDiagnostic& FGraphCompoundEventQueryRequest::GetValidationDiagnostic() const { return ValidationDiagnostic; }

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
			OutError = RouteFailure(TEXT("graph_compound_route_kind_mismatch"),
				TEXT("The graph route kind does not match this request decoder."),
				Profile.GetCapabilityId(), TEXT("route_kind"),
				MakeShared<FJsonValueNumber>(static_cast<int32>(Profile.GetRouteKind())),
				MakeShared<FJsonValueNumber>(static_cast<int32>(EGraphCompoundDomainRouteKind::Query)));
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
				MoveTemp(Query), bOnlyUnbound,
				bIncludeHidden, bCompileBeforeQuery);
		}
		else
		{
			OutRequest = FGraphCompoundEventQueryRequest(
				Object->GetStringField(TEXT("target_ref")),
				Object->GetStringField(TEXT("owner_item_ref")));
		}
		OutError = OutRequest.GetValidationDiagnostic();
		if (!OutError.IsEmpty()
			&& Profile.GetCapabilityId() == TEXT("blueprint.component.events.discover")
			&& OutError.PropertyPath == TEXT("target_ref"))
		{
			OutError.Source.JsonPath = TEXT("/asset_path");
		}
		return OutError.IsEmpty();
	}

	FGraphCompoundAssetCreateRequest::FGraphCompoundAssetCreateRequest()
		: FGraphCompoundAssetCreateRequest(EGraphCompoundTypedInputKind::None, FString(), FString(), FString(), FString(), FString(), FString(), FString())
	{
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
		auto RejectField = [this](const TCHAR* Field, const FString& Value, const bool bExpectedEmpty)
		{
			if (Value.IsEmpty() == bExpectedEmpty) return false;
			ValidationDiagnostic = InputFailure(TEXT("graph_compound_asset_create_invalid"),
				FString::Printf(TEXT("Asset creation field %s must be %s after trimming for this input kind."),
					Field, bExpectedEmpty ? TEXT("empty") : TEXT("non-empty")),
				TargetRef, Field, MakeShared<FJsonValueString>(Value),
				MakeShared<FJsonValueString>(bExpectedEmpty ? TEXT("empty string") : TEXT("non-empty string")));
			return true;
		};
		if (RejectField(TEXT("asset_name"), AssetName, false)
			|| RejectField(TEXT("package_path"), PackagePath, false)) return;
		const FString ExpectedTarget = PackagePath + TEXT("/") + AssetName;
		if (TargetRef != ExpectedTarget)
		{
			ValidationDiagnostic = RouteFailure(TEXT("graph_compound_asset_create_invalid"),
				TEXT("The normalized asset target does not equal package_path plus asset_name."),
				TargetRef, TEXT("target_ref"), MakeShared<FJsonValueString>(TargetRef),
				MakeShared<FJsonValueString>(ExpectedTarget));
			return;
		}
		switch (InputKind)
		{
		case EGraphCompoundTypedInputKind::BlueprintAssetCreate:
			if (RejectField(TEXT("parent_class_ref"), ParentClassRef, false)
				|| RejectField(TEXT("parent_material_ref"), ParentMaterialRef, true)
				|| RejectField(TEXT("source_ref"), SourceRef, true)) return;
			break;
		case EGraphCompoundTypedInputKind::UmgAssetCreate:
			if (RejectField(TEXT("material_kind"), MaterialKind, true)
				|| RejectField(TEXT("parent_class_ref"), ParentClassRef, true)
				|| RejectField(TEXT("parent_material_ref"), ParentMaterialRef, true)
				|| RejectField(TEXT("source_ref"), SourceRef, true)) return;
			break;
		case EGraphCompoundTypedInputKind::MaterialAssetCreate:
			if (MaterialKind != TEXT("material") && MaterialKind != TEXT("material_function"))
			{
				ValidationDiagnostic = InputFailure(TEXT("graph_compound_asset_create_invalid"),
					TEXT("material_kind must be material or material_function."),
					TargetRef, TEXT("material_kind"), MakeShared<FJsonValueString>(MaterialKind),
					MakeShared<FJsonValueString>(TEXT("material or material_function")));
				return;
			}
			if (RejectField(TEXT("parent_class_ref"), ParentClassRef, true)
				|| RejectField(TEXT("parent_material_ref"), ParentMaterialRef, true)
				|| RejectField(TEXT("source_ref"), SourceRef, true)) return;
			break;
		case EGraphCompoundTypedInputKind::MaterialInstanceAssetCreate:
			if (RejectField(TEXT("parent_class_ref"), ParentClassRef, true)
				|| RejectField(TEXT("parent_material_ref"), ParentMaterialRef, false)
				|| RejectField(TEXT("source_ref"), SourceRef, true)) return;
			break;
		case EGraphCompoundTypedInputKind::NiagaraEmitterAssetCreate:
		case EGraphCompoundTypedInputKind::NiagaraSystemAssetCreate:
		case EGraphCompoundTypedInputKind::NiagaraAssetDuplicate:
			if (RejectField(TEXT("parent_class_ref"), ParentClassRef, true)
				|| RejectField(TEXT("parent_material_ref"), ParentMaterialRef, true)
				|| RejectField(TEXT("source_ref"), SourceRef, false)) return;
			break;
		default:
			ValidationDiagnostic = RouteFailure(TEXT("graph_compound_asset_create_invalid"),
				TEXT("The typed input kind is not an asset creation or duplication kind."),
				TargetRef, TEXT("input_kind"), MakeShared<FJsonValueNumber>(static_cast<int32>(InputKind)),
				MakeShared<FJsonValueString>(TEXT("Blueprint/Umg/Material/MaterialInstance/Niagara asset creation or Niagara duplication")));
			break;
		}
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

	bool FGraphCompoundAssetCreateRequest::IsValid() const { return ValidationDiagnostic.IsEmpty(); }
	const FDiagnostic& FGraphCompoundAssetCreateRequest::GetValidationDiagnostic() const { return ValidationDiagnostic; }

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
			OutError = RouteFailure(TEXT("graph_compound_route_kind_mismatch"),
				TEXT("The graph route kind does not match this request decoder."),
				Profile.GetCapabilityId(), TEXT("route_kind"),
				MakeShared<FJsonValueNumber>(static_cast<int32>(Profile.GetRouteKind())),
				MakeShared<FJsonValueNumber>(static_cast<int32>(EGraphCompoundDomainRouteKind::TypedOperation)));
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
			OutError = RouteFailure(TEXT("graph_compound_asset_create_route_mismatch"),
				TEXT("The graph route input kind does not describe asset creation or duplication."),
				Profile.GetCapabilityId(), TEXT("typed_input_kind"),
				MakeShared<FJsonValueNumber>(static_cast<int32>(InputKind)),
				MakeShared<FJsonValueString>(TEXT("Blueprint/Umg/Material/MaterialInstance/Niagara asset creation or Niagara duplication")));
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
		const FString TargetIdentity = (bDuplicate
			? Object->GetStringField(TEXT("target_ref"))
			: AssetTargetRef(*Object)).TrimStartAndEnd();
		FString TargetRef;
		if (!TryNormalizeAssetCreationTarget(TargetIdentity,
			bDuplicate ? TEXT("/target_ref") : TEXT("/"), TargetRef, OutError))
		{
			return false;
		}
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
			OutError = OutRequest.GetValidationDiagnostic();
			return false;
		}
		return true;
	}

	FGraphCompoundNiagaraMutationSelector::FGraphCompoundNiagaraMutationSelector()
		: FGraphCompoundNiagaraMutationSelector(EGraphCompoundTypedInputKind::None, FString(), FString(), FString(), FString(), FString(), FString(), FString(), FString(), FString(), FString(), FString(), FString())
	{
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
		if (!IsNiagaraMutationKind(InputKind))
		{
			ValidationDiagnostic = RouteFailure(TEXT("graph_compound_niagara_selector_kind_invalid"),
				TEXT("The selector input kind is not a supported Niagara mutation."),
				TargetRef, TEXT("input_kind"), MakeShared<FJsonValueNumber>(static_cast<int32>(InputKind)),
				MakeShared<FJsonValueString>(TEXT("NiagaraStackReorder, NiagaraStackIssueFix, NiagaraCompoundInput or NiagaraRendererEventParameter")));
			return;
		}
		auto Require = [this](const TCHAR* Field, const FString& Value)
		{
			if (!Value.IsEmpty()) return false;
			ValidationDiagnostic = InputFailure(TEXT("graph_compound_niagara_selector_invalid"),
				FString::Printf(TEXT("Niagara selector field %s is empty after trimming."), Field),
				TargetRef, Field, MakeShared<FJsonValueString>(Value),
				MakeShared<FJsonValueString>(TEXT("non-empty string")));
			return true;
		};
		if (Require(TEXT("target_ref"), TargetRef) || Require(TEXT("emitter_handle_id"), EmitterHandleId)) return;
		switch (InputKind)
		{
		case EGraphCompoundTypedInputKind::NiagaraStackReorder:
			if (Require(TEXT("stage_ref"), StageRef) || Require(TEXT("item_ref"), ItemRef)) return;
			break;
		case EGraphCompoundTypedInputKind::NiagaraStackIssueFix:
			if (Require(TEXT("issue_id"), IssueId) || Require(TEXT("fix_id"), FixId)) return;
			break;
		case EGraphCompoundTypedInputKind::NiagaraCompoundInput:
			if (Require(TEXT("stage_ref"), StageRef) || Require(TEXT("module_ref"), ModuleRef)
				|| Require(TEXT("input_name"), InputName)) return;
			break;
		case EGraphCompoundTypedInputKind::NiagaraRendererEventParameter:
			if (Require(TEXT("renderer_ref"), RendererRef) || Require(TEXT("parameter_name"), ParameterName)
				|| Require(TEXT("type_ref"), TypeRef)) return;
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
	bool FGraphCompoundNiagaraMutationSelector::IsValid() const { return ValidationDiagnostic.IsEmpty(); }
	const FDiagnostic& FGraphCompoundNiagaraMutationSelector::GetValidationDiagnostic() const { return ValidationDiagnostic; }

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
			const bool bWrongRoute = Profile.GetRouteKind() != EGraphCompoundDomainRouteKind::TypedOperation;
			OutError = RouteFailure(TEXT("graph_compound_niagara_selector_kind_invalid"),
				bWrongRoute ? TEXT("Niagara mutation selector decoding requires a typed operation route.")
					: TEXT("The route input kind does not support a Niagara mutation selector."),
				Profile.GetCapabilityId(), bWrongRoute ? TEXT("route_kind") : TEXT("typed_input_kind"),
				MakeShared<FJsonValueNumber>(bWrongRoute ? static_cast<int32>(Profile.GetRouteKind())
					: static_cast<int32>(Profile.GetTypedInputKind())),
				bWrongRoute ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(static_cast<int32>(EGraphCompoundDomainRouteKind::TypedOperation)))
					: TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(TEXT("NiagaraStackReorder, NiagaraStackIssueFix, NiagaraCompoundInput or NiagaraRendererEventParameter"))));
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
			OutError = RouteFailure(TEXT("graph_compound_route_kind_mismatch"),
				TEXT("This graph route cannot decode a typed operation."), Profile.GetCapabilityId(), TEXT("route_kind"),
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
		const TArray<FString> Authority = {
			Profile.GetDomainId() + TEXT(".write")};
		const FString Evidence =
			TEXT("request:") + Profile.GetCapabilityId();
		TArray<FGraphCompoundCanonicalEdit> Edits;
		TMap<FString, FString> InputPointers;
		FString InputTargetRef;
		Object->TryGetStringField(TEXT("target_ref"), InputTargetRef);

		auto FailInput = [&](const TCHAR* Code, const TCHAR* Reason,
			const FString& Field, const FString& Pointer,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
			const int32 InputIndex = INDEX_NONE)
		{
			OutError = RouteFailure(Code, Reason, InputTargetRef, Field, MoveTemp(Actual), MoveTemp(Expected));
			OutError.SourceKind = TEXT("command_input");
			OutError.Source.JsonPath = Pointer;
			if (InputIndex != INDEX_NONE) OutError.OperationIndex = InputIndex;
			OutError.TargetPath = InputTargetRef;
			OutError.ActualValuePresent = OutError.ActualValue.IsValid();
			return false;
		};

		auto AddStringEdit = [&] (
			const FString& SemanticId,
			const FString& OwnerId,
			const EGraphCompoundElementKind ElementKind,
			const EChangeIntent Intent,
			const FString& Text, const FString& SourcePointer, const bool bGeneratedValue,
			TArray<FUeAgentCanonicalIdentity> Dependencies = {})
		{
			InputPointers.Add(SemanticId, SourcePointer);
			FUeAgentCanonicalValuePtr Value;
			if (!TryCanonicalString(
					Text, Resources, Value, OutError, SemanticId))
			{
				if (bGeneratedValue) MarkGraphCompoundNativeDiagnostic(OutError);
				else BindGraphCompoundInputValueDiagnostic(OutError, SourcePointer, TEXT("/value"));
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
			FString TargetRef;
			if (!TryNormalizeAssetCreationTarget(AssetTargetRef(*Object), TEXT("/"), TargetRef, OutError))
				return false;
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
					Definition, TEXT("/"), true,
					MoveTemp(Dependencies)))
			{
				return false;
			}
			break;
		}
		case EGraphCompoundTypedInputKind::GraphEdits:
		{
			const FString TargetRef = Object->GetStringField(TEXT("target_ref"));
			const TArray<TSharedPtr<FJsonValue>>& InputEdits = Object->GetArrayField(TEXT("edits"));
			for (int32 InputIndex = 0; InputIndex < InputEdits.Num(); ++InputIndex)
			{
				const TSharedPtr<FJsonValue>& EditValue = InputEdits[InputIndex];
				const FString InputPointer = TEXT("/edits/") + ::LexToString(InputIndex);
				const TSharedPtr<FJsonObject> Edit = EditValue->AsObject();
				if (!Edit.IsValid())
					return FailInput(TEXT("graph_compound_edit_invalid"), TEXT("A graph edit must be an object."),
						TEXT("edits"), InputPointer, EditValue, MakeShared<FJsonValueString>(TEXT("object")), InputIndex);
				EGraphCompoundElementKind ElementKind =
					EGraphCompoundElementKind::Item;
				EChangeIntent Intent = EChangeIntent::Update;
				if (!TryElementKind(Edit->GetStringField(TEXT("element_kind")), ElementKind))
					return FailInput(TEXT("graph_compound_edit_kind_invalid"),
						TEXT("The graph edit element kind is not supported."),
						TEXT("element_kind"), InputPointer + TEXT("/element_kind"),
						MakeShared<FJsonValueString>(Edit->GetStringField(TEXT("element_kind"))),
						MakeShared<FJsonValueString>(TEXT("target, structure, item, pin, relation, property, input, hierarchy, order, binding, dependency")), InputIndex);
				if (!TryIntent(Edit->GetStringField(TEXT("intent")), Intent))
					return FailInput(TEXT("graph_compound_edit_kind_invalid"),
						TEXT("The graph edit intent is not supported."), TEXT("intent"), InputPointer + TEXT("/intent"),
						MakeShared<FJsonValueString>(Edit->GetStringField(TEXT("intent"))),
						MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
							MakeShared<FJsonValueString>(TEXT("create")), MakeShared<FJsonValueString>(TEXT("update")),
							MakeShared<FJsonValueString>(TEXT("replace")), MakeShared<FJsonValueString>(TEXT("delete"))}), InputIndex);
				const FString SemanticId = Edit->GetStringField(TEXT("identity"));
				InputPointers.Add(SemanticId, InputPointer);
				FUeAgentCanonicalValuePtr Proposed;
				const TSharedPtr<FJsonValue>* ProposedJson =
					Edit->Values.Find(TEXT("value"));
				if (Intent != EChangeIntent::Delete)
				{
					if (!ProposedJson)
						return FailInput(TEXT("graph_compound_edit_value_missing"),
							TEXT("A non-delete graph edit requires a value."), TEXT("value"),
							InputPointer + TEXT("/value"), MakeShared<FJsonValueBoolean>(false),
							MakeShared<FJsonValueBoolean>(true), InputIndex);
					if (!TryCanonical(*ProposedJson, Resources, Proposed, OutError, SemanticId))
					{
						BindGraphCompoundInputValueDiagnostic(OutError, InputPointer + TEXT("/value"));
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
			const TArray<TSharedPtr<FJsonValue>>& Parameters = Object->GetArrayField(TEXT("parameters"));
			for (int32 InputIndex = 0; InputIndex < Parameters.Num(); ++InputIndex)
			{
				const TSharedPtr<FJsonValue>& ParameterValue = Parameters[InputIndex];
				const FString InputPointer = TEXT("/parameters/") + ::LexToString(InputIndex);
				const TSharedPtr<FJsonObject> Parameter = ParameterValue->AsObject();
				const FString SemanticId = FString::Printf(
					TEXT("property:%s:%s:%d:%s"),
					*Parameter->GetStringField(TEXT("name")),
					*Parameter->GetStringField(TEXT("association")),
					static_cast<int32>(Parameter->GetIntegerField(TEXT("index"))),
					*Parameter->GetStringField(TEXT("type")));
				InputPointers.Add(SemanticId, InputPointer);
				FUeAgentCanonicalValuePtr Proposed;
				if (!TryCanonical(
						Parameter->Values.FindChecked(TEXT("value")),
						Resources, Proposed, OutError, SemanticId))
				{
					BindGraphCompoundInputValueDiagnostic(OutError, InputPointer + TEXT("/value"));
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
					SetError(OutError, StateError, SemanticId);
					MarkGraphCompoundNativeDiagnostic(OutError);
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
			FString TargetRef;
			if (!TryNormalizeAssetCreationTarget(Object->GetStringField(TEXT("target_ref")), TEXT("/target_ref"), TargetRef, OutError))
				return false;
			const FUeAgentCanonicalIdentity SourceIdentity = Identity(
				TEXT("target:") + SourceRef, Profile.GetTargetKind(),
				SourceRef, Generation);
			if (!AddStringEdit(
					TEXT("target:") + TargetRef, TargetRef,
					EGraphCompoundElementKind::Target,
					EChangeIntent::Create, SourceRef, TEXT("/source_ref"), false, {SourceIdentity}))
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
					Selector.GetPredecessorRef(), TEXT("/predecessor_ref"), false))
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
					TEXT("resolved"), TEXT("/"), true))
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
			InputPointers.Add(SemanticId, TEXT("/value"));
			FUeAgentCanonicalValuePtr Proposed;
			if (!TryCanonical(
					Object->Values.FindChecked(TEXT("value")),
					Resources, Proposed, OutError, SemanticId))
			{
				BindGraphCompoundInputValueDiagnostic(OutError, TEXT("/value"));
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
					EChangeIntent::Update, TEXT("requested"), TEXT("/"), true))
			{
				return false;
			}
			break;
		}
		default:
			OutError = RouteFailure(TEXT("graph_compound_typed_input_kind_invalid"),
				TEXT("The Blueprint/Material typed operation input kind is not supported."), Profile.GetCapabilityId(), TEXT("input_kind"),
				MakeShared<FJsonValueNumber>(static_cast<int32>(Profile.GetTypedInputKind())),
				MakeShared<FJsonValueString>(TEXT("a supported Blueprint/Material typed operation input kind")));
			return false;
		}

		OutPayload = FGraphCompoundCanonicalPayload(
			GetGraphCompoundCanonicalSchemaId(), MoveTemp(Edits), MoveTemp(InputPointers));
		if (OutPayload.GetEdits().IsEmpty())
			return FailInput(TEXT("graph_compound_typed_operation_empty"),
				TEXT("The typed operation contains no edits."), TEXT("edits"), TEXT("/"),
				MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("at least one edit")));
		return true;
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
			const bool bWrongRoute = Profile.GetRouteKind() != EGraphCompoundDomainRouteKind::TypedOperation;
			OutError = RouteFailure(TEXT("graph_compound_typed_target_kind_invalid"),
				bWrongRoute ? TEXT("This route cannot decode a typed operation target.") : TEXT("This typed operation does not use a direct target_ref selector."),
				Profile.GetCapabilityId(), bWrongRoute ? TEXT("route_kind") : TEXT("input_kind"),
				MakeShared<FJsonValueNumber>(bWrongRoute ? static_cast<int32>(Profile.GetRouteKind()) : static_cast<int32>(Profile.GetTypedInputKind())),
				MakeShared<FJsonValueString>(bWrongRoute ? TEXT("TypedOperation") : TEXT("GraphEdits, MaterialInstanceParameters, NiagaraCompileRefreshSettle")));
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
			OutError = InputFailure(TEXT("graph_compound_typed_target_missing"),
				TEXT("target_ref must be a non-empty string after trimming."), Profile.GetCapabilityId(), TEXT("target_ref"),
				Object->Values.FindRef(TEXT("target_ref")), MakeShared<FJsonValueString>(TEXT("non-empty string")));
			OutError.ActualValuePresent = Object->HasField(TEXT("target_ref"));
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
			const FUeAgentDescriptorIdentity& Expected = Profile.GetExecutionContract().GetDescriptorIdentity();
			OutError = RouteFailure(TEXT("graph_compound_descriptor_identity_mismatch"),
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
			Payload.LocateDiagnostic(CanonicalError);
			SetError(OutError, CanonicalError, CanonicalError.SubjectId);
			return false;
		}
		return OutDraft.IsSet();
	}
}

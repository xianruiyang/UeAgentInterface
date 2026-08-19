// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Niagara/Authoring/UeAgentNiagaraAuthoringDescriptors.h"

#include "NiagaraEmitter.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticRegistry.h"

namespace UeAgentNiagaraAuthoring
{
	namespace
	{
		bool RegisterAssetIdentity(
			const TCHAR* ModelId,
			const TCHAR* TargetKind,
			FString& OutError)
		{
			UeAgentAuthoringIdentity::FModelDescriptor Model;
			Model.ModelId = ModelId;
			Model.TargetKind = TargetKind;
			Model.Selectors = {
				{ TEXT("asset_path"), 100, false },
				{ TEXT("object_path"), 90, false },
			};
			Model.KeyPatterns = {
				{ TEXT("asset"), 100, { TEXT("asset_path") } },
				{ TEXT("object"), 90, { TEXT("object_path") } },
			};
			UeAgentAuthoringIdentity::ERegistrationResult Result;
			return UeAgentAuthoringIdentity::FRegistry::Get().RegisterModel(Model, Result, OutError);
		}

		bool RegisterStructuredIdentity(
			const TCHAR* ModelId,
			const TCHAR* TargetKind,
			const TArray<UeAgentAuthoringIdentity::FSelectorDescriptor>& Selectors,
			const TArray<UeAgentAuthoringIdentity::FKeyPattern>& Patterns,
			FString& OutError)
		{
			UeAgentAuthoringIdentity::FModelDescriptor Model;
			Model.ModelId = ModelId;
			Model.TargetKind = TargetKind;
			Model.Selectors = Selectors;
			Model.KeyPatterns = Patterns;
			UeAgentAuthoringIdentity::ERegistrationResult Result;
			return UeAgentAuthoringIdentity::FRegistry::Get().RegisterModel(Model, Result, OutError);
		}

		bool RegisterSemantic(
			const TCHAR* SemanticId,
			const TCHAR* TargetKind,
			const TCHAR* IdentityModel,
			const UeAgentAuthoringSemanticRegistry::EDefaultSourceKind DefaultSource,
			const TCHAR* DefaultAccessor,
			const TArray<FString>& Capabilities,
			FString& OutError)
		{
			UeAgentAuthoringSemanticRegistry::FSemanticDescriptor Semantic;
			Semantic.SemanticId = SemanticId;
			Semantic.DomainId = DomainId;
			Semantic.TargetKind = TargetKind;
			Semantic.ValueType = TEXT("object");
			Semantic.OwnerModel = TEXT("niagara_owner");
			Semantic.IdentityModel = IdentityModel;
			Semantic.StateAccessor = StateAccessorId;
			Semantic.WriteCapability = UeAgentAuthoringSemanticRegistry::EWriteCapability::DomainMutation;
			Semantic.ConstraintSource = TEXT("niagara.schema");
			Semantic.AvailabilitySource = TEXT("niagara.state");
			Semantic.DefaultSourceKind = DefaultSource;
			Semantic.DefaultAccessor = DefaultAccessor;
			Semantic.Comparator = TEXT("uai.compare.canonical_value");
			Semantic.CapabilityTags = Capabilities;
			UeAgentAuthoringSemanticRegistry::ERegistrationResult Result;
			return UeAgentAuthoringSemanticRegistry::FRegistry::Get().RegisterSemantic(Semantic, Result, OutError);
		}
	}

	bool RegisterDescriptors(FString& OutError)
	{
		if (!RegisterAssetIdentity(TEXT("niagara_asset_identity"), TEXT("niagara_asset"), OutError)
			|| !RegisterAssetIdentity(TEXT("niagara_system_identity"), TEXT("niagara_system"), OutError)
			|| !RegisterAssetIdentity(TEXT("niagara_emitter_identity"), TEXT("niagara_emitter"), OutError)
			|| !RegisterAssetIdentity(TEXT("niagara_script_identity"), TEXT("niagara_script"), OutError))
		{
			return false;
		}

		const TArray<UeAgentAuthoringIdentity::FSelectorDescriptor> StackSelectors = {
			{ TEXT("asset_path"), 100, false },
			{ TEXT("stage_owner_id"), 98, false },
			{ TEXT("emitter_handle_id"), 95, false },
			{ TEXT("emitter_name"), 90, false },
			{ TEXT("usage"), 80, false },
			{ TEXT("usage_id"), 70, false },
		};
		if (!RegisterStructuredIdentity(
			TEXT("niagara_stack_entry_identity"),
			TEXT("niagara_stack_entry"),
			StackSelectors,
			{
				{ TEXT("stage_owner"), 120, { TEXT("asset_path"), TEXT("stage_owner_id"), TEXT("usage"), TEXT("usage_id") } },
			},
			OutError))
		{
			return false;
		}

		const TArray<UeAgentAuthoringIdentity::FSelectorDescriptor> EmitterOwnerSelectors = {
			{ TEXT("asset_path"), 100, false },
			{ TEXT("emitter_owner_id"), 98, false },
			{ TEXT("emitter_handle_id"), 95, false },
			{ TEXT("emitter_name"), 90, false },
		};
		if (!RegisterStructuredIdentity(
			TEXT("niagara_emitter_owner_identity"),
			TEXT("niagara_emitter_owner"),
			EmitterOwnerSelectors,
			{
				{ TEXT("emitter_owner"), 120, { TEXT("asset_path"), TEXT("emitter_owner_id") } },
			},
			OutError))
		{
			return false;
		}
		TArray<UeAgentAuthoringIdentity::FSelectorDescriptor> EmitterPropertySelectors = EmitterOwnerSelectors;
		EmitterPropertySelectors.Add({ TEXT("property_path"), 110, true });
		if (!RegisterStructuredIdentity(
			TEXT("niagara_emitter_property_identity"),
			TEXT("niagara_emitter_property"),
			EmitterPropertySelectors,
			{
				{ TEXT("emitter_property"), 140, {
					TEXT("asset_path"), TEXT("emitter_owner_id"), TEXT("property_path") } },
			},
			OutError))
		{
			return false;
		}
		TArray<UeAgentAuthoringIdentity::FSelectorDescriptor> RendererSelectors = EmitterOwnerSelectors;
		RendererSelectors.Append({
			{ TEXT("renderer_name"), 100, true },
			{ TEXT("renderer_class"), 70, false },
		});
		if (!RegisterStructuredIdentity(
			TEXT("niagara_renderer_identity"),
			TEXT("niagara_renderer"),
			RendererSelectors,
			{
				{ TEXT("emitter_renderer"), 120, { TEXT("asset_path"), TEXT("emitter_owner_id"), TEXT("renderer_name") } },
			},
			OutError))
		{
			return false;
		}

		TArray<UeAgentAuthoringIdentity::FSelectorDescriptor> EventSelectors = EmitterOwnerSelectors;
		EventSelectors.Append({ { TEXT("script_usage_id"), 110, true } });
		if (!RegisterStructuredIdentity(
			TEXT("niagara_event_handler_identity"),
			TEXT("niagara_event_handler"),
			EventSelectors,
			{
				{ TEXT("emitter_event_handler"), 130, { TEXT("asset_path"), TEXT("emitter_owner_id"), TEXT("script_usage_id") } },
			},
			OutError))
		{
			return false;
		}

		if (!RegisterStructuredIdentity(
			TEXT("niagara_user_parameter_identity"),
			TEXT("niagara_user_parameter"),
			{
				{ TEXT("asset_path"), 100, false },
				{ TEXT("parameter_name"), 110, true },
			},
			{
				{ TEXT("system_user_parameter"), 130, { TEXT("asset_path"), TEXT("parameter_name") } },
			},
			OutError))
		{
			return false;
		}

		TArray<UeAgentAuthoringIdentity::FSelectorDescriptor> ModuleSelectors = StackSelectors;
		ModuleSelectors.Append({
			{ TEXT("module_guid"), 110, false },
			{ TEXT("script_asset_path"), 60, false },
			{ TEXT("occurrence"), 50, true },
		});
		if (!RegisterStructuredIdentity(
			TEXT("niagara_module_identity"),
			TEXT("niagara_module"),
			ModuleSelectors,
			{
				{ TEXT("stage_module_guid"), 140, { TEXT("asset_path"), TEXT("stage_owner_id"), TEXT("module_guid") } },
			},
			OutError))
		{
			return false;
		}

		TArray<UeAgentAuthoringIdentity::FSelectorDescriptor> DynamicSelectors = ModuleSelectors;
		DynamicSelectors.Append({
			{ TEXT("input_name"), 100, true },
			{ TEXT("dynamic_input_guid"), 120, false },
		});
		if (!RegisterStructuredIdentity(
			TEXT("niagara_dynamic_input_identity"),
			TEXT("niagara_dynamic_input"),
			DynamicSelectors,
			{
				{ TEXT("stage_dynamic_binding"), 150, { TEXT("asset_path"), TEXT("stage_owner_id"), TEXT("module_guid"), TEXT("input_name"), TEXT("dynamic_input_guid") } },
			},
			OutError))
		{
			return false;
		}

		UeAgentAuthoringSemanticRegistry::FDomainDescriptor Domain;
		Domain.DomainId = DomainId;
		Domain.MutationProviderId = ApplyProviderId;
		Domain.TargetKinds = {
			TEXT("niagara_asset"), TEXT("niagara_system"), TEXT("niagara_emitter"), TEXT("niagara_script"),
			TEXT("niagara_stack_entry"), TEXT("niagara_module"), TEXT("niagara_dynamic_input"),
			TEXT("niagara_emitter_owner"), TEXT("niagara_emitter_property"), TEXT("niagara_renderer"), TEXT("niagara_event_handler"),
			TEXT("niagara_user_parameter")
		};
		Domain.CapabilityTags = {
			TEXT("asset_lifecycle"), TEXT("stack"), TEXT("module"), TEXT("dynamic_input"),
			TEXT("renderer"), TEXT("event_handler"), TEXT("user_parameter"), TEXT("script_resolution")
		};
		UeAgentAuthoringSemanticRegistry::ERegistrationResult DomainResult;
		if (!UeAgentAuthoringSemanticRegistry::FRegistry::Get().RegisterDomain(Domain, DomainResult, OutError))
		{
			return false;
		}

		UeAgentAuthoringSemanticRegistry::FSemanticDescriptor AssetSemantic;
		AssetSemantic.SemanticId = AssetSemanticId;
		AssetSemantic.DomainId = DomainId;
		AssetSemantic.TargetKind = TEXT("niagara_asset");
		AssetSemantic.ValueType = TEXT("object");
		AssetSemantic.OwnerModel = TEXT("niagara_owner");
		AssetSemantic.IdentityModel = TEXT("niagara_asset_identity");
		AssetSemantic.StateAccessor = StateAccessorId;
		AssetSemantic.WriteCapability = UeAgentAuthoringSemanticRegistry::EWriteCapability::ReadOnly;
		AssetSemantic.ConstraintSource = TEXT("niagara.schema");
		AssetSemantic.AvailabilitySource = TEXT("niagara.state");
		AssetSemantic.Comparator = TEXT("uai.compare.canonical_value");
		AssetSemantic.CapabilityTags = { TEXT("asset"), TEXT("identity"), TEXT("export") };
		UeAgentAuthoringSemanticRegistry::ERegistrationResult AssetSemanticResult;
		if (!UeAgentAuthoringSemanticRegistry::FRegistry::Get().RegisterSemantic(
			AssetSemantic, AssetSemanticResult, OutError))
		{
			return false;
		}

		UeAgentAuthoringSemanticRegistry::FSemanticDescriptor EmitterPropertySemantic;
		EmitterPropertySemantic.SemanticId = EmitterPropertySemanticId;
		EmitterPropertySemantic.DomainId = DomainId;
		EmitterPropertySemantic.TargetKind = TEXT("niagara_emitter_property");
		EmitterPropertySemantic.ValueType = TEXT("*");
		EmitterPropertySemantic.OwnerModel = TEXT("niagara_owner");
		EmitterPropertySemantic.IdentityModel = TEXT("niagara_emitter_property_identity");
		EmitterPropertySemantic.StateAccessor = TEXT("core.property");
		EmitterPropertySemantic.WriteCapability = UeAgentAuthoringSemanticRegistry::EWriteCapability::PropertyAssignment;
		EmitterPropertySemantic.DefaultSourceKind = UeAgentAuthoringSemanticRegistry::EDefaultSourceKind::ExistingInstance;
		EmitterPropertySemantic.Comparator = TEXT("uai.compare.canonical_value");
		EmitterPropertySemantic.CapabilityTags = { TEXT("emitter"), TEXT("property"), TEXT("editable") };
		UeAgentAuthoringSemanticRegistry::ERegistrationResult EmitterPropertySemanticResult;
		if (!UeAgentAuthoringSemanticRegistry::FRegistry::Get().RegisterSemantic(
				EmitterPropertySemantic, EmitterPropertySemanticResult, OutError))
		{
			return false;
		}

		return RegisterSemantic(
			SystemSemanticId, TEXT("niagara_system"), TEXT("niagara_system_identity"),
			UeAgentAuthoringSemanticRegistry::EDefaultSourceKind::Factory, FactoryDefaultProviderId,
			{ TEXT("asset"), TEXT("system"), TEXT("editable") }, OutError)
			&& RegisterSemantic(
				EmitterSemanticId, TEXT("niagara_emitter"), TEXT("niagara_emitter_identity"),
				UeAgentAuthoringSemanticRegistry::EDefaultSourceKind::Factory, FactoryDefaultProviderId,
				{ TEXT("asset"), TEXT("emitter"), TEXT("editable") }, OutError)
			&& RegisterSemantic(
				ScriptSemanticId, TEXT("niagara_script"), TEXT("niagara_script_identity"),
				UeAgentAuthoringSemanticRegistry::EDefaultSourceKind::DomainNative, DomainNativeDefaultProviderId,
				{ TEXT("asset"), TEXT("script"), TEXT("editable") }, OutError)
			&& RegisterSemantic(
				StackSemanticId, TEXT("niagara_stack_entry"), TEXT("niagara_stack_entry_identity"),
				UeAgentAuthoringSemanticRegistry::EDefaultSourceKind::Schema, SchemaDefaultProviderId,
				{ TEXT("stack"), TEXT("usage") }, OutError)
			&& RegisterSemantic(
				ModuleSemanticId, TEXT("niagara_module"), TEXT("niagara_module_identity"),
				UeAgentAuthoringSemanticRegistry::EDefaultSourceKind::DomainNative, DomainNativeDefaultProviderId,
				{ TEXT("stack"), TEXT("module"), TEXT("editable") }, OutError)
			&& RegisterSemantic(
				DynamicInputSemanticId, TEXT("niagara_dynamic_input"), TEXT("niagara_dynamic_input_identity"),
				UeAgentAuthoringSemanticRegistry::EDefaultSourceKind::DomainNative, DomainNativeDefaultProviderId,
				{ TEXT("module_input"), TEXT("dynamic_input"), TEXT("editable") }, OutError)
			&& RegisterSemantic(
				EmitterOwnerSemanticId, TEXT("niagara_emitter_owner"), TEXT("niagara_emitter_owner_identity"),
				UeAgentAuthoringSemanticRegistry::EDefaultSourceKind::DomainNative, DomainNativeDefaultProviderId,
				{ TEXT("emitter"), TEXT("owner"), TEXT("identity") }, OutError)
			&& RegisterSemantic(
				RendererSemanticId, TEXT("niagara_renderer"), TEXT("niagara_renderer_identity"),
				UeAgentAuthoringSemanticRegistry::EDefaultSourceKind::DomainNative, DomainNativeDefaultProviderId,
				{ TEXT("emitter"), TEXT("renderer"), TEXT("editable") }, OutError)
			&& RegisterSemantic(
				EventHandlerSemanticId, TEXT("niagara_event_handler"), TEXT("niagara_event_handler_identity"),
				UeAgentAuthoringSemanticRegistry::EDefaultSourceKind::DomainNative, DomainNativeDefaultProviderId,
				{ TEXT("emitter"), TEXT("event_handler"), TEXT("editable") }, OutError)
			&& RegisterSemantic(
				UserParameterSemanticId, TEXT("niagara_user_parameter"), TEXT("niagara_user_parameter_identity"),
				UeAgentAuthoringSemanticRegistry::EDefaultSourceKind::DomainNative, DomainNativeDefaultProviderId,
				{ TEXT("system"), TEXT("user_parameter"), TEXT("editable") }, OutError);
	}

	FString GetAssetKind(const UObject* Object)
	{
		if (Cast<UNiagaraSystem>(Object)) return TEXT("system");
		if (Cast<UNiagaraEmitter>(Object)) return TEXT("emitter");
		if (Cast<UNiagaraScript>(Object)) return TEXT("script");
		return FString();
	}

	FString GetAssetSemanticId(const UObject* Object)
	{
		if (Cast<UNiagaraSystem>(Object)) return SystemSemanticId;
		if (Cast<UNiagaraEmitter>(Object)) return EmitterSemanticId;
		if (Cast<UNiagaraScript>(Object)) return ScriptSemanticId;
		return FString();
	}

	FString GetAssetIdentityModelId(const UObject* Object)
	{
		if (Cast<UNiagaraSystem>(Object)) return TEXT("niagara_system_identity");
		if (Cast<UNiagaraEmitter>(Object)) return TEXT("niagara_emitter_identity");
		if (Cast<UNiagaraScript>(Object)) return TEXT("niagara_script_identity");
		return FString();
	}
}

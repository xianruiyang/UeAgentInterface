// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Material/Authoring/UeAgentMaterialAuthoringDescriptors.h"

#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstance.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticRegistry.h"

namespace UeAgentMaterialAuthoring
{
	namespace
	{
		bool RegisterAssetIdentity(const TCHAR* ModelId, const TCHAR* TargetKind, UE::UAI::Contract::FDiagnostic& OutError)
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

		bool RegisterScopedIdentity(
			const TCHAR* ModelId,
			const TCHAR* TargetKind,
			const TArray<UeAgentAuthoringIdentity::FSelectorDescriptor>& Selectors,
			const TArray<FString>& KeySelectors,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			UeAgentAuthoringIdentity::FModelDescriptor Model;
			Model.ModelId = ModelId;
			Model.TargetKind = TargetKind;
			Model.Selectors = Selectors;
			UeAgentAuthoringIdentity::FKeyPattern Pattern;
			Pattern.PatternId = TEXT("scoped");
			Pattern.Priority = 100;
			Pattern.Selectors = KeySelectors;
			Model.KeyPatterns = { MoveTemp(Pattern) };
			UeAgentAuthoringIdentity::ERegistrationResult Result;
			return UeAgentAuthoringIdentity::FRegistry::Get().RegisterModel(Model, Result, OutError);
		}

		bool RegisterSemantic(
			const TCHAR* SemanticId,
			const TCHAR* TargetKind,
			const TCHAR* IdentityModelId,
			UE::UAI::Contract::FDiagnostic& OutError,
			const TCHAR* Comparator = TEXT("uai.compare.canonical_value"))
		{
			UeAgentAuthoringSemanticRegistry::FSemanticDescriptor Semantic;
			Semantic.SemanticId = SemanticId;
			Semantic.DomainId = DomainId;
			Semantic.TargetKind = TargetKind;
			Semantic.ValueType = TEXT("object");
			Semantic.OwnerModel = TEXT("material_owner");
			Semantic.IdentityModel = IdentityModelId;
			Semantic.StateAccessor = StateAccessorId;
			Semantic.WriteCapability = UeAgentAuthoringSemanticRegistry::EWriteCapability::DomainMutation;
			Semantic.ConstraintSource = TEXT("material.schema");
			Semantic.AvailabilitySource = StateAccessorId;
			Semantic.DefaultSourceKind = UeAgentAuthoringSemanticRegistry::EDefaultSourceKind::Factory;
			Semantic.DefaultAccessor = FactoryDefaultProviderId;
			Semantic.Comparator = Comparator;
			Semantic.CapabilityTags = { TEXT("asset"), TEXT("graph"), TEXT("expression"), TEXT("editable") };
			UeAgentAuthoringSemanticRegistry::ERegistrationResult Result;
			return UeAgentAuthoringSemanticRegistry::FRegistry::Get().RegisterSemantic(
				Semantic, Result, OutError);
		}

		bool RegisterExportSemantic(
			const TCHAR* SemanticId,
			const TCHAR* TargetKind,
			const TCHAR* IdentityModelId,
			const TCHAR* AccessorId,
			const TCHAR* ValueType,
			const UeAgentAuthoringSemanticRegistry::EWriteCapability WriteCapability,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			UeAgentAuthoringSemanticRegistry::FSemanticDescriptor Semantic;
			Semantic.SemanticId = SemanticId;
			Semantic.DomainId = DomainId;
			Semantic.TargetKind = TargetKind;
			Semantic.ValueType = ValueType;
			Semantic.OwnerModel = TEXT("material_export_owner");
			Semantic.IdentityModel = IdentityModelId;
			Semantic.StateAccessor = AccessorId;
			Semantic.WriteCapability = WriteCapability;
			Semantic.AvailabilitySource = AccessorId;
			Semantic.Comparator = TEXT("uai.compare.canonical_value");
			Semantic.CapabilityTags = { TEXT("export"), TEXT("snapshot"), TEXT("material") };
			UeAgentAuthoringSemanticRegistry::ERegistrationResult Result;
			return UeAgentAuthoringSemanticRegistry::FRegistry::Get().RegisterSemantic(
				Semantic, Result, OutError);
		}
	}

	bool RegisterDescriptors(UE::UAI::Contract::FDiagnostic& OutError)
	{
		using UeAgentAuthoringIdentity::FSelectorDescriptor;
		if (!RegisterAssetIdentity(MaterialIdentityModelId, TEXT("material_asset"), OutError)
			|| !RegisterAssetIdentity(MaterialFunctionIdentityModelId, TEXT("material_function"), OutError)
			|| !RegisterAssetIdentity(MaterialInstanceIdentityModelId, TEXT("material_instance"), OutError)
			|| !RegisterScopedIdentity(MaterialInstanceLayersIdentityModelId, TEXT("material_instance_layers"),
				{{TEXT("asset_path"), 100, false}, {TEXT("selector"), 110, false}},
				{TEXT("asset_path"), TEXT("selector")}, OutError)
			|| !RegisterAssetIdentity(MaterialGraphIdentityModelId, TEXT("material_graph"), OutError)
			|| !RegisterAssetIdentity(MaterialRootNodeIdentityModelId, TEXT("material_root_node"), OutError)
			|| !RegisterScopedIdentity(
				MaterialInstanceParameterIdentityModelId,
				TEXT("material_instance_parameter"),
				{ FSelectorDescriptor{ TEXT("asset_path"), 100, false },
					FSelectorDescriptor{ TEXT("parameter_kind"), 105, false },
					FSelectorDescriptor{ TEXT("parameter_name"), 110, false },
					FSelectorDescriptor{ TEXT("association"), 115, false },
					FSelectorDescriptor{ TEXT("parameter_index"), 120, false } },
				{ TEXT("asset_path"), TEXT("parameter_kind"), TEXT("parameter_name"),
					TEXT("association"), TEXT("parameter_index") },
				OutError)
			|| !RegisterScopedIdentity(
				MaterialExpressionIdentityModelId,
				TEXT("material_expression"),
				{ FSelectorDescriptor{ TEXT("asset_path"), 100, false },
					FSelectorDescriptor{ TEXT("expression_guid"), 110, false } },
				{ TEXT("asset_path"), TEXT("expression_guid") },
				OutError)
			|| !RegisterScopedIdentity(
				MaterialExpressionPropertyIdentityModelId,
				TEXT("material_expression_property"),
				{ FSelectorDescriptor{ TEXT("asset_path"), 100, false },
					FSelectorDescriptor{ TEXT("expression_guid"), 110, false },
					FSelectorDescriptor{ TEXT("property_path"), 120, false } },
				{ TEXT("asset_path"), TEXT("expression_guid"), TEXT("property_path") },
				OutError)
			|| !RegisterScopedIdentity(
				MaterialEdgeIdentityModelId,
				TEXT("material_edge"),
				{ FSelectorDescriptor{ TEXT("asset_path"), 100, false },
					FSelectorDescriptor{ TEXT("edge_key"), 110, false } },
				{ TEXT("asset_path"), TEXT("edge_key") },
				OutError)
			|| !RegisterScopedIdentity(
				MaterialRootInputIdentityModelId,
				TEXT("material_root_input"),
				{ FSelectorDescriptor{ TEXT("asset_path"), 100, false },
					FSelectorDescriptor{ TEXT("material_property"), 110, false } },
				{ TEXT("asset_path"), TEXT("material_property") },
				OutError))
		{
			return false;
		}

		UeAgentAuthoringSemanticRegistry::FDomainDescriptor Domain;
		Domain.DomainId = DomainId;
		Domain.MutationProviderId = ApplyProviderId;
		Domain.TargetKinds = {
			TEXT("material_asset"), TEXT("material_function"), TEXT("material_instance"),
			TEXT("material_graph"),
			TEXT("material_instance_layers"),
			TEXT("material_root_node"),
			TEXT("material_instance_parameter"), TEXT("material_expression"),
			TEXT("material_expression_property"), TEXT("material_edge"), TEXT("material_root_input")
		};
		Domain.CapabilityTags = {
			TEXT("asset_lifecycle"), TEXT("graph"), TEXT("expression"),
			TEXT("connection"), TEXT("class_resolution"), TEXT("function_resolution")
		};
		UeAgentAuthoringSemanticRegistry::ERegistrationResult DomainResult;
		if (!UeAgentAuthoringSemanticRegistry::FRegistry::Get().RegisterDomain(
			Domain, DomainResult, OutError))
		{
			return false;
		}

		return RegisterSemantic(
			MaterialSemanticId, TEXT("material_asset"), MaterialIdentityModelId, OutError, TEXT("uai.compare.graph_int32_position"))
			&& RegisterSemantic(
				MaterialFunctionSemanticId,
				TEXT("material_function"),
				MaterialFunctionIdentityModelId,
				OutError, TEXT("uai.compare.graph_int32_position"))
			&& RegisterSemantic(
				MaterialInstanceSemanticId,
				TEXT("material_instance"),
				MaterialInstanceIdentityModelId,
				OutError)
			&& RegisterSemantic(
				ExportGraphSemanticId,
				TEXT("material_graph"),
				MaterialGraphIdentityModelId,
				OutError, TEXT("uai.compare.graph_int32_position"))
			&& RegisterSemantic(
				ExportRootNodeSemanticId,
				TEXT("material_root_node"),
				MaterialRootNodeIdentityModelId,
				OutError, TEXT("uai.compare.graph_int32_position"))
			&& RegisterExportSemantic(
				ExportTargetSemanticId, TEXT("material_asset"), MaterialIdentityModelId,
				StateAccessorId, TEXT("object"),
				UeAgentAuthoringSemanticRegistry::EWriteCapability::ReadOnly, OutError)
			&& RegisterExportSemantic(
				ExportFunctionTargetSemanticId, TEXT("material_function"), MaterialFunctionIdentityModelId,
				StateAccessorId, TEXT("object"),
				UeAgentAuthoringSemanticRegistry::EWriteCapability::ReadOnly, OutError)
			&& RegisterExportSemantic(
				ExportInstanceTargetSemanticId, TEXT("material_instance"), MaterialInstanceIdentityModelId,
				StateAccessorId, TEXT("object"),
				UeAgentAuthoringSemanticRegistry::EWriteCapability::ReadOnly, OutError)
			&& RegisterExportSemantic(
				ExportInstanceParameterSemanticId, TEXT("material_instance_parameter"),
				MaterialInstanceParameterIdentityModelId, StateAccessorId, TEXT("object"),
				UeAgentAuthoringSemanticRegistry::EWriteCapability::DomainMutation, OutError)
			&& RegisterExportSemantic(
				ExportInstanceLayersSemanticId, TEXT("material_instance_layers"),
				MaterialInstanceLayersIdentityModelId, StateAccessorId, TEXT("object"),
				UeAgentAuthoringSemanticRegistry::EWriteCapability::DomainMutation, OutError)
			&& RegisterExportSemantic(
				ExportExpressionSemanticId, TEXT("material_expression"), MaterialExpressionIdentityModelId,
				StateAccessorId, TEXT("object"),
				UeAgentAuthoringSemanticRegistry::EWriteCapability::ReadOnly, OutError)
			&& RegisterExportSemantic(
				ExportExpressionPropertySemanticId,
				TEXT("material_expression_property"), MaterialExpressionPropertyIdentityModelId,
				TEXT("core.property"), TEXT("*"),
				UeAgentAuthoringSemanticRegistry::EWriteCapability::PropertyAssignment, OutError)
			&& RegisterExportSemantic(
				ExportEdgeSemanticId, TEXT("material_edge"), MaterialEdgeIdentityModelId,
				StateAccessorId, TEXT("object"),
				UeAgentAuthoringSemanticRegistry::EWriteCapability::ReadOnly, OutError)
			&& RegisterExportSemantic(
				ExportRootInputSemanticId, TEXT("material_root_input"), MaterialRootInputIdentityModelId,
				StateAccessorId, TEXT("object"),
				UeAgentAuthoringSemanticRegistry::EWriteCapability::ReadOnly, OutError)
			&& RegisterExportSemantic(
				ExportFunctionSettingsSemanticId, TEXT("material_function"), MaterialFunctionIdentityModelId,
				StateAccessorId, TEXT("object"),
				UeAgentAuthoringSemanticRegistry::EWriteCapability::ReadOnly, OutError);
	}

	FString GetAssetKind(const UObject* Object)
	{
		if (Cast<UMaterial>(Object)) return TEXT("material");
		if (Cast<UMaterialFunction>(Object)) return TEXT("material_function");
		if (Cast<UMaterialInstance>(Object)) return TEXT("material_instance");
		return FString();
	}

	FString GetAssetSemanticId(const UObject* Object)
	{
		if (Cast<UMaterial>(Object)) return MaterialSemanticId;
		if (Cast<UMaterialFunction>(Object)) return MaterialFunctionSemanticId;
		if (Cast<UMaterialInstance>(Object)) return MaterialInstanceSemanticId;
		return FString();
	}

	FString GetAssetIdentityModelId(const UObject* Object)
	{
		if (Cast<UMaterial>(Object)) return MaterialIdentityModelId;
		if (Cast<UMaterialFunction>(Object)) return MaterialFunctionIdentityModelId;
		if (Cast<UMaterialInstance>(Object)) return MaterialInstanceIdentityModelId;
		return FString();
	}

	FString GetAssetTargetKind(const UObject* Object)
	{
		if (Cast<UMaterial>(Object)) return TEXT("material_asset");
		if (Cast<UMaterialFunction>(Object)) return TEXT("material_function");
		if (Cast<UMaterialInstance>(Object)) return TEXT("material_instance");
		return FString();
	}
}

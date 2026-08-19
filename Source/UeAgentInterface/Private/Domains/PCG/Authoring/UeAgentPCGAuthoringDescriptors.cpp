// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/PCG/Authoring/UeAgentPCGAuthoringDescriptors.h"

#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticRegistry.h"

namespace UeAgentPCGAuthoring
{
	namespace
	{
		bool RegisterIdentity(
			const TCHAR* ModelId,
			const TCHAR* TargetKind,
			const TArray<FString>& SelectorNames,
			FString& OutError)
		{
			UeAgentAuthoringIdentity::FModelDescriptor Model;
			Model.ModelId = ModelId;
			Model.TargetKind = TargetKind;
			int32 Priority = 100;
			for (const FString& SelectorName : SelectorNames)
			{
				Model.Selectors.Add({ SelectorName, Priority++, false });
			}
			Model.KeyPatterns = { { TEXT("scoped"), 100, SelectorNames } };
			UeAgentAuthoringIdentity::ERegistrationResult Result;
			return UeAgentAuthoringIdentity::FRegistry::Get().RegisterModel(Model, Result, OutError);
		}

		bool RegisterSemantic(
			const TCHAR* SemanticId,
			const TCHAR* TargetKind,
			const TCHAR* IdentityModelId,
			const TCHAR* AccessorId,
			const TCHAR* ValueType,
			const UeAgentAuthoringSemanticRegistry::EWriteCapability WriteCapability,
			const bool bFactoryDefault,
			FString& OutError)
		{
			UeAgentAuthoringSemanticRegistry::FSemanticDescriptor Semantic;
			Semantic.SemanticId = SemanticId;
			Semantic.DomainId = DomainId;
			Semantic.TargetKind = TargetKind;
			Semantic.ValueType = ValueType;
			Semantic.OwnerModel = TEXT("pcg_graph_owner");
			Semantic.IdentityModel = IdentityModelId;
			Semantic.StateAccessor = AccessorId;
			Semantic.WriteCapability = WriteCapability;
			Semantic.ConstraintSource = TEXT("pcg.graph.schema");
			Semantic.AvailabilitySource = AccessorId;
			Semantic.Comparator = TEXT("uai.compare.canonical_value");
			Semantic.CapabilityTags = { TEXT("pcg"), TEXT("graph"), TEXT("snapshot") };
			if (bFactoryDefault)
			{
				Semantic.DefaultSourceKind = UeAgentAuthoringSemanticRegistry::EDefaultSourceKind::Factory;
				Semantic.DefaultAccessor = FactoryDefaultProviderId;
			}
			UeAgentAuthoringSemanticRegistry::ERegistrationResult Result;
			return UeAgentAuthoringSemanticRegistry::FRegistry::Get().RegisterSemantic(
				Semantic, Result, OutError);
		}
	}

	bool RegisterDescriptors(FString& OutError)
	{
		if (!RegisterIdentity(GraphIdentityModelId, TEXT("pcg_graph"), { TEXT("asset_path") }, OutError)
			|| !RegisterIdentity(NodeIdentityModelId, TEXT("pcg_node"),
				{ TEXT("asset_path"), TEXT("node_id") }, OutError)
			|| !RegisterIdentity(NodePropertyIdentityModelId, TEXT("pcg_node_property"),
				{ TEXT("asset_path"), TEXT("node_id"), TEXT("property_path") }, OutError)
			|| !RegisterIdentity(PinIdentityModelId, TEXT("pcg_pin"),
				{ TEXT("asset_path"), TEXT("node_id"), TEXT("direction"), TEXT("pin_label") }, OutError)
			|| !RegisterIdentity(EdgeIdentityModelId, TEXT("pcg_edge"),
				{ TEXT("asset_path"), TEXT("edge_key") }, OutError)
			|| !RegisterIdentity(ParametersIdentityModelId, TEXT("pcg_parameters"),
				{ TEXT("asset_path") }, OutError))
		{
			return false;
		}

		UeAgentAuthoringSemanticRegistry::FDomainDescriptor Domain;
		Domain.DomainId = DomainId;
		Domain.MutationProviderId = ApplyProviderId;
		Domain.TargetKinds = {
			TEXT("pcg_graph"), TEXT("pcg_node"), TEXT("pcg_node_property"),
			TEXT("pcg_pin"), TEXT("pcg_edge"), TEXT("pcg_parameters")
		};
		Domain.CapabilityTags = {
			TEXT("asset_lifecycle"), TEXT("graph"), TEXT("node"), TEXT("pin"),
			TEXT("connection"), TEXT("parameter_bag"), TEXT("class_resolution")
		};
		UeAgentAuthoringSemanticRegistry::ERegistrationResult DomainResult;
		if (!UeAgentAuthoringSemanticRegistry::FRegistry::Get().RegisterDomain(
			Domain, DomainResult, OutError))
		{
			return false;
		}

		using EWrite = UeAgentAuthoringSemanticRegistry::EWriteCapability;
		return RegisterSemantic(GraphSemanticId, TEXT("pcg_graph"), GraphIdentityModelId,
			StateAccessorId, TEXT("object"), EWrite::DomainMutation, true, OutError)
			&& RegisterSemantic(ExportTargetSemanticId, TEXT("pcg_graph"), GraphIdentityModelId,
				StateAccessorId, TEXT("object"), EWrite::ReadOnly, false, OutError)
			&& RegisterSemantic(ExportNodeSemanticId, TEXT("pcg_node"), NodeIdentityModelId,
				StateAccessorId, TEXT("object"), EWrite::DomainMutation, false, OutError)
			&& RegisterSemantic(ExportNodePropertySemanticId, TEXT("pcg_node_property"),
				NodePropertyIdentityModelId, TEXT("core.property"), TEXT("*"), EWrite::PropertyAssignment, false, OutError)
			&& RegisterSemantic(ExportPinSemanticId, TEXT("pcg_pin"), PinIdentityModelId,
				StateAccessorId, TEXT("object"), EWrite::ReadOnly, false, OutError)
			&& RegisterSemantic(ExportEdgeSemanticId, TEXT("pcg_edge"), EdgeIdentityModelId,
				StateAccessorId, TEXT("object"), EWrite::DomainMutation, false, OutError)
			&& RegisterSemantic(ExportParametersSemanticId, TEXT("pcg_parameters"), ParametersIdentityModelId,
				StateAccessorId, TEXT("object"), EWrite::DomainMutation, false, OutError);
	}
}

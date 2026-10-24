// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Level/Authoring/UeAgentLevelAuthoringDescriptors.h"

#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticRegistry.h"

namespace UeAgentLevelAuthoring
{
	namespace
	{
		using UeAgentAuthoringIdentity::FKeyPattern;
		using UeAgentAuthoringIdentity::FSelectorDescriptor;
		using UeAgentAuthoringSemanticRegistry::EDefaultSourceKind;
		using UeAgentAuthoringSemanticRegistry::EWriteCapability;

		bool RegisterIdentity(
			const TCHAR* ModelId,
			const TCHAR* TargetKind,
			const TArray<FSelectorDescriptor>& Selectors,
			const TArray<FKeyPattern>& Patterns,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			UeAgentAuthoringIdentity::FModelDescriptor Model;
			Model.ModelId = ModelId;
			Model.TargetKind = TargetKind;
			Model.Selectors = Selectors;
			Model.KeyPatterns = Patterns;
			UeAgentAuthoringIdentity::ERegistrationResult Result;
			return UeAgentAuthoringIdentity::FRegistry::Get().RegisterModel(
				Model, Result, OutError);
		}

		bool RegisterSemantic(
			const TCHAR* SemanticId,
			const TCHAR* TargetKind,
			const TCHAR* IdentityModelId,
			const TCHAR* AccessorId,
			const TCHAR* ValueType,
			const EWriteCapability WriteCapability,
			const EDefaultSourceKind DefaultSource,
			const TCHAR* DefaultAccessor,
			const TArray<FString>& Tags,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			UeAgentAuthoringSemanticRegistry::FSemanticDescriptor Semantic;
			Semantic.SemanticId = SemanticId;
			Semantic.DomainId = DomainId;
			Semantic.TargetKind = TargetKind;
			Semantic.ValueType = ValueType;
			Semantic.OwnerModel = TEXT("world_level_owner");
			Semantic.IdentityModel = IdentityModelId;
			Semantic.StateAccessor = AccessorId;
			Semantic.WriteCapability = WriteCapability;
			Semantic.ConstraintSource = TEXT("level.authoring.contract");
			Semantic.AvailabilitySource = AccessorId;
			Semantic.DefaultSourceKind = DefaultSource;
			Semantic.DefaultAccessor = DefaultAccessor;
			Semantic.Comparator = TEXT("uai.compare.canonical_value");
			Semantic.CapabilityTags = Tags;
			UeAgentAuthoringSemanticRegistry::ERegistrationResult Result;
			return UeAgentAuthoringSemanticRegistry::FRegistry::Get().RegisterSemantic(
				Semantic, Result, OutError);
		}
	}

	bool RegisterDescriptors(UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!RegisterIdentity(
			WorldIdentityModelId,
			TEXT("level_world"),
			{ { TEXT("world_path"), 100, false } },
			{ { TEXT("world"), 0, { TEXT("world_path") } } },
			OutError))
		{
			return false;
		}

		const TArray<FSelectorDescriptor> ActorSelectors = {
			{ TEXT("world_path"), 100, false },
			{ TEXT("actor_guid"), 140, false },
			{ TEXT("managed_id"), 130, true },
			{ TEXT("object_path"), 110, true },
			{ TEXT("actor_class"), 90, true },
		};
		const TArray<FKeyPattern> ActorPatterns = {
			{ TEXT("guid"), 0, { TEXT("world_path"), TEXT("actor_guid") } },
			{ TEXT("managed"), 100, { TEXT("world_path"), TEXT("managed_id") } },
			{ TEXT("object"), 200, { TEXT("world_path"), TEXT("object_path") } },
		};
		if (!RegisterIdentity(
			ActorIdentityModelId, TEXT("level_actor"), ActorSelectors, ActorPatterns, OutError))
		{
			return false;
		}

		const TArray<FSelectorDescriptor> ComponentSelectors = {
			{ TEXT("world_path"), 100, false },
			{ TEXT("owner_actor_guid"), 140, false },
			{ TEXT("owner_managed_id"), 130, true },
			{ TEXT("component_name"), 150, true },
			{ TEXT("component_class"), 90, true },
		};
		const TArray<FKeyPattern> ComponentPatterns = {
			{ TEXT("owner_guid_name"), 0, {
				TEXT("world_path"), TEXT("owner_actor_guid"), TEXT("component_name") } },
			{ TEXT("owner_managed_name"), 100, {
				TEXT("world_path"), TEXT("owner_managed_id"), TEXT("component_name") } },
		};
		if (!RegisterIdentity(
			ComponentIdentityModelId,
			TEXT("level_component"),
			ComponentSelectors,
			ComponentPatterns,
			OutError))
		{
			return false;
		}

		if (!RegisterIdentity(
			WorldPropertyIdentityModelId, TEXT("level_world_property"),
			{ { TEXT("world_path"), 100, false }, { TEXT("property_path"), 160, true } },
			{ { TEXT("world_property"), 0, { TEXT("world_path"), TEXT("property_path") } } },
			OutError))
		{
			return false;
		}

		TArray<FSelectorDescriptor> ActorPropertySelectors = ActorSelectors;
		ActorPropertySelectors.Add({ TEXT("property_path"), 160, true });
		if (!RegisterIdentity(
			ActorPropertyIdentityModelId,
			TEXT("level_actor_property"),
			ActorPropertySelectors,
			{
				{ TEXT("guid_property"), 0, {
					TEXT("world_path"), TEXT("actor_guid"), TEXT("property_path") } },
				{ TEXT("managed_property"), 100, {
					TEXT("world_path"), TEXT("managed_id"), TEXT("property_path") } },
			},
			OutError))
		{
			return false;
		}

		TArray<FSelectorDescriptor> ComponentPropertySelectors = ComponentSelectors;
		ComponentPropertySelectors.Add({ TEXT("property_path"), 160, true });
		if (!RegisterIdentity(
			ComponentPropertyIdentityModelId,
			TEXT("level_component_property"),
			ComponentPropertySelectors,
			{
				{ TEXT("owner_guid_name_property"), 0, {
					TEXT("world_path"), TEXT("owner_actor_guid"), TEXT("component_name"),
					TEXT("property_path") } },
				{ TEXT("owner_managed_name_property"), 100, {
					TEXT("world_path"), TEXT("owner_managed_id"), TEXT("component_name"),
					TEXT("property_path") } },
			},
			OutError))
		{
			return false;
		}

		UeAgentAuthoringSemanticRegistry::FDomainDescriptor Domain;
		Domain.DomainId = DomainId;
		Domain.MutationProviderId = ApplyProviderId;
		Domain.TargetKinds = {
			TEXT("level_world"), TEXT("level_actor"), TEXT("level_component"),
			TEXT("level_world_property"), TEXT("level_actor_property"), TEXT("level_component_property")
		};
		Domain.CapabilityTags = {
			TEXT("actor_lifecycle"), TEXT("component_lifecycle"), TEXT("world_owner"),
			TEXT("attachment"), TEXT("space_aware_transform"), TEXT("reflection")
		};
		UeAgentAuthoringSemanticRegistry::ERegistrationResult DomainResult;
		if (!UeAgentAuthoringSemanticRegistry::FRegistry::Get().RegisterDomain(
			Domain, DomainResult, OutError))
		{
			return false;
		}

		return RegisterSemantic(
				WorldStateSemanticId, TEXT("level_world"), WorldIdentityModelId,
				StateAccessorId, TEXT("object"), EWriteCapability::DomainMutation,
				EDefaultSourceKind::ExistingInstance, TEXT(""),
				{ TEXT("level"), TEXT("world"), TEXT("snapshot") }, OutError)
			&& RegisterSemantic(
				WorldTargetSemanticId, TEXT("level_world"), WorldIdentityModelId,
				StateAccessorId, TEXT("object"), EWriteCapability::ReadOnly,
				EDefaultSourceKind::None, TEXT(""),
				{ TEXT("level"), TEXT("world"), TEXT("export") }, OutError)
			&& RegisterSemantic(
				WorldPropertySemanticId, TEXT("level_world_property"),
				WorldPropertyIdentityModelId, TEXT("core.property"), TEXT("*"),
				EWriteCapability::PropertyAssignment, EDefaultSourceKind::ExistingInstance,
				TEXT(""), { TEXT("level"), TEXT("world"), TEXT("property") }, OutError)
			&& RegisterSemantic(
				ActorStateSemanticId, TEXT("level_actor"), ActorIdentityModelId,
				StateAccessorId, TEXT("object"), EWriteCapability::DomainMutation,
				EDefaultSourceKind::Factory, NativeDefaultProviderId,
				{ TEXT("level"), TEXT("actor"), TEXT("transform"), TEXT("folder") }, OutError)
			&& RegisterSemantic(
				ActorTargetSemanticId, TEXT("level_actor"), ActorIdentityModelId,
				StateAccessorId, TEXT("object"), EWriteCapability::ReadOnly,
				EDefaultSourceKind::None, TEXT(""),
				{ TEXT("level"), TEXT("actor"), TEXT("export") }, OutError)
			&& RegisterSemantic(
				ActorPropertySemanticId, TEXT("level_actor_property"),
				ActorPropertyIdentityModelId, TEXT("core.property"), TEXT("*"),
				EWriteCapability::PropertyAssignment, EDefaultSourceKind::ExistingInstance,
				TEXT(""), { TEXT("level"), TEXT("actor"), TEXT("property") }, OutError)
			&& RegisterSemantic(
				ComponentStateSemanticId, TEXT("level_component"), ComponentIdentityModelId,
				StateAccessorId, TEXT("object"), EWriteCapability::DomainMutation,
				EDefaultSourceKind::Factory, NativeDefaultProviderId,
				{ TEXT("level"), TEXT("component"), TEXT("attachment"), TEXT("transform") }, OutError)
			&& RegisterSemantic(
				ComponentTargetSemanticId, TEXT("level_component"), ComponentIdentityModelId,
				StateAccessorId, TEXT("object"), EWriteCapability::ReadOnly,
				EDefaultSourceKind::None, TEXT(""),
				{ TEXT("level"), TEXT("component"), TEXT("export") }, OutError)
			&& RegisterSemantic(
				ComponentPropertySemanticId, TEXT("level_component_property"),
				ComponentPropertyIdentityModelId, TEXT("core.property"), TEXT("*"),
				EWriteCapability::PropertyAssignment, EDefaultSourceKind::ExistingInstance,
				TEXT(""), { TEXT("level"), TEXT("component"), TEXT("property") }, OutError);
	}
}


// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/UMG/Authoring/UeAgentUmgAuthoringDescriptors.h"

#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticRegistry.h"

namespace UeAgentUmgAuthoring
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
			FString& OutError)
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
			const TArray<FString>& CapabilityTags,
			FString& OutError)
		{
			UeAgentAuthoringSemanticRegistry::FSemanticDescriptor Semantic;
			Semantic.SemanticId = SemanticId;
			Semantic.DomainId = DomainId;
			Semantic.TargetKind = TargetKind;
			Semantic.ValueType = ValueType;
			Semantic.OwnerModel = TEXT("widget_blueprint_owner");
			Semantic.IdentityModel = IdentityModelId;
			Semantic.StateAccessor = AccessorId;
			Semantic.WriteCapability = WriteCapability;
			Semantic.ConstraintSource = TEXT("umg.schema");
			Semantic.AvailabilitySource = AccessorId;
			Semantic.DefaultSourceKind = DefaultSource;
			Semantic.DefaultAccessor = DefaultAccessor;
			Semantic.Comparator = TEXT("uai.compare.canonical_value");
			Semantic.CapabilityTags = CapabilityTags;
			UeAgentAuthoringSemanticRegistry::ERegistrationResult Result;
			return UeAgentAuthoringSemanticRegistry::FRegistry::Get().RegisterSemantic(
				Semantic, Result, OutError);
		}

		bool RegisterPropertySemantic(
			const TCHAR* SemanticId,
			const TCHAR* TargetKind,
			const TCHAR* IdentityModelId,
			FString& OutError)
		{
			return RegisterSemantic(
				SemanticId,
				TargetKind,
				IdentityModelId,
				TEXT("core.property"),
				TEXT("*"),
				EWriteCapability::PropertyAssignment,
				EDefaultSourceKind::ExistingInstance,
				TEXT(""),
				{ TEXT("umg"), TEXT("property"), TEXT("snapshot") },
				OutError);
		}
	}

	bool RegisterDescriptors(FString& OutError)
	{
		const TArray<FSelectorDescriptor> AssetSelectors = {
			{ TEXT("asset_path"), 100, false },
			{ TEXT("object_path"), 90, false },
		};
		if (!RegisterIdentity(
			AssetIdentityModelId,
			TEXT("umg_asset"),
			AssetSelectors,
			{
				{ TEXT("asset"), 0, { TEXT("asset_path") } },
				{ TEXT("object"), 100, { TEXT("object_path") } },
			},
			OutError))
		{
			return false;
		}

		const TArray<FSelectorDescriptor> WidgetSelectors = {
			{ TEXT("asset_path"), 100, false },
			{ TEXT("widget_guid"), 130, false },
			{ TEXT("widget_class"), 90, false },
			{ TEXT("widget_name"), 80, true },
		};
		if (!RegisterIdentity(
			WidgetIdentityModelId,
			TEXT("umg_widget"),
			WidgetSelectors,
			{
				{ TEXT("guid"), 0, { TEXT("asset_path"), TEXT("widget_guid") } },
				{ TEXT("name_class"), 100, {
					TEXT("asset_path"), TEXT("widget_class"), TEXT("widget_name") } },
			},
			OutError))
		{
			return false;
		}

		if (!RegisterIdentity(
			SlotIdentityModelId,
			TEXT("umg_slot"),
			WidgetSelectors,
			{
				{ TEXT("guid"), 0, { TEXT("asset_path"), TEXT("widget_guid") } },
				{ TEXT("name_class"), 100, {
					TEXT("asset_path"), TEXT("widget_class"), TEXT("widget_name") } },
			},
			OutError))
		{
			return false;
		}

		const TArray<FSelectorDescriptor> BindingSelectors = {
			{ TEXT("asset_path"), 100, false },
			{ TEXT("widget_name"), 100, true },
			{ TEXT("property_name"), 110, true },
		};
		if (!RegisterIdentity(
			BindingIdentityModelId,
			TEXT("umg_binding"),
			BindingSelectors,
			{ { TEXT("binding"), 0, {
				TEXT("asset_path"), TEXT("widget_name"), TEXT("property_name") } } },
			OutError))
		{
			return false;
		}

		const TArray<FSelectorDescriptor> AnimationSelectors = {
			{ TEXT("asset_path"), 100, false },
			{ TEXT("animation_guid"), 130, false },
			{ TEXT("animation_name"), 100, true },
		};
		if (!RegisterIdentity(
			AnimationIdentityModelId,
			TEXT("umg_animation"),
			AnimationSelectors,
			{
				{ TEXT("guid"), 0, { TEXT("asset_path"), TEXT("animation_guid") } },
				{ TEXT("name"), 100, { TEXT("asset_path"), TEXT("animation_name") } },
			},
			OutError))
		{
			return false;
		}

		TArray<FSelectorDescriptor> WidgetPropertySelectors = WidgetSelectors;
		WidgetPropertySelectors.Add({ TEXT("property_path"), 150, true });
		if (!RegisterIdentity(
			WidgetPropertyIdentityModelId,
			TEXT("umg_widget_property"),
			WidgetPropertySelectors,
			{
				{ TEXT("guid_property"), 0, {
					TEXT("asset_path"), TEXT("widget_guid"), TEXT("property_path") } },
				{ TEXT("name_class_property"), 100, {
					TEXT("asset_path"), TEXT("widget_class"), TEXT("widget_name"),
					TEXT("property_path") } },
			},
			OutError))
		{
			return false;
		}

		TArray<FSelectorDescriptor> SlotPropertySelectors = WidgetSelectors;
		SlotPropertySelectors.Add({ TEXT("property_path"), 150, true });
		if (!RegisterIdentity(
			SlotPropertyIdentityModelId,
			TEXT("umg_slot_property"),
			SlotPropertySelectors,
			{
				{ TEXT("guid_property"), 0, {
					TEXT("asset_path"), TEXT("widget_guid"), TEXT("property_path") } },
				{ TEXT("name_class_property"), 100, {
					TEXT("asset_path"), TEXT("widget_class"), TEXT("widget_name"),
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
			TEXT("umg_asset"), TEXT("umg_widget"), TEXT("umg_slot"),
			TEXT("umg_binding"), TEXT("umg_animation"),
			TEXT("umg_widget_property"), TEXT("umg_slot_property")
		};
		Domain.CapabilityTags = {
			TEXT("asset_lifecycle"), TEXT("widget_tree"), TEXT("hierarchy"),
			TEXT("native_slot"), TEXT("layout"), TEXT("binding"), TEXT("animation")
		};
		UeAgentAuthoringSemanticRegistry::ERegistrationResult DomainResult;
		if (!UeAgentAuthoringSemanticRegistry::FRegistry::Get().RegisterDomain(
			Domain, DomainResult, OutError))
		{
			return false;
		}

		return RegisterSemantic(
				AssetSemanticId, TEXT("umg_asset"), AssetIdentityModelId,
				StateAccessorId, TEXT("object"), EWriteCapability::DomainMutation,
				EDefaultSourceKind::Factory, NativeDefaultProviderId,
				{ TEXT("umg"), TEXT("asset"), TEXT("editable") }, OutError)
			&& RegisterSemantic(
				ExportTargetSemanticId, TEXT("umg_asset"), AssetIdentityModelId,
				StateAccessorId, TEXT("object"), EWriteCapability::ReadOnly,
				EDefaultSourceKind::None, TEXT(""),
				{ TEXT("umg"), TEXT("asset"), TEXT("export") }, OutError)
			&& RegisterSemantic(
				ParentClassSemanticId, TEXT("umg_asset"), AssetIdentityModelId,
				StateAccessorId, TEXT("string"), EWriteCapability::DomainMutation,
				EDefaultSourceKind::ExistingInstance, TEXT(""),
				{ TEXT("umg"), TEXT("parent_class") }, OutError)
			&& RegisterSemantic(
				WidgetSemanticId, TEXT("umg_widget"), WidgetIdentityModelId,
				StateAccessorId, TEXT("object"), EWriteCapability::DomainMutation,
				EDefaultSourceKind::DomainNative, NativeDefaultProviderId,
				{ TEXT("umg"), TEXT("widget_tree"), TEXT("widget") }, OutError)
			&& RegisterSemantic(
				SlotSemanticId, TEXT("umg_slot"), SlotIdentityModelId,
				StateAccessorId, TEXT("object"), EWriteCapability::DomainMutation,
				EDefaultSourceKind::PostCreate, NativeDefaultProviderId,
				{ TEXT("umg"), TEXT("slot"), TEXT("layout") }, OutError)
			&& RegisterSemantic(
				BindingSemanticId, TEXT("umg_binding"), BindingIdentityModelId,
				StateAccessorId, TEXT("object"), EWriteCapability::DomainMutation,
				EDefaultSourceKind::None, TEXT(""),
				{ TEXT("umg"), TEXT("binding") }, OutError)
			&& RegisterSemantic(
				AnimationSemanticId, TEXT("umg_animation"), AnimationIdentityModelId,
				StateAccessorId, TEXT("object"), EWriteCapability::DomainMutation,
				EDefaultSourceKind::None, TEXT(""),
				{ TEXT("umg"), TEXT("animation") }, OutError)
			&& RegisterPropertySemantic(
				WidgetPropertySemanticId, TEXT("umg_widget_property"),
				WidgetPropertyIdentityModelId, OutError)
			&& RegisterPropertySemantic(
				SlotPropertySemanticId, TEXT("umg_slot_property"),
				SlotPropertyIdentityModelId, OutError);
	}
}

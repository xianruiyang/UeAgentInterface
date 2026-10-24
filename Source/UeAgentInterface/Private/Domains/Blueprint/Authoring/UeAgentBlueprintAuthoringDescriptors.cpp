// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringDescriptors.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringOps.h"

#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "GameFramework/Actor.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticCompare.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticRegistry.h"

namespace UeAgentBlueprintAuthoring
{
	namespace
	{
		using UeAgentAuthoringIdentity::FKeyPattern;
		using UeAgentAuthoringIdentity::FSelectorDescriptor;

		bool RegisterComponentComparator(UE::UAI::Contract::FDiagnostic& Error)
		{
			UeAgentAuthoringSemanticCompare::FComparatorDescriptor Descriptor;
			Descriptor.ComparatorId = TEXT("uai.compare.blueprint_component");
			Descriptor.ValueKinds = {TEXT("object")};
			Descriptor.CapabilityTags = {TEXT("native_reference_identity")};
			UeAgentAuthoringSemanticCompare::ERegistrationResult Result;
			return UeAgentAuthoringSemanticCompare::FComparatorRegistry::Get().RegisterComparator(
				Descriptor, [](const auto& Left, const auto& Right, bool& Equivalent, UE::UAI::Contract::FDiagnostic& CompareError)
				{
					auto Expected = Left;
					Expected.Value = UeAgentAuthoringState::CloneCanonicalValue(Left.Value);
					if (Expected.Value && Right.Value && Expected.Value->Type == EJson::Object && Right.Value->Type == EJson::Object)
					{
						const auto A = Expected.Value->AsObject(), B = Right.Value->AsObject();
						FString AParent, BParent;
						if (A->TryGetStringField(TEXT("parent"), AParent) && B->TryGetStringField(TEXT("parent"), BParent)
							&& AParent != BParent && !AParent.IsEmpty() && !BParent.IsEmpty())
						{
							if (auto* Blueprint = UeAgentBlueprintAuthoringOps::LoadBlueprintAsset(Left.TargetIdentity.GetSelector(TEXT("asset_path"))))
							{
								auto Resolve = [Blueprint](const FString& Name) -> UActorComponent*
								{
									if (auto* Node = UeAgentBlueprintAuthoringOps::FindSCSNodeByAnyName(Blueprint->SimpleConstructionScript, Name))
										return Node->ComponentTemplate;
									return UeAgentBlueprintAuthoringOps::ResolveInheritedComponent(*Blueprint, Name);
								};
								auto* Requested = Resolve(AParent);
								if (Requested && Requested == Resolve(BParent)) A->SetStringField(TEXT("parent"), BParent);
							}
						}
					}
					UeAgentAuthoringSemanticCompare::FComparator Canonical;
					if (!UeAgentAuthoringSemanticCompare::FComparatorRegistry::Get().CopyComparator(
						TEXT("uai.compare.canonical_value"), Canonical))
					{
						CompareError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("semantic_comparator_unknown"),
							TEXT("Blueprint component comparison requires the canonical value comparator, which is not registered."),
							TEXT("semantic_compare"), TEXT("uai.compare.canonical_value"));
						CompareError.SourceKind = TEXT("native_result");
						CompareError.TargetPath = Left.TargetIdentity.GetSelector(TEXT("asset_path"));
						CompareError.PropertyPath = TEXT("canonical_comparator_registered");
						CompareError.ActualValue = MakeShared<FJsonValueBoolean>(false);
						CompareError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
						return false;
					}
					return Canonical(Expected, Right, Equivalent, CompareError);
				}, Result, Error);
		}

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
			const UeAgentAuthoringSemanticRegistry::EWriteCapability WriteCapability,
			const UeAgentAuthoringSemanticRegistry::EDefaultSourceKind DefaultSource,
			const TCHAR* DefaultAccessor,
			const TArray<FString>& CapabilityTags,
			UE::UAI::Contract::FDiagnostic& OutError,
			const TCHAR* Comparator = TEXT("uai.compare.canonical_value"))
		{
			UeAgentAuthoringSemanticRegistry::FSemanticDescriptor Semantic;
			Semantic.SemanticId = SemanticId;
			Semantic.DomainId = DomainId;
			Semantic.TargetKind = TargetKind;
			Semantic.ValueType = ValueType;
			Semantic.OwnerModel = TEXT("blueprint_owner");
			Semantic.IdentityModel = IdentityModelId;
			Semantic.StateAccessor = AccessorId;
			Semantic.WriteCapability = WriteCapability;
			Semantic.ConstraintSource = TEXT("blueprint.schema");
			Semantic.AvailabilitySource = AccessorId;
			Semantic.DefaultSourceKind = DefaultSource;
			Semantic.DefaultAccessor = DefaultAccessor;
			Semantic.Comparator = Comparator;
			Semantic.CapabilityTags = CapabilityTags;
			UeAgentAuthoringSemanticRegistry::ERegistrationResult Result;
			return UeAgentAuthoringSemanticRegistry::FRegistry::Get().RegisterSemantic(
				Semantic, Result, OutError);
		}

		bool RegisterPropertySemantic(
			const TCHAR* SemanticId,
			const TCHAR* TargetKind,
			const TCHAR* IdentityModelId,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			return RegisterSemantic(
				SemanticId,
				TargetKind,
				IdentityModelId,
				TEXT("core.property"),
				TEXT("*"),
				UeAgentAuthoringSemanticRegistry::EWriteCapability::PropertyAssignment,
				UeAgentAuthoringSemanticRegistry::EDefaultSourceKind::ExistingInstance,
				TEXT(""),
				{ TEXT("blueprint"), TEXT("property"), TEXT("snapshot") },
				OutError);
		}
	}

	bool RegisterDescriptors(UE::UAI::Contract::FDiagnostic& OutError)
	{
		const TArray<FSelectorDescriptor> AssetSelectors = {
			{ TEXT("asset_path"), 100, false },
			{ TEXT("object_path"), 90, false },
		};
		if (!RegisterIdentity(
			AssetIdentityModelId,
			TEXT("blueprint_asset"),
			AssetSelectors,
			{
				{ TEXT("asset"), 0, { TEXT("asset_path") } },
				{ TEXT("object"), 100, { TEXT("object_path") } },
			},
			OutError))
		{
			return false;
		}

		const TArray<FSelectorDescriptor> InterfaceSelectors = {
			{ TEXT("asset_path"), 100, false },
			{ TEXT("interface_class"), 120, false },
		};
		if (!RegisterIdentity(
			InterfaceIdentityModelId,
			TEXT("blueprint_interface"),
			InterfaceSelectors,
			{ { TEXT("class"), 0, { TEXT("asset_path"), TEXT("interface_class") } } },
			OutError))
		{
			return false;
		}

		const TArray<FSelectorDescriptor> MemberSelectors = {
			{ TEXT("asset_path"), 100, false },
			{ TEXT("member_guid"), 120, false },
			{ TEXT("member_kind"), 90, false },
			{ TEXT("member_name"), 80, true },
		};
		if (!RegisterIdentity(
			MemberIdentityModelId,
			TEXT("blueprint_member"),
			MemberSelectors,
			{
				{ TEXT("guid"), 0, { TEXT("asset_path"), TEXT("member_guid") } },
				{ TEXT("name_kind"), 100, { TEXT("asset_path"), TEXT("member_kind"), TEXT("member_name") } },
			},
			OutError))
		{
			return false;
		}

		const TArray<FSelectorDescriptor> ComponentSelectors = {
			{ TEXT("asset_path"), 100, false },
			{ TEXT("component_guid"), 120, false },
			{ TEXT("component_kind"), 90, false },
			{ TEXT("component_name"), 80, true },
		};
		if (!RegisterIdentity(
			ComponentIdentityModelId,
			TEXT("blueprint_component"),
			ComponentSelectors,
			{
				{ TEXT("guid"), 0, { TEXT("asset_path"), TEXT("component_guid") } },
				{ TEXT("name_kind"), 100, { TEXT("asset_path"), TEXT("component_kind"), TEXT("component_name") } },
			},
			OutError))
		{
			return false;
		}

		const TArray<FSelectorDescriptor> GraphSelectors = {
			{ TEXT("asset_path"), 100, false },
			{ TEXT("graph_guid"), 120, false },
			{ TEXT("graph_kind"), 90, false },
			{ TEXT("graph_name"), 80, true },
		};
		if (!RegisterIdentity(
			GraphIdentityModelId,
			TEXT("blueprint_graph"),
			GraphSelectors,
			{
				{ TEXT("guid"), 0, { TEXT("asset_path"), TEXT("graph_guid") } },
				{ TEXT("name_kind"), 100, { TEXT("asset_path"), TEXT("graph_kind"), TEXT("graph_name") } },
			},
			OutError))
		{
			return false;
		}

		TArray<FSelectorDescriptor> NodeSelectors = GraphSelectors;
		NodeSelectors.Append({
			{ TEXT("node_guid"), 140, false },
			{ TEXT("node_kind"), 110, false },
			{ TEXT("node_name"), 100, true },
			{ TEXT("node_occurrence"), 70, true },
		});
		if (!RegisterIdentity(
			NodeIdentityModelId,
			TEXT("blueprint_node"),
			NodeSelectors,
			{
				{ TEXT("guid"), 0, { TEXT("asset_path"), TEXT("node_guid") } },
				{ TEXT("name_kind"), 100, {
					TEXT("asset_path"), TEXT("graph_kind"), TEXT("graph_name"),
					TEXT("node_kind"), TEXT("node_name"), TEXT("node_occurrence") } },
			},
			OutError))
		{
			return false;
		}

		TArray<FSelectorDescriptor> PinSelectors = NodeSelectors;
		PinSelectors.Append({
			{ TEXT("pin_guid"), 160, false },
			{ TEXT("pin_name"), 120, true },
			{ TEXT("pin_occurrence"), 30, false },
			{ TEXT("property_path"), 170, true },
		});
		if (!RegisterIdentity(
			PinDefaultIdentityModelId,
			TEXT("blueprint_pin_default"),
			PinSelectors,
			{
				{ TEXT("guid"), 0, {
					TEXT("asset_path"), TEXT("node_guid"), TEXT("pin_guid"),
					TEXT("pin_name"), TEXT("pin_occurrence") } },
				{ TEXT("name_kind"), 100, {
					TEXT("asset_path"), TEXT("graph_kind"), TEXT("graph_name"),
					TEXT("node_kind"), TEXT("node_name"), TEXT("node_occurrence"),
					TEXT("pin_name"), TEXT("pin_occurrence") } },
			},
			OutError))
		{
			return false;
		}

		TArray<FSelectorDescriptor> EdgeSelectors = GraphSelectors;
		EdgeSelectors.Add({ TEXT("edge_key"), 130, true });
		if (!RegisterIdentity(
			EdgeIdentityModelId,
			TEXT("blueprint_edge"),
			EdgeSelectors,
			{
				{ TEXT("graph_guid_edge"), 0, { TEXT("asset_path"), TEXT("graph_guid"), TEXT("edge_key") } },
				{ TEXT("graph_name_edge"), 100, {
					TEXT("asset_path"), TEXT("graph_kind"), TEXT("graph_name"), TEXT("edge_key") } },
			},
			OutError))
		{
			return false;
		}

		TArray<FSelectorDescriptor> DefaultPropertySelectors = AssetSelectors;
		DefaultPropertySelectors.Add({ TEXT("property_path"), 130, true });
		if (!RegisterIdentity(
			ClassDefaultPropertyIdentityModelId,
			TEXT("blueprint_class_default_property"),
			DefaultPropertySelectors,
			{ { TEXT("property"), 0, { TEXT("asset_path"), TEXT("property_path") } } },
			OutError))
		{
			return false;
		}

		TArray<FSelectorDescriptor> ComponentPropertySelectors = ComponentSelectors;
		ComponentPropertySelectors.Add({ TEXT("property_path"), 150, true });
		if (!RegisterIdentity(
			ComponentPropertyIdentityModelId,
			TEXT("blueprint_component_property"),
			ComponentPropertySelectors,
			{
				{ TEXT("guid_property"), 0, {
					TEXT("asset_path"), TEXT("component_guid"), TEXT("property_path") } },
				{ TEXT("name_kind_property"), 100, {
					TEXT("asset_path"), TEXT("component_kind"), TEXT("component_name"), TEXT("property_path") } },
			},
			OutError))
		{
			return false;
		}

		TArray<FSelectorDescriptor> NodePropertySelectors = NodeSelectors;
		NodePropertySelectors.Add({ TEXT("property_path"), 160, true });
		if (!RegisterIdentity(
			NodePropertyIdentityModelId,
			TEXT("blueprint_node_property"),
			NodePropertySelectors,
			{
				{ TEXT("guid_property"), 0, {
					TEXT("asset_path"), TEXT("node_guid"), TEXT("property_path") } },
				{ TEXT("name_kind_property"), 100, {
					TEXT("asset_path"), TEXT("graph_kind"), TEXT("graph_name"),
					TEXT("node_kind"), TEXT("node_name"), TEXT("node_occurrence"), TEXT("property_path") } },
			},
			OutError))
		{
			return false;
		}

		UeAgentAuthoringSemanticRegistry::FDomainDescriptor Domain;
		Domain.DomainId = DomainId;
		Domain.MutationProviderId = ApplyProviderId;
		Domain.TargetKinds = {
			TEXT("blueprint_asset"), TEXT("blueprint_interface"), TEXT("blueprint_member"), TEXT("blueprint_component"),
			TEXT("blueprint_graph"), TEXT("blueprint_node"), TEXT("blueprint_pin_default"),
			TEXT("blueprint_edge"), TEXT("blueprint_class_default_property"),
			TEXT("blueprint_component_property"), TEXT("blueprint_node_property")
		};
		Domain.CapabilityTags = {
			TEXT("asset_lifecycle"), TEXT("interface"), TEXT("member"), TEXT("scs"), TEXT("component"),
			TEXT("graph"), TEXT("node"), TEXT("pin_default"), TEXT("connection")
		};
		UeAgentAuthoringSemanticRegistry::ERegistrationResult DomainResult;
		if (!UeAgentAuthoringSemanticRegistry::FRegistry::Get().RegisterDomain(
			Domain, DomainResult, OutError))
		{
			return false;
		}

		using UeAgentAuthoringSemanticRegistry::EDefaultSourceKind;
		using UeAgentAuthoringSemanticRegistry::EWriteCapability;
		if (!RegisterComponentComparator(OutError)) return false;
		return RegisterSemantic(
				AssetSemanticId, TEXT("blueprint_asset"), AssetIdentityModelId,
				StateAccessorId, TEXT("object"), EWriteCapability::DomainMutation,
				EDefaultSourceKind::Factory, FactoryDefaultProviderId,
				{ TEXT("blueprint"), TEXT("asset"), TEXT("editable") }, OutError)
			&& RegisterSemantic(
				ExportTargetSemanticId, TEXT("blueprint_asset"), AssetIdentityModelId,
				StateAccessorId, TEXT("object"), EWriteCapability::ReadOnly,
				EDefaultSourceKind::None, TEXT(""),
				{ TEXT("blueprint"), TEXT("asset"), TEXT("export") }, OutError)
			&& RegisterSemantic(
				ParentClassSemanticId, TEXT("blueprint_asset"), AssetIdentityModelId,
				StateAccessorId, TEXT("string"), EWriteCapability::DomainMutation,
				EDefaultSourceKind::ExistingInstance, TEXT(""),
				{ TEXT("blueprint"), TEXT("parent_class") }, OutError)
			&& RegisterSemantic(
				InterfaceSemanticId, TEXT("blueprint_interface"), InterfaceIdentityModelId,
				StateAccessorId, TEXT("object"), EWriteCapability::DomainMutation,
				EDefaultSourceKind::None, TEXT(""),
				{ TEXT("blueprint"), TEXT("interface") }, OutError)
			&& RegisterSemantic(
				VariableSemanticId, TEXT("blueprint_member"), MemberIdentityModelId,
				StateAccessorId, TEXT("object"), EWriteCapability::DomainMutation,
				EDefaultSourceKind::None, TEXT(""),
				{ TEXT("blueprint"), TEXT("member"), TEXT("variable") }, OutError)
			&& RegisterSemantic(
				DelegateSemanticId, TEXT("blueprint_member"), MemberIdentityModelId,
				StateAccessorId, TEXT("object"), EWriteCapability::DomainMutation,
				EDefaultSourceKind::None, TEXT(""),
				{ TEXT("blueprint"), TEXT("member"), TEXT("delegate") }, OutError)
			&& RegisterSemantic(
				ComponentSemanticId, TEXT("blueprint_component"), ComponentIdentityModelId,
				StateAccessorId, TEXT("object"), EWriteCapability::DomainMutation,
				EDefaultSourceKind::None, TEXT(""),
				{ TEXT("blueprint"), TEXT("scs"), TEXT("component") }, OutError, TEXT("uai.compare.blueprint_component"))
			&& RegisterSemantic(
				GraphSemanticId, TEXT("blueprint_graph"), GraphIdentityModelId,
				StateAccessorId, TEXT("object"), EWriteCapability::DomainMutation,
				EDefaultSourceKind::None, TEXT(""),
				{ TEXT("blueprint"), TEXT("graph") }, OutError, TEXT("uai.compare.graph_rounded_int32_position"))
			&& RegisterSemantic(
				NodeSemanticId, TEXT("blueprint_node"), NodeIdentityModelId,
				StateAccessorId, TEXT("object"), EWriteCapability::DomainMutation,
				EDefaultSourceKind::None, TEXT(""),
				{ TEXT("blueprint"), TEXT("graph"), TEXT("node") }, OutError, TEXT("uai.compare.graph_rounded_int32_position"))
			&& RegisterSemantic(
				PinDefaultSemanticId, TEXT("blueprint_pin_default"), PinDefaultIdentityModelId,
				StateAccessorId, TEXT("*"), EWriteCapability::DomainMutation,
				EDefaultSourceKind::ExistingInstance, TEXT(""),
				{ TEXT("blueprint"), TEXT("graph"), TEXT("pin_default") }, OutError)
			&& RegisterSemantic(
				EdgeSemanticId, TEXT("blueprint_edge"), EdgeIdentityModelId,
				StateAccessorId, TEXT("object"), EWriteCapability::DomainMutation,
				EDefaultSourceKind::None, TEXT(""),
				{ TEXT("blueprint"), TEXT("graph"), TEXT("edge") }, OutError)
			&& RegisterPropertySemantic(
				ClassDefaultPropertySemanticId, TEXT("blueprint_class_default_property"),
				ClassDefaultPropertyIdentityModelId, OutError)
			&& RegisterPropertySemantic(
				ComponentPropertySemanticId, TEXT("blueprint_component_property"),
				ComponentPropertyIdentityModelId, OutError)
			&& RegisterPropertySemantic(
				NodePropertySemanticId, TEXT("blueprint_node_property"),
				NodePropertyIdentityModelId, OutError);
	}

	FString GetAssetKind(const UObject* Object)
	{
		const UBlueprint* Blueprint = Cast<UBlueprint>(Object);
		if (!Blueprint || !Blueprint->ParentClass) return FString();
		return Blueprint->ParentClass->IsChildOf(AActor::StaticClass())
			? FString(TEXT("actor_blueprint"))
			: FString(TEXT("blueprint"));
	}

	FString GetAssetSemanticId(const UObject* Object)
	{
		return GetAssetKind(Object).IsEmpty() ? FString() : FString(AssetSemanticId);
	}

	FString GetAssetIdentityModelId(const UObject* Object)
	{
		return GetAssetKind(Object).IsEmpty() ? FString() : FString(AssetIdentityModelId);
	}

	FString GetAssetTargetKind(const UObject* Object)
	{
		return GetAssetKind(Object).IsEmpty() ? FString() : FString(TEXT("blueprint_asset"));
	}
}

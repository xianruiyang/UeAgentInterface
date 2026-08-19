// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Rigging/Authoring/UeAgentRiggingAuthoringDescriptors.h"

#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticRegistry.h"

namespace UeAgentRiggingAuthoring
{
	namespace
	{
		const TArray<FProfile> Profiles = {
			{
				EAssetKind::ControlRig,
				TEXT("control_rig"),
				TEXT("control_rig"),
				TEXT("rigging.control_rig"),
				TEXT("rigging_control_rig_identity"),
				TEXT("uai.semantic.rigging.control_rig.document"),
				TEXT("uai.semantic.rigging.control_rig.export.target"),
				TEXT("uai.apply_input.rigging.control_rig"),
				TEXT("uai.apply_workspace.rigging.control_rig"),
				TEXT("uai.export.rigging.control_rig"),
			},
			{
				EAssetKind::IKRig,
				TEXT("ik_rig"),
				TEXT("ik_rig"),
				TEXT("rigging.ik_rig"),
				TEXT("rigging_ik_rig_identity"),
				TEXT("uai.semantic.rigging.ik_rig.document"),
				TEXT("uai.semantic.rigging.ik_rig.export.target"),
				TEXT("uai.apply_input.rigging.ik_rig"),
				TEXT("uai.apply_workspace.rigging.ik_rig"),
				TEXT("uai.export.rigging.ik_rig"),
			},
			{
				EAssetKind::IKRetargeter,
				TEXT("ik_retargeter"),
				TEXT("ik_retargeter"),
				TEXT("rigging.ik_retargeter"),
				TEXT("rigging_ik_retargeter_identity"),
				TEXT("uai.semantic.rigging.ik_retargeter.document"),
				TEXT("uai.semantic.rigging.ik_retargeter.export.target"),
				TEXT("uai.apply_input.rigging.ik_retargeter"),
				TEXT("uai.apply_workspace.rigging.ik_retargeter"),
				TEXT("uai.export.rigging.ik_retargeter"),
			},
		};

		bool RegisterIdentity(const FProfile& Profile, FString& OutError)
		{
			UeAgentAuthoringIdentity::FModelDescriptor Model;
			Model.ModelId = Profile.IdentityModelId;
			Model.TargetKind = Profile.TargetKind;
			Model.Selectors = { { TEXT("asset_path"), 100, false } };
			Model.KeyPatterns = { { TEXT("asset"), 100, { TEXT("asset_path") } } };
			UeAgentAuthoringIdentity::ERegistrationResult Result;
			return UeAgentAuthoringIdentity::FRegistry::Get().RegisterModel(
				Model, Result, OutError);
		}

		bool RegisterSemantic(
			const FProfile& Profile,
			const FString& SemanticId,
			const UeAgentAuthoringSemanticRegistry::EWriteCapability WriteCapability,
			const bool bFactoryDefault,
			FString& OutError)
		{
			UeAgentAuthoringSemanticRegistry::FSemanticDescriptor Semantic;
			Semantic.SemanticId = SemanticId;
			Semantic.DomainId = DomainId;
			Semantic.TargetKind = Profile.TargetKind;
			Semantic.ValueType = TEXT("object");
			Semantic.OwnerModel = TEXT("rigging_asset_owner");
			Semantic.IdentityModel = Profile.IdentityModelId;
			Semantic.StateAccessor = StateAccessorId;
			Semantic.WriteCapability = WriteCapability;
			Semantic.ConstraintSource = TEXT("rigging.asset.schema");
			Semantic.AvailabilitySource = StateAccessorId;
			Semantic.Comparator = TEXT("uai.compare.canonical_value");
			Semantic.CapabilityTags = {
				TEXT("rigging"), TEXT("asset"), TEXT("hierarchy"), TEXT("controller")
			};
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

	const TArray<FProfile>& GetProfiles()
	{
		return Profiles;
	}

	const FProfile* FindProfile(const EAssetKind Kind)
	{
		return Profiles.FindByPredicate([Kind](const FProfile& Profile) { return Profile.Kind == Kind; });
	}

	const FProfile* FindProfileByTypeId(const FString& TypeId)
	{
		return Profiles.FindByPredicate([&TypeId](const FProfile& Profile)
		{
			return Profile.TypeId.Equals(TypeId, ESearchCase::IgnoreCase);
		});
	}

	const FProfile* FindProfileByTargetKind(const FString& TargetKind)
	{
		return Profiles.FindByPredicate([&TargetKind](const FProfile& Profile)
		{
			return Profile.TargetKind.Equals(TargetKind, ESearchCase::IgnoreCase);
		});
	}

	const TCHAR* AssetKindToString(const EAssetKind Kind)
	{
		switch (Kind)
		{
		case EAssetKind::ControlRig: return TEXT("control_rig");
		case EAssetKind::IKRig: return TEXT("ik_rig");
		case EAssetKind::IKRetargeter: return TEXT("ik_retargeter");
		default: return TEXT("unknown");
		}
	}

	bool ParseAssetKind(const FString& Text, EAssetKind& OutKind)
	{
		if (const FProfile* Profile = FindProfileByTypeId(Text))
		{
			OutKind = Profile->Kind;
			return true;
		}
		return false;
	}

	bool RegisterDescriptors(FString& OutError)
	{
		for (const FProfile& Profile : Profiles)
		{
			if (!RegisterIdentity(Profile, OutError)) return false;
		}

		UeAgentAuthoringSemanticRegistry::FDomainDescriptor Domain;
		Domain.DomainId = DomainId;
		Domain.MutationProviderId = ApplyProviderId;
		for (const FProfile& Profile : Profiles) Domain.TargetKinds.Add(Profile.TargetKind);
		Domain.CapabilityTags = {
			TEXT("asset_lifecycle"), TEXT("hierarchy"), TEXT("control"), TEXT("graph"),
			TEXT("solver"), TEXT("chain"), TEXT("mapping"), TEXT("retarget_pose")
		};
		UeAgentAuthoringSemanticRegistry::ERegistrationResult DomainResult;
		if (!UeAgentAuthoringSemanticRegistry::FRegistry::Get().RegisterDomain(
			Domain, DomainResult, OutError)) return false;

		using EWrite = UeAgentAuthoringSemanticRegistry::EWriteCapability;
		for (const FProfile& Profile : Profiles)
		{
			if (!RegisterSemantic(Profile, Profile.DocumentSemanticId, EWrite::DomainMutation, true, OutError)
				|| !RegisterSemantic(Profile, Profile.ExportTargetSemanticId, EWrite::ReadOnly, false, OutError))
			{
				return false;
			}
		}
		return true;
	}
}

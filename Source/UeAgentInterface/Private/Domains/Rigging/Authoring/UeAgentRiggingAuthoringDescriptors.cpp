// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Rigging/Authoring/UeAgentRiggingAuthoringDescriptors.h"

#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticRegistry.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticCompare.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Rigs/RigHierarchyElements.h"
#include "ControlRigObjectVersion.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "UObject/NoExportTypes.h"

namespace UeAgentRiggingAuthoring
{
	namespace
	{
		constexpr const TCHAR* ControlRigComparator = TEXT("uai.compare.rigging.control_rig");

		bool ProjectPersistedHierarchy(TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& Error)
		{
			if (!Value || Value->Type != EJson::Object) return true;
			const TSharedPtr<FJsonObject>* Hierarchy = nullptr;
			const TSharedPtr<FJsonObject>* Elements = nullptr;
			if (!Value->AsObject()->TryGetObjectField(TEXT("hierarchy"), Hierarchy)
				|| !(*Hierarchy)->TryGetObjectField(TEXT("elements"), Elements)) return true;
			for (const auto& Element : (*Elements)->Values)
			{
				if (!Element.Value || Element.Value->Type != EJson::Object) continue;
				const auto Record = Element.Value->AsObject();
				for (const TCHAR* Field : {TEXT("initial_local_transform"), TEXT("initial_offset_transform"), TEXT("initial_shape_transform")})
				{
					const auto Input = Record->TryGetField(Field);
					if (!Input) continue;
					FTransform Transform;
					if (!UeAgentPropertyValueCodec::DecodeStructValue(TBaseStructure<FTransform>::Get(), Input, &Transform, Error)) return false;
					// Package persistence uses the hierarchy's default serialization settings.
					// Execute UE's compact codec, including its identity/uniform elision;
					// do not approximate it with a global float tolerance or alter capture.
					TArray<uint8> Bytes;
					FMemoryWriter Writer(Bytes, true);
					Writer.UsingCustomVersion(FControlRigObjectVersion::GUID);
					const FRigHierarchySerializationSettings Settings(Writer);
					FRigCompactTransform(Transform).Save(Writer, Settings);
					FMemoryReader Reader(Bytes, true);
					FRigCompactTransform(Transform).Load(Reader, Settings);
					if (Writer.IsError() || Reader.IsError())
					{
						Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("control_rig_compact_transform_invalid"),
							TEXT("The native hierarchy transform could not be projected through its package storage format."), TEXT("semantic_compare"));
						return false;
					}
					Record->SetField(Field, UeAgentPropertyValueCodec::EncodeStructValue(
						TBaseStructure<FTransform>::Get(), &Transform, nullptr, {}));
				}
			}
			return true;
		}
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

		bool RegisterIdentity(const FProfile& Profile, UE::UAI::Contract::FDiagnostic& OutError)
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
			UE::UAI::Contract::FDiagnostic& OutError)
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
			Semantic.Comparator = Profile.Kind == EAssetKind::ControlRig
				? ControlRigComparator : TEXT("uai.compare.canonical_value");
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

	bool RegisterDescriptors(UE::UAI::Contract::FDiagnostic& OutError)
	{
		auto& Comparators = UeAgentAuthoringSemanticCompare::FComparatorRegistry::Get();
		UeAgentAuthoringSemanticCompare::FComparator Canonical;
		if (!UeAgentAuthoringSemanticCompare::RegisterBuiltInComparators(Comparators, OutError)
			|| !Comparators.CopyComparator(UeAgentAuthoringSemanticCompare::GraphVectorPositionComparator, Canonical)) return false;
		UeAgentAuthoringSemanticCompare::FComparatorDescriptor Comparator;
		Comparator.ComparatorId = ControlRigComparator;
		Comparator.ValueKinds = {TEXT("object")};
		Comparator.CapabilityTags = {TEXT("canonical_value"), TEXT("native_rig_compact_persistence")};
		UeAgentAuthoringSemanticCompare::ERegistrationResult ComparatorResult;
		if (!Comparators.RegisterComparator(Comparator,
			[Canonical](const UeAgentAuthoringState::FStateEntry& Left, const UeAgentAuthoringState::FStateEntry& Right,
				bool& Equivalent, UE::UAI::Contract::FDiagnostic& Error) {
				auto Expected = Left, Actual = Right;
				Expected.Value = UeAgentAuthoringState::CloneCanonicalValue(Left.Value);
				Actual.Value = UeAgentAuthoringState::CloneCanonicalValue(Right.Value);
				return ProjectPersistedHierarchy(Expected.Value, Error) && ProjectPersistedHierarchy(Actual.Value, Error)
					&& Canonical(Expected, Actual, Equivalent, Error);
			}, ComparatorResult, OutError,
			[](UeAgentAuthoringState::FStateEntry& Entry, TSharedPtr<FJsonValue>& Before,
				TSharedPtr<FJsonValue>& Desired, TSharedPtr<FJsonValue>& Actual,
				TMap<FString, FString>& SourcePaths, UE::UAI::Contract::FDiagnostic& Error) {
				return UeAgentAuthoringSemanticCompare::FComparatorRegistry::Get().ProjectOriginalIntent(
					UeAgentAuthoringSemanticCompare::GraphVectorPositionComparator, Entry, Before, Desired, Actual, SourcePaths, Error);
			})) return false;
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

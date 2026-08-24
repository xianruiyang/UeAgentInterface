#include "Composition/TimelineSpatial/UeAgentTimelineSpatialActivation.h"

#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringComposition.h"
#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialDescriptors.h"
#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialExecutionChains.h"
#include "DomainAdapters/TimelineAnimation/UeAgentTimelineAnimationDescriptors.h"
#include "DomainAdapters/TimelineAnimation/UeAgentTimelineAnimationExecutionChains.h"
#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialDescriptors.h"
#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialExecutionChains.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UE::UAI::Composition::TimelineSpatial
{
	using namespace CommandContract;
	using namespace Contract;

	namespace
	{
		bool IsIdentifier(const FString& Value)
		{
			if (Value.IsEmpty()
				|| !(FChar::IsAlpha(Value[0]) || Value[0] == TEXT('_')))
			{
				return false;
			}
			for (int32 Index = 1; Index < Value.Len(); ++Index)
			{
				if (!(FChar::IsAlnum(Value[Index])
					|| Value[Index] == TEXT('_')))
				{
					return false;
				}
			}
			return true;
		}

		bool TryPythonBinding(
			const FString& CapabilityId,
			FString& OutNamespace,
			FString& OutMethod)
		{
			TArray<FString> Segments;
			CapabilityId.ParseIntoArray(Segments, TEXT("."), true);
			if (Segments.Num() < 2)
			{
				return false;
			}
			OutNamespace = Segments[0];
			Segments.RemoveAt(0);
			OutMethod = FString::Join(Segments, TEXT("_"));
			return IsIdentifier(OutNamespace) && IsIdentifier(OutMethod);
		}

		bool HasRequiredInput(const FUeAgentOperationDescriptor& Descriptor)
		{
			TSharedPtr<FJsonObject> Root;
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(
					Descriptor.GetInputSchema().GetJsonValue());
			if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
			{
				return false;
			}
			const TArray<TSharedPtr<FJsonValue>>* Required = nullptr;
			return Root->TryGetArrayField(TEXT("required"), Required)
				&& Required
				&& !Required->IsEmpty();
		}

		bool RequiresPublicContext(
			const FString& AdapterId,
			const FString& CapabilityId)
		{
			if (AdapterId == TEXT("world_spatial"))
			{
				return CapabilityId != TEXT("mesh.transform.bake")
					&& CapabilityId != TEXT("mesh.pivot.update")
					&& CapabilityId != TEXT("viewport.instance.list");
			}
			if (AdapterId == TEXT("timeline_animation"))
			{
				return CapabilityId.StartsWith(TEXT("animation.subject."))
					|| CapabilityId
						== TEXT("animation.ik_probe_candidate.apply");
			}
			static const TSet<FString> PublicRigCapabilities = {
				TEXT("rig.control.bake.animation"),
				TEXT("rig.control.bake.controls"),
				TEXT("physics.simulation.probe"),
				TEXT("deformer.geometry.sample"),
				TEXT("deformer.evaluation.sample"),
				TEXT("pcg.generated_spatial_data.read"),
				TEXT("pcg.partition.spatial_state.read"),
			};
			return PublicRigCapabilities.Contains(CapabilityId);
		}

		bool IsSessionControl(const FString& CapabilityId)
		{
			static const TSet<FString> SessionCapabilities = {
				TEXT("graph.view.update"),
				TEXT("viewport.actor.select"),
				TEXT("mesh.element.select_by_ray"),
				TEXT("viewport.camera.frame"),
				TEXT("viewport.state.restore"),
				TEXT("viewport.state.update"),
				TEXT("animation.subject.pose.update"),
				TEXT("animation.subject.morph_weights.update"),
				TEXT("physics.simulation.probe"),
			};
			return SessionCapabilities.Contains(CapabilityId);
		}

		bool RequiresTarget(const FString& CapabilityId)
		{
			static const TSet<FString> TargetlessCapabilities = {
				TEXT("viewport.warning.list"),
				TEXT("viewport.view_mode.list"),
				TEXT("viewport.instance.list"),
				TEXT("animation.montage.create"),
				TEXT("timeline.sequence.create"),
				TEXT("rig.retarget.assets.generate"),
			};
			return !TargetlessCapabilities.Contains(CapabilityId);
		}

		template <typename TExecutionChainContract>
		bool TryAppendProfiles(
			const FString& AdapterId,
			const TArray<TExecutionChainContract>& Chains,
			const FUeAgentDescriptorRegistry& Descriptors,
			TArray<FUeAgentTimelineSpatialActivationProfile>& OutProfiles,
			FString& OutError)
		{
			for (const TExecutionChainContract& Chain : Chains)
			{
				const FUeAgentOperationDescriptor* Descriptor =
					Descriptors.FindByDescriptorId(Chain.GetDescriptorId());
				if (!Descriptor
					|| Descriptor->GetCapabilityId()
						!= Chain.GetCapabilityId())
				{
					OutError = TEXT("timeline_spatial_descriptor_missing:")
						+ Chain.GetCapabilityId();
					return false;
				}

				FUeAgentTimelineSpatialActivationProfile Profile(
					Descriptor->GetIdentity());
				Profile.AdapterId = AdapterId;
				Profile.CapabilityId = Chain.GetCapabilityId();
				Profile.CommandId = Profile.CapabilityId;
				Profile.OperationKind = Chain.GetOperationKind();
				Profile.EffectClass =
					Profile.OperationKind == TEXT("query")
						? TEXT("read_only")
						: IsSessionControl(Profile.CapabilityId)
							? TEXT("session_control")
							: TEXT("write");
				Profile.FamilyId = Chain.GetFamilyId();
				Profile.TypedOperationId = Chain.GetBindingId();
				Profile.ProviderBinding = Chain.GetBindingId();
				Profile.ContextSource = RequiresPublicContext(
					AdapterId,
					Profile.CapabilityId)
					? TEXT("public_required")
					: TEXT("immutable_application");
				Profile.OracleKind = Chain.GetOracleKind();
				Profile.bRequiresTarget = RequiresTarget(Profile.CapabilityId);
				Profile.bRequiresInput = HasRequiredInput(*Descriptor);
				Profile.bSupportsProjection = true;
				if (!TryPythonBinding(
						Profile.CapabilityId,
						Profile.Namespace,
						Profile.Method)
					|| !Profile.IsValid())
				{
					OutError = TEXT("timeline_spatial_activation_profile_invalid:")
						+ Profile.CapabilityId;
					return false;
				}
				OutProfiles.Add(MoveTemp(Profile));
			}
			return true;
		}

		TArray<FUeAgentCommandParameter> PublicParameters(
			const FUeAgentTimelineSpatialActivationProfile& Profile)
		{
			TArray<FUeAgentCommandParameter> Parameters;
			if (Profile.bRequiresTarget)
			{
				Parameters.Emplace(
					TEXT("target"),
					EUeAgentCommandWireType::String,
					true,
					false);
			}
			Parameters.Emplace(
				TEXT("input"),
				EUeAgentCommandWireType::Object,
				Profile.bRequiresInput,
				false);
			Parameters.Emplace(
				TEXT("context"),
				EUeAgentCommandWireType::Object,
				Profile.ContextSource == TEXT("public_required"),
				false);
			if (Profile.bSupportsProjection)
			{
				Parameters.Emplace(
					TEXT("projection"),
					EUeAgentCommandWireType::Object,
					false,
					false);
			}
			return Parameters;
		}
	}

	bool FUeAgentTimelineSpatialActivationProfile::IsValid() const
	{
		return !AdapterId.IsEmpty()
			&& !CapabilityId.IsEmpty()
			&& CommandId == CapabilityId
			&& IsIdentifier(Namespace)
			&& IsIdentifier(Method)
			&& DescriptorIdentity.IsValid()
			&& (OperationKind == TEXT("query")
				|| OperationKind == TEXT("operation"))
			&& (EffectClass == TEXT("read_only")
				|| EffectClass == TEXT("session_control")
				|| EffectClass == TEXT("write"))
			&& !FamilyId.IsEmpty()
			&& !TypedOperationId.IsEmpty()
			&& !ProviderBinding.IsEmpty()
			&& (ContextSource == TEXT("public_required")
				|| ContextSource == TEXT("immutable_application"))
			&& !OracleKind.IsEmpty();
	}

	bool TryCreateTimelineSpatialDescriptorRegistry(
		TSharedPtr<const FUeAgentDescriptorRegistry>& OutRegistry,
		FString& OutError)
	{
		OutRegistry.Reset();
		OutError.Reset();
		TArray<FUeAgentOperationDescriptor> Merged;
		auto AppendRegistry = [&Merged, &OutError](
			const TCHAR* Label,
			const int32 ExpectedCount,
			const TFunction<bool(
				TSharedPtr<const FUeAgentDescriptorRegistry>&,
				FString&)>& Factory)
		{
			TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
			FString Error;
			if (!Factory(Registry, Error)
				|| !Registry.IsValid()
				|| Registry->GetDescriptors().Num() != ExpectedCount)
			{
				OutError = FString::Printf(
					TEXT("timeline_spatial_descriptor_source_invalid:%s:%s"),
					Label,
					*Error);
				return false;
			}
			Merged.Append(Registry->GetDescriptors());
			return true;
		};

		if (!AppendRegistry(
				TEXT("world_spatial"),
				22,
				DomainAdapters::WorldSpatial::
					TryCreateWorldSpatialDescriptorRegistry)
			|| !AppendRegistry(
				TEXT("timeline_animation"),
				20,
				DomainAdapters::TimelineAnimation::
					TryCreateTimelineAnimationDescriptorRegistry)
			|| !AppendRegistry(
				TEXT("rig_simulation_spatial"),
				18,
				DomainAdapters::RigSimulationSpatial::
					TryCreateRigSimulationSpatialDescriptorRegistry))
		{
			return false;
		}
		return FUeAgentDescriptorRegistry::TryCreate(
			MoveTemp(Merged),
			OutRegistry,
			OutError);
	}

	const TArray<FUeAgentTimelineSpatialActivationProfile>&
	GetTimelineSpatialActivationProfiles()
	{
		static const TArray<FUeAgentTimelineSpatialActivationProfile> Profiles = []()
		{
			TArray<FUeAgentTimelineSpatialActivationProfile> Result;
			TSharedPtr<const FUeAgentDescriptorRegistry> Descriptors;
			FString Error;
			const bool bCreated =
				TryCreateTimelineSpatialDescriptorRegistry(Descriptors, Error)
				&& Descriptors.IsValid()
				&& TryAppendProfiles(
					TEXT("world_spatial"),
					DomainAdapters::WorldSpatial::
						GetWorldSpatialExecutionChainContracts(),
					*Descriptors,
					Result,
					Error)
				&& TryAppendProfiles(
					TEXT("timeline_animation"),
					DomainAdapters::TimelineAnimation::
						GetTimelineAnimationExecutionChainContracts(),
					*Descriptors,
					Result,
					Error)
				&& TryAppendProfiles(
					TEXT("rig_simulation_spatial"),
					DomainAdapters::RigSimulationSpatial::
						GetRigSimulationSpatialExecutionChainContracts(),
					*Descriptors,
					Result,
					Error)
				&& Result.Num() == 60;
			if (!bCreated)
			{
				Result.Reset();
			}
			return Result;
		}();
		return Profiles;
	}

	bool TryCreateTimelineSpatialProductionCommandContractRegistry(
		TSharedPtr<const FUeAgentCommandContractRegistry>& OutRegistry,
		FString& OutError)
	{
		OutRegistry.Reset();
		OutError.Reset();
		TSharedPtr<const FUeAgentCommandContractRegistry> Base;
		if (!SimpleAuthoring::TryCreateProductionCommandContractRegistry(
				Base,
				OutError)
			|| !Base.IsValid())
		{
			return false;
		}
		const TArray<FUeAgentTimelineSpatialActivationProfile>& Profiles =
			GetTimelineSpatialActivationProfiles();
		if (Profiles.Num() != 60)
		{
			OutError = TEXT("timeline_spatial_activation_profiles_invalid");
			return false;
		}

		TArray<FUeAgentCommandContract> Contracts = Base->GetContracts();
		for (const FUeAgentTimelineSpatialActivationProfile& Profile
			: Profiles)
		{
			Contracts.Emplace(
				Profile.CommandId,
				Profile.Namespace,
				Profile.Method,
				PublicParameters(Profile),
				1,
				1,
				EUeAgentCommandStatus::Active);
		}

		return FUeAgentCommandContractRegistry::TryCreate(
			MoveTemp(Contracts),
			OutRegistry,
			OutError);
	}
}

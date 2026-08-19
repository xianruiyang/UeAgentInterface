#include "Composition/GraphCompound/UeAgentGraphCompoundActivation.h"

#include "Composition/GraphCompound/UeAgentGraphCompoundAudioMediaRoutes.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundBlueprintMaterialRoutes.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundPcgAnimationRoutes.h"
#include "Composition/LongRunningSession/UeAgentLongRunningSessionActivation.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::Composition::GraphCompound
{
	using namespace CommandContract;
	using namespace Contract;

	namespace
	{
		void PublicRoute(
			const FString& CommandId,
			FString& OutNamespace,
			FString& OutMethod)
		{
			TArray<FString> Segments;
			CommandId.ParseIntoArray(Segments, TEXT("."), true);
			OutNamespace = Segments.IsEmpty() ? FString() : Segments[0];
			OutMethod.Reset();
			for (int32 Index = 1; Index < Segments.Num(); ++Index)
			{
				if (!OutMethod.IsEmpty())
				{
					OutMethod += TEXT("_");
				}
				OutMethod += Segments[Index];
			}
		}

		EUeAgentCommandWireType WireType(
			const TSharedPtr<FJsonObject>& Property)
		{
			FString Type;
			if (!Property.IsValid()
				|| !Property->TryGetStringField(TEXT("type"), Type))
			{
				return EUeAgentCommandWireType::Any;
			}
			if (Type == TEXT("boolean")) return EUeAgentCommandWireType::Boolean;
			if (Type == TEXT("integer")) return EUeAgentCommandWireType::Integer;
			if (Type == TEXT("number")) return EUeAgentCommandWireType::Number;
			if (Type == TEXT("string")) return EUeAgentCommandWireType::String;
			if (Type == TEXT("object")) return EUeAgentCommandWireType::Object;
			if (Type == TEXT("array")) return EUeAgentCommandWireType::Array;
			return EUeAgentCommandWireType::Any;
		}

		bool TryPublicParameters(
			const FUeAgentOperationDescriptor& Descriptor,
			TArray<FUeAgentCommandParameter>& OutParameters,
			FString& OutError)
		{
			OutParameters.Reset();
			TSharedPtr<FJsonObject> Schema;
			if (!Json::TryParseObject(
					Descriptor.GetInputSchema().GetJsonValue(),
					Schema,
					OutError)
				|| !Schema.IsValid())
			{
				return false;
			}
			const TSharedPtr<FJsonObject>* Properties = nullptr;
			if (!Schema->TryGetObjectField(TEXT("properties"), Properties)
				|| !Properties
				|| !Properties->IsValid())
			{
				OutError = TEXT("graph_compound_input_schema_properties_missing:")
					+ Descriptor.GetCapabilityId();
				return false;
			}
			TSet<FString> Required;
			const TArray<TSharedPtr<FJsonValue>>* RequiredValues = nullptr;
			if (Schema->TryGetArrayField(TEXT("required"), RequiredValues)
				&& RequiredValues)
			{
				for (const TSharedPtr<FJsonValue>& Value : *RequiredValues)
				{
					FString Name;
					if (Value.IsValid() && Value->TryGetString(Name))
					{
						Required.Add(Name);
					}
				}
			}
			TArray<FString> Names;
			(*Properties)->Values.GetKeys(Names);
			Names.Sort();
			for (const FString& Name : Names)
			{
				const TSharedPtr<FJsonObject>* Property = nullptr;
				if (!(*Properties)->TryGetObjectField(Name, Property)
					|| !Property
					|| !Property->IsValid())
				{
					OutError = TEXT("graph_compound_input_property_invalid:")
						+ Descriptor.GetCapabilityId() + TEXT(":") + Name;
					return false;
				}
				OutParameters.Emplace(
					Name,
					WireType(*Property),
					Required.Contains(Name),
					false);
			}
			return true;
		}

		struct FActivationBundle final
		{
			TArray<FUeAgentGraphCompoundActivationProfile> Profiles;
			TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
			FString Error;
		};

		FActivationBundle BuildBundle()
		{
			FActivationBundle Bundle;
			TArray<FUeAgentOperationDescriptor> Descriptors;
			auto AppendRegistry = [&Descriptors, &Bundle](
				const TSharedPtr<const FUeAgentDescriptorRegistry>& Registry)
			{
				if (!Registry.IsValid())
				{
					return false;
				}
				Descriptors.Append(Registry->GetDescriptors());
				return true;
			};

			TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
			if (!TryCreateBlueprintMaterialGraphDescriptorRegistry(
					Registry, Bundle.Error)
				|| !AppendRegistry(Registry)
				|| !TryCreatePcgAnimationGraphDescriptorRegistry(
					Registry, Bundle.Error)
				|| !AppendRegistry(Registry)
				|| !TryCreateAudioMediaGraphRouteDescriptorRegistry(
					Registry, Bundle.Error)
				|| !AppendRegistry(Registry))
			{
				Bundle.Error = Bundle.Error.IsEmpty()
					? TEXT("graph_compound_domain_registry_missing")
					: Bundle.Error;
				return Bundle;
			}
			if (!FUeAgentDescriptorRegistry::TryCreate(
					MoveTemp(Descriptors), Bundle.Registry, Bundle.Error)
				|| !Bundle.Registry.IsValid())
			{
				return Bundle;
			}

			auto AddProfile = [&Bundle](
				const FString& CapabilityId,
				const FUeAgentDescriptorIdentity& Identity,
				const EGraphCompoundExecutionLane Lane)
			{
				FString Namespace;
				FString Method;
				PublicRoute(CapabilityId, Namespace, Method);
				Bundle.Profiles.Emplace(
					CapabilityId,
					MoveTemp(Namespace),
					MoveTemp(Method),
					Identity,
					Lane == EGraphCompoundExecutionLane::Query
						? TEXT("query") : TEXT("operation"));
				return Bundle.Profiles.Last().IsValid();
			};
			for (const FGraphCompoundDomainRouteProfile& Profile
				: GetBlueprintMaterialGraphRouteProfiles())
			{
				if (!AddProfile(Profile.GetCapabilityId(),
						Profile.GetExecutionContract().GetDescriptorIdentity(),
						Profile.GetExecutionContract().GetLane()))
				{
					Bundle.Profiles.Reset();
					Bundle.Error = TEXT("graph_compound_activation_profile_invalid:")
						+ Profile.GetCapabilityId();
					return Bundle;
				}
			}
			for (const FPcgAnimationGraphRouteProfile& Profile
				: GetPcgAnimationGraphRouteProfiles())
			{
				if (!AddProfile(Profile.GetCapabilityId(),
						Profile.GetExecutionContract().GetDescriptorIdentity(),
						Profile.GetExecutionContract().GetLane()))
				{
					Bundle.Profiles.Reset();
					Bundle.Error = TEXT("graph_compound_activation_profile_invalid:")
						+ Profile.GetCapabilityId();
					return Bundle;
				}
			}
			for (const FAudioMediaGraphRouteProfile& Profile
				: GetAudioMediaGraphRouteProfiles())
			{
				if (!AddProfile(Profile.GetCapabilityId(),
						Profile.GetExecutionContract().GetDescriptorIdentity(),
						Profile.GetExecutionContract().GetLane()))
				{
					Bundle.Profiles.Reset();
					Bundle.Error = TEXT("graph_compound_activation_profile_invalid:")
						+ Profile.GetCapabilityId();
					return Bundle;
				}
			}
			if (Bundle.Profiles.Num() != 77)
			{
				Bundle.Profiles.Reset();
				Bundle.Error = TEXT("graph_compound_activation_profile_count_invalid");
			}
			return Bundle;
		}

		const FActivationBundle& ActivationBundle()
		{
			static const FActivationBundle Bundle = BuildBundle();
			return Bundle;
		}
	}

	FUeAgentGraphCompoundActivationProfile::
	FUeAgentGraphCompoundActivationProfile(
		FString InCapabilityId,
		FString InNamespace,
		FString InMethod,
		FUeAgentDescriptorIdentity InDescriptorIdentity,
		FString InOperationKind)
		: CapabilityId(MoveTemp(InCapabilityId))
		, Namespace(MoveTemp(InNamespace))
		, Method(MoveTemp(InMethod))
		, DescriptorIdentity(MoveTemp(InDescriptorIdentity))
		, OperationKind(MoveTemp(InOperationKind))
	{
		bValid = !CapabilityId.IsEmpty()
			&& !Namespace.IsEmpty()
			&& !Method.IsEmpty()
			&& DescriptorIdentity.IsValid()
			&& (OperationKind == TEXT("query")
				|| OperationKind == TEXT("operation"));
	}

	const FString& FUeAgentGraphCompoundActivationProfile::GetCapabilityId() const
	{
		return CapabilityId;
	}

	const FString& FUeAgentGraphCompoundActivationProfile::GetCommandId() const
	{
		return CapabilityId;
	}

	const FString& FUeAgentGraphCompoundActivationProfile::GetNamespace() const
	{
		return Namespace;
	}

	const FString& FUeAgentGraphCompoundActivationProfile::GetMethod() const
	{
		return Method;
	}

	const FUeAgentDescriptorIdentity&
	FUeAgentGraphCompoundActivationProfile::GetDescriptorIdentity() const
	{
		return DescriptorIdentity;
	}

	const FString&
	FUeAgentGraphCompoundActivationProfile::GetOperationKind() const
	{
		return OperationKind;
	}

	bool FUeAgentGraphCompoundActivationProfile::IsValid() const
	{
		return bValid;
	}

	const TArray<FUeAgentGraphCompoundActivationProfile>&
	GetGraphCompoundActivationProfiles()
	{
		return ActivationBundle().Profiles;
	}

	bool TryCreateGraphCompoundDescriptorRegistry(
		TSharedPtr<const FUeAgentDescriptorRegistry>& OutRegistry,
		FString& OutError)
	{
		OutRegistry = ActivationBundle().Registry;
		OutError = ActivationBundle().Error;
		return OutRegistry.IsValid() && OutError.IsEmpty();
	}

	bool TryCreateGraphCompoundProductionCommandContractRegistry(
		TSharedPtr<const FUeAgentCommandContractRegistry>& OutRegistry,
		FString& OutError)
	{
		return TryCreateGraphCompoundProductionCommandContractRegistry(
			GetGraphCompoundActivationProfiles(), OutRegistry, OutError);
	}

	bool TryCreateGraphCompoundProductionCommandContractRegistry(
		const TArray<FUeAgentGraphCompoundActivationProfile>& Profiles,
		TSharedPtr<const FUeAgentCommandContractRegistry>& OutRegistry,
		FString& OutError)
	{
		OutRegistry.Reset();
		OutError.Reset();
		TSharedPtr<const FUeAgentCommandContractRegistry> Base;
		if (!LongRunningSession::
				TryCreateLongRunningSessionProductionCommandContractRegistry(
					Base, OutError)
			|| !Base.IsValid())
		{
			return false;
		}
		const FActivationBundle& Bundle = ActivationBundle();
		if (!Bundle.Registry.IsValid() || Bundle.Profiles.Num() != 77
			|| Profiles.IsEmpty())
		{
			OutError = Bundle.Error.IsEmpty()
				? TEXT("graph_compound_activation_bundle_invalid")
				: Bundle.Error;
			return false;
		}
		TArray<FUeAgentCommandContract> Contracts = Base->GetContracts();
		Contracts.Reserve(Contracts.Num() + Profiles.Num());
		TSet<FString> CommandIds;
		for (const FUeAgentGraphCompoundActivationProfile& Profile : Profiles)
		{
			const FUeAgentGraphCompoundActivationProfile* Canonical = nullptr;
			for (const FUeAgentGraphCompoundActivationProfile& Candidate
				: Bundle.Profiles)
			{
				if (Candidate.GetCommandId() == Profile.GetCommandId())
				{
					Canonical = &Candidate;
					break;
				}
			}
			if (!Canonical
				|| Canonical->GetCapabilityId() != Profile.GetCapabilityId()
				|| Canonical->GetNamespace() != Profile.GetNamespace()
				|| Canonical->GetMethod() != Profile.GetMethod()
				|| !(Canonical->GetDescriptorIdentity()
					== Profile.GetDescriptorIdentity())
				|| Canonical->GetOperationKind() != Profile.GetOperationKind()
				|| CommandIds.Contains(Profile.GetCommandId()))
			{
				OutError = TEXT("graph_compound_activation_profile_not_canonical:")
					+ Profile.GetCommandId();
				return false;
			}
			CommandIds.Add(Profile.GetCommandId());
			const FUeAgentOperationDescriptor* Descriptor =
				Bundle.Registry->FindExact(Profile.GetDescriptorIdentity());
			TArray<FUeAgentCommandParameter> Parameters;
			if (!Descriptor
				|| !TryPublicParameters(*Descriptor, Parameters, OutError))
			{
				OutError = OutError.IsEmpty()
					? TEXT("graph_compound_command_descriptor_missing:")
						+ Profile.GetCommandId()
					: OutError;
				return false;
			}
			Contracts.Emplace(
				Profile.GetCommandId(),
				Profile.GetNamespace(),
				Profile.GetMethod(),
				MoveTemp(Parameters),
				1,
				1,
				EUeAgentCommandStatus::Active);
		}
		return FUeAgentCommandContractRegistry::TryCreate(
			MoveTemp(Contracts), OutRegistry, OutError);
	}
}

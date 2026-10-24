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
		FDiagnostic RegistryFailure(const FString& Code, const FString& Reason,
			const FString& Field, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			auto Error = FDiagnostic::Failure(Code, Reason, TEXT("registry"));
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Field;
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			Error.ActualValuePresent = Error.ActualValue.IsValid();
			Error.ExpectedValuePresent = true;
			return Error;
		}

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
			UE::UAI::Contract::FDiagnostic& OutError)
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
				OutError = RegistryFailure(TEXT("graph_compound_input_schema_properties_missing"),
					TEXT("The operation input schema requires an object-valued properties field"), TEXT("input_schema.properties"),
					Schema->TryGetField(TEXT("properties")), MakeShared<FJsonValueString>(TEXT("object")));
				OutError.CandidateName = Descriptor.GetCapabilityId();
				OutError.ActualValuePresent = Schema->HasField(TEXT("properties"));
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
					OutError = RegistryFailure(TEXT("graph_compound_input_property_invalid"),
						TEXT("An input parameter schema must be a JSON object"), TEXT("input_schema.properties.") + Name,
						(*Properties)->TryGetField(Name), MakeShared<FJsonValueString>(TEXT("object")));
					OutError.CandidateName = Descriptor.GetCapabilityId();
					return false;
				}
				FString PropertySchema;
				if (!Json::TryCanonicalize(*Property, PropertySchema, OutError))
				{
					return false;
				}
				OutParameters.Emplace(
					Name,
					WireType(*Property),
					Required.Contains(Name),
					false,
					MoveTemp(PropertySchema));
			}
			return true;
		}

		struct FActivationBundle final
		{
			TArray<FUeAgentGraphCompoundActivationProfile> Profiles;
			TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
			UE::UAI::Contract::FDiagnostic Error;
		};

		FActivationBundle BuildBundle()
		{
			FActivationBundle Bundle;
			TArray<FUeAgentOperationDescriptor> Descriptors;
			auto AppendRegistry = [&Descriptors, &Bundle](const TCHAR* Family, auto&& Factory)
			{
				TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
				const bool bCreated = Factory(Registry, Bundle.Error);
				if (!bCreated || !Registry.IsValid())
				{
					if (Bundle.Error.IsEmpty()) Bundle.Error = RegistryFailure(TEXT("graph_compound_domain_registry_missing"),
						bCreated ? TEXT("The domain descriptor factory returned no registry")
							: TEXT("The domain descriptor factory failed without a diagnostic"),
						bCreated ? TEXT("descriptor_registry_present") : TEXT("descriptor_factory_succeeded"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					if (Bundle.Error.SubjectId.IsEmpty()) Bundle.Error.SubjectId = Family;
					return false;
				}
				Descriptors.Append(Registry->GetDescriptors());
				return true;
			};
			if (!AppendRegistry(TEXT("blueprint_material"), TryCreateBlueprintMaterialGraphDescriptorRegistry)
				|| !AppendRegistry(TEXT("pcg_animation"), TryCreatePcgAnimationGraphDescriptorRegistry)
				|| !AppendRegistry(TEXT("audio_media"), TryCreateAudioMediaGraphRouteDescriptorRegistry))
			{
				return Bundle;
			}
			if (!FUeAgentDescriptorRegistry::TryCreate(
					MoveTemp(Descriptors), Bundle.Registry, Bundle.Error)
				|| !Bundle.Registry.IsValid())
			{
				if (Bundle.Error.IsEmpty()) Bundle.Error = RegistryFailure(TEXT("graph_compound_descriptor_registry_missing"),
					TEXT("The combined descriptor factory returned no usable registry"), TEXT("descriptor_registry_present"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
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
				Bundle.Error = Bundle.Profiles.Last().GetValidationDiagnostic();
				return Bundle.Error.IsEmpty();
			};
			const TArray<FGraphCompoundDomainRouteProfile>* BlueprintMaterialProfiles = nullptr;
			if (!TryGetBlueprintMaterialGraphRouteProfiles(BlueprintMaterialProfiles, Bundle.Error))
			{
				Bundle.Profiles.Reset();
				return Bundle;
			}
			for (const FGraphCompoundDomainRouteProfile& Profile : *BlueprintMaterialProfiles)
			{
				if (!AddProfile(Profile.GetCapabilityId(),
						Profile.GetExecutionContract().GetDescriptorIdentity(),
						Profile.GetExecutionContract().GetLane()))
				{
					Bundle.Profiles.Reset();
					return Bundle;
				}
			}
			const TArray<FPcgAnimationGraphRouteProfile>* PcgAnimationProfiles = nullptr;
			if (!TryGetPcgAnimationGraphRouteProfiles(PcgAnimationProfiles, Bundle.Error))
			{
				Bundle.Profiles.Reset();
				return Bundle;
			}
			for (const FPcgAnimationGraphRouteProfile& Profile : *PcgAnimationProfiles)
			{
				if (!AddProfile(Profile.GetCapabilityId(),
						Profile.GetExecutionContract().GetDescriptorIdentity(),
						Profile.GetExecutionContract().GetLane()))
				{
					Bundle.Profiles.Reset();
					return Bundle;
				}
			}
			const TArray<FAudioMediaGraphRouteProfile>* AudioMediaProfiles = nullptr;
			if (!TryGetAudioMediaGraphRouteProfiles(AudioMediaProfiles, Bundle.Error))
			{
				Bundle.Profiles.Reset();
				return Bundle;
			}
			for (const FAudioMediaGraphRouteProfile& Profile : *AudioMediaProfiles)
			{
				if (!AddProfile(Profile.GetCapabilityId(),
						Profile.GetExecutionContract().GetDescriptorIdentity(),
						Profile.GetExecutionContract().GetLane()))
				{
					Bundle.Profiles.Reset();
					return Bundle;
				}
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
		auto FailField = [this](const TCHAR* Field, const TCHAR* Reason,
			const FString& Actual, TSharedPtr<FJsonValue> Expected)
		{
			ValidationDiagnostic = FDiagnostic::Failure(
				TEXT("graph_compound_activation_profile_invalid"),
				FString(TEXT("The Graph activation profile ")) + Reason, TEXT("registry"), FString(), CapabilityId);
			ValidationDiagnostic.SourceKind = TEXT("native_result");
			ValidationDiagnostic.PropertyPath = Field;
			ValidationDiagnostic.ActualValue = MakeShared<FJsonValueString>(Actual);
			ValidationDiagnostic.ActualValuePresent = true;
			ValidationDiagnostic.ExpectedValue = MoveTemp(Expected);
		};
		if (CapabilityId.IsEmpty())
			FailField(TEXT("/capability_id"), TEXT("requires a non-empty capability identifier."),
				CapabilityId, MakeShared<FJsonValueString>(TEXT("non-empty string")));
		else if (Namespace.IsEmpty())
			FailField(TEXT("/namespace"), TEXT("requires a non-empty namespace."),
				Namespace, MakeShared<FJsonValueString>(TEXT("non-empty string")));
		else if (Method.IsEmpty())
			FailField(TEXT("/method"), TEXT("requires a non-empty method."),
				Method, MakeShared<FJsonValueString>(TEXT("non-empty string")));
		else if (!DescriptorIdentity.IsValid())
		{
			ValidationDiagnostic = FDiagnostic::WrapFailure(
				TEXT("graph_compound_activation_profile_invalid"),
				DescriptorIdentity.GetValidationDiagnostic(), TEXT("registry"),
				TEXT("The Graph activation profile has an invalid descriptor identity."));
			ValidationDiagnostic.SubjectId = CapabilityId;
			ValidationDiagnostic.SourceKind = TEXT("native_result");
			ValidationDiagnostic.PropertyPath = TEXT("/descriptor_identity")
				+ ValidationDiagnostic.PropertyPath;
		}
		else if (OperationKind != TEXT("query") && OperationKind != TEXT("operation"))
		{
			TArray<TSharedPtr<FJsonValue>> Allowed = {
				MakeShared<FJsonValueString>(TEXT("query")),
				MakeShared<FJsonValueString>(TEXT("operation")) };
			FailField(TEXT("/operation_kind"), TEXT("has an unsupported operation kind."),
				OperationKind, MakeShared<FJsonValueArray>(MoveTemp(Allowed)));
		}
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
		return ValidationDiagnostic.IsEmpty();
	}

	const FDiagnostic& FUeAgentGraphCompoundActivationProfile::GetValidationDiagnostic() const
	{
		return ValidationDiagnostic;
	}

	bool FUeAgentGraphCompoundActivationProfile::TryMatchDefinition(
		const FUeAgentGraphCompoundActivationProfile& Expected, FDiagnostic& OutError, const FString& Stage) const
	{
		OutError.Reset();
		auto MatchString = [this, &OutError, &Stage](const TCHAR* Field, const FString& Actual, const FString& Wanted)
		{
			if (Actual == Wanted) return true;
			OutError = FDiagnostic::Failure(TEXT("graph_compound_activation_profile_not_canonical"),
				TEXT("The activation profile field differs from its registered definition"), Stage);
			OutError.SourceKind = TEXT("native_result");
			OutError.CandidateName = GetCommandId();
			OutError.PropertyPath = Field;
			OutError.ActualValue = MakeShared<FJsonValueString>(Actual);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(Wanted);
			OutError.ActualValuePresent = true;
			OutError.ExpectedValuePresent = true;
			return false;
		};
		if (!MatchString(TEXT("capability_id"), CapabilityId, Expected.CapabilityId)
			|| !MatchString(TEXT("namespace"), Namespace, Expected.Namespace)
			|| !MatchString(TEXT("method"), Method, Expected.Method)
			|| !MatchString(TEXT("descriptor_identity.descriptor_id"), DescriptorIdentity.GetDescriptorId(), Expected.DescriptorIdentity.GetDescriptorId()))
			return false;
		if (DescriptorIdentity.GetVersion() != Expected.DescriptorIdentity.GetVersion())
		{
			OutError = RegistryFailure(TEXT("graph_compound_activation_profile_not_canonical"),
				TEXT("The activation profile descriptor version differs from its registered definition"), TEXT("descriptor_identity.version"),
				MakeShared<FJsonValueNumber>(DescriptorIdentity.GetVersion()), MakeShared<FJsonValueNumber>(Expected.DescriptorIdentity.GetVersion()));
			OutError.Stage = Stage;
			OutError.CandidateName = GetCommandId();
			return false;
		}
		if (DescriptorIdentity.GetDescriptorHash() != Expected.DescriptorIdentity.GetDescriptorHash())
		{
			OutError = FDiagnostic::Failure(TEXT("graph_compound_activation_profile_not_canonical"),
				TEXT("The activation profile descriptor hash differs from its registered definition"), Stage);
			OutError.SourceKind = TEXT("native_result");
			OutError.CandidateName = GetCommandId();
			OutError.PropertyPath = TEXT("descriptor_identity.descriptor_hash");
			OutError.ActualEvidence = MakeShared<FJsonValueString>(DescriptorIdentity.GetDescriptorHash());
			OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Expected.DescriptorIdentity.GetDescriptorHash());
			return false;
		}
		return MatchString(TEXT("operation_kind"), OperationKind, Expected.OperationKind);
	}

	bool TryGetGraphCompoundActivationProfiles(
		const TArray<FUeAgentGraphCompoundActivationProfile>*& OutProfiles, FDiagnostic& OutError)
	{
		OutProfiles = nullptr;
		const auto& Bundle = ActivationBundle();
		OutError = Bundle.Error;
		if (!OutError.IsEmpty()) return false;
		OutProfiles = &Bundle.Profiles;
		return true;
	}

	bool TryCreateGraphCompoundDescriptorRegistry(
		TSharedPtr<const FUeAgentDescriptorRegistry>& OutRegistry,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutRegistry = ActivationBundle().Registry;
		OutError = ActivationBundle().Error;
		return OutRegistry.IsValid() && OutError.IsEmpty();
	}

	bool TryCreateGraphCompoundProductionCommandContractRegistry(
		TSharedPtr<const FUeAgentCommandContractRegistry>& OutRegistry,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		const TArray<FUeAgentGraphCompoundActivationProfile>* Profiles = nullptr;
		if (!TryGetGraphCompoundActivationProfiles(Profiles, OutError))
		{
			OutRegistry.Reset();
			return false;
		}
		return TryCreateGraphCompoundProductionCommandContractRegistry(*Profiles, OutRegistry, OutError);
	}

	bool TryCreateGraphCompoundProductionCommandContractRegistry(
		const TArray<FUeAgentGraphCompoundActivationProfile>& Profiles,
		TSharedPtr<const FUeAgentCommandContractRegistry>& OutRegistry,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutRegistry.Reset();
		OutError.Reset();
		TSharedPtr<const FUeAgentCommandContractRegistry> Base;
		if (!LongRunningSession::
				TryCreateLongRunningSessionProductionCommandContractRegistry(
					Base, OutError)
			|| !Base.IsValid())
		{
			if (OutError.IsEmpty()) OutError = RegistryFailure(TEXT("graph_compound_base_contract_registry_missing"),
				TEXT("The base command contract factory returned no usable registry"), TEXT("base_registry_present"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}
		const FActivationBundle& Bundle = ActivationBundle();
		if (!Bundle.Error.IsEmpty())
		{
			OutError = Bundle.Error;
			return false;
		}
		if (!Bundle.Registry.IsValid() || Profiles.IsEmpty())
		{
			OutError = RegistryFailure(TEXT("graph_compound_activation_bundle_invalid"),
				!Bundle.Registry.IsValid() ? TEXT("The activation bundle has no descriptor registry")
					: TEXT("At least one activation profile is required to build command contracts"),
				!Bundle.Registry.IsValid() ? TEXT("descriptor_registry_present") : TEXT("activation_profiles_present"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
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
			if (!Canonical)
			{
				OutError = RegistryFailure(TEXT("graph_compound_activation_profile_not_canonical"),
					TEXT("No registered activation profile exists for this command"), TEXT("canonical_profile_present"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.CandidateName = Profile.GetCommandId();
				return false;
			}
			if (!Profile.TryMatchDefinition(*Canonical, OutError, TEXT("registry"))) return false;
			if (CommandIds.Contains(Profile.GetCommandId()))
			{
				OutError = RegistryFailure(TEXT("graph_compound_activation_profile_not_canonical"),
					TEXT("The activation subset repeats a command"), TEXT("command_id_unique"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.CandidateName = Profile.GetCommandId();
				return false;
			}
			CommandIds.Add(Profile.GetCommandId());
			const FUeAgentOperationDescriptor* Descriptor =
				Bundle.Registry->FindExact(Profile.GetDescriptorIdentity());
			TArray<FUeAgentCommandParameter> Parameters;
			if (!Descriptor
				|| !TryPublicParameters(*Descriptor, Parameters, OutError))
			{
				if (OutError.IsEmpty()) OutError = RegistryFailure(TEXT("graph_compound_command_descriptor_missing"),
					!Descriptor ? TEXT("No operation descriptor matches the activation profile")
						: TEXT("The public parameter schema builder failed without a diagnostic"),
					!Descriptor ? TEXT("descriptor_present") : TEXT("parameter_schema_created"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Profile.GetCommandId();
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

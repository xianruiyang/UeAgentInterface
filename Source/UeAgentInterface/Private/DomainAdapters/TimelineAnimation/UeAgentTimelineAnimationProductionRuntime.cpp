#include "DomainAdapters/TimelineAnimation/UeAgentTimelineAnimationProductionRuntime.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Core/BindingGateway/Query/UeAgentDomainQuery.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "DomainAdapters/TimelineAnimation/UeAgentTimelineAnimationDescriptors.h"
#include "DomainAdapters/TimelineAnimation/UeAgentTimelineAnimationRequests.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::DomainAdapters::TimelineAnimation
{
	namespace
	{
		bool Fail(UE::UAI::Contract::FDiagnostic& OutError, UE::UAI::Contract::FDiagnostic Diagnostic, const FString& Subject = TEXT("timeline_animation.production_runtime"))
        { OutError = MoveTemp(Diagnostic); if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Subject; return false; }

		bool ValidateConfig(const FUeAgentTimelineAnimationProductionRuntimeConfig& Config,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutError.Reset();
			const auto RequireIdentity = [&](const FString& Value, const TCHAR* Field)
			{
				if (!Value.IsEmpty()) return true;
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("timeline_animation_runtime_config_invalid"),
					TEXT("The runtime configuration requires a non-empty identity."), TEXT("native_execution"));
				OutError.PropertyPath = Field;
				OutError.ActualValuePresent = true;
				OutError.ActualEvidence = MakeShared<FJsonValueString>(Value);
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty string"));
				return false;
			};
			if (!RequireIdentity(Config.ProjectInstanceId, TEXT("project_instance_id"))
				|| !RequireIdentity(Config.EditorInstanceId, TEXT("editor_instance_id"))
				|| !RequireIdentity(Config.ServerSessionId, TEXT("server_session_id"))) return false;
			if (Config.RegistryEpoch < 0)
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("timeline_animation_runtime_config_invalid"),
					TEXT("The runtime registry epoch must not be negative."), TEXT("native_execution"));
				OutError.PropertyPath = TEXT("registry_epoch");
				OutError.ActualValuePresent = true;
				OutError.ActualEvidence = MakeShared<FJsonValueNumber>(Config.RegistryEpoch);
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-negative integer"));
				return false;
			}
			TSet<FString> Identities;
			const auto ValidateBindings = [&](const auto& Bindings, const TCHAR* Collection)
			{
				for (int32 Index = 0; Index < Bindings.Num(); ++Index)
				{
					const auto& Binding = Bindings[Index];
					const FString Path = FString::Printf(TEXT("%s[%d]"), Collection, Index);
					if (!Binding.TryValidate(OutError))
					{
						OutError.PropertyPath = OutError.PropertyPath.IsEmpty() ? Path : Path + TEXT(".") + OutError.PropertyPath;
						return false;
					}
					if (Identities.Contains(Binding.Identity))
					{
						OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("timeline_animation_explicit_binding_duplicate"),
							TEXT("An explicit subject or rig candidate reference is registered more than once."), TEXT("native_execution"));
						OutError.PropertyPath = Path + TEXT(".identity");
						OutError.TargetPath = Binding.Identity;
						OutError.ActualValuePresent = true;
						OutError.ActualValue = MakeShared<FJsonValueString>(Binding.Identity);
						OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("unique reference across subject and rig candidate bindings"));
						return false;
					}
					Identities.Add(Binding.Identity);
				}
				return true;
			};
			return ValidateBindings(Config.SubjectBindings, TEXT("subject_bindings"))
				&& ValidateBindings(Config.RigCandidateBindings, TEXT("rig_candidate_bindings"));
		}
	}

	bool FUeAgentTimelineAnimationProductionRuntimeConfig::IsValid() const
	{
		UE::UAI::Contract::FDiagnostic Error;
		return ValidateConfig(*this, Error);
	}

	FUeAgentTimelineAnimationProductionRuntime::
	FUeAgentTimelineAnimationProductionRuntime(
		TSharedPtr<const Contract::FUeAgentDescriptorRegistry> InDescriptors,
		TSharedPtr<const BindingGateway::Query::FUeAgentDomainQueryProviderRegistry>
			InQueries,
		TArray<TSharedRef<FUeAgentTimelineAnimationOperationChain>> InOperations,
		FUeAgentTimelineAnimationNativeDependencies InDependencies)
		: Descriptors(MoveTemp(InDescriptors))
		, Queries(MoveTemp(InQueries))
		, Operations(MoveTemp(InOperations))
		, Dependencies(MoveTemp(InDependencies))
	{
	}

	bool FUeAgentTimelineAnimationProductionRuntime::TryCreate(
		const FUeAgentTimelineAnimationProductionRuntimeConfig& Config,
		TSharedPtr<FUeAgentTimelineAnimationProductionRuntime>& OutRuntime,
		FUeAgentTimelineAnimationExecutionError& OutError)
	{
		OutRuntime.Reset();
		OutError = {};
		if (!ValidateConfig(Config, OutError))
		{
			if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = TEXT("timeline_animation.production_runtime");
			return false;
		}

		TSharedPtr<const Contract::FUeAgentDescriptorRegistry> DescriptorRegistry;
		UE::UAI::Contract::FDiagnostic DescriptorError;
		if (!TryCreateTimelineAnimationDescriptorRegistry(
				DescriptorRegistry, DescriptorError) || !DescriptorRegistry.IsValid())
		{
			if (DescriptorError.IsEmpty()) DescriptorError = UE::UAI::Contract::FDiagnostic::Failure(
				TEXT("timeline_animation_descriptor_registry_missing"), TEXT("The descriptor factory did not return a registry."), TEXT("native_execution"));
			return Fail(OutError, DescriptorError);
		}
		TSharedPtr<IUeAgentTimelineAnimationRequestDecoder> Decoder;
		if (!TryCreateTimelineAnimationRequestDecoder(Decoder, OutError)
			|| !Decoder.IsValid())
		{
			if (OutError.IsEmpty()) OutError = UE::UAI::Contract::FDiagnostic::Failure(
				TEXT("timeline_animation_decoder_missing"), TEXT("The request decoder factory did not return a decoder."), TEXT("native_execution"));
			return false;
		}

		const FString Generation = FString::Printf(TEXT("%s:%s:%s:%lld"),
			*Config.ProjectInstanceId, *Config.EditorInstanceId,
			*Config.ServerSessionId, Config.RegistryEpoch);
		FUeAgentTimelineAnimationNativeDependencies NativeDependencies;
		if (!TryCreateTimelineAnimationProductionDependencies(
				Generation, Config.ServerSessionId, Config.SubjectBindings,
				Config.RigCandidateBindings, NativeDependencies, OutError)) return false;
		FUeAgentTimelineAnimationNativePorts Ports;
		if (!TryCreateTimelineAnimationNativePorts(
				NativeDependencies, Ports, OutError)) return false;

		TSharedPtr<const BindingGateway::Query::FUeAgentDomainQueryProviderRegistry>
			QueryRegistry;
		if (!TryCreateTimelineAnimationQueryProviderRegistry(
				Decoder.ToSharedRef(), Ports.Readback.ToSharedRef(), QueryRegistry,
				OutError) || !QueryRegistry.IsValid())
		{
			if (OutError.IsEmpty()) OutError = UE::UAI::Contract::FDiagnostic::Failure(
				TEXT("timeline_animation_query_registry_missing"), TEXT("The query provider factory did not return a registry."), TEXT("native_execution"));
			return false;
		}
		TArray<TSharedRef<FUeAgentTimelineAnimationOperationChain>> OperationChains;
		if (!TryCreateTimelineAnimationOperationChains(
				Decoder.ToSharedRef(), Ports.Readback.ToSharedRef(),
				Ports.Mutation.ToSharedRef(), Ports.Settle.ToSharedRef(),
				Ports.Comparator.ToSharedRef(), OperationChains, OutError)) return false;

		OutRuntime = MakeShareable(new FUeAgentTimelineAnimationProductionRuntime(
			MoveTemp(DescriptorRegistry), MoveTemp(QueryRegistry),
			MoveTemp(OperationChains), MoveTemp(NativeDependencies)));
		if (!OutRuntime.IsValid()) return Fail(OutError, UE::UAI::Contract::FDiagnostic::Failure(
			TEXT("timeline_animation_runtime_missing"), TEXT("The factory did not produce a timeline animation runtime."), TEXT("native_execution")));
		return true;
	}

	const TSharedPtr<const Contract::FUeAgentDescriptorRegistry>&
	FUeAgentTimelineAnimationProductionRuntime::GetDescriptorRegistry() const
	{
		return Descriptors;
	}

	const TSharedPtr<const BindingGateway::Query::FUeAgentDomainQueryProviderRegistry>&
	FUeAgentTimelineAnimationProductionRuntime::GetQueryProviderRegistry() const
	{
		return Queries;
	}

	const TArray<TSharedRef<FUeAgentTimelineAnimationOperationChain>>&
	FUeAgentTimelineAnimationProductionRuntime::GetOperationChains() const
	{
		return Operations;
	}

	bool FUeAgentTimelineAnimationProductionRuntime::TryReadRevision(
		const FString& ExplicitRef, FTimelineAnimationRevision& OutRevision,
		FUeAgentTimelineAnimationExecutionError& OutError) const
	{
		if (!Dependencies.RevisionSource.IsValid())
		{
			OutRevision = {};
			return Fail(OutError, UE::UAI::Contract::FDiagnostic::Failure(TEXT("timeline_animation_revision_source_unavailable"), ExplicitRef, TEXT("native_execution")));
		}
		return Dependencies.RevisionSource->TryReadRevision(
			ExplicitRef, OutRevision, OutError);
	}

	void FUeAgentTimelineAnimationProductionRuntime::GetExplicitSubjectIdentities(
		TArray<FString>& OutIds) const
	{
		OutIds.Reset();
		if (Dependencies.Environment.IsValid())
			Dependencies.Environment->GetExplicitSubjectIdentities(OutIds);
	}

	void FUeAgentTimelineAnimationProductionRuntime::
	GetExplicitRigCandidateIdentities(TArray<FString>& OutIds) const
	{
		OutIds.Reset();
		if (Dependencies.Environment.IsValid())
			Dependencies.Environment->GetExplicitRigCandidateIdentities(OutIds);
	}
}

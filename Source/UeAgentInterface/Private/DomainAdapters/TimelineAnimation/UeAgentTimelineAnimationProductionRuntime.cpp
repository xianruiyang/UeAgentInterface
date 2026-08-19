#include "DomainAdapters/TimelineAnimation/UeAgentTimelineAnimationProductionRuntime.h"

#include "Core/BindingGateway/Query/UeAgentDomainQuery.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "DomainAdapters/TimelineAnimation/UeAgentTimelineAnimationDescriptors.h"
#include "DomainAdapters/TimelineAnimation/UeAgentTimelineAnimationRequests.h"

namespace UE::UAI::DomainAdapters::TimelineAnimation
{
	namespace
	{
		bool Fail(FUeAgentTimelineAnimationExecutionError& OutError,
			const FString& Code, const FString& Detail = FString())
		{
			OutError = {};
			OutError.Code = Code;
			OutError.SubjectId = TEXT("timeline_animation.production_runtime");
			OutError.Detail = Detail;
			return false;
		}
	}

	bool FUeAgentTimelineAnimationProductionRuntimeConfig::IsValid() const
	{
		if (ProjectInstanceId.IsEmpty() || EditorInstanceId.IsEmpty()
			|| ServerSessionId.IsEmpty() || RegistryEpoch < 0)
		{
			return false;
		}
		TSet<FString> Identities;
		for (const FUeAgentTimelineAnimationSubjectBinding& Binding : SubjectBindings)
		{
			if (!Binding.IsValid() || Identities.Contains(Binding.Identity)) return false;
			Identities.Add(Binding.Identity);
		}
		for (const FUeAgentTimelineAnimationRigCandidateBinding& Binding
			: RigCandidateBindings)
		{
			if (!Binding.IsValid() || Identities.Contains(Binding.Identity)) return false;
			Identities.Add(Binding.Identity);
		}
		return true;
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
		if (!Config.IsValid()) return Fail(OutError,
			TEXT("timeline_animation_runtime_config_invalid"));

		TSharedPtr<const Contract::FUeAgentDescriptorRegistry> DescriptorRegistry;
		FString DescriptorError;
		if (!TryCreateTimelineAnimationDescriptorRegistry(
				DescriptorRegistry, DescriptorError) || !DescriptorRegistry.IsValid())
		{
			return Fail(OutError,
				TEXT("timeline_animation_descriptor_registry_unavailable"),
				DescriptorError);
		}
		TSharedPtr<IUeAgentTimelineAnimationRequestDecoder> Decoder;
		if (!TryCreateTimelineAnimationRequestDecoder(Decoder, OutError)
			|| !Decoder.IsValid()) return false;

		const FString Generation = FString::Printf(TEXT("%s:%s:%s:%lld"),
			*Config.ProjectInstanceId, *Config.EditorInstanceId,
			*Config.ServerSessionId, Config.RegistryEpoch);
		FUeAgentTimelineAnimationNativeDependencies NativeDependencies;
		if (!TryCreateTimelineAnimationProductionDependencies(
				Generation, Config.ServerSessionId, Config.SubjectBindings,
				Config.RigCandidateBindings, NativeDependencies, OutError)) return false;
		FUeAgentTimelineAnimationNativePorts Ports;
		if (!TryCreateTimelineAnimationNativePorts(
				NativeDependencies, Ports, OutError) || !Ports.IsValid()) return false;

		TSharedPtr<const BindingGateway::Query::FUeAgentDomainQueryProviderRegistry>
			QueryRegistry;
		if (!TryCreateTimelineAnimationQueryProviderRegistry(
				Decoder.ToSharedRef(), Ports.Readback.ToSharedRef(), QueryRegistry,
				OutError) || !QueryRegistry.IsValid()) return false;
		TArray<TSharedRef<FUeAgentTimelineAnimationOperationChain>> OperationChains;
		if (!TryCreateTimelineAnimationOperationChains(
				Decoder.ToSharedRef(), Ports.Readback.ToSharedRef(),
				Ports.Mutation.ToSharedRef(), Ports.Settle.ToSharedRef(),
				Ports.Comparator.ToSharedRef(), OperationChains, OutError)) return false;

		OutRuntime = MakeShareable(new FUeAgentTimelineAnimationProductionRuntime(
			MoveTemp(DescriptorRegistry), MoveTemp(QueryRegistry),
			MoveTemp(OperationChains), MoveTemp(NativeDependencies)));
		return OutRuntime.IsValid();
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
			return Fail(OutError,
				TEXT("timeline_animation_revision_source_unavailable"), ExplicitRef);
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

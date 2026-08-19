#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialProductionRuntime.h"

#include "Core/BindingGateway/Query/UeAgentDomainQuery.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialDescriptors.h"
#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialNative.h"
#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialProductionDependencies.h"
#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialRequests.h"
#include "Engine/World.h"

namespace UE::UAI::DomainAdapters::WorldSpatial
{
	namespace
	{
		bool Fail(
			FUeAgentWorldSpatialExecutionError& OutError,
			const FString& Code,
			const FString& Detail = FString())
		{
			OutError.Code = Code;
			OutError.SubjectId = TEXT("world_spatial.production_runtime");
			OutError.Detail = Detail;
			return false;
		}
	}

	bool FUeAgentWorldSpatialProductionRuntimeConfig::IsValid() const
	{
		if (ProjectInstanceId.IsEmpty()
			|| EditorInstanceId.IsEmpty()
			|| ServerSessionId.IsEmpty()
			|| RegistryEpoch < 0)
		{
			return false;
		}
		TSet<FString> ViewportIds;
		for (const FUeAgentWorldSpatialViewportBinding& Binding : ViewportBindings)
		{
			if (Binding.Identity.IsEmpty() || ViewportIds.Contains(Binding.Identity))
			{
				return false;
			}
			ViewportIds.Add(Binding.Identity);
		}
		TSet<FString> SelectionViewportIds;
		for (const FUeAgentWorldSpatialMeshSelectionBinding& Binding
			: MeshSelectionBindings)
		{
			if (!Binding.IsValid()
				|| !ViewportIds.Contains(Binding.ViewportIdentity)
				|| SelectionViewportIds.Contains(Binding.ViewportIdentity))
			{
				return false;
			}
			SelectionViewportIds.Add(Binding.ViewportIdentity);
		}
		return !ProjectInstanceId.IsEmpty()
			&& !EditorInstanceId.IsEmpty()
			&& !ServerSessionId.IsEmpty()
			&& RegistryEpoch >= 0;
	}

	FUeAgentWorldSpatialProductionRuntime::FUeAgentWorldSpatialProductionRuntime(
		TSharedPtr<const Contract::FUeAgentDescriptorRegistry>
			InDescriptorRegistry,
		TSharedPtr<const BindingGateway::Query::
			FUeAgentDomainQueryProviderRegistry> InQueryProviderRegistry,
		TArray<TSharedRef<FUeAgentWorldSpatialOperationChain>>
			InOperationChains,
		FUeAgentWorldSpatialNativeDependencies InNativeDependencies)
		: DescriptorRegistry(MoveTemp(InDescriptorRegistry))
		, QueryProviderRegistry(MoveTemp(InQueryProviderRegistry))
		, OperationChains(MoveTemp(InOperationChains))
		, NativeDependencies(MoveTemp(InNativeDependencies))
	{
	}

	bool FUeAgentWorldSpatialProductionRuntime::TryCreate(
		const FUeAgentWorldSpatialProductionRuntimeConfig& Config,
		UWorld& ExplicitWorld,
		TSharedPtr<FUeAgentWorldSpatialProductionRuntime>& OutRuntime,
		FUeAgentWorldSpatialExecutionError& OutError)
	{
		OutRuntime.Reset();
		OutError = {};
		if (!Config.IsValid())
		{
			return Fail(OutError, TEXT("world_spatial_runtime_config_invalid"));
		}
		if (!IsValid(&ExplicitWorld) || ExplicitWorld.IsUnreachable())
		{
			return Fail(OutError, TEXT("world_spatial_explicit_world_invalid"));
		}

		TSharedPtr<const Contract::FUeAgentDescriptorRegistry> Descriptors;
		FString DescriptorError;
		if (!TryCreateWorldSpatialDescriptorRegistry(Descriptors, DescriptorError)
			|| !Descriptors.IsValid())
		{
			return Fail(
				OutError,
				TEXT("world_spatial_descriptor_registry_unavailable"),
				DescriptorError);
		}

		TSharedPtr<IUeAgentWorldSpatialRequestDecoder> Decoder;
		if (!TryCreateWorldSpatialRequestDecoder(Decoder, OutError)
			|| !Decoder.IsValid())
		{
			return false;
		}

		const FString RuntimeGeneration = FString::Printf(
			TEXT("%s:%s:%s:%lld"),
			*Config.ProjectInstanceId,
			*Config.EditorInstanceId,
			*Config.ServerSessionId,
			Config.RegistryEpoch);
		FUeAgentWorldSpatialNativeDependencies Dependencies;
		if (!TryCreateWorldSpatialProductionDependencies(
				ExplicitWorld,
				RuntimeGeneration,
				Config.ServerSessionId,
				Dependencies,
				OutError,
				Config.ViewportBindings,
				Config.MeshSelectionBindings))
		{
			return false;
		}

		FUeAgentWorldSpatialNativePorts Ports;
		if (!TryCreateWorldSpatialNativePorts(Dependencies, Ports, OutError)
			|| !Ports.IsValid())
		{
			return false;
		}

		TSharedPtr<const BindingGateway::Query::
			FUeAgentDomainQueryProviderRegistry> QueryProviders;
		if (!TryCreateWorldSpatialQueryProviderRegistry(
				Decoder.ToSharedRef(),
				Ports.Readback.ToSharedRef(),
				QueryProviders,
				OutError)
			|| !QueryProviders.IsValid())
		{
			return false;
		}

		TArray<TSharedRef<FUeAgentWorldSpatialOperationChain>> Chains;
		if (!TryCreateWorldSpatialOperationChains(
				Decoder.ToSharedRef(),
				Ports.Readback.ToSharedRef(),
				Ports.Mutation.ToSharedRef(),
				Ports.Settle.ToSharedRef(),
				Ports.Comparator.ToSharedRef(),
				Chains,
				OutError))
		{
			return false;
		}

		OutRuntime = MakeShareable(new FUeAgentWorldSpatialProductionRuntime(
			MoveTemp(Descriptors),
			MoveTemp(QueryProviders),
			MoveTemp(Chains),
			MoveTemp(Dependencies)));
		return OutRuntime.IsValid();
	}

	const TSharedPtr<const Contract::FUeAgentDescriptorRegistry>&
	FUeAgentWorldSpatialProductionRuntime::GetDescriptorRegistry() const
	{
		return DescriptorRegistry;
	}

	const TSharedPtr<const BindingGateway::Query::
		FUeAgentDomainQueryProviderRegistry>&
	FUeAgentWorldSpatialProductionRuntime::GetQueryProviderRegistry() const
	{
		return QueryProviderRegistry;
	}

	const TArray<TSharedRef<FUeAgentWorldSpatialOperationChain>>&
	FUeAgentWorldSpatialProductionRuntime::GetOperationChains() const
	{
		return OperationChains;
	}

	bool FUeAgentWorldSpatialProductionRuntime::TryReadRevision(
		const FString& ExplicitRef,
		FWorldSpatialRevision& OutRevision,
		FUeAgentWorldSpatialExecutionError& OutError) const
	{
		if (!NativeDependencies.RevisionSource.IsValid())
		{
			OutRevision = {};
			return Fail(
				OutError,
				TEXT("world_spatial_revision_source_unavailable"),
				ExplicitRef);
		}
		return NativeDependencies.RevisionSource->TryReadRevision(
			ExplicitRef, OutRevision, OutError);
	}

	void FUeAgentWorldSpatialProductionRuntime::GetExplicitViewportIdentities(
		TArray<FString>& OutViewportIdentities) const
	{
		OutViewportIdentities.Reset();
		if (NativeDependencies.Environment.IsValid())
		{
			NativeDependencies.Environment->GetExplicitViewportIdentities(
				OutViewportIdentities);
		}
	}
}

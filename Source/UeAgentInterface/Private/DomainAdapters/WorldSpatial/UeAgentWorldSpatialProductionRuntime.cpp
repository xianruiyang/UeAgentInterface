#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialProductionRuntime.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Core/BindingGateway/Query/UeAgentDomainQuery.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialDescriptors.h"
#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialNative.h"
#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialProductionDependencies.h"
#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialRequests.h"
#include "Engine/World.h"
#include "Dom/JsonValue.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

namespace UE::UAI::DomainAdapters::WorldSpatial
{
	namespace
	{
		bool Fail(UE::UAI::Contract::FDiagnostic& OutError, UE::UAI::Contract::FDiagnostic Diagnostic, const FString& Subject = TEXT("world_spatial.production_runtime"))
        { OutError = MoveTemp(Diagnostic); if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Subject; return false; }

		bool ConfigFailure(FUeAgentWorldSpatialExecutionError& OutError, const TCHAR* Reason,
			const FString& Path, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, bool bPublicValue = true)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("world_spatial_runtime_config_invalid"), Reason, TEXT("native_execution"));
			OutError.PropertyPath = Path;
			OutError.SourceKind = TEXT("native_configuration");
			OutError.Source.JsonPath = Path;
			OutError.ActualValuePresent = Actual.IsValid();
			if (bPublicValue) OutError.ActualValue = MoveTemp(Actual);
			else OutError.ActualEvidence = MoveTemp(Actual);
			OutError.ExpectedValue = MoveTemp(Expected);
			return false;
		}
	}

	bool FUeAgentWorldSpatialProductionRuntimeConfig::TryValidate(FUeAgentWorldSpatialExecutionError& OutError) const
	{
		OutError = {};
		if (ProjectInstanceId.IsEmpty()
			|| EditorInstanceId.IsEmpty()
			|| ServerSessionId.IsEmpty())
		{
			const TCHAR* Path = ProjectInstanceId.IsEmpty() ? TEXT("/project_instance_id")
				: (EditorInstanceId.IsEmpty() ? TEXT("/editor_instance_id") : TEXT("/server_session_id"));
			return ConfigFailure(OutError, TEXT("The runtime configuration is missing a required instance or session binding."), Path,
				MakeShared<FJsonValueString>(FString()), MakeShared<FJsonValueString>(TEXT("non-empty binding")), false);
		}
		if (RegistryEpoch < 0)
			return ConfigFailure(OutError, TEXT("The runtime registry epoch must be non-negative."), TEXT("/registry_epoch"),
				MakeShared<FJsonValueString>(::LexToString(RegistryEpoch)), MakeShared<FJsonValueString>(TEXT("non-negative integer")), false);
		TSet<FString> ViewportIds;
		for (int32 Index = 0; Index < ViewportBindings.Num(); ++Index)
		{
			const auto& Binding = ViewportBindings[Index];
			if (Binding.Identity.IsEmpty() || ViewportIds.Contains(Binding.Identity))
			{
				return ConfigFailure(OutError, Binding.Identity.IsEmpty() ? TEXT("The runtime viewport identity is empty.")
					: TEXT("The runtime configuration repeats a viewport identity."), FString::Printf(TEXT("/viewport_bindings/%d/identity"), Index),
					MakeShared<FJsonValueString>(Binding.Identity), MakeShared<FJsonValueString>(TEXT("unique non-empty viewport identity")));
			}
			ViewportIds.Add(Binding.Identity);
		}
		TSet<FString> SelectionViewportIds;
		for (int32 Index = 0; Index < MeshSelectionBindings.Num(); ++Index)
		{
			const auto& Binding = MeshSelectionBindings[Index];
			const FString Path = FString::Printf(TEXT("/mesh_selection_bindings/%d"), Index);
			if (!Binding.TryValidate(OutError))
			{
				UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, Path);
				return false;
			}
			if (!ViewportIds.Contains(Binding.ViewportIdentity) || SelectionViewportIds.Contains(Binding.ViewportIdentity))
				return ConfigFailure(OutError, !ViewportIds.Contains(Binding.ViewportIdentity)
					? TEXT("The mesh selection binding references a viewport absent from the runtime configuration.")
					: TEXT("The runtime configuration repeats a mesh selection viewport binding."), Path + TEXT("/viewport_identity"),
					MakeShared<FJsonValueString>(Binding.ViewportIdentity), MakeShared<FJsonValueString>(TEXT("unique registered viewport identity")));
			SelectionViewportIds.Add(Binding.ViewportIdentity);
		}
		return true;
	}

	bool FUeAgentWorldSpatialProductionRuntimeConfig::IsValid() const
	{
		FUeAgentWorldSpatialExecutionError Error;
		return TryValidate(Error);
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
		if (!Config.TryValidate(OutError)) return false;
		if (!IsValid(&ExplicitWorld) || ExplicitWorld.IsUnreachable())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("world_spatial_explicit_world_invalid"),
				TEXT("The explicit runtime world is invalid or unreachable."), TEXT("native_execution"));
			OutError.PropertyPath = TEXT("/world");
			OutError.SourceKind = TEXT("native_configuration");
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("valid reachable world"));
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
			OutError.ActualValuePresent = true;
			return false;
		}

		TSharedPtr<const Contract::FUeAgentDescriptorRegistry> Descriptors;
		UE::UAI::Contract::FDiagnostic DescriptorError;
		if (!TryCreateWorldSpatialDescriptorRegistry(Descriptors, DescriptorError)
			|| !Descriptors.IsValid())
		{
			return Fail(OutError, DescriptorError);
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
		if (!TryCreateWorldSpatialNativePorts(Dependencies, Ports, OutError))
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
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("world_spatial_revision_source_unavailable"),
				TEXT("World-spatial runtime has no native revision source."), TEXT("native_execution"), FString(), ExplicitRef);
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = TEXT("/revision_source");
			Error.ActualValue = MakeShared<FJsonValueBoolean>(false);
			Error.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			return Fail(OutError, MoveTemp(Error));
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

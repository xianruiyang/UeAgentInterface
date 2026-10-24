#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProvider.h"

#include "Core/BindingGateway/Query/UeAgentQueryBinding.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Core/Projection/UeAgentProjection.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::DomainAdapters::ContentGraphRead
{
	using UE::UAI::BindingGateway::Query::
		FUeAgentQueryBindingError;
	using UE::UAI::Contract::FUeAgentContractValue;
	using UE::UAI::Contract::FUeAgentQueryDescriptor;
	using UE::UAI::ContextResource::FUeAgentOperationContext;
	using UE::UAI::Discovery::FUeAgentCandidateBinding;
	using UE::UAI::Projection::FUeAgentProjectionSourceValue;
	using UE::UAI::StateSemantic::FUeAgentCanonicalSnapshot;

	namespace
	{
		bool Fail(FUeAgentQueryBindingError& OutError, UE::UAI::Contract::FDiagnostic Diagnostic)
		{
			OutError = MoveTemp(Diagnostic);
			return false;
		}
	}

	FUeAgentContentGraphReadProvider::
		FUeAgentContentGraphReadProvider(
			FString InProviderBinding,
			TSharedRef<IUeAgentContentGraphReadHook> InHook)
		: ProviderBinding(MoveTemp(InProviderBinding))
		, Hook(MoveTemp(InHook))
	{
		ProviderBinding.TrimStartAndEndInline();
	}

	const FString&
	FUeAgentContentGraphReadProvider::GetProviderBinding() const
	{
		return ProviderBinding;
	}

	bool FUeAgentContentGraphReadProvider::TryQuery(
		const FUeAgentQueryDescriptor& Descriptor,
		const FUeAgentContractValue& CanonicalInput,
		const TOptional<FUeAgentCandidateBinding>& Target,
		const FUeAgentOperationContext& Context,
		TSharedPtr<const FUeAgentProjectionSourceValue>& OutSource,
		FUeAgentQueryBindingError& OutError)
	{
		OutSource.Reset();
		OutError = {};
		if (ProviderBinding.IsEmpty()
			|| Descriptor.GetProviderBinding() != ProviderBinding)
		{
			auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("content_graph_read_provider_binding_mismatch"),
				TEXT("The query cannot use this content-graph provider because its binding is missing or differs from the descriptor binding."),
				TEXT("content_graph_read"), ProviderBinding, Descriptor.GetProviderBinding());
			Diagnostic.SourceKind = TEXT("native_result");
			Diagnostic.PropertyPath = TEXT("/provider_binding");
			auto Facts = MakeShared<FJsonObject>();
			Facts->SetBoolField(TEXT("provider_binding_present"), !ProviderBinding.IsEmpty());
			Facts->SetBoolField(TEXT("bindings_match"), Descriptor.GetProviderBinding() == ProviderBinding);
			Diagnostic.ActualValue = MakeShared<FJsonValueObject>(Facts);
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("a configured provider binding matching the query descriptor"));
			return Fail(OutError, MoveTemp(Diagnostic));
		}

		TSharedPtr<const FUeAgentCanonicalSnapshot> Snapshot;
		FUeAgentContentGraphReadError HookError;
		if (!Hook->TryCapture(
				CanonicalInput,
				Target,
				Context,
				Snapshot,
				HookError))
		{
			if (HookError.IsEmpty())
			{
				HookError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("content_graph_read_capture_failed"),
					TEXT("The content-graph capture hook failed without returning a diagnostic."), TEXT("content_graph_read"), ProviderBinding);
				HookError.SourceKind = TEXT("native_result");
			}
			return Fail(OutError, MoveTemp(HookError));
		}
		if (!Snapshot.IsValid())
		{
			auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("content_graph_read_snapshot_missing"),
				TEXT("The content-graph capture hook returned success without a canonical snapshot."), TEXT("content_graph_read"), FString(), ProviderBinding);
			Diagnostic.SourceKind = TEXT("native_result");
			Diagnostic.PropertyPath = TEXT("/snapshot");
			Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
			Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			return Fail(OutError, MoveTemp(Diagnostic));
		}

		OutSource = MakeShared<FUeAgentProjectionSourceValue>(
			Snapshot.ToSharedRef());
		return true;
	}
}

#include "DomainAdapters/WorldRead/UeAgentWorldReadProvider.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/ScopeExit.h"

#include "Core/BindingGateway/Query/UeAgentQueryBinding.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Core/Projection/UeAgentProjection.h"

namespace UE::UAI::DomainAdapters::WorldRead
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
		bool Fail(
			FUeAgentQueryBindingError& OutError,
			FUeAgentQueryBindingError Diagnostic)
		{
			OutError = MoveTemp(Diagnostic);
			return false;
		}
	}

	FUeAgentWorldReadProvider::FUeAgentWorldReadProvider(
		FString InProviderBinding,
		TSharedRef<IUeAgentWorldReadHook> InHook)
		: ProviderBinding(MoveTemp(InProviderBinding))
		, Hook(MoveTemp(InHook))
	{
		ProviderBinding.TrimStartAndEndInline();
	}

	const FString&
	FUeAgentWorldReadProvider::GetProviderBinding() const
	{
		return ProviderBinding;
	}

	bool FUeAgentWorldReadProvider::TryQuery(
		const FUeAgentQueryDescriptor& Descriptor,
		const FUeAgentContractValue& CanonicalInput,
		const TOptional<FUeAgentCandidateBinding>& Target,
		const FUeAgentOperationContext& Context,
		TSharedPtr<const FUeAgentProjectionSourceValue>& OutSource,
		FUeAgentQueryBindingError& OutError)
	{
		OutSource.Reset();
		OutError = {};
		ON_SCOPE_EXIT { if (!OutError.IsEmpty() && OutError.TargetPath.IsEmpty() && Target.IsSet()) OutError.TargetPath = Target->GetPublicRef(); };
		if (ProviderBinding.IsEmpty()
			|| Descriptor.GetProviderBinding() != ProviderBinding)
		{
			auto Diagnostic = FUeAgentQueryBindingError::Failure(TEXT("world_read_provider_binding_mismatch"),
				TEXT("World read provider must be configured and match the query descriptor binding."), TEXT("world_read"),
				ProviderBinding, Descriptor.GetProviderBinding());
			Diagnostic.SourceKind = TEXT("native_result");
			Diagnostic.PropertyPath = TEXT("/provider_binding");
			auto Actual = MakeShared<FJsonObject>();
			Actual->SetBoolField(TEXT("provider_configured"), !ProviderBinding.IsEmpty());
			Actual->SetBoolField(TEXT("bindings_match"), Descriptor.GetProviderBinding() == ProviderBinding);
			Diagnostic.ActualValue = MakeShared<FJsonValueObject>(Actual);
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("configured matching provider"));
			return Fail(OutError, MoveTemp(Diagnostic));
		}

		TSharedPtr<const FUeAgentCanonicalSnapshot> Snapshot;
		FUeAgentWorldReadError HookError;
		if (!Hook->TryCapture(
				CanonicalInput,
				Target,
				Context,
				Snapshot,
				HookError))
		{
			if (HookError.IsEmpty())
			{
				HookError = FUeAgentQueryBindingError::Failure(TEXT("world_read_capture_failed"),
					TEXT("World read hook failed without returning a diagnostic."), TEXT("world_read"), FString(), ProviderBinding);
				HookError.SourceKind = TEXT("native_result");
			}
			return Fail(OutError, MoveTemp(HookError));
		}
		if (!Snapshot.IsValid())
		{
			auto Diagnostic = FUeAgentQueryBindingError::Failure(TEXT("world_read_snapshot_missing"),
				TEXT("World read hook succeeded without a canonical snapshot."), TEXT("world_read"), FString(), ProviderBinding);
			Diagnostic.SourceKind = TEXT("native_result");
			Diagnostic.PropertyPath = TEXT("/snapshot");
			Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
			Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			return Fail(OutError, MoveTemp(Diagnostic));
		}

		OutSource =
			MakeShared<FUeAgentProjectionSourceValue>(
				Snapshot.ToSharedRef());
		return true;
	}
}

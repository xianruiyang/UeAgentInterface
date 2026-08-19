#include "DomainAdapters/WorldRead/UeAgentWorldReadProvider.h"

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
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString())
		{
			OutError = {};
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
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
		if (ProviderBinding.IsEmpty()
			|| Descriptor.GetProviderBinding() != ProviderBinding)
		{
			return Fail(
				OutError,
				TEXT("world_read_provider_binding_mismatch"),
				Descriptor.GetProviderBinding(),
				ProviderBinding);
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
			return Fail(
				OutError,
				HookError.Code.IsEmpty()
					? TEXT("world_read_capture_failed")
					: HookError.Code,
				HookError.SubjectId,
				HookError.Detail);
		}
		if (!Snapshot.IsValid())
		{
			return Fail(
				OutError,
				TEXT("world_read_snapshot_missing"),
				ProviderBinding);
		}

		OutSource =
			MakeShared<FUeAgentProjectionSourceValue>(
				Snapshot.ToSharedRef());
		return true;
	}
}

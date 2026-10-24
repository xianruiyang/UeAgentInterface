#include "DomainAdapters/AssetProjectRead/UeAgentAssetProjectReadProvider.h"

#include "Core/BindingGateway/Query/UeAgentQueryBinding.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Core/Projection/UeAgentProjection.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::DomainAdapters::AssetProjectRead
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
		UE::UAI::Contract::FDiagnostic ProviderCondition(const FString& Code,
			const FString& Reason, const FString& Field)
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("asset_project_read"));
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Field;
			Error.ActualValue = MakeShared<FJsonValueBoolean>(false);
			Error.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			Error.ActualValuePresent = true;
			Error.ExpectedValuePresent = true;
			return Error;
		}

	}

	FUeAgentAssetProjectReadProvider::
		FUeAgentAssetProjectReadProvider(
			FString InProviderBinding,
			TSharedRef<IUeAgentAssetProjectReadHook> InHook)
		: ProviderBinding(MoveTemp(InProviderBinding))
		, Hook(MoveTemp(InHook))
	{
		ProviderBinding.TrimStartAndEndInline();
	}

	const FString&
	FUeAgentAssetProjectReadProvider::GetProviderBinding() const
	{
		return ProviderBinding;
	}

	bool FUeAgentAssetProjectReadProvider::TryQuery(
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
			OutError = ProviderCondition(TEXT("asset_project_read_provider_binding_mismatch"),
				ProviderBinding.IsEmpty() ? TEXT("The native query provider has no configured binding.")
					: TEXT("The query descriptor selects a different native provider."),
				ProviderBinding.IsEmpty() ? TEXT("/provider/configured") : TEXT("/provider/binding_matches"));
			OutError.ActualEvidence = MakeShared<FJsonValueString>(Descriptor.GetProviderBinding());
			OutError.ExpectedEvidence = MakeShared<FJsonValueString>(ProviderBinding);
			if (Target.IsSet()) OutError.TargetPath = Target.GetValue().GetPublicRef();
			return false;
		}

		TSharedPtr<const FUeAgentCanonicalSnapshot> Snapshot;
		FUeAgentAssetProjectReadError HookError;
		if (!Hook->TryCapture(
				CanonicalInput,
				Target,
				Context,
				Snapshot,
				HookError))
		{
			OutError = HookError.IsValid() ? MoveTemp(HookError) : ProviderCondition(
				TEXT("asset_project_read_capture_failed"), TEXT("The native query hook failed to capture project state."), TEXT("/capture/succeeded"));
			if (OutError.TargetPath.IsEmpty() && Target.IsSet()) OutError.TargetPath = Target.GetValue().GetPublicRef();
			return false;
		}
		if (!Snapshot.IsValid())
		{
			OutError = ProviderCondition(TEXT("asset_project_read_snapshot_missing"),
				TEXT("The native query hook reported success without returning a snapshot."), TEXT("/snapshot/available"));
			OutError.SubjectId = ProviderBinding;
			if (Target.IsSet()) OutError.TargetPath = Target.GetValue().GetPublicRef();
			return false;
		}

		OutSource = MakeShared<FUeAgentProjectionSourceValue>(
			Snapshot.ToSharedRef());
		return true;
	}
}

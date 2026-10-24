#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "Core/Contract/UeAgentDiagnostic.h"

#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "Core/HjsonWorkspace/UeAgentSemanticLayout.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::HjsonWorkspace
{
	namespace
	{
		bool IsExactIdentity(const FString& Value)
		{
			FString Normalized = Value;
			Normalized.TrimStartAndEndInline();
			return !Normalized.IsEmpty() && Normalized == Value;
		}

		Contract::FDiagnostic RegistryFailure(const FString& Code, const FString& Reason,
			const FString& Field = FString(), const FString& InternalContext = FString())
		{
			auto Error = Contract::FDiagnostic::Failure(Code, Reason, TEXT("import_initialization"), InternalContext);
			Error.PropertyPath = Field;
			Error.SourceKind = TEXT("native_result");
			return Error;
		}

		Contract::FDiagnostic LocateInput(Contract::FDiagnostic Error, const FSourceProvenance& Source,
			const TCHAR* SourceKind = TEXT("authored_value"))
		{
			Error.SourceKind = SourceKind;
			Error.Source.JsonPath = Source.GetJsonPointer();
			Error.Source.Line = Source.GetLine();
			Error.Source.Column = Source.GetColumn();
			Error.SemanticPointer = Source.GetJsonPointer();
			FWorkspaceError Located;
			Located.SetFailure(MoveTemp(Error));
			Located.Locate(Source);
			return Located.GetDiagnostic();
		}

	}

	FImportInitializationDomainRequest::
		FImportInitializationDomainRequest(
			FString InCommandId,
			FString InSchemaId,
			FString InTargetIdentity,
			FString InRootTypeIdentity,
			const EInitializationNodeRole InRole,
			FString InCanonicalParamsJson,
		FString InRequestId,
		TOptional<FSourceProvenance> InSchemaSource,
		TOptional<FSourceProvenance> InRootTypeSource)
		: CommandId(MoveTemp(InCommandId))
		, SchemaId(MoveTemp(InSchemaId))
		, TargetIdentity(MoveTemp(InTargetIdentity))
		, RootTypeIdentity(MoveTemp(InRootTypeIdentity))
		, Role(InRole)
		, CanonicalParamsJson(MoveTemp(InCanonicalParamsJson))
		, RequestId(MoveTemp(InRequestId))
		, SchemaSource(MoveTemp(InSchemaSource))
		, RootTypeSource(MoveTemp(InRootTypeSource))
	{
		const auto ValidateIdentity = [&](const TCHAR* Field, const FString& Value, bool bPublic,
			const TOptional<FSourceProvenance>& Source = {})
		{
			if (ValidationDiagnostic || IsExactIdentity(Value)) return;
			auto Error = RegistryFailure(TEXT("hjson_domain_request_invalid"),
				FString::Printf(TEXT("Initialization request field '%s' must be non-empty and have no surrounding whitespace."), Field), Field);
			if (bPublic) Error.ActualValue = MakeShared<FJsonValueString>(Value);
			else Error.ActualEvidence = MakeShared<FJsonValueString>(Value);
			if (Source.IsSet()) Error = LocateInput(MoveTemp(Error), Source.GetValue());
			ValidationDiagnostic = MakeShared<Contract::FDiagnostic>(MoveTemp(Error));
		};
		ValidateIdentity(TEXT("command_id"), CommandId, true);
		ValidateIdentity(TEXT("schema"), SchemaId, true, SchemaSource);
		ValidateIdentity(TEXT("target"), TargetIdentity, false);
		ValidateIdentity(TEXT("root_type"), RootTypeIdentity, false, RootTypeSource);
		if (!ValidationDiagnostic && CanonicalParamsJson.IsEmpty())
			ValidationDiagnostic = MakeShared<Contract::FDiagnostic>(RegistryFailure(
				TEXT("hjson_domain_request_invalid"), TEXT("Initialization request has no command parameters."), TEXT("params")));
	}

	const FString& FImportInitializationDomainRequest::GetCommandId() const
	{
		return CommandId;
	}

	const FString& FImportInitializationDomainRequest::GetSchemaId() const
	{
		return SchemaId;
	}

	const FString& FImportInitializationDomainRequest::GetTargetIdentity() const
	{
		return TargetIdentity;
	}

	const FString& FImportInitializationDomainRequest::GetRootTypeIdentity() const
	{
		return RootTypeIdentity;
	}

	EInitializationNodeRole FImportInitializationDomainRequest::GetRole() const
	{
		return Role;
	}

	const FString& FImportInitializationDomainRequest::GetCanonicalParamsJson()
		const
	{
		return CanonicalParamsJson;
	}

	bool FImportInitializationDomainRequest::IsValid() const
	{
		return !ValidationDiagnostic.IsValid();
	}

	const Contract::FDiagnostic& FImportInitializationDomainRequest::GetValidationDiagnostic() const
	{
		static const Contract::FDiagnostic NoError;
		return ValidationDiagnostic ? *ValidationDiagnostic : NoError;
	}

	const FString& FImportInitializationDomainRequest::GetRequestId() const
	{
		return RequestId;
	}

	const TOptional<FSourceProvenance>& FImportInitializationDomainRequest::GetSchemaSource() const
	{
		return SchemaSource;
	}

	const TOptional<FSourceProvenance>& FImportInitializationDomainRequest::GetRootTypeSource() const
	{
		return RootTypeSource;
	}

	bool MatchesDomainImportSchema(
		const FImportInitializationDomainRequest& Request,
		const FString& DomainSchemaId)
	{
		return Request.GetCommandId() != TEXT("content_graph.import_workspace")
			|| Request.GetSchemaId() == DomainSchemaId
			|| Request.GetSchemaId() == GetSemanticWorkspaceSchemaId();
	}

	FImportInitializationDomainResponse::
		FImportInitializationDomainResponse(
			FString InPayloadJson,
			const bool bInReadOnly)
		: PayloadJson(MoveTemp(InPayloadJson))
		, bReadOnly(bInReadOnly)
	{
		bValid = !PayloadJson.IsEmpty();
	}

	const FString& FImportInitializationDomainResponse::GetPayloadJson() const
	{
		return PayloadJson;
	}

	bool FImportInitializationDomainResponse::IsReadOnly() const
	{
		return bReadOnly;
	}

	bool FImportInitializationDomainResponse::IsValid() const
	{
		return bValid;
	}

	const Contract::FDiagnostic& FImportInitializationDomainResponse::GetValidationDiagnostic() const
	{
		static const Contract::FDiagnostic NoError;
		static const Contract::FDiagnostic MissingPayload = RegistryFailure(
			TEXT("hjson_domain_response_empty"), TEXT("The domain provider returned no response payload."), TEXT("payload"));
		return bValid ? NoError : MissingPayload;
	}

	FImportInitializationDomainResolution::
		FImportInitializationDomainResolution(
			TSharedPtr<const IImportInitializationDomainProvider> InProvider,
			FString InInitializationContractId)
		: Provider(MoveTemp(InProvider))
		, InitializationContractId(MoveTemp(InInitializationContractId))
	{
	}

	const TSharedPtr<const IImportInitializationDomainProvider>&
	FImportInitializationDomainResolution::GetProvider() const
	{
		return Provider;
	}

	const FString& FImportInitializationDomainResolution::
		GetInitializationContractId() const
	{
		return InitializationContractId;
	}

	bool FImportInitializationDomainResolution::IsValid() const
	{
		return Provider.IsValid()
			&& IsExactIdentity(InitializationContractId);
	}

	void FImportInitializationDomainResolution::Reset()
	{
		Provider.Reset();
		InitializationContractId.Reset();
	}

	FImportInitializationDomainProviderRegistry::
		FImportInitializationDomainProviderRegistry(
			TArray<TSharedRef<IImportInitializationDomainProvider>>
				InProviders)
		: Providers(MoveTemp(InProviders))
	{
	}

	bool FImportInitializationDomainProviderRegistry::TryFreeze(
		TArray<TSharedRef<IImportInitializationDomainProvider>> InProviders,
		TSharedPtr<const FImportInitializationDomainProviderRegistry>&
			OutRegistry,
		FWorkspaceError& OutError)
	{
		OutRegistry.Reset();
		OutError.Reset();
		if (InProviders.IsEmpty())
		{
			OutError.SetFailure(RegistryFailure(TEXT("hjson_domain_provider_empty"),
				TEXT("No import initialization domain provider is registered."), TEXT("providers")));
			return false;
		}
		TSet<FString> ProviderIds;
		for (const TSharedRef<IImportInitializationDomainProvider>& Provider :
			InProviders)
		{
			const FString& ProviderId = Provider->GetProviderId();
			if (!IsExactIdentity(ProviderId))
			{
				OutError.SetFailure(RegistryFailure(TEXT("hjson_domain_provider_identity_invalid"),
					TEXT("A registered initialization provider has an empty or whitespace-padded identifier."), TEXT("provider"), ProviderId));
				return false;
			}
			if (ProviderIds.Contains(ProviderId))
			{
				OutError.SetFailure(RegistryFailure(TEXT("hjson_domain_provider_duplicate"),
					TEXT("The same initialization provider identifier is registered more than once."), TEXT("provider"), ProviderId));
				return false;
			}
			ProviderIds.Add(ProviderId);
		}
		InProviders.Sort(
			[](const TSharedRef<IImportInitializationDomainProvider>& Left,
			   const TSharedRef<IImportInitializationDomainProvider>& Right)
			{
				return Left->GetProviderId() < Right->GetProviderId();
			});
		OutRegistry = TSharedPtr<FImportInitializationDomainProviderRegistry>(
			new FImportInitializationDomainProviderRegistry(
				MoveTemp(InProviders)));
		return true;
	}

	bool FImportInitializationDomainProviderRegistry::TryResolveWorkspaceRootType(
		const FAuthoringModel& Model, const FString& TargetIdentity,
		FString& OutRootType, FWorkspaceError& OutError) const
	{
		OutRootType.Reset();
		OutError.Reset();
		int32 Matches = 0;
		for (const auto& Provider : Providers)
		{
			Matches += Provider->SupportsWorkspaceModelContract(Model.GetContractId()) ? 1 : 0;
		}
		if (Matches == 0) return true;
		if (Matches != 1 || Model.GetTargetUnitIds().Num() != 1)
		{
			auto Error = RegistryFailure(TEXT("hjson_workspace_domain_ambiguous"),
				Matches != 1 ? TEXT("The workspace contract matches more than one initialization provider.")
				: TEXT("A domain workspace must contain exactly one target."),
				Matches != 1 ? TEXT("providers") : TEXT("targets"), Model.GetContractId());
			Error.ActualValue = MakeShared<FJsonValueNumber>(Matches != 1 ? Matches : Model.GetTargetUnitIds().Num());
			Error.ExpectedValue = MakeShared<FJsonValueNumber>(1);
			OutError.SetFailure(MoveTemp(Error));
			return false;
		}
		const FSemanticUnit* Target = Model.FindUnit(Model.GetTargetUnitIds()[0]);
		if (!Target || Target->GetUnitKind() != ESemanticUnitKind::TargetState
			|| Target->GetTargetIdentityKey() != TargetIdentity)
		{
			auto Error = RegistryFailure(TEXT("hjson_workspace_target_mismatch"),
				!Target ? TEXT("The workspace target entry does not resolve to a semantic unit.")
				: Target->GetUnitKind() != ESemanticUnitKind::TargetState ? TEXT("The workspace target must be a target-state unit.")
				: TEXT("The workspace was captured for a different target."), TEXT("target"), TargetIdentity);
			Error.ExpectedEvidence = MakeShared<FJsonValueString>(TargetIdentity);
			if (Target) Error.ActualEvidence = MakeShared<FJsonValueString>(Target->GetTargetIdentityKey());
			OutError.SetFailure(MoveTemp(Error));
			return false;
		}
		OutRootType = Target->GetSemanticTypeRef();
		if (OutRootType.IsEmpty())
		{
			OutError.SetFailure(RegistryFailure(TEXT("hjson_workspace_root_type_missing"),
				TEXT("The captured workspace target has no semantic root type."), TEXT("root_type"), TargetIdentity));
			return false;
		}
		return true;
	}

	bool FImportInitializationDomainProviderRegistry::TryResolve(
		const FImportInitializationDomainRequest& Request,
		FImportInitializationDomainResolution& OutResolution,
		FWorkspaceError& OutError) const
	{
		OutResolution.Reset();
		OutError.Reset();
		if (!Request.IsValid())
		{
			OutError.SetFailure(Request.GetValidationDiagnostic());
			return false;
		}
		TArray<FImportInitializationDomainResolution> Matches;
		for (const bool bFallback : {false, true})
		{
		for (const auto& Provider : Providers)
		{
			if (Provider->IsFallbackProvider() != bFallback || !Provider->Matches(Request))
			{
				continue;
			}
			FString InitializationContractId;
			FWorkspaceError ProviderError;
			if (!Provider->TryResolveInitializationContract(
					Request,
					InitializationContractId,
					ProviderError))
			{
				if (!ProviderError.GetCode().IsEmpty())
				{
					OutError = ProviderError;
					return false;
				}
				continue;
			}
			if (!IsExactIdentity(InitializationContractId))
			{
				auto Error = RegistryFailure(TEXT("hjson_domain_contract_identity_invalid"),
					TEXT("The initialization provider returned an empty or whitespace-padded contract identifier."),
					TEXT("contract"), Provider->GetProviderId());
				Error.ActualEvidence = MakeShared<FJsonValueString>(InitializationContractId);
				OutError.SetFailure(MoveTemp(Error));
				return false;
			}
			Matches.Emplace(
				TSharedPtr<const IImportInitializationDomainProvider>(Provider),
				MoveTemp(InitializationContractId));
		}
		if (!Matches.IsEmpty()) break;
		}
		if (Matches.IsEmpty())
		{
			auto Error = RegistryFailure(TEXT("hjson_domain_provider_unresolved"),
				FString::Printf(TEXT("No initialization provider accepts command '%s' with schema '%s' for this target type."),
					*Request.GetCommandId(), *Request.GetSchemaId()), FString(), Request.GetRootTypeIdentity());
			if (Request.GetSchemaSource().IsSet())
			{
				Error = LocateInput(MoveTemp(Error), Request.GetSchemaSource().GetValue(), TEXT("related_input"));
				if (Request.GetRootTypeSource().IsSet())
				{
					const auto& Source = Request.GetRootTypeSource().GetValue();
					const auto Related = MakeShared<FJsonObject>();
					Related->SetStringField(TEXT("json_pointer"), Source.GetJsonPointer());
					Related->SetNumberField(TEXT("line"), Source.GetLine());
					Related->SetNumberField(TEXT("column"), Source.GetColumn());
					if (!Error.Source.SourceFile.IsEmpty() && Source.GetSourceUnit() == Error.Source.SourceFile)
						Related->SetStringField(TEXT("source_file"), Error.Source.SourceFile);
					Error.RelatedSources.Add(MakeShared<FJsonValueObject>(Related));
				}
			}
			OutError.SetFailure(MoveTemp(Error));
			return false;
		}
		if (Matches.Num() != 1)
		{
			auto Error = RegistryFailure(TEXT("hjson_domain_provider_ambiguous"),
				FString::Printf(TEXT("Command '%s' matches multiple initialization providers for this target type."), *Request.GetCommandId()),
				TEXT("providers"), Request.GetRootTypeIdentity());
			Error.ActualValue = MakeShared<FJsonValueNumber>(Matches.Num());
			Error.ExpectedValue = MakeShared<FJsonValueNumber>(1);
			OutError.SetFailure(MoveTemp(Error));
			return false;
		}
		OutResolution = MoveTemp(Matches[0]);
		return true;
	}

	int32 FImportInitializationDomainProviderRegistry::Num() const
	{
		return Providers.Num();
	}

	bool TryFreezeImportInitializationDomainProviders(
		const TArray<TSharedRef<IImportInitializationDomainProvider>>&
			ExactProviders,
		TSharedPtr<const FImportInitializationDomainProviderRegistry>&
			OutRegistry,
		FWorkspaceError& OutError)
	{
		OutRegistry.Reset();
		OutError.Reset();
		TArray<TSharedRef<IImportInitializationDomainProvider>> Providers =
			ExactProviders;
		TSharedPtr<const FImportInitializationDomainProviderRegistry>
			BuiltRegistry;
		if (!FImportInitializationDomainProviderRegistry::TryFreeze(
			MoveTemp(Providers),
			BuiltRegistry,
			OutError))
		{
			return false;
		}
		OutRegistry = MoveTemp(BuiltRegistry);
		return true;
	}

	bool FImportInitializationDomainProviderRegistry::ValidateNativeOwnedCreationContext(
		const FString& OwnerType, const FString& PropertyPath, FWorkspaceError& OutError) const
	{
		TSharedPtr<const IImportInitializationDomainProvider> Provider;
		return ResolveNativeOwnedCreationProvider(OwnerType, PropertyPath, Provider, OutError);
	}

	bool FImportInitializationDomainProviderRegistry::ResolveNativeOwnedCreationProvider(
		const FString& OwnerType, const FString& PropertyPath,
		TSharedPtr<const IImportInitializationDomainProvider>& OutProvider, FWorkspaceError& OutError) const
	{
		OutProvider.Reset();
		OutError.Reset();
		int32 Matches = 0;
		for (const auto& Provider : Providers)
			if (Provider->SupportsNativeOwnedCreationContext(OwnerType, PropertyPath)) { ++Matches; OutProvider = Provider; }
		if (Matches == 1) return true;
		OutProvider.Reset();
		auto Error = RegistryFailure(Matches == 0 ? TEXT("owned_creation_context_unsupported") : TEXT("owned_creation_context_ambiguous"),
			Matches == 0 ? TEXT("No domain provider supports native object creation for this owner type and property.")
			: TEXT("Multiple domain providers claim native object creation for this owner type and property."), PropertyPath, OwnerType);
		Error.CandidateName = OwnerType;
		Error.ActualValue = MakeShared<FJsonValueNumber>(Matches);
		Error.ExpectedValue = MakeShared<FJsonValueNumber>(1);
		OutError.SetFailure(MoveTemp(Error));
		return false;
	}

	bool TryFreezeProcessImportInitializationDomainProviders(
		TSharedPtr<const FImportInitializationDomainProviderRegistry>&
			OutRegistry,
		FWorkspaceError& OutError)
	{
		OutRegistry.Reset();
		OutError.Reset();
		UE::UAI::Contract::FDiagnostic CapabilityError;
		DomainCapability::FDomainCapabilityAuthorities Authorities;
		if (!DomainCapability::TryGetOrBuildProcessDomainCapabilityAuthorities(
				Authorities,
				CapabilityError))
		{
			OutError.SetFailure(CapabilityError);
			return false;
		}
		if (!Authorities.GetHjsonRegistry().IsValid())
		{
			OutError.SetFailure(RegistryFailure(TEXT("hjson_domain_provider_empty"),
				TEXT("The process has no registered import initialization domain providers."), TEXT("providers")));
			return false;
		}
		OutRegistry = Authorities.GetHjsonRegistry();
		return true;
	}
}

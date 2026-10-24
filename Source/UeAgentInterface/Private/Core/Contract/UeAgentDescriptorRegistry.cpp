#include "Core/Contract/UeAgentDescriptorRegistry.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Core/Contract/UeAgentContractSchema.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::Contract
{
	namespace
	{
		FString RequiredText(FString Value)
		{
			Value.TrimStartAndEndInline();
			return Value;
		}

		const TCHAR* OperationKindText(
			const EUeAgentDescriptorOperationKind Kind)
		{
			switch (Kind)
			{
			case EUeAgentDescriptorOperationKind::Query:
				return TEXT("query");
			case EUeAgentDescriptorOperationKind::Operation:
				return TEXT("operation");
			default:
				return TEXT("");
			}
		}

		const TCHAR* QueryKindText(const EUeAgentQueryKind Kind)
		{
			switch (Kind)
			{
			case EUeAgentQueryKind::Capability:
				return TEXT("capability");
			case EUeAgentQueryKind::Catalog:
				return TEXT("catalog");
			case EUeAgentQueryKind::State:
				return TEXT("state");
			case EUeAgentQueryKind::Reference:
				return TEXT("reference");
			case EUeAgentQueryKind::RuntimeProbe:
				return TEXT("runtime_probe");
			default:
				return TEXT("");
			}
		}

		const TCHAR* MaterializationPolicyText(
			const EUeAgentQueryMaterializationPolicy Policy)
		{
			switch (Policy)
			{
			case EUeAgentQueryMaterializationPolicy::IndexOnly:
				return TEXT("index_only");
			case EUeAgentQueryMaterializationPolicy::LoadReadonly:
				return TEXT("load_readonly");
			case EUeAgentQueryMaterializationPolicy::RuntimeObserve:
				return TEXT("runtime_observe");
			default:
				return TEXT("");
			}
		}

		const TCHAR* SideEffectPolicyText(
			const EUeAgentQuerySideEffectPolicy Policy)
		{
			return Policy
					== EUeAgentQuerySideEffectPolicy::
						NoPersistentMutation
				? TEXT("no_persistent_mutation")
				: TEXT("");
		}

		bool IsCanonicalSha256(const FString& Value)
		{
			if (!Value.StartsWith(TEXT("sha256:"))
				|| Value.Len() != 71)
			{
				return false;
			}
			for (int32 Index = 7; Index < Value.Len(); ++Index)
			{
				const TCHAR Character = Value[Index];
				if (!FChar::IsDigit(Character)
					&& (Character < TEXT('a')
						|| Character > TEXT('f')))
				{
					return false;
				}
			}
			return true;
		}

		TSharedPtr<FJsonValue> IntegerValue(const int32 Value)
		{
			return MakeShared<FJsonValueNumberString>(
				FString::FromInt(Value));
		}

		TSharedPtr<FJsonObject> ExecutionContextPolicyObject(
			const FUeAgentExecutionContextPolicy& Policy)
		{
			TArray<TSharedPtr<FJsonValue>> StatePlanes;
			StatePlanes.Reserve(
				Policy.GetAllowedStatePlanes().Num());
			for (const EUeAgentStatePlane StatePlane :
				Policy.GetAllowedStatePlanes())
			{
				StatePlanes.Add(
					MakeShared<FJsonValueString>(
						LexToString(StatePlane)));
			}

			TArray<TSharedPtr<FJsonValue>> RequiredFacets;
			RequiredFacets.Reserve(
				Policy.GetRequiredFacets().Num());
			for (const EUeAgentExecutionContextFacet Facet :
				Policy.GetRequiredFacets())
			{
				RequiredFacets.Add(
					MakeShared<FJsonValueString>(
						LexToString(Facet)));
			}

			const TSharedPtr<FJsonObject> Object =
				MakeShared<FJsonObject>();
			Object->SetArrayField(
				TEXT("allowed_state_planes"),
				MoveTemp(StatePlanes));
			Object->SetArrayField(
				TEXT("required_facets"),
				MoveTemp(RequiredFacets));
			Object->SetStringField(
				TEXT("resolution_policy"),
				LexToString(Policy.GetResolutionPolicy()));
			Object->SetStringField(
				TEXT("transient_access_policy"),
				LexToString(
					Policy.GetTransientContextPolicy()));
			return Object;
		}

		bool TrySchemaObject(
			const FUeAgentContractValue& Schema,
			const FString& DescriptorId,
			const TCHAR* Stage,
			const TCHAR* ErrorCode,
			TSharedPtr<FJsonObject>& OutObject,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			UE::UAI::Contract::FDiagnostic JsonError;
			if (!Schema.IsValid())
			{
				OutError = FDiagnostic::Failure(ErrorCode,
					TEXT("The descriptor schema is empty; a canonical JSON object is required."), Stage);
				OutError.CandidateName = DescriptorId;
				OutError.PropertyPath = FString(Stage) + TEXT("_schema");
				OutError.ActualValuePresent = false;
				OutError.SourceKind = TEXT("native_result");
				return false;
			}
			if (!Json::TryParseObject(
					Schema.GetJsonValue(),
					OutObject,
					JsonError))
			{
				UE_LOG(LogTemp, Error,
					TEXT("UAI descriptor schema rejected: descriptor=%s stage=%s reason=json_object_invalid detail=%s"),
					*DescriptorId, Stage, *JsonError.Code);
				OutError = FDiagnostic::WrapFailure(ErrorCode, JsonError, Stage);
				OutError.CandidateName = DescriptorId;
				OutError.PropertyPath = FString(Stage) + TEXT("_schema");
				OutError.SourceKind = TEXT("native_result");
				return false;
			}
			FString Canonical;
			if (!Json::TryCanonicalize(
				OutObject,
				Canonical,
				JsonError))
			{
				UE_LOG(LogTemp, Error,
					TEXT("UAI descriptor schema rejected: descriptor=%s stage=%s reason=canonicalization_failed detail=%s"),
					*DescriptorId, Stage, *JsonError.Code);
				OutError = FDiagnostic::WrapFailure(ErrorCode, JsonError, Stage);
				OutError.CandidateName = DescriptorId;
				if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = FString(Stage) + TEXT("_schema");
				OutError.SourceKind = TEXT("native_result");
				return false;
			}
			if (Canonical != Schema.GetJsonValue())
			{
				UE_LOG(LogTemp, Error,
					TEXT("UAI descriptor schema rejected: descriptor=%s stage=%s reason=schema_not_canonical"),
					*DescriptorId, Stage);
				OutError = UE::UAI::Contract::FDiagnostic::Failure(ErrorCode,
					TEXT("The descriptor schema JSON is valid but is not in canonical form."), Stage);
				OutError.CandidateName = DescriptorId;
				OutError.PropertyPath = FString(Stage) + TEXT("_schema");
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("canonical JSON with sorted keys and normalized values"));
				OutError.SourceKind = TEXT("native_result");
				return false;
			}
			return true;
		}

		bool TryDescriptorObject(
			const FUeAgentOperationDescriptor& Descriptor,
			const bool bRequireDescriptorHash,
			TSharedPtr<FJsonObject>& OutObject,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutObject.Reset();
			const FUeAgentDescriptorIdentity& Identity =
				Descriptor.GetIdentity();
			if (Identity.GetDescriptorId().IsEmpty()
				|| Identity.GetVersion() <= 0
				|| (bRequireDescriptorHash && !Identity.IsValid()))
			{
				OutError = FDiagnostic::WrapFailure(TEXT("descriptor_invalid"), Identity.GetValidationDiagnostic(), TEXT("registry"));
				OutError.CandidateName = Identity.GetDescriptorId();
				return false;
			}
			if (bRequireDescriptorHash && !IsCanonicalSha256(Identity.GetDescriptorHash()))
			{
				OutError = FDiagnostic::Failure(TEXT("descriptor_invalid"),
					TEXT("The descriptor hash payload must contain exactly 64 lowercase hexadecimal characters."), TEXT("registry"));
				OutError.CandidateName = Identity.GetDescriptorId();
				OutError.PropertyPath = TEXT("descriptor_hash");
				OutError.ActualEvidence = MakeShared<FJsonValueString>(Identity.GetDescriptorHash());
				OutError.SourceKind = TEXT("native_result");
				return false;
			}
			if (Descriptor.GetCapabilityId().IsEmpty() || Descriptor.GetDomainId().IsEmpty())
			{
				const bool bMissingCapability = Descriptor.GetCapabilityId().IsEmpty();
				OutError = FDiagnostic::Failure(TEXT("descriptor_invalid"),
					bMissingCapability ? TEXT("The descriptor capability identifier is empty.") : TEXT("The descriptor domain identifier is empty."), TEXT("registry"));
				OutError.CandidateName = Identity.GetDescriptorId();
				OutError.PropertyPath = bMissingCapability ? TEXT("capability_id") : TEXT("domain_id");
				OutError.ActualValue = MakeShared<FJsonValueString>(FString());
				OutError.SourceKind = TEXT("native_result");
				return false;
			}
			if (FString(OperationKindText(Descriptor.GetOperationKind())).IsEmpty())
			{
				OutError = FDiagnostic::Failure(TEXT("descriptor_invalid"),
					TEXT("The descriptor operation kind must be query or operation."), TEXT("registry"));
				OutError.CandidateName = Identity.GetDescriptorId();
				OutError.PropertyPath = TEXT("operation_kind");
				OutError.ActualValue = MakeShared<FJsonValueNumber>(static_cast<int32>(Descriptor.GetOperationKind()));
				OutError.SourceKind = TEXT("native_result");
				return false;
			}
			if (!Descriptor.GetExecutionContextPolicy().IsValid())
			{
				OutError = FDiagnostic::WrapFailure(TEXT("descriptor_execution_context_policy_invalid"),
					Descriptor.GetExecutionContextPolicy().GetValidationDiagnostic(), TEXT("registry"));
				OutError.CandidateName = Identity.GetDescriptorId();
				return false;
			}

			const bool bIsQuery =
				Descriptor.GetOperationKind()
				== EUeAgentDescriptorOperationKind::Query;
			if (bIsQuery
				&& !Descriptor.GetQueryBinding().IsSet())
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("descriptor_query_binding_required"), TEXT("A query descriptor requires a query binding."), TEXT("registry"));
				OutError.CandidateName = Identity.GetDescriptorId();
				OutError.PropertyPath = TEXT("query_binding");
				OutError.ActualValuePresent = false;
				return false;
			}
			if (!bIsQuery
				&& Descriptor.GetQueryBinding().IsSet())
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("descriptor_query_binding_forbidden"), TEXT("An operation descriptor cannot declare a query binding."), TEXT("registry"));
				OutError.CandidateName = Identity.GetDescriptorId();
				OutError.PropertyPath = TEXT("query_binding");
				OutError.ActualValuePresent = true;
				OutError.ExpectedValuePresent = false;
				return false;
			}
			if (Descriptor.GetQueryBinding().IsSet()
				&& !Descriptor.GetQueryBinding()->IsValid())
			{
				OutError = FDiagnostic::WrapFailure(TEXT("descriptor_query_binding_invalid"),
					Descriptor.GetQueryBinding()->GetValidationDiagnostic(), TEXT("registry"));
				OutError.CandidateName = Identity.GetDescriptorId();
				return false;
			}

			TSharedPtr<FJsonObject> InputSchema;
			if (!TrySchemaObject(
				Descriptor.GetInputSchema(),
				Descriptor.GetIdentity().GetDescriptorId(), TEXT("input"),
				TEXT("descriptor_input_schema_invalid"),
				InputSchema,
				OutError))
			{
				return false;
			}
			TSharedPtr<FJsonObject> ResultSchema;
			if (!TrySchemaObject(
				Descriptor.GetResultSchema(),
				Descriptor.GetIdentity().GetDescriptorId(), TEXT("result"),
				TEXT("descriptor_result_schema_invalid"),
				ResultSchema,
				OutError))
			{
				return false;
			}

			OutObject = MakeShared<FJsonObject>();
			OutObject->SetStringField(
				TEXT("capability_id"),
				Descriptor.GetCapabilityId());
			OutObject->SetStringField(
				TEXT("descriptor_id"),
				Identity.GetDescriptorId());
			OutObject->SetStringField(
				TEXT("domain_id"),
				Descriptor.GetDomainId());
			OutObject->SetObjectField(
				TEXT("execution_context_policy"),
				ExecutionContextPolicyObject(
					Descriptor.GetExecutionContextPolicy()));
			OutObject->SetObjectField(
				TEXT("input_schema"),
				InputSchema);
			OutObject->SetStringField(
				TEXT("operation_kind"),
				OperationKindText(
					Descriptor.GetOperationKind()));
			if (bIsQuery)
			{
				const FUeAgentQueryBinding& Query =
					Descriptor.GetQueryBinding().GetValue();
				const TSharedPtr<FJsonObject> QueryObject =
					MakeShared<FJsonObject>();
				QueryObject->SetStringField(
					TEXT("materialization_policy"),
					MaterializationPolicyText(
						Query.GetMaterializationPolicy()));
				QueryObject->SetStringField(
					TEXT("provider_binding"),
					Query.GetProviderBinding());
				QueryObject->SetStringField(
					TEXT("query_kind"),
					QueryKindText(Query.GetQueryKind()));
				QueryObject->SetStringField(
					TEXT("side_effect_policy"),
					SideEffectPolicyText(
						Query.GetSideEffectPolicy()));
				OutObject->SetObjectField(
					TEXT("query"),
					QueryObject);
			}
			OutObject->SetObjectField(
				TEXT("result_schema"),
				ResultSchema);
			OutObject->SetField(
				TEXT("version"),
				IntegerValue(Identity.GetVersion()));
			return true;
		}

		bool TryValidateDescriptorHash(
			const FUeAgentOperationDescriptor& Descriptor,
			TSharedPtr<FJsonObject>& OutDescriptorObject,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (!TryDescriptorObject(
				Descriptor,
				true,
				OutDescriptorObject,
				OutError))
			{
				return false;
			}
			FString Canonical;
			UE::UAI::Contract::FDiagnostic JsonError;
			if (!Json::TryCanonicalize(
				OutDescriptorObject,
				Canonical,
				JsonError))
			{
				OutError = JsonError;
				return false;
			}
			if (Json::Sha256Utf8(Canonical)
				!= Descriptor.GetIdentity().GetDescriptorHash())
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("descriptor_hash_mismatch"), TEXT("The canonical descriptor content does not match its declared identity."), TEXT("registry"));
				OutError.CandidateName = Descriptor.GetIdentity().GetDescriptorId();
				OutError.PropertyPath = TEXT("descriptor_hash");
				return false;
			}
			return true;
		}
	}

	bool TryComputeOperationDescriptorHash(
		const FUeAgentOperationDescriptor& DescriptorDraft,
		FString& OutDescriptorHash,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutDescriptorHash.Reset();
		TSharedPtr<FJsonObject> DescriptorObject;
		if (!TryDescriptorObject(
				DescriptorDraft,
				false,
				DescriptorObject,
				OutError))
		{
			return false;
		}
		FString Canonical;
		if (!Json::TryCanonicalize(
				DescriptorObject,
				Canonical,
				OutError))
		{
			return false;
		}
		OutDescriptorHash = Json::Sha256Utf8(Canonical);
		return true;
	}

	FUeAgentQueryBinding::FUeAgentQueryBinding(
		FString InProviderBinding,
		const EUeAgentQueryKind InQueryKind,
		const EUeAgentQueryMaterializationPolicy
			InMaterializationPolicy,
		const EUeAgentQuerySideEffectPolicy
			InSideEffectPolicy)
		: ProviderBinding(
			RequiredText(MoveTemp(InProviderBinding)))
		, QueryKind(InQueryKind)
		, MaterializationPolicy(InMaterializationPolicy)
		, SideEffectPolicy(InSideEffectPolicy)
	{
		if (ProviderBinding.IsEmpty())
		{
			ValidationDiagnostic = FDiagnostic::Failure(TEXT("query_binding_provider_missing"),
				TEXT("The query binding requires a non-empty provider binding."), TEXT("contract_validation"));
			ValidationDiagnostic.PropertyPath = TEXT("/query_binding/provider_binding");
			ValidationDiagnostic.ActualValue = MakeShared<FJsonValueString>(ProviderBinding);
		}
		else if (FString(QueryKindText(QueryKind)).IsEmpty())
		{
			ValidationDiagnostic = FDiagnostic::Failure(TEXT("query_binding_kind_invalid"),
				TEXT("The query kind must be capability, catalog, state, reference or runtime_probe."), TEXT("contract_validation"));
			ValidationDiagnostic.PropertyPath = TEXT("/query_binding/query_kind");
			ValidationDiagnostic.ActualValue = MakeShared<FJsonValueNumber>(static_cast<int32>(QueryKind));
		}
		else if (FString(MaterializationPolicyText(MaterializationPolicy)).IsEmpty())
		{
			ValidationDiagnostic = FDiagnostic::Failure(TEXT("query_binding_materialization_invalid"),
				TEXT("The query materialization policy must be index_only, load_readonly or runtime_observe."), TEXT("contract_validation"));
			ValidationDiagnostic.PropertyPath = TEXT("/query_binding/materialization_policy");
			ValidationDiagnostic.ActualValue = MakeShared<FJsonValueNumber>(static_cast<int32>(MaterializationPolicy));
		}
		else if (FString(SideEffectPolicyText(SideEffectPolicy)).IsEmpty())
		{
			ValidationDiagnostic = FDiagnostic::Failure(TEXT("query_binding_side_effect_invalid"),
				TEXT("The query side effect policy must be no_persistent_mutation."), TEXT("contract_validation"));
			ValidationDiagnostic.PropertyPath = TEXT("/query_binding/side_effect_policy");
			ValidationDiagnostic.ActualValue = MakeShared<FJsonValueNumber>(static_cast<int32>(SideEffectPolicy));
		}
		if (!ValidationDiagnostic.IsEmpty()) ValidationDiagnostic.SourceKind = TEXT("native_result");
	}

	const FString&
	FUeAgentQueryBinding::GetProviderBinding() const
	{
		return ProviderBinding;
	}

	EUeAgentQueryKind FUeAgentQueryBinding::GetQueryKind() const
	{
		return QueryKind;
	}

	EUeAgentQueryMaterializationPolicy
	FUeAgentQueryBinding::GetMaterializationPolicy() const
	{
		return MaterializationPolicy;
	}

	EUeAgentQuerySideEffectPolicy
	FUeAgentQueryBinding::GetSideEffectPolicy() const
	{
		return SideEffectPolicy;
	}

	bool FUeAgentQueryBinding::IsValid() const
	{
		return ValidationDiagnostic.IsEmpty();
	}

	const FDiagnostic& FUeAgentQueryBinding::GetValidationDiagnostic() const
	{
		return ValidationDiagnostic;
	}

	FUeAgentOperationDescriptor::FUeAgentOperationDescriptor(
		FUeAgentDescriptorIdentity InIdentity,
		FString InCapabilityId,
		FString InDomainId,
		const EUeAgentDescriptorOperationKind InOperationKind,
		FUeAgentExecutionContextPolicy
			InExecutionContextPolicy,
		FUeAgentContractValue InInputSchema,
		FUeAgentContractValue InResultSchema,
		TOptional<FUeAgentQueryBinding> InQueryBinding)
		: Identity(MoveTemp(InIdentity))
		, CapabilityId(
			RequiredText(MoveTemp(InCapabilityId)))
		, DomainId(RequiredText(MoveTemp(InDomainId)))
		, OperationKind(InOperationKind)
		, ExecutionContextPolicy(
			MoveTemp(InExecutionContextPolicy))
		, InputSchema(MoveTemp(InInputSchema))
		, ResultSchema(MoveTemp(InResultSchema))
		, QueryBinding(MoveTemp(InQueryBinding))
	{
	}

	const FUeAgentDescriptorIdentity&
	FUeAgentOperationDescriptor::GetIdentity() const
	{
		return Identity;
	}

	const FString&
	FUeAgentOperationDescriptor::GetCapabilityId() const
	{
		return CapabilityId;
	}

	const FString&
	FUeAgentOperationDescriptor::GetDomainId() const
	{
		return DomainId;
	}

	EUeAgentDescriptorOperationKind
	FUeAgentOperationDescriptor::GetOperationKind() const
	{
		return OperationKind;
	}

	const FUeAgentExecutionContextPolicy&
	FUeAgentOperationDescriptor::
	GetExecutionContextPolicy() const
	{
		return ExecutionContextPolicy;
	}

	const FUeAgentContractValue&
	FUeAgentOperationDescriptor::GetInputSchema() const
	{
		return InputSchema;
	}

	const FUeAgentContractValue&
	FUeAgentOperationDescriptor::GetResultSchema() const
	{
		return ResultSchema;
	}

	const TOptional<FUeAgentQueryBinding>&
	FUeAgentOperationDescriptor::GetQueryBinding() const
	{
		return QueryBinding;
	}

	FUeAgentQueryDescriptor::FUeAgentQueryDescriptor(
		TSharedRef<const FUeAgentDescriptorRegistry>
			InRegistryOwner,
		const FUeAgentOperationDescriptor& InDescriptor,
		TSharedRef<const FUeAgentContractSchema>
			InCompiledInputSchema)
		: RegistryOwner(MoveTemp(InRegistryOwner))
		, Descriptor(&InDescriptor)
		, CompiledInputSchema(MoveTemp(InCompiledInputSchema))
	{
		check(RegistryOwner.IsValid());
		check(CompiledInputSchema.IsValid());
		check(InDescriptor.GetOperationKind()
			== EUeAgentDescriptorOperationKind::Query);
		check(InDescriptor.GetQueryBinding().IsSet());
	}

	const FUeAgentOperationDescriptor&
	FUeAgentQueryDescriptor::GetOperationDescriptor() const
	{
		check(Descriptor);
		return *Descriptor;
	}

	const FUeAgentDescriptorIdentity&
	FUeAgentQueryDescriptor::GetIdentity() const
	{
		return GetOperationDescriptor().GetIdentity();
	}

	const FString&
	FUeAgentQueryDescriptor::GetCapabilityId() const
	{
		return GetOperationDescriptor().GetCapabilityId();
	}

	const FString& FUeAgentQueryDescriptor::GetDomainId() const
	{
		return GetOperationDescriptor().GetDomainId();
	}

	const FUeAgentExecutionContextPolicy&
	FUeAgentQueryDescriptor::GetExecutionContextPolicy() const
	{
		return GetOperationDescriptor()
			.GetExecutionContextPolicy();
	}

	const FUeAgentContractValue&
	FUeAgentQueryDescriptor::GetInputSchema() const
	{
		return GetOperationDescriptor().GetInputSchema();
	}

	const FUeAgentContractSchema&
	FUeAgentQueryDescriptor::GetCompiledInputSchema() const
	{
		check(CompiledInputSchema.IsValid());
		return *CompiledInputSchema;
	}

	const FUeAgentContractValue&
	FUeAgentQueryDescriptor::GetResultSchema() const
	{
		return GetOperationDescriptor().GetResultSchema();
	}

	const FString&
	FUeAgentQueryDescriptor::GetProviderBinding() const
	{
		return GetOperationDescriptor()
			.GetQueryBinding()
			.GetValue()
			.GetProviderBinding();
	}

	EUeAgentQueryKind
	FUeAgentQueryDescriptor::GetQueryKind() const
	{
		return GetOperationDescriptor()
			.GetQueryBinding()
			.GetValue()
			.GetQueryKind();
	}

	EUeAgentQueryMaterializationPolicy
	FUeAgentQueryDescriptor::GetMaterializationPolicy() const
	{
		return GetOperationDescriptor()
			.GetQueryBinding()
			.GetValue()
			.GetMaterializationPolicy();
	}

	EUeAgentQuerySideEffectPolicy
	FUeAgentQueryDescriptor::GetSideEffectPolicy() const
	{
		return GetOperationDescriptor()
			.GetQueryBinding()
			.GetValue()
			.GetSideEffectPolicy();
	}

	FUeAgentDescriptorRegistry::FUeAgentDescriptorRegistry(
		TArray<FUeAgentOperationDescriptor> InDescriptors,
		TArray<TSharedRef<const FUeAgentContractSchema>>
			InCompiledInputSchemas,
		TArray<TSharedRef<const FUeAgentContractSchema>>
			InCompiledResultSchemas,
		FString InRegistryHash)
		: Descriptors(MoveTemp(InDescriptors))
		, CompiledInputSchemas(MoveTemp(InCompiledInputSchemas))
		, CompiledResultSchemas(MoveTemp(InCompiledResultSchemas))
		, RegistryHash(MoveTemp(InRegistryHash))
	{
		check(Descriptors.Num() == CompiledInputSchemas.Num());
		check(Descriptors.Num() == CompiledResultSchemas.Num());
		for (int32 Index = 0; Index < Descriptors.Num(); ++Index)
		{
			IndexByDescriptorId.Add(
				Descriptors[Index].GetIdentity().GetDescriptorId(),
				Index);
		}
	}

	bool FUeAgentDescriptorRegistry::TryCreate(
		TArray<FUeAgentOperationDescriptor> Descriptors,
		TSharedPtr<const FUeAgentDescriptorRegistry>& OutRegistry,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutRegistry.Reset();
		OutError.Reset();
		if (Descriptors.IsEmpty())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("descriptor_registry_empty"), TEXT("The descriptor registry contains no descriptors."), TEXT("registry"));
			OutError.PropertyPath = TEXT("descriptors");
			OutError.ActualValue = MakeShared<FJsonValueNumber>(0);
			return false;
		}
		Descriptors.Sort(
			[](const FUeAgentOperationDescriptor& Left,
			   const FUeAgentOperationDescriptor& Right)
			{
				return Left.GetIdentity().GetDescriptorId()
					< Right.GetIdentity().GetDescriptorId();
			});

		TSet<FString> CapabilityIds;
		for (int32 Index = 0; Index < Descriptors.Num(); ++Index)
		{
			if (Index > 0
				&& Descriptors[Index - 1]
						.GetIdentity()
						.GetDescriptorId()
					== Descriptors[Index]
						.GetIdentity()
						.GetDescriptorId())
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("descriptor_registry_descriptor_id_duplicate"), TEXT("The descriptor ID is registered more than once."), TEXT("registry"));
				OutError.CandidateName = Descriptors[Index].GetIdentity().GetDescriptorId();
				OutError.PropertyPath = TEXT("descriptor_id");
				return false;
			}
			const FString& CapabilityId =
				Descriptors[Index].GetCapabilityId();
			if (CapabilityIds.Contains(CapabilityId))
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("descriptor_registry_capability_id_duplicate"), TEXT("The capability ID is registered by more than one descriptor."), TEXT("registry"));
				OutError.CandidateName = Descriptors[Index].GetIdentity().GetDescriptorId();
				OutError.PropertyPath = TEXT("capability_id");
				OutError.ActualValue = MakeShared<FJsonValueString>(CapabilityId);
				return false;
			}
			CapabilityIds.Add(CapabilityId);
		}

		TArray<TSharedPtr<FJsonValue>> DescriptorValues;
		DescriptorValues.Reserve(Descriptors.Num());
		TArray<TSharedRef<const FUeAgentContractSchema>>
			CompiledInputSchemas;
		TArray<TSharedRef<const FUeAgentContractSchema>>
			CompiledResultSchemas;
		CompiledInputSchemas.Reserve(Descriptors.Num());
		CompiledResultSchemas.Reserve(Descriptors.Num());
		for (const FUeAgentOperationDescriptor& Descriptor :
			Descriptors)
		{
			TSharedPtr<const FUeAgentContractSchema> CompiledInput;
			TSharedPtr<const FUeAgentContractSchema> CompiledResult;
			FUeAgentContractSchemaError SchemaError;
			if (!FUeAgentContractSchema::TryCompile(
					Descriptor.GetInputSchema(),
					CompiledInput,
					SchemaError))
			{
				UE_LOG(LogTemp, Error,
					TEXT("UAI descriptor schema rejected: descriptor=%s stage=input code=%s schema_path=%s keyword=%s detail=%s"),
					*Descriptor.GetIdentity().GetDescriptorId(), *SchemaError.Code,
					*SchemaError.SchemaPath, *SchemaError.Keyword, *SchemaError.InternalContext);
				OutError = FDiagnostic::WrapFailure(SchemaError.Code
						== TEXT("contract_schema_keyword_unsupported")
					? TEXT("descriptor_input_schema_keyword_unsupported")
					: TEXT("descriptor_input_schema_invalid"), SchemaError, TEXT("registry"));
				OutError.CandidateName = Descriptor.GetIdentity().GetDescriptorId();
				if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = TEXT("input_schema");
				OutError.SubjectId = Descriptor.GetIdentity().GetDescriptorId();
				return false;
			}
			if (!FUeAgentContractSchema::TryCompile(
					Descriptor.GetResultSchema(),
					CompiledResult,
					SchemaError))
			{
				UE_LOG(LogTemp, Error,
					TEXT("UAI descriptor schema rejected: descriptor=%s stage=result code=%s schema_path=%s keyword=%s detail=%s"),
					*Descriptor.GetIdentity().GetDescriptorId(), *SchemaError.Code,
					*SchemaError.SchemaPath, *SchemaError.Keyword, *SchemaError.InternalContext);
				OutError = FDiagnostic::WrapFailure(SchemaError.Code
						== TEXT("contract_schema_keyword_unsupported")
					? TEXT("descriptor_result_schema_keyword_unsupported")
					: TEXT("descriptor_result_schema_invalid"), SchemaError, TEXT("registry"));
				OutError.CandidateName = Descriptor.GetIdentity().GetDescriptorId();
				if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = TEXT("result_schema");
				OutError.SubjectId = Descriptor.GetIdentity().GetDescriptorId();
				return false;
			}
			TSharedPtr<FJsonObject> DescriptorObject;
			if (!TryValidateDescriptorHash(
				Descriptor,
				DescriptorObject,
				OutError))
			{
				return false;
			}
			DescriptorValues.Add(
				MakeShared<FJsonValueObject>(
					DescriptorObject));
			CompiledInputSchemas.Add(CompiledInput.ToSharedRef());
			CompiledResultSchemas.Add(CompiledResult.ToSharedRef());
		}

		const TSharedPtr<FJsonObject> RegistryObject =
			MakeShared<FJsonObject>();
		RegistryObject->SetArrayField(
			TEXT("descriptors"),
			DescriptorValues);
		FString CanonicalRegistry;
		UE::UAI::Contract::FDiagnostic JsonError;
		if (!Json::TryCanonicalize(
			RegistryObject,
			CanonicalRegistry,
			JsonError))
		{
			OutError = JsonError.IsValid() ? JsonError : UE::UAI::Contract::FDiagnostic::Failure(TEXT("descriptor_registry_hash_failed"), TEXT("The descriptor registry hash could not be computed."), TEXT("registry"));
			return false;
		}
		const FString RegistryHash =
			Json::Sha256Utf8(CanonicalRegistry);
		if (!IsCanonicalSha256(RegistryHash))
		{
			OutError = JsonError.IsValid() ? JsonError : UE::UAI::Contract::FDiagnostic::Failure(TEXT("descriptor_registry_hash_failed"), TEXT("The descriptor registry hash could not be computed."), TEXT("registry"));
			return false;
		}
		OutRegistry =
			MakeShareable(new FUeAgentDescriptorRegistry(
				MoveTemp(Descriptors),
				MoveTemp(CompiledInputSchemas),
				MoveTemp(CompiledResultSchemas),
				RegistryHash));
		return true;
	}

	const TArray<FUeAgentOperationDescriptor>&
	FUeAgentDescriptorRegistry::GetDescriptors() const
	{
		return Descriptors;
	}

	const FString&
	FUeAgentDescriptorRegistry::GetRegistryHash() const
	{
		return RegistryHash;
	}

	const FUeAgentOperationDescriptor*
	FUeAgentDescriptorRegistry::FindByDescriptorId(
		const FString& DescriptorId) const
	{
		const int32* Index =
			IndexByDescriptorId.Find(DescriptorId);
		return Index ? &Descriptors[*Index] : nullptr;
	}

	const FUeAgentOperationDescriptor*
	FUeAgentDescriptorRegistry::FindExact(
		const FUeAgentDescriptorIdentity& Identity) const
	{
		const FUeAgentOperationDescriptor* Descriptor =
			FindByDescriptorId(Identity.GetDescriptorId());
		return Descriptor
				&& Descriptor->GetIdentity() == Identity
			? Descriptor
			: nullptr;
	}

	const FUeAgentContractSchema*
	FUeAgentDescriptorRegistry::FindCompiledInputSchemaExact(
		const FUeAgentDescriptorIdentity& Identity) const
	{
		if (!FindExact(Identity))
		{
			return nullptr;
		}
		const int32* Index =
			IndexByDescriptorId.Find(Identity.GetDescriptorId());
		return Index && CompiledInputSchemas.IsValidIndex(*Index)
			? &CompiledInputSchemas[*Index].Get()
			: nullptr;
	}

	const FUeAgentContractSchema*
	FUeAgentDescriptorRegistry::FindCompiledResultSchemaExact(
		const FUeAgentDescriptorIdentity& Identity) const
	{
		if (!FindExact(Identity))
		{
			return nullptr;
		}
		const int32* Index =
			IndexByDescriptorId.Find(Identity.GetDescriptorId());
		return Index && CompiledResultSchemas.IsValidIndex(*Index)
			? &CompiledResultSchemas[*Index].Get()
			: nullptr;
	}

	TOptional<FUeAgentQueryDescriptor>
	FUeAgentDescriptorRegistry::FindQueryExact(
		const FUeAgentDescriptorIdentity& Identity) const
	{
		const FUeAgentOperationDescriptor* Descriptor =
			FindExact(Identity);
		if (!Descriptor
			|| Descriptor->GetOperationKind()
				!= EUeAgentDescriptorOperationKind::Query)
		{
			return {};
		}
		return FUeAgentQueryDescriptor(
			AsShared(),
			*Descriptor,
			CompiledInputSchemas[
				IndexByDescriptorId.FindChecked(
					Identity.GetDescriptorId())]);
	}

	EUeAgentDescriptorCompatibility
	FUeAgentDescriptorRegistry::CheckCompatibility(
		const FUeAgentDescriptorIdentity& Identity) const
	{
		const FUeAgentOperationDescriptor* Descriptor =
			FindByDescriptorId(Identity.GetDescriptorId());
		if (!Descriptor)
		{
			return EUeAgentDescriptorCompatibility::NotFound;
		}
		if (Descriptor->GetIdentity().GetVersion()
			!= Identity.GetVersion())
		{
			return EUeAgentDescriptorCompatibility::
				VersionIncompatible;
		}
		if (Descriptor->GetIdentity().GetDescriptorHash()
			!= Identity.GetDescriptorHash())
		{
			return EUeAgentDescriptorCompatibility::
				HashIncompatible;
		}
		return EUeAgentDescriptorCompatibility::Exact;
	}
}

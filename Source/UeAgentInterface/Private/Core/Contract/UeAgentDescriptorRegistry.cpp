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
			const TCHAR* ErrorCode,
			TSharedPtr<FJsonObject>& OutObject,
			FString& OutError)
		{
			FString JsonError;
			if (!Schema.IsValid()
				|| !Json::TryParseObject(
					Schema.GetJsonValue(),
					OutObject,
					JsonError))
			{
				OutError = ErrorCode;
				return false;
			}
			FString Canonical;
			if (!Json::TryCanonicalize(
				OutObject,
				Canonical,
				JsonError)
				|| Canonical != Schema.GetJsonValue())
			{
				OutError = ErrorCode;
				return false;
			}
			return true;
		}

		bool TryDescriptorObject(
			const FUeAgentOperationDescriptor& Descriptor,
			TSharedPtr<FJsonObject>& OutObject,
			FString& OutError)
		{
			OutObject.Reset();
			const FUeAgentDescriptorIdentity& Identity =
				Descriptor.GetIdentity();
			if (!Identity.IsValid()
				|| !IsCanonicalSha256(
					Identity.GetDescriptorHash())
				|| Descriptor.GetCapabilityId().IsEmpty()
				|| Descriptor.GetDomainId().IsEmpty()
				|| FString(OperationKindText(
					Descriptor.GetOperationKind())).IsEmpty())
			{
				OutError = TEXT("descriptor_invalid");
				return false;
			}
			if (!Descriptor.GetExecutionContextPolicy().IsValid())
			{
				OutError = TEXT(
					"descriptor_execution_context_policy_invalid");
				return false;
			}

			const bool bIsQuery =
				Descriptor.GetOperationKind()
				== EUeAgentDescriptorOperationKind::Query;
			if (bIsQuery
				&& !Descriptor.GetQueryBinding().IsSet())
			{
				OutError =
					TEXT("descriptor_query_binding_required");
				return false;
			}
			if (!bIsQuery
				&& Descriptor.GetQueryBinding().IsSet())
			{
				OutError =
					TEXT("descriptor_query_binding_forbidden");
				return false;
			}
			if (Descriptor.GetQueryBinding().IsSet()
				&& !Descriptor.GetQueryBinding()->IsValid())
			{
				OutError =
					TEXT("descriptor_query_binding_invalid");
				return false;
			}

			TSharedPtr<FJsonObject> InputSchema;
			if (!TrySchemaObject(
				Descriptor.GetInputSchema(),
				TEXT("descriptor_input_schema_invalid"),
				InputSchema,
				OutError))
			{
				return false;
			}
			TSharedPtr<FJsonObject> ResultSchema;
			if (!TrySchemaObject(
				Descriptor.GetResultSchema(),
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
			FString& OutError)
		{
			if (!TryDescriptorObject(
				Descriptor,
				OutDescriptorObject,
				OutError))
			{
				return false;
			}
			FString Canonical;
			FString JsonError;
			if (!Json::TryCanonicalize(
				OutDescriptorObject,
				Canonical,
				JsonError))
			{
				OutError =
					TEXT("descriptor_canonicalization_failed");
				return false;
			}
			if (Json::Sha256Utf8(Canonical)
				!= Descriptor.GetIdentity().GetDescriptorHash())
			{
				OutError = TEXT("descriptor_hash_mismatch");
				return false;
			}
			return true;
		}
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
		return !ProviderBinding.IsEmpty()
			&& FString(QueryKindText(QueryKind)).Len() > 0
			&& FString(MaterializationPolicyText(
				MaterializationPolicy)).Len() > 0
			&& FString(SideEffectPolicyText(
				SideEffectPolicy)).Len() > 0;
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
		FString& OutError)
	{
		OutRegistry.Reset();
		OutError.Reset();
		if (Descriptors.IsEmpty())
		{
			OutError = TEXT("descriptor_registry_empty");
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
				OutError =
					TEXT(
						"descriptor_registry_"
						"descriptor_id_duplicate");
				return false;
			}
			const FString& CapabilityId =
				Descriptors[Index].GetCapabilityId();
			if (CapabilityIds.Contains(CapabilityId))
			{
				OutError =
					TEXT(
						"descriptor_registry_"
						"capability_id_duplicate");
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
				OutError = SchemaError.Code
						== TEXT(
							"contract_schema_keyword_unsupported")
					? TEXT(
						"descriptor_input_schema_keyword_unsupported")
					: TEXT("descriptor_input_schema_invalid");
				return false;
			}
			if (!FUeAgentContractSchema::TryCompile(
					Descriptor.GetResultSchema(),
					CompiledResult,
					SchemaError))
			{
				OutError = SchemaError.Code
						== TEXT(
							"contract_schema_keyword_unsupported")
					? TEXT(
						"descriptor_result_schema_keyword_unsupported")
					: TEXT("descriptor_result_schema_invalid");
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
		FString JsonError;
		if (!Json::TryCanonicalize(
			RegistryObject,
			CanonicalRegistry,
			JsonError))
		{
			OutError =
				TEXT("descriptor_registry_hash_failed");
			return false;
		}
		const FString RegistryHash =
			Json::Sha256Utf8(CanonicalRegistry);
		if (!IsCanonicalSha256(RegistryHash))
		{
			OutError =
				TEXT("descriptor_registry_hash_failed");
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

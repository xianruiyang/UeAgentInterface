#pragma once

#include "CoreMinimal.h"

namespace UE::UAI::Contract { struct FDiagnostic; }

namespace UE::UAI::Projection
{
	enum class EUeAgentProjectionPageUnit : uint8;
}

namespace UE::UAI::BindingGateway::Query
{
	enum class EUeAgentQueryPublicProjectionStrategy : uint8
	{
		LegacyTypedEnvelope,
		SnapshotEntries,
		SnapshotSections,
		GroupedRecords,
		SingleRecord,
		SnapshotRecordArray,
	};

	enum class EUeAgentQueryPublicIdentityField : uint8
	{
		Selector,
		ObjectRole,
	};

	struct UEAGENTINTERFACE_API FQueryPublicFieldProjection final
	{
		FString Selector;
		FString PublicName;
		bool bRequired = false;
		EUeAgentQueryPublicIdentityField IdentityField =
			EUeAgentQueryPublicIdentityField::Selector;
		// For an object field, Children is its admitted object-field schema. For
		// an array field, Children is the admitted item-object schema (or empty
		// to admit scalar items only). Missing optional fields are omitted;
		// explicitly returned business containers retain their empty results.
		TArray<FQueryPublicFieldProjection> Children;
		bool bArray = false;
		// Caller-owned business keys with scalar values, e.g. DataTable fields.
		bool bScalarMap = false;
		// Group sibling canonical entries inside public record objects. Children
		// still describes one source value; this path applies to record fields only.
		TArray<FString> PublicContainerPath;
		// A value already encoded by the common authoring codec is business
		// data, not an observation envelope; preserve its literal/selected and
		// nested collection shapes exactly.
		bool bAuthoringValue = false;

		// A native observer's structured value may be serialized as one canonical
		// string. Decode it once without a second, partial business-field schema;
		// SDK Result owns the model projection of protocol custody.
		bool bStructuredValue = false;

		// Preserve an explicitly observed null without treating a missing field
		// as present. Opt-in leaves existing optional-field omission unchanged.
		bool bNullable = false;

		bool IsValid() const;
		bool TryValidate(Contract::FDiagnostic& OutDiagnostic, const FString& Path = FString()) const;
	};

	struct UEAGENTINTERFACE_API FQueryPublicResultField final
	{
		FString Name;
		FString WireType;
		bool bRequired = false;
		FString Description;
		bool bNullable = false;

		bool IsValid() const;
		bool TryValidate(Contract::FDiagnostic& OutDiagnostic, const FString& Path = FString()) const;
	};

	struct UEAGENTINTERFACE_API FQueryPublicDynamicFieldProjection final
	{
		FString SemanticIdPrefix;
		FString TargetKind;
		FString PublicContainerName;

		bool IsEmpty() const;
		bool IsValid() const;
		bool TryValidate(Contract::FDiagnostic& OutDiagnostic, const FString& Path = FString()) const;
	};

	struct UEAGENTINTERFACE_API FQueryPublicProjectionDescriptor final
	{
		FString Id;
		EUeAgentQueryPublicProjectionStrategy Strategy =
			EUeAgentQueryPublicProjectionStrategy::LegacyTypedEnvelope;
		FString CollectionField;
		TArray<FQueryPublicFieldProjection> FieldProjections;
		TArray<FQueryPublicResultField> ResultFields;
		FString ResultGuidance;
		bool bFlattenSingleFieldRecords = false;
		FQueryPublicDynamicFieldProjection DynamicFields;
		FString OwnerPublicName;
		TArray<FString> PagedArrayFields;
		FString SnapshotArraySelector;
		TArray<FString> CommandIds;
		EUeAgentQueryPublicIdentityField SnapshotArrayIdentityField =
			EUeAgentQueryPublicIdentityField::Selector;

		bool IsValid() const;
		bool TryValidate(Contract::FDiagnostic& OutDiagnostic, const FString& Path = FString()) const;
	};

	UEAGENTINTERFACE_API const FQueryPublicProjectionDescriptor*
	FindQueryPublicProjectionDescriptor(const FString& DescriptorId, Contract::FDiagnostic* OutDiagnostic = nullptr);

	UEAGENTINTERFACE_API const FQueryPublicProjectionDescriptor*
	FindQueryPublicProjectionDescriptorForCommand(const FString& CommandId, Contract::FDiagnostic* OutDiagnostic = nullptr);

	UEAGENTINTERFACE_API Projection::EUeAgentProjectionPageUnit
	QueryPublicProjectionPageUnit(
		const FQueryPublicProjectionDescriptor& Descriptor);
}

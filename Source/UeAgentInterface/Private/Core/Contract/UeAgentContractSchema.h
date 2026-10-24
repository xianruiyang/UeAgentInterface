#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentContract.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"

class FJsonObject;
class FJsonValue;

namespace UE::UAI::Contract
{
	/** Construct an owner's input diagnostic without changing its error code or inventing a schema path. */
	UEAGENTINTERFACE_API FDiagnostic MakeInputDiagnostic(
		const FString& Code,
		const FString& InstancePath,
		const FString& Keyword,
		const FString& Message,
		const FString* SchemaPath = nullptr,
		TSharedPtr<FJsonValue> Expected = nullptr);

	using FUeAgentContractSchemaError = FDiagnostic;

	using FUeAgentContractSchemaCheckpoint = TFunction<bool(
		int64 WorkUnits,
		const FString& InstancePath,
		FUeAgentContractSchemaError& OutError)>;

	/**
	 * Immutable, registry-compiled ContractSchema v1.
	 *
	 * Schema compilation and instance validation both use explicit work stacks;
	 * schema or instance nesting is bounded by the caller checkpoint, not by an
	 * artificial recursion-depth gate.
	 */
	class UEAGENTINTERFACE_API FUeAgentContractSchema final
	{
	public:
		static bool TryCompile(
			const FUeAgentContractValue& Schema,
			TSharedPtr<const FUeAgentContractSchema>& OutSchema,
			FUeAgentContractSchemaError& OutError);

		const FUeAgentContractValue& GetCanonicalSchema() const;

		bool TryValidateAndCanonicalize(
			const FUeAgentContractValue& Input,
			FUeAgentContractSchemaCheckpoint Checkpoint,
			TOptional<FUeAgentContractValue>& OutCanonicalInput,
			FUeAgentContractSchemaError& OutError) const;

	private:
		FUeAgentContractSchema(
			FUeAgentContractValue InCanonicalSchema,
			TSharedRef<const FJsonObject> InRoot);

		const FUeAgentContractValue CanonicalSchema;
		const TSharedRef<const FJsonObject> Root;
	};
}

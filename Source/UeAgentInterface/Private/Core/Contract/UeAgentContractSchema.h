#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentContract.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"

class FJsonObject;

namespace UE::UAI::Contract
{
	struct UEAGENTINTERFACE_API FUeAgentContractSchemaError
	{
		FString Code;
		FString InstancePath;
		FString SchemaPath;
		FString Keyword;
		FString Detail;

		bool IsValid() const
		{
			return !Code.IsEmpty();
		}
	};

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

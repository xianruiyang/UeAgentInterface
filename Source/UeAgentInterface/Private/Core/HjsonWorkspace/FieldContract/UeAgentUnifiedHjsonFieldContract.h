#pragma once

#include "CoreMinimal.h"
#include "Core/HjsonWorkspace/UeAgentExportFieldContract.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::HjsonWorkspace
{
	enum class EUnifiedHjsonFieldIntent : uint8
	{
		Preserve,
		SetValue,
		SetNull,
		RestoreDefault,
		Remove,
	};

	UEAGENTINTERFACE_API FString LexToString(
		EUnifiedHjsonFieldIntent Intent);

	class UEAGENTINTERFACE_API FUnifiedHjsonFieldContract final
	{
	public:
		FUnifiedHjsonFieldContract(
			FString InFieldId,
			bool bInNullable,
			TArray<EUnifiedHjsonFieldIntent> InAllowedIntents);

		const FString& GetFieldId() const;
		bool IsNullable() const;
		const TArray<EUnifiedHjsonFieldIntent>& GetAllowedIntents() const;
		bool Allows(EUnifiedHjsonFieldIntent Intent) const;
		bool IsValid() const;

	private:
		FString FieldId;
		bool bNullable = false;
		TArray<EUnifiedHjsonFieldIntent> AllowedIntents;
		bool bValid = false;
	};

	class FUnifiedHjsonFieldRegistry;
	using FUnifiedHjsonFieldRegistryPtr =
		TSharedPtr<const FUnifiedHjsonFieldRegistry>;
	using FUnifiedHjsonFieldRegistryRef =
		TSharedRef<const FUnifiedHjsonFieldRegistry>;

	class UEAGENTINTERFACE_API FUnifiedHjsonFieldRegistry final
	{
	public:
		const FExportFieldRegistryRef& GetBaseRegistry() const;
		const TArray<FUnifiedHjsonFieldContract>& GetContracts() const;
		const FUnifiedHjsonFieldContract* FindContract(
			const FString& FieldId) const;
		bool IsValid() const;

	private:
		FUnifiedHjsonFieldRegistry(
			FExportFieldRegistryRef InBaseRegistry,
			TArray<FUnifiedHjsonFieldContract> InContracts);

		FExportFieldRegistryRef BaseRegistry;
		TArray<FUnifiedHjsonFieldContract> Contracts;
		TMap<FString, int32> ContractIndexByFieldId;
		bool bValid = false;

		friend UEAGENTINTERFACE_API bool
			TryCompileUnifiedHjsonFieldRegistry(
				FExportFieldRegistryRef,
				TArray<FUnifiedHjsonFieldContract>,
				FUnifiedHjsonFieldRegistryPtr&,
				FWorkspaceError&);
	};

	UEAGENTINTERFACE_API bool TryCompileUnifiedHjsonFieldRegistry(
		FExportFieldRegistryRef BaseRegistry,
		TArray<FUnifiedHjsonFieldContract> Contracts,
		FUnifiedHjsonFieldRegistryPtr& OutRegistry,
		FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryResolveUnifiedHjsonFieldIntent(
		FUnifiedHjsonFieldRegistryRef Registry,
		const FString& FieldId,
		const TOptional<EUnifiedHjsonFieldIntent>& ExplicitIntent,
		EUnifiedHjsonFieldIntent& OutIntent,
		FWorkspaceError& OutError);
}

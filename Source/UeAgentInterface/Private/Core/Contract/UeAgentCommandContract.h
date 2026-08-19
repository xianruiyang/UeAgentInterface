#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::CommandContract
{
	enum class EUeAgentCommandWireType : uint8
	{
		Boolean,
		Integer,
		Number,
		String,
		Object,
		Array,
		Any,
	};

	enum class EUeAgentCommandStatus : uint8
	{
		Active,
		Deprecated,
	};

	class UEAGENTINTERFACE_API FUeAgentCommandParameter final
	{
	public:
		FUeAgentCommandParameter(
			FString InName,
			EUeAgentCommandWireType InWireType,
			bool bInRequired,
			bool bInNullable);

		const FString& GetName() const;
		EUeAgentCommandWireType GetWireType() const;
		bool IsRequired() const;
		bool IsNullable() const;
		bool IsValid() const;

	private:
		FString Name;
		EUeAgentCommandWireType WireType =
			EUeAgentCommandWireType::Any;
		bool bRequired = false;
		bool bNullable = false;
	};

	class UEAGENTINTERFACE_API FUeAgentCommandContract final
	{
	public:
		FUeAgentCommandContract(
			FString InCommandId,
			FString InNamespace,
			FString InMethod,
			TArray<FUeAgentCommandParameter> InParameters,
			int32 InMinWireVersion,
			int32 InMaxWireVersion,
			EUeAgentCommandStatus InStatus,
			FString InDeprecation = {});

		const FString& GetCommandId() const;
		const FString& GetNamespace() const;
		const FString& GetMethod() const;
		const TArray<FUeAgentCommandParameter>& GetParameters() const;
		int32 GetMinWireVersion() const;
		int32 GetMaxWireVersion() const;
		EUeAgentCommandStatus GetStatus() const;
		const FString& GetDeprecation() const;
		const FString& GetBindingHash() const;
		bool IsValid() const;

	private:
		FString CommandId;
		FString Namespace;
		FString Method;
		TArray<FUeAgentCommandParameter> Parameters;
		int32 MinWireVersion = 0;
		int32 MaxWireVersion = 0;
		EUeAgentCommandStatus Status =
			EUeAgentCommandStatus::Active;
		FString Deprecation;
		FString BindingHash;
	};

	class UEAGENTINTERFACE_API
		FUeAgentCommandContractRegistry final
	{
	public:
		static bool TryCreate(
			TArray<FUeAgentCommandContract> Contracts,
			TSharedPtr<const FUeAgentCommandContractRegistry>&
				OutRegistry,
			FString& OutError);

		const TArray<FUeAgentCommandContract>& GetContracts() const;
		const FUeAgentCommandContract* FindExact(
			const FString& CommandId) const;
		const FString& GetRegistryHash() const;

		bool BuildManifest(
			const FString& PluginVersion,
			const FString& BuildId,
			FString& OutManifest,
			FString& OutError) const;

	private:
		FUeAgentCommandContractRegistry(
			TArray<FUeAgentCommandContract> InContracts,
			FString InRegistryHash);

		TArray<FUeAgentCommandContract> Contracts;
		TMap<FString, int32> IndexByCommandId;
		FString RegistryHash;
	};

	class UEAGENTINTERFACE_API
		FUeAgentCommandManifestExporter final
	{
	public:
		static bool TryWrite(
			const FUeAgentCommandContractRegistry& Registry,
			const FString& PluginVersion,
			const FString& BuildId,
			const FString& ManifestPath,
			FString& OutError);

		static bool TryVerify(
			const FUeAgentCommandContractRegistry& Registry,
			const FString& PluginVersion,
			const FString& BuildId,
			const FString& ManifestPath,
			FString& OutError);
	};

	struct UEAGENTINTERFACE_API
		FUeAgentReadCommandContractDefinition final
	{
		FString CommandId;
		bool bRequiresTarget = false;

		bool IsValid() const;
	};

	UEAGENTINTERFACE_API const TArray<
		FUeAgentReadCommandContractDefinition>&
	GetReadCommandContractDefinitions();

	UEAGENTINTERFACE_API const FString&
	GetSemanticExportCommandId();

	UEAGENTINTERFACE_API
	TSharedRef<const FUeAgentCommandContractRegistry>
	GetDefaultCommandContractRegistry();

	UEAGENTINTERFACE_API const FString&
	GetUaiCommandManifestBuildId();

	UEAGENTINTERFACE_API FString
	GetUaiCommandManifestPluginVersion();

	UEAGENTINTERFACE_API FString GetDefaultCommandManifestPath();
}

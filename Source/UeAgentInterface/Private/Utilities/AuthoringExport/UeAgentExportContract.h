// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

namespace UeAgentExportContract
{
	enum class EFieldSourceKind : uint8
	{
		Semantic,
		FormatConstant,
		DerivedNavigation,
	};

	enum class ERegistrationResult : uint8
	{
		Registered,
		AlreadyRegistered,
	};

	struct UEAGENTINTERFACE_API FProfileDescriptor
	{
		FString ProfileId;
		TArray<FString> IncludedRoles;
		int32 TokenBudget = 0;

		bool IncludesRole(const FString& Role) const;
		bool operator==(const FProfileDescriptor& Other) const;
		bool operator!=(const FProfileDescriptor& Other) const { return !(*this == Other); }
	};

	struct UEAGENTINTERFACE_API FFileDescriptor
	{
		FString FileId;
		FString Path;
		FString Role;
		TArray<FString> Profiles;
		bool bRequired = false;
		FString ProjectionId;
		FString MergePolicy;

		bool IsEnabledForProfile(const FString& ProfileId) const;
		bool operator==(const FFileDescriptor& Other) const;
		bool operator!=(const FFileDescriptor& Other) const { return !(*this == Other); }
	};

	struct UEAGENTINTERFACE_API FFieldSourceDescriptor
	{
		EFieldSourceKind Kind = EFieldSourceKind::Semantic;
		FString SemanticId;
		FString ProjectorId;

		bool operator==(const FFieldSourceDescriptor& Other) const;
		bool operator!=(const FFieldSourceDescriptor& Other) const { return !(*this == Other); }
	};

	struct UEAGENTINTERFACE_API FFieldDescriptor
	{
		FString FieldId;
		FString OutputPath;
		FFieldSourceDescriptor Source;
		FString Role;
		bool bEditable = false;
		bool bRequired = false;
		FString Representation;
		FString EmptyPolicy;
		FString DefaultPolicy;
		FString Externalization;

		bool operator==(const FFieldDescriptor& Other) const;
		bool operator!=(const FFieldDescriptor& Other) const { return !(*this == Other); }
	};

	struct UEAGENTINTERFACE_API FWriterPolicy
	{
		FString Format;
		int32 InlineStructMaxFields = 0;
		bool bListCountsAsOneField = false;
		bool bScalarListInline = false;
		FString KnownStructComponents;
		FString TransformShape;
		TArray<FString> PreserveValueTextProfiles;
		TArray<FString> RemoveFields;
		FString PropertyPathPolicy;
		FString UnknownOutputFieldPolicy;

		bool PreservesValueText(const FString& ProfileId) const;
		bool operator==(const FWriterPolicy& Other) const;
		bool operator!=(const FWriterPolicy& Other) const { return !(*this == Other); }
	};

	struct UEAGENTINTERFACE_API FExtensionPolicy
	{
		FString Input;
		bool bDirectUObjectRead = true;

		bool operator==(const FExtensionPolicy& Other) const;
		bool operator!=(const FExtensionPolicy& Other) const { return !(*this == Other); }
	};

	struct UEAGENTINTERFACE_API FSortPolicy
	{
		bool bDeterministic = false;
		bool bIdentityBeforeContent = false;

		bool operator==(const FSortPolicy& Other) const;
		bool operator!=(const FSortPolicy& Other) const { return !(*this == Other); }
	};

	struct UEAGENTINTERFACE_API FContract
	{
		FString Schema;
		FString ContractId;
		FString ContractVersion;
		FString TypeId;
		TArray<FString> SemanticDescriptorRefs;
		FString DefaultProfile;
		TArray<FProfileDescriptor> Profiles;
		TArray<FFileDescriptor> Files;
		TArray<FFieldDescriptor> Fields;
		FWriterPolicy WriterPolicy;
		FExtensionPolicy ExtensionPolicy;
		FSortPolicy SortPolicy;

		const FProfileDescriptor* FindProfile(const FString& ProfileId) const;
		const FFileDescriptor* FindFile(const FString& FileId) const;
		const FFileDescriptor* FindFileByPath(const FString& Path) const;
		const FFieldDescriptor* FindField(const FString& FieldId) const;
		bool IsValid() const;
		bool operator==(const FContract& Other) const;
		bool operator!=(const FContract& Other) const { return !(*this == Other); }
	};

	class UEAGENTINTERFACE_API FRegistry
	{
	public:
		FRegistry() = default;
		FRegistry(const FRegistry&) = delete;
		FRegistry& operator=(const FRegistry&) = delete;

		static FRegistry& Get();

		bool RegisterContract(
			const FContract& Contract,
			ERegistrationResult& OutResult,
			FString& OutError);
		bool FindContract(const FString& ContractId, FContract& OutContract) const;
		TArray<FContract> ListContracts() const;
		int32 GetContractCount() const;

	private:
		mutable FRWLock RegistryLock;
		TMap<FString, FContract> Contracts;
	};

	UEAGENTINTERFACE_API FString FieldSourceKindToString(EFieldSourceKind Kind);
	UEAGENTINTERFACE_API bool ParseFieldSourceKind(const FString& Text, EFieldSourceKind& OutKind);
	UEAGENTINTERFACE_API bool SplitOutputPath(
		const FString& OutputPath,
		FString& OutFilePath,
		FString& OutJsonPointer);
	UEAGENTINTERFACE_API bool ParseContract(
		const TSharedPtr<FJsonObject>& Object,
		FContract& OutContract,
		FString& OutError);
	UEAGENTINTERFACE_API bool ValidateContract(const FContract& Contract, FString& OutError);
}

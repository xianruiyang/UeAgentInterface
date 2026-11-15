// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"

class FJsonObject;

namespace UeAgentAuthoringIdentity
{
	using UE::UAI::Contract::FDiagnostic;
	enum class ERegistrationResult : uint8
	{
		Registered,
		AlreadyRegistered
	};

	struct UEAGENTINTERFACE_API FSelectorDescriptor
	{
		FString Name;
		int32 Priority = 0;
		bool bCaseSensitive = false;
		bool bTrimValue = true;

		bool operator==(const FSelectorDescriptor& Other) const;
		bool operator!=(const FSelectorDescriptor& Other) const { return !(*this == Other); }
	};

	struct UEAGENTINTERFACE_API FKeyPattern
	{
		FString PatternId;
		int32 Priority = 0;
		TArray<FString> Selectors;

		bool operator==(const FKeyPattern& Other) const;
		bool operator!=(const FKeyPattern& Other) const { return !(*this == Other); }
	};

	struct UEAGENTINTERFACE_API FModelDescriptor
	{
		FString ModelId;
		FString TargetKind;
		TArray<FSelectorDescriptor> Selectors;
		TArray<FKeyPattern> KeyPatterns;

		bool operator==(const FModelDescriptor& Other) const;
		bool operator!=(const FModelDescriptor& Other) const { return !(*this == Other); }
	};

	struct UEAGENTINTERFACE_API FCanonicalIdentity
	{
		FString ModelId;
		FString TargetKind;
		FString KeyPattern;
		FString CanonicalKey;
		TMap<FString, FString> Selectors;

		bool IsValid() const;
		FDiagnostic GetValidationDiagnostic() const;
		FString GetSelector(const FString& Name) const;
		TSharedRef<FJsonObject> ToJsonObject() const;
	};

	using FModelView = TSharedPtr<const FModelDescriptor, ESPMode::ThreadSafe>;

	class UEAGENTINTERFACE_API FRegistry
	{
	public:
		FRegistry() = default;
		FRegistry(const FRegistry&) = delete;
		FRegistry& operator=(const FRegistry&) = delete;

		static FRegistry& Get();

		bool RegisterModel(
			const FModelDescriptor& Descriptor,
			ERegistrationResult& OutResult,
			UE::UAI::Contract::FDiagnostic& OutError);
		bool FindModel(const FString& ModelId, FModelDescriptor& OutDescriptor) const;
		/** A registered declaration is immutable; callers may borrow it without copying its selectors/patterns. */
		FModelView FindModelView(const FString& ModelId) const;
		TArray<FModelDescriptor> ListModels() const;
		int32 GetModelCount() const;

	private:
		mutable FRWLock RegistryLock;
		TMap<FString, FModelView> Models;
	};

	UEAGENTINTERFACE_API bool RegisterBuiltInModels(FRegistry& Registry, UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool BuildCanonicalIdentity(
		const FString& ModelId,
		const FString& TargetKind,
		const TMap<FString, FString>& Selectors,
		FCanonicalIdentity& OutIdentity,
		FDiagnostic& OutError,
		const FRegistry* Registry = nullptr);

	UEAGENTINTERFACE_API bool ParseCanonicalKey(
		const FString& CanonicalKey,
		FCanonicalIdentity& OutIdentity,
		FDiagnostic& OutError,
		const FRegistry* Registry = nullptr);

	/** Synchronous producer inputs, normalized/validated by the same builder.
	 * No view escapes into the resulting complete owning identity. */
	bool BuildCanonicalIdentityFromViews(const FString& ModelId, const FString& TargetKind,
		TConstArrayView<TPair<FStringView, FStringView>> Selectors, FCanonicalIdentity& OutIdentity,
		FDiagnostic& OutError, const FRegistry* Registry = nullptr);

	/** Compare the current source's generated key without materializing an
	 * identity. A valid mismatch reports its actual key, not another read path. */
	bool CompareCanonicalIdentityFromViews(const FString& ModelId, const FString& TargetKind,
		TConstArrayView<TPair<FStringView, FStringView>> Selectors, const FString& ExpectedKey,
		bool& OutMatches, FString& OutActualKey, FDiagnostic& OutError,
		const FRegistry* Registry = nullptr);

	/** Same key grammar and diagnostics; a key-only query consumes only these two fields. */
	bool ValidateCanonicalKey(const FString& CanonicalKey, const FRegistry& Registry, const FModelView& QueryModel,
		FString& OutModelId, FString& OutTargetKind, FDiagnostic& OutError);

	UEAGENTINTERFACE_API void WriteSelectorsFlat(
		const FCanonicalIdentity& Identity,
		const TSharedRef<FJsonObject>& Object);
}

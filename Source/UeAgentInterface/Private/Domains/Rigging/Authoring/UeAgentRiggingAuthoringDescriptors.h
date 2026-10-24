// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

namespace UeAgentRiggingAuthoring
{
	enum class EAssetKind : uint8
	{
		ControlRig,
		IKRig,
		IKRetargeter,
	};

	struct UEAGENTINTERFACE_API FProfile
	{
		EAssetKind Kind = EAssetKind::ControlRig;
		FString TypeId;
		FString TargetKind;
		FString ProfileId;
		FString IdentityModelId;
		FString DocumentSemanticId;
		FString ExportTargetSemanticId;
		FString InputContractId;
		FString WorkspaceContractId;
		FString ExportContractId;
	};

	inline constexpr const TCHAR* DomainId = TEXT("rigging");
	inline constexpr const TCHAR* ApplyProviderId = TEXT("rigging.asset.apply");
	inline constexpr const TCHAR* StateAccessorId = TEXT("rigging.asset.state");
	inline constexpr const TCHAR* FactoryDefaultProviderId = TEXT("rigging.asset.factory");

	UEAGENTINTERFACE_API const TArray<FProfile>& GetProfiles();
	UEAGENTINTERFACE_API const FProfile* FindProfile(EAssetKind Kind);
	UEAGENTINTERFACE_API const FProfile* FindProfileByTypeId(const FString& TypeId);
	UEAGENTINTERFACE_API const FProfile* FindProfileByTargetKind(const FString& TargetKind);
	UEAGENTINTERFACE_API const TCHAR* AssetKindToString(EAssetKind Kind);
	UEAGENTINTERFACE_API bool ParseAssetKind(const FString& Text, EAssetKind& OutKind);
	UEAGENTINTERFACE_API bool RegisterDescriptors(UE::UAI::Contract::FDiagnostic& OutError);
}

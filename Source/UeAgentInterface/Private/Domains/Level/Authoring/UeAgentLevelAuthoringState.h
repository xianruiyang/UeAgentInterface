// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"

class AActor;
class AWorldDataLayers;
class UActorComponent;
class UClass;
class UDataLayerInstance;
class UWorld;
class FJsonObject;

namespace UeAgentLevelAuthoring
{
	inline constexpr TCHAR SceneEditingProfileId[] = TEXT("scene_editing");
	/** Reflected Actor/Component properties in the optional scene-editing projection.
	 * Spatial state and hierarchy keep their existing dedicated contracts. */
	UEAGENTINTERFACE_API bool IsSceneEditingProperty(const FString& PropertyPath, bool bComponent);
	UEAGENTINTERFACE_API bool IsSceneEditingProperty(const FString& PropertyPath, bool bComponent, const UClass* TargetClass);
	/** Defer these names to the same policy after resolving the actual native class. */
	UEAGENTINTERFACE_API bool RequiresSceneEditingClassCheck(const FString& PropertyPath, bool bComponent);
	enum class EDataLayerTargetScope : uint8
	{
		Any,
		PrivateOnly,
	};

	UEAGENTINTERFACE_API FString GetWorldPath(const UWorld& World);
	UEAGENTINTERFACE_API FString GetManagedActorId(const AActor& Actor);
	UEAGENTINTERFACE_API FString MakeManagedActorTag(const FString& ManagedId);
	UEAGENTINTERFACE_API FName GetManagedActorMarkerTag();
	UEAGENTINTERFACE_API bool IsManagedActorIdentityTag(const FName& Tag);
	UEAGENTINTERFACE_API FString MakeManagedActorFolderPath(
		const FString& OwnerId);
	UEAGENTINTERFACE_API bool TryBindManagedActorOwnership(
		AActor& Actor,
		const FString& ManagedId,
		const FString& OwnerId,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool TryFindManagedActorExact(
		UWorld& World,
		const FString& ManagedId,
		const FString& ExpectedFolderPath,
		AActor*& OutActor,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API FString GetActorFolderPathText(const AActor& Actor);
	UEAGENTINTERFACE_API UWorld* FindEditorWorldByPath(const FString& WorldPath);
	UEAGENTINTERFACE_API AActor* FindActorExact(
		UWorld& World,
		const TSharedPtr<const FJsonObject>& Target,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API TArray<UDataLayerInstance*>
	FindDataLayerInstancesExact(
		AWorldDataLayers& WorldDataLayers,
		const FString& TargetRef,
		EDataLayerTargetScope Scope = EDataLayerTargetScope::Any);
	UEAGENTINTERFACE_API bool TrySelectDataLayerIdentityExact(
		const TArray<TArray<FString>>& CandidateIdentities,
		const FString& TargetRef,
		int32& OutCandidateIndex,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool TryResolveDataLayerInstanceExact(
		AWorldDataLayers& WorldDataLayers,
		const FString& TargetRef,
		UDataLayerInstance*& OutInstance,
		UE::UAI::Contract::FDiagnostic& OutError,
		EDataLayerTargetScope Scope = EDataLayerTargetScope::Any);
	UEAGENTINTERFACE_API UActorComponent* FindComponentExact(
		AActor& Owner,
		const FString& ComponentName,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool BuildWorldIdentity(
		const UWorld& World,
		UeAgentAuthoringIdentity::FCanonicalIdentity& OutIdentity,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool BuildActorIdentity(
		const AActor& Actor,
		UeAgentAuthoringIdentity::FCanonicalIdentity& OutIdentity,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool BuildComponentIdentity(
		const UActorComponent& Component,
		UeAgentAuthoringIdentity::FCanonicalIdentity& OutIdentity,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool BuildWorldSnapshotReadRequests(
		UWorld* World,
		const FString& WorldPath,
		bool bExpectedAbsent,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool BuildActorSnapshotReadRequests(
		AActor* Actor,
		const UeAgentAuthoringIdentity::FCanonicalIdentity& Identity,
		bool bExpectedAbsent,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool BuildComponentSnapshotReadRequests(
		UActorComponent* Component,
		const UeAgentAuthoringIdentity::FCanonicalIdentity& Identity,
		bool bExpectedAbsent,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool BuildWorldExportReadRequests(
		UWorld& World,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool BuildActorExportReadRequests(
		AActor& Actor,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		UE::UAI::Contract::FDiagnostic& OutError,
		bool bSceneEditing = false);
	TSet<FName> ComponentStructuralProperties(const UActorComponent& Component);

	UEAGENTINTERFACE_API bool BuildComponentExportReadRequests(
		UActorComponent& Component,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		UE::UAI::Contract::FDiagnostic& OutError,
		bool bSceneEditing = false);

	UEAGENTINTERFACE_API bool RegisterStateAccessor(UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool RegisterNativeDefaultProvider(UE::UAI::Contract::FDiagnostic& OutError);
}

#pragma once

#include "Composition/GraphCompound/UeAgentGraphCompoundAnimationBindings.h"
#include "Dom/JsonObject.h"
#include "UObject/StrongObjectPtr.h"

class UObject;
class UPackage;

namespace UE::UAI::DomainAdapters::AnimationRigGraph
{
	class FAnimationRigGraphNativeHook;
}

namespace UE::UAI::Composition::GraphCompound
{
	enum class EGraphCompoundDeformerReadbackKind : uint8
	{
		DeformerGraphCreate,
		DeformerGraphTopology,
		MeshDeformerCollectionCreate,
		MeshDeformerCollectionEntries,
	};

	struct FGraphCompoundDeformerEntrySpec final
	{
		FString EntryKind;
		FString EntryRef;
		int32 RequestedOrderIndex = INDEX_NONE;
		EChangeIntent Intent = EChangeIntent::Update;
	};

	struct FGraphCompoundDeformerReadbackSpec final
	{
		EGraphCompoundDeformerReadbackKind Kind =
			EGraphCompoundDeformerReadbackKind::DeformerGraphCreate;
		FString TargetKind;
		FString NativeClassPath;
		FString SkeletalMeshRef;
		FName CommentNodeName;
		TArray<FGraphCompoundDeformerEntrySpec> Entries;
	};

	struct FGraphCompoundDeformerCapturedValue final
	{
		FGraphCompoundDeformerCapturedValue(
			FString InSemanticId,
			TSharedRef<FJsonObject> InValue)
			: SemanticId(MoveTemp(InSemanticId))
			, Value(MoveTemp(InValue))
		{
		}

		FString SemanticId;
		TSharedRef<FJsonObject> Value;
	};

	bool IsDeformerProductionProfile(
		const FPcgAnimationGraphRouteProfile& Profile);
	bool IsDeformerCreateProfile(
		const FPcgAnimationGraphRouteProfile& Profile);
	bool IsDeformerMutationProfile(
		const FPcgAnimationGraphRouteProfile& Profile);
	bool IsUnavailableConcreteDeformerProfile(
		const FPcgAnimationGraphRouteProfile& Profile);

	bool TrySetUnavailableConcreteDeformerError(
		const FPcgAnimationGraphRouteProfile& Profile,
		FGraphCompoundRuntimeError& OutError);

	bool TryParseDeformerReadbackSpec(
		const FPcgAnimationGraphRouteProfile& Profile,
		const FGraphCompoundAnimationOperationRequest& Request,
		FGraphCompoundDeformerReadbackSpec& OutSpec,
		FGraphCompoundRuntimeError& OutError);

	bool TryCaptureDeformerValues(
		const FPcgAnimationGraphRouteProfile& Profile,
		UObject& Target,
		TArray<FGraphCompoundDeformerCapturedValue>& OutValues,
		FGraphCompoundRuntimeError& OutError);

	TSharedRef<FJsonObject> DeformerExpectedJson(
		const FGraphCompoundDeformerReadbackSpec& Spec);

	bool TryBuildDeformerActualJson(
		const FString& TargetRef,
		const FGraphCompoundDeformerReadbackSpec& Spec,
		TSharedRef<FJsonObject>& OutRoot,
		FString& OutError);

	void AddDeformerDependencyAnchors(
		const FGraphCompoundDeformerReadbackSpec& Spec,
		TArray<TStrongObjectPtr<UObject>>& OutAnchors);

	UObject* TryCreateDeformerAsset(
		const FPcgAnimationGraphRouteProfile& Profile,
		const FGraphCompoundAnimationOperationRequest& Request,
		const FGraphCompoundDeformerReadbackSpec& Spec,
		UPackage& Package,
		FGraphCompoundRuntimeError& OutError);

	bool TryApplyDeformerMutation(
		const FPcgAnimationGraphRouteProfile& Profile,
		UObject& Target,
		const FGraphCompoundDeformerReadbackSpec& Spec,
		DomainAdapters::AnimationRigGraph::FAnimationRigGraphNativeHook& Hook,
		FString& OutRevision,
		FGraphCompoundRuntimeError& OutError);
}

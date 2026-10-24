#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Composition/GraphCompound/UeAgentGraphCompoundAnimationBindings.h"
#include "DomainAdapters/AnimationRigGraph/UeAgentAnimationRigGraphNativeHook.h"
#include "Dom/JsonObject.h"
#include "UObject/StrongObjectPtr.h"

class UObject;
class UPackage;

namespace UE::UAI::Composition::GraphCompound
{
	using namespace DomainAdapters::AnimationRigGraph;

	enum class ERigIkReadbackKind : uint8
	{
		ControlRigCreate,
		ControlRigDefinition,
		ShapeLibraryCreate,
		ShapeLibraryDefinition,
		IkRigCreate,
		IkRigDefinition,
		IkRigAutoGenerate,
		IkRetargeterCreate,
		IkRetargeterDefinition,
		IkRetargeterSettings,
	};

	struct FRigIkCapturedValue final
	{
		FString SemanticId;
		TSharedRef<FJsonObject> Value;
	};

	struct FRigIkReadbackSpec final
	{
		ERigIkReadbackKind Kind = ERigIkReadbackKind::ControlRigCreate;
		FString TargetKind;
		FString NativeClassPath;
		FString SkeletonRef;
		FString SkeletalMeshRef;
		FString SourceRigRef;
		FString TargetRigRef;
		FString SourceRigInputPointer;
		FString TargetRigInputPointer;
		FString Preset;
		TArray<FAnimationRigGraphControlMutation> Controls;
		TArray<FAnimationRigGraphUnitMutation> Units;
		TArray<FAnimationRigGraphShapeMutation> Shapes;
		FName RetargetRoot;
		FString RetargetRootInputPointer;
		FString AutoGenerateInputPointer;
		TArray<FAnimationRigGraphIkGoalMutation> Goals;
		TArray<FAnimationRigGraphIkSolverMutation> Solvers;
		TArray<FAnimationRigGraphIkChainMutation> Chains;
		TArray<FAnimationRigGraphRetargetMappingMutation> Mappings;
		TArray<FAnimationRigGraphRetargetSettingMutation> Settings;
	};

	bool IsRigIkCreateProfile(const FPcgAnimationGraphRouteProfile& Profile);
	bool IsRigIkMutationProfile(const FPcgAnimationGraphRouteProfile& Profile);

	bool TryCaptureRigIkValues(
		const FPcgAnimationGraphRouteProfile& Profile,
		UObject& Target,
		TArray<FRigIkCapturedValue>& OutValues,
		FGraphCompoundRuntimeError& OutError);

	bool TryParseRigIkReadbackSpec(
		const FPcgAnimationGraphRouteProfile& Profile,
		const FGraphCompoundAnimationOperationRequest& Request,
		FRigIkReadbackSpec& OutSpec,
		FGraphCompoundRuntimeError& OutError);

	TSharedRef<FJsonObject> RigIkExpectedJson(const FRigIkReadbackSpec& Spec);

	bool TryBuildRigIkActualJson(
		const FString& TargetRef,
		const FRigIkReadbackSpec& Spec,
		TSharedRef<FJsonObject>& OutRoot,
		UE::UAI::Contract::FDiagnostic& OutError);

	UObject* TryCreateRigIkAsset(
		const FPcgAnimationGraphRouteProfile& Profile,
		const FGraphCompoundAnimationOperationRequest& Request,
		const FRigIkReadbackSpec& Spec,
		UPackage& Package,
		FGraphCompoundRuntimeError& OutError);

	bool TryApplyRigIkMutation(
		const FPcgAnimationGraphRouteProfile& Profile,
		UObject& Target,
		FRigIkReadbackSpec& Spec,
		FAnimationRigGraphNativeHook& Hook,
		FString& OutRevision,
		FGraphCompoundRuntimeError& OutError);

	void AddRigIkDependencyAnchors(
		const FRigIkReadbackSpec& Spec,
		TArray<TStrongObjectPtr<UObject>>& OutAnchors);
}

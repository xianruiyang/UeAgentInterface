#pragma once

#include "CoreMinimal.h"
#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialNative.h"
#include "UObject/WeakObjectPtr.h"

class APCGPartitionActor;
class UGeometryCache;
class UPCGComponent;
class UPCGDataAsset;

namespace UE::UAI::DomainAdapters::RigSimulationSpatial
{
	struct UEAGENTINTERFACE_API FUeAgentRigSimulationSpatialDeformerGeometryBinding
	{
		FString Identity;
		TWeakObjectPtr<UGeometryCache> GeometryCache;
		FString NativeClockId;
		UE::UAI::StateSemantic::FUeAgentSpatialFrame NativeFrame{
			TEXT(""), TEXT(""), 0, {}, {}, TEXT(""), TEXT(""),
			UE::UAI::StateSemantic::FUeAgentTemporalPoint(TEXT(""), TEXT(""))};

		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FUeAgentRigSimulationSpatialDeformerTrainingInputBinding
	{
		FString Identity;
		TWeakObjectPtr<UObject> Input;
		FString NativeClockId;
		UE::UAI::StateSemantic::FUeAgentSpatialFrame NativeFrame{
			TEXT(""), TEXT(""), 0, {}, {}, TEXT(""), TEXT(""),
			UE::UAI::StateSemantic::FUeAgentTemporalPoint(TEXT(""), TEXT(""))};

		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FUeAgentRigSimulationSpatialDeformerEvaluationBinding
	{
		FString Identity;
		FString SubjectIdentity;
		TWeakObjectPtr<UGeometryCache> EvaluationGeometry;
		TWeakObjectPtr<UObject> Subject;
		FString NativeClockId;
		UE::UAI::StateSemantic::FUeAgentSpatialFrame NativeFrame{
			TEXT(""), TEXT(""), 0, {}, {}, TEXT(""), TEXT(""),
			UE::UAI::StateSemantic::FUeAgentTemporalPoint(TEXT(""), TEXT(""))};

		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FUeAgentRigSimulationSpatialPcgGenerationBinding
	{
		FRigSimulationSpatialGenerationIdentity Identity;
		TWeakObjectPtr<UPCGComponent> SourceComponent;
		TWeakObjectPtr<UPCGDataAsset> GeneratedData;
		TArray<TWeakObjectPtr<APCGPartitionActor>> Partitions;
		UE::UAI::StateSemantic::FUeAgentTemporalPoint GeneratedAt{TEXT(""), TEXT("")};
		UE::UAI::StateSemantic::FUeAgentSpatialFrame NativeFrame{
			TEXT(""), TEXT(""), 0, {}, {}, TEXT(""), TEXT(""),
			UE::UAI::StateSemantic::FUeAgentTemporalPoint(TEXT(""), TEXT(""))};

		bool IsValid() const;
	};

	UEAGENTINTERFACE_API bool TryCreateRigSimulationSpatialDeformerPcgNativePorts(
		TSharedRef<IUeAgentRigSimulationSpatialNativeEnvironment> Environment,
		FUeAgentRigSimulationSpatialNativePorts& OutPorts,
		FUeAgentRigSimulationSpatialExecutionError& OutError);
}

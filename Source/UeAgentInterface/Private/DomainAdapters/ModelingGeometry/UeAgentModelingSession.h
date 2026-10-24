#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class FJsonObject;

namespace UE::UAI::DomainAdapters::ModelingGeometry
{
	struct FModelingSessionRequest
	{
		FString RequestId;
		FString CommandId;
		TSharedPtr<FJsonObject> Params;
	};

	class FModelingSession
	{
	public:
		static bool ExecuteAction(
			const FString& Action,
			const FModelingSessionRequest& Request,
			TSharedPtr<FJsonObject>& OutObservation,
			UE::UAI::Contract::FDiagnostic& OutError);

		static bool Activate(const FModelingSessionRequest& Request, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError);
		static bool ObserveSelection(const FModelingSessionRequest& Request, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError);
		static bool SetActorSelection(const FModelingSessionRequest& Request, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError);
		static bool SetSelectionMode(const FModelingSessionRequest& Request, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError);
		static bool ObserveMeshSelectionInfo(const FModelingSessionRequest& Request, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError);
		static bool ClearMeshSelection(const FModelingSessionRequest& Request, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError);
		static bool SelectViaScreen(const FModelingSessionRequest& Request, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError);
		static bool SelectViaWorldRay(const FModelingSessionRequest& Request, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError);
		static bool SaveMeshAsset(const FModelingSessionRequest& Request, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError);

		static bool StartTool(const FModelingSessionRequest& Request, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError);
		static bool ObserveTool(const FModelingSessionRequest& Request, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError);
		static bool SetToolProperty(const FModelingSessionRequest& Request, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError);
		static bool InvokeToolAction(const FModelingSessionRequest& Request, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError);
		static bool AcceptTool(const FModelingSessionRequest& Request, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError);
		static bool CancelTool(const FModelingSessionRequest& Request, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError);

	private:
		static class UWorld* ResolveEditorWorld(UE::UAI::Contract::FDiagnostic& OutError);
	};
}

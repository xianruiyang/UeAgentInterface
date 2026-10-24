#include "DomainAdapters/ModelingGeometry/UeAgentModelingSession.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "DomainAdapters/ModelingGeometry/UeAgentModelingMechanics.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "Core/Contract/UeAgentMechanicsRequestContext.h"
#include "Module/UeAgentInterfaceLogger.h"

namespace UE::UAI::DomainAdapters::ModelingGeometry
{
	bool FModelingSession::SaveMeshAsset(
		const FModelingSessionRequest& Request,
		TSharedPtr<FJsonObject>& OutData,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		using namespace UeAgentModelingOps;
		OutData = MakeShared<FJsonObject>();

		UWorld* World = ResolveEditorWorld(OutError);
		if (!World)
		{
			return false;
		}

		FUeAgentRequestContext Context;
		Context.RequestId = Request.RequestId;
		Context.Command = Request.CommandId;
		Context.Params = Request.Params;

		TArray<AActor*> Actors;
		if (!ResolveSelectionActorsFromContext(Context, World, Actors, OutError))
		{
			return false;
		}

		TArray<FString> SavedPackagePaths;
		if (!SaveActorStaticMeshes(Actors, SavedPackagePaths, OutError))
		{
			OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("no_static_mesh_packages_saved"), OutError, TEXT("save"));
			return false;
		}

		TArray<TSharedPtr<FJsonValue>> SavedPackageValues;
		SavedPackageValues.Reserve(SavedPackagePaths.Num());
		for (const FString& PackagePath : SavedPackagePaths)
		{
			SavedPackageValues.Add(MakeShared<FJsonValueString>(PackagePath));
		}
		OutData->SetNumberField(TEXT("saved_package_count"), SavedPackagePaths.Num());
		OutData->SetArrayField(TEXT("saved_package_paths"), SavedPackageValues);
		FUeAgentInterfaceLogger::Log(
			TEXT("ModelingSaveMeshAsset req=%s saved_packages=%d"),
			*Request.RequestId,
			SavedPackagePaths.Num());
		return true;
	}
}

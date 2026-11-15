// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringExport.h"

#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringAssets.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringDescriptors.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringProjection.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringState.h"
#include "Misc/Paths.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringWorkspacePaths.h"
#include "Utilities/AuthoringExport/UeAgentSnapshotProjector.h"

namespace UeAgentReflectionAuthoring
{
	namespace
	{
		bool CaptureExportState(
			const FProfile& Profile,
			UObject& Asset,
			UeAgentAuthoringState::FCaptureResult& OutCapture,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			TArray<UeAgentAuthoringState::FReadRequest> Requests;
			if (!BuildAssetExportSnapshotReadRequests(
				Profile,
				Asset,
				Requests,
				OutError))
			{
				return false;
			}
			if (!UeAgentAuthoringState::CaptureSnapshot(
				Requests,
				OutCapture))
			{
				OutError = OutCapture.Diagnostic;
				return false;
			}
			return true;
		}

		bool BuildProjection(
			const FProfile& Profile,
			UObject& Asset,
			const FString& ExportProfileId,
			UeAgentExportContract::FContract& OutContract,
			UeAgentSnapshotProjector::FProjectionResult& OutProjection,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (!GetExportContract(Profile, OutContract, OutError)) return false;
			UeAgentAuthoringState::FCaptureResult Capture;
			if (!CaptureExportState(
				Profile,
				Asset,
				Capture,
				OutError)) return false;
			if (!UeAgentSnapshotProjector::ProjectSnapshot(OutContract, ExportProfileId, Capture.Snapshot, OutProjection))
			{
				OutError = OutProjection.Diagnostic;
				return false;
			}
			return true;
		}
	}

	bool ResolveDefaultWorkspaceFolder(
		const FProfile& Profile,
		const FString& AssetPath,
		FString& OutWorkspaceRoot,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		const FString DomainRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), Profile.TypeId));
		return UeAgentAuthoringWorkspacePaths::ResolveAssetWorkspaceFolder(DomainRoot, NormalizePackagePath(AssetPath), OutWorkspaceRoot, OutError);
	}

	bool ExportAssetWorkspace(
		const FProfile& Profile,
		UObject& Asset,
		const FString& WorkspaceRoot,
		const FString& ExportProfileId,
		UeAgentExportWorkspaceWriter::FWorkspaceResult& OutResult,
		UE::UAI::HjsonWorkspace::FPreparedWorkspaceExport* PreparedExport)
	{
  auto& OutError = OutResult.Diagnostic;
		OutResult = UeAgentExportWorkspaceWriter::FWorkspaceResult();
		UeAgentAuthoringState::FCaptureResult Capture;
		if (!CaptureExportState(
			Profile,
			Asset,
			Capture,
			OutError)) return false;
		return UE::UAI::HjsonWorkspace::TryExportCompositionWorkspace(UE::UAI::HjsonWorkspace::ResolveTargetCompositionKind(
				Profile.TargetKind,
				Profile.TypeId), ExportProfileId, Capture.Snapshot, WorkspaceRoot, OutResult, PreparedExport);
	}

	bool ExportAssetFile(
		const FProfile& Profile,
		UObject& Asset,
		const FString& OutputFile,
		const FString& ExportProfileId,
		UeAgentExportWorkspaceWriter::FWorkspaceResult& OutResult)
	{
  auto& OutError = OutResult.Diagnostic;
		OutResult = UeAgentExportWorkspaceWriter::FWorkspaceResult();
		UeAgentExportContract::FContract Contract;
		UeAgentSnapshotProjector::FProjectionResult Projection;
		if (!BuildProjection(Profile, Asset, ExportProfileId, Contract, Projection, OutError)) return false;
		if (!UeAgentExportWorkspaceWriter::WriteSingleFile(
			Contract, Projection, OutputFile, OutResult))
		{
			OutError = OutResult.Diagnostic;
			return false;
		}
		OutError.Reset();
		return true;
	}
}

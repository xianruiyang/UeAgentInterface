#include "Core/EditorWorld/UeAgentEditorWorldActivation.h"

#include "Editor.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PackageTools.h"
#include "UObject/Package.h"

namespace UE::UAI::EditorWorld
{
namespace
{
bool Fail(FWorldActivationError& OutError, FString Code, FString Reason, const FString& TargetPackage)
{
	OutError = UE::UAI::Contract::FDiagnostic::Failure(MoveTemp(Code), MoveTemp(Reason), TEXT("world_activation"));
	OutError.TargetPath = TargetPackage;
	return false;
}
} // namespace

bool TryActivateExistingWorld(const FString& TargetPackage,
	const ECurrentWorldDirtyPolicy DirtyPolicy, FWorldActivationResult& OutResult,
	FWorldActivationError& OutError)
{
	OutResult = {};
	OutError = {};
	if (!GEditor)
	{
		return Fail(OutError, TEXT("editor_world_editor_unavailable"),
			TEXT("Unreal Editor is unavailable; the requested map cannot be activated."), TargetPackage);
	}
	UWorld* Current = GEditor->GetEditorWorldContext().World();
	if (!Current || !Current->GetOutermost())
	{
		return Fail(OutError, TEXT("editor_world_current_missing"),
			TEXT("The editor world context has no current map package."), TargetPackage);
	}
	OutResult.OriginalPackage = Current->GetOutermost()->GetName();
	OutResult.bOriginalHasPersistentPackage =
		FPackageName::DoesPackageExist(OutResult.OriginalPackage);
	if (Current->GetOutermost()->IsDirty())
	{
		if (DirtyPolicy == ECurrentWorldDirtyPolicy::RequireClean)
		{
			return Fail(OutError, TEXT("editor_world_current_dirty"),
				TEXT("The current map has unsaved changes and the activation policy requires a clean map."), OutResult.OriginalPackage);
		}
		if (!UEditorLoadingAndSavingUtils::SaveDirtyPackages(true, false) ||
			Current->GetOutermost()->IsDirty())
		{
			return Fail(
				OutError, TEXT("editor_world_current_save_failed"),
				TEXT("Unreal Editor could not save the current map before activating the requested map."), OutResult.OriginalPackage);
		}
	}
	if (OutResult.OriginalPackage == TargetPackage)
	{
		OutResult.World = Current;
		return true;
	}

	const FString Filename = FPackageName::LongPackageNameToFilename(
		TargetPackage, FPackageName::GetMapPackageExtension());
	if (!FPackageName::DoesPackageExist(TargetPackage) && !FPaths::FileExists(Filename))
	{
		return Fail(OutError, TEXT("editor_world_target_not_found"),
			TEXT("The requested map package does not exist."), TargetPackage);
	}
	if (UPackage* const LoadedTarget = FindPackage(nullptr, *TargetPackage))
	{
		if (LoadedTarget->IsDirty())
		{
			return Fail(OutError, TEXT("editor_world_loaded_target_dirty"),
				TEXT("The requested map is already loaded with unsaved changes and cannot be reloaded."), TargetPackage);
		}
		FText UnloadError;
		if (!UPackageTools::UnloadPackages({LoadedTarget}, UnloadError, true))
		{
			return Fail(
				OutError, TEXT("editor_world_loaded_target_unload_failed"),
				UnloadError.IsEmpty() ? TEXT("Unreal Editor could not unload the requested map before reloading it.") : UnloadError.ToString(),
				TargetPackage);
		}
	}

	UWorld* Activated = UEditorLoadingAndSavingUtils::LoadMap(Filename);
	if (!Activated || !Activated->GetOutermost() ||
		Activated->GetOutermost()->GetName() != TargetPackage)
	{
		Fail(OutError, TEXT("editor_world_load_failed"),
			TEXT("Loading the requested map did not produce a world with the requested package."), TargetPackage);
		OutError.ExpectedValue = MakeShared<FJsonValueString>(TargetPackage);
		if (Activated && Activated->GetOutermost())
		{
			OutError.ActualValue = MakeShared<FJsonValueString>(Activated->GetOutermost()->GetName());
		}
		else
		{
			OutError.ActualValue = MakeShared<FJsonValueNull>();
		}
		return false;
	}
	OutResult.World = Activated;
	OutResult.bSwitched = true;
	return true;
}
} // namespace UE::UAI::EditorWorld

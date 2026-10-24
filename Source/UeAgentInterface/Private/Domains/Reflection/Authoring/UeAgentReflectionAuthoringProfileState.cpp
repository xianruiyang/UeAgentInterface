// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringProfileState.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Domains/Material/Authoring/UeAgentMaterialInstanceAuthoring.h"
#include "Domains/Material/Authoring/UeAgentMaterialCollectionAuthoring.h"
#include "Materials/MaterialParameterCollection.h"
#include "Domains/PCG/Authoring/UeAgentPcgDataAssetAuthoring.h"
#include "Domains/Deformer/Authoring/UeAgentMlDeformerAuthoring.h"
#include "PCGDataAsset.h"
#include "Domains/Material/Authoring/UeAgentMaterialFunctionInstanceAuthoring.h"
#include "Materials/MaterialFunctionInstance.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringDescriptors.h"
#include "Domains/Reflection/Authoring/UeAgentSoundClassAuthoringRelations.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundSubmix.h"
#include "Domains/Reflection/Authoring/UeAgentSoundSubmixAuthoringRelations.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringProfileStateAI.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringProfileStateSequenceStateTree.h"
#include "Domains/StateTree/Authoring/UeAgentStateTreeDocument.h"
#include "StateTree.h"
#include "Utilities/UeAgentCurveJson.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "JsonUtils/JsonPointer.h"
#include "Domains/Reflection/Authoring/UeAgentStaticMeshAuthoringState.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshCompiler.h"
#include "Engine/SkeletalMesh.h"
#include "SkinnedAssetCompiler.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Domains/Reflection/Authoring/UeAgentPhysicsAssetAuthoringState.h"
#include "Domains/Reflection/Authoring/UeAgentSkeletalMeshAuthoringState.h"
#include "Domains/Reflection/Authoring/UeAgentPaperSpriteAuthoringState.h"
#include "PaperSprite.h"
#include "Misc/ScopeExit.h"
#include "UObject/Class.h"

namespace UeAgentReflectionAuthoring::ProfileState
{
	UE::UAI::Contract::FDiagnostic TargetFailure(const FString& Code, const UObject& Asset, const UClass& Expected, const FString& Stage)
	{
		auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code,
			TEXT("The profile state operation requires a target derived from the expected class."), Stage);
		Error.SourceKind = TEXT("native_result");
		Error.TargetPath = Asset.GetPathName();
		Error.PropertyPath = TEXT("target_class");
		Error.ActualValue = MakeShared<FJsonValueString>(Asset.GetClass()->GetPathName());
		Error.ExpectedValue = MakeShared<FJsonValueString>(Expected.GetPathName());
		return Error;
	}

	void BindTarget(UE::UAI::Contract::FDiagnostic& Error, const FString& NativePath, const FString& SemanticAssetPath)
	{
		if (Error.IsEmpty() || SemanticAssetPath.IsEmpty()) return;
		if (Error.TargetPath.IsEmpty()) Error.TargetPath = SemanticAssetPath;
		else if (!NativePath.IsEmpty() && (Error.TargetPath == NativePath || Error.TargetPath.StartsWith(NativePath + TEXT(":"))
			|| Error.TargetPath.StartsWith(NativePath + TEXT("."))))
		{
			if (Error.TargetPath != SemanticAssetPath && Error.InternalContext.IsEmpty())
				Error.InternalContext = Error.TargetPath;
			Error.TargetPath = SemanticAssetPath + Error.TargetPath.Mid(NativePath.Len());
		}
		for (auto& Child : Error.Diagnostics) BindTarget(Child, NativePath, SemanticAssetPath);
		if (Error.PublicCause)
		{
			auto Cause = MakeShared<UE::UAI::Contract::FDiagnostic>(*Error.PublicCause);
			BindTarget(*Cause, NativePath, SemanticAssetPath);
			Error.PublicCause = MoveTemp(Cause);
		}
	}

	using FDiagnostic = UE::UAI::Contract::FDiagnostic;

	namespace
	{
		bool HasInputPath(const TSharedPtr<FJsonValue>& Input, const FString& Pointer)
		{
			if (Pointer.IsEmpty() || Pointer == TEXT("/")) return Input.IsValid();
			if (!Pointer.StartsWith(TEXT("/"))) return false;
			TSharedPtr<FJsonValue> Found;
			return UE::Json::FJsonPointer(Pointer).TryGet(Input, Found) && Found.IsValid();
		}
	}

	// Resolved settings include native baseline fields. Only an actual input leaf
	// may acquire an authored location; do not use an ancestor source-map fallback
	// to label an inherited native value as HJSON input.
	void BindInput(FDiagnostic& Error, const TSharedPtr<FJsonValue>& Input)
	{
		for (auto& Child : Error.Diagnostics) BindInput(Child, Input);
		if (Error.PublicCause)
		{
			auto Cause = MakeShared<FDiagnostic>(*Error.PublicCause);
			BindInput(*Cause, Input);
			Error.PublicCause = MoveTemp(Cause);
		}
		if (!Error.Source.SourceFile.IsEmpty() || Error.SourceKind == TEXT("native_result")
			|| Error.SourceKind == TEXT("command_input")) return;
		const FString Pointer = Error.Source.InternalJsonPath;
		if (!HasInputPath(Input, Pointer))
		{
			Error.SourceKind = TEXT("native_result");
			Error.Source.SourceUnit.Reset();
			Error.Source.InternalJsonPath.Reset();
			Error.Source.JsonPath.Reset();
			return;
		}
		const bool bRelatedInput = Error.SourceKind == TEXT("related_input");
		Error.SourceKind = TEXT("authored_value");
		if (Input && Input->Type == EJson::Object)
			UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Input->AsObject());
		if (bRelatedInput && Error.SourceKind != TEXT("command_input"))
			Error.SourceKind = TEXT("related_input");
	}

	void MarkNative(FDiagnostic& Error)
	{
		for (auto& Child : Error.Diagnostics) MarkNative(Child);
		if (Error.PublicCause)
		{
			auto Cause = MakeShared<FDiagnostic>(*Error.PublicCause);
			MarkNative(*Cause);
			Error.PublicCause = MoveTemp(Cause);
		}
		if (Error.Source.SourceFile.IsEmpty() && Error.SourceKind != TEXT("command_input"))
			Error.SourceKind = TEXT("native_result");
	}

	void PrefixInputLocation(FDiagnostic& Error, const FString& Prefix)
	{
		for (auto& Child : Error.Diagnostics) PrefixInputLocation(Child, Prefix);
		if (Error.PublicCause)
		{
			auto Cause = MakeShared<FDiagnostic>(*Error.PublicCause);
			PrefixInputLocation(*Cause, Prefix);
			Error.PublicCause = MoveTemp(Cause);
		}
		if (!Error.Source.SourceFile.IsEmpty() || Error.SourceKind == TEXT("native_result")
			|| Error.SourceKind == TEXT("command_input")) return;
		const FString Relative = !Error.Source.InternalJsonPath.IsEmpty() ? Error.Source.InternalJsonPath
			: !Error.SemanticPointer.IsEmpty() ? Error.SemanticPointer : Error.Source.JsonPath;
		Error.Source.InternalJsonPath = Prefix + (Relative.StartsWith(TEXT("/")) && Relative != TEXT("/") ? Relative : FString());
	}

	void AddRelatedInput(FDiagnostic& Error, const TSharedPtr<FJsonValue>& Input, const FString& Pointer)
	{
		if (!HasInputPath(Input, Pointer) || !Input || Input->Type != EJson::Object) return;
		FDiagnostic Location;
		Location.Source.InternalJsonPath = Pointer;
		UeAgentAuthoringDiagnostics::LocateAuthoringSource(Location, *Input->AsObject());
		const auto& Source = Location.Source;
		if (Source.SourceFile.IsEmpty() && Source.JsonPath.IsEmpty()) return;
		for (const auto& Existing : Error.RelatedSources)
		{
			if (!Existing || Existing->Type != EJson::Object) continue;
			FString File, Path;
			Existing->AsObject()->TryGetStringField(TEXT("source_file"), File);
			Existing->AsObject()->TryGetStringField(TEXT("json_path"), Path);
			if (File == Source.SourceFile && Path == Source.JsonPath) return;
		}
		auto Related = MakeShared<FJsonObject>();
		Related->SetStringField(TEXT("source_kind"), TEXT("related_input"));
		if (!Source.SourceFile.IsEmpty()) Related->SetStringField(TEXT("source_file"), Source.SourceFile);
		if (!Source.JsonPath.IsEmpty()) Related->SetStringField(TEXT("json_path"), Source.JsonPath);
		if (Source.Line > 0) Related->SetNumberField(TEXT("line"), Source.Line);
		if (Source.Column > 0) Related->SetNumberField(TEXT("column"), Source.Column);
		Error.RelatedSources.Add(MakeShared<FJsonValueObject>(Related));
	}

	void LocateSettingsFailure(FDiagnostic& Error, const TSharedPtr<FJsonValue>& Settings,
		const FString& Pointer, const TSharedPtr<FJsonValue>& Input)
	{
		UeAgentDetailsAuthoringService::LocateReadablePropertyFailure(Error, Settings, Pointer, [&Input](FDiagnostic& Issue, bool bExact)
		{
			BindInput(Issue, Input);
			if (!bExact && Issue.SourceKind != TEXT("native_result") && Issue.SourceKind != TEXT("command_input"))
				Issue.SourceKind = TEXT("related_input");
		});
	}

	namespace
	{
		FDiagnostic InputFailure(const FProfile& Profile, const TSharedPtr<FJsonValue>& Patch,
			const FString& Code, const FString& Expected, const FString& Reason, const FString& Stage)
		{
			auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Profile.ProfileStateField,
				MakeShared<FJsonValueString>(Expected), Patch, Stage, Reason);
			Error.SourceKind = TEXT("authored_value");
			Error.Source.InternalJsonPath = TEXT("/");
			return Error;
		}

		FDiagnostic SurfaceFailure(const FProfile& Profile)
		{
			auto Error = FDiagnostic::Failure(TEXT("reflection_profile_state_surface_missing"),
				TEXT("No profile state owner supports the requested profile and asset class."), TEXT("profile_state"));
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = TEXT("profile_type");
			auto Actual = MakeShared<FJsonObject>();
			Actual->SetStringField(TEXT("profile_type"), Profile.TypeId);
			Actual->SetStringField(TEXT("asset_class"), Profile.AssetClassPath);
			Error.ActualValue = MakeShared<FJsonValueObject>(Actual);
			Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("a supported profile state owner"));
			return Error;
		}

		bool IsFunctionInstance(const FProfile& Profile)
		{
			return Profile.TypeId == TEXT("material_function_instance") || Profile.TypeId == TEXT("material_layer_instance")
				|| Profile.TypeId == TEXT("material_layer_blend_instance");
		}
		bool ValidateWithOwner(
			const FProfile& Profile,
			const FJsonObject& ProviderExtension,
			const TSharedPtr<FJsonValue>& Patch,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (Profile.TypeId == TEXT("ml_deformer")) {
				if (Patch && Patch->Type == EJson::Object) return true;
				OutError = InputFailure(Profile, Patch, TEXT("ml_deformer_settings_object_required"), TEXT("an object"),
					TEXT("ML deformer settings must be an object."), TEXT("profile_state_preflight"));
				UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, ProviderExtension, Profile.ProfileStateField);
				return false;
			}
			if (Profile.TypeId == TEXT("pcg_data_asset")) {
				if (Patch && Patch->Type == EJson::Object) return true;
				OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("pcg_data_object_required"),
					Profile.ProfileStateField, MakeShared<FJsonValueString>(TEXT("an object")), Patch,
					TEXT("profile_state_preflight"), TEXT("PCG data asset state must be an object."));
				OutError.Source.InternalJsonPath = TEXT("/") + Profile.ProfileStateField.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
				UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, ProviderExtension, Profile.ProfileStateField);
				return false;
			}
			if (IsFunctionInstance(Profile)) {
				if (Patch && Patch->Type == EJson::Array) return true;
				OutError = InputFailure(Profile, Patch, TEXT("material_function_instance_parameters_array_required"), TEXT("an array"),
					TEXT("Material function instance parameters must be an array."), TEXT("profile_state_preflight"));
				UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, ProviderExtension, Profile.ProfileStateField);
				return false;
			}
			if (Profile.TypeId == TEXT("material_parameter_collection")) {
				if (Patch && Patch->Type == EJson::Object) return true;
				OutError = InputFailure(Profile, Patch, TEXT("material_collection_parameters_object_required"), TEXT("an object"),
					TEXT("Material collection parameters must be an object."), TEXT("profile_state_preflight"));
				UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, ProviderExtension, Profile.ProfileStateField);
				return false;
			}
			if (Profile.TypeId == TEXT("physics_asset")) return PhysicsAssetState::Validate(Patch, OutError);
			if (Profile.TypeId == TEXT("paper_sprite")) {
				if (Patch && Patch->Type == EJson::Object) return true;
				OutError = InputFailure(Profile, Patch, TEXT("paper_sprite_collision_object_required"), TEXT("an object"),
					TEXT("Paper sprite collision state must be an object."), TEXT("profile_state_preflight"));
				UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, ProviderExtension, Profile.ProfileStateField);
				return false;
			}
			if (Profile.TypeId == TEXT("static_mesh")) return StaticMeshState::Validate(Patch, OutError);
			if (Profile.TypeId == TEXT("skeletal_mesh")) return SkeletalMeshState::Validate(Patch, OutError);
			if (Profile.TypeId == TEXT("sound_class") || Profile.TypeId == TEXT("sound_submix"))
			{
				const bool bValid = Profile.TypeId == TEXT("sound_class")
					? SoundClassRelations::Validate(Patch, OutError) : SoundSubmixRelations::Validate(Patch, OutError);
				if (!bValid) UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, ProviderExtension, Profile.ProfileStateField);
				return bValid;
			}
			if (const auto* Contract = UeAgentCurveJson::FindAssetCurveContract(Profile.AssetClassPath))
			{
				if (!Patch.IsValid() || Patch->Type != EJson::Object)
				{
					OutError = InputFailure(Profile, Patch, TEXT("curve_json_parse_failed"), TEXT("an object"),
						TEXT("Curve profile state must be an object."), TEXT("profile_state_preflight"));
					UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, ProviderExtension, Profile.ProfileStateField);
					return false;
				}
				TArray<UE::UAI::Contract::FDiagnostic> Issues;
				if (!UeAgentCurveJson::ValidateCurveAssetJson(*Contract, Patch->AsObject(), FString(), Issues))
				{
					OutError = UeAgentAuthoringDiagnostics::MakeIssuesFailure(
						TEXT("curve_profile_state_invalid"), Issues, TEXT("profile_state_preflight"));
					UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Patch->AsObject());
					return false;
				}
				return true;
			}
			if (ProfileStateAI::Supports(Profile))
			{
				return ProfileStateAI::Validate(Profile, Patch, OutError);
			}
			if (ProfileStateSequenceStateTree::Supports(Profile))
			{
				return ProfileStateSequenceStateTree::Validate(Profile, Patch, OutError);
			}
			OutError = SurfaceFailure(Profile);
			return false;
		}
	}

	bool InitializeUpdatePreview(UObject& Source, UObject& Preview, const FString& SemanticAssetPath,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		ON_SCOPE_EXIT { BindTarget(OutError, Preview.GetPathName(), SemanticAssetPath); };
		if (auto* SourceMesh = Cast<UStaticMesh>(&Source))
		{
			auto* PreviewMesh = Cast<UStaticMesh>(&Preview);
			if (!PreviewMesh) { OutError = TargetFailure(TEXT("static_mesh_preview_type_mismatch"), Preview, *UStaticMesh::StaticClass()); return false; }
			FStaticMeshCompilingManager::Get().FinishCompilation({SourceMesh, PreviewMesh});
			// A loaded scratch copy runs PostLoad's material-name repair. An update
			// preview must start from the resident baseline, not a newly loaded asset.
			PreviewMesh->SetStaticMaterials(SourceMesh->GetStaticMaterials());
			PreviewMesh->GetSectionInfoMap() = SourceMesh->GetSectionInfoMap();
			PreviewMesh->GetOriginalSectionInfoMap() = SourceMesh->GetOriginalSectionInfoMap();
		}
		if (auto* SourceMesh = Cast<USkeletalMesh>(&Source))
		{
			auto* PreviewMesh = Cast<USkeletalMesh>(&Preview);
			if (!PreviewMesh) { OutError = TargetFailure(TEXT("skeletal_mesh_preview_type_mismatch"), Preview, *USkeletalMesh::StaticClass()); return false; }
			FSkinnedAssetCompilingManager::Get().FinishCompilation({SourceMesh, PreviewMesh});
			PreviewMesh->SetMaterials(SourceMesh->GetMaterials());
		}
		if (auto* SourceTree = Cast<UStateTree>(&Source))
		{
			auto* PreviewTree = Cast<UStateTree>(&Preview);
			if (!PreviewTree) { OutError = TargetFailure(TEXT("state_tree_preview_type_mismatch"), Preview, *UStateTree::StaticClass()); return false; }
			return UeAgentStateTreeAuthoring::InitializeTreeAuthoringPreview(*SourceTree, *PreviewTree, OutError);
		}
		return true;
	}

	bool HasSurface(const FProfile& Profile)
	{
		return !Profile.ProfileStateSemanticId.IsEmpty()
			&& !Profile.ProfileStateField.IsEmpty()
			&& (ProfileStateAI::Supports(Profile)
				|| IsFunctionInstance(Profile)
				|| Profile.TypeId == TEXT("material_parameter_collection")
				|| Profile.TypeId == TEXT("pcg_data_asset")
				|| Profile.TypeId == TEXT("ml_deformer")
				|| Profile.TypeId == TEXT("physics_asset")
				|| Profile.TypeId == TEXT("static_mesh")
				|| Profile.TypeId == TEXT("paper_sprite")
				|| Profile.TypeId == TEXT("skeletal_mesh")
				|| Profile.TypeId == TEXT("sound_class")
				|| Profile.TypeId == TEXT("sound_submix")
				|| ProfileStateSequenceStateTree::Supports(Profile)
				|| UeAgentCurveJson::FindAssetCurveContract(Profile.AssetClassPath));
	}

	bool PreparePatch(
		const FProfile& Profile,
		const TSharedPtr<const FJsonObject>& ProviderExtension,
		TSharedPtr<FJsonValue>& OutPatch,
		bool& bOutHasPatch,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutPatch.Reset();
		bOutHasPatch = false;
		OutError.Reset();
		if (!HasSurface(Profile) || !ProviderExtension.IsValid())
		{
			return true;
		}
		const TSharedPtr<FJsonValue>* Found = ProviderExtension->Values.Find(Profile.ProfileStateField);
		if (!Found)
		{
			return true;
		}
		if (!Found->IsValid())
		{
			OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("reflection_profile_state_value_invalid"),
				Profile.ProfileStateField, MakeShared<FJsonValueString>(TEXT("a valid JSON value")), *Found,
				TEXT("profile_state_preflight"), TEXT("The supplied profile state field has no JSON value."));
			OutError.Source.InternalJsonPath = TEXT("/") + Profile.ProfileStateField.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
			UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *ProviderExtension, Profile.ProfileStateField);
			return false;
		}
		OutPatch = *Found;
		if (!ValidateWithOwner(Profile, *ProviderExtension, OutPatch, OutError))
		{
			// A delegated root-value failure has no object identity of its own.
			if (OutError.SourceKind == TEXT("authored_value") && OutError.Source.InternalJsonPath == TEXT("/"))
				UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *ProviderExtension, Profile.ProfileStateField);
			OutPatch.Reset();
			return false;
		}
		bOutHasPatch = true;
		return true;
	}

	bool Read(
		const FProfile& Profile,
		UObject& Asset,
		const FString& SemanticAssetPath,
		TSharedPtr<FJsonValue>& OutValue,
		bool& bOutAuthoringVisible,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		ON_SCOPE_EXIT { BindTarget(OutError, Asset.GetPathName(), SemanticAssetPath); };
        if (Profile.TypeId == TEXT("physics_asset")) {
            bOutAuthoringVisible = true;
            auto* Physics = Cast<UPhysicsAsset>(&Asset);
            if (!Physics) { OutError = TargetFailure(TEXT("physics_asset_target_invalid"), Asset, *UPhysicsAsset::StaticClass()); return false; }
            return PhysicsAssetState::Read(*Physics, OutValue, OutError);
        }
		OutValue.Reset();
		bOutAuthoringVisible = true;
		OutError.Reset();
		if (Profile.TypeId == TEXT("ml_deformer"))
			return UeAgentMlDeformerAuthoring::Read(Asset, OutValue, OutError);
		if (IsFunctionInstance(Profile)) {
			auto* Instance = Cast<UMaterialFunctionInstance>(&Asset);
			if (!Instance) { OutError = TargetFailure(TEXT("material_function_instance_target_invalid"), Asset, *UMaterialFunctionInstance::StaticClass()); return false; }
			return UeAgentMaterialAuthoring::FunctionInstance::Read(*Instance, OutValue, OutError);
		}
		if (Profile.TypeId == TEXT("pcg_data_asset")) {
			auto* Data = Cast<UPCGDataAsset>(&Asset);
			if (!Data) {
			    OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("pcg_data_target_invalid"),
			        TEXT("The PCG data asset profile requires a PCGDataAsset target."), TEXT("profile_state"));
			    OutError.SourceKind = TEXT("native_result");
			    OutError.TargetPath = SemanticAssetPath;
			    OutError.PropertyPath = TEXT("target_class");
			    OutError.ActualValue = MakeShared<FJsonValueString>(Asset.GetClass()->GetPathName());
			    OutError.ExpectedValue = MakeShared<FJsonValueString>(UPCGDataAsset::StaticClass()->GetPathName());
			    return false;
			}
			return UeAgentPcgDataAssetAuthoring::Read(*Data, OutValue, OutError, SemanticAssetPath);
		}
		if (Profile.TypeId == TEXT("material_parameter_collection")) {
			auto* Collection = Cast<UMaterialParameterCollection>(&Asset);
			if (!Collection) { OutError = TargetFailure(TEXT("material_collection_target_invalid"), Asset, *UMaterialParameterCollection::StaticClass()); return false; }
			return UeAgentMaterialAuthoring::Collection::Read(*Collection, OutValue, OutError);
		}
		if (Profile.TypeId == TEXT("sound_submix"))
		{
			const auto* Submix = Cast<USoundSubmixBase>(&Asset);
			if (!Submix) { OutError = TargetFailure(TEXT("sound_submix_target_invalid"), Asset, *USoundSubmixBase::StaticClass()); return false; }
			OutValue = SoundSubmixRelations::Read(*Submix);
			return true;
		}
		if (Profile.TypeId == TEXT("static_mesh"))
		{
			auto* Mesh = Cast<UStaticMesh>(&Asset);
			if (!Mesh) { OutError = TargetFailure(TEXT("static_mesh_target_invalid"), Asset, *UStaticMesh::StaticClass()); return false; }
			return StaticMeshState::Read(*Mesh, SemanticAssetPath, OutValue, OutError);
		}
		if (Profile.TypeId == TEXT("paper_sprite"))
		{
			auto* Sprite = Cast<UPaperSprite>(&Asset);
			if (!Sprite) { OutError = TargetFailure(TEXT("paper_sprite_target_invalid"), Asset, *UPaperSprite::StaticClass()); return false; }
			return PaperSpriteState::ReadCollision(*Sprite, OutValue, OutError);
		}
		if (Profile.TypeId == TEXT("skeletal_mesh"))
		{
			auto* Mesh = Cast<USkeletalMesh>(&Asset);
			if (!Mesh) { OutError = TargetFailure(TEXT("skeletal_mesh_target_invalid"), Asset, *USkeletalMesh::StaticClass()); return false; }
			return SkeletalMeshState::Read(*Mesh, SemanticAssetPath, OutValue, OutError);
		}
		if (Profile.TypeId == TEXT("sound_class"))
		{
			const auto* SoundClass = Cast<USoundClass>(&Asset);
			if (!SoundClass) { OutError = TargetFailure(TEXT("sound_class_target_invalid"), Asset, *USoundClass::StaticClass()); return false; }
			OutValue = SoundClassRelations::Read(*SoundClass);
			return true;
		}
		if (const auto* Contract = UeAgentCurveJson::FindAssetCurveContract(Profile.AssetClassPath); Contract && Asset.IsA(Contract->AssetClass))
		{
			OutValue = MakeShared<FJsonValueObject>(UeAgentCurveJson::BuildCurveAssetJson(&Asset, true));
			return true;
		}
		if (ProfileStateAI::Supports(Profile))
		{
			return ProfileStateAI::Read(Profile, Asset, OutValue, OutError);
		}
		if (ProfileStateSequenceStateTree::Supports(Profile))
		{
			return ProfileStateSequenceStateTree::Read(
				Profile, Asset, SemanticAssetPath, OutValue,
				bOutAuthoringVisible, OutError);
		}
		OutError = SurfaceFailure(Profile);
		return false;
	}

	bool Apply(
		const FProfile& Profile,
		UObject& Asset,
		const TSharedPtr<FJsonValue>& Patch,
		const FString& SemanticAssetPath,
		UE::UAI::Contract::FDiagnostic& OutError,
		const bool bPreserveExistingStructure, UPackage* TextIdentityPackage,
		UE::UAI::Authoring::Timeline::FMovieSceneCreationState* MovieSceneCreation,
		FIntentVerifier* OutIntent)
	{
		OutError.Reset();
		if (OutIntent) *OutIntent = nullptr;
		ON_SCOPE_EXIT
		{
			BindTarget(OutError, Asset.GetPathName(), SemanticAssetPath);
			if (OutIntent && *OutIntent)
			{
				auto Intent = MoveTemp(*OutIntent);
				*OutIntent = [Intent = MoveTemp(Intent), SemanticAssetPath](UObject& Current, FDiagnostic& Error)
				{
					const bool bOk = Intent(Current, Error);
					BindTarget(Error, Current.GetPathName(), SemanticAssetPath);
					return bOk;
				};
			}
		};
        if (Profile.TypeId == TEXT("ml_deformer"))
            return UeAgentMlDeformerAuthoring::Apply(Asset, Patch, OutError);
        if (IsFunctionInstance(Profile)) {
            auto* Instance = Cast<UMaterialFunctionInstance>(&Asset);
            if (!Instance) { OutError = TargetFailure(TEXT("material_function_instance_target_invalid"), Asset, *UMaterialFunctionInstance::StaticClass()); return false; }
            auto Intent = MakeShared<TArray<UeAgentMaterialAuthoring::FInstanceParameterEdit>>();
            if (!UeAgentMaterialAuthoring::FunctionInstance::Apply(*Instance, Patch, OutError, &Intent.Get())) return false;
            if (OutIntent) *OutIntent = [Intent](UObject& Current, UE::UAI::Contract::FDiagnostic& Error) {
                auto* Owner = Cast<UMaterialFunctionInstance>(&Current);
                if (!Owner) { Error = TargetFailure(TEXT("material_function_instance_target_invalid"), Current, *UMaterialFunctionInstance::StaticClass()); return false; }
                return UeAgentMaterialAuthoring::FunctionInstance::VerifyParameters(*Owner, *Intent, Error);
            };
            return true;
        }
        if (Profile.TypeId == TEXT("pcg_data_asset")) {
            auto* Data = Cast<UPCGDataAsset>(&Asset);
            if (!Data) {
                OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("pcg_data_target_invalid"),
                    TEXT("The PCG data asset profile requires a PCGDataAsset target."), TEXT("profile_state"));
                OutError.SourceKind = TEXT("native_result");
                OutError.TargetPath = SemanticAssetPath;
                OutError.PropertyPath = TEXT("target_class");
                OutError.ActualValue = MakeShared<FJsonValueString>(Asset.GetClass()->GetPathName());
                OutError.ExpectedValue = MakeShared<FJsonValueString>(UPCGDataAsset::StaticClass()->GetPathName());
                return false;
            }
            return UeAgentPcgDataAssetAuthoring::Apply(*Data, Patch, OutError, SemanticAssetPath);
        }
        if (Profile.TypeId == TEXT("material_parameter_collection")) {
            auto* Collection = Cast<UMaterialParameterCollection>(&Asset);
            if (!Collection) { OutError = TargetFailure(TEXT("material_collection_target_invalid"), Asset, *UMaterialParameterCollection::StaticClass()); return false; }
            TSharedPtr<const UeAgentMaterialAuthoring::Collection::FIntent> Intent;
            if (!UeAgentMaterialAuthoring::Collection::Apply(*Collection, Patch, OutError, &Intent)) return false;
            if (OutIntent) *OutIntent = [Intent](UObject& Current, UE::UAI::Contract::FDiagnostic& Error) {
                auto* Owner = Cast<UMaterialParameterCollection>(&Current);
                if (!Owner) { Error = TargetFailure(TEXT("material_collection_target_invalid"), Current, *UMaterialParameterCollection::StaticClass()); return false; }
                return Intent->Verify(*Owner, Error);
            };
            return true;
        }
        if (Profile.TypeId == TEXT("physics_asset")) {
            auto* Physics = Cast<UPhysicsAsset>(&Asset);
            if (!Physics) { OutError = TargetFailure(TEXT("physics_asset_target_invalid"), Asset, *UPhysicsAsset::StaticClass()); return false; }
            return PhysicsAssetState::Apply(*Physics, Patch, OutError);
        }
		if (Profile.TypeId == TEXT("static_mesh"))
		{
			auto* Mesh = Cast<UStaticMesh>(&Asset);
			if (!Mesh) { OutError = TargetFailure(TEXT("static_mesh_target_invalid"), Asset, *UStaticMesh::StaticClass()); return false; }
			return StaticMeshState::Apply(*Mesh, Patch, OutError);
		}
		if (Profile.TypeId == TEXT("paper_sprite"))
		{
			auto* Sprite = Cast<UPaperSprite>(&Asset);
			if (!Sprite) { OutError = TargetFailure(TEXT("paper_sprite_target_invalid"), Asset, *UPaperSprite::StaticClass()); return false; }
			return PaperSpriteState::ApplyCollision(*Sprite, Patch, OutError);
		}
		if (Profile.TypeId == TEXT("skeletal_mesh"))
		{
			auto* Mesh = Cast<USkeletalMesh>(&Asset);
			if (!Mesh) { OutError = TargetFailure(TEXT("skeletal_mesh_target_invalid"), Asset, *USkeletalMesh::StaticClass()); return false; }
			return SkeletalMeshState::Apply(*Mesh, Patch, OutError);
		}
		if (const auto* Contract = UeAgentCurveJson::FindAssetCurveContract(Profile.AssetClassPath); Contract && Asset.IsA(Contract->AssetClass))
		{
			if (!Patch.IsValid() || Patch->Type != EJson::Object)
			{
				OutError = InputFailure(Profile, Patch, TEXT("curve_json_parse_failed"), TEXT("an object"),
					TEXT("Curve profile state must be an object."), TEXT("profile_state_apply"));
				return false;
			}
			TArray<UE::UAI::Contract::FDiagnostic> Issues;
			if (!UeAgentCurveJson::ApplyCurveAssetJson(&Asset, Patch->AsObject(), FString(), Issues))
			{
				OutError = UeAgentAuthoringDiagnostics::MakeIssuesFailure(
					TEXT("curve_profile_state_invalid"), Issues, TEXT("profile_state_apply"), SemanticAssetPath);
				UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Patch->AsObject());
				return false;
			}
			return true;
		}
		if (ProfileStateAI::Supports(Profile))
		{
			return ProfileStateAI::Apply(Profile, Asset, Patch, OutError, OutIntent);
		}
		if (ProfileStateSequenceStateTree::Supports(Profile))
		{
			return ProfileStateSequenceStateTree::Apply(
				Profile, Asset, Patch, SemanticAssetPath, OutError, bPreserveExistingStructure, TextIdentityPackage, MovieSceneCreation);
		}
		OutError = SurfaceFailure(Profile);
		return false;
	}

	bool CompileIfNeeded(
		const FProfile& Profile,
		UObject& Asset,
		const FString& SemanticAssetPath,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		ON_SCOPE_EXIT { BindTarget(OutError, Asset.GetPathName(), SemanticAssetPath); };
		if (Profile.TypeId == TEXT("skeletal_mesh"))
		{
			auto* Mesh = Cast<USkeletalMesh>(&Asset);
			if (!Mesh) { OutError = TargetFailure(TEXT("skeletal_mesh_target_invalid"), Asset, *USkeletalMesh::StaticClass()); return false; }
			SkeletalMeshState::Settle(*Mesh);
			return true;
		}
		if (ProfileStateSequenceStateTree::Supports(Profile))
		{
			return ProfileStateSequenceStateTree::CompileIfNeeded(Profile, Asset, OutError);
		}
		return true;
	}
}

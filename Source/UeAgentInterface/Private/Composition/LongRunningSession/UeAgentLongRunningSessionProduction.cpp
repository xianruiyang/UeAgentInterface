#include "Composition/LongRunningSession/UeAgentLongRunningSessionProduction.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Utilities/EditorResources/UeAgentEditorResourcePersistence.h"

#include "Async/Async.h"
#include "Async/Future.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Core/AssetPreview/UeAgentAssetPreviewState.h"
#include "Core/AssetPreview/UeAgentAssetEditorLifetime.h"
#include "Components/DynamicMeshComponent.h"
#include "Composition/LongRunningSession/UeAgentLongRunningSessionActivation.h"
#include "Composition/LongRunningSession/UeAgentLongRunningSessionCommandHandlers.h"
#include "Composition/LongRunningSession/UeAgentSafeExitDurableCompletion.h"
#include "Containers/Ticker.h"
#include "CoreGlobals.h"
#include "Core/ArtifactPublisher/UeAgentFileArtifactPublisher.h"
#include "Core/ExecutionMutation/UeAgentLongRunningOperation.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Core/EditorWorld/UeAgentEditorWorldActivation.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaImagePolicy.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureImageProcessing.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureImageArtifactService.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaLongJobProduction.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentLevelViewportSequenceCapture.h"
#include "DomainAdapters/CompileBuildJob/UeAgentCompileBuildJobProduction.h"
#include "DomainAdapters/CompileBuildJob/UeAgentBlueprintCompilePolicy.h"
#include "DomainAdapters/EditorSessionOperation/UeAgentEditorSessionOperationProduction.h"
#include "DomainAdapters/PieRuntimeDebug/UeAgentPieRuntimeDebugEditorSession.h"
#include "DomainAdapters/PieRuntimeDebug/UeAgentPieRuntimeDebugRegistry.h"
#include "DomainAdapters/PieRuntimeDebug/UeAgentPieRuntimeScenarioProduction.h"
#include "Domains/PCG/UeAgentPcgGenerationWork.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Editor/UnrealEdEngine.h"
#include "EditorBuildUtils.h"
#include "Engine/Blueprint.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMeshActor.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "HAL/Event.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "ImageUtils.h"
#include "ISubmixBufferListener.h"
#include "Internationalization/Internationalization.h"
#include "Internationalization/Culture.h"
#include "Interfaces/ITargetPlatform.h"
#include "Interfaces/ITargetPlatformManagerModule.h"
#include "LevelEditorViewport.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "LocalizationConfigurationScript.h"
#include "LocalizationSettings.h"
#include "LocalizationTargetTypes.h"
#include "Misc/FileHelper.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/SecureHash.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "PlayInEditorDataTypes.h"
#include "Engine/TextureRenderTarget.h"
#include "Sound/SoundSubmix.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "AudioDeviceManager.h"
#include "AudioMixerDevice.h"
#include "AudioMixerSubmix.h"
#include "AudioThread.h"
#include "Sound/SampleBufferIO.h"
#include "Sound/SoundWave.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"
#include "UDynamicMesh.h"
#include "UnrealEdGlobals.h"
#include "WorldPartition/LoaderAdapter/LoaderAdapterActorList.h"
#include "WorldPartition/LoaderAdapter/LoaderAdapterShape.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionActorLoaderInterface.h"

namespace UE::UAI::Composition::LongRunningSession
{
	using namespace ArtifactPublisher;
	using namespace ContextResource;
	using namespace Contract;
	using namespace DomainAdapters;
	using namespace EditorWorld;
	using namespace ExecutionMutation;
	using namespace StateSemantic;

	namespace
	{
		UWorld* ResolveCurrentEditorWorld(TWeakObjectPtr<UWorld> Fallback)
		{
			return GEditor ? GEditor->GetEditorWorldContext().World() : Fallback.Get();
		}

		bool Fail(FExecutionMutationError& OutError, FExecutionMutationError Diagnostic)
        {
            OutError = MoveTemp(Diagnostic);
            return false;
        }

		const FUeAgentCanonicalValue* FindField(
			const FUeAgentCanonicalValue& Object,
			const FString& Name)
		{
			if (Object.GetKind() != EUeAgentValueKind::Object)
			{
				return nullptr;
			}
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
				: Object.GetObjectItems())
			{
				if (Item.Key == Name)
				{
					return &Item.Value.Get();
				}
			}
			return nullptr;
		}

		FString InputKey(FString Key)
		{
			return Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
		}

		TSharedPtr<FJsonValue> ValueFact(const FUeAgentCanonicalValue* Value)
		{
			if (!Value) return nullptr;
			switch (Value->GetKind())
			{
			case EUeAgentValueKind::Null: return MakeShared<FJsonValueNull>();
			case EUeAgentValueKind::Boolean: return MakeShared<FJsonValueBoolean>(Value->GetBoolean());
			case EUeAgentValueKind::String: return MakeShared<FJsonValueString>(Value->GetText());
			case EUeAgentValueKind::Integer:
			case EUeAgentValueKind::Decimal: return MakeShared<FJsonValueNumberString>(Value->GetText());
			default:
			{
				const auto Summary = MakeShared<FJsonObject>();
				const bool bArray = Value->GetKind() == EUeAgentValueKind::Array;
				const bool bObject = Value->GetKind() == EUeAgentValueKind::Object;
				Summary->SetStringField(TEXT("type"), bArray ? TEXT("array") : bObject ? TEXT("object")
					: Value->GetKind() == EUeAgentValueKind::Temporal ? TEXT("temporal") : TEXT("spatial"));
				if (bArray) Summary->SetNumberField(TEXT("count"), Value->GetArrayItems().Num());
				if (bObject) Summary->SetNumberField(TEXT("count"), Value->GetObjectItems().Num());
				return MakeShared<FJsonValueObject>(Summary);
			}
			}
		}

		FDiagnostic FactFailure(const FString& Code, const FString& Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
			const FString& SourceKind = TEXT("command_input"), const FString& Target = FString())
		{
			FDiagnostic Error = FDiagnostic::Failure(Code, Reason, TEXT("execution"));
			Error.SourceKind = SourceKind;
			Error.PropertyPath = Path;
			if (SourceKind == TEXT("command_input")) Error.Source.JsonPath = Path;
			Error.TargetPath = Target;
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			return Error;
		}

		FDiagnostic InputFailure(const FString& Code, const FString& Reason, const FString& Path,
			const FUeAgentCanonicalValue* Value, const FString& Expected, const bool bInternal = false)
		{
			FDiagnostic Error = FactFailure(Code, Reason, Path, bInternal ? nullptr : ValueFact(Value),
				MakeShared<FJsonValueString>(Expected));
			Error.ActualValuePresent = Value != nullptr;
			Error.ExpectedValuePresent = true;
			if (bInternal) Error.ActualEvidence = ValueFact(Value);
			return Error;
		}

		TSharedPtr<FJsonValue> StringValues(const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> JsonValues;
			for (const FString& Value : Values) JsonValues.Add(MakeShared<FJsonValueString>(Value));
			return MakeShared<FJsonValueArray>(JsonValues);
		}

		FDiagnostic NativeFailure(const FString& Code, const FString& Reason, const FString& Property,
			const FString& Target, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
			const TCHAR* Stage = TEXT("execution"))
		{
			auto Error = FactFailure(Code, Reason, Property, MoveTemp(Actual), MoveTemp(Expected), TEXT("native_result"), Target);
			Error.Stage = Stage;
			return Error;
		}

		FDiagnostic FileFailure(const FString& Code, const FString& Reason, const FString& Operation,
			const FString& PhysicalPath, const FString& Target = FString())
		{
			FDiagnostic Error = NativeFailure(Code, Reason, Operation, Target,
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			Error.ActualEvidence = MakeShared<FJsonValueString>(PhysicalPath);
			return Error;
		}

		TSharedPtr<FJsonValue> FieldNames(const FUeAgentCanonicalValue& Value)
		{
			TArray<FString> Names;
			for (const auto& Item : Value.GetObjectItems()) Names.Add(Item.Key);
			return StringValues(Names);
		}

		TSharedPtr<FJsonValue> FieldNames(const FJsonObject& Value)
		{
			TArray<FString> Names;
			Value.Values.GetKeys(Names);
			Names.Sort();
			return StringValues(Names);
		}

		FString StringField(
			const FUeAgentCanonicalValue& Object,
			const FString& Name,
			const FString& Default = FString())
		{
			const FUeAgentCanonicalValue* Value = FindField(Object, Name);
			return Value && Value->GetKind() == EUeAgentValueKind::String
				? Value->GetText()
				: Default;
		}

		bool BooleanField(
			const FUeAgentCanonicalValue& Object,
			const FString& Name,
			const bool Default = false)
		{
			const FUeAgentCanonicalValue* Value = FindField(Object, Name);
			return Value && Value->GetKind() == EUeAgentValueKind::Boolean
				? Value->GetBoolean()
				: Default;
		}

		bool TryStringArrayField(
			const FUeAgentCanonicalValue& Object,
			const FString& Name,
			TArray<FString>& OutValues,
			FExecutionMutationError& OutError)
		{
			OutValues.Reset();
			const FUeAgentCanonicalValue* Value = FindField(Object, Name);
			if (!Value)
			{
				return true;
			}
			if (Value->GetKind() != EUeAgentValueKind::Array)
			{
				return Fail(OutError, InputFailure(TEXT("editor_safe_exit_dirty_resolution_invalid"),
					TEXT("The dirty-resolution resource list must be an array."), TEXT("/dirty_resolution/") + InputKey(Name), Value, TEXT("array of resource refs")));
			}
			TSet<FString> UniqueValues;
			const auto& Items = Value->GetArrayItems();
			for (int32 Index = 0; Index < Items.Num(); ++Index)
			{
				const FUeAgentCanonicalValueRef& Item = Items[Index];
				const FString Path = TEXT("/dirty_resolution/") + InputKey(Name) + TEXT("/") + FString::FromInt(Index);
				if (Item->GetKind() != EUeAgentValueKind::String)
				{
					return Fail(OutError, InputFailure(TEXT("editor_safe_exit_dirty_resolution_invalid"),
						TEXT("A resource reference must be a string."), Path, &Item.Get(), TEXT("string")));
				}
				const FString& ResourceRef = Item->GetText();
				FString Normalized = ResourceRef;
				Normalized.TrimStartAndEndInline();
				if (Normalized.IsEmpty() || Normalized != ResourceRef)
				{
					return Fail(OutError, InputFailure(TEXT("editor_safe_exit_dirty_resolution_invalid"),
						TEXT("A resource reference must be non-empty and have no surrounding whitespace."), Path, &Item.Get(), TEXT("trimmed non-empty resource ref")));
				}
				FString CanonicalRef;
				if (ResourceRef.StartsWith(TEXT("config:")))
				{
					if (!EditorResources::ConfigFilenameForResourceRef(ResourceRef))
					{
						return Fail(OutError, InputFailure(TEXT("editor_safe_exit_dirty_resource_ref_invalid"),
							TEXT("The configuration resource reference is not registered."), Path, &Item.Get(), TEXT("registered config resource ref")));
					}
					CanonicalRef = ResourceRef;
				}
				else
				{
					CanonicalRef = EditorResources::NormalizeResourceRef(ResourceRef);
					if (!FPackageName::IsValidLongPackageName(CanonicalRef, false))
					{
						return Fail(OutError, InputFailure(TEXT("editor_safe_exit_dirty_resource_ref_invalid"),
							TEXT("The resource reference does not resolve to a valid long package name."), Path, &Item.Get(), TEXT("valid package resource ref")));
					}
				}
				if (UniqueValues.Contains(CanonicalRef))
				{
					return Fail(OutError, FactFailure(TEXT("editor_safe_exit_dirty_resource_ref_duplicate"),
						TEXT("More than one resource reference resolves to the same canonical resource."), Path,
						MakeShared<FJsonValueString>(CanonicalRef), MakeShared<FJsonValueString>(TEXT("unique canonical resource ref"))));
				}
				UniqueValues.Add(CanonicalRef);
				OutValues.Add(ResourceRef);
			}
			OutValues.Sort();
			return true;
		}

		bool TryCanonicalObjectJson(
			const FUeAgentCanonicalValue& Value,
			TSharedPtr<FJsonObject>& OutObject,
			FExecutionMutationError& OutError)
		{
			OutObject.Reset();
			TSharedPtr<FJsonValue> JsonValue;
			UE::UAI::Contract::FDiagnostic Error;
			if (!CanonicalJson::TryToJsonValue(Value, JsonValue, Error))
				return Fail(OutError, MoveTemp(Error));
			if (JsonValue->Type != EJson::Object)
				return Fail(OutError, InputFailure(TEXT("long_running_session_parameters_object_required"),
					TEXT("The policy parameters must project to a JSON object."), TEXT("/parameters"), &Value, TEXT("object")));
			OutObject = JsonValue->AsObject();
			return true;
		}

		const FString* ConfigFilenameForResourceRef(const FString& ResourceRef)
		{
			return EditorResources::ConfigFilenameForResourceRef(ResourceRef);
		}

		void CollectDirtySafeExitResourceRefs(
			TArray<UPackage*>& OutPackages,
			TArray<FString>& OutResourceRefs)
		{
			OutPackages.Reset();
			OutResourceRefs.Reset();
			TArray<EditorResources::FDirtyResource> Resources;
			EditorResources::CollectDirtyResources(Resources);
			for (const auto& Resource : Resources)
			{
				OutResourceRefs.Add(Resource.ResourceRef);
				if (Resource.Package) OutPackages.AddUnique(Resource.Package);
			}
		}

		TArray<FString> CanonicalResourceRefs(const TArray<FString>& ResourceRefs)
		{
			TArray<FString> Result;
			Result.Reserve(ResourceRefs.Num());
			for (const FString& ResourceRef : ResourceRefs)
			{
				Result.Add(EditorResources::NormalizeResourceRef(ResourceRef));
			}
			Result.Sort();
			return Result;
		}

		void DiscardConfigResources(const TArray<FString>& ResourceRefs)
		{
			if (!GConfig)
			{
				return;
			}
			for (const FString& ResourceRef : ResourceRefs)
			{
				const FString* Filename =
					ConfigFilenameForResourceRef(ResourceRef);
				FConfigFile* Config = Filename
					? GConfig->FindConfigFile(**Filename)
					: nullptr;
				if (Config)
				{
					Config->Dirty = false;
				}
			}
		}

		bool ExactResourceSetMatches(
			const TArray<FString>& Requested,
			const TArray<FString>& Dirty)
		{
			return CanonicalResourceRefs(Requested) == Dirty;
		}

		bool TryReadSafeExitExternalWitnessRef(
			const FUeAgentCanonicalValue& Parameters,
			FUeAgentSafeExitExternalWitnessRef& OutRef,
			FExecutionMutationError& OutError)
		{
			OutRef = {};
			const FUeAgentCanonicalValue* Witness =
				FindField(Parameters, TEXT("external_witness"));
			if (!Witness || Witness->GetKind() != EUeAgentValueKind::Object)
				return Fail(OutError, InputFailure(TEXT("editor_safe_exit_external_witness_required"),
					TEXT("An external witness reference object is required for safe exit."),
					TEXT("/external_witness"), Witness, TEXT("external witness object"), true));
			if (StringField(*Witness, TEXT("schema")) != TEXT("uai.safe_exit.external_witness_ref.v1"))
				return Fail(OutError, InputFailure(TEXT("editor_safe_exit_external_witness_required"),
					TEXT("The external witness schema is not supported."), TEXT("/external_witness/schema"),
					FindField(*Witness, TEXT("schema")), TEXT("uai.safe_exit.external_witness_ref.v1")));
			OutRef.LaunchId = StringField(*Witness, TEXT("launch_id"));
			OutRef.ExitAttemptId = StringField(
				*Witness, TEXT("exit_attempt_id"));
			OutRef.Nonce = StringField(*Witness, TEXT("nonce"));
			if (!OutRef.TryValidate(OutError))
			{
				OutError.SourceKind = TEXT("command_input");
				return false;
			}
			return true;
		}

		bool TryApplySafeExitDirtyResolution(
			const FString& Mode,
			TArray<FString> SaveResources,
			TArray<FString> DiscardResources,
			FUeAgentSafeExitDirtyResolutionReceipt& OutReceipt,
			FExecutionMutationError& OutError)
		{
			OutReceipt = {};
			TArray<UPackage*> DirtyPackages;
			TArray<FString> DirtyResourceRefs;
			CollectDirtySafeExitResourceRefs(
				DirtyPackages, DirtyResourceRefs);

			if (Mode == TEXT("reject"))
			{
				if (!SaveResources.IsEmpty() || !DiscardResources.IsEmpty())
				{
					return Fail(OutError, FactFailure(TEXT("editor_safe_exit_dirty_resolution_invalid"),
						TEXT("Reject mode does not accept save or discard resources."), TEXT("/dirty_resolution"),
						MakeShared<FJsonValueNumber>(SaveResources.Num() + DiscardResources.Num()), MakeShared<FJsonValueNumber>(0)));
				}
				if (DirtyResourceRefs.IsEmpty())
				{
					OutReceipt.Mode = Mode;
					OutReceipt.SaveResources = SaveResources;
					OutReceipt.DiscardResources = DiscardResources;
					return true;
				}
				return Fail(OutError, NativeFailure(TEXT("editor_safe_exit_dirty_resources_present"),
					TEXT("Safe exit was rejected because resources are still dirty."), TEXT("/dirty_resources"), FString(),
					StringValues(DirtyResourceRefs), StringValues({})));
			}

			const TArray<FString>& RequestedResources =
				Mode == TEXT("save") ? SaveResources : DiscardResources;
			const TArray<FString>& ForbiddenResources =
				Mode == TEXT("save") ? DiscardResources : SaveResources;
			if (!ForbiddenResources.IsEmpty())
			{
				return Fail(OutError, FactFailure(TEXT("editor_safe_exit_dirty_resolution_invalid"),
					TEXT("The selected resolution mode does not accept the opposite resource list."),
					Mode == TEXT("save") ? TEXT("/dirty_resolution/discard_resources") : TEXT("/dirty_resolution/save_resources"),
					StringValues(ForbiddenResources), StringValues({})));
			}
			if (!ExactResourceSetMatches(RequestedResources, DirtyResourceRefs))
			{
				return Fail(OutError, FactFailure(TEXT("editor_safe_exit_dirty_resource_set_mismatch"),
					TEXT("The requested resources do not exactly match the dirty resource set."),
					Mode == TEXT("save") ? TEXT("/dirty_resolution/save_resources") : TEXT("/dirty_resolution/discard_resources"),
					StringValues(CanonicalResourceRefs(RequestedResources)), StringValues(DirtyResourceRefs)));
			}

			if (Mode == TEXT("save"))
			{
				FDiagnostic Failures;
				for (const FString& Ref : RequestedResources)
				{
					const auto Saved = EditorResources::SaveResource(Ref);
					if (Saved.Status == TEXT("failed"))
						{
						auto Diagnostic = Saved.Diagnostic;
						if (Diagnostic.TargetPath.IsEmpty()) Diagnostic.TargetPath = Saved.ResourceRef;
						Failures.Append(Diagnostic);
					}
				}
				if (!Failures.IsEmpty())
					return Fail(OutError, UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("editor_safe_exit_save_failed"), Failures, TEXT("execution"), TEXT("One or more dirty resources could not be saved.")));
			}
			if (Mode == TEXT("discard"))
			{
				for (UPackage* Package : DirtyPackages)
				{
					Package->SetDirtyFlag(false);
				}
				DiscardConfigResources(RequestedResources);
			}

			CollectDirtySafeExitResourceRefs(
				DirtyPackages, DirtyResourceRefs);
			if (!DirtyResourceRefs.IsEmpty())
			{
				return Fail(OutError, NativeFailure(TEXT("editor_safe_exit_dirty_resources_unresolved"),
					TEXT("Resources remain dirty after applying the requested resolution."), TEXT("/dirty_resources"), FString(),
					StringValues(DirtyResourceRefs), StringValues({})));
			}
			OutReceipt.Mode = Mode;
			OutReceipt.SaveResources = MoveTemp(SaveResources);
			OutReceipt.DiscardResources = MoveTemp(DiscardResources);
			OutReceipt.RemainingDirtyResources = MoveTemp(DirtyResourceRefs);
			return true;
		}

		bool TryResolveSafeExitDirtyResources(
			const FUeAgentCanonicalValue& Parameters,
			FUeAgentSafeExitDirtyResolutionReceipt& OutReceipt,
			FExecutionMutationError& OutError)
		{
			OutReceipt = {};
			const FUeAgentCanonicalValue* DirtyResolution =
				FindField(Parameters, TEXT("dirty_resolution"));
			if (!DirtyResolution
				|| DirtyResolution->GetKind() != EUeAgentValueKind::Object)
			{
				return Fail(OutError, InputFailure(TEXT("editor_safe_exit_dirty_resolution_invalid"),
					TEXT("The dirty-resolution object is required."), TEXT("/dirty_resolution"), DirtyResolution, TEXT("object")));
			}
			const FString Mode = StringField(*DirtyResolution, TEXT("mode"));
			TArray<FString> SaveResources;
			TArray<FString> DiscardResources;
			if (Mode != TEXT("reject") && Mode != TEXT("save") && Mode != TEXT("discard"))
				return Fail(OutError, InputFailure(TEXT("editor_safe_exit_dirty_resolution_invalid"),
					TEXT("The dirty-resolution mode is not supported."), TEXT("/dirty_resolution/mode"),
					FindField(*DirtyResolution, TEXT("mode")), TEXT("reject, save or discard")));
			if (!TryStringArrayField(*DirtyResolution, TEXT("save_resources"), SaveResources, OutError)
				|| !TryStringArrayField(*DirtyResolution, TEXT("discard_resources"), DiscardResources, OutError)) return false;

			return TryApplySafeExitDirtyResolution(
				Mode,
				MoveTemp(SaveResources),
				MoveTemp(DiscardResources),
				OutReceipt,
				OutError);
		}

		double DecimalField(
			const FUeAgentCanonicalValue& Object,
			const FString& Name,
			const double Default)
		{
			const FUeAgentCanonicalValue* Value = FindField(Object, Name);
			if (!Value || (Value->GetKind() != EUeAgentValueKind::Integer
				&& Value->GetKind() != EUeAgentValueKind::Decimal))
			{
				return Default;
			}
			double Result = Default;
			return LexTryParseString(Result, *Value->GetLexeme())
				? Result : Default;
		}

		FString StripSubjectPrefix(
			const FString& Subject,
			const FString& Prefix)
		{
			return Subject.StartsWith(Prefix)
				? Subject.RightChop(Prefix.Len())
				: FString();
		}

		FString MakeNativeRoot(const FString& OperationId)
		{
			FString Safe = OperationId;
			Safe.ReplaceInline(TEXT(":"), TEXT("_"));
			Safe.ReplaceInline(TEXT("/"), TEXT("_"));
			return FPaths::ConvertRelativePathToFull(
				FPaths::ProjectSavedDir()
				/ TEXT("UeAgentInterface/LongRunningSession/Native")
				/ Safe);
		}

		bool WritePng(
			const TArray<FColor>& Pixels,
			const FIntPoint& Size,
			const FString& Path,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (Size.X <= 0 || Size.Y <= 0
				|| Pixels.Num() != static_cast<int64>(Size.X) * Size.Y)
			{
				const auto Actual = MakeShared<FJsonObject>();
				Actual->SetNumberField(TEXT("width"), Size.X);
				Actual->SetNumberField(TEXT("height"), Size.Y);
				Actual->SetNumberField(TEXT("pixel_count"), Pixels.Num());
				OutError = NativeFailure(TEXT("capture_image_pixels_invalid"),
					TEXT("Image dimensions must be positive and the pixel count must equal width times height."),
					TEXT("/image"), FString(), MakeShared<FJsonValueObject>(Actual),
					MakeShared<FJsonValueString>(TEXT("positive dimensions and exactly width * height pixels")));
				return false;
			}
			return DomainAdapters::CaptureMediaLongJob::FCaptureImageProcessing::WriteCompressedImage(
				Pixels, Size, TEXT("png"), 100, Path, OutError, false);
		}

		bool TryOnlyFields(
			const FUeAgentCanonicalValue& Object,
			std::initializer_list<const TCHAR*> Allowed, FExecutionMutationError& OutError, const FString& Code)
		{
			if (Object.GetKind() != EUeAgentValueKind::Object)
			{
				return Fail(OutError, InputFailure(Code, TEXT("The parameters must be an object."),
					TEXT("/parameters"), &Object, TEXT("object")));
			}
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
				: Object.GetObjectItems())
			{
				bool bAllowed = false;
				for (const TCHAR* Name : Allowed)
				{
					if (Item.Key == Name)
					{
						bAllowed = true;
						break;
					}
				}
				if (!bAllowed)
				{
					TArray<FString> AllowedNames;
					for (const TCHAR* Name : Allowed) AllowedNames.Add(Name);
					FDiagnostic Error = FactFailure(Code, TEXT("The parameter is not supported by this policy."),
						TEXT("/parameters/") + InputKey(Item.Key), ValueFact(&Item.Value.Get()), StringValues(AllowedNames));
					Error.ActualValuePresent = true;
					Error.ExpectedValuePresent = false;
					return Fail(OutError, MoveTemp(Error));
				}
			}
			return true;
		}

		bool TryExactActorGuids(
			const FUeAgentCanonicalValue& Parameters,
			TArray<FString>& OutGuidTexts,
			FExecutionMutationError& OutError)
		{
			OutGuidTexts.Reset();
			const FUeAgentCanonicalValue* Value = FindField(
				Parameters, TEXT("actor_guids"));
			if (!Value || Value->GetKind() != EUeAgentValueKind::Array || Value->GetArrayItems().IsEmpty())
				return Fail(OutError, InputFailure(TEXT("world_partition_resave_actor_guids_invalid"),
					TEXT("actor_guids must be a non-empty array."), TEXT("/parameters/actor_guids"), Value, TEXT("non-empty array of distinct non-zero GUIDs")));
			TSet<FGuid> UniqueGuids;
			const auto& Items = Value->GetArrayItems();
			for (int32 Index = 0; Index < Items.Num(); ++Index)
			{
				const FUeAgentCanonicalValueRef& Item = Items[Index];
				const FString Path = TEXT("/parameters/actor_guids/") + FString::FromInt(Index);
				FGuid Guid;
				if (Item->GetKind() != EUeAgentValueKind::String
					|| !FGuid::Parse(Item->GetText(), Guid) || !Guid.IsValid())
					return Fail(OutError, InputFailure(TEXT("world_partition_resave_actor_guids_invalid"),
						TEXT("The actor GUID must be a valid non-zero GUID string."), Path, &Item.Get(), TEXT("non-zero GUID string")));
				if (UniqueGuids.Contains(Guid))
					return Fail(OutError, InputFailure(TEXT("world_partition_resave_actor_guids_invalid"),
						TEXT("The actor GUID is duplicated in actor_guids."), Path, &Item.Get(), TEXT("distinct GUID")));
				UniqueGuids.Add(Guid);
				OutGuidTexts.Add(
					Guid.ToString(EGuidFormats::DigitsWithHyphens));
			}
			OutGuidTexts.Sort();
			return true;
		}

		bool AppendBoxMesh(
			UE::Geometry::FDynamicMesh3& Mesh,
			const FVector3d& Size)
		{
			if (Size.X <= 0.0 || Size.Y <= 0.0 || Size.Z <= 0.0)
			{
				return false;
			}
			const FVector3d E = Size * 0.5;
			const int32 V0 = Mesh.AppendVertex({-E.X, -E.Y, -E.Z});
			const int32 V1 = Mesh.AppendVertex({ E.X, -E.Y, -E.Z});
			const int32 V2 = Mesh.AppendVertex({ E.X,  E.Y, -E.Z});
			const int32 V3 = Mesh.AppendVertex({-E.X,  E.Y, -E.Z});
			const int32 V4 = Mesh.AppendVertex({-E.X, -E.Y,  E.Z});
			const int32 V5 = Mesh.AppendVertex({ E.X, -E.Y,  E.Z});
			const int32 V6 = Mesh.AppendVertex({ E.X,  E.Y,  E.Z});
			const int32 V7 = Mesh.AppendVertex({-E.X,  E.Y,  E.Z});
			for (const UE::Geometry::FIndex3i Triangle
				: {
					UE::Geometry::FIndex3i(V0, V2, V1),
					UE::Geometry::FIndex3i(V0, V3, V2),
					UE::Geometry::FIndex3i(V4, V5, V6),
					UE::Geometry::FIndex3i(V4, V6, V7),
					UE::Geometry::FIndex3i(V0, V1, V5),
					UE::Geometry::FIndex3i(V0, V5, V4),
					UE::Geometry::FIndex3i(V1, V2, V6),
					UE::Geometry::FIndex3i(V1, V6, V5),
					UE::Geometry::FIndex3i(V2, V3, V7),
					UE::Geometry::FIndex3i(V2, V7, V6),
					UE::Geometry::FIndex3i(V3, V0, V4),
					UE::Geometry::FIndex3i(V3, V4, V7),
				})
			{
				if (Mesh.AppendTriangle(Triangle) < 0)
				{
					return false;
				}
			}
			return true;
		}

		class FCompileBuildStage final
			: public CompileBuildJob::IUeAgentCompileBuildJobGameThreadStage
		{
		public:
			FCompileBuildStage(
				CompileBuildJob::FUeAgentCompileBuildJobTypedRequest InRequest,
				TWeakObjectPtr<UWorld> InWorld)
				: Request(MoveTemp(InRequest)), World(InWorld)
				, ReportPath(FPaths::ConvertRelativePathToFull(
					FPaths::ProjectSavedDir()
						/ TEXT("UeAgentInterface/LongRunningSession/WorldBuild")
						/ (FGuid::NewGuid().ToString(EGuidFormats::Digits)
							+ TEXT(".json"))))
			{
			}

			bool Execute(
				const FUeAgentCancellationToken& Cancellation,
				FExecutionMutationError& OutError) override
			{
				if (bStopRequested || Cancellation.IsCancelled())
				{
					return Fail(OutError, NativeFailure(TEXT("compile_build_job_cancelled"),
						TEXT("The compile/build job was cancelled before execution."), TEXT("/cancellation_requested"), Request.GetSubjectRef(),
						MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false)));
				}
				if (Request.GetFamily()
					== CompileBuildJob::EUeAgentCompileBuildJobFamily::AssetCompile)
				{
					TGuardValue<bool> UnattendedGuard(GIsRunningUnattendedScript, true);
					TGuardValue<bool> SilentGuard(GIsSilent, true);
					const FString ObjectPath = StripSubjectPrefix(
						Request.GetSubjectRef(), TEXT("asset:"));
					UObject* Asset = ObjectPath.IsEmpty()
						? nullptr
						: StaticLoadObject(
							UObject::StaticClass(), nullptr, *ObjectPath);
					if (UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
					{
						CompileBuildJob::FBlueprintCompilePolicyRequest PolicyRequest;
						TSharedPtr<FJsonObject> Parameters;
						UE::UAI::Contract::FDiagnostic CompileError;
						if (Request.GetPolicyId()
							== TEXT("policy:asset-compile:blueprint/v1"))
						{
							if (!TryCanonicalObjectJson(
									*Request.GetParameters(), Parameters, OutError))
							{
								return false;
							}
							if (!CompileBuildJob::
								TryDecodeBlueprintCompilePolicyRequest(
									Parameters, PolicyRequest,
									CompileError))
							{
								return Fail(OutError, CompileError);
							}
						}
						else
						{
							if (!Request.GetParameters()->GetObjectItems().IsEmpty())
								return Fail(OutError, FactFailure(TEXT("asset_compile_standard_input_invalid"),
									TEXT("The standard asset compile policy accepts no parameter fields."), TEXT("/parameters"),
									FieldNames(*Request.GetParameters()), StringValues({})));
							PolicyRequest.AssetPath = Blueprint->GetPathName();
							PolicyRequest.bIncludeMessages = true;
						}
						if (!CompileBuildJob::TryExecuteBlueprintCompilePolicy(
								*Blueprint, PolicyRequest, Parameters, CompileError))
							return Fail(OutError, CompileError);
						return SaveReport(Parameters.ToSharedRef(), TEXT("blueprint_compile_report_write_failed"), OutError);
					}
					if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset))
					{
						if (Request.GetPolicyId() != TEXT("policy:asset-compile:standard/v1"))
							return Fail(OutError, FactFailure(TEXT("asset_compile_subject_not_supported"),
								TEXT("Static mesh compilation requires the standard asset compile policy."), TEXT("/policy_id"),
								MakeShared<FJsonValueString>(Request.GetPolicyId()), MakeShared<FJsonValueString>(TEXT("policy:asset-compile:standard/v1")),
								TEXT("command_input"), ObjectPath));
						if (!Request.GetParameters()->GetObjectItems().IsEmpty())
							return Fail(OutError, FactFailure(TEXT("asset_compile_standard_input_invalid"),
								TEXT("The standard asset compile policy accepts no parameter fields."), TEXT("/parameters"),
								FieldNames(*Request.GetParameters()), StringValues({})));
						// Supplying OutErrors also makes UE wait for this mesh's build.
						TArray<FText> Diagnostics;
						StaticMesh->Build(false, &Diagnostics);
						const TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
						Report->SetStringField(TEXT("schema"), TEXT("uai.static_mesh_compile.report.v1"));
						Report->SetStringField(TEXT("asset_path"), StaticMesh->GetOutermost()->GetName());
						Report->SetStringField(TEXT("object_path"), StaticMesh->GetPathName());
						Report->SetBoolField(TEXT("build_completed"), !StaticMesh->IsCompiling());
						// UE's OutErrors also contains warnings without severity metadata.
						Report->SetNumberField(TEXT("diagnostic_count"), Diagnostics.Num());
						TArray<TSharedPtr<FJsonValue>> Messages;
						for (const FText& Diagnostic : Diagnostics)
							Messages.Add(MakeShared<FJsonValueString>(Diagnostic.ToString()));
						Report->SetArrayField(TEXT("messages"), MoveTemp(Messages));
						return SaveReport(Report, TEXT("static_mesh_compile_report_write_failed"), OutError);
					}
					FDiagnostic Error = NativeFailure(TEXT("asset_compile_subject_not_supported"),
						Asset ? TEXT("The loaded asset class is not supported by asset compilation.") : TEXT("The compile target asset could not be loaded."),
						TEXT("/asset_class"), ObjectPath, Asset ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Asset->GetClass()->GetPathName())) : nullptr,
						MakeShared<FJsonValueString>(TEXT("Blueprint or StaticMesh")));
					Error.ActualValuePresent = Asset != nullptr;
					Error.ExpectedValuePresent = true;
					return Fail(OutError, MoveTemp(Error));
				}

				UWorld* ExplicitWorld = ResolveCurrentEditorWorld(World);
				const FString WorldPackage = StripSubjectPrefix(
					Request.GetSubjectRef(), TEXT("world:"));
				if (!ExplicitWorld || !ExplicitWorld->GetPackage()
					|| ExplicitWorld->GetPackage()->GetName() != WorldPackage)
				{
					if (!ExplicitWorld || !ExplicitWorld->GetPackage())
						return Fail(OutError, NativeFailure(TEXT("world_build_subject_mismatch"),
							!ExplicitWorld ? TEXT("No editor world is available for the build.") : TEXT("The editor world has no package."),
							!ExplicitWorld ? TEXT("/editor_world") : TEXT("/editor_world/package"), WorldPackage,
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
					return Fail(OutError, FactFailure(TEXT("world_build_subject_mismatch"),
						TEXT("The requested build world does not match the current editor world."), TEXT("/subject_ref"),
						MakeShared<FJsonValueString>(WorldPackage), MakeShared<FJsonValueString>(ExplicitWorld->GetPackage()->GetName())));
				}
				const FString& PolicyId = Request.GetPolicyId();
				if (PolicyId == TEXT("policy:world-build:standard/v1")
					|| PolicyId == TEXT("policy:world-build:navigation/v1"))
				{
					if (!Request.GetParameters()->GetObjectItems().IsEmpty())
					{
						return Fail(OutError, FactFailure(TEXT("world_navigation_build_input_invalid"),
							TEXT("Navigation build accepts no parameter fields."), TEXT("/parameters"),
							FieldNames(*Request.GetParameters()), StringValues({})));
					}
					return FEditorBuildUtils::EditorBuild(
							ExplicitWorld,
							FBuildOptions::BuildAIPaths,
							false)
						? true
						: Fail(OutError, NativeFailure(TEXT("world_navigation_build_native_failed"),
							TEXT("The editor navigation build did not complete successfully."), TEXT("/navigation_build/succeeded"), WorldPackage,
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
				}

				TSharedPtr<FJsonObject> Parameters;
				if (!TryCanonicalObjectJson(
						*Request.GetParameters(), Parameters, OutError))
				{
					return false;
				}
				const TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
				Report->SetStringField(
					TEXT("schema"), TEXT("uai.world_build.plan.v1"));
				Report->SetStringField(TEXT("policy_id"), PolicyId);
				Report->SetStringField(TEXT("subject_ref"), Request.GetSubjectRef());
				Report->SetStringField(TEXT("world_package"), WorldPackage);
				Report->SetStringField(
					TEXT("project_file"),
					FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
				FString CommandletArguments;
				if (PolicyId == TEXT("policy:world-build:hlod-plan/v1"))
				{
					FString Step(TEXT("build"));
					Parameters->TryGetStringField(TEXT("step"), Step);
					Step.ToLowerInline();
					const TMap<FString, FString> StepFlags = {
						{TEXT("setup"), TEXT("-SetupHLODs")},
						{TEXT("build"), TEXT("-BuildHLODs")},
						{TEXT("finalize"), TEXT("-FinalizeHLODs")},
						{TEXT("delete"), TEXT("-DeleteHLODs")},
						{TEXT("stats"), TEXT("-DumpHLODStats")},
					};
					const FString* Flag = StepFlags.Find(Step);
					const int32 ExpectedFieldCount =
						(Parameters->HasField(TEXT("step")) ? 1 : 0);
					if (!Flag)
					{
						return Fail(OutError, FactFailure(TEXT("world_hlod_plan_input_invalid"),
							TEXT("The HLOD plan step is not supported."), TEXT("/parameters/step"),
							MakeShared<FJsonValueString>(Step), StringValues({TEXT("setup"), TEXT("build"), TEXT("finalize"), TEXT("delete"), TEXT("stats")})));
					}
					if (Parameters->Values.Num() != ExpectedFieldCount)
					{
						return Fail(OutError, FactFailure(TEXT("world_hlod_plan_input_invalid"),
							TEXT("The HLOD plan accepts only the optional step field."), TEXT("/parameters"),
							FieldNames(*Parameters), StringValues({TEXT("step")})));
					}
					CommandletArguments = FString::Printf(
						TEXT("-run=WorldPartitionBuilderCommandlet ")
						TEXT("-Builder=WorldPartitionHLODsBuilder %s ")
						TEXT("-AllowCommandletRendering"), **Flag);
					Report->SetStringField(TEXT("step"), Step);
				}
				else if (PolicyId
					== TEXT("policy:world-build:partition-streaming-plan/v1"))
				{
					if (!Parameters->Values.IsEmpty())
					{
						return Fail(OutError, FactFailure(TEXT("world_partition_streaming_plan_input_invalid"),
							TEXT("The partition streaming plan accepts no parameter fields."), TEXT("/parameters"),
							FieldNames(*Parameters), StringValues({})));
					}
					if (!ExplicitWorld->GetWorldPartition())
					{
						return Fail(OutError, NativeFailure(TEXT("world_partition_not_enabled"),
							TEXT("The requested world does not have World Partition enabled."), TEXT("/world_partition"), WorldPackage,
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
					}
					CommandletArguments =
						TEXT("-run=WorldPartitionBuilderCommandlet ")
						TEXT("-Builder=WorldPartitionBuilder ")
						TEXT("-AllowCommandletRendering");
				}
				else if (PolicyId
					== TEXT("policy:world-partition-resave:actors-plan/v1"))
				{
					TArray<FString> ActorGuids;
					if (!TryOnlyFields(*Request.GetParameters(), {TEXT("actor_guids")}, OutError,
							TEXT("world_partition_resave_plan_input_invalid"))
						|| !TryExactActorGuids(*Request.GetParameters(), ActorGuids, OutError)) return false;
					if (!ExplicitWorld->GetWorldPartition())
					{
						return Fail(OutError, NativeFailure(TEXT("world_partition_not_enabled"),
							TEXT("The requested world does not have World Partition enabled."), TEXT("/world_partition"), WorldPackage,
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
					}
					TArray<TSharedPtr<FJsonValue>> JsonGuids;
					for (const FString& ActorGuid : ActorGuids)
					{
						JsonGuids.Add(MakeShared<FJsonValueString>(ActorGuid));
					}
					Report->SetArrayField(TEXT("actor_guids"), MoveTemp(JsonGuids));
					CommandletArguments = FString::Printf(
						TEXT("-run=WorldPartitionBuilderCommandlet ")
						TEXT("-Builder=UeAgentWorldPartitionActorGuidResaveBuilder ")
						TEXT("-ActorGuids=%s"),
						*FString::Join(ActorGuids, TEXT("+")));
				}
				else
				{
					return Fail(OutError, FactFailure(TEXT("world_build_policy_not_supported"),
						TEXT("The world build policy is not supported by the game-thread build stage."), TEXT("/policy_id"),
						MakeShared<FJsonValueString>(PolicyId), StringValues({TEXT("policy:world-build:standard/v1"), TEXT("policy:world-build:navigation/v1"),
							TEXT("policy:world-build:hlod-plan/v1"), TEXT("policy:world-build:partition-streaming-plan/v1"), TEXT("policy:world-partition-resave:actors-plan/v1")})));
				}
				Report->SetStringField(
					TEXT("commandlet_arguments"), CommandletArguments);
				Report->SetBoolField(TEXT("mutates_world"), false);
				return SaveReport(Report, TEXT("world_build_plan_report_write_failed"), OutError);
			}

			void RequestStop(const FString& Reason) override
			{
				bStopRequested = true;
			}

			FString GetReportRef() const override
			{
				return FPaths::FileExists(ReportPath)
					? TEXT("file:") + ReportPath : FString();
			}

		private:
			bool SaveReport(const TSharedRef<FJsonObject>& Report,
				const TCHAR* WriteFailedCode, FExecutionMutationError& OutError) const
			{
				FString ReportJson;
				UE::UAI::Contract::FDiagnostic JsonError;
				if (!Json::TryCanonicalize(Report, ReportJson, JsonError))
					return Fail(OutError, JsonError);
				IFileManager::Get().MakeDirectory(*FPaths::GetPath(ReportPath), true);
				return FFileHelper::SaveStringToFile(ReportJson, *ReportPath,
					FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
					? true : Fail(OutError, FileFailure(WriteFailedCode, TEXT("The compile/build report could not be written."),
						TEXT("/report/write"), ReportPath, Request.GetSubjectRef()));
			}

			CompileBuildJob::FUeAgentCompileBuildJobTypedRequest Request;
			TWeakObjectPtr<UWorld> World;
			FString ReportPath;
			TAtomic<bool> bStopRequested{false};
		};

		class FPackagingStage final
			: public CompileBuildJob::IUeAgentCompileBuildJobGameThreadStage
		{
		public:
			explicit FPackagingStage(
				CompileBuildJob::FUeAgentCompileBuildJobTypedRequest InRequest)
				: Request(MoveTemp(InRequest))
				, ReportPath(FPaths::ConvertRelativePathToFull(
					FPaths::ProjectSavedDir()
					/ TEXT("UeAgentInterface/LongRunningSession/Packaging")
					/ (FGuid::NewGuid().ToString(EGuidFormats::Digits)
						+ TEXT(".json"))))
			{
			}

			bool Execute(
				const FUeAgentCancellationToken& Cancellation,
				FExecutionMutationError& OutError) override
			{
				if (bStopRequested || Cancellation.IsCancelled())
				{
					return Fail(OutError, NativeFailure(TEXT("packaging_job_cancelled"),
						TEXT("The packaging job was cancelled before execution."), TEXT("/cancelled"), Request.GetSubjectRef(),
						MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false)));
				}
				TSharedPtr<FJsonObject> Parameters;
				if (!TryCanonicalObjectJson(
					*Request.GetParameters(), Parameters, OutError))
				{
					return false;
				}
				const FString& Policy = Request.GetPolicyId();
				const TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
				Report->SetStringField(TEXT("schema"), TEXT("uai.packaging.report.v1"));
				Report->SetStringField(TEXT("policy_id"), Policy);
				Report->SetStringField(TEXT("subject_ref"), Request.GetSubjectRef());
				Report->SetStringField(TEXT("project_file"),
					FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));

				if (Policy == TEXT("policy:packaging:query-environment/v1"))
				{
					bool bIncludeSdks = true;
					bool bIncludeTools = true;
					Parameters->TryGetBoolField(TEXT("include_sdks"), bIncludeSdks);
					Parameters->TryGetBoolField(TEXT("include_tools"), bIncludeTools);
					TArray<TSharedPtr<FJsonValue>> Tools;
					const FString EngineDir = FPaths::ConvertRelativePathToFull(FPaths::EngineDir());
					const TArray<TPair<FString, FString>> Candidates = {
						{TEXT("uat"), FPaths::Combine(EngineDir,
							TEXT("Build/BatchFiles/RunUAT.bat"))},
						{TEXT("unreal_editor_cmd"), FPaths::Combine(EngineDir,
							TEXT("Binaries/Win64/UnrealEditor-Cmd.exe"))},
						{TEXT("unreal_pak"), FPaths::Combine(EngineDir,
							TEXT("Binaries/Win64/UnrealPak.exe"))},
					};
					if (bIncludeTools)
					{
						for (const TPair<FString, FString>& Candidate : Candidates)
						{
							const TSharedRef<FJsonObject> Tool = MakeShared<FJsonObject>();
							Tool->SetStringField(TEXT("id"), Candidate.Key);
							Tool->SetStringField(TEXT("path"), Candidate.Value);
							Tool->SetBoolField(
								TEXT("available"), FPaths::FileExists(Candidate.Value));
							Tools.Add(MakeShared<FJsonValueObject>(Tool));
						}
					}
					Report->SetArrayField(TEXT("tools"), MoveTemp(Tools));
					TArray<TSharedPtr<FJsonValue>> TargetPlatforms;
					if (FModuleManager::Get().ModuleExists(TEXT("TargetPlatform")))
					{
						ITargetPlatformManagerModule& Manager =
							FModuleManager::LoadModuleChecked<ITargetPlatformManagerModule>(
								TEXT("TargetPlatform"));
						for (ITargetPlatform* Platform : Manager.GetTargetPlatforms())
						{
							if (!Platform)
							{
								continue;
							}
							const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
							Item->SetStringField(TEXT("name"), Platform->PlatformName());
							Item->SetStringField(
								TEXT("display_name"), Platform->DisplayName().ToString());
							Item->SetBoolField(
								TEXT("supports_packaging"),
								Platform->SupportsFeature(ETargetPlatformFeatures::Packaging));
							if (bIncludeSdks)
							{
								FString DocumentationPath;
								Item->SetBoolField(
									TEXT("sdk_installed"),
									Platform->IsSdkInstalled(false, DocumentationPath));
								Item->SetStringField(
									TEXT("sdk_documentation_path"), DocumentationPath);
							}
							TargetPlatforms.Add(MakeShared<FJsonValueObject>(Item));
						}
					}
					Report->SetArrayField(
						TEXT("target_platforms"), MoveTemp(TargetPlatforms));
				}
				else if (Policy == TEXT("policy:packaging:export-profile/v1"))
				{
					FString Platform;
					FString Configuration;
					if (!Parameters->TryGetStringField(TEXT("platform"), Platform))
					{
						return Fail(OutError, InputFailure(TEXT("packaging_profile_input_invalid"),
						TEXT("Packaging platform must be a string."), TEXT("/parameters/platform"), FindField(*Request.GetParameters(), TEXT("platform")),
						TEXT("string")));
					}
					if (!Parameters->TryGetStringField(TEXT("configuration"), Configuration))
					{
						return Fail(OutError, InputFailure(TEXT("packaging_profile_input_invalid"),
						TEXT("Packaging configuration must be a string."), TEXT("/parameters/configuration"), FindField(*Request.GetParameters(), TEXT("configuration")),
						TEXT("string")));
					}
					const TSharedRef<FJsonObject> Profile = MakeShared<FJsonObject>();
					Profile->SetStringField(TEXT("schema"), TEXT("uai.packaging.v1"));
					Profile->SetStringField(TEXT("platform"), Platform);
					Profile->SetStringField(TEXT("configuration"), Configuration);
					Profile->SetStringField(TEXT("project_file"),
						FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
					Profile->SetStringField(TEXT("pipeline"), TEXT("BuildCookRun"));
					Profile->SetArrayField(TEXT("settings_refs"), {
						MakeShared<FJsonValueString>(TEXT("project:current")),
					});
					Profile->SetStringField(TEXT("target"), Platform);
					Report->SetObjectField(TEXT("profile"), Profile);
				}
				else if (Policy == TEXT("policy:packaging:validate-profile/v1"))
				{
					const TSharedPtr<FJsonObject>* Profile = nullptr;
					const bool bValid = Parameters->TryGetObjectField(
						TEXT("profile"), Profile) && Profile && Profile->IsValid()
						&& (*Profile)->HasTypedField<EJson::String>(TEXT("platform"))
						&& (*Profile)->HasTypedField<EJson::String>(TEXT("configuration"));
					TArray<FDiagnostic> ProfileDiagnostics;
					const FUeAgentCanonicalValue* ProfileInput = FindField(*Request.GetParameters(), TEXT("profile"));
					if (!bValid)
					{
						if (!Profile || !Profile->IsValid())
						{
							ProfileDiagnostics.Add(InputFailure(TEXT("packaging_profile_input_invalid"),
								TEXT("The packaging profile must be an object."), TEXT("/parameters/profile"), ProfileInput, TEXT("object")));
						}
						else
						{
							for (const TCHAR* Field : {TEXT("platform"), TEXT("configuration")})
							{
								if (!(*Profile)->HasTypedField<EJson::String>(Field))
								{
									ProfileDiagnostics.Add(InputFailure(TEXT("packaging_profile_input_invalid"),
										FString::Printf(TEXT("Packaging profile %s must be a string."), Field),
										FString(TEXT("/parameters/profile/")) + Field, FindField(*ProfileInput, Field), TEXT("string")));
								}
							}
						}
					}
					Report->SetBoolField(TEXT("valid"), bValid);
					// Retain the report's existing summary field; diagnostic facts are projected once from their typed source.
					Report->SetArrayField(TEXT("issues"), bValid
						? TArray<TSharedPtr<FJsonValue>>()
						: TArray<TSharedPtr<FJsonValue>>{
							MakeShared<FJsonValueString>(TEXT("profile_required_fields_missing"))});
					Report->SetArrayField(TEXT("diagnostics"), ToPublicDiagnostics(ProfileDiagnostics));
					TArray<TSharedPtr<FJsonValue>> EstimatedActions;
					if (bValid)
					{
						for (const FString& Action : {
							TEXT("build"), TEXT("cook"), TEXT("stage"), TEXT("package")})
						{
							EstimatedActions.Add(MakeShared<FJsonValueString>(Action));
						}
					}
					Report->SetArrayField(
						TEXT("estimated_actions"), MoveTemp(EstimatedActions));
				}
				else if (Policy == TEXT("policy:packaging:diff-profile/v1"))
				{
					const TSharedPtr<FJsonObject>* Profile = nullptr;
					if (!Parameters->TryGetObjectField(TEXT("profile"), Profile)
						|| !Profile || !Profile->IsValid())
					{
						return Fail(OutError, InputFailure(TEXT("packaging_profile_input_invalid"),
						TEXT("Packaging profile must be an object."), TEXT("/parameters/profile"), FindField(*Request.GetParameters(), TEXT("profile")),
						TEXT("object")));
					}
					TArray<TSharedPtr<FJsonValue>> Diffs;
					for (const TPair<FString, TSharedPtr<FJsonValue>>& Field :
						(*Profile)->Values)
					{
						if (Field.Key != TEXT("schema")
							&& Field.Key != TEXT("project_file"))
						{
							const TSharedRef<FJsonObject> Diff = MakeShared<FJsonObject>();
							Diff->SetStringField(TEXT("setting"), Field.Key);
							Diff->SetField(TEXT("transient"), Field.Value);
							Diffs.Add(MakeShared<FJsonValueObject>(Diff));
						}
					}
					Report->SetArrayField(TEXT("setting_diffs"), MoveTemp(Diffs));
					Report->SetObjectField(TEXT("transient_overrides"), *Profile);
				}
				else if (Policy == TEXT("policy:packaging:plan-build/v1"))
				{
					const TSharedPtr<FJsonObject>* Profile = nullptr;
					if (!Parameters->TryGetObjectField(TEXT("profile"), Profile)
						|| !Profile || !Profile->IsValid())
					{
						return Fail(OutError, InputFailure(TEXT("packaging_profile_input_invalid"),
						TEXT("Packaging profile must be an object."), TEXT("/parameters/profile"), FindField(*Request.GetParameters(), TEXT("profile")),
						TEXT("object")));
					}
					FString Platform(TEXT("Win64"));
					FString Configuration(TEXT("Development"));
					(*Profile)->TryGetStringField(TEXT("platform"), Platform);
					(*Profile)->TryGetStringField(TEXT("configuration"), Configuration);
					const TArray<FString> UatArgs = {
						TEXT("BuildCookRun"), TEXT("-project=")
							+ FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()),
						TEXT("-platform=") + Platform,
						TEXT("-clientconfig=") + Configuration,
						TEXT("-build"), TEXT("-cook"), TEXT("-stage"), TEXT("-package"),
					};
					TArray<TSharedPtr<FJsonValue>> JsonArgs;
					for (const FString& Arg : UatArgs)
					{
						JsonArgs.Add(MakeShared<FJsonValueString>(Arg));
					}
					Report->SetArrayField(TEXT("uat_args"), MoveTemp(JsonArgs));
					Report->SetStringField(
						TEXT("command_line"), FString::Join(UatArgs, TEXT(" ")));
					const FString ArchiveRoot = FPaths::ConvertRelativePathToFull(
						FPaths::ProjectSavedDir() / TEXT("UeAgentInterface/Packaging")
						/ Platform / Configuration);
					Report->SetArrayField(TEXT("artifacts_expected"), {
						MakeShared<FJsonValueString>(ArchiveRoot),
						MakeShared<FJsonValueString>(ArchiveRoot / TEXT("*.pak")),
					});
				}
				else if (Policy == TEXT("policy:packaging:parse-log/v1"))
				{
					FString LogFile;
					if (!Parameters->TryGetStringField(TEXT("log_file"), LogFile))
					{
						return Fail(OutError, InputFailure(TEXT("packaging_log_path_invalid"),
						TEXT("The packaging log file must be a string path."), TEXT("/parameters/log_file"), FindField(*Request.GetParameters(), TEXT("log_file")),
						TEXT("string path"), true));
					}
					LogFile = FPaths::ConvertRelativePathToFull(LogFile);
					if (!LogFile.StartsWith(FPaths::ConvertRelativePathToFull(
						FPaths::ProjectSavedDir())))
					{
						return Fail(OutError, InputFailure(TEXT("packaging_log_path_unmanaged"),
						TEXT("The log file is outside the project Saved directory."), TEXT("/parameters/log_file"), FindField(*Request.GetParameters(), TEXT("log_file")),
						TEXT("path within the project Saved directory"), true));
					}
					FString Text;
					if (!FFileHelper::LoadFileToString(Text, *LogFile))
					{
						return Fail(OutError, FileFailure(TEXT("packaging_log_read_failed"),
						TEXT("The packaging log could not be read."), TEXT("/log/read"), LogFile, Request.GetSubjectRef()));
					}
					TArray<FString> Lines;
					Text.ParseIntoArrayLines(Lines, false);
					TArray<TSharedPtr<FJsonValue>> Errors;
					TArray<TSharedPtr<FJsonValue>> Warnings;
					int32 BuildLines = 0;
					int32 CookLines = 0;
					int32 StageLines = 0;
					int32 PackageLines = 0;
					for (const FString& Line : Lines)
					{
						if (Line.Contains(TEXT("Error:"), ESearchCase::IgnoreCase))
						{
							Errors.Add(MakeShared<FJsonValueString>(Line));
						}
						else if (Line.Contains(TEXT("Warning:"), ESearchCase::IgnoreCase))
						{
							Warnings.Add(MakeShared<FJsonValueString>(Line));
						}
						BuildLines += Line.Contains(TEXT("build"), ESearchCase::IgnoreCase) ? 1 : 0;
						CookLines += Line.Contains(TEXT("cook"), ESearchCase::IgnoreCase) ? 1 : 0;
						StageLines += Line.Contains(TEXT("stage"), ESearchCase::IgnoreCase) ? 1 : 0;
						PackageLines += Line.Contains(TEXT("package"), ESearchCase::IgnoreCase) ? 1 : 0;
					}
					Report->SetArrayField(TEXT("errors"), MoveTemp(Errors));
					Report->SetArrayField(TEXT("warnings"), MoveTemp(Warnings));
					const TSharedRef<FJsonObject> PhaseSummary = MakeShared<FJsonObject>();
					PhaseSummary->SetNumberField(TEXT("build_lines"), BuildLines);
					PhaseSummary->SetNumberField(TEXT("cook_lines"), CookLines);
					PhaseSummary->SetNumberField(TEXT("stage_lines"), StageLines);
					PhaseSummary->SetNumberField(TEXT("package_lines"), PackageLines);
					Report->SetObjectField(TEXT("phase_summary"), PhaseSummary);
				}
				else if (Policy == TEXT("policy:packaging:report-artifacts/v1"))
				{
					FString ArchiveDir;
					bool bIncludeHashes = false;
					if (!Parameters->TryGetStringField(TEXT("archive_dir"), ArchiveDir))
					{
						return Fail(OutError, InputFailure(TEXT("packaging_archive_path_invalid"),
						TEXT("The packaging archive directory must be a string path."), TEXT("/parameters/archive_dir"), FindField(*Request.GetParameters(), TEXT("archive_dir")),
						TEXT("string path"), true));
					}
					Parameters->TryGetBoolField(TEXT("include_hashes"), bIncludeHashes);
					ArchiveDir = FPaths::ConvertRelativePathToFull(ArchiveDir);
					const FString ManagedRoot = FPaths::ConvertRelativePathToFull(
						FPaths::ProjectSavedDir() / TEXT("UeAgentInterface/Packaging"));
					if (!ArchiveDir.StartsWith(ManagedRoot))
					{
						return Fail(OutError, InputFailure(TEXT("packaging_archive_path_unmanaged"),
						TEXT("The archive directory is outside the managed packaging directory."), TEXT("/parameters/archive_dir"), FindField(*Request.GetParameters(), TEXT("archive_dir")),
						TEXT("path within Saved/UeAgentInterface/Packaging"), true));
					}
					TArray<FString> Files;
					IFileManager::Get().FindFilesRecursive(Files, *ArchiveDir,
						TEXT("*"), true, false, false);
					Files.Sort();
					TArray<TSharedPtr<FJsonValue>> Artifacts;
					int64 TotalSize = 0;
					for (const FString& File : Files)
					{
						const int64 Size = IFileManager::Get().FileSize(*File);
						TotalSize += FMath::Max<int64>(0, Size);
						const TSharedRef<FJsonObject> Artifact = MakeShared<FJsonObject>();
						Artifact->SetStringField(TEXT("path"), File);
						Artifact->SetNumberField(TEXT("size"), Size);
						if (bIncludeHashes)
						{
							Artifact->SetStringField(
								TEXT("md5"), LexToString(FMD5Hash::HashFile(*File)));
						}
						Artifacts.Add(MakeShared<FJsonValueObject>(Artifact));
					}
					Report->SetArrayField(TEXT("artifacts"), MoveTemp(Artifacts));
					Report->SetArrayField(TEXT("missing_expected"), Files.IsEmpty()
						? TArray<TSharedPtr<FJsonValue>>{
							MakeShared<FJsonValueString>(TEXT("archive_contains_no_artifacts"))}
						: TArray<TSharedPtr<FJsonValue>>());
					Report->SetNumberField(TEXT("total_size"), TotalSize);
				}
				else if (Policy == TEXT("policy:packaging:clean-artifacts/v1"))
				{
					FString ArchiveDir;
					bool bDryRun = true;
					if (!Parameters->TryGetStringField(TEXT("archive_dir"), ArchiveDir))
					{
						return Fail(OutError, InputFailure(TEXT("packaging_clean_input_invalid"),
						TEXT("The archive directory to clean must be a string path."), TEXT("/parameters/archive_dir"), FindField(*Request.GetParameters(), TEXT("archive_dir")),
						TEXT("string path"), true));
					}
					if (!Parameters->TryGetBoolField(TEXT("dry_run"), bDryRun))
					{
						return Fail(OutError, InputFailure(TEXT("packaging_clean_input_invalid"),
						TEXT("The clean operation requires a boolean dry_run value."), TEXT("/parameters/dry_run"), FindField(*Request.GetParameters(), TEXT("dry_run")),
						TEXT("boolean")));
					}
					ArchiveDir = FPaths::ConvertRelativePathToFull(ArchiveDir);
					const FString ManagedRoot = FPaths::ConvertRelativePathToFull(
						FPaths::ProjectSavedDir() / TEXT("UeAgentInterface/Packaging"));
					if (!ArchiveDir.StartsWith(ManagedRoot)
						|| ArchiveDir.Equals(ManagedRoot, ESearchCase::IgnoreCase))
					{
						return Fail(OutError, InputFailure(TEXT("packaging_archive_path_unmanaged"),
						TEXT("Cleaning requires a child of the managed packaging directory; the root itself is not accepted."), TEXT("/parameters/archive_dir"), FindField(*Request.GetParameters(), TEXT("archive_dir")),
						TEXT("child path within Saved/UeAgentInterface/Packaging"), true));
					}
					TArray<FString> Files;
					IFileManager::Get().FindFilesRecursive(Files, *ArchiveDir,
						TEXT("*"), true, false, false);
					int64 TotalSize = 0;
					for (const FString& File : Files)
					{
						TotalSize += FMath::Max<int64>(0, IFileManager::Get().FileSize(*File));
					}
					if (!bDryRun && !IFileManager::Get().DeleteDirectory(
						*ArchiveDir, false, true))
					{
						return Fail(OutError, FileFailure(TEXT("packaging_clean_delete_failed"),
						TEXT("The archive directory could not be deleted."), TEXT("/archive/delete"), ArchiveDir, Request.GetSubjectRef()));
					}
					TArray<TSharedPtr<FJsonValue>> Deleted;
					for (const FString& File : Files)
					{
						Deleted.Add(MakeShared<FJsonValueString>(File));
					}
					Report->SetBoolField(TEXT("dry_run"), bDryRun);
					Report->SetArrayField(TEXT("deleted"), MoveTemp(Deleted));
					Report->SetNumberField(TEXT("freed_bytes"), bDryRun ? 0 : TotalSize);
				}
				else
				{
					return Fail(OutError, FactFailure(TEXT("packaging_policy_not_supported"),
						TEXT("The packaging stage does not implement this policy."), TEXT("/policy_id"),
						MakeShared<FJsonValueString>(Policy), StringValues({TEXT("policy:packaging:query-environment/v1"),
							TEXT("policy:packaging:export-profile/v1"), TEXT("policy:packaging:validate-profile/v1"),
							TEXT("policy:packaging:diff-profile/v1"), TEXT("policy:packaging:plan-build/v1"),
							TEXT("policy:packaging:parse-log/v1"), TEXT("policy:packaging:report-artifacts/v1"),
							TEXT("policy:packaging:clean-artifacts/v1")})));
				}

				FString ReportJson;
				UE::UAI::Contract::FDiagnostic JsonError;
				if (!Json::TryCanonicalize(Report, ReportJson, JsonError))
				{
					return Fail(OutError, JsonError);
				}
				IFileManager::Get().MakeDirectory(*FPaths::GetPath(ReportPath), true);
				return FFileHelper::SaveStringToFile(
					ReportJson, *ReportPath,
					FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
					? true
					: Fail(OutError, FileFailure(TEXT("packaging_report_write_failed"),
						TEXT("The packaging report could not be written."), TEXT("/report/write"), ReportPath, Request.GetSubjectRef()));
			}

			void RequestStop(const FString& Reason) override
			{
				bStopRequested = true;
			}

			FString GetReportRef() const override
			{
				return TEXT("file:") + ReportPath;
			}

		private:
			CompileBuildJob::FUeAgentCompileBuildJobTypedRequest Request;
			FString ReportPath;
			TAtomic<bool> bStopRequested{false};
		};

		class FFileProcessSink final
			: public CompileBuildJob::IUeAgentCompileBuildJobProcessOutputSink
		{
		public:
			explicit FFileProcessSink(FString InReportPath)
				: ReportPath(MoveTemp(InReportPath))
				, ReportRef(TEXT("file:") + ReportPath)
			{
			}

			bool Append(
				const FString& Chunk,
				FExecutionMutationError& OutError) override
			{
				FScopeLock Lock(&Mutex);
				if (bReleased)
				{
					return Fail(OutError, NativeFailure(TEXT("owned_process_sink_released"),
						TEXT("Process output cannot be appended after its sink was released."), TEXT("/output_sink/released"),
						TEXT("owned_process"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false)));
				}
				if (!FFileHelper::SaveStringToFile(
						Chunk,
						*ReportPath,
						FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
						&IFileManager::Get(),
						FILEWRITE_Append))
				{
					return Fail(OutError, FileFailure(TEXT("owned_process_report_write_failed"),
						TEXT("Process output could not be appended to the report."), TEXT("/report/append"), ReportPath, TEXT("owned_process")));
				}
				return true;
			}

			FString GetReportRef() const override
			{
				return ReportRef;
			}

			bool Release(FExecutionMutationError& OutError) override
			{
				FScopeLock Lock(&Mutex);
				bReleased = true;
				return true;
			}

		private:
			FString ReportPath;
			FString ReportRef;
			mutable FCriticalSection Mutex;
			bool bReleased = false;
		};

		struct FLocalizationOwnedToolPreparation
		{
			FString PolicyId;
			FString TargetName;
			FString Executable;
			FString Arguments;
			FString OutputDirectory;
			TArray<FString> Cultures;
			TArray<FString> ConfigPaths;
			double TimeoutSeconds = 0.0;
			bool bExecute = false;
		};

		bool MatchesRequestedCulture(
			const FString& Path,
			const TArray<FString>& Cultures)
		{
			if (Cultures.IsEmpty())
			{
				return true;
			}
			FString Normalized = Path;
			FPaths::NormalizeFilename(Normalized);
			for (const FString& Culture : Cultures)
			{
				if (Normalized.Contains(TEXT("/") + Culture + TEXT("/")))
				{
					return true;
				}
			}
			return false;
		}

		TArray<FString> FindLocalizationArtifacts(
			const FString& Root,
			const FString& Extension,
			const TArray<FString>& Cultures)
		{
			TArray<FString> Candidates;
			if (IFileManager::Get().DirectoryExists(*Root))
			{
				IFileManager::Get().FindFilesRecursive(
					Candidates,
					*Root,
					*(TEXT("*.") + Extension),
					true,
					false,
					false);
			}
			Candidates.RemoveAll(
				[&Cultures](const FString& Candidate)
				{
					return !MatchesRequestedCulture(Candidate, Cultures);
				});
			Candidates.Sort();
			return Candidates;
		}

		int32 CountPoEntries(const FString& Path)
		{
			FString Text;
			if (!FFileHelper::LoadFileToString(Text, *Path))
			{
				return 0;
			}
			TArray<FString> Lines;
			Text.ParseIntoArrayLines(Lines, false);
			int32 Count = 0;
			for (FString Line : Lines)
			{
				Line.TrimStartAndEndInline();
				if (Line.StartsWith(TEXT("msgid \""))
					&& Line != TEXT("msgid \"\""))
				{
					++Count;
				}
			}
			return Count;
		}

		bool DeleteOwnedLocalizationConfigs(
			const TArray<FString>& ConfigPaths,
			FExecutionMutationError& OutError)
		{
			const FString ManagedRoot = FPaths::ConvertRelativePathToFull(
				FPaths::ProjectSavedDir()
				/ TEXT("UeAgentInterface/LongRunningSession/LocalizationConfigs"));
			FString ManagedPrefix = ManagedRoot;
			FPaths::NormalizeDirectoryName(ManagedPrefix);
			ManagedPrefix += TEXT("/");
			for (int32 Index = 0; Index < ConfigPaths.Num(); ++Index)
			{
				FString Resolved = FPaths::ConvertRelativePathToFull(ConfigPaths[Index]);
				FPaths::NormalizeFilename(Resolved);
				if (!Resolved.StartsWith(ManagedPrefix, ESearchCase::IgnoreCase))
				{
					FDiagnostic Error = FileFailure(TEXT("localization_owned_tool_cleanup_path_unmanaged"),
						TEXT("The owned localization config is outside its managed directory."),
						TEXT("/localization_configs/") + FString::FromInt(Index), Resolved, TEXT("localization"));
					Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("path within Saved/UeAgentInterface/LongRunningSession/LocalizationConfigs"));
					return Fail(OutError, MoveTemp(Error));
				}
				if (FPaths::FileExists(Resolved)
					&& !IFileManager::Get().Delete(*Resolved, false, true, true))
				{
					return Fail(OutError, FileFailure(TEXT("localization_owned_tool_config_cleanup_failed"),
						TEXT("The owned localization config could not be deleted."),
						TEXT("/localization_configs/") + FString::FromInt(Index), Resolved, TEXT("localization")));
				}
			}
			return true;
		}

		class FLocalizationProcessSink final
			: public CompileBuildJob::IUeAgentCompileBuildJobProcessOutputSink
		{
		public:
			FLocalizationProcessSink(
				FString InReportPath,
				FString InLogPath,
				FLocalizationOwnedToolPreparation InPreparation)
				: ReportPath(MoveTemp(InReportPath))
				, LogPath(MoveTemp(InLogPath))
				, ReportRef(TEXT("file:") + ReportPath)
				, Preparation(MoveTemp(InPreparation))
			{
			}

			~FLocalizationProcessSink() override
			{
				if (!bReleased)
				{
					FExecutionMutationError Ignored;
					DeleteOwnedLocalizationConfigs(
						Preparation.ConfigPaths,
						Ignored);
				}
			}

			bool Append(
				const FString& Chunk,
				FExecutionMutationError& OutError) override
			{
				FScopeLock Lock(&Mutex);
				if (bReleased)
				{
					return Fail(OutError, NativeFailure(TEXT("owned_process_sink_released"),
						TEXT("Process output cannot be appended after its sink was released."), TEXT("/output_sink/released"),
						TEXT("owned_process"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false)));
				}
				return FFileHelper::SaveStringToFile(
					Chunk,
					*LogPath,
					FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
					&IFileManager::Get(),
					FILEWRITE_Append)
					? true
					: Fail(OutError, FileFailure(TEXT("owned_process_report_write_failed"),
						TEXT("Localization process output could not be appended to the log."), TEXT("/log/append"), LogPath, TEXT("localization:") + Preparation.TargetName));
			}

			FString GetReportRef() const override
			{
				return ReportRef;
			}

			bool Release(FExecutionMutationError& OutError) override
			{
				FScopeLock Lock(&Mutex);
				if (bReleased)
				{
					return true;
				}
				if (!DeleteOwnedLocalizationConfigs(Preparation.ConfigPaths, OutError))
				{
					return false;
				}
				bReleased = true;
				return true;
			}

			bool FinalizeReport(FExecutionMutationError& OutError) override
			{
				FScopeLock Lock(&Mutex);
				if (bReportFinalized) return true;
				if (!TryWriteReport(OutError)) return false;
				bReportFinalized = true;
				return true;
			}

		private:
			bool TryWriteReport(FExecutionMutationError& OutError) const
			{
				const TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
				Report->SetStringField(
					TEXT("schema"),
					TEXT("uai.localization.owned_tool_report.v1"));
				Report->SetStringField(TEXT("policy_id"), Preparation.PolicyId);
				Report->SetStringField(TEXT("target"), Preparation.TargetName);
				Report->SetBoolField(TEXT("execute"), Preparation.bExecute);
				Report->SetStringField(
					TEXT("report_state"), TEXT("artifact_capture_complete"));
				Report->SetStringField(TEXT("log_file"), LogPath);
				Report->SetStringField(TEXT("report_file"), ReportPath);
				if (Preparation.TimeoutSeconds > 0.0)
				{
					Report->SetNumberField(
						TEXT("timeout_seconds"),
						Preparation.TimeoutSeconds);
				}
				TArray<TSharedPtr<FJsonValue>> CultureValues;
				for (const FString& Culture : Preparation.Cultures)
				{
					CultureValues.Add(MakeShared<FJsonValueString>(Culture));
				}
				Report->SetArrayField(TEXT("cultures"), MoveTemp(CultureValues));

				const TSharedPtr<FJsonObject> Plan = MakeShared<FJsonObject>();
				Plan->SetStringField(TEXT("executable"), Preparation.Executable);
				Plan->SetStringField(TEXT("arguments"), Preparation.Arguments);
				Plan->SetNumberField(
					TEXT("config_count"),
					Preparation.ConfigPaths.Num());
				Report->SetObjectField(TEXT("commandlet_plan"), Plan);

				TArray<FString> Artifacts;
				if (Preparation.PolicyId
					== TEXT("policy:owned-tool:localization-compile-text/v1"))
				{
					Artifacts = FindLocalizationArtifacts(
						FPaths::ProjectContentDir()
							/ TEXT("Localization")
							/ Preparation.TargetName,
						TEXT("locres"),
						Preparation.Cultures);
					TArray<TSharedPtr<FJsonValue>> Values;
					for (const FString& Artifact : Artifacts)
					{
						Values.Add(MakeShared<FJsonValueString>(Artifact));
					}
					Report->SetArrayField(TEXT("locres_files"), MoveTemp(Values));
					Report->SetNumberField(TEXT("compiled_count"), Artifacts.Num());
				}
				else if (Preparation.PolicyId
					== TEXT("policy:owned-tool:localization-export-po/v1"))
				{
					const FString ScanRoot = Preparation.bExecute
						? Preparation.OutputDirectory
						: FPaths::ProjectContentDir()
							/ TEXT("Localization")
							/ Preparation.TargetName;
					Artifacts = FindLocalizationArtifacts(
						ScanRoot,
						TEXT("po"),
						Preparation.Cultures);
					TArray<TSharedPtr<FJsonValue>> Values;
					int32 EntryCount = 0;
					for (const FString& Artifact : Artifacts)
					{
						Values.Add(MakeShared<FJsonValueString>(Artifact));
						EntryCount += CountPoEntries(Artifact);
					}
					Report->SetArrayField(TEXT("po_files"), MoveTemp(Values));
					Report->SetNumberField(TEXT("entry_count"), EntryCount);
				}

				FString CanonicalJson;
				UE::UAI::Contract::FDiagnostic JsonError;
				if (!Json::TryCanonicalize(Report, CanonicalJson, JsonError))
				{
					return Fail(OutError, JsonError);
				}
				return FFileHelper::SaveStringToFile(
					CanonicalJson,
					*ReportPath,
					FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
					? true
					: Fail(OutError, FileFailure(TEXT("localization_owned_tool_report_write_failed"),
						TEXT("The localization report could not be written."), TEXT("/report/write"), ReportPath, TEXT("localization:") + Preparation.TargetName));
			}

			FString ReportPath;
			FString LogPath;
			FString ReportRef;
			FLocalizationOwnedToolPreparation Preparation;
			mutable FCriticalSection Mutex;
			bool bReportFinalized = false;
			bool bReleased = false;
		};

		bool IsLocalizationOwnedToolPolicy(const FString& PolicyId)
		{
			return PolicyId == TEXT("policy:owned-tool:localization-gather-text/v1")
				|| PolicyId == TEXT("policy:owned-tool:localization-compile-text/v1")
				|| PolicyId == TEXT("policy:owned-tool:localization-export-po/v1");
		}

		ULocalizationTarget* FindLocalizationTarget(const FString& TargetName)
		{
			ULocalizationTargetSet* TargetSet =
				ULocalizationSettings::GetGameTargetSet();
			if (!TargetSet)
			{
				return nullptr;
			}
			const TObjectPtr<ULocalizationTarget>* Match =
				TargetSet->TargetObjects.FindByPredicate(
					[&TargetName](const ULocalizationTarget* Candidate)
					{
						return Candidate
							&& Candidate->Settings.Name == TargetName;
					});
			return Match ? Match->Get() : nullptr;
		}

		bool TryPrepareLocalizationOwnedTool(
			const CompileBuildJob::FUeAgentCompileBuildJobTypedRequest& Request,
			FLocalizationOwnedToolPreparation& OutPreparation,
			FExecutionMutationError& OutError)
		{
			OutPreparation = FLocalizationOwnedToolPreparation();
			FString& OutExecutable = OutPreparation.Executable;
			FString& OutArguments = OutPreparation.Arguments;
			OutExecutable.Reset();
			OutArguments.Reset();
			const FString PolicyId = Request.GetPolicyId();
			OutPreparation.PolicyId = PolicyId;
			if (!IsLocalizationOwnedToolPolicy(PolicyId))
			{
				return Fail(OutError, FactFailure(TEXT("localization_owned_tool_policy_invalid"),
					TEXT("This policy does not select a supported localization tool."), TEXT("/policy_id"),
					MakeShared<FJsonValueString>(PolicyId), StringValues({TEXT("policy:owned-tool:localization-gather-text/v1"),
						TEXT("policy:owned-tool:localization-compile-text/v1"), TEXT("policy:owned-tool:localization-export-po/v1")})));
			}
			const FString TargetName = StripSubjectPrefix(
				Request.GetSubjectRef(), TEXT("localization:"));
			if (TargetName.IsEmpty()
				|| TargetName.Contains(TEXT("/"))
				|| TargetName.Contains(TEXT("\\"))
				|| TargetName.Contains(TEXT("\"")))
			{
				return Fail(OutError, FactFailure(TEXT("localization_owned_tool_subject_invalid"),
					TEXT("The subject must name a localization target without slashes or quotes."), TEXT("/subject_ref"),
					MakeShared<FJsonValueString>(Request.GetSubjectRef()), MakeShared<FJsonValueString>(TEXT("localization:<nonempty target name>"))));
			}
			OutPreparation.TargetName = TargetName;

			TSharedPtr<FJsonObject> Parameters;
			if (!TryCanonicalObjectJson(
					*Request.GetParameters(), Parameters, OutError))
			{
				return false;
			}
			const bool bGather = PolicyId
				== TEXT("policy:owned-tool:localization-gather-text/v1");
			const bool bCompile = PolicyId
				== TEXT("policy:owned-tool:localization-compile-text/v1");
			const bool bExport = PolicyId
				== TEXT("policy:owned-tool:localization-export-po/v1");
			bool bExecute = false;
			Parameters->TryGetBoolField(TEXT("execute"), bExecute);
			OutPreparation.bExecute = bExecute;

			ULocalizationTarget* Target = FindLocalizationTarget(TargetName);
			if (!Target)
			{
				return Fail(OutError, NativeFailure(TEXT("localization_owned_tool_target_not_found"),
					TEXT("The requested localization target is not registered in the project."), TEXT("/localization_target"),
					Request.GetSubjectRef(), MakeShared<FJsonValueString>(TargetName), MakeShared<FJsonValueString>(TEXT("registered game localization target"))));
			}
			if ((bCompile || bExport)
				&& !Target->Settings.SupportedCulturesStatistics.IsValidIndex(
					Target->Settings.NativeCultureIndex))
			{
				return Fail(OutError, NativeFailure(TEXT("localization_owned_tool_native_culture_invalid"),
					TEXT("The target native culture index is outside its supported culture list."), TEXT("/native_culture_index"),
					Request.GetSubjectRef(), MakeShared<FJsonValueNumber>(Target->Settings.NativeCultureIndex),
					MakeShared<FJsonValueString>(FString::Printf(TEXT("0 <= index < %d"), Target->Settings.SupportedCulturesStatistics.Num()))));
			}
			TArray<FString> Cultures;
			const TArray<TSharedPtr<FJsonValue>>* CultureValues = nullptr;
			if (Parameters->TryGetArrayField(TEXT("cultures"), CultureValues)
				&& CultureValues)
			{
				TSet<FString> UniqueCultures;
				for (int32 Index = 0; Index < CultureValues->Num(); ++Index)
				{
					const TSharedPtr<FJsonValue>& Value = (*CultureValues)[Index];
					const FString Path = TEXT("/parameters/cultures/") + FString::FromInt(Index);
					FString Culture;
					if (!Value.IsValid()
						|| !Value->TryGetString(Culture)
						|| Culture.IsEmpty()
						|| UniqueCultures.Contains(Culture))
					{
						FDiagnostic Error = FactFailure(TEXT("localization_owned_tool_cultures_invalid"),
							!Value.IsValid() || Value->Type != EJson::String ? TEXT("Each requested culture must be a string.")
								: Culture.IsEmpty() ? TEXT("The requested culture is empty.") : TEXT("The requested culture occurs more than once."),
							Path, Value, MakeShared<FJsonValueString>(TEXT("unique nonempty culture string")));
						Error.ActualValuePresent = Value.IsValid();
						Error.ExpectedValuePresent = true;
						return Fail(OutError, MoveTemp(Error));
					}
					const bool bSupported =
						Target->Settings.SupportedCulturesStatistics.ContainsByPredicate(
							[&Culture](const FCultureStatistics& Candidate)
							{
								return Candidate.CultureName == Culture;
							});
					if (!bSupported)
					{
						TArray<FString> SupportedCultures;
						for (const FCultureStatistics& Candidate : Target->Settings.SupportedCulturesStatistics)
							SupportedCultures.Add(Candidate.CultureName);
						return Fail(OutError, FactFailure(TEXT("localization_owned_tool_culture_not_supported"),
							TEXT("The requested culture is not supported by the localization target."), Path, Value,
							StringValues(SupportedCultures), TEXT("command_input"), Request.GetSubjectRef()));
					}
					UniqueCultures.Add(Culture);
					Cultures.Add(MoveTemp(Culture));
				}
			}
			OutPreparation.Cultures = Cultures;

			if (bGather || bCompile || bExport)
			{
				double TimeoutSeconds = 0.0;
				Parameters->TryGetNumberField(
					TEXT("timeout_seconds"), TimeoutSeconds);
				if (!FMath::IsFinite(TimeoutSeconds)
					|| TimeoutSeconds < 0.0)
				{
					return Fail(OutError, InputFailure(TEXT("localization_owned_tool_timeout_seconds_invalid"),
						TEXT("The timeout must be finite and nonnegative."), TEXT("/parameters/timeout_seconds"),
						FindField(*Request.GetParameters(), TEXT("timeout_seconds")), TEXT("finite number >= 0")));
				}
				OutPreparation.TimeoutSeconds = TimeoutSeconds;
			}

			FString OutputDirectory;
			if (bExport)
			{
				if (!Parameters->TryGetStringField(
						TEXT("output_dir"), OutputDirectory)
					|| OutputDirectory.TrimStartAndEnd().IsEmpty()
					|| OutputDirectory.Contains(TEXT("\""))
					|| OutputDirectory.Contains(TEXT("\r"))
					|| OutputDirectory.Contains(TEXT("\n")))
				{
					return Fail(OutError, InputFailure(TEXT("localization_owned_tool_output_dir_invalid"),
						TEXT("The output directory must be a nonblank string without quotes or line breaks."), TEXT("/parameters/output_dir"),
						FindField(*Request.GetParameters(), TEXT("output_dir")), TEXT("nonblank path without quote, CR or LF"), true));
				}
				OutputDirectory = FPaths::ConvertRelativePathToFull(OutputDirectory);
				OutPreparation.OutputDirectory = OutputDirectory;
			}

			if (!bExecute)
			{
#if PLATFORM_WINDOWS
				OutExecutable = FPlatformMisc::GetEnvironmentVariable(TEXT("ComSpec"));
				if (OutExecutable.IsEmpty())
				{
					OutExecutable = FPaths::Combine(
						FPlatformMisc::GetEnvironmentVariable(TEXT("SystemRoot")),
						TEXT("System32/cmd.exe"));
				}
				OutArguments = FString::Printf(
					TEXT("/D /S /C \"echo UAI_LOCALIZATION_PLAN policy=%s target=%s\""),
					*PolicyId,
					*TargetName);
#else
				OutExecutable = TEXT("/bin/sh");
				OutArguments = FString::Printf(
					TEXT("-c \"printf 'UAI_LOCALIZATION_PLAN policy=%s target=%s\\n'\""),
					*PolicyId,
					*TargetName);
#endif
				return true;
			}

			const FString ConfigDirectory = FPaths::ConvertRelativePathToFull(
				FPaths::ProjectSavedDir()
				/ TEXT("UeAgentInterface/LongRunningSession/LocalizationConfigs"));
			IFileManager::Get().MakeDirectory(*ConfigDirectory, true);
			TArray<FString>& ConfigPaths = OutPreparation.ConfigPaths;
			auto WriteScript = [&](FLocalizationConfigurationScript Script, const FString& FailureCode) -> bool
			{
				const FString Path = ConfigDirectory
					/ (TargetName + TEXT("-")
						+ FGuid::NewGuid().ToString(EGuidFormats::Digits)
						+ TEXT(".ini"));
				if (!Script.Write(Path))
				{
					OutError = FileFailure(FailureCode, TEXT("The localization commandlet config could not be written."),
						TEXT("/localization_config/write"), Path, Request.GetSubjectRef());
					FExecutionMutationError CleanupError;
					if (!DeleteOwnedLocalizationConfigs(ConfigPaths, CleanupError)) OutError.Append(CleanupError);
					ConfigPaths.Reset();
					return false;
				}
				ConfigPaths.Add(Path);
				return true;
			};
			if (bGather)
			{
				if (!WriteScript(
						LocalizationConfigurationScript::GenerateGatherTextConfigFile(
							Target), TEXT("localization_gather_config_write_failed")))
				{
					return false;
				}
			}
			else
			{
				const TArray<FString> RequestedCultures = Cultures.Num() > 0
					? Cultures
					: TArray<FString>{FString()};
				for (const FString& Culture : RequestedCultures)
				{
					const TOptional<FString> CultureOption = Culture.IsEmpty()
						? TOptional<FString>()
						: TOptional<FString>(Culture);
					// UE treats an override with a selected culture as a file, otherwise as a directory.
					const FString ExportPath = CultureOption.IsSet()
						? OutputDirectory / Culture / LocalizationConfigurationScript::GetDefaultPOFileName(Target)
						: OutputDirectory;
					FLocalizationConfigurationScript Script = bCompile
						? LocalizationConfigurationScript::GenerateCompileTextConfigFile(
							Target, CultureOption)
						: LocalizationConfigurationScript::GenerateExportTextConfigFile(
							Target,
							CultureOption,
							TOptional<FString>(ExportPath));
					if (!WriteScript(MoveTemp(Script), TEXT("localization_owned_tool_config_write_failed")))
					{
						return false;
					}
				}
			}

			OutExecutable = FPlatformProcess::ExecutablePath();
			OutExecutable.ReplaceInline(
				TEXT("UnrealEditor.exe"),
				TEXT("UnrealEditor-Cmd.exe"),
				ESearchCase::IgnoreCase);
			OutArguments = FString::Printf(
				TEXT("\"%s\" -run=GatherText -Config=\"%s\" ")
				TEXT("-unattended -nop4 -nosplash -NullRHI -stdout -FullStdOutLogOutput -LiveCoding=false"),
				*FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()),
				*FString::Join(ConfigPaths, TEXT(";")));
			return true;
		}

		class FCompileBuildBuilder final
			: public CompileBuildJob::IUeAgentCompileBuildJobPortBuilder
		{
		public:
			explicit FCompileBuildBuilder(TWeakObjectPtr<UWorld> InWorld)
				: World(InWorld)
			{
			}

			bool TryCreatePort(
				const CompileBuildJob::FUeAgentCompileBuildJobTypedRequest& Request,
				const CompileBuildJob::FUeAgentCompileBuildJobPolicyDescriptor& Policy,
				TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe>& OutPort,
				FExecutionMutationError& OutError) override
			{
				if (Policy.GetNativeMechanismId() == TEXT("game_thread_stage"))
				{
					if (Request.GetFamily()
						== CompileBuildJob::EUeAgentCompileBuildJobFamily::Packaging)
					{
						return CompileBuildJob::
							TryCreateCompileBuildJobGameThreadStagePort(
								Request,
								Policy,
								MakeShared<FPackagingStage, ESPMode::ThreadSafe>(
									Request),
								OutPort,
								OutError);
					}
					return CompileBuildJob::
						TryCreateCompileBuildJobGameThreadStagePort(
							Request,
							Policy,
							MakeShared<FCompileBuildStage, ESPMode::ThreadSafe>(
								Request, World),
							OutPort,
							OutError);
				}

				FString Executable;
				FString Arguments;
				FString WorkingDirectory = FPaths::ProjectDir();
				FLocalizationOwnedToolPreparation LocalizationPreparation;
				bool bLocalizationOwnedTool = false;
				auto FailPreparation = [&](FDiagnostic Error) -> bool
				{
					if (bLocalizationOwnedTool)
					{
						FExecutionMutationError CleanupError;
						if (!DeleteOwnedLocalizationConfigs(LocalizationPreparation.ConfigPaths, CleanupError)) Error.Append(CleanupError);
					}
					return Fail(OutError, MoveTemp(Error));
				};
				if (Request.GetFamily()
					== CompileBuildJob::EUeAgentCompileBuildJobFamily::ExternalProcess)
				{
					if (IsLocalizationOwnedToolPolicy(Request.GetPolicyId()))
					{
						if (!TryPrepareLocalizationOwnedTool(
								Request,
								LocalizationPreparation,
								OutError))
						{
							return false;
						}
						Executable = LocalizationPreparation.Executable;
						Arguments = LocalizationPreparation.Arguments;
						bLocalizationOwnedTool = true;
					}
					else if (Request.GetSubjectRef()
							!= TEXT("project:owned-tool-probe")
						|| !Request.GetParameters()->GetObjectItems().IsEmpty())
					{
						return Fail(OutError, Request.GetSubjectRef() != TEXT("project:owned-tool-probe")
							? FactFailure(TEXT("owned_tool_policy_input_not_trusted"),
								TEXT("The probe policy requires the owned tool probe subject."), TEXT("/subject_ref"),
								MakeShared<FJsonValueString>(Request.GetSubjectRef()), MakeShared<FJsonValueString>(TEXT("project:owned-tool-probe")))
							: FactFailure(TEXT("owned_tool_policy_input_not_trusted"),
								TEXT("The owned tool probe accepts no parameters."), TEXT("/parameters"),
								FieldNames(*Request.GetParameters()), StringValues({})));
					}
					else
					{
#if PLATFORM_WINDOWS
						Executable = FPlatformMisc::GetEnvironmentVariable(TEXT("ComSpec"));
					if (Executable.IsEmpty())
					{
							Executable = FPaths::Combine(
							FPlatformMisc::GetEnvironmentVariable(TEXT("SystemRoot")),
							TEXT("System32/cmd.exe"));
					}
						Arguments = TEXT(
						"/D /S /C \"echo UAI_OWNED_TOOL_PROBE_OK\"");
#else
						Executable = TEXT("/bin/sh");
						Arguments = TEXT(
						"-c \"printf 'UAI_OWNED_TOOL_PROBE_OK\\n'\"");
#endif
					}
				}
				if (Request.GetFamily()
					== CompileBuildJob::EUeAgentCompileBuildJobFamily::
						WorldPartitionResave)
				{
					const FString WorldPackage = StripSubjectPrefix(
						Request.GetSubjectRef(), TEXT("world:"));
					if (!FPackageName::IsValidLongPackageName(WorldPackage))
					{
						return Fail(OutError, FactFailure(TEXT("world_partition_resave_subject_invalid"),
							TEXT("The resave subject must identify a valid world package."), TEXT("/subject_ref"),
							MakeShared<FJsonValueString>(Request.GetSubjectRef()), MakeShared<FJsonValueString>(TEXT("world:<valid long package name>"))));
					}
					TArray<FString> ActorGuids;
					if (Request.GetPolicyId() != TEXT("policy:world-partition-resave:actors-run/v1"))
						return Fail(OutError, FactFailure(TEXT("world_partition_resave_execute_input_invalid"),
							TEXT("The execution policy does not support exact actor resave."), TEXT("/policy_id"),
							MakeShared<FJsonValueString>(Request.GetPolicyId()), MakeShared<FJsonValueString>(TEXT("policy:world-partition-resave:actors-run/v1"))));
					if (!TryOnlyFields(*Request.GetParameters(), {TEXT("actor_guids")}, OutError,
							TEXT("world_partition_resave_execute_input_invalid"))
						|| !TryExactActorGuids(*Request.GetParameters(), ActorGuids, OutError)) return false;
					Executable = FPlatformProcess::ExecutablePath();
					Executable.ReplaceInline(
						TEXT("UnrealEditor.exe"), TEXT("UnrealEditor-Cmd.exe"),
						ESearchCase::IgnoreCase);
					Arguments = FString::Printf(
						TEXT("\"%s\" \"%s\" -run=WorldPartitionBuilderCommandlet "
							"-Builder=UeAgentWorldPartitionActorGuidResaveBuilder "
							"-ActorGuids=%s -unattended -nop4 -nosplash -NoSound "
							"-stdout -FullStdOutLogOutput -Multiprocess "
							"-LiveCoding=false -NullRHI"),
						*FPaths::ConvertRelativePathToFull(
							FPaths::GetProjectFilePath()),
						*WorldPackage,
						*FString::Join(ActorGuids, TEXT("+")));
				}
				if (Request.GetFamily()
					== CompileBuildJob::EUeAgentCompileBuildJobFamily::WorldBuild
					&& Policy.GetNativeMechanismId() == TEXT("owned_process"))
				{
					const FString WorldPackage = StripSubjectPrefix(
						Request.GetSubjectRef(), TEXT("world:"));
					if (!FPackageName::IsValidLongPackageName(WorldPackage))
					{
						return Fail(OutError, FactFailure(TEXT("world_build_subject_invalid"),
							TEXT("The world build subject must identify a valid world package."), TEXT("/subject_ref"),
							MakeShared<FJsonValueString>(Request.GetSubjectRef()), MakeShared<FJsonValueString>(TEXT("world:<valid long package name>"))));
					}
					TSharedPtr<FJsonObject> Parameters;
					if (!TryCanonicalObjectJson(
							*Request.GetParameters(), Parameters, OutError))
					{
						return false;
					}
					FString BuilderArguments;
					if (Request.GetPolicyId()
						== TEXT("policy:world-build:hlod-run/v1"))
					{
						FString Step(TEXT("build"));
						Parameters->TryGetStringField(TEXT("step"), Step);
						Step.ToLowerInline();
						const TMap<FString, FString> StepFlags = {
							{TEXT("setup"), TEXT("-SetupHLODs")},
							{TEXT("build"), TEXT("-BuildHLODs")},
							{TEXT("finalize"), TEXT("-FinalizeHLODs")},
							{TEXT("delete"), TEXT("-DeleteHLODs")},
							{TEXT("stats"), TEXT("-DumpHLODStats")},
						};
						const FString* Flag = StepFlags.Find(Step);
						bool bAllowDelete = false;
						Parameters->TryGetBoolField(
							TEXT("allow_delete"), bAllowDelete);
						if (!Flag)
						{
							return Fail(OutError, FactFailure(TEXT("world_hlod_execute_input_invalid"),
								TEXT("The HLOD build step is not supported."), TEXT("/parameters/step"),
								MakeShared<FJsonValueString>(Step), StringValues({TEXT("setup"), TEXT("build"), TEXT("finalize"), TEXT("delete"), TEXT("stats")})));
						}
						if (Step == TEXT("delete") && !bAllowDelete)
						{
							return Fail(OutError, InputFailure(TEXT("world_hlod_execute_input_invalid"),
								TEXT("The delete step requires allow_delete to be true."), TEXT("/parameters/allow_delete"),
								FindField(*Request.GetParameters(), TEXT("allow_delete")), TEXT("true")));
						}
						if (!TryOnlyFields(*Request.GetParameters(), {TEXT("step"), TEXT("allow_delete")}, OutError,
							TEXT("world_hlod_execute_input_invalid"))) return false;
						BuilderArguments = FString::Printf(
							TEXT("-run=WorldPartitionBuilderCommandlet ")
							TEXT("-Builder=WorldPartitionHLODsBuilder %s ")
							TEXT("-AllowCommandletRendering"), **Flag);
						if (Step != TEXT("build") && Step != TEXT("finalize"))
						{
							BuilderArguments += TEXT(" -NullRHI");
						}
					}
					else if (Request.GetPolicyId()
						== TEXT("policy:world-build:partition-streaming-run/v1"))
					{
						if (!Parameters->Values.IsEmpty())
						{
							return Fail(OutError, FactFailure(TEXT("world_partition_streaming_execute_input_invalid"),
								TEXT("The partition streaming build accepts no parameters."), TEXT("/parameters"),
								FieldNames(*Parameters), StringValues({})));
						}
						BuilderArguments =
							TEXT("-run=WorldPartitionBuilderCommandlet ")
							TEXT("-Builder=WorldPartitionBuilder ")
							TEXT("-AllowCommandletRendering");
					}
					else
					{
						return Fail(OutError, FactFailure(TEXT("world_build_execute_policy_invalid"),
							TEXT("This world build policy does not select a supported owned process."), TEXT("/policy_id"),
							MakeShared<FJsonValueString>(Request.GetPolicyId()), StringValues({TEXT("policy:world-build:hlod-run/v1"),
								TEXT("policy:world-build:partition-streaming-run/v1")})));
					}
					Executable = FPlatformProcess::ExecutablePath();
					Executable.ReplaceInline(
						TEXT("UnrealEditor.exe"), TEXT("UnrealEditor-Cmd.exe"),
						ESearchCase::IgnoreCase);
					Arguments = FString::Printf(
						TEXT("\"%s\" \"%s\" %s -unattended -nop4 -nosplash ")
						TEXT("-NoSound -stdout -FullStdOutLogOutput -Multiprocess ")
						TEXT("-LiveCoding=false"),
						*FPaths::ConvertRelativePathToFull(
							FPaths::GetProjectFilePath()),
						*WorldPackage,
						*BuilderArguments);
				}
				if (Request.GetFamily()
					== CompileBuildJob::EUeAgentCompileBuildJobFamily::Packaging)
				{
					if (Request.GetPolicyId()
							!= TEXT("policy:packaging:run-build/v1")
						|| Request.GetSubjectRef() != TEXT("project:current"))
					{
						return Fail(OutError, Request.GetPolicyId() != TEXT("policy:packaging:run-build/v1")
							? FactFailure(TEXT("packaging_run_policy_invalid"), TEXT("Packaging execution requires the run-build policy."),
								TEXT("/policy_id"), MakeShared<FJsonValueString>(Request.GetPolicyId()), MakeShared<FJsonValueString>(TEXT("policy:packaging:run-build/v1")))
							: FactFailure(TEXT("packaging_run_policy_invalid"), TEXT("Packaging execution requires the current project subject."),
								TEXT("/subject_ref"), MakeShared<FJsonValueString>(Request.GetSubjectRef()), MakeShared<FJsonValueString>(TEXT("project:current"))));
					}
					TSharedPtr<FJsonObject> Parameters;
					if (!TryCanonicalObjectJson(
						*Request.GetParameters(), Parameters, OutError))
					{
						return false;
					}
					const FUeAgentCanonicalValue* ProfileInput = FindField(*Request.GetParameters(), TEXT("profile"));
					const TSharedPtr<FJsonObject>* Profile = nullptr;
					if (Parameters->Values.Num() != 1
						|| !Parameters->TryGetObjectField(TEXT("profile"), Profile)
						|| !Profile || !Profile->IsValid())
					{
						return Fail(OutError, Parameters->Values.Num() != 1
							? FactFailure(TEXT("packaging_run_input_invalid"), TEXT("Packaging execution requires exactly one profile parameter."),
								TEXT("/parameters"), FieldNames(*Parameters), StringValues({TEXT("profile")}))
							: InputFailure(TEXT("packaging_run_input_invalid"), TEXT("The packaging profile must be an object."),
								TEXT("/parameters/profile"), ProfileInput, TEXT("object")));
					}
					FString Platform;
					FString Configuration;
					FString ArchiveDir;
					if (!(*Profile)->TryGetStringField(TEXT("platform"), Platform))
					{
						return Fail(OutError, InputFailure(TEXT("packaging_run_profile_invalid"),
							TEXT("Packaging profile platform must be a string."), TEXT("/parameters/profile/platform"),
							FindField(*ProfileInput, TEXT("platform")), TEXT("string")));
					}
					if (!(*Profile)->TryGetStringField(TEXT("configuration"), Configuration))
					{
						return Fail(OutError, InputFailure(TEXT("packaging_run_profile_invalid"),
							TEXT("Packaging profile configuration must be a string."), TEXT("/parameters/profile/configuration"),
							FindField(*ProfileInput, TEXT("configuration")), TEXT("string")));
					}
					if (!(*Profile)->TryGetStringField(TEXT("archive_dir"), ArchiveDir))
					{
						return Fail(OutError, InputFailure(TEXT("packaging_run_profile_invalid"),
							TEXT("Packaging profile archive_dir must be a string."), TEXT("/parameters/profile/archive_dir"),
							FindField(*ProfileInput, TEXT("archive_dir")), TEXT("string"), true));
					}
					if (Platform != TEXT("Win64"))
					{
						return Fail(OutError, InputFailure(TEXT("packaging_run_profile_invalid"),
							TEXT("This packaging execution supports only the Win64 platform."), TEXT("/parameters/profile/platform"),
							FindField(*ProfileInput, TEXT("platform")), TEXT("Win64")));
					}
					if (Configuration != TEXT("Development") && Configuration != TEXT("Shipping"))
					{
						return Fail(OutError, InputFailure(TEXT("packaging_run_profile_invalid"),
							TEXT("This packaging execution supports Development or Shipping configuration."), TEXT("/parameters/profile/configuration"),
							FindField(*ProfileInput, TEXT("configuration")), TEXT("Development or Shipping")));
					}
					ArchiveDir = FPaths::ConvertRelativePathToFull(ArchiveDir);
					const FString ManagedRoot = FPaths::ConvertRelativePathToFull(
						FPaths::ProjectSavedDir() / TEXT("UeAgentInterface/Packaging"));
					if (!ArchiveDir.StartsWith(ManagedRoot))
					{
						return Fail(OutError, InputFailure(TEXT("packaging_archive_path_unmanaged"),
							TEXT("The archive directory is outside the managed packaging directory."), TEXT("/parameters/profile/archive_dir"),
							FindField(*ProfileInput, TEXT("archive_dir")), TEXT("path within Saved/UeAgentInterface/Packaging"), true));
					}
					Executable = FPaths::Combine(
						FPaths::EngineDir(), TEXT("Build/BatchFiles/RunUAT.bat"));
					Arguments = FString::Printf(
						TEXT("BuildCookRun -project=\"%s\" -platform=%s "
							"-clientconfig=%s -build -cook -stage -package "
							"-archive -archivedirectory=\"%s\" -nop4 -unattended"),
						*FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()),
						*Platform, *Configuration, *ArchiveDir);
				}
				if (Executable.IsEmpty())
				{
					return FailPreparation(NativeFailure(TEXT("owned_process_executable_unavailable"),
						TEXT("The selected job policy did not resolve an executable."), TEXT("/executable"), Request.GetSubjectRef(),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
				}
				if (!FPaths::FileExists(Executable))
				{
					return FailPreparation(FileFailure(TEXT("owned_process_executable_unavailable"),
						TEXT("The resolved job executable does not exist."), TEXT("/executable/exists"), Executable, Request.GetSubjectRef()));
				}
				const FString ArtifactStem =
					FGuid::NewGuid().ToString(EGuidFormats::Digits);
				const FString ReportPath = FPaths::ConvertRelativePathToFull(
					FPaths::ProjectSavedDir()
					/ TEXT("UeAgentInterface/LongRunningSession/Reports")
					/ (ArtifactStem
						+ (bLocalizationOwnedTool ? TEXT(".json") : TEXT(".log"))));
				IFileManager::Get().MakeDirectory(*FPaths::GetPath(ReportPath), true);
				if (!FFileHelper::SaveStringToFile(
						FString(),
						*ReportPath,
						FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
				{
					return FailPreparation(FileFailure(TEXT("owned_process_report_create_failed"),
						TEXT("The owned process report could not be created."), TEXT("/report/create"), ReportPath, Request.GetSubjectRef()));
				}
				TSharedRef<
					CompileBuildJob::IUeAgentCompileBuildJobProcessOutputSink,
					ESPMode::ThreadSafe> OutputSink =
					MakeShared<FFileProcessSink, ESPMode::ThreadSafe>(ReportPath);
				const double ProcessTimeoutSeconds = bLocalizationOwnedTool
					? LocalizationPreparation.TimeoutSeconds
					: 0.0;
				if (bLocalizationOwnedTool)
				{
					const FString LogFilePath = FPaths::ConvertRelativePathToFull(
						FPaths::ProjectSavedDir()
						/ TEXT("UeAgentInterface/LongRunningSession/Reports")
						/ (ArtifactStem + TEXT(".log")));
					if (!FFileHelper::SaveStringToFile(
							FString(),
							*LogFilePath,
							FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
					{
						return FailPreparation(FileFailure(TEXT("owned_process_report_create_failed"),
							TEXT("The localization process log could not be created."), TEXT("/log/create"), LogFilePath, Request.GetSubjectRef()));
					}
					OutputSink = MakeShared<
						FLocalizationProcessSink,
						ESPMode::ThreadSafe>(
						ReportPath,
						LogFilePath,
						MoveTemp(LocalizationPreparation));
				}
				return CompileBuildJob::TryCreateCompileBuildJobOwnedProcessPort(
					Request,
					Policy,
					CompileBuildJob::FUeAgentCompileBuildJobProcessLaunchSpec(
						MoveTemp(Executable),
						MoveTemp(Arguments),
						MoveTemp(WorkingDirectory),
						ProcessTimeoutSeconds),
					MoveTemp(OutputSink),
					OutPort,
					OutError);
			}

		private:
			TWeakObjectPtr<UWorld> World;
		};

		enum class EEditorNativeState : uint8
		{
			Scheduled,
			Running,
			ExitedSuccess,
			ExitedFailure,
		};

		class FEditorNativeSession final
			: public EditorSessionOperation::IUeAgentEditorSessionNativeSession
			, public TSharedFromThis<FEditorNativeSession, ESPMode::ThreadSafe>
		{
		public:
			FEditorNativeSession(
				EditorSessionOperation::FUeAgentEditorSessionOperationTypedRequest
					InRequest,
				TWeakObjectPtr<UWorld> InWorld)
				: Request(MoveTemp(InRequest)), World(InWorld)
			{
			}

			~FEditorNativeSession() override
			{
				FTSTicker::FDelegateHandle Handle;
				{
					FScopeLock Lock(&Mutex);
					Handle = StopPollTicker;
					StopPollTicker.Reset();
				}
				if (Handle.IsValid())
				{
					FTSTicker::GetCoreTicker().RemoveTicker(Handle);
				}
			}

			bool Start(
				const FLongRunningOperationRequest& OperationRequest,
				const FUeAgentCancellationToken& Cancellation,
				FString& OutNativeResourceId,
				FExecutionMutationError& OutError) override
			{
				FScopeLock Lock(&Mutex);
				if (bStarted)
				{
					return Fail(OutError, NativeFailure(TEXT("editor_native_session_already_started"),
						TEXT("The editor native session has already started."), TEXT("/session/started"), Request.GetSubjectRef(), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false)));
				}
				if (Cancellation.IsCancelled())
				{
					return Fail(OutError, NativeFailure(TEXT("editor_native_session_cancelled_before_start"),
						TEXT("The editor native session was cancelled before it could start."), TEXT("/cancelled"), Request.GetSubjectRef(), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false)));
				}
				bStarted = true;
				NativeResourceId = TEXT("editor-native:")
					+ OperationRequest.GetOperationId();
				OutNativeResourceId = NativeResourceId;
				const TSharedRef<FEditorNativeSession, ESPMode::ThreadSafe> Self =
					StaticCastSharedRef<FEditorNativeSession>(AsShared());
				AsyncTask(ENamedThreads::GameThread, [Self]() { Self->StartOnGameThread(); });
				return true;
			}

			bool Observe(
				EditorSessionOperation::FUeAgentEditorSessionNativeObservation&
					OutObservation,
				FExecutionMutationError& OutError) override
			{
				FScopeLock Lock(&Mutex);
				if (!bStarted)
				{
					return Fail(OutError, NativeFailure(TEXT("editor_native_session_not_started"),
						TEXT("The editor native session cannot be observed before Start."), TEXT("/session/started"), Request.GetSubjectRef(), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
				}
				ELongRunningWorkState WorkState = ELongRunningWorkState::Running;
				FString Phase = TEXT("scheduled");
				if (State == EEditorNativeState::Running)
				{
					Phase = bAssetEditorCleanupPending ? TEXT("cleanup_pending") : TEXT("active");
				}
				else if (State == EEditorNativeState::ExitedSuccess)
				{
					WorkState = ELongRunningWorkState::ExitedSuccess;
					Phase = TEXT("exited");
				}
				else if (State == EEditorNativeState::ExitedFailure)
				{
					WorkState = ELongRunningWorkState::ExitedFailure;
					Phase = TEXT("failed");
				}
				OutObservation = EditorSessionOperation::
					FUeAgentEditorSessionNativeObservation(
						WorkState,
						MoveTemp(Phase),
						StateDiagnostic,
						SessionRef,
						Details);
				return true;
			}

			bool RequestStop(
				const FString& Reason,
				FExecutionMutationError& OutError) override
			{
				{
					FScopeLock Lock(&Mutex);
					if (bStopRequested && !(AssetEditorLease && bAssetEditorCleanupPending))
					{
						return true;
					}
					bStopRequested = true;
				}
				const TSharedRef<FEditorNativeSession, ESPMode::ThreadSafe> Self =
					StaticCastSharedRef<FEditorNativeSession>(AsShared());
				AsyncTask(ENamedThreads::GameThread, [Self]() { Self->StopOnGameThread(); });
				return true;
			}

			bool Join(FExecutionMutationError& OutError) override
			{
				FScopeLock Lock(&Mutex);
				if (AssetEditorLease && !bAssetEditorCleanupComplete)
					return Fail(OutError, NativeFailure(TEXT("asset_editor_cleanup_pending"),
						TEXT("The owned asset editor has not finished closing; recover and close this operation after resolving the reported window state."),
						TEXT("/asset_editor/cleanup_complete"), Request.GetSubjectRef(),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
				return State == EEditorNativeState::ExitedSuccess
					|| State == EEditorNativeState::ExitedFailure
					? true
					: Fail(OutError, NativeFailure(TEXT("editor_native_session_join_before_exit"),
						TEXT("The editor native session cannot be joined before it exits."), TEXT("/session/state"), Request.GetSubjectRef(), MakeShared<FJsonValueString>(State == EEditorNativeState::Scheduled ? TEXT("scheduled") : TEXT("running")), StringValues({TEXT("exited_success"), TEXT("exited_failure")})));
			}

			bool Release(FExecutionMutationError& OutError) override
			{
				FScopeLock Lock(&Mutex);
				if (AssetEditorLease && !bAssetEditorCleanupComplete)
					return Fail(OutError, NativeFailure(TEXT("asset_editor_cleanup_pending"),
						TEXT("The owned asset editor cannot be released before its actual closure is confirmed."),
						TEXT("/asset_editor/cleanup_complete"), Request.GetSubjectRef(),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
				if (Request.GetFamily() == EditorSessionOperation::EUeAgentEditorSessionOperationFamily::PcgGeneration
					&& State != EEditorNativeState::ExitedSuccess && State != EEditorNativeState::ExitedFailure)
					return Fail(OutError, NativeFailure(TEXT("pcg_generation_release_before_exit"),
						TEXT("PCG generation must finish or cancel before its operation can be released."),
						TEXT("/session/state"), Request.GetSubjectRef(), MakeShared<FJsonValueString>(TEXT("running")),
						MakeShared<FJsonValueString>(TEXT("exited"))));
				bReleased = true;
				return true;
			}

		private:
			UWorld* CurrentEditorWorld() const
			{
				return ResolveCurrentEditorWorld(World);
			}

			bool StartWorldContext(
				FString& OutSessionRef,
				bool& bOutKeepRunning,
				UE::UAI::Contract::FDiagnostic& OutError)
			{
				bOutKeepRunning = false;
				const FString WorldPackage = StripSubjectPrefix(
					Request.GetSubjectRef(), TEXT("world:"));
				if (!FPackageName::IsValidLongPackageName(WorldPackage, false))
				{
					OutError = FactFailure(TEXT("world_context_subject_invalid"),
						TEXT("The world context subject must identify a valid world package."), TEXT("/subject_ref"),
						MakeShared<FJsonValueString>(Request.GetSubjectRef()), MakeShared<FJsonValueString>(TEXT("world:<valid long package name>")));
					return false;
				}
				TSharedPtr<FJsonObject> Parameters;
				FExecutionMutationError ParseError;
				if (!TryCanonicalObjectJson(*Request.GetParameters(), Parameters, ParseError))
				{
					OutError = ParseError;
					return false;
				}
				const FString& PolicyId = Request.GetPolicyId();
				if (PolicyId == TEXT("policy:world-context:set-current/v1"))
				{
					bool bSaveExistingMap = true;
					Parameters->TryGetBoolField(TEXT("save_existing_map"), bSaveExistingMap);
					if (!TryOnlyFields(*Request.GetParameters(), {TEXT("save_existing_map")}, OutError,
						TEXT("world_context_open_input_invalid"))) return false;
					FWorldActivationResult Activation;
					FWorldActivationError ActivationError;
					if (!TryActivateExistingWorld(WorldPackage,
							bSaveExistingMap ? ECurrentWorldDirtyPolicy::SaveDirtyMapPackages
											 : ECurrentWorldDirtyPolicy::RequireClean,
							Activation, ActivationError))
					{
						if (ActivationError.Code == TEXT("editor_world_target_not_found"))
						{
							OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(
								TEXT("world_context_map_not_found"), ActivationError, TEXT("world_activation"));
						}
						else if (ActivationError.Code == TEXT("editor_world_current_save_failed"))
						{
							OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(
								TEXT("world_context_save_existing_failed"), ActivationError, TEXT("world_activation"));
						}
						else
						{
							OutError = ActivationError;
						}
						return false;
					}
					OutSessionRef = TEXT("world-context:") + WorldPackage;
					return true;
				}

				if (PolicyId == TEXT("policy:world-context:create-blank/v1"))
				{
					FString OverwritePolicy(TEXT("fail"));
					Parameters->TryGetStringField(TEXT("overwrite_policy"), OverwritePolicy);
					OverwritePolicy.ToLowerInline();
					bool bSaveExistingMap = true;
					Parameters->TryGetBoolField(TEXT("save_existing_map"), bSaveExistingMap);
					bool bCreateWorldPartition = false;
					Parameters->TryGetBoolField(
						TEXT("create_world_partition"), bCreateWorldPartition);
					if (OverwritePolicy != TEXT("fail") && OverwritePolicy != TEXT("reuse") && OverwritePolicy != TEXT("replace"))
					{
						OutError = FactFailure(TEXT("world_context_create_input_invalid"),
							TEXT("The overwrite policy is not supported."), TEXT("/parameters/overwrite_policy"),
							MakeShared<FJsonValueString>(OverwritePolicy), StringValues({TEXT("fail"), TEXT("reuse"), TEXT("replace")}));
						return false;
					}
					if (!TryOnlyFields(*Request.GetParameters(),
						{TEXT("overwrite_policy"), TEXT("save_existing_map"), TEXT("create_world_partition")}, OutError,
						TEXT("world_context_create_input_invalid"))) return false;
					const bool bExists = FPackageName::DoesPackageExist(WorldPackage);
					if (bExists && OverwritePolicy == TEXT("fail"))
					{
						OutError = NativeFailure(TEXT("world_context_map_exists"),
						TEXT("The requested world package already exists and overwrite_policy is fail."), TEXT("/world/package_exists"), Request.GetSubjectRef(), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
						return false;
					}
					if (bExists && OverwritePolicy == TEXT("reuse"))
					{
						FWorldActivationResult Activation;
						FWorldActivationError ActivationError;
						if (!TryActivateExistingWorld(WorldPackage,
								bSaveExistingMap ? ECurrentWorldDirtyPolicy::SaveDirtyMapPackages
												 : ECurrentWorldDirtyPolicy::RequireClean,
								Activation, ActivationError))
						{
							OutError = ActivationError;
							return false;
						}
						OutSessionRef = TEXT("world-context:reused:") + WorldPackage;
						return true;
					}
					UWorld* Created = UEditorLoadingAndSavingUtils::NewBlankMap(bSaveExistingMap);
					if (!Created)
					{
						OutError = NativeFailure(TEXT("world_context_create_failed"),
						TEXT("The editor could not create a blank map."), TEXT("/world/created"), Request.GetSubjectRef(), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						return false;
					}
					if (bCreateWorldPartition)
					{
						if (!Created->PersistentLevel)
						{
							OutError = NativeFailure(TEXT("world_context_persistent_level_missing"),
						TEXT("The new world has no persistent level for external actors."), TEXT("/world/persistent_level/available"), Request.GetSubjectRef(), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
							return false;
						}
						Created->PersistentLevel->SetUseExternalActors(true);
						UWorldPartition* Partition = UWorldPartition::CreateOrRepairWorldPartition(
							Created->GetWorldSettings());
						if (!Partition)
						{
							OutError = NativeFailure(TEXT("world_context_partition_create_failed"),
						TEXT("World Partition creation or repair returned no partition."), TEXT("/world/partition/available"), Request.GetSubjectRef(), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
							return false;
						}
						if (!Partition->IsInitialized())
						{
							Partition->Initialize(Created, FTransform::Identity);
						}
					}
					if (!UEditorLoadingAndSavingUtils::SaveMap(Created, WorldPackage))
					{
						OutError = NativeFailure(TEXT("world_context_create_save_failed"),
						TEXT("The new world could not be saved to the requested package."), TEXT("/world/save_succeeded"), Request.GetSubjectRef(), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						return false;
					}
					OutSessionRef = TEXT("world-context:created:") + WorldPackage;
					return true;
				}

				UWorld* ExplicitWorld = CurrentEditorWorld();
				if (!ExplicitWorld || !ExplicitWorld->GetPackage() ||
					ExplicitWorld->GetPackage()->GetName() != WorldPackage)
				{
					OutError = NativeFailure(TEXT("world_partition_loader_subject_mismatch"),
						!ExplicitWorld ? TEXT("No editor world is available for the partition loader.")
							: !ExplicitWorld->GetPackage() ? TEXT("The editor world has no package.") : TEXT("The current world differs from the requested loader world."),
						TEXT("/world/package"), Request.GetSubjectRef(),
						ExplicitWorld && ExplicitWorld->GetPackage()
							? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(ExplicitWorld->GetPackage()->GetName())) : nullptr,
						MakeShared<FJsonValueString>(WorldPackage));
					OutError.ActualValuePresent = ExplicitWorld && ExplicitWorld->GetPackage();
					OutError.ExpectedValuePresent = true;
					return false;
				}
								if (!ExplicitWorld->GetWorldPartition()) {
                                  OutError = NativeFailure(TEXT("world_partition_not_enabled"),
						TEXT("The current world does not have World Partition enabled."), TEXT("/world/partition/available"), Request.GetSubjectRef(), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
                                  return false;
                                }
                                bool bDryRun = false;
                                Parameters->TryGetBoolField(TEXT("dry_run"),
                                                            bDryRun);
                                if (PolicyId ==
                                    TEXT("policy:world-context:partition-"
                                         "region-loader/v1")) {
                                  if (!TryOnlyFields(*Request.GetParameters(), {TEXT("bounds"), TEXT("label"), TEXT("dry_run")},
                                      OutError, TEXT("world_partition_region_input_invalid"))) return false;
                                  const TSharedPtr<FJsonObject> *Bounds =
                                      nullptr;
                                  const TArray<TSharedPtr<FJsonValue>> *Center =
                                      nullptr;
                                  const TArray<TSharedPtr<FJsonValue>> *Extent =
                                      nullptr;
                                  if (!Parameters->TryGetObjectField(
                                          TEXT("bounds"), Bounds) ||
                                      !Bounds || !Bounds->IsValid() ||
                                      !(*Bounds)->TryGetArrayField(
                                          TEXT("center"), Center) ||
                                      !(*Bounds)->TryGetArrayField(
                                          TEXT("extent"), Extent) ||
                                      !Center || !Extent ||
                                      Center->Num() != 3 ||
                                      Extent->Num() != 3) {
                                    const FUeAgentCanonicalValue* BoundsInput = FindField(*Request.GetParameters(), TEXT("bounds"));
                                    const FString Field = !Bounds || !Bounds->IsValid() ? FString()
                                        : !Center ? TEXT("center") : !Extent ? TEXT("extent")
                                        : Center->Num() != 3 ? TEXT("center") : TEXT("extent");
                                    OutError = InputFailure(TEXT("world_partition_region_bounds_invalid"),
                                        Field.IsEmpty() ? TEXT("Region bounds must be an object.") : TEXT("A region bounds vector must be an array of three components."),
                                        TEXT("/parameters/bounds") + (Field.IsEmpty() ? FString() : TEXT("/") + Field),
                                        Field.IsEmpty() ? BoundsInput : FindField(*BoundsInput, Field),
                                        Field.IsEmpty() ? TEXT("object with center and extent") : TEXT("array with exactly 3 components"));
                                    return false;
                                  }
                                  const FVector CenterValue(
                                      (*Center)[0]->AsNumber(),
                                      (*Center)[1]->AsNumber(),
                                      (*Center)[2]->AsNumber());
                                  const FVector ExtentValue(
                                      (*Extent)[0]->AsNumber(),
                                      (*Extent)[1]->AsNumber(),
                                      (*Extent)[2]->AsNumber());
                                  if (!FMath::IsFinite(CenterValue.X) ||
                                      !FMath::IsFinite(CenterValue.Y) ||
                                      !FMath::IsFinite(CenterValue.Z) ||
                                      !FMath::IsFinite(ExtentValue.X) ||
                                      !FMath::IsFinite(ExtentValue.Y) ||
                                      !FMath::IsFinite(ExtentValue.Z) ||
                                      ExtentValue.GetMin() < 0.0) {
                                    const double Components[] = {CenterValue.X, CenterValue.Y, CenterValue.Z, ExtentValue.X, ExtentValue.Y, ExtentValue.Z};
                                    int32 InvalidIndex = 0;
                                    while (InvalidIndex < 6 && FMath::IsFinite(Components[InvalidIndex])) ++InvalidIndex;
                                    if (InvalidIndex == 6)
                                    {
                                        InvalidIndex = 3;
                                        while (InvalidIndex < 5 && Components[InvalidIndex] >= 0.0) ++InvalidIndex;
                                    }
                                    const FString Field = InvalidIndex < 3 ? TEXT("center") : TEXT("extent");
                                    const int32 ComponentIndex = InvalidIndex % 3;
                                    const auto* BoundsInput = FindField(*Request.GetParameters(), TEXT("bounds"));
                                    const auto* VectorInput = FindField(*BoundsInput, Field);
                                    OutError = InputFailure(TEXT("world_partition_region_bounds_invalid"),
                                        InvalidIndex < 3 ? TEXT("A region center component is not finite.") : TEXT("A region extent component must be finite and nonnegative."),
                                        TEXT("/parameters/bounds/") + Field + TEXT("/") + FString::FromInt(ComponentIndex),
                                        &VectorInput->GetArrayItems()[ComponentIndex].Get(),
                                        InvalidIndex < 3 ? TEXT("finite number") : TEXT("finite number >= 0"));
                                    return false;
                                  }
                                  FString Label(TEXT("UAI_WP_Region"));
                                  Parameters->TryGetStringField(TEXT("label"),
                                                                Label);
                                  if (Label.TrimStartAndEnd().IsEmpty()) {
                                    OutError = InputFailure(TEXT("world_partition_region_label_invalid"),
                                        TEXT("The region label must not be blank."), TEXT("/parameters/label"),
                                        FindField(*Request.GetParameters(), TEXT("label")), TEXT("nonblank label"));
                                    return false;
                                  }
                                  if (!bDryRun) {
                                    WorldPartitionLoader =
                                        MakeUnique<FLoaderAdapterShape>(
                                            ExplicitWorld,
                                            FBox::BuildAABB(CenterValue,
                                                            ExtentValue),
                                            Label);
                                  }
                                } else if (PolicyId ==
                                           TEXT("policy:world-context:"
                                                "partition-actor-loader/v1")) {
                                  if (!TryOnlyFields(*Request.GetParameters(), {TEXT("actor_guids"), TEXT("actor_ids"), TEXT("dry_run")},
                                      OutError, TEXT("world_partition_actor_loader_input_invalid"))) return false;
                                  TArray<FGuid> Guids;
                                  const TArray<TSharedPtr<FJsonValue>>
                                      *GuidValues = nullptr;
                                  if (Parameters->TryGetArrayField(
                                          TEXT("actor_guids"), GuidValues) &&
                                      GuidValues) {
                                    for (int32 Index = 0; Index < GuidValues->Num(); ++Index) {
                                      const TSharedPtr<FJsonValue>& Value = (*GuidValues)[Index];
                                      FGuid Guid;
                                      if (!Value.IsValid() ||
                                          !FGuid::Parse(Value->AsString(),
                                                        Guid)) {
                                        OutError = FactFailure(TEXT("world_partition_actor_guid_invalid"),
                                            TEXT("The actor GUID cannot be parsed."), TEXT("/parameters/actor_guids/") + FString::FromInt(Index),
                                            Value, MakeShared<FJsonValueString>(TEXT("parseable GUID string")));
                                        OutError.ActualValuePresent = Value.IsValid();
                                        OutError.ExpectedValuePresent = true;
                                        return false;
                                      }
                                      Guids.AddUnique(Guid);
                                    }
                                  }
                                  const TArray<TSharedPtr<FJsonValue>>
                                      *ActorIds = nullptr;
                                  if (Parameters->TryGetArrayField(
                                          TEXT("actor_ids"), ActorIds) &&
                                      ActorIds) {
                                    for (int32 Index = 0; Index < ActorIds->Num(); ++Index) {
                                      const TSharedPtr<FJsonValue>& Value = (*ActorIds)[Index];
                                      const FString Identity =
                                          Value.IsValid() ? Value->AsString()
                                                          : FString();
                                      AActor *Match = nullptr;
                                      for (TActorIterator<AActor> It(
                                               ExplicitWorld);
                                           It; ++It) {
                                        if (It->GetPathName() == Identity ||
                                            It->GetName() == Identity ||
                                            It->GetActorLabel() == Identity) {
                                          Match = *It;
                                          break;
                                        }
                                      }
                                      if (!Match) {
                                        OutError = FactFailure(TEXT("world_partition_actor_identity_unresolved"),
                                            TEXT("No actor in the current world matches this path, name or label."),
                                            TEXT("/parameters/actor_ids/") + FString::FromInt(Index), Value,
                                            MakeShared<FJsonValueString>(TEXT("actor path, name or label in the current world")), TEXT("command_input"), Request.GetSubjectRef());
                                        OutError.ActualValuePresent = Value.IsValid();
                                        OutError.ExpectedValuePresent = true;
                                        return false;
                                      }
                                      Guids.AddUnique(Match->GetActorGuid());
                                    }
                                  }
                                  if (Guids.IsEmpty()) {
                                    const auto Actual = MakeShared<FJsonObject>();
                                    for (const TCHAR* Field : {TEXT("actor_guids"), TEXT("actor_ids")})
                                    {
                                        const auto* Input = FindField(*Request.GetParameters(), Field);
                                        Actual->SetBoolField(FString(Field) + TEXT("_present"), Input != nullptr);
                                        if (Input) Actual->SetField(Field, ValueFact(Input));
                                    }
                                    Actual->SetNumberField(TEXT("resolved_actor_count"), Guids.Num());
                                    OutError = FactFailure(TEXT("world_partition_actor_loader_empty"),
                                        TEXT("The actor loader resolved no actors from actor_guids or actor_ids."), TEXT("/parameters"),
                                        MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("at least one actor GUID or resolved actor identity")));
                                    return false;
                                  }
                                  if (!bDryRun) {
                                    TUniquePtr<FLoaderAdapterActorList>
                                        ActorLoader =
                                            MakeUnique<FLoaderAdapterActorList>(
                                                ExplicitWorld);
                                    ActorLoader->AddActors(Guids);
                                    WorldPartitionLoader =
                                        MoveTemp(ActorLoader);
                                  }
                                } else {
                                  OutError = FactFailure(TEXT("world_context_policy_not_supported"),
                                      TEXT("The world context policy is not supported."), TEXT("/policy_id"),
                                      MakeShared<FJsonValueString>(PolicyId), StringValues({TEXT("policy:world-context:set-current/v1"),
                                          TEXT("policy:world-context:create-blank/v1"), TEXT("policy:world-context:partition-region-loader/v1"),
                                          TEXT("policy:world-context:partition-actor-loader/v1")}));
                                  return false;
                                }

                                if (bDryRun)
				{
					OutSessionRef = TEXT("world-partition-loader:planned:")
						+ WorldPackage;
					return true;
				}
				WorldPartitionLoader->SetUserCreated(true);
				WorldPartitionLoader->Load();
				const bool bLoaded = WorldPartitionLoader->IsLoaded();
				OutSessionRef = FString::Printf(
					TEXT("world-partition-loader:%s:loaded=%s"),
					*NativeResourceId, bLoaded ? TEXT("true") : TEXT("false"));
				bOutKeepRunning = true;
				return true;
			}

			bool StartOperationOwnedDynamicMesh(
				FString& OutSessionRef,
				UE::UAI::Contract::FDiagnostic& OutError)
			{
				const FUeAgentCanonicalValue& Parameters =
					Request.GetParameters().Get();
				if (Request.GetSubjectRef() != TEXT("modeling-session:operation-local"))
					return Fail(OutError, FactFailure(TEXT("modeling_session_policy_input_invalid"),
						TEXT("The modeling policy requires an operation-local session subject."), TEXT("/subject_ref"),
						MakeShared<FJsonValueString>(Request.GetSubjectRef()), MakeShared<FJsonValueString>(TEXT("modeling-session:operation-local"))));
				if (StringField(Parameters, TEXT("action_id")) != TEXT("spawn_transient_box"))
					return Fail(OutError, InputFailure(TEXT("modeling_session_policy_input_invalid"),
						TEXT("The modeling action is not supported."), TEXT("/parameters/action_id"),
						FindField(Parameters, TEXT("action_id")), TEXT("spawn_transient_box")));
				if (!TryOnlyFields(Parameters, {TEXT("action_id"), TEXT("size_x"), TEXT("size_y"), TEXT("size_z")},
					OutError, TEXT("modeling_session_policy_input_invalid"))) return false;
				const FVector3d Size(
					DecimalField(Parameters, TEXT("size_x"), 100.0),
					DecimalField(Parameters, TEXT("size_y"), 100.0),
					DecimalField(Parameters, TEXT("size_z"), 100.0));

				if (!FMath::IsFinite(Size.X) || !FMath::IsFinite(Size.Y)
					|| !FMath::IsFinite(Size.Z) || Size.X <= 0.0
					|| Size.Y <= 0.0 || Size.Z <= 0.0
					|| Size.X > 100000.0 || Size.Y > 100000.0
					|| Size.Z > 100000.0)
				{
					const int32 Index = !FMath::IsFinite(Size.X) || Size.X <= 0.0 || Size.X > 100000.0 ? 0
						: !FMath::IsFinite(Size.Y) || Size.Y <= 0.0 || Size.Y > 100000.0 ? 1 : 2;
					const FString Field = Index == 0 ? TEXT("size_x") : Index == 1 ? TEXT("size_y") : TEXT("size_z");
					OutError = InputFailure(TEXT("editor_dynamic_mesh_dimensions_invalid"),
						TEXT("The box dimension must be finite, positive and at most 100000."), TEXT("/parameters/") + Field,
						FindField(Parameters, Field), TEXT("finite number in (0, 100000]"));
					return false;
				}
				UWorld* ExplicitWorld = ResolveCurrentEditorWorld(World);
				if (!ExplicitWorld)
				{
					OutError = NativeFailure(TEXT("editor_dynamic_mesh_world_unavailable"),
						TEXT("No editor world is available for the operation-owned mesh."), TEXT("/editor_world/available"), Request.GetSubjectRef(), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return false;
				}
				FActorSpawnParameters SpawnParameters;
				SpawnParameters.ObjectFlags |= RF_Transient;
				SpawnParameters.SpawnCollisionHandlingOverride =
					ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
				ADynamicMeshActor* Actor = ExplicitWorld->SpawnActor<ADynamicMeshActor>(
					FVector::ZeroVector,
					FRotator::ZeroRotator,
					SpawnParameters);
				UDynamicMeshComponent* Component = Actor
					? Actor->GetDynamicMeshComponent() : nullptr;
				UDynamicMesh* DynamicMesh = Component
					? Component->GetDynamicMesh() : nullptr;
				if (!Actor || !Component || !DynamicMesh)
				{
					if (Actor)
					{
						ExplicitWorld->DestroyActor(Actor);
					}
					OutError = NativeFailure(TEXT("editor_dynamic_mesh_spawn_failed"),
						!Actor ? TEXT("The operation-owned dynamic mesh actor could not be spawned.")
							: !Component ? TEXT("The spawned actor has no dynamic mesh component.") : TEXT("The spawned component has no dynamic mesh."),
						!Actor ? TEXT("/actor/available") : !Component ? TEXT("/actor/component/available") : TEXT("/actor/component/mesh/available"),
						Request.GetSubjectRef(), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return false;
				}
				bool bMeshBuilt = true;
				DynamicMesh->EditMesh(
					[&Size, &bMeshBuilt](UE::Geometry::FDynamicMesh3& Mesh)
					{
						Mesh.Clear();
						bMeshBuilt = AppendBoxMesh(Mesh, Size);
					});
				if (!bMeshBuilt)
				{
					ExplicitWorld->DestroyActor(Actor);
					OutError = NativeFailure(TEXT("editor_dynamic_mesh_build_failed"),
						TEXT("The native box mesh generator failed."), TEXT("/mesh/build_succeeded"), Request.GetSubjectRef(), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return false;
				}
				Component->NotifyMeshUpdated();
				FString Label = NativeResourceId;
				Label.ReplaceInline(TEXT(":"), TEXT("_"));
				Actor->SetActorLabel(TEXT("UAI_") + Label);
				Actor->Tags.AddUnique(
					TEXT("UAI.OperationOwned.Modeling"));
				OwnedActor = Actor;
				OutSessionRef = Actor->GetPathName();
				return true;
			}

			void StartOnGameThread()
			{
				UE::UAI::Contract::FDiagnostic LocalError;
				FString LocalSessionRef;
				TSharedPtr<FJsonObject> LocalDetails;
				bool bKeepRunning = false;
				bool bSuccess = false;
				const auto Family = Request.GetFamily();
				if (Family == EditorSessionOperation::
					EUeAgentEditorSessionOperationFamily::AssetEditor)
				{
					const FString ObjectPath = StripSubjectPrefix(
						Request.GetSubjectRef(), TEXT("asset:"));
					UObject* Asset = ObjectPath.IsEmpty() ? nullptr
						: StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath);
					if (Request.GetPolicyId() == TEXT("policy:asset-editor:preview-action/v1"))
					{
						TSharedPtr<FJsonObject> Parameters;
						FExecutionMutationError ParseError;
						if (TryCanonicalObjectJson(*Request.GetParameters(), Parameters, ParseError))
						{
							FString Action, Target;
							Parameters->TryGetStringField(TEXT("action"), Action);
							Parameters->TryGetStringField(TEXT("target"), Target);
							double Time = 0;
							const bool bTime = Parameters->TryGetNumberField(TEXT("time_seconds"), Time);
							bSuccess = UE::UAI::AssetPreview::ExecuteAction(Asset, Target, Action,
								bTime ? TOptional<double>(Time) : TOptional<double>(), LocalDetails, LocalError);
							LocalSessionRef = TEXT("asset-preview:") + ObjectPath;
						}
						else LocalError = MoveTemp(ParseError);
					}
					else
					{
					UAssetEditorSubsystem* Subsystem = GEditor
						? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
					bSuccess = Asset && Subsystem
						&& UE::UAI::AssetPreview::OpenAssetEditor(Asset, true);
					if (bSuccess)
					{
						TSharedPtr<FJsonObject> Parameters;
						FExecutionMutationError ParseError;
						FString Ownership;
						if (TryCanonicalObjectJson(*Request.GetParameters(), Parameters, ParseError))
							Parameters->TryGetStringField(TEXT("ownership"), Ownership);
						const auto Lease = UE::UAI::AssetPreview::AcquireAssetEditorLease(Asset, Ownership == TEXT("adopt"));
						LocalDetails = Lease->Describe();
						{
							FScopeLock Lock(&Mutex);
							AssetEditorLease = Lease;
						}
						LocalSessionRef = TEXT("asset-editor:") + ObjectPath;
						bKeepRunning = true;
					}
					else
					{
						LocalError = NativeFailure(TEXT("asset_editor_open_failed"),
							!Asset ? TEXT("The requested asset could not be loaded.")
								: !Subsystem ? TEXT("The asset editor subsystem is unavailable.") : TEXT("The asset editor declined to open the asset."),
							!Asset ? TEXT("/asset/loaded") : !Subsystem ? TEXT("/asset_editor_subsystem/available") : TEXT("/asset_editor/open_succeeded"),
							Request.GetSubjectRef(), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					}
					}
				}
				else if (Family == EditorSessionOperation::
					EUeAgentEditorSessionOperationFamily::RuntimePreview)
				{
					if (Request.GetPolicyId()
						== TEXT("policy:runtime-session:culture-preview/v1"))
					{
						const FUeAgentCanonicalValue& Parameters =
							Request.GetParameters().Get();
						const FString Culture = StringField(
							Parameters, TEXT("culture"));
						const FString Scope = StringField(
							Parameters, TEXT("scope"), TEXT("session"));
						const bool bRestoreAfter = BooleanField(
							Parameters, TEXT("restore_after"), true);
						if (!TryOnlyFields(Parameters, {TEXT("culture"), TEXT("restore_after"), TEXT("scope")},
							LocalError, TEXT("culture_preview_policy_input_invalid")))
						{
							// Keep the parameter producer's exact field and value.
						}
						else if (Scope != TEXT("session")
							|| Request.GetSubjectRef() != TEXT("culture:") + Culture
							|| !FInternationalization::Get().GetCulture(Culture).IsValid())
						{
							if (Scope != TEXT("session"))
								LocalError = InputFailure(TEXT("culture_preview_policy_input_invalid"),
									TEXT("Culture preview requires session scope."), TEXT("/parameters/scope"),
									FindField(Parameters, TEXT("scope")), TEXT("session"));
							else if (Request.GetSubjectRef() != TEXT("culture:") + Culture)
								LocalError = FactFailure(TEXT("culture_preview_policy_input_invalid"),
									TEXT("The preview subject does not match the selected culture."), TEXT("/subject_ref"),
									MakeShared<FJsonValueString>(Request.GetSubjectRef()), MakeShared<FJsonValueString>(TEXT("culture:") + Culture));
							else
								LocalError = InputFailure(TEXT("culture_preview_policy_input_invalid"),
									TEXT("The selected culture is not available."), TEXT("/parameters/culture"),
									FindField(Parameters, TEXT("culture")), TEXT("registered culture"));
						}
						else
						{
							PreviousCulture = FInternationalization::Get()
								.GetCurrentCulture()->GetName();
							bSuccess = FInternationalization::Get()
								.SetCurrentCulture(Culture);
							if (!bSuccess)
							{
								LocalError = NativeFailure(TEXT("culture_preview_set_failed"),
									TEXT("The native culture setter failed to apply the preview culture."), TEXT("/current_culture"), Request.GetSubjectRef(),
									MakeShared<FJsonValueString>(FInternationalization::Get().GetCurrentCulture()->GetName()), MakeShared<FJsonValueString>(Culture));
							}
							else if (bRestoreAfter)
							{
								bSuccess = FInternationalization::Get()
									.SetCurrentCulture(PreviousCulture);
								if (!bSuccess) LocalError = NativeFailure(TEXT("culture_preview_restore_failed"),
									TEXT("The original culture could not be restored after preview."), TEXT("/current_culture"), Request.GetSubjectRef(),
									MakeShared<FJsonValueString>(FInternationalization::Get().GetCurrentCulture()->GetName()), MakeShared<FJsonValueString>(PreviousCulture));
							}
							else
							{
								bCulturePreviewActive = true;
								bKeepRunning = true;
							}
							LocalSessionRef = FString::Printf(
								TEXT("culture-preview:%s:%s:restored=%s"),
								*PreviousCulture,
								*Culture,
								bRestoreAfter ? TEXT("true") : TEXT("false"));
						}
					}
					else if (GUnrealEd && !GEditor->PlayWorld)
					{
						FRequestPlaySessionParams Params;
						Params.SessionDestination = EPlaySessionDestinationType::InProcess;
						Params.WorldType = EPlaySessionWorldType::PlayInEditor;
						GUnrealEd->RequestPlaySession(Params);
						bSuccess = true;
						bKeepRunning = true;
						LocalSessionRef = Request.GetSubjectRef();
					}
					else
					{
						LocalError = NativeFailure(TEXT("runtime_preview_already_active_or_unavailable"),
							!GUnrealEd ? TEXT("The editor is unavailable for runtime preview.") : TEXT("A play world is already active."),
							!GUnrealEd ? TEXT("/editor/available") : TEXT("/play_world/active"), Request.GetSubjectRef(),
							MakeShared<FJsonValueBoolean>(GUnrealEd != nullptr), MakeShared<FJsonValueBoolean>(GUnrealEd == nullptr));
					}
				}
				else if (Family == EditorSessionOperation::
					EUeAgentEditorSessionOperationFamily::WorldContext)
				{
					bSuccess = StartWorldContext(
						LocalSessionRef, bKeepRunning, LocalError);
				}
				else if (Family == EditorSessionOperation::
					EUeAgentEditorSessionOperationFamily::ModelingTool)
				{
					bSuccess = StartOperationOwnedDynamicMesh(
						LocalSessionRef, LocalError);
					bKeepRunning = bSuccess;
				}
				else if (Family == EditorSessionOperation::EUeAgentEditorSessionOperationFamily::PcgGeneration)
				{
					bool bCancelledBeforeStart;
					{
						FScopeLock Lock(&Mutex);
						bCancelledBeforeStart = bStopRequested;
					}
					if (bCancelledBeforeStart)
					{
						LocalError = NativeFailure(TEXT("pcg_generation_cancelled_before_start"),
							TEXT("PCG generation was cancelled before native work started."), TEXT("/cancelled"),
							Request.GetSubjectRef(), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
						LocalError.MutationStarted = false;
						LocalError.MutationMayHaveStarted = false;
					}
					else if (TryOnlyFields(Request.GetParameters().Get(), {}, LocalError, TEXT("pcg_generation_parameters_invalid")))
					{
						bSuccess = PcgGeneration::TryStart(CurrentEditorWorld(),
							StripSubjectPrefix(Request.GetSubjectRef(), TEXT("pcg-component:")), PcgWork, LocalError);
						if (bSuccess)
						{
							const auto Observation = PcgWork->Observe();
							LocalDetails = Observation.Details;
							LocalError = Observation.Diagnostic;
							LocalSessionRef = Request.GetSubjectRef();
							bKeepRunning = !Observation.bFinished;
							bSuccess = LocalError.IsEmpty();
							if (bKeepRunning)
							{
								const TWeakPtr<FEditorNativeSession, ESPMode::ThreadSafe> WeakSelf = AsShared();
								const auto Handle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
									[WeakSelf](float)
									{
										const auto Self = WeakSelf.Pin();
										return Self && !Self->PollPcgGeneration();
									}), 0.05f);
								FScopeLock Lock(&Mutex);
								StopPollTicker = Handle;
							}
						}
					}
				}
				else
				{
					const auto* FamilyContract = EditorSessionOperation::FindEditorSessionOperationFamilyContract(Family);
					LocalError = FamilyContract ? FactFailure(TEXT("editor_session_family_not_supported"),
						TEXT("This editor native session does not implement the requested family."), TEXT("/family_id"),
						MakeShared<FJsonValueString>(FamilyContract->GetFamilyId()),
						StringValues({TEXT("asset_editor"), TEXT("runtime_preview"), TEXT("world_context"), TEXT("modeling_tool"), TEXT("pcg_generation")}))
						: Request.GetValidationDiagnostic();
				}

				FScopeLock Lock(&Mutex);
				SessionRef = MoveTemp(LocalSessionRef);
				Details = MoveTemp(LocalDetails);
				StateDiagnostic = MoveTemp(LocalError);
				State = bSuccess
					? (bKeepRunning
						? EEditorNativeState::Running
						: EEditorNativeState::ExitedSuccess)
					: EEditorNativeState::ExitedFailure;
			}

			bool PollPcgGeneration()
			{
				const auto Observation = PcgWork->Observe();
				FScopeLock Lock(&Mutex);
				Details = Observation.Details;
				StateDiagnostic = Observation.Diagnostic;
				State = Observation.bFinished
					? (StateDiagnostic.IsEmpty() ? EEditorNativeState::ExitedSuccess : EEditorNativeState::ExitedFailure)
					: EEditorNativeState::Running;
				if (Observation.bFinished) StopPollTicker.Reset();
				return Observation.bFinished;
			}

			bool PollAssetEditorClose(bool bRequestClose)
			{
				UE::UAI::Contract::FDiagnostic Diagnostic;
				TSharedPtr<FJsonObject> CloseDetails;
				const bool bClosed = !AssetEditorLease || AssetEditorLease->Close(bRequestClose, Diagnostic, CloseDetails);
				FScopeLock Lock(&Mutex);
				StateDiagnostic = MoveTemp(Diagnostic);
				Details = MoveTemp(CloseDetails);
				bAssetEditorCleanupComplete = bClosed;
				bAssetEditorCleanupPending = !bClosed;
				State = bClosed ? EEditorNativeState::ExitedSuccess : EEditorNativeState::Running;
				return bClosed;
			}

			void StopOnGameThread()
			{
				const auto Family = Request.GetFamily();
				if (Family == EditorSessionOperation::EUeAgentEditorSessionOperationFamily::PcgGeneration)
				{
					// Successful output survives Stop/Join/Release. Cancel only the owned task.
					if (PcgWork) PcgWork->RequestCancel();
					// The existing observation ticker confirms native cancellation and detaches.
					return;
				}
				if (Family == EditorSessionOperation::
						EUeAgentEditorSessionOperationFamily::ModelingTool)
				{
					if (ADynamicMeshActor* Actor = OwnedActor.Get())
					{
						if (UWorld* ActorWorld = Actor->GetWorld())
						{
							ActorWorld->DestroyActor(Actor);
						}
					}
					OwnedActor.Reset();
				}
				else if (Family == EditorSessionOperation::
					EUeAgentEditorSessionOperationFamily::AssetEditor)
				{
					if (PollAssetEditorClose(true)) return;
					FScopeLock Lock(&Mutex);
					if (!StopPollTicker.IsValid())
					{
						const TWeakPtr<FEditorNativeSession, ESPMode::ThreadSafe> WeakSelf = AsShared();
						StopPollTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
							[WeakSelf](float)
							{
								const auto Self = WeakSelf.Pin();
								if (!Self) return false;
								if (!Self->PollAssetEditorClose(false)) return true;
								FScopeLock StateLock(&Self->Mutex);
								Self->StopPollTicker.Reset();
								return false;
							}), 0.1f);
					}
					return;
				}
				else if (Family == EditorSessionOperation::
					EUeAgentEditorSessionOperationFamily::RuntimePreview)
				{
					if (Request.GetPolicyId()
						== TEXT("policy:runtime-session:culture-preview/v1"))
					{
						const bool bRestored = !bCulturePreviewActive
							|| FInternationalization::Get()
								.SetCurrentCulture(PreviousCulture);
						bCulturePreviewActive = false;
						FScopeLock Lock(&Mutex);
						if (!bRestored) StateDiagnostic.Append(NativeFailure(TEXT("culture_preview_restore_failed"),
							TEXT("The original culture could not be restored while stopping preview."), TEXT("/current_culture"), Request.GetSubjectRef(),
							MakeShared<FJsonValueString>(FInternationalization::Get().GetCurrentCulture()->GetName()), MakeShared<FJsonValueString>(PreviousCulture)));
						State = StateDiagnostic.IsEmpty() ? EEditorNativeState::ExitedSuccess : EEditorNativeState::ExitedFailure;
						return;
					}
					if (!GUnrealEd)
					{
						FScopeLock Lock(&Mutex);
						State = EEditorNativeState::ExitedFailure;
						StateDiagnostic.Append(NativeFailure(TEXT("runtime_preview_stop_unavailable"),
							TEXT("The editor is unavailable to request the end of runtime preview."), TEXT("/editor/available"), Request.GetSubjectRef(),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
						return;
					}
					GUnrealEd->RequestEndPlayMap();
					const TWeakPtr<FEditorNativeSession, ESPMode::ThreadSafe>
						WeakSelf = AsShared();
					const FTSTicker::FDelegateHandle Handle =
						FTSTicker::GetCoreTicker().AddTicker(
							FTickerDelegate::CreateLambda(
								[WeakSelf](float)
								{
									const TSharedPtr<
										FEditorNativeSession,
										ESPMode::ThreadSafe> Self = WeakSelf.Pin();
									if (!Self.IsValid())
									{
										return false;
									}
									if (GEditor && GEditor->PlayWorld)
									{
										return true;
									}
									FScopeLock Lock(&Self->Mutex);
									Self->StopPollTicker.Reset();
									Self->State = Self->StateDiagnostic.IsEmpty() ? EEditorNativeState::ExitedSuccess : EEditorNativeState::ExitedFailure;
									return false;
								}),
							0.05f);
					FScopeLock Lock(&Mutex);
					if (!Handle.IsValid())
					{
						State = EEditorNativeState::ExitedFailure;
						StateDiagnostic.Append(NativeFailure(TEXT("runtime_preview_stop_poll_schedule_failed"),
							TEXT("The ticker could not schedule runtime preview stop observation."), TEXT("/stop_poll/scheduled"), Request.GetSubjectRef(),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
					}
					else
					{
						StopPollTicker = Handle;
					}
					return;
				}
				else if (Family == EditorSessionOperation::
					EUeAgentEditorSessionOperationFamily::WorldContext)
				{
					if (WorldPartitionLoader.IsValid())
					{
						WorldPartitionLoader->Unload();
						WorldPartitionLoader.Reset();
					}
				}
				FScopeLock Lock(&Mutex);
				State = StateDiagnostic.IsEmpty() ? EEditorNativeState::ExitedSuccess : EEditorNativeState::ExitedFailure;
			}

			EditorSessionOperation::FUeAgentEditorSessionOperationTypedRequest Request;
			TWeakObjectPtr<UWorld> World;
			TSharedPtr<UE::UAI::AssetPreview::IAssetEditorLease, ESPMode::ThreadSafe> AssetEditorLease;
			TSharedPtr<PcgGeneration::IGenerationWork, ESPMode::ThreadSafe> PcgWork;
			TWeakObjectPtr<ADynamicMeshActor> OwnedActor;
			TUniquePtr<IWorldPartitionActorLoaderInterface::ILoaderAdapter>
				WorldPartitionLoader;
			FCriticalSection Mutex;
			FString NativeResourceId;
			FString SessionRef;
			TSharedPtr<FJsonObject> Details;
			UE::UAI::Contract::FDiagnostic StateDiagnostic;
			FString PreviousCulture;
			FTSTicker::FDelegateHandle StopPollTicker;
			EEditorNativeState State = EEditorNativeState::Scheduled;
			bool bStarted = false;
			bool bStopRequested = false;
			bool bAssetEditorCleanupPending = false;
			bool bAssetEditorCleanupComplete = false;
			bool bReleased = false;
			bool bCulturePreviewActive = false;
		};

		class FEditorSessionBuilder final
			: public EditorSessionOperation::IUeAgentEditorSessionOperationPortBuilder
		{
		public:
			FEditorSessionBuilder(
				FString InEditorInstanceId,
				TWeakObjectPtr<UWorld> InWorld,
				TSharedRef<PieRuntimeDebug::FUeAgentPieRuntimeDebugRegistry,
					ESPMode::ThreadSafe> InPieRuntimeDebugRegistry)
				: EditorInstanceId(MoveTemp(InEditorInstanceId))
				, World(InWorld)
				, PieRuntimeDebugRegistry(MoveTemp(InPieRuntimeDebugRegistry))
			{
			}

			bool TryCreatePort(
				const EditorSessionOperation::
					FUeAgentEditorSessionOperationTypedRequest& Request,
				const FLongRunningOperationRequest& OperationRequest,
				const EditorSessionOperation::
					FUeAgentEditorSessionOperationPolicyDescriptor& Policy,
				TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe>& OutPort,
				FExecutionMutationError& OutError) override
			{
				if (Request.GetFamily() == EditorSessionOperation::
					EUeAgentEditorSessionOperationFamily::HostLifecycle)
				{
					FUeAgentSafeExitExternalWitnessRef WitnessRef;
					if (!TryReadSafeExitExternalWitnessRef(
							Request.GetParameters().Get(),
							WitnessRef,
							OutError))
					{
						return false;
					}
					FUeAgentValidatedSafeExitWitness Witness;
					if (!TryValidateSafeExitExternalWitness(
							WitnessRef,
							Witness,
							OutError))
					{
						return false;
					}
					FUeAgentSafeExitDirtyResolutionReceipt DirtyResolution;
					if (!TryResolveSafeExitDirtyResources(
							Request.GetParameters().Get(),
							DirtyResolution,
							OutError))
					{
						return false;
					}
					TSharedPtr<
						EditorSessionOperation::IUeAgentEditorHostExitTarget,
						ESPMode::ThreadSafe> ExitTarget;
					if (!TryCreateDurableSafeExitTarget(
							Witness,
							DirtyResolution,
							OperationRequest,
							EditorInstanceId,
							ExitTarget,
							OutError)
						|| !ExitTarget.IsValid())
					{
						return false;
					}
					return EditorSessionOperation::
						TryCreateEditorHostExitSchedulePort(
							Request,
							Policy,
							0.25,
							ExitTarget.ToSharedRef(),
							OutPort,
							OutError);
				}
				if (Request.GetPolicyId()
					== TEXT("policy:runtime-session:pie-debug/v2"))
				{
					TSharedPtr<EditorSessionOperation::
						IUeAgentEditorSessionNativeSession,
						ESPMode::ThreadSafe> NativeSession;
					if (!PieRuntimeDebug::TryCreatePieRuntimeDebugEditorSession(
							Request,
							PieRuntimeDebugRegistry,
							NativeSession,
							OutError)
						|| !NativeSession.IsValid())
					{
						return false;
					}
					return EditorSessionOperation::
						TryCreateEditorSessionNativeResourcePort(
							Request,
							Policy,
							NativeSession.ToSharedRef(),
							OutPort,
							OutError);
				}
				return EditorSessionOperation::
					TryCreateEditorSessionNativeResourcePort(
						Request,
						Policy,
						MakeShared<FEditorNativeSession, ESPMode::ThreadSafe>(
							Request, World),
						OutPort,
						OutError);
			}

		private:
			FString EditorInstanceId;
			TWeakObjectPtr<UWorld> World;
			TSharedRef<PieRuntimeDebug::FUeAgentPieRuntimeDebugRegistry,
				ESPMode::ThreadSafe> PieRuntimeDebugRegistry;
		};

		// Each capture subscribes to the native mix without resetting UE's shared
		// RecordingData or another capture. The listener also receives silent buffers.
		class FMediaRecordingBuffer final : public ISubmixBufferListener
		{
		public:
			explicit FMediaRecordingBuffer(double InDuration) : Duration(InDuration) {}
			bool IsRenderingAudio() const override { return true; }
			void OnNewSubmixBuffer(const USoundSubmix*, float* Data, int32 Count,
				int32 InChannels, int32 InRate, double) override
			{
				FScopeLock Lock(&Mutex);
				if (!Error.IsEmpty() || Count == 0 || (TargetSamples > 0 && Samples.Num() == TargetSamples)) return;
				if (!Data || Count < 0 || InChannels <= 0 || InRate <= 0 || Count % InChannels != 0)
				{
					auto Actual = MakeShared<FJsonObject>();
					Actual->SetBoolField(TEXT("data_present"), Data != nullptr);
					Actual->SetNumberField(TEXT("sample_count"), Count);
					Actual->SetNumberField(TEXT("channel_count"), InChannels);
					Actual->SetNumberField(TEXT("sample_rate_hz"), InRate);
					Error = NativeFailure(TEXT("capture_media_sample_format_invalid"), TEXT("The audio callback supplied missing sample storage, invalid channel/rate values, or a sample count not divisible by the channel count."), TEXT("audio_samples"), FString(), MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("sample storage; nonnegative sample count divisible by positive channel count; positive sample rate")), TEXT("capture_recording"));
					return;
				}
				if (Channels && (Channels != InChannels || Rate != InRate))
				{
					auto Actual = MakeShared<FJsonObject>();
					Actual->SetNumberField(TEXT("channel_count"), InChannels);
					Actual->SetNumberField(TEXT("sample_rate_hz"), InRate);
					auto Expected = MakeShared<FJsonObject>();
					Expected->SetNumberField(TEXT("channel_count"), Channels);
					Expected->SetNumberField(TEXT("sample_rate_hz"), Rate);
					Error = NativeFailure(TEXT("capture_media_sample_format_changed"), TEXT("The audio channel count or sample rate changed during recording."), TEXT("audio_format"), FString(), MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueObject>(Expected), TEXT("capture_recording"));
					return;
				}
				const double Required = FMath::CeilToDouble(Duration * InRate) * InChannels;
				if (!FMath::IsFinite(Required) || Required < 1 || Required > MAX_int32)
				{
					auto Actual = MakeShared<FJsonObject>();
					if (FMath::IsFinite(Duration)) Actual->SetNumberField(TEXT("duration_seconds"), Duration);
					else Actual->SetStringField(TEXT("duration_seconds"), TEXT("nonfinite"));
					Actual->SetNumberField(TEXT("channel_count"), InChannels);
					Actual->SetNumberField(TEXT("sample_rate_hz"), InRate);
					if (FMath::IsFinite(Required)) Actual->SetNumberField(TEXT("required_samples"), Required);
					else Actual->SetStringField(TEXT("required_samples"), TEXT("nonfinite"));
					Error = NativeFailure(TEXT("capture_media_sample_buffer_overflow"), TEXT("The requested recording duration and audio format require an unsupported sample buffer size."), TEXT("audio_samples"), FString(), MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("required_samples between 1 and MAX_int32")), TEXT("capture_recording"));
					return;
				}
				Channels = InChannels;
				Rate = InRate;
				TargetSamples = static_cast<int32>(Required);
				Samples.Append(Data, FMath::Min(Count, TargetSamples - Samples.Num()));
			}
			bool IsComplete()
			{
				FScopeLock Lock(&Mutex);
				return !Error.IsEmpty() || (TargetSamples > 0 && Samples.Num() == TargetSamples);
			}

			bool Take(Audio::FAlignedFloatBuffer& Out, int32& OutChannels, int32& OutRate, UE::UAI::Contract::FDiagnostic& OutError)
			{
				FScopeLock Lock(&Mutex);
				Out = MoveTemp(Samples);
				OutChannels = Channels;
				OutRate = Rate;
				OutError = Error;
				return Error.IsEmpty();
			}
		private:
			FCriticalSection Mutex;
			Audio::FAlignedFloatBuffer Samples;
			int32 Channels = 0, Rate = 0;
			int32 TargetSamples = 0;
			double Duration;
			UE::UAI::Contract::FDiagnostic Error;
		};

		enum class ECaptureNativeState : uint8
		{
			Scheduled,
			Running,
			NativeReady,
			ExitedSuccess,
			ExitedFailure,
		};

		class FCaptureNativeSession final
			: public CaptureMediaLongJob::IUeAgentCaptureMediaNativeSession
			, public TSharedFromThis<FCaptureNativeSession, ESPMode::ThreadSafe>
		{
		public:
			FCaptureNativeSession(
				CaptureMediaLongJob::FUeAgentCaptureMediaLongJobTypedRequest
					InRequest,
				TWeakObjectPtr<UWorld> InWorld,
				FString InPublisherOwnerId,
				TSharedRef<CaptureMediaLongJob::IUeAgentCaptureImageArtifactService,
					ESPMode::ThreadSafe> InImageArtifactService)
				: Request(MoveTemp(InRequest))
				, World(InWorld)
				, PublisherOwnerId(MoveTemp(InPublisherOwnerId))
				, ImageArtifactService(InImageArtifactService)
			{
			}

			~FCaptureNativeSession() override
			{
				FTSTicker::FDelegateHandle StopHandle;
				FTSTicker::FDelegateHandle ExportHandle;
				FTSTicker::FDelegateHandle SequenceHandle;
				TSharedPtr<CaptureMediaLongJob::FLevelViewportSequenceCapture>
					SequenceCapture;
				{
					FScopeLock Lock(&Mutex);
					StopHandle = MediaStopTicker;
					MediaStopTicker.Reset();
					ExportHandle = MediaWriteTicker;
					MediaWriteTicker.Reset();
					SequenceHandle = ViewportSequenceTicker;
					ViewportSequenceTicker.Reset();
					SequenceCapture = MoveTemp(ViewportSequenceCapture);
				}
				if (StopHandle.IsValid())
				{
					FTSTicker::GetCoreTicker().RemoveTicker(StopHandle);
				}
				if (ExportHandle.IsValid())
				{
					FTSTicker::GetCoreTicker().RemoveTicker(ExportHandle);
				}
				if (SequenceHandle.IsValid())
				{
					FTSTicker::GetCoreTicker().RemoveTicker(SequenceHandle);
				}
				if (SequenceCapture.IsValid())
				{
					AsyncTask(ENamedThreads::GameThread,
						[SequenceCapture]() { SequenceCapture->Cancel(); });
				}
				if (RecordingSubmix.IsValid())
				{
					const auto NativeRecording = MoveTemp(RecordingSubmix);
					const auto Buffer = MoveTemp(RecordingBuffer);
					AsyncTask(ENamedThreads::GameThread, [NativeRecording, Buffer]()
					{
						NativeRecording->UnregisterBufferListener(Buffer.ToSharedRef());
					});
				}
			}

			bool Start(
				const FLongRunningOperationRequest& OperationRequest,
				const FUeAgentCancellationToken& Cancellation,
				FString& OutNativeResourceId,
				FExecutionMutationError& OutError) override
			{
				FScopeLock Lock(&Mutex);
				if (bStarted)
				{
					return Fail(OutError, NativeFailure(TEXT("capture_native_session_already_started"), TEXT("The capture session has already been started."), TEXT("started"), Request.GetSubjectRef(), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), TEXT("execution")));
				}
				if (Cancellation.IsCancelled())
				{
					return Fail(OutError, NativeFailure(TEXT("capture_native_session_cancelled_before_start"), TEXT("The cancellation token was already cancelled before capture start."), TEXT("cancelled"), Request.GetSubjectRef(), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), TEXT("execution")));
				}
				bStarted = true;
				OperationId = OperationRequest.GetOperationId();
				Resources = &OperationRequest.GetResources();
				NativeResourceId = TEXT("capture-native:") + OperationId;
				OutNativeResourceId = NativeResourceId;
				const TSharedRef<FCaptureNativeSession, ESPMode::ThreadSafe> Self =
					StaticCastSharedRef<FCaptureNativeSession>(AsShared());
				AsyncTask(ENamedThreads::GameThread, [Self]() { Self->StartOnGameThread(); });
				return true;
			}

			bool Observe(
				CaptureMediaLongJob::FUeAgentCaptureMediaNativeObservation&
					OutObservation,
				FExecutionMutationError& OutError) override
			{
				FScopeLock Lock(&Mutex);
				if (!bStarted)
				{
					return Fail(OutError, NativeFailure(TEXT("capture_native_session_not_started"), TEXT("The capture session cannot be observed before it is started."), TEXT("started"), Request.GetSubjectRef(), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("execution")));
				}
				if (bSequenceCleanupPending)
				{
					OutObservation = CaptureMediaLongJob::FUeAgentCaptureMediaNativeObservation(ELongRunningWorkState::Running,
		TEXT("stopping"),
		UE::UAI::Contract::FDiagnostic(),
		{},
		{},
		CaptureReceipt);
					return true;
				}
				if (State == ECaptureNativeState::NativeReady && !Artifact.IsSet())
				{
					if (!Resources)
					{
						return Fail(OutError, NativeFailure(TEXT("capture_resource_tracker_unavailable"), TEXT("The capture session has no resource tracker for artifact publication."), TEXT("resource_tracker_available"), Request.GetSubjectRef(), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("execution")));
					}
					FFileArtifactReference Published;
					if (Request.GetFamily()
						== CaptureMediaLongJob::EUeAgentCaptureMediaLongJobFamily::ImageCapture)
					{
						FString EvidenceId;
						if (!StagedImage.IsSet())
						{
							return Fail(OutError, NativeFailure(TEXT("capture_staged_image_missing"), TEXT("The ready image capture has no staged image to publish."), TEXT("staged_image_present"), Request.GetSubjectRef(), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("capture_publish")));
						}
						if (!ImageArtifactService->TryPublishVerified(
								OperationId,
								PublisherOwnerId,
								StagedImage.GetValue(),
								*Resources,
								Published,
								EvidenceId,
								OutError))
						{
							return false;
						}
						TArray<FFileArtifactReference> PublishedAdditional;
						PublishedAdditional.Reserve(
							AdditionalStagedImages.Num());
						for (const CaptureMediaLongJob::FUeAgentStagedCaptureImage&
							AdditionalImage : AdditionalStagedImages)
						{
							FFileArtifactReference AdditionalPublished;
							FString AdditionalEvidenceId;
							if (!ImageArtifactService->TryPublishVerified(
									OperationId,
									PublisherOwnerId,
									AdditionalImage,
									*Resources,
									AdditionalPublished,
									AdditionalEvidenceId,
									OutError))
							{
							// Publish the failure with the artifacts already completed;
							// release only owns staging, not these retained results.
							StateDiagnostic.Append(OutError);
							if (!CaptureReceipt.IsValid()) CaptureReceipt = MakeShared<FJsonObject>();
							CaptureReceipt->SetObjectField(TEXT("publication_error"), StateDiagnostic.ToPublicJson());
							CaptureReceipt->SetBoolField(TEXT("partial"), true);
							OutError = FExecutionMutationError();
							State = ECaptureNativeState::ExitedFailure;
							break;
							}
							PublishedAdditional.Add(
								MoveTemp(AdditionalPublished));
						}
						AdditionalArtifacts = MoveTemp(PublishedAdditional);
					}
					else
					{
						FFileArtifactPublishError PublishError;
						if (!TryPublishFileArtifact(
							FFileArtifactPublishRequest(
								NativePath,
								FinalPath,
								PublisherOwnerId,
								ArtifactKind,
								Format,
								MimeType),
							*Resources,
							Published,
							PublishError))
						{
							return Fail(OutError, PublishError);
						}
					}
					Artifact = MoveTemp(Published);
					if (State != ECaptureNativeState::ExitedFailure) State = ECaptureNativeState::ExitedSuccess;
				}
				ELongRunningWorkState WorkState = ELongRunningWorkState::Running;
				FString Phase = TEXT("scheduled");
				if (State == ECaptureNativeState::Running)
				{
					Phase = TEXT("capturing");
				}
				else if (State == ECaptureNativeState::NativeReady)
				{
					Phase = TEXT("artifact_publish_pending");
				}
				else if (State == ECaptureNativeState::ExitedSuccess)
				{
					WorkState = ELongRunningWorkState::ExitedSuccess;
					Phase = TEXT("artifact_ready");
				}
				else if (State == ECaptureNativeState::ExitedFailure)
				{
					WorkState = ELongRunningWorkState::ExitedFailure;
					Phase = TEXT("failed");
				}
					OutObservation = CaptureMediaLongJob::
					FUeAgentCaptureMediaNativeObservation(
						WorkState,
						MoveTemp(Phase),
						StateDiagnostic,
						Artifact,
						AdditionalArtifacts,
						CaptureReceipt);
				return true;
			}

			bool RequestStop(
				const FString& Reason,
				FExecutionMutationError& OutError) override
			{
				FTSTicker::FDelegateHandle Handle;
				FTSTicker::FDelegateHandle SequenceHandle;
				TSharedPtr<CaptureMediaLongJob::FLevelViewportSequenceCapture>
					SequenceCapture;
				{
					FScopeLock Lock(&Mutex);
					bStopRequested = true;
					Handle = MediaStopTicker;
					MediaStopTicker.Reset();
					SequenceHandle = ViewportSequenceTicker;
					ViewportSequenceTicker.Reset();
					SequenceCapture = MoveTemp(ViewportSequenceCapture);
					if (SequenceCapture.IsValid())
					{
						bSequenceCleanupPending = true;
					}
				}
				if (Handle.IsValid())
				{
					FTSTicker::GetCoreTicker().RemoveTicker(Handle);
				}
				if (SequenceHandle.IsValid())
				{
					FTSTicker::GetCoreTicker().RemoveTicker(SequenceHandle);
				}
				if (SequenceCapture.IsValid())
				{
					const auto Self = StaticCastSharedRef<FCaptureNativeSession>(AsShared());
					AsyncTask(
						ENamedThreads::GameThread,
						[Self, SequenceCapture]()
						{
							SequenceCapture->Cancel();
							FScopeLock Lock(&Self->Mutex);
							Self->bSequenceCleanupPending = false;
							Self->StateDiagnostic.Append(NativeFailure(TEXT("capture_cancelled"), TEXT("The viewport sequence capture was cancelled."), TEXT("stop_requested"), Self->Request.GetSubjectRef(), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), TEXT("native_session")));
							Self->State = ECaptureNativeState::ExitedFailure;
						});
				}
				if (Request.GetFamily()
					== CaptureMediaLongJob::EUeAgentCaptureMediaLongJobFamily::
						MediaRecording)
				{
					const TSharedRef<FCaptureNativeSession, ESPMode::ThreadSafe> Self =
						StaticCastSharedRef<FCaptureNativeSession>(AsShared());
					AsyncTask(
						ENamedThreads::GameThread,
						[Self]() { Self->StopMediaOnGameThread(); });
				}
				return true;
			}

			bool Join(FExecutionMutationError& OutError) override
			{
				FScopeLock Lock(&Mutex);
				return State == ECaptureNativeState::ExitedSuccess
					|| State == ECaptureNativeState::ExitedFailure
					? true
					: Fail(OutError, NativeFailure(TEXT("capture_native_join_before_exit"), TEXT("The capture session cannot be joined before reaching a terminal state."), TEXT("capture_state"), Request.GetSubjectRef(), MakeShared<FJsonValueNumber>(static_cast<int32>(State)), MakeShared<FJsonValueString>(TEXT("ExitedSuccess or ExitedFailure")), TEXT("execution")));
			}

			bool Release(FExecutionMutationError& OutError) override
			{
				FTSTicker::FDelegateHandle SequenceHandle;
				TArray<FString> FailedPaths;
				TSharedPtr<CaptureMediaLongJob::FLevelViewportSequenceCapture>
					SequenceCapture;
				{
					FScopeLock Lock(&Mutex);
					SequenceHandle = ViewportSequenceTicker;
					ViewportSequenceTicker.Reset();
					SequenceCapture = MoveTemp(ViewportSequenceCapture);
				}
				if (SequenceHandle.IsValid())
				{
					FTSTicker::GetCoreTicker().RemoveTicker(SequenceHandle);
				}
				if (SequenceCapture.IsValid())
				{
					if (IsInGameThread())
					{
						SequenceCapture->Cancel();
					}
					else
					{
						FEvent* Completed = FPlatformProcess::GetSynchEventFromPool(true);
						AsyncTask(ENamedThreads::GameThread, [SequenceCapture, Completed]()
						{
							SequenceCapture->Cancel();
							Completed->Trigger();
						});
						Completed->Wait();
						FPlatformProcess::ReturnSynchEventToPool(Completed);
					}
				}
				{
					FScopeLock Lock(&Mutex);
					if (!CaptureMediaLongJob::DiscardProducedCaptureFile(NativePath)) FailedPaths.AddUnique(NativePath);
					for (const FString& Path : OwnedProducedImagePaths)
						if (!CaptureMediaLongJob::DiscardProducedCaptureFile(Path)) FailedPaths.AddUnique(Path);
					for (const CaptureMediaLongJob::FUeAgentStagedCaptureImage& Image
						: AdditionalStagedImages)
					{
						if (!ImageArtifactService->DiscardStaged(Image)) FailedPaths.AddUnique(Image.GetNativePath());
					}
					if (!NativeRoot.IsEmpty() && IFileManager::Get().DirectoryExists(*NativeRoot))
					{
						if (!IFileManager::Get().DeleteDirectory(*NativeRoot, false, false)) FailedPaths.AddUnique(NativeRoot);
					}
					bReleased = FailedPaths.IsEmpty();
				}
				if (FailedPaths.IsEmpty()) return true;
				auto Error = NativeFailure(TEXT("capture_native_cleanup_failed"), TEXT("One or more temporary capture artifacts could not be removed."), TEXT("cleanup_failed_path_count"), Request.GetSubjectRef(), MakeShared<FJsonValueNumber>(FailedPaths.Num()), MakeShared<FJsonValueNumber>(0), TEXT("execution"));
				Error.ActualEvidence = StringValues(FailedPaths);
				return Fail(OutError, MoveTemp(Error));
			}

		private:
			void FailOnGameThread(UE::UAI::Contract::FDiagnostic Diagnostic)
			{
				FScopeLock Lock(&Mutex);
				StateDiagnostic.Append(Diagnostic);
				State = ECaptureNativeState::ExitedFailure;
			}

			bool AdoptImageExecutionResultOnGameThread(
				CaptureMediaLongJob::FUeAgentCaptureMediaImageExecutionResult
					ExecutionResult)
			{
				NativePath = ExecutionResult.ProducedPath;
				OwnedProducedImagePaths = ExecutionResult.AdditionalProducedPaths;
				Format = ExecutionResult.Format;
				CaptureMediaLongJob::FUeAgentStagedCaptureImage Image;
				FExecutionMutationError ImageArtifactError;
				const auto DiscardProducedImages = [&]()
				{
					// Keep the owned paths until Release independently confirms cleanup.
					CaptureMediaLongJob::DiscardProducedCaptureFile(ExecutionResult.ProducedPath);
					for (const FString& Path : ExecutionResult.AdditionalProducedPaths)
					{
						CaptureMediaLongJob::DiscardProducedCaptureFile(Path);
					}
					AdditionalStagedImages.Reset();
				};
				if (!ImageArtifactService->TryAdoptStagedImage(
						OperationId,
						Request.GetSubjectRef(),
						NativePath,
						ExecutionResult.RequestedFinalPath,
						Format,
						Image,
						ImageArtifactError))
				{
					DiscardProducedImages();
					FailOnGameThread(ImageArtifactError);
					return false;
				}
				for (const FString& AdditionalPath
					: ExecutionResult.AdditionalProducedPaths)
				{
					CaptureMediaLongJob::FUeAgentStagedCaptureImage AdditionalImage;
					if (!ImageArtifactService->TryAdoptStagedImage(
							OperationId,
							Request.GetSubjectRef(),
							AdditionalPath,
							FString(),
							Format,
							AdditionalImage,
							ImageArtifactError))
					{
						DiscardProducedImages();
						FailOnGameThread(ImageArtifactError);
						return false;
					}
					AdditionalStagedImages.Add(MoveTemp(AdditionalImage));
				}
				FinalPath = Image.GetFinalPath();
				StagedImage = MoveTemp(Image);
				CaptureReceipt = ExecutionResult.CaptureReceipt;
				ArtifactKind = TEXT("image");
				MimeType = Format == TEXT("png")
					? FString(TEXT("image/png")) : FString(TEXT("image/jpeg"));
				FScopeLock Lock(&Mutex);
				State = ECaptureNativeState::NativeReady;
				return true;
			}

			bool TickViewportSequenceOnGameThread()
			{
				TSharedPtr<CaptureMediaLongJob::FLevelViewportSequenceCapture>
					Capture;
				bool bCancelled = false;
				{
					FScopeLock Lock(&Mutex);
					if (bStopRequested)
					{
						bCancelled = true;
						ViewportSequenceTicker.Reset();
						Capture = MoveTemp(ViewportSequenceCapture);
					}
					else
					{
						Capture = ViewportSequenceCapture;
					}
				}
				if (!Capture.IsValid())
				{
					return false;
				}
				if (bCancelled)
				{
					Capture->Cancel();
					FailOnGameThread(NativeFailure(TEXT("capture_cancelled"), TEXT("The viewport sequence capture was cancelled."), TEXT("stop_requested"), Request.GetSubjectRef(), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), TEXT("capture_native")));
					return false;
				}

				TSharedPtr<FJsonObject> MechanicsResult;
				bool bComplete = false;
				UE::UAI::Contract::FDiagnostic LocalError;
				if (!Capture->Tick(MechanicsResult, bComplete, LocalError))
				{
					PreparedImageExecution.LocateDiagnosticSources(LocalError);
					Capture->Cancel();
					{
						FScopeLock Lock(&Mutex);
						ViewportSequenceCapture.Reset();
						ViewportSequenceTicker.Reset();
					}
					FailOnGameThread(MoveTemp(LocalError));
					return false;
				}
				if (!bComplete)
				{
					return true;
				}

				CaptureMediaLongJob::FUeAgentCaptureMediaImageExecutionResult
					ExecutionResult;
				if (!CaptureMediaLongJob::TryFinalizeCaptureMediaImagePolicyExecution(
						Request,
						PreparedImageExecution,
						MechanicsResult,
						ExecutionResult,
						LocalError))
				{
					Capture->Cancel();
					{
						FScopeLock Lock(&Mutex);
						ViewportSequenceCapture.Reset();
						ViewportSequenceTicker.Reset();
					}
					FailOnGameThread(MoveTemp(LocalError));
					return false;
				}
				{
					FScopeLock Lock(&Mutex);
					ViewportSequenceCapture.Reset();
					ViewportSequenceTicker.Reset();
				}
				AdoptImageExecutionResultOnGameThread(MoveTemp(ExecutionResult));
				return false;
			}

			bool StartViewportSequenceOnGameThread(const FString& Root)
			{
				UE::UAI::Contract::FDiagnostic LocalError;
				const FString PreferredPath = Root / TEXT("capture.png");
				if (!CaptureMediaLongJob::TryPrepareCaptureMediaImagePolicyExecution(
						Request,
						PreferredPath,
						PreparedImageExecution,
						LocalError))
				{
					FailOnGameThread(MoveTemp(LocalError));
					return false;
				}
				TSharedPtr<CaptureMediaLongJob::FLevelViewportSequenceCapture>
					Capture;
				if (!CaptureMediaLongJob::FLevelViewportSequenceCapture::TryCreate(
						PreparedImageExecution.Context, Capture, LocalError)
					|| !Capture.IsValid())
				{
					PreparedImageExecution.LocateDiagnosticSources(LocalError);
					if (LocalError.IsEmpty())
					{
						LocalError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("capture_viewport_sequence_create_failed"),
							TEXT("The viewport sequence factory returned no capture session."), TEXT("capture_native"));
						LocalError.SourceKind = TEXT("native_result");
						LocalError.PropertyPath = TEXT("capture_session_available");
						LocalError.ActualValue = MakeShared<FJsonValueBoolean>(false);
						LocalError.ActualValuePresent = true;
						LocalError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					}
					FailOnGameThread(MoveTemp(LocalError));
					return false;
				}
				const TWeakPtr<FCaptureNativeSession, ESPMode::ThreadSafe> WeakSelf =
					AsShared();
				const FTSTicker::FDelegateHandle Handle =
					FTSTicker::GetCoreTicker().AddTicker(
						FTickerDelegate::CreateLambda(
							[WeakSelf](float)
							{
								const TSharedPtr<FCaptureNativeSession,
									ESPMode::ThreadSafe> Self = WeakSelf.Pin();
								return Self.IsValid()
									? Self->TickViewportSequenceOnGameThread()
									: false;
							}),
						0.0f);
				if (!Handle.IsValid())
				{
					Capture->Cancel();
					FailOnGameThread(NativeFailure(TEXT("capture_viewport_sequence_schedule_failed"), TEXT("The viewport sequence capture ticker could not be registered."), TEXT("viewport_sequence_ticker_available"), Request.GetSubjectRef(), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("capture_native")));
					return false;
				}
				{
					FScopeLock Lock(&Mutex);
					ViewportSequenceCapture = MoveTemp(Capture);
					ViewportSequenceTicker = Handle;
				}
				return true;
			}

			void StartOnGameThread()
			{
				{
					FScopeLock Lock(&Mutex);
					if (bStopRequested)
					{
						StateDiagnostic.Append(NativeFailure(TEXT("capture_cancelled_before_start"), TEXT("A stop was requested before the native capture could start."), TEXT("stop_requested"), Request.GetSubjectRef(), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), TEXT("native_session")));
						State = ECaptureNativeState::ExitedFailure;
						return;
					}
					State = ECaptureNativeState::Running;
				}
				const FString Root = MakeNativeRoot(OperationId);
				NativeRoot = Root;
				IFileManager::Get().MakeDirectory(*Root, true);
				if (Request.GetFamily()
						== CaptureMediaLongJob::EUeAgentCaptureMediaLongJobFamily::
							ImageCapture
					&& CaptureMediaLongJob::IsViewportSequenceCapturePolicy(
						Request.GetPolicyId()))
				{
					StartViewportSequenceOnGameThread(Root);
					return;
				}
				if (Request.GetFamily()
					== CaptureMediaLongJob::EUeAgentCaptureMediaLongJobFamily::
						MediaRecording)
				{
					UWorld* ExplicitWorld = ResolveCurrentEditorWorld(World);
					if (Request.GetPolicyId()
						== TEXT("policy:media-recording:sound-preview-wav/v1"))
					{
						const FString SoundObject = StripSubjectPrefix(
							Request.GetSubjectRef(), TEXT("media-source:sound:"));
						USoundWave* PreviewSound = SoundObject.IsEmpty() ? nullptr
							: Cast<USoundWave>(StaticLoadObject(
								USoundWave::StaticClass(), nullptr, *SoundObject));
						if (!PreviewSound)
						{
							FailOnGameThread(NativeFailure(TEXT("capture_media_preview_sound_unavailable"), TEXT("The requested sound preview asset could not be loaded as a SoundWave."), TEXT("sound_asset"), Request.GetSubjectRef(), MakeShared<FJsonValueString>(SoundObject), MakeShared<FJsonValueString>(TEXT("loadable SoundWave")), TEXT("capture_native")));
							return;
						}
						TArray<uint8> RawPcm;
						uint32 SampleRate = 0;
						uint16 NumChannels = 0;
						if (!PreviewSound->GetImportedSoundWaveData(
								RawPcm, SampleRate, NumChannels))
						{
							FailOnGameThread(NativeFailure(TEXT("capture_media_preview_pcm_unavailable"), TEXT("The sound asset did not provide imported PCM audio data."), TEXT("imported_pcm_available"), Request.GetSubjectRef(), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("capture_native")));
							return;
						}
						NativePath = Root / TEXT("preview.wav");
						FinalPath = FPaths::ProjectSavedDir()
							/ TEXT("UeAgentInterface/LongRunningSession/Artifacts")
							/ (FGuid::NewGuid().ToString(EGuidFormats::Digits)
								+ TEXT(".wav"));
						ArtifactKind = TEXT("sound_preview");
						Format = TEXT("wav");
						MimeType = TEXT("audio/wav");
						if (RawPcm.IsEmpty() || RawPcm.Num() % sizeof(int16) != 0 || !SampleRate || !NumChannels)
						{
							auto Actual = MakeShared<FJsonObject>();
							Actual->SetNumberField(TEXT("pcm_byte_count"), RawPcm.Num());
							Actual->SetNumberField(TEXT("sample_rate_hz"), SampleRate);
							Actual->SetNumberField(TEXT("channel_count"), NumChannels);
							FailOnGameThread(NativeFailure(TEXT("capture_media_preview_pcm_invalid"), TEXT("The imported PCM data is empty, not aligned to 16-bit samples, or has an invalid sample rate or channel count."), TEXT("imported_pcm"), Request.GetSubjectRef(), MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("nonempty 16-bit PCM; positive sample rate and channel count")), TEXT("capture_native")));
							return;
						}
						BeginMediaWriteOnGameThread(Audio::TSampleBuffer<int16>(
							reinterpret_cast<const int16*>(RawPcm.GetData()),
							RawPcm.Num() / sizeof(int16), NumChannels, SampleRate));
						return;
					}
					Audio::FMixerDevice* Mixer = ExplicitWorld
						? FAudioDeviceManager::GetAudioMixerDeviceFromWorldContext(ExplicitWorld) : nullptr;
					if (!Mixer)
					{
						FailOnGameThread(NativeFailure(TEXT("capture_media_audio_device_unavailable"), TEXT("The capture world has no available audio mixer device."), TEXT("audio_mixer_available"), Request.GetSubjectRef(), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("capture_native")));
						return;
					}
					const FString SubmixObject = StripSubjectPrefix(
						Request.GetSubjectRef(), TEXT("media-source:audio-submix:"));
					if (SubmixObject.IsEmpty())
					{
						FailOnGameThread(NativeFailure(TEXT("capture_media_submix_subject_invalid"), TEXT("The media subject does not identify an audio submix."), TEXT("subject"), Request.GetSubjectRef(), MakeShared<FJsonValueString>(Request.GetSubjectRef()), MakeShared<FJsonValueString>(TEXT("media-source:audio-submix:<asset-path> or media-source:audio-submix:master")), TEXT("capture_native")));
						return;
					}
					Submix = SubmixObject == TEXT("master")
						? nullptr
						: Cast<USoundSubmix>(StaticLoadObject(
							USoundSubmix::StaticClass(), nullptr, *SubmixObject));
					if (SubmixObject != TEXT("master") && !Submix.IsValid())
					{
						FailOnGameThread(NativeFailure(TEXT("capture_media_submix_unavailable"), TEXT("The requested audio submix asset could not be loaded."), TEXT("audio_submix"), Request.GetSubjectRef(), MakeShared<FJsonValueString>(SubmixObject), MakeShared<FJsonValueString>(TEXT("loadable SoundSubmix")), TEXT("capture_native")));
						return;
					}
					const double ExpectedDuration = FMath::Max(
						DecimalField(Request.GetParameters().Get(), TEXT("expected_duration"), 5.0),
						0.01);
					NativePath = Root / TEXT("recording.wav");
					FinalPath = FPaths::ProjectSavedDir()
						/ TEXT("UeAgentInterface/LongRunningSession/Artifacts")
						/ (FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".wav"));
					ArtifactKind = TEXT("audio_recording");
					Format = TEXT("wav");
					MimeType = TEXT("audio/wav");
					// Bind the exact native instance on its owning thread. A failed
					// explicit lookup must not inherit FMixerDevice's master fallback.
					if (Submix.IsValid()) Mixer->RegisterSoundSubmix(Submix.Get());
					RecordingBuffer = MakeShared<FMediaRecordingBuffer, ESPMode::ThreadSafe>(ExpectedDuration);
					FAudioThread::RunCommandOnAudioThread([this, Mixer]()
					{
						RecordingSubmix = Submix.IsValid() ? Mixer->GetSubmixInstance(Submix.Get()).Pin()
							: Mixer->GetMasterSubmix().Pin();
						if (RecordingSubmix.IsValid()) RecordingSubmix->RegisterBufferListener(RecordingBuffer.ToSharedRef());
					});
					FAudioCommandFence Fence; Fence.BeginFence(); Fence.Wait();
					if (!RecordingSubmix.IsValid())
					{
						FailOnGameThread(NativeFailure(TEXT("capture_media_submix_instance_unavailable"), TEXT("The audio mixer did not provide the requested submix instance."), TEXT("submix_instance_available"), Request.GetSubjectRef(), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("capture_native")));
						return;
					}
					const TWeakPtr<FCaptureNativeSession, ESPMode::ThreadSafe>
						WeakSelf = AsShared();
					const FTSTicker::FDelegateHandle Handle =
						FTSTicker::GetCoreTicker().AddTicker(
							FTickerDelegate::CreateLambda(
								[WeakSelf](float)
								{
									const TSharedPtr<
										FCaptureNativeSession,
										ESPMode::ThreadSafe> Self = WeakSelf.Pin();
									if (!Self.IsValid())
									{
										return false;
									}
								// The audio callback owns elapsed sample time. A game-thread
								// ticker delay can expire before even one audio buffer arrives.
								if (!Self->RecordingBuffer.IsValid()) return false;
								if (!Self->RecordingBuffer->IsComplete()) return true;
									{
										FScopeLock Lock(&Self->Mutex);
										Self->MediaStopTicker.Reset();
									}
									Self->StopMediaOnGameThread();
									return false;
								}),
						0.05f);
					if (!Handle.IsValid())
					{
						{
							FScopeLock Lock(&Mutex);
							StateDiagnostic.Append(NativeFailure(TEXT("capture_media_recording_poll_schedule_failed"), TEXT("The audio recording completion ticker could not be registered."), TEXT("recording_ticker_available"), Request.GetSubjectRef(), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("capture_native")));
						}
						StopMediaOnGameThread();
						return;
					}
					{
						FScopeLock Lock(&Mutex);
						MediaStopTicker = Handle;
					}
					return;
				}

				TArray<FColor> Pixels;
				FIntPoint Size = FIntPoint::ZeroValue;
				UE::UAI::Contract::FDiagnostic LocalError;
				if (Request.GetFamily()
					== CaptureMediaLongJob::EUeAgentCaptureMediaLongJobFamily::
						ImageCapture)
				{
					const FString PreferredPath = Root / TEXT("capture.png");
					CaptureMediaLongJob::FUeAgentCaptureMediaImageExecutionResult
						ExecutionResult;
					if (!CaptureMediaLongJob::TryExecuteCaptureMediaImagePolicy(
							Request,
							PreferredPath,
							ExecutionResult,
							LocalError))
					{
						FailOnGameThread(MoveTemp(LocalError));
						return;
					}
					AdoptImageExecutionResultOnGameThread(MoveTemp(ExecutionResult));
					return;
				}
				else
				{
					const FString ObjectPath = StripSubjectPrefix(
						Request.GetSubjectRef(), TEXT("render-resource:"));
					UTextureRenderTarget2D* Target = nullptr;
					bool bOperationLocalTarget = false;
					if (ObjectPath == TEXT("operation-local"))
					{
						const int64 Width = static_cast<int64>(DecimalField(
							Request.GetParameters().Get(), TEXT("width"), 0.0));
						const int64 Height = static_cast<int64>(DecimalField(
							Request.GetParameters().Get(), TEXT("height"), 0.0));
						if (Width <= 0 || Height <= 0
							|| Width > MAX_int32 || Height > MAX_int32
							|| Width > MAX_int64 / Height
							|| !Resources)
						{
							auto Actual = MakeShared<FJsonObject>();
							Actual->SetNumberField(TEXT("width"), Width);
							Actual->SetNumberField(TEXT("height"), Height);
							Actual->SetBoolField(TEXT("resource_tracker_available"), Resources != nullptr);
							FailOnGameThread(NativeFailure(TEXT("capture_render_operation_local_size_invalid"), TEXT("The operation-local render target dimensions or resource tracker are invalid."), TEXT("render_target"), Request.GetSubjectRef(), MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("positive int32 dimensions, representable pixel count, and an available resource tracker")), TEXT("capture_native")));
							return;
						}
						FUeAgentResourceChargeError ChargeError;
						const int64 PixelCount = Width * Height;
						if (PixelCount > MAX_int64 / static_cast<int64>(sizeof(FColor)))
						{
							FailOnGameThread(NativeFailure(TEXT("capture_render_operation_local_size_invalid"), TEXT("The render target pixel buffer would exceed the representable memory size."), TEXT("pixel_count"), Request.GetSubjectRef(), MakeShared<FJsonValueNumber>(PixelCount), MakeShared<FJsonValueNumber>(MAX_int64 / static_cast<int64>(sizeof(FColor))), TEXT("capture_native")));
							return;
						}
						if (!Resources->TryChargeMany(
							{
								{EUeAgentResourceDimension::WorkUnits, PixelCount},
								{EUeAgentResourceDimension::EstimatedMemoryBytes,
									PixelCount * static_cast<int64>(sizeof(FColor))},
							},
							TEXT("capture.render.operation_local"),
							ChargeError))
						{
							FailOnGameThread(ChargeError);
							return;
						}
						Target = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
						if (Target)
						{
							Target->AddToRoot();
							Target->InitCustomFormat(
								static_cast<int32>(Width),
								static_cast<int32>(Height),
								PF_B8G8R8A8,
								false);
							Target->UpdateResourceImmediate(true);
							bOperationLocalTarget = true;
						}
					}
					else if (!ObjectPath.IsEmpty())
					{
						Target = Cast<UTextureRenderTarget2D>(StaticLoadObject(
							UTextureRenderTarget2D::StaticClass(), nullptr, *ObjectPath));
					}
					UWorld* ExplicitWorld = ResolveCurrentEditorWorld(World);
					if (!Target || !ExplicitWorld || !Target->GameThread_GetRenderTargetResource())
					{
						if (bOperationLocalTarget && Target)
						{
							Target->ReleaseResource();
							Target->RemoveFromRoot();
						}
						const TCHAR* Reason = !Target ? TEXT("The requested render target could not be loaded or created.")
							: !ExplicitWorld ? TEXT("The editor world is unavailable for render target capture.")
							: TEXT("The render target has no game-thread render resource.");
						FailOnGameThread(NativeFailure(TEXT("capture_render_target_unavailable"), Reason, TEXT("render_target"), Request.GetSubjectRef(), MakeShared<FJsonValueString>(ObjectPath), MakeShared<FJsonValueString>(TEXT("available render target, editor world, and render resource")), TEXT("capture_native")));
						return;
					}
					const FLinearColor Color(
						static_cast<float>(DecimalField(Request.GetParameters().Get(), TEXT("r"), 0.0)),
						static_cast<float>(DecimalField(Request.GetParameters().Get(), TEXT("g"), 0.0)),
						static_cast<float>(DecimalField(Request.GetParameters().Get(), TEXT("b"), 0.0)),
						static_cast<float>(DecimalField(Request.GetParameters().Get(), TEXT("a"), 1.0)));
					UKismetRenderingLibrary::ClearRenderTarget2D(
						ExplicitWorld, Target, Color);
					FlushRenderingCommands();
					FTextureRenderTargetResource* Resource =
						Target->GameThread_GetRenderTargetResource();
					const bool bReadPixels = Resource->ReadPixels(Pixels);
					Size = FIntPoint(Target->SizeX, Target->SizeY);
					if (bOperationLocalTarget)
					{
						Target->ReleaseResource();
						Target->RemoveFromRoot();
					}
					if (!bReadPixels)
					{
						FailOnGameThread(NativeFailure(TEXT("capture_render_read_pixels_failed"), TEXT("The render target resource failed to read its pixels."), TEXT("render_target"), Request.GetSubjectRef(), MakeShared<FJsonValueString>(ObjectPath), MakeShared<FJsonValueString>(TEXT("readable pixel buffer")), TEXT("capture_native")));
						return;
					}
					ArtifactKind = TEXT("render_target_image");
				}
				NativePath = Root / TEXT("capture.png");
				FinalPath = FPaths::ProjectSavedDir()
					/ TEXT("UeAgentInterface/LongRunningSession/Artifacts")
					/ (FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".png"));
				Format = TEXT("png");
				MimeType = TEXT("image/png");
				if (!WritePng(Pixels, Size, NativePath, LocalError))
				{
					FailOnGameThread(MoveTemp(LocalError));
					return;
				}
				FScopeLock Lock(&Mutex);
				State = ECaptureNativeState::NativeReady;
			}

			void BeginMediaWriteOnGameThread(const Audio::TSampleBuffer<int16>& Samples)
			{
				MediaWriter = MakeUnique<Audio::FSoundWavePCMWriter>();
				if (!MediaWriter->BeginWriteToWavFile(Samples, FPaths::GetBaseFilename(NativePath),
					FPaths::ConvertRelativePathToFull(FPaths::GetPath(NativePath))))
				{
					FailOnGameThread(NativeFailure(TEXT("capture_media_wav_write_start_failed"), TEXT("The PCM writer could not start writing the captured audio to WAV."), TEXT("wav_writer_started"), Request.GetSubjectRef(), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("capture_native")));
					return;
				}
				const TWeakPtr<FCaptureNativeSession, ESPMode::ThreadSafe>
					WeakSelf = AsShared();
				const FTSTicker::FDelegateHandle Handle =
					FTSTicker::GetCoreTicker().AddTicker(
						FTickerDelegate::CreateLambda(
							[WeakSelf](float)
							{
								const TSharedPtr<FCaptureNativeSession,
									ESPMode::ThreadSafe> Self = WeakSelf.Pin();
								if (!Self.IsValid())
								{
									return false;
								}
								if (!Self->MediaWriter->IsDone()) return true;
								Audio::ESoundWavePCMWriterState WriterState;
								Self->MediaWriter->CheckStatus(&WriterState);
								Self->MediaWriter.Reset();
								FScopeLock Lock(&Self->Mutex);
								Self->MediaWriteTicker.Reset();
								const bool bWritten = WriterState == Audio::ESoundWavePCMWriterState::Suceeded;
								if (!bWritten)
								{
									Self->StateDiagnostic.Append(NativeFailure(TEXT("capture_media_wav_write_failed"), TEXT("The PCM writer did not successfully finish the captured WAV file."), TEXT("wav_writer_state"), Self->Request.GetSubjectRef(), MakeShared<FJsonValueNumber>(static_cast<int32>(WriterState)), MakeShared<FJsonValueNumber>(static_cast<int32>(Audio::ESoundWavePCMWriterState::Suceeded)), TEXT("capture_write")));
								}
								Self->State = Self->StateDiagnostic.IsEmpty()
									? ECaptureNativeState::NativeReady : ECaptureNativeState::ExitedFailure;
								return false;
							}),
						0.05f);
				FScopeLock Lock(&Mutex);
				if (!Handle.IsValid())
				{
					MediaWriter->CancelWrite();
					MediaWriter.Reset();
					State = ECaptureNativeState::ExitedFailure;
					StateDiagnostic.Append(NativeFailure(TEXT("capture_media_export_poll_schedule_failed"), TEXT("The WAV export completion ticker could not be registered."), TEXT("wav_export_ticker_available"), Request.GetSubjectRef(), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("native_session")));
				}
				else
				{
					MediaWriteTicker = Handle;
				}
			}

			void StopMediaOnGameThread()
			{
				// No native ownership for pre-start cancellation or sound-preview export.
				// Repeated cancellation during writing must not start a second export.
				if (!RecordingSubmix.IsValid()) return;
				const auto NativeRecording = MoveTemp(RecordingSubmix);
				const auto Buffer = MoveTemp(RecordingBuffer);
				NativeRecording->UnregisterBufferListener(Buffer.ToSharedRef());
				int32 Channels = 0, Rate = 0;
				Audio::FAlignedFloatBuffer Recorded;
				UE::UAI::Contract::FDiagnostic SampleError;
				if (!Buffer->Take(Recorded, Channels, Rate, SampleError))
				{
					FailOnGameThread(MoveTemp(SampleError));
					return;
				}
				if (Recorded.IsEmpty() || Channels <= 0 || Rate <= 0)
				{
					auto Actual = MakeShared<FJsonObject>();
					Actual->SetNumberField(TEXT("sample_count"), Recorded.Num());
					Actual->SetNumberField(TEXT("channel_count"), Channels);
					Actual->SetNumberField(TEXT("sample_rate_hz"), Rate);
					FailOnGameThread(NativeFailure(TEXT("capture_media_recording_empty"), TEXT("The recording yielded no samples or an invalid channel count or sample rate."), TEXT("recorded_audio"), Request.GetSubjectRef(), MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("nonempty samples; positive channel count and sample rate")), TEXT("capture_native")));
					return;
				}
				BeginMediaWriteOnGameThread(Audio::TSampleBuffer<int16>(Recorded, Channels, Rate));
			}

			CaptureMediaLongJob::FUeAgentCaptureMediaLongJobTypedRequest Request;
			TWeakObjectPtr<UWorld> World;
			TWeakObjectPtr<USoundSubmix> Submix;
			Audio::FMixerSubmixPtr RecordingSubmix; // Exact native instance, never a fallback on stop.
			TSharedPtr<FMediaRecordingBuffer, ESPMode::ThreadSafe> RecordingBuffer;
			TUniquePtr<Audio::FSoundWavePCMWriter> MediaWriter; // Per operation, shared by both WAV policies.
			FString PublisherOwnerId;
			TSharedRef<CaptureMediaLongJob::IUeAgentCaptureImageArtifactService,
				ESPMode::ThreadSafe> ImageArtifactService;
			FCriticalSection Mutex;
			FString OperationId;
			FString NativeResourceId;
			FString NativeRoot;
			FString NativePath;
			FString FinalPath;
			FString ArtifactKind;
			FString Format;
			FString MimeType;
			UE::UAI::Contract::FDiagnostic StateDiagnostic;
			FTSTicker::FDelegateHandle MediaStopTicker;
			FTSTicker::FDelegateHandle MediaWriteTicker;
			FTSTicker::FDelegateHandle ViewportSequenceTicker;
			FUeAgentResourceTracker* Resources = nullptr;
			TOptional<FFileArtifactReference> Artifact;
			TArray<FFileArtifactReference> AdditionalArtifacts;
			TOptional<CaptureMediaLongJob::FUeAgentStagedCaptureImage> StagedImage;
			TArray<CaptureMediaLongJob::FUeAgentStagedCaptureImage>
				AdditionalStagedImages;
			TArray<FString> OwnedProducedImagePaths;
			TSharedPtr<FJsonObject> CaptureReceipt;
			CaptureMediaLongJob::FUeAgentPreparedCaptureMediaImageExecution
				PreparedImageExecution;
			TSharedPtr<CaptureMediaLongJob::FLevelViewportSequenceCapture>
				ViewportSequenceCapture;
			ECaptureNativeState State = ECaptureNativeState::Scheduled;
			bool bStarted = false;
			bool bStopRequested = false;
			bool bSequenceCleanupPending = false;
			bool bReleased = false;
		};

		class FCaptureMediaBuilder final
			: public CaptureMediaLongJob::IUeAgentCaptureMediaLongJobPortBuilder
		{
		public:
			FCaptureMediaBuilder(
				TWeakObjectPtr<UWorld> InWorld,
				FString InPublisherOwnerId,
				TSharedRef<CaptureMediaLongJob::IUeAgentCaptureImageArtifactService,
					ESPMode::ThreadSafe> InImageArtifactService)
				: World(InWorld), PublisherOwnerId(MoveTemp(InPublisherOwnerId)),
				  ImageArtifactService(InImageArtifactService)
			{
			}

			bool TryCreatePort(
				const CaptureMediaLongJob::FUeAgentCaptureMediaLongJobTypedRequest&
					Request,
				const CaptureMediaLongJob::FUeAgentCaptureMediaLongJobPolicyDescriptor&
					Policy,
				TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe>& OutPort,
				FExecutionMutationError& OutError) override
			{
				return CaptureMediaLongJob::TryCreateCaptureMediaNativeResourcePort(
					Request,
					Policy,
					MakeShared<FCaptureNativeSession, ESPMode::ThreadSafe>(
						Request, World, PublisherOwnerId, ImageArtifactService),
					OutPort,
					OutError);
			}

		private:
			TWeakObjectPtr<UWorld> World;
			FString PublisherOwnerId;
			TSharedRef<CaptureMediaLongJob::IUeAgentCaptureImageArtifactService,
				ESPMode::ThreadSafe> ImageArtifactService;
		};

		CompileBuildJob::FUeAgentCompileBuildJobPolicyDescriptor CompilePolicy(
			CompileBuildJob::EUeAgentCompileBuildJobFamily Family,
			const TCHAR* PolicyId,
			const TCHAR* Mechanism,
			const TCHAR* Oracle)
		{
			return {Family, PolicyId, Mechanism, Oracle};
		}

		EditorSessionOperation::FUeAgentEditorSessionOperationPolicyDescriptor
		EditorPolicy(
			EditorSessionOperation::EUeAgentEditorSessionOperationFamily Family,
			const TCHAR* PolicyId,
			const TCHAR* Mechanism,
			const TCHAR* Oracle,
			EUeAgentStatePlane Plane,
			TArray<EUeAgentExecutionWorldType> Worlds,
			FString SubjectSchemaJson,
			FString ParameterSchemaJson,
			FString ParameterGuidance,
			FString ResultGuidance,
			FString MinimumExample)
		{
			return {
				Family,
				PolicyId,
				Mechanism,
				Oracle,
				Plane,
				MoveTemp(Worlds),
				MoveTemp(SubjectSchemaJson),
				MoveTemp(ParameterSchemaJson),
				MoveTemp(ParameterGuidance),
				MoveTemp(ResultGuidance),
				MoveTemp(MinimumExample),
			};
		}

		FString ExistingWorldSubjectSchema()
		{
			return TEXT(
				"{\"minLength\":12,\"pattern\":\"^world:/Game/.+\","
				"\"type\":\"string\"}");
		}

		FString RuntimePreviewPieSubjectSchema()
		{
			return TEXT(
				"{\"pattern\":\"^runtime-subject:pie:(0|[1-9][0-9]*)$\","
				"\"type\":\"string\"}");
		}

		FString EmptyObjectParameterSchema()
		{
			return TEXT(
				"{\"additionalProperties\":false,\"properties\":{},"
				"\"type\":\"object\"}");
		}

		FString PieDebugParameterSchema()
		{
			return TEXT(
				"{\"additionalProperties\":false,\"properties\":{"
				"\"local_player_index\":{\"const\":0,\"type\":\"integer\"},"
				"\"viewport_width\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":2147483647,\"default\":1280,"
				"\"description\":\"PIE viewport width in physical pixels; session-local, independent of desktop DPI and saved Play window preferences.\"},"
				"\"viewport_height\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":2147483647,\"default\":720,"
				"\"description\":\"PIE viewport height in physical pixels; session-local, independent of desktop DPI and saved Play window preferences.\"}},"
				"\"type\":\"object\"}");
		}

		FString SetCurrentWorldParameterSchema()
		{
			return TEXT(
				"{\"additionalProperties\":false,\"properties\":{"
				"\"save_existing_map\":{\"type\":\"boolean\"}},"
				"\"type\":\"object\"}");
		}

		FString CreateBlankWorldParameterSchema()
		{
			return TEXT(R"schema({"type":"object","additionalProperties":false,"properties":{
"overwrite_policy":{"type":"string","enum":["fail","reuse","replace"],"default":"fail"},
"save_existing_map":{"type":"boolean","default":true},
"create_world_partition":{"type":"boolean","default":false}}})schema");
		}

		FString PartitionRegionParameterSchema()
		{
			return TEXT(R"schema({"type":"object","additionalProperties":false,"required":["bounds"],"properties":{
"bounds":{"type":"object","required":["center","extent"],"properties":{
"center":{"type":"array","minItems":3,"maxItems":3,"items":{"type":"number"},"description":"World-space XYZ center in centimeters."},
"extent":{"type":"array","minItems":3,"maxItems":3,"items":{"type":"number","minimum":0},"description":"Nonnegative XYZ half-extents in centimeters."}}},
"label":{"type":"string","minLength":1,"default":"UAI_WP_Region"},
"dry_run":{"type":"boolean","default":false}}})schema");
		}

		FString PartitionActorParameterSchema()
		{
			return TEXT(R"schema({"type":"object","additionalProperties":false,"properties":{
"actor_guids":{"type":"array","items":{"type":"string","minLength":1},"description":"Actor GUIDs returned by World Partition discovery."},
"actor_ids":{"type":"array","items":{"type":"string","minLength":1},"description":"Exact path, name or label of actors in the current world; prefer an unambiguous path."},
"dry_run":{"type":"boolean","default":false}},
"anyOf":[{"required":["actor_guids"],"properties":{"actor_guids":{"minItems":1}}},{"required":["actor_ids"],"properties":{"actor_ids":{"minItems":1}}}]})schema");
		}

		FString CulturePreviewParameterSchema()
		{
			return TEXT(R"schema({"type":"object","additionalProperties":false,"required":["culture"],"properties":{
"culture":{"type":"string","minLength":1,"description":"Registered culture name matching the culture: subject."},
"scope":{"type":"string","const":"session","default":"session"},
"restore_after":{"type":"boolean","default":true}}})schema");
		}

		FString ModelingActionParameterSchema()
		{
			return TEXT(R"schema({"type":"object","additionalProperties":false,"required":["action_id"],"properties":{
"action_id":{"type":"string","const":"spawn_transient_box"},
"size_x":{"type":"number","exclusiveMinimum":0,"maximum":100000,"default":100,"description":"X dimension in centimeters."},
"size_y":{"type":"number","exclusiveMinimum":0,"maximum":100000,"default":100,"description":"Y dimension in centimeters."},
"size_z":{"type":"number","exclusiveMinimum":0,"maximum":100000,"default":100,"description":"Z dimension in centimeters."}}})schema");
		}

		CaptureMediaLongJob::FUeAgentCaptureMediaLongJobPolicyDescriptor
		CapturePolicy(
			CaptureMediaLongJob::EUeAgentCaptureMediaLongJobFamily Family,
			const TCHAR* PolicyId,
			const TCHAR* Mechanism,
			const TCHAR* Oracle,
			EUeAgentStatePlane Plane,
			TArray<EUeAgentExecutionWorldType> Worlds)
		{
			return {Family, PolicyId, Mechanism, Oracle, Plane, MoveTemp(Worlds)};
		}
	}

#if WITH_DEV_AUTOMATION_TESTS
	namespace TestSupport
	{
		UEAGENTINTERFACE_API bool SafeExitResourceSetsMatchForTest(
			const TArray<FString>& Requested,
			const TArray<FString>& Dirty)
		{
			return ExactResourceSetMatches(Requested, Dirty);
		}

		UEAGENTINTERFACE_API void DiscardSafeExitConfigResourcesForTest(
			const TArray<FString>& ResourceRefs)
		{
			DiscardConfigResources(ResourceRefs);
		}
	}
#endif

	const TArray<EditorSessionOperation::
		FUeAgentEditorSessionOperationPolicyDescriptor>&
	GetLongRunningEditorSessionPolicyDescriptors()
	{
		static const TArray<EditorSessionOperation::
			FUeAgentEditorSessionOperationPolicyDescriptor> Policies = {
			EditorPolicy(
				EditorSessionOperation::EUeAgentEditorSessionOperationFamily::AssetEditor,
				TEXT("policy:asset-editor:open/v1"),
				TEXT("native_session_resource"),
				TEXT("uai.readback.editor.asset_editor/v1"),
				EUeAgentStatePlane::AuthoringMemory,
				{},
				TEXT("{\"type\":\"string\",\"pattern\":\"^asset:/.*\"}"),
				TEXT("{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"ownership\":{\"type\":\"string\",\"enum\":[\"managed\",\"adopt\"],\"default\":\"managed\"}}}"),
				TEXT("Managed ownership closes windows created by UAI, including implicit previews and native reload replacements; existing user windows are borrowed. Use ownership=adopt only to explicitly take responsibility for existing windows of this asset."),
				TEXT("Wait for active, then cancel and observe actual cleanup. cleanup_pending retains the operation and reports the remaining window; resolve unsaved changes and recover/close again. Shared holders close the window when the final holder releases it."),
				TEXT("client.editor.session(family_id='asset_editor', subject_ref='asset:/Game/Example.Example', policy_id='policy:asset-editor:open/v1', parameters={})")),
			EditorPolicy(
				EditorSessionOperation::EUeAgentEditorSessionOperationFamily::AssetEditor,
				TEXT("policy:asset-editor:preview-action/v1"),
				TEXT("native_session_resource"),
				TEXT("uai.readback.editor.asset_preview/v1"),
				EUeAgentStatePlane::AuthoringMemory,
				{},
				TEXT("{\"type\":\"string\",\"pattern\":\"^asset:/.*\"}"),
				TEXT("{\"type\":\"object\",\"additionalProperties\":false,\"required\":[\"action\"],\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"status\",\"focus\",\"reset_view\",\"play\",\"pause\",\"stop\",\"reset\",\"seek\"]},\"target\":{\"type\":\"string\",\"minLength\":1},\"time_seconds\":{\"type\":\"number\"}}}"),
				TEXT("Use the exact asset and a preview action advertised by its preview/index.hjson. Stable configuration is edited in the workspace; seek requires time_seconds. Opens the native asset preview if needed and leaves it open."),
				TEXT("Resume to terminal, inspect the native details and release. Release does not close or reset the asset preview. Playback support follows the actual preview owner."),
				TEXT("client.editor.session(family_id='asset_editor', subject_ref='asset:/Game/Example.Example', policy_id='policy:asset-editor:preview-action/v1', parameters={'action': 'status'})")),
			EditorPolicy(
				EditorSessionOperation::EUeAgentEditorSessionOperationFamily::RuntimePreview,
				TEXT("policy:runtime-session:pie-debug/v2"),
				TEXT("native_session_resource"),
				TEXT("uai.readback.editor.runtime_debug_paused/v1"),
				EUeAgentStatePlane::RuntimeInstance,
				{EUeAgentExecutionWorldType::Pie},
				RuntimePreviewPieSubjectSchema(),
				PieDebugParameterSchema(),
				TEXT("Use family_id runtime_preview, one exact runtime-subject:pie:<instance>, this policy_id and optional local_player_index 0. Optional viewport_width/viewport_height are positive physical pixel dimensions, defaulting to 1280/720. They affect only this session, not saved Play window preferences. The operation becomes active only after exact single-PIE/single-player resolution, a verified pause and initial current-workspace publication."),
				TEXT("Resume until phase active, then use the SDK-owned RuntimeSessionHandle to execute bounded scenarios. Session identity and pause/workspace fences remain machine-side; cancel this editor session only after scenarios are terminal and released."),
				TEXT("client.editor.session(family_id='runtime_preview', subject_ref='runtime-subject:pie:0', policy_id='policy:runtime-session:pie-debug/v2', parameters={'local_player_index': 0})")),
			EditorPolicy(
				EditorSessionOperation::EUeAgentEditorSessionOperationFamily::RuntimePreview,
				TEXT("policy:runtime-session:pie/v1"),
				TEXT("native_session_resource"),
				TEXT("uai.readback.editor.runtime_preview/v1"),
				EUeAgentStatePlane::RuntimeInstance,
				{EUeAgentExecutionWorldType::Pie},
				RuntimePreviewPieSubjectSchema(),
				EmptyObjectParameterSchema(),
				TEXT("Use family_id runtime_preview, subject_ref runtime-subject:pie:<non-negative instance>, this policy_id and an empty parameters object."),
				TEXT("Resume the returned operation until the PIE session is running; use runtime_instance queries for readback, then cancel, resume to terminal, read the receipt and release."),
				TEXT("client.editor.session(family_id='runtime_preview', subject_ref='runtime-subject:pie:0', policy_id='policy:runtime-session:pie/v1', parameters={})")),
			EditorPolicy(
				EditorSessionOperation::EUeAgentEditorSessionOperationFamily::RuntimePreview,
				TEXT("policy:runtime-session:culture-preview/v1"),
				TEXT("native_session_resource"),
				TEXT("uai.readback.editor.culture_preview/v1"),
				EUeAgentStatePlane::RuntimeInstance,
				{EUeAgentExecutionWorldType::Editor},
				TEXT(R"schema({"type":"string","pattern":"^culture:.+"})schema"),
				CulturePreviewParameterSchema(),
				TEXT("Use a registered culture and matching culture:<name> subject. restore_after=true applies and restores immediately; false keeps the preview active until closed."),
				TEXT("Inspect the terminal result for immediate preview, or wait for active and close the retained preview to restore the previous culture."),
				TEXT("client.editor.session(family_id='runtime_preview', subject_ref='culture:en', policy_id='policy:runtime-session:culture-preview/v1', parameters={'culture': 'en'})")),
			EditorPolicy(
				EditorSessionOperation::EUeAgentEditorSessionOperationFamily::WorldContext,
				TEXT("policy:world-context:set-current/v1"),
				TEXT("native_session_resource"),
				TEXT("uai.readback.editor.world_context/v1"),
				EUeAgentStatePlane::AuthoringMemory,
				{EUeAgentExecutionWorldType::Editor},
				ExistingWorldSubjectSchema(),
				SetCurrentWorldParameterSchema(),
				TEXT("Use family_id world_context, one exact subject_ref world:/Game/... map package, this policy_id and only optional save_existing_map (default true)."),
				TEXT("Poll the returned operation_ref until terminal; success reports a world-context session_ref, then independently read the current Editor World and dirty resources before release."),
				TEXT("client.editor.session(family_id='world_context', subject_ref='world:/Game/UAI/Experiments/Foundation/R001/Work/Maps/L_Main', policy_id='policy:world-context:set-current/v1', parameters={'save_existing_map': True})")),
			EditorPolicy(
				EditorSessionOperation::EUeAgentEditorSessionOperationFamily::WorldContext,
				TEXT("policy:world-context:create-blank/v1"),
				TEXT("native_session_resource"),
				TEXT("uai.readback.editor.world_context_created/v1"),
				EUeAgentStatePlane::AuthoringMemory,
				{EUeAgentExecutionWorldType::Editor},
				ExistingWorldSubjectSchema(),
				CreateBlankWorldParameterSchema(),
				TEXT("Use an exact world:/Game/... destination package. fail rejects an existing map, reuse activates it, and replace creates and saves a new blank map at that path. save_existing_map defaults true; create_world_partition affects newly created maps."),
				TEXT("Finish the operation and inspect its receipt, then read the current world and dirty resources. The saved map remains after release; reuse does not recreate it."),
				TEXT("client.editor.session(family_id='world_context', subject_ref='world:/Game/Maps/L_New', policy_id='policy:world-context:create-blank/v1', parameters={'overwrite_policy': 'fail', 'save_existing_map': True})")),
			EditorPolicy(
				EditorSessionOperation::EUeAgentEditorSessionOperationFamily::WorldContext,
				TEXT("policy:world-context:partition-region-loader/v1"),
				TEXT("native_session_resource"),
				TEXT("uai.readback.world_partition.region_loader/v1"),
				EUeAgentStatePlane::AuthoringMemory,
				{EUeAgentExecutionWorldType::Editor},
				ExistingWorldSubjectSchema(),
				PartitionRegionParameterSchema(),
				TEXT("The subject must be the current Editor World with World Partition. Supply world-space center and nonnegative half-extents in centimeters. dry_run validates without retaining a loader."),
				TEXT("A real loader remains active and owned by the operation. Close it to unload/drain; a dry run terminates with a planned result."),
				TEXT("client.editor.session(family_id='world_context', subject_ref='world:/Game/Maps/L_Partition', policy_id='policy:world-context:partition-region-loader/v1', parameters={'bounds': {'center': [0, 0, 0], 'extent': [1000, 1000, 1000]}})")),
			EditorPolicy(
				EditorSessionOperation::EUeAgentEditorSessionOperationFamily::WorldContext,
				TEXT("policy:world-context:partition-actor-loader/v1"),
				TEXT("native_session_resource"),
				TEXT("uai.readback.world_partition.actor_loader/v1"),
				EUeAgentStatePlane::AuthoringMemory,
				{EUeAgentExecutionWorldType::Editor},
				ExistingWorldSubjectSchema(),
				PartitionActorParameterSchema(),
				TEXT("The subject must be the current Editor World with World Partition. Supply at least one discovered actor GUID or current-world actor identity; both arrays may be combined. dry_run validates without loading."),
				TEXT("Observe the operation-owned loader until active, then close to unload/drain it. Dry-run success does not mean actors were loaded."),
				TEXT("client.editor.session(family_id='world_context', subject_ref='world:/Game/Maps/L_Partition', policy_id='policy:world-context:partition-actor-loader/v1', parameters={'actor_ids': ['TargetActor'], 'dry_run': True})")),
			EditorPolicy(
				EditorSessionOperation::EUeAgentEditorSessionOperationFamily::ModelingTool,
				TEXT("policy:modeling-tool:invoke-action/v1"),
				TEXT("native_session_resource"),
				TEXT("uai.readback.editor.modeling_tool/v1"),
				EUeAgentStatePlane::AuthoringMemory,
				{EUeAgentExecutionWorldType::Editor},
				TEXT(R"schema({"type":"string","const":"modeling-session:operation-local"})schema"),
				ModelingActionParameterSchema(),
				TEXT("spawn_transient_box creates an operation-owned dynamic mesh at the world origin. Sizes are centimeters, each in (0, 100000]. This is a transient modeling session, not a saved asset."),
				TEXT("Observe the active session and close it to destroy the owned transient actor; release only after cleanup completes."),
				TEXT("client.editor.session(family_id='modeling_tool', subject_ref='modeling-session:operation-local', policy_id='policy:modeling-tool:invoke-action/v1', parameters={'action_id': 'spawn_transient_box'})")),
			EditorPolicy(
				EditorSessionOperation::EUeAgentEditorSessionOperationFamily::PcgGeneration,
				TEXT("policy:pcg-generation:generate/v1"),
				TEXT("native_session_resource"),
				TEXT("uai.readback.editor.pcg_generation/v1"),
				EUeAgentStatePlane::AuthoringMemory,
				{EUeAgentExecutionWorldType::Editor},
				TEXT(R"schema({"type":"string","pattern":"^pcg-component:/.+"})schema"),
				EmptyObjectParameterSchema(),
				TEXT("Use pcg-component:<exact loaded component_path> from a current-world PCG read and empty parameters. Explicitly generates or regenerates unchanged inputs through native PCG, preserving its cache and editor settings. Requires an active, assigned graph outside preview/runtime generation and no current generation, cleanup or refresh."),
				TEXT("Finish this finite operation. generation_started and generation_completed refer to this native task, not persisted output. Cancel stops only its owned task and does not roll back partial effects. Successful output remains after release; save the affected map/resources through the existing editor save entry. PCG spatial reads provide independent output inspection."),
				TEXT("client.editor.session(family_id='pcg_generation', subject_ref='pcg-component:/Game/Maps/L_Main.L_Main:PersistentLevel.PCGActor.PCGComponent', policy_id='policy:pcg-generation:generate/v1', parameters={})")),
			EditorPolicy(
				EditorSessionOperation::EUeAgentEditorSessionOperationFamily::HostLifecycle,
				TEXT("policy:host-lifecycle:prepare-safe-exit/v1"),
				TEXT("host_exit_schedule"),
				TEXT("uai.readback.editor.host_exit/v1"),
				EUeAgentStatePlane::RuntimeInstance,
				{}, {}, {}, {}, {}, {}), // Fixed editor.safe_exit command owns this contract.
		};
		return Policies;
	}

	bool FUeAgentLongRunningSessionProductionConfig::IsValid() const
	{
		return !ProjectInstanceId.TrimStartAndEnd().IsEmpty()
			&& !EditorInstanceId.TrimStartAndEnd().IsEmpty()
			&& !ServerSessionId.TrimStartAndEnd().IsEmpty()
			&& !OwnerLeaseSecret.TrimStartAndEnd().IsEmpty()
			&& EditorWorld.IsValid();
	}

	bool TrySaveEditorResources(const FString& ParamsJson, FString& OutPayload,
		FUeAgentLongRunningSessionProductionError& OutError)
	{
		if (!IsInGameThread())
		{
			TPromise<bool> Promise;
			TFuture<bool> Future = Promise.GetFuture();
			AsyncTask(ENamedThreads::GameThread, [&]()
			{
				Promise.SetValue(TrySaveEditorResources(ParamsJson, OutPayload, OutError));
			});
			return Future.Get();
		}
		OutPayload.Reset();
		OutError = {};
		TSharedPtr<FJsonObject> Parameters;
		const TArray<TSharedPtr<FJsonValue>>* Requested = nullptr;
		FDiagnostic ParseError;
		if (!Json::TryParseObject(ParamsJson, Parameters, ParseError))
		{
			OutError = MoveTemp(ParseError);
			return false;
		}
		if (!Parameters->TryGetArrayField(TEXT("resource_refs"), Requested))
		{
			OutError = FactFailure(TEXT("editor_save_resources_input_invalid"),
				TEXT("resource_refs must be an array of nonempty strings."), TEXT("/resource_refs"),
				Parameters->TryGetField(TEXT("resource_refs")), MakeShared<FJsonValueString>(TEXT("array of nonempty strings")));
			OutError.Stage = TEXT("input_validation");
			OutError.ActualValuePresent = Parameters->HasField(TEXT("resource_refs"));
			OutError.ExpectedValuePresent = true;
			return false;
		}
		TArray<FString> Refs;
		for (int32 Index = 0; Index < Requested->Num(); ++Index)
		{
			const auto& Value = (*Requested)[Index];
			if (!Value || Value->Type != EJson::String || Value->AsString().TrimStartAndEnd().IsEmpty())
			{
				OutError = FactFailure(TEXT("editor_save_resources_input_invalid"),
					TEXT("The resource reference must be a nonempty string."),
					TEXT("/resource_refs/") + FString::FromInt(Index), Value, MakeShared<FJsonValueString>(TEXT("nonempty string")));
				OutError.Stage = TEXT("input_validation");
				OutError.ActualValuePresent = Value.IsValid();
				OutError.ExpectedValuePresent = true;
				return false;
			}
			Refs.Add(Value->AsString().TrimStartAndEnd());
		}
		Refs = CanonicalResourceRefs(Refs);
		TSet<FString> Seen;
		TArray<TSharedPtr<FJsonValue>> Results;
		FDiagnostic SaveFailures;
		bool bAllSaved = true;
		for (const FString& Ref : Refs)
		{
			if (Seen.Contains(Ref)) continue;
			Seen.Add(Ref);
			const auto Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("resource_ref"), Ref);
			const auto Saved = EditorResources::SaveResource(Ref);
			FString Status = Saved.Status;
			if (Saved.Diagnostic.IsValid())
			{
				Status = TEXT("failed");
				auto Diagnostic = Saved.Diagnostic;
				if (Diagnostic.TargetPath.IsEmpty()) Diagnostic.TargetPath = Saved.ResourceRef;
				// Preserve the final wire reason; never parse it back into a diagnostic.
				FString Reason = Diagnostic.Code;
				if (Diagnostic.Stage == TEXT("save_readback") && !Diagnostic.TargetPath.IsEmpty())
					Reason += TEXT(":") + Diagnostic.TargetPath;
				Item->SetStringField(TEXT("reason"), Reason);
				Item->SetObjectField(TEXT("diagnostic"), Diagnostic.ToPublicJson());
				SaveFailures.Append(Diagnostic);
				bAllSaved = false;
			}
			Item->SetStringField(TEXT("status"), Status);
			Results.Add(MakeShared<FJsonValueObject>(Item));
		}
		TArray<UPackage*> RemainingPackages;
		TArray<FString> RemainingRefs;
		CollectDirtySafeExitResourceRefs(RemainingPackages, RemainingRefs);
		TArray<TSharedPtr<FJsonValue>> Remaining;
		for (const auto& Ref : RemainingRefs) Remaining.Add(MakeShared<FJsonValueString>(Ref));
		const auto Payload = MakeShared<FJsonObject>();
		Payload->SetArrayField(TEXT("resources"), MoveTemp(Results));
		Payload->SetArrayField(TEXT("remaining_dirty_resources"), MoveTemp(Remaining));
		Payload->SetBoolField(TEXT("editor_open"), true);
		FDiagnostic JsonError;
		if (!Json::TryCanonicalize(Payload, OutPayload, JsonError))
		{
			OutError = FDiagnostic::Failure(TEXT("editor_save_resources_payload_invalid"), TEXT("The save-resource result could not be encoded."), TEXT("result_encoding"));
			OutError.PublicCause = MakeShared<FDiagnostic>(MoveTemp(JsonError));
			return false;
		}
		if (!bAllSaved) OutError = FDiagnostic::WrapFailure(TEXT("editor_save_resources_failed"), SaveFailures, TEXT("save"), TEXT("See resources for each failed resource and reason"));
		return bAllSaved;
	}

	bool TryCreateLongRunningSessionProduction(
		const FUeAgentLongRunningSessionProductionConfig& Config,
		TSharedPtr<FUeAgentLongRunningSessionRuntime>& OutRuntime,
		TArray<TSharedRef<CommandEdge::IUeAgentCommandHandler>>& OutHandlers,
		TArray<FString>& OutAuthorityScopes,
		FUeAgentLongRunningSessionProductionError& OutError)
	{
		OutRuntime.Reset();
		OutHandlers.Reset();
		OutAuthorityScopes.Reset();
		OutError = {};
		if (!Config.IsValid())
		{
			const TCHAR* Field = Config.ProjectInstanceId.TrimStartAndEnd().IsEmpty() ? TEXT("project_instance_id")
				: Config.EditorInstanceId.TrimStartAndEnd().IsEmpty() ? TEXT("editor_instance_id")
				: Config.ServerSessionId.TrimStartAndEnd().IsEmpty() ? TEXT("server_session_id")
				: Config.OwnerLeaseSecret.TrimStartAndEnd().IsEmpty() ? TEXT("owner_lease_secret")
				: TEXT("editor_world");
			OutError = NativeFailure(TEXT("long_running_session_production_config_invalid"), TEXT("The production configuration is missing a required identity, owner lease, or editor world."), FString(Field) + TEXT("_available"), FString(), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("runtime"));
			return false;
		}

		const TSharedRef<FCompileBuildBuilder, ESPMode::ThreadSafe> CompileBuilder =
			MakeShared<FCompileBuildBuilder, ESPMode::ThreadSafe>(Config.EditorWorld);
		TSharedPtr<CompileBuildJob::FUeAgentCompileBuildJobProductionPortFactory>
			CompileFactory;
		FExecutionMutationError FactoryError;
		if (!CompileBuildJob::FUeAgentCompileBuildJobProductionPortFactory::TryCreate(
				{
					{CompilePolicy(CompileBuildJob::EUeAgentCompileBuildJobFamily::AssetCompile,
						TEXT("policy:asset-compile:standard/v1"), TEXT("game_thread_stage"),
						TEXT("uai.readback.compile.asset/v1")), CompileBuilder},
					{CompilePolicy(CompileBuildJob::EUeAgentCompileBuildJobFamily::AssetCompile,
						TEXT("policy:asset-compile:blueprint/v1"), TEXT("game_thread_stage"),
						TEXT("uai.readback.compile.blueprint_report/v1")), CompileBuilder},
					{CompilePolicy(CompileBuildJob::EUeAgentCompileBuildJobFamily::WorldBuild,
						TEXT("policy:world-build:standard/v1"), TEXT("game_thread_stage"),
						TEXT("uai.readback.build.world/v1")), CompileBuilder},
					{CompilePolicy(CompileBuildJob::EUeAgentCompileBuildJobFamily::WorldBuild,
						TEXT("policy:world-build:navigation/v1"), TEXT("game_thread_stage"),
						TEXT("uai.readback.world.navigation/v1")), CompileBuilder},
					{CompilePolicy(CompileBuildJob::EUeAgentCompileBuildJobFamily::WorldBuild,
						TEXT("policy:world-build:hlod-plan/v1"), TEXT("game_thread_stage"),
						TEXT("uai.readback.world_hlod.plan/v1")), CompileBuilder},
					{CompilePolicy(CompileBuildJob::EUeAgentCompileBuildJobFamily::WorldBuild,
						TEXT("policy:world-build:hlod-run/v1"), TEXT("owned_process"),
						TEXT("uai.readback.world_hlod.commandlet_log/v1")), CompileBuilder},
					{CompilePolicy(CompileBuildJob::EUeAgentCompileBuildJobFamily::WorldBuild,
						TEXT("policy:world-build:partition-streaming-plan/v1"),
						TEXT("game_thread_stage"),
						TEXT("uai.readback.world_partition.streaming_plan/v1")), CompileBuilder},
					{CompilePolicy(CompileBuildJob::EUeAgentCompileBuildJobFamily::WorldBuild,
						TEXT("policy:world-build:partition-streaming-run/v1"),
						TEXT("owned_process"),
						TEXT("uai.readback.world_partition.commandlet_log/v1")), CompileBuilder},
					{CompilePolicy(CompileBuildJob::EUeAgentCompileBuildJobFamily::ExternalProcess,
						TEXT("policy:owned-tool:standard/v1"), TEXT("owned_process"),
						TEXT("uai.readback.owned_tool.report/v1")), CompileBuilder},
					{CompilePolicy(CompileBuildJob::EUeAgentCompileBuildJobFamily::ExternalProcess,
						TEXT("policy:owned-tool:localization-gather-text/v1"), TEXT("owned_process"),
						TEXT("uai.readback.localization.gather_text/v1")), CompileBuilder},
					{CompilePolicy(CompileBuildJob::EUeAgentCompileBuildJobFamily::ExternalProcess,
						TEXT("policy:owned-tool:localization-compile-text/v1"), TEXT("owned_process"),
						TEXT("uai.readback.localization.compile_text/v1")), CompileBuilder},
					{CompilePolicy(CompileBuildJob::EUeAgentCompileBuildJobFamily::ExternalProcess,
						TEXT("policy:owned-tool:localization-export-po/v1"), TEXT("owned_process"),
						TEXT("uai.readback.localization.export_po/v1")), CompileBuilder},
					{CompilePolicy(CompileBuildJob::EUeAgentCompileBuildJobFamily::WorldPartitionResave,
						TEXT("policy:world-partition-resave:actors-plan/v1"),
						TEXT("game_thread_stage"),
						TEXT("uai.readback.world_partition.resave_plan/v1")), CompileBuilder},
					{CompilePolicy(CompileBuildJob::EUeAgentCompileBuildJobFamily::WorldPartitionResave,
						TEXT("policy:world-partition-resave:actors-run/v1"), TEXT("owned_process"),
						TEXT("uai.readback.world_partition.resave/v1")), CompileBuilder},
					{CompilePolicy(CompileBuildJob::EUeAgentCompileBuildJobFamily::Packaging,
						TEXT("policy:packaging:query-environment/v1"), TEXT("game_thread_stage"),
						TEXT("uai.readback.packaging.environment/v1")), CompileBuilder},
					{CompilePolicy(CompileBuildJob::EUeAgentCompileBuildJobFamily::Packaging,
						TEXT("policy:packaging:export-profile/v1"), TEXT("game_thread_stage"),
						TEXT("uai.readback.packaging.profile/v1")), CompileBuilder},
					{CompilePolicy(CompileBuildJob::EUeAgentCompileBuildJobFamily::Packaging,
						TEXT("policy:packaging:validate-profile/v1"), TEXT("game_thread_stage"),
						TEXT("uai.readback.packaging.profile_validation/v1")), CompileBuilder},
					{CompilePolicy(CompileBuildJob::EUeAgentCompileBuildJobFamily::Packaging,
						TEXT("policy:packaging:diff-profile/v1"), TEXT("game_thread_stage"),
						TEXT("uai.readback.packaging.profile_diff/v1")), CompileBuilder},
					{CompilePolicy(CompileBuildJob::EUeAgentCompileBuildJobFamily::Packaging,
						TEXT("policy:packaging:plan-build/v1"), TEXT("game_thread_stage"),
						TEXT("uai.readback.packaging.plan/v1")), CompileBuilder},
					{CompilePolicy(CompileBuildJob::EUeAgentCompileBuildJobFamily::Packaging,
						TEXT("policy:packaging:run-build/v1"), TEXT("owned_process"),
						TEXT("uai.readback.packaging.build_log/v1")), CompileBuilder},
					{CompilePolicy(CompileBuildJob::EUeAgentCompileBuildJobFamily::Packaging,
						TEXT("policy:packaging:parse-log/v1"), TEXT("game_thread_stage"),
						TEXT("uai.readback.packaging.log_analysis/v1")), CompileBuilder},
					{CompilePolicy(CompileBuildJob::EUeAgentCompileBuildJobFamily::Packaging,
						TEXT("policy:packaging:report-artifacts/v1"), TEXT("game_thread_stage"),
						TEXT("uai.readback.packaging.artifacts/v1")), CompileBuilder},
					{CompilePolicy(CompileBuildJob::EUeAgentCompileBuildJobFamily::Packaging,
						TEXT("policy:packaging:clean-artifacts/v1"), TEXT("game_thread_stage"),
						TEXT("uai.readback.packaging.cleanup/v1")), CompileBuilder},
				},
				CompileFactory,
				FactoryError))
		{
			OutError = FactoryError;
			return false;
		}

		const TSharedRef<PieRuntimeDebug::FUeAgentPieRuntimeDebugRegistry,
			ESPMode::ThreadSafe> PieRuntimeDebugRegistry =
			MakeShared<PieRuntimeDebug::FUeAgentPieRuntimeDebugRegistry,
				ESPMode::ThreadSafe>();
		const TSharedRef<FEditorSessionBuilder, ESPMode::ThreadSafe> EditorBuilder =
			MakeShared<FEditorSessionBuilder, ESPMode::ThreadSafe>(
				Config.EditorInstanceId,
				Config.EditorWorld,
				PieRuntimeDebugRegistry);
		TArray<EditorSessionOperation::
			FUeAgentEditorSessionOperationPolicyRegistration>
			EditorRegistrations;
		for (const EditorSessionOperation::
			FUeAgentEditorSessionOperationPolicyDescriptor& Policy :
			GetLongRunningEditorSessionPolicyDescriptors())
		{
			EditorRegistrations.Emplace(Policy, EditorBuilder);
		}
		TSharedPtr<EditorSessionOperation::
			FUeAgentEditorSessionOperationProductionPortFactory> EditorFactory;
		if (!EditorSessionOperation::
			FUeAgentEditorSessionOperationProductionPortFactory::TryCreate(
				EditorRegistrations,
				EditorFactory,
				FactoryError))
		{
			OutError = FactoryError;
			return false;
		}

		const TSharedRef<CaptureMediaLongJob::IUeAgentCaptureImageArtifactService,
			ESPMode::ThreadSafe> ImageArtifactService =
			MakeShared<CaptureMediaLongJob::FUeAgentCaptureImageArtifactService,
				ESPMode::ThreadSafe>();
		const TSharedRef<FCaptureMediaBuilder, ESPMode::ThreadSafe> CaptureBuilder =
			MakeShared<FCaptureMediaBuilder, ESPMode::ThreadSafe>(
				Config.EditorWorld,
				TEXT("long-running-session:") + Config.ServerSessionId,
				ImageArtifactService);
		TArray<CaptureMediaLongJob::
			FUeAgentCaptureMediaLongJobPolicyRegistration> CaptureRegistrations;
		for (const CaptureMediaLongJob::
			FUeAgentCaptureMediaImagePolicyDescriptor& ImagePolicy
			: CaptureMediaLongJob::GetCaptureMediaImagePolicies())
		{
			CaptureRegistrations.Emplace(
				CapturePolicy(
					CaptureMediaLongJob::EUeAgentCaptureMediaLongJobFamily::ImageCapture,
					*ImagePolicy.GetPolicyId(),
					TEXT("image_capture_resource"),
					TEXT("uai.readback.capture.image/v1"),
					EUeAgentStatePlane::AuthoringMemory,
					{EUeAgentExecutionWorldType::Editor}),
				CaptureBuilder);
		}
		CaptureRegistrations.Emplace(
			CapturePolicy(
				CaptureMediaLongJob::EUeAgentCaptureMediaLongJobFamily::MediaRecording,
				TEXT("policy:media-recording:submix-wav/v1"),
				TEXT("media_recording_resource"),
				TEXT("uai.readback.capture.media/v1"),
				EUeAgentStatePlane::RuntimeInstance,
				{EUeAgentExecutionWorldType::Editor,
					EUeAgentExecutionWorldType::Pie}),
			CaptureBuilder);
		CaptureRegistrations.Emplace(
			CapturePolicy(
				CaptureMediaLongJob::EUeAgentCaptureMediaLongJobFamily::MediaRecording,
				TEXT("policy:media-recording:sound-preview-wav/v1"),
				TEXT("media_recording_resource"),
				TEXT("uai.readback.capture.media/v1"),
				EUeAgentStatePlane::RuntimeInstance,
				{EUeAgentExecutionWorldType::Editor,
					EUeAgentExecutionWorldType::Pie}),
			CaptureBuilder);
		CaptureRegistrations.Emplace(
			CapturePolicy(
				CaptureMediaLongJob::EUeAgentCaptureMediaLongJobFamily::RenderResource,
				TEXT("policy:render-operation:clear/v1"),
				TEXT("gpu_render_resource"),
				TEXT("uai.readback.capture.render_target/v1"),
				EUeAgentStatePlane::AuthoringMemory,
				{EUeAgentExecutionWorldType::Editor}),
			CaptureBuilder);
		TSharedPtr<CaptureMediaLongJob::
			FUeAgentCaptureMediaLongJobProductionPortFactory> CaptureFactory;
		if (!CaptureMediaLongJob::
			FUeAgentCaptureMediaLongJobProductionPortFactory::TryCreate(
				CaptureRegistrations,
				CaptureFactory,
				FactoryError))
		{
			OutError = FactoryError;
			return false;
		}
		const TSharedRef<PieRuntimeDebug::FUeAgentPieRuntimeScenarioPortFactory,
			ESPMode::ThreadSafe> PieRuntimeScenarioFactory =
			MakeShared<PieRuntimeDebug::FUeAgentPieRuntimeScenarioPortFactory,
				ESPMode::ThreadSafe>(PieRuntimeDebugRegistry, ImageArtifactService);

		FUeAgentLongRunningSessionRuntimeConfig RuntimeConfig;
		RuntimeConfig.ProjectInstanceId = Config.ProjectInstanceId;
		RuntimeConfig.EditorInstanceId = Config.EditorInstanceId;
		RuntimeConfig.ServerSessionId = Config.ServerSessionId;
		RuntimeConfig.OwnerLeaseSecret = Config.OwnerLeaseSecret;
		RuntimeConfig.ResolveEditorIdentities = [Fallback = Config.EditorWorld](FString& WorldId, FString& LevelId)
		{
			const auto Read = [&]()
			{
				UWorld* World = ResolveCurrentEditorWorld(Fallback);
				WorldId = World && World->GetPackage()
					? TEXT("world:") + World->GetPackage()->GetName() : FString();
				LevelId = World && World->PersistentLevel
					? TEXT("level:") + World->PersistentLevel->GetPathName() : FString();
			};
			if (IsInGameThread()) Read();
			else
			{
				TPromise<void> Promise;
				TFuture<void> Future = Promise.GetFuture();
				AsyncTask(ENamedThreads::GameThread, [&]() { Read(); Promise.SetValue(); });
				Future.Get();
			}
		};
		FUeAgentLongRunningSessionRuntimeError RuntimeError;
		if (!FUeAgentLongRunningSessionRuntime::TryCreate(
				RuntimeConfig,
				CompileFactory.ToSharedRef(),
				EditorFactory.ToSharedRef(),
				CaptureFactory.ToSharedRef(),
				PieRuntimeScenarioFactory,
				OutRuntime,
				RuntimeError))
		{
			OutError = RuntimeError;
			return false;
		}
		FDiagnostic HandlerError;
		const TArray<FUeAgentLongRunningSessionActivationProfile>* ActivationProfiles = nullptr;
		if (!TryGetLongRunningSessionActivationProfiles(ActivationProfiles, HandlerError)
			|| !TryCreateLongRunningSessionCommandHandlers(
				*ActivationProfiles,
				OutRuntime->GetOperationPort(),
				OutHandlers,
				HandlerError))
		{
			OutRuntime.Reset();
			OutError = HandlerError;
			return false;
		}
		OutAuthorityScopes = {TEXT("uai:operation"), TEXT("uai:job"),
			TEXT("uai:editor:session"), TEXT("uai:capture")};
		return true;
	}
}

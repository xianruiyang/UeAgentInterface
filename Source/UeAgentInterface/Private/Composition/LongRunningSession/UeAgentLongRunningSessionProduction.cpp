#include "Composition/LongRunningSession/UeAgentLongRunningSessionProduction.h"

#include "Async/Async.h"
#include "Components/DynamicMeshComponent.h"
#include "Composition/LongRunningSession/UeAgentLongRunningSessionActivation.h"
#include "Composition/LongRunningSession/UeAgentLongRunningSessionCommandHandlers.h"
#include "Composition/LongRunningSession/UeAgentSafeExitDurableCompletion.h"
#include "Containers/Ticker.h"
#include "Core/ArtifactPublisher/UeAgentFileArtifactPublisher.h"
#include "Core/ExecutionMutation/UeAgentLongRunningOperation.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaImagePolicy.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaLongJobProduction.h"
#include "DomainAdapters/CompileBuildJob/UeAgentCompileBuildJobProduction.h"
#include "DomainAdapters/CompileBuildJob/UeAgentBlueprintCompilePolicy.h"
#include "DomainAdapters/EditorSessionOperation/UeAgentEditorSessionOperationProduction.h"
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
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "ImageUtils.h"
#include "Internationalization/Internationalization.h"
#include "Internationalization/Culture.h"
#include "Interfaces/ITargetPlatform.h"
#include "Interfaces/ITargetPlatformManagerModule.h"
#include "LevelEditorViewport.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "LocalizationConfigurationScript.h"
#include "LocalizationSettings.h"
#include "LocalizationTargetTypes.h"
#include "Misc/FileHelper.h"
#include "Misc/SecureHash.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "PlayInEditorDataTypes.h"
#include "Engine/TextureRenderTarget.h"
#include "Sound/SoundSubmix.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "AudioMixerBlueprintLibrary.h"
#include "Sound/SoundWave.h"
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
	using namespace ExecutionMutation;
	using namespace StateSemantic;

	namespace
	{
		bool Fail(
			FExecutionMutationError& OutError,
			FString Code,
			FString Detail = FString())
		{
			OutError = FExecutionMutationError(
				MoveTemp(Code), MoveTemp(Detail));
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
				return Fail(
					OutError,
					TEXT("editor_safe_exit_dirty_resolution_invalid"),
					Name + TEXT(" must be an array of resource refs"));
			}
			TSet<FString> UniqueValues;
			for (const FUeAgentCanonicalValueRef& Item : Value->GetArrayItems())
			{
				if (Item->GetKind() != EUeAgentValueKind::String)
				{
					return Fail(
						OutError,
						TEXT("editor_safe_exit_dirty_resolution_invalid"),
						Name + TEXT(" contains a non-string resource ref"));
				}
				const FString& ResourceRef = Item->GetText();
				FString Normalized = ResourceRef;
				Normalized.TrimStartAndEndInline();
				if (Normalized.IsEmpty() || Normalized != ResourceRef)
				{
					return Fail(
						OutError,
						TEXT("editor_safe_exit_dirty_resolution_invalid"),
						Name + TEXT(" contains an unnormalized resource ref"));
				}
				const FString PackageName =
					FPackageName::ObjectPathToPackageName(ResourceRef);
				if (!PackageName.StartsWith(TEXT("/Game/"))
					|| !FPackageName::IsValidLongPackageName(PackageName, false))
				{
					return Fail(
						OutError,
						TEXT("editor_safe_exit_dirty_resource_ref_invalid"),
						ResourceRef);
				}
				if (UniqueValues.Contains(PackageName))
				{
					return Fail(
						OutError,
						TEXT("editor_safe_exit_dirty_resource_ref_duplicate"),
						PackageName);
				}
				UniqueValues.Add(PackageName);
				OutValues.Add(PackageName);
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
			FString Error;
			if (!CanonicalJson::TryToJsonValue(Value, JsonValue, Error)
				|| !JsonValue.IsValid()
				|| JsonValue->Type != EJson::Object)
			{
				return Fail(
					OutError, TEXT("packaging_parameters_invalid"), Error);
			}
			OutObject = JsonValue->AsObject();
			return OutObject.IsValid();
		}

		void CollectDirtyProjectPackages(
			TArray<UPackage*>& OutPackages,
			TArray<FString>& OutPackageNames)
		{
			OutPackages.Reset();
			OutPackageNames.Reset();
			for (TObjectIterator<UPackage> It; It; ++It)
			{
				UPackage* Package = *It;
				if (!Package || !Package->IsDirty()
					|| Package->HasAnyFlags(RF_Transient)
					|| Package->HasAnyPackageFlags(PKG_CompiledIn))
				{
					continue;
				}
				const FString PackageName = Package->GetName();
				if (!PackageName.StartsWith(TEXT("/Game/"))
					|| !FPackageName::IsValidLongPackageName(PackageName, false))
				{
					continue;
				}
				OutPackages.Add(Package);
				OutPackageNames.Add(PackageName);
			}
			OutPackages.Sort([](const UPackage& A, const UPackage& B)
			{
				return A.GetName() < B.GetName();
			});
			OutPackageNames.Sort();
		}

		bool ExactResourceSetMatches(
			const TArray<FString>& Requested,
			const TArray<FString>& Dirty)
		{
			return Requested.Num() == Dirty.Num()
				&& Requested == Dirty;
		}

		bool TryReadSafeExitExternalWitnessRef(
			const FUeAgentCanonicalValue& Parameters,
			FUeAgentSafeExitExternalWitnessRef& OutRef,
			FExecutionMutationError& OutError)
		{
			OutRef = {};
			const FUeAgentCanonicalValue* Witness =
				FindField(Parameters, TEXT("external_witness"));
			if (!Witness || Witness->GetKind() != EUeAgentValueKind::Object
				|| StringField(*Witness, TEXT("schema"))
					!= TEXT("uai.safe_exit.external_witness_ref.v1"))
			{
				return Fail(
					OutError,
					TEXT("editor_safe_exit_external_witness_required"));
			}
			OutRef.LaunchId = StringField(*Witness, TEXT("launch_id"));
			OutRef.ExitAttemptId = StringField(
				*Witness, TEXT("exit_attempt_id"));
			OutRef.Nonce = StringField(*Witness, TEXT("nonce"));
			return OutRef.IsValid()
				? true
				: Fail(
					OutError,
					TEXT("editor_safe_exit_external_witness_ref_invalid"));
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
				return Fail(
					OutError,
					TEXT("editor_safe_exit_dirty_resolution_invalid"));
			}
			const FString Mode = StringField(*DirtyResolution, TEXT("mode"));
			TArray<FString> SaveResources;
			TArray<FString> DiscardResources;
			if ((Mode != TEXT("reject") && Mode != TEXT("save")
					&& Mode != TEXT("discard"))
				|| !TryStringArrayField(
					*DirtyResolution,
					TEXT("save_resources"),
					SaveResources,
					OutError)
				|| !TryStringArrayField(
					*DirtyResolution,
					TEXT("discard_resources"),
					DiscardResources,
					OutError))
			{
				if (!OutError.IsPresent())
				{
					return Fail(
						OutError,
						TEXT("editor_safe_exit_dirty_resolution_invalid"));
				}
				return false;
			}

			TArray<UPackage*> DirtyPackages;
			TArray<FString> DirtyPackageNames;
			CollectDirtyProjectPackages(DirtyPackages, DirtyPackageNames);
			const FString DirtyDetail = FString::Join(
				DirtyPackageNames, TEXT(","));
			if (Mode == TEXT("reject"))
			{
				if (!SaveResources.IsEmpty() || !DiscardResources.IsEmpty())
				{
					return Fail(
						OutError,
						TEXT("editor_safe_exit_dirty_resolution_invalid"),
						TEXT("reject mode does not accept save/discard resources"));
				}
				if (DirtyPackages.IsEmpty())
				{
					OutReceipt.Mode = Mode;
					OutReceipt.SaveResources = SaveResources;
					OutReceipt.DiscardResources = DiscardResources;
					return true;
				}
				return Fail(
						OutError,
						TEXT("editor_safe_exit_dirty_resources_present"),
						DirtyDetail);
			}

			const TArray<FString>& RequestedResources =
				Mode == TEXT("save") ? SaveResources : DiscardResources;
			const TArray<FString>& ForbiddenResources =
				Mode == TEXT("save") ? DiscardResources : SaveResources;
			if (!ForbiddenResources.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("editor_safe_exit_dirty_resolution_invalid"),
					TEXT("save and discard resource sets cannot be mixed"));
			}
			if (!ExactResourceSetMatches(RequestedResources, DirtyPackageNames))
			{
				return Fail(
					OutError,
					TEXT("editor_safe_exit_dirty_resource_set_mismatch"),
					DirtyDetail);
			}

			if (Mode == TEXT("save") && !DirtyPackages.IsEmpty()
				&& !UEditorLoadingAndSavingUtils::SavePackages(
					DirtyPackages, false))
			{
				return Fail(
					OutError,
					TEXT("editor_safe_exit_save_failed"),
					DirtyDetail);
			}
			if (Mode == TEXT("discard"))
			{
				for (UPackage* Package : DirtyPackages)
				{
					Package->SetDirtyFlag(false);
				}
			}

			CollectDirtyProjectPackages(DirtyPackages, DirtyPackageNames);
			if (!DirtyPackages.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("editor_safe_exit_dirty_resources_unresolved"),
					FString::Join(DirtyPackageNames, TEXT(",")));
			}
			OutReceipt.Mode = Mode;
			OutReceipt.SaveResources = MoveTemp(SaveResources);
			OutReceipt.DiscardResources = MoveTemp(DiscardResources);
			OutReceipt.RemainingDirtyResources = MoveTemp(DirtyPackageNames);
			return true;
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
			FString& OutError)
		{
			if (Size.X <= 0 || Size.Y <= 0
				|| Pixels.Num() != Size.X * Size.Y)
			{
				OutError = TEXT("capture_image_pixels_invalid");
				return false;
			}
			TArray64<uint8> Compressed;
			const FImageView View(
				const_cast<FColor*>(Pixels.GetData()),
				Size.X,
				Size.Y,
				ERawImageFormat::BGRA8);
			if (!FImageUtils::CompressImage(Compressed, TEXT("png"), View, 100))
			{
				OutError = TEXT("capture_image_compress_failed");
				return false;
			}
			TArray<uint8> Bytes;
			Bytes.Append(Compressed.GetData(), static_cast<int32>(Compressed.Num()));
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
			if (!FFileHelper::SaveArrayToFile(Bytes, *Path))
			{
				OutError = TEXT("capture_image_write_failed");
				return false;
			}
			return true;
		}

		bool WritePcm16Wav(
			const TArray<uint8>& Pcm,
			const uint32 SampleRate,
			const uint16 NumChannels,
			const FString& Path,
			FString& OutError)
		{
			constexpr uint16 BitsPerSample = 16;
			if (Pcm.IsEmpty() || SampleRate == 0 || NumChannels == 0
				|| Pcm.Num() > MAX_uint32 - 36)
			{
				OutError = TEXT("capture_media_preview_pcm_invalid");
				return false;
			}
			const uint16 BlockAlign = NumChannels * (BitsPerSample / 8);
			const uint32 ByteRate = SampleRate * BlockAlign;
			TArray<uint8> Bytes;
			Bytes.Reserve(44 + Pcm.Num());
			auto AppendFourCc = [&Bytes](const ANSICHAR* Value)
			{
				Bytes.Append(
					reinterpret_cast<const uint8*>(Value),
					4);
			};
			auto AppendU16 = [&Bytes](const uint16 Value)
			{
				Bytes.Add(static_cast<uint8>(Value & 0xff));
				Bytes.Add(static_cast<uint8>((Value >> 8) & 0xff));
			};
			auto AppendU32 = [&Bytes](const uint32 Value)
			{
				Bytes.Add(static_cast<uint8>(Value & 0xff));
				Bytes.Add(static_cast<uint8>((Value >> 8) & 0xff));
				Bytes.Add(static_cast<uint8>((Value >> 16) & 0xff));
				Bytes.Add(static_cast<uint8>((Value >> 24) & 0xff));
			};
			AppendFourCc("RIFF");
			AppendU32(36 + static_cast<uint32>(Pcm.Num()));
			AppendFourCc("WAVE");
			AppendFourCc("fmt ");
			AppendU32(16);
			AppendU16(1);
			AppendU16(NumChannels);
			AppendU32(SampleRate);
			AppendU32(ByteRate);
			AppendU16(BlockAlign);
			AppendU16(BitsPerSample);
			AppendFourCc("data");
			AppendU32(static_cast<uint32>(Pcm.Num()));
			Bytes.Append(Pcm);
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
			if (!FFileHelper::SaveArrayToFile(Bytes, *Path))
			{
				OutError = TEXT("capture_media_preview_wav_write_failed");
				return false;
			}
			return true;
		}

		bool HasOnlyFields(
			const FUeAgentCanonicalValue& Object,
			std::initializer_list<const TCHAR*> Allowed)
		{
			if (Object.GetKind() != EUeAgentValueKind::Object)
			{
				return false;
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
					return false;
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
			if (!Value || Value->GetKind() != EUeAgentValueKind::Array
				|| Value->GetArrayItems().IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("world_partition_resave_actor_guids_invalid"));
			}
			TSet<FGuid> UniqueGuids;
			for (const FUeAgentCanonicalValueRef& Item : Value->GetArrayItems())
			{
				FGuid Guid;
				if (Item->GetKind() != EUeAgentValueKind::String
					|| !FGuid::Parse(Item->GetText(), Guid)
					|| !Guid.IsValid()
					|| UniqueGuids.Contains(Guid))
				{
					return Fail(
						OutError,
						TEXT("world_partition_resave_actor_guids_invalid"));
				}
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
					return Fail(OutError, TEXT("compile_build_job_cancelled"));
				}
				if (Request.GetFamily()
					== CompileBuildJob::EUeAgentCompileBuildJobFamily::AssetCompile)
				{
					const FString ObjectPath = StripSubjectPrefix(
						Request.GetSubjectRef(), TEXT("asset:"));
					UObject* Asset = ObjectPath.IsEmpty()
						? nullptr
						: StaticLoadObject(
							UObject::StaticClass(), nullptr, *ObjectPath);
					if (UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
					{
						if (Request.GetPolicyId()
							== TEXT("policy:asset-compile:blueprint/v1"))
						{
							TSharedPtr<FJsonObject> Parameters;
							if (!TryCanonicalObjectJson(
									*Request.GetParameters(), Parameters, OutError))
							{
								return false;
							}
							CompileBuildJob::FBlueprintCompilePolicyRequest
								PolicyRequest;
							FString ErrorCode;
							FString ErrorDetail;
							if (!CompileBuildJob::
								TryDecodeBlueprintCompilePolicyRequest(
									Parameters, PolicyRequest,
									ErrorCode, ErrorDetail)
								|| !CompileBuildJob::TryExecuteBlueprintCompilePolicy(
									*Blueprint, PolicyRequest, Parameters,
									ErrorCode, ErrorDetail))
							{
								return Fail(OutError, ErrorCode, ErrorDetail);
							}
							FString ReportJson;
							FString JsonError;
							if (!Json::TryCanonicalize(
									Parameters.ToSharedRef(), ReportJson, JsonError))
							{
								return Fail(
									OutError,
									TEXT("blueprint_compile_report_invalid"),
									JsonError);
							}
							IFileManager::Get().MakeDirectory(
								*FPaths::GetPath(ReportPath), true);
							return FFileHelper::SaveStringToFile(
								ReportJson, *ReportPath,
								FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
								? true
								: Fail(OutError,
									TEXT("blueprint_compile_report_write_failed"));
						}
						if (!Request.GetParameters()->GetObjectItems().IsEmpty())
						{
							return Fail(
								OutError,
								TEXT("asset_compile_standard_input_invalid"));
						}
						FKismetEditorUtilities::CompileBlueprint(Blueprint);
						return true;
					}
					if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset))
					{
						StaticMesh->Build(false);
						return true;
					}
					return Fail(
						OutError,
						TEXT("asset_compile_subject_not_supported"),
						ObjectPath);
				}

				UWorld* ExplicitWorld = GEditor
					? GEditor->GetEditorWorldContext().World() : World.Get();
				const FString WorldPackage = StripSubjectPrefix(
					Request.GetSubjectRef(), TEXT("world:"));
				if (!ExplicitWorld || !ExplicitWorld->GetPackage()
					|| ExplicitWorld->GetPackage()->GetName() != WorldPackage)
				{
					return Fail(
						OutError,
						TEXT("world_build_subject_mismatch"),
						WorldPackage);
				}
				const FString& PolicyId = Request.GetPolicyId();
				if (PolicyId == TEXT("policy:world-build:standard/v1")
					|| PolicyId == TEXT("policy:world-build:navigation/v1"))
				{
					if (!Request.GetParameters()->GetObjectItems().IsEmpty())
					{
						return Fail(
							OutError, TEXT("world_navigation_build_input_invalid"));
					}
					return FEditorBuildUtils::EditorBuild(
							ExplicitWorld,
							FBuildOptions::BuildAIPaths,
							false)
						? true
						: Fail(OutError, TEXT("world_navigation_build_native_failed"));
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
					bool bAllowDelete = false;
					Parameters->TryGetBoolField(TEXT("allow_delete"), bAllowDelete);
					const int32 ExpectedFieldCount =
						(Parameters->HasField(TEXT("step")) ? 1 : 0)
						+ (Parameters->HasField(TEXT("allow_delete")) ? 1 : 0);
					if (!Flag || (Step == TEXT("delete") && !bAllowDelete))
					{
						return Fail(OutError, TEXT("world_hlod_plan_input_invalid"), Step);
					}
					if (Parameters->Values.Num() != ExpectedFieldCount)
					{
						return Fail(OutError, TEXT("world_hlod_plan_input_invalid"));
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
						return Fail(
							OutError,
							TEXT("world_partition_streaming_plan_input_invalid"));
					}
					if (!ExplicitWorld->GetWorldPartition())
					{
						return Fail(
							OutError, TEXT("world_partition_not_enabled"));
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
					if (!HasOnlyFields(
							*Request.GetParameters(), {TEXT("actor_guids")})
						|| !TryExactActorGuids(
							*Request.GetParameters(), ActorGuids, OutError))
					{
						return OutError.IsPresent()
							? false
							: Fail(
								OutError,
								TEXT("world_partition_resave_plan_input_invalid"));
					}
					if (!ExplicitWorld->GetWorldPartition())
					{
						return Fail(
							OutError, TEXT("world_partition_not_enabled"));
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
					return Fail(
						OutError, TEXT("world_build_policy_not_supported"), PolicyId);
				}
				Report->SetStringField(
					TEXT("commandlet_arguments"), CommandletArguments);
				Report->SetBoolField(TEXT("mutates_world"), false);
				FString ReportJson;
				FString JsonError;
				if (!Json::TryCanonicalize(Report, ReportJson, JsonError))
				{
					return Fail(OutError, TEXT("world_build_plan_report_invalid"), JsonError);
				}
				IFileManager::Get().MakeDirectory(*FPaths::GetPath(ReportPath), true);
				return FFileHelper::SaveStringToFile(
					ReportJson, *ReportPath,
					FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
					? true
					: Fail(OutError, TEXT("world_build_plan_report_write_failed"));
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
					return Fail(OutError, TEXT("packaging_job_cancelled"));
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
					if (!Parameters->TryGetStringField(TEXT("platform"), Platform)
						|| !Parameters->TryGetStringField(
							TEXT("configuration"), Configuration))
					{
						return Fail(OutError, TEXT("packaging_profile_input_invalid"));
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
					Report->SetBoolField(TEXT("valid"), bValid);
					Report->SetArrayField(TEXT("issues"), bValid
						? TArray<TSharedPtr<FJsonValue>>()
						: TArray<TSharedPtr<FJsonValue>>{
							MakeShared<FJsonValueString>(TEXT("profile_required_fields_missing"))});
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
						return Fail(OutError, TEXT("packaging_profile_input_invalid"));
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
						return Fail(OutError, TEXT("packaging_profile_input_invalid"));
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
						return Fail(OutError, TEXT("packaging_log_path_invalid"));
					}
					LogFile = FPaths::ConvertRelativePathToFull(LogFile);
					if (!LogFile.StartsWith(FPaths::ConvertRelativePathToFull(
						FPaths::ProjectSavedDir())))
					{
						return Fail(OutError, TEXT("packaging_log_path_unmanaged"));
					}
					FString Text;
					if (!FFileHelper::LoadFileToString(Text, *LogFile))
					{
						return Fail(OutError, TEXT("packaging_log_read_failed"));
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
							Errors.Add(MakeShared<FJsonValueString>(Line.Left(4096)));
						}
						else if (Line.Contains(TEXT("Warning:"), ESearchCase::IgnoreCase))
						{
							Warnings.Add(MakeShared<FJsonValueString>(Line.Left(4096)));
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
						return Fail(OutError, TEXT("packaging_archive_path_invalid"));
					}
					Parameters->TryGetBoolField(TEXT("include_hashes"), bIncludeHashes);
					ArchiveDir = FPaths::ConvertRelativePathToFull(ArchiveDir);
					const FString ManagedRoot = FPaths::ConvertRelativePathToFull(
						FPaths::ProjectSavedDir() / TEXT("UeAgentInterface/Packaging"));
					if (!ArchiveDir.StartsWith(ManagedRoot))
					{
						return Fail(OutError, TEXT("packaging_archive_path_unmanaged"));
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
					double MaximumDeleteBytes = 0.0;
					if (!Parameters->TryGetStringField(TEXT("archive_dir"), ArchiveDir)
						|| !Parameters->TryGetBoolField(TEXT("dry_run"), bDryRun)
						|| !Parameters->TryGetNumberField(
							TEXT("max_delete_bytes"), MaximumDeleteBytes)
						|| MaximumDeleteBytes < 0.0)
					{
						return Fail(OutError, TEXT("packaging_clean_input_invalid"));
					}
					ArchiveDir = FPaths::ConvertRelativePathToFull(ArchiveDir);
					const FString ManagedRoot = FPaths::ConvertRelativePathToFull(
						FPaths::ProjectSavedDir() / TEXT("UeAgentInterface/Packaging"));
					if (!ArchiveDir.StartsWith(ManagedRoot)
						|| ArchiveDir.Equals(ManagedRoot, ESearchCase::IgnoreCase))
					{
						return Fail(OutError, TEXT("packaging_archive_path_unmanaged"));
					}
					TArray<FString> Files;
					IFileManager::Get().FindFilesRecursive(Files, *ArchiveDir,
						TEXT("*"), true, false, false);
					int64 TotalSize = 0;
					for (const FString& File : Files)
					{
						TotalSize += FMath::Max<int64>(0, IFileManager::Get().FileSize(*File));
					}
					if (TotalSize > static_cast<int64>(MaximumDeleteBytes))
					{
						return Fail(OutError, TEXT("packaging_clean_budget_exceeded"));
					}
					if (!bDryRun && !IFileManager::Get().DeleteDirectory(
						*ArchiveDir, false, true))
					{
						return Fail(OutError, TEXT("packaging_clean_delete_failed"));
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
					return Fail(OutError, TEXT("packaging_policy_not_supported"), Policy);
				}

				FString ReportJson;
				FString JsonError;
				if (!Json::TryCanonicalize(Report, ReportJson, JsonError))
				{
					return Fail(OutError, TEXT("packaging_report_invalid"), JsonError);
				}
				IFileManager::Get().MakeDirectory(*FPaths::GetPath(ReportPath), true);
				return FFileHelper::SaveStringToFile(
					ReportJson, *ReportPath,
					FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
					? true
					: Fail(OutError, TEXT("packaging_report_write_failed"));
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
					return Fail(OutError, TEXT("owned_process_sink_released"));
				}
				if (!FFileHelper::SaveStringToFile(
						Chunk,
						*ReportPath,
						FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
						&IFileManager::Get(),
						FILEWRITE_Append))
				{
					return Fail(OutError, TEXT("owned_process_report_write_failed"));
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
			for (const FString& ConfigPath : ConfigPaths)
			{
				FString Resolved = FPaths::ConvertRelativePathToFull(ConfigPath);
				FPaths::NormalizeFilename(Resolved);
				if (!Resolved.StartsWith(ManagedPrefix, ESearchCase::IgnoreCase))
				{
					return Fail(
						OutError,
						TEXT("localization_owned_tool_cleanup_path_unmanaged"),
						Resolved);
				}
				if (FPaths::FileExists(Resolved)
					&& !IFileManager::Get().Delete(*Resolved, false, true, true))
				{
					return Fail(
						OutError,
						TEXT("localization_owned_tool_config_cleanup_failed"),
						Resolved);
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
					return Fail(OutError, TEXT("owned_process_sink_released"));
				}
				return FFileHelper::SaveStringToFile(
					Chunk,
					*LogPath,
					FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
					&IFileManager::Get(),
					FILEWRITE_Append)
					? true
					: Fail(OutError, TEXT("owned_process_report_write_failed"));
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
				bReleased = true;
				const bool bReportWritten = TryWriteReport(OutError);
				FExecutionMutationError CleanupError;
				const bool bCleaned = DeleteOwnedLocalizationConfigs(
					Preparation.ConfigPaths,
					CleanupError);
				if (!bCleaned && bReportWritten)
				{
					OutError = MoveTemp(CleanupError);
				}
				return bReportWritten && bCleaned;
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
				FString JsonError;
				if (!Json::TryCanonicalize(Report, CanonicalJson, JsonError))
				{
					return Fail(
						OutError,
						TEXT("localization_owned_tool_report_invalid"),
						JsonError);
				}
				return FFileHelper::SaveStringToFile(
					CanonicalJson,
					*ReportPath,
					FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
					? true
					: Fail(
						OutError,
						TEXT("localization_owned_tool_report_write_failed"));
			}

			FString ReportPath;
			FString LogPath;
			FString ReportRef;
			FLocalizationOwnedToolPreparation Preparation;
			mutable FCriticalSection Mutex;
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
				return Fail(OutError, TEXT("localization_owned_tool_policy_invalid"));
			}
			const FString TargetName = StripSubjectPrefix(
				Request.GetSubjectRef(), TEXT("localization:"));
			if (TargetName.IsEmpty()
				|| TargetName.Contains(TEXT("/"))
				|| TargetName.Contains(TEXT("\\"))
				|| TargetName.Contains(TEXT("\"")))
			{
				return Fail(OutError, TEXT("localization_owned_tool_subject_invalid"));
			}
			OutPreparation.TargetName = TargetName;

			TSharedPtr<FJsonObject> Parameters;
			if (!TryCanonicalObjectJson(
					*Request.GetParameters(), Parameters, OutError)
				|| !Parameters.IsValid())
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
			bool bAllowCommandlet = false;
			Parameters->TryGetBoolField(TEXT("allow_commandlet"), bAllowCommandlet);
			if (bExecute && (bGather || bCompile) && !bAllowCommandlet)
			{
				return Fail(
					OutError,
					TEXT("localization_owned_tool_commandlet_not_authorized"));
			}

			ULocalizationTarget* Target = FindLocalizationTarget(TargetName);
			if (!Target)
			{
				return Fail(
					OutError,
					TEXT("localization_owned_tool_target_not_found"),
					TargetName);
			}
			if ((bCompile || bExport)
				&& !Target->Settings.SupportedCulturesStatistics.IsValidIndex(
					Target->Settings.NativeCultureIndex))
			{
				return Fail(
					OutError,
					TEXT("localization_owned_tool_native_culture_invalid"),
					TargetName);
			}
			TArray<FString> Cultures;
			const TArray<TSharedPtr<FJsonValue>>* CultureValues = nullptr;
			if (Parameters->TryGetArrayField(TEXT("cultures"), CultureValues)
				&& CultureValues)
			{
				TSet<FString> UniqueCultures;
				for (const TSharedPtr<FJsonValue>& Value : *CultureValues)
				{
					FString Culture;
					if (!Value.IsValid()
						|| !Value->TryGetString(Culture)
						|| Culture.IsEmpty()
						|| UniqueCultures.Contains(Culture))
					{
						return Fail(
							OutError,
							TEXT("localization_owned_tool_cultures_invalid"));
					}
					const bool bSupported =
						Target->Settings.SupportedCulturesStatistics.ContainsByPredicate(
							[&Culture](const FCultureStatistics& Candidate)
							{
								return Candidate.CultureName == Culture;
							});
					if (!bSupported)
					{
						return Fail(
							OutError,
							TEXT("localization_owned_tool_culture_not_supported"),
							Culture);
					}
					UniqueCultures.Add(Culture);
					Cultures.Add(MoveTemp(Culture));
				}
			}
			OutPreparation.Cultures = Cultures;

			if (bGather || bCompile || bExport)
			{
				double TimeoutSeconds = 300.0;
				Parameters->TryGetNumberField(
					TEXT("timeout_seconds"), TimeoutSeconds);
				if (!FMath::IsFinite(TimeoutSeconds)
					|| TimeoutSeconds <= 0.0
					|| TimeoutSeconds > 300.0)
				{
					return Fail(
						OutError,
						TEXT("localization_owned_tool_timeout_seconds_invalid"));
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
					return Fail(
						OutError,
						TEXT("localization_owned_tool_output_dir_invalid"));
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
			auto WriteScript = [&](FLocalizationConfigurationScript Script) -> bool
			{
				const FString Path = ConfigDirectory
					/ (TargetName + TEXT("-")
						+ FGuid::NewGuid().ToString(EGuidFormats::Digits)
						+ TEXT(".ini"));
				if (!Script.Write(Path))
				{
					FExecutionMutationError Ignored;
					DeleteOwnedLocalizationConfigs(ConfigPaths, Ignored);
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
							Target)))
				{
					return Fail(
						OutError,
						TEXT("localization_gather_config_write_failed"));
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
					FLocalizationConfigurationScript Script = bCompile
						? LocalizationConfigurationScript::GenerateCompileTextConfigFile(
							Target, CultureOption)
						: LocalizationConfigurationScript::GenerateExportTextConfigFile(
							Target,
							CultureOption,
							TOptional<FString>(OutputDirectory));
					if (!WriteScript(MoveTemp(Script)))
					{
						return Fail(
							OutError,
							TEXT("localization_owned_tool_config_write_failed"));
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
				TEXT("-unattended -nop4 -nosplash -NullRHI -LiveCoding=false"),
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
						return Fail(
							OutError,
							TEXT("owned_tool_policy_input_not_trusted"));
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
						return Fail(
							OutError,
							TEXT("world_partition_resave_subject_invalid"));
					}
					TArray<FString> ActorGuids;
					if (Request.GetPolicyId()
							!= TEXT("policy:world-partition-resave:actors-run/v1")
						|| !HasOnlyFields(
							*Request.GetParameters(),
							{TEXT("actor_guids"), TEXT("allow_commandlet"),
								TEXT("allow_destructive")})
						|| !TryExactActorGuids(
							*Request.GetParameters(), ActorGuids, OutError)
						|| !BooleanField(
							*Request.GetParameters(), TEXT("allow_commandlet"))
						|| !BooleanField(
							*Request.GetParameters(), TEXT("allow_destructive")))
					{
						return OutError.IsPresent()
							? false
							: Fail(
								OutError,
								TEXT("world_partition_resave_execute_authorization_invalid"));
					}
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
						return Fail(OutError, TEXT("world_build_subject_invalid"));
					}
					TSharedPtr<FJsonObject> Parameters;
					if (!TryCanonicalObjectJson(
							*Request.GetParameters(), Parameters, OutError))
					{
						return false;
					}
					bool bAllowCommandlet = false;
					bool bAllowDestructive = false;
					Parameters->TryGetBoolField(
						TEXT("allow_commandlet"), bAllowCommandlet);
					Parameters->TryGetBoolField(
						TEXT("allow_destructive"), bAllowDestructive);
					if (!bAllowCommandlet || !bAllowDestructive)
					{
						return Fail(
							OutError, TEXT("world_build_execute_authorization_invalid"));
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
						bool bAllowCommandletRendering = false;
						bool bAllowRhi = false;
						Parameters->TryGetBoolField(
							TEXT("allow_delete"), bAllowDelete);
						Parameters->TryGetBoolField(
							TEXT("allow_commandlet_rendering"),
							bAllowCommandletRendering);
						Parameters->TryGetBoolField(TEXT("allow_rhi"), bAllowRhi);
						const int32 ExpectedFieldCount =
							(Parameters->HasField(TEXT("step")) ? 1 : 0)
							+ (Parameters->HasField(TEXT("allow_commandlet")) ? 1 : 0)
							+ (Parameters->HasField(TEXT("allow_destructive")) ? 1 : 0)
							+ (Parameters->HasField(TEXT("allow_delete")) ? 1 : 0)
							+ (Parameters->HasField(TEXT("allow_commandlet_rendering")) ? 1 : 0)
							+ (Parameters->HasField(TEXT("allow_rhi")) ? 1 : 0);
						if (!Flag || !bAllowCommandletRendering
							|| (Step == TEXT("delete") && !bAllowDelete)
							|| ((Step == TEXT("build") || Step == TEXT("finalize"))
								&& !bAllowRhi)
							|| Parameters->Values.Num() != ExpectedFieldCount)
						{
							return Fail(
								OutError, TEXT("world_hlod_execute_input_invalid"), Step);
						}
						BuilderArguments = FString::Printf(
							TEXT("-run=WorldPartitionBuilderCommandlet ")
							TEXT("-Builder=WorldPartitionHLODsBuilder %s ")
							TEXT("-AllowCommandletRendering"), **Flag);
						if (!bAllowRhi)
						{
							BuilderArguments += TEXT(" -NullRHI");
						}
					}
					else if (Request.GetPolicyId()
						== TEXT("policy:world-build:partition-streaming-run/v1"))
					{
						const int32 ExpectedFieldCount =
							(Parameters->HasField(TEXT("allow_commandlet")) ? 1 : 0)
							+ (Parameters->HasField(TEXT("allow_destructive")) ? 1 : 0);
						if (Parameters->Values.Num() != ExpectedFieldCount)
						{
							return Fail(
								OutError,
								TEXT("world_partition_streaming_execute_input_invalid"));
						}
						BuilderArguments =
							TEXT("-run=WorldPartitionBuilderCommandlet ")
							TEXT("-Builder=WorldPartitionBuilder ")
							TEXT("-AllowCommandletRendering");
					}
					else
					{
						return Fail(
							OutError, TEXT("world_build_execute_policy_invalid"));
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
						return Fail(OutError, TEXT("packaging_run_policy_invalid"));
					}
					TSharedPtr<FJsonObject> Parameters;
					if (!TryCanonicalObjectJson(
						*Request.GetParameters(), Parameters, OutError))
					{
						return false;
					}
					const TSharedPtr<FJsonObject>* Profile = nullptr;
					bool bAllowUat = false;
					if (!Parameters->TryGetBoolField(TEXT("allow_uat"), bAllowUat)
						|| !bAllowUat
						|| !Parameters->TryGetObjectField(TEXT("profile"), Profile)
						|| !Profile || !Profile->IsValid())
					{
						return Fail(OutError, TEXT("packaging_run_authorization_invalid"));
					}
					FString Platform;
					FString Configuration;
					FString ArchiveDir;
					if (!(*Profile)->TryGetStringField(TEXT("platform"), Platform)
						|| !(*Profile)->TryGetStringField(
							TEXT("configuration"), Configuration)
						|| !(*Profile)->TryGetStringField(TEXT("archive_dir"), ArchiveDir)
						|| (Platform != TEXT("Win64"))
						|| (Configuration != TEXT("Development")
							&& Configuration != TEXT("Shipping")))
					{
						return Fail(OutError, TEXT("packaging_run_profile_invalid"));
					}
					ArchiveDir = FPaths::ConvertRelativePathToFull(ArchiveDir);
					const FString ManagedRoot = FPaths::ConvertRelativePathToFull(
						FPaths::ProjectSavedDir() / TEXT("UeAgentInterface/Packaging"));
					if (!ArchiveDir.StartsWith(ManagedRoot))
					{
						return Fail(OutError, TEXT("packaging_archive_path_unmanaged"));
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
					if (bLocalizationOwnedTool)
					{
						FExecutionMutationError Ignored;
						DeleteOwnedLocalizationConfigs(
							LocalizationPreparation.ConfigPaths,
							Ignored);
					}
					return Fail(OutError, TEXT("owned_process_executable_unavailable"));
				}
				if (!FPaths::FileExists(Executable))
				{
					if (bLocalizationOwnedTool)
					{
						FExecutionMutationError Ignored;
						DeleteOwnedLocalizationConfigs(
							LocalizationPreparation.ConfigPaths,
							Ignored);
					}
					return Fail(OutError, TEXT("owned_process_executable_unavailable"));
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
					if (bLocalizationOwnedTool)
					{
						FExecutionMutationError Ignored;
						DeleteOwnedLocalizationConfigs(
							LocalizationPreparation.ConfigPaths,
							Ignored);
					}
					return Fail(OutError, TEXT("owned_process_report_create_failed"));
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
						FExecutionMutationError Ignored;
						DeleteOwnedLocalizationConfigs(
							LocalizationPreparation.ConfigPaths,
							Ignored);
						return Fail(
							OutError,
							TEXT("owned_process_report_create_failed"));
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
					return Fail(OutError, TEXT("editor_native_session_already_started"));
				}
				if (Cancellation.IsCancelled())
				{
					return Fail(
						OutError,
						TEXT("editor_native_session_cancelled_before_start"));
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
					return Fail(OutError, TEXT("editor_native_session_not_started"));
				}
				ELongRunningWorkState WorkState = ELongRunningWorkState::Running;
				FString Phase = TEXT("scheduled");
				if (State == EEditorNativeState::Running)
				{
					Phase = TEXT("active");
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
						ErrorCode,
						SessionRef);
				return true;
			}

			bool RequestStop(
				const FString& Reason,
				FExecutionMutationError& OutError) override
			{
				{
					FScopeLock Lock(&Mutex);
					if (bStopRequested)
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
				return State == EEditorNativeState::ExitedSuccess
					|| State == EEditorNativeState::ExitedFailure
					? true
					: Fail(OutError, TEXT("editor_native_session_join_before_exit"));
			}

			bool Release(FExecutionMutationError& OutError) override
			{
				FScopeLock Lock(&Mutex);
				bReleased = true;
				return true;
			}

		private:
			UWorld* CurrentEditorWorld() const
			{
				return GEditor ? GEditor->GetEditorWorldContext().World() : World.Get();
			}

			bool StartWorldContext(
				FString& OutSessionRef,
				bool& bOutKeepRunning,
				FString& OutError)
			{
				bOutKeepRunning = false;
				const FString WorldPackage = StripSubjectPrefix(
					Request.GetSubjectRef(), TEXT("world:"));
				if (!FPackageName::IsValidLongPackageName(WorldPackage, false))
				{
					OutError = TEXT("world_context_subject_invalid");
					return false;
				}
				TSharedPtr<FJsonObject> Parameters;
				FExecutionMutationError ParseError;
				if (!TryCanonicalObjectJson(
						*Request.GetParameters(), Parameters, ParseError))
				{
					OutError = ParseError.GetCode();
					return false;
				}
				const FString& PolicyId = Request.GetPolicyId();
				if (PolicyId == TEXT("policy:world-context:set-current/v1"))
				{
					bool bSaveExistingMap = true;
					Parameters->TryGetBoolField(
						TEXT("save_existing_map"), bSaveExistingMap);
					if (Parameters->Values.Num()
						> (Parameters->HasField(TEXT("save_existing_map")) ? 1 : 0))
					{
						OutError = TEXT("world_context_open_input_invalid");
						return false;
					}
					UWorld* Existing = CurrentEditorWorld();
					if (Existing && Existing->GetPackage()
						&& Existing->GetPackage()->GetName() == WorldPackage)
					{
						OutSessionRef = TEXT("world-context:") + WorldPackage;
						return true;
					}
					if (bSaveExistingMap
						&& !UEditorLoadingAndSavingUtils::SaveDirtyPackages(true, false))
					{
						OutError = TEXT("world_context_save_existing_failed");
						return false;
					}
					const FString Filename = FPackageName::LongPackageNameToFilename(
						WorldPackage, FPackageName::GetMapPackageExtension());
					if (!FPackageName::DoesPackageExist(WorldPackage)
						&& !FPaths::FileExists(Filename))
					{
						OutError = TEXT("world_context_map_not_found");
						return false;
					}
					UWorld* Loaded = UEditorLoadingAndSavingUtils::LoadMap(Filename);
					if (!Loaded || !Loaded->GetPackage()
						|| Loaded->GetPackage()->GetName() != WorldPackage)
					{
						OutError = TEXT("world_context_load_failed");
						return false;
					}
					OutSessionRef = TEXT("world-context:") + WorldPackage;
					return true;
				}

				if (PolicyId == TEXT("policy:world-context:create-blank/v1"))
				{
					FString OverwritePolicy(TEXT("fail"));
					Parameters->TryGetStringField(
						TEXT("overwrite_policy"), OverwritePolicy);
					OverwritePolicy.ToLowerInline();
					bool bSaveExistingMap = true;
					Parameters->TryGetBoolField(
						TEXT("save_existing_map"), bSaveExistingMap);
					bool bCreateWorldPartition = false;
					Parameters->TryGetBoolField(
						TEXT("create_world_partition"), bCreateWorldPartition);
					if ((OverwritePolicy != TEXT("fail")
							&& OverwritePolicy != TEXT("reuse")
							&& OverwritePolicy != TEXT("replace"))
						|| Parameters->Values.Num()
							> (Parameters->HasField(TEXT("overwrite_policy")) ? 1 : 0)
								+ (Parameters->HasField(TEXT("save_existing_map")) ? 1 : 0)
								+ (Parameters->HasField(TEXT("create_world_partition")) ? 1 : 0))
					{
						OutError = TEXT("world_context_create_input_invalid");
						return false;
					}
					const bool bExists = FPackageName::DoesPackageExist(WorldPackage);
					if (bExists && OverwritePolicy == TEXT("fail"))
					{
						OutError = TEXT("world_context_map_exists");
						return false;
					}
					if (bExists && OverwritePolicy == TEXT("reuse"))
					{
						const FString Filename = FPackageName::LongPackageNameToFilename(
							WorldPackage, FPackageName::GetMapPackageExtension());
						UWorld* Loaded = UEditorLoadingAndSavingUtils::LoadMap(Filename);
						if (!Loaded)
						{
							OutError = TEXT("world_context_reuse_failed");
							return false;
						}
						OutSessionRef = TEXT("world-context:reused:") + WorldPackage;
						return true;
					}
					UWorld* Created =
						UEditorLoadingAndSavingUtils::NewBlankMap(bSaveExistingMap);
					if (!Created)
					{
						OutError = TEXT("world_context_create_failed");
						return false;
					}
					if (bCreateWorldPartition)
					{
						if (!Created->PersistentLevel)
						{
							OutError = TEXT("world_context_persistent_level_missing");
							return false;
						}
						Created->PersistentLevel->SetUseExternalActors(true);
						UWorldPartition* Partition =
							UWorldPartition::CreateOrRepairWorldPartition(
								Created->GetWorldSettings());
						if (!Partition)
						{
							OutError = TEXT("world_context_partition_create_failed");
							return false;
						}
						if (!Partition->IsInitialized())
						{
							Partition->Initialize(Created, FTransform::Identity);
						}
					}
					if (!UEditorLoadingAndSavingUtils::SaveMap(Created, WorldPackage))
					{
						OutError = TEXT("world_context_create_save_failed");
						return false;
					}
					OutSessionRef = TEXT("world-context:created:") + WorldPackage;
					return true;
				}

				UWorld* ExplicitWorld = CurrentEditorWorld();
				if (!ExplicitWorld || !ExplicitWorld->GetPackage()
					|| ExplicitWorld->GetPackage()->GetName() != WorldPackage)
				{
					OutError = TEXT("world_partition_loader_subject_mismatch");
					return false;
				}
				if (!ExplicitWorld->GetWorldPartition())
				{
					OutError = TEXT("world_partition_not_enabled");
					return false;
				}
				bool bDryRun = false;
				Parameters->TryGetBoolField(TEXT("dry_run"), bDryRun);
				if (PolicyId
					== TEXT("policy:world-context:partition-region-loader/v1"))
				{
					const int32 ExpectedFieldCount =
						(Parameters->HasField(TEXT("bounds")) ? 1 : 0)
						+ (Parameters->HasField(TEXT("label")) ? 1 : 0)
						+ (Parameters->HasField(TEXT("dry_run")) ? 1 : 0);
					if (Parameters->Values.Num() != ExpectedFieldCount)
					{
						OutError = TEXT("world_partition_region_input_invalid");
						return false;
					}
					const TSharedPtr<FJsonObject>* Bounds = nullptr;
					const TArray<TSharedPtr<FJsonValue>>* Center = nullptr;
					const TArray<TSharedPtr<FJsonValue>>* Extent = nullptr;
					if (!Parameters->TryGetObjectField(TEXT("bounds"), Bounds)
						|| !Bounds || !Bounds->IsValid()
						|| !(*Bounds)->TryGetArrayField(TEXT("center"), Center)
						|| !(*Bounds)->TryGetArrayField(TEXT("extent"), Extent)
						|| !Center || !Extent || Center->Num() != 3 || Extent->Num() != 3)
					{
						OutError = TEXT("world_partition_region_bounds_invalid");
						return false;
					}
					const FVector CenterValue(
						(*Center)[0]->AsNumber(), (*Center)[1]->AsNumber(),
						(*Center)[2]->AsNumber());
					const FVector ExtentValue(
						(*Extent)[0]->AsNumber(), (*Extent)[1]->AsNumber(),
						(*Extent)[2]->AsNumber());
					if (!FMath::IsFinite(CenterValue.X)
						|| !FMath::IsFinite(CenterValue.Y)
						|| !FMath::IsFinite(CenterValue.Z)
						|| !FMath::IsFinite(ExtentValue.X)
						|| !FMath::IsFinite(ExtentValue.Y)
						|| !FMath::IsFinite(ExtentValue.Z)
						|| ExtentValue.GetMin() < 0.0)
					{
						OutError = TEXT("world_partition_region_bounds_invalid");
						return false;
					}
					FString Label(TEXT("UAI_WP_Region"));
					Parameters->TryGetStringField(TEXT("label"), Label);
					if (Label.TrimStartAndEnd().IsEmpty())
					{
						OutError = TEXT("world_partition_region_label_invalid");
						return false;
					}
					if (!bDryRun)
					{
						WorldPartitionLoader = MakeUnique<FLoaderAdapterShape>(
							ExplicitWorld, FBox::BuildAABB(CenterValue, ExtentValue), Label);
					}
				}
				else if (PolicyId
					== TEXT("policy:world-context:partition-actor-loader/v1"))
				{
					const int32 ExpectedFieldCount =
						(Parameters->HasField(TEXT("actor_guids")) ? 1 : 0)
						+ (Parameters->HasField(TEXT("actor_ids")) ? 1 : 0)
						+ (Parameters->HasField(TEXT("dry_run")) ? 1 : 0);
					if (Parameters->Values.Num() != ExpectedFieldCount)
					{
						OutError = TEXT("world_partition_actor_loader_input_invalid");
						return false;
					}
					TArray<FGuid> Guids;
					const TArray<TSharedPtr<FJsonValue>>* GuidValues = nullptr;
					if (Parameters->TryGetArrayField(TEXT("actor_guids"), GuidValues)
						&& GuidValues)
					{
						for (const TSharedPtr<FJsonValue>& Value : *GuidValues)
						{
							FGuid Guid;
							if (!Value.IsValid() || !FGuid::Parse(Value->AsString(), Guid))
							{
								OutError = TEXT("world_partition_actor_guid_invalid");
								return false;
							}
							Guids.AddUnique(Guid);
						}
					}
					const TArray<TSharedPtr<FJsonValue>>* ActorIds = nullptr;
					if (Parameters->TryGetArrayField(TEXT("actor_ids"), ActorIds)
						&& ActorIds)
					{
						for (const TSharedPtr<FJsonValue>& Value : *ActorIds)
						{
							const FString Identity = Value.IsValid()
								? Value->AsString() : FString();
							AActor* Match = nullptr;
							for (TActorIterator<AActor> It(ExplicitWorld); It; ++It)
							{
								if (It->GetPathName() == Identity
									|| It->GetName() == Identity
									|| It->GetActorLabel() == Identity)
								{
									Match = *It;
									break;
								}
							}
							if (!Match)
							{
								OutError = TEXT("world_partition_actor_identity_unresolved");
								return false;
							}
							Guids.AddUnique(Match->GetActorGuid());
						}
					}
					if (Guids.IsEmpty())
					{
						OutError = TEXT("world_partition_actor_loader_empty");
						return false;
					}
					if (!bDryRun)
					{
						TUniquePtr<FLoaderAdapterActorList> ActorLoader =
							MakeUnique<FLoaderAdapterActorList>(ExplicitWorld);
						ActorLoader->AddActors(Guids);
						WorldPartitionLoader = MoveTemp(ActorLoader);
					}
				}
				else
				{
					OutError = TEXT("world_context_policy_not_supported");
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
				FString& OutError)
			{
				const auto Family = Request.GetFamily();
				const FUeAgentCanonicalValue& Parameters =
					Request.GetParameters().Get();
				FVector3d Size = FVector3d::ZeroVector;
				if (Family == EditorSessionOperation::
					EUeAgentEditorSessionOperationFamily::ModelingTool)
				{
					if (Request.GetSubjectRef()
							!= TEXT("modeling-session:operation-local")
						|| StringField(Parameters, TEXT("action_id"))
							!= TEXT("spawn_transient_box")
						|| !HasOnlyFields(
							Parameters,
							{TEXT("action_id"), TEXT("size_x"),
							 TEXT("size_y"), TEXT("size_z")}))
					{
						OutError = TEXT("modeling_session_policy_input_invalid");
						return false;
					}
					Size = FVector3d(
						DecimalField(Parameters, TEXT("size_x"), 100.0),
						DecimalField(Parameters, TEXT("size_y"), 100.0),
						DecimalField(Parameters, TEXT("size_z"), 100.0));
				}
				else
				{
					if (Request.GetSubjectRef()
							!= TEXT("voxel-session:operation-local")
						|| !HasOnlyFields(
							Parameters,
							{TEXT("cell_size"), TEXT("cells_x"),
							 TEXT("cells_y"), TEXT("cells_z")}))
					{
						OutError = TEXT("voxel_session_policy_input_invalid");
						return false;
					}
					const double CellSize = DecimalField(
						Parameters, TEXT("cell_size"), 100.0);
					const double CountXValue = DecimalField(
						Parameters, TEXT("cells_x"), 1.0);
					const double CountYValue = DecimalField(
						Parameters, TEXT("cells_y"), 1.0);
					const double CountZValue = DecimalField(
						Parameters, TEXT("cells_z"), 1.0);
					const int32 CountX = FMath::RoundToInt(CountXValue);
					const int32 CountY = FMath::RoundToInt(CountYValue);
					const int32 CountZ = FMath::RoundToInt(CountZValue);
					if (!FMath::IsFinite(CellSize) || CellSize <= 0.0
						|| !FMath::IsNearlyEqual(CountXValue, CountX)
						|| !FMath::IsNearlyEqual(CountYValue, CountY)
						|| !FMath::IsNearlyEqual(CountZValue, CountZ)
						|| CountX <= 0 || CountY <= 0 || CountZ <= 0
						|| CountX > 1024 || CountY > 1024 || CountZ > 1024)
					{
						OutError = TEXT("voxel_session_dimensions_invalid");
						return false;
					}
					Size = FVector3d(
						CellSize * CountX,
						CellSize * CountY,
						CellSize * CountZ);
				}

				if (!FMath::IsFinite(Size.X) || !FMath::IsFinite(Size.Y)
					|| !FMath::IsFinite(Size.Z) || Size.X <= 0.0
					|| Size.Y <= 0.0 || Size.Z <= 0.0
					|| Size.X > 100000.0 || Size.Y > 100000.0
					|| Size.Z > 100000.0)
				{
					OutError = TEXT("editor_dynamic_mesh_dimensions_invalid");
					return false;
				}
				UWorld* ExplicitWorld = World.Get();
				if (!ExplicitWorld)
				{
					OutError = TEXT("editor_dynamic_mesh_world_unavailable");
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
					OutError = TEXT("editor_dynamic_mesh_spawn_failed");
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
					OutError = TEXT("editor_dynamic_mesh_build_failed");
					return false;
				}
				Component->NotifyMeshUpdated();
				FString Label = NativeResourceId;
				Label.ReplaceInline(TEXT(":"), TEXT("_"));
				Actor->SetActorLabel(TEXT("UAI_") + Label);
				Actor->Tags.AddUnique(
					Family == EditorSessionOperation::
						EUeAgentEditorSessionOperationFamily::ModelingTool
					? TEXT("UAI.OperationOwned.Modeling")
					: TEXT("UAI.OperationOwned.Voxel"));
				OwnedActor = Actor;
				OutSessionRef = Actor->GetPathName();
				return true;
			}

			void StartOnGameThread()
			{
				FString LocalError;
				FString LocalSessionRef;
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
					UAssetEditorSubsystem* Subsystem = GEditor
						? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
					bSuccess = Asset && Subsystem
						&& Subsystem->OpenEditorForAsset(Asset);
					if (bSuccess)
					{
						OpenedAsset = Asset;
						LocalSessionRef = TEXT("asset-editor:") + ObjectPath;
						bKeepRunning = true;
					}
					else
					{
						LocalError = TEXT("asset_editor_open_failed");
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
						if (!HasOnlyFields(
								Parameters,
								{TEXT("culture"), TEXT("restore_after"), TEXT("scope")})
							|| Scope != TEXT("session")
							|| Request.GetSubjectRef() != TEXT("culture:") + Culture
							|| !FInternationalization::Get().GetCulture(Culture).IsValid())
						{
							LocalError = TEXT("culture_preview_policy_input_invalid");
						}
						else
						{
							PreviousCulture = FInternationalization::Get()
								.GetCurrentCulture()->GetName();
							bSuccess = FInternationalization::Get()
								.SetCurrentCulture(Culture);
							if (!bSuccess)
							{
								LocalError = TEXT("culture_preview_set_failed");
							}
							else if (bRestoreAfter)
							{
								bSuccess = FInternationalization::Get()
									.SetCurrentCulture(PreviousCulture);
								LocalError = bSuccess ? FString()
									: FString(TEXT("culture_preview_restore_failed"));
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
						LocalError = TEXT("runtime_preview_already_active_or_unavailable");
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
				else if (Family == EditorSessionOperation::
					EUeAgentEditorSessionOperationFamily::VoxelGrid)
				{
					bSuccess = StartOperationOwnedDynamicMesh(
						LocalSessionRef, LocalError);
					bKeepRunning = bSuccess;
				}
				else
				{
					LocalError = TEXT("editor_session_family_not_supported");
				}

				FScopeLock Lock(&Mutex);
				SessionRef = MoveTemp(LocalSessionRef);
				ErrorCode = MoveTemp(LocalError);
				State = bSuccess
					? (bKeepRunning
						? EEditorNativeState::Running
						: EEditorNativeState::ExitedSuccess)
					: EEditorNativeState::ExitedFailure;
			}

			void StopOnGameThread()
			{
				const auto Family = Request.GetFamily();
				if (Family == EditorSessionOperation::
						EUeAgentEditorSessionOperationFamily::ModelingTool
					|| Family == EditorSessionOperation::
						EUeAgentEditorSessionOperationFamily::VoxelGrid)
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
					if (UObject* Asset = OpenedAsset.Get())
					{
						if (UAssetEditorSubsystem* Subsystem = GEditor
							? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()
							: nullptr)
						{
							Subsystem->CloseAllEditorsForAsset(Asset);
						}
					}
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
						State = bRestored
							? EEditorNativeState::ExitedSuccess
							: EEditorNativeState::ExitedFailure;
						ErrorCode = bRestored ? FString()
							: FString(TEXT("culture_preview_restore_failed"));
						return;
					}
					if (!GUnrealEd)
					{
						FScopeLock Lock(&Mutex);
						State = EEditorNativeState::ExitedFailure;
						ErrorCode = TEXT("runtime_preview_stop_unavailable");
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
									Self->State = EEditorNativeState::ExitedSuccess;
									Self->ErrorCode.Reset();
									return false;
								}),
							0.05f);
					FScopeLock Lock(&Mutex);
					if (!Handle.IsValid())
					{
						State = EEditorNativeState::ExitedFailure;
						ErrorCode =
							TEXT("runtime_preview_stop_poll_schedule_failed");
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
				State = EEditorNativeState::ExitedSuccess;
				ErrorCode.Reset();
			}

			EditorSessionOperation::FUeAgentEditorSessionOperationTypedRequest Request;
			TWeakObjectPtr<UWorld> World;
			TWeakObjectPtr<UObject> OpenedAsset;
			TWeakObjectPtr<ADynamicMeshActor> OwnedActor;
			TUniquePtr<IWorldPartitionActorLoaderInterface::ILoaderAdapter>
				WorldPartitionLoader;
			FCriticalSection Mutex;
			FString NativeResourceId;
			FString SessionRef;
			FString ErrorCode;
			FString PreviousCulture;
			FTSTicker::FDelegateHandle StopPollTicker;
			EEditorNativeState State = EEditorNativeState::Scheduled;
			bool bStarted = false;
			bool bStopRequested = false;
			bool bReleased = false;
			bool bCulturePreviewActive = false;
		};

		class FEditorSessionBuilder final
			: public EditorSessionOperation::IUeAgentEditorSessionOperationPortBuilder
		{
		public:
			FEditorSessionBuilder(
				FString InEditorInstanceId,
				TWeakObjectPtr<UWorld> InWorld)
				: EditorInstanceId(MoveTemp(InEditorInstanceId)), World(InWorld)
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
				FString InPublisherOwnerId)
				: Request(MoveTemp(InRequest))
				, World(InWorld)
				, PublisherOwnerId(MoveTemp(InPublisherOwnerId))
			{
			}

			~FCaptureNativeSession() override
			{
				FTSTicker::FDelegateHandle StopHandle;
				FTSTicker::FDelegateHandle ExportHandle;
				{
					FScopeLock Lock(&Mutex);
					StopHandle = MediaStopTicker;
					MediaStopTicker.Reset();
					ExportHandle = MediaExportPollTicker;
					MediaExportPollTicker.Reset();
				}
				if (StopHandle.IsValid())
				{
					FTSTicker::GetCoreTicker().RemoveTicker(StopHandle);
				}
				if (ExportHandle.IsValid())
				{
					FTSTicker::GetCoreTicker().RemoveTicker(ExportHandle);
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
					return Fail(OutError, TEXT("capture_native_session_already_started"));
				}
				if (Cancellation.IsCancelled())
				{
					return Fail(
						OutError,
						TEXT("capture_native_session_cancelled_before_start"));
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
					return Fail(OutError, TEXT("capture_native_session_not_started"));
				}
				if (State == ECaptureNativeState::NativeReady && !Artifact.IsSet())
				{
					if (!Resources)
					{
						return Fail(
							OutError,
							TEXT("capture_resource_tracker_unavailable"));
					}
					FFileArtifactReference Published;
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
						return Fail(
							OutError,
							PublishError.GetCode(),
							PublishError.GetDetail());
					}
					Artifact = MoveTemp(Published);
					State = ECaptureNativeState::ExitedSuccess;
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
						ErrorCode,
						Artifact);
				return true;
			}

			bool RequestStop(
				const FString& Reason,
				FExecutionMutationError& OutError) override
			{
				FTSTicker::FDelegateHandle Handle;
				{
					FScopeLock Lock(&Mutex);
					bStopRequested = true;
					Handle = MediaStopTicker;
					MediaStopTicker.Reset();
				}
				if (Handle.IsValid())
				{
					FTSTicker::GetCoreTicker().RemoveTicker(Handle);
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
					: Fail(OutError, TEXT("capture_native_join_before_exit"));
			}

			bool Release(FExecutionMutationError& OutError) override
			{
				FScopeLock Lock(&Mutex);
				if (!NativePath.IsEmpty())
				{
					IFileManager::Get().Delete(*NativePath, false, true, true);
				}
				bReleased = true;
				return true;
			}

		private:
			void FailOnGameThread(FString Code)
			{
				FScopeLock Lock(&Mutex);
				ErrorCode = MoveTemp(Code);
				State = ECaptureNativeState::ExitedFailure;
			}

			void StartOnGameThread()
			{
				{
					FScopeLock Lock(&Mutex);
					if (bStopRequested)
					{
						ErrorCode = TEXT("capture_cancelled_before_start");
						State = ECaptureNativeState::ExitedFailure;
						return;
					}
					State = ECaptureNativeState::Running;
				}
				const FString Root = MakeNativeRoot(OperationId);
				IFileManager::Get().MakeDirectory(*Root, true);
				if (Request.GetFamily()
					== CaptureMediaLongJob::EUeAgentCaptureMediaLongJobFamily::
						MediaRecording)
				{
					UWorld* ExplicitWorld = World.Get();
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
							FailOnGameThread(
								TEXT("capture_media_preview_sound_unavailable"));
							return;
						}
						TArray<uint8> RawPcm;
						uint32 SampleRate = 0;
						uint16 NumChannels = 0;
						if (!PreviewSound->GetImportedSoundWaveData(
								RawPcm, SampleRate, NumChannels))
						{
							FailOnGameThread(
								TEXT("capture_media_preview_pcm_unavailable"));
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
						FString WriteError;
						if (!WritePcm16Wav(
								RawPcm,
								SampleRate,
								NumChannels,
								NativePath,
								WriteError))
						{
							FailOnGameThread(MoveTemp(WriteError));
							return;
						}
						FScopeLock Lock(&Mutex);
						State = ECaptureNativeState::NativeReady;
						return;
					}
					if (!ExplicitWorld || !ExplicitWorld->GetAudioDeviceRaw())
					{
						FailOnGameThread(TEXT("capture_media_audio_device_unavailable"));
						return;
					}
					const FString SubmixObject = StripSubjectPrefix(
						Request.GetSubjectRef(), TEXT("media-source:audio-submix:"));
					Submix = SubmixObject.IsEmpty() || SubmixObject == TEXT("master")
						? nullptr
						: Cast<USoundSubmix>(StaticLoadObject(
							USoundSubmix::StaticClass(), nullptr, *SubmixObject));
					const double ExpectedDuration = FMath::Clamp(
						DecimalField(Request.GetParameters().Get(), TEXT("expected_duration"), 5.0),
						0.01,
						300.0);
					NativePath = Root / TEXT("recording.wav");
					FinalPath = FPaths::ProjectSavedDir()
						/ TEXT("UeAgentInterface/LongRunningSession/Artifacts")
						/ (FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".wav"));
					ArtifactKind = TEXT("audio_recording");
					Format = TEXT("wav");
					MimeType = TEXT("audio/wav");
					UAudioMixerBlueprintLibrary::StartRecordingOutput(
						ExplicitWorld,
						static_cast<float>(ExpectedDuration),
						Submix.Get());
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
									{
										FScopeLock Lock(&Self->Mutex);
										Self->MediaStopTicker.Reset();
									}
									Self->StopMediaOnGameThread();
									return false;
								}),
							static_cast<float>(ExpectedDuration));
					if (!Handle.IsValid())
					{
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
				FString LocalError;
				if (Request.GetFamily()
					== CaptureMediaLongJob::EUeAgentCaptureMediaLongJobFamily::
						ImageCapture)
				{
					const FString PreferredPath = Root / TEXT("capture.png");
					FString RequestedFinalPath;
					if (!CaptureMediaLongJob::TryExecuteCaptureMediaImagePolicy(
							Request,
							PreferredPath,
							NativePath,
							RequestedFinalPath,
							Format,
							LocalError))
					{
						FailOnGameThread(MoveTemp(LocalError));
						return;
					}
					FinalPath = RequestedFinalPath.IsEmpty()
						? FPaths::ProjectSavedDir()
							/ TEXT("UeAgentInterface/LongRunningSession/Artifacts")
							/ (FGuid::NewGuid().ToString(EGuidFormats::Digits)
								+ TEXT(".") + Format)
						: RequestedFinalPath;
					ArtifactKind = TEXT("image");
					MimeType = Format == TEXT("png")
						? FString(TEXT("image/png")) : FString(TEXT("image/jpeg"));
					FScopeLock Lock(&Mutex);
					State = ECaptureNativeState::NativeReady;
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
							FailOnGameThread(
								TEXT("capture_render_operation_local_size_invalid"));
							return;
						}
						FUeAgentResourceBudgetExceeded BudgetError;
						const int64 PixelCount = Width * Height;
						if (PixelCount > MAX_int64 / static_cast<int64>(sizeof(FColor)))
						{
							FailOnGameThread(
								TEXT("capture_render_operation_local_size_invalid"));
							return;
						}
						if (!Resources->TryChargeMany(
							{
								{EUeAgentResourceDimension::WorkUnits, PixelCount},
								{EUeAgentResourceDimension::EstimatedMemoryBytes,
									PixelCount * static_cast<int64>(sizeof(FColor))},
							},
							TEXT("capture.render.operation_local"),
							BudgetError))
						{
							FailOnGameThread(BudgetError.Code);
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
					UWorld* ExplicitWorld = World.Get();
					if (!Target || !ExplicitWorld || !Target->GameThread_GetRenderTargetResource())
					{
						if (bOperationLocalTarget && Target)
						{
							Target->ReleaseResource();
							Target->RemoveFromRoot();
						}
						FailOnGameThread(TEXT("capture_render_target_unavailable"));
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
						FailOnGameThread(TEXT("capture_render_read_pixels_failed"));
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

			void StopMediaOnGameThread()
			{
				UWorld* ExplicitWorld = World.Get();
				if (!ExplicitWorld || !ExplicitWorld->GetAudioDeviceRaw())
				{
					FailOnGameThread(TEXT("capture_media_audio_device_unavailable"));
					return;
				}
				const FString Directory = FPaths::GetPath(NativePath);
				const FString Name = FPaths::GetBaseFilename(NativePath);
				IFileManager::Get().MakeDirectory(*Directory, true);
				UAudioMixerBlueprintLibrary::StopRecordingOutput(
					ExplicitWorld,
					EAudioRecordingExportType::WavFile,
					Name,
					Directory,
					Submix.Get());
				if (FPaths::FileExists(NativePath))
				{
					FScopeLock Lock(&Mutex);
					State = ECaptureNativeState::NativeReady;
					return;
				}
				const TWeakPtr<FCaptureNativeSession, ESPMode::ThreadSafe>
					WeakSelf = AsShared();
				const double Deadline = FPlatformTime::Seconds() + 5.0;
				const FTSTicker::FDelegateHandle Handle =
					FTSTicker::GetCoreTicker().AddTicker(
						FTickerDelegate::CreateLambda(
							[WeakSelf, Deadline](float)
							{
								const TSharedPtr<FCaptureNativeSession,
									ESPMode::ThreadSafe> Self = WeakSelf.Pin();
								if (!Self.IsValid())
								{
									return false;
								}
								FScopeLock Lock(&Self->Mutex);
								if (FPaths::FileExists(Self->NativePath))
								{
									Self->MediaExportPollTicker.Reset();
									Self->State = ECaptureNativeState::NativeReady;
									Self->ErrorCode.Reset();
									return false;
								}
								if (FPlatformTime::Seconds() >= Deadline)
								{
									Self->MediaExportPollTicker.Reset();
									Self->State = ECaptureNativeState::ExitedFailure;
									Self->ErrorCode =
										TEXT("capture_media_wav_missing_after_stop");
									return false;
								}
								return true;
							}),
						0.05f);
				FScopeLock Lock(&Mutex);
				if (!Handle.IsValid())
				{
					State = ECaptureNativeState::ExitedFailure;
					ErrorCode = TEXT("capture_media_export_poll_schedule_failed");
				}
				else
				{
					MediaExportPollTicker = Handle;
				}
			}

			CaptureMediaLongJob::FUeAgentCaptureMediaLongJobTypedRequest Request;
			TWeakObjectPtr<UWorld> World;
			TWeakObjectPtr<USoundSubmix> Submix;
			FString PublisherOwnerId;
			FCriticalSection Mutex;
			FString OperationId;
			FString NativeResourceId;
			FString NativePath;
			FString FinalPath;
			FString ArtifactKind;
			FString Format;
			FString MimeType;
			FString ErrorCode;
			FTSTicker::FDelegateHandle MediaStopTicker;
			FTSTicker::FDelegateHandle MediaExportPollTicker;
			FUeAgentResourceTracker* Resources = nullptr;
			TOptional<FFileArtifactReference> Artifact;
			ECaptureNativeState State = ECaptureNativeState::Scheduled;
			bool bStarted = false;
			bool bStopRequested = false;
			bool bReleased = false;
		};

		class FCaptureMediaBuilder final
			: public CaptureMediaLongJob::IUeAgentCaptureMediaLongJobPortBuilder
		{
		public:
			FCaptureMediaBuilder(
				TWeakObjectPtr<UWorld> InWorld,
				FString InPublisherOwnerId)
				: World(InWorld), PublisherOwnerId(MoveTemp(InPublisherOwnerId))
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
						Request, World, PublisherOwnerId),
					OutPort,
					OutError);
			}

		private:
			TWeakObjectPtr<UWorld> World;
			FString PublisherOwnerId;
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
			TArray<EUeAgentExecutionWorldType> Worlds = {},
			FString SubjectSchemaJson = FString(),
			FString ParameterSchemaJson = FString(),
			FString ParameterGuidance = FString(),
			FString ResultGuidance = FString(),
			FString MinimumExample = FString())
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

		FString SetCurrentWorldParameterSchema()
		{
			return TEXT(
				"{\"additionalProperties\":false,\"properties\":{"
				"\"save_existing_map\":{\"type\":\"boolean\"}},"
				"\"type\":\"object\"}");
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
				EUeAgentStatePlane::AuthoringMemory),
			EditorPolicy(
				EditorSessionOperation::EUeAgentEditorSessionOperationFamily::RuntimePreview,
				TEXT("policy:runtime-session:pie/v1"),
				TEXT("native_session_resource"),
				TEXT("uai.readback.editor.runtime_preview/v1"),
				EUeAgentStatePlane::RuntimeInstance,
				{EUeAgentExecutionWorldType::Pie}),
			EditorPolicy(
				EditorSessionOperation::EUeAgentEditorSessionOperationFamily::RuntimePreview,
				TEXT("policy:runtime-session:culture-preview/v1"),
				TEXT("native_session_resource"),
				TEXT("uai.readback.editor.culture_preview/v1"),
				EUeAgentStatePlane::RuntimeInstance,
				{EUeAgentExecutionWorldType::Editor}),
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
				{EUeAgentExecutionWorldType::Editor}),
			EditorPolicy(
				EditorSessionOperation::EUeAgentEditorSessionOperationFamily::WorldContext,
				TEXT("policy:world-context:partition-region-loader/v1"),
				TEXT("native_session_resource"),
				TEXT("uai.readback.world_partition.region_loader/v1"),
				EUeAgentStatePlane::AuthoringMemory,
				{EUeAgentExecutionWorldType::Editor}),
			EditorPolicy(
				EditorSessionOperation::EUeAgentEditorSessionOperationFamily::WorldContext,
				TEXT("policy:world-context:partition-actor-loader/v1"),
				TEXT("native_session_resource"),
				TEXT("uai.readback.world_partition.actor_loader/v1"),
				EUeAgentStatePlane::AuthoringMemory,
				{EUeAgentExecutionWorldType::Editor}),
			EditorPolicy(
				EditorSessionOperation::EUeAgentEditorSessionOperationFamily::ModelingTool,
				TEXT("policy:modeling-tool:invoke-action/v1"),
				TEXT("native_session_resource"),
				TEXT("uai.readback.editor.modeling_tool/v1"),
				EUeAgentStatePlane::AuthoringMemory,
				{EUeAgentExecutionWorldType::Editor}),
			EditorPolicy(
				EditorSessionOperation::EUeAgentEditorSessionOperationFamily::VoxelGrid,
				TEXT("policy:voxel-session:apply/v1"),
				TEXT("native_session_resource"),
				TEXT("uai.readback.editor.voxel_grid/v1"),
				EUeAgentStatePlane::AuthoringMemory,
				{EUeAgentExecutionWorldType::Editor}),
			EditorPolicy(
				EditorSessionOperation::EUeAgentEditorSessionOperationFamily::HostLifecycle,
				TEXT("policy:host-lifecycle:prepare-safe-exit/v1"),
				TEXT("host_exit_schedule"),
				TEXT("uai.readback.editor.host_exit/v1"),
				EUeAgentStatePlane::RuntimeInstance),
		};
		return Policies;
	}

	bool FUeAgentLongRunningSessionProductionConfig::IsValid() const
	{
		return !ProjectInstanceId.TrimStartAndEnd().IsEmpty()
			&& !EditorInstanceId.TrimStartAndEnd().IsEmpty()
			&& !ServerSessionId.TrimStartAndEnd().IsEmpty()
			&& !OwnerLeaseSecret.TrimStartAndEnd().IsEmpty()
			&& EditorWorld.IsValid()
			&& TimeoutSeconds > 0.0
			&& MaxFrameBytes > 0
			&& MaxActiveOperations > 0
			&& MaxRetainedOperations >= MaxActiveOperations;
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
			OutError.Code = TEXT("long_running_session_production_config_invalid");
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
			OutError = {FactoryError.GetCode(), FactoryError.GetDetail()};
			return false;
		}

		const TSharedRef<FEditorSessionBuilder, ESPMode::ThreadSafe> EditorBuilder =
			MakeShared<FEditorSessionBuilder, ESPMode::ThreadSafe>(
				Config.EditorInstanceId, Config.EditorWorld);
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
			OutError = {FactoryError.GetCode(), FactoryError.GetDetail()};
			return false;
		}

		const TSharedRef<FCaptureMediaBuilder, ESPMode::ThreadSafe> CaptureBuilder =
			MakeShared<FCaptureMediaBuilder, ESPMode::ThreadSafe>(
				Config.EditorWorld,
				TEXT("long-running-session:") + Config.ServerSessionId);
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
			OutError = {FactoryError.GetCode(), FactoryError.GetDetail()};
			return false;
		}

		FUeAgentLongRunningSessionRuntimeConfig RuntimeConfig;
		RuntimeConfig.ProjectInstanceId = Config.ProjectInstanceId;
		RuntimeConfig.EditorInstanceId = Config.EditorInstanceId;
		RuntimeConfig.ServerSessionId = Config.ServerSessionId;
		RuntimeConfig.OwnerLeaseSecret = Config.OwnerLeaseSecret;
		RuntimeConfig.TimeoutSeconds = Config.TimeoutSeconds;
		RuntimeConfig.MaxFrameBytes = Config.MaxFrameBytes;
		RuntimeConfig.MaxActiveOperations = Config.MaxActiveOperations;
		RuntimeConfig.MaxRetainedOperations = Config.MaxRetainedOperations;
		if (UWorld* World = Config.EditorWorld.Get())
		{
			RuntimeConfig.EditorWorldIdentity = World->GetPackage()
				? TEXT("world:") + World->GetPackage()->GetName() : FString();
			RuntimeConfig.EditorLevelIdentity = World->PersistentLevel
				? TEXT("level:") + World->PersistentLevel->GetPathName() : FString();
		}
		FUeAgentLongRunningSessionRuntimeError RuntimeError;
		if (!FUeAgentLongRunningSessionRuntime::TryCreate(
				RuntimeConfig,
				CompileFactory.ToSharedRef(),
				EditorFactory.ToSharedRef(),
				CaptureFactory.ToSharedRef(),
				OutRuntime,
				RuntimeError)
			|| !OutRuntime.IsValid())
		{
			OutError = {RuntimeError.Code, RuntimeError.Detail};
			return false;
		}
		FString HandlerError;
		if (!TryCreateLongRunningSessionCommandHandlers(
				GetLongRunningSessionActivationProfiles(),
				OutRuntime->GetOperationPort(),
				OutHandlers,
				HandlerError))
		{
			OutRuntime.Reset();
			OutError = {HandlerError, TEXT("Long-running command handlers failed to compose.")};
			return false;
		}
		OutAuthorityScopes = {TEXT("uai:operation"), TEXT("uai:job"),
			TEXT("uai:editor:session"), TEXT("uai:capture")};
		return true;
	}
}

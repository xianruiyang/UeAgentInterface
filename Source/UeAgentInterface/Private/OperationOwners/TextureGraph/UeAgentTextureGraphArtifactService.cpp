#include "OperationOwners/TextureGraph/UeAgentTextureGraphArtifactService.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Core/AssetDurable/UeAgentPackageAssetRevision.h"
#include "Core/BindingGateway/Operation/UeAgentOperationLifecycleService.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "DomainAdapters/AudioMediaGraph/UeAgentAudioMediaGraphTextureGraphNativeHook.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PackageTools.h"
#include "TextureGraph.h"
#include "TextureCompiler.h"
#include "UObject/Package.h"
#include "Utilities/UeAgentPropertyValueCodec.h"

namespace UE::UAI::OperationOwners::TextureGraph
{
	using namespace AssetDurable;
	using namespace BindingGateway::Operation;
	using namespace CommandEdge;
	using namespace ContextResource;
	using namespace DomainCapability;
	using namespace DomainAdapters::AudioMediaGraph;
	using namespace PlanningPreflight;
	using namespace StateSemantic;

	namespace
	{
		const FString BakeId = TEXT("texture_graph.artifact.bake");
		const FString ProbeId = TEXT("texture_graph.runtime.probe");

		Contract::FDiagnostic InputFailure(const TCHAR* Code, const FString& Reason, const FString& Pointer,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const FString& Target = FString())
		{
			auto Diagnostic = Contract::FDiagnostic::Failure(Code, Reason, TEXT("input_decode"));
			Diagnostic.SourceKind = TEXT("command_input");
			Diagnostic.Source.JsonPath = Pointer;
			Diagnostic.PropertyPath = Pointer;
			Diagnostic.ActualValuePresent = Actual.IsValid();
			Diagnostic.ExpectedValuePresent = true;
			Diagnostic.ActualValue = MoveTemp(Actual);
			Diagnostic.ExpectedValue = MoveTemp(Expected);
			Diagnostic.TargetPath = Target;
			return Diagnostic;
		}

		Contract::FDiagnostic NativeFailure(const TCHAR* Code, const FString& Reason, const TCHAR* Field,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const FString& Target,
			const FString& Stage = TEXT("texture_graph"))
		{
			auto Diagnostic = Contract::FDiagnostic::Failure(Code, Reason, Stage);
			Diagnostic.SourceKind = TEXT("native_result");
			Diagnostic.PropertyPath = Field;
			Diagnostic.ActualValue = MoveTemp(Actual);
			Diagnostic.ExpectedValue = MoveTemp(Expected);
			Diagnostic.TargetPath = Target;
			return Diagnostic;
		}

		FUeAgentResourceTracker MakeResources()
		{
			return FUeAgentResourceTracker();
		}

		FString ObjectPath(FString Target)
		{
			Target.TrimStartAndEndInline();
			if (Target.Contains(TEXT(".")))
			{
				return Target;
			}
			const FString Name = FPackageName::GetLongPackageAssetName(Target);
			return Name.IsEmpty() ? Target : Target + TEXT(".") + Name;
		}

		UTexture2D* CreateTextureFromRenderedOutput(
			UPackage& Package,
			const FString& AssetName,
			UTextureRenderTarget2D& RenderTarget,
			Contract::FDiagnostic& OutError)
		{
			OutError.Reset();
			FTextureRenderTargetResource* Resource =
				RenderTarget.GameThread_GetRenderTargetResource();
			if (!Resource)
			{
				OutError = NativeFailure(TEXT("texture_graph_bake_render_resource_missing"),
					TEXT("The rendered output has no game-thread render resource."), TEXT("render_resource_available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Package.GetName(), TEXT("domain_execute"));
				return nullptr;
			}
			const int32 Width = Resource->GetSizeX();
			const int32 Height = Resource->GetSizeY();
			TArray<FColor> Pixels;
			if (Width <= 0 || Height <= 0)
			{
				const auto Dimensions = MakeShared<FJsonObject>();
				Dimensions->SetNumberField(TEXT("width"), Width);
				Dimensions->SetNumberField(TEXT("height"), Height);
				OutError = NativeFailure(TEXT("texture_graph_bake_render_dimensions_invalid"),
					TEXT("The rendered output must have positive width and height."), TEXT("dimensions"),
					MakeShared<FJsonValueObject>(Dimensions), MakeShared<FJsonValueString>(TEXT("positive width and height")), Package.GetName(), TEXT("domain_execute"));
				return nullptr;
			}
			if (!Resource->ReadPixels(Pixels))
			{
				OutError = NativeFailure(TEXT("texture_graph_bake_pixel_read_failed"),
					TEXT("The render resource could not read back pixels for texture creation."), TEXT("pixels_read"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Package.GetName(), TEXT("domain_execute"));
				return nullptr;
			}
			if (Pixels.Num() != Width * Height)
			{
				OutError = NativeFailure(TEXT("texture_graph_bake_pixel_count_mismatch"),
					TEXT("The rendered pixel count does not match the output dimensions."), TEXT("pixel_count"),
					MakeShared<FJsonValueNumber>(Pixels.Num()), MakeShared<FJsonValueNumber>(static_cast<int64>(Width) * Height), Package.GetName(), TEXT("domain_execute"));
				return nullptr;
			}
			UTexture2D* Texture = NewObject<UTexture2D>(
				&Package,
				*AssetName,
				RF_Public | RF_Standalone | RF_Transactional);
			if (!Texture)
			{
				OutError = NativeFailure(TEXT("texture_graph_bake_texture_unavailable"),
					TEXT("The native texture object was not created."), TEXT("texture_created"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Package.GetName(), TEXT("domain_execute"));
				return nullptr;
			}
			Texture->Source.Init(
				Width,
				Height,
				1,
				1,
				TSF_BGRA8,
				reinterpret_cast<const uint8*>(Pixels.GetData()));
			Texture->SRGB = RenderTarget.SRGB;
			Texture->UpdateResource();
			return Texture;
		}

		TSharedPtr<FJsonObject> Color(const FLinearColor& Value)
		{
			const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetNumberField(TEXT("r"), Value.R);
			Result->SetNumberField(TEXT("g"), Value.G);
			Result->SetNumberField(TEXT("b"), Value.B);
			Result->SetNumberField(TEXT("a"), Value.A);
			return Result;
		}

		TSharedPtr<FJsonObject> Stats(
			const FAudioMediaGraphTextureGraphPixelStats& Value)
		{
			const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetNumberField(TEXT("width"), Value.GetWidth());
			Result->SetNumberField(TEXT("height"), Value.GetHeight());
			Result->SetNumberField(
				TEXT("sampled_pixel_count"), Value.GetSampledPixelCount());
			Result->SetNumberField(
				TEXT("non_black_pixel_count"), Value.GetNonBlackPixelCount());
			Result->SetNumberField(
				TEXT("transparent_pixel_count"), Value.GetTransparentPixelCount());
			Result->SetNumberField(
				TEXT("unique_color_estimate"), Value.GetUniqueColorEstimate());
			Result->SetBoolField(
				TEXT("appears_non_black"), Value.GetNonBlackPixelCount() > 0);
			Result->SetObjectField(TEXT("min"), Color(Value.GetMinimum()));
			Result->SetObjectField(TEXT("max"), Color(Value.GetMaximum()));
			Result->SetObjectField(TEXT("mean"), Color(Value.GetMean()));
			return Result;
		}

		FUeAgentCommandResult Failure(
			const FUeAgentCommandRequest& Request,
			const UE::UAI::Contract::FDiagnostic& Diagnostic)
		{
			return FUeAgentCommandResult::Failure(Request, TEXT("{}"), Diagnostic);
		}

		bool Canonical(
			const TSharedPtr<FJsonObject>& Object,
			FString& OutJson,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (CommandContract::Json::TryCanonicalize(Object, OutJson, OutError)) return true;
			OutError.SourceKind = TEXT("native_result");
			return false;
		}

		bool TryReadCurrentRevision(
			UTextureGraph& Graph,
			const FString& TargetRef,
			FString& OutRevision,
			Contract::FDiagnostic& OutError)
		{
			OutRevision.Reset();
			OutError.Reset();
			UPackage* const Package = Graph.GetOutermost();
			if (!Package)
			{
				OutError = NativeFailure(TEXT("texture_graph_bake_revision_conflict"),
					TEXT("The TextureGraph asset has no package for revision reading."), TEXT("package_available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TargetRef, TEXT("revision_read"));
				return false;
			}
			if (Package->IsDirty())
			{
				OutRevision = TEXT("authoring-state-dirty");
				OutError = NativeFailure(TEXT("texture_graph_bake_revision_conflict"),
					TEXT("The TextureGraph package has unsaved changes; save it before baking."), TEXT("package_dirty"),
					MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), TargetRef, TEXT("revision_read"));
				return false;
			}
			return TryReadPackageAssetRevision(TargetRef, OutRevision, OutError);
		}

		struct FBakeTarget
		{
			int32 OutputIndex = INDEX_NONE;
			FString PackageName;
			FString AssetName;
		};

		struct FBakeContext final : public IDomainOperationInvocationContext
		{
			FString RequestId;
			FString Target;
			FString ExpectedRevision;
			FString Fingerprint;
			UTextureGraph* Graph = nullptr;
			TArray<FBakeTarget> Targets;
			TArray<FAudioMediaGraphTextureGraphRenderedOutput> Rendered;
			TArray<UTexture2D*> Created;
			TArray<UPackage*> CreatedPackages;
			TArray<FString> ReloadedObjectPaths;
			FUeAgentResourceTracker Resources = MakeResources();
			FPackageAssetDurableBackend DurableBackend;
			FAssetDurableSessionPtr DurableSession;
			FDurableReceipt DurableReceipt;
			int64 FencingToken = 0;
			UE::UAI::Contract::FDiagnostic LastLifecycleDiagnostic;



			bool TryBuildSuccessPayload(
				const FOperationLifecycleReceipt& /*Receipt*/,
				FString& OutPayloadJson,
				UE::UAI::Contract::FDiagnostic& OutError) const override
			{
				OutPayloadJson.Reset();
				OutError.Reset();
				if (ReloadedObjectPaths.Num() != Targets.Num())
				{
					OutError = NativeFailure(TEXT("texture_graph_bake_result_readback_missing"),
						TEXT("The reloaded texture count does not match the requested bake targets."), TEXT("reloaded_texture_count"),
						MakeShared<FJsonValueNumber>(ReloadedObjectPaths.Num()), MakeShared<FJsonValueNumber>(Targets.Num()), Target, TEXT("result_projection"));
					return false;
				}
				const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
				Result->SetStringField(TEXT("command_id"), BakeId);
				Result->SetStringField(TEXT("terminal_status"), TEXT("succeeded"));
				Result->SetStringField(TEXT("target"), Target);
				TArray<TSharedPtr<FJsonValue>> Outputs;
				TArray<TSharedPtr<FJsonValue>> AssetRefs;
				for (int32 Index = 0; Index < Targets.Num(); ++Index)
				{
					const FBakeTarget& OutputTarget = Targets[Index];
					if (!Rendered.IsValidIndex(OutputTarget.OutputIndex))
					{
						OutError = NativeFailure(TEXT("texture_graph_bake_result_output_missing"),
							TEXT("The bake result refers to an unavailable rendered output index."), TEXT("render_output_index"),
							MakeShared<FJsonValueNumber>(OutputTarget.OutputIndex), MakeShared<FJsonValueString>(FString::Printf(TEXT("0 <= index < %d"), Rendered.Num())),
							Target, TEXT("result_projection"));
						return false;
					}
					const TSharedPtr<FJsonObject> Output = MakeShared<FJsonObject>();
					Output->SetNumberField(
						TEXT("render_output_index"), OutputTarget.OutputIndex);
					Output->SetStringField(
						TEXT("asset_path"), ReloadedObjectPaths[Index]);
					Output->SetObjectField(
						TEXT("pixel_stats"),
						Stats(Rendered[OutputTarget.OutputIndex].GetStats()));
					Outputs.Add(MakeShared<FJsonValueObject>(Output));
					AssetRefs.Add(MakeShared<FJsonValueString>(
						ReloadedObjectPaths[Index]));
				}
				Result->SetArrayField(TEXT("outputs"), MoveTemp(Outputs));
				Result->SetArrayField(TEXT("asset_refs"), MoveTemp(AssetRefs));
				if (!Canonical(Result, OutPayloadJson, OutError))
				{
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Target;
					return false;
				}
				return true;
			}
		};

		bool ParseBake(
			const FUeAgentCommandRequest& Request,
			TSharedRef<FBakeContext>& Context,
			Contract::FDiagnostic& OutError)
		{
			OutError.Reset();
			auto Reject = [&OutError](Contract::FDiagnostic Diagnostic)
			{
				OutError = MoveTemp(Diagnostic);
				return false;
			};
			TSharedPtr<FJsonObject> Params;
			Contract::FDiagnostic Error;
			if (!CommandContract::Json::TryParseObject(Request.GetParamsJson(), Params, Error))
			{
				if (Error.IsEmpty()) Error = InputFailure(TEXT("texture_graph_bake_input_invalid"),
					TEXT("TextureGraph bake parameters could not be decoded as an object."), FString(), nullptr, MakeShared<FJsonValueString>(TEXT("object")));
				Error.SourceKind = TEXT("command_input");
				return Reject(Contract::FDiagnostic::WrapFailure(TEXT("texture_graph_bake_input_invalid"), Error));
			}
			if (!Params.IsValid())
				return Reject(InputFailure(TEXT("texture_graph_bake_input_invalid"),
					TEXT("TextureGraph bake parameters must be an object."), FString(), nullptr, MakeShared<FJsonValueString>(TEXT("object"))));
			if (!Params->TryGetStringField(TEXT("target"), Context->Target))
				return Reject(InputFailure(TEXT("texture_graph_bake_input_invalid"),
					TEXT("The target parameter must be a TextureGraph asset path string."), TEXT("/target"),
					Params->TryGetField(TEXT("target")), MakeShared<FJsonValueString>(TEXT("TextureGraph asset path string"))));
			const TArray<TSharedPtr<FJsonValue>>* Outputs = nullptr;
			if (!Params->TryGetArrayField(TEXT("outputs"), Outputs) || !Outputs || Outputs->IsEmpty())
				return Reject(InputFailure(TEXT("texture_graph_bake_outputs_required"),
					TEXT("TextureGraph bake requires a non-empty outputs array."), TEXT("/outputs"),
					Params->TryGetField(TEXT("outputs")), MakeShared<FJsonValueString>(TEXT("non-empty array")), Context->Target));
			TSet<FString> Packages;
			TSet<int32> OutputIndexes;
			for (int32 OutputOrdinal = 0; OutputOrdinal < Outputs->Num(); ++OutputOrdinal)
			{
				const auto& Value = (*Outputs)[OutputOrdinal];
				const FString OutputPointer = FString::Printf(TEXT("/outputs/%d"), OutputOrdinal);
				const TSharedPtr<FJsonObject> Output = Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
				if (!Output.IsValid())
					return Reject(InputFailure(TEXT("texture_graph_bake_output_invalid"),
						TEXT("Each bake output must be an object."), OutputPointer, Value, MakeShared<FJsonValueString>(TEXT("object")), Context->Target));
				int64 IndexNumber = -1;
				Contract::FDiagnostic NumberError;
				if (!UeAgentPropertyValueCodec::TryReadIntegerNumber(
					Output->TryGetField(TEXT("render_output_index")), 0, MAX_int32, IndexNumber, NumberError))
				{
					const FString Pointer = OutputPointer + TEXT("/render_output_index");
					if (NumberError.IsEmpty()) NumberError = InputFailure(TEXT("texture_graph_bake_output_invalid"),
						TEXT("render_output_index must be an integer between zero and MAX_int32."), Pointer,
						Output->TryGetField(TEXT("render_output_index")), MakeShared<FJsonValueString>(TEXT("integer in [0, 2147483647]")), Context->Target);
					NumberError.SourceKind = TEXT("command_input");
					NumberError.Source.JsonPath = Pointer;
					NumberError.PropertyPath = Pointer;
					NumberError.TargetPath = Context->Target;
					return Reject(Contract::FDiagnostic::WrapFailure(TEXT("texture_graph_bake_output_invalid"), NumberError));
				}
				FBakeTarget Target;
				if (!Output->TryGetStringField(TEXT("destination"), Target.PackageName)
					|| !FPackageName::IsValidLongPackageName(Target.PackageName))
					return Reject(InputFailure(TEXT("texture_graph_bake_output_invalid"),
						TEXT("The bake destination must be a valid long package name."), OutputPointer + TEXT("/destination"),
						Output->TryGetField(TEXT("destination")), MakeShared<FJsonValueString>(TEXT("valid long package name")), Context->Target));
				Target.OutputIndex = static_cast<int32>(IndexNumber);
				Target.AssetName = FPackageName::GetLongPackageAssetName(Target.PackageName);
				FString Filename;
				FPackageName::TryConvertLongPackageNameToFilename(Target.PackageName, Filename, FPackageName::GetAssetPackageExtension());
				if (Target.AssetName.IsEmpty() || Packages.Contains(Target.PackageName)
					|| OutputIndexes.Contains(Target.OutputIndex) || FindPackage(nullptr, *Target.PackageName)
					|| IFileManager::Get().FileExists(*Filename))
				{
					const bool bDuplicateIndex = !Target.AssetName.IsEmpty() && !Packages.Contains(Target.PackageName)
						&& OutputIndexes.Contains(Target.OutputIndex);
					const FString Reason = Target.AssetName.IsEmpty() ? TEXT("The bake destination does not contain an asset name.")
						: Packages.Contains(Target.PackageName) ? TEXT("Two bake outputs use the same destination package.")
						: bDuplicateIndex ? TEXT("Two bake outputs use the same render_output_index.")
						: FindPackage(nullptr, *Target.PackageName) ? TEXT("The bake destination package is already loaded.")
						: TEXT("The bake destination package already exists on disk.");
					return Reject(InputFailure(TEXT("texture_graph_bake_destination_conflict"), Reason,
						OutputPointer + (bDuplicateIndex ? TEXT("/render_output_index") : TEXT("/destination")),
						Output->TryGetField(bDuplicateIndex ? TEXT("render_output_index") : TEXT("destination")),
						MakeShared<FJsonValueString>(bDuplicateIndex ? TEXT("unique output index") : TEXT("unique unused destination package")), Context->Target));
				}
				Packages.Add(Target.PackageName);
				OutputIndexes.Add(Target.OutputIndex);
				Context->Targets.Add(MoveTemp(Target));
			}
			Context->RequestId = Request.GetRequestId();
			Context->Fingerprint = CommandContract::Json::Sha256Utf8(Request.GetParamsJson());
			Context->Graph = LoadObject<UTextureGraph>(nullptr, *ObjectPath(Context->Target));
			if (!Context->Graph)
				return Reject(InputFailure(TEXT("texture_graph_not_found"),
					TEXT("The target TextureGraph asset could not be loaded."), TEXT("/target"),
					Params->TryGetField(TEXT("target")), MakeShared<FJsonValueString>(TEXT("loadable TextureGraph asset")), Context->Target));
			Contract::FDiagnostic RevisionError;
			if (!TryReadCurrentRevision(*Context->Graph, Context->Target, Context->ExpectedRevision, RevisionError))
				return Reject(Contract::FDiagnostic::WrapFailure(TEXT("texture_graph_bake_revision_conflict"), RevisionError));
			return true;
		}

		FPlanningPreflightPlanRef BakeDurablePlan(
			const FBakeContext& Context,
			TArray<FUeAgentCanonicalIdentity>& OutIdentities)
		{
			TArray<FPlannedChange> Changes;
			for (const FBakeTarget& Target : Context.Targets)
			{
				FUeAgentCanonicalIdentity Identity(
					TEXT("uai.semantic.texture_graph.baked_output"),
					TEXT("ue.texture2d.asset"),
					Target.PackageName,
					{{TEXT("package"), Target.PackageName}},
					TEXT("asset"),
					TEXT("1"));
				OutIdentities.Add(Identity);
				Changes.Emplace(
					Identity,
					EChangeIntent::Create,
					FUeAgentCanonicalValuePtr(),
					FUeAgentCanonicalValuePtr(),
					TArray<FString>{TEXT("uai:operate")},
					TEXT("texture-graph-bake-preflight"));
			}
			return MakeShared<const FPlanningPreflightPlan>(
				Contract::FUeAgentDescriptorIdentity(
					TEXT("uai.operation.texture_graph.artifact_bake"),
					1,
					Context.Fingerprint),
				Context.RequestId,
				Context.RequestId,
				TEXT("texture-graph-operation-owner"),
				TArray<FString>{TEXT("uai:operate")},
				TEXT("texture-graph-bake:") + Context.Target,
				FUeAgentStateRevision(
					TEXT("texture_graph"), Context.ExpectedRevision, 1),
				TEXT("texture-graph-content-readback"),
				FChangeSet(MoveTemp(Changes)),
				FImpactSet(),
				TArray<FPrecondition>{},
				Context.Fingerprint);
		}

		class FBakeVersionFence final : public IOperationVersionFencePort
		{
		public:
			explicit FBakeVersionFence(TSharedRef<FBakeContext> In)
				: Context(MoveTemp(In)) {}
			bool Check(
				const FOperationLifecyclePlan&,
				EOperationLifecycleStage Stage,
				bool& bOutCurrent,
				FOperationLifecycleError& OutError) override
			{
				FString Revision;
				Contract::FDiagnostic RevisionError;
				const bool bRevisionRead = Context->Graph
					&& TryReadCurrentRevision(
						*Context->Graph, Context->Target, Revision, RevisionError);
				bOutCurrent = bRevisionRead && Revision == Context->ExpectedRevision;
				FString ExistingDestination;
				if (bOutCurrent
					&& Stage == EOperationLifecycleStage::PrePersistFence)
				{
					for (const FBakeTarget& Target : Context->Targets)
					{
						FString Filename;
						FPackageName::TryConvertLongPackageNameToFilename(
							Target.PackageName,
							Filename,
							FPackageName::GetAssetPackageExtension());
						if (IFileManager::Get().FileExists(*Filename))
						{
							bOutCurrent = false;
							ExistingDestination = Target.PackageName;
							break;
						}
					}
				}
				if (!bOutCurrent)
				{
					if (!RevisionError.IsEmpty())
					{
						OutError = Contract::FDiagnostic::WrapFailure(TEXT("texture_graph_bake_revision_conflict"), RevisionError);
						Context->LastLifecycleDiagnostic = OutError;
						return true;
					}
					OutError = FOperationLifecycleError::Failure(TEXT("texture_graph_bake_revision_conflict"),
						!Context->Graph ? TEXT("The TextureGraph asset is unavailable for the version check.")
							: !bRevisionRead ? TEXT("The current TextureGraph revision could not be read.")
							: !ExistingDestination.IsEmpty() ? TEXT("A destination package appeared before persistence.") : TEXT("The TextureGraph revision changed after bake preparation."), LexToString(Stage));
					OutError.SourceKind = TEXT("native_result");
					OutError.TargetPath = ExistingDestination.IsEmpty() ? Context->Target : ExistingDestination;
					OutError.PropertyPath = ExistingDestination.IsEmpty() ? TEXT("revision") : TEXT("destination");
					if (!ExistingDestination.IsEmpty())
					{
						OutError.ActualValue = MakeShared<FJsonValueBoolean>(true);
						OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(false);
					}
					else
					{
						OutError.ActualEvidence = MakeShared<FJsonValueString>(Revision);
						OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Context->ExpectedRevision);
						OutError.PropertyPath = Context->Graph ? TEXT("revision_matches") : TEXT("graph_available");
						OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
						OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					}
                    Context->LastLifecycleDiagnostic = OutError;
				}
				return true;
			}
		private:
			TSharedRef<FBakeContext> Context;
		};

		class FBakeMutation final : public IOperationDomainMutationPort
		{
		public:
			explicit FBakeMutation(TSharedRef<FBakeContext> In)
				: Context(MoveTemp(In)) {}
			bool Execute(
				const FOperationLifecyclePlan&,
				FOperationMutationFacts& OutFacts,
				FOperationLifecycleError& OutError) override
			{
				UWorld* World = GEditor
					? GEditor->GetEditorWorldContext().World() : nullptr;
				FAudioMediaGraphTextureGraphNativeHook Hook;
				FAudioMediaGraphError NativeError;
				if (!World || !Context->Graph
					|| !Hook.TryRenderAndReadOutputs(
						*World, *Context->Graph, Context->Rendered, NativeError))
				{
					if (NativeError.IsValid()) OutError = NativeError;
					else
					{
						OutError = FOperationLifecycleError::Failure(TEXT("texture_graph_render_failed"),
							!World ? TEXT("An editor world is required to render the TextureGraph.")
								: !Context->Graph ? TEXT("The TextureGraph asset is unavailable for rendering.") : TEXT("Native TextureGraph rendering failed without providing a diagnostic."), TEXT("domain_execute"));
						OutError.SourceKind = TEXT("native_result");
						OutError.TargetPath = Context->Target;
						OutError.PropertyPath = !World ? TEXT("world") : !Context->Graph ? TEXT("graph") : TEXT("render_outputs");
					}
					return false;
				}
				for (const FBakeTarget& Target : Context->Targets)
				{
					if (!Context->Rendered.IsValidIndex(Target.OutputIndex))
					{
						OutError = FOperationLifecycleError::Failure(TEXT("texture_graph_bake_output_index_invalid"), TEXT("The requested output index is outside the rendered outputs."), TEXT("domain_execute"));
						OutError.SourceKind = TEXT("native_result");
						OutError.TargetPath = Target.PackageName;
						OutError.PropertyPath = TEXT("output_index");
						OutError.ActualValue = MakeShared<FJsonValueNumber>(Target.OutputIndex);
						OutError.ExpectedValue = MakeShared<FJsonValueString>(FString::Printf(TEXT("0 <= index < %d"), Context->Rendered.Num()));
						return false;
					}
				}
				TArray<FUeAgentCanonicalIdentity> Identities;
				const FPlanningPreflightPlanRef DurablePlan =
					BakeDurablePlan(*Context, Identities);
				TArray<FAssetDurableTarget> DurableTargets;
				for (int32 Index = 0; Index < Context->Targets.Num(); ++Index)
				{
					DurableTargets.Emplace(
						Identities[Index],
						Context->Targets[Index].PackageName,
						EPackageHeaderKind::Asset,
						EAssetDurableIntent::Save,
						ERollbackGrade::DurableBackup);
				}
				static int64 NextFencingToken = 1000;
				Context->FencingToken = ++NextFencingToken;
				FAssetDurableError DurableError;
				if (!TryPrepareAssetDurability(
					FAssetDurableRequest(
						DurablePlan,
						MoveTemp(DurableTargets),
						Context->FencingToken,
						TEXT("texture-graph-bake:") + Context->RequestId,
						EAtomicityMode::CompensatedSaga),
					Context->DurableBackend,
					FPaths::ProjectSavedDir()
						/ TEXT("UeAgentInterface/TextureGraph/BakeDurable"),
					Context->Resources,
					Context->DurableSession,
					DurableError)
					|| !Context->DurableSession.IsValid())
				{
					OutError = DurableError;
					return false;
				}
				TArray<FString> Mutated;
				for (const FBakeTarget& Target : Context->Targets)
				{
					UTextureRenderTarget2D* Source =
						Context->Rendered[Target.OutputIndex].GetRenderTarget();
					UPackage* Package = CreatePackage(*Target.PackageName);
					if (Package)
					{
						Context->CreatedPackages.AddUnique(Package);
					}
					Contract::FDiagnostic TextureError;
					UTexture2D* Texture = Source && Package
						? CreateTextureFromRenderedOutput(
							*Package, Target.AssetName, *Source, TextureError)
						: nullptr;
					if (!Texture)
					{
						auto CreationError = !TextureError.IsEmpty() ? Contract::FDiagnostic::WrapFailure(TEXT("texture_graph_bake_texture_create_failed"), TextureError)
							: FOperationLifecycleError::Failure(TEXT("texture_graph_bake_texture_create_failed"),
								!Source ? TEXT("The rendered output has no render target for creating the texture.")
									: !Package ? TEXT("The output package could not be created.") : TEXT("The native texture factory did not create a texture from the rendered output."), TEXT("domain_execute"));
						if (TextureError.IsEmpty())
						{
							CreationError.SourceKind = TEXT("native_result");
							CreationError.TargetPath = Target.PackageName;
							CreationError.PropertyPath = !Source ? TEXT("render_target") : !Package ? TEXT("package") : TEXT("texture");
							CreationError.ActualValuePresent = false;
							CreationError.ExpectedValue = MakeShared<FJsonValueString>(!Source ? TEXT("render target") : !Package ? TEXT("output package") : TEXT("Texture2D"));
						}
						if (!Mutated.IsEmpty())
						{
							OutFacts = FOperationMutationFacts(
								TEXT("texture-graph-bake:")
									+ Context->RequestId,
								Mutated,
								true);
						}
						else
						{
							FText UnloadError;
							const bool bPackagesUnloaded = UnloadCreatedPackages(
								UnloadError);
							FAssetDurableError CleanupError;
							FDurableReceipt CleanupReceipt;
							const bool bDurableRolledBack = TryRollbackAssetDurability(
								Context->DurableSession.ToSharedRef(),
								Context->DurableBackend,
								Context->FencingToken,
								bPackagesUnloaded,
								Context->Resources,
								CleanupReceipt,
								CleanupError);
							if (!bDurableRolledBack || !bPackagesUnloaded)
							{
								OutFacts = FOperationMutationFacts(TEXT("texture-graph-bake-cleanup:") + Context->RequestId,
									{Target.PackageName}, false);
								OutError = Contract::FDiagnostic::WrapFailure(TEXT("texture_graph_bake_rollback_failed"), CreationError);
								OutError.Stage = TEXT("rollback");
								OutError.PublicMessage = TEXT("Cleanup after texture creation failed.");
								OutError.SourceKind = TEXT("native_result");
								OutError.TargetPath = Target.PackageName;
								if (!bPackagesUnloaded)
								{
									auto Cause = NativeFailure(TEXT("texture_graph_bake_native_unload_failed"),
										UnloadError.IsEmpty() ? FString(TEXT("The created packages could not be unloaded during texture creation cleanup.")) : UnloadError.ToString(),
										TEXT("packages_unloaded"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Target.PackageName, TEXT("rollback"));
									OutError.Append(Cause);
								}
								if (!bDurableRolledBack && CleanupError.IsEmpty()) CleanupError = NativeFailure(TEXT("texture_graph_bake_durable_cleanup_failed"),
									TEXT("Durable cleanup failed without providing a diagnostic."), TEXT("durable_rollback_succeeded"),
									MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Target.PackageName, TEXT("rollback"));
								OutError.Append(CleanupError);
								OutError.RollbackAttempted = true;
								OutError.RollbackSucceeded = false;
								return false;
							}
						}
						OutError = MoveTemp(CreationError);
						return false;
					}
					FAssetRegistryModule::AssetCreated(Texture);
					Texture->PostEditChange();
					TArray<UTexture*> TexturesToCompile{Texture};
					FTextureCompilingManager::Get().FinishCompilation(
						TexturesToCompile);
					Texture->MarkPackageDirty();
					Context->Created.Add(Texture);
					Mutated.Add(Target.PackageName);
				}
				OutFacts = FOperationMutationFacts(
					TEXT("texture-graph-bake:") + Context->RequestId,
					MoveTemp(Mutated),
					true);
				return OutFacts.TryValidate(OutError);
			}

			bool Rollback(
				const FOperationLifecyclePlan&,
				const FOperationMutationFacts&,
				bool& bOutConfirmed,
				FOperationLifecycleError& OutError) override
			{
				bOutConfirmed = false;
				if (Context->DurableReceipt.GetStatus()
					== EDurableStatus::StateUnknown)
				{
					OutError = FOperationLifecycleError::Failure(TEXT("texture_graph_bake_durable_state_unknown"),
						TEXT("The durable state of the baked textures could not be determined."),
						TEXT("rollback"), Context->DurableReceipt.GetJournalId());
					OutError.Append(Context->LastLifecycleDiagnostic);
					OutError.TerminalStatus = TEXT("state_unknown");
					OutError.RollbackAttempted = true;
					OutError.RollbackSucceeded = false;
					return false;
				}
				const bool bDurabilityAlreadyRolledBack =
					Context->DurableReceipt.GetStatus()
						== EDurableStatus::RolledBack;
				if (!Context->DurableSession.IsValid())
				{
					bOutConfirmed = Context->Created.IsEmpty();
					return true;
				}
				for (UTexture2D* Texture : Context->Created)
				{
					if (!Texture) continue;
					FAssetRegistryModule::AssetDeleted(Texture);
					Texture->ClearFlags(RF_Public | RF_Standalone);
					Texture->Rename(
						nullptr,
						GetTransientPackage(),
						REN_DontCreateRedirectors | REN_NonTransactional);
					Texture->MarkAsGarbage();
				}
				Context->Created.Reset();
				FText UnloadError;
				const bool bPackagesUnloaded = UnloadCreatedPackages(UnloadError);
				FAssetDurableError DurableError;
				FDurableReceipt Receipt = Context->DurableReceipt;
				const bool bDurableRolledBack = bDurabilityAlreadyRolledBack
					|| TryRollbackAssetDurability(
						Context->DurableSession.ToSharedRef(),
						Context->DurableBackend,
						Context->FencingToken,
						bPackagesUnloaded,
						Context->Resources,
						Receipt,
						DurableError);
				if (Receipt.GetStatus() == EDurableStatus::StateUnknown)
				{
					OutError = FOperationLifecycleError::Failure(TEXT("texture_graph_bake_durable_state_unknown"),
						TEXT("The durable state of the baked textures could not be determined."),
						TEXT("rollback"), Receipt.GetJournalId());
					OutError.Append(Context->LastLifecycleDiagnostic);
					OutError.TerminalStatus = TEXT("state_unknown");
					OutError.RollbackAttempted = true;
					OutError.RollbackSucceeded = false;
					return false;
				}
				if (!bPackagesUnloaded || !bDurableRolledBack)
				{
					OutError = FOperationLifecycleError::Failure(
						bPackagesUnloaded ? TEXT("texture_graph_bake_rollback_failed") : TEXT("texture_graph_bake_rollback_unload_failed"),
						bPackagesUnloaded ? TEXT("Durable rollback of the baked textures failed.") : TEXT("Baked texture packages could not be unloaded during rollback."),
						TEXT("rollback"));
					OutError.SourceKind = TEXT("native_result");
					if (!bPackagesUnloaded && !UnloadError.IsEmpty())
					{
						auto Cause = FOperationLifecycleError::Failure(TEXT("texture_graph_bake_native_unload_failed"), UnloadError.ToString(), TEXT("rollback"));
						Cause.SourceKind = TEXT("native_result");
						OutError.Append(Cause);
					}
					OutError.Append(DurableError);
					OutError.RollbackAttempted = true;
					OutError.RollbackSucceeded = false;
					return false;
				}
				bOutConfirmed = Receipt.GetStatus() == EDurableStatus::RolledBack;
				return true;
			}
		private:
			bool UnloadCreatedPackages(FText& OutError)
			{
				TArray<UPackage*> Packages;
				for (UPackage* Package : Context->CreatedPackages)
				{
					if (Package)
					{
						Package->SetDirtyFlag(false);
						Packages.AddUnique(Package);
					}
				}
				Context->CreatedPackages.Reset();
				if (Packages.IsEmpty())
				{
					return true;
				}
				const bool bUnloaded = UPackageTools::UnloadPackages(
					Packages, OutError, true);
				CollectGarbage(RF_NoFlags, true);
				return bUnloaded;
			}

			TSharedRef<FBakeContext> Context;
		};

		class FBakeStability final : public IOperationStabilityPort
		{
		public:
			explicit FBakeStability(TSharedRef<FBakeContext> In)
				: Context(MoveTemp(In)) {}
			bool Settle(
				const FOperationLifecyclePlan&,
				const FOperationMutationFacts&,
				FOperationLifecycleError& OutError) override
			{
				const bool bValid = Context->Created.Num() == Context->Targets.Num()
					&& Context->Rendered.Num() >= Context->Targets.Num();
				if (!bValid)
				{
					OutError = FOperationLifecycleError::Failure(TEXT("texture_graph_bake_settle_incomplete"),
						TEXT("The created and rendered texture counts do not match the requested outputs."),
						TEXT("settle"));
					OutError.SourceKind = TEXT("native_result");
					OutError.PropertyPath = TEXT("output_counts");
					auto Actual = MakeShared<FJsonObject>();
					Actual->SetNumberField(TEXT("created"), Context->Created.Num());
					Actual->SetNumberField(TEXT("rendered"), Context->Rendered.Num());
					auto Expected = MakeShared<FJsonObject>();
					Expected->SetNumberField(TEXT("created"), Context->Targets.Num());
					Expected->SetNumberField(TEXT("minimum_rendered"), Context->Targets.Num());
					OutError.ActualValue = MakeShared<FJsonValueObject>(Actual);
					OutError.ExpectedValue = MakeShared<FJsonValueObject>(Expected);
					Context->LastLifecycleDiagnostic = OutError;
				}
				return bValid;
			}
		private:
			TSharedRef<FBakeContext> Context;
		};

		class FBakePersistence final : public IOperationPersistencePort
		{
		public:
			explicit FBakePersistence(TSharedRef<FBakeContext> In)
				: Context(MoveTemp(In)) {}
			bool Persist(
				const FOperationLifecyclePlan&,
				const FOperationMutationFacts&,
				FString& OutReceipt,
				FOperationLifecycleError& OutError) override
			{
				FAssetDurableError DurableError;
				if (!Context->DurableSession.IsValid())
				{
					OutError = FOperationLifecycleError::Failure(TEXT("texture_graph_bake_durable_session_missing"),
						TEXT("The baked textures cannot be persisted because the prepared durability session is missing."), TEXT("persist"));
					OutError.SourceKind = TEXT("native_result");
					OutError.PropertyPath = TEXT("durability_session");
					OutError.ActualValuePresent = false;
					OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("prepared durability session"));
					return false;
				}
				const FAssetDurableSessionRef PreparedSession =
					Context->DurableSession.ToSharedRef();
				if (!TryPersistAssetDurabilityForVerification(
						PreparedSession,
						Context->DurableBackend,
						Context->FencingToken,
						Context->Resources,
						Context->DurableReceipt,
						DurableError))
				{
					Context->LastLifecycleDiagnostic = DurableError;
					OutError = DurableError;
					return false;
				}
				Context->DurableSession = MakeShared<const FAssetDurableSession>(
					PreparedSession->GetRequest(),
					PreparedSession->GetInventory(),
					PreparedSession->GetJournalId(),
					PreparedSession->GetJournalPath(),
					PreparedSession->GetJournalRoot(),
					PreparedSession->GetTransactionRoot(),
					PreparedSession->GetStagingRoot(),
					Context->DurableReceipt.GetPhaseHistory());
				OutReceipt = Context->DurableReceipt.GetJournalId();
				if (OutReceipt.IsEmpty())
				{
					OutError = FOperationLifecycleError::Failure(TEXT("texture_graph_bake_persistence_receipt_missing"),
						TEXT("Durable persistence returned no journal receipt identifier."), TEXT("persist"));
					OutError.SourceKind = TEXT("native_result");
					OutError.PropertyPath = TEXT("persistence_receipt");
					OutError.ActualValuePresent = false;
					OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("nonempty persistence receipt"));
					return false;
				}
				return true;
			}
		private:
			TSharedRef<FBakeContext> Context;
		};

		class FBakeVerification final
			: public IOperationIndependentVerificationPort
		{
		public:
			explicit FBakeVerification(TSharedRef<FBakeContext> In)
				: Context(MoveTemp(In)) {}
			bool Verify(
				const FOperationLifecyclePlan&,
				const FOperationMutationFacts&,
				const FString& PersistenceReceiptId,
				bool& bOutEquivalent,
				FOperationLifecycleError& OutError) override
			{
				bOutEquivalent = false;
				TArray<UPackage*> Packages;
				for (UTexture2D* Texture : Context->Created)
				{
					if (Texture && Texture->GetOutermost())
					{
						Packages.AddUnique(Texture->GetOutermost());
					}
				}
				FText UnloadError;
				if (!UPackageTools::UnloadPackages(Packages, UnloadError, true))
				{
					OutError = FOperationLifecycleError::Failure(TEXT("texture_graph_bake_unload_failed"),
						UnloadError.IsEmpty() ? TEXT("Created texture packages could not be unloaded for independent verification.") : UnloadError.ToString(),
						TEXT("verification"));
					OutError.SourceKind = TEXT("native_result");
					Context->LastLifecycleDiagnostic = OutError;
					return false;
				}
				Context->Created.Reset();
				Context->CreatedPackages.Reset();
				CollectGarbage(RF_NoFlags, true);
				for (int32 Index = 0; Index < Context->Targets.Num(); ++Index)
				{
					const FBakeTarget& Target = Context->Targets[Index];
					UTexture2D* Reloaded = LoadObject<UTexture2D>(
						nullptr,
						*(Target.PackageName + TEXT(".") + Target.AssetName));
					if (Reloaded)
					{
						TArray<UTexture*> TexturesToCompile{Reloaded};
						FTextureCompilingManager::Get().FinishCompilation(
							TexturesToCompile);
					}
					const FAudioMediaGraphTextureGraphPixelStats& Expected =
						Context->Rendered[Target.OutputIndex].GetStats();
					if (!Reloaded
						|| Reloaded->GetSizeX() != Expected.GetWidth()
						|| Reloaded->GetSizeY() != Expected.GetHeight())
					{
						OutError = FOperationLifecycleError::Failure(TEXT("texture_graph_bake_readback_mismatch"),
							TEXT("The saved texture is missing or its dimensions differ from the rendered output."),
							TEXT("verification"));
						OutError.SourceKind = TEXT("native_result");
						OutError.TargetPath = Target.PackageName;
						OutError.PropertyPath = TEXT("dimensions");
						const TSharedRef<FJsonObject> ExpectedSize = MakeShared<FJsonObject>();
						ExpectedSize->SetNumberField(TEXT("width"), Expected.GetWidth());
						ExpectedSize->SetNumberField(TEXT("height"), Expected.GetHeight());
						OutError.ExpectedValue = MakeShared<FJsonValueObject>(ExpectedSize);
						if (Reloaded)
						{
							const TSharedRef<FJsonObject> ActualSize = MakeShared<FJsonObject>();
							ActualSize->SetNumberField(TEXT("width"), Reloaded->GetSizeX());
							ActualSize->SetNumberField(TEXT("height"), Reloaded->GetSizeY());
							OutError.ActualValue = MakeShared<FJsonValueObject>(ActualSize);
						}
						else OutError.ActualValue = MakeShared<FJsonValueNull>();
						OutError.VerificationStatus = TEXT("mismatch");
						Context->LastLifecycleDiagnostic = OutError;
						return false;
					}
					Context->ReloadedObjectPaths.Add(Reloaded->GetPathName());
				}
				FAssetDurableError DurableError;
				if (!Context->DurableSession.IsValid()
					|| !TryAcknowledgeAssetDurability(
						Context->DurableSession.ToSharedRef(),
						FDurableVerificationAcknowledgement(
							PersistenceReceiptId,
							Context->Fingerprint,
							Context->DurableSession->GetInventory().GetInventoryHash(),
							TEXT("texture-graph-independent-readback:")
								+ Context->RequestId,
							TEXT("texture-graph-bake:") + Context->RequestId,
							Context->FencingToken),
						Context->FencingToken,
						Context->Resources,
						DurableError))
				{
					Context->LastLifecycleDiagnostic = DurableError;
					OutError = DurableError;
					return false;
				}
				bOutEquivalent = Context->ReloadedObjectPaths.Num()
					== Context->Targets.Num();
				return true;
			}
		private:
			TSharedRef<FBakeContext> Context;
		};

		class FTextureGraphBakeInvocationFactory final
			: public IDomainOperationInvocationFactory
		{
		public:
			const FString& GetOperationId() const override
			{
				return BakeId;
			}

			bool TryCreate(
				const FUeAgentCommandRequest& Request,
				const Contract::FUeAgentCallerContext&,
				const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation,
				FDomainOperationInvocation& OutInvocation,
				FDomainOperationInvocationError& OutError) const override
			{
				OutInvocation = {};
				OutError = {};
				if (Cancellation->IsCancelled())
				{
					OutError = NativeFailure(TEXT("texture_graph_bake_cancelled"),
						TEXT("The TextureGraph bake was cancelled before preparation."), TEXT("cancellation_requested"),
						MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), FString(), TEXT("prepare"));
					OutError.SubjectId = Request.GetRequestId();
					return false;
				}
				TSharedRef<FBakeContext> Context = MakeShared<FBakeContext>();
				if (!ParseBake(Request, Context, OutError))
				{
					return false;
				}
				TArray<FString> ImpactScope = {Context->Target};
				TMap<FString, FString> ExpectedRevisions = {
					{Context->Target, Context->ExpectedRevision},
				};
				for (const FBakeTarget& Target : Context->Targets)
				{
					ImpactScope.Add(Target.PackageName);
					ExpectedRevisions.Add(Target.PackageName, TEXT("absent"));
				}
				const TSharedRef<const FOperationLifecyclePlan> Plan =
					MakeShared<FOperationLifecyclePlan>(
						TEXT("texture-graph-bake:") + Request.GetRequestId(),
						Context->Fingerprint,
						ImpactScope,
						MoveTemp(ExpectedRevisions),
						TArray<FString>{
							TEXT("render_readback"),
							TEXT("texture2d_created")});
				const TSharedRef<const FOperationImpactAuthorization> Authorization =
					MakeShared<FOperationImpactAuthorization>(
						Context->Fingerprint, ImpactScope);
				const TSharedRef<FOperationLifecycleService> Service =
					MakeShared<FOperationLifecycleService>(
						MakeShared<FBakeVersionFence>(Context),
						MakeShared<FBakeMutation>(Context),
						MakeShared<FBakeStability>(Context),
						MakeShared<FBakePersistence>(Context),
						MakeShared<FBakeVerification>(Context));
				return FDomainOperationInvocation::TryCreate(
					Plan,
					Authorization,
					Service,
					StaticCastSharedRef<IDomainOperationInvocationContext>(Context),
					OutInvocation,
					OutError);
			}
		};

		class FBakeHandler final : public IUeAgentCommandHandler
		{
		public:
			explicit FBakeHandler(
				TSharedRef<const IDomainOperationInvocationFactory> InFactory)
				: Factory(MoveTemp(InFactory))
			{
			}

			const FString& GetCommandId() const override { return BakeId; }
			FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const Contract::FUeAgentCallerContext& Caller,
				const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
				override
			{
				FDomainOperationInvocation Invocation;
				FDomainOperationInvocationError InvocationError;
				if (!Factory->TryCreate(
						Request,
						Caller,
						Cancellation,
						Invocation,
						InvocationError))
				{
					return Failure(Request, InvocationError);
				}
				FOperationLifecycleReceipt Receipt;
				if (!Invocation.GetService()->Run(
						*Invocation.GetPlan(),
						*Invocation.GetAuthorization(),
						Receipt))
				{
					return Failure(Request, Receipt.GetDiagnostic());
				}
				FString Payload;
				UE::UAI::Contract::FDiagnostic PayloadError;
				return Invocation.GetContext()->TryBuildSuccessPayload(
						Receipt, Payload, PayloadError)
					? FUeAgentCommandResult::Success(Request, MoveTemp(Payload))
					: Failure(Request, [&]()
					{
						PayloadError.TerminalStatus = TEXT("succeeded");
						PayloadError.MutationStarted = Receipt.DidMutationStart();
						PayloadError.MutationMayHaveStarted = Receipt.DidMutationStart();
						PayloadError.bRetrySafe = false;
						return PayloadError;
					}());
			}

		private:
			TSharedRef<const IDomainOperationInvocationFactory> Factory;
		};

		class FProbeHandler final : public IUeAgentCommandHandler
		{
		public:
			const FString& GetCommandId() const override { return ProbeId; }
			FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const Contract::FUeAgentCallerContext&,
				const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
				override
			{
				if (Cancellation->IsCancelled())
				{
					return Failure(Request, NativeFailure(TEXT("texture_graph_probe_cancelled"),
						TEXT("The TextureGraph probe was cancelled before execution."), TEXT("cancellation_requested"),
						MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), FString()));
				}
				TSharedPtr<FJsonObject> Params;
				UE::UAI::Contract::FDiagnostic ParseError;
				FString Target;
				if (!CommandContract::Json::TryParseObject(
					Request.GetParamsJson(), Params, ParseError))
				{
					if (ParseError.IsEmpty()) ParseError = InputFailure(TEXT("texture_graph_probe_input_invalid"),
						TEXT("TextureGraph probe parameters could not be decoded as an object."), FString(), nullptr, MakeShared<FJsonValueString>(TEXT("object")));
					ParseError.SourceKind = TEXT("command_input");
					return Failure(Request, Contract::FDiagnostic::WrapFailure(TEXT("texture_graph_probe_input_invalid"), ParseError));
				}
				if (!Params.IsValid()) return Failure(Request, InputFailure(TEXT("texture_graph_probe_input_invalid"),
					TEXT("TextureGraph probe parameters must be an object."), FString(), nullptr, MakeShared<FJsonValueString>(TEXT("object"))));
				if (!Params->TryGetStringField(TEXT("target"), Target)) return Failure(Request, InputFailure(TEXT("texture_graph_probe_input_invalid"),
					TEXT("The target parameter must be a TextureGraph asset path string."), TEXT("/target"),
					Params->TryGetField(TEXT("target")), MakeShared<FJsonValueString>(TEXT("TextureGraph asset path string"))));
				int64 OutputNumber = 0;
				UE::UAI::Contract::FDiagnostic NumberError;
				if (Params->HasField(TEXT("render_output_index"))
					&& !UeAgentPropertyValueCodec::TryReadIntegerNumber(
						Params->TryGetField(TEXT("render_output_index")), 0, MAX_int32, OutputNumber, NumberError))
				{
					if (NumberError.IsEmpty()) NumberError = InputFailure(TEXT("texture_graph_probe_output_invalid"),
						TEXT("render_output_index must be an integer between zero and MAX_int32."), TEXT("/render_output_index"),
						Params->TryGetField(TEXT("render_output_index")), MakeShared<FJsonValueString>(TEXT("integer in [0, 2147483647]")), Target);
					NumberError.SourceKind = TEXT("command_input");
					NumberError.Source.JsonPath = TEXT("/render_output_index");
					NumberError.PropertyPath = TEXT("/render_output_index");
					NumberError.TargetPath = Target;
					return Failure(Request, Contract::FDiagnostic::WrapFailure(TEXT("texture_graph_probe_output_invalid"), NumberError));
				}
				UTextureGraph* Graph = LoadObject<UTextureGraph>(
					nullptr, *ObjectPath(Target));
				UWorld* World = GEditor
					? GEditor->GetEditorWorldContext().World() : nullptr;
				TArray<FAudioMediaGraphTextureGraphRenderedOutput> Outputs;
				FAudioMediaGraphError Error;
				FAudioMediaGraphTextureGraphNativeHook Hook;
				if (!Graph)
				{
					return Failure(Request, InputFailure(TEXT("texture_graph_probe_failed"),
						TEXT("The target TextureGraph asset could not be loaded."), TEXT("/target"),
						Params->TryGetField(TEXT("target")), MakeShared<FJsonValueString>(TEXT("loadable TextureGraph asset")), Target));
				}
				if (!World)
				{
					return Failure(Request, NativeFailure(TEXT("texture_graph_probe_failed"),
						TEXT("The TextureGraph probe requires an available Editor world."), TEXT("editor_world_available"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Target));
				}
				if (!Hook.TryRenderAndReadOutputs(*World, *Graph, Outputs, Error))
				{
					if (Error.IsEmpty()) Error = NativeFailure(TEXT("texture_graph_probe_failed"),
						TEXT("TextureGraph rendering failed without providing a diagnostic."), TEXT("render_succeeded"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Target);
					if (Error.TargetPath.IsEmpty()) Error.TargetPath = Target;
					return Failure(Request, Error);
				}
				if (!Outputs.IsValidIndex(static_cast<int32>(OutputNumber)))
				{
					return Failure(Request, InputFailure(TEXT("texture_graph_probe_failed"),
						TEXT("The selected render output index is outside the rendered output range."), TEXT("/render_output_index"),
						MakeShared<FJsonValueNumber>(OutputNumber), MakeShared<FJsonValueString>(FString::Printf(TEXT("0 <= index < %d"), Outputs.Num())), Target));
				}
				const FAudioMediaGraphTextureGraphPixelStats& PixelStats =
					Outputs[static_cast<int32>(OutputNumber)].GetStats();
				bool bPassed = true;
				const TSharedPtr<FJsonObject>* Expected = nullptr;
				if (Params->TryGetObjectField(TEXT("expected_stats"), Expected)
					&& Expected && Expected->IsValid())
				{
					auto MeetsMinimum = [&bPassed, &Expected](
						const TCHAR* Field, const double Actual)
					{
						double Minimum = 0.0;
						if ((*Expected)->TryGetNumberField(Field, Minimum))
						{
							bPassed = bPassed && Actual >= Minimum;
						}
					};
					auto MeetsMaximum = [&bPassed, &Expected](
						const TCHAR* Field, const double Actual)
					{
						double Maximum = 0.0;
						if ((*Expected)->TryGetNumberField(Field, Maximum))
						{
							bPassed = bPassed && Actual <= Maximum;
						}
					};
					MeetsMinimum(
						TEXT("min_non_black_pixel_count"),
						PixelStats.GetNonBlackPixelCount());
					MeetsMinimum(
						TEXT("min_unique_color_estimate"),
						PixelStats.GetUniqueColorEstimate());
					MeetsMinimum(TEXT("min_width"), PixelStats.GetWidth());
					MeetsMinimum(TEXT("min_height"), PixelStats.GetHeight());
					MeetsMaximum(
						TEXT("max_transparent_pixel_count"),
						PixelStats.GetTransparentPixelCount());
					bool bExpectedNonBlack = false;
					if ((*Expected)->TryGetBoolField(
							TEXT("appears_non_black"), bExpectedNonBlack))
					{
						bPassed = bPassed
							&& ((PixelStats.GetNonBlackPixelCount() > 0)
								== bExpectedNonBlack);
					}
				}
				const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
				Result->SetStringField(TEXT("command_id"), ProbeId);
				Result->SetStringField(TEXT("target"), Target);
				Result->SetNumberField(TEXT("render_output_index"), OutputNumber);
				Result->SetNumberField(TEXT("render_output_count"), Outputs.Num());
				Result->SetObjectField(TEXT("pixel_stats"), Stats(PixelStats));
				if (Expected && Expected->IsValid())
				{
					Result->SetObjectField(TEXT("expected_stats"), *Expected);
				}
				Result->SetBoolField(TEXT("probe_passed"), bPassed);
				FString Payload;
				UE::UAI::Contract::FDiagnostic ProjectionError;
				if (!Canonical(Result, Payload, ProjectionError))
				{
					if (ProjectionError.TargetPath.IsEmpty()) ProjectionError.TargetPath = Target;
					return Failure(Request, ProjectionError);
				}
				return FUeAgentCommandResult::Success(Request, MoveTemp(Payload));
			}
		};
	}

	TSharedRef<const IDomainOperationInvocationFactory>
	GetTextureGraphBakeInvocationFactory()
	{
		static const TSharedRef<const IDomainOperationInvocationFactory> Factory =
			MakeShared<const FTextureGraphBakeInvocationFactory>();
		return Factory;
	}

	bool TryCreateArtifactHandlers(
		TSharedRef<const IDomainOperationInvocationFactory> BakeFactory,
		TMap<FString, TSharedRef<IUeAgentCommandHandler>>& OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		if (BakeFactory->GetOperationId() != BakeId)
		{
			OutError = NativeFailure(TEXT("texture_graph_bake_factory_identity_mismatch"),
				TEXT("The artifact handlers require the TextureGraph bake invocation factory."), TEXT("bake_factory_identity_matches"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), FString());
			OutError.ActualEvidence = MakeShared<FJsonValueString>(BakeFactory->GetOperationId());
			OutError.ExpectedEvidence = MakeShared<FJsonValueString>(BakeId);
			return false;
		}
		OutHandlers.Add(BakeId, MakeShared<FBakeHandler>(BakeFactory));
		OutHandlers.Add(ProbeId, MakeShared<FProbeHandler>());
		return true;
	}

	bool TryCreateArtifactHandlers(
		TMap<FString, TSharedRef<IUeAgentCommandHandler>>& OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		return TryCreateArtifactHandlers(
			GetTextureGraphBakeInvocationFactory(),
			OutHandlers,
			OutError);
	}
}

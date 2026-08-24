#include "OperationOwners/TextureGraph/UeAgentTextureGraphArtifactService.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Core/AssetDurable/UeAgentPackageAssetRevision.h"
#include "Core/BindingGateway/Operation/UeAgentOperationLifecycleService.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "DomainAdapters/AudioMediaGraph/UeAgentAudioMediaGraphTextureGraphNativeHook.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PackageTools.h"
#include "TextureGraph.h"
#include "TextureCompiler.h"
#include "UObject/Package.h"

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

		FUeAgentResourceTracker MakeResources()
		{
			return FUeAgentResourceTracker(FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::OutputBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::StringBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::Nodes, 65536},
				{EUeAgentResourceDimension::Items, 65536},
				{EUeAgentResourceDimension::Files, 4096},
				{EUeAgentResourceDimension::ReferenceEdges, 65536},
				{EUeAgentResourceDimension::WorkUnits, 262144},
				{EUeAgentResourceDimension::EstimatedMemoryBytes,
					512ll * 1024 * 1024},
			}));
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
			UTextureRenderTarget2D& RenderTarget)
		{
			FTextureRenderTargetResource* Resource =
				RenderTarget.GameThread_GetRenderTargetResource();
			if (!Resource)
			{
				return nullptr;
			}
			const int32 Width = Resource->GetSizeX();
			const int32 Height = Resource->GetSizeY();
			TArray<FColor> Pixels;
			if (Width <= 0 || Height <= 0
				|| !Resource->ReadPixels(Pixels)
				|| Pixels.Num() != Width * Height)
			{
				return nullptr;
			}
			UTexture2D* Texture = NewObject<UTexture2D>(
				&Package,
				*AssetName,
				RF_Public | RF_Standalone | RF_Transactional);
			if (!Texture)
			{
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
			FString Code,
			FString Subject = FString(),
			FString Detail = FString())
		{
			const TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("code"), MoveTemp(Code));
			Error->SetStringField(TEXT("subject_id"), MoveTemp(Subject));
			Error->SetStringField(TEXT("detail"), MoveTemp(Detail));
			FString ErrorJson;
			FString JsonError;
			CommandContract::Json::TryCanonicalize(Error, ErrorJson, JsonError);
			return FUeAgentCommandResult::Failure(
				Request, TEXT("{}"), MoveTemp(ErrorJson));
		}

		bool Canonical(
			const TSharedPtr<FJsonObject>& Object,
			FString& OutJson)
		{
			FString Error;
			return CommandContract::Json::TryCanonicalize(Object, OutJson, Error);
		}

		bool CurrentRevision(
			UTextureGraph& Graph,
			const FString& TargetRef,
			FString& OutRevision)
		{
			UPackage* const Package = Graph.GetOutermost();
			if (Package == nullptr || Package->IsDirty())
			{
				OutRevision = TEXT("authoring-state-dirty");
				return false;
			}
			OutRevision = ReadPackageAssetRevision(TargetRef);
			return !OutRevision.IsEmpty();
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
			FString LastLifecycleFailureCode;
			FString LastLifecycleFailureDetail;

			FString GetFailureDetail() const override
			{
				return LastLifecycleFailureCode.IsEmpty()
					? FString()
					: LastLifecycleFailureCode + TEXT(":")
						+ LastLifecycleFailureDetail;
			}

			bool TryBuildSuccessPayload(
				const FOperationLifecycleReceipt& Receipt,
				FString& OutPayloadJson,
				FString& OutError) const override
			{
				OutPayloadJson.Reset();
				OutError.Reset();
				if (ReloadedObjectPaths.Num() != Targets.Num())
				{
					OutError = TEXT("texture_graph_bake_result_readback_missing");
					return false;
				}
				const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
				Result->SetStringField(TEXT("command_id"), BakeId);
				Result->SetStringField(TEXT("terminal_status"), TEXT("succeeded"));
				Result->SetStringField(TEXT("target"), Target);
				Result->SetStringField(TEXT("plan_fingerprint"), Fingerprint);
				Result->SetStringField(
					TEXT("persistence_receipt_id"),
					Receipt.GetPersistenceReceiptId());
				TArray<TSharedPtr<FJsonValue>> Outputs;
				for (int32 Index = 0; Index < Targets.Num(); ++Index)
				{
					const FBakeTarget& OutputTarget = Targets[Index];
					if (!Rendered.IsValidIndex(OutputTarget.OutputIndex))
					{
						OutError = TEXT("texture_graph_bake_result_output_missing");
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
				}
				Result->SetArrayField(TEXT("outputs"), MoveTemp(Outputs));
				if (!Canonical(Result, OutPayloadJson))
				{
					OutError = TEXT("texture_graph_bake_result_invalid");
					return false;
				}
				return true;
			}
		};

		bool ParseBake(
			const FUeAgentCommandRequest& Request,
			TSharedRef<FBakeContext>& Context,
			FString& OutCode,
			FString& OutSubject)
		{
			TSharedPtr<FJsonObject> Params;
			FString Error;
			if (!CommandContract::Json::TryParseObject(
				Request.GetParamsJson(), Params, Error)
				|| !Params.IsValid()
				|| !Params->TryGetStringField(TEXT("target"), Context->Target)
				|| !Params->TryGetStringField(
					TEXT("expected_revision"), Context->ExpectedRevision))
			{
				OutCode = TEXT("texture_graph_bake_input_invalid");
				OutSubject = Request.GetCommandId();
				return false;
			}
			const TArray<TSharedPtr<FJsonValue>>* Outputs = nullptr;
			if (!Params->TryGetArrayField(TEXT("outputs"), Outputs)
				|| !Outputs || Outputs->IsEmpty())
			{
				OutCode = TEXT("texture_graph_bake_outputs_required");
				OutSubject = Context->Target;
				return false;
			}
			TSet<FString> Packages;
			TSet<int32> OutputIndexes;
			for (const TSharedPtr<FJsonValue>& Value : *Outputs)
			{
				const TSharedPtr<FJsonObject> Output = Value.IsValid()
					? Value->AsObject() : nullptr;
				double IndexNumber = -1.0;
				FBakeTarget Target;
				if (!Output.IsValid()
					|| !Output->TryGetNumberField(
						TEXT("render_output_index"), IndexNumber)
					|| !Output->TryGetStringField(
						TEXT("destination"), Target.PackageName)
					|| !FMath::IsFinite(IndexNumber)
					|| FMath::FloorToDouble(IndexNumber) != IndexNumber
					|| IndexNumber < 0.0
					|| !FPackageName::IsValidLongPackageName(Target.PackageName))
				{
					OutCode = TEXT("texture_graph_bake_output_invalid");
					OutSubject = Context->Target;
					return false;
				}
				Target.OutputIndex = static_cast<int32>(IndexNumber);
				Target.AssetName =
					FPackageName::GetLongPackageAssetName(Target.PackageName);
				FString Filename;
				FPackageName::TryConvertLongPackageNameToFilename(
					Target.PackageName,
					Filename,
					FPackageName::GetAssetPackageExtension());
				if (Target.AssetName.IsEmpty()
					|| Packages.Contains(Target.PackageName)
					|| OutputIndexes.Contains(Target.OutputIndex)
					|| FindPackage(nullptr, *Target.PackageName)
					|| IFileManager::Get().FileExists(*Filename))
				{
					OutCode = TEXT("texture_graph_bake_destination_conflict");
					OutSubject = Target.PackageName;
					return false;
				}
				Packages.Add(Target.PackageName);
				OutputIndexes.Add(Target.OutputIndex);
				Context->Targets.Add(MoveTemp(Target));
			}
			Context->RequestId = Request.GetRequestId();
			Context->Fingerprint =
				CommandContract::Json::Sha256Utf8(Request.GetParamsJson());
			Context->Graph = LoadObject<UTextureGraph>(
				nullptr, *ObjectPath(Context->Target));
			if (!Context->Graph)
			{
				OutCode = TEXT("texture_graph_not_found");
				OutSubject = Context->Target;
				return false;
			}
			FString Revision;
			if (!CurrentRevision(
					*Context->Graph, Context->Target, Revision)
				|| Revision != Context->ExpectedRevision)
			{
				OutCode = TEXT("texture_graph_bake_revision_conflict");
				OutSubject = Revision;
				return false;
			}
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
				bOutCurrent = Context->Graph
					&& CurrentRevision(
						*Context->Graph, Context->Target, Revision)
					&& Revision == Context->ExpectedRevision;
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
							Revision = TEXT("destination_became_present:")
								+ Target.PackageName;
							break;
						}
					}
				}
				if (!bOutCurrent)
				{
					Context->LastLifecycleFailureCode =
						TEXT("texture_graph_bake_revision_conflict");
					Context->LastLifecycleFailureDetail = Revision;
					OutError = FOperationLifecycleError(
						TEXT("texture_graph_bake_revision_conflict"), Revision);
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
					OutError = FOperationLifecycleError(
						NativeError.GetCode().IsEmpty()
							? FString(TEXT("texture_graph_render_failed"))
							: NativeError.GetCode());
					return false;
				}
				for (const FBakeTarget& Target : Context->Targets)
				{
					if (!Context->Rendered.IsValidIndex(Target.OutputIndex))
					{
						OutError = FOperationLifecycleError(
							TEXT("texture_graph_bake_output_index_invalid"),
							FString::FromInt(Target.OutputIndex));
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
					OutError = FOperationLifecycleError(
						DurableError.GetCode(), DurableError.GetDetail());
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
					UTexture2D* Texture = Source && Package
						? CreateTextureFromRenderedOutput(
							*Package, Target.AssetName, *Source)
						: nullptr;
					if (!Texture)
					{
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
								OutError = FOperationLifecycleError(
									TEXT("texture_graph_bake_rollback_failed"),
									!CleanupError.GetCode().IsEmpty()
										? CleanupError.GetCode()
										: UnloadError.ToString());
								return false;
							}
						}
						OutError = FOperationLifecycleError(
							TEXT("texture_graph_bake_texture_create_failed"),
							Target.PackageName);
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
				return OutFacts.IsValid();
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
					OutError = FOperationLifecycleError(
						TEXT("texture_graph_bake_durable_state_unknown"),
						Context->LastLifecycleFailureCode + TEXT(":")
							+ Context->LastLifecycleFailureDetail + TEXT(":")
							+ Context->DurableReceipt.GetJournalId());
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
					OutError = FOperationLifecycleError(
						TEXT("texture_graph_bake_durable_state_unknown"),
						Context->LastLifecycleFailureCode + TEXT(":")
							+ Context->LastLifecycleFailureDetail + TEXT(":")
							+ Receipt.GetJournalId());
					return false;
				}
				if (!bPackagesUnloaded || !bDurableRolledBack)
				{
					OutError = FOperationLifecycleError(
						bPackagesUnloaded
							? FString(TEXT("texture_graph_bake_rollback_failed"))
							: FString(TEXT("texture_graph_bake_rollback_unload_failed")),
						bPackagesUnloaded
							? DurableError.GetCode() + TEXT(":")
								+ DurableError.GetDetail()
							: UnloadError.ToString());
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
					Context->LastLifecycleFailureCode =
						TEXT("texture_graph_bake_settle_incomplete");
					Context->LastLifecycleFailureDetail = FString::Printf(
						TEXT("created=%d:targets=%d:rendered=%d"),
						Context->Created.Num(), Context->Targets.Num(),
						Context->Rendered.Num());
					OutError = FOperationLifecycleError(
						TEXT("texture_graph_bake_settle_incomplete"));
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
					OutError = FOperationLifecycleError(
						TEXT("texture_graph_bake_durable_session_missing"));
					return false;
				}
				const FAssetDurableSessionRef PreparedSession =
					Context->DurableSession.ToSharedRef();
				if (!TryPersistAssetDurability(
						PreparedSession,
						Context->DurableBackend,
						Context->FencingToken,
						Context->Resources,
						Context->DurableReceipt,
						DurableError))
				{
					Context->LastLifecycleFailureCode = DurableError.GetCode();
					Context->LastLifecycleFailureDetail = DurableError.GetDetail();
					OutError = FOperationLifecycleError(
						DurableError.GetCode(), DurableError.GetDetail());
					return false;
				}
				if (Context->DurableReceipt.GetStatus()
					!= EDurableStatus::PersistedUnverified)
				{
					Context->LastLifecycleFailureCode =
						TEXT("texture_graph_bake_persistence_not_published");
					Context->LastLifecycleFailureDetail =
						LexToString(Context->DurableReceipt.GetStatus());
					OutError = FOperationLifecycleError(
						TEXT("texture_graph_bake_persistence_not_published"),
						LexToString(Context->DurableReceipt.GetStatus()));
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
				return !OutReceipt.IsEmpty();
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
					Context->LastLifecycleFailureCode =
						TEXT("texture_graph_bake_unload_failed");
					Context->LastLifecycleFailureDetail = UnloadError.ToString();
					OutError = FOperationLifecycleError(
						TEXT("texture_graph_bake_unload_failed"),
						UnloadError.ToString());
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
						Context->LastLifecycleFailureCode =
							TEXT("texture_graph_bake_readback_mismatch");
						Context->LastLifecycleFailureDetail = FString::Printf(
							TEXT("%s:expected=%dx%d:actual=%dx%d"),
							*Target.PackageName,
							Expected.GetWidth(), Expected.GetHeight(),
							Reloaded ? Reloaded->GetSizeX() : -1,
							Reloaded ? Reloaded->GetSizeY() : -1);
						OutError = FOperationLifecycleError(
							TEXT("texture_graph_bake_readback_mismatch"),
							Context->LastLifecycleFailureDetail);
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
					Context->LastLifecycleFailureCode = DurableError.GetCode();
					Context->LastLifecycleFailureDetail = DurableError.GetDetail();
					OutError = FOperationLifecycleError(
						DurableError.GetCode(), DurableError.GetDetail());
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
					OutError = FDomainOperationInvocationError(
						TEXT("texture_graph_bake_cancelled"),
						Request.GetRequestId());
					return false;
				}
				TSharedRef<FBakeContext> Context = MakeShared<FBakeContext>();
				FString Code;
				FString Subject;
				if (!ParseBake(Request, Context, Code, Subject))
				{
					OutError = FDomainOperationInvocationError(
						MoveTemp(Code), MoveTemp(Subject));
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
					return Failure(
						Request,
						InvocationError.GetCode().IsEmpty()
							? FString(TEXT("texture_graph_bake_invocation_failed"))
							: InvocationError.GetCode(),
						InvocationError.GetSubjectId(),
						InvocationError.GetDetail());
				}
				FOperationLifecycleReceipt Receipt;
				if (!Invocation.GetService()->Run(
						*Invocation.GetPlan(),
						*Invocation.GetAuthorization(),
						Receipt))
				{
					const TSharedRef<FBakeContext> BakeContext =
						StaticCastSharedRef<FBakeContext>(Invocation.GetContext());
					FString FailureDetail = BakeContext->GetFailureDetail();
					if (FailureDetail.IsEmpty())
					{
						FailureDetail = TEXT("phase_history:")
							+ FString::Join(Receipt.GetPhaseHistory(), TEXT(","));
					}
					return Failure(
						Request,
						Receipt.GetErrorCode().IsEmpty()
							? FString(TEXT("texture_graph_bake_failed"))
							: Receipt.GetErrorCode(),
						Invocation.GetPlan()->GetImpactScope()[0],
						FailureDetail);
				}
				FString Payload;
				FString PayloadError;
				return Invocation.GetContext()->TryBuildSuccessPayload(
						Receipt, Payload, PayloadError)
					? FUeAgentCommandResult::Success(Request, MoveTemp(Payload))
					: Failure(Request, PayloadError);
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
					return Failure(Request, TEXT("texture_graph_probe_cancelled"));
				}
				TSharedPtr<FJsonObject> Params;
				FString ParseError;
				FString Target;
				if (!CommandContract::Json::TryParseObject(
					Request.GetParamsJson(), Params, ParseError)
					|| !Params.IsValid()
					|| !Params->TryGetStringField(TEXT("target"), Target))
				{
					return Failure(Request, TEXT("texture_graph_probe_input_invalid"));
				}
				double OutputNumber = 0.0;
				Params->TryGetNumberField(TEXT("render_output_index"), OutputNumber);
				if (!FMath::IsFinite(OutputNumber)
					|| FMath::FloorToDouble(OutputNumber) != OutputNumber
					|| OutputNumber < 0.0)
				{
					return Failure(
						Request, TEXT("texture_graph_probe_output_invalid"), Target);
				}
				UTextureGraph* Graph = LoadObject<UTextureGraph>(
					nullptr, *ObjectPath(Target));
				UWorld* World = GEditor
					? GEditor->GetEditorWorldContext().World() : nullptr;
				TArray<FAudioMediaGraphTextureGraphRenderedOutput> Outputs;
				FAudioMediaGraphError Error;
				FAudioMediaGraphTextureGraphNativeHook Hook;
				if (!Graph || !World
					|| !Hook.TryRenderAndReadOutputs(*World, *Graph, Outputs, Error)
					|| !Outputs.IsValidIndex(static_cast<int32>(OutputNumber)))
				{
					return Failure(
						Request,
						Error.GetCode().IsEmpty()
							? FString(TEXT("texture_graph_probe_failed"))
							: Error.GetCode(),
						Target);
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
				return Canonical(Result, Payload)
					? FUeAgentCommandResult::Success(Request, MoveTemp(Payload))
					: Failure(Request, TEXT("texture_graph_probe_result_invalid"));
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
		FString& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		if (BakeFactory->GetOperationId() != BakeId)
		{
			OutError = TEXT("texture_graph_bake_factory_identity_mismatch:")
				+ BakeFactory->GetOperationId();
			return false;
		}
		OutHandlers.Add(BakeId, MakeShared<FBakeHandler>(BakeFactory));
		OutHandlers.Add(ProbeId, MakeShared<FProbeHandler>());
		return OutHandlers.Num() == 2;
	}

	bool TryCreateArtifactHandlers(
		TMap<FString, TSharedRef<IUeAgentCommandHandler>>& OutHandlers,
		FString& OutError)
	{
		return TryCreateArtifactHandlers(
			GetTextureGraphBakeInvocationFactory(),
			OutHandlers,
			OutError);
	}
}

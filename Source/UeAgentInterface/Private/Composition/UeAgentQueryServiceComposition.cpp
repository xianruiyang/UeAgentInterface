#include "Composition/UeAgentQueryServiceComposition.h"

#include "Async/Async.h"
#include "Core/BindingGateway/Query/UeAgentQueryBinding.h"
#include "Core/BindingGateway/Ticket/UeAgentBindingTicket.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/Projection/UeAgentProjection.h"
#include "Composition/NiagaraCompound/UeAgentNiagaraCompoundApplication.h"
#include "Composition/DestructiveImport/UeAgentDestructiveImportActivation.h"
#include "Composition/DestructiveImport/UeAgentDestructiveImportCommandHandlers.h"
#include "Composition/DestructiveImport/UeAgentDestructiveImportExternalCommandHandlers.h"
#include "Composition/DestructiveImport/UeAgentDestructiveImportWorldCommandHandlers.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundActivation.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundAnimationBindings.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundAudioMediaBindings.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundAudioMediaRoutes.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundBlueprintMaterialBindings.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundBlueprintMaterialRoutes.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundCommandHandlers.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundPcgAiBindings.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundPcgAnimationRoutes.h"
#include "Composition/LongRunningSession/UeAgentLongRunningSessionActivation.h"
#include "Composition/LongRunningSession/UeAgentLongRunningSessionProduction.h"
#include "Composition/ReadExportCommand/UeAgentReadExportCommandHandlers.h"
#include "Composition/ReferenceCommand/UeAgentReferenceCommandHandlers.h"
#include "Composition/ReferenceCrud/UeAgentReferenceCrudApplication.h"
#include "Composition/UeAgentCommandServiceComposition.h"
#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringRuntime.h"
#include "Composition/TimelineSpatial/UeAgentTimelineSpatialActivation.h"
#include "Composition/TimelineSpatial/UeAgentTimelineSpatialHjsonCommandHandler.h"
#include "Composition/TimelineSpatial/UeAgentTimelineSpatialRuntime.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "DomainAdapters/GenericRead/UeAgentGenericRead.h"
#include "DomainAdapters/AssetDestructiveImport/UeAgentAssetDeleteOwner.h"
#include "DomainAdapters/AssetDestructiveImport/UeAgentAssetOperationsOwner.h"
#include "DomainAdapters/ExternalDataImport/UeAgentExternalDataImportOwner.h"
#include "DomainAdapters/WorldDestructive/UeAgentWorldDestructiveOwner.h"
#include "DomainAdapters/GenericRead/UeAgentGenericReadAdapter.h"
#include "DomainAdapters/AssetProjectRead/UeAgentAssetProjectReadNative.h"
#include "DomainAdapters/AssetProjectRead/UeAgentAssetProjectReadProductionEnvironment.h"
#include "DomainAdapters/AssetProjectRead/UeAgentAssetProjectReadProvider.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadNative.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProvider.h"
#include "DomainAdapters/NiagaraCompound/UeAgentNiagaraCompoundAdapter.h"
#include "DomainAdapters/NiagaraCompound/UeAgentNiagaraCompoundDiscovery.h"
#include "DomainAdapters/NiagaraCompound/UeAgentNiagaraCompoundNativeHook.h"
#include "DomainAdapters/ReferenceAsset/UeAgentReferenceAssetAdapter.h"
#include "DomainAdapters/WorldRead/UeAgentWorldReadNative.h"
#include "DomainAdapters/WorldRead/UeAgentWorldReadProductionEnvironment.h"
#include "DomainAdapters/WorldRead/UeAgentWorldReadProvider.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "NetworkTransport/UeAgentHttpTransportAdapter.h"
#include "Serialization/JsonSerializer.h"
#include "UaiProtocol/UeAgentUaiProtocol.h"

namespace UE::UAI::Composition
{
	namespace
	{
		using namespace BindingGateway::Query;
		using namespace BindingGateway::Operation;
		using namespace BindingGateway::Ticket;
		using namespace ContextResource;
		using namespace Contract;
		using namespace Discovery;
		using namespace DomainAdapters::GenericRead;
		using namespace DomainAdapters::NiagaraCompound;
		using namespace DomainAdapters::ReferenceAsset;
		using namespace NetworkTransport;
		using namespace Projection;
		using namespace Protocol;
		using namespace StateSemantic;

		const FString ReferenceRegistryHash =
			TEXT("sha256:5388c801035019359c568de214b7dc2fe2d2979bf429175d89a03932c41253ea");
		const FString ReferenceDescriptorId =
			TEXT("uai.query.reference.read");
		const FString ReferenceDescriptorHash =
			TEXT("sha256:b66bbe297913f4df06f04bec2091211abd48adb7ff89b1b021e59a8495038c75");
		const FString ReferenceOperationDescriptorId =
			TEXT("uai.operation.reference.crud");
		const FString ReferenceOperationDescriptorHash =
			TEXT("sha256:fbde900ec387e61915a21e97ae5575a85337b3866d4299be961f75a5fc888491");
		const FString ReferenceCatalogKind = TEXT("uai_reference");
		const FString ReferenceCatalogSourceId =
			TEXT("uai-reference-catalog");
		const FString ReferenceReadPortId =
			TEXT("uai-reference-generic-read");

		bool TryCreateReferenceOperationDescriptorRegistryInternal(
			TSharedPtr<const FUeAgentDescriptorRegistry>&
				OutRegistry,
			FString& OutError)
		{
			return FUeAgentDescriptorRegistry::TryCreate(
				{
					FUeAgentOperationDescriptor(
						GetReferenceOperationDescriptorIdentity(),
						TEXT("reference.asset_apply"),
						TEXT("reference"),
						EUeAgentDescriptorOperationKind::
							Operation,
						FUeAgentExecutionContextPolicy(
							{EUeAgentStatePlane::Persisted},
							{},
							EUeAgentContextResolutionPolicy::
								ExplicitOnly,
							EUeAgentTransientContextPolicy::
								ReadOnly),
						FUeAgentContractValue(
							TEXT("{\"type\":\"object\"}")),
						FUeAgentContractValue(
							TEXT("{\"type\":\"object\"}"))),
				},
				OutRegistry,
				OutError);
		}

		void SetError(
			FUeAgentQueryServiceError& OutError,
			const FString& Code,
			const FString& Detail)
		{
			OutError = {Code, Detail};
		}

		AWorldSettings* ResolveWorldSettings(
			const TWeakObjectPtr<AWorldSettings>& Seed)
		{
			if (GEditor)
			{
				if (UWorld* World =
						GEditor->GetEditorWorldContext().World())
				{
					if (AWorldSettings* Current =
							World->GetWorldSettings())
					{
						return Current;
					}
				}
			}
			return Seed.Get();
		}

		bool Checkpoint(
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			const FString& Stage,
			FUeAgentDiscoveryError& OutError)
		{
			FUeAgentCancellationRequested Cancelled;
			if (!Cancellation.TryCheckpoint(Cancelled))
			{
				OutError.Code = Cancelled.Code;
				OutError.SubjectId = Cancelled.Reason;
				OutError.Detail = TEXT("reference query was cancelled");
				return false;
			}
			FUeAgentOperationDeadlineExceeded Expired;
			if (!Deadline.TryCheckpoint(Stage, Expired))
			{
				OutError.Code = Expired.Code;
				OutError.SubjectId = Stage;
				OutError.Detail =
					TEXT("reference query deadline expired");
				return false;
			}
			return true;
		}

		FUeAgentCandidateBinding BuildReferenceBinding(
			AWorldSettings& WorldSettings,
			const FString& Generation)
		{
			const FUeAgentCanonicalIdentity Identity(
				TEXT("core.editor_world_settings"),
				TEXT("editor_world_settings"),
				WorldSettings.GetPathName(),
				TEXT("PivotOffset"),
				TEXT("world_settings"),
				Generation);
			const FUeAgentCandidateSummary Summary(
				Identity,
				GetReferenceQueryDescriptorIdentity(),
				ReferenceCatalogKind,
				TEXT("Editor World Settings Pivot Offset"),
				TEXT("Real reflected UE editor-world state"),
				{TEXT("UAI"), TEXT("ReferenceQuery")});
			return FUeAgentCandidateBinding(
				TEXT("uai-reference-binding:") + Generation,
				Summary,
				FUeAgentCatalogRevision(
					ReferenceCatalogSourceId,
					Generation,
					1),
				TEXT("uai://reference/editor-world-settings/pivot-offset"),
				ReferenceReadPortId,
				{EUeAgentCandidateOperation::Read},
				TEXT("ue-reflection:AWorldSettings.PivotOffset"));
		}

		class FReferenceCatalogSource final
			: public IUeAgentCatalogSource
		{
		public:
			FReferenceCatalogSource(
				AWorldSettings& InWorldSettings,
				FString InGeneration)
				: Seed(&InWorldSettings)
				, Generation(MoveTemp(InGeneration))
				, Kinds({ReferenceCatalogKind})
			{
			}

			virtual const FString& GetSourceId() const override
			{
				return ReferenceCatalogSourceId;
			}

			virtual const TArray<FString>&
			GetCatalogKinds() const override
			{
				return Kinds;
			}

			virtual bool TrySearch(
				const FUeAgentDiscoveryQuery& Query,
				FUeAgentResourceTracker& Resources,
				const FUeAgentOperationDeadline& Deadline,
				const FUeAgentCancellationToken& Cancellation,
				TSharedPtr<const FUeAgentCatalogSnapshot>& OutCatalog,
				FUeAgentDiscoveryError& OutError) override
			{
				OutCatalog.Reset();
				OutError = {};
				if (!Checkpoint(
						Deadline,
						Cancellation,
						TEXT("reference_catalog.search"),
						OutError))
				{
					return false;
				}
				AWorldSettings* WorldSettings =
					ResolveWorldSettings(Seed);
				if (!IsValid(WorldSettings))
				{
					OutError.Code =
						TEXT("reference_world_settings_unavailable");
					OutError.SubjectId = TEXT("editor_world_settings");
					OutError.Detail =
						TEXT("editor world settings are unavailable");
					return false;
				}
				const FUeAgentCandidateBinding Binding =
					BuildReferenceBinding(*WorldSettings, Generation);
				const FString Search =
					Query.GetSearchText().TrimStartAndEnd();
				const bool bMatch = Search.IsEmpty()
					|| FString(TEXT("PivotOffset")).Contains(
						Search,
						ESearchCase::IgnoreCase)
					|| FString(
						TEXT("Editor World Settings Pivot Offset"))
						.Contains(
							Search,
							ESearchCase::IgnoreCase);
				const TArray<FUeAgentCandidateSummary> Candidates =
					bMatch
						? TArray<FUeAgentCandidateSummary>{
							Binding.GetSummary()}
						: TArray<FUeAgentCandidateSummary>{};
				return FUeAgentCatalogSnapshot::TryCreate(
					TEXT("uai-reference-catalog:") + Generation,
					ReferenceCatalogKind,
					Binding.GetCatalogRevision(),
					EUeAgentCatalogCompleteness::Complete,
					Candidates,
					{},
					Resources,
					OutCatalog,
					OutError);
			}

			virtual bool TryResolveExact(
				const FString& CandidateId,
				const FUeAgentCatalogRevision& ExpectedRevision,
				FUeAgentResourceTracker& Resources,
				const FUeAgentOperationDeadline& Deadline,
				const FUeAgentCancellationToken& Cancellation,
				TSharedPtr<const FUeAgentCandidateBinding>& OutBinding,
				FUeAgentDiscoveryError& OutError) override
			{
				OutBinding.Reset();
				OutError = {};
				if (!Checkpoint(
						Deadline,
						Cancellation,
						TEXT("reference_catalog.resolve"),
						OutError))
				{
					return false;
				}
				AWorldSettings* WorldSettings =
					ResolveWorldSettings(Seed);
				if (!IsValid(WorldSettings))
				{
					OutError.Code =
						TEXT("reference_world_settings_unavailable");
					OutError.SubjectId = TEXT("editor_world_settings");
					OutError.Detail =
						TEXT("editor world settings are unavailable");
					return false;
				}
				const FUeAgentCandidateBinding Binding =
					BuildReferenceBinding(*WorldSettings, Generation);
				if (ExpectedRevision != Binding.GetCatalogRevision())
				{
					OutError.Code = TEXT("catalog_revision_stale");
					OutError.SubjectId =
						ExpectedRevision.GetGeneration();
					OutError.Detail =
						TEXT("reference catalog revision changed");
					return false;
				}
				if (CandidateId
					!= Binding.GetSummary()
						.GetIdentity()
						.GetCanonicalKey())
				{
					OutError.Code = TEXT("candidate_not_found");
					OutError.SubjectId = CandidateId;
					OutError.Detail =
						TEXT("reference candidate identity is unknown");
					return false;
				}
				FUeAgentResourceBudgetExceeded BudgetError;
				if (!Resources.TryCharge(
						EUeAgentResourceDimension::Items,
						1,
						TEXT("reference_catalog.resolve"),
						BudgetError))
				{
					OutError.Code = BudgetError.Code;
					OutError.SubjectId = BudgetError.Path;
					OutError.Detail =
						TEXT("reference resolve budget exceeded");
					OutError.ResourceDimension =
						BudgetError.Dimension;
					OutError.ResourceLimit = BudgetError.Limit;
					OutError.ResourceObserved =
						BudgetError.Observed;
					return false;
				}
				OutBinding =
					MakeShared<FUeAgentCandidateBinding>(Binding);
				return true;
			}

		private:
			TWeakObjectPtr<AWorldSettings> Seed;
			FString Generation;
			TArray<FString> Kinds;
		};

		FUeAgentGenericReadRequest MakeReadTemplate(
			AWorldSettings& WorldSettings,
			const FString& Generation)
		{
			FUeAgentGenericPropertyBinding Property;
			Property.PropertyPath = TEXT("PivotOffset");
			Property.Identity = {
				TEXT("core.editor_world_settings"),
				TEXT("editor_world_settings"),
				WorldSettings.GetPathName(),
				TEXT("PivotOffset"),
				TEXT("world_settings"),
				Generation,
			};
			Property.CanonicalTypeId =
				TEXT("core.spatial.position3");
			Property.ValueKind =
				EUeAgentGenericValueKind::SpatialPoint;
			Property.BindingEvidence =
				TEXT("ue-reflection:AWorldSettings.PivotOffset");
			Property.DefaultSourceIdentity =
				TEXT("UE.AWorldSettings.CDO");
			Property.DefaultProvenance =
				TEXT("ue-reflection:class-default-object");
			Property.Spatial = {
				TEXT("editor-world-settings"),
				TEXT("ue.world"),
				TEXT("ue.world"),
				TEXT("centimeter"),
				TEXT(
					"UE world basis: +X forward, +Y right, +Z up"),
			};

			FUeAgentGenericReadRequest Request;
			Request.RootObject = &WorldSettings;
			Request.SnapshotId =
				TEXT("uai-reference-state:") + Generation;
			Request.StatePlane = TEXT("authoring_memory");
			Request.SourceId = ReferenceReadPortId;
			Request.Generation = Generation;
			Request.Sequence = 1;
			Request.Properties = {MoveTemp(Property)};
			return Request;
		}

		class FReferenceReadHook final : public IUeAgentUeReadHook
		{
		public:
			FReferenceReadHook(
				AWorldSettings& InWorldSettings,
				FString InGeneration)
				: Seed(&InWorldSettings)
				, Generation(MoveTemp(InGeneration))
			{
			}

			virtual bool TryCapture(
				const FUeAgentStateReadRequest& Request,
				FUeAgentResourceTracker& Resources,
				const FUeAgentOperationDeadline& Deadline,
				const FUeAgentCancellationToken& Cancellation,
				TSharedPtr<const FUeAgentUeReadCapture>& OutCapture,
				FUeAgentGenericReadError& OutError) override
			{
				AWorldSettings* WorldSettings =
					ResolveWorldSettings(Seed);
				if (!IsValid(WorldSettings))
				{
					OutError = {
						TEXT("reference_world_settings_unavailable"),
						TEXT("editor_world_settings"),
						TEXT("editor world settings are unavailable"),
					};
					return false;
				}
				FUeAgentReflectionGenericReadHook Reflection(
					MakeReadTemplate(*WorldSettings, Generation));
				return Reflection.TryCapture(
					Request,
					Resources,
					Deadline,
					Cancellation,
					OutCapture,
					OutError);
			}

		private:
			TWeakObjectPtr<AWorldSettings> Seed;
			FString Generation;
		};

		class FReferenceTokenEstimator final
			: public IUeAgentTokenEstimator
		{
		public:
			virtual bool TryEstimate(
				const FUeAgentProjectedPayload& Payload,
				const TArray<FUeAgentArtifactDescriptor>& Artifacts,
				const TArray<FUeAgentProjectionDiagnostic>& Diagnostics,
				FUeAgentResourceTracker& Resources,
				const FUeAgentOperationDeadline& Deadline,
				const FUeAgentCancellationToken& Cancellation,
				FUeAgentTokenEstimate& OutEstimate,
				FUeAgentProjectionError& OutError) override
			{
				OutError = {};
				FUeAgentCancellationRequested Cancelled;
				if (!Cancellation.TryCheckpoint(Cancelled))
				{
					OutError.Code = Cancelled.Code;
					OutError.SubjectId = Cancelled.Reason;
					OutError.Detail =
						TEXT("token estimation was cancelled");
					return false;
				}
				FUeAgentOperationDeadlineExceeded Expired;
				if (!Deadline.TryCheckpoint(
						TEXT("reference_projection.estimate"),
						Expired))
				{
					OutError.Code = Expired.Code;
					OutError.SubjectId =
						TEXT("reference_projection.estimate");
					OutError.Detail =
						TEXT("token estimation deadline expired");
					return false;
				}
				int64 Utf8Bytes = 512;
				if (Payload.GetCatalog().IsValid())
				{
					Utf8Bytes +=
						Payload.GetCatalog()->GetCandidates().Num()
						* 1024;
				}
				else if (Payload.GetCandidateBinding().IsValid())
				{
					Utf8Bytes += 1536;
				}
				else if (Payload.GetSnapshot().IsValid())
				{
					Utf8Bytes +=
						Payload.GetSnapshot()->GetEntries().Num()
						* 2048;
				}
				Utf8Bytes += Artifacts.Num() * 512;
				Utf8Bytes += Diagnostics.Num() * 256;
				FUeAgentResourceBudgetExceeded BudgetError;
				if (!Resources.TryCharge(
						EUeAgentResourceDimension::WorkUnits,
						1,
						TEXT("reference_projection.estimate"),
						BudgetError))
				{
					OutError.Code = BudgetError.Code;
					OutError.SubjectId = BudgetError.Path;
					OutError.Detail =
						TEXT("token estimation budget exceeded");
					OutError.ResourceDimension =
						BudgetError.Dimension;
					OutError.ResourceLimit = BudgetError.Limit;
					OutError.ResourceObserved =
						BudgetError.Observed;
					return false;
				}
				OutEstimate = FUeAgentTokenEstimate(
					EUeAgentTokenEstimateKind::Conservative,
					FMath::Max<int64>(1, (Utf8Bytes + 2) / 3),
					Utf8Bytes);
				return true;
			}
		};

		class FReferenceSessionSource final
			: public IUeAgentQuerySessionSource
		{
		public:
			explicit FReferenceSessionSource(
				FUeAgentQuerySessionIdentity InSession)
				: Session(MoveTemp(InSession))
			{
			}

			virtual FUeAgentQuerySessionIdentity
			GetCurrent() const override
			{
				return Session;
			}

		private:
			FUeAgentQuerySessionIdentity Session;
		};

		class FNeverCancelledSignal final
			: public IUeAgentCancellationSignal
		{
		public:
			virtual bool IsCancelled() const override
			{
				return false;
			}
		};

		FString JsonText(const TSharedRef<FJsonObject>& Object)
		{
			FString Text;
			FString Error;
			return Json::TryCanonicalize(Object, Text, Error)
				? Text
				: FString();
		}

		FUeAgentQueryResult QueryErrorResult(
			const FUeAgentQueryRequest& Request,
			const FString& Code,
			const FString& SubjectId,
			const FString& Detail)
		{
			TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("code"), Code);
			Error->SetStringField(TEXT("subject_id"), SubjectId);
			Error->SetStringField(TEXT("detail"), Detail);
			TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetObjectField(TEXT("query_error"), Error);
			return FUeAgentQueryResult(
				Request.GetDescriptorIdentity(),
				Request.GetRequestId(),
				FUeAgentContractValue(JsonText(Root)));
		}

		FUeAgentResourceTracker MakeQueryResources(
			int64 MaxFrameBytes)
		{
			const int64 ByteLimit =
				FMath::Max<int64>(MaxFrameBytes * 4, 4 * 1024 * 1024);
			return FUeAgentResourceTracker(FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, ByteLimit},
				{EUeAgentResourceDimension::OutputBytes, ByteLimit},
				{EUeAgentResourceDimension::StringBytes, ByteLimit},
				{EUeAgentResourceDimension::Nodes, 16384},
				{EUeAgentResourceDimension::Items, 16384},
				{EUeAgentResourceDimension::Files, 64},
				{EUeAgentResourceDimension::ReferenceEdges, 16384},
				{EUeAgentResourceDimension::WorkUnits, 65536},
				{
					EUeAgentResourceDimension::EstimatedMemoryBytes,
					ByteLimit * 4,
				},
			}));
		}

		FUeAgentQueryResult ExecuteQuery(
			const TSharedRef<
				const BindingGateway::Query::
					FUeAgentQueryBinding>& Binding,
			double TimeoutSeconds,
			int64 MaxFrameBytes,
			const FUeAgentQueryRequest& Request,
			const FUeAgentCallerContext& CallerContext,
			const TSharedRef<IUeAgentCancellationSignal>& Cancellation)
		{
			FUeAgentResourceTracker Resources =
				MakeQueryResources(MaxFrameBytes);
			TOptional<FUeAgentQueryBindingPayload> Payload;
			FUeAgentQueryBindingError Error;
			if (!FUeAgentQueryPayloadCodec::TryDecode(
					Request.GetPayload(),
					Resources,
					Payload,
					Error))
			{
				return QueryErrorResult(
					Request,
					Error.Code,
					Error.SubjectId,
					Error.Detail);
			}
			const auto Clock = []()
			{
				return FPlatformTime::Seconds();
			};
			FUeAgentCancellationSource CancellationSource(Clock);
			if (Cancellation->IsCancelled())
			{
				CancellationSource.Request(
					TEXT("protocol_cancellation_requested"));
			}
			const FUeAgentOperationDeadline Deadline =
				FUeAgentOperationDeadline::FromTimeout(
					TimeoutSeconds,
					Clock);
			const FUeAgentQueryBindingRequest TypedRequest(
				Request.GetDescriptorIdentity(),
				Request.GetRequestId(),
				Payload.GetValue());
			TSharedPtr<const FUeAgentQueryBindingResult> TypedResult;
			if (!Binding->TrySubmit(
					TypedRequest,
					CallerContext,
					Resources,
					Deadline,
					CancellationSource.GetToken(),
					TypedResult,
					Error))
			{
				return QueryErrorResult(
					Request,
					Error.Code,
					Error.SubjectId,
					Error.Detail);
			}
			FUeAgentContractValue Encoded(TEXT("{}"));
			if (!FUeAgentQueryPayloadCodec::TryEncode(
					TypedResult->GetValue(),
					Resources,
					Encoded,
					Error))
			{
				return QueryErrorResult(
					Request,
					Error.Code,
					Error.SubjectId,
					Error.Detail);
			}
			return FUeAgentQueryResult(
				TypedResult->GetDescriptorIdentity(),
				TypedResult->GetRequestId(),
				MoveTemp(Encoded));
		}

		class FReferenceQueryApplicationPort final
			: public IUeAgentQueryApplicationPort
		{
		public:
			FReferenceQueryApplicationPort(
				TSharedRef<
					const BindingGateway::Query::
						FUeAgentQueryBinding> InBinding,
				double InTimeoutSeconds,
				int64 InMaxFrameBytes)
				: Binding(MoveTemp(InBinding))
				, TimeoutSeconds(InTimeoutSeconds)
				, MaxFrameBytes(InMaxFrameBytes)
			{
			}

			virtual FUeAgentQueryResult Submit(
				const FUeAgentQueryRequest& Request,
				const FUeAgentCallerContext& CallerContext,
				const TSharedRef<IUeAgentCancellationSignal>&
					Cancellation) override
			{
				if (IsInGameThread())
				{
					return ExecuteQuery(
						Binding,
						TimeoutSeconds,
						MaxFrameBytes,
						Request,
						CallerContext,
						Cancellation);
				}

				const TSharedRef<
					TOptional<FUeAgentQueryResult>,
					ESPMode::ThreadSafe> Result =
					MakeShared<
						TOptional<FUeAgentQueryResult>,
						ESPMode::ThreadSafe>();
				FEvent* Completed =
					FPlatformProcess::GetSynchEventFromPool(true);
				const TSharedRef<
					const BindingGateway::Query::
						FUeAgentQueryBinding>
					SafeBinding = Binding;
				const double SafeTimeout = TimeoutSeconds;
				const int64 SafeFrameBytes = MaxFrameBytes;
				AsyncTask(
					ENamedThreads::GameThread,
					[
						Result,
						Completed,
						SafeBinding,
						SafeTimeout,
						SafeFrameBytes,
						Request,
						CallerContext,
						Cancellation
					]()
					{
						Result->Emplace(ExecuteQuery(
							SafeBinding,
							SafeTimeout,
							SafeFrameBytes,
							Request,
							CallerContext,
							Cancellation));
						Completed->Trigger();
					});
				Completed->Wait();
				FPlatformProcess::ReturnSynchEventToPool(Completed);
				return Result->GetValue();
			}

		private:
			TSharedRef<
				const BindingGateway::Query::
					FUeAgentQueryBinding> Binding;
			double TimeoutSeconds;
			int64 MaxFrameBytes;
		};

		FUeAgentResourceBudget MakeExportBudget(
			const int64 MaxFrameBytes)
		{
			const int64 ByteLimit =
				FMath::Max<int64>(
					MaxFrameBytes * 16,
					16 * 1024 * 1024);
			return FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, ByteLimit},
				{EUeAgentResourceDimension::OutputBytes, ByteLimit},
				{EUeAgentResourceDimension::StringBytes, ByteLimit},
				{EUeAgentResourceDimension::Nodes, 65536},
				{EUeAgentResourceDimension::Items, 65536},
				{EUeAgentResourceDimension::Files, 64},
				{EUeAgentResourceDimension::ReferenceEdges, 65536},
				{EUeAgentResourceDimension::WorkUnits, 262144},
				{
					EUeAgentResourceDimension::EstimatedMemoryBytes,
					ByteLimit * 2,
				},
			});
		}

		

		

		

		

		

		

		

	}

	bool TryCreateReferenceOperationDescriptorRegistry(
		TSharedPtr<const FUeAgentDescriptorRegistry>& OutRegistry,
		FString& OutError)
	{
		return TryCreateReferenceOperationDescriptorRegistryInternal(
			OutRegistry,
			OutError);
	}

	struct FUeAgentQueryServiceComposition::FImpl
	{
		FString OwnerId;
		TSharedPtr<
			FReferenceQueryApplicationPort,
			ESPMode::ThreadSafe> QueryPort;
		TSharedPtr<
			ReferenceCrud::FUeAgentReferenceCrudApplication,
			ESPMode::ThreadSafe> OperationPort;
		TSharedPtr<SimpleAuthoring::FUeAgentSimpleAuthoringRuntime>
			SimpleAuthoringRuntime;
		TSharedPtr<TimelineSpatial::FUeAgentTimelineSpatialRuntime>
			TimelineSpatialRuntime;
		TSharedPtr<LongRunningSession::FUeAgentLongRunningSessionRuntime>
			LongRunningSessionRuntime;
		TSharedPtr<GraphCompound::FGraphCompoundRuntime>
			BlueprintMaterialGraphRuntime;
		TSharedPtr<GraphCompound::FGraphCompoundRuntime>
			PcgAiGraphRuntime;
		TSharedPtr<GraphCompound::FGraphCompoundRuntime>
			AnimationGraphRuntime;
		TSharedPtr<GraphCompound::FGraphCompoundRuntime>
			AudioMediaGraphRuntime;
		TUniquePtr<FUeAgentCommandServiceComposition>
			CommandService;
		TUniquePtr<FUeAgentHttpTransportAdapter>
			CommandTransport;
		bool bRunning = false;
	};

	const FString& GetReferenceQueryRegistryHash()
	{
		return ReferenceRegistryHash;
	}

	FUeAgentDescriptorIdentity GetReferenceQueryDescriptorIdentity()
	{
		return FUeAgentDescriptorIdentity(
			ReferenceDescriptorId,
			1,
			ReferenceDescriptorHash);
	}

	FUeAgentDescriptorIdentity
	GetReferenceOperationDescriptorIdentity()
	{
		return FUeAgentDescriptorIdentity(
			ReferenceOperationDescriptorId,
			1,
			ReferenceOperationDescriptorHash);
	}

	FUeAgentQueryServiceComposition::FUeAgentQueryServiceComposition(
		TUniquePtr<FImpl> InImpl)
		: Impl(MoveTemp(InImpl))
	{
	}

	FUeAgentQueryServiceComposition::~FUeAgentQueryServiceComposition()
	{
		if (Impl && Impl->bRunning)
		{
			FUeAgentTransportDiagnostic Ignored;
			Stop(Ignored);
		}
	}

	bool FUeAgentQueryServiceComposition::TryCreateReference(
		const FUeAgentQueryServiceConfig& Config,
		AWorldSettings& WorldSettings,
		TSharedRef<IUeAgentHttpRouteHost, ESPMode::ThreadSafe> RouteHost,
		TUniquePtr<FUeAgentQueryServiceComposition>& OutComposition,
		FUeAgentQueryServiceError& OutError)
	{
		OutComposition.Reset();
		OutError = {};
		if (Config.OwnerId.TrimStartAndEnd().IsEmpty()
			|| Config.InstanceId.TrimStartAndEnd().IsEmpty()
			|| Config.ServerSessionId.TrimStartAndEnd().IsEmpty()
			|| Config.RegistryEpoch < 1
			|| Config.AuthToken.IsEmpty()
			|| Config.TicketIntegrityKey.IsEmpty()
			|| Config.QueryTimeoutSeconds <= 0.0
			|| Config.TicketTtlSeconds <= 0.0
			|| Config.MaxFrameBytes <= 0
			|| !IsValid(&WorldSettings))
		{
			SetError(
				OutError,
				TEXT("query_service_config_invalid"),
				TEXT(
					"reference query service configuration is incomplete"));
			return false;
		}

		TSharedPtr<const FUeAgentDescriptorRegistry>
			ReferenceOperationDescriptors;
		FString ReferenceOperationDescriptorError;
		if (!TryCreateReferenceOperationDescriptorRegistry(
				ReferenceOperationDescriptors,
				ReferenceOperationDescriptorError)
			|| !ReferenceOperationDescriptors.IsValid())
		{
			SetError(
				OutError,
				ReferenceOperationDescriptorError,
				TEXT(
					"reference operation descriptor registry "
					"could not compose"));
			return false;
		}

		const TSharedRef<FReferenceCatalogSource> CatalogSource =
			MakeShared<FReferenceCatalogSource>(
				WorldSettings,
				Config.ServerSessionId);
		const TSharedRef<FReferenceReadHook> ReadHook =
			MakeShared<FReferenceReadHook>(
				WorldSettings,
				Config.ServerSessionId);
		const TSharedRef<FUeAgentGenericReadAdapter> ReadPort =
			MakeShared<FUeAgentGenericReadAdapter>(
				ReferenceReadPortId,
				ReadHook);
		const TSharedRef<FUeAgentReferenceAssetCatalogSource>
			AssetCatalogSource =
				MakeShared<FUeAgentReferenceAssetCatalogSource>(
					GetReferenceOperationDescriptorIdentity(),
					Config.ServerSessionId);
		const TSharedRef<FUeAgentReferenceAssetReadHook>
			AssetReadHook =
				MakeShared<FUeAgentReferenceAssetReadHook>(
					Config.ServerSessionId);
		const TSharedRef<FUeAgentGenericReadAdapter>
			AssetReadPort =
				MakeShared<FUeAgentGenericReadAdapter>(
					GetReferenceAssetReadPortId(),
					AssetReadHook);
		const TSharedRef<FUeAgentNiagaraCompoundNativeHook>
			NiagaraNativeHook =
				MakeShared<FUeAgentNiagaraCompoundNativeHook>(
					Config.ServerSessionId);
		const TSharedRef<FUeAgentNiagaraCompoundCatalogSource>
			NiagaraCatalogSource =
				MakeShared<
					FUeAgentNiagaraCompoundCatalogSource>(
					GetNiagaraCompoundOperationDescriptorIdentity(),
					Config.ServerSessionId);
		const TSharedRef<FUeAgentNiagaraCompoundReadHook>
			NiagaraReadHook =
				MakeShared<FUeAgentNiagaraCompoundReadHook>(
					NiagaraNativeHook);
		const TSharedRef<FUeAgentGenericReadAdapter>
			NiagaraReadPort =
				MakeShared<FUeAgentGenericReadAdapter>(
					GetNiagaraCompoundReadPortId(),
					NiagaraReadHook);
		TSharedPtr<const FUeAgentDiscoveryService> Discovery;
		FUeAgentDiscoveryError DiscoveryError;

		const TSharedRef<FReferenceTokenEstimator> Estimator =
			MakeShared<FReferenceTokenEstimator>();
		TSharedPtr<const FUeAgentResponseProjector> Projector;
		FUeAgentProjectionError ProjectionError;
		if (!FUeAgentResponseProjector::TryCreate(
				Estimator,
				Projector,
				ProjectionError))
		{
			SetError(
				OutError,
				ProjectionError.Code,
				ProjectionError.Detail);
			return false;
		}

		const TSharedRef<FReferenceSessionSource> SessionSource =
			MakeShared<FReferenceSessionSource>(
				FUeAgentQuerySessionIdentity(
					Config.ServerSessionId,
					Config.RegistryEpoch,
					ReferenceRegistryHash));
		TSharedPtr<const FUeAgentOperationTicketService>
			SharedTicketService;
		FUeAgentOperationTicketError TicketError;
		if (!FUeAgentOperationTicketService::TryCreate(
				Config.TicketIntegrityKey,
				Config.TicketTtlSeconds,
				[]()
				{
					return FPlatformTime::Seconds();
				},
				[]()
				{
					return FGuid::NewGuid().ToString(
						EGuidFormats::Digits);
				},
				SharedTicketService,
				TicketError))
		{
			SetError(
				OutError,
				TicketError.Code,
				TicketError.Detail);
			return false;
		}
		TSharedPtr<const FUeAgentQueryTicketService> TicketService;
		FUeAgentQueryBindingError QueryError;
		if (!FUeAgentQueryTicketService::TryCreateFromShared(
				SharedTicketService.ToSharedRef(),
				TicketService,
				QueryError))
		{
			SetError(
				OutError,
				QueryError.Code,
				QueryError.Detail);
			return false;
		}

		UWorld* ExplicitWorld = WorldSettings.GetWorld();
		if (!IsValid(ExplicitWorld))
		{
			SetError(
				OutError,
				TEXT("simple_authoring_runtime_world_unavailable"),
				WorldSettings.GetPathName());
			return false;
		}
		SimpleAuthoring::FUeAgentSimpleAuthoringRuntimeConfig
			SimpleRuntimeConfig;
		SimpleRuntimeConfig.ProjectInstanceId = Config.OwnerId;
		SimpleRuntimeConfig.EditorInstanceId = Config.InstanceId;
		SimpleRuntimeConfig.ServerSessionId = Config.ServerSessionId;
		SimpleRuntimeConfig.RegistryEpoch = Config.RegistryEpoch;
		SimpleRuntimeConfig.TimeoutSeconds = Config.QueryTimeoutSeconds;
		SimpleRuntimeConfig.MaxFrameBytes = Config.MaxFrameBytes;
		SimpleRuntimeConfig.JournalRoot =
			FPaths::ProjectSavedDir()
			/ TEXT("UeAgentInterface/SimpleAuthoring");
		TSharedPtr<SimpleAuthoring::FUeAgentSimpleAuthoringRuntime>
			SimpleRuntime;
		SimpleAuthoring::FUeAgentSimpleAuthoringRuntimeError
			SimpleRuntimeError;
		if (!SimpleAuthoring::FUeAgentSimpleAuthoringRuntime::TryCreate(
				SimpleRuntimeConfig,
				*ExplicitWorld,
				SharedTicketService.ToSharedRef(),
				Projector.ToSharedRef(),
				SimpleRuntime,
				SimpleRuntimeError)
			|| !SimpleRuntime.IsValid())
		{
			SetError(
				OutError,
				SimpleRuntimeError.Code,
				SimpleRuntimeError.Detail);
			return false;
		}

		TArray<TSharedRef<IUeAgentCatalogSource>> CatalogSources{
			CatalogSource,
			AssetCatalogSource,
			NiagaraCatalogSource,
		};
		CatalogSources.Append(SimpleRuntime->GetCatalogSources());
		if (!FUeAgentDiscoveryService::TryCreate(
				CatalogSources,
				{
					ReadPort,
					AssetReadPort,
					NiagaraReadPort,
				},
				Discovery,
				DiscoveryError))
		{
			SetError(
				OutError,
				DiscoveryError.Code,
				DiscoveryError.Detail);
			return false;
		}

		TSharedPtr<
			const BindingGateway::Query::
				FUeAgentQueryBinding> QueryBinding;
		if (!BindingGateway::Query::
				FUeAgentQueryBinding::TryCreate(
				GetReferenceQueryDescriptorIdentity(),
				ReferenceRegistryHash,
				Discovery.ToSharedRef(),
				Projector.ToSharedRef(),
				SessionSource,
				TicketService.ToSharedRef(),
				QueryBinding,
				QueryError))
		{
			SetError(
				OutError,
				QueryError.Code,
				QueryError.Detail);
			return false;
		}

		TSharedPtr<const FUeAgentDescriptorRegistry>
			ReadExportDescriptors;
		FString ReadExportError;
		if (!ReadExportCommand::
				TryCreateReadExportDescriptorRegistry(
					ReadExportDescriptors,
					ReadExportError)
			|| !ReadExportDescriptors.IsValid())
		{
			SetError(
				OutError,
				ReadExportError,
				TEXT(
					"read/export descriptors could not compose"));
			return false;
		}
		const TSharedRef<
			DomainAdapters::WorldRead::
				FUeAgentWorldReadProductionEnvironment>
			WorldReadEnvironment =
				MakeShared<
					DomainAdapters::WorldRead::
						FUeAgentWorldReadProductionEnvironment>();
		const TSharedRef<
			DomainAdapters::AssetProjectRead::
				FUeAgentAssetProjectReadProductionEnvironment>
			AssetProjectReadEnvironment =
				MakeShared<
					DomainAdapters::AssetProjectRead::
						FUeAgentAssetProjectReadProductionEnvironment>();
		const TSharedRef<
			DomainAdapters::ContentGraphRead::
				FUeAgentContentGraphReadProductionEnvironment>
			ContentGraphReadEnvironment =
				MakeShared<
					DomainAdapters::ContentGraphRead::
						FUeAgentContentGraphReadProductionEnvironment>();
		TArray<FUeAgentDomainQueryProviderRegistration>
			ReadExportProviderRegistrations;
		ReadExportProviderRegistrations.Reserve(46);
		for (const FUeAgentOperationDescriptor& Descriptor
			: ReadExportDescriptors->GetDescriptors())
		{
			if (!Descriptor.GetQueryBinding().IsSet())
			{
				SetError(
					OutError,
					TEXT(
						"read_export_query_binding_missing"),
					Descriptor.GetIdentity().GetDescriptorId());
				return false;
			}
			const FString& ProviderBinding =
				Descriptor.GetQueryBinding()
					->GetProviderBinding();
			if (Descriptor.GetIdentity().GetDescriptorId()
				.StartsWith(TEXT("uai.query.world.")))
			{
				TSharedPtr<
					DomainAdapters::WorldRead::
						IUeAgentWorldReadHook> Hook;
				DomainAdapters::WorldRead::
					FUeAgentWorldReadError Error;
				if (!DomainAdapters::WorldRead::
						FUeAgentWorldReadNativeHookFactory::
							TryCreate(
								ProviderBinding,
								WorldReadEnvironment,
								Hook,
								Error)
					|| !Hook.IsValid())
				{
					SetError(
						OutError,
						Error.Code,
						Error.Detail);
					return false;
				}
				ReadExportProviderRegistrations.Emplace(
					ProviderBinding,
					MakeShared<
						DomainAdapters::WorldRead::
							FUeAgentWorldReadProvider>(
								ProviderBinding,
								Hook.ToSharedRef()));
			}
			else if (Descriptor.GetIdentity().GetDescriptorId()
				.StartsWith(TEXT("uai.query.content_graph.")))
			{
				TSharedPtr<
					DomainAdapters::ContentGraphRead::
						IUeAgentContentGraphReadHook> Hook;
				DomainAdapters::ContentGraphRead::
					FUeAgentContentGraphReadError Error;
				if (!DomainAdapters::ContentGraphRead::
						FUeAgentContentGraphReadNativeHookFactory::
							TryCreate(
								ProviderBinding,
								ContentGraphReadEnvironment,
								Hook,
								Error)
					|| !Hook.IsValid())
				{
					SetError(
						OutError,
						Error.Code,
						Error.Detail);
					return false;
				}
				ReadExportProviderRegistrations.Emplace(
					ProviderBinding,
					MakeShared<
						DomainAdapters::ContentGraphRead::
							FUeAgentContentGraphReadProvider>(
								ProviderBinding,
								Hook.ToSharedRef()));
			}
			else
			{
				TSharedPtr<
					DomainAdapters::AssetProjectRead::
						IUeAgentAssetProjectReadHook> Hook;
				DomainAdapters::AssetProjectRead::
					FUeAgentAssetProjectReadError Error;
				if (!DomainAdapters::AssetProjectRead::
						FUeAgentAssetProjectReadNativeHookFactory::
							TryCreate(
								ProviderBinding,
								AssetProjectReadEnvironment,
								Hook,
								Error)
					|| !Hook.IsValid())
				{
					SetError(
						OutError,
						Error.Code,
						Error.Detail);
					return false;
				}
				ReadExportProviderRegistrations.Emplace(
					ProviderBinding,
					MakeShared<
						DomainAdapters::AssetProjectRead::
							FUeAgentAssetProjectReadProvider>(
								ProviderBinding,
								Hook.ToSharedRef()));
			}
		}
		TSharedPtr<const FUeAgentDomainQueryProviderRegistry>
			ReadExportProviders;
		if (!FUeAgentDomainQueryProviderRegistry::TryCreate(
				MoveTemp(ReadExportProviderRegistrations),
				ReadExportProviders,
				QueryError)
			|| !ReadExportProviders.IsValid())
		{
			SetError(
				OutError,
				QueryError.Code,
				QueryError.Detail);
			return false;
		}
		const TSharedRef<FReferenceSessionSource>
			ReadExportSessionSource =
				MakeShared<FReferenceSessionSource>(
					FUeAgentQuerySessionIdentity(
						Config.ServerSessionId,
						Config.RegistryEpoch,
						ReadExportDescriptors
							->GetRegistryHash()));
		TSharedPtr<
			const BindingGateway::Query::
				FUeAgentQueryBinding>
			ReadExportQueryBinding;
		if (!BindingGateway::Query::
				FUeAgentQueryBinding::TryCreate(
				ReadExportDescriptors.ToSharedRef(),
				ReadExportProviders.ToSharedRef(),
				Discovery.ToSharedRef(),
				Projector.ToSharedRef(),
				ReadExportSessionSource,
				TicketService.ToSharedRef(),
				ReadExportQueryBinding,
				QueryError)
			|| !ReadExportQueryBinding.IsValid())
		{
			SetError(
				OutError,
				QueryError.Code,
				QueryError.Detail);
			return false;
		}

		TimelineSpatial::FUeAgentTimelineSpatialRuntimeConfig
			TimelineSpatialConfig;
		TimelineSpatialConfig.ProjectInstanceId = Config.OwnerId;
		TimelineSpatialConfig.EditorInstanceId = Config.InstanceId;
		TimelineSpatialConfig.ServerSessionId = Config.ServerSessionId;
		TimelineSpatialConfig.RegistryEpoch = Config.RegistryEpoch;
		TimelineSpatialConfig.TimeoutSeconds = Config.QueryTimeoutSeconds;
		TimelineSpatialConfig.MaxFrameBytes = Config.MaxFrameBytes;
		TSharedPtr<TimelineSpatial::FUeAgentTimelineSpatialRuntime>
			TimelineSpatialRuntime;
		TimelineSpatial::FUeAgentTimelineSpatialRuntimeError
			TimelineSpatialError;
		if (!TimelineSpatial::FUeAgentTimelineSpatialRuntime::TryCreate(
				TimelineSpatialConfig,
				*ExplicitWorld,
				SharedTicketService.ToSharedRef(),
				Discovery.ToSharedRef(),
				Projector.ToSharedRef(),
				TimelineSpatialRuntime,
				TimelineSpatialError)
			|| !TimelineSpatialRuntime.IsValid())
		{
			SetError(
				OutError,
				TimelineSpatialError.Code,
				TimelineSpatialError.Detail);
			return false;
		}

		LongRunningSession::FUeAgentLongRunningSessionProductionConfig
			LongRunningConfig;
		LongRunningConfig.ProjectInstanceId = Config.OwnerId;
		LongRunningConfig.EditorInstanceId =
			TEXT("editor-instance:") + Config.InstanceId;
		LongRunningConfig.ServerSessionId = Config.ServerSessionId;
		LongRunningConfig.OwnerLeaseSecret = Config.TicketIntegrityKey;
		LongRunningConfig.TimeoutSeconds = Config.QueryTimeoutSeconds;
		LongRunningConfig.MaxFrameBytes = Config.MaxFrameBytes;
		LongRunningConfig.MaxActiveOperations = 32;
		LongRunningConfig.MaxRetainedOperations = 128;
		LongRunningConfig.EditorWorld = ExplicitWorld;
		TSharedPtr<LongRunningSession::FUeAgentLongRunningSessionRuntime>
			LongRunningRuntime;
		TArray<TSharedRef<CommandEdge::IUeAgentCommandHandler>>
			LongRunningHandlers;
		TArray<FString> LongRunningAuthorityScopes;
		LongRunningSession::FUeAgentLongRunningSessionProductionError
			LongRunningError;
		if (!LongRunningSession::TryCreateLongRunningSessionProduction(
				LongRunningConfig,
				LongRunningRuntime,
				LongRunningHandlers,
				LongRunningAuthorityScopes,
				LongRunningError)
			|| !LongRunningRuntime.IsValid())
		{
			SetError(
				OutError,
				LongRunningError.Code,
				LongRunningError.Detail);
			return false;
		}

		const TArray<GraphCompound::FUeAgentGraphCompoundActivationProfile>
			GraphProfiles = GraphCompound::GetGraphCompoundActivationProfiles();
		TSet<FString> BlueprintMaterialCapabilities;
		for (const GraphCompound::FGraphCompoundDomainRouteProfile& Profile
			: GraphCompound::GetBlueprintMaterialGraphRouteProfiles())
		{
			BlueprintMaterialCapabilities.Add(Profile.GetCapabilityId());
		}
		TSet<FString> PcgAiCapabilities;
		TSet<FString> AnimationCapabilities;
		for (const GraphCompound::FPcgAnimationGraphRouteProfile& Profile
			: GraphCompound::GetPcgAnimationGraphRouteProfiles())
		{
			if (Profile.GetDomainId() == TEXT("pcg_ai_node_graph"))
			{
				PcgAiCapabilities.Add(Profile.GetCapabilityId());
			}
			else if (Profile.GetDomainId() == TEXT("animation_rig_graph"))
			{
				AnimationCapabilities.Add(Profile.GetCapabilityId());
			}
		}
		TSet<FString> AudioMediaCapabilities;
		for (const GraphCompound::FAudioMediaGraphRouteProfile& Profile
			: GraphCompound::GetAudioMediaGraphRouteProfiles())
		{
			AudioMediaCapabilities.Add(Profile.GetCapabilityId());
		}
		TArray<GraphCompound::FUeAgentGraphCompoundActivationProfile>
			BlueprintMaterialProfiles;
		TArray<GraphCompound::FUeAgentGraphCompoundActivationProfile>
			PcgAiProfiles;
		TArray<GraphCompound::FUeAgentGraphCompoundActivationProfile>
			AnimationProfiles;
		TArray<GraphCompound::FUeAgentGraphCompoundActivationProfile>
			AudioMediaProfiles;
		for (const GraphCompound::FUeAgentGraphCompoundActivationProfile& Profile
			: GraphProfiles)
		{
			const FString& CapabilityId = Profile.GetCapabilityId();
			if (BlueprintMaterialCapabilities.Contains(CapabilityId))
			{
				BlueprintMaterialProfiles.Add(Profile);
			}
			else if (PcgAiCapabilities.Contains(CapabilityId))
			{
				PcgAiProfiles.Add(Profile);
			}
			else if (AnimationCapabilities.Contains(CapabilityId))
			{
				AnimationProfiles.Add(Profile);
			}
			else if (AudioMediaCapabilities.Contains(CapabilityId))
			{
				AudioMediaProfiles.Add(Profile);
			}
		}
		if (GraphProfiles.Num() != 77
			|| BlueprintMaterialProfiles.Num() != 19
			|| PcgAiProfiles.Num() != 17
			|| AnimationProfiles.Num() != 26
			|| AudioMediaProfiles.Num() != 15)
		{
			SetError(OutError,
				TEXT("graph_compound_public_profile_invalid"),
				FString::Printf(
					TEXT("all=%d blueprint_material=%d pcg_ai=%d animation=%d audio_media=%d"),
					GraphProfiles.Num(), BlueprintMaterialProfiles.Num(),
					PcgAiProfiles.Num(), AnimationProfiles.Num(),
					AudioMediaProfiles.Num()));
			return false;
		}
		GraphCompound::FGraphCompoundRuntimeConfig GraphRuntimeConfig;
		GraphRuntimeConfig.MaxFrameBytes = Config.MaxFrameBytes;
		GraphCompound::FGraphCompoundBlueprintMaterialProductionConfig
			BlueprintMaterialProductionConfig;
		BlueprintMaterialProductionConfig.OperationTimeoutSeconds =
			Config.QueryTimeoutSeconds;
		TSharedPtr<GraphCompound::FGraphCompoundRuntime>
			BlueprintMaterialGraphRuntime;
		GraphCompound::FGraphCompoundRuntimeError GraphRuntimeError;
		if (!GraphCompound::TryCreateBlueprintMaterialGraphProductionRuntime(
				GraphRuntimeConfig,
				BlueprintMaterialProductionConfig,
				BlueprintMaterialGraphRuntime,
				GraphRuntimeError)
			|| !BlueprintMaterialGraphRuntime.IsValid())
		{
			SetError(OutError,
				GraphRuntimeError.Code.IsEmpty()
					? TEXT("graph_compound_blueprint_material_runtime_invalid")
					: GraphRuntimeError.Code,
				GraphRuntimeError.SubjectId + TEXT(":")
					+ GraphRuntimeError.Detail);
			return false;
		}
		GraphCompound::FGraphCompoundPcgAiProductionConfig
			PcgAiProductionConfig;
		PcgAiProductionConfig.OperationTimeoutSeconds =
			Config.QueryTimeoutSeconds;
		TSharedPtr<GraphCompound::FGraphCompoundRuntime> PcgAiGraphRuntime;
		GraphRuntimeError = {};
		if (!GraphCompound::TryCreatePcgAiGraphProductionRuntime(
				GraphRuntimeConfig,
				PcgAiProductionConfig,
				PcgAiGraphRuntime,
				GraphRuntimeError)
			|| !PcgAiGraphRuntime.IsValid())
		{
			SetError(OutError,
				GraphRuntimeError.Code.IsEmpty()
					? TEXT("graph_compound_pcg_ai_runtime_invalid")
					: GraphRuntimeError.Code,
				GraphRuntimeError.SubjectId + TEXT(":")
					+ GraphRuntimeError.Detail);
			return false;
		}
		GraphCompound::FGraphCompoundAnimationProductionConfig
			GraphProductionConfig;
		GraphProductionConfig.OperationTimeoutSeconds =
			Config.QueryTimeoutSeconds;
		TSharedPtr<GraphCompound::FGraphCompoundRuntime> GraphRuntime;
		GraphRuntimeError = {};
		if (!GraphCompound::TryCreateAnimationGraphProductionRuntime(
				GraphRuntimeConfig,
				GraphProductionConfig,
				GraphRuntime,
				GraphRuntimeError)
			|| !GraphRuntime.IsValid())
		{
			SetError(OutError,
				GraphRuntimeError.Code.IsEmpty()
					? TEXT("graph_compound_public_runtime_invalid")
					: GraphRuntimeError.Code,
				GraphRuntimeError.SubjectId + TEXT(":")
					+ GraphRuntimeError.Detail);
			return false;
		}
		GraphCompound::FGraphCompoundAudioMediaProductionConfig
			AudioMediaProductionConfig;
		AudioMediaProductionConfig.OperationTimeoutSeconds =
			Config.QueryTimeoutSeconds;
		TSharedPtr<GraphCompound::FGraphCompoundRuntime>
			AudioMediaGraphRuntime;
		GraphRuntimeError = {};
		if (!GraphCompound::TryCreateAudioMediaGraphProductionRuntime(
				GraphRuntimeConfig,
				AudioMediaProductionConfig,
				AudioMediaGraphRuntime,
				GraphRuntimeError)
			|| !AudioMediaGraphRuntime.IsValid())
		{
			SetError(OutError,
				GraphRuntimeError.Code.IsEmpty()
					? TEXT("graph_compound_audio_media_runtime_invalid")
					: GraphRuntimeError.Code,
				GraphRuntimeError.SubjectId + TEXT(":")
					+ GraphRuntimeError.Detail);
			return false;
		}
		TUniquePtr<FImpl> NewImpl = MakeUnique<FImpl>();
		NewImpl->OwnerId = Config.OwnerId;
		NewImpl->SimpleAuthoringRuntime = SimpleRuntime;
		NewImpl->TimelineSpatialRuntime = TimelineSpatialRuntime;
		NewImpl->LongRunningSessionRuntime = LongRunningRuntime;
		NewImpl->BlueprintMaterialGraphRuntime = BlueprintMaterialGraphRuntime;
		NewImpl->PcgAiGraphRuntime = PcgAiGraphRuntime;
		NewImpl->AnimationGraphRuntime = GraphRuntime;
		NewImpl->AudioMediaGraphRuntime = AudioMediaGraphRuntime;
		NewImpl->QueryPort =
			MakeShared<
				FReferenceQueryApplicationPort,
				ESPMode::ThreadSafe>(
				QueryBinding.ToSharedRef(),
				Config.QueryTimeoutSeconds,
				Config.MaxFrameBytes);
		ReferenceCrud::FUeAgentReferenceCrudApplicationConfig
			OperationConfig;
		OperationConfig.OperationDescriptor =
			GetReferenceOperationDescriptorIdentity();
		OperationConfig.OperationDescriptorRegistry =
			ReferenceOperationDescriptors;
		OperationConfig.ExecutionContext.ProjectInstanceId =
			Config.OwnerId;
		OperationConfig.ExecutionContext.EditorInstanceId =
			Config.InstanceId;
		OperationConfig.ExecutionContext.StatePlane =
			EUeAgentStatePlane::Persisted;
		OperationConfig.ExecutionContext.ContextRevision =
			Config.ServerSessionId
			+ TEXT(":")
			+ FString::Printf(
				TEXT("%lld"),
				Config.RegistryEpoch)
			+ TEXT(":")
			+ ReferenceOperationDescriptors
				->GetRegistryHash();
		OperationConfig.ExecutionContext.bResolvedFromCurrent =
			false;
		OperationConfig.ServerSessionId =
			Config.ServerSessionId;
		OperationConfig.RegistryEpoch = Config.RegistryEpoch;
		OperationConfig.TimeoutSeconds =
			Config.QueryTimeoutSeconds;
		OperationConfig.MaxFrameBytes = Config.MaxFrameBytes;
		OperationConfig.JournalRoot =
			FPaths::ProjectSavedDir()
			/ TEXT("UeAgentInterface/ReferenceCrud");
		ReferenceCrud::FUeAgentReferenceCrudApplicationError
			OperationError;
		if (!ReferenceCrud::FUeAgentReferenceCrudApplication::
				TryCreate(
					OperationConfig,
					SharedTicketService.ToSharedRef(),
					AssetReadPort,
					NewImpl->OperationPort,
					OperationError))
		{
			SetError(
				OutError,
				OperationError.Code,
				OperationError.Detail);
			return false;
		}
		TSharedPtr<const CommandContract::
			FUeAgentCommandContractRegistry> GraphCommandRegistry;
		FString CommandRegistryError;
		if (!GraphCompound::
				TryCreateGraphCompoundProductionCommandContractRegistry(
					GraphProfiles,
					GraphCommandRegistry,
					CommandRegistryError)
			|| !GraphCommandRegistry.IsValid())
		{
			SetError(
				OutError,
				TEXT("graph_compound_command_registry_invalid"),
				CommandRegistryError);
			return false;
		}
		TSharedPtr<const CommandContract::
			FUeAgentCommandContractRegistry> CommandRegistry;
		if (!DestructiveImport::
				TryExtendDestructiveImportProductionCommandContractRegistry(
					GraphCommandRegistry.ToSharedRef(),
					CommandRegistry,
					CommandRegistryError)
			|| !CommandRegistry.IsValid())
		{
			SetError(
				OutError,
				TEXT("destructive_import_command_registry_invalid"),
				CommandRegistryError);
			return false;
		}
		TArray<
			TSharedRef<
				CommandEdge::IUeAgentCommandHandler>>
			CommandHandlers;
		FString CommandError;
		if (!ReferenceCommand::
				TryCreateReferenceCommandHandlers(
					ReferenceCommand::
						FUeAgentReferenceCommandHandlerConfig(
							Config.ServerSessionId,
							Config.RegistryEpoch,
							ReferenceRegistryHash,
							GetReferenceQueryDescriptorIdentity(),
							NewImpl->QueryPort.ToSharedRef(),
							NewImpl->OperationPort.ToSharedRef()),
					CommandHandlers,
					CommandError))
		{
			SetError(
				OutError,
				CommandError,
				TEXT(
					"reference command handlers could not compose"));
			return false;
		}
		TArray<TSharedRef<CommandEdge::IUeAgentCommandHandler>>
			ReadExportHandlers;
		if (!ReadExportCommand::
				TryCreateReadExportCommandHandlers(
					ReadExportCommand::
						FUeAgentReadExportCommandHandlerConfig(
							Config.ServerSessionId,
							Config.RegistryEpoch,
							Config.QueryTimeoutSeconds,
							Config.MaxFrameBytes,
							ReadExportDescriptors.ToSharedRef(),
							ReadExportQueryBinding.ToSharedRef(),
							ReadExportSessionSource,
							TicketService.ToSharedRef()),
					ReadExportHandlers,
					CommandError))
		{
			SetError(
				OutError,
				CommandError,
				TEXT(
					"read/export command handlers could not compose"));
			return false;
		}
		CommandHandlers.Append(ReadExportHandlers);
		CommandHandlers.Append(SimpleRuntime->GetCommandHandlers());
		CommandHandlers.Append(
			TimelineSpatialRuntime->GetCommandHandlers());
		CommandHandlers.Append(LongRunningHandlers);
		TArray<TSharedRef<CommandEdge::IUeAgentCommandHandler>>
			GraphHandlers;
		auto AppendGraphDomainHandlers = [
			&GraphHandlers,
			&CommandError](
			const TArray<GraphCompound::
				FUeAgentGraphCompoundActivationProfile>& Profiles,
			const TSharedRef<GraphCompound::FGraphCompoundRuntime>& Runtime)
		{
			TArray<TSharedRef<CommandEdge::IUeAgentCommandHandler>>
				DomainHandlers;
			if (!GraphCompound::TryCreateGraphCompoundCommandHandlers(
					Profiles,
					Runtime->GetQueryPort(),
					Runtime->GetOperationPort(),
					DomainHandlers,
					CommandError))
			{
				return false;
			}
			GraphHandlers.Append(DomainHandlers);
			return true;
		};
		if (!AppendGraphDomainHandlers(
				BlueprintMaterialProfiles,
				BlueprintMaterialGraphRuntime.ToSharedRef())
			|| !AppendGraphDomainHandlers(
				PcgAiProfiles,
				PcgAiGraphRuntime.ToSharedRef())
			|| !AppendGraphDomainHandlers(
				AnimationProfiles,
				GraphRuntime.ToSharedRef())
			|| !AppendGraphDomainHandlers(
				AudioMediaProfiles,
				AudioMediaGraphRuntime.ToSharedRef())
			|| GraphHandlers.Num() != GraphProfiles.Num())
		{
			SetError(OutError,
				CommandError.IsEmpty()
					? TEXT("graph_compound_handler_count_invalid")
					: CommandError,
				TEXT("Graph public command handler could not compose"));
			return false;
		}
		CommandHandlers.Append(GraphHandlers);
		TSharedPtr<DomainAdapters::AssetDestructiveImport::
			IUeAgentAssetDeleteOwner> AssetDeleteOwner;
		if (!DomainAdapters::AssetDestructiveImport::TryCreateAssetDeleteOwner(
				FPaths::ProjectSavedDir()
					/ TEXT("UAI/DestructiveImport/AssetDeleteJournal"),
				Config.OwnerId + TEXT(":asset-delete"),
				AssetDeleteOwner,
				CommandError)
			|| !AssetDeleteOwner.IsValid())
		{
			SetError(
				OutError,
				TEXT("asset_delete_owner_invalid"),
				CommandError);
			return false;
		}
		TArray<TSharedRef<CommandEdge::IUeAgentCommandHandler>>
			DestructiveImportHandlers;
		TSharedPtr<DomainAdapters::AssetDestructiveImport::
			IUeAgentAssetOperationsOwner> AssetOperationsOwner;
		if (!DomainAdapters::AssetDestructiveImport::TryCreateAssetOperationsOwner(
				FPaths::ProjectSavedDir()
					/ TEXT("UAI/DestructiveImport/AssetOperationsJournal"),
				Config.OwnerId + TEXT(":asset-operations"),
				AssetOperationsOwner,
				CommandError)
			|| !AssetOperationsOwner.IsValid())
		{
			SetError(OutError, TEXT("asset_operations_owner_invalid"), CommandError);
			return false;
		}
		if (!DestructiveImport::TryCreateDestructiveImportCommandHandlers(
				AssetDeleteOwner.ToSharedRef(),
				AssetOperationsOwner.ToSharedRef(),
				DestructiveImportHandlers,
				CommandError))
		{
			SetError(
				OutError,
				CommandError,
				TEXT("destructive/import command handler could not compose"));
			return false;
		}
		CommandHandlers.Append(DestructiveImportHandlers);
		TSharedPtr<DomainAdapters::WorldDestructive::
			IUeAgentWorldDestructiveOwner> WorldDestructiveOwner;
		if (!DomainAdapters::WorldDestructive::TryCreateWorldDestructiveOwner(
				*ExplicitWorld,
				FPaths::ProjectSavedDir()
					/ TEXT("UAI/DestructiveImport/WorldDestructiveJournal"),
				Config.OwnerId + TEXT(":world-destructive"),
				WorldDestructiveOwner,
				CommandError)
			|| !WorldDestructiveOwner.IsValid())
		{
			SetError(
				OutError,
				TEXT("world_destructive_owner_invalid"),
				CommandError);
			return false;
		}
		TArray<TSharedRef<CommandEdge::IUeAgentCommandHandler>>
			WorldDestructiveHandlers;
		if (!DestructiveImport::TryCreateDestructiveImportWorldCommandHandlers(
				WorldDestructiveOwner.ToSharedRef(),
				WorldDestructiveHandlers,
				CommandError))
		{
			SetError(
				OutError,
				CommandError,
				TEXT("world destructive command handler could not compose"));
			return false;
		}
		CommandHandlers.Append(WorldDestructiveHandlers);
		TSharedPtr<DomainAdapters::ExternalDataImport::
			IUeAgentExternalDataImportOwner> ExternalDataImportOwner;
		if (!DomainAdapters::ExternalDataImport::
				TryCreateExternalDataImportOwner(
					FPaths::ProjectSavedDir()
						/ TEXT("UAI/DestructiveImport/ExternalDataImportJournal"),
					Config.OwnerId + TEXT(":external-data-import"),
					ExternalDataImportOwner,
					CommandError)
			|| !ExternalDataImportOwner.IsValid())
		{
			SetError(
				OutError,
				TEXT("external_data_import_owner_invalid"),
				CommandError);
			return false;
		}
		TArray<TSharedRef<CommandEdge::IUeAgentCommandHandler>>
			ExternalDataImportHandlers;
		if (!DestructiveImport::
				TryCreateDestructiveImportExternalCommandHandlers(
					ExternalDataImportOwner.ToSharedRef(),
					ExternalDataImportHandlers,
					CommandError))
		{
			SetError(
				OutError,
				CommandError,
				TEXT("external data import command handler could not compose"));
			return false;
		}
		CommandHandlers.Append(ExternalDataImportHandlers);
		TSharedPtr<CommandEdge::IUeAgentCommandHandler> HjsonHandler;
		TimelineSpatial::FUeAgentTimelineSpatialHjsonCommandHandlerConfig
			HjsonConfig;
		HjsonConfig.Operation.ProjectInstanceId = Config.OwnerId;
		HjsonConfig.Operation.EditorInstanceId = Config.InstanceId;
		HjsonConfig.Operation.ServerSessionId = Config.ServerSessionId;
		HjsonConfig.Operation.RegistryEpoch = Config.RegistryEpoch;
		HjsonConfig.TimeoutSeconds = Config.QueryTimeoutSeconds;
		HjsonConfig.MaxFrameBytes = Config.MaxFrameBytes;
		if (!TimelineSpatial::
				TryCreateTimelineSpatialHjsonCommandHandler(
					HjsonConfig,
					SharedTicketService.ToSharedRef(),
					Projector.ToSharedRef(),
					HjsonHandler,
					CommandError)
			|| !HjsonHandler.IsValid())
		{
			SetError(
				OutError,
				CommandError,
				TEXT(
					"HJSON public command handler could not compose"));
			return false;
		}
		CommandHandlers.Add(HjsonHandler.ToSharedRef());
		FUeAgentCommandServiceConfig CommandConfig;
		CommandConfig.PrincipalSubject =
			TEXT("authenticated-uai-agent");
		CommandConfig.AuthorityScopes = {
			TEXT("uai:query"),
			TEXT("uai:export"),
			TEXT("uai:reference:write"),
			TEXT("uai:asset:delete"),
			TEXT("uai:asset:write"),
			TEXT("uai:asset:reimport"),
			TEXT("uai:skeletal_mesh:write"),
		};
		CommandConfig.AuthorityScopes.Append(
			SimpleRuntime->GetAuthorityScopes());
		CommandConfig.AuthorityScopes.Append(
			TimelineSpatialRuntime->GetAuthorityScopes());
		CommandConfig.AuthorityScopes.Append(LongRunningAuthorityScopes);
		CommandConfig.AuthorityScopes.Add(TEXT("animation_rig_graph.read"));
		CommandConfig.AuthorityScopes.Add(TEXT("animation_rig_graph.write"));
		CommandConfig.AuthorityScopes.Add(
			TEXT("uai:authoring:hjson:import"));
		if (!FUeAgentCommandServiceComposition::TryCreate(
				CommandConfig,
				CommandRegistry.ToSharedRef(),
				MoveTemp(CommandHandlers),
				NewImpl->CommandService,
				CommandError))
		{
			SetError(
				OutError,
				CommandError,
				TEXT(
					"public command service could not compose"));
			return false;
		}
		FUeAgentHttpAdapterConfig HttpConfig;
		HttpConfig.OwnerId = Config.OwnerId;
		HttpConfig.InstanceId =
			Config.InstanceId + TEXT(":command");
		HttpConfig.Method = TEXT("POST");
		HttpConfig.RoutePath = GetUaiHttpCommandRoutePath();
		HttpConfig.AuthHeader =
			TEXT("X-UeAgentInterface-Token");
		HttpConfig.AuthToken = Config.AuthToken;
		HttpConfig.Channel = TEXT("uai.command");
		HttpConfig.PrincipalSubject =
			CommandConfig.PrincipalSubject;
		HttpConfig.MaxFrameBytes = Config.MaxFrameBytes;
		NewImpl->CommandTransport =
			MakeUnique<FUeAgentHttpTransportAdapter>(
				MoveTemp(HttpConfig),
				MoveTemp(RouteHost),
				NewImpl->CommandService->GetIngressHandler());
		OutComposition =
			TUniquePtr<FUeAgentQueryServiceComposition>(
				new FUeAgentQueryServiceComposition(
					MoveTemp(NewImpl)));
		return true;
	}

	bool FUeAgentQueryServiceComposition::Start(
		FUeAgentTransportDiagnostic& OutDiagnostic)
	{
		if (!Impl || !Impl->CommandService || !Impl->CommandTransport)
		{
			OutDiagnostic = {
				TEXT("query_service_composition_invalid"),
				TEXT("public command service is unavailable"),
			};
			return false;
		}
		FUeAgentTransportServerIdentity Identity;
		if (!Impl->CommandTransport->Start(Identity, OutDiagnostic))
		{
			return false;
		}
		Impl->bRunning = true;
		return true;
	}

	bool FUeAgentQueryServiceComposition::Stop(
		FUeAgentTransportDiagnostic& OutDiagnostic)
	{
		if (!Impl || !Impl->CommandService || !Impl->CommandTransport)
		{
			OutDiagnostic = {
				TEXT("query_service_composition_invalid"),
				TEXT("public command service is unavailable"),
			};
			return false;
		}
		if (!Impl->bRunning)
		{
			OutDiagnostic = {};
			return true;
		}
		if (!Impl->CommandTransport->Stop(
				Impl->OwnerId,
				OutDiagnostic))
		{
			return false;
		}
		Impl->bRunning = false;
		return true;
	}

	bool FUeAgentQueryServiceComposition::IsRunning() const
	{
		return Impl
			&& Impl->bRunning
			&& Impl->CommandService
			&& Impl->CommandTransport
			&& Impl->CommandTransport->Health().bRunning;
	}
}

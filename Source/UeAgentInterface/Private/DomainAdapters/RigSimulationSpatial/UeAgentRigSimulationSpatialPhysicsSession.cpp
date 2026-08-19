#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialPhysicsSession.h"

#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "Misc/ScopeLock.h"
#include "PreviewScene.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::DomainAdapters::RigSimulationSpatial
{
	using namespace ContextResource;
	using namespace StateSemantic;

	namespace
	{
		bool Fail(
			FUeAgentRigSimulationSpatialExecutionError& OutError,
			const FString& Code,
			const FString& Detail = FString())
		{
			OutError = {};
			OutError.Code = Code;
			OutError.SubjectId = TEXT("physics_deformer.production_session");
			OutError.Detail = Detail;
			return false;
		}

		UPrimitiveComponent* ResolveSubject(const FString& SubjectRef)
		{
			if (SubjectRef.IsEmpty() || SubjectRef.Contains(TEXT("://")))
			{
				return nullptr;
			}
			return FindObject<UPrimitiveComponent>(nullptr, *SubjectRef);
		}

		FUeAgentTemporalPoint SampleTime(
			const FUeAgentRigSimulationSpatialPhysicsProbeSessionRequest& Request)
		{
			if (!Request.Velocities.IsEmpty())
			{
				return Request.Velocities.Last().Time;
			}
			if (!Request.Impulses.IsEmpty())
			{
				return Request.Impulses.Last().Time;
			}
			if (Request.SimulationRange.GetUpperBound().GetPoint().IsSet())
			{
				return Request.SimulationRange.GetUpperBound().GetPoint().GetValue();
			}
			if (Request.SimulationRange.GetLowerBound().GetPoint().IsSet())
			{
				return Request.SimulationRange.GetLowerBound().GetPoint().GetValue();
			}
			return FUeAgentTemporalPoint(
				Request.SimulationRange.GetClockId(), TEXT("0"),
				TEXT("60"), TEXT("1"), Request.SimulationRange.GetState());
		}

		float SimulationSeconds(const FUeAgentTemporalRange& Range)
		{
			const TOptional<FUeAgentTemporalPoint>& Lower =
				Range.GetLowerBound().GetPoint();
			const TOptional<FUeAgentTemporalPoint>& Upper =
				Range.GetUpperBound().GetPoint();
			double LowerTick = 0.0;
			double UpperTick = 1.0;
			double Numerator = 60.0;
			double Denominator = 1.0;
			if (!Lower.IsSet() || !Upper.IsSet()
				|| !LexTryParseString(LowerTick, *Lower->GetTick())
				|| !LexTryParseString(UpperTick, *Upper->GetTick())
				|| !LexTryParseString(Numerator, *Upper->GetRateNumerator())
				|| !LexTryParseString(Denominator, *Upper->GetRateDenominator())
				|| Numerator <= 0.0 || Denominator <= 0.0)
			{
				return 1.0f / 60.0f;
			}
			return FMath::Clamp(
				static_cast<float>((UpperTick - LowerTick) * Denominator / Numerator),
				1.0f / 240.0f,
				1.0f);
		}

		class FProductionPhysicsSessionPort final
			: public IUeAgentRigSimulationSpatialPhysicsSessionPort
		{
		public:
			bool TryReadPhysicsProbe(
				const FRigSimulationSpatialSimulationSession& Session,
				const TArray<FString>& SubjectRefs,
				const FUeAgentOperationContext&,
				FUeAgentRigSimulationSpatialPhysicsProbeSessionResult& OutResult,
				FUeAgentRigSimulationSpatialExecutionError& OutError) override
			{
				OutResult = {};
				OutError = {};
				if (!Session.IsValid() || SubjectRefs.IsEmpty())
				{
					return Fail(OutError,
						TEXT("rig_simulation_spatial_physics_session_read_invalid"));
				}
				{
					FScopeLock Lock(&Mutex);
					if (const FUeAgentRigSimulationSpatialPhysicsProbeSessionResult* Found =
						Results.Find(Session.SessionId))
					{
						OutResult = *Found;
						return OutResult.IsValid();
					}
				}
				UPrimitiveComponent* const Owner = ResolveSubject(SubjectRefs[0]);
				if (!Owner)
				{
					return Fail(OutError,
						TEXT("rig_simulation_spatial_physics_subject_unavailable"),
						SubjectRefs[0]);
				}
				OutResult = FUeAgentRigSimulationSpatialPhysicsProbeSessionResult(
					Session,
					false,
					true,
					{},
					TEXT("physics-session-baseline:") + Session.SessionId,
					Owner);
				return OutResult.IsValid();
			}

			bool TryRunPhysicsProbe(
				const FUeAgentRigSimulationSpatialPhysicsProbeSessionRequest& Request,
				const FUeAgentOperationContext&,
				FUeAgentRigSimulationSpatialPhysicsProbeSessionResult& OutResult,
				FUeAgentRigSimulationSpatialExecutionError& OutError) override
			{
				OutResult = {};
				OutError = {};
				if (!Request.IsValid())
				{
					return Fail(OutError,
						TEXT("rig_simulation_spatial_physics_session_request_invalid"));
				}

				FPreviewScene PreviewScene(
					FPreviewScene::ConstructionValues()
						.SetCreateDefaultLighting(false)
						.SetCreatePhysicsScene(true)
						.SetForceMipsResident(false)
						.ShouldSimulatePhysics(true)
						.SetTransactional(false));
				UWorld* const PreviewWorld = PreviewScene.GetWorld();
				if (!PreviewWorld)
				{
					return Fail(OutError,
						TEXT("rig_simulation_spatial_physics_preview_world_unavailable"));
				}

				TMap<FString, UPrimitiveComponent*> Clones;
				TMap<FString, FTransform> InitialTransforms;
				for (const FUeAgentRigSimulationSpatialPhysicsProbeSubject& Subject
					: Request.Subjects)
				{
					UPrimitiveComponent* const Source = Subject.Component.Get();
					if (!Source)
					{
						return Fail(OutError,
							TEXT("rig_simulation_spatial_physics_subject_unavailable"),
							Subject.SubjectRef);
					}
					UPrimitiveComponent* const Clone = NewObject<UPrimitiveComponent>(
						GetTransientPackage(), Source->GetClass(), NAME_None,
						RF_Transient, Source);
					if (!Clone)
					{
						return Fail(OutError,
							TEXT("rig_simulation_spatial_physics_subject_clone_failed"),
							Subject.SubjectRef);
					}
					Clone->SetMobility(EComponentMobility::Movable);
					Clone->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
					PreviewScene.AddComponent(Clone, Source->GetComponentTransform());
					Clone->SetSimulatePhysics(true);
					if (!Clone->IsRegistered() || !Clone->IsSimulatingPhysics())
					{
						return Fail(OutError,
							TEXT("rig_simulation_spatial_physics_subject_not_simulatable"),
							Subject.SubjectRef);
					}
					Clones.Add(Subject.SubjectRef, Clone);
					InitialTransforms.Add(
						Subject.SubjectRef, Clone->GetComponentTransform());
				}

				for (const FUeAgentRigSimulationSpatialPhysicsProbeVector& Velocity
					: Request.Velocities)
				{
					if (UPrimitiveComponent* const* Clone = Clones.Find(Velocity.SubjectRef))
					{
						(*Clone)->SetPhysicsLinearVelocity(Velocity.Value);
					}
				}
				for (const FUeAgentRigSimulationSpatialPhysicsProbeVector& Impulse
					: Request.Impulses)
				{
					if (UPrimitiveComponent* const* Clone = Clones.Find(Impulse.SubjectRef))
					{
						(*Clone)->AddImpulse(Impulse.Value);
					}
				}
				PreviewWorld->Tick(
					LEVELTICK_All, SimulationSeconds(Request.SimulationRange));

				const FUeAgentTemporalPoint ObservationTime = SampleTime(Request);
				TArray<FUeAgentRigSimulationSpatialPhysicsProbeSample> Samples;
				bool bRestored = true;
				for (const FUeAgentRigSimulationSpatialPhysicsProbeSubject& Subject
					: Request.Subjects)
				{
					UPrimitiveComponent* const Clone = Clones.FindRef(Subject.SubjectRef);
					if (!Clone)
					{
						return Fail(OutError,
							TEXT("rig_simulation_spatial_physics_subject_clone_missing"),
							Subject.SubjectRef);
					}
					FUeAgentRigSimulationSpatialPhysicsProbeSample Sample;
					Sample.SubjectRef = Subject.SubjectRef;
					Sample.Time = ObservationTime;
					Sample.FrameId = Subject.NativeSimulationFrameId;
					Sample.Position = Clone->GetComponentLocation();
					Sample.Velocity = Clone->GetPhysicsLinearVelocity();
					Sample.EvidenceId = TEXT("physics-preview-sample:")
						+ Request.Session.SessionId + TEXT(":") + Subject.SubjectRef;
					Samples.Add(MoveTemp(Sample));

					Clone->SetPhysicsLinearVelocity(FVector::ZeroVector);
					Clone->SetPhysicsAngularVelocityInRadians(FVector::ZeroVector);
					Clone->SetWorldTransform(
						InitialTransforms.FindChecked(Subject.SubjectRef),
						false, nullptr, ETeleportType::TeleportPhysics);
					bRestored = bRestored
						&& Clone->GetComponentTransform().Equals(
							InitialTransforms.FindChecked(Subject.SubjectRef), 0.001)
						&& Clone->GetPhysicsLinearVelocity().IsNearlyZero(0.001);
				}

				OutResult = FUeAgentRigSimulationSpatialPhysicsProbeSessionResult(
					Request.Session,
					true,
					bRestored,
					MoveTemp(Samples),
					TEXT("physics-preview-restored-and-released:")
						+ Request.Session.SessionId,
					Request.Subjects[0].Component.Get());
				if (!OutResult.IsValid())
				{
					return Fail(OutError,
						TEXT("rig_simulation_spatial_physics_session_result_invalid"));
				}
				{
					FScopeLock Lock(&Mutex);
					if (!Results.Contains(Request.Session.SessionId)
						&& ResultOrder.Num() >= MaxRetainedResults)
					{
						Results.Remove(ResultOrder[0]);
						ResultOrder.RemoveAt(0);
					}
					Results.Add(Request.Session.SessionId, OutResult);
					ResultOrder.AddUnique(Request.Session.SessionId);
				}
				return true;
			}

		private:
			static constexpr int32 MaxRetainedResults = 128;
			FCriticalSection Mutex;
			TMap<FString, FUeAgentRigSimulationSpatialPhysicsProbeSessionResult>
				Results;
			TArray<FString> ResultOrder;
		};
	}

	bool TryCreateProductionPhysicsSessionPort(
		TSharedPtr<IUeAgentRigSimulationSpatialPhysicsSessionPort>& OutPort,
		FUeAgentRigSimulationSpatialExecutionError& OutError)
	{
		OutError = {};
		OutPort = MakeShared<FProductionPhysicsSessionPort>();
		return OutPort.IsValid();
	}
}

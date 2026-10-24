#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialPhysicsSession.h"

#include "Components/PrimitiveComponent.h"
#include "Dom/JsonValue.h"
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
		using Contract::FDiagnostic;

		bool SessionFailure(FDiagnostic& OutError, const FString& Code, const FString& Reason,
			const FString& Path, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const FString& Target)
		{
			OutError = FDiagnostic::Failure(Code, Reason, TEXT("native_execution"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = Path;
			OutError.Source.JsonPath = Path;
			OutError.TargetPath = Target;
			OutError.ActualValuePresent = Actual.IsValid();
			OutError.ActualValue = MoveTemp(Actual);
			OutError.ExpectedValuePresent = Expected.IsValid();
			OutError.ExpectedValue = MoveTemp(Expected);
			return false;
		}

		TSharedRef<FJsonValue> NumberFact(double Value)
		{
			if (FMath::IsFinite(Value)) return MakeShared<FJsonValueNumber>(Value);
			return MakeShared<FJsonValueString>(FString::Printf(TEXT("%.17g"), Value));
		}

		TSharedRef<FJsonValue> VectorFact(const FVector& Value)
		{
			return MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
				NumberFact(Value.X), NumberFact(Value.Y), NumberFact(Value.Z)});
		}

		TSharedRef<FJsonValue> RotationFact(const FQuat& Value)
		{
			return MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
				NumberFact(Value.X), NumberFact(Value.Y), NumberFact(Value.Z), NumberFact(Value.W)});
		}

		bool CheckRestoredTransform(const FTransform& Actual, const FTransform& Expected,
			const FString& Path, const FString& Target, FDiagnostic& OutError)
		{
			// Match FTransform::Equals, including quaternion sign equivalence and comparison order.
			if (!Actual.TranslationEquals(Expected, 0.001))
				return SessionFailure(OutError, TEXT("rig_simulation_spatial_physics_restoration_mismatch"),
					TEXT("The restored translation differs from its initial value beyond tolerance 0.001."),
					Path + TEXT("/translation"), VectorFact(Actual.GetTranslation()), VectorFact(Expected.GetTranslation()), Target);
			if (!Actual.RotationEquals(Expected, 0.001))
				return SessionFailure(OutError, TEXT("rig_simulation_spatial_physics_restoration_mismatch"),
					TEXT("The restored quaternion differs from both signs of the initial quaternion beyond tolerance 0.001."),
					Path + TEXT("/rotation"), RotationFact(Actual.GetRotation()), RotationFact(Expected.GetRotation()), Target);
			if (!Actual.Scale3DEquals(Expected, 0.001))
				return SessionFailure(OutError, TEXT("rig_simulation_spatial_physics_restoration_mismatch"),
					TEXT("The restored scale differs from its initial value beyond tolerance 0.001."),
					Path + TEXT("/scale"), VectorFact(Actual.GetScale3D()), VectorFact(Expected.GetScale3D()), Target);
			return true;
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
				if (!Session.TryValidate(OutError))
				{
					OutError.SourceKind = TEXT("native_result");
					OutError.PropertyPath = TEXT("/simulation_session") + OutError.PropertyPath;
					OutError.Source.JsonPath = OutError.PropertyPath;
					return false;
				}
				if (SubjectRefs.IsEmpty())
					return SessionFailure(OutError, TEXT("rig_simulation_spatial_physics_session_read_invalid"),
						TEXT("Reading a physics probe requires at least one subject reference."), TEXT("/subject_refs"),
						MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("one or more subject references")), Session.OwnerRef);
				{
					FScopeLock Lock(&Mutex);
					if (const FUeAgentRigSimulationSpatialPhysicsProbeSessionResult* Found =
						Results.Find(Session.SessionId))
					{
						OutResult = *Found;
						return OutResult.TryValidate(OutError);
					}
				}
				UPrimitiveComponent* const Owner = ResolveSubject(SubjectRefs[0]);
				if (!Owner)
				{
					return SessionFailure(OutError, TEXT("rig_simulation_spatial_physics_subject_unavailable"),
						TEXT("The first probe subject reference does not resolve to an available primitive component."), TEXT("/subject_refs/0"),
						MakeShared<FJsonValueString>(SubjectRefs[0]), MakeShared<FJsonValueString>(TEXT("an available primitive component reference")), SubjectRefs[0]);
				}
				OutResult = FUeAgentRigSimulationSpatialPhysicsProbeSessionResult(
					Session,
					false,
					true,
					{},
					TEXT("physics-session-baseline:") + Session.SessionId,
					Owner);
				return OutResult.TryValidate(OutError);
			}

			bool TryRunPhysicsProbe(
				const FUeAgentRigSimulationSpatialPhysicsProbeSessionRequest& Request,
				const FUeAgentOperationContext&,
				FUeAgentRigSimulationSpatialPhysicsProbeSessionResult& OutResult,
				FUeAgentRigSimulationSpatialExecutionError& OutError) override
			{
				OutResult = {};
				OutError = {};
				if (!Request.TryValidate(OutError)) return false;

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
					return SessionFailure(OutError, TEXT("rig_simulation_spatial_physics_preview_world_unavailable"),
						TEXT("The physics preview scene did not create a world."), TEXT("/preview_world"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Request.Session.OwnerRef);
				}

				TMap<FString, UPrimitiveComponent*> Clones;
				TMap<FString, FTransform> InitialTransforms;
				for (int32 SubjectIndex = 0; SubjectIndex < Request.Subjects.Num(); ++SubjectIndex)
				{
					const auto& Subject = Request.Subjects[SubjectIndex];
					const FString SubjectPath = TEXT("/subjects/") + FString::FromInt(SubjectIndex);
					UPrimitiveComponent* const Source = Subject.Component.Get();
					if (!Source)
					{
						return SessionFailure(OutError, TEXT("rig_simulation_spatial_physics_subject_unavailable"),
							TEXT("The physics probe subject component became unavailable."), SubjectPath + TEXT("/component"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Subject.SubjectRef);
					}
					UPrimitiveComponent* const Clone = NewObject<UPrimitiveComponent>(
						GetTransientPackage(), Source->GetClass(), NAME_None,
						RF_Transient, Source);
					if (!Clone)
					{
						return SessionFailure(OutError, TEXT("rig_simulation_spatial_physics_subject_clone_failed"),
							TEXT("Creating the physics preview component failed."), SubjectPath + TEXT("/preview_component"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Subject.SubjectRef);
					}
					Clone->SetMobility(EComponentMobility::Movable);
					Clone->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
					PreviewScene.AddComponent(Clone, Source->GetComponentTransform());
					Clone->SetSimulatePhysics(true);
					if (!Clone->IsRegistered())
						return SessionFailure(OutError, TEXT("rig_simulation_spatial_physics_subject_not_simulatable"),
							TEXT("The physics preview component is not registered."), SubjectPath + TEXT("/registered"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Subject.SubjectRef);
					if (!Clone->IsSimulatingPhysics())
						return SessionFailure(OutError, TEXT("rig_simulation_spatial_physics_subject_not_simulatable"),
							TEXT("The physics preview component did not start simulating."), SubjectPath + TEXT("/simulating"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Subject.SubjectRef);
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
				FDiagnostic RestorationError;
				for (int32 SubjectIndex = 0; SubjectIndex < Request.Subjects.Num(); ++SubjectIndex)
				{
					const auto& Subject = Request.Subjects[SubjectIndex];
					const FString SubjectPath = TEXT("/subjects/") + FString::FromInt(SubjectIndex);
					UPrimitiveComponent* const Clone = Clones.FindRef(Subject.SubjectRef);
					if (!Clone)
					{
						return SessionFailure(OutError, TEXT("rig_simulation_spatial_physics_subject_clone_missing"),
							TEXT("The physics preview component is missing after simulation."), SubjectPath + TEXT("/preview_component"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Subject.SubjectRef);
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
					if (bRestored)
					{
						bRestored = CheckRestoredTransform(Clone->GetComponentTransform(),
							InitialTransforms.FindChecked(Subject.SubjectRef), SubjectPath + TEXT("/restoration/transform"),
							Subject.SubjectRef, RestorationError);
						if (bRestored)
						{
							const FVector Velocity = Clone->GetPhysicsLinearVelocity();
							if (!Velocity.IsNearlyZero(0.001))
								bRestored = SessionFailure(RestorationError, TEXT("rig_simulation_spatial_physics_restoration_mismatch"),
									TEXT("The restored linear velocity is not zero within tolerance 0.001."),
									SubjectPath + TEXT("/restoration/linear_velocity"), VectorFact(Velocity), VectorFact(FVector::ZeroVector), Subject.SubjectRef);
						}
					}
				}

				OutResult = FUeAgentRigSimulationSpatialPhysicsProbeSessionResult(
					Request.Session,
					true,
					bRestored,
					MoveTemp(Samples),
					TEXT("physics-preview-restored-and-released:")
						+ Request.Session.SessionId,
					Request.Subjects[0].Component.Get());
				OutResult.RestorationDiagnostic = MoveTemp(RestorationError);
				if (!OutResult.TryValidate(OutError)) return false;
				{
					FScopeLock Lock(&Mutex);
					Results.Add(Request.Session.SessionId, OutResult);
				}
				return true;
			}

		private:
			FCriticalSection Mutex;
			TMap<FString, FUeAgentRigSimulationSpatialPhysicsProbeSessionResult>
				Results;
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

#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryEffectCarrierRevision.h"

#include "Components/DynamicMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMeshActor.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "Misc/SecureHash.h"
#include "PhysicsEngine/BodySetup.h"
#include "Serialization/MemoryWriter.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace UE::UAI::DomainAdapters::ModelingGeometryEffect
{
	using namespace ExecutionMutation;
	using namespace StateSemantic;
	using UE::Geometry::FDynamicMesh3;

	namespace
	{
		class FCarrierRevisionHash final
		{
		public:
			FCarrierRevisionHash()
			{
				bValid = SHA256_Init(&Context) == 1;
			}

			void AddBytes(const void* Data, const uint64 Size)
			{
				if (bValid && Size > 0)
				{
					bValid = SHA256_Update(
						&Context,
						Data,
						static_cast<size_t>(Size)) == 1;
				}
			}

			void AddUInt8(const uint8 Value)
			{
				AddBytes(&Value, sizeof(Value));
			}

			void AddUInt32(const uint32 Value)
			{
				const uint8 Bytes[] = {
					static_cast<uint8>((Value >> 24) & 0xff),
					static_cast<uint8>((Value >> 16) & 0xff),
					static_cast<uint8>((Value >> 8) & 0xff),
					static_cast<uint8>(Value & 0xff),
				};
				AddBytes(Bytes, sizeof(Bytes));
			}

			void AddUInt64(const uint64 Value)
			{
				uint8 Bytes[8];
				for (int32 Index = 0; Index < 8; ++Index)
				{
					Bytes[Index] = static_cast<uint8>(
						(Value >> (56 - Index * 8)) & 0xff);
				}
				AddBytes(Bytes, sizeof(Bytes));
			}

			void AddInt32(const int32 Value)
			{
				AddUInt32(static_cast<uint32>(Value));
			}

			void AddString(const FString& Value)
			{
				const FTCHARToUTF8 Utf8(*Value);
				AddUInt64(static_cast<uint64>(Utf8.Length()));
				AddBytes(Utf8.Get(), static_cast<uint64>(Utf8.Length()));
			}

			void AddName(const FName& Value)
			{
				AddString(Value.ToString());
			}

			void AddFloat(const float Value)
			{
				const float Normalized = Value == 0.0f ? 0.0f : Value;
				uint32 Bits = 0;
				FMemory::Memcpy(&Bits, &Normalized, sizeof(Bits));
				AddUInt32(Bits);
			}

			void AddDouble(const double Value)
			{
				const double Normalized = Value == 0.0 ? 0.0 : Value;
				uint64 Bits = 0;
				FMemory::Memcpy(&Bits, &Normalized, sizeof(Bits));
				AddUInt64(Bits);
			}

			void AddVector(const FVector& Value)
			{
				AddDouble(Value.X);
				AddDouble(Value.Y);
				AddDouble(Value.Z);
			}

			void AddRotator(const FRotator& Value)
			{
				AddDouble(Value.Pitch);
				AddDouble(Value.Yaw);
				AddDouble(Value.Roll);
			}

			void AddTransform(const FTransform& Value)
			{
				AddVector(Value.GetTranslation());
				const FQuat Rotation = Value.GetRotation();
				AddDouble(Rotation.X);
				AddDouble(Rotation.Y);
				AddDouble(Rotation.Z);
				AddDouble(Rotation.W);
				AddVector(Value.GetScale3D());
			}

			bool Finalize(int64& OutSequence)
			{
				OutSequence = -1;
				uint8 Digest[SHA256_DIGEST_LENGTH] = {};
				if (!bValid || SHA256_Final(Digest, &Context) != 1)
				{
					return false;
				}
				uint64 Prefix = 0;
				for (int32 Index = 0; Index < 8; ++Index)
				{
					Prefix = (Prefix << 8) | Digest[Index];
				}
				constexpr uint64 ExactJsonMask = (1ULL << 53) - 1ULL;
				OutSequence = static_cast<int64>(Prefix & ExactJsonMask);
				return true;
			}

		private:
			SHA256_CTX Context{};
			bool bValid = false;
		};

		void HashMaterial(
			FCarrierRevisionHash& Hash,
			const UMaterialInterface* Material)
		{
			Hash.AddString(
				Material ? Material->GetPathName() : FString());
		}

		void HashShapeBase(
			FCarrierRevisionHash& Hash,
			const FKShapeElem& Shape)
		{
			Hash.AddName(Shape.GetName());
			Hash.AddInt32(static_cast<int32>(Shape.GetShapeType()));
			Hash.AddUInt8(Shape.GetContributeToMass() ? 1 : 0);
			Hash.AddInt32(static_cast<int32>(Shape.GetCollisionEnabled()));
			Hash.AddFloat(Shape.RestOffset);
		}

		void HashBodySetup(
			FCarrierRevisionHash& Hash,
			const UBodySetup* BodySetup)
		{
			Hash.AddUInt8(BodySetup ? 1 : 0);
			if (!BodySetup)
			{
				return;
			}
			Hash.AddInt32(
				static_cast<int32>(BodySetup->GetCollisionTraceFlag()));
			Hash.AddUInt8(BodySetup->bDoubleSidedGeometry ? 1 : 0);
			Hash.AddUInt8(BodySetup->bMeshCollideAll ? 1 : 0);
			Hash.AddUInt8(
				BodySetup->bNeverNeedsCookedCollisionData ? 1 : 0);
			Hash.AddVector(BodySetup->BuildScale3D);

			const FKAggregateGeom& Geometry = BodySetup->AggGeom;
			Hash.AddInt32(Geometry.BoxElems.Num());
			for (const FKBoxElem& Shape : Geometry.BoxElems)
			{
				HashShapeBase(Hash, Shape);
				Hash.AddVector(Shape.Center);
				Hash.AddRotator(Shape.Rotation);
				Hash.AddFloat(Shape.X);
				Hash.AddFloat(Shape.Y);
				Hash.AddFloat(Shape.Z);
			}
			Hash.AddInt32(Geometry.SphereElems.Num());
			for (const FKSphereElem& Shape : Geometry.SphereElems)
			{
				HashShapeBase(Hash, Shape);
				Hash.AddVector(Shape.Center);
				Hash.AddFloat(Shape.Radius);
			}
			Hash.AddInt32(Geometry.SphylElems.Num());
			for (const FKSphylElem& Shape : Geometry.SphylElems)
			{
				HashShapeBase(Hash, Shape);
				Hash.AddVector(Shape.Center);
				Hash.AddRotator(Shape.Rotation);
				Hash.AddFloat(Shape.Radius);
				Hash.AddFloat(Shape.Length);
			}
			Hash.AddInt32(Geometry.TaperedCapsuleElems.Num());
			for (const FKTaperedCapsuleElem& Shape
				: Geometry.TaperedCapsuleElems)
			{
				HashShapeBase(Hash, Shape);
				Hash.AddVector(Shape.Center);
				Hash.AddRotator(Shape.Rotation);
				Hash.AddFloat(Shape.Radius0);
				Hash.AddFloat(Shape.Radius1);
				Hash.AddFloat(Shape.Length);
				Hash.AddUInt8(Shape.bOneSidedCollision ? 1 : 0);
			}
			Hash.AddInt32(Geometry.ConvexElems.Num());
			for (const FKConvexElem& Shape : Geometry.ConvexElems)
			{
				HashShapeBase(Hash, Shape);
				Hash.AddTransform(Shape.GetTransform());
				Hash.AddInt32(Shape.VertexData.Num());
				for (const FVector& Vertex : Shape.VertexData)
				{
					Hash.AddVector(Vertex);
				}
				Hash.AddInt32(Shape.IndexData.Num());
				for (const int32 Index : Shape.IndexData)
				{
					Hash.AddInt32(Index);
				}
			}
			Hash.AddInt32(Geometry.LevelSetElems.Num());
			Hash.AddInt32(Geometry.SkinnedLevelSetElems.Num());
			Hash.AddInt32(Geometry.MLLevelSetElems.Num());
			Hash.AddInt32(Geometry.SkinnedTriangleMeshElems.Num());
		}

		bool HashDynamicMesh(
			FCarrierRevisionHash& Hash,
			const FDynamicMesh3& Mesh)
		{
			FDynamicMesh3 Copy(Mesh);
			TArray<uint8> Serialized;
			FMemoryWriter Writer(Serialized, false);
			Writer.SetIsTransacting(true);
			Copy.Serialize(Writer);
			Writer.Close();
			if (Writer.IsError())
			{
				return false;
			}
			Hash.AddString(TEXT("ue5.6-dynamic-mesh-transaction-v1"));
			Hash.AddUInt64(static_cast<uint64>(Serialized.Num()));
			Hash.AddBytes(Serialized.GetData(), Serialized.Num());

			TArray<FName> GenericAttributeNames;
			if (const UE::Geometry::FDynamicMeshAttributeSet* Attributes =
				Mesh.Attributes())
			{
				Attributes->GetAttachedAttributes().GenerateKeyArray(
					GenericAttributeNames);
				GenericAttributeNames.Sort(FNameLexicalLess());
			}
			Hash.AddInt32(GenericAttributeNames.Num());
			for (const FName& Name : GenericAttributeNames)
			{
				Hash.AddName(Name);
			}
			return true;
		}

		bool HashMeshDescription(
			FCarrierRevisionHash& Hash,
			const FMeshDescription& Description)
		{
			FMeshDescription Copy(Description);
			TArray<uint8> Serialized;
			FMemoryWriter Writer(Serialized, false);
			Copy.Serialize(Writer);
			Writer.Close();
			if (Writer.IsError())
			{
				return false;
			}
			Hash.AddString(TEXT("ue5.6-mesh-description-v1"));
			Hash.AddUInt64(static_cast<uint64>(Serialized.Num()));
			Hash.AddBytes(Serialized.GetData(), Serialized.Num());
			return true;
		}

		FString CarrierIdentity(UObject& Carrier, const FString& Kind)
		{
			FString StableIdentity;
			if (const ADynamicMeshActor* Actor =
				Cast<ADynamicMeshActor>(&Carrier))
			{
				const FGuid ActorGuid = Actor->GetActorGuid();
				StableIdentity = ActorGuid.IsValid()
					? TEXT("actor-guid:")
						+ ActorGuid.ToString(
							EGuidFormats::DigitsWithHyphensLower)
					: FString::Printf(
						TEXT("object-uid:%u"),
						Carrier.GetUniqueID());
			}
			else
			{
				const UPackage* Package = Carrier.GetOutermost();
				const FGuid PackageGuid = Package
					? Package->GetPersistentGuid()
					: FGuid();
				StableIdentity = PackageGuid.IsValid()
					? TEXT("package-guid:")
						+ PackageGuid.ToString(
							EGuidFormats::DigitsWithHyphensLower)
					: FString::Printf(
						TEXT("object-uid:%u"),
						Carrier.GetUniqueID());
			}
			return TEXT("modeling-carrier-v1:") + Kind + TEXT(":")
				+ Carrier.GetPathName() + TEXT(":") + StableIdentity;
		}
	}

	bool IsModelingGeometryCarrier(const UObject& Object)
	{
		return Object.IsA<ADynamicMeshActor>()
			|| Object.IsA<UStaticMesh>();
	}

	bool TryReadModelingGeometryCarrierRevision(
		const FString& SourceId,
		UObject& Carrier,
		TOptional<FUeAgentStateRevision>& OutRevision,
		FExecutionMutationError& OutError)
	{
		OutRevision.Reset();
		OutError = FExecutionMutationError();
		if (SourceId.IsEmpty())
		{
			OutError = FExecutionMutationError(
				TEXT("modeling_geometry_revision_source_missing"));
			return false;
		}

		FCarrierRevisionHash Hash;
		FString CarrierKind;
		if (ADynamicMeshActor* Actor = Cast<ADynamicMeshActor>(&Carrier))
		{
			CarrierKind = TEXT("dynamic_mesh_actor");
			UDynamicMeshComponent* Component =
				Actor->GetDynamicMeshComponent();
			UDynamicMesh* DynamicMesh =
				Component ? Component->GetDynamicMesh() : nullptr;
			if (!Component || !DynamicMesh)
			{
				OutError = FExecutionMutationError(
					TEXT("modeling_geometry_dynamic_mesh_missing"),
					Carrier.GetPathName());
				return false;
			}
			Hash.AddTransform(Actor->GetActorTransform());
			Hash.AddTransform(Component->GetRelativeTransform());
			Hash.AddInt32(static_cast<int32>(Component->GetCollisionEnabled()));
			Hash.AddName(Component->GetCollisionProfileName());
			Hash.AddUInt8(Component->GetGenerateOverlapEvents() ? 1 : 0);
			Hash.AddInt32(Component->GetNumMaterials());
			const TArray<FName> SlotNames = Component->GetMaterialSlotNames();
			Hash.AddInt32(SlotNames.Num());
			for (const FName& SlotName : SlotNames)
			{
				Hash.AddName(SlotName);
			}
			for (int32 Index = 0;
				Index < Component->GetNumMaterials();
				++Index)
			{
				HashMaterial(Hash, Component->GetMaterial(Index));
			}
			HashBodySetup(Hash, Component->GetBodySetup());
			bool bMeshRead = false;
			DynamicMesh->ProcessMesh(
				[&](const FDynamicMesh3& Mesh)
				{
					bMeshRead = HashDynamicMesh(Hash, Mesh);
				});
			if (!bMeshRead)
			{
				OutError = FExecutionMutationError(
					TEXT("modeling_geometry_revision_mesh_read_failed"),
					Carrier.GetPathName());
				return false;
			}
		}
		else if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(&Carrier))
		{
			CarrierKind = TEXT("static_mesh_asset");
			const FMeshDescription* Description =
				StaticMesh->GetMeshDescription(0);
			if (!Description)
			{
				OutError = FExecutionMutationError(
					TEXT("modeling_geometry_static_mesh_source_missing"),
					Carrier.GetPathName());
				return false;
			}
			if (!HashMeshDescription(Hash, *Description))
			{
				OutError = FExecutionMutationError(
					TEXT("modeling_geometry_revision_mesh_read_failed"),
					Carrier.GetPathName());
				return false;
			}
			Hash.AddInt32(StaticMesh->GetStaticMaterials().Num());
			for (const FStaticMaterial& Material
				: StaticMesh->GetStaticMaterials())
			{
				Hash.AddName(Material.MaterialSlotName);
				Hash.AddName(Material.ImportedMaterialSlotName);
				HashMaterial(Hash, Material.MaterialInterface);
			}
			HashBodySetup(Hash, StaticMesh->GetBodySetup());
		}
		else
		{
			OutError = FExecutionMutationError(
				TEXT("modeling_geometry_carrier_kind_unsupported"),
				Carrier.GetClass()->GetPathName());
			return false;
		}

		const FString Generation = CarrierIdentity(Carrier, CarrierKind);
		Hash.AddString(TEXT("modeling-carrier-revision-v1"));
		Hash.AddString(SourceId);
		Hash.AddString(CarrierKind);
		Hash.AddString(Generation);
		Hash.AddString(Carrier.GetClass()->GetPathName());
		int64 Sequence = -1;
		if (!Hash.Finalize(Sequence))
		{
			OutError = FExecutionMutationError(
				TEXT("modeling_geometry_revision_hash_failed"),
				Carrier.GetPathName());
			return false;
		}
		FUeAgentStateRevision Revision(SourceId, Generation, Sequence);
		if (!Revision.IsValid())
		{
			OutError = FExecutionMutationError(
				TEXT("modeling_geometry_revision_invalid"),
				Carrier.GetPathName());
			return false;
		}
		OutRevision.Emplace(MoveTemp(Revision));
		return true;
	}
}

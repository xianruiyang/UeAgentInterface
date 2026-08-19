#include "Composition/UeAgentCommandServiceComposition.h"

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "Core/Contract/UeAgentContract.h"

namespace UE::UAI::Composition
{
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace Contract;
	using namespace NetworkTransport;

	namespace
	{
		class FNeverCancelled final
			: public IUeAgentCancellationSignal
		{
		public:
			virtual bool IsCancelled() const override
			{
				return false;
			}
		};
	}

	struct FUeAgentCommandServiceComposition::FImpl
	{
		TSharedPtr<
			FUeAgentCommandIngress,
			ESPMode::ThreadSafe> Ingress;
	};

	bool FUeAgentCommandServiceConfig::IsValid() const
	{
		return !PrincipalSubject.IsEmpty()
			&& !AuthorityScopes.IsEmpty();
	}

	FUeAgentCommandServiceComposition::
		~FUeAgentCommandServiceComposition() = default;

	bool FUeAgentCommandServiceComposition::TryCreate(
		const FUeAgentCommandServiceConfig& Config,
		TSharedRef<const FUeAgentCommandContractRegistry> Registry,
		TArray<TSharedRef<IUeAgentCommandHandler>> Handlers,
		TUniquePtr<FUeAgentCommandServiceComposition>&
			OutComposition,
		FString& OutError)
	{
		OutComposition.Reset();
		OutError.Reset();
		if (!Config.IsValid())
		{
			OutError = TEXT("command_service_config_invalid");
			return false;
		}

		TSharedPtr<const FUeAgentCommandDispatch> Dispatch;
		if (!FUeAgentCommandDispatch::TryCreate(
			Registry,
			MoveTemp(Handlers),
			Dispatch,
			OutError))
		{
			return false;
		}

		TUniquePtr<FImpl> NewImpl = MakeUnique<FImpl>();
		const FString PrincipalSubject =
			Config.PrincipalSubject;
		const TArray<FString> AuthorityScopes =
			Config.AuthorityScopes;
		NewImpl->Ingress =
			MakeShared<
				FUeAgentCommandIngress,
				ESPMode::ThreadSafe>(
				FUeAgentCommandProtocolCodec(),
				Registry,
				Dispatch.ToSharedRef(),
				[
					PrincipalSubject,
					AuthorityScopes
				](
					const FUeAgentTransportPeer& Peer,
					const FString& CorrelationId,
					FString& CallerError)
					-> TOptional<FUeAgentCallerContext>
				{
					CallerError.Reset();
					if (Peer.Principal.Subject
						!= PrincipalSubject)
					{
						CallerError =
							TEXT(
								"authenticated transport principal mismatch");
						return {};
					}
					return FUeAgentCallerContext(
						Peer.Principal.Subject,
						AuthorityScopes,
						CorrelationId);
				},
				[](const FString& CorrelationId)
					-> TSharedRef<IUeAgentCancellationSignal>
				{
					return MakeShared<FNeverCancelled>();
				});

		OutComposition =
			TUniquePtr<FUeAgentCommandServiceComposition>(
				new FUeAgentCommandServiceComposition(
					MoveTemp(NewImpl)));
		return true;
	}

	TSharedRef<
		IUeAgentTransportRequestHandler,
		ESPMode::ThreadSafe>
	FUeAgentCommandServiceComposition::GetIngressHandler() const
	{
		checkf(
			Impl && Impl->Ingress.IsValid(),
			TEXT("command_service_ingress_unavailable"));
		return Impl->Ingress.ToSharedRef();
	}

	FUeAgentCommandServiceComposition::
		FUeAgentCommandServiceComposition(
			TUniquePtr<FImpl> InImpl)
		: Impl(MoveTemp(InImpl))
	{
	}
}

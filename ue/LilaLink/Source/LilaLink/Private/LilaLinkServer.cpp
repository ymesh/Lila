#include "LilaLinkServer.h"

#include "LilaLinkLog.h"
#include "LilaLinkMethodRegistry.h"

#include "Async/Async.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Misc/PackageName.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionActorDescInstance.h"
#include "WorldPartition/LoaderAdapter/LoaderAdapterActorList.h"
#include "WorldPartition/WorldPartitionHelpers.h"
#include "Common/TcpSocketBuilder.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/RunnableThread.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

namespace
{
constexpr int32 MaxLineBytes = 64 * 1024 * 1024;

enum ERpcError : int32
{
	ParseError = -32700,
	InvalidRequest = -32600,
	MethodNotFound = -32601,
	ServerError = -32000,
};

void DestroySocket(FSocket* Socket)
{
	if (Socket)
	{
		Socket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
	}
}
} // namespace

FString LilaLink::MakeResponse(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonValue>& Result, int32 ErrorCode, const FString& ErrorMessage)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLink::MakeResponse);

	TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	Response->SetField(TEXT("id"), Id.IsValid() ? Id : MakeShared<FJsonValueNull>());
	if (Result.IsValid())
	{
		Response->SetField(TEXT("result"), Result);
	}
	else
	{
		TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
		Error->SetNumberField(TEXT("code"), ErrorCode);
		Error->SetStringField(TEXT("message"), ErrorMessage);
		Response->SetObjectField(TEXT("error"), Error);
	}

	FString Out;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Response, Writer);
	return Out;
}

void FLilaLinkClient::QueueLine(const FString& Line)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkClient::QueueLine);

	FTCHARToUTF8 Utf8(*Line);
	FScopeLock Lock(&OutboxLock);
	Outbox.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	Outbox.Add('\n');
}

FLilaLinkCall::FLilaLinkCall(const TSharedPtr<FLilaLinkClient, ESPMode::ThreadSafe>& InClient, TSharedPtr<FJsonValue> InId, FString InMethod)
	: Client(InClient)
	, Id(MoveTemp(InId))
	, Method(MoveTemp(InMethod))
	, StartSeconds(FPlatformTime::Seconds())
{
}

FLilaLinkCall::~FLilaLinkCall()
{
	if (!bReplied)
	{
		Reply(nullptr, TEXT("The editor dropped this request without answering; see LogLilaLink in its log"));
	}
}

double FLilaLinkCall::GetElapsedSeconds() const
{
	return FPlatformTime::Seconds() - StartSeconds;
}

FString FLilaLinkCall::Describe() const
{
	FString IdText = TEXT("(notification)");
	if (Id.IsValid())
	{
		IdText = Id->Type == EJson::Number ? FString::Printf(TEXT("#%lld"), static_cast<int64>(Id->AsNumber())) : TEXT("#") + Id->AsString();
	}
	return Method + TEXT(" ") + IdText;
}

void FLilaLinkCall::Reply(const TSharedPtr<FJsonValue>& Result, const FString& Error)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkCall::Reply);

	if (bReplied.exchange(true))
	{
		UE_LOG(LogLilaLink, Warning, TEXT("%s answered twice; the second reply is dropped"), *Describe());
		return;
	}
	TSharedPtr<FJsonValue> Value = Result;
	if (!Value.IsValid() && Error.IsEmpty())
	{
		Value = MakeShared<FJsonValueNull>();
	}
	if (Value.IsValid())
	{
		UE_LOG(LogLilaLink, Log, TEXT("<- %s done in %.2f s"), *Describe(), GetElapsedSeconds());
	}
	else
	{
		UE_LOG(LogLilaLink, Warning, TEXT("<- %s failed in %.2f s: %s"), *Describe(), GetElapsedSeconds(), *Error);
	}
	if (!Id.IsValid())
	{
		return;
	}
	if (TSharedPtr<FLilaLinkClient, ESPMode::ThreadSafe> Pinned = Client.Pin())
	{
		Pinned->QueueLine(LilaLink::MakeResponse(Id, Value, ServerError, Error));
	}
	else
	{
		UE_LOG(LogLilaLink, Warning, TEXT("%s finished after its client disconnected"), *Describe());
	}
}

void FLilaLinkCall::Progress(const FString& Stage)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkCall::Progress);

	if (bReplied || !Id.IsValid())
	{
		return;
	}
	TSharedPtr<FLilaLinkClient, ESPMode::ThreadSafe> Pinned = Client.Pin();
	if (!Pinned)
	{
		return;
	}
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetField(TEXT("id"), Id);
	Params->SetStringField(TEXT("stage"), Stage);
	Params->SetNumberField(TEXT("elapsed"), GetElapsedSeconds());
	TSharedRef<FJsonObject> Note = MakeShared<FJsonObject>();
	Note->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	Note->SetStringField(TEXT("method"), TEXT("progress"));
	Note->SetObjectField(TEXT("params"), Params);

	FString Out;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Note, Writer);
	Pinned->QueueLine(Out);
}

FLilaLinkMethod LilaLink::Sync(FLilaLinkSyncMethod Fn)
{
	return [Fn = MoveTemp(Fn)](const TSharedPtr<FJsonObject>& Params, const FLilaLinkCallRef& Call)
	{
		FString Error;
		TSharedPtr<FJsonValue> Result = Fn(Params, Error);
		Call->Reply(Result, Error);
	};
}

FLilaLinkServer::FLilaLinkServer(TMap<FString, FLilaLinkMethod> InMethods)
	: Methods(MoveTemp(InMethods))
{
}

FLilaLinkServer::~FLilaLinkServer()
{
	Shutdown();
}

bool FLilaLinkServer::Start(int32 FirstPort, int32 PortsToTry)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkServer::Start);

	for (int32 Candidate = FirstPort; Candidate < FirstPort + PortsToTry && !Listener; ++Candidate)
	{
		// Not reusable: on Windows SO_REUSEADDR would let us share a port another editor already holds.
		Listener = FTcpSocketBuilder(TEXT("LilaLink listener"))
			.AsNonBlocking()
			.BoundToEndpoint(FIPv4Endpoint(FIPv4Address(127, 0, 0, 1), static_cast<uint16>(Candidate)))
			.Listening(8)
			.Build();
		if (Listener)
		{
			Port = Candidate;
		}
	}
	if (!Listener)
	{
		UE_LOG(LogLilaLink, Error, TEXT("No free port in %d-%d"), FirstPort, FirstPort + PortsToTry - 1);
		return false;
	}

	bStopping = false;
	LastGameTickSeconds = FPlatformTime::Seconds();
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(TEXT("LilaLinkServer"), 0.0f, [this](float DeltaTime) { return TickGameThread(DeltaTime); });
	Thread = FRunnableThread::Create(this, TEXT("LilaLinkServer"), 0, TPri_BelowNormal);
	UE_LOG(LogLilaLink, Log, TEXT("Listening on 127.0.0.1:%d"), Port);
	return Thread != nullptr;
}

void FLilaLinkServer::Shutdown()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkServer::Shutdown);

	if (Thread)
	{
		Stop();
		Thread->WaitForCompletion();
		delete Thread;
		Thread = nullptr;
	}
	if (TickHandle.IsValid())
	{
		FTSTicker::RemoveTicker(TickHandle);
		TickHandle.Reset();
	}
	FQueuedRequest Dropped;
	while (Requests.Dequeue(Dropped))
	{
	}
	QueuedCount = 0;
	InFlight.Reset();
	for (const TSharedPtr<FLilaLinkClient, ESPMode::ThreadSafe>& Client : Clients)
	{
		DestroySocket(Client->Socket);
		Client->Socket = nullptr;
	}
	Clients.Reset();
	ClientCount = 0;
	DestroySocket(Listener);
	Listener = nullptr;
}

uint32 FLilaLinkServer::Run()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkServer::Run);

	while (!bStopping)
	{
		bool bActive = false;

		bool bPending = false;
		if (Listener->HasPendingConnection(bPending) && bPending)
		{
			if (FSocket* Socket = Listener->Accept(TEXT("LilaLink client")))
			{
				Socket->SetNonBlocking(true);
				Socket->SetNoDelay(true);
				TSharedPtr<FLilaLinkClient, ESPMode::ThreadSafe> Client = MakeShared<FLilaLinkClient, ESPMode::ThreadSafe>();
				Client->Socket = Socket;
				Clients.Add(Client);
				ClientCount = Clients.Num();
				bActive = true;
				UE_LOG(LogLilaLink, Log, TEXT("Client connected"));
			}
		}

		for (int32 Index = Clients.Num() - 1; Index >= 0; --Index)
		{
			if (!ServiceClient(Clients[Index], bActive))
			{
				DestroySocket(Clients[Index]->Socket);
				Clients[Index]->Socket = nullptr;
				Clients.RemoveAtSwap(Index);
				ClientCount = Clients.Num();
				UE_LOG(LogLilaLink, Log, TEXT("Client disconnected"));
			}
		}

		WatchGameThread();

		if (!bActive)
		{
			FPlatformProcess::Sleep(0.005f);
		}
	}
	return 0;
}

void FLilaLinkServer::WatchGameThread()
{
	if (QueuedCount.load() == 0)
	{
		return;
	}
	const double Now = FPlatformTime::Seconds();
	const double Stalled = Now - LastGameTickSeconds.load();
	if (Stalled > 5.0 && Now >= NextStallWarningSeconds)
	{
		NextStallWarningSeconds = Now + 10.0;
		UE_LOG(LogLilaLink, Warning, TEXT("The game thread has not ticked for %.0f s; %d request(s) are waiting for it (editor busy, in a modal dialog, or hung)"), Stalled, QueuedCount.load());
	}
}

namespace
{
/**
 * Loads the open World Partition level's actors Lila made (any tag starting with Lila), once per level, so sends find,
 * update and remove what earlier sessions placed instead of adding a second copy beside unloaded actors.
 */
void LoadLilaActors()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkServer::LoadLilaActors);

	static TWeakObjectPtr<UWorldPartition> Done;
	// Holds the actors loaded; released when its level's World Partition shuts down, before the world goes.
	static TUniquePtr<FLoaderAdapterActorList> Loader;
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	UWorldPartition* Partition = World ? World->GetWorldPartition() : nullptr;
	if (!Partition || Done.Get() == Partition)
	{
		return;
	}
	Done = Partition;
	// The slot table sends write into, loaded now while nothing else is: loading it mid-send flushes async loading while
	// World Partition may be registering the actors placed just before.
	const TCHAR* SlotTable = TEXT("/Game/Lila/Slots/DA_LilaSlotTable");
	if (FPackageName::DoesPackageExist(SlotTable))
	{
		LoadObject<UObject>(nullptr, TEXT("/Game/Lila/Slots/DA_LilaSlotTable.DA_LilaSlotTable"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	}
	TArray<FGuid> Guids;
	FWorldPartitionHelpers::ForEachActorDescInstance(Partition, AActor::StaticClass(), [&Guids](const FWorldPartitionActorDescInstance* Desc) {
		if (!Desc->IsLoaded() && Desc->GetTags().ContainsByPredicate([](const FName& Tag) { return Tag.ToString().StartsWith(TEXT("Lila")); }))
		{
			Guids.Add(Desc->GetGuid());
		}
		return true;
	});
	if (!Guids.IsEmpty())
	{
		Loader = MakeUnique<FLoaderAdapterActorList>(World);
		// Level Instance partitions broadcast here too, while their cleanup swallows unregistrations: releasing the loader
		// then leaves its actors flagged as unregistering, and the next reference to one asserts in World Partition.
		TWeakObjectPtr<UWorldPartition> Owner = Partition;
		World->OnWorldPartitionUninitialized().AddLambda([Owner](UWorldPartition* Gone) {
			if (Gone == Owner.Get())
			{
				Loader.Reset();
			}
		});
		Loader->AddActors(Guids);
		UE_LOG(LogLilaLink, Log, TEXT("Loaded %d Lila actors of %s that World Partition had unloaded"), Guids.Num(), *World->GetName());
	}
}
} // namespace

bool FLilaLinkServer::TickGameThread(float DeltaTime)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkServer::TickGameThread);

	const double Now = FPlatformTime::Seconds();
	LastGameTickSeconds = Now;

	FQueuedRequest Request;
	while (Requests.Dequeue(Request))
	{
		--QueuedCount;
		FLilaLinkCallRef Call = Request.Call.ToSharedRef();
		FLilaLinkMethod External;
		const FLilaLinkMethod* Handler = Methods.Find(Call->GetMethod());
		if (!Handler && LilaLink::FindExternalMethod(Call->GetMethod(), External))
		{
			Handler = &External;
		}
		if (!Handler)
		{
			continue;
		}
		UE_LOG(LogLilaLink, Log, TEXT("-> %s (waited %.2f s for the game thread)"), *Call->Describe(), Now - Request.QueuedSeconds);
		LoadLilaActors();
		(*Handler)(Request.Params, Call);
		if (!Call->HasReplied())
		{
			InFlight.Add(Call.ToWeakPtr());
		}
	}

	ReportLongCalls(Now);
	return true;
}

void FLilaLinkServer::ReportLongCalls(double Now)
{
	InFlight.RemoveAll([](const TWeakPtr<FLilaLinkCall, ESPMode::ThreadSafe>& Weak)
	{
		TSharedPtr<FLilaLinkCall, ESPMode::ThreadSafe> Call = Weak.Pin();
		return !Call || Call->HasReplied();
	});
	if (InFlight.IsEmpty() || Now < NextLongCallReportSeconds)
	{
		return;
	}
	NextLongCallReportSeconds = Now + 10.0;
	for (const TWeakPtr<FLilaLinkCall, ESPMode::ThreadSafe>& Weak : InFlight)
	{
		if (TSharedPtr<FLilaLinkCall, ESPMode::ThreadSafe> Call = Weak.Pin(); Call && Call->GetElapsedSeconds() > 10.0)
		{
			UE_LOG(LogLilaLink, Log, TEXT("%s still running after %.0f s"), *Call->Describe(), Call->GetElapsedSeconds());
		}
	}
}

bool FLilaLinkServer::ServiceClient(const TSharedPtr<FLilaLinkClient, ESPMode::ThreadSafe>& Client, bool& bOutActive)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkServer::ServiceClient);

	FSocket* Socket = Client->Socket;
	uint8 Buffer[16 * 1024];
	for (;;)
	{
		int32 Read = 0;
		if (!Socket->Recv(Buffer, sizeof(Buffer), Read))
		{
			return false;
		}
		if (Read == 0)
		{
			break;
		}
		Client->Inbox.Append(Buffer, Read);
		bOutActive = true;
	}

	int32 Start = 0;
	for (int32 Index = 0; Index < Client->Inbox.Num(); ++Index)
	{
		if (Client->Inbox[Index] != '\n')
		{
			continue;
		}
		int32 End = Index;
		if (End > Start && Client->Inbox[End - 1] == '\r')
		{
			--End;
		}
		if (End > Start)
		{
			FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Client->Inbox.GetData() + Start), End - Start);
			HandleLine(Client, FString(Text.Length(), Text.Get()));
		}
		Start = Index + 1;
	}
	if (Start > 0)
	{
		Client->Inbox.RemoveAt(0, Start, EAllowShrinking::No);
	}
	if (Client->Inbox.Num() > MaxLineBytes)
	{
		UE_LOG(LogLilaLink, Warning, TEXT("Dropping client: request line over %d bytes"), MaxLineBytes);
		return false;
	}

	FScopeLock Lock(&Client->OutboxLock);
	if (Client->Outbox.Num() > 0)
	{
		int32 Sent = 0;
		if (!Socket->Send(Client->Outbox.GetData(), Client->Outbox.Num(), Sent))
		{
			if (ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->GetLastErrorCode() != SE_EWOULDBLOCK)
			{
				return false;
			}
			Sent = 0;
		}
		if (Sent > 0)
		{
			Client->Outbox.RemoveAt(0, Sent, EAllowShrinking::No);
			bOutActive = true;
		}
	}
	return true;
}

void FLilaLinkServer::HandleLine(const TSharedPtr<FLilaLinkClient, ESPMode::ThreadSafe>& Client, const FString& Line)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkServer::HandleLine);

	TSharedPtr<FJsonObject> Request;
	TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Line);
	if (!FJsonSerializer::Deserialize(Reader, Request) || !Request.IsValid())
	{
		Client->QueueLine(LilaLink::MakeResponse(nullptr, nullptr, ParseError, TEXT("Parse error")));
		return;
	}

	// No id means a notification: run it, send nothing back.
	TSharedPtr<FJsonValue> Id = Request->TryGetField(TEXT("id"));
	const bool bWantsReply = Id.IsValid();

	FString Method;
	if (!Request->TryGetStringField(TEXT("method"), Method))
	{
		if (bWantsReply)
		{
			Client->QueueLine(LilaLink::MakeResponse(Id, nullptr, InvalidRequest, TEXT("Missing method")));
		}
		return;
	}

	FLilaLinkMethod External;
	const FLilaLinkMethod* Handler = Methods.Find(Method);
	if (!Handler && LilaLink::FindExternalMethod(Method, External))
	{
		Handler = &External;
	}
	if (!Handler)
	{
		if (bWantsReply)
		{
			Client->QueueLine(LilaLink::MakeResponse(Id, nullptr, MethodNotFound, FString::Printf(TEXT("Unknown method '%s'"), *Method)));
		}
		return;
	}

	const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
	FQueuedRequest Queued;
	Queued.Params = Request->TryGetObjectField(TEXT("params"), ParamsPtr) ? *ParamsPtr : MakeShared<FJsonObject>();
	Queued.Call = MakeShared<FLilaLinkCall, ESPMode::ThreadSafe>(Client, Id, Method);
	Queued.QueuedSeconds = FPlatformTime::Seconds();
	UE_LOG(LogLilaLink, Verbose, TEXT("Queued %s"), *Queued.Call->Describe());
	++QueuedCount;
	Requests.Enqueue(MoveTemp(Queued));
}

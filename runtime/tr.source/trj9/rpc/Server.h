#ifndef RPC_SERVER_H
#define RPC_SERVER_H

#include <grpc++/grpc++.h>
#include "rpc/types.h"

namespace JAAS
{

class J9CompileStream
   {
public:
   J9CompileStream(size_t streamNum,
                   J9CompileService::AsyncService *service,
                   grpc::ServerCompletionQueue *notif)
      : _streamNum(streamNum), _service(service), _notif(notif)
      {
      acceptNewRPC();
      }

   ~J9CompileStream()
      {
      _cq.Shutdown();
      }

   bool readBlocking()
      {
      auto tag = (void *)_streamNum;
      bool ok;

      _stream->Read(&_cMsg, tag);
      return _cq.Next(&tag, &ok) && ok;
      }

   bool writeBlocking()
      {
      auto tag = (void *)_streamNum;
      bool ok;

      _stream->Write(_sMsg, tag);
      return _cq.Next(&tag, &ok) && ok;
      }

   // TODO: add checks for when Next fails
   void finish()
      {
      auto tag = (void *)_streamNum;
      bool ok;
      _stream->Finish(Status::OK, tag);
      _cq.Next(&tag, &ok);
      acceptNewRPC();
      }

   // Hopefully temporary
   void finishWithOnlyCode(const uint32_t &code)
      {
      _sMsg.set_compilation_code(code);
      writeBlocking();
      finish();
      }

   // For reading after calling readBlocking
   const J9ClientMessage& clientMessage()
      {
      return _cMsg;
      }

   // For setting up the message before calling writeBlocking
   J9ServerMessage* serverMessage()
      {
      return &_sMsg;
      }

   void acceptNewRPC()
      {
      _ctx.reset(new grpc::ServerContext);
      _stream.reset(new J9AsyncServerStream(_ctx.get()));
      _service->RequestCompile(_ctx.get(), _stream.get(), &_cq, _notif, (void *)_streamNum);
      }

private:
   const size_t _streamNum; // tagging for notification loop, used to identify associated CompletionQueue in vector
   grpc::ServerCompletionQueue *_notif;
   grpc::CompletionQueue _cq;
   J9CompileService::AsyncService *const _service;
   std::unique_ptr<J9AsyncServerStream> _stream;
   std::unique_ptr<grpc::ServerContext> _ctx;

   // re-usable message objects
   J9ServerMessage _sMsg;
   J9ClientMessage _cMsg;
   };

//
// Inherited class is starting point for the received compilation request
//
class J9BaseCompileDispatcher
   {
public:
   virtual void compile(J9CompileStream *stream) = 0;
   };

class J9CompileServer
   {
public:
   ~J9CompileServer()
      {
      _server->Shutdown();
      _notificationQueue->Shutdown();
      }

   void buildAndServe(J9BaseCompileDispatcher *compiler)
      {
      grpc::ServerBuilder builder;
      builder.AddListeningPort("0.0.0.0:38400", grpc::InsecureServerCredentials());
      builder.RegisterService(&_service);
      _notificationQueue = builder.AddCompletionQueue();

      _server = builder.BuildAndStart();
      serve(compiler);
      }

private:
   void serve(J9BaseCompileDispatcher *compiler)
      {
      bool ok = false;
      void *tag;

      // TODO: make this nicer
      for (size_t i = 0; i < 7; ++i)
         {
         _streams.push_back(std::unique_ptr<J9CompileStream>(new J9CompileStream(i, &_service, _notificationQueue.get())));
         }

      while (true)
         {
         _notificationQueue->Next(&tag, &ok);
         compiler->compile(_streams[(size_t)tag].get());
         }
      }


   std::unique_ptr<grpc::Server> _server;
   J9CompileService::AsyncService _service;
   std::unique_ptr<grpc::ServerCompletionQueue> _notificationQueue;
   std::vector<std::unique_ptr<J9CompileStream>> _streams;
   };

}

#endif // RPC_SERVER_H

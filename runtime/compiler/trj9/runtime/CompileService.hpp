#ifndef COMPILE_SERVICE_H
#define COMPILE_SERVICE_H

#include "rpc/Server.h"
#include "j9.h"
#include "j9nonbuilder.h"
#include "vmaccess.h"
#include "infra/Monitor.hpp"  // TR::Monitor
#include "control/CompilationRuntime.hpp"
#include "control/CompilationController.hpp"
#include "env/ClassLoaderTable.hpp"

class VMAccessHolder
   {
public:
   VMAccessHolder(J9VMThread *vm): _vm(vm)
      {
      acquireVMAccess(_vm);
      }

   ~VMAccessHolder()
      {
      releaseVMAccess(_vm);
      }

private:
   J9VMThread *_vm;
   };

J9Method *ramMethodFromRomMethod(J9JITConfig *jitConfig, J9VMThread *vmThread, 
   const J9ROMClass* romClass, const J9ROMMethod* romMethod, 
   void* classChainC, void* classChainCL) 
   {
   // Acquire vm access within this scope, variable is intentionally unused
   VMAccessHolder access(vmThread);

   TR_J9VMBase *fej9 = TR_J9VMBase::get(jitConfig, vmThread);
   TR_J9SharedCache *cache = fej9->sharedCache();
   J9ClassLoader *CL = (J9ClassLoader*) cache->persistentClassLoaderTable()->lookupClassLoaderAssociatedWithClassChain(classChainCL);
   if (CL)
      {
      J9Class *ramClass = (J9Class*)cache->lookupClassFromChainAndLoader((uintptrj_t *)classChainC, CL);
      if (ramClass)
         {
         J9Method *ramMethods = ramClass->ramMethods;
         for (int32_t i = 0; i < romClass->romMethodCount; i++)
            {
            J9Method *curMethod = ramMethods + i;
            J9ROMMethod *curROMMethod = J9_ROM_METHOD_FROM_RAM_METHOD(curMethod);
            if (curROMMethod == romMethod)
               return curMethod;
            }
         }
      }
   return NULL;
   }

void doAOTCompile(J9JITConfig* jitConfig, J9VMThread* vmThread, 
                  J9ROMClass* romClass, const J9ROMMethod* romMethod, 
                  J9Method* ramMethod, JAAS::J9CompileStream *rpc)
   {
   J9UTF8 *methodNameUTF = J9ROMNAMEANDSIGNATURE_NAME(&romMethod->nameAndSignature);
   std::string methodNameStr((const char*)methodNameUTF->data, (size_t)methodNameUTF->length);
   const char *methodName = methodNameStr.c_str();
   J9UTF8 *classNameUTF = J9ROMCLASS_CLASSNAME(romClass);
   std::string classNameStr((const char*)classNameUTF->data, (size_t)classNameUTF->length);
   const char *className = classNameStr.c_str();

   // Acquire vm access within this scope, variable is intentionally unused
   VMAccessHolder access(vmThread);

   PORT_ACCESS_FROM_JITCONFIG(jitConfig);

   if (TR::Options::getVerboseOption(TR_VerboseJaas))
      TR_VerboseLog::writeLineLocked(TR_Vlog_JAAS,
            "Server received request to compile %s.%s", className, methodName);

   TR::CompilationInfo * compInfo = getCompilationInfo(jitConfig);
   if (!(compInfo->reloRuntime()->isROMClassInSharedCaches((UDATA)romClass, jitConfig->javaVM))) 
      { 
      if (TR::Options::getVerboseOption(TR_VerboseJaas))
         TR_VerboseLog::writeLineLocked(TR_Vlog_JAAS,
               "ROMClass for %s is not in SCC so we cannot compile method %s. Aborting compilation", className, methodName);
      rpc->finishWithOnlyCode(compilationFailure);
      }
   else 
      {
      if (jitConfig->javaVM->sharedClassConfig->existsCachedCodeForROMMethod(vmThread, romMethod)) 
         {
         if (TR::Options::getVerboseOption(TR_VerboseJaas))
            TR_VerboseLog::writeLineLocked(TR_Vlog_JAAS,
                  "Method %s.%s already exists in SCC, aborting compilation.", className, methodName);
         rpc->finishWithOnlyCode(compilationNotNeeded);
         }
      else // do AOT compilation
         {
         if (ramMethod)
            {
            TR_J9VMBase *fe = TR_J9VMBase::get(jitConfig, vmThread);
            char sig[1000];
            fe->printTruncatedSignature(sig, 1000, (TR_OpaqueMethodBlock*)ramMethod);
            bool queued = false;
            TR_CompilationErrorCode compErrCode = compilationFailure;
            TR_YesNoMaybe async = TR_yes;
            TR_MethodEvent event;
            event._eventType = TR_MethodEvent::InterpreterCounterTripped;
            event._j9method = ramMethod;
            event._oldStartPC = 0;
            event._vmThread = vmThread;
            event._classNeedingThunk = 0;
            bool newPlanCreated;
            IDATA result = 0;
            TR_OptimizationPlan *plan = TR::CompilationController::getCompilationStrategy()->processEvent(&event, &newPlanCreated);

            // if the controller decides to compile this method, trigger the compilation
            if (plan)
               {
               TR::IlGeneratorMethodDetails details(ramMethod);
               result = (IDATA)compInfo->compileMethod(vmThread, details, 0, async, &compErrCode, &queued, plan, rpc);

               if (newPlanCreated)
                  {
                  if (!queued)
                     TR_OptimizationPlan::freeOptimizationPlan(plan);

                  // If the responder has been handed over to the compilation thread, the compErrCode should be compilationInProgress
                  if (compErrCode == compilationInProgress)
                     {
                     // This should be the only path in which we do not call finish (the compilation thread will do that instead)
                     if (TR::Options::getVerboseOption(TR_VerboseJaas))
                        TR_VerboseLog::writeLineLocked(TR_Vlog_JAAS,
                           "Server queued compilation for %s.%s", className, methodName);
                     }
                  else
                     {
                     rpc->finishWithOnlyCode(compErrCode);
                     if (TR::Options::getVerboseOption(TR_VerboseJaas))
                        TR_VerboseLog::writeLineLocked(TR_Vlog_JAAS,
                           "Server failed to queue compilation for %s.%s", className, methodName);
                     }
                  }
               else
                  {
                  if (TR::Options::getVerboseOption(TR_VerboseJaas))
                     TR_VerboseLog::writeLineLocked(TR_Vlog_JAAS,
                        "Server failed to compile %s.%s because a new plan could not be created.", className, methodName);
                  rpc->finishWithOnlyCode(compilationFailure);
                  }
               }
            else
               {
               if (TR::Options::getVerboseOption(TR_VerboseJaas))
                  TR_VerboseLog::writeLineLocked(TR_Vlog_JAAS,
                     "Server failed to compile %s.%s because no memory was available to create an optimization plan.", className, methodName);
               rpc->finishWithOnlyCode(compilationFailure);
               }
            }
         else // !method
            {
            if (TR::Options::getVerboseOption(TR_VerboseJaas))
               TR_VerboseLog::writeLineLocked(TR_Vlog_JAAS,
                  "Server couldn't find ramMethod for romMethod %s.%s .", className, methodName);
            rpc->finishWithOnlyCode(compilationFailure);
            }
         }
      }
   }

class J9CompileDispatcher : public JAAS::J9BaseCompileDispatcher
{
public:
   J9CompileDispatcher(J9JITConfig *jitConfig, J9VMThread *vmThread) : _jitConfig(jitConfig), _vmThread(vmThread) { }

   void compile(JAAS::J9CompileStream *stream) override
      {
      stream->readBlocking();
      auto req = stream->clientMessage();
      PORT_ACCESS_FROM_JITCONFIG(_jitConfig);
      TR_J9VMBase *fej9 = TR_J9VMBase::get(_jitConfig, _vmThread);
      TR_J9SharedCache *cache = fej9->sharedCache();
      J9ROMClass *romClass = (J9ROMClass*) cache->pointerFromOffsetInSharedCache((void*) req.classoffset());
      J9ROMMethod *romMethod = (J9ROMMethod*) cache->pointerFromOffsetInSharedCache((void*) req.methodoffset());
      void *classChainC = cache->pointerFromOffsetInSharedCache((void*) req.classchaincoffset());
      void *classChainCL = cache->pointerFromOffsetInSharedCache((void*) req.classchaincloffset());
      J9Method *ramMethod = ramMethodFromRomMethod(_jitConfig, _vmThread, romClass, romMethod, classChainC, classChainCL);
      doAOTCompile(_jitConfig, _vmThread, romClass, romMethod, ramMethod, stream);
      }

private:
   J9JITConfig *_jitConfig;
   J9VMThread *_vmThread;
};




#endif

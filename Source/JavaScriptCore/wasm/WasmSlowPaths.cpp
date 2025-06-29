/*
 * Copyright (C) 2019-2024 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "WasmSlowPaths.h"
#include "JSCJSValue.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

#if ENABLE(WEBASSEMBLY)

#include "BytecodeStructs.h"
#include "FrameTracers.h"
#include "JITExceptions.h"
#include "JSWebAssemblyArrayInlines.h"
#include "JSWebAssemblyException.h"
#include "JSWebAssemblyInstance.h"
#include "LLIntCommon.h"
#include "LLIntData.h"
#include "WasmBBQPlan.h"
#include "WasmCallee.h"
#include "WasmCallingConvention.h"
#include "WasmFunctionCodeBlockGenerator.h"
#include "WasmFunctionParser.h"
#include "WasmLLIntBuiltin.h"
#include "WasmLLIntGenerator.h"
#include "WasmModuleInformation.h"
#include "WasmOSREntryPlan.h"
#include "WasmOperationsInlines.h"
#include "WasmTypeDefinitionInlines.h"
#include "WasmWorklist.h"
#include "WebAssemblyFunction.h"
#include <bit>

namespace JSC { namespace LLInt {

#define WASM_RETURN_TWO(first, second) do { \
        return encodeResult(first, second); \
    } while (false)

#define WASM_END_IMPL() WASM_RETURN_TWO(pc, 0)

#define WASM_THROW(exceptionType) do { \
        callFrame->setArgumentCountIncludingThis(static_cast<int>(exceptionType)); \
        WASM_RETURN_TWO(LLInt::wasmExceptionInstructions(), 0); \
    } while (false)

#define WASM_END() do { \
        WASM_END_IMPL(); \
    } while (false)

#define WASM_RETURN(value) do { \
        callFrame->uncheckedR(instruction.m_dst) = static_cast<EncodedJSValue>(value); \
        WASM_END_IMPL(); \
    } while (false)

#define WASM_CALL_RETURN(targetInstance, callTarget) do { \
        static_assert(callTarget.getTag() == WasmEntryPtrTag); \
        callTarget.validate(); \
        WASM_RETURN_TWO(callTarget.taggedPtr(), targetInstance); \
    } while (false)

#define CALLEE() \
    static_cast<Wasm::LLIntCallee*>(callFrame->callee().asNativeCallee())

#define READ(virtualRegister) \
    (virtualRegister.isConstant() \
        ? JSValue::decode(CALLEE()->getConstant(virtualRegister)) \
        : callFrame->r(virtualRegister))

#if ENABLE(WEBASSEMBLY_BBQJIT)

extern "C" void SYSV_ABI wasm_log_crash(CallFrame*, JSWebAssemblyInstance* instance)
{
    dataLogLn("Reached LLInt code that should never have been executed.");
    dataLogLn("Module internal function count: ", instance->module().moduleInformation().internalFunctionCount());
    RELEASE_ASSERT_NOT_REACHED();
}

static inline bool shouldJIT(Wasm::LLIntCallee* callee)
{
    if (!Options::useBBQJIT() || !Wasm::BBQPlan::ensureGlobalBBQAllowlist().containsWasmFunction(callee->functionIndex()))
        return false;

    if (!Options::wasmFunctionIndexRangeToCompile().isInRange(callee->functionIndex()))
        return false;

    return true;
}

enum class OSRFor { Call, Loop };
static inline RefPtr<Wasm::JITCallee> jitCompileAndSetHeuristics(Wasm::LLIntCallee* callee, JSWebAssemblyInstance* instance, OSRFor osrFor)
{
    ASSERT(!instance->module().moduleInformation().usesSIMD(callee->functionIndex()));

    Wasm::LLIntTierUpCounter& tierUpCounter = callee->tierUpCounter();
    if (!tierUpCounter.checkIfOptimizationThresholdReached()) {
        dataLogLnIf(Options::verboseOSR(), "\tJIT threshold not reached. Adjusted: ", tierUpCounter);
        return nullptr;
    }

    MemoryMode memoryMode = instance->memory()->mode();
    Wasm::CalleeGroup& calleeGroup = *instance->calleeGroup();
    auto getReplacement = [&] () -> RefPtr<Wasm::JITCallee> {
        Locker locker { calleeGroup.m_lock };
        if (osrFor == OSRFor::Call)
            return calleeGroup.replacement(locker, callee->index());
        return calleeGroup.tryGetBBQCalleeForLoopOSR(locker, instance->vm(), callee->functionIndex());
    };

    if (RefPtr replacement = getReplacement()) {
        dataLogLnIf(Options::verboseOSR(), "\tCode was already compiled.");
        // FIXME: This should probably be some optimizeNow() for calls or checkIfOptimizationThresholdReached() should have a different threshold for calls.
        tierUpCounter.optimizeSoon();
        return replacement;
    }

    bool compile = false;
    {
        Locker locker { tierUpCounter.m_lock };
        switch (tierUpCounter.compilationStatus(memoryMode)) {
        case Wasm::LLIntTierUpCounter::CompilationStatus::NotCompiled:
            compile = true;
            tierUpCounter.setCompilationStatus(memoryMode, Wasm::LLIntTierUpCounter::CompilationStatus::Compiling);
            break;
        case Wasm::LLIntTierUpCounter::CompilationStatus::Compiling:
            tierUpCounter.optimizeAfterWarmUp();
            break;
        case Wasm::LLIntTierUpCounter::CompilationStatus::Compiled:
            break;
        }
    }

    if (compile) {
        Wasm::FunctionCodeIndex functionIndex = callee->functionIndex();
        if (Wasm::BBQPlan::ensureGlobalBBQAllowlist().containsWasmFunction(functionIndex)) {
            auto plan = Wasm::BBQPlan::create(instance->vm(), const_cast<Wasm::ModuleInformation&>(instance->module().moduleInformation()), functionIndex, callee->hasExceptionHandlers(), Ref(*instance->calleeGroup()), Wasm::Plan::dontFinalize());
            Wasm::ensureWorklist().enqueue(plan.get());
            dataLogLnIf(Options::verboseOSR(), "\tStarted BBQ compilation.");
            if (!Options::useConcurrentJIT() || !Options::useWasmLLInt()) [[unlikely]]
                plan->waitForCompletion();
            else
                tierUpCounter.optimizeAfterWarmUp();
        }
    }

    return getReplacement();
}

static inline Expected<RefPtr<Wasm::JITCallee>, Wasm::Plan::Error> jitCompileSIMDFunction(Wasm::LLIntCallee* callee, JSWebAssemblyInstance* instance)
{
    Wasm::LLIntTierUpCounter& tierUpCounter = callee->tierUpCounter();

    MemoryMode memoryMode = instance->memory()->mode();
    Wasm::CalleeGroup& calleeGroup = *instance->calleeGroup();
    {
        Locker locker { calleeGroup.m_lock };
        if (RefPtr replacement = calleeGroup.replacement(locker, callee->index()))  {
            dataLogLnIf(Options::verboseOSR(), "\tSIMD code was already compiled.");
            return replacement;
        }
    }

    bool compile = false;
    while (!compile) {
        Locker locker { tierUpCounter.m_lock };
        switch (tierUpCounter.compilationStatus(memoryMode)) {
        case Wasm::LLIntTierUpCounter::CompilationStatus::NotCompiled:
            compile = true;
            tierUpCounter.setCompilationStatus(memoryMode, Wasm::LLIntTierUpCounter::CompilationStatus::Compiling);
            break;
        case Wasm::LLIntTierUpCounter::CompilationStatus::Compiling:
            Thread::yield();
            continue;
        case Wasm::LLIntTierUpCounter::CompilationStatus::Compiled: {
            // We can't hold a tierUpCounter lock while holding the calleeGroup lock since calleeGroup could reset our counter while releasing BBQ code.
            // Besides we're outside the critical section.
            locker.unlockEarly();
            {
                Locker locker { calleeGroup.m_lock };
                RefPtr replacement = calleeGroup.replacement(locker, callee->index());
                RELEASE_ASSERT(replacement);
                return replacement;
            }
        }
        }
    }

    Wasm::FunctionCodeIndex functionIndex = callee->functionIndex();
    ASSERT(instance->module().moduleInformation().usesSIMD(functionIndex));
    auto plan = Wasm::BBQPlan::create(instance->vm(), const_cast<Wasm::ModuleInformation&>(instance->module().moduleInformation()), functionIndex, callee->hasExceptionHandlers(), Ref(*instance->calleeGroup()), Wasm::Plan::dontFinalize());
    Wasm::ensureWorklist().enqueue(plan.get());
    plan->waitForCompletion();
    if (plan->failed())
        return makeUnexpected(plan->error());

    {
        Locker locker { tierUpCounter.m_lock };
        RELEASE_ASSERT(tierUpCounter.compilationStatus(memoryMode) == Wasm::LLIntTierUpCounter::CompilationStatus::Compiled);
    }

    Locker locker { calleeGroup.m_lock };
    RefPtr replacement = calleeGroup.replacement(locker, callee->index());
    RELEASE_ASSERT(replacement);
    return replacement;
}

WASM_SLOW_PATH_DECL(prologue_osr)
{
    UNUSED_PARAM(pc);

    Wasm::LLIntCallee* callee = CALLEE();

    if (!shouldJIT(callee)) {
        callee->tierUpCounter().deferIndefinitely();
        WASM_RETURN_TWO(nullptr, nullptr);
    }

    if (!Options::useWasmLLIntPrologueOSR())
        WASM_RETURN_TWO(nullptr, nullptr);

    dataLogLnIf(Options::verboseOSR(), *callee, ": Entered prologue_osr with tierUpCounter = ", callee->tierUpCounter());

    if (RefPtr replacement = jitCompileAndSetHeuristics(callee, instance, OSRFor::Call))
        WASM_RETURN_TWO(replacement->entrypoint().taggedPtr(), nullptr);

    WASM_RETURN_TWO(nullptr, nullptr);
}

WASM_SLOW_PATH_DECL(loop_osr)
{
    Wasm::LLIntCallee* callee = CALLEE();
    Wasm::LLIntTierUpCounter& tierUpCounter = callee->tierUpCounter();

    if (!Options::useWasmOSR() || !Options::useWasmLLIntLoopOSR() || !shouldJIT(callee)) {
        slow_path_wasm_prologue_osr(callFrame, pc, instance);
        WASM_RETURN_TWO(nullptr, nullptr);
    }

    dataLogLnIf(Options::verboseOSR(), *callee, ": Entered loop_osr with tierUpCounter = ", tierUpCounter);

    if (!tierUpCounter.checkIfOptimizationThresholdReached()) {
        dataLogLnIf(Options::verboseOSR(), "\tJIT threshold not reached. Adjusted: ", tierUpCounter);
        WASM_RETURN_TWO(nullptr, nullptr);
    }

    unsigned loopOSREntryBytecodeOffset = callee->bytecodeOffset(pc);
    const auto& osrEntryData = tierUpCounter.osrEntryDataForLoop(loopOSREntryBytecodeOffset);

    if (!Options::useBBQJIT())
        WASM_RETURN_TWO(nullptr, nullptr);

    RefPtr compiledCallee = jitCompileAndSetHeuristics(callee, instance, OSRFor::Loop);
    if (!compiledCallee) {
        dataLogLnIf(Options::verboseOSR(), "\tNo BBQCallee yet, bailing from loop OSR");
        WASM_RETURN_TWO(nullptr, nullptr);
    }

    auto* bbqCallee = static_cast<Wasm::BBQCallee*>(compiledCallee.get());
    ASSERT(bbqCallee->compilationMode() == Wasm::CompilationMode::BBQMode);

    size_t osrEntryScratchBufferSize = bbqCallee->osrEntryScratchBufferSize();
    RELEASE_ASSERT(osrEntryScratchBufferSize >= osrEntryData.values.size());

    uintptr_t stackPointer = reinterpret_cast<uintptr_t>(currentStackPointer());
    ASSERT(bbqCallee->stackCheckSize());
    uintptr_t stackExtent = stackPointer - bbqCallee->stackCheckSize();
    uintptr_t stackLimit = reinterpret_cast<uintptr_t>(instance->softStackLimit());
    if (stackExtent >= stackPointer || stackExtent <= stackLimit) [[unlikely]] {
        dataLogLnIf(Options::verboseOSR(), "\tSkipping BBQ loop tier up due to stack check; ", RawHex(stackPointer), " -> ", RawHex(stackExtent), " is past soft limit ", RawHex(stackLimit));
        WASM_RETURN_TWO(nullptr, nullptr);
    }

    uint64_t* buffer = instance->vm().wasmContext.scratchBufferForSize(osrEntryScratchBufferSize);
    if (!buffer) {
        dataLogLnIf(Options::verboseOSR(), "\tSkipping BBQ loop tier up due to lack of scratch buffer");
        WASM_RETURN_TWO(nullptr, nullptr);
    }

    uint32_t index = 0;
    buffer[index ++] = osrEntryData.loopIndex; // First entry is the loop index.
    for (VirtualRegister reg : osrEntryData.values)
        buffer[index++] = READ(reg).encodedJSValue();

    auto sharedLoopEntrypoint = bbqCallee->sharedLoopEntrypoint();
    RELEASE_ASSERT(sharedLoopEntrypoint);

    dataLogLnIf(Options::verboseOSR(), "\tEntering BBQ in loop tier up now.");
    WASM_RETURN_TWO(buffer, sharedLoopEntrypoint->taggedPtr());
}

WASM_SLOW_PATH_DECL(epilogue_osr)
{
    Wasm::LLIntCallee* callee = CALLEE();

    if (!shouldJIT(callee)) {
        callee->tierUpCounter().deferIndefinitely();
        WASM_END_IMPL();
    }
    if (!Options::useWasmLLIntEpilogueOSR())
        WASM_END_IMPL();

    dataLogLnIf(Options::verboseOSR(), *callee, ": Entered epilogue_osr with tierUpCounter = ", callee->tierUpCounter());

    jitCompileAndSetHeuristics(callee, instance, OSRFor::Call);
    WASM_END_IMPL();
}

WASM_SLOW_PATH_DECL(simd_go_straight_to_bbq_osr)
{
    UNUSED_PARAM(pc);
    Wasm::LLIntCallee* callee = CALLEE();

    if (!Options::useWasmSIMD())
        RELEASE_ASSERT_NOT_REACHED();
    RELEASE_ASSERT(shouldJIT(callee));

    dataLogLnIf(Options::verboseOSR(), *callee, ": Entered simd_go_straight_to_bbq_osr with tierUpCounter = ", callee->tierUpCounter());

    auto result = jitCompileSIMDFunction(callee, instance);
    if (result.has_value()) [[likely]]
        WASM_RETURN_TWO(result.value()->entrypoint().taggedPtr(), nullptr);

    switch (result.error()) {
    case Wasm::Plan::Error::OutOfMemory:
        WASM_THROW(Wasm::ExceptionType::OutOfMemory);
    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}
#endif

#if LLINT_TRACING
extern "C" void SYSV_ABI logWasmPrologue(uint64_t i, uint64_t* fp, uint64_t* sp)
{
    if (!Options::traceWasmLLIntExecution())
        return;
    CallFrame* callFrame = reinterpret_cast<CallFrame*>(fp);
    dataLogLn("logWasmPrologue ", i, " ", RawPointer(fp), " ", RawPointer(sp));
    dataLogLn("FP[+Callee] ", RawHex(fp[static_cast<int>(CallFrameSlot::callee)]));
    dataLogLn("FP[+CodeBlock] ", RawHex(fp[static_cast<int>(CallFrameSlot::codeBlock)]));
    dataLogLn("FP[+returnpc] ", RawHex(fp[static_cast<int>(OBJECT_OFFSETOF(CallerFrameAndPC, returnPC) / 8)]));
    dataLogLn("FP[+callerFrame] ", RawHex(fp[static_cast<int>(OBJECT_OFFSETOF(CallerFrameAndPC, callerFrame) / 8)]));
    dataLogLn("WasmCallee ", *static_cast<Wasm::Callee*>(callFrame->callee().asNativeCallee()));
}
#endif

WASM_SLOW_PATH_DECL(trace)
{
    UNUSED_PARAM(instance);

    if (!Options::traceWasmLLIntExecution())
        WASM_END_IMPL();

    WasmOpcodeID opcodeID = pc->opcodeID();
    dataLogF("<%p> %p / %p: executing bc#%zu, %s, pc = %p\n",
        &Thread::currentSingleton(),
        CALLEE(),
        callFrame,
        static_cast<intptr_t>(CALLEE()->bytecodeOffset(pc)),
        pc->name(),
        pc);
    if (opcodeID == wasm_enter) {
        dataLogF("Frame will eventually return to %p\n", callFrame->returnPCForInspection());
        *std::bit_cast<volatile char*>(callFrame->returnPCForInspection());
    }
    if (opcodeID == wasm_ret) {
        dataLogF("Will be returning to %p\n", callFrame->returnPCForInspection());
        dataLogF("The new cfr will be %p\n", callFrame->callerFrame());
    }
    WASM_END_IMPL();
}

WASM_SLOW_PATH_DECL(out_of_line_jump_target)
{
    UNUSED_PARAM(instance);

    pc = CALLEE()->outOfLineJumpTarget(pc);
    WASM_END_IMPL();
}

WASM_SLOW_PATH_DECL(ref_func)
{
    auto instruction = pc->as<WasmRefFunc>();
    WASM_RETURN(Wasm::refFunc(instance, instruction.m_functionIndex));
}

WASM_SLOW_PATH_DECL(array_new)
{
    auto instruction = pc->as<WasmArrayNew>();
    uint32_t size = READ(instruction.m_size).unboxedUInt32();
    Wasm::ArrayGetKind kind = static_cast<Wasm::ArrayGetKind>(instruction.m_arrayNewKind);

    const Wasm::TypeDefinition& arraySignature = instance->module().moduleInformation().typeSignatures[instruction.m_typeIndex]->expand();
    ASSERT(arraySignature.is<Wasm::ArrayType>());
    Wasm::StorageType elementType = arraySignature.as<Wasm::ArrayType>()->elementType().type;

    EncodedJSValue value = 0;
    switch (kind) {
    case Wasm::ArrayGetKind::New: {
        value = READ(instruction.m_value).encodedJSValue();
        break;
    }
    case Wasm::ArrayGetKind::NewDefault: {
        if (Wasm::isRefType(elementType))
            value = JSValue::encode(jsNull());
        else if (elementType.unpacked().isV128()) {
            JSValue result = Wasm::arrayNew(instance, instruction.m_typeIndex, size, vectorAllZeros());
            if (result.isNull()) [[unlikely]]
                WASM_THROW(Wasm::ExceptionType::BadArrayNew);
            WASM_RETURN(JSValue::encode(result));
        }
        break;
    }
    case Wasm::ArrayGetKind::NewFixed: {
        // In this case, m_value must refer to a possibly-empty array of arguments,
        // so m_value being constant would be a bug.
        ASSERT(!instruction.m_value.isConstant());
        JSValue result = Wasm::arrayNewFixed(instance, instruction.m_typeIndex, size, reinterpret_cast<uint64_t*>(&callFrame->r(instruction.m_value)));
        if (result.isNull()) [[unlikely]]
            WASM_THROW(Wasm::ExceptionType::BadArrayNew);
        WASM_RETURN(JSValue::encode(result));
    }
    }
    ASSERT(!elementType.unpacked().isV128());
    JSValue result = Wasm::arrayNew(instance, instruction.m_typeIndex, size, value);
    if (result.isNull()) [[unlikely]]
        WASM_THROW(Wasm::ExceptionType::BadArrayNew);
    WASM_RETURN(JSValue::encode(result));
}

WASM_SLOW_PATH_DECL(array_get)
{
    SlowPathFrameTracer tracer(instance->vm(), callFrame);

    auto instruction = pc->as<WasmArrayGet>();
    EncodedJSValue arrayref = READ(instruction.m_arrayref).encodedJSValue();
    if (JSValue::decode(arrayref).isNull())
        WASM_THROW(Wasm::ExceptionType::NullArrayGet);
    uint32_t index = READ(instruction.m_index).unboxedUInt32();
    JSValue arrayValue = JSValue::decode(arrayref);
    ASSERT(arrayValue.isObject());
    JSWebAssemblyArray* arrayObject = jsCast<JSWebAssemblyArray*>(arrayValue.getObject());
    if (index >= arrayObject->size())
        WASM_THROW(Wasm::ExceptionType::OutOfBoundsArrayGet);
    Wasm::ExtGCOpType arrayGetKind = static_cast<Wasm::ExtGCOpType>(instruction.m_arrayGetKind);
    if (arrayGetKind == Wasm::ExtGCOpType::ArrayGetS) {
        EncodedJSValue value = Wasm::arrayGet(instance, instruction.m_typeIndex, arrayref, index);
        Wasm::StorageType type = arrayObject->elementType().type;
        ASSERT(type.is<Wasm::PackedType>());
        size_t elementSize = type.as<Wasm::PackedType>() == Wasm::PackedType::I8 ? sizeof(uint8_t) : sizeof(uint16_t);
        uint8_t bitShift = (sizeof(uint32_t) - elementSize) * 8;
        int32_t result = static_cast<int32_t>(value);
        result = result << bitShift;
        WASM_RETURN(static_cast<EncodedJSValue>(result >> bitShift));
    } else
        WASM_RETURN(Wasm::arrayGet(instance, instruction.m_typeIndex, arrayref, index));
}

WASM_SLOW_PATH_DECL(array_set)
{
    SlowPathFrameTracer tracer(instance->vm(), callFrame);

    auto instruction = pc->as<WasmArraySet>();
    EncodedJSValue arrayref = READ(instruction.m_arrayref).encodedJSValue();
    if (JSValue::decode(arrayref).isNull())
        WASM_THROW(Wasm::ExceptionType::NullArraySet);
    uint32_t index = READ(instruction.m_index).unboxedUInt32();
    uint64_t value = static_cast<uint64_t>(READ(instruction.m_value).unboxedInt64());

    JSValue arrayValue = JSValue::decode(arrayref);
    ASSERT(arrayValue.isObject());
    JSWebAssemblyArray* arrayObject = jsCast<JSWebAssemblyArray*>(arrayValue.getObject());
    if (index >= arrayObject->size())
        WASM_THROW(Wasm::ExceptionType::OutOfBoundsArraySet);

    Wasm::arraySet(instance, instruction.m_typeIndex, arrayref, index, value);
    WASM_END_IMPL();
}

WASM_SLOW_PATH_DECL(array_fill)
{
    VM& vm = instance->vm();
    SlowPathFrameTracer tracer(vm, callFrame);

    auto instruction = pc->as<WasmArrayFill>();
    EncodedJSValue arrayref = READ(instruction.m_arrayref).encodedJSValue();
    if (JSValue::decode(arrayref).isNull())
        WASM_THROW(Wasm::ExceptionType::NullArrayFill);
    uint32_t offset = READ(instruction.m_offset).unboxedUInt32();
    EncodedJSValue value = READ(instruction.m_value).encodedJSValue();
    uint32_t size = READ(instruction.m_size).unboxedUInt32();

    if (!Wasm::arrayFill(vm, arrayref, offset, value, size))
        WASM_THROW(Wasm::ExceptionType::OutOfBoundsArrayFill);
    WASM_END_IMPL();
}

WASM_SLOW_PATH_DECL(struct_new)
{
    SlowPathFrameTracer tracer(instance->vm(), callFrame);

    auto instruction = pc->as<WasmStructNew>();
    ASSERT(instruction.m_typeIndex < instance->module().moduleInformation().typeCount());

    ASSERT(!instruction.m_firstValue.isConstant());
    EncodedJSValue result = Wasm::structNew(instance, instruction.m_typeIndex, instruction.m_useDefault, instruction.m_useDefault ? nullptr : reinterpret_cast<uint64_t*>(&callFrame->r(instruction.m_firstValue)));
    if (JSValue::decode(result).isNull())
        WASM_THROW(Wasm::ExceptionType::BadStructNew);
    WASM_RETURN(result);
}

WASM_SLOW_PATH_DECL(struct_get)
{
    UNUSED_PARAM(instance);
    auto instruction = pc->as<WasmStructGet>();
    auto structReference = READ(instruction.m_structReference).encodedJSValue();
    if (JSValue::decode(structReference).isNull())
        WASM_THROW(Wasm::ExceptionType::NullStructGet);
    Wasm::ExtGCOpType structGetKind = static_cast<Wasm::ExtGCOpType>(instruction.m_structGetKind);
    if (structGetKind == Wasm::ExtGCOpType::StructGetS) {
        EncodedJSValue value = Wasm::structGet(structReference, instruction.m_fieldIndex);
        JSWebAssemblyStruct* structObject = jsCast<JSWebAssemblyStruct*>(JSValue::decode(structReference).getObject());
        Wasm::StorageType type = structObject->fieldType(instruction.m_fieldIndex).type;
        ASSERT(type.is<Wasm::PackedType>());
        size_t elementSize = type.as<Wasm::PackedType>() == Wasm::PackedType::I8 ? sizeof(uint8_t) : sizeof(uint16_t);
        uint8_t bitShift = (sizeof(uint32_t) - elementSize) * 8;
        int32_t result = static_cast<int32_t>(value);
        result = result << bitShift;
        WASM_RETURN(static_cast<EncodedJSValue>(result >> bitShift));
    } else
        WASM_RETURN(Wasm::structGet(structReference, instruction.m_fieldIndex));
}

WASM_SLOW_PATH_DECL(struct_set)
{
    UNUSED_PARAM(instance);
    SlowPathFrameTracer tracer(instance->vm(), callFrame);

    auto instruction = pc->as<WasmStructSet>();
    auto structReference = READ(instruction.m_structReference).encodedJSValue();
    if (JSValue::decode(structReference).isNull())
        WASM_THROW(Wasm::ExceptionType::NullStructSet);
    auto value = static_cast<uint64_t>(READ(instruction.m_value).unboxedInt64());
    Wasm::structSet(structReference, instruction.m_fieldIndex, value);
    WASM_END();
}

WASM_SLOW_PATH_DECL(table_get)
{
    auto instruction = pc->as<WasmTableGet>();
    int32_t index = READ(instruction.m_index).unboxedInt32();
    EncodedJSValue result = Wasm::tableGet(instance, instruction.m_tableIndex, index);
    if (!result)
        WASM_THROW(Wasm::ExceptionType::OutOfBoundsTableAccess);
    WASM_RETURN(result);
}

WASM_SLOW_PATH_DECL(table_set)
{
    auto instruction = pc->as<WasmTableSet>();
    uint32_t index = READ(instruction.m_index).unboxedUInt32();
    EncodedJSValue value = READ(instruction.m_value).encodedJSValue();
    if (!Wasm::tableSet(instance, instruction.m_tableIndex, index, value))
        WASM_THROW(Wasm::ExceptionType::OutOfBoundsTableAccess);
    WASM_END();
}

WASM_SLOW_PATH_DECL(table_init)
{
    auto instruction = pc->as<WasmTableInit>();
    uint32_t dstOffset = READ(instruction.m_dstOffset).unboxedUInt32();
    uint32_t srcOffset = READ(instruction.m_srcOffset).unboxedUInt32();
    uint32_t length = READ(instruction.m_length).unboxedUInt32();
    if (!Wasm::tableInit(instance, instruction.m_elementIndex, instruction.m_tableIndex, dstOffset, srcOffset, length))
        WASM_THROW(Wasm::ExceptionType::OutOfBoundsTableAccess);
    WASM_END();
}

WASM_SLOW_PATH_DECL(table_fill)
{
    auto instruction = pc->as<WasmTableFill>();
    uint32_t offset = READ(instruction.m_offset).unboxedUInt32();
    EncodedJSValue fill = READ(instruction.m_fill).encodedJSValue();
    uint32_t size = READ(instruction.m_size).unboxedUInt32();
    if (!Wasm::tableFill(instance, instruction.m_tableIndex, offset, fill, size))
        WASM_THROW(Wasm::ExceptionType::OutOfBoundsTableAccess);
    WASM_END();
}

WASM_SLOW_PATH_DECL(table_grow)
{
    auto instruction = pc->as<WasmTableGrow>();
    EncodedJSValue fill = READ(instruction.m_fill).encodedJSValue();
    uint32_t size = READ(instruction.m_size).unboxedUInt32();
    WASM_RETURN(Wasm::tableGrow(instance, instruction.m_tableIndex, fill, size));
}

WASM_SLOW_PATH_DECL(grow_memory)
{
    SlowPathFrameTracer tracer(instance->vm(), callFrame);

    auto instruction = pc->as<WasmGrowMemory>();
    int32_t delta = READ(instruction.m_delta).unboxedInt32();
    WASM_RETURN(Wasm::growMemory(instance, delta));
}

/**
 * Given a function index, determine the pointer to its executable code.
 * Return a pair of the wasm instance pointer and the code pointer.
 */
static inline UGPRPair resolveWasmCall(Register* partiallyConstructedCalleeFrame, JSWebAssemblyInstance* instance, Wasm::FunctionSpaceIndex functionIndex)
{
    uint32_t importFunctionCount = instance->module().moduleInformation().importFunctionCount();

    CodePtr<WasmEntryPtrTag> codePtr;

    Register& calleeStackSlot = partiallyConstructedCalleeFrame[static_cast<int>(CallFrameSlot::callee)];
    Register& functionInfoSlot = partiallyConstructedCalleeFrame[static_cast<int>(CallFrameSlot::codeBlock)];
    ASSERT(calleeStackSlot.unboxedInt64() == 0xBEEF);

    if (functionIndex < importFunctionCount) {
        auto* functionInfo = instance->importFunctionInfo(functionIndex);
        codePtr = functionInfo->importFunctionStub;
        // This may call the  wasm_to_js or wasm_to_wasm thunks.
        // In the jit case, they already have everything they need to set the callee and target instance.
        // For the non-jit case, we set those here.

        calleeStackSlot = functionInfo->boxedWasmCalleeLoadLocation->encodedBits();
        // For the non-jit wasm_to_js case specifically, we also pass along this functionInfo*, since
        // this new callee will have no way to access it.
        if (!functionInfo->targetInstance)
            functionInfoSlot = reinterpret_cast<uintptr_t>(functionInfo);
        else
            functionInfoSlot = functionInfo->targetInstance.get();
    } else {
        // Target is a wasm function within the same instance
        codePtr = *instance->calleeGroup()->entrypointLoadLocationFromFunctionIndexSpace(functionIndex);
        auto callee = instance->calleeGroup()->wasmCalleeFromFunctionIndexSpace(functionIndex);
        calleeStackSlot = CalleeBits::encodeNativeCallee(callee.get());
    }

    WASM_CALL_RETURN(instance, codePtr);
}

WASM_SLOW_PATH_DECL(call)
{
    auto instruction = pc->as<WasmCall>();
    Register* partiallyConstructedCalleeFrame = &callFrame->registers()[-safeCast<int>(instruction.m_stackOffset)];
    return resolveWasmCall(partiallyConstructedCalleeFrame, instance, Wasm::FunctionSpaceIndex(instruction.m_functionIndex));
}

/**
 * Given a table index and an index of a function in the table, determine the pointer to the
 * executable code of the function. Return a pair of the function's module and the code pointer.
 */
static inline UGPRPair resolveWasmCallIndirect(Register* partiallyConstructedCalleeFrame, CallFrame* callFrame, JSWebAssemblyInstance* instance, Wasm::FunctionSpaceIndex functionIndex, unsigned tableIndex, unsigned typeIndex)
{
    Wasm::FuncRefTable* table = instance->table(tableIndex)->asFuncrefTable();

    if (functionIndex >= table->length())
        WASM_THROW(Wasm::ExceptionType::OutOfBoundsCallIndirect);

    const Wasm::FuncRefTable::Function& function = table->function(functionIndex);

    if (function.m_function.typeIndex == Wasm::TypeDefinition::invalidIndex)
        WASM_THROW(Wasm::ExceptionType::NullTableEntry);

    const auto& callSignature = CALLEE()->signature(typeIndex);
    if (!Wasm::isSubtypeIndex(function.m_function.typeIndex, callSignature.index()))
        WASM_THROW(Wasm::ExceptionType::BadSignature);

    Register& calleeStackSlot = partiallyConstructedCalleeFrame[static_cast<int>(CallFrameSlot::callee)];
    Register& functionInfoSlot = partiallyConstructedCalleeFrame[static_cast<int>(CallFrameSlot::codeBlock)];
    ASSERT(calleeStackSlot.unboxedInt64() == 0xBEEF);

    if (function.m_function.boxedWasmCalleeLoadLocation)
        calleeStackSlot = function.m_function.boxedWasmCalleeLoadLocation->encodedBits();
    else
        calleeStackSlot = CalleeBits::nullCallee().encodedBits();

    if (!function.m_function.targetInstance)
        functionInfoSlot = reinterpret_cast<uintptr_t>(function.m_callLinkInfo);
    else
        functionInfoSlot = function.m_instance;

    auto callTarget = *function.m_function.entrypointLoadLocation;
    WASM_CALL_RETURN(function.m_instance, callTarget);
}

WASM_SLOW_PATH_DECL(call_indirect)
{
    auto instruction = pc->as<WasmCallIndirect>();
    auto functionIndex = Wasm::FunctionSpaceIndex(READ(instruction.m_functionIndex).unboxedInt32());
    Register* partiallyConstructedCalleeFrame = &callFrame->registers()[-safeCast<int>(instruction.m_stackOffset)];
    return resolveWasmCallIndirect(partiallyConstructedCalleeFrame, callFrame, instance, functionIndex, instruction.m_tableIndex, instruction.m_typeIndex);
}

/**
 * Given a Wasm function as a JS object, determine the pointer to the executable code of the
 * function. Return a pair of the function's Wasm instance and the code pointer.
 */
static inline UGPRPair resolveWasmCallRef(Register* partiallyConstructedCalleeFrame, CallFrame* callFrame, JSWebAssemblyInstance* callerInstance, JSValue targetReference, unsigned typeIndex)
{
    UNUSED_PARAM(callFrame);
    UNUSED_PARAM(callerInstance);

    if (targetReference.isNull())
        WASM_THROW(Wasm::ExceptionType::NullReference);

    ASSERT(targetReference.isObject());
    JSObject* referenceAsObject = jsCast<JSObject*>(targetReference);

    ASSERT(referenceAsObject->inherits<WebAssemblyFunctionBase>());
    auto* wasmFunction = jsCast<WebAssemblyFunctionBase*>(referenceAsObject);
    auto& function = wasmFunction->importableFunction();
    JSWebAssemblyInstance* calleeInstance = wasmFunction->instance();

    Register& calleeStackSlot = partiallyConstructedCalleeFrame[static_cast<int>(CallFrameSlot::callee)];
    Register& functionInfoSlot = partiallyConstructedCalleeFrame[static_cast<int>(CallFrameSlot::codeBlock)];
    ASSERT(calleeStackSlot.unboxedInt64() == 0xBEEF);

    if (function.boxedWasmCalleeLoadLocation)
        calleeStackSlot = function.boxedWasmCalleeLoadLocation->encodedBits();
    else
        calleeStackSlot = CalleeBits::nullCallee().encodedBits();
    if (!function.targetInstance)
        functionInfoSlot = reinterpret_cast<uintptr_t>(wasmFunction->callLinkInfo());
    else
        functionInfoSlot = function.targetInstance.get();

    ASSERT(Wasm::isSubtypeIndex(function.typeIndex, CALLEE()->signature(typeIndex).index()));
    UNUSED_PARAM(typeIndex);
    auto callTarget = *function.entrypointLoadLocation;
    WASM_CALL_RETURN(calleeInstance, callTarget);
}

WASM_SLOW_PATH_DECL(call_ref)
{
    auto instruction = pc->as<WasmCallRef>();
    JSValue reference = JSValue::decode(READ(instruction.m_functionReference).encodedJSValue());
    Register* partiallyConstructedCalleeFrame = &callFrame->registers()[-safeCast<int>(instruction.m_stackOffset)];
    return resolveWasmCallRef(partiallyConstructedCalleeFrame, callFrame, instance, reference, instruction.m_typeIndex);
}

WASM_SLOW_PATH_DECL(tail_call)
{
    auto instruction = pc->as<WasmTailCall>();
    Register* partiallyConstructedCalleeFrame = &callFrame->registers()[-safeCast<int>(instruction.m_stackOffset)];
    return resolveWasmCall(partiallyConstructedCalleeFrame, instance, Wasm::FunctionSpaceIndex(instruction.m_functionIndex));
}

WASM_SLOW_PATH_DECL(tail_call_indirect)
{
    auto instruction = pc->as<WasmTailCallIndirect>();
    auto functionIndex = Wasm::FunctionSpaceIndex(READ(instruction.m_functionIndex).unboxedInt32());
    Register* partiallyConstructedCalleeFrame = &callFrame->registers()[-safeCast<int>(instruction.m_stackOffset)];
    return resolveWasmCallIndirect(partiallyConstructedCalleeFrame, callFrame, instance, functionIndex, instruction.m_tableIndex, instruction.m_signatureIndex);
}

WASM_SLOW_PATH_DECL(tail_call_ref)
{
    auto instruction = pc->as<WasmTailCallRef>();
    JSValue reference = JSValue::decode(READ(instruction.m_functionReference).encodedJSValue());
    Register* partiallyConstructedCalleeFrame = &callFrame->registers()[-safeCast<int>(instruction.m_stackOffset)];
    return resolveWasmCallRef(partiallyConstructedCalleeFrame, callFrame, instance, reference, instruction.m_typeIndex);
}

static size_t jsrSize()
{
    static size_t jsrSize = 0;
    static std::once_flag onceKey;
    std::call_once(onceKey, [&] {
        jsrSize = Wasm::wasmCallingConvention().jsrArgs.size();
    });
    return jsrSize;
}

WASM_SLOW_PATH_DECL(call_builtin)
{
    SlowPathFrameTracer tracer(instance->vm(), callFrame);

    auto instruction = pc->as<WasmCallBuiltin>();
    Register* stackBottom = callFrame->registers() - instruction.m_stackOffset;
    Register* stackStart = stackBottom + CallFrame::headerSizeInRegisters + /* indirect call target */ 1;
    Register* gprStart = stackStart + instruction.m_numberOfStackArgs;
    Register* fprStart = gprStart + jsrSize();
    UNUSED_PARAM(fprStart);

    Wasm::LLIntBuiltin builtin = static_cast<Wasm::LLIntBuiltin>(instruction.m_builtinIndex);

    unsigned gprIndex = 0;
    unsigned fprIndex = 0;
    unsigned stackIndex = 0;
    auto takeGPR = [&]() -> Register& {
        if (gprIndex != jsrSize())
            return gprStart[gprIndex++];
        return stackStart[stackIndex++];
    };
    UNUSED_PARAM(fprIndex);

    switch (builtin) {
    case Wasm::LLIntBuiltin::CurrentMemory: {
        size_t size = instance->memory()->memory().handle().size() >> 16;
        gprStart[0] = static_cast<EncodedJSValue>(size);
        WASM_END();
    }
    case Wasm::LLIntBuiltin::MemoryFill: {
        uint32_t dstAddress = takeGPR().unboxedUInt32();
        uint32_t targetValue = takeGPR().unboxedUInt32();
        uint32_t count = takeGPR().unboxedUInt32();
        if (!Wasm::memoryFill(instance, dstAddress, targetValue, count))
            WASM_THROW(Wasm::ExceptionType::OutOfBoundsMemoryAccess);
        WASM_END();
    }
    case Wasm::LLIntBuiltin::MemoryCopy: {
        uint32_t dstAddress = takeGPR().unboxedUInt32();
        uint32_t srcAddress = takeGPR().unboxedUInt32();
        uint32_t count = takeGPR().unboxedUInt32();
        if (!Wasm::memoryCopy(instance, dstAddress, srcAddress, count))
            WASM_THROW(Wasm::ExceptionType::OutOfBoundsMemoryAccess);
        WASM_END();
    }
    case Wasm::LLIntBuiltin::MemoryInit: {
        uint32_t dstAddress = takeGPR().unboxedUInt32();
        uint32_t srcAddress = takeGPR().unboxedUInt32();
        uint32_t length = takeGPR().unboxedUInt32();
        uint32_t dataSegmentIndex = takeGPR().unboxedUInt32();
        if (!Wasm::memoryInit(instance, dataSegmentIndex, dstAddress, srcAddress, length))
            WASM_THROW(Wasm::ExceptionType::OutOfBoundsMemoryAccess);
        WASM_END();
    }
    case Wasm::LLIntBuiltin::TableSize: {
        uint32_t tableIndex = takeGPR().unboxedUInt32();
        int32_t result = Wasm::tableSize(instance, tableIndex);
        gprStart[0] = static_cast<EncodedJSValue>(result);
        WASM_END();
    }
    case Wasm::LLIntBuiltin::TableCopy: {
        int32_t dstOffset = takeGPR().unboxedInt32();
        int32_t srcOffset = takeGPR().unboxedInt32();
        int32_t length = takeGPR().unboxedInt32();
        uint32_t dstTableIndex = takeGPR().unboxedUInt32();
        uint32_t srcTableIndex = takeGPR().unboxedUInt32();
        if (!Wasm::tableCopy(instance, dstTableIndex, srcTableIndex, dstOffset, srcOffset, length))
            WASM_THROW(Wasm::ExceptionType::OutOfBoundsTableAccess);
        WASM_END();
    }
    case Wasm::LLIntBuiltin::DataDrop: {
        uint32_t dataSegmentIndex = takeGPR().unboxedUInt32();
        Wasm::dataDrop(instance, dataSegmentIndex);
        WASM_END();
    }
    case Wasm::LLIntBuiltin::ElemDrop: {
        uint32_t elementIndex = takeGPR().unboxedUInt32();
        Wasm::elemDrop(instance, elementIndex);
        WASM_END();
    }
    case Wasm::LLIntBuiltin::RefTest:
    case Wasm::LLIntBuiltin::RefCast: {
        auto reference = takeGPR().encodedJSValue();
        bool allowNull = static_cast<bool>(takeGPR().unboxedInt32());
        int32_t heapType = takeGPR().unboxedInt32();
        bool shouldNegate = false;
        if (builtin == Wasm::LLIntBuiltin::RefTest)
            shouldNegate = takeGPR().unboxedInt32();
        Wasm::TypeIndex typeIndex;
        if (Wasm::typeIndexIsType(static_cast<Wasm::TypeIndex>(heapType)))
            typeIndex = static_cast<Wasm::TypeIndex>(heapType);
        else
            typeIndex = instance->module().moduleInformation().typeSignatures[heapType]->index();
        if (builtin == Wasm::LLIntBuiltin::RefTest) {
            bool result = Wasm::refCast(reference, allowNull, typeIndex);
            gprStart[0] = static_cast<uint32_t>((!shouldNegate || !result) && (shouldNegate || result));
        } else {
            if (!Wasm::refCast(reference, allowNull, typeIndex))
                WASM_THROW(Wasm::ExceptionType::CastFailure);
            gprStart[0] = reference;
        }
        WASM_END();
    }
    case Wasm::LLIntBuiltin::ArrayNewData: {
        uint32_t typeIndex = takeGPR().unboxedUInt32();
        uint32_t dataSegmentIndex = takeGPR().unboxedUInt32();
        uint32_t arraySize = takeGPR().unboxedUInt32();
        uint32_t offset = takeGPR().unboxedUInt32();

        EncodedJSValue result = Wasm::arrayNewData(instance, typeIndex, dataSegmentIndex, arraySize, offset);
        // arrayNewData returns false iff the segment access is out of bounds or allocation failed
        if (JSValue::decode(result).isNull())
            WASM_THROW(Wasm::ExceptionType::BadArrayNewInitData);
        gprStart[0] = static_cast<EncodedJSValue>(result);
        WASM_END();
    }
    case Wasm::LLIntBuiltin::ArrayNewElem: {
        uint32_t typeIndex = takeGPR().unboxedUInt32();
        uint32_t elemSegmentIndex = takeGPR().unboxedUInt32();
        uint32_t arraySize = takeGPR().unboxedUInt32();
        uint32_t offset = takeGPR().unboxedUInt32();

        EncodedJSValue result = Wasm::arrayNewElem(instance, typeIndex, elemSegmentIndex, arraySize, offset);
        // arrayNewElem returns null iff the segment access is out of bounds or allocation failed
        if (JSValue::decode(result).isNull())
            WASM_THROW(Wasm::ExceptionType::BadArrayNewInitElem);
        gprStart[0] = static_cast<EncodedJSValue>(result);
        WASM_END();
    }
    case Wasm::LLIntBuiltin::AnyConvertExtern: {
        auto reference = takeGPR().encodedJSValue();
        gprStart[0] = Wasm::externInternalize(reference);
        WASM_END();
    }
    case Wasm::LLIntBuiltin::ArrayCopy: {
        takeGPR().unboxedUInt32();
        EncodedJSValue dst = takeGPR().encodedJSValue();
        uint32_t dstOffset = takeGPR().unboxedUInt32();
        takeGPR().unboxedUInt32();
        EncodedJSValue src = takeGPR().encodedJSValue();
        uint32_t srcOffset = takeGPR().unboxedUInt32();
        uint32_t size = takeGPR().unboxedUInt32();

        if (JSValue::decode(dst).isNull() || JSValue::decode(src).isNull())
            WASM_THROW(Wasm::ExceptionType::NullArrayCopy);

        if (!Wasm::arrayCopy(instance, dst, dstOffset, src, srcOffset, size))
            WASM_THROW(Wasm::ExceptionType::OutOfBoundsArrayCopy);
        WASM_END();
    }
    case Wasm::LLIntBuiltin::ArrayInitElem: {
        takeGPR().unboxedUInt32();
        EncodedJSValue dst = takeGPR().encodedJSValue();
        uint32_t dstOffset = takeGPR().unboxedUInt32();
        uint32_t srcElementIndex = takeGPR().unboxedUInt32();
        uint32_t srcOffset = takeGPR().unboxedUInt32();
        uint32_t size = takeGPR().unboxedUInt32();

        if (JSValue::decode(dst).isNull())
            WASM_THROW(Wasm::ExceptionType::NullArrayInitElem);

        if (!Wasm::arrayInitElem(instance, dst, dstOffset, srcElementIndex, srcOffset, size))
            WASM_THROW(Wasm::ExceptionType::OutOfBoundsArrayInitElem);
        WASM_END();
    }
    case Wasm::LLIntBuiltin::ArrayInitData: {
        takeGPR().unboxedUInt32();
        EncodedJSValue dst = takeGPR().encodedJSValue();
        uint32_t dstOffset = takeGPR().unboxedUInt32();
        uint32_t srcDataIndex = takeGPR().unboxedUInt32();
        uint32_t srcOffset = takeGPR().unboxedUInt32();
        uint32_t size = takeGPR().unboxedUInt32();

        if (JSValue::decode(dst).isNull())
            WASM_THROW(Wasm::ExceptionType::NullArrayInitData);

        if (!Wasm::arrayInitData(instance, dst, dstOffset, srcDataIndex, srcOffset, size))
            WASM_THROW(Wasm::ExceptionType::OutOfBoundsArrayInitData);
        WASM_END();
    }
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
    WASM_END();
}

WASM_SLOW_PATH_DECL(set_global_ref)
{
    auto instruction = pc->as<WasmSetGlobalRef>();
    instance->setGlobal(instruction.m_globalIndex, READ(instruction.m_value).jsValue());
    WASM_END_IMPL();
}

WASM_SLOW_PATH_DECL(set_global_ref_portable_binding)
{
    auto instruction = pc->as<WasmSetGlobalRefPortableBinding>();
    instance->setGlobal(instruction.m_globalIndex, READ(instruction.m_value).jsValue());
    WASM_END_IMPL();
}

WASM_SLOW_PATH_DECL(memory_atomic_wait32)
{
    auto instruction = pc->as<WasmMemoryAtomicWait32>();
    unsigned base = READ(instruction.m_pointer).unboxedInt32();
    unsigned offset = instruction.m_offset;
    uint32_t value = READ(instruction.m_value).unboxedInt32();
    int64_t timeout = READ(instruction.m_timeout).unboxedInt64();
    int32_t result = Wasm::memoryAtomicWait32(instance, base, offset, value, timeout);
    if (result < 0)
        WASM_THROW(Wasm::ExceptionType::OutOfBoundsMemoryAccess);
    WASM_RETURN(result);
}

WASM_SLOW_PATH_DECL(memory_atomic_wait64)
{
    auto instruction = pc->as<WasmMemoryAtomicWait64>();
    unsigned base = READ(instruction.m_pointer).unboxedInt32();
    unsigned offset = instruction.m_offset;
    uint64_t value = READ(instruction.m_value).unboxedInt64();
    int64_t timeout = READ(instruction.m_timeout).unboxedInt64();
    int32_t result = Wasm::memoryAtomicWait64(instance, base, offset, value, timeout);
    if (result < 0)
        WASM_THROW(Wasm::ExceptionType::OutOfBoundsMemoryAccess);
    WASM_RETURN(result);
}

WASM_SLOW_PATH_DECL(memory_atomic_notify)
{
    auto instruction = pc->as<WasmMemoryAtomicNotify>();
    unsigned base = READ(instruction.m_pointer).unboxedInt32();
    unsigned offset = instruction.m_offset;
    int32_t count = READ(instruction.m_count).unboxedInt32();
    int32_t result = Wasm::memoryAtomicNotify(instance, base, offset, count);
    if (result < 0)
        WASM_THROW(Wasm::ExceptionType::OutOfBoundsMemoryAccess);
    WASM_RETURN(result);
}

WASM_SLOW_PATH_DECL(throw)
{
    SlowPathFrameTracer tracer(instance->vm(), callFrame);

    JSGlobalObject* globalObject = instance->globalObject();
    VM& vm = globalObject->vm();
    auto throwScope = DECLARE_THROW_SCOPE(vm);

    auto instruction = pc->as<WasmThrow>();
    Ref<const Wasm::Tag> tag = instance->tag(instruction.m_exceptionIndex);

    FixedVector<uint64_t> values(tag->parameterBufferSize());
    for (unsigned i = 0; i < tag->parameterBufferSize(); ++i)
        values[i] = READ((instruction.m_firstValue - i)).encodedJSValue();

    JSWebAssemblyException* exception = JSWebAssemblyException::create(vm, globalObject->webAssemblyExceptionStructure(), WTFMove(tag), WTFMove(values));
    throwException(globalObject, throwScope, exception);

    genericUnwind(vm, callFrame);
    ASSERT(!!vm.callFrameForCatch);
    ASSERT(!!vm.targetMachinePCForThrow);
    WASM_RETURN_TWO(vm.targetMachinePCForThrow, nullptr);
}

WASM_SLOW_PATH_DECL(rethrow)
{
    SlowPathFrameTracer tracer(instance->vm(), callFrame);

    JSGlobalObject* globalObject = instance->globalObject();
    VM& vm = globalObject->vm();
    auto throwScope = DECLARE_THROW_SCOPE(vm);

    auto instruction = pc->as<WasmRethrow>();
    JSValue exceptionValue = READ(instruction.m_exception).jsValue();

    JSValue thrownValue = exceptionValue;
    if (auto* exception = jsDynamicCast<JSWebAssemblyException*>(exceptionValue)) {
        if (&exception->tag() == &Wasm::Tag::jsExceptionTag())
            thrownValue = JSValue::decode(exception->payload().at(0));
    }

    throwException(globalObject, throwScope, thrownValue);

    genericUnwind(vm, callFrame);
    ASSERT(!!vm.callFrameForCatch);
    ASSERT(!!vm.targetMachinePCForThrow);
    WASM_RETURN_TWO(vm.targetMachinePCForThrow, nullptr);
}

WASM_SLOW_PATH_DECL(throw_ref)
{
    SlowPathFrameTracer tracer(instance->vm(), callFrame);

    JSGlobalObject* globalObject = instance->globalObject();
    VM& vm = globalObject->vm();
    auto throwScope = DECLARE_THROW_SCOPE(vm);

    auto instruction = pc->as<WasmThrowRef>();
    auto exceptionValue = READ(instruction.m_exception).jsValue();

    if (exceptionValue == jsNull())
        WASM_THROW(Wasm::ExceptionType::NullExnReference);

    auto* exception = jsCast<JSWebAssemblyException*>(exceptionValue);
    JSValue thrownValue = exceptionValue;
    if (&exception->tag() == &Wasm::Tag::jsExceptionTag())
        thrownValue = JSValue::decode(exception->payload().at(0));

    throwException(globalObject, throwScope, thrownValue);

    genericUnwind(vm, callFrame);
    ASSERT(!!vm.callFrameForCatch);
    ASSERT(!!vm.targetMachinePCForThrow);
    WASM_RETURN_TWO(vm.targetMachinePCForThrow, exception);
}

WASM_SLOW_PATH_DECL(retrieve_and_clear_exception)
{
    UNUSED_PARAM(callFrame);
    JSGlobalObject* globalObject = instance->globalObject();
    VM& vm = globalObject->vm();
    auto throwScope = DECLARE_THROW_SCOPE(vm);

    RELEASE_ASSERT(!!throwScope.exception());

    Exception* exception = throwScope.exception();
    JSValue thrownValue = exception->value();
    void* payload = nullptr;

    const auto& handleCatchAll = [&](const auto& instruction) {
        callFrame->uncheckedR(instruction.m_exception) = thrownValue;
    };

    const auto& handleCatch = [&](const auto& instruction) {
        JSWebAssemblyException* wasmException = jsDynamicCast<JSWebAssemblyException*>(thrownValue);
        RELEASE_ASSERT(!!wasmException);
        payload = std::bit_cast<void*>(wasmException->payload().span().data());
        callFrame->uncheckedR(instruction.m_exception) = thrownValue;
    };

    if (pc->is<WasmCatch>())
        handleCatch(pc->as<WasmCatch>());
    else if (pc->is<WasmCatchAll>())
        handleCatchAll(pc->as<WasmCatchAll>());
    else if (pc->is<WasmTryTableCatch>()) {
        JSWebAssemblyException* wasmException = jsDynamicCast<JSWebAssemblyException*>(thrownValue);
        RELEASE_ASSERT(!!wasmException);
        payload = std::bit_cast<void*>(wasmException->payload().span().data());
        auto instr = pc->as<WasmTryTableCatch>();
        if (instr.m_kind == static_cast<unsigned>(Wasm::CatchKind::CatchRef) || instr.m_kind == static_cast<unsigned>(Wasm::CatchKind::CatchAllRef))
            callFrame->uncheckedR(pc->as<WasmTryTableCatch>().m_exception) = thrownValue;
    }

    // We want to clear the exception here rather than in the catch prologue
    // JIT code because clearing it also entails clearing a bit in an Atomic
    // bit field in VMTraps.
    throwScope.clearException();
    WASM_RETURN_TWO(pc, payload);
}

#if USE(JSVALUE32_64)
WASM_SLOW_PATH_DECL(f32_ceil)
{
    UNUSED_PARAM(instance);
    auto instruction = pc->as<WasmF32Ceil>();
    float operand = READ(instruction.m_operand).unboxedFloat();
    WASM_RETURN(JSValue::encode(wasmUnboxedFloat(std::ceil(operand))));
}

WASM_SLOW_PATH_DECL(f32_floor)
{
    UNUSED_PARAM(instance);
    auto instruction = pc->as<WasmF32Floor>();
    float operand = READ(instruction.m_operand).unboxedFloat();
    WASM_RETURN(JSValue::encode(wasmUnboxedFloat(std::floor(operand))));
}

WASM_SLOW_PATH_DECL(f32_trunc)
{
    UNUSED_PARAM(instance);
    auto instruction = pc->as<WasmF32Trunc>();
    float operand = READ(instruction.m_operand).unboxedFloat();
    WASM_RETURN(JSValue::encode(wasmUnboxedFloat(std::trunc(operand))));
}

WASM_SLOW_PATH_DECL(f32_nearest)
{
    static_assert(std::numeric_limits<float>::round_style == std::round_to_nearest);
    UNUSED_PARAM(instance);
    auto instruction = pc->as<WasmF32Nearest>();
    float operand = READ(instruction.m_operand).unboxedFloat();
    WASM_RETURN(JSValue::encode(wasmUnboxedFloat(std::nearbyint(operand))));
}

WASM_SLOW_PATH_DECL(f64_ceil)
{
    UNUSED_PARAM(instance);
    auto instruction = pc->as<WasmF64Ceil>();
    double operand = READ(instruction.m_operand).unboxedDouble();
    WASM_RETURN(JSValue::encode(jsDoubleNumber(std::ceil(operand))));
}

WASM_SLOW_PATH_DECL(f64_floor)
{
    UNUSED_PARAM(instance);
    auto instruction = pc->as<WasmF64Floor>();
    double operand = READ(instruction.m_operand).unboxedDouble();
    WASM_RETURN(JSValue::encode(jsDoubleNumber(std::floor(operand))));
}

WASM_SLOW_PATH_DECL(f64_trunc)
{
    UNUSED_PARAM(instance);
    auto instruction = pc->as<WasmF64Trunc>();
    double operand = READ(instruction.m_operand).unboxedDouble();
    WASM_RETURN(JSValue::encode(jsDoubleNumber(std::trunc(operand))));
}

WASM_SLOW_PATH_DECL(f64_nearest)
{
    static_assert(std::numeric_limits<float>::round_style == std::round_to_nearest);
    UNUSED_PARAM(instance);
    auto instruction = pc->as<WasmF64Nearest>();
    double operand = READ(instruction.m_operand).unboxedDouble();
    WASM_RETURN(JSValue::encode(jsDoubleNumber(std::nearbyint(operand))));
}

WASM_SLOW_PATH_DECL(f32_convert_u_i64)
{
    static_assert(std::numeric_limits<float>::round_style == std::round_to_nearest);
    UNUSED_PARAM(instance);
    auto instruction = pc->as<WasmF32ConvertUI64>();
    uint64_t operand = READ(instruction.m_operand).unboxedInt64();
    WASM_RETURN(JSValue::encode(wasmUnboxedFloat(static_cast<float>(operand))));
}

WASM_SLOW_PATH_DECL(f32_convert_s_i64)
{
    static_assert(std::numeric_limits<float>::round_style == std::round_to_nearest);
    UNUSED_PARAM(instance);
    auto instruction = pc->as<WasmF32ConvertSI64>();
    int64_t operand = READ(instruction.m_operand).unboxedInt64();
    WASM_RETURN(JSValue::encode(wasmUnboxedFloat(static_cast<float>(operand))));
}

WASM_SLOW_PATH_DECL(f64_convert_u_i64)
{
    static_assert(std::numeric_limits<float>::round_style == std::round_to_nearest);
    UNUSED_PARAM(instance);
    auto instruction = pc->as<WasmF64ConvertUI64>();
    uint64_t operand = READ(instruction.m_operand).unboxedInt64();
    WASM_RETURN(JSValue::encode(jsDoubleNumber(static_cast<double>(operand))));
}

WASM_SLOW_PATH_DECL(f64_convert_s_i64)
{
    static_assert(std::numeric_limits<float>::round_style == std::round_to_nearest);
    UNUSED_PARAM(instance);
    auto instruction = pc->as<WasmF64ConvertSI64>();
    int64_t operand = READ(instruction.m_operand).unboxedInt64();
    WASM_RETURN(JSValue::encode(jsDoubleNumber(static_cast<double>(operand))));
}

WASM_SLOW_PATH_DECL(i64_trunc_u_f32)
{
    UNUSED_PARAM(instance);
    auto instruction = pc->as<WasmI64TruncUF32>();
    float operand = READ(instruction.m_operand).unboxedFloat();
    if (std::isnan(operand) || operand <= -1.0f || operand >= -2.0f * static_cast<float>(INT64_MIN))
        WASM_THROW(Wasm::ExceptionType::OutOfBoundsTrunc);
    WASM_RETURN(static_cast<uint64_t>(operand));
}

WASM_SLOW_PATH_DECL(i64_trunc_s_f32)
{
    UNUSED_PARAM(instance);
    auto instruction = pc->as<WasmI64TruncSF32>();
    float operand = READ(instruction.m_operand).unboxedFloat();
    if (std::isnan(operand) || operand < static_cast<float>(INT64_MIN) || operand >= -static_cast<float>(INT64_MIN))
        WASM_THROW(Wasm::ExceptionType::OutOfBoundsTrunc);
    WASM_RETURN(static_cast<int64_t>(operand));
}

WASM_SLOW_PATH_DECL(i64_trunc_u_f64)
{
    UNUSED_PARAM(instance);
    auto instruction = pc->as<WasmI64TruncUF64>();
    double operand = READ(instruction.m_operand).unboxedDouble();
    if (std::isnan(operand) || operand <= -1.0 || operand >= -2.0 * static_cast<double>(INT64_MIN))
        WASM_THROW(Wasm::ExceptionType::OutOfBoundsTrunc);
    WASM_RETURN(static_cast<uint64_t>(operand));
}

WASM_SLOW_PATH_DECL(i64_trunc_s_f64)
{
    UNUSED_PARAM(instance);
    auto instruction = pc->as<WasmI64TruncSF64>();
    double operand = READ(instruction.m_operand).unboxedDouble();
    if (std::isnan(operand) || operand < static_cast<double>(INT64_MIN) || operand >= -static_cast<double>(INT64_MIN))
        WASM_THROW(Wasm::ExceptionType::OutOfBoundsTrunc);
    WASM_RETURN(static_cast<int64_t>(operand));
}

WASM_SLOW_PATH_DECL(i64_trunc_sat_f32_u)
{
    UNUSED_PARAM(instance);
    auto instruction = pc->as<WasmI64TruncSatF32U>();
    float operand = READ(instruction.m_operand).unboxedFloat();
    uint64_t result;
    if (std::isnan(operand) || operand <= -1.0f)
        result = 0;
    else if (operand >= -2.0f * static_cast<float>(INT64_MIN))
        result = UINT64_MAX;
    else
        result = static_cast<uint64_t>(operand);
    WASM_RETURN(result);
}

WASM_SLOW_PATH_DECL(i64_trunc_sat_f32_s)
{
    UNUSED_PARAM(instance);
    auto instruction = pc->as<WasmI64TruncSatF32S>();
    float operand = READ(instruction.m_operand).unboxedFloat();
    int64_t result;
    if (std::isnan(operand))
        result = 0;
    else if (operand < static_cast<float>(INT64_MIN))
        result = INT64_MIN;
    else if (operand >= -static_cast<float>(INT64_MIN))
        result = INT64_MAX;
    else
        result = static_cast<int64_t>(operand);
    WASM_RETURN(result);
}

WASM_SLOW_PATH_DECL(i64_trunc_sat_f64_u)
{
    UNUSED_PARAM(instance);
    auto instruction = pc->as<WasmI64TruncSatF64U>();
    double operand = READ(instruction.m_operand).unboxedDouble();
    uint64_t result;
    if (std::isnan(operand) || operand <= -1.0)
        result = 0;
    else if (operand >= -2.0 * static_cast<double>(INT64_MIN))
        result = UINT64_MAX;
    else
        result = static_cast<uint64_t>(operand);
    WASM_RETURN(result);
}

WASM_SLOW_PATH_DECL(i64_trunc_sat_f64_s)
{
    UNUSED_PARAM(instance);
    auto instruction = pc->as<WasmI64TruncSatF64S>();
    double operand = READ(instruction.m_operand).unboxedDouble();
    int64_t result;
    if (std::isnan(operand))
        result = 0;
    else if (operand < static_cast<double>(INT64_MIN))
        result = INT64_MIN;
    else if (operand >= -static_cast<double>(INT64_MIN))
        result = INT64_MAX;
    else
        result = static_cast<int64_t>(operand);
    WASM_RETURN(result);
}
#endif

extern "C" UGPRPair SYSV_ABI slow_path_wasm_throw_exception(CallFrame* callFrame, JSWebAssemblyInstance* instance, Wasm::ExceptionType exceptionType)
{
    SlowPathFrameTracer tracer(instance->vm(), callFrame);
#if ENABLE(WEBASSEMBLY_BBQJIT)
    void* pc = instance->faultPC();
    instance->setFaultPC(nullptr);
    auto* callee = callFrame->callee().asNativeCallee();
    ASSERT(callee->category() == NativeCallee::Category::Wasm);
    auto& wasmCallee = static_cast<Wasm::Callee&>(*callee);
    if (isAnyOMG(wasmCallee.compilationMode())) {
        if (auto callSiteIndexFromPC = static_cast<Wasm::OptimizingJITCallee&>(wasmCallee).tryGetCallSiteIndex(pc))
            callFrame->setCallSiteIndex(callSiteIndexFromPC.value());
    }
#endif
    WASM_RETURN_TWO(Wasm::throwWasmToJSException(callFrame, exceptionType, instance), nullptr);
}

extern "C" UGPRPair SYSV_ABI slow_path_wasm_popcount(const WasmInstruction* pc, uint32_t x)
{
    void* result = std::bit_cast<void*>(static_cast<size_t>(std::popcount(x)));
    WASM_RETURN_TWO(pc, result);
}

extern "C" UGPRPair SYSV_ABI slow_path_wasm_popcountll(const WasmInstruction* pc, uint64_t x)
{
    void* result = std::bit_cast<void*>(static_cast<size_t>(std::popcount(x)));
    WASM_RETURN_TWO(pc, result);
}

} } // namespace JSC::LLInt

#endif // ENABLE(WEBASSEMBLY)

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

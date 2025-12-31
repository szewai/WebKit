/*
 * Copyright (C) 2017 Oleksandr Skachkov <gskachkov@gmail.com>.
 * Copyright (C) 2019 Apple Inc. All rights reserved.
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

@linkTimeConstant
function asyncGeneratorResumeNext(generator)
{
    "use strict";

    @assert(@isAsyncGenerator(generator), "Generator is not an AsyncGenerator instance.");

    while (true) {
        var state = @getAsyncGeneratorInternalField(generator, @generatorFieldState);

        @assert(state !== @AsyncGeneratorStateExecuting, "Async generator should not be in executing state");

        if (state === @AsyncGeneratorStateAwaitingReturn)
            return;

        var resumeMode = @getAsyncGeneratorInternalField(generator, @asyncGeneratorFieldResumeMode);
        if (resumeMode === @AsyncGeneratorResumeModeEmpty)
            return;

        var resumeValue = @getAsyncGeneratorInternalField(generator, @asyncGeneratorFieldResumeValue);

        if (resumeMode !== @GeneratorResumeModeNormal) {
            if (state === @AsyncGeneratorStateSuspendedStart) {
                @putAsyncGeneratorInternalField(generator, @generatorFieldState, @AsyncGeneratorStateCompleted);
                state = @AsyncGeneratorStateCompleted;
            }

            if (state === @AsyncGeneratorStateCompleted) {
                if (resumeMode === @GeneratorResumeModeReturn) {
                    @putAsyncGeneratorInternalField(generator, @generatorFieldState, @AsyncGeneratorStateAwaitingReturn);
                    @resolveWithInternalMicrotaskForAsyncAwait(resumeValue, @InternalMicrotaskAsyncGeneratorResumeNext, generator);
                    return;
                }

                @assert(resumeMode === @GeneratorResumeModeThrow, "Async generator has wrong mode");


                @asyncGeneratorQueueDequeueReject(generator, resumeValue);
                continue;
            }
        } else if (state === @AsyncGeneratorStateCompleted) {
            @asyncGeneratorQueueDequeueResolve(generator, { value: @undefined, done: true });
            continue;
        }

        var suspendReason = @getAsyncGeneratorInternalField(generator, @asyncGeneratorFieldSuspendReason);
        if (resumeMode === @GeneratorResumeModeReturn && ((state > 0 && suspendReason === @AsyncGeneratorSuspendReasonYield) || state === @AsyncGeneratorStateSuspendedYield)) {
            @putAsyncGeneratorInternalField(generator, @asyncGeneratorFieldSuspendReason, @AsyncGeneratorSuspendReasonAwait);
            return @resolveWithInternalMicrotaskForAsyncAwait(resumeValue, @InternalMicrotaskAsyncGeneratorBodyCallReturn, generator);
        }

        var value = @undefined;

        @putAsyncGeneratorInternalField(generator, @generatorFieldState, @AsyncGeneratorStateExecuting);
        @putAsyncGeneratorInternalField(generator, @asyncGeneratorFieldSuspendReason, @AsyncGeneratorSuspendReasonNone);

        try {
            value = @getAsyncGeneratorInternalField(generator, @generatorFieldNext).@call(@getAsyncGeneratorInternalField(generator, @generatorFieldThis), generator, state, resumeValue, resumeMode, @getAsyncGeneratorInternalField(generator, @generatorFieldFrame));
            state = @getAsyncGeneratorInternalField(generator, @generatorFieldState);
            if (state === @AsyncGeneratorStateExecuting) {
                @putAsyncGeneratorInternalField(generator, @generatorFieldState, @AsyncGeneratorStateCompleted);
                state = @AsyncGeneratorStateCompleted;
            }
        } catch (error) {
            @putAsyncGeneratorInternalField(generator, @generatorFieldState, @AsyncGeneratorStateCompleted);
            @putAsyncGeneratorInternalField(generator, @asyncGeneratorFieldSuspendReason, @AsyncGeneratorSuspendReasonNone);

            @asyncGeneratorQueueDequeueReject(generator, error);
            continue;
        }

        var reason = @getAsyncGeneratorInternalField(generator, @asyncGeneratorFieldSuspendReason);
        if (reason === @AsyncGeneratorSuspendReasonAwait)
            return @resolveWithInternalMicrotaskForAsyncAwait(value, @InternalMicrotaskAsyncGeneratorBodyCallNormal, generator);

        if (reason === @AsyncGeneratorSuspendReasonYield) {
            @putAsyncGeneratorInternalField(generator, @asyncGeneratorFieldSuspendReason, @AsyncGeneratorSuspendReasonAwait);
            return @resolveWithInternalMicrotaskForAsyncAwait(value, @InternalMicrotaskAsyncGeneratorYieldAwaited, generator);
        }

        if (state === @AsyncGeneratorStateCompleted) {
            @assert(@getAsyncGeneratorInternalField(generator, @generatorFieldState) == @AsyncGeneratorStateCompleted);
            @putAsyncGeneratorInternalField(generator, @asyncGeneratorFieldSuspendReason, @AsyncGeneratorSuspendReasonNone);
            @asyncGeneratorQueueDequeueResolve(generator, { value, done: true });
            continue;
        }
        return;
    }
}

function next(value)
{
    "use strict";

    var promise = @newPromise();

    if (@asyncGeneratorQueueEnqueue(this, value, @GeneratorResumeModeNormal, promise))
        @asyncGeneratorResumeNext(this);

    return promise;
}

function return(value)
{
    "use strict";

    var promise = @newPromise();

    if (@asyncGeneratorQueueEnqueue(this, value, @GeneratorResumeModeReturn, promise))
        @asyncGeneratorResumeNext(this);

    return promise;
}

function throw(value)
{
    "use strict";

    var promise = @newPromise();

    if (@asyncGeneratorQueueEnqueue(this, value, @GeneratorResumeModeThrow, promise))
        @asyncGeneratorResumeNext(this);

    return promise;
}

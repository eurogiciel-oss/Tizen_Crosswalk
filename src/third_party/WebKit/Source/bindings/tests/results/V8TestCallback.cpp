/*
 * Copyright (C) 2013 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

// This file has been auto-generated by code_generator_v8.pm. DO NOT MODIFY!

#include "config.h"
#include "V8TestCallback.h"

#include "V8TestObject.h"
#include "bindings/v8/V8Binding.h"
#include "bindings/v8/V8Callback.h"
#include "core/dom/ExecutionContext.h"
#include "wtf/Assertions.h"
#include "wtf/GetPtr.h"
#include "wtf/RefPtr.h"
namespace WebCore {

V8TestCallback::V8TestCallback(v8::Handle<v8::Object> callback, ExecutionContext* context)
    : ActiveDOMCallback(context)
    , m_callback(toIsolate(context), callback)
    , m_world(DOMWrapperWorld::current())
{
}

V8TestCallback::~V8TestCallback()
{
}

// Functions

bool V8TestCallback::callbackWithNoParam()
{
    if (!canInvokeCallback())
        return true;

    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    v8::HandleScope handleScope(isolate);

    v8::Handle<v8::Context> v8Context = toV8Context(executionContext(), m_world.get());
    if (v8Context.IsEmpty())
        return true;

    v8::Context::Scope scope(v8Context);


    v8::Handle<v8::Value> *argv = 0;

    bool callbackReturnValue = false;
    return !invokeCallback(m_callback.newLocal(isolate), 0, argv, callbackReturnValue, executionContext(), isolate);
}

bool V8TestCallback::callbackWithTestObjectParam(TestObj* class1Param)
{
    if (!canInvokeCallback())
        return true;

    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    v8::HandleScope handleScope(isolate);

    v8::Handle<v8::Context> v8Context = toV8Context(executionContext(), m_world.get());
    if (v8Context.IsEmpty())
        return true;

    v8::Context::Scope scope(v8Context);

    v8::Handle<v8::Value> class1ParamHandle = toV8(class1Param, v8::Handle<v8::Object>(), isolate);
    if (class1ParamHandle.IsEmpty()) {
        if (!isScriptControllerTerminating())
            CRASH();
        return true;
    }

    v8::Handle<v8::Value> argv[] = {
        class1ParamHandle
    };

    bool callbackReturnValue = false;
    return !invokeCallback(m_callback.newLocal(isolate), 1, argv, callbackReturnValue, executionContext(), isolate);
}

bool V8TestCallback::callbackWithTestObjectParam(TestObj* class2Param, const String& strArg)
{
    if (!canInvokeCallback())
        return true;

    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    v8::HandleScope handleScope(isolate);

    v8::Handle<v8::Context> v8Context = toV8Context(executionContext(), m_world.get());
    if (v8Context.IsEmpty())
        return true;

    v8::Context::Scope scope(v8Context);

    v8::Handle<v8::Value> class2ParamHandle = toV8(class2Param, v8::Handle<v8::Object>(), isolate);
    if (class2ParamHandle.IsEmpty()) {
        if (!isScriptControllerTerminating())
            CRASH();
        return true;
    }
    v8::Handle<v8::Value> strArgHandle = v8String(strArg, isolate);
    if (strArgHandle.IsEmpty()) {
        if (!isScriptControllerTerminating())
            CRASH();
        return true;
    }

    v8::Handle<v8::Value> argv[] = {
        class2ParamHandle,
        strArgHandle
    };

    bool callbackReturnValue = false;
    return !invokeCallback(m_callback.newLocal(isolate), 2, argv, callbackReturnValue, executionContext(), isolate);
}

bool V8TestCallback::callbackWithBoolean(bool boolParam)
{
    if (!canInvokeCallback())
        return true;

    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    v8::HandleScope handleScope(isolate);

    v8::Handle<v8::Context> v8Context = toV8Context(executionContext(), m_world.get());
    if (v8Context.IsEmpty())
        return true;

    v8::Context::Scope scope(v8Context);

    v8::Handle<v8::Value> boolParamHandle = v8Boolean(boolParam, isolate);
    if (boolParamHandle.IsEmpty()) {
        if (!isScriptControllerTerminating())
            CRASH();
        return true;
    }

    v8::Handle<v8::Value> argv[] = {
        boolParamHandle
    };

    bool callbackReturnValue = false;
    return !invokeCallback(m_callback.newLocal(isolate), 1, argv, callbackReturnValue, executionContext(), isolate);
}

bool V8TestCallback::callbackWithSequence(const Vector<RefPtr<TestObj> >& sequenceParam)
{
    if (!canInvokeCallback())
        return true;

    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    v8::HandleScope handleScope(isolate);

    v8::Handle<v8::Context> v8Context = toV8Context(executionContext(), m_world.get());
    if (v8Context.IsEmpty())
        return true;

    v8::Context::Scope scope(v8Context);

    v8::Handle<v8::Value> sequenceParamHandle = v8Array(sequenceParam, isolate);
    if (sequenceParamHandle.IsEmpty()) {
        if (!isScriptControllerTerminating())
            CRASH();
        return true;
    }

    v8::Handle<v8::Value> argv[] = {
        sequenceParamHandle
    };

    bool callbackReturnValue = false;
    return !invokeCallback(m_callback.newLocal(isolate), 1, argv, callbackReturnValue, executionContext(), isolate);
}

bool V8TestCallback::callbackWithFloat(float floatParam)
{
    if (!canInvokeCallback())
        return true;

    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    v8::HandleScope handleScope(isolate);

    v8::Handle<v8::Context> v8Context = toV8Context(executionContext(), m_world.get());
    if (v8Context.IsEmpty())
        return true;

    v8::Context::Scope scope(v8Context);

    v8::Handle<v8::Value> floatParamHandle = v8::Number::New(isolate, floatParam);
    if (floatParamHandle.IsEmpty()) {
        if (!isScriptControllerTerminating())
            CRASH();
        return true;
    }

    v8::Handle<v8::Value> argv[] = {
        floatParamHandle
    };

    bool callbackReturnValue = false;
    return !invokeCallback(m_callback.newLocal(isolate), 1, argv, callbackReturnValue, executionContext(), isolate);
}

bool V8TestCallback::callbackWithThisArg(ScriptValue thisValue, int param)
{
    if (!canInvokeCallback())
        return true;

    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    v8::HandleScope handleScope(isolate);

    v8::Handle<v8::Context> v8Context = toV8Context(executionContext(), m_world.get());
    if (v8Context.IsEmpty())
        return true;

    v8::Context::Scope scope(v8Context);

    v8::Handle<v8::Value> thisHandle = thisValue.v8Value();
    if (thisHandle.IsEmpty()) {
        if (!isScriptControllerTerminating())
            CRASH();
        return true;
    }
    ASSERT(thisHandle->IsObject());
    v8::Handle<v8::Value> paramHandle = v8::Integer::New(param, isolate);
    if (paramHandle.IsEmpty()) {
        if (!isScriptControllerTerminating())
            CRASH();
        return true;
    }

    v8::Handle<v8::Value> argv[] = {
        paramHandle
    };

    bool callbackReturnValue = false;
    return !invokeCallback(m_callback.newLocal(isolate), v8::Handle<v8::Object>::Cast(thisHandle), 1, argv, callbackReturnValue, executionContext(), isolate);
}

} // namespace WebCore

/*
 * Copyright (C) 2019 ProSiebenSat1.Digital GmbH.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "Deferred.h"

#include "JsBridgeContext.h"
#include "QuickJsUtils.h"
#include "log.h"
#include "jni-helpers/JniContext.h"
#include "../JavaTypeId.h"

namespace {
  const char *PROMISE_OBJECT_GLOBAL_NAME_PREFIX = "__javaTypes_deferred_promiseobject_";

  struct OnPromisePayload {
    JniGlobalRef<jobject> javaDeferred;
    std::shared_ptr<const JavaType> componentType;
  };

  JSValue onPromiseFulfilled(JSContext *ctx, JSValueConst /*this_val*/, int argc, JSValueConst *argv, int /*magic*/, JSValueConst *datav) {
    JsBridgeContext *jsBridgeContext = JsBridgeContext::getInstance(ctx);
    assert(jsBridgeContext != nullptr);

    JniContext *jniContext = jsBridgeContext->jniContext();

    // data => OnPromisePayload
    auto payload = QuickJsUtils::getCppPtr<OnPromisePayload>(*datav);

    // Pop promise value
    JSValueConst promiseValue = argc >= 1 ? *argv : JS_NULL;
    JValue value = payload->componentType->toJava(promiseValue, true);

    // Complete the native Deferred
    jniContext->callJsBridgeVoidMethod("resolveDeferred",
                                       "(Lkotlinx/coroutines/CompletableDeferred;Ljava/lang/Object;)V",
                                       payload->javaDeferred, value);
    if (jsBridgeContext->hasPendingJniException()) {
      jsBridgeContext->rethrowJniException();
      return JS_EXCEPTION;
    }

    return JS_UNDEFINED;
  }

  JSValue onPromiseRejected(JSContext *ctx, JSValueConst /*this_val*/, int argc, JSValueConst *argv, int /*magic*/, JSValueConst *datav) {
    JsBridgeContext *jsBridgeContext = JsBridgeContext::getInstance(ctx);
    assert(jsBridgeContext != nullptr);

    JniContext *jniContext = jsBridgeContext->jniContext();

    // data => OnPromisePayload
    auto payload = QuickJsUtils::getCppPtr<OnPromisePayload>(*datav);

    JS_Throw(ctx, argc > 0 ? JS_DupValue(ctx, *argv) : JS_NULL);
    JValue value(jsBridgeContext->getJavaExceptionForJsError());

    // Reject the native Deferred
    jniContext->callJsBridgeVoidMethod("rejectDeferred", "(Lkotlinx/coroutines/CompletableDeferred;Lde/prosiebensat1digital/oasisjsbridge/JsException;)V", payload->javaDeferred, value);
    if (jsBridgeContext->hasPendingJniException()) {
      jsBridgeContext->rethrowJniException();
      return JS_EXCEPTION;
    }

    return JS_UNDEFINED;
  }

  // Add resolve and reject to bound PromiseObject instance
  JSValue promiseFunction(JSContext *ctx, JSValueConst /*this_val*/, int argc, JSValueConst *argv, int /*magic*/, JSValueConst *datav) {
    JsBridgeContext *jsBridgeContext = JsBridgeContext::getInstance(ctx);
    assert(jsBridgeContext != nullptr);

    JSValueConst promiseObject = *datav;

    // Set PromiseObject.resolve and PromiseObject.reject
    JS_SetPropertyStr(ctx, promiseObject, "resolve", argc >= 1 ? JS_DupValue(ctx, argv[0]) : JS_NULL);
    JS_SetPropertyStr(ctx, promiseObject, "reject", argc >= 2 ? JS_DupValue(ctx, argv[1]) : JS_NULL);

    return JS_UNDEFINED;
  }
}


namespace JavaTypes {

// static
const char *Deferred::PROMISE_COMPONENT_TYPE_PROP_NAME = "\xff\xff" "promise_type";

Deferred::Deferred(const JsBridgeContext *jsBridgeContext, std::unique_ptr<const JavaType> &&componentType)
 : JavaType(jsBridgeContext, JavaTypeId::Deferred)
 , m_componentType(std::move(componentType)) {
}

// JS Promise to native Deferred
JValue Deferred::toJava(JSValueConst v, bool inScript) const {
  const QuickJsUtils *utils = m_jsBridgeContext->getUtils();
  assert(utils != nullptr);

  // Create a native Deferred instance
  JniLocalRef<jobject> javaDeferred =
      m_jniContext->callJsBridgeObjectMethod("createCompletableDeferred", "()Lkotlinx/coroutines/CompletableDeferred;");
  if (m_jsBridgeContext->hasPendingJniException()) {
    m_jsBridgeContext->rethrowJniException();
    return JValue();
  }

  bool isPromise = JS_IsObject(v) && utils->hasPropertyStr(v, "then");
  if (!isPromise) {
    // Not a Promise => directly resolve the native Deferred with the value
    JValue value = m_componentType->toJava(v, inScript);

    m_jniContext->callJsBridgeVoidMethod("resolveDeferred",
                                         "(Lkotlinx/coroutines/CompletableDeferred;Ljava/lang/Object;)V",
                                         javaDeferred, value);
    if (m_jsBridgeContext->hasPendingJniException()) {
      m_jsBridgeContext->rethrowJniException();
      return JValue();
    }

    return JValue(javaDeferred);
  }

  // onPromiseFulfilledFunc with data
  auto onPromiseFulfilledPayload = new OnPromisePayload { JniGlobalRef<jobject>(javaDeferred), m_componentType };
  JSValue onPromiseFulfilledPayloadValue = utils->createCppPtrValue<OnPromisePayload>(onPromiseFulfilledPayload, true);
  JSValue onPromiseFulfilledValue = JS_NewCFunctionData(
      m_ctx, onPromiseFulfilled, 1 /*length*/, 0 /*magic*/, 1, &onPromiseFulfilledPayloadValue);
  JS_FreeValue(m_ctx, onPromiseFulfilledPayloadValue);

  // onPromiseRejectedFunc with data
  auto onPromiseRejectedPayload = new OnPromisePayload { JniGlobalRef<jobject>(javaDeferred), m_componentType };
  JSValue onPromiseRejectedPayloadValue = utils->createCppPtrValue<OnPromisePayload>(onPromiseRejectedPayload, true);
  JSValue onPromiseRejectedValue = JS_NewCFunctionData(
      m_ctx, onPromiseRejected, 1 /*length*/, 0 /*magic*/, 1, &onPromiseRejectedPayloadValue);
  JS_FreeValue(m_ctx, onPromiseRejectedPayloadValue);

  // JsPromise.then()
  JSValue thenValue = JS_GetPropertyStr(m_ctx, v, "then");
  assert(JS_IsFunction(m_ctx, thenValue));

  // Call JsPromise.then(onPromiseFulfilled, onPromiseRejected)
  JSValueConst thenArgs[2];
  thenArgs[0] = onPromiseFulfilledValue;
  thenArgs[1] = onPromiseRejectedValue;
  JSValue ret = JS_Call(m_ctx, thenValue, v, 2, thenArgs);

  if (JS_IsException(ret)) {
    alog("Error while calling JSPromise.then()");

    JniLocalRef<jthrowable> javaException = m_jsBridgeContext->getJavaExceptionForJsError();
    m_jniContext->callJsBridgeVoidMethod("rejectDeferred",
                                         "(Lkotlinx/coroutines/CompletableDeferred;Ljava/lang/Object;)V",
                                         javaDeferred, javaException);
    if (m_jsBridgeContext->hasPendingJniException()) {
      m_jsBridgeContext->rethrowJniException();
      return JValue();
    }
  }

  JS_FreeValue(m_ctx, ret);
  JS_FreeValue(m_ctx, onPromiseFulfilledValue);
  JS_FreeValue(m_ctx, onPromiseRejectedValue);
  JS_FreeValue(m_ctx, thenValue);

  return JValue(javaDeferred);
}

// Native Deferred to JS Promise
JSValue Deferred::fromJava(const JValue &value, bool inScript) const {
  const JniLocalRef<jobject> &jDeferred = value.getLocalRef();

  if (jDeferred.isNull()) {
    return JS_NULL;
  }

  const QuickJsUtils *utils = m_jsBridgeContext->getUtils();

  // Create a PromiseObject which will be eventually filled with {resolve, reject}
  JSValue promiseObject = JS_NewObject(m_ctx);
  JSValue componentTypeValue = utils->createCppPtrValue(new std::shared_ptr<const JavaType>(m_componentType), true /*deleteOnFinalize*/);
  JS_SetPropertyStr(m_ctx, promiseObject, PROMISE_COMPONENT_TYPE_PROP_NAME, componentTypeValue);
  // No JS_FreeValue(m_ctx, componentTypeValue) after JS_SetPropertyStr()

  static int promiseCount = 0;
  std::string promiseObjectGlobalName = PROMISE_OBJECT_GLOBAL_NAME_PREFIX + std::to_string(++promiseCount);

  // Put it to the global stash
  JSValue globalObj = JS_GetGlobalObject(m_ctx);
  JS_SetPropertyStr(m_ctx, globalObj, promiseObjectGlobalName.c_str(), JS_DupValue(m_ctx, promiseObject));
  JS_FreeValue(m_ctx, globalObj);

  // promiseFunction = function(resolve, reject) + data (promiseObject)
  JSValue promiseFunctionValue = JS_NewCFunctionData(m_ctx, promiseFunction, 1, 0, 1, &promiseObject);
  JS_FreeValue(m_ctx, promiseObject);

  // Create a new JS promise with the promiseFunction as parameter
  // => new Promise(promiseFunction)
  JSValue promiseCtor = JS_GetPropertyStr(m_ctx, globalObj, "Promise");
  JSValue promiseInstance = JS_CallConstructor(m_ctx, promiseCtor, 1, &promiseFunctionValue);
  assert(JS_IsObject(promiseInstance));
  JS_FreeValue(m_ctx, promiseCtor);

  // Call Java setUpJsPromise()
  m_jniContext->callJsBridgeVoidMethod("setUpJsPromise", "(Ljava/lang/String;Lkotlinx/coroutines/Deferred;)V",
                                       JStringLocalRef(m_jniContext, promiseObjectGlobalName.c_str()), jDeferred);
  if (m_jsBridgeContext->hasPendingJniException()) {
    m_jsBridgeContext->rethrowJniException();
    return JS_EXCEPTION;
  }

  return promiseInstance;
}

  void Deferred::completeJsPromise(const JsBridgeContext *jsBridgeContext, const std::string &strId, bool isFulfilled, const JniLocalRef<jobject> &value) {
    JSContext *ctx = jsBridgeContext->getCContext();
    assert(ctx != nullptr);

    const QuickJsUtils *utils = jsBridgeContext->getUtils();

    // Get the global PromiseObject
    JSValue globalObj = JS_GetGlobalObject(ctx);
    JSValue promiseObj = JS_GetPropertyStr(ctx, globalObj, strId.c_str());
    JS_FreeValue(ctx, globalObj);
    if (!JS_IsObject(promiseObj)) {
      alog_warn("Could not find PromiseObject with id %s", strId.c_str());
      return;
    }

    // Get attached type ptr...
    JSValue componentTypeValue = JS_GetPropertyStr(ctx, promiseObj, JavaTypes::Deferred::PROMISE_COMPONENT_TYPE_PROP_NAME);
    if (JS_IsNull(componentTypeValue) || !JS_IsObject(componentTypeValue)) {
      alog_warn("Could not get component type from Promise with id %s", strId.c_str());
      JS_FreeValue(ctx, promiseObj);
      return;
    }
    auto componentType = *utils->getCppPtr<std::shared_ptr<const JavaType>>(componentTypeValue);
    JS_FreeValue(ctx, componentTypeValue);

    // Get the resolve/reject function
    const char *resolveOrRejectStr = isFulfilled ? "resolve" : "reject";
    JSValue resolveOrReject = JS_GetPropertyStr(ctx, promiseObj, resolveOrRejectStr);
    if (JS_IsFunction(ctx, resolveOrReject)) {
      // Call it with the Promise value
      JSValue promiseParam = componentType->fromJava(JValue(value), false /*inScript*/);
      JSValue ret = JS_Call(ctx, resolveOrReject, promiseObj, 1, &promiseParam);
      if (JS_IsException(ret)) {
        alog("Could not complete Promise with id %s", strId.c_str());
      }

      JS_FreeValue(ctx, ret);
      JS_FreeValue(ctx, promiseParam);
    } else {
      alog("Could not complete Promise with id %s: cannot find %s", strId.c_str(), resolveOrRejectStr);
    }

    JS_FreeValue(ctx, resolveOrReject);
    JS_FreeValue(ctx, promiseObj);
  }

}  // namespace JavaTypes

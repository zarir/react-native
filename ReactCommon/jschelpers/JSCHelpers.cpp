// Copyright 2004-present Facebook. All Rights Reserved.

#include "JSCHelpers.h"

#ifdef WITH_FBSYSTRACE
#include <fbsystrace.h>
#endif

#include <JavaScriptCore/JSStringRef.h>
#include <folly/String.h>
#include <glog/logging.h>

#include "Value.h"

namespace facebook {
namespace react {

namespace {

JSValueRef functionCaller(
    JSContextRef ctx,
    JSObjectRef function,
    JSObjectRef thisObject,
    size_t argumentCount,
    const JSValueRef arguments[],
    JSValueRef* exception) {
  auto* f = static_cast<JSFunction*>(JSObjectGetPrivate(function));
  return (*f)(ctx, thisObject, argumentCount, arguments);
}

JSClassRef createFuncClass() {
  auto definition = kJSClassDefinitionEmpty;
  definition.finalize = [](JSObjectRef object) {
    auto* function = static_cast<JSFunction*>(JSObjectGetPrivate(object));
    delete function;
  };
  definition.callAsFunction = exceptionWrapMethod<&functionCaller>();

  return JSClassCreate(&definition);
}

JSObjectRef makeFunction(
    JSContextRef ctx,
    JSStringRef name,
    JSFunction function) {
  static auto kClassDef = createFuncClass();
  auto functionObject = Object(ctx, JSObjectMake(ctx, kClassDef, new JSFunction(std::move(function))));
  functionObject.setProperty("name", Value(ctx, name));
  return functionObject;
}

}

JSObjectRef makeFunction(
    JSContextRef ctx,
    const char* name,
    JSFunction function) {
  return makeFunction(ctx, String(ctx, name), std::move(function));
}

void installGlobalFunction(
    JSGlobalContextRef ctx,
    const char* name,
    JSFunction function) {
  auto jsName = String(ctx, name);
  auto functionObj = makeFunction(ctx, jsName, std::move(function));
  Object::getGlobalObject(ctx).setProperty(jsName, Value(ctx, functionObj));
}

JSObjectRef makeFunction(
    JSGlobalContextRef ctx,
    const char* name,
    JSObjectCallAsFunctionCallback callback) {
  auto jsName = String(ctx, name);
  return JSObjectMakeFunctionWithCallback(ctx, jsName, callback);
}

void installGlobalFunction(
    JSGlobalContextRef ctx,
    const char* name,
    JSObjectCallAsFunctionCallback callback) {
  String jsName(ctx, name);
  JSObjectRef functionObj = JSObjectMakeFunctionWithCallback(
    ctx, jsName, callback);
  Object::getGlobalObject(ctx).setProperty(jsName, Value(ctx, functionObj));
}

void installGlobalProxy(
    JSGlobalContextRef ctx,
    const char* name,
    JSObjectGetPropertyCallback callback) {
  JSClassDefinition proxyClassDefintion = kJSClassDefinitionEmpty;
  proxyClassDefintion.className = "_FBProxyClass";
  proxyClassDefintion.getProperty = callback;

  JSClassRef proxyClass = JSClassCreate(&proxyClassDefintion);
  JSObjectRef proxyObj = JSObjectMake(ctx, proxyClass, nullptr);
  JSClassRelease(proxyClass);

  Object::getGlobalObject(ctx).setProperty(name, Value(ctx, proxyObj));
}

void removeGlobal(JSGlobalContextRef ctx, const char* name) {
  Object::getGlobalObject(ctx).setProperty(name, Value::makeUndefined(ctx));
}

JSValueRef evaluateScript(JSContextRef context, JSStringRef script, JSStringRef source) {
  #ifdef WITH_FBSYSTRACE
  fbsystrace::FbSystraceSection s(TRACE_TAG_REACT_CXX_BRIDGE, "evaluateScript");
  #endif
  JSValueRef exn, result;
  result = JSEvaluateScript(context, script, NULL, source, 0, &exn);
  if (result == nullptr) {
    formatAndThrowJSException(context, exn, source);
  }
  return result;
}

#if WITH_FBJSCEXTENSIONS
JSValueRef evaluateSourceCode(JSContextRef context, JSSourceCodeRef source, JSStringRef sourceURL) {
  JSValueRef exn, result;
  result = JSEvaluateSourceCode(context, source, NULL, &exn);
  if (result == nullptr) {
    formatAndThrowJSException(context, exn, sourceURL);
  }
  return result;
}
#endif

void formatAndThrowJSException(JSContextRef context, JSValueRef exn, JSStringRef source) {
  Value exception = Value(context, exn);
  std::string exceptionText = exception.toString().str();

  // The null/empty-ness of source tells us if the JS came from a
  // file/resource, or was a constructed statement.  The location
  // info will include that source, if any.
  std::string locationInfo = source != nullptr ? String::ref(context, source).str() : "";
  Object exObject = exception.asObject();
  auto line = exObject.getProperty("line");
  if (line != nullptr && line.isNumber()) {
    if (locationInfo.empty() && line.asInteger() != 1) {
      // If there is a non-trivial line number, but there was no
      // location info, we include a placeholder, and the line
      // number.
      locationInfo = folly::to<std::string>("<unknown file>:", line.asInteger());
    } else if (!locationInfo.empty()) {
      // If there is location info, we always include the line
      // number, regardless of its value.
      locationInfo += folly::to<std::string>(":", line.asInteger());
    }
  }

  if (!locationInfo.empty()) {
    exceptionText += " (" + locationInfo + ")";
  }

  LOG(ERROR) << "Got JS Exception: " << exceptionText;

  Value jsStack = exObject.getProperty("stack");
  if (jsStack.isNull() || !jsStack.isString()) {
    throwJSExecutionException("%s", exceptionText.c_str());
  } else {
    LOG(ERROR) << "Got JS Stack: " << jsStack.toString().str();
    throwJSExecutionExceptionWithStack(
        exceptionText.c_str(), jsStack.toString().str().c_str());
  }
}

JSValueRef translatePendingCppExceptionToJSError(JSContextRef ctx, const char *exceptionLocation) {
  std::ostringstream msg;
  try {
    throw;
  } catch (const std::bad_alloc& ex) {
    throw; // We probably shouldn't try to handle this in JS
  } catch (const std::exception& ex) {
    msg << "C++ Exception in '" << exceptionLocation << "': " << ex.what();
    return Value::makeError(ctx, msg.str().c_str());
  } catch (const char* ex) {
    msg << "C++ Exception (thrown as a char*) in '" << exceptionLocation << "': " << ex;
    return Value::makeError(ctx, msg.str().c_str());
  } catch (...) {
    msg << "Unknown C++ Exception in '" << exceptionLocation << "'";
    return Value::makeError(ctx, msg.str().c_str());
  }
}

JSValueRef translatePendingCppExceptionToJSError(JSContextRef ctx, JSObjectRef jsFunctionCause) {
  try {
    auto functionName = Object(ctx, jsFunctionCause).getProperty("name").toString().str();
    return translatePendingCppExceptionToJSError(ctx, functionName.c_str());
  } catch (...) {
    return Value::makeError(ctx, "Failed to get function name while handling exception");
  }
}

} }

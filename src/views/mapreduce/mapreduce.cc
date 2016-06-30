/**
 * @copyright 2013 Couchbase, Inc.
 *
 * @author Filipe Manana  <filipe@couchbase.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. You may obtain a copy of
 * the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations under
 * the License.
 **/

#include "mapreduce.h"
#include "mapreduce_internal.h"
#include <iostream>
#include <cstring>
#include <stdlib.h>
// This is libv8_libplatform library which handles garbage collection for v8
#include <include/libplatform/libplatform.h>
#include "jsfunctions/jsfunctions_data.h"

using namespace v8;

typedef struct {
    Persistent<Object>    jsonObject;
    Persistent<Function>  jsonParseFun;
    Persistent<Function>  stringifyFun;
    mapreduce_ctx_t       *ctx;
} isolate_data_t;

static Local<Context> createJsContext();
static void emit(const FunctionCallbackInfo<Value> &args);

static void doInitContext(mapreduce_ctx_t *ctx);
static Handle<Function> compileFunction(const std::string &function);
static std::string exceptionString(const TryCatch &tryCatch);
static void loadFunctions(mapreduce_ctx_t *ctx,
                          const std::list<std::string> &function_sources);
static inline isolate_data_t *getIsolateData();
static inline mapreduce_json_t jsonStringify(const Handle<Value> &obj);
static inline Handle<Value> jsonParse(const mapreduce_json_t &thing);
static inline void taskStarted(mapreduce_ctx_t *ctx);
static inline void taskFinished(mapreduce_ctx_t *ctx);
static void freeKvListEntries(kv_list_int_t &kvs);
static void freeJsonListEntries(json_results_list_t &list);
static inline Handle<Array> jsonListToJsArray(const mapreduce_json_list_t &list);

static Platform *v8platform;
static StartupData startupData;
void initV8()
{
    V8::InitializeICU();
    v8platform = platform::CreateDefaultPlatform();
    V8::InitializePlatform(v8platform);
    V8::Initialize();
    startupData = V8::CreateSnapshotDataBlob((char *)jsFunction_src);
}

void deinitV8()
{
    V8::Dispose();
    V8::ShutdownPlatform();
    delete v8platform;
    delete[] startupData.data;
}

void initContext(mapreduce_ctx_t *ctx,
                 const std::list<std::string> &function_sources)
{
    doInitContext(ctx);

    try {
        Locker locker(ctx->isolate);
        Isolate::Scope isolate_scope(ctx->isolate);
        HandleScope handle_scope(ctx->isolate);
        Local<Context> context =
            Local<Context>::New(ctx->isolate, ctx->jsContext);
        Context::Scope context_scope(context);

        loadFunctions(ctx, function_sources);
    } catch (...) {
        destroyContext(ctx);
        throw;
    }
}


void destroyContext(mapreduce_ctx_t *ctx)
{
    {
        Locker locker(ctx->isolate);
        Isolate::Scope isolate_scope(ctx->isolate);
        HandleScope handle_scope(ctx->isolate);
        Local<Context> context =
            Local<Context>::New(ctx->isolate, ctx->jsContext);
        Context::Scope context_scope(context);

        for (unsigned int i = 0; i < ctx->functions->size(); ++i) {
            (*ctx->functions)[i]->Reset();
            delete (*ctx->functions)[i];

        }
        delete ctx->functions;

        isolate_data_t *isoData = getIsolateData();
        isoData->jsonObject.Reset();
        isoData->jsonParseFun.Reset();
        isoData->stringifyFun.Reset();
        delete isoData;

        ctx->jsContext.Reset();
    }

    ctx->isolate->Dispose();
    delete ctx->bufAllocator;

}

static Local<String> createUtf8String(Isolate *isolate, const char *str)
{
    return String::NewFromUtf8(isolate, str,
        NewStringType::kNormal).ToLocalChecked();
}

static Local<String> createUtf8String(Isolate *isolate, const char *str,
                                      size_t len)
{
    return String::NewFromUtf8(isolate, str,
        NewStringType::kNormal, len).ToLocalChecked();
}

static void doInitContext(mapreduce_ctx_t *ctx)
{
    ctx->bufAllocator = new ArrayBufferAllocator();
    Isolate::CreateParams createParams;
    createParams.snapshot_blob = &startupData;
    createParams.array_buffer_allocator = ctx->bufAllocator;
    ctx->isolate = Isolate::New(createParams);
    Locker locker(ctx->isolate);
    Isolate::Scope isolate_scope(ctx->isolate);

    HandleScope handle_scope(ctx->isolate);
    ctx->jsContext.Reset(ctx->isolate, createJsContext());
    Local<Context> context = Local<Context>::New(ctx->isolate, ctx->jsContext);
    Context::Scope context_scope(context);
    Local<String> jsonString = createUtf8String(ctx->isolate, "JSON");
    Handle<Object> jsonObject =
        Local<Object>::Cast(context->Global()->Get(jsonString));

    Local<String> parseString = createUtf8String(ctx->isolate, "parse");
    Handle<Function> parseFun =
        Local<Function>::Cast(jsonObject->Get(parseString));
    Local<String> stringifyString = createUtf8String(ctx->isolate, "stringify");
    Handle<Function> stringifyFun =
        Local<Function>::Cast(jsonObject->Get(stringifyString));

    isolate_data_t *isoData = new isolate_data_t();
    isoData->jsonObject.Reset(ctx->isolate, jsonObject);
    isoData->jsonParseFun.Reset(ctx->isolate, parseFun);
    isoData->stringifyFun.Reset(ctx->isolate, stringifyFun);
    isoData->ctx = ctx;

    ctx->isolate->SetData(0, (void *)isoData);
    ctx->taskStartTime = -1;
}


static Local<Context> createJsContext()
{
    Isolate *isolate = Isolate::GetCurrent();
    EscapableHandleScope handle_scope(isolate);

    Handle<ObjectTemplate> global = ObjectTemplate::New();
    global->Set(createUtf8String(isolate, "emit"),
            FunctionTemplate::New(isolate, emit));

    Handle<Context> context = Context::New(isolate, NULL, global);
    Context::Scope context_scope(context);

    // Use EscapableHandleScope and return using .Escape
    // This will ensure that return values are not garbage collected
    // as soon as the function returns.
    return handle_scope.Escape(context);
}


void mapDoc(mapreduce_ctx_t *ctx,
            const mapreduce_json_t &doc,
            const mapreduce_json_t &meta,
            mapreduce_map_result_list_t *results)
{
    Locker locker(ctx->isolate);
    Isolate::Scope isolate_scope(ctx->isolate);
    HandleScope handle_scope(ctx->isolate);
    Local<Context> context = Local<Context>::New(ctx->isolate, ctx->jsContext);
    Context::Scope context_scope(context);
    Handle<Value> docObject = jsonParse(doc);
    Handle<Value> metaObject = jsonParse(meta);


    if (!metaObject->IsObject()) {
        throw MapReduceError(MAPREDUCE_INVALID_ARG,
                "metadata is not a JSON object");
    }

    Handle<Value> funArgs[] = { docObject, metaObject };

    taskStarted(ctx);
    kv_list_int_t kvs;
    ctx->kvs = &kvs;

    for (unsigned int i = 0; i < ctx->functions->size(); ++i) {
        mapreduce_map_result_t mapResult;
        Local<Function> fun =
            Local<Function>::New(ctx->isolate, *(*ctx->functions)[i]);
        TryCatch try_catch(ctx->isolate);
        Handle<Value> result = fun->Call(context->Global(), 2, funArgs);

        if (!result.IsEmpty()) {
            mapResult.error = MAPREDUCE_SUCCESS;
            mapResult.result.kvs.length = kvs.size();
            size_t sz = sizeof(mapreduce_kv_t) * mapResult.result.kvs.length;
            mapResult.result.kvs.kvs = (mapreduce_kv_t *) malloc(sz);
            if (mapResult.result.kvs.kvs == NULL) {
                freeKvListEntries(kvs);
                throw std::bad_alloc();
            }
            kv_list_int_t::iterator it = kvs.begin();
            for (int j = 0; it != kvs.end(); ++it, ++j) {
                mapResult.result.kvs.kvs[j] = *it;
            }
        } else {
            freeKvListEntries(kvs);

            if (!try_catch.CanContinue()) {
                throw MapReduceError(MAPREDUCE_TIMEOUT, "timeout");
            }

            mapResult.error = MAPREDUCE_RUNTIME_ERROR;
            std::string exceptString = exceptionString(try_catch);
            size_t len = exceptString.length();

            mapResult.result.error_msg = (char *) malloc(len + 1);
            if (mapResult.result.error_msg == NULL) {
                throw std::bad_alloc();
            }
            memcpy(mapResult.result.error_msg, exceptString.data(), len);
            mapResult.result.error_msg[len] = '\0';
        }

        results->list[i] = mapResult;
        results->length += 1;
        kvs.clear();
    }

    taskFinished(ctx);
}


json_results_list_t runReduce(mapreduce_ctx_t *ctx,
                              const mapreduce_json_list_t &keys,
                              const mapreduce_json_list_t &values)
{
    Locker locker(ctx->isolate);
    Isolate::Scope isolateScope(ctx->isolate);
    HandleScope handle_scope(ctx->isolate);
    Local<Context> context = Local<Context>::New(ctx->isolate, ctx->jsContext);
    Context::Scope context_scope(context);
    Handle<Array> keysArray = jsonListToJsArray(keys);
    Handle<Array> valuesArray = jsonListToJsArray(values);
    json_results_list_t results;

    Handle<Value> args[] =
        { keysArray, valuesArray, Boolean::New(ctx->isolate, false) };

    taskStarted(ctx);

    for (unsigned int i = 0; i < ctx->functions->size(); ++i) {
        Local<Function> fun =
            Local<Function>::New(ctx->isolate, *(*ctx->functions)[i]);
        TryCatch try_catch(ctx->isolate);
        Handle<Value> result = fun->Call(context->Global(), 3, args);

        if (result.IsEmpty()) {
            freeJsonListEntries(results);

            if (!try_catch.CanContinue()) {
                throw MapReduceError(MAPREDUCE_TIMEOUT, "timeout");
            }

            throw MapReduceError(MAPREDUCE_RUNTIME_ERROR,
                    exceptionString(try_catch));
        }

        try {
            mapreduce_json_t jsonResult = jsonStringify(result);
            results.push_back(jsonResult);
        } catch(...) {
            freeJsonListEntries(results);
            throw;
        }
    }

    taskFinished(ctx);

    return results;
}


mapreduce_json_t runReduce(mapreduce_ctx_t *ctx,
                           int reduceFunNum,
                           const mapreduce_json_list_t &keys,
                           const mapreduce_json_list_t &values)
{
    Locker locker(ctx->isolate);
    Isolate::Scope isolateScope(ctx->isolate);
    HandleScope handle_scope(ctx->isolate);
    Local<Context> context = Local<Context>::New(ctx->isolate, ctx->jsContext);
    Context::Scope context_scope(context);

    reduceFunNum -= 1;
    if (reduceFunNum < 0 ||
        static_cast<unsigned int>(reduceFunNum) >= ctx->functions->size()) {
        throw MapReduceError(MAPREDUCE_INVALID_ARG,
                "invalid reduce function number");
    }

    Local<Function> fun =
        Local<Function>::New(ctx->isolate, *(*ctx->functions)[reduceFunNum]);
    Handle<Array> keysArray = jsonListToJsArray(keys);
    Handle<Array> valuesArray = jsonListToJsArray(values);
    Handle<Value> args[] =
        { keysArray, valuesArray, Boolean::New(ctx->isolate, false) };

    taskStarted(ctx);

    TryCatch try_catch(ctx->isolate);
    Handle<Value> result = fun->Call(context->Global(), 3, args);

    taskFinished(ctx);

    if (result.IsEmpty()) {
        if (!try_catch.CanContinue()) {
            throw MapReduceError(MAPREDUCE_TIMEOUT, "timeout");
        }

        throw MapReduceError(MAPREDUCE_RUNTIME_ERROR,
                exceptionString(try_catch));
    }

    return jsonStringify(result);
}


mapreduce_json_t runRereduce(mapreduce_ctx_t *ctx,
                             int reduceFunNum,
                             const mapreduce_json_list_t &reductions)
{
    Locker locker(ctx->isolate);
    Isolate::Scope isolateScope(ctx->isolate);
    HandleScope handle_scope(ctx->isolate);
    Local<Context> context = Local<Context>::New(ctx->isolate, ctx->jsContext);
    Context::Scope context_scope(context);

    reduceFunNum -= 1;
    if (reduceFunNum < 0 ||
        static_cast<unsigned int>(reduceFunNum) >= ctx->functions->size()) {
        throw MapReduceError(MAPREDUCE_INVALID_ARG,
                "invalid reduce function number");
    }

    Local<Function> fun =
        Local<Function>::New(ctx->isolate, *(*ctx->functions)[reduceFunNum]);
    Handle<Array> valuesArray = jsonListToJsArray(reductions);
    Handle<Value> args[] =
        { Null(ctx->isolate), valuesArray, Boolean::New(ctx->isolate, true) };

    taskStarted(ctx);

    TryCatch try_catch(ctx->isolate);
    Handle<Value> result = fun->Call(context->Global(), 3, args);

    taskFinished(ctx);

    if (result.IsEmpty()) {
        if (!try_catch.CanContinue()) {
            throw MapReduceError(MAPREDUCE_TIMEOUT, "timeout");
        }

        throw MapReduceError(MAPREDUCE_RUNTIME_ERROR,
                exceptionString(try_catch));
    }

    return jsonStringify(result);
}


void terminateTask(mapreduce_ctx_t *ctx)
{
    V8::TerminateExecution(ctx->isolate);
    taskFinished(ctx);
}


static void freeKvListEntries(kv_list_int_t &kvs)
{
    kv_list_int_t::iterator it = kvs.begin();

    for ( ; it != kvs.end(); ++it) {
        mapreduce_kv_t kv = *it;
        free(kv.key.json);
        free(kv.value.json);
    }
    kvs.clear();
}


static void freeJsonListEntries(json_results_list_t &list)
{
    json_results_list_t::iterator it = list.begin();

    for ( ; it != list.end(); ++it) {
        free((*it).json);
    }
    list.clear();
}


static Handle<Function> compileFunction(const std::string &funSource)
{
    Isolate *isolate = Isolate::GetCurrent();
    Local<Context> context(isolate->GetCurrentContext());
    EscapableHandleScope handle_scope(isolate);
    TryCatch try_catch(isolate);
    Local<String> source = createUtf8String(isolate, funSource.data(),
        funSource.length());
    Local<Script> script;
    if (!Script::Compile(context, source).ToLocal(&script)) {
        throw MapReduceError(MAPREDUCE_SYNTAX_ERROR,
                exceptionString(try_catch));
    }

    if (script.IsEmpty()) {
        throw MapReduceError(MAPREDUCE_SYNTAX_ERROR,
                exceptionString(try_catch));
    }

    Handle<Value> result = script->Run();

    if (result.IsEmpty()) {
        throw MapReduceError(MAPREDUCE_SYNTAX_ERROR,
                exceptionString(try_catch));
    }

    if (!result->IsFunction()) {
        throw MapReduceError(MAPREDUCE_SYNTAX_ERROR,
                std::string("Invalid function: ") + funSource.c_str());
    }

    // Use EscapableHandleScope and return using .Escape
    // This will ensure that return values are not garbage collected
    // as soon as the function returns.
    return handle_scope.Escape(Handle<Function>::Cast(result));
}


static std::string exceptionString(const TryCatch &tryCatch)
{
    HandleScope handle_scope(Isolate::GetCurrent());
    String::Utf8Value exception(tryCatch.Exception());
    const char *exceptionString = (*exception);

    if (exceptionString) {
        Handle<Message> message = tryCatch.Message();
        return std::string(exceptionString) + " (line " +
            std::to_string(message->GetLineNumber()) + ":" +
            std::to_string(message->GetStartColumn()) + ")";
    }

    return std::string("runtime error");
}


static void loadFunctions(mapreduce_ctx_t *ctx,
                          const std::list<std::string> &function_sources)
{
    HandleScope handle_scope(ctx->isolate);

    ctx->functions = new function_vector_t();

    std::list<std::string>::const_iterator it = function_sources.begin();

    for ( ; it != function_sources.end(); ++it) {
        Handle<Function> fun = compileFunction(*it);
        Persistent<Function> *perFn = new Persistent<Function>();
        perFn->Reset(ctx->isolate, fun);
        ctx->functions->push_back(perFn);
    }
}


static void emit(const FunctionCallbackInfo<Value> &args)
{
    isolate_data_t *isoData = getIsolateData();

    if (isoData->ctx->kvs == NULL) {
        return;
    }

    try {
        mapreduce_kv_t result;

        result.key   = jsonStringify(args[0]);
        result.value = jsonStringify(args[1]);
        isoData->ctx->kvs->push_back(result);

        return;
    } catch(Local<String> &ex) {
        isoData->ctx->isolate->ThrowException(ex);
    }
}


static inline isolate_data_t *getIsolateData()
{
    Isolate *isolate = Isolate::GetCurrent();
    return reinterpret_cast<isolate_data_t*>(isolate->GetData(0));
}


static inline mapreduce_json_t jsonStringify(const Handle<Value> &obj)
{
    isolate_data_t *isoData = getIsolateData();
    Handle<Value> args[] = { obj };
    TryCatch try_catch(isoData->ctx->isolate);
    Local<Function> stringifyFun =
        Local<Function>::New(isoData->ctx->isolate, isoData->stringifyFun);
    Local<Object> jsonObject =
        Local<Object>::New(isoData->ctx->isolate, isoData->jsonObject);
    Handle<Value> result = stringifyFun->Call(jsonObject, 1, args);

    if (result.IsEmpty()) {
        throw try_catch.Exception();
    }

    mapreduce_json_t jsonResult;

    if (!result->IsUndefined()) {
        Handle<String> str = Handle<String>::Cast(result);
        jsonResult.length = str->Utf8Length();
        jsonResult.json = (char *) malloc(jsonResult.length);
        if (jsonResult.json == NULL) {
            throw std::bad_alloc();
        }
        str->WriteUtf8(jsonResult.json, jsonResult.length,
                       NULL, String::NO_NULL_TERMINATION);
    } else {
        jsonResult.length = sizeof("null") - 1;
        jsonResult.json = (char *) malloc(jsonResult.length);
        if (jsonResult.json == NULL) {
            throw std::bad_alloc();
        }
        memcpy(jsonResult.json, "null", jsonResult.length);
    }

    // Caller responsible for freeing jsonResult.json
    return jsonResult;
}


static inline Handle<Value> jsonParse(const mapreduce_json_t &thing)
{
    isolate_data_t *isoData = getIsolateData();
    Handle<Value> args[] =
        { createUtf8String(isoData->ctx->isolate, thing.json,
            thing.length) };
    TryCatch try_catch(isoData->ctx->isolate);
    Local<Function> jsonParseFun =
        Local<Function>::New(isoData->ctx->isolate, isoData->jsonParseFun);
    Local<Object> jsonObject =
        Local<Object>::New(isoData->ctx->isolate, isoData->jsonObject);
    Handle<Value> result = jsonParseFun->Call(jsonObject, 1, args);

    if (result.IsEmpty()) {
        throw MapReduceError(MAPREDUCE_RUNTIME_ERROR,
                exceptionString(try_catch));
    }

    return result;
}


static inline void taskStarted(mapreduce_ctx_t *ctx)
{
    ctx->taskStartTime = time(NULL);
    ctx->kvs = NULL;
}


static inline void taskFinished(mapreduce_ctx_t *ctx)
{
    ctx->taskStartTime = -1;
}


static inline Handle<Array> jsonListToJsArray(const mapreduce_json_list_t &list)
{
    Isolate *isolate = Isolate::GetCurrent();
    Handle<Array> array = Array::New(isolate, list.length);

    for (int i = 0 ; i < list.length; ++i) {
        Handle<Value> v = jsonParse(list.values[i]);
        array->Set(Number::New(isolate, i), v);
    }

    return array;
}

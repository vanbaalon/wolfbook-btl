// =============================================================
// addon.cpp  —  Node.js N-API wrapper for boxToLatex
//
// Exposes a single synchronous function:
//   boxToLatex(wlBoxString: string): { latex: string; error: string | null }
//
// Built with node-addon-api (C++ wrapper over raw N-API).
// This file is the only file that is aware of Node.js; all
// translation logic is isolated in box_to_latex.cpp.
// =============================================================

#include <napi.h>
#include "box_to_latex.h"
#include "line_breaker.h"

namespace {

// ----------------------------------------------------------
// JS binding:
//   boxToLatex(wlString: string)
//     => { latex: string; error: string | null }
//
// On success: { latex: "...", error: null }
// On parse/translation error: { latex: <raw input>, error: "<message>" }
// ----------------------------------------------------------
Napi::Value JsBoxToLatex(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 1 || !info[0].IsString()) {
        Napi::TypeError::New(env, "boxToLatex expects a single string argument")
            .ThrowAsJavaScriptException();
        return env.Null();
    }

    // Decode the JS UTF-8 string into a std::string
    std::string input = info[0].As<Napi::String>().Utf8Value();

    // Call the C++ translator — never throws (catches internally)
    wolfbook::BoxResult result = wolfbook::boxToLatex(input);

    // Build result object: { latex, error }
    Napi::Object obj = Napi::Object::New(env);
    obj.Set("latex", Napi::String::New(env, result.latex));
    if (result.error.empty()) {
        obj.Set("error", env.Null());
    } else {
        obj.Set("error", Napi::String::New(env, result.error));
    }
    return obj;
}

// ----------------------------------------------------------
// JS binding:
//   lineBreakLatex(latexString: string, options?: { pageWidth, indentStep, compact, maxDelimDepth })
//     => string
// ----------------------------------------------------------
Napi::Value JsLineBreakLatex(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 1 || !info[0].IsString()) {
        Napi::TypeError::New(env, "lineBreakLatex expects a string argument")
            .ThrowAsJavaScriptException();
        return env.Null();
    }

    std::string latex = info[0].As<Napi::String>().Utf8Value();

    wolfbook::LineBreakOptions opts;
    if (info.Length() >= 2 && info[1].IsObject()) {
        Napi::Object jsOpts = info[1].As<Napi::Object>();
        if (jsOpts.Has("pageWidth"))
            opts.pageWidth = jsOpts.Get("pageWidth").As<Napi::Number>().DoubleValue();
        if (jsOpts.Has("indentStep"))
            opts.indentStep = jsOpts.Get("indentStep").As<Napi::Number>().DoubleValue();
        if (jsOpts.Has("compact"))
            opts.compact = jsOpts.Get("compact").As<Napi::Boolean>().Value();
        if (jsOpts.Has("maxDelimDepth"))
            opts.maxDelimDepth = jsOpts.Get("maxDelimDepth").As<Napi::Number>().Int32Value();
    }

    std::string result = wolfbook::lineBreakLatex(latex, opts);
    return Napi::String::New(env, result);
}

// ----------------------------------------------------------
// Module initialiser
// ----------------------------------------------------------
Napi::Object Init(Napi::Env env, Napi::Object exports) {
    exports.Set(
        Napi::String::New(env, "boxToLatex"),
        Napi::Function::New(env, JsBoxToLatex, "boxToLatex"));
    exports.Set(
        Napi::String::New(env, "lineBreakLatex"),
        Napi::Function::New(env, JsLineBreakLatex, "lineBreakLatex"));
    return exports;
}

} // anonymous namespace

NODE_API_MODULE(wolfbook_btl, Init)

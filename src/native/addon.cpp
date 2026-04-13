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
#if __has_include("build_version.h")
#  include "build_version.h"
#endif

namespace {

// ----------------------------------------------------------
// JS binding:
//   boxToLatex(wlString: string, opts?: { trigOmitParens?: boolean, trigPowerForm?: boolean })
//     => { latex: string; error: string | null }
//
// On success: { latex: "...", error: null }
// On parse/translation error: { latex: <raw input>, error: "<message>" }
// ----------------------------------------------------------
Napi::Value JsBoxToLatex(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 1 || !info[0].IsString()) {
        Napi::TypeError::New(env, "boxToLatex expects a string as first argument")
            .ThrowAsJavaScriptException();
        return env.Null();
    }

    // Decode the JS UTF-8 string into a std::string
    std::string input = info[0].As<Napi::String>().Utf8Value();

    // Read optional style options from second argument
    wolfbook::BtlOptions opts;
    int btlRequestedPage = 0;
    if (info.Length() >= 2 && info[1].IsObject()) {
        Napi::Object jsOpts = info[1].As<Napi::Object>();
        if (jsOpts.Has("trigOmitParens"))
            opts.trigOmitParens = jsOpts.Get("trigOmitParens").As<Napi::Boolean>().Value();
        if (jsOpts.Has("trigPowerForm"))
            opts.trigPowerForm = jsOpts.Get("trigPowerForm").As<Napi::Boolean>().Value();
        if (jsOpts.Has("maxRows"))
            opts.maxRows = jsOpts.Get("maxRows").As<Napi::Number>().Int32Value();
        if (jsOpts.Has("requestedPage"))
            btlRequestedPage = jsOpts.Get("requestedPage").As<Napi::Number>().Int32Value();
    }
    bool allPages = false;
    if (info.Length() >= 2 && info[1].IsObject()) {
        Napi::Object jsOpts = info[1].As<Napi::Object>();
        if (jsOpts.Has("allPages"))
            allPages = jsOpts.Get("allPages").As<Napi::Boolean>().Value();
    }

    // Call the C++ translator — never throws (catches internally)
    wolfbook::BoxResult result = wolfbook::boxToLatex(input, opts);

    // Build result object: { latex, error, totalPages?, pages? }
    // When paging is triggered (pages.size() > 1):
    //   - allPages:true  → return all page LaTeX strings in result.pages[] (no re-run needed)
    //   - allPages:false → select the single requested page (legacy behaviour)
    Napi::Object obj = Napi::Object::New(env);
    if (result.pages.size() > 1) {
        const int totalPages = (int)result.pages.size();
        obj.Set("totalPages", Napi::Number::New(env, totalPages));
        if (allPages) {
            // Expose all pages as a JS array — caller caches them all at once
            Napi::Array pagesArr = Napi::Array::New(env, totalPages);
            for (int i = 0; i < totalPages; i++)
                pagesArr.Set((uint32_t)i, Napi::String::New(env, result.pages[i]));
            obj.Set("pages", pagesArr);
            obj.Set("latex", Napi::String::New(env, result.pages[0]));
        } else {
            int pg = btlRequestedPage;
            if (pg < 0) pg = 0;
            if (pg >= totalPages) pg = totalPages - 1;
            obj.Set("latex", Napi::String::New(env, result.pages[pg]));
        }
    } else {
        obj.Set("latex", Napi::String::New(env, result.latex));
    }
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
        if (jsOpts.Has("pageWidthPx"))
            opts.pageWidthPx = jsOpts.Get("pageWidthPx").As<Napi::Number>().DoubleValue();
        if (jsOpts.Has("baseFontSizePx"))
            opts.baseFontSizePx = jsOpts.Get("baseFontSizePx").As<Napi::Number>().DoubleValue();
        if (jsOpts.Has("maxIterations"))
            opts.maxIterations = jsOpts.Get("maxIterations").As<Napi::Number>().Int32Value();
        if (jsOpts.Has("maxRows"))
            opts.maxRows = jsOpts.Get("maxRows").As<Napi::Number>().Int32Value();
        if (jsOpts.Has("requestedPage"))
            opts.requestedPage = jsOpts.Get("requestedPage").As<Napi::Number>().Int32Value();
        if (jsOpts.Has("allPages"))
            opts.allPages = jsOpts.Get("allPages").As<Napi::Boolean>().Value();
    }

    wolfbook::LineBreakResult result = wolfbook::lineBreakLatex(latex, opts);

    // Return { result: string, totalPages: int, pages?: string[] }.
    // totalPages > 1 means the expression was split into pages; the caller
    // should display prev/next controls and request other pages on demand.
    // When allPages was requested, pages[] holds all page LaTeX strings.
    Napi::Object obj = Napi::Object::New(env);
    obj.Set("result", Napi::String::New(env, result.result));
    obj.Set("totalPages", Napi::Number::New(env, result.totalPages));
    if (!result.pages.empty()) {
        const int n = (int)result.pages.size();
        Napi::Array pagesArr = Napi::Array::New(env, n);
        for (int i = 0; i < n; i++)
            pagesArr.Set((uint32_t)i, Napi::String::New(env, result.pages[i]));
        obj.Set("pages", pagesArr);
    }
    return obj;
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
#ifndef WOLFBOOK_BTL_VERSION
#  define WOLFBOOK_BTL_VERSION "dev"
#endif
#ifndef WOLFBOOK_BTL_BUILD_DATE
#  define WOLFBOOK_BTL_BUILD_DATE "unknown"
#endif
    exports.Set("version",   Napi::String::New(env, WOLFBOOK_BTL_VERSION));
    exports.Set("buildDate", Napi::String::New(env, WOLFBOOK_BTL_BUILD_DATE));
    return exports;
}

} // anonymous namespace

NODE_API_MODULE(wolfbook_btl, Init)

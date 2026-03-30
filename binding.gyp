{
  "targets": [
    {
      "target_name": "wolfbook_btl",

      "sources": [
        "src/native/addon.cpp",
        "src/native/box_to_latex.cpp",
        "src/native/line_breaker.cpp",
        "src/native/wl_parser.cpp",
        "src/native/special_chars.cpp"
      ],

      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")"
      ],

      "defines": [
        "NAPI_CPP_EXCEPTIONS"
      ],

      "cflags_cc": [
        "-std=c++17",
        "-O3",
        "-Wall",
        "-Wextra",
        "-fvisibility=hidden"
      ],

      "xcode_settings": {
        "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
        "CLANG_CXX_LANGUAGE_STANDARD": "c++17",
        "CLANG_CXX_LIBRARY": "libc++",
        "OTHER_CFLAGS": [ "-O3", "-Wall", "-fvisibility=hidden" ],
        "MACOSX_DEPLOYMENT_TARGET": "11.0"
      },

      "msvs_settings": {
        "VCCLCompilerTool": {
          "ExceptionHandling": 1,
          "Optimization": 3,
          "AdditionalOptions": [ "/std:c++17" ]
        }
      },

      "conditions": [
        [
          "OS == 'linux'",
          {
            "cflags_cc": [ "-std=c++17", "-O3", "-fvisibility=hidden", "-fexceptions", "-frtti" ]
          }
        ]
      ]
    }
  ]
}

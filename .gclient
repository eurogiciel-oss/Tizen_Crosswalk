solutions = [
  { "name"        : "src/xwalk",
    "url"         : "git://github.com/crosswalk-project/crosswalk.git",
    "deps_file"   : "DEPS",
    "managed"     : True,
    "custom_deps" : {
    },
    "safesync_url": "",
    "custom_hooks": [
      {
        "name": "generate-gclient-xwalk",
        "pattern": ".",
        "action": ["python", "src/xwalk/tools/generate_gclient-xwalk.py"],
      },
      { "name": "fetch-deps" },
      { "name": "gyp-xwalk" },
    ],
  },
]
cache_dir = None

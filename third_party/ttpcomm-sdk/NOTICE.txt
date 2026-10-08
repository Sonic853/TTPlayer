TTPlayerComm shared SDK

Canonical headers and consumer helpers live in this directory. Export a pinned
consumer snapshot with cmake/sync_sdk.py. Generated SDK_MANIFEST.json is checked
without accessing the network or requiring a sibling source checkout.

The legacy C ABI stays in ../include and ../src/ttpcomm_api.c; sync_sdk.py includes
it in the snapshot. Do not hand-edit a generated snapshot. Do not change the
historical reverse-engineering evidence to match this SDK.

C++ helpers require C++17 and the Windows SDK; client.h uses MSVC x86 SEH and
the recovered x86 calling conventions. tags_zlib.h also requires zlib. STL
objects and borrowed byte views stay inside the consumer module, never across
the DLL ABI. The named extension uses a size/version checked C POD structure.

Keep the repository's third-party notices when distributing the corresponding
libraries (../licenses). The SDK does not include third-party library sources.
The zlib helper calls the consumer's zlib; it does not load ttpcomm.dll or change
the strictness of archive validation. Decoder, filesystem and UI policy remains
with the host.

runtime_api.h defines the separate rebuilt-host ABI 1 (ordinal 501).
runtime_client.cpp, runtime_pcm.h and runtime_tags.h use the rebuilt DLL for
compression, PCM and standard tag primitives. Unlike the reference helpers,
they require this runtime and do not fall back to the original DLL. The
consumer allocates PCM buffers; DLL-owned decoded payloads and quantizer
handles must be destroyed through the matching function table. The standalone
updater continues using its private inflater so it can replace the core DLL.

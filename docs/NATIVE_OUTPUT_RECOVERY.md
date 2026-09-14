# Native KS / ASIO recovery boundary

This recovery step adds tested device-key/catalog resolution, a native Kernel
Streaming (KS) playback sink, and ASIO channel/buffer contracts.  KS and ASIO
selection open only the accepted native device; neither silently substitutes
waveOut, DirectSound or WASAPI.

## Recovered and executable

- `FUN_004918B1` / `FUN_004918FE`: a zero-tail GUID uses `Data1.high` for
  backend and `Data1.low` for ordinal. A nonzero tail selects DirectSound,
  even if `Data1.high` resembles KS/ASIO. The default DirectSound GUID remains
  `DEF00000-9C6D-47ED-AAF1-4DDA8F2B5C03`.
- `ResolveLegacyNativeOutputDevice` resolves the persisted key against the
  accepted discovery snapshot. Display sorting cannot change ordinal identity.
  Missing, duplicate, wrong-backend and incomplete descriptors are distinct
  failures. ASIO requires the registry CLSID as well as its module path.
- `FUN_004E14BD`: supported ASIO channel types are `0..4`, `16..20`, and
  `24..27`. The latter are 32-bit little-endian integer containers with
  16/18/20/24 valid bits. Container width and valid width must remain distinct.
  DSD and the unsupported big-endian reduced-valid-bit types are not accepted.
- `FUN_004E1610`: preferred driver frames determine the period; requested
  source bytes are rounded to a period count, with a minimum of four periods.
  `PlanAsioBuffers` implements that formula and rejects zero, negative or
  overflowing driver results before allocation.

The deterministic `native_output_contract_tests` exercise these contracts
without loading a hardware driver.  Conversion, callback ownership and the
device-stream workers are implemented and covered by their dedicated sink
tests; catalogue acceptance by itself is still not a claim that a particular
third-party driver has been exercised on physical hardware.

## Native KS stream

| Original entry | Contract / remaining implementation |
| --- | --- |
| `004E029E` | `KernelStreamingSink::OpenDevice` resolves the selected Audio+Render filter, finds a standard-streaming render pin, calls `KsCreatePin` with source-width candidates and then a 16-bit candidate. It preserves source-byte accounting across the fallback. |
| `004E0430` | Each `Packet` owns a manual-reset event, `KSSTREAM_HEADER`, `OVERLAPPED`, and PCM allocation. `PresentationTime` uses numerator 10,000,000 and output bytes/sec denominator. |
| `004E088A` / `004F0A9E` | Completed packets are converted/copied and submitted via the real `IOCTL_KS_WRITE_STREAM` (`0x2F8013`). Pending header/data/event storage is never reused or freed. |
| `004E0773` / `004E07FF` | Position comes from `KSPROPERTY_AUDIO_POSITION` when the pin supplies it and is mapped back to source bytes; completed-packet accounting is the bounded fallback. |
| `004E059B` / `004F0A53` / `004E01C1` | Seek/reset and teardown perform explicit pause/stop/cancel/drain transitions. Reset uses `IOCTL_KS_RESET_STATE` begin/end (`0x2F001B`), then returns the pin to run/pause. A noncooperative driver retains the complete packet owner until completion instead of receiving freed memory. |

`kernel_streaming_sink_tests` drives the sink with a deterministic transport:
initial state order, 24-to-16 fallback PCM, pending-packet exclusion,
position mapping, cancellation/reset and error propagation are all covered
without loading a driver.  The sink is owned by `AudioEngine`'s audio worker;
there is no waveOut fallback on this path.

The original can set hardware topology volume nodes (`004E061B`). That node
topology is device-specific and its recovered traversal ABI is incomplete, so
KS user volume/balance is safely applied while copying future native packets.
Opening play/pause/seek fade is not pre-baked into the initial queue: doing so
would insert a long silent period before the fade worker can publish a gain.

## ASIO stream

`AsioSink` activates the registry CLSID with that same CLSID as the requested
IID, then calls vtable `+0x0C` to initialize. The `ASIO Kernel-Streaming driver`
uses the recovered `DllGetClassObject` / `IClassFactory::CreateInstance` path;
other entries use generic `CoCreateInstance`.

The x86 vtable offsets visible in the original are:

| Offset | Operation |
| --- | --- |
| `0x0C` | initialize |
| `0x24` | get input/output channel counts |
| `0x2C` | get minimum/maximum/preferred/granularity buffer sizes |
| `0x30` | sample-rate support query |
| `0x38` | set sample rate |
| `0x48` | channel information |
| `0x4C` | create buffers and install callback table |
| `0x50` | dispose buffers |
| `0x5C` | output-ready handshake |

`004E131C` installs a four-function callback table and a single global active
instance. `004E1450` rejects a second active instance. `004E1860` handles
per-channel queue reads and zero fills short periods. `004E1383` first detaches
the callback target, waits for in-flight callbacks, disposes driver buffers,
then releases the driver. `AsioSink` follows this ordering and maps signed
interleaved 16/24/32-bit PCM to the recovered `max(2, min(source, output))`
channel count; mono is duplicated to the first stereo pair.  The original
output object's `SetVolume` and `SetBalance` vtable entries (`+0x20/+0x24`) are
literal `E_NOTIMPL` stubs.  The rebuild therefore submits ASIO PCM at unity
instead of inventing software volume, balance or fade processing, and the
worker stops/restarts the IASIO clock for pause and seek.

Do not copy the floating-point converter expressions from the pseudo-C as
literal executable arithmetic. For example `004E1133` / `004E0DE1` show
multiplication by `DAT_005263B8` / `DAT_005263C0`. The bytes in TTPlayer.exe at
those addresses decode to `2147483648.0f` and `2147483647.0`, respectively.
The implementation normalizes integer PCM against its signed range before
writing integer or IEEE floating-point ASIO samples; mock tests cover normal
little-endian PCM, MSB PCM, short-period zero-fill, the callback singleton,
start/stop, reset, disposal and output-ready handshake.

## Hardware verification boundary

An actual compatible device is still required to verify its claimed ASIO ABI,
channel routing, underrun recovery, pause/seek/stop timing and disconnect
behavior. Deterministic lifecycle tests establish ownership and state behavior,
not audible hardware compatibility.

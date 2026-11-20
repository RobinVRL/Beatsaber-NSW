# Original Quest resources.arsc — bounded, configuration-aware resolver

The original Quest APK's unique stored `resources.arsc` entry is **258,492
bytes**, CRC32 `0x0B0977D4`. The new
`source/unity_dex_quest_arsc_resolver.[ch]` reads that entry using the
existing verified ZIP32 stored-entry reader and validates its Android
binary resource table before exposing native values.

This table has one package (ID `0x7F`, package
`com.beatgames.beatsaber`), a global UTF-8 string pool, UTF-16 package
type-name pool, UTF-8 resource key-name pool, typeSpec records, and
configuration-specific type chunks. There are 86 variants of the string
resource type, as well as density-, orientation- and other qualified
resources. The resolver walks actual chunk sizes/offsets and validates
entry maps, `Res_value` sizes, string indexes and Unicode data, including
UTF-16 surrogate pairs and UTF-8 code-point/declared-length consistency.
Invalid/overlapping chunks, unsupported flags, invalid typeSpec/entry
offset arithmetic, invalid resource names and malformed strings fail
without publishing a partial resolver. Parsing copies the table into
independently owned memory; release frees it.

## Exact callable API

```c
BsQuestArscResolver table = {0};
int verified = bs_quest_arsc_open_apk(original_quest_apk, &table);

/* Only after a real Horizon configuration service observed its language. */
BsQuestArscConfiguration config = {0};
int observed = bs_quest_arsc_set_observed_language(
    &config, native_context_configuration.horizon_language_tag);

char name[4096] = {0};
int result = verified && observed && bs_quest_arsc_get_string(
    &table, 0x7F0D0005u, &config, name, sizeof name);

int native_boolean = 0;
int boolean_ok = verified && bs_quest_arsc_get_boolean(
    &table, 0x7F040001u, NULL, &native_boolean);

int32_t native_integer = 0;
int integer_ok = verified && bs_quest_arsc_get_integer(
    &table, 0x7F090000u, NULL, &native_integer);

bs_quest_arsc_release(&table);
```

`bs_quest_arsc_parse(bytes, size, &table)` is the general bounded parser,
used for malformed-fixture tests. It is **not** proof of source APK
authenticity: production calls `bs_quest_arsc_open_apk` for the pinned
file CRC and exact size. Readback outputs are owned by the caller. The
string getter returns copied UTF-8 into the caller's bounded buffer;
Boolean and signed integer getters require actual `TYPE_INT_BOOLEAN`
and `TYPE_INT_DEC/TYPE_INT_HEX` values, respectively.
`bs_quest_arsc_get_name` resolves the actual package type/key pools.

## Verified values from original Quest APK and DEX

Each of these resource IDs also appears as an actual little-endian integer
in the original Quest `classes.dex`, independently checked in the test.

| ID | Type / resource key | Actual result |
| --- | --- | --- |
| `0x7F0D0021` | `string/app_name` | `Beat Saber` |
| `0x7F0D0005` | `string/abc_action_bar_home_description` | `Zur Startseite` for observed `de-DE`; `ホームに戻る` for `ja-JP` |
| `0x7F040001` | `bool/abc_config_actionMenuItemAllCaps` | true |
| `0x7F040000` | `bool/abc_action_bar_embed_tabs` | true for observed landscape, false for observed portrait |
| `0x7F090000` | `integer/abc_config_activityDefaultDur` | 220 |

An absent locale for the localized `0x7F0D0005` fails 0; the resolver
does **not** assume English. An unobserved orientation for
`0x7F040000` also fails 0. The original six density variants of
`mipmap/0x7F0C0000` are rejected while Android density is unknown.

## Precise configuration limits

The observed configuration contains only explicit
`language_tag`, `density_dpi`, `orientation`, and
`sdk_version` fields. A zero/empty attribute means *unknown*.
Currently Horizon supplies an independently read BCP47 language, which
can safely choose a language or language-region variant. For matching
resource IDs with additional unknown qualifiers (density, orientation,
screen dp, MCC/MNC, script, color mode, API level, layout, device input),
resolution fails 0 even when an unqualified fallback is present.

Qualified density values need an independently observed **exact** Android
density. The resolver refuses Android's more complicated density-scaling
fallback when no exact candidate exists. It never derives Android dpi or
dp sizes from NWindow framebuffer pixels. SDK-dependent choices similarly
need an independently observed runtime SDK level, not merely the APK
manifest targetSdkVersion. Locale scripts and variants are not inferred
from a two-letter language code. A missing or ambiguous winning resource
configuration fails closed.

Scalar `TYPE_REFERENCE` values are followed only within the same
verified package, up to 12 hops with cycle detection. Theme attributes,
complex style/map/array entries, drawables/layouts/XML decoding,
configuration-aware density scaling, overlays, and resource table styles
are not implemented. The parser rejects unsupported chunk structures
instead of inventing a value.

## Integration boundary

Prime may use this native resolver as the data source for the existing
`BsDexHorizonJavaResources` provider once a real `Resources` class
registry and the original DEX framework-method dispatcher have been
installed. The resolver itself does **not** construct or identify Java
`Resources`, `AssetManager`, `Configuration` or `String` instances;
it cannot establish Android `Activity.attach`, Java Looper/Handler,
onCreate completion or graphics readiness. Actual Java method execution,
object lifetime, exception behavior and the remaining resource APIs
must still be provided and independently observed by the VM/Java backend.

Validation uses `python tests/test_unity_dex_quest_arsc_resolver.py`:
the exact original table, type/name pools and locale/Boolean/integer
values, plus root/package/typeSpec/type-chunk corruption, invalid UTF-8,
UTF-16 surrogates, bad string/key indexes and truncated inputs. New C
source independently compiles using host Clang and devkitA64 GCC with
`-Wall -Wextra -Werror`.

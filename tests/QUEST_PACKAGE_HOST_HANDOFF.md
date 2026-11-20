# Original Quest APK package model and Java integration boundary

source/unity_dex_quest_package_host.[ch] reads the exact Quest 1.46
AndroidManifest.xml and the stored resources.arsc (258492 bytes, CRC32
0x0B0977D4), and independently validates the original classes.dex through
the pinned DEX loader in the same staged APK. Its returned package model
is native C data, never a JNI jobject. These identities must all verify.

## Original onCreate call path

The original UnityPlayerGameActivity.onCreate at code 0x23794C invokes
GameActivity.onCreate at code 0x22EDAC. Its exact observed DEX imports:

| PC | Original Android method | Requirement |
| --- | --- | --- |
| 0x18 | GameActivity.getPackageManager | live PackageManager |
| 0x1C | GameActivity.getIntent | delivered Activity launch Intent |
| 0x20 | Intent.getComponent | real ComponentName from Intent |
| 0x26 | PackageManager.getActivityInfo | actual ActivityInfo for component |
| 0x32 | Bundle.getString | ActivityInfo.metaData: android.app.lib_name absent |
| 0x62 | GameActivity.getClassLoader | real Java ClassLoader |
| 0xCB | GameActivity.getFilesDir | Java File object |
| 0xD3 | GameActivity.getObbDir | Java File object |
| 0xDF | GameActivity.getAssets | Java AssetManager |
| 0xE3 | GameActivity.getResources | Java Resources |
| 0xE7 | Resources.getConfiguration | live Configuration |
| 0xEC | GameActivity.initializeNativeCode | completed Java/NDK lifecycle |

Original Unity also reads ApplicationInfo.nativeLibraryDir,
ApplicationInfo.metaData and the Boolean unity.run-without-focus, which
is true in the original application manifest.

## Verified native package data

The actual APK supplies package com.beatgames.beatsaber, version
1.46.0_34060, code 3491, min/target SDK 32, UnityPlayerGameActivity
as the enabled exported launch Activity, label resource 0x7F0D0021,
theme 0x7F0E00FF and configChanges 1073758207.
Its intent filter accepts android.intent.action.MAIN and categories
android.intent.category.LAUNCHER and com.oculus.intent.category.VR.

The parsed metadata has 15 application entries and one activity entry.
Keeping these scopes distinct is required: GameActivity reads
ActivityInfo.metaData and Unity reads ApplicationInfo.metaData.
The native data model exposes exact typed BOOLEAN, INTEGER, STRING and
RESOURCE_ID values without coercion. Missing-key native Bundle-source
string/boolean getters return null/false; incompatible present types fail
closed. This does not execute Java Bundle methods.

Staged sourceDir/publicSourceDir are the verified original APK file path.
The nativeLibraryDir is only reported after validating real staged ARM64
ELF headers (libmain.so and libunity.so), and dataDir after observing a
physical directory. These path strings are not Java File/String handles.

## Production integration

BsDexQuestJavaOwners/BsDexQuestJavaWitness require independent live-Java
readback of actual Intent, ComponentName, PackageManager, ApplicationInfo,
ActivityInfo, the typed metadata Bundles and nativeLibraryDir String.
The manifest launch filter does not prove Activity.getIntent delivered that
Intent. The PackageManager witness must confirm the concrete
getActivityInfo(ComponentName, GET_META_DATA) result.

bs_dex_quest_package_stage_context rechecks every witness before copying
ONLY package-related Java owners to a candidate
BsDexAndroidContextAttachment. It never creates or claims Activity,
Resources, AssetManager, files/OBB Java File objects or attachment readiness.
The full context host still requires the independent real Activity.attach
and live Java object verifier; all missing services return unavailable.

The host regression uses simulated Java witnesses to test exact field
verification and rejection; it does not prove production Android readiness.

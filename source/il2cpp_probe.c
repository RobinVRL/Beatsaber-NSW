/* IL2CPP runtime introspection (diagnostic).
 * SPDX-License-Identifier: MIT
 *
 * Global metadata v39 (Unity 6.3) is not readable by offline dumpers, so the
 * running game's own IL2CPP API is queried instead: every class whose name
 * matches a filter gets its fields and methods (with code offsets inside
 * libil2cpp.so, matching llvm-objdump addresses) written to loader.log. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "il2cpp_probe.h"
#include "so_util.h"
#include "util.h"

typedef void *(*FnDomainGet)(void);
typedef void **(*FnDomainGetAssemblies)(void *domain, size_t *size);
typedef void *(*FnAssemblyGetImage)(void *assembly);
typedef size_t (*FnImageGetClassCount)(void *image);
typedef void *(*FnImageGetClass)(void *image, size_t index);
typedef const char *(*FnImageGetName)(void *image);
typedef const char *(*FnClassGetName)(void *klass);
typedef const char *(*FnClassGetNamespace)(void *klass);
typedef void *(*FnClassGetMethods)(void *klass, void **iter);
typedef void *(*FnClassGetFields)(void *klass, void **iter);
typedef void *(*FnClassGetParent)(void *klass);
typedef const char *(*FnMethodGetName)(void *method);
typedef uint32_t (*FnMethodGetParamCount)(void *method);
typedef const char *(*FnFieldGetName)(void *field);
typedef size_t (*FnFieldGetOffset)(void *field);
typedef void *(*FnThreadAttach)(void *domain);

static so_module *g_il2cpp;

static void *sym(const char *name) {
    return (void *)so_find_module_export(g_il2cpp, name);
}

static int match(const char *name, const char *const *filters) {
    for (; *filters; ++filters)
        if (strstr(name, *filters)) return 1;
    return 0;
}

void bs_il2cpp_probe(const char *const *filters) {
    g_il2cpp = so_find_module_by_name("libil2cpp.so");
    if (!g_il2cpp) { debugPrintf("[il2cpp:probe] libil2cpp.so not loaded\n"); return; }
    FnDomainGet domain_get = (FnDomainGet)sym("il2cpp_domain_get");
    FnDomainGetAssemblies get_asms = (FnDomainGetAssemblies)sym("il2cpp_domain_get_assemblies");
    FnAssemblyGetImage asm_image = (FnAssemblyGetImage)sym("il2cpp_assembly_get_image");
    FnImageGetClassCount class_count = (FnImageGetClassCount)sym("il2cpp_image_get_class_count");
    FnImageGetClass get_class = (FnImageGetClass)sym("il2cpp_image_get_class");
    FnImageGetName image_name = (FnImageGetName)sym("il2cpp_image_get_name");
    FnClassGetName class_name = (FnClassGetName)sym("il2cpp_class_get_name");
    FnClassGetNamespace class_ns = (FnClassGetNamespace)sym("il2cpp_class_get_namespace");
    FnClassGetMethods methods = (FnClassGetMethods)sym("il2cpp_class_get_methods");
    FnClassGetFields fields = (FnClassGetFields)sym("il2cpp_class_get_fields");
    FnClassGetParent parent = (FnClassGetParent)sym("il2cpp_class_get_parent");
    FnMethodGetName method_name = (FnMethodGetName)sym("il2cpp_method_get_name");
    FnMethodGetParamCount param_count = (FnMethodGetParamCount)sym("il2cpp_method_get_param_count");
    FnFieldGetName field_name = (FnFieldGetName)sym("il2cpp_field_get_name");
    FnFieldGetOffset field_offset = (FnFieldGetOffset)sym("il2cpp_field_get_offset");
    FnThreadAttach attach = (FnThreadAttach)sym("il2cpp_thread_attach");
    if (!domain_get || !get_asms || !asm_image || !class_count || !get_class ||
        !class_name || !methods || !method_name || !attach) {
        debugPrintf("[il2cpp:probe] API exports missing\n");
        return;
    }
    void *domain = domain_get();
    attach(domain);
    uintptr_t base = (uintptr_t)g_il2cpp->load_virtbase;
    size_t n_asm = 0;
    void **asms = get_asms(domain, &n_asm);
    debugPrintf("[il2cpp:probe] %zu assemblies, code base %p\n", n_asm, (void *)base);
    for (size_t a = 0; a < n_asm; ++a) {
        void *image = asm_image(asms[a]);
        size_t nc = class_count(image);
        for (size_t c = 0; c < nc; ++c) {
            void *k = get_class(image, c);
            const char *cn = k ? class_name(k) : NULL;
            if (!cn || !match(cn, filters)) continue;
            void *p = parent ? parent(k) : NULL;
            debugPrintf("[il2cpp:probe] class %s.%s : %s (%s)\n", class_ns ? class_ns(k) : "", cn,
                        p ? class_name(p) : "-", image_name ? image_name(image) : "?");
            void *it = NULL, *f;
            while (fields && (f = fields(k, &it)))
                debugPrintf("[il2cpp:probe]   field +%#zx %s\n",
                            field_offset ? field_offset(f) : 0, field_name(f));
            it = NULL;
            void *m;
            while ((m = methods(k, &it))) {
                uintptr_t ptr = *(uintptr_t *)m; /* MethodInfo::methodPointer */
                debugPrintf("[il2cpp:probe]   method %s/%u @ %#lx\n", method_name(m),
                            param_count ? param_count(m) : 0,
                            ptr ? (unsigned long)(ptr - base) : 0ul);
            }
        }
    }
}

typedef void *(*FnResolveIcall)(const char *name);
typedef struct { const void *begin; int32_t length; } BsSpan;

void bs_il2cpp_probe_xr_devices(void) {
    g_il2cpp = so_find_module_by_name("libil2cpp.so");
    if (!g_il2cpp) return;
    FnResolveIcall resolve = (FnResolveIcall)sym("il2cpp_resolve_icall");
    if (!resolve) { debugPrintf("[il2cpp:xr] no il2cpp_resolve_icall\n"); return; }
    uint64_t (*dev_at_node)(int) = (uint64_t (*)(int))resolve(
        "UnityEngine.XR.InputTracking::GetDeviceIdAtXRNode(UnityEngine.XR.XRNode)");
    uint8_t (*valid)(uint64_t) = (uint8_t (*)(uint64_t))resolve(
        "UnityEngine.XR.InputDevices::IsDeviceValid(System.UInt64)");
    uint32_t (*chars)(uint64_t) = (uint32_t (*)(uint64_t))resolve(
        "UnityEngine.XR.InputDevices::GetDeviceCharacteristics(System.UInt64)");
    uint8_t (*get_bool)(uint64_t, BsSpan *, uint8_t *) =
        (uint8_t (*)(uint64_t, BsSpan *, uint8_t *))resolve(
            "UnityEngine.XR.InputDevices::TryGetFeatureValue_bool_Injected(System.UInt64,"
            "UnityEngine.Bindings.ManagedSpanWrapper&,System.Boolean&)");
    if (!dev_at_node || !valid || !chars || !get_bool) {
        debugPrintf("[il2cpp:xr] icalls missing %p %p %p %p\n", (void *)dev_at_node,
                    (void *)valid, (void *)chars, (void *)get_bool);
        return;
    }
    void (*get_manu)(uint64_t, BsSpan *) = (void (*)(uint64_t, BsSpan *))resolve(
        "UnityEngine.XR.InputDevices::GetDeviceManufacturer_Injected(System.UInt64,"
        "UnityEngine.Bindings.ManagedSpanWrapper&)");
    void (*get_name)(uint64_t, BsSpan *) = (void (*)(uint64_t, BsSpan *))resolve(
        "UnityEngine.XR.InputDevices::GetDeviceName_Injected(System.UInt64,"
        "UnityEngine.Bindings.ManagedSpanWrapper&)");
    void (*node_states)(void *) = (void (*)(void *))resolve(
        "UnityEngine.XR.InputTracking::GetNodeStates_Internal_Injected("
        "UnityEngine.Bindings.BlittableListWrapper&)");
    if (node_states) {
        static uint8_t buf[16 * 96];
        struct { void *data; int32_t size; int32_t flags; int32_t list; } w = {buf, 16, 0, 0};
        memset(buf, 0, sizeof buf);
        node_states(&w);
        debugPrintf("[il2cpp:xr] node states: list=%d size=%d flags=%d\n", w.list, w.size, w.flags);
        for (int n = 0; n < w.size && n < 16; ++n) {
            const float *f = (const float *)(buf + n * 96);
            const int32_t *iv = (const int32_t *)(buf + n * 96);
            debugPrintf("[il2cpp:xr]  type=%d avail=%#x pos=(%.2f %.2f %.2f) rot=(%.2f %.2f %.2f %.2f) tracked=%d\n",
                        iv[0], iv[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], iv[21]);
        }
    }
    static const uint16_t tracked[] = {'I','s','T','r','a','c','k','e','d'};
    static const uint16_t primary[] = {'U','s','e','r','P','r','e','s','e','n','c','e'};
    for (int node = 3; node <= 5; ++node) {
        uint64_t id = dev_at_node(node);
        uint8_t ok = id ? valid(id) : 0, t = 0, up = 0;
        BsSpan s1 = {tracked, 9}, s2 = {primary, 12};
        uint8_t has_t = ok ? get_bool(id, &s1, &t) : 0;
        uint8_t has_up = ok ? get_bool(id, &s2, &up) : 0;
        debugPrintf("[il2cpp:xr] node %d id=%#llx valid=%d chars=%#x IsTracked=%d/%d UserPresence=%d/%d\n",
                    node, (unsigned long long)id, ok, ok ? chars(id) : 0, has_t, t, has_up, up);
        for (int k = 0; ok && k < 2; ++k) {
            BsSpan out = {0};
            void (*fn)(uint64_t, BsSpan *) = k ? get_name : get_manu;
            if (!fn) continue;
            fn(id, &out);
            char a[96] = {0};
            const uint16_t *w = (const uint16_t *)out.begin;
            for (int j = 0; w && j < out.length && j < 95; ++j) a[j] = (char)w[j];
            debugPrintf("[il2cpp:xr]   %s='%s' (len %d)\n", k ? "name" : "manufacturer", a, out.length);
        }
    }
}
typedef void *(*FnClassGetType)(void *klass);
typedef void *(*FnTypeGetObject)(void *type);

static void *find_class(const char *want) {
    FnDomainGet domain_get = (FnDomainGet)sym("il2cpp_domain_get");
    FnDomainGetAssemblies get_asms = (FnDomainGetAssemblies)sym("il2cpp_domain_get_assemblies");
    FnAssemblyGetImage asm_image = (FnAssemblyGetImage)sym("il2cpp_assembly_get_image");
    FnImageGetClassCount class_count = (FnImageGetClassCount)sym("il2cpp_image_get_class_count");
    FnImageGetClass get_class = (FnImageGetClass)sym("il2cpp_image_get_class");
    FnClassGetName class_name = (FnClassGetName)sym("il2cpp_class_get_name");
    size_t n_asm = 0;
    void **asms = get_asms(domain_get(), &n_asm);
    for (size_t a = 0; a < n_asm; ++a) {
        void *image = asm_image(asms[a]);
        size_t nc = class_count(image);
        for (size_t c = 0; c < nc; ++c) {
            void *k = get_class(image, c);
            if (k && !strcmp(class_name(k), want)) return k;
        }
    }
    return NULL;
}

typedef void *(*FnMethodFromName)(void *klass, const char *name, int argc);
typedef void *(*FnRuntimeInvoke)(void *method, void *obj, void **params, void **exc);
typedef void *(*FnObjectGetClass)(void *obj);
static void invoke_str(void *obj, const char *method, char *out, size_t cap);
void bs_il2cpp_probe_objects(const char *const *class_names) {
    g_il2cpp = so_find_module_by_name("libil2cpp.so");
    if (!g_il2cpp) return;
    FnResolveIcall resolve = (FnResolveIcall)sym("il2cpp_resolve_icall");
    FnClassGetType get_type = (FnClassGetType)sym("il2cpp_class_get_type");
    FnTypeGetObject type_obj = (FnTypeGetObject)sym("il2cpp_type_get_object");
    void *(*find)(void *, int, int) = (void *(*)(void *, int, int))resolve(
        "UnityEngine.Object::FindObjectsByType(System.Type,UnityEngine.FindObjectsInactive,"
        "UnityEngine.FindObjectsSortMode)");
    void *(*get_go)(void *) = (void *(*)(void *))resolve(
        "UnityEngine.Component::get_gameObject_Injected(System.IntPtr)");
    uint8_t (*active)(void *) = (uint8_t (*)(void *))resolve(
        "UnityEngine.GameObject::get_activeInHierarchy_Injected(System.IntPtr)");
    void (*get_name)(void *, BsSpan *) = (void (*)(void *, BsSpan *))resolve(
        "UnityEngine.Object::GetName_Injected(System.IntPtr,UnityEngine.Bindings.ManagedSpanWrapper&)");
    if (!get_type || !type_obj || !find || !get_go || !active || !get_name) {
        debugPrintf("[il2cpp:obj] API missing\n");
        return;
    }
    for (; *class_names; ++class_names) {
        void *k = find_class(*class_names);
        if (!k) { debugPrintf("[il2cpp:obj] class %s not found\n", *class_names); continue; }
        uint8_t *arr = find(type_obj(get_type(k)), 1, 0);
        size_t n = arr ? *(size_t *)(arr + 0x18) : 0;
        debugPrintf("[il2cpp:obj] %s: %zu object(s)\n", *class_names, n);
        for (size_t i = 0; i < n && i < 8; ++i) {
            uint8_t *obj = ((uint8_t **)(arr + 0x20))[i];
            void *native = obj ? *(void **)(obj + 0x10) : NULL;
            if (!native) continue;
            char act[32];
            { void *mg = NULL, *exc = NULL; FnMethodFromName mfn2 = (FnMethodFromName)sym("il2cpp_class_get_method_from_name");
              FnClassGetParent par2 = (FnClassGetParent)sym("il2cpp_class_get_parent");
              for (void *kk = ((FnObjectGetClass)sym("il2cpp_object_get_class"))(obj); kk && !mg; kk = par2(kk)) mg = mfn2(kk, "get_gameObject", 0);
              void *gom = mg ? ((FnRuntimeInvoke)sym("il2cpp_runtime_invoke"))(mg, obj, NULL, &exc) : NULL;
              invoke_str(gom, "get_activeInHierarchy", act, sizeof act); }
            void *go = get_go(native);
            /* get_gameObject_Injected returns the managed wrapper. */
            void *go_native = go ? *(void **)((uint8_t *)go + 0x10) : NULL;
            BsSpan s = {0};
            get_name(native, &s);
            char nm[64] = {0};
            for (int j = 0; s.begin && j < s.length && j < 63; ++j) nm[j] = (char)((const uint16_t *)s.begin)[j];
            char hex[3 * 0x70 + 1] = {0};
            for (int j = 0; j < 0x70; ++j) snprintf(hex + 3 * j, 4, "%02x ", obj[0x18 + j]);
            debugPrintf("[il2cpp:obj]  [%zu] '%s' active=%d +0x18: %s\n", i, nm,
                        go_native ? active(go_native) : -1, hex);
            debugPrintf("[il2cpp:obj]      activeInHierarchy=%s\n", act);
        }
    }
}
typedef void *(*FnFieldFromName)(void *klass, const char *name);
typedef void (*FnFieldStaticGet)(void *field, void *out);
typedef void (*FnFieldGetValue)(void *obj, void *field, void *out);
typedef void *(*FnObjectGetClass)(void *obj);

static void utf16_str(void *s, char *out, size_t cap) {
    out[0] = 0;
    if (!s) return;
    int32_t n = *(int32_t *)((uint8_t *)s + 0x10);
    const uint16_t *w = (const uint16_t *)((uint8_t *)s + 0x14);
    size_t i = 0;
    for (; i + 1 < cap && (int32_t)i < n; ++i) out[i] = (char)w[i];
    out[i] = 0;
}

static void invoke_str(void *obj, const char *method, char *out, size_t cap);
typedef void *(*FnMethodFromName)(void *klass, const char *name, int argc);
typedef void *(*FnRuntimeInvoke)(void *method, void *obj, void **params, void **exc);
void bs_il2cpp_probe_input_system(void) {
    g_il2cpp = so_find_module_by_name("libil2cpp.so");
    if (!g_il2cpp) return;
    FnFieldFromName field = (FnFieldFromName)sym("il2cpp_class_get_field_from_name");
    FnFieldStaticGet sget = (FnFieldStaticGet)sym("il2cpp_field_static_get_value");
    FnFieldGetValue get = (FnFieldGetValue)sym("il2cpp_field_get_value");
    FnObjectGetClass obj_class = (FnObjectGetClass)sym("il2cpp_object_get_class");
    FnClassGetName class_name = (FnClassGetName)sym("il2cpp_class_get_name");
    void *is = find_class("InputSystem");
    if (!is || !field || !sget || !get) { debugPrintf("[il2cpp:is] InputSystem missing\n"); return; }
    void *f = field(is, "s_Manager");
    void *mgr = NULL;
    if (f) sget(f, &mgr);
    if (!mgr) {
        void *it = NULL, *ff;
        FnClassGetFields fields = (FnClassGetFields)sym("il2cpp_class_get_fields");
        FnFieldGetName fname = (FnFieldGetName)sym("il2cpp_field_get_name");
        while ((ff = fields(is, &it))) debugPrintf("[il2cpp:is] InputSystem field %s\n", fname(ff));
        return;
    }
    void *mk = obj_class(mgr);
    int32_t count = 0;
    void *devs = NULL;
    void *fc = field(mk, "m_DevicesCount"), *fd = field(mk, "m_Devices");
    if (fc) get(mgr, fc, &count);
    if (fd) get(mgr, fd, &devs);
    uint8_t has_focus = 0xff;
    void *fh = field(mk, "m_HasFocus");
    if (fh) get(mgr, fh, &has_focus);
    FnResolveIcall resolve = (FnResolveIcall)sym("il2cpp_resolve_icall");
    uint8_t (*app_focused)(void) = (uint8_t (*)(void))resolve("UnityEngine.Application::get_isFocused()");
    uint8_t (*app_bg)(void) = (uint8_t (*)(void))resolve("UnityEngine.Application::get_runInBackground()");
    debugPrintf("[il2cpp:is] manager %s devices=%d m_HasFocus=%d Application.isFocused=%d runInBackground=%d\n",
                class_name(mk), count, has_focus, app_focused ? app_focused() : -1, app_bg ? app_bg() : -1);
    for (int32_t i = 0; devs && i < count && i < 24; ++i) {
        void *d = ((void **)((uint8_t *)devs + 0x20))[i];
        if (!d) continue;
        void *dk = obj_class(d);
        void *name = NULL, *layout = NULL;
        double last = 0;
        int32_t flags = 0, id = 0;
        void *fn = field(dk, "m_Name"), *fl = field(dk, "m_Layout"),
             *ft = field(dk, "m_LastUpdateTimeInternal"), *ff = field(dk, "m_DeviceFlags"),
             *fi = field(dk, "m_DeviceId");
        if (fn) get(d, fn, &name);
        if (ft) get(d, ft, &last);
        if (ff) get(d, ff, &flags);
        if (fi) get(d, fi, &id);
        char a[96], b[96] = {0};
        utf16_str(name, a, sizeof a);
        (void)fl; (void)layout;
        char en[32], bg[32];
        invoke_str(d, "get_enabled", en, sizeof en);
        invoke_str(d, "get_canRunInBackground", bg, sizeof bg);
        debugPrintf("[il2cpp:is]  dev %d id=%d class=%s name='%s' flags=%#x lastUpdate=%.3f enabled=%s canRunInBackground=%s\n", i, id,
                    class_name(dk), a, flags, last, en, bg);
        static const char *const ctl[] = {"devicePosition", "deviceRotation", "trigger", "isTracked", "trackingState", "pointerPosition", "devicePosition/x", "devicePosition/y", "deviceRotation/w"};
        void *(*str_new)(const char *) = (void *(*)(const char *))sym("il2cpp_string_new");
        FnMethodFromName mfn = (FnMethodFromName)sym("il2cpp_class_get_method_from_name");
        FnRuntimeInvoke inv = (FnRuntimeInvoke)sym("il2cpp_runtime_invoke");
        FnClassGetParent parent = (FnClassGetParent)sym("il2cpp_class_get_parent");
        void *m = NULL;
        for (void *kk = dk; kk && !m; kk = parent(kk)) m = mfn(kk, "TryGetChildControl", 1);
        for (size_t j = 0; m && i >= 2 && j < sizeof ctl / sizeof ctl[0]; ++j) {
            void *arg = str_new(ctl[j]), *exc = NULL;
            void *params[1] = {arg};
            void *control = inv(m, d, params, &exc);
            char val[128];
            if (exc || !control) { snprintf(val, sizeof val, "%s", exc ? "(exc)" : "(none)"); }
            else {
                invoke_str(control, "ReadValue", val, sizeof val);
                if (val[0] == '(') invoke_str(control, "get_value", val, sizeof val);
            }
            uint32_t sb[4] = {0};
            if (control) {
                void *fsb = NULL;
                for (void *kk = obj_class(control); kk && !fsb; kk = parent(kk)) fsb = field(kk, "m_StateBlock");
                if (fsb) get(control, fsb, sb);
            }
            if (control && sb[0] == 0x56454333u) { /* 'VEC3' */
                typedef struct { float x, y, z; } V3;
                void *mu = NULL, *mr = NULL, *mcs2 = NULL;
                for (void *kk = obj_class(control); kk && !mu; kk = parent(kk)) mu = mfn(kk, "ReadUnprocessedValueFromState", 1);
                for (void *kk = obj_class(control); kk && !mr; kk = parent(kk)) mr = mfn(kk, "ReadValue", 0);
                for (void *kk = obj_class(control); kk && !mcs2; kk = parent(kk)) mcs2 = mfn(kk, "get_currentStatePtr", 0);
                void *sp2 = mcs2 ? ((void *(*)(void *, void *))(*(void **)mcs2))(control, mcs2) : NULL;
                V3 u = {0}, rv = {0};
                if (mu && sp2) u = ((V3 (*)(void *, void *, void *))(*(void **)mu))(control, sp2, mu);
                if (mr) rv = ((V3 (*)(void *, void *))(*(void **)mr))(control, mr);
                const uint8_t *co = (const uint8_t *)control;
                debugPrintf("[il2cpp:is]     direct: statePtr=%p unprocessed=(%.2f %.2f %.2f) ReadValue=(%.2f %.2f %.2f) +0xd8=%.4s\n",
                            sp2, u.x, u.y, u.z, rv.x, rv.y, rv.z, (const char *)(co + 0xd8));
                char hx[3 * 0x100 + 1] = {0};
                for (int q = 0; q < 0x100; ++q) snprintf(hx + 3 * q, 4, "%02x ", co[0x10 + q]);
                debugPrintf("[il2cpp:is]     obj: %s\n", hx);
            }
            debugPrintf("[il2cpp:is]    %s = %s  [fmt=%.4s off=%u bit=%u size=%u]\n", ctl[j], val,
                        (const char *)&sb[0], sb[1], sb[2], sb[3]);
        }
        if (i >= 2) {
            uint32_t sb[4] = {0};
            void *fsb = NULL;
            for (void *kk = dk; kk && !fsb; kk = parent(kk)) fsb = field(kk, "m_StateBlock");
            if (fsb) get(d, fsb, sb);
            void *mcs = NULL;
            for (void *kk = dk; kk && !mcs; kk = parent(kk)) mcs = mfn(kk, "get_currentStatePtr", 0);
            uint8_t *state = mcs ? ((uint8_t *(*)(void *, void *))(*(void **)mcs))(d, mcs) : NULL;
            debugPrintf("[il2cpp:is]   device state fmt=%.4s off=%u size=%u ptr=%p\n",
                        (const char *)&sb[0], sb[1], sb[3], (void *)state);
            for (uint32_t o = 0; state && o < sb[3] / 8 && o < 256; o += 32) {
                char hx[3 * 32 + 1] = {0};
                for (int q = 0; q < 32; ++q) snprintf(hx + 3 * q, 4, "%02x ", state[sb[1] + o + q]);
                debugPrintf("[il2cpp:is]    +%03u: %s\n", o, hx);
            }
        }
        (void)b;
    }
}
static void invoke_str(void *obj, const char *method, char *out, size_t cap) {
    FnMethodFromName mfn = (FnMethodFromName)sym("il2cpp_class_get_method_from_name");
    FnRuntimeInvoke inv = (FnRuntimeInvoke)sym("il2cpp_runtime_invoke");
    FnObjectGetClass obj_class = (FnObjectGetClass)sym("il2cpp_object_get_class");
    out[0] = 0;
    if (!obj) { snprintf(out, cap, "(null)"); return; }
    void *k = obj_class(obj), *m = NULL;
    FnClassGetParent parent = (FnClassGetParent)sym("il2cpp_class_get_parent");
    for (; k && !m; k = parent(k)) m = mfn(k, method, 0);
    if (!m) { snprintf(out, cap, "(no %s)", method); return; }
    void *exc = NULL;
    uint8_t (*is_vt)(void *) = (uint8_t (*)(void *))sym("il2cpp_class_is_valuetype");
    void *(*unbox)(void *) = (void *(*)(void *))sym("il2cpp_object_unbox");
    void *self = obj;
    if (is_vt && unbox && is_vt(obj_class(obj))) self = unbox(obj);
    void *r = inv(m, self, NULL, &exc);
    if (exc) { snprintf(out, cap, "(exception)"); return; }
    if (!strcmp(method, "ToString")) { utf16_str(r, out, cap); return; }
    /* boxed result: value at +0x10 */
    if (!r) { snprintf(out, cap, "null"); return; }
    char s[256];
    invoke_str(r, "ToString", s, sizeof s);
    snprintf(out, cap, "%s", s);
}

void bs_il2cpp_probe_helper_actions(void) {
    g_il2cpp = so_find_module_by_name("libil2cpp.so");
    if (!g_il2cpp) return;
    FnResolveIcall resolve = (FnResolveIcall)sym("il2cpp_resolve_icall");
    FnClassGetType get_type = (FnClassGetType)sym("il2cpp_class_get_type");
    FnTypeGetObject type_obj = (FnTypeGetObject)sym("il2cpp_type_get_object");
    FnObjectGetClass obj_class = (FnObjectGetClass)sym("il2cpp_object_get_class");
    FnClassGetName class_name = (FnClassGetName)sym("il2cpp_class_get_name");
    void *(*find)(void *, int, int) = (void *(*)(void *, int, int))resolve(
        "UnityEngine.Object::FindObjectsByType(System.Type,UnityEngine.FindObjectsInactive,"
        "UnityEngine.FindObjectsSortMode)");
    void *k = find_class("UnityXRHelper");
    uint8_t *arr = k ? find(type_obj(get_type(k)), 1, 0) : NULL;
    if (!arr || !*(size_t *)(arr + 0x18)) { debugPrintf("[il2cpp:act] no UnityXRHelper\n"); return; }
    uint8_t *helper = ((uint8_t **)(arr + 0x20))[0];
    static const struct { int off; const char *what; } fields[] = {
        {0x60, "leftController"}, {0x68, "rightController"}, {0x70, "headPositionAction"},
        {0x78, "headOrientationAction"}};
    char s[512], v[256], e[64];
    for (size_t i = 0; i < 4; ++i) {
        uint8_t *o = *(uint8_t **)(helper + fields[i].off);
        if (!o) { debugPrintf("[il2cpp:act] %s null\n", fields[i].what); continue; }
        if (i < 2) {
            debugPrintf("[il2cpp:act] %s class=%s\n", fields[i].what, class_name(obj_class(o)));
            for (int off = 0x10; off <= 0x40; off += 8) {
                void *a = *(void **)(o + off);
                if (!a || (uintptr_t)a < 0x1000000) continue;
                const char *cn = class_name(obj_class(a));
                if (strcmp(cn, "InputAction")) { debugPrintf("[il2cpp:act]   +%#x %s\n", off, cn); continue; }
                invoke_str(a, "ToString", s, sizeof s);
                invoke_str(a, "ReadValueAsObject", v, sizeof v);
                invoke_str(a, "get_enabled", e, sizeof e);
                char ph[64], ac[160];
                invoke_str(a, "get_phase", ph, sizeof ph);
                { void *mac = NULL, *exc = NULL; FnClassGetParent par = (FnClassGetParent)sym("il2cpp_class_get_parent");
                  FnMethodFromName mfn = (FnMethodFromName)sym("il2cpp_class_get_method_from_name");
                  for (void *kk = obj_class(a); kk && !mac; kk = par(kk)) mac = mfn(kk, "get_activeControl", 0);
                  void *ctl = mac ? ((FnRuntimeInvoke)sym("il2cpp_runtime_invoke"))(mac, a, NULL, &exc) : NULL;
                  if (ctl) invoke_str(ctl, "ToString", ac, sizeof ac); else snprintf(ac, sizeof ac, "null"); }
                debugPrintf("[il2cpp:act]   +%#x %s enabled=%s phase=%s active=%s value=%s\n", off, s, e, ph, ac, v);
            }
        } else {
            invoke_str(o, "ToString", s, sizeof s);
            invoke_str(o, "ReadValueAsObject", v, sizeof v);
            invoke_str(o, "get_enabled", e, sizeof e);
            debugPrintf("[il2cpp:act] %s %s enabled=%s value=%s\n", fields[i].what, s, e, v);
        }
    }
}
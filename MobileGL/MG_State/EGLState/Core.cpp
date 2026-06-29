// MobileGL - MobileGL/MG_State/EGLState/Core.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "Core.h"

namespace MobileGL {
    namespace MG_State {
        namespace EGLState {
            namespace {
                constexpr EGLint EGL_DISPLAY_MAJOR_VERSION = 1;
                constexpr EGLint EGL_DISPLAY_MINOR_VERSION = 5;
                constexpr EGLint DEFAULT_MAX_PBUFFER_SIZE = 16384;

                template <typename AttrType>
                Optional<AttrType> ParseAttribValue(const AttrType* attribList, EGLint attrib) {
                    if (!attribList) {
                        return Nullopt;
                    }
                    for (SizeT i = 0; attribList[i] != EGL_NONE; i += 2) {
                        if (attribList[i] == static_cast<AttrType>(attrib)) {
                            return attribList[i + 1];
                        }
                    }
                    return Nullopt;
                }

                EGLint QueryDefaultX11VisualId() {
#if defined(__linux__) && !defined(__ANDROID__)
                    const char* displayName = std::getenv("DISPLAY");
                    if (!displayName) {
                        return 0;
                    }

                    void* x11Lib = dlopen("libX11.so.6", RTLD_LOCAL | RTLD_NOW);
                    if (!x11Lib) {
                        x11Lib = dlopen("libX11.so", RTLD_LOCAL | RTLD_NOW);
                    }
                    if (!x11Lib) {
                        return 0;
                    }

                    using XOpenDisplayFn = void* (*)(const char*);
                    using XDefaultScreenFn = int (*)(void*);
                    using XDefaultVisualFn = void* (*)(void*, int);
                    using XVisualIDFromVisualFn = unsigned long (*)(void*);
                    using XCloseDisplayFn = int (*)(void*);

                    auto* xOpenDisplay = reinterpret_cast<XOpenDisplayFn>(dlsym(x11Lib, "XOpenDisplay"));
                    auto* xDefaultScreen = reinterpret_cast<XDefaultScreenFn>(dlsym(x11Lib, "XDefaultScreen"));
                    auto* xDefaultVisual = reinterpret_cast<XDefaultVisualFn>(dlsym(x11Lib, "XDefaultVisual"));
                    auto* xVisualIDFromVisual =
                        reinterpret_cast<XVisualIDFromVisualFn>(dlsym(x11Lib, "XVisualIDFromVisual"));
                    auto* xCloseDisplay = reinterpret_cast<XCloseDisplayFn>(dlsym(x11Lib, "XCloseDisplay"));
                    if (!xOpenDisplay || !xDefaultScreen || !xDefaultVisual || !xVisualIDFromVisual || !xCloseDisplay) {
                        dlclose(x11Lib);
                        return 0;
                    }

                    void* display = xOpenDisplay(displayName);
                    if (!display) {
                        dlclose(x11Lib);
                        return 0;
                    }

                    const int screen = xDefaultScreen(display);
                    void* visual = xDefaultVisual(display, screen);
                    const auto visualId = visual ? static_cast<EGLint>(xVisualIDFromVisual(visual)) : 0;
                    xCloseDisplay(display);
                    dlclose(x11Lib);
                    return visualId;
#else
                    return 0;
#endif
                }
            } // namespace

            Bool EGLContext::DisplayLookupKey::operator==(const DisplayLookupKey& rhs) const {
                return this->NativeDisplayKey == rhs.NativeDisplayKey && this->Platform == rhs.Platform;
            }

            SizeT EGLContext::DisplayLookupHasher::operator()(const DisplayLookupKey& key) const {
                const auto nativeHash = std::hash<Uint64>{}(key.NativeDisplayKey);
                const auto platformHash = std::hash<Uint64>{}(static_cast<Uint64>(key.Platform));
                return nativeHash ^ (platformHash + 0x9e3779b97f4a7c15ull + (nativeHash << 6) + (nativeHash >> 2));
            }

            std::thread::id EGLContext::CurrentThreadKey() {
                return std::this_thread::get_id();
            }

            void EGLContext::SetError(EGLint errorCode) {
                if (errorCode == EGL_SUCCESS) {
                    return;
                }
                const std::lock_guard<std::recursive_mutex> lock(m_mutex);
                m_threadErrors[CurrentThreadKey()] = errorCode;
            }

            EGLint EGLContext::ConsumeError() {
                const std::lock_guard<std::recursive_mutex> lock(m_mutex);
                const auto threadKey = CurrentThreadKey();
                auto errorIt = m_threadErrors.find(threadKey);
                if (errorIt == m_threadErrors.end()) {
                    return EGL_SUCCESS;
                }

                const EGLint error = errorIt->second;
                m_threadErrors.erase(errorIt);
                return error;
            }

            void EGLContext::SetBoundAPI(EGLenum api) {
                const std::lock_guard<std::recursive_mutex> lock(m_mutex);
                m_threadBoundAPI[CurrentThreadKey()] = api;
            }

            EGLenum EGLContext::GetBoundAPI() const {
                const std::lock_guard<std::recursive_mutex> lock(m_mutex);
                auto apiIt = m_threadBoundAPI.find(CurrentThreadKey());
                if (apiIt == m_threadBoundAPI.end()) {
                    return EGL_OPENGL_API;
                }
                return apiIt->second;
            }

            EGLContext::EGLDisplayHandle EGLContext::GetOrCreateDisplay(Uint64 nativeDisplayKey, EGLenum platform) {
                const DisplayLookupKey key = {
                    .NativeDisplayKey = nativeDisplayKey,
                    .Platform = platform,
                };

                auto lookupIt = m_displayLookup.find(key);
                if (lookupIt != m_displayLookup.end()) {
                    return lookupIt->second;
                }

                const auto display = EncodeHandle<EGLDisplayHandle>(m_nextDisplayHandle++);
                DisplayObject displayObject = {
                    .NativeDisplayKey = nativeDisplayKey,
                    .Platform = platform,
                    .Initialized = false,
                    .MajorVersion = EGL_DISPLAY_MAJOR_VERSION,
                    .MinorVersion = EGL_DISPLAY_MINOR_VERSION,
                    .SwapInterval = 1,
                };

                displayObject.Configs.push_back(CreateDefaultConfig(display, 1, 0));
                displayObject.Configs.push_back(CreateDefaultConfig(display, 2, 8));

                m_displays[display] = displayObject;
                m_displayLookup[key] = display;
                return display;
            }

            EGLContext::EGLConfigHandle EGLContext::CreateDefaultConfig(EGLDisplayHandle display, EGLint configId,
                                                                        EGLint stencilSize) {
                const auto config = EncodeHandle<EGLConfigHandle>(m_nextConfigHandle++);
                ConfigObject cfg = {
                    .Display = display,
                    .ConfigId = configId,
                    .RedSize = 8,
                    .GreenSize = 8,
                    .BlueSize = 8,
                    .AlphaSize = 8,
                    .DepthSize = 24,
                    .StencilSize = stencilSize,
                    .SurfaceType = EGL_WINDOW_BIT | EGL_PBUFFER_BIT | EGL_PIXMAP_BIT,
                    .RenderableType = EGL_OPENGL_BIT | EGL_OPENGL_ES2_BIT | EGL_OPENGL_ES3_BIT,
                    .MinSwapInterval = 0,
                    .MaxSwapInterval = 4,
                    .NativeVisualId = QueryDefaultX11VisualId(),
                };
#if defined(ANDROID) || defined(__ANDROID__)
                cfg.NativeVisualId = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
#endif
                m_configs[config] = cfg;
                return config;
            }

            EGLContext::DisplayObject* EGLContext::TryGetDisplay(EGLDisplayHandle display) {
                auto it = m_displays.find(display);
                return it == m_displays.end() ? nullptr : &it->second;
            }

            const EGLContext::DisplayObject* EGLContext::TryGetDisplay(EGLDisplayHandle display) const {
                auto it = m_displays.find(display);
                return it == m_displays.end() ? nullptr : &it->second;
            }

            const EGLContext::ConfigObject* EGLContext::TryGetConfig(EGLConfigHandle config) const {
                auto it = m_configs.find(config);
                return it == m_configs.end() ? nullptr : &it->second;
            }

            const EGLContext::ContextObject* EGLContext::TryGetContext(EGLContextHandle context) const {
                auto it = m_contexts.find(context);
                return it == m_contexts.end() ? nullptr : &it->second;
            }

            const EGLContext::SurfaceObject* EGLContext::TryGetSurface(EGLSurfaceHandle surface) const {
                auto it = m_surfaces.find(surface);
                return it == m_surfaces.end() ? nullptr : &it->second;
            }

            const EGLContext::SyncObject* EGLContext::TryGetSync(EGLSyncHandle sync) const {
                auto it = m_syncs.find(sync);
                return it == m_syncs.end() ? nullptr : &it->second;
            }

            const EGLContext::ImageObject* EGLContext::TryGetImage(EGLImageHandle image) const {
                auto it = m_images.find(image);
                return it == m_images.end() ? nullptr : &it->second;
            }

            void EGLContext::ReleaseThreadUnlocked(const std::thread::id& threadKey) {
                auto currentIt = m_threadCurrents.find(threadKey);
                if (currentIt == m_threadCurrents.end()) {
                    return;
                }

                const EGLContextHandle context = currentIt->second.Context;
                if (context != nullptr) {
                    auto ownerIt = m_contextOwners.find(context);
                    if (ownerIt != m_contextOwners.end() && ownerIt->second == threadKey) {
                        m_contextOwners.erase(ownerIt);
                    }
                }
                m_threadCurrents.erase(currentIt);
            }

            void EGLContext::ReleaseDisplayObjects(EGLDisplayHandle display) {
                Vector<EGLContextHandle> removedContexts;
                for (auto contextIt = m_contexts.begin(); contextIt != m_contexts.end();) {
                    if (contextIt->second.Display == display) {
                        removedContexts.push_back(contextIt->first);
                        contextIt = m_contexts.erase(contextIt);
                    } else {
                        ++contextIt;
                    }
                }

                for (const auto context : removedContexts) {
                    m_contextOwners.erase(context);
                }

                for (auto surfaceIt = m_surfaces.begin(); surfaceIt != m_surfaces.end();) {
                    if (surfaceIt->second.Display == display) {
                        surfaceIt = m_surfaces.erase(surfaceIt);
                    } else {
                        ++surfaceIt;
                    }
                }

                for (auto syncIt = m_syncs.begin(); syncIt != m_syncs.end();) {
                    if (syncIt->second.Display == display) {
                        syncIt = m_syncs.erase(syncIt);
                    } else {
                        ++syncIt;
                    }
                }

                for (auto imageIt = m_images.begin(); imageIt != m_images.end();) {
                    if (imageIt->second.Display == display) {
                        imageIt = m_images.erase(imageIt);
                    } else {
                        ++imageIt;
                    }
                }

                for (auto threadIt = m_threadCurrents.begin(); threadIt != m_threadCurrents.end();) {
                    if (threadIt->second.Display == display) {
                        if (threadIt->second.Context != nullptr) {
                            auto ownerIt = m_contextOwners.find(threadIt->second.Context);
                            if (ownerIt != m_contextOwners.end() && ownerIt->second == threadIt->first) {
                                m_contextOwners.erase(ownerIt);
                            }
                        }
                        threadIt = m_threadCurrents.erase(threadIt);
                    } else {
                        ++threadIt;
                    }
                }
            }

            EGLContext::EGLDisplayHandle EGLContext::GetDisplay(NativeDisplayType nativeDisplay) {
                const std::lock_guard<std::recursive_mutex> lock(m_mutex);
                return GetOrCreateDisplay(ToNativeKey(nativeDisplay), EGL_NONE);
            }

            EGLContext::EGLDisplayHandle EGLContext::GetPlatformDisplay(EGLenum platform, void* nativeDisplay) {
                const std::lock_guard<std::recursive_mutex> lock(m_mutex);
                return GetOrCreateDisplay(ToNativeKey(nativeDisplay), platform);
            }

            Bool EGLContext::ValidateDisplay(EGLDisplayHandle display) const {
                const std::lock_guard<std::recursive_mutex> lock(m_mutex);
                return m_displays.find(display) != m_displays.end();
            }

            Bool EGLContext::IsDisplayInitialized(EGLDisplayHandle display) const {
                const std::lock_guard<std::recursive_mutex> lock(m_mutex);
                const auto* displayObject = TryGetDisplay(display);
                return displayObject && displayObject->Initialized;
            }

            Bool EGLContext::InitializeDisplay(EGLDisplayHandle display, EGLint* major, EGLint* minor) {
                const std::lock_guard<std::recursive_mutex> lock(m_mutex);
                auto* displayObject = TryGetDisplay(display);
                if (!displayObject) {
                    SetError(EGL_BAD_DISPLAY);
                    return false;
                }

                displayObject->Initialized = true;
                displayObject->MajorVersion = EGL_DISPLAY_MAJOR_VERSION;
                displayObject->MinorVersion = EGL_DISPLAY_MINOR_VERSION;

                if (major) {
                    *major = displayObject->MajorVersion;
                }
                if (minor) {
                    *minor = displayObject->MinorVersion;
                }
                return true;
            }

            Bool EGLContext::TerminateDisplay(EGLDisplayHandle display) {
                const std::lock_guard<std::recursive_mutex> lock(m_mutex);
                auto* displayObject = TryGetDisplay(display);
                if (!displayObject) {
                    SetError(EGL_BAD_DISPLAY);
                    return false;
                }

                ReleaseDisplayObjects(display);
                displayObject->Initialized = false;
                return true;
            }

            Bool EGLContext::ChooseConfig(EGLDisplayHandle display, const EGLint* attribList, EGLConfigHandle* configs,
                                          EGLint configSize, EGLint* numConfig) {
                const std::lock_guard<std::recursive_mutex> lock(m_mutex);
                auto* displayObject = TryGetDisplay(display);
                if (!displayObject) {
                    SetError(EGL_BAD_DISPLAY);
                    return false;
                }
                if (!displayObject->Initialized) {
                    SetError(EGL_NOT_INITIALIZED);
                    return false;
                }
                if (!numConfig) {
                    SetError(EGL_BAD_PARAMETER);
                    return false;
                }

                Vector<EGLConfigHandle> matchedConfigs;
                for (const auto config : displayObject->Configs) {
                    const auto* cfg = TryGetConfig(config);
                    if (!cfg) {
                        continue;
                    }

                    Bool matches = true;
                    if (attribList) {
                        for (SizeT i = 0; attribList[i] != EGL_NONE; i += 2) {
                            const EGLint attr = attribList[i];
                            const EGLint requested = attribList[i + 1];
                            if (requested == EGL_DONT_CARE) {
                                continue;
                            }

                            switch (attr) {
                            case EGL_CONFIG_ID:
                                matches = cfg->ConfigId == requested;
                                break;
                            case EGL_RED_SIZE:
                                matches = cfg->RedSize >= requested;
                                break;
                            case EGL_GREEN_SIZE:
                                matches = cfg->GreenSize >= requested;
                                break;
                            case EGL_BLUE_SIZE:
                                matches = cfg->BlueSize >= requested;
                                break;
                            case EGL_ALPHA_SIZE:
                                matches = cfg->AlphaSize >= requested;
                                break;
                            case EGL_BUFFER_SIZE:
                                matches = (cfg->RedSize + cfg->GreenSize + cfg->BlueSize + cfg->AlphaSize) >= requested;
                                break;
                            case EGL_DEPTH_SIZE:
                                matches = cfg->DepthSize >= requested;
                                break;
                            case EGL_STENCIL_SIZE:
                                matches = cfg->StencilSize >= requested;
                                break;
                            case EGL_SURFACE_TYPE:
                                matches = (cfg->SurfaceType & requested) == requested;
                                break;
                            case EGL_RENDERABLE_TYPE:
                            case EGL_CONFORMANT:
                                matches = (cfg->RenderableType & requested) == requested;
                                break;
                            case EGL_SAMPLE_BUFFERS:
                            case EGL_SAMPLES:
                                matches = requested == 0;
                                break;
                            case EGL_NATIVE_VISUAL_ID:
                                matches = cfg->NativeVisualId == requested;
                                break;
                            default:
                                break;
                            }

                            if (!matches) {
                                break;
                            }
                        }
                    }

                    if (matches) {
                        matchedConfigs.push_back(config);
                    }
                }

                *numConfig = static_cast<EGLint>(matchedConfigs.size());
                if (!configs || configSize <= 0) {
                    return true;
                }

                const SizeT copyCount = std::min(static_cast<SizeT>(configSize), matchedConfigs.size());
                for (SizeT i = 0; i < copyCount; ++i) {
                    configs[i] = matchedConfigs[i];
                }
                return true;
            }

            Bool EGLContext::GetConfigs(EGLDisplayHandle display, EGLConfigHandle* configs, EGLint configSize,
                                        EGLint* numConfig) {
                const std::lock_guard<std::recursive_mutex> lock(m_mutex);
                return ChooseConfig(display, nullptr, configs, configSize, numConfig);
            }

            Bool EGLContext::ValidateConfig(EGLConfigHandle config) const {
                const std::lock_guard<std::recursive_mutex> lock(m_mutex);
                return m_configs.find(config) != m_configs.end();
            }

            Bool EGLContext::ValidateConfigOnDisplay(EGLDisplayHandle display, EGLConfigHandle config) const {
                const std::lock_guard<std::recursive_mutex> lock(m_mutex);
                const auto* cfg = TryGetConfig(config);
                return cfg && cfg->Display == display;
            }

            Bool EGLContext::GetConfigAttrib(EGLDisplayHandle display, EGLConfigHandle config, EGLint attribute,
                                             EGLint* value) const {
                const std::lock_guard<std::recursive_mutex> lock(m_mutex);
                if (!value) {
                    const_cast<EGLContext*>(this)->SetError(EGL_BAD_PARAMETER);
                    return false;
                }
                const auto* cfg = TryGetConfig(config);
                if (!cfg) {
                    const_cast<EGLContext*>(this)->SetError(EGL_BAD_CONFIG);
                    return false;
                }
                if (cfg->Display != display) {
                    const_cast<EGLContext*>(this)->SetError(EGL_BAD_MATCH);
                    return false;
                }

                switch (attribute) {
                case EGL_BUFFER_SIZE:
                    *value = cfg->RedSize + cfg->GreenSize + cfg->BlueSize + cfg->AlphaSize;
                    return true;
                case EGL_ALPHA_MASK_SIZE:
                    *value = 0;
                    return true;
                case EGL_BIND_TO_TEXTURE_RGB:
                case EGL_BIND_TO_TEXTURE_RGBA:
                    *value = EGL_FALSE;
                    return true;
                case EGL_COLOR_BUFFER_TYPE:
                    *value = EGL_RGB_BUFFER;
                    return true;
                case EGL_CONFIG_CAVEAT:
                    *value = EGL_NONE;
                    return true;
                case EGL_CONFIG_ID:
                    *value = cfg->ConfigId;
                    return true;
                case EGL_LEVEL:
                    *value = 0;
                    return true;
                case EGL_LUMINANCE_SIZE:
                    *value = 0;
                    return true;
                case EGL_MAX_PBUFFER_WIDTH:
                case EGL_MAX_PBUFFER_HEIGHT:
                    *value = DEFAULT_MAX_PBUFFER_SIZE;
                    return true;
                case EGL_MAX_PBUFFER_PIXELS:
                    *value = DEFAULT_MAX_PBUFFER_SIZE * DEFAULT_MAX_PBUFFER_SIZE;
                    return true;
                case EGL_RED_SIZE:
                    *value = cfg->RedSize;
                    return true;
                case EGL_GREEN_SIZE:
                    *value = cfg->GreenSize;
                    return true;
                case EGL_BLUE_SIZE:
                    *value = cfg->BlueSize;
                    return true;
                case EGL_ALPHA_SIZE:
                    *value = cfg->AlphaSize;
                    return true;
                case EGL_DEPTH_SIZE:
                    *value = cfg->DepthSize;
                    return true;
                case EGL_STENCIL_SIZE:
                    *value = cfg->StencilSize;
                    return true;
                case EGL_SURFACE_TYPE:
                    *value = cfg->SurfaceType;
                    return true;
                case EGL_RENDERABLE_TYPE:
                case EGL_CONFORMANT:
                    *value = cfg->RenderableType;
                    return true;
                case EGL_MIN_SWAP_INTERVAL:
                    *value = cfg->MinSwapInterval;
                    return true;
                case EGL_MAX_SWAP_INTERVAL:
                    *value = cfg->MaxSwapInterval;
                    return true;
                case EGL_NATIVE_RENDERABLE:
                    *value = EGL_TRUE;
                    return true;
                case EGL_NATIVE_VISUAL_ID:
                    *value = cfg->NativeVisualId;
                    return true;
                case EGL_NATIVE_VISUAL_TYPE:
                    *value = cfg->NativeVisualId;
                    return true;
                case EGL_SAMPLE_BUFFERS:
                case EGL_SAMPLES:
                    *value = 0;
                    return true;
                case EGL_TRANSPARENT_TYPE:
                    *value = EGL_NONE;
                    return true;
                case EGL_TRANSPARENT_RED_VALUE:
                case EGL_TRANSPARENT_GREEN_VALUE:
                case EGL_TRANSPARENT_BLUE_VALUE:
                    *value = 0;
                    return true;
                default:
                    const_cast<EGLContext*>(this)->SetError(EGL_BAD_ATTRIBUTE);
                    return false;
                }
            }

            EGLContext::EGLContextHandle EGLContext::CreateContext(EGLDisplayHandle display, EGLConfigHandle config,
                                                                   EGLContextHandle shareCtx,
                                                                   const EGLint* attribList) {
                const std::lock_guard<std::recursive_mutex> lock(m_mutex);
                const auto* displayObject = TryGetDisplay(display);
                if (!displayObject) {
                    SetError(EGL_BAD_DISPLAY);
                    return nullptr;
                }
                if (!displayObject->Initialized) {
                    SetError(EGL_NOT_INITIALIZED);
                    return nullptr;
                }
                if (!ValidateConfigOnDisplay(display, config)) {
                    SetError(EGL_BAD_CONFIG);
                    return nullptr;
                }
                if (shareCtx != nullptr && !ValidateContextOnDisplay(display, shareCtx)) {
                    SetError(EGL_BAD_CONTEXT);
                    return nullptr;
                }

                ContextObject contextObject = {
                    .Display = display,
                    .Config = config,
                    .SharedContext = shareCtx,
                    .ClientAPI = GetBoundAPI(),
                    .ClientVersion = 1,
                    .MajorVersion = 1,
                    .MinorVersion = 0,
                };

                if (auto value = ParseAttribValue(attribList, EGL_CONTEXT_CLIENT_VERSION); value) {
                    contextObject.ClientVersion = *value;
                    contextObject.MajorVersion = *value;
                }
                if (auto value = ParseAttribValue(attribList, EGL_CONTEXT_MAJOR_VERSION); value) {
                    contextObject.MajorVersion = *value;
                }
                if (auto value = ParseAttribValue(attribList, EGL_CONTEXT_MINOR_VERSION); value) {
                    contextObject.MinorVersion = *value;
                }

                const auto context = EncodeHandle<EGLContextHandle>(m_nextContextHandle++);
                m_contexts[context] = contextObject;
                return context;
            }

            Bool EGLContext::DestroyContext(EGLDisplayHandle display, EGLContextHandle context) {
                const std::lock_guard<std::recursive_mutex> lock(m_mutex);
                auto contextIt = m_contexts.find(context);
                if (contextIt == m_contexts.end()) {
                    SetError(EGL_BAD_CONTEXT);
                    return false;
                }
                if (contextIt->second.Display != display) {
                    SetError(EGL_BAD_MATCH);
                    return false;
                }

                auto ownerIt = m_contextOwners.find(context);
                if (ownerIt != m_contextOwners.end()) {
                    SetError(EGL_BAD_ACCESS);
                    return false;
                }
                m_contexts.erase(contextIt);
                return true;
            }

            Bool EGLContext::QueryContext(EGLDisplayHandle display, EGLContextHandle context, EGLint attribute,
                                          EGLint* value) const {
                const std::lock_guard<std::recursive_mutex> lock(m_mutex);
                if (!value) {
                    const_cast<EGLContext*>(this)->SetError(EGL_BAD_PARAMETER);
                    return false;
                }
                const auto* contextObject = TryGetContext(context);
                if (!contextObject) {
                    const_cast<EGLContext*>(this)->SetError(EGL_BAD_CONTEXT);
                    return false;
                }
                if (contextObject->Display != display) {
                    const_cast<EGLContext*>(this)->SetError(EGL_BAD_MATCH);
                    return false;
                }

                switch (attribute) {
                case EGL_CONTEXT_CLIENT_TYPE:
                    *value = static_cast<EGLint>(contextObject->ClientAPI);
                    return true;
                case EGL_CONTEXT_CLIENT_VERSION:
#if EGL_CONTEXT_MAJOR_VERSION != EGL_CONTEXT_CLIENT_VERSION
                case EGL_CONTEXT_MAJOR_VERSION:
#endif
                    *value = contextObject->MajorVersion;
                    return true;
                case EGL_CONTEXT_MINOR_VERSION:
                    *value = contextObject->MinorVersion;
                    return true;
                case EGL_CONFIG_ID: {
                    const auto* cfg = TryGetConfig(contextObject->Config);
                    if (!cfg) {
                        const_cast<EGLContext*>(this)->SetError(EGL_BAD_CONFIG);
                        return false;
                    }
                    *value = cfg->ConfigId;
                    return true;
                }
                default:
                    const_cast<EGLContext*>(this)->SetError(EGL_BAD_ATTRIBUTE);
                    return false;
                }
            }

            Bool EGLContext::ValidateContext(EGLContextHandle context) const {
                const std::lock_guard<std::recursive_mutex> lock(m_mutex);
                return m_contexts.find(context) != m_contexts.end();
            }

            Bool EGLContext::ValidateContextOnDisplay(EGLDisplayHandle display, EGLContextHandle context) const {
                const std::lock_guard<std::recursive_mutex> lock(m_mutex);
                const auto* ctx = TryGetContext(context);
                return ctx && ctx->Display == display;
            }

            EGLContext::EGLSurfaceHandle EGLContext::CreateWindowSurface(EGLDisplayHandle display,
                                                                         EGLConfigHandle config,
                                                                         NativeWindowType window,
                                                                         const EGLint* attribList) {
                const std::lock_guard<std::recursive_mutex> lock(m_mutex);
                (void)attribList;
                if (!IsDisplayInitialized(display)) {
                    SetError(EGL_NOT_INITIALIZED);
                    return EGL_NO_SURFACE;
                }
                if (!ValidateConfigOnDisplay(display, config)) {
                    SetError(EGL_BAD_CONFIG);
                    return EGL_NO_SURFACE;
                }
                if (ToNativeKey(window) == 0) {
                    SetError(EGL_BAD_NATIVE_WINDOW);
                    return EGL_NO_SURFACE;
                }

                const auto surface = EncodeHandle<EGLSurfaceHandle>(m_nextSurfaceHandle++);
                m_surfaces[surface] = SurfaceObject{
                    .Display = display,
                    .Config = config,
                    .Type = SurfaceType::Window,
                    .NativeHandleKey = ToNativeKey(window),
                    .ClientBuffer = nullptr,
                    .BufferType = EGL_NONE,
                };
                return surface;
            }

            EGLContext::EGLSurfaceHandle EGLContext::CreatePbufferSurface(EGLDisplayHandle display,
                                                                          EGLConfigHandle config,
                                                                          const EGLint* attribList) {
                const std::lock_guard<std::recursive_mutex> lock(m_mutex);
                if (!IsDisplayInitialized(display)) {
                    SetError(EGL_NOT_INITIALIZED);
                    return EGL_NO_SURFACE;
                }
                if (!ValidateConfigOnDisplay(display, config)) {
                    SetError(EGL_BAD_CONFIG);
                    return EGL_NO_SURFACE;
                }

                EGLint width = 1;
                EGLint height = 1;
                if (auto value = ParseAttribValue(attribList, EGL_WIDTH); value) {
                    width = *value;
                }
                if (auto value = ParseAttribValue(attribList, EGL_HEIGHT); value) {
                    height = *value;
                }
                if (width < 0 || height < 0) {
                    SetError(EGL_BAD_ATTRIBUTE);
                    return EGL_NO_SURFACE;
                }

                const auto surface = EncodeHandle<EGLSurfaceHandle>(m_nextSurfaceHandle++);
                m_surfaces[surface] = SurfaceObject{
                    .Display = display,
                    .Config = config,
                    .Type = SurfaceType::Pbuffer,
                    .NativeHandleKey = 0,
                    .ClientBuffer = nullptr,
                    .BufferType = EGL_NONE,
                    .Width = width,
                    .Height = height,
                    .TextureFormat = ParseAttribValue(attribList, EGL_TEXTURE_FORMAT).value_or(EGL_NO_TEXTURE),
                    .TextureTarget = ParseAttribValue(attribList, EGL_TEXTURE_TARGET).value_or(EGL_NO_TEXTURE),
                    .MipmapLevel = ParseAttribValue(attribList, EGL_MIPMAP_LEVEL).value_or(0),
                    .MipmapTexture = ParseAttribValue(attribList, EGL_MIPMAP_TEXTURE).value_or(EGL_FALSE),
                };
                return surface;
            }

            EGLContext::EGLSurfaceHandle EGLContext::CreatePixmapSurface(EGLDisplayHandle display,
                                                                         EGLConfigHandle config,
                                                                         EGLNativePixmapType pixmap,
                                                                         const EGLint* attribList) {
                const std::lock_guard<std::recursive_mutex> lock(m_mutex);
                (void)attribList;
                if (!IsDisplayInitialized(display)) {
                    SetError(EGL_NOT_INITIALIZED);
                    return EGL_NO_SURFACE;
                }
                if (!ValidateConfigOnDisplay(display, config)) {
                    SetError(EGL_BAD_CONFIG);
                    return EGL_NO_SURFACE;
                }
                if (ToNativeKey(pixmap) == 0) {
                    SetError(EGL_BAD_NATIVE_PIXMAP);
                    return EGL_NO_SURFACE;
                }

                const auto surface = EncodeHandle<EGLSurfaceHandle>(m_nextSurfaceHandle++);
                m_surfaces[surface] = SurfaceObject{
                    .Display = display,
                    .Config = config,
                    .Type = SurfaceType::Pixmap,
                    .NativeHandleKey = ToNativeKey(pixmap),
                    .ClientBuffer = nullptr,
                    .BufferType = EGL_NONE,
                };
                return surface;
            }

            EGLContext::EGLSurfaceHandle EGLContext::CreatePbufferFromClientBuffer(EGLDisplayHandle display,
                                                                                   EGLenum bufferType,
                                                                                   EGLClientBuffer buffer,
                                                                                   EGLConfigHandle config,
                                                                                   const EGLint* attribList) {
                const std::lock_guard<std::recursive_mutex> lock(m_mutex);
                if (!IsDisplayInitialized(display)) {
                    SetError(EGL_NOT_INITIALIZED);
                    return EGL_NO_SURFACE;
                }
                if (!ValidateConfigOnDisplay(display, config)) {
                    SetError(EGL_BAD_CONFIG);
                    return EGL_NO_SURFACE;
                }
                if (!buffer) {
                    SetError(EGL_BAD_PARAMETER);
                    return EGL_NO_SURFACE;
                }

                EGLint width = 1;
                EGLint height = 1;
                if (auto value = ParseAttribValue(attribList, EGL_WIDTH); value) {
                    width = *value;
                }
                if (auto value = ParseAttribValue(attribList, EGL_HEIGHT); value) {
                    height = *value;
                }

                const auto surface = EncodeHandle<EGLSurfaceHandle>(m_nextSurfaceHandle++);
                m_surfaces[surface] = SurfaceObject{
                    .Display = display,
                    .Config = config,
                    .Type = SurfaceType::PbufferFromClientBuffer,
                    .NativeHandleKey = 0,
                    .ClientBuffer = buffer,
                    .BufferType = bufferType,
                    .Width = width,
                    .Height = height,
                };
                return surface;
            }

            EGLContext::EGLSurfaceHandle EGLContext::CreatePlatformWindowSurface(EGLDisplayHandle display,
                                                                                 EGLConfigHandle config,
                                                                                 void* nativeWindow,
                                                                                 const EGLAttrib* attribList) {
                const std::lock_guard<std::recursive_mutex> lock(m_mutex);
                if (!IsDisplayInitialized(display)) {
                    SetError(EGL_NOT_INITIALIZED);
                    return EGL_NO_SURFACE;
                }
                if (!ValidateConfigOnDisplay(display, config)) {
                    SetError(EGL_BAD_CONFIG);
                    return EGL_NO_SURFACE;
                }
                if (ToNativeKey(nativeWindow) == 0) {
                    SetError(EGL_BAD_NATIVE_WINDOW);
                    return EGL_NO_SURFACE;
                }

                const auto surface = EncodeHandle<EGLSurfaceHandle>(m_nextSurfaceHandle++);
                m_surfaces[surface] = SurfaceObject{
                    .Display = display,
                    .Config = config,
                    .Type = SurfaceType::PlatformWindow,
                    .NativeHandleKey = ToNativeKey(nativeWindow),
                    .ClientBuffer = nullptr,
                    .BufferType = EGL_NONE,
                    .Width = static_cast<EGLint>(ParseAttribValue(attribList, EGL_WIDTH).value_or(0)),
                    .Height = static_cast<EGLint>(ParseAttribValue(attribList, EGL_HEIGHT).value_or(0)),
                };
                return surface;
            }

            EGLContext::EGLSurfaceHandle EGLContext::CreatePlatformPixmapSurface(EGLDisplayHandle display,
                                                                                 EGLConfigHandle config,
                                                                                 void* nativePixmap,
                                                                                 const EGLAttrib* attribList) {
                const std::lock_guard<std::recursive_mutex> lock(m_mutex);
                (void)attribList;
                if (!IsDisplayInitialized(display)) {
                    SetError(EGL_NOT_INITIALIZED);
                    return EGL_NO_SURFACE;
                }
                if (!ValidateConfigOnDisplay(display, config)) {
                    SetError(EGL_BAD_CONFIG);
                    return EGL_NO_SURFACE;
                }
                if (ToNativeKey(nativePixmap) == 0) {
                    SetError(EGL_BAD_NATIVE_PIXMAP);
                    return EGL_NO_SURFACE;
                }

                const auto surface = EncodeHandle<EGLSurfaceHandle>(m_nextSurfaceHandle++);
                m_surfaces[surface] = SurfaceObject{
                    .Display = display,
                    .Config = config,
                    .Type = SurfaceType::PlatformPixmap,
                    .NativeHandleKey = ToNativeKey(nativePixmap),
                    .ClientBuffer = nullptr,
                    .BufferType = EGL_NONE,
                };
                return surface;
            }

            Bool EGLContext::DestroySurface(EGLDisplayHandle display, EGLSurfaceHandle surface) {
                const std::lock_guard<std::recursive_mutex> lock(m_mutex);
                auto surfaceIt = m_surfaces.find(surface);
                if (surfaceIt == m_surfaces.end()) {
                    SetError(EGL_BAD_SURFACE);
                    return false;
                }
                if (surfaceIt->second.Display != display) {
                    SetError(EGL_BAD_MATCH);
                    return false;
                }

                for (auto currentIt = m_threadCurrents.begin(); currentIt != m_threadCurrents.end();) {
                    if (currentIt->second.DrawSurface == surface) {
                        currentIt->second.DrawSurface = EGL_NO_SURFACE;
                    }
                    if (currentIt->second.ReadSurface == surface) {
                        currentIt->second.ReadSurface = EGL_NO_SURFACE;
                    }

                    const Bool emptyCurrent = currentIt->second.Context == nullptr &&
                                              currentIt->second.DrawSurface == EGL_NO_SURFACE &&
                                              currentIt->second.ReadSurface == EGL_NO_SURFACE;
                    if (emptyCurrent) {
                        currentIt = m_threadCurrents.erase(currentIt);
                        continue;
                    }
                    ++currentIt;
                }
                m_surfaces.erase(surfaceIt);
                return true;
            }

            Bool EGLContext::ResizeSurface(EGLDisplayHandle display, EGLSurfaceHandle surface,
                                           EGLint width, EGLint height) {
                const std::lock_guard<std::recursive_mutex> lock(m_mutex);
                auto surfaceIt = m_surfaces.find(surface);
                if (surfaceIt == m_surfaces.end()) {
                    SetError(EGL_BAD_SURFACE);
                    return false;
                }
                if (surfaceIt->second.Display != display) {
                    SetError(EGL_BAD_MATCH);
                    return false;
                }
                surfaceIt->second.Width = std::max<EGLint>(width, 1);
                surfaceIt->second.Height = std::max<EGLint>(height, 1);
                return true;
            }

            Bool EGLContext::QuerySurface(EGLDisplayHandle display, EGLSurfaceHandle surface, EGLint attribute,
                                          EGLint* value) const {
                const std::lock_guard<std::recursive_mutex> lock(m_mutex);
                if (!value) {
                    const_cast<EGLContext*>(this)->SetError(EGL_BAD_PARAMETER);
                    return false;
                }
                const auto* surfaceObject = TryGetSurface(surface);
                if (!surfaceObject) {
                    const_cast<EGLContext*>(this)->SetError(EGL_BAD_SURFACE);
                    return false;
                }
                if (surfaceObject->Display != display) {
                    const_cast<EGLContext*>(this)->SetError(EGL_BAD_MATCH);
                    return false;
                }

                switch (attribute) {
                case EGL_WIDTH:
                    *value = surfaceObject->Width;
                    return true;
                case EGL_HEIGHT:
                    *value = surfaceObject->Height;
                    return true;
                case EGL_TEXTURE_FORMAT:
                    *value = surfaceObject->TextureFormat;
                    return true;
                case EGL_TEXTURE_TARGET:
                    *value = surfaceObject->TextureTarget;
                    return true;
                case EGL_MIPMAP_LEVEL:
                    *value = surfaceObject->MipmapLevel;
                    return true;
                case EGL_MIPMAP_TEXTURE:
                    *value = surfaceObject->MipmapTexture;
                    return true;
                case EGL_RENDER_BUFFER:
                    *value = surfaceObject->RenderBuffer;
                    return true;
                case EGL_SWAP_BEHAVIOR:
                    *value = surfaceObject->SwapBehavior;
                    return true;
                case EGL_CONFIG_ID: {
                    const auto* cfg = TryGetConfig(surfaceObject->Config);
                    if (!cfg) {
                        const_cast<EGLContext*>(this)->SetError(EGL_BAD_CONFIG);
                        return false;
                    }
                    *value = cfg->ConfigId;
                    return true;
                }
                default:
                    const_cast<EGLContext*>(this)->SetError(EGL_BAD_ATTRIBUTE);
                    return false;
                }
            }

            Bool EGLContext::ValidateSurface(EGLSurfaceHandle surface) const {
                const std::lock_guard<std::recursive_mutex> lock(m_mutex);
                return m_surfaces.find(surface) != m_surfaces.end();
            }

            Bool EGLContext::ValidateSurfaceOnDisplay(EGLDisplayHandle display, EGLSurfaceHandle surface) const {
                const std::lock_guard<std::recursive_mutex> lock(m_mutex);
                const auto* surf = TryGetSurface(surface);
                return surf && surf->Display == display;
            }

            Bool EGLContext::SwapInterval(EGLDisplayHandle display, EGLint interval) {
                const std::lock_guard<std::recursive_mutex> lock(m_mutex);
                auto* displayObject = TryGetDisplay(display);
                if (!displayObject) {
                    SetError(EGL_BAD_DISPLAY);
                    return false;
                }
                if (!displayObject->Initialized) {
                    SetError(EGL_NOT_INITIALIZED);
                    return false;
                }

                if (displayObject->Configs.empty()) {
                    SetError(EGL_BAD_CONFIG);
                    return false;
                }
                const auto* cfg = TryGetConfig(displayObject->Configs.front());
                if (!cfg) {
                    SetError(EGL_BAD_CONFIG);
                    return false;
                }
                if (interval < cfg->MinSwapInterval || interval > cfg->MaxSwapInterval) {
                    SetError(EGL_BAD_PARAMETER);
                    return false;
                }

                displayObject->SwapInterval = interval;
                return true;
            }

            Bool EGLContext::MakeCurrent(EGLDisplayHandle display, EGLSurfaceHandle draw, EGLSurfaceHandle read,
                                         EGLContextHandle context) {
                const std::lock_guard<std::recursive_mutex> lock(m_mutex);
                const Bool releaseCurrentRequest =
                    display == EGL_NO_DISPLAY && draw == EGL_NO_SURFACE && read == EGL_NO_SURFACE && context == nullptr;
                const auto threadKey = CurrentThreadKey();
                if (releaseCurrentRequest) {
                    ReleaseThreadUnlocked(threadKey);
                    return true;
                }

                auto* displayObject = TryGetDisplay(display);
                if (!displayObject) {
                    SetError(EGL_BAD_DISPLAY);
                    return false;
                }
                if (!displayObject->Initialized) {
                    SetError(EGL_NOT_INITIALIZED);
                    return false;
                }

                if (context != nullptr) {
                    if (!ValidateContextOnDisplay(display, context)) {
                        SetError(EGL_BAD_CONTEXT);
                        return false;
                    }
                    if (draw == EGL_NO_SURFACE || read == EGL_NO_SURFACE) {
                        SetError(EGL_BAD_MATCH);
                        return false;
                    }
                } else {
                    if (draw != EGL_NO_SURFACE || read != EGL_NO_SURFACE) {
                        SetError(EGL_BAD_MATCH);
                        return false;
                    }
                }

                if (draw != EGL_NO_SURFACE && !ValidateSurfaceOnDisplay(display, draw)) {
                    SetError(EGL_BAD_SURFACE);
                    return false;
                }
                if (read != EGL_NO_SURFACE && !ValidateSurfaceOnDisplay(display, read)) {
                    SetError(EGL_BAD_SURFACE);
                    return false;
                }

                if (context != nullptr) {
                    auto ownerIt = m_contextOwners.find(context);
                    if (ownerIt != m_contextOwners.end() && ownerIt->second != threadKey) {
                        SetError(EGL_BAD_ACCESS);
                        return false;
                    }
                }

                ReleaseThreadUnlocked(threadKey);

                m_threadCurrents[threadKey] = ThreadCurrentState{
                    .Display = display,
                    .DrawSurface = draw,
                    .ReadSurface = read,
                    .Context = context,
                };
                if (context != nullptr) {
                    m_contextOwners[context] = threadKey;
                }
                return true;
            }

            void EGLContext::ReleaseThread() {
                const std::lock_guard<std::recursive_mutex> lock(m_mutex);
                ReleaseThreadUnlocked(CurrentThreadKey());
            }

            EGLContext::EGLContextHandle EGLContext::GetCurrentContext() const {
                const std::lock_guard<std::recursive_mutex> lock(m_mutex);
                auto currentIt = m_threadCurrents.find(CurrentThreadKey());
                if (currentIt == m_threadCurrents.end()) {
                    return nullptr;
                }
                return currentIt->second.Context;
            }

            EGLContext::EGLDisplayHandle EGLContext::GetCurrentDisplay() const {
                const std::lock_guard<std::recursive_mutex> lock(m_mutex);
                auto currentIt = m_threadCurrents.find(CurrentThreadKey());
                if (currentIt == m_threadCurrents.end()) {
                    return EGL_NO_DISPLAY;
                }
                return currentIt->second.Display;
            }

            EGLContext::EGLSurfaceHandle EGLContext::GetCurrentSurface(EGLint readdraw) const {
                const std::lock_guard<std::recursive_mutex> lock(m_mutex);
                auto currentIt = m_threadCurrents.find(CurrentThreadKey());
                if (currentIt == m_threadCurrents.end()) {
                    return EGL_NO_SURFACE;
                }
                if (readdraw == EGL_DRAW) {
                    return currentIt->second.DrawSurface;
                }
                if (readdraw == EGL_READ) {
                    return currentIt->second.ReadSurface;
                }
                return EGL_NO_SURFACE;
            }

            Bool EGLContext::IsDoubleBufferedSurface(EGLSurfaceHandle surface) const {
                const std::lock_guard<std::recursive_mutex> lock(m_mutex);
                const auto* surfaceObject = TryGetSurface(surface);
                if (!surfaceObject) {
                    return false;
                }

                if (surfaceObject->RenderBuffer != EGL_BACK_BUFFER) {
                    return false;
                }

                return surfaceObject->Type == SurfaceType::Window ||
                       surfaceObject->Type == SurfaceType::PlatformWindow;
            }

            EGLContext::EGLSyncHandle EGLContext::CreateSync(EGLDisplayHandle display, EGLenum type,
                                                             const EGLAttrib* attribList) {
                const std::lock_guard<std::recursive_mutex> lock(m_mutex);
                if (!IsDisplayInitialized(display)) {
                    SetError(EGL_NOT_INITIALIZED);
                    return EGL_NO_SYNC;
                }
                if (type != EGL_SYNC_FENCE) {
                    SetError(EGL_BAD_ATTRIBUTE);
                    return EGL_NO_SYNC;
                }

                SyncObject sync = {
                    .Display = display,
                    .Type = type,
                    .Condition = EGL_SYNC_PRIOR_COMMANDS_COMPLETE,
                    .Status = EGL_SIGNALED,
                };
                if (auto value = ParseAttribValue(attribList, EGL_SYNC_CONDITION); value) {
                    sync.Condition = static_cast<EGLenum>(*value);
                }

                const auto syncHandle = EncodeHandle<EGLSyncHandle>(m_nextSyncHandle++);
                m_syncs[syncHandle] = sync;
                return syncHandle;
            }

            Bool EGLContext::DestroySync(EGLDisplayHandle display, EGLSyncHandle sync) {
                const std::lock_guard<std::recursive_mutex> lock(m_mutex);
                auto syncIt = m_syncs.find(sync);
                if (syncIt == m_syncs.end()) {
                    SetError(EGL_BAD_PARAMETER);
                    return false;
                }
                if (syncIt->second.Display != display) {
                    SetError(EGL_BAD_MATCH);
                    return false;
                }
                m_syncs.erase(syncIt);
                return true;
            }

            EGLint EGLContext::ClientWaitSync(EGLDisplayHandle display, EGLSyncHandle sync, EGLint flags,
                                              EGLTime timeout) {
                const std::lock_guard<std::recursive_mutex> lock(m_mutex);
                (void)flags;
                (void)timeout;

                const auto* syncObject = TryGetSync(sync);
                if (!syncObject) {
                    SetError(EGL_BAD_PARAMETER);
                    return EGL_FALSE;
                }
                if (syncObject->Display != display) {
                    SetError(EGL_BAD_MATCH);
                    return EGL_FALSE;
                }

                return EGL_CONDITION_SATISFIED;
            }

            Bool EGLContext::GetSyncAttrib(EGLDisplayHandle display, EGLSyncHandle sync, EGLint attribute,
                                           EGLAttrib* value) const {
                const std::lock_guard<std::recursive_mutex> lock(m_mutex);
                if (!value) {
                    const_cast<EGLContext*>(this)->SetError(EGL_BAD_PARAMETER);
                    return false;
                }
                const auto* syncObject = TryGetSync(sync);
                if (!syncObject) {
                    const_cast<EGLContext*>(this)->SetError(EGL_BAD_PARAMETER);
                    return false;
                }
                if (syncObject->Display != display) {
                    const_cast<EGLContext*>(this)->SetError(EGL_BAD_MATCH);
                    return false;
                }

                switch (attribute) {
                case EGL_SYNC_TYPE:
                    *value = static_cast<EGLAttrib>(syncObject->Type);
                    return true;
                case EGL_SYNC_STATUS:
                    *value = static_cast<EGLAttrib>(syncObject->Status);
                    return true;
                case EGL_SYNC_CONDITION:
                    *value = static_cast<EGLAttrib>(syncObject->Condition);
                    return true;
                default:
                    const_cast<EGLContext*>(this)->SetError(EGL_BAD_ATTRIBUTE);
                    return false;
                }
            }

            Bool EGLContext::WaitSync(EGLDisplayHandle display, EGLSyncHandle sync, EGLint flags) const {
                const std::lock_guard<std::recursive_mutex> lock(m_mutex);
                (void)flags;
                const auto* syncObject = TryGetSync(sync);
                if (!syncObject) {
                    const_cast<EGLContext*>(this)->SetError(EGL_BAD_PARAMETER);
                    return false;
                }
                if (syncObject->Display != display) {
                    const_cast<EGLContext*>(this)->SetError(EGL_BAD_MATCH);
                    return false;
                }
                return true;
            }

            EGLContext::EGLImageHandle EGLContext::CreateImage(EGLDisplayHandle display, EGLContextHandle context,
                                                               EGLenum target, EGLClientBuffer buffer,
                                                               const EGLAttrib* attribList) {
                const std::lock_guard<std::recursive_mutex> lock(m_mutex);
                (void)attribList;
                if (!IsDisplayInitialized(display)) {
                    SetError(EGL_NOT_INITIALIZED);
                    return EGL_NO_IMAGE;
                }
                if (context != nullptr && !ValidateContextOnDisplay(display, context)) {
                    SetError(EGL_BAD_CONTEXT);
                    return EGL_NO_IMAGE;
                }

                const auto image = EncodeHandle<EGLImageHandle>(m_nextImageHandle++);
                m_images[image] = ImageObject{
                    .Display = display,
                    .Context = context,
                    .Target = target,
                    .Buffer = buffer,
                };
                return image;
            }

            Bool EGLContext::DestroyImage(EGLDisplayHandle display, EGLImageHandle image) {
                const std::lock_guard<std::recursive_mutex> lock(m_mutex);
                auto imageIt = m_images.find(image);
                if (imageIt == m_images.end()) {
                    SetError(EGL_BAD_PARAMETER);
                    return false;
                }
                if (imageIt->second.Display != display) {
                    SetError(EGL_BAD_MATCH);
                    return false;
                }
                m_images.erase(imageIt);
                return true;
            }
        } // namespace EGLState

        UniquePtr<EGLState::EGLContext> pEGLContext;
    } // namespace MG_State
} // namespace MobileGL

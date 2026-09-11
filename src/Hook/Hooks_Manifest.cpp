#include "Hooks_Manifest.h"
#include "HookMacros.h"
#include "dllmain.h"
#include "Utils/Config/Config.h"
#include "Utils/Manifest/ManifestDecompressor.h"
#include "Utils/SteamMetadata/GitHubManifestClient.h"
#include <format>
#include <unordered_set>

// ═══════════════════════════════════════════════════════════════════
//  Manifest override hooks:
//    BuildDepotDependency — patches depot entries' gid/size directly
//      in the output vector (replaces the old KV-tree approach).
// ═══════════════════════════════════════════════════════════════════
namespace {

    std::string DepotEntryDebug(const DepotEntry& e) {
        return std::format("DepotId={} AppId={} Gid={} Size={} Dlc={} Lcs={} Carry={} Shared={}",
            e.DepotId, e.AppId, e.ManifestGid, e.ManifestSize, e.DlcAppId,
            (int)e.LcsRequired, (int)e.bNotNewTarget, (int)e.SharedInstall);
    }

    HOOK_FUNC(BuildDepotDependency, bool, void* pUserAppMgr, AppId_t AppId,
              void* pUserConfig, CUtlVector<DepotEntry>* pDepotInfo,
              CUtlVector<DepotEntry>* pSharedDepotInfo, void* pSteamApp,
              uint32* pBuildId, bool* pbBetaFallback)
    {
        bool result = oBuildDepotDependency(pUserAppMgr, AppId, pUserConfig,
            pDepotInfo, pSharedDepotInfo, pSteamApp, pBuildId, pbBetaFallback);

        LOG_MANIFEST_TRACE("BuildDepotDependency: AppId={} pUserConfig=0x{:X} result={} pSteamApp=0x{:X} pBuildId={} pbBetaFallback={}",
            AppId, (uintptr_t)pUserConfig, result, (uintptr_t)pSteamApp,
            pBuildId ? *pBuildId : 0, pbBetaFallback ? *pbBetaFallback : false);
        if (pDepotInfo) {
            LOG_MANIFEST_TRACE("pDepotInfo->nCount={}", pDepotInfo->m_Size);
            for (uint32 i = 0; i < pDepotInfo->m_Size; ++i) {
                LOG_MANIFEST_TRACE("  [{}] {}", i, DepotEntryDebug(pDepotInfo->m_Memory.m_pMemory[i]));
            }
        }
        if (pSharedDepotInfo) {
            LOG_MANIFEST_TRACE("pSharedDepotInfo->nCount={}", pSharedDepotInfo->m_Size);
            for (uint32 i = 0; i < pSharedDepotInfo->m_Size; ++i) {
                LOG_MANIFEST_TRACE("  shared[{}] {}", i, DepotEntryDebug(pSharedDepotInfo->m_Memory.m_pMemory[i]));
            }
        }

        if (!result) return result;

        // Synchronize manifests on active install or update
        if (pSteamApp && Config::IsGitHubManifestRepo()) {
            constexpr uint32 kActiveInstallFlags =
                k_EAppStateUpdateRequired | k_EAppStateUpdateQueued |
                k_EAppStateUpdateRunning  | k_EAppStateUpdateStarted |
                k_EAppStateDownloading    | k_EAppStatePreallocating |
                k_EAppStateReconfiguring;

            uint32 appState = *reinterpret_cast<const uint32*>(reinterpret_cast<const uint8*>(pSteamApp) + 8);
            if ((appState & kActiveInstallFlags) != 0) {
                std::unordered_set<uint32> syncedApps;
                auto syncApp = [&](uint32 targetAppId) {
                    if (targetAppId && !syncedApps.contains(targetAppId)) {
                        syncedApps.insert(targetAppId);
                        if (LuaConfig::HasDepot(targetAppId, false)) {
                            LOG_MANIFEST_DEBUG("BuildDepotDependency: syncing manifests for AppID {}", targetAppId);
                            GitHubManifestClient::EnsureManifestsForApp(targetAppId, false);
                        }
                    }
                };

                syncApp(AppId);
                if (pDepotInfo) {
                    for (uint32 i = 0; i < pDepotInfo->m_Size; ++i) {
                        syncApp(pDepotInfo->m_Memory.m_pMemory[i].AppId);
                    }
                }
                if (pSharedDepotInfo) {
                    for (uint32 i = 0; i < pSharedDepotInfo->m_Size; ++i) {
                        syncApp(pSharedDepotInfo->m_Memory.m_pMemory[i].AppId);
                    }
                }
            }
        }

        const auto& overrides = LuaConfig::GetManifestOverrides();
        if (!overrides.empty() && pDepotInfo && pDepotInfo->m_Size) {
            for (uint32 i = 0; i < pDepotInfo->m_Size; ++i) {
                DepotEntry& e = pDepotInfo->m_Memory.m_pMemory[i];
                auto it = overrides.find(e.DepotId);
                if (it != overrides.end()) {
                    // if size=0 in the override, keep the original size(affects download display but not the actual download)
                    uint64_t newSize = it->second.size ? it->second.size : e.ManifestSize;
                    LOG_MANIFEST_INFO("BuildDepotDependency: patching depot {} gid={}->{} size={}->{}",
                        e.DepotId, e.ManifestGid, it->second.gid,
                        e.ManifestSize, newSize);
                    e.ManifestGid  = it->second.gid;
                    e.ManifestSize = newSize;
                }
            }
        }
        if (!overrides.empty() && pSharedDepotInfo && pSharedDepotInfo->m_Size) {
            for (uint32 i = 0; i < pSharedDepotInfo->m_Size; ++i) {
                DepotEntry& e = pSharedDepotInfo->m_Memory.m_pMemory[i];
                auto it = overrides.find(e.DepotId);
                if (it != overrides.end()) {
                    uint64_t newSize = it->second.size ? it->second.size : e.ManifestSize;
                    LOG_MANIFEST_INFO("BuildDepotDependency: patching shared depot {} gid={}->{} size={}->{}",
                        e.DepotId, e.ManifestGid, it->second.gid,
                        e.ManifestSize, newSize);
                    e.ManifestGid  = it->second.gid;
                    e.ManifestSize = newSize;
                }
            }
        }

        // Sanitize local depotcache manifest files before Steam loads them
        if (pDepotInfo && pDepotInfo->m_Size) {
            for (uint32 i = 0; i < pDepotInfo->m_Size; ++i) {
                const DepotEntry& e = pDepotInfo->m_Memory.m_pMemory[i];
                ManifestDecompressor::SanitizeDepotManifest(e.DepotId, e.ManifestGid);
            }
        }
        if (pSharedDepotInfo && pSharedDepotInfo->m_Size) {
            for (uint32 i = 0; i < pSharedDepotInfo->m_Size; ++i) {
                const DepotEntry& e = pSharedDepotInfo->m_Memory.m_pMemory[i];
                ManifestDecompressor::SanitizeDepotManifest(e.DepotId, e.ManifestGid);
            }
        }

        return result;
    }

} // anonymous namespace

namespace Hooks_Manifest {

    void Install() {
        HOOK_BEGIN();
        INSTALL_HOOK_C(BuildDepotDependency);
        HOOK_END();
    }

    void Uninstall() {
        UNHOOK_BEGIN();
        UNINSTALL_HOOK(BuildDepotDependency);
        UNHOOK_END();
    }
}

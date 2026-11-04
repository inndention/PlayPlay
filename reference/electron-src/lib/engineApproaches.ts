import type { EngineApproachesConfig, RamdiskEngineConfig } from './types';

/** Merge input: top-level partials plus deep-partial `ramdisk` patches from UI. */
export type EngineApproachesMergeInput = Partial<Omit<EngineApproachesConfig, 'ramdisk'>> & {
    ramdisk?: Partial<RamdiskEngineConfig>;
};

const DEFAULT_RAMDISK: RamdiskEngineConfig = {
    root: 'B:/',
    casparMediaFolder: 'PlayPlay-caspar-media',
    enableCasparMediaOnRamdisk: false,
    electronTempFolder: 'PlayPlay-electron-temp',
    enableElectronTempOnRamdisk: false,
};

export const DEFAULT_ENGINE_APPROACHES: EngineApproachesConfig = {
    casparTransport: true,
    casparAutoconfigureOnLaunch: true,
    casparHourAutoPush: true,
    casparStartupCatchup: true,
    mpvBroadcastEngine: false,
    mpvFallbackToElectron: true,
    ramdisk: { ...DEFAULT_RAMDISK },
};

function mergeRamdisk(partial?: Partial<RamdiskEngineConfig>): RamdiskEngineConfig {
    return { ...DEFAULT_RAMDISK, ...partial };
}

export function mergeEngineApproaches(partial?: EngineApproachesMergeInput): EngineApproachesConfig {
    const p = partial || {};
    return {
        ...DEFAULT_ENGINE_APPROACHES,
        ...p,
        ramdisk: mergeRamdisk({
            ...DEFAULT_ENGINE_APPROACHES.ramdisk,
            ...(p.ramdisk || {}),
        }),
    };
}

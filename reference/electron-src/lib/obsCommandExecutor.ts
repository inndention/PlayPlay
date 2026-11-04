import { OBSCommand, PlaylistItem } from './types';
import { OBSWebSocketClient } from './obsWebSocket';

/**
 * Executes an OBS command using the provided client.
 * Handles variable substitution for filenames.
 */
export async function executeOBSCommand(
    command: OBSCommand,
    client: OBSWebSocketClient,
    context?: {
        item?: PlaylistItem;
        date?: Date;
    }
): Promise<void> {
    if (!client.isConnected) {
        console.warn(`[OBSCommand] Client not connected, skipping command: ${command.label}`);
        return;
    }

    console.log(`[OBSCommand] Executing: ${command.label} (${command.commandType})`);

    try {
        switch (command.commandType) {
            case 'change_scene':
                if (command.sceneName) {
                    await client.setCurrentScene(command.sceneName);
                }
                break;

            case 'change_scene_collection':
                if (command.sceneCollection) {
                    await client.setSceneCollection(command.sceneCollection);
                }
                break;

            case 'show_source':
            case 'hide_source':
            case 'set_source_visibility':
                if (command.sourceName && command.sceneName) {
                    const visible = command.commandType === 'show_source' ? true :
                        command.commandType === 'hide_source' ? false :
                            !!command.visible;
                    await client.setSceneItemEnabled(command.sceneName, command.sourceName, visible);
                }
                break;

            case 'toggle_source':
                if (command.sourceName && command.sceneName) {
                    // Get current state via getSceneItemList
                    const sceneItems = await client.getSceneItemList(command.sceneName);
                    const item = sceneItems.sceneItems.find(i => i.sourceName === command.sourceName);
                    if (item) {
                        // Toggle the visibility
                        await client.setSceneItemEnabled(command.sceneName, command.sourceName, !item.sceneItemEnabled);
                    }
                }
                break;

            case 'start_record':
                if (command.recordDirectory || command.recordFileName) {
                    await client.startRecordWithFilename(command.recordFileName, command.recordDirectory);
                } else {
                    await client.startRecord();
                }
                break;

            case 'stop_record':
                await client.stopRecord();
                break;

            case 'start_stream':
                await client.startStream();
                break;

            case 'stop_stream':
                await client.stopStream();
                break;

            case 'start_udp_source':
                // Show the source
                if (command.sourceName && command.sceneName) {
                    await client.setSceneItemEnabled(command.sceneName, command.sourceName, true);
                }
                break;

            case 'custom':
                if (command.requestType) {
                    const data = command.requestData ? JSON.parse(command.requestData) : {};
                    await client.call(command.requestType, data);
                }
                break;
        }
    } catch (e) {
        console.error(`[OBSCommand] Failed to execute ${command.label}:`, e);
        throw e;
    }
}

/**
 * Resolves the OBS WebSocket client for a saved command (or routing-only `null`).
 * When `command` is null, implement the same default as "command without sessionId"
 * (e.g. active channel output mapping, then first connected).
 */
export type OBSClientResolver = (command: OBSCommand | null) => OBSWebSocketClient | undefined;

/**
 * Executes all start or end actions for a playlist item.
 * Note: PlaylistItem.obsCommands are executed at start (type='start').
 * End commands can be added in future by extending PlaylistItem.
 */
export async function executeOBSCommandsForItem(
    item: PlaylistItem,
    type: 'start' | 'end',
    resolveClient: OBSClientResolver
): Promise<void> {
    // Only execute commands at start for now (obsCommands field)
    // End commands support can be added by extending PlaylistItem interface
    // Simple support for end commands if the item has obsEndCommands array
    if (type === 'end' && !(item as any).obsEndCommands) return;

    const commands = type === 'start' ? item.obsCommands : (item as any).obsEndCommands;
    if (!commands || commands.length === 0) return;

    console.log(`[OBSCommand] Executing ${type} commands for item: ${item.id}`);

    // obsCommands contains command IDs, need to resolve them
    const { loadOBSCommands } = await import('./storage');
    const allCommands = loadOBSCommands();
    const commandMap = new Map(allCommands.map(c => [c.id, c]));

    for (const cmdId of commands) {
        const command = commandMap.get(cmdId);
        if (!command) {
            console.warn(`[OBSCommand] Command not found: ${cmdId}`);
            continue;
        }
        const client = resolveClient(command);
        if (!client) {
            const sid = command.sessionId ?? (command as { obsSessionId?: string }).obsSessionId;
            const hint = sid
                ? `session "${sid}" not connected`
                : 'no connected OBS client';
            console.warn(`[OBSCommand] Skip "${command.label}" (${hint})`);
            continue;
        }
        await executeOBSCommand(command, client, { item });
    }
}

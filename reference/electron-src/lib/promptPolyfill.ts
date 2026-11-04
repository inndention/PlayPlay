/**
 * Async prompt for environments where window.prompt() is not supported (e.g. Electron).
 * Use safePrompt() instead of prompt() and await the result.
 */

export type PromptState = {
    message: string;
    defaultValue: string;
};

let resolveCurrent: ((value: string | null) => void) | null = null;
let setStateRef: ((state: PromptState | null) => void) | null = null;

export function registerPromptProvider(setState: (state: PromptState | null) => void): void {
    setStateRef = setState;
}

export function promptAsync(message: string, defaultValue: string = ''): Promise<string | null> {
    return new Promise((resolve) => {
        resolveCurrent = resolve;
        setStateRef?.({ message, defaultValue });
    });
}

/** Called by the prompt modal when user submits or cancels. */
export function resolvePrompt(value: string | null): void {
    if (resolveCurrent) {
        resolveCurrent(value);
        resolveCurrent = null;
    }
    setStateRef?.(null);
}

/**
 * Use this instead of window.prompt() so it works in Electron (where prompt is disabled).
 * Returns the entered string or null if cancelled. Always async.
 */
export function safePrompt(message: string, defaultValue: string = ''): Promise<string | null> {
    if (typeof window !== 'undefined' && typeof (window as any).prompt === 'function') {
        try {
            const result = (window as any).prompt(message, defaultValue);
            return Promise.resolve(result);
        } catch {
            return promptAsync(message, defaultValue);
        }
    }
    return promptAsync(message, defaultValue);
}

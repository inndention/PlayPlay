import { logError } from './errorBus';

export function safeHandler<T extends (...args: unknown[]) => unknown>(
    fn: T,
    componentName: string = 'Unknown'
): T {
    return ((...args: unknown[]) => {
        try {
            const result = fn(...args);
            if (result instanceof Promise) {
                return result.catch((err: unknown) => {
                    const msg = err instanceof Error ? err.message : String(err);
                    console.error(`[${componentName}] Handler error:`, err);
                    logError(`${componentName}: ${msg}`, err);
                });
            }
            return result;
        } catch (err) {
            const msg = err instanceof Error ? err.message : String(err);
            console.error(`[${componentName}] Handler error:`, err);
            logError(`${componentName}: ${msg}`, err);
        }
    }) as T;
}

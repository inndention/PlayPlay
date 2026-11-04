import { useEffect, useRef, useCallback } from 'react';

export function useSafeAsync() {
    const isMountedRef = useRef(true);

    useEffect(() => {
        isMountedRef.current = true;
        return () => {
            isMountedRef.current = false;
        };
    }, []);

    const safeSetState = useCallback((fn: () => void) => {
        if (isMountedRef.current) {
            fn();
        }
    }, []);

    return { isMounted: () => isMountedRef.current, safeSetState };
}

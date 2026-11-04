import React, { Component, ErrorInfo, ReactNode } from 'react';
import ReactDOM from 'react-dom/client';
import { HashRouter } from 'react-router-dom';
import App from './App';
import './globals.css';
import { logError, logInfo } from './lib/errorBus';
import { PromptProvider } from './components/PromptProvider';

// Top-level error boundary to catch any rendering errors
class AppErrorBoundary extends Component<{ children: ReactNode }, { hasError: boolean; error: Error | null; retryCount: number }> {
    constructor(props: { children: ReactNode }) {
        super(props);
        this.state = { hasError: false, error: null, retryCount: 0 };
    }

    static getDerivedStateFromError(error: Error) {
        console.warn('[App] Error caught by boundary:', error.message);
        return { hasError: true, error };
    }

    componentDidCatch(error: Error, errorInfo: ErrorInfo) {
        console.error('[App] Error details:', error, errorInfo);
        const win = window as unknown as { electronAPI?: { logError: (data: unknown) => void } };
        if (win.electronAPI?.logError) {
            win.electronAPI.logError({
                type: 'react',
                error: error.message,
                stack: error.stack,
                componentStack: errorInfo.componentStack,
            });
        }
    }

    handleRetry = () => {
        this.setState((prev) => ({ hasError: false, error: null, retryCount: prev.retryCount + 1 }));
    };

    render() {
        if (this.state.hasError) {
            return (
                <div style={{
                    position: 'fixed',
                    top: 0,
                    left: 0,
                    right: 0,
                    padding: '10px',
                    background: '#ef4444',
                    color: 'white',
                    zIndex: 9999,
                    fontSize: '12px',
                    display: 'flex',
                    justifyContent: 'space-between',
                    alignItems: 'center'
                }}>
                    <span>
                        <strong>Application Error:</strong> {this.state.error?.message || 'Unknown error'}
                    </span>
                    <button
                        onClick={this.handleRetry}
                        style={{ background: 'white', color: '#ef4444', border: 'none', padding: '2px 8px', borderRadius: '4px', cursor: 'pointer' }}
                    >
                        Retry
                    </button>
                </div>
            );
        }
        return <React.Fragment key={this.state.retryCount}>{this.props.children}</React.Fragment>;
    }
}

const rootElement = document.getElementById('root');
if (!rootElement) {
    throw new Error('Root element not found');
}

// Global error listeners: log to Error Console and optionally to Electron
window.addEventListener('error', (event) => {
    const msg = event.message || '';
    // Avoid cluttering Error Console with browser media errors (we handle these in PlayerEngine/OutputPage)
    if (msg.includes('no supported sources') || msg.includes('MEDIA_ERR_SRC_NOT_SUPPORTED')) {
        logInfo('Media source could not be loaded', { message: msg });
        return;
    }
    console.error('[GlobalError]', event.message, event.filename, event.lineno);
    logError(event.message, { filename: event.filename, lineno: event.lineno });
    const win = window as unknown as { electronAPI?: { logError: (data: unknown) => void } };
    if (win.electronAPI?.logError) {
        win.electronAPI.logError({ type: 'window.error', message: event.message, filename: event.filename, lineno: event.lineno });
    }
});

window.addEventListener('unhandledrejection', (event) => {
    const reason = event.reason;
    const message = reason instanceof Error ? reason.message : String(reason);
    console.error('[GlobalRejection]', reason);
    logError(message, { reason });
    const win = window as unknown as { electronAPI?: { logError: (data: unknown) => void } };
    if (win.electronAPI?.logError) {
        win.electronAPI.logError({ type: 'unhandledrejection', message, reason: String(reason) });
    }
    event.preventDefault?.(); // Mark as handled so devtools don't treat as unhandled
});

const root = ReactDOM.createRoot(rootElement);

root.render(
    <AppErrorBoundary>
        <HashRouter>
            <PromptProvider>
                <App />
            </PromptProvider>
        </HashRouter>
    </AppErrorBoundary>
);

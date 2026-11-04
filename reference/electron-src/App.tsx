import React from 'react';
import { Routes, Route, useLocation } from 'react-router-dom';
import { ErrorBoundary } from './components/ErrorBoundary';
import { PlayoutController } from './components/PlayoutController';
import { OutputPage } from './pages/OutputPage';
import { AsPlayedReportPage } from './pages/AsPlayedReportPage';
import { RemoteSessionsPage } from './pages/RemoteSessionsPage';
import { CGBrowserOutputPage } from './pages/CGBrowserOutputPage';
import { CGTemplatesPage } from './pages/CGTemplatesPage';
import { MultiviewerPage } from './pages/MultiviewerPage';
import { ToastProvider } from './components/Toast';
import { ErrorConsole } from './components/ErrorConsole';

function CorePlayoutController() {
    return <PlayoutController coreOnly />;
}

function withErrorBoundary(Component: React.ComponentType, componentName: string) {
    return (
        <ErrorBoundary componentName={componentName}>
            <Component />
        </ErrorBoundary>
    );
}

function App() {
    const location = useLocation();
    
    const currentPath = React.useMemo(() => {
        const path = location.pathname || location.hash.replace('#', '');
        return path;
    }, [location.pathname, location.hash]);

    // If we're on the output route, render it separately to keep it lightweight for OBS
    if (
        currentPath === '/output'
        || currentPath === '#/output'
        || currentPath.startsWith('/output?')
        || currentPath.startsWith('/output/')
    ) {
        return withErrorBoundary(OutputPage, 'OutputPage');
    }
    if (currentPath === '/multiviewer' || currentPath === '#/multiviewer') {
        return withErrorBoundary(MultiviewerPage, 'MultiviewerPage');
    }
    if (currentPath === '/cg-preview' || currentPath === '#/cg-preview') {
        return (
            <ErrorBoundary componentName="CGPreviewOutputPage">
                <CGBrowserOutputPage channel="preview" />
            </ErrorBoundary>
        );
    }
    if (
        currentPath === '/cg-program' ||
        currentPath === '#/cg-program' ||
        currentPath === '/cg-output' ||
        currentPath === '#/cg-output'
    ) {
        return (
            <ErrorBoundary componentName="CGProgramOutputPage">
                <CGBrowserOutputPage channel="program" />
            </ErrorBoundary>
        );
    }
    return (
        <ToastProvider>
            <Routes>
                <Route path="/" element={withErrorBoundary(CorePlayoutController, 'PlayoutController')} />
                <Route path="/playout" element={withErrorBoundary(CorePlayoutController, 'PlayoutController')} />
                <Route path="/output" element={withErrorBoundary(OutputPage, 'OutputPage')} />
                <Route
                    path="/cg-preview"
                    element={(
                        <ErrorBoundary componentName="CGPreviewOutputPage">
                            <CGBrowserOutputPage channel="preview" />
                        </ErrorBoundary>
                    )}
                />
                <Route
                    path="/cg-program"
                    element={(
                        <ErrorBoundary componentName="CGProgramOutputPage">
                            <CGBrowserOutputPage channel="program" />
                        </ErrorBoundary>
                    )}
                />
                <Route
                    path="/cg-output"
                    element={(
                        <ErrorBoundary componentName="CGOutputAliasPage">
                            <CGBrowserOutputPage channel="program" />
                        </ErrorBoundary>
                    )}
                />
                <Route path="/reports" element={withErrorBoundary(AsPlayedReportPage, 'AsPlayedReportPage')} />
                <Route path="/remote" element={withErrorBoundary(RemoteSessionsPage, 'RemoteSessionsPage')} />
                <Route path="/cg-templates" element={withErrorBoundary(CGTemplatesPage, 'CGTemplatesPage')} />
                <Route path="/multiviewer" element={withErrorBoundary(MultiviewerPage, 'MultiviewerPage')} />
            </Routes>
            <ErrorConsole />
        </ToastProvider>
    );
}

export default App;

import React, { Component, ReactNode, ErrorInfo } from 'react';

interface Props {
    children: ReactNode;
    fallback?: ReactNode;
    componentName?: string;
}

interface State {
    hasError: boolean;
    error: Error | null;
    retryCount: number;
}

export class ErrorBoundary extends Component<Props, State> {
    constructor(props: Props) {
        super(props);
        this.state = { hasError: false, error: null, retryCount: 0 };
    }

    static getDerivedStateFromError(error: Error): Partial<State> {
        return { hasError: true, error };
    }

    componentDidCatch(error: Error, errorInfo: ErrorInfo) {
        console.error(`[ErrorBoundary ${this.props.componentName || 'Unknown'}]`, error, errorInfo);
    }

    handleRetry = () => {
        this.setState((prev) => ({ hasError: false, error: null, retryCount: prev.retryCount + 1 }));
    };

    render() {
        if (this.state.hasError) {
            return this.props.fallback ?? (
                <div style={{ padding: '1rem', color: 'var(--danger, #ef4444)' }}>
                    <h3>Component Error</h3>
                    <p>{this.state.error?.message}</p>
                    <button
                        type="button"
                        onClick={this.handleRetry}
                        style={{
                            marginTop: '0.75rem',
                            padding: '0.5rem 1rem',
                            background: 'var(--danger, #ef4444)',
                            color: 'white',
                            border: 'none',
                            borderRadius: '4px',
                            cursor: 'pointer',
                        }}
                    >
                        Retry
                    </button>
                </div>
            );
        }
        return <React.Fragment key={this.state.retryCount}>{this.props.children}</React.Fragment>;
    }
}
